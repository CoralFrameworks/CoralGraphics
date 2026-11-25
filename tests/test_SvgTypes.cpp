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
 * @file test_SvgTypes.cpp
 *
 * The SVG value parsers.
 *
 * The cases here are the ones where a parser that *looks* right produces a value that renders as a nearly-correct
 * picture: a transform list applied in the wrong order, a smooth curve that reflects a control point it should have
 * ignored, a relative command measured from the wrong origin, a colour component read as one digit when it is two.
 * Each of those is invisible in a small test image and obvious in a large one, which is why they are pinned by value
 * here rather than by comparing rendered output.
 */

#include "svg/SvgTypes.h"

#include <gtest/gtest.h>

#include <cmath>

namespace {

using graphics::Path;
using graphics::Point;
using graphics::svg::Length;
using graphics::svg::LengthUnit;
using graphics::svg::Paint;
using graphics::svg::parseColor;
using graphics::svg::parseLength;
using graphics::svg::parsePaint;
using graphics::svg::parsePathData;
using graphics::svg::parsePoints;
using graphics::svg::parseTransform;

/** The path's control-point count, which is enough to tell one command sequence from another. */
size_t pointCount(const Path& path) { return path.pointCount(); }

/**
 * A colour's channels as the eight-bit values a document writes.
 *
 * `graphics::Color` holds floats in 0..1, so comparing the packed integer is what makes an assertion read like the
 * attribute it came from -- `#ff0000` is red 255, not red 1.0.
 */
struct Rgba8 {
  int r = 0;
  int g = 0;
  int b = 0;
  int a = 0;
};

Rgba8 to8(const graphics::Color& color) {
  const auto channel = [](const graphics::Scalar v) { return static_cast<int>(std::lround(v * 255.0f)); };
  return Rgba8 {channel(color.red), channel(color.green), channel(color.blue), channel(color.alpha)};
}

} // namespace anonymous

// ==================================================================================================
// Lengths
// ==================================================================================================

TEST(SvgLength, ReadsAUnitlessNumber) {
  const auto length = parseLength("42");
  ASSERT_TRUE(length.has_value());
  EXPECT_FLOAT_EQ(length->value, 42.0f);
  EXPECT_EQ(length->unit, LengthUnit::Number);
}

TEST(SvgLength, ReadsEveryUnitTheGrammarHas) {
  const struct {
    const char* text;
    LengthUnit unit;
  } cases[] = {
    {"1px", LengthUnit::Px},   {"2em", LengthUnit::Em},   {"3ex", LengthUnit::Ex}, {"4pt", LengthUnit::Pt},
    {"5pc", LengthUnit::Pc},   {"6cm", LengthUnit::Cm},   {"7mm", LengthUnit::Mm}, {"8in", LengthUnit::In},
    {"50%", LengthUnit::Percent},
  };
  for (const auto& [text, unit] : cases) {
    const auto length = parseLength(text);
    ASSERT_TRUE(length.has_value()) << text;
    EXPECT_EQ(length->unit, unit) << text;
  }
}

TEST(SvgLength, ReadsNegativeAndFractionalValues) {
  const auto length = parseLength("-12.5");
  ASSERT_TRUE(length.has_value());
  EXPECT_FLOAT_EQ(length->value, -12.5f);
}

TEST(SvgLength, RejectsTrailingText) {
  // `10px junk` is not a length. Accepting the prefix would turn a malformed document into a slightly wrong picture
  // instead of into the caller's documented fallback.
  EXPECT_FALSE(parseLength("10px junk").has_value());
  EXPECT_FALSE(parseLength("").has_value());
  EXPECT_FALSE(parseLength("px").has_value());
}

