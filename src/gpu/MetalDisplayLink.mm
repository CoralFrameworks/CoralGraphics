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
 * @file MetalDisplayLink.mm
 *
 * `CAMetalDisplayLink` on its own run-loop thread.
 *
 * The link owns the drawable and the target presentation time; this class only carries them across the
 * Objective-C/C++ boundary and keeps the run loop alive. It does not render, and it does not touch the
 * `WindowSurface`; the callback is where the renderer lives, which is what makes "the display link
 * thread is the render thread" a rule with no exceptions to remember.
 */

#include <gpu/MetalDisplayLink.h>

#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

@interface CoralMetalDisplayLinkDelegate : NSObject <CAMetalDisplayLinkDelegate>
- (instancetype)initWithContext:(void*)context deliver:(void (*)(void*, void*))deliver;
- (void)metalDisplayLink:(CAMetalDisplayLink*)link needsUpdate:(CAMetalDisplayLinkUpdate*)update;
@end

namespace graphics::gpu {
namespace {

/** @return true when the running OS has the CAMetalDisplayLink API. */
bool metalDisplayLinkAvailable() {
  if (@available(macOS 14.0, iOS 17.0, tvOS 17.0, *)) {
    return true;
  }
  return false;
}

} // namespace

struct MetalDisplayLink::Impl {
  void* metalLayer = nullptr;
  std::thread thread;

  mutable std::mutex mutex;
  std::condition_variable condition;
  Callback callback;
  std::atomic<bool> running { false };
  bool ready = false;
  bool failed = false;

  // The run loop the callback thread owns. `CFRunLoopStop` is the documented way to end it from
  // another thread, and it is why `stop` does not have to wait for a callback to finish first.
  CFRunLoopRef runLoop = nullptr;
  CAMetalDisplayLink* link = nil;

  double lastTargetPresentation = 0.0;
  double intervalSeconds = 1.0 / 60.0;
  uint64_t updates = 0;

  float minimumFPS = 0.0f;
  float maximumFPS = 0.0f;
  float preferredFPS = 0.0f;
  bool rangeSet = false;
  float frameLatency = 2.0f;

