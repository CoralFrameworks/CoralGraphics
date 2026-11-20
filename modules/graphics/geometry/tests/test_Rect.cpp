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

#include <geometry/Rect.h>

#include "test_asserts.h"

using namespace graphics;

class RectTest : public testing::Test {
protected:
};

TEST(RectTest, RectEmptyDeclaration) {
  Rect rect;

  EXPECT_EQ(rect.getLeft(), 0.0f);
  EXPECT_EQ(rect.getTop(), 0.0f);
  EXPECT_EQ(rect.getRight(), 0.0f);
  EXPECT_EQ(rect.getBottom(), 0.0f);
  EXPECT_EQ(rect.getX(), 0.0f);
  EXPECT_EQ(rect.getY(), 0.0f);
  EXPECT_EQ(rect.getWidth(), 0.0f);
  EXPECT_EQ(rect.getHeight(), 0.0f);
  EXPECT_TRUE(rect.isEmpty());
  EXPECT_TRUE(rect.isFinite());
}

TEST(RectTest, IRectEmptyDeclaration) {
  IRect rect;

  EXPECT_EQ(rect.getLeft(), 0);
  EXPECT_EQ(rect.getTop(), 0);
  EXPECT_EQ(rect.getRight(), 0);
  EXPECT_EQ(rect.getBottom(), 0);
  EXPECT_EQ(rect.getX(), 0);
  EXPECT_EQ(rect.getY(), 0);
  EXPECT_EQ(rect.getWidth(), 0);
  EXPECT_EQ(rect.getHeight(), 0);
  EXPECT_TRUE(rect.isEmpty());
  // EXPECT_TRUE(rect.isFinite());  // should fail to compile
}

TEST(RectTest, RectDefaultConstructor) {
  Rect rect = Rect();

  EXPECT_EQ(rect.getLeft(), 0.0f);
  EXPECT_EQ(rect.getTop(), 0.0f);
  EXPECT_EQ(rect.getRight(), 0.0f);
  EXPECT_EQ(rect.getBottom(), 0.0f);
  EXPECT_EQ(rect.getX(), 0.0f);
  EXPECT_EQ(rect.getY(), 0.0f);
  EXPECT_EQ(rect.getWidth(), 0.0f);
  EXPECT_EQ(rect.getHeight(), 0.0f);
  EXPECT_TRUE(rect.isEmpty());
  EXPECT_TRUE(rect.isFinite());
}

TEST(RectTest, IRectDefaultConstructor) {
  IRect rect = IRect();

  EXPECT_EQ(rect.getLeft(), 0);
  EXPECT_EQ(rect.getTop(), 0);
  EXPECT_EQ(rect.getRight(), 0);
  EXPECT_EQ(rect.getBottom(), 0);
  EXPECT_EQ(rect.getX(), 0);
  EXPECT_EQ(rect.getY(), 0);
  EXPECT_EQ(rect.getWidth(), 0);
  EXPECT_EQ(rect.getHeight(), 0);
  EXPECT_TRUE(rect.isEmpty());
}

TEST(RectTest, RectSimpleLTRB) {
  // Using fractional-power-of-2 friendly values for equality tests
  Rect rect = Rect::MakeLTRB(5.125f, 10.25f, 20.625f, 25.375f);

  EXPECT_EQ(rect.getLeft(), 5.125f);
  EXPECT_EQ(rect.getTop(), 10.25f);
  EXPECT_EQ(rect.getRight(), 20.625f);
  EXPECT_EQ(rect.getBottom(), 25.375f);
  EXPECT_EQ(rect.getX(), 5.125f);
  EXPECT_EQ(rect.getY(), 10.25f);
  EXPECT_EQ(rect.getWidth(), 15.5f);
  EXPECT_EQ(rect.getHeight(), 15.125f);
  EXPECT_FALSE(rect.isEmpty());
  EXPECT_TRUE(rect.isFinite());
}

TEST(RectTest, IRectSimpleLTRB) {
  IRect rect = IRect::MakeLTRB(5, 10, 20, 25);

  EXPECT_EQ(rect.getLeft(), 5);
  EXPECT_EQ(rect.getTop(), 10);
  EXPECT_EQ(rect.getRight(), 20);
  EXPECT_EQ(rect.getBottom(), 25);
  EXPECT_EQ(rect.getX(), 5);
  EXPECT_EQ(rect.getY(), 10);
  EXPECT_EQ(rect.getWidth(), 15);
  EXPECT_EQ(rect.getHeight(), 15);
  EXPECT_FALSE(rect.isEmpty());
}

TEST(RectTest, RectSimpleXYWH) {
  // Using fractional-power-of-2 friendly values for equality tests
  Rect rect = Rect::MakeXYWH(5.125f, 10.25f, 15.5f, 15.125f);

  EXPECT_EQ(rect.getLeft(), 5.125f);
  EXPECT_EQ(rect.getTop(), 10.25f);
  EXPECT_EQ(rect.getRight(), 20.625f);
  EXPECT_EQ(rect.getBottom(), 25.375f);
  EXPECT_EQ(rect.getX(), 5.125f);
  EXPECT_EQ(rect.getY(), 10.25f);
  EXPECT_EQ(rect.getWidth(), 15.5f);
  EXPECT_EQ(rect.getHeight(), 15.125f);
  EXPECT_FALSE(rect.isEmpty());
  EXPECT_TRUE(rect.isFinite());
}

TEST(RectTest, IRectSimpleXYWH) {
  IRect rect = IRect::MakeXYWH(5, 10, 15, 16);

  EXPECT_EQ(rect.getLeft(), 5);
  EXPECT_EQ(rect.getTop(), 10);
  EXPECT_EQ(rect.getRight(), 20);
  EXPECT_EQ(rect.getBottom(), 26);
  EXPECT_EQ(rect.getX(), 5);
  EXPECT_EQ(rect.getY(), 10);
  EXPECT_EQ(rect.getWidth(), 15);
  EXPECT_EQ(rect.getHeight(), 16);
  EXPECT_FALSE(rect.isEmpty());
}

TEST(RectTest, RectSimpleWH) {
  // Using fractional-power-of-2 friendly values for equality tests
  Rect rect = Rect::MakeWH(15.5f, 15.125f);

  EXPECT_EQ(rect.getLeft(), 0.0f);
  EXPECT_EQ(rect.getTop(), 0.0f);
  EXPECT_EQ(rect.getRight(), 15.5f);
  EXPECT_EQ(rect.getBottom(), 15.125f);
  EXPECT_EQ(rect.getX(), 0.0f);
  EXPECT_EQ(rect.getY(), 0.0f);
  EXPECT_EQ(rect.getWidth(), 15.5f);
  EXPECT_EQ(rect.getHeight(), 15.125f);
  EXPECT_FALSE(rect.isEmpty());
  EXPECT_TRUE(rect.isFinite());
}

TEST(RectTest, IRectSimpleWH) {
  // Using fractional-power-of-2 friendly values for equality tests
  IRect rect = IRect::MakeWH(15, 25);

  EXPECT_EQ(rect.getLeft(), 0);
  EXPECT_EQ(rect.getTop(), 0);
  EXPECT_EQ(rect.getRight(), 15);
  EXPECT_EQ(rect.getBottom(), 25);
  EXPECT_EQ(rect.getX(), 0);
  EXPECT_EQ(rect.getY(), 0);
  EXPECT_EQ(rect.getWidth(), 15);
  EXPECT_EQ(rect.getHeight(), 25);
  EXPECT_FALSE(rect.isEmpty());
}

TEST(RectTest, RectFromIRect) {
  IRect irect = IRect::MakeLTRB(10, 20, 30, 40);
  Rect rect = Rect::Make(irect);

  EXPECT_EQ(rect.getLeft(), 10);
  EXPECT_EQ(rect.getTop(), 20);
  EXPECT_EQ(rect.getRight(), 30);
  EXPECT_EQ(rect.getBottom(), 40);

  // The following do not compile
  // IRect irect2 = IRect::Make(rect);
  // IRect irect2 = IRect::Make(irect);
}

TEST(RectTest, RectOverflowXYWH) {
  auto min = std::numeric_limits<Scalar>::lowest();
  auto max = std::numeric_limits<Scalar>::max();
  auto inf = std::numeric_limits<Scalar>::infinity();

  // 8 cases:
  //   finite X, max W
  //   max X, max W
  //   finite Y, max H
  //   max Y, max H
  //   finite X, min W
  //   min X, min W
  //   finite Y, min H
  //   min Y, min H

  // a small finite value added to a max value will remain max
  // a very large finite value (like max) added to max will go to infinity

  {
    Rect rect = Rect::MakeXYWH(5.0, 10.0f, max, 15.0f);

    EXPECT_EQ(rect.getLeft(), 5.0f);
    EXPECT_EQ(rect.getTop(), 10.0f);
    EXPECT_EQ(rect.getRight(), max);
    EXPECT_EQ(rect.getBottom(), 25.0f);
    EXPECT_EQ(rect.getX(), 5.0f);
    EXPECT_EQ(rect.getY(), 10.0f);
    EXPECT_EQ(rect.getWidth(), max);
    EXPECT_EQ(rect.getHeight(), 15.0f);
    EXPECT_FALSE(rect.isEmpty());
    EXPECT_TRUE(rect.isFinite());
  }

  {
    Rect rect = Rect::MakeXYWH(max, 10.0f, max, 15.0f);

    EXPECT_EQ(rect.getLeft(), max);
    EXPECT_EQ(rect.getTop(), 10.0f);
    EXPECT_EQ(rect.getRight(), inf);
    EXPECT_EQ(rect.getBottom(), 25.0f);
    EXPECT_EQ(rect.getX(), max);
    EXPECT_EQ(rect.getY(), 10.0f);
    EXPECT_EQ(rect.getWidth(), inf);
    EXPECT_EQ(rect.getHeight(), 15.0f);
    EXPECT_FALSE(rect.isEmpty());
    EXPECT_FALSE(rect.isFinite());
  }

  {
    Rect rect = Rect::MakeXYWH(5.0f, 10.0f, 20.0f, max);

    EXPECT_EQ(rect.getLeft(), 5.0f);
    EXPECT_EQ(rect.getTop(), 10.0f);
    EXPECT_EQ(rect.getRight(), 25.0f);
    EXPECT_EQ(rect.getBottom(), max);
    EXPECT_EQ(rect.getX(), 5.0f);
    EXPECT_EQ(rect.getY(), 10.0f);
    EXPECT_EQ(rect.getWidth(), 20.0f);
    EXPECT_EQ(rect.getHeight(), max);
    EXPECT_FALSE(rect.isEmpty());
    EXPECT_TRUE(rect.isFinite());
  }

  {
    Rect rect = Rect::MakeXYWH(5.0f, max, 20.0f, max);

    EXPECT_EQ(rect.getLeft(), 5.0f);
    EXPECT_EQ(rect.getTop(), max);
    EXPECT_EQ(rect.getRight(), 25.0f);
    EXPECT_EQ(rect.getBottom(), inf);
    EXPECT_EQ(rect.getX(), 5.0f);
    EXPECT_EQ(rect.getY(), max);
    EXPECT_EQ(rect.getWidth(), 20.0f);
    EXPECT_EQ(rect.getHeight(), inf);
    EXPECT_FALSE(rect.isEmpty());
    EXPECT_FALSE(rect.isFinite());
  }

  {
    Rect rect = Rect::MakeXYWH(5.0, 10.0f, min, 15.0f);

    EXPECT_EQ(rect.getLeft(), 5.0f);
    EXPECT_EQ(rect.getTop(), 10.0f);
    EXPECT_EQ(rect.getRight(), min);
    EXPECT_EQ(rect.getBottom(), 25.0f);
    EXPECT_EQ(rect.getX(), 5.0f);
    EXPECT_EQ(rect.getY(), 10.0f);
    EXPECT_EQ(rect.getWidth(), min);
    EXPECT_EQ(rect.getHeight(), 15.0f);
    EXPECT_TRUE(rect.isEmpty());
    EXPECT_TRUE(rect.isFinite());
  }

  {
    Rect rect = Rect::MakeXYWH(min, 10.0f, min, 15.0f);

    EXPECT_EQ(rect.getLeft(), min);
    EXPECT_EQ(rect.getTop(), 10.0f);
    EXPECT_EQ(rect.getRight(), -inf);
    EXPECT_EQ(rect.getBottom(), 25.0f);
    EXPECT_EQ(rect.getX(), min);
    EXPECT_EQ(rect.getY(), 10.0f);
    EXPECT_EQ(rect.getWidth(), -inf);
    EXPECT_EQ(rect.getHeight(), 15.0f);
    EXPECT_TRUE(rect.isEmpty());
    EXPECT_FALSE(rect.isFinite());
  }

  {
    Rect rect = Rect::MakeXYWH(5.0f, 10.0f, 20.0f, min);

    EXPECT_EQ(rect.getLeft(), 5.0f);
    EXPECT_EQ(rect.getTop(), 10.0f);
    EXPECT_EQ(rect.getRight(), 25.0f);
    EXPECT_EQ(rect.getBottom(), min);
    EXPECT_EQ(rect.getX(), 5.0f);
    EXPECT_EQ(rect.getY(), 10.0f);
    EXPECT_EQ(rect.getWidth(), 20.0f);
    EXPECT_EQ(rect.getHeight(), min);
    EXPECT_TRUE(rect.isEmpty());
    EXPECT_TRUE(rect.isFinite());
  }

  {
    Rect rect = Rect::MakeXYWH(5.0f, min, 20.0f, min);

    EXPECT_EQ(rect.getLeft(), 5.0f);
    EXPECT_EQ(rect.getTop(), min);
    EXPECT_EQ(rect.getRight(), 25.0f);
    EXPECT_EQ(rect.getBottom(), -inf);
    EXPECT_EQ(rect.getX(), 5.0f);
    EXPECT_EQ(rect.getY(), min);
    EXPECT_EQ(rect.getWidth(), 20.0f);
    EXPECT_EQ(rect.getHeight(), -inf);
    EXPECT_TRUE(rect.isEmpty());
    EXPECT_FALSE(rect.isFinite());
  }
}

TEST(RectTest, IRectOverflowXYWH) {
  auto min = std::numeric_limits<int64_t>::min();
  auto max = std::numeric_limits<int64_t>::max();

  // 4 cases
  //   x near max, positive w takes it past max
  //   x near min, negative w takes it below min
  //   y near max, positive h takes it past max
  //   y near min, negative h takes it below min

  {
    IRect rect = IRect::MakeXYWH(max - 5, 10, 10, 16);

    EXPECT_EQ(rect.getLeft(), max - 5);
    EXPECT_EQ(rect.getTop(), 10);
    EXPECT_EQ(rect.getRight(), max);
    EXPECT_EQ(rect.getBottom(), 26);
    EXPECT_EQ(rect.getX(), max - 5);
    EXPECT_EQ(rect.getY(), 10);
    EXPECT_EQ(rect.getWidth(), 5);
    EXPECT_EQ(rect.getHeight(), 16);
    EXPECT_FALSE(rect.isEmpty());
  }

  {
    IRect rect = IRect::MakeXYWH(min + 5, 10, -10, 16);

    EXPECT_EQ(rect.getLeft(), min + 5);
    EXPECT_EQ(rect.getTop(), 10);
    EXPECT_EQ(rect.getRight(), min);
    EXPECT_EQ(rect.getBottom(), 26);
    EXPECT_EQ(rect.getX(), min + 5);
    EXPECT_EQ(rect.getY(), 10);
    EXPECT_EQ(rect.getWidth(), -5);
    EXPECT_EQ(rect.getHeight(), 16);
    EXPECT_TRUE(rect.isEmpty());
  }

  {
    IRect rect = IRect::MakeXYWH(5, max - 10, 10, 16);

    EXPECT_EQ(rect.getLeft(), 5);
    EXPECT_EQ(rect.getTop(), max - 10);
    EXPECT_EQ(rect.getRight(), 15);
    EXPECT_EQ(rect.getBottom(), max);
    EXPECT_EQ(rect.getX(), 5);
    EXPECT_EQ(rect.getY(), max - 10);
    EXPECT_EQ(rect.getWidth(), 10);
    EXPECT_EQ(rect.getHeight(), 10);
    EXPECT_FALSE(rect.isEmpty());
  }

  {
    IRect rect = IRect::MakeXYWH(5, min + 10, 10, -16);

    EXPECT_EQ(rect.getLeft(), 5);
    EXPECT_EQ(rect.getTop(), min + 10);
    EXPECT_EQ(rect.getRight(), 15);
    EXPECT_EQ(rect.getBottom(), min);
    EXPECT_EQ(rect.getX(), 5);
    EXPECT_EQ(rect.getY(), min + 10);
    EXPECT_EQ(rect.getWidth(), 10);
    EXPECT_EQ(rect.getHeight(), -10);
    EXPECT_TRUE(rect.isEmpty());
  }
}

