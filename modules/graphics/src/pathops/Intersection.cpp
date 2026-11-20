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

#include "Intersection.h"

#include <geometry/Scalar.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace graphics::pathops {
namespace {

/**
 * The largest number of times subdivision will recurse before it gives up and reports a touch.
 *
 * This has to exceed the depth at which the termination test fires, or the recursion is cut off before
 * any leaf is ever recorded. That test waits until both boxes are smaller than the tolerance, which for
 * a path a few units across and a tolerance of 1e-10 takes about 33 halvings -- so a limit of 32, which
 * is what this was, meant the search pruned its way down and then returned nothing at all.
 *
 * The cost of a higher limit is bounded by the box test rather than by the limit itself: a pair of boxes
 * that no longer overlap is discarded immediately, so only the branches near a genuine intersection are
 * explored. What the limit protects against is the degenerate case where two curves overlap along a
 * stretch, where the boxes keep overlapping however small they get.
 */
constexpr int kMaxSubdivisionDepth = 64;

/**
 * How close two points must be to be called the same, relative to the size of the coordinates.
 *
 * A fixed epsilon is the wrong tool here. A path expressed in the thousands needs a larger absolute
 * tolerance than one expressed in fractions, or nothing ever compares equal; a fixed 1e-9 would call an
 * intersection on a large path a miss. Scaling by the coordinate magnitude makes the same code behave
 * the same way on both -- which is the property that matters, not the absolute number.
 */
double toleranceFor(const DRect& bounds) noexcept {
  const double magnitude = std::max({ std::fabs(bounds.left), std::fabs(bounds.right),
    std::fabs(bounds.top), std::fabs(bounds.bottom), 1.0 });
  return magnitude * 1e-10;
}

/** @return true when @p t is inside the unit interval, allowing for rounding at the ends. */
bool inUnitRange(const double t) noexcept { return t >= -1e-9 && t <= 1.0 + 1e-9; }

/** @return @p t clamped to [0,1]. */
double clamp01(const double t) noexcept { return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t); }

/**
 * @return true when @p bounds and @p other overlap, each grown by @p tolerance.
 *
 * The overlap test is inclusive, so two boxes that merely touch are reported as overlapping. That is the
 * safe direction for a subdivision search: a false positive costs one more round of subdivision, while a
 * false negative loses an intersection entirely and leaves a gap in the result.
 */
bool overlaps(const DRect& bounds, const DRect& other, const double tolerance) noexcept {
  return bounds.left - tolerance <= other.right && other.left - tolerance <= bounds.right &&
         bounds.top - tolerance <= other.bottom && other.top - tolerance <= bounds.bottom;
}

/** Records where on each curve an intersection at @p t and @p u sits. */
Location classifyLocation(const double t, const double u) noexcept {
  const bool tAtEnd = t <= 1e-9 || t >= 1.0 - 1e-9;
  const bool uAtEnd = u <= 1e-9 || u >= 1.0 - 1e-9;
  if (tAtEnd && uAtEnd) return Location::BothEndpoints;
  if (tAtEnd) return Location::FirstEndpoint;
  if (uAtEnd) return Location::SecondEndpoint;
  return Location::Interior;
}

/**
 * @return true when the two curves touch rather than cross at the intersection (@p t, @p u).
 *
 * The test is the sign of the cross product of the two tangents. When the tangents are parallel the
 * cross product is zero, and the curves either touch and turn back (a tangency) or are collinear for a
 * stretch (an overlap, which the caller handles). Both are "not a crossing", which is the answer this
 * reports.
 *
 * The tolerance is on the *normalised* cross product, so it does not depend on how long the tangent
 * vectors happen to be -- comparing raw cross products would call a sharp crossing of two short
 * tangents a tangency.
 */
bool isTangentAt(const Segment& a, const Segment& b, const double t, const double u) noexcept {
  const DPoint da = a.derivative(clamp01(t));
  const DPoint db = b.derivative(clamp01(u));

  const double lengthA = da.length();
  const double lengthB = db.length();
  if (!(lengthA > 0.0) || !(lengthB > 0.0)) return true;  // a cusp or a degenerate segment

  const double sine = da.cross(db) / (lengthA * lengthB);
  return std::fabs(sine) < 1e-6;
}

