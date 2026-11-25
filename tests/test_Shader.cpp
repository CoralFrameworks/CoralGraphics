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
 * @file test_Shader.cpp
 *
 * Gradients, sampled directly and drawn.
 *
 * The properties worth pinning are the ones that are wrong in a way nobody notices: a tile mode that is off by one
 * period, a mirror that does not reflect, a gradient that samples at pixel corners rather than centres, a local matrix
 * applied in the wrong direction. Each produces a gradient that looks like a gradient -- plausible, smooth, and not what
 * the document asked for.
 */

#include <graphics/Shader.h>
#include <graphics/Surface.h>
#include <graphics/Canvas.h>
#include <graphics/Paint.h>
#include <graphics/Path.h>

#include <gtest/gtest.h>

#include <cmath>

namespace {

using graphics::Canvas;
using graphics::Color;
using graphics::GradientStop;
using graphics::Matrix;
using graphics::Paint;
using graphics::Path;
using graphics::Point;
using graphics::Shader;
using graphics::Surface;
using graphics::TileMode;
using graphics::Vector3;

/** Black at zero and white at one, which makes the sampled channel a direct read of `t`. */
std::vector<GradientStop> blackToWhite() {
  return {GradientStop {0.0f, Color::MakeRGBA8(0, 0, 0, 255)}, GradientStop {1.0f, Color::MakeRGBA8(255, 255, 255, 255)}};
}

int red8(const Color& color) { return static_cast<int>(std::lround(color.red * 255.0f)); }
int alpha8(const Color& color) { return static_cast<int>(std::lround(color.alpha * 255.0f)); }

/** A surface with a rectangle filled through @p shader. */
struct Drawn {
  std::unique_ptr<Surface> surface;

  uint32_t at(const int x, const int y) const { return surface->peekPixels().getPixelARGB32(x, y); }
  int red(const int x, const int y) const { return static_cast<int>((at(x, y) >> 16) & 0xFFu); }
  int alpha(const int x, const int y) const { return static_cast<int>((at(x, y) >> 24) & 0xFFu); }
};

/** Fills the whole surface with a shader, through the canvas, which is the path a document takes. */
Drawn fillWith(const std::shared_ptr<Shader>& shader, const int size = 64) {
  Drawn drawn;
  drawn.surface = Surface::makeRaster(size, size);
  if (drawn.surface == nullptr) return drawn;

  Paint paint;
  paint.color = Color::MakeRGBA8(0, 0, 0, 255);
  paint.shader = shader;

  Path path;
  path.moveTo(Point {0, 0});
  path.lineTo(Point {static_cast<graphics::Scalar>(size), 0});
  path.lineTo(Point {static_cast<graphics::Scalar>(size), static_cast<graphics::Scalar>(size)});
  path.lineTo(Point {0, static_cast<graphics::Scalar>(size)});
  path.close();

  drawn.surface->getCanvas()->drawPath(path, paint);
  return drawn;
}

} // namespace anonymous

// ==================================================================================================
// Sampling a linear gradient
// ==================================================================================================

TEST(ShaderTest, SamplesTheStopsAtTheEndsAndBetweenThem) {
  const auto shader = Shader::LinearGradient(Point {0, 0}, Point {10, 0}, blackToWhite());
  ASSERT_TRUE(shader->isValid());

  EXPECT_EQ(red8(shader->sampleAt(Point {0, 0})), 0) << "at the start";
  EXPECT_EQ(red8(shader->sampleAt(Point {10, 0})), 255) << "at the end";
  EXPECT_NEAR(red8(shader->sampleAt(Point {5, 0})), 128, 1) << "halfway";
  // The gradient varies along its direction only, so every point on a perpendicular through the same x samples the
  // same colour. Getting this wrong -- sampling by distance rather than by projection -- makes a linear gradient
  // radial, which looks almost right on a square.
  EXPECT_EQ(red8(shader->sampleAt(Point {5, 40})), red8(shader->sampleAt(Point {5, -40})));
}

