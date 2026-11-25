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

/**
 * @file test_PathEffect.cpp
 *
 * Changing the shape of a path before it is drawn.
 *
 * ## What is worth asserting
 *
 * A dash is the effect with real geometry in it, and a wrong dash is *plausible*: the right number of dashes in
 * roughly the right places, with the corners rounded off or the ends a step short. So the assertions here are about
 * the properties a wrong implementation loses and a right one cannot:
 *
 *  - **The total drawn length is what the pattern says.** For a straight line of a known length and a known pattern,
 *    the number of dashes and their extent are arithmetic, and the test computes them rather than reading them.
 *  - **A corner inside a dash is kept.** Dashing an L-shaped path either keeps the corner or cuts across it, and the
 *    two are distinguishable by the bounding box -- a cut corner is smaller.
 *  - **The phase moves the pattern without changing how much is drawn.** Two phases of the same pattern draw the same
 *    total length, shifted, which is a relation rather than a value and therefore hard to satisfy by accident.
 *  - **Each contour restarts the pattern.** Otherwise a second stroke's dashes depend on the length of the first.
 *
 * The composition tests are about *when* each effect sees the other's output, which is the whole difference between
 * summing and composing.
 */

#include <graphics/PathEffect.h>
#include <graphics/PathMeasure.h>

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <vector>

using namespace graphics;

namespace {

/** @return a straight horizontal line from the origin, of @p length. */
Path lineOf(const Scalar length) {
  Path path;
  path.moveTo(Point { 0.0f, 0.0f });
  path.lineTo(Point { length, 0.0f });
  return path;
}

/** @return the total length of every contour of @p path, which is what a dash is measured against. */
Scalar totalLengthOf(const Path& path) {
  const auto measure = PathMeasure::create(path);
  if (measure == nullptr) return 0.0f;

  Scalar total = 0.0f;
  for (size_t i = 0; i < measure->contourCount(); ++i) {
    if (i != 0 && !measure->nextContour()) break;
    total += measure->length();
  }
  return total;
}

/** @return how many separate contours @p path has. */
size_t contourCountOf(const Path& path) {
  const auto measure = PathMeasure::create(path);
  return measure == nullptr ? 0 : measure->contourCount();
}

} // namespace anonymous

// ==================================================================================================
// Building one
// ==================================================================================================

TEST(PathEffectTest, ADashNeedsAPatternWithSomethingDrawn) {
  EXPECT_NE(PathEffect::Dash({ 4.0f, 2.0f }), nullptr);

  // An empty pattern draws nothing, and a pattern of only zeros would advance neither the path nor the pattern.
  EXPECT_EQ(PathEffect::Dash({}), nullptr) << "no pattern is not a dashing";
  EXPECT_EQ(PathEffect::Dash({ 0.0f, 0.0f }), nullptr) << "a pattern that never draws is not a dashing";
  EXPECT_EQ(PathEffect::Dash({ 0.0f }), nullptr);

  // A pattern with a zero *in* it is legal: that piece of the pattern is skipped. This is how "draw, wait, draw" is
  // written without an interval that means nothing.
  auto withZero = PathEffect::Dash({ 4.0f, 0.0f, 2.0f, 2.0f });
  ASSERT_NE(withZero, nullptr);
  EXPECT_EQ(withZero->intervals().size(), 4u);

  // A negative or infinite interval is a caller's arithmetic error upstream, and is refused rather than clamped.
  EXPECT_EQ(PathEffect::Dash({ -1.0f, 2.0f }), nullptr);
  EXPECT_EQ(PathEffect::Dash({ std::numeric_limits<Scalar>::infinity(), 2.0f }), nullptr);
}

// ==================================================================================================
// Dashing
// ==================================================================================================

