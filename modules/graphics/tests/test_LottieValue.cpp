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
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file test_LottieValue.cpp
 *
 * Animation values and the easing between their keyframes.
 *
 * The easing is the part with a wrong answer that still looks like an answer. A cubic evaluated at the time fraction
 * rather than inverted through it produces motion that starts, ends and is monotone -- and is a different curve from the
 * one the designer drew, everywhere except the endpoints. So the tests below pin the curve at points where the two
 * readings disagree, and against the shapes a designer actually produces: ease-in, ease-out, and a hold.
 */

#include "lottie/LottieValue.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

using graphics::Color;
using graphics::Point;
using graphics::Scalar;
using graphics::lottie::Animatable;
using graphics::lottie::easeProgress;
using graphics::lottie::Keyframe;

/** An ease that starts fast and slows down: the control points CSS writes as `cubic-bezier(0.42, 0, 1, 1)`. */
constexpr Point kEaseIn {0.42f, 0.0f};
constexpr Point kEaseInEnd {1.0f, 1.0f};

/** The linear identity, which is what an exporter writes when nothing was eased. */
constexpr Point kLinearOut {1.0f / 3.0f, 1.0f / 3.0f};
constexpr Point kLinearIn {2.0f / 3.0f, 2.0f / 3.0f};

} // namespace anonymous

// ==================================================================================================
// The easing curve
// ==================================================================================================

TEST(LottieEase, IsTheIdentityForTheLinearControlPoints) {
  // Linear is not a special case in the arithmetic, but it is one in the answer: an exporter writes these points for
  // every un-eased keyframe, so a rounding error here would be in almost every value of almost every document.
  for (const Scalar t : {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f}) {
    EXPECT_FLOAT_EQ(easeProgress(t, kLinearOut, kLinearIn), t) << "at t = " << t;
  }
}

TEST(LottieEase, PinsBothEndpoints) {
  EXPECT_FLOAT_EQ(easeProgress(0.0f, kEaseIn, kEaseInEnd), 0.0f);
  EXPECT_FLOAT_EQ(easeProgress(1.0f, kEaseIn, kEaseInEnd), 1.0f);
}

TEST(LottieEase, ClampsOutsideTheUnitInterval) {
  // A caller asking about a moment inside a segment cannot produce these; one that did has the wrong segment, and
  // continuing the curve past its own ends would produce a value the document never described.
  EXPECT_FLOAT_EQ(easeProgress(-1.0f, kEaseIn, kEaseInEnd), 0.0f);
  EXPECT_FLOAT_EQ(easeProgress(2.0f, kEaseIn, kEaseInEnd), 1.0f);
}

TEST(LottieEase, IsNotTheCurveEvaluatedAtTheTimeFraction) {
  // The mistake this file exists to rule out. Evaluating the cubic's y at s = t instead of inverting x first gives a
  // different number everywhere in between, and the two agree exactly at 0, 0.5 for a symmetric curve, and 1 -- which is
  // why a test that only checked those would pass a broken implementation.
  //
  // `cubic-bezier(0.42, 0, 1, 1)` is CSS's ease-in. Inverting x(s) = 0.25 gives s = 0.1885, and y(0.1885) = 0.0932:
  // an ease-in is deliberately slow at the start. Evaluating the cubic's y directly at s = 0.25 instead gives 0.156, so
  // the two readings differ by more than half the correct value -- which is what makes this worth pinning.
  const Scalar eased = easeProgress(0.25f, kEaseIn, kEaseInEnd);
  EXPECT_NEAR(eased, 0.0932f, 0.005f) << "the curve, inverted through x";
  EXPECT_LT(eased, 0.12f) << "and nowhere near the un-inverted 0.156";
}

TEST(LottieEase, SlowsDownAtTheEndForAnEaseOut) {
  // `cubic-bezier(0, 0, 0.58, 1)`: fast at the start, slow at the finish. Half the time has to be more than half the
  // change, which is what makes it an ease-out rather than an ease-in.
  const Point out {0.0f, 0.0f};
  const Point in {0.58f, 1.0f};
  EXPECT_GT(easeProgress(0.5f, out, in), 0.5f);
  // And the mirror of it is below half.
  EXPECT_LT(easeProgress(0.5f, Point {0.42f, 0.0f}, Point {1.0f, 1.0f}), 0.5f);
}

