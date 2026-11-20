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
 * @file VideoFrameInputNode.cpp
 *
 * The CPU bridge from decoded frames to the effect graph. Submission copies/converts on the application
 * thread into a bounded queue; the render thread consumes one frame per render and uploads it into one
 * reusable texture. Queue overflow follows the engine's DropOldest/Block mode.
 */

#include <effect/VideoFrameInputNode.h>

#include <libyuv/convert_argb.h>

#include "effect/EffectInternal.h"
#include "effect/EffectRenderCore.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>


namespace coral {

struct VideoFrameInputNode::State {
  struct QueuedFrame {
    std::vector<uint8_t> pixels;
    std::vector<uint8_t> y;
    std::vector<uint8_t> u;
    std::vector<uint8_t> v;
    uint32_t yRowBytes = 0;
    uint32_t uRowBytes = 0;
    uint32_t vRowBytes = 0;
    EffectFrameSource source = EffectFrameSource::Planes;
    std::shared_ptr<void> hardwareBuffer;
    bool bt2020 = false;
    bool chromaCositedEven = true;
    bool yuv = false;
    bool bt709 = false;
    bool fullRange = false;
    bool p010 = false;
    bool hdr = false;
    EffectHdrTransfer hdrTransfer = EffectHdrTransfer::PQ;
    float hdr10PlusTargetedNits = 0.0f;
    float hdr10PlusAverageMaxRgb = 0.0f;
    float hdr10PlusFractionBrightPixels = 0.0f;
    uint32_t hdrMaxContentLightLevel = 0;
    uint32_t hdrMaxFrameAverageLightLevel = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    RenderTime timestamp;
  };

  State(const uint32_t w, const uint32_t h, const uint32_t depth)
          : width(w), height(h), capacity(depth == 0 ? 1 : depth) {}

