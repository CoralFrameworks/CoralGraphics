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

#include "pathops/Curve.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace graphics::pathops {
namespace {

/**
 * How flat a curve must be before it is approximated by a line.
 *
 * This is not a rendering tolerance; it is the tolerance at which a curve's *topology* stops being
 * ambiguous. Two curves that pass within this distance of each other without touching will be treated
 * as touching, which is the safe direction for a boolean operation -- an intersection found where
 * there is none produces a zero-length edge that the traversal can drop, while an intersection missed
 * produces a path with a gap in it that nothing downstream can repair.
 */
constexpr double kFlatnessTolerance = 1.0 / 16.0;

/**
 * A hard limit on subdivision depth.
 *
 * A curve whose control points are not finite, or whose two halves are so nearly identical that the
 * flatness test never converges, would otherwise recurse until the stack runs out. Returning the
 * chord at this depth produces a slightly wrong answer for a degenerate input instead of a crash,
 * which is the right trade for data that came from a file.
 */
constexpr int kMaxSubdivisionDepth = 24;

/** @return true when @p v is finite. Deliberately not std::isfinite, which is not constexpr in C++17. */
bool finite(const double v) noexcept { return v - v == 0.0; }

/** Adds one flattened point, skipping an exact repeat of the previous one. */
void appendPoint(std::vector<DPoint>& out, const DPoint& p) {
  if (out.empty() || out.back() != p) out.push_back(p);
}

/**
 * Flattens a quadratic by recursive subdivision.
 *
 * The flatness test uses the distance from the control point to the chord rather than the curve's own
 * deviation, because for a quadratic the control point deviates from the chord by exactly twice the
 * curve's maximum deviation -- so the test is a constant factor off, and the constant is folded into
 * the tolerance rather than computed.
 */
void flattenQuad(const DPoint& p0, const DPoint& p1, const DPoint& p2, const int depth, std::vector<DPoint>& out) {
  if (depth >= kMaxSubdivisionDepth) {
    appendPoint(out, p2);
    return;
  }

  const DPoint chord = p2 - p0;
  // Twice the area of the triangle, divided by the chord length, is the control point's distance from
  // the chord. Comparing the squared quantities avoids a square root in the common case.
  const double cross = std::fabs((p1 - p0).cross(chord));
  const double chordLengthSquared = chord.lengthSquared();
  if (cross * cross <= kFlatnessTolerance * kFlatnessTolerance * chordLengthSquared) {
    appendPoint(out, p2);
    return;
  }

  // de Casteljau at the midpoint: two halves that each have their own control point.
  const DPoint a = (p0 + p1) * 0.5;
  const DPoint b = (p1 + p2) * 0.5;
  const DPoint mid = (a + b) * 0.5;

  flattenQuad(p0, a, mid, depth + 1, out);
  flattenQuad(mid, b, p2, depth + 1, out);
}

/** Flattens a cubic by recursive subdivision. See @c flattenQuad for the tolerance argument. */
void flattenCubic(const DPoint& p0, const DPoint& p1, const DPoint& p2, const DPoint& p3, const int depth,
        std::vector<DPoint>& out) {
  if (depth >= kMaxSubdivisionDepth) {
    appendPoint(out, p3);
    return;
  }

  // A cubic is flat enough when both control points are close to the chord. Testing the two separately
  // rather than the sum matters for an S-shaped curve, whose control points lie on opposite sides and
  // would otherwise cancel.
  const DPoint chord = p3 - p0;
  const double cross1 = std::fabs((p1 - p0).cross(chord));
  const double cross2 = std::fabs((p2 - p0).cross(chord));
  const double worst = std::max(cross1, cross2);
  const double chordLengthSquared = chord.lengthSquared();
  // The 3/4 factor is the cubic's deviation-to-control-distance ratio, as 1/2 is the quad's.
  if (worst * worst <= (3.0 / 4.0) * kFlatnessTolerance * kFlatnessTolerance * chordLengthSquared) {
    appendPoint(out, p3);
    return;
  }

  const DPoint a = (p0 + p1) * 0.5;
  const DPoint b = (p1 + p2) * 0.5;
  const DPoint c = (p2 + p3) * 0.5;
  const DPoint d = (a + b) * 0.5;
  const DPoint e = (b + c) * 0.5;
  const DPoint mid = (d + e) * 0.5;

  flattenCubic(p0, a, d, mid, depth + 1, out);
  flattenCubic(mid, e, c, p3, depth + 1, out);
}

/**
 * @return the parameters in (0,1) at which a cubic's coordinate function has a stationary point.
 *
 * The derivative of a cubic in Bernstein form is a quadratic, and its roots are where the curve turns
 * around in that axis. These are the points a bounds computation has to look at: the endpoints and
 * the extrema bound the curve, while the control points only bound it loosely.
 */
void cubicExtrema(const double p0, const double p1, const double p2, const double p3, std::vector<double>& out) {
  // B'(t) = 3[(p1-p0)(1-t)^2 + 2(p2-p1)(1-t)t + (p3-p2)t^2], a quadratic in t.
  const double a = 3.0 * (-p0 + 3.0 * p1 - 3.0 * p2 + p3);
  const double b = 6.0 * (p0 - 2.0 * p1 + p2);
  const double c = 3.0 * (p1 - p0);

  if (std::fabs(a) < 1e-12) {
    // Degenerates to a linear derivative, which has at most one root.
    if (std::fabs(b) > 1e-12) {
      const double t = -c / b;
      if (t > 0.0 && t < 1.0) out.push_back(t);
    }
    return;
  }

  const double discriminant = b * b - 4.0 * a * c;
  if (discriminant < 0.0) return;
  const double root = std::sqrt(discriminant);
  for (const double t: { (-b + root) / (2.0 * a), (-b - root) / (2.0 * a) }) {
    if (t > 0.0 && t < 1.0) out.push_back(t);
  }
}

/** Quad variant of @c cubicExtrema. */
void quadExtrema(const double p0, const double p1, const double p2, std::vector<double>& out) {
  // B'(t) = 2[(p1-p0)(1-t) + (p2-p1)t], linear in t.
  const double denominator = p0 - 2.0 * p1 + p2;
  if (std::fabs(denominator) < 1e-12) return;
  const double t = (p0 - p1) / denominator;
  if (t > 0.0 && t < 1.0) out.push_back(t);
}

} // namespace anonymous

