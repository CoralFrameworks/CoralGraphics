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

#include <geometry/RoundRect.h>

using namespace graphics;

class RoundRectTest : public testing::Test {
protected:
};

TEST(RoundRectTest, EmptyDeclaration) {
  RoundRect rect;

  EXPECT_TRUE(rect.isEmpty());
  EXPECT_FALSE(rect.isRect());
  EXPECT_FALSE(rect.isOval());
  EXPECT_TRUE(rect.isFinite());
  EXPECT_TRUE(rect.getBounds().isEmpty());

  EXPECT_EQ(rect.getBounds(), Rect());
  EXPECT_EQ(rect.getBounds().getLeft(), 0.0f);
  EXPECT_EQ(rect.getBounds().getTop(), 0.0f);
  EXPECT_EQ(rect.getBounds().getRight(), 0.0f);
  EXPECT_EQ(rect.getBounds().getBottom(), 0.0f);
  EXPECT_EQ(rect.getRadii().top_left, Size());
  EXPECT_EQ(rect.getRadii().top_right, Size());
  EXPECT_EQ(rect.getRadii().bottom_left, Size());
  EXPECT_EQ(rect.getRadii().bottom_right, Size());
  EXPECT_EQ(rect.getRadii().top_left.width, 0.0f);
  EXPECT_EQ(rect.getRadii().top_left.height, 0.0f);
  EXPECT_EQ(rect.getRadii().top_right.width, 0.0f);
  EXPECT_EQ(rect.getRadii().top_right.height, 0.0f);
  EXPECT_EQ(rect.getRadii().bottom_left.width, 0.0f);
  EXPECT_EQ(rect.getRadii().bottom_left.height, 0.0f);
  EXPECT_EQ(rect.getRadii().bottom_right.width, 0.0f);
  EXPECT_EQ(rect.getRadii().bottom_right.height, 0.0f);
}

TEST(RoundRectTest, DefaultConstructor) {
  constexpr auto rect = RoundRect();

  EXPECT_TRUE(rect.isEmpty());
  EXPECT_FALSE(rect.isRect());
  EXPECT_FALSE(rect.isOval());
  EXPECT_TRUE(rect.isFinite());
  EXPECT_TRUE(rect.getBounds().isEmpty());
  EXPECT_EQ(rect.getBounds(), Rect());
  EXPECT_EQ(rect.getRadii().top_left, Size());
  EXPECT_EQ(rect.getRadii().top_right, Size());
  EXPECT_EQ(rect.getRadii().bottom_left, Size());
  EXPECT_EQ(rect.getRadii().bottom_right, Size());
}

TEST(RoundRectTest, EmptyRectConstruction) {
  const auto rect = RoundRect::MakeRect(
    Rect::MakeLTRB(20.0f, 20.0f, 20.0f, 20.0f));

  EXPECT_TRUE(rect.isEmpty());
  EXPECT_FALSE(rect.isRect());
  EXPECT_FALSE(rect.isOval());
  EXPECT_TRUE(rect.isFinite());
  EXPECT_TRUE(rect.getBounds().isEmpty());
  EXPECT_EQ(rect.getBounds(), Rect::MakeLTRB(20.0f, 20.0f, 20.0f, 20.0f));
  EXPECT_EQ(rect.getRadii().top_left, Size());
  EXPECT_EQ(rect.getRadii().top_right, Size());
  EXPECT_EQ(rect.getRadii().bottom_left, Size());
  EXPECT_EQ(rect.getRadii().bottom_right, Size());
}

TEST(RoundRectTest, RectConstructor) {
  auto rect = RoundRect::MakeRect(
    Rect::MakeLTRB(10.0f, 10.0f, 20.0f, 20.0f));

  EXPECT_FALSE(rect.isEmpty());
  EXPECT_TRUE(rect.isRect());
  EXPECT_FALSE(rect.isOval());
  EXPECT_TRUE(rect.isFinite());
  EXPECT_FALSE(rect.getBounds().isEmpty());
  EXPECT_EQ(rect.getBounds(), Rect::MakeLTRB(10.0f, 10.0f, 20.0f, 20.0f));
  EXPECT_EQ(rect.getRadii().top_left, Size());
  EXPECT_EQ(rect.getRadii().top_right, Size());
  EXPECT_EQ(rect.getRadii().bottom_left, Size());
  EXPECT_EQ(rect.getRadii().bottom_right, Size());
}

TEST(RoundRectTest, InvertedRectConstruction) {
  auto rect = RoundRect::MakeRect(
    Rect::MakeLTRB(20.0f, 20.0f, 10.0f, 10.0f));

  EXPECT_FALSE(rect.isEmpty());
  EXPECT_TRUE(rect.isRect());
  EXPECT_FALSE(rect.isOval());
  EXPECT_TRUE(rect.isFinite());
  EXPECT_FALSE(rect.getBounds().isEmpty());
  EXPECT_EQ(rect.getBounds(), Rect::MakeLTRB(10.0f, 10.0f, 20.0f, 20.0f));
  EXPECT_EQ(rect.getRadii().top_left, Size());
  EXPECT_EQ(rect.getRadii().top_right, Size());
  EXPECT_EQ(rect.getRadii().bottom_left, Size());
  EXPECT_EQ(rect.getRadii().bottom_right, Size());
}

