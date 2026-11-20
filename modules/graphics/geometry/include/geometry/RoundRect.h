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
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CORAL_GEOMETRY_ROUND_RECT_H
#define CORAL_GEOMETRY_ROUND_RECT_H

#include <geometry/Point.h>
#include <geometry/Rect.h>
#include <geometry/RoundingRadii.h>
#include <geometry/Size.h>

namespace graphics {

class RoundRect {
public:
  RoundRect() = default;

  static RoundRect MakeRect(const Rect& rect) {
    return MakeRectRadii(rect, RoundingRadii());
  }

  static RoundRect MakeOval(const Rect& rect) {
    return MakeRectRadii(rect, RoundingRadii::MakeRadii(rect.getSize() * 0.5f));
  }

  static RoundRect MakeRectRadius(const Rect& rect, const Scalar radius) {
    return MakeRectRadii(rect, RoundingRadii::MakeRadius(radius));
  }

  static RoundRect MakeRectXY(const Rect& rect, const Scalar xRadius, const Scalar yRadius) {
    return MakeRectRadii(rect, RoundingRadii::MakeRadii(Size(xRadius, yRadius)));
  }

  static RoundRect MakeRectXY(const Rect& rect, const Size& conerRadii) {
    return MakeRectRadii(rect, RoundingRadii::MakeRadii(conerRadii));
  }

  static RoundRect MakeRectRadii(const Rect& rect, const RoundingRadii& radii);

  constexpr const Rect& getBounds() const { return mBounds; }

  constexpr const RoundingRadii& getRadii() const { return mRadii; }

  [[nodiscard]] constexpr bool isFinite() const {
    return mBounds.isFinite() &&
           mRadii.top_left.isFinite() &&
           mRadii.top_right.isFinite() &&
           mRadii.bottom_left.isFinite() &&
           mRadii.bottom_right.isFinite();
  }

  [[nodiscard]] constexpr bool isEmpty() const {
    return mBounds.isEmpty();
  }

  [[nodiscard]] constexpr bool isRect() const {
    return !mBounds.isEmpty() && mRadii.areAllCornersEmpty();
  }

  [[nodiscard]] constexpr bool isOval() const {
    return !mBounds.isEmpty() &&
           mRadii.areAllCornersSame() &&
           ScalarNearlyEqual(mRadii.top_left.width, mBounds.getWidth() * 0.5f) &&
           ScalarNearlyEqual(mRadii.top_left.height, mBounds.getHeight() * 0.5f);
  }

  /**
   * Returns true iff the provided point |p| is inside the half-open interior of this rectangle.
   *
   * For purposes of containment, a rectangle contains points along the top and left edges but
   * not points along the right and bottom edges so that a point is only ever considered inside
   * one of two abutting rectangles.
   */
  [[nodiscard]] bool contains(const Point& p) const;

  /**
   * Returns a new round rectangle translated by the given offset.
   */
  [[nodiscard]] RoundRect shift(const Scalar dx, const Scalar dy) const {
    // Just in case, use the factory rather than the internal constructor
    // as shifting the rectangle may increase/decrease its bit precision
    // so we should re-validate the radii to the newly located rectangle.
    return MakeRectRadii(mBounds.shift(dx, dy), mRadii);
  }

  /**
   * Returns a round rectangle with expanded edges. Negative expansion results in shrinking.
   */
  [[nodiscard]] RoundRect getExpand(
    const Scalar left,
    const Scalar top,
    const Scalar right,
    const Scalar bottom) const {
    // Use the factory rather than the internal constructor as the changing
    // size of the rectangle requires that we re-validate the radii to the
    // newly sized rectangle.
    return MakeRectRadii(mBounds.getExpand(left, top, right, bottom), mRadii);
  }

  /**
   * Returns a round rectangle with expanded edges. Negative expansion results in shrinking.
   */
  [[nodiscard]] RoundRect getExpand(const Scalar horizontal, const Scalar vertical) const {
    // Use the factory rather than the internal constructor as the changing
    // size of the rectangle requires that we re-validate the radii to the
    // newly sized rectangle.
    return MakeRectRadii(mBounds.getExpand(horizontal, vertical), mRadii);
  }

  [[nodiscard]] RoundRect getExpand(const Scalar amount) const {
    // Use the factory rather than the internal constructor as the changing
    // size of the rectangle requires that we re-validate the radii to the
    // newly sized rectangle.
    return MakeRectRadii(mBounds.getExpand(amount), mRadii);
  }

  friend bool operator==(const RoundRect& lhs, const RoundRect& rhs) noexcept {
    return lhs.mBounds == rhs.mBounds && lhs.mRadii == rhs.mRadii;
  }

  friend bool operator!=(const RoundRect& lhs, const RoundRect& rhs) noexcept {
    return !(lhs == rhs);
  }

private:
  constexpr RoundRect(const Rect& rect, const RoundingRadii& radii)
    : mBounds(rect), mRadii(radii) {
  }

  Rect mBounds;
  RoundingRadii mRadii;
};

} // namespace graphics

#endif //CORAL_GEOMETRY_ROUND_RECT_H
