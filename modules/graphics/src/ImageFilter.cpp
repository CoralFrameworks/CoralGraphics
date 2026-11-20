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

#include <graphics/ImageFilter.h>

#include <graphics/Bitmap.h>
#include <graphics/Pixmap.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

/**
 * @file ImageFilter.cpp
 *
 * Filtering whole images, and the three things that make it different from filtering a shape's coverage.
 *
 * **Four channels, premultiplied.** A `MaskFilter` convolves one number per pixel; this convolves four, and they are a
 * premultiplied colour rather than four independent quantities. Convolving them independently is nevertheless correct,
 * because a blur is a weighted sum and premultiplied components are linear in the colour -- so blurring the
 * premultiplied values gives the premultiplied values of the blurred colour. It is *not* correct to unpremultiply,
 * blur and premultiply again: that weights each pixel's colour by the inverse of its own alpha, so a nearly
 * transparent pixel with an arbitrary colour would drag that colour into its neighbours.
 *
 * **The frame is fixed.** The result has the source's dimensions. A blur therefore falls off into transparency at the
 * edges rather than the image growing, which is the behaviour a filter used as a paint property needs -- the shape
 * being filtered has a size, and growing it would move everything around it.
 *
 * **Rounding is fixed point.** As in the mask filter, the kernel's weights are integers scaled by a power of two, so
 * the same input gives the same output on every machine. A blur that rounded differently per platform would make a
 * comparison of two renders pass on one and fail on another.
 */

