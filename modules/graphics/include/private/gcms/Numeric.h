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

#ifndef CORAL_GRAPHICS_GCMS_MATH_H
#define CORAL_GRAPHICS_GCMS_MATH_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>

/**
 * @file Numeric.h
 *
 * Scalar helpers and the two small matrix types the colour pipeline is built from.
 *
 * **On the name.** This was `Math.h` until it broke every build on Windows: the filesystem there is
 * case-insensitive, so `#include <math.h>` -- which `<cmath>` pulls in -- resolved to this file
 * instead of the C standard header, and the standard library failed to compile itself. The same trap
 * would catch any header here named after one in `<cmath>`, `<cstdio>`, `<cstring>`, `<cstdlib>`,
 * `<cstdint>`, `<limits>` or `<optional>`.
 *
 * ## Where this came from
 *
 * This module is a translation of skcms (Skia's colour management system) into modern C++, with the
 * `Sk`/`skcms_` vocabulary replaced by the `gcms` namespace. The translation is deliberate about what
 * it keeps and what it changes:
 *
 *  - **The numerical algorithms are kept.** The polynomial approximations for `pow`, `exp` and `log`
 *    below are the same ones skcms uses, because they are what makes colour conversion deterministic
 *    across platforms: `std::pow` is not required to be correctly rounded, so two machines can disagree
 *    on the last bit, and a shader that matches a CPU path bit for bit is worth more than a division
 *    that is one ulp more accurate.
 *  - **The shape is changed.** C arrays become `std::array`, out-parameters that only exist because C
 *    cannot return two values become return values, and the "returns bool, writes through a pointer"
 *    pattern becomes `std::optional`.
 */

namespace gcms {

/** The largest finite float, for the overflow cases the polynomial approximations detect by hand. */
inline constexpr float kInfinity = std::numeric_limits<float>::infinity();

/** One ulp below @p x, as a bit pattern. Used to make a range check exclude its endpoint exactly. */
[[nodiscard]] inline float minusOneUlp(float x) noexcept {
  static_assert(sizeof(float) == sizeof(int32_t), "float must be 32-bit for this bit trick");
  int32_t bits = 0;
  std::memcpy(&bits, &x, sizeof(bits));
  bits -= 1;
  std::memcpy(&x, &bits, sizeof(bits));
  return x;
}

/** @return true when @p x is finite, without pulling in the <cmath> classification machinery. */
[[nodiscard]] inline bool isFinite(float x) noexcept { return x * 0.0f == 0.0f; }

/**
 * A fast base-2 logarithm.
 *
 * The exponent field of the float *is* `floor(log2(x)) + 127`, which gives a first approximation for
 * free; the mantissa is then refined with a rational fit. The result is accurate to well under a
 * percent, which is ample for a transfer function whose inputs are already approximations of a
 * measured curve.
 */
[[nodiscard]] inline float log2Approx(float x) noexcept {
  int32_t bits = 0;
  std::memcpy(&bits, &x, sizeof(bits));

  const float e = static_cast<float>(bits) * (1.0f / (1 << 23));

  // Reusing the mantissa bits with the exponent forced to 0.5 gives a value in [0.5, 1), which is
  // where the rational fit below is valid.
  const int32_t mBits = (bits & 0x007fffff) | 0x3f000000;
  float m = 0.0f;
  std::memcpy(&m, &mBits, sizeof(m));

  return e - 124.225514990f - 1.498030302f * m - 1.725879990f / (0.3520887068f + m);
}

/** A fast base-2 exponential, the inverse of @c log2Approx. */
[[nodiscard]] inline float exp2Approx(float x) noexcept {
  if (x > 128.0f) return kInfinity;
  if (x < -127.0f) return 0.0f;

  const float fract = x - std::floor(x);

  // The same trick in reverse: build a bit pattern whose exponent and mantissa encode the result.
  const float fBits =
          1.0f * (1 << 23) * (x + 121.274057500f - 1.490129070f * fract + 27.728023300f / (4.84252568f - fract));

  // The check happens before the cast because a float this large is not representable as int32_t, and
  // converting it is undefined behaviour rather than a saturated value.
  if (fBits >= static_cast<float>(std::numeric_limits<int32_t>::max())) return kInfinity;
  if (fBits < 0.0f) return 0.0f;

  const int32_t bits = static_cast<int32_t>(fBits);
  std::memcpy(&x, &bits, sizeof(x));
  return x;
}

/**
 * @return @p x raised to @p y, clamped to zero for negative base.
 *
 * A transfer function applied to a negative encoded value has no real answer, and the colour pipeline
 * treats "no answer" as black rather than as a NaN that would propagate into every later stage.
 */
[[nodiscard]] inline float powApprox(float x, float y) noexcept {
  if (x <= 0.0f) return 0.0f;
  if (x == 1.0f) return 1.0f;
  return exp2Approx(log2Approx(x) * y);
}

/** A row-major 3x3 matrix, as `m[row][col]`. */
struct Matrix3x3 {
  /**
   * Zero-initialised rather than left indeterminate.
   *
   * `= {}` is the idiomatic way to zero an aggregate and, unlike spelling the nested braces out, it
   * cannot be got wrong: a nested `std::array` needs a brace count that reads as one too many, and
   * getting it wrong is a "too many initializers" error rather than anything informative.
   *
   * An all-zero matrix is at least a well-defined starting point; indeterminate floats would produce
   * NaNs that look like a maths bug rather than an initialisation bug.
   */
  std::array<std::array<float, 3>, 3> values = {};

