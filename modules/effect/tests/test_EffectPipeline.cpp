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
 * @file test_EffectPipeline.cpp
 *
 * End-to-end frames: a deterministic input, a pass, and the readback output. These cases are the M2/M3
 * acceptance tests - the device is brought up on the render thread, a known shader draws into a pooled
 * frame, and the pixels come back as the same picture.
 *
 * Every case skips when no device can be brought up, which is the convention the GPU half of this suite
 * has always followed: a machine without a graphics stack should report "not covered here", not "broken".
 */

#include <effect/EffectEngine.h>
#include <backend/ShaderCompiler.h>
#include <effect/EffectPassNode.h>
#include <effect/Fence.h>
#include <effect/ImageInputNode.h>
#include <effect/NullOutputNode.h>
#include <effect/ReadbackOutputNode.h>
#include <effect/TestPatternInputNode.h>
#include <effect/VideoFrameInputNode.h>
#include "effect/EffectRenderCore.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using coral::EffectEngine;
using coral::EffectEngineConfig;
using coral::EffectEngineStats;
using coral::EffectRenderMode;
using coral::EffectResult;
using coral::EffectShaderDesc;
using coral::EffectShaderState;

constexpr uint32_t kSize = 64;
constexpr uint8_t kTolerance = 2;

EffectEngineConfig testConfig() {
  EffectEngineConfig config;
  config.renderWidth = kSize;
  config.renderHeight = kSize;
  config.renderMode = EffectRenderMode::Manual;
  config.timeBase = 60;
  config.maxFramesInFlight = 2;
  config.framePoolBudgetBytes = 64u * 1024u * 1024u;
  return config;
}

EffectShaderDesc bodyShader(const char* const body, const uint32_t inputCount = 1) {
  EffectShaderDesc shader;
  shader.name = "test";
  shader.fragmentSource = body;
  shader.inputCount = inputCount;
  return shader;
}

/** @return the RGBA of pixel (@p x, @p y) in a top-down readback. */
void pixelAt(const std::vector<uint8_t>& pixels, const uint32_t x, const uint32_t y, uint8_t out[4]) {
  const size_t index = (static_cast<size_t>(y) * kSize + x) * 4;
  for (size_t channel = 0; channel < 4; ++channel) out[channel] = pixels[index + channel];
}

/** A complete input -> pass -> readback graph. */
struct Graph {
  std::shared_ptr<EffectEngine> engine;
  std::shared_ptr<coral::TestPatternInputNode> input;
  std::shared_ptr<coral::EffectPassNode> pass;
  std::shared_ptr<coral::ReadbackOutputNode> output;
};

Graph makeGraph(const char* const body, const bool usePattern = true) {
  Graph graph;
  graph.engine = EffectEngine::create(testConfig());
  if (graph.engine == nullptr) return graph;

  if (usePattern) {
    graph.input = coral::TestPatternInputNode::create(kSize, kSize);
    if (graph.input == nullptr) return Graph{};
    if (graph.engine->attach(graph.input) != EffectResult::Success) return Graph{};
  }

  graph.pass = graph.engine->createEffectNode("pass");
  if (graph.pass == nullptr) return Graph{};
  if (graph.pass->setShader(bodyShader(body)) != EffectResult::Success) return Graph{};

  graph.output = coral::ReadbackOutputNode::create();
  if (graph.output == nullptr) return Graph{};
  if (graph.engine->attach(graph.output) != EffectResult::Success) return Graph{};

  if (usePattern && graph.engine->connect(graph.input, graph.pass) != EffectResult::Success) return Graph{};
  if (graph.engine->connect(graph.pass, graph.output) != EffectResult::Success) return Graph{};
  if (graph.engine->setOutput(graph.output) != EffectResult::Success) return Graph{};
  return graph;
}

/**
 * Brings @p graph up, or skips the calling test.
 *
 * A macro rather than a function because `GTEST_SKIP()` returns from the *calling* function, and a test
 * helper that returned a bool could not express a skip - it would have to report "false" and every case
 * would repeat the diagnosis.
 */
#define START_OR_SKIP(graph)                                                                  \
  do {                                                                                        \
    if ((graph).engine == nullptr) {                                                          \
      GTEST_SKIP() << "the effect engine could not be created";                               \
    }                                                                                         \
    const EffectResult started_ = (graph).engine->start();                                    \
    if (started_ == EffectResult::DeviceLost) {                                               \
      GTEST_SKIP() << "no GPU device could be brought up: " << (graph).engine->diagnostics(); \
    }                                                                                         \
    ASSERT_EQ(started_, EffectResult::Success) << (graph).engine->diagnostics();              \
  } while (false)

TEST(EffectPipeline, AConstantColourPassReachesTheReadback) {
  Graph graph = makeGraph("  return vec4(1.0, 0.0, 0.0, 1.0);");
  START_OR_SKIP(graph);

  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  coral::EffectFrame frame;
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(graph.output->waitForFrame(&frame, &pixels)) << graph.engine->diagnostics();
  ASSERT_EQ(pixels.size(), static_cast<size_t>(kSize) * kSize * 4);
  EXPECT_TRUE(frame.isValid());
  EXPECT_EQ(frame.width(), kSize);
  EXPECT_EQ(frame.height(), kSize);

  for (const uint32_t x : { 0u, 17u, 63u }) {
    uint8_t rgba[4] = {};
    pixelAt(pixels, x, 33, rgba);
    EXPECT_NEAR(rgba[0], 255, kTolerance);
    EXPECT_NEAR(rgba[1], 0, kTolerance);
    EXPECT_NEAR(rgba[2], 0, kTolerance);
    EXPECT_NEAR(rgba[3], 255, kTolerance);
  }
  EXPECT_EQ(graph.engine->stats().frameCount, 1u);
  graph.engine->stop();
}

TEST(EffectPipeline, AnIdentityPassReproducesThePattern) {
  Graph graph = makeGraph("  return texture(uInput0, uv);");
  START_OR_SKIP(graph);

  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(graph.output->waitForFrame(nullptr, &pixels));
  ASSERT_EQ(pixels.size(), static_cast<size_t>(kSize) * kSize * 4);

  // Eight vertical bars: white, yellow, cyan, green, magenta, red, blue, black. Their vertical position
  // is not asserted here, so the case tests the chain rather than a particular origin convention.
  struct Expected {
    uint32_t x;
    uint8_t r, g, b;
  };
  const Expected expected[8] = {
    { 4, 255, 255, 255 }, { 12, 255, 255, 0 }, { 20, 0, 255, 255 }, { 28, 0, 255, 0 },
    { 36, 255, 0, 255 },  { 44, 255, 0, 0 },   { 52, 0, 0, 255 },   { 60, 0, 0, 0 },
  };
  for (const Expected& bar : expected) {
    uint8_t rgba[4] = {};
    pixelAt(pixels, bar.x, 33, rgba);
    EXPECT_NEAR(rgba[0], bar.r, kTolerance) << "bar at x=" << bar.x;
    EXPECT_NEAR(rgba[1], bar.g, kTolerance) << "bar at x=" << bar.x;
    EXPECT_NEAR(rgba[2], bar.b, kTolerance) << "bar at x=" << bar.x;
    EXPECT_NEAR(rgba[3], 255, kTolerance) << "bar at x=" << bar.x;
  }
  graph.engine->stop();
}

TEST(EffectPipeline, TheShaderCompilesOnTheRenderThread) {
  Graph graph = makeGraph("  return texture(uInput0, uv);");
  START_OR_SKIP(graph);

  EXPECT_EQ(graph.pass->shaderState(), EffectShaderState::Pending);
  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  EXPECT_EQ(graph.pass->shaderState(), EffectShaderState::Ready);
  EXPECT_TRUE(graph.pass->diagnostics().empty()) << graph.pass->diagnostics();
  EXPECT_GE(graph.engine->stats().programCount, 1u);
  graph.engine->stop();
}

TEST(EffectPipeline, StartRequiresAnOutputAndACompleteChain) {
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(engine, nullptr);

  // No output at all.
  EXPECT_EQ(engine->start(), EffectResult::InvalidArgument);

  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto output = coral::NullOutputNode::create();
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);

  // An output with no incoming edge is not a chain.
  EXPECT_EQ(engine->start(), EffectResult::InvalidArgument);
  ASSERT_EQ(engine->connect(input, output), EffectResult::Success);
  EXPECT_EQ(engine->start(), EffectResult::Success);
  EXPECT_TRUE(engine->isRunning());
  EXPECT_EQ(engine->stop(), EffectResult::Success);
  EXPECT_FALSE(engine->isRunning());
}

TEST(EffectPipeline, FanOutExecutesAsADag) {
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(engine, nullptr);

  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto split = engine->createEffectNode("split");
  const auto left = engine->createEffectNode("left");
  const auto right = engine->createEffectNode("right");
  const auto join = engine->createEffectNode("join");
  const auto output = coral::NullOutputNode::create();
  ASSERT_NE(split, nullptr);
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  ASSERT_NE(join, nullptr);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);

  EffectShaderDesc one = bodyShader("  return texture(uInput0, uv);", 1);
  EffectShaderDesc two = bodyShader("  return texture(uInput0, uv) + texture(uInput1, uv);", 2);
  two.name = "join";
  ASSERT_EQ(split->setShader(one), EffectResult::Success);
  ASSERT_EQ(left->setShader(one), EffectResult::Success);
  ASSERT_EQ(right->setShader(one), EffectResult::Success);
  ASSERT_EQ(join->setShader(two), EffectResult::Success);

  // A true fan-out: `split` feeds two branches, which rejoin at `join`, so every node reaches the output.
  ASSERT_EQ(engine->connect(input, split), EffectResult::Success);
  ASSERT_EQ(engine->connect(split, left), EffectResult::Success);
  ASSERT_EQ(engine->connect(split, right), EffectResult::Success);
  ASSERT_EQ(engine->connect(left, join, 0), EffectResult::Success);
  ASSERT_EQ(engine->connect(right, join, 1), EffectResult::Success);
  ASSERT_EQ(engine->connect(join, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);

  EXPECT_EQ(engine->start(), EffectResult::Success) << engine->diagnostics();
  EXPECT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  engine->stop();
}

TEST(EffectPipeline, TwoPassesRunInOrder) {
  Graph graph = makeGraph("  return vec4(texture(uInput0, uv).g, 0.0, 0.0, 1.0);");
  START_OR_SKIP(graph);

  const auto second = graph.engine->createEffectNode("second");
  ASSERT_NE(second, nullptr);
  EffectShaderDesc invert = bodyShader("  return vec4(1.0 - texture(uInput0, uv).r, 0.0, 0.0, 1.0);");
  invert.name = "invert";
  ASSERT_EQ(second->setShader(invert), EffectResult::Success);

  // Insert the second pass between the first and the output.
  ASSERT_EQ(graph.engine->disconnect(graph.pass, graph.output), EffectResult::Success);
  ASSERT_EQ(graph.engine->connect(graph.pass, second), EffectResult::Success);
  ASSERT_EQ(graph.engine->connect(second, graph.output), EffectResult::Success);

  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(graph.output->waitForFrame(nullptr, &pixels));

  // The first pass copies green into red; the second inverts red. White's green is 255 -> 255 -> 0,
  // black's is 0 -> 0 -> 255. Reading the two ends therefore proves both passes ran, in order.
  uint8_t white[4] = {};
  pixelAt(pixels, 4, 33, white);
  EXPECT_NEAR(white[0], 0, kTolerance);
  uint8_t black[4] = {};
  pixelAt(pixels, 60, 33, black);
  EXPECT_NEAR(black[0], 255, kTolerance);
  EXPECT_GE(graph.engine->stats().programCount, 2u);
  graph.engine->stop();
}

TEST(EffectPipeline, ThePoolDoesNotGrowOnceItIsWarm) {
  Graph graph = makeGraph("  return texture(uInput0, uv);");
  START_OR_SKIP(graph);

  for (int frame = 0; frame < 3; ++frame) {
    ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
    ASSERT_TRUE(graph.output->waitForFrame(nullptr, nullptr));
  }
  const EffectEngineStats warm = graph.engine->stats();
  for (int frame = 0; frame < 5; ++frame) {
    ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
    ASSERT_TRUE(graph.output->waitForFrame(nullptr, nullptr));
  }
  const EffectEngineStats steady = graph.engine->stats();

  EXPECT_EQ(steady.frameCount, 8u);
  EXPECT_EQ(steady.framePoolMisses, warm.framePoolMisses);
  EXPECT_GE(steady.framePoolHits, warm.framePoolHits);
  // Readback completion is asynchronous; the last submission may still be retired on the next frame.
  graph.engine->stop();
}

TEST(EffectPipeline, PauseAndResumeRoundTrip) {
  Graph graph = makeGraph("  return texture(uInput0, uv);");
  START_OR_SKIP(graph);

  ASSERT_EQ(graph.engine->pause(), EffectResult::Success);
  EXPECT_TRUE(graph.engine->isPaused());
  ASSERT_EQ(graph.engine->resume(), EffectResult::Success);
  EXPECT_FALSE(graph.engine->isPaused());

  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  EXPECT_TRUE(graph.output->waitForFrame(nullptr, nullptr));
  EXPECT_EQ(graph.engine->flush(), EffectResult::Success);
  EXPECT_EQ(graph.engine->flushAndWait(), EffectResult::Success);
  graph.engine->stop();
}