namespace graphics {
namespace {

/** The fixed-point scale for kernel weights, so that the arithmetic is integer and reproducible. */
constexpr int kKernelShift = 16;
constexpr int kKernelOne = 1 << kKernelShift;

/**
 * @return the one-dimensional Gaussian weights for @p sigma, scaled to sum to exactly `kKernelOne`.
 *
 * The samples of a Gaussian do not sum to one -- its integral does -- so they have to be scaled before they are
 * rounded. Skipping that step is the mistake that turns a blur into an amplifier: the taps sum to roughly
 * `sigma * sqrt(2 * pi)`, which for a sigma of two is about five.
 */
[[nodiscard]] std::vector<int> gaussianWeights(const Scalar sigma, int& radius) {
  if (!(sigma > 0.0f)) {
    radius = 0;
    return { kKernelOne };
  }

  radius = static_cast<int>(std::ceil(sigma * 3.0f));
  if (radius < 1) radius = 1;

  const double twoSigmaSquared = 2.0 * static_cast<double>(sigma) * static_cast<double>(sigma);
  std::vector<double> samples(static_cast<size_t>(radius) * 2u + 1u);

  double total = 0.0;
  for (int i = -radius; i <= radius; ++i) {
    samples[static_cast<size_t>(i + radius)] = std::exp(-static_cast<double>(i * i) / twoSigmaSquared);
    total += samples[static_cast<size_t>(i + radius)];
  }

  std::vector<int> weights(samples.size());
  if (!(total > 0.0)) {
    weights[static_cast<size_t>(radius)] = kKernelOne;
    return weights;
  }

  const double scale = static_cast<double>(kKernelOne) / total;
  int sum = 0;
  for (size_t i = 0; i < samples.size(); ++i) {
    weights[i] = static_cast<int>(std::lround(samples[i] * scale));
    sum += weights[i];
  }
  // What rounding left over goes on the centre tap, so the kernel sums to exactly one and a flat area comes out
  // unchanged rather than drifting -- which a blur shows as a halo.
  weights[static_cast<size_t>(radius)] += kKernelOne - sum;
  return weights;
}

/** @return @p value confined to the range of an eight-bit sample. */
uint8_t toByte(const int value) noexcept {
  if (value <= 0) return 0;
  if (value >= 255) return 255;
  return static_cast<uint8_t>(value);
}

/** @return the four premultiplied channels of @p argb, as bytes. */
std::array<uint8_t, 4> channelsOf(const uint32_t argb) noexcept {
  return { static_cast<uint8_t>((argb >> 24) & 0xFFu), static_cast<uint8_t>((argb >> 16) & 0xFFu),
    static_cast<uint8_t>((argb >> 8) & 0xFFu), static_cast<uint8_t>(argb & 0xFFu) };
}

/** @return @p channels packed back into `0xAARRGGBB`. */
uint32_t argbOf(const std::array<uint8_t, 4>& channels) noexcept {
  return (static_cast<uint32_t>(channels[0]) << 24) | (static_cast<uint32_t>(channels[1]) << 16) |
         (static_cast<uint32_t>(channels[2]) << 8) | static_cast<uint32_t>(channels[3]);
}

} // namespace anonymous

ImageFilter::~ImageFilter() = default;

ImageFilter::ImageFilter(ImageFilter&&) noexcept = default;
ImageFilter& ImageFilter::operator=(ImageFilter&&) noexcept = default;

// ==================================================================================================
// Building one
// ==================================================================================================

std::shared_ptr<ImageFilter> ImageFilter::Blur(const Scalar sigmaX, const Scalar sigmaY) {
  if (!std::isfinite(sigmaX) || !std::isfinite(sigmaY)) return nullptr;
  if (!(sigmaX > 0.0f) || !(sigmaY > 0.0f)) return nullptr;

  auto filter = std::shared_ptr<ImageFilter>(new ImageFilter());
  filter->mKind = Kind::kBlur;
  filter->mSigmaX = sigmaX;
  filter->mSigmaY = sigmaY;
  return filter;
}

std::shared_ptr<ImageFilter> ImageFilter::Blur(const Scalar sigma) { return Blur(sigma, sigma); }

std::shared_ptr<ImageFilter> ImageFilter::Offset(const Scalar dx, const Scalar dy) {
  if (!std::isfinite(dx) || !std::isfinite(dy)) return nullptr;

  auto filter = std::shared_ptr<ImageFilter>(new ImageFilter());
  filter->mKind = Kind::kOffset;
  filter->mOffsetX = std::round(dx);
  filter->mOffsetY = std::round(dy);
  return filter;
}

std::shared_ptr<ImageFilter> ImageFilter::Compose(
        const std::shared_ptr<ImageFilter>& outer, const std::shared_ptr<ImageFilter>& inner) {
  if (outer == nullptr) return inner;
  if (inner == nullptr) return outer;

  // Composing with a filter that does nothing returns the other operand *itself*, so a caller comparing handles gets
  // the answer it expects and no stage is spent on a no-op. The same law the colour filter and the path effect
  // satisfy, which is what lets a caller assembling any of the three drop the no-ops the same way.
  if (outer->isIdentity()) return inner;
  if (inner->isIdentity()) return outer;

  auto filter = std::shared_ptr<ImageFilter>(new ImageFilter());
  filter->mKind = Kind::kCompose;
  filter->mOuter = outer;
  filter->mInner = inner;
  return filter;
}

// ==================================================================================================
// Where the result lies
// ==================================================================================================

Rect ImageFilter::filterBounds(const Rect& bounds) const {
  switch (mKind) {
    case Kind::kBlur: {
      // Three standard deviations on each side, where the Gaussian's tails round to zero at eight bits. Report less
      // than the blur reaches and a caller's buffer is too small for the result.
      const Scalar marginX = std::ceil(mSigmaX * 3.0f);
      const Scalar marginY = std::ceil(mSigmaY * 3.0f);
      return Rect::MakeLTRB(bounds.getLeft() - marginX, bounds.getTop() - marginY, bounds.getRight() + marginX,
              bounds.getBottom() + marginY);
    }

    case Kind::kOffset:
      return Rect::MakeLTRB(bounds.getLeft() + mOffsetX, bounds.getTop() + mOffsetY, bounds.getRight() + mOffsetX,
              bounds.getBottom() + mOffsetY);

    case Kind::kCompose:
      // The inner filter's result is what the outer one sees, so the two are applied in that order. The intersection
      // with the original bounds is what the fixed frame means: the result never lies outside the box the caller is
      // going to allocate for it.
      return mOuter->filterBounds(mInner->filterBounds(bounds)).getIntersectionOrEmpty(bounds);
  }
  return bounds;
}

// ==================================================================================================
// Applying one
// ==================================================================================================

Image ImageFilter::applyBlur(const Image& source) const {
  const ImageInfo& info = source.info();
  if (!info.isValid()) return {};

  int radiusX = 0;
  int radiusY = 0;
  const std::vector<int> weightsX = gaussianWeights(mSigmaX, radiusX);
  const std::vector<int> weightsY = gaussianWeights(mSigmaY, radiusY);

  if (radiusX == 0 && radiusY == 0) return source;

  const int width = info.width();
  const int height = info.height();
  const Pixmap sourcePixels = source.makePixmap();
  if (sourcePixels.isEmpty()) return {};

  // The source is read into a flat buffer first, because a pass has to read a neighbourhood and the pixmap's own row
  // length may be padded. One copy up front is cheaper than testing the padding on every tap.
  std::vector<uint32_t> input(static_cast<size_t>(width) * static_cast<size_t>(height));
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      input[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] = sourcePixels.getPixelARGB32(x, y);
    }
  }

  std::vector<uint32_t> intermediate(input.size());
  std::vector<uint32_t> output(input.size());

  // Horizontal, then vertical. A two-dimensional Gaussian is the product of two one-dimensional ones, so this is
  // exact -- and it reads `2 * radius` samples per pixel instead of `radius * radius`, which for a blur of any size is
  // the difference between usable and not.
  const auto pass = [&](const std::vector<uint32_t>& from, std::vector<uint32_t>& to, const std::vector<int>& weights,
                        const int radius, const bool horizontal) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        std::array<int, 4> accumulator {};

        for (int i = -radius; i <= radius; ++i) {
          const int sampleX = horizontal ? x + i : x;
          const int sampleY = horizontal ? y : y + i;

          // Outside the image is transparent black, which is what the edge of the picture is. Not a repeat of the
          // edge pixel: that would smear the border colour outwards, and for a shape with transparent surroundings it
          // would produce a visible bright rim.
          if (sampleX < 0 || sampleY < 0 || sampleX >= width || sampleY >= height) continue;

          const int weight = weights[static_cast<size_t>(i + radius)];
          const std::array<uint8_t, 4> channels =
                  channelsOf(from[static_cast<size_t>(sampleY) * static_cast<size_t>(width) +
                                  static_cast<size_t>(sampleX)]);
          for (size_t c = 0; c < 4; ++c) accumulator[c] += weight * channels[c];
        }

        std::array<uint8_t, 4> result {};
        for (size_t c = 0; c < 4; ++c) result[c] = toByte((accumulator[c] + kKernelOne / 2) >> kKernelShift);
        to[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] = argbOf(result);
      }
    }
  };

  pass(input, intermediate, weightsX, radiusX, true);
  pass(intermediate, output, weightsY, radiusY, false);

  // Into a bitmap, which is how an `Image` is made. Rows are tightly packed here, so the bitmap takes them at their
  // natural length.
  Bitmap bitmap = Bitmap::allocate(width, height);
  Pixmap destination = bitmap.makePixmap();
  if (destination.isEmpty()) return {};

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      destination.setPixelARGB32(x, y, output[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)]);
    }
  }

  return Image::fromBitmap(bitmap);
}

