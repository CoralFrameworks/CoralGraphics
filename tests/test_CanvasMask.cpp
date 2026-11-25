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
 * @file test_CanvasMask.cpp
 *
 * Masking a layer by a coverage image.
 *
 * The two modes agree on the commonest mask -- white shapes on nothing -- so a test that only used one of those would
 * pass whichever rule was implemented. The cases below are the ones where they disagree: a mask whose colour varies
 * while its alpha does not, and a mask that is transparent where it is white. Those are the masks a document actually
 * writes when it wants a fade, and they are why the mode is a parameter rather than a constant.
 */

#include <graphics/Canvas.h>
#include <graphics/Paint.h>
#include <graphics/Path.h>
#include <graphics/Pixmap.h>
#include <graphics/Surface.h>

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

namespace {

using graphics::Bitmap;
using graphics::Canvas;
using graphics::Color;
using graphics::MaskMode;
using graphics::Paint;
using graphics::Path;
using graphics::Point;
using graphics::Rect;
using graphics::Surface;

/**
 * A mask image of one colour, at a uniform alpha.
 *
 * Built through a surface rather than by writing bytes, so the test exercises the same premultiplication the canvas
 * does. A mask written directly as premultiplied bytes would let a mistake in the premultiply pass unnoticed.
 */
Bitmap uniformMask(const int size, const uint32_t grey, const uint8_t alpha) {
  auto surface = Surface::makeRaster(size, size);
  if (surface == nullptr) return Bitmap {};

  const auto canvas = surface->getCanvas();
  Paint paint;
  paint.color = Color::MakeRGBA8(static_cast<uint8_t>(grey), static_cast<uint8_t>(grey), static_cast<uint8_t>(grey),
                                 alpha);
  canvas->drawRect(Rect::MakeWH(static_cast<graphics::Scalar>(size), static_cast<graphics::Scalar>(size)), paint);
  return surface->detachBitmap();
}

/** A mask split left and right: @p leftGrey on one half, @p rightGrey on the other, both opaque. */
Bitmap splitMask(const int size, const uint32_t leftGrey, const uint32_t rightGrey) {
  auto surface = Surface::makeRaster(size, size);
  if (surface == nullptr) return Bitmap {};

  const auto canvas = surface->getCanvas();
  Paint left;
  left.color = Color::MakeRGBA8(static_cast<uint8_t>(leftGrey), static_cast<uint8_t>(leftGrey),
                                static_cast<uint8_t>(leftGrey), 255);
  Paint right;
  right.color = Color::MakeRGBA8(static_cast<uint8_t>(rightGrey), static_cast<uint8_t>(rightGrey),
                                 static_cast<uint8_t>(rightGrey), 255);

  const auto half = static_cast<graphics::Scalar>(size) / 2.0f;
  canvas->drawRect(Rect::MakeLTRB(0, 0, half, static_cast<graphics::Scalar>(size)), left);
  canvas->drawRect(Rect::MakeLTRB(half, 0, static_cast<graphics::Scalar>(size), static_cast<graphics::Scalar>(size)),
                   right);
  return surface->detachBitmap();
}

/** A surface, filled through a layer that is then masked. */
class MaskedFill {
  public:
  MaskedFill(const int size, const Bitmap& mask, const MaskMode mode) {
    surface = Surface::makeRaster(size, size);
    if (surface == nullptr) return;
    canvas = surface->getCanvas();

    // The element: an opaque red square, drawn into a layer. `kUnbounded` for the bounds, which are only a clip.
    canvas->saveLayer(Rect::MakeLTRB(-1.0e6f, -1.0e6f, 1.0e6f, 1.0e6f), Paint {});
    Paint red;
    red.color = Color::MakeRGBA8(255, 0, 0, 255);
    canvas->drawRect(Rect::MakeLTRB(0, 0, static_cast<graphics::Scalar>(size), static_cast<graphics::Scalar>(size)),
                     red);
    canvas->maskLayer(mask, mode);
    canvas->restore();
  }

  [[nodiscard]] uint32_t at(const int x, const int y) const { return surface->peekPixels().getPixelARGB32(x, y); }
  [[nodiscard]] int alpha(const int x, const int y) const { return static_cast<int>((at(x, y) >> 24) & 0xFFu); }

  std::unique_ptr<Surface> surface;
  std::shared_ptr<Canvas> canvas;
};

} // namespace anonymous

// ==================================================================================================
// The alpha mode
// ==================================================================================================

TEST(CanvasMask, AlphaKeepsTheLayerWhereTheMaskIsOpaque) {
  const Bitmap mask = uniformMask(32, 255, 255);
  const MaskedFill masked(32, mask, MaskMode::kAlpha);

  EXPECT_EQ(masked.alpha(16, 16), 255) << "an opaque mask keeps the layer";
}

