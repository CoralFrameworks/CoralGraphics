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

#ifndef CORAL_GRAPHICS_PATHOPS_INTERSECTION_H
#define CORAL_GRAPHICS_PATHOPS_INTERSECTION_H

#include "pathops/Curve.h"

#include <array>
#include <cstdint>
#include <vector>

/**
 * @file Intersection.h
 *
 * Where two curves meet.
 *
 * ## Why this is the hard part
 *
 * A boolean operation looks like it is about regions -- "the union of these two shapes" -- but almost
 * all of the difficulty is in this file. Everything else is bookkeeping over the answer to one question:
 * where do the two outlines cross? Get that wrong by one crossing and the result has a gap or a doubled
 * edge; get the *order* of two nearby crossings wrong and the result turns itself inside out.
 *
 * ## The approach
 *
 * Each pair of segments is reduced to a polynomial and solved:
 *
 *  - Two lines give a 2x2 system, solved directly in double.
 *  - A line and a quadratic, or a line and a cubic, give a polynomial in the curve's parameter by
 *    substituting the line's parametric equation into the curve's -- degree 2 and 3 respectively.
 *  - Two curves give a polynomial system that is genuinely harder; it is handled by subdividing one
 *    curve and testing the pieces against the other, which trades exactness for robustness in exactly
 *    the cases where the exact form is ill-conditioned.
 *
 * ## What "t" means, and why there are two of them
 *
 * An intersection is reported as a parameter on *each* curve, because that is what the caller needs to
 * split them: `a` says where on the first curve, `b` where on the second. A point alone would leave the
 * caller to invert the curve again, which is the same work done worse.
 *
 * The parameters are in [0,1]. An intersection at t=0 or t=1 is at an endpoint, which the operations
 * treat differently from one in the interior -- an endpoint touch does not change which side is inside,
 * so it is not a crossing. That distinction is what `Location` records, and it is why the parameter is
 * reported rather than only the point.
 */

namespace graphics::pathops {

/** A curve segment's control points, in the form the intersection routines work with. */
struct Segment {
  /** Up to four control points: 2 for a line, 3 for a quadratic, 4 for a cubic. */
  std::array<DPoint, 4> points {};
  /** 2, 3 or 4. A count of 2 is a line. */
  uint8_t count = 0;

  /** The number of the first control point. */
  [[nodiscard]] const DPoint& operator[](const size_t i) const noexcept { return points[i]; }

  [[nodiscard]] bool isLine() const noexcept { return count == 2; }
  [[nodiscard]] bool isQuad() const noexcept { return count == 3; }
  [[nodiscard]] bool isCubic() const noexcept { return count == 4; }

  /** @return the point at @p t, evaluated in Bernstein form. */
  [[nodiscard]] DPoint eval(double t) const noexcept;

  /** @return the derivative at @p t. */
  [[nodiscard]] DPoint derivative(double t) const noexcept;

  /** @return the bounds of the control points, which contains the curve. */
  [[nodiscard]] DRect controlBounds() const noexcept;

  /** @return the piece of this segment between @p t0 and @p t1, via de Casteljau. */
  [[nodiscard]] Segment subsegment(double t0, double t1) const noexcept;

  /** Builds a line segment. */
  [[nodiscard]] static Segment line(const DPoint& a, const DPoint& b) noexcept;

  /** Builds a quadratic. */
  [[nodiscard]] static Segment quad(const DPoint& p0, const DPoint& p1, const DPoint& p2) noexcept;

  /** Builds a cubic. */
  [[nodiscard]] static Segment cubic(
          const DPoint& p0, const DPoint& p1, const DPoint& p2, const DPoint& p3) noexcept;

  /** Builds the segment a @c PathSegment describes, given the point it starts from. */
  [[nodiscard]] static Segment fromPathSegment(const DPoint& start, const PathSegment& segment) noexcept;
};

/**
 * Where on a curve an intersection lies.
 *
 * This is the distinction the traversal is built on. Two curves that meet at an endpoint have merely
 * touched -- the region does not change side there -- whereas two that cross in their interiors swap
 * which is above the other. Treating a touch as a crossing puts a spurious vertex in the output;
 * treating a crossing as a touch loses one, and the result has a gap.
 */
enum class Location : uint8_t {
  /** In the interior of both curves. */
  Interior,
  /** At the start or end of the first curve, in the interior of the second. */
  FirstEndpoint,
  /** In the interior of the first curve, at an endpoint of the second. */
  SecondEndpoint,
  /** At an endpoint of both, i.e. they share a point without crossing. */
  BothEndpoints,
};

/** Human readable name of @p location, never null. */
[[nodiscard]] const char* toString(Location location) noexcept;

/** One point where two curves meet. */
struct Intersection {
  /** The point, which is on both curves within the tolerance used to find it. */
  DPoint point;

  /** The parameter on the first curve, in [0,1]. */
  double t = 0.0;

  /** The parameter on the second curve, in [0,1]. */
  double u = 0.0;

  /** Where the intersection sits on each curve. */
  Location location = Location::Interior;

  /**
   * True when the curves merely touch rather than cross through one another.
   *
   * A tangency -- a line grazing a circle, or two curves meeting at a cusp -- does not change which
   * side of the other the curve is on. It is a real intersection for the purpose of splitting the
   * curves, and not one for the purpose of deciding what is inside what.
   */
  bool isTangent = false;
};

/**
 * @return every point at which @p a and @p b meet, in increasing order of the parameter on @p a.
 *
 * A pair of curves of these degrees can meet up to nine times, so the result is a vector rather than a
 * single point. The parameters are exact in the sense that they locate the intersection on the curve
 * that produced them, which is what splitting needs -- a point that is slightly off both curves is far
 * less useful than a parameter that is exact on one.
 *
 * The tolerance is not a parameter. A caller cannot pick a good one: too tight and an intersection that
 * a rasterizer would show as real is discarded, too loose and two curves that pass near each other are
 * reported as meeting. The value used is tied to the scale of the coordinates, so it behaves the same
 * on a path in the thousands and a path in the fractions.
 */
[[nodiscard]] std::vector<Intersection> intersect(const Segment& a, const Segment& b) noexcept;

/**
 * @return every point where the line @p a meets the curve @p b.
 *
 * Exposed separately from @c intersect because a line against a curve is the case that arises when a
 * path is flattened, and it has a closed form that is both faster and more accurate than the general
 * subdivision approach.
 */
[[nodiscard]] std::vector<Intersection> intersectLineCurve(const Segment& a, const Segment& b,
        double minTolerance = 0.0) noexcept;

/**
 * @return the real roots of `c0 + c1*x + c2*x^2 + c3*x^3` that lie strictly inside (0,1).
 *
 * Fourth degree, with the leading coefficient ignored when it is zero so the same call handles a
 * quadratic that has been written into the cubic form. Roots exactly at 0 or 1 are excluded, because an
 * intersection at a curve's endpoint is handled by the endpoint logic in the caller rather than as a
 * root -- including them here would report the same point twice.
 *
 * Exposed for testing: this is the numerical core of the whole file, and pinning its behaviour directly
 * is far more informative than inferring it from intersection results.
 */
[[nodiscard]] std::vector<double> solveUnitCubic(double c0, double c1, double c2, double c3) noexcept;

} // namespace graphics::pathops

#endif // CORAL_GRAPHICS_PATHOPS_INTERSECTION_H
