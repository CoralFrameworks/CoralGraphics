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

#include <gpu/FrameClock.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace graphics::gpu {

#if defined(__APPLE__)
/**
 * The Apple vblank source, implemented over `CADisplayLink` in `FrameClockApple.mm`.
 *
 * It is a separate translation unit because `CADisplayLink` is Objective-C and a run-loop object; the
 * factory is the only thing this file has to know. Passing @p view lets a per-window clock follow the
 * display that window is on (macOS); a null view uses the main display.
 */
[[nodiscard]] std::unique_ptr<VBlankSource> makeAppleDisplayLinkSource(void* view);
#endif

namespace {

/** The interval a refresh estimate is kept inside: 1 Hz to 1000 Hz. */
constexpr double kMinimumIntervalSeconds = 0.001;
constexpr double kMaximumIntervalSeconds = 1.0;

/** How much one observed interval moves the estimate. Slow enough to ride out a missed present. */
constexpr double kIntervalSmoothing = 0.2;

/** Bounds of the frame-cost estimate the deadline scheduler uses. */
constexpr double kMinimumCostSeconds = 0.001;
constexpr double kMaximumCostSeconds = 0.100;
constexpr double kCostSmoothing = 0.2;

/** Phase of an instant inside a period, in seconds. */
double phaseOf(const std::chrono::steady_clock::time_point instant, const double intervalSeconds) noexcept
{
  if (intervalSeconds <= 0.0)
    return 0.0;
  const double seconds = std::chrono::duration<double>(instant.time_since_epoch()).count();
  const double phase = std::fmod(seconds, intervalSeconds);
  return phase >= 0.0 ? phase : phase + intervalSeconds;
}

void recordCost(double& cost, const std::chrono::steady_clock::duration sample) noexcept
{
  const double seconds = std::clamp(std::chrono::duration<double>(sample).count(), kMinimumCostSeconds,
    kMaximumCostSeconds);
  cost += (seconds - cost) * kCostSmoothing;
}

std::chrono::steady_clock::time_point deadlineFor(
        const std::chrono::steady_clock::time_point deadline, const double costSeconds,
        const std::chrono::steady_clock::time_point now) noexcept
{
  const auto start = deadline - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                   std::chrono::duration<double>(costSeconds));
  return std::max(start, now);
}

} // namespace anonymous

// ==================================================================================================
// ManualFrameClock
// ==================================================================================================

ManualFrameClock::ManualFrameClock(const std::chrono::steady_clock::time_point start) noexcept : mNow(start) {}

void ManualFrameClock::setMode(const FrameMode mode) {
  mMode = mode;
}

FrameMode ManualFrameClock::mode() const noexcept {
  return mMode;
}

void ManualFrameClock::requestFrame(const std::chrono::steady_clock::time_point deadline) {
  if (!mRequested || deadline < mDeadline) {
    mDeadline = deadline;
  }
  mRequested = true;
}

bool ManualFrameClock::frameRequested() const noexcept {
  return mRequested;
}

std::chrono::steady_clock::time_point ManualFrameClock::deadline() const noexcept {
  return mDeadline;
}

void ManualFrameClock::onFrameStarted() {
  mFrameStart = mNow;
  mHasFrameStart = true;
}

bool ManualFrameClock::waitForFrame() {
  mWakeTarget = deadlineFor(mDeadline, mCostSeconds, mNow);
  // Deterministic by construction: a test that wants to model a blocked scheduler advances the clock
  // itself; the wake instant is recorded instead of slept to.
  return mRequested;
}

void ManualFrameClock::onFramePresented(const std::chrono::steady_clock::time_point presented) {
  if (mHasFrameStart && presented > mFrameStart) {
    recordCost(mCostSeconds, presented - mFrameStart);
  }
  mLastPresented = presented;
  mHasPresented = true;
  mRequested = false;
}

RefreshInfo ManualFrameClock::refresh() const noexcept {
  return RefreshInfo{};
}

std::chrono::steady_clock::time_point ManualFrameClock::expectedPresentTime() const noexcept {
  // One display interval ahead of "now": the frame being prepared is the next one shown. There is no
  // measured refresh offline, so the nominal 60 Hz interval is the useful answer.
  return mNow + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                  std::chrono::duration<double>(1.0 / 60.0));
}

double ManualFrameClock::frameCostSeconds() const noexcept {
  return mCostSeconds;
}

std::chrono::steady_clock::time_point ManualFrameClock::wakeTarget() const noexcept {
  return mWakeTarget;
}

void ManualFrameClock::advance(const std::chrono::steady_clock::duration delta) noexcept {
  mNow += delta;
}

std::chrono::steady_clock::time_point ManualFrameClock::now() const noexcept {
  return mNow;
}

// ==================================================================================================
// PresentFrameClock
// ==================================================================================================

PresentFrameClock::PresentFrameClock(const std::chrono::steady_clock::time_point now) noexcept : mNow(now) {}

