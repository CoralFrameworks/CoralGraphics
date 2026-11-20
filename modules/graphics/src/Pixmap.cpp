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
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF
 * ANY KIND, either express or implied. See the License for the specific language governing permissions
 * and limitations under the License.
 */

#include <graphics/Pixmap.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace graphics {
namespace {

constexpr uint32_t kMaxChannel8 = 255;

/**
 * @return the product of two 8-bit values, rescaled to 8 bits with rounding.
 *
 * ## Why this does the whole conversion rather than half of it
 *
 * The arithmetic is simple enough that the interesting part is the mistake it replaces, which survived a full
 * round of tests. An earlier version widened each channel to 16 bits, multiplied, and rescaled -- and then the
 * caller rescaled *again* with the `(v + 128) * 257 >> 16` idiom. The two conversions nearly cancelled: the
 * result came out one part in 259 too large, which for a red of 10 truncates back to 10 and looks perfect. It
 * took a value of 30 to expose it, and the symptom was a colour multiply that appeared to be exactly 3x.
 *
 * There is no precision to be had from a 16-bit intermediate here: both operands are bytes, so the product is
 * exact in 32 bits and one rounding is all that is needed. Doing the whole thing in one place removes the
 * opportunity to apply the scale twice, which is a mistake that is invisible at small numbers and only shows on
 * the bright ones -- the worst property a colour bug can have.
 *
 * `+ 127` rounds to nearest rather than truncating toward zero. Truncation would leave every premultiplied
 * colour slightly dark, and the pairing with the un-premultiply on the way out would not put it back.
 */
constexpr uint32_t premultiply8(const uint32_t value8, const uint32_t alpha8) noexcept {
  return (value8 * alpha8 + 127u) / 255u;
}

/** @return the luminance of an 8-bit colour, weighted as the eye sees it. */
constexpr uint32_t luminance8(const uint32_t r, const uint32_t g, const uint32_t b) noexcept {
  // A plain average would make green and blue equally bright, which is visibly wrong on any saturated
  // colour and is the usual sign that someone converted to grey by averaging.
  return (299u * r + 587u * g + 114u * b + 500u) / 1000u;
}

/** @return true when the format stores an alpha channel. */
bool hasAlphaChannel(const ColorType colorType) noexcept {
  switch (colorType) {
    case ColorType::kRGBA_8888:
    case ColorType::kBGRA_8888:
    case ColorType::kAlpha_8:
      return true;
    default:
      return false;
  }
}

/**
 * @return the channel @p channel of the pixel @p pixel, as 8 bits.
 *
 * `channel` is 0=red, 1=green, 2=blue, 3=alpha in the caller's unpremultiplied terms; this maps that onto
 * each format's memory layout.
 *
 * Formats wider than 8 bits per channel are not converted. They are reported as transparent instead,
 * because a conversion that produced a plausible colour from half the bits would be a bug nobody notices,
 * whereas a hole is obvious. Widening them is real work -- half-float to sRGB is not a shift -- and is
 * better done once, in a conversion routine, than approximately here.
 */
uint32_t readChannel8(const uint8_t* pixel, const ColorType colorType, const int channel) noexcept {
  switch (colorType) {
    case ColorType::kRGBA_8888:
      return pixel[channel];
    case ColorType::kBGRA_8888:
      // B and R are swapped; alpha stays last. Getting this wrong turns red into blue, which is at least
      // conspicuous -- unlike a wrong stride, which is not.
      if (channel == 0) return pixel[2];
      if (channel == 2) return pixel[0];
      return pixel[channel];
    case ColorType::kGray_8:
      return channel < 3 ? pixel[0] : kMaxChannel8;
    case ColorType::kAlpha_8:
      return channel == 3 ? pixel[0] : 0;
    case ColorType::kRGB_565: {
      // 16-bit little-endian: R in the top 5 bits, G in the middle 6, B in the low 5.
      const auto packed = static_cast<uint16_t>(pixel[0] | (pixel[1] << 8));
      switch (channel) {
        case 0: return ((packed >> 11) & 0x1Fu) * kMaxChannel8 / 31u;
        case 1: return ((packed >> 5) & 0x3Fu) * kMaxChannel8 / 63u;
        case 2: return (packed & 0x1Fu) * kMaxChannel8 / 31u;
        default: return kMaxChannel8;  // no alpha channel, so opaque
      }
    }
    default:
      break;
  }
  return 0;
}

/** Writes the four channels into @p pixel, in the layout @p colorType describes. */
void writeChannels8(uint8_t* pixel, const ColorType colorType, const uint32_t r, const uint32_t g, const uint32_t b,
        const uint32_t a) noexcept {
  switch (colorType) {
    case ColorType::kRGBA_8888:
      pixel[0] = static_cast<uint8_t>(r);
      pixel[1] = static_cast<uint8_t>(g);
      pixel[2] = static_cast<uint8_t>(b);
      pixel[3] = static_cast<uint8_t>(a);
      break;
    case ColorType::kBGRA_8888:
      pixel[0] = static_cast<uint8_t>(b);
      pixel[1] = static_cast<uint8_t>(g);
      pixel[2] = static_cast<uint8_t>(r);
      pixel[3] = static_cast<uint8_t>(a);
      break;
    case ColorType::kGray_8:
      pixel[0] = static_cast<uint8_t>(std::min(luminance8(r, g, b), kMaxChannel8));
      break;
    case ColorType::kAlpha_8:
      pixel[0] = static_cast<uint8_t>(a);
      break;
    case ColorType::kRGB_565: {
      // Quantised by rounding rather than truncating, so that white stays white: truncating 255 into 5 bits
      // gives 31 only with luck, and usually gives 30, which darkens every bright pixel by a little.
      const uint32_t r5 = (std::min(r, kMaxChannel8) * 31u + 127u) / 255u;
      const uint32_t g6 = (std::min(g, kMaxChannel8) * 63u + 127u) / 255u;
      const uint32_t b5 = (std::min(b, kMaxChannel8) * 31u + 127u) / 255u;
      const auto packed = static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
      pixel[0] = static_cast<uint8_t>(packed & 0xFFu);
      pixel[1] = static_cast<uint8_t>((packed >> 8) & 0xFFu);
      break;
    }
    default:
      break;
  }
}

} // namespace anonymous

