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
 * @file AppleIOSurfaceOutputNode.cpp
 *
 * The Apple encoder/compositor output: the graph's final frame lands in an IOSurface the caller owns. Metal
 * imports the surface as a writeback `id<MTLTexture>`; OpenGL renders into a `GL_TEXTURE_2D` the GL bridge
 * owns. On macOS the bridge pushes that image into the surface with one GPU blit after the submission; on
 * iOS the 2D texture is the surface, so there is nothing to push. The CPU never touches the pixels.
 */

#include <effect/platform/apple/AppleIOSurfaceOutputNode.h>

#include "AppleIOSurfaceGL.h"
#include "effect/EffectInternal.h"
#include "effect/EffectRenderCore.h"

#include <atomic>
#include <mutex>
#include <string>
#include <utility>

namespace coral {

struct AppleIOSurfaceOutputNode::State {
  State(IOSurfaceRef destination, const uint32_t w, const uint32_t h, const EffectPixelFormat f)
          : surface(destination), width(w), height(h), format(f) {
    if (surface != nullptr) CFRetain(surface);
  }

  ~State() {
    if (surface != nullptr) CFRelease(surface);
  }

  mutable std::mutex mutex;
  IOSurfaceRef surface = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  EffectPixelFormat format = EffectPixelFormat::BGRA8;
  EffectOutputConfig config;
  std::atomic_bool zeroCopy { false };
  std::atomic_bool presentFailed { false };
  std::atomic<uint64_t> frameCount { 0 };
};

namespace {

bool isSupportedFormat(const EffectPixelFormat format) noexcept {
  return format == EffectPixelFormat::RGBA8 || format == EffectPixelFormat::BGRA8 ||
         format == EffectPixelFormat::RGBA16F;
}

backend::PixelFormat toBackendFormat(const EffectPixelFormat format) noexcept {
  switch (format) {
    case EffectPixelFormat::RGBA8:
      return backend::PixelFormat::R8G8B8A8_UNORM;
    case EffectPixelFormat::BGRA8:
      return backend::PixelFormat::B8G8R8A8_UNORM;
    case EffectPixelFormat::RGBA16F:
      return backend::PixelFormat::R16G16B16A16_SFLOAT;
    default:
      return backend::PixelFormat::Undefined;
  }
}

uint32_t surfaceDimension(const IOSurfaceRef surface, const bool width) noexcept {
  return surface == nullptr ? 0 : static_cast<uint32_t>(width ? IOSurfaceGetWidth(surface) : IOSurfaceGetHeight(surface));
}

/** @return true when the surface is a bi-planar YUV image, which cannot be written as one RGBA texture. */
bool isBiPlanarYuvSurface(const IOSurfaceRef surface) noexcept {
  if (surface == nullptr) return false;
  const uint32_t format = static_cast<uint32_t>(IOSurfaceGetPixelFormat(surface));
  // '420v', '420f', '420i', 'x420'.
  return format == 0x34323076u || format == 0x34323066u || format == 0x34323069u || format == 0x78343230u;
}

class AppleIOSurfaceOutputRuntime final : public detail::OutputRuntime {
public:
  explicit AppleIOSurfaceOutputRuntime(std::shared_ptr<AppleIOSurfaceOutputNode::State> state)
          : mState(std::move(state)) {}

  ~AppleIOSurfaceOutputRuntime() override { releaseResources(); }

  [[nodiscard]] bool isReady() const noexcept override {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    return mState->surface != nullptr;
  }

  bool targetSize(const uint32_t sourceWidth, const uint32_t sourceHeight, uint32_t& width,
                  uint32_t& height) const override {
    (void) sourceWidth;
    (void) sourceHeight;
    const std::lock_guard<std::mutex> lock(mState->mutex);
    width = mState->width;
    height = mState->height;
    return width != 0 && height != 0;
  }

  EffectPixelFormat targetFormat() const override {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    return mState->format;
  }

  EffectScaleMode targetScaleMode() const override {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    return mState->config.scaleMode;
  }

  EffectColor targetLetterboxColor() const override {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    return mState->config.letterboxColor;
  }

  bool targetTransform(EffectRotation& rotation, bool& mirrorHorizontal, bool& mirrorVertical) const override {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    rotation = mState->config.rotation;
    mirrorHorizontal = mState->config.mirrorHorizontal;
    mirrorVertical = mState->config.mirrorVertical;
    return rotation != EffectRotation::None || mirrorHorizontal || mirrorVertical;
  }