TEST(CanvasMask, AlphaErasesTheLayerWhereTheMaskIsTransparent) {
  const Bitmap mask = uniformMask(32, 255, 0);
  const MaskedFill masked(32, mask, MaskMode::kAlpha);

  EXPECT_EQ(masked.alpha(16, 16), 0) << "a transparent mask removes it";
}

TEST(CanvasMask, AlphaScalesByTheMasksAlphaAndIgnoresItsColour) {
  // The distinguishing property of the alpha mode: a *black* opaque mask keeps the layer, because black is opaque.
  const Bitmap mask = uniformMask(32, 0, 255);
  const MaskedFill masked(32, mask, MaskMode::kAlpha);

  EXPECT_EQ(masked.alpha(16, 16), 255) << "black is opaque, so the alpha mode keeps the layer";
}

TEST(CanvasMask, AlphaIsProportional) {
  const Bitmap mask = uniformMask(32, 255, 128);
  const MaskedFill masked(32, mask, MaskMode::kAlpha);

  EXPECT_NEAR(masked.alpha(16, 16), 128, 2);
  EXPECT_EQ((masked.at(16, 16) >> 16) & 0xFFu, 255u) << "un-premultiplied, so the colour is still full red";
}

// ==================================================================================================
// The luminance mode
// ==================================================================================================

TEST(CanvasMask, LuminanceKeepsTheLayerWhereTheMaskIsWhite) {
  const Bitmap mask = uniformMask(32, 255, 255);
  const MaskedFill masked(32, mask, MaskMode::kLuminance);

  EXPECT_EQ(masked.alpha(16, 16), 255) << "white and opaque is full coverage";
}

TEST(CanvasMask, LuminanceErasesTheLayerWhereTheMaskIsBlack) {
  // And this is where the two modes part company: the mask is fully opaque, so the alpha mode would keep every pixel.
  const Bitmap mask = uniformMask(32, 0, 255);
  const MaskedFill masked(32, mask, MaskMode::kLuminance);

  EXPECT_EQ(masked.alpha(16, 16), 0) << "an opaque black mask hides the layer under a luminance rule";
}

TEST(CanvasMask, LuminanceErasesTheLayerWhereTheMaskIsTransparentEvenIfWhite) {
  // Luminance alone is not the rule: a white pixel with no alpha carries no coverage. Treating it as white would make a
  // mask that is mostly transparent keep the whole layer, which is the mirror of the black-mask mistake.
  const Bitmap mask = uniformMask(32, 255, 0);
  const MaskedFill masked(32, mask, MaskMode::kLuminance);

  EXPECT_EQ(masked.alpha(16, 16), 0);
}

TEST(CanvasMask, LuminanceFollowsTheRec709Coefficients) {
  // Green is worth about 0.7154 and blue about 0.0721, so an opaque pure-green mask keeps far more of the layer than an
  // opaque pure-blue one. A rule that averaged the channels would give the same answer for both.
  auto greenSurface = Surface::makeRaster(16, 16);
  auto blueSurface = Surface::makeRaster(16, 16);
  ASSERT_NE(greenSurface, nullptr);
  ASSERT_NE(blueSurface, nullptr);

  Paint green;
  green.color = Color::MakeRGBA8(0, 255, 0, 255);
  greenSurface->getCanvas()->drawRect(Rect::MakeWH(16, 16), green);

  Paint blue;
  blue.color = Color::MakeRGBA8(0, 0, 255, 255);
  blueSurface->getCanvas()->drawRect(Rect::MakeWH(16, 16), blue);

  const MaskedFill withGreen(16, greenSurface->detachBitmap(), MaskMode::kLuminance);
  const MaskedFill withBlue(16, blueSurface->detachBitmap(), MaskMode::kLuminance);

  EXPECT_NEAR(withGreen.alpha(8, 8), 182, 3) << "0.7154 of 255";
  EXPECT_NEAR(withBlue.alpha(8, 8), 18, 3) << "0.0721 of 255";
  EXPECT_GT(withGreen.alpha(8, 8), withBlue.alpha(8, 8) * 5);
}

TEST(CanvasMask, LuminanceVariesAcrossTheMask) {
  // A black-to-white mask at full alpha, which is what a fade looks like and what the alpha mode cannot express.
  const Bitmap mask = splitMask(32, 0, 255);
  const MaskedFill masked(32, mask, MaskMode::kLuminance);

  EXPECT_EQ(masked.alpha(4, 16), 0) << "the black half hides the layer";
  EXPECT_EQ(masked.alpha(28, 16), 255) << "the white half keeps it";
}

// ==================================================================================================
// Cost, and the cases that must do nothing
// ==================================================================================================

