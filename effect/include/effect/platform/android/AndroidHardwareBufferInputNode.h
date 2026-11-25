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

#ifndef CORAL_ANDROID_HARDWARE_BUFFER_INPUT_NODE_H
#define CORAL_ANDROID_HARDWARE_BUFFER_INPUT_NODE_H

#include <effect/EffectInputNode.h>
#include <effect/EffectTypes.h>

#include <android/hardware_buffer.h>

#include <cstdint>
#include <memory>

namespace coral {

/**
 * Colour description for a YUV `AHardwareBuffer`.
 *
 * The buffer does not name its coefficients or range; the producer has to. The defaults are BT.709
 * limited-range SDR, which is what an 8-bit camera/decoder buffer usually is.
 */
struct AndroidHardwareBufferYuvDesc {
  bool bt2020 = false;
  bool fullRange = false;
  bool hdr = false;
  bool hlg = false;
};

/**
 * An input node that samples an `AHardwareBuffer` without a CPU copy.
 *
 * ## Backend support
 *
 * The Vulkan backend imports RGBA8/BGRA8/RGBA16F with
 * `VK_ANDROID_external_memory_android_hardware_buffer`. A YUV buffer additionally needs a sampler YCbCr
 * conversion; ask `EffectEngine::supportsYuvHardwareBuffer()` before choosing that path. On a backend
 * without the import, `produce()` reports `Unsupported` and the caller should use
 * `VideoFrameInputNode`'s CPU plane path instead.
 *
 * ## Ownership
 *
 * The node borrows the `AHardwareBuffer*`; the caller must keep it alive until the node is destroyed or a
 * new buffer is submitted, and must not reuse/close it while the engine may still be sampling it. The
 * RHI imports the handle and does not take an extra `AHardwareBuffer` reference.
 */
class AndroidHardwareBufferInputNode final : public EffectInputNode {
public:
  static std::shared_ptr<AndroidHardwareBufferInputNode> create(AHardwareBuffer* buffer, uint32_t width,
                                                                uint32_t height,
                                                                EffectPixelFormat format = EffectPixelFormat::RGBA8);

  /** Creates the node before the producer has a buffer; call `submit`/`setBuffer` later. */
  static std::shared_ptr<AndroidHardwareBufferInputNode> create(uint32_t width, uint32_t height,
                                                                EffectPixelFormat format = EffectPixelFormat::YUV420);

  ~AndroidHardwareBufferInputNode() override;

  bool hasFrame() const noexcept override;
  bool repeatLastFrame() const noexcept override;
  void setRepeatLastFrame(bool repeat) override;

  [[nodiscard]] uint32_t width() const noexcept;
  [[nodiscard]] uint32_t height() const noexcept;

  /** Replaces the buffer without a timestamp; the next frame uses the engine's frame timestamp. */
  EffectResult setBuffer(AHardwareBuffer* buffer, uint32_t width, uint32_t height, EffectPixelFormat format);

  /** Replaces the buffer and carries the producer's PTS atomically with it. */
  EffectResult submit(AHardwareBuffer* buffer, uint32_t width, uint32_t height, EffectPixelFormat format,
                      RenderTime timestamp);

  /** The unified effect input entry point for an AHardwareBuffer view. */
  EffectResult submit(const PixelBufferView& view) override;

  void setTimestamp(RenderTime timestamp);

  /**
   * Sets the colour metadata used when the submitted format is `YUV420`.
   *
   * Ignored for RGBA8/BGRA8/RGBA16F. The defaults are BT.709 limited-range SDR.
   */
  void setYuvDescription(const AndroidHardwareBufferYuvDesc& description);

  /** Output of the YUV conversion pass: RGBA8 (default) or RGBA16F for linear HDR. */
  void setYuvOutputFormat(EffectPixelFormat format);

  /** Tone map used when a YUV HDR buffer is converted to RGBA8. */
  void setYuvToneMap(EffectToneMapMode mode, float exposure, float paperWhiteNits);

  /** Implementation detail; opaque to a client. */
  struct State;

private:
  AndroidHardwareBufferInputNode(AHardwareBuffer* buffer, uint32_t width, uint32_t height,
                                 EffectPixelFormat format);
  std::shared_ptr<State> mState;
};

} // namespace coral

#endif // CORAL_ANDROID_HARDWARE_BUFFER_INPUT_NODE_H
