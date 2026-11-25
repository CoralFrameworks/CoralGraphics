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
 * @file AppleIOSurfaceInputNode.cpp
 *
 * The Apple input: an IOSurface is handed to the graph through the engine's texture import. Metal wraps the
 * surface as an `id<MTLTexture>`; OpenGL goes through `AppleGLIOSurfaceBridge`, which is zero-copy on iOS and
 * one GPU blit on macOS. The producer's surface is retained by the queue, not copied.
 */

#include <effect/platform/apple/AppleIOSurfaceInputNode.h>

#include "AppleIOSurfaceGL.h"
#include "effect/EffectRenderCore.h"

#include <TargetConditionals.h>

#include <deque>
#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace coral {

struct AppleIOSurfaceInputNode::State {
  struct Queued {
    IOSurfaceRef surface = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    EffectPixelFormat format = EffectPixelFormat::BGRA8;
    RenderTime timestamp;
    bool hasTimestamp = false;
    std::shared_ptr<void> lifetime;
  };

  State(IOSurfaceRef source, const uint32_t w, const uint32_t h, const EffectPixelFormat f, const uint32_t depth)
          : surface(source), width(w), height(h), format(f), capacity(depth == 0 ? 1 : depth) {
    if (surface != nullptr) CFRetain(surface);
  }

  ~State() {
    if (surface != nullptr) CFRelease(surface);
    for (const Queued& entry : queue) {
      if (entry.surface != nullptr) CFRelease(entry.surface);
    }
  }

  mutable std::mutex mutex;
  IOSurfaceRef surface = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  EffectPixelFormat format = EffectPixelFormat::BGRA8;
  bool repeat = true;
  RenderTime timestamp;
  bool hasTimestamp = false;
  std::deque<Queued> queue;
  uint32_t capacity = 4;
  EffectBackpressureMode backpressure = EffectBackpressureMode::DropOldest;
  std::atomic_bool zeroCopy { false };
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

/**
 * @return true when the surface carries a bi-planar YUV video image.
 *
 * Those are the frames a hardware decoder produces by default ('420v'/'420f'), and they are not importable as
 * one RGBA texture: a YUV import needs two planes and a conversion shader. Refusing them by name is what turns
 * "the import failed" into a message the caller can act on (decode to BGRA, or use the CPU plane path).
 */
bool isBiPlanarYuvSurface(const IOSurfaceRef surface) noexcept {
  if (surface == nullptr) return false;
  const uint32_t format = static_cast<uint32_t>(IOSurfaceGetPixelFormat(surface));
  // '420v', '420f', '420i', 'x420', in the four-character code's big-endian order.
  return format == 0x34323076u || format == 0x34323066u || format == 0x34323069u || format == 0x78343230u;
}

/** The node's public entry points all agree on what a valid surface/format/extent is. */
bool validDescription(const IOSurfaceRef surface, const uint32_t width, const uint32_t height,
                      const EffectPixelFormat format) noexcept {
  if (surface == nullptr || width == 0 || height == 0 || !isSupportedFormat(format)) return false;
  if (isBiPlanarYuvSurface(surface)) return false;
  return surfaceDimension(surface, true) == width && surfaceDimension(surface, false) == height;
}

class AppleIOSurfaceRuntime final : public detail::InputRuntime {
public:
  explicit AppleIOSurfaceRuntime(std::shared_ptr<AppleIOSurfaceInputNode::State> state)
          : mState(std::move(state)) {}

  ~AppleIOSurfaceRuntime() override { releaseResources(); }

