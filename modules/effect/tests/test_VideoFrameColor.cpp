// Copyright (c) Coral. All rights reserved.
//
// The effect graph takes decoded YUV planes and hands the layer pass RGBA bytes. libyuv's `...ToARGB`
// conversions write a 32-bit ARGB word, which is BGRA in memory, so a caller that names its destination
// RGBA has to use the mirrored chroma order. This test pins the channel order down with a red frame: a
// B/R swap still renders a valid picture, and only a pixel check catches it.

#include <effect/EffectEngine.h>
#include <effect/EffectFrame.h>
#include <effect/EffectPassNode.h>
#include <effect/EffectShader.h>
#include <effect/ReadbackOutputNode.h>
#include <effect/VideoFrameInputNode.h>

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

constexpr uint32_t kSize = 8;
constexpr uint8_t kTolerance = 24;

EffectEngineConfig testConfig() {
  EffectEngineConfig config;
  config.renderWidth = kSize;
  config.renderHeight = kSize;
  config.renderMode = EffectRenderMode::Manual;
  config.timeBase = 60;
  config.maxFramesInFlight = 1;
  config.framePoolBudgetBytes = 4u * 1024u * 1024u;
  return config;
}

EffectShaderDesc copyShader() {
  EffectShaderDesc shader;
  shader.name = "copy-video";
  shader.fragmentSource = "  return texture(uInput0, uv);";
  shader.inputCount = 1;
  return shader;
}

} // namespace

TEST(EffectVideoColor, Yuv420RedArrivesAsRedNotBlueInTheReadback) {
  const auto engine = EffectEngine::create(testConfig());
  if (engine == nullptr) {
    GTEST_SKIP() << "the effect engine could not be created";
  }
  const auto input = coral::VideoFrameInputNode::create(kSize, kSize);
  ASSERT_NE(input, nullptr);
  ASSERT_EQ(engine->attach(input), EffectResult::Success) << engine->diagnostics();

  const auto pass = engine->createEffectNode("copy");
  ASSERT_NE(pass, nullptr);
  ASSERT_EQ(pass->setShader(copyShader()), EffectResult::Success) << engine->diagnostics();

  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(engine->attach(output), EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->connect(input, pass), EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->connect(pass, output), EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->setOutput(output), EffectResult::Success) << engine->diagnostics();

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) {
    GTEST_SKIP() << "no GPU device could be brought up: " << engine->diagnostics();
  }
  ASSERT_EQ(started, EffectResult::Success) << engine->diagnostics();

  // BT.601 limited range: Y=81, U=90, V=240 is saturated red. The scalar conversion this replaced produced
  // (roughly) R=255, G=0, B=0 for those codes.
  std::vector<uint8_t> y(kSize * kSize, 81);
  std::vector<uint8_t> u((kSize / 2) * (kSize / 2), 90);
  std::vector<uint8_t> v((kSize / 2) * (kSize / 2), 240);
  coral::VideoFrameDesc desc;
  desc.width = kSize;
  desc.height = kSize;
  desc.format = EffectPixelFormat::YUV420;
  desc.bt709 = false;
  desc.fullRange = false;
  desc.planeCount = 3;
  desc.planes[0] = coral::VideoFramePlane{ y.data(), kSize };
  desc.planes[1] = coral::VideoFramePlane{ u.data(), kSize / 2 };
  desc.planes[2] = coral::VideoFramePlane{ v.data(), kSize / 2 };
  ASSERT_EQ(input->submit(desc), EffectResult::Success) << engine->diagnostics();

  ASSERT_EQ(engine->execute(), EffectResult::Success) << engine->diagnostics();

  coral::EffectFrame frame;
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(&frame, &pixels)) << engine->diagnostics();
  ASSERT_EQ(pixels.size(), static_cast<size_t>(kSize) * kSize * 4);
  const size_t centre = (static_cast<size_t>(kSize / 2) * kSize + kSize / 2) * 4;
  EXPECT_GE(pixels[centre + 0], 255 - kTolerance) << "red is in byte 0, so the destination is RGBA";
  EXPECT_LE(pixels[centre + 1], kTolerance);
  EXPECT_LE(pixels[centre + 2], kTolerance)
    << "blue in byte 2 means the libyuv ARGB (BGRA) word was stored as if it were RGBA";
  EXPECT_GE(pixels[centre + 3], 255 - kTolerance);
}
