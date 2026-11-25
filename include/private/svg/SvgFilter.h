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
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#ifndef CORAL_GRAPHICS_SVG_SVGFILTER_H
#define CORAL_GRAPHICS_SVG_SVGFILTER_H

#include "svg/SvgTypes.h"

#include <graphics/Bitmap.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/**
 * @file SvgFilter.h
 *
 * SVG's filter primitives: the `<fe*>` family and the graph they form.
 *
 * ## The graph is the work; each primitive is a per-pixel operation
 *
 * A filter is a chain, not a list. Each primitive reads one or two named inputs and publishes its output under a
 * `result` name, and later primitives refer to those names. Measured across this corpus: of 37 `in` references, 21 name
 * nothing at all -- which means "whatever the previous primitive produced" -- and the rest name `SourceGraphic`,
 * `SourceAlpha` or a `result` from earlier in the same filter. That chaining is what this file implements; the
 * primitives themselves are small once a chain exists.
 *
 * ## What the corpus needs
 *
 * Six primitives, measured: `feGaussianBlur` (14 uses), `feColorMatrix` (12), `feComposite` (10), `feBlend` (9),
 * `feOffset` (9) and `feFlood` (7), across five of the twenty-six documents. The rest of the family -- morphology,
 * turbulence, displacement, lighting, component transfer, merge, image -- is not implemented, and nothing in the corpus
 * asks for it.
 */

namespace graphics::svg {

/** One `<fe*>` primitive. */
struct FilterPrimitive {
  enum class Kind : uint8_t {
    ColorMatrix,
    Flood,
    Offset,
    /** `feGaussianBlur`. */
    Blur,
    Blend,
    Composite,
  };

  enum class ColorMatrixType : uint8_t {
    /** 20 numbers, four rows of five, applied to (r, g, b, a). The format's default. */
    Matrix,
    Saturate,
    HueRotate,
    LuminanceToAlpha,
  };

  /** The subset of blend modes the format names and this library has. */
  enum class Blend : uint8_t {
    Normal,
    Multiply,
    Screen,
    Darken,
    Lighten,
    Overlay,
    ColorDodge,
    ColorBurn,
    HardLight,
    SoftLight,
    Difference,
    Exclusion,
  };

  enum class Composite : uint8_t {
    /** Porter-Duff operations, plus the arithmetic one that is not a Porter-Duff operation at all. */
    Over,
    In,
    Out,
    Atop,
    Xor,
    Arithmetic,
  };

  Kind kind = Kind::ColorMatrix;

  /**
   * `in` and `in2`, as written. An empty `input` means "the previous primitive's result", which is the format's rule
   * and the case 21 of this corpus's 37 references fall into. An empty `input2` is the same, and a primitive that reads
   * two inputs with both unspecified reads the previous result twice.
   */
  std::string input;
  std::string input2;

  /** `result`: the name this primitive's output is published under. Empty means it is not named. */
  std::string result;

  // feColorMatrix.
  ColorMatrixType colorMatrixType = ColorMatrixType::Matrix;
  /** Row-major, four rows of five: the fifth column of each row is the constant term. */
  std::array<Scalar, 20> values {};

  // feFlood.
  Color floodColor;
  Scalar floodOpacity = 1.0f;

  // feOffset, in the primitive's user units.
  Scalar dx = 0;
  Scalar dy = 0;

  // feGaussianBlur. One value means both axes, which is the format's rule.
  Scalar deviationX = 0;
  Scalar deviationY = 0;

  // feBlend and feComposite.
  Blend blend = Blend::Normal;
  Composite composite = Composite::Over;
  /** `k1` to `k4`, for the arithmetic operator only. */
  Scalar k1 = 0;
  Scalar k2 = 0;
  Scalar k3 = 0;
  Scalar k4 = 0;

  /**
   * The primitive's own subregion, when it states one.
   *
   * The default is not a constant: it is the union of the subregions of this primitive's inputs, or the whole filter
   * region when any input is a source rather than a primitive. That rule is in @c resolveSubregion.
   */
  std::optional<Length> x;
  std::optional<Length> y;
  std::optional<Length> width;
  std::optional<Length> height;
};

/** A `<filter>`. */
struct Filter {
  std::vector<FilterPrimitive> primitives;

  /**
   * The filter effects region: what part of the canvas the filter may write to.
   *
   * All four default to a margin around the element -- `-10%` to `120%` of its box -- which is why a filter that states
   * none of them still changes something.
   */
  std::optional<Length> x;
  std::optional<Length> y;
  std::optional<Length> width;
  std::optional<Length> height;

  /** `filterUnits`: true when the region is fractions of the element's box. The format's default, and 9 of this
   *  corpus's 13 filters say `userSpaceOnUse` instead. */
  bool objectBoundingBox = true;

  [[nodiscard]] bool isEmpty() const noexcept { return primitives.empty(); }
};

/**
 * @return @p source with @p filter applied, or nothing when it cannot be built.
 *
 * @param source the element's own rendering, already in device space and the size of the canvas.
 * @param objectBounds the element's bounds in its own user space, which `objectBoundingBox` regions are fractions of.
 * @param context resolves the region's lengths. Its viewport is the document's.
 * @param scale the canvas's current scale, which converts the user-unit values -- `dx`, `dy` and the blur's deviation --
 *        into the device pixels the buffers are measured in. A blur of 5 user units is 10 device pixels at 2x.
 * @param deviceBounds the canvas's device rectangle, which is the size of every buffer here.
 */
[[nodiscard]] std::optional<Bitmap> applyFilter(const Filter& filter, const Bitmap& source, const Rect& objectBounds,
                                                const LengthContext& context, Scalar scale,
                                                const Rect& deviceBounds);

} // namespace graphics::svg

#endif // CORAL_GRAPHICS_SVG_SVGFILTER_H