void PresentFrameClock::setMode(const FrameMode mode) {
  mMode = mode;
}

FrameMode PresentFrameClock::mode() const noexcept {
  return mMode;
}

void PresentFrameClock::requestFrame(const std::chrono::steady_clock::time_point deadline) {
  mNow = std::chrono::steady_clock::now();
  if (!mRequested || deadline < mDeadline) {
    mDeadline = deadline;
  }
  mRequested = true;
}

bool PresentFrameClock::frameRequested() const noexcept {
  return mRequested;
}

std::chrono::steady_clock::time_point PresentFrameClock::deadline() const noexcept {
  return mDeadline;
}

void PresentFrameClock::onFrameStarted() {
  mFrameStart = std::chrono::steady_clock::now();
  mHasFrameStart = true;
}

bool PresentFrameClock::waitForFrame() {
  mNow = std::chrono::steady_clock::now();
  mWakeTarget = deadlineFor(mDeadline, mCostSeconds, mNow);
  if (mWakeTarget > mNow) {
    std::this_thread::sleep_until(mWakeTarget);
    mNow = std::chrono::steady_clock::now();
  }
  return mRequested;
}

void PresentFrameClock::onFramePresented(const std::chrono::steady_clock::time_point presented) {
  if (mHasFrameStart && presented > mFrameStart) {
    recordCost(mCostSeconds, presented - mFrameStart);
  }
  if (mHasPresented) {
    const double delta = std::chrono::duration<double>(presented - mLastPresented).count();
    if (delta > 0.0 && std::isfinite(delta)) {
      const double clamped = std::clamp(delta, kMinimumIntervalSeconds, kMaximumIntervalSeconds);
      // One interval is a sample, not a recalibration: a single long frame during startup must not
      // convince the clock that the display is 10 Hz forever.
      mIntervalSeconds += (clamped - mIntervalSeconds) * kIntervalSmoothing;
    }
  } else {
    mIntervalSeconds = 1.0 / 60.0;
  }
  mLastPresented = presented;
  mHasPresented = true;
  ++mPresentCount;
  mPhaseSeconds = phaseOf(presented, mIntervalSeconds);
  mRequested = false;
  mNow = presented;
}

RefreshInfo PresentFrameClock::refresh() const noexcept {
  RefreshInfo info;
  // A rate needs two presents: one interval from a single sample would be a guess, and the first
  // frame of a run is usually not representative of the steady state.
  info.valid = mPresentCount >= 2;
  info.hertz = info.valid ? 1.0 / mIntervalSeconds : 0.0;
  info.phaseSeconds = info.valid ? mPhaseSeconds : 0.0;
  info.variable = false;
  return info;
}

std::chrono::steady_clock::time_point PresentFrameClock::expectedPresentTime() const noexcept {
  const std::chrono::steady_clock::time_point anchor =
          mHasPresented ? mLastPresented : std::chrono::steady_clock::now();
  return anchor + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(mIntervalSeconds));
}

double PresentFrameClock::frameCostSeconds() const noexcept {
  return mCostSeconds;
}

std::chrono::steady_clock::time_point PresentFrameClock::wakeTarget() const noexcept {
  return mWakeTarget;
}

// ==================================================================================================
// PlatformFrameClock
// ==================================================================================================

PlatformFrameClock::PlatformFrameClock(VBlankSource& source) : mSource(source) {}

void PlatformFrameClock::setMode(const FrameMode mode) {
  mMode = mode;
}

FrameMode PlatformFrameClock::mode() const noexcept {
  return mMode;
}

void PlatformFrameClock::requestFrame(const std::chrono::steady_clock::time_point deadline) {
  if (!mRequested || deadline < mDeadline) {
    mDeadline = deadline;
  }
  mRequested = true;
}

bool PlatformFrameClock::frameRequested() const noexcept {
  return mRequested;
}

std::chrono::steady_clock::time_point PlatformFrameClock::deadline() const noexcept {
  return mDeadline;
}

void PlatformFrameClock::onFrameStarted() {
  mFrameStart = std::chrono::steady_clock::now();
  mHasFrameStart = true;
}

bool PlatformFrameClock::waitForFrame() {
  if (!mRequested) {
    return false;
  }
  // Align once: the vblank source tells the clock where the display's phase is. After that the
  // scheduler uses deadlines and the measured frame cost, so it can start just before the blank
  // instead of being released by the previous one.
  if (!mAligned) {
    // Polled sources (DXGI/DRM) need two blanks for an interval estimate. Event-driven sources
    // (Wayland/X11) can only be armed by a present, so their first wait has nothing to consume.
    if (!mSource.isEventDriven()) {
      mSource.waitForVBlank();
      mSource.waitForVBlank();
      const RefreshInfo info = mSource.refresh();
      if (info.valid) {
        mIntervalSeconds = 1.0 / info.hertz;
      }
    }
    mAligned = true;
  }
  if (mSource.isEventDriven()) {
    // Consume the previous frame's tick, then arm the next one before the draw. The callback only
    // fires after the commit the frame's present performs, so it cannot be armed after the present
    // and waited on before the next draw.
    mSource.waitForVBlank();
    mSource.notePresented();
  }
  const auto now = std::chrono::steady_clock::now();
  mWakeTarget = deadlineFor(mDeadline, mCostSeconds, now);
  if (mWakeTarget > now) {
    std::this_thread::sleep_until(mWakeTarget);
  }
  return true;
}

