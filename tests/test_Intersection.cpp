/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and limitations under the
 * License.
 */

/**
 * @file test_Intersection.cpp
 *
 * The curve intersection engine.
 *
 * ## What these tests are for
 *
 * This is the numerically hardest part of the path operations, and its failures are the worst kind: an
 * intersection that is off by a little splits a curve in the wrong place, and the result is a shape that
 * looks almost right. So the cases here are chosen so that the *answer is known independently* of the
 * code under test -- axis-aligned lines whose crossings are obvious by inspection, a curve meeting a
 * line at a point that can be verified by substituting the parameter back in, roots of polynomials whose
 * coefficients were constructed from known roots.
 *
 * The recurring check is the round trip: an intersection reports a parameter on each curve, and
 * evaluating the curve at that parameter must land on the reported point. That verifies the parameter
 * and the point agree, which is the property the splitter depends on and which a plausible-looking
 * implementation can get wrong in a way that no comparison against an expected point would catch.
 */

#include "pathops/Intersection.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

using namespace graphics;
using namespace graphics::pathops;

namespace {

/** Asserts that @p actual and @p expected are the same point to within @p tolerance. */
void expectPointNear(const DPoint& actual, const DPoint& expected, const double tolerance, const char* what) {
  EXPECT_NEAR(actual.x, expected.x, tolerance) << what << " (x)";
  EXPECT_NEAR(actual.y, expected.y, tolerance) << what << " (y)";
}

/**
 * The check that matters for every intersection: the reported parameters must actually locate the
 * reported point on the curves that produced them.
 */
void expectParametersAgreeWithPoint(const Segment& a, const Segment& b, const Intersection& hit,
        const double tolerance) {
  expectPointNear(a.eval(hit.t), hit.point, tolerance, "the parameter on the first curve locates the point");
  expectPointNear(b.eval(hit.u), hit.point, tolerance, "the parameter on the second curve locates the point");
}

} // namespace anonymous

// ==================================================================================================
// Polynomial roots: the numerical core
// ==================================================================================================

TEST(PathOpsIntersection, SolvesACubicWithKnownRoots) {
  // Built from known roots so the expected answer is not the solver's own output. (x - 0.2)(x - 0.5)(x - 0.8)
  // expands to x^3 - 1.5x^2 + 0.66x - 0.08, and all three roots lie inside the unit interval.
  const auto roots = solveUnitCubic(-0.08, 0.66, -1.5, 1.0);
  ASSERT_EQ(roots.size(), 3u);
  // The tolerance is 1e-6 rather than something tighter because three distinct real roots are found
  // through the trigonometric branch, whose accuracy is limited by the arc cosine. That is far finer
  // than locating a crossing on a curve needs -- a parameter error of 1e-8 moves the point by far less
  // than a pixel even on a path thousands of units across.
  EXPECT_NEAR(roots[0], 0.2, 1e-6);
  EXPECT_NEAR(roots[1], 0.5, 1e-6);
  EXPECT_NEAR(roots[2], 0.8, 1e-6);

  // Roots come back sorted, which the callers rely on to keep intersections in order.
  for (size_t i = 1; i < roots.size(); ++i) EXPECT_LT(roots[i - 1], roots[i]);
}

TEST(PathOpsIntersection, ExcludesRootsAtTheEndpoints) {
  // Roots at 0 and 1 exactly. They must not be reported: an intersection at a segment's endpoint is the
  // caller's endpoint case, and reporting it here as well would count the same point twice -- which in
  // the traversal means a spurious vertex, and in a boolean operation means a doubled edge.
  // x(x - 1) = x^2 - x, so the cubic coefficient is zero.
  const auto roots = solveUnitCubic(0.0, -1.0, 1.0, 0.0);
  EXPECT_TRUE(roots.empty()) << "roots at the endpoints belong to the caller's endpoint logic";

  // One interior root and two at the ends: only the interior one is reported.
  const auto mixed = solveUnitCubic(0.0, 0.0, -1.0, 1.0);  // x^3 - x^2 = x^2(x-1)
  EXPECT_TRUE(mixed.empty()) << "0 is a double root and 1 a single one; neither is interior";
}

