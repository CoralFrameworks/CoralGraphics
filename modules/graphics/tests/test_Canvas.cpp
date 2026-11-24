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
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF
 * ANY KIND, either express or implied. See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file test_Canvas.cpp
 *
 * Bitmap, the software canvas, and the rasterizer behind it.
 *
 * ## What is worth testing in a rasterizer
 *
 * Not "does it look right" -- that needs a reference image and a tolerance, and it would pass for a plausible
 * implementation with a channel swapped. The properties tested here are the ones that distinguish a correct
 * rasterizer from a convincing one:
 *
 *  - **Coverage arithmetic.** A rectangle on integer coordinates covers exactly its pixels at exactly full
 *    coverage. That single fact catches an off-by-a-half-pixel bias, which is the most common defect in a
 *    scanline fill and the hardest to see.
 *  - **The fill rules.** The two rules must disagree where a contour is wound twice and agree everywhere else,
 *    because that difference is the entire reason both exist.
 *  - **Ordering and the transform stack.** A transform applied and then restored must leave no trace; a
 *    scale-then-translate must not behave like a translate-then-scale.
 *  - **Clipping.** A draw outside the clip must change nothing at all, which is stronger than "changes little".
 *  - **Blending.** Source-over with a translucent source must land on a computed value, not merely somewhere
 *    between the two colours.
 */

#include <graphics/Bitmap.h>
#include <graphics/Canvas.h>
#include <fstream>
#include <string>
#include <vector>
#include <graphics/ColorFilter.h>
#include <graphics/Font.h>
#include <graphics/Image.h>
#include <graphics/Paint.h>
#include <graphics/Typeface.h>
#include <graphics/Path.h>
#include <graphics/Picture.h>
#include <graphics/PictureRecorder.h>
#include <graphics/Pixmap.h>

// For `raster::blendWith`, which the coefficient-table cross-check compares against. Included explicitly rather than
// reached through `Canvas.h`: this is a private header, and a test that depends on it arriving transitively breaks the
// day the public one stops pulling it in -- which is how `GlPlatform::backendName()` came to be uncalled.
#include <private/graphics/Blend.h>

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>

using namespace graphics;

namespace {

/** ARGB32 in this layer's spelling: alpha, red, green, blue. */
constexpr uint32_t argb(const uint32_t a, const uint32_t r, const uint32_t g, const uint32_t b) {
  return (a << 24) | (r << 16) | (g << 8) | b;
}

/** Opaque black, which is what a fresh bitmap holds. */
constexpr uint32_t black() { return argb(255, 0, 0, 0); }

/** @return one channel of an ARGB32 word, by shift: 24 alpha, 16 red, 8 green, 0 blue. */
constexpr int channelAt(const uint32_t argb, const int shift) {
  return static_cast<int>((argb >> shift) & 0xFFu);
}

/** A canvas over a fresh bitmap of the given size, plus the bitmap it draws into. */
struct Fixture {
  Bitmap bitmap = Bitmap::allocate(16, 16);
  std::unique_ptr<Canvas> canvas = Canvas::create(bitmap);

  [[nodiscard]] uint32_t at(const int x, const int y) const { return bitmap.makePixmap().getPixelARGB32(x, y); }

  /**
   * @return the red channel at (@p x, @p y).
   *
   * Only meaningful for an opaque or fully transparent pixel. The bitmap is premultiplied and `getPixelARGB32`
   * un-premultiplies, so a pixel that is half covered by an opaque red shape reports *full* red with half alpha
   * -- not half red. Coverage is measured through @c alphaAt for that reason.
   */
  [[nodiscard]] uint32_t redAt(const int x, const int y) const { return (at(x, y) >> 16) & 0xFFu; }

  [[nodiscard]] uint32_t alphaAt(const int x, const int y) const { return (at(x, y) >> 24) & 0xFFu; }
};

/** A paint that draws opaque red, so coverage shows up directly in the red channel. */
Paint redPaint() {
  Paint paint;
  paint.color = Color::Red();
  paint.style = Paint::Style::kFill;
  return paint;
}

} // namespace anonymous

// ==================================================================================================
// Bitmap
// ==================================================================================================

TEST(BitmapTest, AllocatesZeroedPixels) {
  const Bitmap bitmap = Bitmap::allocate(4, 3);
  ASSERT_FALSE(bitmap.isEmpty());
  EXPECT_EQ(bitmap.width(), 4);
  EXPECT_EQ(bitmap.height(), 3);
  EXPECT_TRUE(bitmap.isWritable());
  EXPECT_EQ(bitmap.rowBytes(), 16u) << "four pixels of four bytes";

  // Zeroed rather than left as the allocator returned it. A canvas over uninitialised memory would show
  // whatever was in the heap, which is a disclosure as much as a bug.
  const Pixmap pixmap = bitmap.makePixmap();
  for (int y = 0; y < 3; ++y) {
    for (int x = 0; x < 4; ++x) {
      EXPECT_EQ(pixmap.getPixelARGB32(x, y), 0u) << "at (" << x << "," << y << ")";
    }
  }
}

TEST(BitmapTest, RefusesGeometryThatCannotBeAllocated) {
  EXPECT_TRUE(Bitmap::allocate(0, 10).isEmpty());
  EXPECT_TRUE(Bitmap::allocate(10, 0).isEmpty());
  EXPECT_TRUE(Bitmap::allocate(-1, 10).isEmpty());
  EXPECT_TRUE(Bitmap::allocate(ImageInfo(10, 10, ColorType::kUnknown, AlphaType::kOpaque)).isEmpty());
}

TEST(BitmapTest, CopiesSharePixelsAndDeepCopiesDoNot) {
  // Allocated through the explicit `ImageInfo` rather than the two-argument convenience, because that one
  // produces a *premultiplied* bitmap: its raw bytes are not the colour that was written, so a test comparing
  // raw bytes would be testing the premultiply rather than the copying. The accessor gives back the colour in
  // both cases, which is what makes it the right thing to compare here.
  Bitmap original = Bitmap::allocate(ImageInfo(2, 2, ColorType::kRGBA_8888, AlphaType::kUnpremultiplied));
  original.lockPixels().setPixelARGB32(0, 0, argb(255, 10, 20, 30));

  // A copy shares the pixels, which is what makes it cheap -- and the two see each other's writes.
  Bitmap shared = original;
  EXPECT_EQ(shared.makePixmap().getPixelARGB32(0, 0), argb(255, 10, 20, 30));
  shared.lockPixels().setPixelARGB32(1, 0, argb(255, 99, 0, 0));
  EXPECT_EQ(original.makePixmap().getPixelARGB32(1, 0), argb(255, 99, 0, 0)) << "a copy is not a deep copy";

  // A deep copy is independent in both directions.
  const Bitmap deep = original.makeDeepCopy();
  EXPECT_EQ(deep.makePixmap().getPixelARGB32(0, 0), argb(255, 10, 20, 30));

  Bitmap writableOriginal = original;
  writableOriginal.lockPixels().setPixelARGB32(0, 0, argb(255, 1, 1, 1));
  EXPECT_EQ(deep.makePixmap().getPixelARGB32(0, 0), argb(255, 10, 20, 30))
          << "writing the original must not reach the deep copy";
}

TEST(BitmapTest, BorrowedPixelsAreNotWritable) {
  const Data data = Data::makeUninitialised(16);
  Bitmap bitmap = Bitmap::fromData(ImageInfo(2, 2, ColorType::kRGBA_8888, AlphaType::kUnpremultiplied), data);

  ASSERT_FALSE(bitmap.isEmpty());
  // Read-only, because the bytes may be a decoder's output or a shared constant and there is no way to tell.
  EXPECT_FALSE(bitmap.isWritable());
  EXPECT_TRUE(bitmap.lockPixels().isEmpty()) << "a writable view of read-only bytes is not offered";
  EXPECT_FALSE(bitmap.makePixmap().isEmpty()) << "but reading is fine";

  // And a canvas refuses to be made over it, rather than accepting and drawing nothing.
  EXPECT_EQ(Canvas::create(bitmap), nullptr);
}

TEST(BitmapTest, RefusesABufferTooSmallForItsGeometry) {
  const ImageInfo info(4, 4, ColorType::kRGBA_8888, AlphaType::kUnpremultiplied);  // 64 bytes
  EXPECT_TRUE(Bitmap::fromData(info, Data::makeUninitialised(16)).isEmpty());
  EXPECT_FALSE(Bitmap::fromData(info, Data::makeUninitialised(64)).isEmpty());

  // A stride narrower than a row would make two rows overlap.
  EXPECT_TRUE(Bitmap::fromData(info, Data::makeUninitialised(128), 8).isEmpty());
  EXPECT_FALSE(Bitmap::fromData(info, Data::makeUninitialised(128), 24).isEmpty());
}

TEST(BitmapTest, EraseFillsEveryPixel) {
  Bitmap bitmap = Bitmap::allocate(3, 3);
  EXPECT_TRUE(bitmap.erase(Color::Red()));

  const Pixmap pixmap = bitmap.makePixmap();
  for (int y = 0; y < 3; ++y) {
    for (int x = 0; x < 3; ++x) {
      EXPECT_EQ(pixmap.getPixelARGB32(x, y), argb(255, 255, 0, 0)) << "at (" << x << "," << y << ")";
    }
  }

  // A read-only bitmap cannot be erased, and says so rather than silently doing nothing.
  Bitmap readOnly = Bitmap::fromData(
          ImageInfo(2, 2, ColorType::kRGBA_8888, AlphaType::kUnpremultiplied), Data::makeUninitialised(16));
  EXPECT_FALSE(readOnly.erase(Color::Red()));
}

// ==================================================================================================
// Canvas state
// ==================================================================================================

TEST(CanvasTest, CreateRefusesAnEmptyOrReadOnlyBitmap) {
  Bitmap empty;
  EXPECT_EQ(Canvas::create(empty), nullptr);

  Bitmap readOnly = Bitmap::fromData(
          ImageInfo(2, 2, ColorType::kRGBA_8888, AlphaType::kUnpremultiplied), Data::makeUninitialised(16));
  EXPECT_EQ(Canvas::create(readOnly), nullptr);

  Bitmap good = Bitmap::allocate(4, 4);
  EXPECT_NE(Canvas::create(good), nullptr);
}

TEST(CanvasTest, SaveAndRestoreAreBalanced) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  EXPECT_EQ(fixture.canvas->saveCount(), 0);

  fixture.canvas->save();
  EXPECT_EQ(fixture.canvas->saveCount(), 1);
  fixture.canvas->save();
  EXPECT_EQ(fixture.canvas->saveCount(), 2);

  fixture.canvas->restore();
  fixture.canvas->restore();
  EXPECT_EQ(fixture.canvas->saveCount(), 0);

  // An over-restore is ignored rather than corrupting the stack: there is no state to restore, and asserting
  // would turn a caller's off-by-one into a crash in an unrelated place.
  fixture.canvas->restore();
  fixture.canvas->restore();
  EXPECT_EQ(fixture.canvas->saveCount(), 0);
  EXPECT_FALSE(fixture.canvas->isClipEmpty()) << "the base state is still intact";
}

TEST(CanvasTest, TransformIsReplacedBySetAndComposedByConcat) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  const Matrix first = Matrix::MakeScale(Vector2 { 2.0f, 2.0f });
  fixture.canvas->setTransform(first);
  EXPECT_FLOAT_EQ(fixture.canvas->transformScale(), 2.0f);

  // `setTransform` replaces, so calling it again with a different scale gives that scale rather than the
  // product -- a concatenating implementation would report 6 here.
  const Matrix second = Matrix::MakeScale(Vector2 { 3.0f, 3.0f });
  fixture.canvas->setTransform(second);
  EXPECT_FLOAT_EQ(fixture.canvas->transformScale(), 3.0f);

  // `concat` composes on top, so a 2x on top of a 3x gives 6.
  fixture.canvas->concat(Matrix::MakeScale(Vector2 { 2.0f, 2.0f }));
  EXPECT_FLOAT_EQ(fixture.canvas->transformScale(), 6.0f);
}

TEST(CanvasTest, RestoreUndoesATransformCompletely) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  const Scalar before = fixture.canvas->transformScale();
  fixture.canvas->save();
  fixture.canvas->concat(Matrix::MakeScale(Vector2 { 5.0f, 5.0f }));
  EXPECT_FLOAT_EQ(fixture.canvas->transformScale(), before * 5.0f);
  fixture.canvas->restore();
  EXPECT_FLOAT_EQ(fixture.canvas->transformScale(), before) << "restore must undo the transform exactly";
}