void PlatformFrameClock::onFramePresented(const std::chrono::steady_clock::time_point presented) {
  if (mHasFrameStart && presented > mFrameStart) {
    recordCost(mCostSeconds, presented - mFrameStart);
  }
  mLastPresented = presented;
  mHasPresented = true;
  mRequested = false;
  const RefreshInfo info = mSource.refresh();
  if (info.valid) {
    mIntervalSeconds = 1.0 / info.hertz;
  }
}

RefreshInfo PlatformFrameClock::refresh() const noexcept {
  return mSource.refresh();
}

std::chrono::steady_clock::time_point PlatformFrameClock::expectedPresentTime() const noexcept {
  const std::chrono::steady_clock::time_point anchor =
          mHasPresented ? mLastPresented : std::chrono::steady_clock::now();
  return anchor + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(mIntervalSeconds));
}

double PlatformFrameClock::frameCostSeconds() const noexcept {
  return mCostSeconds;
}

std::chrono::steady_clock::time_point PlatformFrameClock::wakeTarget() const noexcept {
  return mWakeTarget;
}

// ==================================================================================================
// Platform vertical blank
// ==================================================================================================

#if defined(_WIN32)

namespace {

/** Shared refresh bookkeeping for every Windows display source. */
struct DisplayTiming {
  std::chrono::steady_clock::time_point last{};
  double intervalSeconds = 1.0 / 60.0;
  double phaseSeconds = 0.0;
  uint32_t count = 0;
  bool hasLast = false;

  void record(const std::chrono::steady_clock::time_point now) noexcept {
    if (hasLast) {
      const double delta = std::chrono::duration<double>(now - last).count();
      if (delta > 0.0 && std::isfinite(delta)) {
        const double clamped = std::clamp(delta, kMinimumIntervalSeconds, kMaximumIntervalSeconds);
        intervalSeconds += (clamped - intervalSeconds) * kIntervalSmoothing;
      }
    }
    last = now;
    hasLast = true;
    phaseSeconds = phaseOf(now, intervalSeconds);
    ++count;
  }

  [[nodiscard]] RefreshInfo refresh() const noexcept {
    RefreshInfo info;
    info.valid = count >= 2;
    info.hertz = info.valid ? 1.0 / intervalSeconds : 0.0;
    info.phaseSeconds = info.valid ? phaseSeconds : 0.0;
    return info;
  }
};

/**
 * DXGI's output vblank, the platform tick the M4 scheduler prefers.
 *
 * `IDXGIOutput::WaitForVBlank` blocks on the display's vertical blank without needing the swapchain
 * that presents to it, which is exactly what a Vulkan window needs: the swapchain belongs to the RHI,
 * not to this clock. One output is enough -- Windows desktops normally share the composition clock --
 * and the DwmFlush source below remains the fallback when DXGI cannot enumerate an attached output.
 */
class DxgiVBlankSource final : public VBlankSource {
 public:
  static std::unique_ptr<DxgiVBlankSource> create() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
      return nullptr;
    }
    IDXGIAdapter1* adapter = nullptr;
    IDXGIOutput* output = nullptr;
    for (UINT adapterIndex = 0;
         factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND; ++adapterIndex) {
      if (adapter == nullptr) {
        continue;
      }
      for (UINT outputIndex = 0;
           adapter->EnumOutputs(outputIndex, &output) != DXGI_ERROR_NOT_FOUND; ++outputIndex) {
        if (output == nullptr) {
          continue;
        }
        DXGI_OUTPUT_DESC description{};
        if (SUCCEEDED(output->GetDesc(&description)) && description.AttachedToDesktop) {
          adapter->Release();
          return std::unique_ptr<DxgiVBlankSource>(new DxgiVBlankSource(factory, output));
        }
        output->Release();
        output = nullptr;
      }
      adapter->Release();
      adapter = nullptr;
    }
    factory->Release();
    return nullptr;
  }

  ~DxgiVBlankSource() override {
    if (mOutput != nullptr) {
      mOutput->Release();
    }
    if (mFactory != nullptr) {
      mFactory->Release();
    }
  }

  bool waitForVBlank() override {
    if (mOutput == nullptr) {
      return false;
    }
    if (FAILED(mOutput->WaitForVBlank())) {
      return false;
    }
    mTiming.record(std::chrono::steady_clock::now());
    return true;
  }

  [[nodiscard]] RefreshInfo refresh() const override {
    return mTiming.refresh();
  }

 private:
  DxgiVBlankSource(IDXGIFactory1* factory, IDXGIOutput* output) : mFactory(factory), mOutput(output) {}

  IDXGIFactory1* mFactory = nullptr;
  IDXGIOutput* mOutput = nullptr;
  DisplayTiming mTiming;
};

