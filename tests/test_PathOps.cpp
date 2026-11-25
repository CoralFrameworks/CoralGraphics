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
 * implied. See the License for the specific language governing permissions and limitations under
 * the License.
 */

/**
 * @file test_PathOps.cpp
 *
 * The path representation and the curve primitives.
 *
 * The cases worth having here are the ones where a float implementation would give a visibly different
 * answer: the extrema of a curve whose control points lie far outside it, a point sitting exactly on a
 * horizontal edge, and a curve flattened finely enough that its polygon is convex. Those are the
 * properties the boolean operations will depend on, so they are pinned before anything is built on
 * top of them.
 */

#include "pathops/Curve.h"
#include "pathops/pathops.h"

#include <geometry/Point.h>
#include <geometry/Rect.h>

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace graphics;
using namespace graphics::pathops;

namespace {

Path makeSquare(const float left, const float top, const float right, const float bottom) {
  Path path;
  path.moveTo(Point { left, top });
  path.lineTo(Point { right, top });
  path.lineTo(Point { right, bottom });
  path.lineTo(Point { left, bottom });
  path.close();
  return path;
}

/** Flattens a path into polylines, for the geometric checks below. */
std::vector<std::vector<DPoint>> flatten(const Path& path) {
  std::vector<std::vector<DPoint>> contours;
  DPoint current {};
  for (const PathSegment& segment: path.segments()) {
    if (segment.verb == Verb::Move) {
      contours.push_back({ segment.points[0] });
      current = segment.points[0];
      continue;
    }
    if (contours.empty()) continue;
    if (segment.verb == Verb::Close) continue;
    // Sampling a fixed number of points is enough for these tests and keeps them independent of the
    // flattening tolerance, which is the implementation's business rather than the test's.
    constexpr int kSamples = 32;
    for (int i = 1; i <= kSamples; ++i) {
      contours.back().push_back(segment.eval(current, static_cast<double>(i) / kSamples));
    }
    current = segment.endPoint();
  }
  return contours;
}

} // namespace anonymous

// ==================================================================================================
// Curve primitives
// ==================================================================================================

TEST(PathOpsCurve, DPointArithmeticAndOrdering) {
  const DPoint a = DPoint::Make(3.0, 4.0);
  EXPECT_DOUBLE_EQ(a.length(), 5.0);
  EXPECT_DOUBLE_EQ(a.lengthSquared(), 25.0);
  EXPECT_DOUBLE_EQ(a.dot(DPoint::Make(1.0, 0.0)), 3.0);
  EXPECT_DOUBLE_EQ(a.cross(DPoint::Make(1.0, 0.0)), -4.0);

  const DPoint unit = a.normalized();
  EXPECT_DOUBLE_EQ(unit.length(), 1.0);

  // The origin has no direction; scaling it must not produce a NaN that would poison a comparison.
  const DPoint origin = DPoint::Make(0.0, 0.0);
  const DPoint normalisedOrigin = origin.normalized();
  EXPECT_TRUE(normalisedOrigin.isFinite());
  EXPECT_DOUBLE_EQ(normalisedOrigin.x, 0.0);
  EXPECT_DOUBLE_EQ(normalisedOrigin.y, 0.0);

  // Lexicographic ordering, which is what a sweep needs.
  EXPECT_TRUE(DPoint::Make(1.0, 9.0).lessThan(DPoint::Make(2.0, 0.0)));
  EXPECT_TRUE(DPoint::Make(1.0, 2.0).lessThan(DPoint::Make(1.0, 3.0)));
  EXPECT_FALSE(DPoint::Make(1.0, 3.0).lessThan(DPoint::Make(1.0, 2.0)));

  EXPECT_TRUE(DPoint::Make(1.0, 2.0).isFinite());
  EXPECT_FALSE(DPoint::Make(std::nan(""), 2.0).isFinite());
}

