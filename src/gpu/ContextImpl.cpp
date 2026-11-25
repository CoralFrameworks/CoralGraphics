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

#include <private/gpu/ContextImpl.h>

#include "GpuRenderer.h"

#include <graphics/Bitmap.h>

#include <gpu/DrawList.h>
#include <gpu/Layer.h>

#include <utility>

namespace graphics::gpu {

/**
 * The definition of the nested type `ContextImpl` declares.
 *
 * `ContextImpl.h` forward declares a private nested `Renderer` so that the context's own header never
 * mentions a device resource, a pipeline or a shader module. This is that type, and it is a
 * `GpuRenderer` and nothing else: the alternative -- a wrapper holding one by value -- would add a
 * forwarding call to every method in the class and buy nothing, and the split that matters is already
 * there, between the context's interface and the renderer's implementation.
 *
 * The definition lives here rather than in `GpuRenderer.h` because a nested class of a class it also
 * *uses* cannot be declared before it: `ContextImpl.h` would have to include the renderer's header, and
 * the renderer's header includes the context's, which is a cycle. One translation unit owns both.
 */
class ContextImpl::Renderer final : public GpuRenderer {};

ContextImpl::ContextImpl() = default;

ContextImpl::~ContextImpl() {
  // Renderers are destroyed last to first. The first one owns the device, so it must outlive every
  // borrowed renderer that references it.
  mRenderer = nullptr;
  // Destroy last to first: the first renderer owns the device, so popping from the back keeps it
  // alive until every borrowed renderer that references it is gone.
  while (!mRenderers.empty()) mRenderers.pop_back();
  mDevice = nullptr;
  mOwnsDevice = false;
}

backend::Device* ContextImpl::releaseDevice() {
  if (!mOwnsDevice || mRenderer == nullptr) return nullptr;
  backend::Device* device = mRenderer->releaseDevice();
  if (device != nullptr) mOwnsDevice = false;
  return device;
}

backend::Result ContextImpl::initialize(backend::Device* device, const backend::Backend preferred,
        utils::JobSystem* jobs, const backend::DebugMessageCallback diagnostics, void* const diagnosticsUserData,
        const uint32_t maxFramesInFlight) {
  if (mRenderer != nullptr && mRenderer->isValid()) {
    return backend::Result::Error;
  }
  mRenderer = nullptr;
  mRenderers.clear();

  auto renderer = std::make_unique<Renderer>();
  const backend::Result result = renderer->initialize(device, preferred, jobs, diagnostics, diagnosticsUserData);
  if (!backend::isSuccess(result)) {
    mRenderers.push_back(std::move(renderer));
    mRenderer = mRenderers.front().get();
    mDevice = nullptr;
    mOwnsDevice = false;
    mJobs = jobs;
    return result;
  }

  renderer->setAsyncSubmissions(false);
  mRenderers.push_back(std::move(renderer));
  mRenderer = mRenderers.front().get();
  mDevice = mRenderer->device();
  mOwnsDevice = mRenderer->ownsDevice();
  mJobs = jobs;

  // Only Vulkan has an asynchronous queue submission here. OpenGL replays inside `Queue::submit`, so
  // extra slots would duplicate every pipeline, atlas and target while adding a fence/flush per frame
  // for no overlap. Keep the GL path at one slot even if the caller asked for more.
  const bool asyncCapable = mDevice != nullptr && mDevice->getInfo().backend == backend::Backend::Vulkan;
  const uint32_t wanted = asyncCapable
          ? std::min<uint32_t>(maxFramesInFlight == 0 ? 1 : maxFramesInFlight, 8u)
          : 1u;
  if (wanted > 1) mRenderer->setAsyncSubmissions(true);
  for (uint32_t slot = 1; slot < wanted; ++slot) {
    auto extra = std::make_unique<Renderer>();
    const backend::Result created = extra->initialize(mDevice, preferred, jobs, diagnostics, diagnosticsUserData);
    if (!backend::isSuccess(created)) {
      // One slot is still correct, just synchronous. Reporting the reduced count is better than
      // failing a context that can render.
      break;
    }
    extra->setAsyncSubmissions(true);
    mRenderers.push_back(std::move(extra));
  }
  mRenderer = mRenderers.front().get();
  if (mRenderers.size() == 1) mRenderer->setAsyncSubmissions(false);
  return backend::Result::Success;
}

uint32_t ContextImpl::frameSlotCount() const noexcept {
  return static_cast<uint32_t>(mRenderers.size());
}

void ContextImpl::selectFrameSlot(const uint32_t slot) noexcept {
  if (mRenderers.empty()) {
    mRenderer = nullptr;
    return;
  }
  mActiveFrameSlot = std::min<uint32_t>(slot, static_cast<uint32_t>(mRenderers.size() - 1));
  mRenderer = mRenderers[mActiveFrameSlot].get();
}

backend::Result ContextImpl::waitFrameSlot(const uint32_t slot) {
  if (mRenderers.empty()) return backend::Result::Unsupported;
  const uint32_t index = std::min<uint32_t>(slot, static_cast<uint32_t>(mRenderers.size() - 1));
  return mRenderers[index]->waitForFrame();
}

std::string ContextImpl::describeDevice() const {
  return mRenderer != nullptr ? mRenderer->describeDevice() : std::string("no device");
}

backend::Result ContextImpl::createRenderTarget(const RenderTargetDescription& description, RenderTarget* outTarget) {
  if (outTarget == nullptr) {
    return backend::Result::InvalidArgument;
  }
  if (mRenderer == nullptr) {
    *outTarget = RenderTarget {};
    return backend::Result::Unsupported;
  }
  return mRenderer->createRenderTarget(description, outTarget);
}

backend::Result ContextImpl::createLayerTarget(const uint32_t width, const uint32_t height,
        const backend::PixelFormat format, const bool needsStencil, RenderTarget* outTarget) {
  if (outTarget == nullptr) {
    return backend::Result::InvalidArgument;
  }
  if (mRenderer == nullptr) {
    *outTarget = RenderTarget {};
    return backend::Result::Unsupported;
  }
  return mRenderer->createLayerTarget(width, height, format, needsStencil, outTarget);
}

backend::Result ContextImpl::createWindowTarget(const uint32_t width, const uint32_t height,
        const backend::PixelFormat format, const uint32_t sampleCount, RenderTarget* outTarget) {
  if (outTarget == nullptr) {
    return backend::Result::InvalidArgument;
  }
  if (mRenderer == nullptr) {
    *outTarget = RenderTarget {};
    return backend::Result::Unsupported;
  }
  return mRenderer->createWindowTarget(width, height, format, sampleCount, outTarget);
}

void ContextImpl::destroyRenderTarget(RenderTarget& target) {
  if (mRenderer != nullptr) {
    mRenderer->destroyRenderTarget(target);
    return;
  }
  // Without a renderer there are no handles that could be released, and the target is emptied anyway so
  // that a caller which destroys after a failed `initialize` does not keep stale handles around.
  target = RenderTarget {};
}

bool ContextImpl::setSdfPattern(const Bitmap& pattern) {
  return mRenderer != nullptr && mRenderer->setSdfPattern(pattern);
}

uint64_t ContextImpl::fieldByteCacheHits() const noexcept {
  return mRenderer != nullptr ? mRenderer->fieldByteCacheHits() : 0;
}
backend::Result ContextImpl::submit(const DrawList& list, RenderTarget& target, const std::vector<Bitmap>& images,
    const backend::LoadOp rootLoad, const backend::SemaphoreHandle waitSemaphore,
    const backend::SemaphoreHandle signalSemaphore, const Rect& damage, const Point& translation,
    const bool continueFrame) {
  if (mRenderer == nullptr) {
    return backend::Result::Unsupported;
  }
  // The list is consumed by value and moved onward: a caller may recycle its own list immediately, which
  // is what lets a renderer keep one reusable list per recording thread.
  const backend::Result result = mRenderer->submit(list, target, images, rootLoad, waitSemaphore, signalSemaphore,
    damage, translation, continueFrame);
  // The renderer's counters are the ones that were actually updated, so they are copied out here rather
  // than kept in parallel. `lastFrameStats` and `frameCount` are this class's interface, and a second
  // copy of a number is a second place for it to be wrong -- which is what happened when this call was
  // missing: the interface reported a frame of zeros while the renderer's own report showed the truth.
  mStats = mRenderer->stats();
  mFrameCount = mRenderer->frameCount();
  return result;
}

uint64_t ContextImpl::textureUploadCount() const noexcept {
  // A getter on the renderer rather than a field here, for the reason stats() is copied out and not kept in
  // parallel: a second copy of a number is a second place for it to be wrong.
  return mRenderer != nullptr ? mRenderer->textureUploadCount() : 0;
}

double ContextImpl::lastFrameGpuMilliseconds() const noexcept {
  return mRenderer != nullptr ? mRenderer->gpuMilliseconds() : 0.0;
}

uint64_t ContextImpl::residentBytes() const noexcept {
  if (mRenderer == nullptr || mRenderer->device() == nullptr) return 0;
  return mRenderer->device()->memoryStatistics().totalBytes();
}

uint32_t ContextImpl::pooledFieldAtlasCount() const noexcept {
  // Asked of the renderer, like the upload count above: the pool is the renderer's, and a copy here would be a number
  // that is right only until the next frame changes it.
  return mRenderer != nullptr ? mRenderer->pooledFieldAtlasCount() : 0;
}

uint32_t ContextImpl::imageAtlasPageCount() const noexcept {
  // Asked of the renderer for the same reason: the pages are the renderer's and they outlive every frame.
  return mRenderer != nullptr ? mRenderer->imageAtlasPageCount() : 0;
}

backend::Result ContextImpl::present(const RenderTarget& source, RenderTarget& target,
    const PresentationParameters& parameters, const backend::SemaphoreHandle waitSemaphore,
    const backend::SemaphoreHandle signalSemaphore) {
  if (mRenderer == nullptr) return backend::Result::Unsupported;
  return mRenderer->present(source, target, parameters, waitSemaphore, signalSemaphore);
}

backend::Result ContextImpl::readback(RenderTarget& target, Bitmap* outBitmap) {
  if (mRenderer == nullptr) {
    return backend::Result::Unsupported;
  }
  return mRenderer->readback(target, outBitmap);
}

backend::Result ContextImpl::waitIdle() {
  if (mRenderer == nullptr) {
    return backend::Result::Unsupported;
  }
  return mRenderer->waitIdle();
}

std::string ContextImpl::diagnostics() const {
  if (mRenderer == nullptr) {
    return "canvas GPU context: not initialised\n";
  }
  return mRenderer->diagnostics();
}

} // namespace graphics::gpu

