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

#ifndef CORAL_GRAPHICS_IMAGE_INFO_H
#define CORAL_GRAPHICS_IMAGE_INFO_H

#include <geometry/Size.h>

#include <graphics/ColorSpace.h>
#include <graphics/Enums.h>

#include <memory>

namespace graphics {

/**
 * The shape and meaning of a block of pixels: how large it is, how each pixel is encoded, and what
 * the numbers in it mean.
 *
 * A value type, not a resource. It says nothing about where the pixels are, which is `Pixmap`'s job.
 * Keeping the two apart is what lets a decoded image, a window's back buffer and a texture's upload
 * format all be described without any of them owning another.
 */
class ImageInfo {
public:
  ImageInfo() = default;

  ImageInfo(const int width, const int height, const ColorType colorType, const AlphaType alphaType)
      : mWidth(width), mHeight(height), mColorType(colorType), mAlphaType(alphaType) {}

  /** @return a copy of this info with @p colorSpace attached. */
  [[nodiscard]] ImageInfo withColorSpace(std::shared_ptr<ColorSpace> colorSpace) const;

  [[nodiscard]] int width() const noexcept { return mWidth; }
  [[nodiscard]] int height() const noexcept { return mHeight; }
  [[nodiscard]] Size size() const noexcept {
    return Size { static_cast<Scalar>(mWidth), static_cast<Scalar>(mHeight) };
  }

  [[nodiscard]] ColorType colorType() const noexcept { return mColorType; }
  [[nodiscard]] AlphaType alphaType() const noexcept { return mAlphaType; }
  [[nodiscard]] const std::shared_ptr<ColorSpace>& colorSpace() const noexcept { return mColorSpace; }

  /** True when there is something to draw, i.e. both dimensions are positive. */
  [[nodiscard]] bool isValid() const noexcept { return mWidth > 0 && mHeight > 0; }

  /** True when every pixel is fully opaque, so a compositor can skip reading its alpha. */
  [[nodiscard]] bool isOpaque() const noexcept { return mAlphaType == AlphaType::kOpaque; }

  /**
   * @return the number of bytes one pixel occupies, or 0 for a type this build does not know.
   *
   * Zero rather than an exception because the answer is used to size an allocation, and a caller that
   * cannot get a byte count must not proceed. Treating an unrecognised enumerator as 0 is also what
   * gives forward compatibility for a colour type added in a later version.
   */
  [[nodiscard]] size_t bytesPerPixel() const noexcept;

  /**
   * @return the number of bytes one row occupies, with no padding.
   *
   * Rows are tightly packed. A general pixel buffer often wants a row stride rounded up to an
   * alignment, because it is read a vector at a time; a codec's output wants no padding, because it is
   * about to be handed to a decoder. A caller that needs a stride should derive one from this and
   * carry it, which is the one place that knowledge belongs.
   */
  [[nodiscard]] size_t minRowBytes() const noexcept { return bytesPerPixel() * static_cast<size_t>(mWidth); }

  /** @return the total bytes for a tightly packed image, or 0 when the size is not representable. */
  [[nodiscard]] size_t computeByteSize() const noexcept;

  friend bool operator==(const ImageInfo& lhs, const ImageInfo& rhs) noexcept {
    // The colour space is compared by value, not by pointer. Two images describing the same space are
    // interchangeable, and requiring the same `shared_ptr` would turn a cache hit into a cache miss.
    // `ColorSpace` hashes and compares its own fields for exactly this reason.
    const bool sameSpace = (lhs.mColorSpace == nullptr) == (rhs.mColorSpace == nullptr) &&
                           (lhs.mColorSpace == nullptr || *lhs.mColorSpace == *rhs.mColorSpace);
    return lhs.mWidth == rhs.mWidth && lhs.mHeight == rhs.mHeight && lhs.mColorType == rhs.mColorType &&
           lhs.mAlphaType == rhs.mAlphaType && sameSpace;
  }

  friend bool operator!=(const ImageInfo& lhs, const ImageInfo& rhs) noexcept { return !(lhs == rhs); }

private:
  int mWidth = 0;
  int mHeight = 0;
  ColorType mColorType = ColorType::kUnknown;
  AlphaType mAlphaType = AlphaType::kUnknown;
  std::shared_ptr<ColorSpace> mColorSpace;
};

/**
 * @return true when @p colorType is one this build can store.
 *
 * Exists so a caller can distinguish "the image is in a format I understand" from "the image is in a
 * format this version has never heard of", rather than discovering it as a zero byte count later.
 */
[[nodiscard]] bool isSupportedColorType(ColorType colorType) noexcept;

} // namespace graphics

#endif //CORAL_GRAPHICS_IMAGE_INFO_H
