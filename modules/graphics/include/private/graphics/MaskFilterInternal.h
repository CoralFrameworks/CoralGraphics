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

#ifndef CORAL_PRIVATE_GRAPHICS_MASK_FILTER_INTERNAL_H
#define CORAL_PRIVATE_GRAPHICS_MASK_FILTER_INTERNAL_H

#include <graphics/MaskFilter.h>
#include <private/graphics/Mask.h>

/**
 * @file MaskFilterInternal.h
 *
 * Applying a `MaskFilter` to a coverage mask.
 *
 * The public `MaskFilter` is parameters; this is the operation. They are apart because the operation works in
 * `mask::Mask`, which is a private type -- the public header must not name it, and the canvas that consumes the
 * result is the only other caller.
 *
 * This header is private to the project and is not installed.
 */

namespace graphics::mask_filter_detail {

/**
 * @return @p sharp with @p filter applied, in a mask that may be larger than the input.
 *
 * ## The four styles, in terms of two masks
 *
 * Writing `S` for the sharp coverage and `B` for the blurred one, where `B` has been computed over a padded frame so
 * that its spread is not cut off:
 *
 *  - **Normal** is `B`. The shape's edges soften and nothing else changes. On a thin shape this makes the whole thing
 *    translucent, because no part of it is far enough from an edge to stay at full coverage -- which is the behaviour
 *    that distinguishes it from Solid, and the reason both exist.
 *  - **Solid** is `B` plus the sharp shape. The interior stays fully covered and only the outside is soft, so a thin
 *    shape reads as solid rather than as a smear.
 *  - **Outer** is `B` minus `S`: only what spilled beyond the shape. This is the glow, and with the drawing offset,
 *    the drop shadow.
 *  - **Inner** is `B` intersected with `S`: the blurred falloff seen only where the shape is. This is the inner
 *    shadow, and it is why `intersected` takes the *smaller* of the two samples -- coverage outside the shape has to
 *    disappear rather than being added.
 *
 * The result carries the padded frame's origin, so a caller composites it where the shape actually is rather than
 * where the padding starts.
 */
[[nodiscard]] mask::Mask apply(const MaskFilter& filter, const mask::Mask& sharp);

} // namespace graphics::mask_filter_detail

#endif //CORAL_PRIVATE_GRAPHICS_MASK_FILTER_INTERNAL_H
