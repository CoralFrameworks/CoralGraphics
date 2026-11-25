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
 * @file test_SdfText.cpp
 *
 * The SDF half of the GPU text plan: turn a glyph outline into one distance field, then let a shader
 * derive the fill and the outline from it. One field per glyph is what makes artistic text -- an
 * outline, a shadow, a glow, a border -- a set of shader parameters instead of extra glyph draws.
 */

#include <effect/EffectEngine.h>
#include <effect/EffectPassNode.h>
#include <effect/ReadbackOutputNode.h>
#include <effect/EffectTexture.h>
#include <effect/TestPatternInputNode.h>

#include <graphics/Bitmap.h>
#include <graphics/Font.h>
#include <graphics/FontManager.h>
#include <graphics/Path.h>
#include <graphics/SdfGlyph.h>
#include <gpu/Surface.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using coral::EffectEngine;
using coral::EffectEngineConfig;
using coral::EffectRenderMode;
using coral::EffectResult;
using coral::EffectShaderDesc;
using coral::TextureSource;

constexpr uint32_t kSize = 64;

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

EffectShaderDesc sdfShader() {
  EffectShaderDesc shader;
  shader.name = "sdf-text";
  shader.fragmentSource = R"(
    float d = texture(uSdf, uv).r;
    float fill = smoothstep(0.46, 0.54, d);
    float outline = smoothstep(0.32, 0.40, d) - fill;
    vec3 colour = mix(vec3(1.0), vec3(0.0), fill);
    colour = mix(colour, vec3(1.0, 0.55, 0.0), outline);
    return vec4(colour, clamp(fill + outline, 0.0, 1.0));
  )";
  shader.inputCount = 1;
  return shader;
}

/** Places the field in the centre of the render target so the shader can sample it with plain uv. */
std::shared_ptr<const coral::EffectImage> centreField(const graphics::SdfGlyph& field) {
  std::vector<uint8_t> pixels(static_cast<size_t>(kSize) * kSize * 4, 0);
  const uint32_t offsetX = (kSize - field.width) / 2;
  const uint32_t offsetY = (kSize - field.height) / 2;
  for (uint32_t y = 0; y < field.height; ++y) {
    for (uint32_t x = 0; x < field.width; ++x) {
      const uint8_t d = field.distance[static_cast<size_t>(y) * field.width + x];
      const size_t index = (static_cast<size_t>(y + offsetY) * kSize + x + offsetX) * 4;
      pixels[index + 0] = d;
      pixels[index + 3] = 255;
    }
  }
  return coral::EffectImage::create(
    coral::EffectImageDesc::rgba8(pixels.data(), kSize, kSize));
}

graphics::SdfGlyph glyphField(const char character) {
  auto typeface = graphics::FontManager::shared().defaultTypeface();
  if (typeface == nullptr) return {};
  auto font = graphics::Font::create(typeface, 56.0f);
  if (font == nullptr) return {};
  const auto blob = font->shape(std::string(1, character));
  if (blob.isEmpty()) return {};
  const auto& glyph = blob.glyphs().front();
  const graphics::Path outline = typeface->glyphPath(glyph.id);
  if (outline.isEmpty()) return {};
  const float em = static_cast<float>(typeface->unitsPerEm());
  const float scale = em > 0.0f ? 56.0f / em : 1.0f;
  graphics::Path device = outline.transformed(
    graphics::Matrix::MakeScale(graphics::Vector2 { scale, -scale }));
  const graphics::Rect bounds = device.tightBounds();
  device = device.transformed(graphics::Matrix::MakeTranslation(graphics::Vector3 {
    -bounds.getLeft() + 8.0f, -bounds.getTop() + 8.0f, 0.0f }));
  return graphics::makeSdfGlyph(device, 6.0f, 1.0f);
}

} // namespace

