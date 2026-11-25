/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file VulkanPresentation.cpp
 *
 * Surfaces, the swapchain and the frame lifecycle.
 *
 * ## No acquire or render-finished semaphores
 *
 * The obvious Vulkan frame uses `vkAcquireNextImageKHR` with a semaphore, has the render submission
 * wait on it, signals another, and presents waiting on the second. This backend instead acquires with
 * a fence and waits for it, because `executeCommandBuffer` already waits for the frame's submission
 * before it returns. Once the CPU knows the GPU is idle, the acquire is complete and the rendering is
 * complete, so a present with no semaphores is correctly ordered - and there is one fewer class of bug
 * (a semaphore used twice, or a submit that does not wait) that cannot arise.
 *
 * The cost is a frame of parallelism: the CPU never runs ahead of the GPU. A pipelined version is the
 * obvious next step and is recorded as future work in `GPU后端支持Vulkan进度与问题记录.md`; the RHI's
 * `maxFramesInFlight` limit reports 1 so an upper layer does not assume otherwise.
 *
 * ## The one layout transition
 *
 * Rendering keeps the swapchain image in `GENERAL`. Presentation requires `PRESENT_SRC_KHR`, so
 * `endFrame` records that transition in a one-shot command buffer and waits for it before presenting.
 * The next `beginRenderPass` on the window target transitions back.
 */

#include "private/backend/vulkan/VulkanDevice.h"

#include <backend/TestHooks.h>

#include "private/backend/vulkan/VulkanCommandBuffer.h"
#include "private/backend/vulkan/VulkanEnums.h"
#include "private/backend/vulkan/VulkanEntryPoints.h"
#include "private/backend/vulkan/VulkanPlatform.h"

#include <algorithm>
#include <string>

namespace backend::vulkan {
namespace {

VkPresentModeKHR toVkPresentMode(const PresentMode mode) noexcept {
  switch (mode) {
    case PresentMode::Immediate:
      return VK_PRESENT_MODE_IMMEDIATE_KHR;
    case PresentMode::Fifo:
      return VK_PRESENT_MODE_FIFO_KHR;
    case PresentMode::Mailbox:
      return VK_PRESENT_MODE_MAILBOX_KHR;
  }
  return VK_PRESENT_MODE_FIFO_KHR;
}

PresentMode fromVkPresentMode(const VkPresentModeKHR mode) noexcept {
  switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
      return PresentMode::Immediate;
    case VK_PRESENT_MODE_MAILBOX_KHR:
      return PresentMode::Mailbox;
    default:
      return PresentMode::Fifo;
  }
}

Result vkResultToResult(const VkResult result) noexcept {
  switch (result) {
    case VK_SUCCESS:
      return Result::Success;
    case VK_SUBOPTIMAL_KHR:
    case VK_ERROR_OUT_OF_DATE_KHR:
    case VK_ERROR_SURFACE_LOST_KHR:
      return Result::OutOfDate;
    case VK_TIMEOUT:
      return Result::Timeout;
    case VK_NOT_READY:
      return Result::NotReady;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
      return Result::OutOfMemory;
    case VK_ERROR_DEVICE_LOST:
      return Result::DeviceLost;
    default:
      return Result::Error;
  }
}

} // namespace anonymous

// ==================================================================================================
// Surfaces
// ==================================================================================================

Result VulkanDevice::createSurface(const SurfaceDescriptor& descriptor, SurfaceHandle* outSurface) {
  if (outSurface == nullptr) return Result::InvalidArgument;
  *outSurface = SurfaceHandle::invalid();

  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (const Result created = createPlatformSurface(*mApi, mInstance, descriptor, &surface); !isSuccess(created)) {
    if (created == Result::InvalidArgument) {
      emitMessage(MessageSeverity::Error, "createSurface was given no native window");
    }
    return created;
  }

  if (mApi->vkGetPhysicalDeviceSurfaceSupportKHR != nullptr) {
    VkBool32 supported = VK_FALSE;
    if (mApi->vkGetPhysicalDeviceSurfaceSupportKHR(mPhysicalDevice, mPresentQueueFamily, surface, &supported) !=
            VK_SUCCESS ||
        supported != VK_TRUE) {
      emitMessage(MessageSeverity::Error,
              "the selected present queue family cannot present to this window surface");
      mApi->vkDestroySurfaceKHR(mInstance, surface, nullptr);
      return Result::Unsupported;
    }
  }

  VulkanSurface entry;
  entry.descriptor = descriptor;
  entry.surface = surface;
  if (descriptor.label != nullptr) entry.label = descriptor.label;

  *outSurface = mSurfaces.allocate(std::move(entry));
  if (!outSurface->valid()) {
    mApi->vkDestroySurfaceKHR(mInstance, surface, nullptr);
    return Result::OutOfMemory;
  }
  return Result::Success;
}

