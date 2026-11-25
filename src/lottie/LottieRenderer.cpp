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

/**
 * @file LottieRenderer.cpp
 *
 * The walk described in the header.
 */

#include "lottie/LottieRenderer.h"

#include "lottie/LottieGeometry.h"
#include "lottie/LottieTrim.h"

#include <graphics/Bitmap.h>
#include <graphics/Paint.h>
#include <graphics/Surface.h>
#include <graphics/Path.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace graphics::lottie {
namespace {

/** A half turn in radians, for the angles the document states in degrees. */
constexpr Scalar kPi = 3.14159265358979323846f;

/**
 * The layer rectangle for a group that needs one.
 *
 * A layer's bounds are only a clip -- the canvas intersects them with the clip already in force -- so this has to be
 * *large* rather than accurate. Working out the real extent would mean bounding the group's geometry, which is a
 * traversal per frame to compute a value that is then intersected away.
 */
const Rect kUnboundedLayer = Rect::MakeLTRB(-1.0e6f, -1.0e6f, 1.0e6f, 1.0e6f);

/** A paint carrying only an alpha, which is all a layer reads from it. */
Paint layerPaint(const Scalar alpha) {
  const auto byte = static_cast<uint8_t>(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
  Paint paint;
  paint.color = Color::MakeRGBA8(255, 255, 255, byte);
  return paint;
}

/** Appends @p paths into one, keeping the contours separate. */
Path splice(const std::vector<Path>& paths) {
  Path combined;
  for (const Path& path : paths) {
    if (path.isEmpty()) continue;
    combined.appendPath(path);
  }
  return combined;
}

/** The path operation a merge mode means, for the modes that are a boolean operation. */
std::optional<PathOp> mergeOperation(const MergeMode mode) {
  switch (mode) {
    case MergeMode::kUnion: return PathOp::Union;
    case MergeMode::kDifference: return PathOp::Difference;
    case MergeMode::kIntersection: return PathOp::Intersection;
    case MergeMode::kExclusion: return PathOp::Xor;
    case MergeMode::kMerge: break;
  }
  return std::nullopt;
}

/**
 * Combines @p paths as @p mode says.
 *
 * `kMerge` is the splice, which is the mode that matters: a document uses it to build a ring from two contours, and a
 * union of those two areas is the filled disc rather than the ring. The boolean modes fold from the left, which is what
 * Skottie's operand builder does.
 */
Path combinePaths(const std::vector<Path>& paths, const MergeMode mode) {
  if (paths.empty()) return Path {};
  if (paths.size() == 1) return paths.front();
  if (mode == MergeMode::kMerge) return splice(paths);

  const std::optional<PathOp> operation = mergeOperation(mode);
  if (!operation) return splice(paths);

  // A boolean operation on two paths can fail -- for operands that enclose no area, or whose crossings cannot be
  // resolved. The accumulated result is then left as it was, so a failed fold loses one operand rather than the whole
  // group, which is the difference between a slightly wrong gear and no gear.
  Path accumulated = paths.front();
  for (size_t i = 1; i < paths.size(); ++i) {
    const Path combined = op(accumulated, paths[i], *operation);
    if (!combined.isEmpty()) accumulated = combined;
  }
  return accumulated;
}

/** The colour a fill or stroke paints with, with its own opacity folded in. */
Color paintColor(const Shape& shape, const Scalar frame) {
  Color colour = shape.color.at(frame);
  // The format states a fill's opacity as a percentage, and a document that omits it means fully opaque.
  const Scalar opacity = shape.colorOpacity.isEmpty() ? 1.0f : std::clamp(shape.colorOpacity.at(frame) / 100.0f, 0.0f, 1.0f);
  colour.alpha *= opacity;
  return colour;
}

/**
 * Something to paint, held rather than painted.
 *
 * Painting is deferred because a repeater repeats *draws*: it takes everything painted so far in its group and draws it
 * again, transformed, which cannot be done to a canvas that has already been drawn to.
 *
 * One type rather than a hierarchy, for the same reason the shape model is one type: the alternatives are a path with a
 * paint, a group, and a repeater, and each is a case in one switch rather than a vtable.
 */
struct Draw {
  enum class Kind : uint8_t {
    /** A path with a paint. */
    kPath,
    /** A group: a transform, an opacity, and its own draws. */
    kGroup,
    /** A repeater: its children, painted `count` times. */
    kRepeater,
  };

  Kind kind = Kind::kPath;

  // A path draw.
  Path path;
  graphics::Paint paint;

  // A group or a repeater.
  std::vector<Draw> children;

  /** A group's transform, and its opacity -- the two things that would otherwise be a canvas state. */
  Matrix transform;
  Scalar opacity = 1.0f;

  /** A repeater's parameters, evaluated per frame at execution time. */
  Repeater repeater;
};

std::vector<Draw> collectShapeList(const std::vector<ShapePtr>& items, Scalar frame);

/**
 * @return the draw a group produces: its transform, its opacity, and its children in document order.
 *
 * The group's geometry does **not** reach its parent. That was worth checking rather than assuming: Skia's
 * `ShapeLayer.cpp` passes the parent's geometry vector into the recursive call, which reads like sharing -- but the
 * context never stores or reads it, and `attachShape` declares `geos` locally on every call. So a group is
 * self-contained, and a paint beside a group has nothing to paint.
 *
 * The consequence is visible in `trim_modes.json`, whose stroke sits outside the group holding the shapes: the document
 * draws only its background. That is the reference behaviour, not a gap here.
 */
Draw collectGroup(const Shape& group, const Scalar frame) {
  Draw draw;
  draw.kind = Draw::Kind::kGroup;
  draw.transform = group.transform.matrixAt(frame);
  // A group's opacity is a property of the group, not of each shape in it. Folding it into every fill would double it
  // wherever two shapes overlap, so it is kept here and becomes a layer at execution time -- the same reasoning as the
  // SVG layer's.
  draw.opacity = group.transform.opacityAt(frame);
  draw.children = collectShapeList(group.items, frame);
  return draw;
}

/**
 * @return the gradient's stops at @p frame, merged from the two runs the format writes.
 *
 * @param gradient the gradient, whose @c records hold the runs.
 * @param frame the layer-local time to evaluate the animatable array at.
 *
 * An empty result means the array is malformed -- too short for the colour records it claims -- and the caller then paints
 * nothing rather than guessing at a picture.
 */
std::vector<GradientStop> gradientStops(const Gradient& gradient, const Scalar frame) {
  const std::vector<Scalar> values = gradient.records.isEmpty() ? std::vector<Scalar> {} : gradient.records.at(frame);

  const size_t colorCount = static_cast<size_t>(std::max(0, gradient.colorRecords));
  const size_t colorSize = colorCount * 4;

  // The length has to be exactly the colour records plus a whole number of two-float opacity records. A partial trailing
  // record is a corrupt array, not a shorter gradient.
  if (values.size() < colorSize || (values.size() - colorSize) % 2 != 0) return {};
  const size_t opacityCount = (values.size() - colorSize) / 2;

  // The runs are read as records rather than as loose floats, which is the layout's whole shape: four floats per colour
  // stop with the position first.
  struct ColorRecord {
    Scalar position;
    Scalar red;
    Scalar green;
    Scalar blue;
  };
  struct OpacityRecord {
    Scalar position;
    Scalar alpha;
  };

  std::vector<ColorRecord> colors;
  colors.reserve(colorCount);
  for (size_t i = 0; i < colorCount; ++i) {
    const Scalar* const record = values.data() + i * 4;
    colors.push_back(ColorRecord {record[0], record[1], record[2], record[3]});
  }

  std::vector<OpacityRecord> opacities;
  opacities.reserve(opacityCount);
  for (size_t i = 0; i < opacityCount; ++i) {
    const Scalar* const record = values.data() + colorSize + i * 2;
    opacities.push_back(OpacityRecord {record[0], record[1]});
  }

  // The first output stop comes from whichever run is present, and its own position, so a gradient with no opacity run
  // is fully opaque rather than transparent.
  GradientStop current;
  current.position = 0;
  current.color = Color {colors.empty() ? 0.0f : colors.front().red,
                         colors.empty() ? 0.0f : colors.front().green,
                         colors.empty() ? 0.0f : colors.front().blue,
                         opacities.empty() ? 1.0f : opacities.front().alpha};

  size_t colorIndex = 0;
  size_t opacityIndex = 0;
  std::vector<GradientStop> stops;
  stops.reserve(colorCount + opacityCount);

  while (colorIndex < colors.size() || opacityIndex < opacities.size()) {
    // Once a run is exhausted, the other keeps going with the values computed so far -- as though its partner had a
    // record at the same position. Without this the loop would stop early and lose the tail of the longer run.
    const ColorRecord color = colorIndex < colors.size()
                                      ? colors[colorIndex]
                                      : ColorRecord {opacities[opacityIndex].position, current.color.red,
                                                     current.color.green, current.color.blue};
    const OpacityRecord opacity = opacityIndex < opacities.size()
                                          ? opacities[opacityIndex]
                                          : OpacityRecord {colors[colorIndex].position, current.color.alpha};

    const Scalar colorPosition = std::max(color.position, current.position);
    const Scalar opacityPosition = std::max(opacity.position, current.position);
    const Scalar colorRelative = colorPosition - current.position;
    const Scalar opacityRelative = opacityPosition - current.position;

    // The coefficients that select the nearer stop as-is and interpolate the further one towards it.
    const auto ratio = [](const Scalar numerator, const Scalar denominator) {
      if (denominator == 0) return Scalar {1};
      return std::clamp(numerator / denominator, Scalar {0}, Scalar {1});
    };
    const Scalar colorT = ratio(opacityRelative, colorRelative);
    const Scalar opacityT = ratio(colorRelative, opacityRelative);

    const auto lerp = [](const Scalar a, const Scalar b, const Scalar t) { return a + t * (b - a); };

    current.position = std::min(colorPosition, opacityPosition);
    current.color = Color {lerp(current.color.red, color.red, colorT), lerp(current.color.green, color.green, colorT),
                           lerp(current.color.blue, color.blue, colorT),
                           lerp(current.color.alpha, opacity.alpha, opacityT)};
    stops.push_back(current);

    // Both, when the positions coincide -- otherwise one of the two runs advances past a record that shares its position
    // and the next output stop is in the wrong place.
    if (colorPosition <= opacityPosition && colorIndex < colors.size()) ++colorIndex;
    if (opacityPosition <= colorPosition && opacityIndex < opacities.size()) ++opacityIndex;
  }

  return stops;
}

/**
 * @return a shader painting @p gradient, or null when it has no usable stops.
 *
 * A radial gradient's focus is moved along the axis by `h` and turned by `a`, which is the SVG focal point under another
 * name. This library's radial shader has no focal point, so the highlight is read and *not* applied -- recorded here
 * rather than silently dropped, because `skottie-gradient-radial-highlight.json` exists precisely to show the
 * difference.
 */
std::shared_ptr<Shader> gradientShader(const Gradient& gradient, const Scalar frame, const Scalar opacity) {
  std::vector<GradientStop> stops = gradientStops(gradient, frame);
  if (stops.empty()) return nullptr;

  // The shape's own opacity multiplies the gradient's, so it is folded into each stop's alpha. A shader has no separate
  // opacity, and applying it to the paint instead would be overridden by the shader.
  if (opacity < 1.0f) {
    for (GradientStop& stop : stops) stop.color.alpha *= opacity;
  }

  const Point start = gradient.start.isEmpty() ? Point {0, 0} : gradient.start.at(frame);
  const Point end = gradient.end.isEmpty() ? Point {0, 0} : gradient.end.at(frame);

  if (gradient.type == 2) {
    // Radial: the centre is the start point and the radius is how far the end point is from it. The format does not use
    // the end point as a point on the circle directly.
    const Scalar radius = std::hypot(end.x - start.x, end.y - start.y);
    if (!(radius > 0)) return nullptr;
    return Shader::RadialGradient(start, radius, std::move(stops), TileMode::kClamp);
  }

  // A linear gradient with a zero-length axis describes no direction. Refused rather than drawn, as the shader itself
  // would refuse it.
  if (start.x == end.x && start.y == end.y) return nullptr;
  return Shader::LinearGradient(start, end, std::move(stops), TileMode::kClamp);
}

/**
 * @return the transform copy @p index of @p repeater draws itself with.
 *
 * The three scalings by the copy index are the whole character of a repeater, and each is easy to mistranslate:
 *
 *   * the position is **multiplied** by `t`, so the copies march along a line rather than piling up;
 *   * the rotation is **multiplied** by `t`, so each copy turns a further `r` degrees -- a rotation of 10 with four
 *     copies gives 0, 10, 20 and 30 degrees;
 *   * the scale is **raised to** `t` rather than multiplied, so it compounds: a scale of 90% with four copies gives
 *     100%, 90%, 81% and 72.9%, not 100%, 90%, 80% and 70%.
 */
Matrix repeaterTransform(const Repeater& repeater, const Scalar frame, const size_t index) {
  // `o` slides the whole series: it shifts which copy is "the first one" without changing the spacing.
  const Scalar offset = repeater.offset.isEmpty() ? 0.0f : repeater.offset.at(frame);
  const Scalar t = offset + static_cast<Scalar>(index);

  const Point anchor = repeater.anchor.isEmpty() ? Point {0, 0} : repeater.anchor.at(frame);
  const Point position = repeater.position.isEmpty() ? Point {0, 0} : repeater.position.at(frame);
  const Point scalePercent = repeater.scale.isEmpty() ? Point {100, 100} : repeater.scale.at(frame);
  const Scalar rotation = repeater.rotation.isEmpty() ? 0.0f : repeater.rotation.at(frame);

  // A negative base raised to a fractional power is not a number, so a scale of zero or below is treated as unscaled
  // rather than allowed to produce a NaN matrix that would poison every point it touched.
  const auto compound = [t](const Scalar percent) {
    const Scalar base = percent / 100.0f;
    return base > 0.0f ? std::pow(base, t) : 1.0f;
  };

  return Matrix::MakeTranslation(Vector3 {t * position.x + anchor.x, t * position.y + anchor.y, 0}) *
         Matrix::MakeRotationZ(Radians {rotation * t * kPi / 180.0f}) *
         Matrix::MakeScale(Vector3 {compound(scalePercent.x), compound(scalePercent.y), 1}) *
         Matrix::MakeTranslation(Vector3 {-anchor.x, -anchor.y, 0});
}

void executeDraw(const Draw& draw, Canvas& canvas, Scalar frame, Scalar opacityScale);

/**
 * Paints a collected list, **bottom first**.
 *
 * The list is in document order, and the format lists a group's items top first -- so painting it forwards would put
 * the first-listed item at the bottom. Skia reverses at emit for the same reason ("Emit local draws reversed
 * (bottom->top, per spec)"), and this renderer did not until a repeater made the difference observable.
 */
void executeShapeList(const std::vector<Draw>& draws, Canvas& canvas, const Scalar frame, const Scalar opacityScale) {
  for (auto it = draws.rbegin(); it != draws.rend(); ++it) {
    executeDraw(*it, canvas, frame, opacityScale);
  }
}

void executeDraw(const Draw& draw, Canvas& canvas, const Scalar frame, const Scalar opacityScale) {
  switch (draw.kind) {
    case Draw::Kind::kPath: {
      if (draw.path.isEmpty()) return;

      // The opacity is folded into the paint's alpha rather than put through a layer: a single path has nothing to
      // isolate from, and the repeater below relies on this being cheap.
      graphics::Paint paint = draw.paint;
      paint.color.alpha *= opacityScale;
      if (!(paint.color.alpha > 0.0f)) return;
      canvas.drawPath(draw.path, paint);
      return;
    }

    case Draw::Kind::kGroup: {
      canvas.save();
      canvas.concat(draw.transform);

      const Scalar opacity = draw.opacity * opacityScale;
      const bool faded = opacity < 1.0f;
      if (faded) canvas.saveLayer(kUnboundedLayer, layerPaint(opacity));

      // The scale is *not* passed down: a group's children are at full opacity within it, and the group's own layer
      // carries the fade. Passing it down as well would apply it twice.
      executeShapeList(draw.children, canvas, frame, 1.0f);

      if (faded) canvas.restore();
      canvas.restore();
      return;
    }

    case Draw::Kind::kRepeater: {
      if (draw.children.empty()) return;

      // Rounded rather than truncated, and capped as Skottie caps it, so a corrupt document cannot ask for a million
      // copies.
      constexpr Scalar kMaxCount = 1024;
      const Scalar rawCount = draw.repeater.count.isEmpty() ? 0.0f : draw.repeater.count.at(frame);
      const size_t count = static_cast<size_t>(std::clamp(rawCount, 0.0f, kMaxCount) + 0.5f);
      if (count == 0) return;

      const auto percent = [&frame](const Animatable<Scalar>& property, const Scalar fallback) {
        const Scalar value = property.isEmpty() ? fallback : property.at(frame);
        return std::clamp(value / 100.0f, 0.0f, 1.0f);
      };
      const Scalar startOpacity = percent(draw.repeater.startOpacity, 100.0f);
      const Scalar endOpacity = percent(draw.repeater.endOpacity, 100.0f);

      // Divided by `count` rather than `count - 1`. Skottie divides by `count` too and marks it: "To cover the full
      // opacity range, the denominator below should be (fCount - 1). Interestingly, that's not what AE does." It is
      // reproduced because the reference does it, and the note is here so nobody corrects it by accident.
      const Scalar step = count > 1 ? (endOpacity - startOpacity) / static_cast<Scalar>(count) : 0.0f;

      for (size_t i = 0; i < count; ++i) {
        // `m == 1` paints the copies back to front, so the highest-numbered copy goes down first and the lowest is on
        // top. The same index also decides which end of the opacity ramp the copy gets, so reversing one reverses the
        // other.
        const size_t index = draw.repeater.below ? count - i - 1 : i;
        const Scalar opacity = startOpacity + step * static_cast<Scalar>(index);
        // A copy that is fully transparent is skipped rather than drawn: with a start opacity of zero that is most of
        // them, and each one would otherwise cost a matrix and a full pass over the children.
        if (!(opacity > 0.0f)) continue;

        canvas.save();
        canvas.concat(repeaterTransform(draw.repeater, frame, index));

        // The children are already in paint order -- the collector reversed them when the repeater wrapped them -- so
        // they are executed forwards rather than through `executeShapeList`.
        //
        // The opacity is folded into each child rather than isolated in a layer. Skottie isolates only when a repeater
        // has more than one child, and folding is the same result for the single child every repeater in this corpus
        // has; a layer per copy would be one full-canvas bitmap per copy, which for `bulge.json` is fifty.
        for (const Draw& child : draw.children) {
          executeDraw(child, canvas, frame, opacity * opacityScale);
        }
        canvas.restore();
      }
      return;
    }
  }
}

void renderShapeList(const std::vector<ShapePtr>& items, Canvas& canvas, const Scalar frame) {
  executeShapeList(collectShapeList(items, frame), canvas, frame, 1.0f);
}

std::vector<Draw> collectShapeList(const std::vector<ShapePtr>& items, const Scalar frame) {
  // The geometry accumulated so far, in document order, belonging to this list alone. A fill paints what is pending when
  // it is reached, which is what makes the order of a group's items meaningful -- and what makes a group a unit.
  //
  // A paint does not *clear* the stack: two strokes in a row paint the same geometry, which is what the format means.
  // Only a merge or a trim replaces it.
  std::vector<Path> pending;
  std::vector<Draw> draws;

  for (const ShapePtr& item : items) {
    if (item == nullptr || item->hidden) continue;

    switch (item->kind) {
      case ShapeKind::Path:
        pending.push_back(item->path.at(frame).toPath());
        break;

      // A parametric shape is geometry like any other: it produces a path and the fill or stroke that follows paints it.
      // The three kinds share a case because they differ only in how the path is computed, which is `geometryPath`'s
      // business rather than this loop's.
      case ShapeKind::Rectangle:
      case ShapeKind::Ellipse:
      case ShapeKind::Polystar:
        if (item->geometry) pending.push_back(geometryPath(*item->geometry, frame));
        break;

      case ShapeKind::Trim: {
        if (!item->trim || pending.empty()) break;

        const Scalar start = item->trim->start.isEmpty() ? 0.0f : item->trim->start.at(frame);
        const Scalar end = item->trim->end.isEmpty() ? 100.0f : item->trim->end.at(frame);
        const Scalar offset = item->trim->offset.isEmpty() ? 0.0f : item->trim->offset.at(frame);

        if (item->trim->trimsAsOnePath) {
          // Joined first, then cut as one path -- which is the option the format calls trimming "individually". The two
          // differ visibly the moment a group holds more than one shape: joined, the range spans them in order; apart,
          // every shape is cut to the same range of its own length.
          pending = {trimPath(splice(pending), start, end, offset)};
        } else {
          std::vector<Path> trimmed;
          trimmed.reserve(pending.size());
          for (const Path& geometry : pending) {
            trimmed.push_back(trimPath(geometry, start, end, offset));
          }
          pending = std::move(trimmed);
        }
        break;
      }

      case ShapeKind::Merge:
        pending = {combinePaths(pending, item->merge)};
        break;

      case ShapeKind::Fill: {
        const Path geometry = splice(pending);
        if (geometry.isEmpty()) break;

        Draw draw;
        draw.path = geometry;
        draw.path.setFillType(item->rule);
        draw.paint.color = paintColor(*item, frame);
        draw.paint.style = graphics::Paint::Style::kFill;
        draw.path.setFillType(item->rule);
        draws.push_back(std::move(draw));
        break;
      }

      case ShapeKind::GradientFill:
      case ShapeKind::GradientStroke: {
        if (!item->gradient) break;

        const Path geometry = splice(pending);
        if (geometry.isEmpty()) break;

        const bool stroked = item->kind == ShapeKind::GradientStroke;
        const Scalar width = item->strokeWidth.isEmpty() ? 0.0f : item->strokeWidth.at(frame);
        // A gradient stroke of no width is what a document writes for one it has faded out; a gradient fill has no width
        // to check.
        if (stroked && !(width > 0)) break;

        Path painted = geometry;
        if (!stroked) painted.setFillType(item->rule);

        const Scalar opacity = item->colorOpacity.isEmpty()
                                       ? 1.0f
                                       : std::clamp(item->colorOpacity.at(frame) / 100.0f, 0.0f, 1.0f);
        const std::shared_ptr<Shader> shader = gradientShader(*item->gradient, frame, opacity);
        if (shader == nullptr) break;

        Draw draw;
        draw.path = std::move(painted);
        draw.paint.color = Color {1, 1, 1, 1};
        draw.paint.shader = shader;
        draw.paint.style = stroked ? graphics::Paint::Style::kStroke : graphics::Paint::Style::kFill;
        if (stroked) draw.paint.strokeWidth = width;
        draws.push_back(std::move(draw));
        break;
      }

      case ShapeKind::Stroke: {
        const Path geometry = splice(pending);
        if (geometry.isEmpty()) break;

        const Scalar width = item->strokeWidth.isEmpty() ? 0.0f : item->strokeWidth.at(frame);
        // A stroke of no width is what a document writes for a stroke it has faded out rather than removed. Drawing it
        // would put a hairline where the reference has nothing.
        if (!(width > 0)) break;

        Draw draw;
        draw.path = geometry;
        draw.paint.color = paintColor(*item, frame);
        draw.paint.style = graphics::Paint::Style::kStroke;
        draw.paint.strokeWidth = width;
        draws.push_back(std::move(draw));
        break;
      }

      case ShapeKind::Group:
        // A nested group is self-contained: it produces one draw, and its geometry does not join the parent's pending
        // list. Adding the geometry would make a fill after the group paint the group's shapes a second time.
        draws.push_back(collectGroup(*item, frame));
        break;

      case ShapeKind::Repeater: {
        if (!item->repeater || draws.empty()) break;

        Draw repeater;
        repeater.kind = Draw::Kind::kRepeater;
        repeater.repeater = *item->repeater;

        // The collected draws are in document order -- top first -- and a repeater paints its copies bottom first, so
        // they are reversed into paint order here. Skottie's `AttachRepeaterDrawEffect` reverses at the same point and
        // says why: "input draws are in top->bottom order - reverse for paint order".
        std::reverse(draws.begin(), draws.end());
        repeater.children = std::move(draws);
        draws = {std::move(repeater)};
        break;
      }

      case ShapeKind::Transform:
        // Consumed by the group that owns it, which the parser lifted onto the group itself. Reaching one here means it
        // belongs to no group, and there is nothing for it to move.
        break;

      case ShapeKind::Unknown:
        break;
    }
  }

  return draws;
}

} // namespace anonymous

/** The composition's own coordinates mapped onto @p bounds. */
Matrix compositionMatrix(const Animation& animation, const Rect& bounds) {
  if (!(animation.width > 0) || !(animation.height > 0)) return Matrix {};
  return Matrix::MakeTranslation(Vector3 {bounds.getLeft(), bounds.getTop(), 0}) *
         Matrix::MakeScale(Vector3 {bounds.getWidth() / animation.width, bounds.getHeight() / animation.height, 1});
}

/** Draws what a layer holds, in the layer's own coordinates. The caller has already applied its transform. */
void drawLayerContent(const Layer& layer, Canvas& canvas, const Scalar frame) {
  switch (layer.kind) {
    case Layer::Kind::Shape:
      renderShapeList(layer.shapes, canvas, frame);
      break;

    case Layer::Kind::Solid: {
      // A solid is a rectangle at the layer's origin, of the size `sw` and `sh` give. Its anchor point is what centres it,
      // and that is already in the transform -- so the rectangle is drawn from (0, 0) rather than centred here.
      if (!(layer.solidWidth > 0) || !(layer.solidHeight > 0)) break;

      Paint paint;
      paint.color = layer.solidColor;
      paint.style = Paint::Style::kFill;
      canvas.drawRect(Rect::MakeLTRB(0, 0, layer.solidWidth, layer.solidHeight), paint);
      break;
    }

    case Layer::Kind::Other:
      // A precomposition, an image, a null or text: counted as a layer so that matte pairing keeps its positions, but
      // nothing is drawn for it.
      break;
  }
}

/**
 * @return @p coverage with its coverage inverted, in the channel @p mode reads.
 *
 * Un-premultiplied on the way in and premultiplied on the way out, because that is the accessor pair's contract -- and
 * because inverting a premultiplied colour is not inverting a colour.
 */
Bitmap invertedCoverage(const Bitmap& coverage, const MaskMode mode) {
  Bitmap inverted = coverage.makeDeepCopy();
  Pixmap pixels = inverted.makePixmap();
  if (pixels.isEmpty()) return inverted;

  for (int y = 0; y < pixels.height(); ++y) {
    for (int x = 0; x < pixels.width(); ++x) {
      const uint32_t pixel = pixels.getPixelARGB32(x, y);
      const uint32_t alpha = (pixel >> 24) & 0xFFu;

      if (mode == MaskMode::kAlpha) {
        // The colour is irrelevant to an alpha rule, so the result is left opaque white wherever the matte was
        // transparent. Only the alpha carries meaning here.
        pixels.setPixelARGB32(x, y, ((255u - alpha) << 24) | 0x00FFFFFFu);
        continue;
      }

      // A luminance rule reads `luminance(rgb) * alpha`, so what has to be inverted is that product -- which is the
      // colour *premultiplied*, not the colour on its own. Inverting the unpremultiplied colour and keeping the alpha
      // leaves a transparent region transparent, and a transparent region has zero luminance: an inverted matte would
      // then hide exactly the places it exists to reveal. That was the first version, and it drew the opposite picture.
      //
      // `luminance(255 - premultiplied) == 1 - luminance(premultiplied)` because the coefficients sum to one, so
      // inverting after premultiplication really is inverting the coverage -- and the result is made opaque so the alpha
      // term contributes a full one.
      const auto premultiply = [alpha](const uint32_t channel) { return (channel * alpha + 127u) / 255u; };
      const uint32_t red = 255u - premultiply((pixel >> 16) & 0xFFu);
      const uint32_t green = 255u - premultiply((pixel >> 8) & 0xFFu);
      const uint32_t blue = 255u - premultiply(pixel & 0xFFu);
      pixels.setPixelARGB32(x, y, 0xFF000000u | (red << 16) | (green << 8) | blue);
    }
  }
  return inverted;
}

/** @return @p coverage inverted per the track matte mode @p trackMatte names. 2 and 4 are the inverted ones. */
Bitmap invertedCoverageFor(const Bitmap& coverage, const int trackMatte) {
  const bool inverted = trackMatte == 2 || trackMatte == 4;
  if (!inverted) return coverage;
  return invertedCoverage(coverage, trackMatte >= 3 ? MaskMode::kLuminance : MaskMode::kAlpha);
}

void renderFrame(const Animation& animation, Canvas& canvas, const Rect& bounds, const Scalar frame) {
  if (animation.layers.empty()) return;

  // Clamped into the composition's own range: a player that has run past the end shows the last frame rather than
  // nothing, and one that has run before the start shows the first.
  const Scalar clamped = std::clamp(frame, animation.inPoint, animation.outPoint);

  canvas.save();

  // The composition's own size is mapped onto the destination. Without this a 128x128 document drawn into a 512x512
  // surface occupies a corner of it.
  if (animation.width > 0 && animation.height > 0) {
    canvas.concat(Matrix::MakeTranslation(Vector3 {bounds.getLeft(), bounds.getTop(), 0}) *
                  Matrix::MakeScale(Vector3 {bounds.getWidth() / animation.width,
                                             bounds.getHeight() / animation.height, 1}));
  }

  const Matrix composition = compositionMatrix(animation, bounds);

  // ## Two things about the layer list that are easy to get backwards
  //
  // **The order is front to back.** `layers[0]` is the *topmost*, so drawing them in array order paints the bottom layer
  // over the top one. The loop below therefore runs backwards.
  //
  // **A matte is "the layer above me"**, which in a front-to-back list is the *previous* entry. Consumption is worked out
  // in a pre-pass rather than while drawing: doing it inline marks a layer consumed only after it has already been
  // drawn, so every matte layer was painted twice -- once as content and once as a matte.
  std::vector<bool> consumed(animation.layers.size(), false);
  for (size_t index = 0; index < animation.layers.size(); ++index) {
    const Layer& layer = animation.layers[index];
    if (layer.trackMatte == 0 || index == 0) continue;
    const Layer& candidate = animation.layers[index - 1];
    if (clamped >= candidate.inPoint && clamped <= candidate.outPoint) consumed[index - 1] = true;
  }

  for (size_t step = animation.layers.size(); step-- > 0;) {
    const size_t index = step;
    if (consumed[index]) continue;

    const Layer& layer = animation.layers[index];

    // A layer's own range, which is not the composition's: a layer may start late or end early, and one outside its
    // range contributes nothing.
    if (clamped < layer.inPoint || clamped > layer.outPoint) continue;

    // `st` offsets the layer's time from the composition's, which is how a precomp or a delayed layer is written. For a
    // shape layer it is almost always zero, and reading it costs nothing.
    const Scalar localFrame = clamped - layer.startTime;

    // ## The matte, if this layer takes one
    //
    // `tt` on the first layer names nothing above it, which is a document error rather than a reason to fail; the layer
    // then draws unmatted. A matte outside its own time range likewise leaves the layer unmatted, because a matte with
    // nothing in it would hide everything.
    const Layer* matte = nullptr;
    Scalar matteFrame = 0;
    if (layer.trackMatte != 0 && index > 0 && consumed[index - 1]) {
      matte = &animation.layers[index - 1];
      matteFrame = clamped - matte->startTime;
    }

    // The matte's bitmap, in the composition's coordinates -- which is the space the content's layer bitmap lives in, so
    // the two agree without any further transform. Rendered before the layer is opened, because it has to exist before
    // the multiply.
    std::optional<Bitmap> coverage;
    if (matte != nullptr) {
      const Rect device = canvas.deviceBounds();
      auto matteSurface = Surface::makeRaster(static_cast<int>(device.getWidth()), static_cast<int>(device.getHeight()));
      if (matteSurface != nullptr) {
        if (const auto matteCanvas = matteSurface->getCanvas()) {
          matteCanvas->setTransform(composition);
          matteCanvas->concat(matte->transform.matrixAt(matteFrame));
          drawLayerContent(*matte, *matteCanvas, matteFrame);
        }
        coverage = matteSurface->detachBitmap();
      }
    }

    if (coverage) {
      // 3 and 4 are the luminance modes; 2 and 4 are the inverted ones.
      coverage = invertedCoverageFor(*coverage, layer.trackMatte);
      if (coverage->isEmpty()) coverage.reset();
    }

    canvas.save();
    canvas.concat(layer.transform.matrixAt(localFrame));

    const Scalar opacity = layer.transform.opacityAt(localFrame);
    const bool faded = opacity < 1.0f;
    // A matte needs a layer of its own to multiply even when the layer is fully opaque, so the layer is opened then too.
    // One `saveLayer` serves both the opacity and the matte: nesting them would square the composite.
    if (faded || coverage) canvas.saveLayer(kUnboundedLayer, layerPaint(faded ? opacity : 1.0f));

    drawLayerContent(layer, canvas, localFrame);

    if (coverage) {
      canvas.maskLayer(*coverage, layer.trackMatte >= 3 ? MaskMode::kLuminance : MaskMode::kAlpha);
    }

    if (faded || coverage) canvas.restore();
    canvas.restore();
  }

  canvas.restore();
}

void renderFrame(const Animation& animation, Canvas& canvas, const Scalar frame) {
  renderFrame(animation, canvas, Rect::MakeWH(animation.width, animation.height), frame);
}

} // namespace graphics::lottie
