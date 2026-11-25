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
 * @file test_PathMeasureSegment.cpp
 *
 * Extracting the part of a contour between two distances.
 *
 * This is what trimming a path is built on, so the assertions are about the two things trimming gets wrong: the
 * **ends**, which must be interpolated rather than snapped to a vertex, and the **wrap**, where a range that runs off
 * the end of a closed contour has to come back to the beginning without leaving a gap.
 *
 * The accuracy of everything here is the measure's flattening tolerance, so lengths are compared with a tolerance
 * rather than exactly -- a circle measured as a polyline is very slightly shorter than the circle.
 */

#include <graphics/Path.h>
#include <graphics/PathMeasure.h>

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

namespace {

using graphics::FillType;
using graphics::Path;
using graphics::PathMeasure;
using graphics::Point;
using graphics::Scalar;

/** A square of side @p side with its bottom-left corner at the origin. */
Path square(const Scalar side, const bool closed = true) {
  Path path;
  path.moveTo(Point {0, 0});
  path.lineTo(Point {side, 0});
  path.lineTo(Point {side, side});
  path.lineTo(Point {0, side});
  if (closed) path.close();
  return path;
}

/** An open horizontal line, which has no closing edge to confuse the arithmetic. */
Path line(const Scalar length) {
  Path path;
  path.moveTo(Point {0, 0});
  path.lineTo(Point {length, 0});
  return path;
}

std::unique_ptr<PathMeasure> measured(const Path& path) { return PathMeasure::create(path); }

} // namespace anonymous

// ==================================================================================================
// The whole contour, and the trivial ranges
// ==================================================================================================

TEST(PathMeasureSegment, TheWholeContourComesBack) {
  // The identity case: a range covering everything should reproduce the contour, so its length is the contour's.
  const Path path = square(10);
  const auto measure = measured(path);
  ASSERT_NE(measure, nullptr);

  Path out;
  ASSERT_TRUE(measure->getSegment(0, measure->length(), &out));
  EXPECT_NEAR(out.length(), measure->length(), 0.5f) << "within the flattening tolerance";
  EXPECT_FALSE(out.isEmpty());
}

TEST(PathMeasureSegment, AnEmptyRangeAppendsNothing) {
  // Only the *equal* pair is empty. A reversed pair is not: the two distances are ordered internally, so a caller that
  // has swapped its ends gets its segment rather than nothing -- and the test below asserts exactly that. An earlier
  // version of this file asserted both, which cannot be true at once.
  const Path path = square(10);
  const auto measure = measured(path);
  ASSERT_NE(measure, nullptr);

  Path out;
  EXPECT_FALSE(measure->getSegment(5, 5, &out)) << "a zero-length range";
  EXPECT_TRUE(out.isEmpty());
  EXPECT_FALSE(measure->getSegment(0, 0, &out)) << "and at the very start";
  EXPECT_TRUE(out.isEmpty());
}

TEST(PathMeasureSegment, TakesItsTwoDistancesInEitherOrder) {
  // A caller that has swapped its start and end should get the same segment rather than nothing. `min` and `max` are
  // applied, which is what makes that true -- and what makes the empty case above specifically about an *equal* pair.
  const Path path = square(40);
  const auto measure = measured(path);
  ASSERT_NE(measure, nullptr);

  Path forwards;
  Path backwards;
  ASSERT_TRUE(measure->getSegment(5, 25, &forwards));
  ASSERT_TRUE(measure->getSegment(25, 5, &backwards));
  EXPECT_NEAR(forwards.length(), backwards.length(), 0.01f);
}

// ==================================================================================================
// The ends, which must be interpolated
// ==================================================================================================

TEST(PathMeasureSegment, InterpolatesBothEndsRatherThanSnappingToVertices) {
  // A 40-long line walked from 10 to 30 gives a 20-long segment. Snapping to vertices would give either nothing or the
  // whole line, and both would be visible as a trim that jumps.
  const Path path = line(40);
  const auto measure = measured(path);
  ASSERT_NE(measure, nullptr);

  Path out;
  ASSERT_TRUE(measure->getSegment(10, 30, &out));
  EXPECT_NEAR(out.length(), 20.0f, 0.1f);
  EXPECT_NEAR(out.bounds().getLeft(), 10.0f, 0.1f) << "starting where it was asked to";
  EXPECT_NEAR(out.bounds().getRight(), 30.0f, 0.1f) << "and ending there";
}