void VulkanDevice::destroySurface(const SurfaceHandle surfaceHandle) {
  if (VulkanSurface* surface = mSurfaces.get(surfaceHandle); surface != nullptr) {
    if (surface->surface != VK_NULL_HANDLE) mApi->vkDestroySurfaceKHR(mInstance, surface->surface, nullptr);
    surface->surface = VK_NULL_HANDLE;
    mSurfaces.free(surfaceHandle);
  }
}

// ==================================================================================================
// Swapchain
// ==================================================================================================

Result VulkanDevice::createSwapchain(const SwapchainDescriptor& descriptor, SwapchainHandle* outOut) {
  if (outOut == nullptr) return Result::InvalidArgument;
  *outOut = SwapchainHandle::invalid();

  const VulkanSurface* surface = mSurfaces.get(descriptor.surface);
  if (surface == nullptr || surface->surface == VK_NULL_HANDLE) return Result::InvalidHandle;
  if (!mHasSwapchainExtension) {
    emitMessage(MessageSeverity::Error,
            "the device was created without VK_KHR_swapchain, so a window cannot be presented to");
    return Result::Unsupported;
  }

  VulkanSwapchain swapchain;
  swapchain.surface = surface->surface;
  swapchain.surfaceHandle = descriptor.surface;
  if (descriptor.label != nullptr) swapchain.label = descriptor.label;

  if (const Result created = createSwapchainObjects(descriptor, &swapchain); !isSuccess(created)) {
    return created;
  }

  *outOut = mSwapchains.allocate(std::move(swapchain));
  return outOut->valid() ? Result::Success : Result::OutOfMemory;
}