TEST(PathOpsIntersection, HandlesDegenerateAndRepeatedRoots) {
  // The identically zero polynomial: every point is a root, which is not an answer the caller can use,
  // so it reports none rather than infinitely many.
  EXPECT_TRUE(solveUnitCubic(0.0, 0.0, 0.0, 0.0).empty());

  // A repeated interior root. (x - 0.5)^2 = x^2 - x + 0.25, as a quadratic.
  const auto repeated = solveUnitCubic(0.25, -1.0, 1.0, 0.0);
  ASSERT_EQ(repeated.size(), 2u) << "a double root is reported twice, from the two closed-form branches";
  for (const double root: repeated) EXPECT_NEAR(root, 0.5, 1e-8);

  // A cubic with one real root and a complex pair. (x - 0.5)(x^2 + 1) = x^3 - 0.5x^2 + x - 0.5.
  const auto single = solveUnitCubic(-0.5, 1.0, -0.5, 1.0);
  ASSERT_EQ(single.size(), 1u);
  EXPECT_NEAR(single[0], 0.5, 1e-8);

  // No real roots at all: x^2 + x + 1 has a negative discriminant.
  EXPECT_TRUE(solveUnitCubic(1.0, 1.0, 1.0, 0.0).empty());
}

TEST(PathOpsIntersection, SolvesRootsAcrossCoordinateMagnitudes) {
  // The same roots stated at wildly different scales. Normalising by the largest coefficient is what
  // makes the solver behave identically at both -- a fixed epsilon would fail at one end or the other,
  // and a path expressed in the millions is not unusual.
  for (const double scale: { 1e-6, 1.0, 1e6 }) {
    const auto roots = solveUnitCubic(-0.08 * scale, 0.66 * scale, -1.5 * scale, scale);
    ASSERT_EQ(roots.size(), 3u) << "at scale " << scale;
    EXPECT_NEAR(roots[0], 0.2, 1e-6) << "at scale " << scale;
    EXPECT_NEAR(roots[2], 0.8, 1e-6) << "at scale " << scale;
  }
}

// ==================================================================================================
// Segment basics
// ==================================================================================================

TEST(PathOpsIntersection, SubsegmentPreservesTheCurve) {
  // A cubic, split at several places. Each piece must lie on the original curve -- which is verified by
  // sampling the piece and checking that those points are on the original, rather than by comparing
  // control points, because a correct split produces different control points for the same curve.
  const Segment curve = Segment::cubic(DPoint::Make(0.0, 0.0), DPoint::Make(0.0, 10.0),
          DPoint::Make(10.0, 10.0), DPoint::Make(10.0, 0.0));

  for (const auto& [t0, t1]: { std::pair<double, double> { 0.0, 0.5 }, { 0.25, 0.75 }, { 0.5, 1.0 },
             { 0.1, 0.9 } }) {
    const Segment piece = curve.subsegment(t0, t1);

    // The endpoints must be exactly the original's values at those parameters, not merely close: this is
    // the property the splitter relies on to keep adjacent pieces joined.
    expectPointNear(piece.eval(0.0), curve.eval(t0), 1e-12, "the split starts where it should");
    expectPointNear(piece.eval(1.0), curve.eval(t1), 1e-12, "the split ends where it should");

    // And the interior must follow the original, sampled at several places.
    for (int i = 1; i < 8; ++i) {
      const double local = static_cast<double>(i) / 8.0;
      const double originalParameter = t0 + (t1 - t0) * local;
      expectPointNear(piece.eval(local), curve.eval(originalParameter), 1e-9, "the interior follows");
    }
  }
}

