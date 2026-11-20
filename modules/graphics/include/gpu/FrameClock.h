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

#ifndef CORAL_GRAPHICS_GPU_FRAMECLOCK_H
#define CORAL_GRAPHICS_GPU_FRAMECLOCK_H

#include <chrono>
#include <cstdint>
#include <memory>

namespace graphics::gpu {

/** Why a frame is being drawn. The mode decides what a clock does with the requests it receives. */
enum class FrameMode : uint8_t {
  /** Draw only when somebody asked; an idle page costs nothing. */
  OnDemand,
  /** Keep drawing; a media preview uses this so playback keeps moving between input events. */
  Continuous,
  /**
   * Keep drawing, and let input-driven frames overtake animation ones: the camera/playhead follows the
   * pointer with the shortest deadline, while an animation that is already late is skipped.
   */
  LowLatency,
};

/** What the clock has learned about the display it is pacing. */
struct RefreshInfo {
  /** False until at least two presents have been observed. */
  bool valid = false;
  /** Measured refresh rate in hertz. */
  double hertz = 0.0;
  /** Where the last observed present landed within its refresh interval, in seconds. */
  double phaseSeconds = 0.0;
  /** True when the display is treated as variable-rate (not measured here; reserved for a platform clock). */
  bool variable = false;
};

/**
 * A frame clock: the source of "when may the next UI frame be drawn?".
 *
 * ## Phase 1: present is the tick
 *
 * The first implementation does not own a timer. `WindowSurface` in `Fifo`/`Mailbox` mode blocks in
 * `present()` until the display can take another frame, so the render loop is already paced by the
 * display; the clock's job is to coalesce the requests that arrive inside that interval and to report
 * what it observed (`RefreshInfo`, `expectedPresentTime`). A caller therefore does:
 *
 * ```cpp
 * clock.requestFrame(deadline);
 * if (clock.waitForFrame()) {
 *   draw();
 *   present();                       // the actual tick
 *   clock.onFramePresented(now);
 * }
 * ```
 *
 * `waitForFrame` returning immediately is deliberate in phase 1: sleeping here *and* blocking in
 * `present()` would halve the frame rate. The explicit deadline/sleep scheduler, the latency budget
 * and the platform vblank sources arrive with `FrameClock` phase 2 (M1/M4); the interface and its
 * tests stay the same.
 *
 * ## What the requests mean
 *
 * Repeated requests before a present coalesce into one frame, and the earliest deadline wins. A frame
 * that is drawn consumes the request: `onFramePresented` clears it, so an `OnDemand` page that has
 * nothing new does not draw again until somebody asks.
 *
 * Thread-affine: one clock per window, owned and called by the thread that presents the window.
 */
class FrameClock {
 public:
  virtual ~FrameClock() = default;

  virtual void setMode(FrameMode mode) = 0;
  [[nodiscard]] virtual FrameMode mode() const noexcept = 0;

  /** Requests one frame, no later than @p deadline. The earliest pending deadline is kept. */
  virtual void requestFrame(std::chrono::steady_clock::time_point deadline) = 0;

  [[nodiscard]] virtual bool frameRequested() const noexcept = 0;
  [[nodiscard]] virtual std::chrono::steady_clock::time_point deadline() const noexcept = 0;

  /**
   * Marks the start of the CPU/GPU work for the frame about to be drawn.
   *
   * `waitForFrame` uses the durations this produces to start the next frame early enough to finish by
   * its deadline. Call it immediately before recording, not before `waitForFrame`.
   */
  virtual void onFrameStarted() = 0;

  /**
   * Blocks until the frame should start.
   *
   * M1: sleeps until `deadline - estimated frame cost`; the frame then has the display interval up to
   * its deadline to render and queue. Returns false when no frame is requested.
   */
  virtual bool waitForFrame() = 0;

  /** Records that a frame reached the display, and consumes the pending request. */
  virtual void onFramePresented(std::chrono::steady_clock::time_point presented) = 0;

  [[nodiscard]] virtual RefreshInfo refresh() const noexcept = 0;

  /**
   * The time the next frame will be shown, which is what animation should be sampled at.
   *
   * Sampling at the wall clock instead makes an animation's speed follow the frame rate: a 30 fps
   * frame advances twice as far as a 60 fps one. This is the presentation time the current frame is
   * being prepared for.
   */
  [[nodiscard]] virtual std::chrono::steady_clock::time_point expectedPresentTime() const noexcept = 0;

