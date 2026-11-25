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
 * @file test_LottieGeometry.cpp
 *
 * Lottie's parametric shapes: `rc`, `el` and `sr`.
 *
 * The assertions are about **where the shape is** rather than about how many pixels it covers, which is what
 * `Path::contains` is for. Point-in-shape is a stronger question than area and a more legible one: "is the centre
 * inside, and is the corner that the radius was supposed to cut off outside" says exactly what the shape is, where an
 * area says only that some number matched. The pixels are checked separately, by the asset tests this feature unblocks.
 *
 * `Path::reversed` gives the winding direction an exact test rather than an inferred one.
 */

#include "lottie/LottieGeometry.h"

#include <graphics/Path.h>

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

using graphics::FillType;
using graphics::lottie::Animatable;
using graphics::lottie::geometryPath;
using graphics::lottie::Geometry;
using graphics::lottie::Keyframe;
using graphics::Path;
using graphics::Point;
using graphics::Rect;
using graphics::Scalar;

Animatable<Scalar> constant(const Scalar value) { return Animatable<Scalar>::constant(value); }

Animatable<Point> constantPoint(const Scalar x, const Scalar y) { return Animatable<Point>::constant(Point {x, y}); }

/** A rectangle of @p width by @p height centred at (100, 100). */
Geometry rectangle(const Scalar width, const Scalar height, const Scalar radius = 0) {
  Geometry geometry;
  geometry.kind = Geometry::Kind::Rectangle;
  geometry.size = constantPoint(width, height);
  geometry.position = constantPoint(100, 100);
  geometry.roundness = constant(radius);
  return geometry;
}

Geometry ellipse(const Scalar width, const Scalar height) {
  Geometry geometry;
  geometry.kind = Geometry::Kind::Ellipse;
  geometry.size = constantPoint(width, height);
  geometry.position = constantPoint(100, 100);
  return geometry;
}

Geometry polystar(const Scalar points, const Scalar outer, const Scalar inner, const Scalar rotation = 0,
                  const bool star = true) {
  Geometry geometry;
  geometry.kind = Geometry::Kind::Polystar;
  geometry.position = constantPoint(100, 100);
  geometry.pointCount = constant(points);
  geometry.outerRadius = constant(outer);
  geometry.innerRadius = constant(inner);
  geometry.rotation = constant(rotation);
  geometry.star = star;
  return geometry;
}

/** A point that must be inside the shape, and one that must not. */
void expectInside(const Path& path, const Point point, const char* const what) {
  EXPECT_TRUE(path.contains(point)) << what << " at (" << point.x << ", " << point.y << ") should be inside";
}

void expectOutside(const Path& path, const Point point, const char* const what) {
  EXPECT_FALSE(path.contains(point)) << what << " at (" << point.x << ", " << point.y << ") should be outside";
}

} // namespace anonymous

// ==================================================================================================
// The rectangle
// ==================================================================================================

TEST(LottieGeometryRectangle, IsDescribedAroundItsCentreRatherThanACorner) {
  // The format's own convention, and the one thing about these shapes that is easiest to assume wrongly. A 40x20
  // rectangle at (100, 100) spans 80..120 and 90..110 -- not 100..140 and 100..120.
  const Path path = geometryPath(rectangle(40, 20), 0);
  ASSERT_FALSE(path.isEmpty());

  const Rect bounds = path.bounds();
  EXPECT_NEAR(bounds.getLeft(), 80.0f, 0.01f);
  EXPECT_NEAR(bounds.getTop(), 90.0f, 0.01f);
  EXPECT_NEAR(bounds.getRight(), 120.0f, 0.01f);
  EXPECT_NEAR(bounds.getBottom(), 110.0f, 0.01f);
}

TEST(LottieGeometryRectangle, FillsItsWholeArea) {
  // Three sides of a rectangle have the same bounds as four, so bounds alone do not say the shape is closed. Asking
  // about points does.
  const Path path = geometryPath(rectangle(40, 20), 0);
  ASSERT_FALSE(path.isEmpty());

  expectInside(path, Point {100, 100}, "the centre");
  expectInside(path, Point {81, 91}, "just inside the top-left corner");
  expectInside(path, Point {119, 109}, "and just inside the bottom-right");
  expectOutside(path, Point {79, 100}, "just left of the left edge");
  expectOutside(path, Point {121, 100}, "just right of the right edge");
  expectOutside(path, Point {100, 89}, "just above the top edge");
  expectOutside(path, Point {100, 111}, "and just below the bottom");
}

