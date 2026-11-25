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

#include <geometry/RoundRect.h>

#include <utils/Assert.h>

namespace graphics {

RoundRect RoundRect::MakeRectRadii(const Rect& rect, const RoundingRadii& radii) {
  if (!rect.isFinite()) {
    return {};
  }
  const auto bounds = rect.getPositive();
  // RoundingRadii::scaled might return an empty radii if bounds or radii is
  // empty, which is expected. Pass along the bounds even if the radii is empty
  // as it would still have a valid location and/or 1-dimensional size which
  // might appear when stroked
  return RoundRect(bounds, radii.scaled(bounds));
}

// Determine if p is inside the elliptical corner curve defined by the
// indicated corner point and the indicated radii.
// p - is the test point in absolute coordinates
// corner - is the location of the associated corner in absolute coordinates
// direction - is the sign of (corner - center), or the sign of coordinates
//             as they move in the direction of the corner from inside the
//             rect ((-1,-1) for the upper left corner for instance)
// radii - the non-negative X and Y size of the corner's radii.
static bool CornerContains(const Point& p,
  const Point& corner,
  const Point& direction,
  const Size& radii) {
  CORAL_ASSERT(radii.width >= 0.0f && radii.height >= 0.0f);
  if (radii.isEmpty()) {
    // This corner is not curved, therefore the containment is the same as
    // the previously checked bounds containment.
    return true;
  }

  // The positive X,Y distance between the corner and the point.
  const Point corner_relative = (corner - p) * direction;

  // The distance from the "center" of the corner's elliptical curve.
  // If both numbers are positive then we need to do an elliptical distance
  // check to determine if it is inside the curve.
  // If either number is negative, then the point is outside this quadrant
  // and is governed by inclusion in the bounds and inclusion within other
  // corners of this round rect. In that case, we return true here to allow
  // further evaluation within other quadrants.
  const Point quadrant_relative = radii - corner_relative;
  if (quadrant_relative.x <= 0.0f || quadrant_relative.y <= 0.0f) {
    // Not within the curved quadrant of this corner, therefore "inside"
    // relative to this one corner.
    return true;
  }

  // Dividing the quadrant_relative point by the radii gives a corresponding
  // location within a unit circle which can be more easily tested for
  // containment. We can use x^2 + y^2 and compare it against the radius
  // squared (1.0) to avoid the sqrt.
  const Point quadrant_unit_circle_point = quadrant_relative / radii;
  return quadrant_unit_circle_point.getLengthSquared() <= 1.0;
}

// The sign of the direction that points move as they approach the indicated
// corner from within the rectangle.
static constexpr Point kUpperLeftDirection(-1.0f, -1.0f);
static constexpr Point kUpperRightDirection(1.0f, -1.0f);
static constexpr Point kLowerLeftDirection(-1.0f, 1.0f);
static constexpr Point kLowerRightDirection(1.0f, 1.0f);

bool RoundRect::contains(const Point& p) const {
  if (!mBounds.contains(p)) {
    return false;
  }
  if (!CornerContains(p, mBounds.getLeftTop(), kUpperLeftDirection, mRadii.top_left) ||
      !CornerContains(p, mBounds.getRightTop(), kUpperRightDirection, mRadii.top_right) ||
      !CornerContains(p, mBounds.getLeftBottom(), kLowerLeftDirection, mRadii.bottom_left) ||
      !CornerContains(p, mBounds.getRightBottom(), kLowerRightDirection, mRadii.bottom_right)) {
    return false;
  }
  return true;
}

} // namespace graphics