TEST(RectTest, RectOverflowLTRB) {
  auto min = std::numeric_limits<Scalar>::lowest();
  auto max = std::numeric_limits<Scalar>::max();
  auto inf = std::numeric_limits<Scalar>::infinity();

  // 8 cases:
  //   finite negative X, max W
  //   ~min X, ~max W
  //   finite negative Y, max H
  //   ~min Y, ~max H
  //   finite positive X, min W
  //   ~min X, ~min W
  //   finite positive Y, min H
  //   ~min Y, ~min H

  // a small finite value subtracted from a max value will remain max
  // a very large finite value (like min) subtracted from max will go to inf

  {
    Rect rect = Rect::MakeLTRB(-5.0f, 10.0f, max, 25.0f);

    EXPECT_EQ(rect.getLeft(), -5.0f);
    EXPECT_EQ(rect.getTop(), 10.0f);
    EXPECT_EQ(rect.getRight(), max);
    EXPECT_EQ(rect.getBottom(), 25.0f);
    EXPECT_EQ(rect.getX(), -5.0f);
    EXPECT_EQ(rect.getY(), 10.0f);
    EXPECT_EQ(rect.getWidth(), max);
    EXPECT_EQ(rect.getHeight(), 15.0f);
    EXPECT_FALSE(rect.isEmpty());
    EXPECT_TRUE(rect.isFinite());
  }

  {
    Rect rect = Rect::MakeLTRB(min + 5.0f, 10.0f, max - 5.0f, 25.0f);

    EXPECT_EQ(rect.getLeft(), min + 5.0f);
    EXPECT_EQ(rect.getTop(), 10.0f);
    EXPECT_EQ(rect.getRight(), max - 5.0f);
    EXPECT_EQ(rect.getBottom(), 25.0f);
    EXPECT_EQ(rect.getX(), min + 5.0f);
    EXPECT_EQ(rect.getY(), 10.0f);
    EXPECT_EQ(rect.getWidth(), inf);
    EXPECT_EQ(rect.getHeight(), 15.0f);
    EXPECT_FALSE(rect.isEmpty());
    EXPECT_TRUE(rect.isFinite());
  }

  {
    Rect rect = Rect::MakeLTRB(5.0f, -10.0f, 20.0f, max);

    EXPECT_EQ(rect.getLeft(), 5.0f);
    EXPECT_EQ(rect.getTop(), -10.0f);
    EXPECT_EQ(rect.getRight(), 20.0f);
    EXPECT_EQ(rect.getBottom(), max);
    EXPECT_EQ(rect.getX(), 5.0f);
    EXPECT_EQ(rect.getY(), -10.0f);
    EXPECT_EQ(rect.getWidth(), 15.0f);
    EXPECT_EQ(rect.getHeight(), max);
    EXPECT_FALSE(rect.isEmpty());
    EXPECT_TRUE(rect.isFinite());
  }

  {
    Rect rect = Rect::MakeLTRB(5.0f, min + 10.0f, 20.0f, max - 15.0f);

    EXPECT_EQ(rect.getLeft(), 5.0f);
    EXPECT_EQ(rect.getTop(), min + 10.0f);
    EXPECT_EQ(rect.getRight(), 20.0f);
    EXPECT_EQ(rect.getBottom(), max - 15.0f);
    EXPECT_EQ(rect.getX(), 5.0f);
    EXPECT_EQ(rect.getY(), min + 10.0f);
    EXPECT_EQ(rect.getWidth(), 15.0f);
    EXPECT_EQ(rect.getHeight(), inf);
    EXPECT_FALSE(rect.isEmpty());
    EXPECT_TRUE(rect.isFinite());
  }

  {
    Rect rect = Rect::MakeLTRB(5.0f, 10.0f, min, 25.0f);

    EXPECT_EQ(rect.getLeft(), 5.0f);
    EXPECT_EQ(rect.getTop(), 10.0f);
    EXPECT_EQ(rect.getRight(), min);
    EXPECT_EQ(rect.getBottom(), 25.0f);
    EXPECT_EQ(rect.getX(), 5.0f);
    EXPECT_EQ(rect.getY(), 10.0f);
    EXPECT_EQ(rect.getWidth(), min);
    EXPECT_EQ(rect.getHeight(), 15.0f);
    EXPECT_TRUE(rect.isEmpty());
    EXPECT_TRUE(rect.isFinite());
  }

  {
    Rect rect = Rect::MakeLTRB(max - 5.0f, 10.0f, min + 10.0f, 25.0f);

    EXPECT_EQ(rect.getLeft(), max - 5.0f);
    EXPECT_EQ(rect.getTop(), 10.0f);
    EXPECT_EQ(rect.getRight(), min + 10.0f);
    EXPECT_EQ(rect.getBottom(), 25.0f);
    EXPECT_EQ(rect.getX(), max - 5.0f);
    EXPECT_EQ(rect.getY(), 10.0f);
    EXPECT_EQ(rect.getWidth(), -inf);
    EXPECT_EQ(rect.getHeight(), 15.0f);
    EXPECT_TRUE(rect.isEmpty());
    EXPECT_TRUE(rect.isFinite());
  }

  {
    Rect rect = Rect::MakeLTRB(5.0f, 10.0f, 20.0f, min);

    EXPECT_EQ(rect.getLeft(), 5.0f);
    EXPECT_EQ(rect.getTop(), 10.0f);
    EXPECT_EQ(rect.getRight(), 20.0f);
    EXPECT_EQ(rect.getBottom(), min);
    EXPECT_EQ(rect.getX(), 5.0f);
    EXPECT_EQ(rect.getY(), 10.0f);
    EXPECT_EQ(rect.getWidth(), 15.0f);
    EXPECT_EQ(rect.getHeight(), min);
    EXPECT_TRUE(rect.isEmpty());
    EXPECT_TRUE(rect.isFinite());
  }

  {
    Rect rect = Rect::MakeLTRB(5.0f, max - 5.0f, 20.0f, min + 10.0f);

    EXPECT_EQ(rect.getLeft(), 5.0f);
    EXPECT_EQ(rect.getTop(), max - 5.0f);
    EXPECT_EQ(rect.getRight(), 20.0f);
    EXPECT_EQ(rect.getBottom(), min + 10.0f);
    EXPECT_EQ(rect.getX(), 5.0f);
    EXPECT_EQ(rect.getY(), max - 5.0f);
    EXPECT_EQ(rect.getWidth(), 15.0f);
    EXPECT_EQ(rect.getHeight(), -inf);
    EXPECT_TRUE(rect.isEmpty());
    EXPECT_TRUE(rect.isFinite());
  }
}

TEST(RectTest, IRectOverflowLTRB) {
  auto min = std::numeric_limits<int64_t>::min();
  auto max = std::numeric_limits<int64_t>::max();

  // 4 cases
  //   negative l, r near max takes width past max
  //   positive l, r near min takes width below min
  //   negative t, b near max takes width past max
  //   positive t, b near min takes width below min

  {
    IRect rect = IRect::MakeLTRB(-10, 10, max - 5, 26);

    EXPECT_EQ(rect.getLeft(), -10);
    EXPECT_EQ(rect.getTop(), 10);
    EXPECT_EQ(rect.getRight(), max - 5);
    EXPECT_EQ(rect.getBottom(), 26);
    EXPECT_EQ(rect.getX(), -10);
    EXPECT_EQ(rect.getY(), 10);
    EXPECT_EQ(rect.getWidth(), max);
    EXPECT_EQ(rect.getHeight(), 16);
    EXPECT_FALSE(rect.isEmpty());
  }

  {
    IRect rect = IRect::MakeLTRB(10, 10, min + 5, 26);

    EXPECT_EQ(rect.getLeft(), 10);
    EXPECT_EQ(rect.getTop(), 10);
    EXPECT_EQ(rect.getRight(), min + 5);
    EXPECT_EQ(rect.getBottom(), 26);
    EXPECT_EQ(rect.getX(), 10);
    EXPECT_EQ(rect.getY(), 10);
    EXPECT_EQ(rect.getWidth(), min);
    EXPECT_EQ(rect.getHeight(), 16);
    EXPECT_TRUE(rect.isEmpty());
  }

  {
    IRect rect = IRect::MakeLTRB(5, -10, 15, max - 5);

    EXPECT_EQ(rect.getLeft(), 5);
    EXPECT_EQ(rect.getTop(), -10);
    EXPECT_EQ(rect.getRight(), 15);
    EXPECT_EQ(rect.getBottom(), max - 5);
    EXPECT_EQ(rect.getX(), 5);
    EXPECT_EQ(rect.getY(), -10);
    EXPECT_EQ(rect.getWidth(), 10);
    EXPECT_EQ(rect.getHeight(), max);
    EXPECT_FALSE(rect.isEmpty());
  }

  {
    IRect rect = IRect::MakeLTRB(5, 10, 15, min + 5);

    EXPECT_EQ(rect.getLeft(), 5);
    EXPECT_EQ(rect.getTop(), 10);
    EXPECT_EQ(rect.getRight(), 15);
    EXPECT_EQ(rect.getBottom(), min + 5);
    EXPECT_EQ(rect.getX(), 5);
    EXPECT_EQ(rect.getY(), 10);
    EXPECT_EQ(rect.getWidth(), 10);
    EXPECT_EQ(rect.getHeight(), min);
    EXPECT_TRUE(rect.isEmpty());
  }
}

TEST(RectTest, RectMakeSize) {
  {
    Size s(100, 200);
    Rect r = Rect::MakeSize(s);
    Rect expected = Rect::MakeLTRB(0, 0, 100, 200);
    EXPECT_RECT_NEAR(r, expected);
  }

  {
    ISize s(100, 200);
    Rect r = Rect::MakeSize(s);
    Rect expected = Rect::MakeLTRB(0, 0, 100, 200);
    EXPECT_RECT_NEAR(r, expected);
  }

  {
    Size s(100, 200);
    IRect r = IRect::MakeSize(s);
    IRect expected = IRect::MakeLTRB(0, 0, 100, 200);
    EXPECT_EQ(r, expected);
  }

  {
    ISize s(100, 200);
    IRect r = IRect::MakeSize(s);
    IRect expected = IRect::MakeLTRB(0, 0, 100, 200);
    EXPECT_EQ(r, expected);
  }
}

TEST(RectTest, RectMakeMaximum) {
  Rect rect = Rect::MakeMaximum();
  auto inf = std::numeric_limits<Scalar>::infinity();
  auto min = std::numeric_limits<Scalar>::lowest();
  auto max = std::numeric_limits<Scalar>::max();

  EXPECT_EQ(rect.getLeft(), min);
  EXPECT_EQ(rect.getTop(), min);
  EXPECT_EQ(rect.getRight(), max);
  EXPECT_EQ(rect.getBottom(), max);
  EXPECT_EQ(rect.getX(), min);
  EXPECT_EQ(rect.getY(), min);
  EXPECT_EQ(rect.getWidth(), inf);
  EXPECT_EQ(rect.getHeight(), inf);
  EXPECT_FALSE(rect.isEmpty());
  EXPECT_TRUE(rect.isFinite());
}

TEST(RectTest, IRectMakeMaximum) {
  IRect rect = IRect::MakeMaximum();
  auto min = std::numeric_limits<int64_t>::min();
  auto max = std::numeric_limits<int64_t>::max();

  EXPECT_EQ(rect.getLeft(), min);
  EXPECT_EQ(rect.getTop(), min);
  EXPECT_EQ(rect.getRight(), max);
  EXPECT_EQ(rect.getBottom(), max);
  EXPECT_EQ(rect.getX(), min);
  EXPECT_EQ(rect.getY(), min);
  EXPECT_EQ(rect.getWidth(), max);
  EXPECT_EQ(rect.getHeight(), max);
  EXPECT_FALSE(rect.isEmpty());
}

TEST(RectTest, RectFromRect) {
  EXPECT_EQ(Rect(Rect::MakeXYWH(2, 3, 7, 15)),
    Rect::MakeXYWH(2.0, 3.0, 7.0, 15.0));
  EXPECT_EQ(Rect(Rect::MakeLTRB(2, 3, 7, 15)),
    Rect::MakeLTRB(2.0, 3.0, 7.0, 15.0));
}

TEST(RectTest, IRectFromIRect) {
  EXPECT_EQ(IRect(IRect::MakeXYWH(2, 3, 7, 15)),
    IRect::MakeXYWH(2, 3, 7, 15));
  EXPECT_EQ(IRect(IRect::MakeLTRB(2, 3, 7, 15)),
    IRect::MakeLTRB(2, 3, 7, 15));
}

TEST(RectTest, RectCopy) {
  // Using fractional-power-of-2 friendly values for equality tests
  Rect rect = Rect::MakeLTRB(5.125f, 10.25f, 20.625f, 25.375f);
  Rect copy = rect;

  EXPECT_EQ(rect, copy);
  EXPECT_EQ(copy.getLeft(), 5.125f);
  EXPECT_EQ(copy.getTop(), 10.25f);
  EXPECT_EQ(copy.getRight(), 20.625f);
  EXPECT_EQ(copy.getBottom(), 25.375f);
  EXPECT_EQ(copy.getX(), 5.125f);
  EXPECT_EQ(copy.getY(), 10.25f);
  EXPECT_EQ(copy.getWidth(), 15.5f);
  EXPECT_EQ(copy.getHeight(), 15.125f);
  EXPECT_FALSE(copy.isEmpty());
  EXPECT_TRUE(copy.isFinite());
}

TEST(RectTest, IRectCopy) {
  IRect rect = IRect::MakeLTRB(5, 10, 20, 25);
  IRect copy = rect;

  EXPECT_EQ(rect, copy);
  EXPECT_EQ(copy.getLeft(), 5);
  EXPECT_EQ(copy.getTop(), 10);
  EXPECT_EQ(copy.getRight(), 20);
  EXPECT_EQ(copy.getBottom(), 25);
  EXPECT_EQ(copy.getX(), 5);
  EXPECT_EQ(copy.getY(), 10);
  EXPECT_EQ(copy.getWidth(), 15);
  EXPECT_EQ(copy.getHeight(), 15);
  EXPECT_FALSE(copy.isEmpty());
}

TEST(RectTest, RectOriginSizeXYWHGetters) {
  {
    Rect r = Rect::MakeOriginSize({ 10, 20 }, { 50, 40 });
    EXPECT_EQ(r.getOrigin(), Point(10, 20));
    EXPECT_EQ(r.getSize(), Size(50, 40));
    EXPECT_EQ(r.getX(), 10);
    EXPECT_EQ(r.getY(), 20);
    EXPECT_EQ(r.getWidth(), 50);
    EXPECT_EQ(r.getHeight(), 40);
    auto expected_array = std::array<Scalar, 4>{ 10, 20, 50, 40 };
    EXPECT_EQ(r.getXYWH(), expected_array);
  }

  {
    Rect r = Rect::MakeLTRB(10, 20, 50, 40);
    EXPECT_EQ(r.getOrigin(), Point(10, 20));
    EXPECT_EQ(r.getSize(), Size(40, 20));
    EXPECT_EQ(r.getX(), 10);
    EXPECT_EQ(r.getY(), 20);
    EXPECT_EQ(r.getWidth(), 40);
    EXPECT_EQ(r.getHeight(), 20);
    auto expected_array = std::array<Scalar, 4>{ 10, 20, 40, 20 };
    EXPECT_EQ(r.getXYWH(), expected_array);
  }
}

TEST(RectTest, IRectOriginSizeXYWHGetters) {
  {
    IRect r = IRect::MakeOriginSize({ 10, 20 }, { 50, 40 });
    EXPECT_EQ(r.getOrigin(), IPoint(10, 20));
    EXPECT_EQ(r.getSize(), ISize(50, 40));
    EXPECT_EQ(r.getX(), 10);
    EXPECT_EQ(r.getY(), 20);
    EXPECT_EQ(r.getWidth(), 50);
    EXPECT_EQ(r.getHeight(), 40);
    auto expected_array = std::array<int64_t, 4>{ 10, 20, 50, 40 };
    EXPECT_EQ(r.getXYWH(), expected_array);
  }

  {
    IRect r = IRect::MakeLTRB(10, 20, 50, 40);
    EXPECT_EQ(r.getOrigin(), IPoint(10, 20));
    EXPECT_EQ(r.getSize(), ISize(40, 20));
    EXPECT_EQ(r.getX(), 10);
    EXPECT_EQ(r.getY(), 20);
    EXPECT_EQ(r.getWidth(), 40);
    EXPECT_EQ(r.getHeight(), 20);
    auto expected_array = std::array<int64_t, 4>{ 10, 20, 40, 20 };
    EXPECT_EQ(r.getXYWH(), expected_array);
  }
}

TEST(RectTest, RectRoundOutEmpty) {
  Rect rect;

  EXPECT_EQ(Rect::RoundOut(rect), Rect());

  EXPECT_EQ(IRect::RoundOut(rect), IRect());
}

TEST(RectTest, RectRoundOutSimple) {
  Rect rect = Rect::MakeLTRB(5.125f, 10.75f, 20.625f, 25.375f);

  EXPECT_EQ(Rect::RoundOut(rect), Rect::MakeLTRB(5.0f, 10.0f, 21.0f, 26.0f));

  EXPECT_EQ(IRect::RoundOut(rect), IRect::MakeLTRB(5, 10, 21, 26));
}

TEST(RectTest, RectRoundOutToIRectHuge) {
  auto test = [](int corners) {
    EXPECT_TRUE(corners >= 0 && corners <= 0xf);
    Scalar l, t, r, b;
    int64_t il, it, ir, ib;
    l = il = 50;
    t = it = 50;
    r = ir = 80;
    b = ib = 80;
    if ((corners & (1 << 0)) != 0) {
      l = -1E20;
      il = std::numeric_limits<int64_t>::min();
    }
    if ((corners & (1 << 1)) != 0) {
      t = -1E20;
      it = std::numeric_limits<int64_t>::min();
    }
    if ((corners & (1 << 2)) != 0) {
      r = +1E20;
      ir = std::numeric_limits<int64_t>::max();
    }
    if ((corners & (1 << 3)) != 0) {
      b = +1E20;
      ib = std::numeric_limits<int64_t>::max();
    }

    Rect rect = Rect::MakeLTRB(l, t, r, b);
    IRect irect = IRect::RoundOut(rect);
    EXPECT_EQ(irect.getLeft(), il) << corners;
    EXPECT_EQ(irect.getTop(), it) << corners;
    EXPECT_EQ(irect.getRight(), ir) << corners;
    EXPECT_EQ(irect.getBottom(), ib) << corners;
  };

  for (int corners = 0; corners <= 15; corners++) {
    test(corners);
  }
}

TEST(RectTest, RectDoesNotIntersectEmpty) {
  Rect rect = Rect::MakeLTRB(50, 50, 100, 100);

  auto test = [&rect](Scalar l, Scalar t, Scalar r, Scalar b,
    const std::string& label) {
    EXPECT_FALSE(rect.isIntersectsWithRect(Rect::MakeLTRB(l, b, r, t)))
        << label << " with Top/Bottom swapped";
    EXPECT_FALSE(rect.isIntersectsWithRect(Rect::MakeLTRB(r, b, l, t)))
        << label << " with Left/Right swapped";
    EXPECT_FALSE(rect.isIntersectsWithRect(Rect::MakeLTRB(r, t, l, b)))
        << label << " with all sides swapped";
  };

  test(20, 20, 30, 30, "Above and Left");
  test(70, 20, 80, 30, "Above");
  test(120, 20, 130, 30, "Above and Right");
  test(120, 70, 130, 80, "Right");
  test(120, 120, 130, 130, "Below and Right");
  test(70, 120, 80, 130, "Below");
  test(20, 120, 30, 130, "Below and Left");
  test(20, 70, 30, 80, "Left");

  test(70, 70, 80, 80, "Inside");

  test(40, 70, 60, 80, "Straddling Left");
  test(70, 40, 80, 60, "Straddling Top");
  test(90, 70, 110, 80, "Straddling Right");
  test(70, 90, 80, 110, "Straddling Bottom");
}

