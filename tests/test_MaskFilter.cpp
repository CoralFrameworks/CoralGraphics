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
 * @file test_MaskFilter.cpp
 *
 * Softening a shape's coverage.
 *
 * ## What is worth asserting
 *
 * A blur is a *conservative* operation: it moves coverage around but does not create or destroy it. That is the
 * property a wrong implementation loses first -- a kernel that is not normalised, or an edge that is renormalised
 * where it should not be, changes the total. So the central assertion here is that the total coverage is preserved,
 * computed from the mask rather than read from the filter.
 *
 * The other assertions are about the four styles, which are four different answers to "what is the shape now" and are
 * easy to implement as each other by mistake. Each is checked against a shape with a known inside: a `kSolid` blur
 * must leave the middle fully covered while a `kNormal` one does not, an `kOuter` one must leave the middle *empty*,
 * and a `kInner` one must leave everything outside empty. Getting those the wrong way round produces a picture that is
 * obviously wrong -- a drop shadow over the thing casting it -- so they are asserted on the mask directly.
 *
 * ## What is not asserted
 *
 * Exact sample values. The kernel's weights are rounded to fixed point and the taps are added in an order, so a test
 * that pinned a particular blur radius's output would be asserting this implementation's arithmetic rather than the
 * blur. The properties above are what a caller depends on; the arithmetic is checked by the conservation test.
 */

#include <graphics/MaskFilter.h>

#include <private/graphics/Mask.h>
#include <private/graphics/MaskFilterInternal.h>

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

using namespace graphics;

namespace {

/** @return a square mask of @p size, fully covered, which is a shape with an obvious inside and outside. */
mask::Mask solidSquare(const int size) {
  mask::Mask result = mask::Mask::allocate(size, size, 0, 0);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) result.set(x, y, 255);
  }
  return result;
}

/** @return the sum of every sample, which is the coverage the mask holds in total. */
long long totalCoverage(const mask::Mask& mask) {
  long long total = 0;
  for (int y = 0; y < mask.height; ++y) {
    for (int x = 0; x < mask.width; ++x) total += mask.at(x, y);
  }
  return total;
}

/** @return the coverage at the mask's centre, in the coordinates the mask itself uses. */
uint8_t centreCoverage(const mask::Mask& mask) { return mask.at(mask.width / 2, mask.height / 2); }

} // namespace anonymous

// ==================================================================================================
// Building one
// ==================================================================================================

TEST(MaskFilterTest, ABlurNeedsAPositiveSigma) {
  EXPECT_NE(MaskFilter::Blur(MaskFilter::Style::kNormal, 2.0f), nullptr);

  // Zero is refused rather than treated as no blur. A caller that passed it has a scale computed wrong upstream, and
  // a silent no-op would send it looking at the drawing code instead of at the scale.
  EXPECT_EQ(MaskFilter::Blur(MaskFilter::Style::kNormal, 0.0f), nullptr);
  EXPECT_EQ(MaskFilter::Blur(MaskFilter::Style::kNormal, -1.0f), nullptr);
  EXPECT_EQ(MaskFilter::Blur(MaskFilter::Style::kNormal, std::numeric_limits<Scalar>::infinity()), nullptr);
  EXPECT_EQ(MaskFilter::Blur(MaskFilter::Style::kNormal, std::numeric_limits<Scalar>::quiet_NaN()), nullptr);
}

TEST(MaskFilterTest, TheStyleAndSigmaAreReportedBack) {
  const auto filter = MaskFilter::Blur(MaskFilter::Style::kOuter, 3.5f);
  ASSERT_NE(filter, nullptr);

  EXPECT_EQ(filter->style(), MaskFilter::Style::kOuter);
  EXPECT_FLOAT_EQ(filter->sigma(), 3.5f);
}

TEST(MaskFilterTest, TheMarginIsThreeStandardDeviations) {
  // Where the Gaussian's tails round to zero at eight bits, and what a caller sizes its layer by. A margin smaller
  // than the blur's reach clips the spread off square, which is the most visible way to get a soft shadow wrong.
  const auto small = MaskFilter::Blur(MaskFilter::Style::kNormal, 2.0f);
  const auto large = MaskFilter::Blur(MaskFilter::Style::kNormal, 10.0f);
  ASSERT_NE(small, nullptr);
  ASSERT_NE(large, nullptr);

  EXPECT_NEAR(small->margin(), 6.0f, 0.001f);
  EXPECT_NEAR(large->margin(), 30.0f, 0.001f);
  EXPECT_GT(large->margin(), small->margin()) << "a wider blur reaches further";
}

