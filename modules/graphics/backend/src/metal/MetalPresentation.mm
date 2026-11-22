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

#include "private/backend/metal/MetalDevice.h"

#include <TargetConditionals.h>

#include <algorithm>
#include <string>

namespace backend::metal {
namespace {

/**
 * Formats a CAMetalLayer can present.
 *
 * The RHI asks for `R8G8B8A8_UNORM` because that is what the canvas renders in, but a Metal drawable is a
 * WindowServer-owned surface and its format is the layer's: `BGRA8Unorm` on every Apple platform. Substituting it is
 * not a quality loss -- the bytes are the same four channels in a different order -- and the RHI is explicit that a
 * requested swapchain format is a *preference*: `SwapchainInfo::format` is what the caller builds its window pipeline
 * against.
 */
constexpr PixelFormat kPresentFormat = PixelFormat::B8G8R8A8_UNORM;

} // namespace anonymous

// ==================================================================================================
// Surface
// ==================================================================================================

Result MetalDevice::createSurface(const SurfaceDescriptor& descriptor, SurfaceHandle* outSurface) {
  if (outSurface == nullptr) return Result::InvalidArgument;
  *outSurface = SurfaceHandle::invalid();

  if (descriptor.nativeWindow == nullptr) {
    emitMessage(MessageSeverity::Error,
            "a Metal surface needs a CAMetalLayer* in SurfaceDescriptor::nativeWindow; a headless device creates no "
            "surface at all");
    return Result::InvalidArgument;
  }
  if (descriptor.nativeWindowSystem != NativeWindowSystem::Unknown &&
      descriptor.nativeWindowSystem != NativeWindowSystem::Metal) {
    emitMessage(MessageSeverity::Error, "the window system named in the surface descriptor is not Metal");
    return Result::InvalidArgument;
  }

  CAMetalLayer* layer = (__bridge CAMetalLayer*) descriptor.nativeWindow;
  if (![layer isKindOfClass:[CAMetalLayer class]]) {
    emitMessage(MessageSeverity::Error,
            "the Metal backend's SurfaceDescriptor::nativeWindow must be a CAMetalLayer, not some other object");
    return Result::InvalidArgument;
  }
  if (layer.device == nil || layer.device != mDevice) {
    // A layer created by another device would present images this device did not render. Adopting it is a driver-level
    // failure with no useful message, so it is refused here where the reason is nameable.
    if (layer.device == nil) {
      layer.device = mDevice;
    } else {
      emitMessage(MessageSeverity::Error,
              "the CAMetalLayer was created for a different MTLDevice; presentation would be undefined");
      return Result::InvalidArgument;
    }
  }

  MetalSurface surface;
  surface.descriptor = descriptor;
  // A strong reference is deliberate: the RHI's surface record has to outlive the window's own lifetime assumption,
  // and the layer is the only object that can present. ARC releases it when the pool slot is freed.
  surface.layer = layer;

  *outSurface = mSurfaces.allocate(std::move(surface));
  return outSurface->valid() ? Result::Success : Result::OutOfMemory;
}

void MetalDevice::destroySurface(const SurfaceHandle surfaceHandle) {
  if (MetalSurface* surface = mSurfaces.get(surfaceHandle); surface != nullptr) {
    surface->layer = nil;
    mSurfaces.free(surfaceHandle);
  }
}

// ==================================================================================================
// Swapchain
// ==================================================================================================

Result MetalDevice::createSwapchain(const SwapchainDescriptor& descriptor, SwapchainHandle* outSwapchain) {
  if (outSwapchain == nullptr) return Result::InvalidArgument;
  *outSwapchain = SwapchainHandle::invalid();

  MetalSurface* surface = mSurfaces.get(descriptor.surface);
  if (surface == nullptr || surface->layer == nil) {
    emitMessage(MessageSeverity::Error, "the swapchain names a surface that is not alive");
    return Result::InvalidHandle;
  }

  MetalSwapchain swapchain;
  swapchain.surface = descriptor.surface;
  swapchain.width = descriptor.width != 0 ? descriptor.width
                                          : static_cast<uint32_t>(surface->layer.drawableSize.width);
  swapchain.height = descriptor.height != 0 ? descriptor.height
                                            : static_cast<uint32_t>(surface->layer.drawableSize.height);
  if (swapchain.width == 0 || swapchain.height == 0) {
    emitMessage(MessageSeverity::Error, "a swapchain needs a non-zero width and height");
    return Result::InvalidArgument;
  }

  // The drawable's format is the layer's. See `kPresentFormat`.
  swapchain.format = kPresentFormat;
  if (mIos && descriptor.presentMode != PresentMode::Fifo) {
    // iOS has no swap-interval or display-sync control: the compositor presents the drawable at the display's refresh
    // rate. Reporting the granted mode honestly is what lets a caller that wanted Immediate know it did not get it.
    swapchain.presentMode = PresentMode::Fifo;
  }
  swapchain.mtlFormat = toMtlPixelFormat(swapchain.format);
  swapchain.srgb = false;
  swapchain.presentMode = descriptor.presentMode;
  swapchain.imageCount = descriptor.imageCount != 0 ? std::max(2u, descriptor.imageCount)
                                                    : (descriptor.presentMode == PresentMode::Mailbox ? 3u : 2u);

  CAMetalLayer* layer = surface->layer;
  layer.device = mDevice;
  layer.pixelFormat = swapchain.mtlFormat;
  // The canvas reads a frame back for `--snapshot` and the effect graph copies into decoder/encoder textures; a
  // framebuffer-only layer forbids both, and the flag costs nothing on Apple GPUs.
  layer.framebufferOnly = NO;
  layer.drawableSize = CGSizeMake(swapchain.width, swapchain.height);
  layer.maximumDrawableCount = swapchain.imageCount;
#if !TARGET_OS_IPHONE
  // `displaySyncEnabled` is a macOS-only property. On iOS a CAMetalLayer presents in step with the display and offers
  // no per-layer control, so the requested mode is granted as Fifo below rather than pretended.
  layer.displaySyncEnabled = descriptor.presentMode != PresentMode::Immediate;
#endif
  layer.allowsNextDrawableTimeout = YES;
  layer.opaque = YES;
  if (descriptor.label != nullptr) layer.name = [NSString stringWithUTF8String:descriptor.label];

  // The RHI's colour-space request. CAMetalLayer can present a PQ/EDR surface, but the presentation pass that would
  // encode PQ is the graphics layer's (`SurfaceConfiguration::pqOutput`) and no Metal path drives it yet; reporting the
  // SDR grant is what stops the caller encoding PQ numbers into an sRGB-presented surface.
  if (descriptor.colorSpace != SwapchainColorSpace::Srgb) {
    emitMessage(MessageSeverity::Warning,
            "this Metal backend presents SDR sRGB; the requested HDR colour space is not implemented and the swapchain "
            "was granted Srgb");
  }

  *outSwapchain = mSwapchains.allocate(std::move(swapchain));
  return outSwapchain->valid() ? Result::Success : Result::OutOfMemory;
}

void MetalDevice::destroySwapchain(const SwapchainHandle swapchainHandle) {
  MetalSwapchain* swapchain = mSwapchains.get(swapchainHandle);
  if (swapchain == nullptr) return;
  if (mCurrentFrameSwapchain == swapchain) mCurrentFrameSwapchain = nullptr;
  // An acquired but unpresented drawable is simply dropped: ARC releases it, which returns it to the layer's pool.
  swapchain->drawable = nil;
  swapchain->frameAcquired = false;
  mSwapchains.free(swapchainHandle);
}

Result MetalDevice::getSwapchainInfo(const SwapchainHandle swapchainHandle, SwapchainInfo* outInfo) {
  if (outInfo == nullptr) return Result::InvalidArgument;
  const MetalSwapchain* swapchain = mSwapchains.get(swapchainHandle);
  if (swapchain == nullptr) return Result::InvalidHandle;

  SwapchainInfo info;
  info.width = swapchain->width;
  info.height = swapchain->height;
  info.format = swapchain->format;
  info.imageCount = swapchain->imageCount;
  info.presentMode = swapchain->presentMode;
  info.currentImageIndex = 0;
  // The acquired image is not a pool texture: it is a drawable that only exists for this frame, so a caller renders
  // into it through `defaultFramebufferView()` rather than through `currentImage`.
  info.currentImage = TextureHandle::invalid();
  info.currentRenderPass = RenderPassHandle::invalid();
  info.srgb = swapchain->srgb;
  info.colorSpace = SwapchainColorSpace::Srgb;
  *outInfo = info;
  return Result::Success;
}

Result MetalDevice::beginFrame(const SwapchainHandle swapchainHandle, SwapchainInfo* outInfo,
        const SemaphoreHandle signalSemaphore) {
  MetalSwapchain* swapchain = mSwapchains.get(swapchainHandle);
  if (swapchain == nullptr) return Result::InvalidHandle;
  if (swapchain->frameAcquired) {
    emitMessage(MessageSeverity::Error, "beginFrame was called while a frame is already acquired");
    return Result::InvalidArgument;
  }
  MetalSurface* surface = mSurfaces.get(swapchain->surface);
  if (surface == nullptr || surface->layer == nil) return Result::InvalidHandle;

  id<CAMetalDrawable> drawable = [surface->layer nextDrawable];
  if (drawable == nil) {
    // A nil drawable means the layer has no image to hand over: the window is gone, occluded for long enough that the
    // pool timed out, or the surface was resized out from under it. That is `OutOfDate`, and the caller's contract is
    // to rebuild the swapchain.
    swapchain->outOfDate = true;
    return Result::OutOfDate;
  }

  swapchain->drawable = drawable;
  swapchain->frameAcquired = true;
  swapchain->outOfDate = false;
  // The drawable's own size is authoritative: a layer whose drawableSize was changed by the view's `layout` between
  // frames reports the new size here, which is what lets `WindowSurface` follow a live resize.
  swapchain->width = static_cast<uint32_t>(drawable.texture.width);
  swapchain->height = static_cast<uint32_t>(drawable.texture.height);
  mCurrentFrameSwapchain = swapchain;

  if (signalSemaphore.valid()) {
    // Acquisition is complete when `nextDrawable` returns, so the acquire semaphore is signalled on the host rather
    // than by a GPU command. A shared event's value is visible to both, so a submission that waits on it proceeds.
    if (MetalSemaphore* semaphore = mSemaphores.get(signalSemaphore); semaphore != nullptr) {
      semaphore->value += 1;
      semaphore->event.signaledValue = semaphore->value;
    }
  }

  if (outInfo != nullptr) {
    SwapchainInfo info;
    info.width = swapchain->width;
    info.height = swapchain->height;
    info.format = swapchain->format;
    info.imageCount = swapchain->imageCount;
    info.presentMode = swapchain->presentMode;
    info.currentImageIndex = 0;
    info.currentImage = TextureHandle::invalid();
    info.currentRenderPass = RenderPassHandle::invalid();
    info.srgb = swapchain->srgb;
    info.colorSpace = SwapchainColorSpace::Srgb;
    *outInfo = info;
  }
  return Result::Success;
}

Result MetalDevice::tryBeginFrame(const SwapchainHandle swapchain, SwapchainInfo* outInfo) {
  // CAMetalLayer has no non-blocking acquire: `nextDrawable` waits for the pool and a nil result is a timeout, not
  // "try again next frame". Reporting the blocking behaviour rather than inventing a different contract is what the
  // RHI's default does, and this override exists to say why there is nothing better here.
  return beginFrame(swapchain, outInfo, SemaphoreHandle::invalid());
}

Result MetalDevice::endFrame(const SwapchainHandle swapchainHandle) {
  return endFrame(swapchainHandle, SemaphoreHandle::invalid());
}

Result MetalDevice::endFrame(const SwapchainHandle swapchainHandle, const SemaphoreHandle waitSemaphore) {
  MetalSwapchain* swapchain = mSwapchains.get(swapchainHandle);
  if (swapchain == nullptr) return Result::InvalidHandle;
  if (!swapchain->frameAcquired || swapchain->drawable == nil) {
    emitMessage(MessageSeverity::Error, "endFrame was called without an acquired frame");
    return Result::InvalidArgument;
  }

  MetalSemaphore* semaphore = waitSemaphore.valid() ? mSemaphores.get(waitSemaphore) : nullptr;
  if (waitSemaphore.valid() && semaphore == nullptr) return Result::InvalidHandle;

  id<MTLCommandBuffer> presentBuffer = [mQueue commandBuffer];
  if (presentBuffer == nil) return Result::Error;
  presentBuffer.label = @"coral/metal/present";

  // The present is a command buffer of its own, so a render submission and its presentation need not be separated by a
  // device-wide wait -- the same shape as Vulkan's `vkQueuePresentKHR` with a wait semaphore. Commands on one queue
  // execute in commit order, so the drawable's rendering is already ordered first.
  if (semaphore != nullptr) {
    [presentBuffer encodeWaitForEvent:semaphore->event value:semaphore->value];
  }
  [presentBuffer presentDrawable:swapchain->drawable];
  const uint64_t completion = nextCompletionValue();
  [presentBuffer encodeSignalEvent:mCompletionEvent value:completion];
  [presentBuffer commit];

  swapchain->drawable = nil;
  swapchain->frameAcquired = false;
  if (mCurrentFrameSwapchain == swapchain) mCurrentFrameSwapchain = nullptr;
  return Result::Success;
}

Result MetalDevice::resizeSwapchain(const SwapchainHandle swapchainHandle, const uint32_t width,
        const uint32_t height) {
  MetalSwapchain* swapchain = mSwapchains.get(swapchainHandle);
  if (swapchain == nullptr) return Result::InvalidHandle;
  if (width == 0 || height == 0) return Result::InvalidArgument;

  MetalSurface* surface = mSurfaces.get(swapchain->surface);
  if (surface != nullptr && surface->layer != nil) {
    // Resizing only sets the layer's drawable size; the images themselves belong to the layer's pool and are recycled
    // by the next `nextDrawable`. That is the whole of a Metal swapchain rebuild.
    surface->layer.drawableSize = CGSizeMake(width, height);
  }
  swapchain->width = width;
  swapchain->height = height;
  swapchain->outOfDate = false;
  return Result::Success;
}

} // namespace backend::metal