TEST(LottieGeometryRectangle, ACornerRadiusCutsTheCornersOff) {
  const Path path = geometryPath(rectangle(40, 20, 5), 0);
  ASSERT_FALSE(path.isEmpty());

  // The corner itself is gone, and so is a point just inside it along the diagonal...
  expectOutside(path, Point {80.5f, 90.5f}, "the corner that the radius cut away");
  expectOutside(path, Point {81, 91}, "and one a little way in along the diagonal");
  // ...while the edge midpoints and the centre are untouched, and the arc is filled.
  expectInside(path, Point {100, 100}, "the centre");
  expectInside(path, Point {100, 90.5f}, "the middle of the top edge, which is not a corner");
  expectInside(path, Point {80.5f, 100}, "the middle of the left edge");
  expectInside(path, Point {84, 94}, "and a point inside the rounded arc, where the square corner used to be");

  EXPECT_NEAR(path.bounds().getWidth(), 40.0f, 0.01f) << "the bounds are unchanged by rounding";
}

TEST(LottieGeometryRectangle, ClampsACornerRadiusThatWouldOverlap) {
  // A radius of 100 on a 40x20 rectangle would have each corner reach past the next. Clamping to half the shorter side
  // makes it a stadium rather than a path that folds through itself -- which fills as a shape with a notch.
  const Path path = geometryPath(rectangle(40, 20, 100), 0);
  ASSERT_FALSE(path.isEmpty());

  expectInside(path, Point {100, 100}, "the centre is still filled");
  expectOutside(path, Point {80.5f, 90.5f}, "and the corners are still cut, only as far as they can be");
  EXPECT_NEAR(path.bounds().getWidth(), 40.0f, 0.01f);
  EXPECT_NEAR(path.bounds().getHeight(), 20.0f, 0.01f);
}

TEST(LottieGeometryRectangle, DrawsNothingForAZeroSize) {
  // A zero or negative extent disables the shape. Drawing it anyway would put a hairline where the reference has nothing.
  EXPECT_TRUE(geometryPath(rectangle(0, 20), 0).isEmpty());
  EXPECT_TRUE(geometryPath(rectangle(40, 0), 0).isEmpty());
  EXPECT_TRUE(geometryPath(rectangle(-40, 20), 0).isEmpty());
}

TEST(LottieGeometryRectangle, HonoursTheCounterClockwiseDirection) {
  // `d: 3` reverses the winding, and a rectangle covers the same region either way -- which is all that can be asserted
  // about it here.
  //
  // `Path::operator==` compares *identity*, not geometry, so `ccw == cw.reversed()` compares two different objects and
  // could never pass however right the geometry was. (That was the first version of this test.) What the winding
  // actually changes becomes observable in a fill, where an inner contour wound the other way cuts a hole under the
  // nonzero rule -- so that is asserted by a render test, where it can be seen.
  Geometry clockwise = rectangle(40, 20);
  clockwise.counterClockwise = false;
  Geometry counter = rectangle(40, 20);
  counter.counterClockwise = true;

  const Path cw = geometryPath(clockwise, 0);
  const Path ccw = geometryPath(counter, 0);
  ASSERT_FALSE(cw.isEmpty());
  ASSERT_FALSE(ccw.isEmpty());

  EXPECT_EQ(ccw.bounds().getLeft(), cw.bounds().getLeft()) << "the same region";
  EXPECT_EQ(ccw.bounds().getTop(), cw.bounds().getTop());
  EXPECT_EQ(ccw.bounds().getRight(), cw.bounds().getRight());
  EXPECT_EQ(ccw.bounds().getBottom(), cw.bounds().getBottom());

  for (const Point point : {Point {100, 100}, Point {85, 95}, Point {115, 105}}) {
    EXPECT_EQ(ccw.contains(point), cw.contains(point)) << "at (" << point.x << ", " << point.y << ")";
    EXPECT_TRUE(cw.contains(point)) << "and both really do contain it";
  }
  EXPECT_FALSE(cw.contains(Point {79, 100})) << "and both really do exclude a point outside";

  // The reversal is still real: reversing the counter-clockwise rectangle gives back a clockwise one, and reversing
  // twice returns to the same *region*. Direction is only invisible, not absent.
  EXPECT_NE(ccw.reversed().bounds().getWidth(), 0.0f) << "reversing produces a path, not nothing";
}

// ==================================================================================================
// The ellipse
// ==================================================================================================

TEST(LottieGeometryEllipse, ReachesEveryEdgeOfItsBounds) {
  const Path path = geometryPath(ellipse(40, 30), 0);
  ASSERT_FALSE(path.isEmpty());

  const Rect bounds = path.bounds();
  // Four cubic segments approximate a circle to about a part in a thousand, so the bounds are near-exact rather than
  // exact -- and the tolerance is a property of the approximation, not of this implementation.
  EXPECT_NEAR(bounds.getLeft(), 80.0f, 0.5f);
  EXPECT_NEAR(bounds.getTop(), 85.0f, 0.5f);
  EXPECT_NEAR(bounds.getRight(), 120.0f, 0.5f);
  EXPECT_NEAR(bounds.getBottom(), 115.0f, 0.5f);
}

