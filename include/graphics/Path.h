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

#ifndef CORAL_GRAPHICS_PATH_H
#define CORAL_GRAPHICS_PATH_H

#include <graphics/Enums.h>

#include <geometry/Matrix.h>
#include <geometry/Point.h>

#include <functional>
#include <geometry/Rect.h>

#include <memory>
#include <cstdint>

namespace graphics {

/**
 * Forward declaration of the internal point type, so the private hook below can name it.
 *
 * Declared rather than included: `include/private/pathops/Curve.h` is internal, and pulling it in here would put the
 * private representation in front of every caller of this header. A forward declaration is enough for the
 * by-reference parameter, and the definition is only needed where the hook is implemented.
 */
namespace pathops {
struct DPoint;
} // namespace pathops

using pathops::DPoint;

/**
 * A shape: a sequence of lines and curves that may be open or closed.
 *
 * ## What this class is and is not
 *
 * It is the *public* path -- the one a caller builds, transforms, measures and asks about. The arithmetic
 * behind it lives in `src/pathops`, which is private, works in `double` rather than `Scalar`, and has a
 * path type of its own. The two are deliberately separate: the internal one is free to change shape as the
 * boolean operations land, while this one is what callers compile against.
 *
 * ## Why it is copyable but not a value type
 *
 * A path holds a variable number of verbs and points, so copying is not free. It is nonetheless copyable,
 * because a caller that wants to transform a copy rather than the original has no other convenient way to
 * say so, and paths in practice are small. The cost is one allocation per copy.
 *
 * ## Open and closed
 *
 * A contour is open until @c close is called on it, and each closes independently. The distinction is not
 * cosmetic: filling an open contour implicitly closes it, while stroking one does not, so the same points
 * produce a different shape depending on how the contour ends.
 */
class Path {
public:
  Path();
  ~Path();

  Path(const Path& other);
  Path& operator=(const Path& other);
  Path(Path&& other) noexcept;
  Path& operator=(Path&& other) noexcept;

  /**
   * Maps a point from the public float space into the internal double one, through a matrix.
   *
   * Declared here, and public, so that its type is complete wherever `mapPoints` is called; what the type
   * *does* is entirely internal and a caller outside this file has no reason to construct one. The canvas does,
   * because it needs exactly this: apply its own transform and widen the result for the rasterizer.
   */
  struct PathTransformer {
    Matrix matrix;

    /** @return @p point transformed and widened. */
    [[nodiscard]] DPoint operator()(const DPoint& point) const;
  };

  /**
   * The fill rule, which decides what "inside" means where a contour crosses itself.
   *
   * The two rules disagree only on self-intersecting shapes, which is exactly where a caller needs to be
   * able to say which one it meant -- and why this belongs to the path rather than being an argument to
   * fill.
   */
  [[nodiscard]] FillType fillType() const noexcept;
  void setFillType(FillType fillType) noexcept;

  /** @return true when the path has no verbs at all. */
  [[nodiscard]] bool isEmpty() const noexcept;

  /**
   * @return the number of verbs, which is not the number of points.
   *
   * Each verb carries its own number of points -- one for a line, two for a quadratic -- so this is the
   * count a caller iterating the path needs, not the count of coordinates.
   */
  [[nodiscard]] size_t verbCount() const noexcept;

  /**
   * @return the number of points the path holds.
   *
   * The sum of each verb's points, so it equals @c verbCount only for a path made entirely of lines. A
   * caller sizing a buffer from @c verbCount and then filling it with every point reads past the end,
   * which is why both counts exist.
   */
  [[nodiscard]] size_t pointCount() const noexcept;

  /** @return true when the path has any curve rather than only lines. */
  [[nodiscard]] bool hasCurves() const noexcept;

  /**
   * @return true when the path consists of nothing but non-overlapping rectangles.
   *
   * A caller that can rely on this may take a shortcut -- such a path rasterises directly and its fill is
   * trivially decidable. It is a question about the geometry, not about the verbs, so a rectangle spelled
   * out as four lines also counts.
   *
   * @param outRect when non-null and the answer is true, receives the rectangle.
   */
  [[nodiscard]] bool isRect(Rect* outRect = nullptr) const noexcept;

  /** @return true when the path contains no curve and no closing verb, i.e. a run of line segments. */
  [[nodiscard]] bool isLine() const noexcept;

  // ================================================================================================
  // Building
  // ================================================================================================

  /**
   * Starts a new contour at @p point.
   *
   * Every contour begins with this. A path may hold several, and they are filled as one shape -- which is
   * what makes a letter with a hole in it a single path rather than two.
   */
  void moveTo(Point point);

