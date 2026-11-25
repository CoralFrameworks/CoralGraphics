/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and limitations under the
 * License.
 */

#include <graphics/ImageInfo.h>

#include <limits>
#include <utility>

namespace graphics {

ImageInfo ImageInfo::withColorSpace(std::shared_ptr<ColorSpace> colorSpace) const {
  ImageInfo copy = *this;
  copy.mColorSpace = std::move(colorSpace);
  return copy;
}

size_t ImageInfo::bytesPerPixel() const noexcept {
  switch (mColorType) {
    case ColorType::kAlpha_8:
    case ColorType::kGray_8:
      return 1;
    case ColorType::kRGB_565:
      return 2;
    case ColorType::kRGBA_8888:
    case ColorType::kBGRA_8888:
    case ColorType::kRGBA_1010102:
    case ColorType::kBGRA_1010102:
      return 4;
    case ColorType::kRGBA_F16Norm:
    case ColorType::kRGBA_F16:
      return 8;
    case ColorType::kRGBA_F32:
      return 16;
    case ColorType::kUnknown:
      break;
  }
  // Zero is the answer that makes a caller stop. Returning a guess here -- one byte, say -- would let
  // an unsupported format through to an allocation of the wrong size and then to a decoder reading
  // past the end of it.
  return 0;
}

size_t ImageInfo::computeByteSize() const noexcept {
  if (!isValid()) return 0;

  // A per-axis bound as well as the overflow check below, because the two catch different things.
  // The overflow check would accept 200000 x 200000 -- 1.6e11 bytes fits in a 64-bit `size_t` -- and
  // then hand the caller an allocation request that will certainly fail. An image larger than this on
  // either axis is not a picture anybody is rendering, so it is refused where the answer is still a
  // clear "no" rather than a three-minute allocation attempt that ends in a `bad_alloc`.
  constexpr int kMaxDimension = 65535;
  if (mWidth > kMaxDimension || mHeight > kMaxDimension) return 0;

  const size_t rowBytes = minRowBytes();
  if (rowBytes == 0) return 0;

  // The multiplication itself is checked in the wide type and against the maximum beforehand, because
  // a product that wraps would be a small, plausible, wrong number.
  constexpr size_t kMax = std::numeric_limits<size_t>::max();
  if (rowBytes > kMax / static_cast<size_t>(mHeight)) return 0;
  return rowBytes * static_cast<size_t>(mHeight);
}

bool isSupportedColorType(const ColorType colorType) noexcept {
  switch (colorType) {
    case ColorType::kAlpha_8:
    case ColorType::kGray_8:
    case ColorType::kRGB_565:
    case ColorType::kRGBA_8888:
    case ColorType::kBGRA_8888:
    case ColorType::kRGBA_1010102:
    case ColorType::kBGRA_1010102:
    case ColorType::kRGBA_F16Norm:
    case ColorType::kRGBA_F16:
    case ColorType::kRGBA_F32:
      return true;
    case ColorType::kUnknown:
      break;
  }
  return false;
}

} // namespace graphics