TEST(SvgLength, ResolvesAgainstTheContextItIsGiven) {
  graphics::svg::LengthContext context;
  context.viewportWidth = 200;
  context.viewportHeight = 100;

  EXPECT_FLOAT_EQ(context.resolve(Length {10, LengthUnit::Number}, LengthUnit::Px).value_or(-1), 10.0f);
  // A percentage of the width, then of the height: the axis is what the caller passes, which is how SVG's rule that a
  // `width` is a fraction of the viewport width is expressed without this layer knowing about elements.
  EXPECT_FLOAT_EQ(context.resolve(Length {50, LengthUnit::Percent}, LengthUnit::Px).value_or(-1), 100.0f);
  EXPECT_FLOAT_EQ(context.resolve(Length {50, LengthUnit::Percent}, LengthUnit::Ex).value_or(-1), 50.0f);
  // The absolute units, at the CSS 96-pixels-to-the-inch that SVG inherited.
  EXPECT_FLOAT_EQ(context.resolve(Length {1, LengthUnit::In}, LengthUnit::Px).value_or(-1), 96.0f);
}

// ==================================================================================================
// Colours
// ==================================================================================================

TEST(SvgColor, ReadsNamedColors) {
  const auto red = parseColor("red");
  ASSERT_TRUE(red.has_value());
  EXPECT_EQ(to8(*red).r, 255);
  EXPECT_EQ(to8(*red).g, 0);
  EXPECT_EQ(to8(*red).b, 0);

  // Case-insensitive, as CSS is.
  EXPECT_TRUE(parseColor("ReBeCcApUrPlE").has_value());
  EXPECT_TRUE(parseColor("cornflowerblue").has_value());
}

TEST(SvgColor, ReadsHexInAllFourForms) {
  const auto short3 = parseColor("#f00");
  ASSERT_TRUE(short3.has_value());
  EXPECT_EQ(to8(*short3).r, 255);
  EXPECT_EQ(to8(*short3).g, 0);

  // `#f` is `#ff`: a short form repeats its digit rather than being scaled by sixteen. `#f00` reading as (15, 0, 0)
  // instead of (255, 0, 0) is a dark red that looks deliberate.
  const auto short4 = parseColor("#f00f");
  ASSERT_TRUE(short4.has_value());
  EXPECT_EQ(to8(*short4).r, 255);
  EXPECT_EQ(to8(*short4).a, 255);

  const auto long6 = parseColor("#00ff00");
  ASSERT_TRUE(long6.has_value());
  EXPECT_EQ(to8(*long6).r, 0);
  EXPECT_EQ(to8(*long6).g, 255);

  const auto long8 = parseColor("#0000ff80");
  ASSERT_TRUE(long8.has_value());
  EXPECT_EQ(to8(*long8).b, 255);
  EXPECT_EQ(to8(*long8).a, 128);
}

TEST(SvgColor, ReadsTheRgbFunctions) {
  const auto legacy = parseColor("rgb(255, 128, 0)");
  ASSERT_TRUE(legacy.has_value());
  EXPECT_EQ(to8(*legacy).r, 255);
  EXPECT_EQ(to8(*legacy).g, 128);

  const auto percentage = parseColor("rgb(100%, 50%, 0%)");
  ASSERT_TRUE(percentage.has_value());
  EXPECT_EQ(to8(*percentage).r, 255);
  EXPECT_EQ(to8(*percentage).g, 128);

  const auto withAlpha = parseColor("rgba(0, 0, 255, 0.5)");
  ASSERT_TRUE(withAlpha.has_value());
  EXPECT_EQ(to8(*withAlpha).b, 255);
  EXPECT_EQ(to8(*withAlpha).a, 128);
}

TEST(SvgColor, RejectsWhatItCannotParse) {
  EXPECT_FALSE(parseColor("notacolour").has_value());
  EXPECT_FALSE(parseColor("#12345").has_value()); // five digits is not a form
  EXPECT_FALSE(parseColor("").has_value());
}

// ==================================================================================================
// Paint
// ==================================================================================================

TEST(SvgPaint, DistinguishesNoneFromAColour) {
  const auto none = parsePaint("none");
  ASSERT_TRUE(none.has_value());
  EXPECT_EQ(none->type, Paint::Type::None);

  const auto colour = parsePaint("#ff0000");
  ASSERT_TRUE(colour.has_value());
  EXPECT_EQ(colour->type, Paint::Type::Color);
}

