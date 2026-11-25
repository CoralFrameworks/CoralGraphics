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

#ifndef CORAL_GRAPHICS_PIXMAP_H
#define CORAL_GRAPHICS_PIXMAP_H

#include <graphics/Data.h>
#include <graphics/ImageInfo.h>

#include <geometry/Rect.h>

#include <cstdint>

namespace graphics {

/**
 * A window onto pixels somebody else owns.
 *
 * ## Not an image
 *
 * The distinction from `Image` is ownership, and it is worth being precise because the two are otherwise
 * described the same way. A `Pixmap` says "there are pixels here, of this format, with this row stride"
 * and holds no claim on them. An `Image` owns its pixels through a `Data` and cannot be invalidated. A
 * `Pixmap` is what a decoder writes into, what a canvas draws from, and what crosses an API boundary
 * where a copy would be wasteful -- and it is invalid the moment the buffer behind it goes away.
 *
 * Being a view, it is copyable: copying one copies the *view*, not the pixels. Two pixmaps over the same
 * buffer see each other's writes, which is the intent.
 *
 * ## The stride is stored, not derived
 *
 * `rowBytes` is kept rather than computed from the width, because it genuinely varies. A decoder may
 * align each row to a boundary for vectorised access, a GPU texture may require it, and a sub-window onto
 * a larger buffer has it forced by the parent. Assuming `width * bytesPerPixel` is the mistake this type
 * exists to prevent: it works on every image its author tested and produces a skewed picture on the first
 * one whose rows happen to be padded.
 */
class Pixmap {
public:
  /** An empty pixmap: no pixels, no size, unusable until assigned. */
  Pixmap() noexcept = default;

  /**
   * A view of @p pixels with the geometry @p info describes.
   *
   * @param rowBytes the distance between the starts of consecutive rows, or zero to mean "the minimum for
   *                 this format" -- the common case, and what a caller with a tightly packed buffer wants.
   *
   * The arguments are not validated here. A `Pixmap` is a view, and re-checking its arithmetic on every
   * construction would cost more than it catches. @c isValid answers whether the geometry makes sense and
   * it is the caller's job to ask before addressing pixels -- but the accessors below bounds-check anyway,
   * so an invalid pixmap reads as transparent rather than as a wild read.
   */
  Pixmap(const ImageInfo& info, void* pixels, size_t rowBytes = 0) noexcept;

  /**
   * A view of @p data's bytes, which this keeps alive.
   *
   * The safer of the two constructors and the one to prefer: the alternative is a bare pointer whose
   * validity the caller has promised, while this one holds a reference to the buffer and cannot dangle.
   */
  Pixmap(const ImageInfo& info, const Data& data) noexcept;

  /**
   * A view of a sub-rectangle of @p parent.
   *
   * @return a pixmap over the intersection of @p area with the parent's bounds, so a request that runs
   *         off the edge yields the part that exists rather than a view of memory outside the buffer --
   *         the difference between a clipped draw and a read of somebody else's heap.
   */
  [[nodiscard]] static Pixmap makeSubset(const Pixmap& parent, const Rect& area) noexcept;

  ~Pixmap() = default;

  Pixmap(const Pixmap&) noexcept = default;
  Pixmap& operator=(const Pixmap&) noexcept = default;

  /** The format and dimensions of the pixels. */
  [[nodiscard]] const ImageInfo& info() const noexcept { return mInfo; }

  [[nodiscard]] int width() const noexcept { return mInfo.width(); }
  [[nodiscard]] int height() const noexcept { return mInfo.height(); }

  /** The distance between the starts of consecutive rows, in bytes. */
  [[nodiscard]] size_t rowBytes() const noexcept { return mRowBytes; }

  /** The pixel format. */
  [[nodiscard]] ColorType colorType() const noexcept { return mInfo.colorType(); }

  /** @return a pointer to the first pixel, or null when this holds none. */
  [[nodiscard]] void* pixels() noexcept { return mPixels; }
  [[nodiscard]] const void* pixels() const noexcept { return mPixels; }

  [[nodiscard]] bool isEmpty() const noexcept { return mPixels == nullptr || !mInfo.isValid(); }

  /**
   * @return true when the geometry can be addressed safely.
   *
   * The checks are the ones that make every pixel access below well defined: a supported format, a
   * non-negative size, a row stride at least as wide as a row of pixels, and a total size whose
   * arithmetic -- `rowBytes * (height - 1) + minRowBytes` -- does not wrap. That last one is the reason
   * this is not simply "does it have pixels": a large stride times a large height overflows on 32-bit
   * builds, and the wrap turns a bounds check into a pass.
   */
  [[nodiscard]] bool isValid() const noexcept;

  /** @return a pointer to the pixel at (@p x, @p y), or null when the position is outside. */
  [[nodiscard]] void* addr(int x, int y) noexcept;

  /** @return a read-only pointer to the pixel at (@p x, @p y), or null when the position is outside. */
  [[nodiscard]] const void* addr(int x, int y) const noexcept;

  /**
   * @return the pixel at (@p x, @p y) as unpremultiplied 8-bit RGBA, in the low four bytes.
   *
   * Zero for every component when the position is outside the pixmap, so a caller probing past an edge
   * reads transparent rather than whatever memory is there. Formats with no useful conversion -- a
   * floating-point format read through an 8-bit accessor -- are also reported as transparent rather than
   * silently scaled, because a plausible-looking wrong colour is harder to notice than a hole.
   */
  [[nodiscard]] uint32_t getPixelARGB32(int x, int y) const noexcept;

  /**
   * Writes @p argb to (@p x, @p y), leaving the position untouched when it is outside the pixmap.
   *
   * @param argb unpremultiplied 8-bit RGBA in the low four bytes, matching @c getPixelARGB32. Unpremultiplied
   *             on both sides on purpose: a caller reading a pixel and writing it back should not have to
   *             know whether the destination format premultiplies, so the conversion belongs here in both
   *             directions rather than at every call site.
   */
  void setPixelARGB32(int x, int y, uint32_t argb) noexcept;

private:
  ImageInfo mInfo;
  void* mPixels = nullptr;
  size_t mRowBytes = 0;
  /**
   * Keeps a borrowed buffer alive when one was supplied.
   *
   * Only the `Data` constructor sets this. It is what makes that constructor safe rather than merely
   * documented, at the cost of an atomic increment per pixmap -- worth paying for a guarantee the
   * alternative would have to state in prose and hope every caller read.
   */
  Data mOwner;
};

} // namespace graphics

#endif //CORAL_GRAPHICS_PIXMAP_H
