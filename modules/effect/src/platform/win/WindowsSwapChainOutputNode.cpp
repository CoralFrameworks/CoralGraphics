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
 * @file WindowsSwapChainOutputNode.cpp
 *
 * Win32 presentation for the effect engine: one swapchain over the window's HDC/HWND, and one full-screen
 * copy pass from the final pooled frame into the acquired image. The same code drives the OpenGL and the
 * Vulkan backend because both realise `defaultFramebufferView()` as "the acquired swapchain image".
 */

#include <effect/platform/win/WindowsSwapChainOutputNode.h>

#include "effect/EffectInternal.h"
#include "effect/EffectRenderCore.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>

namespace coral {
struct WindowsSwapChainOutputNode::State {
  explicit State(HWND nativeWindow) : window(nativeWindow) {
    if (window != nullptr) dc = GetDC(window);
  }

  ~State() {
    if (window != nullptr && dc != nullptr) ReleaseDC(window, dc);
    dc = nullptr;
    window = nullptr;
  }

  HWND window = nullptr;
  HDC dc = nullptr;
  mutable std::mutex mutex;
  EffectOutputConfig config;
  mutable bool engineHasWindow = false;
  backend::SwapchainHandle swapchain = backend::SwapchainHandle::invalid();
};



namespace {

using detail::FrameView;
using detail::ProgramSnapshot;
using detail::RenderProgram;

struct BlitVertex {
  float x, y, u, v;
};

constexpr uint32_t kBlitVertexCount = 6;

bool clientSize(HWND window, uint32_t& width, uint32_t& height) {
  RECT rect{};
  if (window == nullptr || GetClientRect(window, &rect) == FALSE) return false;
  if (rect.right <= rect.left || rect.bottom <= rect.top) return false;
  width = static_cast<uint32_t>(rect.right - rect.left);
  height = static_cast<uint32_t>(rect.bottom - rect.top);
  return true;
}

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

class WindowsSwapChainRuntime final : public detail::OutputRuntime {
public:
  explicit WindowsSwapChainRuntime(std::shared_ptr<WindowsSwapChainOutputNode::State> state)
          : mState(std::move(state)) {}

  ~WindowsSwapChainRuntime() override { releaseResources(); }

  bool surfaceDescriptor(backend::SurfaceDescriptor& descriptor) const override {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    if (mState->window == nullptr) return false;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!clientSize(mState->window, width, height)) return false;
    mState->engineHasWindow = true;

    descriptor = backend::SurfaceDescriptor {};
    descriptor.nativeWindow = mState->window;
    descriptor.nativeDisplay = mState->dc;
    descriptor.nativeWindowSystem = backend::NativeWindowSystem::Win32;
    descriptor.width = mState->config.width != 0 ? mState->config.width : width;
    descriptor.height = mState->config.height != 0 ? mState->config.height : height;
    descriptor.presentMode = backend::PresentMode::Fifo;
    descriptor.label = "coral/effect-win-output";
    return true;
  }

  bool isReady() const noexcept override {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    return mState->window != nullptr && IsWindow(mState->window) != FALSE;
  }

  bool supportsDirectSurface() const noexcept override { return true; }

