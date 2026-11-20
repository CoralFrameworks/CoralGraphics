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

#ifndef CORAL_GRAPHICS_STROKE_REC_H
#define CORAL_GRAPHICS_STROKE_REC_H

#include <graphics/Paint.h>

#include <geometry/Scalar.h>

#include <cstdint>

namespace graphics {

class Path;

/**
 * The stroke parameters alone, separated from the colour and the rest of a `Paint`.
 *
 * ## Why this is a type of its own
 *
 * A `Paint` answers "how should this be drawn", and most of that answer is colour, blending and antialiasing, none of
 * which affects the *geometry*. Stroking does affect the geometry -- it turns a path into a different path -- and the
 * code that does so needs the stroke parameters without the rest. Passing a whole `Paint` would let it read the
 * colour and make the geometry depend on something it must not.
 *
 * Keeping the parameters separate also makes them comparable in the way that matters. Two strokes are the same when
 * they produce the same outline, which is what `hasEqualEffect` answers: it compares the parameters that reach the
 * geometry and deliberately ignores the resolution scale, which is a precision hint rather than a shape.
 *
 * ## The four styles
 *
 * `Fill` draws the path as it is; `Hairline` draws a line one device pixel wide whatever the transform, which is
 * useful for a grid and is not the same as a tiny stroke width; `Stroke` replaces the path with its outline; and
 * `StrokeAndFill` does both, leaving a filled shape with its own outline painted over it.
 *
 * A hairline is the one style this does not implement -- see `applyToPath`.
 */
class StrokeRec {
public:
  /** What a style of `Fill` or `Hairline` means, for a caller constructing one of the two directly. */
  enum class InitStyle : uint8_t {
    kHairline,
    kFill,
  };

  /** How the path is drawn. */
  enum class Style : uint8_t {
    kHairline,
    kFill,
    kStroke,
    kStrokeAndFill,

    kLast = kStrokeAndFill,
  };

  static constexpr int kStyleCount = static_cast<int>(Style::kLast) + 1;

  /**
   * @param style which of the two parameterless styles to start from.
   *
   * A stroke has no sensible default width, so there is no default constructor: a caller that wants a stroke says so
   * and gives the width, either through `setStrokeStyle` or from a paint.
   */
  explicit StrokeRec(InitStyle style) noexcept;

  /**
   * @param paint the paint to take the stroke parameters from.
   * @param style overrides the paint's own style, because a caller sometimes strokes a paint that is set to fill --
   *              for instance when measuring text, which is drawn filled but laid out as though stroked.
   * @param resScale the scale the outline will be drawn at, used only as a precision hint.
   */
  StrokeRec(const Paint& paint, Paint::Style style, Scalar resScale = 1.0f) noexcept;

  /** Takes the stroke parameters from @p paint and the style from @p paint's own. */
  explicit StrokeRec(const Paint& paint, Scalar resScale = 1.0f) noexcept;

  [[nodiscard]] Style style() const noexcept { return mStyle; }
  [[nodiscard]] Scalar width() const noexcept { return mWidth; }
  [[nodiscard]] Scalar miter() const noexcept { return mMiterLimit; }
  [[nodiscard]] Paint::Cap cap() const noexcept { return mCap; }
  [[nodiscard]] Paint::Join join() const noexcept { return mJoin; }

  [[nodiscard]] bool isHairlineStyle() const noexcept { return mStyle == Style::kHairline; }
  [[nodiscard]] bool isFillStyle() const noexcept { return mStyle == Style::kFill; }

  /**
   * @return true when applying this to a path would change it, which is the question a caller asks before paying for
   *         the outline. A fill or a hairline leaves the path alone, so the answer is false for both.
   */
  [[nodiscard]] bool needToApply() const noexcept {
    return mStyle == Style::kStroke || mStyle == Style::kStrokeAndFill;
  }

  void setFillStyle() noexcept;
  void setHairlineStyle() noexcept;

