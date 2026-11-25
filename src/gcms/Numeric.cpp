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

#include "gcms/Numeric.h"

namespace gcms {

std::optional<Matrix3x3> invert(const Matrix3x3& src) noexcept {
  // The arithmetic is done in double throughout even though the inputs and outputs are float. A 3x3
  // colour matrix is frequently close to singular -- the primaries of two similar gamuts give a
  // determinant near zero -- and accumulating the cofactors in float loses enough precision that an
  // invertible matrix is rejected or, worse, inverted inaccurately.
  const double a00 = src.values[0][0], a01 = src.values[1][0], a02 = src.values[2][0];
  const double a10 = src.values[0][1], a11 = src.values[1][1], a12 = src.values[2][1];
  const double a20 = src.values[0][2], a21 = src.values[1][2], a22 = src.values[2][2];

  double b0 = a00 * a11 - a01 * a10;
  double b1 = a00 * a12 - a02 * a10;
  double b2 = a01 * a12 - a02 * a11;
  double b3 = a20;
  double b4 = a21;
  double b5 = a22;

  const double determinant = b0 * b5 - b1 * b4 + b2 * b3;
  if (determinant == 0.0) return std::nullopt;

  const double invDet = 1.0 / determinant;
  // A determinant this small means the inverse is not representable as float, so returning one would
  // hand back infinities that silently destroy every colour that passes through it.
  if (invDet > +std::numeric_limits<float>::max() || invDet < -std::numeric_limits<float>::max() ||
          !isFinite(static_cast<float>(invDet))) {
    return std::nullopt;
  }

  b0 *= invDet;
  b1 *= invDet;
  b2 *= invDet;
  b3 *= invDet;
  b4 *= invDet;
  b5 *= invDet;

  // Note the transposed indices: the cofactors above are indexed by column of the source, so writing
  // them out in row-major order at the same positions would produce the transpose of the inverse.
  // `values[0][0]` below reads `vals[0][0]`, but `values[1][0]` reads `vals[0][1]` -- that is the
  // transpose, and it is intended.
  Matrix3x3 dst;
  dst.values[0][0] = static_cast<float>(a11 * b5 - a12 * b4);
  dst.values[1][0] = static_cast<float>(a02 * b4 - a01 * b5);
  dst.values[2][0] = static_cast<float>(+b2);
  dst.values[0][1] = static_cast<float>(a12 * b3 - a10 * b5);
  dst.values[1][1] = static_cast<float>(a00 * b5 - a02 * b3);
  dst.values[2][1] = static_cast<float>(-b1);
  dst.values[0][2] = static_cast<float>(a10 * b4 - a11 * b3);
  dst.values[1][2] = static_cast<float>(a01 * b3 - a00 * b4);
  dst.values[2][2] = static_cast<float>(+b0);

  for (const auto& row: dst.values) {
    for (const float value: row) {
      if (!isFinite(value)) return std::nullopt;
    }
  }
  return dst;
}

Matrix3x3 concat(const Matrix3x3& a, const Matrix3x3& b) noexcept {
  Matrix3x3 m;
  for (size_t row = 0; row < 3; ++row) {
    for (size_t col = 0; col < 3; ++col) {
      m.values[row][col] = a.values[row][0] * b.values[0][col] + a.values[row][1] * b.values[1][col] +
                           a.values[row][2] * b.values[2][col];
    }
  }
  return m;
}

std::array<float, 3> transform(const Matrix3x3& m, const std::array<float, 3>& v) noexcept {
  std::array<float, 3> dst = { 0.0f, 0.0f, 0.0f };
  for (size_t row = 0; row < 3; ++row) {
    dst[row] = m.values[row][0] * v[0] + m.values[row][1] * v[1] + m.values[row][2] * v[2];
  }
  return dst;
}

} // namespace gcms
