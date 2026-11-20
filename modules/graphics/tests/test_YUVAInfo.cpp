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
 * @file test_YUVAInfo.cpp
 *
 * The shape of a YUV image.
 *
 * ## What is worth asserting
 *
 * Plane configurations and subsampling are two descriptions of the same image, and they can disagree. Almost every test
 * here is about the disagreements, because that is where a wrong answer is invisible:
 *
 *  - a chroma plane's size is the luma size divided by a factor, and the division rounds *up*, so an odd dimension
 *    produces a plane one sample larger than a naive division would;
 *  - subsampling is only meaningful where chroma is in a plane of its own, so a configuration that shares a plane
 *    between luma and chroma *cannot* be subsampled, and saying it is has no interpretation;
 *  - an orientation that swaps the axes swaps the plane dimensions too.
 *
 * A description that cannot be true is reported as invalid rather than as an exception, so most of these check
 * `isValid()` rather than looking for a throw.
 */

#include <graphics/YUVAInfo.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

using namespace graphics;

namespace {

/** A description of @p config at @p subsampling, at a size chosen to divide evenly unless told otherwise. */
YUVAInfo describe(const YUVAInfo::PlaneConfig config, const YUVAInfo::Subsampling subsampling, const int width = 64,
        const int height = 64, const EncodedOrigin origin = EncodedOrigin::kTopLeft) {
  return YUVAInfo(Size { static_cast<Scalar>(width), static_cast<Scalar>(height) }, config, subsampling,
          YUVColorSpace::kJPEG, origin);
}

} // namespace anonymous

// ==================================================================================================
// The layout, without an image
// ==================================================================================================

TEST(YUVAInfoTest, PlaneCountsFollowTheConfiguration) {
  // Three separate planes, and the two orders are different descriptions of different memory.
  EXPECT_EQ(YUVAInfo::NumPlanes(YUVAInfo::PlaneConfig::kY_U_V), 3);
  EXPECT_EQ(YUVAInfo::NumPlanes(YUVAInfo::PlaneConfig::kY_V_U), 3);

  EXPECT_EQ(YUVAInfo::NumPlanes(YUVAInfo::PlaneConfig::kY_UV), 2);
  EXPECT_EQ(YUVAInfo::NumPlanes(YUVAInfo::PlaneConfig::kYUV), 1);
  EXPECT_EQ(YUVAInfo::NumPlanes(YUVAInfo::PlaneConfig::kUYV), 1);

  EXPECT_EQ(YUVAInfo::NumPlanes(YUVAInfo::PlaneConfig::kY_U_V_A), 4);
  EXPECT_EQ(YUVAInfo::NumPlanes(YUVAInfo::PlaneConfig::kY_UV_A), 3);
  EXPECT_EQ(YUVAInfo::NumPlanes(YUVAInfo::PlaneConfig::kYUVA), 1);

  EXPECT_EQ(YUVAInfo::NumPlanes(YUVAInfo::PlaneConfig::kUnknown), 0) << "no description is not one plane";
}

TEST(YUVAInfoTest, ChannelCountsArePerPlane) {
  // Where the channels are interleaved, the plane has more than one channel. This is the question a caller asks to
  // know how wide a row of a plane is.
  EXPECT_EQ(YUVAInfo::NumChannelsInPlane(YUVAInfo::PlaneConfig::kY_U_V, 0), 1);
  EXPECT_EQ(YUVAInfo::NumChannelsInPlane(YUVAInfo::PlaneConfig::kY_U_V, 1), 1);
  EXPECT_EQ(YUVAInfo::NumChannelsInPlane(YUVAInfo::PlaneConfig::kY_U_V, 2), 1);

  EXPECT_EQ(YUVAInfo::NumChannelsInPlane(YUVAInfo::PlaneConfig::kY_UV, 0), 1);
  EXPECT_EQ(YUVAInfo::NumChannelsInPlane(YUVAInfo::PlaneConfig::kY_UV, 1), 2) << "U and V share a plane";

  EXPECT_EQ(YUVAInfo::NumChannelsInPlane(YUVAInfo::PlaneConfig::kYUV, 0), 3);
  EXPECT_EQ(YUVAInfo::NumChannelsInPlane(YUVAInfo::PlaneConfig::kYUVA, 0), 4);

  EXPECT_EQ(YUVAInfo::NumChannelsInPlane(YUVAInfo::PlaneConfig::kY_UV_A, 2), 1) << "alpha is a plane of its own";

  // Out of range is zero rather than a guess, for a caller looping over the planes of an unknown configuration.
  EXPECT_EQ(YUVAInfo::NumChannelsInPlane(YUVAInfo::PlaneConfig::kY_U_V, 3), 0);
  EXPECT_EQ(YUVAInfo::NumChannelsInPlane(YUVAInfo::PlaneConfig::kY_U_V, -1), 0);
  EXPECT_EQ(YUVAInfo::NumChannelsInPlane(YUVAInfo::PlaneConfig::kUnknown, 0), 0);
}

