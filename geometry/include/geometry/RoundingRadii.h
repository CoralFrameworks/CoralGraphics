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

#ifndef CORAL_GEOMETRY_ROUNDING_RADII_H
#define CORAL_GEOMETRY_ROUNDING_RADII_H

#include <geometry/Point.h>
#include <geometry/Rect.h>
#include <geometry/Size.h>

namespace graphics {

struct RoundingRadii {
  Size top_left;
  Size top_right;
  Size bottom_left;
  Size bottom_right;

  constexpr static RoundingRadii MakeRadius(const Scalar radius) {
    return { Size(radius), Size(radius), Size(radius), Size(radius) };
  }

  constexpr static RoundingRadii MakeRadii(const Size radii) {
    return { radii, radii, radii, radii };
  }

  constexpr static RoundingRadii MakeNinePatch(
    const Scalar left,
    const Scalar top,
    const Scalar right,
    const Scalar bottom) {
    return {
      .top_left = Size{ left, top },
      .top_right = Size{ right, top },
      .bottom_left = Size(left, bottom),
      .bottom_right = Size(right, bottom),
    };
  }

  [[nodiscard]] bool isFinite() const {
    return top_left.isFinite() &&
           top_right.isFinite() &&
           bottom_left.isFinite() &&
           bottom_right.isFinite();
  }

  [[nodiscard]] bool areAllCornersEmpty() const {
    return top_left.isEmpty() &&
           top_right.isEmpty() &&
           bottom_left.isEmpty() &&
           bottom_right.isEmpty();
  }

  [[nodiscard]] bool areAllCornersSame(const Scalar tolerance = kEhCloseEnough) const {
    return ScalarNearlyEqual(top_left.width, top_right.width, tolerance) &&
           ScalarNearlyEqual(top_left.width, bottom_right.width, tolerance) &&
           ScalarNearlyEqual(top_left.width, bottom_left.width, tolerance) &&
           ScalarNearlyEqual(top_left.height, top_right.height, tolerance) &&
           ScalarNearlyEqual(top_left.height, bottom_right.height, tolerance) &&
           ScalarNearlyEqual(top_left.height, bottom_left.height, tolerance);
  }

  /// @brief  Returns a scaled copy of this object, ensuring that the sum of the
  ///         corner radii on each side does not exceed the width or height of
  ///         the given bounds.
  ///
  ///         See the [Skia scaling
  ///         implementation](https://github.com/google/skia/blob/main/src/core/SkRRect.cpp)
  ///         for more details.
  [[nodiscard]] RoundingRadii scaled(const Rect& bounds) const;

  constexpr RoundingRadii operator*(const Scalar scale) const {
    return {
      .top_left = top_left * scale,
      .top_right = top_right * scale,
      .bottom_left = bottom_left * scale,
      .bottom_right = bottom_right * scale,
    };
  }

  [[nodiscard]] constexpr bool operator==(const RoundingRadii& rr) const {
    return top_left == rr.top_left &&
           top_right == rr.top_right &&
           bottom_left == rr.bottom_left &&
           bottom_right == rr.bottom_right;
  }

  [[nodiscard]] constexpr bool operator!=(const RoundingRadii& rr) const {
    return !(*this == rr);
  }
};

inline std::ostream& operator<<(std::ostream& out, const RoundingRadii& rr) {
  out << "("
    << "ul: " << rr.top_left << ", "
    << "ur: " << rr.top_right << ", "
    << "ll: " << rr.bottom_left << ", "
    << "lr: " << rr.bottom_right
    << ")";
  return out;
}

} // namespace graphics

#endif //CORAL_GEOMETRY_ROUNDING_RADII_H
