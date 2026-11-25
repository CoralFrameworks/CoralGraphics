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

#ifndef CORAL_EFFECT_RENDER_CORE_H
#define CORAL_EFFECT_RENDER_CORE_H

/**
 * @file EffectRenderCore.h
 *
 * The engine's internal render core: the device, the pooled offscreen frames, the compiled programs and
 * the command-buffer/submission machinery. Everything here is render-thread-only and invisible to a
 * client; the public headers name none of these types.
 *
 * The shader conventions are the ones the previous verification framework measured (see the note on
 * `fillFullTargetQuad` and `composeFragmentSource`): a full-screen quad with `v = 1` at the target's top
 * row, `uv` as a texture coordinate, and the parameter block uploaded as a 16x1 `RGBA32F` texture.
 */

#include "EffectInternal.h"
#include "ShaderCompilePool.h"

#include <backend/Descriptors.h>
#include <backend/Device.h>
#include <backend/Handles.h>
#include <backend/Pipelines.h>
#include <backend/Types.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coral::detail {

class RenderCore;

/**
 * @return the backend's spelling of @p mode, for a swapchain descriptor.
 *
 * The public enum exists so an application can ask for "do not wait for the display"; a swapchain takes
 * the backend's own enum, and a backend that cannot grant the request reports the mode it granted
 * instead, so this is only a translation.
 */
[[nodiscard]] inline backend::PresentMode toBackendPresentMode(const EffectPresentMode mode) noexcept {
  switch (mode) {
    case EffectPresentMode::Immediate: return backend::PresentMode::Immediate;
    case EffectPresentMode::Mailbox: return backend::PresentMode::Mailbox;
    case EffectPresentMode::Fifo: break;
  }
  return backend::PresentMode::Fifo;
}

/** A thread-safe queue of callbacks drained by the application thread. */
class CallbackQueue final {
public:
  void push(std::function<void()> callback) {
    const std::lock_guard<std::mutex> lock(mMutex);
    mCallbacks.push_back(std::move(callback));
  }

  /** Runs at most @p max callbacks on the calling thread and returns how many ran. */
  size_t drain(size_t max) {
    std::deque<std::function<void()>> callbacks;
    {
      const std::lock_guard<std::mutex> lock(mMutex);
      while (max != 0 && !mCallbacks.empty()) {
        callbacks.push_back(std::move(mCallbacks.front()));
        mCallbacks.pop_front();
        --max;
      }
    }
    for (std::function<void()>& callback : callbacks) {
      if (callback) callback();
    }
    return callbacks.size();
  }

private:
  std::mutex mMutex;
  std::deque<std::function<void()>> mCallbacks;
};

/** One vertex of the final frame -> output blit. Shared by every window output. */
struct OutputBlitVertex {
  float x = 0.0f;
  float y = 0.0f;
  float u = 0.0f;
  float v = 0.0f;
};

/**
 * Builds the six vertices of the presentation quad.
 *
 * Scale mode and rotation are applied to the source coordinates, so an output node only has to bind a
 * vertex buffer and draw. The returned coordinates are in clip space, with v=1 at the target's top row.
 */
inline void buildOutputBlitVertices(const EffectScaleMode scaleMode, const EffectRotation rotation,
                                    const bool mirrorHorizontal, const bool mirrorVertical,
                                    const uint32_t sourceWidth, const uint32_t sourceHeight,
                                    const uint32_t targetWidth, const uint32_t targetHeight,
                                    OutputBlitVertex out[6]) {
  const bool swapAxes = rotation == EffectRotation::Clockwise90 || rotation == EffectRotation::Clockwise270;
  const float rotatedWidth = static_cast<float>(swapAxes ? sourceHeight : sourceWidth);
  const float rotatedHeight = static_cast<float>(swapAxes ? sourceWidth : sourceHeight);
  const float sourceAspect = rotatedHeight != 0.0f ? rotatedWidth / rotatedHeight : 1.0f;
  const float targetAspect = targetHeight != 0.0f ? static_cast<float>(targetWidth) / static_cast<float>(targetHeight)
                                                  : 1.0f;

  float clipX = 1.0f;
  float clipY = 1.0f;
  float u0 = 0.0f;
  float u1 = 1.0f;
  float v0 = 0.0f;
  float v1 = 1.0f;
  if (scaleMode == EffectScaleMode::Fit) {
    if (sourceAspect > targetAspect) {
      clipY = targetAspect / sourceAspect;
    } else {
      clipX = sourceAspect / targetAspect;
    }
  } else if (scaleMode == EffectScaleMode::Crop) {
    if (sourceAspect > targetAspect) {
      const float visible = targetAspect / sourceAspect;
      u0 = (1.0f - visible) * 0.5f;
      u1 = 1.0f - u0;
    } else {
      const float visible = sourceAspect / targetAspect;
      v0 = (1.0f - visible) * 0.5f;
      v1 = 1.0f - v0;
    }
  }

  const auto mapUv = [&](float u, float v) {
    float ru = u;
    float rv = v;
    switch (rotation) {
      case EffectRotation::Clockwise90:
        ru = 1.0f - v;
        rv = u;
        break;
      case EffectRotation::Clockwise180:
        ru = 1.0f - u;
        rv = 1.0f - v;
        break;
      case EffectRotation::Clockwise270:
        ru = v;
        rv = 1.0f - u;
        break;
      case EffectRotation::None:
      default:
        break;
    }
    if (mirrorHorizontal) ru = 1.0f - ru;
    if (mirrorVertical) rv = 1.0f - rv;
    return std::pair<float, float>{ ru, rv };
  };

  const float x0 = -clipX;
  const float x1 = clipX;
  const float y0 = -clipY;
  const float y1 = clipY;
  const auto topLeft = mapUv(u0, v1);
  const auto topRight = mapUv(u1, v1);
  const auto bottomRight = mapUv(u1, v0);
  const auto bottomLeft = mapUv(u0, v0);

  out[0] = { x0, y1, topLeft.first, topLeft.second };
  out[1] = { x1, y1, topRight.first, topRight.second };
  out[2] = { x1, y0, bottomRight.first, bottomRight.second };
  out[3] = { x0, y1, topLeft.first, topLeft.second };
  out[4] = { x1, y0, bottomRight.first, bottomRight.second };
  out[5] = { x0, y0, bottomLeft.first, bottomLeft.second };
}

