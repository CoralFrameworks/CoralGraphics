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

#include <effect/ExternalTextureInputNode.h>

#include "effect/EffectRenderCore.h"

#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace coral {

struct ExternalTextureInputNode::State {
  std::mutex mutex;
  uint32_t glTexture = 0;
  uint32_t glTarget = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  EffectPixelFormat format = EffectPixelFormat::RGBA8;
  uint32_t version = 1;
  bool repeat = true;
  RenderTime timestamp;
  bool hasTimestamp = false;
};

namespace {

backend::PixelFormat toPixelFormat(const EffectPixelFormat format) {
  switch (format) {
    case EffectPixelFormat::RGBA8:
      return backend::PixelFormat::R8G8B8A8_UNORM;
    case EffectPixelFormat::BGRA8:
      return backend::PixelFormat::B8G8R8A8_UNORM;
    default:
      return backend::PixelFormat::Undefined;
  }
}

class ExternalTextureRuntime final : public detail::InputRuntime {
public:
  explicit ExternalTextureRuntime(std::shared_ptr<ExternalTextureInputNode::State> state)
          : mState(std::move(state)) {}

  ~ExternalTextureRuntime() override { releaseResources(); }

  bool produce(detail::FrameContext& context, detail::FrameView& view, EffectFrame& metadata,
               std::string& error) override {
    if (context.core == nullptr) {
      error = "the external texture input has no render context";
      return false;
    }
    pollRetired();

    uint32_t glTexture = 0;
    uint32_t glTarget = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    EffectPixelFormat format = EffectPixelFormat::RGBA8;
    uint32_t version = 0;
    RenderTime timestamp;
    bool hasTimestamp = false;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      glTexture = mState->glTexture;
      glTarget = mState->glTarget;
      width = mState->width;
      height = mState->height;
      format = mState->format;
      version = mState->version;
      timestamp = mState->timestamp;
      hasTimestamp = mState->hasTimestamp;
    }
    if (glTexture == 0 || glTarget == 0 || width == 0 || height == 0) {
      error = "the external texture input has no texture";
      return false;
    }

    mDevice = context.core->device();
    if (mDevice == nullptr || !mDevice->supportsNativeTextureImport(backend::NativeTextureKind::OpenGlTexture)) {
      error = "this backend cannot import an OpenGL texture";
      return false;
    }

    if (version != mVersion || !mImported.valid()) {
      backend::NativeTextureDescriptor descriptor;
      descriptor.kind = backend::NativeTextureKind::OpenGlTexture;
      descriptor.glTexture = glTexture;
      descriptor.glTarget = glTarget;
      descriptor.width = width;
      descriptor.height = height;
      descriptor.format = toPixelFormat(format);
      if (!backend::isSuccess(mDevice->importNativeTexture(descriptor, &mImported))) {
        mImported = backend::TextureHandle::invalid();
        error = "the OpenGL texture could not be imported";
        return false;
      }
      mVersion = version;
    }

    const RenderTime effectiveTimestamp = hasTimestamp ? timestamp : context.timestamp;
    view.texture = mImported;
    view.width = width;
    view.height = height;
    view.format = format;
    view.colorSpace = EffectColorSpace::SRGB;
    view.timestamp = effectiveTimestamp;
    view.external = true;
    metadata = detail::FrameAccess::make(detail::allocateFrameId(context), width, height, format,
                                         EffectColorSpace::SRGB, effectiveTimestamp);
    return true;
  }

  void afterSubmit(const std::shared_ptr<detail::GpuSubmission>& submission) override {
    if (!mImported.valid()) return;
    mRetired.push_back(Retired{ mImported, submission });
    mImported = backend::TextureHandle::invalid();
  }

  void afterAbort() override {
    if (mDevice != nullptr && mImported.valid()) mDevice->destroyTexture(mImported);
    mImported = backend::TextureHandle::invalid();
  }