  void handleUpdate(CAMetalDisplayLinkUpdate* update) {
    if (update == nil) return;

    MetalDisplayLinkUpdate delivered;
    delivered.drawable = (__bridge void*)update.drawable;
    delivered.timestamp = update.targetTimestamp;
    delivered.targetTimestamp = update.targetTimestamp;
    delivered.targetPresentationTimestamp = update.targetPresentationTimestamp;

    Callback callbackCopy;
    {
      const std::lock_guard<std::mutex> lock(mutex);
      if (update.targetPresentationTimestamp > 0.0) {
        if (lastTargetPresentation > 0.0) {
          const double delta = update.targetPresentationTimestamp - lastTargetPresentation;
          if (delta > 0.001 && delta < 0.25) intervalSeconds = delta;
        }
        lastTargetPresentation = update.targetPresentationTimestamp;
      }
      ++updates;
      callbackCopy = callback;
    }

    // Outside the lock: a callback may call `refresh()` or hand work to another thread, and holding
    // the mutex through arbitrary user code would make a display-link deadlock look like a renderer
    // one. The copy is deliberate for the same reason -- `stop` may clear the callback concurrently.
    if (callbackCopy) callbackCopy(delivered);
  }
};

MetalDisplayLink::MetalDisplayLink() : mImpl(std::make_unique<Impl>()) {}

MetalDisplayLink::~MetalDisplayLink() { stop(); }

std::unique_ptr<MetalDisplayLink> MetalDisplayLink::make(void* metalLayer) {
  if (metalLayer == nullptr || !metalDisplayLinkAvailable()) return nullptr;
  id object = (__bridge id)metalLayer;
  if (![object isKindOfClass:[CAMetalLayer class]]) return nullptr;

  auto link = std::unique_ptr<MetalDisplayLink>(new MetalDisplayLink());
  link->mImpl->metalLayer = metalLayer;
  return link;
}

bool MetalDisplayLink::start(Callback callback) {
  if (mImpl == nullptr || mImpl->running.load(std::memory_order_acquire)) return false;
  if (mImpl->thread.joinable()) return false;
  if (!callback) return false;

  {
    const std::lock_guard<std::mutex> lock(mImpl->mutex);
    mImpl->callback = std::move(callback);
    mImpl->ready = false;
    mImpl->failed = false;
  }

  mImpl->running.store(true, std::memory_order_release);
  mImpl->thread = std::thread([this] {
    @autoreleasepool {
      CoralMetalDisplayLinkDelegate* delegate =
              [[CoralMetalDisplayLinkDelegate alloc] initWithContext:mImpl.get()
                                                             deliver:&MetalDisplayLink::deliverUpdate];
      CAMetalDisplayLink* link =
              [[CAMetalDisplayLink alloc] initWithMetalLayer:(__bridge CAMetalLayer*)mImpl->metalLayer];
      if (link == nil) {
        const std::lock_guard<std::mutex> lock(mImpl->mutex);
        mImpl->failed = true;
        mImpl->running.store(false, std::memory_order_release);
        mImpl->condition.notify_all();
        return;
      }

      link.delegate = delegate;
      link.preferredFrameLatency = mImpl->frameLatency;
      if (mImpl->rangeSet) {
        CAFrameRateRange range = CAFrameRateRangeMake(mImpl->minimumFPS, mImpl->maximumFPS,
                mImpl->preferredFPS);
        link.preferredFrameRateRange = range;
      }
      [link addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSRunLoopCommonModes];

      {
        const std::lock_guard<std::mutex> lock(mImpl->mutex);
        mImpl->link = link;
        mImpl->runLoop = CFRunLoopGetCurrent();
        CFRetain(mImpl->runLoop);
        mImpl->ready = true;
      }
      mImpl->condition.notify_all();

      CFRunLoopRun();

      [link invalidate];
      link.delegate = nil;
      {
        const std::lock_guard<std::mutex> lock(mImpl->mutex);
        mImpl->link = nil;
        if (mImpl->runLoop != nullptr) {
          CFRelease(mImpl->runLoop);
          mImpl->runLoop = nullptr;
        }
        mImpl->callback = {};
      }
      mImpl->running.store(false, std::memory_order_release);
      mImpl->condition.notify_all();
    }
  });

  std::unique_lock<std::mutex> lock(mImpl->mutex);
  mImpl->condition.wait_for(lock, std::chrono::seconds(2),
          [this] { return mImpl->ready || mImpl->failed; });
  if (!mImpl->ready) {
    lock.unlock();
    stop();
    return false;
  }
  return true;
}

void MetalDisplayLink::deliverUpdate(void* context, void* nativeUpdate) {
  auto* impl = static_cast<Impl*>(context);
  if (impl != nullptr) impl->handleUpdate((__bridge CAMetalDisplayLinkUpdate*)nativeUpdate);
}

void MetalDisplayLink::stop() {
  if (mImpl == nullptr || !mImpl->thread.joinable()) return;

  CFRunLoopRef runLoop = nullptr;
  {
    const std::lock_guard<std::mutex> lock(mImpl->mutex);
    runLoop = mImpl->runLoop;
  }
  if (runLoop != nullptr) CFRunLoopStop(runLoop);
  {
    const std::lock_guard<std::mutex> lock(mImpl->mutex);
    mImpl->callback = {};
  }
  mImpl->thread.join();
  mImpl->thread = std::thread();
  mImpl->running.store(false, std::memory_order_release);
}

bool MetalDisplayLink::isRunning() const noexcept {
  return mImpl != nullptr && mImpl->running.load(std::memory_order_acquire);
}

void MetalDisplayLink::setPreferredFrameRate(const float minimum, const float maximum,
        const float preferred) noexcept {
  if (mImpl == nullptr) return;
  const std::lock_guard<std::mutex> lock(mImpl->mutex);
  mImpl->minimumFPS = std::max(0.0f, minimum);
  mImpl->maximumFPS = std::max(0.0f, maximum);
  mImpl->preferredFPS = std::max(0.0f, preferred);
  mImpl->rangeSet = true;
}

void MetalDisplayLink::setPreferredFrameLatency(const float frames) noexcept {
  if (mImpl == nullptr) return;
  const std::lock_guard<std::mutex> lock(mImpl->mutex);
  mImpl->frameLatency = std::max(0.0f, frames);
}

RefreshInfo MetalDisplayLink::refresh() const noexcept {
  if (mImpl == nullptr) return {};
  const std::lock_guard<std::mutex> lock(mImpl->mutex);
  RefreshInfo info;
  info.valid = mImpl->updates >= 2 && mImpl->intervalSeconds > 0.0;
  info.hertz = info.valid ? 1.0 / mImpl->intervalSeconds : 0.0;
  info.variable = mImpl->rangeSet &&
          (mImpl->minimumFPS > 0.0f || mImpl->maximumFPS > 0.0f) &&
          mImpl->minimumFPS != mImpl->maximumFPS;
  return info;
}

} // namespace graphics::gpu

/**
 * The delegate. `CAMetalDisplayLink` holds it weakly, so `start` keeps the only strong reference for
 * the lifetime of the run loop; `stop` drops it after the link is invalidated.
 */
@implementation CoralMetalDisplayLinkDelegate {
  void* _context;
  void (*_deliver)(void*, void*);
}

- (instancetype)initWithContext:(void*)context deliver:(void (*)(void*, void*))deliver {
  self = [super init];
  if (self != nil) {
    _context = context;
    _deliver = deliver;
  }
  return self;
}

- (void)metalDisplayLink:(CAMetalDisplayLink*)link needsUpdate:(CAMetalDisplayLinkUpdate*)update {
  (void)link;
  if (_deliver != nullptr) _deliver(_context, (__bridge void*)update);
}

@end
