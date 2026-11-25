// Copyright (c) Coral. All rights reserved.
//
// A damage update is normally recorded from the full page, so every pixel has a draw behind it. A window that only
// redraws the video rectangle records a partial list instead; the pixels outside the damage have no command in that
// frame and must come from the previous submit. This isolates that difference from the UI and the video pipeline.

#include <graphics/Bitmap.h>
#include <graphics/Canvas.h>
#include <graphics/Paint.h>
#include <graphics/Pixmap.h>
#include <gpu/Surface.h>

#include <gtest/gtest.h>

TEST(GpuSurface, DamageUpdateFromAPartialRecordingKeepsTheOtherPixels) {
  auto surface = graphics::gpu::Surface::make(64, 64);
  if (surface == nullptr) {
    GTEST_SKIP() << "no GPU device could be brought up in this environment";
  }

  {
    graphics::Canvas* canvas = surface->beginFrame();
    ASSERT_NE(canvas, nullptr);
    canvas->clear(graphics::Color::MakeRGBA8(0, 0, 255, 255));
    ASSERT_TRUE(surface->submit());
  }

  {
    graphics::Canvas* canvas = surface->beginFrame();
    ASSERT_NE(canvas, nullptr);
    graphics::Paint paint;
    paint.color = graphics::Color::Red();
    paint.style = graphics::Paint::Style::kFill;
    canvas->drawRect(graphics::Rect::MakeWH(16.0f, 16.0f), paint);
    ASSERT_TRUE(surface->submitDamage(graphics::Rect::MakeWH(16.0f, 16.0f)));
  }

  const graphics::Bitmap bitmap = surface->snapshot();
  ASSERT_FALSE(bitmap.isEmpty());
  const graphics::Pixmap pixels = bitmap.makePixmap();
  EXPECT_EQ(pixels.getPixelARGB32(8, 8) & 0x00FFFFFFu, 0x00FF0000u) << "the damage is red";
  EXPECT_EQ(pixels.getPixelARGB32(40, 40) & 0x00FFFFFFu, 0x000000FFu)
    << "pixels outside the damage keep the previous submit";
}
