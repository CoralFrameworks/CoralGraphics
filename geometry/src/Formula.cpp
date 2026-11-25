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

#include <geometry/Formula.h>

#include <cmath>

namespace graphics {

namespace {

// Don't allow linearized segments to be off by more than 1/4th of a pixel from
// the true curve. This value should be scaled by the max basis of the
// X and Y directions.
constexpr static Scalar kPrecision = 4;

inline Scalar length(const Point n) {
  const Point nn = n * n;
  return std::sqrt(nn.x + nn.y);
}

} // namespace anonymous

Scalar Formula::ComputeCubicSubdivisions(
  const Scalar scaleFactor,
  const Point p0,
  const Point p1,
  const Point p2,
  const Point p3) {
  const Scalar k = scaleFactor * 0.75f * kPrecision;
  const Point a = (p0 - p1 * 2 + p2).abs();
  const Point b = (p1 - p2 * 2 + p3).abs();
  return std::sqrt(k * length(a.max(b)));
}

Scalar Formula::ComputeQuadraticSubdivisions(
  const Scalar scaleFactor,
  const Point p0,
  const Point p1,
  const Point p2) {
  const Scalar k = scaleFactor * 0.25f * kPrecision;
  return std::sqrt(k * length(p0 - p1 * 2 + p2));
}

Scalar Formula::ComputeConicSubdivisions(
  const Scalar scaleFactor,
  Point p0,
  Point p1,
  Point p2,
  const Scalar w) {

  // Compute center of bounding box in projected space
  const Point center = 0.5f * (p0.min(p1).min(p2) + p0.max(p1).max(p2));

  // Translate by -center.
  // This improves translation-invariance of the formula, see Sec. 3.3 of cited paper
  p0 -= center;
  p1 -= center;
  p2 -= center;

  // Compute max length
  const Scalar maxLength = std::sqrt(std::max(p0.dot(p0), std::max(p1.dot(p1), p2.dot(p2))));

  // Compute forward differences
  const Point dp = -2 * w * p1 + p0 + p2;
  const Scalar dw = std::abs(-2 * w + 2);

  // Compute numerator and denominator for parametric step size of linearization.
  // Here, the epsilon referenced from the cited paper is 1/precision.
  const Scalar k = scaleFactor * kPrecision;
  const Scalar rpMinus1 = std::max(0.0f, maxLength * k - 1);
  const Scalar numer = std::sqrt(dp.dot(dp)) * k + rpMinus1 * dw;
  const Scalar denom = 4 * std::min(w, 1.0f);

  // Number of segments = sqrt(numer / denom).
  // This assumes parametric interval of curve being linearized is [t0,t1] = [0, 1].
  // If not, the number of segments is (tmax - tmin) / sqrt(denom / numer).
  return std::sqrt(numer / denom);
}

} // namespace graphics
