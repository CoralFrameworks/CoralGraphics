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
 * @file AndroidSurfaceOutputNode.cpp
 *
 * The Android presentation node: a swapchain over an `ANativeWindow`, and one full-screen copy pass from
 * the graph's final pooled frame into the acquired image.
 *
 * The engine creates its device for this window (see `surfaceDescriptor`), so `beginRenderPass` with
 * `backend::defaultFramebufferView()` targets the EGL window surface and `endFrame` presents. The copy is
 * a normal `RenderProgram`, which is what keeps this node on the same RHI path as every other pass.
 */

#include <effect/platform/android/AndroidSurfaceOutputNode.h>

#include <android/data_space.h>
#include <android/log.h>
#include <android/native_window_jni.h>

#include <dlfcn.h>

// The graphics backend defines the EGL handle types itself, so the real EGL headers cannot be
// included here: their `void*` typedefs would conflict. The three entry points used below are
// declared with the opaque handles they actually take, which has the same ABI.
extern "C" {
void* eglGetCurrentDisplay();
void* eglGetCurrentSurface(int which);
void eglPresentationTimeANDROID(void* display, void* surface, int64_t time);
}

namespace {

/** `EGL_DRAW`, the surface the current context renders into. */
constexpr int kEglDrawSurface = 0x3059;

} // namespace anonymous

#include "effect/EffectInternal.h"
#include "effect/EffectRenderCore.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>
#include <cmath>
#include <mutex>
#include <string>
#include <utility>

namespace coral {
struct AndroidSurfaceOutputNode::State {
  explicit State(ANativeWindow* nativeWindow) : window(nativeWindow) { ANativeWindow_acquire(window); }

  ~State() {
    if (window != nullptr) ANativeWindow_release(window);
  }

  ANativeWindow* window = nullptr;
  mutable std::mutex mutex;
  EffectOutputConfig config;
  /** Set by `surfaceDescriptor`; the engine device can only present if it was asked for the window. */
  mutable bool engineHasWindow = false;
  backend::SwapchainHandle swapchain = backend::SwapchainHandle::invalid();
  /** The media time of the next frame, for a consumer that times frames on the surface. */
  std::atomic<int64_t> nextPresentationTimeUs { -1 };
};


namespace {

using SetBuffersDataSpaceFn = int32_t (*)(ANativeWindow*, int32_t);

/** Applies the output colour space to the compositor; absent below API 28. */
void applyWindowDataSpace(ANativeWindow* window, const EffectColorSpace colorSpace) {
  static SetBuffersDataSpaceFn function = [] {
    void* library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    return library == nullptr ? nullptr
                              : reinterpret_cast<SetBuffersDataSpaceFn>(
                                        dlsym(library, "ANativeWindow_setBuffersDataSpace"));
  }();
  if (function == nullptr || window == nullptr) return;
  int32_t dataSpace = ADATASPACE_SRGB;
  if (colorSpace == EffectColorSpace::DisplayP3) dataSpace = ADATASPACE_DISPLAY_P3;
  if (colorSpace == EffectColorSpace::BT2020PQ) dataSpace = ADATASPACE_BT2020_PQ;
  if (colorSpace == EffectColorSpace::BT2020HLG) dataSpace = ADATASPACE_BT2020_HLG;
  function(window, dataSpace);
}

using detail::FrameView;
using detail::ProgramSnapshot;
using detail::RenderProgram;

/**
 * Times the next swap on the current EGL surface, which is what a `MediaCodec` input surface reads
 * as the presentation time of the frame it receives.
 */
void stampEglPresentationTime(const int64_t timeUs) {
  if (timeUs < 0) {
    return;
  }
  void* display = eglGetCurrentDisplay();
  void* surface = eglGetCurrentSurface(kEglDrawSurface);
  if (display == nullptr || surface == nullptr) {
    return;
  }
  eglPresentationTimeANDROID(display, surface, timeUs * 1000);
}

/** One vertex of the copy quad: clip-space position and the texture coordinate that goes with it. */
struct BlitVertex {
  float x, y, u, v;
};

constexpr uint32_t kBlitVertexCount = 6;

/**
 * Builds the six copy-quad vertices for @p mode.
 *
 * The position/UV pairing is the framework's orientation contract (`v = 1` at clip y = -1). `Fit` shrinks
 * the quad and leaves the rest of the swapchain black; `Crop` keeps the quad and narrows the sampled
 * region; `Stretch` does neither.
 */
void buildBlitVertices(const EffectScaleMode mode, const uint32_t sourceWidth, const uint32_t sourceHeight,
                       const uint32_t targetWidth, const uint32_t targetHeight, BlitVertex out[kBlitVertexCount]) {
  float clipX = 1.0f;
  float clipY = 1.0f;
  float u0 = 0.0f;
  float u1 = 1.0f;
  float v0 = 0.0f;
  float v1 = 1.0f;

  const float sourceAspect =
      sourceHeight != 0 ? static_cast<float>(sourceWidth) / static_cast<float>(sourceHeight) : 1.0f;
  const float targetAspect =
      targetHeight != 0 ? static_cast<float>(targetWidth) / static_cast<float>(targetHeight) : 1.0f;

  if (mode == EffectScaleMode::Fit) {
    if (sourceAspect > targetAspect) {
      clipY = targetAspect / sourceAspect;
    } else {
      clipX = sourceAspect / targetAspect;
    }
  } else if (mode == EffectScaleMode::Crop) {
    if (sourceAspect > targetAspect) {
      // The source is wider than the target: keep full height and crop the sides.
      const float visible = targetAspect / sourceAspect;
      u0 = (1.0f - visible) * 0.5f;
      u1 = 1.0f - u0;
    } else {
      const float visible = sourceAspect / targetAspect;
      v0 = (1.0f - visible) * 0.5f;
      v1 = 1.0f - v0;
    }
  }

  const float x0 = -clipX;
  const float x1 = clipX;
  const float y0 = -clipY;
  const float y1 = clipY;
  const BlitVertex vertices[kBlitVertexCount] = {
    { x0, y1, u0, v1 },
    { x1, y1, u1, v1 },
    { x1, y0, u1, v0 },
    { x0, y1, u0, v1 },
    { x1, y0, u1, v0 },
    { x0, y0, u0, v0 },
  };
  std::copy(std::begin(vertices), std::end(vertices), out);
}

class AndroidSurfaceOutputRuntime final : public detail::OutputRuntime {
public:
  explicit AndroidSurfaceOutputRuntime(std::shared_ptr<AndroidSurfaceOutputNode::State> state)
          : mState(std::move(state)) {}

