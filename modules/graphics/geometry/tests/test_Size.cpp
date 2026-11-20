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

#include <geometry/Size.h>

using namespace graphics;

class SizeTest : public testing::Test {
protected:
};

TEST(SizeTest, SizeIsEmpty) {
  const auto nan = std::numeric_limits<Scalar>::quiet_NaN();

  // Non-empty
  EXPECT_FALSE(Size(10.5, 7.2).isEmpty());

  // Empty both width and height both 0 or negative, in all combinations
  EXPECT_TRUE(Size(0.0, 0.0).isEmpty());
  EXPECT_TRUE(Size(-1.0, -1.0).isEmpty());
  EXPECT_TRUE(Size(-1.0, 0.0).isEmpty());
  EXPECT_TRUE(Size(0.0, -1.0).isEmpty());

  // Empty for 0 or negative width or height (but not both at the same time)
  EXPECT_TRUE(Size(10.5, 0.0).isEmpty());
  EXPECT_TRUE(Size(10.5, -1.0).isEmpty());
  EXPECT_TRUE(Size(0.0, 7.2).isEmpty());
  EXPECT_TRUE(Size(-1.0, 7.2).isEmpty());

  // Empty for NaN in width or height or both
  EXPECT_TRUE(Size(10.5, nan).isEmpty());
  EXPECT_TRUE(Size(nan, 7.2).isEmpty());
  EXPECT_TRUE(Size(nan, nan).isEmpty());
}

TEST(SizeTest, ISizeIsEmpty) {
  // Non-empty
  EXPECT_FALSE(ISize(10, 7).isEmpty());

  // Empty both width and height both 0 or negative, in all combinations
  EXPECT_TRUE(ISize(0, 0).isEmpty());
  EXPECT_TRUE(ISize(-1, -1).isEmpty());
  EXPECT_TRUE(ISize(-1, 0).isEmpty());
  EXPECT_TRUE(ISize(0, -1).isEmpty());

  // Empty for 0 or negative width or height (but not both at the same time)
  EXPECT_TRUE(ISize(10, 0).isEmpty());
  EXPECT_TRUE(ISize(10, -1).isEmpty());
  EXPECT_TRUE(ISize(0, 7).isEmpty());
  EXPECT_TRUE(ISize(-1, 7).isEmpty());
}

TEST(SizeTest, IsSquare) {
  EXPECT_TRUE(Size(20, 20).isSquare());
  EXPECT_FALSE(Size(20, 19).isSquare());
  EXPECT_FALSE(Size(19, 20).isSquare());

  EXPECT_TRUE(ISize(20, 20).isSquare());
  EXPECT_FALSE(ISize(20, 19).isSquare());
  EXPECT_FALSE(ISize(19, 20).isSquare());
}

TEST(SizeTest, MaxDimension) {
  EXPECT_EQ(Size(20, 20).maxDimension(), 20);
  EXPECT_EQ(Size(20, 19).maxDimension(), 20);
  EXPECT_EQ(Size(19, 20).maxDimension(), 20);
  EXPECT_EQ(Size(20, 21).maxDimension(), 21);
  EXPECT_EQ(Size(21, 20).maxDimension(), 21);

  EXPECT_EQ(ISize(20, 20).maxDimension(), 20);
  EXPECT_EQ(ISize(20, 19).maxDimension(), 20);
  EXPECT_EQ(ISize(19, 20).maxDimension(), 20);
  EXPECT_EQ(ISize(20, 21).maxDimension(), 21);
  EXPECT_EQ(ISize(21, 20).maxDimension(), 21);
}

TEST(SizeTest, NegationOperator) {
  EXPECT_EQ(-Size(10, 20), Size(-10, -20));
  EXPECT_EQ(-Size(-10, 20), Size(10, -20));
  EXPECT_EQ(-Size(10, -20), Size(-10, 20));
  EXPECT_EQ(-Size(-10, -20), Size(10, 20));
}
