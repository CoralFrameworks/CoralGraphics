/*
 * Copyright (C) 2026 The Coral Project
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

#ifndef CORAL_TEST_ASSERTS_H
#define CORAL_TEST_ASSERTS_H

#include <array>
#include <iostream>

#include <gtest/gtest.h>

#include <geometry/Matrix.h>
#include <geometry/Point.h>
#include <geometry/Rect.h>
#include <geometry/Size.h>
#include <geometry/Vector.h>

#include <utils/Assert.h>

inline bool NumberNear(const double a, const double b) {
  if (a == b) {
    return true;
  }
  if (std::isnan(a) || std::isnan(b)) {
    return false;
  }

  // We used to compare based on an absolute difference of 1e-3 which
  // would allow up to 10 bits of mantissa difference in a float
  // (leaving 14 bits of accuracy being tested). Some numbers in the tests
  // will fail with a bit difference of up to 19 (a little over 4 bits) even
  // though the numbers print out identically using the float ostream output
  // at the default output precision. Choosing a max "units of least precision"
  // of 32 allows up to 5 bits of imprecision.
  static constexpr float kImpellerTestingMaxULP = 32;

  // We also impose a minimum step size so that cases of comparing numbers
  // very close to 0.0 don't compute a huge number of ULPs due to the ever
  // increasing precision near 0. This value is approximately the step size
  // of numbers going less than 1.0f.
  static constexpr float kMinimumULPStep = (1.0f / (1 << 24));

  auto adjust_step = [](float v) {
    return (std::abs(v) < kMinimumULPStep) ? std::copysignf(kMinimumULPStep, v)
                                           : v;
  };

  float step_ab = adjust_step(a - std::nexttowardf(a, b));
  float step_ba = adjust_step(b - std::nexttowardf(b, a));

  float ab_ulps = (a - b) / step_ab;
  float ba_ulps = (b - a) / step_ba;
  CORAL_ASSERT(ab_ulps >= 0 && ba_ulps >= 0);

  return (std::min(ab_ulps, ba_ulps) < kImpellerTestingMaxULP);
}

inline ::testing::AssertionResult MatrixNear(graphics::Matrix a,
                                             graphics::Matrix b) {
  auto equal = NumberNear(a.m[0], b.m[0])       //
               && NumberNear(a.m[1], b.m[1])    //
               && NumberNear(a.m[2], b.m[2])    //
               && NumberNear(a.m[3], b.m[3])    //
               && NumberNear(a.m[4], b.m[4])    //
               && NumberNear(a.m[5], b.m[5])    //
               && NumberNear(a.m[6], b.m[6])    //
               && NumberNear(a.m[7], b.m[7])    //
               && NumberNear(a.m[8], b.m[8])    //
               && NumberNear(a.m[9], b.m[9])    //
               && NumberNear(a.m[10], b.m[10])  //
               && NumberNear(a.m[11], b.m[11])  //
               && NumberNear(a.m[12], b.m[12])  //
               && NumberNear(a.m[13], b.m[13])  //
               && NumberNear(a.m[14], b.m[14])  //
               && NumberNear(a.m[15], b.m[15]);

  return equal ? ::testing::AssertionSuccess()
               : ::testing::AssertionFailure()
                     << "Matrixes are not equal " << a << " " << b;
}

inline ::testing::AssertionResult QuaternionNear(graphics::Quaternion a,
                                                 graphics::Quaternion b) {
  auto equal = NumberNear(a.x, b.x) && NumberNear(a.y, b.y) &&
               NumberNear(a.z, b.z) && NumberNear(a.w, b.w);

  return equal ? ::testing::AssertionSuccess()
               : ::testing::AssertionFailure() << "Quaternions are not equal.";
}

inline ::testing::AssertionResult RectNear(graphics::Rect a, graphics::Rect b) {
  auto equal = NumberNear(a.getLeft(), b.getLeft()) &&
               NumberNear(a.getTop(), b.getTop()) &&
               NumberNear(a.getRight(), b.getRight()) &&
               NumberNear(a.getBottom(), b.getBottom());

  return equal ? ::testing::AssertionSuccess()
               : ::testing::AssertionFailure()
                     << "Rects are not equal (" << a << " " << b << ")";
}

inline ::testing::AssertionResult PointNear(graphics::Point a,
                                            graphics::Point b) {
  auto equal = NumberNear(a.x, b.x) && NumberNear(a.y, b.y);

  return equal ? ::testing::AssertionSuccess()
               : ::testing::AssertionFailure()
                     << "Points are not equal (" << a << " " << b << ").";
}

inline ::testing::AssertionResult Vector3Near(graphics::Vector3 a,
                                              graphics::Vector3 b) {
  auto equal =
      NumberNear(a.x, b.x) && NumberNear(a.y, b.y) && NumberNear(a.z, b.z);

  return equal ? ::testing::AssertionSuccess()
               : ::testing::AssertionFailure() << "Vector3s are not equal.";
}

inline ::testing::AssertionResult Vector4Near(graphics::Vector4 a,
                                              graphics::Vector4 b) {
  auto equal = NumberNear(a.x, b.x) && NumberNear(a.y, b.y) &&
               NumberNear(a.z, b.z) && NumberNear(a.w, b.w);

  return equal ? ::testing::AssertionSuccess()
               : ::testing::AssertionFailure() << "Vector4s are not equal.";
}

inline ::testing::AssertionResult SizeNear(graphics::Size a, graphics::Size b) {
  auto equal = NumberNear(a.width, b.width) && NumberNear(a.height, b.height);

  return equal ? ::testing::AssertionSuccess()
               : ::testing::AssertionFailure() << "Sizes are not equal.";
}

inline ::testing::AssertionResult Array4Near(std::array<uint8_t, 4> a,
                                             std::array<uint8_t, 4> b) {
  auto equal = NumberNear(a[0], b[0]) && NumberNear(a[1], b[1]) &&
               NumberNear(a[2], b[2]) && NumberNear(a[3], b[3]);

  return equal ? ::testing::AssertionSuccess()
               : ::testing::AssertionFailure() << "Arrays are not equal.";
}

#define ASSERT_MATRIX_NEAR(a, b) ASSERT_PRED2(&::MatrixNear, a, b)
#define ASSERT_QUATERNION_NEAR(a, b) ASSERT_PRED2(&::QuaternionNear, a, b)
#define ASSERT_RECT_NEAR(a, b) ASSERT_PRED2(&::RectNear, a, b)
#define ASSERT_POINT_NEAR(a, b) ASSERT_PRED2(&::PointNear, a, b)
#define ASSERT_VECTOR3_NEAR(a, b) ASSERT_PRED2(&::Vector3Near, a, b)
#define ASSERT_VECTOR4_NEAR(a, b) ASSERT_PRED2(&::Vector4Near, a, b)
#define ASSERT_SIZE_NEAR(a, b) ASSERT_PRED2(&::SizeNear, a, b)
#define ASSERT_ARRAY_4_NEAR(a, b) ASSERT_PRED2(&::Array4Near, a, b)

#define EXPECT_MATRIX_NEAR(a, b) EXPECT_PRED2(&::MatrixNear, a, b)
#define EXPECT_QUATERNION_NEAR(a, b) EXPECT_PRED2(&::QuaternionNear, a, b)
#define EXPECT_RECT_NEAR(a, b) EXPECT_PRED2(&::RectNear, a, b)
#define EXPECT_POINT_NEAR(a, b) EXPECT_PRED2(&::PointNear, a, b)
#define EXPECT_VECTOR3_NEAR(a, b) EXPECT_PRED2(&::Vector3Near, a, b)
#define EXPECT_VECTOR4_NEAR(a, b) EXPECT_PRED2(&::Vector4Near, a, b)
#define EXPECT_SIZE_NEAR(a, b) EXPECT_PRED2(&::SizeNear, a, b)
#define EXPECT_ARRAY_4_NEAR(a, b) EXPECT_PRED2(&::Array4Near, a, b)

#endif //CORAL_TEST_ASSERTS_H