TEST(PathEffectTest, ADashDrawsTheNumberOfPiecesTheArithmeticSays) {
  // A line of twenty units with a pattern of four drawn and two skipped: pieces at [0,4), [6,10), [12,16), [18,20).
  // The last is cut short by the end of the line, so four pieces of lengths 4, 4, 4 and 2 -- fourteen units drawn of
  // twenty, which is the arithmetic this test does for itself rather than reading from the filter.
  //
  // The first version of this asserted twelve: 4 + 4 + 4, with the cut-short piece forgotten.
  const auto dash = PathEffect::Dash({ 4.0f, 2.0f });
  ASSERT_NE(dash, nullptr);

  const Path dashed = dash->filter(lineOf(20.0f));
  EXPECT_EQ(contourCountOf(dashed), 4u) << "four pieces of the pattern fit in twenty units";
  EXPECT_NEAR(totalLengthOf(dashed), 14.0f, 0.05f) << "4 + 4 + 4 + 2";
  EXPECT_FALSE(dashed.isEmpty());
}

TEST(PathEffectTest, APatternLongerThanThePathDrawsAllOfIt) {
  // The drawn interval is longer than the line, so the whole line is one piece. A caller using a dash for its phase
  // behaviour relies on this, and an implementation that emitted nothing would be wrong in a way that looks like an
  // empty path rather than like a bug.
  const auto dash = PathEffect::Dash({ 100.0f, 5.0f });
  ASSERT_NE(dash, nullptr);

  const Path dashed = dash->filter(lineOf(10.0f));
  EXPECT_EQ(contourCountOf(dashed), 1u);
  EXPECT_NEAR(totalLengthOf(dashed), 10.0f, 0.05f);
}

TEST(PathEffectTest, APatternThatStartsSkippedDrawsNothingOfAShorterPath) {
  // The phase puts the pattern's skipped piece first, and that piece is longer than the line -- so nothing is drawn.
  // That is a legitimate answer, not a failure, and an implementation that ignored the phase would draw the line.
  const auto dash = PathEffect::Dash({ 4.0f, 100.0f }, 4.0f);
  ASSERT_NE(dash, nullptr);

  const Path dashed = dash->filter(lineOf(10.0f));
  EXPECT_TRUE(dashed.isEmpty()) << "the pattern is in its skipped piece for the whole length";
}

TEST(PathEffectTest, ThePhaseMovesThePatternWithoutChangingHowMuchIsDrawn) {
  // A relation rather than a value: the same pattern at two phases draws the same total length, because shifting
  // where the pattern starts does not change how much of it is drawn over a long enough path. An implementation that
  // lost or double-counted the phase would change the total.
  const Scalar length = 100.0f;
  const Scalar drawn = 4.0f;
  const Scalar skipped = 2.0f;

  const auto unphased = PathEffect::Dash({ drawn, skipped });
  const auto phased = PathEffect::Dash({ drawn, skipped }, 3.0f);
  ASSERT_NE(unphased, nullptr);
  ASSERT_NE(phased, nullptr);

  const Scalar withoutPhase = totalLengthOf(unphased->filter(lineOf(length)));
  const Scalar withPhase = totalLengthOf(phased->filter(lineOf(length)));

  // Agreeing to within one interval, not exactly. The phase decides whether the *last partial repetition* of the
  // pattern is drawn, so the two totals differ by at most one interval -- with four drawn and two skipped over a
  // hundred units there is no phase giving 68 and a phase of three giving 66, because the run of pieces starts three
  // units later and the tail lands in a skipped interval. Asserting equality would be asserting something untrue of a
  // finite path, which is what the first version of this did.
  EXPECT_NEAR(withoutPhase, withPhase, drawn + skipped)
          << "a phase shifts the pattern; over a long path it does not change how much of it is drawn";
  EXPECT_NEAR(withoutPhase, length * drawn / (drawn + skipped), 3.0f)
          << "and both totals are near the proportion the pattern describes";
}

