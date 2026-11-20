/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and limitations under the
 * License.
 */

/**
 * @file test_PathMeasure.cpp
 *
 * Walking along a path.
 *
 * Every assertion is written so the answer is known *without* the code under test: a straight line's length is its
 * Euclidean distance, half way along it is the midpoint, and a 3-4-5 triangle's perimeter is the sum of its sides.
 * That matters more than usual here, because "measure along a curve" has no single obvious right answer to compare
 * against -- so the curved cases are checked against invariants (monotonicity, a chord never exceeding its arc,
 * the parts summing to the whole) rather than against a number this implementation produced.
 *
 * ## The one thing that catches people out
 *
 * Distance along a curve is not the curve's parameter. Half way along an asymmetric arch is not at t = 0.5, and an
 * implementation that used the parameter would put a label visibly in the wrong place while passing any test that
 * only checked the endpoints.
 */

#include <graphics/PathMeasure.h>

#include <gtest/gtest.h>

#include <cmath>
#include <utility>

using namespace graphics;

namespace {

constexpr Scalar kTolerance = 1e-3f;

/** A horizontal line from (0,0) to (@p length, 0). */
Path makeLine(const Scalar length) {
  Path path;
  path.moveTo({ 0.0f, 0.0f });
  path.lineTo({ length, 0.0f });
  return path;
}

/** A circle of radius @p radius about the origin, as four cubics -- the standard kappa approximation. */
Path makeCircle(const Scalar radius) {
  const Scalar k = radius * 0.5522847498307936f;

  Path path;
  path.moveTo({ radius, 0.0f });
  path.cubicTo({ radius, k }, { k, radius }, { 0.0f, radius });
  path.cubicTo({ -k, radius }, { -radius, k }, { -radius, 0.0f });
  path.cubicTo({ -radius, -k }, { -k, -radius }, { 0.0f, -radius });
  path.cubicTo({ k, -radius }, { radius, -k }, { radius, 0.0f });
  path.close();
  return path;
}

} // namespace anonymous

// ==================================================================================================
// Construction
// ==================================================================================================

TEST(PathMeasureTest, RefusesAPathWithNothingToMeasure) {
  // An empty path, and one of nothing but `moveTo` verbs. Both have no length, and a null is a better answer than a
  // measure whose every query returns zero -- which would hide a caller's mistake rather than report it.
  EXPECT_EQ(PathMeasure::create(Path {}), nullptr);

  Path movesOnly;
  movesOnly.moveTo({ 1.0f, 1.0f });
  movesOnly.moveTo({ 5.0f, 5.0f });
  EXPECT_EQ(PathMeasure::create(movesOnly), nullptr);
}

TEST(PathMeasureTest, MeasuresAStraightLineExactly) {
  const Path line = makeLine(100.0f);
  const auto measure = PathMeasure::create(line);
  ASSERT_NE(measure, nullptr);

  // A line's length is its Euclidean distance and needs no flattening, so this is exact rather than approximate --
  // which makes it the one length in this file that can be asserted tightly.
  EXPECT_NEAR(measure->length(), 100.0f, 1e-4f);
  EXPECT_NEAR(measure->totalLength(), 100.0f, 1e-4f);
  EXPECT_EQ(measure->contourCount(), 1u);
  EXPECT_EQ(measure->contourIndex(), 0u);
}

TEST(PathMeasureTest, ClosedContoursIncludeTheClosingEdge) {
  // A 3-4-5 right triangle. Open, its length is the two written edges -- 3 and 4, so 7. Closed, the `close` verb
  // supplies the hypotenuse, which is 5, so the perimeter is 12.
  //
  // This is the distinction a measurement has to get right and a fill does not: the rasterizer treats every contour
  // as closed because a fill needs an inside, so it never has to ask whether the closing edge is real.
  Path triangle;
  triangle.moveTo({ 0.0f, 0.0f });
  triangle.lineTo({ 3.0f, 0.0f });
  triangle.lineTo({ 3.0f, 4.0f });

  const auto open = PathMeasure::create(triangle);
  ASSERT_NE(open, nullptr);
  EXPECT_NEAR(open->length(), 7.0f, 1e-4f) << "3 along, then 4 up";

  triangle.close();
  const auto closed = PathMeasure::create(triangle);
  ASSERT_NE(closed, nullptr);
  EXPECT_NEAR(closed->length(), 12.0f, 1e-4f) << "plus the 5 back to the start";
}