  bool produce(detail::FrameContext& context, detail::FrameView& view, EffectFrame& metadata,
               std::string& error) override {
    if (context.core == nullptr) {
      error = "the IOSurface input has no render context";
      return false;
    }
    pollRetired();

    AppleIOSurfaceInputNode::State::Queued queued;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      if (!mState->queue.empty()) {
        queued = std::move(mState->queue.front());
        mState->queue.pop_front();
      } else {
        queued.surface = mState->surface;
        queued.width = mState->width;
        queued.height = mState->height;
        queued.format = mState->format;
        queued.timestamp = mState->timestamp;
        queued.hasTimestamp = mState->hasTimestamp;
        if (queued.surface != nullptr) CFRetain(queued.surface);
      }
    }
    // The queue entry owns a reference; the frame holds it until the import below has retained the surface on
    // its own (a Metal texture keeps the surface alive, the GL bridge keeps a CF reference).
    struct SurfaceReference {
      IOSurfaceRef surface = nullptr;
      ~SurfaceReference() {
        if (surface != nullptr) CFRelease(surface);
      }
    } reference { queued.surface };

    if (queued.surface == nullptr || queued.width == 0 || queued.height == 0) {
      // No frame has been submitted yet: an empty error means "try again next frame", which is what a live
      // producer needs on the ticks before its first surface arrives.
      return false;
    }

    mDevice = context.core->device();
    if (mDevice == nullptr) {
      error = "the IOSurface input has no device";
      return false;
    }

    const RenderTime effectiveTimestamp = queued.hasTimestamp ? queued.timestamp : context.timestamp;
    const backend::Backend deviceBackend = mDevice->getInfo().backend;
    if (deviceBackend == backend::Backend::Metal) {
      if (!produceMetal(context, queued, effectiveTimestamp, view, metadata, error)) return false;
    } else if (deviceBackend == backend::Backend::OpenGL) {
      if (!produceOpenGL(context, queued, effectiveTimestamp, view, metadata, error)) return false;
    } else {
      error = "the IOSurface input supports Metal and OpenGL only";
      return false;
    }
    mFrameLifetime = std::move(queued.lifetime);
    mState->zeroCopy.store(mZeroCopy, std::memory_order_relaxed);
    return true;
  }

  void setBackpressure(const EffectBackpressureMode mode) override {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    mState->backpressure = mode;
  }

  void afterSubmit(const std::shared_ptr<detail::GpuSubmission>& submission) override {
    // The import stays alive: the engine's `repeatLastFrame` path can hand the previous FrameView to the next
    // frame, and a texture destroyed here would make that a dangling handle. A replaced surface retires the
    // previous import against this submission in `produce`.
    mFrameLifetime.reset();
    mLastSubmission = submission;
  }

  void afterAbort() override {
    // The produced image is still a valid surface, and the next frame may repeat it; only the producer token
    // belongs to the aborted frame. Device teardown goes through `releaseResources`.
    mFrameLifetime.reset();
  }

  void releaseResources() override {
    mFrameLifetime.reset();
    retireCurrent(nullptr, true);
    for (const Retired& retired : mRetired) {
      if (mDevice != nullptr && retired.texture.valid()) mDevice->destroyTexture(retired.texture);
    }
    mRetired.clear();
    mDevice = nullptr;
    mLastSubmission.reset();
  }

private:
  struct Retired {
    backend::TextureHandle texture = backend::TextureHandle::invalid();
    std::shared_ptr<detail::AppleGLIOSurfaceBridge> bridge;
    std::shared_ptr<detail::GpuSubmission> submission;
  };

