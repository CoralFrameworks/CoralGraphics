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
 * @file test_EffectGraph.cpp
 *
 * The graph's structural contract: attach/detach, one edge per input port, cycle refusal, fan-out, a
 * single active output, and the shader/parameter snapshot rules that a node applies before the render
 * thread ever sees them.
 *
 * None of these cases needs a device. That is deliberate: the validator is the part of the engine that
 * decides whether a graph *can* run, and it should be testable on a machine with no graphics stack.
 */

#include <effect/EffectEngine.h>
#include <effect/EffectInputNode.h>
#include <effect/EffectNode.h>
#include <effect/EffectOutputNode.h>
#include <effect/EffectPassNode.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace {

using coral::EffectEngine;
using coral::EffectEngineConfig;
using coral::EffectInputNode;
using coral::EffectNode;
using coral::EffectOutputConfig;
using coral::EffectOutputNode;
using coral::EffectPassNode;
using coral::EffectRenderMode;
using coral::EffectResult;
using coral::EffectShaderDesc;

/** An input node with no frames: the graph half of the engine does not need pixels. */
class TestInput final : public EffectInputNode {
public:
  TestInput() = default;

  [[nodiscard]] bool hasFrame() const noexcept override { return false; }
  [[nodiscard]] bool repeatLastFrame() const noexcept override { return true; }
  void setRepeatLastFrame(const bool repeat) override { mRepeat = repeat; }

private:
  bool mRepeat = true;
};

/** An output node that accepts everything and presents nothing. */
class TestOutput final : public EffectOutputNode {
public:
  TestOutput() = default;

  [[nodiscard]] bool isReady() const noexcept override { return true; }
  EffectResult configure(const EffectOutputConfig& config) override {
    mConfig = config;
    return EffectResult::Success;
  }

  [[nodiscard]] const EffectOutputConfig& config() const noexcept { return mConfig; }

private:
  EffectOutputConfig mConfig;
};

EffectEngineConfig testConfig() {
  EffectEngineConfig config;
  config.renderWidth = 64;
  config.renderHeight = 64;
  config.renderMode = EffectRenderMode::Manual;
  config.timeBase = 60;
  return config;
}

/** @return an engine, or null; every case skips rather than failing when no engine can come up. */
std::shared_ptr<EffectEngine> makeEngine() { return EffectEngine::create(testConfig()); }

EffectShaderDesc bodyShader(const char* body = "  return texture(uInput0, uv);", const uint32_t inputCount = 1) {
  EffectShaderDesc shader;
  shader.name = "test";
  shader.fragmentSource = body;
  shader.inputCount = inputCount;
  return shader;
}

TEST(EffectGraph, AttachRejectsNullAndDuplicateNodes) {
  const auto engine = makeEngine();
  if (engine == nullptr) GTEST_SKIP() << "no effect engine could be created";

  EXPECT_EQ(engine->attach(nullptr), EffectResult::InvalidArgument);

  const auto input = std::make_shared<TestInput>();
  EXPECT_EQ(engine->attach(input), EffectResult::Success);
  EXPECT_TRUE(input->isAttached());
  EXPECT_NE(input->id(), 0u);

  // The second attach is refused and must not disturb the first binding.
  EXPECT_EQ(engine->attach(input), EffectResult::InvalidArgument);
  EXPECT_TRUE(input->isAttached());
}

TEST(EffectGraph, ConnectValidatesPortsKindsAndCycles) {
  const auto engine = makeEngine();
  if (engine == nullptr) GTEST_SKIP() << "no effect engine could be created";

  const auto input = std::make_shared<TestInput>();
  const auto passA = engine->createEffectNode("a");
  const auto passB = engine->createEffectNode("b");
  const auto output = std::make_shared<TestOutput>();
  ASSERT_NE(passA, nullptr);
  ASSERT_NE(passB, nullptr);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);

  // An input has no input port; an output produces no frame to connect.
  EXPECT_EQ(engine->connect(input, passA, 1), EffectResult::InvalidArgument);
  EXPECT_EQ(engine->connect(output, passA), EffectResult::InvalidArgument);
  EXPECT_EQ(engine->connect(input, input), EffectResult::InvalidArgument);

  // A pass has exactly one port until its shader says otherwise.
  EXPECT_EQ(engine->connect(input, passA, 0), EffectResult::Success);
  EXPECT_EQ(engine->connect(input, passA, 1), EffectResult::InvalidArgument);

  // a -> b -> a would close a cycle.
  EXPECT_EQ(engine->connect(passA, passB), EffectResult::Success);
  EXPECT_EQ(engine->connect(passB, passA), EffectResult::InvalidArgument);

  EXPECT_EQ(engine->connect(passB, output), EffectResult::Success);
}

