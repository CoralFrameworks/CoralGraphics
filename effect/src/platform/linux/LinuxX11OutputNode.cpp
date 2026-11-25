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
 * @file LinuxX11OutputNode.cpp
 *
 * X11 presentation for the effect engine: one swapchain over the X display/window, and one full-screen
 * copy pass from the final pooled frame into the acquired image. The same code drives the OpenGL and the
 * Vulkan backend because both realise `defaultFramebufferView()` as "the acquired swapchain image".
 */

#include "effect/EffectInternal.h"
#include "effect/EffectRenderCore.h"

#include <X11/Xlib.h>

// Xlib defines these as macros, and this file uses the same spellings as enumerators in the RHI
// (`Result::Success`, `CompareOp::Always`, `CullMode::None`). Undefining them here is the standard price
// of including Xlib in a C++ translation unit that also includes a modern API.
#undef None
#undef Success
#undef Status
#undef Always
#undef Bool
#undef True
#undef False

#include <effect/platform/linux/LinuxX11OutputNode.h>

#include <algorithm>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>

namespace coral {
struct LinuxX11OutputNode::State {
  State(Display* nativeDisplay, Window nativeWindow) : display(nativeDisplay), window(nativeWindow) {}

  Display* display = nullptr;
  Window window = 0;
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

bool clientSize(Display* display, Window window, uint32_t& width, uint32_t& height) {
  if (display == nullptr || window == 0) return false;
  XWindowAttributes attributes{};
  if (XGetWindowAttributes(display, window, &attributes) == 0) return false;
  if (attributes.width <= 0 || attributes.height <= 0) return false;
  width = static_cast<uint32_t>(attributes.width);
  height = static_cast<uint32_t>(attributes.height);
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

class LinuxX11Runtime final : public detail::OutputRuntime {
public:
  explicit LinuxX11Runtime(std::shared_ptr<LinuxX11OutputNode::State> state)
          : mState(std::move(state)) {}

  ~LinuxX11Runtime() override { releaseResources(); }

  bool surfaceDescriptor(backend::SurfaceDescriptor& descriptor) const override {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    if (mState->window == 0 || mState->display == nullptr) return false;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!clientSize(mState->display, mState->window, width, height)) return false;
    mState->engineHasWindow = true;

    descriptor = backend::SurfaceDescriptor {};
    descriptor.nativeWindow = reinterpret_cast<void*>(static_cast<uintptr_t>(mState->window));
    descriptor.nativeDisplay = mState->display;
    descriptor.nativeWindowSystem = backend::NativeWindowSystem::X11;
    descriptor.width = mState->config.width != 0 ? mState->config.width : width;
    descriptor.height = mState->config.height != 0 ? mState->config.height : height;
    descriptor.presentMode = backend::PresentMode::Fifo;
    descriptor.label = "coral/effect-x11-output";
    return true;
  }

  bool isReady() const noexcept override {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    return mState->window != 0 && mState->display != nullptr;
  }

  bool consume(detail::FrameContext& context, const FrameView& view, std::string& error) override {
    EffectOutputConfig config;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      if (mState->window == 0 || mState->display == nullptr) {
        error = "the X11 output window is gone";
        return false;
      }
      config = mState->config;
    }
    if (!config.present) return true;
    if (mPresentFailed) {
      error = "the X11 swapchain lost its presentable surface";
      return false;
    }
    if (!view.isValid()) {
      error = "the X11 output received an invalid frame";
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
      if (!clientSize(mState->display, mState->window, desiredWidth, desiredHeight)) {
        error = "the X11 window has no client area";
        return false;
      }
      desiredWidth = mState->config.width != 0 ? mState->config.width : desiredWidth;
      desiredHeight = mState->config.height != 0 ? mState->config.height : desiredHeight;
    }
    if (desiredWidth != mSwapchainWidth || desiredHeight != mSwapchainHeight) {
      if (!backend::isSuccess(device->resizeSwapchain(mState->swapchain, desiredWidth, desiredHeight))) {
        error = "the X11 swapchain could not be resized";
        return false;
      }
      mSwapchainWidth = desiredWidth;
      mSwapchainHeight = desiredHeight;
    }

    backend::SwapchainInfo info;
    backend::Result acquired = device->beginFrame(mState->swapchain, &info);
    if (acquired == backend::Result::OutOfDate) {
      uint32_t width = 0;
      uint32_t height = 0;
      clientSize(mState->display, mState->window, width, height);
      device->resizeSwapchain(mState->swapchain, width, height);
      mSwapchainWidth = width;
      mSwapchainHeight = height;
      acquired = device->beginFrame(mState->swapchain, &info);
    }
    if (!backend::isSuccess(acquired)) {
      error = "the X11 swapchain could not hand over a frame (" + std::to_string(static_cast<int>(acquired)) + ")";
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
      error = "the X11 copy quad could not be uploaded";
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
      error = "the X11 copy pass could not be recorded";
      return false;
    }
    return true;
  }