// ==================================================================================================
// GPU submission tracking
// ==================================================================================================

/**
 * A CPU-side completion signal for one submitted frame.
 *
 * It carries no backend handle: the render thread polls the frame's submission and raises the signal, so
 * a public `Fence` can be waited on, and kept, without risking a stale device pointer.
 */
class FrameCompletion {
public:
  FrameCompletion() = default;
  FrameCompletion(const FrameCompletion&) = delete;
  FrameCompletion& operator=(const FrameCompletion&) = delete;

  /** Raises the signal. A second call is ignored, which makes shutdown idempotent. */
  void signal(bool success) noexcept;

  /** @return `kConditionSatisfied`/`kError` after completion, `kTimeoutExpired` when the wait ran out. */
  [[nodiscard]] Fence::Status waitFor(uint64_t timeoutNanoseconds);

  [[nodiscard]] bool isDone() const noexcept;

private:
  mutable std::mutex mMutex;
  std::condition_variable mCv;
  bool mDone = false;
  bool mSuccess = false;
};

/**
 * One fence and its completion state, shared between the submission ring and every frame the submission
 * wrote.
 *
 * Shared rather than copied because the fence must outlive the slot that submitted it: a frame recycled
 * two submissions later still needs to ask whether its own GPU work is done, and the slot has long since
 * moved on. The last owner destroys the backend fence, on the render thread.
 */
class GpuSubmission {
public:
  GpuSubmission(backend::Device* device, backend::FenceHandle fence,
                std::shared_ptr<FrameCompletion> completion = nullptr) noexcept;
  ~GpuSubmission();

  GpuSubmission(const GpuSubmission&) = delete;
  GpuSubmission& operator=(const GpuSubmission&) = delete;

  [[nodiscard]] backend::FenceHandle fence() const noexcept { return mFence; }
  [[nodiscard]] const std::shared_ptr<FrameCompletion>& completion() const noexcept { return mCompletion; }

  /** @return true once the GPU has finished. Polls the backend at most once. */
  [[nodiscard]] bool isDone() noexcept;

  /** Blocks until the submission completes. Used by `flushAndWait` and by a blocking slot acquire. */
  void wait() noexcept;

private:
  backend::Device* mDevice = nullptr;
  backend::FenceHandle mFence = backend::FenceHandle::invalid();
  std::shared_ptr<FrameCompletion> mCompletion;
  bool mDone = false;
};

// ==================================================================================================
// Frames and the pool
// ==================================================================================================

/** What a pooled frame is: the colour format and shape decide which bucket it belongs to. */
struct FrameKey {
  uint32_t width = 0;
  uint32_t height = 0;
  backend::PixelFormat format = backend::PixelFormat::R8G8B8A8_UNORM;
  uint32_t sampleCount = 1;
  bool needsDepthStencil = false;

  friend bool operator==(const FrameKey& a, const FrameKey& b) noexcept {
    return a.width == b.width && a.height == b.height && a.format == b.format &&
           a.sampleCount == b.sampleCount && a.needsDepthStencil == b.needsDepthStencil;
  }
};

struct FrameKeyHash {
  [[nodiscard]] size_t operator()(const FrameKey& key) const noexcept;
};

/** A fixed read-only description of a frame's pixels, for a pass to sample. */
struct FrameView {
  backend::TextureHandle texture = backend::TextureHandle::invalid();
  uint32_t width = 0;
  uint32_t height = 0;
  EffectPixelFormat format = EffectPixelFormat::RGBA8;
  EffectColorSpace colorSpace = EffectColorSpace::SRGB;
  /** The source presentation time, carried through every pass to the output. */
  RenderTime timestamp;
  /** Set for a YUV external texture; the sampler bound with this view must name the same conversion. */
  std::optional<backend::YcbcrConversionDescriptor> ycbcr;
  /** True when a per-output GPU subchain already applied rotation/mirror/scale. */
  bool transformApplied = false;
  /** True when the texture belongs to an input node rather than the pool; such a view is never returned. */
  bool external = true;

  [[nodiscard]] bool isValid() const noexcept { return texture.valid(); }
};

class FrameLease;

/**
 * One pooled offscreen colour target.
 *
 * Owned by `FramePool`; a node only ever sees a `FrameLease` and a `FrameView`. `refCount` is the number
 * of live leases, and a frame is not returned to the pool just because the last lease went away - the GPU
 * may still be reading it. `submission` is what makes that distinction possible.
 */
struct Frame {
  uint64_t id = 0;
  uint32_t generation = 0;
  FrameKey key;

  backend::TextureHandle color = backend::TextureHandle::invalid();
  backend::TextureViewHandle view = backend::TextureViewHandle::invalid();
  backend::RenderPassHandle pass = backend::RenderPassHandle::invalid();

  std::shared_ptr<GpuSubmission> submission;
  bool submitted = false;
  uint32_t refCount = 0;
  bool retired = false;

  uint64_t bytes = 0;