  mutable std::mutex mutex;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t capacity = 1;
  std::deque<QueuedFrame> queue;
  EffectBackpressureMode backpressure = EffectBackpressureMode::DropOldest;
  bool repeat = true;
  bool gpuConversion = false;
  EffectPixelFormat gpuOutputFormat = EffectPixelFormat::RGBA8;
  EffectToneMapMode toneMapMode = EffectToneMapMode::Reinhard;
  float exposure = 1.0f;
  float paperWhiteNits = 100.0f;
  uint64_t dropped = 0;
};

namespace {

uint8_t clampByte(const int32_t value) {
  return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

bool copyRgba(const VideoFrameDesc& frame, std::vector<uint8_t>& out, std::string& error) {
  const uint64_t rowBytes = static_cast<uint64_t>(frame.width) * 4;
  const VideoFramePlane& plane = frame.planes[0];
  if (plane.data == nullptr || plane.rowBytes < rowBytes) {
    error = "the video frame's RGBA plane is missing or its row is too short";
    return false;
  }
  out.resize(static_cast<size_t>(rowBytes) * frame.height);
  const bool swapRedBlue = frame.format == EffectPixelFormat::BGRA8;
  for (uint32_t row = 0; row < frame.height; ++row) {
    const uint8_t* source = plane.data + static_cast<size_t>(row) * plane.rowBytes;
    uint8_t* destination = out.data() + static_cast<size_t>(row) * rowBytes;
    if (!swapRedBlue) {
      std::memcpy(destination, source, static_cast<size_t>(rowBytes));
    } else {
      for (uint32_t x = 0; x < frame.width; ++x) {
        destination[x * 4 + 0] = source[x * 4 + 2];
        destination[x * 4 + 1] = source[x * 4 + 1];
        destination[x * 4 + 2] = source[x * 4 + 0];
        destination[x * 4 + 3] = source[x * 4 + 3];
      }
    }
  }
  return true;
}

bool copyYuv420(const VideoFrameDesc& frame, std::vector<uint8_t>& out, std::string& error) {
  if (frame.planeCount < 3 || frame.planes[0].data == nullptr || frame.planes[1].data == nullptr ||
      frame.planes[2].data == nullptr) {
    error = "the YUV420 frame needs Y, U and V planes";
    return false;
  }
  const uint32_t chromaWidth = (frame.width + 1) / 2;
  const uint32_t chromaHeight = (frame.height + 1) / 2;
  if (frame.planes[0].rowBytes < frame.width || frame.planes[1].rowBytes < chromaWidth ||
      frame.planes[2].rowBytes < chromaWidth) {
    error = "a YUV420 plane row is too short";
    return false;
  }

  // The destination is RGBA (R in byte 0), while libyuv's `...ToARGB` names a 32-bit ARGB word, which is
  // BGRA in memory. Swap the chroma pointers and use the mirrored `kYvu*` matrix, exactly as libyuv's own
  // `...ToABGRMatrix` macros do, so the bytes the effect samples are RGBA rather than BGRA.
  const struct libyuv::YuvConstants* matrixVU = &libyuv::kYvuI601Constants;
  if (frame.bt2020) {
    matrixVU = frame.fullRange ? &libyuv::kYvuV2020Constants : &libyuv::kYvu2020Constants;
  } else if (frame.bt709) {
    matrixVU = frame.fullRange ? &libyuv::kYvuF709Constants : &libyuv::kYvuH709Constants;
  } else if (frame.fullRange) {
    matrixVU = &libyuv::kYvuJPEGConstants;
  }

  if (frame.p010) {
    if (frame.planes[0].rowBytes < frame.width * 2 || frame.planes[1].rowBytes < chromaWidth * 2 ||
        frame.planes[2].rowBytes < chromaWidth * 2) {
      error = "a P010 plane row is too short";
      return false;
    }
    // The macro expands its matrix argument to `<name>VU`, so it must receive a variable called `matrix`.
    const struct libyuv::YuvConstants* matrix = matrixVU;
    out.resize(static_cast<size_t>(frame.width) * frame.height * 4);
    const auto* srcY = reinterpret_cast<const uint16_t*>(frame.planes[0].data);
    const auto* srcU = reinterpret_cast<const uint16_t*>(frame.planes[1].data);
    const auto* srcV = reinterpret_cast<const uint16_t*>(frame.planes[2].data);
    if (libyuv::I010ToABGRMatrix(srcY, static_cast<int>(frame.planes[0].rowBytes / 2),
          srcU, static_cast<int>(frame.planes[1].rowBytes / 2),
          srcV, static_cast<int>(frame.planes[2].rowBytes / 2),
          out.data(), static_cast<int>(frame.width * 4), matrix, static_cast<int>(frame.width),
          static_cast<int>(frame.height)) != 0) {
      error = "the P010 frame could not be converted to RGBA";
      return false;
    }
    (void) chromaHeight;
    return true;
  }

  out.resize(static_cast<size_t>(frame.width) * frame.height * 4);
  if (libyuv::I420ToARGBMatrix(frame.planes[0].data, static_cast<int>(frame.planes[0].rowBytes),
        frame.planes[2].data, static_cast<int>(frame.planes[2].rowBytes),
        frame.planes[1].data, static_cast<int>(frame.planes[1].rowBytes),
        out.data(), static_cast<int>(frame.width * 4), matrixVU, static_cast<int>(frame.width),
        static_cast<int>(frame.height)) != 0) {
    error = "the YUV420 frame could not be converted to RGBA";
    return false;
  }
  return true;
}

class VideoFrameRuntime final : public detail::InputRuntime {
public:
  explicit VideoFrameRuntime(std::shared_ptr<VideoFrameInputNode::State> state) : mState(std::move(state)) {}
  ~VideoFrameRuntime() override { releaseResources(); }

  void setBackpressure(const EffectBackpressureMode mode) override {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    mState->backpressure = mode;
  }

  bool produce(detail::FrameContext& context, detail::FrameView& view, EffectFrame& metadata,
               std::string& error) override {
    if (context.core == nullptr || context.commands == nullptr) {
      error = "the video frame input has no render context";
      return false;
    }

    VideoFrameInputNode::State::QueuedFrame frame;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      if (mState->queue.empty()) {
        // "Nothing has been submitted yet" is not a failure: the caller retries on the next frame (the
        // executor turns an empty error into Busy, or repeats the last frame). Reporting it as an error
        // made a live preview fail hard on the first tick before its first frame arrived.
        return false;
      }
      frame = std::move(mState->queue.front());
      mState->queue.pop_front();
    }

    if (frame.width == 0 || frame.height == 0) {
      error = "the video frame has a zero size";
      return false;
    }
    bool gpuConversion = false;
  EffectPixelFormat gpuOutputFormat = EffectPixelFormat::RGBA8;
  EffectToneMapMode toneMapMode = EffectToneMapMode::Reinhard;
  float exposure = 1.0f;
  float paperWhiteNits = 100.0f;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      gpuConversion = mState->gpuConversion;
    }
    if (frame.source == EffectFrameSource::HardwareBuffer) {
      if (!context.core->convertHardwareBufferYuv(context.commands, frame.hardwareBuffer.get(),
                  frame.width, frame.height, frame.bt2020, frame.fullRange, frame.p010, frame.hdr,
                  frame.hdrTransfer, gpuOutputFormat, toneMapMode, exposure, paperWhiteNits,
                  frame.hdr10PlusTargetedNits, frame.hdr10PlusAverageMaxRgb,
                  frame.hdr10PlusFractionBrightPixels,
                  static_cast<float>(frame.hdrMaxContentLightLevel),
                  static_cast<float>(frame.hdrMaxFrameAverageLightLevel), mConverted, error)) {
        return false;
      }
      view = mConverted.view();
      view.format = gpuOutputFormat;
      view.timestamp = frame.timestamp;
      metadata = detail::FrameAccess::make(detail::allocateFrameId(context), frame.width, frame.height,
              gpuOutputFormat, EffectColorSpace::SRGB, frame.timestamp);
      return true;
    }
    if (frame.yuv && gpuConversion) {
      if (!context.core->convertYuv420(context.commands, frame.width, frame.height, frame.y.data(), frame.yRowBytes,
                  frame.u.data(), frame.uRowBytes, frame.v.data(), frame.vRowBytes, frame.bt709, frame.fullRange, frame.p010, frame.hdr, frame.hdrTransfer, gpuOutputFormat, toneMapMode,
                  exposure, paperWhiteNits, frame.hdr10PlusTargetedNits, frame.hdr10PlusAverageMaxRgb,
                  frame.hdr10PlusFractionBrightPixels, static_cast<float>(frame.hdrMaxContentLightLevel),
                   static_cast<float>(frame.hdrMaxFrameAverageLightLevel), mConverted, error)) {
        return false;
      }
      view = mConverted.view();
      view.format = gpuOutputFormat;
      view.timestamp = frame.timestamp;
      metadata = detail::FrameAccess::make(detail::allocateFrameId(context), frame.width, frame.height,
              gpuOutputFormat, EffectColorSpace::SRGB, frame.timestamp);
      return true;
    }
    if (frame.yuv) {
      const VideoFrameDesc convertedFrame = [&] {
        VideoFrameDesc value;
        value.width = frame.width;
        value.height = frame.height;
        value.format = EffectPixelFormat::YUV420;
        value.planes[0] = { frame.y.data(), frame.yRowBytes };
        value.planes[1] = { frame.u.data(), frame.uRowBytes };
        value.planes[2] = { frame.v.data(), frame.vRowBytes };
        value.planeCount = 3;
        value.bt709 = frame.bt709;
        value.fullRange = frame.fullRange;
        value.p010 = frame.p010;
        value.hdr = frame.hdr;
        value.hdrTransfer = frame.hdrTransfer;
        value.hdr10PlusTargetedNits = frame.hdr10PlusTargetedNits;
        value.hdr10PlusAverageMaxRgb = frame.hdr10PlusAverageMaxRgb;
        value.hdr10PlusFractionBrightPixels = frame.hdr10PlusFractionBrightPixels;
        value.hdrMaxContentLightLevel = frame.hdrMaxContentLightLevel;
        value.hdrMaxFrameAverageLightLevel = frame.hdrMaxFrameAverageLightLevel;
        return value;
      }();
      if (!copyYuv420(convertedFrame, frame.pixels, error)) return false;
    }
    backend::Device* const device = context.core->device();
    if (!mTexture.valid() || frame.width != mTextureWidth || frame.height != mTextureHeight) {
      if (mTexture.valid() && mDevice != nullptr) mDevice->destroyTexture(mTexture);
      backend::TextureDescriptor descriptor = backend::TextureDescriptor::texture2D(
          frame.width, frame.height, backend::PixelFormat::R8G8B8A8_UNORM,
          static_cast<backend::TextureUsages>(backend::TextureUsage::Sampled));
      descriptor.label = "effect/video-frame";
      if (!backend::isSuccess(device->createTexture(descriptor, &mTexture))) {
        error = "the video frame texture could not be created";
        return false;
      }
      mDevice = device;
      mTextureWidth = frame.width;
      mTextureHeight = frame.height;
    }

    backend::TextureCopyRegion region;
    region.textureOrigin = { 0, 0, 0 };
    region.size = { frame.width, frame.height, 1 };
    region.mipLevel = 0;
    region.baseArrayLayer = 0;
    region.arrayLayerCount = 1;
    if (!backend::isSuccess(context.commands->writeTexture(mTexture, region, frame.pixels.data(),
                                                           frame.pixels.size(), frame.width * 4))) {
      error = "the video frame pixels could not be uploaded";
      return false;
    }

    view.texture = mTexture;
    view.width = frame.width;
    view.height = frame.height;
    view.format = EffectPixelFormat::RGBA8;
    view.colorSpace = EffectColorSpace::SRGB;
    view.timestamp = frame.timestamp;
    view.external = true;
    metadata = detail::FrameAccess::make(detail::allocateFrameId(context), frame.width, frame.height,
                                         EffectPixelFormat::RGBA8, EffectColorSpace::SRGB, frame.timestamp);
    return true;
  }