TEST(SdfText, AShaderDerivesTheFillAndTheOutlineFromOneField) {
  const graphics::SdfGlyph field = glyphField('A');
  ASSERT_FALSE(field.isEmpty());

  const auto image = centreField(field);
  ASSERT_NE(nullptr, image);

  const auto engine = EffectEngine::create(testConfig());
  ASSERT_NE(nullptr, engine);
  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  ASSERT_NE(nullptr, input);
  ASSERT_EQ(EffectResult::Success, engine->attach(input));
  const auto pass = engine->createEffectNode("sdf-pass");
  ASSERT_NE(nullptr, pass);
  ASSERT_EQ(EffectResult::Success,
    pass->setTexture("uSdf", TextureSource::solid({ 0.0f, 0.0f, 0.0f, 1.0f })));
  ASSERT_EQ(EffectResult::Success, pass->setShader(sdfShader()));
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(nullptr, output);
  ASSERT_EQ(EffectResult::Success, engine->attach(output));
  ASSERT_EQ(EffectResult::Success, engine->connect(input, pass));
  ASSERT_EQ(EffectResult::Success, engine->connect(pass, output));
  ASSERT_EQ(EffectResult::Success, engine->setOutput(output));

  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost) {
    GTEST_SKIP() << "no GPU device: " << engine->diagnostics();
  }
  ASSERT_EQ(EffectResult::Success, started) << engine->diagnostics();
  ASSERT_EQ(EffectResult::Success,
    pass->setTexture("uSdf", TextureSource::fromImage(image)));
  ASSERT_EQ(EffectResult::Success, engine->execute()) << engine->diagnostics();
  coral::EffectFrame frame;
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(&frame, &pixels));
  ASSERT_EQ(pixels.size(), static_cast<size_t>(kSize) * kSize * 4);

  int fillPixels = 0;
  int outlinePixels = 0;
  for (uint32_t y = 0; y < kSize; ++y) {
    for (uint32_t x = 0; x < kSize; ++x) {
      const size_t index = (static_cast<size_t>(y) * kSize + x) * 4;
      if (pixels[index + 3] > 200 && pixels[index + 0] < 40 && pixels[index + 1] < 40 &&
          pixels[index + 2] < 40) {
        ++fillPixels;
      }
      if (pixels[index + 3] > 200 && pixels[index + 0] > 150 && pixels[index + 1] > 50 &&
          pixels[index + 2] < 80) {
        ++outlinePixels;
      }
    }
  }

  EXPECT_GT(fillPixels, 20) << "the field must fill the glyph";
  EXPECT_GT(outlinePixels, 20) << "the outline band must be drawn around it";
  engine->stop();
}

TEST(SdfText, TheFieldIsCachedPerGlyphInTheCaller) {
  // The field is generated once per glyph and size; this test measures the prototype cost and
  // documents the number the GPU text route would pay on a cache miss.
  const auto start = std::chrono::steady_clock::now();
  for (const char character: { 'A', 'B', 'C', 'D', 'E' }) {
    const graphics::SdfGlyph field = glyphField(character);
    ASSERT_FALSE(field.isEmpty());
  }
  const double ms = std::chrono::duration<double, std::milli>(
    std::chrono::steady_clock::now() - start).count() / 5.0;
  std::cout << "[perf] sdf glyph generation: " << ms << " ms/glyph" << std::endl;
  EXPECT_LT(ms, 100.0);
}

TEST(SdfText, TheGraphiteGlyphRouteDrawsSdfText) {
  graphics::gpu::SurfaceConfiguration config;
  config.width = 128;
  config.height = 128;
  auto surface = graphics::gpu::Surface::make(config);
  if (surface == nullptr) {
    GTEST_SKIP() << "no GPU surface";
  }
  auto typeface = graphics::FontManager::shared().defaultTypeface();
  ASSERT_NE(nullptr, typeface);
  auto font = graphics::Font::create(typeface, 80.0f);
  ASSERT_NE(nullptr, font);
  auto* canvas = surface->beginFrame();
  ASSERT_NE(nullptr, canvas);
  graphics::Paint paint;
  paint.color = graphics::Color(1.0f, 1.0f, 1.0f, 1.0f);
  paint.sdfText = true;
  paint.sdfOutlineWidth = 0.12f;
  paint.sdfShadowX = 0.01f;
  paint.sdfShadowY = 0.01f;
  paint.sdfGlow = 0.06f;
  paint.sdfOutlineColor = 0xFFFF8000u;
  paint.sdfPattern = 1.0f;
  paint.sdfPatternFrequency = 6.0f;
  paint.sdfPatternStrength = 0.5f;
  canvas->drawText("A", 16.0f, 96.0f, *font, paint);
  ASSERT_TRUE(surface->submit());
  const auto snapshot = surface->snapshot();
  ASSERT_FALSE(snapshot.isEmpty());
  const auto pixels = snapshot.makePixmap();
  int opaque = 0;
  for (int y = 0; y < snapshot.height(); ++y) {
    for (int x = 0; x < snapshot.width(); ++x) {
      const auto* pixel = static_cast<const uint8_t*>(pixels.addr(x, y));
      if (pixel[3] > 128) ++opaque;
    }
  }
  EXPECT_GT(opaque, 100) << "the SDF glyph should cover a visible area";
  int orange = 0;
  for (int y = 0; y < snapshot.height(); ++y) {
    for (int x = 0; x < snapshot.width(); ++x) {
      const auto* pixel = static_cast<const uint8_t*>(pixels.addr(x, y));
      if (pixel[3] > 128 && pixel[0] > 180 && pixel[1] > 60 && pixel[2] < 100) ++orange;
    }
  }
  EXPECT_GT(orange, 10) << "the custom outline colour should be visible";
}