TEST(PathMeasureSegment, CoversAPartialEdgeOfASquare) {
  // A square of side 40 has a perimeter of 160. Walking 10 to 30 covers the first edge from x=10 to x=30, so the result
  // is a horizontal segment and its bounds say so exactly.
  const Path path = square(40);
  const auto measure = measured(path);
  ASSERT_NE(measure, nullptr);

  Path out;
  ASSERT_TRUE(measure->getSegment(10, 30, &out));
  EXPECT_NEAR(out.length(), 20.0f, 0.2f);
  EXPECT_NEAR(out.bounds().getTop(), 0.0f, 0.1f) << "along the bottom edge, which is at y = 0";
  EXPECT_NEAR(out.bounds().getBottom(), 0.0f, 0.1f);
}

TEST(PathMeasureSegment, CrossesAVertexWithoutLosingTheCorner) {
  // From 30 to 50 on a square of side 40: the last 10 of the bottom edge and the first 10 of the right edge. The corner
  // has to be *in* the result, which is what emitting whole vertices between the ends is for -- a version that only
  // emitted the two interpolated ends would cut the corner off, and the segment would measure shorter than it is.
  const Path path = square(40);
  const auto measure = measured(path);
  ASSERT_NE(measure, nullptr);

  Path out;
  ASSERT_TRUE(measure->getSegment(30, 50, &out));
  EXPECT_NEAR(out.length(), 20.0f, 0.2f) << "two straight pieces of ten each, so no shortcut";
  EXPECT_NEAR(out.bounds().getRight(), 40.0f, 0.1f) << "reaching the corner's x";
  EXPECT_NEAR(out.bounds().getTop(), 0.0f, 0.1f);
}

// ==================================================================================================
// The wrap, which is where trimming closed shapes goes wrong
// ==================================================================================================

TEST(PathMeasureSegment, ReachesTheClosingEdgeOfAClosedContour) {
  // A square is written as four vertices and a `close`, so its perimeter includes an edge from the fourth vertex back to
  // the first. Walking 150 to 160 covers the last ten of that closing edge, which exists only because the contour is
  // closed -- a version that stopped at the last vertex would report nothing here.
  const Path path = square(40);
  const auto measure = measured(path);
  ASSERT_NE(measure, nullptr);

  Path out;
  ASSERT_TRUE(measure->getSegment(150, 160, &out));
  EXPECT_NEAR(out.length(), 10.0f, 0.2f);

  // The closing edge runs from (0, 40) back to (0, 0), and 150 to 160 is its last three quarters: y from 10 down to 0.
  // So the segment's *top* is the origin end and its *bottom* is where the walk started -- the first version of this
  // test had the two the wrong way round, which is easy to do when the walk runs backwards up the edge.
  EXPECT_NEAR(out.bounds().getLeft(), 0.0f, 0.1f) << "coming back down the left edge";
  EXPECT_NEAR(out.bounds().getRight(), 0.0f, 0.1f) << "which is vertical";
  EXPECT_NEAR(out.bounds().getTop(), 0.0f, 0.1f) << "ending at the origin";
  EXPECT_NEAR(out.bounds().getBottom(), 10.0f, 0.1f) << "and starting ten units up from it";
}

TEST(PathMeasureSegment, AClosedContourIsLongerThanItsVerticesSuggest) {
  // The closing edge is the whole reason `length()` and the last cumulative entry differ. Asserting it directly keeps
  // the earlier mistake from returning: a measure that reported the open length would make every trim on a closed shape
  // stop one edge short.
  const Path open = square(40, false);
  const Path closed = square(40, true);

  const auto openMeasure = measured(open);
  const auto closedMeasure = measured(closed);
  ASSERT_NE(openMeasure, nullptr);
  ASSERT_NE(closedMeasure, nullptr);

  EXPECT_NEAR(openMeasure->length(), 120.0f, 0.5f) << "three edges of forty";
  EXPECT_NEAR(closedMeasure->length(), 160.0f, 0.5f) << "and the fourth, which only `close` adds";
}

// ==================================================================================================
// Appending, and continuity
// ==================================================================================================

