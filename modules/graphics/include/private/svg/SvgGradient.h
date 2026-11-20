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

#ifndef CORAL_GRAPHICS_SVG_SVG_GRADIENT_H
#define CORAL_GRAPHICS_SVG_SVG_GRADIENT_H

#include "svg/SvgTypes.h"

#include <graphics/Shader.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * @file SvgGradient.h
 *
 * `linearGradient` and `radialGradient` elements, and turning one into a shader.
 *
 * ## Why the definition and the shader are separate
 *
 * A gradient element states its geometry in a coordinate system that may be *the bounding box of whatever it fills*. That
 * box is not known when the element is parsed -- the same gradient may fill a circle and a rectangle, and must be a
 * different shader for each. So the parse produces a `Gradient`, which is a description, and the shader is built at draw
 * time from that description plus the shape's box. Building it once at parse time would bake in the first shape's
 * bounding box and paint every later shape with a gradient sized for the wrong one.
 *
 * ## The two coordinate systems
 *
 * `gradientUnits="objectBoundingBox"` -- the default -- writes the coordinates in a unit box, so `x2="100%"` means the
 * right edge of the shape whatever size it is. `userSpaceOnUse` writes them in the coordinates the document is drawn in.
 * The difference is a matrix, and it is the whole reason `Shader` carries a local matrix.
 */

/**
 * Declared here because this header is included before the one that owns the XML reader, and a signature naming
 * `xml::Node` needs the name to exist. Only a reference to it crosses this boundary.
 */
namespace graphics::xml {
class Node;
} // namespace graphics::xml

namespace graphics::svg {

/** A gradient element's description, as written. */
struct Gradient {
  enum class Kind : uint8_t {
    kLinear,
    kRadial,
  };

  /** Which box the coordinates are measured in. */
  enum class Units : uint8_t {
    kObjectBoundingBox,
    kUserSpaceOnUse,
  };

  Kind kind = Kind::kLinear;
  Units units = Units::kObjectBoundingBox;

  /** `spreadMethod`, mapped onto the canvas's tile modes. */
  TileMode spread = TileMode::kClamp;

  /** `gradientTransform`, applied in the gradient's own space. */
  Matrix transform;

  // Linear geometry. Absent means the initial value, which is what `buildShader` substitutes.
  std::optional<Length> x1;
  std::optional<Length> y1;
  std::optional<Length> x2;
  std::optional<Length> y2;

  // Radial geometry.
  std::optional<Length> cx;
  std::optional<Length> cy;
  std::optional<Length> r;
  std::optional<Length> fx;
  std::optional<Length> fy;

  /**
   * The stops, in the order they were written.
   *
   * Not sorted here: `Shader` sorts them, and doing it in one place means the two cannot disagree about what "sorted"
   * means for stops that share a position.
   */
  std::vector<GradientStop> stops;
};

/** Reads a `linearGradient` or `radialGradient` element. @p kind is decided by the caller, from the element's name. */
[[nodiscard]] Gradient parseGradient(const class xml::Node& element, Gradient::Kind kind);

/**
 * Builds a shader for @p gradient filling a shape whose bounding box is @p objectBounds.
 *
 * @param viewport the document's viewport, which is what a percentage in `userSpaceOnUse` resolves against.
 * @param objectBounds the shape's own box, which is what every coordinate resolves against under `objectBoundingBox`.
 * @return the shader, or nothing when the gradient has too few stops to interpolate between.
 */
[[nodiscard]] std::shared_ptr<Shader> buildShader(const Gradient& gradient, const Rect& objectBounds,
                                                 const LengthContext& viewport);

} // namespace graphics::svg

#endif // CORAL_GRAPHICS_SVG_SVG_GRADIENT_H