Result VulkanDevice::createSwapchainObjects(const SwapchainDescriptor& descriptor, VulkanSwapchain* swapchain) {
  VkSurfaceCapabilitiesKHR capabilities{};
  if (const VkResult result =
                  mApi->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(mPhysicalDevice, swapchain->surface, &capabilities);
      result != VK_SUCCESS) {
    return vkResultToResult(result);
  }

  uint32_t formatCount = 0;
  mApi->vkGetPhysicalDeviceSurfaceFormatsKHR(mPhysicalDevice, swapchain->surface, &formatCount, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(formatCount);
  if (formatCount != 0) {
    mApi->vkGetPhysicalDeviceSurfaceFormatsKHR(mPhysicalDevice, swapchain->surface, &formatCount, formats.data());
  }
  if (formats.empty()) {
    emitMessage(MessageSeverity::Error, "the window surface reports no supported image format");
    return Result::Unsupported;
  }

  // What the *display* offers, as opposed to what the device can do: a surface that lists
  // Display-P3 or HDR10 has a wide-gamut/HDR output, and the capability fields are what let an
  // application find that out without creating a swapchain of that kind first.
  mCapabilities.displayP3Swapchain = std::any_of(formats.begin(), formats.end(),
    [](const VkSurfaceFormatKHR& candidate) {
      return candidate.colorSpace == VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT;
    });
  mCapabilities.hdr10Swapchain = std::any_of(formats.begin(), formats.end(),
    [](const VkSurfaceFormatKHR& candidate) {
      return candidate.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT;
    });

  // The requested format is tried first because the canvas's pipelines are compiled for it and a
  // dynamic-rendering pipeline's attachment format has to match. The fallback list is the usual
  // Windows/Linux pair, and the very last resort is whatever the surface offers first.
  const VkFormat requested = toVkFormat(descriptor.format);
  VkSurfaceFormatKHR chosen = formats.front();
  bool found = false;
  // ## HDR10 first, and only when asked
  //
  // The canvas writes sRGB-encoded numbers; the HDR output stage converts them to PQ on the way into
  // this swapchain. A surface that lists neither half of the pair gets a warning and the SDR path,
  // because a window that refused to open would be a worse answer than one that presents SDR.
  const bool wantsHdr = descriptor.colorSpace == SwapchainColorSpace::Hdr10Pq;
  const bool wantsP3 = descriptor.colorSpace == SwapchainColorSpace::DisplayP3;
  const bool wantsEdr = descriptor.colorSpace == SwapchainColorSpace::ExtendedLinearP3;
  if (wantsP3) {
    // Wide-gamut SDR: an 8-bit image with the P3 non-linear colour space. The presentation pass
    // gamma-encodes after converting primaries, so the format stays 8-bit.
    for (const VkSurfaceFormatKHR& candidate: formats) {
      if (candidate.colorSpace == VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT &&
          (candidate.format == requested || candidate.format == VK_FORMAT_B8G8R8A8_UNORM ||
           candidate.format == VK_FORMAT_R8G8B8A8_UNORM)) {
        chosen = candidate;
        found = true;
        break;
      }
    }
    if (!found) {
      emitMessage(MessageSeverity::Warning,
              "Display-P3 output was requested but this surface offers no P3 pair; the swapchain falls back to SDR");
    }
  }
  if (wantsEdr) {
    // Vulkan has no extended-linear P3 colour space; the closest is linear P3 on a float image. A
    // surface that offers neither that pair nor a float format falls back to SDR rather than pretending.
    for (const VkSurfaceFormatKHR& candidate: formats) {
      if (candidate.colorSpace == VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT &&
          candidate.format == VK_FORMAT_R16G16B16A16_SFLOAT) {
        chosen = candidate;
        found = true;
        break;
      }
    }
    if (!found) {
      emitMessage(MessageSeverity::Warning,
              "extended-linear P3 was requested but this Vulkan surface offers no linear-P3 float pair; "
              "the swapchain falls back to SDR");
    }
  }
  if (wantsHdr) {
    for (const VkSurfaceFormatKHR& candidate: formats) {
      if (candidate.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
          candidate.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
        chosen = candidate;
        found = true;
        break;
      }
    }
    if (!found) {
      std::string offered = "HDR output was requested but no HDR10 pair was offered; formats:";
      for (const VkSurfaceFormatKHR& candidate: formats) {
        offered += " (" + std::to_string(static_cast<int>(candidate.format)) + ", cs " +
          std::to_string(static_cast<int>(candidate.colorSpace)) + ")";
      }
      emitMessage(MessageSeverity::Warning, offered);
    }
  }
  for (const VkSurfaceFormatKHR& candidate: formats) {
    // The HDR selection above may already have chosen the pair; this loop is the SDR preference and
    // must not overwrite that choice (it did, which is how an HDR request ended up presenting sRGB).
    if (found) break;
    if (candidate.format == requested && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      chosen = candidate;
      found = true;
      break;
    }
  }
  if (!found) {
    for (const VkFormat preferred: { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM }) {
      for (const VkSurfaceFormatKHR& candidate: formats) {
        if (candidate.format == preferred && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
          chosen = candidate;
          found = true;
          break;
        }
      }
      if (found) break;
    }
  }
  if (!found) {
    std::string message = "the window surface does not offer the requested swapchain format; using the first "
                          "format it reports";
    emitMessage(MessageSeverity::Warning, message);
  }

  uint32_t presentModeCount = 0;
  mApi->vkGetPhysicalDeviceSurfacePresentModesKHR(mPhysicalDevice, swapchain->surface, &presentModeCount, nullptr);
  std::vector<VkPresentModeKHR> presentModes(presentModeCount);
  if (presentModeCount != 0) {
    mApi->vkGetPhysicalDeviceSurfacePresentModesKHR(mPhysicalDevice, swapchain->surface, &presentModeCount,
            presentModes.data());
  }
  const bool hasMailbox = std::find(presentModes.begin(), presentModes.end(), VK_PRESENT_MODE_MAILBOX_KHR) !=
                          presentModes.end();
  const bool hasImmediate = std::find(presentModes.begin(), presentModes.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) !=
                            presentModes.end();
  mCapabilities.mailboxPresent = hasMailbox;

  // ## What is granted, not what was asked for
  //
  // Fifo is the only mode Vulkan guarantees. A Mailbox request the surface cannot honour is approximated
  // by Immediate, which is the same "do not wait" semantic described in `PresentMode`; reporting Fifo
  // there would claim a vertical-sync wait that is not happening.
  VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
  switch (descriptor.presentMode) {
    case PresentMode::Fifo:
      presentMode = VK_PRESENT_MODE_FIFO_KHR;
      break;
    case PresentMode::Immediate:
      presentMode = hasImmediate ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
      break;
    case PresentMode::Mailbox:
      presentMode = hasMailbox ? VK_PRESENT_MODE_MAILBOX_KHR
                               : hasImmediate ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
      break;
  }

  VkExtent2D extent = capabilities.currentExtent;
  if (descriptor.width != 0 && descriptor.height != 0) {
    // ## The caller's size wins
    //
    // A Win32 surface reports the window's *client* area as `currentExtent`, and Windows enforces a
    // minimum track size on an overlapped window: a "64x64" hidden test window is really 120x25, so
    // honouring `currentExtent` would silently render at a size the caller never asked for. Vulkan
    // permits a swapchain whose extent differs from `currentExtent` (the presentation engine scales),
    // so the descriptor is authoritative here; the surface bounds only narrow it.
    extent.width = descriptor.width;
    extent.height = descriptor.height;
    if (capabilities.maxImageExtent.width > 0 && capabilities.maxImageExtent.height > 0) {
      extent.width = std::min(extent.width, capabilities.maxImageExtent.width);
      extent.height = std::min(extent.height, capabilities.maxImageExtent.height);
    }
    if (capabilities.minImageExtent.width > 0 && capabilities.minImageExtent.height > 0) {
      extent.width = std::max(extent.width, capabilities.minImageExtent.width);
      extent.height = std::max(extent.height, capabilities.minImageExtent.height);
    }
  }
  if (extent.width == std::numeric_limits<uint32_t>::max() || extent.width == 0 || extent.height == 0) {
    emitMessage(MessageSeverity::Error,
            "the swapchain has no size: the window reports a zero extent and the descriptor names none");
    return Result::InvalidArgument;
  }

  uint32_t imageCount = capabilities.minImageCount + 1;
  if (capabilities.maxImageCount > 0) imageCount = std::min(imageCount, capabilities.maxImageCount);
  if (descriptor.imageCount > 0) {
    imageCount = std::clamp(descriptor.imageCount, capabilities.minImageCount > 0 ? capabilities.minImageCount : 1u,
            capabilities.maxImageCount > 0 ? capabilities.maxImageCount : descriptor.imageCount);
  }

  VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  if ((capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) == 0) {
    for (const VkCompositeAlphaFlagBitsKHR candidate: { VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR }) {
      if ((capabilities.supportedCompositeAlpha & candidate) != 0) {
        compositeAlpha = candidate;
        break;
      }
    }
  }

  // The canvas renders sRGB-encoded values into a UNORM attachment; the swapchain's
  // `SRGB_NONLINEAR` colour space is the display's interpretation of those bytes. An `_SRGB`
  // attachment would apply a second linear->sRGB conversion, and an HDR colour space needs a real
  // tone-mapping/encoding stage. Neither is silently approximated here: only an SDR UNORM surface is
  // accepted, and the actual format is reported through `SwapchainInfo::format`.
  PixelFormat chosenPixelFormat = PixelFormat::Undefined;
  for (uint32_t raw = 1; raw <= static_cast<uint32_t>(PixelFormat::ASTC_8x8_SRGB); ++raw) {
    const auto candidateFormat = static_cast<PixelFormat>(raw);
    if (toVkFormat(candidateFormat) == chosen.format) {
      chosenPixelFormat = candidateFormat;
      break;
    }
  }
  // The SDR path needs a non-sRGB UNORM attachment; the HDR path needs its own 10-bit pair, which was
  // selected above. Anything else is a surface this renderer cannot feed and is refused rather than
  // rendered into with the wrong numbers.
  const bool hdr10 = chosen.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
                     chosen.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  if (!hdr10 && (chosen.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR || chosenPixelFormat == PixelFormat::Undefined ||
      getPixelFormatInfo(chosenPixelFormat).srgb)) {
    emitMessage(MessageSeverity::Error,
            "the window surface only offers an sRGB/HDR swapchain format; the canvas currently needs an "
            "SDR UNORM swapchain (tone mapping/encoding is a separate output stage)");
    return Result::Unsupported;
  }

  VkSwapchainCreateInfoKHR info{};
  info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  info.surface = swapchain->surface;
  info.minImageCount = imageCount;
  info.imageFormat = chosen.format;
  info.imageColorSpace = chosen.colorSpace;
  info.imageExtent = extent;
  info.imageArrayLayers = 1;
  info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  const uint32_t queueFamilies[2] = { mGraphicsQueueFamily, mPresentQueueFamily };
  if (mPresentQueueFamily != mGraphicsQueueFamily) {
    // The images are written on the graphics queue and presented on another family, so the swapchain
    // needs concurrent sharing; exclusive sharing would require an ownership transfer per image.
    info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    info.queueFamilyIndexCount = 2;
    info.pQueueFamilyIndices = queueFamilies;
  } else {
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  }
  info.preTransform = capabilities.currentTransform;
  info.compositeAlpha = compositeAlpha;
  info.presentMode = presentMode;
  // `clipped` lets the presentation engine discard the parts of the image the window does not show,
  // which is free performance on every platform that supports it (all of them in practice).
  info.clipped = VK_TRUE;

  // ## One live swapchain per surface
  //
  // Unlike a GL context, a Vulkan swapchain owns the window: creating a second one for the same
  // `VkSurfaceKHR` without retiring the first fails with `VK_ERROR_NATIVE_WINDOW_IN_USE_KHR`. If this
  // device already has one for the surface, it is named as the old swapchain, which retires it; the
  // older `VulkanSwapchain` record stays valid (and destroyable) but can no longer present.
  VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE;
  mSwapchains.forEach([&](SwapchainHandle, VulkanSwapchain& candidate) {
    if (&candidate == swapchain || oldSwapchain != VK_NULL_HANDLE) return;
    if (candidate.surface == swapchain->surface && candidate.swapchain != VK_NULL_HANDLE) {
      oldSwapchain = candidate.swapchain;
    }
  });
  if (oldSwapchain != VK_NULL_HANDLE) {
    // Every use of the retired swapchain's images has to be finished before it is handed over.
    mApi->vkDeviceWaitIdle(mDevice);
  }
  info.oldSwapchain = oldSwapchain;

  VkSwapchainKHR created = VK_NULL_HANDLE;
  if (const VkResult result = mApi->vkCreateSwapchainKHR(mDevice, &info, nullptr, &created); result != VK_SUCCESS) {
    std::string message = "vkCreateSwapchainKHR failed; the window cannot be presented to (VkResult ";
    message += std::to_string(static_cast<int>(result));
    message += ')';
    emitMessage(MessageSeverity::Error, message);
    return vkResultToResult(result);
  }

  uint32_t actualCount = 0;
  mApi->vkGetSwapchainImagesKHR(mDevice, created, &actualCount, nullptr);
  std::vector<VkImage> images(actualCount);
  if (actualCount != 0) {
    mApi->vkGetSwapchainImagesKHR(mDevice, created, &actualCount, images.data());
  }
  if (images.empty()) {
    mApi->vkDestroySwapchainKHR(mDevice, created, nullptr);
    return Result::Error;
  }

  std::vector<VkImageView> views(images.size(), VK_NULL_HANDLE);
  for (size_t index = 0; index < images.size(); ++index) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = images[index];
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = chosen.format;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (const VkResult result = mApi->vkCreateImageView(mDevice, &viewInfo, nullptr, &views[index]);
        result != VK_SUCCESS) {
      for (VkImageView view: views) {
        if (view != VK_NULL_HANDLE) mApi->vkDestroyImageView(mDevice, view, nullptr);
      }
      mApi->vkDestroySwapchainKHR(mDevice, created, nullptr);
      return vkResultToResult(result);
    }
  }

  swapchain->swapchain = created;
  swapchain->width = extent.width;
  swapchain->height = extent.height;
  swapchain->format = chosenPixelFormat;
  swapchain->vkFormat = chosen.format;
  swapchain->srgb = getPixelFormatInfo(swapchain->format).srgb;
  swapchain->presentMode = fromVkPresentMode(presentMode);
  swapchain->colorSpace =
    chosen.colorSpace == VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT ? SwapchainColorSpace::DisplayP3
    : chosen.colorSpace == VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT  ? SwapchainColorSpace::ExtendedLinearP3
    : chosen.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT       ? SwapchainColorSpace::Hdr10Pq
                                                                 : SwapchainColorSpace::Srgb;
  swapchain->imageCount = static_cast<uint32_t>(images.size());
  swapchain->images = std::move(images);
  swapchain->views = std::move(views);
  swapchain->layouts.assign(swapchain->imageCount, VK_IMAGE_LAYOUT_UNDEFINED);
  swapchain->currentImage = 0;
  swapchain->frameAcquired = false;
  swapchain->outOfDate = false;

  return Result::Success;
}

