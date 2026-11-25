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

#ifndef CORAL_GRAPHICS_PATHOPS_CURVE_H
#define CORAL_GRAPHICS_PATHOPS_CURVE_H

#include <geometry/Point.h>
#include <geometry/Rect.h>
#include <geometry/Scalar.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

/**
 * @file Curve.h
 *
 * The curve primitives the path operations are built from, in double precision.
 *
 * ## Why double, when the path itself is float
 *
 * `graphics::Point` holds floats, because a path is a thing a user writes and a float is what fits in
 * one. Everything here is double.
 *
 * The reason is the operation, not the data. Deciding whether two curves cross, and where, is a
 * root-finding problem on numbers that are frequently almost equal -- two edges of a shape that a user
 * drew to meet at a corner, or an intersection that sits within a rounding error of an endpoint. In
 * float, the discriminant of that solve loses most of its significant digits exactly when the answer
 * matters most, and the failure mode is not a slightly wrong coordinate: it is a crossing that is
 * missed, or found twice, and a boolean operation that then produces a path with a gap or a doubled
 * edge.
 *
 * Carrying double here and narrowing once at the end is the same trade skcms makes for its matrices,
 * and for the same reason.
 *
 * ## Relationship to the reference
 *
 * This reimplements the logic of the extracted `pathops` module rather than translating it. The types
 * are named for what they are (`Line`, `Quad`, `Cubic` rather than `SkDLine`, `SkDQuad`, `SkDCubic`),
 * the out-parameters that existed because C cannot return two values are return values, and the curve
 * operations are functions on the types rather than member functions that mutate shared state.
 */

namespace graphics::pathops {

/** A point in the double-precision space the operations work in. */
struct DPoint {
  double x = 0.0;
  double y = 0.0;

  [[nodiscard]] static constexpr DPoint Make(double x, double y) noexcept { return { x, y }; }

  /** Widens a path coordinate into the working space. */
  [[nodiscard]] static constexpr DPoint From(const Point& p) noexcept {
    return { static_cast<double>(p.x), static_cast<double>(p.y) };
  }

  /** Narrows back to a path coordinate. */
  [[nodiscard]] constexpr Point toPoint() const noexcept {
    return Point { static_cast<Scalar>(x), static_cast<Scalar>(y) };
  }

  [[nodiscard]] constexpr DPoint operator+(const DPoint& o) const noexcept { return { x + o.x, y + o.y }; }
  [[nodiscard]] constexpr DPoint operator-(const DPoint& o) const noexcept { return { x - o.x, y - o.y }; }
  [[nodiscard]] constexpr DPoint operator*(double s) const noexcept { return { x * s, y * s }; }

  constexpr DPoint& operator+=(const DPoint& o) noexcept {
    x += o.x;
    y += o.y;
    return *this;
  }

  [[nodiscard]] constexpr bool operator==(const DPoint& o) const noexcept { return x == o.x && y == o.y; }
  [[nodiscard]] constexpr bool operator!=(const DPoint& o) const noexcept { return !(*this == o); }

  /** An ordering, so points can be sorted for the sweep the operations use. */
  [[nodiscard]] constexpr bool lessThan(const DPoint& o) const noexcept {
    return x != o.x ? x < o.x : y < o.y;
  }

  [[nodiscard]] constexpr double dot(const DPoint& o) const noexcept { return x * o.x + y * o.y; }
  [[nodiscard]] constexpr double cross(const DPoint& o) const noexcept { return x * o.y - y * o.x; }
  [[nodiscard]] double lengthSquared() const noexcept { return x * x + y * y; }
  [[nodiscard]] double length() const noexcept;

  /** @return this point moved to the unit vector in its own direction, or (0,0) when it is the origin. */
  [[nodiscard]] DPoint normalized() const noexcept;

  /** @return true when both coordinates are finite. NaN propagates into every later comparison. */
  [[nodiscard]] bool isFinite() const noexcept;
};

/** A double-precision rectangle. */
struct DRect {
  double left = 0.0;
  double top = 0.0;
  double right = 0.0;
  double bottom = 0.0;

  [[nodiscard]] static DRect MakeLTRB(double left, double top, double right, double bottom) noexcept {
    return { left, top, right, bottom };
  }

  /** The rectangle that contains nothing, in the form that makes `unionWith` work on the first call. */
  [[nodiscard]] static DRect MakeEmpty() noexcept;

  [[nodiscard]] constexpr double width() const noexcept { return right - left; }
  [[nodiscard]] constexpr double height() const noexcept { return bottom - top; }

  /**
   * @return true when this rectangle contains no area.
   *
   * Note that a single point, and a line, are "no area" -- so this is *not* the test for "the bounds
   * computation found nothing". @c isDegenerate is that test. The two were the same predicate until a
   * path consisting of one `moveTo` reported no bounds at all, which is a real answer being thrown
   * away: the point is where the path is.
   */
  [[nodiscard]] constexpr bool isEmpty() const noexcept { return !(left < right && top < bottom); }

  /** @return true when the rectangle has collapsed to a point or a line, so it has no area but does
   *          have a location. */
  [[nodiscard]] constexpr bool isDegenerate() const noexcept {
    return (right < left || bottom < top);
  }

  [[nodiscard]] constexpr bool contains(const DPoint& p) const noexcept {
    return p.x >= left && p.x <= right && p.y >= top && p.y <= bottom;
  }