TEST(RoundRectTest, EmptyOvalConstruction) {
  auto rect = RoundRect::MakeRectXY(
    Rect::MakeLTRB(20.0f, 20.0f, 20.0f, 20.0f), 10.0f, 10.0f);

  EXPECT_TRUE(rect.isEmpty());
  EXPECT_FALSE(rect.isRect());
  EXPECT_FALSE(rect.isOval());
  EXPECT_TRUE(rect.isFinite());
  EXPECT_TRUE(rect.getBounds().isEmpty());
  EXPECT_EQ(rect.getBounds(), Rect::MakeLTRB(20.0f, 20.0f, 20.0f, 20.0f));
  EXPECT_EQ(rect.getRadii().top_left, Size());
  EXPECT_EQ(rect.getRadii().top_right, Size());
  EXPECT_EQ(rect.getRadii().bottom_left, Size());
  EXPECT_EQ(rect.getRadii().bottom_right, Size());
}

TEST(RoundRectTest, OvalConstructor) {
  auto rect = RoundRect::MakeOval(
    Rect::MakeLTRB(10.0f, 10.0f, 20.0f, 20.0f));

  EXPECT_FALSE(rect.isEmpty());
  EXPECT_FALSE(rect.isRect());
  EXPECT_TRUE(rect.isOval());
  EXPECT_TRUE(rect.isFinite());
  EXPECT_FALSE(rect.getBounds().isEmpty());
  EXPECT_EQ(rect.getBounds(), Rect::MakeLTRB(10.0f, 10.0f, 20.0f, 20.0f));
  EXPECT_EQ(rect.getRadii().top_left, Size(5.0f, 5.0f));
  EXPECT_EQ(rect.getRadii().top_right, Size(5.0f, 5.0f));
  EXPECT_EQ(rect.getRadii().bottom_left, Size(5.0f, 5.0f));
  EXPECT_EQ(rect.getRadii().bottom_right, Size(5.0f, 5.0f));
}

TEST(RoundRectTest, InvertedOvalConstruction) {
  auto rect = RoundRect::MakeRectXY(
    Rect::MakeLTRB(20.0f, 20.0f, 10.0f, 10.0f), 10.0f, 10.0f);

  EXPECT_FALSE(rect.isEmpty());
  EXPECT_FALSE(rect.isRect());
  EXPECT_TRUE(rect.isOval());
  EXPECT_TRUE(rect.isFinite());
  EXPECT_FALSE(rect.getBounds().isEmpty());
  EXPECT_EQ(rect.getBounds(), Rect::MakeLTRB(10.0f, 10.0f, 20.0f, 20.0f));
  EXPECT_EQ(rect.getRadii().top_left, Size(5.0f, 5.0f));
  EXPECT_EQ(rect.getRadii().top_right, Size(5.0f, 5.0f));
  EXPECT_EQ(rect.getRadii().bottom_left, Size(5.0f, 5.0f));
  EXPECT_EQ(rect.getRadii().bottom_right, Size(5.0f, 5.0f));
}

TEST(RoundRectTest, RectRadiusConstructor) {
  auto rect = RoundRect::MakeRectRadius(
    Rect::MakeLTRB(10.0f, 10.0f, 20.0f, 20.0f), 2.0f);

  EXPECT_FALSE(rect.isEmpty());
  EXPECT_FALSE(rect.isRect());
  EXPECT_FALSE(rect.isOval());
  EXPECT_TRUE(rect.isFinite());
  EXPECT_FALSE(rect.getBounds().isEmpty());
  EXPECT_EQ(rect.getBounds(), Rect::MakeLTRB(10.0f, 10.0f, 20.0f, 20.0f));
  EXPECT_EQ(rect.getRadii().top_left, Size(2.0f, 2.0f));
  EXPECT_EQ(rect.getRadii().top_right, Size(2.0f, 2.0f));
  EXPECT_EQ(rect.getRadii().bottom_left, Size(2.0f, 2.0f));
  EXPECT_EQ(rect.getRadii().bottom_right, Size(2.0f, 2.0f));
}

