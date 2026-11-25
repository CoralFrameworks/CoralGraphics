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
 * @file SvgGradient.cpp
 *
 * Reading gradient elements and building shaders from them.
 *
 * ## The bounding-box matrix
 *
 * Under `objectBoundingBox` the coordinates are in a unit square, so the matrix that carries them into the drawing is
 * the shape's box: translate to its corner, then scale by its size. `gradientTransform` is applied *within* that space,
 * so it goes on the right -- the other order would scale the transform's own translation by the box, which moves a
 * gradient that was meant to sit still.
 *
 * ## What a degenerate box means
 *
 * A shape with no width or no height has no box to scale into, and the matrix becomes singular. That is reported by the
 * shader as invalid rather than by a special case here, because the shader is where the inverse is taken and where the
 * consequence -- there is no colour to give -- belongs.
 */

#include "svg/SvgGradient.h"

#include "SvgScanner.h"

#include "svg/SvgPresentation.h"
#include "svg/SvgTypes.h"
#include "xml/Xml.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace graphics::svg {
namespace {

using xml::Node;

/** Maps `spreadMethod` onto the canvas's tile modes. */
TileMode toTileMode(const std::string_view value) {
  if (value == "repeat") return TileMode::kRepeat;
  if (value == "reflect") return TileMode::kMirror;
  // `pad` is the default and is what an unrecognised value gets: holding the end colours is the least surprising
  // reading of a spread the renderer does not know.
  return TileMode::kClamp;
}

/** One `<stop>`: its offset, its colour, and its opacity, which SVG writes as two separate properties. */
std::optional<GradientStop> parseStop(const Node& element) {
  const auto offset = element.attribute("offset");
  if (!offset) return std::nullopt;

  // An offset may be a fraction or a percentage. Clamped to [0,1]: a stop outside the range is a document error, and
  // clamping is what the specification asks for.
  Scalar position = 0;
  if (const auto length = parseLength(*offset)) {
    position = length->isPercent() ? length->value / 100.0f : length->value;
  } else {
    return std::nullopt;
  }

  // `stop-color` and `stop-opacity` are properties, so they may come from a `style` attribute as well as an attribute.
  // A `currentColor` stop-color resolves against `color`, which the gradient element inherits like anything else.
  Presentation presentation;
  LengthContext context;
  std::vector<Declaration> attributes;
  attributes.reserve(element.attributes().size());
  for (const xml::Attribute& attribute : element.attributes()) {
    attributes.emplace_back(attribute.name, attribute.value);
  }
  parsePresentation(element.name(), attributes, nullptr, context, presentation);

  // The presentation layer does not know `stop-color`, so it is read directly here.
  Color color = Color::Black();
  for (const Declaration& declaration : attributes) {
    if (declaration.first == "stop-color") {
      const std::string_view value = trimmed(declaration.second);
      if (value == "currentColor") {
        color = presentation.color.value_or(Color::Black());
      } else if (const auto parsed = parseColor(value)) {
        color = *parsed;
      }
    } else if (declaration.first == "style") {
      for (const auto& [property, propertyValue] : parseStyleAttribute(declaration.second)) {
        if (property != "stop-color") continue;
        if (const auto parsed = parseColor(propertyValue)) color = *parsed;
      }
    }
  }

  Scalar stopOpacity = 1;
  for (const Declaration& declaration : attributes) {
    const auto readOpacity = [&stopOpacity](const std::string_view text) {
      if (const auto value = parseLength(text)) {
        stopOpacity = std::clamp(value->isPercent() ? value->value / 100.0f : value->value, 0.0f, 1.0f);
      }
    };
    if (declaration.first == "stop-opacity") {
      readOpacity(declaration.second);
    } else if (declaration.first == "style") {
      for (const auto& [property, propertyValue] : parseStyleAttribute(declaration.second)) {
        if (property == "stop-opacity") readOpacity(propertyValue);
      }
    }
  }
  color.alpha *= stopOpacity;

  return GradientStop {std::clamp(position, 0.0f, 1.0f), color};
}

/** A geometry attribute, parsed as a length. */
void readLength(const Node& element, const char* name, std::optional<Length>& out) {
  if (const auto value = element.attribute(name)) out = parseLength(*value);
}

/**
 * Resolves @p length, or @p fallback when it is absent, in the units the gradient is written in.
 *
 * @param context the box the coordinates are measured in: the unit square for `objectBoundingBox`, the viewport for
 *        `userSpaceOnUse`.
 */
Scalar resolveOr(const std::optional<Length>& length, const Scalar fallback, const LengthContext& context,
                 const LengthUnit axis) {
  if (!length) return fallback;
  return context.resolve(*length, axis).value_or(fallback);
}

} // namespace anonymous

