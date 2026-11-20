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
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file test_CanvasLayer.cpp
 *
 * Offscreen layers and path clips.
 *
 * Both exist for the same reason: a rectangle is not enough. Group opacity cannot be expressed by scaling each child's
 * alpha, because children that overlap composite twice. A clip cannot be expressed as a rectangle, because most clips
 * are not rectangles. The tests below are the cases where the approximation and the real thing differ -- a single shape
 * at half alpha looks the same either way, so testing that would prove nothing.
 */

#include <graphics/Canvas.h>
#include <graphics/Paint.h>
#include <graphics/Path.h>
#include <graphics/Surface.h>

#include <gtest/gtest.h>

namespace {

using graphics::BlendMode;
using graphics::Canvas;
using graphics::Color;
using graphics::Paint;
using graphics::Path;
using graphics::Point;
using graphics::Rect;
using graphics::Surface;

/** A surface with a canvas, and the pixel reads a test needs. */
class CanvasFixture {
  public:
  explicit CanvasFixture(const int size = 32) {
    surface = Surface::makeRaster(size, size);
    if (surface != nullptr) canvas = surface->getCanvas();
  }

  [[nodiscard]] uint32_t at(const int x, const int y) const { return surface->peekPixels().getPixelARGB32(x, y); }
  /** The colour channels are un-premultiplied, so coverage is read through the alpha. */
  [[nodiscard]] int alpha(const int x, const int y) const { return static_cast<int>((at(x, y) >> 24) & 0xFFu); }
  [[nodiscard]] int red(const int x, const int y) const { return static_cast<int>((at(x, y) >> 16) & 0xFFu); }
  [[nodiscard]] int green(const int x, const int y) const { return static_cast<int>((at(x, y) >> 8) & 0xFFu); }
  [[nodiscard]] int blue(const int x, const int y) const { return static_cast<int>(at(x, y) & 0xFFu); }

  std::unique_ptr<Surface> surface;
  std::shared_ptr<Canvas> canvas;
};

Paint solid(const uint32_t r, const uint32_t g, const uint32_t b, const float alpha = 1.0f) {
  Paint paint;
  paint.color = Color::MakeRGBA8(static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b),
                                 static_cast<uint8_t>(alpha * 255.0f + 0.5f));
  return paint;
}

/** A rectangle as a path, for the cases that must go through `drawPath` rather than `drawRect`. */
Path rectPath(const float left, const float top, const float right, const float bottom) {
  Path path;
  path.moveTo(Point {left, top});
  path.lineTo(Point {right, top});
  path.lineTo(Point {right, bottom});
  path.lineTo(Point {left, bottom});
  path.close();
  return path;
}

} // namespace anonymous

// ==================================================================================================
// saveLayer
// ==================================================================================================

TEST(CanvasLayer, CompositesTheLayerOntoWhatWasAlreadyThere) {
  CanvasFixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // A red background, then a blue layer over it at full alpha: the layer wins where it covers.
  fixture.canvas->drawColor(Color::MakeRGBA8(255, 0, 0, 255));
  fixture.canvas->saveLayer(Rect::MakeLTRB(0, 0, 16, 32), solid(0, 0, 255));
  fixture.canvas->drawColor(Color::MakeRGBA8(0, 0, 255, 255));
  fixture.canvas->restore();

  EXPECT_EQ(fixture.blue(8, 16), 255) << "inside the layer's bounds";
  EXPECT_EQ(fixture.red(24, 16), 255) << "outside them, the background";
}

TEST(CanvasLayer, AppliesTheGroupAlphaOnceRatherThanPerShape) {
  // This is the whole point. Two overlapping opaque rectangles in a group at half alpha:
  //
  //   * with a real layer, the group is composited once, so the overlap is the same 50% as the rest;
  //   * scaling each child's alpha instead composites the overlap twice, giving 75% -- visibly darker.
  //
  // A test that drew a single shape could not tell the two apart, which is why it would be no test at all.
  CanvasFixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  Paint half = solid(255, 0, 0, 0.5f);
  fixture.canvas->saveLayer(Rect::MakeLTRB(0, 0, 32, 32), half);
  fixture.canvas->drawRect(Rect::MakeLTRB(4, 4, 28, 28), solid(255, 0, 0));
  fixture.canvas->drawRect(Rect::MakeLTRB(4, 4, 28, 28), solid(255, 0, 0));
  fixture.canvas->restore();

  // One rectangle's worth of coverage at half alpha: 128, not the 191 that a double composite would give.
  EXPECT_NEAR(fixture.alpha(16, 16), 128, 3) << "the overlap, composited once";
  EXPECT_NEAR(fixture.alpha(6, 16), 128, 3) << "and a single-covered part is the same";
}

