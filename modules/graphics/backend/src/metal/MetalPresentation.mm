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

#import <CoreGraphics/CoreGraphics.h>

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

  // ## The colour space chooses the drawable format
  //
  // `Srgb` and `DisplayP3` are 8-bit BGRA drawables. EDR is RGBA16Float so that values above 1.0
  // survive, and HDR10 is a 10-bit RGB drawable. `CAMetalLayer` takes the format and the colour space
  // separately; the presentation pass in the graphics layer writes the encoding the granted space
  // expects, so this side only has to configure the surface honestly.
  switch (descriptor.colorSpace) {
    case SwapchainColorSpace::Srgb:
      swapchain.format = kPresentFormat;
      swapchain.colorSpace = SwapchainColorSpace::Srgb;
      break;
    case SwapchainColorSpace::DisplayP3:
      swapchain.format = kPresentFormat;
      swapchain.colorSpace = SwapchainColorSpace::DisplayP3;
      break;
    case SwapchainColorSpace::ExtendedLinearP3:
      swapchain.format = PixelFormat::R16G16B16A16_SFLOAT;
      swapchain.colorSpace = SwapchainColorSpace::ExtendedLinearP3;
      break;
    case SwapchainColorSpace::Hdr10Pq:
      swapchain.format = PixelFormat::R10G10B10A2_UNORM;
      swapchain.colorSpace = SwapchainColorSpace::Hdr10Pq;
      break;
  }
  if (mIos) {
    // iOS has no swap-interval or display-sync control: the compositor presents the drawable at the display's refresh
    // rate. Reporting the granted mode honestly is what lets a caller that wanted Immediate know it did not get it.
    swapchain.presentMode = PresentMode::Fifo;
  } else {
    swapchain.presentMode = descriptor.presentMode;
  }
  swapchain.mtlFormat = toMtlPixelFormat(swapchain.format);
  swapchain.srgb = false;
  swapchain.imageCount = descriptor.imageCount != 0 ? std::max(2u, descriptor.imageCount)
                                                    : (descriptor.presentMode == PresentMode::Mailbox ? 3u : 2u);

  CAMetalLayer* layer = surface->layer;
  layer.device = mDevice;
  layer.pixelFormat = swapchain.mtlFormat;
  // The canvas reads a frame back for `--snapshot` and the effect graph copies into decoder/encoder textures; a
  // framebuffer-only layer forbids both, and the flag costs nothing on Apple GPUs.
  layer.framebufferOnly = NO;
  layer.drawableSize = CGSizeMake(swapchain.width, swapchain.height);
  // `maximumDrawableCount` is deliberately not set: CAMetalLayer refuses it outright once a
  // CAMetalDisplayLink owns the layer's drawable pool ("should not be called when using
  // CAMetalDisplayLink"), and the default pool of three is the number the RHI already asks for in
  // mailbox mode. The requested image count is still reported in `SwapchainInfo` for callers that
  // size their own in-flight budget from it.
#if !TARGET_OS_IPHONE
  // `displaySyncEnabled` is a macOS-only property. On iOS a CAMetalLayer presents in step with the display and offers
  // no per-layer control, so the requested mode is granted as Fifo below rather than pretended.
  layer.displaySyncEnabled = descriptor.presentMode != PresentMode::Immediate;
#endif
  layer.allowsNextDrawableTimeout = YES;
  layer.opaque = YES;
  if (descriptor.label != nullptr) layer.name = [NSString stringWithUTF8String:descriptor.label];

  // The colour space object is what tells the window server how to interpret the drawable's numbers.
  // `ExtendedLinearP3` additionally asks for EDR; the others leave the flag off, because an SDR
  // surface with EDR enabled would let the compositor apply a display transform the content never
  // described.
  CGColorSpaceRef colorSpace = nullptr;
  bool extendedRange = false;
  switch (swapchain.colorSpace) {
    case SwapchainColorSpace::Srgb:
      colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
      break;
    case SwapchainColorSpace::DisplayP3:
      colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceDisplayP3);
      break;
    case SwapchainColorSpace::ExtendedLinearP3:
      colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearDisplayP3);
      extendedRange = true;
      break;
    case SwapchainColorSpace::Hdr10Pq:
      colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceITUR_2100_PQ);
      extendedRange = true;
      break;
  }
  if (colorSpace == nullptr) {
    // A system without the requested space falls back to SDR rather than presenting numbers the
    // compositor would misread. The caller sees the granted space and the warning.
    emitMessage(MessageSeverity::Warning,
            "the requested Metal colour space is unavailable on this system; the swapchain was granted Srgb");
    swapchain.format = kPresentFormat;
    swapchain.mtlFormat = toMtlPixelFormat(swapchain.format);
    swapchain.colorSpace = SwapchainColorSpace::Srgb;
    colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    extendedRange = false;
    layer.pixelFormat = swapchain.mtlFormat;
  }
  if (colorSpace != nullptr) {
    layer.colorspace = colorSpace;
    CGColorSpaceRelease(colorSpace);
  }
  layer.wantsExtendedDynamicRangeContent = extendedRange;

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
  info.colorSpace = swapchain->colorSpace;
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
    info.colorSpace = swapchain->colorSpace;
    *outInfo = info;
  }
  return Result::Success;
}

Result MetalDevice::beginFrameWithDrawable(const SwapchainHandle swapchainHandle, void* nativeDrawable,
        SwapchainInfo* outInfo) {
  MetalSwapchain* swapchain = mSwapchains.get(swapchainHandle);
  if (swapchain == nullptr) return Result::InvalidHandle;
  if (swapchain->frameAcquired) {
    emitMessage(MessageSeverity::Error, "beginFrameWithDrawable was called while a frame is already acquired");
    return Result::InvalidArgument;
  }
  if (nativeDrawable == nullptr) {
    emitMessage(MessageSeverity::Error, "beginFrameWithDrawable needs an id<CAMetalDrawable>");
    return Result::InvalidArgument;
  }
  id<CAMetalDrawable> drawable = (__bridge id<CAMetalDrawable>)nativeDrawable;
  if (drawable == nil || drawable.texture == nil) {
    emitMessage(MessageSeverity::Error, "the display link's drawable has no texture");
    return Result::InvalidArgument;
  }
  if (drawable.texture.pixelFormat != swapchain->mtlFormat) {
    // The drawable comes from the same layer, so this should agree; a mismatch means the layer was
    // reconfigured behind the swapchain's back. Refusing is better than rendering into a format the
    // window pipelines were not built for.
    emitMessage(MessageSeverity::Error,
            "the display link's drawable format does not match the swapchain's; the layer was reconfigured");
    return Result::InvalidArgument;
  }

  swapchain->drawable = drawable;
  swapchain->frameAcquired = true;
  swapchain->outOfDate = false;
  swapchain->width = static_cast<uint32_t>(drawable.texture.width);
  swapchain->height = static_cast<uint32_t>(drawable.texture.height);
  mCurrentFrameSwapchain = swapchain;

  // There is no acquire command to wait on: the display link already owns the drawable. The caller's
  // render submission therefore has no acquire semaphore, which is why this overload takes none.
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
    info.colorSpace = swapchain->colorSpace;
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