  ~AndroidSurfaceOutputRuntime() override { releaseResources(); }

  bool surfaceDescriptor(backend::SurfaceDescriptor& descriptor) const override {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    if (mState->window == nullptr) return false;
    mState->engineHasWindow = true;

    descriptor = backend::SurfaceDescriptor {};
    descriptor.nativeWindow = mState->window;
    descriptor.nativeWindowSystem = backend::NativeWindowSystem::Android;
    descriptor.width = mState->config.width != 0 ? mState->config.width
                                                 : static_cast<uint32_t>(ANativeWindow_getWidth(mState->window));
    descriptor.height = mState->config.height != 0 ? mState->config.height
                                                   : static_cast<uint32_t>(ANativeWindow_getHeight(mState->window));
    applyWindowDataSpace(mState->window, mState->config.colorSpace);
    descriptor.preferredFormat = (mState->config.colorSpace == EffectColorSpace::BT2020PQ ||
                                   mState->config.colorSpace == EffectColorSpace::BT2020HLG)
                                         ? backend::PixelFormat::R16G16B16A16_SFLOAT
                                         : backend::PixelFormat::Undefined;
    descriptor.presentMode = backend::PresentMode::Fifo;
    descriptor.label = "coral/effect-android-output";
    return true;
  }

  bool isReady() const noexcept override {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    return mState->window != nullptr;
  }

  bool supportsDirectSurface() const noexcept override { return true; }

  /**
   * Creates the EGL surface and the swapchain during `start()`, when the window already has a size.
   *
   * A `SurfaceView` is frequently not laid out yet when the engine starts, and a surface with no size is not an
   * error - the frame that needs it creates it then. So this only moves the work when it can be moved; on a driver
   * that is slow to create a swapchain that is the difference between the cost landing before the first frame or
   * inside it. On the Windows compositor the same call is 180 ms, which is why the hook exists at all.
   */
  bool prepare(detail::RenderCore& core, std::string& error) override {
    backend::Device* const device = core.device();
    if (device == nullptr) {
      error = "the engine has no device";
      return false;
    }
    EffectOutputConfig config;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      if (mState->window == nullptr) return true;
      config = mState->config;
    }
    if (!config.present) return true;
    if (mState->swapchain.valid()) return true;