TEST(YUVAInfoTest, AlphaIsReportedOnlyWhereItExists) {
  EXPECT_FALSE(YUVAInfo::HasAlpha(YUVAInfo::PlaneConfig::kY_U_V));
  EXPECT_FALSE(YUVAInfo::HasAlpha(YUVAInfo::PlaneConfig::kYUV));

  EXPECT_TRUE(YUVAInfo::HasAlpha(YUVAInfo::PlaneConfig::kY_U_V_A));
  EXPECT_TRUE(YUVAInfo::HasAlpha(YUVAInfo::PlaneConfig::kY_UV_A));
  EXPECT_TRUE(YUVAInfo::HasAlpha(YUVAInfo::PlaneConfig::kYUVA));
  EXPECT_TRUE(YUVAInfo::HasAlpha(YUVAInfo::PlaneConfig::kUYVA));

  EXPECT_FALSE(YUVAInfo::HasAlpha(YUVAInfo::PlaneConfig::kUnknown));
}

// ==================================================================================================
// Subsampling
// ==================================================================================================

TEST(YUVAInfoTest, SubsamplingFactorsAreTheJAbNotation) {
  EXPECT_EQ(YUVAInfo::SubsamplingFactors(YUVAInfo::Subsampling::k444), std::make_tuple(1, 1));

  // 4:2:2 is halved horizontally; 4:2:0 is halved in both; 4:4:0 is halved vertically only. The notation reads as
  // (width, height) of the chroma block per luma block, and mixing the two up produces a plane of the right area and
  // the wrong shape.
  EXPECT_EQ(YUVAInfo::SubsamplingFactors(YUVAInfo::Subsampling::k422), std::make_tuple(2, 1));
  EXPECT_EQ(YUVAInfo::SubsamplingFactors(YUVAInfo::Subsampling::k420), std::make_tuple(2, 2));
  EXPECT_EQ(YUVAInfo::SubsamplingFactors(YUVAInfo::Subsampling::k440), std::make_tuple(1, 2));
  EXPECT_EQ(YUVAInfo::SubsamplingFactors(YUVAInfo::Subsampling::k411), std::make_tuple(4, 1));
  EXPECT_EQ(YUVAInfo::SubsamplingFactors(YUVAInfo::Subsampling::k410), std::make_tuple(4, 2));
}

TEST(YUVAInfoTest, OnlyChromaInItsOwnPlaneIsSubsampled) {
  // Luma is never subsampled, whatever the request.
  EXPECT_EQ(YUVAInfo::PlaneSubsamplingFactors(YUVAInfo::PlaneConfig::kY_U_V, YUVAInfo::Subsampling::k420, 0),
          std::make_tuple(1, 1)) << "plane 0 is luma and is always full resolution";
  EXPECT_EQ(YUVAInfo::PlaneSubsamplingFactors(YUVAInfo::PlaneConfig::kY_U_V, YUVAInfo::Subsampling::k420, 1),
          std::make_tuple(2, 2)) << "plane 1 is chroma and is subsampled";

  // Alpha is not subsampled either.
  EXPECT_EQ(YUVAInfo::PlaneSubsamplingFactors(YUVAInfo::PlaneConfig::kY_U_V_A, YUVAInfo::Subsampling::k420, 3),
          std::make_tuple(1, 1)) << "alpha keeps full resolution";
}