  /** Estimated cost of one frame, in seconds: the value `waitForFrame` subtracts from a deadline. */
  [[nodiscard]] virtual double frameCostSeconds() const noexcept = 0;

  /** The instant `waitForFrame` would wake for the current deadline. Telemetry and tests. */
  [[nodiscard]] virtual std::chrono::steady_clock::time_point wakeTarget() const noexcept = 0;
};

/** What the native handles in `NativeDisplay` belong to. */
enum class NativeDisplayKind : uint8_t {
  Unknown,
  Windows,
  X11,
  Wayland,
};

/**
 * The window whose vblank a per-window clock should follow.
 *
 * `display` is a `wl_display*` on Wayland or an X11 `Display*`; `surface` is a `wl_surface*` or
 * an X11 `Window` (the integer handle, carried as a pointer). Windows ignores both: DXGI's output
 * vblank is not per-window and needs no HWND.
 */
struct NativeDisplay {
  NativeDisplayKind kind = NativeDisplayKind::Unknown;
  void* display = nullptr;
  void* surface = nullptr;
};

/**
 * A display's vertical-blank source.
 *
 * Phase 2: the platform supplies the tick, rather than the swapchain's blocking present. The clock
 * uses it to learn the display interval and phase; the scheduler still starts a frame at its
 * deadline minus the measured frame cost, so rendering can run just before the blank instead of being
 * released by it.
 */
class VBlankSource {
 public:
  virtual ~VBlankSource() = default;

  /** Blocks until the next vertical blank. @return false when the display is gone or cannot wait. */
  virtual bool waitForVBlank() = 0;

  [[nodiscard]] virtual RefreshInfo refresh() const = 0;

  /**
   * Tells an event-driven source that a frame reached the surface.
   *
   * Wayland's `wl_surface.frame` callback only fires after a commit, so the source cannot arm its
   * next wait until the previous frame has been presented -- which is exactly this call. It is a
   * no-op for polled sources (DXGI, DRM), whose wait is armed by the kernel.
   */
  virtual void notePresented() {}

  /** True when waits are armed by `notePresented` (Wayland/X11 events), false for polled vblank. */
  [[nodiscard]] virtual bool isEventDriven() const noexcept {
    return false;
  }
};

/** @return the platform's vblank source, or null when this build/platform has none. */
[[nodiscard]] std::unique_ptr<VBlankSource> makePlatformVBlankSource();

/** As above, for a specific window; per-window sources use @p native, others ignore it. */
[[nodiscard]] std::unique_ptr<VBlankSource> makePlatformVBlankSource(const NativeDisplay& native,
  bool waitForDisplay = true);

/**
 * A deterministic clock for tests and offline rendering.
 *
 * Time only moves when the test calls `advance`, so a scheduling assertion is a sequence of calls
 * rather than a sleep with a tolerance. `refresh()` stays invalid: there is no display.
 */
class ManualFrameClock final : public FrameClock {
 public:
  explicit ManualFrameClock(std::chrono::steady_clock::time_point start) noexcept;

  void setMode(FrameMode mode) override;
  [[nodiscard]] FrameMode mode() const noexcept override;

  void requestFrame(std::chrono::steady_clock::time_point deadline) override;
  [[nodiscard]] bool frameRequested() const noexcept override;
  [[nodiscard]] std::chrono::steady_clock::time_point deadline() const noexcept override;
  void onFrameStarted() override;
  bool waitForFrame() override;
  void onFramePresented(std::chrono::steady_clock::time_point presented) override;
  [[nodiscard]] RefreshInfo refresh() const noexcept override;
  [[nodiscard]] std::chrono::steady_clock::time_point expectedPresentTime() const noexcept override;
  [[nodiscard]] double frameCostSeconds() const noexcept override;
  [[nodiscard]] std::chrono::steady_clock::time_point wakeTarget() const noexcept override;