  void releaseResources() override {
    if (mDevice != nullptr) {
      if (mImported.valid()) mDevice->destroyTexture(mImported);
      for (const Retired& retired : mRetired) {
        if (retired.texture.valid()) mDevice->destroyTexture(retired.texture);
      }
    }
    mRetired.clear();
    mImported = backend::TextureHandle::invalid();
    mDevice = nullptr;
    mVersion = 0;
  }

private:
  struct Retired {
    backend::TextureHandle texture = backend::TextureHandle::invalid();
    std::shared_ptr<detail::GpuSubmission> submission;
  };

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

  std::shared_ptr<ExternalTextureInputNode::State> mState;
  backend::Device* mDevice = nullptr;
  backend::TextureHandle mImported = backend::TextureHandle::invalid();
  std::vector<Retired> mRetired;
  uint32_t mVersion = 0;
};

} // namespace

ExternalTextureInputNode::ExternalTextureInputNode(const uint32_t width, const uint32_t height,
                                                   const EffectPixelFormat format)
        : EffectInputNode(), mState(std::make_shared<State>()) {
  mState->width = width;
  mState->height = height;
  mState->format = format;
  setInputRuntime(std::make_shared<ExternalTextureRuntime>(mState));
}

ExternalTextureInputNode::~ExternalTextureInputNode() = default;

std::shared_ptr<ExternalTextureInputNode> ExternalTextureInputNode::create(const uint32_t width,
                                                                           const uint32_t height,
                                                                           const EffectPixelFormat format) {
  if (width == 0 || height == 0) return nullptr;
  if (format != EffectPixelFormat::RGBA8 && format != EffectPixelFormat::BGRA8) return nullptr;
  return std::shared_ptr<ExternalTextureInputNode>(new ExternalTextureInputNode(width, height, format));
}

bool ExternalTextureInputNode::hasFrame() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->glTexture != 0;
}

bool ExternalTextureInputNode::repeatLastFrame() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->repeat;
}

void ExternalTextureInputNode::setRepeatLastFrame(const bool repeat) {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->repeat = repeat;
}

uint32_t ExternalTextureInputNode::width() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->width;
}

uint32_t ExternalTextureInputNode::height() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->height;
}

EffectResult ExternalTextureInputNode::setTexture(const uint32_t glTexture, const uint32_t glTarget,
                                                  const uint32_t width, const uint32_t height,
                                                  const EffectPixelFormat format) {
  if (glTexture == 0 || glTarget == 0 || width == 0 || height == 0) return EffectResult::InvalidArgument;
  if (format != EffectPixelFormat::RGBA8 && format != EffectPixelFormat::BGRA8) {
    return EffectResult::Unsupported;
  }
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->glTexture = glTexture;
  mState->glTarget = glTarget;
  mState->width = width;
  mState->height = height;
  mState->format = format;
  mState->timestamp = {};
  mState->hasTimestamp = false;
  ++mState->version;
  return EffectResult::Success;
}

EffectResult ExternalTextureInputNode::submit(const uint32_t glTexture, const uint32_t glTarget,
                                              const uint32_t width, const uint32_t height,
                                              const EffectPixelFormat format, const RenderTime timestamp) {
  if (glTexture == 0 || glTarget == 0 || width == 0 || height == 0) return EffectResult::InvalidArgument;
  if (format != EffectPixelFormat::RGBA8 && format != EffectPixelFormat::BGRA8) {
    return EffectResult::Unsupported;
  }
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->glTexture = glTexture;
  mState->glTarget = glTarget;
  mState->width = width;
  mState->height = height;
  mState->format = format;
  mState->timestamp = timestamp;
  mState->hasTimestamp = timestamp.isValid();
  ++mState->version;
  return EffectResult::Success;
}

void ExternalTextureInputNode::setTimestamp(const RenderTime timestamp) {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->timestamp = timestamp;
  mState->hasTimestamp = timestamp.isValid();
}

} // namespace coral
