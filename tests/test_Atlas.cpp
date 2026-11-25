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

#include <graphics/Bitmap.h>
#include <graphics/Canvas.h>
#include <graphics/Color.h>
#include <graphics/Paint.h>
#include <graphics/GlyphAtlas.h>
#include <graphics/Pixmap.h>
#include <gpu/DrawList.h>
#include <gpu/Surface.h>

#include <gtest/gtest.h>

namespace {

uint32_t pixelAt(const graphics::Bitmap& bitmap, const int x, const int y) {
  return bitmap.makePixmap().getPixelARGB32(x, y);
}

} // namespace

TEST(AtlasPacking, ADifferentPageIsUsedWhenTheFirstIsFull) {
  graphics::GlyphAtlas& atlas = graphics::GlyphAtlas::instance();
  graphics::GlyphBitmap first;
  first.pixels = graphics::Bitmap::allocate(700, 700);
  first.width = 700.0f;
  first.height = 700.0f;
  graphics::GlyphBitmap second = first;
  const auto firstEntry = atlas.add(first, 900001u, 14.0f);
  const auto secondEntry = atlas.add(second, 900002u, 14.0f);
  ASSERT_TRUE(firstEntry.has_value());
  ASSERT_TRUE(secondEntry.has_value());
  EXPECT_NE(firstEntry->page, secondEntry->page);
  EXPECT_GE(atlas.pageCount(), 2u);
  EXPECT_EQ(atlas.bytes(), atlas.pageCount() * 1024u * 1024u * 4u);
}

TEST(AtlasRecording, AtlasCellsShareOneRecordedImage) {
  auto list = std::make_shared<graphics::gpu::DrawList>();
  auto canvas = graphics::Canvas::createRecordingBackend(32, 32, nullptr, list);
  ASSERT_NE(canvas, nullptr);
  graphics::Bitmap atlas = graphics::Bitmap::allocate(8, 8);
  graphics::Paint paint;
  paint.color = graphics::Color::White();
  canvas->drawBitmapRect(atlas, graphics::Rect::MakeLTRB(0.0f, 0.0f, 4.0f, 4.0f),
    graphics::Rect::MakeXYWH(0.0f, 0.0f, 4.0f, 4.0f), paint);
  canvas->drawBitmapRect(atlas, graphics::Rect::MakeLTRB(4.0f, 0.0f, 8.0f, 4.0f),
    graphics::Rect::MakeXYWH(4.0f, 0.0f, 4.0f, 4.0f), paint);
  ASSERT_EQ(list->images.size(), 1u) << "the atlas is registered once per recording";
  ASSERT_EQ(list->commands.size(), 2u);
  EXPECT_EQ(list->commands[0].state.image, list->commands[1].state.image);
  EXPECT_NE(list->commands[0].texCoords[0].x, list->commands[1].texCoords[0].x);
}

TEST(AtlasGpu, ASubRectangleSamplesTheAtlasCell) {
  auto surface = graphics::gpu::Surface::make(32, 32);
  if (surface == nullptr) GTEST_SKIP() << "no GPU device could be brought up in this environment";

  graphics::Bitmap atlas = graphics::Bitmap::allocate(4, 2);
  {
    graphics::Pixmap pixels = atlas.lockPixels();
    for (int y = 0; y < 2; ++y) {
      for (int x = 0; x < 2; ++x) pixels.setPixelARGB32(x, y, 0xFFFF0000u);
      for (int x = 2; x < 4; ++x) pixels.setPixelARGB32(x, y, 0xFF0000FFu);
    }
  }
  graphics::Canvas* canvas = surface->beginFrame();
  ASSERT_NE(canvas, nullptr);
  canvas->clear(graphics::Color::MakeRGBA8(0, 0, 0, 0));
  graphics::Paint white;
  white.color = graphics::Color::White();
  canvas->drawBitmapRect(atlas, graphics::Rect::MakeLTRB(0.0f, 0.0f, 2.0f, 2.0f),
    graphics::Rect::MakeXYWH(8.0f, 8.0f, 4.0f, 4.0f), white);
  ASSERT_TRUE(surface->submit()) << surface->diagnostics();
  const graphics::Bitmap pixels = surface->snapshot();
  ASSERT_FALSE(pixels.isEmpty());
  const auto centre = pixelAt(pixels, 9, 9);
  EXPECT_EQ((centre >> 16) & 0xFFu, 255u) << "the red atlas cell is sampled";
  EXPECT_EQ(centre & 0xFFu, 0u) << "the blue neighbour is not sampled";
}