TEST(RectTest, IRectDoesNotIntersectEmpty) {
  IRect rect = IRect::MakeLTRB(50, 50, 100, 100);

  auto test = [&rect](int64_t l, int64_t t, int64_t r, int64_t b,
    const std::string& label) {
    EXPECT_FALSE(rect.isIntersectsWithRect(IRect::MakeLTRB(l, b, r, t)))
        << label << " with Top/Bottom swapped";
    EXPECT_FALSE(rect.isIntersectsWithRect(IRect::MakeLTRB(r, b, l, t)))
        << label << " with Left/Right swapped";
    EXPECT_FALSE(rect.isIntersectsWithRect(IRect::MakeLTRB(r, t, l, b)))
        << label << " with all sides swapped";
  };

  test(20, 20, 30, 30, "Above and Left");
  test(70, 20, 80, 30, "Above");
  test(120, 20, 130, 30, "Above and Right");
  test(120, 70, 130, 80, "Right");
  test(120, 120, 130, 130, "Below and Right");
  test(70, 120, 80, 130, "Below");
  test(20, 120, 30, 130, "Below and Left");
  test(20, 70, 30, 80, "Left");

  test(70, 70, 80, 80, "Inside");

  test(40, 70, 60, 80, "Straddling Left");
  test(70, 40, 80, 60, "Straddling Top");
  test(90, 70, 110, 80, "Straddling Right");
  test(70, 90, 80, 110, "Straddling Bottom");
}

TEST(RectTest, EmptyRectDoesNotIntersect) {
  Rect rect = Rect::MakeLTRB(50, 50, 100, 100);

  auto test = [&rect](Scalar l, Scalar t, Scalar r, Scalar b,
    const std::string& label) {
    EXPECT_FALSE(Rect::MakeLTRB(l, b, r, t).isIntersectsWithRect(rect))
        << label << " with Top/Bottom swapped";
    EXPECT_FALSE(Rect::MakeLTRB(r, b, l, t).isIntersectsWithRect(rect))
        << label << " with Left/Right swapped";
    EXPECT_FALSE(Rect::MakeLTRB(r, t, l, b).isIntersectsWithRect(rect))
        << label << " with all sides swapped";
  };

  test(20, 20, 30, 30, "Above and Left");
  test(70, 20, 80, 30, "Above");
  test(120, 20, 130, 30, "Above and Right");
  test(120, 70, 130, 80, "Right");
  test(120, 120, 130, 130, "Below and Right");
  test(70, 120, 80, 130, "Below");
  test(20, 120, 30, 130, "Below and Left");
  test(20, 70, 30, 80, "Left");

  test(70, 70, 80, 80, "Inside");

  test(40, 70, 60, 80, "Straddling Left");
  test(70, 40, 80, 60, "Straddling Top");
  test(90, 70, 110, 80, "Straddling Right");
  test(70, 90, 80, 110, "Straddling Bottom");
}

TEST(RectTest, EmptyIRectDoesNotIntersect) {
  IRect rect = IRect::MakeLTRB(50, 50, 100, 100);

  auto test = [&rect](int64_t l, int64_t t, int64_t r, int64_t b,
    const std::string& label) {
    EXPECT_FALSE(IRect::MakeLTRB(l, b, r, t).isIntersectsWithRect(rect))
        << label << " with Top/Bottom swapped";
    EXPECT_FALSE(IRect::MakeLTRB(r, b, l, t).isIntersectsWithRect(rect))
        << label << " with Left/Right swapped";
    EXPECT_FALSE(IRect::MakeLTRB(r, t, l, b).isIntersectsWithRect(rect))
        << label << " with all sides swapped";
  };

  test(20, 20, 30, 30, "Above and Left");
  test(70, 20, 80, 30, "Above");
  test(120, 20, 130, 30, "Above and Right");
  test(120, 70, 130, 80, "Right");
  test(120, 120, 130, 130, "Below and Right");
  test(70, 120, 80, 130, "Below");
  test(20, 120, 30, 130, "Below and Left");
  test(20, 70, 30, 80, "Left");

  test(70, 70, 80, 80, "Inside");

  test(40, 70, 60, 80, "Straddling Left");
  test(70, 40, 80, 60, "Straddling Top");
  test(90, 70, 110, 80, "Straddling Right");
  test(70, 90, 80, 110, "Straddling Bottom");
}

TEST(RectTest, RectScale) {
  auto test1 = [](Rect rect, Scalar scale) {
    Rect expected = Rect::MakeXYWH(rect.getX() * scale,
      rect.getY() * scale,
      rect.getWidth() * scale,
      rect.getHeight() * scale);

    EXPECT_RECT_NEAR(rect.scale(scale), expected) //
        << rect << " * " << scale;
    EXPECT_RECT_NEAR(rect.scale(scale, scale), expected) //
        << rect << " * " << scale;
    EXPECT_RECT_NEAR(rect.scale(Point(scale, scale)), expected) //
        << rect << " * " << scale;
    EXPECT_RECT_NEAR(rect.scale(Size(scale, scale)), expected) //
        << rect << " * " << scale;
  };

  auto test2 = [&test1](Rect rect, Scalar scale_x, Scalar scale_y) {
    Rect expected = Rect::MakeXYWH(rect.getX() * scale_x,
      rect.getY() * scale_y,
      rect.getWidth() * scale_x,
      rect.getHeight() * scale_y);

    EXPECT_RECT_NEAR(rect.scale(scale_x, scale_y), expected)
        << rect << " * " << scale_x << ", " << scale_y;
    EXPECT_RECT_NEAR(rect.scale(Point(scale_x, scale_y)), expected)
        << rect << " * " << scale_x << ", " << scale_y;
    EXPECT_RECT_NEAR(rect.scale(Size(scale_x, scale_y)), expected)
        << rect << " * " << scale_x << ", " << scale_y;

    test1(rect, scale_x);
    test1(rect, scale_y);
  };

  test2(Rect::MakeLTRB(10, 15, 100, 150), 1.0, 0.0);
  test2(Rect::MakeLTRB(10, 15, 100, 150), 0.0, 1.0);
  test2(Rect::MakeLTRB(10, 15, 100, 150), 0.0, 0.0);
  test2(Rect::MakeLTRB(10, 15, 100, 150), 2.5, 3.5);
  test2(Rect::MakeLTRB(10, 15, 100, 150), 3.5, 2.5);
  test2(Rect::MakeLTRB(10, 15, -100, 150), 2.5, 3.5);
  test2(Rect::MakeLTRB(10, 15, 100, -150), 2.5, 3.5);
  test2(Rect::MakeLTRB(10, 15, 100, 150), -2.5, 3.5);
  test2(Rect::MakeLTRB(10, 15, 100, 150), 2.5, -3.5);
}

TEST(RectTest, IRectScale) {
  auto test1 = [](IRect rect, int64_t scale) {
    IRect expected = IRect::MakeXYWH(rect.getX() * scale,
      rect.getY() * scale,
      rect.getWidth() * scale,
      rect.getHeight() * scale);

    EXPECT_EQ(rect.scale(scale), expected) << rect << " * " << scale;
    EXPECT_EQ(rect.scale(scale, scale), expected) << rect << " * " << scale;
    EXPECT_EQ(rect.scale(IPoint(scale, scale)), expected) << rect << " * " << scale;
    EXPECT_EQ(rect.scale(ISize(scale, scale)), expected) << rect << " * " << scale;
  };

  auto test2 = [&test1](IRect rect, int64_t scale_x, int64_t scale_y) {
    IRect expected = IRect::MakeXYWH(rect.getX() * scale_x,
      rect.getY() * scale_y,
      rect.getWidth() * scale_x,
      rect.getHeight() * scale_y);

    EXPECT_EQ(rect.scale(scale_x, scale_y), expected)
        << rect << " * " << scale_x << ", " << scale_y;
    EXPECT_EQ(rect.scale(IPoint(scale_x, scale_y)), expected)
        << rect << " * " << scale_x << ", " << scale_y;
    EXPECT_EQ(rect.scale(ISize(scale_x, scale_y)), expected)
        << rect << " * " << scale_x << ", " << scale_y;

    test1(rect, scale_x);
    test1(rect, scale_y);
  };

  test2(IRect::MakeLTRB(10, 15, 100, 150), 2, 3);
  test2(IRect::MakeLTRB(10, 15, 100, 150), 3, 2);
  test2(IRect::MakeLTRB(10, 15, -100, 150), 2, 3);
  test2(IRect::MakeLTRB(10, 15, 100, -150), 2, 3);
  test2(IRect::MakeLTRB(10, 15, 100, 150), -2, 3);
  test2(IRect::MakeLTRB(10, 15, 100, 150), 2, -3);
}

TEST(RectTest, RectArea) {
  EXPECT_EQ(Rect::MakeXYWH(0, 0, 100, 200).getArea(), 20000);
  EXPECT_EQ(Rect::MakeXYWH(10, 20, 100, 200).getArea(), 20000);
  EXPECT_EQ(Rect::MakeXYWH(0, 0, 200, 100).getArea(), 20000);
  EXPECT_EQ(Rect::MakeXYWH(10, 20, 200, 100).getArea(), 20000);
  EXPECT_EQ(Rect::MakeXYWH(0, 0, 100, 100).getArea(), 10000);
  EXPECT_EQ(Rect::MakeXYWH(10, 20, 100, 100).getArea(), 10000);
}

TEST(RectTest, IRectArea) {
  EXPECT_EQ(IRect::MakeXYWH(0, 0, 100, 200).getArea(), 20000);
  EXPECT_EQ(IRect::MakeXYWH(10, 20, 100, 200).getArea(), 20000);
  EXPECT_EQ(IRect::MakeXYWH(0, 0, 200, 100).getArea(), 20000);
  EXPECT_EQ(IRect::MakeXYWH(10, 20, 200, 100).getArea(), 20000);
  EXPECT_EQ(IRect::MakeXYWH(0, 0, 100, 100).getArea(), 10000);
  EXPECT_EQ(IRect::MakeXYWH(10, 20, 100, 100).getArea(), 10000);
}

TEST(RectTest, RectGetNormalizingTransform) {
  {
    // Checks for expected matrix values

    auto r = Rect::MakeXYWH(100, 200, 200, 400);

    EXPECT_EQ(r.getNormalizingTransform(),
      Matrix::MakeScale({0.005, 0.0025, 1.0}) *
      Matrix::MakeTranslation({-100, -200}));
  }

  {
    // Checks for expected transform of points relative to the rect

    auto r = Rect::MakeLTRB(300, 500, 400, 700);
    auto m = r.getNormalizingTransform();

    // The 4 corners of the rect => (0, 0) to (1, 1)
    EXPECT_EQ(m * Point(300, 500), Point(0, 0));
    EXPECT_EQ(m * Point(400, 500), Point(1, 0));
    EXPECT_EQ(m * Point(400, 700), Point(1, 1));
    EXPECT_EQ(m * Point(300, 700), Point(0, 1));

    // The center => (0.5, 0.5)
    EXPECT_EQ(m * Point(350, 600), Point(0.5, 0.5));

    // Outside the 4 corners => (-1, -1) to (2, 2)
    EXPECT_EQ(m * Point(200, 300), Point(-1, -1));
    EXPECT_EQ(m * Point(500, 300), Point(2, -1));
    EXPECT_EQ(m * Point(500, 900), Point(2, 2));
    EXPECT_EQ(m * Point(200, 900), Point(-1, 2));
  }

  {
    // Checks for behavior with empty rects

    auto zero = Matrix::MakeScale({ 0.0, 0.0, 1.0 });

    // Empty for width and/or height == 0
    EXPECT_EQ(Rect::MakeXYWH(10, 10, 0, 10).getNormalizingTransform(), zero);
    EXPECT_EQ(Rect::MakeXYWH(10, 10, 10, 0).getNormalizingTransform(), zero);
    EXPECT_EQ(Rect::MakeXYWH(10, 10, 0, 0).getNormalizingTransform(), zero);

    // Empty for width and/or height < 0
    EXPECT_EQ(Rect::MakeXYWH(10, 10, -1, 10).getNormalizingTransform(), zero);
    EXPECT_EQ(Rect::MakeXYWH(10, 10, 10, -1).getNormalizingTransform(), zero);
    EXPECT_EQ(Rect::MakeXYWH(10, 10, -1, -1).getNormalizingTransform(), zero);
  }

  {
    // Checks for behavior with non-finite rects

    auto z = Matrix::MakeScale({ 0.0, 0.0, 1.0 });
    auto nan = std::numeric_limits<Scalar>::quiet_NaN();
    auto inf = std::numeric_limits<Scalar>::infinity();

    // Non-finite for width and/or height == nan
    EXPECT_EQ(Rect::MakeXYWH(10, 10, nan, 10).getNormalizingTransform(), z);
    EXPECT_EQ(Rect::MakeXYWH(10, 10, 10, nan).getNormalizingTransform(), z);
    EXPECT_EQ(Rect::MakeXYWH(10, 10, nan, nan).getNormalizingTransform(), z);

    // Non-finite for width and/or height == inf
    EXPECT_EQ(Rect::MakeXYWH(10, 10, inf, 10).getNormalizingTransform(), z);
    EXPECT_EQ(Rect::MakeXYWH(10, 10, 10, inf).getNormalizingTransform(), z);
    EXPECT_EQ(Rect::MakeXYWH(10, 10, inf, inf).getNormalizingTransform(), z);

    // Non-finite for width and/or height == -inf
    EXPECT_EQ(Rect::MakeXYWH(10, 10, -inf, 10).getNormalizingTransform(), z);
    EXPECT_EQ(Rect::MakeXYWH(10, 10, 10, -inf).getNormalizingTransform(), z);
    EXPECT_EQ(Rect::MakeXYWH(10, 10, -inf, -inf).getNormalizingTransform(), z);

    // Non-finite for origin X and/or Y == nan
    EXPECT_EQ(Rect::MakeXYWH(nan, 10, 10, 10).getNormalizingTransform(), z);
    EXPECT_EQ(Rect::MakeXYWH(10, nan, 10, 10).getNormalizingTransform(), z);
    EXPECT_EQ(Rect::MakeXYWH(nan, nan, 10, 10).getNormalizingTransform(), z);

    // Non-finite for origin X and/or Y == inf
    EXPECT_EQ(Rect::MakeXYWH(inf, 10, 10, 10).getNormalizingTransform(), z);
    EXPECT_EQ(Rect::MakeXYWH(10, inf, 10, 10).getNormalizingTransform(), z);
    EXPECT_EQ(Rect::MakeXYWH(inf, inf, 10, 10).getNormalizingTransform(), z);

    // Non-finite for origin X and/or Y == -inf
    EXPECT_EQ(Rect::MakeXYWH(-inf, 10, 10, 10).getNormalizingTransform(), z);
    EXPECT_EQ(Rect::MakeXYWH(10, -inf, 10, 10).getNormalizingTransform(), z);
    EXPECT_EQ(Rect::MakeXYWH(-inf, -inf, 10, 10).getNormalizingTransform(), z);
  }
}

TEST(RectTest, IRectGetNormalizingTransform) {
  {
    // Checks for expected matrix values

    auto r = IRect::MakeXYWH(100, 200, 200, 400);

    EXPECT_EQ(r.getNormalizingTransform(),
      Matrix::MakeScale({0.005, 0.0025, 1.0}) *
      Matrix::MakeTranslation({-100, -200}));
  }

  {
    // Checks for expected transform of points relative to the rect

    auto r = IRect::MakeLTRB(300, 500, 400, 700);
    auto m = r.getNormalizingTransform();

    // The 4 corners of the rect => (0, 0) to (1, 1)
    EXPECT_EQ(m * Point(300, 500), Point(0, 0));
    EXPECT_EQ(m * Point(400, 500), Point(1, 0));
    EXPECT_EQ(m * Point(400, 700), Point(1, 1));
    EXPECT_EQ(m * Point(300, 700), Point(0, 1));

    // The center => (0.5, 0.5)
    EXPECT_EQ(m * Point(350, 600), Point(0.5, 0.5));

    // Outside the 4 corners => (-1, -1) to (2, 2)
    EXPECT_EQ(m * Point(200, 300), Point(-1, -1));
    EXPECT_EQ(m * Point(500, 300), Point(2, -1));
    EXPECT_EQ(m * Point(500, 900), Point(2, 2));
    EXPECT_EQ(m * Point(200, 900), Point(-1, 2));
  }

  {
    // Checks for behavior with empty rects

    auto zero = Matrix::MakeScale({ 0.0, 0.0, 1.0 });

    // Empty for width and/or height == 0
    EXPECT_EQ(IRect::MakeXYWH(10, 10, 0, 10).getNormalizingTransform(), zero);
    EXPECT_EQ(IRect::MakeXYWH(10, 10, 10, 0).getNormalizingTransform(), zero);
    EXPECT_EQ(IRect::MakeXYWH(10, 10, 0, 0).getNormalizingTransform(), zero);

    // Empty for width and/or height < 0
    EXPECT_EQ(IRect::MakeXYWH(10, 10, -1, 10).getNormalizingTransform(), zero);
    EXPECT_EQ(IRect::MakeXYWH(10, 10, 10, -1).getNormalizingTransform(), zero);
    EXPECT_EQ(IRect::MakeXYWH(10, 10, -1, -1).getNormalizingTransform(), zero);
  }
}