TEST(PathOpsIntersection, SegmentEvaluationAndDerivative) {
  const Segment quad = Segment::quad(DPoint::Make(0.0, 0.0), DPoint::Make(5.0, 10.0), DPoint::Make(10.0, 0.0));

  // The endpoints are exact, which is what makes a split curve stay joined to its neighbour.
  EXPECT_EQ(quad.eval(0.0), DPoint::Make(0.0, 0.0));
  EXPECT_EQ(quad.eval(1.0), DPoint::Make(10.0, 0.0));

  // A quadratic reaches half its control height at the apex.
  EXPECT_NEAR(quad.eval(0.5).y, 5.0, 1e-12);

  // The derivative at the apex is horizontal by symmetry.
  EXPECT_NEAR(quad.derivative(0.5).y, 0.0, 1e-12);

  // At the start it points along the first control leg, which runs from (0,0) to (5,10): the derivative
  // is twice that leg, so both components are positive and in the leg's proportion. An earlier version
  // of this test asserted the x component was zero, which would only hold if the control point were
  // directly above the start -- it is not, and the curve is not vertical where it begins.
  const DPoint startDerivative = quad.derivative(0.0);
  EXPECT_NEAR(startDerivative.x, 10.0, 1e-12) << "twice the leg's x component";
  EXPECT_NEAR(startDerivative.y, 20.0, 1e-12) << "twice the leg's y component";
  EXPECT_GT(startDerivative.y, 0.0) << "the curve rises away from its start";
}

// ==================================================================================================
// Line against line
// ==================================================================================================

TEST(PathOpsIntersection, TwoCrossingLinesMeetOnce) {
  // A plus sign, so the answer is obvious by inspection: they cross at the centre.
  const Segment horizontal = Segment::line(DPoint::Make(-10.0, 0.0), DPoint::Make(10.0, 0.0));
  const Segment vertical = Segment::line(DPoint::Make(0.0, -10.0), DPoint::Make(0.0, 10.0));

  const auto hits = intersect(horizontal, vertical);
  ASSERT_EQ(hits.size(), 1u);
  expectPointNear(hits[0].point, DPoint::Make(0.0, 0.0), 1e-12, "the crossing point");
  EXPECT_NEAR(hits[0].t, 0.5, 1e-12);
  EXPECT_NEAR(hits[0].u, 0.5, 1e-12);

  // Interior on both, and a crossing rather than a touch.
  EXPECT_EQ(hits[0].location, Location::Interior);
  EXPECT_FALSE(hits[0].isTangent);
  expectParametersAgreeWithPoint(horizontal, vertical, hits[0], 1e-12);
}

TEST(PathOpsIntersection, ParallelLinesDoNotMeet) {
  const Segment a = Segment::line(DPoint::Make(0.0, 0.0), DPoint::Make(10.0, 0.0));
  const Segment b = Segment::line(DPoint::Make(0.0, 5.0), DPoint::Make(10.0, 5.0));
  EXPECT_TRUE(intersect(a, b).empty());

  // Collinear overlap is deliberately not reported: it has no single parameter, and the caller handles
  // collinear runs as a separate case.
  const Segment c = Segment::line(DPoint::Make(5.0, 0.0), DPoint::Make(15.0, 0.0));
  EXPECT_TRUE(intersect(a, c).empty());
}