// ==================================================================================================
// DPoint and DRect
// ==================================================================================================

double DPoint::length() const noexcept { return std::sqrt(lengthSquared()); }

DPoint DPoint::normalized() const noexcept {
  const double len = length();
  if (!(len > 0.0)) return { 0.0, 0.0 };
  return { x / len, y / len };
}

bool DPoint::isFinite() const noexcept { return finite(x) && finite(y); }

DRect DRect::MakeEmpty() noexcept {
  // The inverted form, so that `unionWith` on an empty rectangle returns the point unchanged. Using
  // zeroes would make the empty rectangle claim to contain the origin.
  constexpr double inf = std::numeric_limits<double>::infinity();
  return { inf, inf, -inf, -inf };
}

void DRect::unionWith(const DPoint& p) noexcept {
  if (!p.isFinite()) return;
  left = std::min(left, p.x);
  top = std::min(top, p.y);
  right = std::max(right, p.x);
  bottom = std::max(bottom, p.y);
}

void DRect::unionWith(const DRect& other) noexcept {
  // Only a rectangle with no location at all contributes nothing. A point or line rectangle is a real
  // bound and must be merged, which is why this tests `isDegenerate` and not `isEmpty`.
  if (other.isDegenerate()) return;
  left = std::min(left, other.left);
  top = std::min(top, other.top);
  right = std::max(right, other.right);
  bottom = std::max(bottom, other.bottom);
}

