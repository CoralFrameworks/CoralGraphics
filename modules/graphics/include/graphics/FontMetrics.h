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

#ifndef CORAL_GRAPHICS_FONT_METRICS_H
#define CORAL_GRAPHICS_FONT_METRICS_H

#include <cstdint>

namespace graphics {

/**
 * The vertical measurements of a font at one size.
 *
 * ## Why the extents are separated
 *
 * Ascent and descent are not the same as "the top and bottom of a line of text", and conflating them is
 * the most common way a layout comes out subtly wrong. A font states several different vertical
 * extents, and each answers a different question:
 *
 *  - **ascent / descent** -- the typographic extents the designer intends a line to occupy. These are
 *    what line spacing is built from.
 *  - **capHeight / xHeight** -- where capital letters and lowercase x's actually reach. These are what
 *    vertical *centring* needs: centring a line of lowercase text by ascent puts it too low, because
 *    the space reserved above a capital is not where lowercase letters live.
 *  - **underline and strike-through offsets** -- properties of the design, not something to invent from
 *    the size. A renderer that guesses puts the line through the middle of the descenders.
 *
 * Every value is in the same units as the size the font was created at, so a metrics object belongs to
 * a scaled `Font` rather than to a `Typeface`.
 */
struct FontMetrics {
  /** Distance from the baseline to the top of the line, as the designer intends it. */
  float ascent = 0.0f;

  /**
   * Distance from the baseline to the bottom of the line.
   *
   * Positive, even though it describes space below the baseline. FreeType and every text layout system
   * express descent as a distance rather than as a signed offset, and a negative value here would
   * silently invert every line-height calculation built from `ascent + descent`.
   */
  float descent = 0.0f;

  /** Extra space the designer asks for between lines, over and above ascent + descent. */
  float lineGap = 0.0f;

  /** Height of a capital letter above the baseline. */
  float capHeight = 0.0f;

  /** Height of a lowercase x above the baseline. */
  float xHeight = 0.0f;

  /** How far below the baseline an underline sits. */
  float underlineOffset = 0.0f;

  /** How thick an underline is drawn. */
  float underlineThickness = 0.0f;

  /** How far above the baseline a strike-through sits. */
  float strikeThroughOffset = 0.0f;

  /** How thick a strike-through is drawn. */
  float strikeThroughThickness = 0.0f;

  /**
   * @return the distance between consecutive baselines.
   *
   * The sum rather than a choice among the three parts, because that is what the font asks for. A
   * caller wanting tighter or looser spacing scales this rather than recomputing it from the pieces.
   */
  [[nodiscard]] constexpr float lineHeight() const noexcept { return ascent + descent + lineGap; }

  friend constexpr bool operator==(const FontMetrics& lhs, const FontMetrics& rhs) noexcept {
    return lhs.ascent == rhs.ascent && lhs.descent == rhs.descent && lhs.lineGap == rhs.lineGap &&
           lhs.capHeight == rhs.capHeight && lhs.xHeight == rhs.xHeight;
  }

  friend constexpr bool operator!=(const FontMetrics& lhs, const FontMetrics& rhs) noexcept {
    return !(lhs == rhs);
  }
};

} // namespace graphics

#endif //CORAL_GRAPHICS_FONT_METRICS_H