TEST(CanvasLayer, LeavesTheCanvasUntouchedWhenTheLayerIsTransparent) {
  CanvasFixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  fixture.canvas->drawColor(Color::MakeRGBA8(0, 255, 0, 255));
  fixture.canvas->saveLayer(Rect::MakeLTRB(0, 0, 32, 32), solid(255, 0, 0));
  // Nothing drawn inside the layer.
  fixture.canvas->restore();

  EXPECT_EQ(fixture.green(16, 16), 255) << "the background survived an empty layer";
  EXPECT_EQ(fixture.red(16, 16), 0);
}

TEST(CanvasLayer, ZeroAlphaLayerDrawsNothing) {
  CanvasFixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  fixture.canvas->drawColor(Color::MakeRGBA8(0, 255, 0, 255));
  fixture.canvas->saveLayerAlpha(Rect::MakeLTRB(0, 0, 32, 32), 0);
  fixture.canvas->drawRect(Rect::MakeLTRB(0, 0, 32, 32), solid(255, 0, 0));
  fixture.canvas->restore();

  EXPECT_EQ(fixture.green(16, 16), 255) << "the background is unchanged";
  EXPECT_EQ(fixture.red(16, 16), 0);
}

TEST(CanvasLayer, SaveLayerAlphaMultipliesTheLayersOwnCoverage) {
  CanvasFixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  fixture.canvas->saveLayerAlpha(Rect::MakeLTRB(0, 0, 32, 32), 128);
  fixture.canvas->drawRect(Rect::MakeLTRB(0, 0, 32, 32), solid(0, 0, 255));
  fixture.canvas->restore();

  EXPECT_NEAR(fixture.alpha(16, 16), 128, 3) << "half of an opaque layer";
  EXPECT_NEAR(fixture.blue(16, 16), 255, 3) << "and still blue, un-premultiplied";
}

TEST(CanvasLayer, BoundsClipTheLayer) {
  CanvasFixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // The layer's bounds are a clip: a draw that runs past them is cut off at the composite.
  fixture.canvas->saveLayer(Rect::MakeLTRB(0, 0, 16, 32), solid(255, 0, 0));
  fixture.canvas->drawRect(Rect::MakeLTRB(0, 0, 32, 32), solid(255, 0, 0));
  fixture.canvas->restore();

  EXPECT_EQ(fixture.red(8, 16), 255) << "inside the bounds";
  EXPECT_EQ(fixture.alpha(24, 16), 0) << "past them, nothing";
}

TEST(CanvasLayer, RestoresTheTargetSoLaterDrawsReachTheCanvas) {
  // A layer that is popped must leave the canvas pointing at its own pixels again. Getting this wrong is silent: the
  // draws after the layer go into an offscreen buffer that is never composited, and simply vanish.
  CanvasFixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  fixture.canvas->saveLayer(Rect::MakeLTRB(0, 0, 32, 32), solid(255, 0, 0));
  fixture.canvas->drawRect(Rect::MakeLTRB(0, 0, 32, 32), solid(0, 0, 255));
  fixture.canvas->restore();

  fixture.canvas->drawRect(Rect::MakeLTRB(0, 0, 16, 16), solid(0, 255, 0));

  EXPECT_EQ(fixture.blue(8, 24), 255) << "the layer was composited";
  EXPECT_EQ(fixture.green(8, 8), 255) << "and a later draw reached the canvas";
}

TEST(CanvasLayer, Nests) {
  CanvasFixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // An outer layer at half alpha containing an inner one at half alpha: the inner composites into the outer, and the
  // outer composites into the canvas, so the result is a quarter -- not a half, and not a whole.
  fixture.canvas->saveLayerAlpha(Rect::MakeLTRB(0, 0, 32, 32), 128);
  fixture.canvas->saveLayerAlpha(Rect::MakeLTRB(0, 0, 32, 32), 128);
  fixture.canvas->drawRect(Rect::MakeLTRB(0, 0, 32, 32), solid(255, 0, 0));
  fixture.canvas->restore();
  fixture.canvas->restore();

  EXPECT_NEAR(fixture.alpha(16, 16), 64, 4) << "half of a half";
}

TEST(CanvasLayer, AnOverRestoreDoesNotUnbalanceTheLayerStack) {
  // `restore` on the base state is ignored, which the header promises. That has to hold for layers too, or an extra
  // restore pops a layer early and the picture loses whatever was drawn into it.
  CanvasFixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  fixture.canvas->restore(); // nothing to restore
  fixture.canvas->saveLayer(Rect::MakeLTRB(0, 0, 32, 32), solid(255, 0, 0));
  fixture.canvas->drawRect(Rect::MakeLTRB(0, 0, 32, 32), solid(0, 0, 255));
  fixture.canvas->restore();
  fixture.canvas->restore(); // and one more

  EXPECT_EQ(fixture.blue(16, 16), 255) << "the layer was composited exactly once";
}

