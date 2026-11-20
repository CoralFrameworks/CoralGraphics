/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License is distributed on
 * an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 */

/**
 * @file test_Arc.cpp
 *
 * `Arc::computeIterations`, which decides how an arc is walked one quadrant at a time.
 *
 * ## Why this file was rewritten rather than restored
 *
 * It referenced `graphics::Tessellator::Trigs`, a type that no longer exists: the design it was written against passed
 * in a table of precomputed sine and cosine values, and the current one takes a quadrant step count. So the file had not
 * compiled for some time, and it was never listed in `CMakeLists.txt` -- which is why nobody noticed. It was found by
 * reading the README, which claimed the file covered arcs.
 *
 * The consequence is worth stating plainly: **`Arc::computeIterations` has never been executed by any test.** Its only
 * caller anywhere was this file. The tests below are therefore the first thing to run it, and they were written from
 * observed behaviour rather than from the old expectations, because the old expectations describe a different function.
 *
 * ## What the iteration describes
 *
 * An arc is walked quadrant by quadrant. Each `Quadrant` names the axis its angles belong to and a half-open range of
 * indices into the *caller's* sine table for that axis; the caller multiplies `axis` by the table entry. So the axis is
 * chosen here and the trigonometry is the caller's, which is what keeps this free of any particular table's resolution.
 *
 * The `+1` on the first quadrant's start index for a full circle is the convention this file pins: the caller's table
 * has its 0 and its maximum at the same angle, so starting at 0 would emit that direction twice.
 */

#include <geometry/Arc.h>
#include <geometry/Matrix.h>
#include <geometry/Rect.h>

#include "test_asserts.h"

#include <gtest/gtest.h>

#include <cstddef>