TEST(YUVAInfoTest, SubsamplingWhereChromaSharesAPlaneHasNoAnswer) {
  // A single-plane configuration has no chroma plane of its own, so its one plane is at full resolution and (1, 1)
  // is the honest answer -- there is no subsampled plane to report a factor for. What makes such a configuration
  // *invalid* with subsampling is a separate question, answered by the constructor and checked below.
  EXPECT_EQ(YUVAInfo::PlaneSubsamplingFactors(YUVAInfo::PlaneConfig::kYUV, YUVAInfo::Subsampling::k420, 0),
          std::make_tuple(1, 1)) << "a plane that holds no chroma is full resolution";
  EXPECT_EQ(YUVAInfo::PlaneSubsamplingFactors(YUVAInfo::PlaneConfig::kYUVA, YUVAInfo::Subsampling::k422, 0),
          std::make_tuple(1, 1));

  // A plane index past the end has no answer at all, which is a different thing again.
  EXPECT_EQ(YUVAInfo::PlaneSubsamplingFactors(YUVAInfo::PlaneConfig::kYUV, YUVAInfo::Subsampling::k420, 1),
          std::make_tuple(0, 0)) << "there is no plane 1 in a single-plane configuration";

  // And an unknown configuration or subsampling has no answer either.
  EXPECT_EQ(YUVAInfo::PlaneSubsamplingFactors(YUVAInfo::PlaneConfig::kUnknown, YUVAInfo::Subsampling::k420, 0),
          std::make_tuple(0, 0));
  EXPECT_EQ(YUVAInfo::PlaneSubsamplingFactors(YUVAInfo::PlaneConfig::kY_U_V, YUVAInfo::Subsampling::kUnknown, 1),
          std::make_tuple(0, 0));
}

// ==================================================================================================
// Plane dimensions
// ==================================================================================================

TEST(YUVAInfoTest, PlaneDimensionsOfTheObviousCase) {
  Size dimensions[YUVAInfo::kMaxPlanes] {};
  const YUVAInfo info = describe(YUVAInfo::PlaneConfig::kY_UV, YUVAInfo::Subsampling::k420, 64, 64);
  ASSERT_TRUE(info.isValid());

  ASSERT_EQ(info.planeDimensions(dimensions), 2);
  EXPECT_FLOAT_EQ(dimensions[0].width, 64.0f) << "luma is full resolution";
  EXPECT_FLOAT_EQ(dimensions[0].height, 64.0f);
  EXPECT_FLOAT_EQ(dimensions[1].width, 32.0f) << "chroma is halved in both directions";
  EXPECT_FLOAT_EQ(dimensions[1].height, 32.0f);
}

TEST(YUVAInfoTest, PlaneDimensionsRoundUpForOddSizes) {
  // The case a plain integer division gets wrong. An odd dimension has a partial block at the end, and that block still
  // needs a sample to describe it -- so the plane is 33 wide, not 32. Getting this wrong produces a plane that is one
  // column short, which shows as the rightmost column of the image being wrong.
  Size dimensions[YUVAInfo::kMaxPlanes] {};
  const YUVAInfo info = describe(YUVAInfo::PlaneConfig::kY_UV, YUVAInfo::Subsampling::k420, 65, 65);
  ASSERT_TRUE(info.isValid());

  ASSERT_EQ(info.planeDimensions(dimensions), 2);
  EXPECT_FLOAT_EQ(dimensions[0].width, 65.0f);
  EXPECT_FLOAT_EQ(dimensions[1].width, 33.0f) << "65 divided by 2 rounds up to 33, not down to 32";
  EXPECT_FLOAT_EQ(dimensions[1].height, 33.0f);
}