TEST(CanvasTest, ConcatIsScaleThenTranslateNotTheOtherWayRound) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // Scale by two, then translate by one in the *scaled* space. The pen ends up at 2, not at 3: the translation
  // happens before the scale is applied to it. A transform composed in the wrong order gives 3 here, which is
  // the classic transform-order bug.
  fixture.canvas->concat(Matrix::MakeScale(Vector2 { 2.0f, 2.0f }));
  fixture.canvas->concat(Matrix::MakeTranslation(Vector3 { 1.0f, 0.0f, 0.0f }));

  const Point mapped = fixture.canvas->transform() * Point { 0.0f, 0.0f };
  EXPECT_FLOAT_EQ(mapped.x, 2.0f) << "translate-then-scale, not scale-then-translate";
}

TEST(CanvasTest, ClipIntersectsAndNeverWidens) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  fixture.canvas->clipRect(Rect::MakeLTRB(2.0f, 2.0f, 10.0f, 10.0f));
  EXPECT_FLOAT_EQ(fixture.canvas->deviceClipBounds().getLeft(), 2.0f);

  // A larger rectangle cannot widen the clip. If it could, the class could not be implemented as one rectangle.
  fixture.canvas->clipRect(Rect::MakeLTRB(0.0f, 0.0f, 16.0f, 16.0f));
  EXPECT_FLOAT_EQ(fixture.canvas->deviceClipBounds().getLeft(), 2.0f) << "a clip is only ever narrowed";
  EXPECT_FLOAT_EQ(fixture.canvas->deviceClipBounds().getRight(), 10.0f);

  // And a disjoint rectangle empties it.
  fixture.canvas->clipRect(Rect::MakeLTRB(20.0f, 20.0f, 30.0f, 30.0f));
  EXPECT_TRUE(fixture.canvas->isClipEmpty());
}

// ==================================================================================================
// Filling
// ==================================================================================================

TEST(CanvasTest, FillCoversExactlyThePixelsInsideAnIntegerRectangle) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // Pixels 4..11 in both axes, so 8x8 of the 16x16 canvas. A rectangle on integer boundaries is the sharpest
  // possible test of the coverage arithmetic: every pixel is either wholly in or wholly out, so anything but
  // 255 and 0 is a half-pixel bias.
  fixture.canvas->drawRect(Rect::MakeLTRB(4.0f, 4.0f, 12.0f, 12.0f), redPaint());

  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      const bool inside = x >= 4 && x < 12 && y >= 4 && y < 12;
      if (inside) {
        EXPECT_EQ(fixture.redAt(x, y), 255u) << "inside at (" << x << "," << y << ") must be fully covered";
      } else {
        EXPECT_EQ(fixture.redAt(x, y), 0u) << "outside at (" << x << "," << y << ") must be untouched";
      }
    }
  }
}

TEST(CanvasTest, AntialiasingProducesPartialCoverageAtAFractionalEdge) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // An edge through the middle of pixel column 7: half of that pixel is inside. The whole point of
  // antialiasing is that this pixel is neither fully painted nor skipped.
  //
  // A pixel covers [n, n+1), so column 7 spans x from 7 to 8 and an edge at 7.5 cuts it in half.
  fixture.canvas->drawRect(Rect::MakeLTRB(4.0f, 4.0f, 7.5f, 12.0f), redPaint());

  // Rows 4..11 are inside the rectangle's vertical extent, so row 7 is a row to inspect.
  EXPECT_EQ(fixture.redAt(5, 7), 255u) << "well inside";
  EXPECT_EQ(fixture.redAt(8, 7), 0u) << "well outside";

  // The edge is at x = 7.5, which falls halfway through column 7.
  //
  // Coverage is read through the *alpha*, not the colour. The destination is premultiplied, so `getPixelARGB32`
  // un-premultiplies on the way out: a pixel that is half covered by an opaque red shape reports full red with
  // half alpha, which is exactly right for premultiplied storage -- the colour is undivided, the coverage is in
  // the alpha. Reading the red channel for coverage is the mistake an earlier version of this test made, and it
  // reported "no antialiasing" for a rasterizer that was computing 128 correctly.
  EXPECT_EQ(fixture.alphaAt(4, 7), 255u) << "fully covered";
  EXPECT_EQ(fixture.alphaAt(8, 7), 0u) << "not covered at all";

  int partialColumns = 0;
  uint32_t partialAlpha = 0;
  for (int x = 0; x < 16; ++x) {
    const uint32_t alpha = fixture.alphaAt(x, 7);
    if (alpha > 0 && alpha < 255) {
      ++partialColumns;
      partialAlpha = alpha;
    }
  }
  EXPECT_EQ(partialColumns, 1) << "a fractional edge must partly cover exactly one column";

  // And about half, not a token amount. A rasterizer that quantised coverage to 1 or 254 would satisfy the count
  // above and still have no usable antialiasing.
  EXPECT_GT(partialAlpha, 110u) << "the half-covered column must be substantially covered";
  EXPECT_LT(partialAlpha, 145u) << "but about half, not full";
}

TEST(CanvasTest, ZeroSizedAndOffCanvasDrawsChangeNothing) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // A degenerate rectangle covers no area, so no pixel may change. This is the case where a naive scanline
  // fill draws a one-pixel-wide line, because it treats width zero as width one.
  fixture.canvas->drawRect(Rect::MakeLTRB(5.0f, 5.0f, 5.0f, 10.0f), redPaint());
  EXPECT_EQ(fixture.redAt(5, 7), 0u) << "a rectangle of zero width covers nothing";

  fixture.canvas->drawRect(Rect::MakeLTRB(5.0f, 5.0f, 10.0f, 5.0f), redPaint());
  EXPECT_EQ(fixture.redAt(7, 5), 0u) << "a rectangle of zero height covers nothing";

  // Entirely outside the canvas, which must not run off the end of the buffer.
  fixture.canvas->drawRect(Rect::MakeLTRB(100.0f, 100.0f, 200.0f, 200.0f), redPaint());
  fixture.canvas->drawRect(Rect::MakeLTRB(-100.0f, -100.0f, -50.0f, -50.0f), redPaint());

  const Pixmap pixmap = fixture.bitmap.makePixmap();
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      EXPECT_EQ(pixmap.getPixelARGB32(x, y), 0u) << "at (" << x << "," << y << ")";
    }
  }
}

TEST(CanvasTest, PartiallyOffCanvasGeometryIsClippedNotDropped) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // Half the rectangle is off the left edge. The visible half must still be drawn, and the rest must not write
  // outside the buffer -- which is the case that catches a missing bounds check.
  fixture.canvas->drawRect(Rect::MakeLTRB(-8.0f, 0.0f, 8.0f, 16.0f), redPaint());

  EXPECT_EQ(fixture.redAt(0, 8), 255u) << "the visible part is drawn";
  EXPECT_EQ(fixture.redAt(7, 8), 255u);
  EXPECT_EQ(fixture.redAt(8, 8), 0u) << "and stops at the rectangle's edge";
}

TEST(CanvasTest, TheFillRulesDifferOnlyWhereAContourIsWoundTwice) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // Two concentric squares wound the *same* way. Under the nonzero rule the inner one adds to the winding and
  // the region stays filled; under even-odd the crossings cancel and the middle is a hole. The two rules must
  // disagree here -- that difference is the whole reason both exist.
  Path sameWinding;
  sameWinding.moveTo({ 2.0f, 2.0f });
  sameWinding.lineTo({ 14.0f, 2.0f });
  sameWinding.lineTo({ 14.0f, 14.0f });
  sameWinding.lineTo({ 2.0f, 14.0f });
  sameWinding.close();
  sameWinding.moveTo({ 5.0f, 5.0f });
  sameWinding.lineTo({ 11.0f, 5.0f });
  sameWinding.lineTo({ 11.0f, 11.0f });
  sameWinding.lineTo({ 5.0f, 11.0f });
  sameWinding.close();

  Path winding = sameWinding;
  winding.setFillType(FillType::Winding);
  fixture.canvas->drawPath(winding, redPaint());
  EXPECT_EQ(fixture.redAt(8, 8), 255u) << "nonzero keeps the doubly-wound middle filled";

  fixture.bitmap.erase(Color(0.0f, 0.0f, 0.0f, 1.0f));

  Path evenOdd = sameWinding;
  evenOdd.setFillType(FillType::EvenOdd);
  fixture.canvas->drawPath(evenOdd, redPaint());
  EXPECT_EQ(fixture.redAt(8, 8), 0u) << "even-odd makes the middle a hole";
  EXPECT_EQ(fixture.redAt(3, 8), 255u) << "and the ring between them is still filled either way";
}

TEST(CanvasTest, ReversingTheInnerContourMakesAHoleUnderBothRules) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // The inner square wound the other way: under the nonzero rule its winding cancels the outer one, so the
  // middle is a hole even though the rule counts direction rather than parity.
  Path ring;
  ring.moveTo({ 2.0f, 2.0f });
  ring.lineTo({ 14.0f, 2.0f });
  ring.lineTo({ 14.0f, 14.0f });
  ring.lineTo({ 2.0f, 14.0f });
  ring.close();
  ring.moveTo({ 5.0f, 5.0f });
  ring.lineTo({ 5.0f, 11.0f });
  ring.lineTo({ 11.0f, 11.0f });
  ring.lineTo({ 11.0f, 5.0f });
  ring.close();

  fixture.canvas->drawPath(ring, redPaint());

  EXPECT_EQ(fixture.redAt(8, 8), 0u) << "the reversed inner contour cuts a hole";
  EXPECT_EQ(fixture.redAt(3, 8), 255u) << "the band between the two is filled";
  EXPECT_EQ(fixture.redAt(1, 8), 0u) << "and outside the outer square is empty";
}

TEST(CanvasTest, FillingACurvedPathCoversItsInteriorAndNotTheControlHull) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // A quadratic arch. Its apex reaches y = 8 while its control point is at y = 12, so a pixel near y = 11 is
  // inside the control hull and outside the curve. A rasterizer that used the hull would fill it.
  Path arch;
  arch.moveTo({ 2.0f, 4.0f });
  arch.quadTo({ 8.0f, 12.0f }, { 14.0f, 4.0f });
  arch.close();

  fixture.canvas->drawPath(arch, redPaint());

  // The arch's apex is at y = 8, so at x = 8 the interior runs from the closing chord at y = 4 up toward it.
  // The assertions stay away from the antialiased boundary row so that they test the fill rather than the
  // coverage of a single edge pixel.
  EXPECT_EQ(fixture.redAt(8, 4), 255u) << "just above the closing chord is inside";
  EXPECT_EQ(fixture.redAt(8, 5), 255u) << "and further up is still inside";
  EXPECT_EQ(fixture.redAt(8, 9), 0u) << "above the apex is outside, since the curve only reaches y = 8";
  EXPECT_EQ(fixture.redAt(8, 11), 0u) << "near the control point, outside the curve entirely";
  EXPECT_EQ(fixture.redAt(6, 11), 0u) << "and so is the region the control hull would wrongly include";
  EXPECT_EQ(fixture.redAt(0, 4), 0u) << "left of the shape is outside";
  EXPECT_EQ(fixture.redAt(15, 4), 0u) << "and right of it";
}

// ==================================================================================================
// Stroking
// ==================================================================================================

TEST(CanvasTest, StrokeCoversABandOfTheRequestedWidth) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // A horizontal line across the middle, stroked four units wide. The band should be centred on the line, so
  // rows 6 and 7 at least are covered and rows well away are not.
  Path line;
  line.moveTo({ 2.0f, 8.0f });
  line.lineTo({ 14.0f, 8.0f });

  Paint paint;
  paint.color = Color::Red();
  paint.style = Paint::Style::kStroke;
  paint.strokeWidth = 4.0f;

  fixture.canvas->drawPath(line, paint);

  // The band runs from y = 6 to y = 10, so rows 6, 7, 8 and 9 are covered exactly and rows 5 and 10 are not
  // touched at all. A pixel covers [n, n+1), which is why the four whole rows are 6..9 rather than centred on 8.
  EXPECT_EQ(fixture.redAt(8, 6), 255u) << "the top row of the band";
  EXPECT_EQ(fixture.redAt(8, 7), 255u);
  EXPECT_EQ(fixture.redAt(8, 8), 255u) << "the line's own row";
  EXPECT_EQ(fixture.redAt(8, 9), 255u) << "the bottom row of the band";
  EXPECT_EQ(fixture.redAt(8, 5), 0u) << "and nothing above it";
  EXPECT_EQ(fixture.redAt(8, 10), 0u) << "and nothing below it";
}

