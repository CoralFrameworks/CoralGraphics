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

#ifndef CORAL_ANDROID_SURFACE_OUTPUT_NODE_H
#define CORAL_ANDROID_SURFACE_OUTPUT_NODE_H

#include <effect/EffectOutputNode.h>

#include <android/native_window.h>
#include <jni.h>

#include <cstdint>
#include <memory>

namespace coral {

/**
 * Presents the graph's final pooled frame to an `ANativeWindow` (a `Surface`).
 *
 * The node owns a reference to the window, acquired at creation and released at destruction. The engine
 * creates its device for that window, so the platform's EGL surface is part of device creation; the graph
 * therefore has to call `setOutput` **before** `start()` for this node to present.
 *
 * Presentation is the design's "final pooled frame + blit" path: the last pass writes a normal RGBA8
 * frame, and this node records one copy pass into the swapchain image. `present == false` consumes the
 * frame without acquiring a swapchain image, which is what an encoder input or an offline run wants.
 */
class AndroidSurfaceOutputNode final : public EffectOutputNode {
public:
  /** @param window the target surface; the node takes its own reference. Null is refused. */
  static std::shared_ptr<AndroidSurfaceOutputNode> create(ANativeWindow* window);

  /**
   * Creates an output for a Java `android.view.Surface`.
   *
   * This is the encoder path: pass a `MediaCodec` input Surface and the node presents the final pooled
   * frame into the codec's surface. The node takes its own `ANativeWindow` reference; the Java Surface is
   * only borrowed for the call.
   */
  static std::shared_ptr<AndroidSurfaceOutputNode> create(JNIEnv* env, jobject surface);

  ~AndroidSurfaceOutputNode() override;

  bool isReady() const noexcept override;
  EffectResult configure(const EffectOutputConfig& config) override;

  /** The window this node presents to. Borrowed for the duration of the call. */
  [[nodiscard]] ANativeWindow* window() const noexcept;

  /**
   * Stamps the next presented frame with @p timeUs for the consumer of the window.
   *
   * @discussion
   * A `MediaCodec` input surface times the frame it receives by the EGL presentation time of the
   * producing surface; without it every encoded frame carries the same time. An export that
   * presents into an encoder surface sets the time of each composition frame here, right before it
   * executes the engine. A negative value clears the stamp; a window output ignores it.
   */
  void setPresentationTimeUs(int64_t timeUs) noexcept;

  /** Implementation detail; opaque to a client. */
  struct State;

private:
  explicit AndroidSurfaceOutputNode(ANativeWindow* window);

  std::shared_ptr<State> mState;
};

} // namespace coral

#endif // CORAL_ANDROID_SURFACE_OUTPUT_NODE_H