TEST(EffectPipeline, ANullOutputConsumesFrames) {
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto output = coral::NullOutputNode::create();
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);

  if (engine->start() == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(engine->start(), EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  EXPECT_EQ(engine->stats().frameCount, 1u);
  engine->stop();
}

TEST(EffectPipeline, ADynamicShaderTakesEffectOnTheNextFrame) {
  Graph graph = makeGraph("  return vec4(1.0, 0.0, 0.0, 1.0);");
  START_OR_SKIP(graph);

  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(graph.output->waitForFrame(nullptr, &pixels));
  uint8_t rgba[4] = {};
  pixelAt(pixels, 20, 20, rgba);
  EXPECT_NEAR(rgba[0], 255, kTolerance);

  EffectShaderDesc blue = bodyShader("  return vec4(0.0, 0.0, 1.0, 1.0);");
  blue.name = "blue";
  ASSERT_EQ(graph.pass->setShader(blue), EffectResult::Success);
  EXPECT_EQ(graph.pass->shaderState(), EffectShaderState::Pending);

  const uint32_t programsBefore = graph.engine->stats().programCount;
  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  ASSERT_TRUE(graph.output->waitForFrame(nullptr, &pixels));
  EXPECT_EQ(graph.pass->shaderState(), EffectShaderState::Ready) << graph.pass->diagnostics();
  pixelAt(pixels, 20, 20, rgba);
  EXPECT_NEAR(rgba[2], 255, kTolerance);
  EXPECT_NEAR(rgba[0], 0, kTolerance);
  EXPECT_GE(graph.engine->stats().programCount, programsBefore);
  graph.engine->stop();
}

TEST(EffectPipeline, AFailedShaderKeepsThePreviousPicture) {
  Graph graph = makeGraph("  return vec4(1.0, 0.0, 0.0, 1.0);");
  START_OR_SKIP(graph);

  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  ASSERT_TRUE(graph.output->waitForFrame(nullptr, nullptr));

  EffectShaderDesc broken = bodyShader("  this is not GLSL;");
  broken.name = "broken";
  ASSERT_EQ(graph.pass->setShader(broken), EffectResult::Success);

  // The compile fails on the render thread; the frame still renders with the previous program, which is
  // the rule that keeps a failed effect from blanking the picture.
  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(graph.output->waitForFrame(nullptr, &pixels));
  EXPECT_EQ(graph.pass->shaderState(), EffectShaderState::Failed);
  EXPECT_FALSE(graph.pass->diagnostics().empty());
  uint8_t rgba[4] = {};
  pixelAt(pixels, 20, 20, rgba);
  EXPECT_NEAR(rgba[0], 255, kTolerance);
  graph.engine->stop();
}

TEST(EffectPipeline, ParameterUpdatesDoNotRecompileTheShader) {
  EffectShaderDesc shader = bodyShader(R"(
  float amount = clamp(effectParams(0).x, 0.0, 1.0);
  return mix(vec4(1.0, 0.0, 0.0, 1.0), vec4(0.0, 1.0, 0.0, 1.0), amount);
)");
  shader.parameters = { { "amount", coral::EffectValueType::Float } };

  Graph graph;
  graph.engine = EffectEngine::create(testConfig());
  if (graph.engine == nullptr) GTEST_SKIP() << "the effect engine could not be created";
  graph.input = coral::TestPatternInputNode::create(kSize, kSize);
  graph.pass = graph.engine->createEffectNode("mix");
  graph.output = coral::ReadbackOutputNode::create();
  if (graph.input == nullptr || graph.pass == nullptr || graph.output == nullptr) {
    GTEST_SKIP() << "the graph could not be built";
  }
  ASSERT_EQ(graph.engine->attach(graph.input), EffectResult::Success);
  ASSERT_EQ(graph.engine->attach(graph.output), EffectResult::Success);
  ASSERT_EQ(graph.engine->connect(graph.input, graph.pass), EffectResult::Success);
  ASSERT_EQ(graph.engine->connect(graph.pass, graph.output), EffectResult::Success);
  ASSERT_EQ(graph.engine->setOutput(graph.output), EffectResult::Success);
  ASSERT_EQ(graph.pass->setShader(shader), EffectResult::Success);
  ASSERT_EQ(graph.pass->setParameter("amount", 0.0f), EffectResult::Success);
  START_OR_SKIP(graph);

  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(graph.output->waitForFrame(nullptr, &pixels));
  uint8_t rgba[4] = {};
  pixelAt(pixels, 20, 20, rgba);
  EXPECT_NEAR(rgba[0], 255, kTolerance);

  const uint32_t programs = graph.engine->stats().programCount;
  ASSERT_EQ(graph.pass->setParameter("amount", 1.0f), EffectResult::Success);
  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  ASSERT_TRUE(graph.output->waitForFrame(nullptr, &pixels));
  pixelAt(pixels, 20, 20, rgba);
  EXPECT_NEAR(rgba[1], 255, kTolerance);
  EXPECT_NEAR(rgba[0], 0, kTolerance);
  // A parameter lives in the block, not in the program: the cache entry is the same one.
  EXPECT_EQ(graph.engine->stats().programCount, programs);
  graph.engine->stop();
}

TEST(EffectPipeline, AMultiInputPassReadsItsVec4Parameter) {
  EffectShaderDesc shader = bodyShader(R"(
  return vec4(effectParams(0).z, effectParams(0).w, effectParams(1).x, 1.0);
)");
  shader.inputCount = 2;
  shader.parameters = {
    { "uCrop", coral::EffectValueType::Vec4 },
    { "uOpacity", coral::EffectValueType::Float },
  };

  Graph graph;
  graph.engine = EffectEngine::create(testConfig());
  if (graph.engine == nullptr) GTEST_SKIP() << "the effect engine could not be created";
  graph.input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto secondInput = coral::TestPatternInputNode::create(kSize, kSize);
  graph.pass = graph.engine->createEffectNode("two-input-mix");
  graph.output = coral::ReadbackOutputNode::create();
  if (graph.input == nullptr || secondInput == nullptr || graph.pass == nullptr || graph.output == nullptr) {
    GTEST_SKIP() << "the graph could not be built";
  }
  ASSERT_EQ(graph.engine->attach(graph.input), EffectResult::Success);
  ASSERT_EQ(graph.engine->attach(secondInput), EffectResult::Success);
  ASSERT_EQ(graph.engine->attach(graph.output), EffectResult::Success);
  ASSERT_EQ(graph.pass->setShader(shader), EffectResult::Success);
  ASSERT_EQ(graph.engine->connect(graph.input, graph.pass, 0), EffectResult::Success);
  ASSERT_EQ(graph.engine->connect(secondInput, graph.pass, 1), EffectResult::Success);
  ASSERT_EQ(graph.engine->connect(graph.pass, graph.output), EffectResult::Success);
  ASSERT_EQ(graph.engine->setOutput(graph.output), EffectResult::Success);
  ASSERT_EQ(graph.pass->setParameter("uCrop", coral::EffectVec4{ 0.0f, 0.0f, 1.0f, 0.5f }),
            EffectResult::Success);
  ASSERT_EQ(graph.pass->setParameter("uOpacity", 0.25f), EffectResult::Success);
  START_OR_SKIP(graph);

  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(graph.output->waitForFrame(nullptr, &pixels));
  uint8_t rgba[4] = {};
  pixelAt(pixels, 20, 20, rgba);
  EXPECT_NEAR(rgba[0], 255, kTolerance);
  EXPECT_NEAR(rgba[1], 128, kTolerance);
  EXPECT_NEAR(rgba[2], 64, kTolerance);
  graph.engine->stop();
}

TEST(EffectPipeline, ABoundTextureIsSampledByName) {
  Graph graph = makeGraph("  return texture(uOverlay, uv);");
  if (graph.engine == nullptr || graph.pass == nullptr) GTEST_SKIP() << "the effect engine could not be created";
  ASSERT_EQ(graph.pass->setTexture("uOverlay", coral::TextureSource::solid({ 0.0f, 0.0f, 1.0f, 1.0f })),
            EffectResult::Success);
  START_OR_SKIP(graph);

  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(graph.output->waitForFrame(nullptr, &pixels)) << graph.pass->diagnostics();
  uint8_t rgba[4] = {};
  pixelAt(pixels, 20, 20, rgba);
  EXPECT_NEAR(rgba[2], 255, kTolerance);
  EXPECT_NEAR(rgba[0], 0, kTolerance);
  graph.engine->stop();
}

TEST(EffectPipeline, ASequenceShowsTheSelectedFrame) {
  Graph graph = makeGraph("  return texture(uSeq, uv);");
  if (graph.engine == nullptr || graph.pass == nullptr) GTEST_SKIP() << "the effect engine could not be created";

  const auto image = [](const uint8_t r, const uint8_t g, const uint8_t b) {
    const uint8_t pixel[4] = { r, g, b, 255 };
    return coral::EffectImage::create(coral::EffectImageDesc::rgba8(pixel, 1, 1));
  };
  coral::EffectTextureSequenceDesc sequence;
  sequence.frames = {
    coral::TextureSource::fromImage(image(255, 0, 0)),
    coral::TextureSource::fromImage(image(0, 255, 0)),
    coral::TextureSource::fromImage(image(0, 0, 255)),
  };
  sequence.frameDuration = { 2, 60 };
  sequence.loop = false;

  ASSERT_EQ(graph.pass->setTextureSequence("uSeq", sequence), EffectResult::Success);
  ASSERT_EQ(graph.pass->setSequenceFrame("uSeq", 2), EffectResult::Success);
  START_OR_SKIP(graph);

  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(graph.output->waitForFrame(nullptr, &pixels)) << graph.pass->diagnostics();
  uint8_t rgba[4] = {};
  pixelAt(pixels, 20, 20, rgba);
  EXPECT_NEAR(rgba[2], 255, kTolerance);

  // Index 0 is red; the override is what selects it, without rebuilding anything.
  ASSERT_EQ(graph.pass->setSequenceFrame("uSeq", 0), EffectResult::Success);
  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  ASSERT_TRUE(graph.output->waitForFrame(nullptr, &pixels));
  pixelAt(pixels, 20, 20, rgba);
  EXPECT_NEAR(rgba[0], 255, kTolerance);
  EXPECT_NEAR(rgba[2], 0, kTolerance);
  graph.engine->stop();
}

TEST(EffectPipeline, TwoInputsAreBothSampled) {
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(engine, nullptr);

  const auto bars = coral::TestPatternInputNode::create(kSize, kSize,
                                                       coral::TestPatternInputNode::Pattern::ColorBars);
  const auto checkers = coral::TestPatternInputNode::create(kSize, kSize,
                                                           coral::TestPatternInputNode::Pattern::Checkerboard);
  const auto pass = engine->createEffectNode("sum");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(pass, nullptr);
  ASSERT_EQ(engine->attach(bars), EffectResult::Success);
  ASSERT_EQ(engine->attach(checkers), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);

  EffectShaderDesc shader = bodyShader("  return texture(uInput0, uv) + texture(uInput1, uv);", 2);
  shader.name = "sum";
  ASSERT_EQ(pass->setShader(shader), EffectResult::Success);
  ASSERT_EQ(engine->connect(bars, pass, 0), EffectResult::Success);
  ASSERT_EQ(engine->connect(checkers, pass, 1), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();

  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
  // The black bar (x = 60) contributes 0 and the checkerboard contributes about 235; a pass that sampled
  // only the first input would read 0 here.
  uint8_t rgba[4] = {};
  pixelAt(pixels, 60, 33, rgba);
  EXPECT_GT(rgba[0], 180);
  engine->stop();
}


class StubExternalTexture final : public coral::EffectExternalTexture {
public:
  [[nodiscard]] uint64_t version() const noexcept override { return 1; }
  [[nodiscard]] void* nativeHandle() const noexcept override { return nullptr; }
  [[nodiscard]] uint32_t nativeKind() const noexcept override { return 0; }
};

TEST(EffectPipeline, ExternalTextureWithoutResolverFailsClearly) {
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("external");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(pass->setShader(bodyShader("  return texture(uOverlay, uv);")), EffectResult::Success);
  ASSERT_EQ(pass->setTexture("uOverlay", coral::TextureSource::fromExternal(
      std::make_shared<StubExternalTexture>())), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);
  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  EXPECT_NE(engine->execute(), EffectResult::Success);
  EXPECT_NE(engine->diagnostics().find("resolver"), std::string::npos) << engine->diagnostics();
  engine->stop();
}
TEST(EffectPipeline, PassCanChooseOutputTimestampFromSecondInput) {
  constexpr uint32_t kWidth = 4;
  constexpr uint32_t kHeight = 4;
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(engine, nullptr);
  const auto first = coral::VideoFrameInputNode::create(kWidth, kHeight);
  const auto second = coral::VideoFrameInputNode::create(kWidth, kHeight);
  const auto pass = engine->createEffectNode("timestamps");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);

  std::vector<uint8_t> red(static_cast<size_t>(kWidth) * kHeight * 4, 255);
  std::vector<uint8_t> blue(static_cast<size_t>(kWidth) * kHeight * 4, 255);
  for (uint32_t i = 0; i < kWidth * kHeight; ++i) {
    red[i * 4 + 0] = 255; red[i * 4 + 1] = 0; red[i * 4 + 2] = 0;
    blue[i * 4 + 0] = 0; blue[i * 4 + 1] = 0; blue[i * 4 + 2] = 255;
  }
  coral::VideoFrameDesc firstFrame;
  firstFrame.width = kWidth;
  firstFrame.height = kHeight;
  firstFrame.format = coral::EffectPixelFormat::RGBA8;
  firstFrame.planes[0] = { red.data(), kWidth * 4 };
  firstFrame.planeCount = 1;
  firstFrame.timestamp = coral::RenderTime::fromTicks(10, 60);
  coral::VideoFrameDesc secondFrame = firstFrame;
  secondFrame.planes[0] = { blue.data(), kWidth * 4 };
  secondFrame.timestamp = coral::RenderTime::fromTicks(20, 60);
  ASSERT_EQ(first->submit(firstFrame), EffectResult::Success);
  ASSERT_EQ(second->submit(secondFrame), EffectResult::Success);

  ASSERT_EQ(pass->setShader(bodyShader("  return texture(uInput0, uv);", 2)), EffectResult::Success);
  ASSERT_EQ(engine->attach(first), EffectResult::Success);
  ASSERT_EQ(engine->attach(second), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(first, pass, 0), EffectResult::Success);
  ASSERT_EQ(engine->connect(second, pass, 1), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);
  ASSERT_EQ(pass->setTimestampSource(1), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  coral::EffectFrame frame;
  ASSERT_TRUE(output->waitForFrame(&frame, nullptr));
  EXPECT_EQ(frame.timestamp().value, 20);
  EXPECT_EQ(frame.timestamp().timescale, 60u);
  engine->stop();
}
TEST(EffectPipeline, ConcurrentSettersDoNotRaceTheRenderThread) {
  EffectShaderDesc shader = bodyShader(R"(
  float amount = clamp(effectParams(0).x, 0.0, 1.0);
  return mix(vec4(1.0, 0.0, 0.0, 1.0), vec4(0.0, 1.0, 0.0, 1.0), amount);
)");
  shader.parameters = { { "amount", coral::EffectValueType::Float } };

  Graph graph;
  graph.engine = EffectEngine::create(testConfig());
  if (graph.engine == nullptr) GTEST_SKIP() << "the effect engine could not be created";
  graph.input = coral::TestPatternInputNode::create(kSize, kSize);
  graph.pass = graph.engine->createEffectNode("animated");
  graph.output = coral::ReadbackOutputNode::create();
  if (graph.input == nullptr || graph.pass == nullptr || graph.output == nullptr) {
    GTEST_SKIP() << "the graph could not be built";
  }
  ASSERT_EQ(graph.engine->attach(graph.input), EffectResult::Success);
  ASSERT_EQ(graph.engine->attach(graph.output), EffectResult::Success);
  ASSERT_EQ(graph.engine->connect(graph.input, graph.pass), EffectResult::Success);
  ASSERT_EQ(graph.engine->connect(graph.pass, graph.output), EffectResult::Success);
  ASSERT_EQ(graph.engine->setOutput(graph.output), EffectResult::Success);
  ASSERT_EQ(graph.pass->setShader(shader), EffectResult::Success);
  START_OR_SKIP(graph);

  std::atomic<bool> stop { false };
  std::thread setter([&graph, &stop] {
    uint32_t value = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      graph.pass->setParameter("amount", static_cast<float>(value % 2));
      ++value;
    }
  });

  for (int frame = 0; frame < 10; ++frame) {
    ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
    ASSERT_TRUE(graph.output->waitForFrame(nullptr, nullptr));
    // The render thread must observe only whole snapshots; the value it picks is one of the two writes.
    EXPECT_EQ(graph.engine->stats().frameCount, static_cast<uint64_t>(frame + 1));
  }
  stop.store(true, std::memory_order_relaxed);
  setter.join();
  graph.engine->stop();
}


TEST(EffectPipeline, ASequenceCanBeDrivenByTime) {
  Graph graph = makeGraph("  return texture(uSeq, uv);");
  if (graph.engine == nullptr || graph.pass == nullptr) GTEST_SKIP() << "the effect engine could not be created";

  const auto image = [](const uint8_t r, const uint8_t g, const uint8_t b) {
    const uint8_t pixel[4] = { r, g, b, 255 };
    return coral::EffectImage::create(coral::EffectImageDesc::rgba8(pixel, 1, 1));
  };
  coral::EffectTextureSequenceDesc sequence;
  sequence.frames = {
    coral::TextureSource::fromImage(image(255, 0, 0)),
    coral::TextureSource::fromImage(image(0, 255, 0)),
    coral::TextureSource::fromImage(image(0, 0, 255)),
  };
  sequence.frameDuration = { 2, 60 };
  sequence.loop = false;
  ASSERT_EQ(graph.pass->setTextureSequence("uSeq", sequence), EffectResult::Success);
  START_OR_SKIP(graph);
  std::fprintf(stderr, "sequence state=%d diag=%s\\n", static_cast<int>(graph.pass->shaderState()), graph.pass->diagnostics().c_str());

  const auto expectChannel = [&](const coral::RenderTime& time, const size_t channel) {
    ASSERT_EQ(graph.pass->setSequenceTime("uSeq", time), EffectResult::Success);
    ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
    std::vector<uint8_t> pixels;
    ASSERT_TRUE(graph.output->waitForFrame(nullptr, &pixels));
    uint8_t rgba[4] = {};
    pixelAt(pixels, 20, 20, rgba);
    EXPECT_NEAR(rgba[channel], 255, kTolerance) << "time " << time.value << "/" << time.timescale;
    // Exactly one channel is lit, so a wrong frame cannot pass by having two of them high.
    for (size_t other = 0; other < 3; ++other) {
      if (other != channel) EXPECT_NEAR(rgba[other], 0, kTolerance);
    }
  };

  expectChannel({ 0, 60 }, 0); // frame 0: [0, 2)
  expectChannel({ 1, 60 }, 0); // still frame 0
  expectChannel({ 2, 60 }, 1); // frame 1: [2, 4)
  expectChannel({ 4, 60 }, 2); // frame 2: [4, 6)
  expectChannel({ 99, 60 }, 2); // past the end, loop off: hold the last frame
  graph.engine->stop();
}


TEST(EffectPipeline, AnImageInputUploadsEachImageOnce) {
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(engine, nullptr);

  const uint8_t redPixel[4] = { 255, 0, 0, 255 };
  const uint8_t greenPixel[4] = { 0, 255, 0, 255 };
  const auto input = coral::ImageInputNode::create(1, 1);
  const auto pass = engine->createEffectNode("copy");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(input->setImage(coral::EffectImage::create(coral::EffectImageDesc::rgba8(redPixel, 1, 1))),
            EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(pass->setShader(bodyShader("  return texture(uInput0, uv);")), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
  uint8_t rgba[4] = {};
  pixelAt(pixels, 20, 20, rgba);
  EXPECT_NEAR(rgba[0], 255, kTolerance);
  const uint32_t textures = engine->stats().textureCount;

  ASSERT_EQ(input->setImage(coral::EffectImage::create(coral::EffectImageDesc::rgba8(greenPixel, 1, 1))),
            EffectResult::Success);
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
  pixelAt(pixels, 20, 20, rgba);
  EXPECT_NEAR(rgba[1], 255, kTolerance);
  EXPECT_NEAR(rgba[0], 0, kTolerance);
  // The first image stays cached; the second adds one texture, and repeated frames add nothing.
  EXPECT_EQ(engine->stats().textureCount, textures + 1u);
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  ASSERT_TRUE(output->waitForFrame(nullptr, nullptr));
  EXPECT_EQ(engine->stats().textureCount, textures + 1u);
  engine->stop();
}

TEST(EffectPipeline, AFrameFenceSignalsTheSubmission) {
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(24, 24);
  const auto pass = engine->createEffectNode("fence");
  const auto output = coral::NullOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);

  ASSERT_EQ(pass->setShader(bodyShader("  return texture(uInput0, uv);")), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();

  std::shared_ptr<coral::Fence> fence;
  ASSERT_EQ(engine->execute(&fence), EffectResult::Success) << engine->diagnostics();
  ASSERT_NE(fence, nullptr);
  EXPECT_TRUE(fence->isValid());
  EXPECT_EQ(fence->wait(coral::Fence::Mode::kDontFlush, 5'000'000'000ull),
            coral::Fence::Status::kConditionSatisfied);

  engine->stop();
  // The frame already completed, so the handle is no longer pending; a second wait still reports the
  // stored result instead of touching the stopped engine.
  EXPECT_FALSE(fence->isValid());
  EXPECT_EQ(fence->wait(coral::Fence::Mode::kDontFlush, 0), coral::Fence::Status::kConditionSatisfied);
}

TEST(EffectPipeline, AStridedVideoFrameReachesReadbackWithItsTimestamp) {
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(engine, nullptr);
  const auto input = coral::VideoFrameInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("video");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);

  // A padded row proves the node honours rowBytes rather than assuming width * 4.
  const uint32_t rowBytes = kSize * 4 + 8;
  std::vector<uint8_t> rows(static_cast<size_t>(rowBytes) * kSize, 0);
  for (uint32_t y = 0; y < kSize; ++y) {
    for (uint32_t x = 0; x < kSize; ++x) {
      uint8_t* pixel = rows.data() + static_cast<size_t>(y) * rowBytes + x * 4;
      pixel[0] = 0;
      pixel[1] = 255;
      pixel[2] = 0;
      pixel[3] = 255;
    }
  }
  coral::VideoFrameDesc frame;
  frame.width = kSize;
  frame.height = kSize;
  frame.format = coral::EffectPixelFormat::RGBA8;
  frame.planes[0] = { rows.data(), rowBytes };
  frame.planeCount = 1;
  frame.timestamp = coral::RenderTime::fromTicks(42, 60);
  ASSERT_EQ(input->submit(frame), EffectResult::Success);

  ASSERT_EQ(pass->setShader(bodyShader("  return texture(uInput0, uv);")), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();

  coral::EffectFrame delivered;
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(&delivered, &pixels));
  ASSERT_TRUE(delivered.isValid());
  EXPECT_EQ(delivered.timestamp().value, 42);
  EXPECT_EQ(delivered.timestamp().timescale, 60u);
  uint8_t rgba[4] = {};
  pixelAt(pixels, 20, 20, rgba);
  EXPECT_NEAR(rgba[0], 0, kTolerance);
  EXPECT_NEAR(rgba[1], 255, kTolerance);
  EXPECT_NEAR(rgba[2], 0, kTolerance);
  EXPECT_NEAR(rgba[3], 255, kTolerance);
  engine->stop();
}

TEST(EffectPipeline, ABgraVideoFrameIsConvertedBeforeUpload) {
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(engine, nullptr);
  const auto input = coral::VideoFrameInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("bgra");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);

  std::vector<uint8_t> pixels(static_cast<size_t>(kSize) * kSize * 4, 0);
  for (size_t index = 0; index < pixels.size(); index += 4) {
    pixels[index + 0] = 0;   // B
    pixels[index + 1] = 255; // G
    pixels[index + 2] = 0;   // R
    pixels[index + 3] = 255;
  }
  coral::VideoFrameDesc frame;
  frame.width = kSize;
  frame.height = kSize;
  frame.format = coral::EffectPixelFormat::BGRA8;
  frame.planes[0] = { pixels.data(), kSize * 4 };
  frame.planeCount = 1;
  ASSERT_EQ(input->submit(frame), EffectResult::Success);
  ASSERT_EQ(pass->setShader(bodyShader("  return texture(uInput0, uv);")), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  std::vector<uint8_t> readback;
  ASSERT_TRUE(output->waitForFrame(nullptr, &readback));
  uint8_t rgba[4] = {};
  pixelAt(readback, 20, 20, rgba);
  EXPECT_NEAR(rgba[0], 0, kTolerance);
  EXPECT_NEAR(rgba[1], 255, kTolerance);
  EXPECT_NEAR(rgba[2], 0, kTolerance);
  EXPECT_NEAR(rgba[3], 255, kTolerance);
  engine->stop();
}
TEST(EffectPipeline, AYuv420FrameIsConvertedBeforeUpload) {
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(engine, nullptr);
  const auto input = coral::VideoFrameInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("yuv");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);

  std::vector<uint8_t> yPlane(kSize * kSize, 235); // BT.601 limited-range white
  std::vector<uint8_t> uPlane((kSize / 2) * (kSize / 2), 128);
  std::vector<uint8_t> vPlane((kSize / 2) * (kSize / 2), 128);
  coral::VideoFrameDesc frame;
  frame.width = kSize;
  frame.height = kSize;
  frame.format = coral::EffectPixelFormat::YUV420;
  frame.planeCount = 3;
  frame.planes[0] = { yPlane.data(), kSize };
  frame.planes[1] = { uPlane.data(), kSize / 2 };
  frame.planes[2] = { vPlane.data(), kSize / 2 };
  ASSERT_EQ(input->submit(frame), EffectResult::Success);

  ASSERT_EQ(pass->setShader(bodyShader("  return texture(uInput0, uv);")), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
  uint8_t rgba[4] = {};
  pixelAt(pixels, 20, 20, rgba);
  EXPECT_NEAR(rgba[0], 255, kTolerance);
  EXPECT_NEAR(rgba[1], 255, kTolerance);
  EXPECT_NEAR(rgba[2], 255, kTolerance);
  engine->stop();
}

TEST(EffectPipeline, AYuv420PixelBufferViewIsConvertedBeforeUpload) {
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(engine, nullptr);
  const auto input = coral::VideoFrameInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("yuv-view");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);

  std::vector<uint8_t> yPlane(kSize * kSize, 235);
  std::vector<uint8_t> uPlane((kSize / 2) * (kSize / 2), 128);
  std::vector<uint8_t> vPlane((kSize / 2) * (kSize / 2), 128);
  coral::PixelBufferView view;
  view.width = kSize;
  view.height = kSize;
  view.format = coral::EffectPixelFormat::YUV420;
  view.storage = coral::PixelBufferStorageKind::Cpu;
  view.capabilities = coral::PixelBufferCapability::CpuRead;
  view.planeCount = 3;
  view.planes[0] = { yPlane.data(), kSize, kSize, kSize };
  view.planes[1] = { uPlane.data(), kSize / 2, kSize / 2, kSize / 2 };
  view.planes[2] = { vPlane.data(), kSize / 2, kSize / 2, kSize / 2 };
  ASSERT_EQ(input->submit(view), EffectResult::Success);

  ASSERT_EQ(pass->setShader(bodyShader("  return texture(uInput0, uv);")), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
  uint8_t rgba[4] = {};
  pixelAt(pixels, 20, 20, rgba);
  EXPECT_NEAR(rgba[0], 255, kTolerance);
  EXPECT_NEAR(rgba[1], 255, kTolerance);
  EXPECT_NEAR(rgba[2], 255, kTolerance);
  engine->stop();
}

TEST(EffectPipeline, AVideoFrameQueueReturnsBusyInBlockMode) {
  EffectEngineConfig config = testConfig();
  config.backpressureMode = coral::EffectBackpressureMode::Block;
  const auto engine = EffectEngine::create(config);
  ASSERT_NE(engine, nullptr);
  const auto input = coral::VideoFrameInputNode::create(kSize, kSize, 1);
  const auto pass = engine->createEffectNode("queue");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);

  std::vector<uint8_t> rows(static_cast<size_t>(kSize) * kSize * 4, 0);
  const auto makeFrame = [&](const uint8_t red, const uint8_t green) {
    for (uint32_t i = 0; i < kSize * kSize; ++i) {
      rows[i * 4 + 0] = red;
      rows[i * 4 + 1] = green;
      rows[i * 4 + 2] = 0;
      rows[i * 4 + 3] = 255;
    }
    coral::VideoFrameDesc frame;
    frame.width = kSize;
    frame.height = kSize;
    frame.format = coral::EffectPixelFormat::RGBA8;
    frame.planes[0] = { rows.data(), kSize * 4 };
    frame.planeCount = 1;
    return frame;
  };

  ASSERT_EQ(input->submit(makeFrame(255, 0)), EffectResult::Success);
  ASSERT_EQ(pass->setShader(bodyShader("  return texture(uInput0, uv);")), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));

  // The executor has now told the input that the engine is in Block mode. One queued frame is allowed;
  // the second submit must report Busy instead of silently dropping.
  ASSERT_EQ(input->submit(makeFrame(0, 255)), EffectResult::Success);
  ASSERT_EQ(input->submit(makeFrame(0, 0)), EffectResult::Busy);
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
  uint8_t rgba[4] = {};
  pixelAt(pixels, 20, 20, rgba);
  EXPECT_NEAR(rgba[0], 0, kTolerance);
  EXPECT_NEAR(rgba[1], 255, kTolerance);
  engine->stop();
}

/**
 * A tick with nothing submitted is a drop, and it must not cost the frame ring a slot.
 *
 * The executor begins recording *before* it asks the input for a frame, so an input that has not
 * produced yet aborts a recording. On Vulkan that abort used to leave the command buffer flagged as
 * recording - `VulkanCommandBuffer::reset` refused while recording - after which `begin()` failed for
 * that buffer forever. Two such ticks, one per slot, made every later frame report "no command buffer
 * could be acquired": a preview that goes black and never recovers.
 */
TEST(EffectPipeline, AnInputWithoutAFrameIsBusyAndKeepsTheFrameRingUsable) {
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(engine, nullptr);
  const auto input = coral::VideoFrameInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("empty-input");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(pass->setShader(bodyShader("  return texture(uInput0, uv);")), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();

  // More empty ticks than the engine has command-buffer slots: every one is a drop, none is an error.
  for (uint32_t attempt = 0; attempt < 4; ++attempt) {
    EXPECT_EQ(engine->execute(), EffectResult::Busy) << "tick " << attempt << ": " << engine->diagnostics();
  }

  // And the ring is still usable: a real frame submitted afterwards renders.
  std::vector<uint8_t> rows(static_cast<size_t>(kSize) * kSize * 4, 0);
  for (uint32_t index = 0; index < kSize * kSize; ++index) {
    rows[index * 4 + 1] = 255;
    rows[index * 4 + 3] = 255;
  }
  coral::VideoFrameDesc frame;
  frame.width = kSize;
  frame.height = kSize;
  frame.format = coral::EffectPixelFormat::RGBA8;
  frame.planes[0] = { rows.data(), kSize * 4 };
  frame.planeCount = 1;
  ASSERT_EQ(input->submit(frame), EffectResult::Success);
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
  uint8_t rgba[4] = {};
  pixelAt(pixels, 20, 20, rgba);
  EXPECT_NEAR(rgba[1], 255, kTolerance);
  engine->stop();
}

TEST(EffectPipeline, AReadbackRotationSwapsTheFrameDimensions) {
  constexpr uint32_t kWidth = 16;
  constexpr uint32_t kHeight = 8;
  EffectEngineConfig config = testConfig();
  config.renderWidth = kWidth;
  config.renderHeight = kHeight;
  const auto engine = EffectEngine::create(config);
  ASSERT_NE(engine, nullptr);
  const auto input = coral::VideoFrameInputNode::create(kWidth, kHeight);
  const auto pass = engine->createEffectNode("rotate");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);

  std::vector<uint8_t> rows(static_cast<size_t>(kWidth) * kHeight * 4, 0);
  for (uint32_t i = 0; i < kWidth * kHeight; ++i) {
    rows[i * 4 + 1] = 255;
    rows[i * 4 + 3] = 255;
  }
  coral::VideoFrameDesc frame;
  frame.width = kWidth;
  frame.height = kHeight;
  frame.format = coral::EffectPixelFormat::RGBA8;
  frame.planes[0] = { rows.data(), kWidth * 4 };
  frame.planeCount = 1;
  ASSERT_EQ(input->submit(frame), EffectResult::Success);

  coral::EffectOutputConfig outputConfig;
  outputConfig.rotation = coral::EffectRotation::Clockwise90;
  ASSERT_EQ(output->configure(outputConfig), EffectResult::Success);
  ASSERT_EQ(pass->setShader(bodyShader("  return texture(uInput0, uv);")), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  coral::EffectFrame delivered;
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(&delivered, &pixels));
  EXPECT_EQ(pixels.size(), static_cast<size_t>(kWidth) * kHeight * 4);
  EXPECT_EQ(delivered.width(), kHeight);
  EXPECT_EQ(delivered.height(), kWidth);
  const size_t centre = (static_cast<size_t>(2) * delivered.width() + 2) * 4;
  ASSERT_LT(centre + 3, pixels.size());
  EXPECT_NEAR(pixels[centre + 1], 255, kTolerance);
  engine->stop();
}

TEST(EffectPipeline, RecoverRecreatesTheDeviceAndKeepsTheGraph) {
  Graph graph = makeGraph("  return vec4(0.0, 1.0, 0.0, 1.0);");
  START_OR_SKIP(graph);

  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(graph.output->waitForFrame(nullptr, &pixels));

  ASSERT_EQ(graph.engine->recover(), EffectResult::Success) << graph.engine->diagnostics();
  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  ASSERT_TRUE(graph.output->waitForFrame(nullptr, &pixels));
  uint8_t rgba[4] = {};
  pixelAt(pixels, 20, 20, rgba);
  EXPECT_NEAR(rgba[1], 255, kTolerance);
  graph.engine->stop();
}

TEST(EffectPipeline, ADisabledInputProducesNoFrame) {
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto output = coral::NullOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);
  ASSERT_EQ(input->setEnabled(false), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  EXPECT_EQ(engine->execute(), EffectResult::Busy);
  engine->stop();
}

TEST(EffectPipeline, ImageTexturesAreEvictedAfterTheyLeaveTheSubmissionRing) {
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(engine, nullptr);
  const auto input = coral::ImageInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("images");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(pass->setShader(bodyShader("  return texture(uInput0, uv);")), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  for (uint32_t frame = 0; frame < 10; ++frame) {
    const uint8_t pixel[4] = { static_cast<uint8_t>(frame * 20), 0, 0, 255 };
    ASSERT_EQ(input->setImage(coral::EffectImage::create(coral::EffectImageDesc::rgba8(pixel, 1, 1))),
              EffectResult::Success);
    ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
    ASSERT_TRUE(output->waitForFrame(nullptr, nullptr));
  }
  // Four-frame retention + the current image; ten distinct images must not all stay cached.
  EXPECT_LE(engine->stats().textureCount, 6u);
  EXPECT_GT(engine->stats().imageTextureEvictions, 0u);
  engine->stop();
}

TEST(EffectPipeline, InactiveProgramsAreTrimmedAtTheConfiguredCap) {
  EffectEngineConfig config = testConfig();
  config.maxCachedPrograms = 4;
  const auto engine = EffectEngine::create(config);
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("shader-churn");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  for (uint32_t frame = 0; frame < 8; ++frame) {
    const std::string source = "  return vec4(" + std::to_string(frame) + ".0 / 8.0, 0.2, 0.3, 1.0);";
    ASSERT_EQ(pass->setShader(bodyShader(source.c_str())), EffectResult::Success);
    ASSERT_EQ(engine->execute(), EffectResult::Success)
        << "frame " << frame << " programs " << engine->stats().programCount << " " << engine->diagnostics();
    ASSERT_TRUE(output->waitForFrame(nullptr, nullptr));
  }
  EXPECT_LE(engine->stats().programCount, 5u);
  EXPECT_GT(engine->stats().programEvictions, 0u);
  engine->stop();
}

TEST(EffectPipeline, SolidTexturesAreEvictedAfterTheyLeaveTheSubmissionRing) {
  Graph graph = makeGraph("  return texture(uOverlay, uv);");
  START_OR_SKIP(graph);
  for (uint32_t frame = 0; frame < 10; ++frame) {
    const float value = static_cast<float>(frame) / 10.0f;
    ASSERT_EQ(graph.pass->setTexture("uOverlay", coral::TextureSource::solid({ value, 0.0f, 0.0f, 1.0f })),
              EffectResult::Success);
    ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
    ASSERT_TRUE(graph.output->waitForFrame(nullptr, nullptr));
  }
  // Four-frame retention + the current solid; ten distinct colours must not all stay cached.
  EXPECT_LE(graph.engine->stats().textureCount, 6u);
  EXPECT_GT(graph.engine->stats().solidTextureEvictions, 0u);
  graph.engine->stop();
}

#if defined(_WIN32)
TEST(EffectPipeline, ShaderCAddsMissingVertexOutputLocation) {
  Graph graph = makeGraph("  return texture(uInput0, uv);");
  EffectShaderDesc shader;
  shader.name = "custom-vertex";
  shader.fragmentSource = "  return vec4(0.0, 1.0, 0.0, 1.0);";
  shader.vertexSource = R"(#version 300 es
precision highp float;
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;
out vec2 vUV;
void main() {
  vUV = aTexCoord;
  gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";
  shader.inputCount = 1;
  ASSERT_EQ(graph.pass->setShader(shader), EffectResult::Success);
  START_OR_SKIP(graph);
  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(graph.output->waitForFrame(nullptr, &pixels));
  uint8_t rgba[4] = {};
  pixelAt(pixels, kSize / 2, kSize / 2, rgba);
  EXPECT_NEAR(rgba[1], 255, kTolerance);
  graph.engine->stop();
}
TEST(EffectPipeline, ShaderCRejectsConflictingVaryingLocations) {
  Graph graph = makeGraph("  return vec4(0.0, 1.0, 0.0, 1.0);");
  EffectShaderDesc shader;
  shader.name = "conflicting-vertex";
  shader.fragmentSource = "  return vec4(0.0, 1.0, 0.0, 1.0);";
  shader.vertexSource = R"(#version 300 es
precision highp float;
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;
layout(location = 1) out vec2 vUV;
void main() {
  vUV = aTexCoord;
  gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";
  shader.inputCount = 1;
  ASSERT_EQ(graph.pass->setShader(shader), EffectResult::Success);
  START_OR_SKIP(graph);
  // The executor passes an input through when a pass has no compiled program, so the frame itself can
  // still succeed; the shader state is where the interface error is reported.
  EXPECT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  EXPECT_EQ(graph.pass->shaderState(), EffectShaderState::Failed);
  EXPECT_NE(graph.pass->diagnostics().find("shader interface"), std::string::npos)
      << graph.pass->diagnostics();
  graph.engine->stop();
}

#endif

TEST(EffectPipeline, ShaderInterfaceRejectsMissingVaryingProducer) {
  Graph graph = makeGraph("  return vec4(0.0, 1.0, 0.0, 1.0);");
  EffectShaderDesc shader;
  shader.name = "missing-varying";
  shader.fragmentSource = "  return vec4(0.0, 1.0, 0.0, 1.0);";
  shader.vertexSource = R"(#version 410 core
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;
out vec2 other;
void main() {
  other = aTexCoord;
  gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";
  shader.inputCount = 1;
  ASSERT_EQ(graph.pass->setShader(shader), EffectResult::Success);
  START_OR_SKIP(graph);
  EXPECT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  EXPECT_EQ(graph.pass->shaderState(), EffectShaderState::Failed);
  EXPECT_NE(graph.pass->diagnostics().find("no matching vertex output"), std::string::npos)
      << graph.pass->diagnostics();
  graph.engine->stop();
}

TEST(EffectPipeline, DetachReleasesRuntimeAfterTheGpuIsIdle) {
  Graph graph = makeGraph("  return texture(uInput0, uv);");
  START_OR_SKIP(graph);
  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  ASSERT_TRUE(graph.output->waitForFrame(nullptr, nullptr));
  EXPECT_EQ(graph.engine->detach(graph.input), EffectResult::Success);
  EXPECT_TRUE(graph.engine->isRunning());
  graph.engine->stop();
}

TEST(EffectPipeline, ReadbackCallbacksRunOnThePollingThread) {
  Graph graph = makeGraph("  return vec4(0.0, 1.0, 0.0, 1.0);");
  START_OR_SKIP(graph);
  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  ASSERT_TRUE(graph.output->waitForFrame(nullptr, nullptr));
  std::atomic<int> callbacks { 0 };
  std::thread::id callbackThread;
  graph.output->setCallback([&](const coral::EffectFrame&, const uint8_t*, size_t) {
    callbacks.fetch_add(1, std::memory_order_relaxed);
    callbackThread = std::this_thread::get_id();
  });
  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  ASSERT_TRUE(graph.output->waitForFrame(nullptr, nullptr));
  for (int attempt = 0; attempt < 100 && callbacks.load(std::memory_order_relaxed) == 0; ++attempt) {
    graph.engine->pollCallbacks();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(callbacks.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(callbackThread, std::this_thread::get_id());
  graph.engine->stop();
}

TEST(EffectPipeline, OneGraphCanFeedTwoReadbackOutputs) {
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto first = coral::ReadbackOutputNode::create();
  const auto second = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(first), EffectResult::Success);
  ASSERT_EQ(engine->attach(second), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, first), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, second), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(first), EffectResult::Success);
  ASSERT_EQ(engine->addOutput(second), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  coral::EffectFrame firstFrame;
  coral::EffectFrame secondFrame;
  ASSERT_TRUE(first->waitForFrame(&firstFrame, nullptr));
  ASSERT_TRUE(second->waitForFrame(&secondFrame, nullptr));
  EXPECT_EQ(firstFrame.width(), kSize);
  EXPECT_EQ(secondFrame.width(), kSize);
  EXPECT_EQ(engine->removeOutput(second), EffectResult::Success);
  engine->stop();
}

TEST(EffectPipeline, ReadbackHonoursConfiguredOutputSize) {
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("scale");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(pass->setShader(bodyShader("  return vec4(0.0, 1.0, 0.0, 1.0);")), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);
  coral::EffectOutputConfig config;
  config.width = 32;
  config.height = 16;
  ASSERT_EQ(output->configure(config), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  coral::EffectFrame frame;
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(&frame, &pixels));
  EXPECT_EQ(frame.width(), 32u);
  EXPECT_EQ(frame.height(), 16u);
  EXPECT_EQ(pixels.size(), static_cast<size_t>(32) * 16 * 4);
  uint8_t rgba[4] = {};
  const size_t centre = (static_cast<size_t>(8) * 32 + 16) * 4;
  ASSERT_LT(centre + 3, pixels.size());
  std::memcpy(rgba, pixels.data() + centre, 4);
  EXPECT_NEAR(rgba[1], 255, kTolerance);
  engine->stop();
}

TEST(EffectPipeline, TwoOutputsCanUseDifferentGpuScales) {
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto half = engine->createEffectNode("half");
  const auto full = engine->createEffectNode("full");
  const auto halfOutput = coral::ReadbackOutputNode::create();
  const auto fullOutput = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(half, nullptr);
  ASSERT_NE(full, nullptr);
  ASSERT_NE(halfOutput, nullptr);
  ASSERT_NE(fullOutput, nullptr);
  const EffectShaderDesc identity = bodyShader("  return texture(uInput0, uv);");
  ASSERT_EQ(half->setShader(identity), EffectResult::Success);
  ASSERT_EQ(full->setShader(identity), EffectResult::Success);
  ASSERT_EQ(half->setOutputScale(0.5f), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(halfOutput), EffectResult::Success);
  ASSERT_EQ(engine->attach(fullOutput), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, half), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, full), EffectResult::Success);
  ASSERT_EQ(engine->connect(half, halfOutput), EffectResult::Success);
  ASSERT_EQ(engine->connect(full, fullOutput), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(halfOutput), EffectResult::Success);
  ASSERT_EQ(engine->addOutput(fullOutput), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  coral::EffectFrame halfFrame;
  coral::EffectFrame fullFrame;
  ASSERT_TRUE(halfOutput->waitForFrame(&halfFrame, nullptr));
  ASSERT_TRUE(fullOutput->waitForFrame(&fullFrame, nullptr));
  EXPECT_EQ(halfFrame.width(), kSize / 2);
  EXPECT_EQ(halfFrame.height(), kSize / 2);
  EXPECT_EQ(fullFrame.width(), kSize);
  EXPECT_EQ(fullFrame.height(), kSize);
  engine->stop();
}

namespace {

class SpyDirectOutputNode final : public coral::EffectOutputNode {
public:
  static std::shared_ptr<SpyDirectOutputNode> create() {
    return std::shared_ptr<SpyDirectOutputNode>(new SpyDirectOutputNode());
  }

  bool isReady() const noexcept override { return true; }
  coral::EffectResult configure(const coral::EffectOutputConfig& config) override {
    mConfig = config;
    return coral::EffectResult::Success;
  }

  struct State {
    std::atomic<bool> beginCalled { false };
    std::atomic<bool> acquireCalled { false };
    std::atomic<bool> consumeCalled { false };
    std::atomic<bool> presentCalled { false };
  };

  std::shared_ptr<State> state() const { return mState; }

private:
  class Runtime final : public coral::detail::OutputRuntime {
  public:
    Runtime(std::shared_ptr<State> state, coral::EffectOutputConfig config)
            : mState(std::move(state)), mConfig(config) {}

    bool supportsDirectSurface() const noexcept override { return true; }

    bool beginDirectFrame(coral::detail::FrameContext& context, uint32_t width, uint32_t height,
                          std::string& error) override {
      mState->beginCalled.store(true, std::memory_order_release);
      coral::detail::FrameKey key;
      key.width = width;
      key.height = height;
      key.format = backend::PixelFormat::R8G8B8A8_UNORM;
      key.sampleCount = 1;
      mLease = context.pool != nullptr ? context.pool->acquire(key, error) : coral::detail::FrameLease();
      return mLease.isValid();
    }

    backend::Result acquireDirectTarget(backend::SwapchainTargetInfo& info) override {
      if (!mLease.isValid()) return backend::Result::NotReady;
      mState->acquireCalled.store(true, std::memory_order_release);
      info.view = mLease.get()->view;
      info.renderPass = mLease.get()->pass;
      info.width = mLease.get()->key.width;
      info.height = mLease.get()->key.height;
      info.format = backend::PixelFormat::R8G8B8A8_UNORM;
      return backend::Result::Success;
    }

    backend::Result presentDirectTarget() override {
      mState->presentCalled.store(true, std::memory_order_release);
      mLease.reset();
      return backend::Result::Success;
    }

    bool consume(coral::detail::FrameContext&, const coral::detail::FrameView&, std::string&) override {
      mState->consumeCalled.store(true, std::memory_order_release);
      return true;
    }

  private:
    std::shared_ptr<State> mState;
    coral::EffectOutputConfig mConfig;
    coral::detail::FrameLease mLease;
  };

  SpyDirectOutputNode() : EffectOutputNode(), mState(std::make_shared<State>()) {
    setOutputRuntime(std::make_shared<Runtime>(mState, mConfig));
  }

  std::shared_ptr<State> mState;
  coral::EffectOutputConfig mConfig;
};

} // namespace
TEST(EffectPipeline, TwoOutputsCanUseDifferentFormats) {
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("copy");
  const auto rgba = coral::ReadbackOutputNode::create();
  const auto bgra = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(rgba, nullptr);
  ASSERT_NE(bgra, nullptr);
  ASSERT_EQ(pass->setShader(bodyShader("  return texture(uInput0, uv);")), EffectResult::Success);
  coral::EffectOutputConfig rgbaConfig;
  rgbaConfig.width = kSize;
  rgbaConfig.height = kSize;
  rgbaConfig.format = coral::EffectPixelFormat::RGBA8;
  coral::EffectOutputConfig bgraConfig;
  bgraConfig.width = kSize / 2;
  bgraConfig.height = kSize / 2;
  bgraConfig.format = coral::EffectPixelFormat::BGRA8;
  ASSERT_EQ(rgba->configure(rgbaConfig), EffectResult::Success);
  ASSERT_EQ(bgra->configure(bgraConfig), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(rgba), EffectResult::Success);
  ASSERT_EQ(engine->attach(bgra), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, rgba), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, bgra), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(rgba), EffectResult::Success);
  ASSERT_EQ(engine->addOutput(bgra), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  coral::EffectFrame rgbaFrame;
  coral::EffectFrame bgraFrame;
  std::vector<uint8_t> rgbaPixels;
  std::vector<uint8_t> bgraPixels;
  ASSERT_TRUE(rgba->waitForFrame(&rgbaFrame, &rgbaPixels));
  ASSERT_TRUE(bgra->waitForFrame(&bgraFrame, &bgraPixels));
  EXPECT_EQ(rgbaFrame.format(), coral::EffectPixelFormat::RGBA8);
  EXPECT_EQ(bgraFrame.format(), coral::EffectPixelFormat::BGRA8);
  EXPECT_EQ(rgbaFrame.width(), kSize);
  EXPECT_EQ(bgraFrame.width(), kSize / 2);
  EXPECT_EQ(rgbaPixels.size(), static_cast<size_t>(kSize) * kSize * 4);
  EXPECT_EQ(bgraPixels.size(), static_cast<size_t>(kSize / 2) * (kSize / 2) * 4);
  engine->stop();
}
TEST(EffectPipeline, GpuRotationKeepsQuadrantOrientation) {
  constexpr uint32_t kWidth = 16;
  constexpr uint32_t kHeight = 8;
  EffectEngineConfig config = testConfig();
  config.renderWidth = kWidth;
  config.renderHeight = kHeight;
  const auto engine = EffectEngine::create(config);
  ASSERT_NE(engine, nullptr);
  const auto input = coral::VideoFrameInputNode::create(kWidth, kHeight);
  const auto pass = engine->createEffectNode("rotate");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);

  std::vector<uint8_t> rows(static_cast<size_t>(kWidth) * kHeight * 4, 0);
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      uint8_t* pixel = rows.data() + (static_cast<size_t>(y) * kWidth + x) * 4;
      const bool left = x < kWidth / 2;
      const bool top = y < kHeight / 2;
      if (top && left) {
        pixel[0] = 255;
      } else if (top) {
        pixel[1] = 255;
      } else if (left) {
        pixel[2] = 255;
      } else {
        pixel[0] = pixel[1] = pixel[2] = 255;
      }
      pixel[3] = 255;
    }
  }
  coral::VideoFrameDesc frame;
  frame.width = kWidth;
  frame.height = kHeight;
  frame.format = coral::EffectPixelFormat::RGBA8;
  frame.planes[0] = { rows.data(), kWidth * 4 };
  frame.planeCount = 1;
  ASSERT_EQ(input->submit(frame), EffectResult::Success);
  coral::EffectOutputConfig outputConfig;
  outputConfig.rotation = coral::EffectRotation::Clockwise90;
  ASSERT_EQ(output->configure(outputConfig), EffectResult::Success);
  ASSERT_EQ(pass->setShader(bodyShader("  return texture(uInput0, uv);")), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
  const auto pixel = [&](const uint32_t x, const uint32_t y) {
    const size_t offset = (static_cast<size_t>(y) * kHeight + x) * 4;
    return &pixels[offset];
  };
  const uint8_t* topLeft = pixel(0, 0);
  const uint8_t* topRight = pixel(kHeight - 1, 0);
  const uint8_t* bottomLeft = pixel(0, kWidth - 1);
  const uint8_t* bottomRight = pixel(kHeight - 1, kWidth - 1);
  EXPECT_NEAR(topLeft[2], 255, kTolerance);      // source bottom-left is blue
  EXPECT_NEAR(topRight[0], 255, kTolerance);     // source top-left is red
  EXPECT_NEAR(bottomLeft[0], 255, kTolerance);   // source bottom-right is white
  EXPECT_NEAR(bottomLeft[1], 255, kTolerance);
  EXPECT_NEAR(bottomLeft[2], 255, kTolerance);
  std::fprintf(stderr, "quad TL %u %u %u TR %u %u %u BL %u %u %u BR %u %u %u\\n", topLeft[0], topLeft[1], topLeft[2], topRight[0], topRight[1], topRight[2], bottomLeft[0], bottomLeft[1], bottomLeft[2], bottomRight[0], bottomRight[1], bottomRight[2]);
  EXPECT_NEAR(bottomRight[1], 255, kTolerance);  // source top-right is green
  engine->stop();
}

TEST(EffectPipeline, GpuMirrorKeepsQuadrantOrientation) {
  constexpr uint32_t kWidth = 16;
  constexpr uint32_t kHeight = 8;
  EffectEngineConfig config = testConfig();
  config.renderWidth = kWidth;
  config.renderHeight = kHeight;
  const auto engine = EffectEngine::create(config);
  ASSERT_NE(engine, nullptr);
  const auto input = coral::VideoFrameInputNode::create(kWidth, kHeight);
  const auto pass = engine->createEffectNode("mirror");
  const auto horizontal = coral::ReadbackOutputNode::create();
  const auto vertical = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(horizontal, nullptr);
  ASSERT_NE(vertical, nullptr);

  std::vector<uint8_t> rows(static_cast<size_t>(kWidth) * kHeight * 4, 0);
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      uint8_t* pixel = rows.data() + (static_cast<size_t>(y) * kWidth + x) * 4;
      const bool left = x < kWidth / 2;
      const bool top = y < kHeight / 2;
      if (top && left) {
        pixel[0] = 255;
      } else if (top) {
        pixel[1] = 255;
      } else if (left) {
        pixel[2] = 255;
      } else {
        pixel[0] = pixel[1] = pixel[2] = 255;
      }
      pixel[3] = 255;
    }
  }
  coral::VideoFrameDesc frame;
  frame.width = kWidth;
  frame.height = kHeight;
  frame.format = coral::EffectPixelFormat::RGBA8;
  frame.planes[0] = { rows.data(), kWidth * 4 };
  frame.planeCount = 1;
  ASSERT_EQ(input->submit(frame), EffectResult::Success);
  coral::EffectOutputConfig horizontalConfig;
  horizontalConfig.width = kWidth;
  horizontalConfig.height = kHeight;
  horizontalConfig.mirrorHorizontal = true;
  ASSERT_EQ(horizontal->configure(horizontalConfig), EffectResult::Success);
  coral::EffectOutputConfig verticalConfig = horizontalConfig;
  verticalConfig.mirrorHorizontal = false;
  verticalConfig.mirrorVertical = true;
  ASSERT_EQ(vertical->configure(verticalConfig), EffectResult::Success);
  ASSERT_EQ(pass->setShader(bodyShader("  return texture(uInput0, uv);")), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(horizontal), EffectResult::Success);
  ASSERT_EQ(engine->attach(vertical), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, horizontal), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, vertical), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(horizontal), EffectResult::Success);
  ASSERT_EQ(engine->addOutput(vertical), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  std::vector<uint8_t> horizontalPixels;
  std::vector<uint8_t> verticalPixels;
  ASSERT_TRUE(horizontal->waitForFrame(nullptr, &horizontalPixels));
  ASSERT_TRUE(vertical->waitForFrame(nullptr, &verticalPixels));
  const auto pixel = [](const std::vector<uint8_t>& pixels, const uint32_t x, const uint32_t y) {
    const size_t offset = (static_cast<size_t>(y) * 16 + x) * 4;
    return &pixels[offset];
  };
  const uint8_t* htl = pixel(horizontalPixels, 0, 0);
  const uint8_t* htr = pixel(horizontalPixels, 15, 0);
  const uint8_t* hbl = pixel(horizontalPixels, 0, 7);
  const uint8_t* hbr = pixel(horizontalPixels, 15, 7);
  EXPECT_NEAR(htl[1], 255, kTolerance);  // source top-right green
  EXPECT_NEAR(htr[0], 255, kTolerance);  // source top-left red
  EXPECT_NEAR(hbl[0], 255, kTolerance);  // source bottom-right white
  EXPECT_NEAR(hbl[1], 255, kTolerance);
  EXPECT_NEAR(hbl[2], 255, kTolerance);
  EXPECT_NEAR(hbr[2], 255, kTolerance);  // source bottom-left blue
  const uint8_t* vtl = pixel(verticalPixels, 0, 0);
  const uint8_t* vbl = pixel(verticalPixels, 0, 7);
  EXPECT_NEAR(vtl[2], 255, kTolerance);  // source bottom-left blue
  EXPECT_NEAR(vbl[0], 255, kTolerance);  // source top-left red
  engine->stop();
}

