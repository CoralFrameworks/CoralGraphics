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

#ifndef CORAL_TEST_PATTERN_INPUT_NODE_H
#define CORAL_TEST_PATTERN_INPUT_NODE_H

#include <effect/EffectInputNode.h>

#include <cstdint>
#include <memory>

namespace coral {

/**
 * An input node that generates a deterministic image.
 *
 * Its purpose is the one thing a camera, a decoder and a window all make hard: a test that needs a known
 * picture, no platform, and no timing. The pattern is generated on the CPU once per change and uploaded;
 * every frame afterwards repeats it, which is what makes a capture comparison meaningful.
 */
class TestPatternInputNode final : public EffectInputNode {
public:
  enum class Pattern : uint8_t {
    /** Eight vertical SMPTE-style bars: white, yellow, cyan, green, magenta, red, blue, black. */
    ColorBars,
    /** A checkerboard whose squares are one eighth of the shorter side. */
    Checkerboard,
  };

  static std::shared_ptr<TestPatternInputNode> create(uint32_t width, uint32_t height,
                                                      Pattern pattern = Pattern::ColorBars);

  ~TestPatternInputNode() override;

  bool hasFrame() const noexcept override;
  bool repeatLastFrame() const noexcept override;
  void setRepeatLastFrame(bool repeat) override;

  [[nodiscard]] Pattern pattern() const noexcept;
  void setPattern(Pattern pattern);

  [[nodiscard]] uint32_t width() const noexcept;
  [[nodiscard]] uint32_t height() const noexcept;

  /** Forces the next produced frame to re-upload the pattern. */
  void invalidate();

  /** Implementation detail; opaque to a client. */
  struct State;

private:
  TestPatternInputNode(uint32_t width, uint32_t height, Pattern pattern);

  std::shared_ptr<State> mState;
};

} // namespace coral

#endif // CORAL_TEST_PATTERN_INPUT_NODE_H
