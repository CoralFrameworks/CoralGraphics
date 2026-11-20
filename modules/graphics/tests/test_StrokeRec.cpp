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
 * See the License for the specific language governing permissions and limitations under the License.
 */

/**
 * @file test_StrokeRec.cpp
 *
 * The stroke parameters, and the two places a caller can be surprised by them.
 *
 * The behaviour worth pinning is not the plumbing -- taking a paint's width and giving it back -- but the three
 * decisions that are *not* obvious from the names:
 *
 *  - a width of zero is a shorthand, not a request to draw nothing;
 *  - a hairline cannot be computed here, and says so rather than returning a wrong outline;
 *  - the inflation radius is a deliberate over-estimate, and only a miter join and a square cap change it.
 *
 * The outline itself is checked by its bounds rather than its verbs: the rasterizer's stroke is already covered by
 * the canvas tests, and what this file is responsible for is passing the width through and reporting what happened.
 */

#include <graphics/Path.h>
#include <graphics/StrokeRec.h>

#include <gtest/gtest.h>

#include <cmath>

using namespace graphics;

namespace {

/** A rectangle as a path, which strokes into a shape whose bounds are easy to predict. */
Path rect(const Scalar left, const Scalar top, const Scalar right, const Scalar bottom) {
  Path path;
  path.moveTo(Point { left, top });
  path.lineTo(Point { right, top });
  path.lineTo(Point { right, bottom });
  path.lineTo(Point { left, bottom });
  path.close();
  return path;
}

} // namespace anonymous

// ==================================================================================================
// Construction
// ==================================================================================================

TEST(StrokeRecTest, TakesItsParametersFromAPaint) {
  Paint paint;
  paint.strokeWidth = 3.5f;
  paint.strokeMiter = 7.0f;
  paint.strokeCap = Paint::Cap::kRound;
  paint.strokeJoin = Paint::Join::kBevel;
  paint.style = Paint::Style::kStroke;

  const StrokeRec stroke(paint);
  EXPECT_EQ(stroke.style(), StrokeRec::Style::kStroke);
  EXPECT_FLOAT_EQ(stroke.width(), 3.5f);
  EXPECT_FLOAT_EQ(stroke.miter(), 7.0f);
  EXPECT_EQ(stroke.cap(), Paint::Cap::kRound);
  EXPECT_EQ(stroke.join(), Paint::Join::kBevel);
  EXPECT_TRUE(stroke.needToApply()) << "a stroke changes the path";
}

TEST(StrokeRecTest, AResScaleThatWouldBreakTheOutlineIsIgnored) {
  Paint paint;
  paint.strokeWidth = 2.0f;
  paint.style = Paint::Style::kStroke;
  const StrokeRec stroke(paint, 4.0f);
  EXPECT_FLOAT_EQ(stroke.resScale(), 4.0f);

  // A scale of zero or below, or a NaN, would make every tolerance in the outline computation meaningless -- a zero
  // tolerance flattens every curve into an unbounded number of segments. Ignoring it keeps the previous scale, which
  // is what a caller that passed a broken value needs: the drawing continues rather than hanging.
  StrokeRec mutableStroke = stroke;
  mutableStroke.setResScale(0.0f);
  EXPECT_FLOAT_EQ(mutableStroke.resScale(), 4.0f) << "zero is not a usable scale";

  mutableStroke.setResScale(-2.0f);
  EXPECT_FLOAT_EQ(mutableStroke.resScale(), 4.0f);

  mutableStroke.setResScale(std::nanf(""));
  EXPECT_FLOAT_EQ(mutableStroke.resScale(), 4.0f);

  mutableStroke.setResScale(2.5f);
  EXPECT_FLOAT_EQ(mutableStroke.resScale(), 2.5f) << "a usable scale is taken";
}

TEST(StrokeRecTest, SettingAFillOrHairlineStyleClearsTheStroke) {
  StrokeRec stroke(StrokeRec::InitStyle::kFill);
  EXPECT_TRUE(stroke.isFillStyle());
  EXPECT_FALSE(stroke.needToApply());

  stroke.setStrokeStyle(5.0f);
  EXPECT_TRUE(stroke.needToApply());

  stroke.setHairlineStyle();
  EXPECT_TRUE(stroke.isHairlineStyle());
  EXPECT_FALSE(stroke.needToApply()) << "a hairline is not computed here, so it is not 'applied'";
}

