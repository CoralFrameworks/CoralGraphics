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
 * @file TextureOutputNode.cpp
 *
 * The GPU -> GPU output: each committed frame is copied into one of a few ring textures, and the
 * caller samples it on the engine's own device. A copy rather than a retained pooled frame keeps the
 * lifetime answer simple: the pool may recycle a frame as soon as its submission completes, while the
 * UI records against the copy one or two frames later.
 */

#include <effect/TextureOutputNode.h>

#include "EffectInternal.h"
#include "EffectRenderCore.h"

#include <backend/Descriptors.h>
#include <backend/Handles.h>
#include <backend/Types.h>
#include <backend/TestHooks.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace coral {

struct TextureOutputNode::State {
  mutable std::mutex mutex;
  FrameCallback callback;
  Frame latest;
  std::shared_ptr<coral::detail::GpuSubmission> lastSubmission;
  EffectOutputConfig config;
  uint32_t bufferCount = 3;
  bool active = true;
};

namespace {

using Frame = TextureOutputNode::Frame;

class TextureRuntime final : public coral::detail::OutputRuntime {
public:
  explicit TextureRuntime(std::shared_ptr<TextureOutputNode::State> state) : mState(std::move(state)) {}

  ~TextureRuntime() override { releaseResources(); }

  [[nodiscard]] bool isReady() const noexcept override { return true; }

  bool consume(coral::detail::FrameContext& context, const coral::detail::FrameView& view,
               std::string& error) override {
    if (context.commands == nullptr || context.core == nullptr) {
      error = "the texture output has no command buffer to record its copy into";
      return false;
    }
    if (context.core->device() == nullptr) {
      error = "the texture output has no device";
      return false;
    }
    if (!view.isValid() || view.width == 0 || view.height == 0) {
      error = "the texture output received an invalid frame";
      return false;
    }
    if (view.format != EffectPixelFormat::RGBA8 && view.format != EffectPixelFormat::BGRA8 &&
        view.format != EffectPixelFormat::RGBA16F) {
      error = "the texture output supports RGBA8/BGRA8/RGBA16F frames only";
      return false;
    }

    EffectOutputConfig config;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      config = mState->config;
    }
    if ((config.width != 0 && config.width != view.width) ||
        (config.height != 0 && config.height != view.height)) {
      error = "the texture output does not rescale: the configured size differs from the frame";
      return false;
    }

    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      if (!mState->active) {
        // A hidden preview consumes its frame without copying it. The frame in the ring stays valid,
        // so a view that comes back shows the last picture immediately, and the consumer's wait is
        // the only thing that stopped -- the graph itself keeps running.
        return true;
      }
    }

    backend::Device* const device = context.core->device();
    if (backend::test::consumeDeviceLossInjection()) {
      error = "injected device loss at the texture output";
      return false;
    }

    if (!ensureSlots(device, view, error)) {
      return false;
    }

    size_t index = mSlots.size();
    for (size_t attempt = 0; attempt < mSlots.size(); ++attempt) {
      const size_t candidate = (mNextSlot + attempt) % mSlots.size();
      Slot& slot = mSlots[candidate];
      if (slot.submission != nullptr && !slot.submission->isDone()) continue;
      if (slot.lease == nullptr || slot.lease.use_count() == 1) {
        index = candidate;
        break;
      }
      // The only extra reference may be `State::latest`, which is replaced by this frame anyway.
      bool latestOnly = false;
      {
        const std::lock_guard<std::mutex> lock(mState->mutex);
        latestOnly = mState->latest.keepAlive == slot.lease && slot.lease.use_count() == 2;
      }
      if (latestOnly) {
        index = candidate;
        break;
      }
    }
    if (index == mSlots.size()) {
      // Every slot is held by a consumer. A preview drops this frame instead of overwriting a
      // texture the consumer is about to sample; the producer's next attempt retries.
      return true;
    }

    Slot& slot = mSlots[index];
    slot.submission.reset();

    backend::TextureCopyRegion region;
    region.textureOrigin = { 0, 0, 0 };
    region.size = { view.width, view.height, 1 };
    region.mipLevel = 0;
    region.baseArrayLayer = 0;
    region.arrayLayerCount = 1;
    if (!backend::isSuccess(context.commands->copyTextureToTexture(view.texture, slot.texture, region))) {
      error = "the texture output could not record its copied frame";
      return false;
    }

    mPendingIndex = index;
    mPending = true;
    mPendingMetadata = coral::detail::FrameAccess::make(
      coral::detail::allocateFrameId(context), view.width, view.height, view.format, view.colorSpace,
      view.timestamp.isValid() ? view.timestamp : context.timestamp);
    mNextSlot = index + 1;
    return true;
  }

  void afterSubmit(const std::shared_ptr<coral::detail::GpuSubmission>& submission) override {
    if (!mPending || submission == nullptr) {
      mPending = false;
      return;
    }

    Frame frame;
    if (mPendingIndex >= mSlots.size()) {
      mPending = false;
      return;
    }
    Slot& slot = mSlots[mPendingIndex];
    slot.submission = submission;
    frame.texture = slot.texture.value();
    frame.view = slot.view.value();
    frame.width = slot.width;
    frame.height = slot.height;
    frame.metadata = mPendingMetadata;
    frame.sequence = ++mSequence;
    frame.keepAlive = std::make_shared<char>(0);
    slot.lease = frame.keepAlive;

    TextureOutputNode::FrameCallback callback;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      mState->latest = frame;
      mState->lastSubmission = submission;
      callback = mState->callback;
    }
    mPending = false;
    if (callback) {
      callback(frame);
    }
  }

  void afterAbort() override { mPending = false; }

  void releaseResources() override {
    mPending = false;
    // The last submission outlives the engine's device if it is not dropped here: the node itself is
    // owned by the caller, and a `GpuSubmission` freed at process exit would destroy its fence on a
    // device that is already gone. The engine calls this before it tears the device down, which is
    // the one moment the handle is still valid.
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      mState->lastSubmission.reset();
      mState->latest = Frame{};
    }
    if (mDevice != nullptr) {
      for (Slot& slot : mSlots) {
        if (slot.view.valid()) mDevice->destroyTextureView(slot.view);
        if (slot.texture.valid()) mDevice->destroyTexture(slot.texture);
        slot.submission.reset();
        slot.lease.reset();
      }
    }
    mSlots.clear();
    mDevice = nullptr;
  }