TEST(RectTest, RectXYWHIsEmpty) {
  auto nan = std::numeric_limits<Scalar>::quiet_NaN();

  // Non-empty
  EXPECT_FALSE(Rect::MakeXYWH(1.5, 2.3, 10.5, 7.2).isEmpty());

  // Empty both width and height both 0 or negative, in all combinations
  EXPECT_TRUE(Rect::MakeXYWH(1.5, 2.3, 0.0, 0.0).isEmpty());
  EXPECT_TRUE(Rect::MakeXYWH(1.5, 2.3, -1.0, -1.0).isEmpty());
  EXPECT_TRUE(Rect::MakeXYWH(1.5, 2.3, 0.0, -1.0).isEmpty());
  EXPECT_TRUE(Rect::MakeXYWH(1.5, 2.3, -1.0, 0.0).isEmpty());

  // Empty for 0 or negative width or height (but not both at the same time)
  EXPECT_TRUE(Rect::MakeXYWH(1.5, 2.3, 10.5, 0.0).isEmpty());
  EXPECT_TRUE(Rect::MakeXYWH(1.5, 2.3, 10.5, -1.0).isEmpty());
  EXPECT_TRUE(Rect::MakeXYWH(1.5, 2.3, 0.0, 7.2).isEmpty());
  EXPECT_TRUE(Rect::MakeXYWH(1.5, 2.3, -1.0, 7.2).isEmpty());

  // Empty for NaN in width or height or both
  EXPECT_TRUE(Rect::MakeXYWH(1.5, 2.3, 10.5, nan).isEmpty());
  EXPECT_TRUE(Rect::MakeXYWH(1.5, 2.3, nan, 7.2).isEmpty());
  EXPECT_TRUE(Rect::MakeXYWH(1.5, 2.3, nan, nan).isEmpty());
}

TEST(RectTest, IRectXYWHIsEmpty) {
  // Non-empty
  EXPECT_FALSE(IRect::MakeXYWH(1, 2, 10, 7).isEmpty());

  // Empty both width and height both 0 or negative, in all combinations
  EXPECT_TRUE(IRect::MakeXYWH(1, 2, 0, 0).isEmpty());
  EXPECT_TRUE(IRect::MakeXYWH(1, 2, -1, -1).isEmpty());
  EXPECT_TRUE(IRect::MakeXYWH(1, 2, -1, 0).isEmpty());
  EXPECT_TRUE(IRect::MakeXYWH(1, 2, 0, -1).isEmpty());

  // Empty for 0 or negative width or height (but not both at the same time)
  EXPECT_TRUE(IRect::MakeXYWH(1, 2, 10, 0).isEmpty());
  EXPECT_TRUE(IRect::MakeXYWH(1, 2, 10, -1).isEmpty());
  EXPECT_TRUE(IRect::MakeXYWH(1, 2, 0, 7).isEmpty());
  EXPECT_TRUE(IRect::MakeXYWH(1, 2, -1, 7).isEmpty());
}

TEST(RectTest, MakePointBoundsQuad) {
  Quad quad = {
    Point(10, 10),
    Point(20, 10),
    Point(10, 20),
    Point(20, 20),
  };
  std::optional<Rect> bounds = Rect::MakePointBounds(quad);
  EXPECT_TRUE(bounds.has_value());
  if (bounds.has_value()) {
    EXPECT_TRUE(RectNear(bounds.value(), Rect::MakeLTRB(10, 10, 20, 20)));
  }
}

TEST(RectTest, IsSquare) {
  EXPECT_TRUE(Rect::MakeXYWH(10, 30, 20, 20).isSquare());
  EXPECT_FALSE(Rect::MakeXYWH(10, 30, 20, 19).isSquare());
  EXPECT_FALSE(Rect::MakeXYWH(10, 30, 19, 20).isSquare());
  EXPECT_TRUE(Rect::MakeMaximum().isSquare());

  EXPECT_TRUE(IRect::MakeXYWH(10, 30, 20, 20).isSquare());
  EXPECT_FALSE(IRect::MakeXYWH(10, 30, 20, 19).isSquare());
  EXPECT_FALSE(IRect::MakeXYWH(10, 30, 19, 20).isSquare());
  EXPECT_TRUE(IRect::MakeMaximum().isSquare());
}

TEST(RectTest, GetCenter) {
  EXPECT_EQ(Rect::MakeXYWH(10, 30, 20, 20).getCenter(), Point(20, 40));
  EXPECT_EQ(Rect::MakeXYWH(10, 30, 20, 19).getCenter(), Point(20, 39.5));
  EXPECT_EQ(Rect::MakeMaximum().getCenter(), Point(0, 0));

  // Note that we expect a Point as the answer from an IRect
  EXPECT_EQ(IRect::MakeXYWH(10, 30, 20, 20).getCenter(), Point(20, 40));
  EXPECT_EQ(IRect::MakeXYWH(10, 30, 20, 19).getCenter(), Point(20, 39.5));
  EXPECT_EQ(IRect::MakeMaximum().getCenter(), Point(0, 0));
}

TEST(RectTest, RectExpand) {
  auto rect = Rect::MakeLTRB(100, 100, 200, 200);

  // Expand(T amount)
  EXPECT_EQ(rect.getExpand(10), Rect::MakeLTRB(90, 90, 210, 210));
  EXPECT_EQ(rect.getExpand(-10), Rect::MakeLTRB(110, 110, 190, 190));

  // Expand(amount, amount)
  EXPECT_EQ(rect.getExpand(10, 10), Rect::MakeLTRB(90, 90, 210, 210));
  EXPECT_EQ(rect.getExpand(10, -10), Rect::MakeLTRB(90, 110, 210, 190));
  EXPECT_EQ(rect.getExpand(-10, 10), Rect::MakeLTRB(110, 90, 190, 210));
  EXPECT_EQ(rect.getExpand(-10, -10), Rect::MakeLTRB(110, 110, 190, 190));

  // Expand(amount, amount, amount, amount)
  EXPECT_EQ(rect.getExpand(10, 20, 30, 40), Rect::MakeLTRB(90, 80, 230, 240));
  EXPECT_EQ(rect.getExpand(-10, 20, 30, 40), Rect::MakeLTRB(110, 80, 230, 240));
  EXPECT_EQ(rect.getExpand(10, -20, 30, 40), Rect::MakeLTRB(90, 120, 230, 240));
  EXPECT_EQ(rect.getExpand(10, 20, -30, 40), Rect::MakeLTRB(90, 80, 170, 240));
  EXPECT_EQ(rect.getExpand(10, 20, 30, -40), Rect::MakeLTRB(90, 80, 230, 160));

  // Expand(Point amount)
  EXPECT_EQ(rect.getExpand(Point{10, 10}), Rect::MakeLTRB(90, 90, 210, 210));
  EXPECT_EQ(rect.getExpand(Point{10, -10}), Rect::MakeLTRB(90, 110, 210, 190));
  EXPECT_EQ(rect.getExpand(Point{-10, 10}), Rect::MakeLTRB(110, 90, 190, 210));
  EXPECT_EQ(rect.getExpand(Point{-10, -10}), Rect::MakeLTRB(110, 110, 190, 190));

  // Expand(Size amount)
  EXPECT_EQ(rect.getExpand(Size{10, 10}), Rect::MakeLTRB(90, 90, 210, 210));
  EXPECT_EQ(rect.getExpand(Size{10, -10}), Rect::MakeLTRB(90, 110, 210, 190));
  EXPECT_EQ(rect.getExpand(Size{-10, 10}), Rect::MakeLTRB(110, 90, 190, 210));
  EXPECT_EQ(rect.getExpand(Size{-10, -10}), Rect::MakeLTRB(110, 110, 190, 190));
}

TEST(RectTest, IRectExpand) {
  auto rect = IRect::MakeLTRB(100, 100, 200, 200);

  // Expand(T amount)
  EXPECT_EQ(rect.getExpand(10), IRect::MakeLTRB(90, 90, 210, 210));
  EXPECT_EQ(rect.getExpand(-10), IRect::MakeLTRB(110, 110, 190, 190));

  // Expand(amount, amount)
  EXPECT_EQ(rect.getExpand(10, 10), IRect::MakeLTRB(90, 90, 210, 210));
  EXPECT_EQ(rect.getExpand(10, -10), IRect::MakeLTRB(90, 110, 210, 190));
  EXPECT_EQ(rect.getExpand(-10, 10), IRect::MakeLTRB(110, 90, 190, 210));
  EXPECT_EQ(rect.getExpand(-10, -10), IRect::MakeLTRB(110, 110, 190, 190));

  // Expand(amount, amount, amount, amount)
  EXPECT_EQ(rect.getExpand(10, 20, 30, 40), IRect::MakeLTRB(90, 80, 230, 240));
  EXPECT_EQ(rect.getExpand(-10, 20, 30, 40), IRect::MakeLTRB(110, 80, 230, 240));
  EXPECT_EQ(rect.getExpand(10, -20, 30, 40), IRect::MakeLTRB(90, 120, 230, 240));
  EXPECT_EQ(rect.getExpand(10, 20, -30, 40), IRect::MakeLTRB(90, 80, 170, 240));
  EXPECT_EQ(rect.getExpand(10, 20, 30, -40), IRect::MakeLTRB(90, 80, 230, 160));

  // Expand(IPoint amount)
  EXPECT_EQ(rect.getExpand(IPoint{10, 10}), IRect::MakeLTRB(90, 90, 210, 210));
  EXPECT_EQ(rect.getExpand(IPoint{10, -10}), IRect::MakeLTRB(90, 110, 210, 190));
  EXPECT_EQ(rect.getExpand(IPoint{-10, 10}), IRect::MakeLTRB(110, 90, 190, 210));
  EXPECT_EQ(rect.getExpand(IPoint{-10, -10}), IRect::MakeLTRB(110, 110, 190, 190));

  // Expand(ISize amount)
  EXPECT_EQ(rect.getExpand(ISize{10, 10}), IRect::MakeLTRB(90, 90, 210, 210));
  EXPECT_EQ(rect.getExpand(ISize{10, -10}), IRect::MakeLTRB(90, 110, 210, 190));
  EXPECT_EQ(rect.getExpand(ISize{-10, 10}), IRect::MakeLTRB(110, 90, 190, 210));
  EXPECT_EQ(rect.getExpand(ISize{-10, -10}), IRect::MakeLTRB(110, 110, 190, 190));
}

TEST(RectTest, ContainsFloatingPoint) {
  auto rect1 = Rect::MakeXYWH(472.599945f, 440.999969f, 1102.80005f, 654.000061f);
  auto rect2 = Rect::MakeXYWH(724.f, 618.f, 600.f, 300.f);
  EXPECT_TRUE(rect1.contains(rect2));
}

template<typename R>
static constexpr inline R flip_lr(R rect) {
  return R::MakeLTRB(rect.getRight(), rect.getTop(), rect.getLeft(), rect.getBottom());
}

template<typename R>
static constexpr inline R flip_tb(R rect) {
  return R::MakeLTRB(rect.getLeft(), rect.getBottom(), rect.getRight(), rect.getTop());
}

template<typename R>
static constexpr inline R flip_lrtb(R rect) {
  return flip_lr(flip_tb(rect));
}

static constexpr inline Rect swap_nan(const Rect& rect, int index) {
  Scalar nan = std::numeric_limits<Scalar>::quiet_NaN();
  CORAL_ASSERT(index >= 0 && index <= 15);
  Scalar l = ((index & (1 << 0)) != 0) ? nan : rect.getLeft();
  Scalar t = ((index & (1 << 1)) != 0) ? nan : rect.getTop();
  Scalar r = ((index & (1 << 2)) != 0) ? nan : rect.getRight();
  Scalar b = ((index & (1 << 3)) != 0) ? nan : rect.getBottom();
  return Rect::MakeLTRB(l, t, r, b);
}

static constexpr inline Point swap_nan(const Point& point, int index) {
  Scalar nan = std::numeric_limits<Scalar>::quiet_NaN();
  CORAL_ASSERT(index >= 0 && index <= 3);
  Scalar x = ((index & (1 << 0)) != 0) ? nan : point.x;
  Scalar y = ((index & (1 << 1)) != 0) ? nan : point.y;
  return Point(x, y);
}

TEST(RectTest, RectUnion) {
  auto check_nans = [](const Rect& a, const Rect& b, const std::string& label) {
    ASSERT_TRUE(a.isFinite()) << label;
    ASSERT_TRUE(b.isFinite()) << label;
    ASSERT_FALSE(a.getUnion(b).isEmpty());

    for (int i = 1; i < 16; i++) {
      // NaN in a produces b
      EXPECT_EQ(swap_nan(a, i).getUnion(b), b) << label << ", index = " << i;
      // NaN in b produces a
      EXPECT_EQ(a.getUnion(swap_nan(b, i)), a) << label << ", index = " << i;
      // NaN in both is empty
      for (int j = 1; j < 16; j++) {
        EXPECT_TRUE(swap_nan(a, i).getUnion(swap_nan(b, j)).isEmpty())
            << label << ", indices = " << i << ", " << j;
      }
    }
  };

  auto check_empty_flips = [](const Rect& a, const Rect& b, const std::string& label) {
    ASSERT_FALSE(a.isEmpty());
    // b is allowed to be empty

    // unflipped a vs flipped (empty) b yields a
    EXPECT_EQ(a.getUnion(flip_lr(b)), a) << label;
    EXPECT_EQ(a.getUnion(flip_tb(b)), a) << label;
    EXPECT_EQ(a.getUnion(flip_lrtb(b)), a) << label;

    // flipped (empty) a vs unflipped b yields b
    EXPECT_EQ(flip_lr(a).getUnion(b), b) << label;
    EXPECT_EQ(flip_tb(a).getUnion(b), b) << label;
    EXPECT_EQ(flip_lrtb(a).getUnion(b), b) << label;

    // flipped (empty) a vs flipped (empty) b yields empty
    EXPECT_TRUE(flip_lr(a).getUnion(flip_lr(b)).isEmpty()) << label;
    EXPECT_TRUE(flip_tb(a).getUnion(flip_tb(b)).isEmpty()) << label;
    EXPECT_TRUE(flip_lrtb(a).getUnion(flip_lrtb(b)).isEmpty()) << label;
  };

  auto test = [&check_nans, &check_empty_flips](const Rect& a, const Rect& b, const Rect& result) {
    ASSERT_FALSE(a.isEmpty()) << a;
    // b is allowed to be empty

    std::stringstream stream;
    stream << a << " union " << b;
    auto label = stream.str();

    EXPECT_EQ(a.getUnion(b), result) << label;
    EXPECT_EQ(b.getUnion(a), result) << label;
    check_empty_flips(a, b, label);
    check_nans(a, b, label);
  };

  {
    auto a = Rect::MakeXYWH(100, 100, 100, 100);
    auto b = Rect::MakeXYWH(0, 0, 0, 0);
    auto expected = Rect::MakeXYWH(100, 100, 100, 100);
    test(a, b, expected);
  }

  {
    auto a = Rect::MakeXYWH(100, 100, 100, 100);
    auto b = Rect::MakeXYWH(0, 0, 1, 1);
    auto expected = Rect::MakeXYWH(0, 0, 200, 200);
    test(a, b, expected);
  }

  {
    auto a = Rect::MakeXYWH(100, 100, 100, 100);
    auto b = Rect::MakeXYWH(10, 10, 1, 1);
    auto expected = Rect::MakeXYWH(10, 10, 190, 190);
    test(a, b, expected);
  }

  {
    auto a = Rect::MakeXYWH(0, 0, 100, 100);
    auto b = Rect::MakeXYWH(10, 10, 100, 100);
    auto expected = Rect::MakeXYWH(0, 0, 110, 110);
    test(a, b, expected);
  }

  {
    auto a = Rect::MakeXYWH(0, 0, 100, 100);
    auto b = Rect::MakeXYWH(100, 100, 100, 100);
    auto expected = Rect::MakeXYWH(0, 0, 200, 200);
    test(a, b, expected);
  }
}

TEST(RectTest, OptRectUnion) {
  auto a = Rect::MakeLTRB(0, 0, 100, 100);
  auto b = Rect::MakeLTRB(100, 100, 200, 200);
  auto c = Rect::MakeLTRB(100, 0, 200, 100);

  // NullOpt, NullOpt
  EXPECT_FALSE(Rect::Union(std::nullopt, std::nullopt).has_value());
  EXPECT_EQ(Rect::Union(std::nullopt, std::nullopt), std::nullopt);

  auto test1 = [](const Rect& r) {
    // Rect, NullOpt
    EXPECT_EQ(Rect::Union(r, std::nullopt), r);

    // OptRect, NullOpt
    EXPECT_TRUE(Rect::Union(std::optional(r), std::nullopt).has_value());
    EXPECT_EQ(Rect::Union(std::optional(r), std::nullopt).value(), r);

    // NullOpt, Rect
    EXPECT_EQ(Rect::Union(std::nullopt, r), r);

    // NullOpt, OptRect
    EXPECT_TRUE(Rect::Union(std::nullopt, std::optional(r)).has_value());
    EXPECT_EQ(Rect::Union(std::nullopt, std::optional(r)).value(), r);
  };

  test1(a);
  test1(b);
  test1(c);

  auto test2 = [](const Rect& a, const Rect& b, const Rect& u) {
    ASSERT_EQ(a.getUnion(b), u);

    // Rect, OptRect
    EXPECT_EQ(Rect::Union(a, std::optional(b)), u);

    // OptRect, Rect
    EXPECT_EQ(Rect::Union(std::optional(a), b), u);

    // OptRect, OptRect
    EXPECT_TRUE(Rect::Union(std::optional(a), std::optional(b)).has_value());
    EXPECT_EQ(Rect::Union(std::optional(a), std::optional(b)).value(), u);
  };

  test2(a, b, Rect::MakeLTRB(0, 0, 200, 200));
  test2(a, c, Rect::MakeLTRB(0, 0, 200, 100));
  test2(b, c, Rect::MakeLTRB(100, 0, 200, 200));
}

