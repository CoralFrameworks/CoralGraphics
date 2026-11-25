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
 * implied. See the License for the specific language governing permissions and limitations under
 * the License.
 */

#include "gcms/Gamut.h"

namespace gcms {
namespace {

/** @return true when @p x is a usable chromaticity coordinate. */
bool isCoordinate(const float x) noexcept { return x >= 0.0f && x <= 1.0f; }

} // namespace anonymous

std::optional<Matrix3x3> adaptToXYZD50(const float wx, const float wy) noexcept {
  if (!isCoordinate(wx) || !isCoordinate(wy) || wy == 0.0f) return std::nullopt;

  // The source white point in XYZ, normalised so that Y is 1. The same convention is used for D50
  // below, which is what makes the adaptation a pure chromatic one -- it changes the hue of the white
  // point without changing its luminance.
  const std::array<float, 3> whiteXYZ = { wx / wy, 1.0f, (1.0f - wx - wy) / wy };

  // D50 as the ICC specification defines it, again with Y normalised to 1.
  const std::array<float, 3> d50XYZ = { 0.96422f, 1.0f, 0.82521f };

  // Bradford cone response: XYZ to LMS. Both matrices are the published ones, written out rather than
  // computed, because they are the definition of "the Bradford method" rather than a derivation.
  const Matrix3x3 xyzToLms = Matrix3x3::fromRows(0.8951f, 0.2664f, -0.1614f, -0.7502f, 1.7135f, 0.0367f,
          0.0389f, -0.0685f, 1.0296f);
  const Matrix3x3 lmsToXyz = Matrix3x3::fromRows(0.9869929f, -0.1470543f, 0.1599627f, 0.4323053f, 0.5183603f,
          0.0492912f, -0.0085287f, 0.0400428f, 0.9684867f);

  const std::array<float, 3> srcCone = transform(xyzToLms, whiteXYZ);
  const std::array<float, 3> dstCone = transform(xyzToLms, d50XYZ);

  if (srcCone[0] == 0.0f || srcCone[1] == 0.0f || srcCone[2] == 0.0f) return std::nullopt;

  // Scale each cone channel independently by the ratio of the destination to the source. This diagonal
  // matrix *is* the adaptation; everything else here is the change of basis that lets it be expressed
  // in XYZ.
  Matrix3x3 scale;
  scale.values[0][0] = dstCone[0] / srcCone[0];
  scale.values[1][1] = dstCone[1] / srcCone[1];
  scale.values[2][2] = dstCone[2] / srcCone[2];

  // Concat composes as "apply the right operand first", so this reads right to left:
  // XYZ -> LMS -> adapt -> XYZ.
  return concat(lmsToXyz, concat(scale, xyzToLms));
}

std::optional<Matrix3x3> primariesToXYZD50(const float rx, const float ry, const float gx, const float gy,
        const float bx, const float by, const float wx, const float wy) noexcept {
  if (!isCoordinate(rx) || !isCoordinate(ry) || !isCoordinate(gx) || !isCoordinate(gy) || !isCoordinate(bx) ||
          !isCoordinate(by) || !isCoordinate(wx) || !isCoordinate(wy)) {
    return std::nullopt;
  }

  // Each primary's chromaticity turned into XYZ with Y left as an unknown: the column is the direction
  // of that primary, and the scaling factors that follow supply its magnitude.
  const Matrix3x3 chromaticities = Matrix3x3::fromRows(rx, gx, bx, ry, gy, by, 1.0f - rx - ry,
          1.0f - gx - gy, 1.0f - bx - by);

  const auto inverse = invert(chromaticities);
  if (!inverse) return std::nullopt;

  // Solving the chromaticity matrix for the white point gives the relative luminance each primary
  // contributes to white, which is exactly the scaling each column needs.
  const std::array<float, 3> whiteXYZ = { wx / wy, 1.0f, (1.0f - wx - wy) / wy };
  const std::array<float, 3> luminance = transform(*inverse, whiteXYZ);

  Matrix3x3 toXYZ;
  toXYZ.values[0][0] = luminance[0];
  toXYZ.values[1][1] = luminance[1];
  toXYZ.values[2][2] = luminance[2];

  const Matrix3x3 scaled = concat(chromaticities, toXYZ);

  const auto adaptation = adaptToXYZD50(wx, wy);
  if (!adaptation) return std::nullopt;

  return concat(*adaptation, scaled);
}

} // namespace gcms
