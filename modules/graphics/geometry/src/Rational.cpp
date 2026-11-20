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

#include <geometry/Rational.h>

#include <cmath>
#include <cstdlib>
#include <numeric>

namespace graphics {

namespace {
uint32_t AbsToUnsigned(const int32_t x) {
  return static_cast<uint32_t>(std::abs(x));
}
}  // namespace

bool Rational::operator==(const Rational& that) const {
  if (mDen == that.mDen) {
    return mNum == that.mNum;
  } else if ((mNum >= 0) != (that.mNum >= 0)) {
    return false;
  } else {
    return AbsToUnsigned(mNum) * that.mDen == AbsToUnsigned(that.mNum) * mDen;
  }
}

bool Rational::operator!=(const Rational& that) const {
  return !(*this == that);
}

bool Rational::operator<(const Rational& that) const {
  if (mDen == that.mDen) {
    return mNum < that.mNum;
  } else if ((mNum >= 0) != (that.mNum >= 0)) {
    return mNum < that.mNum;
  } else {
    return AbsToUnsigned(mNum) * that.mDen < AbsToUnsigned(that.mNum) * mDen;
  }
}

uint64_t Rational::getHash() const {
  if (mNum == 0) {
    return 0;
  }
  const uint64_t gcd = std::gcd(mNum, mDen);
  return ((mNum / gcd) << 32) | (mDen / gcd);
}

Rational Rational::invert() const {
  if (mNum >= 0) {
    return Rational(mDen, mNum);
  } else {
    return Rational(-1 * static_cast<int32_t>(mDen), std::abs(mNum));
  }
}

} // namespace graphics