/**
 * Windows' composition clock, used when DXGI has no attached output.
 *
 * `DwmFlush` blocks until the desktop composition has consumed a frame. It is the documented fallback
 * for `IDXGIOutput::WaitForVBlank`; resolved dynamically so the graphics library does not gain a link
 * dependency on `dwmapi` for builds that never open a window.
 */
class WindowsVBlankSource final : public VBlankSource {
 public:
  WindowsVBlankSource() {
    HMODULE module = GetModuleHandleW(L"dwmapi.dll");
    if (module == nullptr) {
      module = LoadLibraryW(L"dwmapi.dll");
      mOwnsModule = module != nullptr;
    }
    if (module != nullptr) {
      mFlush = reinterpret_cast<DwmFlushFn>(GetProcAddress(module, "DwmFlush"));
      mModule = module;
    }
  }

  ~WindowsVBlankSource() override {
    if (mOwnsModule && mModule != nullptr) {
      FreeLibrary(mModule);
    }
  }

  bool waitForVBlank() override {
    if (mFlush == nullptr) {
      return false;
    }
    const HRESULT result = mFlush();
    if (FAILED(result)) {
      return false;
    }
    mTiming.record(std::chrono::steady_clock::now());
    return true;
  }

  [[nodiscard]] RefreshInfo refresh() const override {
    return mTiming.refresh();
  }

 private:
  using DwmFlushFn = HRESULT(WINAPI*)();
  DwmFlushFn mFlush = nullptr;
  HMODULE mModule = nullptr;
  bool mOwnsModule = false;
  DisplayTiming mTiming;
};

} // namespace anonymous

#elif defined(__linux__)

#include <cerrno>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#if defined(CORAL_HAVE_X11)
#include <X11/Xlib.h>
#endif

namespace {

/**
 * Linux's kernel vblank, through the DRM device.
 *
 * `drmWaitVBlank` is the display controller's vertical blank, independent of a window system or a
 * swapchain, which fits this clock: the RHI owns the swapchain and the clock must not. It is opened
 * from `/dev/dri/cardN`; a user without access to the card node (a container, or a machine where the
 * compositor uses a render node only) gets no source and the caller falls back to the present clock.
 *
 * The ABI is declared here rather than including `xf86drm.h`: the structs are stable kernel ABI and
 * this keeps the graphics library from taking a libdrm build dependency for one ioctl.
 */
struct DrmWaitVBlankRequest {
  unsigned int type;
  unsigned int sequence;
  unsigned long signal;
};

struct DrmWaitVBlankReply {
  unsigned int type;
  unsigned int sequence;
  long tvSec;
  long tvUsec;
};

union DrmWaitVBlank {
  DrmWaitVBlankRequest request;
  DrmWaitVBlankReply reply;
};

constexpr unsigned int kDrmVBlankRelative = 0x01;
#define CORAL_DRM_IOCTL_WAIT_VBLANK _IOWR('d', 0x3a, union DrmWaitVBlank)

class DrmVBlankSource final : public VBlankSource {
 public:
  static std::unique_ptr<DrmVBlankSource> create() {
    for (int index = 0; index < 8; ++index) {
      char path[32];
      std::snprintf(path, sizeof(path), "/dev/dri/card%d", index);
      const int fd = open(path, O_RDWR);
      if (fd < 0) {
        continue;
      }
      auto source = std::unique_ptr<DrmVBlankSource>(new DrmVBlankSource(fd));
      if (source->probe()) {
        return source;
      }
    }
    return nullptr;
  }

  ~DrmVBlankSource() override {
    if (mFd >= 0) {
      close(mFd);
    }
  }

  bool waitForVBlank() override {
    if (mFd < 0) {
      return false;
    }
    DrmWaitVBlank wait{};
    wait.request.type = kDrmVBlankRelative;
    wait.request.sequence = 1;
    if (ioctl(mFd, CORAL_DRM_IOCTL_WAIT_VBLANK, &wait) < 0) {
      return false;
    }
    mTiming.record(std::chrono::steady_clock::now());
    return true;
  }

  [[nodiscard]] RefreshInfo refresh() const override {
    return mTiming.refresh();
  }

 private:
  explicit DrmVBlankSource(const int fd) : mFd(fd) {}

  bool probe() {
    DrmWaitVBlank wait{};
    wait.request.type = kDrmVBlankRelative;
    wait.request.sequence = 1;
    return ioctl(mFd, CORAL_DRM_IOCTL_WAIT_VBLANK, &wait) == 0;
  }

  int mFd = -1;