DRect DRect::outsetBy(const double margin) const noexcept {
  if (isEmpty()) return *this;
  return { left - margin, top - margin, right + margin, bottom + margin };
}

Rect DRect::toRect() const noexcept {
  return Rect::MakeLTRB(static_cast<Scalar>(left), static_cast<Scalar>(top), static_cast<Scalar>(right),
          static_cast<Scalar>(bottom));
}

// ==================================================================================================
// PathSegment
// ==================================================================================================

PathSegment PathSegment::makeMove(const DPoint& to) noexcept {
  PathSegment s;
  s.verb = Verb::Move;
  s.points[0] = to;
  s.pointCount = 1;
  return s;
}

PathSegment PathSegment::makeLine(const DPoint& to) noexcept {
  PathSegment s;
  s.verb = Verb::Line;
  s.points[0] = to;
  s.pointCount = 1;
  return s;
}

PathSegment PathSegment::makeQuad(const DPoint& control, const DPoint& to) noexcept {
  PathSegment s;
  s.verb = Verb::Quad;
  s.points[0] = control;
  s.points[1] = to;
  s.pointCount = 2;
  return s;
}

PathSegment PathSegment::makeCubic(const DPoint& control1, const DPoint& control2, const DPoint& to) noexcept {
  PathSegment s;
  s.verb = Verb::Cubic;
  s.points[0] = control1;
  s.points[1] = control2;
  s.points[2] = to;
  s.pointCount = 3;
  return s;
}

PathSegment PathSegment::makeClose() noexcept {
  PathSegment s;
  s.verb = Verb::Close;
  s.pointCount = 0;
  return s;
}

DPoint PathSegment::endPoint() const noexcept {
  if (pointCount == 0) return {};
  return points[pointCount - 1];
}

DPoint PathSegment::eval(const DPoint& start, const double t) const noexcept {
  switch (verb) {
    case Verb::Move:
    case Verb::Line:
      return start + (points[0] - start) * t;

    case Verb::Quad: {
      // Bernstein form. Evaluated directly rather than by de Casteljau because this is called with a
      // known t far more often than it is called in a subdivision loop, and the direct form is fewer
      // operations for the same result within a few ulps.
      const double u = 1.0 - t;
      return start * (u * u) + points[0] * (2.0 * u * t) + points[1] * (t * t);
    }

    case Verb::Cubic: {
      const double u = 1.0 - t;
      const double uu = u * u;
      const double tt = t * t;
      return start * (uu * u) + points[0] * (3.0 * uu * t) + points[1] * (3.0 * u * tt) + points[2] * (tt * t);
    }

    case Verb::Close:
      return start;

    case Verb::Done:
      break;
  }
  return start;
}

DPoint PathSegment::derivative(const DPoint& start, const double t) const noexcept {
  switch (verb) {
    case Verb::Move:
    case Verb::Line:
    case Verb::Close:
      return points[0] - start;

    case Verb::Quad: {
      const double u = 1.0 - t;
      return (points[0] - start) * (2.0 * u) + (points[1] - points[0]) * (2.0 * t);
    }

    case Verb::Cubic: {
      const double u = 1.0 - t;
      return (points[0] - start) * (3.0 * u * u) + (points[1] - points[0]) * (6.0 * u * t) +
             (points[2] - points[1]) * (3.0 * t * t);
    }

    case Verb::Done:
      break;
  }
  return {};
}

// ==================================================================================================
// Path
// ==================================================================================================

void Path::moveTo(const Point& p) {
  mSegments.push_back(PathSegment::makeMove(DPoint::From(p)));
}

void Path::lineTo(const Point& p) { mSegments.push_back(PathSegment::makeLine(DPoint::From(p))); }

void Path::quadTo(const Point& control, const Point& p) {
  mSegments.push_back(PathSegment::makeQuad(DPoint::From(control), DPoint::From(p)));
}

