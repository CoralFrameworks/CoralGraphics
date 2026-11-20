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

#include <private/graphics/Mask.h>

#include <cmath>
#include <vector>

/**
 * @file Mask.cpp
 *
 * Blurring and combining coverage masks.
 *
 * ## Rounding
 *
 * Every operation here works in integers at the end, because a mask is eight bits per sample and that is what a
 * caller reads. The accumulators are integers too -- a kernel's weights scaled by a power of two -- rather than
 * floating point, so that the same input gives the same output on every machine and the result does not depend on the
 * order the taps happened to be added in. A blur that rounded differently per platform would make a test that
 * compared two renders fail on one platform and pass on another, which is a class of bug worth designing out.
 */

namespace graphics::mask {
namespace {

/** The fixed-point scale for kernel weights, so that the arithmetic is integer and reproducible. */
constexpr int kKernelShift = 16;
constexpr int kKernelOne = 1 << kKernelShift;

/**
 * @return the one-dimensional Gaussian weights for @p sigma, scaled by `kKernelOne`.
 *
 * The radius is three standard deviations. At that distance the weight is about 0.01 of the peak, which rounds to
 * zero in the eight-bit output -- so taps beyond it would cost time and change nothing.
 */
[[nodiscard]] std::vector<int> gaussianWeights(const Scalar sigma, int& radius) {
  if (!(sigma > 0.0f)) {
    radius = 0;
    return { kKernelOne };
  }

  radius = static_cast<int>(std::ceil(sigma * 3.0f));
  if (radius < 1) radius = 1;

  const double twoSigmaSquared = 2.0 * static_cast<double>(sigma) * static_cast<double>(sigma);

  // The samples of a Gaussian do not sum to one -- its *integral* does -- so they have to be scaled before they are
  // rounded, or the kernel multiplies the coverage by whatever the samples happened to add up to. For a sigma of two
  // and a radius of three standard deviations that is about four, which is not a blur at all: it amplifies every
  // edge and increases the total coverage.
  //
  // This scaling is the whole of the normalisation. The original code skipped it and then added the entire excess to
  // the centre tap, which made the centre a large negative number; the result was a kernel that sharpened rather than
  // softened, and it passed a casual look because the shape still moved a little.
  std::vector<double> samples(static_cast<size_t>(radius) * 2u + 1u);
  double sampleTotal = 0.0;
  for (int i = -radius; i <= radius; ++i) {
    const double weight = std::exp(-static_cast<double>(i * i) / twoSigmaSquared);
    samples[static_cast<size_t>(i + radius)] = weight;
    sampleTotal += weight;
  }

  std::vector<int> weights(samples.size());
  if (!(sampleTotal > 0.0)) {
    weights[static_cast<size_t>(radius)] = kKernelOne;
    return weights;
  }

  const double scale = static_cast<double>(kKernelOne) / sampleTotal;
  int sum = 0;
  for (size_t i = 0; i < samples.size(); ++i) {
    weights[i] = static_cast<int>(std::lround(samples[i] * scale));
    sum += weights[i];
  }

  // What is left over after rounding goes on the centre tap, which is the largest and so the one where a unit is
  // least significant. The kernel now sums to exactly `kKernelOne`, so a region of uniform coverage comes out of the
  // blur unchanged -- a kernel that sums to slightly more than one drifts the whole picture lighter, and the drift
  // is worst where the shape is flat, which is exactly where it shows as a halo.
  weights[static_cast<size_t>(radius)] += kKernelOne - sum;

  return weights;
}

/** @return @p value confined to the range of an eight-bit sample. */
uint8_t toByte(const int value) noexcept {
  if (value <= 0) return 0;
  if (value >= 255) return 255;
  return static_cast<uint8_t>(value);
}

/**
 * @return @p source with one dimension blurred, @p horizontal choosing which.
 *
 * A single pass in one direction. The accumulators are `int` and the result is rounded once, when it is written --
 * rounding at every tap would accumulate the error of `2 * radius` roundings per sample.
 */
[[nodiscard]] Mask blurPass(const Mask& source, const std::vector<int>& weights, const int radius,
        const bool horizontal) {
  Mask result = Mask::allocate(source.width, source.height, source.originX, source.originY);
  if (result.isEmpty()) return result;

  if (horizontal) {
    for (int y = 0; y < source.height; ++y) {
      for (int x = 0; x < source.width; ++x) {
        int accumulator = 0;
        for (int i = -radius; i <= radius; ++i) {
          accumulator += weights[static_cast<size_t>(i + radius)] * source.at(x + i, y);
        }
        result.set(x, y, toByte((accumulator + kKernelOne / 2) >> kKernelShift));
      }
    }
  } else {
    for (int y = 0; y < source.height; ++y) {
      for (int x = 0; x < source.width; ++x) {
        int accumulator = 0;
        for (int i = -radius; i <= radius; ++i) {
          accumulator += weights[static_cast<size_t>(i + radius)] * source.at(x, y + i);
        }
        result.set(x, y, toByte((accumulator + kKernelOne / 2) >> kKernelShift));
      }
    }
  }

  return result;
}

/**
 * @return @p first and @p second combined sample by sample, in the coordinate space of whichever is larger.
 *
 * Both masks are canvassed onto the union of their bounds. Two masks produced by the same shape at different blurs
 * have the same origin, but one of them may have been padded and the other not, so the combination has to be written
 * in terms of the pixels rather than of the indices -- which is what the origin is for.
 */
template <typename Combine>
[[nodiscard]] Mask combine(const Mask& first, const Mask& second, Combine combine) {
  if (first.isEmpty()) return second;
  if (second.isEmpty()) return first;

  const int left = std::min(first.originX, second.originX);
  const int top = std::min(first.originY, second.originY);
  const int right = std::max(first.originX + first.width, second.originX + second.width);
  const int bottom = std::max(first.originY + first.height, second.originY + second.height);

  Mask result = Mask::allocate(right - left, bottom - top, left, top);
  if (result.isEmpty()) return result;

  for (int y = 0; y < result.height; ++y) {
    for (int x = 0; x < result.width; ++x) {
      // Absolute coordinates, then translated into each mask's own. A sample outside a mask is zero coverage, which
      // is what makes the union the right frame: the missing part of either mask genuinely has nothing there.
      const int absoluteX = left + x;
      const int absoluteY = top + y;
      const uint8_t a = first.at(absoluteX - first.originX, absoluteY - first.originY);
      const uint8_t b = second.at(absoluteX - second.originX, absoluteY - second.originY);
      result.set(x, y, combine(a, b));
    }
  }

  return result;
}

} // namespace anonymous

Mask blur(const Mask& source, const Scalar sigma) {
  if (source.isEmpty()) return source;

  int radius = 0;
  const std::vector<int> weights = gaussianWeights(sigma, radius);
  if (radius == 0) return source;

  // Two passes rather than one two-dimensional one: a two-dimensional Gaussian is the product of two one-dimensional
  // ones, so this is exact, and it reads `2 * radius` samples per pixel instead of `radius * radius`.
  return blurPass(blurPass(source, weights, radius, true), weights, radius, false);
}

Mask added(const Mask& first, const Mask& second) {
  return combine(first, second, [](const uint8_t a, const uint8_t b) {
    return toByte(static_cast<int>(a) + static_cast<int>(b));
  });
}

Mask thresholded(const Mask& source, const uint8_t minimum) {
  Mask result = Mask::allocate(source.width, source.height, source.originX, source.originY);
  if (result.isEmpty()) return result;

  for (int y = 0; y < source.height; ++y) {
    for (int x = 0; x < source.width; ++x) {
      const uint8_t value = source.at(x, y);
      // At or above the threshold becomes fully covered; below it becomes empty. This is the shape of the "solid"
      // blur style: wherever the shape is, it is solid, and only the outside keeps its falloff.
      //
      // The first version had this the other way round, which turned a fully covered square into nothing at all --
      // and because the Solid style *adds* the result to the blur, the mistake was invisible on its own: Solid
      // produced exactly what Normal produced, and a test comparing the two was the only thing that could see it.
      result.set(x, y, value >= minimum ? uint8_t { 255 } : uint8_t { 0 });
    }
  }

  return result;
}

Mask subtracted(const Mask& first, const Mask& second) {
  return combine(first, second, [](const uint8_t a, const uint8_t b) {
    return static_cast<uint8_t>(std::max(0, static_cast<int>(a) - static_cast<int>(b)));
  });
}

Mask intersected(const Mask& first, const Mask& second) {
  return combine(first, second, [](const uint8_t a, const uint8_t b) { return std::min(a, b); });
}

Mask padded(const Mask& source, const int radius) {
  if (source.isEmpty() || radius <= 0) return source;

  Mask result = Mask::allocate(source.width + radius * 2, source.height + radius * 2, source.originX - radius,
          source.originY - radius);
  if (result.isEmpty()) return result;

  for (int y = 0; y < source.height; ++y) {
    for (int x = 0; x < source.width; ++x) {
      result.set(x + radius, y + radius, source.at(x, y));
    }
  }

  return result;
}

} // namespace graphics::mask