TEST(CanvasLayer, BlendModeDecidesHowTheLayerMeetsTheCanvas) {
  CanvasFixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // A white background and a red layer with multiply: red times white is red.
  fixture.canvas->drawColor(Color::MakeRGBA8(255, 255, 255, 255));
  Paint multiply = solid(0, 0, 255);
  multiply.blendMode = BlendMode::kMultiply;
  fixture.canvas->saveLayer(Rect::MakeLTRB(0, 0, 32, 32), multiply);
  fixture.canvas->drawRect(Rect::MakeLTRB(0, 0, 32, 32), solid(0, 0, 255));
  fixture.canvas->restore();

  // Blue over white under multiply stays blue in the blue channel, and the red channel comes down.
  EXPECT_EQ(fixture.blue(16, 16), 255);
  EXPECT_EQ(fixture.red(16, 16), 0) << "multiply took the red channel to zero";
}

TEST(CanvasLayer, ReportsItsClipThroughTheQueryMethods) {
  CanvasFixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  EXPECT_FALSE(fixture.canvas->isClipEmpty());
  // Bounds entirely outside the canvas leave the intersection empty. Note that a rectangle with its corners the other
  // way round is *not* the way to write this: the bounds are normalised into a bounding box, exactly as `clipRect`
  // transforms its rectangle, so `MakeLTRB(10, 10, 5, 5)` is the same region as `MakeLTRB(5, 5, 10, 10)`.
  fixture.canvas->saveLayer(Rect::MakeLTRB(40, 40, 50, 50), solid(255, 0, 0));
  EXPECT_TRUE(fixture.canvas->isClipEmpty());
  fixture.canvas->restore();
  EXPECT_FALSE(fixture.canvas->isClipEmpty()) << "and the clip comes back with the state";
}

// ==================================================================================================
// clipPath
// ==================================================================================================

TEST(CanvasClipPath, ClipsToATriangle) {
  CanvasFixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // A triangle from the top-left corner, and a red rectangle over the whole canvas.
  Path triangle;
  triangle.moveTo(Point {0, 0});
  triangle.lineTo(Point {32, 0});
  triangle.lineTo(Point {0, 32});
  triangle.close();

  fixture.canvas->save();
  fixture.canvas->clipPath(triangle);
  fixture.canvas->drawRect(Rect::MakeLTRB(0, 0, 32, 32), solid(255, 0, 0));
  fixture.canvas->restore();

  EXPECT_EQ(fixture.red(4, 4), 255) << "inside the triangle";
  EXPECT_EQ(fixture.alpha(28, 28), 0) << "outside it, past the hypotenuse";
  EXPECT_EQ(fixture.alpha(28, 4), 0) << "and in the other corner";
}

TEST(CanvasClipPath, AntialiasesItsEdge) {
  // A shape clipped to a diagonal has to fade at the diagonal rather than stepping, or the clip shows a staircase where
  // the shape inside it is smooth -- which is the visible difference between a coverage mask and a rectangle test.
  CanvasFixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  Path triangle;
  triangle.moveTo(Point {0, 0});
  triangle.lineTo(Point {32, 0});
  triangle.lineTo(Point {0, 32});
  triangle.close();

  fixture.canvas->save();
  fixture.canvas->clipPath(triangle);
  fixture.canvas->drawRect(Rect::MakeLTRB(0, 0, 32, 32), solid(255, 0, 0));
  fixture.canvas->restore();

  // Along the hypotenuse the coverage has to take intermediate values somewhere. A hard clip gives only 0 and 255.
  bool sawPartial = false;
  for (int i = 0; i < 32; ++i) {
    const int a = fixture.alpha(i, 32 - i - 1);
    if (a > 8 && a < 247) sawPartial = true;
  }
  EXPECT_TRUE(sawPartial) << "a soft edge somewhere along the diagonal";
}

