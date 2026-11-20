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

#ifndef CORAL_READBACK_OUTPUT_NODE_H
#define CORAL_READBACK_OUTPUT_NODE_H

#include <effect/EffectFrame.h>
#include <effect/EffectOutputNode.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace coral {

/**
 * An output node that copies the final frame back to the CPU.
 *
 * The callback runs on the render thread once the frame's copy has completed, which the design allows
 * explicitly ("a short callback on the render thread") and which keeps the pixels in the frame's own
 * order without a second queue. A callback must therefore not block, and must not call back into the
 * engine.
 *
 * `waitForFrame` is the test and export helper: it waits for the next completed frame and returns a copy,
 * so a caller can drive the engine manually and then inspect the picture.
 */
class ReadbackOutputNode final : public EffectOutputNode {
public:
  /**
   * @param frame    metadata of the frame the pixels belong to.
   * @param pixels   tightly packed RGBA8 pixels, valid only for the duration of the call.
   * @param byteSize `width * height * 4`.
   */
  using Callback = std::function<void(const EffectFrame& frame, const uint8_t* pixels, size_t byteSize)>;

  static std::shared_ptr<ReadbackOutputNode> create();

  ~ReadbackOutputNode() override;

  bool isReady() const noexcept override;
  EffectResult configure(const EffectOutputConfig& config) override;

  void setCallback(Callback callback);

  /**
   * Waits for the next frame after this call (or the current one if it has already completed) and copies
   * its pixels into @p pixels.
   *
   * @return false on timeout, or when the engine produced no frame.
   */
  bool waitForFrame(EffectFrame* frame, std::vector<uint8_t>* pixels, uint64_t timeoutMilliseconds = 5000);

  /** The most recently delivered frame, or an invalid one. */
  [[nodiscard]] EffectFrame lastFrame() const;

  /** Implementation detail; opaque to a client. */
  struct State;

private:
  ReadbackOutputNode();

  std::shared_ptr<State> mState;
};

} // namespace coral

#endif // CORAL_READBACK_OUTPUT_NODE_H