void VulkanDevice::destroySwapchainObjects(VulkanSwapchain* swapchain) {
  if (swapchain == nullptr) return;
  for (VkImageView view: swapchain->views) {
    if (view != VK_NULL_HANDLE) {
      // A fallback framebuffer names the swapchain image view; it must be released before the view.
      releaseFramebuffersUsing(view);
      mApi->vkDestroyImageView(mDevice, view, nullptr);
    }
  }
  swapchain->views.clear();
  swapchain->images.clear();
  swapchain->layouts.clear();
  if (swapchain->swapchain != VK_NULL_HANDLE) {
    mApi->vkDestroySwapchainKHR(mDevice, swapchain->swapchain, nullptr);
    swapchain->swapchain = VK_NULL_HANDLE;
  }
  swapchain->imageCount = 0;
  swapchain->frameAcquired = false;
}

void VulkanDevice::destroySwapchain(const SwapchainHandle swapchainHandle) {
  if (VulkanSwapchain* swapchain = mSwapchains.get(swapchainHandle); swapchain != nullptr) {
    destroySwapchainObjects(swapchain);
    swapchain->surface = VK_NULL_HANDLE;
    mSwapchains.free(swapchainHandle);
  }
}

Result VulkanDevice::getSwapchainInfo(const SwapchainHandle swapchainHandle, SwapchainInfo* outInfo) {
  if (outInfo == nullptr) return Result::InvalidArgument;
  const VulkanSwapchain* swapchain = mSwapchains.get(swapchainHandle);
  if (swapchain == nullptr) return Result::InvalidHandle;

  outInfo->width = swapchain->width;
  outInfo->height = swapchain->height;
  outInfo->format = swapchain->format;
  outInfo->imageCount = swapchain->imageCount;
  outInfo->presentMode = swapchain->presentMode;
  outInfo->currentImageIndex = swapchain->currentImage;
  outInfo->srgb = swapchain->srgb;
  outInfo->colorSpace = swapchain->colorSpace;
  return Result::Success;
}

