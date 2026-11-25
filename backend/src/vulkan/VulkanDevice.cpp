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

#include "private/backend/vulkan/VulkanDevice.h"

#include "private/backend/vulkan/VulkanCommandBuffer.h"
#include "private/backend/vulkan/VulkanEnums.h"
#include "private/backend/vulkan/VulkanAndroidHardwareBuffer.h"
#include "private/backend/vulkan/VulkanEntryPoints.h"
#include "private/backend/vulkan/VulkanPlatform.h"

#include "GlslCompiler.h"
#include "ShaderCross.h"

#include <backend/Validation.h>

#include <minilog/log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <utility>

namespace backend::vulkan {
namespace {

/** Maps a Vulkan result onto the RHI's vocabulary. */
Result toResult(const VkResult result) noexcept {
  switch (result) {
    case VK_SUCCESS:
      return Result::Success;
    case VK_NOT_READY:
      return Result::NotReady;
    case VK_TIMEOUT:
      return Result::Timeout;
    case VK_SUBOPTIMAL_KHR:
    case VK_ERROR_OUT_OF_DATE_KHR:
    case VK_ERROR_SURFACE_LOST_KHR:
      return Result::OutOfDate;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
      return Result::OutOfMemory;
    case VK_ERROR_DEVICE_LOST:
      return Result::DeviceLost;
    case VK_ERROR_INITIALIZATION_FAILED:
    case VK_ERROR_EXTENSION_NOT_PRESENT:
    case VK_ERROR_FEATURE_NOT_PRESENT:
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
    case VK_ERROR_LAYER_NOT_PRESENT:
      return Result::Unsupported;
    default:
      return Result::Error;
  }
}

/** FNV-1a, matching the source hash used by the compiler path. */
uint64_t hashBytes(const void* data, const size_t size) noexcept {
  uint64_t hash = 1469598103934665603ull;
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= 1099511628211ull;
  }
  return hash;
}

/** @return true when @p name is in @p available. */
bool hasExtension(const std::vector<VkExtensionProperties>& available, const char* name) noexcept {
  for (const VkExtensionProperties& extension: available) {
    if (std::strcmp(extension.extensionName, name) == 0) return true;
  }
  return false;
}

std::string formatVersion(const uint32_t version) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%u.%u.%u", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
          VK_API_VERSION_PATCH(version));
  return buffer;
}

const char* vendorName(const uint32_t vendorID) noexcept {
  switch (vendorID) {
    case 0x10DE:
      return "NVIDIA";
    case 0x1002:
    case 0x1022:
      return "AMD";
    case 0x8086:
      return "Intel";
    case 0x13B5:
      return "Arm";
    case 0x5143:
      return "Qualcomm";
    case 0x1010:
      return "Imagination Technologies";
    case 0x106B:
      return "Apple";
    case 0x10005:
      return "Mesa";
    default:
      return "unknown vendor";
  }
}

const char* deviceTypeName(const VkPhysicalDeviceType type) noexcept {
  switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
      return "integrated GPU";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
      return "discrete GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
      return "virtual GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
      return "CPU";
    default:
      return "other";
  }
}

uint32_t scoreDevice(const VkPhysicalDeviceType type) noexcept {
  switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
      return 1000;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
      return 600;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
      return 400;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
      return 100;
    default:
      return 50;
  }
}

BindingType toBindingType(const ShaderBindingType type) noexcept {
  switch (type) {
    case ShaderBindingType::TextureSampler:
      return BindingType::TextureSampler;
    case ShaderBindingType::Sampler:
      return BindingType::Sampler;
    case ShaderBindingType::SampledTexture:
      return BindingType::Texture;
    case ShaderBindingType::UniformBuffer:
      return BindingType::UniformBuffer;
    case ShaderBindingType::StorageBuffer:
      return BindingType::StorageBuffer;
    case ShaderBindingType::ReadOnlyStorageBuffer:
      return BindingType::ReadOnlyStorageBuffer;
    case ShaderBindingType::PushConstant:
    case ShaderBindingType::StageInput:
    case ShaderBindingType::StageOutput:
      break;
  }
  return BindingType::TextureSampler;
}

bool isDescriptorBinding(const ShaderBindingType type) noexcept {
  return type == ShaderBindingType::TextureSampler || type == ShaderBindingType::Sampler ||
         type == ShaderBindingType::SampledTexture || type == ShaderBindingType::UniformBuffer ||
         type == ShaderBindingType::StorageBuffer || type == ShaderBindingType::ReadOnlyStorageBuffer;
}

VkBufferUsageFlags toVkBufferUsage(const BufferUsages usage) noexcept {
  VkBufferUsageFlags flags = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (hasUsage(usage, BufferUsage::Vertex)) flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  if (hasUsage(usage, BufferUsage::Index)) flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  if (hasUsage(usage, BufferUsage::Uniform)) flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  if (hasUsage(usage, BufferUsage::Storage)) flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (hasUsage(usage, BufferUsage::Indirect)) flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  if (hasUsage(usage, BufferUsage::TransferSrc)) flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if (hasUsage(usage, BufferUsage::TransferDst)) flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  return flags;
}

VkImageUsageFlags toVkImageUsage(const TextureUsages usage) noexcept {
  VkImageUsageFlags flags = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (hasUsage(usage, TextureUsage::Sampled)) flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
  if (hasUsage(usage, TextureUsage::Storage)) flags |= VK_IMAGE_USAGE_STORAGE_BIT;
  if (hasUsage(usage, TextureUsage::ColorAttachment)) flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  if (hasUsage(usage, TextureUsage::DepthStencilAttachment)) flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  // Immutable and External add nothing here: the first is a GL storage hint, the second is satisfied by
  // the GPU-only allocation this backend always makes. A texture with neither sampled nor attachment
  // usage is still legal and is what a staging texture looks like.
  return flags;
}

} // namespace anonymous

// ==================================================================================================
// Construction and teardown
// ==================================================================================================

VulkanDevice::VulkanDevice() = default;

VulkanDevice::~VulkanDevice() {
  // A device that failed part-way through `initialize` still owns whatever it managed to create: an
  // instance, a physical-device selection, maybe the logical device. The destructor has to release those
  // rather than return early, or a Vulkan-capable machine whose driver rejected one feature would leak an
  // instance for the life of the process.
  if (mApi == nullptr) {
    return;
  }

  stopPipelineWorker();

  if (mDevice != VK_NULL_HANDLE) {
    mApi->vkDeviceWaitIdle(mDevice);
  }

  if (mProbeSurface != VK_NULL_HANDLE && mInstance != VK_NULL_HANDLE) {
    mApi->vkDestroySurfaceKHR(mInstance, mProbeSurface, nullptr);
    mProbeSurface = VK_NULL_HANDLE;
  }

  releaseFramebuffers();

  // Every pool first, in an order that is never given: nothing else depends on a resource existing.
  mSwapchains.forEach([this](SwapchainHandle, VulkanSwapchain& swapchain) {
    if (swapchain.swapchain != VK_NULL_HANDLE) {
      for (VkImageView view: swapchain.views) {
        if (view != VK_NULL_HANDLE) mApi->vkDestroyImageView(mDevice, view, nullptr);
      }
      mApi->vkDestroySwapchainKHR(mDevice, swapchain.swapchain, nullptr);
    }
  });
  mSurfaces.forEach([this](SurfaceHandle, VulkanSurface& surface) {
    if (surface.surface != VK_NULL_HANDLE) mApi->vkDestroySurfaceKHR(mInstance, surface.surface, nullptr);
  });

  mRenderPipelines.forEach([this](RenderPipelineHandle, VulkanRenderPipeline& pipeline) {
    if (pipeline.pipeline != VK_NULL_HANDLE) mApi->vkDestroyPipeline(mDevice, pipeline.pipeline, nullptr);
  });
  mComputePipelines.forEach([this](ComputePipelineHandle, VulkanComputePipeline& pipeline) {
    if (pipeline.pipeline != VK_NULL_HANDLE) mApi->vkDestroyPipeline(mDevice, pipeline.pipeline, nullptr);
  });
  mRenderPasses.forEach([this](RenderPassHandle, VulkanRenderPass& pass) {
    if (pass.renderPass != VK_NULL_HANDLE && mApi->vkDestroyRenderPass != nullptr) {
      mApi->vkDestroyRenderPass(mDevice, pass.renderPass, nullptr);
      pass.renderPass = VK_NULL_HANDLE;
    }
  });
  mPipelineLayouts.forEach([this](PipelineLayoutHandle, VulkanPipelineLayout& layout) {
    if (layout.layout != VK_NULL_HANDLE) mApi->vkDestroyPipelineLayout(mDevice, layout.layout, nullptr);
  });
  mDescriptorSetLayouts.forEach([this](DescriptorSetLayoutHandle, VulkanDescriptorSetLayout& layout) {
    if (layout.layout != VK_NULL_HANDLE) mApi->vkDestroyDescriptorSetLayout(mDevice, layout.layout, nullptr);
  });
  mShaderModules.forEach([this](ShaderModuleHandle, VulkanShaderModule& module) {
    if (module.module != VK_NULL_HANDLE) mApi->vkDestroyShaderModule(mDevice, module.module, nullptr);
  });
  mSamplers.forEach([this](TextureSamplerHandle, VulkanSampler& sampler) {
    if (sampler.sampler != VK_NULL_HANDLE) mApi->vkDestroySampler(mDevice, sampler.sampler, nullptr);
  });
  mQuerySets.forEach([this](QuerySetHandle, VulkanQuerySet& querySet) {
    if (querySet.pool != VK_NULL_HANDLE) mApi->vkDestroyQueryPool(mDevice, querySet.pool, nullptr);
  });
  mFences.forEach([this](FenceHandle, VulkanFence& fence) {
    if (fence.fence != VK_NULL_HANDLE) mApi->vkDestroyFence(mDevice, fence.fence, nullptr);
  });
  mSemaphores.forEach([this](SemaphoreHandle, VulkanSemaphore& semaphore) {
    if (semaphore.semaphore != VK_NULL_HANDLE) mApi->vkDestroySemaphore(mDevice, semaphore.semaphore, nullptr);
  });
  mTextureViews.forEach([this](TextureViewHandle, VulkanTextureView& view) {
    if (view.view != VK_NULL_HANDLE) mApi->vkDestroyImageView(mDevice, view.view, nullptr);
  });
  mTextures.forEach([this](TextureHandle, VulkanTexture& texture) {
    if (texture.view != VK_NULL_HANDLE) mApi->vkDestroyImageView(mDevice, texture.view, nullptr);
    if (texture.image != VK_NULL_HANDLE) vmaDestroyImage(mAllocator, texture.image, texture.allocation);
  });
  mBuffers.forEach([this](BufferHandle, VulkanBuffer& buffer) {
    if (buffer.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(mAllocator, buffer.buffer, buffer.allocation);
  });

  destroyFallbackResources();

  // The conversions belong to no pool: they are created on demand and live as long as the device, so they go
  // after the samplers and views that name them and before the device itself.
  for (auto& [description, conversion] : mYcbcrConversions) {
    if (conversion != VK_NULL_HANDLE && mApi->vkDestroySamplerYcbcrConversion != nullptr) {
      mApi->vkDestroySamplerYcbcrConversion(mDevice, conversion, nullptr);
    }
  }
  mYcbcrConversions.clear();

  if (mPipelineCache != VK_NULL_HANDLE) {
    if (!mPipelineCachePath.empty() && mApi->vkGetPipelineCacheData != nullptr) {
      size_t bytes = 0;
      if (mApi->vkGetPipelineCacheData(mDevice, mPipelineCache, &bytes, nullptr) == VK_SUCCESS && bytes != 0) {
        std::vector<char> data(bytes);
        if (mApi->vkGetPipelineCacheData(mDevice, mPipelineCache, &bytes, data.data()) == VK_SUCCESS) {
          std::ofstream output(mPipelineCachePath, std::ios::binary | std::ios::trunc);
          if (output) output.write(data.data(), static_cast<std::streamsize>(bytes));
        }
      }
    }
    mApi->vkDestroyPipelineCache(mDevice, mPipelineCache, nullptr);
    mPipelineCache = VK_NULL_HANDLE;
  }

  if (mEmptyPipelineLayout != VK_NULL_HANDLE) {
    mApi->vkDestroyPipelineLayout(mDevice, mEmptyPipelineLayout, nullptr);
  }
  if (mEmptyDescriptorSetLayout != VK_NULL_HANDLE) {
    mApi->vkDestroyDescriptorSetLayout(mDevice, mEmptyDescriptorSetLayout, nullptr);
  }

  if (mAllocator != nullptr) vmaDestroyAllocator(mAllocator);
  if (mDebugMessenger != VK_NULL_HANDLE && mApi->vkDestroyDebugUtilsMessengerEXT != nullptr) {
    mApi->vkDestroyDebugUtilsMessengerEXT(mInstance, mDebugMessenger, nullptr);
  }
  if (mDevice != VK_NULL_HANDLE && mApi->vkDestroyDevice != nullptr) mApi->vkDestroyDevice(mDevice, nullptr);
  if (mInstance != VK_NULL_HANDLE && mApi->vkDestroyInstance != nullptr) mApi->vkDestroyInstance(mInstance, nullptr);
  mApi->unload();

  mInitialized = false;
}

// ==================================================================================================
// Initialisation
// ==================================================================================================

Result VulkanDevice::initialize(const DeviceConfiguration& configuration) {
  mMaxFramesInFlight = configuration.maxFramesInFlight == 0 ? 1 : configuration.maxFramesInFlight;
  mPipelineCachePath = configuration.pipelineCachePath;
  mNativeWindowSystem = configuration.surface != nullptr ? configuration.surface->nativeWindowSystem
                                                         : NativeWindowSystem::Unknown;
  mApi = std::make_unique<VulkanApi>();
  if (const Result loaded = mApi->loadLibrary(); !isSuccess(loaded)) {
    emitMessage(MessageSeverity::Error,
            "no Vulkan loader could be opened: the platform's loader library is missing. The backend "
            "reports Unsupported so the factory can fall through to OpenGL.");
    return Result::Unsupported;
  }
  if (const Result globals = mApi->loadGlobalFunctions(); !isSuccess(globals)) {
    emitMessage(MessageSeverity::Error, "the Vulkan loader does not export vkCreateInstance");
    return Result::Unsupported;
  }

  if (const Result created = createInstance(configuration); !isSuccess(created)) {
    return created;
  }

  // The device has to choose its present queue family before `vkCreateDevice`, but the public surface
  // handle is created later. A temporary surface on the same native window is enough for
  // `vkGetPhysicalDeviceSurfaceSupportKHR`; the real surface is created later against the selected
  // family and is checked again.
  if (configuration.surface != nullptr && configuration.surface->nativeWindow != nullptr) {
    const Result probe =
            createPlatformSurface(*mApi, mInstance, *configuration.surface, &mProbeSurface);
    if (!isSuccess(probe)) {
      mProbeSurface = VK_NULL_HANDLE;
      emitMessage(MessageSeverity::Warning,
              "the window surface could not be created for present-queue selection; the graphics "
              "queue will be used for presentation");
    }
  }

  if (const Result picked = pickPhysicalDevice(configuration); !isSuccess(picked)) {
    return picked;
  }
  if (const Result logical = createLogicalDevice(); !isSuccess(logical)) {
    return logical;
  }
  if (mProbeSurface != VK_NULL_HANDLE) {
    mApi->vkDestroySurfaceKHR(mInstance, mProbeSurface, nullptr);
    mProbeSurface = VK_NULL_HANDLE;
  }
  if (const Result allocator = createAllocator(); !isSuccess(allocator)) {
    return allocator;
  }

  probeCapabilities();
  createFallbackResources();

  // The renderer's hard requirements are checked before the device is handed out: a backend that
  // cannot satisfy them is reported as Unsupported so the factory can try the next one.
  if (configuration.requiredFeatures != 0 &&
      (mCapabilities.features & configuration.requiredFeatures) != configuration.requiredFeatures) {
    const Features missing = configuration.requiredFeatures & ~mCapabilities.features;
    std::string message = "the Vulkan backend cannot satisfy the renderer's required features:";
    for (uint32_t bit = 0; bit < 32; ++bit) {
      const auto feature = static_cast<Feature>(1u << bit);
      if ((missing & static_cast<Features>(feature)) == 0) continue;
      message += ' ';
      message += toString(feature);
      message += " (";
      message += toString(mCapabilities.reasonFor(feature));
      message += ')';
    }
    emitMessage(MessageSeverity::Error, message);
    return Result::Unsupported;
  }

  if (configuration.capabilityOverride != nullptr) {
    mCapabilities = *configuration.capabilityOverride;
  }

  // The queue object is bound now that the queue exists; its VkQueue member was created with the
  // device and could not be named at construction time.
  mGraphicsQueueObject = VulkanQueue(*this, mGraphicsQueue, QueueType::Graphics);

  mInitialized = true;
  return Result::Success;
}

Result VulkanDevice::createInstance(const DeviceConfiguration& configuration) {
  uint32_t loaderVersion = VK_API_VERSION_1_0;
  if (mApi->vkEnumerateInstanceVersion != nullptr) {
    mApi->vkEnumerateInstanceVersion(&loaderVersion);
  }

  // The instance asks for at most 1.3: the device may well be 1.4, and a higher instance version than
  // the loader understands is rejected. Dynamic rendering is core from 1.3, which is the only version
  // this backend cares about.
  uint32_t apiVersion = std::min(loaderVersion, VK_API_VERSION_1_3);
  if (apiVersion < VK_API_VERSION_1_0) apiVersion = VK_API_VERSION_1_0;

  // ---------------------------------------------------------------- extensions

  uint32_t extensionCount = 0;
  mApi->vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
  std::vector<VkExtensionProperties> available(extensionCount);
  if (extensionCount != 0) {
    mApi->vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, available.data());
  }

  std::vector<const char*> extensions;
  const auto addExtension = [&](const char* name) {
    for (const char* existing: extensions) {
      if (std::strcmp(existing, name) == 0) return;
    }
    extensions.push_back(name);
  };

  if (hasExtension(available, VK_KHR_SURFACE_EXTENSION_NAME)) {
    addExtension(VK_KHR_SURFACE_EXTENSION_NAME);
    if (const char* platform = platformSurfaceExtensionName(configuration.surface);
        platform != nullptr && hasExtension(available, platform)) {
      addExtension(platform);
    }
  }

  mHasDebugUtils = hasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  if (configuration.enableValidation && mHasDebugUtils) {
    addExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  }

  if (const SurfaceDescriptor* surface = configuration.surface; surface != nullptr) {
    for (uint32_t index = 0; index < surface->instanceExtensionCount; ++index) {
      const char* name = surface->instanceExtensions[index];
      if (name != nullptr && hasExtension(available, name)) addExtension(name);
    }
  }

  // ---------------------------------------------------------------- layers

  std::vector<const char*> layers;
  if (configuration.enableValidation) {
    uint32_t layerCount = 0;
    mApi->vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layerProperties(layerCount);
    if (layerCount != 0) {
      mApi->vkEnumerateInstanceLayerProperties(&layerCount, layerProperties.data());
      for (const VkLayerProperties& layer: layerProperties) {
        if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
          mHasValidationLayer = true;
          mValidationLayerName = layer.layerName;
          layers.push_back(mValidationLayerName.c_str());
          break;
        }
      }
    }
    if (!mHasValidationLayer) {
      emitMessage(MessageSeverity::Warning,
              "validation was requested but VK_LAYER_KHRONOS_validation is not installed; "
              "the device continues without it");
    }
  }

  VkApplicationInfo application{};
  application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  application.pApplicationName = configuration.appName != nullptr ? configuration.appName : "Coral";
  application.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
  application.pEngineName = "Coral";
  application.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
  application.apiVersion = apiVersion;

  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.pApplicationInfo = &application;
  info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  info.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
  info.enabledLayerCount = static_cast<uint32_t>(layers.size());
  info.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();

  const VkResult created = mApi->vkCreateInstance(&info, nullptr, &mInstance);
  if (created != VK_SUCCESS) {
    std::string message = "vkCreateInstance failed with VkResult ";
    message += std::to_string(static_cast<int>(created));
    message += ". The loader is installed but no driver answered, which usually means no Vulkan "
               "runtime is installed for this GPU.";
    emitMessage(MessageSeverity::Error, message);
    return Result::Unsupported;
  }

  mApi->loadInstanceFunctions(mInstance);
  if (!mApi->hasRequiredFunctions()) {
    emitMessage(MessageSeverity::Error,
            "the Vulkan loader resolved an instance but not the core entry points this backend "
            "requires; the driver is too old or the loader is incomplete");
    return Result::Unsupported;
  }

  if (configuration.enableValidation && mHasDebugUtils && mApi->vkCreateDebugUtilsMessengerEXT != nullptr) {
    if (const Result messenger = captureDebugMessenger(); !isSuccess(messenger)) {
      emitMessage(MessageSeverity::Warning, "the Vulkan debug messenger could not be created");
      mHasDebugUtils = false;
    }
  }
  return Result::Success;
}