    const uint32_t width = config.width != 0 ? config.width
                                             : static_cast<uint32_t>(ANativeWindow_getWidth(mState->window));
    const uint32_t height = config.height != 0 ? config.height
                                               : static_cast<uint32_t>(ANativeWindow_getHeight(mState->window));
    // Not laid out yet: not an error, and not something to report as one.
    if (width == 0 || height == 0) return true;

    mDevice = device;
    mOwner = &core;
    const auto swapchainBegin = std::chrono::steady_clock::now();
    const bool created = ensureSwapchain(device, error);
    // One line, because "did the hook actually run" is otherwise indistinguishable from "it ran and was skipped
    // for a zero-sized surface" - and this is the only place that can say which.
    __android_log_print(ANDROID_LOG_INFO, "CoralPerf", "prepare_swapchain ok=%d ms=%.1f",
                        created ? 1 : 0,
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - swapchainBegin)
                                .count());
    if (!created) return false;

    // The presentation program and its render pass are also driver work - a shader compiled the first time it is
    // handed over - and they used to be created inside the first `beginDirectFrame`. Now that the swapchain exists
    // its format is known, which is all `ensurePresentResources` needs.
    backend::SwapchainInfo info;
    if (!backend::isSuccess(device->getSwapchainInfo(mState->swapchain, &info))) {
      // Not fatal: the frame that needs the target will ask again. Say so rather than pretending it is prepared.
      error = "the Android swapchain did not report its format for the presentation program";
      return false;
    }
    const auto presentBegin = std::chrono::steady_clock::now();
    const bool presentReady = ensurePresentResources(device, info, error);
    __android_log_print(ANDROID_LOG_INFO, "CoralPerf", "prepare_present ok=%d ms=%.1f", presentReady ? 1 : 0,
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - presentBegin)
                                .count());
    return presentReady;
  }

  bool beginDirectFrame(detail::FrameContext& context, const uint32_t width, const uint32_t height,
                        std::string& error) override {
    EffectOutputConfig config;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      if (mState->window == nullptr) {
        error = "the Android surface output has no window";
        return false;
      }
      config = mState->config;
    }
    if (!config.present) return false;
    if (mPresentFailed) {
      error = "the Android swapchain lost its presentable surface";
      return false;
    }
    backend::Device* const device = context.core != nullptr ? context.core->device() : nullptr;
    if (device == nullptr) {
      error = "the engine has no device";
      return false;
    }
    mOwner = context.core;
    mDevice = device;
    const bool hadSwapchain = mState->swapchain.valid();
    const auto swapchainBegin = std::chrono::steady_clock::now();
    const bool ready = ensureSwapchain(device, error);
    if (!mLoggedFirstFrame) {
      mLoggedFirstFrame = true;
      // The counterpart to `prepare_swapchain`: if `prepare` did its job this line shows `prepared=1` and a
      // ~0 ms creation. If it shows `prepared=0`, the hook was skipped - a window with no size at start() -
      // and the cost is back in the frame, which is worth knowing rather than assuming.
      __android_log_print(ANDROID_LOG_INFO, "CoralPerf", "first_frame_swapchain prepared=%d ok=%d ms=%.1f",
                          hadSwapchain ? 1 : 0, ready ? 1 : 0,
                          std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - swapchainBegin).count());
    }
    if (!ready) return false;
    uint32_t desiredWidth = width != 0 ? width : config.width;
    uint32_t desiredHeight = height != 0 ? height : config.height;
    if (desiredWidth == 0 || desiredHeight == 0) {
      desiredWidth = static_cast<uint32_t>(ANativeWindow_getWidth(mState->window));
      desiredHeight = static_cast<uint32_t>(ANativeWindow_getHeight(mState->window));
    }
    if (desiredWidth != mSwapchainWidth || desiredHeight != mSwapchainHeight) {
      if (!backend::isSuccess(device->resizeSwapchain(mState->swapchain, desiredWidth, desiredHeight))) {
        error = "the Android swapchain could not be resized";
        return false;
      }
      mSwapchainWidth = desiredWidth;
      mSwapchainHeight = desiredHeight;
    }
    backend::SwapchainInfo info;
    backend::Result acquired = device->beginFrame(mState->swapchain, &info);
    if (acquired == backend::Result::OutOfDate) {
      device->resizeSwapchain(mState->swapchain, desiredWidth, desiredHeight);
      acquired = device->beginFrame(mState->swapchain, &info);
    }
    if (!backend::isSuccess(acquired)) {
      error = "the swapchain could not hand over a frame (" + std::to_string(static_cast<int>(acquired)) + ")";
      return false;
    }
    mFrameAcquired = true;
    if (!ensurePresentResources(device, info, error)) {
      device->endFrame(mState->swapchain);
      mFrameAcquired = false;
      return false;
    }
    return true;
  }

  backend::Result acquireDirectTarget(backend::SwapchainTargetInfo& info) override {
    if (!mFrameAcquired || mDevice == nullptr) return backend::Result::NotReady;
    const backend::Result result =
        mDevice->acquireSwapchainTarget(mState->swapchain, &mDirectTarget, &info);
    if (!backend::isSuccess(result)) mDirectTarget = backend::SwapchainTargetHandle::invalid();
    return result;
  }

  backend::Result presentDirectTarget() override {
    if (!mDirectTarget.valid() || mDevice == nullptr) return backend::Result::InvalidHandle;
    stampEglPresentationTime(
      mState->nextPresentationTimeUs.exchange(-1, std::memory_order_relaxed));
    const backend::Result result = mDevice->presentSwapchainTarget(mState->swapchain, mDirectTarget);
    mDirectTarget = backend::SwapchainTargetHandle::invalid();
    mFrameAcquired = false;
    return result;
  }

  bool consume(detail::FrameContext& context, const FrameView& view, std::string& error) override {
    EffectOutputConfig config;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      if (mState->window == nullptr) {
        error = "the Android surface output has no window";
        return false;
      }
      config = mState->config;
    }
    if (!config.present) return true;
    if (mPresentFailed) {
      error = "the Android swapchain lost its presentable surface";
      return false;
    }
    if (!view.isValid()) {
      error = "the Android surface output received an invalid frame";
      return false;
    }

    mOwner = context.core;
    backend::Device* const device = context.core->device();
    if (device == nullptr) {
      error = "the engine has no device";
      return false;
    }
    mDevice = device;

    if (!ensureSwapchain(device, error)) return false;

    uint32_t desiredWidth = 0;
    uint32_t desiredHeight = 0;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      desiredWidth = mState->config.width != 0
                         ? mState->config.width
                         : static_cast<uint32_t>(ANativeWindow_getWidth(mState->window));
      desiredHeight = mState->config.height != 0
                          ? mState->config.height
                          : static_cast<uint32_t>(ANativeWindow_getHeight(mState->window));
    }
    if (desiredWidth != mSwapchainWidth || desiredHeight != mSwapchainHeight) {
      if (!backend::isSuccess(device->resizeSwapchain(mState->swapchain, desiredWidth, desiredHeight))) {
        error = "the Android swapchain could not be resized";
        return false;
      }
      mSwapchainWidth = desiredWidth;
      mSwapchainHeight = desiredHeight;
    }

    backend::SwapchainInfo info;
    backend::Result acquired = device->beginFrame(mState->swapchain, &info);
    if (acquired == backend::Result::OutOfDate) {
      const uint32_t width = static_cast<uint32_t>(ANativeWindow_getWidth(mState->window));
      const uint32_t height = static_cast<uint32_t>(ANativeWindow_getHeight(mState->window));
      device->resizeSwapchain(mState->swapchain, width, height);
      mSwapchainWidth = width;
      mSwapchainHeight = height;
      acquired = device->beginFrame(mState->swapchain, &info);
    }
    if (!backend::isSuccess(acquired)) {
      error = "the swapchain could not hand over a frame (" + std::to_string(static_cast<int>(acquired)) + ")";
      return false;
    }
    mFrameAcquired = true;

    if (!ensurePresentResources(device, info, error)) {
      device->endFrame(mState->swapchain);
      mFrameAcquired = false;
      return false;
    }

    detail::OutputBlitVertex vertices[kBlitVertexCount];
    detail::buildOutputBlitVertices(config.scaleMode, config.rotation, config.mirrorHorizontal,
                                    config.mirrorVertical, view.width, view.height, info.width, info.height,
                                    vertices);
    if (!backend::isSuccess(context.commands->writeBuffer(mVertexBuffer, 0, vertices, sizeof(vertices)))) {
      device->endFrame(mState->swapchain);
      mFrameAcquired = false;
      error = "the copy quad could not be uploaded";
      return false;
    }

    bool pipelineCreated = false;
    const backend::RenderPipelineHandle pipeline =
        mProgram->pipelineFor(info.format, mPass, &pipelineCreated);
    if (!pipeline.valid()) {
      device->endFrame(mState->swapchain);
      mFrameAcquired = false;
      error = "the copy program has no pipeline for the swapchain format";
      return false;
    }
    const backend::DescriptorSetHandle set = mProgram->setFor(
        { view.texture }, context.core->parameterTexture(), context.core->linearSampler(), {}, nullptr);
    if (!set.valid()) {
      device->endFrame(mState->swapchain);
      mFrameAcquired = false;
      error = "the copy program could not bind the final frame";
      return false;
    }

    backend::RenderPassBeginInfo begin;
    begin.renderPass = mPass;
    begin.colorAttachmentCount = 1;
    begin.colorAttachments[0].view = backend::defaultFramebufferView();
    begin.colorAttachments[0].loadOp = backend::LoadOp::Clear;
    begin.colorAttachments[0].storeOp = backend::StoreOp::Store;
    begin.colorAttachments[0].clearColor = backend::Color { config.letterboxColor.r, config.letterboxColor.g, config.letterboxColor.b, config.letterboxColor.a };
    begin.renderArea = { info.width, info.height };

    const bool recorded =
        backend::isSuccess(context.commands->beginRenderPass(begin)) &&
        backend::isSuccess(context.commands->setViewport({ 0.0f, 0.0f, static_cast<float>(info.width),
                                                           static_cast<float>(info.height), 0.0f, 1.0f })) &&
        backend::isSuccess(context.commands->bindRenderPipeline(pipeline)) &&
        backend::isSuccess(context.commands->bindDescriptorSet(0, set)) &&
        backend::isSuccess(context.commands->setVertexBuffer(0, mVertexBuffer, 0)) &&
        backend::isSuccess(context.commands->draw(kBlitVertexCount, 1, 0, 0)) &&
        backend::isSuccess(context.commands->endRenderPass());
    if (!recorded) {
      device->endFrame(mState->swapchain);
      mFrameAcquired = false;
      error = "the copy pass could not be recorded";
      return false;
    }

    return true;
  }

  void afterSubmit(const std::shared_ptr<detail::GpuSubmission>& submission) override {
    if (submission != nullptr) submission->wait();
    if (!mFrameAcquired || mDevice == nullptr) return;
    stampEglPresentationTime(
      mState->nextPresentationTimeUs.exchange(-1, std::memory_order_relaxed));
    if (!backend::isSuccess(mDevice->endFrame(mState->swapchain))) mPresentFailed = true;
    mFrameAcquired = false;
  }

  void afterAbort() override {
    if (mDirectTarget.valid() && mDevice != nullptr) {
      if (!backend::isSuccess(mDevice->presentSwapchainTarget(mState->swapchain, mDirectTarget))) {
        mPresentFailed = true;
      }
      mDirectTarget = backend::SwapchainTargetHandle::invalid();
      mFrameAcquired = false;
      return;
    }
    if (mFrameAcquired && mDevice != nullptr) {
      if (!backend::isSuccess(mDevice->endFrame(mState->swapchain))) mPresentFailed = true;
    }
    mFrameAcquired = false;
  }

  void releaseResources() override {
    if (mDevice != nullptr && mDirectTarget.valid()) {
      mDevice->presentSwapchainTarget(mState->swapchain, mDirectTarget);
      mDirectTarget = backend::SwapchainTargetHandle::invalid();
    }
    if (mDevice != nullptr) {
      if (mVertexBuffer.valid()) mDevice->destroyBuffer(mVertexBuffer);
      if (mPass.valid()) mDevice->destroyRenderPass(mPass);
      if (mState->swapchain.valid()) mDevice->destroySwapchain(mState->swapchain);
      if (mSurface.valid()) mDevice->destroySurface(mSurface);
    }
    mVertexBuffer = backend::BufferHandle::invalid();
    mPass = backend::RenderPassHandle::invalid();
    mState->swapchain = backend::SwapchainHandle::invalid();
    mSurface = backend::SurfaceHandle::invalid();
    mProgram = nullptr;
    mSnapshot.reset();
    mFormat = backend::PixelFormat::Undefined;
    mDevice = nullptr;
    mFrameAcquired = false;
    mPresentFailed = false;
    mDirectTarget = backend::SwapchainTargetHandle::invalid();
  }