Pixmap::Pixmap(const ImageInfo& info, void* pixels, const size_t rowBytes) noexcept
    : mInfo(info), mPixels(pixels), mRowBytes(rowBytes == 0 ? info.minRowBytes() : rowBytes) {}

Pixmap::Pixmap(const ImageInfo& info, const Data& data) noexcept
    : mInfo(info), mPixels(const_cast<uint8_t*>(data.bytes())), mRowBytes(info.minRowBytes()), mOwner(data) {
  // A `Data` too small for the geometry gets an empty view rather than a short one. Handing back a pixmap
  // claiming a height its buffer cannot hold would make every later bounds check agree with a lie, so the
  // mismatch is resolved here, once, by refusing.
  if (data.size() < info.computeByteSize()) {
    mPixels = nullptr;
  }
}

bool Pixmap::isValid() const noexcept {
  if (mPixels == nullptr) return false;
  if (!mInfo.isValid()) return false;
  if (!isSupportedColorType(mInfo.colorType())) return false;
  if (mRowBytes < mInfo.minRowBytes()) return false;

  // The total extent: the end of the last row. Computed in 64 bits, because a large stride times a large
  // height overflows a 32-bit `size_t`, and the wrap would turn a bounds check into a pass.
  const auto height = static_cast<uint64_t>(mInfo.height());
  const auto stride = static_cast<uint64_t>(mRowBytes);
  const auto lastRow = static_cast<uint64_t>(mInfo.minRowBytes());
  const uint64_t extent = stride * (height - 1) + lastRow;

  return extent <= static_cast<uint64_t>(std::numeric_limits<size_t>::max());
}