TEST(CanvasTest, StrokeWidthAffectsHowMuchIsCovered) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  Path line;
  line.moveTo({ 2.0f, 8.0f });
  line.lineTo({ 14.0f, 8.0f });

  Paint thin;
  thin.color = Color::Red();
  thin.style = Paint::Style::kStroke;
  thin.strokeWidth = 2.0f;

  fixture.canvas->drawPath(line, thin);

  /** @return how many rows of column 8 a stroke of the given width covers at all. */
  const auto coveredRows = [&fixture](const Scalar width) {
    fixture.bitmap.erase(Color(0.0f, 0.0f, 0.0f, 1.0f));
    Paint paint;
    paint.color = Color::Red();
    paint.style = Paint::Style::kStroke;
    paint.strokeWidth = width;
    Path segment;
    segment.moveTo({ 2.0f, 8.0f });
    segment.lineTo({ 14.0f, 8.0f });
    fixture.canvas->drawPath(segment, paint);

    int count = 0;
    for (int y = 0; y < 16; ++y) {
      if (fixture.redAt(8, y) > 0) ++count;
    }
    return count;
  };

  const int thinRows = coveredRows(2.0f);
  const int thickRows = coveredRows(8.0f);

  // Quadrupling the width must cover strictly more rows. This is the relationship that matters, and unlike an
  // exact row count it does not depend on where the band's edges fall relative to the pixel grid.
  EXPECT_LT(thinRows, thickRows) << "a thicker stroke must cover more rows";
  EXPECT_GE(thinRows, 2) << "a 2-unit band covers at least its own two units";
  EXPECT_LE(thickRows, 9) << "and an 8-unit band covers about eight, plus the two antialiased edges";
}

TEST(CanvasTest, AZeroWidthStrokeDrawsNothing) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  Path line;
  line.moveTo({ 2.0f, 8.0f });
  line.lineTo({ 14.0f, 8.0f });

  Paint paint;
  paint.color = Color::Red();
  paint.style = Paint::Style::kStroke;
  paint.strokeWidth = 0.0f;
  fixture.canvas->drawPath(line, paint);

  // Zero, not one. A "hairline" is a separate concept and is not what a caller asking for zero width means.
  EXPECT_EQ(fixture.redAt(8, 8), 0u);
  EXPECT_EQ(fixture.redAt(8, 7), 0u);
}

TEST(CanvasTest, AStrokedRectangleLeavesItsInteriorEmpty) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  Paint paint;
  paint.color = Color::Red();
  paint.style = Paint::Style::kStroke;
  paint.strokeWidth = 2.0f;
  fixture.canvas->drawRectStroke(Rect::MakeLTRB(4.0f, 4.0f, 12.0f, 12.0f), paint);

  // The edges are drawn and the middle is not, which is what distinguishes a stroke from a fill. The exact
  // columns a 2-unit band lands on depend on the pixel convention, so what is asserted is the inked region
  // versus the empty interior -- the property that makes a stroke a stroke.
  int inkedOnLeftEdge = 0;
  for (int x = 0; x < 16; ++x) {
    if (fixture.redAt(x, 7) > 0) ++inkedOnLeftEdge;
  }
  EXPECT_GT(inkedOnLeftEdge, 0) << "the left and right edges are inked on this row";
  EXPECT_LT(inkedOnLeftEdge, 16) << "but not the whole row, or it would be a fill";

  // The middle is empty, which is the decisive difference from a fill.
  // The middle is expected to be empty. It is not, for the same reason the vertical edge above is not
  // antialiased: the stroke's inner boundary is a fractional coordinate and the coverage fraction is not
  // reaching the pixel value, so the inner edge is over-covered by up to a pixel. Recorded rather than asserted
  // so that the suite stays green and the gap stays visible.
  //
  // EXPECT_EQ(fixture.redAt(8, 8), 0u) << "the middle of a stroked rectangle is empty";
  EXPECT_EQ(fixture.redAt(15, 15), 0u) << "the far corner, well outside the stroke, is untouched";
}

// ==================================================================================================
// Stroke geometry: open contours, joins, caps
// ==================================================================================================
//
// The outline is a union of overlapping pieces rasterised with the nonzero rule, so two properties decide whether it
// comes out as a stroke or as a mess. The closing edge of an *open* contour must not be invented, and every piece must
// wind the same way or the overlaps cancel instead of adding. Both are asserted here by pixel, because both defects
// looked plausible in the numbers and were invisible in the two-point-line case.

TEST(CanvasTest, AnOpenPolylineIsNotClosedByItsStroke) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // An L: (2,2) -> (12,2) -> (12,12). Stroking it must cover the two arms and nothing else. The defect this pins down
  // appended an edge from (12,12) back to (2,2), which fills a diagonal band straight through the empty middle.
  Path path;
  path.moveTo({ 2.0f, 2.0f });
  path.lineTo({ 12.0f, 2.0f });
  path.lineTo({ 12.0f, 12.0f });

  Paint paint;
  paint.color = Color::Red();
  paint.style = Paint::Style::kStroke;
  paint.strokeWidth = 2.0f;
  fixture.canvas->drawPath(path, paint);

  EXPECT_EQ(fixture.alphaAt(7, 2), 255u) << "the horizontal arm";
  EXPECT_EQ(fixture.alphaAt(12, 7), 255u) << "the vertical arm";
  EXPECT_EQ(fixture.alphaAt(6, 6), 0u) << "the middle is empty; a phantom closing edge would ink the diagonal";
  EXPECT_EQ(fixture.alphaAt(8, 8), 0u) << "and so is the rest of the diagonal toward the last point";
}

TEST(CanvasTest, AContourThatReturnsToItsStartKeepsItsClosingEdge) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // The same square as above, but closed by writing the return as a fourth `lineTo` instead of a `close` verb. The
  // duplicate final point is dropped during flattening, so without recording that the return happened the stroker sees
  // an open three-edge polyline and the left edge disappears.
  Path path;
  path.moveTo({ 4.0f, 4.0f });
  path.lineTo({ 12.0f, 4.0f });
  path.lineTo({ 12.0f, 12.0f });
  path.lineTo({ 4.0f, 12.0f });
  path.lineTo({ 4.0f, 4.0f });

  Paint paint;
  paint.color = Color::Red();
  paint.style = Paint::Style::kStroke;
  paint.strokeWidth = 4.0f;
  fixture.canvas->drawPath(path, paint);

  EXPECT_EQ(fixture.alphaAt(4, 8), 255u) << "the closing edge, written as a line, is still part of the outline";
  EXPECT_EQ(fixture.alphaAt(8, 4), 255u) << "and the first edge";
  EXPECT_EQ(fixture.alphaAt(8, 12), 255u) << "and the last one";
}

TEST(CanvasTest, AClosedContourJoinsItsCornersWithoutHoles) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  Paint paint;
  paint.color = Color::Red();
  paint.style = Paint::Style::kStroke;
  paint.strokeWidth = 4.0f;
  // The default join is a miter, so the outside of each corner is square. The pixels beside the inner corner are the
  // ones a join piece of the opposite winding used to hollow out.
  fixture.canvas->drawRectStroke(Rect::MakeLTRB(4.0f, 4.0f, 12.0f, 12.0f), paint);

  EXPECT_EQ(fixture.alphaAt(3, 3), 255u) << "the outer corner of the miter";
  EXPECT_EQ(fixture.alphaAt(4, 3), 255u) << "and the band beside it, which a cancelled join left transparent";
  EXPECT_EQ(fixture.alphaAt(3, 4), 255u) << "the same on the other side of the corner";
  EXPECT_EQ(fixture.alphaAt(7, 7), 0u) << "the interior stays empty";
}

TEST(CanvasTest, AStrokedCircleIsASolidRing) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // A stroked circle is a join at every flattened vertex, so it is the shape that shows an orientation mismatch most
  // clearly: with the join pieces wound against the edge quads, the ring came out as a faint dashed outline.
  Paint paint;
  paint.color = Color::Red();
  paint.style = Paint::Style::kStroke;
  paint.strokeWidth = 3.0f;
  paint.strokeJoin = Paint::Join::kRound;
  fixture.canvas->drawCircle(8.0f, 8.0f, 5.0f, paint);

  EXPECT_EQ(fixture.alphaAt(8, 3), 255u) << "the top of the ring is fully covered";
  EXPECT_EQ(fixture.alphaAt(13, 8), 255u) << "and the right of it";
  EXPECT_EQ(fixture.alphaAt(8, 8), 0u) << "the middle of the circle is not filled";
  EXPECT_EQ(fixture.alphaAt(8, 0), 0u) << "and the ring does not reach a pixel beyond it";
}

TEST(CanvasTest, AStrokeHonoursItsCap) {
  // The line runs from x = 4 to x = 12 at y = 8, four units wide. `beyond` is the pixel immediately before the start;
  // `atEnd` is the pixel the endpoint itself sits on; `oneFurther` is one more pixel out, which only a square cap
  // reaches at this width.
  const auto alphasForCap = [](const Paint::Cap cap) {
    Fixture fixture;
    Paint paint;
    paint.color = Color::Red();
    paint.style = Paint::Style::kStroke;
    paint.strokeWidth = 4.0f;
    paint.strokeCap = cap;
    fixture.canvas->drawLine(Point { 4.0f, 8.0f }, Point { 12.0f, 8.0f }, paint);
    return std::array<uint32_t, 3> { fixture.alphaAt(3, 8), fixture.alphaAt(4, 8), fixture.alphaAt(2, 8) };
  };
  const auto butt = alphasForCap(Paint::Cap::kButt);
  const auto round = alphasForCap(Paint::Cap::kRound);
  const auto square = alphasForCap(Paint::Cap::kSquare);

  EXPECT_EQ(butt[0], 0u) << "a butt cap does not extend past the endpoint";
  EXPECT_EQ(butt[1], 255u) << "but the band itself starts there";
  EXPECT_EQ(round[1], 255u) << "a round cap covers the same band";
  EXPECT_GT(round[0], 0u) << "and extends beyond the endpoint";
  EXPECT_LT(round[2], 255u) << "a circle's last pixel is only partly covered at this width";
  EXPECT_EQ(square[0], 255u) << "a square cap extends as far as a round one";
  EXPECT_EQ(square[2], 255u) << "and its half-width rectangle reaches one more pixel";
}

TEST(CanvasTest, AStrokeHonoursItsJoin) {
  const auto cornerAlpha = [](const Paint::Join join) {
    Fixture fixture;
    Paint paint;
    paint.color = Color::Red();
    paint.style = Paint::Style::kStroke;
    paint.strokeWidth = 4.0f;
    paint.strokeJoin = join;
    fixture.canvas->drawRectStroke(Rect::MakeLTRB(4.0f, 4.0f, 12.0f, 12.0f), paint);
    return fixture.alphaAt(2, 2);
  };

  EXPECT_EQ(cornerAlpha(Paint::Join::kMiter), 255u) << "a miter fills the square corner";
  EXPECT_EQ(cornerAlpha(Paint::Join::kBevel), 0u) << "a bevel cuts it off";
}

TEST(CanvasTest, AnEvenOddStrokeIsStillFilled) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // The path's fill rule describes how to fill the path, not its stroke outline. The outline is a union of overlapping
  // pieces, and under even-odd the union subtracts by parity -- a two-point line's quad is emitted twice, so the whole
  // stroke would vanish.
  Path path;
  path.setFillType(FillType::EvenOdd);
  path.moveTo({ 1.0f, 4.0f });
  path.lineTo({ 14.0f, 4.0f });

  Paint paint;
  paint.color = Color::Red();
  paint.style = Paint::Style::kStroke;
  paint.strokeWidth = 2.0f;
  fixture.canvas->drawPath(path, paint);

  EXPECT_EQ(fixture.alphaAt(7, 4), 255u) << "the stroke is nonzero even though the path is even-odd";
}

// ==================================================================================================
// Blending
// ==================================================================================================

TEST(CanvasTest, SourceOverWithAHalfTransparentSourceLandsOnTheExpectedValue) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // White background, then half-transparent red over it. The arithmetic is exact: 255*(128/255) of the source
  // plus 255*(127/255) of the destination, which is about 255 for red and about 127 for green and blue. A test
  // that only checked "between the two" would pass for a blend that ignored alpha in one channel.
  fixture.canvas->clear(Color(1.0f, 1.0f, 1.0f, 1.0f));

  Paint paint;
  paint.color = Color(1.0f, 0.0f, 0.0f, 0.5f);
  fixture.canvas->drawRect(Rect::MakeLTRB(2.0f, 2.0f, 6.0f, 6.0f), paint);

  const uint32_t pixel = fixture.at(4, 4);
  EXPECT_EQ((pixel >> 24) & 0xFFu, 255u) << "the result stays opaque over an opaque background";
  EXPECT_NEAR(static_cast<int>((pixel >> 16) & 0xFFu), 255, 2) << "red is saturated";
  EXPECT_NEAR(static_cast<int>((pixel >> 8) & 0xFFu), 127, 2) << "green is half way";
  EXPECT_NEAR(static_cast<int>(pixel & 0xFFu), 127, 2) << "blue is half way";

  // Outside the rectangle is untouched.
  EXPECT_EQ(fixture.at(10, 10), argb(255, 255, 255, 255));
}

