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
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

/**
 * @file test_SvgFilter.cpp
 *
 * SVG's filter primitives and the chain that connects them.
 *
 * `applyFilter` is deliberately a plain function over a bitmap: the element's own rendering goes in, the filtered picture
 * comes out. That makes every assertion here about pixels rather than about a rendered document, which is the right
 * level for arithmetic -- a matrix that is one row out gives a picture that is wrong in a way a document comparison
 * would only report as "different".
 *
 * The chain is asserted as carefully as the primitives. Of the corpus's 37 `in` references, 21 name nothing at all, and
 * "nothing" means *the previous primitive's result* -- a rule that is invisible in a single-primitive test and decides
 * the whole of a drop shadow.
 */

#include "svg/SvgFilter.h"

#include <graphics/Pixmap.h>
#include <graphics/Surface.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using graphics::Bitmap;
using graphics::Color;
using graphics::Rect;
using graphics::Scalar;
using graphics::Surface;
using graphics::svg::applyFilter;
using graphics::svg::Filter;
using graphics::svg::FilterPrimitive;
using graphics::svg::Length;
using graphics::svg::LengthContext;
using graphics::svg::LengthUnit;

constexpr int kSize = 40;

/** A canvas-sized bitmap holding a picture drawn by @p draw. */
template <typename Draw>
Bitmap source(const Draw& draw) {
  auto surface = Surface::makeRaster(kSize, kSize);
  if (surface == nullptr) return {};
  draw(*surface->getCanvas());
  return surface->detachBitmap();
}

/** A bitmap holding one opaque red square covering @p width by @p height at the origin. */
Bitmap redSquare(const int width = 20, const int height = 20) {
  return source([width, height](graphics::Canvas& canvas) {
    graphics::Paint paint;
    paint.color = Color::MakeRGBA8(255, 0, 0, 255);
    canvas.drawRect(Rect::MakeLTRB(0, 0, static_cast<Scalar>(width), static_cast<Scalar>(height)), paint);
  });
}

/** The filter's default region covers the whole surface here, which keeps the arithmetic about the primitive. */
Filter oneStep(FilterPrimitive primitive) {
  Filter filter;
  filter.objectBoundingBox = false;
  filter.x = Length {0, LengthUnit::Px};
  filter.y = Length {0, LengthUnit::Px};
  filter.width = Length {static_cast<Scalar>(kSize), LengthUnit::Px};
  filter.height = Length {static_cast<Scalar>(kSize), LengthUnit::Px};
  filter.primitives.push_back(std::move(primitive));
  return filter;
}

/** Runs @p filter over @p bitmap with the whole device as its bounds. */
Bitmap filtered(const Filter& filter, const Bitmap& bitmap) {
  const auto result = applyFilter(filter, bitmap, Rect::MakeLTRB(0, 0, kSize, kSize), LengthContext {}, 1.0f,
                                  Rect::MakeLTRB(0, 0, kSize, kSize));
  return result.value_or(Bitmap {});
}

uint32_t at(const Bitmap& bitmap, const int x, const int y) { return bitmap.makePixmap().getPixelARGB32(x, y); }

int channel(const Bitmap& bitmap, const int x, const int y, const int shift) {
  return static_cast<int>((at(bitmap, x, y) >> shift) & 0xFFu);
}

// ==================================================================================================
// feColorMatrix
// ==================================================================================================

TEST(SvgFilterColorMatrix, AppliesTheCorpusAlphaInversionExactly) {
  // The only filter in the corpus that stands alone: `mask_invert_picture_image.svg` writes the matrix
  // `1 0 0 0 0 / 0 1 0 0 0 / 0 0 1 0 0 / 0 0 0 -1 1`, which is `alpha' = 1 - alpha` and leaves the colour alone. It
  // turns a picture into its own negative coverage, which is what an inverted mask is built from.
  FilterPrimitive primitive;
  primitive.kind = FilterPrimitive::Kind::ColorMatrix;
  primitive.values = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, -1, 1};

  const Bitmap out = filtered(oneStep(primitive), redSquare());
  ASSERT_FALSE(out.isEmpty());

  EXPECT_EQ(channel(out, 5, 5, 24), 0) << "the square was opaque, so the inversion leaves it transparent";
  EXPECT_EQ(channel(out, 30, 30, 24), 255) << "and where the source was transparent, opaque";
  EXPECT_LT(channel(out, 30, 30, 16), 8) << "black, since the colour rows are the identity on a zero source";
}