TEST(SvgPaint, ReadsCurrentColor) {
  const auto paint = parsePaint("currentColor");
  ASSERT_TRUE(paint.has_value());
  EXPECT_EQ(paint->type, Paint::Type::CurrentColor);
}

TEST(SvgPaint, ReadsAUrlReferenceWithAndWithoutAFallback) {
  const auto plain = parsePaint("url(#gradient)");
  ASSERT_TRUE(plain.has_value());
  EXPECT_EQ(plain->type, Paint::Type::IRI);
  EXPECT_EQ(plain->iri, "gradient");
  EXPECT_FALSE(plain->fallback.has_value());

  const auto quoted = parsePaint("url('#other')");
  ASSERT_TRUE(quoted.has_value());
  EXPECT_EQ(quoted->iri, "other");

  const auto withFallback = parsePaint("url(#g) red");
  ASSERT_TRUE(withFallback.has_value());
  EXPECT_EQ(withFallback->iri, "g");
  ASSERT_TRUE(withFallback->fallback.has_value());
  EXPECT_EQ(to8(*withFallback->fallback).r, 255);
}

// ==================================================================================================
// Transforms
// ==================================================================================================

TEST(SvgTransform, AppliesAListInTheOrderWritten) {
  // The case that catches a reversed multiplication. `translate(10,0) scale(2)` maps the point (1,0) to (12,0): the
  // scale acts on the point first, and then the translation is not scaled. Composed the other way it would be (22,0).
  const auto transform = parseTransform("translate(10,0) scale(2)");
  ASSERT_TRUE(transform.has_value());
  const Point mapped = *transform * Point {1, 0};
  EXPECT_NEAR(mapped.x, 12.0f, 1e-4f);
  EXPECT_NEAR(mapped.y, 0.0f, 1e-4f);
}

TEST(SvgTransform, ReadsTheMatrixFormWithTheRightInterleaving) {
  // (a b c d e f) is x' = a*x + c*y + e, y' = b*x + d*y + f. A transposed reading swaps c and b, which shears a shape
  // rather than scaling it -- a difference that is invisible on an axis-aligned rectangle and obvious on anything else.
  const auto transform = parseTransform("matrix(1 2 3 4 5 6)");
  ASSERT_TRUE(transform.has_value());
  const Point mapped = *transform * Point {1, 1};
  EXPECT_NEAR(mapped.x, 1 + 3 + 5, 1e-4f);
  EXPECT_NEAR(mapped.y, 2 + 4 + 6, 1e-4f);
}

TEST(SvgTransform, ReadsTranslateWithOneOrTwoArguments) {
  const auto one = parseTransform("translate(5)");
  ASSERT_TRUE(one.has_value());
  const Point mapped = *one * Point {0, 0};
  EXPECT_NEAR(mapped.x, 5.0f, 1e-4f);
  EXPECT_NEAR(mapped.y, 0.0f, 1e-4f);
}

TEST(SvgTransform, ReadsScaleWithOneOrTwoArguments) {
  const auto uniform = parseTransform("scale(3)");
  ASSERT_TRUE(uniform.has_value());
  const Point mapped = *uniform * Point {2, 5};
  EXPECT_NEAR(mapped.x, 6.0f, 1e-4f);
  EXPECT_NEAR(mapped.y, 15.0f, 1e-4f);
}