TEST(EffectPipeline, ReadbackCanOutputBgra) {
  constexpr uint32_t kSize = 4;
  EffectEngineConfig config = testConfig();
  config.renderWidth = kSize;
  config.renderHeight = kSize;
  const auto engine = EffectEngine::create(config);
  ASSERT_NE(engine, nullptr);
  const auto input = coral::VideoFrameInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("bgra");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(pass->setShader(bodyShader("  return texture(uInput0, uv);")), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);
  coral::EffectOutputConfig outputConfig;
  outputConfig.format = coral::EffectPixelFormat::BGRA8;
  ASSERT_EQ(output->configure(outputConfig), EffectResult::Success);

  std::vector<uint8_t> rows(static_cast<size_t>(kSize) * kSize * 4, 0);
  for (uint32_t i = 0; i < kSize * kSize; ++i) {
    rows[i * 4 + 0] = 255;
    rows[i * 4 + 3] = 255;
  }
  coral::VideoFrameDesc frame;
  frame.width = kSize;
  frame.height = kSize;
  frame.format = coral::EffectPixelFormat::RGBA8;
  frame.planes[0] = { rows.data(), kSize * 4 };
  frame.planeCount = 1;
  ASSERT_EQ(input->submit(frame), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  coral::EffectFrame delivered;
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(&delivered, &pixels));
  EXPECT_EQ(delivered.format(), coral::EffectPixelFormat::BGRA8);
  ASSERT_EQ(pixels.size(), static_cast<size_t>(kSize) * kSize * 4);
  EXPECT_NEAR(pixels[0], 0, kTolerance);    // B
  EXPECT_NEAR(pixels[1], 0, kTolerance);    // G
  EXPECT_NEAR(pixels[2], 255, kTolerance);  // R
  EXPECT_NEAR(pixels[3], 255, kTolerance);  // A
  engine->stop();
}