private:
  bool ensureSwapchain(backend::Device* device, std::string& error) {
    if (mState->swapchain.valid()) return true;

    uint32_t width = 0;
    uint32_t height = 0;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      width = mState->config.width != 0 ? mState->config.width
                                        : static_cast<uint32_t>(ANativeWindow_getWidth(mState->window));
      height = mState->config.height != 0 ? mState->config.height
                                          : static_cast<uint32_t>(ANativeWindow_getHeight(mState->window));
    }
    if (width == 0 || height == 0) {
      // A surface with no size yet (a view that has not been laid out) is not a failure to present later.
      error = "the Android surface has no size yet";
      return false;
    }

    backend::SurfaceDescriptor descriptor;
    descriptor.nativeWindow = mState->window;
    descriptor.nativeWindowSystem = backend::NativeWindowSystem::Android;
    descriptor.width = width;
    descriptor.height = height;
    descriptor.label = "coral/effect-android-output";
    if (!backend::isSuccess(device->createSurface(descriptor, &mSurface))) {
      error = "the Android surface could not be registered";
      return false;
    }

    backend::SwapchainDescriptor swapchain;
    swapchain.surface = mSurface;
    swapchain.width = width;
    swapchain.height = height;
    swapchain.format = mState->config.format == EffectPixelFormat::RGBA16F
                               ? backend::PixelFormat::R16G16B16A16_SFLOAT
                               : backend::PixelFormat::R8G8B8A8_UNORM;
    swapchain.presentMode = detail::toBackendPresentMode(mState->config.presentMode);
    swapchain.label = "coral/effect-android-swapchain";
    if (!backend::isSuccess(device->createSwapchain(swapchain, &mState->swapchain))) {
      error = "the Android swapchain could not be created";
      return false;
    }
    mSwapchainWidth = width;
    mSwapchainHeight = height;
    return true;
  }

  bool ensurePresentResources(backend::Device* device, const backend::SwapchainInfo& info, std::string& error) {
    if (mSnapshot == nullptr) {
      auto snapshot = std::make_shared<ProgramSnapshot>();
      snapshot->desc.name = "android/present";
      // The window is opaque, so the copy states alpha rather than forwarding it. A camera or video
      // buffer sampled through an external (OES/YCbCr) texture carries no alpha of its own: the
      // Maleoon 910 driver returns 0 for it, and a pass that returns its input unchanged then presents
      // a fully transparent frame, which the compositor draws as black. The colour is untouched.
      snapshot->desc.fragmentSource = "  return vec4(texture(uInput0, uv).rgb, 1.0);";
      snapshot->desc.inputCount = 1;
      snapshot->hash = detail::hashProgram(snapshot->desc, snapshot->parameters, snapshot->textureNames);
      mSnapshot = std::move(snapshot);
    }
    if (mProgram == nullptr) {
      const std::shared_ptr<const ProgramSnapshot> stored = mSnapshot;
      std::string diagnostic;
      mProgram = mOwner != nullptr ? mOwner->programFor(stored, diagnostic) : nullptr;
      if (mProgram == nullptr) {
        error = "the presentation program did not compile: " + diagnostic;
        return false;
      }
    }

    if (!mVertexBuffer.valid()) {
      backend::BufferDescriptor descriptor;
      descriptor.size = kBlitVertexCount * sizeof(BlitVertex);
      descriptor.usage = static_cast<backend::BufferUsages>(backend::BufferUsage::Vertex);
      descriptor.memoryUsage = backend::MemoryUsage::CpuToGpuDynamic;
      descriptor.label = "effect/android-present-vertices";
      if (!backend::isSuccess(device->createBuffer(descriptor, &mVertexBuffer))) {
        error = "the presentation vertex buffer could not be created";
        return false;
      }
    }

    if (mFormat != info.format) {
      if (mPass.valid()) {
        device->destroyRenderPass(mPass);
        mPass = backend::RenderPassHandle::invalid();
      }
      backend::RenderPassDescriptor pass;
      pass.colorFormats[0] = info.format;
      pass.colorAttachmentCount = 1;
      pass.sampleCount = 1;
      pass.label = "coral/effect-android-present";
      if (!backend::isSuccess(device->createRenderPass(pass, &mPass))) {
        error = "the presentation render pass could not be created";
        return false;
      }
      mFormat = info.format;
    }
    // Link the presentation pipeline here too: it is the same argument as the program, and a driver that links
    // slowly would otherwise do it inside the frame that first present. The pipeline is cached by format, so this
    // costs nothing on a later frame that asks for the same one.
    bool pipelineCreated = false;
    if (!mProgram->pipelineFor(info.format, mPass, &pipelineCreated).valid()) {
      error = "the presentation program has no pipeline for the swapchain format";
      return false;
    }
    return true;
  }