TEST(LottieEase, IsMonotone) {
  // A timing curve that goes backwards would make a value stutter, and it is what a solve that takes a wrong branch
  // produces. Sampling densely is the only way to see it.
  Scalar previous = -1.0f;
  for (int i = 0; i <= 100; ++i) {
    const Scalar t = static_cast<Scalar>(i) / 100.0f;
    const Scalar value = easeProgress(t, kEaseIn, kEaseInEnd);
    EXPECT_GE(value, previous - 1e-4f) << "at t = " << t;
    previous = value;
  }
}

TEST(LottieEase, HandlesANearlyVerticalCurve) {
  // A handle dragged almost straight up makes x(s) nearly flat, so Newton's step is nearly a division by zero. The
  // fallback is what keeps this finite and inside [0, 1].
  const Point out {0.99f, 0.0f};
  const Point in {0.99f, 0.01f};
  for (int i = 0; i <= 20; ++i) {
    const Scalar value = easeProgress(static_cast<Scalar>(i) / 20.0f, out, in);
    EXPECT_TRUE(std::isfinite(value)) << "at i = " << i;
    EXPECT_GE(value, 0.0f);
    EXPECT_LE(value, 1.0f);
  }
}

// ==================================================================================================
// A constant property
// ==================================================================================================

TEST(LottieAnimatable, AConstantAnswersTheSameValueAtEveryTime) {
  const auto value = Animatable<Scalar>::constant(7.5f);
  EXPECT_FALSE(value.isAnimated());
  for (const Scalar t : {-100.0f, 0.0f, 5.0f, 1e6f}) {
    EXPECT_FLOAT_EQ(value.at(t), 7.5f) << "at t = " << t;
  }
}

TEST(LottieAnimatable, AnEmptyPropertyAnswersADefault) {
  const Animatable<Scalar> empty;
  EXPECT_TRUE(empty.isEmpty());
  EXPECT_FLOAT_EQ(empty.at(0), 0.0f);
}

// ==================================================================================================
// Keyframes
// ==================================================================================================

TEST(LottieAnimatable, InterpolatesBetweenTwoKeyframes) {
  const auto value = Animatable<Scalar>::animated({
    Keyframe<Scalar> {0, 0.0f, std::nullopt, kLinearOut, kLinearIn, false},
    Keyframe<Scalar> {10, 100.0f, std::nullopt, kLinearOut, kLinearIn, false},
  });
  EXPECT_TRUE(value.isAnimated());
  EXPECT_FLOAT_EQ(value.at(0), 0.0f);
  EXPECT_FLOAT_EQ(value.at(5), 50.0f);
  EXPECT_FLOAT_EQ(value.at(10), 100.0f);
}

TEST(LottieAnimatable, HoldsTheEndsOutsideTheKeyframeRange) {
  // A document that starts its keyframes at frame 10 means "this value until then", not "carry on backwards".
  const auto value = Animatable<Scalar>::animated({
    Keyframe<Scalar> {10, 5.0f, std::nullopt, kLinearOut, kLinearIn, false},
    Keyframe<Scalar> {20, 15.0f, std::nullopt, kLinearOut, kLinearIn, false},
  });
  EXPECT_FLOAT_EQ(value.at(0), 5.0f);
  EXPECT_FLOAT_EQ(value.at(9.999f), 5.0f);
  EXPECT_FLOAT_EQ(value.at(20), 15.0f);
  EXPECT_FLOAT_EQ(value.at(1000), 15.0f);
}