Result VulkanDevice::captureDebugMessenger() {
  VkDebugUtilsMessengerCreateInfoEXT info{};
  info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
  info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  info.pfnUserCallback = &VulkanDevice::debugCallback;
  info.pUserData = this;
  return toResult(mApi->vkCreateDebugUtilsMessengerEXT(mInstance, &info, nullptr, &mDebugMessenger));
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDevice::debugCallback(const VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT* data, void* userData) {
  (void) types;
  auto* device = static_cast<VulkanDevice*>(userData);
  if (device == nullptr || data == nullptr || data->pMessage == nullptr) return VK_FALSE;

  MessageSeverity mapped = MessageSeverity::Info;
  if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
    mapped = MessageSeverity::Error;
  } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
    mapped = MessageSeverity::Warning;
  } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) != 0) {
    mapped = MessageSeverity::Info;
  } else {
    mapped = MessageSeverity::Verbose;
  }
  device->emitMessage(mapped, data->pMessage);
  return VK_FALSE;
}

Result VulkanDevice::pickPhysicalDevice(const DeviceConfiguration& configuration) {
  uint32_t deviceCount = 0;
  if (const VkResult result = mApi->vkEnumeratePhysicalDevices(mInstance, &deviceCount, nullptr);
      result != VK_SUCCESS) {
    return Result::Unsupported;
  }
  if (deviceCount == 0) {
    emitMessage(MessageSeverity::Error, "the Vulkan loader reported no physical devices");
    return Result::Unsupported;
  }

  std::vector<VkPhysicalDevice> devices(deviceCount);
  mApi->vkEnumeratePhysicalDevices(mInstance, &deviceCount, devices.data());

  VkPhysicalDevice best = VK_NULL_HANDLE;
  uint32_t bestScore = 0;

  for (const VkPhysicalDevice device: devices) {
    VkPhysicalDeviceProperties properties{};
    mApi->vkGetPhysicalDeviceProperties(device, &properties);

    // A graphics queue is the one thing the backend cannot do without.
    uint32_t familyCount = 0;
    mApi->vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    mApi->vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
    uint32_t graphicsFamily = std::numeric_limits<uint32_t>::max();
    for (uint32_t index = 0; index < familyCount; ++index) {
      if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && families[index].queueCount > 0) {
        graphicsFamily = index;
        break;
      }
    }
    if (graphicsFamily == std::numeric_limits<uint32_t>::max()) continue;

    const bool needsSurface = configuration.surface != nullptr && configuration.surface->nativeWindow != nullptr;
    bool graphicsQueuePresents = false;
    if (needsSurface && mProbeSurface != VK_NULL_HANDLE && mApi->vkGetPhysicalDeviceSurfaceSupportKHR != nullptr) {
      bool anyPresent = false;
      for (uint32_t index = 0; index < familyCount; ++index) {
        if (families[index].queueCount == 0) continue;
        VkBool32 supported = VK_FALSE;
        if (mApi->vkGetPhysicalDeviceSurfaceSupportKHR(device, index, mProbeSurface, &supported) != VK_SUCCESS) {
          continue;
        }
        if (supported == VK_TRUE) {
          anyPresent = true;
          if (index == graphicsFamily) graphicsQueuePresents = true;
        }
      }
      // A surface needs some queue family that can present. Prefer the same family because it avoids
      // the swapchain's CONCURRENT sharing mode, but a separate present family is valid.
      if (!anyPresent) continue;
    }

    uint32_t extensionCount = 0;
    mApi->vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extensionCount);
    if (extensionCount != 0) {
      mApi->vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());
    }
    const bool hasSwapchain = hasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    const bool hasDynamicRendering = hasExtension(extensions, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);

    // A window surface needs the swapchain; a headless device does not. Dynamic rendering is preferred
    // and scores higher, but it is no longer required: Vulkan 1.0/1.1 devices take the render-pass path.
    if (needsSurface && !hasSwapchain) continue;

    uint32_t score = scoreDevice(properties.deviceType);
    if (hasSwapchain) score += 50;
    if (hasDynamicRendering) score += 20;
    if (graphicsQueuePresents) score += 10;
    if (score > bestScore) {
      bestScore = score;
      best = device;
    }
  }

  if (best == VK_NULL_HANDLE) {
    emitMessage(MessageSeverity::Error,
            "no Vulkan physical device provides a graphics queue (and a swapchain when a window is requested)");
    return Result::Unsupported;
  }

  mPhysicalDevice = best;
  mApi->vkGetPhysicalDeviceProperties(mPhysicalDevice, &mProperties);
  mApi->vkGetPhysicalDeviceMemoryProperties(mPhysicalDevice, &mMemoryProperties);

  uint32_t extensionCount = 0;
  mApi->vkEnumerateDeviceExtensionProperties(mPhysicalDevice, nullptr, &extensionCount, nullptr);
  std::vector<VkExtensionProperties> extensions(extensionCount);
  if (extensionCount != 0) {
    mApi->vkEnumerateDeviceExtensionProperties(mPhysicalDevice, nullptr, &extensionCount, extensions.data());
  }

  mDeviceExtensions.clear();
  mDynamicRenderingExtension = hasExtension(extensions, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
  mDynamicRenderingCore = !mDynamicRenderingExtension && mProperties.apiVersion >= VK_API_VERSION_1_3;
  mHasSwapchainExtension = hasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME);

  // ## External memory: the platform's own handle type
  //
  // Importing a platform texture is only possible when the device enabled the extension for the handle
  // type it comes in as. Those extensions build on `VK_KHR_external_memory`, which is core in Vulkan 1.1,
  // so a 1.0 device is answered with "no" instead of a device that would fail at the first import: every
  // driver this project runs on is 1.1 or newer.
  //
  // The probe is a *device* capability, which is why it is recorded here rather than being a compile-time
  // answer: a driver without the extension must fall back to the CPU upload path, and
  // `supportsNativeTextureImport` is what tells the caller to take it.
  //
  // On Android the extension is only enabled when the runtime can also take a reference on the buffer
  // (`androidHardwareBufferApi()`, resolved through `dlopen` because the app targets API 23 while the
  // symbols start at 26): an imported image whose buffer the producer may free at any moment is not one
  // this backend wants to hand out.
#if defined(_WIN32)
  mHasExternalMemoryWin32 = mProperties.apiVersion >= VK_API_VERSION_1_1 &&
                            hasExtension(extensions, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
#elif defined(__ANDROID__)
  mHasExternalMemoryAndroidHardwareBuffer = mProperties.apiVersion >= VK_API_VERSION_1_1 &&
                                            hasExtension(extensions,
                                              VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME) &&
                                            androidHardwareBufferApi().available;
#endif

  // Test hook for the Vulkan 1.0/1.1 path on a modern driver: the render-pass path is otherwise only
  // reachable on hardware that lacks dynamic rendering, which is exactly what a desktop CI machine does
  // not have. It removes the extension and the device feature so the code runs as it would on a 1.1
  // device, without changing what the driver reports.
  if (std::getenv("CORAL_VULKAN_FORCE_RENDER_PASS") != nullptr) {
    mDynamicRenderingExtension = false;
    mDynamicRenderingCore = false;
  }

  if (mDynamicRenderingExtension) mDeviceExtensions.emplace_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
  if (mHasSwapchainExtension) mDeviceExtensions.emplace_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  // HDR10 without metadata is a PQ signal the compositor has to guess about; the extension is optional
  // and only matters for the HDR path, so it is enabled when offered and skipped otherwise.
  if (hasExtension(extensions, VK_EXT_HDR_METADATA_EXTENSION_NAME)) {
    mDeviceExtensions.emplace_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);
  }
#if defined(_WIN32)
  // The name is declared by `vulkan_win32.h`, which only exists in a Windows build, so the enablement is
  // what has to be guarded: no other platform can have a Direct3D handle type to enable it for.
  if (mHasExternalMemoryWin32) mDeviceExtensions.emplace_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
#elif defined(__ANDROID__)
  if (mHasExternalMemoryAndroidHardwareBuffer) {
    mDeviceExtensions.emplace_back(VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
    // Core in 1.1, but a driver may only take the conversion path when the extension is enabled as well.
    if (hasExtension(extensions, VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME)) {
      mDeviceExtensions.emplace_back(VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME);
    }
  }
#endif

  // The logical device asks for the lower of the instance's version and the device's: a 1.1 device on
  // a 1.4 loader is still a 1.1 device.
  uint32_t loaderVersion = VK_API_VERSION_1_0;
  if (mApi->vkEnumerateInstanceVersion != nullptr) mApi->vkEnumerateInstanceVersion(&loaderVersion);
  mDeviceApiVersion = std::min({ mProperties.apiVersion, loaderVersion, VK_API_VERSION_1_3 });

  mGraphicsQueueFamily = std::numeric_limits<uint32_t>::max();
  uint32_t familyCount = 0;
  mApi->vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice, &familyCount, nullptr);
  std::vector<VkQueueFamilyProperties> families(familyCount);
  mApi->vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice, &familyCount, families.data());
  for (uint32_t index = 0; index < familyCount; ++index) {
    if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && families[index].queueCount > 0) {
      mGraphicsQueueFamily = index;
      break;
    }
  }
  if (mGraphicsQueueFamily == std::numeric_limits<uint32_t>::max()) {
    return Result::Unsupported;
  }
  return Result::Success;
}