  bool beginDirectFrame(detail::FrameContext& context, const uint32_t width, const uint32_t height,
                        std::string& error) override {
    EffectOutputConfig config;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      if (mState->window == nullptr || IsWindow(mState->window) == FALSE) {
        error = "the Win32 output window is gone";
        return false;
      }
      config = mState->config;
    }
    if (!config.present) return false;
    if (mPresentFailed) {
      error = "the Win32 swapchain lost its presentable surface";
      return false;
    }
    backend::Device* const device = context.core != nullptr ? context.core->device() : nullptr;
    if (device == nullptr) {
      error = "the engine has no device";
      return false;
    }
    mDevice = device;
    mOwner = context.core;
    if (!ensureSwapchain(device, error)) return false;
    uint32_t desiredWidth = width != 0 ? width : config.width;
    uint32_t desiredHeight = height != 0 ? height : config.height;
    if (desiredWidth == 0 || desiredHeight == 0) {
      if (!clientSize(mState->window, desiredWidth, desiredHeight)) {
        error = "the Win32 window has no client area";
        return false;
      }
    }
    if (desiredWidth != mSwapchainWidth || desiredHeight != mSwapchainHeight) {
      if (!backend::isSuccess(device->resizeSwapchain(mState->swapchain, desiredWidth, desiredHeight))) {
        error = "the Win32 swapchain could not be resized";
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
      error = "the Win32 swapchain could not hand over a frame (" + std::to_string(static_cast<int>(acquired)) + ")";
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
    const backend::Result result = mDevice->presentSwapchainTarget(mState->swapchain, mDirectTarget);
    mDirectTarget = backend::SwapchainTargetHandle::invalid();
    mFrameAcquired = false;
    return result;
  }

  bool consume(detail::FrameContext& context, const FrameView& view, std::string& error) override {
    if (!beginPresent(context, view, error)) return false;
    return backend::isSuccess(commitPresent());
  }

  /**
   * Acquires the frame's presentable image and records the copy pass into it.
   *
   * The present itself happens from `afterSubmit`, after the frame's GPU work - that is where a
   * `WindowsSwapChainOutputNode` has always presented, and `commitPresent` therefore has nothing to do.
   *
   * When the output was configured `Deferred + DropOldest`, the acquire does not wait: a backend that can
   * answer "no image free" reports it, the frame keeps its rendered result, and only the present is skipped.
   * That is the difference between running at the compositor's rate and waiting inside `execute()` for one.
   */
  bool beginPresent(detail::FrameContext& context, const FrameView& view, std::string& error) override {

    EffectOutputConfig config;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      if (mState->window == nullptr || IsWindow(mState->window) == FALSE) {
        error = "the Win32 output window is gone";
        return false;
      }
      config = mState->config;
    }
    if (!config.present) return true;
    if (mPresentFailed) {
      error = "the Win32 swapchain lost its presentable surface";
      return false;
    }
    if (!view.isValid()) {
      error = "the Win32 output received an invalid frame";
      return false;
    }

    backend::Device* const device = context.core->device();
    if (device == nullptr) {
      error = "the engine has no device";
      return false;
    }
    mDevice = device;
    mOwner = context.core;

    if (!ensureSwapchain(device, error)) return false;

    uint32_t desiredWidth = 0;
    uint32_t desiredHeight = 0;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      if (!clientSize(mState->window, desiredWidth, desiredHeight)) {
        error = "the Win32 window has no client area";
        return false;
      }
      desiredWidth = mState->config.width != 0 ? mState->config.width : desiredWidth;
      desiredHeight = mState->config.height != 0 ? mState->config.height : desiredHeight;
    }
    if (desiredWidth != mSwapchainWidth || desiredHeight != mSwapchainHeight) {
      if (!backend::isSuccess(device->resizeSwapchain(mState->swapchain, desiredWidth, desiredHeight))) {
        error = "the Win32 swapchain could not be resized";
        return false;
      }
      mSwapchainWidth = desiredWidth;
      mSwapchainHeight = desiredHeight;
    }

    // `DropOldest` is the only mode that skips: `Wait` asked for every frame, so it takes the blocking
    // acquire, which is precisely "wait until the display has room".
    const bool maySkip = config.presentPolicy == EffectPresentPolicy::Deferred &&
            config.presentOverflow == EffectPresentOverflow::DropOldest;
    // Frame pacing. A free image is not the same thing as a display ready to show it: with the compositor
    // still working on the previous frame, `endFrame` blocks there instead. Presenting at most once per
    // interval keeps the renderer running at its own rate and lets the skipped frames cost nothing.
    if (maySkip && config.presentIntervalMilliseconds != 0 && mLastPresent != std::chrono::steady_clock::time_point {}) {
      const auto sinceLast = std::chrono::steady_clock::now() - mLastPresent;
      if (sinceLast < std::chrono::milliseconds(config.presentIntervalMilliseconds)) {
        return false;
      }
    }
    backend::SwapchainInfo info;
    backend::Result acquired = maySkip ? device->tryBeginFrame(mState->swapchain, &info)
                                       : device->beginFrame(mState->swapchain, &info);
    if (acquired == backend::Result::NotReady) {
      // Busy, not broken: an empty error tells the executor to skip the present rather than fail the frame.
      return false;
    }
    if (acquired == backend::Result::OutOfDate) {
      uint32_t width = 0;
      uint32_t height = 0;
      clientSize(mState->window, width, height);
      device->resizeSwapchain(mState->swapchain, width, height);
      mSwapchainWidth = width;
      mSwapchainHeight = height;
      acquired = device->beginFrame(mState->swapchain, &info);
    }
    if (!backend::isSuccess(acquired)) {
      error = "the Win32 swapchain could not hand over a frame (" + std::to_string(static_cast<int>(acquired)) + ")";
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
      error = "the Win32 copy quad could not be uploaded";
      return false;
    }