  [[nodiscard]] constexpr bool intersects(const DRect& o) const noexcept {
    return left <= o.right && o.left <= right && top <= o.bottom && o.top <= bottom;
  }

  void unionWith(const DPoint& p) noexcept;
  void unionWith(const DRect& other) noexcept;

  [[nodiscard]] DRect outsetBy(double margin) const noexcept;

  /** Narrows to the facade's rectangle type. */
  [[nodiscard]] Rect toRect() const noexcept;
};

/** Which kind of segment a verb introduces. */
enum class Verb : uint8_t {
  Move,
  Line,
  Quad,
  Cubic,
  /** Closes back to the contour's start. Carries no points of its own. */
  Close,
  /** Ends the path. Not stored; a path is a vector and knows its own length. */
  Done,
};

/**
 * One segment of a path: a verb and the points it consumes.
 *
 * A flat representation rather than a class hierarchy. Curves in a path are not polymorphic -- they
 * are iterated in order, transformed in bulk and compared by value -- so a tag plus a fixed-size point
 * array is both smaller and cheaper than three types behind a pointer.
 */
struct PathSegment {
  Verb verb = Verb::Move;
  /** Control points: 1 for a line (the end point), 2 for a quad, 3 for a cubic, 0 for close. */
  std::array<DPoint, 3> points {};
  uint8_t pointCount = 0;

  [[nodiscard]] static PathSegment makeMove(const DPoint& to) noexcept;
  [[nodiscard]] static PathSegment makeLine(const DPoint& to) noexcept;
  [[nodiscard]] static PathSegment makeQuad(const DPoint& control, const DPoint& to) noexcept;
  [[nodiscard]] static PathSegment makeCubic(
          const DPoint& control1, const DPoint& control2, const DPoint& to) noexcept;
  [[nodiscard]] static PathSegment makeClose() noexcept;

  /** @return the segment's end point, which for a close is the contour's start. */
  [[nodiscard]] DPoint endPoint() const noexcept;

  /** @return the segment evaluated at @p t, where t=0 is the start and t=1 the end. */
  [[nodiscard]] DPoint eval(const DPoint& start, double t) const noexcept;

  /** @return the derivative at @p t, undefined at a cusp but usable for a tangent. */
  [[nodiscard]] DPoint derivative(const DPoint& start, double t) const noexcept;
};

/**
 * How a path decides which side of it is filled.
 *
 * The two rules disagree whenever a shape overlaps itself, which is exactly when a boolean operation
 * has to choose. Keeping the fill rule on the path rather than as an argument to every operation
 * matches how a caller thinks about it: the rule is a property of the shape.
 */
enum class FillRule : uint8_t {
  /** A point is inside when a ray from it crosses the path a non-zero number of times, counting
   *  direction. Overlapping same-direction contours stay filled. */
  Winding,
  /** A point is inside when a ray crosses an odd number of times. Any overlap cancels. */
  EvenOdd,
};

/**
 * A sequence of contours.
 *
 * One contour is a `Move` followed by segments and an optional `Close`; the next contour starts at the
 * next `Move`. A path with no `Move` at all is empty, which is the natural representation of "nothing"
 * and saves a special case for it.
 */
class Path {
public:
  Path() = default;

  void moveTo(const Point& p);
  void lineTo(const Point& p);
  void quadTo(const Point& control, const Point& p);
  void cubicTo(const Point& control1, const Point& control2, const Point& p);
  void close();

  /** Appends @p other's segments, shifted so its contours do not merge with this path's. */
  void addPath(const Path& other);

  void reset();

  [[nodiscard]] bool isEmpty() const noexcept { return mSegments.empty(); }
  [[nodiscard]] const std::vector<PathSegment>& segments() const noexcept { return mSegments; }

  /**
   * Mutable access, for a caller that transforms the points in place.
   *
   * Exposed rather than replaced with a `transform` method because a transformation is not the only thing
   * a caller might do -- reversing runs backwards through this, and the operations that will land later
   * rewrite individual points -- and one accessor serves all of them. The segments remain well formed
   * because a caller may only change coordinates, never the verbs or the point counts.
   */
  [[nodiscard]] std::vector<PathSegment>& segments() noexcept { return mSegments; }

  [[nodiscard]] FillRule fillRule() const noexcept { return mFillRule; }
  void setFillRule(FillRule rule) noexcept { mFillRule = rule; }

  /**
   * @return the bounds of the control points, which is a superset of the curve's own bounds.
   *
   * Fast, and the right thing for a conservative test or a culling decision. Callers that need the
   * bounds of the drawn shape -- a selection rectangle, a damage region -- want @c tightBounds, which
   * solves for the extrema and is more work.
   */
  [[nodiscard]] DRect controlBounds() const noexcept;

  /**
   * @return the bounds of the curves themselves, computed from the extrema of each segment.
   *
   * A cubic's control points can lie far outside the curve it draws, so the control bounds of a path
   * can be much larger than the shape. This is the answer a caller usually means by "bounds".
   */
  [[nodiscard]] DRect tightBounds() const noexcept;

  /** @return true when @p p is inside the path under @c fillRule. */
  [[nodiscard]] bool contains(const Point& p) const noexcept;

  /** @return the path reversed, so that each contour runs backwards. */
  [[nodiscard]] Path reversed() const;

private:
  std::vector<PathSegment> mSegments;
  FillRule mFillRule = FillRule::Winding;
};

} // namespace graphics::pathops

#endif // CORAL_GRAPHICS_PATHOPS_CURVE_H