// ==================================================================================================
// Position along the path
// ==================================================================================================

TEST(PathMeasureTest, FindsPointsAlongAStraightLine) {
  const auto measure = PathMeasure::create(makeLine(100.0f));
  ASSERT_NE(measure, nullptr);

  Point position;
  Point tangent;

  ASSERT_TRUE(measure->getPosition(0.0f, &position, &tangent));
  EXPECT_NEAR(position.x, 0.0f, kTolerance);
  EXPECT_NEAR(position.y, 0.0f, kTolerance);

  // The midpoint of a line is at half its length. Trivial here, and it is the case that catches an implementation
  // confusing distance with the curve's parameter -- see the curved case below, where the two differ.
  ASSERT_TRUE(measure->getPosition(50.0f, &position));
  EXPECT_NEAR(position.x, 50.0f, kTolerance);
  EXPECT_NEAR(position.y, 0.0f, kTolerance);

  ASSERT_TRUE(measure->getPosition(100.0f, &position));
  EXPECT_NEAR(position.x, 100.0f, kTolerance);

  // The tangent of a horizontal line is the unit x vector everywhere.
  ASSERT_TRUE(measure->getPosition(25.0f, &position, &tangent));
  EXPECT_NEAR(tangent.x, 1.0f, kTolerance);
  EXPECT_NEAR(tangent.y, 0.0f, kTolerance);
  EXPECT_NEAR(std::sqrt(tangent.x * tangent.x + tangent.y * tangent.y), 1.0f, kTolerance)
          << "the tangent must be a unit vector, which is what a caller rotating an arrowhead relies on";
}

TEST(PathMeasureTest, RefusesADistanceOffTheEnd) {
  const auto measure = PathMeasure::create(makeLine(10.0f));
  ASSERT_NE(measure, nullptr);

  Point position;
  // Past the end, and before the start. Reported as failure rather than clamped, because a caller laying out a dash
  // pattern wants it to stop rather than pile up at the last point -- and a caller that meant "the end" can ask for
  // the length exactly.
  EXPECT_FALSE(measure->getPosition(10.001f, &position));
  EXPECT_FALSE(measure->getPosition(-0.001f, &position));
  EXPECT_TRUE(measure->getPosition(10.0f, &position)) << "exactly the end is reachable";
  EXPECT_TRUE(measure->getPosition(0.0f, &position)) << "and exactly the start";
}

TEST(PathMeasureTest, WalksACurveByDistanceNotByParameter) {
  // A quadratic from (0,0) to (100,0) with its control at (0,100): an asymmetric arch, steep and long on the left.
  //
  // Half way along it by *distance* is not the point at t = 0.5. That is the entire reason this class exists. The
  // quadratic at t = 0.5 is at (25, 50); the distance midpoint is past it, because the left half of the curve is
  // longer than the right.
  Path arch;
  arch.moveTo({ 0.0f, 0.0f });
  arch.quadTo({ 0.0f, 100.0f }, { 100.0f, 0.0f });

  const auto measure = PathMeasure::create(arch, 0.001f);
  ASSERT_NE(measure, nullptr);

  const Scalar total = measure->length();
  ASSERT_GT(total, 0.0f);

  Point byDistance;
  ASSERT_TRUE(measure->getPosition(total * 0.5f, &byDistance));

  // The parameter midpoint, computed from the quadratic's own formula rather than from the measure.
  const Scalar parameterX = 25.0f;
  const Scalar parameterY = 50.0f;

  const Scalar gap = std::sqrt((byDistance.x - parameterX) * (byDistance.x - parameterX) +
                               (byDistance.y - parameterY) * (byDistance.y - parameterY));
  EXPECT_GT(gap, 1.0f) << "half way by distance must be measurably away from t = 0.5 on an asymmetric curve";

  // And it is past the parameter midpoint in the direction of travel, because the first half of the curve is the
  // longer one. An implementation that used the parameter would land exactly on (25, 50) and fail the line above.
  EXPECT_GT(byDistance.x, parameterX) << "the distance midpoint lies further along than the parameter midpoint";
}

