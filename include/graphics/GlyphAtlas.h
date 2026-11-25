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

#ifndef CORAL_GRAPHICS_GLYPHATLAS_H
#define CORAL_GRAPHICS_GLYPHATLAS_H

#include <geometry/Rect.h>
#include <graphics/Bitmap.h>
#include <graphics/Typeface.h>

#include <cstdint>
#include <optional>

namespace graphics {

/**
 * A process-wide page of hinted glyph coverage, packed on first use.
 *
 * This is a graphics resource, not a UI one: the input is a `GlyphBitmap` produced by `Typeface`, and the
 * output is a source rectangle into a shared bitmap. Keeping it here means a second renderer can reuse the
 * same page without depending on `libs/ui`, and it keeps the page's lifetime tied to the graphics cache it
 * belongs to.
 *
 * The page is append-only: a glyph is written once and never moved, so a command recorded against a cell
 * keeps sampling valid pixels after later insertions. A full page returns no entry and the caller falls
 * back to a standalone glyph bitmap.
 */
class GlyphAtlas {
public:
  struct Entry {
    uint32_t page = 0;
    Rect source;
  };

  [[nodiscard]] static GlyphAtlas& instance();

  /**
   * Packs @p glyph at @p pixelSize into the first page with room (creating one when needed).
   * Returns null for a colour bitmap or when every page is full.
   */
  [[nodiscard]] std::optional<Entry> add(const GlyphBitmap& glyph, uint32_t glyphId, float pixelSize,
    const Typeface* typeface = nullptr);

  /** @return the page bitmap, or an empty bitmap for an invalid page. */
  [[nodiscard]] const Bitmap& bitmap(uint32_t page) const;

  [[nodiscard]] uint32_t pageCount() const noexcept;
  /** Pixel bytes currently allocated by all pages. */
  [[nodiscard]] size_t bytes() const noexcept;

private:
  GlyphAtlas();
  ~GlyphAtlas();
  GlyphAtlas(const GlyphAtlas&) = delete;
  GlyphAtlas& operator=(const GlyphAtlas&) = delete;

  struct Impl;
  Impl* mImpl = nullptr;
};

} // namespace graphics

#endif // CORAL_GRAPHICS_GLYPHATLAS_H
