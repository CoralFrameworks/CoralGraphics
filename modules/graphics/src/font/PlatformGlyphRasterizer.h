/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CORAL_GRAPHICS_FONT_PLATFORMGLYPHRASTERIZER_H
#define CORAL_GRAPHICS_FONT_PLATFORMGLYPHRASTERIZER_H

#include <graphics/Typeface.h>

#include <cstdint>
#include <string>

namespace graphics::font_detail {

/**
 * Rasterises one glyph through the platform's own text engine when it has one.
 *
 * The result is a `GlyphBitmap` with `subpixel = true` and RGB coverage, or an empty bitmap when the
 * platform has no backend or refuses this face. A `Typeface` calls this before its FreeType fallback;
 * the platform renderer is what supplies ClearType/CoreText-quality coverage, while the UI layer still
 * owns composition.
 */
[[nodiscard]] bool rasterizePlatformGlyph(const std::string& filePath, uint32_t faceIndex,
  uint32_t glyphId, float pixelSize, float phaseX, GlyphBitmap& out);

} // namespace graphics::font_detail

#endif // CORAL_GRAPHICS_FONT_PLATFORMGLYPHRASTERIZER_H
