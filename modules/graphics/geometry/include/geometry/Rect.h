/*
 * Copyright (C) 2025 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CORAL_GEOMETRY_RECT_H
#define CORAL_GEOMETRY_RECT_H

#include <geometry/Matrix.h>
#include <geometry/Point.h>
#include <geometry/Scalar.h>
#include <geometry/Size.h>
#include <geometry/saturated_math.h>

#include <array>
#include <optional>

namespace graphics {

/**
 * Templated struct for holding an axis-aligned rectangle.
 */
template<class T>
struct TRect {
private:
  using Type = T;

public:
  constexpr TRect() : mLeft(0), mTop(0), mRight(0), mBottom(0) {
  }

  constexpr static TRect MakeLTRB(Type left, Type top, Type right, Type bottom) {
    return TRect(left, top, right, bottom);
  }

  constexpr static TRect MakeXYWH(Type x, Type y, Type width, Type height) {
    return TRect(x, y, saturated::Add(x, width), saturated::Add(y, height));
  }

  constexpr static TRect MakeWH(Type width, Type height) {
    return TRect(0, 0, width, height);
  }

  constexpr static TRect MakeOriginSize(const TPoint<Type>& origin, const TSize<Type>& size) {
    return MakeXYWH(origin.x, origin.y, size.width, size.height);
  }

  template<class U>
  constexpr static TRect MakeSize(const TSize<U>& size) {
    return TRect(0.0, 0.0, size.width, size.height);
  }

  /**
   * Construct a floating point rect |Rect| from another Rect of a potentially different storage type (e.g. |IRect|).
   */
  template<class U, class FT = T>
  constexpr static std::enable_if_t<std::is_floating_point_v<FT>, TRect> Make(const TRect<U>& rect) {
    return MakeLTRB(
      static_cast<FT>(rect.getLeft()), static_cast<FT>(rect.getTop()),
      static_cast<FT>(rect.getRight()), static_cast<FT>(rect.getBottom()));
  }

  template<typename U>
  constexpr static std::optional<TRect> MakePointBounds(const U& value) {
    return MakePointBounds(value.begin(), value.end());
  }

  template<typename PointIter>
  constexpr static std::optional<TRect> MakePointBounds(const PointIter first, const PointIter last) {
    if (first == last) {
      return std::nullopt;
    }
    auto left = first->x;
    auto top = first->y;
    auto right = first->x;
    auto bottom = first->y;
    for (auto it = first + 1; it < last; ++it) {
      left = std::min(left, it->x);
      top = std::min(top, it->y);
      right = std::max(right, it->x);
      bottom = std::max(bottom, it->y);
    }
    return TRect::MakeLTRB(left, top, right, bottom);
  }

  [[nodiscard]] constexpr static TRect MakeMaximum() {
    return TRect::MakeLTRB(std::numeric_limits<Type>::lowest(),
      std::numeric_limits<Type>::lowest(),
      std::numeric_limits<Type>::max(),
      std::numeric_limits<Type>::max());
  }

  [[nodiscard]] constexpr bool operator==(const TRect& r) const {
    return mLeft == r.mLeft && mTop == r.mTop && mRight == r.mRight && mBottom == r.mBottom;
  }

  [[nodiscard]] constexpr bool operator!=(const TRect& r) const {
    return !(*this == r);
  }

  [[nodiscard]] constexpr TRect scale(Type newScale) const {
    return TRect(mLeft * newScale, mTop * newScale, mRight * newScale, mBottom * newScale);
  }

  [[nodiscard]] constexpr TRect scale(Type scale_x, Type scale_y) const {
    return TRect(mLeft * scale_x, mTop * scale_y, mRight * scale_x, mBottom * scale_y);
  }

  [[nodiscard]] constexpr TRect scale(TPoint<T> newScale) const {
    return scale(newScale.x, newScale.y);
  }

  [[nodiscard]] constexpr TRect scale(TSize<T> newScale) const {
    return scale(newScale.width, newScale.height);
  }

  /**
   * Returns true if the provided point |p| is inside the half-open interior of this rectangle.
   *
   * For purposes of containment, a rectangle contains points along the top and left edges but
   * not points along the right and bottom edges so that a point is only ever considered inside
   * one of two abutting rectangles.
   */
  [[nodiscard]] constexpr bool contains(const TPoint<Type>& p) const {
    return !isEmpty() && p.x >= mLeft && p.y >= mTop && p.x < mRight && p.y < mBottom;
  }

