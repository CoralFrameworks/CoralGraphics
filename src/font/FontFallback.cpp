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

#include <graphics/FontFallback.h>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace graphics {

namespace {

size_t sequenceLength(const std::string_view text, const size_t offset) {
  if (offset >= text.size()) return 0;
  const auto lead = static_cast<unsigned char>(text[offset]);
  if ((lead & 0x80u) == 0) return 1;
  if ((lead & 0xE0u) == 0xC0u) return 2;
  if ((lead & 0xF0u) == 0xE0u) return 3;
  if ((lead & 0xF8u) == 0xF0u) return 4;
  return 1;
}

uint32_t codePointAt(const std::string_view text, const size_t offset, const size_t length) {
  const auto lead = static_cast<unsigned char>(text[offset]);
  if (length == 1) return lead;
  if (length == 2) {
    return ((lead & 0x1Fu) << 6) | (static_cast<unsigned char>(text[offset + 1]) & 0x3Fu);
  }
  if (length == 3) {
    return ((lead & 0x0Fu) << 12) |
      ((static_cast<unsigned char>(text[offset + 1]) & 0x3Fu) << 6) |
      (static_cast<unsigned char>(text[offset + 2]) & 0x3Fu);
  }
  return ((lead & 0x07u) << 18) |
    ((static_cast<unsigned char>(text[offset + 1]) & 0x3Fu) << 12) |
    ((static_cast<unsigned char>(text[offset + 2]) & 0x3Fu) << 6) |
    (static_cast<unsigned char>(text[offset + 3]) & 0x3Fu);
}

float blobWidth(const Font& font, const TextBlob& blob) {
  const float em = static_cast<float>(font.typeface() != nullptr
    ? font.typeface()->unitsPerEm() : 0);
  return em > 0.0f ? blob.advance().x * font.size() / em : 0.0f;
}

} // namespace

FontFallback::FontFallback(std::shared_ptr<const Font> primary,
    std::vector<std::shared_ptr<const Font>> fallbacks)
    : mPrimary(std::move(primary)), mFallbacks(std::move(fallbacks)) {}

void FontFallback::addFallback(std::shared_ptr<const Font> font) {
  if (font != nullptr) mFallbacks.push_back(std::move(font));
}

const std::shared_ptr<const Font>& FontFallback::fontFor(const uint32_t codePoint) const {
  if (mPrimary != nullptr && mPrimary->typeface() != nullptr &&
      mPrimary->typeface()->hasGlyphForCodePoint(codePoint)) {
    return mPrimary;
  }
  for (const auto& fallback: mFallbacks) {
    if (fallback != nullptr && fallback->typeface() != nullptr &&
        fallback->typeface()->hasGlyphForCodePoint(codePoint)) {
      return fallback;
    }
  }
  return mPrimary;
}

std::vector<FontRun> FontFallback::shape(const std::string_view text) const {
  std::vector<FontRun> runs;
  if (text.empty() || mPrimary == nullptr) return runs;

  size_t offset = 0;
  std::string currentText;
  const Font* currentFont = nullptr;
  const auto flush = [&]() {
    if (currentText.empty() || currentFont == nullptr) return;
    auto blob = std::make_shared<const TextBlob>(currentFont->shape(currentText,
      Font::Direction::Auto));
    FontRun run;
    run.text = currentText;
    if (currentFont == mPrimary.get()) {
      run.font = mPrimary;
    } else {
      for (const auto& fallback: mFallbacks) {
        if (fallback != nullptr && fallback.get() == currentFont) {
          run.font = fallback;
          break;
        }
      }
    }
    if (run.font == nullptr) run.font = mPrimary;
    run.width = blobWidth(*currentFont, *blob);
    run.blob = std::move(blob);
    runs.push_back(std::move(run));
    currentText.clear();
    currentFont = nullptr;
  };

  while (offset < text.size()) {
    const size_t length = std::max<size_t>(1, sequenceLength(text, offset));
    const uint32_t cp = codePointAt(text, offset, length);
    const Font* selected = fontFor(cp).get();
    if (selected == nullptr) selected = mPrimary.get();
    if (currentFont != nullptr && currentFont != selected) flush();
    currentFont = selected;
    currentText.append(text.substr(offset, length));
    offset += length;
  }
  flush();
  return runs;
}

float FontFallback::measure(const std::string_view text) const {
  float width = 0.0f;
  for (const FontRun& run: shape(text)) width += run.width;
  return width;
}

} // namespace graphics
