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

#ifndef CORAL_GRAPHICS_FLATTEN_H
#define CORAL_GRAPHICS_FLATTEN_H

#include <geometry/Point.h>
#include <geometry/Rect.h>

#include "pathops/Curve.h"

#include <cstddef>
#include <vector>

/**
 * @file Flatten.h
 *
 * Turning curves into line segments, and stroke outlines into fillable contours.
 *
 * ## Why flatten at all
 *
 * A scanline rasterizer walks edges and asks "which pixels is this edge in". A curve has no single answer
 * to that -- it crosses a scanline once per monotonic piece -- so the general approach is to replace each
 * curve with a polyline fine enough that the difference is below what a pixel can show, and rasterise that.
 * The alternative, evaluating the curve's crossings exactly, is what Skia does for its analytic
 * anti-aliasing; it is faster and sharper, and it is a much larger piece of arithmetic. Flattening is the
 * honest first implementation: correct, simple, and with a visible limit that is worth stating rather than
 * hiding.
 *
 * ## The tolerance is in device pixels
 *
 * Every function here works in whatever space it is handed, and the caller is expected to have transformed
 * the geometry to device coordinates first. That ordering matters: flattening a curve in path space and
 * then scaling it up would leave too few segments, so a zoomed-in curve would be visibly faceted while the
 * same curve at 1:1 was smooth. Flattening after the transform makes the error a constant in pixels, which
 * is the only scale the viewer can see.
 */

namespace graphics::raster {

/**
 * A polyline produced by flattening.
 *
 * ## Why it records whether it was closed
 *
 * The rasterizer does not need to know: it treats every contour as closed, because a fill has to have an inside
 * and an open contour is implicitly closed when it is filled. A *measurement* does need to know, because the
 * perimeter of a closed triangle is three edges and that of an open one traced through the same points is two.
 * Carrying the flag here rather than having the measurer re-derive it from the path is what keeps the two
 * consistent, and it is the reason `flattenToPolylines` does not simply always append the closing point.
 */
struct Contour {
  std::vector<Point> points;

  /** True when the path closed this contour, so the edge from the last point back to the first is real. */
  bool closed = false;

  /** @return true when there is no area here, which is what a fill cares about. */
  [[nodiscard]] bool isEmpty() const noexcept { return points.size() < 3; }

  /** @return true when there is no length here, which is what a measurement cares about. */
  [[nodiscard]] bool hasLength() const noexcept { return points.size() >= 2; }

  /** @return the bounds of the points, or an empty rectangle when there are none. */
  [[nodiscard]] Rect bounds() const noexcept;
};

/**
 * @return the maximum deviation, in caller units, that a flattened segment may have from the curve.
 *
 * Set from the pixel grid rather than chosen: a quarter of a pixel is below what antialiasing can resolve,
 * so refining further costs segments and changes nothing visible. Public because the stroker needs the same
 * number to decide how finely to flatten before it offsets, and the two must agree or a stroked curve and a
 * filled one would disagree about where the curve is.
 */
constexpr Scalar kFlattenTolerance = 0.25f;

/**
 * How finely to flatten, and in what space the tolerance is measured.
 *
 * The tolerance is the only part that varies in practice, and it is a parameter rather than a constant because
 * the right value depends on the caller. A rasterizer flattens in device space and wants a fraction of a
 * *device* pixel; a measurer working in a caller's own coordinates wants a fraction of a pixel at the scale it
 * will eventually be drawn at, which is a different number whenever a transform is involved.
 */
struct FlattenOptions {
  Scalar tolerance = kFlattenTolerance;
};

/**
 * @return @p value if it is finite, else @p fallback.
 *
 * Flattening is a recursion with a floating-point termination test, and a coordinate that is not finite
 * makes that test never true -- so a NaN in a path turns a bounded recursion into an unbounded one that
 * exhausts the stack. Every coordinate entering the flattener is checked here, which is cheaper than
 * carrying a depth limit everywhere and does not silently truncate a legitimate curve.
 */
[[nodiscard]] Scalar sanitize(Scalar value, Scalar fallback = 0.0f) noexcept;

/** @return true when both coordinates of @p point are finite. */
[[nodiscard]] bool isFinite(const Point& point) noexcept;

/**
 * Appends the flattened form of a quadratic curve to @p out.
 *
 * @param from the current point, which is on the curve.
 * @param control the guide point, which is not.
 * @param to the end point.
 *
 * The start point is not appended -- the caller owns it -- so a chain of segments appends each one's end and
 * the polyline has no duplicated vertices.
 */
void flattenQuadratic(const Point& from, const Point& control, const Point& to, std::vector<Point>& out,
  FlattenOptions options = {});

/** Appends the flattened form of a cubic curve. As above, @p from is not re-appended. */
void flattenCubic(const Point& from, const Point& control1, const Point& control2, const Point& to,
  std::vector<Point>& out, FlattenOptions options = {});

/**
 * @return the path's polylines, keeping every contour that has at least one edge.
 *
 * The unfiltered form, for callers that care about length. @c flattenPath applies the stricter rule that a
 * fill needs -- three points, or there is no area -- which would discard the two-point contour that is a single
 * stroked line, the most common thing anybody measures or strokes.
 *
 * A contour's @c closed flag says whether the closing edge is real, so a measurer can include it and a
 * rasterizer can ignore the question.
 */
[[nodiscard]] std::vector<Contour> flattenToPolylines(const pathops::Path& path, FlattenOptions options = {});

/**
 * @return the closed contours of @p path, flattened and with each contour explicitly closed.
 *
 * Open contours are closed, because a fill has to have an inside. A contour whose last point already equals
 * its first does not get a duplicate -- a zero-length edge is not harmful to the rasterizer but it does
 * produce a degenerate edge that costs work and can confuse the direction bookkeeping.
 *
 * Degenerate contours -- fewer than three distinct points -- are dropped rather than returned, since a
 * contour with no area cannot contribute to a fill and would only widen the loop that walks them.
 */
[[nodiscard]] std::vector<Contour> flattenPath(const pathops::Path& path, FlattenOptions options = {});

/**
 * @return the closed contours of @p path expanded to the region a stroke of @p width would cover.
 *
 * The result is a fill: the outline of the stroke, as a set of closed contours. Rasterising it with the
 * nonzero rule gives the stroked shape, which is what lets one rasterizer serve both fill and stroke instead
 * of two.
 *
 * ## The corners
 *
 * Joins and caps are handled as follows, and the choice is visible in the output:
 *
 *  - **Joins** are round. A round join is an arc, flattened like any other curve; a miter join would need
 *    the miter limit and the spike it produces, and a bevel joint leaves a notch at every corner. Round is
 *    the least surprising default and the only one that needs no extra state in `Paint`.
 *  - **Caps** are butt for an unstroked path, which matches every other graphics API's default: the stroke
 *    ends exactly where the path does rather than extending beyond it.
 *
 * @param width the stroke width in caller units. Non-positive widths produce no contours, because a
 *              zero-width stroke is invisible rather than hairline -- hairline is a separate concept (a
 *              stroke that is one device pixel regardless of transform) and is not implemented here.
 */
[[nodiscard]] std::vector<Contour> strokePath(const pathops::Path& path, Scalar width,
  FlattenOptions options = {});

} // namespace graphics::raster

#endif // CORAL_GRAPHICS_FLATTEN_H
