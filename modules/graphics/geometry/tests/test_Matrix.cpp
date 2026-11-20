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

#include <gtest/gtest.h>

#include <geometry/Matrix.h>

#include "test_asserts.h"

using namespace graphics;

class MatrixTest : public testing::Test {
protected:
};

TEST(MatrixTest, Multiply) {
  Matrix x(0.0, 0.0, 0.0, 1.0,
    1.0, 0.0, 0.0, 1.0,
    0.0, 1.0, 0.0, 1.0,
    1.0, 1.0, 0.0, 1.0);
  Matrix translate = Matrix::MakeTranslation({ 10, 20, 0 });
  Matrix result = translate * x;
  EXPECT_TRUE(MatrixNear(result, Matrix(10.0, 20.0, 0.0, 1.0,
    11.0, 20.0, 0.0, 1.0,
    10.0, 21.0, 0.0, 1.0,
    11.0, 21.0, 0.0, 1.0)));
}

TEST(MatrixTest, Equals) {
  Matrix x;
  Matrix y = x;
  EXPECT_TRUE(x.equals(y));
}

TEST(MatrixTest, NotEquals) {
  Matrix x;
  Matrix y = x.translate({ 1, 0, 0 });
  EXPECT_FALSE(x.equals(y));
}

TEST(MatrixTest, HasPerspective2D) {
  EXPECT_FALSE(Matrix().hasPerspective2D());

  auto test = [](int index, bool expect) {
    Matrix matrix;
    EXPECT_FALSE(matrix.hasPerspective2D());
    matrix.m[index] = 0.5f;
    EXPECT_EQ(matrix.hasPerspective2D(), expect) << "index: " << index;
  };

  // clang-format off
  test( 0, false);  test( 1, false);  test( 2, false);  test( 3, true);
  test( 4, false);  test( 5, false);  test( 6, false);  test( 7, true);
  test( 8, false);  test( 9, false);  test(10, false);  test(11, false);
  test(12, false);  test(13, false);  test(14, false);  test(15, true);
  // clang-format on
}

TEST(MatrixTest, HasPerspective) {
  EXPECT_FALSE(Matrix().hasPerspective());

  auto test = [](int index, bool expect) {
    Matrix matrix;
    EXPECT_FALSE(matrix.hasPerspective());
    matrix.m[index] = 0.5f;
    EXPECT_EQ(matrix.hasPerspective(), expect) << "index: " << index;
  };

  // clang-format off
  test( 0, false);  test( 1, false);  test( 2, false);  test( 3, true);
  test( 4, false);  test( 5, false);  test( 6, false);  test( 7, true);
  test( 8, false);  test( 9, false);  test(10, false);  test(11, true);
  test(12, false);  test(13, false);  test(14, false);  test(15, true);
  // clang-format on
}

TEST(MatrixTest, HasTranslation) {
  EXPECT_TRUE(Matrix::MakeTranslation({100, 100, 0}).hasTranslation());
  EXPECT_TRUE(Matrix::MakeTranslation({0, 100, 0}).hasTranslation());
  EXPECT_TRUE(Matrix::MakeTranslation({100, 0, 0}).hasTranslation());
  EXPECT_FALSE(Matrix().hasTranslation());
}

TEST(MatrixTest, IsTranslationOnly) {
  EXPECT_TRUE(Matrix::MakeTranslation({100, 100, 0}).isTranslationOnly());
  EXPECT_TRUE(Matrix::MakeTranslation({100, 100, 0}).isTranslationScaleOnly());
  EXPECT_TRUE(Matrix::MakeTranslation({0, 100, 0}).isTranslationOnly());
  EXPECT_TRUE(Matrix::MakeTranslation({0, 100, 0}).isTranslationScaleOnly());
  EXPECT_TRUE(Matrix::MakeTranslation({100, 0, 0}).isTranslationOnly());
  EXPECT_TRUE(Matrix::MakeTranslation({100, 0, 0}).isTranslationScaleOnly());
  EXPECT_TRUE(Matrix().isTranslationOnly());
  EXPECT_TRUE(Matrix().isTranslationScaleOnly());
}

