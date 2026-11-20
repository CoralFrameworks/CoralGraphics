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

#ifndef CORAL_GEOMETRY_VECTOR_H
#define CORAL_GEOMETRY_VECTOR_H

#include <geometry/compiler.h>

#include <geometry/Point.h>
#include <geometry/Scalar.h>
#include <geometry/Size.h>

#include <array>
#include <cmath>
#include <string>

namespace graphics {

struct GEOMETRY_EMPTY_BASES Vector3 {
  union {
    // Explicit zero initializer rather than GEOMETRY_CONSTEXPR_INIT so that
    // the default value is well defined on every compiler, not just MSVC.
    Scalar e[3] = { 0.0f, 0.0f, 0.0f };
    struct {
      Scalar x;
      Scalar y;
      Scalar z;
    };
  };

  GEOMETRY_DEFAULT_CTOR_CONSTEXPR Vector3() GEOMETRY_DEFAULT_CTOR

  constexpr Vector3(const Point& p) : x(p.x), y(p.y), z(0) {}

  constexpr Vector3(const Size& s) : x(s.width), y(s.height), z(0) {}

  constexpr Vector3(const Scalar x, const Scalar y) : x(x), y(y), z(0) {}

  constexpr Vector3(const Scalar x, const Scalar y, const Scalar z) : x(x), y(y), z(z) {}

  /**
   *  The length (or magnitude of the vector).
   *
   *  @return the calculated length.
   */
  [[nodiscard]] inline Scalar getLength() const { return sqrt(x * x + y * y + z * z); }

  [[nodiscard]] inline Vector3 normalize() const {
    const auto len = getLength();
    return { x / len, y / len, z / len };
  }

  [[nodiscard]] constexpr Scalar dot(const Vector3& other) const {
    return ((x * other.x) + (y * other.y) + (z * other.z));
  }

  [[nodiscard]] inline Vector3 abs() const {
    return { std::fabs(x), std::fabs(y), std::fabs(z) };
  }

  [[nodiscard]] constexpr Vector3 cross(const Vector3& other) const {
    return {
        (y * other.z) - (z * other.y),  //
        (z * other.x) - (x * other.z),  //
        (x * other.y) - (y * other.x)   //
    };
  }

  [[nodiscard]] inline Vector3 min(const Vector3& p) const {
    return { std::min(x, p.x), std::min(y, p.y), std::min(z, p.z) };
  }

  [[nodiscard]] inline Vector3 max(const Vector3& p) const {
    return { std::max(x, p.x), std::max(y, p.y), std::max(z, p.z) };
  }

  [[nodiscard]] inline Vector3 floor() const {
    return { std::floor(x), std::floor(y), std::floor(z) };
  }

  [[nodiscard]] inline Vector3 Ceil() const {
    return { std::ceil(x), std::ceil(y), std::ceil(z) };
  }

  [[nodiscard]] inline Vector3 round() const {
    return { std::round(x), std::round(y), std::round(z) };
  }

  constexpr bool operator==(const Vector3& v) const {
    return v.x == x && v.y == y && v.z == z;
  }

  constexpr bool operator!=(const Vector3& v) const {
    return v.x != x || v.y != y || v.z != z;
  }

  constexpr Vector3 operator+=(const Vector3& p) {
    x += p.x;
    y += p.y;
    z += p.z;
    return *this;
  }

  constexpr Vector3 operator-=(const Vector3& p) {
    x -= p.x;
    y -= p.y;
    z -= p.z;
    return *this;
  }

  constexpr Vector3 operator*=(const Vector3& p) {
    x *= p.x;
    y *= p.y;
    z *= p.z;
    return *this;
  }

  template<class U, class = std::enable_if_t<std::is_arithmetic_v<U>>>
  constexpr Vector3 operator*=(U scale) {
    x *= scale;
    y *= scale;
    z *= scale;
    return *this;
  }

  constexpr Vector3 operator/=(const Vector3& p) {
    x /= p.x;
    y /= p.y;
    z /= p.z;
    return *this;
  }

  template<class U, class = std::enable_if_t<std::is_arithmetic_v<U>>>
  constexpr Vector3 operator/=(U scale) {
    x /= scale;
    y /= scale;
    z /= scale;
    return *this;
  }

  constexpr Vector3 operator-() const { return {-x, -y, -z}; }

  constexpr Vector3 operator+(const Vector3& v) const {
    return {x + v.x, y + v.y, z + v.z};
  }

  constexpr Vector3 operator-(const Vector3& v) const {
    return {x - v.x, y - v.y, z - v.z};
  }

  constexpr Vector3 operator+(Scalar s) const {
    return {x + s, y + s, z + s};
  }

  constexpr Vector3 operator-(Scalar s) const {
    return {x - s, y - s, z - s};
  }

  constexpr Vector3 operator*(const Vector3& v) const {
    return {x * v.x, y * v.y, z * v.z};
  }

  template<class U, class = std::enable_if_t<std::is_arithmetic_v<U>>>
  constexpr Vector3 operator*(U scale) const {
    return Vector3(x * scale, y * scale, z * scale);
  }

  constexpr Vector3 operator/(const Vector3& v) const {
    return {x / v.x, y / v.y, z / v.z};
  }

  template<class U, class = std::enable_if_t<std::is_arithmetic_v<U>>>
  constexpr Vector3 operator/(U scale) const {
    return Vector3(x / scale, y / scale, z / scale);
  }

  [[nodiscard]] constexpr Vector3 lerp(const Vector3& v, Scalar t) const {
    return *this + (v - *this) * t;
  }