TEST(SvgFilterColorMatrix, LeavesTheColourAloneWhenOnlyTheAlphaRowChanges) {
  // The inversion's RGB rows are the identity, so a partly transparent red stays red -- at its own alpha, not at the
  // inverted one. Asserted in the region the source covers and the output does not, which needs a source whose alpha is
  // between the two.
  FilterPrimitive primitive;
  primitive.kind = FilterPrimitive::Kind::ColorMatrix;
  // A no-op matrix: the identity with no constant terms.
  primitive.values = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0};

  const Bitmap out = filtered(oneStep(primitive), redSquare());
  EXPECT_EQ(channel(out, 5, 5, 16), 255) << "still red";
  EXPECT_EQ(channel(out, 5, 5, 24), 255) << "and still opaque";
}

TEST(SvgFilterColorMatrix, TheConstantTermsShiftEachChannelIndependently) {
  // The fifth column is a constant added to the channel. Setting the red constant to 1 makes every pixel red regardless
  // of what was there -- which is the assertion that the column is read from the right place rather than being folded
  // into the wrong row.
  FilterPrimitive primitive;
  primitive.kind = FilterPrimitive::Kind::ColorMatrix;
  // The last row carries the alpha through, and it has to: the pixel setter premultiplies, so a colour whose alpha
  // is zero reads back as zero however large the colour row made it. An earlier version of this test set the red
  // constant and left the alpha row zero, then expected to see red.
  primitive.values = {0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1};

  const Bitmap out = filtered(oneStep(primitive), redSquare());
  EXPECT_EQ(channel(out, 5, 5, 16), 255) << "red everywhere, from the constant";
  EXPECT_EQ(channel(out, 5, 5, 8), 0);
  EXPECT_EQ(channel(out, 5, 5, 0), 0);
  EXPECT_EQ(channel(out, 30, 30, 16), 255) << "including where the source had nothing";
}

TEST(SvgFilterColorMatrix, LuminanceToAlphaPutsTheBrightnessIntoTheAlpha) {
  // The odd type out: it discards the colour and makes the luminance the alpha. A red square has a luminance of 0.2125,
  // and that is what the alpha becomes -- so this also pins the Rec.709 weights.
  FilterPrimitive primitive;
  primitive.kind = FilterPrimitive::Kind::ColorMatrix;
  primitive.colorMatrixType = FilterPrimitive::ColorMatrixType::LuminanceToAlpha;

  const Bitmap out = filtered(oneStep(primitive), redSquare());
  EXPECT_NEAR(channel(out, 5, 5, 24), 54, 4) << "red's luminance, 0.2125 of 255";
  EXPECT_EQ(channel(out, 5, 5, 16), 0) << "and the colour is gone";
}

TEST(SvgFilterColorMatrix, SaturateZeroLeavesGrey) {
  // The saturate matrix with a factor of zero is the luminance weights applied to every channel, so a red square comes
  // out grey at its own luminance.
  FilterPrimitive primitive;
  primitive.kind = FilterPrimitive::Kind::ColorMatrix;
  primitive.colorMatrixType = FilterPrimitive::ColorMatrixType::Saturate;
  primitive.values[0] = 0.0f;

  const Bitmap out = filtered(oneStep(primitive), redSquare());
  const int red = channel(out, 5, 5, 16);
  const int green = channel(out, 5, 5, 8);
  const int blue = channel(out, 5, 5, 0);
  EXPECT_NEAR(red, 54, 4) << "the luminance of red";
  EXPECT_EQ(red, green) << "all three channels equal, which is what grey means";
  EXPECT_EQ(green, blue);
}