Gradient parseGradient(const Node& element, const Gradient::Kind kind) {
  Gradient gradient;
  gradient.kind = kind;

  if (const auto value = element.attribute("gradientUnits")) {
    gradient.units = *value == "userSpaceOnUse" ? Gradient::Units::kUserSpaceOnUse : Gradient::Units::kObjectBoundingBox;
  }
  if (const auto value = element.attribute("spreadMethod")) gradient.spread = toTileMode(*value);
  if (const auto value = element.attribute("gradientTransform")) {
    if (const auto transform = parseTransform(*value)) gradient.transform = *transform;
  }

  if (kind == Gradient::Kind::kLinear) {
    readLength(element, "x1", gradient.x1);
    readLength(element, "y1", gradient.y1);
    readLength(element, "x2", gradient.x2);
    readLength(element, "y2", gradient.y2);
  } else {
    readLength(element, "cx", gradient.cx);
    readLength(element, "cy", gradient.cy);
    readLength(element, "r", gradient.r);
    readLength(element, "fx", gradient.fx);
    readLength(element, "fy", gradient.fy);
  }

  for (const auto& child : element.children()) {
    if (!child->isElement() || child->name() != "stop") continue;
    if (const auto stop = parseStop(*child)) gradient.stops.push_back(*stop);
  }

  return gradient;
}

std::shared_ptr<Shader> buildShader(const Gradient& gradient, const Rect& objectBounds,
                                    const LengthContext& viewport) {
  // A gradient needs two stops to interpolate between. One stop is a flat colour, which the caller already has as the
  // fallback; nothing here is better than that.
  if (gradient.stops.size() < 2) return nullptr;

  // The coordinate space, and the matrix carrying it into the drawing.
  //
  // Under `objectBoundingBox` the coordinates are fractions of the shape's box, so the context is a unit square and the
  // matrix is the box itself. Under `userSpaceOnUse` the coordinates are already in drawing space, the context is the
  // viewport, and the matrix is the identity -- only `gradientTransform` moves anything.
  LengthContext context;
  Matrix box;
  if (gradient.units == Gradient::Units::kObjectBoundingBox) {
    context.viewportWidth = 1;
    context.viewportHeight = 1;
    box = Matrix::MakeTranslation(Vector3 {objectBounds.getLeft(), objectBounds.getTop(), 0}) *
          Matrix::MakeScale(Vector3 {objectBounds.getWidth(), objectBounds.getHeight(), 1});
  } else {
    context = viewport;
  }

  // `gradientTransform` is applied inside the gradient's own space, so it goes on the right of the box.
  const Matrix localMatrix = box * gradient.transform;

  if (gradient.kind == Gradient::Kind::kLinear) {
    // The initial values are x1=0%, y1=0%, x2=100%, y2=0%: a gradient across the box, left to right.
    const Scalar x1 = resolveOr(gradient.x1, 0.0f, context, LengthUnit::Px);
    const Scalar y1 = resolveOr(gradient.y1, 0.0f, context, LengthUnit::Px);
    const Scalar x2 = resolveOr(gradient.x2, 1.0f, context, LengthUnit::Px);
    const Scalar y2 = resolveOr(gradient.y2, 0.0f, context, LengthUnit::Px);
    return Shader::LinearGradient(Point {x1, y1}, Point {x2, y2}, gradient.stops, gradient.spread, localMatrix);
  }

  // The initial values are cx=cy=r=50%, so the gradient reaches the edge of the box's height. The radius resolves along
  // the diagonal, which is what makes an objectBoundingBox radial gradient circular in a square box and elliptical in a
  // non-square one -- which is what SVG asks for and what a reference renderer draws.
  const Scalar cx = resolveOr(gradient.cx, 0.5f, context, LengthUnit::Px);
  const Scalar cy = resolveOr(gradient.cy, 0.5f, context, LengthUnit::Px);
  const Scalar radius = resolveOr(gradient.r, 0.5f, context, LengthUnit::Px);
  (void)gradient.fx;
  (void)gradient.fy;
  return Shader::RadialGradient(Point {cx, cy}, radius, gradient.stops, gradient.spread, localMatrix);
}

} // namespace graphics::svg