TEST(LottieAnimatable, PicksTheRightSegmentOfMany) {
  const auto value = Animatable<Scalar>::animated({
    Keyframe<Scalar> {0, 0.0f, std::nullopt, kLinearOut, kLinearIn, false},
    Keyframe<Scalar> {10, 10.0f, std::nullopt, kLinearOut, kLinearIn, false},
    Keyframe<Scalar> {20, 30.0f, std::nullopt, kLinearOut, kLinearIn, false},
    Keyframe<Scalar> {30, 40.0f, std::nullopt, kLinearOut, kLinearIn, false},
  });
  EXPECT_FLOAT_EQ(value.at(5), 5.0f) << "the first segment";
  EXPECT_FLOAT_EQ(value.at(15), 20.0f) << "the second, which does not start where the first ended";
  EXPECT_FLOAT_EQ(value.at(25), 35.0f) << "the third";
  EXPECT_FLOAT_EQ(value.at(9.5f), 9.5f);
}

TEST(LottieAnimatable, SortsKeyframesWrittenOutOfOrder) {
  // Lottie does not promise an order. Every lookup above assumes sorted, so getting this wrong makes a document with its
  // keyframes shuffled animate to the wrong place rather than not at all.
  const auto value = Animatable<Scalar>::animated({
    Keyframe<Scalar> {20, 30.0f, std::nullopt, kLinearOut, kLinearIn, false},
    Keyframe<Scalar> {0, 0.0f, std::nullopt, kLinearOut, kLinearIn, false},
    Keyframe<Scalar> {10, 10.0f, std::nullopt, kLinearOut, kLinearIn, false},
  });
  EXPECT_FLOAT_EQ(value.at(0), 0.0f);
  EXPECT_FLOAT_EQ(value.at(5), 5.0f);
  EXPECT_FLOAT_EQ(value.at(15), 20.0f);
}

TEST(LottieAnimatable, APreferredEndValueWinsOverTheNextKeyframe) {
  // Lottie's `e`, which older exporters always wrote. When it disagrees with the next keyframe's `s`, the segment's own
  // end is what the document said, and the value then jumps at the next keyframe -- which is what the format means.
  const auto value = Animatable<Scalar>::animated({
    Keyframe<Scalar> {0, 0.0f, 100.0f, kLinearOut, kLinearIn, false},
    Keyframe<Scalar> {10, 0.0f, std::nullopt, kLinearOut, kLinearIn, false},
  });
  EXPECT_FLOAT_EQ(value.at(5), 50.0f) << "halfway to the segment's own end";
  EXPECT_FLOAT_EQ(value.at(10), 0.0f) << "and the next keyframe is where it says it is";
}

TEST(LottieAnimatable, AHoldKeyframeDoesNotMove) {
  const auto value = Animatable<Scalar>::animated({
    Keyframe<Scalar> {0, 5.0f, std::nullopt, kLinearOut, kLinearIn, true},
    Keyframe<Scalar> {10, 50.0f, std::nullopt, kLinearOut, kLinearIn, false},
  });
  EXPECT_FLOAT_EQ(value.at(0), 5.0f);
  EXPECT_FLOAT_EQ(value.at(9.99f), 5.0f) << "still held";
  EXPECT_FLOAT_EQ(value.at(10), 50.0f) << "and jumps at the next keyframe";
}

TEST(LottieAnimatable, TwoKeyframesAtTheSameTimeAreAJump) {
  // Not a division by zero. Taking the later value is what the document's own ordering means.
  const auto value = Animatable<Scalar>::animated({
    Keyframe<Scalar> {0, 1.0f, std::nullopt, kLinearOut, kLinearIn, false},
    Keyframe<Scalar> {0, 2.0f, std::nullopt, kLinearOut, kLinearIn, false},
    Keyframe<Scalar> {10, 3.0f, std::nullopt, kLinearOut, kLinearIn, false},
  });
  EXPECT_FLOAT_EQ(value.at(0), 2.0f);
  EXPECT_FLOAT_EQ(value.at(5), 2.5f);
}

