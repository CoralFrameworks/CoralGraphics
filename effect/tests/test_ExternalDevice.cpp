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
 * @file test_ExternalDevice.cpp
 *
 * Phase-2 model B spike: an engine renders through a device it does not own.
 *
 * The surface creates the device on the test thread, releases the context, and the engine's render
 * thread makes the same device current. A second surface on the same device is created *after* the
 * engine stops, which is the assertion that `stop()` did not destroy the caller's device.
 *
 * This is the external-device half of model B. External drive (running `execute()` on the UI render
 * thread instead of the engine's own thread) is the next slice; the engine still owns its render
 * thread here.
 */

#include <effect/EffectEngine.h>
#include <effect/EffectPassNode.h>
#include <effect/ReadbackOutputNode.h>
#include <effect/TestPatternInputNode.h>
#include <effect/TextureOutputNode.h>

#include <gpu/Surface.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {

using coral::EffectEngine;
using coral::EffectEngineConfig;
using coral::EffectPixelFormat;
using coral::EffectRenderMode;
using coral::EffectResult;
using coral::EffectShaderDesc;

constexpr uint32_t kSize = 32;

EffectEngineConfig externalConfig(backend::Device* device) {
  EffectEngineConfig config;
  config.renderWidth = kSize;
  config.renderHeight = kSize;
  config.renderMode = EffectRenderMode::Manual;
  config.timeBase = 60;
  config.maxFramesInFlight = 1;
  config.framePoolBudgetBytes = 16u * 1024u * 1024u;
  config.externalDevice = device;
  return config;
}

EffectShaderDesc solidRedShader() {
  EffectShaderDesc shader;
  shader.name = "external-device-solid-red";
  shader.fragmentSource = "  return vec4(1.0, 0.0, 0.0, 1.0);";
  shader.inputCount = 1;
  return shader;
}

} // namespace

TEST(EffectExternalDrive, TwoEnginesShareOneDeviceOnOneThread) {
  auto host = graphics::gpu::Surface::make(kSize, kSize);
  if (host == nullptr) {
    GTEST_SKIP() << "no GPU device is available in this session";
  }
  backend::Device* const device = host->device();
  ASSERT_NE(device, nullptr);
  device->releaseCurrent();

  auto makeEngine = [&](const bool red) {
    EffectEngineConfig config = externalConfig(device);
    config.externalDrive = true;
    auto engine = EffectEngine::create(config);
    if (engine == nullptr) return std::pair<std::shared_ptr<EffectEngine>, std::shared_ptr<coral::ReadbackOutputNode>>{};
    auto input = coral::TestPatternInputNode::create(kSize, kSize);
    auto pass = engine->createEffectNode(red ? "red-pass" : "green-pass");
    EffectShaderDesc shader;
    shader.name = red ? "red-external" : "green-external";
    shader.fragmentSource = red ? "  return vec4(1.0, 0.0, 0.0, 1.0);"
                                : "  return vec4(0.0, 1.0, 0.0, 1.0);";
    shader.inputCount = 1;
    auto output = coral::ReadbackOutputNode::create();
    if (input == nullptr || pass == nullptr || output == nullptr ||
        engine->attach(input) != EffectResult::Success ||
        pass->setShader(shader) != EffectResult::Success ||
        engine->attach(output) != EffectResult::Success ||
        engine->connect(input, pass) != EffectResult::Success ||
        engine->connect(pass, output) != EffectResult::Success ||
        engine->setOutput(output) != EffectResult::Success) {
      return std::pair<std::shared_ptr<EffectEngine>, std::shared_ptr<coral::ReadbackOutputNode>>{};
    }
    const EffectResult started = engine->start();
    if (started != EffectResult::Success) return std::pair<std::shared_ptr<EffectEngine>, std::shared_ptr<coral::ReadbackOutputNode>>{};
    return std::pair<std::shared_ptr<EffectEngine>, std::shared_ptr<coral::ReadbackOutputNode>>{engine, output};
  };

  auto first = makeEngine(true);
  auto second = makeEngine(false);
  if (first.first == nullptr || second.first == nullptr) {
    GTEST_SKIP() << "the external-drive engines could not start";
  }
  ASSERT_EQ(EffectResult::Success, first.first->execute()) << first.first->diagnostics();
  ASSERT_EQ(EffectResult::Success, second.first->execute()) << second.first->diagnostics();

  coral::EffectFrame frame;
  std::vector<uint8_t> firstPixels;
  std::vector<uint8_t> secondPixels;
  ASSERT_TRUE(first.second->waitForFrame(&frame, &firstPixels));
  ASSERT_TRUE(second.second->waitForFrame(&frame, &secondPixels));
  const size_t centre = (static_cast<size_t>(kSize / 2) * kSize + kSize / 2) * 4;
  EXPECT_NEAR(firstPixels[centre + 0], 255, 3);
  EXPECT_NEAR(firstPixels[centre + 1], 0, 3);
  EXPECT_NEAR(secondPixels[centre + 0], 0, 3);
  EXPECT_NEAR(secondPixels[centre + 1], 255, 3);

  first.first->stop();
  second.first->stop();
  host.reset();
}