TEST(SvgTransform, RotatesAboutTheOriginAndAboutAPoint) {
  const auto aboutOrigin = parseTransform("rotate(90)");
  ASSERT_TRUE(aboutOrigin.has_value());
  const Point rotated = *aboutOrigin * Point {1, 0};
  EXPECT_NEAR(rotated.x, 0.0f, 1e-4f);
  EXPECT_NEAR(rotated.y, 1.0f, 1e-4f);

  // The three-argument form is a rotation about that point, so the point itself must not move.
  const auto aboutPoint = parseTransform("rotate(90 10 10)");
  ASSERT_TRUE(aboutPoint.has_value());
  const Point centre = *aboutPoint * Point {10, 10};
  EXPECT_NEAR(centre.x, 10.0f, 1e-4f);
  EXPECT_NEAR(centre.y, 10.0f, 1e-4f);
  // And a point one unit to the right of it ends one unit above.
  const Point nearby = *aboutPoint * Point {11, 10};
  EXPECT_NEAR(nearby.x, 10.0f, 1e-4f);
  EXPECT_NEAR(nearby.y, 11.0f, 1e-4f);
}

TEST(SvgTransform, ReadsSkews) {
  const auto skewX = parseTransform("skewX(45)");
  ASSERT_TRUE(skewX.has_value());
  // skewX(45) maps (0,1) to (1,1): tan(45) is one.
  const Point mapped = *skewX * Point {0, 1};
  EXPECT_NEAR(mapped.x, 1.0f, 1e-4f);
  EXPECT_NEAR(mapped.y, 1.0f, 1e-4f);
}

TEST(SvgTransform, AcceptsCommasAndWhitespaceBetweenArgumentGroups) {
  for (const char* text : {"translate(1,2)", "translate(1 2)", "translate( 1 , 2 )", "translate(1\n2)"}) {
    const auto transform = parseTransform(text);
    ASSERT_TRUE(transform.has_value()) << text;
    const Point mapped = *transform * Point {0, 0};
    EXPECT_NEAR(mapped.x, 1.0f, 1e-4f) << text;
    EXPECT_NEAR(mapped.y, 2.0f, 1e-4f) << text;
  }
}

TEST(SvgTransform, RejectsMalformedLists) {
  EXPECT_FALSE(parseTransform("translate(").has_value());
  EXPECT_FALSE(parseTransform("unknown(1)").has_value());
  EXPECT_FALSE(parseTransform("matrix(1 2 3)").has_value()); // six values are required
  EXPECT_FALSE(parseTransform("rotate()").has_value());
}

// ==================================================================================================
// Points
// ==================================================================================================

TEST(SvgPoints, ReadsAListWithAnySeparators) {
  const auto points = parsePoints("1,2 3,4 5,6");
  ASSERT_TRUE(points.has_value());
  ASSERT_EQ(points->size(), 3U);
  EXPECT_FLOAT_EQ((*points)[1].x, 3.0f);
  EXPECT_FLOAT_EQ((*points)[2].y, 6.0f);

  // Negative numbers need no separator at all.
  const auto tight = parsePoints("1 2-3-4");
  ASSERT_TRUE(tight.has_value());
  ASSERT_EQ(tight->size(), 2U);
  EXPECT_FLOAT_EQ((*tight)[1].x, -3.0f);
  EXPECT_FLOAT_EQ((*tight)[1].y, -4.0f);
}

TEST(SvgPoints, RejectsAnOddNumberOfCoordinates) {
  EXPECT_FALSE(parsePoints("1,2 3").has_value());
}

// ==================================================================================================
// Path data
// ==================================================================================================

TEST(SvgPathData, ReadsMovesAndLines) {
  const auto path = parsePathData("M 0 0 L 10 0 L 10 10 Z");
  ASSERT_TRUE(path.has_value());
  EXPECT_FALSE(path->isEmpty());
  EXPECT_EQ(pointCount(*path), 3U);
}

TEST(SvgPathData, TreatsExtraMoveArgumentsAsLines) {
  // `M 0 0 1 1 2 2` is a move and *two lines*, not three moves. Three moves would leave three open subpaths, and an
  // open subpath fills as if it were closed -- so the picture would show a triangle rather than a polyline.
  const auto asLines = parsePathData("M 0 0 1 1 2 2");
  const auto explicitLines = parsePathData("M 0 0 L 1 1 L 2 2");
  ASSERT_TRUE(asLines.has_value());
  ASSERT_TRUE(explicitLines.has_value());
  EXPECT_EQ(pointCount(*asLines), pointCount(*explicitLines));
}