namespace graphics {
namespace {

/** A square oval, so an arc's angles and its points stay in the same proportion. */
Rect squareBounds() { return Rect::MakeLTRB(10.0f, 10.0f, 20.0f, 20.0f); }

constexpr size_t kSteps = 4u;

TEST(ArcTest, FullCircleWalksAllFourQuadrantsFromThePositiveXAxis) {
  // A sweep of at least a full turn is a circle, whatever angle it claims to start at -- the start is dropped rather
  // than honoured, because a caller asking for 360 degrees has asked for something with no start.
  for (const float start : { 0.0f, 30.0f, 90.0f, 200.0f, 359.0f, -90.0f }) {
    const Arc arc(squareBounds(), Degrees(start), Degrees(360.0f), false);
    const auto iteration = arc.computeIterations(kSteps);

    EXPECT_EQ(iteration.quadrant_count, 4u) << "start " << start;
    EXPECT_POINT_NEAR(iteration.start, Vector2(1.0f, 0.0f)) << "start " << start;
    EXPECT_POINT_NEAR(iteration.end, Vector2(1.0f, 0.0f)) << "start " << start;

    // The four axes in turn. This is the whole of the ordering rule: an arc is walked counter-clockwise from the
    // positive x axis, so the quadrants are the four unit directions in that order.
    EXPECT_POINT_NEAR(iteration.quadrants[0].axis, Vector2(1.0f, 0.0f)) << "start " << start;
    EXPECT_POINT_NEAR(iteration.quadrants[1].axis, Vector2(0.0f, 1.0f)) << "start " << start;
    EXPECT_POINT_NEAR(iteration.quadrants[2].axis, Vector2(-1.0f, 0.0f)) << "start " << start;
    EXPECT_POINT_NEAR(iteration.quadrants[3].axis, Vector2(0.0f, -1.0f)) << "start " << start;

    // The first quadrant starts one index in, the rest start at zero. See the note at the top of the file: index 0 and
    // index `stepCount` name the same direction, so starting the first at 0 would walk that direction twice.
    EXPECT_EQ(iteration.quadrants[0].start_index, 1u) << "start " << start;
    EXPECT_EQ(iteration.quadrants[1].start_index, 0u) << "start " << start;
    EXPECT_EQ(iteration.quadrants[2].start_index, 0u) << "start " << start;
    EXPECT_EQ(iteration.quadrants[3].start_index, 0u) << "start " << start;

    for (size_t i = 0; i < 4u; ++i) {
      EXPECT_EQ(iteration.quadrants[i].end_index, kSteps) << "quadrant " << i << ", start " << start;
    }

    // Two endpoints plus the interior points of every quadrant. Four quadrants contributing `kSteps - 1` each where the
    // first contributes one fewer, plus the two ends: (3 + 4 + 4 + 4) + 2 = 17.
    EXPECT_EQ(iteration.getPointCount(), 17u) << "start " << start;
  }
}

TEST(ArcTest, AFullTurnIsSimplifiedButMoreThanOneIsNot) {
  // `simplify_360` decides whether a sweep of a full turn is treated as *the* circle -- start 0, four whole quadrants --
  // or as an arc that happens to sweep 360 degrees from wherever it starts. Both are defensible, so which one happens
  // is pinned rather than left to whichever branch a reader assumes.
  const Arc arc(squareBounds(), Degrees(45.0f), Degrees(360.0f), false);

  const auto simplified = arc.computeIterations(kSteps, true);
  EXPECT_EQ(simplified.quadrant_count, 4u);
  EXPECT_POINT_NEAR(simplified.start, Vector2(1.0f, 0.0f)) << "simplified: the start angle is discarded";
  EXPECT_EQ(simplified.quadrants[0].start_index, 1u) << "simplified: four whole quadrants";

  const auto verbatim = arc.computeIterations(kSteps, false);
  EXPECT_POINT_NEAR(verbatim.start, Matrix::CosSin(Degrees(45.0f)))
          << "verbatim: the start angle is honoured, so the walk begins where the caller said";
  EXPECT_POINT_NEAR(verbatim.end, Matrix::CosSin(Degrees(45.0f)))
          << "verbatim: and it comes back to where it began, a full turn later";

  // Five quadrants, not four, and that is the reason the flag exists rather than an oddity. Walking from 45 degrees for
  // a full turn crosses the boundaries at 90, 180, 270, 360 and 450 -- five of them -- so the first and last entries are
  // partial: 45 to 90, and 360 to 405. Treating a full turn as *the* circle instead is what the simplified branch does,
  // and it is why that one's first index is 1: four whole quadrants beginning at a boundary, with the direction they
  // share emitted once.
  //
  // The first version of this assertion said 4 for both branches. It was a guess, the guess was wrong, and the failure
  // is what corrected it -- which is worth the two lines it takes to say so, because the number is not obvious.
  EXPECT_EQ(verbatim.quadrant_count, 5u);
  EXPECT_NE(verbatim.quadrants[0].start_index, 1u)
          << "a partial first quadrant begins at the true start, with no shared direction to skip";

  // The two branches enumerate the same *number* of points -- 17 either way -- because each is a full turn sampled at the
  // same step count. They are not the same points: the simplified walk runs from 0 degrees to 0, and the verbatim one
  // from 45 to 45 a turn later, so the vertices are a rotation apart.
  //
  // Recorded because it is the opposite of what the previous two attempts at this test assumed. The quadrant counts
  // differ (4 against 5) while the point totals do not, which is a good illustration of why `quadrant_count` is not a
  // proxy for how much work an arc is: the branches are bookkeeping around the same sampled turn.
  EXPECT_EQ(simplified.getPointCount(), verbatim.getPointCount());
  EXPECT_NE(simplified.start.x, verbatim.start.x)
          << "the same count, at a different place on the circle -- so not the same points";
}

TEST(ArcTest, AQuarterInsideOneQuadrantNeedsOneEntry) {
  // An arc that stays within a single quadrant has no interior quadrant boundary to cross, so it is one entry whose
  // range covers only the part of the quadrant it spans.
  const Arc arc(squareBounds(), Degrees(0.0f), Degrees(90.0f), false);
  const auto iteration = arc.computeIterations(kSteps);

  EXPECT_EQ(iteration.quadrant_count, 1u);
  EXPECT_POINT_NEAR(iteration.start, Vector2(1.0f, 0.0f));
  EXPECT_POINT_NEAR(iteration.end, Vector2(0.0f, 1.0f));
  EXPECT_POINT_NEAR(iteration.quadrants[0].axis, Vector2(1.0f, 0.0f));

  // The start is nudged a tenth of a step past the true start, which is what keeps a degenerate first step from being
  // emitted -- so the first index is 1 rather than 0, the same reason as the full circle's.
  EXPECT_EQ(iteration.quadrants[0].start_index, 1u);
  EXPECT_EQ(iteration.quadrants[0].end_index, kSteps);

  // Two ends, plus what the single quadrant contributes.
  EXPECT_EQ(iteration.getPointCount(), 2u + iteration.quadrants[0].getPointCount());
}

TEST(ArcTest, TheSecondQuadrantUsesThePositiveYAxis) {
  // The axis is chosen from the quadrant the arc *starts* in, which is the part a reader is most likely to get wrong:
  // 90 to 180 degrees is the second quadrant, whose axis is +y, not +x.
  const Arc arc(squareBounds(), Degrees(90.0f), Degrees(90.0f), false);
  const auto iteration = arc.computeIterations(kSteps);

  EXPECT_EQ(iteration.quadrant_count, 1u);
  EXPECT_POINT_NEAR(iteration.quadrants[0].axis, Vector2(0.0f, 1.0f));
  EXPECT_POINT_NEAR(iteration.start, Matrix::CosSin(Degrees(90.0f)));
  EXPECT_POINT_NEAR(iteration.end, Matrix::CosSin(Degrees(180.0f)));
}

TEST(ArcTest, AnArcCrossingAQuadrantBoundarySplitsIntoTwo) {
  // 45 to 135 degrees crosses the boundary at 90. The first entry covers from the start to the end of the first
  // quadrant, the second from the start of the second quadrant to the end angle -- which is why the first ends at
  // `kSteps` and the second begins at 0.
  const Arc arc(squareBounds(), Degrees(45.0f), Degrees(90.0f), false);
  const auto iteration = arc.computeIterations(kSteps);

  ASSERT_EQ(iteration.quadrant_count, 2u);
  EXPECT_POINT_NEAR(iteration.quadrants[0].axis, Vector2(1.0f, 0.0f));
  EXPECT_POINT_NEAR(iteration.quadrants[1].axis, Vector2(0.0f, 1.0f));

  EXPECT_EQ(iteration.quadrants[0].end_index, kSteps) << "the first runs to the end of its quadrant";
  EXPECT_EQ(iteration.quadrants[1].start_index, 0u) << "the second begins at the start of its own";

  // Both ranges are non-empty and neither overlaps the other, which is the invariant that makes the walk monotonic.
  for (size_t i = 0; i < iteration.quadrant_count; ++i) {
    EXPECT_LT(iteration.quadrants[i].start_index, iteration.quadrants[i].end_index)
            << "quadrant " << i << " describes an empty range";
  }

  EXPECT_POINT_NEAR(iteration.start, Matrix::CosSin(Degrees(45.0f)));
  EXPECT_POINT_NEAR(iteration.end, Matrix::CosSin(Degrees(135.0f)));
}

TEST(ArcTest, AnArcShorterThanTheFirstStepHasNoQuadrants) {
  // The start and end are nudged a tenth of a step inwards, which is what stops a degenerate step being emitted at each
  // end. When the arc is shorter than two nudges there is nothing left between them, and the answer is that the walk is
  // just its two endpoints -- `quadrant_count == 0` with the true start and end still reported.
  //
  // This is the branch the old file could not exercise, because it depended on a sine table's resolution to decide
  // whether it was taken. Here the step count is the only input that matters, so a one-degree arc at four steps is
  // deterministically empty.
  const Arc arc(squareBounds(), Degrees(0.0f), Degrees(1.0f), false);
  const auto iteration = arc.computeIterations(kSteps);

  EXPECT_EQ(iteration.quadrant_count, 0u);
  EXPECT_POINT_NEAR(iteration.start, Matrix::CosSin(Degrees(0.0f)));
  EXPECT_POINT_NEAR(iteration.end, Matrix::CosSin(Degrees(1.0f)));
  EXPECT_EQ(iteration.getPointCount(), 2u) << "the two endpoints and nothing between them";
}

TEST(ArcTest, PointCountIsTheEndpointsPlusEveryQuadrantsInterior) {
  // `getPointCount` is what a caller sizes its buffer with, so it has to equal the sum of the parts rather than be
  // maintained alongside them. Stated as a relationship over several arcs rather than one expected total.
  for (const float sweep : { 30.0f, 90.0f, 120.0f, 179.0f, 181.0f, 270.0f, 359.0f }) {
    const Arc arc(squareBounds(), Degrees(10.0f), Degrees(sweep), false);
    const auto iteration = arc.computeIterations(kSteps);

    size_t expected = 2u;
    for (size_t i = 0; i < iteration.quadrant_count; ++i) {
      expected += iteration.quadrants[i].end_index - iteration.quadrants[i].start_index;
    }

    EXPECT_EQ(iteration.getPointCount(), expected) << "sweep " << sweep;
  }
}

TEST(ArcTest, ADegenerateSweepProducesNoPoints) {
  // A zero sweep is nothing, so it must not claim to be a line or a point. It does report two coincident endpoints --
  // `getPointCount` is structural and the endpoints are always there -- but they are both the origin vector, which is a
  // statement that there is no direction rather than a direction of zero length.
  //
  // Asserting the origin rather than `CosSin(0)` records what actually happens: the zero-sweep case returns the default
  // `Iteration`, whose vectors are default-constructed. Anything drawing this has to treat a zero sweep as empty, and
  // the test says so rather than implying the endpoints mean something.
  const Arc arc(squareBounds(), Degrees(0.0f), Degrees(0.0f), false);
  const auto iteration = arc.computeIterations(kSteps);

  EXPECT_EQ(iteration.quadrant_count, 0u);
  EXPECT_POINT_NEAR(iteration.start, Vector2(0.0f, 0.0f));
  EXPECT_POINT_NEAR(iteration.end, Vector2(0.0f, 0.0f));
}

TEST(ArcTest, QuadrantAxesFollowTheStartAngleAroundTheCircle) {
  // One arc per quadrant, so the axis is checked at all four rather than the two the tests above cover. The arc's own
  // start angle decides which axis its angles are read from, and getting that wrong shifts every point by a quarter
  // turn -- which produces a plausible arc in the wrong place, not an error.
  struct Case { float start; Vector2 axis; };
  const Case cases[] = {
    { 0.0f, { 1.0f, 0.0f } },
    { 90.0f, { 0.0f, 1.0f } },
    { 180.0f, { -1.0f, 0.0f } },
    { 270.0f, { 0.0f, -1.0f } },
  };

  for (const Case& testCase : cases) {
    const Arc arc(squareBounds(), Degrees(testCase.start), Degrees(45.0f), false);
    const auto iteration = arc.computeIterations(kSteps);

    ASSERT_GE(iteration.quadrant_count, 1u) << "start " << testCase.start;
    EXPECT_POINT_NEAR(iteration.quadrants[0].axis, testCase.axis) << "start " << testCase.start;

    // And the start vector is the unit direction of that angle, not the axis -- the axis is a table selector and the
    // start is a position. Conflating them would put every arc's first point a quarter turn out.
    EXPECT_POINT_NEAR(iteration.start, Matrix::CosSin(Degrees(testCase.start))) << "start " << testCase.start;
  }
}

} // namespace anonymous
} // namespace graphics
