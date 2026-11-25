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

#include <geometry/Arc.h>

namespace graphics {

Arc::Arc(const Rect& bounds, Degrees start, Degrees sweep, const bool includeCenter)
  : mBounds(bounds), mIncludeCenter(includeCenter) {
  if (bounds.isFinite() && start.isFinite() && sweep.isFinite()) {
    if (sweep.degrees < 0) {
      start = start + sweep;
      sweep = -sweep;
    }
    if (sweep.degrees > 360) {
      // We need to represent over-sweeping a full circle for the case where
      // we will be stroking the circle with the center included where we
      // stroke the entire perimeter, but the two segments that connect to
      // the center are at the proper angles.
      // Normalize to less than 720.
      sweep.degrees = 360.0f + std::fmodf(sweep.degrees, 360.0f);
    }
  } else {
    // Don't bother sweeping any distance if anything is non-finite.
    sweep = Degrees(0);
    if (!start.isFinite() || !bounds.isFinite()) {
      // We can maintain start if both it and bounds are finite.
      start = Degrees(0);
    }
  }
  mStart = start;
  mSweep = sweep;
}

size_t Arc::Iteration::getPointCount() const {
  size_t count = 2;
  for (size_t i = 0; i < quadrant_count; i++) {
    count += quadrants[i].getPointCount();
  }
  return count;
}

Arc::Iteration Arc::ComputeCircleArcIterations(size_t stepCount) {
  return {
    { 1.0f, 0.0f },
    { 1.0f, 0.0f },
    4u,
    {
      { kQuadrantAxes[0], 1u, stepCount },
      { kQuadrantAxes[1], 0u, stepCount },
      { kQuadrantAxes[2], 0u, stepCount },
      { kQuadrantAxes[3], 0u, stepCount },
      { {}, 0u, 0u },
    },
  };
}

Rect Arc::getTightArcBounds() const {
  if (isFullCircle()) {
    return mBounds;
  }

  const Degrees startAngle = mStart.getPositive();
  const Degrees endAngle = startAngle + mSweep;
  CORAL_ASSERT(startAngle.degrees >= 0 && startAngle.degrees < 360);
  CORAL_ASSERT(endAngle > startAngle && endAngle.degrees < 720);

  // 1. start vector
  // 2. end vector
  // 3. optional center
  // 4-7. optional quadrant extrema
  Point extrema[7];
  int count = 0;

  extrema[count++] = Matrix::CosSin(startAngle);
  extrema[count++] = Matrix::CosSin(endAngle);

  if (includeCenter()) {
    extrema[count++] = { 0, 0 };
  }

  // curAxis will be pre-incremented before recording the following axis
  int curAxis = std::floor(startAngle.degrees / 90.0f);
  // endAxis is a non-inclusive end of the range
  int endAxis = std::ceil(endAngle.degrees / 90.0f);
  while (++curAxis < endAxis) {
    extrema[count++] = kQuadrantAxes[curAxis & 3];
  }

  CORAL_ASSERT(count <= 7);

  const auto center = mBounds.getCenter();
  const auto radii = mBounds.getSize() * 0.5f;

  for (int i = 0; i < count; i++) {
    extrema[i] = center + extrema[i] * radii;
  }
  return Rect::MakePointBounds(extrema, extrema + count).value_or(Rect());
}

Arc::Iteration Arc::computeIterations(size_t stepCount, const bool simplify_360) const {
  if (mSweep.degrees == 0) {
    return {};
  }

  CORAL_ASSERT(mSweep.degrees >= 0);

  if (simplify_360 && mSweep.degrees >= 360) {
    return ComputeCircleArcIterations(stepCount);
  }
  CORAL_ASSERT(mSweep.degrees < 720);

  const Degrees start = mStart.getPositive();
  const Degrees end = start + mSweep;
  CORAL_ASSERT(start.degrees >= 0.0f && start.degrees < 360.0f);
  CORAL_ASSERT(end >= start);
  CORAL_ASSERT(end.degrees < start.degrees + (simplify_360 ? 360.0f : 720.0f));

  Iteration iterations;
  iterations.start = Matrix::CosSin(start);
  iterations.end = Matrix::CosSin(end);

  // We nudge the start and stop by 1/10th of a step so we don't end
  // up with degenerately small steps at the start and end of the arc.
  const auto nudge = Degrees((90.0f / static_cast<Scalar>(stepCount)) * 0.1f);

  if ((start + nudge) >= (end - nudge)) {
    iterations.quadrant_count = 0u;
    return iterations;
  }

  auto curQuadrant = static_cast<int>(std::floor((start + nudge).degrees / 90.0f));
  const int endQuadrant = static_cast<int>(std::floor((end - nudge).degrees / 90.0f));
  CORAL_ASSERT(curQuadrant >= 0 && curQuadrant <= 4);
  CORAL_ASSERT(endQuadrant >= curQuadrant && endQuadrant <= curQuadrant + 8);
  CORAL_ASSERT(curQuadrant * 90 <= (start + nudge).degrees);
  CORAL_ASSERT(endQuadrant * 90 + 90 >= (end - nudge).degrees);

  auto nextStep = [stepCount](const Degrees angle, const int quadrant) -> size_t {
    const Scalar quadrantFract = angle.degrees / 90.0f - quadrant;
    return std::ceil(quadrantFract * stepCount);
  };

  int i = 0;
  iterations.quadrants[i] = {
    .axis = kQuadrantAxes[curQuadrant & 3],
    .start_index = nextStep(start + nudge, curQuadrant),
    .end_index = stepCount,
  };
  if (iterations.quadrants[0].end_index > iterations.quadrants[0].start_index) {
    i++;
  }
  while (curQuadrant < endQuadrant) {
    iterations.quadrants[i++] = {
      .axis = kQuadrantAxes[(++curQuadrant) % 4],
      .start_index = 0u,
      .end_index = stepCount,
    };
  }
  CORAL_ASSERT(i <= 9);
  if (i > 0) {
    iterations.quadrants[i - 1].end_index =
      nextStep(end - nudge, curQuadrant);
    if (iterations.quadrants[i - 1].end_index <=
        iterations.quadrants[i - 1].start_index) {
      i--;
    }
  }
  iterations.quadrant_count = i;
  return iterations;
}

} // namespace graphics