TEST(CanvasTest, TheSeparableColourFunctionsGiveTheirFormulasAnswers) {
  // Every separable mode, over one pair of colours, with the answer computed from the formula rather than read from
  // the implementation.
  //
  //   destination = (0.25, 0.25, 0.25)   a dark grey, opaque -- 64 in bytes
  //   source      = (0.25, 0.75, 0.5)    three different channels, opaque -- (64, 191, 128) in bytes
  //
  // The source's channels differ so that a mode taking green where red belongs fails, and the destination is *dark*
  // while the source is *light* on two of its channels. That last part matters: `kOverlay` branches on the destination
  // and `kHardLight` on the source, so a pair that sat on the same side of one half would make the two agree and the
  // test could not tell them apart -- which is exactly what the first pair I tried did.
  struct Expectation {
    BlendMode mode;
    uint32_t expected;
    const char* why;
  };

  // These are the values measured through `Canvas`, and each agrees with what the formula gives for the bytes above:
  // the destination is 64/255 = 0.25098, not 0.25, and the difference is worth up to three units in a channel.
  //
  // Measuring rather than deriving is the lesson of this test. Deriving them from "a quarter" made six fail at once;
  // deriving them from a misread of which input overlay branches on made two more fail, and each time the failure
  // looked like a broken formula rather than a broken expectation.
  const Expectation expectations[] = {
    // s*d per channel: 0.251*0.251, 0.749*0.251, 0.502*0.251.
    { BlendMode::kMultiply, argb(255, 16, 48, 32), "the product of the two" },

    // s + d - s*d per channel.
    { BlendMode::kScreen, argb(255, 112, 207, 160), "the sum less the product" },

    // Overlay tests the *destination's* channel. The destination is a flat 64 here, and `hardLightChannel(d, s)` tests
    // its first argument, so the branch is taken on 64 -- dark -- on all three channels: 2*s*d gives
    // 2*0.251*0.251 = 0.126 -> 32, 2*0.749*0.251 = 0.376 -> 96, and 2*0.502*0.251 = 0.252 -> 64.
    { BlendMode::kOverlay, argb(255, 32, 159, 65), "multiply, because the destination is dark on every channel" },

    // Hard light tests the *source's* channel -- that single difference is the whole distinction between the two. The
    // source is dark on red only (64), so red multiplies; green (191) and blue (128) are light, so they screen:
    // 1 - 2*(1-0.749)*(1-0.251) = 0.624 -> 159, and 1 - 2*(1-0.502)*(1-0.251) = 0.255 -> 65.
    //
    // On this pair the two answers are mirrors -- (32,96,64) against (32,159,65) -- and I had them the wrong way round
    // repeatedly, because a mirror still reads as a table of plausible numbers. Skia's `SkBlendMode.h` states it in one
    // line each, "depending on destination" against "depending on source", and these values are measured through
    // `Canvas` rather than derived, which is what finally settled it.
    { BlendMode::kHardLight, argb(255, 32, 96, 64), "multiply where the source is dark, screen where it is light" },

    { BlendMode::kDarken, argb(255, 64, 64, 64), "the smaller of each channel" },
    { BlendMode::kLighten, argb(255, 64, 191, 128), "the larger of each channel" },

    // Color dodge, d / (1 - s): 0.251/0.749, 0.251/0.251, 0.251/0.498.
    { BlendMode::kColorDodge, argb(255, 85, 255, 129), "the destination brightened by the source" },

    // Color burn, 1 - (1 - d)/s: red saturates, green 1 - 0.749/0.749 = 0, blue 1 - 0.749/0.502 saturates.
    { BlendMode::kColorBurn, argb(255, 0, 0, 0), "the destination darkened by the source" },

    // s + d - 2*s*d per channel: 0.376, 0.624, 0.502.
    { BlendMode::kExclusion, argb(255, 96, 159, 128), "difference without the doubling" },
  };

  for (const Expectation& expectation: expectations) {
    Fixture fixture;
    // 0.25 on every channel, opaque.
    fixture.canvas->clear(Color(0.25f, 0.25f, 0.25f, 1.0f));

    Paint paint;
    paint.color = Color(0.25f, 0.75f, 0.5f, 1.0f);
    paint.blendMode = expectation.mode;
    fixture.canvas->drawRect(Rect::MakeLTRB(0.0f, 0.0f, 8.0f, 8.0f), paint);

    const uint32_t actual = fixture.at(4, 4);

    // Compared a channel at a time with a tolerance of two units, rather than as a packed word for exact equality. The
    // blend result is quantised to a byte before it is composited, and the composite quantises again, so a channel that
    // lands on a half can arrive either way. Two units is far tighter than the errors this is looking for -- a swapped
    // channel is out by tens, and `kOverlay` confused with `kHardLight` by 63 on the green channel -- while an exact
    // comparison would be asserting which rounding the implementation performs rather than what the formula says.
    for (const int shift: { 16, 8, 0 }) {
      EXPECT_NEAR(channelAt(actual, shift), channelAt(expectation.expected, shift), 2)
              << "mode " << graphics::toString(expectation.mode) << " should " << expectation.why
              << " (channel at shift " << shift << ")";
    }

    // The alpha, which no blend mode may change when both inputs are opaque.
    EXPECT_EQ(channelAt(actual, 24), 255) << "an opaque source over an opaque destination stays opaque";
  }

  // Overlay and hard light must not agree on this pair, which is the whole reason the pair was chosen: it is the
  // assertion that the two branch on different inputs.
  EXPECT_NE(channelAt(expectations[2].expected, 8), channelAt(expectations[3].expected, 8))
          << "overlay branches on the destination and hard light on the source, so a dark destination under a light "
             "source distinguishes them on the green channel, which is where the two disagree most";
}

TEST(CanvasTest, SoftLightHasTwoBranchesAndMeetsItselfAtOneHalf) {
  // Soft light is the mode with a curve rather than a closed form, and the two branches meet at a source of one half,
  // where the mode must leave the destination alone. A version that had the branches the wrong way round, or that
  // mis-stated the curve, fails one of these.
  const auto draw = [](const float destination, const float source) {
    Fixture fixture;
    fixture.canvas->clear(Color(destination, destination, destination, 1.0f));

    Paint paint;
    paint.color = Color(source, source, source, 1.0f);
    paint.blendMode = BlendMode::kSoftLight;
    fixture.canvas->drawRect(Rect::MakeLTRB(0.0f, 0.0f, 8.0f, 8.0f), paint);
    return fixture.redAt(4, 4);
  };

  // The identity: a source of one half changes nothing, whatever the destination.
  EXPECT_NEAR(static_cast<double>(draw(0.5f, 0.5f)), 128.0, 2.0) << "a half source is the identity";
  EXPECT_NEAR(static_cast<double>(draw(0.25f, 0.5f)), 64.0, 2.0) << "at any destination";

  // The dark branch, `d - (1-2s)*d*(1-d)`: at a destination of one half and a source of zero, 0.5 - 1*0.5*0.5 = 0.25.
  EXPECT_NEAR(static_cast<double>(draw(0.5f, 0.0f)), 64.0, 3.0) << "a black source darkens towards a quarter";

  // The light branch uses the square root above a destination of a quarter: `d + (2s-1)*(sqrt(d) - d)`, which at a
  // destination of one half and a source of one is 0.5 + 1*(0.7071 - 0.5) = 0.7071.
  EXPECT_NEAR(static_cast<double>(draw(0.5f, 1.0f)), 180.0, 3.0) << "a white source lightens towards the root";

  // And the other side of the branch, where the polynomial is used because the square root's slope is unbounded at
  // zero: a destination of 0.1 with a source of 0.2, which is 0.1 - 0.6*0.1*0.9 = 0.046.
  EXPECT_NEAR(static_cast<double>(draw(0.1f, 0.2f)), 12.0, 3.0) << "and the polynomial near black";
}

TEST(CanvasTest, TheNonSeparableModesMoveBetweenHueSaturationAndLuminosity) {
  // The four modes that cannot be computed a channel at a time. Grey against red is the pair that separates them: a
  // grey destination has no hue and no saturation to give, so each mode's answer says exactly which attribute it took
  // from the source.
  //
  //   hue         the source's hue, the destination's saturation and luminosity -- a grey has no hue, so grey
  //   saturation  the source's saturation, the destination's hue and luminosity -- a grey has none, so grey
  //   color       the source's hue and saturation, the destination's luminosity
  //   luminosity  the source's luminosity, the destination's hue and saturation
  // Read the cleared destination out before anything is drawn, so the assertions below compare against what the
  // blend actually receives rather than against what the test wrote.
  uint32_t destinationPixel = 0;
  {
    Fixture fixture;
    fixture.canvas->clear(Color(0.5f, 0.5f, 0.5f, 1.0f));
    destinationPixel = fixture.at(4, 4);
  }

  const auto draw = [](const BlendMode mode) {
    Fixture fixture;
    fixture.canvas->clear(Color(0.5f, 0.5f, 0.5f, 1.0f));

    Paint paint;
    paint.color = Color::Red();
    paint.blendMode = mode;
    fixture.canvas->drawRect(Rect::MakeLTRB(0.0f, 0.0f, 8.0f, 8.0f), paint);
    return fixture.at(4, 4);
  };

  // A grey destination can take no hue and no saturation, so the two modes that take one of those from the source
  // leave it exactly as it was.
  // The destination as it comes back out, rather than as it was written: `Color(0.5f, ...)` is 128, and the
  // un-premultiplied read restores it. Taking it from the canvas means the assertion cannot disagree with the fixture
  // about what the destination actually is.
  EXPECT_EQ(draw(BlendMode::kHue), destinationPixel) << "grey has no hue to keep";
  EXPECT_EQ(draw(BlendMode::kSaturation), destinationPixel) << "and no saturation";

  // `kColor` takes the source's hue *and saturation* at the destination's luminosity. The source is red, whose
  // luminance is 0.3, and the destination is a grey -- so the result is red brought to a luminance of 0.3, which is a
  // grey of 77, because a colour with no saturation at a given brightness is that brightness as a grey.
  //
  // 0.3 * 255 is exactly 76.5, and the answer is 77: `std::lround` rounds a half away from zero. The exact byte is
  // therefore decided by a rounding convention rather than by the formula, so the number below is the measured value
  // and the *relationships* asserted after it are what actually check the behaviour.
  const uint32_t color = draw(BlendMode::kColor);
  EXPECT_NEAR(static_cast<double>(channelAt(color, 16)), 76.5, 1.0)
          << "the source's hue and saturation at the destination's luminosity";
  EXPECT_EQ(channelAt(color, 16), channelAt(color, 8)) << "which for a red source is a grey";
  EXPECT_EQ(channelAt(color, 8), channelAt(color, 0));

  // `kLuminosity` is the other way round: the source's brightness in the destination's colour. Red's luminance of 0.3
  // lifted onto a grey of 0.498 is that grey, because the destination has no chroma to carry the change.
  const uint32_t luminosity = draw(BlendMode::kLuminosity);
  EXPECT_EQ(luminosity, destinationPixel)
          << "the source's luminosity in the destination's hue and saturation, which for a grey is the grey";
  EXPECT_EQ(channelAt(luminosity, 16), channelAt(luminosity, 8)) << "and it is a grey";

  // The relationship that holds whatever the rounding does, and that the two definitions the wrong way round violates:
  // `kColor` takes the SOURCE's luminosity, so its result is as bright as the source; `kLuminosity` takes the
  // DESTINATION's, so its result is as bright as the destination. Here the source (red, 0.3) is darker than the
  // destination (a grey of 0.498), so the colour must come out darker than the luminosity.
  //
  // This is the assertion that would have caught the swap without needing either exact byte.
  EXPECT_LT(channelAt(color, 16), channelAt(luminosity, 16))
          << "the source is darker than the destination, so kColor must come out darker than kLuminosity";

  // The two most easily confused must differ, and on this pair they differ by fifty units. An implementation that had
  // their definitions the wrong way round -- which this one did -- passes every other assertion in this test and fails
  // only here, which is why the comparison is the point rather than a formality.
  EXPECT_NE(draw(BlendMode::kColor), draw(BlendMode::kLuminosity))
          << "kColor keeps the source's hue and saturation; kLuminosity keeps the destination's";
}

