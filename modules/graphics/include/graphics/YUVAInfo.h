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

#ifndef CORAL_GRAPHICS_YUVA_INFO_H
#define CORAL_GRAPHICS_YUVA_INFO_H

#include <graphics/Enums.h>

#include <geometry/Scalar.h>
#include <geometry/Size.h>

#include <cstdint>
#include <tuple>
#include <utility>

namespace graphics {

/**
 * How a YUV (and optionally A) image is divided among planes.
 *
 * ## What this describes, and what it does not
 *
 * The pixels are not here; where they are kept -- a texture, a pixmap, four buffers -- is the caller's business. This
 * is the *shape* of them: which channels live together, how large each plane is, and what the numbers in it mean.
 * Everything that has to read the pixels needs this first, which is why it is a plain value that can be copied and
 * compared rather than something that owns anything.
 *
 * ## The two axes of the description
 *
 * They are independent and both are needed:
 *
 *  - **PlaneConfig** says which channels share a plane. A camera's output is usually three planes, one of them luma and
 *    one holding both chroma channels interleaved; a graphics API's surface is usually one plane holding all four.
 *  - **Subsampling** says whether the chroma is at a lower resolution than the luma. It only applies where chroma is
 *    in a plane of its own, because a plane cannot be two resolutions at once -- so a config that puts Y and UV
 *    together is only valid at 4:4:4, and the constructor refuses the combination rather than describing something
 *    impossible.
 *
 * ## Widths and heights are `Scalar`
 *
 * The rest of this library uses `Size` with `Scalar` components, and this follows: a plane's dimensions are whole
 * numbers, but a description that had its own integer size type would not be usable with the geometry the rest of the
 * layer works in. The values are always integral, and `PlaneDimensions` produces them by whole-number division.
 */
struct YUVAInfo {
  /** The four channels a plane can hold, in the order they appear in `PlaneConfig`'s names. */
  enum Channels : uint8_t {
    kY = 0,
    kU = 1,
    kV = 2,
    kA = 3,

    kLast = kA,
  };

  static constexpr int kChannelCount = static_cast<int>(Channels::kLast) + 1;

  /** The most planes a description can have. */
  static constexpr int kMaxPlanes = 4;

  /**
   * How the channels are divided among planes.
   *
   * Underscores in the names separate one plane from the next, and the letters within a name are the channels in that
   * plane in order. So `kY_U_V` is three planes of one channel each, and `kY_UV` is a luma plane followed by a plane
   * holding U in its first channel and V in its second.
   */
  enum class PlaneConfig : uint8_t {
    /** Not a description. The value an invalid `YUVAInfo` carries. */
    kUnknown,

    kY_U_V,  ///< Three planes: Y, then U, then V.
    kY_V_U,  ///< Three planes: Y, then V, then U. The channel swap matters, not just the count.
    kY_UV,   ///< Two planes: Y, then interleaved UV.
    kY_VU,   ///< Two planes: Y, then interleaved VU.
    kYUV,    ///< One plane holding Y, U and V.
    kUYV,    ///< One plane holding U, Y and V.

    kY_U_V_A,  ///< Four planes: Y, U, V, A.
    kY_V_U_A,  ///< Four planes: Y, V, U, A.
    kY_UV_A,   ///< Three planes: Y, interleaved UV, A.
    kY_VU_A,   ///< Three planes: Y, interleaved VU, A.
    kYUVA,     ///< One plane holding all four channels.
    kUYVA,     ///< One plane holding all four channels, in the order U, Y, V, A.

    kLast = kUYVA,
  };

  /**
   * How much lower the chroma resolution is than the luma's, in the J:a:b notation.
   *
   * `k420` is one chroma sample per 2x2 block of luma samples, which is what most consumer video uses; `k444` is no
   * subsampling at all. The chroma is only ever subsampled where it is in its own plane -- see the class comment.
   */
  enum class Subsampling : uint8_t {
    kUnknown,