TEST(SvgPathData, MeasuresRelativeCommandsFromTheCurrentPoint) {
  // After `M 10 10`, `l 5 0` ends at (15, 10) and `v 5` at (15, 15). Measuring from the subpath start instead would
  // put the second line back at the origin-relative position.
  const auto relative = parsePathData("M 10 10 l 5 0 v 5");
  const auto absolute = parsePathData("M 10 10 L 15 10 V 15");
  ASSERT_TRUE(relative.has_value());
  ASSERT_TRUE(absolute.has_value());

  EXPECT_NEAR(relative->bounds().getRight(), absolute->bounds().getRight(), 1e-4f);
  EXPECT_NEAR(relative->bounds().getBottom(), absolute->bounds().getBottom(), 1e-4f);
  EXPECT_NEAR(relative->bounds().getRight(), 15.0f, 1e-4f);
}

TEST(SvgPathData, ReturnsToTheSubpathStartAfterAClose) {
  // `Z` sets the current point back to where the subpath began, so a relative command after it is measured from there.
  // Carrying the pre-close point instead shifts everything after the first subpath.
  const auto path = parsePathData("M 10 10 L 20 10 Z l 5 0");
  ASSERT_TRUE(path.has_value());
  // The move after the close starts at (10,10) and runs to (15,10), so the bounds stay inside 10..20.
  EXPECT_NEAR(path->bounds().getLeft(), 10.0f, 1e-4f);
  EXPECT_NEAR(path->bounds().getRight(), 20.0f, 1e-4f);
}

TEST(SvgPathData, ReadsCubicAndSmoothCubicCommands) {
  const auto explicitCubic = parsePathData("M 0 0 C 0 10 10 10 10 0 S 20 -10 20 0");
  ASSERT_TRUE(explicitCubic.has_value());
  // A move contributes one point and each cubic three -- two controls and an endpoint -- so this is two cubics.
  EXPECT_EQ(pointCount(*explicitCubic), 7U);
}

TEST(SvgPathData, MakesASmoothCurveStraightWhenItFollowsSomethingElse) {
  // `S` reflects the previous cubic's control point, but only when the previous command *was* a cubic. After a line the
  // reflection is the current point, so the curve leaves straight. Reflecting unconditionally would bend it.
  const auto path = parsePathData("M 0 0 L 10 0 S 20 10 30 0");
  ASSERT_TRUE(path.has_value());

  // `bounds()` is the control bounds, so the first control point is inside it either way -- which is what makes this
  // observable at all. Reflecting (wrongly) would put that control at (20,-10) and pull the top of the bounds to -10;
  // using the current point leaves the control at (10,0) and the top at zero.
  EXPECT_NEAR(path->bounds().getTop(), 0.0f, 1e-3f);
}

TEST(SvgPathData, ReadsQuadraticAndSmoothQuadraticCommands) {
  const auto path = parsePathData("M 0 0 Q 5 10 10 0 T 20 0");
  ASSERT_TRUE(path.has_value());
  EXPECT_FALSE(path->isEmpty());
}

TEST(SvgPathData, ReadsHorizontalAndVerticalCommands) {
  const auto path = parsePathData("M 0 0 H 10 V 10 h -5 v -5");
  ASSERT_TRUE(path.has_value());
  EXPECT_NEAR(path->bounds().getRight(), 10.0f, 1e-4f);
  EXPECT_NEAR(path->bounds().getBottom(), 10.0f, 1e-4f);
}