// ==================================================================================================
// feFlood, feOffset
// ==================================================================================================

TEST(SvgFilterFlood, FillsTheRegionWithItsColour) {
  // A flood generates rather than transforms: it has no input at all, and its output is its colour over the region. The
  // corpus uses it as a transparent backdrop (`flood-opacity="0"`) and as a solid one (`complex4`).
  FilterPrimitive primitive;
  primitive.kind = FilterPrimitive::Kind::Flood;
  primitive.floodColor = Color {0.0f, 0.0f, 1.0f, 1.0f};
  primitive.floodOpacity = 1.0f;

  const Bitmap out = filtered(oneStep(primitive), redSquare());
  ASSERT_FALSE(out.isEmpty());
  EXPECT_EQ(channel(out, 5, 5, 0), 255) << "blue at the source's own pixel";
  EXPECT_EQ(channel(out, 30, 30, 0), 255) << "and outside it, because a flood covers the region";
  EXPECT_EQ(channel(out, 5, 5, 16), 0) << "and it is not the source's red";
}

TEST(SvgFilterFlood, HonoursItsOpacity) {
  // `flood-opacity` is a fraction rather than a percentage, unlike almost every other opacity in SVG. Half is a
  // half-transparent colour, not a fully opaque one.
  FilterPrimitive primitive;
  primitive.kind = FilterPrimitive::Kind::Flood;
  primitive.floodColor = Color {1.0f, 0.0f, 0.0f, 1.0f};
  primitive.floodOpacity = 0.5f;

  const Bitmap out = filtered(oneStep(primitive), redSquare());
  EXPECT_NEAR(channel(out, 30, 30, 24), 128, 4) << "half opaque outside the source";
}

TEST(SvgFilterOffset, MovesTheInputByItsVector) {
  // `dx` and `dy` in user units, which at a scale of one are device pixels.
  FilterPrimitive primitive;
  primitive.kind = FilterPrimitive::Kind::Offset;
  primitive.dx = 10;
  primitive.dy = 0;

  const Bitmap out = filtered(oneStep(primitive), redSquare());
  EXPECT_EQ(channel(out, 5, 5, 24), 0) << "the source's own pixel is now empty";
  EXPECT_EQ(channel(out, 25, 5, 24), 255) << "and the square is at its new place";
  EXPECT_EQ(channel(out, 15, 5, 24), 255) << "covering the gap it moved through";
}

TEST(SvgFilterOffset, ScalesWithTheCanvas) {
  // An offset of five user units is ten device pixels at 2x. The buffers are device-sized, so this is the conversion
  // that would be wrong if `scale` were ignored -- and it is only visible at a scale other than one.
  FilterPrimitive primitive;
  primitive.kind = FilterPrimitive::Kind::Offset;
  primitive.dx = 5;

  Filter filter = oneStep(primitive);
  const auto result = applyFilter(filter, redSquare(), Rect::MakeLTRB(0, 0, kSize, kSize), LengthContext {}, 2.0f,
                                  Rect::MakeLTRB(0, 0, kSize, kSize));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(channel(*result, 5, 5, 24), 0);
  EXPECT_EQ(channel(*result, 25, 5, 24), 255) << "ten device pixels along, not five";
  EXPECT_EQ(channel(*result, 15, 5, 24), 255) << "and the ten-pixel gap it left is covered";
}

// ==================================================================================================
// feGaussianBlur
// ==================================================================================================

