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
 * @file test_EffectNativeTexture.cpp
 *
 * A pass samples a texture that belongs to the caller and lives on the engine device, with no upload
 * and no readback. This is the zero-copy path a GPU text surface uses: the surface renders the glyphs
 * into its target, and the effect pass samples that target directly.
 */

#include <effect/EffectEngine.h>
#include <effect/EffectPassNode.h>
#include <effect/ReadbackOutputNode.h>
#include <effect/TestPatternInputNode.h>

#include <graphics/Canvas.h>
#include <graphics/Paint.h>
#include <geometry/Rect.h>
#include <gpu/Surface.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace {

using coral::EffectEngine;
using coral::EffectEngineConfig;
using coral::EffectPixelFormat;
using coral::EffectRenderMode;
using coral::EffectResult;
using coral::EffectShaderDesc;
using coral::TextureSource;

constexpr uint32_t kSize = 64;
constexpr uint8_t kTolerance = 3;

EffectEngineConfig testConfig() {
  EffectEngineConfig config;
  config.renderWidth = kSize;
  config.renderHeight = kSize;
  config.renderMode = EffectRenderMode::Manual;
  config.timeBase = 60;
  config.maxFramesInFlight = 1;
  config.framePoolBudgetBytes = 16u * 1024u * 1024u;
  return config;
}

EffectShaderDesc nativeSamplerShader() {
  EffectShaderDesc shader;
  shader.name = "native-texture";
  shader.fragmentSource = "  return texture(uSubtitle, uv);";
  shader.inputCount = 1;
  return shader;
}

} // namespace

TEST(EffectNativeTexture, SamplesACallerOwnedGpuTexture) {
  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(nullptr, engine);

  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  ASSERT_NE(nullptr, input);
  ASSERT_EQ(EffectResult::Success, engine->attach(input));

  const auto pass = engine->createEffectNode("pass");
  ASSERT_NE(nullptr, pass);
  // The sampler has to be named before the body compiles; a transparent texel is the placeholder.
  ASSERT_EQ(EffectResult::Success,
    pass->setTexture("uSubtitle", TextureSource::solid({ 0.0f, 0.0f, 0.0f, 1.0f })));
  ASSERT_EQ(EffectResult::Success, pass->setShader(nativeSamplerShader()));

  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(nullptr, output);
  ASSERT_EQ(EffectResult::Success, engine->attach(output));
  ASSERT_EQ(EffectResult::Success, engine->connect(input, pass));
  ASSERT_EQ(EffectResult::Success, engine->connect(pass, output));
  ASSERT_EQ(EffectResult::Success, engine->setOutput(output));

  std::shared_ptr<graphics::gpu::Surface> surface;
  engine->setOnDeviceReady([&surface](backend::Device& device) {
    graphics::gpu::SurfaceConfiguration configuration;
    configuration.width = kSize;
    configuration.height = kSize;
    configuration.device = &device;
    surface = graphics::gpu::Surface::make(configuration);
    if (surface == nullptr) {
      return;
    }
    auto* canvas = surface->beginFrame();
    if (canvas == nullptr) {
      return;
    }
    graphics::Paint paint;
    paint.color = graphics::Color(1.0f, 0.0f, 0.0f, 1.0f);
    canvas->drawRect(graphics::Rect::MakeLTRB(0.0f, 0.0f, static_cast<float>(kSize),
                        static_cast<float>(kSize)), paint);
    surface->submit();
  });

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) {
    GTEST_SKIP() << "no GPU device could be brought up: " << engine->diagnostics();
  }
  ASSERT_EQ(EffectResult::Success, started) << engine->diagnostics();
  if (surface == nullptr || !surface->target().valid()) {
    GTEST_SKIP() << "the GPU surface could not be created on the engine device";
  }

  const auto token = surface->target().value();
  ASSERT_EQ(EffectResult::Success,
    pass->setTexture("uSubtitle", TextureSource::fromNativeTexture(token, kSize, kSize,
                                    EffectPixelFormat::RGBA8)));

  ASSERT_EQ(EffectResult::Success, engine->execute()) << engine->diagnostics();
  coral::EffectFrame frame;
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(&frame, &pixels)) << engine->diagnostics();
  ASSERT_EQ(pixels.size(), static_cast<size_t>(kSize) * kSize * 4);
  const size_t centre = (static_cast<size_t>(kSize / 2) * kSize + kSize / 2) * 4;
  EXPECT_NEAR(pixels[centre + 0], 255, kTolerance);
  EXPECT_NEAR(pixels[centre + 1], 0, kTolerance);
  EXPECT_NEAR(pixels[centre + 2], 0, kTolerance);
  EXPECT_NEAR(pixels[centre + 3], 255, kTolerance);

  surface.reset();
  engine->stop();
}