void Path::cubicTo(const Point& control1, const Point& control2, const Point& p) {
  mSegments.push_back(
          PathSegment::makeCubic(DPoint::From(control1), DPoint::From(control2), DPoint::From(p)));
}

void Path::close() { mSegments.push_back(PathSegment::makeClose()); }

void Path::reset() { mSegments.clear(); }

void Path::addPath(const Path& other) {
  // A contour has to start with a Move or it is not a contour. If this path already has segments and
  // the appended one starts with a Move, the two stay separate, which is what a caller means by
  // "add this path".
  mSegments.insert(mSegments.end(), other.mSegments.begin(), other.mSegments.end());
}

DRect Path::controlBounds() const noexcept {
  DRect bounds = DRect::MakeEmpty();
  for (const PathSegment& segment: mSegments) {
    for (uint8_t i = 0; i < segment.pointCount; ++i) {
      bounds.unionWith(segment.points[i]);
    }
  }
  return bounds;
}

DRect Path::tightBounds() const noexcept {
  DRect bounds = DRect::MakeEmpty();

  DPoint contourStart {};
  DPoint current {};
  bool open = false;

  for (const PathSegment& segment: mSegments) {
    if (segment.verb == Verb::Move) {
      current = segment.points[0];
      contourStart = current;
      bounds.unionWith(current);
      open = true;
      continue;
    }
    if (!open) continue;

    switch (segment.verb) {
      case Verb::Line:
        bounds.unionWith(segment.points[0]);
        break;

      case Verb::Quad: {
        // The endpoints plus any interior extremum. A quadratic's curve is contained in the triangle
        // of its control points, so this is exact and not an approximation.
        std::vector<double> ts;
        quadExtrema(current.x, segment.points[0].x, segment.points[1].x, ts);
        quadExtrema(current.y, segment.points[0].y, segment.points[1].y, ts);
        bounds.unionWith(current);
        bounds.unionWith(segment.points[1]);
        for (const double t: ts) bounds.unionWith(segment.eval(current, t));
        break;
      }

      case Verb::Cubic: {
        std::vector<double> ts;
        cubicExtrema(current.x, segment.points[0].x, segment.points[1].x, segment.points[2].x, ts);
        cubicExtrema(current.y, segment.points[0].y, segment.points[1].y, segment.points[2].y, ts);
        bounds.unionWith(current);
        bounds.unionWith(segment.points[2]);
        for (const double t: ts) bounds.unionWith(segment.eval(current, t));
        break;
      }

      case Verb::Close:
        bounds.unionWith(contourStart);
        current = contourStart;
        continue;

      case Verb::Move:
      case Verb::Done:
        continue;
    }

    current = segment.endPoint();
  }

  return bounds;
}

namespace {

/** Flattens one whole path into polylines, one vector per contour. */
std::vector<std::vector<DPoint>> flattenContours(const Path& path) {
  std::vector<std::vector<DPoint>> contours;

  DPoint contourStart {};
  DPoint current {};
  bool open = false;

  for (const PathSegment& segment: path.segments()) {
    if (segment.verb == Verb::Move) {
      // A contour that was open when this Move arrived is finished; start a new one.
      if (open && contours.back().size() < 2) contours.pop_back();
      current = segment.points[0];
      contourStart = current;
      contours.emplace_back();
      contours.back().push_back(current);
      open = true;
      continue;
    }
    if (!open) continue;

    switch (segment.verb) {
      case Verb::Line:
        appendPoint(contours.back(), segment.points[0]);
        break;

      case Verb::Quad:
        flattenQuad(current, segment.points[0], segment.points[1], 0, contours.back());
        break;

      case Verb::Cubic:
        flattenCubic(current, segment.points[0], segment.points[1], segment.points[2], 0, contours.back());
        break;

      case Verb::Close:
        // An explicit close returns to the start, which is what makes the polygon closed for the
        // containment test below. Appending the start point again is harmless: the crossing count
        // treats the final edge as running from the last point back to the first.
        current = contourStart;
        continue;

      case Verb::Move:
      case Verb::Done:
        continue;
    }

    current = segment.endPoint();
  }

  return contours;
}

} // namespace anonymous