TEST(SvgFilterBlur, SpreadsTheEdgeOutwards) {
  // A blur softens the boundary: just outside the square there is now some coverage, and just inside it there is less
  // than full.
  FilterPrimitive primitive;
  primitive.kind = FilterPrimitive::Kind::Blur;
  primitive.deviationX = 2;
  primitive.deviationY = 2;

  const Bitmap out = filtered(oneStep(primitive), redSquare());
  ASSERT_FALSE(out.isEmpty());

  // Ten pixels in, which is five deviations from either edge: the Gaussian has gathered almost all its mass by then, so
  // the middle really is solid. An earlier version asked this at five pixels in and was wrong -- 2.5 deviations is still
  // inside the falloff, but only just, and the answer rounded to fully opaque.
  EXPECT_EQ(channel(out, 10, 10, 24), 255) << "the middle is solid";

  // The *square's* edge -- x = 19, one deviation inside it -- is where the falloff shows: part of the distribution
  // lies beyond the boundary at x = 20, so the coverage is well below full and well above nothing.
  //
  // Not x = 0, which is what an earlier version of this test asked about. That is the *buffer's* edge, and with the
  // default `edgeMode` of `duplicate` the samples outside are clamped back to the edge pixel, so a shape reaching the
  // boundary stays fully covered there. The blur softens the shape's edge, not the buffer's, and the two coincide only
  // when the shape runs to the boundary.
  const int edge = channel(out, 19, 10, 24);
  EXPECT_LT(edge, 240) << "the square's edge is no longer full";
  EXPECT_GT(edge, 100) << "but it is not empty either";

  EXPECT_EQ(channel(out, 0, 10, 24), 255)
          << "the buffer's own edge stays full, which is `duplicate` clamping rather than a shape";

  // And the blur reaches past where the square ended.
  EXPECT_GT(channel(out, 22, 10, 24), 0) << "the edge spreads outwards";
  EXPECT_LT(channel(out, 22, 10, 24), 100) << "faintly rather than fully";
}

TEST(SvgFilterBlur, DoesNotPullColourOutOfNothing) {
  // The reason the blur premultiplies. A transparent pixel beside a red one holds no colour, so it must contribute none;
  // blurring the un-premultiplied channels instead puts a bright halo around every edge, and `blur.svg` is a blur demo.
  //
  // Just outside the square the coverage rises from zero, so the colour must fall away with it rather than staying at
  // full red -- a halo is exactly a pixel that is faint but saturated.
  FilterPrimitive primitive;
  primitive.kind = FilterPrimitive::Kind::Blur;
  primitive.deviationX = 3;
  primitive.deviationY = 3;

  const Bitmap out = filtered(oneStep(primitive), redSquare());
  for (int x = 22; x < 30; ++x) {
    const int alpha = channel(out, x, 10, 24);
    const int red = channel(out, x, 10, 16);
    // Un-premultiplied, so a correct blur keeps red at 255 and lets the alpha fall. What must *not* happen is colour
    // appearing where the source had none and the alpha is zero.
    if (alpha == 0) {
      EXPECT_EQ(red, 0) << "at x = " << x << " there is no coverage and so no colour";
    }
  }
}

TEST(SvgFilterBlur, ADEviationOfZeroChangesNothing) {
  // `complex4.svg` writes `stdDeviation="0"` to mean a drop shadow with no spread, so the identity case is a real one
  // rather than a degenerate input.
  FilterPrimitive primitive;
  primitive.kind = FilterPrimitive::Kind::Blur;
  primitive.deviationX = 0;
  primitive.deviationY = 0;

  const Bitmap before = redSquare();
  const Bitmap out = filtered(oneStep(primitive), before);
  EXPECT_EQ(channel(out, 5, 5, 16), channel(before, 5, 5, 16));
  EXPECT_EQ(channel(out, 25, 10, 24), channel(before, 25, 10, 24)) << "and nothing spread";
}

// ==================================================================================================
// feBlend and feComposite
// ==================================================================================================

TEST(SvgFilterBlend, NormalPutsTheForegroundOverTheBackground) {
  FilterPrimitive matcher;
  matcher.kind = FilterPrimitive::Kind::Blend;
  matcher.input = "SourceAlpha";
  matcher.input2 = "SourceGraphic";
  matcher.blend = FilterPrimitive::Blend::Normal;

  const Bitmap out = filtered(oneStep(matcher), redSquare());
  ASSERT_FALSE(out.isEmpty());
  // SourceAlpha is black at the square's coverage, drawn over the red square: the square is covered by black.
  EXPECT_EQ(channel(out, 5, 5, 16), 0) << "the black foreground is on top";
  EXPECT_EQ(channel(out, 5, 5, 24), 255) << "and it is opaque";
}

