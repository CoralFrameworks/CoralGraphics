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
 * @file LottieValue.cpp
 *
 * The easing solver.
 *
 * ## What the control points describe
 *
 * A Lottie keyframe's `o` and `i` are the two control points of a cubic Bezier from (0,0) to (1,1), with the endpoints
 * pinned. x is time and y is progress, so the curve *is* the timing function -- the same object as CSS's
 * `cubic-bezier()`, with the same meaning for its four numbers.
 *
 *     x(s) = 3(1-s)^2 s * o.x + 3(1-s) s^2 * i.x + s^3
 *     y(s) = 3(1-s)^2 s * o.y + 3(1-s) s^2 * i.y + s^3
 *
 * ## Why it has to be inverted
 *
 * A caller knows the time fraction t and wants the progress. The curve is parameterised by s, which is neither. So x(s) =
 * t has to be solved first, and s substituted into y. There is a closed form for a cubic, and using it would mean
 * evaluating a cube root of a discriminant that can be negative, for a curve the whole of which lies in a unit square --
 * Newton's method from a good starting guess converges in three or four iterations and cannot leave the interval.
 *
 * The starting guess is t itself. For the linear curve that is exactly right, so the common case costs one iteration
 * rather than the worst case.
 */

#include "lottie/LottieValue.h"

#include <algorithm>
#include <cmath>

namespace graphics::lottie {
namespace {

/**
 * How many Newton steps before falling back to bisection.
 *
 * Four is past the point where a unit-square cubic has converged to float precision, and the fallback is what keeps the
 * answer correct when the derivative is near zero -- a curve that is nearly vertical in x, which a designer produces by
 * dragging a handle almost straight up.
 */
constexpr int kNewtonIterations = 4;

/** The curve's x at parameter @p s. */
Scalar bezierX(const Scalar s, const Scalar x1, const Scalar x2) {
  const Scalar u = 1.0f - s;
  return 3.0f * u * u * s * x1 + 3.0f * u * s * s * x2 + s * s * s;
}

/** The curve's y at parameter @p s. */
Scalar bezierY(const Scalar s, const Scalar y1, const Scalar y2) {
  const Scalar u = 1.0f - s;
  return 3.0f * u * u * s * y1 + 3.0f * u * s * s * y2 + s * s * s;
}

/** The derivative of the curve's x at parameter @p s, which is what Newton's method steps by. */
Scalar bezierDX(const Scalar s, const Scalar x1, const Scalar x2) {
  const Scalar u = 1.0f - s;
  return 3.0f * u * u * x1 + 6.0f * u * s * (x2 - x1) + 3.0f * s * s * (1.0f - x2);
}

} // namespace anonymous

Scalar easeProgress(const Scalar t, const Point outTangent, const Point inTangent) {
  // Clamped rather than extrapolated: the caller is asking about a moment inside a segment, so a value outside the
  // interval means the segment it chose was wrong -- and a curve continued past its own ends produces a value the
  // document never described.
  const Scalar target = std::clamp(t, 0.0f, 1.0f);
  if (target <= 0.0f) return 0.0f;
  if (target >= 1.0f) return 1.0f;

  const Scalar x1 = outTangent.x;
  const Scalar y1 = outTangent.y;
  const Scalar x2 = inTangent.x;
  const Scalar y2 = inTangent.y;

  // The identity, which is what a linear keyframe is and what an exporter writes when nothing was eased. Answering it
  // directly is exact, where the numeric solve would leave a rounding error in the one case that should have none.
  if (x1 == y1 && x2 == y2) return target;

  Scalar s = target;
  for (int i = 0; i < kNewtonIterations; ++i) {
    const Scalar error = bezierX(s, x1, x2) - target;
    if (std::abs(error) < 1e-6f) return bezierY(s, y1, y2);

    const Scalar slope = bezierDX(s, x1, x2);
    // A vanishing slope is the case Newton's method cannot take a step from. Bisection always can, and the answer is
    // bracketed because the curve's x is monotone in s for control points inside the unit square.
    if (std::abs(slope) < 1e-6f) break;
    s = std::clamp(s - error / slope, 0.0f, 1.0f);
  }

  // Bisection to finish, on whatever the Newton steps left. Twenty-four halvings take the interval below float
  // precision, so this is exact rather than approximate.
  Scalar low = 0.0f;
  Scalar high = 1.0f;
  s = target;
  for (int i = 0; i < 24; ++i) {
    const Scalar middle = (low + high) * 0.5f;
    if (bezierX(middle, x1, x2) < target) {
      low = middle;
    } else {
      high = middle;
    }
  }
  s = (low + high) * 0.5f;
  return bezierY(s, y1, y2);
}

} // namespace graphics::lottie