private:
  struct Slot {
    backend::TextureHandle texture = backend::TextureHandle::invalid();
    backend::TextureViewHandle view = backend::TextureViewHandle::invalid();
    backend::PixelFormat format = backend::PixelFormat::Undefined;
    uint32_t width = 0;
    uint32_t height = 0;
    std::shared_ptr<coral::detail::GpuSubmission> submission;
    /** Held by the last Frame handed out for this slot; prevents reuse while a consumer samples it. */
    std::shared_ptr<void> lease;
  };

  bool ensureSlots(backend::Device* const device, const coral::detail::FrameView& view, std::string& error) {
    const backend::PixelFormat format = coral::detail::toPixelFormat(view.format);
    if (format == backend::PixelFormat::Undefined) {
      error = "the texture output received an unsupported pixel format";
      return false;
    }
    if (!mSlots.empty() && mDevice == device && mSlots.front().width == view.width &&
        mSlots.front().height == view.height && mSlots.front().format == format) {
      return true;
    }

    uint32_t bufferCount = 3;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      bufferCount = std::max<uint32_t>(1, mState->bufferCount);
    }

    releaseResources();
    mDevice = device;
    mSlots.reserve(bufferCount);
    backend::TextureDescriptor textureDescriptor = backend::TextureDescriptor::texture2D(
      view.width, view.height, format, static_cast<backend::TextureUsages>(backend::TextureUsage::Sampled));
    textureDescriptor.label = "effect/texture-output";
    for (uint32_t i = 0; i < bufferCount; ++i) {
      Slot slot;
      slot.format = format;
      slot.width = view.width;
      slot.height = view.height;
      if (!backend::isSuccess(mDevice->createTexture(textureDescriptor, &slot.texture))) {
        error = "the texture output could not create its frame texture";
        releaseResources();
        return false;
      }
      backend::TextureViewDescriptor viewDescriptor;
      viewDescriptor.texture = slot.texture;
      viewDescriptor.dimension = backend::TextureViewDimension::Texture2D;
      viewDescriptor.label = "effect/texture-output";
      if (!backend::isSuccess(mDevice->createTextureView(viewDescriptor, &slot.view))) {
        mDevice->destroyTexture(slot.texture);
        error = "the texture output could not create its frame view";
        releaseResources();
        return false;
      }
      mSlots.push_back(slot);
    }
    mNextSlot = 0;
    return true;
  }

  std::shared_ptr<TextureOutputNode::State> mState;
  backend::Device* mDevice = nullptr;
  std::vector<Slot> mSlots;
  size_t mNextSlot = 0;

  bool mPending = false;
  size_t mPendingIndex = 0;
  EffectFrame mPendingMetadata;
  uint64_t mSequence = 0;
};

} // namespace anonymous

TextureOutputNode::TextureOutputNode(const Config& config)
        : mState(std::make_shared<State>()) {
  mState->bufferCount = std::max<uint32_t>(1, config.bufferCount);
  setOutputRuntime(std::make_shared<TextureRuntime>(mState));
}

TextureOutputNode::~TextureOutputNode() = default;

std::shared_ptr<TextureOutputNode> TextureOutputNode::create() {
  return create(Config{});
}

std::shared_ptr<TextureOutputNode> TextureOutputNode::create(const Config& config) {
  return std::shared_ptr<TextureOutputNode>(new TextureOutputNode(config));
}

bool TextureOutputNode::isReady() const noexcept { return true; }

EffectResult TextureOutputNode::configure(const EffectOutputConfig& config) {
  if (config.format != EffectPixelFormat::RGBA8 && config.format != EffectPixelFormat::BGRA8 &&
      config.format != EffectPixelFormat::RGBA16F) {
    detail::NodeAccess::setDiagnostics(*this, "the texture output supports RGBA8/BGRA8/RGBA16F only");
    return EffectResult::Unsupported;
  }
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->config = config;
  return EffectResult::Success;
}

void TextureOutputNode::setFrameCallback(FrameCallback callback) {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->callback = std::move(callback);
}

void TextureOutputNode::setActive(const bool active) {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->active = active;
}

bool TextureOutputNode::active() const {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->active;
}

uint32_t TextureOutputNode::bufferCount() const {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return std::max<uint32_t>(1, mState->bufferCount);
}

TextureOutputNode::Frame TextureOutputNode::latestFrame() const {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->latest;
}

void TextureOutputNode::waitForLatestFrame() {
  std::shared_ptr<coral::detail::GpuSubmission> submission;
  {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    submission = mState->lastSubmission;
  }
  if (submission != nullptr) {
    submission->wait();
  }
}

} // namespace coral