// ==================================================================================================
// The blur itself
// ==================================================================================================

TEST(MaskTest, ABlurPreservesTheTotalCoverage) {
  // The property that says the kernel is normalised. A blur moves coverage; it must not create or destroy it.
  //
  // Measured on a *padded* mask, which is the state the blur is used in and the only state in which conservation is
  // even the right thing to ask for. A sample outside a mask reads as zero coverage -- deliberately, since that is
  // what the edge of a shape is -- so blurring a bare square loses its border into the empty space around it, and the
  // amount lost depends on how large the square is relative to the blur. That is correct behaviour and not
  // conservation's business; what conservation is about is that no coverage is created or destroyed *within* the
  // frame, which the padding makes measurable.
  const mask::Mask source = mask::padded(solidSquare(32), 16);
  const long long before = totalCoverage(source);
  ASSERT_GT(before, 0);

  for (const Scalar sigma: { 1.0f, 2.0f, 4.0f }) {
    const mask::Mask blurred = mask::blur(source, sigma);
    ASSERT_FALSE(blurred.isEmpty()) << "sigma " << sigma;

    const long long after = totalCoverage(blurred);
    const double ratio = static_cast<double>(after) / static_cast<double>(before);
    EXPECT_NEAR(ratio, 1.0, 0.03)
            << "a blur must conserve coverage within its frame, and at sigma " << sigma << " it kept " << ratio;
  }
}

TEST(MaskTest, ABlurLosesTheBorderOfAnUnpaddedMask) {
  // The other side of the same behaviour, asserted so that it is a decision rather than a surprise: a mask that ends
  // is a shape that ends, so the coverage outside it is genuinely zero and the blur spreads the edge into nothing.
  //
  // This is why `mask_filter_detail::apply` pads before blurring. Without the padding a soft shadow would be cut off
  // square at the frame, and the amount cut off would depend on how the caller happened to size its buffer.
  const mask::Mask source = solidSquare(32);
  const mask::Mask blurred = mask::blur(source, 2.0f);

  const double ratio = static_cast<double>(totalCoverage(blurred)) / static_cast<double>(totalCoverage(source));
  EXPECT_LT(ratio, 0.95) << "an unpadded square loses its border to the empty space around it";
  EXPECT_GT(ratio, 0.5) << "but most of it survives, because most of it is not near an edge";
}

TEST(MaskTest, ABlurOfAUniformRegionLeavesItUniform) {
  // The sharpest test of the kernel's normalisation: a region that is uniformly covered must come out of the blur
  // exactly as it went in. An unnormalised kernel, or a rounding error that is not corrected, shows up here as a
  // flat area that has drifted -- which in a real picture is a visible halo around a blurred shape.
  // Padded, so that the kernel's taps read real coverage rather than the empty space outside the frame. On a bare
  // square the samples within three sigma of the border are pulled down by the zero outside -- correctly, since that
  // is the shape's edge -- and the test would be measuring the edge rather than the kernel's normalisation.
  //
  // The padding is three sigma for the first pass plus three more for the second, because the vertical pass reads the
  // *output* of the horizontal one: a row three pixels above the sample being checked has already been pulled down if
  // it was within three sigma of the top edge, and that loss then travels into the sample.
  const mask::Mask source = mask::padded(solidSquare(24), 24);
  const mask::Mask blurred = mask::blur(source, 3.0f);

  // The middle is far from any edge, so every tap landed on full coverage and the result must be exactly full.
  const int centre = blurred.width / 2;
  const int centreY = blurred.height / 2;
  for (int offset = -3; offset <= 3; ++offset) {
    EXPECT_EQ(blurred.at(centre + offset, centreY), 255)
            << "a fully covered area stays fully covered, at offset " << offset;
  }
}

TEST(MaskTest, ABlurSpreadsCoverageOutwards) {
  // The other half of conservation: what leaves the middle arrives at the edges. A blur that did nothing would pass
  // the conservation test and fail this one.
  const mask::Mask source = solidSquare(32);
  const mask::Mask blurred = mask::blur(source, 3.0f);

  // One pixel in from an edge the coverage has fallen away from full...
  EXPECT_LT(blurred.at(0, 16), 255) << "the edge of the shape is no longer sharp";
  EXPECT_GT(blurred.at(0, 16), 0) << "but the blur reached it";

  // ...and the falloff is monotonic going inwards, which a blur that merely scaled the mask would not give.
  EXPECT_LT(blurred.at(0, 16), blurred.at(4, 16));
  EXPECT_LE(blurred.at(4, 16), blurred.at(8, 16));
}