TEST(MatrixTest, IsTranslationScaleOnly) {
  EXPECT_FALSE(Matrix::MakeScale({100, 100, 1}).isTranslationOnly());
  EXPECT_TRUE(Matrix::MakeScale({100, 100, 1}).isTranslationScaleOnly());
  EXPECT_FALSE(Matrix::MakeScale({1, 100, 1}).isTranslationOnly());
  EXPECT_TRUE(Matrix::MakeScale({1, 100, 1}).isTranslationScaleOnly());
  EXPECT_FALSE(Matrix::MakeScale({100, 1, 1}).isTranslationOnly());
  EXPECT_TRUE(Matrix::MakeScale({100, 1, 1}).isTranslationScaleOnly());
  EXPECT_TRUE(Matrix().isTranslationOnly());
  EXPECT_TRUE(Matrix().isTranslationScaleOnly());
}

TEST(MatrixTest, IsInvertibleGetDeterminant) {
  EXPECT_TRUE(Matrix().isInvertible());
  EXPECT_NE(Matrix().getDeterminant(), 0.0f);

  EXPECT_TRUE(Matrix::MakeTranslation({100, 100, 0}).isInvertible());
  EXPECT_NE(Matrix::MakeTranslation({100, 100, 0}).getDeterminant(), 0.0f);

  EXPECT_TRUE(Matrix::MakeScale({100, 100, 1}).isInvertible());
  EXPECT_NE(Matrix::MakeScale({100, 100, 1}).getDeterminant(), 0.0f);

  EXPECT_TRUE(Matrix::MakeRotationX(Degrees(30)).isInvertible());
  EXPECT_NE(Matrix::MakeRotationX(Degrees(30)).getDeterminant(), 0.0f);

  EXPECT_TRUE(Matrix::MakeRotationY(Degrees(30)).isInvertible());
  EXPECT_NE(Matrix::MakeRotationY(Degrees(30)).getDeterminant(), 0.0f);

  EXPECT_TRUE(Matrix::MakeRotationZ(Degrees(30)).isInvertible());
  EXPECT_NE(Matrix::MakeRotationZ(Degrees(30)).getDeterminant(), 0.0f);

  EXPECT_FALSE(Matrix::MakeScale({0, 1, 1}).isInvertible());
  EXPECT_EQ(Matrix::MakeScale({0, 1, 1}).getDeterminant(), 0.0f);
  EXPECT_FALSE(Matrix::MakeScale({1, 0, 1}).isInvertible());
  EXPECT_EQ(Matrix::MakeScale({1, 0, 1}).getDeterminant(), 0.0f);
  EXPECT_FALSE(Matrix::MakeScale({1, 1, 0}).isInvertible());
  EXPECT_EQ(Matrix::MakeScale({1, 1, 0}).getDeterminant(), 0.0f);
}

TEST(MatrixTest, IsFinite) {
  EXPECT_TRUE(Matrix().isFinite());

  EXPECT_TRUE(Matrix::MakeTranslation({100, 100, 0}).isFinite());
  EXPECT_TRUE(Matrix::MakeScale({100, 100, 1}).isFinite());

  EXPECT_TRUE(Matrix::MakeRotationX(Degrees(30)).isFinite());
  EXPECT_TRUE(Matrix::MakeRotationY(Degrees(30)).isFinite());
  EXPECT_TRUE(Matrix::MakeRotationZ(Degrees(30)).isFinite());

  EXPECT_TRUE(Matrix::MakeScale({0, 1, 1}).isFinite());
  EXPECT_TRUE(Matrix::MakeScale({1, 0, 1}).isFinite());
  EXPECT_TRUE(Matrix::MakeScale({1, 1, 0}).isFinite());

  for (int i = 0; i < 16; i++) {
    {
      Matrix matrix;
      ASSERT_TRUE(matrix.isFinite());
      matrix.m[i] = std::numeric_limits<Scalar>::infinity();
      ASSERT_FALSE(matrix.isFinite());
    }

    {
      Matrix matrix;
      ASSERT_TRUE(matrix.isFinite());
      matrix.m[i] = -std::numeric_limits<Scalar>::infinity();
      ASSERT_FALSE(matrix.isFinite());
    }

    {
      Matrix matrix;
      ASSERT_TRUE(matrix.isFinite());
      matrix.m[i] = -std::numeric_limits<Scalar>::quiet_NaN();
      ASSERT_FALSE(matrix.isFinite());
    }
  }
}