  bool produceMetal(detail::FrameContext& context, const AppleIOSurfaceInputNode::State::Queued& queued,
                    const RenderTime timestamp, detail::FrameView& view, EffectFrame& metadata,
                    std::string& error) {
    if (!mDevice->supportsNativeTextureImport(backend::NativeTextureKind::IOSurface)) {
      error = "this Metal device cannot import an IOSurface";
      return false;
    }
    // A new import every frame: the wrapper shares the surface's storage, so this is a descriptor and a
    // retained surface, not a copy.
    backend::NativeTextureDescriptor descriptor;
    descriptor.kind = backend::NativeTextureKind::IOSurface;
    descriptor.platformHandle = queued.surface;
    descriptor.width = queued.width;
    descriptor.height = queued.height;
    descriptor.format = toBackendFormat(queued.format);
    descriptor.plane = 0;
    backend::TextureHandle imported = backend::TextureHandle::invalid();
    const backend::Result result = mDevice->importNativeTexture(descriptor, &imported);
    if (!backend::isSuccess(result) || !imported.valid()) {
      error = "the IOSurface could not be imported (backend result " + std::to_string(static_cast<int>(result)) + ")";
      return false;
    }
    // The previous frame's wrapper is still readable by that frame's GPU work; hand it to the retire list rather
    // than destroying it here.
    retireCurrent(mLastSubmission, true);
    mImported = imported;
    mZeroCopy = true;

    view.texture = mImported;
    view.width = queued.width;
    view.height = queued.height;
    view.format = queued.format;
    view.colorSpace = EffectColorSpace::SRGB;
    view.timestamp = timestamp;
    view.external = true;
    metadata = detail::FrameAccess::make(detail::allocateFrameId(context), queued.width, queued.height,
            queued.format, EffectColorSpace::SRGB, timestamp);
    return true;
  }

  bool produceOpenGL(detail::FrameContext& context, const AppleIOSurfaceInputNode::State::Queued& queued,
                     const RenderTime timestamp, detail::FrameView& view, EffectFrame& metadata,
                     std::string& error) {
    if (mBridge == nullptr || mBridgeSurface != queued.surface || mBridgeWidth != queued.width ||
        mBridgeHeight != queued.height || mBridgeFormat != queued.format) {
      std::string bridgeError;
      auto bridge = detail::AppleGLIOSurfaceBridge::create(queued.surface, queued.width, queued.height,
              queued.format, false, bridgeError);
      if (bridge == nullptr) {
        error = bridgeError;
        return false;
      }
      // Replace the previous bridge only after the new one exists, and retire it against the last submission
      // so its GL textures are not deleted under an in-flight frame.
      retireCurrent(mLastSubmission, true);
      mBridge = std::move(bridge);
      mBridgeSurface = queued.surface;
      mBridgeWidth = queued.width;
      mBridgeHeight = queued.height;
      mBridgeFormat = queued.format;
      mZeroCopy = mBridge->isZeroCopy();
    }

    if (!mImported.valid()) {
      backend::NativeTextureDescriptor descriptor;
      descriptor.kind = backend::NativeTextureKind::OpenGlTexture;
      descriptor.glTexture = mBridge->texture();
      descriptor.glTarget = 0x0DE1; // GL_TEXTURE_2D; the bridge only exposes a 2D name to the engine
      descriptor.width = queued.width;
      descriptor.height = queued.height;
      descriptor.format = toBackendFormat(queued.format);
      backend::TextureHandle imported = backend::TextureHandle::invalid();
      const backend::Result result = mDevice->importNativeTexture(descriptor, &imported);
      if (!backend::isSuccess(result) || !imported.valid()) {
        error = "the OpenGL IOSurface texture could not be imported";
        return false;
      }
      mImported = imported;
      mZeroCopy = mBridge->isZeroCopy();
    }

    // On macOS the 2D image is a copy of the surface and has to be refreshed for the current contents. On iOS the
    // texture is the surface itself and this is a cache flush.
    std::string refreshError;
    if (!mBridge->refresh(refreshError)) {
      error = refreshError;
      return false;
    }

    view.texture = mImported;
    view.width = queued.width;
    view.height = queued.height;
    view.format = queued.format;
    view.colorSpace = EffectColorSpace::SRGB;
    view.timestamp = timestamp;
    view.external = true;
    metadata = detail::FrameAccess::make(detail::allocateFrameId(context), queued.width, queued.height,
            queued.format, EffectColorSpace::SRGB, timestamp);
    return true;
  }