TEST(RoundRectTest, RectXYConstructor) {
  auto rect = RoundRect::MakeRectXY(
    Rect::MakeLTRB(10.0f, 10.0f, 20.0f, 20.0f), 2.0f, 3.0f);

  EXPECT_FALSE(rect.isEmpty());
  EXPECT_FALSE(rect.isRect());
  EXPECT_FALSE(rect.isOval());
  EXPECT_TRUE(rect.isFinite());
  EXPECT_FALSE(rect.getBounds().isEmpty());
  EXPECT_EQ(rect.getBounds(), Rect::MakeLTRB(10.0f, 10.0f, 20.0f, 20.0f));
  EXPECT_EQ(rect.getRadii().top_left, Size(2.0f, 3.0f));
  EXPECT_EQ(rect.getRadii().top_right, Size(2.0f, 3.0f));
  EXPECT_EQ(rect.getRadii().bottom_left, Size(2.0f, 3.0f));
  EXPECT_EQ(rect.getRadii().bottom_right, Size(2.0f, 3.0f));
}

TEST(RoundRectTest, RectSizeConstructor) {
  auto rect = RoundRect::MakeRectXY(
    Rect::MakeLTRB(10.0f, 10.0f, 20.0f, 20.0f), Size(2.0f, 3.0f));

  EXPECT_FALSE(rect.isEmpty());
  EXPECT_FALSE(rect.isRect());
  EXPECT_FALSE(rect.isOval());
  EXPECT_TRUE(rect.isFinite());
  EXPECT_FALSE(rect.getBounds().isEmpty());
  EXPECT_EQ(rect.getBounds(), Rect::MakeLTRB(10.0f, 10.0f, 20.0f, 20.0f));
  EXPECT_EQ(rect.getRadii().top_left, Size(2.0f, 3.0f));
  EXPECT_EQ(rect.getRadii().top_right, Size(2.0f, 3.0f));
  EXPECT_EQ(rect.getRadii().bottom_left, Size(2.0f, 3.0f));
  EXPECT_EQ(rect.getRadii().bottom_right, Size(2.0f, 3.0f));
}

TEST(RoundRectTest, RectRadiiConstructor) {
  auto rect =
    RoundRect::MakeRectRadii(Rect::MakeLTRB(10.0f, 10.0f, 20.0f, 20.0f),
    {
      .top_left = Size(1.0, 1.5),
      .top_right = Size(2.0, 2.5f),
      .bottom_left = Size(3.0, 3.5f),
      .bottom_right = Size(4.0, 4.5f),
    });

  EXPECT_FALSE(rect.isEmpty());
  EXPECT_FALSE(rect.isRect());
  EXPECT_FALSE(rect.isOval());
  EXPECT_TRUE(rect.isFinite());
  EXPECT_FALSE(rect.getBounds().isEmpty());
  EXPECT_EQ(rect.getBounds(), Rect::MakeLTRB(10.0f, 10.0f, 20.0f, 20.0f));
  EXPECT_EQ(rect.getRadii().top_left, Size(1.0f, 1.5f));
  EXPECT_EQ(rect.getRadii().top_right, Size(2.0f, 2.5f));
  EXPECT_EQ(rect.getRadii().bottom_left, Size(3.0f, 3.5f));
  EXPECT_EQ(rect.getRadii().bottom_right, Size(4.0f, 4.5f));
}

TEST(RoundRectTest, RectRadiiOverflowWidthConstructor) {
  auto rect =
    RoundRect::MakeRectRadii(Rect::MakeXYWH(10.0f, 10.0f, 6.0f, 30.0f),
    {
      .top_left = Size(1.0f, 2.0f),
      .top_right = Size(3.0f, 4.0f),
      .bottom_left = Size(5.0f, 6.0f),
      .bottom_right = Size(7.0f, 8.0f),
    });

  EXPECT_FALSE(rect.isEmpty());
  EXPECT_FALSE(rect.isRect());
  EXPECT_FALSE(rect.isOval());
  EXPECT_TRUE(rect.isFinite());
  EXPECT_FALSE(rect.getBounds().isEmpty());
  EXPECT_EQ(rect.getBounds(), Rect::MakeLTRB(10.0f, 10.0f, 16.0f, 40.0f));
  EXPECT_EQ(rect.getRadii().top_left, Size(0.5f, 1.0f));
  EXPECT_EQ(rect.getRadii().top_right, Size(1.5f, 2.0f));
  EXPECT_EQ(rect.getRadii().bottom_left, Size(2.5f, 3.0f));
  EXPECT_EQ(rect.getRadii().bottom_right, Size(3.5f, 4.0f));
}