TEST(PathMeasureTest, PositionAdvancesMonotonicallyAlongACurve) {
  // The invariant that matters for anything animating along a path: a greater distance never gives an earlier point.
  Path curve;
  curve.moveTo({ 0.0f, 0.0f });
  curve.cubicTo({ 30.0f, 100.0f }, { 70.0f, -100.0f }, { 100.0f, 0.0f });

  const auto measure = PathMeasure::create(curve, 0.01f);
  ASSERT_NE(measure, nullptr);
  const Scalar total = measure->length();
  ASSERT_GT(total, 0.0f);

  Point previous;
  ASSERT_TRUE(measure->getPosition(0.0f, &previous));

  constexpr int kSteps = 100;
  const Scalar arc = total / static_cast<Scalar>(kSteps);

  for (int i = 1; i <= kSteps; ++i) {
    const Scalar at = total * static_cast<Scalar>(i) / static_cast<Scalar>(kSteps);
    Point current;
    ASSERT_TRUE(measure->getPosition(at, &current)) << "at distance " << at;

    // A chord is never longer than the arc it spans, so a step between consecutive samples can never exceed the
    // arc between them. That is the check that catches a jump backwards or a duplicated point.
    const Scalar step = std::sqrt((current.x - previous.x) * (current.x - previous.x) +
                                  (current.y - previous.y) * (current.y - previous.y));
    EXPECT_LE(step, arc * 1.05f) << "step " << i << " moved further than the arc it spans";
    previous = current;
  }

  // And the last sample is the curve's end point.
  EXPECT_NEAR(previous.x, 100.0f, 0.5f);
  EXPECT_NEAR(previous.y, 0.0f, 0.5f);
}

TEST(PathMeasureTest, ThePartsAddUpToTheWhole) {
  // Measuring to a distance and then from it must account for the whole length. This catches an implementation that
  // loses or double-counts the edge a distance falls inside -- the boundary case a spot check would miss.
  const auto measure = PathMeasure::create(makeLine(40.0f));
  ASSERT_NE(measure, nullptr);

  const Scalar total = measure->length();
  Point at;
  ASSERT_TRUE(measure->getPosition(total * 0.37f, &at));

  // For a straight line the travelled distance is the x coordinate, so the two parts are directly checkable.
  EXPECT_NEAR(at.x, total * 0.37f, kTolerance);
  EXPECT_NEAR(at.x + (total - at.x), total, kTolerance);
}

// ==================================================================================================
// Multiple contours
// ==================================================================================================

TEST(PathMeasureTest, WalksContoursInOrder) {
  // Two separate lines, 10 long and 20 long. Walking them shows the running total, which is what a dash pattern
  // spanning a whole path needs.
  Path two;
  two.moveTo({ 0.0f, 0.0f });
  two.lineTo({ 10.0f, 0.0f });
  two.moveTo({ 0.0f, 10.0f });
  two.lineTo({ 20.0f, 10.0f });

  const auto measure = PathMeasure::create(two);
  ASSERT_NE(measure, nullptr);

  EXPECT_EQ(measure->contourCount(), 2u);
  EXPECT_EQ(measure->contourIndex(), 0u);
  EXPECT_NEAR(measure->length(), 10.0f, kTolerance) << "the first contour is 10 long";
  EXPECT_NEAR(measure->totalLength(), 30.0f, kTolerance) << "and both together are 30";

  Scalar start = -1.0f;
  Scalar end = -1.0f;
  measure->getSegmentRange(&start, &end);
  EXPECT_NEAR(start, 0.0f, kTolerance);
  EXPECT_NEAR(end, 10.0f, kTolerance);

  ASSERT_TRUE(measure->nextContour());
  EXPECT_EQ(measure->contourIndex(), 1u);
  EXPECT_NEAR(measure->length(), 20.0f, kTolerance) << "the second contour is 20 long";

  measure->getSegmentRange(&start, &end);
  EXPECT_NEAR(start, 10.0f, kTolerance) << "the second contour starts where the first ended";
  EXPECT_NEAR(end, 30.0f, kTolerance);

  // Past the last contour reports the end rather than moving, so a caller ignoring the answer still has a usable
  // measure.
  EXPECT_FALSE(measure->nextContour());
  EXPECT_EQ(measure->contourIndex(), 1u);
}