  void retireCurrent(const std::shared_ptr<detail::GpuSubmission>& submission, const bool destroyBridge) {
    if (!mImported.valid() && mBridge == nullptr) return;
    Retired retired;
    retired.texture = mImported;
    if (destroyBridge) retired.bridge = std::move(mBridge);
    retired.submission = submission;
    mImported = backend::TextureHandle::invalid();
    mBridge.reset();
    mBridgeSurface = nullptr;
    mBridgeWidth = 0;
    mBridgeHeight = 0;
    if (mDevice == nullptr || submission == nullptr) {
      if (mDevice != nullptr && retired.texture.valid()) mDevice->destroyTexture(retired.texture);
      return;
    }
    mRetired.push_back(std::move(retired));
  }

  void pollRetired() {
    if (mDevice == nullptr) return;
    for (auto it = mRetired.begin(); it != mRetired.end();) {
      if (it->submission == nullptr || it->submission->isDone()) {
        if (it->texture.valid()) mDevice->destroyTexture(it->texture);
        it = mRetired.erase(it);
      } else {
        ++it;
      }
    }
  }

  std::shared_ptr<AppleIOSurfaceInputNode::State> mState;
  backend::Device* mDevice = nullptr;
  backend::TextureHandle mImported = backend::TextureHandle::invalid();
  std::shared_ptr<detail::AppleGLIOSurfaceBridge> mBridge;
  IOSurfaceRef mBridgeSurface = nullptr;
  uint32_t mBridgeWidth = 0;
  uint32_t mBridgeHeight = 0;
  EffectPixelFormat mBridgeFormat = EffectPixelFormat::BGRA8;
  bool mZeroCopy = false;
  std::shared_ptr<detail::GpuSubmission> mLastSubmission;
  std::shared_ptr<void> mFrameLifetime;
  std::vector<Retired> mRetired;
};

} // namespace anonymous

AppleIOSurfaceInputNode::AppleIOSurfaceInputNode(IOSurfaceRef const surface, const uint32_t width,
                                                 const uint32_t height, const EffectPixelFormat format,
                                                 const uint32_t queueDepth)
        : EffectInputNode(), mState(std::make_shared<State>(surface, width, height, format, queueDepth)) {
  setInputRuntime(std::make_shared<AppleIOSurfaceRuntime>(mState));
}

AppleIOSurfaceInputNode::~AppleIOSurfaceInputNode() = default;

std::shared_ptr<AppleIOSurfaceInputNode> AppleIOSurfaceInputNode::create(IOSurfaceRef const surface,
                                                                         const uint32_t width,
                                                                         const uint32_t height,
                                                                         const EffectPixelFormat format,
                                                                         const uint32_t queueDepth) {
  if (surface == nullptr) return nullptr;
  const uint32_t resolvedWidth = width != 0 ? width : surfaceDimension(surface, true);
  const uint32_t resolvedHeight = height != 0 ? height : surfaceDimension(surface, false);
  if (!validDescription(surface, resolvedWidth, resolvedHeight, format)) return nullptr;
  return std::shared_ptr<AppleIOSurfaceInputNode>(
          new AppleIOSurfaceInputNode(surface, resolvedWidth, resolvedHeight, format, queueDepth));
}

std::shared_ptr<AppleIOSurfaceInputNode> AppleIOSurfaceInputNode::create(const uint32_t width,
                                                                         const uint32_t height,
                                                                         const EffectPixelFormat format,
                                                                         const uint32_t queueDepth) {
  if (width == 0 || height == 0 || !isSupportedFormat(format)) return nullptr;
  return std::shared_ptr<AppleIOSurfaceInputNode>(
          new AppleIOSurfaceInputNode(nullptr, width, height, format, queueDepth));
}

bool AppleIOSurfaceInputNode::hasFrame() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->surface != nullptr;
}

bool AppleIOSurfaceInputNode::repeatLastFrame() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->repeat;
}

void AppleIOSurfaceInputNode::setRepeatLastFrame(const bool repeat) {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->repeat = repeat;
}

uint32_t AppleIOSurfaceInputNode::width() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->width;
}