TEST(EffectGraph, ReconnectingAPortReplacesItsEdge) {
  const auto engine = makeEngine();
  if (engine == nullptr) GTEST_SKIP() << "no effect engine could be created";

  const auto first = std::make_shared<TestInput>();
  const auto second = std::make_shared<TestInput>();
  ASSERT_EQ(engine->attach(first), EffectResult::Success);
  ASSERT_EQ(engine->attach(second), EffectResult::Success);
  const auto pass = engine->createEffectNode("pass");
  ASSERT_NE(pass, nullptr);

  EXPECT_EQ(engine->connect(first, pass), EffectResult::Success);
  EXPECT_EQ(engine->connect(second, pass), EffectResult::Success);
  // The first edge no longer exists, because a port has exactly one source.
  EXPECT_EQ(engine->disconnect(first, pass), EffectResult::NotFound);
  EXPECT_EQ(engine->disconnect(second, pass), EffectResult::Success);
  EXPECT_EQ(engine->disconnect(second, pass), EffectResult::NotFound);
}

TEST(EffectGraph, FanOutIsAllowedByTheValidator) {
  const auto engine = makeEngine();
  if (engine == nullptr) GTEST_SKIP() << "no effect engine could be created";

  const auto input = std::make_shared<TestInput>();
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  const auto left = engine->createEffectNode("left");
  const auto right = engine->createEffectNode("right");
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);

  // DAG structure allows one output to feed two consumers; the v1 executor rejects this at start(),
  // not connect(), which is what lets the data structure be final before the executor is.
  EXPECT_EQ(engine->connect(input, left), EffectResult::Success);
  EXPECT_EQ(engine->connect(input, right), EffectResult::Success);
}

TEST(EffectGraph, SetOutputRequiresAnOutputNode) {
  const auto engine = makeEngine();
  if (engine == nullptr) GTEST_SKIP() << "no effect engine could be created";

  const auto pass = engine->createEffectNode("pass");
  ASSERT_NE(pass, nullptr);
  EXPECT_EQ(engine->setOutput(nullptr), EffectResult::InvalidArgument);

  // An output that has not been attached has no id the graph could name.
  const auto unattachedOutput = std::make_shared<TestOutput>();
  EXPECT_EQ(engine->setOutput(unattachedOutput), EffectResult::NotAttached);

  const auto output = std::make_shared<TestOutput>();
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  EXPECT_EQ(engine->setOutput(output), EffectResult::Success);
}

TEST(EffectGraph, DetachClearsEdgesAndTheOutput) {
  const auto engine = makeEngine();
  if (engine == nullptr) GTEST_SKIP() << "no effect engine could be created";

  const auto input = std::make_shared<TestInput>();
  const auto output = std::make_shared<TestOutput>();
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, output), EffectResult::Success);

  EXPECT_EQ(engine->detach(output), EffectResult::Success);
  EXPECT_FALSE(output->isAttached());
  EXPECT_EQ(output->id(), 0u);
  EXPECT_EQ(engine->detach(output), EffectResult::NotAttached);

  // The edge into the removed output is gone with it, so it can be detached and the input re-attached
  // without a stale port pointing at a node that no longer exists.
  EXPECT_EQ(engine->detach(input), EffectResult::Success);
}

TEST(EffectGraph, AShaderMayNotOrphanAnExistingConnection) {
  const auto engine = makeEngine();
  if (engine == nullptr) GTEST_SKIP() << "no effect engine could be created";

  const auto input = std::make_shared<TestInput>();
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  const auto pass = engine->createEffectNode("pass");
  ASSERT_NE(pass, nullptr);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);

  // Port 0 is occupied, so a shader with zero inputs would leave that edge unrepresentable.
  EXPECT_EQ(pass->setShader(bodyShader("  return vec4(1.0);", 0)), EffectResult::InvalidArgument);
  EXPECT_EQ(pass->inputCount(), 1u);

  // Growing is fine: the new ports are simply unconnected for now.
  EXPECT_EQ(pass->setShader(bodyShader("  return texture(uInput0, uv);", 2)), EffectResult::Success);
  EXPECT_EQ(pass->inputCount(), 2u);
  EXPECT_EQ(pass->shaderState(), coral::EffectShaderState::Pending);
}