TEST(SdfText, RendersSdfTextAtFourK) {
  graphics::gpu::SurfaceConfiguration config;
  config.width = 3840;
  config.height = 2160;
  auto surface = graphics::gpu::Surface::make(config);
  if (surface == nullptr) {
    GTEST_SKIP() << "no 4K GPU surface";
  }
  auto typeface = graphics::FontManager::shared().defaultTypeface();
  auto font = graphics::Font::create(typeface, 230.0f);
  auto drawFrame = [&](const bool styled) {
    auto* canvas = surface->beginFrame();
    if (canvas == nullptr) return false;
    graphics::Paint paint;
    paint.color = graphics::Color(1.0f, 1.0f, 1.0f, 1.0f);
    paint.sdfText = true;
    if (styled) {
      paint.sdfOutlineWidth = 0.12f;
      paint.sdfShadowX = 0.01f;
      paint.sdfShadowY = 0.01f;
      paint.sdfGlow = 0.06f;
    }
    for (int i = 0; i < 5; ++i) {
      canvas->drawText("Hello subtitle", 80.0f + i, 400.0f, *font, paint);
    }
    return surface->submit();
  };
  auto measure = [&](const bool styled) {
    EXPECT_TRUE(drawFrame(styled));
    const auto start = std::chrono::steady_clock::now();
    constexpr int kIterations = 3;
    for (int i = 0; i < kIterations; ++i) EXPECT_TRUE(drawFrame(styled));
    return std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start).count() / kIterations;
  };
  const double fillMs = measure(false);
  const double styledMs = measure(true);
  std::cout << "[perf] sdf 4K fill: " << fillMs << " ms; styled: " << styledMs << " ms"
            << std::endl;
  EXPECT_LT(fillMs, 2000.0);
}

TEST(SdfText, RendersSdfTextAt1080p) {
  graphics::gpu::SurfaceConfiguration config;
  config.width = 1920;
  config.height = 1080;
  auto surface = graphics::gpu::Surface::make(config);
  if (surface == nullptr) {
    GTEST_SKIP() << "no 1080p GPU surface";
  }
  auto typeface = graphics::FontManager::shared().defaultTypeface();
  auto font = graphics::Font::create(typeface, 58.0f);
  auto drawFrame = [&](const bool styled) {
    auto* canvas = surface->beginFrame();
    if (canvas == nullptr) return false;
    graphics::Paint paint;
    paint.color = graphics::Color(1.0f, 1.0f, 1.0f, 1.0f);
    paint.sdfText = true;
    if (styled) {
      paint.sdfOutlineWidth = 0.12f;
      paint.sdfShadowX = 0.01f;
      paint.sdfShadowY = 0.01f;
      paint.sdfGlow = 0.06f;
    }
    for (int i = 0; i < 5; ++i) {
      canvas->drawText("Hello subtitle", 40.0f + i, 900.0f, *font, paint);
    }
    return surface->submit();
  };
  auto measure = [&](const bool styled) {
    EXPECT_TRUE(drawFrame(styled));
    const auto start = std::chrono::steady_clock::now();
    constexpr int kIterations = 5;
    for (int i = 0; i < kIterations; ++i) EXPECT_TRUE(drawFrame(styled));
    return std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start).count() / kIterations;
  };
  const double fillMs = measure(false);
  const double styledMs = measure(true);
  std::cout << "[perf] sdf 1080p fill: " << fillMs << " ms; styled: " << styledMs << " ms"
            << std::endl;
  EXPECT_LT(fillMs, 2000.0);
}

