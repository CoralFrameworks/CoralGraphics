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

#include "pathops/pathops.h"

#include "Intersection.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <utility>
#include <vector>

/**
 * @file Boolean.cpp
 *
 * The set operations, built on the intersection engine.
 *
 * ## The shape of the algorithm
 *
 * Four steps, and each one is separately checkable:
 *
 *  1. **Collect** every edge of both paths as a curve segment with an explicit start and end.
 *  2. **Split** each edge at every parameter where it meets an edge of the other path, so that no two pieces in
 *     the result cross.
 *  3. **Classify** each piece by asking whether its midpoint is inside the other shape.
 *  4. **Stitch** the pieces the operation keeps back into closed contours.
 *
 * ## Why classification is by midpoint rather than by traversal
 *
 * A production implementation walks the split edges in order, carrying a winding count from one edge to the next,
 * and decides each edge's fate from the count on either side. That is faster and it is exact. It is also where the
 * subtle bugs live: a mistake in the ordering rule puts a hole where there should be a fill, and the result still
 * looks like a shape.
 *
 * Asking "is this piece's midpoint inside the other path" is slower -- one ray cast per piece -- and it is far
 * harder to get wrong, because the answer comes from the *original* geometry rather than from the accumulated state
 * of a walk. Where the two disagree it is the walk that is wrong. The cost is bounded: the pieces are few, and a
 * boolean operation is not a per-frame operation.
 *
 * ## What the split guarantees
 *
 * After step 2 no piece crosses another path's edges, but a piece's midpoint can still lie exactly on the other
 * path -- when two edges overlap along a stretch. That case is handled by ignoring horizontal edges in the ray cast
 * and by treating the boundary as inside, which is the convention that makes a union of two shapes sharing an edge
 * come out with one shared edge rather than two coincident ones.
 */

namespace graphics::pathops {
namespace {

/** How far a parameter must be from an edge's end before it is worth splitting there. */
constexpr double kSplitEpsilon = 1e-9;

/** How close two points must be to be the same vertex. Relative to the coordinates, like every other tolerance. */
double vertexTolerance(const DRect& bounds) {
  const double magnitude = std::max({ std::fabs(bounds.left), std::fabs(bounds.right), std::fabs(bounds.top),
    std::fabs(bounds.bottom), 1.0 });
  return magnitude * 1e-9;
}

/**
 * @return how far apart two endpoints may be and still be the same vertex.
 *
 * Much looser than `vertexTolerance`, and deliberately so. An intersection between two *curves* is a point that both
 * approximate: `intersect` computes it from one of the two curves, so the two callers of it for a single crossing get
 * points that differ by whatever the flattening error is -- parts in 10^6 on the circles this was found with, which is
 * three orders of magnitude coarser than the arithmetic's own noise.
 *
 * Treating those as different vertices leaves a contour that cannot close: four pieces that should form a lens instead
 * form three separate chains, and `op` reports failure. The tolerance has to cover the geometry's own approximation
 * rather than only its rounding, so it is derived from the size of the shapes involved and set well below any feature
 * a caller could be resolving -- a millionth of the extent, against features that are fractions of it.
 */
double snapTolerance(const DRect& bounds) {
  const double extent = std::max({ bounds.width(), bounds.height(), 1.0 });
  return extent * 1e-6;
}

/**
 * One edge of the arrangement: a curve segment with the points it runs between.
 *
 * The endpoints are stored rather than derived from the segment, because splitting has to produce pieces whose ends
 * agree exactly. Deriving them by evaluating the curve twice would give values that differ in the last bit, and two
 * vertices that are "the same" but not equal are a graph with two nodes where it needs one -- which shows up as a
 * contour that will not close.
 */
struct Edge {
  /**
   * The edge's control polygon, **including both endpoints**.
   *
   * The polygon is stored rather than a `Segment` because `Segment` is built from a start point and a *delta* to each
   * control point, whereas `PathSegment` stores absolute control points without the start at all. Rebuilding a piece
   * after a split needs an absolute polygon, and deriving it once here is what keeps that reconstruction honest.
   *
   * This distinction is not academic. The first version of this file passed the path's start point to a helper that
   * built a `Segment` by treating the path segment's control points as deltas -- so for a line it produced an edge from
   * the *previous* segment's endpoint. Every straight edge in the arrangement ran between the wrong pair of points, and
   * the only reason it was not obvious is that `op` between two shapes re-derived its pieces from intersections, which
   * happened to land on the right points. `simplify` had no such luck, and neither did any case where a shape's own
   * vertices mattered.
   */
  std::vector<DPoint> polygon;

  /** Which operand this edge came from: 0 for the first path, 1 for the second. */
  int source = 0;

  /** True when this edge is traversed against its own direction. */
  bool reversed = false;

  /** @return the points the edge runs between, as stored. */
  [[nodiscard]] DPoint start() const noexcept { return polygon.front(); }
  [[nodiscard]] DPoint end() const noexcept { return polygon.back(); }

  /** @return how many points the control polygon has: 2 for a line, 3 for a quad, 4 for a cubic. */
  [[nodiscard]] size_t degree() const noexcept { return polygon.size(); }

  /** @return the point at @p t, evaluated on the control polygon. */
  [[nodiscard]] DPoint eval(double t) const noexcept;

  /** @return the derivative at @p t, evaluated on the control polygon. */
  [[nodiscard]] DPoint derivativeAt(double t) const noexcept;

  /** @return the point half way along, which is where the inside test is made. */
  [[nodiscard]] DPoint midpoint() const noexcept { return eval(0.5); }

  /** @return the piece of this edge between @p t0 and @p t1, with matching endpoints. */
  [[nodiscard]] Edge subedge(double t0, double t1) const;