  bool prepare(detail::RenderCore& core, std::string& error) override {
    backend::Device* const device = core.device();
    if (device == nullptr) {
      error = "the IOSurface output has no device";
      return false;
    }
    if (mDevice == device && mImported.valid()) return true;

    uint32_t width = 0;
    uint32_t height = 0;
    EffectPixelFormat format = EffectPixelFormat::BGRA8;
    IOSurfaceRef surface = nullptr;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      width = mState->width;
      height = mState->height;
      format = mState->format;
      surface = mState->surface;
    }
    if (surface == nullptr || width == 0 || height == 0) {
      error = "the IOSurface output has no destination surface";
      return false;
    }

    mDevice = device;
    const backend::Backend deviceBackend = device->getInfo().backend;
    if (deviceBackend == backend::Backend::Metal) {
      if (!device->supportsNativeTextureImport(backend::NativeTextureKind::IOSurface)) {
        error = "this Metal device cannot import an IOSurface";
        return false;
      }
      backend::NativeTextureDescriptor descriptor;
      descriptor.kind = backend::NativeTextureKind::IOSurface;
      descriptor.platformHandle = surface;
      descriptor.width = width;
      descriptor.height = height;
      descriptor.format = toBackendFormat(format);
      descriptor.writeback = true;
      descriptor.plane = 0;
      if (!backend::isSuccess(device->importNativeTexture(descriptor, &mImported)) || !mImported.valid()) {
        mImported = backend::TextureHandle::invalid();
        error = "the IOSurface could not be imported as a writeback texture";
        return false;
      }
      mZeroCopy = true;
    } else if (deviceBackend == backend::Backend::OpenGL) {
      std::string bridgeError;
      mBridge = detail::AppleGLIOSurfaceBridge::create(surface, width, height, format, true, bridgeError);
      if (mBridge == nullptr) {
        error = bridgeError;
        return false;
      }
      backend::NativeTextureDescriptor descriptor;
      descriptor.kind = backend::NativeTextureKind::OpenGlTexture;
      descriptor.glTexture = mBridge->texture();
      descriptor.glTarget = 0x0DE1; // GL_TEXTURE_2D; the bridge exposes a 2D name to the engine
      descriptor.width = width;
      descriptor.height = height;
      descriptor.format = toBackendFormat(format);
      if (!backend::isSuccess(device->importNativeTexture(descriptor, &mImported)) || !mImported.valid()) {
        mImported = backend::TextureHandle::invalid();
        mBridge.reset();
        error = "the OpenGL IOSurface texture could not be imported";
        return false;
      }
      mZeroCopy = mBridge->isZeroCopy();
    } else {
      error = "the IOSurface output supports Metal and OpenGL only";
      return false;
    }
    mState->zeroCopy.store(mZeroCopy, std::memory_order_relaxed);
    return true;
  }

  bool consume(detail::FrameContext& context, const detail::FrameView& view, std::string& error) override {
    if (!mImported.valid() || context.commands == nullptr) {
      error = "the IOSurface output is not prepared";
      return false;
    }
    uint32_t width = 0;
    uint32_t height = 0;
    EffectPixelFormat format = EffectPixelFormat::BGRA8;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      width = mState->width;
      height = mState->height;
      format = mState->format;
    }
    if (view.width != width || view.height != height) {
      error = "the final frame does not match the IOSurface size";
      return false;
    }
    if (view.format != format) {
      error = "the final frame does not match the IOSurface pixel format";
      return false;
    }
    backend::TextureCopyRegion region;
    region.textureOrigin = { 0, 0, 0 };
    region.size = { width, height, 1 };
    region.mipLevel = 0;
    region.baseArrayLayer = 0;
    region.arrayLayerCount = 1;
    if (!backend::isSuccess(context.commands->copyTextureToTexture(view.texture, mImported, region))) {
      mState->presentFailed.store(true, std::memory_order_release);
      error = "the final frame could not be copied into the IOSurface";
      return false;
    }
    mCopied = true;
    return true;
  }