  void releaseResources() override {
    if (mDevice != nullptr && mTexture.valid()) mDevice->destroyTexture(mTexture);
    mConverted.reset();
    mTexture = backend::TextureHandle::invalid();
    mDevice = nullptr;
    mTextureWidth = 0;
    mTextureHeight = 0;
  }

private:
  std::shared_ptr<VideoFrameInputNode::State> mState;
  backend::Device* mDevice = nullptr;
  backend::TextureHandle mTexture = backend::TextureHandle::invalid();
  detail::FrameLease mConverted;
  uint32_t mTextureWidth = 0;
  uint32_t mTextureHeight = 0;
};

} // namespace

VideoFrameInputNode::VideoFrameInputNode(const uint32_t width, const uint32_t height, const uint32_t queueDepth)
        : EffectInputNode(),
          mState(std::make_shared<State>(width, height, queueDepth)) {
  setInputRuntime(std::make_shared<VideoFrameRuntime>(mState));
}

VideoFrameInputNode::~VideoFrameInputNode() = default;

std::shared_ptr<VideoFrameInputNode> VideoFrameInputNode::create(const uint32_t width, const uint32_t height,
                                                                 const uint32_t queueDepth) {
  if (width == 0 || height == 0) return nullptr;
  return std::shared_ptr<VideoFrameInputNode>(new VideoFrameInputNode(width, height, queueDepth));
}

