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

#ifndef CORAL_GRAPHICS_IMAGE_H
#define CORAL_GRAPHICS_IMAGE_H

#include <graphics/Bitmap.h>
#include <graphics/Data.h>
#include <graphics/ImageInfo.h>

#include <graphics/ImageCodec.h>
#include <geometry/Rect.h>

#include <cstdint>
#include <string>
#include <utility>

namespace graphics {

/**
 * Pixels that cannot change.
 *
 * ## The difference from Bitmap, and why both exist
 *
 * A `Bitmap` is a place to draw; an `Image` is a picture. The distinction is immutability, and it is not
 * decoration: because an `Image` cannot change it can be shared freely between callers and across threads without
 * a lock, cached by content, and handed to something that outlives the caller. A `Bitmap` can promise none of
 * that, which is why a drawing routine takes a `Bitmap` and everything that *consumes* pixels takes an `Image`.
 *
 * The conversion goes one way by design. `Image::fromBitmap` copies and there is no `toBitmap`: a caller that
 * needs to draw into a picture made an `Image` too early, and a mutable escape hatch would let the guarantee be
 * broken from a distance -- which is the aliasing bug this type exists to make impossible. A caller that really
 * does need to draw calls @c makeBitmap, which is named the long way round because the copy is real.
 *
 * ## Sharing
 *
 * Copying an `Image` copies a reference rather than the pixels, so passing one by value through several layers is
 * cheap and the pixels are released when the last reference goes.
 */
class Image {
public:
  Image() = default;
  ~Image() = default;

  Image(const Image&) noexcept = default;
  Image& operator=(const Image&) noexcept = default;
  Image(Image&&) noexcept = default;
  Image& operator=(Image&&) noexcept = default;

  /**
   * Copies @p bitmap's pixels.
   *
   * A copy rather than a reference, because the bitmap can be redrawn the moment this returns and an `Image` that
   * changed underneath its holder would be worse than useless. The cost is one allocation, paid once at the
   * boundary -- which is the point of having a boundary.
   */
  [[nodiscard]] static Image fromBitmap(const Bitmap& bitmap);

  /**
   * Wraps @p data without copying.
   *
   * The bytes must already be in @p info's format with @p rowBytes between rows. Nothing is validated beyond the
   * length, because there is nothing to validate against: a buffer of the right size holding the wrong format is
   * indistinguishable from a correct one, and the caller is the only party that knows.
   *
   * @return an empty `Image` when the buffer cannot hold the geometry @p info describes -- the one thing that
   *         *can* be checked, and the one that would otherwise become a read past the end.
   */
  [[nodiscard]] static Image fromData(const ImageInfo& info, Data data, size_t rowBytes = 0);

  /**
   * @return the decoded image, or an empty one with @p outError explaining why not.
   *
   * Encoded bytes in, pixels out -- PNG, JPEG or WebP, chosen by content rather than by file name. This is the
   * one entry point here that can fail for reasons outside the caller's control, which is why it reports a reason
   * and not only a failure.
   */
  [[nodiscard]] static Image decode(const Data& encoded, std::string* outError = nullptr);

  /** @return the encoded bytes in @p format, or an empty `Data` with @p outError set. */
  [[nodiscard]] Data encode(ImageFormat format, int quality = 90, std::string* outError = nullptr) const;

  /** @return true when there are no pixels. */
  [[nodiscard]] bool isEmpty() const noexcept { return mBitmap.isEmpty(); }

  /** The format and dimensions. */
  [[nodiscard]] const ImageInfo& info() const noexcept { return mBitmap.info(); }

  [[nodiscard]] int width() const noexcept { return mBitmap.width(); }
  [[nodiscard]] int height() const noexcept { return mBitmap.height(); }

  /** The rectangle from the origin to the image's size. */
  [[nodiscard]] Rect bounds() const noexcept;

  /**
   * @return a read-only view of the pixels.
   *
   * Reads only, and now truthfully so: the bitmap behind an `Image` is stored **non-writable** (see `fromBitmap`), so
   * nothing can draw into a picture that other holders believe is fixed. A caller that needs to modify pixels calls
   * @c makeBitmap.
   */
  [[nodiscard]] Pixmap makePixmap() const noexcept { return mBitmap.makePixmap(); }

  /**
   * @return a read-only view of the pixels, for a drawing routine.
   *
   * The counterpart of `makeBitmap`, and the one the drawing path wants: **no copy**, no allocation, and the pixels are
   * the image's own -- the returned reference stays valid as long as the image does. It is safe to hand out because the
   * stored bitmap is non-writable: a caller that copies it gets another non-writable `Bitmap` (copies share the pixels
   * *and* the flag), so `lockPixels` refuses and the picture cannot be changed from under its other holders.
   *
   * This is what `Canvas::drawImage` uses, which is what keeps drawing an image free of the deep copy that
   * `makeBitmap` exists for.
   */
  [[nodiscard]] const Bitmap& peekBitmap() const noexcept { return mBitmap; }

  /**
   * @return a writable copy of the pixels, for a caller about to draw.
   *
   * The explicit way back to something drawable, and a copy on purpose: the result is a new `Bitmap` nobody else
   * holds, so drawing into it cannot affect this image or any other holder of it.
   */
  [[nodiscard]] Bitmap makeBitmap() const { return mBitmap.makeDeepCopy(); }

  /** The bytes, for a caller that needs to hand them to a decoder or an upload. */
  [[nodiscard]] const Data& data() const noexcept { return mBitmap.data(); }

  /**
   * @return true when the two hold the same pixels.
   *
   * A comparison of contents rather than of identity, so two images decoded from one file compare equal. That is
   * what a cache needs, and it is the same choice `ImageInfo` and `Data` make.
   */
  [[nodiscard]] bool equals(const Image& other) const noexcept;

  friend bool operator==(const Image& lhs, const Image& rhs) noexcept { return lhs.equals(rhs); }
  friend bool operator!=(const Image& lhs, const Image& rhs) noexcept { return !lhs.equals(rhs); }

private:
  explicit Image(Bitmap bitmap) noexcept: mBitmap(std::move(bitmap)) {}

  /**
   * The pixels.
   *
   * A `Bitmap` rather than a bare `Data`, so the format, the stride and the ownership arrive together and cannot
   * disagree. It is only ever read from here: the immutability is the absence of a non-const accessor to it,
   * which is a weaker mechanism than a `const` member would be but is what allows the defaulted copy and move
   * operations to exist at all.
   */
  Bitmap mBitmap;
};

} // namespace graphics

#endif //CORAL_GRAPHICS_IMAGE_H