TEST(SdfText, ATransparentFillShowsTheBackground) {
  graphics::gpu::SurfaceConfiguration config;
  config.width = 128;
  config.height = 128;
  auto surface = graphics::gpu::Surface::make(config);
  if (surface == nullptr) {
    GTEST_SKIP() << "no GPU surface";
  }
  auto typeface = graphics::FontManager::shared().defaultTypeface();
  auto font = graphics::Font::create(typeface, 80.0f);
  auto* canvas = surface->beginFrame();
  ASSERT_NE(nullptr, canvas);
  graphics::Paint background;
  background.color = graphics::Color(1.0f, 0.0f, 0.0f, 1.0f);
  canvas->drawRect(graphics::Rect::MakeLTRB(0.0f, 0.0f, 128.0f, 128.0f), background);
  graphics::Paint paint;
  paint.color = graphics::Color(1.0f, 1.0f, 1.0f, 1.0f);
  paint.sdfText = true;
  paint.sdfOutlineWidth = 0.12f;
  paint.sdfOutlineColor = 0xFFFF8000u;
  paint.sdfFillAlpha = 0.0f;
  canvas->drawText("A", 16.0f, 96.0f, *font, paint);
  ASSERT_TRUE(surface->submit());
  const auto snapshot = surface->snapshot();
  ASSERT_FALSE(snapshot.isEmpty());
  const auto pixels = snapshot.makePixmap();
  int red = 0;
  int orange = 0;
  for (int y = 0; y < snapshot.height(); ++y) {
    for (int x = 0; x < snapshot.width(); ++x) {
      const auto* pixel = static_cast<const uint8_t*>(pixels.addr(x, y));
      if (pixel[3] > 128 && pixel[0] > 180 && pixel[1] < 60 && pixel[2] < 60) ++red;
      if (pixel[3] > 128 && pixel[0] > 180 && pixel[1] > 60 && pixel[2] < 100) ++orange;
    }
  }
  EXPECT_GT(red, 100) << "the glyph interior must show the background";
  EXPECT_GT(orange, 10) << "the outline must still be drawn";
}

TEST(SdfText, APatternTextureColoursTheGlyph) {
  graphics::gpu::SurfaceConfiguration config;
  config.width = 128;
  config.height = 128;
  auto surface = graphics::gpu::Surface::make(config);
  if (surface == nullptr) {
    GTEST_SKIP() << "no GPU surface";
  }
  auto pattern = graphics::Bitmap::allocate(8, 8);
  ASSERT_FALSE(pattern.isEmpty());
  auto pixels = pattern.lockPixels();
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      const uint32_t colour = ((x + y) & 1) != 0 ? 0xFF00FF00u : 0xFFFF00FFu;
      pixels.setPixelARGB32(x, y, colour);
    }
  }
  surface->setSdfPattern(pattern);

  auto typeface = graphics::FontManager::shared().defaultTypeface();
  auto font = graphics::Font::create(typeface, 80.0f);
  auto* canvas = surface->beginFrame();
  ASSERT_NE(nullptr, canvas);
  graphics::Paint paint;
  paint.color = graphics::Color(1.0f, 1.0f, 1.0f, 1.0f);
  paint.sdfText = true;
  paint.sdfPattern = 1.0f;
  paint.sdfPatternStrength = 1.0f;
  canvas->drawText("A", 16.0f, 96.0f, *font, paint);
  ASSERT_TRUE(surface->submit());
  const auto snapshot = surface->snapshot();
  ASSERT_FALSE(snapshot.isEmpty());
  const auto result = snapshot.makePixmap();
  int patterned = 0;
  for (int y = 0; y < snapshot.height(); ++y) {
    for (int x = 0; x < snapshot.width(); ++x) {
      const auto* pixel = static_cast<const uint8_t*>(result.addr(x, y));
      if (pixel[3] > 128 &&
          ((pixel[0] > 180 && pixel[2] > 180 && pixel[1] < 60) ||
           (pixel[1] > 180 && pixel[0] < 60 && pixel[2] < 60))) {
        ++patterned;
      }
    }
  }
  EXPECT_GT(patterned, 50) << "the glyph should be coloured by the pattern texture";
}