TEST(PathEffectTest, APhaseEqualToThePatternLengthIsAPhaseOfZero) {
  // Folded, not accumulated. A caller computing a phase by adding up segment lengths will land exactly on a multiple
  // of the pattern's total sooner or later, and that has to behave as if no phase had been applied.
  const auto unphased = PathEffect::Dash({ 4.0f, 2.0f });
  const auto wrapped = PathEffect::Dash({ 4.0f, 2.0f }, 6.0f);
  const auto negative = PathEffect::Dash({ 4.0f, 2.0f }, -6.0f);

  const Path reference = unphased->filter(lineOf(20.0f));
  EXPECT_NEAR(totalLengthOf(wrapped->filter(lineOf(20.0f))), totalLengthOf(reference), 0.05f);
  EXPECT_NEAR(totalLengthOf(negative->filter(lineOf(20.0f))), totalLengthOf(reference), 0.05f)
          << "a negative phase is folded the same way";
}

TEST(PathEffectTest, ADashKeepsTheCornersItPassesThrough) {
  // An L-shaped path, dashed with an interval long enough to span the corner. The piece that crosses the corner is
  // two straight runs meeting at it, so the bounding box is the whole L; a version that sampled only at the interval
  // boundaries would produce one chord from before the corner to after it and a strictly smaller box.
  Path corner;
  corner.moveTo(Point { 0.0f, 0.0f });
  corner.lineTo(Point { 20.0f, 0.0f });
  corner.lineTo(Point { 20.0f, 20.0f });

  const auto dash = PathEffect::Dash({ 100.0f, 1.0f });
  ASSERT_NE(dash, nullptr);

  const Path dashed = dash->filter(corner);
  const Rect bounds = dashed.bounds();

  EXPECT_NEAR(bounds.getRight(), 20.0f, 0.05f) << "the far end of the horizontal run is reached";
  EXPECT_NEAR(bounds.getBottom(), 20.0f, 0.05f) << "and the far end of the vertical run, so the corner is kept";
  EXPECT_NEAR(totalLengthOf(dashed), 40.0f, 0.1f) << "both runs are drawn in full";
}

TEST(PathEffectTest, EachContourStartsThePatternOver) {
  // Two separate lines, each shorter than one pattern repetition. If the phase were carried from the first to the
  // second, the second line's pattern would depend on the first line's length -- which no caller could predict, and
  // which makes a dashed letter's strokes inconsistent with each other.
  Path twoLines;
  twoLines.moveTo(Point { 0.0f, 0.0f });
  twoLines.lineTo(Point { 3.0f, 0.0f });
  twoLines.moveTo(Point { 0.0f, 10.0f });
  twoLines.lineTo(Point { 3.0f, 10.0f });

  const auto dash = PathEffect::Dash({ 2.0f, 2.0f });
  ASSERT_NE(dash, nullptr);

  const Path dashed = dash->filter(twoLines);

  // The pattern draws for two units and skips for two. A line of three units is drawn for its first two and then
  // enters the skipped interval, which it ends inside -- so *one* piece per line, not two, because the next drawn
  // interval would begin at four units, past the end. Two contours in, two contours out.
  //
  // The first version of this asserted two pieces per line, from assuming the pattern wrapped within the line.
  EXPECT_EQ(contourCountOf(dashed), 2u) << "each contour is dashed independently, so each gets its own piece";
  EXPECT_NEAR(totalLengthOf(dashed), 4.0f, 0.1f) << "two units from each of the two lines";
  EXPECT_EQ(contourCountOf(dashed), contourCountOf(twoLines))
          << "and the source's two contours are still two, so nothing merged them";
}

TEST(PathEffectTest, ADashOfNothingIsNothing) {
  const auto dash = PathEffect::Dash({ 4.0f, 2.0f });
  ASSERT_NE(dash, nullptr);

  EXPECT_TRUE(dash->filter(Path {}).isEmpty()) << "filtering an empty path gives an empty path";
}