TEST(RoundRectTest, RectRadiiOverflowHeightConstructor) {
  auto rect =
    RoundRect::MakeRectRadii(Rect::MakeXYWH(10.0f, 10.0f, 30.0f, 6.0f),
    {
      .top_left = Size(1.0f, 2.0f),
      .top_right = Size(3.0f, 4.0f),
      .bottom_left = Size(5.0f, 6.0f),
      .bottom_right = Size(7.0f, 8.0f),
    });
  // Largest sum of paired radii heights is the right edge which sums to 12
  // Rect is only 6 tall so all radii are scaled by half
  // Rect is 30 wide so no scaling should happen due to radii widths

  EXPECT_FALSE(rect.isEmpty());
  EXPECT_FALSE(rect.isRect());
  EXPECT_FALSE(rect.isOval());
  EXPECT_TRUE(rect.isFinite());
  EXPECT_FALSE(rect.getBounds().isEmpty());
  EXPECT_EQ(rect.getBounds(), Rect::MakeLTRB(10.0f, 10.0f, 40.0f, 16.0f));
  EXPECT_EQ(rect.getRadii().top_left, Size(0.5f, 1.0f));
  EXPECT_EQ(rect.getRadii().top_right, Size(1.5f, 2.0f));
  EXPECT_EQ(rect.getRadii().bottom_left, Size(2.5f, 3.0f));
  EXPECT_EQ(rect.getRadii().bottom_right, Size(3.5f, 4.0f));
}

TEST(RoundRectTest, Shift) {
  const auto rect =
    RoundRect::MakeRectRadii(Rect::MakeXYWH(10.0f, 10.0f, 30.0f, 30.0f),
    {
      .top_left = Size(1.0f, 2.0f),
      .top_right = Size(3.0f, 4.0f),
      .bottom_left = Size(5.0f, 6.0f),
      .bottom_right = Size(7.0f, 8.0f),
    });
  const auto shifted = rect.shift(5.0, 6.0);

  EXPECT_FALSE(shifted.isEmpty());
  EXPECT_FALSE(shifted.isRect());
  EXPECT_FALSE(shifted.isOval());
  EXPECT_TRUE(shifted.isFinite());
  EXPECT_FALSE(shifted.getBounds().isEmpty());
  EXPECT_EQ(shifted.getBounds(), Rect::MakeLTRB(15.0f, 16.0f, 45.0f, 46.0f));
  EXPECT_EQ(shifted.getRadii().top_left, Size(1.0f, 2.0f));
  EXPECT_EQ(shifted.getRadii().top_right, Size(3.0f, 4.0f));
  EXPECT_EQ(shifted.getRadii().bottom_left, Size(5.0f, 6.0f));
  EXPECT_EQ(shifted.getRadii().bottom_right, Size(7.0f, 8.0f));

  EXPECT_EQ(shifted,
    RoundRect::MakeRectRadii(Rect::MakeXYWH(15.0f, 16.0f, 30.0f, 30.0f),
      {
      .top_left = Size(1.0f, 2.0f),
      .top_right = Size(3.0f, 4.0f),
      .bottom_left = Size(5.0f, 6.0f),
      .bottom_right = Size(7.0f, 8.0f),
      }));
}

TEST(RoundRectTest, ExpandScalar) {
  const auto rect =
    RoundRect::MakeRectRadii(Rect::MakeXYWH(10.0f, 10.0f, 30.0f, 30.0f),
    {
      .top_left = Size(1.0f, 2.0f),
      .top_right = Size(3.0f, 4.0f),
      .bottom_left = Size(5.0f, 6.0f),
      .bottom_right = Size(7.0f, 8.0f),
    });
  const auto expanded = rect.getExpand(5.0);

  EXPECT_FALSE(expanded.isEmpty());
  EXPECT_FALSE(expanded.isRect());
  EXPECT_FALSE(expanded.isOval());
  EXPECT_TRUE(expanded.isFinite());
  EXPECT_FALSE(expanded.getBounds().isEmpty());
  EXPECT_EQ(expanded.getBounds(), Rect::MakeLTRB(5.0f, 5.0f, 45.0f, 45.0f));
  EXPECT_EQ(expanded.getRadii().top_left, Size(1.0f, 2.0f));
  EXPECT_EQ(expanded.getRadii().top_right, Size(3.0f, 4.0f));
  EXPECT_EQ(expanded.getRadii().bottom_left, Size(5.0f, 6.0f));
  EXPECT_EQ(expanded.getRadii().bottom_right, Size(7.0f, 8.0f));

  EXPECT_EQ(expanded,
    RoundRect::MakeRectRadii(Rect::MakeXYWH(5.0f, 5.0f, 40.0f, 40.0f),
      {
      .top_left = Size(1.0f, 2.0f),
      .top_right = Size(3.0f, 4.0f),
      .bottom_left = Size(5.0f, 6.0f),
      .bottom_right = Size(7.0f, 8.0f),
      }));
}