  /**
   *  Make a linear combination of two vectors and return the result.
   *
   *  @param a      the first vector.
   *  @param aScale the scale to use for the first vector.
   *  @param b      the second vector.
   *  @param bScale the scale to use for the second vector.
   *
   *  @return the combined vector.
   */
  static constexpr Vector3 Combine(const Vector3& a,
      Scalar aScale,
      const Vector3& b,
      Scalar bScale) {
    return {
        aScale * a.x + bScale * b.x,
        aScale * a.y + bScale * b.y,
        aScale * a.z + bScale * b.z,
    };
  }

  [[nodiscard]] std::string toString() const;
};

// RHS algebraic operations with arithmetic types.

template<class U, class = std::enable_if_t<std::is_arithmetic_v<U>>>
constexpr Vector3 operator*(U s, const Vector3& p) {
  return p * s;
}

template<class U, class = std::enable_if_t<std::is_arithmetic_v<U>>>
constexpr Vector3 operator+(U s, const Vector3& p) {
  return p + s;
}

template<class U, class = std::enable_if_t<std::is_arithmetic_v<U>>>
constexpr Vector3 operator-(U s, const Vector3& p) {
  return -p + s;
}

template<class U, class = std::enable_if_t<std::is_arithmetic_v<U>>>
constexpr Vector3 operator/(U s, const Vector3& p) {
  return {
      static_cast<Scalar>(s) / p.x,
      static_cast<Scalar>(s) / p.y,
      static_cast<Scalar>(s) / p.z,
  };
}

struct GEOMETRY_EMPTY_BASES Vector4 {
  static constexpr size_t SIZE = 4;
  union {
    // The default value is the identity (0, 0, 0, 1). The initializer has to
    // live on the array member: MSVC rejects default member initializers on
    // members of an anonymous struct inside a union (C2926), and the union may
    // only have one variant member with a default member initializer.
    Scalar e[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    struct {
      Scalar x;
      Scalar y;
      Scalar z;
      Scalar w;
    };
  };

  GEOMETRY_DEFAULT_CTOR_CONSTEXPR Vector4() GEOMETRY_DEFAULT_CTOR

  constexpr Vector4(Scalar x, Scalar y, Scalar z, Scalar w)
      : x(x), y(y), z(z), w(w) {}

  // Both of these describe a homogeneous position, so the untouched
  // components must be initialized explicitly: they initialize the struct
  // variant of the union, which means the initializer on |e| above does not
  // apply to them.
  constexpr Vector4(const Vector3& v) : x(v.x), y(v.y), z(v.z), w(1.0f) {}

  constexpr Vector4(const Point& p) : x(p.x), y(p.y), z(0.0f), w(1.0f) {}

  constexpr Vector4(std::array<Scalar, 4> values)
      : x(values[0]), y(values[1]), z(values[2]), w(values[3]) {}

   [[nodiscard]] bool isFinite() const {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
        std::isfinite(w);
  }

  [[nodiscard]] Vector4 normalize() const {
    const Scalar inverse = 1.0f / sqrt(x * x + y * y + z * z + w * w);
    return { x * inverse, y * inverse, z * inverse, w * inverse };
  }

  constexpr bool operator==(const Vector4& v) const {
    return (x == v.x) && (y == v.y) && (z == v.z) && (w == v.w);
  }

  constexpr bool operator!=(const Vector4& v) const {
    return (x != v.x) || (y != v.y) || (z != v.z) || (w != v.w);
  }

  constexpr Vector4 operator+(const Vector4& v) const {
    return { x + v.x, y + v.y, z + v.z, w + v.w };
  }

  constexpr Vector4 operator-(const Vector4& v) const {
    return { x - v.x, y - v.y, z - v.z, w - v.w };
  }

  constexpr Vector4 operator*(Scalar f) const {
    return { x * f, y * f, z * f, w * f };
  }

  constexpr Vector4 operator*(const Vector4& v) const {
    return { x * v.x, y * v.y, z * v.z, w * v.w };
  }

  [[nodiscard]] Vector4 min(const Vector4& p) const {
    return { std::min(x, p.x), std::min(y, p.y), std::min(z, p.z), std::min(w, p.w) };
  }

  [[nodiscard]] Vector4 max(const Vector4& p) const {
    return { std::max(x, p.x), std::max(y, p.y), std::max(z, p.z), std::max(w, p.w) };
  }

  [[nodiscard]] Vector4 floor() const {
    return { std::floor(x), std::floor(y), std::floor(z), std::floor(w) };
  }

  [[nodiscard]] Vector4 ceil() const {
    return { std::ceil(x), std::ceil(y), std::ceil(z), std::ceil(w) };
  }

  [[nodiscard]] Vector4 round() const {
    return { std::round(x), std::round(y), std::round(z), std::round(w) };
  }

  [[nodiscard]] Vector4 lerp(const Vector4& v, Scalar t) const {
    return *this + (v - *this) * t;
  }

  [[nodiscard]] Vector2 xy() const { return { x, y }; }

  [[nodiscard]] std::string toString() const;
};

static_assert(sizeof(Vector3) == 3 * sizeof(Scalar));
static_assert(sizeof(Vector4) == 4 * sizeof(Scalar));

inline std::ostream& operator<<(std::ostream& out, const Vector3& p) {
  out << "(" << p.x << ", " << p.y << ", " << p.z << ")";
  return out;
}

inline std::ostream& operator<<(std::ostream& out, const Vector4& p) {
  out << "(" << p.x << ", " << p.y << ", " << p.z << ", " << p.w << ")";
  return out;
}

} // namespace graphics

#endif //CORAL_GEOMETRY_VECTOR_H
