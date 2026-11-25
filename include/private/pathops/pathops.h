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

#ifndef CORAL_GRAPHICS_PATHOPS_H
#define CORAL_GRAPHICS_PATHOPS_H

#include "pathops/Curve.h"

#include <optional>

/**
 * @file pathops.h
 *
 * Boolean operations on paths: combining two shapes into one, and flattening a self-overlapping shape
 * into an equivalent one that does not overlap itself.
 *
 * ## What this is for
 *
 * A vector renderer needs to answer questions like "what does the union of these two glyphs look
 * like", "what is left of this shape after subtracting that one", and "what is the outline of this
 * shape with its self-intersection resolved". Each of those is the same operation underneath:
 * intersect the two outlines, cut them at every crossing, and reassemble the pieces according to which
 * side of each edge is filled.
 *
 * ## The result is built from non-overlapping contours
 *
 * That property is what makes the output useful rather than merely correct. A caller wants to hand the
 * result to a rasterizer, an SVG writer or a hit-test without worrying that two contours run along the
 * same edge in opposite directions, or that the shape depends on the fill rule to look right. The
 * operations here resolve those cases away, which is also why a result can come back with a different
 * fill rule than its inputs -- the rule is how the shape is *described*, and there is only one
 * description that needs no rule.
 *
 * ## What `std::nullopt` means here
 *
 * Every operation in this header is implemented. The optional is not a "not written yet" marker -- it reports a
 * *degeneracy*: a path with no segments, or an operand whose outline could not be resolved at all. A caller that treats
 * `nullopt` as "this library cannot do booleans" will mislead itself, and the distinction matters because the two
 * demand different responses. A missing feature is a reason to work around the library; a degenerate input is a reason
 * to look at the input.
 *
 * ## How the traversal is arranged
 *
 * An operation collects both outlines as edges, cuts every edge at each of its crossings with any other, and then
 * decides each resulting piece by sampling a point a little to one side of it and counting how many edges the segment
 * from that sample to a distant point crosses. That is why the pieces are classified rather than derived: the
 * classification is the part that is easy to get wrong, and it is stated as a question about the geometry rather than
 * as a rule about which pieces to keep.
 *
 * The one piece of state it needs is that a sample a step away from a piece *does* cross that piece, and only when the
 * step landed on the side the piece bounds -- excluding the piece being classified reads as obviously right and gives
 * the wrong answer for a square's own edge. The note on `PathOpsBoolean.SimplifyResolvesASelfCrossing` records that,
 * along with three other diagnoses that were also wrong.
 */

namespace graphics::pathops {

/** The logical operations that combine two paths. */
enum class PathOp : uint8_t {
  /** The part of @c one that is not in @c two. */
  Difference,
  /** The part that is in both. */
  Intersection,
  /** The part that is in either. */
  Union,
  /** The part that is in exactly one of the two. */
  ExclusiveOr,
  /** The part of @c two that is not in @c one. The mirror of @c Difference. */
  ReverseDifference,
};

/** Human readable name of @p op, never null. */
[[nodiscard]] const char* toString(PathOp op) noexcept;

/**
 * @return the result of applying @p op to @p one and @p two.
 *
 * @return `std::nullopt` when the operation could not be performed -- for a degenerate operand, or a path with no
 *         segments. See the file comment: this is not a "not implemented" marker.
 */
[[nodiscard]] std::optional<Path> op(const Path& one, const Path& two, PathOp op);

/**
 * @return @p path as a set of contours that do not overlap each other or themselves.
 *
 * The useful preparation step before rasterising a shape a user drew freehand, and the operation that
 * makes a self-intersecting path safe to hand to something that assumes it is simple.
 */
[[nodiscard]] std::optional<Path> simplify(const Path& path);

/**
 * @return the tight bounds of @p path, or `std::nullopt` for a path with no segments.
 *
 * The bounds of the curves, not of the control points: a cubic whose control points lie far outside
 * the curve it draws has bounds much smaller than its control hull, and a caller computing a damage
 * region or a selection rectangle means the former.
 */
[[nodiscard]] std::optional<Rect> tightBounds(const Path& path) noexcept;

/**
 * @return @p path rewritten so that its fill rule is @c Winding and it fills the same area.
 *
 * A path filled by the even-odd rule can always be re-expressed as a winding path -- the regions where
 * an odd number of contours overlap become separate contours. The result is what a caller wants when
 * the destination only understands one rule.
 */
[[nodiscard]] std::optional<Path> asWinding(const Path& path);

/**
 * Applies a sequence of operations to a list of paths.
 *
 * Unioning many shapes one at a time recomputes the crossing structure of the accumulated result on
 * every step, which is quadratic in the number of inputs. The builder exists to make that cost
 * visible and to give the eventual implementation a place to amortise it.
 */
class OpBuilder {
public:
  /**
   * Adds @p path as the next operand.
   *
   * The builder starts empty, so the first @p op is applied against an empty path: `Union` therefore
   * makes the first path the starting accumulator, which is what a caller expects from "add this".
   */
  void add(const Path& path, PathOp op);

  /** @return the accumulated result, and resets the builder. */
  [[nodiscard]] std::optional<Path> resolve();

  void reset();

private:
  std::vector<Path> mPaths;
  std::vector<PathOp> mOps;
};

} // namespace graphics::pathops

#endif // CORAL_GRAPHICS_PATHOPS_H