Image ImageFilter::applyOffset(const Image& source) const {
  const ImageInfo& info = source.info();
  if (!info.isValid()) return {};

  const auto dx = static_cast<int>(mOffsetX);
  const auto dy = static_cast<int>(mOffsetY);

  const int width = info.width();
  const int height = info.height();

  const Pixmap sourcePixels = source.makePixmap();
  if (sourcePixels.isEmpty()) return {};

  Bitmap bitmap = Bitmap::allocate(width, height);
  Pixmap destination = bitmap.makePixmap();
  if (destination.isEmpty()) return {};

  // Cleared first, because `allocate` returns uninitialised storage and this filter only writes where the source had a
  // pixel. The vacated strip would otherwise hold whatever the allocator happened to leave there -- zeros from a fresh
  // page, and arbitrary colour from recycled memory, which is the kind of bug that appears only after a long run.
  std::memset(destination.addr(0, 0), 0, destination.rowBytes() * static_cast<size_t>(height));

  // The frame is the source's, so a positive offset moves the picture towards the bottom right and the strip left
  // behind is transparent. A version that scrolled the pixels round instead would produce a picture with a wrap-around
  // seam that looks like a bug in the caller's geometry.
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int fromX = x - dx;
      const int fromY = y - dy;
      if (fromX < 0 || fromY < 0 || fromX >= width || fromY >= height) continue;

      destination.setPixelARGB32(x, y, sourcePixels.getPixelARGB32(fromX, fromY));
    }
  }

  return Image::fromBitmap(bitmap);
}

Image ImageFilter::filter(const Image& source) const {
  if (source.isEmpty()) return {};

  switch (mKind) {
    case Kind::kBlur:
      return applyBlur(source);

    case Kind::kOffset:
      return applyOffset(source);

    case Kind::kCompose: {
      // The outer sees the inner's result, so an inner that produced nothing gives nothing -- checked here rather
      // than left for the outer to discover, because a blur of an empty image is still an empty image and there is no
      // reason to walk the whole empty frame to find that out.
      const Image inner = mInner->filter(source);
      if (inner.isEmpty()) return inner;
      return mOuter->filter(inner);
    }
  }

  return {};
}

bool ImageFilter::isIdentity() const {
  switch (mKind) {
    case Kind::kBlur:
      // A sigma that rounds to a radius of zero leaves every pixel alone, which is the only way a blur is the
      // identity -- a genuinely positive sigma changes something, even if a particular image happens to be uniform.
      return static_cast<int>(std::ceil(mSigmaX * 3.0f)) < 1 && static_cast<int>(std::ceil(mSigmaY * 3.0f)) < 1;

    case Kind::kOffset:
      return mOffsetX == 0.0f && mOffsetY == 0.0f;

    case Kind::kCompose:
      return mOuter->isIdentity() && mInner->isIdentity();
  }
  return false;
}

} // namespace graphics