/** Removes duplicated intersections, keeping the first of each group. */
void removeDuplicates(std::vector<Intersection>& found, const double tolerance) {
  if (found.size() < 2) return;

  std::vector<Intersection> kept;
  kept.reserve(found.size());
  for (const Intersection& candidate: found) {
    bool duplicate = false;
    for (const Intersection& existing: kept) {
      if ((candidate.point - existing.point).lengthSquared() <= tolerance * tolerance) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) kept.push_back(candidate);
  }
  found = std::move(kept);
}

// ==================================================================================================
// Curve-curve intersection by subdivision
// ==================================================================================================

} // namespace anonymous

const char* toString(const Location location) noexcept {
  switch (location) {
    case Location::Interior: return "Interior";
    case Location::FirstEndpoint: return "FirstEndpoint";
    case Location::SecondEndpoint: return "SecondEndpoint";
    case Location::BothEndpoints: return "BothEndpoints";
  }
  return "Unknown Location";
}

// ==================================================================================================
// Segment
// ==================================================================================================

Segment Segment::line(const DPoint& a, const DPoint& b) noexcept {
  Segment segment;
  segment.points[0] = a;
  segment.points[1] = b;
  segment.count = 2;
  return segment;
}

Segment Segment::quad(const DPoint& p0, const DPoint& p1, const DPoint& p2) noexcept {
  Segment segment;
  segment.points[0] = p0;
  segment.points[1] = p1;
  segment.points[2] = p2;
  segment.count = 3;
  return segment;
}

Segment Segment::cubic(const DPoint& p0, const DPoint& p1, const DPoint& p2, const DPoint& p3) noexcept {
  Segment segment;
  segment.points[0] = p0;
  segment.points[1] = p1;
  segment.points[2] = p2;
  segment.points[3] = p3;
  segment.count = 4;
  return segment;
}

Segment Segment::fromPathSegment(const DPoint& start, const PathSegment& segment) noexcept {
  switch (segment.verb) {
    case Verb::Line:
    case Verb::Close:
      return line(start, segment.verb == Verb::Close ? start : segment.points[0]);
    case Verb::Quad:
      return quad(start, segment.points[0], segment.points[1]);
    case Verb::Cubic:
      return cubic(start, segment.points[0], segment.points[1], segment.points[2]);
    case Verb::Move:
    case Verb::Done:
      break;
  }
  // A move or the end marker has no extent, so it is reported as a degenerate line rather than as
  // something with a shape. The caller is expected to skip it.
  return line(start, start);
}

DPoint Segment::eval(const double t) const noexcept {
  switch (count) {
    case 2:
      return points[0] + (points[1] - points[0]) * t;
    case 3: {
      const double u = 1.0 - t;
      return points[0] * (u * u) + points[1] * (2.0 * u * t) + points[2] * (t * t);
    }
    case 4: {
      const double u = 1.0 - t;
      const double uu = u * u;
      const double tt = t * t;
      return points[0] * (uu * u) + points[1] * (3.0 * uu * t) + points[2] * (3.0 * u * tt) +
             points[3] * (tt * t);
    }
    default:
      break;
  }
  return points[0];
}

DPoint Segment::derivative(const double t) const noexcept {
  switch (count) {
    case 2:
      return points[1] - points[0];
    case 3: {
      const double u = 1.0 - t;
      return (points[1] - points[0]) * (2.0 * u) + (points[2] - points[1]) * (2.0 * t);
    }
    case 4: {
      const double u = 1.0 - t;
      return (points[1] - points[0]) * (3.0 * u * u) + (points[2] - points[1]) * (6.0 * u * t) +
             (points[3] - points[2]) * (3.0 * t * t);
    }
    default:
      break;
  }
  return { 0.0, 0.0 };
}

DRect Segment::controlBounds() const noexcept {
  DRect bounds = DRect::MakeEmpty();
  for (uint8_t i = 0; i < count; ++i) bounds.unionWith(points[i]);
  return bounds;
}

