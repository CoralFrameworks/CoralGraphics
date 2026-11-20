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
 * @file ReadbackOutputNode.cpp
 *
 * GPU -> CPU for the final frame. The copy is recorded into the frame's own command buffer, so it reads
 * the frame's pixels rather than a stale image; `afterSubmit` waits for that submission's fence and then
 * maps the staging buffer. That wait is why readback is documented as the export/screenshot path, not a
 * preview path.
 */

#include <effect/ReadbackOutputNode.h>

#include "EffectInternal.h"
#include "EffectRenderCore.h"

#include <algorithm>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <utility>

namespace coral {

struct ReadbackOutputNode::State {
  mutable std::mutex mutex;
  std::condition_variable cv;
  Callback callback;
  EffectOutputConfig config;
  EffectFrame lastFrame;
  std::vector<uint8_t> lastPixels;
  uint64_t producedVersion = 0;
  uint64_t observedVersion = 0;
};

namespace {

size_t bytesPerPixel(const EffectPixelFormat format) noexcept {
  switch (format) {
    case EffectPixelFormat::RGBA16F:
      return 8;
    case EffectPixelFormat::RGBA8:
    case EffectPixelFormat::BGRA8:
    default:
      return 4;
  }
}

std::vector<uint8_t> transformPixels(const uint8_t* source, const uint32_t sourceWidth, const uint32_t sourceHeight,
                                     const EffectRotation rotation, const bool mirrorHorizontal,
                                     const bool mirrorVertical, const uint32_t outputWidth,
                                     const uint32_t outputHeight) {
  std::vector<uint8_t> output(static_cast<size_t>(outputWidth) * outputHeight * 4);
  for (uint32_t y = 0; y < outputHeight; ++y) {
    for (uint32_t x = 0; x < outputWidth; ++x) {
      uint32_t ox = x;
      uint32_t oy = y;
      if (mirrorHorizontal) ox = outputWidth - 1 - ox;
      if (mirrorVertical) oy = outputHeight - 1 - oy;
      uint32_t sx = ox;
      uint32_t sy = oy;
      switch (rotation) {
        case EffectRotation::Clockwise90:
          sx = oy;
          sy = sourceHeight - 1 - ox;
          break;
        case EffectRotation::Clockwise180:
          sx = sourceWidth - 1 - ox;
          sy = sourceHeight - 1 - oy;
          break;
        case EffectRotation::Clockwise270:
          sx = sourceWidth - 1 - oy;
          sy = ox;
          break;
        case EffectRotation::None:
        default:
          break;
      }
      if (sx < sourceWidth && sy < sourceHeight) {
        std::memcpy(output.data() + (static_cast<size_t>(y) * outputWidth + x) * 4,
                    source + (static_cast<size_t>(sy) * sourceWidth + sx) * 4, 4);
      }
    }
  }
  return output;
}

std::vector<uint8_t> scaleNearest(const std::vector<uint8_t>& source, const uint32_t sourceWidth,
                                  const uint32_t sourceHeight, const uint32_t targetWidth,
                                  const uint32_t targetHeight) {
  std::vector<uint8_t> output(static_cast<size_t>(targetWidth) * targetHeight * 4);
  for (uint32_t y = 0; y < targetHeight; ++y) {
    const uint32_t sy = static_cast<uint32_t>((static_cast<uint64_t>(y) * sourceHeight) / targetHeight);
    for (uint32_t x = 0; x < targetWidth; ++x) {
      const uint32_t sx = static_cast<uint32_t>((static_cast<uint64_t>(x) * sourceWidth) / targetWidth);
      std::memcpy(output.data() + (static_cast<size_t>(y) * targetWidth + x) * 4,
                  source.data() + (static_cast<size_t>(sy) * sourceWidth + sx) * 4, 4);
    }
  }
  return output;
}

class ReadbackRuntime final : public coral::detail::OutputRuntime {
public:
  explicit ReadbackRuntime(std::shared_ptr<ReadbackOutputNode::State> state) : mState(std::move(state)) {
    startWorker();
  }

  ~ReadbackRuntime() override { releaseResources(); }

  [[nodiscard]] bool isReady() const noexcept override {
    return true;
  }

  void setCallbackQueue(std::shared_ptr<coral::detail::CallbackQueue> queue) override {
    mCallbackQueue = std::move(queue);
  }