TEST(YUVAInfoTest, ASwappedOrientationSwapsThePlaneDimensions) {
  // A 90-degree orientation means the stored image is the transpose of the displayed one, so the planes are the
  // transpose of what the display size would suggest. A description whose plane dimensions are the right area and the
  // wrong shape shows as a sheared image, which is the failure this test exists to catch.
  Size displayed[YUVAInfo::kMaxPlanes] {};
  Size stored[YUVAInfo::kMaxPlanes] {};

  const YUVAInfo upright = describe(YUVAInfo::PlaneConfig::kY_UV, YUVAInfo::Subsampling::k420, 64, 32);
  ASSERT_EQ(upright.planeDimensions(displayed), 2);
  EXPECT_FLOAT_EQ(displayed[0].width, 64.0f);
  EXPECT_FLOAT_EQ(displayed[0].height, 32.0f);

  const YUVAInfo rotated =
          describe(YUVAInfo::PlaneConfig::kY_UV, YUVAInfo::Subsampling::k420, 64, 32, EncodedOrigin::kRightTop);
  ASSERT_EQ(rotated.planeDimensions(stored), 2);
  EXPECT_FLOAT_EQ(stored[0].width, 32.0f) << "stored width is the displayed height";
  EXPECT_FLOAT_EQ(stored[0].height, 64.0f);

  // The mirror-only orientations do not swap, so they are not affected.
  Size mirrored[YUVAInfo::kMaxPlanes] {};
  const YUVAInfo flipped =
          describe(YUVAInfo::PlaneConfig::kY_UV, YUVAInfo::Subsampling::k420, 64, 32, EncodedOrigin::kTopRight);
  ASSERT_EQ(flipped.planeDimensions(mirrored), 2);
  EXPECT_FLOAT_EQ(mirrored[0].width, 64.0f) << "a horizontal mirror does not swap the axes";
}

TEST(YUVAInfoTest, AnInvalidDescriptionHasNoPlaneDimensions) {
  Size dimensions[YUVAInfo::kMaxPlanes] {};
  const YUVAInfo invalid;
  EXPECT_FALSE(invalid.isValid());
  EXPECT_EQ(invalid.planeDimensions(dimensions), 0);
  EXPECT_EQ(invalid.numPlanes(), 0);
}

// ==================================================================================================
// What cannot be described
// ==================================================================================================

TEST(YUVAInfoTest, SubsampledChromaSharingAPlaneIsInvalid) {
  // A configuration that interleaves Y with U and V describes them at the same resolution. Asking for subsampling at
  // the same time is a contradiction, and the description is invalid rather than being silently adjusted.
  const YUVAInfo contradictory = describe(YUVAInfo::PlaneConfig::kYUV, YUVAInfo::Subsampling::k420);
  EXPECT_FALSE(contradictory.isValid()) << "Y and UV cannot share a plane and be subsampled";

  // The same configuration without subsampling is fine, which is what makes this a check on the combination rather
  // than on the configuration.
  EXPECT_TRUE(describe(YUVAInfo::PlaneConfig::kYUV, YUVAInfo::Subsampling::k444).isValid());

  // And three separate planes can be subsampled, because the chroma has a plane of its own.
  EXPECT_TRUE(describe(YUVAInfo::PlaneConfig::kY_U_V, YUVAInfo::Subsampling::k420).isValid());
}

TEST(YUVAInfoTest, EmptyDimensionsAreInvalid) {
  EXPECT_FALSE(describe(YUVAInfo::PlaneConfig::kY_U_V, YUVAInfo::Subsampling::k420, 0, 64).isValid());
  EXPECT_FALSE(describe(YUVAInfo::PlaneConfig::kY_U_V, YUVAInfo::Subsampling::k420, 64, 0).isValid());
  EXPECT_FALSE(describe(YUVAInfo::PlaneConfig::kY_U_V, YUVAInfo::Subsampling::k420, -1, 64).isValid());
}