TEST(CanvasMask, IsExactWhereTheMaskIsOpaqueWhite) {
  // The commonest mask by far -- white shapes on nothing -- must leave the layer *bit for bit* as it was, or every
  // masked document in the corpus acquires a tint from a rounding error.
  const int size = 24;
  const Bitmap mask = uniformMask(size, 255, 255);

  auto plain = Surface::makeRaster(size, size);
  ASSERT_NE(plain, nullptr);
  Paint red;
  red.color = Color::MakeRGBA8(255, 0, 0, 255);
  plain->getCanvas()->drawRect(Rect::MakeWH(size, size), red);

  const MaskedFill masked(size, mask, MaskMode::kLuminance);

  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      EXPECT_EQ(masked.at(x, y), plain->peekPixels().getPixelARGB32(x, y)) << "at (" << x << "," << y << ")";
    }
  }
}

TEST(CanvasMask, DoesNothingWhenNoLayerIsOpen) {
  // Masking a canvas rather than a layer would erase everything already drawn on it, which is not what "mask this
  // element" means. The call is a no-op, so a caller who gets it wrong loses the mask rather than the picture.
  auto surface = Surface::makeRaster(16, 16);
  ASSERT_NE(surface, nullptr);
  const auto canvas = surface->getCanvas();

  Paint red;
  red.color = Color::MakeRGBA8(255, 0, 0, 255);
  canvas->drawRect(Rect::MakeWH(16, 16), red);

  const Bitmap mask = uniformMask(16, 0, 255);
  canvas->maskLayer(mask, MaskMode::kLuminance);

  EXPECT_EQ((surface->peekPixels().getPixelARGB32(8, 8) >> 24) & 0xFFu, 255u) << "the canvas is untouched";
  EXPECT_EQ((surface->peekPixels().getPixelARGB32(8, 8) >> 16) & 0xFFu, 255u);
}

TEST(CanvasMask, StretchesAMaskOfADifferentSize) {
  // A caller that renders its mask at another resolution gets a defined answer rather than a mask applied to a corner.
  auto surface = Surface::makeRaster(32, 32);
  ASSERT_NE(surface, nullptr);
  const auto canvas = surface->getCanvas();

  const Bitmap smallMask = splitMask(8, 0, 255);

  canvas->saveLayer(Rect::MakeLTRB(-1.0e6f, -1.0e6f, 1.0e6f, 1.0e6f), Paint {});
  Paint red;
  red.color = Color::MakeRGBA8(255, 0, 0, 255);
  canvas->drawRect(Rect::MakeWH(32, 32), red);
  canvas->maskLayer(smallMask, MaskMode::kLuminance);
  canvas->restore();

  const auto pixels = surface->peekPixels();
  EXPECT_EQ((pixels.getPixelARGB32(4, 16) >> 24) & 0xFFu, 0u) << "the dark half, stretched to the whole layer";
  EXPECT_EQ((pixels.getPixelARGB32(28, 16) >> 24) & 0xFFu, 255u) << "and the light half";
}

TEST(CanvasMask, CombinesWithTheLayersOwnAlpha) {
  // A masked half-transparent layer is the product of the two, not whichever was applied last.
  auto surface = Surface::makeRaster(16, 16);
  ASSERT_NE(surface, nullptr);
  const auto canvas = surface->getCanvas();

  Paint half;
  half.color = Color::MakeRGBA8(255, 255, 255, 128);

  canvas->saveLayer(Rect::MakeLTRB(-1.0e6f, -1.0e6f, 1.0e6f, 1.0e6f), half);
  Paint red;
  red.color = Color::MakeRGBA8(255, 0, 0, 255);
  canvas->drawRect(Rect::MakeWH(16, 16), red);
  canvas->maskLayer(uniformMask(16, 255, 128), MaskMode::kAlpha);
  canvas->restore();

  // A half-alpha layer masked at half again: about a quarter.
  EXPECT_NEAR((surface->peekPixels().getPixelARGB32(8, 8) >> 24) & 0xFFu, 64, 4);
}

TEST(CanvasMask, MaskingALayerThatWasNeverDrawnLeavesNothing) {
  auto surface = Surface::makeRaster(16, 16);
  ASSERT_NE(surface, nullptr);
  const auto canvas = surface->getCanvas();

  canvas->saveLayer(Rect::MakeLTRB(-1.0e6f, -1.0e6f, 1.0e6f, 1.0e6f), Paint {});
  canvas->maskLayer(uniformMask(16, 255, 255), MaskMode::kLuminance);
  canvas->restore();

  EXPECT_EQ(surface->peekPixels().getPixelARGB32(8, 8), 0u) << "nothing was drawn, so there is nothing to composite";
}