TEST(PathOpsIntersection, LinesThatWouldCrossOutsideTheSegmentsDoNot) {
  // Their infinite extensions cross at (0,0), but neither segment reaches it -- so there is no
  // intersection of the *segments*, and reporting one would put a vertex in the output where the shapes
  // do not touch.
  const Segment a = Segment::line(DPoint::Make(1.0, 1.0), DPoint::Make(10.0, 1.0));
  const Segment b = Segment::line(DPoint::Make(1.0, 1.0), DPoint::Make(1.0, 10.0));

  // They share the point (1,1) as their start, so that one *is* reported, as an endpoint.
  const auto hits = intersect(a, b);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].location, Location::BothEndpoints) << "a shared endpoint is a touch, not a crossing";
  expectPointNear(hits[0].point, DPoint::Make(1.0, 1.0), 1e-12, "the shared endpoint");

  // Now genuinely disjoint segments whose extensions would cross.
  const Segment c = Segment::line(DPoint::Make(5.0, 5.0), DPoint::Make(10.0, 5.0));
  const Segment d = Segment::line(DPoint::Make(5.0, 6.0), DPoint::Make(5.0, 10.0));
  EXPECT_TRUE(intersect(c, d).empty());
}

TEST(PathOpsIntersection, AnEndpointTouchIsClassifiedAsSuch) {
  // An L: two segments meeting at a corner. The corner is at the *end* of the first and the *start* of
  // the second, which is the ordinary case of a polyline and must not be mistaken for a crossing.
  const Segment a = Segment::line(DPoint::Make(0.0, 0.0), DPoint::Make(10.0, 0.0));
  const Segment b = Segment::line(DPoint::Make(10.0, 0.0), DPoint::Make(10.0, 10.0));

  const auto hits = intersect(a, b);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_NE(hits[0].location, Location::Interior) << "the corner is not an interior crossing";
  expectPointNear(hits[0].point, DPoint::Make(10.0, 0.0), 1e-12, "the corner");
}

TEST(PathOpsIntersection, ATJunctionIsAnEndpointOnOneSideOnly) {
  // A T: the horizontal bar runs past the point where the stem meets it, so the meeting is in the
  // *interior* of the bar and at the *end* of the stem. Distinguishing this from a corner is what lets
  // the traversal decide whether the region changes side.
  const Segment bar = Segment::line(DPoint::Make(0.0, 0.0), DPoint::Make(10.0, 0.0));
  const Segment stem = Segment::line(DPoint::Make(5.0, 0.0), DPoint::Make(5.0, 10.0));

  const auto hits = intersect(bar, stem);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].location, Location::SecondEndpoint);
  EXPECT_NEAR(hits[0].t, 0.5, 1e-12);
  EXPECT_NEAR(hits[0].u, 0.0, 1e-12);
}

// ==================================================================================================
// Line against curve
// ==================================================================================================

TEST(PathOpsIntersection, ALineCrossesAQuadraticWhereItShould) {
  // A quadratic arch from (0,0) up to (10,0) with its apex at y=5. A horizontal line at y=3 crosses it
  // twice, and the two crossings are symmetric about the apex by construction.
  const Segment quad = Segment::quad(DPoint::Make(0.0, 0.0), DPoint::Make(5.0, 10.0), DPoint::Make(10.0, 0.0));
  const Segment line = Segment::line(DPoint::Make(-5.0, 3.0), DPoint::Make(15.0, 3.0));

  const auto hits = intersect(line, quad);
  ASSERT_EQ(hits.size(), 2u) << "a line below the apex crosses the arch twice";

  for (const Intersection& hit: hits) {
    EXPECT_NEAR(hit.point.y, 3.0, 1e-9) << "every crossing is on the line";
    expectParametersAgreeWithPoint(line, quad, hit, 1e-9);
  }

  // Symmetry about the apex: the two curve parameters sum to one, and the two x coordinates are
  // equidistant from the apex at x=5.
  EXPECT_NEAR(hits[0].u + hits[1].u, 1.0, 1e-9);
  EXPECT_NEAR(hits[0].point.x + hits[1].point.x, 10.0, 1e-9);
  EXPECT_LT(hits[0].point.x, hits[1].point.x) << "results are sorted by the parameter on the first segment";
}