  struct Timing {
    std::chrono::steady_clock::time_point last{};
    double intervalSeconds = 1.0 / 60.0;
    double phaseSeconds = 0.0;
    uint32_t count = 0;
    bool hasLast = false;

    void record(const std::chrono::steady_clock::time_point now) noexcept {
      if (hasLast) {
        const double delta = std::chrono::duration<double>(now - last).count();
        if (delta > 0.0 && std::isfinite(delta)) {
          const double clamped = std::clamp(delta, kMinimumIntervalSeconds, kMaximumIntervalSeconds);
          intervalSeconds += (clamped - intervalSeconds) * kIntervalSmoothing;
        }
      }
      last = now;
      hasLast = true;
      phaseSeconds = phaseOf(now, intervalSeconds);
      ++count;
    }

    [[nodiscard]] RefreshInfo refresh() const noexcept {
      RefreshInfo info;
      info.valid = count >= 2;
      info.hertz = info.valid ? 1.0 / intervalSeconds : 0.0;
      info.phaseSeconds = info.valid ? phaseSeconds : 0.0;
      return info;
    }
  } mTiming;
};

} // namespace anonymous

// ------------------------------------------------------------------------------------------------
// Wayland: the surface's frame callback is the per-window tick
// ------------------------------------------------------------------------------------------------

struct WlDisplay;
struct WlSurface;
struct WlCallback;
struct WlEventQueue;
struct WlProxy;

struct WlCallbackListener {
  void (*done)(void* data, WlCallback* callback, uint32_t callbackData);
};

/**
 * Wayland's `wl_surface.frame` callback.
 *
 * The callback is armed by `notePresented` and fires on the next commit, so the next wait consumes
 * the previous frame's tick rather than asking for a tick before anything has been committed. The
 * library is loaded dynamically: the graphics library does not take a link dependency on
 * libwayland-client for a window that may never exist.
 */