Segment Segment::subsegment(const double t0, const double t1) const noexcept {
  // Splitting at t0 and then taking the first `t1'` of what remains, where t1' is t1 rescaled into the
  // new parameter. De Casteljau is used twice rather than once with a reparameterisation because it is
  // the numerically stable way to do it, and because doing it in one step needs the same two steps
  // anyway.
  const auto splitAt = [](const Segment& s, const double t) {
    Segment left = s;
    Segment right = s;
    // Repeated linear interpolation between neighbouring control points, which is de Casteljau.
    std::array<DPoint, 4> work = s.points;
    for (uint8_t level = 0; level + 1 < s.count; ++level) {
      for (uint8_t i = 0; i + 1 < s.count - level; ++i) {
        work[i] = work[i] + (work[i + 1] - work[i]) * t;
      }
      left.points[level + 1] = work[0];
      right.points[s.count - 2 - level] = work[s.count - 2 - level];
    }
    left.points[0] = s.points[0];
    left.count = s.count;
    right.points[s.count - 1] = s.points[s.count - 1];
    right.count = s.count;
    return std::pair<Segment, Segment>(left, right);
  };

  Segment tail = *this;
  if (t0 > 0.0) tail = splitAt(*this, clamp01(t0)).second;

  if (t1 >= 1.0) return tail;

  // Rescale t1 into the tail's own parameter, guarding the degenerate case where the whole segment was
  // asked for.
  const double span = 1.0 - clamp01(t0);
  if (!(span > 0.0)) return tail;
  const double local = clamp01((t1 - clamp01(t0)) / span);
  return splitAt(tail, local).first;
}

// ==================================================================================================
// Polynomial roots
// ==================================================================================================

std::vector<double> solveUnitCubic(const double c0, const double c1, const double c2, double c3) noexcept {
  std::vector<double> roots;

  const double scale = std::max({ std::fabs(c0), std::fabs(c1), std::fabs(c2), std::fabs(c3) });
  if (!(scale > 0.0)) return roots;  // the identically-zero polynomial: every point, reported as none

  // Normalising first is what keeps the comparisons below meaningful at any coordinate magnitude.
  const double a0 = c0 / scale;
  const double a1 = c1 / scale;
  const double a2 = c2 / scale;
  const double a3 = c3 / scale;

  /**
   * Keeps a root only when it is genuinely inside the interval.
   *
   * A root that lands marginally outside is pulled to the boundary and then rejected, because an
   * intersection at a segment's own endpoint is the caller's endpoint case and would otherwise be
   * reported twice -- once here and once by the endpoint test.
   */
  const auto record = [&roots](const double t) {
    if (t > 1e-9 && t < 1.0 - 1e-9) {
      roots.push_back(t);
    } else if (t >= -1e-9 && t <= 1.0 + 1e-9) {
      const double clamped = clamp01(t);
      if (clamped > 1e-9 && clamped < 1.0 - 1e-9) roots.push_back(clamped);
    }
  };

  if (std::fabs(a3) < 1e-12) {
    // A quadratic, or a line. The cubic coefficient being negligible is the common case when a cubic
    // curve's control points happen to be collinear with the line being tested.
    if (std::fabs(a2) < 1e-12) {
      if (std::fabs(a1) > 1e-12) record(-a0 / a1);
      return roots;
    }
    const double discriminant = a1 * a1 - 4.0 * a2 * a0;
    if (discriminant < 0.0) return roots;
    const double root = std::sqrt(discriminant);
    // The stable form of the quadratic formula: computing both roots as (-b +/- sqrt)/2a loses precision
    // catastrophically when b dominates, because the two terms cancel. Forming the larger root from the
    // sum and getting the smaller from the product avoids it.
    const double q = -0.5 * (a1 + (a1 >= 0.0 ? root : -root));
    if (std::fabs(a2) > 1e-12) {
      record(q / a2);
      if (std::fabs(q) > 1e-12) record(a0 / q);
      else record(0.0);
    }
    return roots;
  }

  // Depressed cubic: substitute x = y - a2/(3 a3) to remove the squared term.
  const double p = (3.0 * a3 * a1 - a2 * a2) / (3.0 * a3 * a3);
  const double q = (2.0 * a2 * a2 * a2 - 9.0 * a3 * a2 * a1 + 27.0 * a3 * a3 * a0) / (27.0 * a3 * a3 * a3);
  const double shift = a2 / (3.0 * a3);
  const double discriminant = q * q / 4.0 + p * p * p / 27.0;

  if (discriminant > 0.0) {
    // One real root. Cardano's formula.
    const double root = std::sqrt(discriminant);
    const double u = std::cbrt(-q / 2.0 + root);
    const double v = std::cbrt(-q / 2.0 - root);
    record(u + v - shift);
  } else if (std::fabs(discriminant) < 1e-14) {
    // A repeated root. Two distinct values, one of which is doubled.
    const double u = std::cbrt(-q / 2.0);
    record(2.0 * u - shift);
    record(-u - shift);
  } else {
    // Three real roots. The trigonometric form is used rather than Cardano with complex cube roots,
    // because it stays in real arithmetic and is far better conditioned near this boundary.
    const double radius = std::sqrt(std::max(-p * p * p / 27.0, 0.0));
    const double phi = std::acos(std::clamp(-q / (2.0 * radius), -1.0, 1.0));
    const double magnitude = 2.0 * std::sqrt(std::max(-p / 3.0, 0.0));
    for (int k = 0; k < 3; ++k) {
      record(magnitude * std::cos((phi + 2.0 * kPi * k) / 3.0) - shift);
    }
  }

  std::sort(roots.begin(), roots.end());
  return roots;
}

