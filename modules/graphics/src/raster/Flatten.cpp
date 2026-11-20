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

#include <private/graphics/Flatten.h>

#include <algorithm>
#include <cmath>

namespace graphics::raster {
namespace {

/**
 * The recursion limit for subdivision.
 *
 * A limit as well as a flatness test, because the test is a floating-point comparison and a curve with
 * coordinates near the edge of what a float can distinguish can fail it forever. Twenty levels is far more
 * than any visible curve needs -- a cubic reaching it has been subdivided into a million pieces -- so
 * reaching the limit means the input was degenerate, and stopping there is better than exhausting the stack.
 */
constexpr int kMaxSubdivisionDepth = 20;

/** @return the squared distance from @p point to the line through @p a and @p b. */
Scalar distanceToLineSquared(const Point& point, const Point& a, const Point& b) noexcept {
  const Scalar dx = b.x - a.x;
  const Scalar dy = b.y - a.y;
  const Scalar lengthSquared = dx * dx + dy * dy;
  if (lengthSquared <= 0.0f) {
    // A degenerate chord: the two ends coincide, so the "distance to the line" is the distance to the point.
    const Scalar px = point.x - a.x;
    const Scalar py = point.y - a.y;
    return px * px + py * py;
  }
  const Scalar cross = (point.x - a.x) * dy - (point.y - a.y) * dx;
  return cross * cross / lengthSquared;
}

/** @return the midpoint of two points. */
Point midpoint(const Point& a, const Point& b) noexcept {
  return Point{ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f };
}

/** Recursively subdivides a cubic until it is flat enough, appending each piece's end. */
void subdivideCubic(const Point& p0, const Point& p1, const Point& p2, const Point& p3, const int depth,
        std::vector<Point>& out, const Scalar tolerance) {
  // The flatness test compares both control points against the chord. Testing only one of them lets a curve
  // with an S shape pass -- both controls can be near the chord while the curve between them swings wide.
  const Scalar toleranceSquared = tolerance * tolerance;
  const bool flat = distanceToLineSquared(p1, p0, p3) <= toleranceSquared &&
                    distanceToLineSquared(p2, p0, p3) <= toleranceSquared;

  if (flat || depth >= kMaxSubdivisionDepth) {
    out.push_back(p3);
    return;
  }

  // de Casteljau at t = 0.5, which is the numerically stable way to split a Bezier. Splitting by evaluating
  // a polynomial at 0.5 would lose precision for coordinates far from the origin.
  const Point p01 = midpoint(p0, p1);
  const Point p12 = midpoint(p1, p2);
  const Point p23 = midpoint(p2, p3);
  const Point p012 = midpoint(p01, p12);
  const Point p123 = midpoint(p12, p23);
  const Point p0123 = midpoint(p012, p123);

  subdivideCubic(p0, p01, p012, p0123, depth + 1, out, tolerance);
  subdivideCubic(p0123, p123, p23, p3, depth + 1, out, tolerance);
}

/** Recursively subdivides a quadratic. The same shape as the cubic above, with one fewer control point. */
void subdivideQuadratic(const Point& p0, const Point& p1, const Point& p2, const int depth,
        std::vector<Point>& out, const Scalar tolerance) {
  const Scalar toleranceSquared = tolerance * tolerance;
  if (distanceToLineSquared(p1, p0, p2) <= toleranceSquared || depth >= kMaxSubdivisionDepth) {
    out.push_back(p2);
    return;
  }

  const Point p01 = midpoint(p0, p1);
  const Point p12 = midpoint(p1, p2);
  const Point p012 = midpoint(p01, p12);

  subdivideQuadratic(p0, p01, p012, depth + 1, out, tolerance);
  subdivideQuadratic(p012, p12, p2, depth + 1, out, tolerance);
}

/** Appends a closed half-disc of radius @p radius about @p centre, as a polygon. */
void appendDisc(const Point& centre, const Scalar radius, std::vector<Point>& out) {
  // The segment count follows the radius so that a large join is smooth and a small one does not pay for
  // detail it cannot show. Bounded at both ends: below six the join is visibly a polygon, and above the
  // maximum the cost stops being worth anything visible.
  constexpr int kMinSegments = 6;
  constexpr int kMaxSegments = 64;
  const int segments = std::clamp(static_cast<int>(std::ceil(radius * 4.0f)), kMinSegments, kMaxSegments);

  for (int i = 0; i < segments; ++i) {
    const Scalar angle = 2.0f * kPi * static_cast<Scalar>(i) / static_cast<Scalar>(segments);
    out.push_back(Point{ centre.x + std::cos(angle) * radius, centre.y + std::sin(angle) * radius });
  }
}

/** @return the unit vector from @p from to @p to, or zero when they coincide. */
Point direction(const Point& from, const Point& to) noexcept {
  const Scalar dx = to.x - from.x;
  const Scalar dy = to.y - from.y;
  const Scalar length = std::sqrt(dx * dx + dy * dy);
  if (!(length > 0.0f)) return Point{ 0.0f, 0.0f };
  return Point{ dx / length, dy / length };
}

} // namespace anonymous

Scalar sanitize(const Scalar value, const Scalar fallback) noexcept {
  return std::isfinite(value) ? value : fallback;
}

bool isFinite(const Point& point) noexcept { return std::isfinite(point.x) && std::isfinite(point.y); }

Rect Contour::bounds() const noexcept {
  if (points.empty()) return Rect::MakeLTRB(0.0f, 0.0f, 0.0f, 0.0f);

  Scalar left = points[0].x;
  Scalar top = points[0].y;
  Scalar right = points[0].x;
  Scalar bottom = points[0].y;
  for (const Point& point: points) {
    left = std::min(left, point.x);
    top = std::min(top, point.y);
    right = std::max(right, point.x);
    bottom = std::max(bottom, point.y);
  }
  return Rect::MakeLTRB(left, top, right, bottom);
}

void flattenQuadratic(const Point& from, const Point& control, const Point& to, std::vector<Point>& out,
        const FlattenOptions options) {
  if (!isFinite(from) || !isFinite(control) || !isFinite(to)) {
    out.push_back(Point{ sanitize(to.x), sanitize(to.y) });
    return;
  }
  subdivideQuadratic(from, control, to, 0, out, options.tolerance);
}

void flattenCubic(const Point& from, const Point& control1, const Point& control2, const Point& to,
        std::vector<Point>& out, const FlattenOptions options) {
  if (!isFinite(from) || !isFinite(control1) || !isFinite(control2) || !isFinite(to)) {
    out.push_back(Point{ sanitize(to.x), sanitize(to.y) });
    return;
  }
  subdivideCubic(from, control1, control2, to, 0, out, options.tolerance);
}

std::vector<Contour> flattenToPolylines(const pathops::Path& path, const FlattenOptions options) {
  std::vector<Contour> contours;

  // The contour being built, and the point its `moveTo` started from.
  Contour current;
  Point start{ 0.0f, 0.0f };
  Point cursor{ 0.0f, 0.0f };
  bool open = false;

  /**
   * Set when the contour in progress was ended by a `close` verb.
   *
   * Declared before the lambda because the lambda captures it by reference and records it on the contour it
   * finishes. It is what a measurement needs and a fill does not: a `close` supplies an edge, so a triangle
   * written as three points has a perimeter of three sides only if the contour is closed.
   */
  bool closing = false;

  /**
   * Finishes the contour in progress, keeping it when it has at least an edge.
   *
   * At least *two* points, not three. A fill can do nothing with a contour that has no area, but the stroker
   * needs the two-point ones: a single line segment is two points and is the most common thing anybody strokes.
   * Requiring three here meant every stroked line was silently discarded and produced no pixels at all, which is
   * what the first round of stroke tests found. `flattenPath` applies the stricter rule for fills.
   */
  const auto finishContour = [&contours, &current, &start, &open, &closing]() {
    // The closure is recorded before the move, so the contour that is stored carries it.
    current.closed = closing;
    if (open && current.points.size() >= 2) {
      // A duplicated final point would be a zero-length edge. It costs a crossing test per scanline and makes the
      // contour's direction ambiguous where the two coincide, so it is dropped rather than passed on.
      if (current.points.back().x == start.x && current.points.back().y == start.y) {
        current.points.pop_back();
      }
      if (current.points.size() >= 2) contours.push_back(std::move(current));
    }
    current = Contour{};
    open = false;
    closing = false;
  };

  for (const pathops::PathSegment& segment: path.segments()) {
    switch (segment.verb) {
      case pathops::Verb::Move:
        finishContour();
        start = Point{ static_cast<Scalar>(segment.points[0].x), static_cast<Scalar>(segment.points[0].y) };
        cursor = start;
        current.points.push_back(start);
        open = true;
        break;

      case pathops::Verb::Line: {
        if (!open) break; // a segment before any move has nothing to attach to
        const Point to{ static_cast<Scalar>(segment.points[0].x), static_cast<Scalar>(segment.points[0].y) };
        current.points.push_back(to);
        cursor = to;
        break;
      }

      case pathops::Verb::Quad: {
        if (!open) break;
        const Point control{
          static_cast<Scalar>(segment.points[0].x), static_cast<Scalar>(segment.points[0].y)
        };
        const Point to{ static_cast<Scalar>(segment.points[1].x), static_cast<Scalar>(segment.points[1].y) };
        flattenQuadratic(cursor, control, to, current.points, options);
        cursor = to;
        break;
      }

      case pathops::Verb::Cubic: {
        if (!open) break;
        const Point control1{
          static_cast<Scalar>(segment.points[0].x), static_cast<Scalar>(segment.points[0].y)
        };
        const Point control2{
          static_cast<Scalar>(segment.points[1].x), static_cast<Scalar>(segment.points[1].y)
        };
        const Point to{ static_cast<Scalar>(segment.points[2].x), static_cast<Scalar>(segment.points[2].y) };
        flattenCubic(cursor, control1, control2, to, current.points, options);
        cursor = to;
        break;
      }

      case pathops::Verb::Close:
        // The closing edge is implicit and nothing is appended for it. What is recorded is that it exists --
        // a perimeter that included the closing edge of a triangle would otherwise be short by one side -- and
        // that the contour is finished here, so a following `moveTo` starts a new one rather than continuing
        // this one.
        closing = true;
        finishContour();
        break;

      case pathops::Verb::Done:
        break;
    }
  }

  // A trailing contour is taken whether or not it was closed: a path with no closing verb still has an inside
  // when it is filled, and still has a length when it is measured.
  finishContour();
  return contours;
}

std::vector<Contour> flattenPath(const pathops::Path& path, const FlattenOptions options) {
  // Only contours that can hold an area. A two-point contour has no inside, so passing it on would widen the
  // rasterizer's loops for geometry that cannot contribute -- and returning one from a *fill* would be a lie
  // about what a fill can use. The stroke and measurement paths use `flattenToPolylines` instead, which keeps
  // them, because a single line segment is the most common thing anybody strokes or measures.
  std::vector<Contour> contours;
  for (Contour& contour: flattenToPolylines(path, options)) {
    if (contour.points.size() >= 3) {
      // A fill always closes its contours -- an open contour has no inside -- so the flag is set here rather
      // than propagated. That is what the rasterizer assumes anyway; making it true is what lets this function
      // hand back contours the rest of the layer can reason about uniformly.
      contour.closed = true;
      contours.push_back(std::move(contour));
    }
  }
  return contours;
}

std::vector<Contour> strokePath(const pathops::Path& path, const Scalar width, const FlattenOptions options) {
  if (!(width > 0.0f) || !std::isfinite(width)) return {};

  const Scalar half = width * 0.5f;
  // The unfiltered polylines: a stroke of a single line is the case a fill cannot express and the one
  // that matters most.
  const std::vector<Contour> flattened = flattenToPolylines(path, options);

  std::vector<Contour> result;
  // A generous reserve: one quad per flattened edge, which is the dominant term.
  result.reserve(flattened.size());

  /** Appends the quad covering one edge of the stroke. */
  const auto appendEdge = [half](const Point& a, const Point& b, std::vector<Point>& out) {
    const Point normal = direction(a, b);
    if (normal.x == 0.0f && normal.y == 0.0f) return; // a zero-length edge has no direction to offset

    // The perpendicular, scaled to half the width.
    const Scalar ox = -normal.y * half;
    const Scalar oy = normal.x * half;

    // One quad per edge, each closed on its own. Overlapping quads would be a problem for the even-odd rule,
    // where two overlapping regions cancel -- but the stroke is always filled with the nonzero rule, where
    // overlaps simply stay filled. That is why the stroke can be built this way at all.
    out.push_back(Point{ a.x + ox, a.y + oy });
    out.push_back(Point{ b.x + ox, b.y + oy });
    out.push_back(Point{ b.x - ox, b.y - oy });
    out.push_back(Point{ a.x - ox, a.y - oy });
  };

  for (const Contour& contour: flattened) {
    const std::vector<Point>& points = contour.points;
    if (points.size() < 2) continue;

    // ## One contour per piece, not one contour holding every piece
    //
    // Each edge's quad and each vertex's disc is emitted as its own closed contour, and the nonzero rule takes their
    // union -- so the stroke comes out with no seams where the pieces overlap, which is what this is for.
    //
    // Concatenating them into a single contour instead is the obvious-looking thing to do and it is wrong: consecutive
    // pieces are then joined by a straight segment from the end of one to the start of the next, and those segments cut
    // across the shape. The contour becomes a pinwheel whose chords wind around the interior, so a point inside the
    // shape can have nonzero winding and the stroke fills an area nobody drew. It stayed hidden because a two-point
    // line is one quad and has no joins at all -- and that is the case every stroke test used.
    for (size_t i = 0; i < points.size(); ++i) {
      const Point& a = points[i];
      const Point& b = points[(i + 1) % points.size()];

      Contour quad;
      appendEdge(a, b, quad.points);
      if (quad.points.size() >= 3) {
        quad.closed = true;
        result.push_back(std::move(quad));
      }
    }

    // A disc at every vertex covers the wedge the two adjoining quads leave, which is the round join. Doing
    // it for every vertex rather than only the convex ones is simpler and correct: at a concave corner the
    // disc is already inside the quads' union, so it adds nothing but a few redundant edges.
    for (const Point& point: points) {
      Contour disc;
      appendDisc(point, half, disc.points);
      if (disc.points.size() >= 3) {
        disc.closed = true;
        result.push_back(std::move(disc));
      }
    }
  }

  return result;
}

} // namespace graphics::raster