  /**
   * Returns true if the provided point |p| is inside the closed-range interior of this rectangle.
   *
   * Unlike the regular |Contains(TPoint)| method, this method considers all points along boundary
   * of the rectangle to be contained within the rectangle - useful for testing if vertices that define
   * a filled shape would carry the interior of that shape outside the bounds of the rectangle.
   * Since both geometries are defining half-open spaces, their defining geometry needs to consider
   * their boundaries to be equivalent with respect to interior and exterior.
   */
  [[nodiscard]] constexpr bool containsInclusive(const TPoint<Type>& p) const {
    return !isEmpty() &&
           p.x >= mLeft &&
           p.y >= mTop &&
           p.x <= mRight &&
           p.y <= mBottom;
  }

  /**
   * Returns true if this rectangle is not empty, and it also contains every point considered
   * inside the provided rectangle |o| (as determined by |Contains(TPoint)|).
   *
   * This is similar to a definition where the result is true if the union of the two rectangles
   * is equal to this rectangle, ignoring precision issues with performing those operations and
   * assuming that empty rectangles are never equal.
   *
   * An empty rectangle can contain no other rectangle.
   *
   * An empty rectangle is, however, contained within any other non-empy rectangle as the set of
   * points it contains is an empty set and so there are no points to fail the containment criteria.
   */
  [[nodiscard]] constexpr bool contains(const TRect& other) const {
    return !isEmpty() &&
           (other.isEmpty() ||
            (other.mLeft >= mLeft &&
             other.mTop >= mTop &&
             other.mRight <= mRight &&
             other.mBottom <= mBottom));
  }

  /**
   * Returns true if all the fields of this floating point rectangle are finite.
   *
   * Note that the results of |getWidth()| and |getHeight()| may still be infinite
   * due to overflow even if the fields themselves are finite.
   */
  template<typename U = T>
  constexpr std::enable_if_t<std::is_floating_point_v<U>, bool> isFinite() const {
    return std::isfinite(mLeft) && std::isfinite(mTop) && std::isfinite(mRight) && std::isfinite(mBottom);
  }

  /**
   * Returns true if either of the width or height are 0, negative, or NaN.
   */
  [[nodiscard]] constexpr bool isEmpty() const {
    // Computing the non-empty condition and negating the result causes any
    // NaN value to return true - i.e. is considered empty.
    return !(mLeft < mRight && mTop < mBottom);
  }

  /**
   * Returns true if width and height are equal and neither is NaN.
   */
  [[nodiscard]] constexpr bool isSquare() const {
    // empty rectangles can technically be "square", but would be
    // misleading to most callers. Using |isEmpty| also prevents
    // "non-empty and non-overflowing" computations from happening
    // to be equal to "empty and overflowing" results.
    // (Consider LTRB(10, 15, MAX-2, MIN+2) which is empty, but both
    //  w/h subtractions equal "5").
    return !isEmpty() && (mRight - mLeft) == (mBottom - mTop);
  }

  [[nodiscard]] constexpr bool isMaximum() const {
    return *this == MakeMaximum();
  }

  /**
   * Returns the upper left corner of the rectangle as specified by the left/top or x/y values
   * when it was constructed.
   */
  [[nodiscard]] constexpr TPoint<Type> getOrigin() const {
    return { mLeft, mTop };
  }

  /**
   * Returns the size of the rectangle which may be negative in either width or height and may have
   * been clipped to the maximum integer values for integer rects whose size overflows.
   */
  [[nodiscard]] constexpr TSize<Type> getSize() const {
    return { getWidth(), getHeight() };
  }

  /**
   * Returns the X coordinate of the upper left corner, equivalent to |getOrigin().x|
   */
  [[nodiscard]] constexpr Type getX() const { return mLeft; }

  /**
   * Returns the Y coordinate of the upper left corner, equivalent to |getOrigin().y|
   */
  [[nodiscard]] constexpr Type getY() const { return mTop; }

  /**
   * Returns the width of the rectangle, equivalent to |getSize().width|
   */
  [[nodiscard]] constexpr Type getWidth() const {
    return saturated::Sub(mRight, mLeft);
  }

