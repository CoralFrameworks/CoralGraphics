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

#include <graphics/YUVAInfo.h>

#include <algorithm>
#include <limits>

namespace graphics {
namespace {

/** @return the number of chrome samples per @p subsampling block in x and y, or (0,0) when unknown. */
std::pair<int, int> factorsOf(const YUVAInfo::Subsampling subsampling) noexcept {
  switch (subsampling) {
    case YUVAInfo::Subsampling::k444: return { 1, 1 };
    case YUVAInfo::Subsampling::k422: return { 2, 1 };
    case YUVAInfo::Subsampling::k420: return { 2, 2 };
    case YUVAInfo::Subsampling::k440: return { 1, 2 };
    case YUVAInfo::Subsampling::k411: return { 4, 1 };
    case YUVAInfo::Subsampling::k410: return { 4, 2 };
    case YUVAInfo::Subsampling::kUnknown: break;
  }
  return { 0, 0 };
}

/** @return which of Y, U, V and A live in @p planeIndex, as flags into the YUVA order. */
int channelsInPlane(const YUVAInfo::PlaneConfig config, const int planeIndex) noexcept {
  if (planeIndex < 0 || planeIndex >= YUVAInfo::kMaxPlanes) return 0;

  switch (config) {
    case YUVAInfo::PlaneConfig::kY_U_V:
    case YUVAInfo::PlaneConfig::kY_V_U:
      return planeIndex < 3 ? 1 : 0;

    case YUVAInfo::PlaneConfig::kY_UV:
    case YUVAInfo::PlaneConfig::kY_VU:
      return planeIndex == 0 ? 1 : (planeIndex == 1 ? 2 : 0);

    case YUVAInfo::PlaneConfig::kYUV:
    case YUVAInfo::PlaneConfig::kUYV:
      return planeIndex == 0 ? 3 : 0;

    case YUVAInfo::PlaneConfig::kY_U_V_A:
    case YUVAInfo::PlaneConfig::kY_V_U_A:
      return planeIndex < 4 ? 1 : 0;

    case YUVAInfo::PlaneConfig::kY_UV_A:
    case YUVAInfo::PlaneConfig::kY_VU_A:
      return planeIndex == 0 ? 1 : (planeIndex == 1 ? 2 : (planeIndex == 2 ? 1 : 0));

    case YUVAInfo::PlaneConfig::kYUVA:
    case YUVAInfo::PlaneConfig::kUYVA:
      return planeIndex == 0 ? 4 : 0;

    case YUVAInfo::PlaneConfig::kUnknown: break;
  }
  return 0;
}

/** @return the number of planes @p config describes. */
int planesOf(const YUVAInfo::PlaneConfig config) noexcept {
  switch (config) {
    case YUVAInfo::PlaneConfig::kUnknown: return 0;
    case YUVAInfo::PlaneConfig::kY_U_V:
    case YUVAInfo::PlaneConfig::kY_V_U: return 3;
    case YUVAInfo::PlaneConfig::kY_UV:
    case YUVAInfo::PlaneConfig::kY_VU: return 2;
    case YUVAInfo::PlaneConfig::kYUV:
    case YUVAInfo::PlaneConfig::kUYV: return 1;
    case YUVAInfo::PlaneConfig::kY_U_V_A:
    case YUVAInfo::PlaneConfig::kY_V_U_A: return 4;
    case YUVAInfo::PlaneConfig::kY_UV_A:
    case YUVAInfo::PlaneConfig::kY_VU_A: return 3;
    case YUVAInfo::PlaneConfig::kYUVA:
    case YUVAInfo::PlaneConfig::kUYVA: return 1;
  }
  return 0;
}

/** @return true when @p config puts U and V in a plane that Y is not in, which is what subsampling requires. */
bool chromaIsSeparate(const YUVAInfo::PlaneConfig config) noexcept {
  switch (config) {
    // Y alone in its own plane, so U and V can be a lower resolution.
    case YUVAInfo::PlaneConfig::kY_U_V:
    case YUVAInfo::PlaneConfig::kY_V_U:
    case YUVAInfo::PlaneConfig::kY_UV:
    case YUVAInfo::PlaneConfig::kY_VU:
    case YUVAInfo::PlaneConfig::kY_U_V_A:
    case YUVAInfo::PlaneConfig::kY_V_U_A:
    case YUVAInfo::PlaneConfig::kY_UV_A:
    case YUVAInfo::PlaneConfig::kY_VU_A:
      return true;

    // Y shares a plane with U and V, so they are at the same resolution by construction.
    case YUVAInfo::PlaneConfig::kYUV:
    case YUVAInfo::PlaneConfig::kUYV:
    case YUVAInfo::PlaneConfig::kYUVA:
    case YUVAInfo::PlaneConfig::kUYVA:
    case YUVAInfo::PlaneConfig::kUnknown:
      return false;
  }
  return false;
}

/** @return true when @p planeIndex holds U or V rather than Y or A. */
bool planeHoldsChroma(const YUVAInfo::PlaneConfig config, const int planeIndex) noexcept {
  switch (config) {
    case YUVAInfo::PlaneConfig::kY_U_V:
    case YUVAInfo::PlaneConfig::kY_V_U:
    case YUVAInfo::PlaneConfig::kY_U_V_A:
    case YUVAInfo::PlaneConfig::kY_V_U_A:
      return planeIndex == 1 || planeIndex == 2;

    case YUVAInfo::PlaneConfig::kY_UV:
    case YUVAInfo::PlaneConfig::kY_VU:
    case YUVAInfo::PlaneConfig::kY_UV_A:
    case YUVAInfo::PlaneConfig::kY_VU_A:
      return planeIndex == 1;

    default:
      return false;
  }
}

} // namespace anonymous

// ==================================================================================================
// The static description
// ==================================================================================================

std::tuple<int, int> YUVAInfo::SubsamplingFactors(const Subsampling subsampling) {
  const auto [x, y] = factorsOf(subsampling);
  return { x, y };
}

std::tuple<int, int> YUVAInfo::PlaneSubsamplingFactors(
        const PlaneConfig config, const Subsampling subsampling, const int planeIndex) {
  // An invalid combination is reported as (0, 0) rather than as (1, 1), which is the difference between "these planes
  // are full resolution" and "this question has no answer". A caller that divided by the result needs to be able to
  // tell them apart, and a factor of zero fails loudly where a factor of one would silently be wrong.
  if (config == PlaneConfig::kUnknown || subsampling == Subsampling::kUnknown) return { 0, 0 };
  if (planeIndex < 0 || planeIndex >= planesOf(config)) return { 0, 0 };

  // Only a plane holding chroma is subsampled. Y and A are always full resolution, and a chroma plane is only
  // subsamplable when it is not sharing with Y.
  if (!planeHoldsChroma(config, planeIndex)) return { 1, 1 };
  if (!chromaIsSeparate(config)) return { 0, 0 };

  const auto [x, y] = factorsOf(subsampling);
  return { x, y };
}

int YUVAInfo::NumPlanes(const PlaneConfig config) noexcept { return planesOf(config); }

int YUVAInfo::NumChannelsInPlane(const PlaneConfig config, const int i) noexcept { return channelsInPlane(config, i); }

bool YUVAInfo::HasAlpha(const PlaneConfig config) noexcept {
  switch (config) {
    case PlaneConfig::kY_U_V_A:
    case PlaneConfig::kY_V_U_A:
    case PlaneConfig::kY_UV_A:
    case PlaneConfig::kY_VU_A:
    case PlaneConfig::kYUVA:
    case PlaneConfig::kUYVA:
      return true;

    default:
      return false;
  }
}

int YUVAInfo::PlaneDimensions(const Size imageDimensions, const PlaneConfig config, const Subsampling subsampling,
        const EncodedOrigin origin, Size planeDimensions[kMaxPlanes]) noexcept {
  if (planeDimensions == nullptr) return 0;

  const int planeCount = planesOf(config);
  if (planeCount == 0) return 0;

  // An orientation that swaps the axes means the *stored* dimensions are the transpose of the display ones, so the
  // plane sizes are computed against the stored size and the caller's swap is theirs to remember. Getting this wrong
  // produces planes that are the right area and the wrong shape, which shows as a sheared image.
  const bool swapsAxes = origin == EncodedOrigin::kLeftTop || origin == EncodedOrigin::kRightTop ||
                         origin == EncodedOrigin::kRightBottom || origin == EncodedOrigin::kLeftBottom;
  const int storedWidth = swapsAxes ? static_cast<int>(imageDimensions.height)
                                    : static_cast<int>(imageDimensions.width);
  const int storedHeight = swapsAxes ? static_cast<int>(imageDimensions.width)
                                     : static_cast<int>(imageDimensions.height);

  for (int plane = 0; plane < planeCount; ++plane) {
    const auto [factorX, factorY] = PlaneSubsamplingFactors(config, subsampling, plane);
    if (factorX == 0 || factorY == 0) return 0;

    // Chroma is subsampled by taking every `factor`th sample, so the plane is smaller by that factor. The division
    // rounds up, because a block that is only partly covered still needs a sample to describe it.
    const int planeWidth = (storedWidth + factorX - 1) / factorX;
    const int planeHeight = (storedHeight + factorY - 1) / factorY;
    planeDimensions[plane] = Size { static_cast<Scalar>(planeWidth), static_cast<Scalar>(planeHeight) };
  }

  return planeCount;
}

// ==================================================================================================
// One description
// ==================================================================================================

YUVAInfo::YUVAInfo(const Size dimensions, const PlaneConfig planeConfig, const Subsampling subsampling,
        const YUVColorSpace yuvColorSpace, const EncodedOrigin origin, const Siting sitingX, const Siting sitingY)
    : mDimensions(dimensions), mPlaneConfig(planeConfig), mSubsampling(subsampling), mYUVColorSpace(yuvColorSpace),
      mOrigin(origin), mSitingX(sitingX), mSitingY(sitingY) {
  // ## The two ways a description can contradict itself
  //
  // Both are checked here rather than at every use, because a description that cannot be true has no useful
  // interpretation: every accessor would have to decide what to answer, and a caller would have to check each one.
  // Reporting it once, as invalid, is what lets the accessors mean what they say.
  //
  //  1. **Chroma where it cannot be subsampled.** A config that puts Y in the same plane as U and V describes them at
  //     the same resolution, so a subsampling other than 4:4:4 is a contradiction. A file can carry both fields and
  //     disagree with itself, and this is where that is caught.
  //  2. **A size that is not a size.** Empty dimensions describe no image.
  if (mPlaneConfig != PlaneConfig::kUnknown) {
    if (mSubsampling != Subsampling::k444 && !chromaIsSeparate(mPlaneConfig)) {
      mPlaneConfig = PlaneConfig::kUnknown;
    } else if (!(mDimensions.width > 0.0f) || !(mDimensions.height > 0.0f)) {
      mPlaneConfig = PlaneConfig::kUnknown;
    }
  }
}

std::pair<int, int> YUVAInfo::planeSubsamplingFactors(const int planeIndex) const noexcept {
  // The static form answers with a tuple, which is what its signature declares; this one answers with a pair, which is
  // what a caller destructuring `auto [x, y]` wants and what the two-element case really is. The conversion is here,
  // once, rather than being left to the compiler to accept or refuse depending on its permissiveness -- which is
  // exactly what happened: the looser build took it and the stricter one did not.
  const auto [x, y] = PlaneSubsamplingFactors(mPlaneConfig, mSubsampling, planeIndex);
  return { x, y };
}

int YUVAInfo::planeDimensions(Size planeDimensions[kMaxPlanes]) const noexcept {
  return PlaneDimensions(mDimensions, mPlaneConfig, mSubsampling, mOrigin, planeDimensions);
}

size_t YUVAInfo::computeTotalBytes(const size_t rowBytes[kMaxPlanes], size_t planeSizes[kMaxPlanes]) const {
  if (rowBytes == nullptr) return 0;

  const int planeCount = planesOf(mPlaneConfig);
  if (planeCount == 0) return 0;

  Size dimensions[kMaxPlanes] {};
  if (PlaneDimensions(mDimensions, mPlaneConfig, mSubsampling, mOrigin, dimensions) != planeCount) return 0;

  constexpr size_t kOverflow = std::numeric_limits<size_t>::max();
  size_t total = 0;

  for (int plane = 0; plane < planeCount; ++plane) {
    const auto height = static_cast<size_t>(dimensions[plane].height);

    // ## What can and cannot be checked about a row length
    //
    // A row length is in *bytes*, and this function does not know a plane's bytes per sample -- the caller supplies
    // the row lengths and nothing supplies the format. So a row cannot be compared against the plane's width in
    // samples: the first version of this did exactly that and refused a perfectly good 64-sample plane whose rows
    // were 64 bytes of 16-bit samples.
    //
    // What can be established is that the row is not empty, and that the total does not overflow. A row too short for
    // the plane it describes is a caller's error that this cannot detect, and the honest position is to say so rather
    // than to half-check it: a check that fires on correct input is worse than no check, because it sends a caller
    // looking for a bug in the wrong place.
    if (rowBytes[plane] == 0) {
      // The whole array is filled, not just the planes this description has. A caller that sizes its own loop by
      // `kMaxPlanes` -- which is the obvious thing to do with a fixed-size array -- would otherwise read a stale zero
      // from the fourth slot and believe that plane existed and was empty.
      if (planeSizes != nullptr) {
        for (int i = 0; i < kMaxPlanes; ++i) planeSizes[i] = kOverflow;
      }
      return kOverflow;
    }

    // Multiplication that reports overflow rather than wrapping, because a wrapped size is a small number that a
    // caller would allocate happily.
    if (height != 0 && rowBytes[plane] > kOverflow / height) {
      // The whole array is filled, not just the planes this description has. A caller that sizes its own loop by
      // `kMaxPlanes` -- which is the obvious thing to do with a fixed-size array -- would otherwise read a stale zero
      // from the fourth slot and believe that plane existed and was empty.
      if (planeSizes != nullptr) {
        for (int i = 0; i < kMaxPlanes; ++i) planeSizes[i] = kOverflow;
      }
      return kOverflow;
    }
    const size_t planeSize = rowBytes[plane] * height;
    if (planeSizes != nullptr) planeSizes[plane] = planeSize;

    if (planeSize > kOverflow - total) {
      // The whole array is filled, not just the planes this description has. A caller that sizes its own loop by
      // `kMaxPlanes` -- which is the obvious thing to do with a fixed-size array -- would otherwise read a stale zero
      // from the fourth slot and believe that plane existed and was empty.
      if (planeSizes != nullptr) {
        for (int i = 0; i < kMaxPlanes; ++i) planeSizes[i] = kOverflow;
      }
      return kOverflow;
    }
    total += planeSize;
  }

  return total;
}

int YUVAInfo::numPlanes() const noexcept { return planesOf(mPlaneConfig); }

int YUVAInfo::numChannelsInPlane(const int i) const noexcept { return channelsInPlane(mPlaneConfig, i); }

YUVAInfo YUVAInfo::makeSubsampling(const Subsampling subsampling) const {
  YUVAInfo result = *this;
  result.mSubsampling = subsampling;

  // The constructor is the one place that decides whether a combination is coherent, so it is asked rather than
  // duplicating the rule. A result that cannot be expressed becomes invalid, which is the documented behaviour.
  if (subsampling != Subsampling::k444 && !chromaIsSeparate(result.mPlaneConfig)) {
    result.mPlaneConfig = PlaneConfig::kUnknown;
  }
  return result;
}

YUVAInfo YUVAInfo::makeDimensions(const Size dimensions) const {
  YUVAInfo result = *this;
  result.mDimensions = dimensions;
  if (!(dimensions.width > 0.0f) || !(dimensions.height > 0.0f)) {
    result.mPlaneConfig = PlaneConfig::kUnknown;
  }
  return result;
}

bool operator==(const YUVAInfo& lhs, const YUVAInfo& rhs) noexcept {
  return lhs.mDimensions == rhs.mDimensions && lhs.mPlaneConfig == rhs.mPlaneConfig &&
         lhs.mSubsampling == rhs.mSubsampling && lhs.mYUVColorSpace == rhs.mYUVColorSpace &&
         lhs.mOrigin == rhs.mOrigin && lhs.mSitingX == rhs.mSitingX && lhs.mSitingY == rhs.mSitingY;
}

bool operator!=(const YUVAInfo& lhs, const YUVAInfo& rhs) noexcept { return !(lhs == rhs); }

// ==================================================================================================
// Enum names
// ==================================================================================================

const char* toString(const EncodedOrigin origin) noexcept {
  switch (origin) {
    case EncodedOrigin::kTopLeft: return "TopLeft";
    case EncodedOrigin::kTopRight: return "TopRight";
    case EncodedOrigin::kBottomRight: return "BottomRight";
    case EncodedOrigin::kBottomLeft: return "BottomLeft";
    case EncodedOrigin::kLeftTop: return "LeftTop";
    case EncodedOrigin::kRightTop: return "RightTop";
    case EncodedOrigin::kRightBottom: return "RightBottom";
    case EncodedOrigin::kLeftBottom: return "LeftBottom";
  }
  return "Unknown EncodedOrigin";
}

const char* toString(const YUVColorSpace space) noexcept {
  switch (space) {
    case YUVColorSpace::kIdentity: return "Identity";
    case YUVColorSpace::kJPEG: return "JPEG";
    case YUVColorSpace::kRec601: return "Rec601";
    case YUVColorSpace::kRec709: return "Rec709";
    case YUVColorSpace::kRec2020: return "Rec2020";
    case YUVColorSpace::kBT2020ConstantLuminance: return "BT2020ConstantLuminance";
  }
  return "Unknown YUVColorSpace";
}

} // namespace graphics