private:
  std::shared_ptr<AndroidSurfaceOutputNode::State> mState;
  backend::Device* mDevice = nullptr;
  detail::RenderCore* mOwner = nullptr;
  backend::SurfaceHandle mSurface = backend::SurfaceHandle::invalid();
  backend::BufferHandle mVertexBuffer = backend::BufferHandle::invalid();
  backend::RenderPassHandle mPass = backend::RenderPassHandle::invalid();
  backend::SwapchainTargetHandle mDirectTarget = backend::SwapchainTargetHandle::invalid();
  backend::PixelFormat mFormat = backend::PixelFormat::Undefined;
  std::shared_ptr<const ProgramSnapshot> mSnapshot;
  RenderProgram* mProgram = nullptr;
  bool mFrameAcquired = false;
  bool mPresentFailed = false;
  /** Whether the one-off first-frame swapchain log has been written; see `beginDirectFrame`. */
  bool mLoggedFirstFrame = false;
  uint32_t mSwapchainWidth = 0;
  uint32_t mSwapchainHeight = 0;
};

} // namespace



AndroidSurfaceOutputNode::AndroidSurfaceOutputNode(ANativeWindow* window)
        : EffectOutputNode(),
          mState(std::make_shared<State>(window)) {
  setOutputRuntime(std::make_shared<AndroidSurfaceOutputRuntime>(mState));
}

