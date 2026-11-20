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

#include <graphics/MaskFilter.h>

#include <private/graphics/MaskFilterInternal.h>

#include <cmath>

namespace graphics {

struct MaskFilter::Impl {};

MaskFilter::~MaskFilter() = default;

MaskFilter::MaskFilter(MaskFilter&&) noexcept = default;
MaskFilter& MaskFilter::operator=(MaskFilter&&) noexcept = default;

std::shared_ptr<MaskFilter> MaskFilter::Blur(const Style style, const Scalar sigma) {
  // Refused rather than clamped or treated as a pass-through. A caller that reached here with a non-positive or
  // non-finite sigma has a scale or a radius computed wrong upstream, and a filter that quietly did nothing would
  // send it looking at the drawing code instead.
  if (!std::isfinite(sigma) || !(sigma > 0.0f)) return nullptr;

  auto filter = std::shared_ptr<MaskFilter>(new MaskFilter());
  filter->mStyle = style;
  filter->mSigma = sigma;
  return filter;
}

Scalar MaskFilter::margin() const noexcept {
  // Three standard deviations, matching the radius the blur itself uses. Kept in one place conceptually even though
  // the two are computed separately, because a margin smaller than the blur's reach would clip the spread.
  return std::ceil(mSigma * 3.0f);
}

/**
 * @return @p source with @p filter applied.
 *
 * The four styles, each expressed as a combination of the blurred mask and the sharp one. Kept out of the public
 * header because it works in `mask::Mask`, which is a private type -- and kept in this file because it is the whole of
 * what a mask filter *does*, with the class around it being only the parameters.
 *
 * ## Each style, in terms of the two masks
 *
 * Writing `S` for the sharp coverage and `B` for the blurred one:
 *
 *  - **Normal** is `B`. The shape's edges are softened and nothing else changes.
 *  - **Solid** is `S` plus the part of `B` that lies outside it. The inside stays fully covered, so the shape reads
 *    as solid, and only the outside is soft -- which is what distinguishes it from Normal on a thin shape, where
 *    normal blurring makes the whole thing semi-transparent and this does not.
 *  - **Outer** is `B` minus `S`: only what spilled beyond the shape. This is the glow, and with the drawing offset,
 *    the drop shadow.
 *  - **Inner** is `S` minus the part of `B` that lies *inside* it, which is the same as the blurred mask restricted
 *    to the shape. It darkens the interior near the edge and leaves everything outside alone.
 */
mask::Mask mask_filter_detail::apply(const MaskFilter& filter, const mask::Mask& sharp) {
  if (sharp.isEmpty()) return sharp;

  // Padded before blurring, so the kernel has something to read where the coverage stops and the spread is not cut
  // off square. The padding is what `margin` is for, and the result carries the larger bounds so a caller composites
  // it in the right place.
  const int radius = static_cast<int>(std::lround(filter.margin()));
  const mask::Mask padded = mask::padded(sharp, radius);
  const mask::Mask blurred = mask::blur(padded, filter.sigma());

  switch (filter.style()) {
    case MaskFilter::Style::kNormal:
      return blurred;

    case MaskFilter::Style::kSolid: {
      // The sharp shape again, at the blurred mask's bounds, then added. `added` canvasses onto the union, so the
      // inside is filled in and the outside keeps the blur.
      const mask::Mask inside = mask::thresholded(sharp, 1);
      return mask::added(blurred, inside);
    }

    case MaskFilter::Style::kOuter:
      // What the blur put outside the shape. Subtracting the sharp coverage from the blurred one gives exactly that,
      // and the sampler reads zero outside the padded frame, so the subtraction cannot go negative.
      return mask::subtracted(blurred, sharp);

    case MaskFilter::Style::kInner:
      // The blur, restricted to where the shape is. `intersected` takes the smaller of the two, so coverage outside
      // the shape disappears and the interior keeps the blurred falloff.
      return mask::intersected(blurred, sharp);
  }

  return blurred;
}

} // namespace graphics