TEST(SvgPathData, ReadsAnArcAsACurveReachingItsEndpoint) {
  // The endpoint is what the command promises, and it is the property a broken arc conversion loses first.
  const auto path = parsePathData("M 0 0 A 5 5 0 0 1 10 0");
  ASSERT_TRUE(path.has_value());
  EXPECT_NEAR(path->bounds().getRight(), 10.0f, 1e-2f);
  // A half circle of radius five bulges five units from the chord. A positive sweep-flag is the positive-angle direction,
  // which in SVG's y-down coordinates is towards negative y, so the bulge is the top of the control bounds.
  EXPECT_NEAR(path->bounds().getTop(), -5.0f, 0.2f);
  EXPECT_NEAR(path->bounds().getBottom(), 0.0f, 1e-2f);
}

TEST(SvgPathData, ReadsAnArcOnAnEllipse) {
  // Radii that differ are where a conversion that scales both axes by the same radius goes wrong.
  const auto path = parsePathData("M 0 0 A 10 2 0 0 1 20 0");
  ASSERT_TRUE(path.has_value());
  EXPECT_NEAR(path->bounds().getRight(), 20.0f, 1e-2f);
  // The bulge is the y radius and not the x radius: two, not ten.
  EXPECT_NEAR(path->bounds().getTop(), -2.0f, 0.2f);
}

TEST(SvgPathData, ReadsArcFlagsThatRunTogetherWithTheFollowingNumber) {
  // `A 5 5 0 0110 0` is the same command as `A 5 5 0 0 1 10 0`: a flag is one character, so reading it as a number
  // would swallow `0110`.
  const auto tight = parsePathData("M 0 0 A 5 5 0 0110 0");
  const auto spaced = parsePathData("M 0 0 A 5 5 0 0 1 10 0");
  ASSERT_TRUE(tight.has_value());
  ASSERT_TRUE(spaced.has_value());
  EXPECT_NEAR(tight->bounds().getRight(), spaced->bounds().getRight(), 1e-3f);
  EXPECT_NEAR(tight->bounds().getBottom(), spaced->bounds().getBottom(), 1e-3f);
}

TEST(SvgPathData, TreatsAZeroRadiusArcAsALine) {
  const auto path = parsePathData("M 0 0 A 0 0 0 0 1 10 10");
  ASSERT_TRUE(path.has_value());
  EXPECT_NEAR(path->bounds().getRight(), 10.0f, 1e-4f);
  EXPECT_NEAR(path->bounds().getBottom(), 10.0f, 1e-4f);
}

TEST(SvgPathData, ReadsMultipleSubpaths) {
  const auto path = parsePathData("M 0 0 L 10 0 Z M 20 20 L 30 20 Z");
  ASSERT_TRUE(path.has_value());
  EXPECT_NEAR(path->bounds().getLeft(), 0.0f, 1e-4f);
  EXPECT_NEAR(path->bounds().getRight(), 30.0f, 1e-4f);
}

TEST(SvgPathData, ReadsNumbersWithoutAnySeparators) {
  // The rule that most of the corpus depends on: `10-10` is two numbers.
  const auto path = parsePathData("M0 0L10-10");
  ASSERT_TRUE(path.has_value());
  EXPECT_NEAR(path->bounds().getRight(), 10.0f, 1e-4f);
  // `getBottom` is the larger y, so a point below the origin is in `getTop`.
  EXPECT_NEAR(path->bounds().getTop(), -10.0f, 1e-4f);
}

TEST(SvgPathData, AcceptsAnEmptyValueAsAnEmptyPath) {
  // An empty `d` is a legal way to draw nothing, which is different from a malformed one.
  const auto path = parsePathData("");
  ASSERT_TRUE(path.has_value());
  EXPECT_TRUE(path->isEmpty());
}

TEST(SvgPathData, RejectsAnUnknownCommand) {
  EXPECT_FALSE(parsePathData("M 0 0 X 10 10").has_value());
}

TEST(SvgPathData, RejectsAMissingArgument) {
  EXPECT_FALSE(parsePathData("M 0 0 L 10").has_value());
  EXPECT_FALSE(parsePathData("M 0 0 C 1 2 3").has_value());
}

TEST(SvgPathData, RejectsLeadingArgumentsWithNoCommand) {
  EXPECT_FALSE(parsePathData("0 0 L 1 1").has_value());
}
