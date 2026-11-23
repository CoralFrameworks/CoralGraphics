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

#ifndef CORAL_GPU_METAL_DISPLAY_LINK_H
#define CORAL_GPU_METAL_DISPLAY_LINK_H

#include <gpu/FrameClock.h>

#include <cstdint>
#include <functional>
#include <memory>

#if defined(__APPLE__)

namespace graphics::gpu {

/**
 * One `CAMetalDisplayLink` update: the drawable to render and the time it is meant for.
 *
 * The timestamps are Core Animation's seconds on the host clock, not `steady_clock`; a caller that
 * needs a `steady_clock` deadline converts once, at the boundary, because the two clocks have
 * different epochs. `targetPresentationTimestamp` is the instant the frame will be displayed, which is
 * the value animation should be sampled at.
 */
struct MetalDisplayLinkUpdate {
  /** The `id<CAMetalDrawable>` to render into. Borrowed for the callback; present it before returning. */
  void* drawable = nullptr;
  /** The display link's time for this update. */
  double timestamp = 0.0;
  /** The time the render is meant to finish by. */
  double targetTimestamp = 0.0;
  /** The time the frame is expected to be shown. */
  double targetPresentationTimestamp = 0.0;
};

/**
 * A `CAMetalDisplayLink` on a dedicated run-loop thread: the Metal presentation tick.
 *
 * ## Why this is not a `VBlankSource`
 *
 * `VBlankSource` only reports that a blank happened. `CAMetalDisplayLink` also owns a drawable for
 * each update, and that ownership is the useful part: the delegate renders into the drawable the
 * display is about to show, then presents it, rather than acquiring a second one with `nextDrawable`
 * and hoping the timing lands. Using it as a plain tick source would acquire drawables nobody draws
 * into and exhaust the layer's pool.
 *
 * ## The thread model
 *
 * `start` creates a thread, a run loop and the link on it, and every callback arrives on that thread.
 * The callback is where a renderer lives: create the `WindowSurface` there, call
 * `beginFrame(update.drawable)`, record, `present()`. The UI thread records `DrawList`s and hands them
 * over with `WindowSurface::presentRecording`, which is already the split the window surface documents.
 * Do not call `stop` from inside the callback: it joins the callback's own thread. A display link that
 * wants to end itself should arrange for `stop` on the owner's thread.
 *
 * ## Availability
 *
 * `CAMetalDisplayLink` and `CADisplayLink` need macOS 14 / iOS 17. `make` returns null on an older OS,
 * and the caller falls back to `beginFrame()` from its own run loop.
 */
class MetalDisplayLink final {
public:
  using Callback = std::function<void(const MetalDisplayLinkUpdate&)>;

  /**
   * @return a link for @p metalLayer (a `CAMetalLayer*`), or null when the platform or object cannot
   *         provide one.
   */
  [[nodiscard]] static std::unique_ptr<MetalDisplayLink> make(void* metalLayer);

  ~MetalDisplayLink();

  MetalDisplayLink(const MetalDisplayLink&) = delete;
  MetalDisplayLink& operator=(const MetalDisplayLink&) = delete;

  /**
   * Starts the run-loop thread and delivers @p callback on it.
   *
   * @return false when the thread or the link could not start; `false` is also returned by a second
   *         `start` on a running link.
   */
  bool start(Callback callback);

  /** Invalidates the link, stops the run loop and joins the thread. Safe to call twice. */
  void stop();

  [[nodiscard]] bool isRunning() const noexcept;

  /**
   * Sets the frame-rate range the link should prefer.
   *
   * Applies at `start`; calling it on a running link stores the values for the next start rather than
   * mutating a property from a different thread.
   */
  void setPreferredFrameRate(float minimum, float maximum, float preferred) noexcept;

  /**
   * How many display intervals of latency the link may use.
   *
   * A value of 2 is the usual choice for a renderer that records, submits and then presents: one
   * interval of headroom for the CPU and one for the GPU. The default is 2.
   */
  void setPreferredFrameLatency(float frames) noexcept;

  /** The last interval the link reported, in hertz. Invalid until two updates have arrived. */
  [[nodiscard]] RefreshInfo refresh() const noexcept;

private:
  MetalDisplayLink();

  struct Impl;
  std::unique_ptr<Impl> mImpl;

  /**
   * The bridge the Objective-C delegate calls.
   *
   * A static function rather than a direct delegate/`Impl` friendship because the delegate is an
   * Objective-C object and cannot be a C++ friend. `context` is an `Impl*`, and `nativeUpdate` an
   * opaque `CAMetalDisplayLinkUpdate*`; the delegate never has to know the private type.
   */
  static void deliverUpdate(void* context, void* nativeUpdate);
};

} // namespace graphics::gpu

#endif // __APPLE__

#endif // CORAL_GPU_METAL_DISPLAY_LINK_H