  bool targetSize(const uint32_t sourceWidth, const uint32_t sourceHeight, uint32_t& width,
                  uint32_t& height) const override {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    if (mState->config.width != 0 && mState->config.height != 0) {
      width = mState->config.width;
      height = mState->config.height;
      return true;
    }
    const bool swapAxes = mState->config.rotation == EffectRotation::Clockwise90 ||
                          mState->config.rotation == EffectRotation::Clockwise270;
    const bool transformed = swapAxes || mState->config.rotation == EffectRotation::Clockwise180 ||
                             mState->config.mirrorHorizontal || mState->config.mirrorVertical;
    if (!transformed) return false;
    width = swapAxes ? sourceHeight : sourceWidth;
    height = swapAxes ? sourceWidth : sourceHeight;
    return true;
  }

  bool targetTransform(EffectRotation& rotation, bool& mirrorHorizontal, bool& mirrorVertical) const override {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    rotation = mState->config.rotation;
    mirrorHorizontal = mState->config.mirrorHorizontal;
    mirrorVertical = mState->config.mirrorVertical;
    return rotation != EffectRotation::None || mirrorHorizontal || mirrorVertical;
  }

  EffectPixelFormat targetFormat() const override {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    return mState->config.format;
  }

  EffectScaleMode targetScaleMode() const override {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    return mState->config.scaleMode;
  }

  EffectColor targetLetterboxColor() const override {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    return mState->config.letterboxColor;
  }

  bool consume(coral::detail::FrameContext& context, const coral::detail::FrameView& view,
               std::string& error) override {
    if (context.commands == nullptr || context.core == nullptr) {
      error = "the readback output has no command buffer to record its copy into";
      return false;
    }
    if (view.width == 0 || view.height == 0) {
      error = "the readback output received a zero-sized frame";
      return false;
    }
    if (view.format != EffectPixelFormat::RGBA8 && view.format != EffectPixelFormat::BGRA8 &&
        view.format != EffectPixelFormat::RGBA16F) {
      error = "readback supports RGBA8/BGRA8/RGBA16F frames only";
      return false;
    }

    backend::Device* device = context.core->device();
    mAsync = device != nullptr && device->getInfo().backend == backend::Backend::Vulkan;
    EffectOutputConfig config;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      config = mState->config;
    }
    const bool swapAxes = config.rotation == EffectRotation::Clockwise90 ||
                          config.rotation == EffectRotation::Clockwise270;
    const uint32_t transformedWidth = view.transformApplied ? view.width
                                                            : (swapAxes ? view.height : view.width);
    const uint32_t transformedHeight = view.transformApplied ? view.height
                                                             : (swapAxes ? view.width : view.height);
    const uint32_t outputWidth = view.transformApplied
                                     ? view.width
                                     : (config.width != 0 ? config.width : transformedWidth);
    const uint32_t outputHeight = view.transformApplied
                                      ? view.height
                                      : (config.height != 0 ? config.height : transformedHeight);
    const size_t bytes = static_cast<size_t>(view.width) * view.height * bytesPerPixel(view.format);
    backend::BufferHandle buffer = backend::BufferHandle::invalid();
    if (!acquireBuffer(device, bytes, &buffer, error)) {
      return false;
    }

    backend::BufferTextureCopy copy;
    copy.buffer = buffer;
    copy.texture = view.texture;
    copy.bufferRegion = { 0, 0, bytes };
    copy.textureRegion.textureOrigin = { 0, 0, 0 };
    copy.textureRegion.size = { view.width, view.height, 1 };
    copy.textureRegion.mipLevel = 0;
    copy.textureRegion.baseArrayLayer = 0;
    copy.textureRegion.arrayLayerCount = 1;
    if (!backend::isSuccess(context.commands->copyTextureToBuffer(copy))) {
      device->destroyBuffer(buffer);
      error = "the readback copy could not be recorded";
      return false;
    }

    Pending pending;
    pending.device = device;
    pending.buffer = buffer;
    pending.bytes = bytes;
    pending.width = view.width;
    pending.height = view.height;
    pending.transformedWidth = transformedWidth;
    pending.transformedHeight = transformedHeight;
    pending.outputWidth = outputWidth;
    pending.outputHeight = outputHeight;
    pending.rotation = view.transformApplied ? EffectRotation::None : config.rotation;
    pending.mirrorHorizontal = view.transformApplied ? false : config.mirrorHorizontal;
    pending.mirrorVertical = view.transformApplied ? false : config.mirrorVertical;
    pending.frame = coral::detail::FrameAccess::make(detail::allocateFrameId(context), outputWidth, outputHeight,
                                                     view.format, view.colorSpace,
                                                     view.timestamp.isValid() ? view.timestamp : context.timestamp);
    mPending.push_back(std::move(pending));
    return true;
  }