    bool pipelineCreated = false;
    const backend::RenderPipelineHandle pipeline = mProgram->pipelineFor(info.format, mPass, &pipelineCreated);
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
      error = "the Win32 copy pass could not be recorded";
      return false;
    }
    return true;
  }

  /**
   * Nothing to commit: this node presents from `afterSubmit`, once the frame's GPU work has finished.
   *
   * Splitting the present out of `beginPresent` is what lets the executor skip a present without skipping the
   * frame, and it is why the deferred path needs no extra synchronisation here - the image is only handed to
   * the display after the submission that rendered it.
   */
  backend::Result commitPresent() override { return backend::Result::Success; }

  [[nodiscard]] bool supportsDeferredPresent() const noexcept override { return true; }

  [[nodiscard]] EffectPresentPolicy presentPolicy() const noexcept override {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    return mState->config.presentPolicy;
  }

  [[nodiscard]] EffectPresentOverflow presentOverflow() const noexcept override {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    return mState->config.presentOverflow;
  }

  /**
   * Creates the surface and the swapchain now, during `start()`, instead of in the first frame.
   *
   * `vkCreateSwapchainKHR` measures at ~180 ms for the first swapchain in a process and ~25 ms for the second on
   * this machine - driver and compositor work that cannot be made cheaper, only moved to where nobody is waiting
   * for a frame. It is the whole reason a windowed first frame used to cost ~220 ms.
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
      config = mState->config;
    }
    if (!config.present) return true;

    mDevice = device;
    mOwner = &core;
    if (!ensureSwapchain(device, error)) return false;

    // The presentation program and pipeline are driver work too, and they used to be created inside the first
    // frame's `beginPresent`. With the swapchain up its format is known, which is all this needs.
    backend::SwapchainInfo info;
    if (!backend::isSuccess(device->getSwapchainInfo(mState->swapchain, &info))) {
      error = "the swapchain did not report its format for the presentation program";
      return false;
    }
    return ensurePresentResources(device, info, error);
  }

  void afterSubmit(const std::shared_ptr<detail::GpuSubmission>& submission) override {
    // The wait exists to know the image is safe to present. A frame this output skipped has no image, so it
    // must not pay for the synchronisation either - that is the difference between pacing the display and
    // pacing the renderer.
    if (!mFrameAcquired || mDevice == nullptr) return;
    if (submission != nullptr) submission->wait();
    if (!backend::isSuccess(mDevice->endFrame(mState->swapchain))) mPresentFailed = true;
    mFrameAcquired = false;
    mLastPresent = std::chrono::steady_clock::now();
  }

  void afterAbort() override {
    if (mFrameAcquired && mDevice != nullptr) {
      if (!backend::isSuccess(mDevice->endFrame(mState->swapchain))) mPresentFailed = true;
    }
    mFrameAcquired = false;
  }

  void releaseResources() override {
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
    mOwner = nullptr;
    mFrameAcquired = false;
    mPresentFailed = false;
  }

private:
  bool ensureSwapchain(backend::Device* device, std::string& error) {
    if (mState->swapchain.valid()) return true;

    uint32_t width = 0;
    uint32_t height = 0;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      if (!clientSize(mState->window, width, height)) {
        error = "the Win32 window has no client area";
        return false;
      }
      width = mState->config.width != 0 ? mState->config.width : width;
      height = mState->config.height != 0 ? mState->config.height : height;
    }

    backend::SurfaceDescriptor descriptor;
    descriptor.nativeWindow = mState->window;
    descriptor.nativeDisplay = mState->dc;
    descriptor.nativeWindowSystem = backend::NativeWindowSystem::Win32;
    descriptor.width = width;
    descriptor.height = height;
    descriptor.label = "coral/effect-win-output";
    if (!backend::isSuccess(device->createSurface(descriptor, &mSurface))) {
      error = "the Win32 surface could not be registered";
      return false;
    }

    backend::SwapchainDescriptor swapchain;
    swapchain.surface = mSurface;
    swapchain.width = width;
    swapchain.height = height;
    swapchain.format = backend::PixelFormat::R8G8B8A8_UNORM;
    swapchain.presentMode = detail::toBackendPresentMode(mState->config.presentMode);
    swapchain.label = "coral/effect-win-swapchain";
    if (!backend::isSuccess(device->createSwapchain(swapchain, &mState->swapchain))) {
      error = "the Win32 swapchain could not be created";
      return false;
    }
    mSwapchainWidth = width;
    mSwapchainHeight = height;
    return true;
  }

  bool ensurePresentResources(backend::Device* device, const backend::SwapchainInfo& info, std::string& error) {
    if (mSnapshot == nullptr) {
      auto snapshot = std::make_shared<ProgramSnapshot>();
      snapshot->desc.name = "win/present";
      snapshot->desc.fragmentSource = "  return texture(uInput0, uv);";
      snapshot->desc.inputCount = 1;
      snapshot->hash = detail::hashProgram(snapshot->desc, snapshot->parameters, snapshot->textureNames);
      mSnapshot = std::move(snapshot);
    }
    if (mProgram == nullptr) {
      const std::shared_ptr<const ProgramSnapshot> stored = mSnapshot;
      std::string diagnostic;
      mProgram = mOwner != nullptr ? mOwner->programFor(stored, diagnostic) : nullptr;
      if (mProgram == nullptr) {
        error = "the Win32 presentation program did not compile: " + diagnostic;
        return false;
      }
    }

    if (!mVertexBuffer.valid()) {
      backend::BufferDescriptor descriptor;
      descriptor.size = kBlitVertexCount * sizeof(BlitVertex);
      descriptor.usage = static_cast<backend::BufferUsages>(backend::BufferUsage::Vertex);
      descriptor.memoryUsage = backend::MemoryUsage::CpuToGpuDynamic;
      descriptor.label = "effect/win-present-vertices";
      if (!backend::isSuccess(device->createBuffer(descriptor, &mVertexBuffer))) {
        error = "the Win32 presentation vertex buffer could not be created";
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
      pass.label = "coral/effect-win-present";
      if (!backend::isSuccess(device->createRenderPass(pass, &mPass))) {
        error = "the Win32 presentation render pass could not be created";
        return false;
      }
      mFormat = info.format;
    }
    // Link the presentation pipeline as well. It is a few milliseconds here rather than the ~100 a GLES driver
    // wants, but the rule is the same one the rest of `start()` follows: anything a frame would otherwise compile
    // or link is done while nothing is on screen.
    bool pipelineCreated = false;
    if (!mProgram->pipelineFor(info.format, mPass, &pipelineCreated).valid()) {
      error = "the Win32 presentation program has no pipeline for the swapchain format";
      return false;
    }
    return true;
  }

  std::shared_ptr<WindowsSwapChainOutputNode::State> mState;
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
  uint32_t mSwapchainWidth = 0;
  uint32_t mSwapchainHeight = 0;
  /** When the last frame was handed to the display, for `presentIntervalMilliseconds`. */
  std::chrono::steady_clock::time_point mLastPresent {};
};

} // namespace

WindowsSwapChainOutputNode::WindowsSwapChainOutputNode(HWND window)
        : EffectOutputNode(),
          mState(std::make_shared<State>(window)) {
  setOutputRuntime(std::make_shared<WindowsSwapChainRuntime>(mState));
}

WindowsSwapChainOutputNode::~WindowsSwapChainOutputNode() = default;

std::shared_ptr<WindowsSwapChainOutputNode> WindowsSwapChainOutputNode::create(HWND window) {
  if (window == nullptr) return nullptr;
  return std::shared_ptr<WindowsSwapChainOutputNode>(new WindowsSwapChainOutputNode(window));
}

bool WindowsSwapChainOutputNode::isReady() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->window != nullptr && IsWindow(mState->window) != FALSE && mState->dc != nullptr;
}

EffectResult WindowsSwapChainOutputNode::configure(const EffectOutputConfig& config) {
  if (config.format != EffectPixelFormat::RGBA8) {
    detail::NodeAccess::setDiagnostics(*this, "the Win32 swapchain output accepts RGBA8 frames only");
    return EffectResult::Unsupported;
  }
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->config = config;
  return EffectResult::Success;
}

HWND WindowsSwapChainOutputNode::window() const noexcept { return mState->window; }

} // namespace coral