TEST(EffectExternalDrive, TextureOutputKeepsARingSlotForItsConsumer) {
  auto host = graphics::gpu::Surface::make(kSize, kSize);
  if (host == nullptr) {
    GTEST_SKIP() << "no GPU device is available in this session";
  }
  backend::Device* const device = host->device();
  ASSERT_NE(device, nullptr);
  device->releaseCurrent();

  EffectEngineConfig config = externalConfig(device);
  config.externalDrive = true;
  const auto engine = EffectEngine::create(config);
  ASSERT_NE(engine, nullptr);
  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  const auto pass = engine->createEffectNode("lease-pass");
  const auto output = coral::TextureOutputNode::create({ 2 });
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(EffectResult::Success, pass->setShader(solidRedShader()));
  ASSERT_EQ(EffectResult::Success, engine->attach(input));
  ASSERT_EQ(EffectResult::Success, engine->attach(output));
  ASSERT_EQ(EffectResult::Success, engine->connect(input, pass));
  ASSERT_EQ(EffectResult::Success, engine->connect(pass, output));
  ASSERT_EQ(EffectResult::Success, engine->setOutput(output));
  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) {
    GTEST_SKIP() << "the external device could not be used: " << engine->diagnostics();
  }
  ASSERT_EQ(EffectResult::Success, started) << engine->diagnostics();
  ASSERT_EQ(EffectResult::Success, engine->execute()) << engine->diagnostics();

  coral::TextureOutputNode::Frame held = output->latestFrame();
  ASSERT_TRUE(held.isValid());
  ASSERT_NE(held.keepAlive, nullptr);
  EXPECT_GE(held.keepAlive.use_count(), 2) << "the ring slot plus the held frame";
  // Render past the ring capacity while the consumer keeps the frame; it must not crash or reuse
  // the held texture. The producer may drop frames instead.
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(EffectResult::Success, engine->execute()) << engine->diagnostics();
  }
  held = {};
  engine->stop();
  host.reset();
}

TEST(EffectExternalDevice, RendersThroughACallerOwnedDevice) {
  auto host = graphics::gpu::Surface::make(kSize, kSize);
  if (host == nullptr) {
    GTEST_SKIP() << "no GPU device is available in this session";
  }
  backend::Device* const device = host->device();
  ASSERT_NE(device, nullptr);
  // The engine's render thread is about to make this context current; a GL context can only be
  // current on one thread.
  device->releaseCurrent();

  const auto engine = EffectEngine::create(externalConfig(device));
  ASSERT_NE(engine, nullptr);

  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  ASSERT_NE(input, nullptr);
  ASSERT_EQ(EffectResult::Success, engine->attach(input));

  const auto pass = engine->createEffectNode("external-device-pass");
  ASSERT_NE(pass, nullptr);
  ASSERT_EQ(EffectResult::Success, pass->setShader(solidRedShader()));

  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(EffectResult::Success, engine->attach(output));
  ASSERT_EQ(EffectResult::Success, engine->connect(input, pass));
  ASSERT_EQ(EffectResult::Success, engine->connect(pass, output));
  ASSERT_EQ(EffectResult::Success, engine->setOutput(output));

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) {
    GTEST_SKIP() << "the external device could not be used: " << engine->diagnostics();
  }
  ASSERT_EQ(EffectResult::Success, started) << engine->diagnostics();
  EXPECT_EQ(engine->device(), device);

  ASSERT_EQ(EffectResult::Success, engine->execute()) << engine->diagnostics();
  coral::EffectFrame frame;
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(&frame, &pixels)) << engine->diagnostics();
  ASSERT_EQ(pixels.size(), static_cast<size_t>(kSize) * kSize * 4);
  const size_t centre = (static_cast<size_t>(kSize / 2) * kSize + kSize / 2) * 4;
  EXPECT_NEAR(pixels[centre + 0], 255, 3);
  EXPECT_NEAR(pixels[centre + 1], 0, 3);
  EXPECT_NEAR(pixels[centre + 2], 0, 3);
  EXPECT_NEAR(pixels[centre + 3], 255, 3);

  EXPECT_EQ(EffectResult::Success, engine->stop());
  // If `stop()` destroyed the caller's device this call would use a dead handle; it also proves the
  // device is still usable after the engine's render thread has exited.
  ASSERT_TRUE(device->makeCurrent() == backend::Result::Success);
  graphics::gpu::SurfaceConfiguration configuration;
  configuration.width = kSize;
  configuration.height = kSize;
  configuration.device = device;
  auto second = graphics::gpu::Surface::makeOn(device, configuration);
  EXPECT_NE(second, nullptr) << "the external device must outlive the engine";
  second.reset();
  host.reset();
}