TEST(EffectPipeline, ReadbackOutputCanBeReconfiguredWhileRunning) {
  Graph graph = makeGraph("  return vec4(0.0, 1.0, 0.0, 1.0);");
  START_OR_SKIP(graph);
  coral::EffectOutputConfig config;
  config.width = kSize;
  config.height = kSize;
  ASSERT_EQ(graph.output->configure(config), EffectResult::Success);
  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  coral::EffectFrame frame;
  ASSERT_TRUE(graph.output->waitForFrame(&frame, nullptr));
  EXPECT_EQ(frame.width(), kSize);

  config.width = kSize / 2;
  config.height = kSize / 4;
  ASSERT_EQ(graph.output->configure(config), EffectResult::Success);
  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  ASSERT_TRUE(graph.output->waitForFrame(&frame, nullptr));
  EXPECT_EQ(frame.width(), kSize / 2);
  EXPECT_EQ(frame.height(), kSize / 4);

  config.width = kSize;
  config.height = kSize;
  ASSERT_EQ(graph.output->configure(config), EffectResult::Success);
  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  ASSERT_TRUE(graph.output->waitForFrame(&frame, nullptr));
  EXPECT_EQ(frame.width(), kSize);
  graph.engine->stop();
}

TEST(EffectPipeline, ReadbackFitLetterboxesTheSource) {
  constexpr uint32_t kWidth = 64;
  constexpr uint32_t kHeight = 16;
  EffectEngineConfig config = testConfig();
  config.renderWidth = kWidth;
  config.renderHeight = kHeight;
  const auto engine = EffectEngine::create(config);
  ASSERT_NE(engine, nullptr);
  const auto input = coral::VideoFrameInputNode::create(kWidth, kHeight);
  const auto pass = engine->createEffectNode("fit");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(pass->setShader(bodyShader("  return texture(uInput0, uv);")), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);
  coral::EffectOutputConfig outputConfig;
  outputConfig.width = 32;
  outputConfig.height = 32;
  outputConfig.scaleMode = coral::EffectScaleMode::Fit;
  ASSERT_EQ(output->configure(outputConfig), EffectResult::Success);

  std::vector<uint8_t> rows(static_cast<size_t>(kWidth) * kHeight * 4, 0);
  for (uint32_t i = 0; i < kWidth * kHeight; ++i) {
    rows[i * 4 + 1] = 255;
    rows[i * 4 + 3] = 255;
  }
  coral::VideoFrameDesc frame;
  frame.width = kWidth;
  frame.height = kHeight;
  frame.format = coral::EffectPixelFormat::RGBA8;
  frame.planes[0] = { rows.data(), kWidth * 4 };
  frame.planeCount = 1;
  ASSERT_EQ(input->submit(frame), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
  const auto pixel = [&](const uint32_t x, const uint32_t y) {
    return &pixels[(static_cast<size_t>(y) * 32 + x) * 4];
  };
  EXPECT_NEAR(pixel(16, 0)[3], 0, kTolerance);     // letterbox above
  EXPECT_NEAR(pixel(16, 16)[1], 255, kTolerance);  // content
  EXPECT_NEAR(pixel(16, 31)[3], 0, kTolerance);    // letterbox below

  outputConfig.scaleMode = coral::EffectScaleMode::Crop;
  ASSERT_EQ(output->configure(outputConfig), EffectResult::Success);
  ASSERT_EQ(input->submit(frame), EffectResult::Success);
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
  EXPECT_NEAR(pixel(0, 0)[1], 255, kTolerance);
  EXPECT_NEAR(pixel(0, 0)[3], 255, kTolerance);

  outputConfig.scaleMode = coral::EffectScaleMode::Fit;
  outputConfig.letterboxColor = coral::EffectColor { 1.0f, 0.0f, 0.0f, 1.0f };
  ASSERT_EQ(output->configure(outputConfig), EffectResult::Success);
  ASSERT_EQ(input->submit(frame), EffectResult::Success);
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
  EXPECT_NEAR(pixel(16, 0)[0], 255, kTolerance);
  EXPECT_NEAR(pixel(16, 0)[3], 255, kTolerance);
  engine->stop();
}

TEST(EffectPipeline, TextureCacheHonoursTheByteBudget) {
  EffectEngineConfig config = testConfig();
  config.textureCacheBudgetBytes = 8; // two 1x1 RGBA entries
  const auto engine = EffectEngine::create(config);
  ASSERT_NE(engine, nullptr);
  const auto input = coral::ImageInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("budget");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(pass->setShader(bodyShader("  return texture(uInput0, uv);")), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  for (uint32_t frame = 0; frame < 8; ++frame) {
    const uint8_t pixel[4] = { static_cast<uint8_t>(frame * 20), 0, 0, 255 };
    ASSERT_EQ(input->setImage(coral::EffectImage::create(coral::EffectImageDesc::rgba8(pixel, 1, 1))),
              EffectResult::Success);
    ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
    ASSERT_TRUE(output->waitForFrame(nullptr, nullptr));
  }
  // The budget is enforced at beginFrame; the image just uploaded for the final frame can be one over.
  EXPECT_LE(engine->stats().textureCount, 3u);
  EXPECT_GT(engine->stats().imageTextureEvictions, 0u);
  EXPECT_GT(engine->stats().textureCacheBytes, 0u);
  EXPECT_GE(engine->stats().textureCachePeakBytes, engine->stats().textureCacheBytes);
  EXPECT_GT(engine->stats().programCompileCount, 0u);
  EXPECT_GE(engine->stats().programCompileMilliseconds, 0.0);
  EXPECT_GT(engine->stats().pipelineCreationCount, 0u);
  EXPECT_GT(engine->stats().pipelineCreationMilliseconds, 0.0);
  engine->stop();
}

TEST(EffectPipeline, PrimaryOutputCanSwitchWhileRunning) {
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("switch");
  const auto first = coral::ReadbackOutputNode::create();
  const auto second = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  ASSERT_EQ(pass->setShader(bodyShader("  return texture(uInput0, uv);")), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(first), EffectResult::Success);
  ASSERT_EQ(engine->attach(second), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, first), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, second), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(first), EffectResult::Success);
  ASSERT_EQ(engine->addOutput(second), EffectResult::Success);
  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  ASSERT_TRUE(first->waitForFrame(nullptr, nullptr));
  ASSERT_EQ(engine->setOutput(second), EffectResult::Success);
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  ASSERT_TRUE(second->waitForFrame(nullptr, nullptr));
  engine->stop();
}