TEST(PathOpsIntersection, ALineAboveTheApexMisses) {
  const Segment quad = Segment::quad(DPoint::Make(0.0, 0.0), DPoint::Make(5.0, 10.0), DPoint::Make(10.0, 0.0));
  const Segment line = Segment::line(DPoint::Make(-5.0, 6.0), DPoint::Make(15.0, 6.0));

  // The apex is at y=5, so a line at y=6 is above the whole curve.
  EXPECT_TRUE(intersect(line, quad).empty());
}

TEST(PathOpsIntersection, ALineThroughTheApexIsATangency) {
  const Segment quad = Segment::quad(DPoint::Make(0.0, 0.0), DPoint::Make(5.0, 10.0), DPoint::Make(10.0, 0.0));
  // y = 5 exactly touches the apex at x = 5, where the curve's tangent is horizontal -- the same
  // direction as the line.
  const Segment line = Segment::line(DPoint::Make(-5.0, 5.0), DPoint::Make(15.0, 5.0));

  const auto hits = intersect(line, quad);
  ASSERT_GE(hits.size(), 1u) << "the line does meet the curve at the apex";
  EXPECT_TRUE(hits[0].isTangent)
          << "a horizontal line at the apex touches rather than crosses, and the traversal needs to know";
  EXPECT_NEAR(hits[0].point.x, 5.0, 1e-6);
  EXPECT_NEAR(hits[0].point.y, 5.0, 1e-6);
}

TEST(PathOpsIntersection, ALineCrossesACubicThreeTimes) {
  // A cubic whose y control values are 10, -15, 15, -10, against a horizontal line at y = 0.
  //
  // The crossings are designed to sit strictly inside the curve. An earlier version of this test used
  // control points whose crossings fell at t = 0, 0.5 and 1 -- and the solver correctly returned only
  // the middle one, because a crossing at a segment's endpoint is the caller's endpoint case and is
  // deliberately excluded here. The test was wrong, not the code, and the difference is exactly the kind
  // of thing a test built from a plausible-looking curve rather than a computed one gets wrong.
  const Segment cubic = Segment::cubic(DPoint::Make(0.0, 10.0), DPoint::Make(3.0, -15.0),
          DPoint::Make(7.0, 15.0), DPoint::Make(10.0, -10.0));
  const Segment line = Segment::line(DPoint::Make(-5.0, 0.0), DPoint::Make(15.0, 0.0));

  const auto hits = intersect(line, cubic);
  ASSERT_EQ(hits.size(), 3u) << "a cubic can cross a line three times, and this one does";

  for (const Intersection& hit: hits) {
    // 1e-6 rather than something tighter: the solve is exact in the line's parameter, but the curve's
    // parameter is recovered from a chord and then polished, which converges to about 1e-7 on a curve
    // this steep. That is four orders of magnitude finer than a pixel on any realistic path.
    EXPECT_NEAR(hit.point.y, 0.0, 1e-6) << "every crossing is on the line";
    expectParametersAgreeWithPoint(line, cubic, hit, 1e-6);
  }

  // The crossings must be in increasing order along both segments.
  for (size_t i = 1; i < hits.size(); ++i) {
    EXPECT_LT(hits[i - 1].t, hits[i].t);
    EXPECT_LT(hits[i - 1].u, hits[i].u);
  }
}

TEST(PathOpsIntersection, TheLineParameterIsRecoveredCorrectly) {
  // A vertical line, which is the case that breaks a naive "divide by the x component" recovery -- the
  // x component of the line's direction is zero, so the parameter has to come from y.
  const Segment quad = Segment::quad(DPoint::Make(0.0, 0.0), DPoint::Make(5.0, 10.0), DPoint::Make(10.0, 0.0));
  const Segment vertical = Segment::line(DPoint::Make(5.0, -1.0), DPoint::Make(5.0, 8.0));

  const auto hits = intersect(vertical, quad);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_NEAR(hits[0].point.x, 5.0, 1e-9);
  EXPECT_NEAR(hits[0].point.y, 5.0, 1e-9) << "the apex";
  expectParametersAgreeWithPoint(vertical, quad, hits[0], 1e-9);

  // On the line, y=5 is 6/9 of the way from -1 to 8.
  EXPECT_NEAR(hits[0].t, 6.0 / 9.0, 1e-9);
}

