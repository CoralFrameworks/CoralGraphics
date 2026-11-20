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
 * @file test_ShaderImage.cpp
 *
 * The image/tile shader.
 *
 * A tile is sampled at a scale other than one -- that is what a pattern is for -- so the assertions are about what
 * happens *between* pixels as much as at them. A nearest-neighbour implementation passes every test that samples at a
 * pixel centre and fails the moment one lands between two, which is why the tests below deliberately sample at halves.
 *
 * The tile modes are the other half. They are applied to each of the four interpolation taps rather than to the sample
 * point, and a renderer that did the latter is right at every pixel centre and wrong in between -- a one-pixel seam at
 * every tile boundary, which is the kind of thing that looks like an off-by-one in the geometry.
 */

#include <graphics/Shader.h>
#include <graphics/Surface.h>

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <vector>

namespace {

using graphics::Bitmap;
using graphics::Color;
using graphics::Matrix;
using graphics::Paint;
using graphics::Point;
using graphics::Shader;
using graphics::Surface;
using graphics::TileMode;
using graphics::Vector3;

int to8(const graphics::Scalar value) { return static_cast<int>(std::lround(value * 255.0f)); }

/** A black-to-white ramp across four pixels, so a sample reads out where in the image it landed. */
Bitmap rampImage() {
  auto surface = Surface::makeRaster(4, 1);
  if (surface == nullptr) return Bitmap {};
  const auto canvas = surface->getCanvas();

  for (int x = 0; x < 4; ++x) {
    const auto level = static_cast<uint8_t>(x * 85); // 0, 85, 170, 255
    Paint paint;
    paint.color = Color::MakeRGBA8(level, level, level, 255);
    canvas->drawRect(graphics::Rect::MakeLTRB(static_cast<graphics::Scalar>(x), 0,
                                              static_cast<graphics::Scalar>(x + 1), 1),
                     paint);
  }
  return surface->detachBitmap();
}

/** A 2x2 image with a different colour in each quadrant, for orientation. */
Bitmap quadrantImage() {
  auto surface = Surface::makeRaster(2, 2);
  if (surface == nullptr) return Bitmap {};
  const auto canvas = surface->getCanvas();

  const auto block = [&canvas](const int x, const int y, const uint8_t r, const uint8_t g, const uint8_t b) {
    Paint paint;
    paint.color = Color::MakeRGBA8(r, g, b, 255);
    canvas->drawRect(graphics::Rect::MakeLTRB(static_cast<graphics::Scalar>(x), static_cast<graphics::Scalar>(y),
                                              static_cast<graphics::Scalar>(x + 1),
                                              static_cast<graphics::Scalar>(y + 1)),
                     paint);
  };
  block(0, 0, 255, 0, 0);   // red, top-left
  block(1, 0, 0, 255, 0);   // green, top-right
  block(0, 1, 0, 0, 255);   // blue, bottom-left
  block(1, 1, 255, 255, 255); // white, bottom-right
  return surface->detachBitmap();
}

} // namespace anonymous

// ==================================================================================================
// Sampling the picture
// ==================================================================================================

TEST(ShaderImage, SamplesAtPixelCentres) {
  // The identity matrix, so image coordinates are drawing coordinates. Pixel centres are at x + 0.5.
  const auto shader = Shader::ImageShader(rampImage(), TileMode::kClamp);
  ASSERT_TRUE(shader->isValid());
  ASSERT_EQ(shader->kind(), Shader::Kind::kImage);

  EXPECT_EQ(to8(shader->sampleAt(Point {0.5f, 0.5f}).red), 0);
  EXPECT_EQ(to8(shader->sampleAt(Point {1.5f, 0.5f}).red), 85);
  EXPECT_EQ(to8(shader->sampleAt(Point {2.5f, 0.5f}).red), 170);
  EXPECT_EQ(to8(shader->sampleAt(Point {3.5f, 0.5f}).red), 255);
}