  /** Moves the clock forward by @p delta. */
  void advance(std::chrono::steady_clock::duration delta) noexcept;
  [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept;

 private:
  std::chrono::steady_clock::time_point mNow;
  std::chrono::steady_clock::time_point mDeadline{};
  std::chrono::steady_clock::time_point mLastPresented{};
  std::chrono::steady_clock::time_point mFrameStart{};
  std::chrono::steady_clock::time_point mWakeTarget{};
  double mCostSeconds = 0.008;
  FrameMode mMode = FrameMode::OnDemand;
  bool mRequested = false;
  bool mHasPresented = false;
  bool mHasFrameStart = false;
};

/**
 * The present-driven clock. `waitForFrame` starts a frame at its deadline minus the measured frame
 * cost; the swapchain's present is still what puts it on the display, so this clock is the fallback
 * when no platform vblank source exists. `onFramePresented` turns the intervals it sees into a
 * refresh estimate and a presentation time for animation.
 */
class PresentFrameClock final : public FrameClock {
 public:
  explicit PresentFrameClock(std::chrono::steady_clock::time_point now) noexcept;

  void setMode(FrameMode mode) override;
  [[nodiscard]] FrameMode mode() const noexcept override;

  void requestFrame(std::chrono::steady_clock::time_point deadline) override;
  [[nodiscard]] bool frameRequested() const noexcept override;
  [[nodiscard]] std::chrono::steady_clock::time_point deadline() const noexcept override;
  void onFrameStarted() override;
  bool waitForFrame() override;
  void onFramePresented(std::chrono::steady_clock::time_point presented) override;
  [[nodiscard]] RefreshInfo refresh() const noexcept override;
  [[nodiscard]] std::chrono::steady_clock::time_point expectedPresentTime() const noexcept override;
  [[nodiscard]] double frameCostSeconds() const noexcept override;
  [[nodiscard]] std::chrono::steady_clock::time_point wakeTarget() const noexcept override;

 private:
  std::chrono::steady_clock::time_point mNow;
  std::chrono::steady_clock::time_point mDeadline{};
  std::chrono::steady_clock::time_point mLastPresented{};
  std::chrono::steady_clock::time_point mFrameStart{};
  std::chrono::steady_clock::time_point mWakeTarget{};
  double mIntervalSeconds = 1.0 / 60.0;
  double mPhaseSeconds = 0.0;
  double mCostSeconds = 0.008;
  uint32_t mPresentCount = 0;
  FrameMode mMode = FrameMode::OnDemand;
  bool mRequested = false;
  bool mHasPresented = false;
  bool mHasFrameStart = false;
};

/**
 * M1 clock with a platform vblank source.
 *
 * The source is used to learn the display interval and phase; the scheduler starts a frame at
 * `deadline - frameCost`, so the work runs just before the blank that will show it rather than being
 * released by the previous blank. The first wait aligns the clock to a blank.
 */
class PlatformFrameClock final : public FrameClock {
 public:
  explicit PlatformFrameClock(VBlankSource& source);

  void setMode(FrameMode mode) override;
  [[nodiscard]] FrameMode mode() const noexcept override;

  void requestFrame(std::chrono::steady_clock::time_point deadline) override;
  [[nodiscard]] bool frameRequested() const noexcept override;
  [[nodiscard]] std::chrono::steady_clock::time_point deadline() const noexcept override;
  void onFrameStarted() override;
  bool waitForFrame() override;
  void onFramePresented(std::chrono::steady_clock::time_point presented) override;
  [[nodiscard]] RefreshInfo refresh() const noexcept override;
  [[nodiscard]] std::chrono::steady_clock::time_point expectedPresentTime() const noexcept override;
  [[nodiscard]] double frameCostSeconds() const noexcept override;
  [[nodiscard]] std::chrono::steady_clock::time_point wakeTarget() const noexcept override;

 private:
  VBlankSource& mSource;
  std::chrono::steady_clock::time_point mDeadline{};
  std::chrono::steady_clock::time_point mLastPresented{};
  std::chrono::steady_clock::time_point mFrameStart{};
  std::chrono::steady_clock::time_point mWakeTarget{};
  double mIntervalSeconds = 1.0 / 60.0;
  double mCostSeconds = 0.008;
  FrameMode mMode = FrameMode::OnDemand;
  bool mRequested = false;
  bool mHasPresented = false;
  bool mHasFrameStart = false;
  bool mAligned = false;
};

} // namespace graphics::gpu

#endif // CORAL_GRAPHICS_GPU_FRAMECLOCK_H
