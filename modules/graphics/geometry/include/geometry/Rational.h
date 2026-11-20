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

#ifndef CORAL_GEOMETRY_RATIONAL_H
#define CORAL_GEOMETRY_RATIONAL_H

#include <geometry/Scalar.h>

#include <cstdint>

namespace graphics {

class Rational {
private:
  int32_t mNum;
  uint32_t mDen;

public:
  constexpr explicit Rational(const int32_t num) noexcept
      : mNum(num), mDen(1) {}

  constexpr explicit Rational(const int32_t num, const uint32_t den) noexcept
      : mNum(num), mDen(den) {}

  [[nodiscard]] int32_t getNumerator() const { return mNum; }

  [[nodiscard]] uint32_t getDenominator() const { return mDen; }

  bool operator==(const Rational& that) const;

  bool operator!=(const Rational& that) const;

  bool operator<(const Rational& that) const;

  [[nodiscard]] uint64_t getHash() const;

  explicit operator Scalar() const {
    return static_cast<Scalar>(mNum) / static_cast<Scalar>(mDen);
  }

  [[nodiscard]] Rational invert() const;
};

} // namespace graphics

#endif //CORAL_GEOMETRY_RATIONAL_H
