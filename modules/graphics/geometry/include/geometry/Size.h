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

#ifndef CORAL_GEOMETRY_SIZE_H
#define CORAL_GEOMETRY_SIZE_H

#include <geometry/Scalar.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <ostream>
#include <string>

namespace graphics {

#define ONLY_ON_FLOAT_M(Modifiers, Return) \
  template <typename U = T>                \
  Modifiers std::enable_if_t<std::is_floating_point_v<U>, Return>
#define ONLY_ON_FLOAT(Return) ONLY_ON_FLOAT_M(, Return)

template<class T>
struct TSize {
  using Type = T;

  Type width = {};
  Type height = {};

  constexpr TSize() = default;

  constexpr TSize(Type width, Type height) : width(width), height(height) {
  }

  constexpr explicit TSize(Type dimension) : width(dimension), height(dimension) {
  }

  template<class U>
  explicit constexpr TSize(const TSize<U>& other)
    : TSize(static_cast<Type>(other.width), static_cast<Type>(other.height)) {
  }

  static constexpr TSize MakeWH(Type width, Type height) {
    return TSize{ width, height };
  }

  static constexpr TSize Infinite() {
    return TSize{
      std::numeric_limits<Type>::max(),
      std::numeric_limits<Type>::max()
    };
  }

  constexpr TSize operator*(Scalar scale) const {
    return { width * scale, height * scale };
  }

  template<class U, class = std::enable_if_t<std::is_arithmetic_v<U>>>
  inline TSize operator*=(U scale) {
    width *= static_cast<Type>(scale);
    height *= static_cast<Type>(scale);
    return *this;
  }

  constexpr TSize operator/(const Scalar scale) const {
    return {
      static_cast<Scalar>(width) / scale,
      static_cast<Scalar>(height) / scale
    };
  }

  constexpr TSize operator/(const TSize& s) const {
    return { width / s.width, height / s.height };
  }

  constexpr bool operator==(const TSize& s) const {
    return s.width == width && s.height == height;
  }

  constexpr bool operator!=(const TSize& s) const {
    return s.width != width || s.height != height;
  }

  constexpr TSize operator+(const TSize& s) const {
    return { width + s.width, height + s.height };
  }

  constexpr TSize operator-(const TSize& s) const {
    return { width - s.width, height - s.height };
  }

  constexpr TSize operator-() const { return { -width, -height }; }

  [[nodiscard]] constexpr TSize min(const TSize& other) const {
    return {
      std::min(width, other.width),
      std::min(height, other.height),
    };
  }

  [[nodiscard]] constexpr TSize max(const TSize& other) const {
    return {
      std::max(width, other.width),
      std::max(height, other.height),
    };
  }

  [[nodiscard]] constexpr Type minDimension() const { return std::min(width, height); }

  [[nodiscard]] constexpr Type maxDimension() const { return std::max(width, height); }

  [[nodiscard]] constexpr TSize abs() const { return { std::fabs(width), std::fabs(height) }; }

  [[nodiscard]] constexpr TSize floor() const {
    return { std::floor(width), std::floor(height) };
  }

  [[nodiscard]] constexpr TSize ceil() const { return { std::ceil(width), std::ceil(height) }; }

  [[nodiscard]] constexpr TSize round() const {
    return { std::round(width), std::round(height) };
  }

  [[nodiscard]] constexpr Type getArea() const { return width * height; }

  /// Returns true if either of the width or height are 0, negative, or NaN.
  [[nodiscard]] constexpr bool isEmpty() const { return !(width > 0 && height > 0); }

  ONLY_ON_FLOAT_M(constexpr, bool)
    isFinite() const { return std::isfinite(width) && std::isfinite(height); }

  [[nodiscard]] constexpr bool isSquare() const { return width == height; }

  template<class U>
  static constexpr TSize ceil(const TSize<U>& other) {
    return TSize{
      static_cast<Type>(std::ceil(other.width)),
      static_cast<Type>(std::ceil(other.height))
    };
  }

  /**
   * Returns the mip count of the texture.
   */
  [[nodiscard]] constexpr size_t mipCount() const {
    constexpr size_t minimum_mip = 1u;
    if (isEmpty() || width <= 0 || height <= 0) {
      return minimum_mip;
    }
    const size_t result = std::min(log2(width), log2(height));
    return std::max(result, minimum_mip);
  }
};

// RHS algebraic operations with arithmetic types.

template<class T, class U, class = std::enable_if_t<std::is_arithmetic_v<U>>>
constexpr TSize<T> operator*(U s, const TSize<T>& p) {
  return p * s;
}

template<class T, class U, class = std::enable_if_t<std::is_arithmetic_v<U>>>
constexpr TSize<T> operator/(U s, const TSize<T>& p) {
  return { static_cast<T>(s) / p.width, static_cast<T>(s) / p.height };
}

using Size = TSize<Scalar>;
using ISize32 = TSize<int32_t>;
using ISize64 = TSize<int64_t>;
using ISize = ISize64;

static_assert(sizeof(Size) == 2 * sizeof(Scalar));

template<class T>
inline std::ostream& operator<<(std::ostream& out, const TSize<T>& s) {
  out << "(" << s.width << ", " << s.height << ")";
  return out;
}

} // namespace graphics

#endif //CORAL_GEOMETRY_SIZE_H