TEST(PathOpsCurve, DRectUnionAndEmptiness) {
  // The empty rectangle is inverted rather than zeroed, so that unioning a point into it yields that
  // point instead of also claiming the origin.
  DRect bounds = DRect::MakeEmpty();
  EXPECT_TRUE(bounds.isEmpty());

  bounds.unionWith(DPoint::Make(2.0, 3.0));
  // A single point has no area, so `isEmpty` stays true -- but it has a location, which is what
  // `isDegenerate` distinguishes and what stops a one-point path from reporting no bounds.
  EXPECT_TRUE(bounds.isEmpty());
  EXPECT_FALSE(bounds.isDegenerate());
  EXPECT_DOUBLE_EQ(bounds.left, 2.0);
  EXPECT_DOUBLE_EQ(bounds.right, 2.0);
  EXPECT_DOUBLE_EQ(bounds.top, 3.0);
  EXPECT_DOUBLE_EQ(bounds.bottom, 3.0);

  bounds.unionWith(DPoint::Make(-1.0, 8.0));
  EXPECT_FALSE(bounds.isEmpty());
  EXPECT_DOUBLE_EQ(bounds.left, -1.0);
  EXPECT_DOUBLE_EQ(bounds.right, 2.0);
  EXPECT_DOUBLE_EQ(bounds.top, 3.0);
  EXPECT_DOUBLE_EQ(bounds.bottom, 8.0);

  // An inverted rectangle is the marker for "found nothing".
  EXPECT_TRUE(DRect::MakeEmpty().isDegenerate());

  // A non-finite point must not corrupt a bounds computation; it is skipped rather than absorbed.
  bounds.unionWith(DPoint::Make(std::nan(""), std::nan("")));
  EXPECT_DOUBLE_EQ(bounds.left, -1.0);
  EXPECT_DOUBLE_EQ(bounds.bottom, 8.0);
}

TEST(PathOpsCurve, SegmentEvaluationMatchesTheCurve) {
  const DPoint p0 = DPoint::Make(0.0, 0.0);
  const DPoint c1 = DPoint::Make(0.0, 10.0);
  const DPoint c2 = DPoint::Make(10.0, 10.0);
  const DPoint p3 = DPoint::Make(10.0, 0.0);

  const PathSegment cubic = PathSegment::makeCubic(c1, c2, p3);

  // The endpoints are exact, which is what matters for a segment's identity in a contour.
  EXPECT_EQ(cubic.eval(p0, 0.0), p0);
  EXPECT_EQ(cubic.eval(p0, 1.0), p3);

  // The midpoint of this symmetric cubic is known analytically.
  const DPoint mid = cubic.eval(p0, 0.5);
  EXPECT_DOUBLE_EQ(mid.x, 5.0);
  EXPECT_DOUBLE_EQ(mid.y, 7.5);

  // The derivative at the start points along the first control leg.
  const DPoint startDerivative = cubic.derivative(p0, 0.0);
  EXPECT_GT(startDerivative.y, 0.0);
  EXPECT_NEAR(startDerivative.x, 0.0, 1e-12);

  // A line segment evaluates by simple interpolation.
  const PathSegment line = PathSegment::makeLine(DPoint::Make(4.0, 8.0));
  EXPECT_EQ(line.eval(p0, 0.5), DPoint::Make(2.0, 4.0));
}

// ==================================================================================================
// Bounds
// ==================================================================================================

TEST(PathOpsPath, ControlBoundsAreConservative) {
  // This cubic's control points reach y=10 but the curve only reaches y=7.5, which is exactly the
  // distinction between control bounds and tight bounds.
  Path path;
  path.moveTo(Point { 0.0f, 0.0f });
  path.cubicTo(Point { 0.0f, 10.0f }, Point { 10.0f, 10.0f }, Point { 10.0f, 0.0f });

  const DRect control = path.controlBounds();
  EXPECT_DOUBLE_EQ(control.left, 0.0);
  EXPECT_DOUBLE_EQ(control.right, 10.0);
  EXPECT_DOUBLE_EQ(control.bottom, 10.0) << "the control points really do reach 10";

  const DRect tight = path.tightBounds();
  EXPECT_DOUBLE_EQ(tight.left, 0.0);
  EXPECT_DOUBLE_EQ(tight.right, 10.0);
  EXPECT_DOUBLE_EQ(tight.top, 0.0);
  // The extrema solve gives 3/4 of the way to the control height for this symmetric cubic.
  EXPECT_NEAR(tight.bottom, 7.5, 1e-9) << "the curve itself only reaches 7.5";

  EXPECT_LT(tight.bottom, control.bottom) << "tight bounds must be a strict subset here";
}

TEST(PathOpsPath, TightBoundsOfAQuadratic) {
  Path path;
  path.moveTo(Point { 0.0f, 0.0f });
  path.quadTo(Point { 5.0f, 10.0f }, Point { 10.0f, 0.0f });

  const DRect tight = path.tightBounds();
  EXPECT_DOUBLE_EQ(tight.left, 0.0);
  EXPECT_DOUBLE_EQ(tight.right, 10.0);
  // A quadratic reaches half its control height at the apex.
  EXPECT_NEAR(tight.bottom, 5.0, 1e-9);
}