EffectResult VideoFrameInputNode::submitHardwareBuffer(std::shared_ptr<void> buffer, const uint32_t width,
        const uint32_t height, const bool bt2020, const bool fullRange, const bool p010,
        const bool hdr, const bool hlg, const RenderTime timestamp) {
  if (buffer == nullptr || width == 0 || height == 0) return EffectResult::InvalidArgument;
  VideoFrameDesc frame;
  frame.width = width;
  frame.height = height;
  frame.format = EffectPixelFormat::YUV420;
  frame.source = EffectFrameSource::HardwareBuffer;
  frame.hardwareBuffer = std::move(buffer);
  frame.bt2020 = bt2020;
  frame.bt709 = !bt2020;
  frame.fullRange = fullRange;
  frame.p010 = p010;
  frame.hdr = hdr;
  frame.hdrTransfer = hlg ? EffectHdrTransfer::HLG : EffectHdrTransfer::PQ;
  frame.timestamp = timestamp;
  return submit(frame);
}

EffectResult VideoFrameInputNode::submit(const PixelBufferView& view) {
  if (!view.hasCpuPlanes()) {
    // Platform nodes override this entry point for their native texture/buffer path.
    return EffectResult::Unsupported;
  }
  VideoFrameDesc frame;
  frame.width = view.width;
  frame.height = view.height;
  frame.format = view.format;
  frame.colorSpace = view.colorSpace;
  frame.bt709 = view.bt709;
  frame.fullRange = view.fullRange;
  frame.p010 = view.p010;
  frame.hdr = view.hdr;
  frame.hdrTransfer = view.hdrTransfer;
  frame.hdr10PlusTargetedNits = view.hdr10PlusTargetedNits;
  frame.hdr10PlusAverageMaxRgb = view.hdr10PlusAverageMaxRgb;
  frame.hdr10PlusFractionBrightPixels = view.hdr10PlusFractionBrightPixels;
  frame.hdrMaxContentLightLevel = view.hdrMaxContentLightLevel;
  frame.hdrMaxFrameAverageLightLevel = view.hdrMaxFrameAverageLightLevel;
  frame.bt2020 = view.bt2020;
  frame.chromaCositedEven = view.chromaCositedEven;
  frame.source = view.source;
  frame.timestamp = view.timestamp;
  frame.planeCount = view.planeCount;
  for (uint32_t index = 0; index < view.planeCount && index < 3; ++index) {
    frame.planes[index] = { view.planes[index].data, view.planes[index].rowBytes };
  }
  return submit(frame);
}

