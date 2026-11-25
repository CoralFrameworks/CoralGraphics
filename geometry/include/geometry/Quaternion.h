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

#ifndef CORAL_GEOMETRY_QUATERNION_H
#define CORAL_GEOMETRY_QUATERNION_H

#include <geometry/Vector.h>

#include <ostream>

namespace graphics {

struct GEOMETRY_EMPTY_BASES Quaternion {
  static constexpr size_t SIZE = 4;
  union {
    // The default value is the identity quaternion (0, 0, 0, 1). The
    // initializer has to live on the array member: MSVC rejects default member
    // initializers on members of an anonymous struct inside a union (C2926),
    // and the union may only have one variant member with a default member
    // initializer.
    Scalar e[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    struct {
      Scalar x;
      Scalar y;
      Scalar z;
      Scalar w;
    };
  };

  GEOMETRY_DEFAULT_CTOR_CONSTEXPR Quaternion() noexcept GEOMETRY_DEFAULT_CTOR

  Quaternion(const Scalar px, const Scalar py, const Scalar pz, const Scalar pw)
      : x(px), y(py), z(pz), w(pw) {}

  explicit Quaternion(const Vector3& axis, Scalar angle) noexcept {
    const auto sine = sin(angle * 0.5f);
    x = sine * axis.x;
    y = sine * axis.y;
    z = sine * axis.z;
    w = cos(angle * 0.5f);
  }

  [[nodiscard]] Scalar dot(const Quaternion& q) const {
    return x * q.x + y * q.y + z * q.z + w * q.w;
  }

  [[nodiscard]] Scalar length() const { return sqrt(x * x + y * y + z * z + w * w); }

  /**
   * Normalize quaternion data
   */
  [[nodiscard]] Quaternion normalize() const {
    auto m = 1.0f / length();
    return { x * m, y * m, z * m, w * m };
  }

  [[nodiscard]] Quaternion invert() const { return { -x, -y, -z, w }; }

  /**
   * Spherical linear interpolation
   */
  [[nodiscard]] Quaternion slerp(const Quaternion& to, double time) const;

  Quaternion operator*(const Quaternion& o) const {
    return {
        w * o.x + x * o.w + y * o.z - z * o.y,
        w * o.y + y * o.w + z * o.x - x * o.z,
        w * o.z + z * o.w + x * o.y - y * o.x,
        w * o.w - x * o.x - y * o.y - z * o.z,
    };
  }

  Quaternion operator*(const Scalar scale) const {
    return { scale * x, scale * y, scale * z, scale * w };
  }

  Vector3 operator*(const Vector3& vector) const {
    const Vector3 v(x, y, z);
    return v * v.dot(vector) * 2 + vector * (w * w - v.dot(v)) + v.cross(vector) * 2 * w;
  }

  Quaternion operator+(const Quaternion& o) const {
    return { x + o.x, y + o.y, z + o.z, w + o.w };
  }

  Quaternion operator-(const Quaternion& o) const {
    return { x - o.x, y - o.y, z - o.z, w - o.w };
  }

  bool operator==(const Quaternion& o) const {
    return x == o.x && y == o.y && z == o.z && w == o.w;
  }

  bool operator!=(const Quaternion& o) const {
    return x != o.x || y != o.y || z != o.z || w != o.w;
  }
};

inline std::ostream& operator<<(std::ostream& out, const Quaternion& q) {
  out << "(" << q.x << ", " << q.y << ", " << q.z << ", " << q.w << ")";
  return out;
}

} // namespace graphics

#endif //CORAL_GEOMETRY_QUATERNION_H