// ==================================================================================================
// The zero-width shorthand
// ==================================================================================================

TEST(StrokeRecTest, AWidthOfZeroIsAShorthandRatherThanNothing) {
  // This is the surprising one. Asking for a stroke of zero width does *not* mean "draw nothing": it means "the
  // thinnest line available", which is a hairline, or a fill when the caller also asked to fill. A caller that wants
  // nothing sets the style to fill and does not stroke.
  StrokeRec hairline(StrokeRec::InitStyle::kFill);
  hairline.setStrokeStyle(0.0f, false);
  EXPECT_TRUE(hairline.isHairlineStyle()) << "a zero-width stroke is a hairline";

  StrokeRec both(StrokeRec::InitStyle::kFill);
  both.setStrokeStyle(0.0f, true);
  EXPECT_TRUE(both.isFillStyle()) << "a zero-width stroke that also fills is just a fill";

  // A negative width takes the same path rather than producing a stroke that grows inwards.
  StrokeRec negative(StrokeRec::InitStyle::kFill);
  negative.setStrokeStyle(-1.0f, false);
  EXPECT_TRUE(negative.isHairlineStyle());
}

TEST(StrokeRecTest, APositiveWidthStrokes) {
  StrokeRec stroke(StrokeRec::InitStyle::kFill);
  stroke.setStrokeStyle(2.0f);
  EXPECT_EQ(stroke.style(), StrokeRec::Style::kStroke);
  EXPECT_FLOAT_EQ(stroke.width(), 2.0f);

  stroke.setStrokeStyle(2.0f, true);
  EXPECT_EQ(stroke.style(), StrokeRec::Style::kStrokeAndFill);
  EXPECT_TRUE(stroke.needToApply());
}

// ==================================================================================================
// Applying to a path
// ==================================================================================================

TEST(StrokeRecTest, AFillLeavesThePathAloneAndSaysSo) {
  const Path square = rect(0.0f, 0.0f, 10.0f, 10.0f);

  StrokeRec fill(StrokeRec::InitStyle::kFill);
  Path destination;
  EXPECT_FALSE(fill.applyToPath(&destination, square))
          << "a fill does not produce an outline, and the caller is told rather than given an empty path";
  EXPECT_TRUE(destination.isEmpty()) << "and the destination is untouched";
}

TEST(StrokeRecTest, AHairlineIsRefusedRatherThanApproximated) {
  const Path square = rect(0.0f, 0.0f, 10.0f, 10.0f);

  StrokeRec hairline(StrokeRec::InitStyle::kHairline);
  Path destination;
  EXPECT_FALSE(hairline.applyToPath(&destination, square))
          << "a hairline is one device pixel wide, which this layer cannot know; refusing is the honest answer";
}

TEST(StrokeRecTest, AStrokeWidensThePath) {
  const Path square = rect(0.0f, 0.0f, 10.0f, 10.0f);

  StrokeRec stroke(StrokeRec::InitStyle::kFill);
  stroke.setStrokeStyle(4.0f);

  Path outline;
  ASSERT_TRUE(stroke.applyToPath(&outline, square)) << "a stroke produces an outline";
  EXPECT_FALSE(outline.isEmpty());

  // The outline reaches half the stroke width beyond the path on every side. A butt cap does not extend past the
  // ends of a closed contour, so all four edges move out by exactly half the width.
  const Rect bounds = outline.bounds();
  EXPECT_NEAR(bounds.getLeft(), -2.0f, 0.05f) << "half of four units to the left";
  EXPECT_NEAR(bounds.getTop(), -2.0f, 0.05f);
  EXPECT_NEAR(bounds.getRight(), 12.0f, 0.05f);
  EXPECT_NEAR(bounds.getBottom(), 12.0f, 0.05f);
}

TEST(StrokeRecTest, AWiderStrokeWidensFurther) {
  const Path square = rect(0.0f, 0.0f, 10.0f, 10.0f);

  // The relationship, not just a value: doubling the width doubles how far the outline reaches. A caller that reads
  // the width from the wrong place would produce different absolute numbers and still pass a single-value check.
  StrokeRec narrow(StrokeRec::InitStyle::kFill);
  narrow.setStrokeStyle(2.0f);
  StrokeRec wide(StrokeRec::InitStyle::kFill);
  wide.setStrokeStyle(8.0f);

  Path narrowOutline;
  Path wideOutline;
  ASSERT_TRUE(narrow.applyToPath(&narrowOutline, square));
  ASSERT_TRUE(wide.applyToPath(&wideOutline, square));

  const Scalar narrowOutset = -narrowOutline.bounds().getLeft();
  const Scalar wideOutset = -wideOutline.bounds().getLeft();
  EXPECT_NEAR(wideOutset, narrowOutset * 4.0f, 0.1f) << "four times the width is four times the outset";
}