  void afterSubmit(const std::shared_ptr<coral::detail::GpuSubmission>& submission) override {
    if (mPending.empty() || submission == nullptr) return;
    startWorker();
    Pending pending = std::move(mPending.front());
    mPending.pop_front();
    pending.submission = submission;
    if (!mAsync) {
      // OpenGL's map path needs the device's context on the calling thread; only Vulkan maps safely from
      // the readback worker.
      processPending(std::move(pending));
      return;
    }
    {
      const std::lock_guard<std::mutex> lock(mWorkMutex);
      mWork.push_back(std::move(pending));
    }
    mWorkCv.notify_one();
  }

  void afterAbort() override { releaseResources(); }

  void releaseResources() override {
    {
      const std::lock_guard<std::mutex> lock(mWorkMutex);
      mStopWorker = true;
    }
    mWorkCv.notify_all();
    if (mWorker.joinable()) mWorker.join();
    {
      const std::lock_guard<std::mutex> lock(mWorkMutex);
      for (Pending& pending : mWork) {
        if (pending.device != nullptr && pending.buffer.valid()) pending.device->destroyBuffer(pending.buffer);
      }
      mWork.clear();
    }
    for (Pending& pending : mPending) {
      if (pending.device != nullptr && pending.buffer.valid()) pending.device->destroyBuffer(pending.buffer);
    }
    mPending.clear();
    {
      const std::lock_guard<std::mutex> lock(mBufferMutex);
      if (mDevice != nullptr) {
        for (auto& bucket : mFreeBuffers) {
          for (backend::BufferHandle buffer : bucket.second) {
            if (buffer.valid()) mDevice->destroyBuffer(buffer);
          }
        }
      }
      mFreeBuffers.clear();
    }
    mDevice = nullptr;
  }

private:
  struct Pending {
    backend::Device* device = nullptr;
    backend::BufferHandle buffer = backend::BufferHandle::invalid();
    std::shared_ptr<coral::detail::GpuSubmission> submission;
    size_t bytes = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t transformedWidth = 0;
    uint32_t transformedHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    EffectRotation rotation = EffectRotation::None;
    bool mirrorHorizontal = false;
    bool mirrorVertical = false;
    EffectFrame frame;
  };

  void startWorker() {
    const std::lock_guard<std::mutex> lock(mWorkMutex);
    if (mWorker.joinable()) return;
    mStopWorker = false;
    mWorker = std::thread([this] { workerLoop(); });
  }

  void workerLoop() {
    for (;;) {
      Pending pending;
      {
        std::unique_lock<std::mutex> lock(mWorkMutex);
        mWorkCv.wait(lock, [this] { return mStopWorker || !mWork.empty(); });
        if (mStopWorker && mWork.empty()) return;
        pending = std::move(mWork.front());
        mWork.pop_front();
      }
      processPending(std::move(pending));
    }
  }

  void processPending(Pending pending) {
    if (pending.submission != nullptr) pending.submission->wait();
    void* mapped = nullptr;
    if (pending.device == nullptr ||
        !backend::isSuccess(pending.device->mapBuffer(pending.buffer, 0, pending.bytes, &mapped)) ||
        mapped == nullptr) {
      if (pending.device != nullptr) releaseBuffer(pending.buffer, pending.bytes);
      return;
    }

    std::vector<uint8_t> pixels;
    if (pending.rotation == EffectRotation::None && !pending.mirrorHorizontal && !pending.mirrorVertical) {
      pixels.resize(pending.bytes);
      std::memcpy(pixels.data(), mapped, pending.bytes);
    } else {
      pixels = transformPixels(static_cast<const uint8_t*>(mapped), pending.width, pending.height,
                               pending.rotation, pending.mirrorHorizontal, pending.mirrorVertical,
                               pending.transformedWidth, pending.transformedHeight);
    }
    if (pending.outputWidth != pending.transformedWidth || pending.outputHeight != pending.transformedHeight) {
      pixels = scaleNearest(pixels, pending.transformedWidth, pending.transformedHeight, pending.outputWidth,
                            pending.outputHeight);
    }
    pending.device->unmapBuffer(pending.buffer);
    releaseBuffer(pending.buffer, pending.bytes);

    ReadbackOutputNode::Callback callback;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      mState->lastFrame = pending.frame;
      mState->lastPixels = pixels;
      ++mState->producedVersion;
      callback = mState->callback;
    }
    mState->cv.notify_all();
    if (callback) {
      if (mCallbackQueue != nullptr) {
        auto copy = std::make_shared<std::vector<uint8_t>>(std::move(pixels));
        const EffectFrame frame = pending.frame;
        mCallbackQueue->push([callback, frame, copy] { callback(frame, copy->data(), copy->size()); });
      } else {
        callback(pending.frame, pixels.data(), pixels.size());
      }
    }
  }

