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

#ifndef CORAL_GRAPHICS_SVG_SVG_TYPES_H
#define CORAL_GRAPHICS_SVG_SVG_TYPES_H

#include <geometry/Color.h>
#include <geometry/Matrix.h>
#include <geometry/Point.h>
#include <geometry/Rect.h>
#include <geometry/Scalar.h>
#include <graphics/Path.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * @file SvgTypes.h
 *
 * The values an SVG document is written in, and a parser for each.
 *
 * ## Why the values are their own types rather than the ones the renderer uses
 *
 * An SVG length is not a number. `50%` is a length, and so is `2em`, and what either comes to depends on where it is
 * used -- a percentage of the viewport for a `width`, of the diagonal for a `stroke-width`. Collapsing that to a float at
 * parse time would bake in the context before the element that supplies it has been seen, so `Length` carries the unit
 * and the renderer resolves it later with a `LengthContext`.
 *
 * The same reasoning holds for paint: `fill="url(#g)"` is not a colour, and `fill="currentColor"` is not known until the
 * `color` property is resolved.
 *
 * ## The parsers
 *
 * Every one is total: it either consumes the whole input and returns a value, or it returns nothing. Partial credit is
 * not offered, because `50px trailing junk` rendering as `50px` hides a malformed document, and SVG's error handling for
 * an unparsable value is to fall back to the property's initial value -- which is the caller's decision, not this
 * layer's.
 */

namespace graphics::svg {

// ==================================================================================================
// Enumerations
// ==================================================================================================
// Each pairs with a `parse` overload below and mirrors the SVG keyword set exactly, so the mapping from document text to
// enum is one table and not a chain of string comparisons at each use site.

enum class LengthUnit : uint8_t {
  Number,  ///< Unitless, which in SVG means user units.
  Percent, ///< Relative to the relevant viewport dimension, diagonal, or font size.
  Em,
  Ex,
  Px,
  Pt,
  Pc,
  Cm,
  Mm,
  In,
};

enum class LineCap : uint8_t { Butt, Round, Square };
enum class LineJoin : uint8_t { Miter, Round, Bevel };
enum class FillRule : uint8_t { NonZero, EvenOdd };
enum class SpreadMethod : uint8_t { Pad, Reflect, Repeat };
enum class Visibility : uint8_t { Visible, Hidden, Collapse };

/** Whether a coordinate is in user space or a fraction of the object's bounding box. */
enum class Units : uint8_t { UserSpaceOnUse, ObjectBoundingBox };

enum class TextAnchor : uint8_t { Start, Middle, End };

enum class FontStyle : uint8_t { Normal, Italic, Oblique };
enum class FontWeight : uint8_t { Normal, Bold, Bolder, Lighter, W100, W200, W300, W400, W500, W600, W700, W800, W900 };

/** How a viewBox is fitted into a viewport whose aspect ratio differs. */
struct PreserveAspectRatio {
  bool none = false;
  bool slice = false;
  enum class Align : uint8_t { None, XMinYMin, XMidYMin, XMaxYMin, XMinYMid, XMidYMid, XMaxYMid, XMinYMax, XMidYMax, XMaxYMax };
  Align align = Align::XMidYMid;
};

// ==================================================================================================
// Length
// ==================================================================================================

/** A number with a unit, unresolved. */
struct Length {
  Scalar value = 0;
  LengthUnit unit = LengthUnit::Number;

  [[nodiscard]] bool isPercent() const noexcept { return unit == LengthUnit::Percent; }
};

/**
 * The geometry a percentage or a font-relative unit is a fraction of.
 *
 * SVG resolves a length against different things depending on the property, which is why this is passed in rather than
 * captured: a `width` is a fraction of the viewport's width, a `stroke-width` of the viewport diagonal, an `x` on a
 * gradient of the bounding box.
 */
struct LengthContext {
  Scalar viewportWidth = 0;
  Scalar viewportHeight = 0;
  Scalar fontSize = 16;
  /** The font's x-height, which `ex` is a fraction of. */
  Scalar xHeight = 8;

  /** @return the diagonal, which percentages of "the other length" resolve against. */
  [[nodiscard]] Scalar diagonal() const noexcept;