Result VulkanDevice::beginFrame(const SwapchainHandle swapchainHandle, SwapchainInfo* outInfo,
        const SemaphoreHandle signalSemaphore) {
  return acquireFrame(swapchainHandle, outInfo, signalSemaphore, UINT64_MAX);
}

Result VulkanDevice::tryBeginFrame(const SwapchainHandle swapchainHandle, SwapchainInfo* outInfo) {
  // Zero timeout: vkAcquireNextImageKHR answers VK_NOT_READY instead of waiting for the display, which is
  // the difference between "this frame costs a millisecond" and "this frame waits for a refresh".
  return acquireFrame(swapchainHandle, outInfo, SemaphoreHandle::invalid(), 0);
}

Result VulkanDevice::acquireFrame(const SwapchainHandle swapchainHandle, SwapchainInfo* outInfo,
        const SemaphoreHandle signalSemaphore, const uint64_t timeoutNanoseconds) {
  if (backend::test::consumeDeviceLossInjection()) return Result::DeviceLost;
  VulkanSwapchain* swapchain = mSwapchains.get(swapchainHandle);
  if (swapchain == nullptr || swapchain->swapchain == VK_NULL_HANDLE) return Result::InvalidHandle;
  if (swapchain->frameAcquired) {
    emitMessage(MessageSeverity::Error, "beginFrame was called while the previous frame was still open");
    return Result::Error;
  }

  // A caller-supplied semaphore is the asynchronous path: the acquire signals it and the render
  // submission waits on it. Without one, keep the historical temporary-fence wait so existing callers
  // that do not know about semaphores still get a finished acquire.
  VkSemaphore acquireSemaphore = VK_NULL_HANDLE;
  if (signalSemaphore.valid()) {
    VulkanSemaphore* entry = semaphore(signalSemaphore);
    if (entry == nullptr || entry->semaphore == VK_NULL_HANDLE) return Result::InvalidHandle;
    acquireSemaphore = entry->semaphore;
  }

  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  if (acquireSemaphore == VK_NULL_HANDLE) {
    if (const VkResult created = mApi->vkCreateFence(mDevice, &fenceInfo, nullptr, &fence); created != VK_SUCCESS) {
      return vkResultToResult(created);
    }
  }

  uint32_t imageIndex = 0;
  const VkResult acquired = mApi->vkAcquireNextImageKHR(mDevice, swapchain->swapchain, timeoutNanoseconds,
          acquireSemaphore, fence, &imageIndex);
  if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
    if (fence != VK_NULL_HANDLE) mApi->vkDestroyFence(mDevice, fence, nullptr);
    // A timeout is not a broken surface: it is the caller asking not to wait. Reporting NotReady keeps it
    // out of the `outOfDate` path, which would otherwise rebuild a perfectly good swapchain.
    if (acquired == VK_NOT_READY || acquired == VK_TIMEOUT) {
      return Result::NotReady;
    }
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR || acquired == VK_SUBOPTIMAL_KHR ||
            acquired == VK_ERROR_SURFACE_LOST_KHR) {
      swapchain->outOfDate = true;
    }
    return vkResultToResult(acquired);
  }
  if (fence != VK_NULL_HANDLE) {
    mApi->vkWaitForFences(mDevice, 1, &fence, VK_TRUE, UINT64_MAX);
    mApi->vkDestroyFence(mDevice, fence, nullptr);
  }

  swapchain->currentImage = imageIndex;
  swapchain->frameAcquired = true;

  // ## The swapchain's size, not the surface's
  //
  // The RHI's frame contract is that the caller renders at the size the swapchain reports and the
  // presentation engine presents it. Reporting the window's current client area here would make the
  // facade reallocate its targets every frame for a hidden window whose client rect is not the size the
  // swapchain was created at. A genuine resize surfaces through `vkAcquireNextImageKHR` returning
  // `VK_SUBOPTIMAL_KHR`/`VK_ERROR_OUT_OF_DATE_KHR`, which the caller turns into `resize` explicitly.
  if (outInfo != nullptr) {
    outInfo->width = swapchain->width;
    outInfo->height = swapchain->height;
    outInfo->format = swapchain->format;
    outInfo->imageCount = swapchain->imageCount;
    outInfo->presentMode = swapchain->presentMode;
    outInfo->currentImageIndex = imageIndex;
    outInfo->srgb = swapchain->srgb;
  }

  // A caller-supplied semaphore was passed to `vkAcquireNextImageKHR`; the render submission is
  // responsible for waiting on it. With no semaphore the temporary fence above already waited.
  return Result::Success;
}