  [[nodiscard]] FrameView frameView() const noexcept {
    FrameView view;
    view.texture = color;
    view.width = key.width;
    view.height = key.height;
    view.format = key.format == backend::PixelFormat::B8G8R8A8_UNORM
                      ? EffectPixelFormat::BGRA8
                      : (key.format == backend::PixelFormat::R16G16B16A16_SFLOAT ? EffectPixelFormat::RGBA16F
                                                                                 : EffectPixelFormat::RGBA8);
    view.colorSpace = EffectColorSpace::SRGB;
    view.external = false;
    return view;
  }
};

/** The pool's counters, copied out once per `stats()` call. */
struct FramePoolStats {
  uint32_t free = 0;
  uint32_t inUse = 0;
  uint32_t inFlight = 0;
  uint64_t bytes = 0;
  uint64_t peakBytes = 0;
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t evictions = 0;
  uint64_t backpressureWaits = 0;
};

/**
 * The engine-owned framebuffer pool.
 *
 * `beginFrame` recycles the frames whose GPU work has completed; `acquire` hands out a lease; releasing
 * the lease retires the frame, and the next `beginFrame` after its fence completes puts it at the **back**
 * of its bucket's free queue - the cooling-off rule that makes reuse order deterministic and reduces the
 * chance of reusing a texture the driver is still finishing.
 */
class FramePool final : public std::enable_shared_from_this<FramePool> {
public:
  FramePool(backend::Device* device, uint64_t budgetBytes, uint64_t startId,
            EffectBackpressureMode backpressure = EffectBackpressureMode::DropOldest);
  ~FramePool();

  FramePool(const FramePool&) = delete;
  FramePool& operator=(const FramePool&) = delete;

  /** Borrows a frame of @p key. Returns an empty lease and fills @p error when the budget is exhausted. */
  FrameLease acquire(const FrameKey& key, std::string& error);

  /** Recycles retired frames whose submission completed, then trims the pool to its budget. */
  void beginFrame();

  /** Records that @p frame's pixels are read by @p submission. Called after the command buffer is submitted. */
  void markSubmitted(Frame* frame, std::shared_ptr<GpuSubmission> submission);

  /** Called by `FrameLease`. */
  void release(Frame* frame) noexcept;

  void setBudget(uint64_t bytes);
  void waitIdle();

  [[nodiscard]] FramePoolStats stats() const;
  [[nodiscard]] backend::Device* device() const noexcept { return mDevice; }

private:
  [[nodiscard]] Frame* createFrame(const FrameKey& key);
  void destroyFrame(Frame* frame);
  [[nodiscard]] uint64_t keyBytes(const FrameKey& key) const noexcept;
  void trimToBudget();

  backend::Device* mDevice = nullptr;
  uint64_t mBudgetBytes = 0;
  EffectBackpressureMode mBackpressure = EffectBackpressureMode::DropOldest;
  uint64_t mResidentBytes = 0;
  uint64_t mPeakBytes = 0;
  uint64_t mHits = 0;
  uint64_t mMisses = 0;
  uint64_t mEvictions = 0;
  uint64_t mBackpressureWaits = 0;
  uint64_t mNextId = 0;

  std::unordered_map<FrameKey, std::deque<Frame*>, FrameKeyHash> mFree;
  std::vector<Frame*> mInUse;
  std::vector<Frame*> mRetired;
  std::vector<std::unique_ptr<Frame>> mOwned;
};

/** Move-only RAII borrow of a pooled frame. The pool outlives a lease through the shared pointer it holds. */
class FrameLease {
public:
  FrameLease() noexcept = default;
  ~FrameLease();

  FrameLease(const FrameLease&) = delete;
  FrameLease& operator=(const FrameLease&) = delete;
  FrameLease(FrameLease&& other) noexcept;
  FrameLease& operator=(FrameLease&& other) noexcept;

  [[nodiscard]] Frame* get() const noexcept { return mFrame; }
  [[nodiscard]] FrameView view() const noexcept;
  [[nodiscard]] bool isValid() const noexcept { return mFrame != nullptr; }

  /** Associates the frame with the submission that reads it, so the pool knows when it may recycle. */
  void markSubmitted(std::shared_ptr<GpuSubmission> submission);

  void reset() noexcept;

private:
  friend class FramePool;

  FrameLease(std::shared_ptr<FramePool> pool, Frame* frame) noexcept;

  std::shared_ptr<FramePool> mPool;
  Frame* mFrame = nullptr;
};

// ==================================================================================================
// Input / output runtimes
// ==================================================================================================

/** What a node runtime needs to know about the frame it is producing or consuming. */
struct FrameContext {
  RenderCore* core = nullptr;
  /** The command buffer the frame is being recorded into, so an input upload or readback copy is ordered. */
  backend::CommandBuffer* commands = nullptr;
  /** The engine's frame pool, for a runtime that renders an input conversion into a pooled frame. */
  FramePool* pool = nullptr;
  /** Engine-wide source of `EffectFrame` ids; one counter per engine, not per input runtime. */
  std::atomic<uint64_t>* nextFrameId = nullptr;
  uint64_t frameIndex = 0;
  RenderTime timestamp {};
  uint32_t renderWidth = 0;
  uint32_t renderHeight = 0;
};

/** @return the next engine-unique frame id, or 0 when the context has no allocator (unit tests). */
inline uint64_t allocateFrameId(const FrameContext& context) noexcept {
  return context.nextFrameId != nullptr ? context.nextFrameId->fetch_add(1, std::memory_order_relaxed) : 0;
}

