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

/**
 * Appends a closed disc of radius @p radius about @p centre, as a polygon.
 *
 * The point order is whatever the angle sweep produces; `StrokeOutline::append` is what guarantees that the disc winds
 * the same way as the edge quads it is unioned with. Used for both a round join and a round cap, which are the same
 * piece of geometry.
 */
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

namespace {

/**
 * Pieces below this twice-area contribute no pixel and are dropped.
 *
 * A collinear piece has exactly zero area, but a piece produced by an almost-degenerate turn can have an area that is
 * merely tiny. Both cover nothing visible, and dropping them keeps the degenerate-turn arithmetic out of the contour
 * list. The threshold is on twice the signed area, so it is in squared caller units; 1e-12 is far below a pixel at any
 * sane scale and far above what floating-point cancellation produces.
 */
constexpr Scalar kDegeneratePieceArea = 1e-12f;

/** @return twice the signed area of a closed polygon through @p points. */
Scalar signedAreaTwice(const std::vector<Point>& points) noexcept {
  Scalar sum = 0.0f;
  for (size_t i = 0; i < points.size(); ++i) {
    const Point& a = points[i];
    const Point& b = points[(i + 1) % points.size()];
    sum += a.x * b.y - b.x * a.y;
  }
  return sum;
}

/** @return the squared distance between two points. */
Scalar distanceSquared(const Point& a, const Point& b) noexcept {
  const Scalar dx = b.x - a.x;
  const Scalar dy = b.y - a.y;
  return dx * dx + dy * dy;
}

/**
 * The accumulator for a stroke's pieces.
 *
 * ## Why orientation is normalised here rather than reasoned about per piece
 *
 * The pieces are built by three different helpers with three different natural point orders, and the one thing that
 * must hold is that they all agree: the nonzero fill unions windings of the same sign and cancels them of opposite
 * sign. Rather than deriving the correct order for each helper -- and getting it wrong once, which is exactly the
 * defect this replaces -- every piece is passed through `append`, which reverses the point order whenever the signed
 * area is positive. All pieces therefore come out with the same (negative) signed area, whatever order they were
 * built in, and the union is a union by construction.
 */
class StrokeOutline {
public:
  /** Appends @p points as one closed contour, or drops it when it has no area. */
  void append(std::vector<Point> points) {
    if (points.size() < 3) return;

    const Scalar area = signedAreaTwice(points);
    if (!std::isfinite(area) || std::abs(area) <= kDegeneratePieceArea) return;
    if (area > 0.0f) std::reverse(points.begin(), points.end());

    Contour contour;
    contour.points = std::move(points);
    contour.closed = true;
    mContours.push_back(std::move(contour));
  }