TEST(EffectGraph, ParametersAreTypeCheckedAgainstTheDeclaration) {
  const auto engine = makeEngine();
  if (engine == nullptr) GTEST_SKIP() << "no effect engine could be created";

  const auto pass = engine->createEffectNode("graded");
  ASSERT_NE(pass, nullptr);

  EffectShaderDesc shader = bodyShader();
  shader.parameters = {
    { "strength", coral::EffectValueType::Float },
    { "tint", coral::EffectValueType::Color },
  };
  ASSERT_EQ(pass->setShader(shader), EffectResult::Success);

  EXPECT_EQ(pass->setParameter("strength", 0.5f), EffectResult::Success);
  EXPECT_EQ(pass->setParameter("tint", coral::EffectColor { 1.0f, 0.5f, 0.25f, 1.0f }), EffectResult::Success);

  // A colour where the shader reads a float, and a name the shader never declared.
  EXPECT_EQ(pass->setParameter("strength", coral::EffectColor { 1.0f, 1.0f, 1.0f, 1.0f }),
            EffectResult::InvalidArgument);
  EXPECT_EQ(pass->setParameter("missing", 1.0f), EffectResult::InvalidArgument);
  EXPECT_FALSE(pass->diagnostics().empty());
}

TEST(EffectGraph, DuplicateParameterNamesAreRefused) {
  const auto engine = makeEngine();
  if (engine == nullptr) GTEST_SKIP() << "no effect engine could be created";

  const auto pass = engine->createEffectNode("duplicate");
  ASSERT_NE(pass, nullptr);

  EffectShaderDesc shader = bodyShader();
  shader.parameters = {
    { "amount", coral::EffectValueType::Float },
    { "amount", coral::EffectValueType::Vec2 },
  };
  EXPECT_EQ(pass->setShader(shader), EffectResult::InvalidArgument);
  EXPECT_EQ(pass->shaderState(), coral::EffectShaderState::Empty);
}

TEST(EffectGraph, MoreParametersThanTheBlockHoldsAreRefused) {
  const auto engine = makeEngine();
  if (engine == nullptr) GTEST_SKIP() << "no effect engine could be created";

  const auto pass = engine->createEffectNode("crowded");
  ASSERT_NE(pass, nullptr);

  EffectShaderDesc shader = bodyShader();
  // 14 effect slots: three matrices already need twelve, and a fourth would need sixteen.
  for (int index = 0; index < 4; ++index) {
    shader.parameters.push_back({ "matrix" + std::to_string(index), coral::EffectValueType::Matrix4 });
  }
  EXPECT_EQ(pass->setShader(shader), EffectResult::InvalidArgument);
  EXPECT_FALSE(pass->diagnostics().empty());
  EXPECT_EQ(pass->shaderState(), coral::EffectShaderState::Empty);
}

TEST(EffectGraph, ANodesHandleOutlivesItsEngine) {
  auto engine = makeEngine();
  if (engine == nullptr) GTEST_SKIP() << "no effect engine could be created";

  const auto pass = engine->createEffectNode("survivor");
  ASSERT_NE(pass, nullptr);
  ASSERT_TRUE(pass->isAttached());

  engine.reset();

  // The node is still a valid object; it simply has no engine and no runtime.
  EXPECT_FALSE(pass->isAttached());
  EXPECT_EQ(pass->id(), 0u);
  EXPECT_EQ(pass->setParameter("anything", 1.0f), EffectResult::NotAttached);
  EXPECT_EQ(pass->setEnabled(false), EffectResult::NotAttached);
}

TEST(EffectGraph, NamesAndStatsSurviveAttachment) {
  const auto engine = makeEngine();
  if (engine == nullptr) GTEST_SKIP() << "no effect engine could be created";

  const auto pass = engine->createEffectNode("named");
  ASSERT_NE(pass, nullptr);
  EXPECT_EQ(std::string(pass->name()), "named");
  EXPECT_EQ(pass->setName("renamed"), EffectResult::Success);
  EXPECT_EQ(std::string(pass->name()), "renamed");
  EXPECT_EQ(engine->stats().nodeCount, 1u);
}

} // namespace anonymous