  /** @return the direction of travel at @p t, unnormalised. */
  [[nodiscard]] DPoint tangentAt(double t) const noexcept { return derivativeAt(t); }
};

/**
 * @return @p edge's geometry as a `Segment`, which is the form the intersection engine takes.
 *
 * The arrangement stores its own control polygon (see `Edge::polygon`); the engine takes a start point plus deltas, so
 * this is where the two representations meet. Keeping the conversion in one place is the point: exchanging them
 * accidentally is what made every straight edge run between the wrong points.
 */
Segment toSegment(const Edge& edge) {
  const auto& p = edge.polygon;
  switch (p.size()) {
    case 2: return Segment::line(p[0], p[1]);
    case 3: return Segment::quad(p[0], p[1], p[2]);
    case 4: return Segment::cubic(p[0], p[1], p[2], p[3]);
    default: return Segment::line(p.front(), p.back());
  }
}

/** @return the curve through @p points at parameter @p t, by the de Casteljau recurrence. */
DPoint evalPolygon(const std::vector<DPoint>& points, const double t) {
  std::vector<DPoint> work = points;
  for (size_t n = work.size(); n > 1; --n) {
    for (size_t i = 0; i + 1 < n; ++i) {
      work[i] = DPoint { work[i].x + (work[i + 1].x - work[i].x) * t, work[i].y + (work[i + 1].y - work[i].y) * t };
    }
  }
  return work.front();
}

DPoint Edge::eval(const double t) const noexcept { return evalPolygon(polygon, t); }

DPoint Edge::derivativeAt(const double t) const noexcept {
  // The derivative of a Bezier of degree n is n times the Bezier of degree n-1 over the control *differences*.
  if (polygon.size() < 2) return DPoint { 0.0, 0.0 };
  std::vector<DPoint> differences;
  differences.reserve(polygon.size() - 1);
  for (size_t i = 0; i + 1 < polygon.size(); ++i) {
    differences.push_back(
            DPoint { polygon[i + 1].x - polygon[i].x, polygon[i + 1].y - polygon[i].y });
  }
  return evalPolygon(differences, t) * static_cast<double>(polygon.size() - 1);
}

Edge Edge::subedge(const double t0, const double t1) const {
  if (polygon.size() < 2 || !(t1 > t0)) {
    Edge copy = *this;
    copy.polygon.assign(polygon.size(), eval(t0));
    return copy;
  }

  // ## De Casteljau's triangle, and which column is which
  //
  // Splitting at @p t0 gives a triangle of intermediate points, and two of its columns are the two halves of the curve:
  //
  //  - The **low** column -- each level's *last* point, read back to front -- is the half over [t0, 1].
  //  - The **high** column -- each level's *first* point, in order -- is the half over [0, t0].
  //
  // Both orderings are easy to get wrong, and the symptoms are quiet rather than loud: the wrong column still starts and
  // ends at the right points, because the first and last of the triangle are shared. What differs is the middle, so the
  // piece is a different curve with the same endpoints -- a contour that closes, has the right vertex count, and
  // encloses the wrong area. Measured against the arc it came from, a column in the wrong order is off by several units
  // in the middle and exact at the ends, which is exactly how it was found.
  //
  // The [t0, t1] piece is the [0, u] part of the low column, where u reparameterises t1 into it. Splitting the low
  // column again and taking *its* high column gives that.
  const auto triangleAt = [this](const std::vector<DPoint>& points, const double s) {
    std::vector<std::vector<DPoint>> levels;
    levels.push_back(points);
    while (levels.back().size() > 1) {
      const std::vector<DPoint>& previous = levels.back();
      std::vector<DPoint> next;
      next.reserve(previous.size() - 1);
      for (size_t i = 0; i + 1 < previous.size(); ++i) {
        next.push_back(DPoint { previous[i].x + (previous[i + 1].x - previous[i].x) * s,
          previous[i].y + (previous[i + 1].y - previous[i].y) * s });
      }
      levels.push_back(std::move(next));
    }
    return levels;
  };

  const auto lowColumn = [](const std::vector<std::vector<DPoint>>& levels) {
    std::vector<DPoint> out;
    out.reserve(levels.size());
    for (auto it = levels.rbegin(); it != levels.rend(); ++it) out.push_back(it->back());
    return out;
  };

  const auto highColumn = [](const std::vector<std::vector<DPoint>>& levels) {
    std::vector<DPoint> out;
    out.reserve(levels.size());
    for (const std::vector<DPoint>& level: levels) out.push_back(level.front());
    return out;
  };

  const std::vector<DPoint> low = lowColumn(triangleAt(polygon, t0));

  Edge piece = *this;
  if (t1 >= 1.0) {
    piece.polygon = low;
  } else {
    const double u = (t1 - t0) / (1.0 - t0);
    piece.polygon = highColumn(triangleAt(low, u));
  }

  // The endpoints are set from the original curve, so that a chain of pieces shares its vertices bit for bit.
  piece.polygon.front() = eval(t0);
  piece.polygon.back() = eval(t1);
  return piece;
}

/**
 * @return every edge of @p path, with an explicit control polygon and no closing verb.
 *
 * A `close` becomes a line back to the contour's start, because the arrangement has to know about that edge: it is real
 * geometry that other edges can cross, and a boolean operation that ignored it would leave the closing edge unsplit.
 *
 * The polygon is built with the cursor as its first point, because a `PathSegment` stores its control points as
 * absolutes and does not carry the start at all. Getting that wrong is what made every straight edge run between the
 * wrong pair of points; see the note on `Edge::polygon`.
 */
std::vector<Edge> collectEdges(const Path& path, const int source) {
  std::vector<Edge> edges;

  DPoint contourStart { 0.0, 0.0 };
  DPoint cursor { 0.0, 0.0 };
  bool open = false;

  const auto add = [&edges, source](std::vector<DPoint> polygon) {
    // A zero-length edge has no direction and cannot be split usefully; keeping it would add a vertex of degree zero to
    // the graph, which is a contour that never closes.
    if ((polygon.back() - polygon.front()).lengthSquared() <= 0.0) return;
    Edge edge;
    edge.polygon = std::move(polygon);
    edge.source = source;
    edges.push_back(std::move(edge));
  };

  for (const PathSegment& pathSegment: path.segments()) {
    switch (pathSegment.verb) {
      case Verb::Move:
        contourStart = pathSegment.points[0];
        cursor = contourStart;
        open = true;
        break;

      case Verb::Line:
        if (!open) break;
        add({ cursor, pathSegment.points[0] });
        cursor = pathSegment.points[0];
        break;

      case Verb::Quad:
        if (!open) break;
        add({ cursor, pathSegment.points[0], pathSegment.points[1] });
        cursor = pathSegment.points[1];
        break;

      case Verb::Cubic:
        if (!open) break;
        add({ cursor, pathSegment.points[0], pathSegment.points[1], pathSegment.points[2] });
        cursor = pathSegment.points[2];
        break;

      case Verb::Close:
        if (!open) break;
        // The closing edge, as an explicit line. Skipped when the contour is already back at its start.
        add({ cursor, contourStart });
        cursor = contourStart;
        break;

      case Verb::Done:
        break;
    }
  }

  return edges;
}

/**
 * @return the number of times a ray cast in +x from @p point crosses the curve, and the signed winding.
 *
 * ## Why this does not go through the intersection engine
 *
 * It looks like it should: intersect a horizontal ray with the curve and count the results. It cannot, for two
 * reasons. The engine's line-against-curve solve is driven by Newton refinement from a starting guess, which
 * converges to whichever crossing is nearest and can miss one entirely; and the classification loop calls this
 * thousands of times, so a search per call would dominate the cost of the whole operation.
 *
 * Solving the polynomial directly is exact and cheap. The roots are the curve's parameters where it crosses the
 * ray's height, which is the same cubic `solveUnitCubic` already handles for the line-against-curve case -- so this
 * is the same arithmetic, just asked a different question.
 *
 * A near-horizontal stretch is skipped: it runs along the ray rather than across it, and counting it would either
 * double a crossing or invent one. The test is on the curve's own control hull, so it is a statement about the
 * geometry rather than about a sampled approximation of it.
 */
struct RayResult {
  int crossings = 0;
  int winding = 0;
};

RayResult countRayCrossings(const Edge& edge, const DPoint& point) {
  RayResult result;

  // The curve's height above the ray, as a cubic in its own parameter. This is the distance function the
  // line-against-curve solve builds, with the ray's origin as the reference point.
  const double rayY = point.y;

  const auto heightAt = [rayY](const DPoint& p) { return p.y - rayY; };

  const auto& polygon = edge.polygon;
  const double d0 = heightAt(polygon.front());
  const double d1 = polygon.size() >= 2 ? heightAt(polygon[1]) : d0;
  const double d2 = polygon.size() >= 3 ? heightAt(polygon[2]) : d1;
  const double d3 = polygon.size() >= 4 ? heightAt(polygon[3]) : d2;

  // A curve whose whole height range is tiny runs along the ray, not across it.
  const double heightSpan = std::max({ std::fabs(d0), std::fabs(d1), std::fabs(d2), std::fabs(d3) });
  const double scale = std::max(1.0, std::max(std::fabs(edge.start().y), std::fabs(edge.end().y)));
  if (heightSpan < scale * 1e-12) return result;

  // The curve's height above the ray as a polynomial in its own parameter, converted from the Bernstein basis by the
  // usual binomial expansion. A line is degree one, a quad degree two, a cubic degree three.
  double c0 = 0.0;
  double c1 = 0.0;
  double c2 = 0.0;
  double c3 = 0.0;

  switch (polygon.size()) {
    case 2:
      c0 = d0;
      c1 = d1 - d0;
      break;
    case 3:
      c0 = d0;
      c1 = 2.0 * (d1 - d0);
      c2 = d0 - 2.0 * d1 + d2;
      break;
    default:
      c0 = d0;
      c1 = 3.0 * (d1 - d0);
      c2 = 3.0 * (d0 - 2.0 * d1 + d2);
      c3 = -d0 + 3.0 * d1 - 3.0 * d2 + d3;
      break;
  }

  for (const double t: solveUnitCubic(c0, c1, c2, c3)) {
    // ## Endpoint roots are discarded
    //
    // The polynomial is solved over the whole parameter range, and a curve whose endpoint happens to sit exactly at the
    // ray's height has a root there. Counting it is wrong: the adjacent edge shares that vertex, so a crossing at it
    // would be counted twice, and the vertex would appear to cross a ray that only touches it.
    //
    // This is what made `simplify` fail on a plain square. The east edge runs from (4,0) to (4,4), and a ray at height
    // 2 from inside the square meets it at its *end* rather than in its interior, so the root came back at exactly
    // t = 1. Counting it gave a winding of two where the geometry has one, an even crossing count read as outside under
    // the even-odd rule, and the edge was dropped -- leaving three edges of four and a contour that could not close.
    //
    // Roots are dropped at t = 1 and *kept* at t = 0, which is the half-open interval a crossing count needs: a vertex
    // is then attributed to the edge that starts there and not to the one that ends there, so it is counted once
    // however many edges meet at it.
    if (t >= 1.0 - 1e-9) continue;

    const DPoint at = edge.eval(t);
    // Only crossings strictly to the right of the point count. A crossing exactly at the point's x is on the
    // boundary, and the caller's convention is that the boundary is inside.
    if (at.x <= point.x) continue;
    ++result.crossings;

    // The direction matters for the nonzero rule: an edge going up contributes +1 and one going down -1.
    const double dy = edge.derivativeAt(t).y;
    if (dy > 0.0) {
      ++result.winding;
    } else if (dy < 0.0) {
      --result.winding;
    }
  }

  return result;
}

/** @return the winding number of @p edges at @p point, under @p rule. */
/**
 * @return whether @p point is inside the result of applying @p setOp to the two shapes.
 *
 * The operations are defined on *points*, not on edges, which is what makes the classification below simple and
 * complete: a point is in the union when it is in either shape, in the intersection when it is in both, and so on.
 */
bool inResult(const PathOp setOp, const bool inOne, const bool inTwo) {
  switch (setOp) {
    case PathOp::Union: return inOne || inTwo;
    case PathOp::Intersection: return inOne && inTwo;
    case PathOp::Difference: return inOne && !inTwo;
    case PathOp::ReverseDifference: return !inOne && inTwo;
    case PathOp::ExclusiveOr: return inOne != inTwo;
  }
  return false;
}

int windingAt(const std::vector<Edge>& edges, const DPoint& point, const FillRule rule) {
  int winding = 0;
  int crossings = 0;
  for (const Edge& edge: edges) {
    const RayResult ray = countRayCrossings(edge, point);
    winding += ray.winding;
    crossings += ray.crossings;
  }
  return rule == FillRule::EvenOdd ? (crossings & 1) : winding;
}

/** @return true when @p point is inside the shape @p edges describes, under @p rule. */
bool isInside(const std::vector<Edge>& edges, const DPoint& point, const FillRule rule) {
  return windingAt(edges, point, rule) != 0;
}

/**
 * @return true when @p point lies on the boundary of @p edges, to within @p tolerance.
 *
 * ## Why the boundary case cannot be folded into "inside"
 *
 * The set operations have three cases per piece, not two, and the third only shows up when the two shapes share an
 * edge. A piece lying *along* the other shape's boundary is neither in its interior nor in its exterior, and which
 * answer it needs depends on the operation:
 *
 *  - For a **union**, it is on the boundary of the result and must be kept.
 *  - For an **intersection**, it is likewise on the result's boundary and must be kept.
 *  - For an **exclusive or**, a shared edge is in exactly one of the two shapes on either side (in one, out of the
 *    other), so it is on the result's boundary and must be kept.
 *  - For a **difference**, a shared edge is in the first shape and not the second on one side and in both on the
 *    other, so it contributes nothing and must be dropped.
 *
 * Treating a coincident piece as "inside" -- which the winding test reports, since a boundary has no winding of its
 * own -- gives the right answer for a difference and the wrong one for the other three. That is why an exclusive or
 * of two unit squares sharing a corner came out identical to their union before this test existed.
 */
/**
 * @return whether @p point is in the result, testing a ring of points around it.
 *
 * ## Why a ring rather than a single point
 *
 * A sample can land exactly on a boundary -- on the other shape's outline, or on the piece being classified -- and
 * there the answer is genuinely ambiguous, because a boundary has no winding of its own. A point that close to a
 * boundary is not a point in a region at all, so the probe samples a ring of offsets and takes the majority.
 *
 * This is what the classification was missing. The earlier version asked the winding test at a single point, and a
 * point on a boundary has no winding of its own -- so a piece running *along* the other shape's edge was reported as
 * outside. That is right for a union and for a difference and wrong for an exclusive or.
 */
bool regionContains(const PathOp setOp, const std::vector<Edge>& edgesOne, const FillRule ruleOne,
        const std::vector<Edge>& edgesTwo, const FillRule ruleTwo, const DPoint& point, const double radius) {
  int inside = 0;
  int outside = 0;
  // Eight directions, so that a boundary running along either axis is stepped off in the other.
  for (int i = 0; i < 8; ++i) {
    const double angle = static_cast<double>(i) * 3.14159265358979 / 4.0;
    const DPoint probe { point.x + std::cos(angle) * radius, point.y + std::sin(angle) * radius };
    const bool inOne = isInside(edgesOne, probe, ruleOne);
    const bool inTwo = isInside(edgesTwo, probe, ruleTwo);
    if (inResult(setOp, inOne, inTwo)) {
      ++inside;
    } else {
      ++outside;
    }
  }
  return inside > outside;
}

bool isOnBoundary(const std::vector<Edge>& edges, const DPoint& point, const double tolerance) {
  for (const Edge& edge: edges) {
    // Tested against the edge's *control polygon*, which contains the curve. So a point that is not within tolerance
    // of the polygon is certainly not on the curve, and a point that is gets examined further -- the test errs toward
    // reporting coincidence, and the two cases are told apart below.
    //
    // An earlier version sampled the curve at five parameters and compared distances. That is both slower and, at
    // five samples, wrong: an edge running from (2,1) to (2,2) sampled at quarters lands on y = 1, 1.25, 1.5, 1.75, 2,
    // and a midpoint at y = 1.5 is one of them -- but an edge from (1,1) to (2,1) sampled at quarters gives
    // x = 1, 1.25, 1.5, 1.75, 2, and the same midpoint at (2, 1.5) is not. Which of the two neighbouring edges
    // happens to catch a given point depends on the sampling, so the answer was arbitrary.
    bool nearPolygon = false;
    for (size_t i = 0; i + 1 < edge.polygon.size() && !nearPolygon; ++i) {
      const DPoint& a = edge.polygon[i];
      const DPoint& b = edge.polygon[i + 1];
      const DPoint ab = b - a;
      const double lengthSquared = ab.lengthSquared();
      if (!(lengthSquared > 0.0)) {
        nearPolygon = (point - a).lengthSquared() <= tolerance * tolerance;
        continue;
      }
      // The projection parameter, clamped to the segment, and the distance to that clamped point.
      double s = (point - a).dot(ab) / lengthSquared;
      s = std::clamp(s, 0.0, 1.0);
      const DPoint closest { a.x + ab.x * s, a.y + ab.y * s };
      nearPolygon = (point - closest).lengthSquared() <= tolerance * tolerance;
    }

    if (!nearPolygon) continue;

    // Within tolerance of the polygon. For a straight edge that is the answer, because the polygon is the curve. For a
    // curved one the point may be inside the hull but off the curve, so it is refined: the closest point on the actual
    // curve is found by sampling finely and then tested properly.
    if (edge.polygon.size() == 2) return true;

    double bestSquared = std::numeric_limits<double>::max();
    constexpr int kSamples = 64;
    for (int i = 0; i <= kSamples; ++i) {
      const DPoint at = edge.eval(static_cast<double>(i) / kSamples);
      bestSquared = std::min(bestSquared, (at - point).lengthSquared());
    }
    if (bestSquared <= tolerance * tolerance) return true;
  }
  return false;
}

/**
 * @return @p edges with every edge split at each parameter where it meets an edge of the other set.
 *
 * The split points are collected first and applied afterwards, so that a single edge crossed by three others
 * produces four pieces in one pass rather than being resplit each time. Resplitting would work but would compound
 * the rounding at every stage.
 *
 * The pieces are built so that each one ends exactly where the next begins: the start of a piece is the previous
 * piece's stored end, and the end is the curve evaluated at the split parameter. Evaluating twice would give two
 * values differing in the last bit, and the stitching step needs them equal.
 */
/**
 * @return true when the two pieces run along the same stretch of curve, in either direction.
 *
 * Two pieces are coincident when they have the same endpoints, allowing for which way round they are stored. Only
 * endpoints are compared, which is exact for straight pieces -- the case that arises whenever two shapes share an edge
 * -- and is a sound approximation for curves, because the split step already cut both at every crossing, so a pair
 * that shares both ends and comes from the same geometry lies on the same curve.
 */
bool coincident(const Edge& a, const Edge& b, const double tolerance) {
  const double squared = tolerance * tolerance;
  const bool sameWay =
          (a.start() - b.start()).lengthSquared() <= squared && (a.end() - b.end()).lengthSquared() <= squared;
  const bool opposed =
          (a.start() - b.end()).lengthSquared() <= squared && (a.end() - b.start()).lengthSquared() <= squared;
  return sameWay || opposed;
}

/**
 * @return true when the operation's answer differs on the two sides of @p piece.
 *
 * The sides are sampled at a distance chosen from the piece's own length rather than from the geometry as a whole. That
 * matters more than it looks: a coincident stretch is often far shorter than the shapes it belongs to, and a probe
 * sized for the shapes steps straight over the stretch and lands in the region on the far side of it, where the answer
 * is different for reasons that have nothing to do with this piece.
 *
 * The distance is a fraction of the piece, and the ring test inside `regionContains` handles the rest -- the ring is
 * what keeps a sample that lands on a boundary from deciding the answer on its own.
 */
bool boundaryOfResult(const Edge& piece, const PathOp setOp, const std::vector<Edge>& edgesOne,
        const FillRule ruleOne, const std::vector<Edge>& edgesTwo, const FillRule ruleTwo, const double tolerance) {
  const DPoint tangent = piece.derivativeAt(0.5);
  const double magnitude = tangent.length();
  if (!(magnitude > 0.0)) return false;

  // A third of the piece's own length, floored so that a very short piece still steps clear of the arithmetic, and
  // capped so that a piece spanning a whole shape does not reach out of the region it borders.
  const double length = (piece.end() - piece.start()).length();
  const double probe = std::clamp(length / 3.0, tolerance * 64.0, std::max(length * 0.5, tolerance * 64.0));

  const DPoint middle = piece.midpoint();
  // The winding-positive side of a tangent (tx, ty) is (-ty, tx); see the note on `RayResult`.
  const DPoint offset { -tangent.y / magnitude * probe, tangent.x / magnitude * probe };

  const DPoint positive { middle.x + offset.x, middle.y + offset.y };
  const DPoint negative { middle.x - offset.x, middle.y - offset.y };

  const bool positiveIn = regionContains(setOp, edgesOne, ruleOne, edgesTwo, ruleTwo, positive, probe);
  const bool negativeIn = regionContains(setOp, edgesOne, ruleOne, edgesTwo, ruleTwo, negative, probe);
  return positiveIn != negativeIn;
}

std::vector<Edge> splitEdges(const std::vector<Edge>& edges, const std::vector<Edge>& others) {
  std::vector<Edge> result;
  result.reserve(edges.size() * 2);

  for (const Edge& edge: edges) {
    std::vector<double> cuts;

    for (const Edge& other: others) {
      for (const Intersection& hit: intersect(toSegment(edge), toSegment(other))) {
        // The parameter on this curve is the one that would split it -- but only if the intersection is actually *on*
        // the other one.
        //
        // `intersect` works on curves, not on parameter ranges, so for two cubics it reports every crossing of the
        // whole of one against the whole of the other. A crossing that lies outside this edge's own extent is still a
        // crossing of the geometry, and its `t` is a perfectly good parameter on this edge -- so splitting there
        // produces a vertex in the middle of a piece where nothing meets, and the contour cannot close.
        //
        // That is not a rounding error and it does not show up on straight edges, where a crossing outside the extent
        // comes back with a parameter outside [0,1] and the test below discards it anyway. On a curved piece the
        // parameter is inside the range while the *point* is not, so the two ends of an intersection disagree by parts
        // in 10^6 -- which is how this was found: two pieces that should have shared an endpoint printed the same to
        // four decimals and were three millionths apart.
        if (!(hit.u >= -kSplitEpsilon && hit.u <= 1.0 + kSplitEpsilon)) continue;
        const double t = hit.t;
        if (t > kSplitEpsilon && t < 1.0 - kSplitEpsilon) cuts.push_back(t);
      }
    }

    if (cuts.empty()) {
      result.push_back(edge);
      continue;
    }

    std::sort(cuts.begin(), cuts.end());

    DPoint cursor = edge.start();
    double previous = 0.0;
    for (const double cut: cuts) {
      // Duplicate parameters come from an edge meeting two others at the same place, which is a vertex of degree
      // three rather than two splits.
      if (cut - previous <= kSplitEpsilon) continue;

      Edge piece = edge.subedge(previous, cut);
      result.push_back(piece);

      cursor = piece.end();
      previous = cut;
    }

    // The final piece, from the last cut to the edge's own end. Its end is the original edge's end rather than a
    // fresh evaluation, which is what keeps the last piece joined to whatever followed the original.
    if (1.0 - previous > kSplitEpsilon) {
      result.push_back(edge.subedge(previous, 1.0));
    }
  }

  return result;
}

/** @return the bounding rectangle of every endpoint in @p pieces. */

/** A kept piece, ready to be stitched. */
struct Segment2 {
  Edge edge;
  bool reverse = false;
};

/**
 * @return the closed contours formed by @p pieces, or an empty path when they do not close.
 *
 * ## Reversal
 *
 * A piece may be marked reversed, which is what makes a subtracted shape become the wall of a hole rather than a
 * second solid region. Its *vertices have to be swapped to match*: the walk follows a piece from where it starts to
 * where it ends, and a reversed piece starts at what the edge stores as its end. Treating a reversed piece as
 * running forwards makes it a loop from one vertex back to itself, which has no continuation and silently leaves
 * every contour open -- the result is empty and the pieces look perfectly correct in a trace.
 *
 * ## The walk
 *
 * Each piece runs from one vertex to another. The walk starts at an unused piece, then repeatedly moves to a piece
 * beginning at the current vertex. Where several qualify the choice is the one that turns least far to the left,
 * because the boundary of the region to the left of the pieces leaves a vertex along the edge that hugs that region
 * -- turning right instead would follow the boundary of the *other* region wherever two regions touch at a point.
 *
 * A result that fails to close is returned as an empty path rather than as an open contour. That is the honest
 * answer: the pieces did not form a boundary, and an open contour filled by the caller would be a shape nobody
 * asked for.
 */
/**
 * @return the bounding rectangle of every endpoint in @p pieces.
 *
 * Used to size the vertex-snapping distance, which has to be measured against the shapes involved rather than against
 * their coordinates: a shape a thousand units from the origin is no larger than one at the origin, and its arithmetic
 * is no coarser either.
 */
DRect resultBounds(const std::vector<Segment2>& pieces) {
  DRect bounds = DRect::MakeEmpty();
  for (const Segment2& piece: pieces) {
    bounds.unionWith(piece.edge.start());
    bounds.unionWith(piece.edge.end());
  }
  return bounds;
}


/**
 * @param pieces the pieces to walk into closed contours.
 * @param tolerance how far apart two endpoints may be and still be the same vertex.
 * @return the closed contours the pieces form.
 */
Path stitch(const std::vector<Segment2>& pieces, const double tolerance) {
  Path result;
  if (pieces.empty()) return result;

  // Vertices are matched by proximity rather than by identity, because two pieces meeting at a split point hold
  // coordinates that agree to within the arithmetic rather than exactly.
  std::vector<DPoint> vertices;
  // The snapping distance, not the caller's numeric tolerance: see the note on `snapTolerance`. Two endpoints of a
  // single crossing are approximations of one point rather than two points that happen to be close.
  const double snap = snapTolerance(resultBounds(pieces));
  const auto vertexIndex = [&vertices, snap](const DPoint& point) {
    for (size_t i = 0; i < vertices.size(); ++i) {
      if ((vertices[i] - point).lengthSquared() <= snap * snap) return i;
    }
    vertices.push_back(point);
    return vertices.size() - 1;
  };

  struct Entry {
    size_t from = 0;
    size_t to = 0;
    size_t piece = 0;
    bool used = false;
  };

  std::vector<Entry> entries;
  entries.reserve(pieces.size());
  for (size_t i = 0; i < pieces.size(); ++i) {
    const Segment2& piece = pieces[i];
    // A reversed piece runs the other way, so the vertex it leaves from is the edge's stored end. See the note above:
    // getting this wrong turns the piece into a self-loop.
    const DPoint from = piece.reverse ? piece.edge.end() : piece.edge.start();
    const DPoint to = piece.reverse ? piece.edge.start() : piece.edge.end();
    entries.push_back(Entry { vertexIndex(from), vertexIndex(to), i, false });
  }

  const auto directionAtStart = [&pieces](const Entry& entry) {
    const Segment2& piece = pieces[entry.piece];
    // The direction of travel at the entry point, in the piece's own orientation.
    const DPoint tangent = piece.edge.tangentAt(piece.reverse ? 1.0 : 0.0);
    return piece.reverse ? DPoint { -tangent.x, -tangent.y } : tangent;
  };

  for (size_t seed = 0; seed < entries.size(); ++seed) {
    if (entries[seed].used) continue;

    // Walk a contour from this piece.
    std::vector<size_t> contour;
    size_t current = seed;
    const size_t startVertex = entries[seed].from;

    for (size_t guard = 0; guard <= entries.size(); ++guard) {
      entries[current].used = true;
      contour.push_back(current);

      const size_t at = entries[current].to;
      if (at == startVertex && contour.size() >= 2) break;  // closed

      // Candidate continuations: unused pieces leaving this vertex.
      const DPoint incoming = directionAtStart(entries[current]);
      size_t best = entries.size();
      double bestTurn = 0.0;
      for (size_t candidate = 0; candidate < entries.size(); ++candidate) {
        if (entries[candidate].used || entries[candidate].from != at) continue;
        const DPoint outgoing = directionAtStart(entries[candidate]);

        // The turn is the cross product of the incoming and outgoing directions: positive is a left turn, negative a
        // right turn, zero straight on.
        //
        // The rule is "the least counter-clockwise turn, and a right turn beats any left turn", which is what keeps
        // the filled region on the left of the walk. The right-turn precedence is the part that is easy to miss and
        // it is what makes the walk stay on the boundary of *this* region when two boundaries meet at a vertex --
        // taking the left turn there would hand the walk over to the neighbouring region's boundary, and the contour
        // would come out covering the wrong side.
        const double turn = incoming.cross(outgoing);

        // Negative turns sort before non-negative ones, and within each group the smallest is the least turn. The
        // offset has to be larger than any reachable cross product, whose magnitude is at most the product of the two
        // tangents -- far below this for any ordinary geometry.
        constexpr double kLeftTurnOffset = 1e300;
        const double key = turn < 0.0 ? turn : turn + kLeftTurnOffset;
        if (best == entries.size() || key < bestTurn) {
          best = candidate;
          bestTurn = key;
        }
      }

      if (best == entries.size()) break;  // dead end: the contour does not close
      current = best;
    }

    // A contour is only emitted when the walk came back to where it started. An unclosed walk means the pieces do
    // not bound a region, and emitting it would hand the caller a shape nobody asked for.
    //
    // ## What is still wrong here
    //
    // The walk marks each piece it visits as used, so a chain that leads nowhere -- which happens at a vertex where
    // several boundaries meet and the turn rule walks into a stretch that dead-ends -- consumes its pieces without
    // emitting them. They belong to no contour afterwards and no later walk can reach them, and counting unused pieces
    // does not reveal it, because the count is zero either way.
    //
    // Two responses to that were written and measured, and **both were worse than leaving it**:
    //
    //  * Reporting it so the operation refuses. Failures across sixty adversarial seeds fell to zero and first-pass
    //    refusals rose to 60 of 60 -- and to 59 of 60 with the generator restricted to coordinates a few hundred units
    //    across. Dead-ending is common and usually harmless, so refusing on it leaves an operation that answers almost
    //    nothing.
    //  * Releasing the consumed pieces so a later walk can use them. Adversarial failures *rose*, 41 to 54: the greedy
    //    turn rule depends on a consumed piece staying consumed, and handing it back lets a later contour pick up pieces
    //    belonging to a different region.
    //
    // So this is recorded rather than fixed. A union whose pieces dead-end still comes back as a partial shape, which is
    // the `union-contains` failure the fuzz harness reports; the measurements are in docs/graphics.md.
    if (contour.size() < 2 || entries[contour.back()].to != startVertex) continue;

    // Emit the contour as verbs.
    //
    // ## Emitting from the snapped vertices rather than from the pieces
    //
    // A curve is emitted with `cubicTo`, which continues from wherever the path currently is and does not carry a
    // start point of its own. The walk, meanwhile, works on *snapped* vertices -- two endpoints of one crossing are
    // approximations of a single point and are merged -- so a piece's stored start and the vertex it was matched to are
    // not quite the same coordinates.
    //
    // Emitting the control points as stored therefore starts every curve a few millionths away from where the previous
    // one ended. The gap is far too small to see and it is not harmless: the flattened contour gains a sliver of area
    // at each of the crossings, which is enough to move the measured area of a circle-circle lens by more than a square
    // unit and fail an assertion that should have passed.
    //
    // So the endpoints come from the vertices, which are the coordinates the walk actually agreed on, and only the
    // interior control points come from the piece. The alternative -- snapping the pieces themselves before emitting --
    // would work too, but it would mutate the geometry after it has been classified, and the classification is what the
    // pieces are for.
    const Entry& first = entries[contour.front()];
    const DPoint contourStart = vertices[first.from];
    result.moveTo(Point { static_cast<Scalar>(contourStart.x), static_cast<Scalar>(contourStart.y) });

    for (const size_t index: contour) {
      const Entry& entry = entries[index];
      const Segment2& piece = pieces[entry.piece];
      const DPoint to = vertices[entry.to];

      // The interior control points, in travel order. The first and last of a piece's points are its endpoints, which
      // the vertices supply, so only the ones between them are used here.
      std::array<DPoint, 3> controls {};
      int controlCount = 0;
      for (size_t i = 1; i + 1 < piece.edge.polygon.size(); ++i) {
        const size_t at = piece.reverse ? piece.edge.polygon.size() - 1 - i : i;
        controls[controlCount++] = piece.edge.polygon[at];
      }

      if (controlCount == 0) {
        result.lineTo(Point { static_cast<Scalar>(to.x), static_cast<Scalar>(to.y) });
      } else if (controlCount == 1) {
        result.quadTo(Point { static_cast<Scalar>(controls[0].x), static_cast<Scalar>(controls[0].y) },
                Point { static_cast<Scalar>(to.x), static_cast<Scalar>(to.y) });
      } else {
        result.cubicTo(Point { static_cast<Scalar>(controls[0].x), static_cast<Scalar>(controls[0].y) },
                Point { static_cast<Scalar>(controls[1].x), static_cast<Scalar>(controls[1].y) },
                Point { static_cast<Scalar>(to.x), static_cast<Scalar>(to.y) });
      }
    }
    result.close();

  }

  return result;
}


/** @return the fill rule a boolean result is expressed in, which is always nonzero. */
constexpr FillRule kResultRule = FillRule::Winding;




} // namespace anonymous

std::optional<Path> op(const Path& one, const Path& two, const PathOp setOp) {
  if (one.isEmpty() && two.isEmpty()) return Path {};

  DRect bounds = one.controlBounds();
  bounds.unionWith(two.controlBounds());
  const double tolerance = vertexTolerance(bounds);

  const std::vector<Edge> edgesOne = collectEdges(one, 0);
  const std::vector<Edge> edgesTwo = collectEdges(two, 1);

  // With no edges on one side the operation is a containment question, and the classification below answers it
  // directly -- there is simply nothing to split against.
  const std::vector<Edge> splitOne = splitEdges(edgesOne, edgesTwo);
  const std::vector<Edge> splitTwo = splitEdges(edgesTwo, edgesOne);

  const FillRule ruleOne = one.fillRule();
  const FillRule ruleTwo = two.fillRule();

  std::vector<Segment2> kept;

  // ## Classification, in two stages
  //
  // ### Stage one: which pieces lie on the result's boundary
  //
  // A piece bounds the result exactly when the operation's answer differs on its two sides, so both are sampled and the
  // piece is kept when they disagree. That is the definition applied directly, and it is why there is no table of cases:
  // a piece of one shape outside the other is on the union's boundary and not the intersection's, and a piece lying
  // *along* the other shape's boundary is handled without being recognised as special at all.
  //
  // ### Stage two: reconciling coincident pieces
  //
  // Stage one is necessary and **not sufficient**, and finding that out cost several attempts. When two shapes share an
  // edge, *both* copies of it satisfy the two-sided test, so a union of two corner-overlapping squares comes out with
  // twelve boundary pieces instead of eight -- the shared edge twice -- and the traversal then walks a contour that
  // doubles back on itself. The union measured eight square units where seven were correct, while the exclusive or that
  // the same change had fixed came out right.
  //
  // So pieces running along the same stretch are collected into groups and each group contributes at most one piece.
  //
  // ### Choosing which copy of a group to keep
  //
  // The winding-positive side of a piece is the side of the shape it came from; two coincident copies from different
  // shapes are wound against each other, so their positive sides are *opposite*. The result differs across the group
  // exactly when it is in on one of those two sides and out on the other -- which is the same two-sided question, asked
  // once for the group rather than once per copy.
  //
  // The copy that is kept is the one whose direction the result is on the *positive* side of, so that the surviving
  // piece already runs the right way and the walk needs no separate orientation pass. If the result is on the negative
  // side instead, that copy is reversed. Keeping the first copy unconditionally would leave an outer boundary and the
  // wall of a hole wound the same way, which is what made an earlier attempt at the exclusive or count the shared
  // square twice.
  const double probeRadius = std::max(tolerance * 64.0, 1e-6);

  std::vector<Edge> candidates;
  candidates.reserve(splitOne.size() + splitTwo.size());
  for (const Edge& piece: splitOne) candidates.push_back(piece);
  for (const Edge& piece: splitTwo) candidates.push_back(piece);

  std::vector<bool> grouped(candidates.size(), false);
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (grouped[i]) continue;
    // Only pieces that are actually on the boundary of the result can contribute.
    if (!boundaryOfResult(candidates[i], setOp, edgesOne, ruleOne, edgesTwo, ruleTwo, tolerance)) continue;

    // The group: this piece and every later piece coincident with it.
    std::vector<size_t> group { i };
    grouped[i] = true;
    for (size_t j = i + 1; j < candidates.size(); ++j) {
      if (grouped[j]) continue;
      if (coincident(candidates[i], candidates[j], tolerance)) {
        group.push_back(j);
        grouped[j] = true;
      }
    }

    // Whether the result is on the positive side of the group's direction. Tested against the first member, whose
    // positive side and the opposite one the group's two sides are.
    const Edge& reference = candidates[i];
    const DPoint tangent = reference.derivativeAt(0.5);
    const double magnitude = tangent.length();
    if (!(magnitude > 0.0)) continue;

    const double length = (reference.end() - reference.start()).length();
    const double probe = std::clamp(length / 4.0, probeRadius, std::max(length * 0.4, probeRadius));
    const DPoint offset { -tangent.y / magnitude * probe, tangent.x / magnitude * probe };
    const DPoint middle = reference.midpoint();

    const bool positiveIn = regionContains(setOp, edgesOne, ruleOne, edgesTwo, ruleTwo,
            DPoint { middle.x + offset.x, middle.y + offset.y }, probe);
    const bool negativeIn = regionContains(setOp, edgesOne, ruleOne, edgesTwo, ruleTwo,
            DPoint { middle.x - offset.x, middle.y - offset.y }, probe);

    if (positiveIn == negativeIn) continue;  // the group is not on the boundary after all

    // A reversed copy is what is wanted when the result turned out to be on the negative side; the walk will traverse
    // it from its stored end, which puts the result on the positive side of the direction travelled.
    const bool reverse = !positiveIn;
    kept.push_back(Segment2 { reference, reverse });
  }
  Path result = stitch(kept, tolerance);
  result.setFillRule(kResultRule);