    k444,  ///< No subsampling: a chroma sample for every luma sample.
    k422,  ///< One chroma sample per 2x1 block of luma.
    k420,  ///< One per 2x2.
    k440,  ///< One per 1x2.
    k411,  ///< One per 4x1.
    k410,  ///< One per 4x2.

    kLast = k410,
  };

  /**
   * Where a subsampled chroma sample sits relative to the luma samples it covers.
   *
   * Only the centred siting is defined, because it is the only one in practical use: the chroma sample is treated as
   * describing the middle of its block. The type is here rather than being a comment so that a second siting can be
   * added without changing every caller.
   */
  enum class Siting : uint8_t {
    /** The chroma value describes the centre of the luma block it covers. */
    kCentered,
  };

  /**
   * @param dimensions the size of the full-resolution image, after any orientation has been applied.
   * @param planeConfig how the channels are divided among planes.
   * @param subsampling how much lower the chroma resolution is.
   * @param yuvColorSpace the coefficients the chroma was derived with.
   * @param origin the stored orientation, which the plane sizes are computed against.
   *
   * A combination that cannot be true -- chroma subsampled where it shares a plane with luma, or empty dimensions --
   * produces an invalid description rather than an exception. Invalid is the value's own state, reported by
   * `isValid`, and every accessor means what it says when the description is valid.
   */
  YUVAInfo(Size dimensions, PlaneConfig planeConfig, Subsampling subsampling, YUVColorSpace yuvColorSpace,
          EncodedOrigin origin = EncodedOrigin::kTopLeft, Siting sitingX = Siting::kCentered,
          Siting sitingY = Siting::kCentered);

  /** An invalid description, which is what a default-constructed one is. */
  YUVAInfo() = default;

  [[nodiscard]] PlaneConfig planeConfig() const noexcept { return mPlaneConfig; }
  [[nodiscard]] Subsampling subsampling() const noexcept { return mSubsampling; }
  [[nodiscard]] YUVColorSpace yuvColorSpace() const noexcept { return mYUVColorSpace; }
  [[nodiscard]] EncodedOrigin origin() const noexcept { return mOrigin; }
  [[nodiscard]] Siting sitingX() const noexcept { return mSitingX; }
  [[nodiscard]] Siting sitingY() const noexcept { return mSitingY; }

  /** The size of the full-resolution image, after orientation. */
  [[nodiscard]] Size dimensions() const noexcept { return mDimensions; }

  [[nodiscard]] Scalar width() const noexcept { return mDimensions.width; }
  [[nodiscard]] Scalar height() const noexcept { return mDimensions.height; }

  /** @return true when this describes an image, which is false for a default-constructed one. */
  [[nodiscard]] bool isValid() const noexcept { return mPlaneConfig != PlaneConfig::kUnknown; }

  /** @return the number of planes. */
  [[nodiscard]] int numPlanes() const noexcept;

  /** @return the number of channels in plane @p planeIndex, or zero when the index is out of range. */
  [[nodiscard]] int numChannelsInPlane(int planeIndex) const noexcept;

  /** @return true when the description includes an alpha channel. */
  [[nodiscard]] bool hasAlpha() const noexcept { return HasAlpha(mPlaneConfig); }

  /** @return how much lower plane @p planeIndex's resolution is, as (x, y) factors. */
  [[nodiscard]] std::pair<int, int> planeSubsamplingFactors(int planeIndex) const noexcept;

  /**
   * @param planeDimensions receives one entry per plane, in the order the planes are stored *in memory*.
   * @return the number of planes written, or zero when the description is invalid.
   *
   * The dimensions are the stored ones, before the orientation is undone. A caller that wants the sizes as displayed
   * has to apply the orientation itself -- which is the point of separating them: the pixels are stored one way and
   * shown another, and code that reads the pixels needs the first.
   */
  int planeDimensions(Size planeDimensions[kMaxPlanes]) const noexcept;