AndroidSurfaceOutputNode::~AndroidSurfaceOutputNode() = default;

std::shared_ptr<AndroidSurfaceOutputNode> AndroidSurfaceOutputNode::create(ANativeWindow* window) {
  if (window == nullptr) return nullptr;
  return std::shared_ptr<AndroidSurfaceOutputNode>(new AndroidSurfaceOutputNode(window));
}

std::shared_ptr<AndroidSurfaceOutputNode> AndroidSurfaceOutputNode::create(JNIEnv* const env, jobject const surface) {
  if (env == nullptr || surface == nullptr) return nullptr;
  ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
  if (window == nullptr) return nullptr;
  std::shared_ptr<AndroidSurfaceOutputNode> node = create(window);
  // create() took its own reference; drop the one ANativeWindow_fromSurface returned.
  ANativeWindow_release(window);
  return node;
}

bool AndroidSurfaceOutputNode::isReady() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->window != nullptr;
}

EffectResult AndroidSurfaceOutputNode::configure(const EffectOutputConfig& config) {
  if (config.format != EffectPixelFormat::RGBA8 && config.format != EffectPixelFormat::RGBA16F) {
    detail::NodeAccess::setDiagnostics(*this, "the Android surface output accepts RGBA8/RGBA16F frames only");
    return EffectResult::Unsupported;
  }
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->config = config;
  return EffectResult::Success;
}

ANativeWindow* AndroidSurfaceOutputNode::window() const noexcept { return mState->window; }

void AndroidSurfaceOutputNode::setPresentationTimeUs(const int64_t timeUs) noexcept {
  mState->nextPresentationTimeUs.store(timeUs, std::memory_order_relaxed);
}

} // namespace coral