  // A result with no contours means the operation is empty: two disjoint shapes have no intersection, and a shape
  // minus itself is nothing. That is a legitimate answer and not a failure, so it is an empty path rather than a
  // null. Only a failure to build a boundary from pieces that exist is reported as a null.
  if (result.isEmpty() && !kept.empty()) return std::nullopt;
  return result;
}

std::optional<Path> simplify(const Path& path) {
  if (path.isEmpty()) return Path {};

  DRect bounds = path.controlBounds();
  const double tolerance = vertexTolerance(bounds);

  const std::vector<Edge> edges = collectEdges(path, 0);

  // Split each edge where it meets any *other* edge of the same path. Crossing an edge with itself would report the
  // whole overlap, which is not a split point.
  std::vector<Edge> split;
  split.reserve(edges.size() * 2);
  for (size_t i = 0; i < edges.size(); ++i) {
    std::vector<Edge> others;
    others.reserve(edges.size() - 1);
    for (size_t j = 0; j < edges.size(); ++j) {
      if (i != j) others.push_back(edges[j]);
    }

    std::vector<double> cuts;
    for (const Edge& other: others) {
      for (const Intersection& hit: intersect(toSegment(edges[i]), toSegment(other))) {
      // As in `splitEdges`: only an intersection that is on the *other* piece splits this one.
      if (!(hit.u >= -kSplitEpsilon && hit.u <= 1.0 + kSplitEpsilon)) continue;
        if (hit.t > kSplitEpsilon && hit.t < 1.0 - kSplitEpsilon) cuts.push_back(hit.t);
      }
    }

    if (cuts.empty()) {
      split.push_back(edges[i]);
      continue;
    }

    std::sort(cuts.begin(), cuts.end());
    DPoint cursor = edges[i].start();
    double previous = 0.0;
    for (const double cut: cuts) {
      if (cut - previous <= kSplitEpsilon) continue;
      Edge piece = edges[i].subedge(previous, cut);
      split.push_back(piece);
      cursor = piece.end();
      previous = cut;
    }
    if (1.0 - previous > kSplitEpsilon) {
      split.push_back(edges[i].subedge(previous, 1.0));
    }
  }

  // Keep the pieces that bound a filled region.
  //
  // ## The question being asked
  //
  // A piece is kept when the region on its winding-positive side is filled. Under the even-odd rule that is the
  // crossing count from every *other* piece, and under the nonzero rule it is the winding -- so resolving even-odd into
  // nonzero, which is what this function is for, is a matter of keeping the pieces whose positive side is filled.
  //
  // ## Why the sample is a ring and not a point
  //
  // A single point just off the midpoint is not a point in a region if it happens to land on another edge, and that is
  // exactly what a bow tie does: for the piece from (0,0) to (5,5) the midpoint is (2.5,2.5) and the offset to its
  // positive side lands on (2.5,2.5) as well, because that is where the *other* edge of the bow tie passes. The ray
  // test then correctly reports no crossing at that x -- a boundary point is not a crossing -- and the region reads as
  // empty. The lower lobe is discarded, the contour cannot close, and `simplify` reports failure on a shape whose two
  // lobes are perfectly plain.
  //
  // So the test is made where the geometry is unambiguous: a ring of samples is taken at several radii and the
  // majority decides, exactly as `regionContains` does for the two-operand case. A sample on a boundary contributes
  // whichever answer its own arithmetic gives, and eight of them cannot all be coincidentally wrong.
  const FillRule rule = path.fillRule();
  std::vector<Segment2> kept;
  kept.reserve(split.size());

  /** @return how many of the sampled neighbours of @p point are in the filled region. */
  const auto filledAround = [&split, &rule](const DPoint& point, const double radius) {
    int inside = 0;
    int outside = 0;
    for (int s = 0; s < 8; ++s) {
      const double angle = static_cast<double>(s) * 3.14159265358979 / 4.0;
      const DPoint probe { point.x + std::cos(angle) * radius, point.y + std::sin(angle) * radius };

      // ## Every piece counts, including the one being classified
      //
      // The first version of this skipped the piece itself, on the reasoning that a contour does not cross its own
      // boundary. That reasoning is about the *piece*, and the sample is not on the piece -- it is a small step to one
      // side of it. For a square's east edge the sample sits just inside, the only edge between the sample and the ray's
      // far end is that east edge, and excluding it left nothing to count: winding zero, an inside point reported as
      // outside, and three edges of four.
      //
      // Counting it instead gives the answer the classification wants. A sample a small step from a piece crosses that
      // piece exactly when the step put it on the side the piece bounds, which is precisely the question being asked.
      int winding = 0;
      int crossings = 0;
      for (const Edge& other: split) {
        const RayResult ray = countRayCrossings(other, probe);
        winding += ray.winding;
        crossings += ray.crossings;
      }

      const bool in = rule == FillRule::EvenOdd ? (crossings & 1) != 0 : winding != 0;
      if (in) {
        ++inside;
      } else {
        ++outside;
      }
    }
    return inside > outside;
  };

  for (size_t i = 0; i < split.size(); ++i) {
    const Edge& piece = split[i];
    const DPoint middle = piece.midpoint();
    const DPoint tangent = piece.derivativeAt(0.5);
    const double magnitude = tangent.length();
    if (!(magnitude > 0.0)) continue;

    // The offset to the piece's winding-positive side, scaled with the piece.
    //
    // ## The bounds have to be ordered, and the upper one has to include the lower
    //
    // `std::clamp` requires its lower bound not to exceed its upper, and it is undefined behaviour when it does. This
    // call used to be
    //
    //     std::clamp(length / 4.0, std::max(tolerance * 64.0, length * 1e-6), std::max(length * 0.4, 1e-12));
    //
    // whose upper bound has no `tolerance` term while its lower one does. A path spanning a large range with a short
    // piece in it inverts them -- measured on one such piece, `length` was 4.9e-06 against a `tolerance` of 1e-3, so the
    // lower was 0.064 and the upper 2.0e-06. The two placements that differ from this one, in `op` and in
    // `boundaryOfResult`, both fold their lower bound into their upper; this one did not, which is what made it a defect
    // rather than a convention, and the fix is to do what they do.
    //
    // In a release build the inverted call is quietly wrong. In a debug build it is loud and misleading: the standard
    // library's own check reports "invalid bounds arguments passed to std::clamp" through the CRT, which puts up a modal
    // assertion and waits -- a process that stops returning while consuming no processor time at all, which is what made
    // this look like a hang in pathops for three rounds.
    const double length = (piece.end() - piece.start()).length();
    const double lower = std::max(tolerance * 64.0, length * 1e-6);
    const double upper = std::max({ length * 0.4, 1e-12, lower });
    const double offset = std::clamp(length / 4.0, lower, upper);
    const DPoint positive { middle.x - tangent.y / magnitude * offset, middle.y + tangent.x / magnitude * offset };

    // ## The sample is a ring, and that is what fixed the simpler cases
    //
    // A single point just off the midpoint is not a point in a region if it lands on another edge, which is exactly
    // what a bow tie does: for the piece from (0,0) to (5,5) the midpoint is (2.5,2.5) and the offset to its positive
    // side lands on (2.5,2.5) as well, because that is where the *other* edge of the bow tie passes. The ray test then
    // correctly reports no crossing at that x -- a boundary point is not a crossing -- and the region reads as empty.
    //
    // A ring of eight samples with a majority cannot be wrong in the same direction eight times over, which is why
    // `regionContains` uses one for the two-operand case.
    //
    // ## What used to be wrong here, and what replaced it
    //
    // This note said that testing *both* sides of the piece and keeping it when they differ "was tried, twice, and it
    // breaks the cases that work -- `asWinding` on a plain square stops resolving", that the orientation-independent form
    // was nonetheless "the right rule", and that the reason it failed "has not been found".
    //
    // It has since been applied, and `asWinding` on a square does resolve -- `AsWindingLeavesAWindingPathAlone` and the
    // simplification tests pass with it, along with every other test in the suite. The earlier attempts were made before
    // `Edge` carried its own control polygon and before the ring of eight samples replaced the single sample, so the rule
    // they tried was not this one, however much the description matches. The paragraph is kept because the reasoning in it
    // is what pointed at the fix, and the shape of the error -- a note describing a rule that no longer existed -- is
    // worth recognising again.
    //
    // The ring is what makes the two-sided test viable: with a single sample, one side of a piece could land on the piece
    // itself and read as unfilled, and both sides then read the same.
    //
    // ## The ring has to be much smaller than the offset
    //
    // The probe sits `offset` away from the piece, and the winding is counted from every *other* piece -- the piece
    // itself is excluded, because a contour does not cross its own boundary. So if a ring sample lands back on this
    // piece's own line, that crossing is discarded, and the sample is left counting only the edges beyond it. For a
    // square's east edge that is exactly what happened: the probe is one unit to the left and the ring was also one
    // unit, so every sample but the leftmost sat on the east edge's line, the crossing was excluded, and the winding
    // came out zero -- an inside point reported as outside, the edge dropped, and three edges of four left for a
    // contour that cannot close.
    //
    // The ring is therefore a small fraction of the offset: far enough to escape the arithmetic, and near enough that
    // the piece being classified is the nearest geometry to it.
    const double ring = std::max(offset * 0.05, 1e-9);
    const bool positiveIn = filledAround(positive, ring) || filledAround(positive, ring * 2.0);

    // The same question asked of the other side of the piece, which is what makes the answer independent of the
    // direction the contour happens to run in.
    //
    // The earlier rule tested only the positive side and kept the piece when that side was filled. That is
    // orientation-dependent, and `simplify` cannot assume an orientation, because it has to accept its own output -- and
    // its output is whatever set of contours `stitch` happened to produce. Measured on a path that reproduced the
    // failure, one piece of length 484.883 came out filled on the first pass and unfilled on the second, same length and
    // same geometry: the samples had simply landed on the other side. That is what made `simplify` unable to simplify
    // its own result.
    //
    // A piece bounds the result when the two sides *differ* -- filled on one and not the other -- which is the rule
    // `boundaryOfResult` already uses for the two-operand case, and the rule the note that used to sit here called "the
    // right rule". The note also recorded that trying it broke `asWinding` on a plain square; that does not reproduce
    // against the current tests, which includes `AsWindingLeavesAWindingPathAlone` and the simplification tests, all of
    // which pass with this form. The version that note described was written before `Edge` carried its own control
    // polygon and before the ring replaced the single sample, so it was a different rule wearing the same description.
    //
    // What it costs, measured over sixty adversarial seeds from the fuzz harness: failures 58 -> 53, and first-pass
    // refusals 45 -> 49. Roughly four paths moved from "returns geometry that then fails an invariant" to "declines to
    // resolve this path", which is the better of the two, and the refusal is a documented outcome.
    const DPoint negative { middle.x + tangent.y / magnitude * offset, middle.y - tangent.x / magnitude * offset };
    const bool negativeIn = filledAround(negative, ring) || filledAround(negative, ring * 2.0);
    if (positiveIn != negativeIn) {
      // The result lies on the negative side when that is the filled one. A stored piece is traversed from its end, so
      // such a piece is reversed to put the result on the positive side of the direction travelled -- the same rule
      // `boundaryOfResult` applies.
      kept.push_back(Segment2 { piece, !positiveIn });
    }
  }

  Path result = stitch(kept, tolerance);
  result.setFillRule(kResultRule);

  // A result with no contours means the operation is empty: two disjoint shapes have no intersection, and a shape
  // minus itself is nothing. That is a legitimate answer and not a failure, so it is an empty path rather than a
  // null. Only a failure to build a boundary from pieces that exist is reported as a null.
  if (result.isEmpty() && !kept.empty()) return std::nullopt;
  return result;
}

std::optional<Path> asWinding(const Path& path) {
  if (path.fillRule() == FillRule::Winding) {
    // Already the rule a boolean result is expressed in, so there is nothing to resolve.
    return path;
  }
  return simplify(path);
}

} // namespace graphics::pathops
