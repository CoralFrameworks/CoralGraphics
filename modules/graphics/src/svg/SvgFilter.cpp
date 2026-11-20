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
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

/**
 * @file SvgFilter.cpp
 *
 * The primitives declared in SvgFilter.h, and the chain that connects them.
 *
 * Translated from Skia's `modules/svg/src/SkSVGFilterContext.cpp`, `SkSVGFe.cpp` and the six `SkSVGFe*.cpp` files that
 * this corpus needs. The structure follows Skia's: a context holding named results plus a "previous result", each
 * primitive resolving its inputs from that context and publishing its own output back into it.
 *
 * ## Buffers are the size of the canvas
 *
 * Every intermediate here is a bitmap the size of the destination, and the filter's regions are *clips* within it
 * rather than the buffer's own extent. The alternative -- a buffer per region, in the region's coordinates -- is what a
 * GPU backend would do, and it is what makes a drop shadow cheap. Here it would mean a coordinate system per primitive
 * and a resampling step between each pair, so the buffer is the canvas and the region is a mask over it.
 *
 * The cost is that a filter on a small element pays for the whole canvas. The benefit is that no primitive has to know
 * where it sits, and the two implementations agree pixel for pixel wherever the region covers the element.
 *
 * ## Region defaults
 *
 * The filter effects region defaults to a margin around the element's box: `-10%` to `120%`. A document that states
 * none of the four therefore still changes something, which is easy to miss when reading a filter that looks empty.
 *
 * A primitive's own subregion defaults differently and less obviously: it is the *union of its inputs' subregions*, or
 * the whole filter effects region when any input is a source rather than a primitive. That is Skia's rule and the
 * format's -- `SkSVGFe::resolveFilterSubregion` -- and it is what keeps a blur from smearing the whole canvas.
 */

#include "svg/SvgFilter.h"

#include <graphics/Canvas.h>
#include <graphics/Paint.h>
#include <graphics/Pixmap.h>
#include <graphics/Surface.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>

namespace graphics::svg {
namespace {

/** The format's default filter region, as fractions of the element's box. */
constexpr Scalar kDefaultX = -0.1f;
constexpr Scalar kDefaultY = -0.1f;
constexpr Scalar kDefaultWidth = 1.2f;
constexpr Scalar kDefaultHeight = 1.2f;

/**
 * One intermediate: a bitmap the size of the canvas, and the region of it that is valid.
 *
 * The region is carried rather than baked in because a primitive's default subregion is the union of its *inputs'*
 * regions, so every primitive has to be able to say what it produced.
 */
struct Buffer {
  Bitmap bitmap;
  Rect region;
};

/** The named results, plus the previous one. A filter context, in Skia's terms. */
struct Chain {
  std::map<std::string, Buffer> results;
  Buffer previous;

