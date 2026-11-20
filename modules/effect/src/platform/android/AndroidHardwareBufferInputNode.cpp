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

#include <effect/platform/android/AndroidHardwareBufferInputNode.h>

#include "effect/EffectRenderCore.h"

#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace coral {

struct AndroidHardwareBufferInputNode::State {
  std::mutex mutex;
  AHardwareBuffer* buffer = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  EffectPixelFormat format = EffectPixelFormat::RGBA8;
  AndroidHardwareBufferYuvDesc yuv;
  EffectPixelFormat yuvOutputFormat = EffectPixelFormat::RGBA8;
  EffectToneMapMode yuvToneMap = EffectToneMapMode::Reinhard;
  float yuvExposure = 1.0f;
  float yuvPaperWhiteNits = 100.0f;
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
    case EffectPixelFormat::RGBA16F:
      return backend::PixelFormat::R16G16B16A16_SFLOAT;
    default:
      return backend::PixelFormat::Undefined;
  }
}

class AndroidHardwareBufferRuntime final : public detail::InputRuntime {
public:
  explicit AndroidHardwareBufferRuntime(std::shared_ptr<AndroidHardwareBufferInputNode::State> state)
          : mState(std::move(state)) {}

  ~AndroidHardwareBufferRuntime() override { releaseResources(); }

  bool produce(detail::FrameContext& context, detail::FrameView& view, EffectFrame& metadata,
               std::string& error) override {
    if (context.core == nullptr) {
      error = "the AHardwareBuffer input has no render context";
      return false;
    }
    pollRetired();

    AHardwareBuffer* buffer = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    EffectPixelFormat format = EffectPixelFormat::RGBA8;
    AndroidHardwareBufferYuvDesc yuv;
    EffectPixelFormat yuvOutputFormat = EffectPixelFormat::RGBA8;
    EffectToneMapMode yuvToneMap = EffectToneMapMode::Reinhard;
    float yuvExposure = 1.0f;
    float yuvPaperWhiteNits = 100.0f;
    uint32_t version = 0;
    RenderTime timestamp;
    bool hasTimestamp = false;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      buffer = mState->buffer;
      width = mState->width;
      height = mState->height;
      format = mState->format;
      yuv = mState->yuv;
      yuvOutputFormat = mState->yuvOutputFormat;
      yuvToneMap = mState->yuvToneMap;
      yuvExposure = mState->yuvExposure;
      yuvPaperWhiteNits = mState->yuvPaperWhiteNits;
      version = mState->version;
      timestamp = mState->timestamp;
      hasTimestamp = mState->hasTimestamp;
    }
    if (buffer == nullptr || width == 0 || height == 0) {
      // No frame has been submitted yet: the executor treats an empty error as "try again next frame",
      // which is what a live producer needs on the ticks before its first buffer arrives.
      return false;
    }

    mDevice = context.core->device();
    if (mDevice == nullptr || !mDevice->supportsNativeTextureImport(backend::NativeTextureKind::AndroidHardwareBuffer)) {
      error = "this backend cannot import an AHardwareBuffer (Vulkan is required); backend=" +
              std::to_string(static_cast<int>(mDevice->getInfo().backend));
      return false;
    }

    const RenderTime effectiveTimestamp = hasTimestamp ? timestamp : context.timestamp;

    // ## YUV path
    //
    // The driver names an opaque external format; the conversion pass asks for it and samples through a
    // sampler YCbCr conversion. This is the same path `VideoFrameInputNode::submitHardwareBuffer` takes,
    // but kept here so a platform AHB producer can use the node directly.
    const bool glExternal = mDevice->getInfo().backend == backend::Backend::OpenGL &&
            mDevice->supportsNativeTextureImport(backend::NativeTextureKind::AndroidHardwareBuffer);
    if (format == EffectPixelFormat::YUV420 && !glExternal) {
      if (!mDevice->supportsYuvHardwareBufferImport()) {
        error = "this backend cannot import a YUV AHardwareBuffer; use the CPU plane path";
        return false;
      }
      if (yuvOutputFormat != EffectPixelFormat::RGBA8 && yuvOutputFormat != EffectPixelFormat::RGBA16F) {
        yuvOutputFormat = EffectPixelFormat::RGBA8;
      }
      if (!context.core->convertHardwareBufferYuv(context.commands, buffer, width, height, yuv.bt2020,
                  yuv.fullRange, false, yuv.hdr,
                  yuv.hlg ? EffectHdrTransfer::HLG : EffectHdrTransfer::PQ, yuvOutputFormat, yuvToneMap,
                  yuvExposure, yuvPaperWhiteNits, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, mConverted, error)) {
        return false;
      }
      view = mConverted.view();
      view.format = yuvOutputFormat;
      view.timestamp = effectiveTimestamp;
      metadata = detail::FrameAccess::make(detail::allocateFrameId(context), width, height, yuvOutputFormat,
                                           EffectColorSpace::SRGB, effectiveTimestamp);
      return true;
    }

    if (version != mVersion || !mImported.valid()) {
      backend::NativeTextureDescriptor descriptor;
      descriptor.kind = backend::NativeTextureKind::AndroidHardwareBuffer;
      descriptor.platformHandle = buffer;
      descriptor.width = width;
      descriptor.height = height;
      descriptor.format = toPixelFormat(format);
      if (!backend::isSuccess(mDevice->importNativeTexture(descriptor, &mImported))) {
        mImported = backend::TextureHandle::invalid();
        error = "the AHardwareBuffer could not be imported";
        return false;
      }
      mVersion = version;
    }

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
    mConverted.reset();
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

