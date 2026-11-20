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

#include <geometry/Rational.h>

using namespace graphics;

class RationalTest : public testing::Test {
protected:
};

TEST(RationalTest, Make) {
  Rational value(1, 2);
  EXPECT_EQ(value.getNumerator(), 1);
  EXPECT_EQ(value.getDenominator(), 2u);
}

TEST(RationalTest, EqualsSameDen) {
  EXPECT_TRUE(Rational(1, 2) == Rational(1, 2));
}

TEST(RationalTest, NotEqualsSameDen) {
  EXPECT_FALSE(Rational(3, 2) == Rational(1, 2));
}

TEST(RationalTest, EqualsDifferentDen) {
  EXPECT_TRUE(Rational(1, 2) == Rational(2, 4));
}

TEST(RationalTest, NegationNotEquals) {
  EXPECT_FALSE(Rational(1, 2) == Rational(-1, 2));
}

TEST(RationalTest, LessThanSameDen) {
  EXPECT_TRUE(Rational(1, 2) < Rational(2, 2));
}

TEST(RationalTest, LessThanNegation) {
  EXPECT_TRUE(Rational(-1, 2) < Rational(2, 23));
}

TEST(RationalTest, LessThanDifferentDen) {
  EXPECT_TRUE(Rational(1, 2) < Rational(25, 23));
}

TEST(RationalTest, NotLessThanDifferentDen) {
  EXPECT_FALSE(Rational(25, 23) < Rational(1, 2));
}

TEST(RationalTest, SameHashes) {
  EXPECT_EQ(Rational(1, 2).getHash(), Rational(2, 4).getHash());
}

TEST(RationalTest, DifferentHashes) {
  EXPECT_NE(Rational(2, 2).getHash(), Rational(2, 4).getHash());
}
