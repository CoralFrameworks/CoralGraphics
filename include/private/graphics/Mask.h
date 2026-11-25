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

#ifndef CORAL_PRIVATE_GRAPHICS_MASK_H
#define CORAL_PRIVATE_GRAPHICS_MASK_H

#include <geometry/Rect.h>
#include <geometry/Scalar.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * @file Mask.h
 *
 * An 8-bit coverage bitmap, and why it is separate from a `Pixmap`.
 *
 * A mask is what a shape contributes to a pixel before any colour is involved: a single number per pixel saying how
 * much of it the shape covers. It is not a colour and not premultiplied, and the operations that matter for it --
 * blurring, thresholding, offsetting -- are meaningless for a colour bitmap. Keeping the two types apart is what lets
 * a mask filter be written without thinking about channels.
 *
 * This header is private to the project: it is included by the mask filter that produces one and by the canvas that
 * consumes one, and it is not installed.
 *
 * ## Coverage at the edges
 *
 * A blurred mask is larger than the shape that produced it, so a mask has a *safe* rectangle: the region within which
 * a sample is meaningful. Outside it the coverage is implicitly zero, which is what lets a blur read past the edge
 * without a special case -- the kernel simply gets nothing from there.
 */

namespace graphics::mask {

/**
 * A coverage bitmap.
 *
 * Deliberately a plain struct with a vector rather than a class with invariants: it is built once, read once and
 * thrown away, and every operation on it is a loop over the same buffer. The only invariant worth keeping is that
 * `coverage` has exactly `rowBytes * height` entries, and `allocate` is the only way to make one.
 */
struct Mask {
  /** The coverage values, row by row, each row `rowBytes` long. */
  std::vector<uint8_t> coverage;

  /** The width in pixels of a row of the buffer, which may exceed the mask's own width. */
  size_t rowBytes = 0;

  int width = 0;
  int height = 0;

  /** Where the mask's top-left corner sits in the coordinate space it was made from. */
  int originX = 0;
  int originY = 0;

  /** @return true when there is nothing to sample. */
  [[nodiscard]] bool isEmpty() const noexcept { return width <= 0 || height <= 0 || coverage.empty(); }

  /** @return the coverage at @p x, @p y in the mask's own coordinates, or zero outside it. */
  [[nodiscard]] uint8_t at(const int x, const int y) const noexcept {
    if (x < 0 || y < 0 || x >= width || y >= height) return 0;
    return coverage[static_cast<size_t>(y) * rowBytes + static_cast<size_t>(x)];
  }

  void set(const int x, const int y, const uint8_t value) noexcept {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    coverage[static_cast<size_t>(y) * rowBytes + static_cast<size_t>(x)] = value;
  }

  /**
   * @return a mask of @p width by @p height, all zero, positioned at @p originX, @p originY.
   *
   * The row length is the width rounded up to a multiple of four. Not an alignment requirement of this code -- the
   * buffer is a `vector` and is read a byte at a time -- but it keeps a row from having an odd length, which is what a
   * later vectorised pass would need and what costs nothing now.
   */
  [[nodiscard]] static Mask allocate(int width, int height, int originX = 0, int originY = 0) {
    Mask result;
    if (width <= 0 || height <= 0) return result;

    result.width = width;
    result.height = height;
    result.originX = originX;
    result.originY = originY;
    result.rowBytes = (static_cast<size_t>(width) + 3u) & ~size_t{ 3u };
    result.coverage.assign(result.rowBytes * static_cast<size_t>(height), 0);
    return result;
  }

  /** @return the absolute bounds this mask covers. */
  [[nodiscard]] Rect bounds() const noexcept {
    const auto left = static_cast<Scalar>(originX);
    const auto top = static_cast<Scalar>(originY);
    return Rect::MakeLTRB(left, top, left + static_cast<Scalar>(width), top + static_cast<Scalar>(height));
  }
};

/**
 * @return @p mask with a Gaussian of standard deviation @p sigma applied.
 *
 * ## How it is done
 *
 * As two one-dimensional passes, horizontal then vertical. A two-dimensional Gaussian is the product of two
 * one-dimensional ones, so this is exact rather than an approximation -- and it costs `2 * radius` reads per pixel
 * instead of `radius * radius`, which for a blur of any size is the difference between usable and not.
 *
 * ## The edges
 *
 * Samples outside the mask are zero, and the weights of the taps that fell outside are simply not added. That makes
 * the result *darker* at the edges than a version that renormalised -- and that is the correct behaviour: a shape's
 * coverage genuinely falls off at its edge, and renormalising would invent coverage that is not there. The caller
 * that wants the blur not to reach the edge pads the mask first.
 *
 * The radius is three standard deviations, which is where a Gaussian has fallen to about a hundredth of its peak and
 * beyond which the taps round to zero at eight bits.
 */
[[nodiscard]] Mask blur(const Mask& source, Scalar sigma);

/** @return @p mask with the given coverage added to every sample, saturated. */
[[nodiscard]] Mask added(const Mask& first, const Mask& second);

/** @return @p mask made solid: samples at or above @p minimum become fully covered, the rest become empty. */
[[nodiscard]] Mask thresholded(const Mask& source, uint8_t minimum);

/**
 * @return the part of @p first that @p second does not cover, sample by sample.
 *
 * Used for the outer blur style, where what is drawn is the blurred shape minus the shape itself.
 */
[[nodiscard]] Mask subtracted(const Mask& first, const Mask& second);

/**
 * @return @p first intersected with @p second, sample by sample.
 *
 * Used for the inner blur style. Both masks must be in the same coordinate space; the result takes the first's.
 */
[[nodiscard]] Mask intersected(const Mask& first, const Mask& second);

/**
 * @return @p mask grown by @p radius pixels on every side, with the original copied into the middle.
 *
 * The padding a blur needs so that its kernel has something to read where the shape's coverage stops. Not part of
 * the blur itself, because whether to pad is a decision about what the caller wants at the edge -- a soft falloff, or
 * a hard cut.
 */
[[nodiscard]] Mask padded(const Mask& source, int radius);

} // namespace graphics::mask

#endif //CORAL_PRIVATE_GRAPHICS_MASK_H
