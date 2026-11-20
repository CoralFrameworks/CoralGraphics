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

#ifndef CORAL_GEOMETRY_SHEAR_H
#define CORAL_GEOMETRY_SHEAR_H

#include <geometry/compiler.h>

#include <cassert>

namespace graphics {

struct GEOMETRY_EMPTY_BASES Shear {
  static constexpr size_t SIZE = 3;

  union {
    // Explicit zero initializer rather than GEOMETRY_CONSTEXPR_INIT so that
    // the default value is well defined on every compiler, not just MSVC.
    double e[SIZE] = { 0.0, 0.0, 0.0 };

    struct {
      double xy;
      double xz;
      double yz;
    };
  };

  GEOMETRY_DEFAULT_CTOR_CONSTEXPR Shear() GEOMETRY_DEFAULT_CTOR

  Shear(const double xy, const double xz, const double yz) : e{ xy, xz, yz } {
  }

  constexpr double const& operator[](const size_t i) const noexcept {
    assert(i < SIZE);
    return e[i];
  }

  constexpr double& operator[](const size_t i) noexcept {
    assert(i < SIZE);
    return e[i];
  }

  bool operator==(const Shear& o) const {
    return xy == o.xy && xz == o.xz && yz == o.yz;
  }

  bool operator!=(const Shear& o) const { return !(*this == o); }
};

} // namespace graphics

#endif //CORAL_GEOMETRY_SHEAR_H