  [[nodiscard]] static Matrix3x3 identity() noexcept {
    // Written out field by field on purpose. Aggregate-initialising a nested `std::array` needs a
    // brace count that is easy to get wrong and produces a confusing "too many initializers" when you
    // do, so the one place that needs a specific matrix spells it out.
    Matrix3x3 m;
    m.values[0][0] = 1.0f;
    m.values[1][1] = 1.0f;
    m.values[2][2] = 1.0f;
    return m;
  }

  /**
   * Builds a matrix from its nine elements, in row-major order.
   *
   * This exists because brace-initialising a `Matrix3x3` directly is a trap: `values` is a nested
   * `std::array`, so the natural `{ { a, b, c }, { d, e, f }, { g, h, i } }` is off by one level of
   * braces, and MSVC rejects the correct nesting with a bare "too many initializers" that points at
   * the member declaration rather than at the call site. A named constructor cost nothing and cannot
   * be got wrong.
   */
  [[nodiscard]] static Matrix3x3 fromRows(float m00, float m01, float m02, float m10, float m11, float m12,
          float m20, float m21, float m22) noexcept {
    Matrix3x3 m;
    m.values[0][0] = m00;
    m.values[0][1] = m01;
    m.values[0][2] = m02;
    m.values[1][0] = m10;
    m.values[1][1] = m11;
    m.values[1][2] = m12;
    m.values[2][0] = m20;
    m.values[2][1] = m21;
    m.values[2][2] = m22;
    return m;
  }

  [[nodiscard]] float& at(size_t row, size_t col) noexcept { return values[row][col]; }
  [[nodiscard]] float at(size_t row, size_t col) const noexcept { return values[row][col]; }
};

/** A row-major 3x4 matrix: a 3x3 transform with a per-channel offset in the last column. */
struct Matrix3x4 {
  /** Zero-initialised, for the same reasons as @c Matrix3x3::values. */
  std::array<std::array<float, 4>, 3> values = {};

  [[nodiscard]] float& at(size_t row, size_t col) noexcept { return values[row][col]; }
  [[nodiscard]] float at(size_t row, size_t col) const noexcept { return values[row][col]; }
};

/**
 * @return the inverse of @p m, or `std::nullopt` when it is singular.
 *
 * Unlike skcms this cannot alias its input, which is expressed by taking and returning by value rather
 * than by a comment warning that two pointers must not be equal.
 */
[[nodiscard]] std::optional<Matrix3x3> invert(const Matrix3x3& m) noexcept;

/** @return `a * b`, the composition that applies @p b first. */
[[nodiscard]] Matrix3x3 concat(const Matrix3x3& a, const Matrix3x3& b) noexcept;

/** @return @p m applied to the vector @p v as a colour transform, i.e. without any offset. */
[[nodiscard]] std::array<float, 3> transform(const Matrix3x3& m, const std::array<float, 3>& v) noexcept;

} // namespace gcms

#endif // CORAL_GRAPHICS_GCMS_MATH_H
