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

#ifndef CORAL_GRAPHICS_MASK_FILTER_H
#define CORAL_GRAPHICS_MASK_FILTER_H

#include <geometry/Scalar.h>

#include <cstdint>
#include <memory>

namespace graphics {

/**
 * A transformation applied to a shape's *coverage* before it is blended.
 *
 * ## How it differs from the other two filters
 *
 * There are three filters in this library and they act at three different stages:
 *
 *  - a **colour filter** changes the value of a pixel;
 *  - a **path effect** changes which shape is drawn, before rasterisation;
 *  - a **mask filter** changes the shape's coverage after rasterisation but before blending.
 *
 * So a mask filter is the only one that can make a shape *soft*. A colour filter cannot: it has one value per pixel
 * and no notion of how much of that pixel the shape covers. A path effect cannot either, for the opposite reason --
 * it decides the geometry, and softness is not a property of geometry.
 *
 * ## What is here
 *
 * The Gaussian blur, in the four styles that decide what happens where the blurred shape and the sharp one overlap.
 * Those four are not variations on a blur; they are four different answers to "what is the shape now", and a caller
 * recognises them immediately: a glow is an outer blur, a drop shadow is an offset outer blur, a soft edge is a
 * normal one, and an inset shadow is an inner one.
 *
 * The emboss and shader variants of the reference are not here. The emboss needs a direction and a lighting model,
 * which is a different kind of operation rather than a variant of a blur.
 */
class MaskFilter {
public:
  ~MaskFilter();

  MaskFilter(const MaskFilter&) = delete;
  MaskFilter& operator=(const MaskFilter&) = delete;
  MaskFilter(MaskFilter&&) noexcept;
  MaskFilter& operator=(MaskFilter&&) noexcept;

  /**
   * What the blur does where it overlaps the shape it came from.
   *
   * The names say what is *solid*: `kSolid` fills the inside, `kOuter` keeps only the outside, and so on. An
   * implementation that got these the wrong way round would produce a picture that is obviously wrong -- a drop shadow
   * that covered the thing casting it -- which is why each is tested against a shape with a known inside.
   */
  enum class Style : uint8_t {
    /** Fuzzy inside and outside: the shape as blurred, with its edges softened. */
    kNormal = 0,
    /** Solid inside, fuzzy outside: the shape filled in, plus the blur's spread beyond it. */
    kSolid,
    /** Nothing inside, fuzzy outside: the glow, or with an offset, the drop shadow. */
    kOuter,
    /** Fuzzy inside, nothing outside: an inner shadow. */
    kInner,

    kLast = kInner,
  };

  /**
   * @param style what the blur does where it overlaps the shape.
   * @param sigma the standard deviation of the Gaussian, in device pixels. Must be greater than zero.
   * @return a blur, or null when @p sigma is not a positive finite number.
   *
   * Zero is refused rather than treated as "no blur": a caller that passed zero has a computation wrong upstream --
   * usually a scale that came out empty -- and a filter that silently did nothing would hide it until the picture
   * looked wrong in a way that pointed at the drawing code instead.
   */
  [[nodiscard]] static std::shared_ptr<MaskFilter> Blur(Style style, Scalar sigma);

  [[nodiscard]] Style style() const noexcept { return mStyle; }
  [[nodiscard]] Scalar sigma() const noexcept { return mSigma; }

  /**
   * @return how far beyond the shape's bounds this filter can put coverage.
   *
   * Three standard deviations, which is where the Gaussian's tails round to zero at eight bits. A caller uses it to
   * size the layer it is about to blur, so that the spread is not clipped off -- a soft shadow cut off square at the
   * edge of a surface is the most visible consequence of getting this wrong.
   */
  [[nodiscard]] Scalar margin() const noexcept;

  struct Impl;

private:
  MaskFilter() = default;

  Style mStyle = Style::kNormal;
  Scalar mSigma = 0.0f;
};

} // namespace graphics

#endif //CORAL_GRAPHICS_MASK_FILTER_H