class WaylandVBlankSource final : public VBlankSource {
 public:
  static std::unique_ptr<WaylandVBlankSource> create(void* display, void* surface,
      const bool waitForDisplay) {
    if (display == nullptr || surface == nullptr) return nullptr;
    void* library = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) return nullptr;
    Functions functions{};
    functions.createQueue = reinterpret_cast<CreateQueueFn>(dlsym(library, "wl_display_create_queue"));
    functions.marshalFlags = reinterpret_cast<MarshalFlagsFn>(dlsym(library, "wl_proxy_marshal_flags"));
    functions.proxyVersion = reinterpret_cast<ProxyVersionFn>(dlsym(library, "wl_proxy_get_version"));
    functions.setQueue = reinterpret_cast<SetQueueFn>(dlsym(library, "wl_proxy_set_queue"));
    functions.addListener = reinterpret_cast<AddListenerFn>(dlsym(library, "wl_proxy_add_listener"));
    functions.proxyDestroy = reinterpret_cast<ProxyDestroyFn>(dlsym(library, "wl_proxy_destroy"));
    functions.dispatchQueue = reinterpret_cast<DispatchQueueFn>(dlsym(library, "wl_display_dispatch_queue"));
    functions.dispatchQueuePending = reinterpret_cast<DispatchQueuePendingFn>(
      dlsym(library, "wl_display_dispatch_queue_pending"));
    functions.destroyQueue = reinterpret_cast<DestroyQueueFn>(dlsym(library, "wl_event_queue_destroy"));
    functions.callbackInterface = dlsym(library, "wl_callback_interface");
    if (functions.createQueue == nullptr || functions.marshalFlags == nullptr ||
        functions.proxyVersion == nullptr || functions.setQueue == nullptr ||
        functions.addListener == nullptr || functions.proxyDestroy == nullptr ||
        functions.dispatchQueue == nullptr || functions.destroyQueue == nullptr ||
        functions.callbackInterface == nullptr) {
      dlclose(library);
      return nullptr;
    }
    return std::unique_ptr<WaylandVBlankSource>(
      new WaylandVBlankSource(library, functions, display, surface, waitForDisplay));
  }

  ~WaylandVBlankSource() override {
    releasePending();
    if (mLibrary != nullptr) dlclose(mLibrary);
  }

  bool waitForVBlank() override {
    if (!mPending) return false;
    int dispatched = 0;
    if (mWaitForDisplay) {
      while (!mDone && dispatched >= 0) dispatched = mFunctions.dispatchQueue(mDisplay, mQueue);
    } else {
      // The UI thread owns the single display reader for a threaded window. Dispatch only what
      // is already queued; the frame clock proceeds and the next commit wakes the next tick.
      dispatched = mFunctions.dispatchQueuePending(mDisplay, mQueue);
    }
    if (dispatched < 0 || !mDone) {
      releasePending();
      return false;
    }
    mTiming.record(std::chrono::steady_clock::now());
    releasePending();
    return true;
  }

  void notePresented() override {
    releasePending();
    mQueue = mFunctions.createQueue(mDisplay);
    if (mQueue == nullptr) return;
    // WL_SURFACE_FRAME == 3. `wl_surface_frame` is a static inline in the protocol header, so the
    // proxy marshal entry point is what a dynamically loaded client can call.
    WlProxy* const surface = reinterpret_cast<WlProxy*>(mSurface);
    mCallback = reinterpret_cast<WlCallback*>(mFunctions.marshalFlags(surface, 3,
      mFunctions.callbackInterface, mFunctions.proxyVersion(surface), 0));
    if (mCallback == nullptr) {
      releasePending();
      return;
    }
    mFunctions.setQueue(reinterpret_cast<WlProxy*>(mCallback), mQueue);
    static const WlCallbackListener listener = {
      [](void* data, WlCallback* /*callback*/, const uint32_t /*callbackData*/) {
        static_cast<WaylandVBlankSource*>(data)->mDone = true;
      },
    };
    mFunctions.addListener(reinterpret_cast<WlProxy*>(mCallback),
      reinterpret_cast<const void*>(&listener), this);
    mDone = false;
    mPending = true;
  }

  [[nodiscard]] RefreshInfo refresh() const override {
    return mTiming.refresh();
  }

  [[nodiscard]] bool isEventDriven() const noexcept override {
    return true;
  }

 private:
  using CreateQueueFn = WlEventQueue* (*)(WlDisplay*);
  using MarshalFlagsFn = WlProxy* (*)(WlProxy*, uint32_t, const void*, uint32_t, uint32_t, ...);
  using ProxyVersionFn = uint32_t (*)(WlProxy*);
  using SetQueueFn = void (*)(WlProxy*, WlEventQueue*);
  using AddListenerFn = int (*)(WlProxy*, const void*, void*);
  using ProxyDestroyFn = void (*)(WlProxy*);
  using DispatchQueueFn = int (*)(WlDisplay*, WlEventQueue*);
  using DispatchQueuePendingFn = int (*)(WlDisplay*, WlEventQueue*);
  using DestroyQueueFn = void (*)(WlEventQueue*);

  struct Functions {
    CreateQueueFn createQueue = nullptr;
    MarshalFlagsFn marshalFlags = nullptr;
    ProxyVersionFn proxyVersion = nullptr;
    SetQueueFn setQueue = nullptr;
    AddListenerFn addListener = nullptr;
    ProxyDestroyFn proxyDestroy = nullptr;
    DispatchQueueFn dispatchQueue = nullptr;
    DispatchQueuePendingFn dispatchQueuePending = nullptr;
    DestroyQueueFn destroyQueue = nullptr;
    const void* callbackInterface = nullptr;
  };

  WaylandVBlankSource(void* library, const Functions& functions, void* display, void* surface,
      const bool waitForDisplay)
      : mLibrary(library), mFunctions(functions), mDisplay(static_cast<WlDisplay*>(display)),
        mSurface(static_cast<WlSurface*>(surface)), mWaitForDisplay(waitForDisplay) {}

  void releasePending() {
    if (mCallback != nullptr) {
      mFunctions.proxyDestroy(reinterpret_cast<WlProxy*>(mCallback));
      mCallback = nullptr;
    }
    if (mQueue != nullptr) {
      mFunctions.destroyQueue(mQueue);
      mQueue = nullptr;
    }
    mPending = false;
    mDone = false;
  }

  struct Timing {
    std::chrono::steady_clock::time_point last{};
    double intervalSeconds = 1.0 / 60.0;
    double phaseSeconds = 0.0;
    uint32_t count = 0;
    bool hasLast = false;

    void record(const std::chrono::steady_clock::time_point now) noexcept {
      if (hasLast) {
        const double delta = std::chrono::duration<double>(now - last).count();
        if (delta > 0.0 && std::isfinite(delta)) {
          const double clamped = std::clamp(delta, kMinimumIntervalSeconds, kMaximumIntervalSeconds);
          intervalSeconds += (clamped - intervalSeconds) * kIntervalSmoothing;
        }
      }
      last = now;
      hasLast = true;
      phaseSeconds = phaseOf(now, intervalSeconds);
      ++count;
    }

    [[nodiscard]] RefreshInfo refresh() const noexcept {
      RefreshInfo info;
      info.valid = count >= 2;
      info.hertz = info.valid ? 1.0 / intervalSeconds : 0.0;
      info.phaseSeconds = info.valid ? phaseSeconds : 0.0;
      return info;
    }
  };

  void* mLibrary = nullptr;
  Functions mFunctions{};
  WlDisplay* mDisplay = nullptr;
  WlSurface* mSurface = nullptr;
  WlCallback* mCallback = nullptr;
  WlEventQueue* mQueue = nullptr;
  Timing mTiming;
  bool mPending = false;
  bool mDone = false;
  bool mWaitForDisplay = true;
};

#if defined(CORAL_HAVE_X11)