TEST(StrokeRecTest, StrokeAndFillCoversThePathAsWellAsTheOutline) {
  const Path square = rect(0.0f, 0.0f, 10.0f, 10.0f);

  StrokeRec strokeAndFill(StrokeRec::InitStyle::kFill);
  strokeAndFill.setStrokeStyle(4.0f, true);
  ASSERT_EQ(strokeAndFill.style(), StrokeRec::Style::kStrokeAndFill);

  Path result;
  ASSERT_TRUE(strokeAndFill.applyToPath(&result, square));

  // The union of a path with a shape that contains it is at least as large, so nothing the original covered is lost.
  // This is the property that distinguishes stroke-and-fill from a plain stroke.
  EXPECT_LE(result.bounds().getLeft(), -2.0f + 0.05f);
  EXPECT_GE(result.bounds().getRight(), 12.0f - 0.05f);
}

TEST(StrokeRecTest, ApplyingToANullDestinationIsRefused) {
  const Path square = rect(0.0f, 0.0f, 10.0f, 10.0f);
  StrokeRec stroke(StrokeRec::InitStyle::kFill);
  stroke.setStrokeStyle(2.0f);

  EXPECT_FALSE(stroke.applyToPath(nullptr, square)) << "there is nowhere to put the result";
}

TEST(StrokeRecTest, AnEmptySourceProducesNothing) {
  StrokeRec stroke(StrokeRec::InitStyle::kFill);
  stroke.setStrokeStyle(2.0f);

  Path destination;
  EXPECT_FALSE(stroke.applyToPath(&destination, Path {})) << "there is nothing to stroke";
}

// ==================================================================================================
// Applying back to a paint
// ==================================================================================================

TEST(StrokeRecTest, ApplyingToAPaintWritesTheStrokeAndNothingElse) {
  Paint paint;
  paint.strokeWidth = 1.0f;
  paint.strokeCap = Paint::Cap::kButt;
  paint.strokeJoin = Paint::Join::kMiter;
  paint.color = Color::Red();

  StrokeRec stroke(StrokeRec::InitStyle::kFill);
  stroke.setStrokeStyle(6.0f);
  stroke.setStrokeParams(Paint::Cap::kRound, Paint::Join::kBevel, 9.0f);
  stroke.applyToPaint(&paint);

  EXPECT_FLOAT_EQ(paint.strokeWidth, 6.0f);
  EXPECT_EQ(paint.strokeCap, Paint::Cap::kRound);
  EXPECT_EQ(paint.strokeJoin, Paint::Join::kBevel);
  EXPECT_FLOAT_EQ(paint.strokeMiter, 9.0f);
  EXPECT_EQ(paint.style, Paint::Style::kStroke);

  // The colour is the caller's and has nothing to do with the geometry, so it must survive untouched.
  EXPECT_EQ(paint.color, Color::Red()) << "the stroke parameters do not include the colour";
}

// ==================================================================================================
// Inflation
// ==================================================================================================

TEST(StrokeRecTest, AFillInflatesNothing) {
  Paint paint;
  paint.strokeWidth = 100.0f;
  EXPECT_FLOAT_EQ(StrokeRec::InflationRadius(paint, Paint::Style::kFill), 0.0f);
}

TEST(StrokeRecTest, AHairlineInflatesByOneUnit) {
  // A hairline is one device pixel wide, and this cannot know the transform. One unit is the smallest answer that is
  // never an under-estimate on an unscaled device, which is the safe direction for a bound used to decide whether
  // anything is visible.
  EXPECT_FLOAT_EQ(StrokeRec::InflationRadius(Paint::Join::kMiter, 4.0f, Paint::Cap::kButt, 0.0f), 1.0f);
}