TEST(ShaderTest, ProjectsOntoTheGradientDirection) {
  // A diagonal gradient: the colour at a point is decided by where it falls along the line, not by its x or its y.
  const auto shader = Shader::LinearGradient(Point {0, 0}, Point {10, 10}, blackToWhite());
  ASSERT_TRUE(shader->isValid());
  // (5,5) is halfway along the diagonal, and (10,0) projects to the same place -- both are at t = 0.5.
  EXPECT_NEAR(red8(shader->sampleAt(Point {5, 5})), 128, 1);
  EXPECT_NEAR(red8(shader->sampleAt(Point {10, 0})), 128, 1) << "the projection, not the distance";
}

TEST(ShaderTest, InterpolatesBetweenMoreThanTwoStops) {
  const std::vector<GradientStop> stops {
    {0.0f, Color::MakeRGBA8(255, 0, 0, 255)},
    {0.5f, Color::MakeRGBA8(0, 255, 0, 255)},
    {1.0f, Color::MakeRGBA8(0, 0, 255, 255)},
  };
  const auto shader = Shader::LinearGradient(Point {0, 0}, Point {100, 0}, stops);
  ASSERT_TRUE(shader->isValid());

  EXPECT_EQ(red8(shader->sampleAt(Point {0, 0})), 255) << "the first stop";
  EXPECT_EQ(red8(shader->sampleAt(Point {50, 0})), 0);
  EXPECT_EQ(static_cast<int>(std::lround(shader->sampleAt(Point {50, 0}).green * 255.0f)), 255) << "the middle stop";
  EXPECT_EQ(static_cast<int>(std::lround(shader->sampleAt(Point {100, 0}).blue * 255.0f)), 255) << "the last";
  // A quarter of the way is halfway between the first two.
  EXPECT_NEAR(red8(shader->sampleAt(Point {25, 0})), 128, 1);
}

TEST(ShaderTest, SortsStopsWrittenOutOfOrder) {
  // CSS does not require a stop list to be written in order, and a renderer that assumes it does produces a gradient
  // with a discontinuity where the order changes.
  const std::vector<GradientStop> stops {
    {1.0f, Color::MakeRGBA8(255, 255, 255, 255)},
    {0.0f, Color::MakeRGBA8(0, 0, 0, 255)},
  };
  const auto shader = Shader::LinearGradient(Point {0, 0}, Point {10, 0}, stops);
  ASSERT_TRUE(shader->isValid());
  EXPECT_EQ(red8(shader->sampleAt(Point {0, 0})), 0) << "the stop written second but positioned first";
  EXPECT_EQ(red8(shader->sampleAt(Point {10, 0})), 255);
}

TEST(ShaderTest, TreatsCoincidentStopsAsAHardEdge) {
  const std::vector<GradientStop> stops {
    {0.0f, Color::MakeRGBA8(0, 0, 0, 255)},
    {0.5f, Color::MakeRGBA8(0, 0, 0, 255)},
    {0.5f, Color::MakeRGBA8(255, 255, 255, 255)},
    {1.0f, Color::MakeRGBA8(255, 255, 255, 255)},
  };
  const auto shader = Shader::LinearGradient(Point {0, 0}, Point {10, 0}, stops);
  ASSERT_TRUE(shader->isValid());
  EXPECT_EQ(red8(shader->sampleAt(Point {2, 0})), 0);
  EXPECT_EQ(red8(shader->sampleAt(Point {8, 0})), 255);
}

// ==================================================================================================
// The tile modes
// ==================================================================================================

TEST(ShaderTest, ClampHoldsTheEndColours) {
  const auto shader = Shader::LinearGradient(Point {0, 0}, Point {10, 0}, blackToWhite(), TileMode::kClamp);
  ASSERT_TRUE(shader->isValid());
  EXPECT_EQ(red8(shader->sampleAt(Point {-50, 0})), 0) << "before the start";
  EXPECT_EQ(red8(shader->sampleAt(Point {50, 0})), 255) << "past the end";
}