Result VulkanDevice::endFrame(const SwapchainHandle swapchainHandle) {
  return endFrame(swapchainHandle, SemaphoreHandle::invalid());
}

Result VulkanDevice::endFrame(const SwapchainHandle swapchainHandle, const SemaphoreHandle waitSemaphore) {
  VulkanSwapchain* swapchain = mSwapchains.get(swapchainHandle);
  if (swapchain == nullptr || swapchain->swapchain == VK_NULL_HANDLE) return Result::InvalidHandle;
  if (!swapchain->frameAcquired) {
    emitMessage(MessageSeverity::Error, "endFrame was called without a frame");
    return Result::Error;
  }

  VkSemaphore wait = VK_NULL_HANDLE;
  if (waitSemaphore.valid()) {
    VulkanSemaphore* entry = semaphore(waitSemaphore);
    if (entry == nullptr || entry->semaphore == VK_NULL_HANDLE) return Result::InvalidHandle;
    wait = entry->semaphore;
  }

  const uint32_t image = swapchain->currentImage;
  if (image < swapchain->layouts.size() && swapchain->layouts[image] != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
    const VkImage swapchainImage = swapchain->images[image];
    const VkImageLayout oldLayout = swapchain->layouts[image];
    const Result transitioned = oneShot("present-transition", [this, swapchainImage, oldLayout](
                                                                 VkCommandBuffer commandBuffer) {
      VkImageMemoryBarrier barrier{};
      barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
      barrier.dstAccessMask = 0;
      barrier.oldLayout = oldLayout;
      barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = swapchainImage;
      barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
      mApi->vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    });
    if (!isSuccess(transitioned)) {
      swapchain->frameAcquired = false;
      return transitioned;
    }
    swapchain->layouts[image] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  }

  VkPresentInfoKHR present{};
  present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present.swapchainCount = 1;
  present.pSwapchains = &swapchain->swapchain;
  present.pImageIndices = &swapchain->currentImage;
  if (wait != VK_NULL_HANDLE) {
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &wait;
  }

  VkResult presented = VK_SUCCESS;
  {
    std::lock_guard<std::mutex> lock(mQueueMutex);
    presented = mApi->vkQueuePresentKHR(mPresentQueue, &present);
  }
  swapchain->frameAcquired = false;

  if (presented == VK_SUCCESS) return Result::Success;
  if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR ||
          presented == VK_ERROR_SURFACE_LOST_KHR) {
    swapchain->outOfDate = true;
  }
  return vkResultToResult(presented);
}

