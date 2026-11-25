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

#ifndef CORAL_GRAPHICS_TEXT_BLOB_H
#define CORAL_GRAPHICS_TEXT_BLOB_H

#include <geometry/Point.h>
#include <geometry/Rect.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace graphics {

/**
 * One positioned glyph, in the order it should be drawn.
 *
 * ## Why this is not just a glyph and an x
 *
 * Shaping a script does three things a character-by-character layout cannot express. It *substitutes*:
 * one character can become several glyphs (an `fi` ligature), or several can become one (a base letter
 * plus a combining mark). It *reorders*: an Arabic word is stored in logical order and drawn in visual
 * order, so the glyphs come out in a different sequence from the characters. And it *positions*: a mark
 * such as an acute accent is not advanced along the line at all -- it is placed at a zero advance with
 * an offset that lifts it over the letter it belongs to.
 *
 * A structure holding only a glyph and an x cannot represent any of that. The advance and the offset
 * carry the first and third; producing the glyphs in visual order carries the second, which is what
 * makes this vector's order meaningful rather than incidental.
 */
struct Glyph {
  /** The identifier the font knows this glyph by. */
  uint32_t id = 0;

  /**
   * Where to start drawing, relative to the pen position passed to the shaping call.
   *
   * The offset is what makes marks work: a combining acute has a zero advance and a positive y offset
   * that lifts it above the letter. Ignoring it stacks every accent on the baseline.
   */
  Point offset { 0.0f, 0.0f };

  /**
   * How far the pen moves after drawing this glyph.
   *
   * Separate x and y because a script can advance vertically, and because a mark advances by zero.
   */
  Point advance { 0.0f, 0.0f };

  /**
   * The index of the character this glyph came from, in the original string.
   *
   * This is the bridge back from glyphs to text, and it is not one-to-one: several glyphs can share a
   * cluster (a decomposed accent) and several characters can share one (a ligature). Callers use it for
   * hit testing -- "which character is under this point" -- and to map a selection back onto a string.
   *
   * A cluster index is a *byte* offset into the UTF-8 input, not a character index, because that is
   * what the shaper reports and converting it would need the original string again.
   */
  uint32_t cluster = 0;
};

/**
 * A run of shaped text: the glyphs to draw, in draw order.
 *
 * Produced by `Font::shape` and immutable afterwards, which is what makes it cheap to share between a
 * measurement pass and a drawing pass. It holds no font and no reference to one, so it stays valid on
 * its own -- but it can only be drawn correctly by a font whose metrics match the one that shaped it.
 */
class TextBlob {
public:
  TextBlob() = default;

  explicit TextBlob(std::vector<Glyph> glyphs) noexcept: mGlyphs(std::move(glyphs)) {}

  [[nodiscard]] const std::vector<Glyph>& glyphs() const noexcept { return mGlyphs; }
  [[nodiscard]] size_t count() const noexcept { return mGlyphs.size(); }
  [[nodiscard]] bool isEmpty() const noexcept { return mGlyphs.empty(); }

  /**
   * The total advance of the run: how far the pen moves from start to finish.
   *
   * Not the same as the ink extent. A run can have a positive width and a negative left edge, because an
   * italic `f` overhangs its own origin, and a trailing space contributes width but no ink. This is the
   * measurement layout wants; a rectangle for clipping is a different question, which is what
   * @c bounds answers.
   */
  [[nodiscard]] Point advance() const noexcept;

  /**
   * The rectangle the run would occupy, relative to the pen origin.
   *
   * An approximation rather than an ink measurement: it is built from the advances and the font's
   * typographic extents, not from the outlines, so it is wide enough for a run of text but does not
   * account for an individual glyph overhanging. Exact bounds need the outlines, which this layer does
   * not produce; see the note on `Typeface`.
   */
  [[nodiscard]] Rect bounds(float ascent, float descent) const noexcept;

private:
  std::vector<Glyph> mGlyphs;
};

} // namespace graphics

#endif //CORAL_GRAPHICS_TEXT_BLOB_H