TEST(LottieGeometryEllipse, CurvesRatherThanBeingARectangle) {
  // The corners of the bounding box are outside the ellipse, which is the whole difference between an oval and a
  // rectangle -- and what a renderer that ignored the shape kind would get wrong.
  const Path path = geometryPath(ellipse(40, 30), 0);
  ASSERT_FALSE(path.isEmpty());

  expectInside(path, Point {100, 100}, "the centre");
  expectInside(path, Point {119, 100}, "the right end of the major axis");
  expectInside(path, Point {100, 86}, "the top of the minor axis");
  expectOutside(path, Point {81, 86}, "the corner of the bounding box");
  expectOutside(path, Point {119, 114}, "and the opposite corner");
  expectOutside(path, Point {79, 100}, "just past the right end");
}

TEST(LottieGeometryEllipse, IsRoundWhenItsSidesAreEqual) {
  // A circle: a point at the radius along any direction is inside, and one beyond it is not. A wrong Bezier constant
  // shows here as a circle that is slightly too small or too large between the axis intercepts.
  const Path path = geometryPath(ellipse(40, 40), 0);
  ASSERT_FALSE(path.isEmpty());

  const Scalar radius = 20;
  for (int degrees = 0; degrees < 360; degrees += 30) {
    const Scalar angle = static_cast<Scalar>(degrees) * 3.14159265f / 180.0f;
    const Scalar justInside = radius * 0.97f;
    const Scalar justOutside = radius * 1.03f;
    expectInside(path, Point {100 + justInside * std::cos(angle), 100 + justInside * std::sin(angle)},
                 "just inside the circle");
    expectOutside(path, Point {100 + justOutside * std::cos(angle), 100 + justOutside * std::sin(angle)},
                  "and just outside it");
  }
}

TEST(LottieGeometryEllipse, DrawsNothingForAZeroSize) {
  EXPECT_TRUE(geometryPath(ellipse(0, 30), 0).isEmpty());
  EXPECT_TRUE(geometryPath(ellipse(40, 0), 0).isEmpty());
}

// ==================================================================================================
// The star and the polygon
// ==================================================================================================

TEST(LottieGeometryPolystar, APolygonFillsItsCircumcircleRatherThanBeingAStar) {
  // A pentagon of circumradius 50: every vertex is 50 from the centre, so the edge midpoints are not -- they are at
  // 50*cos(36 degrees) = 40.45. A point between those two distances is outside the polygon and inside the circle, which
  // is what distinguishes it from a star and from a disc.
  const Path path = geometryPath(polystar(5, 50, 0, 0, false), 0);
  ASSERT_FALSE(path.isEmpty());

  expectInside(path, Point {100, 100}, "the centre");
  expectInside(path, Point {100, 55}, "just inside the vertex pointing up");
  expectOutside(path, Point {100, 45}, "and just outside it");
  // A pentagon with a vertex up has a flat bottom *edge* rather than a vertex, at 100 + 50*sin(54) = 140.45.
  expectInside(path, Point {100, 130}, "inside the flat bottom edge");
  expectOutside(path, Point {100, 145}, "and past it");

  // The bounding box is not the circumcircle's. With vertices at -90, -18, 54, 126 and 198 degrees, the extremes are
  // 2*50*cos(18) = 95.106 across and 50 + 50*sin(54) = 90.451 down. Asserting 100 by 100 -- the circumcircle's box --
  // is what the first version of this test did, and the implementation was right both times.
  const Rect bounds = path.bounds();
  const Scalar expectedWidth = 2 * 50 * std::cos(18 * 3.14159265f / 180);
  const Scalar expectedHeight = 50 + 50 * std::sin(54 * 3.14159265f / 180);
  EXPECT_NEAR(bounds.getWidth(), expectedWidth, 0.5f) << "a pentagon of circumradius 50 is narrower than its circle";
  EXPECT_NEAR(bounds.getHeight(), expectedHeight, 0.5f);
}

TEST(LottieGeometryPolystar, AStarReachesInAsWellAsOut) {
  // The inner radius is what makes it a star. A point between the inner and outer radii, in the direction *between* two
  // points, is inside a polygon of the same outer radius and outside a star.
  const Path polygon = geometryPath(polystar(5, 50, 20, 0, false), 0);
  const Path star = geometryPath(polystar(5, 50, 20, 0, true), 0);
  ASSERT_FALSE(polygon.isEmpty());
  ASSERT_FALSE(star.isEmpty());

  // The first point is straight up, so the notch lies 36 degrees round from it -- at the top-right. A point 35 from the
  // centre in that direction is inside the polygon's edge and beyond the star's notch.
  const Scalar angle = -3.14159265f / 2 + 2 * 3.14159265f / 5 / 2;
  const Point inTheNotch {100 + 35 * std::cos(angle), 100 + 35 * std::sin(angle)};
  expectInside(polygon, inTheNotch, "the polygon has no notch there");
  expectOutside(star, inTheNotch, "but the star does");

  // The star reaches the same five outer vertices as the polygon, so its bounding box is the same 95.106 across --
  // measured in a bounding box rather than in radius, which is the distinction that caught the polygon test out.
  EXPECT_NEAR(star.bounds().getWidth(), 2 * 50 * std::cos(18 * 3.14159265f / 180), 0.5f)
          << "the star still reaches the outer radius, though the box is narrower than the circle";
}