TEST(EffectPipeline, MultiDeclaratorVaryingsGetSeparateLocations) {
  Graph graph = makeGraph("  return texture(uInput0, uv);");
  EffectShaderDesc shader = bodyShader("  return texture(uInput0, uv);");
  shader.vertexSource = R"(#version 300 es
precision highp float;
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;
out vec2 vUV, vColor;
void main() {
  vUV = aTexCoord;
  vColor = aTexCoord;
  gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";
  ASSERT_EQ(graph.pass->setShader(shader), EffectResult::Success);
  START_OR_SKIP(graph);
  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  ASSERT_TRUE(graph.output->waitForFrame(nullptr, nullptr));
  graph.engine->stop();
}

TEST(EffectPipeline, ArrayVaryingsReserveConsecutiveLocations) {
  Graph graph = makeGraph("  return texture(uInput0, uv);");
  EffectShaderDesc shader = bodyShader("  return texture(uInput0, uv);");
  shader.vertexSource = R"(#version 300 es
precision highp float;
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;
out vec2 vUV;
out vec2 vColor[2];
void main() {
  vUV = aTexCoord;
  vColor[0] = aTexCoord;
  vColor[1] = aTexCoord;
  gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";
  ASSERT_EQ(graph.pass->setShader(shader), EffectResult::Success);
  START_OR_SKIP(graph);
  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  ASSERT_TRUE(graph.output->waitForFrame(nullptr, nullptr));
  graph.engine->stop();
}

TEST(EffectPipeline, LongRunKeepsCachesAndFramesBounded) {
  EffectEngineConfig config = testConfig();
  config.maxCachedPrograms = 4;
  config.textureCacheBudgetBytes = 1u << 20;
  const auto engine = EffectEngine::create(config);
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("long-run");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);
  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  for (uint32_t frame = 0; frame < 150; ++frame) {
    if (frame % 10 == 0) {
      const float shade = static_cast<float>(frame % 255) / 255.0f;
      const std::string body = "  return vec4(" + std::to_string(shade) + ", 0.25, 0.5, 1.0);";
      ASSERT_EQ(pass->setShader(bodyShader(body.c_str())), EffectResult::Success);
    }
    ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
    ASSERT_TRUE(output->waitForFrame(nullptr, nullptr));
  }
  EXPECT_LE(engine->stats().programCount, 5u);
  EXPECT_LE(engine->stats().textureCount, 6u);
  EXPECT_GE(engine->stats().frameCount, 150u);
  engine->stop();
}

TEST(EffectPipeline, RenderSizeCanChangeWhileRunning) {
  EffectEngineConfig config = testConfig();
  config.enableGpuTiming = true;
  const auto engine = EffectEngine::create(config);
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("resize");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(pass->setShader(bodyShader("  return texture(uInput0, uv);")), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);
  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  coral::EffectFrame frame;
  ASSERT_TRUE(output->waitForFrame(&frame, nullptr));
  EXPECT_EQ(frame.width(), kSize);

  engine->setRenderSize(kSize / 2, kSize / 4);
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  ASSERT_TRUE(output->waitForFrame(&frame, nullptr));
  EXPECT_EQ(frame.width(), kSize / 2);
  EXPECT_EQ(frame.height(), kSize / 4);

  engine->setRenderSize(kSize, kSize);
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  ASSERT_TRUE(output->waitForFrame(&frame, nullptr));
  EXPECT_EQ(frame.width(), kSize);
  EXPECT_GT(engine->stats().gpuMilliseconds, 0.0);
  engine->stop();
}

TEST(EffectPipeline, RotatedFitBgraOutputComposesTransforms) {
  constexpr uint32_t kWidth = 16;
  constexpr uint32_t kHeight = 8;
  EffectEngineConfig config = testConfig();
  config.renderWidth = kWidth;
  config.renderHeight = kHeight;
  const auto engine = EffectEngine::create(config);
  ASSERT_NE(engine, nullptr);
  const auto input = coral::VideoFrameInputNode::create(kWidth, kHeight);
  const auto pass = engine->createEffectNode("rotate-fit-bgra");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(pass->setShader(bodyShader("  return texture(uInput0, uv);")), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);
  coral::EffectOutputConfig outputConfig;
  outputConfig.width = 8;
  outputConfig.height = 32;
  outputConfig.format = coral::EffectPixelFormat::BGRA8;
  outputConfig.rotation = coral::EffectRotation::Clockwise90;
  outputConfig.scaleMode = coral::EffectScaleMode::Fit;
  ASSERT_EQ(output->configure(outputConfig), EffectResult::Success);

  std::vector<uint8_t> rows(static_cast<size_t>(kWidth) * kHeight * 4, 0);
  for (uint32_t i = 0; i < kWidth * kHeight; ++i) {
    rows[i * 4 + 1] = 255;
    rows[i * 4 + 3] = 255;
  }
  coral::VideoFrameDesc frame;
  frame.width = kWidth;
  frame.height = kHeight;
  frame.format = coral::EffectPixelFormat::RGBA8;
  frame.planes[0] = { rows.data(), kWidth * 4 };
  frame.planeCount = 1;
  ASSERT_EQ(input->submit(frame), EffectResult::Success);
  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  coral::EffectFrame delivered;
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(&delivered, &pixels));
  EXPECT_EQ(delivered.format(), coral::EffectPixelFormat::BGRA8);
  ASSERT_EQ(pixels.size(), static_cast<size_t>(8) * 32 * 4);
  const auto pixel = [&](const uint32_t x, const uint32_t y) {
    return &pixels[(static_cast<size_t>(y) * 8 + x) * 4];
  };
  EXPECT_NEAR(pixel(4, 0)[3], 0, kTolerance);      // top letterbox after rotation
  EXPECT_NEAR(pixel(4, 16)[1], 255, kTolerance);   // green content, BGRA has G at byte 1
  EXPECT_NEAR(pixel(4, 16)[3], 255, kTolerance);
  engine->stop();
}