TEST(RectTest, IRectUnion) {
  auto check_empty_flips = [](const IRect& a, const IRect& b, const std::string& label) {
    ASSERT_FALSE(a.isEmpty());
    // b is allowed to be empty

    // unflipped a vs flipped (empty) b yields a
    EXPECT_EQ(a.getUnion(flip_lr(b)), a) << label;
    EXPECT_EQ(a.getUnion(flip_tb(b)), a) << label;
    EXPECT_EQ(a.getUnion(flip_lrtb(b)), a) << label;

    // flipped (empty) a vs unflipped b yields b
    EXPECT_EQ(flip_lr(a).getUnion(b), b) << label;
    EXPECT_EQ(flip_tb(a).getUnion(b), b) << label;
    EXPECT_EQ(flip_lrtb(a).getUnion(b), b) << label;

    // flipped (empty) a vs flipped (empty) b yields empty
    EXPECT_TRUE(flip_lr(a).getUnion(flip_lr(b)).isEmpty()) << label;
    EXPECT_TRUE(flip_tb(a).getUnion(flip_tb(b)).isEmpty()) << label;
    EXPECT_TRUE(flip_lrtb(a).getUnion(flip_lrtb(b)).isEmpty()) << label;
  };

  auto test = [&check_empty_flips](const IRect& a, const IRect& b,
    const IRect& result) {
    ASSERT_FALSE(a.isEmpty()) << a;
    // b is allowed to be empty

    std::stringstream stream;
    stream << a << " union " << b;
    auto label = stream.str();

    EXPECT_EQ(a.getUnion(b), result) << label;
    EXPECT_EQ(b.getUnion(a), result) << label;
    check_empty_flips(a, b, label);
  };

  {
    auto a = IRect::MakeXYWH(100, 100, 100, 100);
    auto b = IRect::MakeXYWH(0, 0, 0, 0);
    auto expected = IRect::MakeXYWH(100, 100, 100, 100);
    test(a, b, expected);
  }

  {
    auto a = IRect::MakeXYWH(100, 100, 100, 100);
    auto b = IRect::MakeXYWH(0, 0, 1, 1);
    auto expected = IRect::MakeXYWH(0, 0, 200, 200);
    test(a, b, expected);
  }

  {
    auto a = IRect::MakeXYWH(100, 100, 100, 100);
    auto b = IRect::MakeXYWH(10, 10, 1, 1);
    auto expected = IRect::MakeXYWH(10, 10, 190, 190);
    test(a, b, expected);
  }

  {
    auto a = IRect::MakeXYWH(0, 0, 100, 100);
    auto b = IRect::MakeXYWH(10, 10, 100, 100);
    auto expected = IRect::MakeXYWH(0, 0, 110, 110);
    test(a, b, expected);
  }

  {
    auto a = IRect::MakeXYWH(0, 0, 100, 100);
    auto b = IRect::MakeXYWH(100, 100, 100, 100);
    auto expected = IRect::MakeXYWH(0, 0, 200, 200);
    test(a, b, expected);
  }
}

TEST(RectTest, OptIRectUnion) {
  auto a = IRect::MakeLTRB(0, 0, 100, 100);
  auto b = IRect::MakeLTRB(100, 100, 200, 200);
  auto c = IRect::MakeLTRB(100, 0, 200, 100);

  // NullOpt, NullOpt
  EXPECT_FALSE(IRect::Union(std::nullopt, std::nullopt).has_value());
  EXPECT_EQ(IRect::Union(std::nullopt, std::nullopt), std::nullopt);

  auto test1 = [](const IRect& r) {
    // Rect, NullOpt
    EXPECT_EQ(IRect::Union(r, std::nullopt), r);

    // OptRect, NullOpt
    EXPECT_TRUE(IRect::Union(std::optional(r), std::nullopt).has_value());
    EXPECT_EQ(IRect::Union(std::optional(r), std::nullopt).value(), r);

    // NullOpt, Rect
    EXPECT_EQ(IRect::Union(std::nullopt, r), r);

    // NullOpt, OptRect
    EXPECT_TRUE(IRect::Union(std::nullopt, std::optional(r)).has_value());
    EXPECT_EQ(IRect::Union(std::nullopt, std::optional(r)).value(), r);
  };

  test1(a);
  test1(b);
  test1(c);

  auto test2 = [](const IRect& a, const IRect& b, const IRect& u) {
    ASSERT_EQ(a.getUnion(b), u);

    // Rect, OptRect
    EXPECT_EQ(IRect::Union(a, std::optional(b)), u);

    // OptRect, Rect
    EXPECT_EQ(IRect::Union(std::optional(a), b), u);

    // OptRect, OptRect
    EXPECT_TRUE(IRect::Union(std::optional(a), std::optional(b)).has_value());
    EXPECT_EQ(IRect::Union(std::optional(a), std::optional(b)).value(), u);
  };

  test2(a, b, IRect::MakeLTRB(0, 0, 200, 200));
  test2(a, c, IRect::MakeLTRB(0, 0, 200, 100));
  test2(b, c, IRect::MakeLTRB(100, 0, 200, 200));
}

TEST(RectTest, RectIntersection) {
  auto check_nans = [](const Rect& a, const Rect& b, const std::string& label) {
    ASSERT_TRUE(a.isFinite()) << label;
    ASSERT_TRUE(b.isFinite()) << label;

    for (int i = 1; i < 16; i++) {
      // NaN in a produces empty
      EXPECT_FALSE(swap_nan(a, i).getIntersection(b).has_value())
          << label << ", index = " << i;
      // NaN in b produces empty
      EXPECT_FALSE(a.getIntersection(swap_nan(b, i)).has_value())
          << label << ", index = " << i;
      // NaN in both is empty
      for (int j = 1; j < 16; j++) {
        EXPECT_FALSE(swap_nan(a, i).getIntersection(swap_nan(b, j)).has_value())
            << label << ", indices = " << i << ", " << j;
      }
    }
  };

  auto check_empty_flips = [](const Rect& a, const Rect& b, const std::string& label) {
    ASSERT_FALSE(a.isEmpty());
    // b is allowed to be empty

    // unflipped a vs flipped (empty) b yields a
    EXPECT_FALSE(a.getIntersection(flip_lr(b)).has_value()) << label;
    EXPECT_TRUE(a.getIntersectionOrEmpty(flip_lr(b)).isEmpty()) << label;
    EXPECT_FALSE(a.getIntersection(flip_tb(b)).has_value()) << label;
    EXPECT_TRUE(a.getIntersectionOrEmpty(flip_tb(b)).isEmpty()) << label;
    EXPECT_FALSE(a.getIntersection(flip_lrtb(b)).has_value()) << label;
    EXPECT_TRUE(a.getIntersectionOrEmpty(flip_lrtb(b)).isEmpty()) << label;

    // flipped (empty) a vs unflipped b yields b
    EXPECT_FALSE(flip_lr(a).getIntersection(b).has_value()) << label;
    EXPECT_TRUE(flip_lr(a).getIntersectionOrEmpty(b).isEmpty()) << label;
    EXPECT_FALSE(flip_tb(a).getIntersection(b).has_value()) << label;
    EXPECT_TRUE(flip_tb(a).getIntersectionOrEmpty(b).isEmpty()) << label;
    EXPECT_FALSE(flip_lrtb(a).getIntersection(b).has_value()) << label;
    EXPECT_TRUE(flip_lrtb(a).getIntersectionOrEmpty(b).isEmpty()) << label;

    // flipped (empty) a vs flipped (empty) b yields empty
    EXPECT_FALSE(flip_lr(a).getIntersection(flip_lr(b)).has_value()) << label;
    EXPECT_TRUE(flip_lr(a).getIntersectionOrEmpty(flip_lr(b)).isEmpty()) << label;
    EXPECT_FALSE(flip_tb(a).getIntersection(flip_tb(b)).has_value()) << label;
    EXPECT_TRUE(flip_tb(a).getIntersectionOrEmpty(flip_tb(b)).isEmpty()) << label;
    EXPECT_FALSE(flip_lrtb(a).getIntersection(flip_lrtb(b)).has_value()) << label;
    EXPECT_TRUE(flip_lrtb(a).getIntersectionOrEmpty(flip_lrtb(b)).isEmpty()) << label;
  };

  auto test_non_empty = [&check_nans, &check_empty_flips](
    const Rect& a, const Rect& b, const Rect& result) {
    ASSERT_FALSE(a.isEmpty()) << a;
    // b is allowed to be empty

    std::stringstream stream;
    stream << a << " union " << b;
    auto label = stream.str();

    EXPECT_TRUE(a.getIntersection(b).has_value()) << label;
    EXPECT_TRUE(b.getIntersection(a).has_value()) << label;
    EXPECT_EQ(a.getIntersection(b), result) << label;
    EXPECT_EQ(b.getIntersection(a), result) << label;
    check_empty_flips(a, b, label);
    check_nans(a, b, label);
  };

  auto test_empty = [&check_nans, &check_empty_flips](const Rect& a, const Rect& b) {
    ASSERT_FALSE(a.isEmpty()) << a;
    // b is allowed to be empty

    std::stringstream stream;
    stream << a << " union " << b;
    auto label = stream.str();

    EXPECT_FALSE(a.getIntersection(b).has_value()) << label;
    EXPECT_TRUE(a.getIntersectionOrEmpty(b).isEmpty()) << label;
    EXPECT_FALSE(b.getIntersection(a).has_value()) << label;
    EXPECT_TRUE(b.getIntersectionOrEmpty(a).isEmpty()) << label;
    check_empty_flips(a, b, label);
    check_nans(a, b, label);
  };

  {
    auto a = Rect::MakeXYWH(100, 100, 100, 100);
    auto b = Rect::MakeXYWH(0, 0, 0, 0);

    test_empty(a, b);
  }

  {
    auto a = Rect::MakeXYWH(100, 100, 100, 100);
    auto b = Rect::MakeXYWH(10, 10, 0, 0);

    test_empty(a, b);
  }

  {
    auto a = Rect::MakeXYWH(0, 0, 100, 100);
    auto b = Rect::MakeXYWH(10, 10, 100, 100);
    auto expected = Rect::MakeXYWH(10, 10, 90, 90);

    test_non_empty(a, b, expected);
  }

  {
    auto a = Rect::MakeXYWH(0, 0, 100, 100);
    auto b = Rect::MakeXYWH(100, 100, 100, 100);

    test_empty(a, b);
  }

  {
    auto a = Rect::MakeMaximum();
    auto b = Rect::MakeXYWH(10, 10, 300, 300);

    test_non_empty(a, b, b);
  }

  {
    auto a = Rect::MakeMaximum();
    auto b = Rect::MakeMaximum();

    test_non_empty(a, b, Rect::MakeMaximum());
  }
}

TEST(RectTest, OptRectIntersection) {
  auto a = Rect::MakeLTRB(0, 0, 110, 110);
  auto b = Rect::MakeLTRB(100, 100, 200, 200);
  auto c = Rect::MakeLTRB(100, 0, 200, 110);

  // NullOpt, NullOpt
  EXPECT_FALSE(Rect::Intersection(std::nullopt, std::nullopt).has_value());
  EXPECT_EQ(Rect::Intersection(std::nullopt, std::nullopt), std::nullopt);

  auto test1 = [](const Rect& r) {
    // Rect, NullOpt
    EXPECT_TRUE(Rect::Intersection(r, std::nullopt).has_value());
    EXPECT_EQ(Rect::Intersection(r, std::nullopt).value(), r);

    // OptRect, NullOpt
    EXPECT_TRUE(Rect::Intersection(std::optional(r), std::nullopt).has_value());
    EXPECT_EQ(Rect::Intersection(std::optional(r), std::nullopt).value(), r);

    // NullOpt, Rect
    EXPECT_TRUE(Rect::Intersection(std::nullopt, r).has_value());
    EXPECT_EQ(Rect::Intersection(std::nullopt, r).value(), r);

    // NullOpt, OptRect
    EXPECT_TRUE(Rect::Intersection(std::nullopt, std::optional(r)).has_value());
    EXPECT_EQ(Rect::Intersection(std::nullopt, std::optional(r)).value(), r);
  };

  test1(a);
  test1(b);
  test1(c);

  auto test2 = [](const Rect& a, const Rect& b, const Rect& i) {
    ASSERT_EQ(a.getIntersection(b), i);

    // Rect, OptRect
    EXPECT_TRUE(Rect::Intersection(a, std::optional(b)).has_value());
    EXPECT_EQ(Rect::Intersection(a, std::optional(b)).value(), i);

    // OptRect, Rect
    EXPECT_TRUE(Rect::Intersection(std::optional(a), b).has_value());
    EXPECT_EQ(Rect::Intersection(std::optional(a), b).value(), i);

    // OptRect, OptRect
    EXPECT_TRUE(Rect::Intersection(std::optional(a), std::optional(b)).has_value());
    EXPECT_EQ(Rect::Intersection(std::optional(a), std::optional(b)).value(), i);
  };

  test2(a, b, Rect::MakeLTRB(100, 100, 110, 110));
  test2(a, c, Rect::MakeLTRB(100, 0, 110, 110));
  test2(b, c, Rect::MakeLTRB(100, 100, 200, 110));
}

TEST(RectTest, IRectIntersection) {
  auto check_empty_flips = [](const IRect& a, const IRect& b, const std::string& label) {
    ASSERT_FALSE(a.isEmpty());
    // b is allowed to be empty

    // unflipped a vs flipped (empty) b yields a
    EXPECT_FALSE(a.getIntersection(flip_lr(b)).has_value()) << label;
    EXPECT_FALSE(a.getIntersection(flip_tb(b)).has_value()) << label;
    EXPECT_FALSE(a.getIntersection(flip_lrtb(b)).has_value()) << label;

    // flipped (empty) a vs unflipped b yields b
    EXPECT_FALSE(flip_lr(a).getIntersection(b).has_value()) << label;
    EXPECT_FALSE(flip_tb(a).getIntersection(b).has_value()) << label;
    EXPECT_FALSE(flip_lrtb(a).getIntersection(b).has_value()) << label;

    // flipped (empty) a vs flipped (empty) b yields empty
    EXPECT_FALSE(flip_lr(a).getIntersection(flip_lr(b)).has_value()) << label;
    EXPECT_FALSE(flip_tb(a).getIntersection(flip_tb(b)).has_value()) << label;
    EXPECT_FALSE(flip_lrtb(a).getIntersection(flip_lrtb(b)).has_value()) << label;
  };

  auto test_non_empty = [&check_empty_flips](const IRect& a, const IRect& b, const IRect& result) {
    ASSERT_FALSE(a.isEmpty()) << a;
    // b is allowed to be empty

    std::stringstream stream;
    stream << a << " union " << b;
    auto label = stream.str();

    EXPECT_TRUE(a.getIntersection(b).has_value()) << label;
    EXPECT_TRUE(b.getIntersection(a).has_value()) << label;
    EXPECT_EQ(a.getIntersection(b), result) << label;
    EXPECT_EQ(b.getIntersection(a), result) << label;
    check_empty_flips(a, b, label);
  };

  auto test_empty = [&check_empty_flips](const IRect& a, const IRect& b) {
    ASSERT_FALSE(a.isEmpty()) << a;
    // b is allowed to be empty

    std::stringstream stream;
    stream << a << " union " << b;
    auto label = stream.str();

    EXPECT_FALSE(a.getIntersection(b).has_value()) << label;
    EXPECT_FALSE(b.getIntersection(a).has_value()) << label;
    check_empty_flips(a, b, label);
  };

  {
    auto a = IRect::MakeXYWH(100, 100, 100, 100);
    auto b = IRect::MakeXYWH(0, 0, 0, 0);

    test_empty(a, b);
  }

  {
    auto a = IRect::MakeXYWH(100, 100, 100, 100);
    auto b = IRect::MakeXYWH(10, 10, 0, 0);

    test_empty(a, b);
  }

  {
    auto a = IRect::MakeXYWH(0, 0, 100, 100);
    auto b = IRect::MakeXYWH(10, 10, 100, 100);
    auto expected = IRect::MakeXYWH(10, 10, 90, 90);

    test_non_empty(a, b, expected);
  }

  {
    auto a = IRect::MakeXYWH(0, 0, 100, 100);
    auto b = IRect::MakeXYWH(100, 100, 100, 100);

    test_empty(a, b);
  }

  {
    auto a = IRect::MakeMaximum();
    auto b = IRect::MakeXYWH(10, 10, 300, 300);

    test_non_empty(a, b, b);
  }

  {
    auto a = IRect::MakeMaximum();
    auto b = IRect::MakeMaximum();

    test_non_empty(a, b, IRect::MakeMaximum());
  }
}

TEST(RectTest, OptIRectIntersection) {
  auto a = IRect::MakeLTRB(0, 0, 110, 110);
  auto b = IRect::MakeLTRB(100, 100, 200, 200);
  auto c = IRect::MakeLTRB(100, 0, 200, 110);

  // NullOpt, NullOpt
  EXPECT_FALSE(IRect::Intersection(std::nullopt, std::nullopt).has_value());
  EXPECT_EQ(IRect::Intersection(std::nullopt, std::nullopt), std::nullopt);

  auto test1 = [](const IRect& r) {
    // Rect, NullOpt
    EXPECT_TRUE(IRect::Intersection(r, std::nullopt).has_value());
    EXPECT_EQ(IRect::Intersection(r, std::nullopt).value(), r);

    // OptRect, NullOpt
    EXPECT_TRUE(IRect::Intersection(std::optional(r), std::nullopt).has_value());
    EXPECT_EQ(IRect::Intersection(std::optional(r), std::nullopt).value(), r);

    // NullOpt, Rect
    EXPECT_TRUE(IRect::Intersection(std::nullopt, r).has_value());
    EXPECT_EQ(IRect::Intersection(std::nullopt, r).value(), r);

    // NullOpt, OptRect
    EXPECT_TRUE(IRect::Intersection(std::nullopt, std::optional(r)).has_value());
    EXPECT_EQ(IRect::Intersection(std::nullopt, std::optional(r)).value(), r);
  };

  test1(a);
  test1(b);
  test1(c);

  auto test2 = [](const IRect& a, const IRect& b, const IRect& i) {
    ASSERT_EQ(a.getIntersection(b), i);

    // Rect, OptRect
    EXPECT_TRUE(IRect::Intersection(a, std::optional(b)).has_value());
    EXPECT_EQ(IRect::Intersection(a, std::optional(b)).value(), i);

    // OptRect, Rect
    EXPECT_TRUE(IRect::Intersection(std::optional(a), b).has_value());
    EXPECT_EQ(IRect::Intersection(std::optional(a), b).value(), i);

    // OptRect, OptRect
    EXPECT_TRUE(IRect::Intersection(std::optional(a), std::optional(b)).has_value());
    EXPECT_EQ(IRect::Intersection(std::optional(a), std::optional(b)).value(), i);
  };

  test2(a, b, IRect::MakeLTRB(100, 100, 110, 110));
  test2(a, c, IRect::MakeLTRB(100, 0, 110, 110));
  test2(b, c, IRect::MakeLTRB(100, 100, 200, 110));
}