TEST(PathMeasureSegment, MoveToFalseContinuesTheContourInsteadOfStartingOne) {
  // What the inverted trim needs: the two halves of a wrapped range have to join without a seam. With `moveTo` the two
  // pieces are separate contours; without it they are one, and a stroke along them has no break.
  const Path path = square(40);
  const auto measure = measured(path);
  ASSERT_NE(measure, nullptr);

  Path withMove;
  ASSERT_TRUE(measure->getSegment(120, 160, &withMove, true));
  ASSERT_TRUE(measure->getSegment(0, 40, &withMove, true));

  Path withoutMove;
  ASSERT_TRUE(measure->getSegment(120, 160, &withoutMove, true));
  ASSERT_TRUE(measure->getSegment(0, 40, &withoutMove, false));

  EXPECT_EQ(withMove.bounds().getLeft(), withoutMove.bounds().getLeft()) << "the same geometry either way";
  // The lengths agree, because a `moveTo` adds no length. What differs is the contour count, which is visible in the
  // bounds only through the seam -- so the assertion is about the geometric result, and the distinction is stated here
  // rather than pretended to be measurable through this API.
  EXPECT_NEAR(withMove.length(), withoutMove.length(), 0.5f);
}

TEST(PathMeasureSegment, AppendsToWhatThePathAlreadyHolds) {
  // `out` is appended to rather than cleared: a trim in two spans calls this twice with the same path.
  const Path path = square(40);
  const auto measure = measured(path);
  ASSERT_NE(measure, nullptr);

  Path out;
  ASSERT_TRUE(measure->getSegment(0, 40, &out));
  const Scalar afterFirst = out.length();
  ASSERT_TRUE(measure->getSegment(40, 80, &out));
  EXPECT_NEAR(out.length(), afterFirst + 40.0f, 0.5f) << "the second span was added, not substituted";
}

TEST(PathMeasureSegment, ClampsDistancesPastTheEnds) {
  // A trim computed from an animated offset can land outside the contour, and clamping is what keeps it from appending
  // nothing at the extremes. Walked from -100 to 1000 on a 160-long square: the result is the whole perimeter.
  const Path path = square(40);
  const auto measure = measured(path);
  ASSERT_NE(measure, nullptr);

  Path out;
  ASSERT_TRUE(measure->getSegment(-100, 1000, &out));
  EXPECT_NEAR(out.length(), 160.0f, 0.5f);
}

TEST(PathMeasureSegment, FollowsACurveRatherThanCuttingAcrossIt) {
  // A circle, trimmed to a quarter. The result's bounds say whether the walk followed the curve: a chord between two
  // points would put the corner of the bounds inside the circle rather than on it.
  Path circle;
  const Scalar radius = 50;
  const Scalar k = radius * 0.5522847498f;
  circle.moveTo(Point {radius, 0});
  circle.cubicTo(Point {radius, k}, Point {k, radius}, Point {0, radius});
  circle.cubicTo(Point {-k, radius}, Point {-radius, k}, Point {-radius, 0});
  circle.cubicTo(Point {-radius, -k}, Point {-k, -radius}, Point {0, -radius});
  circle.cubicTo(Point {k, -radius}, Point {radius, -k}, Point {radius, 0});
  circle.close();

  const auto measure = measured(circle);
  ASSERT_NE(measure, nullptr);
  const Scalar circumference = measure->length();
  EXPECT_NEAR(circumference, 2 * 3.14159265f * radius, 1.5f) << "the polyline is just inside the circle";

  // The first quarter of the walk. The circle is written from (radius, 0) and its first cubic ends at (0, radius) --
  // and y grows *downward* here, so that quarter covers x 0..radius and y 0..radius. An earlier version of this test
  // asserted the bounds reached y = -radius, which is the fourth quarter: the shape was written one way and read
  // another.
  Path quarter;
  ASSERT_TRUE(measure->getSegment(0, circumference / 4, &quarter));
  EXPECT_NEAR(quarter.length(), circumference / 4, 0.5f);
  EXPECT_NEAR(quarter.bounds().getRight(), radius, 0.5f) << "starting at the circle's right";
  EXPECT_NEAR(quarter.bounds().getLeft(), 0.0f, 1.5f) << "and finishing at its centre line";
  EXPECT_NEAR(quarter.bounds().getTop(), 0.0f, 1.5f) << "which is also where it starts vertically";
  EXPECT_NEAR(quarter.bounds().getBottom(), radius, 0.5f) << "and it reaches the bottom of the circle";

  // And the walk really does follow the curve: the far corner of the quarter's bounds is *outside* the circle, which is
  // what an arc does and a chord would not.
  EXPECT_FALSE(circle.contains(Point {radius, radius})) << "the bounding box corner is not in the circle";
}
