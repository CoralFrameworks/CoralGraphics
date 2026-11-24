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
 * See the License for the specific language governing permissions and limitations under the License.
 */

#include <graphics/StrokeRec.h>

#include <graphics/Path.h>

#include <private/graphics/Flatten.h>
#include <private/graphics/StrokeOptions.h>

#include <algorithm>
#include <cmath>

/**
 * @file StrokeRec.cpp
 *
 * The stroke parameters, and turning them into an outline.
 *
 * ## What the outline computation is not
 *
 * This class does not know how to offset a path. The rasterizer does, in `raster::strokePath`, because that is where
 * the flattening, the joins and the caps already live -- the canvas strokes through the same function. Duplicating the
 * geometry here would give two implementations that agree until one is fixed, so `applyToPath` converts and calls,
 * and the only thing this file owns is the parameter bookkeeping.
 *
 * ## The three places the reference's behaviour is surprising
 *
 *  - A stroke width of **zero** is not "draw nothing": it becomes a hairline, or a fill when the stroke was asked to
 *    fill as well. A caller that means "nothing" sets the style to fill and does not stroke.
 *  - A **hairline's outline depends on the device**, so it cannot be computed here at all. `applyToPath` reports that
 *    it did nothing, which makes a caller draw the path unstroked rather than draw a wrong outline confidently.
 *  - The **inflation radius** is a deliberate over-estimate. It is used to decide whether anything could be visible,
 *    so being too large costs a wasted test and being too small drops geometry.
 */