  /** @return @p length in user units, or nothing when a percentage has no basis to resolve against. */
  [[nodiscard]] std::optional<Scalar> resolve(const Length& length, const LengthUnit axis) const;
};

// ==================================================================================================
// Paint
// ==================================================================================================

/** What a `fill` or `stroke` says. */
struct Paint {
  enum class Type : uint8_t {
    None,         ///< Draw nothing.
    Color,        ///< A specific colour.
    CurrentColor, ///< Whatever `color` resolved to.
    IRI,          ///< `url(#id)` -- a gradient, pattern or filter.
  };

  Type type = Type::None;
  Color color = Color::Black();
  std::string iri;
  /** A `url(#id)` fallback, as in `fill="url(#g) red"`. */
  std::optional<Color> fallback;
};

// ==================================================================================================
// Stroke dashes
// ==================================================================================================

struct DashArray {
  std::vector<Length> values;
  /** The sum of @p values, or infinity when the array is empty (a solid line). */
  [[nodiscard]] Scalar totalLength(const LengthContext& context) const;
};

// ==================================================================================================
// Parsers
// ==================================================================================================
// Each consumes the whole input, ignoring surrounding whitespace, and returns nothing when it does not match.

[[nodiscard]] std::optional<Length> parseLength(std::string_view text);
[[nodiscard]] std::optional<Scalar> parseNumber(std::string_view text);
[[nodiscard]] std::optional<Color> parseColor(std::string_view text);
[[nodiscard]] std::optional<Paint> parsePaint(std::string_view text);
[[nodiscard]] std::optional<DashArray> parseDashArray(std::string_view text);
[[nodiscard]] std::optional<std::vector<Point>> parsePoints(std::string_view text);
[[nodiscard]] std::optional<Matrix> parseTransform(std::string_view text);
[[nodiscard]] std::optional<PreserveAspectRatio> parsePreserveAspectRatio(std::string_view text);
[[nodiscard]] std::optional<Rect> parseViewBox(std::string_view text);
[[nodiscard]] std::optional<std::vector<Length>> parseLengthList(std::string_view text);

/**
 * Parses an SVG path `d` attribute.
 *
 * The whole of the corpus's geometry goes through here -- `path` is by far the most-used element in it -- so this is the
 * one parser where a subtle mistake is visible in every picture.
 *
 * @param text the attribute value, with or without the commands separated by whitespace.
 * @return the path, or nothing when the data is malformed. An empty path is returned for empty data rather than a
 *         failure, because an empty `d` is a legal way to draw nothing.
 */
[[nodiscard]] std::optional<Path> parsePathData(std::string_view text);

[[nodiscard]] std::optional<LineCap> parseLineCap(std::string_view text);
[[nodiscard]] std::optional<LineJoin> parseLineJoin(std::string_view text);
[[nodiscard]] std::optional<FillRule> parseFillRule(std::string_view text);
[[nodiscard]] std::optional<SpreadMethod> parseSpreadMethod(std::string_view text);
[[nodiscard]] std::optional<Visibility> parseVisibility(std::string_view text);
[[nodiscard]] std::optional<Units> parseUnits(std::string_view text);
[[nodiscard]] std::optional<TextAnchor> parseTextAnchor(std::string_view text);
[[nodiscard]] std::optional<FontStyle> parseFontStyle(std::string_view text);
[[nodiscard]] std::optional<FontWeight> parseFontWeight(std::string_view text);

/**
 * Fits @p viewBox into a viewport of @p width by @p height, honouring @p aspect.
 *
 * @return the transform to apply, plus the scale it produced so a caller can convert stroke widths and font sizes the
 *         way `vector-effect` and `non-scaling-stroke` decide they should be.
 */
struct ViewBoxTransform {
  Matrix matrix;
  Scalar scale = 1;
};
[[nodiscard]] ViewBoxTransform fitViewBox(const Rect& viewBox, Scalar width, Scalar height, const PreserveAspectRatio& aspect);

// ==================================================================================================
// Helpers
// ==================================================================================================

/** @return @p text with leading and trailing XML whitespace removed. */
[[nodiscard]] std::string_view trim(std::string_view text) noexcept;

/** True for the four characters SVG treats as whitespace. */
[[nodiscard]] bool isSvgSpace(char c) noexcept;

} // namespace graphics::svg

#endif // CORAL_GRAPHICS_SVG_SVG_TYPES_H