TEST(ShaderImage, InterpolatesBetweenPixels) {
  // Halfway between the first and second pixels is halfway between their colours. This is the assertion that tells a
  // bilinear sampler from a nearest-neighbour one, and it is the reason the sampler is not the canvas's `drawBitmap`.
  const auto shader = Shader::ImageShader(rampImage(), TileMode::kClamp);
  ASSERT_TRUE(shader->isValid());

  EXPECT_NEAR(to8(shader->sampleAt(Point {1.0f, 0.5f}).red), 42, 3) << "between 0 and 85";
  EXPECT_NEAR(to8(shader->sampleAt(Point {2.0f, 0.5f}).red), 127, 3) << "between 85 and 170";
}

TEST(ShaderImage, SamplesTheRightWayUp) {
  // Image coordinates have their origin at the top-left, as the pixels do. A shader that flipped the y axis draws every
  // picture upside down, which for a photograph is obvious and for a pattern tile is not.
  const auto shader = Shader::ImageShader(quadrantImage(), TileMode::kClamp);
  ASSERT_TRUE(shader->isValid());

  const Color topLeft = shader->sampleAt(Point {0.5f, 0.5f});
  EXPECT_EQ(to8(topLeft.red), 255) << "red at the top-left";
  EXPECT_EQ(to8(topLeft.green), 0);

  const Color bottomLeft = shader->sampleAt(Point {0.5f, 1.5f});
  EXPECT_EQ(to8(bottomLeft.blue), 255) << "blue at the bottom-left";
  EXPECT_EQ(to8(bottomLeft.red), 0);
}

// ==================================================================================================
// The local matrix
// ==================================================================================================

TEST(ShaderImage, AppliesTheInverseOfTheLocalMatrix) {
  // A matrix that scales the image up by two means a drawing coordinate of 4 is image coordinate 2 -- so the sample lands
  // on the third pixel rather than off the end.
  const auto shader = Shader::ImageShader(rampImage(), TileMode::kClamp, Matrix::MakeScale(Vector3 {2, 1, 1}));
  ASSERT_TRUE(shader->isValid());

  EXPECT_EQ(to8(shader->sampleAt(Point {1.0f, 0.5f}).red), 0) << "drawing 1 is image 0.5";
  EXPECT_EQ(to8(shader->sampleAt(Point {5.0f, 0.5f}).red), 170) << "drawing 5 is image 2.5";
}

TEST(ShaderImage, AppliesATranslationInTheLocalMatrix) {
  const auto shader = Shader::ImageShader(rampImage(), TileMode::kClamp, Matrix::MakeTranslation(Vector3 {4, 0, 0}));
  ASSERT_TRUE(shader->isValid());

  // The image's origin is at drawing 4, so drawing 4.5 is its first pixel and drawing 3.5 is outside it to the left.
  EXPECT_EQ(to8(shader->sampleAt(Point {4.5f, 0.5f}).red), 0);
  EXPECT_EQ(to8(shader->sampleAt(Point {7.5f, 0.5f}).red), 255);
}

// ==================================================================================================
// The tile modes
// ==================================================================================================

TEST(ShaderImage, ClampHoldsTheEdgePixels) {
  const auto shader = Shader::ImageShader(rampImage(), TileMode::kClamp);
  EXPECT_EQ(to8(shader->sampleAt(Point {-4.0f, 0.5f}).red), 0) << "left of the image, the first pixel";
  EXPECT_EQ(to8(shader->sampleAt(Point {9.0f, 0.5f}).red), 255) << "and right of it, the last";
}

TEST(ShaderImage, RepeatWraps) {
  const auto shader = Shader::ImageShader(rampImage(), TileMode::kRepeat);
  ASSERT_TRUE(shader->isValid());

  // Four pixels wide, so drawing 4.5 is the same texel as 0.5.
  EXPECT_EQ(to8(shader->sampleAt(Point {4.5f, 0.5f}).red), 0);
  EXPECT_EQ(to8(shader->sampleAt(Point {6.5f, 0.5f}).red), 170);
  EXPECT_EQ(to8(shader->sampleAt(Point {-1.5f, 0.5f}).red), 170) << "and it wraps backwards too";
}

TEST(ShaderImage, RepeatHasNoSeamBetweenTheTaps) {
  // The property that makes tiling look continuous. Between the last pixel and the first of the next tile -- drawing 3.5
  // to 4.5 -- the two taps come from the last pixel and the *first* pixel, not from the last pixel and a clamped edge.
  // Interpolating towards the edge instead would put a bright band at every boundary of a dark ramp, which is a seam no
  // geometry explains.
  const auto shader = Shader::ImageShader(rampImage(), TileMode::kRepeat);
  ASSERT_TRUE(shader->isValid());

  // At exactly the boundary the weight is half each: (255 + 0) / 2.
  EXPECT_NEAR(to8(shader->sampleAt(Point {4.0f, 0.5f}).red), 127, 4) << "the wrap, not a clamped edge";
}