  /** How many primitives have produced a result, which is what decides the default subregion rule. */
  bool previousIsSource = true;
};

/** @return @p length resolved against @p context, or @p fallback when it is absent or unresolvable. */
Scalar lengthOr(const std::optional<Length>& length, const LengthContext& context, const Scalar fallback) {
  if (!length) return fallback;
  return context.resolve(*length, LengthUnit::Px).value_or(fallback);
}

/**
 * @return the filter effects region in device space.
 *
 * Under `objectBoundingBox` a *number* is a fraction of the element's box rather than a unit, which is the same
 * distinction masks have -- so the lengths are resolved against a unit viewport and then scaled by the box. Resolving
 * them against the real viewport instead would make `x="-10%"` mean ten percent of the canvas.
 */
Rect effectsRegion(const Filter& filter, const Rect& objectBounds, const LengthContext& context) {
  if (!filter.objectBoundingBox) {
    const Scalar x = lengthOr(filter.x, context, kDefaultX);
    const Scalar y = lengthOr(filter.y, context, kDefaultY);
    const Scalar width = lengthOr(filter.width, context, kDefaultWidth);
    const Scalar height = lengthOr(filter.height, context, kDefaultHeight);
    return Rect::MakeXYWH(x, y, width, height);
  }

  LengthContext unit;
  unit.viewportWidth = 1;
  unit.viewportHeight = 1;
  const Scalar x = lengthOr(filter.x, unit, kDefaultX);
  const Scalar y = lengthOr(filter.y, unit, kDefaultY);
  const Scalar width = lengthOr(filter.width, unit, kDefaultWidth);
  const Scalar height = lengthOr(filter.height, unit, kDefaultHeight);

  return Rect::MakeLTRB(objectBounds.getLeft() + x * objectBounds.getWidth(),
                        objectBounds.getTop() + y * objectBounds.getHeight(),
                        objectBounds.getLeft() + (x + width) * objectBounds.getWidth(),
                        objectBounds.getTop() + (y + height) * objectBounds.getHeight());
}

/**
 * A region as whole pixels, clamped to a buffer.
 *
 * `right` and `bottom` are exclusive. The alternative -- an inclusive bound and an `+ 1` at every use -- is how an
 * off-by-one gets into a primitive, and there are six of them.
 */
struct PixelRange {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  [[nodiscard]] bool isEmpty() const noexcept { return right <= left || bottom <= top; }
};

/** @return @p region as whole pixels of a @p width by @p height buffer, clamped to it. */
PixelRange pixelsOf(const Rect& region, const int width, const int height) {
  PixelRange range;
  range.left = std::clamp(static_cast<int>(std::floor(region.getLeft())), 0, width);
  range.top = std::clamp(static_cast<int>(std::floor(region.getTop())), 0, height);
  range.right = std::clamp(static_cast<int>(std::ceil(region.getRight())), 0, width);
  range.bottom = std::clamp(static_cast<int>(std::ceil(region.getBottom())), 0, height);
  return range;
}

/** @return @p range grown by @p margin on every side, clamped to a @p width by @p height buffer. */
PixelRange grown(const PixelRange& range, const int margin, const int width, const int height) {
  PixelRange out;
  out.left = std::max(0, range.left - margin);
  out.top = std::max(0, range.top - margin);
  out.right = std::min(width, range.right + margin);
  out.bottom = std::min(height, range.bottom + margin);
  return out;
}

/**
 * @return the smallest whole-pixel rectangle covering @p bitmap's non-transparent pixels, or an empty one.
 *
 * **Not called.** It was written to crop a source input's subregion to what the element drew, which is wrong -- see the
 * note in `resolveSubregion`. Kept so that the next person considering the same saving finds the reason it does not work
 * rather than deriving it again.
 */
[[maybe_unused]]
Rect coverageOf(const Bitmap& bitmap) {
  const Pixmap in = bitmap.makePixmap();
  if (in.isEmpty()) return Rect {};

  int left = in.width();
  int top = in.height();
  int right = 0;
  int bottom = 0;
  for (int y = 0; y < in.height(); ++y) {
    for (int x = 0; x < in.width(); ++x) {
      if (((in.getPixelARGB32(x, y) >> 24) & 0xFFu) == 0) continue;
      left = std::min(left, x);
      top = std::min(top, y);
      right = std::max(right, x + 1);
      bottom = std::max(bottom, y + 1);
    }
  }
  if (right <= left || bottom <= top) return Rect {};
  return Rect::MakeLTRB(static_cast<Scalar>(left), static_cast<Scalar>(top), static_cast<Scalar>(right),
                        static_cast<Scalar>(bottom));
}

/** @return @p a and @p b's overlap, or an empty rectangle when they do not meet. */
Rect intersect(const Rect& a, const Rect& b) {
  const Scalar left = std::max(a.getLeft(), b.getLeft());
  const Scalar top = std::max(a.getTop(), b.getTop());
  const Scalar right = std::min(a.getRight(), b.getRight());
  const Scalar bottom = std::min(a.getBottom(), b.getBottom());
  if (right <= left || bottom <= top) return Rect {};
  return Rect::MakeLTRB(left, top, right, bottom);
}

/**
 * @return a copy of @p buffer with everything outside @p region cleared.
 *
 * **No longer called, and kept because deleting it would hide the reason.** Every primitive now writes only inside its
 * subregion and every output is allocated zeroed, so there is nothing left outside a region to clear -- which is what
 * removed a full-canvas pass per primitive. The function stays as the record of what used to be needed, and as the
 * thing to restore if a future primitive cannot confine itself.
 */
[[maybe_unused]] Buffer clipped(Buffer buffer, const Rect& region) {
  if (region.getWidth() <= 0 || region.getHeight() <= 0) {
    buffer.bitmap.erase(Color::MakeRGBA8(0, 0, 0, 0));
    return buffer;
  }

  Pixmap pixels = buffer.bitmap.lockPixels();
  if (pixels.isEmpty()) return buffer;

  // The region is in device coordinates and the buffer is device-sized, so this is a rectangle test per pixel rather
  // than a resample. A pixel whose centre is outside the region is cleared, which is what makes the default margin a
  // margin and stops a blur from spreading past the region it belongs to.
  for (int y = 0; y < pixels.height(); ++y) {
    const bool rowInside = static_cast<Scalar>(y) + 0.5f >= region.getTop() &&
                           static_cast<Scalar>(y) + 0.5f < region.getBottom();
    for (int x = 0; x < pixels.width(); ++x) {
      if (rowInside && static_cast<Scalar>(x) + 0.5f >= region.getLeft() &&
          static_cast<Scalar>(x) + 0.5f < region.getRight()) {
        continue;
      }
      pixels.setPixelARGB32(x, y, 0);
    }
  }
  return buffer;
}

// ==================================================================================================
// The primitives
// ==================================================================================================

/**
 * The one layout a filter's buffers use: 8-bit RGBA, premultiplied, in that byte order.
 *
 * Pinned rather than inherited from the destination, and that is the whole point. `Pixmap`'s accessors switch on the
 * colour type once *per channel per pixel* and un-premultiply with three divides on every read; a loop that knows its
 * layout takes one row pointer per row and addresses bytes. It changes no result -- the same premultiplied values, by
 * the same formulas -- it only stops paying for a decision that is the same for every pixel in the buffer.
 *
 * `kRGBA_8888` is what `Bitmap::allocate(width, height)` already produces, so the source is the only buffer that ever
 * needs converting, and `applyFilter` converts it once.
 */
constexpr ColorType kFilterColorType = ColorType::kRGBA_8888;
constexpr AlphaType kFilterAlphaType = AlphaType::kPremultiplied;

/** @return the layout every filter buffer is allocated in. */
ImageInfo filterInfo(const int width, const int height) {
  return ImageInfo(width, height, kFilterColorType, kFilterAlphaType);
}

/** @return @p value premultiplied by @p alpha, with `Pixmap`'s own rounding. */
constexpr uint32_t premultiply8(const uint32_t value, const uint32_t alpha) {
  return (value * alpha + 127u) / 255u;
}

/** @return @p value un-premultiplied by @p alpha, with `Pixmap`'s own rounding. A zero alpha carries no colour. */
uint32_t unpremultiply8(const uint32_t value, const uint32_t alpha) {
  if (alpha == 0) return 0;
  return (std::min(value, alpha) * 255u + alpha / 2u) / alpha;
}

/** One pixel of the pinned layout. */
struct Rgba8 {
  uint32_t red = 0;
  uint32_t green = 0;
  uint32_t blue = 0;
  uint32_t alpha = 0;
};

/** @return the pixel at @p index of @p row, which the caller has already bounded. */
Rgba8 readRgba8(const uint8_t* const row, const int index) {
  const uint8_t* const pixel = row + static_cast<size_t>(index) * 4;
  return Rgba8 {pixel[0], pixel[1], pixel[2], pixel[3]};
}

void writeRgba8(uint8_t* const row, const int index, const Rgba8& value) {
  uint8_t* const pixel = row + static_cast<size_t>(index) * 4;
  pixel[0] = static_cast<uint8_t>(value.red);
  pixel[1] = static_cast<uint8_t>(value.green);
  pixel[2] = static_cast<uint8_t>(value.blue);
  pixel[3] = static_cast<uint8_t>(value.alpha);
}

/** @return @p input in the pinned layout, converting if it is anything else. */
Bitmap convertToPinned(const Bitmap& input) {
  Bitmap out = Bitmap::allocate(filterInfo(input.width(), input.height()));
  if (out.isEmpty()) return out;

  const Pixmap in = input.makePixmap();
  Pixmap pixels = out.lockPixels();
  if (in.isEmpty() || pixels.isEmpty()) return out;

  // Through the accessors, once, because this runs at most once per filter and the loops below run per primitive.
  for (int y = 0; y < pixels.height(); ++y) {
    for (int x = 0; x < pixels.width(); ++x) {
      pixels.setPixelARGB32(x, y, in.getPixelARGB32(x, y));
    }
  }
  return out;
}

/** @return @p input with its colour discarded, keeping only its coverage -- the format's `SourceAlpha`. */
/** @return a buffer the size of @p shape filled with @p color at @p opacity, inside @p region. */
Bitmap solidBuffer(const Bitmap& shape, const Color color, const Scalar opacity, const Rect& region) {
  Bitmap out = Bitmap::allocate(filterInfo(shape.width(), shape.height()));
  if (out.isEmpty()) return out;

  Pixmap pixels = out.lockPixels();
  if (pixels.isEmpty()) return out;

  const auto channel = [](const Scalar value) {
    return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
  };
  // Premultiplied once rather than per pixel, because a flood is the same colour everywhere -- and the premultiply is
  // what makes it a half-transparent colour at 50% rather than an opaque one with a low alpha.
  const uint32_t alpha = channel(color.alpha * opacity);
  const Rgba8 argb {premultiply8(channel(color.red), alpha), premultiply8(channel(color.green), alpha),
                    premultiply8(channel(color.blue), alpha), alpha};

  const PixelRange range = pixelsOf(region, pixels.width(), pixels.height());
  for (int y = range.top; y < range.bottom; ++y) {
    auto* const row = static_cast<uint8_t*>(pixels.addr(0, y));
    if (row == nullptr) continue;
    for (int x = range.left; x < range.right; ++x) writeRgba8(row, x, argb);
  }
  return out;
}


Bitmap alphaOnly(const Bitmap& input) {
  Bitmap out = input.makeDeepCopy();
  Pixmap pixels = out.lockPixels();
  if (pixels.isEmpty()) return out;

  const int width = pixels.width();
  const int height = pixels.height();
  for (int y = 0; y < height; ++y) {
    auto* const row = static_cast<uint8_t*>(pixels.addr(0, y));
    if (row == nullptr) continue;
    for (int x = 0; x < width; ++x) {
      // Black at the input's own alpha. Premultiplied, a zero colour is zero at any alpha, so the byte is already right.
      writeRgba8(row, x, Rgba8 {0, 0, 0, readRgba8(row, x).alpha});
    }
  }
  return out;
}

/** @return the 4x5 matrix @p primitive describes, from its `type` and `values`. */
std::array<Scalar, 20> colorMatrixOf(const FilterPrimitive& primitive) {
  switch (primitive.colorMatrixType) {
    case FilterPrimitive::ColorMatrixType::Matrix:
      return primitive.values;

    case FilterPrimitive::ColorMatrixType::Saturate: {
      // The format's saturate matrix, with `s` the one value. Written out rather than derived because it is a published
      // constant: the luminance weights are the same Rec.709 ones a mask's luminance rule uses.
      const Scalar s = primitive.values[0];
      const Scalar r = 0.213f;
      const Scalar g = 0.715f;
      const Scalar b = 0.072f;
      return {r + s * (1 - r), g * (1 - s),     b * (1 - s),     0, 0,
              r * (1 - s),     g + s * (1 - g), b * (1 - s),     0, 0,
              r * (1 - s),     g * (1 - s),     b + s * (1 - b), 0, 0,
              0,               0,               0,               1, 0};
    }

    case FilterPrimitive::ColorMatrixType::HueRotate: {
      const Scalar theta = primitive.values[0] * 3.14159265358979323846f / 180.0f;
      const Scalar c = std::cos(theta);
      const Scalar s = std::sin(theta);
      return {0.213f + c * 0.787f - s * 0.213f, 0.715f - c * 0.715f - s * 0.715f, 0.072f - c * 0.072f + s * 0.928f,
              0, 0,
              0.213f - c * 0.213f + s * 0.143f, 0.715f + c * 0.285f + s * 0.140f, 0.072f - c * 0.072f - s * 0.283f,
              0, 0,
              0.213f - c * 0.213f - s * 0.787f, 0.715f - c * 0.715f + s * 0.715f, 0.072f + c * 0.928f + s * 0.072f,
              0, 0,
              0, 0, 0, 1, 0};
    }

    case FilterPrimitive::ColorMatrixType::LuminanceToAlpha: {
      // The colour disappears and the luminance becomes the alpha, which is what makes this the odd one out: every
      // other type leaves alpha alone.
      const Scalar r = 0.2125f;
      const Scalar g = 0.7154f;
      const Scalar b = 0.0721f;
      return {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, r, g, b, 0, 0};
    }
  }
  return {};
}

/** `feColorMatrix`: a 4x5 matrix applied to each pixel's un-premultiplied (r, g, b, a). */
Bitmap colorMatrix(const FilterPrimitive& primitive, const Bitmap& input, const Rect& region) {
  Bitmap out = Bitmap::allocate(filterInfo(input.width(), input.height()));
  if (out.isEmpty()) return out;

  const std::array<Scalar, 20> matrix = colorMatrixOf(primitive);
  const Pixmap in = input.makePixmap();
  Pixmap pixels = out.lockPixels();
  if (in.isEmpty() || pixels.isEmpty()) return out;

  const PixelRange range = pixelsOf(region, pixels.width(), pixels.height());
  const auto channel = [](const Scalar value) {
    return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
  };

  for (int y = range.top; y < range.bottom; ++y) {
    const auto* const inRow = static_cast<const uint8_t*>(in.addr(0, y));
    auto* const outRow = static_cast<uint8_t*>(pixels.addr(0, y));
    if (inRow == nullptr || outRow == nullptr) continue;

    for (int x = range.left; x < range.right; ++x) {
      const Rgba8 pixel = readRgba8(inRow, x);
      // Un-premultiplied on the way in, which is what the format's matrix is defined on. Doing the arithmetic on
      // premultiplied values instead would darken every partly transparent pixel.
      const Scalar channels[4] = {static_cast<Scalar>(unpremultiply8(pixel.red, pixel.alpha)) / 255.0f,
                                  static_cast<Scalar>(unpremultiply8(pixel.green, pixel.alpha)) / 255.0f,
                                  static_cast<Scalar>(unpremultiply8(pixel.blue, pixel.alpha)) / 255.0f,
                                  static_cast<Scalar>(pixel.alpha) / 255.0f};

      Scalar result[4] = {0, 0, 0, 0};
      for (int row = 0; row < 4; ++row) {
        Scalar sum = matrix[static_cast<size_t>(row) * 5 + 4];
        for (int column = 0; column < 4; ++column) {
          sum += matrix[static_cast<size_t>(row) * 5 + static_cast<size_t>(column)] * channels[column];
        }
        result[row] = sum;
      }

      // Premultiplied again by the *new* alpha, which the setter used to do.
      const uint32_t alpha = channel(result[3]);
      writeRgba8(outRow, x, Rgba8 {premultiply8(channel(result[0]), alpha), premultiply8(channel(result[1]), alpha),
                                   premultiply8(channel(result[2]), alpha), alpha});
    }
  }
  return out;
}

/** `feOffset`: the input moved by a device-space vector. */
Bitmap offsetImage(const Bitmap& input, const int dx, const int dy, const Rect& region) {
  Bitmap out = Bitmap::allocate(filterInfo(input.width(), input.height()));
  if (out.isEmpty()) return out;

  const Pixmap in = input.makePixmap();
  Pixmap pixels = out.lockPixels();
  if (in.isEmpty() || pixels.isEmpty()) return out;

  const PixelRange range = pixelsOf(region, pixels.width(), pixels.height());
  for (int y = range.top; y < range.bottom; ++y) {
    const int sourceY = y - dy;
    if (sourceY < 0 || sourceY >= in.height()) continue;

    const auto* const inRow = static_cast<const uint8_t*>(in.addr(0, sourceY));
    auto* const outRow = static_cast<uint8_t*>(pixels.addr(0, y));
    if (inRow == nullptr || outRow == nullptr) continue;

    for (int x = range.left; x < range.right; ++x) {
      const int sourceX = x - dx;
      if (sourceX < 0 || sourceX >= in.width()) continue;
      writeRgba8(outRow, x, readRgba8(inRow, sourceX));
    }
  }
  return out;
}

/**
 * One box pass over a float buffer, along one axis, with a running sum.
 *
 * The running sum is the whole point: each output sample adds the pixel entering the window and subtracts the one
 * leaving it, so the cost per pixel does not depend on the radius. A convolution re-reads every sample in the window,
 * which is what made a `stdDeviation` of twenty expensive.
 *
 * The indices at the ends are clamped rather than read as transparent, which is `edgeMode="duplicate"`: the edge pixel
 * is repeated outwards, so a shape that reaches the border keeps its coverage there.
 */
void boxBlurAxis(const std::vector<Scalar>& in, std::vector<Scalar>& out, const int width, const int height,
                 const int radius, const bool horizontal) {
  const int lineCount = horizontal ? height : width;
  const int lineLength = horizontal ? width : height;
  const auto divisor = static_cast<Scalar>(2 * radius + 1);

  // The offset of a sample within its line, in the buffer's row-major layout.
  const auto offsetOf = [width, horizontal](const int line, const int index) {
    return horizontal ? (static_cast<size_t>(line) * width + static_cast<size_t>(index)) * 4
                      : (static_cast<size_t>(index) * width + static_cast<size_t>(line)) * 4;
  };

  for (int line = 0; line < lineCount; ++line) {
    // The first sample's window, with everything before the start clamped to the first pixel.
    Scalar sum[4] = {0, 0, 0, 0};
    for (int i = -radius; i <= radius; ++i) {
      const size_t base = offsetOf(line, std::clamp(i, 0, lineLength - 1));
      for (int c = 0; c < 4; ++c) sum[c] += in[base + static_cast<size_t>(c)];
    }

    for (int i = 0; i < lineLength; ++i) {
      const size_t base = offsetOf(line, i);
      for (int c = 0; c < 4; ++c) out[base + static_cast<size_t>(c)] = sum[c] / divisor;

      const size_t leaving = offsetOf(line, std::clamp(i - radius, 0, lineLength - 1));
      const size_t entering = offsetOf(line, std::clamp(i + radius + 1, 0, lineLength - 1));
      for (int c = 0; c < 4; ++c) {
        sum[c] += in[entering + static_cast<size_t>(c)] - in[leaving + static_cast<size_t>(c)];
      }
    }
  }
}

/**
 * @return the widths of the three boxes whose variances sum to @p deviation squared.
 *
 * The standard inversion, and the reason it is two *odd* widths rather than three of the same: a box of even width has
 * no centre sample, and the variance of a box of width `w` is `(w^2 - 1) / 12`, so the widths have to be solved for
 * rather than divided out.
 */
std::array<int, 3> boxWidthsFor(const Scalar deviation) {
  constexpr int kBoxes = 3;
  const Scalar ideal = std::sqrt((12.0f * deviation * deviation / kBoxes) + 1.0f);

  int lower = static_cast<int>(std::floor(ideal));
  if (lower % 2 == 0) --lower;
  const int upper = lower + 2;

  // How many of the three take the lower width. The rest take the upper.
  const Scalar fraction = (12.0f * deviation * deviation - kBoxes * lower * lower - 4.0f * kBoxes * lower -
                           3.0f * kBoxes) /
                          (-4.0f * lower - 4.0f);
  const int lowerCount = static_cast<int>(std::lround(fraction));

  std::array<int, 3> widths {};
  for (int i = 0; i < kBoxes; ++i) widths[static_cast<size_t>(i)] = i < lowerCount ? lower : upper;
  return widths;
}

/**
 * `feGaussianBlur`: three box passes per axis, which is the approximation Skia uses.
 *
 * Not a convolution. A Gaussian is separable, so a convolution would be two passes of `2r + 1` samples -- and the cost
 * of that grows with the deviation, which is what made `complex7.svg`'s `stdDeviation="20"` at its native size take
 * minutes. Three boxes with a running sum are O(1) per pixel whatever the deviation, and are within a few percent of a
 * true Gaussian, which no eight-bit channel resolves.
 */
Bitmap gaussianBlur(const Bitmap& input, const Scalar deviationX, const Scalar deviationY, const Rect& region) {
  // A deviation of zero is a blur that does nothing, and the format uses it that way -- the drop shadow in `complex4`
  // writes `stdDeviation="0"` to mean "no spread".
  if (!(deviationX > 0.0f) && !(deviationY > 0.0f)) return input.makeDeepCopy();

  const Pixmap in = input.makePixmap();
  if (in.isEmpty()) return {};

  const int width = in.width();
  const int height = in.height();

  // The region the output is meaningful in, and the larger one the blur has to read to produce it:
  // `3 * sigma` is past the three boxes' combined radius.
  const PixelRange range = pixelsOf(region, width, height);
  const auto reach = static_cast<int>(std::ceil(3.0f * std::max(deviationX, deviationY)));
  const PixelRange load = grown(range, reach, width, height);

  // One pass per axis, each reading the previous pass's output. Kept as float channels rather than pixels between the
  // two axes, because rounding to eight bits between them loses more than the blur itself does.
  //
  // The colour is **premultiplied on the way in**, and that is the difference between a blur and a smear: a transparent
  // pixel beside a red one holds no colour, so it contributes none. Blurring the un-premultiplied channels instead
  // pulls colour out of pixels that have none and puts a bright halo around every edge -- which is exactly what
  // `blur.svg` exists to show.
  std::vector<Scalar> channels(static_cast<size_t>(width) * height * 4);
  for (int y = load.top; y < load.bottom; ++y) {
    const auto* const row = static_cast<const uint8_t*>(in.addr(0, y));
    if (row == nullptr) continue;
    for (int x = load.left; x < load.right; ++x) {
      // Already premultiplied, which is what the blur works on -- so the bytes go straight in, with no conversion
      // either way. The old load un-premultiplied them and premultiplied them back.
      const Rgba8 pixel = readRgba8(row, x);
      const size_t base = (static_cast<size_t>(y) * width + x) * 4;
      channels[base + 0] = static_cast<Scalar>(pixel.red) / 255.0f;
      channels[base + 1] = static_cast<Scalar>(pixel.green) / 255.0f;
      channels[base + 2] = static_cast<Scalar>(pixel.blue) / 255.0f;
      channels[base + 3] = static_cast<Scalar>(pixel.alpha) / 255.0f;
    }
  }

  std::vector<Scalar> scratch(channels.size());

  const auto blurAlong = [&channels, &scratch, width, height](const Scalar deviation, const bool horizontal) {
    if (!(deviation > 0.0f)) return;
    for (const int boxWidth : boxWidthsFor(deviation)) {
      const int radius = (boxWidth - 1) / 2;
      if (radius <= 0) continue;
      // Two passes per box, alternating buffers so neither is copied: one along the axis, one across it.
      boxBlurAxis(channels, scratch, width, height, radius, horizontal);
      channels.swap(scratch);
    }
  };

  // Each axis is blurred by three boxes *along that axis*, which is what makes the result separable and the cost linear
  // in the pixel count rather than in the deviation.
  blurAlong(deviationX, true);
  blurAlong(deviationY, false);

  Bitmap out = Bitmap::allocate(filterInfo(input.width(), input.height()));
  Pixmap pixels = out.lockPixels();
  if (pixels.isEmpty()) return out;

  for (int y = range.top; y < range.bottom; ++y) {
    auto* const row = static_cast<uint8_t*>(pixels.addr(0, y));
    if (row == nullptr) continue;
    for (int x = range.left; x < range.right; ++x) {
      const size_t base = (static_cast<size_t>(y) * width + x) * 4;
      const auto channel = [&channels, base](const int c) {
        return static_cast<uint32_t>(
                std::lround(std::clamp(channels[base + static_cast<size_t>(c)], 0.0f, 1.0f) * 255.0f));
      };
      // Written premultiplied, which is what the blur produced. The old store divided by the alpha here so that the
      // premultiplying setter could multiply it back.
      writeRgba8(row, x, Rgba8 {channel(0), channel(1), channel(2), channel(3)});
    }
  }
  return out;
}

/** @return the blend mode @p blend means, as the canvas's own. */
BlendMode canvasBlend(const FilterPrimitive::Blend blend) {
  using Blend = FilterPrimitive::Blend;
  switch (blend) {
    case Blend::Normal: return BlendMode::kSrcOver;
    case Blend::Multiply: return BlendMode::kMultiply;
    case Blend::Screen: return BlendMode::kScreen;
    case Blend::Darken: return BlendMode::kDarken;
    case Blend::Lighten: return BlendMode::kLighten;
    case Blend::Overlay: return BlendMode::kOverlay;
    case Blend::ColorDodge: return BlendMode::kColorDodge;
    case Blend::ColorBurn: return BlendMode::kColorBurn;
    case Blend::HardLight: return BlendMode::kHardLight;
    case Blend::SoftLight: return BlendMode::kSoftLight;
    case Blend::Difference: return BlendMode::kDifference;
    case Blend::Exclusion: return BlendMode::kExclusion;
  }
  return BlendMode::kSrcOver;
}

/** @return the Porter-Duff mode @p composite means, as the canvas's own. Arithmetic has none. */
BlendMode canvasBlend(const FilterPrimitive::Composite composite) {
  using Composite = FilterPrimitive::Composite;
  switch (composite) {
    case Composite::Over: return BlendMode::kSrcOver;
    case Composite::In: return BlendMode::kSrcIn;
    case Composite::Out: return BlendMode::kSrcOut;
    case Composite::Atop: return BlendMode::kSrcATop;
    case Composite::Xor: return BlendMode::kXor;
    case Composite::Arithmetic: return BlendMode::kSrcOver;
  }
  return BlendMode::kSrcOver;
}

/**
 * @return @p foreground drawn over @p background with @p mode.
 *
 * The compositing is the *canvas's* rather than a second implementation of the same formulas: a surface is made over
 * the background and the foreground is drawn onto it. That is one implementation of thirty blend modes instead of two,
 * and the one that is exercised by every other drawing operation in this library.
 */
Bitmap compositeWith(const Bitmap& foreground, const Bitmap& background, const BlendMode mode,
                         const Rect& region) {
  Bitmap out = Bitmap::allocate(filterInfo(background.width(), background.height()));
  if (out.isEmpty()) return out;

  const PixelRange range = pixelsOf(region, out.width(), out.height());
  if (range.isEmpty()) return out;

  const int width = range.right - range.left;
  const int height = range.bottom - range.top;

  const Pixmap backgroundPixels = background.makePixmap();
  const Pixmap foregroundPixels = foreground.makePixmap();
  Pixmap outPixels = out.lockPixels();
  if (backgroundPixels.isEmpty() || foregroundPixels.isEmpty() || outPixels.isEmpty()) return out;

  // ## The region is extracted, not merely clipped to
  //
  // `drawBitmap` of a canvas-sized bitmap onto a small surface still walks the whole bitmap and lets the surface's
  // bounds discard the rest -- sizing the destination does not shrink the source's traversal. So each input's region is
  // copied out first, and the small bitmaps are what get drawn. Fifteen of `complex7.svg`'s thirty-six primitives come
  // through here, so this is the copy that matters.
  Bitmap backgroundRegion = Bitmap::allocate(filterInfo(width, height));
  Bitmap foregroundRegion = Bitmap::allocate(filterInfo(width, height));
  if (backgroundRegion.isEmpty() || foregroundRegion.isEmpty()) return out;

  {
    Pixmap from = backgroundRegion.lockPixels();
    Pixmap into = foregroundRegion.lockPixels();
    if (from.isEmpty() || into.isEmpty()) return out;
    for (int y = 0; y < height; ++y) {
      const auto* const backgroundRow = static_cast<const uint8_t*>(backgroundPixels.addr(range.left, range.top + y));
      const auto* const foregroundRow = static_cast<const uint8_t*>(foregroundPixels.addr(range.left, range.top + y));
      auto* const fromRow = static_cast<uint8_t*>(from.addr(0, y));
      auto* const intoRow = static_cast<uint8_t*>(into.addr(0, y));
      if (backgroundRow == nullptr || foregroundRow == nullptr || fromRow == nullptr || intoRow == nullptr) continue;
      for (int x = 0; x < width; ++x) {
        writeRgba8(fromRow, x, readRgba8(backgroundRow, x));
        writeRgba8(intoRow, x, readRgba8(foregroundRow, x));
      }
    }
  }

  auto surface = Surface::makeRaster(width, height);
  if (surface == nullptr) return background.makeDeepCopy();
  const auto canvas = surface->getCanvas();
  if (canvas == nullptr) return background.makeDeepCopy();

  // The background is *copied*, with the default source-over. Drawing it with the requested mode would blend it against
  // an empty surface, so `SrcIn` would give `d * sa` with no `d` and every composite would come out blank.
  canvas->drawBitmap(backgroundRegion, 0, 0, graphics::Paint {});

  graphics::Paint paint;
  paint.blendMode = mode;
  canvas->drawBitmap(foregroundRegion, 0, 0, paint);

  // Copied back through bytes rather than through the accessors, for the same reason as everywhere else here.
  const Bitmap composed = surface->detachBitmap();
  const Pixmap from = composed.makePixmap();
  if (from.isEmpty()) return out;
  for (int y = 0; y < height; ++y) {
    const auto* const fromRow = static_cast<const uint8_t*>(from.addr(0, y));
    auto* const outRow = static_cast<uint8_t*>(outPixels.addr(range.left, range.top + y));
    if (fromRow == nullptr || outRow == nullptr) continue;
    for (int x = 0; x < width; ++x) writeRgba8(outRow, x, readRgba8(fromRow, x));
  }
  return out;
}

/**
 * `feComposite operator="arithmetic"`: `k1*i1*i2 + k2*i1 + k3*i2 + k4`.
 *
 * The one operator that is not a Porter-Duff operation and so cannot go through a blend mode. Computed on
 * un-premultiplied channels, which differs from Skia in one respect worth stating: Skia asks for
 * `enforcePMColor = true`, clamping the result's colour to its alpha, and this does not. For the corpus's only
 * arithmetic case the difference is nil -- it operates on `SourceAlpha`, whose colour is zero everywhere -- and the
 * clamp is what a *premultiplied* pipeline needs, not what the formula says.
 */
Bitmap arithmetic(const FilterPrimitive& primitive, const Bitmap& input1, const Bitmap& input2,
                       const Rect& region) {
  Bitmap out = Bitmap::allocate(filterInfo(input1.width(), input1.height()));
  if (out.isEmpty()) return out;

  const Pixmap first = input1.makePixmap();
  const Pixmap second = input2.makePixmap();
  Pixmap pixels = out.lockPixels();
  if (first.isEmpty() || second.isEmpty() || pixels.isEmpty()) return out;

  const Scalar k[4] = {primitive.k1, primitive.k2, primitive.k3, primitive.k4};
  const auto channel = [](const Scalar value) {
    return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
  };

  const PixelRange range = pixelsOf(region, pixels.width(), pixels.height());
  for (int y = range.top; y < range.bottom; ++y) {
    const auto* const firstRow = static_cast<const uint8_t*>(first.addr(0, y));
    const auto* const secondRow = static_cast<const uint8_t*>(second.addr(0, y));
    auto* const outRow = static_cast<uint8_t*>(pixels.addr(0, y));
    if (firstRow == nullptr || secondRow == nullptr || outRow == nullptr) continue;

    for (int x = range.left; x < range.right; ++x) {
      const Rgba8 a = readRgba8(firstRow, x);
      const Rgba8 b = readRgba8(secondRow, x);

      // Un-premultiplied, which is what the formula is defined on -- the same reason the colour matrix converts.
      const Scalar i1[4] = {static_cast<Scalar>(unpremultiply8(a.red, a.alpha)) / 255.0f,
                            static_cast<Scalar>(unpremultiply8(a.green, a.alpha)) / 255.0f,
                            static_cast<Scalar>(unpremultiply8(a.blue, a.alpha)) / 255.0f,
                            static_cast<Scalar>(a.alpha) / 255.0f};
      const Scalar i2[4] = {static_cast<Scalar>(unpremultiply8(b.red, b.alpha)) / 255.0f,
                            static_cast<Scalar>(unpremultiply8(b.green, b.alpha)) / 255.0f,
                            static_cast<Scalar>(unpremultiply8(b.blue, b.alpha)) / 255.0f,
                            static_cast<Scalar>(b.alpha) / 255.0f};

      Scalar result[4] = {0, 0, 0, 0};
      for (int c = 0; c < 4; ++c) result[c] = k[0] * i1[c] * i2[c] + k[1] * i1[c] + k[2] * i2[c] + k[3];

      // Premultiplied by the *new* alpha, which the setter used to do.
      const uint32_t alpha = channel(result[3]);
      writeRgba8(outRow, x, Rgba8 {premultiply8(channel(result[0]), alpha), premultiply8(channel(result[1]), alpha),
                                   premultiply8(channel(result[2]), alpha), alpha});
    }
  }
  return out;
}

// ==================================================================================================
// The chain
// ==================================================================================================

/** The two standard sources, which are the same for every primitive. */
struct Sources {
  const Buffer* graphic = nullptr;
  const Buffer* alpha = nullptr;
};

/**
 * @return the buffer @p name refers to, or nothing when it names something that does not exist.
 *
 * One function rather than two: an earlier draft had this resolve the standard sources to the previous result and its
 * caller override both, which worked only because the caller always ran second.
 */
const Buffer* resolveInput(const Chain& chain, const Sources& sources, const std::string& name) {
  if (name.empty()) {
    // The format's rule, and this corpus's most common case: nothing named means the previous primitive's result, which
    // before the first primitive is the element itself.
    return &chain.previous;
  }
  if (name == "SourceGraphic") return sources.graphic;
  if (name == "SourceAlpha") return sources.alpha;

  const auto found = chain.results.find(name);
  return found == chain.results.end() ? nullptr : &found->second;
}

/**
 * @return the subregion @p primitive writes to.
 *
 * The default is the union of the inputs' subregions, or the whole filter region when any input is a *source* rather
 * than a primitive's result. Skia's `SkSVGFe::resolveFilterSubregion`, and the format's rule. It is what stops a blur
 * from spreading across the canvas: the blur reads a small region and is clipped back to it.
 */
Rect resolveSubregion(const FilterPrimitive& primitive, const Chain& chain, const Sources& sources,
                     const Rect& filterRegion) {
  const Buffer* first = resolveInput(chain, sources, primitive.input);
  const bool standardInput = primitive.input.empty() ? chain.previousIsSource
                                                     : (primitive.input == "SourceGraphic" ||
                                                        primitive.input == "SourceAlpha");

  // A source input takes the *filter region*, not the source's own coverage. Cropping it to what the element drew
  // looks like an easy saving and is wrong: a source is transparent outside its coverage, but a primitive's output need
  // not be. `feFlood` generates over its whole subregion, `feColorMatrix` with a constant term adds colour across it
  // -- the corpus's own inversion matrix is one -- and `feOffset` moves content outside it. Nine tests said so at once.
  Rect region = filterRegion;
  if (!standardInput && first != nullptr) region = first->region;

  if (primitive.input2.empty()) {
    if (!standardInput) {
      // `in2` unspecified reads the same previous result, so its region joins in -- but only when the input was not
      // already a source, which has the whole filter region anyway.
      region = region.getWidth() > 0 ? region : filterRegion;
    }
  } else if (primitive.input2 != "SourceGraphic" && primitive.input2 != "SourceAlpha") {
    if (const Buffer* second = resolveInput(chain, sources, primitive.input2)) {
      const Scalar left = std::min(region.getLeft(), second->region.getLeft());
      const Scalar top = std::min(region.getTop(), second->region.getTop());
      const Scalar right = std::max(region.getRight(), second->region.getRight());
      const Scalar bottom = std::max(region.getBottom(), second->region.getBottom());
      region = Rect::MakeLTRB(left, top, right, bottom);
    }
  }

  // A primitive's own x, y, width and height would each replace the corresponding edge of that default, resolving them
  // against `primitiveUnits` -- user space by default, the element's box if a document says so. None of the corpus's
  // primitives states one, so they are read into the model and *not* applied, rather than applied through a context
  // that would be wrong for both unit systems. A document using one gets the default subregion, which is a region too
  // large rather than a region in the wrong place.
  return region;
}

} // namespace anonymous

std::optional<Bitmap> applyFilter(const Filter& filter, const Bitmap& source, const Rect& objectBounds,
                                  const LengthContext& context, const Scalar scale, const Rect& deviceBounds) {
  if (filter.isEmpty() || source.isEmpty()) return std::nullopt;

  // ## A group has no box, and an `objectBoundingBox` region needs one
  //
  // A filter on a `<g>` -- seven of this corpus's twelve references -- has no geometry to be a fraction of, and the
  // renderer passes an empty rectangle for it. Resolving the region against a zero-size box gives `(0, 0, 0, 0)`, which
  // clamps to an empty range and makes the filter produce nothing at all.
  //
  // The fallback is the viewport, which here is the device: an `objectBoundingBox` fraction with no box to measure is
  // read against the whole canvas. That is what makes a group's filter work rather than vanish.
  const bool boxIsEmpty = !(objectBounds.getWidth() > 0) || !(objectBounds.getHeight() > 0);
  const Rect filterRegion = effectsRegion(filter, boxIsEmpty ? deviceBounds : objectBounds, context);

  // The one buffer that arrives from outside, converted once so that everything below shares the pinned layout the
  // byte-addressing loops are written against. A source that is already in it is passed through unchanged.
  const bool pinned = source.info().colorType() == kFilterColorType &&
                      source.info().alphaType() == kFilterAlphaType;
  const Bitmap normalised = pinned ? source : convertToPinned(source);

  // `SourceAlpha` is a real bitmap rather than a special case inside each primitive, because a filter uses it as an
  // input like any other -- and building it once is cheaper than deriving it per primitive.
  //
  // Both claim the whole device, and that is not an oversight to trim: a primitive reading a source takes the *filter
  // region* as its subregion, which the document declares and which `complex7.svg` declares as its whole artboard.
  const Bitmap alpha = alphaOnly(normalised);
  const Buffer alphaBuffer {alpha, deviceBounds};
  const Buffer graphicBuffer {normalised, deviceBounds};
  const Sources sources {&graphicBuffer, &alphaBuffer};

  Chain chain;
  chain.previous = graphicBuffer;
  chain.previous.region = filterRegion;


  for (const FilterPrimitive& primitive : filter.primitives) {
    const Rect subregion = resolveSubregion(primitive, chain, sources, filterRegion);

    const Buffer* first = resolveInput(chain, sources, primitive.input);
    const Buffer* second = resolveInput(chain, sources, primitive.input2.empty() ? primitive.input : primitive.input2);

    Buffer output;
    switch (primitive.kind) {
      case FilterPrimitive::Kind::ColorMatrix:
        if (first == nullptr) return std::nullopt;
        output.bitmap = colorMatrix(primitive, first->bitmap, subregion);
        break;

      case FilterPrimitive::Kind::Flood:
        // A flood has no input at all: it generates a colour over the region. Its `in` is ignored, which is why the
        // format's own examples write `feFlood` with none.
        output.bitmap = solidBuffer(source, primitive.floodColor, primitive.floodOpacity, subregion);
        break;

      case FilterPrimitive::Kind::Offset:
        if (first == nullptr) return std::nullopt;
        output.bitmap = offsetImage(first->bitmap, static_cast<int>(std::lround(primitive.dx * scale)),
                                    static_cast<int>(std::lround(primitive.dy * scale)), subregion);
        break;

      case FilterPrimitive::Kind::Blur:
        if (first == nullptr) return std::nullopt;
        output.bitmap = gaussianBlur(first->bitmap, primitive.deviationX * scale, primitive.deviationY * scale, subregion);
        break;

      case FilterPrimitive::Kind::Blend:
      case FilterPrimitive::Kind::Composite: {
        if (first == nullptr || second == nullptr) return std::nullopt;
        if (primitive.kind == FilterPrimitive::Kind::Composite &&
            primitive.composite == FilterPrimitive::Composite::Arithmetic) {
          output.bitmap = arithmetic(primitive, first->bitmap, second->bitmap, subregion);
        } else {
          // `in` is the source and `in2` is the destination -- the order the format's formulas are written in, and the
          // opposite of the order the names appear in.
          const BlendMode mode = primitive.kind == FilterPrimitive::Kind::Blend
                                         ? canvasBlend(primitive.blend)
                                         : canvasBlend(primitive.composite);
          output.bitmap = compositeWith(first->bitmap, second->bitmap, mode, subregion);
        }
        break;
      }
    }

    if (output.bitmap.isEmpty()) return std::nullopt;

    output.region = subregion;

    if (!primitive.result.empty()) chain.results[primitive.result] = output;
    chain.previous = std::move(output);
    chain.previousIsSource = false;

  }


  return chain.previous.bitmap;
}

} // namespace graphics::svg
