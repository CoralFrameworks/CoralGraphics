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

#ifndef CORAL_GRAPHICS_FONT_H
#define CORAL_GRAPHICS_FONT_H

#include <graphics/FontMetrics.h>
#include <graphics/TextBlob.h>
#include <graphics/Typeface.h>

#include <cstdint>
#include <memory>
#include <string_view>

namespace graphics {

/**
 * A typeface at a particular size, with everything needed to measure and shape text.
 *
 * ## What "size" means here
 *
 * The size is in the units the caller lays out in -- pixels for a screen renderer -- and every
 * measurement reported is in those units. That is not the same as the font's own units: a font states
 * its metrics in integer units of an em, so a 16-pixel font with 2048 units per em converts every
 * metric by 16/2048. Doing that conversion once, here, is what stops every caller from having to know
 * about it, and it is the main reason a `Font` exists rather than a set of free functions over a
 * `Typeface`.
 *
 * ## Why shaping is a method
 *
 * Shaping needs the font at a size, the shaping state harfbuzz keeps for it, and an answer to "which
 * direction is this text" -- all of which live here. Free functions would need all of it passed in, and
 * the caller would be responsible for keeping it consistent.
 *
 * ## Relationship to the platform text stacks
 *
 * FreeType supplies glyph metrics and harfbuzz supplies shaping, on every platform. On Apple the plan is
 * a separate path through CoreText, because that is what the platform's own text rendering uses and
 * matching it is what makes text look native there. This class is the cross-platform path, and the seam
 * for the other one is this same public interface -- which is why nothing here names a FreeType or
 * harfbuzz type.
 */
class Font {
public:
  ~Font();

  Font(const Font&) = delete;
  Font& operator=(const Font&) = delete;

  /**
   * @return @p typeface at @p size, or null when it cannot be scaled to that size.
   *
   * A size of zero or less, or one large enough to overflow the fixed-point representation used
   * internally, is refused rather than clamped. A caller that asked for 0 almost certainly has an unset
   * variable, and silently substituting something reasonable would hide that until the layout looked
   * wrong for an unrelated reason.
   */
  [[nodiscard]] static std::shared_ptr<Font> create(std::shared_ptr<Typeface> typeface, float size);

  /** The typeface this font was created from, which the font keeps alive. */
  [[nodiscard]] const std::shared_ptr<Typeface>& typeface() const noexcept { return mTypeface; }

  /** The size, in the caller's units. */
  [[nodiscard]] float size() const noexcept { return mSize; }

  /**
   * @return this font at @p newSize.
   *
   * A new font rather than a mutation, because a shaped result is tied to the size that produced it:
   * changing the size in place would silently invalidate every `TextBlob` already handed out. The new
   * font shares the typeface, so the cost is one size object rather than a re-parse of the file.
   */
  [[nodiscard]] std::shared_ptr<Font> withSize(float newSize) const;

  /** The font's vertical measurements at this size. */
  [[nodiscard]] const FontMetrics& metrics() const noexcept { return mMetrics; }

  /** How the text is laid out, for the cases where a caller knows better than the shaper. */
  enum class Direction : uint8_t {
    /** Decide from the text's own script. */
    Auto,
    LeftToRight,
    RightToLeft,
  };

  /**
   * @return the advance of @p text, which is its layout width.
   *
   * Shaping the text in order to measure it sounds wasteful and is not: the width of a run is not the
   * sum of its characters' advances, because kerning and ligatures are precisely the cases where it
   * differs. A cheaper estimate would be wrong for the text that most needs measuring.
   */
  [[nodiscard]] float measureWidth(std::string_view text, Direction direction = Direction::Auto) const;

  /**
   * @return the advance of one glyph.
   *
   * Not the width of the character that produced it -- see @c measureWidth -- but the right answer for a
   * caller that already holds glyph identifiers, which is exactly what a `TextBlob` contains.
   */
  [[nodiscard]] float advanceForGlyph(uint32_t glyphId) const noexcept;

  /**
   * @return @p text shaped into positioned glyphs, ready to draw.
   *
   * The glyphs come back in visual order, which for a right-to-left script is the reverse of the input.
   * Positions are relative to a pen at the origin, so a caller draws by adding its own origin to each
   * glyph's offset.
   */
  [[nodiscard]] TextBlob shape(std::string_view text, Direction direction = Direction::Auto) const;

  /**
   * @return the glyph identifier for @p codePoint, or 0 when there is none.
   *
   * A convenience over `typeface()->glyphForCodePoint`, and the entry point to prefer for a single
   * character such as a bullet or an ellipsis, where shaping a string would be overkill.
   */
  [[nodiscard]] uint32_t glyphForCodePoint(uint32_t codePoint) const noexcept;

  /** @return true when this font can render @p codePoint without showing a missing-glyph box. */
  [[nodiscard]] bool hasGlyphForCodePoint(uint32_t codePoint) const noexcept;

private:
  Font() = default;

  struct Impl;

  /** Recomputes the metrics and anything else that depends on the size. */
  void updateMetrics();

  std::shared_ptr<Typeface> mTypeface;
  float mSize = 0.0f;
  FontMetrics mMetrics;

  /**
   * The harfbuzz font, hidden here for the same reason `Typeface::mImpl` is hidden: `<hb.h>` in a public
   * header would leak its macros into every translation unit that includes a font header.
   */
  std::unique_ptr<Impl> mImpl;
};

} // namespace graphics

#endif //CORAL_GRAPHICS_FONT_H
