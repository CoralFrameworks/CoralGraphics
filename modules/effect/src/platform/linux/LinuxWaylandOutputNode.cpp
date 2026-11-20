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
 * @file LinuxWaylandOutputNode.cpp
 *
 * Wayland presentation for the effect engine. The application owns the `xdg_surface`/`xdg_toplevel`
 * role; this node wraps the `wl_surface` in the `wl_egl_window` the OpenGL EGL surface needs and then
 * follows the same "final pooled frame -> copy pass -> default-framebuffer view" path as every other
 * window output.
 */

#include "effect/EffectInternal.h"
#include "effect/EffectRenderCore.h"

#include <wayland-client.h>
#include <wayland-egl.h>

#include <effect/platform/linux/LinuxWaylandOutputNode.h>

#include <algorithm>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>

namespace coral {

struct LinuxWaylandOutputNode::State {
  State(wl_display* nativeDisplay, wl_surface* nativeSurface, const uint32_t w, const uint32_t h)
          : display(nativeDisplay), surface(nativeSurface), width(w), height(h) {}

  wl_display* display = nullptr;
  wl_surface* surface = nullptr;
  wl_egl_window* eglWindow = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
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
    { x0, y1, u0, v1 }, { x1, y1, u1, v1 }, { x1, y0, u1, v0 },
    { x0, y1, u0, v1 }, { x1, y0, u1, v0 }, { x0, y0, u0, v0 },
  };
  std::copy(std::begin(vertices), std::end(vertices), out);
}

class LinuxWaylandRuntime final : public detail::OutputRuntime {
public:
  explicit LinuxWaylandRuntime(std::shared_ptr<LinuxWaylandOutputNode::State> state) : mState(std::move(state)) {}
  ~LinuxWaylandRuntime() override { releaseResources(); }

  bool surfaceDescriptor(backend::SurfaceDescriptor& descriptor) const override {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    if (mState->display == nullptr || mState->surface == nullptr) return false;
    uint32_t width = mState->config.width != 0 ? mState->config.width : mState->width;
    uint32_t height = mState->config.height != 0 ? mState->config.height : mState->height;
    if (width == 0 || height == 0) return false;
    if (mState->eglWindow == nullptr) {
      // The GL backend's EGL surface wraps the wl_surface; Vulkan uses the surface directly.
      mState->eglWindow = wl_egl_window_create(mState->surface, static_cast<int>(width), static_cast<int>(height));
    }
    mState->engineHasWindow = true;

    descriptor = backend::SurfaceDescriptor {};
    descriptor.nativeWindow = mState->surface;
    descriptor.nativeDisplay = mState->display;
    descriptor.nativeWindowExtra = mState->eglWindow;
    descriptor.nativeWindowSystem = backend::NativeWindowSystem::Wayland;
    descriptor.width = width;
    descriptor.height = height;
    descriptor.presentMode = backend::PresentMode::Fifo;
    descriptor.label = "coral/effect-wayland-output";
    return true;
  }

  bool isReady() const noexcept override {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    return mState->display != nullptr && mState->surface != nullptr;
  }

  bool consume(detail::FrameContext& context, const FrameView& view, std::string& error) override {
    EffectOutputConfig config;
    uint32_t width = 0;
    uint32_t height = 0;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      if (mState->display == nullptr || mState->surface == nullptr) {
        error = "the Wayland output surface is gone";
        return false;
      }
      config = mState->config;
      width = config.width != 0 ? config.width : mState->width;
      height = config.height != 0 ? config.height : mState->height;
    }
    if (!config.present) return true;
    if (mPresentFailed) {
      error = "the Wayland swapchain lost its presentable surface";
      return false;
    }
    if (!view.isValid() || width == 0 || height == 0) {
      error = "the Wayland output received an invalid frame or size";
      return false;
    }

    backend::Device* const device = context.core->device();
    if (device == nullptr) {
      error = "the engine has no device";
      return false;
    }
    mDevice = device;
    mOwner = context.core;
    if (!ensureSwapchain(device, width, height, error)) return false;
    if (width != mSwapchainWidth || height != mSwapchainHeight) {
      if (!backend::isSuccess(device->resizeSwapchain(mState->swapchain, width, height))) {
        error = "the Wayland swapchain could not be resized";
        return false;
      }
      mSwapchainWidth = width;
      mSwapchainHeight = height;
    }

    backend::SwapchainInfo info;
    backend::Result acquired = device->beginFrame(mState->swapchain, &info);
    if (acquired == backend::Result::OutOfDate) {
      device->resizeSwapchain(mState->swapchain, width, height);
      acquired = device->beginFrame(mState->swapchain, &info);
    }
    if (!backend::isSuccess(acquired)) {
      error = "the Wayland swapchain could not hand over a frame";
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
      error = "the Wayland copy quad could not be uploaded";
      return false;
    }