TEST(LottieAnimatable, AppliesTheEasingToTheChangeNotTheTime) {
  // An ease-in over a segment from 0 to 100, asked a quarter of the way through its time, gives a quarter of the way
  // through the *eased* fraction: about 9.3, not 25.
  const auto value = Animatable<Scalar>::animated({
    Keyframe<Scalar> {0, 0.0f, std::nullopt, kEaseIn, kEaseInEnd, false},
    Keyframe<Scalar> {100, 100.0f, std::nullopt, kLinearOut, kLinearIn, false},
  });
  EXPECT_NEAR(value.at(25), 9.3f, 1.0f);
  EXPECT_LT(value.at(25), 15.0f) << "not the uneased quarter";
}

// ==================================================================================================
// Types other than a number
// ==================================================================================================

TEST(LottieAnimatable, InterpolatesAPosition) {
  const auto value = Animatable<Point>::animated({
    Keyframe<Point> {0, Point {0, 10}, std::nullopt, kLinearOut, kLinearIn, false},
    Keyframe<Point> {10, Point {100, 30}, std::nullopt, kLinearOut, kLinearIn, false},
  });
  const Point middle = value.at(5);
  EXPECT_FLOAT_EQ(middle.x, 50.0f);
  EXPECT_FLOAT_EQ(middle.y, 20.0f);
}

TEST(LottieAnimatable, InterpolatesAColourWithoutDarkeningAFade) {
  // Channel by channel in the values as written, so a fade to transparent stays the same colour all the way. A
  // premultiplied interpolation would darken the middle of it, which is visible in every fading layer.
  const auto value = Animatable<Color>::animated({
    Keyframe<Color> {0, Color::MakeRGBA8(255, 0, 0, 255), std::nullopt, kLinearOut, kLinearIn, false},
    Keyframe<Color> {10, Color::MakeRGBA8(255, 0, 0, 0), std::nullopt, kLinearOut, kLinearIn, false},
  });
  const Color middle = value.at(5);
  EXPECT_NEAR(middle.red, 1.0f, 0.01f) << "the red is unchanged";
  EXPECT_NEAR(middle.green, 0.0f, 0.01f);
  EXPECT_NEAR(middle.alpha, 0.5f, 0.01f) << "and only the alpha moves";
}

TEST(LottieAnimatable, InterpolatesAVectorOfNumbers) {
  // How Lottie writes a scale, a 3D position, or a colour's components.
  const auto value = Animatable<std::vector<Scalar>>::animated({
    Keyframe<std::vector<Scalar>> {0, {0.0f, 10.0f, 100.0f}, std::nullopt, kLinearOut, kLinearIn, false},
    Keyframe<std::vector<Scalar>> {10, {10.0f, 20.0f, 0.0f}, std::nullopt, kLinearOut, kLinearIn, false},
  });
  const std::vector<Scalar> middle = value.at(5);
  ASSERT_EQ(middle.size(), 3U);
  EXPECT_FLOAT_EQ(middle[0], 5.0f);
  EXPECT_FLOAT_EQ(middle[1], 15.0f);
  EXPECT_FLOAT_EQ(middle[2], 50.0f);
}

TEST(LottieAnimatable, AVectorLengthMismatchDoesNotInventComponents) {
  // A malformed document. Taking the shorter is the safe reading: the extra components have no counterpart to move
  // towards, and padding with zeros would animate a value to the origin.
  const auto value = Animatable<std::vector<Scalar>>::animated({
    Keyframe<std::vector<Scalar>> {0, {1.0f, 2.0f, 3.0f}, std::nullopt, kLinearOut, kLinearIn, false},
    Keyframe<std::vector<Scalar>> {10, {5.0f, 6.0f}, std::nullopt, kLinearOut, kLinearIn, false},
  });
  EXPECT_EQ(value.at(5).size(), 2U);
}

TEST(LottieAnimatable, AOneKeyframePropertyIsNotAnimated) {
  // A document with `a: 1` and a single keyframe is a property that does not move, and asking it to interpolate towards
  // nothing would divide by a span of zero.
  const auto value = Animatable<Scalar>::animated({
    Keyframe<Scalar> {0, 42.0f, std::nullopt, kLinearOut, kLinearIn, false},
  });
  EXPECT_FALSE(value.isAnimated());
  EXPECT_FLOAT_EQ(value.at(500), 42.0f);
}
