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

#ifndef CORAL_GRAPHICS_BITMAP_H
#define CORAL_GRAPHICS_BITMAP_H

#include <graphics/Data.h>
#include <graphics/ImageInfo.h>
#include <graphics/Pixmap.h>

#include <geometry/Color.h>

namespace graphics {

/**
 * Pixels that this object owns.
 *
 * ## The difference from Pixmap, which is the whole point
 *
 * A `Pixmap` is a view: it says where pixels are and holds no claim on them. A `Bitmap` holds the pixels
 * through a `Data`, so it cannot be invalidated by anything outside itself, and a copy of it shares those
 * pixels rather than duplicating them. That sharing is deliberate and is worth being explicit about -- a
 * `Bitmap` copy is cheap and the two copies see each other's writes, exactly as a `Pixmap` copy does. A
 * caller wanting independent pixels calls @c makeDeepCopy, which is named the long way round so that the
 * cheap operation is the one you get by writing `=`.
 *
 * ## Why the pixels may be read-only
 *
 * @c lockPixels returns a `Pixmap` for drawing into, and it can hand back an empty one. A bitmap created by
 * @c fromData wraps bytes it does not own, and those may be a decoded image or a literal in read-only
 * memory; writing through a view of them would corrupt the source or crash. Rather than making that a
 * runtime surprise, @c isWritable answers the question up front and @c lockPixels refuses when it is false.
 */
class Bitmap {
public:
  /** An empty bitmap: no pixels, no size. */
  Bitmap() noexcept = default;

  ~Bitmap() = default;

  /** Copies share the pixels. Use @c makeDeepCopy for an independent copy. */
  Bitmap(const Bitmap&) noexcept = default;
  Bitmap& operator=(const Bitmap&) noexcept = default;

  /**
   * Moving clears the source.
   *
   * Written out rather than defaulted because a defaulted move copies the `ImageInfo`: it holds no resources, so
   * nothing about it is moved and the source keeps claiming the old dimensions while its pixels have gone. The
   * result is an object that reports a size, passes an `isValid` check, and has no buffer -- which is worse than
   * an empty one, because every subsequent validity test agrees with it. A moved-from bitmap is empty instead.
   */
  Bitmap(Bitmap&& other) noexcept
    : mInfo(std::move(other.mInfo)),
      mData(other.mData),
      mRowBytes(other.mRowBytes),
      mWritable(other.mWritable) {
    other.mInfo = ImageInfo{};
    other.mRowBytes = 0;
    other.mWritable = false;
  }

  Bitmap& operator=(Bitmap&& other) noexcept {
    if (this != &other) {
      mInfo = std::move(other.mInfo);
      mData = other.mData;
      mRowBytes = other.mRowBytes;
      mWritable = other.mWritable;
      other.mInfo = ImageInfo{};
      other.mRowBytes = 0;
      other.mWritable = false;
    }
    return *this;
  }

  /**
   * Allocates pixels for @p info, zeroed.
   *
   * @return an empty bitmap when @p info does not describe something that can be allocated -- an
   *         unrecognised colour type, a non-positive size, or a total that overflows. Refusing here means
   *         that every later operation can assume a valid geometry instead of testing for it, which is a
   *         check saved on the hot path of every draw.
   *
   * Zeroed rather than left as the allocator returns it, so that a partially drawn canvas shows black
   * rather than whatever was in the heap -- which would be a disclosure as much as a bug.
   */
  [[nodiscard]] static Bitmap allocate(const ImageInfo& info);

  /** Convenience for the common case: an 8-bit RGBA bitmap of the given size. */
  [[nodiscard]] static Bitmap allocate(int width, int height);

  /**
   * Wraps bytes that somebody else owns, without copying.
   *
   * @param info the info of the Image.
   * @param data holds the bytes and keeps them alive.
   * @param rowBytes the distance between rows, or zero for the minimum for @p info.
   *
   * The result is not writable -- see the note on the class -- because a `Data` handed in this way may well
   * be the output of a decoder or a shared constant, and there is no way to tell from here.
   */
  [[nodiscard]] static Bitmap fromData(const ImageInfo& info, const Data& data, size_t rowBytes = 0);

  /** @return an independent copy of the pixels, or an empty bitmap when this one is empty. */
  [[nodiscard]] Bitmap makeDeepCopy() const;

  /** The format and dimensions, or an empty `ImageInfo` for an empty bitmap. */
  [[nodiscard]] const ImageInfo& info() const noexcept { return mInfo; }

  [[nodiscard]] int width() const noexcept { return mInfo.width(); }
  [[nodiscard]] int height() const noexcept { return mInfo.height(); }
  [[nodiscard]] ColorType colorType() const noexcept { return mInfo.colorType(); }
  [[nodiscard]] AlphaType alphaType() const noexcept { return mInfo.alphaType(); }

  /** @return true when there are no pixels, or the geometry does not describe anything drawable. */
  [[nodiscard]] bool isEmpty() const noexcept { return mData.isEmpty() || !mInfo.isValid(); }

  /** @return true when the pixels came from @c allocate and may be written through. */
  [[nodiscard]] bool isWritable() const noexcept { return mWritable; }

  /** The distance between rows, in bytes. */
  [[nodiscard]] size_t rowBytes() const noexcept { return mRowBytes; }

  /**
   * @return a read-only view of the pixels, or an empty pixmap when this bitmap is empty.
   *
   * The @c Pixmap borrows from this bitmap's `Data`, so it keeps the pixels alive and stays valid after the
   * bitmap itself is gone -- which is what makes it safe to hand a decoded image's view to something that
   * outlives the decode.
   */
  [[nodiscard]] Pixmap makePixmap() const noexcept;

  /**
   * @return a writable view of the pixels, or an empty pixmap when they are read-only.
   *
   * Named for what it does rather than called @c pixels, because the empty result on a read-only bitmap is
   * the kind of thing a caller should have to think about: a call that silently returned nothing writable
   * would make every draw a no-op with no indication why.
   */
  [[nodiscard]] Pixmap lockPixels() noexcept;

  /**
   * Fills the whole bitmap with @p color.
   *
   * Writes the pixel rather than doing a fast path over a run of bytes, because the pixel's layout depends on
   * the format -- filling a `kRGB_565` bitmap with a byte pattern would be a different operation from filling
   * it with a colour, and only one of them is what the caller asked for.
   *
   * @return false when the pixels are not writable.
   *
   * The colour comes from `geometry`, which is where the shared value types live; see the note on `Paint` for
   * why the namespace is spelled out.
   */
  bool erase(const Color& color) noexcept;

  /** The bytes, for a caller that needs to hand them to a decoder or a texture upload. */
  [[nodiscard]] const Data& data() const noexcept { return mData; }

private:
  ImageInfo mInfo;
  Data mData;
  size_t mRowBytes = 0;
  bool mWritable = false;
};

} // namespace graphics

#endif //CORAL_GRAPHICS_BITMAP_H