  /**
   * Returns the height of the rectangle, equivalent to |getSize().height|
   */
  [[nodiscard]] constexpr Type getHeight() const {
    return saturated::Sub(mBottom, mTop);
  }

  [[nodiscard]] constexpr auto getLeft() const { return mLeft; }

  [[nodiscard]] constexpr auto getTop() const { return mTop; }

  [[nodiscard]] constexpr auto getRight() const { return mRight; }

  [[nodiscard]] constexpr auto getBottom() const { return mBottom; }

  [[nodiscard]] constexpr TPoint<T> getLeftTop() const {
    return { mLeft, mTop };
  }

  [[nodiscard]] constexpr TPoint<T> getRightTop() const {
    return { mRight, mTop };
  }

  [[nodiscard]] constexpr TPoint<T> getLeftBottom() const {
    return { mLeft, mBottom };
  }

  [[nodiscard]] constexpr TPoint<T> getRightBottom() const {
    return { mRight, mBottom };
  }

  /**
   * Gets the area of the rectangle, equivalent to getSize().getArea()
   */
  [[nodiscard]] constexpr T getArea() const {
    // TODO(141710): Use saturated math to avoid overflow.
    return isEmpty() ? 0 : (mRight - mLeft) * (mBottom - mTop);
  }

  /**
   * Gets the center point as a Point.
   */
  [[nodiscard]] constexpr Point getCenter() const {
    return {
      saturated::AverageScalar(mLeft, mRight),
      saturated::AverageScalar(mTop, mBottom) };
  }

  [[nodiscard]] constexpr std::array<T, 4> getLTRB() const {
    return { mLeft, mTop, mRight, mBottom };
  }

  /**
   * Gets the x, y coordinates of the origin and the width and height of the rectangle in an array.
   */
  [[nodiscard]] constexpr std::array<T, 4> getXYWH() const {
    return { mLeft, mTop, getWidth(), getHeight() };
  }

  /**
   * Gets a version of this rectangle that has a non-negative size.
   */
  [[nodiscard]] constexpr TRect getPositive() const {
    if (!isEmpty()) {
      return *this;
    }
    return {
      std::min(mLeft, mRight),
      std::min(mTop, mBottom),
      std::max(mLeft, mRight),
      std::max(mTop, mBottom),
    };
  }

  /**
   * Gets the points that represent the 4 corners of this rectangle in a Z order that is compatible with
   * triangle strips or a set of all zero points if the rectangle is empty.
   * The order is: Top left, top right, bottom left, bottom right.
   */
  [[nodiscard]] constexpr std::array<TPoint<T>, 4> getPoints() const {
    if (isEmpty()) {
      return {};
    }
    return {
      TPoint{ mLeft, mTop },
      TPoint{ mRight, mTop },
      TPoint{ mLeft, mBottom },
      TPoint{ mRight, mBottom },
    };
  }

  [[nodiscard]] constexpr std::array<TPoint<T>, 4> getTransformedPoints(const Matrix& transform) const {
    auto points = getPoints();
    for (size_t i = 0; i < points.size(); i++) {
      points[i] = transform * points[i];
    }
    return points;
  }

  /**
   * Creates a new bounding box that contains this transformed rectangle,
   * clipped against the near clipping plane if necessary.
   */
  [[nodiscard]] constexpr TRect transformAndClipBounds(const Matrix& transform) const {
    if (!transform.hasPerspective2D()) {
      return transformBounds(transform);
    }

    if (isEmpty()) {
      return {};
    }

    auto ul = transform.transformHomogenous({ mLeft, mTop });
    auto ur = transform.transformHomogenous({ mRight, mTop });
    auto ll = transform.transformHomogenous({ mLeft, mBottom });
    auto lr = transform.transformHomogenous({ mRight, mBottom });

    // It can probably be proven that we only ever have 5 points at most
    // which happens when only 1 corner is clipped, and we get 2 points
    // in return for it as we interpolate against its neighbors.
    Point points[8];
    int index = 0;

    // Process (clip and interpolate) each point against its 2 neighbors:
    //                                 left, pt, right
    index = ClipAndInsert(points, index, ll, ul, ur);
    index = ClipAndInsert(points, index, ul, ur, lr);
    index = ClipAndInsert(points, index, ur, lr, ll);
    index = ClipAndInsert(points, index, lr, ll, ul);

    auto bounds = TRect::MakePointBounds(points, points + index);
    return bounds.value_or(TRect{});
  }