TEST(ShaderTest, RepeatTilesTheGradient) {
  const auto shader = Shader::LinearGradient(Point {0, 0}, Point {10, 0}, blackToWhite(), TileMode::kRepeat);
  ASSERT_TRUE(shader->isValid());
  EXPECT_NEAR(red8(shader->sampleAt(Point {15, 0})), 128, 1) << "one and a half periods";
  EXPECT_NEAR(red8(shader->sampleAt(Point {25, 0})), 128, 1) << "two and a half";
  // A negative position is the case `fmod` gets wrong: it would leave t negative and clamp instead of wrapping.
  EXPECT_NEAR(red8(shader->sampleAt(Point {-5, 0})), 128, 1) << "half a period before the start";
  EXPECT_EQ(red8(shader->sampleAt(Point {20, 0})), 0) << "two whole periods lands on the first stop again";
}

TEST(ShaderTest, MirrorReflectsEveryOtherPeriod) {
  const auto shader = Shader::LinearGradient(Point {0, 0}, Point {10, 0}, blackToWhite(), TileMode::kMirror);
  ASSERT_TRUE(shader->isValid());
  // At t = 1.5 the mirror gives 0.5, and repeat would also give 0.5 -- so that is not the discriminating case. At
  // t = 0.5 repeat gives 0.5 and mirror gives 0.5 too. The difference shows at t = 1.25: repeat gives 0.25, mirror 0.75.
  EXPECT_NEAR(red8(shader->sampleAt(Point {12.5f, 0})), 191, 2) << "reflected, so three quarters";
  EXPECT_NEAR(red8(shader->sampleAt(Point {7.5f, 0})), 191, 2) << "and the first period is not mirrored";
}

TEST(ShaderTest, DecalDrawsNothingOutsideTheRange) {
  const auto shader = Shader::LinearGradient(Point {0, 0}, Point {10, 0}, blackToWhite(), TileMode::kDecal);
  ASSERT_TRUE(shader->isValid());
  EXPECT_EQ(alpha8(shader->sampleAt(Point {-1, 0})), 0) << "before the start";
  EXPECT_EQ(alpha8(shader->sampleAt(Point {11, 0})), 0) << "past the end";
  EXPECT_EQ(alpha8(shader->sampleAt(Point {5, 0})), 255) << "and opaque within it";
}

// ==================================================================================================
// Radial
// ==================================================================================================

TEST(ShaderTest, SamplesARadialGradientByDistanceFromItsCentre) {
  const auto shader = Shader::RadialGradient(Point {30, 30}, 20.0f, blackToWhite());
  ASSERT_TRUE(shader->isValid());

  EXPECT_EQ(red8(shader->sampleAt(Point {30, 30})), 0) << "at the centre";
  EXPECT_EQ(red8(shader->sampleAt(Point {50, 30})), 255) << "at the radius";
  EXPECT_NEAR(red8(shader->sampleAt(Point {40, 30})), 128, 1) << "halfway out";
  // Distance, so every direction is the same -- which is what makes it radial. A gradient that used x and y
  // separately would give a diamond.
  EXPECT_NEAR(red8(shader->sampleAt(Point {40, 30})), red8(shader->sampleAt(Point {30, 40})), 1);
  EXPECT_NEAR(red8(shader->sampleAt(Point {44.14f, 44.14f})), red8(shader->sampleAt(Point {50, 30})), 2)
          << "the diagonal at the same distance";
}

TEST(ShaderTest, RadialClampsBeyondItsRadiusByDefault) {
  const auto shader = Shader::RadialGradient(Point {0, 0}, 10.0f, blackToWhite());
  ASSERT_TRUE(shader->isValid());
  EXPECT_EQ(red8(shader->sampleAt(Point {100, 0})), 255);
}

// ==================================================================================================
// The local matrix
// ==================================================================================================

TEST(ShaderTest, AppliesTheInverseOfTheLocalMatrix) {
  // The matrix maps shader space to drawing space. A gradient scaled by two therefore varies half as fast across the
  // drawing: a point at drawing x = 20 is shader x = 10, which is the far stop.
  const auto shader = Shader::LinearGradient(Point {0, 0}, Point {10, 0}, blackToWhite(), TileMode::kClamp,
                                             Matrix::MakeScale(Vector3 {2, 2, 1}));
  ASSERT_TRUE(shader->isValid());
  EXPECT_EQ(red8(shader->sampleAt(Point {0, 0})), 0);
  EXPECT_EQ(red8(shader->sampleAt(Point {20, 0})), 255) << "drawing space 20 is shader space 10";
  EXPECT_NEAR(red8(shader->sampleAt(Point {10, 0})), 128, 1) << "and drawing 10 is shader 5";
}

