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
#include <cstdint>
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
 * Three callers need different things from the same points. A *fill* treats every contour as closed, because a fill
 * has to have an inside and an open contour is implicitly closed when it is filled. A *measurement* needs to know,
 * because the perimeter of a closed triangle is three edges and that of an open one traced through the same points is
 * two. A *stroke* needs it for something neither of the other two does: an open contour has caps at its ends and no
 * edge from the last point back to the first, so emitting that edge (which the first version did) draws a line nobody
 * asked for.
 *
 * Carrying the flag here rather than having each caller re-derive it from the path is what keeps them consistent, and
 * it is the reason `flattenToPolylines` does not simply always append the closing point. A contour that spells the
 * return as a final `lineTo` to its start is recorded as closed too: the geometry is the same, and the duplicate point
 * is dropped, so the flag is the only record that the closing edge exists.
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
 * How the end of an open subpath is finished.
 *
 * Mirrors `graphics::Paint::Cap`, but stated here rather than reaching into the painting API: the rasterizer offsets
 * geometry and knows nothing about colour, blending or shaders, and a stroke's cap is a fact about the outline.
 */
enum class StrokeCap : uint8_t {
  kButt,
  kRound,
  kSquare,
};

/** How the corner where two segments meet is finished. Mirrors `graphics::Paint::Join` for the same reason. */
enum class StrokeJoin : uint8_t {
  kMiter,
  kRound,
  kBevel,
};

/**
 * Everything about a stroke that changes the outline it produces.
 *
 * ## Why the width is not the only input any more
 *
 * The first version of `strokePath` took a width and hard-coded round joins and round caps. That was not a default, it
 * was the only possible result -- a caller that set `Paint::strokeCap` to butt, or a `stroke-linejoin="miter"` in an
 * SVG, silently got round geometry instead. Worse, the pieces it emitted did not all have the same orientation: the
 * edge quads and the join discs wound in opposite directions, so the nonzero fill that is supposed to union them
 * instead *cancelled* wherever they overlapped. A stroked circle -- a join at every flattened vertex -- came out as a
 * faint dashed ring, and a stroked rectangle had a hole at every corner. Passing the full style is what lets the
 * outline be built consistently instead of guessing.
 *
 * @see strokePath
 */
struct StrokeStyle {
  /** The stroke width, in the same units as the path. Non-positive produces no contours. */
  Scalar width = 0.0f;

  /** The cap applied to the ends of open subpaths. */
  StrokeCap cap = StrokeCap::kButt;

  /** The join applied where two segments of one contour meet. */
  StrokeJoin join = StrokeJoin::kMiter;

  /**
   * The multiple of the width past which a miter is bevelled instead of extended.
   *
   * Only read by a miter join. Clamped to at least one by the stroker, because a limit below one would bevel every
   * corner and is indistinguishable from asking for a bevel.
   */
  Scalar miterLimit = 4.0f;
};

/**
 * @return the closed contours of @p path expanded to the region a stroke of @p style would cover.
 *
 * The result is a fill: the outline of the stroke, as a set of closed contours. Rasterising it with the nonzero rule
 * gives the stroked shape, which is what lets one rasterizer serve both fill and stroke instead of two.
 *
 * ## Every piece is wound the same way, and that is load-bearing
 *
 * The outline is emitted as a union of simple, overlapping pieces: one quad per edge, one join per vertex and one cap
 * per open end. Two things make that union work, and neither is optional:
 *
 *  - every piece is emitted with the **same orientation**. The nonzero rule keeps a region covered when the windings
 *    add, but *cancels* it when they oppose, so a join disc wound against its edge quad punches a transparent hole
 *    exactly where the two overlap. That was a real defect here: it is invisible on a two-point line (whose edge quad
 *    is emitted twice, once for each direction, so the double winding survived one disc) and ruins every stroked
 *    circle and rounded rectangle.
 *  - the result must be filled with @c FillType::Winding. Under the even-odd rule the overlaps subtract by parity no
 *    matter which way they are wound. The canvas forces the rule for a stroke for that reason.
 *
 * The pieces are kept as separate contours rather than concatenated into one, because consecutive pieces are not
 * connected: joining them with a segment would cut a chord across the shape.
 *
 * ## The corners
 *
 * A round join is a disc at the vertex; a bevel is the triangle between the two outer offset points; a miter is the
 * kite up to the point where the two outer edges meet, kept only while the full miter length stays within
 * `miterLimit` stroke widths. A round cap is a disc at the end, a square cap extends the band by half a width, and a
 * butt cap adds nothing -- which is the default, and what a caller that never touched the cap should get.
 *
 * @param style the width, cap, join and miter limit. A non-positive width produces no contours.
 */
[[nodiscard]] std::vector<Contour> strokePath(const pathops::Path& path, const StrokeStyle& style,
  FlattenOptions options = {});

} // namespace graphics::raster

#endif // CORAL_GRAPHICS_FLATTEN_H