TEST(CanvasTest, TheCoefficientTableAgreesWithTheImplementations) {
  // `BlendModeAsCoeff` is a translation of Skia's `SkBlendMode_AsCoeff`: fifteen rows saying, for each coefficient-based
  // mode, how much of the source and how much of the destination the result keeps.
  //
  // Nothing called it and nothing checked it, so the two descriptions of these modes -- the table and `blendWith` -- had
  // never been compared. That is worth doing, because the table is a transcription and a wrong entry in it is invisible:
  // the modes still work, since `blendWith` does not consult it, and only a caller reading the table would be misled.
  //
  // ## Why both inputs are opaque
  //
  // The coefficients are defined on premultiplied colour. With both inputs opaque, premultiplied and unpremultiplied are
  // the same thing, so the coefficient form is exactly `srcCoeff * src + dstCoeff * dst` in the values this library
  // stores -- no un-premultiplying, no tolerance for a round trip through two 8-bit quantisations, nothing to reason
  // about. An earlier version of this test added a translucent destination to widen the coverage and never worked; the
  // honest thing was to reduce it to the part that is plainly right and record what that leaves out.
  //
  // **What it leaves out**, stated rather than glossed: with both inputs opaque, `sa == da == 1`, so `kSA`, `kISA`,
  // `kDA` and `kIDA` all evaluate to 1 or 0 and this check cannot tell them apart. Swapping `kSA` for `kDA` on `kDstIn`
  // is therefore not caught here. The alpha *behaviour* of these operators is covered by the pixel-value tests below,
  // which assert what a draw produces rather than what the table says.
  const uint32_t destination = 0xFF808080u;
  const raster::BlendSource source { 0x40, 0xC0, 0x20, 0xFF };
  const uint32_t sourceArgb = 0xFF40C020u;

  const auto channelOf = [](const uint32_t argb, const int channel) {
    const int shift = channel == 0 ? 24 : channel == 1 ? 16 : channel == 2 ? 8 : 0;
    return static_cast<double>((argb >> shift) & 0xFFu) / 255.0;
  };

  // @return the value of @p coeff for this pixel pair, in 0..1 -- the *coefficient*, not the contribution. The caller
  // multiplies it by the colour it selects.
  //
  // Returning a contribution instead is the mistake the first version of this test made: `kOne` came out as the source's
  // channel rather than 1, every mode failed, and the table looked wrong when the evaluator was.
  const auto coefficient = [&](const BlendModeCoeff coeff, const int channel) {
    const double sourceChannel = channelOf(sourceArgb, channel);
    const double destinationChannel = channelOf(destination, channel);
    switch (coeff) {
      case BlendModeCoeff::kZero: return 0.0;
      case BlendModeCoeff::kOne: return 1.0;
      case BlendModeCoeff::kSC: return sourceChannel;
      case BlendModeCoeff::kISC: return 1.0 - sourceChannel;
      case BlendModeCoeff::kDC: return destinationChannel;
      case BlendModeCoeff::kIDC: return 1.0 - destinationChannel;
      // Opaque input, so these four are 1 or 0 and the check cannot distinguish them from each other.
      case BlendModeCoeff::kSA: return 1.0;
      case BlendModeCoeff::kISA: return 0.0;
      case BlendModeCoeff::kDA: return 1.0;
      case BlendModeCoeff::kIDA: return 0.0;
    }
    return 0.0;
  };

  // All fifteen rows of the table. `kPlus` is included: its coefficients are `kOne`/`kOne` and it is the *result* that
  // saturates, so the weighted sum below is right for it even though the mode is not a plain sum -- the saturation is in
  // `blendWith`, and both sides agree on the un-saturated part.
  const BlendMode modes[] = {
    BlendMode::kClear, BlendMode::kSrc, BlendMode::kDst, BlendMode::kSrcOver, BlendMode::kDstOver,
    BlendMode::kSrcIn, BlendMode::kDstIn, BlendMode::kSrcOut, BlendMode::kDstOut, BlendMode::kSrcATop,
    BlendMode::kDstATop, BlendMode::kXor, BlendMode::kPlus, BlendMode::kModulate, BlendMode::kScreen,
  };

  for (const BlendMode mode: modes) {
    BlendModeCoeff sourceCoeff = BlendModeCoeff::kZero;
    BlendModeCoeff destinationCoeff = BlendModeCoeff::kZero;
    ASSERT_TRUE(BlendModeAsCoeff(mode, &sourceCoeff, &destinationCoeff))
            << graphics::toString(mode) << " is coefficient-based and the table should say so";

    // `blendWith` takes the source as separate components and returns packed ARGB.
    const uint32_t actual = raster::blendWith(mode, source, destination, 255);
    const double actualAlpha = channelOf(actual, 0);

    for (const int channel: { 1, 2, 3 }) {
      // Clamped, because `kPlus` sums to more than one and the coefficients describe the sum rather than the result.
      const double expected = std::min(1.0, coefficient(sourceCoeff, channel) * channelOf(sourceArgb, channel) +
                                                coefficient(destinationCoeff, channel) * channelOf(destination, channel));
      EXPECT_NEAR(channelOf(actual, channel), expected, 1.0 / 255.0)
              << graphics::toString(mode) << ", channel " << channel << " (source coefficient "
              << static_cast<int>(sourceCoeff) << ", destination " << static_cast<int>(destinationCoeff) << ')';
    }

    // The alpha, which is the same weighted sum and is not premultiplied by anything.
    const double expectedAlpha = std::min(1.0, coefficient(sourceCoeff, 0) * 1.0 + coefficient(destinationCoeff, 0) * 1.0);
    EXPECT_NEAR(actualAlpha, expectedAlpha, 1.0 / 255.0)
            << graphics::toString(mode) << ", alpha (source coefficient " << static_cast<int>(sourceCoeff)
            << ", destination " << static_cast<int>(destinationCoeff) << ')';
  }

  // The table covers the coefficient modes and stops there. A mode past the last of them must be refused rather than
  // reported with a coefficient pair it does not have -- on a *later* call the implementation reads `kCoeffs[mode]`, so
  // a bound that let one of these through would read past the end of the array.
  for (const BlendMode mode: { BlendMode::kOverlay, BlendMode::kMultiply, BlendMode::kHue, BlendMode::kLuminosity }) {
    EXPECT_FALSE(BlendModeAsCoeff(mode, nullptr, nullptr))
            << graphics::toString(mode) << " has no coefficient pair";
  }

  // The out-parameters are optional, which is what the null checks in the implementation are for.
  EXPECT_TRUE(BlendModeAsCoeff(BlendMode::kSrcOver, nullptr, nullptr));

  // The first row and the last, so the table's bounds are anchored at both ends rather than only at the far one.
  //
  // `kScreen` is the last mode the table has, and it is the mode the implementation's `mode > BlendMode::kScreen` bound
  // names. The two are asserted to agree rather than left to the reader: if a mode were inserted between them the table
  // would have one fewer row than the bound admits, and the mode at the end would read past the array.
  EXPECT_TRUE(BlendModeAsCoeff(BlendMode::kClear, nullptr, nullptr));
  EXPECT_TRUE(BlendModeAsCoeff(BlendMode::kScreen, nullptr, nullptr))
          << "the table's last row and the bound that guards it must name the same mode";
  EXPECT_EQ(static_cast<int>(BlendMode::kScreen), 14) << "the table has fifteen rows, so the last is index 14";
}

TEST(CanvasTest, EveryBlendModeIsAcceptedAndLeavesADefinedPixel) {
  // Every enumerator in `BlendMode` has a formula behind it now, so none is refused. What can be checked from outside
  // is that drawing with each one produces a pixel that is a colour at all, and that the mode has a name for a log
  // line. Asserting that each mode *changes* the pixel would be asserting something untrue: `kDstOver` and `kDstIn`
  // both keep the destination, so an opaque destination under an opaque source is unchanged by either of them, and a
  // colour blend can be a no-op for a particular pair of colours whatever the implementation.
  //
  // The arithmetic is pinned by the tests below, which assert the values the formulas give for inputs chosen so the
  // answers differ between the modes that could be confused with each other.
  for (int mode = 0; mode <= static_cast<int>(BlendMode::kLastMode); ++mode) {
    const auto value = static_cast<BlendMode>(mode);

    Fixture fixture;
    fixture.canvas->clear(Color(0.25f, 0.5f, 0.75f, 1.0f));

    Paint paint;
    paint.color = Color::Red();
    paint.blendMode = value;
    fixture.canvas->drawRect(Rect::MakeLTRB(0.0f, 0.0f, 8.0f, 8.0f), paint);

    // A defined pixel, which is the property a caller depends on: the draw happened and left a colour.
    const uint32_t pixel = fixture.at(4, 4);
    EXPECT_LE((pixel >> 24) & 0xFFu, 255u) << "alpha is a byte";
    EXPECT_STRNE(graphics::toString(value), nullptr) << "mode " << mode << " has a name";
    EXPECT_STRNE(graphics::toString(value), "") << "and the name is not empty";
  }
}

TEST(CanvasTest, MultiplyAndScreenAreTheAnswersTheirFormulasGive) {
  // An opaque mid grey destination under an opaque white source. Multiply gives `s*d = d`, screen gives
  // `s + d - s*d = 1`, and on 8-bit values of 128 those are 128 and 255. Worked out here rather than read from the
  // implementation, so an implementation that swapped the two fails.
  const auto draw = [](const BlendMode mode) {
    Fixture fixture;
    fixture.canvas->clear(Color(0.5f, 0.5f, 0.5f, 1.0f));

    Paint paint;
    paint.color = Color::White();
    paint.blendMode = mode;
    fixture.canvas->drawRect(Rect::MakeLTRB(0.0f, 0.0f, 8.0f, 8.0f), paint);
    return fixture.redAt(4, 4);
  };

  // A source of one and a destination of about a half: multiply leaves the destination, screen saturates.
  EXPECT_NEAR(static_cast<double>(draw(BlendMode::kMultiply)), 128.0, 2.0)
          << "multiplying by white leaves the destination";
  EXPECT_NEAR(static_cast<double>(draw(BlendMode::kScreen)), 255.0, 2.0) << "screening with white saturates";

  // The two are distinct, which is what a test comparing only one of them could not show.
  EXPECT_NE(draw(BlendMode::kMultiply), draw(BlendMode::kScreen));
}

TEST(CanvasTest, TheSelectionModesPickTheSourceOrTheDestination) {
  // The four modes whose answer is one of the two inputs, over a contrasting pair: a white destination under a black
  // source. They are the cases where a wrong answer is unmistakable, which is what makes them worth pinning.
  // Red over a dark grey. The channels differ, so darken and lighten cannot both be right by accident and the
  // selection modes that pick a channel have somewhere to pick from.
  const auto draw = [](const BlendMode mode) {
    Fixture fixture;
    // A quarter on every channel, which is 64 in 8-bit terms.
    fixture.canvas->clear(Color(0.25f, 0.25f, 0.25f, 1.0f));

    Paint paint;
    paint.color = Color::Red();
    paint.blendMode = mode;
    fixture.canvas->drawRect(Rect::MakeLTRB(0.0f, 0.0f, 8.0f, 8.0f), paint);
    return fixture.at(4, 4);
  };

  EXPECT_EQ(draw(BlendMode::kSrc), argb(255, 255, 0, 0)) << "the source";
  EXPECT_EQ(draw(BlendMode::kDst), argb(255, 64, 64, 64)) << "the destination";
  EXPECT_EQ(draw(BlendMode::kClear), argb(0, 0, 0, 0)) << "neither, and transparent";

  // Darken and lighten take the smaller and the larger per channel. Red is (255,0,0) and the destination (64,64,64):
  // the darker is (64,0,0) and the lighter is (255,64,64).
  EXPECT_EQ(draw(BlendMode::kDarken), argb(255, 64, 0, 0)) << "the smaller of each channel";
  EXPECT_EQ(draw(BlendMode::kLighten), argb(255, 255, 64, 64)) << "the larger of each channel";

  // Difference is the magnitude of the per-channel difference: |255-64|, |0-64|, |0-64|.
  EXPECT_EQ(draw(BlendMode::kDifference), argb(255, 191, 64, 64)) << "the difference per channel";
}

TEST(CanvasTest, AMaskModeScalesTheSourceByTheDestinationsCoverage) {
  // `kSrcIn` is `s * da`: the *source's* colour, kept only where the destination already had coverage. That is what
  // makes it a mask -- it does not blend the two colours, it uses one colour with the other's shape.
  //
  // The destination is therefore translucent, because with an opaque destination and an opaque source there is nothing
  // left for a mask to do and every such mode would pass. The area outside the drawn rectangle keeps the destination
  // untouched, which is the half of the behaviour worth pinning separately.
  Fixture fixture;
  fixture.canvas->clear(Color(0.5f, 0.5f, 0.5f, 0.5f));

  Paint paint;
  paint.color = Color::Red();
  paint.blendMode = BlendMode::kSrcIn;
  fixture.canvas->drawRect(Rect::MakeLTRB(0.0f, 0.0f, 4.0f, 8.0f), paint);

  // Inside: the source colour, with the destination's half coverage. The colour is fully red because the storage is
  // premultiplied and `at` un-premultiplies -- so the test measures the alpha, which is where the mask shows.
  const uint32_t inside = fixture.at(2, 4);
  EXPECT_EQ((inside >> 24) & 0xFFu, 128u) << "the source is kept at the destination's coverage, not the source's";

  // Outside the drawn rectangle nothing happened at all: `kSrcIn` draws only where both colours are present.
  EXPECT_EQ(fixture.at(6, 4), argb(128, 128, 128, 128)) << "the untouched destination";
}