  /**
   * Creates a new bounding box that contains this transformed rectangle.
   */
  [[nodiscard]] constexpr TRect transformBounds(const Matrix& transform) const {
    if (isEmpty()) {
      return {};
    }
    auto points = getTransformedPoints(transform);
    auto bounds = TRect::MakePointBounds(points.begin(), points.end());
    if (bounds.has_value()) {
      return bounds.value();
    }
    return {};
  }

  /**
   * Constructs a Matrix that will map all points in the coordinate space of the rectangle into
   * a new normalized coordinate space where the upper left corner of the rectangle maps to (0, 0)
   * and the lower right corner of the rectangle maps to (1, 1).
   *
   * Empty and non-finite rectangles will return a zero-scaling transform that maps all points to (0, 0).
   */
  [[nodiscard]] constexpr Matrix getNormalizingTransform() const {
    if (!isEmpty()) {
      const Scalar sx = 1.0 / getWidth();
      const Scalar sy = 1.0 / getHeight();
      const Scalar tx = mLeft * -sx;
      const Scalar ty = mTop * -sy;

      // Exclude NaN and infinities and either scale underflow to zero
      if (sx != 0.0 && sy != 0.0 && 0.0 * sx * sy * tx * ty == 0.0) {
        // clang-format off
        return {
          sx,   0.0f, 0.0f, 0.0f,
          0.0f,   sy, 0.0f, 0.0f,
          0.0f, 0.0f, 1.0f, 0.0f,
          tx,     ty, 0.0f, 1.0f
        };
        // clang-format on
      }
    }

    // Map all coordinates to the origin.
    return Matrix::MakeScale({ 0.0f, 0.0f, 1.0f });
  }

  [[nodiscard]] constexpr TRect getUnion(const TRect& other) const {
    if (isEmpty()) {
      return other;
    }
    if (other.isEmpty()) {
      return *this;
    }
    return {
      std::min(mLeft, other.mLeft),
      std::min(mTop, other.mTop),
      std::max(mRight, other.mRight),
      std::max(mBottom, other.mBottom),
    };
  }

  [[nodiscard]] constexpr std::optional<TRect> getIntersection(const TRect& other) const {
    if (isIntersectsWithRect(other)) {
      return TRect{
        std::max(mLeft, other.mLeft),
        std::max(mTop, other.mTop),
        std::min(mRight, other.mRight),
        std::min(mBottom, other.mBottom),
      };
    }
    return std::nullopt;
  }

  [[nodiscard]] constexpr TRect getIntersectionOrEmpty(const TRect& other) const {
    return getIntersection(other).value_or(TRect());
  }

  [[nodiscard]] constexpr bool isIntersectsWithRect(const TRect& other) const {
    return !isEmpty() &&
           !other.isEmpty() &&
           mLeft < other.mRight &&
           mTop < other.mBottom &&
           mRight > other.mLeft &&
           mBottom > other.mTop;
  }

  /**
   * Returns the new boundary rectangle that would result from this rectangle being cut out by
   * the specified rectangle.
   */
  [[nodiscard]] constexpr std::optional<TRect<T>> getCutout(const TRect& other) const {
    if (isEmpty()) {
      // This test isn't just a short-circuit, it also prevents the concise
      // math below from returning the wrong answer on empty rects.
      // Once we know that this rectangle is not empty, the math below can
      // only succeed in computing a value if o is also non-empty and non-nan.
      // Otherwise, the method returns *this by default.
      return std::nullopt;
    }

    const auto& [a_left, a_top, a_right, a_bottom] = getLTRB(); // Source rect.
    const auto& [b_left, b_top, b_right, b_bottom] = other.getLTRB(); // Cutout.
    if (b_left <= a_left && b_right >= a_right) {
      if (b_top <= a_top && b_bottom >= a_bottom) {
        // Full cutout.
        return std::nullopt;
      }
      if (b_top <= a_top && b_bottom > a_top) {
        // Cuts off the top.
        return TRect::MakeLTRB(a_left, b_bottom, a_right, a_bottom);
      }
      if (b_bottom >= a_bottom && b_top < a_bottom) {
        // Cuts off the bottom.
        return TRect::MakeLTRB(a_left, a_top, a_right, b_top);
      }
    }
    if (b_top <= a_top && b_bottom >= a_bottom) {
      if (b_left <= a_left && b_right > a_left) {
        // Cuts off the left.
        return TRect::MakeLTRB(b_right, a_top, a_right, a_bottom);
      }
      if (b_right >= a_right && b_left < a_right) {
        // Cuts off the right.
        return TRect::MakeLTRB(a_left, a_top, b_left, a_bottom);
      }
    }

    return *this;
  }