TEST(SvgFilterComposite, InUsesTheSecondInputsAlpha) {
  // `operator="in"` keeps the foreground only where the background covers -- `SrcIn`, and the operation `complex4.svg`
  // uses to colour a shadow by a flood. With the source as the foreground and the source as the background, the result
  // is the source.
  FilterPrimitive primitive;
  primitive.kind = FilterPrimitive::Kind::Composite;
  primitive.composite = FilterPrimitive::Composite::In;
  primitive.input = "SourceGraphic";
  primitive.input2 = "SourceGraphic";

  const Bitmap out = filtered(oneStep(primitive), redSquare());
  EXPECT_EQ(channel(out, 5, 5, 16), 255) << "inside the square, kept";
  EXPECT_EQ(channel(out, 5, 5, 24), 255);
  EXPECT_EQ(channel(out, 30, 30, 24), 0) << "and outside it, gone";
}

TEST(SvgFilterComposite, OutKeepsTheForegroundOnlyWhereTheBackgroundDoesNot) {
  FilterPrimitive primitive;
  primitive.kind = FilterPrimitive::Kind::Composite;
  primitive.composite = FilterPrimitive::Composite::Out;
  primitive.input = "SourceGraphic";
  primitive.input2 = "SourceGraphic";

  const Bitmap out = filtered(oneStep(primitive), redSquare());
  EXPECT_EQ(channel(out, 5, 5, 24), 0) << "inside, subtracted away";
  EXPECT_EQ(channel(out, 30, 30, 24), 0) << "and outside there was nothing to keep";
}

TEST(SvgFilterComposite, ArithmeticComputesTheCorpusInnerShadow) {
  // `complex2.svg`'s inner shadow: `operator="arithmetic" k2="-1" k3="1"` with `in` = the blurred offset alpha and
  // `in2` = SourceAlpha. The formula is `k2*i1 + k3*i2`, so the result is `SourceAlpha - blurredOffsetAlpha` -- the
  // coverage the shadow does *not* reach. With both inputs the same source it comes out as zero everywhere, which is
  // the value that says the two terms cancel rather than the formula folding to one of them.
  FilterPrimitive primitive;
  primitive.kind = FilterPrimitive::Kind::Composite;
  primitive.composite = FilterPrimitive::Composite::Arithmetic;
  primitive.input = "SourceAlpha";
  primitive.input2 = "SourceAlpha";
  primitive.k2 = -1;
  primitive.k3 = 1;

  const Bitmap out = filtered(oneStep(primitive), redSquare());
  ASSERT_FALSE(out.isEmpty());
  EXPECT_EQ(channel(out, 5, 5, 24), 0) << "equal and opposite, so nothing survives";
  EXPECT_EQ(channel(out, 30, 30, 24), 0);
}

TEST(SvgFilterComposite, ArithmeticHonoursEveryConstant) {
  // `k4` alone is a constant, so a result of a quarter everywhere says the constant term is read and applied.
  FilterPrimitive primitive;
  primitive.kind = FilterPrimitive::Kind::Composite;
  primitive.composite = FilterPrimitive::Composite::Arithmetic;
  primitive.input = "SourceGraphic";
  primitive.input2 = "SourceGraphic";
  primitive.k4 = 0.25f;

  const Bitmap out = filtered(oneStep(primitive), redSquare());
  EXPECT_NEAR(channel(out, 5, 5, 16), 64, 3) << "a quarter of red";
  EXPECT_NEAR(channel(out, 30, 30, 16), 64, 3) << "everywhere, since the other terms are zero";
}

// ==================================================================================================
// The chain, which is the part a single-primitive test cannot reach
// ==================================================================================================