TEST(EffectPipeline, VertexOutputInterfaceBlockIsFlattened) {
  Graph graph = makeGraph("  return texture(uInput0, uv);");
  EffectShaderDesc shader = bodyShader("  return texture(uInput0, uv);");
  shader.vertexSource = R"(#version 300 es
precision highp float;
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;
out VSOut { vec2 vUV; } vsOut;
void main() {
  vsOut.vUV = aTexCoord;
  gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";
  ASSERT_EQ(graph.pass->setShader(shader), EffectResult::Success);
  START_OR_SKIP(graph);
  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  ASSERT_TRUE(graph.output->waitForFrame(nullptr, nullptr));
  graph.engine->stop();
}

TEST(EffectPipeline, MatrixVaryingReservesItsColumns) {
  Graph graph = makeGraph("  return texture(uInput0, uv);");
  EffectShaderDesc shader = bodyShader("  return texture(uInput0, uv);");
  shader.vertexSource = R"(#version 300 es
precision highp float;
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;
out mat2 vMatrix;
out vec2 vUV;
void main() {
  vMatrix = mat2(aTexCoord, aTexCoord);
  vUV = aTexCoord;
  gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";
  ASSERT_EQ(graph.pass->setShader(shader), EffectResult::Success);
  START_OR_SKIP(graph);
  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  ASSERT_TRUE(graph.output->waitForFrame(nullptr, nullptr));
  graph.engine->stop();
}

TEST(EffectTypes, BackendCompilesGlslOnAWorkerThread) {
  const std::string source = R"(#version 450
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;
layout(location = 0) out vec2 vUV;
void main() {
  vUV = aTexCoord;
  gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";
  auto future = std::async(std::launch::async, [&] {
    backend::ShaderCompileResult result;
    const backend::Result status = backend::compileShaderToSpirv(source, backend::ShaderStage::Vertex, result);
    return std::make_pair(status, std::move(result));
  });
  auto [status, result] = future.get();
  if (status == backend::Result::Unsupported) GTEST_SKIP() << result.error;
  ASSERT_EQ(status, backend::Result::Success) << result.error;
  EXPECT_FALSE(result.spirv.empty());
  EXPECT_TRUE(result.success());
}

TEST(EffectPipeline, PrecompiledVertexSpirvFeedsTheProgram) {
  const std::string vertex = R"(#version 310 es
precision highp float;
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;
layout(location = 0) out vec2 vUV;
void main() {
  vUV = aTexCoord;
  gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";
  backend::ShaderCompileResult compiled;
  const backend::Result status =
      backend::compileShaderToSpirv(vertex, backend::ShaderStage::Vertex, compiled);
  if (status == backend::Result::Unsupported) GTEST_SKIP() << compiled.error;
  ASSERT_EQ(status, backend::Result::Success) << compiled.error;

  Graph graph = makeGraph("  return texture(uInput0, uv);");
  EffectShaderDesc shader = bodyShader("  return texture(uInput0, uv);");
  shader.vertexSource = vertex;
  shader.vertexSpirv = compiled.spirv;
  ASSERT_EQ(graph.pass->setShader(shader), EffectResult::Success);
  START_OR_SKIP(graph);
  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  ASSERT_TRUE(graph.output->waitForFrame(nullptr, nullptr));
  graph.engine->stop();
}

#if defined(_WIN32)
TEST(EffectPipeline, AsyncShaderCompilationKeepsTheOldProgramUntilReady) {
  EffectEngineConfig config = testConfig();
  config.asyncShaderCompilation = true;
  const auto engine = EffectEngine::create(config);
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("async");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(pass->setShader(bodyShader("  return vec4(1.0, 0.0, 0.0, 1.0);")), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);
  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
  ASSERT_EQ(pixels.size(), static_cast<size_t>(kSize) * kSize * 4);
  EXPECT_NEAR(pixels[0], 255, kTolerance);

  // The new shader compiles on the worker; this frame must still be the old red program.
  ASSERT_EQ(pass->setShader(bodyShader("  return vec4(0.0, 1.0, 0.0, 1.0);")), EffectResult::Success);
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
  EXPECT_NEAR(pixels[0], 255, kTolerance);
  EXPECT_NEAR(pixels[1], 0, kTolerance);

  bool becameGreen = false;
  for (uint32_t attempt = 0; attempt < 400 && !becameGreen; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
    ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
    becameGreen = pixels[1] > 200 && pixels[0] < 50;
  }
  EXPECT_TRUE(becameGreen) << pass->diagnostics() << " state=" << static_cast<int>(pass->shaderState());
  engine->stop();
}
TEST(EffectPipeline, AsyncPipelineKeepsTheOldProgramUntilReady) {
  EffectEngineConfig config = testConfig();
  config.asyncShaderCompilation = true;
  config.asyncPipelineCreation = true;
  const auto engine = EffectEngine::create(config);
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("async");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(pass->setShader(bodyShader("  return vec4(1.0, 0.0, 0.0, 1.0);")), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);
  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
  ASSERT_EQ(pixels.size(), static_cast<size_t>(kSize) * kSize * 4);
  EXPECT_NEAR(pixels[0], 255, kTolerance);

  // The new shader compiles on the worker; this frame must still be the old red program.
  ASSERT_EQ(pass->setShader(bodyShader("  return vec4(0.0, 1.0, 0.0, 1.0);")), EffectResult::Success);
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
  EXPECT_NEAR(pixels[0], 255, kTolerance);
  EXPECT_NEAR(pixels[1], 0, kTolerance);

  bool becameGreen = false;
  for (uint32_t attempt = 0; attempt < 400 && !becameGreen; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
    ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
    becameGreen = pixels[1] > 200 && pixels[0] < 50;
  }
  EXPECT_TRUE(becameGreen) << pass->diagnostics() << " state=" << static_cast<int>(pass->shaderState());
  engine->stop();
}
#endif

#if defined(_WIN32)
TEST(EffectPipeline, AsyncShaderFailureKeepsTheOldProgram) {
  EffectEngineConfig config = testConfig();
  config.asyncShaderCompilation = true;
  const auto engine = EffectEngine::create(config);
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("async-fail");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(pass->setShader(bodyShader("  return vec4(1.0, 0.0, 0.0, 1.0);")), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);
  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
  EXPECT_NEAR(pixels[0], 255, kTolerance);

  ASSERT_EQ(pass->setShader(bodyShader("  return this_is_not_valid;")), EffectResult::Success);
  bool failed = false;
  for (uint32_t attempt = 0; attempt < 400 && !failed; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
    ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
    failed = pass->shaderState() == coral::EffectShaderState::Failed;
    EXPECT_NEAR(pixels[0], 255, kTolerance);
    EXPECT_NEAR(pixels[1], 0, kTolerance);
  }
  EXPECT_TRUE(failed);
  EXPECT_FALSE(pass->diagnostics().empty());
  engine->stop();
}
#endif

/**
 * A rejected shader must not pin the node to the block it will never install.
 *
 * The pass keeps drawing the program it already had - that is the documented contract - but the
 * application's next `setParameter` has to reach *that* program. While the failed description stayed
 * pending, every write landed in its block, the executor skipped the description because it had already
 * failed, and nothing ever copied the values across: the pass silently stopped responding to its
 * parameters while `setShader` kept returning `Success`.
 */
TEST(EffectPipeline, ARejectedShaderLeavesTheParameterBlockWritable) {
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("reject");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);

  EffectShaderDesc shader = bodyShader("  return vec4(effectParams(0).x, 0.0, 0.0, 1.0);");
  shader.parameters = { { "tint", coral::EffectValueType::Vec4 } };
  ASSERT_EQ(pass->setShader(shader), EffectResult::Success);
  ASSERT_EQ(pass->setParameter("tint", coral::EffectVec4{ 1.0f, 0.0f, 0.0f, 0.0f }), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
  EXPECT_NEAR(pixels[0], 255, kTolerance);

  ASSERT_EQ(pass->setShader(bodyShader("  return this_is_not_valid;")), EffectResult::Success);
  for (uint32_t attempt = 0; attempt < 400 && pass->shaderState() != EffectShaderState::Failed; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
    ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
  }
  ASSERT_EQ(pass->shaderState(), EffectShaderState::Failed);
  const std::string compileError = pass->diagnostics();
  ASSERT_FALSE(compileError.empty());

  // The installed program still reads its block, so this write has to reach it.
  ASSERT_EQ(pass->setParameter("tint", coral::EffectVec4{ 0.0f, 0.0f, 0.0f, 1.0f }), EffectResult::Success)
      << pass->diagnostics();
  bool dimmed = false;
  for (uint32_t attempt = 0; attempt < 400 && !dimmed; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
    ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
    dimmed = pixels[0] < 20;
  }
  EXPECT_TRUE(dimmed) << "the rejected shader left the parameter block unwritable";
  // A parameter write must not erase the reason the pass is still drawing its previous program.
  EXPECT_EQ(pass->diagnostics(), compileError);
  engine->stop();
}

#if defined(_WIN32)
TEST(EffectPipeline, AsyncShaderSwitchAppliesTheLatestPendingShader) {
  EffectEngineConfig config = testConfig();
  config.asyncShaderCompilation = true;
  const auto engine = EffectEngine::create(config);
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("async-latest");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(pass->setShader(bodyShader("  return vec4(1.0, 0.0, 0.0, 1.0);")), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);
  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));

  ASSERT_EQ(pass->setShader(bodyShader("  return vec4(0.0, 1.0, 0.0, 1.0);")), EffectResult::Success);
  ASSERT_EQ(pass->setShader(bodyShader("  return vec4(0.0, 0.0, 1.0, 1.0);")), EffectResult::Success);
  bool blue = false;
  for (uint32_t attempt = 0; attempt < 400 && !blue; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
    ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
    blue = pixels[2] > 200 && pixels[0] < 50 && pixels[1] < 50;
    EXPECT_FALSE(pixels[1] > 200 && pixels[0] < 50);
  }
  EXPECT_TRUE(blue) << pass->diagnostics();
  engine->stop();
}
TEST(EffectPipeline, AsyncPipelineSwitchAppliesTheLatestPendingShader) {
  EffectEngineConfig config = testConfig();
  config.asyncShaderCompilation = true;
  config.asyncPipelineCreation = true;
  const auto engine = EffectEngine::create(config);
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("async-latest");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(pass->setShader(bodyShader("  return vec4(1.0, 0.0, 0.0, 1.0);")), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);
  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));

  ASSERT_EQ(pass->setShader(bodyShader("  return vec4(0.0, 1.0, 0.0, 1.0);")), EffectResult::Success);
  ASSERT_EQ(pass->setShader(bodyShader("  return vec4(0.0, 0.0, 1.0, 1.0);")), EffectResult::Success);
  bool blue = false;
  for (uint32_t attempt = 0; attempt < 400 && !blue; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
    ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
    blue = pixels[2] > 200 && pixels[0] < 50 && pixels[1] < 50;
    EXPECT_FALSE(pixels[1] > 200 && pixels[0] < 50);
  }
  EXPECT_TRUE(blue) << pass->diagnostics();
  engine->stop();
}
#endif