  void afterSubmit(const std::shared_ptr<detail::GpuSubmission>& submission) override {
    (void) submission;
    if (!mCopied) return;
    mCopied = false;
    // Metal already wrote the surface from the frame's command buffer. OpenGL's macOS normalisation target
    // needs the post-submission blit; iOS only needs the Core Video cache flush.
    if (mDevice != nullptr && mDevice->getInfo().backend == backend::Backend::OpenGL) {
      std::string error;
      if (mBridge == nullptr || !mBridge->present(error)) {
        mState->presentFailed.store(true, std::memory_order_release);
        return;
      }
    }
    mState->presentFailed.store(false, std::memory_order_release);
    mState->frameCount.fetch_add(1, std::memory_order_relaxed);
  }

  void afterAbort() override { mCopied = false; }

  void releaseResources() override {
    if (mDevice != nullptr && mImported.valid()) mDevice->destroyTexture(mImported);
    mImported = backend::TextureHandle::invalid();
    mBridge.reset();
    mDevice = nullptr;
    mCopied = false;
    mZeroCopy = false;
  }

private:
  std::shared_ptr<AppleIOSurfaceOutputNode::State> mState;
  backend::Device* mDevice = nullptr;
  backend::TextureHandle mImported = backend::TextureHandle::invalid();
  std::shared_ptr<detail::AppleGLIOSurfaceBridge> mBridge;
  bool mCopied = false;
  bool mZeroCopy = false;
};

} // namespace anonymous

AppleIOSurfaceOutputNode::AppleIOSurfaceOutputNode(IOSurfaceRef const surface, const uint32_t width,
                                                   const uint32_t height, const EffectPixelFormat format)
        : EffectOutputNode(), mState(std::make_shared<State>(surface, width, height, format)) {
  setOutputRuntime(std::make_shared<AppleIOSurfaceOutputRuntime>(mState));
}

AppleIOSurfaceOutputNode::~AppleIOSurfaceOutputNode() = default;

std::shared_ptr<AppleIOSurfaceOutputNode> AppleIOSurfaceOutputNode::create(IOSurfaceRef const surface,
                                                                           const uint32_t width,
                                                                           const uint32_t height,
                                                                           const EffectPixelFormat format) {
  if (surface == nullptr || !isSupportedFormat(format) || isBiPlanarYuvSurface(surface)) return nullptr;
  const uint32_t resolvedWidth = width != 0 ? width : surfaceDimension(surface, true);
  const uint32_t resolvedHeight = height != 0 ? height : surfaceDimension(surface, false);
  if (resolvedWidth == 0 || resolvedHeight == 0) return nullptr;
  if (surfaceDimension(surface, true) != resolvedWidth || surfaceDimension(surface, false) != resolvedHeight) {
    return nullptr;
  }
  return std::shared_ptr<AppleIOSurfaceOutputNode>(
          new AppleIOSurfaceOutputNode(surface, resolvedWidth, resolvedHeight, format));
}

bool AppleIOSurfaceOutputNode::isReady() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->surface != nullptr;
}

EffectResult AppleIOSurfaceOutputNode::configure(const EffectOutputConfig& config) {
  // The destination's size and pixel layout are properties of the IOSurface, not of this call. Keep the parts that
  // describe how the frame is mapped into it (scale mode, letterbox colour, rotation and mirroring); the size and
  // format are overridden so a default-constructed config cannot turn a BGRA surface into an RGBA one.
  const std::lock_guard<std::mutex> lock(mState->mutex);
  if ((config.width != 0 && config.width != mState->width) ||
      (config.height != 0 && config.height != mState->height)) {
    return EffectResult::InvalidArgument;
  }
  mState->config = config;
  mState->config.width = mState->width;
  mState->config.height = mState->height;
  mState->config.format = mState->format;
  return EffectResult::Success;
}

IOSurfaceRef AppleIOSurfaceOutputNode::surface() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->surface;
}

uint32_t AppleIOSurfaceOutputNode::width() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->width;
}

uint32_t AppleIOSurfaceOutputNode::height() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->height;
}

bool AppleIOSurfaceOutputNode::isZeroCopy() const noexcept {
  return mState->zeroCopy.load(std::memory_order_relaxed);
}

uint64_t AppleIOSurfaceOutputNode::frameCount() const noexcept {
  return mState->frameCount.load(std::memory_order_relaxed);
}

bool AppleIOSurfaceOutputNode::hasPresentFailure() const noexcept {
  return mState->presentFailed.load(std::memory_order_relaxed);
}

} // namespace coral
