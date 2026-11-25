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

#ifndef CORAL_GRAPHICS_STROKE_OPTIONS_H
#define CORAL_GRAPHICS_STROKE_OPTIONS_H

#include <private/graphics/Flatten.h>

#include <graphics/Enums.h>
#include <graphics/Paint.h>

/**
 * @file StrokeOptions.h
 *
 * The one bridge between a `Paint` and the rasterizer's `StrokeStyle`.
 *
 * ## Why this is a separate header
 *
 * `Flatten.h` is the rasterizer's contract, and the rasterizer has no business knowing about colour, blending, shaders
 * or the rest of `Paint`. Its stroke inputs are therefore its own small enums. But the canvas *does* have a `Paint`,
 * and the two enums have to be connected somewhere. Putting the connection here -- rather than teaching the rasterizer
 * about `Paint`, or duplicating a switch in all three callers -- keeps the layering honest and makes it impossible for
 * one caller to map a cap differently from another.
 *
 * The scale is applied to the width and to nothing else. A cap, a join and a miter limit are geometric in every space:
 * a butt cap cuts at the path's own end whether the path is 1x or 2x. The width is the one length, and the caller
 * passes the factor that takes it from path space to the space the outline is computed in.
 */
namespace graphics::raster {

/** @return the rasterizer's spelling of a paint's cap. */
inline StrokeCap strokeCapFor(const Paint::Cap cap) noexcept {
  switch (cap) {
    case Paint::Cap::kButt: return StrokeCap::kButt;
    case Paint::Cap::kRound: return StrokeCap::kRound;
    case Paint::Cap::kSquare: return StrokeCap::kSquare;
  }
  return StrokeCap::kButt;
}

/** @return the rasterizer's spelling of a paint's join. */
inline StrokeJoin strokeJoinFor(const Paint::Join join) noexcept {
  switch (join) {
    case Paint::Join::kMiter: return StrokeJoin::kMiter;
    case Paint::Join::kRound: return StrokeJoin::kRound;
    case Paint::Join::kBevel: return StrokeJoin::kBevel;
  }
  return StrokeJoin::kMiter;
}

/**
 * @return everything about @p paint that changes a stroke's outline.
 *
 * @param widthScale the factor that takes the paint's width from the space it was written in to the space the outline
 *                   is computed in -- the device scale for a canvas, one for `StrokeRec`, which works in path space.
 */
inline StrokeStyle strokeStyleFor(const Paint& paint, const Scalar widthScale = 1.0f) noexcept {
  return StrokeStyle {
    paint.strokeWidth * widthScale,
    strokeCapFor(paint.strokeCap),
    strokeJoinFor(paint.strokeJoin),
    paint.strokeMiter,
  };
}

/**
 * @return the fill rule a stroke outline must be rasterised with.
 *
 * Always nonzero. The outline is a union of overlapping pieces, and the even-odd rule would subtract every overlap no
 * matter which way the pieces are wound -- a stroked line would come out empty. A caller that set the path's own fill
 * type to even-odd asked about filling the path, not about filling its outline.
 */
[[nodiscard]] inline FillType strokeFillType() noexcept { return FillType::Winding; }

} // namespace graphics::raster

#endif // CORAL_GRAPHICS_STROKE_OPTIONS_H