  /** Adds a line from the current point to @p point. */
  void lineTo(Point point);

  /**
   * Adds a quadratic curve whose shape is pulled toward @p control.
   *
   * The curve passes through the current point and @p point; @p control is a guide rather than a point on
   * the curve. That distinction is the whole reason the type is separate from a cubic.
   */
  void quadTo(Point control, Point point);

  /**
   * Adds a cubic curve pulled toward @p control1 and @p control2.
   *
   * The most general segment, and the one every other curve form can be expressed as: a quadratic is a
   * cubic with its controls placed two-thirds of the way to the single control point, and a circular arc is
   * approximated by up to four of them.
   */
  void cubicTo(Point control1, Point control2, Point point);

  /**
   * Closes the current contour back to the point its @c moveTo started from.
   *
   * Adds no geometry when the contour is already at that point, so calling it twice is harmless.
   */
  void close();

  /** Discards every verb, leaving an empty path. The fill type is kept. */
  void reset();

  // ================================================================================================
  // Asking about the geometry
  // ================================================================================================

  /**
   * @return a rectangle guaranteed to contain every point the path could draw.
   *
   * Built from the control points, so it is cheap and never too small -- a curve lies inside the hull of
   * its controls. Use it to answer "is this worth drawing at all", where a loose bound is fine.
   */
  [[nodiscard]] Rect bounds() const noexcept;

  /**
   * @return the tightest rectangle the path's *ink* actually reaches.
   *
   * Costs more than @c bounds, because it needs each curve's real extrema rather than its control hull. The
   * difference is not small: a curve's controls can sit far outside the curve, and a caller laying out
   * around the shape needs the shape rather than the hull.
   */
  [[nodiscard]] Rect tightBounds() const noexcept;

  /**
   * @return a hash of the path's *content*: verbs, points and fill rule.
   *
   * This is the identity a cache needs. `Path` has value semantics and a caller rebuilding the same
   * shape each frame produces a different object, so pointer identity misses exactly the case a clip
   * cache exists for. Two paths with equal verbs and points hash equally; a path that differs in one
   * point, the fill rule, or a curve's control point does not (barring a hash collision, which every
   * cache must tolerate).
   *
   * The hash is computed on demand and not cached: a path is usually a handful of segments, and a
   * mutable cache field would make copies share state in a way that is easy to get wrong. If a caller
   * ever hashes a large static path every frame, memoize at the call site.
   */
  [[nodiscard]] std::uint64_t contentHash() const noexcept;

  /**
   * @return true when @p point is inside the shape, using the path's fill type.
   *
   * Answers the region question, not the outline question: a point in the hole of a ring is outside, and a
   * point exactly on the outline is inside. The point is in the path's own coordinates -- transform the
   * point into that space rather than transforming the path, if that is cheaper.
   */
  [[nodiscard]] bool contains(Point point) const noexcept;

  /** @return the length of the outline, measured along the curves rather than as a polyline estimate. */
  [[nodiscard]] Scalar length() const noexcept;

  /** @return a copy with every contour's point order reversed. */
  /**
   * Appends @p other to this path, as a union.
   *
   * A union rather than a splice of the two sets of contours, because appending contours gives a path whose pieces
   * overlap wherever the two shapes did -- which is invisible for an opaque fill and a visible internal seam for a
   * stroke or a translucent one. The union removes those seams.
   *
   * The fill rule of @p other is not consulted; the result takes this path's.
   */
  void addPath(const Path& other);

  /**
   * Appends @p other's contours to this path, keeping them separate rather than uniting them.
   *
   * The difference from @c addPath is what happens where the two overlap. A union removes the shared region so the
   * result is a single outline; a splice keeps both contours and leaves the fill rule to decide what is inside. Neither
   * is the general case of the other, which is why both exist.
   *
   * The case that needs a splice is a ring built from two contours wound in opposite directions: a union fills the hole
   * in, because the union of the two enclosed areas *is* the disc. Lottie's "merge" paths modifier is specified as a
   * splice for exactly that reason, and documents use it to build exactly that ring -- so a renderer that united
   * instead would draw a solid disc where the reference draws a gear with a hollow centre.
   *
   * Note that this is the operation Skia spells `SkPath::addPath`, while this library's @c addPath unites instead.
   */
  void appendPath(const Path& other);

  [[nodiscard]] Path reversed() const;

  /** @return a copy with @p matrix applied to every point, including the control points. */
  [[nodiscard]] Path transformed(const Matrix& matrix) const;

