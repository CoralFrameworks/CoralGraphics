/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and limitations under the
 * License.
 */

#include <graphics/TextBlob.h>

#include <algorithm>

namespace graphics {

Point TextBlob::advance() const noexcept {
  Point total { 0.0f, 0.0f };
  for (const Glyph& glyph: mGlyphs) {
    total.x += glyph.advance.x;
    total.y += glyph.advance.y;
  }
  return total;
}

Rect TextBlob::bounds(const float ascent, const float descent) const noexcept {
  if (mGlyphs.empty()) return Rect::MakeLTRB(0.0f, 0.0f, 0.0f, 0.0f);

  // The running pen position, so that a glyph's own offset can be turned into an absolute position. A
  // mark glyph has a zero advance and a non-zero offset, so ignoring the pen would place every accent at
  // the origin.
  float pen = 0.0f;
  float left = 0.0f;
  float right = 0.0f;
  bool first = true;

  for (const Glyph& glyph: mGlyphs) {
    const float glyphLeft = pen + glyph.offset.x;
    const float glyphRight = glyphLeft + glyph.advance.x;

    if (first) {
      left = std::min(glyphLeft, glyphRight);
      right = std::max(glyphLeft, glyphRight);
      first = false;
    } else {
      left = std::min(left, std::min(glyphLeft, glyphRight));
      right = std::max(right, std::max(glyphLeft, glyphRight));
    }

    pen += glyph.advance.x;
  }

  // The vertical extent comes from the font's typographic metrics rather than from the glyphs, because
  // the outlines are not available at this layer. `ascent` is above the baseline and `descent` below it,
  // so the rectangle runs from -ascent to +descent in a coordinate system whose y grows downward --
  // which is why descent is the bottom rather than the top.
  return Rect::MakeLTRB(left, -ascent, right, descent);
}

} // namespace graphics