EffectResult VideoFrameInputNode::submit(const VideoFrameDesc& frame) {
  if (frame.width == 0 || frame.height == 0) return EffectResult::InvalidArgument;
  if (frame.source == EffectFrameSource::HardwareBuffer && frame.hardwareBuffer == nullptr) {
    return EffectResult::InvalidArgument;
  }
  if (frame.source == EffectFrameSource::Planes && frame.planeCount == 0) return EffectResult::InvalidArgument;
  if (frame.format != EffectPixelFormat::RGBA8 && frame.format != EffectPixelFormat::BGRA8 &&
      frame.format != EffectPixelFormat::YUV420) {
    return EffectResult::Unsupported;
  }

  State::QueuedFrame queued;
  queued.width = frame.width;
  queued.height = frame.height;
  queued.timestamp = frame.timestamp;
  std::string error;
  if (frame.source == EffectFrameSource::HardwareBuffer) {
    queued.source = EffectFrameSource::HardwareBuffer;
    queued.hardwareBuffer = frame.hardwareBuffer;
    queued.bt2020 = frame.bt2020;
    queued.chromaCositedEven = frame.chromaCositedEven;
    queued.bt709 = frame.bt709;
    queued.fullRange = frame.fullRange;
    queued.p010 = frame.p010;
    queued.hdr = frame.hdr;
    queued.hdrTransfer = frame.hdrTransfer;
    queued.hdr10PlusTargetedNits = frame.hdr10PlusTargetedNits;
    queued.hdr10PlusAverageMaxRgb = frame.hdr10PlusAverageMaxRgb;
    queued.hdr10PlusFractionBrightPixels = frame.hdr10PlusFractionBrightPixels;
    queued.hdrMaxContentLightLevel = frame.hdrMaxContentLightLevel;
    queued.hdrMaxFrameAverageLightLevel = frame.hdrMaxFrameAverageLightLevel;
  } else if (frame.format == EffectPixelFormat::YUV420) {
    if (frame.planeCount < 3 || frame.planes[0].data == nullptr || frame.planes[1].data == nullptr ||
        frame.planes[2].data == nullptr) {
      return EffectResult::InvalidArgument;
    }
    const uint32_t chromaWidth = (frame.width + 1) / 2;
    const uint32_t yRowBytes = frame.p010 ? frame.width * 2 : frame.width;
    const uint32_t chromaRowBytes = frame.p010 ? chromaWidth * 2 : chromaWidth;
    if (frame.planes[0].rowBytes < yRowBytes || frame.planes[1].rowBytes < chromaRowBytes ||
        frame.planes[2].rowBytes < chromaRowBytes) {
      return EffectResult::InvalidArgument;
    }
    const auto copyPlane = [](const VideoFramePlane& source, const uint32_t rowBytes, const uint32_t rows,
                               std::vector<uint8_t>& out) {
      out.resize(static_cast<size_t>(rowBytes) * rows);
      for (uint32_t row = 0; row < rows; ++row) {
        std::memcpy(out.data() + static_cast<size_t>(row) * rowBytes,
                source.data + static_cast<size_t>(row) * source.rowBytes, rowBytes);
      }
    };
    copyPlane(frame.planes[0], frame.planes[0].rowBytes, frame.height, queued.y);
    copyPlane(frame.planes[1], frame.planes[1].rowBytes, (frame.height + 1) / 2, queued.u);
    copyPlane(frame.planes[2], frame.planes[2].rowBytes, (frame.height + 1) / 2, queued.v);
    queued.yRowBytes = frame.planes[0].rowBytes;
    queued.uRowBytes = frame.planes[1].rowBytes;
    queued.vRowBytes = frame.planes[2].rowBytes;
    queued.yuv = true;
    queued.bt709 = frame.bt709;
    queued.fullRange = frame.fullRange;
    queued.p010 = frame.p010;
    queued.hdr = frame.hdr;
    queued.hdrTransfer = frame.hdrTransfer;
    queued.hdr10PlusTargetedNits = frame.hdr10PlusTargetedNits;
    queued.hdr10PlusAverageMaxRgb = frame.hdr10PlusAverageMaxRgb;
    queued.hdr10PlusFractionBrightPixels = frame.hdr10PlusFractionBrightPixels;
    queued.hdrMaxContentLightLevel = frame.hdrMaxContentLightLevel;
    queued.hdrMaxFrameAverageLightLevel = frame.hdrMaxFrameAverageLightLevel;
  } else if (!copyRgba(frame, queued.pixels, error)) {
    return EffectResult::InvalidArgument;
  }

  const std::lock_guard<std::mutex> lock(mState->mutex);
  if (mState->queue.size() >= mState->capacity) {
    if (mState->backpressure == EffectBackpressureMode::Block) return EffectResult::Busy;
    mState->queue.pop_front();
    ++mState->dropped;
  }
  mState->width = frame.width;
  mState->height = frame.height;
  mState->queue.push_back(std::move(queued));
  return EffectResult::Success;
}

bool VideoFrameInputNode::hasFrame() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return !mState->queue.empty();
}

bool VideoFrameInputNode::repeatLastFrame() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->repeat;
}

void VideoFrameInputNode::setRepeatLastFrame(const bool repeat) {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->repeat = repeat;
}

void VideoFrameInputNode::setGpuConversion(const bool enabled) {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->gpuConversion = enabled;
}

void VideoFrameInputNode::setGpuOutputFormat(const EffectPixelFormat format) {
  if (format != EffectPixelFormat::RGBA8 && format != EffectPixelFormat::RGBA16F) return;
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->gpuOutputFormat = format;
}

void VideoFrameInputNode::setHdrToneMap(const EffectToneMapMode mode, const float exposure,
                                        const float paperWhiteNits) {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->toneMapMode = mode;
  mState->exposure = exposure > 0.0f ? exposure : 1.0f;
  mState->paperWhiteNits = paperWhiteNits > 0.0f ? paperWhiteNits : 100.0f;
}

uint32_t VideoFrameInputNode::width() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->width;
}

uint32_t VideoFrameInputNode::height() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->height;
}

} // namespace coral