TEST(MatrixTest, IsAligned2D) {
  EXPECT_TRUE(Matrix().isAligned2D());
  EXPECT_TRUE(Matrix::MakeScale({1.0f, 1.0f, 2.0f}).isAligned2D());

  auto test = [](int index, bool expect) {
    Matrix matrix;
    EXPECT_TRUE(matrix.isAligned2D());
    matrix.m[index] = 0.5f;
    EXPECT_EQ(matrix.isAligned2D(), expect) << "index: " << index;
  };

  // clang-format off
  test( 0, true);   test( 1, false);  test( 2, true);   test( 3, false);
  test( 4, false);  test( 5, true);   test( 6, true);   test( 7, false);
  test( 8, true);   test( 9, true);   test(10, true);   test(11, true);
  test(12, true);   test(13, true);   test(14, true);   test(15, false);
  // clang-format on

  // True for quadrant rotations from -250 to +250 full circles
  for (int i = -1000; i < 1000; i++) {
    auto d = Degrees(i * 90);
    auto matrix = Matrix::MakeRotationZ(Degrees(d));
    EXPECT_TRUE(matrix.isAligned2D()) << "degrees: " << d.degrees;
  }

  // False for half degree rotations from -999.5 to +1000.5 degrees
  for (int i = -1000; i < 1000; i++) {
    auto d = Degrees(i + 0.5f);
    auto matrix = Matrix::MakeRotationZ(Degrees(d));
    EXPECT_FALSE(matrix.isAligned2D()) << "degrees: " << d.degrees;
  }
}

TEST(MatrixTest, IsAligned) {
  EXPECT_TRUE(Matrix().isAligned());
  EXPECT_TRUE(Matrix::MakeScale({1.0f, 1.0f, 2.0f}).isAligned());

  // Begin Legacy tests transferred over from geometry_unittests.cc
  {
    auto m = Matrix::MakeTranslation({ 1, 2, 3 });
    bool result = m.isAligned();
    ASSERT_TRUE(result);
  }

  {
    auto m = Matrix::MakeRotationZ(Degrees{ 123 });
    bool result = m.isAligned();
    ASSERT_FALSE(result);
  }
  // End Legacy tests transferred over from geometry_unittests.cc

  auto test = [](int index, bool expect) {
    Matrix matrix;
    EXPECT_TRUE(matrix.isAligned());
    matrix.m[index] = 0.5f;
    EXPECT_EQ(matrix.isAligned(), expect) << "index: " << index;
  };

  // clang-format off
  test( 0, true);   test( 1, false);  test( 2, false);  test( 3, false);
  test( 4, false);  test( 5, true);   test( 6, false);  test( 7, false);
  test( 8, false);  test( 9, false);  test(10, true);   test(11, false);
  test(12, true);   test(13, true);   test(14, true);   test(15, false);
  // clang-format on

  // True for quadrant rotations from -250 to +250 full circles
  for (int i = -1000; i < 1000; i++) {
    auto d = Degrees(i * 90);
    Matrix matrix = Matrix::MakeRotationZ(Degrees(d));
    EXPECT_TRUE(matrix.isAligned()) << "degrees: " << d.degrees;
  }

  // False for half degree rotations from -999.5 to +1000.5 degrees
  for (int i = -1000; i < 1000; i++) {
    auto d = Degrees(i + 0.5f);
    Matrix matrix = Matrix::MakeRotationZ(Degrees(d));
    EXPECT_FALSE(matrix.isAligned()) << "degrees: " << d.degrees;
  }
}

TEST(MatrixTest, TransformHomogenous) {
  auto matrix = Matrix::MakeColumn(
      // clang-format off
       2.0f,  3.0f,  5.0f,  7.0f,
      11.0f, 13.0f, 17.0f, 19.0f,
      23.0f, 29.0f, 31.0f, 37.0f,
      41.0f, 43.0f, 47.0f, 53.0f
    // clang-format on
    );
  EXPECT_EQ(matrix.transformHomogenous({1.0f, -1.0f}),
    Vector3(32.0f, 33.0f, 41.0f));
}

TEST(MatrixTest, GetMaxBasisXYNegativeScale) {
  auto m = Matrix::MakeScale({ -2, 1, 1 });

  EXPECT_EQ(m.getMaxBasisLengthXY(), 2);

  m = Matrix::MakeScale({ 1, -3, 1 });

  EXPECT_EQ(m.getMaxBasisLengthXY(), 3);
}