TEST(RoundRectTest, ExpandTwoScalars) {
  const auto rect =
    RoundRect::MakeRectRadii(Rect::MakeXYWH(10.0f, 10.0f, 30.0f, 30.0f),
    {
      .top_left = Size(1.0f, 2.0f),
      .top_right = Size(3.0f, 4.0f),
      .bottom_left = Size(5.0f, 6.0f),
      .bottom_right = Size(7.0f, 8.0f),
    });
  const auto expanded = rect.getExpand(5.0, 6.0);

  EXPECT_FALSE(expanded.isEmpty());
  EXPECT_FALSE(expanded.isRect());
  EXPECT_FALSE(expanded.isOval());
  EXPECT_TRUE(expanded.isFinite());
  EXPECT_FALSE(expanded.getBounds().isEmpty());
  EXPECT_EQ(expanded.getBounds(), Rect::MakeLTRB(5.0f, 4.0f, 45.0f, 46.0f));
  EXPECT_EQ(expanded.getRadii().top_left, Size(1.0f, 2.0f));
  EXPECT_EQ(expanded.getRadii().top_right, Size(3.0f, 4.0f));
  EXPECT_EQ(expanded.getRadii().bottom_left, Size(5.0f, 6.0f));
  EXPECT_EQ(expanded.getRadii().bottom_right, Size(7.0f, 8.0f));

  EXPECT_EQ(expanded,
    RoundRect::MakeRectRadii(Rect::MakeXYWH(5.0f, 4.0f, 40.0f, 42.0f),
      {
      .top_left = Size(1.0f, 2.0f),
      .top_right = Size(3.0f, 4.0f),
      .bottom_left = Size(5.0f, 6.0f),
      .bottom_right = Size(7.0f, 8.0f),
      }));
}

TEST(RoundRectTest, ExpandFourScalars) {
  const auto rect =
    RoundRect::MakeRectRadii(Rect::MakeXYWH(10.0f, 10.0f, 30.0f, 30.0f),
    {
      .top_left = Size(1.0f, 2.0f),
      .top_right = Size(3.0f, 4.0f),
      .bottom_left = Size(5.0f, 6.0f),
      .bottom_right = Size(7.0f, 8.0f),
    });
  const auto expanded = rect.getExpand(5.0, 6.0, 7.0, 8.0);

  EXPECT_FALSE(expanded.isEmpty());
  EXPECT_FALSE(expanded.isRect());
  EXPECT_FALSE(expanded.isOval());
  EXPECT_TRUE(expanded.isFinite());
  EXPECT_FALSE(expanded.getBounds().isEmpty());
  EXPECT_EQ(expanded.getBounds(), Rect::MakeLTRB(5.0f, 4.0f, 47.0f, 48.0f));
  EXPECT_EQ(expanded.getRadii().top_left, Size(1.0f, 2.0f));
  EXPECT_EQ(expanded.getRadii().top_right, Size(3.0f, 4.0f));
  EXPECT_EQ(expanded.getRadii().bottom_left, Size(5.0f, 6.0f));
  EXPECT_EQ(expanded.getRadii().bottom_right, Size(7.0f, 8.0f));

  EXPECT_EQ(expanded,
    RoundRect::MakeRectRadii(Rect::MakeXYWH(5.0f, 4.0f, 42.0f, 44.0f),
      {
      .top_left = Size(1.0f, 2.0f),
      .top_right = Size(3.0f, 4.0f),
      .bottom_left = Size(5.0f, 6.0f),
      .bottom_right = Size(7.0f, 8.0f),
      }));
}

TEST(RoundRectTest, ContractScalar) {
  const auto rect =
    RoundRect::MakeRectRadii(Rect::MakeXYWH(10.0f, 10.0f, 30.0f, 30.0f),
    {
      .top_left = Size(1.0f, 2.0f),
      .top_right = Size(3.0f, 4.0f),
      .bottom_left = Size(5.0f, 6.0f),
      .bottom_right = Size(7.0f, 8.0f),
    });
  const auto expanded = rect.getExpand(-2.0);

  EXPECT_FALSE(expanded.isEmpty());
  EXPECT_FALSE(expanded.isRect());
  EXPECT_FALSE(expanded.isOval());
  EXPECT_TRUE(expanded.isFinite());
  EXPECT_FALSE(expanded.getBounds().isEmpty());
  EXPECT_EQ(expanded.getBounds(), Rect::MakeLTRB(12.0f, 12.0f, 38.0f, 38.0f));
  EXPECT_EQ(expanded.getRadii().top_left, Size(1.0f, 2.0f));
  EXPECT_EQ(expanded.getRadii().top_right, Size(3.0f, 4.0f));
  EXPECT_EQ(expanded.getRadii().bottom_left, Size(5.0f, 6.0f));
  EXPECT_EQ(expanded.getRadii().bottom_right, Size(7.0f, 8.0f));

  EXPECT_EQ(expanded,
    RoundRect::MakeRectRadii(Rect::MakeXYWH(12.0f, 12.0f, 26.0f, 26.0f),
      {
      .top_left = Size(1.0f, 2.0f),
      .top_right = Size(3.0f, 4.0f),
      .bottom_left = Size(5.0f, 6.0f),
      .bottom_right = Size(7.0f, 8.0f),
      }));
}