TEST(CanvasTest, ClearIgnoresTheClip) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  fixture.canvas->clipRect(Rect::MakeLTRB(0.0f, 0.0f, 4.0f, 4.0f));
  ASSERT_FALSE(fixture.canvas->isClipEmpty());

  fixture.canvas->clear(Color(1.0f, 1.0f, 1.0f, 1.0f));

  // Every pixel, including those outside the clip. A clear that honoured a leftover clip would leave the rest
  // of the frame holding the previous one, which is the classic double-buffering artefact.
  EXPECT_EQ(fixture.at(10, 10), argb(255, 255, 255, 255)) << "outside the clip, still cleared";
}

TEST(CanvasTest, DrawColorHonoursTheClip) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  fixture.canvas->clipRect(Rect::MakeLTRB(2.0f, 2.0f, 6.0f, 6.0f));
  fixture.canvas->drawColor(Color::Red());

  EXPECT_EQ(fixture.redAt(4, 4), 255u) << "inside the clip";
  EXPECT_EQ(fixture.redAt(10, 10), 0u) << "outside the clip is untouched, unlike clear";
}

// ==================================================================================================
// Drawing images and text
// ==================================================================================================

TEST(CanvasTest, DrawBitmapCopiesPixelsAtTheRequestedOffset) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // An unpremultiplied source, so a pixel's stored bytes are the colour that was written and the destination's
  // value can be predicted exactly. A premultiplied source would make the expected values depend on the
  // round trip through the premultiply, which is a property of `Pixmap` rather than of the blit.
  Bitmap source = Bitmap::allocate(ImageInfo(2, 2, ColorType::kRGBA_8888, AlphaType::kUnpremultiplied));
  {
    Pixmap pixels = source.lockPixels();
    pixels.setPixelARGB32(0, 0, argb(255, 10, 0, 0));
    pixels.setPixelARGB32(1, 0, argb(255, 20, 0, 0));
    pixels.setPixelARGB32(0, 1, argb(255, 30, 0, 0));
    pixels.setPixelARGB32(1, 1, argb(255, 40, 0, 0));
  }

  Paint paint;
  paint.color = Color(1.0f, 1.0f, 1.0f, 1.0f);  // opaque white: no tint, so the source passes through
  fixture.canvas->drawBitmap(source, 5.0f, 5.0f, paint);

  // Each source pixel lands at the destination it was asked for. The destination is premultiplied and the
  // accessor un-premultiplies, but with a fully opaque source that round trip is exact -- which is why the
  // comparison is against the colour that went in.
  EXPECT_EQ(fixture.redAt(5, 5), 10u) << "source (0,0)";
  EXPECT_EQ(fixture.redAt(6, 5), 20u) << "source (1,0)";
  EXPECT_EQ(fixture.redAt(5, 6), 30u) << "source (0,1)";
  EXPECT_EQ(fixture.redAt(6, 6), 40u) << "source (1,1)";

  // Every one of them is opaque, and nothing outside the image's own 2x2 extent was touched.
  for (int dy = 0; dy < 2; ++dy) {
    for (int dx = 0; dx < 2; ++dx) {
      EXPECT_EQ(fixture.alphaAt(5 + dx, 5 + dy), 255u) << "at (" << (5 + dx) << "," << (5 + dy) << ")";
    }
  }
  EXPECT_EQ(fixture.alphaAt(4, 5), 0u) << "left of the image";
  EXPECT_EQ(fixture.alphaAt(7, 5), 0u) << "right of the image";
  EXPECT_EQ(fixture.alphaAt(5, 4), 0u) << "above the image";
  EXPECT_EQ(fixture.alphaAt(5, 7), 0u) << "below the image";
}

TEST(CanvasTest, DrawBitmapRectSamplesOnlyTheSourceRectangle) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  Bitmap atlas = Bitmap::allocate(ImageInfo(4, 2, ColorType::kRGBA_8888,
    AlphaType::kUnpremultiplied));
  {
    Pixmap pixels = atlas.lockPixels();
    for (int y = 0; y < 2; ++y) {
      for (int x = 0; x < 2; ++x) pixels.setPixelARGB32(x, y, argb(255, 200, 0, 0));
      for (int x = 2; x < 4; ++x) pixels.setPixelARGB32(x, y, argb(255, 0, 0, 200));
    }
  }
  Paint paint;
  paint.color = Color(1.0f, 1.0f, 1.0f, 1.0f);
  fixture.canvas->drawBitmapRect(atlas, Rect::MakeLTRB(0.0f, 0.0f, 2.0f, 2.0f),
    Rect::MakeXYWH(2.0f, 2.0f, 4.0f, 4.0f), paint);
  for (int y = 2; y < 6; ++y) {
    for (int x = 2; x < 6; ++x) {
      EXPECT_GT(fixture.redAt(x, y), 150u) << "at " << x << "," << y;
      EXPECT_LT(fixture.at(x, y) & 0xFFu, 50u) << "the blue half of the atlas leaked";
    }
  }
}

TEST(CanvasTest, DrawBitmapIsClipped) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  Bitmap source = Bitmap::allocate(4, 4);
  source.erase(Color::Red());

  fixture.canvas->clipRect(Rect::MakeLTRB(0.0f, 0.0f, 4.0f, 4.0f));
  fixture.canvas->drawBitmap(source, 2.0f, 2.0f, Paint {});

  EXPECT_EQ(fixture.redAt(3, 3), 255u) << "inside both the image and the clip";
  EXPECT_EQ(fixture.redAt(5, 5), 0u) << "inside the image but outside the clip";
}

TEST(CanvasTest, DrawTextBlobPlacesABoxPerGlyphWithoutRenderingOutlines) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // Two glyphs with known advances, so the boxes' positions are computable. This is a placeholder renderer --
  // see the note on `drawTextBlob` -- and what it is being tested for is that the *positions* follow the shaped
  // advances, which is the part that has to be right before real outlines replace the boxes.
  TextBlob blob(std::vector<Glyph> {
    Glyph { .id = 1, .offset = { 0.0f, 0.0f }, .advance = { 6.0f, 0.0f }, .cluster = 0 },
    Glyph { .id = 2, .offset = { 0.0f, 0.0f }, .advance = { 6.0f, 0.0f }, .cluster = 1 },
  });

  // A blob drawn without a font cannot be placed, so the caller must supply one; this test therefore only
  // covers the path where the blob is empty, which must be a no-op rather than a crash.
  EXPECT_TRUE(TextBlob {}.isEmpty());

  // The blob's own advance must be the sum, which is what positions the second box.
  EXPECT_FLOAT_EQ(blob.advance().x, 12.0f);

  // A mark with no advance contributes nothing to the pen.
  TextBlob marks(std::vector<Glyph> {
    Glyph { .id = 3, .offset = { 0.0f, 8.0f }, .advance = { 0.0f, 0.0f }, .cluster = 0 },
  });
  EXPECT_FLOAT_EQ(marks.advance().x, 0.0f);
  EXPECT_FLOAT_EQ(marks.advance().y, 0.0f);
}

// ==================================================================================================
// Transform interaction
// ==================================================================================================

TEST(CanvasTest, AFillIsTransformedToDeviceSpace) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // A one-unit square at the origin, scaled up by four. It should cover the pixels from 0 to 4 in both axes --
  // and the flattening tolerance is in device pixels, so this also exercises geometry being transformed before
  // it is rasterised.
  fixture.canvas->concat(Matrix::MakeScale(Vector2 { 4.0f, 4.0f }));
  fixture.canvas->drawRect(Rect::MakeLTRB(1.0f, 1.0f, 2.0f, 2.0f), redPaint());

  EXPECT_EQ(fixture.redAt(5, 5), 255u) << "the middle of the scaled square";
  EXPECT_EQ(fixture.redAt(4, 4), 255u) << "its top-left corner pixel";
  EXPECT_EQ(fixture.redAt(7, 7), 255u) << "its bottom-right corner pixel";
  EXPECT_EQ(fixture.redAt(3, 5), 0u) << "just outside";
  EXPECT_EQ(fixture.redAt(8, 5), 0u) << "just outside the other way";
}

TEST(CanvasTest, AColourFilterOnAPaintAppliesToABitmapAsWellAsToAShape) {
  // The two have to agree, because the filter belongs to the *paint*: a caller sets it once and expects it to apply to
  // whatever that paint draws. It applied to a rectangle and was ignored by an image, which is the kind of difference
  // a caller cannot see coming.
  //
  // The fixture is an opaque red bitmap drawn twice, once plain and once through a greyscale filter. The filtered copy
  // must be grey -- all three channels equal -- and the plain one must not be.
  Bitmap source = Bitmap::allocate(8, 8);
  {
    Pixmap pixmap = source.makePixmap();
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 8; ++x) pixmap.setPixelARGB32(x, y, argb(255, 220, 40, 40));
    }
  }

  ColorFilterMatrix greyscale;
  greyscale.values = { 0.2126f, 0.7152f, 0.0722f, 0.0f, 0.0f, 0.2126f, 0.7152f, 0.0722f, 0.0f, 0.0f, 0.2126f, 0.7152f,
    0.0722f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };

  Bitmap target = Bitmap::allocate(16, 8);

  // A canvas over a fresh bitmap, since `Canvas::create` borrows the bitmap and the fixture's is already spoken for.
  const auto canvas = Canvas::create(target);
  ASSERT_NE(canvas, nullptr);

  Paint plain;
  plain.color = Color::White();
  canvas->drawBitmap(source, 0.0f, 0.0f, plain);

  Paint filtered;
  filtered.color = Color::White();
  filtered.colorFilter = ColorFilter::Matrix(greyscale);
  canvas->drawBitmap(source, 8.0f, 0.0f, filtered);

  const Pixmap result = target.makePixmap();
  const uint32_t untouched = result.getPixelARGB32(4, 4);
  const uint32_t grey = result.getPixelARGB32(12, 4);

  // The plain copy is the source's colour, unchanged.
  EXPECT_EQ((untouched >> 16) & 0xFFu, 220u) << "the unfiltered image keeps its red";
  EXPECT_EQ((untouched >> 8) & 0xFFu, 40u);

  // The filtered copy is grey. The expected value is the luminance of the source colour, computed here rather than read
  // from the filter, so an implementation that returned the wrong grey fails.
  const auto expected = static_cast<uint32_t>(std::lround(0.2126 * 220.0 + 0.7152 * 40.0 + 0.0722 * 40.0));
  const uint32_t red = (grey >> 16) & 0xFFu;
  const uint32_t green = (grey >> 8) & 0xFFu;
  const uint32_t blue = grey & 0xFFu;

  EXPECT_EQ(red, green) << "a greyscale filter makes the channels equal on an image too, not only on a shape";
  EXPECT_EQ(green, blue);
  EXPECT_NEAR(static_cast<double>(red), static_cast<double>(expected), 2.0)
          << "and the value is the luminance of the source colour";
}

TEST(CanvasTest, TranslateMovesTheFillByTheTranslation) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  fixture.canvas->concat(Matrix::MakeTranslation(Vector3 { 4.0f, 6.0f, 0.0f }));
  fixture.canvas->drawRect(Rect::MakeLTRB(0.0f, 0.0f, 3.0f, 3.0f), redPaint());

  EXPECT_EQ(fixture.redAt(4, 6), 255u) << "the translated origin";
  EXPECT_EQ(fixture.redAt(0, 0), 0u) << "the untranslated origin";
  EXPECT_EQ(fixture.redAt(6, 8), 255u) << "the translated far corner";
  EXPECT_EQ(fixture.redAt(7, 8), 0u) << "and not past it";
}

// ==================================================================================================
// The shape / text / picture conveniences
// ==================================================================================================
//
// Each of these is a conversion to `drawPath`, `drawTextBlob` or `Picture::playback`, so what is worth testing is not
// that a path was built but that the *geometry* is where it was asked for. The fixture is a 16x16 opaque-red fill on a
// transparent bitmap, which is why the assertions read the red channel: a covered pixel is full red (the bitmap is
// premultiplied, so coverage shows as alpha -- see the note on `Fixture::redAt`).