TEST(SvgFilterChain, AnUnspecifiedInputReadsThePreviousResult) {
  // The corpus's most common case: 21 of 37 `in` references name nothing. A flood followed by a blur must blur the
  // *flood*, not the source -- so if the chain were wrong the picture would be a blurred square instead of a soft field
  // of colour.
  FilterPrimitive flood;
  flood.kind = FilterPrimitive::Kind::Flood;
  flood.floodColor = Color {0.0f, 0.0f, 1.0f, 1.0f};

  FilterPrimitive blur;
  blur.kind = FilterPrimitive::Kind::Blur;
  blur.deviationX = 4;  // No `in`: blurred, and the flood is what it blurs.

  Filter filter = oneStep(std::move(flood));
  filter.primitives.push_back(blur);

  const Bitmap out = filtered(filter, redSquare());
  ASSERT_FALSE(out.isEmpty());
  EXPECT_EQ(channel(out, 5, 5, 0), 255) << "blue, which only the flood could have provided";
  EXPECT_EQ(channel(out, 5, 5, 16), 0) << "and not the source's red";
}

TEST(SvgFilterChain, AResultIsReferencedByName) {
  // Two floods, the second reading the first by name. The named one must win, which is what tells a lookup from a
  // fall-through to the previous result -- those differ only when a primitive names something other than the last one.
  FilterPrimitive first;
  first.kind = FilterPrimitive::Kind::Flood;
  first.floodColor = Color {1.0f, 0.0f, 0.0f, 1.0f};
  first.result = "red";

  FilterPrimitive second;
  second.kind = FilterPrimitive::Kind::Flood;
  second.floodColor = Color {0.0f, 1.0f, 0.0f, 1.0f};

  FilterPrimitive third;
  third.kind = FilterPrimitive::Kind::Flood;
  third.floodColor = Color {0.0f, 0.0f, 1.0f, 1.0f};
  third.result = "blue";

  // A blur reading "blue" must take the third flood rather than the one before it.
  FilterPrimitive blur;
  blur.kind = FilterPrimitive::Kind::Blur;
  blur.input = "blue";
  blur.deviationX = 1;

  Filter filter = oneStep(std::move(first));
  filter.primitives.push_back(second);
  filter.primitives.push_back(third);
  filter.primitives.push_back(blur);

  const Bitmap out = filtered(filter, redSquare());
  ASSERT_FALSE(out.isEmpty());
  EXPECT_EQ(channel(out, 5, 5, 0), 255) << "blue, from the named result rather than the previous one";
  EXPECT_EQ(channel(out, 5, 5, 8), 0) << "and not green, which was the previous primitive";
}

TEST(SvgFilterChain, SourceAlphaIsTheCoverageWithoutTheColour) {
  // A real buffer rather than a special case: black at the source's own alpha. `complex2.svg` and `complex7.svg` both
  // build their shadows from it, so it has to be the coverage rather than the picture.
  FilterPrimitive primitive;
  primitive.kind = FilterPrimitive::Kind::Flood;
  primitive.input = "SourceAlpha";
  primitive.floodColor = Color {0.0f, 1.0f, 0.0f, 0.0f};  // Wholly transparent.

  const Bitmap out = filtered(oneStep(primitive), redSquare());
  EXPECT_EQ(channel(out, 5, 5, 24), 0) << "a transparent flood over the region";

  // Read the alpha into the colour instead, which needs a matrix -- and that is the assertion that `SourceAlpha` is
  // black-with-coverage rather than the picture.
  FilterPrimitive matrix;
  matrix.kind = FilterPrimitive::Kind::ColorMatrix;
  matrix.input = "SourceAlpha";
  // Move alpha into red: `red' = alpha`.
  // The alpha row is the identity, for the reason the constant-term test above records -- zeroing it would
  // premultiply the result to nothing and there would be no red to read.
  matrix.values = {0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0};

  Filter filter = oneStep(matrix);
  const Bitmap fromAlpha = filtered(filter, redSquare());
  ASSERT_FALSE(fromAlpha.isEmpty());
  EXPECT_EQ(channel(fromAlpha, 5, 5, 16), 255) << "the coverage became red, at the square's own pixels";
  EXPECT_EQ(channel(fromAlpha, 30, 30, 16), 0) << "and not outside it";
}