// Verifies a translate scale matrix doesn't need to compute sqrt(pow(scale, 2))
TEST(MatrixTest, GetMaxBasisXYWithLargeAndSmallScalingFactor) {
  auto m = Matrix::MakeScale({ 2.625e+20, 2.625e+20, 1 });
  EXPECT_NEAR(m.getMaxBasisLengthXY(), 2.625e+20, 1e+20);

  m = Matrix::MakeScale({ 2.625e-20, 2.625e-20, 1 });
  EXPECT_NEAR(m.getMaxBasisLengthXY(), 2.625e-20, 1e-20);
}

TEST(MatrixTest, GetMaxBasisXYWithLargeAndSmallScalingFactorNonScaleTranslate) {
  auto m = Matrix::MakeScale({ 2.625e+20, 2.625e+20, 1 });
  m.e[0][1] = 2;

  EXPECT_TRUE(std::isinf(m.getMaxBasisLengthXY()));
}

TEST(MatrixTest, TranslateWithPerspective) {
  auto m = Matrix::MakeRow(1.0, 0.0, 0.0, 10.0,
    0.0, 1.0, 0.0, 20.0,
    0.0, 0.0, 1.0, 0.0,
    0.0, 2.0, 0.0, 30.0);
  auto result = m.translate({ 100, 200 });
  EXPECT_TRUE(MatrixNear(result, Matrix::MakeRow(1.0, 0.0, 0.0, 110.0,
    0.0, 1.0, 0.0, 220.0,
    0.0, 0.0, 1.0, 0.0,
    0.0, 2.0, 0.0, 430.0)));
}

TEST(MatrixTest, MakeScaleTranslate) {
  EXPECT_TRUE(MatrixNear(
    Matrix::MakeTranslateScale({1, 1, 1.0 / 1024}, {10, 10, 1.0 / 1024}),
    Matrix::MakeTranslation({10, 10, 1.0 / 1024}) *
    Matrix::MakeScale({1, 1, 1.0 / 1024})));

  EXPECT_TRUE(MatrixNear(
    Matrix::MakeTranslateScale({2, 2, 2}, {10, 10, 0}),
    Matrix::MakeTranslation({10, 10, 0}) * Matrix::MakeScale({2, 2, 2})));

  EXPECT_TRUE(MatrixNear(
    Matrix::MakeTranslateScale({0, 0, 0}, {0, 0, 0}),
    Matrix::MakeTranslation({0, 0, 0}) * Matrix::MakeScale({0, 0, 0})));
}

TEST(MatrixTest, To3x3) {
  Matrix x(1.0, 0.0, 4.0, 0.0,
    0.0, 1.0, 4.0, 0.0,
    6.0, 5.0, 111.0, 7.0,
    0.0, 0.0, 9.0, 1.0);

  EXPECT_TRUE(MatrixNear(x.to3x3(), Matrix()));
}