  /**
   * @param rowBytes the row length of each plane, from the caller.
   * @param planeSizes when non-null, receives the byte size of each plane.
   * @return the total bytes the planes need, or `SIZE_MAX` when the request cannot be satisfied.
   *
   * `SIZE_MAX` rather than a smaller number, and every entry of @p planeSizes set to it as well: a caller that
   * allocated a wrapped size would write out of bounds, so the failure has to be unmistakable rather than plausible.
   * Both the multiplications and the sum are checked for overflow.
   *
   * A row shorter than a plane's width is refused for the same reason: the rows would overlap, and the plane would not
   * be the shape the description claims.
   */
  [[nodiscard]] size_t computeTotalBytes(const size_t rowBytes[kMaxPlanes],
          size_t planeSizes[kMaxPlanes] = nullptr) const;

  /**
   * @return the same description with a different chroma subsampling.
   *
   * An invalid result when @p subsampling cannot apply to this plane layout, which is the case whenever chroma shares a
   * plane with luma and the request is not 4:4:4. Reporting an invalid description is better than ignoring the request
   * or than changing the plane layout to accommodate it -- the first would silently do nothing and the second would
   * describe an image the caller does not have.
   */
  [[nodiscard]] YUVAInfo makeSubsampling(Subsampling subsampling) const;

  /** @return the same description with different dimensions, or an invalid one when they are empty. */
  [[nodiscard]] YUVAInfo makeDimensions(Size dimensions) const;

  // ================================================================================================
  // The description of a layout, without an image
  // ================================================================================================

  /** @return the number of planes @p config describes. */
  [[nodiscard]] static int NumPlanes(PlaneConfig config) noexcept;

  /** @return the number of channels in plane @p planeIndex of @p config, or zero when out of range. */
  [[nodiscard]] static int NumChannelsInPlane(PlaneConfig config, int planeIndex) noexcept;

  /** @return true when @p config has an alpha channel. */
  [[nodiscard]] static bool HasAlpha(PlaneConfig config) noexcept;

  /** @return how much lower the chroma resolution is under @p subsampling, as (x, y) factors. */
  [[nodiscard]] static std::tuple<int, int> SubsamplingFactors(Subsampling subsampling);

  /**
   * @return how much lower plane @p planeIndex's resolution is under @p config and @p subsampling.
   *
   * `(0, 0)` for a combination that cannot be true, rather than `(1, 1)`. The difference matters to a caller that
   * divides: a factor of one produces a plane that is silently the wrong size, and a factor of zero is caught.
   */
  [[nodiscard]] static std::tuple<int, int> PlaneSubsamplingFactors(
          PlaneConfig config, Subsampling subsampling, int planeIndex);

  /**
   * @param planeDimensions receives one entry per plane.
   * @return the number of planes written, or zero when the description is invalid.
   *
   * The static form, for a caller that has the components and not a `YUVAInfo`.
   */
  static int PlaneDimensions(Size imageDimensions, PlaneConfig planeConfig, Subsampling subsampling,
          EncodedOrigin origin, Size planeDimensions[kMaxPlanes]) noexcept;

private:
  // The equality is a free function so that it can live in the implementation file -- it compares seven fields, and a
  // caller that only needs `isValid` should not pay to have it inlined into every translation unit that includes this.
  friend bool operator==(const YUVAInfo& lhs, const YUVAInfo& rhs) noexcept;

  Size mDimensions { 0.0f, 0.0f };
  PlaneConfig mPlaneConfig = PlaneConfig::kUnknown;
  Subsampling mSubsampling = Subsampling::kUnknown;
  YUVColorSpace mYUVColorSpace = YUVColorSpace::kIdentity;
  EncodedOrigin mOrigin = EncodedOrigin::kTopLeft;
  Siting mSitingX = Siting::kCentered;
  Siting mSitingY = Siting::kCentered;
};

/** @return true when the two descriptions are the same in every field, invalid ones included. */
[[nodiscard]] bool operator==(const YUVAInfo& lhs, const YUVAInfo& rhs) noexcept;

/** @return true when the two descriptions differ in any field. */
[[nodiscard]] bool operator!=(const YUVAInfo& lhs, const YUVAInfo& rhs) noexcept;

} // namespace graphics

#endif //CORAL_GRAPHICS_YUVA_INFO_H
