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

#include <gtest/gtest.h>

#include <chrono>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

TEST(FrameClock, ARequestCoalescesToTheEarliestDeadline) {
  graphics::gpu::ManualFrameClock clock(Clock::time_point{});
  const Clock::time_point start = clock.now();

  clock.requestFrame(start + 20ms);
  clock.requestFrame(start + 10ms);
  clock.requestFrame(start + 30ms);

  EXPECT_TRUE(clock.frameRequested());
  EXPECT_EQ(clock.deadline(), start + 10ms);
}

TEST(FrameClock, APresentedFrameConsumesTheRequest) {
  graphics::gpu::ManualFrameClock clock(Clock::time_point{});
  clock.requestFrame(clock.now() + 16ms);
  ASSERT_TRUE(clock.waitForFrame());

  clock.onFramePresented(clock.now());
  EXPECT_FALSE(clock.frameRequested());
  EXPECT_FALSE(clock.waitForFrame());
}

TEST(FrameClock, ANinthRequestAfterAPresentIsANewFrame) {
  graphics::gpu::ManualFrameClock clock(Clock::time_point{});
  clock.requestFrame(clock.now() + 16ms);
  clock.onFramePresented(clock.now());

  clock.requestFrame(clock.now() + 32ms);
  EXPECT_TRUE(clock.frameRequested());
  EXPECT_EQ(clock.deadline(), clock.now() + 32ms);
}

TEST(FrameClock, ManualTimeOnlyMovesWhenAdvanced) {
  graphics::gpu::ManualFrameClock clock(Clock::time_point{});
  const Clock::time_point start = clock.now();

  clock.advance(5ms);
  EXPECT_EQ(clock.now(), start + 5ms);
  clock.advance(11ms);
  EXPECT_EQ(clock.now(), start + 16ms);
  const double ahead = std::chrono::duration<double>(clock.expectedPresentTime() - clock.now()).count();
  EXPECT_NEAR(ahead, 1.0 / 60.0, 0.001);
}

TEST(FrameClock, ManualClockReportsNoDisplay) {
  graphics::gpu::ManualFrameClock clock(Clock::time_point{});
  clock.onFramePresented(clock.now());
  clock.onFramePresented(clock.now() + 16ms);
  EXPECT_FALSE(clock.refresh().valid);
}

TEST(FrameClock, ThePresentClockEstimatesTheRefreshFromPresents) {
  graphics::gpu::PresentFrameClock clock(Clock::time_point{});
  const Clock::time_point start = Clock::time_point{};

  clock.onFramePresented(start);
  clock.onFramePresented(start + 16ms);
  clock.onFramePresented(start + 32ms);
  clock.onFramePresented(start + 48ms);

  const graphics::gpu::RefreshInfo info = clock.refresh();
  ASSERT_TRUE(info.valid);
  EXPECT_NEAR(info.hertz, 60.0, 2.0);
  EXPECT_GE(info.phaseSeconds, 0.0);
  EXPECT_LT(info.phaseSeconds, 1.0 / 55.0);
}

TEST(FrameClock, ThePresentClockNeedsTwoPresentsForARate) {
  graphics::gpu::PresentFrameClock clock(Clock::time_point{});
  EXPECT_FALSE(clock.refresh().valid);
  clock.onFramePresented(Clock::time_point{});
  EXPECT_FALSE(clock.refresh().valid);
  clock.onFramePresented(Clock::time_point{} + 16ms);
  EXPECT_TRUE(clock.refresh().valid);
}

TEST(FrameClock, ThePresentClockSamplesAnimationAtTheNextPresentation) {
  graphics::gpu::PresentFrameClock clock(Clock::time_point{});
  const Clock::time_point start = Clock::time_point{};
  clock.onFramePresented(start);
  clock.onFramePresented(start + 16ms);

  // The next frame is prepared after the 16 ms present and shown one interval later, not "now".
  EXPECT_NEAR(std::chrono::duration<double>(clock.expectedPresentTime() - start).count(), 0.0325, 0.001);
}

TEST(FrameClock, TheManualClockStartsAtTheDeadlineMinusTheFrameCost) {
  graphics::gpu::ManualFrameClock clock(Clock::time_point{});
  clock.requestFrame(clock.now() + 100ms);
  ASSERT_TRUE(clock.waitForFrame());

  // No frame has been measured yet, so the initial 8 ms estimate is what the scheduler reserves.
  const double ahead = std::chrono::duration<double>(100ms - (clock.wakeTarget() - clock.now())).count();
  EXPECT_NEAR(ahead, 0.008, 0.0005);
}

