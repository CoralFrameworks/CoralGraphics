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

#ifndef CORAL_EFFECT_OUTPUT_NODE_H
#define CORAL_EFFECT_OUTPUT_NODE_H

#include <effect/EffectNode.h>

#include <cstdint>
#include <memory>

namespace coral {

namespace detail {
class OutputRuntime;
} // namespace detail

/** How a final frame is mapped onto an output of a different size. */
enum class EffectScaleMode : uint8_t {
  /** Fill the output, ignoring the source aspect ratio. */
  Stretch,
  /** Fit inside the output, letterboxing the remainder. */
  Fit,
  /** Fill the output, cropping the source. */
  Crop,
};

/** Clockwise rotation applied to the final image. */
enum class EffectRotation : uint8_t {
  None,
  Clockwise90,
  Clockwise180,
  Clockwise270,
};

/**
 * How presentation waits for the display.
 *
 * Presentation is the one part of a frame the application does not control the cost of: `Fifo` blocks in
 * the swapchain until the display is ready, so a 30 fps preview pays ~33 ms per frame half of which is
 * spent inside `execute()`. `Immediate` returns as soon as the image is queued, which is what a benchmark
 * or an offscreen composition wants; `Mailbox` keeps a single queued image and is the usual choice for a
 * latency-sensitive interactive path.
 *
 * The mode is a request: a backend that cannot grant it falls back (Mailbox is commonly approximated by
 * Immediate), and every swapchain reports what it actually got.
 */
enum class EffectPresentMode : uint8_t {
  Fifo,
  Immediate,
  Mailbox,
};

/**
 * Whether `execute()` waits for the frame to reach the display.
 *
 * Presenting is the one stage the application cannot make cheaper by rendering less: a `Fifo` swapchain
 * blocks inside `execute()` until the display is ready, which on a 30 fps preview is a third of the frame
 * budget spent waiting. `Deferred` lets the engine queue the present and return, so the next frame's work
 * overlaps the wait. It is a backend capability, not a promise - `EffectEngine::supportsDeferredPresent()`
 * says whether the running backend can actually defer, and one that cannot runs `Deferred` as `Blocking`
 * rather than failing.
 */
enum class EffectPresentPolicy : uint8_t {
  /** `execute()` returns once the frame has been handed to the display system. */
  Blocking,
  /** `execute()` does not wait; a busy target is handled according to `presentOverflow`. */
  Deferred,
};

/**
 * What a `Deferred` output does when the display is not ready for another frame yet.
 */
enum class EffectPresentOverflow : uint8_t {
  /** Drop the queued frame and keep the newest: a late frame has no value for a live preview. */
  DropOldest,
  /**
   * Never drop: `execute()` waits until the target can take the frame.
   *
   * This is the mode for offline export and recording, where a missing frame is a defect. It still
   * pipelines one frame deep, so rendering and presenting overlap.
   */
  Wait,
};

/**
 * What an output node should produce.
 *
 * A zero width or height means "follow the engine's render size". The defaults are what a preview wants:
 * the engine's size, RGBA8/sRGB, stretched, and presented.
 */
struct EffectOutputConfig {
  uint32_t width = 0;
  uint32_t height = 0;
  EffectPixelFormat format = EffectPixelFormat::RGBA8;
  EffectColorSpace colorSpace = EffectColorSpace::SRGB;
  EffectScaleMode scaleMode = EffectScaleMode::Stretch;
  /** Colour written outside the content rect in Fit mode; fully transparent by default. */
  EffectColor letterboxColor { 0.0f, 0.0f, 0.0f, 0.0f };
  EffectRotation rotation = EffectRotation::None;
  bool mirrorHorizontal = false;
  bool mirrorVertical = false;
  /** false = consume the frame without presenting it, which is what an encoder input may want. */
  bool present = true;
  /** How presentation waits for the display. Applied when the swapchain is (re)created. */
  EffectPresentMode presentMode = EffectPresentMode::Fifo;
  /** Whether `execute()` waits for the present to complete. Ignored by outputs with no swapchain. */
  EffectPresentPolicy presentPolicy = EffectPresentPolicy::Blocking;
  /** What a `Deferred` output does when the target is busy. */
  EffectPresentOverflow presentOverflow = EffectPresentOverflow::DropOldest;
  /**
   * Minimum time between presents, in milliseconds; 0 presents as fast as the target accepts.
   *
   * A `Deferred` output presents at most this often and skips the frames in between, which is frame pacing:
   * the renderer runs at its own rate while the display still gets one image per interval. Without it a
   * deferred output still pays the compositor's wait on every frame, because the swapchain has an image free
   * even when the display has not consumed the last one.
   */
  uint32_t presentIntervalMilliseconds = 0;
};

/**
 * The base class of nodes that consume the graph's final frame.
 *
 * As with `EffectInputNode`, presenting or reading back is a platform concern: a concrete node registers
 * an internal `detail::OutputRuntime`, and the public class stays free of window-system and RHI types.
 */
class EffectOutputNode : public EffectNode {
public:
  ~EffectOutputNode() override;

  /** @return true when the output can currently accept a frame (surface alive, buffer available). */
  [[nodiscard]] virtual bool isReady() const noexcept = 0;

  /**
   * Releases backend resources (textures/views/submissions) while the device is still alive.
   *
   * The engine calls this before it tears its device down when the node is still attached; a caller
   * that detaches an output early (for example an editor session shutting down) must call it itself
   * before the engine/device goes away. Safe to call more than once.
   */
  void release();

  /** Applies a new output configuration. Takes effect on the next frame. */
  virtual EffectResult configure(const EffectOutputConfig& config) = 0;

  /** true when this output can render the last pass directly into its presentable target. */
  [[nodiscard]] virtual bool supportsDirectSurface() const noexcept { return false; }

protected:
  EffectOutputNode();

  /** Installs the runtime that presents or reads back frames. See `EffectInputNode::setInputRuntime`. */
  void setOutputRuntime(std::shared_ptr<detail::OutputRuntime> runtime);

private:
  friend struct detail::NodeAccess;
};

} // namespace coral

#endif // CORAL_EFFECT_OUTPUT_NODE_H