TEST(PathOpsIntersection, SwappingTheOperandsSwapsTheParameters) {
  // The closed form only exists for line-against-curve, so the other orientation is served by calling it
  // and swapping the parameters. That swap has to be complete -- both parameters and the classification
  // -- or the caller gets a location that disagrees with the parameters.
  const Segment quad = Segment::quad(DPoint::Make(0.0, 0.0), DPoint::Make(5.0, 10.0), DPoint::Make(10.0, 0.0));
  const Segment line = Segment::line(DPoint::Make(-5.0, 3.0), DPoint::Make(15.0, 3.0));

  const auto forward = intersect(line, quad);
  const auto reversed = intersect(quad, line);

  ASSERT_EQ(forward.size(), reversed.size());
  ASSERT_EQ(forward.size(), 2u);

  for (size_t i = 0; i < forward.size(); ++i) {
    EXPECT_NEAR(forward[i].point.x, reversed[i].point.x, 1e-9);
    EXPECT_NEAR(forward[i].point.y, reversed[i].point.y, 1e-9);
    // `forward` sorts by the parameter on the line, `reversed` by the parameter on the quad, so the
    // order may differ -- but each intersection's own parameters must be the mirror of the other's.
    const auto match = std::min_element(reversed.begin(), reversed.end(),
            [&forward, i](const Intersection& lhs, const Intersection& rhs) {
              return (lhs.point - forward[i].point).lengthSquared() <
                     (rhs.point - forward[i].point).lengthSquared();
            });
    EXPECT_NEAR(match->t, forward[i].u, 1e-7) << "the quad's parameter becomes the line's";
    EXPECT_NEAR(match->u, forward[i].t, 1e-7) << "the line's parameter becomes the quad's";
  }
}

// ==================================================================================================
// Curve against curve
// ==================================================================================================

TEST(PathOpsIntersection, TwoQuadraticsCross) {
  // The pair this file recorded as a known gap. The geometry was verified to cross exactly twice *before* the
  // implementation existed, by sampling the signed distance from one curve to the other and counting sign changes:
  // once at t = 0.1237 along the arch and once at t = 0.8162.
  //
  // That pre-computed answer is the whole value of this test. Curve-against-curve has no closed form worth writing,
  // so an implementation is easy to write in a way that agrees only with itself -- and three earlier attempts here
  // did exactly that. Checking against parameters derived outside the code is what makes this a test rather than a
  // restatement of the algorithm.
  const Segment arch = Segment::quad(DPoint::Make(0.0, 0.0), DPoint::Make(5.0, 10.0), DPoint::Make(10.0, 0.0));
  const Segment diver = Segment::quad(DPoint::Make(0.0, 4.0), DPoint::Make(6.0, -6.0), DPoint::Make(10.0, 8.0));

  const auto hits = intersect(arch, diver);
  ASSERT_EQ(hits.size(), 2u) << "these two curves cross twice";

  // The two parameters along the arch, checked against the independently sampled values.
  EXPECT_NEAR(hits[0].t, 0.1237, 0.01);
  EXPECT_NEAR(hits[1].t, 0.8162, 0.01);

  // And each intersection is genuinely on both curves -- the assertion that catches a parameter consistent with
  // itself but in the wrong place.
  for (const Intersection& hit: hits) {
    expectParametersAgreeWithPoint(arch, diver, hit, 1e-3);
    EXPECT_FALSE(hit.isTangent) << "a crossing is not a tangency";
  }

  // Reported in increasing order along the first curve, which the traversal relies on.
  EXPECT_LT(hits[0].t, hits[1].t);
  EXPECT_LT(hits[0].point.x, hits[1].point.x);

  // Swapping the operands swaps the parameters, because the answer is a property of the geometry rather than of
  // the order the two were passed in.
  const auto reversed = intersect(diver, arch);
  ASSERT_EQ(reversed.size(), 2u);
  for (const Intersection& forward: hits) {
    bool matched = false;
    for (const Intersection& back: reversed) {
      if ((forward.point - back.point).length() < 1e-3) {
        matched = true;
        EXPECT_NEAR(back.t, forward.u, 1e-2) << "the second curve's parameter becomes the first's";
        EXPECT_NEAR(back.u, forward.t, 1e-2);
      }
    }
    EXPECT_TRUE(matched) << "every intersection must still be found when the operands are swapped";
  }
}

