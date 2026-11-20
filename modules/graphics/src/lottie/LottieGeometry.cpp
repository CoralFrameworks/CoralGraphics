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
 * implied. See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file LottieGeometry.cpp
 *
 * The path builders declared in LottieGeometry.h, translated from Skottie's `Rectangle.cpp`, `Ellipse.cpp` and
 * `Polystar.cpp`.
 *
 * Two things are true of all three and worth saying once rather than three times:
 *
 *   * the shapes are described around a **centre**, not a corner -- `p` is the middle, and the bounds are the centre
 *     plus and minus half of `s`;
 *   * a shape with no extent produces an **empty** path rather than a degenerate one. A zero-width rectangle drawn as a
 *     path is a line, and a filled line is a hairline in the wrong place; the format means "do not draw this".
 */

#include "lottie/LottieGeometry.h"

#include <algorithm>
#include <cmath>

namespace graphics::lottie {
namespace {

constexpr Scalar kPi = 3.14159265358979323846f;

/**
 * The constant that makes four cubic segments approximate a circle.
 *
 * `4/3 * (sqrt(2) - 1)`: where each control point sits along the tangent, as a fraction of the radius. A quarter circle
 * built this way is off by about one part in a thousand, which no rasteriser shows at a sane scale -- and it is what
 * every vector format uses, so the error belongs to the format rather than to this file.
 */
constexpr Scalar kCircleConstant = 0.5522847498f;

/** @return the point at @p angle on the circle of radius @p radius about @p centre. */
Point onCircle(const Point centre, const Scalar radius, const Scalar angle) {
  return Point {centre.x + radius * std::cos(angle), centre.y + radius * std::sin(angle)};
}

Scalar toRadians(const Scalar degrees) { return degrees * kPi / 180.0f; }

/**
 * One piece of a rounded rectangle: a straight or a corner, with the points at both ends.
 *
 * Kept as a list with `from` and `to` recorded rather than written out as a sequence of calls, because the direction has
 * to be reversible. Reversing a hand-written sequence means rewriting it, and a reversed corner is exactly the kind of
 * thing that comes out subtly wrong instead of obviously wrong.
 */
struct Segment {
  /** A corner is a quadratic; a straight is a line. */
  bool corner = false;
  Point from;
  Point to;
  /** A corner's control point, which for a rounded rectangle is the corner itself. */
  Point control;
};

/**
 * @return a rectangle with its corners rounded by @p radius, wound in the direction @p counterClockwise asks for.
 *
 * Clockwise is the format's default. Both directions come out of the same eight segments: reversing a quadratic keeps
 * its control point and swaps its ends, so walking the list backwards is all a counter-clockwise rectangle needs.
 */
Path roundedRectangle(const Rect bounds, const Scalar radius, const bool counterClockwise) {
  const Scalar left = bounds.getLeft();
  const Scalar top = bounds.getTop();
  const Scalar right = bounds.getRight();
  const Scalar bottom = bounds.getBottom();

  // The radius cannot exceed half of either side: beyond that two corners would overlap and the path would fold through
  // itself, which fills as a shape with a notch rather than as a rounded rectangle.
  const Scalar limit = std::min(bounds.getWidth(), bounds.getHeight()) / 2;
  const Scalar r = std::clamp(radius, 0.0f, std::max(0.0f, limit));

  if (!(r > 0)) {
    // A plain rectangle, drawn as four lines. A rounded one with a zero radius would be eight degenerate segments, which
    // the rasteriser would then have to special-case.
    Path path;
    if (counterClockwise) {
      path.moveTo(Point {left, top});
      path.lineTo(Point {left, bottom});
      path.lineTo(Point {right, bottom});
      path.lineTo(Point {right, top});
    } else {
      path.moveTo(Point {left, top});
      path.lineTo(Point {right, top});
      path.lineTo(Point {right, bottom});
      path.lineTo(Point {left, bottom});
    }
    path.close();
    return path;
  }

  // Clockwise, starting at the top edge's left end: top edge, top-right corner, right edge, bottom-right corner, bottom
  // edge, bottom-left corner, left edge, top-left corner. Eight pieces that close back on the start.
  const Segment segments[8] = {
          {false, {left + r, top}, {right - r, top}, {}},
          {true, {right - r, top}, {right, top + r}, {right, top}},
          {false, {right, top + r}, {right, bottom - r}, {}},
          {true, {right, bottom - r}, {right, bottom}, {right, bottom}},
          {false, {right - r, bottom}, {left + r, bottom}, {}},
          {true, {left + r, bottom}, {left, bottom - r}, {left, bottom}},
          {false, {left, bottom - r}, {left, top + r}, {}},
          {true, {left, top + r}, {left + r, top}, {left, top}},
  };

  Path path;
  const auto emit = [&path](const Segment& segment, const bool reversed) {
    const Point target = reversed ? segment.from : segment.to;
    if (segment.corner) {
      // A quadratic reverses without touching its control point, which is why one control per corner is enough for both
      // directions.
      path.quadTo(segment.control, target);
    } else {
      path.lineTo(target);
    }
  };

  path.moveTo(segments[0].from);
  if (counterClockwise) {
    for (int i = 7; i >= 0; --i) emit(segments[i], true);
  } else {
    for (const Segment& segment : segments) emit(segment, false);
  }
  path.close();
  return path;
}

} // namespace anonymous

Path geometryPath(const Geometry& geometry, const Scalar frame) {
  switch (geometry.kind) {
    case Geometry::Kind::Rectangle: {
      const Point size = geometry.size.isEmpty() ? Point {0, 0} : geometry.size.at(frame);
      const Point centre = geometry.position.isEmpty() ? Point {0, 0} : geometry.position.at(frame);
      if (!(size.x > 0) || !(size.y > 0)) return {};

      const Rect bounds = Rect::MakeXYWH(centre.x - size.x / 2, centre.y - size.y / 2, size.x, size.y);
      const Scalar radius = geometry.roundness.isEmpty() ? 0.0f : geometry.roundness.at(frame);
      return roundedRectangle(bounds, radius, geometry.counterClockwise);
    }

    case Geometry::Kind::Ellipse: {
      const Point size = geometry.size.isEmpty() ? Point {0, 0} : geometry.size.at(frame);
      const Point centre = geometry.position.isEmpty() ? Point {0, 0} : geometry.position.at(frame);
      if (!(size.x > 0) || !(size.y > 0)) return {};

      // Four cubics, one per quadrant. The control points are the axis intercepts pushed out by the circle constant,
      // scaled per axis -- which is what makes it an ellipse rather than a distorted circle.
      const Scalar rx = size.x / 2;
      const Scalar ry = size.y / 2;
      const Scalar cx = rx * kCircleConstant;
      const Scalar cy = ry * kCircleConstant;
      const Scalar x = centre.x;
      const Scalar y = centre.y;

      Path path;
      // From the right of the centre, travelling clockwise -- the format's direction.
      path.moveTo(Point {x + rx, y});
      path.cubicTo(Point {x + rx, y + cy}, Point {x + cx, y + ry}, Point {x, y + ry});
      path.cubicTo(Point {x - cx, y + ry}, Point {x - rx, y + cy}, Point {x - rx, y});
      path.cubicTo(Point {x - rx, y - cy}, Point {x - cx, y - ry}, Point {x, y - ry});
      path.cubicTo(Point {x + cx, y - ry}, Point {x + rx, y - cy}, Point {x + rx, y});
      path.close();
      return path;
    }

    case Geometry::Kind::Polystar: {
      // Skottie rounds the count and clamps it, and builds nothing at all for zero -- an empty or unbounded loop
      // otherwise. The ceiling is its own, and keeps a corrupt document from asking for a million vertices.
      constexpr int kMaxPointCount = 100000;
      const Scalar raw = geometry.pointCount.isEmpty() ? 0.0f : geometry.pointCount.at(frame);
      const int count = std::clamp(static_cast<int>(std::lround(raw)), 0, kMaxPointCount);
      if (count <= 0) return {};

      const Point centre = geometry.position.isEmpty() ? Point {0, 0} : geometry.position.at(frame);
      const Scalar outer = geometry.outerRadius.isEmpty() ? 0.0f : geometry.outerRadius.at(frame);
      const Scalar inner = geometry.innerRadius.isEmpty() ? 0.0f : geometry.innerRadius.at(frame);
      if (!(outer > 0)) return {};

      const Scalar arc = 2 * kPi / static_cast<Scalar>(count);

      // With no rotation the first point sits straight up. The format measures angles from the positive x axis but
      // places that first point at the top, which is where the quarter turn comes from.
      Scalar angle = toRadians(geometry.rotation.isEmpty() ? 0.0f : geometry.rotation.at(frame)) - kPi / 2;

      Path path;
      path.moveTo(onCircle(centre, outer, angle));

      for (int i = 0; i < count; ++i) {
        if (geometry.star) {
          // Half a step round to the inner point, which is what alternates a star's points instead of lining them up.
          path.lineTo(onCircle(centre, inner, angle + arc * 0.5f));
        }
        angle += arc;
        path.lineTo(onCircle(centre, outer, angle));
      }
      path.close();
      return path;
    }
  }
  return {};
}

} // namespace graphics::lottie