  /** Applies @p matrix to every point in place. */
  void transform(const Matrix& matrix);

  /**
   * @return true when the two paths describe the same shape.
   *
   * A geometric comparison, not a structural one: the same rectangle walked clockwise and then
   * counter-clockwise, or spelled as four lines and then as a closed quad, are the same path. A structural
   * comparison would be cheaper and would answer a question nobody asks.
   */
  friend bool operator==(const Path& lhs, const Path& rhs) noexcept;

  friend bool operator!=(const Path& lhs, const Path& rhs) noexcept { return !(lhs == rhs); }

private:
  friend Path op(const Path&, const Path&, PathOp);
  friend Path simplify(const Path&);
  friend Path asWinding(const Path&);

public:
  /** A path over a handle another part of the graphics implementation already built. */
  [[nodiscard]] static Path fromExternalHandle(const void* handle) { return fromHandle(handle); }

private:
  friend class Canvas;
  friend class PathMeasure;
  // `StrokeRec` builds an outline from a path and hands one back, which is the same access `Canvas` needs and for the
  // same reason: the representation stays private to `src/`, so the two headers together still expose nothing.
  friend class StrokeRec;

  /**
   * A path over an implementation the caller already built.
   *
   * The pointer is opaque -- `const void*` rather than `const Impl*` -- so that the signature here does not name
   * the private representation. `Canvas` is the only caller and holds a `void*` it obtained from
   * @c pathOpsHandle, which means the two headers together still do not expose the representation to anything
   * outside `src/`.
   */
  [[nodiscard]] static Path fromHandle(const void* handle);

  /** @return an opaque pointer to the internal path, for a function that speaks its language. */
  [[nodiscard]] const void* pathOpsHandle() const noexcept;

  /**
   * Applies @p map to every point, control points included.
   *
   * Takes a concrete function type rather than being a template, so that its definition can live in the
   * implementation file. A template would have to be visible here and therefore recompile every caller whenever
   * its body changed -- and this is a private hook, so none of those callers exist.
   */
  void mapPoints(const std::function<void(DPoint&)>& map);

  /**
   * Replaces the geometry with @p path, which must be in the same coordinate space.
   *
   * Used by `Canvas` to swap in a device-space copy for rasterising, without it having to know the internal
   * representation. The `void*` is opaque for the same reason @c fromHandle's is.
   */
  void setPathOps(const void* handle);

  /**
   * The private representation, holding a `pathops::Path`.
   *
   * A `unique_ptr` to an incomplete type, for the same reason `Typeface` uses one: `include/private/pathops/Curve.h` is
   * internal, and including it here would make every caller of this header depend on the private
   * representation and recompile whenever it changed.
   */
  struct Impl;
  std::unique_ptr<Impl> mImpl;
};

/**
 * @return the region described by applying @p pathOp to @p first and @p second.
 *
 * ## Cost, and why it is a free function
 *
 * This is by far the most expensive operation in the path layer -- it intersects every segment of one path
 * with every segment of the other, then stitches the surviving pieces back into contours -- which is why it
 * is visible at the call site rather than hidden behind an operator.
 *
 * ## The result's fill type
 *
 * Always @c FillType::Winding, whatever the inputs used. The two fill rules disagree about what "inside" a
 * self-crossing contour means, and the operation has to resolve that disagreement to decide which pieces
 * survive -- so by the time it is done there is nothing left for the fill rule to decide.
 *
 * @return an empty path when the operation cannot be completed -- for a degenerate operand rather than for a missing
 *         feature. The boolean operations are implemented; see the note in `include/private/pathops/pathops.h` on what the failure
 *         case means.
 */
[[nodiscard]] Path op(const Path& first, const Path& second, PathOp pathOp);

/**
 * @return @p path with its self-intersections resolved, so that filling it no longer depends on the fill
 *         type.
 *
 * This is the other fill rule's answer, made explicit: a path that crosses itself under
 * @c FillType::EvenOdd becomes an equivalent path with no self-crossings, whose fill is then unambiguous.
 *
 * @return an empty path when the operation cannot be completed; see the note on @c op.
 */
[[nodiscard]] Path simplify(const Path& path);

/**
 * @return @p path converted to @c FillType::Winding by resolving its crossings.
 *
 * A no-op when it already is winding.
 *
 * @return an empty path when the conversion cannot be completed; see the note on @c op.
 */
[[nodiscard]] Path asWinding(const Path& path);

} // namespace graphics

#endif //CORAL_GRAPHICS_PATH_H