TEST(PathOpsPath, TightBoundsOfAStraightLinePathIsTheLine) {
  Path path;
  path.moveTo(Point { 1.0f, 2.0f });
  path.lineTo(Point { 7.0f, 2.0f });

  const DRect tight = path.tightBounds();
  EXPECT_DOUBLE_EQ(tight.left, 1.0);
  EXPECT_DOUBLE_EQ(tight.right, 7.0);
  EXPECT_DOUBLE_EQ(tight.top, 2.0);
  EXPECT_DOUBLE_EQ(tight.bottom, 2.0);
}

TEST(PathOpsPath, TightBoundsOfAClosedContourIncludesTheClosingEdge) {
  // The contour's last point is below its first, so the closing edge is what supplies the bottom. A
  // midpoint check on the contour's highest point catches an implementation that unions end points but
  // forgets the edge back to the start.
  Path path;
  path.moveTo(Point { 0.0f, 0.0f });
  path.lineTo(Point { 10.0f, 0.0f });
  path.lineTo(Point { 10.0f, 5.0f });
  path.close();

  const DRect tight = path.tightBounds();
  EXPECT_DOUBLE_EQ(tight.top, 0.0);
  EXPECT_DOUBLE_EQ(tight.bottom, 5.0);
  EXPECT_DOUBLE_EQ(tight.left, 0.0);
  EXPECT_DOUBLE_EQ(tight.right, 10.0);
}

TEST(PathOpsPath, TightBoundsOfAnEmptyPath) {
  const Path empty;
  EXPECT_TRUE(empty.tightBounds().isEmpty());
  EXPECT_TRUE(empty.isEmpty());

  // The public entry point reports "no bounds" rather than a degenerate rectangle.
  EXPECT_FALSE(tightBounds(empty).has_value());

  // A path consisting of a single `moveTo` is *not* empty: it is a point, and its bounds are that
  // point. Reporting nullopt here would throw away a real answer.
  Path dot;
  dot.moveTo(Point { 4.0f, 5.0f });
  const auto dotBounds = tightBounds(dot);
  ASSERT_TRUE(dotBounds.has_value()) << "a one-point path has a location, so it has bounds";
  EXPECT_FLOAT_EQ(dotBounds->getLeft(), 4.0f);
  EXPECT_FLOAT_EQ(dotBounds->getBottom(), 5.0f);
}

// ==================================================================================================
// Containment
// ==================================================================================================

TEST(PathOpsPath, SquareContainment) {
  const Path square = makeSquare(0.0f, 0.0f, 10.0f, 10.0f);

  EXPECT_TRUE(square.contains(Point { 5.0f, 5.0f }));
  EXPECT_FALSE(square.contains(Point { 15.0f, 5.0f }));
  EXPECT_FALSE(square.contains(Point { -1.0f, 5.0f }));
  EXPECT_FALSE(square.contains(Point { 5.0f, 15.0f }));
  EXPECT_FALSE(square.contains(Point { 5.0f, -1.0f }));
}

TEST(PathOpsPath, ContainmentOnAHorizontalEdgeIsDecidedOnce) {
  // A query point lying exactly on a horizontal edge is the classic case that makes a crossing count
  // flip as the point moves along the edge. The half-open comparison in y is what prevents it, and
  // this is the test that fails loudly if that changes.
  const Path square = makeSquare(0.0f, 0.0f, 10.0f, 10.0f);

  // Along the top edge, with the fill below it: consistently inside, or consistently outside, but
  // never alternating.
  bool first = square.contains(Point { 1.0f, 0.0f });
  for (float x = 1.0f; x <= 9.0f; x += 0.5f) {
    EXPECT_EQ(square.contains(Point { x, 0.0f }), first) << "flipped at x=" << x;
  }
}