TEST(RectTest, RectIntersectsWithRect) {
  auto check_nans = [](const Rect& a, const Rect& b, const std::string& label) {
    ASSERT_TRUE(a.isFinite()) << label;
    ASSERT_TRUE(b.isFinite()) << label;

    for (int i = 1; i < 16; i++) {
      // NaN in a produces b
      EXPECT_FALSE(swap_nan(a, i).isIntersectsWithRect(b))
          << label << ", index = " << i;
      // NaN in b produces a
      EXPECT_FALSE(a.isIntersectsWithRect(swap_nan(b, i)))
          << label << ", index = " << i;
      // NaN in both is empty
      for (int j = 1; j < 16; j++) {
        EXPECT_FALSE(swap_nan(a, i).isIntersectsWithRect(swap_nan(b, j)))
            << label << ", indices = " << i << ", " << j;
      }
    }
  };

  auto check_empty_flips = [](const Rect& a, const Rect& b, const std::string& label) {
    ASSERT_FALSE(a.isEmpty());
    // b is allowed to be empty

    // unflipped a vs flipped (empty) b yields a
    EXPECT_FALSE(a.isIntersectsWithRect(flip_lr(b))) << label;
    EXPECT_FALSE(a.isIntersectsWithRect(flip_tb(b))) << label;
    EXPECT_FALSE(a.isIntersectsWithRect(flip_lrtb(b))) << label;

    // flipped (empty) a vs unflipped b yields b
    EXPECT_FALSE(flip_lr(a).isIntersectsWithRect(b)) << label;
    EXPECT_FALSE(flip_tb(a).isIntersectsWithRect(b)) << label;
    EXPECT_FALSE(flip_lrtb(a).isIntersectsWithRect(b)) << label;

    // flipped (empty) a vs flipped (empty) b yields empty
    EXPECT_FALSE(flip_lr(a).isIntersectsWithRect(flip_lr(b))) << label;
    EXPECT_FALSE(flip_tb(a).isIntersectsWithRect(flip_tb(b))) << label;
    EXPECT_FALSE(flip_lrtb(a).isIntersectsWithRect(flip_lrtb(b))) << label;
  };

  auto test_non_empty = [&check_nans, &check_empty_flips](const Rect& a, const Rect& b) {
    ASSERT_FALSE(a.isEmpty()) << a;
    // b is allowed to be empty

    std::stringstream stream;
    stream << a << " union " << b;
    auto label = stream.str();

    EXPECT_TRUE(a.isIntersectsWithRect(b)) << label;
    EXPECT_TRUE(b.isIntersectsWithRect(a)) << label;
    check_empty_flips(a, b, label);
    check_nans(a, b, label);
  };

  auto test_empty = [&check_nans, &check_empty_flips](const Rect& a, const Rect& b) {
    ASSERT_FALSE(a.isEmpty()) << a;
    // b is allowed to be empty

    std::stringstream stream;
    stream << a << " union " << b;
    auto label = stream.str();

    EXPECT_FALSE(a.isIntersectsWithRect(b)) << label;
    EXPECT_FALSE(b.isIntersectsWithRect(a)) << label;
    check_empty_flips(a, b, label);
    check_nans(a, b, label);
  };

  {
    auto a = Rect::MakeXYWH(100, 100, 100, 100);
    auto b = Rect::MakeXYWH(0, 0, 0, 0);

    test_empty(a, b);
  }

  {
    auto a = Rect::MakeXYWH(100, 100, 100, 100);
    auto b = Rect::MakeXYWH(10, 10, 0, 0);

    test_empty(a, b);
  }

  {
    auto a = Rect::MakeXYWH(0, 0, 100, 100);
    auto b = Rect::MakeXYWH(10, 10, 100, 100);

    test_non_empty(a, b);
  }

  {
    auto a = Rect::MakeXYWH(0, 0, 100, 100);
    auto b = Rect::MakeXYWH(100, 100, 100, 100);

    test_empty(a, b);
  }

  {
    auto a = Rect::MakeMaximum();
    auto b = Rect::MakeXYWH(10, 10, 100, 100);

    test_non_empty(a, b);
  }

  {
    auto a = Rect::MakeMaximum();
    auto b = Rect::MakeMaximum();

    test_non_empty(a, b);
  }
}

TEST(RectTest, IRectIntersectsWithRect) {
  auto check_empty_flips = [](const IRect& a, const IRect& b, const std::string& label) {
    ASSERT_FALSE(a.isEmpty());
    // b is allowed to be empty

    // unflipped a vs flipped (empty) b yields a
    EXPECT_FALSE(a.isIntersectsWithRect(flip_lr(b))) << label;
    EXPECT_FALSE(a.isIntersectsWithRect(flip_tb(b))) << label;
    EXPECT_FALSE(a.isIntersectsWithRect(flip_lrtb(b))) << label;

    // flipped (empty) a vs unflipped b yields b
    EXPECT_FALSE(flip_lr(a).isIntersectsWithRect(b)) << label;
    EXPECT_FALSE(flip_tb(a).isIntersectsWithRect(b)) << label;
    EXPECT_FALSE(flip_lrtb(a).isIntersectsWithRect(b)) << label;

    // flipped (empty) a vs flipped (empty) b yields empty
    EXPECT_FALSE(flip_lr(a).isIntersectsWithRect(flip_lr(b))) << label;
    EXPECT_FALSE(flip_tb(a).isIntersectsWithRect(flip_tb(b))) << label;
    EXPECT_FALSE(flip_lrtb(a).isIntersectsWithRect(flip_lrtb(b))) << label;
  };

  auto test_non_empty = [&check_empty_flips](const IRect& a, const IRect& b) {
    ASSERT_FALSE(a.isEmpty()) << a;
    // b is allowed to be empty

    std::stringstream stream;
    stream << a << " union " << b;
    auto label = stream.str();

    EXPECT_TRUE(a.isIntersectsWithRect(b)) << label;
    EXPECT_TRUE(b.isIntersectsWithRect(a)) << label;
    check_empty_flips(a, b, label);
  };

  auto test_empty = [&check_empty_flips](const IRect& a, const IRect& b) {
    ASSERT_FALSE(a.isEmpty()) << a;
    // b is allowed to be empty

    std::stringstream stream;
    stream << a << " union " << b;
    auto label = stream.str();

    EXPECT_FALSE(a.isIntersectsWithRect(b)) << label;
    EXPECT_FALSE(b.isIntersectsWithRect(a)) << label;
    check_empty_flips(a, b, label);
  };

  {
    auto a = IRect::MakeXYWH(100, 100, 100, 100);
    auto b = IRect::MakeXYWH(0, 0, 0, 0);

    test_empty(a, b);
  }

  {
    auto a = IRect::MakeXYWH(100, 100, 100, 100);
    auto b = IRect::MakeXYWH(10, 10, 0, 0);

    test_empty(a, b);
  }

  {
    auto a = IRect::MakeXYWH(0, 0, 100, 100);
    auto b = IRect::MakeXYWH(10, 10, 100, 100);

    test_non_empty(a, b);
  }

  {
    auto a = IRect::MakeXYWH(0, 0, 100, 100);
    auto b = IRect::MakeXYWH(100, 100, 100, 100);

    test_empty(a, b);
  }

  {
    auto a = IRect::MakeMaximum();
    auto b = IRect::MakeXYWH(10, 10, 100, 100);

    test_non_empty(a, b);
  }

  {
    auto a = IRect::MakeMaximum();
    auto b = IRect::MakeMaximum();

    test_non_empty(a, b);
  }
}

TEST(RectTest, RectContainsPoint) {
  auto check_nans = [](const Rect& rect, const Point& point, const std::string& label) {
    ASSERT_TRUE(rect.isFinite()) << label;
    ASSERT_TRUE(point.isFinite()) << label;

    for (int i = 1; i < 16; i++) {
      EXPECT_FALSE(swap_nan(rect, i).contains(point)) << label << ", index = " << i;
      for (int j = 1; j < 4; j++) {
        EXPECT_FALSE(swap_nan(rect, i).contains(swap_nan(point, j)))
            << label << ", indices = " << i << ", " << j;
      }
    }
  };

  auto check_empty_flips = [](const Rect& rect, const Point& point, const std::string& label) {
    ASSERT_FALSE(rect.isEmpty());

    EXPECT_FALSE(flip_lr(rect).contains(point)) << label;
    EXPECT_FALSE(flip_tb(rect).contains(point)) << label;
    EXPECT_FALSE(flip_lrtb(rect).contains(point)) << label;
  };

  auto test_inside = [&check_nans, &check_empty_flips](const Rect& rect, const Point& point) {
    ASSERT_FALSE(rect.isEmpty()) << rect;

    std::stringstream stream;
    stream << rect << " contains " << point;
    auto label = stream.str();

    EXPECT_TRUE(rect.contains(point)) << label;
    check_empty_flips(rect, point, label);
    check_nans(rect, point, label);
  };

  auto test_outside = [&check_nans, &check_empty_flips](const Rect& rect,
    const Point& point) {
    ASSERT_FALSE(rect.isEmpty()) << rect;

    std::stringstream stream;
    stream << rect << " contains " << point;
    auto label = stream.str();

    EXPECT_FALSE(rect.contains(point)) << label;
    check_empty_flips(rect, point, label);
    check_nans(rect, point, label);
  };

  {
    // Origin is inclusive
    auto r = Rect::MakeXYWH(100, 100, 100, 100);
    auto p = Point(100, 100);

    test_inside(r, p);
  }
  {
    // Size is exclusive
    auto r = Rect::MakeXYWH(100, 100, 100, 100);
    auto p = Point(200, 200);

    test_outside(r, p);
  }
  {
    auto r = Rect::MakeXYWH(100, 100, 100, 100);
    auto p = Point(99, 99);

    test_outside(r, p);
  }
  {
    auto r = Rect::MakeXYWH(100, 100, 100, 100);
    auto p = Point(199, 199);

    test_inside(r, p);
  }

  {
    auto r = Rect::MakeMaximum();
    auto p = Point(199, 199);

    test_inside(r, p);
  }
}

TEST(RectTest, IRectContainsIPoint) {
  auto check_empty_flips = [](const IRect& rect, const IPoint& point, const std::string& label) {
    ASSERT_FALSE(rect.isEmpty());

    EXPECT_FALSE(flip_lr(rect).contains(point)) << label;
    EXPECT_FALSE(flip_tb(rect).contains(point)) << label;
    EXPECT_FALSE(flip_lrtb(rect).contains(point)) << label;
  };

  auto test_inside = [&check_empty_flips](const IRect& rect, const IPoint& point) {
    ASSERT_FALSE(rect.isEmpty()) << rect;

    std::stringstream stream;
    stream << rect << " contains " << point;
    auto label = stream.str();

    EXPECT_TRUE(rect.contains(point)) << label;
    check_empty_flips(rect, point, label);
  };

  auto test_outside = [&check_empty_flips](const IRect& rect, const IPoint& point) {
    ASSERT_FALSE(rect.isEmpty()) << rect;

    std::stringstream stream;
    stream << rect << " contains " << point;
    auto label = stream.str();

    EXPECT_FALSE(rect.contains(point)) << label;
    check_empty_flips(rect, point, label);
  };

  {
    // Origin is inclusive
    auto r = IRect::MakeXYWH(100, 100, 100, 100);
    auto p = IPoint(100, 100);

    test_inside(r, p);
  }
  {
    // Size is exclusive
    auto r = IRect::MakeXYWH(100, 100, 100, 100);
    auto p = IPoint(200, 200);

    test_outside(r, p);
  }
  {
    auto r = IRect::MakeXYWH(100, 100, 100, 100);
    auto p = IPoint(99, 99);

    test_outside(r, p);
  }
  {
    auto r = IRect::MakeXYWH(100, 100, 100, 100);
    auto p = IPoint(199, 199);

    test_inside(r, p);
  }

  {
    auto r = IRect::MakeMaximum();
    auto p = IPoint(199, 199);

    test_inside(r, p);
  }
}

TEST(RectTest, RectContainsInclusivePoint) {
  auto check_nans = [](const Rect& rect, const Point& point, const std::string& label) {
    ASSERT_TRUE(rect.isFinite()) << label;
    ASSERT_TRUE(point.isFinite()) << label;

    for (int i = 1; i < 16; i++) {
      EXPECT_FALSE(swap_nan(rect, i).containsInclusive(point))
          << label << ", index = " << i;
      for (int j = 1; j < 4; j++) {
        EXPECT_FALSE(swap_nan(rect, i).containsInclusive(swap_nan(point, j)))
            << label << ", indices = " << i << ", " << j;
      }
    }
  };

  auto check_empty_flips = [](const Rect& rect, const Point& point, const std::string& label) {
    ASSERT_FALSE(rect.isEmpty());

    EXPECT_FALSE(flip_lr(rect).containsInclusive(point)) << label;
    EXPECT_FALSE(flip_tb(rect).containsInclusive(point)) << label;
    EXPECT_FALSE(flip_lrtb(rect).containsInclusive(point)) << label;
  };

  auto test_inside = [&check_nans, &check_empty_flips](const Rect& rect, const Point& point) {
    ASSERT_FALSE(rect.isEmpty()) << rect;

    std::stringstream stream;
    stream << rect << " contains " << point;
    auto label = stream.str();

    EXPECT_TRUE(rect.containsInclusive(point)) << label;
    check_empty_flips(rect, point, label);
    check_nans(rect, point, label);
  };

  auto test_outside = [&check_nans, &check_empty_flips](const Rect& rect, const Point& point) {
    ASSERT_FALSE(rect.isEmpty()) << rect;

    std::stringstream stream;
    stream << rect << " contains " << point;
    auto label = stream.str();

    EXPECT_FALSE(rect.containsInclusive(point)) << label;
    check_empty_flips(rect, point, label);
    check_nans(rect, point, label);
  };

  {
    // Origin is inclusive
    auto r = Rect::MakeXYWH(100, 100, 100, 100);
    auto p = Point(100, 100);

    test_inside(r, p);
  }
  {
    // Size is inclusive
    auto r = Rect::MakeXYWH(100, 100, 100, 100);
    auto p = Point(200, 200);

    test_inside(r, p);
  }
  {
    // Size + epsilon is exclusive
    auto r = Rect::MakeXYWH(100, 100, 100, 100);
    auto p = Point(200 + kEhCloseEnough, 200 + kEhCloseEnough);

    test_outside(r, p);
  }
  {
    auto r = Rect::MakeXYWH(100, 100, 100, 100);
    auto p = Point(99, 99);

    test_outside(r, p);
  }
  {
    auto r = Rect::MakeXYWH(100, 100, 100, 100);
    auto p = Point(199, 199);

    test_inside(r, p);
  }

  {
    auto r = Rect::MakeMaximum();
    auto p = Point(199, 199);

    test_inside(r, p);
  }
}

TEST(RectTest, IRectContainsInclusiveIPoint) {
  auto check_empty_flips = [](const IRect& rect, const IPoint& point, const std::string& label) {
    ASSERT_FALSE(rect.isEmpty());

    EXPECT_FALSE(flip_lr(rect).containsInclusive(point)) << label;
    EXPECT_FALSE(flip_tb(rect).containsInclusive(point)) << label;
    EXPECT_FALSE(flip_lrtb(rect).containsInclusive(point)) << label;
  };

  auto test_inside = [&check_empty_flips](const IRect& rect,
    const IPoint& point) {
    ASSERT_FALSE(rect.isEmpty()) << rect;

    std::stringstream stream;
    stream << rect << " contains " << point;
    auto label = stream.str();

    EXPECT_TRUE(rect.containsInclusive(point)) << label;
    check_empty_flips(rect, point, label);
  };

  auto test_outside = [&check_empty_flips](const IRect& rect, const IPoint& point) {
    ASSERT_FALSE(rect.isEmpty()) << rect;

    std::stringstream stream;
    stream << rect << " contains " << point;
    auto label = stream.str();

    EXPECT_FALSE(rect.containsInclusive(point)) << label;
    check_empty_flips(rect, point, label);
  };

  {
    // Origin is inclusive
    auto r = IRect::MakeXYWH(100, 100, 100, 100);
    auto p = IPoint(100, 100);

    test_inside(r, p);
  }
  {
    // Size is inclusive
    auto r = IRect::MakeXYWH(100, 100, 100, 100);
    auto p = IPoint(200, 200);

    test_inside(r, p);
  }
  {
    // Size + "epsilon" is exclusive
    auto r = IRect::MakeXYWH(100, 100, 100, 100);
    auto p = IPoint(201, 201);

    test_outside(r, p);
  }
  {
    auto r = IRect::MakeXYWH(100, 100, 100, 100);
    auto p = IPoint(99, 99);

    test_outside(r, p);
  }
  {
    auto r = IRect::MakeXYWH(100, 100, 100, 100);
    auto p = IPoint(199, 199);

    test_inside(r, p);
  }

  {
    auto r = IRect::MakeMaximum();
    auto p = IPoint(199, 199);

    test_inside(r, p);
  }
}