TEST(EffectPipeline, ReadbackCanOutputRgba16f) {
  EffectEngineConfig config = testConfig();
  config.renderWidth = kSize;
  config.renderHeight = kSize;
  const auto engine = EffectEngine::create(config);
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("hdr");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(pass->setShader(bodyShader("  return vec4(2.0, 0.5, 0.25, 1.0);")), EffectResult::Success);
  ASSERT_EQ(pass->setOutputFormat(coral::EffectPixelFormat::RGBA16F), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);
  coral::EffectOutputConfig outputConfig;
  outputConfig.width = kSize;
  outputConfig.height = kSize;
  outputConfig.format = coral::EffectPixelFormat::RGBA16F;
  ASSERT_EQ(output->configure(outputConfig), EffectResult::Success);
  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  coral::EffectFrame frame;
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(&frame, &pixels));
  EXPECT_EQ(frame.format(), coral::EffectPixelFormat::RGBA16F);
  ASSERT_EQ(pixels.size(), static_cast<size_t>(kSize) * kSize * 8);
  const auto half = [](const uint16_t bits) {
    const uint32_t sign = (bits & 0x8000u) != 0 ? 1u : 0u;
    const uint32_t exponent = (bits >> 10) & 0x1fu;
    const uint32_t mantissa = bits & 0x3ffu;
    float value = 0.0f;
    if (exponent == 0) value = static_cast<float>(mantissa) / 1024.0f;
    else if (exponent != 31) value = (1.0f + static_cast<float>(mantissa) / 1024.0f) * std::pow(2.0f, static_cast<int>(exponent) - 15);
    return sign != 0 ? -value : value;
  };
  EXPECT_NEAR(half(static_cast<uint16_t>(pixels[0] | (pixels[1] << 8))), 2.0f, 0.02f);
  EXPECT_NEAR(half(static_cast<uint16_t>(pixels[2] | (pixels[3] << 8))), 0.5f, 0.02f);
  EXPECT_NEAR(half(static_cast<uint16_t>(pixels[4] | (pixels[5] << 8))), 0.25f, 0.02f);
  EXPECT_NEAR(half(static_cast<uint16_t>(pixels[6] | (pixels[7] << 8))), 1.0f, 0.02f);
  engine->stop();
}

} // namespace