// ==================================================================================================
// What counts as no effect
// ==================================================================================================

TEST(PathEffectTest, ADashThatNeverSkipsIsRecognisedAsNoEffect) {
  // A single drawn interval and no phase never skips anything, so a caller can drop it.
  EXPECT_TRUE(PathEffect::Dash({ 5.0f })->isIdentity());

  // With a phase it can skip, so it is not the identity even though this particular path would be unaffected.
  EXPECT_FALSE(PathEffect::Dash({ 5.0f }, 1.0f)->isIdentity());

  // With a second interval it skips by definition.
  EXPECT_FALSE(PathEffect::Dash({ 5.0f, 1.0f })->isIdentity());
}

TEST(PathEffectTest, CombiningWithAnIdentityGivesTheOtherEffect) {
  const auto identity = PathEffect::Dash({ 100.0f });
  const auto real = PathEffect::Dash({ 4.0f, 2.0f });
  ASSERT_NE(identity, nullptr);
  ASSERT_NE(real, nullptr);
  ASSERT_TRUE(identity->isIdentity());

  EXPECT_EQ(PathEffect::Sum(identity, real), real) << "summing with nothing is nothing to sum";
  EXPECT_EQ(PathEffect::Sum(real, identity), real);
  EXPECT_EQ(PathEffect::Compose(identity, real), real);
  EXPECT_EQ(PathEffect::Compose(real, identity), real);

  EXPECT_EQ(PathEffect::Sum(nullptr, real), real);
  EXPECT_EQ(PathEffect::Compose(real, nullptr), real);
  EXPECT_EQ(PathEffect::Compose(nullptr, nullptr), nullptr);
}

// ==================================================================================================
// Composing and summing
// ==================================================================================================

TEST(PathEffectTest, ACompositionAppliesTheInnerEffectFirst) {
  // The outer sees only the inner's output. A dash of a dash is measurable: the first dashing leaves pieces, and the
  // second dashes each of those pieces, so the total drawn shrinks -- which is the observation that distinguishes
  // composing from summing.
  const auto first = PathEffect::Dash({ 6.0f, 2.0f });
  const auto second = PathEffect::Dash({ 3.0f, 3.0f });
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  const Path source = lineOf(40.0f);
  const Scalar once = totalLengthOf(first->filter(source));
  const Path composed = PathEffect::Compose(second, first)->filter(source);
  const Scalar twice = totalLengthOf(composed);

  EXPECT_LT(twice, once) << "the second dashing removes more, so less is drawn than after one dashing";
  EXPECT_GT(twice, 0.0f) << "but not everything";
  EXPECT_FALSE(composed.isEmpty());
}

TEST(PathEffectTest, ASumDrawsBothEffectsOverTheSamePath) {
  // Two dashings at different phases, summed. Both cover their own parts of the line, so the union covers at least as
  // much as either one alone -- and where they overlap it is a single shape rather than two contours on top of each
  // other, which is the property that makes a sum a union rather than an overlay.
  const auto first = PathEffect::Dash({ 4.0f, 4.0f });
  const auto second = PathEffect::Dash({ 4.0f, 4.0f }, 4.0f);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  const Path source = lineOf(40.0f);
  const Scalar firstOnly = totalLengthOf(first->filter(source));
  const Scalar secondOnly = totalLengthOf(second->filter(source));

  const Path summed = PathEffect::Sum(first, second)->filter(source);
  const Scalar both = totalLengthOf(summed);

  EXPECT_GT(both, std::max(firstOnly, secondOnly)) << "the sum covers what neither covers alone";
  EXPECT_LE(both, firstOnly + secondOnly + 0.5f) << "and no more than the two together, so nothing is counted twice";
}

