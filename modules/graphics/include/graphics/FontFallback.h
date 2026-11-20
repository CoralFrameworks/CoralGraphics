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

#ifndef CORAL_GRAPHICS_FONTFALLBACK_H
#define CORAL_GRAPHICS_FONTFALLBACK_H

#include <graphics/Font.h>
#include <graphics/TextBlob.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace graphics {

/** One shaped run, all in one font; the text is a slice of the caller's original UTF-8. */
struct FontRun {
  std::string text;
  std::shared_ptr<const Font> font;
  std::shared_ptr<const TextBlob> blob;
  float width = 0.0f;
};

/**
 * A primary face plus faces used for characters the primary does not cover.
 *
 * Split by Unicode scalar value, not by script: a CJK character that the primary face lacks moves to the first
 * fallback that has a glyph for it, while the Latin around it stays in the primary. Shaping is per run, so a run
 * boundary loses kerning across the boundary -- acceptable for fallback, which is by definition the mixed case.
 */
class FontFallback {
public:
  explicit FontFallback(std::shared_ptr<const Font> primary,
    std::vector<std::shared_ptr<const Font>> fallbacks = {});

  [[nodiscard]] const std::shared_ptr<const Font>& primary() const noexcept {
    return mPrimary;
  }
  void addFallback(std::shared_ptr<const Font> font);

  /** @return runs in visual text order, each with its own shaped blob and measured width. */
  [[nodiscard]] std::vector<FontRun> shape(std::string_view text) const;

  /** @return the sum of the run widths, in logical units. */
  [[nodiscard]] float measure(std::string_view text) const;

  /** @return the face used for @p codePoint, or the primary when no face covers it. */
  [[nodiscard]] const std::shared_ptr<const Font>& fontFor(uint32_t codePoint) const;

private:
  std::shared_ptr<const Font> mPrimary;
  std::vector<std::shared_ptr<const Font>> mFallbacks;
};

} // namespace graphics

#endif // CORAL_GRAPHICS_FONTFALLBACK_H
