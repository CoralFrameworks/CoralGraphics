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

/**
 * @file FrameClockApple.mm
 *
 * The Apple display tick: `CADisplayLink`, on a run loop owned by this file.
 *
 * `CVDisplayLink` was the previous source. It is deprecated on macOS 15 and has no iOS counterpart, so
 * one implementation could not serve both platforms -- the iOS build simply had no vblank source and
 * fell back to the present-driven estimate. `CADisplayLink` exists on both, and its callback carries
 * the display's own `timestamp`, `targetTimestamp` and `duration`, which is the information a UI
 * scheduler needs to sample animation at the frame it will actually be shown at.
 *
 * ## The thread model
 *
 * The link belongs to a run loop, and a run loop belongs to a thread. This source owns that thread:
 * `waitForVBlank` blocks the caller (the render thread), while the callback runs on the run-loop
 * thread and only records the tick. Keeping the two separate is what lets `FrameClock` block without
 * blocking the display timer, which would otherwise stop ticking while the renderer was working.
 *
 * `CAMetalDisplayLink` is deliberately not used here. It hands out a drawable at the target
 * presentation time, so using it as a *tick* source would acquire a drawable that the swapchain also
 * contends for. It belongs to the presentation path instead -- see `gpu::MetalDisplayLink` and
 * `WindowSurface::beginFrame(void*)`, which render into the drawable the update carries and present it
 * on the same thread.
 */

#include <gpu/FrameClock.h>

#include <TargetConditionals.h>

#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>

#if defined(TARGET_OS_OSX) && TARGET_OS_OSX
#import <AppKit/AppKit.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

@class CoralDisplayLinkTarget;

/** AppKit's NSView/NSScreen display-link factories require the main thread. */
static CADisplayLink* createAppleDisplayLink(CoralDisplayLinkTarget* target, void* view) {
  __block CADisplayLink* link = nil;
  const auto create = ^{
#if defined(TARGET_OS_OSX) && TARGET_OS_OSX
    if (view != nullptr && [((__bridge id)view) isKindOfClass:[NSView class]]) {
      link = [(__bridge NSView*)view displayLinkWithTarget:target selector:@selector(tick:)];
    } else {
      NSScreen* screen = [NSScreen mainScreen];
      if (screen != nil) {
        link = [screen displayLinkWithTarget:target selector:@selector(tick:)];
      }
    }
#else
    (void)view;
    link = [CADisplayLink displayLinkWithTarget:target selector:@selector(tick:)];
#endif
  };
  if ([NSThread isMainThread]) {
    create();
  } else {
    dispatch_sync(dispatch_get_main_queue(), create);
  }
  return link;
}


namespace graphics::gpu {
namespace {

bool appleDisplayLinkAvailable() {
#if defined(TARGET_OS_OSX) && TARGET_OS_OSX
  if (@available(macOS 14.0, *)) return true;
  return false;
#else
  return true;
#endif
}


class AppleDisplayLinkSource;

} // namespace

} // namespace graphics::gpu

/**
 * The selector target. It is Objective-C because `CADisplayLink` retains a target and calls a selector;
 * a block would be cleaner but `CAMetalDisplayLink`/`CADisplayLink`'s target/selector is the API both
 * platforms share.
 */
@interface CoralDisplayLinkTarget : NSObject
- (instancetype)initWithSource:(void*)source;
- (void)tick:(CADisplayLink*)link;
@end

namespace graphics::gpu {
namespace {

class AppleDisplayLinkSource final : public VBlankSource {
public:
  static std::unique_ptr<AppleDisplayLinkSource> create(void* view) {
    if (!appleDisplayLinkAvailable()) return nullptr;

    auto source = std::unique_ptr<AppleDisplayLinkSource>(new AppleDisplayLinkSource());
    if (!source->start(view)) return nullptr;
    return source;
  }

  ~AppleDisplayLinkSource() override { stop(); }

  bool waitForVBlank() override {
    std::unique_lock<std::mutex> lock(mMutex);
    const uint64_t target = mTicks + 1;
    mCondition.wait_for(lock, std::chrono::milliseconds(100),
            [this, target] { return mTicks >= target || mStopped; });
    return mTicks >= target;
  }

  [[nodiscard]] RefreshInfo refresh() const override {
    const std::lock_guard<std::mutex> lock(mMutex);
    RefreshInfo info;
    info.valid = mTicks >= 2 && mIntervalSeconds > 0.0;
    info.hertz = info.valid ? 1.0 / mIntervalSeconds : 0.0;
    info.phaseSeconds = info.valid ? mPhaseSeconds : 0.0;
    info.variable = mVariable;
    return info;
  }

  /** A display link is a timer, not an event that has to be re-armed after a commit. */
  [[nodiscard]] bool isEventDriven() const noexcept override { return false; }