TEST(FrameClock, TheMeasuredFrameCostMovesTheStartEarlier) {
  graphics::gpu::ManualFrameClock clock(Clock::time_point{});
  clock.onFrameStarted();
  clock.advance(4ms);
  clock.onFramePresented(clock.now());

  // 8 ms initial, 80/20 smoothing toward a 4 ms sample: 7.2 ms.
  EXPECT_NEAR(clock.frameCostSeconds(), 0.0072, 0.0005);

  clock.requestFrame(clock.now() + 100ms);
  ASSERT_TRUE(clock.waitForFrame());
  const double ahead = std::chrono::duration<double>(100ms - (clock.wakeTarget() - clock.now())).count();
  EXPECT_NEAR(ahead, 0.0072, 0.0005);
}

namespace {

class FakeVBlankSource final : public graphics::gpu::VBlankSource {
 public:
  bool waitForVBlank() override {
    ++waits;
    return true;
  }

  [[nodiscard]] graphics::gpu::RefreshInfo refresh() const override {
    graphics::gpu::RefreshInfo info;
    info.valid = true;
    info.hertz = 60.0;
    return info;
  }

  int waits = 0;
};

} // namespace

TEST(FrameClock, ThePlatformClockAlignsOnceToVBlank) {
  FakeVBlankSource vblank;
  graphics::gpu::PlatformFrameClock clock(vblank);
  clock.requestFrame(Clock::now());
  ASSERT_TRUE(clock.waitForFrame());

  EXPECT_EQ(vblank.waits, 2) << "the first wait needs two blanks to learn the phase and interval";
  EXPECT_TRUE(clock.refresh().valid);
  EXPECT_NEAR(clock.refresh().hertz, 60.0, 0.001);
}

TEST(FrameClock, ThePlatformClockDoesNotWaitForVBlankAgain) {
  FakeVBlankSource vblank;
  graphics::gpu::PlatformFrameClock clock(vblank);
  clock.requestFrame(Clock::now());
  clock.waitForFrame();
  clock.onFrameStarted();
  clock.onFramePresented(Clock::now());

  clock.requestFrame(Clock::now());
  clock.waitForFrame();
  EXPECT_EQ(vblank.waits, 2);
}

#if defined(_WIN32)
TEST(FrameClock, ThePlatformVBlankSourceExists) {
  // DXGI when an attached output is enumerable, DwmFlush otherwise; either way Windows has a tick.
  EXPECT_NE(graphics::gpu::makePlatformVBlankSource(), nullptr);
}
#endif

#if defined(__APPLE__)
TEST(FrameClock, TheAppleDisplayLinkSourceReportsTheDisplayCadence) {
  // CADisplayLink needs a screen and a run loop. A headless build machine can have neither, so no
  // source is a skip rather than a failure; a source that exists but never ticks is the failure.
  auto source = graphics::gpu::makePlatformVBlankSource();
  if (source == nullptr) GTEST_SKIP() << "no display link is available (headless machine or older OS)";

  ASSERT_TRUE(source->waitForVBlank()) << "the first display-link tick did not arrive";
  ASSERT_TRUE(source->waitForVBlank()) << "the second display-link tick did not arrive";

  const graphics::gpu::RefreshInfo info = source->refresh();
  EXPECT_TRUE(info.valid);
  EXPECT_GT(info.hertz, 1.0);
  EXPECT_LT(info.hertz, 1000.0);
  EXPECT_GE(info.phaseSeconds, 0.0);
  EXPECT_LT(info.phaseSeconds, 1.0);
}
#endif

TEST(FrameClock, ModesAreRemembered) {  graphics::gpu::ManualFrameClock manual(Clock::time_point{});
  manual.setMode(graphics::gpu::FrameMode::LowLatency);
  EXPECT_EQ(manual.mode(), graphics::gpu::FrameMode::LowLatency);

  graphics::gpu::PresentFrameClock present(Clock::time_point{});
  present.setMode(graphics::gpu::FrameMode::Continuous);
  EXPECT_EQ(present.mode(), graphics::gpu::FrameMode::Continuous);
}

} // namespace
