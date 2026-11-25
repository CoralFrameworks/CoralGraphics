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

#ifndef CORAL_ANDROID_HARDWARE_BUFFER_OUTPUT_NODE_H
#define CORAL_ANDROID_HARDWARE_BUFFER_OUTPUT_NODE_H

#include <effect/EffectOutputNode.h>

#include <android/hardware_buffer.h>
#include <android/native_window.h>

#include <cstdint>
#include <memory>

namespace coral {

/**
 * Renders the graph's final frame into an `AHardwareBuffer` and presents it to an encoder surface.
 *
 * The Vulkan backend imports the buffer as a writeback texture; `consume` copies the last pass into
 * it. Once the frame's Vulkan submission has completed, an EGL/GLES context imports the same buffer
 * as an `EGLImage` and blits it into the `ANativeWindow` of a MediaCodec input Surface. The frame
 * therefore stays on the GPU end to end; the GL context is only the platform bridge between Vulkan
 * and the encoder's BufferQueue.
 *
 * The encoder surface is supplied by the caller (the Android export gets it from the encoder's
 * `SurfaceHolder`). The node takes its own `ANativeWindow` reference.
 */
class AndroidHardwareBufferOutputNode final : public EffectOutputNode {
public:
  static std::shared_ptr<AndroidHardwareBufferOutputNode> create(
    ANativeWindow* encoderWindow, uint32_t width, uint32_t height);

  ~AndroidHardwareBufferOutputNode() override;

  bool isReady() const noexcept override;
  EffectResult configure(const EffectOutputConfig& config) override;

  /** The buffer Vulkan writes and the encoder reads. Owned by the node. */
  [[nodiscard]] AHardwareBuffer* hardwareBuffer() const noexcept;

  /** Stamps the encoder frame with @p timeUs on the next present. */
  void setPresentationTimeUs(int64_t timeUs) noexcept;

  [[nodiscard]] uint32_t width() const noexcept;
  [[nodiscard]] uint32_t height() const noexcept;

  /** True once the Vulkan import and the EGL bridge are ready. */
  [[nodiscard]] bool isZeroCopy() const noexcept;

  /** True after the EGL present bridge could not hand a frame to the encoder. */
  [[nodiscard]] bool hasPresentFailure() const noexcept;

  struct State;

private:
  AndroidHardwareBufferOutputNode(ANativeWindow* encoderWindow, uint32_t width, uint32_t height);

  std::shared_ptr<State> mState;
};

} // namespace coral

#endif // CORAL_ANDROID_HARDWARE_BUFFER_OUTPUT_NODE_H
