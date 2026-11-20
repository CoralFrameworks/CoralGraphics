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

#ifndef CORAL_GRAPHICS_GPU_BLEND_ROUTING_H
#define CORAL_GRAPHICS_GPU_BLEND_ROUTING_H

#include <graphics/Enums.h>

#include <cstdint>

namespace graphics::gpu {

/**
 * @return true when @p mode is one a fixed-function blend unit can express with a factor pair.
 *
 * ## Why this is a shared predicate and not a constant in two files
 *
 * The boundary is not a detail of either side that consults it, it is the *contract between* them, and both sides have to
 * agree on the same number or the two disagree about a colour:
 *
 *  - `GraphiteCanvasBackend` asks it to decide whether a draw's paint has to be isolated in an offscreen layer, because a
 *    fragment stage cannot read the destination it is writing;
 *  - `GpuRenderer` asks it to decide whether a batch can be drawn with `BlendState` or has to be composited through
 *    `composite.frag`, and to report a list that names such a mode *outside* a layer as an omission rather than drawing
 *    it as source-over.
 *
 * The cut is `kScreen`, which is also where `composite.frag` puts its own `kLastBlendUnitMode`: every mode at or below it
 * has a coefficient pair (`graphics::BlendModeAsCoeff`) and every mode above it -- `kOverlay`, `kDarken`, `kLighten`,
 * `kColorDodge`, `kColorBurn`, `kHardLight`, `kSoftLight`, `kDifference`, `kExclusion`, `kMultiply` and the four
 * non-separable modes -- is defined by a function of *both* colours and cannot be reached by a factor pair.
 *
 * ## What the two halves of a disagreement look like
 *
 * A mode the unit cannot express, drawn through `blendStateFor`, lands on its source-over fallback: a plausible picture
 * that is simply the wrong colour, with nothing anywhere reporting it. That is not hypothetical -- it is what ten of the
 * demo's twelve blend swatches measured before this predicate was shared, and the cost of finding it was a comparison of
 * two rendered pictures rather than a failing test, because every CPU-side trace was correct.
 */
[[nodiscard]] constexpr bool isBlendUnitMode(const BlendMode mode) noexcept {
  return static_cast<uint8_t>(mode) <= static_cast<uint8_t>(BlendMode::kScreen);
}

} // namespace graphics::gpu

#endif // CORAL_GRAPHICS_GPU_BLEND_ROUTING_H