#include <poll.h>

// ------------------------------------------------------------------------------------------------
// X11: PresentNotifyMSC on the window, through a private display connection
// ------------------------------------------------------------------------------------------------

/**
 * Waits for one X event with a bounded timeout.
 *
 * `XNextEvent` blocks forever when the server never answers a PresentNotifyMSC probe (WSLg's X
 * server is one such case). Startup must not hang because a display has no frame counter, so the
 * probe polls the connection first and gives up on timeout.
 */
inline bool waitForXEvent(Display* connection, XEvent& event, const int timeoutMs) {
  pollfd descriptor{ ConnectionNumber(connection), POLLIN, 0 };
  if (poll(&descriptor, 1, timeoutMs) <= 0) return false;
  XNextEvent(connection, &event);
  return true;
}

/**
 * X11's Present extension, `PresentNotifyMSC`, on the window the RHI presents to.
 *
 * A private X connection means the wait does not steal the application's events, and
 * `PresentNotifyMSC` asks for a completion event at a target MSC without owning the swapchain. The
 * extension is loaded dynamically: libXpresent is optional on many desktops.
 */
class X11VBlankSource final : public VBlankSource {
 public:
  static std::unique_ptr<X11VBlankSource> create(void* display, void* window) {
    if (display == nullptr || window == nullptr) return nullptr;
    Display* const source = static_cast<Display*>(display);
    const char* const name = XDisplayString(source);
    Display* const connection = XOpenDisplay(name);
    if (connection == nullptr) return nullptr;
    void* library = dlopen("libXpresent.so.1", RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
      XCloseDisplay(connection);
      return nullptr;
    }
    auto query = reinterpret_cast<QueryExtensionFn>(dlsym(library, "XPresentQueryExtension"));
    auto notify = reinterpret_cast<NotifyMSCFn>(dlsym(library, "XPresentNotifyMSC"));
    auto selectInput = reinterpret_cast<SelectInputFn>(dlsym(library, "XPresentSelectInput"));
    int majorOpcode = 0;
    int eventBase = 0;
    int errorBase = 0;
    if (query == nullptr || notify == nullptr || selectInput == nullptr ||
        !query(connection, &majorOpcode, &eventBase, &errorBase)) {
      // XCloseDisplay before dlclose: Xlib calls the extension's close hook, which lives in the
      // library, so unloading it first jumps into unmapped code.
      XCloseDisplay(connection);
      dlclose(library);
      return nullptr;
    }
    // PresentNotifyMSC only delivers its completion event to a client that selected the window's
    // Present input; the private connection is the one that will wait for it.
    const Window target = static_cast<Window>(reinterpret_cast<uintptr_t>(window));
    selectInput(connection, target, 2 /* PresentCompleteNotifyMask */);

    // Probe before handing the source out: a server that answers with MSC 0 immediately (WSLg's X
    // server does) has no frame counter behind the notification, and a source that "waits" for zero
    // milliseconds would leave the clock with no cadence at all. A real Present server advances the
    // MSC and takes about one refresh interval for the next one.
    XEvent first{};
    notify(connection, target, 1, 0, 0, 0);
    if (!waitForXEvent(connection, first, 250)) {
      XCloseDisplay(connection);
      dlclose(library);
      return nullptr;
    }
    const auto* firstComplete = reinterpret_cast<const PresentCompleteEvent*>(&first);
    XEvent second{};
    const auto before = std::chrono::steady_clock::now();
    notify(connection, target, 2, firstComplete->msc + 1, 0, 0);
    if (!waitForXEvent(connection, second, 250)) {
      XCloseDisplay(connection);
      dlclose(library);
      return nullptr;
    }
    const auto elapsed = std::chrono::steady_clock::now() - before;
    const auto* secondComplete = reinterpret_cast<const PresentCompleteEvent*>(&second);
    if (secondComplete->msc <= firstComplete->msc ||
        elapsed < std::chrono::milliseconds(5)) {
      XCloseDisplay(connection);
      dlclose(library);
      return nullptr;
    }
    return std::unique_ptr<X11VBlankSource>(new X11VBlankSource(library, connection, notify, target,
      secondComplete->msc));
  }

  ~X11VBlankSource() override {
    if (mConnection != nullptr) XCloseDisplay(mConnection);
    if (mLibrary != nullptr) dlclose(mLibrary);
  }

  bool waitForVBlank() override {
    if (!mPending) return false;
    XEvent event{};
    XNextEvent(mConnection, &event);
    // The completion event's MSC is the display's frame counter; requesting `msc + 1` next time is
    // what makes the following wait land on the *next* blank instead of completing immediately.
    const auto* complete = reinterpret_cast<const PresentCompleteEvent*>(&event);
    if (complete->msc != 0) {
      mLastMsc = complete->msc;
      mHasMsc = true;
    }
    mPending = false;
    mTiming.record(std::chrono::steady_clock::now());
    return true;
  }