TEST(MaskTest, AZeroSigmaBlurChangesNothing) {
  // Not reachable through the public factory, which refuses a zero sigma -- but the primitive is used by the styles
  // and has to answer something sensible rather than dividing by zero.
  const mask::Mask source = solidSquare(8);
  const mask::Mask blurred = mask::blur(source, 0.0f);

  ASSERT_EQ(blurred.width, source.width);
  ASSERT_EQ(blurred.height, source.height);
  for (int y = 0; y < source.height; ++y) {
    for (int x = 0; x < source.width; ++x) EXPECT_EQ(blurred.at(x, y), source.at(x, y));
  }
}

TEST(MaskTest, AnEmptyMaskBlursToAnEmptyMask) {
  const mask::Mask empty;
  EXPECT_TRUE(mask::blur(empty, 2.0f).isEmpty());
}

// ==================================================================================================
// Padding
// ==================================================================================================

TEST(MaskTest, PaddingGrowsTheMaskAndKeepsItsOrigin) {
  const mask::Mask source = solidSquare(8);
  const mask::Mask padded = mask::padded(source, 3);

  ASSERT_EQ(padded.width, 14);
  ASSERT_EQ(padded.height, 14);

  // The origin moves out with the padding, so the shape stays where it was in the space it came from. A padded mask
  // that kept its origin would be drawn three pixels up and to the left.
  EXPECT_EQ(padded.originX, -3);
  EXPECT_EQ(padded.originY, -3);

  // And the original is in the middle, with a zero border around it.
  EXPECT_EQ(padded.at(3, 3), 255) << "the shape's top-left corner is three pixels in";
  EXPECT_EQ(padded.at(0, 0), 0) << "and the border is empty";
  EXPECT_EQ(padded.at(13, 13), 0);
}

TEST(MaskTest, PaddingByNothingIsTheSameMask) {
  const mask::Mask source = solidSquare(8);
  const mask::Mask padded = mask::padded(source, 0);
  EXPECT_EQ(padded.width, source.width);
  EXPECT_EQ(padded.originX, source.originX);
}

// ==================================================================================================
// The four styles
// ==================================================================================================

TEST(MaskFilterStylesTest, NormalKeepsTheMiddleAndSoftensTheEdge) {
  const mask::Mask source = solidSquare(32);
  const auto filter = MaskFilter::Blur(MaskFilter::Style::kNormal, 3.0f);
  ASSERT_NE(filter, nullptr);

  const mask::Mask result = mask_filter_detail::apply(*filter, source);

  // The middle stays covered: a blur of a large solid shape does not hollow it out.
  EXPECT_GT(result.at(result.width / 2, result.height / 2), 250);

  // And the result reaches beyond the source, because the blur spread.
  EXPECT_LT(result.originX, source.originX) << "a normal blur grows the shape's bounds";
  EXPECT_LT(result.originY, source.originY);
}

TEST(MaskFilterStylesTest, SolidFillsTheInteriorAndSoftensOnlyTheOutside) {
  // The distinction from Normal is on a *thin* shape: normal blurring makes the whole thing semi-transparent because
  // there is no interior far enough from an edge to stay at full coverage, and solid blurring does not.
  mask::Mask thin = mask::Mask::allocate(32, 4, 0, 0);
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 32; ++x) thin.set(x, y, 255);
  }

  const auto normal = MaskFilter::Blur(MaskFilter::Style::kNormal, 2.0f);
  const auto solid = MaskFilter::Blur(MaskFilter::Style::kSolid, 2.0f);
  ASSERT_NE(normal, nullptr);
  ASSERT_NE(solid, nullptr);

  const mask::Mask normalResult = mask_filter_detail::apply(*normal, thin);
  const mask::Mask solidResult = mask_filter_detail::apply(*solid, thin);

  // The sharp coverage sits inside the padded frame, offset by the padding radius -- which is `ceil(3 * sigma)`, the
  // radius the blur itself uses. Working it out here rather than reading it back means an implementation that padded
  // by something else would put the shape in the wrong place and fail, instead of moving the test's expectations with
  // it.
  const int pad = static_cast<int>(std::ceil(3.0f * 2.0f));
  const int midX = pad + 16;
  const int midY = pad + 2;

  EXPECT_LT(normalResult.at(midX, midY), 255) << "a thin shape blurred normally goes translucent";
  EXPECT_EQ(solidResult.at(midX, midY), 255) << "the solid style keeps it fully covered";
  EXPECT_GT(solidResult.at(midX, midY), normalResult.at(midX, midY));
}