  std::shared_ptr<AndroidHardwareBufferInputNode::State> mState;
  backend::Device* mDevice = nullptr;
  backend::TextureHandle mImported = backend::TextureHandle::invalid();
  detail::FrameLease mConverted;
  std::vector<Retired> mRetired;
  uint32_t mVersion = 0;
};

} // namespace

AndroidHardwareBufferInputNode::AndroidHardwareBufferInputNode(AHardwareBuffer* const buffer, const uint32_t width,
                                                               const uint32_t height,
                                                               const EffectPixelFormat format)
        : EffectInputNode(), mState(std::make_shared<State>()) {
  mState->buffer = buffer;
  mState->width = width;
  mState->height = height;
  mState->format = format;
  setInputRuntime(std::make_shared<AndroidHardwareBufferRuntime>(mState));
}

AndroidHardwareBufferInputNode::~AndroidHardwareBufferInputNode() = default;

std::shared_ptr<AndroidHardwareBufferInputNode> AndroidHardwareBufferInputNode::create(
        AHardwareBuffer* const buffer, const uint32_t width, const uint32_t height, const EffectPixelFormat format) {
  if (buffer == nullptr || width == 0 || height == 0) return nullptr;
  if (format != EffectPixelFormat::RGBA8 && format != EffectPixelFormat::BGRA8 &&
      format != EffectPixelFormat::RGBA16F && format != EffectPixelFormat::YUV420) {
    return nullptr;
  }
  return std::shared_ptr<AndroidHardwareBufferInputNode>(
          new AndroidHardwareBufferInputNode(buffer, width, height, format));
}

std::shared_ptr<AndroidHardwareBufferInputNode> AndroidHardwareBufferInputNode::create(
        const uint32_t width, const uint32_t height, const EffectPixelFormat format) {
  if (width == 0 || height == 0) return nullptr;
  if (format != EffectPixelFormat::RGBA8 && format != EffectPixelFormat::BGRA8 &&
      format != EffectPixelFormat::RGBA16F && format != EffectPixelFormat::YUV420) {
    return nullptr;
  }
  return std::shared_ptr<AndroidHardwareBufferInputNode>(
          new AndroidHardwareBufferInputNode(nullptr, width, height, format));
}

bool AndroidHardwareBufferInputNode::hasFrame() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->buffer != nullptr;
}

bool AndroidHardwareBufferInputNode::repeatLastFrame() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->repeat;
}

void AndroidHardwareBufferInputNode::setRepeatLastFrame(const bool repeat) {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->repeat = repeat;
}

uint32_t AndroidHardwareBufferInputNode::width() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->width;
}

uint32_t AndroidHardwareBufferInputNode::height() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->height;
}

EffectResult AndroidHardwareBufferInputNode::setBuffer(AHardwareBuffer* const buffer, const uint32_t width,
                                                       const uint32_t height, const EffectPixelFormat format) {
  if (buffer == nullptr || width == 0 || height == 0) return EffectResult::InvalidArgument;
  if (format != EffectPixelFormat::RGBA8 && format != EffectPixelFormat::BGRA8 &&
      format != EffectPixelFormat::RGBA16F && format != EffectPixelFormat::YUV420) {
    return EffectResult::Unsupported;
  }
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->buffer = buffer;
  mState->width = width;
  mState->height = height;
  mState->format = format;
  mState->timestamp = {};
  mState->hasTimestamp = false;
  ++mState->version;
  return EffectResult::Success;
}

EffectResult AndroidHardwareBufferInputNode::submit(const PixelBufferView& view) {
  if (view.nativeKind != NativeHandleKind::AndroidHardwareBuffer || view.nativeHandle == nullptr) {
    return EffectResult::Unsupported;
  }
  return submit(static_cast<AHardwareBuffer*>(view.nativeHandle), view.width, view.height,
    view.format, view.timestamp);
}

EffectResult AndroidHardwareBufferInputNode::submit(AHardwareBuffer* const buffer, const uint32_t width,
                                                    const uint32_t height, const EffectPixelFormat format,
                                                    const RenderTime timestamp) {
  if (buffer == nullptr || width == 0 || height == 0) return EffectResult::InvalidArgument;
  if (format != EffectPixelFormat::RGBA8 && format != EffectPixelFormat::BGRA8 &&
      format != EffectPixelFormat::RGBA16F && format != EffectPixelFormat::YUV420) {
    return EffectResult::Unsupported;
  }
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->buffer = buffer;
  mState->width = width;
  mState->height = height;
  mState->format = format;
  mState->timestamp = timestamp;
  mState->hasTimestamp = timestamp.isValid();
  ++mState->version;
  return EffectResult::Success;
}

void AndroidHardwareBufferInputNode::setTimestamp(const RenderTime timestamp) {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->timestamp = timestamp;
  mState->hasTimestamp = timestamp.isValid();
}

void AndroidHardwareBufferInputNode::setYuvDescription(const AndroidHardwareBufferYuvDesc& description) {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->yuv = description;
}

void AndroidHardwareBufferInputNode::setYuvOutputFormat(const EffectPixelFormat format) {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->yuvOutputFormat =
          format == EffectPixelFormat::RGBA16F ? EffectPixelFormat::RGBA16F : EffectPixelFormat::RGBA8;
}

void AndroidHardwareBufferInputNode::setYuvToneMap(const EffectToneMapMode mode, const float exposure,
                                                   const float paperWhiteNits) {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->yuvToneMap = mode;
  mState->yuvExposure = exposure > 0.0f ? exposure : 1.0f;
  mState->yuvPaperWhiteNits = paperWhiteNits > 0.0f ? paperWhiteNits : 100.0f;
}

} // namespace coral
