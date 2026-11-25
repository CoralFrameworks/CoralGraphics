/* Copyright (C) 2026 The Coral Project
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

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace graphics::gpu {

/**
 * Where the next bitmap goes in an image atlas page: a shelf allocator that only ever appends.
 *
 * ## Why an image atlas needs one, and why it never frees
 *
 * A glyph's bitmap is the same every time it is drawn, so the image route's textures are cached by **content**, and the
 * only thing that ever grows is the set of distinct bitmaps a session has seen. Packing those into a shared page is what
 * lets the route batch -- an image batch binds one texture, and a batch of glyphs with one texture each is one draw per
 * glyph, which is 559 draws in 559 batches on the demo scene (`路径边缘抗锯齿设计与现状.md` 4.1.26).
 *
 * Freeing is what this class deliberately does not do: the RHI has no contract for releasing a texture the GPU may still
 * be reading (`Device::destroyTexture` says nothing about resources in flight), so an eviction policy has nowhere safe to
 * run -- the same reason `FieldAtlas` never destroys one. A page is therefore filled once and kept; a page with no room
 * for the next bitmap is left alone and a new one is started, and when the page budget is spent the caller falls back to
 * a texture of that bitmap's own.
 *
 * ## Why a shelf and not a free-list
 *
 * Because nothing is ever freed, the packing only has to answer "where does the next one go", and shelves answer it in
 * constant time with no bookkeeping at all: fill a row left to right, then start the next row below the tallest bitmap
 * that row held. That waste is the strip above short bitmaps in a row of mixed heights, which is why the caller should
 * feed the bitmaps in descending height --- `balanceByCost`'s sibling problem, and one the *page* cannot solve alone
 * because the bitmaps arrive over a session rather than all at once.
 *
 * The one invariant every caller depends on: two successful allocations never overlap. That is what a test can pin
 * without a GPU, which is why this is a class of its own rather than six lines inside the renderer.
 */
class ImageShelfAllocator {
public:
  ImageShelfAllocator(const uint32_t width, const uint32_t height) noexcept
    : mWidth(width), mHeight(height) {}

  /** @return the page's extent, which is what a caller uploads into and normalizes a sub-rectangle against. */
  [[nodiscard]] uint32_t width() const noexcept { return mWidth; }
  [[nodiscard]] uint32_t height() const noexcept { return mHeight; }

  /**
   * Reserves a @p width by @p height rectangle and reports where it is.
   *
   * @param x written only on success.
   * @param y written only on success.
   * @return false when the bitmap does not fit -- too wide or too tall for the page, or the page is full. The caller's
   *         answer to false is a texture of that bitmap's own, so a refusal is a cost and never a wrong picture.
   */
  [[nodiscard]] bool allocate(const uint32_t width, const uint32_t height, uint32_t* x, uint32_t* y) noexcept {
    if (x == nullptr || y == nullptr) return false;
    if (width == 0 || height == 0) return false;
    // Wider or taller than the page: no arrangement fits it, so the caller must not be told to try again later.
    if (width > mWidth || height > mHeight) return false;

    // A bitmap that does not fit the current shelf starts a new one, below whatever that shelf held.
    if (mX + width > mWidth) {
      mX = 0;
      mY += mShelfHeight;
      mShelfHeight = 0;
    }
    if (mY + height > mHeight) return false;

    *x = mX;
    *y = mY;
    mX += width;
    mShelfHeight = std::max(mShelfHeight, height);
    return true;
  }

  /** @return the rows used so far, which is all a shelf allocator can honestly report as "how full". */
  [[nodiscard]] uint32_t usedRows() const noexcept { return mY + mShelfHeight; }

  /** @return true once an allocation of any size would fail for want of rows. */
  [[nodiscard]] bool full() const noexcept { return mY + mShelfHeight >= mHeight; }

private:
  uint32_t mWidth = 0;
  uint32_t mHeight = 0;
  /** The next free column of the current shelf. */
  uint32_t mX = 0;
  /** The top row of the current shelf. */
  uint32_t mY = 0;
  /** How tall the current shelf is, which is what the next shelf starts below. */
  uint32_t mShelfHeight = 0;
};

/**
 * The four numbers `image.frag` reads as its sub-rectangle, for a bitmap placed with a padded rectangle in a page.
 *
 * ## The border, which is the whole reason this is a function and not a division at the call site
 *
 * An atlas page sampled with **linear** filtering blends neighbouring texels at a bitmap's edge: a tap at the bitmap's
 * last texel reaches half a texel beyond it, and what is there is the *next* bitmap's first texel. So every bitmap is
 * placed with a **one-texel border** and only its interior is sampled -- exactly what the coverage-field atlas does, and
 * why that one replicates the field's edge texels into its ring (`GpuRenderer::packCoverageFields`). Replication is what
 * makes a sample *equal* to clamp-to-edge sampling of the bitmap alone; a transparent border would fade a glyph's edge
 * toward nothing, which is a wrong picture that looks like an antialiasing difference.
 *
 * @param pageX  the padded rectangle's corner, which is what the allocator returned.
 * @param pageY  the padded rectangle's corner, which is what the allocator returned.
 * @param width  the bitmap's own size, inside that padded rectangle.
 * @param height the bitmap's own size, inside that padded rectangle.
 * @return `(u0, v0, du, dv)`: the bitmap's interior in **normalized** texture coordinates, ready for `vParams`.
 *
 * The divisor is the **page's** extent, because the page is the texture the fragment stage samples, and there is no
 * half-texel offset: a normalized coordinate of `x / pageWidth` is the *boundary* between texels `x-1` and `x`, which is
 * where a rectangle's edge belongs. The half-texel error of 4.1.8 was a CPU reference sampling at a pixel's *centre*;
 * here the coordinate is an edge, and adding half a texel would shift the whole bitmap by one texel -- a shifted glyph is
 * a picture nobody would call an atlas bug.
 */
[[nodiscard]] inline std::array<float, 4> normalizedImageRect(const uint32_t pageX, const uint32_t pageY,
        const uint32_t width, const uint32_t height, const uint32_t pageWidth, const uint32_t pageHeight) noexcept {
  if (pageWidth == 0 || pageHeight == 0) return { 0.0f, 0.0f, 0.0f, 0.0f };
  const float inverseWidth = 1.0f / static_cast<float>(pageWidth);
  const float inverseHeight = 1.0f / static_cast<float>(pageHeight);
  // One texel in on every side: the interior, whose ring is the replicated border this function exists to leave out.
  return { static_cast<float>(pageX + 1u) * inverseWidth,
           static_cast<float>(pageY + 1u) * inverseHeight,
           static_cast<float>(width) * inverseWidth,
           static_cast<float>(height) * inverseHeight };
}

} // namespace graphics::gpu

