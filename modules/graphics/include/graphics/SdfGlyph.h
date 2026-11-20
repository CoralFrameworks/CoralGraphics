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

#ifndef CORAL_GRAPHICS_SDF_GLYPH_H
#define CORAL_GRAPHICS_SDF_GLYPH_H

#include <graphics/Path.h>

#include <cstdint>
#include <vector>

namespace graphics {

/**
 * A signed distance field for one glyph, ready to upload as a single-channel texture.
 *
 * The field is stored as 8-bit values: 128 is the outline, values above it are inside and values
 * below it are outside. The distance is clamped to @c padding pixels, so a shader can derive an
 * outline or a shadow from the same field without a second glyph rasterisation.
 *
 * This is the first step of the artistic-text work: an SDF is scale-independent within its padding,
 * one texture per glyph serves every fill, outline, shadow and border effect, and the glyph cache key
 * used by the GPU text route can address it exactly as it addresses the coverage bitmap.
 */
struct SdfGlyph {
  uint32_t width = 0;
  uint32_t height = 0;
  /** Pixels from the outline to the edge of the field; the shader's usable distance range. */
  float padding = 0.0f;
  /** Row-major 8-bit distance values, 128 at the outline. */
  std::vector<uint8_t> distance;
  /** The device-space rectangle the field covers. */
  float left = 0.0f;
  float top = 0.0f;

  [[nodiscard]] bool isEmpty() const noexcept { return width == 0 || height == 0; }
};

/**
 * Rasterises @p devicePath and turns its coverage into a signed distance field.
 *
 * @param devicePath The glyph outline in device pixels.
 * @param padding The distance range to keep, in pixels.
 * @param spread The number of pixels over which the edge is softened; 1 is a crisp edge.
 */
[[nodiscard]] SdfGlyph makeSdfGlyph(const Path& devicePath, float padding = 4.0f,
  float spread = 1.0f);

} // namespace graphics

#endif //CORAL_GRAPHICS_SDF_GLYPH_H