TEST(ShaderTest, AppliesATranslationInTheLocalMatrix) {
  const auto shader = Shader::LinearGradient(Point {0, 0}, Point {10, 0}, blackToWhite(), TileMode::kClamp,
                                             Matrix::MakeTranslation(Vector3 {5, 0, 0}));
  ASSERT_TRUE(shader->isValid());
  EXPECT_EQ(red8(shader->sampleAt(Point {5, 0})), 0) << "the shader's origin sits at drawing 5";
  EXPECT_EQ(red8(shader->sampleAt(Point {15, 0})), 255);
}

TEST(ShaderTest, ReportsAnUninvertibleLocalMatrixAsInvalid) {
  // A matrix with no inverse -- here a zero scale -- cannot map drawing space back, so there is no colour to give.
  const auto shader = Shader::LinearGradient(Point {0, 0}, Point {10, 0}, blackToWhite(), TileMode::kClamp,
                                             Matrix::MakeScale(Vector3 {0, 1, 1}));
  EXPECT_FALSE(shader->isValid());
}

// ==================================================================================================
// The degenerate cases
// ==================================================================================================

TEST(ShaderTest, ReportsADegenerateLinearGradientAsInvalid) {
  // Endpoints that coincide describe no direction. Evaluating would divide by zero, so this is refused rather than
  // special-cased into a flat colour -- the caller asked for a gradient and did not get one.
  EXPECT_FALSE(Shader::LinearGradient(Point {5, 5}, Point {5, 5}, blackToWhite())->isValid());
}

TEST(ShaderTest, ReportsARadialGradientOfNoRadiusAsInvalid) {
  EXPECT_FALSE(Shader::RadialGradient(Point {0, 0}, 0.0f, blackToWhite())->isValid());
  EXPECT_FALSE(Shader::RadialGradient(Point {0, 0}, -4.0f, blackToWhite())->isValid());
}

TEST(ShaderTest, ReportsFewerThanTwoStopsAsInvalid) {
  // One stop is not a gradient, and zero stops have nothing to interpolate. Both are how a document that failed to
  // parse its stops ends up, and drawing the shape flat is better than drawing it invisible.
  EXPECT_FALSE(Shader::LinearGradient(Point {0, 0}, Point {10, 0}, {})->isValid());
  EXPECT_FALSE(Shader::LinearGradient(Point {0, 0}, Point {10, 0}, {GradientStop {0.5f, Color::Black()}})->isValid());
}

TEST(ShaderTest, APaintHoldingAnInvalidShaderFallsBackToItsColour) {
  Paint paint;
  paint.color = Color::MakeRGBA8(255, 0, 0, 255);
  paint.shader = Shader::LinearGradient(Point {0, 0}, Point {0, 0}, blackToWhite());
  EXPECT_FALSE(paint.hasShader());

  paint.shader = Shader::LinearGradient(Point {0, 0}, Point {10, 0}, blackToWhite());
  EXPECT_TRUE(paint.hasShader());
}

// ==================================================================================================
// Drawing
// ==================================================================================================

TEST(ShaderRender, FillsARectangleWithAGradientAlongIt) {
  // The gradient runs across the whole surface, so the colour at a pixel reads out its position -- which is how a
  // sampling error of half a pixel, or a wrong matrix direction, becomes visible.
  const auto shader = Shader::LinearGradient(Point {0, 0}, Point {64, 0}, blackToWhite());
  const Drawn drawn = fillWith(shader);
  ASSERT_NE(drawn.surface, nullptr);

  EXPECT_LE(drawn.red(1, 32), 12) << "near the dark end";
  EXPECT_GE(drawn.red(62, 32), 243) << "near the light end";
  EXPECT_NEAR(drawn.red(32, 32), 128, 4) << "and roughly halfway in between";
  // Monotone across the surface: a gradient that reverses anywhere is a tile mode or a projection mistake.
  for (int x = 1; x < 63; ++x) {
    EXPECT_LE(drawn.red(x, 32), drawn.red(x + 1, 32)) << "at x = " << x;
  }
  EXPECT_EQ(drawn.alpha(32, 32), 255) << "opaque, because both stops are";
}