Result VulkanDevice::resizeSwapchain(const SwapchainHandle swapchainHandle, const uint32_t width,
        const uint32_t height) {
  VulkanSwapchain* swapchain = mSwapchains.get(swapchainHandle);
  if (swapchain == nullptr) return Result::InvalidHandle;
  if (width == 0 || height == 0) return Result::InvalidArgument;
  if (width == swapchain->width && height == swapchain->height && !swapchain->outOfDate) {
    return Result::Success;
  }

  if (const Result idle = waitIdle(); !isSuccess(idle)) return idle;

  // The swapchain is rebuilt from the parameters it was created with, with the new extent. The window
  // itself is not touched: it belongs to the caller and has already been resized by the platform.
  SwapchainDescriptor descriptor;
  descriptor.surface = swapchain->surfaceHandle;
  descriptor.width = width;
  descriptor.height = height;
  descriptor.format = swapchain->format;
  descriptor.presentMode = swapchain->presentMode;
  descriptor.label = swapchain->label.empty() ? nullptr : swapchain->label.c_str();

  destroySwapchainObjects(swapchain);
  return createSwapchainObjects(descriptor, swapchain);
}

// ==================================================================================================
// The default framebuffer
// ==================================================================================================

bool VulkanDevice::currentSwapchainImage(VkImage* outImage, VkImageView* outView, Extent2D* outExtent,
        VkFormat* outFormat) {
  bool found = false;
  mSwapchains.forEach([&](SwapchainHandle, VulkanSwapchain& candidate) {
    if (found || !candidate.frameAcquired) return;
    if (candidate.currentImage >= candidate.images.size()) return;
    if (outImage != nullptr) *outImage = candidate.images[candidate.currentImage];
    if (outView != nullptr) *outView = candidate.views[candidate.currentImage];
    if (outExtent != nullptr) *outExtent = { candidate.width, candidate.height };
    if (outFormat != nullptr) *outFormat = candidate.vkFormat;
    found = true;
  });
  return found;
}

Result VulkanDevice::acquireSwapchainTarget(const SwapchainHandle swapchainHandle, SwapchainTargetHandle* outTarget,
        SwapchainTargetInfo* outInfo) {
  if (outTarget == nullptr || outInfo == nullptr) return Result::InvalidArgument;
  *outTarget = SwapchainTargetHandle::invalid();
  VulkanSwapchain* swapchain = mSwapchains.get(swapchainHandle);
  if (swapchain == nullptr || !swapchain->frameAcquired || swapchain->currentImage >= swapchain->images.size()) {
    return Result::NotReady;
  }
  backend::RenderPassDescriptor passDescriptor;
  passDescriptor.colorFormats[0] = swapchain->format;
  passDescriptor.colorAttachmentCount = 1;
  passDescriptor.sampleCount = 1;
  passDescriptor.label = "coral/direct-swapchain-target";
  RenderPassHandle pass = RenderPassHandle::invalid();
  if (!isSuccess(createRenderPass(passDescriptor, &pass))) return Result::Error;

  VulkanSwapchainTarget target;
  target.swapchain = swapchainHandle;
  target.image = swapchain->images[swapchain->currentImage];
  target.view = swapchain->views[swapchain->currentImage];
  target.width = swapchain->width;
  target.height = swapchain->height;
  target.vkFormat = swapchain->vkFormat;
  target.format = swapchain->format;
  target.pass = pass;
  *outTarget = mSwapchainTargets.allocate(std::move(target));
  if (!outTarget->valid()) {
    destroyRenderPass(pass);
    return Result::OutOfMemory;
  }
  outInfo->view = defaultFramebufferView();
  outInfo->width = swapchain->width;
  outInfo->height = swapchain->height;
  outInfo->format = swapchain->format;
  outInfo->renderPass = pass;
  outInfo->srgb = swapchain->srgb;
  return Result::Success;
}