TEST(SvgFilterChain, AMissingResultNameAppendsNothing) {
  // A name nothing produced is a document error. Refused rather than falling back to the previous result, because the
  // two give different pictures and only one of them is what the document asked for.
  FilterPrimitive primitive;
  primitive.kind = FilterPrimitive::Kind::Blur;
  primitive.input = "no-such-result";

  const auto result = applyFilter(oneStep(primitive), redSquare(), Rect::MakeLTRB(0, 0, kSize, kSize), LengthContext {},
                                  1.0f, Rect::MakeLTRB(0, 0, kSize, kSize));
  EXPECT_FALSE(result.has_value()) << "the filter could not be built, which the caller reads as no filter";
}

TEST(SvgFilterChain, AnEmptyFilterIsNotApplied) {
  // A `<filter>` with no primitives has nothing to do. Reported as nothing rather than as an unchanged copy, so the
  // caller can tell "no filter" from "a filter that happened to change nothing".
  const Filter empty;
  EXPECT_TRUE(empty.isEmpty());
  EXPECT_FALSE(applyFilter(empty, redSquare(), Rect::MakeLTRB(0, 0, kSize, kSize), LengthContext {}, 1.0f,
                           Rect::MakeLTRB(0, 0, kSize, kSize))
                       .has_value());
}

// ==================================================================================================
// The region
// ==================================================================================================

TEST(SvgFilterRegion, DefaultsToAMarginAroundTheElement) {
  // A filter that states none of the four attributes still has a region: -10% to 120% of the element's box. A document
  // relying on that is relying on a margin, and reading the defaults as "the whole canvas" would make every filter write
  // over the whole picture.
  FilterPrimitive primitive;
  primitive.kind = FilterPrimitive::Kind::Flood;
  primitive.floodColor = Color {0.0f, 1.0f, 0.0f, 1.0f};

  Filter filter;  // No region stated, and `objectBoundingBox` by default.
  filter.primitives.push_back(primitive);

  // The element's box is a 20x20 square at the origin, so the region is -2 to 22 in both directions.
  const Bitmap out =
          applyFilter(filter, redSquare(), Rect::MakeLTRB(0, 0, 20, 20), LengthContext {}, 1.0f,
                      Rect::MakeLTRB(0, 0, kSize, kSize))
                  .value_or(Bitmap {});

  ASSERT_FALSE(out.isEmpty());
  EXPECT_EQ(channel(out, 10, 10, 8), 255) << "inside the element, flooded";
  EXPECT_EQ(channel(out, 30, 30, 8), 0) << "and outside the margin, which the flood does not reach";
}

TEST(SvgFilterRegion, AUserSpaceRegionIsInUserUnits) {
  // `filterUnits="userSpaceOnUse"` -- which 9 of this corpus's 13 filters say -- means the region's numbers are user
  // units rather than fractions of the box. The same numbers therefore mean different things, and this is the assertion
  // that separates the two readings.
  FilterPrimitive primitive;
  primitive.kind = FilterPrimitive::Kind::Flood;
  primitive.floodColor = Color {0.0f, 1.0f, 0.0f, 1.0f};

  Filter filter;
  filter.objectBoundingBox = false;
  filter.x = Length {20, LengthUnit::Px};
  filter.y = Length {20, LengthUnit::Px};
  filter.width = Length {10, LengthUnit::Px};
  filter.height = Length {10, LengthUnit::Px};
  filter.primitives.push_back(primitive);

  const Bitmap out =
          applyFilter(filter, redSquare(), Rect::MakeLTRB(0, 0, 20, 20), LengthContext {}, 1.0f,
                      Rect::MakeLTRB(0, 0, kSize, kSize))
                  .value_or(Bitmap {});

  ASSERT_FALSE(out.isEmpty());
  EXPECT_EQ(channel(out, 25, 25, 8), 255) << "inside the region the document stated";
  EXPECT_EQ(channel(out, 5, 5, 8), 0) << "and not before it, where the element itself is";
}

} // namespace anonymous