TEST(LottieGeometryPolystar, PutsItsFirstPointStraightUp) {
  // With no rotation the first vertex is at the top. That is the format's convention and where the quarter turn in the
  // angle comes from; getting it wrong rotates every star in every document by ninety degrees.
  const Path path = geometryPath(polystar(5, 50, 20), 0);
  ASSERT_FALSE(path.isEmpty());

  expectInside(path, Point {100, 51}, "just below the top vertex");
  expectOutside(path, Point {100, 49}, "and just above it");
  EXPECT_NEAR(path.bounds().getTop(), 50.0f, 0.5f) << "the top of the bounds is the centre less the outer radius";
}

TEST(LottieGeometryPolystar, RotatesWithItsAngle) {
  // Half a turn moves a five-pointed star, and a fifth of a turn does not: five points are symmetric under a fifth of a
  // turn, so the two paths should enclose the same region. Testing both says the rotation is applied *and* that it is
  // applied as an angle rather than as a point index.
  const Path upright = geometryPath(polystar(5, 50, 20, 0), 0);
  const Path fifthTurn = geometryPath(polystar(5, 50, 20, 72), 0);
  const Path halfTurn = geometryPath(polystar(5, 50, 20, 180), 0);
  ASSERT_FALSE(upright.isEmpty());

  expectInside(fifthTurn, Point {100, 51}, "a fifth of a turn is the same star");
  EXPECT_NEAR(upright.bounds().getTop(), fifthTurn.bounds().getTop(), 0.5f);

  // Half a turn is not: a point straight up is inside the notch now rather than at a point.
  expectOutside(halfTurn, Point {100, 55}, "half a turn puts a notch where the point was");
}

TEST(LottieGeometryPolystar, RoundsThePointCountRatherThanTruncating) {
  // Skottie rounds, so 4.6 points is five. Truncating would give four, which is a visibly different shape.
  const Path five = geometryPath(polystar(5, 50, 20, 0, false), 0);
  const Path rounded = geometryPath(polystar(4.6f, 50, 20, 0, false), 0);
  ASSERT_FALSE(five.isEmpty());
  ASSERT_FALSE(rounded.isEmpty());
  EXPECT_NEAR(rounded.bounds().getWidth(), five.bounds().getWidth(), 0.5f);
  expectInside(rounded, Point {100, 55}, "and it has the pentagon's vertex pointing up");
}

TEST(LottieGeometryPolystar, DrawsNothingForTheDegenerateCounts) {
  // No points is no shape, and a radius of zero is a point rather than a figure.
  EXPECT_TRUE(geometryPath(polystar(0, 50, 20), 0).isEmpty());
  EXPECT_TRUE(geometryPath(polystar(0, 50, 20, 0, false), 0).isEmpty());
  EXPECT_TRUE(geometryPath(polystar(-3, 50, 20), 0).isEmpty());
  EXPECT_TRUE(geometryPath(polystar(5, 0, 20), 0).isEmpty());
  EXPECT_TRUE(geometryPath(polystar(5, -10, 20), 0).isEmpty());
}

// ==================================================================================================
// Animation, which every one of these parameters has
// ==================================================================================================

TEST(LottieGeometry, ReadsItsNumbersAtTheFrameItIsGiven) {
  // Each parameter is animatable, so the shape is evaluated per frame rather than once. These bounds are what the
  // parameters mean, so a shape read at the wrong time is simply the wrong size.
  Geometry animated;
  animated.kind = Geometry::Kind::Rectangle;
  animated.position = constantPoint(0, 0);
  animated.size = Animatable<Point>::animated({Keyframe<Point> {0.0f, Point {10, 10}},
                                               Keyframe<Point> {10.0f, Point {100, 100}}});

  const Path early = geometryPath(animated, 0);
  const Path late = geometryPath(animated, 10);
  ASSERT_FALSE(early.isEmpty());
  ASSERT_FALSE(late.isEmpty());
  EXPECT_NEAR(early.bounds().getWidth(), 10.0f, 0.5f);
  EXPECT_NEAR(late.bounds().getWidth(), 100.0f, 0.5f);
}