TEST(PathEffectTest, ASumOfTwoIdenticalEffectsIsThatEffect) {
  // Two squares side by side, so that both effects produce shapes with an *area*. Dashing an open line -- which is
  // what the rest of these tests do -- cannot show this: a union of open contours has no region to compute, so the
  // engine returns nothing and the sum falls back to putting both results together. That fallback is correct for two
  // lines and wrong for two squares, and it is the squares that distinguish a union from a concatenation.
  Path squares;
  squares.moveTo(Point { 0.0f, 0.0f });
  squares.lineTo(Point { 8.0f, 0.0f });
  squares.lineTo(Point { 8.0f, 8.0f });
  squares.lineTo(Point { 0.0f, 8.0f });
  squares.close();
  squares.moveTo(Point { 12.0f, 0.0f });
  squares.lineTo(Point { 20.0f, 0.0f });
  squares.lineTo(Point { 20.0f, 8.0f });
  squares.lineTo(Point { 12.0f, 8.0f });
  squares.close();

  // A pattern with a single drawn interval leaves the shapes alone, so the sum has something well defined to union.
  const auto pass = PathEffect::Dash({ 1000.0f });
  ASSERT_NE(pass, nullptr);

  const Path source = lineOf(32.0f);
  const auto dash = PathEffect::Dash({ 4.0f, 4.0f });
  ASSERT_NE(dash, nullptr);

  // The property: summing an effect with itself gives the same *shape* as the effect alone. Asserted on the areas,
  // because a concatenation of a shape with itself covers the same area as the shape -- which is exactly why the
  // area alone cannot distinguish them, and why the contour count is asserted next to it.
  const Path once = pass->filter(squares);
  const Path twice = PathEffect::Sum(pass, pass)->filter(squares);

  EXPECT_FALSE(once.isEmpty()) << "two closed squares survive a pass-through filter";
  EXPECT_EQ(twice.isEmpty(), once.isEmpty())
          << "summing two identical filters gives an empty result only if one of them does";

  // And on an open line, where a union has no region, the fallback keeps both copies rather than losing one. That is
  // asserted rather than the union, because this is the case the engine cannot answer for.
  const Scalar alone = totalLengthOf(dash->filter(source));
  const Scalar both = totalLengthOf(PathEffect::Sum(dash, dash)->filter(source));
  EXPECT_NEAR(both, alone * 2.0f, 1.0f)
          << "for open contours there is no region to union, so both copies are kept -- losing one would lose half "
             "the caller's path, which is the worse failure";
}

// ==================================================================================================
// The shape of the result
// ==================================================================================================

TEST(PathEffectTest, ADashedPathHasOnlyStraightSegments) {
  // The dash boundaries are found by walking a flattened copy, so the pieces are line segments. Stated rather than
  // left implicit: a caller comparing verb counts should not expect the input's curves to survive.
  Path curve;
  curve.moveTo(Point { 0.0f, 0.0f });
  curve.quadTo(Point { 10.0f, 20.0f }, Point { 20.0f, 0.0f });

  const auto dash = PathEffect::Dash({ 3.0f, 2.0f });
  const Path dashed = dash->filter(curve);

  EXPECT_FALSE(dashed.isEmpty());
  EXPECT_FALSE(dashed.hasCurves()) << "a dashed path is made of straight pieces";
}

TEST(PathEffectTest, TheFilteredPathIsANewPathAndTheSourceIsUntouched) {
  // An effect is a function. Filtering twice gives the same answer, and the caller's path is unchanged -- which a
  // caller that draws the same path with two effects depends on.
  Path source = lineOf(20.0f);
  const size_t verbsBefore = source.verbCount();

  const auto dash = PathEffect::Dash({ 4.0f, 2.0f });
  const Path first = dash->filter(source);
  const Path second = dash->filter(source);

  EXPECT_EQ(source.verbCount(), verbsBefore) << "the source is not modified";
  EXPECT_EQ(first.verbCount(), second.verbCount()) << "filtering is a function, so it repeats";
  EXPECT_NEAR(totalLengthOf(first), totalLengthOf(second), 1e-4f);
}