TEST(RectTest, RectContainsRect) {
  auto check_nans = [](const Rect& a, const Rect& b, const std::string& label) {
    ASSERT_TRUE(a.isFinite()) << label;
    ASSERT_TRUE(b.isFinite()) << label;
    ASSERT_FALSE(a.isEmpty());

    for (int i = 1; i < 16; i++) {
      // NaN in a produces false
      EXPECT_FALSE(swap_nan(a, i).contains(b)) << label << ", index = " << i;
      // NaN in b produces false
      EXPECT_TRUE(a.contains(swap_nan(b, i))) << label << ", index = " << i;
      // NaN in both is false
      for (int j = 1; j < 16; j++) {
        EXPECT_FALSE(swap_nan(a, i).contains(swap_nan(b, j)))
            << label << ", indices = " << i << ", " << j;
      }
    }
  };

  auto check_empty_flips = [](const Rect& a, const Rect& b, const std::string& label) {
    ASSERT_FALSE(a.isEmpty());
    // test b rects are allowed to have 0 w/h, but not be backwards
    ASSERT_FALSE(b.getLeft() > b.getRight() || b.getTop() > b.getBottom());

    // unflipped a vs flipped (empty) b yields false
    EXPECT_TRUE(a.contains(flip_lr(b))) << label;
    EXPECT_TRUE(a.contains(flip_tb(b))) << label;
    EXPECT_TRUE(a.contains(flip_lrtb(b))) << label;

    // flipped (empty) a vs unflipped b yields false
    EXPECT_FALSE(flip_lr(a).contains(b)) << label;
    EXPECT_FALSE(flip_tb(a).contains(b)) << label;
    EXPECT_FALSE(flip_lrtb(a).contains(b)) << label;

    // flipped (empty) a vs flipped (empty) b yields empty
    EXPECT_FALSE(flip_lr(a).contains(flip_lr(b))) << label;
    EXPECT_FALSE(flip_tb(a).contains(flip_tb(b))) << label;
    EXPECT_FALSE(flip_lrtb(a).contains(flip_lrtb(b))) << label;
  };

  auto test_inside = [&check_nans, &check_empty_flips](const Rect& a, const Rect& b) {
    ASSERT_FALSE(a.isEmpty()) << a;
    // test b rects are allowed to have 0 w/h, but not be backwards
    ASSERT_FALSE(b.getLeft() > b.getRight() || b.getTop() > b.getBottom());

    std::stringstream stream;
    stream << a << " contains " << b;
    auto label = stream.str();

    EXPECT_TRUE(a.contains(b)) << label;
    check_empty_flips(a, b, label);
    check_nans(a, b, label);
  };

  auto test_not_inside = [&check_nans, &check_empty_flips](const Rect& a, const Rect& b) {
    ASSERT_FALSE(a.isEmpty()) << a;
    // If b was empty, it would be contained and should not be tested with
    // this function - use |test_inside| instead.
    ASSERT_FALSE(b.isEmpty()) << b;

    std::stringstream stream;
    stream << a << " contains " << b;
    auto label = stream.str();

    EXPECT_FALSE(a.contains(b)) << label;
    check_empty_flips(a, b, label);
    check_nans(a, b, label);
  };

  {
    auto a = Rect::MakeXYWH(100, 100, 100, 100);

    test_inside(a, a);
  }
  {
    auto a = Rect::MakeXYWH(100, 100, 100, 100);
    auto b = Rect::MakeXYWH(0, 0, 0, 0);

    test_inside(a, b);
  }
  {
    auto a = Rect::MakeXYWH(100, 100, 100, 100);
    auto b = Rect::MakeXYWH(150, 150, 20, 20);

    test_inside(a, b);
  }
  {
    auto a = Rect::MakeXYWH(100, 100, 100, 100);
    auto b = Rect::MakeXYWH(150, 150, 100, 100);

    test_not_inside(a, b);
  }
  {
    auto a = Rect::MakeXYWH(100, 100, 100, 100);
    auto b = Rect::MakeXYWH(50, 50, 100, 100);

    test_not_inside(a, b);
  }
  {
    auto a = Rect::MakeXYWH(100, 100, 100, 100);
    auto b = Rect::MakeXYWH(0, 0, 300, 300);

    test_not_inside(a, b);
  }
  {
    auto a = Rect::MakeMaximum();
    auto b = Rect::MakeXYWH(0, 0, 300, 300);

    test_inside(a, b);
  }
}

TEST(RectTest, IRectContainsIRect) {
  auto check_empty_flips = [](const IRect& a, const IRect& b, const std::string& label) {
    ASSERT_FALSE(a.isEmpty());
    // test b rects are allowed to have 0 w/h, but not be backwards
    ASSERT_FALSE(b.getLeft() > b.getRight() || b.getTop() > b.getBottom());

    // unflipped a vs flipped (empty) b yields true
    EXPECT_TRUE(a.contains(flip_lr(b))) << label;
    EXPECT_TRUE(a.contains(flip_tb(b))) << label;
    EXPECT_TRUE(a.contains(flip_lrtb(b))) << label;

    // flipped (empty) a vs unflipped b yields false
    EXPECT_FALSE(flip_lr(a).contains(b)) << label;
    EXPECT_FALSE(flip_tb(a).contains(b)) << label;
    EXPECT_FALSE(flip_lrtb(a).contains(b)) << label;

    // flipped (empty) a vs flipped (empty) b yields empty
    EXPECT_FALSE(flip_lr(a).contains(flip_lr(b))) << label;
    EXPECT_FALSE(flip_tb(a).contains(flip_tb(b))) << label;
    EXPECT_FALSE(flip_lrtb(a).contains(flip_lrtb(b))) << label;
  };

  auto test_inside = [&check_empty_flips](const IRect& a, const IRect& b) {
    ASSERT_FALSE(a.isEmpty()) << a;
    // test b rects are allowed to have 0 w/h, but not be backwards
    ASSERT_FALSE(b.getLeft() > b.getRight() || b.getTop() > b.getBottom());

    std::stringstream stream;
    stream << a << " contains " << b;
    auto label = stream.str();

    EXPECT_TRUE(a.contains(b)) << label;
    check_empty_flips(a, b, label);
  };

  auto test_not_inside = [&check_empty_flips](const IRect& a, const IRect& b) {
    ASSERT_FALSE(a.isEmpty()) << a;
    // If b was empty, it would be contained and should not be tested with
    // this function - use |test_inside| instead.
    ASSERT_FALSE(b.isEmpty()) << b;

    std::stringstream stream;
    stream << a << " contains " << b;
    auto label = stream.str();

    EXPECT_FALSE(a.contains(b)) << label;
    check_empty_flips(a, b, label);
  };

  {
    auto a = IRect::MakeXYWH(100, 100, 100, 100);

    test_inside(a, a);
  }
  {
    auto a = IRect::MakeXYWH(100, 100, 100, 100);
    auto b = IRect::MakeXYWH(0, 0, 0, 0);

    test_inside(a, b);
  }
  {
    auto a = IRect::MakeXYWH(100, 100, 100, 100);
    auto b = IRect::MakeXYWH(150, 150, 20, 20);

    test_inside(a, b);
  }
  {
    auto a = IRect::MakeXYWH(100, 100, 100, 100);
    auto b = IRect::MakeXYWH(150, 150, 100, 100);

    test_not_inside(a, b);
  }
  {
    auto a = IRect::MakeXYWH(100, 100, 100, 100);
    auto b = IRect::MakeXYWH(50, 50, 100, 100);

    test_not_inside(a, b);
  }
  {
    auto a = IRect::MakeXYWH(100, 100, 100, 100);
    auto b = IRect::MakeXYWH(0, 0, 300, 300);

    test_not_inside(a, b);
  }
  {
    auto a = IRect::MakeMaximum();
    auto b = IRect::MakeXYWH(0, 0, 300, 300);

    test_inside(a, b);
  }
}

TEST(RectTest, RectCutOut) {
  Rect cull_rect = Rect::MakeLTRB(20, 20, 40, 40);

  auto check_nans = [&cull_rect](const Rect& diff_rect, const std::string& label) {
    EXPECT_TRUE(cull_rect.isFinite()) << label;
    EXPECT_TRUE(diff_rect.isFinite()) << label;

    for (int i = 1; i < 16; i++) {
      // NaN in cull_rect produces empty
      EXPECT_FALSE(swap_nan(cull_rect, i).getCutout(diff_rect).has_value())
          << label << ", index " << i;
      EXPECT_EQ(swap_nan(cull_rect, i).getCutoutOrEmpty(diff_rect), Rect())
          << label << ", index " << i;

      // NaN in diff_rect is nop
      EXPECT_TRUE(cull_rect.getCutout(swap_nan(diff_rect, i)).has_value())
          << label << ", index " << i;
      EXPECT_EQ(cull_rect.getCutoutOrEmpty(swap_nan(diff_rect, i)), cull_rect)
          << label << ", index " << i;

      for (int j = 1; j < 16; j++) {
        // NaN in both is also empty
        EXPECT_FALSE(
          swap_nan(cull_rect, i).getCutout(swap_nan(diff_rect, j)).has_value())
            << label << ", indices " << i << ", " << j;
        EXPECT_EQ(swap_nan(cull_rect, i).getCutoutOrEmpty(swap_nan(diff_rect, j)),
          Rect())
            << label << ", indices " << i << ", " << j;
      }
    }
  };

  auto check_empty_flips = [&cull_rect](const Rect& diff_rect, const std::string& label) {
    EXPECT_FALSE(cull_rect.isEmpty()) << label;
    EXPECT_FALSE(diff_rect.isEmpty()) << label;

    // unflipped cull_rect vs flipped(empty) diff_rect == cull_rect
    EXPECT_TRUE(cull_rect.getCutout(flip_lr(diff_rect)).has_value()) << label;
    EXPECT_EQ(cull_rect.getCutout(flip_lr(diff_rect)), cull_rect) << label;
    EXPECT_TRUE(cull_rect.getCutout(flip_tb(diff_rect)).has_value()) << label;
    EXPECT_EQ(cull_rect.getCutout(flip_tb(diff_rect)), cull_rect) << label;
    EXPECT_TRUE(cull_rect.getCutout(flip_lrtb(diff_rect)).has_value()) << label;
    EXPECT_EQ(cull_rect.getCutout(flip_lrtb(diff_rect)), cull_rect) << label;

    // flipped(empty) cull_rect vs unflipped diff_rect == empty
    EXPECT_FALSE(flip_lr(cull_rect).getCutout(diff_rect).has_value()) << label;
    EXPECT_EQ(flip_lr(cull_rect).getCutoutOrEmpty(diff_rect), Rect()) << label;
    EXPECT_FALSE(flip_tb(cull_rect).getCutout(diff_rect).has_value()) << label;
    EXPECT_EQ(flip_tb(cull_rect).getCutoutOrEmpty(diff_rect), Rect()) << label;
    EXPECT_FALSE(flip_lrtb(cull_rect).getCutout(diff_rect).has_value()) << label;
    EXPECT_EQ(flip_lrtb(cull_rect).getCutoutOrEmpty(diff_rect), Rect()) << label;

    // flipped(empty) cull_rect vs flipped(empty) diff_rect == empty
    EXPECT_FALSE(flip_lr(cull_rect).getCutout(flip_lr(diff_rect)).has_value()) << label;
    EXPECT_EQ(flip_lr(cull_rect).getCutoutOrEmpty(flip_lr(diff_rect)), Rect()) << label;
    EXPECT_FALSE(flip_tb(cull_rect).getCutout(flip_tb(diff_rect)).has_value()) << label;
    EXPECT_EQ(flip_tb(cull_rect).getCutoutOrEmpty(flip_tb(diff_rect)), Rect()) << label;
    EXPECT_FALSE(flip_lrtb(cull_rect).getCutout(flip_lrtb(diff_rect)).has_value()) << label;
    EXPECT_EQ(flip_lrtb(cull_rect).getCutoutOrEmpty(flip_lrtb(diff_rect)), Rect()) << label;
  };

  auto non_reducing = [&cull_rect, &check_empty_flips, &check_nans](
    const Rect& diff_rect, const std::string& label) {
    EXPECT_EQ(cull_rect.getCutout(diff_rect), cull_rect) << label;
    EXPECT_EQ(cull_rect.getCutoutOrEmpty(diff_rect), cull_rect) << label;
    check_empty_flips(diff_rect, label);
    check_nans(diff_rect, label);
  };

  auto reducing = [&cull_rect, &check_empty_flips, &check_nans](
    const Rect& diff_rect, const Rect& result_rect,
    const std::string& label) {
    EXPECT_TRUE(!result_rect.isEmpty());
    EXPECT_EQ(cull_rect.getCutout(diff_rect), result_rect) << label;
    EXPECT_EQ(cull_rect.getCutoutOrEmpty(diff_rect), result_rect) << label;
    check_empty_flips(diff_rect, label);
    check_nans(diff_rect, label);
  };

  auto emptying = [&cull_rect, &check_empty_flips, &check_nans](
    const Rect& diff_rect, const std::string& label) {
    EXPECT_FALSE(cull_rect.getCutout(diff_rect).has_value()) << label;
    EXPECT_EQ(cull_rect.getCutoutOrEmpty(diff_rect), Rect()) << label;
    check_empty_flips(diff_rect, label);
    check_nans(diff_rect, label);
  };

  // Skim the corners and edge
  non_reducing(Rect::MakeLTRB(10, 10, 20, 20), "outside UL corner");
  non_reducing(Rect::MakeLTRB(20, 10, 40, 20), "Above");
  non_reducing(Rect::MakeLTRB(40, 10, 50, 20), "outside UR corner");
  non_reducing(Rect::MakeLTRB(40, 20, 50, 40), "Right");
  non_reducing(Rect::MakeLTRB(40, 40, 50, 50), "outside LR corner");
  non_reducing(Rect::MakeLTRB(20, 40, 40, 50), "Below");
  non_reducing(Rect::MakeLTRB(10, 40, 20, 50), "outside LR corner");
  non_reducing(Rect::MakeLTRB(10, 20, 20, 40), "Left");

  // Overlap corners
  non_reducing(Rect::MakeLTRB(15, 15, 25, 25), "covering UL corner");
  non_reducing(Rect::MakeLTRB(35, 15, 45, 25), "covering UR corner");
  non_reducing(Rect::MakeLTRB(35, 35, 45, 45), "covering LR corner");
  non_reducing(Rect::MakeLTRB(15, 35, 25, 45), "covering LL corner");

  // Overlap edges, but not across an entire side
  non_reducing(Rect::MakeLTRB(20, 15, 39, 25), "Top edge left-biased");
  non_reducing(Rect::MakeLTRB(21, 15, 40, 25), "Top edge, right biased");
  non_reducing(Rect::MakeLTRB(35, 20, 45, 39), "Right edge, top-biased");
  non_reducing(Rect::MakeLTRB(35, 21, 45, 40), "Right edge, bottom-biased");
  non_reducing(Rect::MakeLTRB(20, 35, 39, 45), "Bottom edge, left-biased");
  non_reducing(Rect::MakeLTRB(21, 35, 40, 45), "Bottom edge, right-biased");
  non_reducing(Rect::MakeLTRB(15, 20, 25, 39), "Left edge, top-biased");
  non_reducing(Rect::MakeLTRB(15, 21, 25, 40), "Left edge, bottom-biased");

  // Slice all the way through the middle
  non_reducing(Rect::MakeLTRB(25, 15, 35, 45), "Vertical interior slice");
  non_reducing(Rect::MakeLTRB(15, 25, 45, 35), "Horizontal interior slice");

  // Slice off each edge
  reducing(Rect::MakeLTRB(20, 15, 40, 25),
    Rect::MakeLTRB(20, 25, 40, 40),
    "Slice off top");
  reducing(Rect::MakeLTRB(35, 20, 45, 40),
    Rect::MakeLTRB(20, 20, 35, 40),
    "Slice off right");
  reducing(Rect::MakeLTRB(20, 35, 40, 45),
    Rect::MakeLTRB(20, 20, 40, 35),
    "Slice off bottom");
  reducing(Rect::MakeLTRB(15, 20, 25, 40),
    Rect::MakeLTRB(25, 20, 40, 40),
    "Slice off left");

  // cull rect contains diff rect
  non_reducing(Rect::MakeLTRB(21, 21, 39, 39), "Contained, non-covering");

  // cull rect equals diff rect
  emptying(cull_rect, "Perfectly covering");

  // diff rect contains cull rect
  emptying(Rect::MakeLTRB(15, 15, 45, 45), "Smothering");
}