TEST(YUVAInfoTest, MakingAnImpossibleSubsamplingIsReportedRatherThanIgnored) {
  const YUVAInfo interleaved = describe(YUVAInfo::PlaneConfig::kYUV, YUVAInfo::Subsampling::k444);
  ASSERT_TRUE(interleaved.isValid());

  // Ignoring the request would leave a caller believing it had specified subsampling; changing the plane layout to
  // accommodate it would describe an image the caller does not have. An invalid result says so.
  EXPECT_FALSE(interleaved.makeSubsampling(YUVAInfo::Subsampling::k420).isValid());
  EXPECT_TRUE(interleaved.makeSubsampling(YUVAInfo::Subsampling::k444).isValid());

  // Where it is possible, it is applied and everything else is unchanged.
  const YUVAInfo separate = describe(YUVAInfo::PlaneConfig::kY_U_V, YUVAInfo::Subsampling::k444);
  const YUVAInfo subsampled = separate.makeSubsampling(YUVAInfo::Subsampling::k422);
  EXPECT_TRUE(subsampled.isValid());
  EXPECT_EQ(subsampled.subsampling(), YUVAInfo::Subsampling::k422);
  EXPECT_EQ(subsampled.planeConfig(), YUVAInfo::PlaneConfig::kY_U_V) << "the layout is not changed";
  EXPECT_EQ(subsampled.dimensions(), separate.dimensions());
}

TEST(YUVAInfoTest, MakingEmptyDimensionsIsReported) {
  const YUVAInfo info = describe(YUVAInfo::PlaneConfig::kY_U_V, YUVAInfo::Subsampling::k444);
  EXPECT_FALSE(info.makeDimensions(Size { 0.0f, 10.0f }).isValid());
  EXPECT_TRUE(info.makeDimensions(Size { 10.0f, 20.0f }).isValid());
}

// ==================================================================================================
// Sizes
// ==================================================================================================

TEST(YUVAInfoTest, TotalBytesSumsThePlanes) {
  const YUVAInfo info = describe(YUVAInfo::PlaneConfig::kY_UV, YUVAInfo::Subsampling::k420, 64, 64);
  ASSERT_TRUE(info.isValid());

  // One byte per sample: 64x64 of luma plus 32x32 of interleaved chroma, which is two channels. So the chroma plane's
  // row is 64 bytes and it has 32 rows.
  const size_t rowBytes[YUVAInfo::kMaxPlanes] = { 64, 64, 0, 0 };
  size_t planeSizes[YUVAInfo::kMaxPlanes] = {};
  const size_t total = info.computeTotalBytes(rowBytes, planeSizes);

  EXPECT_EQ(total, 64u * 64u + 64u * 32u);
  EXPECT_EQ(planeSizes[0], 64u * 64u);
  EXPECT_EQ(planeSizes[1], 64u * 32u);
}

TEST(YUVAInfoTest, AnEmptyRowLengthIsRefused) {
  // A row of no bytes gives a plane of no bytes, which is not a plane. This is the one row length that is certainly
  // wrong whatever the format, and the one that can be checked without knowing a plane's bytes per sample.
  const YUVAInfo info = describe(YUVAInfo::PlaneConfig::kY_UV, YUVAInfo::Subsampling::k420, 64, 64);
  const size_t rowBytes[YUVAInfo::kMaxPlanes] = { 0, 64, 0, 0 };

  size_t planeSizes[YUVAInfo::kMaxPlanes] = {};
  EXPECT_EQ(info.computeTotalBytes(rowBytes, planeSizes), std::numeric_limits<size_t>::max());
  for (size_t size: planeSizes) {
    EXPECT_EQ(size, std::numeric_limits<size_t>::max()) << "every plane reports the failure, not just the bad one";
  }
}

TEST(YUVAInfoTest, ARowWiderThanThePlaneIsAccepted) {
  // A row can legitimately be longer than the plane is wide: padding to an alignment is ordinary, and so are 16-bit
  // samples, where a 64-sample plane has 128-byte rows. A check that compared the row length against the plane's width
  // in samples would refuse both -- which is why there is no such check.
  const YUVAInfo info = describe(YUVAInfo::PlaneConfig::kY_UV, YUVAInfo::Subsampling::k420, 64, 64);
  const size_t rowBytes[YUVAInfo::kMaxPlanes] = { 128, 128, 0, 0 };

  size_t planeSizes[YUVAInfo::kMaxPlanes] = {};
  const size_t total = info.computeTotalBytes(rowBytes, planeSizes);
  EXPECT_EQ(total, 128u * 64u + 128u * 32u) << "the row length is what the caller gave, and it is used as it is";
  EXPECT_EQ(planeSizes[0], 128u * 64u);
}