TEST(MatrixTest, MinMaxScales2D) {
  // The GetScales2D() method is allowed to return the scales in any
  // order so we need to take special care in verifying the return
  // value to test them in either order.
  auto check_pair = [](const Matrix& matrix, Scalar scale1, Scalar scale2) {
    auto pair = matrix.getScales2D();
    EXPECT_TRUE(pair.has_value())
        << "Scales: " << scale1 << ", " << scale2 << ", " << matrix;
    if (ScalarNearlyEqual(pair->first, scale1)) {
      EXPECT_FLOAT_EQ(pair->first, scale1) << matrix;
      EXPECT_FLOAT_EQ(pair->second, scale2) << matrix;
    } else {
      EXPECT_FLOAT_EQ(pair->first, scale2) << matrix;
      EXPECT_FLOAT_EQ(pair->second, scale1) << matrix;
    }
  };

  for (int i = 1; i < 10; i++) {
    auto xScale = static_cast<Scalar>(i);
    for (int j = 1; j < 10; j++) {
      auto yScale = static_cast<Scalar>(j);
      auto minScale = std::min(xScale, yScale);
      auto maxScale = std::max(xScale, yScale);

      {
        // Simple scale
        auto matrix = Matrix::MakeScale({ xScale, yScale, 1.0f });
        EXPECT_TRUE(matrix.getMinScale2D().has_value());
        EXPECT_TRUE(matrix.getMaxScale2D().has_value());
        EXPECT_FLOAT_EQ(matrix.getMinScale2D().value_or(-1.0f), minScale);
        EXPECT_FLOAT_EQ(matrix.getMaxScale2D().value_or(-1.0f), maxScale);
        check_pair(matrix, xScale, yScale);
      }

      {
        // Simple scale with Z scale
        auto matrix = Matrix::MakeScale({ xScale, yScale, 5.0f });
        EXPECT_TRUE(matrix.getMinScale2D().has_value());
        EXPECT_TRUE(matrix.getMaxScale2D().has_value());
        EXPECT_FLOAT_EQ(matrix.getMinScale2D().value_or(-1.0f), minScale);
        EXPECT_FLOAT_EQ(matrix.getMaxScale2D().value_or(-1.0f), maxScale);
        check_pair(matrix, xScale, yScale);
      }

      {
        // Simple scale + translate
        auto matrix = Matrix::MakeTranslateScale({ xScale, yScale, 1.0f },
          { 10.0f, 15.0f, 2.0f });
        EXPECT_TRUE(matrix.getMinScale2D().has_value());
        EXPECT_TRUE(matrix.getMaxScale2D().has_value());
        EXPECT_FLOAT_EQ(matrix.getMinScale2D().value_or(-1.0f), minScale);
        EXPECT_FLOAT_EQ(matrix.getMaxScale2D().value_or(-1.0f), maxScale);
        check_pair(matrix, xScale, yScale);
      }

      for (int d = 45; d < 360; d += 45) {
        {
          // Rotation * Scale
          auto matrix = Matrix::MakeScale({ xScale, yScale, 1.0f }) *
                        Matrix::MakeRotationZ(Degrees(d));
          EXPECT_TRUE(matrix.getMinScale2D().has_value());
          EXPECT_TRUE(matrix.getMaxScale2D().has_value());
          EXPECT_FLOAT_EQ(matrix.getMinScale2D().value_or(-1.0f), minScale);
          EXPECT_FLOAT_EQ(matrix.getMaxScale2D().value_or(-1.0f), maxScale);
          check_pair(matrix, xScale, yScale);
        }

        {
          // Scale * Rotation
          auto matrix = Matrix::MakeRotationZ(Degrees(d)) *
                        Matrix::MakeScale({ xScale, yScale, 1.0f });
          EXPECT_TRUE(matrix.getMinScale2D().has_value());
          EXPECT_TRUE(matrix.getMaxScale2D().has_value());
          EXPECT_FLOAT_EQ(matrix.getMinScale2D().value_or(-1.0f), minScale);
          EXPECT_FLOAT_EQ(matrix.getMaxScale2D().value_or(-1.0f), maxScale);
          check_pair(matrix, xScale, yScale);
        }
      }

      {
        // Scale + PerspectiveX (returns invalid values)
        auto matrix = Matrix::MakeScale({ xScale, yScale, 1.0f });
        matrix.m[3] = 0.1;
        EXPECT_FALSE(matrix.getMinScale2D().has_value());
        EXPECT_FALSE(matrix.getMaxScale2D().has_value());
        EXPECT_FALSE(matrix.getScales2D().has_value());
      }

      {
        // Scale + PerspectiveY (returns invalid values)
        auto matrix = Matrix::MakeScale({ xScale, yScale, 1.0f });
        matrix.m[7] = 0.1;
        EXPECT_FALSE(matrix.getMinScale2D().has_value());
        EXPECT_FALSE(matrix.getMaxScale2D().has_value());
        EXPECT_FALSE(matrix.getScales2D().has_value());
      }

      {
        // Scale + PerspectiveZ (Z ignored; returns actual scales)
        auto matrix = Matrix::MakeScale({ xScale, yScale, 1.0f });
        matrix.m[11] = 0.1;
        EXPECT_TRUE(matrix.getMinScale2D().has_value());
        EXPECT_TRUE(matrix.getMaxScale2D().has_value());
        EXPECT_FLOAT_EQ(matrix.getMinScale2D().value_or(-1.0f), minScale);
        EXPECT_FLOAT_EQ(matrix.getMaxScale2D().value_or(-1.0f), maxScale);
        check_pair(matrix, xScale, yScale);
      }

      {
        // Scale + PerspectiveW (returns invalid values)
        auto matrix = Matrix::MakeScale({ xScale, yScale, 1.0f });
        matrix.m[15] = 0.1;
        EXPECT_FALSE(matrix.getMinScale2D().has_value());
        EXPECT_FALSE(matrix.getMaxScale2D().has_value());
        EXPECT_FALSE(matrix.getScales2D().has_value());
      }
    }
  }
}