  void handleTick(CADisplayLink* link) noexcept {
    const auto now = std::chrono::steady_clock::now();
    double duration = mLastDuration;
    if (link != nil) {
      // `duration` is the display's current frame interval. It is a better estimate than the delta
      // between two callbacks, which includes the run loop's own scheduling jitter and is what used to
      // make a 120 Hz display report 70-90 Hz for the first few frames.
      const CFTimeInterval reported = link.duration;
      if (reported > 0.001 && reported < 0.25) duration = reported;
      const double range = link.targetTimestamp - link.timestamp;
      if (range > 0.001 && range < 0.25 && std::fabs(range - duration) > 0.0005) {
        // The display is running at a rate the last `duration` did not describe (ProMotion, a mode
        // change, or an adaptive range). Report the link's own target interval for this tick.
        duration = range;
      }
    }

    {
      const std::lock_guard<std::mutex> lock(mMutex);
      if (mLastTick.time_since_epoch().count() != 0) {
        const double delta = std::chrono::duration<double>(now - mLastTick).count();
        if (delta > 0.001 && delta < 0.25) {
          // Two independent signals: the link's reported duration and the observed cadence. The
          // reported one is the authoritative display rate; the observed one catches a link whose
          // duration has not caught up with a mode change yet.
          mVariable = std::fabs(delta - duration) > 0.004;
        }
      }
      mLastTick = now;
      mLastDuration = duration;
      mIntervalSeconds = duration;
      mPhaseSeconds = std::fmod(std::chrono::duration<double>(now.time_since_epoch()).count(), duration);
      ++mTicks;
    }
    mCondition.notify_all();
  }

private:
  AppleDisplayLinkSource() = default;

  bool start(void* view) {
    // AppKit's factories must run on the caller (normally the UI) thread. Create the link before
    // starting the run-loop thread so this wait cannot deadlock on a dispatch_sync to the UI thread.
    CoralDisplayLinkTarget* target = [[CoralDisplayLinkTarget alloc] initWithSource:this];
    CADisplayLink* link = createAppleDisplayLink(target, view);
    if (link == nil) return false;
    std::unique_lock<std::mutex> readyLock(mReadyMutex);
    mThread = std::thread([this, link] { run(link); });
    return mReadyCondition.wait_for(readyLock, std::chrono::seconds(2),
                   [this] { return mReady || mStartupFailed; }) &&
           mReady;
  }

  void stop() {
    if (!mThread.joinable()) return;
    CFRunLoopRef runLoop = nullptr;
    {
      std::lock_guard<std::mutex> lock(mMutex);
      runLoop = mRunLoop;
    }
    if (runLoop != nullptr) CFRunLoopStop(runLoop);
    mThread.join();
    mThread = std::thread();
  }

  void run(CADisplayLink* link) {
    @autoreleasepool {
      if (link == nil) {
        {
          std::lock_guard<std::mutex> lock(mReadyMutex);
          mStartupFailed = true;
        }
        mReadyCondition.notify_all();
        return;
      }

      [link addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSRunLoopCommonModes];
      {
        std::lock_guard<std::mutex> lock(mMutex);
        mRunLoop = CFRunLoopGetCurrent();
        CFRetain(mRunLoop);
      }
      {
        std::lock_guard<std::mutex> lock(mReadyMutex);
        mReady = true;
      }
      mReadyCondition.notify_all();

      // `CFRunLoopRun` rather than `-[NSRunLoop run]`: the latter returns early when no input source is
      // attached, and the display link is attached to the run loop rather than to its sources. The
      // explicit `CFRunLoopStop` in `stop()` is what ends this.
      CFRunLoopRun();

      [link invalidate];
      {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mRunLoop != nullptr) {
          CFRelease(mRunLoop);
          mRunLoop = nullptr;
        }
        mStopped = true;
      }
      mCondition.notify_all();
    }
  }

  mutable std::mutex mMutex;
  std::condition_variable mCondition;
  std::chrono::steady_clock::time_point mLastTick {};
  double mLastDuration = 1.0 / 60.0;
  double mIntervalSeconds = 1.0 / 60.0;
  double mPhaseSeconds = 0.0;
  bool mVariable = false;
  bool mStopped = false;
  uint64_t mTicks = 0;
  CFRunLoopRef mRunLoop = nullptr;

  std::mutex mReadyMutex;
  std::condition_variable mReadyCondition;
  bool mReady = false;
  bool mStartupFailed = false;
  std::thread mThread;
};

} // namespace

std::unique_ptr<VBlankSource> makeAppleDisplayLinkSource(void* view) {
  return AppleDisplayLinkSource::create(view);
}

} // namespace graphics::gpu

@implementation CoralDisplayLinkTarget {
  void* _source;
}

- (instancetype)initWithSource:(void*)source {
  self = [super init];
  if (self != nil) _source = source;
  return self;
}

- (void)tick:(CADisplayLink*)link {
  auto* source = static_cast<graphics::gpu::AppleDisplayLinkSource*>(_source);
  if (source != nullptr) source->handleTick(link);
}

@end