  void notePresented() override {
    mNotify(mConnection, mWindow, static_cast<uint32_t>(++mSerial), mHasMsc ? mLastMsc + 1 : 0, 0, 0);
    mPending = true;
  }

  [[nodiscard]] RefreshInfo refresh() const override {
    return mTiming.refresh();
  }

  [[nodiscard]] bool isEventDriven() const noexcept override {
    return true;
  }

 private:
  using QueryExtensionFn = int (*)(Display*, int*, int*, int*);
  using NotifyMSCFn = void (*)(Display*, Window, uint32_t, uint64_t, uint64_t, uint64_t);
  using SelectInputFn = unsigned long (*)(Display*, Window, unsigned long);

  /**
   * The layout of `XPresentCompleteNotifyEvent`, declared here so the X11 source does not require
   * libXpresent's development header; it is the event ABI from `presenttokens.h`.
   */
  struct PresentCompleteEvent {
    int type;
    unsigned long serial;
    Bool send_event;
    Display* display;
    int extension;
    int evtype;
    uint32_t eid;
    Window window;
    uint32_t serialNumber;
    uint64_t ust;
    uint64_t msc;
    uint8_t kind;
    uint8_t mode;
  };

  X11VBlankSource(void* library, Display* connection, const NotifyMSCFn notify, const Window window,
      const uint64_t lastMsc)
      : mLibrary(library), mConnection(connection), mNotify(notify), mWindow(window),
        mLastMsc(lastMsc), mHasMsc(true) {
    // The two probe requests above used serials 1 and 2 on this connection.
    mSerial = 2;
  }

  struct Timing {
    std::chrono::steady_clock::time_point last{};
    double intervalSeconds = 1.0 / 60.0;
    double phaseSeconds = 0.0;
    uint32_t count = 0;
    bool hasLast = false;

    void record(const std::chrono::steady_clock::time_point now) noexcept {
      if (hasLast) {
        const double delta = std::chrono::duration<double>(now - last).count();
        if (delta > 0.0 && std::isfinite(delta)) {
          const double clamped = std::clamp(delta, kMinimumIntervalSeconds, kMaximumIntervalSeconds);
          intervalSeconds += (clamped - intervalSeconds) * kIntervalSmoothing;
        }
      }
      last = now;
      hasLast = true;
      phaseSeconds = phaseOf(now, intervalSeconds);
      ++count;
    }

    [[nodiscard]] RefreshInfo refresh() const noexcept {
      RefreshInfo info;
      info.valid = count >= 2;
      info.hertz = info.valid ? 1.0 / intervalSeconds : 0.0;
      info.phaseSeconds = info.valid ? phaseSeconds : 0.0;
      return info;
    }
  };

  void* mLibrary = nullptr;
  Display* mConnection = nullptr;
  NotifyMSCFn mNotify = nullptr;
  Window mWindow = 0;
  unsigned long mSerial = 0;
  uint64_t mLastMsc = 0;
  bool mHasMsc = false;
  Timing mTiming;
  bool mPending = false;
};

#endif // CORAL_HAVE_X11

#endif // _WIN32 / __linux__



std::unique_ptr<VBlankSource> makePlatformVBlankSource() {
#if defined(_WIN32)
  if (std::unique_ptr<DxgiVBlankSource> dxgi = DxgiVBlankSource::create(); dxgi != nullptr) {
    return dxgi;
  }
  return std::make_unique<WindowsVBlankSource>();
#elif defined(__linux__)
  return DrmVBlankSource::create();
#elif defined(__APPLE__)
  return makeAppleDisplayLinkSource(nullptr);
#else
  return nullptr;
#endif
}

std::unique_ptr<VBlankSource> makePlatformVBlankSource(const NativeDisplay& native,
    const bool waitForDisplay) {
#if defined(_WIN32)
  (void)native;
  return makePlatformVBlankSource();
#elif defined(__linux__)
  if (native.kind == NativeDisplayKind::Wayland && native.display != nullptr && native.surface != nullptr) {
    if (std::unique_ptr<WaylandVBlankSource> wayland =
          WaylandVBlankSource::create(native.display, native.surface, waitForDisplay);
        wayland != nullptr) {
      return wayland;
    }
  }
#if defined(CORAL_HAVE_X11)
  if (waitForDisplay && native.kind == NativeDisplayKind::X11 && native.display != nullptr &&
      native.surface != nullptr) {
    if (std::unique_ptr<X11VBlankSource> x11 = X11VBlankSource::create(native.display, native.surface);
        x11 != nullptr) {
      return x11;
    }
  }
#endif
  return DrmVBlankSource::create();
#elif defined(__APPLE__)
  (void)waitForDisplay;
  return makeAppleDisplayLinkSource(native.kind == NativeDisplayKind::MacOS ? native.surface : nullptr);
#else
  (void)native;
  return nullptr;
#endif
}

} // namespace graphics::gpu