TEST(CanvasClipPath, IntersectsWithWhatWasAlreadyClipped) {
  CanvasFixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // A rectangular clip on the left half, then a triangular clip covering the top-left. The result is their
  // intersection: clipping is never a union and never a replacement.
  fixture.canvas->save();
  fixture.canvas->clipRect(Rect::MakeLTRB(0, 0, 16, 32));

  Path triangle;
  triangle.moveTo(Point {0, 0});
  triangle.lineTo(Point {32, 0});
  triangle.lineTo(Point {0, 32});
  triangle.close();
  fixture.canvas->clipPath(triangle);

  fixture.canvas->drawRect(Rect::MakeLTRB(0, 0, 32, 32), solid(255, 0, 0));
  fixture.canvas->restore();

  EXPECT_EQ(fixture.red(4, 4), 255) << "inside both";
  EXPECT_EQ(fixture.alpha(20, 4), 0) << "inside the triangle but past the rectangle";
  EXPECT_EQ(fixture.alpha(4, 28), 0) << "inside the rectangle but outside the triangle";
}

TEST(CanvasClipPath, TwoPathClipsIntersect) {
  CanvasFixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // The left half and the top half. Their intersection is the top-left quarter.
  Path left;
  left.moveTo(Point {0, 0});
  left.lineTo(Point {16, 0});
  left.lineTo(Point {16, 32});
  left.lineTo(Point {0, 32});
  left.close();

  Path top;
  top.moveTo(Point {0, 0});
  top.lineTo(Point {32, 0});
  top.lineTo(Point {32, 16});
  top.lineTo(Point {0, 16});
  top.close();

  fixture.canvas->save();
  fixture.canvas->clipPath(left);
  fixture.canvas->clipPath(top);
  fixture.canvas->drawRect(Rect::MakeLTRB(0, 0, 32, 32), solid(255, 0, 0));
  fixture.canvas->restore();

  EXPECT_EQ(fixture.red(8, 8), 255) << "the intersection";
  EXPECT_EQ(fixture.alpha(24, 8), 0) << "right of it";
  EXPECT_EQ(fixture.alpha(8, 24), 0) << "below it";
}

TEST(CanvasClipPath, IsUndoneByRestore) {
  CanvasFixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  Path small = rectPath(0, 0, 8, 8);
  fixture.canvas->save();
  fixture.canvas->clipPath(small);
  fixture.canvas->restore();

  EXPECT_FALSE(fixture.canvas->hasClipPath()) << "the mask goes with the state";

  fixture.canvas->drawRect(Rect::MakeLTRB(0, 0, 32, 32), solid(255, 0, 0));
  EXPECT_EQ(fixture.red(24, 24), 255) << "so the whole rectangle draws";
}

TEST(CanvasClipPath, ReportsWhetherAShapedClipIsInForce) {
  CanvasFixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  EXPECT_FALSE(fixture.canvas->hasClipPath());
  fixture.canvas->clipRect(Rect::MakeLTRB(0, 0, 16, 16));
  EXPECT_FALSE(fixture.canvas->hasClipPath()) << "a rectangle is not a path clip";

  fixture.canvas->save();
  fixture.canvas->clipPath(rectPath(0, 0, 8, 8));
  EXPECT_TRUE(fixture.canvas->hasClipPath());
  fixture.canvas->restore();
}

TEST(CanvasClipPath, HonoursThePathsFillType) {
  // An even-odd clip with a hole keeps the hole: the fill type decides what is inside, exactly as it does for a fill.
  CanvasFixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  Path ring;
  ring.moveTo(Point {0, 0});
  ring.lineTo(Point {32, 0});
  ring.lineTo(Point {32, 32});
  ring.lineTo(Point {0, 32});
  ring.close();
  // The inner square, wound the same way, which only even-odd turns into a hole.
  ring.moveTo(Point {8, 8});
  ring.lineTo(Point {24, 8});
  ring.lineTo(Point {24, 24});
  ring.lineTo(Point {8, 24});
  ring.close();
  ring.setFillType(graphics::FillType::EvenOdd);

  fixture.canvas->save();
  fixture.canvas->clipPath(ring);
  fixture.canvas->drawRect(Rect::MakeLTRB(0, 0, 32, 32), solid(255, 0, 0));
  fixture.canvas->restore();

  EXPECT_EQ(fixture.red(2, 16), 255) << "inside the ring";
  EXPECT_EQ(fixture.alpha(16, 16), 0) << "and the hole stays a hole";
}

TEST(CanvasClipPath, ClipsEveryKindOfDraw) {
  // The clip is asked of the canvas by each draw rather than tested by each of them, so a fill, a flood and an image all
  // obey it. A draw that forgot would be the one case the tests above miss.
  CanvasFixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  Path left = rectPath(0, 0, 16, 32);
  fixture.canvas->save();
  fixture.canvas->clipPath(left);
  fixture.canvas->drawColor(Color::MakeRGBA8(255, 0, 0, 255));
  fixture.canvas->restore();

  EXPECT_EQ(fixture.red(8, 16), 255) << "drawColor obeys the clip";
  EXPECT_EQ(fixture.alpha(24, 16), 0) << "and stops at it";
}