TEST(EffectPipeline, DisabledOutputIsSkipped) {
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("disabled-output");
  const auto first = SpyDirectOutputNode::create();
  const auto second = SpyDirectOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  ASSERT_EQ(pass->setShader(bodyShader("  return texture(uInput0, uv);")), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(first), EffectResult::Success);
  ASSERT_EQ(engine->attach(second), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, first), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, second), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(first), EffectResult::Success);
  ASSERT_EQ(engine->addOutput(second), EffectResult::Success);
  ASSERT_EQ(second->setEnabled(false), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  EXPECT_TRUE(first->state()->consumeCalled.load());
  EXPECT_FALSE(second->state()->consumeCalled.load());
  EXPECT_FALSE(second->state()->beginCalled.load());
  engine->stop();
}
TEST(EffectPipeline, MultipleOutputsDisableDirectSurface) {
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("multi");
  const auto readback = coral::ReadbackOutputNode::create();
  const auto spy = SpyDirectOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(readback, nullptr);
  ASSERT_NE(spy, nullptr);
  ASSERT_EQ(pass->setShader(bodyShader("  return texture(uInput0, uv);")), EffectResult::Success);
  ASSERT_EQ(pass->setDirectToSurface(true), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(readback), EffectResult::Success);
  ASSERT_EQ(engine->attach(spy), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, readback), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, spy), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(readback), EffectResult::Success);
  ASSERT_EQ(engine->addOutput(spy), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  EXPECT_FALSE(spy->state()->beginCalled.load());
  EXPECT_FALSE(spy->state()->acquireCalled.load());
  EXPECT_TRUE(spy->state()->consumeCalled.load());
  engine->stop();
}
TEST(EffectPipeline, ExecutorUsesDirectOutputWhenEligible) {
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("direct");
  const auto output = SpyDirectOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(pass->setShader(bodyShader("  return texture(uInput0, uv);")), EffectResult::Success);
  ASSERT_EQ(pass->setDirectToSurface(true), EffectResult::Success);
  coral::EffectOutputConfig config;
  config.width = kSize;
  config.height = kSize;
  config.format = coral::EffectPixelFormat::RGBA8;
  ASSERT_EQ(output->configure(config), EffectResult::Success);
  ASSERT_EQ(engine->attach(input), EffectResult::Success);
  ASSERT_EQ(engine->attach(output), EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success);

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();
  EXPECT_TRUE(output->state()->beginCalled.load());
  EXPECT_TRUE(output->state()->acquireCalled.load());
  EXPECT_TRUE(output->state()->presentCalled.load());
  EXPECT_FALSE(output->state()->consumeCalled.load());
  engine->stop();
}

/**
 * The two-phase present seam has to be inert until a backend implements it.
 *
 * No output in the tree answers `supportsDeferredPresent()` true today, so `Deferred` must run as
 * `Blocking`: the frame still renders, the engine reports the capability honestly, and `drainPresents` is a
 * synchronisation point a caller can use unconditionally.
 */
TEST(EffectPipeline, DeferredPresentDegradesToBlockingWithoutBackendSupport) {
  Graph graph = makeGraph("  return vec4(0.0, 1.0, 0.0, 1.0);");
  START_OR_SKIP(graph);

  coral::EffectOutputConfig outputConfig;
  outputConfig.presentPolicy = coral::EffectPresentPolicy::Deferred;
  outputConfig.presentOverflow = coral::EffectPresentOverflow::Wait;
  EXPECT_EQ(graph.output->configure(outputConfig), EffectResult::Success);

  // A readback output has no swapchain, so nothing can be deferred whatever the request says.
  EXPECT_FALSE(graph.engine->supportsDeferredPresent());
  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  EXPECT_EQ(graph.engine->drainPresents(std::chrono::milliseconds(50)), EffectResult::Success);

  std::vector<uint8_t> pixels;
  ASSERT_TRUE(graph.output->waitForFrame(nullptr, &pixels));

  // Switching back to the default keeps the same graph usable, which is what one configuration shared
  // across backends relies on.
  outputConfig.presentPolicy = coral::EffectPresentPolicy::Blocking;
  EXPECT_EQ(graph.output->configure(outputConfig), EffectResult::Success);
  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();
  graph.engine->stop();
}

/**
 * Dropping the last engine handle while it is running must stop its render thread.
 *
 * The render thread owns a strong reference to the implementation, so without an explicit stop in the
 * facade's destructor the engine would stay alive (and keep its device) until the thread was joined by
 * someone who no longer exists. The join inside the destructor is also what makes the graph teardown that
 * follows it safe.
 */
TEST(EffectPipeline, DestroyingARunningEngineStopsItsRenderThread) {
  Graph graph = makeGraph("  return vec4(0.0, 0.0, 1.0, 1.0);");
  START_OR_SKIP(graph);
  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();

  // Drop every external handle: the destructor has to ask the thread to exit and reap it before the
  // implementation (and the graph above it) is freed.
  graph.engine.reset();
  graph.input.reset();
  graph.pass.reset();
  graph.output.reset();
  SUCCEED();
}

/**
 * Two callers stopping at once must both be answered, and neither may race the thread handle.
 *
 * Before the lifecycle lock, the second `stop()` could reach `join()` on the handle the first one had
 * already reaped, which is undefined behaviour; with the explicit exit event it observes "no thread" and
 * returns success.
 */
TEST(EffectPipeline, ConcurrentStopsAreBothSafeAndTheEngineCanRestart) {
  Graph graph = makeGraph("  return vec4(1.0, 1.0, 0.0, 1.0);");
  START_OR_SKIP(graph);
  ASSERT_EQ(graph.engine->execute(), EffectResult::Success) << graph.engine->diagnostics();

  std::atomic<int> successes { 0 };
  std::thread first([&] {
    if (graph.engine->stop() == EffectResult::Success) ++successes;
  });
  std::thread second([&] {
    if (graph.engine->stop() == EffectResult::Success) ++successes;
  });
  first.join();
  second.join();
  EXPECT_EQ(successes.load(), 2);
  EXPECT_FALSE(graph.engine->isRunning());

  // The reaped handle must not stop a later start from installing a new one.
  EXPECT_EQ(graph.engine->start(), EffectResult::Success) << graph.engine->diagnostics();
  EXPECT_TRUE(graph.engine->isRunning());
  graph.engine->stop();
}

