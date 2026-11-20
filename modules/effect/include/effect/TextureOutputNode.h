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

#ifndef CORAL_TEXTURE_OUTPUT_NODE_H
#define CORAL_TEXTURE_OUTPUT_NODE_H

#include <effect/EffectFrame.h>
#include <effect/EffectOutputNode.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace coral {

/**
 * An output node that keeps the final frame on the GPU as a texture.
 *
 * <p>This is the offscreen counterpart of the platform swapchain outputs: instead of presenting, it
 * copies each committed frame into one of a small ring of textures and reports it to the caller. A
 * user interface that shares the effect engine's device samples that texture directly, with no
 * readback.
 *
 * <p>The frame handles are exposed as opaque `uint64_t` tokens (the value of the RHI texture handle
 * and of the RHI texture-view handle) so that this public header stays free of RHI types. A
 * consumer that lives in the RHI layer reconstructs the handles from the tokens.
 *
 * <p>## Lifetime and threads
 *
 * <p>A reported frame stays valid until the slot is reused, which is after `bufferCount` further
 * committed frames. The consumer that records commands naming the frame's view must have submitted
 * them before then; the UI path does this on the same GPU thread that receives the callback.
 *
 * <p>The frame callback runs on the engine's render thread. It must not block, allocate GPU
 * resources, or call back into the engine. `latestFrame()` is thread-safe and is the alternative for
 * a consumer that polls from its own thread.
 *
 * <p>## Scope
 *
 * <p>The current implementation copies the frame at the graph's size and format; it does not scale or
 * rotate. A configured width or height that differs from the graph's frame is rejected rather than
 * silently ignored, because a caller that asked for a size and got another one would size its
 * sampling incorrectly.
 */
class TextureOutputNode final : public EffectOutputNode {
public:
  /** One frame the node has committed, as opaque backend handle tokens. */
  struct Frame {
    /** The value of the RHI texture handle, or 0 when invalid. */
    uint64_t texture = 0;
    /** The value of the RHI texture-view handle, or 0 when invalid. */
    uint64_t view = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    /** Monotonically increasing per committed frame. */
    uint64_t sequence = 0;
    /** Timestamp, format and colour space of the frame. */
    EffectFrame metadata;

    /**
     * Keeps the ring slot alive while a consumer still holds this frame.
     *
     * A render host copies this token into its `ContentFrame`/frame packet and drops it after the
     * commands that sample the view have been submitted. The producer will not reuse a slot whose
     * token is still externally referenced; if every slot is held, a preview frame is dropped rather
     * than overwriting a texture a consumer is about to sample.
     */
    std::shared_ptr<void> keepAlive;

    [[nodiscard]] bool isValid() const noexcept { return view != 0; }
  };

  struct Config {
    /**
     * How many frames may be in flight before the oldest slot is reused.
     *
     * Two is enough for a preview; three lets a 60 Hz consumer read a 30 Hz producer without waiting.
     */
    uint32_t bufferCount = 3;
  };

  using FrameCallback = std::function<void(const Frame&)>;

  static std::shared_ptr<TextureOutputNode> create();
  static std::shared_ptr<TextureOutputNode> create(const Config& config);
  ~TextureOutputNode() override;

  TextureOutputNode(const TextureOutputNode&) = delete;
  TextureOutputNode& operator=(const TextureOutputNode&) = delete;

  bool isReady() const noexcept override;

  /**
   * Applies the output configuration.
   *
   * `width`/`height` are advisory: the node rejects a frame of a different size until the graph
   * renders at that size. `format` selects the texture format and must be RGBA8, BGRA8 or RGBA16F.
   */
  EffectResult configure(const EffectOutputConfig& config) override;

  /** Sets the callback invoked on the render thread after each committed frame. */
  void setFrameCallback(FrameCallback callback);

  /**
   * Whether committed frames are copied into the ring.
   *
   * An inactive node consumes a frame without copying it or invoking the callback: the last frame
   * stays available, and the graph keeps running (the engine has no per-output pause yet). A preview
   * that is hidden or scrolled away uses this to stop paying the copy and the consumer's wait while
   * still resuming instantly when it becomes visible again.
   */
  void setActive(bool active);
  [[nodiscard]] bool active() const;
  /** The configured ring capacity; a consumer should not reserve more frames than this. */
  [[nodiscard]] uint32_t bufferCount() const;

  /** @return the most recently committed frame, or an invalid one. Safe from any thread. */
  [[nodiscard]] Frame latestFrame() const;

  /**
   * Blocks until the most recently committed frame's GPU work has finished.
   *
   * A consumer that samples `latestFrame()` on another thread -- the usual case, because model A renders
   * the page in the engine's own callbacks -- needs this before recording, unless the engine already
   * waits for each submission. It turns the one-frame latency into a guaranteed-visible frame at the
   * cost of a stall, which is the right trade for a prototype and the wrong one for a shipping
   * compositor: production moves the wait onto a semaphore the consumer's submission waits on.
   */
  void waitForLatestFrame();

  /** Implementation detail; opaque to a client. */
  struct State;

private:
  explicit TextureOutputNode(const Config& config);

  std::shared_ptr<State> mState;
};

} // namespace coral

#endif // CORAL_TEXTURE_OUTPUT_NODE_H
