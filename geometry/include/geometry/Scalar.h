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

#ifndef CORAL_GEOMETRY_SCALAR_H
#define CORAL_GEOMETRY_SCALAR_H

#include <cmath>
#include <ostream>
#include <type_traits>

namespace graphics {

// e
constexpr float kE = 2.7182818284590452354f;

// log_2 e
constexpr float kLog2E = 1.4426950408889634074f;

// log_10 e
constexpr float kLog10E = 0.43429448190325182765f;

// log_e 2
constexpr float kLogE2 = 0.69314718055994530942f;

// log_e 10
constexpr float kLogE10 = 2.30258509299404568402f;

// pi
constexpr float kPi = 3.14159265358979323846f;

// pi*2
constexpr float k2Pi = 6.28318530717958647693f;

// pi/2
constexpr float kPiOver2 = 1.57079632679489661923f;

// pi/4
constexpr float kPiOver4 = 0.78539816339744830962f;

// 1/pi
constexpr float k1OverPi = 0.31830988618379067154f;

// 2/pi
constexpr float k2OverPi = 0.63661977236758134308f;

// 2/sqrt(pi)
constexpr float k2OverSqrtPi = 1.12837916709551257390f;

// sqrt(2)
constexpr float kSqrt2 = 1.41421356237309504880f;

// sqrt(2) / 2 == 1/sqrt(2)
constexpr float k1OverSqrt2 = 0.70710678118654752440f;
constexpr float kSqrt2Over2 = 0.70710678118654752440f;

// phi
constexpr float kPhi = 1.61803398874989484820f;

// 0.001
constexpr float kEhCloseEnough = 1e-3f;

using Scalar = float;

template<class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
T Absolute(const T& val) {
  return val >= T{} ? val : -val;
}

template<>
inline Scalar Absolute<Scalar>(const float& val) {
  return fabsf(val);
}

inline bool ScalarNearlyZero(const Scalar x, const Scalar tolerance = kEhCloseEnough) {
  return Absolute(x) <= tolerance;
}

inline bool ScalarNearlyEqual(const Scalar x, const Scalar y, const Scalar tolerance = kEhCloseEnough) {
  return ScalarNearlyZero(x - y, tolerance);
}

struct Degrees;

struct Radians {
  Scalar radians = 0.0;

  constexpr Radians() = default;

  explicit constexpr Radians(const Scalar radians) : radians(radians) {
  }

  [[nodiscard]] bool isFinite() const { return std::isfinite(radians); }

  constexpr Radians operator-() const { return Radians{ -radians }; }
  constexpr Radians operator+(const Radians r) const { return Radians{ radians + r.radians }; }
  constexpr Radians operator-(const Radians r) const { return Radians{ radians - r.radians }; }
  constexpr bool operator>(const Radians r) const { return radians > r.radians; }
  constexpr bool operator>=(const Radians r) const { return radians >= r.radians; }
  constexpr bool operator==(const Radians r) const { return radians == r.radians; }
  constexpr bool operator!=(const Radians r) const { return radians != r.radians; }
  constexpr bool operator<=(const Radians r) const { return radians <= r.radians; }
  constexpr bool operator<(const Radians r) const { return radians < r.radians; }
};

struct Degrees {
  Scalar degrees = 0.0;

  constexpr Degrees() = default;

  explicit constexpr Degrees(const Scalar degrees) : degrees(degrees) {
  }

  constexpr operator Radians() const {
    return Radians{ degrees * kPi / 180.0f };
  }

  [[nodiscard]] bool isFinite() const { return std::isfinite(degrees); }

  constexpr Degrees operator-() const { return Degrees{ -degrees }; }

  constexpr Degrees operator+(const Degrees d) const {
    return Degrees{ degrees + d.degrees };
  }

  constexpr Degrees operator-(const Degrees d) const {
    return Degrees{ degrees - d.degrees };
  }

  constexpr bool operator>(const Degrees d) const { return degrees > d.degrees; }
  constexpr bool operator>=(const Degrees d) const { return degrees >= d.degrees; }
  constexpr bool operator==(const Degrees d) const { return degrees == d.degrees; }
  constexpr bool operator!=(const Degrees d) const { return degrees != d.degrees; }
  constexpr bool operator<=(const Degrees d) const { return degrees <= d.degrees; }
  constexpr bool operator<(const Degrees d) const { return degrees < d.degrees; }

  [[nodiscard]] Degrees getPositive() const {
    Scalar deg = std::fmod(degrees, 360.0f);
    if (deg < 0.0f) {
      deg += 360.0f;
    }
    return Degrees{ deg };
  }
};

inline std::ostream& operator<<(std::ostream& out, const Degrees& d) {
  return out << "Degrees(" << d.degrees << ")";
}

inline std::ostream& operator<<(std::ostream& out, const Radians& r) {
  return out << "Radians(" << r.radians << ")";
}

} // namespace graphics

#endif //CORAL_GEOMETRY_SCALAR_H