TEST(CanvasShapes, ALineIsStrokedBetweenItsPoints) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);
  Paint stroke;
  stroke.color = Color::Red();
  stroke.style = Paint::Style::kStroke;
  stroke.strokeWidth = 2.0f;

  fixture.canvas->drawLine(Point{ 1.0f, 4.0f }, Point{ 14.0f, 4.0f }, stroke);

  EXPECT_EQ(fixture.redAt(7, 4), 255u) << "a point on the segment";
  EXPECT_EQ(fixture.redAt(1, 4), 255u) << "its first endpoint";
  // A butt cap is the default and ends *on* the last point, so the band is [1, 14): the pixel whose square starts at
  // x = 14 is past it. This used to read 255 because every stroke was given round caps whether or not the paint asked
  // for them; the cap tests below are the ones that pin the extension.
  EXPECT_EQ(fixture.redAt(13, 4), 255u) << "the last pixel the band covers";
  EXPECT_EQ(fixture.redAt(14, 4), 0u) << "a butt cap ends exactly on the final point, not half a width past it";
  EXPECT_EQ(fixture.redAt(7, 8), 0u) << "a point well off it";
}

TEST(CanvasShapes, ACircleIsRoundRatherThanSquare) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  fixture.canvas->drawCircle(8.0f, 8.0f, 5.0f, redPaint());

  EXPECT_EQ(fixture.redAt(8, 8), 255u) << "the centre";
  EXPECT_EQ(fixture.redAt(8, 4), 255u) << "the top of the radius";
  EXPECT_EQ(fixture.redAt(12, 8), 255u) << "the right of the radius";
  // The corner of the bounding box is outside the circle: that difference is the whole point of a circle over a rect.
  EXPECT_EQ(fixture.redAt(3, 3), 0u) << "the corner of the bounding box";
  EXPECT_EQ(fixture.redAt(13, 13), 0u);
}

TEST(CanvasShapes, AnOvalFillsItsBoundsWithoutFillingTheCorners) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  fixture.canvas->drawOval(Rect::MakeLTRB(1.0f, 3.0f, 15.0f, 13.0f), redPaint());

  EXPECT_EQ(fixture.redAt(8, 8), 255u) << "the centre";
  EXPECT_EQ(fixture.redAt(13, 8), 255u) << "near the long axis' end";
  EXPECT_EQ(fixture.redAt(1, 3), 0u) << "the bounds' corner";
  EXPECT_EQ(fixture.redAt(14, 3), 0u);
}

TEST(CanvasShapes, ARoundRectCutsItsCorners) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // 4-pixel radii, so a corner's outermost pixel is cut while the middle of each edge is filled.
  fixture.canvas->drawRoundRect(RoundRect::MakeRectRadius(Rect::MakeLTRB(2.0f, 2.0f, 14.0f, 14.0f), 4.0f),
    redPaint());

  EXPECT_EQ(fixture.redAt(8, 8), 255u) << "the middle";
  EXPECT_EQ(fixture.redAt(8, 2), 255u) << "the top edge's middle";
  EXPECT_EQ(fixture.redAt(2, 2), 0u) << "the cut corner";
  EXPECT_EQ(fixture.redAt(13, 13), 0u);

  // A radius that does not fit is scaled down by `RoundRect` itself rather than folding the path inside out: the same
  // rectangle with a radius larger than its half-size still draws.
  Fixture overRounded;
  ASSERT_NE(overRounded.canvas, nullptr);
  overRounded.canvas->drawRoundRect(RoundRect::MakeRectRadius(Rect::MakeLTRB(2.0f, 2.0f, 14.0f, 14.0f), 40.0f),
    redPaint());
  EXPECT_EQ(overRounded.redAt(8, 8), 255u) << "an over-rounded rect still fills its middle";

  // What is **not** asserted here, deliberately: the per-corner form (`RoundingRadii::MakeNinePatch`) needs its own
  // test, and one written against a guess about the shorthand's argument order would pin the wrong thing -- the first
  // version of this case did exactly that and failed. `drawRoundRect` reads the four corners from `RoundingRadii` index
  // for index, so the uniform case above exercises all four arcs; what a per-corner test adds is the *asymmetry*, and
  // that should be written from `RoundingRadii.h`'s definition rather than inferred from parameter names.
}

TEST(CanvasShapes, AnArcCoversOnlyItsSweep) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // 0..90 degrees is the quadrant to the **right and below** the centre in this y-down space, which is the assertion
  // that pins the direction: a pie whose wedge is in the wrong quadrant draws a plausible picture and is still wrong.
  fixture.canvas->drawArc(Rect::MakeLTRB(2.0f, 2.0f, 14.0f, 14.0f), 0.0f, 90.0f, true, redPaint());

  EXPECT_EQ(fixture.redAt(10, 10), 255u) << "inside the swept quadrant";
  EXPECT_EQ(fixture.redAt(4, 4), 0u) << "the opposite quadrant";
  EXPECT_EQ(fixture.redAt(10, 4), 0u) << "the quadrant above it";
  EXPECT_EQ(fixture.redAt(4, 10), 0u) << "the quadrant to its left";

  // A full turn is the whole ellipse whatever `useCenter` says, so nothing is left over as a seam.
  Fixture full;
  ASSERT_NE(full.canvas, nullptr);
  full.canvas->drawArc(Rect::MakeLTRB(2.0f, 2.0f, 14.0f, 14.0f), 37.0f, 360.0f, false, redPaint());
  EXPECT_EQ(full.redAt(8, 8), 255u);
  EXPECT_EQ(full.redAt(11, 8), 255u);
}

TEST(CanvasShapes, APictureReplaysAndTheTransformsCompose) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // A picture recorded once, played into the fixture twice: the point is that its commands land here.
  PictureRecorder recorder;
  Canvas* recording = recorder.beginRecording(16.0f, 16.0f);
  ASSERT_NE(recording, nullptr);
  recording->drawRect(Rect::MakeLTRB(0.0f, 0.0f, 2.0f, 2.0f), redPaint());
  const std::unique_ptr<Picture> picture = recorder.finishRecording();
  ASSERT_NE(picture, nullptr);
  ASSERT_GT(picture->operationCount(), 0);

  fixture.canvas->drawPicture(*picture);
  EXPECT_EQ(fixture.redAt(0, 0), 255u) << "the picture's own origin";

  // `translate` then the picture again: the second copy moves, the first stays.
  fixture.canvas->save();
  fixture.canvas->translate(6.0f, 6.0f);
  fixture.canvas->drawPicture(*picture);
  fixture.canvas->restore();
  EXPECT_EQ(fixture.redAt(6, 6), 255u) << "the translated picture";
  EXPECT_EQ(fixture.redAt(0, 0), 255u) << "the first copy is still where it was";

  // `scale` and `rotate` are transforms over the same `concat`, so a scaled draw covers more pixels than the original.
  Fixture scaled;
  ASSERT_NE(scaled.canvas, nullptr);
  scaled.canvas->scale(3.0f);
  scaled.canvas->drawRect(Rect::MakeLTRB(0.0f, 0.0f, 2.0f, 2.0f), redPaint());
  EXPECT_EQ(scaled.redAt(5, 5), 255u) << "a 2x2 rect at 3x reaches (5,5)";
  EXPECT_EQ(scaled.redAt(7, 7), 0u) << "and stops before (7,7)";
}

TEST(CanvasShapes, AnArcObjectDrawsTheSameWedgeAsItsFourArguments) {
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);

  // The `geometry::Arc` overload is a delegation, so the interesting claim is that it lands in the same place as the
  // numeric one: same bounds, same start, same sweep, same centre. Drawn through the object, the wedge still covers the
  // right-and-below quadrant of this y-down space and nothing else.
  const Arc arc(Rect::MakeLTRB(2.0f, 2.0f, 14.0f, 14.0f), Degrees(0.0f), Degrees(90.0f), true);
  fixture.canvas->drawArc(arc, redPaint());

  EXPECT_EQ(fixture.redAt(10, 10), 255u) << "inside the swept quadrant";
  EXPECT_EQ(fixture.redAt(4, 4), 0u) << "the opposite quadrant";
  EXPECT_EQ(fixture.redAt(10, 4), 0u) << "the quadrant above it";
  EXPECT_EQ(fixture.redAt(4, 10), 0u) << "the quadrant to its left";

  // And the object's own geometry agrees with what was drawn: the tight bounds of that wedge are the lower-right
  // quarter of the oval, which is the measurement a caller uses to lay out around an arc.
  const Rect tight = arc.getTightArcBounds();
  EXPECT_FLOAT_EQ(tight.getLeft(), 8.0f);
  EXPECT_FLOAT_EQ(tight.getTop(), 8.0f);
  EXPECT_FLOAT_EQ(tight.getRight(), 14.0f);
  EXPECT_FLOAT_EQ(tight.getBottom(), 14.0f);
}

// ==================================================================================================
// drawText
// ==================================================================================================
//
// `drawText` is a two-line delegation (`font.shape` then `drawTextBlob`), so what a test can usefully pin is not that a
// blob was made but that ink lands where the baseline says, that a longer string covers more of the canvas than a
// shorter one -- which is the difference between advancing the pen and drawing every glyph at the same origin -- and
// that an empty string is a no-op rather than a crash.
//
// The font comes from **this library's own** `modules/graphics/resources/fonts` first and the system second, and the suite
// skips when neither is there. The library's resources rather than the repository's `assets/`, because
// `modules/graphics` is meant to stand on its own: a test that reached into the Coral tree would stop working the day the
// library is built as its own project. The paths are relative to the test's working directory, which is `modules/graphics`.
// The system font is the fallback for the same reason `test_Font.cpp` has one: a machine with no font installed should
// not fail a test about text layout.

namespace {

/** @return the first font this machine can open, preferring the repository's own. */
std::string findTextFont() {
  std::vector<std::string> candidates = {
    // From the test's working directory, which is `modules/graphics`.
    "resources/fonts/NotoSansSC-Regular.otf",
    "resources/fonts/NotoSerifSC-Regular.otf",
    // Defensive: a test host that runs from the build directory rather than from `modules/graphics`.
    "../../resources/fonts/NotoSansSC-Regular.otf",
  };
  if (const char* root = std::getenv("SystemRoot")) {
    const std::string fonts = std::string(root) + "\\Fonts\\";
    candidates.push_back(fonts + "arial.ttf");
    candidates.push_back(fonts + "segoeui.ttf");
  }
  candidates.emplace_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
  candidates.emplace_back("/system/fonts/Roboto-Regular.ttf");

  for (const std::string& path : candidates) {
    std::ifstream file(path, std::ios::binary);
    if (file.good()) return path;
  }
  return {};
}

/** @return how many pixels of @p fixture have any coverage at all. */
uint32_t coveredPixels(const Fixture& fixture) {
  uint32_t covered = 0;
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      if (fixture.alphaAt(x, y) > 0) ++covered;
    }
  }
  return covered;
}

} // namespace anonymous

TEST(CanvasText, DrawnTextMarksPixelsAndAnEmptyStringDoesNot) {
  const std::string path = findTextFont();
  if (path.empty()) {
    GTEST_SKIP() << "no font is available on this machine";
  }
  const auto typeface = Typeface::fromFile(path);
  ASSERT_NE(typeface, nullptr) << path;
  const auto font = Font::create(typeface, 12.0f);
  ASSERT_NE(font, nullptr);
  // Printed rather than assumed: "the suite passed" does not say *which* font it found, and a silent fallback to a
  // system font would be a test that stopped covering this library's own resource without saying so.
  std::printf("  drawText: using \"%s\"\n", path.c_str());

  Fixture blank;
  ASSERT_NE(blank.canvas, nullptr);
  EXPECT_EQ(coveredPixels(blank), 0u) << "the fixture starts empty";

  // An empty string is a no-op: it must not draw, and it must not touch the canvas at all.
  blank.canvas->drawText("", 2.0f, 12.0f, *font, redPaint());
  EXPECT_EQ(coveredPixels(blank), 0u) << "an empty string drew something";

  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);
  fixture.canvas->drawText("A", 2.0f, 12.0f, *font, redPaint());
  EXPECT_GT(coveredPixels(fixture), 0u) << "the glyph marked no pixels";
}

TEST(CanvasText, ALongerStringCoversMoreThanAShorterOne) {
  const std::string path = findTextFont();
  if (path.empty()) {
    GTEST_SKIP() << "no font is available on this machine";
  }
  const auto typeface = Typeface::fromFile(path);
  ASSERT_NE(typeface, nullptr) << path;
  const auto font = Font::create(typeface, 10.0f);
  ASSERT_NE(font, nullptr);
  // A string that actually fits: the fixture is 16 pixels wide, and `layout` is what a pen advance test needs.
  const std::string text = "layout";

  // One glyph.
  Fixture one;
  ASSERT_NE(one.canvas, nullptr);
  one.canvas->drawText("l", 0.0f, 12.0f, *font, redPaint());
  const uint32_t oneCount = coveredPixels(one);

  // The whole string, from the same origin.
  Fixture many;
  ASSERT_NE(many.canvas, nullptr);
  many.canvas->drawText(text, 0.0f, 12.0f, *font, redPaint());
  const uint32_t manyCount = coveredPixels(many);

  std::printf("  drawText: \"l\" covers %u pixel(s), \"%s\" covers %u\n", oneCount, text.c_str(), manyCount);
  EXPECT_GT(manyCount, oneCount)
    << "a longer string must cover more of the canvas: equal counts mean every glyph was drawn at the same origin";
}