bool Path::contains(const Point& p) const noexcept {
  const DPoint query = DPoint::From(p);
  const auto contours = flattenContours(*this);

  // The standard ray-crossing test, casting toward +x from the query point. Only the x coordinate
  // needs a division, and the half-open y comparison is what stops a vertex exactly on the ray from
  // being counted twice -- the classic source of a point flipping between inside and outside as it
  // moves along a horizontal line.
  int winding = 0;
  int crossings = 0;

  for (const auto& contour: contours) {
    if (contour.size() < 2) continue;

    for (size_t i = 0; i < contour.size(); ++i) {
      const DPoint& a = contour[i];
      const DPoint& b = contour[(i + 1) % contour.size()];

      if (a.y == b.y) continue;
      // Half-open in y: the lower end counts, the upper does not.
      const bool aBelow = a.y <= query.y;
      const bool bBelow = b.y <= query.y;
      if (aBelow == bBelow) continue;

      // x of the crossing, solved from the parametric form so that no slope is computed.
      const double t = (query.y - a.y) / (b.y - a.y);
      const double x = a.x + t * (b.x - a.x);
      if (x <= query.x) continue;

      ++crossings;
      if (b.y > a.y) {
        ++winding;
      } else {
        --winding;
      }
    }
  }

  return mFillRule == FillRule::EvenOdd ? (crossings % 2) != 0 : winding != 0;
}

Path Path::reversed() const {
  Path result;
  result.mFillRule = mFillRule;

  // Contours are collected first so that each can be emitted backwards; a path's segments are a flat
  // stream, so reversing cannot be done in place without knowing where each contour begins and ends.
  std::vector<DPoint> contourStartStack;
  struct Contour {
    DPoint start;
    std::vector<PathSegment> segments;
    bool closed = false;
  };
  std::vector<Contour> contours;
  DPoint current {};
  bool open = false;

  for (const PathSegment& segment: mSegments) {
    if (segment.verb == Verb::Move) {
      contours.push_back({ segment.points[0], {}, false });
      current = segment.points[0];
      open = true;
      continue;
    }
    if (!open) continue;

    if (segment.verb == Verb::Close) {
      contours.back().closed = true;
      current = contours.back().start;
      continue;
    }
    contours.back().segments.push_back(segment);
    current = segment.endPoint();
  }

  (void) contourStartStack;

  for (const Contour& contour: contours) {
    if (contour.segments.empty()) {
      result.moveTo(contour.start.toPoint());
      continue;
    }

    // Walking a contour backwards: the points of each segment are emitted in reverse order, and each
    // segment's *start* becomes the previous segment's end. A cubic reversed is the same cubic with
    // its control points swapped, which is why reversing is exact rather than a re-fit.
    std::vector<DPoint> knots;
    knots.push_back(contour.start);
    for (const PathSegment& segment: contour.segments) knots.push_back(segment.endPoint());

    result.moveTo(knots.back().toPoint());
    for (size_t i = contour.segments.size(); i-- > 0;) {
      const PathSegment& segment = contour.segments[i];
      switch (segment.verb) {
        case Verb::Line:
          result.lineTo(knots[i].toPoint());
          break;
        case Verb::Quad:
          // Reversed: the same control point, endpoints swapped.
          result.quadTo(segment.points[0].toPoint(), knots[i].toPoint());
          break;
        case Verb::Cubic:
          result.cubicTo(segment.points[1].toPoint(), segment.points[0].toPoint(), knots[i].toPoint());
          break;
        default:
          break;
      }
    }
    if (contour.closed) result.close();
  }

  return result;
}

} // namespace graphics::pathops