// ==================================================================================================
// Line against curve
// ==================================================================================================

std::vector<Intersection> intersectLineCurve(const Segment& line, const Segment& curve,
        const double minTolerance) noexcept {
  std::vector<Intersection> found;
  if (!(line.count == 2) || curve.count < 3) return found;

  const DPoint origin = curve.points[0];
  const DPoint direction = line.points[1] - line.points[0];

  // The roots are in the *curve's* parameter u, and the polynomial is the signed distance from the
  // curve to the line expressed as a scalar-valued function of u.
  //
  // A zero of that function is the curve touching the line, which is the intersection being looked for.
  // Solving this way round means the answered question is "where on the curve" -- which is precisely
  // what the caller splits the curve at. Writing the polynomial in the line's parameter instead would
  // answer "where on the line", and recovering the curve's parameter from a point would need a second
  // inversion of the curve.
  //
  // The signed distance is a cross product rather than a Euclidean distance: it is zero exactly where
  // the true distance is, it is a polynomial in u, and it costs no square root.
  //
  // The subtraction of the segment's start point is what moves the line to where the segment actually
  // is. `p.cross(direction)` alone would measure from the line through the *origin*, which coincides
  // with the segment's line only when the segment happens to start at (0,0) -- so every intersection
  // would be offset by the segment's distance from the origin, which reads as a small systematic error
  // rather than as the bug it is.
  const DPoint lineOrigin = line.points[0];
  const auto distanceAt = [&lineOrigin, &direction](const DPoint& p) {
    return (p - lineOrigin).cross(direction);
  };

  // The distance of each control point. These are the *Bernstein* coefficients of the distance function,
  // because the curve's own basis carries through the linear distance operator unchanged -- which is
  // what makes this expansion so short.
  const double d0 = distanceAt(curve.points[0]);
  const double d1 = distanceAt(curve.points[1]);
  const double d2 = distanceAt(curve.points[2]);

  double c0 = 0.0;
  double c1 = 0.0;
  double c2 = 0.0;
  double c3 = 0.0;

  if (curve.count == 3) {
    // d(u) = (1-u)^2 d0 + 2(1-u)u d1 + u^2 d2, expanded into the power basis.
    c0 = d0;
    c1 = 2.0 * (d1 - d0);
    c2 = d0 - 2.0 * d1 + d2;
  } else {
    const double d3 = distanceAt(curve.points[3]);
    // The same expansion for a cubic.
    c0 = d0;
    c1 = 3.0 * (d1 - d0);
    c2 = 3.0 * (d0 - 2.0 * d1 + d2);
    c3 = -d0 + 3.0 * d1 - 3.0 * d2 + d3;
  }

  // Not const: the bounds are accumulated into.
  DRect bounds = line.controlBounds();
  bounds.unionWith(curve.controlBounds());
  // The floor is what stops a root that is only reachable by coincidence from being reported as an intersection.
  //
  // The closed form solves exactly, so a curve that passes within a few units of a line has *no* real root and
  // reports nothing. But a curve whose closest approach is a few times 1e-16 does have one, and finding it says
  // nothing about whether the shapes meet: two curves can be that close over a stretch without touching, and a
  // boolean operation that put a crossing there would produce a vertex in mid-air. A caller that folded this
  // solver into a curve-against-curve loop supplies a floor; a direct caller leaves it at zero and gets the exact
  // answer it asked for.
  const double tolerance = std::max(toleranceFor(bounds), minTolerance);

  for (const double u : solveUnitCubic(c0, c1, c2, c3)) {
    const DPoint point = curve.eval(u);

    // Now the line's parameter, from projecting the point onto the line. Whichever axis is better
    // conditioned is used: taking x always would divide by a near-zero component for a vertical line and
    // lose all precision, which is exactly the geometry of a nearly-vertical line crossing a curve.
    double t = 0.0;
    if (std::fabs(direction.x) >= std::fabs(direction.y)) {
      if (std::fabs(direction.x) < 1e-12) continue;
      t = (point.x - line.points[0].x) / direction.x;
    } else {
      if (std::fabs(direction.y) < 1e-12) continue;
      t = (point.y - line.points[0].y) / direction.y;
    }

    if (!inUnitRange(t)) continue;
    t = clamp01(t);

    Intersection hit;
    hit.t = t;
    hit.u = clamp01(u);
    hit.point = point;
    hit.location = classifyLocation(hit.t, hit.u);
    hit.isTangent = isTangentAt(line, curve, hit.t, hit.u);
    found.push_back(hit);
  }

  std::sort(found.begin(), found.end(),
          [](const Intersection& lhs, const Intersection& rhs) { return lhs.t < rhs.t; });
  removeDuplicates(found, tolerance);
  return found;
}