TEST(RoundRectTest, ContractTwoScalars) {
  const auto rect =
    RoundRect::MakeRectRadii(Rect::MakeXYWH(10.0f, 10.0f, 30.0f, 30.0f),
    {
      .top_left = Size(1.0f, 2.0f),
      .top_right = Size(3.0f, 4.0f),
      .bottom_left = Size(5.0f, 6.0f),
      .bottom_right = Size(7.0f, 8.0f),
    });
  const auto expanded = rect.getExpand(-1.0, -2.0);

  EXPECT_FALSE(expanded.isEmpty());
  EXPECT_FALSE(expanded.isRect());
  EXPECT_FALSE(expanded.isOval());
  EXPECT_TRUE(expanded.isFinite());
  EXPECT_FALSE(expanded.getBounds().isEmpty());
  EXPECT_EQ(expanded.getBounds(), Rect::MakeLTRB(11.0f, 12.0f, 39.0f, 38.0f));
  EXPECT_EQ(expanded.getRadii().top_left, Size(1.0f, 2.0f));
  EXPECT_EQ(expanded.getRadii().top_right, Size(3.0f, 4.0f));
  EXPECT_EQ(expanded.getRadii().bottom_left, Size(5.0f, 6.0f));
  EXPECT_EQ(expanded.getRadii().bottom_right, Size(7.0f, 8.0f));

  EXPECT_EQ(expanded,
    RoundRect::MakeRectRadii(Rect::MakeXYWH(11.0f, 12.0f, 28.0f, 26.0f),
      {
      .top_left = Size(1.0f, 2.0f),
      .top_right = Size(3.0f, 4.0f),
      .bottom_left = Size(5.0f, 6.0f),
      .bottom_right = Size(7.0f, 8.0f),
      }));
}

TEST(RoundRectTest, ContractFourScalars) {
  const auto rect =
    RoundRect::MakeRectRadii(Rect::MakeXYWH(10.0f, 10.0f, 30.0f, 30.0f),
    {
      .top_left = Size(1.0f, 2.0f),
      .top_right = Size(3.0f, 4.0f),
      .bottom_left = Size(5.0f, 6.0f),
      .bottom_right = Size(7.0f, 8.0f),
    });
  const auto expanded = rect.getExpand(-1.0, -1.5, -2.0, -2.5);

  EXPECT_FALSE(expanded.isEmpty());
  EXPECT_FALSE(expanded.isRect());
  EXPECT_FALSE(expanded.isOval());
  EXPECT_TRUE(expanded.isFinite());
  EXPECT_FALSE(expanded.getBounds().isEmpty());
  EXPECT_EQ(expanded.getBounds(), Rect::MakeLTRB(11.0f, 11.5f, 38.0f, 37.5f));
  EXPECT_EQ(expanded.getRadii().top_left, Size(1.0f, 2.0f));
  EXPECT_EQ(expanded.getRadii().top_right, Size(3.0f, 4.0f));
  EXPECT_EQ(expanded.getRadii().bottom_left, Size(5.0f, 6.0f));
  EXPECT_EQ(expanded.getRadii().bottom_right, Size(7.0f, 8.0f));

  EXPECT_EQ(expanded,
    RoundRect::MakeRectRadii(Rect::MakeXYWH(11.0f, 11.5f, 27.0f, 26.0f),
      {
      .top_left = Size(1.0f, 2.0f),
      .top_right = Size(3.0f, 4.0f),
      .bottom_left = Size(5.0f, 6.0f),
      .bottom_right = Size(7.0f, 8.0f),
      }));
}

TEST(RoundRectTest, ContractAndRequireRadiiAdjustment) {
  const auto rect =
    RoundRect::MakeRectRadii(Rect::MakeXYWH(10.0f, 10.0f, 30.0f, 30.0f),
    {
      .top_left = Size(1.0f, 2.0f),
      .top_right = Size(3.0f, 4.0f),
      .bottom_left = Size(5.0f, 6.0f),
      .bottom_right = Size(7.0f, 8.0f),
    });
  auto expanded = rect.getExpand(-12.0);

  EXPECT_FALSE(expanded.isEmpty());
  EXPECT_FALSE(expanded.isRect());
  EXPECT_FALSE(expanded.isOval());
  EXPECT_TRUE(expanded.isFinite());
  EXPECT_FALSE(expanded.getBounds().isEmpty());
  EXPECT_EQ(expanded.getBounds(), Rect::MakeLTRB(22.0f, 22.0f, 28.0f, 28.0f));
  EXPECT_EQ(expanded.getRadii().top_left, Size(0.5f, 1.0f));
  EXPECT_EQ(expanded.getRadii().top_right, Size(1.5f, 2.0f));
  EXPECT_EQ(expanded.getRadii().bottom_left, Size(2.5f, 3.0f));
  EXPECT_EQ(expanded.getRadii().bottom_right, Size(3.5f, 4.0f));

  // In this test, the MakeRectRadii constructor will make the same
  // adjustment to the radii that the Expand method applied.
  EXPECT_EQ(expanded,
    RoundRect::MakeRectRadii(Rect::MakeXYWH(22.0f, 22.0f, 6.0f, 6.0f),
      {
      .top_left = Size(1.0f, 2.0f),
      .top_right = Size(3.0f, 4.0f),
      .bottom_left = Size(5.0f, 6.0f),
      .bottom_right = Size(7.0f, 8.0f),
      }));

  // In this test, the arguments to the constructor supply the correctly
  // adjusted radii (though there is no real way to tell other than
  // the result is the same).
  EXPECT_EQ(expanded,
    RoundRect::MakeRectRadii(Rect::MakeXYWH(22.0f, 22.0f, 6.0f, 6.0f),
      {
      .top_left = Size(0.5f, 1.0f),
      .top_right = Size(1.5f, 2.0f),
      .bottom_left = Size(2.5f, 3.0f),
      .bottom_right = Size(3.5f, 4.0f),
      }));
}