  [[nodiscard]] constexpr TRect getCutoutOrEmpty(const TRect& other) const {
    return getCutout(other).value_or(TRect());
  }

  /**
   * Returns a new rectangle translated by the given offset.
   */
  [[nodiscard]] constexpr TRect<T> shift(T dx, T dy) const {
    return {
      saturated::Add(mLeft, dx),
      saturated::Add(mTop, dy),
      saturated::Add(mRight, dx),
      saturated::Add(mBottom, dy),
    };
  }

  /**
   * Returns a new rectangle translated by the given offset.
   */
  [[nodiscard]] constexpr TRect<T> shift(TPoint<T> offset) const {
    return shift(offset.x, offset.y);
  }

  /**
   * Returns a rectangle with expanded edges. Negative expansion results in shrinking.
   */
  [[nodiscard]] constexpr TRect<T> getExpand(T left, T top, T right, T bottom) const {
    return {
      saturated::Sub(mLeft, left),
      saturated::Sub(mTop, top),
      saturated::Add(mRight, right),
      saturated::Add(mBottom, bottom),
    };
  }

  /**
   * Returns a rectangle with expanded edges in all directions.
   *
   * Negative expansion results in shrinking.
   */
  [[nodiscard]] constexpr TRect<T> getExpand(T amount) const {
    return {
      saturated::Sub(mLeft, amount),
      saturated::Sub(mTop, amount),
      saturated::Add(mRight, amount),
      saturated::Add(mBottom, amount),
    };
  }

  /**
   * Returns a rectangle with expanded edges in all directions.
   *
   * Negative expansion results in shrinking.
   */
  [[nodiscard]] constexpr TRect<T> getExpand(T horizontal_amount, T vertical_amount) const {
    return {
      saturated::Sub(mLeft, horizontal_amount),
      saturated::Sub(mTop, vertical_amount),
      saturated::Add(mRight, horizontal_amount),
      saturated::Add(mBottom, vertical_amount),
    };
  }

  /**
   * Returns a rectangle with expanded edges in all directions.
   *
   * Negative expansion results in shrinking.
   */
  [[nodiscard]] constexpr TRect<T> getExpand(TPoint<T> amount) const {
    return getExpand(amount.x, amount.y);
  }

  /**
   * Returns a rectangle with expanded edges in all directions.
   *
   * Negative expansion results in shrinking.
   */
  [[nodiscard]] constexpr TRect<T> getExpand(TSize<T> amount) const {
    return getExpand(amount.width, amount.height);
  }

  /**
   * Returns a new rectangle that represents the projection of the source rectangle onto this rectangle.
   *
   * In other words, the source rectangle is redefined in terms of the coordinate space of this rectangle.
   */
  [[nodiscard]] constexpr TRect<T> getProjection(const TRect<T> source) const {
    if (isEmpty()) {
      return {};
    }
    return source.shift(-mLeft, -mTop)
                 .scale(1.0 / static_cast<Scalar>(getWidth()),
                   1.0 / static_cast<Scalar>(getHeight()));
  }

  template<typename U = T>
  [[nodiscard]] constexpr static std::enable_if_t<std::is_floating_point_v<U>, TRect> RoundOut(const TRect<U>& r) {
    return TRect::MakeLTRB(saturated::Cast<U, Type>(floor(r.getLeft())),
      saturated::Cast<U, Type>(floor(r.getTop())),
      saturated::Cast<U, Type>(ceil(r.getRight())),
      saturated::Cast<U, Type>(ceil(r.getBottom())));
  }

  template<typename U = T>
  [[nodiscard]] constexpr static std::enable_if_t<std::is_floating_point_v<U>, TRect> RoundIn(const TRect<U>& r) {
    return TRect::MakeLTRB(saturated::Cast<U, Type>(ceil(r.getLeft())),
      saturated::Cast<U, Type>(ceil(r.getTop())),
      saturated::Cast<U, Type>(floor(r.getRight())),
      saturated::Cast<U, Type>(floor(r.getBottom())));
  }