TEST(ShaderImage, MirrorReflects) {
  const auto shader = Shader::ImageShader(rampImage(), TileMode::kMirror);
  ASSERT_TRUE(shader->isValid());

  // A mirrored period runs 0,1,2,3 then 3,2,1,0. So drawing 4.5 is the last pixel and 7.5 is the first.
  EXPECT_EQ(to8(shader->sampleAt(Point {4.5f, 0.5f}).red), 255);
  EXPECT_EQ(to8(shader->sampleAt(Point {7.5f, 0.5f}).red), 0);
}

TEST(ShaderImage, DecalDrawsNothingOutsideTheImage) {
  const auto shader = Shader::ImageShader(rampImage(), TileMode::kDecal);
  ASSERT_TRUE(shader->isValid());

  EXPECT_EQ(to8(shader->sampleAt(Point {-1.0f, 0.5f}).alpha), 0);
  EXPECT_EQ(to8(shader->sampleAt(Point {9.0f, 0.5f}).alpha), 0);
  EXPECT_EQ(to8(shader->sampleAt(Point {1.5f, 0.5f}).alpha), 255) << "and opaque within it";
}

// ==================================================================================================
// The degenerate cases
// ==================================================================================================

TEST(ShaderImage, AnEmptyTileIsInvalid) {
  // A caller that assembled one from a document that failed to load gets the paint's flat colour rather than a shape that
  // vanished.
  const auto shader = Shader::ImageShader(Bitmap {}, TileMode::kRepeat);
  EXPECT_FALSE(shader->isValid());
  EXPECT_EQ(to8(shader->sampleAt(Point {0, 0}).alpha), 0) << "and samples nothing";
}

TEST(ShaderImage, AnUninvertibleMatrixIsInvalid) {
  // With no inverse there is no way to ask where a drawing point came from, so there is no colour to give.
  const auto shader = Shader::ImageShader(rampImage(), TileMode::kRepeat, Matrix::MakeScale(Vector3 {0, 1, 1}));
  EXPECT_FALSE(shader->isValid());
}

TEST(ShaderImage, APaintHoldingAnInvalidImageShaderFallsBackToItsColour) {
  Paint paint;
  paint.color = Color::MakeRGBA8(255, 0, 0, 255);
  EXPECT_FALSE(paint.hasShader());

  paint.shader = Shader::ImageShader(rampImage(), TileMode::kRepeat);
  EXPECT_TRUE(paint.hasShader());
}

// ==================================================================================================
// Drawn
// ==================================================================================================

TEST(ShaderImageRender, FillsAShapeWithTheTiledPicture) {
  // The end-to-end case: a rectangle painted with a tile shader, which is what a pattern becomes. The default local
  // matrix is the identity, so the image occupies the top-left corner of the shape at its own size and repeats.
  auto surface = Surface::makeRaster(64, 16);
  ASSERT_NE(surface, nullptr);

  Paint paint;
  paint.color = Color::MakeRGBA8(255, 255, 255, 255);
  paint.shader = Shader::ImageShader(rampImage(), TileMode::kRepeat);

  graphics::Path path;
  path.moveTo(Point {0, 0});
  path.lineTo(Point {64, 0});
  path.lineTo(Point {64, 16});
  path.lineTo(Point {0, 16});
  path.close();
  surface->getCanvas()->drawPath(path, paint);

  const auto pixels = surface->peekPixels();
  const auto red = [&pixels](const int x, const int y) {
    return static_cast<int>((pixels.getPixelARGB32(x, y) >> 16) & 0xFFu);
  };

  EXPECT_LT(red(0, 8), 24) << "the dark end of the ramp";
  EXPECT_GT(red(3, 8), 231) << "the light end of it";
  EXPECT_LT(red(4, 8), 24) << "and the tile starts again";
  EXPECT_GT(red(7, 8), 231) << "which is what a repeat looks like";
}