TEST(ShaderRender, VariesOnlyAlongTheGradientDirection) {
  const auto shader = Shader::LinearGradient(Point {0, 0}, Point {64, 0}, blackToWhite());
  const Drawn drawn = fillWith(shader);
  ASSERT_NE(drawn.surface, nullptr);
  // A column of pixels at the same x is one colour: the gradient is horizontal, and a renderer that sampled in both
  // axes would show a vertical ramp as well.
  for (int y = 2; y < 62; y += 8) {
    EXPECT_NEAR(drawn.red(32, y), drawn.red(32, 32), 1) << "at y = " << y;
  }
}

TEST(ShaderRender, AGlobalAlphaFadesTheWholeGradient) {
  // The paint's alpha multiplies whatever the shader produced. That is what lets the SVG layer fold `opacity` into the
  // paint without rebuilding the gradient, and it is the only use of `Paint::color` when a shader is present.
  Drawn drawn;
  drawn.surface = Surface::makeRaster(64, 64);
  ASSERT_NE(drawn.surface, nullptr);

  Paint paint;
  paint.color = Color::MakeRGBA8(0, 0, 0, 128); // half alpha
  paint.shader = Shader::LinearGradient(Point {0, 0}, Point {64, 0}, blackToWhite());

  Path path;
  path.moveTo(Point {0, 0});
  path.lineTo(Point {64, 0});
  path.lineTo(Point {64, 64});
  path.lineTo(Point {0, 64});
  path.close();
  drawn.surface->getCanvas()->drawPath(path, paint);

  // Un-premultiplied by the accessor, so the colour still reads as the gradient's and the half is in the alpha.
  EXPECT_NEAR(drawn.alpha(32, 32), 128, 2);
  EXPECT_NEAR(drawn.red(32, 32), 128, 4);
}

TEST(ShaderRender, DrawsARadialGradientAsConcentricRings) {
  const auto shader = Shader::RadialGradient(Point {32, 32}, 30.0f, blackToWhite());
  const Drawn drawn = fillWith(shader);
  ASSERT_NE(drawn.surface, nullptr);

  EXPECT_LE(drawn.red(32, 32), 12) << "dark at the centre";
  // Four points at the same radius are the same colour: that is what makes it radial rather than a linear ramp.
  //
  // The radius has to be measured to pixel *centres*, which are at x + 0.5. The gradient's centre sits at the coordinate
  // 32, so the pixel 51 has its centre at 51.5 and is 19.5 away, while the pixel 52 has its centre at 52.5 and is 20.5
  // away. Comparing 52 against 12 would compare two different radii and call the eight-level difference a shader bug.
  const int right = drawn.red(51, 32);
  EXPECT_NEAR(drawn.red(12, 32), right, 2) << "left, at the same radius";
  EXPECT_NEAR(drawn.red(32, 51), right, 2) << "below";
  EXPECT_NEAR(drawn.red(32, 12), right, 2) << "above";
  // And the ring is genuinely further along than the centre.
  EXPECT_GT(right, drawn.red(32, 32)) << "the gradient brightens outwards";
}

TEST(ShaderRender, FallsBackToThePaintColourWhenTheShaderIsUnusable) {
  Drawn drawn;
  drawn.surface = Surface::makeRaster(16, 16);
  ASSERT_NE(drawn.surface, nullptr);

  Paint paint;
  paint.color = Color::MakeRGBA8(255, 0, 0, 255);
  paint.shader = Shader::LinearGradient(Point {0, 0}, Point {0, 0}, blackToWhite()); // degenerate

  Path path;
  path.moveTo(Point {0, 0});
  path.lineTo(Point {16, 0});
  path.lineTo(Point {16, 16});
  path.lineTo(Point {0, 16});
  path.close();
  drawn.surface->getCanvas()->drawPath(path, paint);

  EXPECT_EQ(drawn.red(8, 8), 255) << "the flat colour, rather than an empty shape";
}