// ==================================================================================================
// Entry point
// ==================================================================================================

std::vector<Intersection> intersect(const Segment& a, const Segment& b) noexcept {
  std::vector<Intersection> found;

  DRect bounds = a.controlBounds();
  bounds.unionWith(b.controlBounds());
  const double tolerance = toleranceFor(bounds);

  if (a.isLine() && b.isLine()) {
    // An exact solve, used in preference to subdivision because two lines are the case a flattened path
    // is made of, and because subdivision would round a crossing that can be computed exactly.
    const DPoint da = a.points[1] - a.points[0];
    const DPoint db = b.points[1] - b.points[0];
    const double denominator = da.cross(db);
    if (std::fabs(denominator) < 1e-15) {
      // Parallel: either no intersection or a collinear overlap. An overlap is not reported here --
      // it has no single parameter and the caller handles collinear runs separately.
      return found;
    }

    const DPoint delta = b.points[0] - a.points[0];
    const double t = delta.cross(db) / denominator;
    const double u = delta.cross(da) / denominator;
    if (!inUnitRange(t) || !inUnitRange(u)) return found;

    Intersection hit;
    hit.t = clamp01(t);
    hit.u = clamp01(u);
    hit.point = a.eval(hit.t);
    hit.location = classifyLocation(hit.t, hit.u);
    hit.isTangent = false;  // two non-parallel lines always cross
    found.push_back(hit);
    return found;
  }

  // A line against a curve has a closed form, so it is taken before the subdivision path.
  if (a.isLine() && !b.isLine()) return intersectLineCurve(a, b);
  if (b.isLine() && !a.isLine()) {
    // Solved in the other orientation and the parameters swapped, so that only one closed-form routine
    // has to exist. Swapping them here is cheaper than maintaining the mirror image of that code.
    auto swapped = intersectLineCurve(b, a);
    for (Intersection& hit: swapped) {
      std::swap(hit.t, hit.u);
      hit.location = classifyLocation(hit.t, hit.u);
    }
    return swapped;
  }

  // A segment against itself is everything shared and nothing crossed.
  //
  // Every point is common, so "where do these meet" has no answer a caller can use, and running the
  // subdivision produces one hit per surviving leaf pair -- a large, arbitrary number that depends on
  // the recursion rather than on the geometry. Reporting none is the honest answer: the traversal that
  // calls this exists to find where two *different* edges cross, and an edge does not cross itself.
  if (a.count == b.count) {
    bool identical = true;
    for (uint8_t i = 0; i < a.count; ++i) {
      if (!(a.points[i] == b.points[i])) {
        identical = false;
        break;
      }
    }
    if (identical) return found;
  }

  // Two curves, neither of them a line.
  //
  // ## Flatten one, then solve line-against-curve
  //
  // The obvious approach -- subdivide both curves and see which small pieces overlap -- is what three earlier
  // attempts here did, and none of them produced a correct answer on a pair of quadratics verified to cross twice.
  // Rather than write a fourth, this reuses the piece that *is* verified: `intersectLineCurve`, which solves a line
  // against a quadratic or cubic in closed form and then polishes the result with Newton. Flatten one of the two
  // curves into short line segments and ask that solver where each segment meets the other curve.
  //
  // That division of labour is what makes the result trustworthy. The flattening is the same code the rasterizer
  // uses, so its error is the tolerance it was given and nothing else; the solving is the code already pinned
  // against roots constructed in advance; and the only new logic is the loop that walks the segments and merges
  // their answers.
  //
  // ## Why the segments are short enough
  //
  // `intersectLineCurve` finds every crossing of the *whole* other curve with the line each segment lies on, and
  // rejects the ones outside the segment. A segment that is nearly flat therefore reports one crossing per genuine
  // intersection, and a segment that spans a large turn could report several -- which the deduplication below
  // handles. The flattening tolerance is what bounds that, and it is the same quarter-unit the rest of this file
  // works to.
  const Segment& flattened = a;
  const Segment& other = b;

  // The number of pieces to cut `flattened` into. Chosen from the tolerance the caller's geometry implies rather
  // than from a fixed count: too few and a segment spanning a sharp turn reports a crossing twice, too many and the
  // loop does work for a precision nothing downstream can use.
  const Scalar extent = std::max({ flattened.controlBounds().width(), flattened.controlBounds().height(),
    other.controlBounds().width(), other.controlBounds().height() });
  const Scalar scale = extent > 0.0 ? extent : 1.0;
  const Scalar step = std::max(tolerance * 8.0, scale * 1e-4);

  // The arc length is bounded by the control polygon, so this is an upper bound on how many pieces are needed and
  // never too few. Capped so that a huge coordinate range cannot turn this into an unbounded loop.
  const double polygonLength = (flattened.count >= 2)
          ? [&flattened] {
              double total = 0.0;
              for (uint8_t i = 0; i + 1 < flattened.count; ++i) {
                total += (flattened.points[i + 1] - flattened.points[i]).length();
              }
              return total;
            }()
          : 0.0;
  const int pieceCount = std::clamp(static_cast<int>(std::ceil(polygonLength / step)), 8, 4096);

  for (int i = 0; i < pieceCount; ++i) {
    const double t0 = static_cast<double>(i) / pieceCount;
    const double t1 = static_cast<double>(i + 1) / pieceCount;

    // The piece as a line between its two endpoints. The endpoints lie exactly on the curve, so the line
    // under-approximates the curve by at most the sagitta of the piece -- which is why the piece count is derived
    // from the length rather than fixed.
    const Segment piece = Segment::line(flattened.eval(t0), flattened.eval(t1));

    // The floor is the sagitta of one piece: two curves closer together than that are indistinguishable at the
    // precision this loop works to, so a root that fine is an artefact of the flattening rather than a crossing.
    for (const Intersection& hit : intersectLineCurve(piece, other, step * 0.25)) {
      // The line's parameter is local to the piece, so it is mapped back to the whole curve. This is the step that
      // has to be right: a local parameter reported as a global one puts every intersection at the wrong distance
      // along the first curve, which is the kind of error that produces a shape that looks almost right.
      Intersection mapped = hit;
      mapped.t = t0 + hit.t * (t1 - t0);
      // The point and the second curve's parameter need no adjustment: the solver reported them against the real
      // second curve, and the point is on it.
      mapped.point = other.eval(hit.u);
      mapped.location = classifyLocation(mapped.t, mapped.u);
      mapped.isTangent = isTangentAt(flattened, other, mapped.t, mapped.u);
      found.push_back(mapped);
    }
  }

  std::sort(found.begin(), found.end(),
          [](const Intersection& lhs, const Intersection& rhs) { return lhs.t < rhs.t; });

  // Deduplicated because neighbouring pieces share an endpoint, so a crossing exactly at a piece boundary is found
  // by both. The parameters are the identity that matters: they are what the caller splits at.
  {
    std::vector<Intersection> unique;
    unique.reserve(found.size());
    for (const Intersection& candidate : found) {
      bool duplicate = false;
      for (const Intersection& kept : unique) {
        if (std::fabs(candidate.t - kept.t) <= 1e-6 && std::fabs(candidate.u - kept.u) <= 1e-6) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) unique.push_back(candidate);
    }
    found = std::move(unique);
  }

  return found;
}

} // namespace graphics::pathops