Result VulkanDevice::presentSwapchainTarget(const SwapchainHandle swapchain, const SwapchainTargetHandle target) {
  VulkanSwapchainTarget* entry = mSwapchainTargets.get(target);
  if (entry == nullptr) return Result::InvalidHandle;
  const RenderPassHandle pass = entry->pass;
  mSwapchainTargets.free(target);
  if (pass.valid()) destroyRenderPass(pass);
  return endFrame(swapchain);
}
VkImageLayout VulkanDevice::currentSwapchainLayout() {
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  mSwapchains.forEach([&](SwapchainHandle, VulkanSwapchain& candidate) {
    if (!candidate.frameAcquired) return;
    if (candidate.currentImage < candidate.layouts.size()) layout = candidate.layouts[candidate.currentImage];
  });
  return layout;
}

void VulkanDevice::setCurrentSwapchainLayout(const VkImageLayout layout) {
  mSwapchains.forEach([layout](SwapchainHandle, VulkanSwapchain& candidate) {
    if (!candidate.frameAcquired) return;
    if (candidate.currentImage < candidate.layouts.size()) candidate.layouts[candidate.currentImage] = layout;
  });
}

bool VulkanDevice::resolveAttachmentView(const TextureViewHandle handle, VulkanTextureView* outView) {
  if (outView == nullptr) return false;

  if (handle == defaultFramebufferView()) {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    Extent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    if (!currentSwapchainImage(&image, &view, &extent, &format)) return false;

    *outView = VulkanTextureView{};
    outView->isDefaultFramebuffer = true;
    outView->image = image;
    outView->view = view;
    outView->extent = extent;
    outView->vkFormat = format;
    outView->viewType = VK_IMAGE_VIEW_TYPE_2D;
    outView->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    outView->mipLevelCount = 1;
    outView->arrayLayerCount = 1;
    return true;
  }

  const VulkanTextureView* view = mTextureViews.get(handle);
  if (view == nullptr || view->view == VK_NULL_HANDLE) return false;
  *outView = *view;
  return true;
}

// ==================================================================================================
// Queue
// ==================================================================================================

Result VulkanQueue::submit(CommandBuffer& commandBuffer, const FenceHandle fence, const SemaphoreHandle waitSemaphore,
        const SemaphoreHandle signalSemaphore) {
  auto* vulkanCommandBuffer = dynamic_cast<VulkanCommandBuffer*>(&commandBuffer);
  if (vulkanCommandBuffer == nullptr || !vulkanCommandBuffer->isReady()) return Result::Error;
  if (vulkanCommandBuffer->isInFlight()) return Result::Error;
  if (mDevice == nullptr) return Result::DeviceLost;

  VkFence vkFence = VK_NULL_HANDLE;
  if (fence.valid()) {
    VulkanFence* entry = mDevice->fence(fence);
    if (entry == nullptr || entry->fence == VK_NULL_HANDLE) return Result::InvalidHandle;
    vkFence = entry->fence;
  }

  VkSemaphore vkWait = VK_NULL_HANDLE;
  if (waitSemaphore.valid()) {
    VulkanSemaphore* entry = mDevice->semaphore(waitSemaphore);
    if (entry == nullptr || entry->semaphore == VK_NULL_HANDLE) return Result::InvalidHandle;
    vkWait = entry->semaphore;
  }
  VkSemaphore vkSignal = VK_NULL_HANDLE;
  if (signalSemaphore.valid()) {
    VulkanSemaphore* entry = mDevice->semaphore(signalSemaphore);
    if (entry == nullptr || entry->semaphore == VK_NULL_HANDLE) return Result::InvalidHandle;
    vkSignal = entry->semaphore;
  }

  // A caller that supplies a fence is asking for CPU/GPU overlap: submit, signal that fence, and let
  // the command buffer's next begin/reset wait for it. With no fence the historical synchronous
  // contract is kept, so `Queue::submit(buffer)` cannot surprise a caller that never polls.
  if (vkFence != VK_NULL_HANDLE) {
    if (const Result submitted = mDevice->submit(vulkanCommandBuffer->handle(), vkFence, vkWait, vkSignal);
        !isSuccess(submitted)) {
      return submitted;
    }
    vulkanCommandBuffer->markSubmitted(vkFence, true);
    mDevice->noteAsyncSubmission();
    return Result::Success;
  }

  if (const Result submitted = mDevice->submitAndWait(vulkanCommandBuffer->handle(), VK_NULL_HANDLE, vkWait,
              vkSignal);
      !isSuccess(submitted)) {
    return submitted;
  }
  vulkanCommandBuffer->markComplete();
  return Result::Success;
}

Result VulkanQueue::waitIdle() {
  if (mDevice == nullptr) return Result::DeviceLost;
  return mDevice->waitIdle();
}

} // namespace backend::vulkan