  [[nodiscard]] std::vector<Contour> take() && { return std::move(mContours); }

private:
  std::vector<Contour> mContours;
};

/**
 * Appends the quad covering one edge of the stroke.
 *
 * The quad is centred on the segment and reaches half a width to either side. Its orientation is whatever the formula
 * produces; `StrokeOutline::append` normalises it with every other piece.
 */
void appendEdgeQuad(const Point& a, const Point& b, const Scalar half, StrokeOutline& out) {
  const Point directionOfEdge = direction(a, b);
  if (directionOfEdge.x == 0.0f && directionOfEdge.y == 0.0f) return; // a zero-length edge has no direction to offset

  // The perpendicular, scaled to half the width.
  const Scalar ox = -directionOfEdge.y * half;
  const Scalar oy = directionOfEdge.x * half;

  out.append({
    Point{ a.x + ox, a.y + oy },
    Point{ b.x + ox, b.y + oy },
    Point{ b.x - ox, b.y - oy },
    Point{ a.x - ox, a.y - oy },
  });
}

/** Appends the circular piece a round cap or a round join is made of. */
void appendRoundPiece(const Point& centre, const Scalar radius, StrokeOutline& out) {
  std::vector<Point> disc;
  appendDisc(centre, radius, disc);
  out.append(std::move(disc));
}

/**
 * Appends the piece that finishes one end of an open contour.
 *
 * @param end the end point.
 * @param edgeDirection the unit direction of the edge the cap belongs to, pointing *along* the contour.
 * @param atStart true for the first point, whose cap extends backwards along @p edgeDirection, false for the last,
 *                whose cap extends forwards.
 *
 * A butt cap appends nothing: the edge quad already ends exactly at the point, which is the definition of a butt cap.
 * A square cap appends the half-width rectangle past the end. A round cap appends a disc, which overlaps the edge
 * quad and covers the semicircle beyond it -- the same disc a round join uses, because the geometry is identical.
 */
void appendCap(const Point& end, const Point& edgeDirection, const bool atStart, const Scalar half,
        const StrokeCap cap, StrokeOutline& out) {
  if (cap == StrokeCap::kButt) return;
  if (edgeDirection.x == 0.0f && edgeDirection.y == 0.0f) return;

  if (cap == StrokeCap::kRound) {
    appendRoundPiece(end, half, out);
    return;
  }

  // Square: the band's own cross-section, pushed half a width past the end.
  const Scalar ox = -edgeDirection.y * half;
  const Scalar oy = edgeDirection.x * half;
  const Scalar extension = atStart ? -half : half;
  const Scalar sx = edgeDirection.x * extension;
  const Scalar sy = edgeDirection.y * extension;
  out.append({
    Point{ end.x + ox, end.y + oy },
    Point{ end.x - ox, end.y - oy },
    Point{ end.x - ox + sx, end.y - oy + sy },
    Point{ end.x + ox + sx, end.y + oy + sy },
  });
}

/**
 * Appends the piece that fills the outside of the corner at @p at.
 *
 * ## Which side is outside
 *
 * The two edge quads reach up to @p at and stop; on the side the path turns *towards* they overlap, so the union is
 * already solid there. On the other side they diverge and leave a wedge, and that wedge is what this fills. The turn's
 * sign -- the cross product of the incoming and outgoing directions -- says which side that is, and it is the same
 * rule for a convex and a concave vertex of the path: what matters is which way the path turns, not which side its
 * interior happens to be on.
 *
 * @param prev the previous point, which defines the incoming direction.
 * @param at the corner.
 * @param next the next point, which defines the outgoing direction.
 */
void appendJoin(const Point& prev, const Point& at, const Point& next, const Scalar half,
        const StrokeJoin join, const Scalar miterLimit, StrokeOutline& out) {
  const Point incoming = direction(prev, at);
  const Point outgoing = direction(at, next);
  if ((incoming.x == 0.0f && incoming.y == 0.0f) || (outgoing.x == 0.0f && outgoing.y == 0.0f)) return;

  const Scalar cross = incoming.x * outgoing.y - incoming.y * outgoing.x;
  const Scalar dot = incoming.x * outgoing.x + incoming.y * outgoing.y;
  // A straight continuation needs nothing: the two quads already meet exactly. The dot check keeps a literal reversal
  // (cross zero, dot negative) in the piece-producing path below, where it needs a bevel rather than nothing.
  if (cross == 0.0f && dot > 0.0f) return;

  // The outer side is the one the path turns away from. With the perpendicular taken as (-y, x), a positive cross
  // turns towards that perpendicular's side, so the outside is the other sign.
  const Scalar side = cross > 0.0f ? -1.0f : 1.0f;
  const Point incomingNormal{ -incoming.y, incoming.x };
  const Point outgoingNormal{ -outgoing.y, outgoing.x };

  const Point outerIncoming{ at.x + side * half * incomingNormal.x, at.y + side * half * incomingNormal.y };
  const Point outerOutgoing{ at.x + side * half * outgoingNormal.x, at.y + side * half * outgoingNormal.y };

  if (join == StrokeJoin::kRound) {
    appendRoundPiece(at, half, out);
    return;
  }

  if (join == StrokeJoin::kMiter) {
    // The two outer normals point into the wedge. The miter tip is along their bisector, at `half / sin(half angle)`,
    // where the half angle is measured between the normals: for a square corner the normals are perpendicular, the
    // sine is sqrt(1/2), and the tip is sqrt(2) half-widths out, which is the corner of the square outline.
    const Point outerIncomingUnit{ side * incomingNormal.x, side * incomingNormal.y };
    const Point outerOutgoingUnit{ side * outgoingNormal.x, side * outgoingNormal.y };
    const Scalar normalDot = outerIncomingUnit.x * outerOutgoingUnit.x +
                             outerIncomingUnit.y * outerOutgoingUnit.y;
    const Scalar sineHalfAngle = std::sqrt(std::max(0.0f, 0.5f * (1.0f + normalDot)));

    const Scalar limit = std::max(1.0f, miterLimit);
    if (sineHalfAngle > 0.0f && sineHalfAngle >= 1.0f / limit) {
      Point bisector{ outerIncomingUnit.x + outerOutgoingUnit.x, outerIncomingUnit.y + outerOutgoingUnit.y };
      const Scalar bisectorLength = std::sqrt(bisector.x * bisector.x + bisector.y * bisector.y);
      if (bisectorLength > 0.0f) {
        const Scalar tipDistance = half / sineHalfAngle;
        const Point tip{ at.x + bisector.x / bisectorLength * tipDistance,
          at.y + bisector.y / bisectorLength * tipDistance };
        out.append({ at, outerIncoming, tip, outerOutgoing });
        return;
      }
    }
    // Past the miter limit the corner is bevelled, which is the same piece a bevel join uses. Falling through rather
    // than duplicating the triangle keeps one definition of "the bevel".
  }

  out.append({ at, outerIncoming, outerOutgoing });
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
    // ## A contour that returns to its start is closed, however it was spelled
    //
    // A stroker must know whether the edge from the last point back to the first is real, and the `close` verb is only
    // one way to say it: writing that edge as a final `lineTo(start)` describes the same closed shape. The duplicate
    // final point is dropped either way, so the point list alone cannot answer, and recording only the `close` verb
    // would make a stroker drop the last edge of every explicitly-closed contour. The flag is therefore recorded from
    // wherever the closure actually happened.
    const bool returnsToStart = open && current.points.size() >= 2 &&
      current.points.back().x == start.x && current.points.back().y == start.y;
    // The closure is recorded before the move, so the contour that is stored carries it.
    current.closed = closing || returnsToStart;
    if (open && current.points.size() >= 2) {
      // A duplicated final point would be a zero-length edge. It costs a crossing test per scanline and makes the
      // contour's direction ambiguous where the two coincide, so it is dropped rather than passed on.
      if (returnsToStart) {
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

std::vector<Contour> strokePath(const pathops::Path& path, const StrokeStyle& style,
        const FlattenOptions options) {
  if (!(style.width > 0.0f) || !std::isfinite(style.width)) return {};

  const Scalar half = style.width * 0.5f;
  // The unfiltered polylines: a stroke of a single line is the case a fill cannot express and the one
  // that matters most.
  const std::vector<Contour> flattened = flattenToPolylines(path, options);

  StrokeOutline outline;

  for (const Contour& contour: flattened) {
    // ## One point per distinct position
    //
    // Consecutive duplicates are dropped before anything is offset. They are what a flattened curve produces where a
    // subdivision is finer than the coordinates can express, and a zero-length edge has no direction to offset -- left
    // in, it turns into a division that the direction helper papers over while the join arithmetic either side of it
    // reads a meaningless normal.
    std::vector<Point> points;
    points.reserve(contour.points.size());
    for (const Point& point: contour.points) {
      if (points.empty() || distanceSquared(points.back(), point) > 1e-12f) points.push_back(point);
    }

    bool closed = contour.closed;
    // A contour that returns to its own start is closed in fact even if the path spelled the return as a `lineTo`
    // instead of a `close`. `flattenToPolylines` already drops the duplicated final point and records the closure --
    // this only has to avoid treating the two remaining coincident ends as an edge.
    if (closed && points.size() >= 2 && distanceSquared(points.front(), points.back()) <= 1e-12f) {
      points.pop_back();
    }
    if (points.size() < 2) continue;

    const size_t count = points.size();
    const size_t edgeCount = closed ? count : count - 1;
    for (size_t i = 0; i < edgeCount; ++i) {
      appendEdgeQuad(points[i], points[(i + 1) % count], half, outline);
    }

    if (closed) {
      // Every vertex is a join, including the one where the closing edge meets the first. That wrap is why the index
      // arithmetic is modular rather than a plain loop over the interior points.
      for (size_t i = 0; i < count; ++i) {
        appendJoin(points[(i + count - 1) % count], points[i], points[(i + 1) % count], half, style.join,
                style.miterLimit, outline);
      }
    } else {
      appendCap(points.front(), direction(points.front(), points[1]), true, half, style.cap, outline);
      appendCap(points.back(), direction(points[count - 2], points.back()), false, half, style.cap, outline);
      for (size_t i = 1; i + 1 < count; ++i) {
        appendJoin(points[i - 1], points[i], points[i + 1], half, style.join, style.miterLimit, outline);
      }
    }
  }

  return std::move(outline).take();
}

} // namespace graphics::raster