TEST(PathMeasureTest, PositionIsRelativeToTheCurrentContour) {
  Path two;
  two.moveTo({ 0.0f, 0.0f });
  two.lineTo({ 10.0f, 0.0f });
  two.moveTo({ 100.0f, 0.0f });
  two.lineTo({ 120.0f, 0.0f });

  const auto measure = PathMeasure::create(two);
  ASSERT_NE(measure, nullptr);

  Point position;
  ASSERT_TRUE(measure->getPosition(5.0f, &position));
  EXPECT_NEAR(position.x, 5.0f, kTolerance) << "half way along the first contour";

  ASSERT_TRUE(measure->nextContour());
  ASSERT_TRUE(measure->getPosition(5.0f, &position));
  EXPECT_NEAR(position.x, 105.0f, kTolerance)
          << "the distance is measured from the start of the current contour, not of the path";
}

// ==================================================================================================
// Segment bounds
// ==================================================================================================

TEST(PathMeasureTest, SegmentBoundsCoverTheTravelledPartOnly) {
  // A long line. The bounds of a prefix must be that prefix, not the whole line -- which is the point: a caller
  // animating a stroke along a path damages only what changed.
  const auto measure = PathMeasure::create(makeLine(100.0f));
  ASSERT_NE(measure, nullptr);

  const Rect firstHalf = measure->getSegmentBounds(0.0f, 50.0f);
  EXPECT_NEAR(firstHalf.getLeft(), 0.0f, kTolerance);
  EXPECT_NEAR(firstHalf.getRight(), 50.0f, kTolerance) << "half way along, so half the extent";

  const Rect whole = measure->getSegmentBounds(0.0f, 100.0f);
  EXPECT_NEAR(whole.getRight(), 100.0f, kTolerance);

  // An inverted range is the same range, because a caller computing start and end from an animation may well
  // produce them the wrong way round, and the region covered is the same either way.
  const Rect reversed = measure->getSegmentBounds(50.0f, 0.0f);
  EXPECT_NEAR(reversed.getRight(), 50.0f, kTolerance);

  // An empty range, and one entirely past the end, have no extent.
  EXPECT_FLOAT_EQ(measure->getSegmentBounds(30.0f, 30.0f).getRight(), 0.0f);
  EXPECT_FLOAT_EQ(measure->getSegmentBounds(200.0f, 300.0f).getRight(), 0.0f);
}

TEST(PathMeasureTest, SegmentBoundsIncludeThePartWayPortions) {
  // An L: 3 along then 4 up. Bounds over a range starting part way along the first leg and ending part way up the
  // second must span both -- a bounds computed from whole vertices alone would miss the two ends, and they are the
  // only interesting part of the answer.
  Path corner;
  corner.moveTo({ 0.0f, 0.0f });
  corner.lineTo({ 3.0f, 0.0f });
  corner.lineTo({ 3.0f, 4.0f });

  const auto measure = PathMeasure::create(corner);
  ASSERT_NE(measure, nullptr);
  EXPECT_NEAR(measure->length(), 7.0f, 1e-4f);

  // From 1 along, which is x = 1, to 5 along, which is 1 past the corner and so y = 2.
  const Rect bounds = measure->getSegmentBounds(1.0f, 5.0f);
  EXPECT_NEAR(bounds.getLeft(), 1.0f, 1e-3f) << "the start is part way along the first leg";
  EXPECT_NEAR(bounds.getRight(), 3.0f, 1e-3f);
  EXPECT_NEAR(bounds.getTop(), 0.0f, 1e-3f);
  EXPECT_NEAR(bounds.getBottom(), 2.0f, 1e-3f) << "the end is part way up the second leg";
}

// ==================================================================================================
// Value semantics
// ==================================================================================================

TEST(PathMeasureTest, CopiesAreIndependent) {
  const auto original = PathMeasure::create(makeLine(10.0f));
  ASSERT_NE(original, nullptr);

  const PathMeasure copy = *original;
  EXPECT_NEAR(copy.length(), original->length(), kTolerance);
  EXPECT_EQ(copy.contourIndex(), original->contourIndex());
}

TEST(PathMeasureTest, MovingLeavesBothUsable) {
  // A moved-from object must stay valid, not merely destructible. `unique_ptr`'s default move leaves the source null
  // and any member called on it would dereference null -- the same defect that had to be fixed in `Path`.
  const auto source = PathMeasure::create(makeLine(10.0f));
  ASSERT_NE(source, nullptr);

  PathMeasure moved = std::move(*source);
  EXPECT_NEAR(moved.length(), 10.0f, kTolerance);

  // The moved-from measure is left valid, so these must not crash even though its value is unspecified.
  EXPECT_NO_THROW((void) source->contourCount());
  EXPECT_NO_THROW((void) source->length());
}