    bool created = false;
    const backend::RenderPipelineHandle pipeline = mProgram->pipelineFor(info.format, mPass, &created);
    const backend::DescriptorSetHandle set = mProgram->setFor(
        { view.texture }, context.core->parameterTexture(), context.core->linearSampler(), {}, nullptr);
    if (!pipeline.valid() || !set.valid()) {
      device->endFrame(mState->swapchain);
      mFrameAcquired = false;
      error = "the Wayland copy program is not ready";
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
      error = "the Wayland copy pass could not be recorded";
      return false;
    }
    return true;
  }

  void afterSubmit(const std::shared_ptr<detail::GpuSubmission>& submission) override {
    if (submission != nullptr) submission->wait();
    if (mFrameAcquired && mDevice != nullptr) {
      if (!backend::isSuccess(mDevice->endFrame(mState->swapchain))) mPresentFailed = true;
    }
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
    if (mState->eglWindow != nullptr) {
      wl_egl_window_destroy(mState->eglWindow);
      mState->eglWindow = nullptr;
    }
  }

private:
  bool ensureSwapchain(backend::Device* device, const uint32_t width, const uint32_t height, std::string& error) {
    if (mState->swapchain.valid()) return true;
    backend::SurfaceDescriptor descriptor;
    descriptor.nativeWindow = mState->surface;
    descriptor.nativeDisplay = mState->display;
    descriptor.nativeWindowExtra = mState->eglWindow;
    descriptor.nativeWindowSystem = backend::NativeWindowSystem::Wayland;
    descriptor.width = width;
    descriptor.height = height;
    descriptor.label = "coral/effect-wayland-output";
    if (!backend::isSuccess(device->createSurface(descriptor, &mSurface))) {
      error = "the Wayland surface could not be registered";
      return false;
    }
    backend::SwapchainDescriptor swapchain;
    swapchain.surface = mSurface;
    swapchain.width = width;
    swapchain.height = height;
    swapchain.format = backend::PixelFormat::R8G8B8A8_UNORM;
    swapchain.presentMode = detail::toBackendPresentMode(mState->config.presentMode);
    swapchain.label = "coral/effect-wayland-swapchain";
    if (!backend::isSuccess(device->createSwapchain(swapchain, &mState->swapchain))) {
      error = "the Wayland swapchain could not be created";
      return false;
    }
    mSwapchainWidth = width;
    mSwapchainHeight = height;
    return true;
  }

  bool ensurePresentResources(backend::Device* device, const backend::SwapchainInfo& info, std::string& error) {
    if (mSnapshot == nullptr) {
      auto snapshot = std::make_shared<ProgramSnapshot>();
      snapshot->desc.name = "wayland/present";
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
        error = "the Wayland presentation program did not compile: " + diagnostic;
        return false;
      }
    }
    if (!mVertexBuffer.valid()) {
      backend::BufferDescriptor descriptor;
      descriptor.size = kBlitVertexCount * sizeof(BlitVertex);
      descriptor.usage = static_cast<backend::BufferUsages>(backend::BufferUsage::Vertex);
      descriptor.memoryUsage = backend::MemoryUsage::CpuToGpuDynamic;
      descriptor.label = "effect/wayland-present-vertices";
      if (!backend::isSuccess(device->createBuffer(descriptor, &mVertexBuffer))) {
        error = "the Wayland presentation vertex buffer could not be created";
        return false;
      }
    }
    if (mFormat != info.format) {
      if (mPass.valid()) device->destroyRenderPass(mPass);
      backend::RenderPassDescriptor pass;
      pass.colorFormats[0] = info.format;
      pass.colorAttachmentCount = 1;
      pass.sampleCount = 1;
      pass.label = "coral/effect-wayland-present";
      if (!backend::isSuccess(device->createRenderPass(pass, &mPass))) {
        error = "the Wayland presentation render pass could not be created";
        return false;
      }
      mFormat = info.format;
    }
    return true;
  }

  std::shared_ptr<LinuxWaylandOutputNode::State> mState;
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

LinuxWaylandOutputNode::LinuxWaylandOutputNode(wl_display* display, wl_surface* surface, const uint32_t width,
                                               const uint32_t height)
        : EffectOutputNode(),
          mState(std::make_shared<State>(display, surface, width, height)) {
  setOutputRuntime(std::make_shared<LinuxWaylandRuntime>(mState));
}

LinuxWaylandOutputNode::~LinuxWaylandOutputNode() = default;

std::shared_ptr<LinuxWaylandOutputNode> LinuxWaylandOutputNode::create(wl_display* display, wl_surface* surface,
                                                                       const uint32_t width, const uint32_t height) {
  if (display == nullptr || surface == nullptr || width == 0 || height == 0) return nullptr;
  return std::shared_ptr<LinuxWaylandOutputNode>(new LinuxWaylandOutputNode(display, surface, width, height));
}

bool LinuxWaylandOutputNode::isReady() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->display != nullptr && mState->surface != nullptr;
}

EffectResult LinuxWaylandOutputNode::configure(const EffectOutputConfig& config) {
  if (config.format != EffectPixelFormat::RGBA8) {
    detail::NodeAccess::setDiagnostics(*this, "the Wayland output accepts RGBA8 frames only");
    return EffectResult::Unsupported;
  }
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->config = config;
  return EffectResult::Success;
}

wl_display* LinuxWaylandOutputNode::display() const noexcept { return mState->display; }

wl_surface* LinuxWaylandOutputNode::surface() const noexcept { return mState->surface; }

} // namespace coral