TEST(StrokeRecTest, OnlyAMiterJoinAndASquareCapChangeTheInflation) {
  // Half the width is the floor, which is what a butt cap on a bevel or round join reaches.
  EXPECT_FLOAT_EQ(StrokeRec::InflationRadius(Paint::Join::kRound, 4.0f, Paint::Cap::kButt, 10.0f), 5.0f);
  EXPECT_FLOAT_EQ(StrokeRec::InflationRadius(Paint::Join::kBevel, 4.0f, Paint::Cap::kButt, 10.0f), 5.0f);
  EXPECT_FLOAT_EQ(StrokeRec::InflationRadius(Paint::Join::kRound, 4.0f, Paint::Cap::kRound, 10.0f), 5.0f);

  // A miter reaches `miterLimit` times the half-width.
  EXPECT_FLOAT_EQ(StrokeRec::InflationRadius(Paint::Join::kMiter, 3.0f, Paint::Cap::kButt, 10.0f), 15.0f);
  // A square cap reaches the half-width times the diagonal of a unit square.
  EXPECT_NEAR(StrokeRec::InflationRadius(Paint::Join::kRound, 4.0f, Paint::Cap::kSquare, 10.0f), 5.0f * 1.41421356f,
          1e-4f);
  // A miter limit below one does not shrink the bound below the half-width, which every join reaches.
  EXPECT_FLOAT_EQ(StrokeRec::InflationRadius(Paint::Join::kMiter, 0.5f, Paint::Cap::kButt, 10.0f), 5.0f);
}

// ==================================================================================================
// Identity
// ==================================================================================================

TEST(StrokeRecTest, TheResolutionScaleIsNotPartOfAStrokesIdentity) {
  Paint paint;
  paint.strokeWidth = 3.0f;
  paint.style = Paint::Style::kStroke;

  const StrokeRec atOne(paint, 1.0f);
  const StrokeRec atFour(paint, 4.0f);

  // The scale is a precision hint, so two strokes that differ only in it produce the same shape. A cache keyed on
  // this must not miss because of a hint.
  EXPECT_TRUE(atOne.hasEqualEffect(atFour)) << "the scale does not change the outline";
}

TEST(StrokeRecTest, DifferentStrokesHaveDifferentEffects) {
  Paint paint;
  paint.strokeWidth = 3.0f;
  paint.style = Paint::Style::kStroke;
  const StrokeRec reference(paint);

  Paint wider = paint;
  wider.strokeWidth = 4.0f;
  EXPECT_FALSE(reference.hasEqualEffect(StrokeRec(wider))) << "a different width is a different outline";

  Paint roundCap = paint;
  roundCap.strokeCap = Paint::Cap::kRound;
  EXPECT_FALSE(reference.hasEqualEffect(StrokeRec(roundCap)));

  Paint roundJoin = paint;
  roundJoin.strokeJoin = Paint::Join::kRound;
  EXPECT_FALSE(reference.hasEqualEffect(StrokeRec(roundJoin)));

  // A fill and a hairline both produce no outline, but they are not the same instruction.
  EXPECT_FALSE(StrokeRec(StrokeRec::InitStyle::kFill).hasEqualEffect(StrokeRec(StrokeRec::InitStyle::kHairline)));
  EXPECT_TRUE(StrokeRec(StrokeRec::InitStyle::kFill).hasEqualEffect(StrokeRec(StrokeRec::InitStyle::kFill)));
}

TEST(StrokeRecTest, TheMiterLimitOnlyMattersForAMiterJoin) {
  Paint paint;
  paint.strokeWidth = 3.0f;
  paint.strokeJoin = Paint::Join::kMiter;
  paint.strokeMiter = 4.0f;
  paint.style = Paint::Style::kStroke;
  const StrokeRec reference(paint);

  Paint otherMiter = paint;
  otherMiter.strokeMiter = 8.0f;
  EXPECT_FALSE(reference.hasEqualEffect(StrokeRec(otherMiter))) << "a miter join reads the limit";

  // With a round join the limit reaches nothing, so two strokes that differ only in it are the same stroke. Treating
  // them as different would split a cache for no reason.
  Paint round = paint;
  round.strokeJoin = Paint::Join::kRound;
  const StrokeRec roundReference(round);
  Paint roundOtherLimit = round;
  roundOtherLimit.strokeMiter = 8.0f;
  EXPECT_TRUE(roundReference.hasEqualEffect(StrokeRec(roundOtherLimit)))
          << "a round join does not read the miter limit";
}