TEST(RoundRectTest, NoCornerRoundRectContains) {
  const auto bounds = Rect::MakeLTRB(-50.0f, -50.0f, 50.0f, 50.0f);
  // RRect of bounds with no corners contains corners just barely
  auto noCornerRect = RoundRect::MakeRectXY(bounds, 0.0f, 0.0f);

  EXPECT_TRUE(noCornerRect.contains({-50, -50}));
  // Rectangles have half-in, half-out containment so we need
  // to be careful about testing containment of right/bottom corners.
  EXPECT_TRUE(noCornerRect.contains({-50, 49.99}));
  EXPECT_TRUE(noCornerRect.contains({49.99, -50}));
  EXPECT_TRUE(noCornerRect.contains({49.99, 49.99}));
  EXPECT_FALSE(noCornerRect.contains({-50.01, -50}));
  EXPECT_FALSE(noCornerRect.contains({-50, -50.01}));
  EXPECT_FALSE(noCornerRect.contains({-50.01, 50}));
  EXPECT_FALSE(noCornerRect.contains({-50, 50.01}));
  EXPECT_FALSE(noCornerRect.contains({50.01, -50}));
  EXPECT_FALSE(noCornerRect.contains({50, -50.01}));
  EXPECT_FALSE(noCornerRect.contains({50.01, 50}));
  EXPECT_FALSE(noCornerRect.contains({50, 50.01}));
}

TEST(RoundRectTest, TinyCornerRoundRectContains) {
  const auto bounds = Rect::MakeLTRB(-50.0f, -50.0f, 50.0f, 50.0f);
  // RRect of bounds with even the tiniest corners does not contain corners
  const auto tinyCornerRect = RoundRect::MakeRectXY(bounds, 0.01f, 0.01f);

  EXPECT_FALSE(tinyCornerRect.contains({-50, -50}));
  EXPECT_FALSE(tinyCornerRect.contains({-50, 50}));
  EXPECT_FALSE(tinyCornerRect.contains({50, -50}));
  EXPECT_FALSE(tinyCornerRect.contains({50, 50}));
}

TEST(RoundRectTest, UniformCircularRoundRectContains) {
  const auto bounds = Rect::MakeLTRB(-50.0f, -50.0f, 50.0f, 50.0f);
  const auto expanded = RoundRect::MakeRectXY(bounds.getExpand(2.0), 2.0f, 2.0f);

  // Expanded by 2.0 and then with a corner of 2.0 obviously still
  // contains the corners
  EXPECT_TRUE(expanded.contains({-50, -50}));
  EXPECT_TRUE(expanded.contains({-50, 50}));
  EXPECT_TRUE(expanded.contains({50, -50}));
  EXPECT_TRUE(expanded.contains({50, 50}));

  // Now we try to box in the corner containment to exactly where the
  // rounded corner of the expanded round rect with radii of 2.0 lies.
  // The 45-degree diagonal point of a circle of radius 2.0 lies at:
  //
  // (2 * sqrt(2) / 2, 2 * sqrt(2) / 2)
  // (sqrt(2), sqrt(2))
  //
  // So we test +/- (50 + sqrt(2) +/- epsilon)
  const auto coordOut = 50 + kSqrt2 + kEhCloseEnough;
  const auto coordIn = 50 + kSqrt2 - kEhCloseEnough;
  // Upper left corner
  EXPECT_TRUE(expanded.contains({-coordIn, -coordIn}));
  EXPECT_FALSE(expanded.contains({-coordOut, -coordOut}));
  // Upper right corner
  EXPECT_TRUE(expanded.contains({coordIn, -coordIn}));
  EXPECT_FALSE(expanded.contains({coordOut, -coordOut}));
  // Lower left corner
  EXPECT_TRUE(expanded.contains({-coordIn, coordIn}));
  EXPECT_FALSE(expanded.contains({-coordOut, coordOut}));
  // Lower right corner
  EXPECT_TRUE(expanded.contains({coordIn, coordIn}));
  EXPECT_FALSE(expanded.contains({coordOut, coordOut}));
}

