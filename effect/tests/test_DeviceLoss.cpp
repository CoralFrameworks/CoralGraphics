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
 * @file test_DeviceLoss.cpp
 *
 * Injects one DeviceLost-style failure at the output boundary and checks that `execute()` recovers and
 * retries exactly once. The test needs the internal OutputRuntime, so only this target sees src/.
 */

#include <effect/EffectEngine.h>
#include <effect/EffectOutputNode.h>
#include <effect/TestPatternInputNode.h>

#include "effect/EffectRenderCore.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace {

class FailingOutputNode final : public coral::EffectOutputNode {
public:
  static std::shared_ptr<FailingOutputNode> create() {
    return std::shared_ptr<FailingOutputNode>(new FailingOutputNode());
  }

  bool isReady() const noexcept override { return true; }
  coral::EffectResult configure(const coral::EffectOutputConfig&) override { return coral::EffectResult::Success; }

  struct State {
    std::atomic<int> failures { 1 };
  };

private:
  class Runtime final : public coral::detail::OutputRuntime {
  public:
    explicit Runtime(std::shared_ptr<State> state) : mState(std::move(state)) {}

    bool consume(coral::detail::FrameContext&, const coral::detail::FrameView&, std::string& error) override {
      if (mState->failures.fetch_sub(1) > 0) {
        error = "injected device loss";
        return false;
      }
      return true;
    }

  private:
    std::shared_ptr<State> mState;
  };

  FailingOutputNode() : EffectOutputNode(), mState(std::make_shared<State>()) {
    setOutputRuntime(std::make_shared<Runtime>(mState));
  }

  std::shared_ptr<State> mState;
};

TEST(DeviceLoss, ContinuousModeRecoversOnTheRenderThread) {
  coral::EffectEngineConfig config;
  config.renderWidth = 16;
  config.renderHeight = 16;
  config.renderMode = coral::EffectRenderMode::Continuous;
  config.frameRate = 60.0f;

  const auto engine = coral::EffectEngine::create(config);
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(16, 16);
  const auto output = FailingOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(engine->attach(input), coral::EffectResult::Success);
  ASSERT_EQ(engine->attach(output), coral::EffectResult::Success);
  ASSERT_EQ(engine->connect(input, output), coral::EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), coral::EffectResult::Success);

  if (engine->start() == coral::EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline && engine->stats().frameCount < 2u) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_GE(engine->stats().frameCount, 2u);
  EXPECT_TRUE(engine->isRunning());
  engine->stop();
}

} // namespace

TEST(DeviceLoss, ExecuteRecoversAndRetriesOnce) {
  coral::EffectEngineConfig config;
  config.renderWidth = 16;
  config.renderHeight = 16;
  config.renderMode = coral::EffectRenderMode::Manual;

  const auto engine = coral::EffectEngine::create(config);
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(16, 16);
  const auto output = FailingOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(output, nullptr);

  ASSERT_EQ(engine->attach(input), coral::EffectResult::Success);
  ASSERT_EQ(engine->attach(output), coral::EffectResult::Success);
  ASSERT_EQ(engine->connect(input, output), coral::EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), coral::EffectResult::Success);

  if (engine->start() == coral::EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_TRUE(engine->isRunning()) << engine->diagnostics();

  // First consume fails -> DeviceLost; execute() recovers the device and retries once. The second
  // consume succeeds, so the caller sees Success and two attempted frames.
  EXPECT_EQ(engine->execute(), coral::EffectResult::Success) << engine->diagnostics();
  EXPECT_EQ(engine->stats().frameCount, 2u);
  EXPECT_EQ(engine->execute(), coral::EffectResult::Success) << engine->diagnostics();
  engine->stop();
}