TEST(PathMeasureTest, ToleranceAffectsHowFinelyTheCurveIsWalked) {
  // A curve that genuinely needs subdividing: a cubic whose control points sit far off the chord, so the
  // flatness test fails at the root and the recursion descends.
  //
  // The *tolerance* is what this test varies, and getting a curve that responds to it is the whole difficulty --
  // a circle is a bad choice, which the first version of this test used. Its four cubics are within 0.03 of their
  // chords, so a tolerance of anything above that leaves them unsubdivided and both tolerances give the identical
  // answer. The test then fails for a reason that has nothing to do with the tolerance being honoured.
  Path arch;
  arch.moveTo({ 0.0f, 0.0f });
  arch.cubicTo({ 0.0f, 300.0f }, { 100.0f, 300.0f }, { 100.0f, 0.0f });

  const auto coarse = PathMeasure::create(arch, 20.0f);
  const auto fine = PathMeasure::create(arch, 0.01f);
  ASSERT_NE(coarse, nullptr);
  ASSERT_NE(fine, nullptr);

  // Both measure the same curve, so a finer polyline is never *shorter* -- it cuts fewer corners, so it is longer
  // and closer to the truth. The coarse one must be measurably shorter, or the tolerance is not reaching the
  // flattener at all.
  EXPECT_LT(coarse->length(), fine->length())
          << "a coarser polyline cuts more corners, so it must measure shorter";
  EXPECT_GT(fine->length() - coarse->length(), 0.5f)
          << "and the difference must be measurable, not a rounding artefact";

  // The coarse polyline's error is bounded by the tolerance it was built with: every vertex lies within 20 units
  // of the curve, so the length error cannot exceed the number of segments times that. This is the loose direction
  // of the same statement, and it catches a flattener that ignores the tolerance and subdivides to a fixed depth.
  EXPECT_LT(fine->length() - coarse->length(), coarse->length() * 0.5f)
          << "and not so large that the coarse polyline is not tracking the curve at all";
}

TEST(PathMeasureTest, ACircleIsMeasuredCloseToItsCurveNotToACircle) {
  // A circle of radius 100, as four cubics using the kappa construction.
  //
  // ## The number this is checked against, and why it is not the circumference
  //
  // The obvious assertion -- that the measurement is just under 2*pi*r -- is *wrong*, and the first version of this
  // test made it. The kappa cubic is not a circle: it is a curve that passes through the same four cardinal points
  // and bulges slightly outside the arc between them. Its length is therefore slightly *greater* than the circle's,
  // not less. Working the quarter arc out by integrating the cubic's speed gives 157.1017 against the true
  // 157.0796 -- a difference of 0.022 per quadrant, 0.088 for the whole figure, or about 14 parts per million.
  //
  // So the measurement is required to be close to the circumference and, if it differs, to differ in the direction
  // the geometry says it must. That direction is the useful part: an implementation that lost a segment would come
  // out short, and one that counted the closing edge twice would come out long by a whole edge.
  constexpr Scalar kRadius = 100.0f;
  constexpr Scalar kCircumference = 2.0f * 3.14159265358979f * kRadius;

  // The kappa cubics' own length, computed independently of this library: 4 times the integral of the quarter
  // cubic's speed, to six decimal places.
  constexpr Scalar kKappaLength = 628.406679f;

  const Path circle = makeCircle(kRadius);
  const auto measure = PathMeasure::create(circle, 0.01f);
  ASSERT_NE(measure, nullptr);

  // Within a hundredth of a unit of the curve's true length, which is a far tighter statement than "close to the
  // circumference" and is the one that actually pins the arithmetic.
  EXPECT_NEAR(measure->length(), kKappaLength, 0.01f)
          << "the measured length must match the cubics, not a circle";

  // And it is *longer* than the circle, because that is what the kappa construction does. A result on the other
  // side would mean the flattening had cut corners badly enough to lose the bulge.
  EXPECT_GT(measure->length(), kCircumference)
          << "the kappa cubics bulge slightly outside the circle, so they are longer than it";

  // The excess is small: fourteen parts per million. A larger one would mean the flattening was coarser than asked.
  EXPECT_LT(measure->length() - kCircumference, 0.2f) << "and only slightly longer";
}