TEST(RoundRectTest, UniformEllipticalRoundRectContains) {
  const auto bounds = Rect::MakeLTRB(-50.0f, -50.0f, 50.0f, 50.0f);
  const auto expanded = RoundRect::MakeRectXY(bounds.getExpand(2.0), 2.0f, 3.0f);

  // Expanded by 2.0 and then with a corner of 2x3 should still
  // contain the corners
  EXPECT_TRUE(expanded.contains({-50, -50}));
  EXPECT_TRUE(expanded.contains({-50, 50}));
  EXPECT_TRUE(expanded.contains({50, -50}));
  EXPECT_TRUE(expanded.contains({50, 50}));

  // Now we try to box in the corner containment to exactly where the
  // rounded corner of the expanded round rect with radii of 2x3 lies.
  // The "45-degree diagonal point" of an ellipse of radii 2x3 lies at:
  //
  // (2 * sqrt(2) / 2, 3 * sqrt(2) / 2)
  // (sqrt(2), 3 * sqrt(2) / 2)
  //
  // And the center(s) of these corners are at:
  // (+/-(50 + 2 - 2), +/-(50 + 2 - 3))
  // = (+/-50, +/-49)
  const auto xCoordOut = 50 + kSqrt2 + kEhCloseEnough;
  const auto xCoordIn = 50 + kSqrt2 - kEhCloseEnough;
  const auto yCoordOut = 49 + 3 * kSqrt2 / 2 + kEhCloseEnough;
  const auto yCoordIn = 49 + 3 * kSqrt2 / 2 - kEhCloseEnough;
  // Upper left corner
  EXPECT_TRUE(expanded.contains({-xCoordIn, -yCoordIn}));
  EXPECT_FALSE(expanded.contains({-xCoordOut, -yCoordOut}));
  // Upper right corner
  EXPECT_TRUE(expanded.contains({xCoordIn, -yCoordIn}));
  EXPECT_FALSE(expanded.contains({xCoordOut, -yCoordOut}));
  // Lower left corner
  EXPECT_TRUE(expanded.contains({-xCoordIn, yCoordIn}));
  EXPECT_FALSE(expanded.contains({-xCoordOut, yCoordOut}));
  // Lower right corner
  EXPECT_TRUE(expanded.contains({xCoordIn, yCoordIn}));
  EXPECT_FALSE(expanded.contains({xCoordOut, yCoordOut}));
}

TEST(RoundRectTest, DifferingCornersRoundRectContains) {
  const auto bounds = Rect::MakeLTRB(-50.0f, -50.0f, 50.0f, 50.0f);
  const auto round_rect =
    RoundRect::MakeRectRadii(bounds, {
      .top_left = Size(2.0, 3.0),
      .top_right = Size(4.0, 5.0),
      .bottom_left = Size(6.0, 7.0),
      .bottom_right = Size(8.0, 9.0),
    });

  // For a corner with radii {A, B}, the "45 degree point" on the
  // corner curve will be at an offset of:
  //
  // (A * sqrt(2) / 2, B * sqrt(2) / 2)
  //
  // And the center(s) of these corners are at:
  //
  // (+/-(50 - A), +/-(50 - B))
  auto coord = [](Scalar radius) {
    return 50 - radius + radius * kSqrt2 / 2.0f - kEhCloseEnough;
  };
  auto coordIn = [&coord](Scalar radius) {
    return coord(radius) - kEhCloseEnough;
  };
  auto coordOut = [&coord](Scalar radius) {
    // For some reason 1 kEhCloseEnough is not enough to put us outside
    // in some of the cases, so we use 2x the epsilon.
    return coord(radius) + 2 * kEhCloseEnough;
  };
  // Upper left corner (radii = {2.0, 3.0})
  EXPECT_TRUE(round_rect.contains({-coordIn(2.0), -coordIn(3.0)}));
  EXPECT_FALSE(round_rect.contains({-coordOut(2.0), -coordOut(3.0)}));
  // Upper right corner (radii = {4.0, 5.0})
  EXPECT_TRUE(round_rect.contains({coordIn(4.0), -coordIn(5.0)}));
  EXPECT_FALSE(round_rect.contains({coordOut(4.0), -coordOut(5.0)}));
  // Lower left corner (radii = {6.0, 7.0})
  EXPECT_TRUE(round_rect.contains({-coordIn(6.0), coordIn(7.0)}));
  EXPECT_FALSE(round_rect.contains({-coordOut(6.0), coordOut(7.0)}));
  // Lower right corner (radii = {8.0, 9.0})
  EXPECT_TRUE(round_rect.contains({coordIn(8.0), coordIn(9.0)}));
  EXPECT_FALSE(round_rect.contains({coordOut(8.0), coordOut(9.0)}));
}