void* Pixmap::addr(const int x, const int y) noexcept {
  if (!isValid()) return nullptr;
  if (x < 0 || y < 0 || x >= mInfo.width() || y >= mInfo.height()) return nullptr;
  const auto offset = static_cast<size_t>(y) * mRowBytes + static_cast<size_t>(x) * mInfo.bytesPerPixel();
  return static_cast<uint8_t*>(mPixels) + offset;
}

const void* Pixmap::addr(const int x, const int y) const noexcept {
  return const_cast<Pixmap*>(this)->addr(x, y);
}

Pixmap Pixmap::makeSubset(const Pixmap& parent, const Rect& area) noexcept {
  // Clipped to the parent in whole pixels. The requested area is rounded outward to cover it and then
  // clipped, so a caller asking for a fractional window gets every pixel it touches and none outside the
  // parent's buffer.
  const int left = std::max(0, static_cast<int>(std::floor(area.getLeft())));
  const int top = std::max(0, static_cast<int>(std::floor(area.getTop())));
  const int right = std::min(parent.width(), static_cast<int>(std::ceil(area.getRight())));
  const int bottom = std::min(parent.height(), static_cast<int>(std::ceil(area.getBottom())));

  if (right <= left || bottom <= top) return {};

  auto* base = static_cast<uint8_t*>(parent.mPixels);
  if (base == nullptr) return {};

  const auto offset = static_cast<size_t>(top) * parent.mRowBytes +
                      static_cast<size_t>(left) * parent.mInfo.bytesPerPixel();

  Pixmap result(ImageInfo(right - left, bottom - top, parent.mInfo.colorType(), parent.mInfo.alphaType()),
          base + offset, parent.mRowBytes);
  // The subset borrows the parent's buffer, so it holds the parent's owner too. Without this the parent
  // could be released while the subset is still being written through.
  result.mOwner = parent.mOwner;
  return result;
}

uint32_t Pixmap::getPixelARGB32(const int x, const int y) const noexcept {
  const auto* pixel = static_cast<const uint8_t*>(addr(x, y));
  if (pixel == nullptr) return 0;

  uint32_t channels[4];
  for (int channel = 0; channel < 4; ++channel) {
    channels[channel] = readChannel8(pixel, mInfo.colorType(), channel);
  }
  if (!hasAlphaChannel(mInfo.colorType())) channels[3] = kMaxChannel8;

  const uint32_t alpha = channels[3];

  if (mInfo.alphaType() == AlphaType::kPremultiplied) {
    // Unpremultiply, because this accessor's contract is unpremultiplied output. Dividing by a zero alpha
    // would be undefined, so that case is answered directly -- and correctly: a premultiplied pixel with no
    // alpha carries no colour, whatever its other bytes happen to hold.
    if (alpha == 0) return 0;
    for (int channel = 0; channel < 3; ++channel) {
      const uint32_t value = std::min(channels[channel], alpha);
      // `value * 255` reaches 65025, which fits in 32 bits, so this one needs no widening -- but the
      // premultiply it inverts did, which is why the two are not symmetric.
      channels[channel] = (value * kMaxChannel8 + alpha / 2) / alpha;
    }
  }

  return (channels[3] << 24) | (channels[0] << 16) | (channels[1] << 8) | channels[2];
}

void Pixmap::setPixelARGB32(const int x, const int y, const uint32_t argb) noexcept {
  auto* pixel = static_cast<uint8_t*>(addr(x, y));
  if (pixel == nullptr) return;

  uint32_t r = (argb >> 16) & 0xFFu;
  uint32_t g = (argb >> 8) & 0xFFu;
  uint32_t b = argb & 0xFFu;
  const uint32_t a = (argb >> 24) & 0xFFu;

  if (mInfo.alphaType() == AlphaType::kPremultiplied) {
    // One rounding, in one place. See the note on `premultiply8` for what happens when the rescale is split
    // between the helper and its caller.
    r = premultiply8(r, a);
    g = premultiply8(g, a);
    b = premultiply8(b, a);
  }

  writeChannels8(pixel, mInfo.colorType(), r, g, b, a);
}

} // namespace graphics