TEST(PathOpsIntersection, TwoQuadraticsCrossAtEveryScaleAndOffset) {
  // The same shape at three scales and three translations, so that a crossing never lands on a convenient
  // coordinate. An implementation quietly depending on the curves sharing an endpoint or an axis would pass the test
  // above and fail here.
  for (const double scale: { 0.01, 1.0, 100.0 }) {
    for (const double offset: { 0.0, 7.5, -3.25 }) {
      const Segment arch = Segment::quad(DPoint::Make(offset, offset),
              DPoint::Make(offset + 5.0 * scale, offset + 10.0 * scale), DPoint::Make(offset + 10.0 * scale, offset));
      const Segment diver = Segment::quad(DPoint::Make(offset, offset + 4.0 * scale),
              DPoint::Make(offset + 6.0 * scale, offset - 6.0 * scale),
              DPoint::Make(offset + 10.0 * scale, offset + 8.0 * scale));

      const auto hits = intersect(arch, diver);
      ASSERT_EQ(hits.size(), 2u) << "at scale " << scale << ", offset " << offset;
      for (const Intersection& hit: hits) {
        expectParametersAgreeWithPoint(arch, diver, hit, 1e-2 * scale + 1e-6);
      }
    }
  }
}

TEST(PathOpsIntersection, TwoCubicsThatCross) {
  // Cubic against cubic, the case with no closed form at all. Two arches leaning opposite ways, so they swap sides
  // and must cross an even number of times.
  const Segment left = Segment::cubic(DPoint::Make(0.0, 0.0), DPoint::Make(2.0, 20.0), DPoint::Make(8.0, 20.0),
          DPoint::Make(10.0, 0.0));
  const Segment right = Segment::cubic(DPoint::Make(10.0, 10.0), DPoint::Make(8.0, -10.0), DPoint::Make(2.0, -10.0),
          DPoint::Make(0.0, 10.0));

  const auto hits = intersect(left, right);
  ASSERT_GE(hits.size(), 1u) << "these two cubics overlap and must cross";

  for (const Intersection& hit: hits) {
    expectParametersAgreeWithPoint(left, right, hit, 1e-2);
  }

  // An even count for two open curves that swap sides. That is a property of the geometry rather than of the
  // algorithm, and an implementation losing a crossing would violate it.
  EXPECT_EQ(hits.size() % 2u, 0u) << "two open curves that swap sides cross an even number of times";
}

TEST(PathOpsIntersection, ACurveTangentialToACurve) {
  // A line against a curve is handled separately, so this is curve-against-curve tangency: the arch's apex touches a
  // shallow curve that only grazes it, sharing the horizontal tangent there. Reporting it as a crossing would leave
  // a boolean operation deciding the region changes side where it does not.
  const Segment arch = Segment::quad(DPoint::Make(0.0, 0.0), DPoint::Make(5.0, 10.0), DPoint::Make(10.0, 0.0));
  const Segment graze = Segment::quad(DPoint::Make(0.0, 5.0), DPoint::Make(5.0, 5.0), DPoint::Make(10.0, 5.0));

  const auto hits = intersect(arch, graze);
  ASSERT_GE(hits.size(), 1u) << "the arch does reach the grazing curve at its apex";
  EXPECT_TRUE(hits.front().isTangent) << "and it touches rather than crosses";
  expectParametersAgreeWithPoint(arch, graze, hits.front(), 1e-2);
}