/** @return the backend format a pass of @p format renders to, or Undefined when it is not a colour target. */
inline backend::PixelFormat toPixelFormat(const EffectPixelFormat format) noexcept {
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

/**
 * Produces an input node's frame on the render thread.
 *
 * A concrete input node implements this in its own `.cpp`, which is the only place platform types appear.
 * Returning false with an empty @p error means "no new frame"; the engine then repeats the last frame or
 * counts a dropped frame according to `repeatLastFrame`.
 */
class InputRuntime {
public:
  InputRuntime() = default;
  virtual ~InputRuntime() = default;

  InputRuntime(const InputRuntime&) = delete;
  InputRuntime& operator=(const InputRuntime&) = delete;

  virtual bool produce(FrameContext& context, FrameView& view, EffectFrame& metadata, std::string& error) = 0;

  virtual void setBackpressure(EffectBackpressureMode mode) { (void) mode; }

  /** Called after the frame that used this input has been submitted. */
  virtual void afterSubmit(const std::shared_ptr<GpuSubmission>& submission) { (void) submission; }

  /** Called when the frame was dropped instead of submitted. */
  virtual void afterAbort() {}

  /** Drops any cached GPU state. Called before the render core is destroyed. */
  virtual void releaseResources() {}
};

/** Consumes the graph's final frame on the render thread. */
class OutputRuntime {
public:
  OutputRuntime() = default;
  virtual ~OutputRuntime() = default;

  OutputRuntime(const OutputRuntime&) = delete;
  OutputRuntime& operator=(const OutputRuntime&) = delete;

  virtual bool consume(FrameContext& context, const FrameView& view, std::string& error) = 0;

  /**
   * Creates what this output needs before the first frame, on a device that is already up.
   *
   * Called once from `start()`. A platform target can use it to lift its first-frame setup out of the frame: on
   * this machine `vkCreateSwapchainKHR` costs about 180 ms the first time and 25 ms for a second swapchain, none
   * of which belongs inside the frame a user is waiting for.
   *
   * A failure is not fatal - the output retries on the frame that needs it and reports there - so an
   * implementation only has to be honest about whether it succeeded.
   */
  virtual bool prepare(detail::RenderCore& core, std::string& error) {
    (void) core;
    (void) error;
    return true;
  }

  /**
   * Whether this output can skip a present that has nowhere to go.
   *
   * Default false: the split below exists so a backend that can answer "no image free" overrides these
   * methods, and every other output keeps the single `consume` call it has today.
   */
  [[nodiscard]] virtual bool supportsDeferredPresent() const noexcept { return false; }

  /**
   * Renders @p view into the presentable target.
   *
   * Returns false with an **empty** error when the target is busy: the executor then skips this frame's
   * present rather than failing the graph. Default: the whole job, `consume`.
   */
  virtual bool beginPresent(FrameContext& context, const FrameView& view, std::string& error) {
    return consume(context, view, error);
  }

  /** Hands the finished frame to the display system. Default: `beginPresent` already did it. */
  virtual backend::Result commitPresent() { return backend::Result::Success; }

  /** The output's configured policy; only meaningful when `supportsDeferredPresent()` is true. */
  [[nodiscard]] virtual EffectPresentPolicy presentPolicy() const noexcept {
    return EffectPresentPolicy::Blocking;
  }

  /** What this output does when a `Deferred` target is busy. */
  [[nodiscard]] virtual EffectPresentOverflow presentOverflow() const noexcept {
    return EffectPresentOverflow::DropOldest;
  }

  /** Waits until no present is outstanding. Default: nothing is outstanding. */
  virtual backend::Result drainPresents(std::chrono::milliseconds timeout) {
    (void) timeout;
    return backend::Result::Success;
  }

  [[nodiscard]] virtual bool isReady() const noexcept { return true; }

  /** true when the output can hand the next pass a swapchain target directly. */
  [[nodiscard]] virtual bool supportsDirectSurface() const noexcept { return false; }

  /** Acquires a direct target for the last pass. Default: unsupported. */
  virtual backend::Result acquireDirectTarget(backend::SwapchainTargetInfo& info) {
    (void) info;
    return backend::Result::Unsupported;
  }

  /** Presents/releases a target from acquireDirectTarget. Default: unsupported. */
  virtual backend::Result presentDirectTarget() { return backend::Result::Unsupported; }

  /** Starts a direct frame before the last pass: acquire/resize the platform target. */
  virtual bool beginDirectFrame(FrameContext& context, uint32_t width, uint32_t height, std::string& error) {
    (void) context;
    (void) width;
    (void) height;
    error = "direct surface is not implemented";
    return false;
  }

  /** Injected by the engine so an output can dispatch app-thread callbacks. */
  virtual void setCallbackQueue(std::shared_ptr<CallbackQueue> queue) { (void) queue; }

  /**
   * When this output wants its own GPU-scaled frame, fills @p width/@p height and returns true.
   * @p sourceWidth/@p sourceHeight are the incoming graph frame dimensions, so a rotated output can
   * derive the swapped extent when no explicit size was configured.
   */
  virtual bool targetSize(uint32_t sourceWidth, uint32_t sourceHeight, uint32_t& width, uint32_t& height) const {
    (void) sourceWidth;
    (void) sourceHeight;
    (void) width;
    (void) height;
    return false;
  }

  /** The rotation/mirror a per-output GPU scale pass should apply. */
  virtual bool targetTransform(EffectRotation& rotation, bool& mirrorHorizontal, bool& mirrorVertical) const {
    rotation = EffectRotation::None;
    mirrorHorizontal = false;
    mirrorVertical = false;
    return false;
  }

  /** The format a per-output GPU pass should write; RGBA8 unless the output asks for a conversion. */
  virtual EffectPixelFormat targetFormat() const { return EffectPixelFormat::RGBA8; }

  /** How the final frame is mapped when the output size differs. */
  virtual EffectScaleMode targetScaleMode() const { return EffectScaleMode::Stretch; }

  /** Colour used for the Fit-mode bars. */
  virtual EffectColor targetLetterboxColor() const { return EffectColor { 0.0f, 0.0f, 0.0f, 0.0f }; }

  /**
   * Called after the frame's command buffer has been submitted.
   *
   * The one hook a readback needs: its copy is recorded with the frame (so it reads the frame's pixels),
   * and only the submission fence can say when the copy has completed.
   */
  virtual void afterSubmit(const std::shared_ptr<GpuSubmission>& submission) { (void) submission; }

  /** Called when the frame the output recorded against was dropped instead of submitted. */
  virtual void afterAbort() {}

  /**
   * Fills @p descriptor when this output presents to a platform window.
   *
   * The engine asks before it creates its device, because a platform window has to be part of device
   * creation (EGL makes the window surface current there). An output attached after `start()` therefore
   * cannot present; `setOutput` is documented to be called before `start()` for such a node.
   */
  [[nodiscard]] virtual bool surfaceDescriptor(backend::SurfaceDescriptor& descriptor) const {
    (void) descriptor;
    return false;
  }

  virtual void releaseResources() {}
};

// ==================================================================================================
// Shader conventions (device-free)
// ==================================================================================================

/** @return the built-in full-screen vertex stage, in the dialect the device's compiler accepts. */
[[nodiscard]] const char* fullScreenVertexSource(bool direct, bool esDialect) noexcept;

/**
 * Fills six vertices with clip-space positions and texture coordinates.
 *
 * The contract, measured by the previous framework: a fragment at output row `r` interpolates
 * `v = 1 - (r + 0.5) / height`, and a target holds device row `r` at the same `v`, so sampling with this
 * `uv` reads the source's own row. `v = 1` goes on the vertex whose clip-space y is **-1**.
 */
void fillFullTargetQuad(float* out) noexcept;

/** @return whether @p device hands GLSL source to its driver unchanged. */
[[nodiscard]] bool acceptsDirectGlsl(const backend::Device& device) noexcept;

/** @return whether @p device's driver speaks OpenGL ES rather than desktop GL. */
[[nodiscard]] bool isEsDialect(const backend::Device& device) noexcept;

/** @return true when a rendered target's row 0 is the top of the picture. */
[[nodiscard]] bool hasTopDownTargets(const backend::Device& device) noexcept;

/** @return the composed fragment source for @p program, in the dialect @p direct selects. */
[[nodiscard]] std::string composeFragmentSource(const ProgramSnapshot& program, bool direct, bool esDialect,
                                                bool topDownTargets);

// ==================================================================================================
// Render programs
// ==================================================================================================

/**
 * One compiled effect: a fragment stage, the built-in vertex stage, and the pipeline/descriptor state a
 * pass over it needs.
 *
 * A program is shared by every node whose shader hash matches. It keeps its source rather than a shader
 * module, because the OpenGL backend consumes a module when it links a pipeline, so a second attachment
 * configuration needs a second module; the pipeline cache is what makes that a recompile only the first
 * time.
 */
class RenderProgram final {
public:
  RenderProgram(RenderCore& core, const ProgramSnapshot& snapshot);
  ~RenderProgram();

  RenderProgram(const RenderProgram&) = delete;
  RenderProgram& operator=(const RenderProgram&) = delete;

  [[nodiscard]] bool isValid() const noexcept { return mValid; }
  [[nodiscard]] const std::string& diagnostics() const noexcept { return mDiagnostics; }
  [[nodiscard]] uint32_t inputCount() const noexcept { return mInputCount; }
  [[nodiscard]] uint32_t pipelineCount() const noexcept { return static_cast<uint32_t>(mPipelines.size()); }
  [[nodiscard]] const ProgramSnapshot& snapshot() const noexcept { return *mSnapshot; }

  /** Frame that last resolved this program; the cache trimmer keeps recently used entries. */
  uint64_t lastUsedFrame = 0;

  /** @return a pipeline for a target of @p colorFormat, creating it when this shape is new. */
  [[nodiscard]] backend::RenderPipelineHandle pipelineFor(backend::PixelFormat colorFormat,
                                                          backend::RenderPassHandle compatiblePass, bool* created);

  /**
   * Requests the pipeline without blocking the render thread. Completion runs on the thread that polls
   * backend async operations. Returns false when the backend has no async path, in which case the
   * caller must call pipelineFor synchronously.
   */
  bool requestPipelineAsync(backend::PixelFormat colorFormat, backend::RenderPassHandle compatiblePass,
                            std::function<void(bool)> completion);

  [[nodiscard]] bool hasPendingPipeline() const noexcept { return mPendingPipelines != 0; }

  /** @return a descriptor set binding @p inputs, the parameter texture and the node's named textures. */
  [[nodiscard]] backend::DescriptorSetHandle setFor(const std::vector<backend::TextureHandle>& inputs,
                                                    backend::TextureHandle params, backend::TextureSamplerHandle sampler,
                                                    const std::vector<backend::TextureHandle>& extraTextures,
                                                    bool* created,
                                                    const std::vector<backend::TextureSamplerHandle>* inputSamplers = nullptr, const std::vector<backend::TextureViewHandle>* extraViews = nullptr, const std::vector<backend::TextureViewHandle>* inputViews = nullptr);

  void setLastUsed(uint64_t pass) noexcept { mLastUsed = pass; }
  [[nodiscard]] uint64_t lastUsed() const noexcept { return mLastUsed; }

private:
  struct PipelineEntry {
    backend::PixelFormat colorFormat = backend::PixelFormat::Undefined;
    backend::RenderPipelineHandle pipeline = backend::RenderPipelineHandle::invalid();
  };

  struct SetEntry {
    std::vector<backend::TextureHandle> inputs;
    std::vector<backend::TextureHandle> extras;
    backend::DescriptorSetHandle set = backend::DescriptorSetHandle::invalid();
    uint64_t lastUsed = 0;
  };

  [[nodiscard]] backend::RenderPipelineHandle createPipeline(backend::PixelFormat colorFormat,
                                                             backend::RenderPassHandle compatiblePass);
  bool makePipelineDescriptor(backend::PixelFormat colorFormat, backend::RenderPassHandle compatiblePass,
                              backend::RenderPipelineDescriptor& descriptor,
                              backend::ShaderModuleHandle& vertexModule,
                              backend::ShaderModuleHandle& fragmentModule);
  bool makeShaderModuleDescriptors(backend::ShaderModuleDescriptor& vertex,
                                   backend::ShaderModuleDescriptor& fragment) const;
  void fillPipelineDescriptor(backend::PixelFormat colorFormat, backend::RenderPassHandle compatiblePass,
                              backend::ShaderModuleHandle vertexModule,
                              backend::ShaderModuleHandle fragmentModule,
                              backend::RenderPipelineDescriptor& descriptor) const;
  void destroyObjects() noexcept;
  [[nodiscard]] bool isDeviceCurrent() const noexcept;

  RenderCore& mCore;
  uint64_t mDeviceGeneration = 0;
  std::shared_ptr<const ProgramSnapshot> mSnapshot;
  bool mValid = false;
  uint32_t mPendingPipelines = 0;
  std::string mDiagnostics;
  bool mDirectGlsl = false;
  bool mEsDialect = false;
  bool mTopDown = false;

  std::string mVertexSource;
  std::string mFragmentSource;

  uint32_t mInputCount = 0;
  std::vector<uint32_t> mSamplerBindings;
  uint32_t mParamsBinding = 0;
  uint32_t mExtraBinding = 0;
  /** The GLSL names of the sampler bindings, in binding order; the declarations point into these. */
  std::vector<std::string> mBindingNames;
  std::vector<backend::ShaderBindingDeclaration> mDeclarations;

  backend::DescriptorSetLayoutHandle mSetLayout = backend::DescriptorSetLayoutHandle::invalid();
  backend::PipelineLayoutHandle mPipelineLayout = backend::PipelineLayoutHandle::invalid();

  std::vector<PipelineEntry> mPipelines;
  std::vector<SetEntry> mSets;
  uint64_t mLastUsed = 0;
};

// ==================================================================================================
// Render core
// ==================================================================================================

/** The engine's device-owning core. Only the render thread may construct, use or destroy one. */
class RenderCore final {
public:
  RenderCore();
  ~RenderCore();

  RenderCore(const RenderCore&) = delete;
  RenderCore& operator=(const RenderCore&) = delete;

  /**
   * Brings up the device and the shared resources.
   *
   * @param surface when non-null, the device is created for that platform window, so the engine's own
   *                command buffers can present to it. A headless engine passes null.
   * @return false with a diagnostic on failure.
   */
  bool initialize(const EffectEngineConfig& config, const backend::SurfaceDescriptor* surface, std::string& error);

  /** Waits for the device, releases every resource. Safe to call twice. */
  void shutdown();

  [[nodiscard]] backend::Device* device() const noexcept { return mDevice.get(); }
  /** Monotonic id of the device this core currently owns; programs captured under an older id are stale. */
  [[nodiscard]] uint64_t deviceGeneration() const noexcept { return mDeviceGeneration; }
  [[nodiscard]] FramePool& framePool() noexcept { return *mPool; }
  [[nodiscard]] const FramePool& framePool() const noexcept { return *mPool; }
  [[nodiscard]] std::atomic<uint64_t>& frameIdAllocator() noexcept { return mFrameIdCounter; }
  [[nodiscard]] uint64_t timeBase() const noexcept { return mTimeBase; }
  [[nodiscard]] uint32_t maxFramesInFlight() const noexcept { return mMaxFramesInFlight; }
  [[nodiscard]] EffectBackpressureMode backpressureMode() const noexcept { return mBackpressure; }

  // ---- frame recording ---------------------------------------------------------------------------

  /** Recycles completed submissions and pooled frames. Must be called once at the start of a frame. */
  void beginFrame();

  /**
   * @return the command buffer for this frame, or null when no slot is free and the caller asked not to
   *         block. The buffer is already recording.
   */
  [[nodiscard]] backend::CommandBuffer* beginRecording(bool waitForSlot);

  /**
   * Ends and submits the frame's command buffer.
   *
   * @param submission receives the fence shared by every frame the recording wrote.
   * @param completion optional CPU-side completion raised once this submission finishes.
   */
  bool submitFrame(std::shared_ptr<GpuSubmission>& submission, std::string& error,
                   std::shared_ptr<FrameCompletion> completion = nullptr);

  /** Raises and retires any frame completions whose submissions have finished. */
  void pollCompletions();

  /** @return true when a public Fence is still waiting for its frame. */
  [[nodiscard]] bool hasPendingCompletions() const noexcept;

  /** Discards the current recording without submitting it. Used when a frame is dropped mid-record. */
  void abortRecording() noexcept;

  /** @return true when a submission slot is available without waiting. */
  [[nodiscard]] bool hasFreeSlot() const noexcept;

  void waitIdle();

  // ---- resources ---------------------------------------------------------------------------------

  /** @return the quad's vertex buffer. Every pass binds it. */
  [[nodiscard]] backend::BufferHandle quadBuffer() const noexcept { return mQuad; }

  /** @return the shared 16x1 parameter texture. */
  [[nodiscard]] backend::TextureHandle parameterTexture() const noexcept { return mParamsTexture; }
  [[nodiscard]] backend::TextureSamplerHandle linearSampler() const noexcept { return mLinearSampler; }

  /** @return a cached linear sampler carrying @p conversion, required for a YUV external texture. */
  backend::TextureSamplerHandle ycbcrSampler(const backend::YcbcrConversionDescriptor& conversion,
                                             std::string& error);

  /** @return true when the device accepts SPIR-V and a worker-safe GLSL compiler exists. */
  [[nodiscard]] bool canCompileAsync() const noexcept;
  [[nodiscard]] bool asyncPipelineCreation() const noexcept { return mAsyncPipelineCreation; }
  [[nodiscard]] bool topDownTargets() const noexcept;
  /** Queues a device-free shader compile; false when the pool is unavailable. */
  [[nodiscard]] bool submitShaderCompile(std::function<void()> task);

  /** @return the program for @p snapshot, compiling it on first use. Null on failure with @p error set. */
  [[nodiscard]] RenderProgram* programFor(const std::shared_ptr<const ProgramSnapshot>& snapshot, std::string& error);

  /**
   * Links @p program's pipeline for a target of @p format before a frame needs it.
   *
   * A driver links a pipeline the first time it draws, which is the first frame - the one a user is waiting for.
   * The warm-up borrows a pooled frame of that format for its render pass, so the link happens while `start()` is
   * still running. The pipeline is cached per (program, format, pass), so if the first frame ends up drawing into
   * a different pass the link still ran once for this program, which is the expensive part.
   */
  bool warmPipeline(RenderProgram& program, backend::PixelFormat format, uint32_t width, uint32_t height,
                    std::string& error);

  /** Converts YUV420 planes into a pooled RGBA8 frame with a fullscreen shader pass. */
  bool convertYuv420(backend::CommandBuffer* commands, uint32_t width, uint32_t height, const uint8_t* y,
          uint32_t yRowBytes, const uint8_t* u, uint32_t uRowBytes, const uint8_t* v, uint32_t vRowBytes,
          bool bt709, bool fullRange, bool p010, bool hdr, EffectHdrTransfer hdrTransfer,
          EffectPixelFormat targetFormat, EffectToneMapMode toneMapMode, float exposure, float paperWhiteNits,
          float hdr10PlusTargetedNits, float hdr10PlusAverageMaxRgb, float hdr10PlusFractionBrightPixels, float hdrMaxCll, float hdrMaxFall,
          FrameLease& out, std::string& error);

  bool convertHardwareBufferYuv(backend::CommandBuffer* commands, const void* buffer,
          uint32_t width, uint32_t height, bool bt2020, bool fullRange, bool p010, bool hdr,
          EffectHdrTransfer hdrTransfer, EffectPixelFormat targetFormat, EffectToneMapMode toneMapMode,
          float exposure, float paperWhiteNits, float hdr10PlusTargetedNits,
          float hdr10PlusAverageMaxRgb, float hdr10PlusFractionBrightPixels, float hdrMaxCll,
          float hdrMaxFall, FrameLease& out, std::string& error);

  /** @return the cached full-screen scale/copy program used by per-output GPU subchains. */
  [[nodiscard]] RenderProgram* outputScaleProgram(EffectRotation rotation, bool mirrorHorizontal,
                                                  bool mirrorVertical, EffectScaleMode scaleMode,
                                                  std::string& error);

  /** @return a texture holding @p image, uploading it on first use. */
  [[nodiscard]] backend::TextureHandle textureForImage(const std::shared_ptr<const EffectImage>& image,
                                                       std::string& error);

  /** @return a 1x1 texture of @p color. */
  [[nodiscard]] backend::TextureHandle textureForSolid(EffectColor color, std::string& error);

  /** Uploads @p block to the parameter texture. Must be called while recording. */
  bool writeParameters(const ParameterBlock& block, std::string& error);

  /** Reads @p texture's pixels back. Blocks until the copy completes; used by export and tests. */
  bool readback(backend::TextureHandle texture, uint32_t width, uint32_t height, backend::PixelFormat format,
                std::vector<uint8_t>& out, std::string& error);

  [[nodiscard]] uint32_t programCount() const noexcept { return static_cast<uint32_t>(mPrograms.size()); }
  [[nodiscard]] uint32_t pipelineCount() const noexcept {
    uint32_t total = 0;
    for (const auto& entry : mPrograms) total += entry.second->pipelineCount();
    return total;
  }

  /**
   * Destroys cached programs not resolved for a full submission ring once no submission is in flight.
   *
   * `programFor` stamps `lastUsedFrame` on every resolve, so a shader that no runtime asks for is safe
   * to drop after the GPU ring drains.
   */
  void trimPrograms(uint32_t maxPrograms);

  /**
   * Queues @p task to run after the submission ring drains.
   *
   * This is the render core's deferred-destroy hook: a runtime can hand over its GPU resource release and
   * return without stalling a frame. `flushRetired` runs the tasks when no slot is in flight.
   */
  void retireAfterGpu(std::function<void()> task);

  void setExternalTextureResolver(std::shared_ptr<EffectExternalTextureResolver> resolver);
  backend::TextureHandle resolveExternalTexture(const EffectExternalTexture& texture, std::string& error);
  void flushRetired();

  [[nodiscard]] uint32_t textureCount() const noexcept;
  [[nodiscard]] uint64_t programEvictions() const noexcept { return mProgramEvictions; }
  [[nodiscard]] uint64_t imageTextureEvictions() const noexcept { return mImageEvictions; }
  [[nodiscard]] uint64_t solidTextureEvictions() const noexcept { return mSolidEvictions; }
  [[nodiscard]] uint64_t textureCacheBytes() const noexcept;
  [[nodiscard]] uint64_t textureCachePeakBytes() const noexcept { return mTextureCachePeakBytes; }
  [[nodiscard]] uint64_t programCompileCount() const noexcept { return mProgramCompileCount; }
  [[nodiscard]] double programCompileMilliseconds() const noexcept { return mProgramCompileMilliseconds; }
  [[nodiscard]] uint64_t pipelineCreationCount() const noexcept { return mPipelineCreationCount; }
  [[nodiscard]] double pipelineCreationMilliseconds() const noexcept { return mPipelineCreationMilliseconds; }
  void recordPipelineCreation(double milliseconds) noexcept;
  [[nodiscard]] double gpuMilliseconds() const noexcept { return mGpuMilliseconds; }
  [[nodiscard]] std::string diagnostics() const;

private:
  struct Slot {
    backend::CommandBuffer* commands = nullptr;
    std::shared_ptr<GpuSubmission> submission;
    /** Created before recording starts; see the note in `beginRecording` on why. */
    backend::FenceHandle fence = backend::FenceHandle::invalid();
    bool recording = false;
  };

  bool createSharedResources(std::string& error);
  void destroySharedResources() noexcept;
  [[nodiscard]] Slot* acquireSlot(bool waitForSlot);
  [[nodiscard]] backend::CommandBuffer* temporaryCommands(std::string& error);
  /** Records into a one-shot command buffer, submits it and (optionally) waits for it. */
  bool executeTemporary(const std::function<void(backend::CommandBuffer&)>& record, bool wait, std::string& error);

  backend::Backend mapBackend(EffectBackend backend) const noexcept;

  /**
   * The device this core renders through.
   *
   * A created device is owned through the shared_ptr's default deleter; a borrowed
   * `EffectEngineConfig::externalDevice` uses a no-op deleter, so `shutdown()` releases the handle
   * without destroying the caller's device.
   */
  std::shared_ptr<backend::Device> mDevice;
  uint64_t mDeviceGeneration = 0;
  std::shared_ptr<FramePool> mPool;

  backend::BufferHandle mQuad = backend::BufferHandle::invalid();
  backend::TextureHandle mParamsTexture = backend::TextureHandle::invalid();
  backend::TextureHandle mYuvTextures[3] = {};
  bool mYuvPlaneP010 = false;
  std::unique_ptr<RenderProgram> mHardwareProgram;
  bool mHardwareHdr = false;
  EffectHdrTransfer mHardwareTransfer = EffectHdrTransfer::PQ;
  EffectToneMapMode mHardwareToneMap = EffectToneMapMode::Reinhard;
  float mHardwareExposure = 1.0f;
  float mHardwarePaperWhite = 100.0f;
  EffectPixelFormat mHardwareTarget = EffectPixelFormat::RGBA8;
  uint32_t mYuvWidth = 0;
  uint32_t mYuvHeight = 0;
  RenderProgram* mYuvProgram = nullptr;
  bool mYuvBt709 = false;
  bool mYuvFullRange = false;
  bool mYuvP010 = false;
  bool mYuvHdr = false;
  EffectHdrTransfer mYuvHdrTransfer = EffectHdrTransfer::PQ;
  EffectToneMapMode mYuvToneMap = EffectToneMapMode::Reinhard;
  float mYuvExposure = 1.0f;
  float mYuvPaperWhite = 100.0f;
  backend::TextureSamplerHandle mLinearSampler = backend::TextureSamplerHandle::invalid();
  backend::TextureSamplerHandle mNearestSampler = backend::TextureSamplerHandle::invalid();
  std::vector<std::pair<backend::YcbcrConversionDescriptor, backend::TextureSamplerHandle>> mYcbcrSamplers;

  std::vector<Slot> mSlots;
  struct PendingCompletion {
    std::shared_ptr<FrameCompletion> completion;
    std::weak_ptr<GpuSubmission> submission;
  };
  std::vector<PendingCompletion> mPendingCompletions;
  uint32_t mSlotCount = 0;
  uint32_t mNextSlot = 0;
  Slot* mRecording = nullptr;

  std::unordered_map<uint64_t, std::unique_ptr<RenderProgram>> mPrograms;
  std::vector<std::function<void()>> mRetired;
  std::shared_ptr<EffectExternalTextureResolver> mExternalTextureResolver;
  std::vector<std::function<void()>> mPendingRetired;
  struct RetiredSubmission {
    std::weak_ptr<GpuSubmission> submission;
    std::vector<std::function<void()>> tasks;
  };
  std::vector<RetiredSubmission> mRetiredSubmissions;

  struct ImageEntry {
    std::shared_ptr<const EffectImage> image;
    backend::TextureHandle texture = backend::TextureHandle::invalid();
    uint64_t lastUsedFrame = 0;
    size_t bytes = 0;
  };
  std::unordered_map<const EffectImage*, ImageEntry> mImages;
  struct SolidEntry {
    backend::TextureHandle texture = backend::TextureHandle::invalid();
    uint64_t lastUsedFrame = 0;
  };
  std::unordered_map<uint32_t, SolidEntry> mSolids;

  uint64_t mTimeBase = 60;
  uint32_t mMaxFramesInFlight = 2;
  bool mAsyncShaderCompilation = false;
  bool mAsyncPipelineCreation = false;
  std::unique_ptr<ShaderCompilePool> mShaderPool;
  size_t mTextureCacheBudgetBytes = 64u * 1024u * 1024u;
  EffectBackpressureMode mBackpressure = EffectBackpressureMode::DropOldest;
  uint64_t mFrameCounter = 0;
  uint64_t mProgramEvictions = 0;
  uint64_t mImageEvictions = 0;
  uint64_t mSolidEvictions = 0;
  uint64_t mTextureCachePeakBytes = 0;
  uint64_t mProgramCompileCount = 0;
  double mProgramCompileMilliseconds = 0.0;
  uint64_t mPipelineCreationCount = 0;
  double mPipelineCreationMilliseconds = 0.0;
  double mGpuMilliseconds = 0.0;
  bool mGpuTimingElapsed = false;
  backend::QuerySetHandle mTimestampQuery;
  std::atomic<uint64_t> mFrameIdCounter { 1 };
  std::string mDiagnostics;

  std::vector<backend::CommandBuffer*> mTemporaryBuffers;
};

} // namespace coral::detail

#endif // CORAL_EFFECT_RENDER_CORE_H