Result VulkanDevice::createLogicalDevice() {
  const float priority = 1.0f;

  // Pick the present family before creating the device: a swapchain created with EXCLUSIVE sharing
  // needs both families at creation, and a device whose graphics family cannot present needs a second
  // queue. Prefer the graphics family when it presents, so the common path stays one queue.
  mPresentQueueFamily = mGraphicsQueueFamily;
  if (mProbeSurface != VK_NULL_HANDLE && mApi->vkGetPhysicalDeviceSurfaceSupportKHR != nullptr) {
    uint32_t familyCount = 0;
    mApi->vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    mApi->vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice, &familyCount, families.data());

    bool graphicsPresents = false;
    uint32_t fallbackPresent = std::numeric_limits<uint32_t>::max();
    for (uint32_t index = 0; index < familyCount; ++index) {
      if (families[index].queueCount == 0) continue;
      VkBool32 supported = VK_FALSE;
      if (mApi->vkGetPhysicalDeviceSurfaceSupportKHR(mPhysicalDevice, index, mProbeSurface, &supported) !=
              VK_SUCCESS ||
          supported != VK_TRUE) {
        continue;
      }
      if (index == mGraphicsQueueFamily) {
        graphicsPresents = true;
      } else if (fallbackPresent == std::numeric_limits<uint32_t>::max()) {
        fallbackPresent = index;
      }
    }
    if (!graphicsPresents) {
      if (fallbackPresent == std::numeric_limits<uint32_t>::max()) {
        emitMessage(MessageSeverity::Error,
                "no Vulkan queue family can present to the window surface");
        return Result::Unsupported;
      }
      mPresentQueueFamily = fallbackPresent;
    }
  }

  std::array<VkDeviceQueueCreateInfo, 2> queueInfos{};
  queueInfos[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queueInfos[0].queueFamilyIndex = mGraphicsQueueFamily;
  queueInfos[0].queueCount = 1;
  queueInfos[0].pQueuePriorities = &priority;
  uint32_t queueInfoCount = 1;
  if (mPresentQueueFamily != mGraphicsQueueFamily) {
    queueInfos[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfos[1].queueFamilyIndex = mPresentQueueFamily;
    queueInfos[1].queueCount = 1;
    queueInfos[1].pQueuePriorities = &priority;
    queueInfoCount = 2;
  }

  VkPhysicalDeviceFeatures available{};
  mApi->vkGetPhysicalDeviceFeatures(mPhysicalDevice, &available);

  VkPhysicalDeviceFeatures features{};
  features.samplerAnisotropy = available.samplerAnisotropy;
  features.fillModeNonSolid = available.fillModeNonSolid;
  features.depthClamp = available.depthClamp;
  features.logicOp = available.logicOp;
  features.sampleRateShading = available.sampleRateShading;
  features.imageCubeArray = available.imageCubeArray;
  features.geometryShader = available.geometryShader;
  features.tessellationShader = available.tessellationShader;
  features.independentBlend = available.independentBlend;
  features.dualSrcBlend = available.dualSrcBlend;
  features.pipelineStatisticsQuery = available.pipelineStatisticsQuery;
  features.occlusionQueryPrecise = available.occlusionQueryPrecise;
  features.shaderStorageImageWriteWithoutFormat = available.shaderStorageImageWriteWithoutFormat;

  // The dynamic-rendering feature has to be enabled through whichever structure the device's API
  // version expects: the KHR feature struct when the extension is in use, the 1.3 aggregate when the
  // functionality is core.
  VkPhysicalDeviceDynamicRenderingFeatures dynamicRendering{};
  dynamicRendering.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
  dynamicRendering.dynamicRendering = VK_TRUE;

  VkPhysicalDeviceVulkan13Features vulkan13{};
  vulkan13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  vulkan13.dynamicRendering = VK_TRUE;

  // samplerYcbcrConversion is core in 1.1 but the *feature* still has to be enabled: without it an image
  // with an external format cannot be imported at all, and the driver reports that as
  // VK_ERROR_FORMAT_NOT_SUPPORTED from the format probe rather than as a missing feature. It is requested
  // only when the device has it, because enabling an unsupported feature fails device creation.
  VkPhysicalDeviceVulkan11Features vulkan11{};
  vulkan11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
  if (mDeviceApiVersion >= VK_API_VERSION_1_1 && mApi->vkGetPhysicalDeviceFeatures2 != nullptr) {
    VkPhysicalDeviceVulkan11Features probe{};
    probe.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &probe;
    mApi->vkGetPhysicalDeviceFeatures2(mPhysicalDevice, &features2);
    vulkan11.samplerYcbcrConversion = probe.samplerYcbcrConversion;
  }

  VkDeviceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  info.queueCreateInfoCount = queueInfoCount;
  info.pQueueCreateInfos = queueInfos.data();
  info.pEnabledFeatures = &features;
  info.enabledExtensionCount = static_cast<uint32_t>(mDeviceExtensions.size());
  std::vector<const char*> extensionNames;
  extensionNames.reserve(mDeviceExtensions.size());
  for (const std::string& extension: mDeviceExtensions) extensionNames.push_back(extension.c_str());
  info.ppEnabledExtensionNames = extensionNames.empty() ? nullptr : extensionNames.data();

  if (mDynamicRenderingExtension) {
    info.pNext = &dynamicRendering;
    if (vulkan11.samplerYcbcrConversion == VK_TRUE) dynamicRendering.pNext = &vulkan11;
  } else if (mDynamicRenderingCore) {
    info.pNext = &vulkan13;
    if (vulkan11.samplerYcbcrConversion == VK_TRUE) vulkan13.pNext = &vulkan11;
  } else if (vulkan11.samplerYcbcrConversion == VK_TRUE) {
    info.pNext = &vulkan11;
  }

  if (const VkResult created = mApi->vkCreateDevice(mPhysicalDevice, &info, nullptr, &mDevice);
      created != VK_SUCCESS) {
    std::string message = "vkCreateDevice failed with VkResult ";
    message += std::to_string(static_cast<int>(created));
    emitMessage(MessageSeverity::Error, message);
    return toResult(created) == Result::Success ? Result::Error : toResult(created);
  }

  mApi->vkGetDeviceQueue(mDevice, mGraphicsQueueFamily, 0, &mGraphicsQueue);
  if (mPresentQueueFamily == mGraphicsQueueFamily) {
    mPresentQueue = mGraphicsQueue;
  } else {
    mApi->vkGetDeviceQueue(mDevice, mPresentQueueFamily, 0, &mPresentQueue);
  }

  if (!mPipelineCachePath.empty()) {
    std::vector<char> initialData;
    std::ifstream input(mPipelineCachePath, std::ios::binary);
    if (input) {
      input.seekg(0, std::ios::end);
      const std::streamoff size = input.tellg();
      if (size > 0) {
        input.seekg(0, std::ios::beg);
        initialData.resize(static_cast<size_t>(size));
        input.read(initialData.data(), size);
      }
    }
    VkPipelineCacheCreateInfo cacheInfo{};
    cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    cacheInfo.initialDataSize = initialData.size();
    cacheInfo.pInitialData = initialData.empty() ? nullptr : initialData.data();
    if (mApi->vkCreatePipelineCache(mDevice, &cacheInfo, nullptr, &mPipelineCache) != VK_SUCCESS) {
      mPipelineCache = VK_NULL_HANDLE;
    }
  }

  // A zero-binding layout is the answer for a pipeline-layout slot the caller left unused, which is how
  // a renderer reserves a set index for an optional feature. Vulkan requires a real layout for every
  // set in a pipeline layout, so the empty one is created once and reused.
  VkDescriptorSetLayoutCreateInfo emptyLayout{};
  emptyLayout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  if (const VkResult result =
                  mApi->vkCreateDescriptorSetLayout(mDevice, &emptyLayout, nullptr, &mEmptyDescriptorSetLayout);
      result != VK_SUCCESS) {
    return toResult(result);
  }

  VkPipelineLayoutCreateInfo emptyPipeline{};
  emptyPipeline.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  if (const VkResult result = mApi->vkCreatePipelineLayout(mDevice, &emptyPipeline, nullptr, &mEmptyPipelineLayout);
      result != VK_SUCCESS) {
    return toResult(result);
  }
  return Result::Success;
}

Result VulkanDevice::createAllocator() {
  VmaVulkanFunctions functions{};
  functions.vkGetInstanceProcAddr = mApi->vkGetInstanceProcAddr;
  functions.vkGetDeviceProcAddr = mApi->vkGetDeviceProcAddr;
  if (mApi->vkGetDeviceProcAddr != nullptr) {
    mSetHdrMetadata = reinterpret_cast<PFN_vkSetHdrMetadataEXT>(
      mApi->vkGetDeviceProcAddr(mDevice, "vkSetHdrMetadataEXT"));
  }

  VmaAllocatorCreateInfo info{};
  info.flags = 0;
  info.physicalDevice = mPhysicalDevice;
  info.device = mDevice;
  info.instance = mInstance;
  info.vulkanApiVersion = mDeviceApiVersion;
  info.pVulkanFunctions = &functions;

  if (const VkResult result = vmaCreateAllocator(&info, &mAllocator); result != VK_SUCCESS) {
    emitMessage(MessageSeverity::Error, "Vulkan Memory Allocator could not be created");
    return toResult(result);
  }
  return Result::Success;
}

// ==================================================================================================
// Capabilities
// ==================================================================================================

void VulkanDevice::probeCapabilities() {
  const VkPhysicalDeviceProperties& properties = mProperties;
  VkPhysicalDeviceFeatures available{};
  mApi->vkGetPhysicalDeviceFeatures(mPhysicalDevice, &available);

  mVendor = vendorName(properties.vendorID);
  mDeviceName = properties.deviceName;
  mApiVersion = "Vulkan " + formatVersion(properties.apiVersion);
  mDriverVersion = formatVersion(properties.driverVersion);
  mBackendName = std::string("coral/backend/vulkan/") + platformWindowSystemName(mNativeWindowSystem);

  mInfo.backend = Backend::Vulkan;
  mInfo.vendor = mVendor.c_str();
  mInfo.deviceName = mDeviceName.c_str();
  mInfo.apiVersion = mApiVersion.c_str();
  mInfo.driverVersion = mDriverVersion.c_str();
  mInfo.backendName = mBackendName.c_str();
  mInfo.acceptsSpirv = true;
  mInfo.acceptsGlslDirect = false;

  CapabilityBuilder builder(mCapabilities);
  builder.apiVersion(mInfo.apiVersion);

  mCapabilities.acceptsSpirv = true;
  mCapabilities.acceptsGlslDirect = false;
  mCapabilities.runtimeShaderCompilation = isGlslCompilerAvailable();
  mCapabilities.supportsDebugMarkers = mHasDebugUtils;

  // ---------------------------------------------------------------- limits
  const VkPhysicalDeviceLimits& limits = properties.limits;
  Limits values;
  values.maxTextureDimension1D = limits.maxImageDimension1D;
  values.maxTextureDimension2D = limits.maxImageDimension2D;
  values.maxTextureDimension3D = limits.maxImageDimension3D;
  values.maxTextureDimensionCube = limits.maxImageDimensionCube;
  values.maxTextureArrayLayers = limits.maxImageArrayLayers;
  values.maxVertexBuffers = std::min(limits.maxVertexInputBindings, 16u);
  values.maxVertexAttributes = std::min(limits.maxVertexInputAttributes, 32u);
  values.maxVertexBufferStride = limits.maxVertexInputBindingStride;
  values.maxBufferSize = limits.maxStorageBufferRange != 0 ? limits.maxStorageBufferRange : 256u * 1024u * 1024u;
  values.maxUniformBufferBindingSize = limits.maxUniformBufferRange;
  values.maxStorageBufferBindingSize = limits.maxStorageBufferRange;
  values.maxUniformBuffersPerStage = std::min(limits.maxPerStageDescriptorUniformBuffers, 16u);
  values.maxStorageBuffersPerStage = std::min(limits.maxPerStageDescriptorStorageBuffers, 16u);
  values.maxSampledTexturesPerStage = std::min(limits.maxPerStageDescriptorSampledImages, 16u);
  values.maxSamplersPerStage = std::min(limits.maxPerStageDescriptorSamplers, 16u);
  values.maxBoundDescriptorSets = std::min({ limits.maxBoundDescriptorSets, kMaxDescriptorSetLayouts, 8u });
  values.maxColorAttachments = std::min(limits.maxColorAttachments, kMaxColorAttachments);
  values.maxComputeWorkgroupSizeX = limits.maxComputeWorkGroupSize[0];
  values.maxComputeWorkgroupSizeY = limits.maxComputeWorkGroupSize[1];
  values.maxComputeWorkgroupSizeZ = limits.maxComputeWorkGroupSize[2];
  values.maxComputeInvocationsPerWorkgroup = limits.maxComputeWorkGroupInvocations;
  values.maxComputeWorkgroupCountX = limits.maxComputeWorkGroupCount[0];
  values.maxComputeWorkgroupCountY = limits.maxComputeWorkGroupCount[1];
  values.maxComputeWorkgroupCountZ = limits.maxComputeWorkGroupCount[2];
  values.maxFramesInFlight = mMaxFramesInFlight;
  // The limit is the caller's CPU-ahead budget. `Queue::submit` with a fence is asynchronous and lets
  // a caller keep that many command buffers alive; `executeCommandBuffer` remains the synchronous
  // convenience path, and the renderer has to provide per-frame resources to exploit the budget.

  // The push-constant range a pipeline layout declares, and the number the RHI reports. A 128 byte
  // range is what every Vulkan implementation must provide; a larger one is used only up to 128 here
  // so the backend's own layout and the reported budget cannot disagree.
  mPushConstantBudget = limits.maxPushConstantsSize >= 128 ? 128u : limits.maxPushConstantsSize;
  values.maxPushConstantSize = mPushConstantBudget;
  mCapabilities.limits = values;

  mCapabilities.maxSamplerAnisotropy = mProperties.limits.maxSamplerAnisotropy >= 1.0f
                                         ? mProperties.limits.maxSamplerAnisotropy
                                         : 1.0f;
  mCapabilities.maxTextureSize = values.maxTextureDimension2D;
  mCapabilities.maxCubeMapSize = values.maxTextureDimensionCube;
  mCapabilities.maxRenderbufferSize = limits.maxFramebufferWidth;
  mCapabilities.maxViewportWidth = limits.maxViewportDimensions[0];
  mCapabilities.maxViewportHeight = limits.maxViewportDimensions[1];
  mCapabilities.maxFragmentTextureUnits = values.maxSampledTexturesPerStage;
  mCapabilities.maxVertexTextureUnits = values.maxSampledTexturesPerStage;
  mCapabilities.maxCombinedTextureUnits = values.maxSampledTexturesPerStage * 2;

  // ---------------------------------------------------------------- features
  //
  // Every bit is a statement, and every unsupported one carries a reason: `reasonFor` is what an upper
  // layer reports when its technique was disabled, and an Available answer for an absent feature is
  // worse than no answer at all.
  const bool hasTimestamp = limits.timestampPeriod > 0.0f && limits.timestampComputeAndGraphics;
  builder.feature(Feature::ComputeShader, true)
          .feature(Feature::ComputeShaderAtomics, true)
          .feature(Feature::IndirectDispatch, true)
          .feature(Feature::ComputeWorkgroupBarrier, true)
          .feature(Feature::GeometryShader, available.geometryShader,
                  FeatureUnavailableReason::HardwareUnsupported)
          .feature(Feature::TessellationShader, available.tessellationShader,
                  FeatureUnavailableReason::HardwareUnsupported)
          .feature(Feature::Texture3D, true)
          .feature(Feature::TextureCube, true)
          .feature(Feature::TextureArray, true)
          .feature(Feature::TextureCubeArray, available.imageCubeArray,
                  FeatureUnavailableReason::HardwareUnsupported)
          .feature(Feature::SamplerAnisotropy, available.samplerAnisotropy,
                  FeatureUnavailableReason::HardwareUnsupported)
          .feature(Feature::SamplerBorderClamp, true)
          .feature(Feature::TextureLod, true)
          .feature(Feature::SeparateSamplers, true)
          .feature(Feature::StorageTexture, true)
          .feature(Feature::ComparisonSampler, true)
          .feature(Feature::MultipleRenderTargets, true)
          .feature(Feature::MultisampleResolve, true)
          .feature(Feature::SampleRateShading, available.sampleRateShading,
                  FeatureUnavailableReason::HardwareUnsupported)
          // Vulkan has no capability bit for alpha-to-coverage: it is core pipeline state, so the
          // feature is reported available and the pipeline simply honours the requested state.
          .feature(Feature::AlphaToCoverage, true)
          .feature(Feature::FillModeNonSolid, available.fillModeNonSolid,
                  FeatureUnavailableReason::HardwareUnsupported)
          .feature(Feature::DepthClamp, available.depthClamp, FeatureUnavailableReason::HardwareUnsupported)
          .feature(Feature::ProgrammableBlending, false, FeatureUnavailableReason::ArchitecturallyImpossible)
          .feature(Feature::LogicOp, available.logicOp, FeatureUnavailableReason::HardwareUnsupported)
          .feature(Feature::IntegerVertexAttributes, true)
          .feature(Feature::HostWaitFence, true)
          .feature(Feature::TimelineSemaphore, false, FeatureUnavailableReason::NotImplementedByBackend)
          .feature(Feature::QueryObjects, true)
          .feature(Feature::TimestampQuery, hasTimestamp, FeatureUnavailableReason::HardwareUnsupported)
          .feature(Feature::PipelineStatisticsQuery, available.pipelineStatisticsQuery,
                  FeatureUnavailableReason::HardwareUnsupported)
          .feature(Feature::Swapchain, mHasSwapchainExtension, FeatureUnavailableReason::ExtensionMissing)
          .feature(Feature::NativeSurface, mHasSwapchainExtension, FeatureUnavailableReason::ExtensionMissing);

  // ---------------------------------------------------------------- sample counts
  const VkSampleCountFlags colorCounts = limits.framebufferColorSampleCounts;
  const VkSampleCountFlags depthCounts = limits.framebufferDepthSampleCounts;
  for (const uint32_t count: { 1u, 2u, 4u }) {
    const VkSampleCountFlagBits bit = static_cast<VkSampleCountFlagBits>(count);
    if ((colorCounts & bit) != 0 && (depthCounts & bit) != 0) {
      builder.sampleCount(count);
    }
  }
  uint32_t maxSamples = 1;
  for (const uint32_t count: { 4u, 2u }) {
    if ((colorCounts & static_cast<VkSampleCountFlagBits>(count)) != 0) {
      maxSamples = count;
      break;
    }
  }
  mCapabilities.maxSamples = maxSamples;

  // ---------------------------------------------------------------- formats
  //
  // Queried rather than tabulated: a driver knows which formats it can render to, and a table written
  // here would be a second, staler copy of that answer. The last enumerator is the loop bound because
  // the RHI format list is an enum whose values are contiguous from 1.
  for (uint32_t raw = 1; raw <= static_cast<uint32_t>(PixelFormat::ASTC_8x8_SRGB); ++raw) {
    const auto format = static_cast<PixelFormat>(raw);
    const VkFormat vkFormat = toVkFormat(format);
    if (vkFormat == VK_FORMAT_UNDEFINED) continue;

    VkFormatProperties formatProperties{};
    mApi->vkGetPhysicalDeviceFormatProperties(mPhysicalDevice, vkFormat, &formatProperties);
    VkFormatFeatureFlags features = formatProperties.optimalTilingFeatures;
    if (features == 0) features = formatProperties.linearTilingFeatures;

    const bool depth = isDepthFormat(format);
    TextureFormatCapabilities support;
    support.sampled = (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    support.renderable = depth ? (features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0
                               : (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
    support.storage = (features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
    support.blendable = (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0;
    support.multisampleResolve = support.renderable;

    const VkSampleCountFlags counts = depth ? depthCounts : colorCounts;
    support.sampleCounts[1] = (counts & VK_SAMPLE_COUNT_1_BIT) != 0;
    support.sampleCounts[2] = (counts & VK_SAMPLE_COUNT_2_BIT) != 0;
    support.sampleCounts[4] = (counts & VK_SAMPLE_COUNT_4_BIT) != 0;
    builder.textureFormat(format, support);
  }

  // The canvas needs these three, and a backend that cannot provide them is not a canvas backend. They
  // are named here so a driver that fails one reports which, instead of failing much later in a pass.
  for (const PixelFormat required: { PixelFormat::R8G8B8A8_UNORM, PixelFormat::R8_UNORM,
             PixelFormat::D24_UNORM_S8_UINT }) {
    const TextureFormatCapabilities& support = mCapabilities.queryTextureFormat(required);
    if (!support.sampled || !support.renderable) {
      std::string message = "the device does not offer ";
      message += getPixelFormatInfo(required).name;
      message += " as a sampled render target, which the canvas requires";
      emitMessage(MessageSeverity::Warning, message);
    }
  }
}

// ==================================================================================================
// Fallbacks and empty state
// ==================================================================================================

void VulkanDevice::createFallbackResources() {
  // ---------------------------------------------------------------- 1x1 texture and sampler
  //
  // A descriptor set may be bound with a binding the caller never wrote (a layout can declare an
  // optional binding). Vulkan requires every statically-used descriptor to be valid, so an unwritten
  // one is filled with this transparent black texel rather than left null - a null image view is a
  // validation error and, on some drivers, a device loss.
  {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = { 1, 1, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    if (vmaCreateImage(mAllocator, &imageInfo, &allocationInfo, &mFallbackImage, &mFallbackAllocation, nullptr) ==
            VK_SUCCESS) {
      VkImageViewCreateInfo viewInfo{};
      viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      viewInfo.image = mFallbackImage;
      viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
      viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
      viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
      mApi->vkCreateImageView(mDevice, &viewInfo, nullptr, &mFallbackView);

      oneShot("fallback-texture", [this](VkCommandBuffer commandBuffer) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = mFallbackImage;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        mApi->vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkClearColorValue clear{};
        clear.float32[0] = 0.0f;
        clear.float32[1] = 0.0f;
        clear.float32[2] = 0.0f;
        clear.float32[3] = 0.0f;
        const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        mApi->vkCmdClearColorImage(commandBuffer, mFallbackImage, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
      });
    }
  }

  {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    mApi->vkCreateSampler(mDevice, &samplerInfo, nullptr, &mFallbackSampler);
  }

  // ---------------------------------------------------------------- fallback buffer
  {
    BufferDescriptor descriptor;
    descriptor.size = kFallbackBufferSize;
    descriptor.usage = static_cast<BufferUsages>(BufferUsage::Uniform | BufferUsage::Storage);
    descriptor.memoryUsage = MemoryUsage::CpuToGpu;
    descriptor.label = "vulkan/fallback-buffer";

    VulkanBuffer buffer;
    if (isSuccess(createBufferInternal(descriptor, &buffer))) {
      mFallbackBuffer = buffer.buffer;
      mFallbackBufferAllocation = buffer.allocation;
    }
  }

  // ---------------------------------------------------------------- fan index buffer
  //
  // Vulkan has no triangle fan, and the canvas draws paths as fans. The index pattern is identical for
  // every fan, so one buffer holds it for all of them: `0, i-1, i` for every i from 2.
  {
    const uint32_t indexCount = (kFanIndexCapacity - 2) * 3;
    std::vector<uint16_t> indices(indexCount);
    for (uint32_t fanVertex = 2; fanVertex < kFanIndexCapacity; ++fanVertex) {
      const uint32_t base = (fanVertex - 2) * 3;
      indices[base + 0] = 0;
      indices[base + 1] = static_cast<uint16_t>(fanVertex - 1);
      indices[base + 2] = static_cast<uint16_t>(fanVertex);
    }

    BufferDescriptor descriptor;
    descriptor.size = static_cast<uint64_t>(indices.size()) * sizeof(uint16_t);
    descriptor.usage = static_cast<BufferUsages>(BufferUsage::Index);
    descriptor.memoryUsage = MemoryUsage::GpuOnly;
    descriptor.initialData = indices.data();
    descriptor.label = "vulkan/fan-indices";

    VulkanBuffer buffer;
    if (isSuccess(createBufferInternal(descriptor, &buffer))) {
      mFanIndexBuffer = buffer.buffer;
      mFanIndexAllocation = buffer.allocation;
    }
  }
}

void VulkanDevice::destroyFallbackResources() {
  if (mFallbackView != VK_NULL_HANDLE) {
    mApi->vkDestroyImageView(mDevice, mFallbackView, nullptr);
    mFallbackView = VK_NULL_HANDLE;
  }
  if (mFallbackImage != VK_NULL_HANDLE) {
    vmaDestroyImage(mAllocator, mFallbackImage, mFallbackAllocation);
    mFallbackImage = VK_NULL_HANDLE;
    mFallbackAllocation = nullptr;
  }
  if (mFallbackSampler != VK_NULL_HANDLE) {
    mApi->vkDestroySampler(mDevice, mFallbackSampler, nullptr);
    mFallbackSampler = VK_NULL_HANDLE;
  }
  if (mFallbackBuffer != VK_NULL_HANDLE) {
    vmaDestroyBuffer(mAllocator, mFallbackBuffer, mFallbackBufferAllocation);
    mFallbackBuffer = VK_NULL_HANDLE;
    mFallbackBufferAllocation = nullptr;
  }
  if (mFanIndexBuffer != VK_NULL_HANDLE) {
    vmaDestroyBuffer(mAllocator, mFanIndexBuffer, mFanIndexAllocation);
    mFanIndexBuffer = VK_NULL_HANDLE;
    mFanIndexAllocation = nullptr;
  }
}

// ==================================================================================================
// Queues, submission and command buffers
// ==================================================================================================

Queue* VulkanDevice::getQueue(const QueueType type) noexcept {
  if (type == QueueType::Graphics) return &mGraphicsQueueObject;
  // One queue family backs every queue type on this backend; reporting a second object for compute or
  // transfer would suggest an independence the implementation does not have.
  return nullptr;
}

Result VulkanDevice::createCommandBuffer(const CommandBufferDescriptor& descriptor,
        CommandBuffer** outCommandBuffer) {
  if (outCommandBuffer == nullptr) return Result::InvalidArgument;
  *outCommandBuffer = nullptr;
  // Owned until the pool and the command buffer exist; `release` is the single handover, so a failed
  // allocation frees the object that had already been constructed.
  auto commandBuffer = std::make_unique<VulkanCommandBuffer>(*this, descriptor);
  if (const Result initialized = commandBuffer->initialize(); !isSuccess(initialized)) {
    return initialized;
  }
  *outCommandBuffer = commandBuffer.release();
  return Result::Success;
}

void VulkanDevice::destroyCommandBuffer(CommandBuffer* commandBuffer) {
  if (commandBuffer == nullptr) return;
  delete dynamic_cast<VulkanCommandBuffer*>(commandBuffer);
}

Result VulkanDevice::executeCommandBuffer(CommandBuffer& commandBuffer) {
  auto* vulkanCommandBuffer = dynamic_cast<VulkanCommandBuffer*>(&commandBuffer);
  if (vulkanCommandBuffer == nullptr) return Result::InvalidHandle;
  if (!vulkanCommandBuffer->isReady()) {
    emitMessage(MessageSeverity::Error, "executeCommandBuffer was given a command buffer that is not finished recording");
    return Result::Error;
  }
  if (const Result result = vulkanCommandBuffer->submitAndWait(); !isSuccess(result)) {
    return result;
  }
  return Result::Success;
}

Result VulkanDevice::createCommandPool(VkCommandPool* outPool) const {
  if (outPool == nullptr) return Result::InvalidArgument;
  VkCommandPoolCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  info.queueFamilyIndex = mGraphicsQueueFamily;
  return toResult(mApi->vkCreateCommandPool(mDevice, &info, nullptr, outPool));
}

Result VulkanDevice::createDescriptorPool(VkDescriptorPool* outPool) const {
  if (outPool == nullptr) return Result::InvalidArgument;

  // One set per draw is the worst case: the renderer refills a set between draws, and every distinct
  // version needs its own descriptor memory to keep the earlier draw's contents. The counts are sized
  // for a large canvas frame and are grown by the command buffer when a pool runs out.
  const VkDescriptorPoolSize sizes[] = {
    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096 },
    { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024 },
    { VK_DESCRIPTOR_TYPE_SAMPLER, 1024 },
    { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096 },
    { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 512 },
  };
  VkDescriptorPoolCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  info.maxSets = 4096;
  info.poolSizeCount = static_cast<uint32_t>(sizeof(sizes) / sizeof(sizes[0]));
  info.pPoolSizes = sizes;
  return toResult(mApi->vkCreateDescriptorPool(mDevice, &info, nullptr, outPool));
}

Result VulkanDevice::submit(const VkCommandBuffer commandBuffer, const VkFence fence,
        const VkSemaphore waitSemaphore, const VkSemaphore signalSemaphore) {
  VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkSubmitInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  info.commandBufferCount = 1;
  info.pCommandBuffers = &commandBuffer;
  if (waitSemaphore != VK_NULL_HANDLE) {
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &waitSemaphore;
    info.pWaitDstStageMask = &waitStage;
  }
  if (signalSemaphore != VK_NULL_HANDLE) {
    info.signalSemaphoreCount = 1;
    info.pSignalSemaphores = &signalSemaphore;
  }

  std::lock_guard<std::mutex> lock(mQueueMutex);
  return toResult(mApi->vkQueueSubmit(mGraphicsQueue, 1, &info, fence));
}

Result VulkanDevice::submitAndWait(const VkCommandBuffer commandBuffer, const VkFence fence,
        const VkSemaphore waitSemaphore, const VkSemaphore signalSemaphore) {
  // The queue is serialised by `submit`, so the caller's fence is the one thing that must not be shared.
  VkFence local = fence;
  bool owned = false;
  if (local == VK_NULL_HANDLE) {
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (const VkResult created = mApi->vkCreateFence(mDevice, &fenceInfo, nullptr, &local); created != VK_SUCCESS) {
      return toResult(created);
    }
    owned = true;
  }

  if (const Result submitted = submit(commandBuffer, local, waitSemaphore, signalSemaphore);
      !isSuccess(submitted)) {
    if (owned) mApi->vkDestroyFence(mDevice, local, nullptr);
    return submitted;
  }

  const VkResult waited = mApi->vkWaitForFences(mDevice, 1, &local, VK_TRUE, UINT64_MAX);
  if (owned) mApi->vkDestroyFence(mDevice, local, nullptr);
  return toResult(waited);
}

Result VulkanDevice::oneShot(const char* label, const std::function<void(VkCommandBuffer)>& record) {
  std::lock_guard<std::mutex> lock(mOneShotMutex);

  VkCommandPool pool = VK_NULL_HANDLE;
  if (const Result created = createCommandPool(&pool); !isSuccess(created)) return created;

  VkCommandBufferAllocateInfo allocate{};
  allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocate.commandPool = pool;
  allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate.commandBufferCount = 1;
  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  if (const VkResult result = mApi->vkAllocateCommandBuffers(mDevice, &allocate, &commandBuffer);
      result != VK_SUCCESS) {
    mApi->vkDestroyCommandPool(mDevice, pool, nullptr);
    return toResult(result);
  }

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  mApi->vkBeginCommandBuffer(commandBuffer, &begin);

  if (mHasDebugUtils && mApi->vkCmdBeginDebugUtilsLabelEXT != nullptr && label != nullptr) {
    VkDebugUtilsLabelEXT labelInfo{};
    labelInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    labelInfo.pLabelName = label;
    mApi->vkCmdBeginDebugUtilsLabelEXT(commandBuffer, &labelInfo);
  }

  record(commandBuffer);

  if (mHasDebugUtils && mApi->vkCmdEndDebugUtilsLabelEXT != nullptr && label != nullptr) {
    mApi->vkCmdEndDebugUtilsLabelEXT(commandBuffer);
  }
  mApi->vkEndCommandBuffer(commandBuffer);

  const Result submitted = submitAndWait(commandBuffer, VK_NULL_HANDLE);

  mApi->vkFreeCommandBuffers(mDevice, pool, 1, &commandBuffer);
  mApi->vkDestroyCommandPool(mDevice, pool, nullptr);
  return submitted;
}

// ==================================================================================================
// Fences, semaphores and queries
// ==================================================================================================

Result VulkanDevice::createFence(const FenceDescriptor& descriptor, FenceHandle* outFence) {
  if (outFence == nullptr) return Result::InvalidArgument;
  *outFence = FenceHandle::invalid();

  VkFenceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  info.flags = descriptor.flag == FenceFlag::Signalled ? VK_FENCE_CREATE_SIGNALED_BIT : 0;

  VulkanFence fence;
  if (const VkResult result = mApi->vkCreateFence(mDevice, &info, nullptr, &fence.fence); result != VK_SUCCESS) {
    return toResult(result);
  }
  if (descriptor.label != nullptr) fence.label = descriptor.label;
  *outFence = mFences.allocate(std::move(fence));
  return outFence->valid() ? Result::Success : Result::OutOfMemory;
}

void VulkanDevice::destroyFence(const FenceHandle fenceHandle) {
  if (VulkanFence* fence = mFences.get(fenceHandle); fence != nullptr) {
    if (fence->fence != VK_NULL_HANDLE) mApi->vkDestroyFence(mDevice, fence->fence, nullptr);
    fence->fence = VK_NULL_HANDLE;
    mFences.free(fenceHandle);
  }
}

Result VulkanDevice::createSemaphore(const SemaphoreDescriptor& descriptor, SemaphoreHandle* outSemaphore) {
  if (outSemaphore == nullptr) return Result::InvalidArgument;
  *outSemaphore = SemaphoreHandle::invalid();

  VkSemaphoreCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  VulkanSemaphore semaphore;
  if (const VkResult result = mApi->vkCreateSemaphore(mDevice, &info, nullptr, &semaphore.semaphore);
      result != VK_SUCCESS) {
    return toResult(result);
  }
  if (descriptor.label != nullptr) semaphore.label = descriptor.label;
  *outSemaphore = mSemaphores.allocate(std::move(semaphore));
  return outSemaphore->valid() ? Result::Success : Result::OutOfMemory;
}

void VulkanDevice::destroySemaphore(const SemaphoreHandle semaphoreHandle) {
  if (VulkanSemaphore* semaphore = mSemaphores.get(semaphoreHandle); semaphore != nullptr) {
    if (semaphore->semaphore != VK_NULL_HANDLE) mApi->vkDestroySemaphore(mDevice, semaphore->semaphore, nullptr);
    semaphore->semaphore = VK_NULL_HANDLE;
    mSemaphores.free(semaphoreHandle);
  }
}

Result VulkanDevice::getFenceStatus(const FenceHandle fenceHandle) const {
  const VulkanFence* fence = mFences.get(fenceHandle);
  if (fence == nullptr || fence->fence == VK_NULL_HANDLE) return Result::InvalidHandle;
  const VkResult result = mApi->vkGetFenceStatus(mDevice, fence->fence);
  return result == VK_SUCCESS ? Result::Success : toResult(result);
}

Result VulkanDevice::waitForFence(const FenceHandle fenceHandle, const uint64_t timeoutNanoseconds) {
  const VulkanFence* fence = mFences.get(fenceHandle);
  if (fence == nullptr || fence->fence == VK_NULL_HANDLE) return Result::InvalidHandle;
  return toResult(mApi->vkWaitForFences(mDevice, 1, &fence->fence, VK_TRUE, timeoutNanoseconds));
}

Result VulkanDevice::resetFence(const FenceHandle fenceHandle) {
  const VulkanFence* fence = mFences.get(fenceHandle);
  if (fence == nullptr || fence->fence == VK_NULL_HANDLE) return Result::InvalidHandle;
  return toResult(mApi->vkResetFences(mDevice, 1, &fence->fence));
}

Result VulkanDevice::waitIdle() {
  if (mDevice == VK_NULL_HANDLE) return Result::Error;
  const Result waited = toResult(mApi->vkDeviceWaitIdle(mDevice));
  if (isSuccess(waited)) mAsyncSubmissions.store(0, std::memory_order_relaxed);
  return waited;
}

Result VulkanDevice::createQuerySet(const QuerySetDescriptor& descriptor, QuerySetHandle* outQuerySet) {
  if (outQuerySet == nullptr) return Result::InvalidArgument;
  *outQuerySet = QuerySetHandle::invalid();
  if (descriptor.count == 0) return Result::InvalidArgument;

  VkQueryPoolCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  info.queryType = toVkQueryType(descriptor.type);
  info.queryCount = descriptor.count;

  VulkanQuerySet querySet;
  querySet.type = descriptor.type;
  querySet.count = descriptor.count;
  if (const VkResult result = mApi->vkCreateQueryPool(mDevice, &info, nullptr, &querySet.pool);
      result != VK_SUCCESS) {
    return toResult(result);
  }
  if (descriptor.label != nullptr) querySet.label = descriptor.label;
  *outQuerySet = mQuerySets.allocate(std::move(querySet));
  if (!outQuerySet->valid()) return Result::OutOfMemory;

  // Vulkan requires a query pool to be reset before its first use; OpenGL does not, so a caller that
  // creates a set and immediately records a query would otherwise hit an invalid-use error that has no
  // counterpart on the other backend. Resetting here makes the two behave the same.
  const VkQueryPool pool = mQuerySets.get(*outQuerySet)->pool;
  if (const Result reset = oneShot("query-set-reset",
              [this, pool, count = descriptor.count](VkCommandBuffer commandBuffer) {
                mApi->vkCmdResetQueryPool(commandBuffer, pool, 0, count);
              });
      !isSuccess(reset)) {
    destroyQuerySet(*outQuerySet);
    *outQuerySet = QuerySetHandle::invalid();
    return reset;
  }
  return Result::Success;
}

void VulkanDevice::destroyQuerySet(const QuerySetHandle querySetHandle) {
  if (VulkanQuerySet* querySet = mQuerySets.get(querySetHandle); querySet != nullptr) {
    if (querySet->pool != VK_NULL_HANDLE) mApi->vkDestroyQueryPool(mDevice, querySet->pool, nullptr);
    querySet->pool = VK_NULL_HANDLE;
    mQuerySets.free(querySetHandle);
  }
}

Result VulkanDevice::getQuerySetCount(const QuerySetHandle querySetHandle, uint32_t* outCount) const {
  const VulkanQuerySet* querySet = mQuerySets.get(querySetHandle);
  if (querySet == nullptr || outCount == nullptr) return Result::InvalidHandle;
  *outCount = querySet->count;
  return Result::Success;
}

Result VulkanDevice::getQueryResults(const QuerySetHandle querySetHandle, const uint32_t firstQuery,
        const uint32_t count, void* outData, const size_t outDataSize) {
  const VulkanQuerySet* querySet = mQuerySets.get(querySetHandle);
  if (querySet == nullptr || querySet->pool == VK_NULL_HANDLE || outData == nullptr) {
    return Result::InvalidHandle;
  }
  if (firstQuery > querySet->count || count > querySet->count - firstQuery) {
    return Result::InvalidArgument;
  }
  if (outDataSize < static_cast<size_t>(count) * sizeof(uint64_t)) {
    return Result::InvalidArgument;
  }
  if (const VkResult result = mApi->vkGetQueryPoolResults(mDevice, querySet->pool, firstQuery, count, outDataSize,
              outData, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
      result != VK_SUCCESS) {
    return toResult(result);
  }

  // Vulkan timestamps are raw GPU ticks; the RHI reports nanoseconds on every backend, matching
  // OpenGL's timer queries. `timestampPeriod` is the nanoseconds-per-tick conversion the physical
  // device advertises. Occlusion and pipeline-statistics results are counts and stay untouched.
  if (querySet->type == QueryType::Timestamp) {
    const float period = mProperties.limits.timestampPeriod;
    if (!(period > 0.0f)) return Result::Unsupported;
    auto* values = static_cast<uint64_t*>(outData);
    for (uint32_t index = 0; index < count; ++index) {
      values[index] = static_cast<uint64_t>(std::llround(static_cast<double>(values[index]) * period));
    }
  }
  return Result::Success;
}

bool VulkanDevice::isQueryResultAvailable(const QuerySetHandle querySetHandle, const uint32_t query) const {
  const VulkanQuerySet* querySet = mQuerySets.get(querySetHandle);
  if (querySet == nullptr || querySet->pool == VK_NULL_HANDLE) return false;
  uint64_t value = 0;
  return mApi->vkGetQueryPoolResults(mDevice, querySet->pool, query, 1, sizeof(value), &value, sizeof(value),
                 VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
}

// ==================================================================================================
// Buffers, textures and samplers
// ==================================================================================================

Result VulkanDevice::createBufferInternal(const BufferDescriptor& descriptor, VulkanBuffer* outBuffer) {
  if (outBuffer == nullptr) return Result::InvalidArgument;
  if (descriptor.size == 0) return Result::InvalidArgument;
  const ValidationResult validation = validateBufferDescriptor(descriptor, mCapabilities);
  if (!validation.valid) {
    emitMessage(MessageSeverity::Error, validation.message);
    return Result::Unsupported;
  }

  VkBufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.size = descriptor.size;
  info.usage = toVkBufferUsage(descriptor.usage);
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VmaAllocationCreateInfo allocation{};
  allocation.usage = VMA_MEMORY_USAGE_AUTO;
  switch (descriptor.memoryUsage) {
    case MemoryUsage::GpuOnly:
      allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
      break;
    case MemoryUsage::CpuToGpu:
    case MemoryUsage::CpuToGpuDynamic:
      allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                         VMA_ALLOCATION_CREATE_MAPPED_BIT;
      break;
    case MemoryUsage::GpuToCpu:
      allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
      break;
  }

  VulkanBuffer result;
  result.size = descriptor.size;
  result.usage = descriptor.usage;
  result.memoryUsage = descriptor.memoryUsage;
  if (descriptor.label != nullptr) result.label = descriptor.label;

  VmaAllocationInfo allocationInfo{};
  if (const VkResult created =
                  vmaCreateBuffer(mAllocator, &info, &allocation, &result.buffer, &result.allocation, &allocationInfo);
      created != VK_SUCCESS) {
    return toResult(created);
  }
  result.allocationInfo = allocationInfo;

  if (descriptor.initialData != nullptr) {
    // ## Staged rather than mapped
    //
    // A device-local buffer cannot be written from the host at all, and a host-visible one may be
    // uncached write-combined memory. One staging buffer and one command buffer cover both cases and
    // keep one code path, at the cost of a submit during creation.
    VulkanBuffer stagingBuffer;
    BufferDescriptor stagingDescriptor;
    stagingDescriptor.size = descriptor.size;
    stagingDescriptor.usage = static_cast<BufferUsages>(BufferUsage::TransferSrc);
    stagingDescriptor.memoryUsage = MemoryUsage::CpuToGpu;
    stagingDescriptor.label = "vulkan/upload";
    if (const Result staging = createBufferInternal(stagingDescriptor, &stagingBuffer); !isSuccess(staging)) {
      vmaDestroyBuffer(mAllocator, result.buffer, result.allocation);
      return staging;
    }

    void* mapped = nullptr;
    if (vmaMapMemory(mAllocator, stagingBuffer.allocation, &mapped) == VK_SUCCESS && mapped != nullptr) {
      std::memcpy(mapped, descriptor.initialData, static_cast<size_t>(descriptor.size));
      vmaUnmapMemory(mAllocator, stagingBuffer.allocation);
    }

    const Result copied = oneShot("buffer-initial-data", [this, &result, &stagingBuffer, &descriptor](
                                                               VkCommandBuffer commandBuffer) {
      VkBufferCopy copy{};
      copy.srcOffset = 0;
      copy.dstOffset = 0;
      copy.size = descriptor.size;
      mApi->vkCmdCopyBuffer(commandBuffer, stagingBuffer.buffer, result.buffer, 1, &copy);
      VkMemoryBarrier barrier{};
      barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      mApi->vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    });

    vmaDestroyBuffer(mAllocator, stagingBuffer.buffer, stagingBuffer.allocation);
    if (!isSuccess(copied)) {
      vmaDestroyBuffer(mAllocator, result.buffer, result.allocation);
      return copied;
    }
  }

  *outBuffer = std::move(result);
  return Result::Success;
}

Result VulkanDevice::createBuffer(const BufferDescriptor& descriptor, BufferHandle* outBuffer) {
  if (outBuffer == nullptr) return Result::InvalidArgument;
  *outBuffer = BufferHandle::invalid();

  VulkanBuffer buffer;
  if (const Result created = createBufferInternal(descriptor, &buffer); !isSuccess(created)) {
    return created;
  }
  const uint64_t bytes = buffer.allocationInfo.size;
  *outBuffer = mBuffers.allocate(std::move(buffer));
  if (!outBuffer->valid()) return Result::OutOfMemory;
  mResidentBufferBytes += bytes;
  return Result::Success;
}

void VulkanDevice::destroyBuffer(const BufferHandle bufferHandle) {
  if (VulkanBuffer* buffer = mBuffers.get(bufferHandle); buffer != nullptr) {
    if (buffer->mappedPointer != nullptr) {
      vmaUnmapMemory(mAllocator, buffer->allocation);
      buffer->mappedPointer = nullptr;
    }
    mResidentBufferBytes -= std::min(mResidentBufferBytes, buffer->allocationInfo.size);
    if (buffer->buffer != VK_NULL_HANDLE) vmaDestroyBuffer(mAllocator, buffer->buffer, buffer->allocation);
    buffer->buffer = VK_NULL_HANDLE;
    mBuffers.free(bufferHandle);
  }
}

Result VulkanDevice::mapBuffer(const BufferHandle bufferHandle, const uint64_t offset, const uint64_t size,
        void** outPointer) {
  VulkanBuffer* buffer = mBuffers.get(bufferHandle);
  if (buffer == nullptr || outPointer == nullptr) return Result::InvalidHandle;
  if (offset > buffer->size) return Result::InvalidArgument;

  void* mapped = nullptr;
  if (const VkResult result = vmaMapMemory(mAllocator, buffer->allocation, &mapped); result != VK_SUCCESS) {
    return toResult(result);
  }
  // A host-visible allocation is not necessarily coherent: without this the read that follows a copy
  // can see whatever was in the CPU cache before the GPU wrote the buffer.
  vmaInvalidateAllocation(mAllocator, buffer->allocation, 0, VK_WHOLE_SIZE);
  buffer->mappedPointer = mapped;

  const auto* bytes = static_cast<const uint8_t*>(mapped);
  *outPointer = const_cast<uint8_t*>(bytes + offset);
  (void) size;
  return Result::Success;
}

void VulkanDevice::unmapBuffer(const BufferHandle bufferHandle) {
  if (VulkanBuffer* buffer = mBuffers.get(bufferHandle); buffer != nullptr && buffer->mappedPointer != nullptr) {
    vmaFlushAllocation(mAllocator, buffer->allocation, 0, VK_WHOLE_SIZE);
    vmaUnmapMemory(mAllocator, buffer->allocation);
    buffer->mappedPointer = nullptr;
  }
}

Result VulkanDevice::createImageInternal(const TextureDescriptor& descriptor, VulkanTexture* outTexture) {
  if (outTexture == nullptr) return Result::InvalidArgument;

  const VkFormat format = toVkFormat(descriptor.format);
  if (format == VK_FORMAT_UNDEFINED) {
    emitMessage(MessageSeverity::Error,
            "the requested texture format has no Vulkan equivalent; see getPixelFormatInfo");
    return Result::Unsupported;
  }

  VulkanTexture texture;
  texture.format = descriptor.format;
  texture.vkFormat = format;
  texture.type = descriptor.type;
  texture.extent = descriptor.size;
  texture.mipLevels = std::max(descriptor.mipLevelCount, 1u);
  texture.arrayLayers = layerCountFor(descriptor.type, descriptor.size);
  texture.sampleCount = std::max(descriptor.sampleCount, 1u);
  texture.usage = descriptor.usage;
  texture.depth = isDepthFormat(descriptor.format);
  texture.stencil = isStencilFormat(descriptor.format);
  texture.aspect = aspectFor(descriptor.format);
  if (descriptor.label != nullptr) texture.label = descriptor.label;

  VkImageCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = toVkImageType(descriptor.type);
  info.format = format;
  info.extent = { std::max(descriptor.size.width, 1u), std::max(descriptor.size.height, 1u),
    descriptor.type == TextureType::Texture3D ? std::max(descriptor.size.depthOrArrayLayers, 1u) : 1u };
  if (descriptor.type == TextureType::Texture1D) {
    info.extent.height = 1;
    info.extent.depth = 1;
  }
  info.mipLevels = texture.mipLevels;
  info.arrayLayers = std::max(texture.arrayLayers, 1u);
  info.samples = static_cast<VkSampleCountFlagBits>(std::max(texture.sampleCount, 1u));
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = toVkImageUsage(descriptor.usage);
  // `VkImageCreateInfo` forbids TRANSFER_DST on a multisampled image. The shared mapping adds both
  // transfer directions for ordinary textures; a multisampled image keeps TRANSFER_SRC because that is
  // the direction `vkCmdResolveImage` needs, and drops the destination bit.
  if (info.samples != VK_SAMPLE_COUNT_1_BIT) {
    info.usage &= ~VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  }
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  if (descriptor.type == TextureType::TextureCube || descriptor.type == TextureType::TextureCubeArray) {
    info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  }

  VmaAllocationCreateInfo allocation{};
  allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  VmaAllocationInfo allocationInfo{};
  if (const VkResult created =
                  vmaCreateImage(mAllocator, &info, &allocation, &texture.image, &texture.allocation, &allocationInfo);
      created != VK_SUCCESS) {
    std::string message = "the texture could not be allocated (VkResult ";
    message += std::to_string(static_cast<int>(created));
    message += ')';
    emitMessage(MessageSeverity::Error, message);
    return toResult(created);
  }
  texture.allocationInfo = allocationInfo;

  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image = texture.image;
  viewInfo.viewType = toVkImageViewType(
          descriptor.type == TextureType::TextureCube
                  ? TextureViewDimension::Cube
                  : descriptor.type == TextureType::TextureCubeArray
                  ? TextureViewDimension::CubeArray
                  : descriptor.type == TextureType::Texture2DArray
                  ? TextureViewDimension::Texture2DArray
                  : descriptor.type == TextureType::Texture3D ? TextureViewDimension::Texture3D
                  : descriptor.type == TextureType::Texture1D ? TextureViewDimension::Texture1D
                                                              : TextureViewDimension::Texture2D);
  viewInfo.format = format;
  viewInfo.subresourceRange = { texture.aspect, 0, texture.mipLevels, 0, std::max(texture.arrayLayers, 1u) };
  if (const VkResult created = mApi->vkCreateImageView(mDevice, &viewInfo, nullptr, &texture.view);
      created != VK_SUCCESS) {
    vmaDestroyImage(mAllocator, texture.image, texture.allocation);
    return toResult(created);
  }

  // ## The one layout transition every image gets
  //
  // Vulkan starts an image in UNDEFINED, and this backend keeps every image in GENERAL for its whole
  // life. Doing the transition here rather than at first use means a texture is usable as an
  // attachment, a sampled source or a copy destination without the command buffer having to know
  // which use came first - and it is a submission that happens once per texture.
  const Result transitioned = oneShot("texture-init", [this, &texture](VkCommandBuffer commandBuffer) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture.image;
    barrier.subresourceRange = { texture.aspect, 0, texture.mipLevels, 0, std::max(texture.arrayLayers, 1u) };
    mApi->vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  });
  if (!isSuccess(transitioned)) {
    mApi->vkDestroyImageView(mDevice, texture.view, nullptr);
    vmaDestroyImage(mAllocator, texture.image, texture.allocation);
    return transitioned;
  }

  *outTexture = std::move(texture);
  return Result::Success;
}

namespace {

/**
 * The uses an imported texture has in this backend.
 *
 * Sampled by every pass that consumes a video frame, and a copy source for readback and for the blit
 * that moves a frame into a pooled target. It is deliberately *not* a render target: an imported image
 * belongs to its producer, and rendering into it would write over a buffer the decoder is going to reuse.
 * Keeping the set minimal is also what makes the probe below likely to pass, since the producer's own
 * bind flags have to cover every bit requested here.
 */
constexpr VkImageUsageFlags kImportedImageUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

} // namespace anonymous

uint32_t VulkanDevice::findImportedMemoryType(const uint32_t compatibleMemoryTypes,
        const VkMemoryRequirements& imageRequirements) const {
  const uint32_t candidates = compatibleMemoryTypes & imageRequirements.memoryTypeBits;
  uint32_t fallback = VK_MAX_MEMORY_TYPES;
  for (uint32_t index = 0; index < mMemoryProperties.memoryTypeCount; ++index) {
    if ((candidates & (1u << index)) == 0) continue;
    if ((mMemoryProperties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
      return index;
    }
    if (fallback == VK_MAX_MEMORY_TYPES) fallback = index;
  }
  // `VK_MAX_MEMORY_TYPES` is the sentinel the caller checks, not a valid index: returning 0 when the two
  // sets are disjoint would name a memory type the handle cannot be bound to, and the failure would move
  // from "this device cannot import that texture" to a driver error inside vkAllocateMemory.
  return fallback;
}

Result VulkanDevice::createExternalImage(const NativeTextureDescriptor& descriptor, VulkanTexture* outTexture) {
  if (outTexture == nullptr) return Result::InvalidArgument;
  if (descriptor.platformHandle == nullptr) return Result::InvalidArgument;
  if (descriptor.width == 0 || descriptor.height == 0) return Result::InvalidArgument;

  // Only the kinds this platform has an import for are handled. The others are refused before a caller gets
  // here through `supportsNativeTextureImport`; repeating the refusal is what keeps a caller that skipped
  // the query from having a foreign pointer handed to the driver as if it were the handle it expects.
  if (descriptor.kind != NativeTextureKind::Direct3D11Texture &&
      descriptor.kind != NativeTextureKind::AndroidHardwareBuffer) {
    return Result::Unsupported;
  }
#if defined(__ANDROID__)
  if (descriptor.kind == NativeTextureKind::AndroidHardwareBuffer) {
    // A YUV buffer is refused inside unless the descriptor names the conversion; the caller that needs to
    // learn the driver's external format calls `importNativeYuvTexture` instead.
    return createExternalHardwareBufferImage(descriptor, outTexture, nullptr);
  }
  return Result::Unsupported;
#elif !defined(_WIN32)
  return Result::Unsupported;
#else
  if (!mHasExternalMemoryWin32 || mApi->vkGetMemoryWin32HandlePropertiesKHR == nullptr ||
      mApi->vkGetPhysicalDeviceImageFormatProperties2 == nullptr) {
    return Result::Unsupported;
  }

  const VkFormat format = toVkFormat(descriptor.format);
  if (format == VK_FORMAT_UNDEFINED) {
    emitMessage(MessageSeverity::Error, "an imported texture needs a format the backend can name");
    return Result::Unsupported;
  }

  constexpr VkExternalMemoryHandleTypeFlagBits handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
  const VkImageUsageFlags importUsage =
    kImportedImageUsage | (descriptor.writeback ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0);

  // ## 1. Ask the driver whether this format can be imported at all
  //
  // The driver is the only authority on that, and asking before anything is created is what turns "the
  // import failed inside vkAllocateMemory" into "this device does not import RGBA textures", which is an
  // answer a caller can act on by uploading the pixels instead.
  VkPhysicalDeviceExternalImageFormatInfo externalFormat{};
  externalFormat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
  externalFormat.handleType = handleType;

  VkPhysicalDeviceImageFormatInfo2 formatInfo{};
  formatInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
  formatInfo.pNext = &externalFormat;
  formatInfo.format = format;
  formatInfo.type = VK_IMAGE_TYPE_2D;
  formatInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  formatInfo.usage = importUsage;

  VkExternalImageFormatProperties externalProperties{};
  externalProperties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
  VkImageFormatProperties2 imageFormatProperties{};
  imageFormatProperties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
  imageFormatProperties.pNext = &externalProperties;
  if (mApi->vkGetPhysicalDeviceImageFormatProperties2(mPhysicalDevice, &formatInfo, &imageFormatProperties) !=
      VK_SUCCESS) {
    emitMessage(MessageSeverity::Verbose, "the texture format cannot be imported from Direct3D 11 here");
    return Result::Unsupported;
  }
  if ((externalProperties.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0) {
    emitMessage(MessageSeverity::Verbose, "the driver reports the format as not importable");
    return Result::Unsupported;
  }

  // ## 2. The image over the producer's memory
  //
  // `VkExternalMemoryImageCreateInfo` is what tells the driver the image's storage will come from a
  // handle rather than from an allocation of ours. Everything else matches what an imported frame is: a
  // single-level 2D texture, sampled and copyable, never a render target.
  VkExternalMemoryImageCreateInfo externalImage{};
  externalImage.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  externalImage.handleTypes = handleType;

  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.pNext = &externalImage;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = format;
  imageInfo.extent = { descriptor.width, descriptor.height, 1 };
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.usage = importUsage;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  // A writeback import is filled by a copy: beginning in GENERAL lets that copy use the image
  // without a layout transition on a resource whose first use is already a full overwrite.
  imageInfo.initialLayout = descriptor.writeback ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;

  VulkanTexture texture;
  texture.format = descriptor.format;
  texture.vkFormat = format;
  texture.type = TextureType::Texture2D;
  texture.extent = { descriptor.width, descriptor.height, 1 };
  texture.usage = static_cast<TextureUsages>(TextureUsage::Sampled | TextureUsage::External);
  texture.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  texture.label = "imported";

  if (const VkResult created = mApi->vkCreateImage(mDevice, &imageInfo, nullptr, &texture.image);
          created != VK_SUCCESS) {
    std::string message = "the imported image could not be created (VkResult ";
    message += std::to_string(static_cast<int>(created));
    message += ')';
    emitMessage(MessageSeverity::Error, message);
    return toResult(created);
  }

  // ## 3. The memory the handle names
  //
  // `vkGetMemoryWin32HandlePropertiesKHR` is the driver saying which of its memory types this handle can
  // be bound to; it is not a hint. The import structure then hands over the handle itself, and the
  // dedicated structure is required rather than an optimisation: an imported allocation *is* the
  // producer's texture, so it cannot be suballocated into or shared with another resource.
  VkMemoryWin32HandlePropertiesKHR handleProperties{};
  handleProperties.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR;
  if (mApi->vkGetMemoryWin32HandlePropertiesKHR(mDevice, handleType,
              static_cast<HANDLE>(descriptor.platformHandle), &handleProperties) != VK_SUCCESS) {
    mApi->vkDestroyImage(mDevice, texture.image, nullptr);
    texture.image = VK_NULL_HANDLE;
    emitMessage(MessageSeverity::Verbose, "the shared handle is not a resource this driver can import");
    return Result::Unsupported;
  }

  VkMemoryRequirements requirements{};
  mApi->vkGetImageMemoryRequirements(mDevice, texture.image, &requirements);
  const uint32_t memoryType = findImportedMemoryType(handleProperties.memoryTypeBits, requirements);
  if (memoryType >= mMemoryProperties.memoryTypeCount) {
    mApi->vkDestroyImage(mDevice, texture.image, nullptr);
    texture.image = VK_NULL_HANDLE;
    emitMessage(MessageSeverity::Verbose, "the image and the shared handle have no memory type in common");
    return Result::Unsupported;
  }

  VkMemoryDedicatedAllocateInfo dedicated{};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicated.image = texture.image;

  VkImportMemoryWin32HandleInfoKHR import{};
  import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
  import.pNext = &dedicated;
  import.handleType = handleType;
  import.handle = descriptor.platformHandle;

  VkMemoryAllocateInfo allocateInfo{};
  allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocateInfo.pNext = &import;
  allocateInfo.allocationSize = requirements.size;
  allocateInfo.memoryTypeIndex = memoryType;

  if (const VkResult allocated =
                  mApi->vkAllocateMemory(mDevice, &allocateInfo, nullptr, &texture.importedMemory);
          allocated != VK_SUCCESS) {
    texture.importedMemory = VK_NULL_HANDLE;
    mApi->vkDestroyImage(mDevice, texture.image, nullptr);
    texture.image = VK_NULL_HANDLE;
    std::string message = "the shared handle could not be imported (VkResult ";
    message += std::to_string(static_cast<int>(allocated));
    message += ')';
    emitMessage(MessageSeverity::Error, message);
    return toResult(allocated);
  }

  if (const VkResult bound = mApi->vkBindImageMemory(mDevice, texture.image, texture.importedMemory, 0);
          bound != VK_SUCCESS) {
    mApi->vkFreeMemory(mDevice, texture.importedMemory, nullptr);
    texture.importedMemory = VK_NULL_HANDLE;
    mApi->vkDestroyImage(mDevice, texture.image, nullptr);
    texture.image = VK_NULL_HANDLE;
    return toResult(bound);
  }

  const Result completed = completeImportedImage(&texture, std::nullopt);
  if (!isSuccess(completed)) {
    return completed;
  }
  // The helper filled the view and claimed the layout; handing the texture back is this function's job,
  // because the handle table is what the caller's pointer points into.
  *outTexture = std::move(texture);
  return Result::Success;
#endif
}

Result VulkanDevice::completeImportedImage(VulkanTexture* texture,
        const std::optional<YcbcrConversionDescriptor>& ycbcrDescription) {
  if (texture == nullptr || texture->image == VK_NULL_HANDLE) return Result::InvalidArgument;

  // ## The view every consumer samples
  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image = texture->image;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = ycbcrDescription.has_value() ? VK_FORMAT_UNDEFINED : texture->vkFormat;
  viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
  // An image with an external format has no VkFormat: the conversion is what turns its samples into RGB, and
  // the view has to name it or the image cannot be read at all. The external format is repeated on the view
  // too: a conformant driver does not need it there, and at least one (the Maleoon 910) samples the raw
  // planes unless it is present.
#if defined(__ANDROID__)
  VkExternalFormatANDROID viewExternalFormat{};
  viewExternalFormat.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
#endif
  VkSamplerYcbcrConversionInfo ycbcr{};
  if (ycbcrDescription.has_value()) {
    const VkSamplerYcbcrConversion conversion = ycbcrConversionFor(*ycbcrDescription);
    if (conversion == VK_NULL_HANDLE) {
      mApi->vkDestroyImage(mDevice, texture->image, nullptr);
      texture->image = VK_NULL_HANDLE;
      mApi->vkFreeMemory(mDevice, texture->importedMemory, nullptr);
      texture->importedMemory = VK_NULL_HANDLE;
      return Result::Unsupported;
    }
    ycbcr.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    ycbcr.conversion = conversion;
#if defined(__ANDROID__)
    if (ycbcrDescription->externalFormat != 0) {
      viewExternalFormat.externalFormat = ycbcrDescription->externalFormat;
      viewExternalFormat.pNext = &ycbcr;
      viewInfo.pNext = &viewExternalFormat;
    } else
#endif
    {
      viewInfo.pNext = &ycbcr;
    }
  }
  if (const VkResult created = mApi->vkCreateImageView(mDevice, &viewInfo, nullptr, &texture->view);
          created != VK_SUCCESS) {
    mApi->vkFreeMemory(mDevice, texture->importedMemory, nullptr);
    texture->importedMemory = VK_NULL_HANDLE;
    mApi->vkDestroyImage(mDevice, texture->image, nullptr);
    texture->image = VK_NULL_HANDLE;
    return toResult(created);
  }

  // ## Claim the layout the rest of the backend assumes
  //
  // Every texture in this backend is sampled as `GENERAL`, and the command buffer never transitions one
  // because `createImageInternal` did it once at creation. An imported image cannot be declared GENERAL at
  // creation (`initialLayout` may only be UNDEFINED or PREINITIALIZED) and transitioning *from* UNDEFINED
  // is the case where a driver is allowed to discard the contents the producer already wrote. The barrier
  // below therefore keeps the layout unchanged and only carries the ownership of the memory across: the
  // producer's writes happen-before the first sample, and the image is from here on in GENERAL, which is
  // the layout shared/imported memory is presented in.
  const Result transitioned = oneShot("import-init", [this, texture](VkCommandBuffer commandBuffer) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture->image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    mApi->vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  });

  if (!isSuccess(transitioned)) {
    mApi->vkDestroyImageView(mDevice, texture->view, nullptr);
    texture->view = VK_NULL_HANDLE;
    mApi->vkFreeMemory(mDevice, texture->importedMemory, nullptr);
    texture->importedMemory = VK_NULL_HANDLE;
    mApi->vkDestroyImage(mDevice, texture->image, nullptr);
    texture->image = VK_NULL_HANDLE;
    return transitioned;
  }
  return Result::Success;
}

#if defined(__ANDROID__)
Result VulkanDevice::createExternalHardwareBufferImage(const NativeTextureDescriptor& descriptor,
        VulkanTexture* outTexture, YcbcrConversionDescriptor* outConversion) {
  if (outTexture == nullptr || descriptor.platformHandle == nullptr) return Result::InvalidArgument;
  if (descriptor.width == 0 || descriptor.height == 0) return Result::InvalidArgument;
  if (!mHasExternalMemoryAndroidHardwareBuffer || mApi->vkGetAndroidHardwareBufferPropertiesANDROID == nullptr ||
      mApi->vkGetPhysicalDeviceImageFormatProperties2 == nullptr) {
    return Result::Unsupported;
  }
  auto* buffer = static_cast<AHardwareBuffer*>(descriptor.platformHandle);
  constexpr VkExternalMemoryHandleTypeFlagBits handleType =
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

  // ## 1. What the buffer actually holds
  //
  // The driver describes the buffer; the caller only names it. A buffer whose `format` is undefined carries
  // a YUV external format: it is not a colour image at all, cannot be copied or rendered to, and can only be
  // sampled through a `VkSamplerYcbcrConversion`. That is the decoder's NV12 case, and it needs the caller to
  // say which coefficients and range the buffer uses, because the buffer itself does not.
  //
  // `externalFormat` alone does not make a buffer YUV. The spec makes it meaningful only when `format` is
  // `VK_FORMAT_UNDEFINED`, and some drivers set it anyway: the Maleoon 910 reports `externalFormat` equal to
  // the format id of an RGBA buffer it also describes as `VK_FORMAT_R8G8B8A8_UNORM`. Refusing on
  // `externalFormat` alone rejected buffers that the very next line could name, which is how this was found.
  VkAndroidHardwareBufferFormatPropertiesANDROID formatProperties{};
  formatProperties.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
  VkAndroidHardwareBufferPropertiesANDROID properties{};
  properties.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
  properties.pNext = &formatProperties;
  const VkResult described = mApi->vkGetAndroidHardwareBufferPropertiesANDROID(mDevice, buffer, &properties);
  if (described != VK_SUCCESS) {
    emitMessage(MessageSeverity::Error, "the hardware buffer could not be described by the driver");
    return Result::Unsupported;
  }
  {
    // The numbers are the only way to tell why an import was refused: a vendor buffer is described by an
    // opaque external format, and "unsupported" without them says nothing about which one it was. Logged
    // once per format rather than once per frame, because a preview imports one buffer per frame.
    static uint64_t lastLoggedFormat = 0;
    static int lastLoggedVkFormat = -1;
    if (static_cast<uint64_t>(formatProperties.externalFormat) != lastLoggedFormat ||
        static_cast<int>(formatProperties.format) != lastLoggedVkFormat) {
      lastLoggedFormat = formatProperties.externalFormat;
      lastLoggedVkFormat = static_cast<int>(formatProperties.format);
      char detail[160];
      std::snprintf(detail, sizeof(detail), "hardware buffer described: VkFormat %d, external format %llu",
              lastLoggedVkFormat, static_cast<unsigned long long>(lastLoggedFormat));
      emitMessage(MessageSeverity::Info, detail);
    }
  }
  const bool yuv = formatProperties.format == VK_FORMAT_UNDEFINED && formatProperties.externalFormat != 0;
  if (yuv && !descriptor.ycbcr.has_value()) {
    // The coefficients are the caller's to name: a buffer does not say whether it is limited-range BT.601
    // or full-range BT.709, and guessing would produce a plausible picture with the wrong colours.
    emitMessage(MessageSeverity::Verbose,
            "the hardware buffer holds a YUV format, so the import has to say which conversion reads it");
    return Result::Unsupported;
  }
  // The driver's external format is the authoritative one; the descriptor only names the conversion's
  // coefficients, so the resolved description is what the image, the view and every sampler have to use.
  std::optional<YcbcrConversionDescriptor> conversion;
  if (yuv) {
    conversion = descriptor.ycbcr;
    conversion->externalFormat = formatProperties.externalFormat;
    if (outConversion != nullptr) *outConversion = *conversion;
  }
  const VkFormat format =
          formatProperties.format != VK_FORMAT_UNDEFINED ? formatProperties.format : toVkFormat(descriptor.format);
  if (!yuv && format == VK_FORMAT_UNDEFINED) {
    return Result::Unsupported;
  }

  // ## 2. Is this format importable as a sampled image on this device?
  //
  // For a *colour* image the probe below is the right gate: the driver answers which usage flags and memory
  // features the handle type has. For an image with an external format it is not, on at least one driver:
  // the Maleoon 910 answers `VK_ERROR_FORMAT_NOT_SUPPORTED` to the probe for an NV12 buffer it imports
  // without complaint, because the query is defined over `VkFormat`s and an external format is not one.
  // Creating the image is the authority there, and its failure is reported with the driver's own result.
  if (yuv) {
    conversion->externalFormat = formatProperties.externalFormat;
  } else {
    VkPhysicalDeviceExternalImageFormatInfo externalFormat{};
    externalFormat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    externalFormat.handleType = handleType;
    VkPhysicalDeviceImageFormatInfo2 formatInfo{};
    formatInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    formatInfo.pNext = &externalFormat;
    formatInfo.format = format;
    formatInfo.type = VK_IMAGE_TYPE_2D;
    formatInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    formatInfo.usage = kImportedImageUsage;
    VkExternalImageFormatProperties externalProperties{};
    externalProperties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
    VkImageFormatProperties2 imageFormatProperties{};
    imageFormatProperties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    imageFormatProperties.pNext = &externalProperties;
    if (mApi->vkGetPhysicalDeviceImageFormatProperties2(mPhysicalDevice, &formatInfo, &imageFormatProperties) !=
                VK_SUCCESS ||
        (externalProperties.externalMemoryProperties.externalMemoryFeatures &
                VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0) {
      return Result::Unsupported;
    }
  }

  // ## 3. The image, over the buffer's own memory
  //
  // An external-format image is created with `VK_FORMAT_UNDEFINED` and `VkExternalFormatANDROID`, which is
  // what tells the driver the plane layout lives in the buffer rather than in the image.
  VkExternalFormatANDROID externalImageFormat{};
  externalImageFormat.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
  externalImageFormat.externalFormat = yuv ? formatProperties.externalFormat : 0;
  VkExternalMemoryImageCreateInfo externalImage{};
  externalImage.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  externalImage.handleTypes = handleType;
  // VkExternalFormatANDROID is allowed in VkImageCreateInfo's chain and nowhere else here: chaining it to
  // the external-memory struct instead leaves the image without its format, which the driver then samples as if
  // the first plane were R.
  externalImageFormat.pNext = &externalImage;
  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.pNext = yuv ? static_cast<const void*>(&externalImageFormat) : static_cast<const void*>(&externalImage);
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = yuv ? VK_FORMAT_UNDEFINED : format;
  imageInfo.extent = { descriptor.width, descriptor.height, 1 };
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  // A YUV image is sampleable and nothing else: it has no VkFormat a transfer could convert to and from.
  const VkImageUsageFlags importUsage =
          kImportedImageUsage | (descriptor.writeback ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0);
  imageInfo.usage = yuv ? VK_IMAGE_USAGE_SAMPLED_BIT : importUsage;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VulkanTexture texture;
  texture.format = descriptor.format;
  // A YUV image has no VkFormat: the external format and the conversion are what describe it, and the view
  // that reads it is created with the conversion rather than a format.
  texture.vkFormat = yuv ? VK_FORMAT_UNDEFINED : format;
  texture.type = TextureType::Texture2D;
  texture.extent = { descriptor.width, descriptor.height, 1 };
  texture.usage = static_cast<TextureUsages>(TextureUsage::Sampled | TextureUsage::External);
  texture.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  texture.label = "imported";

  if (const VkResult created = mApi->vkCreateImage(mDevice, &imageInfo, nullptr, &texture.image);
          created != VK_SUCCESS) {
    return toResult(created);
  }

  // ## 4. Bind the buffer's memory
  //
  // Unlike the Direct3D case there is no handle to import: the image is bound to the buffer itself. The
  // allocation size comes from the buffer (`properties.allocationSize`), not from the image's requirements,
  // and the allocation is dedicated to this image by definition.
  VkMemoryRequirements requirements{};
  mApi->vkGetImageMemoryRequirements(mDevice, texture.image, &requirements);
  const uint32_t memoryType = findImportedMemoryType(properties.memoryTypeBits, requirements);
  if (memoryType >= mMemoryProperties.memoryTypeCount) {
    mApi->vkDestroyImage(mDevice, texture.image, nullptr);
    texture.image = VK_NULL_HANDLE;
    return Result::Unsupported;
  }

  VkMemoryDedicatedAllocateInfo dedicated{};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicated.image = texture.image;
  VkImportAndroidHardwareBufferInfoANDROID import{};
  import.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
  import.pNext = &dedicated;
  import.buffer = buffer;
  VkMemoryAllocateInfo allocateInfo{};
  allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocateInfo.pNext = &import;
  allocateInfo.allocationSize = properties.allocationSize;
  allocateInfo.memoryTypeIndex = memoryType;

  if (const VkResult allocated =
                  mApi->vkAllocateMemory(mDevice, &allocateInfo, nullptr, &texture.importedMemory);
          allocated != VK_SUCCESS) {
    texture.importedMemory = VK_NULL_HANDLE;
    mApi->vkDestroyImage(mDevice, texture.image, nullptr);
    texture.image = VK_NULL_HANDLE;
    return toResult(allocated);
  }
  if (const VkResult bound = mApi->vkBindImageMemory(mDevice, texture.image, texture.importedMemory, 0);
          bound != VK_SUCCESS) {
    mApi->vkFreeMemory(mDevice, texture.importedMemory, nullptr);
    texture.importedMemory = VK_NULL_HANDLE;
    mApi->vkDestroyImage(mDevice, texture.image, nullptr);
    texture.image = VK_NULL_HANDLE;
    return toResult(bound);
  }

  // The image now reads the buffer's memory, so the buffer has to outlive it: the reference taken here is
  // dropped by `destroyTexture`, which is what keeps a producer that recycles its buffers from freeing the
  // storage under a frame the renderer is still sampling.
  androidHardwareBufferApi().acquire(buffer);
  texture.importedHardwareBuffer = buffer;

  const Result completed = completeImportedImage(&texture, conversion);
  if (!isSuccess(completed)) {
    androidHardwareBufferApi().release(buffer);
    return completed;
  }
  *outTexture = std::move(texture);
  return Result::Success;
}
#endif

Result VulkanDevice::setHdrMetadata(const SwapchainHandle swapchainHandle, const HdrMetadata& metadata) {
  if (mSetHdrMetadata == nullptr) return Result::Unsupported;
  VulkanSwapchain* swapchain = mSwapchains.get(swapchainHandle);
  if (swapchain == nullptr || swapchain->swapchain == VK_NULL_HANDLE) return Result::InvalidHandle;

  // The primaries and white point say what the content's colours mean (BT.2020 / D65); the light-level
  // pair describes the content, and for this stage that is an SDR frame whose white sits at the BT.2408
  // 203 cd/m^2 -- which is exactly what the output shader encodes. The mastering luminance pair names
  // the display the frame would have been graded on, not the panel's peak.
  VkHdrMetadataEXT info{};
  info.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT;
  info.displayPrimaryRed = { metadata.redPrimary[0], metadata.redPrimary[1] };
  info.displayPrimaryGreen = { metadata.greenPrimary[0], metadata.greenPrimary[1] };
  info.displayPrimaryBlue = { metadata.bluePrimary[0], metadata.bluePrimary[1] };
  info.whitePoint = { metadata.whitePoint[0], metadata.whitePoint[1] };
  info.maxLuminance = metadata.maxLuminance;
  info.minLuminance = metadata.minLuminance;
  info.maxContentLightLevel = metadata.maxContentLightLevel;
  info.maxFrameAverageLightLevel = metadata.maxFrameAverageLightLevel;
  const VkSwapchainKHR chains[1] = { swapchain->swapchain };
  const VkHdrMetadataEXT metadataArray[1] = { info };
  mSetHdrMetadata(mDevice, 1, chains, metadataArray);
  return Result::Success;
}

Result VulkanDevice::createTexture(const TextureDescriptor& descriptor, TextureHandle* outTexture) {
  if (outTexture == nullptr) return Result::InvalidArgument;
  *outTexture = TextureHandle::invalid();
  if (!mInitialized) return Result::Error;

  // The validator is shared with the OpenGL backend, and it is what turns an unsupported combination
  // into a named reason rather than a driver error later.
  TextureDescriptor resolved = descriptor;
  const TextureValidationResult validation = validateTextureDescriptor(descriptor, mCapabilities);
  if (!validation.valid) {
    if (validation.resolved.format == PixelFormat::Undefined) {
      emitMessage(MessageSeverity::Error, validation.message);
      return Result::Unsupported;
    }
    resolved = validation.resolved;
  }

  VulkanTexture texture;
  if (const Result created = createImageInternal(resolved, &texture); !isSuccess(created)) {
    return created;
  }
  const uint64_t bytes = texture.allocationInfo.size;
  *outTexture = mTextures.allocate(std::move(texture));
  if (!outTexture->valid()) return Result::OutOfMemory;
  mResidentTextureBytes += bytes;
  return Result::Success;
}

void VulkanDevice::destroyTexture(const TextureHandle textureHandle) {
  VulkanTexture* texture = mTextures.get(textureHandle);
  if (texture == nullptr) return;

  // Views of a destroyed texture must stop resolving before the image they point at is freed; a view
  // that kept a dangling image would be a use-after-free the validation layers would report as a
  // much less specific error at the next draw.
  mTextureViews.forEach([this, textureHandle](TextureViewHandle, VulkanTextureView& view) {
    if (view.texture != textureHandle) return;
    if (view.view != VK_NULL_HANDLE) {
      releaseFramebuffersUsing(view.view);
      mApi->vkDestroyImageView(mDevice, view.view, nullptr);
      view.view = VK_NULL_HANDLE;
    }
    view.image = VK_NULL_HANDLE;
    view.texture = TextureHandle::invalid();
  });

  if (texture->view != VK_NULL_HANDLE) {
    releaseFramebuffersUsing(texture->view);
    mApi->vkDestroyImageView(mDevice, texture->view, nullptr);
  }
  // An imported image's storage belongs to the producer: the allocation was never VMA's, so handing it
  // back to `vmaDestroyImage` would free memory the handle still refers to. The two paths are told apart
  // by `importedMemory`, which only the import sets.
  if (texture->image != VK_NULL_HANDLE && !texture->externalImage) {
    if (texture->importedMemory != VK_NULL_HANDLE) {
      mApi->vkDestroyImage(mDevice, texture->image, nullptr);
      mApi->vkFreeMemory(mDevice, texture->importedMemory, nullptr);
    } else {
      vmaDestroyImage(mAllocator, texture->image, texture->allocation);
    }
  }
  texture->view = VK_NULL_HANDLE;
  texture->image = VK_NULL_HANDLE;
  texture->importedMemory = VK_NULL_HANDLE;
  if (texture->importedHardwareBuffer != nullptr) {
    // The image no longer reads the buffer, so the reference the import took goes back here: the producer
    // is free to reuse the buffer from this point on.
    androidHardwareBufferApi().release(texture->importedHardwareBuffer);
    texture->importedHardwareBuffer = nullptr;
  }
  texture->allocation = nullptr;
  mResidentTextureBytes -= std::min(mResidentTextureBytes, texture->allocationInfo.size);
  mTextures.free(textureHandle);
}

Result VulkanDevice::createTextureView(const TextureViewDescriptor& descriptor, TextureViewHandle* outView) {
  if (outView == nullptr) return Result::InvalidArgument;
  *outView = TextureViewHandle::invalid();

  const VulkanTexture* texture = mTextures.get(descriptor.texture);
  if (texture == nullptr) {
    emitMessage(MessageSeverity::Error, "a texture view was asked for a handle that is not alive");
    return Result::InvalidHandle;
  }

  VulkanTextureView view;
  view.texture = descriptor.texture;
  view.image = texture->image;
  view.viewType = toVkImageViewType(descriptor.dimension);
  view.format = descriptor.format != PixelFormat::Undefined ? descriptor.format : texture->format;
  view.vkFormat = descriptor.format != PixelFormat::Undefined ? toVkFormat(descriptor.format) : texture->vkFormat;
  // An image with an external (YUV) format has no VkFormat at all: its view is created with
  // VK_FORMAT_UNDEFINED plus the conversion, so the check only applies to colour images.
  if (view.vkFormat == VK_FORMAT_UNDEFINED && !descriptor.ycbcr.has_value()) {
    emitMessage(MessageSeverity::Error, "a texture view would be created with an undefined format");
    return Result::Unsupported;
  }
  view.aspect = aspectFor(view.format);
  view.baseMipLevel = descriptor.range.baseMipLevel;
  view.mipLevelCount = descriptor.range.mipLevelCount != 0 ? descriptor.range.mipLevelCount
                                                           : texture->mipLevels - descriptor.range.baseMipLevel;
  view.baseArrayLayer = descriptor.range.baseArrayLayer;
  view.arrayLayerCount = descriptor.range.arrayLayerCount != 0
                           ? descriptor.range.arrayLayerCount
                           : texture->arrayLayers - descriptor.range.baseArrayLayer;
  view.extent = { std::max(texture->extent.width >> view.baseMipLevel, 1u),
    std::max(texture->extent.height >> view.baseMipLevel, 1u) };
  if (descriptor.label != nullptr) view.label = descriptor.label;

  // A cube view needs a multiple of six layers; a 2D view of a layered texture is legal and is how a
  // slice is addressed.
  if ((view.viewType == VK_IMAGE_VIEW_TYPE_CUBE || view.viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) &&
      view.arrayLayerCount % 6 != 0) {
    emitMessage(MessageSeverity::Error, "a cube texture view needs a layer count that is a multiple of six");
    return Result::InvalidArgument;
  }

  VkImageViewCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  info.image = view.image;
  info.viewType = view.viewType;
  info.format = view.vkFormat;
  info.subresourceRange = { view.aspect, view.baseMipLevel, view.mipLevelCount, view.baseArrayLayer,
    view.arrayLayerCount };

  // A view of an image with an external (YUV) format has no single format to name, so the conversion is what
  // describes how it is read; the hardware buffer import fills the optional in for exactly that case.
  VkSamplerYcbcrConversionInfo ycbcr{};
  if (descriptor.ycbcr.has_value()) {
    const VkSamplerYcbcrConversion conversion = ycbcrConversionFor(*descriptor.ycbcr);
    if (conversion == VK_NULL_HANDLE) {
      emitMessage(MessageSeverity::Error, "the view's YUV conversion could not be created");
      return Result::Unsupported;
    }
    ycbcr.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    ycbcr.conversion = conversion;
    info.pNext = &ycbcr;
  }
  if (const VkResult created = mApi->vkCreateImageView(mDevice, &info, nullptr, &view.view);
      created != VK_SUCCESS) {
    return toResult(created);
  }

  *outView = mTextureViews.allocate(std::move(view));
  return outView->valid() ? Result::Success : Result::OutOfMemory;
}

VkSamplerYcbcrConversion VulkanDevice::ycbcrConversionFor(const YcbcrConversionDescriptor& descriptor) {
  for (const auto& [description, conversion] : mYcbcrConversions) {
    if (description == descriptor) {
      return conversion;
    }
  }
  if (mApi->vkCreateSamplerYcbcrConversion == nullptr) {
    // Vulkan 1.0 without `VK_KHR_sampler_ycbcr_conversion`: there is no way to read the format at all.
    emitMessage(MessageSeverity::Verbose, "the driver has no sampler YUV conversion entry point");
    return VK_NULL_HANDLE;
  }

  VkSamplerYcbcrConversionCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
#if defined(__ANDROID__)
  // The only YUV images this backend imports are Android hardware buffers, whose format the driver names
  // with an opaque external format rather than a VkFormat.
  VkExternalFormatANDROID externalFormat{};
  externalFormat.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
  externalFormat.externalFormat = descriptor.externalFormat;
  if (descriptor.externalFormat != 0) {
    info.pNext = &externalFormat;
    info.format = VK_FORMAT_UNDEFINED;
  }
#endif
  if (descriptor.externalFormat == 0) {
    // A conversion over a real pixel format would mean a planar image (a two-plane format, sampled through
    // two planes); this backend does not create those, so there is nothing to convert and the caller's
    // request is answered with "no" rather than with a conversion that reads the wrong channels.
    emitMessage(MessageSeverity::Verbose,
            "a YUV conversion is only implemented for an image with an external format");
    return VK_NULL_HANDLE;
  }
  info.ycbcrModel = toVkYcbcrModel(descriptor.model);
  info.ycbcrRange = toVkYcbcrRange(descriptor.range);
  // Explicit R,G,B,A rather than IDENTITY: both mean the same thing to a conformant driver, and the Maleoon
  // driver applies the conversion only when the mapping is spelled out.
  info.components = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
    VK_COMPONENT_SWIZZLE_A };
  info.xChromaOffset = toVkChromaLocation(descriptor.xChromaOffset);
  info.yChromaOffset = toVkChromaLocation(descriptor.yChromaOffset);
  info.chromaFilter = VK_FILTER_LINEAR;
  info.forceExplicitReconstruction = descriptor.forceExplicitReconstruction ? VK_TRUE : VK_FALSE;

  VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
  if (const VkResult created = mApi->vkCreateSamplerYcbcrConversion(mDevice, &info, nullptr, &conversion);
      created != VK_SUCCESS) {
    std::string message = "the YUV conversion could not be created (VkResult ";
    message += std::to_string(static_cast<int>(created));
    message += ')';
    emitMessage(MessageSeverity::Error, message.c_str());
    return VK_NULL_HANDLE;
  }
  mYcbcrConversions.emplace_back(descriptor, conversion);
  return conversion;
}

void VulkanDevice::destroyTextureView(const TextureViewHandle viewHandle) {
  if (VulkanTextureView* view = mTextureViews.get(viewHandle); view != nullptr) {
    releaseFramebuffersUsing(view->view);
    if (view->view != VK_NULL_HANDLE) mApi->vkDestroyImageView(mDevice, view->view, nullptr);
    view->view = VK_NULL_HANDLE;
    mTextureViews.free(viewHandle);
  }
}

Result VulkanDevice::createSampler(const SamplerDescriptor& descriptor, TextureSamplerHandle* outSampler) {
  if (outSampler == nullptr) return Result::InvalidArgument;
  *outSampler = TextureSamplerHandle::invalid();

  const ValidationResult validation = validateSamplerDescriptor(descriptor, mCapabilities);
  if (!validation.valid) {
    emitMessage(MessageSeverity::Error, validation.message);
    return Result::Unsupported;
  }

  VkSamplerCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  info.magFilter = toVkFilter(descriptor.magFilter);
  info.minFilter = toVkFilter(descriptor.minFilter);
  info.mipmapMode = toVkMipmapMode(descriptor.mipmapFilter);
  info.addressModeU = toVkAddressMode(descriptor.addressModeU);
  info.addressModeV = toVkAddressMode(descriptor.addressModeV);
  info.addressModeW = toVkAddressMode(descriptor.addressModeW);
  info.mipLodBias = 0.0f;
  info.anisotropyEnable = descriptor.maxAnisotropy > 1.0f && mCapabilities.supports(Feature::SamplerAnisotropy)
                            ? VK_TRUE
                            : VK_FALSE;
  info.maxAnisotropy = std::min(descriptor.maxAnisotropy, mCapabilities.maxSamplerAnisotropy);
  info.compareEnable = descriptor.filterType == FilterType::Comparison || descriptor.depthCompare ? VK_TRUE : VK_FALSE;
  info.compareOp = toVkCompareOp(descriptor.compare);
  info.minLod = descriptor.lodMinClamp;
  info.maxLod = descriptor.lodMaxClamp;
  info.borderColor = toVkBorderColor(descriptor.borderColor);
  info.unnormalizedCoordinates = VK_FALSE;

  // A sampler that reads a YUV image names the same conversion its view was created with: Vulkan compares the
  // conversion objects, not their descriptions, so both have to come from `ycbcrConversionFor`.
  VkSamplerYcbcrConversionInfo ycbcr{};
  if (descriptor.ycbcr.has_value()) {
    const VkSamplerYcbcrConversion conversion = ycbcrConversionFor(*descriptor.ycbcr);
    if (conversion == VK_NULL_HANDLE) {
      emitMessage(MessageSeverity::Error, "the sampler's YUV conversion could not be created");
      return Result::Unsupported;
    }
    ycbcr.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    ycbcr.conversion = conversion;
    info.pNext = &ycbcr;
  }

  VulkanSampler sampler;
  sampler.comparison = info.compareEnable == VK_TRUE;
  if (descriptor.label != nullptr) sampler.label = descriptor.label;
  if (const VkResult created = mApi->vkCreateSampler(mDevice, &info, nullptr, &sampler.sampler);
      created != VK_SUCCESS) {
    return toResult(created);
  }
  *outSampler = mSamplers.allocate(std::move(sampler));
  return outSampler->valid() ? Result::Success : Result::OutOfMemory;
}

void VulkanDevice::destroySampler(const TextureSamplerHandle samplerHandle) {
  if (VulkanSampler* sampler = mSamplers.get(samplerHandle); sampler != nullptr) {
    if (sampler->sampler != VK_NULL_HANDLE) mApi->vkDestroySampler(mDevice, sampler->sampler, nullptr);
    sampler->sampler = VK_NULL_HANDLE;
    mSamplers.free(samplerHandle);
  }
}

// ==================================================================================================
// Shader modules
// ==================================================================================================

Result VulkanDevice::buildShaderModule(const ShaderModuleDescriptor& descriptor, VulkanShaderModule& module) {
  if (descriptor.code == nullptr || descriptor.codeSize == 0) {
    emitMessage(MessageSeverity::Error, "the shader module descriptor carries no code");
    return Result::InvalidArgument;
  }

  module = VulkanShaderModule{};
  module.stage = descriptor.stage;
  if (descriptor.label != nullptr) module.label = descriptor.label;
  if (descriptor.entryPoint != nullptr && *descriptor.entryPoint != '\0') {
    module.entryPoint = descriptor.entryPoint;
  }

  std::vector<uint32_t> words;
  switch (descriptor.language) {
    case ShaderModuleDescriptor::Language::SPIRV: {
      if (descriptor.codeSize % sizeof(uint32_t) != 0) {
        emitMessage(MessageSeverity::Error, "the SPIR-V module's size is not a whole number of words");
        return Result::InvalidArgument;
      }
      const auto* source = static_cast<const uint32_t*>(descriptor.code);
      words.assign(source, source + descriptor.codeSize / sizeof(uint32_t));
      module.inputSpirv = words;
      module.sourceInfo.spirvInput = true;
      module.sourceInfo.language = ShaderSourceLanguage::Detect;
      module.sourceInfo.version = 0;
      module.sourceInfo.sourceSize = descriptor.codeSize;
      module.sourceInfo.sourceHash = hashBytes(words.data(), words.size() * sizeof(uint32_t));
      break;
    }
    case ShaderModuleDescriptor::Language::GLSL: {
      std::string source(static_cast<const char*>(descriptor.code), descriptor.codeSize);
      if (!source.empty() && source.back() == '\0') source.pop_back();

      const GlslCompileResult compiled = compileGlslToSpirvVulkan(source, descriptor.stage,
              descriptor.sourceLanguage, descriptor.entryPoint, descriptor.optimizeSource,
              module.label.empty() ? nullptr : module.label.c_str());
      if (!compiled.success()) {
        std::string message = "GLSL source compilation failed";
        if (!module.label.empty()) {
          message += " for ";
          message += module.label;
        }
        message += ": ";
        message += compiled.error;
        emitMessage(MessageSeverity::Error, message);
        return Result::ShaderCompilationFailed;
      }
      module.sourceInfo = compiled.sourceInfo;
      module.compiledSpirv = compiled.spirv;
      module.compiledFromSource = true;
      words = compiled.spirv;
      break;
    }
    case ShaderModuleDescriptor::Language::GLSLDirect:
      emitMessage(MessageSeverity::Error,
              "the Vulkan backend takes SPIR-V only; Language::GLSLDirect is an OpenGL path. "
              "Use Language::GLSL (compiled here) or supply SPIR-V");
      return Result::Unsupported;
  }

  if (words.empty()) {
    emitMessage(MessageSeverity::Error, "the shader module contains no SPIR-V");
    return Result::ShaderCompilationFailed;
  }
  if (words.front() != 0x07230203u) {
    emitMessage(MessageSeverity::Error, "the supplied data is not a SPIR-V module (the magic number is missing)");
    return Result::ShaderCompilationFailed;
  }

  // Reflection before creation, so a module the driver rejects does not leave a half-built record. It
  // is also what the RHI's `getShaderReflection` answers from, and a module with no SPIRV-Cross in the
  // build simply reports an empty table.
  uint32_t pushConstantSize = 0;
  const ShaderReflection reflection = reflectSpirvModule(words.data(), words.size(), &pushConstantSize);
  module.pushConstantSize = pushConstantSize;

  module.bindingNames.reserve(reflection.bindings.size());
  for (const ShaderBindingInfo& binding: reflection.bindings) {
    if (!isDescriptorBinding(binding.type)) continue;
    module.bindingNames.push_back(binding.name);

    ShaderBindingDeclaration declaration;
    declaration.set = binding.set;
    declaration.binding = binding.binding;
    declaration.type = toBindingType(binding.type);
    declaration.arraySize = std::max(binding.arraySize, 1u);
    declaration.location = kNoShaderLocation;
    declaration.name = nullptr; // filled in below, after the name vector is final
    module.bindings.push_back(declaration);
  }
  // The pointers have to be taken *after* the vector reached its final size: a reallocation would leave
  // every `name` dangling, which reads as a garbled binding name rather than as a bug. The two vectors
  // are built in the same order, so index i of one names index i of the other.
  for (size_t index = 0; index < module.bindings.size(); ++index) {
    module.bindings[index].name = module.bindingNames[index].c_str();
  }

  VkShaderModuleCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = words.size() * sizeof(uint32_t);
  info.pCode = words.data();
  if (const VkResult created = mApi->vkCreateShaderModule(mDevice, &info, nullptr, &module.module);
      created != VK_SUCCESS) {
    std::string message = "the shader module was rejected by the driver (VkResult ";
    message += std::to_string(static_cast<int>(created));
    message += ')';
    emitMessage(MessageSeverity::Error, message);
    return toResult(created);
  }

  return Result::Success;
}

Result VulkanDevice::createShaderModule(const ShaderModuleDescriptor& descriptor, ShaderModuleHandle* outModule) {
  if (outModule == nullptr) return Result::InvalidArgument;
  *outModule = ShaderModuleHandle::invalid();
  VulkanShaderModule module;
  const Result built = buildShaderModule(descriptor, module);
  if (!isSuccess(built)) return built;
  *outModule = mShaderModules.allocate(std::move(module));
  return outModule->valid() ? Result::Success : Result::OutOfMemory;
}

void VulkanDevice::destroyShaderModule(const ShaderModuleHandle moduleHandle) {
  if (VulkanShaderModule* module = mShaderModules.get(moduleHandle); module != nullptr) {
    if (module->module != VK_NULL_HANDLE) mApi->vkDestroyShaderModule(mDevice, module->module, nullptr);
    module->module = VK_NULL_HANDLE;
    mShaderModules.free(moduleHandle);
  }
}

Result VulkanDevice::getShaderReflection(const ShaderModuleHandle moduleHandle,
        std::vector<ShaderBindingDeclaration>* outBindings) {
  if (outBindings == nullptr) return Result::InvalidArgument;
  const VulkanShaderModule* module = mShaderModules.get(moduleHandle);
  if (module == nullptr) return Result::InvalidHandle;
  *outBindings = module->bindings;
  return Result::Success;
}

Result VulkanDevice::getShaderSourceInfo(const ShaderModuleHandle moduleHandle, ShaderSourceInfo* outInfo) {
  if (outInfo == nullptr) return Result::InvalidArgument;
  const VulkanShaderModule* module = mShaderModules.get(moduleHandle);
  if (module == nullptr) return Result::InvalidHandle;
  if (!module->compiledFromSource && module->inputSpirv.empty()) return Result::Unsupported;
  *outInfo = module->sourceInfo;
  return Result::Success;
}

Result VulkanDevice::getShaderSpirv(const ShaderModuleHandle moduleHandle, std::vector<uint32_t>* outWords) {
  if (outWords == nullptr) return Result::InvalidArgument;
  const VulkanShaderModule* module = mShaderModules.get(moduleHandle);
  if (module == nullptr) return Result::InvalidHandle;
  if (module->compiledFromSource) {
    *outWords = module->compiledSpirv;
  } else if (!module->inputSpirv.empty()) {
    *outWords = module->inputSpirv;
  } else {
    return Result::Unsupported;
  }
  return Result::Success;
}

// ==================================================================================================
// Diagnostics
// ==================================================================================================

void VulkanDevice::setDebugMessageCallback(const DebugMessageCallback callback, void* userData) {
  mDebugCallback.store(callback, std::memory_order_release);
  mDebugUserData.store(userData, std::memory_order_release);
}

void VulkanDevice::emitMessage(const MessageSeverity severity, const char* message) const {
  if (message == nullptr) return;
  const DebugMessageCallback callback = mDebugCallback.load(std::memory_order_acquire);
  if (callback != nullptr) {
    callback(severity, message, mDebugUserData.load(std::memory_order_acquire));
    return;
  }
  switch (severity) {
    case MessageSeverity::Verbose:
    case MessageSeverity::Info:
      MINILOG_INFO("[backend/vulkan] {}", message);
      break;
    case MessageSeverity::Warning:
      MINILOG_WARN("[backend/vulkan] {}", message);
      break;
    case MessageSeverity::Error:
    case MessageSeverity::Fatal:
      MINILOG_ERROR("[backend/vulkan] {}", message);
      break;
  }
}

void VulkanDevice::appendDiagnostics(std::string& out) const {
  out += "RHI backend: Vulkan (";
  out += platformWindowSystemName(mNativeWindowSystem);
  out += ")\n  device:        ";
  out += mInfo.deviceName != nullptr ? mInfo.deviceName : "";
  out += "\n  vendor:        ";
  out += mInfo.vendor != nullptr ? mInfo.vendor : "";
  out += "\n  API version:   ";
  out += mInfo.apiVersion != nullptr ? mInfo.apiVersion : "";
  out += "\n  driver:        ";
  out += mInfo.driverVersion != nullptr ? mInfo.driverVersion : "";
  out += "\n  device type:   ";
  out += deviceTypeName(mProperties.deviceType);
  out += "\n  dynamic rendering: ";
  out += mDynamicRenderingCore ? "core (Vulkan 1.3)"
                               : mDynamicRenderingExtension ? "VK_KHR_dynamic_rendering" : "no (render-pass fallback)";
  out += "\n  queues:        graphics family " + std::to_string(mGraphicsQueueFamily) + ", present family " +
         std::to_string(mPresentQueueFamily);
  out += "\n  swapchain:     ";
  out += mHasSwapchainExtension ? "VK_KHR_swapchain" : "no";
  out += "\n  external text: ";
  if (mHasExternalMemoryWin32) {
    out += "Direct3D 11 texture import (VK_KHR_external_memory_win32)";
  } else if (mHasExternalMemoryAndroidHardwareBuffer) {
    out += "AHardwareBuffer import (VK_ANDROID_external_memory_android_hardware_buffer)";
  } else {
    out += "no";
  }
  out += "\n  SPIR-V input:  yes";
  out += "\n  GLSL input:    ";
  out += isGlslCompilerAvailable() ? "yes (compiled to Vulkan SPIR-V at load)" : "no (shaderc absent)";
  out += "\n  validation:    ";
  out += mDebugMessenger != VK_NULL_HANDLE ? "yes" : "no";
  out += "\n  loader:        ";
  out += loaderLibraryName();
  out += "\n\nDevice extensions (";
  out += std::to_string(mDeviceExtensions.size());
  out += "):\n";
  for (const std::string& extension: mDeviceExtensions) {
    out += "  ";
    out += extension;
    out += '\n';
  }
  out += '\n';
  mCapabilities.appendReport(out);

  out += "\n  Live resources: textures=";
  out += std::to_string(mTextures.liveCount());
  out += " buffers=";
  out += std::to_string(mBuffers.liveCount());
  out += " renderPipelines=";
  out += std::to_string(mRenderPipelines.liveCount());
  out += " shaderModules=";
  out += std::to_string(mShaderModules.liveCount());
  out += " surfaces=";
  out += std::to_string(mSurfaces.liveCount());
  out += " swapchains=";
  out += std::to_string(mSwapchains.liveCount());
  out += '\n';
}

} // namespace backend::vulkan