TEST(PathOpsIntersection, TwoCurvesThatDoNotMeet) {
  // Curves that come near but never touch. Reporting a crossing here would put a vertex in the output where the
  // shapes do not meet, which a boolean operation turns into a spurious corner.
  //
  // ## What this test used to assert, and why it was wrong
  //
  // The pair originally used here was an arch and a shallow valley that appeared, at a coarse sampling, to come
  // within a tenth of a unit and never meet. Both halves of that were false. Refining the closest-approach search
  // with a Newton polish drives the gap to 4.4e-16 -- which is not a near miss at all, it is an intersection at
  // machine precision, and the two curves really do cross at t = 0.8536. The engine was right to report it and the
  // test was asserting a falsehood.
  //
  // The pair below has a minimum separation of exactly 20, which is a gap no tolerance can mistake for a contact.
  // That makes it a test of the *refusal* rather than of a borderline case, and the borderline case is covered
  // instead by `ACurveTangentialToACurve`, where the curves genuinely touch.
  const Segment lower = Segment::quad(DPoint::Make(0.0, 0.0), DPoint::Make(5.0, 2.0), DPoint::Make(10.0, 0.0));
  const Segment upper = Segment::quad(DPoint::Make(0.0, 20.0), DPoint::Make(5.0, 22.0), DPoint::Make(10.0, 20.0));

  EXPECT_TRUE(intersect(lower, upper).empty()) << "curves 20 units apart do not meet";

  // And two curves that would meet if extended, but do not within their own extents. Reporting a crossing here
  // would mean the solver was ignoring the parameter range, which is the mistake that puts a vertex outside both
  // shapes.
  const Segment shortLeft = Segment::quad(DPoint::Make(0.0, 0.0), DPoint::Make(1.0, 2.0), DPoint::Make(2.0, 0.0));
  const Segment shortRight = Segment::quad(DPoint::Make(20.0, 5.0), DPoint::Make(21.0, -5.0), DPoint::Make(22.0, 5.0));
  EXPECT_TRUE(intersect(shortLeft, shortRight).empty()) << "these do not overlap at all";
}

TEST(PathOpsIntersection, IntersectionsAtEveryCoordinateScale) {
  // The same geometry at three scales. The tolerances are relative, so the answer must be the same --
  // which is the property that makes the engine usable on a path in the millions and one in the
  // fractions without the caller tuning anything.
  for (const double scale: { 1e-3, 1.0, 1e3 }) {
    const Segment quad = Segment::quad(DPoint::Make(0.0, 0.0), DPoint::Make(5.0 * scale, 10.0 * scale),
            DPoint::Make(10.0 * scale, 0.0));
    const Segment line = Segment::line(DPoint::Make(-1.0 * scale, 3.0 * scale),
            DPoint::Make(11.0 * scale, 3.0 * scale));

    const auto hits = intersect(line, quad);
    ASSERT_EQ(hits.size(), 2u) << "at scale " << scale;
    for (const Intersection& hit: hits) {
      EXPECT_NEAR(hit.point.y / scale, 3.0, 1e-6) << "at scale " << scale;
      expectParametersAgreeWithPoint(line, quad, hit, 1e-6 * scale + 1e-9);
    }
  }
}

TEST(PathOpsIntersection, LocationNamesAreUsable) {
  EXPECT_STREQ(toString(Location::Interior), "Interior");
  EXPECT_STREQ(toString(Location::FirstEndpoint), "FirstEndpoint");
  EXPECT_STREQ(toString(Location::SecondEndpoint), "SecondEndpoint");
  EXPECT_STREQ(toString(Location::BothEndpoints), "BothEndpoints");
}