  void afterSubmit(const std::shared_ptr<detail::GpuSubmission>& submission) override {
    if (submission != nullptr) submission->wait();
    if (!mFrameAcquired || mDevice == nullptr) return;
    if (!backend::isSuccess(mDevice->endFrame(mState->swapchain))) mPresentFailed = true;
    mFrameAcquired = false;
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
      if (!clientSize(mState->display, mState->window, width, height)) {
        error = "the X11 window has no client area";
        return false;
      }
      width = mState->config.width != 0 ? mState->config.width : width;
      height = mState->config.height != 0 ? mState->config.height : height;
    }

    backend::SurfaceDescriptor descriptor;
    descriptor.nativeWindow = reinterpret_cast<void*>(static_cast<uintptr_t>(mState->window));
    descriptor.nativeDisplay = mState->display;
    descriptor.nativeWindowSystem = backend::NativeWindowSystem::X11;
    descriptor.width = width;
    descriptor.height = height;
    descriptor.label = "coral/effect-x11-output";
    if (!backend::isSuccess(device->createSurface(descriptor, &mSurface))) {
      error = "the X11 surface could not be registered";
      return false;
    }

    backend::SwapchainDescriptor swapchain;
    swapchain.surface = mSurface;
    swapchain.width = width;
    swapchain.height = height;
    swapchain.format = backend::PixelFormat::R8G8B8A8_UNORM;
    swapchain.presentMode = detail::toBackendPresentMode(mState->config.presentMode);
    swapchain.label = "coral/effect-x11-swapchain";
    if (!backend::isSuccess(device->createSwapchain(swapchain, &mState->swapchain))) {
      error = "the X11 swapchain could not be created";
      return false;
    }
    mSwapchainWidth = width;
    mSwapchainHeight = height;
    return true;
  }

  bool ensurePresentResources(backend::Device* device, const backend::SwapchainInfo& info, std::string& error) {
    if (mSnapshot == nullptr) {
      auto snapshot = std::make_shared<ProgramSnapshot>();
      snapshot->desc.name = "x11/present";
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
        error = "the X11 presentation program did not compile: " + diagnostic;
        return false;
      }
    }

    if (!mVertexBuffer.valid()) {
      backend::BufferDescriptor descriptor;
      descriptor.size = kBlitVertexCount * sizeof(BlitVertex);
      descriptor.usage = static_cast<backend::BufferUsages>(backend::BufferUsage::Vertex);
      descriptor.memoryUsage = backend::MemoryUsage::CpuToGpuDynamic;
      descriptor.label = "effect/x11-present-vertices";
      if (!backend::isSuccess(device->createBuffer(descriptor, &mVertexBuffer))) {
        error = "the X11 presentation vertex buffer could not be created";
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
      pass.label = "coral/effect-x11-present";
      if (!backend::isSuccess(device->createRenderPass(pass, &mPass))) {
        error = "the X11 presentation render pass could not be created";
        return false;
      }
      mFormat = info.format;
    }
    return true;
  }

  std::shared_ptr<LinuxX11OutputNode::State> mState;
  backend::Device* mDevice = nullptr;
  detail::RenderCore* mOwner = nullptr;
  backend::SurfaceHandle mSurface = backend::SurfaceHandle::invalid();
  backend::BufferHandle mVertexBuffer = backend::BufferHandle::invalid();
  backend::RenderPassHandle mPass = backend::RenderPassHandle::invalid();
  backend::PixelFormat mFormat = backend::PixelFormat::Undefined;
  std::shared_ptr<const ProgramSnapshot> mSnapshot;
  RenderProgram* mProgram = nullptr;
  bool mFrameAcquired = false;
  bool mPresentFailed = false;
  uint32_t mSwapchainWidth = 0;
  uint32_t mSwapchainHeight = 0;
};

} // namespace

LinuxX11OutputNode::LinuxX11OutputNode(Display* display, Window window)
        : EffectOutputNode(),
          mState(std::make_shared<State>(display, window)) {
  setOutputRuntime(std::make_shared<LinuxX11Runtime>(mState));
}

LinuxX11OutputNode::~LinuxX11OutputNode() = default;

std::shared_ptr<LinuxX11OutputNode> LinuxX11OutputNode::create(Display* display, Window window) {
  if (display == nullptr || window == 0) return nullptr;
  return std::shared_ptr<LinuxX11OutputNode>(new LinuxX11OutputNode(display, window));
}

bool LinuxX11OutputNode::isReady() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->window != 0 && mState->display != nullptr;
}

EffectResult LinuxX11OutputNode::configure(const EffectOutputConfig& config) {
  if (config.format != EffectPixelFormat::RGBA8) {
    detail::NodeAccess::setDiagnostics(*this, "the X11 swapchain output accepts RGBA8 frames only");
    return EffectResult::Unsupported;
  }
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->config = config;
  return EffectResult::Success;
}

Display* LinuxX11OutputNode::display() const noexcept { return mState->display; }

Window LinuxX11OutputNode::window() const noexcept { return mState->window; }

} // namespace coral


