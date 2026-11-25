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
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CORAL_GRAPHICS_LOTTIE_LOTTIETRIM_H
#define CORAL_GRAPHICS_LOTTIE_LOTTIETRIM_H

#include <graphics/Path.h>

/**
 * @file LottieTrim.h
 *
 * Lottie's trim path effect: keeping a range of a path's length and discarding the rest.
 *
 * Separate from the renderer because it is a self-contained operation on one path -- the "which pieces and in what
 * order" question belongs to the renderer, and the "cut this path between these two distances" question belongs here,
 * where it can be asked directly.
 */

namespace graphics::lottie {

/**
 * @return @p source trimmed to the range the three values describe.
 *
 * @param startPercent where the kept range begins, as a percentage of the path's length. `s` in the document.
 * @param endPercent where it ends. `e`.
 * @param offsetDegrees how far the whole range is slid along the path, in degrees -- a full turn being the whole path.
 *        `o`, and the unit is the format's own oddity rather than a mistake here.
 *
 * A range that covers the whole path returns it unchanged, and one that covers nothing returns an empty path; both are
 * ordinary answers rather than errors, because a trim animated from nothing to everything passes through both.
 *
 * The result is a polyline, exact to `PathMeasure`'s flattening tolerance -- see `PathMeasure::getSegment` for why, and
 * for what it costs against Skia, which keeps the original curves.
 */
[[nodiscard]] Path trimPath(const Path& source, Scalar startPercent, Scalar endPercent, Scalar offsetDegrees);

} // namespace graphics::lottie

#endif // CORAL_GRAPHICS_LOTTIE_LOTTIETRIM_H
