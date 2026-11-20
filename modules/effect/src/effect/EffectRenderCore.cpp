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
 * @file EffectRenderCore.cpp
 *
 * The frame pool, the device, the command-buffer ring and the upload/readback helpers. Everything here
 * runs on the engine's render thread; nothing is exposed through a public header.
 */

#include "EffectRenderCore.h"

#include <backend/ShaderCompiler.h>

#include <chrono>
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>


namespace coral::detail {

// ==================================================================================================
// FrameCompletion
// ==================================================================================================

void FrameCompletion::signal(const bool success) noexcept {
  {
    const std::lock_guard<std::mutex> lock(mMutex);
    if (mDone) return;
    mDone = true;
    mSuccess = success;
  }
  mCv.notify_all();
}

Fence::Status FrameCompletion::waitFor(const uint64_t timeoutNanoseconds) {
  std::unique_lock<std::mutex> lock(mMutex);
  if (!mDone) {
    if (timeoutNanoseconds == Fence::kWaitForEver) {
      mCv.wait(lock, [this] { return mDone; });
    } else {
      mCv.wait_for(lock, std::chrono::nanoseconds(timeoutNanoseconds), [this] { return mDone; });
    }
  }
  if (!mDone) return Fence::Status::kTimeoutExpired;
  return mSuccess ? Fence::Status::kConditionSatisfied : Fence::Status::kError;
}

bool FrameCompletion::isDone() const noexcept {
  const std::lock_guard<std::mutex> lock(mMutex);
  return mDone;
}

// ==================================================================================================
// GpuSubmission
// ==================================================================================================

GpuSubmission::GpuSubmission(backend::Device* const device, const backend::FenceHandle fence,
                             std::shared_ptr<FrameCompletion> completion) noexcept
        : mDevice(device),
          mFence(fence),
          mCompletion(std::move(completion)) {}

GpuSubmission::~GpuSubmission() {
  if (mDevice != nullptr && mFence.valid()) mDevice->destroyFence(mFence);
}

bool GpuSubmission::isDone() noexcept {
  if (mDone) return true;
  if (mDevice == nullptr || !mFence.valid()) {
    mDone = true;
    return true;
  }
  mDone = backend::isSuccess(mDevice->getFenceStatus(mFence));
  return mDone;
}

void GpuSubmission::wait() noexcept {
  if (mDone || mDevice == nullptr || !mFence.valid()) return;
  mDevice->waitForFence(mFence);
  mDone = true;
}

// ==================================================================================================
// FramePool
// ==================================================================================================

size_t FrameKeyHash::operator()(const FrameKey& key) const noexcept {
  size_t hash = 1469598103934665603ull;
  const auto mix = [&hash](const uint64_t value) {
    hash ^= static_cast<size_t>(value);
    hash *= 1099511628211ull;
  };
  mix(key.width);
  mix(key.height);
  mix(static_cast<uint64_t>(key.format));
  mix(key.sampleCount);
  mix(key.needsDepthStencil ? 1u : 0u);
  return hash;
}

FramePool::FramePool(backend::Device* const device, const uint64_t budgetBytes, const uint64_t startId,
                     const EffectBackpressureMode backpressure)
        : mDevice(device),
          mBudgetBytes(budgetBytes),
          mBackpressure(backpressure),
          mNextId(startId) {}

FramePool::~FramePool() {
  for (const auto& frame : mOwned) {
    if (frame != nullptr) destroyFrame(frame.get());
  }
  mOwned.clear();
}

uint64_t FramePool::keyBytes(const FrameKey& key) const noexcept {
  const uint64_t color = backend::getFormatByteSize(key.format, key.width, key.height);
  const uint64_t depth = key.needsDepthStencil ? static_cast<uint64_t>(key.width) * key.height * 3u : 0u;
  return color + depth;
}

Frame* FramePool::createFrame(const FrameKey& key) {
  if (mDevice == nullptr) return nullptr;

  auto frame = std::make_unique<Frame>();
  frame->key = key;
  frame->bytes = keyBytes(key);

  backend::TextureDescriptor texture =
      backend::TextureDescriptor::texture2D(key.width, key.height, key.format,
                                            static_cast<backend::TextureUsages>(backend::TextureUsage::ColorAttachment |
                                                                                backend::TextureUsage::Sampled));
  texture.sampleCount = key.sampleCount;
  texture.mipLevelCount = 1;
  texture.label = "effect/frame";
  if (!backend::isSuccess(mDevice->createTexture(texture, &frame->color))) return nullptr;

  backend::TextureViewDescriptor view;
  view.texture = frame->color;
  view.dimension = backend::TextureViewDimension::Texture2D;
  view.format = key.format;
  view.range = backend::TextureSubresourceRange::all(1, 1);
  if (!backend::isSuccess(mDevice->createTextureView(view, &frame->view))) {
    mDevice->destroyTexture(frame->color);
    return nullptr;
  }

  backend::RenderPassDescriptor pass;
  pass.colorFormats[0] = key.format;
  pass.colorAttachmentCount = 1;
  pass.depthStencilFormat = key.needsDepthStencil ? backend::PixelFormat::D24_UNORM_S8_UINT
                                                  : backend::PixelFormat::Undefined;
  pass.sampleCount = key.sampleCount;
  pass.label = "effect/frame";
  if (!backend::isSuccess(mDevice->createRenderPass(pass, &frame->pass))) {
    mDevice->destroyTextureView(frame->view);
    mDevice->destroyTexture(frame->color);
    return nullptr;
  }

  frame->id = mNextId++;
  ++mMisses;
  mResidentBytes += frame->bytes;
  mPeakBytes = std::max(mPeakBytes, mResidentBytes);
  Frame* raw = frame.get();
  mOwned.push_back(std::move(frame));
  return raw;
}

void FramePool::destroyFrame(Frame* const frame) {
  if (frame == nullptr || mDevice == nullptr) return;
  if (frame->pass.valid()) mDevice->destroyRenderPass(frame->pass);
  if (frame->view.valid()) mDevice->destroyTextureView(frame->view);
  if (frame->color.valid()) mDevice->destroyTexture(frame->color);
  frame->pass = backend::RenderPassHandle::invalid();
  frame->view = backend::TextureViewHandle::invalid();
  frame->color = backend::TextureHandle::invalid();
  mResidentBytes = mResidentBytes >= frame->bytes ? mResidentBytes - frame->bytes : 0;
}

FrameLease FramePool::acquire(const FrameKey& key, std::string& error) {
  if (mDevice == nullptr || key.width == 0 || key.height == 0) {
    error = "the frame pool has no device or was asked for a zero-sized frame";
    return FrameLease();
  }

  Frame* frame = nullptr;
  const auto bucket = mFree.find(key);
  if (bucket != mFree.end() && !bucket->second.empty()) {
    frame = bucket->second.front();
    bucket->second.pop_front();
    ++mHits;
  } else {
    const uint64_t bytes = keyBytes(key);
    const bool hasRoom = mBudgetBytes == 0 || mResidentBytes + bytes <= mBudgetBytes;
    if (!hasRoom) {
      trimToBudget();
      bool nowHasRoom = mBudgetBytes == 0 || mResidentBytes + bytes <= mBudgetBytes;
      if (!nowHasRoom && mBackpressure == EffectBackpressureMode::Block && mDevice != nullptr) {
        // Offline export asks to wait rather than drop. Wait for the GPU, recycle every retired frame, and
        // try the budget once more; a graph whose transient set needs more than the budget still fails.
        ++mBackpressureWaits;
        mDevice->waitIdle();
        beginFrame();
        trimToBudget();
        nowHasRoom = mBudgetBytes == 0 || mResidentBytes + bytes <= mBudgetBytes;
      }
      if (!nowHasRoom) {
        error = "the frame pool budget is exhausted and no free frame could be evicted";
        return FrameLease();
      }
    }
    frame = createFrame(key);
    if (frame == nullptr) {
      error = "the frame pool could not allocate a " + std::to_string(key.width) + "x" +
              std::to_string(key.height) + " frame";
      return FrameLease();
    }
  }

  frame->refCount = 1;
  frame->retired = false;
  frame->submitted = false;
  frame->submission.reset();
  mInUse.push_back(frame);
  return FrameLease(shared_from_this(), frame);
}

void FramePool::beginFrame() {
  // A retired frame goes back only once the GPU has finished reading it. The fence is the whole reason
  // this is not "the lease went out of scope"; a frame that was never submitted has no GPU work to wait
  // for and is recycled immediately.
  for (auto it = mRetired.begin(); it != mRetired.end();) {
    Frame* frame = *it;
    const bool done = !frame->submitted || frame->submission == nullptr || frame->submission->isDone();
    if (!done) {
      ++it;
      continue;
    }
    frame->submission.reset();
    frame->submitted = false;
    frame->refCount = 0;
    frame->retired = false;
    ++frame->generation;
    // Back of the queue, not the front: the frame that has rested longest is the one least likely to
    // still be in the driver's hands, and the FIFO order is deterministic for tests.
    mFree[frame->key].push_back(frame);
    it = mRetired.erase(it);
  }
}

void FramePool::markSubmitted(Frame* const frame, std::shared_ptr<GpuSubmission> submission) {
  if (frame == nullptr) return;
  frame->submission = std::move(submission);
  frame->submitted = true;
}

void FramePool::release(Frame* const frame) noexcept {
  if (frame == nullptr || frame->refCount == 0) return;
  --frame->refCount;
  if (frame->refCount != 0) return;

  const auto it = std::find(mInUse.begin(), mInUse.end(), frame);
  if (it != mInUse.end()) mInUse.erase(it);
  if (!frame->retired) {
    frame->retired = true;
    mRetired.push_back(frame);
  }
}

void FramePool::trimToBudget() {
  if (mBudgetBytes == 0) return;
  while (mResidentBytes > mBudgetBytes) {
    Frame* oldest = nullptr;
    FrameKey oldestKey;
    for (auto& bucket : mFree) {
      // The free queues are FIFO, so the head is the frame that has rested longest in that bucket.
      if (bucket.second.empty()) continue;
      Frame* candidate = bucket.second.front();
      if (oldest == nullptr || candidate->generation < oldest->generation) {
        oldest = candidate;
        oldestKey = bucket.first;
      }
    }
    if (oldest == nullptr) return;
    auto& queue = mFree[oldestKey];
    queue.pop_front();
    destroyFrame(oldest);
    ++mEvictions;
    // Drop the frame from `mOwned`; the pool's destructor must not free it a second time.
    for (auto it = mOwned.begin(); it != mOwned.end(); ++it) {
      if (it->get() == oldest) {
        mOwned.erase(it);
        break;
      }
    }
    if (queue.empty()) mFree.erase(oldestKey);
  }
}

void FramePool::setBudget(const uint64_t bytes) {
  mBudgetBytes = bytes;
  trimToBudget();
}

void FramePool::waitIdle() {
  if (mDevice == nullptr) return;
  mDevice->waitIdle();
  for (const auto& frame : mOwned) {
    if (frame != nullptr) frame->submission.reset();
  }
}

FramePoolStats FramePool::stats() const {
  FramePoolStats result;
  for (const auto& bucket : mFree) result.free += static_cast<uint32_t>(bucket.second.size());
  result.inUse = static_cast<uint32_t>(mInUse.size());
  for (const Frame* frame : mRetired) {
    if (frame->submitted && frame->submission != nullptr && !frame->submission->isDone()) ++result.inFlight;
  }
  result.bytes = mResidentBytes;
  result.peakBytes = mPeakBytes;
  result.hits = mHits;
  result.misses = mMisses;
  result.evictions = mEvictions;
  result.backpressureWaits = mBackpressureWaits;
  return result;
}

// ==================================================================================================
// FrameLease
// ==================================================================================================

FrameLease::FrameLease(std::shared_ptr<FramePool> pool, Frame* const frame) noexcept
        : mPool(std::move(pool)),
          mFrame(frame) {}

FrameLease::~FrameLease() { reset(); }

FrameLease::FrameLease(FrameLease&& other) noexcept
        : mPool(std::move(other.mPool)),
          mFrame(other.mFrame) {
  other.mFrame = nullptr;
}

FrameLease& FrameLease::operator=(FrameLease&& other) noexcept {
  if (this == &other) return *this;
  reset();
  mPool = std::move(other.mPool);
  mFrame = other.mFrame;
  other.mFrame = nullptr;
  return *this;
}

FrameView FrameLease::view() const noexcept {
  return mFrame != nullptr ? mFrame->frameView() : FrameView {};
}

void FrameLease::markSubmitted(std::shared_ptr<GpuSubmission> submission) {
  if (mPool != nullptr && mFrame != nullptr) mPool->markSubmitted(mFrame, std::move(submission));
}

void FrameLease::reset() noexcept {
  if (mPool != nullptr && mFrame != nullptr) mPool->release(mFrame);
  mFrame = nullptr;
  mPool.reset();
}

// ==================================================================================================
// RenderCore
// ==================================================================================================

RenderCore::RenderCore() = default;

RenderCore::~RenderCore() { shutdown(); }

backend::Backend RenderCore::mapBackend(const EffectBackend backend) const noexcept {
  switch (backend) {
    case EffectBackend::Default:
      return backend::Backend::Null;
    case EffectBackend::OpenGL:
      return backend::Backend::OpenGL;
    case EffectBackend::Vulkan:
      return backend::Backend::Vulkan;
    case EffectBackend::Metal:
      return backend::Backend::Metal;
  }
  return backend::Backend::Null;
}

namespace {

/** Collects the device factory's warnings and errors into the core's diagnostics. */
struct DeviceLog {
  std::string text;
};

void collectDeviceMessage(const backend::MessageSeverity severity, const char* const message, void* const userData) {
  if (message == nullptr || userData == nullptr) return;
  if (severity != backend::MessageSeverity::Error && severity != backend::MessageSeverity::Warning &&
      severity != backend::MessageSeverity::Fatal) {
    return;
  }
  auto* log = static_cast<DeviceLog*>(userData);
  if (!log->text.empty()) log->text += '\n';
  log->text += message;
}

} // namespace anonymous

bool RenderCore::initialize(const EffectEngineConfig& config, const backend::SurfaceDescriptor* surface,
                            std::string& error) {
  if (mDevice != nullptr) {
    error = "the render core is already initialised";
    return false;
  }

  backend::DeviceConfiguration configuration;
  configuration.preferredBackend = mapBackend(config.backend);
  configuration.allowFallback = true;
  configuration.enableValidation = config.enableValidation;
  configuration.enableGpuTiming = config.enableGpuTiming;
  configuration.pipelineCachePath = config.vulkanPipelineCachePath;
  // `sharedContext` is the platform's own context (an EGLContext on Android). The GL backend takes it
  // through the surface descriptor's `glShareContext`; `shareContext` is kept for backends that name it
  // differently.
  backend::SurfaceDescriptor surfaceCopy;
  if (surface != nullptr) {
    surfaceCopy = *surface;
    surfaceCopy.glShareContext = config.sharedContext;
    if (config.eglContext != nullptr) {
      surfaceCopy.eglDisplay = static_cast<decltype(surfaceCopy.eglDisplay)>(config.eglDisplay);
      surfaceCopy.eglConfig = static_cast<decltype(surfaceCopy.eglConfig)>(config.eglConfig);
      surfaceCopy.eglContext = static_cast<decltype(surfaceCopy.eglContext)>(config.eglContext);
    }
    configuration.surface = &surfaceCopy;
  } else {
    configuration.surface = nullptr;
  }
  configuration.shareContext = config.sharedContext;
  configuration.maxFramesInFlight = std::max(1u, std::min(4u, config.maxFramesInFlight));
  configuration.appName = "coral-effect";

  if (config.externalDevice != nullptr) {
    // Model B's first half: the caller owns the device and has already released its context from the
    // calling thread. `shutdown()` must not destroy it, hence the no-op deleter.
    mDevice = std::shared_ptr<backend::Device>(config.externalDevice, [](backend::Device*) {});
    mDiagnostics = "the effect engine is using an external device";
  } else {
    DeviceLog log;
    backend::Device* device = nullptr;
    const backend::Result created = backend::createDevice(configuration, &device, collectDeviceMessage, &log);
    if (!backend::isSuccess(created) || device == nullptr) {
      error = "no graphics device could be brought up for the effect engine (" +
              std::to_string(static_cast<int>(created)) + ")";
      if (!log.text.empty()) error += ": " + log.text;
      return false;
    }
    mDiagnostics = std::move(log.text);
    mDevice.reset(device);
  }
  ++mDeviceGeneration;
  mTimeBase = config.timeBase != 0 ? config.timeBase : RenderTime::kDefaultTimeBase;
  mMaxFramesInFlight = std::max(1u, std::min(4u, config.maxFramesInFlight));
  mAsyncShaderCompilation = config.asyncShaderCompilation;
  mAsyncPipelineCreation = config.asyncPipelineCreation;
  mTextureCacheBudgetBytes = config.textureCacheBudgetBytes;
  mBackpressure = config.backpressureMode;
  const backend::Result currentResult = mDevice->makeCurrent();
  if (!backend::isSuccess(currentResult)) {
    error = "the graphics device could not make its context current (" +
            std::to_string(static_cast<int>(currentResult)) + ")";
    if (!mDiagnostics.empty()) error += ": " + mDiagnostics;
    // No shared resources exist yet, but the device may be in a partially initialized platform
    // state. Do not let the render thread continue into shader linking with no current context.
    if (config.externalDevice == nullptr) {
      mDevice->releaseCurrent();
    }
    mDevice.reset();
    return false;
  }
  if (mAsyncShaderCompilation && mDevice->getInfo().acceptsSpirv && backend::isShaderCompilerAvailable()) {
    mShaderPool = std::make_unique<ShaderCompilePool>();
    std::string poolError;
    if (!mShaderPool->start(poolError)) {
      mShaderPool.reset();
      mAsyncShaderCompilation = false;
    }
  } else {
    mAsyncShaderCompilation = false;
  }

  // ## Warm the compiler now, not inside the first frame
  //
  // The first compile on a thread pays ~45 ms for the GLSL compiler and its built-in symbol table. Doing it here
  // moves that cost into `start()`, which is before anything is on screen, instead of into the first frame - the
  // frame that a user actually waits for.
  //
  // The gate is "this engine will compile GLSL to SPIR-V", not "the device can accept SPIR-V". The OpenGL backend
  // *can* accept SPIR-V - it cross-compiles it - but a GL program in this engine goes down `GLSLDirect` and hands
  // its source to the driver, so shaderc never runs. Gating on the capability alone made a GLES device pay 82 ms
  // of start-up for a compiler it would never use, which is how this comment came to exist.
  //
  // The compile worker has its own compiler (it is per thread), so it is warmed by a task rather than by a call
  // here, and the task is not waited for: if the first shader turns out to be a cache hit the compiler is never
  // needed, and the warm-up is then happening off the critical path rather than in front of it.
  const bool compilesFromSpirv = !mDevice->getInfo().acceptsGlslDirect;
  if (config.warmUpShaderCompiler && compilesFromSpirv && backend::isShaderCompilerAvailable()) {
    backend::warmUpShaderCompiler();
    // The worker's warm-up is a hint, not a requirement: if the pool cannot take the task the render thread's own
    // warm-up above already covered the compiler that will be used here.
    if (mShaderPool != nullptr) static_cast<void>(submitShaderCompile([] { backend::warmUpShaderCompiler(); }));
  }

  if (!createSharedResources(error)) {
    destroySharedResources();
    mDevice.reset();
    return false;
  }

  mPool = std::make_shared<FramePool>(mDevice.get(), config.framePoolBudgetBytes, 1, config.backpressureMode);

  mSlotCount = mMaxFramesInFlight;
  mSlots.resize(mSlotCount);
  for (uint32_t index = 0; index < mSlotCount; ++index) {
    backend::CommandBufferDescriptor descriptor;
    descriptor.transient = true;
    descriptor.label = "effect/frame";
    backend::CommandBuffer* commands = nullptr;
    if (!backend::isSuccess(mDevice->createCommandBuffer(descriptor, &commands)) || commands == nullptr) {
      error = "the effect engine could not create a command buffer";
      destroySharedResources();
      mDevice.reset();
      return false;
    }
    mSlots[index].commands = commands;
  }
  if (config.enableGpuTiming && mDevice->getCapabilities().supports(backend::Feature::TimestampQuery)) {
    mGpuTimingElapsed = mDevice->getInfo().backend == backend::Backend::OpenGL;
    backend::QuerySetDescriptor descriptor;
    descriptor.type = backend::QueryType::Timestamp;
    descriptor.count = mSlotCount * 2;
    descriptor.label = "effect/frame-timestamps";
    backend::QuerySetHandle query;
    if (backend::isSuccess(mDevice->createQuerySet(descriptor, &query))) mTimestampQuery = query;
  }
  return true;
}

bool RenderCore::createSharedResources(std::string& error) {
  if (mDevice == nullptr) {
    error = "no device";
    return false;
  }

  float quad[kFullTargetVertexCount * 4];
  fillFullTargetQuad(quad);
  const backend::BufferDescriptor quadDescriptor{
    .size = sizeof(quad),
    .usage = static_cast<backend::BufferUsages>(backend::BufferUsage::Vertex),
    .memoryUsage = backend::MemoryUsage::GpuOnly,
    .initialData = quad,
    .label = "effect/quad",
  };
  if (!backend::isSuccess(mDevice->createBuffer(quadDescriptor, &mQuad))) {
    error = "the effect engine could not create its full-screen quad";
    return false;
  }

  const backend::TextureDescriptor params =
      backend::TextureDescriptor::texture2D(kParameterSlotCount, 1, backend::PixelFormat::R32G32B32A32_SFLOAT,
                                            static_cast<backend::TextureUsages>(backend::TextureUsage::Sampled));
  if (!backend::isSuccess(mDevice->createTexture(params, &mParamsTexture))) {
    error = "the effect engine could not create its parameter texture";
    return false;
  }

  if (!backend::isSuccess(mDevice->createSampler(backend::SamplerDescriptor::linearClamp(), &mLinearSampler)) ||
      !backend::isSuccess(mDevice->createSampler(backend::SamplerDescriptor::nearestClamp(), &mNearestSampler))) {
    error = "the effect engine could not create its samplers";
    return false;
  }

  // Zeroed once, so a shader that somehow runs before its pass wrote the block reads zeros rather than
  // whatever the allocation held.
  float zeros[kParameterComponentCount] = {};
  const bool written = executeTemporary(
      [&](backend::CommandBuffer& commands) {
        backend::TextureCopyRegion region;
        region.textureOrigin = { 0, 0, 0 };
        region.size = { kParameterSlotCount, 1, 1 };
        region.mipLevel = 0;
        region.baseArrayLayer = 0;
        region.arrayLayerCount = 1;
        commands.writeTexture(mParamsTexture, region, zeros, sizeof(zeros));
      },
      true, error);
  if (!written) return false;
  return true;
}

void RenderCore::destroySharedResources() noexcept {
  if (mDevice == nullptr) return;

  for (const auto& entry : mImages) {
    if (entry.second.texture.valid()) mDevice->destroyTexture(entry.second.texture);
  }
  mImages.clear();
  for (const auto& entry : mSolids) {
    if (entry.second.texture.valid()) mDevice->destroyTexture(entry.second.texture);
  }
  mSolids.clear();

  // Programs first: they own pipeline/descriptor state that names the device.
  mPrograms.clear();
  if (mTimestampQuery.valid()) {
    mDevice->destroyQuerySet(mTimestampQuery);
    mTimestampQuery = backend::QuerySetHandle::invalid();
  }

  for (Slot& slot : mSlots) {
    if (slot.fence.valid()) mDevice->destroyFence(slot.fence);
    slot.fence = backend::FenceHandle::invalid();
    if (slot.commands != nullptr) mDevice->destroyCommandBuffer(slot.commands);
    slot.commands = nullptr;
    slot.submission.reset();
  }
  mSlots.clear();
  mRecording = nullptr;

  mPool.reset();

  for (backend::TextureHandle& texture : mYuvTextures) {
    if (texture.valid()) mDevice->destroyTexture(texture);
    texture = backend::TextureHandle::invalid();
  }
  mYuvProgram = nullptr;
  mYuvBt709 = false;
  mYuvFullRange = false;
  mYuvP010 = false;
  mYuvHdr = false;
  mYuvWidth = 0;
  mYuvHeight = 0;
  if (mQuad.valid()) mDevice->destroyBuffer(mQuad);
  if (mParamsTexture.valid()) mDevice->destroyTexture(mParamsTexture);
  if (mLinearSampler.valid()) mDevice->destroySampler(mLinearSampler);
  if (mNearestSampler.valid()) mDevice->destroySampler(mNearestSampler);
  for (const auto& entry : mYcbcrSamplers) {
    if (entry.second.valid()) mDevice->destroySampler(entry.second);
  }
  mYcbcrSamplers.clear();
  mQuad = backend::BufferHandle::invalid();
  mParamsTexture = backend::TextureHandle::invalid();
  mLinearSampler = backend::TextureSamplerHandle::invalid();
  mNearestSampler = backend::TextureSamplerHandle::invalid();
}

void RenderCore::shutdown() {
  if (mDevice == nullptr) return;
  if (mShaderPool != nullptr) {
    mShaderPool->stop();
    mShaderPool.reset();
    mAsyncShaderCompilation = false;
  }
  waitIdle();
  for (PendingCompletion& pending : mPendingCompletions) pending.completion->signal(false);
  mPendingCompletions.clear();
  destroySharedResources();
  ++mDeviceGeneration;
  mDevice.reset();
}

void RenderCore::beginFrame() {
  if (mDevice == nullptr) return;

  mDevice->pollAsyncOperations();
  // A completed submission frees its command buffer for reuse; the frame pool then recycles the frames
  // whose own submissions are done. Both checks are fence polls, never waits.
  for (Slot& slot : mSlots) {
    if (slot.submission != nullptr && slot.submission->isDone()) {
      if (mTimestampQuery.valid()) {
        const uint32_t query = static_cast<uint32_t>(&slot - mSlots.data()) * 2;
        if (mGpuTimingElapsed) {
          uint64_t elapsed = 0;
          if (backend::isSuccess(mDevice->getQueryResults(mTimestampQuery, query, 1, &elapsed, sizeof(elapsed)))) {
            mGpuMilliseconds = static_cast<double>(elapsed) / 1.0e6;
          }
        } else {
          uint64_t timestamps[2] = {};
          if (backend::isSuccess(mDevice->getQueryResults(mTimestampQuery, query, 2, timestamps, sizeof(timestamps))) &&
              timestamps[1] >= timestamps[0]) {
            mGpuMilliseconds = static_cast<double>(timestamps[1] - timestamps[0]) / 1.0e6;
          }
        }
      }
      if (slot.commands != nullptr) slot.commands->reset();
      slot.submission.reset();
    }
    if (slot.submission == nullptr && slot.fence.valid()) {
      mDevice->destroyFence(slot.fence);
      slot.fence = backend::FenceHandle::invalid();
    }
  }
  if (mPool != nullptr) mPool->beginFrame();
  flushRetired();
  ++mFrameCounter;

  // Image textures not touched for a full submission ring are no longer referenced by an in-flight frame.
  // Keeping a small tail avoids re-uploading an image that alternates with another one.
  const uint64_t retainFrames = static_cast<uint64_t>(std::max(2u, mMaxFramesInFlight + 2u));
  for (auto it = mImages.begin(); it != mImages.end();) {
    if (mFrameCounter > it->second.lastUsedFrame + retainFrames) {
      if (it->second.texture.valid()) mDevice->destroyTexture(it->second.texture);
      ++mImageEvictions;
      it = mImages.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = mSolids.begin(); it != mSolids.end();) {
    if (mFrameCounter > it->second.lastUsedFrame + retainFrames) {
      if (it->second.texture.valid()) mDevice->destroyTexture(it->second.texture);
      ++mSolidEvictions;
      it = mSolids.erase(it);
    } else {
      ++it;
    }
  }

  if (mTextureCacheBudgetBytes != 0) {
    size_t total = mSolids.size() * 4;
    for (const auto& entry : mImages) total += entry.second.bytes;
    if (total > mTextureCacheBudgetBytes) {
      std::vector<const EffectImage*> oldest;
      oldest.reserve(mImages.size());
      for (const auto& entry : mImages) oldest.push_back(entry.first);
      std::sort(oldest.begin(), oldest.end(), [this](const EffectImage* a, const EffectImage* b) {
        return mImages.at(a).lastUsedFrame < mImages.at(b).lastUsedFrame;
      });
      for (const EffectImage* key : oldest) {
        if (total <= mTextureCacheBudgetBytes) break;
        const auto it = mImages.find(key);
        if (it == mImages.end()) continue;
        if (it->second.texture.valid()) mDevice->destroyTexture(it->second.texture);
        total -= it->second.bytes;
        ++mImageEvictions;
        mImages.erase(it);
      }
      std::vector<uint32_t> oldestSolids;
      oldestSolids.reserve(mSolids.size());
      for (const auto& entry : mSolids) oldestSolids.push_back(entry.first);
      std::sort(oldestSolids.begin(), oldestSolids.end(), [this](const uint32_t a, const uint32_t b) {
        return mSolids.at(a).lastUsedFrame < mSolids.at(b).lastUsedFrame;
      });
      for (const uint32_t key : oldestSolids) {
        if (total <= mTextureCacheBudgetBytes) break;
        const auto it = mSolids.find(key);
        if (it == mSolids.end()) continue;
        if (it->second.texture.valid()) mDevice->destroyTexture(it->second.texture);
        total -= 4;
        ++mSolidEvictions;
        mSolids.erase(it);
      }
    }
  }
  mTextureCachePeakBytes = std::max(mTextureCachePeakBytes, textureCacheBytes());
}

bool RenderCore::hasFreeSlot() const noexcept {
  for (const Slot& slot : mSlots) {
    if (slot.submission == nullptr && !slot.recording) return true;
  }
  return false;
}

RenderCore::Slot* RenderCore::acquireSlot(const bool waitForSlot) {
  for (uint32_t offset = 0; offset < mSlotCount; ++offset) {
    Slot& slot = mSlots[(mNextSlot + offset) % mSlotCount];
    if (slot.submission == nullptr && !slot.recording) {
      mNextSlot = (mNextSlot + offset + 1) % mSlotCount;
      return &slot;
    }
  }
  if (!waitForSlot || mSlotCount == 0) return nullptr;

  // Every slot is still in flight: waiting on one is the back-pressure that keeps
  // `maxFramesInFlight` meaningful. This is a GPU wait, not a device idle.
  Slot& slot = mSlots[mNextSlot % mSlotCount];
  if (slot.submission != nullptr) {
    slot.submission->wait();
    if (slot.commands != nullptr) slot.commands->reset();
    slot.submission.reset();
  }
  mNextSlot = (mNextSlot + 1) % mSlotCount;
  return &slot;
}

backend::CommandBuffer* RenderCore::beginRecording(const bool waitForSlot) {
  if (mRecording != nullptr) return mRecording->commands;
  Slot* slot = acquireSlot(waitForSlot);
  if (slot == nullptr || slot->commands == nullptr) return nullptr;
  // The fence is created before recording begins. On the Vulkan backend a fence created while a command
  // buffer of the same device is in the ended-but-not-submitted state faulted in the driver, and creating
  // it first is both legal and one fewer thing between a finished recording and its submission.
  if (!slot->fence.valid()) {
    backend::FenceDescriptor descriptor;
    descriptor.label = "effect/frame";
    if (!backend::isSuccess(mDevice->createFence(descriptor, &slot->fence))) return nullptr;
  }
  if (!backend::isSuccess(slot->commands->begin())) {
    mDevice->destroyFence(slot->fence);
    slot->fence = backend::FenceHandle::invalid();
    return nullptr;
  }
  if (mTimestampQuery.valid()) {
    const uint32_t query = static_cast<uint32_t>(slot - mSlots.data()) * 2;
    if (backend::isSuccess(slot->commands->resetQuerySet(mTimestampQuery, query, 2))) {
      if (mGpuTimingElapsed) {
        slot->commands->beginQuery(mTimestampQuery, query);
      } else {
        slot->commands->endQuery(mTimestampQuery, query);
      }
    }
  }
  slot->recording = true;
  mRecording = slot;
  return slot->commands;
}

bool RenderCore::submitFrame(std::shared_ptr<GpuSubmission>& submission, std::string& error,
                             std::shared_ptr<FrameCompletion> completion) {
  if (mRecording == nullptr || mRecording->commands == nullptr) {
    error = "there is no recorded frame to submit";
    return false;
  }
  Slot* slot = mRecording;
  mRecording = nullptr;
  slot->recording = false;

  backend::FenceHandle fence = slot->fence;
  slot->fence = backend::FenceHandle::invalid();

  if (mTimestampQuery.valid()) {
    const uint32_t query = static_cast<uint32_t>(slot - mSlots.data()) * 2;
    slot->commands->endQuery(mTimestampQuery, mGpuTimingElapsed ? query : query + 1);
  }
  if (!backend::isSuccess(slot->commands->end())) {
    if (fence.valid()) mDevice->destroyFence(fence);
    error = "the frame's command buffer refused to end recording";
    return false;
  }
  if (!fence.valid()) {
    error = "the frame has no completion fence";
    return false;
  }

  backend::Queue* queue = mDevice->graphicsQueue();
  if (queue == nullptr) {
    mDevice->destroyFence(fence);
    error = "the device exposes no graphics queue";
    return false;
  }
  if (!backend::isSuccess(queue->submit(*slot->commands, fence))) {
    mDevice->destroyFence(fence);
    error = "the device refused the frame submission";
    return false;
  }

  submission = std::make_shared<GpuSubmission>(mDevice.get(), fence, completion);
  slot->submission = submission;
  if (!mPendingRetired.empty()) {
    mRetiredSubmissions.push_back({ submission, std::move(mPendingRetired) });
    mPendingRetired.clear();
  }
  if (completion != nullptr) mPendingCompletions.push_back({ completion, submission });
  return true;
}

void RenderCore::pollCompletions() {
  for (auto it = mRetiredSubmissions.begin(); it != mRetiredSubmissions.end();) {
    const std::shared_ptr<GpuSubmission> submission = it->submission.lock();
    if (submission == nullptr || submission->isDone()) {
      for (std::function<void()>& task : it->tasks) {
        if (task) task();
      }
      it = mRetiredSubmissions.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = mPendingCompletions.begin(); it != mPendingCompletions.end();) {
    const std::shared_ptr<GpuSubmission> submission = it->submission.lock();
    if (submission == nullptr) {
      // The pool only releases a submission after its fence has completed; if it is already gone, the
      // frame is done.
      it->completion->signal(true);
      it = mPendingCompletions.erase(it);
    } else if (submission->isDone()) {
      it->completion->signal(true);
      it = mPendingCompletions.erase(it);
    } else {
      ++it;
    }
  }
}

bool RenderCore::hasPendingCompletions() const noexcept { return !mPendingCompletions.empty(); }

void RenderCore::abortRecording() noexcept {
  if (mRecording == nullptr) return;
  if (mRecording->commands != nullptr) mRecording->commands->reset();
  if (mRecording->fence.valid()) {
    mDevice->destroyFence(mRecording->fence);
    mRecording->fence = backend::FenceHandle::invalid();
  }
  mRecording->recording = false;
  mRecording = nullptr;
}

void RenderCore::waitIdle() {
  // The command buffers go first: on Vulkan each one remembers the fence its submission was given and
  // waits on it when reset, so a fence destroyed before that reset is a wait on a freed handle.
  if (mDevice != nullptr) mDevice->waitIdle();
  for (Slot& slot : mSlots) {
    if (slot.submission != nullptr) {
      if (slot.commands != nullptr) slot.commands->reset();
      slot.submission.reset();
    }
    if (slot.fence.valid()) {
      mDevice->destroyFence(slot.fence);
      slot.fence = backend::FenceHandle::invalid();
    }
  }
  if (mPool != nullptr) mPool->waitIdle();
  flushRetired();
  if (mDevice != nullptr) mDevice->waitIdle();
}

backend::CommandBuffer* RenderCore::temporaryCommands(std::string& error) {
  backend::CommandBufferDescriptor descriptor;
  descriptor.transient = true;
  descriptor.oneTimeSubmit = true;
  descriptor.label = "effect/temp";
  backend::CommandBuffer* commands = nullptr;
  if (!backend::isSuccess(mDevice->createCommandBuffer(descriptor, &commands)) || commands == nullptr) {
    error = "a temporary command buffer could not be created";
    return nullptr;
  }
  mTemporaryBuffers.push_back(commands);
  if (!backend::isSuccess(commands->begin())) {
    error = "a temporary command buffer refused to begin";
    mDevice->destroyCommandBuffer(commands);
    mTemporaryBuffers.erase(std::remove(mTemporaryBuffers.begin(), mTemporaryBuffers.end(), commands),
                            mTemporaryBuffers.end());
    return nullptr;
  }
  return commands;
}

bool RenderCore::executeTemporary(const std::function<void(backend::CommandBuffer&)>& record, const bool wait,
                                  std::string& error) {
  if (mDevice == nullptr) {
    error = "no device";
    return false;
  }
  backend::CommandBuffer* commands = temporaryCommands(error);
  if (commands == nullptr) return false;

  // The fence exists before a single command is recorded; see the note in `beginRecording`.
  backend::FenceHandle fence = backend::FenceHandle::invalid();
  backend::FenceDescriptor fenceDescriptor;
  fenceDescriptor.flag = backend::FenceFlag::Unsignalled;
  fenceDescriptor.label = "effect/temp";
  if (!backend::isSuccess(mDevice->createFence(fenceDescriptor, &fence))) {
    error = "a temporary fence could not be created";
    mDevice->destroyCommandBuffer(commands);
    mTemporaryBuffers.erase(std::remove(mTemporaryBuffers.begin(), mTemporaryBuffers.end(), commands),
                            mTemporaryBuffers.end());
    return false;
  }

  record(*commands);
  const bool ended = backend::isSuccess(commands->end());
  backend::Queue* queue = mDevice->graphicsQueue();
  const bool submitted = ended && queue != nullptr && backend::isSuccess(queue->submit(*commands, fence));
  if (submitted) {
    mDevice->waitForFence(fence);
  } else if (error.empty()) {
    error = "a temporary submission was refused";
  }
  (void) wait;

  // The command buffer goes first. The Vulkan backend records the submission fence in the command buffer
  // and waits for it when the buffer is reset or destroyed, so destroying the fence first would make the
  // backend wait on a handle it no longer owns.
  mDevice->destroyCommandBuffer(commands);
  mDevice->destroyFence(fence);
  mTemporaryBuffers.erase(std::remove(mTemporaryBuffers.begin(), mTemporaryBuffers.end(), commands),
                          mTemporaryBuffers.end());
  return submitted;
}
uint64_t RenderCore::textureCacheBytes() const noexcept {
  uint64_t total = mSolids.size() * 4u;
  for (const auto& entry : mImages) total += entry.second.bytes;
  return total;
}

void RenderCore::setExternalTextureResolver(std::shared_ptr<EffectExternalTextureResolver> resolver) {
  mExternalTextureResolver = std::move(resolver);
}

backend::TextureHandle RenderCore::resolveExternalTexture(const EffectExternalTexture& texture, std::string& error) {
  if (mExternalTextureResolver == nullptr) {
    error = "no external texture resolver is registered";
    return backend::TextureHandle::invalid();
  }
  uint64_t resolvedVersion = 0;
  const uint64_t token = mExternalTextureResolver->resolve(texture, resolvedVersion, error);
  if (token == 0) {
    if (error.empty()) error = "the external texture resolver returned no texture";
    return backend::TextureHandle::invalid();
  }
  return backend::TextureHandle(static_cast<uint32_t>(token & backend::TextureHandle::kIndexMask),
                                static_cast<uint32_t>(token >> backend::TextureHandle::kIndexBits));
}

void RenderCore::retireAfterGpu(std::function<void()> task) {
  if (!task) return;
  if (mRecording != nullptr) mPendingRetired.push_back(std::move(task));
  else mRetired.push_back(std::move(task));
}

void RenderCore::flushRetired() {
  for (const Slot& slot : mSlots) {
    if (slot.submission != nullptr) return;
  }
  for (RetiredSubmission& retired : mRetiredSubmissions) {
    for (std::function<void()>& task : retired.tasks) {
      if (task) task();
    }
  }
  mRetiredSubmissions.clear();
  for (std::function<void()>& task : mPendingRetired) {
    if (task) task();
  }
  mPendingRetired.clear();
  std::vector<std::function<void()>> tasks;
  tasks.swap(mRetired);
  for (std::function<void()>& task : tasks) {
    if (task) task();
  }
}

void RenderCore::trimPrograms(const uint32_t maxPrograms) {
  if (mPrograms.size() <= maxPrograms) return;
  for (const Slot& slot : mSlots) {
    if (slot.submission != nullptr) return;
  }
  const uint64_t retainFrames = static_cast<uint64_t>(std::max(2u, mMaxFramesInFlight + 2u));
  for (auto it = mPrograms.begin(); it != mPrograms.end();) {
    if (it->second->hasPendingPipeline()) {
      ++it;
      continue;
    }
    if (mFrameCounter > it->second->lastUsedFrame + retainFrames) {
      ++mProgramEvictions;
      std::shared_ptr<RenderProgram> retired = std::move(it->second);
      it = mPrograms.erase(it);
      // Pipeline/descriptor destruction is deferred to the same device-free queue as runtime releases.
      retireAfterGpu([program = std::move(retired)]() mutable { program.reset(); });
    } else {
      ++it;
    }
  }
}

RenderProgram* RenderCore::outputScaleProgram(const EffectRotation rotation, const bool mirrorHorizontal,
                                              const bool mirrorVertical, const EffectScaleMode scaleMode,
                                              std::string& error) {
  std::string body = "  vec2 t = vec2(uv.x, 1.0 - uv.y);\n";
  if (scaleMode == EffectScaleMode::Fit || scaleMode == EffectScaleMode::Crop) {
    body += "  vec2 sourceSize = effectSourceSize().xy;\n";
    body += "  vec2 targetSize = effectTargetSize().xy;\n";
    body += scaleMode == EffectScaleMode::Fit
                ? "  float fitScale = min(targetSize.x / sourceSize.x, targetSize.y / sourceSize.y);\n"
                : "  float fitScale = max(targetSize.x / sourceSize.x, targetSize.y / sourceSize.y);\n";
    body += "  vec2 content = vec2(sourceSize.x * fitScale / targetSize.x, sourceSize.y * fitScale / targetSize.y);\n";
    body += "  vec2 offset = (1.0 - content) * 0.5;\n";
    if (scaleMode == EffectScaleMode::Fit) {
      body += "  if (t.x < offset.x || t.x > offset.x + content.x || t.y < offset.y || t.y > offset.y + content.y) {\n";
      body += "    return effectSlot(2);\n";
      body += "  }\n";
    }
    body += "  t = (t - offset) / content;\n";
  }
  if (mirrorHorizontal) body += "  t.x = 1.0 - t.x;\n";
  if (mirrorVertical) body += "  t.y = 1.0 - t.y;\n";
  body += "  vec2 s = t;\n";
  switch (rotation) {
    case EffectRotation::Clockwise90:
      body += "  s = vec2(1.0 - t.y, t.x);\n";
      break;
    case EffectRotation::Clockwise180:
      body += "  s = vec2(1.0 - t.x, 1.0 - t.y);\n";
      break;
    case EffectRotation::Clockwise270:
      body += "  s = vec2(t.y, 1.0 - t.x);\n";
      break;
    case EffectRotation::None:
    default:
      break;
  }
  body += "  return texture(uInput0, vec2(s.x, 1.0 - s.y));";

  auto snapshot = std::make_shared<ProgramSnapshot>();
  snapshot->desc.name = "effect/output-scale";
  snapshot->desc.fragmentSource = std::move(body);
  snapshot->desc.inputCount = 1;
  snapshot->hash = detail::hashProgram(snapshot->desc, snapshot->parameters, snapshot->textureNames);
  return programFor(snapshot, error);
}

void RenderCore::recordPipelineCreation(const double milliseconds) noexcept {
  ++mPipelineCreationCount;
  mPipelineCreationMilliseconds += milliseconds;
}

bool RenderCore::canCompileAsync() const noexcept {
  return mAsyncShaderCompilation && mShaderPool != nullptr && mShaderPool->running() && mDevice != nullptr &&
         mDevice->getInfo().acceptsSpirv && backend::isShaderCompilerAvailable();
}

bool RenderCore::submitShaderCompile(std::function<void()> task) {
  return mShaderPool != nullptr && mShaderPool->submit(std::move(task));
}

bool RenderCore::topDownTargets() const noexcept {
  return mDevice != nullptr && (mDevice->getInfo().backend == backend::Backend::Vulkan ||
                                mDevice->getInfo().backend == backend::Backend::Metal);
}

bool RenderCore::convertYuv420(backend::CommandBuffer* const commands, const uint32_t width,
        const uint32_t height, const uint8_t* const y, const uint32_t yRowBytes, const uint8_t* const u,
        const uint32_t uRowBytes, const uint8_t* const v, const uint32_t vRowBytes, const bool bt709,
        const bool fullRange, const bool p010, const bool hdr, const EffectHdrTransfer hdrTransfer,
        const EffectPixelFormat targetFormat, const EffectToneMapMode toneMapMode, const float exposure,
        const float paperWhiteNits, const float hdr10PlusTargetedNits, const float hdr10PlusAverageMaxRgb,
        const float hdr10PlusFractionBrightPixels, const float hdrMaxCll, const float hdrMaxFall,
        FrameLease& out, std::string& error) {
  if (commands == nullptr || mDevice == nullptr || y == nullptr || u == nullptr || v == nullptr) {
    error = "the YUV conversion has no command buffer or planes";
    return false;
  }
  if (mYuvProgram == nullptr || mYuvBt709 != bt709 || mYuvFullRange != fullRange ||
      mYuvP010 != p010 || mYuvHdr != hdr || mYuvHdrTransfer != hdrTransfer ||
      mYuvToneMap != toneMapMode || mYuvExposure != exposure || mYuvPaperWhite != paperWhiteNits) {
    auto snapshot = std::make_shared<ProgramSnapshot>();
    snapshot->desc.name = "effect/yuv420-gpu";
    snapshot->desc.authoring = EffectShaderAuthoring::Body;
    snapshot->desc.inputCount = 3;
    snapshot->parameters.push_back({ "hdr10TargetedNits", EffectValueType::Float, 0, 1 });
    snapshot->parameters.push_back({ "hdr10AverageMaxRgb", EffectValueType::Float, 1, 1 });
    snapshot->parameters.push_back({ "hdr10FractionBright", EffectValueType::Float, 2, 1 });
    snapshot->parameters.push_back({ "hdrMaxCll", EffectValueType::Float, 3, 1 });
    snapshot->parameters.push_back({ "hdrMaxFall", EffectValueType::Float, 4, 1 });
    std::string body;
    if (p010) {
      body = "      float yy = (texture(uInput0, uv).r * 255.0 - 16.0) * 1.164;\n"
             "      float uu = texture(uInput1, uv).r * 255.0 - 128.0;\n"
             "      float vv = texture(uInput2, uv).r * 255.0 - 128.0;\n";
    } else {
      body = std::string("      float yy = (texture(uInput0, uv).r * 255.0 - ") +
             (fullRange ? "0.0" : "16.0") + ") * " + (fullRange ? "1.0" : "1.164") + ";\n"
             "      float uu = texture(uInput1, uv).r * 255.0 - 128.0;\n"
             "      float vv = texture(uInput2, uv).r * 255.0 - 128.0;\n";
    }
    if (hdr) {
      const std::string chromaDen = fullRange ? "255.0" : "224.0";
      body += std::string("      vec3 yuv = clamp(vec3(yy / 255.0, ") +
              "(texture(uInput1, uv).r * 255.0 - 128.0) / " + chromaDen + ", " +
              "(texture(uInput2, uv).r * 255.0 - 128.0) / " + chromaDen + ")," +
              " vec3(0.0, -0.5, -0.5), vec3(1.0, 0.5, 0.5));\n";
      body += "      vec3 rgb = vec3(yuv.x + 1.4746 * yuv.z, yuv.x - 0.16455 * yuv.y - 0.57135 * yuv.z,"
              " yuv.x + 1.8814 * yuv.y);\n";
      if (hdrTransfer == EffectHdrTransfer::HLG) {
        body +=
            "      float a = 0.17883277;\n"
            "      float b = 0.28466892;\n"
            "      float cc = 0.55991073;\n"
            "      vec3 hlg = clamp(rgb, 0.0, 1.0);\n"
            "      vec3 signal = mix(hlg * hlg / 3.0, (exp((hlg - cc) / a) + b) / 12.0,"
            " step(vec3(0.5), hlg));\n"
            "      vec3 linear = signal * 1000.0;\n"
            "      float hlgY = dot(linear, vec3(0.2627, 0.6780, 0.0593)) / 1000.0;\n"
            "      linear *= pow(max(hlgY, 0.0), 0.2);\n";
      } else {
        body +=
            "      vec3 eotf = pow(clamp(rgb, 0.0, 1.0), vec3(1.0 / 78.84375));\n"
            "      vec3 linear = 10000.0 * pow(max(eotf - 0.8359375, vec3(0.0)) /"
            " (18.8515625 - 18.6875 * eotf), vec3(1.0 / 0.1593017578));\n";
      }
      if (targetFormat == EffectPixelFormat::RGBA16F) {
        body += "      return vec4(linear / 10000.0, 1.0);\n";
      } else {
        body += "      float hdrTarget = effectParams(0).x;\n"
                "      float hdrAvg = effectParams(0).y;\n"
                "      float paper = hdrTarget > 0.0 ? clamp(hdrTarget * 0.5, 80.0, 400.0) : " +
                std::to_string(paperWhiteNits) + ";\n"
                "      float hdrFrac = effectParams(0).z;\n"
                "      float maxCll = effectParams(0).w;\n"
                "      float maxFall = effectParams(1).x;\n"
                "      float expo = " + std::to_string(exposure) +
                " * (hdrAvg > 0.0 ? mix(1.0, 1.3, clamp(hdrAvg * 8.0, 0.0, 1.0)) : 1.0);\n"
                "      expo *= mix(1.0, 0.9, clamp(maxFall / 1000.0, 0.0, 1.0));\n"
                "      expo = mix(expo, expo * 1.1, clamp(hdrFrac * 20.0, 0.0, 1.0));\n"
                "      paper *= mix(1.0, 1.2, clamp(maxCll / 1000.0, 0.0, 1.0));\n";
        if (toneMapMode == EffectToneMapMode::Aces) {
          body += "      vec3 x = linear * expo / paper;\n"
                  "      vec3 num = x * (2.51 * x + 0.03);\n"
                  "      vec3 den = x * (2.43 * x + 0.59) + 0.14;\n"
                  "      return vec4(pow(clamp(num / den, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);\n";
        } else {
          body += "      vec3 x = linear * expo;\n"
                  "      float toneY = dot(x, vec3(0.2126, 0.7152, 0.0722));\n"
                  "      float mappedY = toneY / (toneY + paper);\n"
                  "      vec3 mapped = toneY > 1e-5 ? x * (mappedY / toneY) : x;\n"
                  "      return vec4(pow(clamp(mapped, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);\n";
        }
      }    } else if (p010) {
      body +=
          "      vec3 rgb = vec3(yy + 1.793 * vv, yy - 0.213 * uu - 0.533 * vv,"
          " yy + 2.112 * uu);\n"
          "      return vec4(clamp(rgb, 0.0, 1.0), 1.0);\n";
    } else {
      body += std::string("      vec3 rgb = vec3(yy + ") + (bt709 ? "1.793" : "1.596") +
              " * vv, yy " + (bt709 ? "- 0.213" : "- 0.391") + " * uu " +
              (bt709 ? "- 0.533" : "- 0.813") + " * vv, yy + " + (bt709 ? "2.112" : "2.018") +
              " * uu) / 255.0;\n"
              "      return vec4(clamp(rgb, 0.0, 1.0), 1.0);\n";
    }
    snapshot->desc.fragmentSource = body;
    snapshot->hash = hashProgram(snapshot->desc, snapshot->parameters, {}, 0);
    mYuvProgram = programFor(snapshot, error);
    if (mYuvProgram == nullptr) return false;
    mYuvBt709 = bt709;
    mYuvFullRange = fullRange;
    mYuvP010 = p010;
    mYuvHdr = hdr;
    mYuvHdrTransfer = hdrTransfer;
    mYuvToneMap = toneMapMode;
    mYuvExposure = exposure;
    mYuvPaperWhite = paperWhiteNits;
  }
  if (!mYuvTextures[0].valid() || mYuvWidth != width || mYuvHeight != height || mYuvPlaneP010 != p010) {
    for (backend::TextureHandle& texture : mYuvTextures) {
      if (texture.valid()) mDevice->destroyTexture(texture);
      texture = backend::TextureHandle::invalid();
    }
    for (uint32_t index = 0; index < 3; ++index) {
      const uint32_t planeWidth = index == 0 ? width : (width + 1) / 2;
      const uint32_t planeHeight = index == 0 ? height : (height + 1) / 2;
      backend::TextureDescriptor descriptor = backend::TextureDescriptor::texture2D(
              planeWidth, planeHeight, p010 ? backend::PixelFormat::R16_UNORM : backend::PixelFormat::R8_UNORM,
              static_cast<backend::TextureUsages>(backend::TextureUsage::Sampled));
      descriptor.label = "effect/yuv-plane";
      if (!backend::isSuccess(mDevice->createTexture(descriptor, &mYuvTextures[index]))) {
        error = "the YUV plane texture could not be created";
        return false;
      }
    }
    mYuvWidth = width;
    mYuvHeight = height;
    mYuvPlaneP010 = p010;
  }

  backend::TextureCopyRegion lumaRegion;
  lumaRegion.size = { width, height, 1 };
  lumaRegion.arrayLayerCount = 1;
  backend::TextureCopyRegion chromaRegion;
  chromaRegion.size = { (width + 1) / 2, (height + 1) / 2, 1 };
  chromaRegion.arrayLayerCount = 1;
  if (!backend::isSuccess(commands->writeTexture(mYuvTextures[0], lumaRegion, y, yRowBytes * height, yRowBytes)) ||
      !backend::isSuccess(commands->writeTexture(mYuvTextures[1], chromaRegion, u, uRowBytes * ((height + 1) / 2), uRowBytes)) ||
      !backend::isSuccess(commands->writeTexture(mYuvTextures[2], chromaRegion, v, vRowBytes * ((height + 1) / 2), vRowBytes))) {
    error = "the YUV planes could not be uploaded";
    return false;
  }

  FrameKey key;
  key.width = width;
  key.height = height;
  key.format = targetFormat == EffectPixelFormat::RGBA16F
                        ? backend::PixelFormat::R16G16B16A16_SFLOAT
                        : backend::PixelFormat::R8G8B8A8_UNORM;
  FrameLease lease = mPool->acquire(key, error);
  if (!lease.isValid()) return false;
  const backend::RenderPipelineHandle pipeline = mYuvProgram->pipelineFor(key.format, lease.get()->pass, nullptr);
  if (!pipeline.valid()) {
    error = "the YUV conversion pipeline could not be built";
    return false;
  }
  const backend::DescriptorSetHandle set = mYuvProgram->setFor(
          { mYuvTextures[0], mYuvTextures[1], mYuvTextures[2] }, mParamsTexture, mLinearSampler, {}, nullptr, nullptr);
  if (!set.valid()) {
    error = "the YUV conversion set could not be bound";
    return false;
  }
  ParameterBlock block;
  block.reset();
  block.values[0] = static_cast<float>(width);
  block.values[1] = static_cast<float>(height);
  block.values[2] = width != 0 ? 1.0f / static_cast<float>(width) : 0.0f;
  block.values[3] = height != 0 ? 1.0f / static_cast<float>(height) : 0.0f;
  block.values[4] = static_cast<float>(width);
  block.values[5] = static_cast<float>(height);
  block.values[6] = block.values[2];
  block.values[7] = block.values[3];
  block.values[8] = hdr10PlusTargetedNits;
  block.values[9] = hdr10PlusAverageMaxRgb;
  block.values[10] = hdr10PlusFractionBrightPixels;
  block.values[12] = hdrMaxCll;
  block.values[13] = hdrMaxFall;
  if (!writeParameters(block, error)) return false;
  backend::RenderPassBeginInfo info;
  info.renderPass = lease.get()->pass;
  info.colorAttachmentCount = 1;
  info.colorAttachments[0].view = lease.get()->view;
  info.colorAttachments[0].loadOp = backend::LoadOp::Clear;
  info.colorAttachments[0].storeOp = backend::StoreOp::Store;
  info.colorAttachments[0].clearColor = backend::Color::transparentBlack();
  info.renderArea = { width, height };
  if (!backend::isSuccess(commands->beginRenderPass(info)) ||
      !backend::isSuccess(commands->bindRenderPipeline(pipeline)) ||
      !backend::isSuccess(commands->bindDescriptorSet(0, set)) ||
      !backend::isSuccess(commands->setVertexBuffer(0, mQuad, 0)) ||
      !backend::isSuccess(commands->draw(6, 1, 0, 0)) ||
      !backend::isSuccess(commands->endRenderPass())) {
    error = "the YUV conversion pass could not be recorded";
    return false;
  }
  out = std::move(lease);
  return true;
}
bool RenderCore::convertHardwareBufferYuv(backend::CommandBuffer* const commands, const void* const buffer,
        const uint32_t width, const uint32_t height, const bool bt2020, const bool fullRange,
        const bool p010, const bool hdr, const EffectHdrTransfer hdrTransfer,
        const EffectPixelFormat targetFormat, const EffectToneMapMode toneMapMode, const float exposure,
        const float paperWhiteNits, const float hdr10PlusTargetedNits, const float hdr10PlusAverageMaxRgb,
        const float hdr10PlusFractionBrightPixels, const float hdrMaxCll, const float hdrMaxFall,
        FrameLease& out, std::string& error) {
  if (commands == nullptr || mDevice == nullptr || buffer == nullptr) {
    error = "the hardware-buffer conversion has no command buffer or buffer";
    return false;
  }
  backend::YcbcrConversionDescriptor ycbcr;
  ycbcr.model = bt2020 ? backend::YcbcrModelConversion::Bt2020 : backend::YcbcrModelConversion::Bt709;
  ycbcr.range = fullRange ? backend::YcbcrRange::Full : backend::YcbcrRange::Narrow;
  ycbcr.xChromaOffset = backend::ChromaLocation::Midpoint;
  ycbcr.yChromaOffset = backend::ChromaLocation::Midpoint;

  backend::NativeTextureDescriptor descriptor;
  descriptor.kind = backend::NativeTextureKind::AndroidHardwareBuffer;
  descriptor.platformHandle = const_cast<void*>(buffer);
  descriptor.width = width;
  descriptor.height = height;
  descriptor.format = backend::PixelFormat::Undefined;
  descriptor.ycbcr = ycbcr;
  backend::TextureHandle texture = backend::TextureHandle::invalid();
  backend::YcbcrConversionDescriptor conversion = ycbcr;
  const backend::Result imported = mDevice->importNativeYuvTexture(descriptor, &texture, &conversion);
  if (!backend::isSuccess(imported) || !texture.valid()) {
    error = "the YUV hardware buffer could not be imported (backend result " +
            std::to_string(static_cast<int>(imported)) + ")";
    return false;
  }
  backend::TextureViewDescriptor viewDescriptor;
  viewDescriptor.texture = texture;
  viewDescriptor.ycbcr = conversion;
  backend::TextureViewHandle view = backend::TextureViewHandle::invalid();
  if (!backend::isSuccess(mDevice->createTextureView(viewDescriptor, &view)) || !view.valid()) {
    mDevice->destroyTexture(texture);
    error = "the YUV hardware-buffer view could not be created";
    return false;
  }
  backend::SamplerDescriptor samplerDescriptor = backend::SamplerDescriptor::linearClamp();
  samplerDescriptor.ycbcr = conversion;
  backend::TextureSamplerHandle sampler = backend::TextureSamplerHandle::invalid();
  if (!backend::isSuccess(mDevice->createSampler(samplerDescriptor, &sampler)) || !sampler.valid()) {
    mDevice->destroyTextureView(view);
    mDevice->destroyTexture(texture);
    error = "the YUV hardware-buffer sampler could not be created";
    return false;
  }

  if (mHardwareProgram == nullptr || mHardwareHdr != hdr || mHardwareTransfer != hdrTransfer ||
      mHardwareToneMap != toneMapMode || mHardwareExposure != exposure ||
      mHardwarePaperWhite != paperWhiteNits || mHardwareTarget != targetFormat) {
    auto snapshot = std::make_shared<ProgramSnapshot>();
    snapshot->desc.name = "effect/hardware-yuv";
    snapshot->desc.authoring = EffectShaderAuthoring::Body;
    snapshot->desc.inputCount = 1;
    snapshot->parameters.push_back({ "hdr10TargetedNits", EffectValueType::Float, 0, 1 });
    snapshot->parameters.push_back({ "hdr10AverageMaxRgb", EffectValueType::Float, 1, 1 });
    snapshot->parameters.push_back({ "hdr10FractionBright", EffectValueType::Float, 2, 1 });
    snapshot->parameters.push_back({ "hdrMaxCll", EffectValueType::Float, 3, 1 });
    snapshot->parameters.push_back({ "hdrMaxFall", EffectValueType::Float, 4, 1 });
    std::string body = "      vec3 rgb = clamp(texture(uInput0, uv).rgb, 0.0, 1.0);\n";
    if (hdr) {
      if (hdrTransfer == EffectHdrTransfer::HLG) {
        body += "      float a = 0.17883277;\n"
                "      float b = 0.28466892;\n"
                "      float cc = 0.55991073;\n"
                "      vec3 signal = mix(rgb * rgb / 3.0, (exp((rgb - cc) / a) + b) / 12.0, step(vec3(0.5), rgb));\n"
                "      vec3 linear = signal * 1000.0;\n"
            "      float hlgY = dot(linear, vec3(0.2627, 0.6780, 0.0593)) / 1000.0;\n"
            "      linear *= pow(max(hlgY, 0.0), 0.2);\n";
      } else {
        body += "      vec3 eotf = pow(rgb, vec3(1.0 / 78.84375));\n"
                "      vec3 linear = 10000.0 * pow(max(eotf - 0.8359375, vec3(0.0)) /\n"
                "              (18.8515625 - 18.6875 * eotf), vec3(1.0 / 0.1593017578));\n";
      }
      if (targetFormat == EffectPixelFormat::RGBA16F) {
        body += "      return vec4(linear / 10000.0, 1.0);\n";
      } else {
        body += "      float hdrTarget = effectParams(0).x;\n"
                "      float hdrAvg = effectParams(0).y;\n"
                "      float paper = hdrTarget > 0.0 ? clamp(hdrTarget * 0.5, 80.0, 400.0) : " +
                std::to_string(paperWhiteNits) + ";\n"
                "      float maxCll = effectParams(0).w;\n"
                "      float maxFall = effectParams(1).x;\n"
                "      float expo = " + std::to_string(exposure) +
                " * (hdrAvg > 0.0 ? mix(1.0, 1.3, clamp(hdrAvg * 8.0, 0.0, 1.0)) : 1.0);\n"
                "      expo *= mix(1.0, 0.9, clamp(maxFall / 1000.0, 0.0, 1.0));\n"
                "      paper *= mix(1.0, 1.2, clamp(maxCll / 1000.0, 0.0, 1.0));\n";
        if (toneMapMode == EffectToneMapMode::Aces) {
          body += "      vec3 x = linear * expo / paper;\n"
                  "      vec3 num = x * (2.51 * x + 0.03);\n"
                  "      vec3 den = x * (2.43 * x + 0.59) + 0.14;\n"
                  "      return vec4(pow(clamp(num / den, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);\n";
        } else {
          body += "      vec3 x = linear * expo;\n"
                  "      float toneY = dot(x, vec3(0.2126, 0.7152, 0.0722));\n"
                  "      float mappedY = toneY / (toneY + paper);\n"
                  "      vec3 mapped = toneY > 1e-5 ? x * (mappedY / toneY) : x;\n"
                  "      return vec4(pow(clamp(mapped, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);\n";
        }
      }
    } else {
      body += "      return vec4(rgb, 1.0);\n";
    }
    snapshot->desc.fragmentSource = std::move(body);
    mHardwareProgram.reset(programFor(snapshot, error));
    if (mHardwareProgram == nullptr) {
      mDevice->destroyTextureView(view);
      mDevice->destroySampler(sampler);
      mDevice->destroyTexture(texture);
      return false;
    }
    mHardwareHdr = hdr;
    mHardwareTransfer = hdrTransfer;
    mHardwareToneMap = toneMapMode;
    mHardwareExposure = exposure;
    mHardwarePaperWhite = paperWhiteNits;
    mHardwareTarget = targetFormat;
  }

  FrameKey key;
  key.width = width;
  key.height = height;
  key.format = targetFormat == EffectPixelFormat::RGBA16F
                        ? backend::PixelFormat::R16G16B16A16_SFLOAT
                        : backend::PixelFormat::R8G8B8A8_UNORM;
  FrameLease lease = mPool->acquire(key, error);
  if (!lease.isValid()) {
    mDevice->destroyTextureView(view);
    mDevice->destroySampler(sampler);
    mDevice->destroyTexture(texture);
    return false;
  }
  const backend::RenderPipelineHandle pipeline = mHardwareProgram->pipelineFor(key.format, lease.get()->pass, nullptr);
  if (!pipeline.valid()) {
    error = "the hardware-buffer pipeline could not be built";
    return false;
  }
  const std::vector<backend::TextureHandle> inputs = { texture };
  const std::vector<backend::TextureViewHandle> inputViews = { view };
  const backend::DescriptorSetHandle set = mHardwareProgram->setFor(inputs, mParamsTexture, sampler, {}, nullptr,
          nullptr, nullptr, &inputViews);
  if (!set.valid()) {
    error = "the hardware-buffer set could not be bound";
    return false;
  }
  ParameterBlock block;
  block.reset();
  block.values[0] = static_cast<float>(width);
  block.values[1] = static_cast<float>(height);
  block.values[2] = width != 0 ? 1.0f / static_cast<float>(width) : 0.0f;
  block.values[3] = height != 0 ? 1.0f / static_cast<float>(height) : 0.0f;
  block.values[4] = static_cast<float>(width);
  block.values[5] = static_cast<float>(height);
  block.values[6] = block.values[2];
  block.values[7] = block.values[3];
  block.values[8] = hdr10PlusTargetedNits;
  block.values[9] = hdr10PlusAverageMaxRgb;
  block.values[10] = hdr10PlusFractionBrightPixels;
  block.values[12] = hdrMaxCll;
  block.values[13] = hdrMaxFall;
  if (!writeParameters(block, error)) return false;
  backend::RenderPassBeginInfo info;
  info.renderPass = lease.get()->pass;
  info.colorAttachmentCount = 1;
  info.colorAttachments[0].view = lease.get()->view;
  info.colorAttachments[0].loadOp = backend::LoadOp::Clear;
  info.colorAttachments[0].storeOp = backend::StoreOp::Store;
  info.colorAttachments[0].clearColor = backend::Color::transparentBlack();
  info.renderArea = { width, height };
  if (!backend::isSuccess(commands->beginRenderPass(info)) ||
      !backend::isSuccess(commands->bindRenderPipeline(pipeline)) ||
      !backend::isSuccess(commands->bindDescriptorSet(0, set)) ||
      !backend::isSuccess(commands->setVertexBuffer(0, mQuad, 0)) ||
      !backend::isSuccess(commands->draw(6, 1, 0, 0)) ||
      !backend::isSuccess(commands->endRenderPass())) {
    error = "the hardware-buffer pass could not be recorded";
    return false;
  }
  retireAfterGpu([this, texture, view, sampler]() {
    if (mDevice == nullptr) return;
    mDevice->destroyTextureView(view);
    mDevice->destroySampler(sampler);
    mDevice->destroyTexture(texture);
  });
  out = std::move(lease);
  return true;
}
RenderProgram* RenderCore::programFor(const std::shared_ptr<const ProgramSnapshot>& snapshot, std::string& error) {
  if (snapshot == nullptr || mDevice == nullptr) {
    error = "there is no shader description to compile";
    return nullptr;
  }
  const auto existing = mPrograms.find(snapshot->hash);
  if (existing != mPrograms.end()) {
    existing->second->lastUsedFrame = mFrameCounter;
    if (!existing->second->isValid()) {
      error = existing->second->diagnostics();
      return nullptr;
    }
    return existing->second.get();
  }

  const auto compileStart = std::chrono::steady_clock::now();
  auto program = std::make_unique<RenderProgram>(*this, *snapshot);
  RenderProgram* result = program.get();
  result->lastUsedFrame = mFrameCounter;
  mPrograms.emplace(snapshot->hash, std::move(program));
  ++mProgramCompileCount;
  mProgramCompileMilliseconds +=
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - compileStart).count();
  if (!result->isValid()) {
    error = result->diagnostics();
    return nullptr;
  }
  return result;
}

bool RenderCore::warmPipeline(RenderProgram& program, const backend::PixelFormat format, const uint32_t width,
                              const uint32_t height, std::string& error) {
  if (mPool == nullptr || mDevice == nullptr) {
    error = "there is no device to warm the pipeline on";
    return false;
  }
  if (format == backend::PixelFormat::Undefined) {
    error = "the pass requested an unsupported output format";
    return false;
  }
  FrameKey key;
  key.width = std::max(1u, width);
  key.height = std::max(1u, height);
  key.format = format;
  key.sampleCount = 1;
  FrameLease lease = mPool->acquire(key, error);
  if (!lease.isValid()) return false;
  const backend::RenderPipelineHandle pipeline = program.pipelineFor(format, lease.get()->pass, nullptr);
  if (!pipeline.valid()) {
    error = "the pipeline could not be linked for the pass's output format";
    return false;
  }
  return true;
}

backend::TextureHandle RenderCore::textureForImage(const std::shared_ptr<const EffectImage>& image, std::string& error) {
  if (image == nullptr || mDevice == nullptr) {
    error = "the image is null";
    return backend::TextureHandle::invalid();
  }
  const auto existing = mImages.find(image.get());
  if (existing != mImages.end()) {
    existing->second.lastUsedFrame = mFrameCounter;
    return existing->second.texture;
  }

  backend::PixelFormat format = backend::PixelFormat::R8G8B8A8_UNORM;
  switch (image->format()) {
    case EffectPixelFormat::RGBA8:
      format = backend::PixelFormat::R8G8B8A8_UNORM;
      break;
    case EffectPixelFormat::BGRA8:
      format = backend::PixelFormat::B8G8R8A8_UNORM;
      break;
    default:
      error = "an effect image can only be uploaded as RGBA8 or BGRA8";
      return backend::TextureHandle::invalid();
  }

  backend::TextureDescriptor descriptor = backend::TextureDescriptor::texture2D(
      image->width(), image->height(), format,
      static_cast<backend::TextureUsages>(backend::TextureUsage::Sampled | backend::TextureUsage::Immutable));
  descriptor.label = "effect/image";
  backend::TextureHandle texture = backend::TextureHandle::invalid();
  if (!backend::isSuccess(mDevice->createTexture(descriptor, &texture))) {
    error = "the image could not be uploaded as a texture";
    return backend::TextureHandle::invalid();
  }

  const bool written = [&] {
    if (mRecording != nullptr && mRecording->commands != nullptr) {
      // Recording an upload into the frame's own buffer is both cheaper and better ordered than a separate
      // submission: the draw that samples it is later in the same recording.
      backend::TextureCopyRegion region;
      region.textureOrigin = { 0, 0, 0 };
      region.size = { image->width(), image->height(), 1 };
      region.mipLevel = 0;
      region.baseArrayLayer = 0;
      region.arrayLayerCount = 1;
      return backend::isSuccess(mRecording->commands->writeTexture(texture, region, image->pixels(),
                                                                   image->byteSize(), image->rowBytes()));
    }
    return executeTemporary(
        [&](backend::CommandBuffer& commands) {
          backend::TextureCopyRegion region;
          region.textureOrigin = { 0, 0, 0 };
          region.size = { image->width(), image->height(), 1 };
          region.mipLevel = 0;
          region.baseArrayLayer = 0;
          region.arrayLayerCount = 1;
          commands.writeTexture(texture, region, image->pixels(), image->byteSize(), image->rowBytes());
        },
        true, error);
  }();
  if (!written) {
    mDevice->destroyTexture(texture);
    return backend::TextureHandle::invalid();
  }

  mImages.emplace(image.get(), ImageEntry{ image, texture, mFrameCounter,
                                            static_cast<size_t>(image->width()) * image->height() * 4 });
  mTextureCachePeakBytes = std::max(mTextureCachePeakBytes, textureCacheBytes());
  return texture;
}

backend::TextureSamplerHandle RenderCore::ycbcrSampler(const backend::YcbcrConversionDescriptor& conversion,
                                                        std::string& error) {
  for (const auto& entry : mYcbcrSamplers) {
    if (entry.first == conversion) return entry.second;
  }
  backend::SamplerDescriptor descriptor = backend::SamplerDescriptor::linearClamp();
  descriptor.ycbcr = conversion;
  descriptor.label = "effect/ycbcr";
  backend::TextureSamplerHandle sampler = backend::TextureSamplerHandle::invalid();
  if (!backend::isSuccess(mDevice->createSampler(descriptor, &sampler))) {
    error = "a YUV sampler for the external texture could not be created";
    return backend::TextureSamplerHandle::invalid();
  }
  mYcbcrSamplers.emplace_back(conversion, sampler);
  return sampler;
}

backend::TextureHandle RenderCore::textureForSolid(const EffectColor color, std::string& error) {
  const auto key = (static_cast<uint32_t>(std::clamp(color.r, 0.0f, 1.0f) * 255.0f + 0.5f) << 24) |
                   (static_cast<uint32_t>(std::clamp(color.g, 0.0f, 1.0f) * 255.0f + 0.5f) << 16) |
                   (static_cast<uint32_t>(std::clamp(color.b, 0.0f, 1.0f) * 255.0f + 0.5f) << 8) |
                   static_cast<uint32_t>(std::clamp(color.a, 0.0f, 1.0f) * 255.0f + 0.5f);
  const auto existing = mSolids.find(key);
  if (existing != mSolids.end()) {
    existing->second.lastUsedFrame = mFrameCounter;
    return existing->second.texture;
  }

  backend::TextureDescriptor descriptor = backend::TextureDescriptor::texture2D(
      1, 1, backend::PixelFormat::R8G8B8A8_UNORM,
      static_cast<backend::TextureUsages>(backend::TextureUsage::Sampled | backend::TextureUsage::Immutable));
  descriptor.label = "effect/solid";
  backend::TextureHandle texture = backend::TextureHandle::invalid();
  if (!backend::isSuccess(mDevice->createTexture(descriptor, &texture))) {
    error = "a solid colour texture could not be created";
    return backend::TextureHandle::invalid();
  }
  const uint8_t pixel[4] = {
    static_cast<uint8_t>((key >> 24) & 0xFFu),
    static_cast<uint8_t>((key >> 16) & 0xFFu),
    static_cast<uint8_t>((key >> 8) & 0xFFu),
    static_cast<uint8_t>(key & 0xFFu),
  };
  const bool written = [&] {
    if (mRecording != nullptr && mRecording->commands != nullptr) {
      backend::TextureCopyRegion region;
      region.size = { 1, 1, 1 };
      region.arrayLayerCount = 1;
      return backend::isSuccess(mRecording->commands->writeTexture(texture, region, pixel, sizeof(pixel)));
    }
    return executeTemporary(
        [&](backend::CommandBuffer& commands) {
          backend::TextureCopyRegion region;
          region.size = { 1, 1, 1 };
          region.arrayLayerCount = 1;
          commands.writeTexture(texture, region, pixel, sizeof(pixel));
        },
        true, error);
  }();
  if (!written) {
    mDevice->destroyTexture(texture);
    return backend::TextureHandle::invalid();
  }
  mSolids.emplace(key, SolidEntry{ texture, mFrameCounter });
  mTextureCachePeakBytes = std::max(mTextureCachePeakBytes, textureCacheBytes());
  return texture;
}

bool RenderCore::writeParameters(const ParameterBlock& block, std::string& error) {
  backend::CommandBuffer* commands = mRecording != nullptr ? mRecording->commands : nullptr;
  if (commands == nullptr) {
    error = "parameters can only be uploaded while a frame is being recorded";
    return false;
  }
  backend::TextureCopyRegion region;
  region.textureOrigin = { 0, 0, 0 };
  region.size = { kParameterSlotCount, 1, 1 };
  region.mipLevel = 0;
  region.baseArrayLayer = 0;
  region.arrayLayerCount = 1;
  if (!backend::isSuccess(commands->writeTexture(mParamsTexture, region, block.values, sizeof(block.values)))) {
    error = "the pass parameters could not be uploaded";
    return false;
  }
  return true;
}

bool RenderCore::readback(const backend::TextureHandle texture, const uint32_t width, const uint32_t height,
                          const backend::PixelFormat format, std::vector<uint8_t>& out, std::string& error) {
  if (mDevice == nullptr || !texture.valid()) {
    error = "there is no texture to read back";
    return false;
  }
  const size_t bytes = backend::getFormatByteSize(format, width, height);
  if (bytes == 0) {
    error = "the readback format has no byte size";
    return false;
  }

  backend::BufferDescriptor bufferDescriptor;
  bufferDescriptor.size = bytes;
  bufferDescriptor.usage = static_cast<backend::BufferUsages>(backend::BufferUsage::TransferDst);
  bufferDescriptor.memoryUsage = backend::MemoryUsage::GpuToCpu;
  bufferDescriptor.label = "effect/readback";
  backend::BufferHandle buffer = backend::BufferHandle::invalid();
  if (!backend::isSuccess(mDevice->createBuffer(bufferDescriptor, &buffer))) {
    error = "the readback buffer could not be created";
    return false;
  }

  bool submitted = false;
  const bool copied = executeTemporary(
      [&](backend::CommandBuffer& commands) {
        backend::BufferTextureCopy copy;
        copy.buffer = buffer;
        copy.texture = texture;
        copy.bufferRegion = { 0, 0, bytes };
        copy.textureRegion.textureOrigin = { 0, 0, 0 };
        copy.textureRegion.size = { width, height, 1 };
        copy.textureRegion.mipLevel = 0;
        copy.textureRegion.baseArrayLayer = 0;
        copy.textureRegion.arrayLayerCount = 1;
        submitted = backend::isSuccess(commands.copyTextureToBuffer(copy));
      },
      true, error);
  if (!copied || !submitted) {
    if (error.empty()) error = "the readback copy could not be recorded";
    mDevice->destroyBuffer(buffer);
    return false;
  }

  void* mapped = nullptr;
  if (!backend::isSuccess(mDevice->mapBuffer(buffer, 0, bytes, &mapped)) || mapped == nullptr) {
    error = "the readback buffer could not be mapped";
    mDevice->destroyBuffer(buffer);
    return false;
  }
  out.resize(bytes);
  std::memcpy(out.data(), mapped, bytes);
  mDevice->unmapBuffer(buffer);
  mDevice->destroyBuffer(buffer);
  return true;
}

uint32_t RenderCore::textureCount() const noexcept {
  return static_cast<uint32_t>(mImages.size() + mSolids.size());
}

std::string RenderCore::diagnostics() const {
  std::string result = mDiagnostics;
  if (mDevice != nullptr) {
    if (!result.empty()) result += '\n';
    mDevice->appendDiagnostics(result);
  }
  return result;
}

} // namespace coral::detail