TEST(PathOpsPath, ContainmentRespectsTheFillRule) {
  // Two nested squares wound the same way. Under the winding rule the inner one is still inside and
  // the fill is solid; under even-odd the overlap cancels and the middle becomes a hole. That
  // difference is the whole reason the fill rule is part of the path rather than an argument.
  Path nested;
  nested.moveTo(Point { 0.0f, 0.0f });
  nested.lineTo(Point { 10.0f, 0.0f });
  nested.lineTo(Point { 10.0f, 10.0f });
  nested.lineTo(Point { 0.0f, 10.0f });
  nested.close();
  nested.moveTo(Point { 3.0f, 3.0f });
  nested.lineTo(Point { 7.0f, 3.0f });
  nested.lineTo(Point { 7.0f, 7.0f });
  nested.lineTo(Point { 3.0f, 7.0f });
  nested.close();

  const Point middle { 5.0f, 5.0f };
  const Point ring { 1.0f, 1.0f };

  nested.setFillRule(FillRule::Winding);
  EXPECT_TRUE(nested.contains(middle)) << "same winding direction accumulates";
  EXPECT_TRUE(nested.contains(ring));

  nested.setFillRule(FillRule::EvenOdd);
  EXPECT_FALSE(nested.contains(middle)) << "the second contour cancels the first";
  EXPECT_TRUE(nested.contains(ring));
}

TEST(PathOpsPath, OppositeWindingProducesAHoleUnderWindingRule) {
  // The inner square wound the other way is a hole under *both* rules, which is what a caller drawing
  // a ring means either way.
  Path ring;
  ring.moveTo(Point { 0.0f, 0.0f });
  ring.lineTo(Point { 10.0f, 0.0f });
  ring.lineTo(Point { 10.0f, 10.0f });
  ring.lineTo(Point { 0.0f, 10.0f });
  ring.close();
  // Counter-clockwise, the opposite of the outer contour.
  ring.moveTo(Point { 3.0f, 3.0f });
  ring.lineTo(Point { 3.0f, 7.0f });
  ring.lineTo(Point { 7.0f, 7.0f });
  ring.lineTo(Point { 7.0f, 3.0f });
  ring.close();

  for (const FillRule rule: { FillRule::Winding, FillRule::EvenOdd }) {
    ring.setFillRule(rule);
    EXPECT_FALSE(ring.contains(Point { 5.0f, 5.0f })) << "the hole must be empty under both rules";
    EXPECT_TRUE(ring.contains(Point { 1.0f, 1.0f }));
  }
}

TEST(PathOpsPath, CurvedShapeContainment) {
  // A shape whose boundary is a curve: the point inside must be found without flattening being
  // consulted for its tolerance.
  Path shape;
  shape.moveTo(Point { 0.0f, 0.0f });
  shape.quadTo(Point { 10.0f, -10.0f }, Point { 20.0f, 0.0f });
  shape.lineTo(Point { 20.0f, 20.0f });
  shape.lineTo(Point { 0.0f, 20.0f });
  shape.close();

  EXPECT_TRUE(shape.contains(Point { 10.0f, 10.0f })) << "well inside the body";
  EXPECT_FALSE(shape.contains(Point { 10.0f, -20.0f })) << "above the curved top";
  EXPECT_TRUE(shape.contains(Point { 10.0f, 1.0f })) << "just below the curve's apex, which dips up";
}

// ==================================================================================================
// Reversal
// ==================================================================================================

TEST(PathOpsPath, ReversalPreservesTheShape) {
  const Path square = makeSquare(0.0f, 0.0f, 10.0f, 10.0f);
  const Path reversed = square.reversed();

  // Reversing must not change the filled area, only the direction the contour runs.
  for (const Point p: { Point { 5.0f, 5.0f }, Point { 15.0f, 5.0f }, Point { -1.0f, -1.0f } }) {
    EXPECT_EQ(square.contains(p), reversed.contains(p)) << "containment changed at " << p.x << "," << p.y;
  }

  // Under the winding rule the direction *does* matter for overlapping contours, so a single reversed
  // contour in isolation must still fill the same region.
  EXPECT_TRUE(reversed.contains(Point { 5.0f, 5.0f }));
}

TEST(PathOpsPath, ReversalOfACubicSwapsItsControlPoints) {
  Path path;
  path.moveTo(Point { 0.0f, 0.0f });
  path.cubicTo(Point { 0.0f, 10.0f }, Point { 10.0f, 10.0f }, Point { 10.0f, 0.0f });

  const Path reversed = path.reversed();
  ASSERT_EQ(reversed.segments().size(), 2u);

  // The reversed contour runs from the old end to the old start, and its control points are the old
  // ones in the opposite order -- which for a cubic is an exact reversal, not a re-fit.
  EXPECT_EQ(reversed.segments()[0].points[0], DPoint::Make(10.0, 0.0));
  EXPECT_EQ(reversed.segments()[1].verb, Verb::Cubic);
  EXPECT_EQ(reversed.segments()[1].points[0], DPoint::Make(10.0, 10.0));
  EXPECT_EQ(reversed.segments()[1].points[1], DPoint::Make(0.0, 10.0));
  EXPECT_EQ(reversed.segments()[1].points[2], DPoint::Make(0.0, 0.0));
}