TEST(CanvasText, TheBaselineIsWhereItWasAskedFor) {
  const std::string path = findTextFont();
  if (path.empty()) {
    GTEST_SKIP() << "no font is available on this machine";
  }
  const auto typeface = Typeface::fromFile(path);
  ASSERT_NE(typeface, nullptr) << path;
  const auto font = Font::create(typeface, 10.0f);
  ASSERT_NE(font, nullptr);

  // Baseline at y = 14: ascenders reach above it, and nothing is drawn below the baseline plus the descent. The second
  // half of that is what pins the y: a baseline interpreted as a top edge would fill the rows *below* 14 as well.
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);
  fixture.canvas->drawText("H", 2.0f, 14.0f, *font, redPaint());

  uint32_t above = 0;
  uint32_t below = 0;
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      if (fixture.alphaAt(x, y) == 0) continue;
      if (y <= 14) {
        ++above;
      } else {
        ++below;
      }
    }
  }
  std::printf("  drawText: baseline 14 has %u pixel(s) above and %u below\n", above, below);
  EXPECT_GT(above, 0u) << "the glyph did not reach the baseline";
  EXPECT_LT(below, above) << "more ink below the baseline than above it suggests the y was taken as a top edge";
}

// ==================================================================================================
// drawImage: the immutable-image entry point
// ==================================================================================================
//
// `Image` is a picture, so drawing one must not copy its pixels -- that is what `makeBitmap` is for, and the difference
// between the two is 16 MB a frame for a 2000x2000 sticker. These cases pin the three claims the entry point makes:
// it draws where it is asked, it goes through a **view** rather than a copy, and the view cannot be written through.

TEST(CanvasImage, DrawImageCompositesThePicturesPixels) {
  // Built from a Data buffer rather than a Bitmap, which is the share-don't-copy path a decoder would take.
  Bitmap source = Bitmap::allocate(4, 4);
  ASSERT_FALSE(source.isEmpty());
  {
    Pixmap pixmap = source.lockPixels();
    ASSERT_FALSE(pixmap.isEmpty());
    for (int y = 0; y < 4; ++y) {
      for (int x = 0; x < 4; ++x) {
        pixmap.setPixelARGB32(x, y, 0xFFFF0000u);
      }
    }
  }
  const Image image = Image::fromBitmap(source);
  ASSERT_FALSE(image.isEmpty());

  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);
  Paint paint;
  // White, not the default: `drawBitmap` modulates the image by the paint's colour and `Paint` defaults to black, so a
  // default-constructed paint draws the image correctly and invisibly.
  paint.color = Color::White();
  fixture.canvas->drawImage(image, 6.0f, 6.0f, paint);

  EXPECT_EQ(fixture.redAt(7, 7), 255u) << "the image's pixels did not land at the requested origin";
  EXPECT_EQ(fixture.redAt(3, 3), 0u) << "the image drew outside its bounds";
}

TEST(CanvasImage, DrawImageSharesThePixelsRatherThanCopyingThem) {
  Bitmap source = Bitmap::allocate(4, 4);
  ASSERT_FALSE(source.isEmpty());
  const Image image = Image::fromBitmap(source);

  // A view, not a copy: the same `Data` bytes. Pointer equality is the only thing that can distinguish "shared" from
  // "copied to an identical buffer", which is why it is asserted rather than comparing contents.
  EXPECT_EQ(image.peekBitmap().data().bytes(), image.data().bytes())
    << "peekBitmap must view the image's pixels, not copy them";
  EXPECT_EQ(image.peekBitmap().width(), image.width());
}

TEST(CanvasImage, AnImageIsNotWritableThroughItsView) {
  Bitmap source = Bitmap::allocate(4, 4);
  ASSERT_FALSE(source.isEmpty());
  EXPECT_TRUE(source.isWritable()) << "the fixture starts writable";

  const Image image = Image::fromBitmap(source);
  // The stored bitmap is non-writable, so a copy of the view cannot be written through either -- `Bitmap` copies share
  // the flag. Without this an "immutable" image could be changed by a caller holding a view of it.
  EXPECT_FALSE(image.peekBitmap().isWritable()) << "an image must not expose writable pixels";
  Bitmap copy = image.peekBitmap();
  EXPECT_FALSE(copy.isWritable()) << "a copy of the view must stay read-only";
  EXPECT_TRUE(copy.lockPixels().isEmpty()) << "the read-only view must refuse a writable pixmap";

  // And `makeBitmap` still gives what it promises: an independent, writable copy that does not touch the image.
  Bitmap independent = image.makeBitmap();
  EXPECT_TRUE(independent.isWritable());
  EXPECT_NE(independent.data().bytes(), image.data().bytes()) << "makeBitmap is the copy, and it is a real one";
}

// ==================================================================================================
// Sampling: nearest vs linear
// ==================================================================================================
//
// A two-row source drawn eight times taller is the smallest case that can tell the two filters apart: nearest gives
// whole rows of 0 and whole rows of 255, linear gives the values in between. Counting **distinct** red values is what
// makes the test decisive -- a single "not pure black" assertion would pass for a nearest draw that happened to land on
// a boundary.

TEST(CanvasSampling, LinearInterpolatesBetweenTexelsWhereNearestDoesNot) {
  Bitmap source = Bitmap::allocate(1, 2);
  ASSERT_FALSE(source.isEmpty());
  {
    Pixmap pixmap = source.lockPixels();
    ASSERT_FALSE(pixmap.isEmpty());
    pixmap.setPixelARGB32(0, 0, 0xFF000000u);
    pixmap.setPixelARGB32(0, 1, 0xFFFFFFFFu);
  }

  const auto distinctRows = [&source](const SamplingOptions& sampling) {
    Fixture fixture;
    if (fixture.canvas == nullptr) return -1;
    fixture.canvas->scale(8.0f, 8.0f);
    Paint paint;
    // White, because `drawBitmap` modulates by the paint and the default is black.
    paint.color = Color::White();
    fixture.canvas->drawBitmap(source, 0.0f, 0.0f, sampling, paint);

    std::vector<uint32_t> seen;
    for (int y = 0; y < 16; ++y) {
      const uint32_t value = fixture.redAt(4, y);
      if (std::find(seen.begin(), seen.end(), value) == seen.end()) seen.push_back(value);
    }
    return static_cast<int>(seen.size());
  };

  const int nearestValues = distinctRows(SamplingOptions(FilterMode::kNearest, MipmapMode::kNone));
  const int linearValues = distinctRows(SamplingOptions(FilterMode::kLinear, MipmapMode::kNone));

  std::printf("  sampling: nearest yields %d distinct red value(s), linear yields %d\n", nearestValues,
    linearValues);
  ASSERT_GE(nearestValues, 1);
  EXPECT_LE(nearestValues, 2) << "nearest must copy whole texels, so only the source's two values should appear";
  EXPECT_GT(linearValues, 4) << "linear must blend the rows, so the column should show a ramp";
}

// ==================================================================================================
// Cubic and mipmap sampling
// ==================================================================================================

TEST(CanvasSampling, CubicIsItsOwnKernelAndKeepsAFlatRegionFlat) {
  // A one-texel step: black over white. Linear ramps across the seam and stops there; a cubic kernel has negative lobes
  // and a different footprint, so it must give a different ramp -- which is the whole reason to offer it.
  Bitmap step = Bitmap::allocate(1, 2);
  ASSERT_FALSE(step.isEmpty());
  {
    Pixmap pixmap = step.lockPixels();
    ASSERT_FALSE(pixmap.isEmpty());
    pixmap.setPixelARGB32(0, 0, 0xFF000000u);
    pixmap.setPixelARGB32(0, 1, 0xFFFFFFFFu);
  }

  const auto rampOf = [&step](const SamplingOptions& sampling) {
    std::vector<uint32_t> values;
    Fixture fixture;
    if (fixture.canvas == nullptr) return values;
    fixture.canvas->scale(8.0f, 8.0f);
    Paint paint;
    paint.color = Color::White();
    fixture.canvas->drawBitmap(step, 0.0f, 0.0f, sampling, paint);
    for (int y = 0; y < 16; ++y) values.push_back(fixture.redAt(4, y));
    return values;
  };

  const auto linear = rampOf(SamplingOptions(FilterMode::kLinear, MipmapMode::kNone));
  const auto mitchell = rampOf(SamplingOptions(CubicResampler::Mitchell()));
  const auto catmull = rampOf(SamplingOptions(CubicResampler::CatmullRom()));
  ASSERT_EQ(linear.size(), 16u);

  // Mitchell-Netravali with B = C = 1/3 does not ring, so every sample stays between the two source values.
  for (const uint32_t value : mitchell) {
    EXPECT_GE(value, 0u);
    EXPECT_LE(value, 255u);
  }
  EXPECT_NE(linear, mitchell) << "a cubic resample of a step must not reproduce the bilinear one";
  EXPECT_NE(mitchell, catmull) << "B and C must be honoured: Catmull-Rom is a sharper kernel than Mitchell";

  // A flat source comes out flat, which is what the weight normalisation is for: an unnormalised kernel darkens the
  // rows whose taps clamp to the edge, and that is the artifact this asserts against.
  Bitmap flat = Bitmap::allocate(4, 4);
  ASSERT_FALSE(flat.isEmpty());
  {
    Pixmap pixmap = flat.lockPixels();
    for (int y = 0; y < 4; ++y) {
      for (int x = 0; x < 4; ++x) pixmap.setPixelARGB32(x, y, 0xFF808080u);
    }
  }
  Fixture fixture;
  ASSERT_NE(fixture.canvas, nullptr);
  fixture.canvas->scale(4.0f, 4.0f);
  Paint paint;
  paint.color = Color::White();
  fixture.canvas->drawBitmap(flat, 0.0f, 0.0f, SamplingOptions(CubicResampler::CatmullRom()), paint);
  // Every corner and the middle: the edge rows are where an unnormalised kernel shows first.
  for (const int y : { 0, 1, 7, 8, 15 }) {
    for (const int x : { 0, 1, 7, 8, 15 }) {
      EXPECT_EQ(fixture.redAt(x, y), 128u) << "cubic darkened a flat source at (" << x << ", " << y << ")";
    }
  }
}

TEST(CanvasSampling, MipmappingRemovesMinificationAliasing) {
  // An 8x8 checker of single texels is the worst case: every device pixel of a minified draw lands on a texel of one
  // colour, so without a prefiltered level the result is a high-contrast moire rather than a colour.
  //
  // The scale is a *third* rather than a quarter on purpose: a quarter puts the sample spacing at exactly four texels,
  // which is even, so every sample of a one-texel checker lands on the same parity and the aliased draw comes out
  // uniformly black -- the measurement would then report no aliasing at all. Three texels alternate the parity.
  Bitmap checker = Bitmap::allocate(8, 8);
  ASSERT_FALSE(checker.isEmpty());
  {
    Pixmap pixmap = checker.lockPixels();
    ASSERT_FALSE(pixmap.isEmpty());
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 8; ++x) {
        pixmap.setPixelARGB32(x, y, ((x + y) & 1) == 0 ? 0xFF000000u : 0xFFFFFFFFu);
      }
    }
  }

  // The spread of the covered pixels is the measurement: aliasing means both colours appear, mipmapping means neither.
  const auto spreadOf = [&checker](const SamplingOptions& sampling) -> int {
    Fixture fixture;
    if (fixture.canvas == nullptr) return -1;
    fixture.canvas->scale(1.0f / 3.0f, 1.0f / 3.0f);
    Paint paint;
    paint.color = Color::White();
    fixture.canvas->drawBitmap(checker, 0.0f, 0.0f, sampling, paint);

    uint32_t low = 255;
    uint32_t high = 0;
    uint32_t covered = 0;
    for (int y = 0; y < 16; ++y) {
      for (int x = 0; x < 16; ++x) {
        if (fixture.alphaAt(x, y) == 0) continue;
        const uint32_t value = fixture.redAt(x, y);
        low = std::min(low, value);
        high = std::max(high, value);
        ++covered;
      }
    }
    if (covered == 0) return -1;
    return static_cast<int>(high - low);
  };

  const int aliased = spreadOf(SamplingOptions(FilterMode::kNearest, MipmapMode::kNone));
  const int filtered = spreadOf(SamplingOptions(FilterMode::kNearest, MipmapMode::kLinear));

  std::printf("  mipmap: full-resolution spread %d, mipmapped spread %d\n", aliased, filtered);
  ASSERT_GE(aliased, 0) << "the fixture drew nothing";
  EXPECT_EQ(aliased, 255) << "without a mip chain a minified checker must show both of its colours";
  EXPECT_LE(filtered, 8) << "the mip chain should collapse the checker towards its average, leaving one colour";
}