  template<typename U = T>
  [[nodiscard]] constexpr static std::enable_if_t<std::is_floating_point_v<U>, TRect> Round(const TRect<U>& r) {
    return TRect::MakeLTRB(saturated::Cast<U, Type>(round(r.getLeft())),
      saturated::Cast<U, Type>(round(r.getTop())),
      saturated::Cast<U, Type>(round(r.getRight())),
      saturated::Cast<U, Type>(round(r.getBottom())));
  }

  [[nodiscard]] constexpr static TRect Union(const TRect& a,
    const std::optional<TRect> b) {
    return b.has_value() ? a.getUnion(b.value()) : a;
  }

  [[nodiscard]] constexpr static TRect Union(const std::optional<TRect> a,
    const TRect& b) {
    return a.has_value() ? a->getUnion(b) : b;
  }

  [[nodiscard]] constexpr static std::optional<TRect> Union(
    const std::optional<TRect> a,
    const std::optional<TRect> b) {
    return a.has_value() ? Union(a.value(), b) : b;
  }

  [[nodiscard]] constexpr static std::optional<TRect> Intersection(
    const TRect& a,
    const std::optional<TRect> b) {
    return b.has_value() ? a.getIntersection(b.value()) : a;
  }

  [[nodiscard]] constexpr static std::optional<TRect> Intersection(
    const std::optional<TRect> a,
    const TRect& b) {
    return a.has_value() ? a->getIntersection(b) : b;
  }

  [[nodiscard]] constexpr static std::optional<TRect> Intersection(
    const std::optional<TRect> a,
    const std::optional<TRect> b) {
    return a.has_value() ? Intersection(a.value(), b) : b;
  }

private:
  constexpr TRect(Type left, Type top, Type right, Type bottom)
    : mLeft(left), mTop(top), mRight(right), mBottom(bottom) {
  }

  Type mLeft;
  Type mTop;
  Type mRight;
  Type mBottom;

  static constexpr Scalar kMinimumHomogenous = 1.0f / (1 << 14);

  // Clip p against the near clipping plane (W = kMinimumHomogenous)
  // and interpolate a crossing point against the nearby neighbors
  // left and right if p is clipped and either of them is not.
  // This method can produce 0, 1, or 2 points per call depending on
  // how many of the points are clipped.
  // 0 - all points are clipped
  // 1 - p is unclipped OR
  //     p is clipped and exactly one of the neighbors is not
  // 2 - p is clipped and both neighbors are not
  static constexpr int ClipAndInsert(Point clipped[],
    int index,
    const Vector3& left,
    const Vector3& p,
    const Vector3& right) {
    if (p.z >= kMinimumHomogenous) {
      clipped[index++] = { p.x / p.z, p.y / p.z };
    } else {
      index = InterpolateAndInsert(clipped, index, p, left);
      index = InterpolateAndInsert(clipped, index, p, right);
    }
    return index;
  }

  // Interpolate (a clipped) point p against one of its neighbors
  // and insert the point into the array where the line between them
  // veers from clipped space to unclipped, if such a point exists.
  static constexpr int InterpolateAndInsert(Point clipped[],
    int index,
    const Vector3& p,
    const Vector3& neighbor) {
    if (neighbor.z >= kMinimumHomogenous) {
      // t is the fraction along the segment [p, neighbor] at which the
      // homogeneous w reaches the near plane, so the crossing point is
      // (1 - t) * p + t * neighbor. Dividing by kMinimumHomogenous performs
      // the perspective divide of that crossing point.
      const auto t = (kMinimumHomogenous - p.z) / (neighbor.z - p.z);
      clipped[index++] = {
        ((1.0f - t) * p.x + t * neighbor.x) / kMinimumHomogenous,
        ((1.0f - t) * p.y + t * neighbor.y) / kMinimumHomogenous,
      };
    }
    return index;
  }
};

using Rect = TRect<Scalar>;
using IRect32 = TRect<int32_t>;
using IRect64 = TRect<int64_t>;
using IRect = IRect64;

template<class T>
std::ostream& operator<<(std::ostream& out, const TRect<T>& r) {
  out << "(" << r.getLeftTop() << " => " << r.getRightBottom() << ")";
  return out;
}

} // namespace graphics

#endif //CORAL_GEOMETRY_RECT_H