uint32_t AppleIOSurfaceInputNode::height() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->height;
}

EffectResult AppleIOSurfaceInputNode::setSurface(IOSurfaceRef const surface, const uint32_t width,
                                                const uint32_t height, const EffectPixelFormat format) {
  if (!isSupportedFormat(format) || isBiPlanarYuvSurface(surface)) return EffectResult::Unsupported;
  const uint32_t resolvedWidth = width != 0 ? width : surfaceDimension(surface, true);
  const uint32_t resolvedHeight = height != 0 ? height : surfaceDimension(surface, false);
  if (!validDescription(surface, resolvedWidth, resolvedHeight, format)) return EffectResult::InvalidArgument;

  const std::lock_guard<std::mutex> lock(mState->mutex);
  CFRetain(surface);
  for (const State::Queued& entry : mState->queue) {
    if (entry.surface != nullptr) CFRelease(entry.surface);
  }
  mState->queue.clear();
  if (mState->surface != nullptr) CFRelease(mState->surface);
  mState->surface = surface;
  mState->width = resolvedWidth;
  mState->height = resolvedHeight;
  mState->format = format;
  mState->timestamp = {};
  mState->hasTimestamp = false;
  return EffectResult::Success;
}

EffectResult AppleIOSurfaceInputNode::submit(IOSurfaceRef const surface, const uint32_t width,
                                             const uint32_t height, const EffectPixelFormat format,
                                             const RenderTime timestamp, std::shared_ptr<void> lifetime) {
  if (!isSupportedFormat(format)) return EffectResult::Unsupported;
  if (isBiPlanarYuvSurface(surface)) return EffectResult::Unsupported;
  if (!validDescription(surface, width, height, format)) return EffectResult::InvalidArgument;

  const std::lock_guard<std::mutex> lock(mState->mutex);
  if (mState->queue.size() >= mState->capacity) {
    if (mState->backpressure == EffectBackpressureMode::Block) return EffectResult::Busy;
    State::Queued& oldest = mState->queue.front();
    if (oldest.surface != nullptr) CFRelease(oldest.surface);
    mState->queue.pop_front();
  }
  CFRetain(surface);
  State::Queued queued;
  queued.surface = surface;
  queued.width = width;
  queued.height = height;
  queued.format = format;
  queued.timestamp = timestamp;
  queued.hasTimestamp = timestamp.isValid();
  queued.lifetime = std::move(lifetime);
  mState->queue.push_back(std::move(queued));

  // The most recent submission is also the "current" surface, exactly as `AndroidHardwareBufferInputNode` keeps
  // its buffer and `WindowsD3D11InputNode` keeps its texture: after the queue drains, a frame still samples the
  // newest image instead of reporting "no frame", and the metadata getters answer with the newest description.
  CFRetain(surface);
  if (mState->surface != nullptr) CFRelease(mState->surface);
  mState->surface = surface;
  mState->width = width;
  mState->height = height;
  mState->format = format;
  mState->timestamp = timestamp;
  mState->hasTimestamp = timestamp.isValid();
  return EffectResult::Success;
}

EffectResult AppleIOSurfaceInputNode::submit(const PixelBufferView& view) {
  if (view.nativeKind != NativeHandleKind::IOSurface || view.nativeHandle == nullptr) {
    return EffectResult::Unsupported;
  }
  if (view.waitToken != nullptr && !view.waitToken->wait(2000000000ull)) {
    return EffectResult::Busy;
  }
  return submit(static_cast<IOSurfaceRef>(view.nativeHandle), view.width, view.height, view.format,
    view.timestamp, view.lifetime);
}

void AppleIOSurfaceInputNode::setTimestamp(const RenderTime timestamp) {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->timestamp = timestamp;
  mState->hasTimestamp = timestamp.isValid();
}

bool AppleIOSurfaceInputNode::isZeroCopy() const noexcept {
  return mState->zeroCopy.load(std::memory_order_relaxed);
}

} // namespace coral