  /**
   * Requests a stroke of @p width.
   *
   * A width of zero is a request this cannot honour as written, and it is taken as the caller's shorthand rather than
   * as an error: with @p strokeAndFill it becomes a fill, and without it a hairline. That is the behaviour the
   * reference defines, and it is what makes `width = 0` mean "the thinnest line available" rather than "draw
   * nothing", which is what a literal reading would produce.
   */
  void setStrokeStyle(Scalar width, bool strokeAndFill = false) noexcept;

  void setStrokeParams(Paint::Cap cap, Paint::Join join, Scalar miterLimit) noexcept;

  /** The scale the outline will be drawn at. Only a precision hint, and never part of a stroke's identity. */
  [[nodiscard]] Scalar resScale() const noexcept { return mResScale; }

  /**
   * @param scale the scale to draw at; a non-positive or non-finite value is ignored.
   *
   * Ignored rather than stored, because a scale of zero would make every tolerance in the outline computation zero
   * and every curve flatten into an unbounded number of segments. The caller that passed it has a bug, and the useful
   * behaviour is to carry on at the previous scale rather than to hang.
   */
  void setResScale(Scalar scale) noexcept;

  /**
   * @return true when the stroke changed @p dst, false when the path was left alone.
   *
   * False means the style is fill or hairline, in which case @p dst is untouched and the caller should use @p src.
   * That is the contract the reference defines, and it is why the answer is returned rather than assumed: a caller
   * that ignored it would draw an empty path for every fill.
   *
   * A hairline is not implemented. Its outline depends on the device transform, which this layer does not have -- a
   * hairline is one *device* pixel wide and this works in the path's own space -- so the only honest answers are to
   * refuse it or to approximate it, and this returns false so that the caller draws the path unstroked rather than
   * drawing something that claims to be a hairline and is not.
   */
  bool applyToPath(Path* dst, const Path& src) const;

  /** Copies the stroke parameters back into @p paint, without touching its colour or its blending. */
  void applyToPaint(Paint* paint) const;

  /**
   * @return a radius that, added to a path's bounds and then stroked, would contain the outline.
   *
   * A conservative over-estimate rather than a tight bound, because it is used to decide how much to allocate and to
   * reject a path that is almost off-screen, and both are better served by being slightly too generous than by being
   * occasionally too small.
   */
  [[nodiscard]] Scalar inflationRadius() const noexcept;

  /** The same, for a paint and a style, without building a `StrokeRec`. */
  [[nodiscard]] static Scalar InflationRadius(const Paint& paint, Paint::Style style) noexcept;

  /** The same, from the parameters directly. */
  [[nodiscard]] static Scalar InflationRadius(
          Paint::Join join, Scalar miterLimit, Paint::Cap cap, Scalar strokeWidth) noexcept;

  /**
   * @return true when @p other would produce the same outline.
   *
   * The resolution scale is deliberately not compared: two strokes that differ only in the scale they are drawn at
   * produce outlines that are the same shape, and a cache keyed on this must not miss because of a hint.
   *
   * The miter limit is compared only when the join is a miter, because it is the only join that reads it. Two strokes
   * that differ in a value nothing uses are the same stroke, and treating them as different would split the cache for
   * no reason.
   */
  [[nodiscard]] bool hasEqualEffect(const StrokeRec& other) const noexcept;

private:
  void init(const Paint& paint, Paint::Style style, Scalar resScale) noexcept;

  Scalar mResScale = 1.0f;
  Scalar mWidth = 0.0f;
  Scalar mMiterLimit = 4.0f;
  Paint::Cap mCap = Paint::Cap::kButt;
  Paint::Join mJoin = Paint::Join::kMiter;
  Style mStyle = Style::kFill;
  bool mStrokeAndFill = false;
};

} // namespace graphics

#endif //CORAL_GRAPHICS_STROKE_REC_H