  bool acquireBuffer(backend::Device* const device, const size_t bytes, backend::BufferHandle* out,
                     std::string& error) {
    const std::lock_guard<std::mutex> lock(mBufferMutex);
    mDevice = device;
    auto& bucket = mFreeBuffers[bytes];
    if (!bucket.empty()) {
      *out = bucket.back();
      bucket.pop_back();
      return true;
    }
    backend::BufferDescriptor descriptor;
    descriptor.size = bytes;
    descriptor.usage = static_cast<backend::BufferUsages>(backend::BufferUsage::TransferDst);
    descriptor.memoryUsage = backend::MemoryUsage::GpuToCpu;
    descriptor.label = "effect/readback";
    if (!backend::isSuccess(device->createBuffer(descriptor, out))) {
      error = "the readback staging buffer could not be created";
      return false;
    }
    return true;
  }

  void releaseBuffer(const backend::BufferHandle buffer, const size_t bytes) {
    const std::lock_guard<std::mutex> lock(mBufferMutex);
    if (buffer.valid() && mDevice != nullptr) mFreeBuffers[bytes].push_back(buffer);
  }

  std::shared_ptr<ReadbackOutputNode::State> mState;
  std::shared_ptr<coral::detail::CallbackQueue> mCallbackQueue;
  backend::Device* mDevice = nullptr;
  std::mutex mBufferMutex;
  std::unordered_map<size_t, std::vector<backend::BufferHandle>> mFreeBuffers;
  std::deque<Pending> mPending;
  std::mutex mWorkMutex;
  std::condition_variable mWorkCv;
  std::deque<Pending> mWork;
  std::thread mWorker;
  bool mStopWorker = false;
  bool mAsync = false;
};

} // namespace anonymous

ReadbackOutputNode::ReadbackOutputNode() : mState(std::make_shared<State>()) {
  setOutputRuntime(std::make_shared<ReadbackRuntime>(mState));
}

ReadbackOutputNode::~ReadbackOutputNode() = default;

std::shared_ptr<ReadbackOutputNode> ReadbackOutputNode::create() {
  return std::shared_ptr<ReadbackOutputNode>(new ReadbackOutputNode());
}

bool ReadbackOutputNode::isReady() const noexcept { return true; }

EffectResult ReadbackOutputNode::configure(const EffectOutputConfig& config) {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  if (config.format != EffectPixelFormat::RGBA8 && config.format != EffectPixelFormat::BGRA8 &&
      config.format != EffectPixelFormat::RGBA16F) {
    detail::NodeAccess::setDiagnostics(*this, "readback supports RGBA8/BGRA8/RGBA16F only");
    return EffectResult::Unsupported;
  }
  mState->config = config;
  return EffectResult::Success;
}

void ReadbackOutputNode::setCallback(Callback callback) {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->callback = std::move(callback);
}

bool ReadbackOutputNode::waitForFrame(EffectFrame* frame, std::vector<uint8_t>* pixels,
                                      const uint64_t timeoutMilliseconds) {
  std::unique_lock<std::mutex> lock(mState->mutex);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
  if (!mState->cv.wait_until(lock, deadline,
                             [this] { return mState->producedVersion > mState->observedVersion; })) {
    return false;
  }
  mState->observedVersion = mState->producedVersion;
  if (frame != nullptr) *frame = mState->lastFrame;
  if (pixels != nullptr) *pixels = mState->lastPixels;
  return true;
}

EffectFrame ReadbackOutputNode::lastFrame() const {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->lastFrame;
}

} // namespace coral