TEST(RectTest, IRectCutOut) {
  IRect cull_rect = IRect::MakeLTRB(20, 20, 40, 40);

  auto check_empty_flips = [&cull_rect](const IRect& diff_rect, const std::string& label) {
    EXPECT_FALSE(diff_rect.isEmpty());
    EXPECT_FALSE(cull_rect.isEmpty());

    // unflipped cull_rect vs flipped(empty) diff_rect == cull_rect
    EXPECT_TRUE(cull_rect.getCutout(flip_lr(diff_rect)).has_value()) << label;
    EXPECT_EQ(cull_rect.getCutout(flip_lr(diff_rect)), cull_rect) << label;
    EXPECT_TRUE(cull_rect.getCutout(flip_tb(diff_rect)).has_value()) << label;
    EXPECT_EQ(cull_rect.getCutout(flip_tb(diff_rect)), cull_rect) << label;
    EXPECT_TRUE(cull_rect.getCutout(flip_lrtb(diff_rect)).has_value()) << label;
    EXPECT_EQ(cull_rect.getCutout(flip_lrtb(diff_rect)), cull_rect) << label;

    // flipped(empty) cull_rect vs flipped(empty) diff_rect == empty
    EXPECT_FALSE(flip_lr(cull_rect).getCutout(diff_rect).has_value()) << label;
    EXPECT_EQ(flip_lr(cull_rect).getCutoutOrEmpty(diff_rect), IRect()) << label;
    EXPECT_FALSE(flip_tb(cull_rect).getCutout(diff_rect).has_value()) << label;
    EXPECT_EQ(flip_tb(cull_rect).getCutoutOrEmpty(diff_rect), IRect()) << label;
    EXPECT_FALSE(flip_lrtb(cull_rect).getCutout(diff_rect).has_value()) << label;
    EXPECT_EQ(flip_lrtb(cull_rect).getCutoutOrEmpty(diff_rect), IRect()) << label;

    // flipped(empty) cull_rect vs unflipped diff_rect == empty
    EXPECT_FALSE(flip_lr(cull_rect).getCutout(flip_lr(diff_rect)).has_value()) << label;
    EXPECT_EQ(flip_lr(cull_rect).getCutoutOrEmpty(flip_lr(diff_rect)), IRect()) << label;
    EXPECT_FALSE(flip_tb(cull_rect).getCutout(flip_tb(diff_rect)).has_value()) << label;
    EXPECT_EQ(flip_tb(cull_rect).getCutoutOrEmpty(flip_tb(diff_rect)), IRect()) << label;
    EXPECT_FALSE(flip_lrtb(cull_rect).getCutout(flip_lrtb(diff_rect)).has_value()) << label;
    EXPECT_EQ(flip_lrtb(cull_rect).getCutoutOrEmpty(flip_lrtb(diff_rect)), IRect()) << label;
  };

  auto non_reducing = [&cull_rect, &check_empty_flips](const IRect& diff_rect, const std::string& label) {
    EXPECT_EQ(cull_rect.getCutout(diff_rect), cull_rect) << label;
    EXPECT_EQ(cull_rect.getCutoutOrEmpty(diff_rect), cull_rect) << label;
    check_empty_flips(diff_rect, label);
  };

  auto reducing = [&cull_rect, &check_empty_flips](const IRect& diff_rect,
    const IRect& result_rect,
    const std::string& label) {
    EXPECT_TRUE(!result_rect.isEmpty());
    EXPECT_EQ(cull_rect.getCutout(diff_rect), result_rect) << label;
    EXPECT_EQ(cull_rect.getCutoutOrEmpty(diff_rect), result_rect) << label;
    check_empty_flips(diff_rect, label);
  };

  auto emptying = [&cull_rect, &check_empty_flips](const IRect& diff_rect, const std::string& label) {
    EXPECT_FALSE(cull_rect.getCutout(diff_rect).has_value()) << label;
    EXPECT_EQ(cull_rect.getCutoutOrEmpty(diff_rect), IRect()) << label;
    check_empty_flips(diff_rect, label);
  };

  // Skim the corners and edge
  non_reducing(IRect::MakeLTRB(10, 10, 20, 20), "outside UL corner");
  non_reducing(IRect::MakeLTRB(20, 10, 40, 20), "Above");
  non_reducing(IRect::MakeLTRB(40, 10, 50, 20), "outside UR corner");
  non_reducing(IRect::MakeLTRB(40, 20, 50, 40), "Right");
  non_reducing(IRect::MakeLTRB(40, 40, 50, 50), "outside LR corner");
  non_reducing(IRect::MakeLTRB(20, 40, 40, 50), "Below");
  non_reducing(IRect::MakeLTRB(10, 40, 20, 50), "outside LR corner");
  non_reducing(IRect::MakeLTRB(10, 20, 20, 40), "Left");

  // Overlap corners
  non_reducing(IRect::MakeLTRB(15, 15, 25, 25), "covering UL corner");
  non_reducing(IRect::MakeLTRB(35, 15, 45, 25), "covering UR corner");
  non_reducing(IRect::MakeLTRB(35, 35, 45, 45), "covering LR corner");
  non_reducing(IRect::MakeLTRB(15, 35, 25, 45), "covering LL corner");

  // Overlap edges, but not across an entire side
  non_reducing(IRect::MakeLTRB(20, 15, 39, 25), "Top edge left-biased");
  non_reducing(IRect::MakeLTRB(21, 15, 40, 25), "Top edge, right biased");
  non_reducing(IRect::MakeLTRB(35, 20, 45, 39), "Right edge, top-biased");
  non_reducing(IRect::MakeLTRB(35, 21, 45, 40), "Right edge, bottom-biased");
  non_reducing(IRect::MakeLTRB(20, 35, 39, 45), "Bottom edge, left-biased");
  non_reducing(IRect::MakeLTRB(21, 35, 40, 45), "Bottom edge, right-biased");
  non_reducing(IRect::MakeLTRB(15, 20, 25, 39), "Left edge, top-biased");
  non_reducing(IRect::MakeLTRB(15, 21, 25, 40), "Left edge, bottom-biased");

  // Slice all the way through the middle
  non_reducing(IRect::MakeLTRB(25, 15, 35, 45), "Vertical interior slice");
  non_reducing(IRect::MakeLTRB(15, 25, 45, 35), "Horizontal interior slice");

  // Slice off each edge
  reducing(IRect::MakeLTRB(20, 15, 40, 25),
    IRect::MakeLTRB(20, 25, 40, 40),
    "Slice off top");
  reducing(IRect::MakeLTRB(35, 20, 45, 40),
    IRect::MakeLTRB(20, 20, 35, 40),
    "Slice off right");
  reducing(IRect::MakeLTRB(20, 35, 40, 45),
    IRect::MakeLTRB(20, 20, 40, 35),
    "Slice off bottom");
  reducing(IRect::MakeLTRB(15, 20, 25, 40),
    IRect::MakeLTRB(25, 20, 40, 40),
    "Slice off left");

  // cull rect contains diff rect
  non_reducing(IRect::MakeLTRB(21, 21, 39, 39), "Contained, non-covering");

  // cull rect equals diff rect
  emptying(cull_rect, "Perfectly covering");

  // diff rect contains cull rect
  emptying(IRect::MakeLTRB(15, 15, 45, 45), "Smothering");
}

TEST(RectTest, RectGetPoints) {
  {
    Rect r = Rect::MakeXYWH(100, 200, 300, 400);
    auto points = r.getPoints();
    EXPECT_POINT_NEAR(points[0], Point(100, 200));
    EXPECT_POINT_NEAR(points[1], Point(400, 200));
    EXPECT_POINT_NEAR(points[2], Point(100, 600));
    EXPECT_POINT_NEAR(points[3], Point(400, 600));
  }

  {
    Rect r = Rect::MakeMaximum();
    auto points = r.getPoints();
    EXPECT_EQ(points[0], Point(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()));
    EXPECT_EQ(points[1], Point(std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest()));
    EXPECT_EQ(points[2], Point(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max()));
    EXPECT_EQ(points[3], Point(std::numeric_limits<float>::max(), std::numeric_limits<float>::max()));
  }
}

TEST(RectTest, RectShift) {
  auto r = Rect::MakeLTRB(0, 0, 100, 100);

  EXPECT_EQ(r.shift(Point(10, 5)), Rect::MakeLTRB(10, 5, 110, 105));
  EXPECT_EQ(r.shift(Point(-10, -5)), Rect::MakeLTRB(-10, -5, 90, 95));
}

TEST(RectTest, RectGetTransformedPoints) {
  Rect r = Rect::MakeXYWH(100, 200, 300, 400);
  auto points = r.getTransformedPoints(Matrix::MakeTranslation({ 10, 20 }));
  EXPECT_POINT_NEAR(points[0], Point(110, 220));
  EXPECT_POINT_NEAR(points[1], Point(410, 220));
  EXPECT_POINT_NEAR(points[2], Point(110, 620));
  EXPECT_POINT_NEAR(points[3], Point(410, 620));
}

TEST(RectTest, RectMakePointBounds) {
  {
    std::vector<Point> points{ { 1, 5 }, { 4, -1 }, { 0, 6 } };
    auto r = Rect::MakePointBounds(points.begin(), points.end());
    auto expected = Rect::MakeXYWH(0, -1, 4, 7);
    EXPECT_TRUE(r.has_value());
    if (r.has_value()) {
      EXPECT_RECT_NEAR(r.value(), expected);
    }
  }
  {
    std::vector<Point> points;
    std::optional<Rect> r = Rect::MakePointBounds(points.begin(), points.end());
    EXPECT_FALSE(r.has_value());
  }
}

TEST(RectTest, RectGetPositive) {
  {
    Rect r = Rect::MakeXYWH(100, 200, 300, 400);
    auto actual = r.getPositive();
    EXPECT_RECT_NEAR(r, actual);
  }
  {
    Rect r = Rect::MakeXYWH(100, 200, -100, -100);
    auto actual = r.getPositive();
    Rect expected = Rect::MakeXYWH(0, 100, 100, 100);
    EXPECT_RECT_NEAR(expected, actual);
  }
}

TEST(RectTest, RectDirections) {
  auto r = Rect::MakeLTRB(1, 2, 3, 4);

  EXPECT_EQ(r.getLeft(), 1);
  EXPECT_EQ(r.getTop(), 2);
  EXPECT_EQ(r.getRight(), 3);
  EXPECT_EQ(r.getBottom(), 4);

  EXPECT_POINT_NEAR(r.getLeftTop(), Point(1, 2));
  EXPECT_POINT_NEAR(r.getRightTop(), Point(3, 2));
  EXPECT_POINT_NEAR(r.getLeftBottom(), Point(1, 4));
  EXPECT_POINT_NEAR(r.getRightBottom(), Point(3, 4));
}

TEST(RectTest, RectProject) {
  {
    auto r = Rect::MakeLTRB(-100, -100, 100, 100);
    auto actual = r.getProjection(r);
    auto expected = Rect::MakeLTRB(0, 0, 1, 1);
    EXPECT_RECT_NEAR(expected, actual);
  }
  {
    auto r = Rect::MakeLTRB(-100, -100, 100, 100);
    auto actual = r.getProjection(Rect::MakeLTRB(0, 0, 100, 100));
    auto expected = Rect::MakeLTRB(0.5, 0.5, 1, 1);
    EXPECT_RECT_NEAR(expected, actual);
  }
}

TEST(RectTest, RectRoundOut) {
  {
    auto r = Rect::MakeLTRB(-100, -200, 300, 400);
    EXPECT_EQ(Rect::RoundOut(r), r);
  }
  {
    auto r = Rect::MakeLTRB(-100.1, -200.1, 300.1, 400.1);
    EXPECT_EQ(Rect::RoundOut(r), Rect::MakeLTRB(-101, -201, 301, 401));
  }
}

TEST(RectTest, IRectRoundOut) {
  {
    auto r = Rect::MakeLTRB(-100, -200, 300, 400);
    auto ir = IRect::MakeLTRB(-100, -200, 300, 400);
    EXPECT_EQ(IRect::RoundOut(r), ir);
  }
  {
    auto r = Rect::MakeLTRB(-100.1, -200.1, 300.1, 400.1);
    auto ir = IRect::MakeLTRB(-101, -201, 301, 401);
    EXPECT_EQ(IRect::RoundOut(r), ir);
  }
}

TEST(RectTest, RectRound) {
  {
    auto r = Rect::MakeLTRB(-100, -200, 300, 400);
    EXPECT_EQ(Rect::Round(r), r);
  }
  {
    auto r = Rect::MakeLTRB(-100.4, -200.4, 300.4, 400.4);
    EXPECT_EQ(Rect::Round(r), Rect::MakeLTRB(-100, -200, 300, 400));
  }
  {
    auto r = Rect::MakeLTRB(-100.5, -200.5, 300.5, 400.5);
    EXPECT_EQ(Rect::Round(r), Rect::MakeLTRB(-101, -201, 301, 401));
  }
}

TEST(RectTest, IRectRound) {
  {
    auto r = Rect::MakeLTRB(-100, -200, 300, 400);
    auto ir = IRect::MakeLTRB(-100, -200, 300, 400);
    EXPECT_EQ(IRect::Round(r), ir);
  }
  {
    auto r = Rect::MakeLTRB(-100.4, -200.4, 300.4, 400.4);
    auto ir = IRect::MakeLTRB(-100, -200, 300, 400);
    EXPECT_EQ(IRect::Round(r), ir);
  }
  {
    auto r = Rect::MakeLTRB(-100.5, -200.5, 300.5, 400.5);
    auto ir = IRect::MakeLTRB(-101, -201, 301, 401);
    EXPECT_EQ(IRect::Round(r), ir);
  }
}

TEST(RectTest, TransformAndClipBounds) {
  {
    // This matrix should clip no corners.
    auto matrix = Matrix::MakeColumn(
        // clang-format off
        2.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 4.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 8.0f
      // clang-format on
      );
    Rect src = Rect::MakeLTRB(100.0f, 100.0f, 200.0f, 200.0f);
    // None of these should have a W<0
    EXPECT_EQ(matrix.transformHomogenous(src.getLeftTop()),
      Vector3(200.0f, 400.0f, 8.0f));
    EXPECT_EQ(matrix.transformHomogenous(src.getRightTop()),
      Vector3(400.0f, 400.0f, 8.0f));
    EXPECT_EQ(matrix.transformHomogenous(src.getLeftBottom()),
      Vector3(200.0f, 800.0f, 8.0f));
    EXPECT_EQ(matrix.transformHomogenous(src.getRightBottom()),
      Vector3(400.0f, 800.0f, 8.0f));

    Rect expect = Rect::MakeLTRB(25.0f, 50.0f, 50.0f, 100.0f);
    EXPECT_FALSE(src.transformAndClipBounds(matrix).isEmpty());
    EXPECT_EQ(src.transformAndClipBounds(matrix), expect);
  }

  {
    // This matrix should clip one corner.
    auto matrix = Matrix::MakeColumn(
        // clang-format off
        2.0f, 0.0f, 0.0f, -0.01f,
        0.0f, 2.0f, 0.0f, -0.006f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 3.0f
      // clang-format on
      );
    Rect src = Rect::MakeLTRB(100.0f, 100.0f, 200.0f, 200.0f);
    // Exactly one of these should have a W<0
    EXPECT_VECTOR3_NEAR(matrix.transformHomogenous(src.getLeftTop()),
      Vector3(200.0f, 200.0f, 1.4f));
    EXPECT_VECTOR3_NEAR(matrix.transformHomogenous(src.getRightTop()),
      Vector3(400.0f, 200.0f, 0.4f));
    EXPECT_VECTOR3_NEAR(matrix.transformHomogenous(src.getLeftBottom()),
      Vector3(200.0f, 400.0f, 0.8f));
    EXPECT_VECTOR3_NEAR(matrix.transformHomogenous(src.getRightBottom()),
      Vector3(400.0f, 400.0f, -0.2f));

    Rect expect = Rect::MakeLTRB(142.85715f, 142.85715f, 6553600.f, 6553600.f);
    EXPECT_FALSE(src.transformAndClipBounds(matrix).isEmpty());
    EXPECT_RECT_NEAR(src.transformAndClipBounds(matrix), expect);
  }

  {
    // This matrix should clip two corners.
    auto matrix = Matrix::MakeColumn(
        // clang-format off
        2.0f, 0.0f, 0.0f, -.015f,
        0.0f, 2.0f, 0.0f, -.006f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 3.0f
      // clang-format on
      );
    Rect src = Rect::MakeLTRB(100.0f, 100.0f, 200.0f, 200.0f);
    // Exactly two of these should have a W<0
    EXPECT_VECTOR3_NEAR(matrix.transformHomogenous(src.getLeftTop()),
      Vector3(200.0f, 200.0f, 0.9f));
    EXPECT_VECTOR3_NEAR(matrix.transformHomogenous(src.getRightTop()),
      Vector3(400.0f, 200.0f, -0.6f));
    EXPECT_VECTOR3_NEAR(matrix.transformHomogenous(src.getLeftBottom()),
      Vector3(200.0f, 400.0f, 0.3f));
    EXPECT_VECTOR3_NEAR(matrix.transformHomogenous(src.getRightBottom()),
      Vector3(400.0f, 400.0f, -1.2f));

    Rect expect = Rect::MakeLTRB(222.2222f, 222.2222f, 5242747.f, 6553600.f);
    EXPECT_FALSE(src.transformAndClipBounds(matrix).isEmpty());
    EXPECT_RECT_NEAR(src.transformAndClipBounds(matrix), expect);
  }

  {
    // This matrix should clip three corners.
    auto matrix = Matrix::MakeColumn(
        // clang-format off
        2.0f, 0.0f, 0.0f, -.02f,
        0.0f, 2.0f, 0.0f, -.006f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 3.0f
      // clang-format on
      );
    Rect src = Rect::MakeLTRB(100.0f, 100.0f, 200.0f, 200.0f);
    // Exactly three of these should have a W<0
    EXPECT_VECTOR3_NEAR(matrix.transformHomogenous(src.getLeftTop()),
      Vector3(200.0f, 200.0f, 0.4f));
    EXPECT_VECTOR3_NEAR(matrix.transformHomogenous(src.getRightTop()),
      Vector3(400.0f, 200.0f, -1.6f));
    EXPECT_VECTOR3_NEAR(matrix.transformHomogenous(src.getLeftBottom()),
      Vector3(200.0f, 400.0f, -0.2f));
    EXPECT_VECTOR3_NEAR(matrix.transformHomogenous(src.getRightBottom()),
      Vector3(400.0f, 400.0f, -2.2f));

    Rect expect = Rect::MakeLTRB(500.f, 500.f, 3932060.f, 5461000.f);
    EXPECT_FALSE(src.transformAndClipBounds(matrix).isEmpty());
    EXPECT_RECT_NEAR(src.transformAndClipBounds(matrix), expect);
  }

  {
    // This matrix should clip all four corners.
    auto matrix = Matrix::MakeColumn(
        // clang-format off
        2.0f, 0.0f, 0.0f, -.025f,
        0.0f, 2.0f, 0.0f, -.006f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 3.0f
      // clang-format on
      );
    Rect src = Rect::MakeLTRB(100.0f, 100.0f, 200.0f, 200.0f);
    // All of these should have a W<0
    EXPECT_VECTOR3_NEAR(matrix.transformHomogenous(src.getLeftTop()),
      Vector3(200.0f, 200.0f, -0.1f));
    EXPECT_VECTOR3_NEAR(matrix.transformHomogenous(src.getRightTop()),
      Vector3(400.0f, 200.0f, -2.6f));
    EXPECT_VECTOR3_NEAR(matrix.transformHomogenous(src.getLeftBottom()),
      Vector3(200.0f, 400.0f, -0.7f));
    EXPECT_VECTOR3_NEAR(matrix.transformHomogenous(src.getRightBottom()),
      Vector3(400.0f, 400.0f, -3.2f));

    EXPECT_TRUE(src.transformAndClipBounds(matrix).isEmpty());
  }
}
