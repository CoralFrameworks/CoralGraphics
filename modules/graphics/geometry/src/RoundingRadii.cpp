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

#include <geometry/RoundingRadii.h>

#include <utils/Assert.h>

namespace graphics {

static inline void NormalizeEmptyToZero(Size& radii) {
  if (radii.isEmpty()) {
    radii = Size();
  }
}

static inline void AdjustScale(Scalar& radius1,
    Scalar& radius2,
    Scalar dimension,
    Scalar& scale) {
  CORAL_ASSERT(radius1 >= 0.0f && radius2 >= 0.0f);
  CORAL_ASSERT(dimension > 0.0f);
  if (radius1 + radius2 > dimension) {
    scale = std::min(scale, dimension / (radius1 + radius2));
  }
}

RoundingRadii RoundingRadii::scaled(const Rect& in_bounds) const {
  Rect bounds = in_bounds.getPositive();
  if (bounds.isEmpty() ||  //
      areAllCornersEmpty() || !isFinite()) {
    // Normalize empty radii.
    return RoundingRadii();
  }

  // Copy the incoming radii so that we can work on normalizing them to the
  // particular rectangle they are paired with without disturbing the caller.
  RoundingRadii radii = *this;

  // If any corner is flat or has a negative value, normalize it to zeros
  // We do this first so that the unnecessary non-flat part of that radius
  // does not contribute to the global scaling below.
  NormalizeEmptyToZero(radii.top_left);
  NormalizeEmptyToZero(radii.top_right);
  NormalizeEmptyToZero(radii.bottom_left);
  NormalizeEmptyToZero(radii.bottom_right);

  // Now determine a global scale to apply to all of the radii to ensure
  // that none of the adjacent pairs of radius values sum to larger than
  // the corresponding dimension of the rectangle.
  Size size = bounds.getSize();
  Scalar scale = 1.0f;
  // clang-format off
  AdjustScale(radii.top_left.width, radii.top_right.width, size.width, scale);
  AdjustScale(radii.bottom_left.width, radii.bottom_right.width, size.width, scale);
  AdjustScale(radii.top_left.height, radii.bottom_left.height, size.height, scale);
  AdjustScale(radii.top_right.height, radii.bottom_right.height, size.height, scale);
  // clang-format on
  if (scale < 1.0f) {
    radii = radii * scale;
  }

  return radii;
}

} // namespace graphics