TEST(YUVAInfoTest, AnOverflowingSizeIsRefusedRatherThanWrapped) {
  // A wrapped size is a small number that a caller would allocate happily, and then write past. The failure has to be
  // unmistakable.
  const YUVAInfo info = describe(YUVAInfo::PlaneConfig::kY_U_V, YUVAInfo::Subsampling::k444, 100000, 100000);
  ASSERT_TRUE(info.isValid());

  // A row of 2^60 bytes over 100000 rows is about 2^77 bytes, which is far past what a size_t holds. The first
  // version of this test used 2^40, which is about 2^57 in total -- large, and comfortably representable, so the
  // check correctly did not fire and the test was asserting arithmetic of its own that was wrong.
  const size_t rowBytes[YUVAInfo::kMaxPlanes] = { size_t{1} << 60, size_t{1} << 60, size_t{1} << 60, 0 };
  size_t planeSizes[YUVAInfo::kMaxPlanes] = {};
  EXPECT_EQ(info.computeTotalBytes(rowBytes, planeSizes), std::numeric_limits<size_t>::max());
  EXPECT_EQ(planeSizes[0], std::numeric_limits<size_t>::max());
}

TEST(YUVAInfoTest, AnInvalidDescriptionHasNoSize) {
  const YUVAInfo invalid;
  const size_t rowBytes[YUVAInfo::kMaxPlanes] = { 64, 64, 64, 64 };
  EXPECT_EQ(invalid.computeTotalBytes(rowBytes), 0u) << "there is nothing to size";
  EXPECT_EQ(invalid.computeTotalBytes(nullptr), 0u);
}

// ==================================================================================================
// Identity
// ==================================================================================================

TEST(YUVAInfoTest, DescriptionsCompareByEveryField) {
  const YUVAInfo reference = describe(YUVAInfo::PlaneConfig::kY_UV, YUVAInfo::Subsampling::k420);
  EXPECT_EQ(reference, describe(YUVAInfo::PlaneConfig::kY_UV, YUVAInfo::Subsampling::k420));

  // Each field on its own has to change the answer, or a cache keyed on this would serve a description of a different
  // image. The colour space is the one that is easiest to forget, because it does not affect the layout.
  EXPECT_NE(reference, describe(YUVAInfo::PlaneConfig::kY_VU, YUVAInfo::Subsampling::k420));
  EXPECT_NE(reference, describe(YUVAInfo::PlaneConfig::kY_UV, YUVAInfo::Subsampling::k422));
  EXPECT_NE(reference, describe(YUVAInfo::PlaneConfig::kY_UV, YUVAInfo::Subsampling::k420, 32, 64));

  const YUVAInfo otherSpace(Size { 64.0f, 64.0f }, YUVAInfo::PlaneConfig::kY_UV, YUVAInfo::Subsampling::k420,
          YUVColorSpace::kRec709);
  EXPECT_NE(reference, otherSpace) << "the colour space is part of the description";

  const YUVAInfo rotated = describe(
          YUVAInfo::PlaneConfig::kY_UV, YUVAInfo::Subsampling::k420, 64, 64, EncodedOrigin::kRightTop);
  EXPECT_NE(reference, rotated) << "the orientation is part of the description";

  EXPECT_EQ(YUVAInfo {}, YUVAInfo {}) << "two invalid descriptions are equal to each other";
}

TEST(YUVAInfoTest, TheEnumsHaveNames) {
  // The names are what a log line prints, and a switch that lost a case would report something else's name.
  EXPECT_STREQ(toString(EncodedOrigin::kTopLeft), "TopLeft");
  EXPECT_STREQ(toString(EncodedOrigin::kRightTop), "RightTop");
  EXPECT_STREQ(toString(EncodedOrigin::kLeftBottom), "LeftBottom");

  EXPECT_STREQ(toString(YUVColorSpace::kIdentity), "Identity");
  EXPECT_STREQ(toString(YUVColorSpace::kRec709), "Rec709");
  EXPECT_STREQ(toString(YUVColorSpace::kBT2020ConstantLuminance), "BT2020ConstantLuminance");
}