TEST(MaskFilterStylesTest, OuterLeavesTheInteriorEmpty) {
  // The glow, and with an offset the drop shadow. What matters is that the shape's own area is *not* drawn -- a
  // shadow that covered the thing casting it is the mistake this catches.
  const mask::Mask source = solidSquare(32);
  const auto filter = MaskFilter::Blur(MaskFilter::Style::kOuter, 3.0f);
  ASSERT_NE(filter, nullptr);

  const mask::Mask result = mask_filter_detail::apply(*filter, source);
  ASSERT_FALSE(result.isEmpty());

  // Recomputed rather than read from `margin`, which rounds its answer and would move these expectations with the
  // implementation.
  const int pad = static_cast<int>(std::ceil(3.0f * 3.0f));
  EXPECT_EQ(result.at(pad + 16, pad + 16), 0) << "the middle of the shape is not drawn";
  EXPECT_EQ(result.at(pad + 2, pad + 2), 0) << "nor is the part of the shape away from its edge";

  // But the blur outside the shape is drawn, which is the whole point.
  EXPECT_GT(result.at(pad - 1, pad + 16), 0) << "the spread beyond the edge is drawn";
}

TEST(MaskFilterStylesTest, InnerLeavesEverythingOutsideEmpty) {
  // The inner shadow. Exactly the complement of the outer style: something inside the shape, nothing outside it.
  const mask::Mask source = solidSquare(32);
  const auto filter = MaskFilter::Blur(MaskFilter::Style::kInner, 3.0f);
  ASSERT_NE(filter, nullptr);

  const mask::Mask result = mask_filter_detail::apply(*filter, source);
  ASSERT_FALSE(result.isEmpty());

  const int pad = static_cast<int>(std::ceil(3.0f * 3.0f));
  EXPECT_EQ(result.at(pad - 1, pad + 16), 0) << "nothing outside the shape is drawn";
  EXPECT_EQ(result.at(pad, pad - 1), 0) << "nor above it";

  // And the interior near the edge is drawn, because that is where an inner shadow falls.
  EXPECT_GT(result.at(pad + 1, pad + 16), 0) << "the inside of the edge is drawn";
}

TEST(MaskFilterStylesTest, TheFourStylesAreAllDifferent) {
  // A blunt check that the styles are four answers and not one with three aliases. Two of them agreeing everywhere
  // would mean at least one branch of the switch is wrong.
  const mask::Mask source = solidSquare(24);

  const char* names[] = { "Normal", "Solid", "Outer", "Inner" };
  std::vector<long long> totals;
  for (const auto style: { MaskFilter::Style::kNormal, MaskFilter::Style::kSolid, MaskFilter::Style::kOuter,
               MaskFilter::Style::kInner }) {
    const auto filter = MaskFilter::Blur(style, 2.5f);
    ASSERT_NE(filter, nullptr);
    totals.push_back(totalCoverage(mask_filter_detail::apply(*filter, source)));
  }

  for (const long long total: totals) EXPECT_GT(total, 0) << "every style draws something for a solid square";

  for (size_t i = 0; i < totals.size(); ++i) {
    for (size_t j = i + 1; j < totals.size(); ++j) {
      EXPECT_NE(totals[i], totals[j])
              << names[i] << " (" << totals[i] << ") and " << names[j] << " (" << totals[j]
              << ") produced the same total coverage, so one of them is not doing what its name says";
    }
  }
}

TEST(MaskFilterStylesTest, AFilteredEmptyMaskIsEmpty) {
  // Every style, on nothing. A caller that filters a shape which turned out to be empty must get nothing back rather
  // than an allocation of the margin's size.
  for (const auto style: { MaskFilter::Style::kNormal, MaskFilter::Style::kSolid, MaskFilter::Style::kOuter,
               MaskFilter::Style::kInner }) {
    const auto filter = MaskFilter::Blur(style, 2.0f);
    ASSERT_NE(filter, nullptr);
    EXPECT_TRUE(mask_filter_detail::apply(*filter, mask::Mask {}).isEmpty()) << "style " << static_cast<int>(style);
  }
}
