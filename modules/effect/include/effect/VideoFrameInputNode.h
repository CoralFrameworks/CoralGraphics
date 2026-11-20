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

#ifndef CORAL_VIDEO_FRAME_INPUT_NODE_H
#define CORAL_VIDEO_FRAME_INPUT_NODE_H

#include <effect/EffectInputNode.h>
#include <effect/EffectTypes.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace coral {

/**
 * One plane of a CPU video frame.
 *
 * The data pointer is only read during `VideoFrameInputNode::submit`; the node copies/converts the pixels,
 * so a decoder may recycle its buffer as soon as the call returns. `rowBytes` is the distance between
 * consecutive rows, including padding.
 */
struct VideoFramePlane {
  const uint8_t* data = nullptr;
  uint32_t rowBytes = 0;
};

/**
 * A decoded frame handed to `VideoFrameInputNode`.
 *
 * `RGBA8`/`BGRA8` use plane 0 only. `YUV420` uses three planes (Y, U, V) in the usual I420 layout; the
 * conversion is YUV limited-range to sRGB and happens on the submitting thread; `bt709` selects the\n * BT.709 matrix instead of the default BT.601. This is the CPU bridge
 * for decoders that do not expose a platform texture; zero-copy decoder paths belong in platform nodes.
 */
struct VideoFrameDesc {
  uint32_t width = 0;
  uint32_t height = 0;
  EffectPixelFormat format = EffectPixelFormat::RGBA8;
  EffectColorSpace colorSpace = EffectColorSpace::SRGB;
  /** true selects the BT.709 YUV matrix; false uses BT.601. */
  bool bt709 = false;
  /** true means full-range YUV (0..255) instead of limited-range (16..235 / 16..240). */
  bool fullRange = false;
  /** true means the planes are 10-bit P010 (16-bit words, 10 significant bits). */
  bool p010 = false;
  /** true marks BT.2020 HDR content; the GPU path tone maps it when the target is SDR. */
  bool hdr = false;
  /** HDR transfer used when `hdr` is true. */
  EffectHdrTransfer hdrTransfer = EffectHdrTransfer::PQ;
  /** HDR10+ per-frame targeted display luminance (nits); zero means none. */
  float hdr10PlusTargetedNits = 0.0f;
  /** HDR10+ per-frame average_maxrgb (0..1). */
  float hdr10PlusAverageMaxRgb = 0.0f;
  /** HDR10+ per-frame fraction_bright_pixels (0..1). */
  float hdr10PlusFractionBrightPixels = 0.0f;
  /** HDR static MaxCLL (nits, 0 when unknown). */
  uint32_t hdrMaxContentLightLevel = 0;
  /** HDR static MaxFALL (nits, 0 when unknown). */
  uint32_t hdrMaxFrameAverageLightLevel = 0;
  EffectFrameSource source = EffectFrameSource::Planes;
  /** Keeps an AHardwareBuffer alive while the frame is queued. */
  std::shared_ptr<void> hardwareBuffer;
  bool bt2020 = false;
  bool chromaCositedEven = true;
  VideoFramePlane planes[3];
  uint32_t planeCount = 0;
  /** Source presentation time; zero is a valid time and is still carried to the output. */
  RenderTime timestamp;
};

/**
 * A core input fed by decoded CPU frames.
 *
 * The node owns one RGBA8 staging image and uploads it on the render thread; it never exposes the pixel
 * buffer or a backend texture. `submit` may be called from a decoder thread: pixels are copied under the
 * node's mutex into a bounded queue, and the render thread consumes one frame per render. In
 * `DropOldest` the oldest queued frame is discarded when the queue is full; in `Block` `submit` returns
 * `Busy` instead of dropping, leaving retry/backpressure to the producer. When the queue is empty,
 * `repeatLastFrame()` decides whether the previous picture is shown again.
 */
class VideoFrameInputNode final : public EffectInputNode {
public:
  /**
   * @param queueDepth maximum queued frames; 0 is treated as 1. The default matches a small preview buffer.
   */
  static std::shared_ptr<VideoFrameInputNode> create(uint32_t width, uint32_t height, uint32_t queueDepth = 4);

  ~VideoFrameInputNode() override;

  /**
   * Copies/converts @p frame and queues it.
   *
   * @return `InvalidArgument` for a null plane, a zero size, an unsupported format or a short row;
   *         `Busy` when the queue is full in `Block` mode.
   */
  EffectResult submit(const VideoFrameDesc& frame);

  /** The unified effect input entry point; consumes CPU planes from @p view. */
  EffectResult submit(const PixelBufferView& view) override;
  /** GPU-only path for a decoder YUV AHardwareBuffer. */
  EffectResult submitHardwareBuffer(std::shared_ptr<void> buffer, uint32_t width, uint32_t height,
                                    bool bt2020, bool fullRange, bool p010, bool hdr, bool hlg,
                                    RenderTime timestamp);

  bool hasFrame() const noexcept override;
  bool repeatLastFrame() const noexcept override;
  void setRepeatLastFrame(bool repeat) override;

  /** Selects the GPU YUV420->RGBA pass instead of the CPU conversion. Default: CPU. */
  void setGpuConversion(bool enabled);

  /** GPU output format: RGBA8 (default) or RGBA16F for linear HDR data. */
  void setGpuOutputFormat(EffectPixelFormat format);

  /** SDR tone-map controls used when an HDR input targets RGBA8. */
  void setHdrToneMap(EffectToneMapMode mode, float exposure, float paperWhiteNits);

  [[nodiscard]] uint32_t width() const noexcept;
  [[nodiscard]] uint32_t height() const noexcept;

  /** Implementation detail; opaque to a client. */
  struct State;

private:
  VideoFrameInputNode(uint32_t width, uint32_t height, uint32_t queueDepth);

  std::shared_ptr<State> mState;
};

} // namespace coral

#endif // CORAL_VIDEO_FRAME_INPUT_NODE_H