namespace graphics {
namespace {

/** The rasterizer's flattening tolerance, matching the one the canvas uses. */
constexpr float kFlattenTolerance = raster::kFlattenTolerance;

} // namespace anonymous

// ==================================================================================================
// Construction
// ==================================================================================================

StrokeRec::StrokeRec(const InitStyle style) noexcept {
  if (style == InitStyle::kHairline) {
    setHairlineStyle();
  } else {
    setFillStyle();
  }
}

StrokeRec::StrokeRec(const Paint& paint, const Scalar resScale) noexcept {
  init(paint, paint.style, resScale);
}

StrokeRec::StrokeRec(const Paint& paint, const Paint::Style style, const Scalar resScale) noexcept {
  init(paint, style, resScale);
}

void StrokeRec::init(const Paint& paint, const Paint::Style style, const Scalar resScale) noexcept {
  mWidth = paint.strokeWidth;
  mMiterLimit = paint.strokeMiter;
  mCap = paint.strokeCap;
  mJoin = paint.strokeJoin;
  setResScale(resScale);

  if (style == Paint::Style::kStroke) {
    // Through `setStrokeStyle`, so that the zero-width shorthand is applied in one place rather than two.
    setStrokeStyle(mWidth, false);
  } else {
    setFillStyle();
  }
}

// ==================================================================================================
// Styles
// ==================================================================================================

void StrokeRec::setFillStyle() noexcept {
  mStyle = Style::kFill;
  mStrokeAndFill = false;
}

void StrokeRec::setHairlineStyle() noexcept {
  mStyle = Style::kHairline;
  mStrokeAndFill = false;
}

void StrokeRec::setStrokeStyle(const Scalar width, const bool strokeAndFill) noexcept {
  if (!(width > 0.0f)) {
    // Zero or negative. With the fill requested this is a fill; without it, a hairline. `!(width > 0)` rather than
    // `width <= 0` so that a NaN takes this branch instead of falling through to a stroke of NaN width.
    if (strokeAndFill) {
      setFillStyle();
    } else {
      setHairlineStyle();
    }
    mWidth = 0.0f;
    return;
  }

  mWidth = width;
  mStrokeAndFill = strokeAndFill;
  mStyle = strokeAndFill ? Style::kStrokeAndFill : Style::kStroke;
}

void StrokeRec::setStrokeParams(const Paint::Cap cap, const Paint::Join join, const Scalar miterLimit) noexcept {
  mCap = cap;
  mJoin = join;
  // A miter limit below one would bevel every corner the moment the direction changed, which is what a bevel join is
  // for; clamping keeps the geometry well-defined for a caller that passed something meaningless.
  mMiterLimit = (miterLimit >= 1.0f) ? miterLimit : 1.0f;
}

void StrokeRec::setResScale(const Scalar scale) noexcept {
  if (!(scale > 0.0f) || !std::isfinite(scale)) return;
  mResScale = scale;
}

// ==================================================================================================
// Applying
// ==================================================================================================

bool StrokeRec::applyToPath(Path* dst, const Path& src) const {
  if (dst == nullptr) return false;

  // A fill draws the path as it is, and a hairline's outline is measured in device pixels which this layer has no
  // access to. Both leave the destination alone and report that they did. See the class comment.
  if (!needToApply()) return false;

  const auto* source = static_cast<const pathops::Path*>(src.pathOpsHandle());
  if (source == nullptr || source->isEmpty()) return false;

  // The outline is computed in the path's own space, so the stroke width is not scaled here: a stroke of two units is
  // two units wide however large the shape is drawn, and the transform is applied to the outline afterwards. Scaling
  // it here would be applying the transform twice.
  const raster::StrokeStyle style {
    mWidth,
    raster::strokeCapFor(mCap),
    raster::strokeJoinFor(mJoin),
    mMiterLimit,
  };
  const auto contours = raster::strokePath(*source, style, raster::FlattenOptions { kFlattenTolerance });
  if (contours.empty()) return false;

  pathops::Path outline;
  for (const raster::Contour& contour: contours) {
    if (contour.points.size() < 2) continue;
    outline.moveTo(contour.points.front());
    for (size_t i = 1; i < contour.points.size(); ++i) outline.lineTo(contour.points[i]);
    if (contour.closed) outline.close();
  }
  if (outline.isEmpty()) return false;

  // `kStrokeAndFill` fills the shape as well as outlining it, so the original path is unioned with the outline. A
  // union rather than an append: the two overlap wherever the stroke covers the shape's own edge, and appending would
  // leave the result depending on the fill rule of a path that a caller never asked to be combined.
  if (mStrokeAndFill) {
    // The public `op` rather than the internal one: it takes the two `Path`s the caller and this function already
    // have, and it reports failure the way the rest of the layer does. The call is the same expensive arrangement
    // either way, so there is nothing to gain from reaching past it.
    const Path outlinePath = Path::fromHandle(&outline);
    const Path united = op(outlinePath, src, PathOp::Union);
    if (!united.isEmpty()) {
      *dst = united;
      return true;
    }
    // The union failed, or produced nothing. Falling through to the outline alone is better than reporting nothing
    // to draw: the stroke is still correct, and only the fill beneath it is missing.
  }

  *dst = Path::fromHandle(&outline);
  return true;
}

void StrokeRec::applyToPaint(Paint* paint) const {
  if (paint == nullptr) return;

  // Only the stroke parameters are written. The colour, the blending and the antialiasing belong to the caller and
  // have nothing to do with how the outline was computed.
  paint->strokeWidth = mWidth;
  paint->strokeMiter = mMiterLimit;
  paint->strokeCap = mCap;
  paint->strokeJoin = mJoin;
  paint->style = needToApply() ? Paint::Style::kStroke : Paint::Style::kFill;
}

// ==================================================================================================
// Inflation
// ==================================================================================================

Scalar StrokeRec::inflationRadius() const noexcept {
  if (!needToApply()) return 0.0f;
  return InflationRadius(mJoin, mMiterLimit, mCap, mWidth);
}

Scalar StrokeRec::InflationRadius(const Paint& paint, const Paint::Style style) noexcept {
  // A fill inflates nothing. Signalled by a negative width, which is what the reference does and is safe here because
  // a negative width has already been given a meaning of its own by `setStrokeStyle`.
  const Scalar width = (style == Paint::Style::kFill) ? -1.0f : paint.strokeWidth;
  return InflationRadius(paint.strokeJoin, paint.strokeMiter, paint.strokeCap, width);
}

Scalar StrokeRec::InflationRadius(
        const Paint::Join join, const Scalar miterLimit, const Paint::Cap cap, const Scalar strokeWidth) noexcept {
  if (strokeWidth < 0.0f) return 0.0f;  // a fill

  // A zero-width stroke is a hairline, which is one device pixel wide whatever the transform. This cannot know the
  // transform, so it reports one unit: the smallest answer that is never an under-estimate on an unscaled device, and
  // an over-estimate anywhere else.
  if (strokeWidth == 0.0f) return 1.0f;

  // Half the width, times the furthest a corner can reach beyond the path.
  Scalar multiplier = 1.0f;
  if (join == Paint::Join::kMiter) {
    // A mitre can reach `miterLimit` times the half-width before it is bevelled.
    multiplier = std::max(multiplier, std::isfinite(miterLimit) ? miterLimit : 1.0f);
  }
  if (cap == Paint::Cap::kSquare) {
    // A square cap extends half the width along the direction of travel, so the corner of it reaches the half-width
    // times the diagonal of a unit square.
    multiplier = std::max(multiplier, 1.41421356f);
  }
  return strokeWidth / 2.0f * multiplier;
}

// ==================================================================================================
// Comparison
// ==================================================================================================

bool StrokeRec::hasEqualEffect(const StrokeRec& other) const noexcept {
  // Neither produces an outline, so the only question is whether they are the same style. A hairline and a fill both
  // leave the path alone but do different things with it, so they are not equal.
  if (!needToApply() || !other.needToApply()) return mStyle == other.mStyle;

  if (mWidth != other.mWidth) return false;
  if (mCap != other.mCap) return false;
  if (mJoin != other.mJoin) return false;
  if (mStrokeAndFill != other.mStrokeAndFill) return false;

  // The miter limit only reaches the geometry through a miter join. Two strokes that differ in a value nothing reads
  // produce the same outline, and a cache that treated them as different would split for no reason.
  if (mJoin == Paint::Join::kMiter && mMiterLimit != other.mMiterLimit) return false;

  // The resolution scale is not compared on purpose: it is a precision hint, so two strokes that differ only in it
  // are the same shape -- see the class comment.
  return true;
}

} // namespace graphics