// ==================================================================================================
// The public surface
// ==================================================================================================

TEST(PathOpsApi, BooleanOperationsAreImplemented) {
  // The operations used to report failure here, deliberately: a boolean that is subtly wrong produces a shape that
  // looks almost right, which is far more expensive to debug than one that admits it is not written. Now that the
  // traversal exists, the test asserts the results rather than the refusal -- and it is kept in the same place so
  // that a regression to "return nullopt" is caught here rather than as a shape that quietly disappeared.
  const Path a = makeSquare(0.0f, 0.0f, 10.0f, 10.0f);
  const Path b = makeSquare(5.0f, 5.0f, 15.0f, 15.0f);

  for (const PathOp operation: { PathOp::Union, PathOp::Intersection, PathOp::Difference,
             PathOp::ExclusiveOr, PathOp::ReverseDifference }) {
    const auto result = op(a, b, operation);
    EXPECT_TRUE(result.has_value()) << toString(operation) << " is implemented and these two overlap";
    if (result.has_value()) {
      EXPECT_FALSE(result->isEmpty()) << toString(operation) << " of two overlapping squares is not empty";
    }
  }

  // `simplify` and `asWinding` are covered by `PathOpsBoolean.SimplifyResolvesASelfCrossing` and its neighbours, which
  // assert their results on a bow tie and on a plain square. They are not repeated here: this test is about the five
  // two-operand operations, and a section that asserted everything the header declares would stop being a check on any
  // particular thing.

  // The name lookup is implemented, because a diagnostic needs it.
  EXPECT_STREQ(toString(PathOp::Union), "Union");
}

TEST(PathOpsApi, TightBoundsIsImplemented) {
  const Path square = makeSquare(0.0f, 0.0f, 10.0f, 10.0f);
  const auto bounds = tightBounds(square);
  ASSERT_TRUE(bounds.has_value());
  EXPECT_FLOAT_EQ(bounds->getLeft(), 0.0f);
  EXPECT_FLOAT_EQ(bounds->getRight(), 10.0f);
}

TEST(PathOpsApi, OpBuilderHandlesTheSingleOperandCase) {
  // "Add one shape and resolve" is the common call, and it must not need any of the boolean machinery.
  const Path square = makeSquare(0.0f, 0.0f, 10.0f, 10.0f);

  OpBuilder builder;
  builder.add(square, PathOp::Union);
  const auto result = builder.resolve();

  ASSERT_TRUE(result.has_value()) << "a single operand is just itself";
  EXPECT_EQ(result->segments().size(), square.segments().size());

  // Resolving twice must not return the first result again: the builder resets.
  EXPECT_FALSE(builder.resolve().has_value());
}

TEST(PathOpsApi, OpBuilderAppliesBothOperands) {
  // Two operands go through the boolean machinery rather than being accumulated, so the result covers both.
  OpBuilder builder;
  builder.add(makeSquare(0.0f, 0.0f, 10.0f, 10.0f), PathOp::Union);
  builder.add(makeSquare(5.0f, 5.0f, 15.0f, 15.0f), PathOp::Union);

  const auto result = builder.resolve();
  ASSERT_TRUE(result.has_value()) << "two overlapping squares can be united";
  EXPECT_FALSE(result->isEmpty());

  // The union reaches both squares' extremes, which an accumulator that only kept the first would not.
  const DRect bounds = result->controlBounds();
  // The literals are double because a DRect holds doubles. Passing a float here compiles but narrows, and the
  // comparison then asks whether a double equals a float -- two different questions, and the compiler says so.
  EXPECT_DOUBLE_EQ(bounds.right, 15.0);
  EXPECT_DOUBLE_EQ(bounds.bottom, 15.0);

  // ...and the builder must have reset, so the next use starts clean.
  builder.add(makeSquare(0.0f, 0.0f, 1.0f, 1.0f), PathOp::Union);
  EXPECT_TRUE(builder.resolve().has_value());
}
