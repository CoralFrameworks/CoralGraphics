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
 * @file SvgDom.cpp
 *
 * Turning a parsed XML tree into the document's node tree.
 *
 * ## The shape of the build
 *
 * One recursive function per level, dispatching on the element name. Anything unrecognised becomes a group when it has
 * element children and is dropped when it does not -- which is the forgiving reading of an unknown element, and the one
 * that renders a document using a feature this implementation does not have as correctly as it can rather than not at
 * all.
 *
 * ## What is measured before it is written
 *
 * The corpus was scanned for the elements and attributes it actually uses before any of this was written, and the
 * dispatch below is that list. Elements outside it -- `marker`, `pattern`, `animate` -- are recognised as definitions or
 * ignored, rather than half-implemented; a half-implemented `pattern` renders as a solid rectangle where the reference
 * has a texture, which is worse than the shape being unfilled because it looks intentional.
 */

#include "svg/SvgDocument.h"

#include <graphics/Image.h>

#include "SvgScanner.h"

#include "svg/SvgDataUri.h"
#include "svg/SvgFilter.h"
#include "svg/SvgGradient.h"
#include "svg/SvgPresentation.h"

#include "xml/Xml.h"

#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace graphics::svg {
namespace {

/** The elements whose only purpose is to be referred to, so they are built but never drawn in place. */
bool isDefinition(const std::string_view name) {
  return name == "defs" || name == "symbol" || name == "linearGradient" || name == "radialGradient" ||
         name == "pattern" || name == "clipPath" || name == "mask" || name == "filter" || name == "style" ||
         name == "marker" || name == "title" || name == "desc" || name == "metadata";
}

/** A length attribute, resolved against the viewport. Absent returns @p fallback. */
Scalar lengthAttribute(const xml::Node& element, const std::string_view name, const LengthContext& context,
                       const Scalar fallback = 0) {
  const auto value = element.attribute(name);
  if (!value) return fallback;
  const auto length = parseLength(*value);
  if (!length) return fallback;
  return context.resolve(*length, LengthUnit::Px).value_or(fallback);
}

/**
 * Reads a length attribute without resolving it.
 *
 * Deliberately unresolved: a mask's region and a gradient's coordinates are written in units that depend on the element
 * they are applied to, so the numbers have to survive parsing and be resolved against a box that does not exist yet.
 * `lengthAttribute` below is the other half of this -- the one that resolves against a context already in hand.
 */
void readLength(const xml::Node& element, const char* name, std::optional<Length>& out) {
  if (const auto value = element.attribute(name)) {
    if (const auto parsed = parseLength(*value)) out = parsed;
  }
}

// ==================================================================================================
// Geometry
// ==================================================================================================

/** The cubic constant for a quarter circle: 4/3 * tan(pi/8). */
constexpr Scalar kQuarterCircle = 0.5522847498f;

/** `<rect>`, with SVG's rules for a missing or non-positive size and for the two corner radii. */
Path rectPath(const xml::Node& element, const LengthContext& context) {
  const Scalar x = lengthAttribute(element, "x", context);
  const Scalar y = lengthAttribute(element, "y", context);
  const Scalar width = lengthAttribute(element, "width", context);
  const Scalar height = lengthAttribute(element, "height", context);

  Path path;
  // A non-positive width or height disables rendering of the element rather than drawing a degenerate rectangle.
  if (!(width > 0) || !(height > 0)) return path;

  Scalar rx = lengthAttribute(element, "rx", context, -1);
  Scalar ry = lengthAttribute(element, "ry", context, -1);
  // Either radius alone means both take that value; neither means square corners. That asymmetry is in the grammar.
  if (rx < 0 && ry < 0) {
    rx = 0;
    ry = 0;
  } else if (rx < 0) {
    rx = ry;
  } else if (ry < 0) {
    ry = rx;
  }
  rx = std::min(rx, width / 2);
  ry = std::min(ry, height / 2);

  if (!(rx > 0) || !(ry > 0)) {
    path.moveTo(Point {x, y});
    path.lineTo(Point {x + width, y});
    path.lineTo(Point {x + width, y + height});
    path.lineTo(Point {x, y + height});
    path.close();
    return path;
  }

  const Scalar cx = rx * kQuarterCircle;
  const Scalar cy = ry * kQuarterCircle;

  path.moveTo(Point {x + rx, y});
  path.lineTo(Point {x + width - rx, y});
  path.cubicTo(Point {x + width - rx + cx, y}, Point {x + width, y + ry - cy}, Point {x + width, y + ry});
  path.lineTo(Point {x + width, y + height - ry});
  path.cubicTo(Point {x + width, y + height - ry + cy}, Point {x + width - rx + cx, y + height},
               Point {x + width - rx, y + height});
  path.lineTo(Point {x + rx, y + height});
  path.cubicTo(Point {x + rx - cx, y + height}, Point {x, y + height - ry + cy}, Point {x, y + height - ry});
  path.lineTo(Point {x, y + ry});
  path.cubicTo(Point {x, y + ry - cy}, Point {x + rx - cx, y}, Point {x + rx, y});
  path.close();
  return path;
}

/** `<circle>` and `<ellipse>`, both as four cubic quarter arcs. */
Path ellipsePath(const Scalar cx, const Scalar cy, const Scalar rx, const Scalar ry) {
  Path path;
  if (!(rx > 0) || !(ry > 0)) return path;

  const Scalar ox = rx * kQuarterCircle;
  const Scalar oy = ry * kQuarterCircle;

  path.moveTo(Point {cx + rx, cy});
  path.cubicTo(Point {cx + rx, cy + oy}, Point {cx + ox, cy + ry}, Point {cx, cy + ry});
  path.cubicTo(Point {cx - ox, cy + ry}, Point {cx - rx, cy + oy}, Point {cx - rx, cy});
  path.cubicTo(Point {cx - rx, cy - oy}, Point {cx - ox, cy - ry}, Point {cx, cy - ry});
  path.cubicTo(Point {cx + ox, cy - ry}, Point {cx + rx, cy - oy}, Point {cx + rx, cy});
  path.close();
  return path;
}

/** `<polygon>` and `<polyline>`: the same points, closed or open. */
Path pointsPath(const xml::Node& element, const bool close) {
  Path path;
  const auto value = element.attribute("points");
  if (!value) return path;
  const auto points = parsePoints(*value);
  if (!points || points->empty()) return path;

  path.moveTo(points->front());
  for (size_t i = 1; i < points->size(); ++i) {
    path.lineTo((*points)[i]);
  }
  if (close) path.close();
  return path;
}

/** `<line>`: a single open segment, which is never filled. */
Path linePath(const xml::Node& element, const LengthContext& context) {
  Path path;
  path.moveTo(Point {lengthAttribute(element, "x1", context), lengthAttribute(element, "y1", context)});
  path.lineTo(Point {lengthAttribute(element, "x2", context), lengthAttribute(element, "y2", context)});
  return path;
}

/**
 * @return the path for a shape element, in @p context's coordinates, or nothing when the element is not a shape.
 *
 * The one place the seven shape elements are turned into geometry -- used by the builder and by a `clipPath`'s children,
 * which are the same shapes read for a different purpose. Two copies of this dispatch would drift the moment a shape was
 * added to one of them.
 */
std::optional<Path> shapePath(const xml::Node& element, const LengthContext& context) {
  const std::string& name = element.name();

  if (name == "path") {
    const auto data = element.attribute("d");
    if (!data) return Path {};
    const auto parsed = parsePathData(*data);
    return parsed ? *parsed : Path {};
  }
  if (name == "rect") return rectPath(element, context);
  if (name == "circle") {
    const Scalar r = lengthAttribute(element, "r", context);
    return ellipsePath(lengthAttribute(element, "cx", context), lengthAttribute(element, "cy", context), r, r);
  }
  if (name == "ellipse") {
    return ellipsePath(lengthAttribute(element, "cx", context), lengthAttribute(element, "cy", context),
                       lengthAttribute(element, "rx", context), lengthAttribute(element, "ry", context));
  }
  if (name == "line") return linePath(element, context);
  if (name == "polygon") return pointsPath(element, true);
  if (name == "polyline") return pointsPath(element, false);
  return std::nullopt;
}

/**
 * The union of a `clipPath`'s children, as one path.
 *
 * SVG composes a clip path from its children as a union. Under the nonzero rule, concatenating contours that wind the
 * same way *is* their union wherever they do not overlap, and where they do overlap the winding adds and the region
 * stays filled -- so the common case is exact.
 *
 * The approximation is the case the corpus does not contain: a child written with `clip-rule="evenodd"`, or a child
 * whose contours wind the other way, would carve a hole out of an overlapping sibling instead of unioning with it.
 * Combining them properly needs the path boolean operations, which this library has for fills but which are not the same
 * question as a clip -- a union of two paths is a different operation from an intersection with either.
 *
 * @param objectBoundingBox when true the children's coordinates are fractions of a unit box, so they are left as
 *        fractions and scaled later by whatever the clip is applied to.
 */
Path clipPathGeometry(const xml::Node& element, const LengthContext& context, const bool objectBoundingBox) {
  // Under `objectBoundingBox` every length is a fraction of the box, so percentages and user units alike are measured
  // against a unit square rather than the viewport.
  LengthContext units = context;
  if (objectBoundingBox) {
    units.viewportWidth = 1;
    units.viewportHeight = 1;
  }

  Path combined;
  for (const auto& child : element.children()) {
    if (!child->isElement()) continue;

    const auto shape = shapePath(*child, units);
    if (!shape || shape->isEmpty()) continue;

    // A child's own transform applies to its own geometry, before it joins the union.
    Path placed = *shape;
    if (const auto value = child->attribute("transform")) {
      if (const auto transform = parseTransform(*value)) placed.transform(*transform);
    }
    combined.addPath(placed);
  }
  return combined;
}

// ==================================================================================================
// The build
// ==================================================================================================

class Builder {
  public:
  Builder(const StyleSheet* sheet, const LengthContext& context) : mSheet(sheet), mContext(context) {}

  /** @return the node for @p element, or null when it contributes nothing to the picture. */
  /**
 * @return every number in @p text, in order, ignoring whatever separates them.
 *
 * `Scanner` rather than `parseNumber`, and the distinction is the whole of this function. `parseNumber` requires the
 * *entire* string to be one number -- right for a single attribute, wrong for a list -- so calling it here made the list
 * come out empty, the matrix stay zero, and every filtered element render as nothing. `atEnd()` and `takeNumber()` both
 * step over spaces and commas, so the loop needs no separator handling of its own.
 */
std::vector<Scalar> scalarList(const std::string_view text) {
  std::vector<Scalar> values;
  Scanner scanner(text);
  while (!scanner.atEnd()) {
    const auto value = scanner.takeNumber();
    if (!value) break;
    values.push_back(*value);
  }
  return values;
}

/** @return the first number @p name states, or @p fallback when it is absent or unreadable. */
Scalar scalarAttribute(const xml::Node& element, const char* const name, const Scalar fallback = 0.0f) {
  const auto value = element.attribute(name);
  if (!value) return fallback;
  const auto parsed = parseNumber(*value);
  return parsed ? *parsed : fallback;
}

/** @return the colour @p name states, as an attribute or as a declaration inside `style`. */
std::optional<Color> styleOrAttributeColor(const xml::Node& element, const std::string_view name) {
  if (const auto value = element.attribute(name)) {
    if (const auto color = parseColor(*value)) return color;
  }
  if (const auto style = element.attribute("style")) {
    for (const auto& [property, value] : parseStyleAttribute(*style)) {
      if (property == name) {
        if (const auto color = parseColor(value)) return color;
      }
    }
  }
  return std::nullopt;
}

/**
 * @return the opacity @p name states, as a fraction from zero to one.
 *
 * Unlike almost every other opacity in SVG, `flood-opacity` is written as a fraction rather than a percentage -- so a
 * document saying `0.5` means half, and one saying `50` means fully opaque after clamping. Reading it as a percentage
 * would make every flood fully transparent.
 */
std::optional<Scalar> styleOrAttributeOpacity(const xml::Node& element, const std::string_view name) {
  const auto read = [](const std::string_view value) -> std::optional<Scalar> {
    if (const auto parsed = parseNumber(value)) return std::clamp(*parsed, 0.0f, 1.0f);
    return std::nullopt;
  };
  if (const auto value = element.attribute(name)) {
    if (const auto opacity = read(*value)) return opacity;
  }
  if (const auto style = element.attribute("style")) {
    for (const auto& [property, value] : parseStyleAttribute(*style)) {
      if (property == name) {
        if (const auto opacity = read(value)) return opacity;
      }
    }
  }
  return std::nullopt;
}

/**
 * @return the filter primitive @p element describes, or nothing when it is one this library does not implement.
 *
 * An unimplemented primitive yields nothing rather than a placeholder, so a filter containing one comes out short by
 * that step instead of applying a step that does nothing. Both are wrong, and the first leaves the rest of the chain
 * meaningful rather than silently wrong.
 */
std::optional<FilterPrimitive> readFilterPrimitive(const xml::Node& element) {
  using Kind = FilterPrimitive::Kind;

  const std::string name = element.name();
  FilterPrimitive primitive;
  if (name == "feColorMatrix") {
    primitive.kind = Kind::ColorMatrix;
  } else if (name == "feFlood") {
    primitive.kind = Kind::Flood;
  } else if (name == "feOffset") {
    primitive.kind = Kind::Offset;
  } else if (name == "feGaussianBlur") {
    primitive.kind = Kind::Blur;
  } else if (name == "feBlend") {
    primitive.kind = Kind::Blend;
  } else if (name == "feComposite") {
    primitive.kind = Kind::Composite;
  } else {
    return std::nullopt;
  }

  // The three every primitive carries. `in` and `in2` stay empty when absent, which the chain reads as "the previous
  // primitive's result" -- the format's rule and this corpus's most common case.
  if (const auto value = element.attribute("in")) primitive.input = std::string {*value};
  if (const auto value = element.attribute("in2")) primitive.input2 = std::string {*value};
  if (const auto value = element.attribute("result")) primitive.result = std::string {*value};

  switch (primitive.kind) {
    case Kind::ColorMatrix: {
      using Type = FilterPrimitive::ColorMatrixType;
      const std::string type = std::string {element.attribute("type").value_or("matrix")};
      if (type == "saturate") {
        primitive.colorMatrixType = Type::Saturate;
      } else if (type == "hueRotate") {
        primitive.colorMatrixType = Type::HueRotate;
      } else if (type == "luminanceToAlpha") {
        primitive.colorMatrixType = Type::LuminanceToAlpha;
      } else {
        // `matrix` is the format's default, and so is anything unrecognised: a type this renderer has not heard of
        // leaves the values to be read as a matrix, which is what a document stating only `values` means.
        primitive.colorMatrixType = Type::Matrix;
      }

      const std::vector<Scalar> values = scalarList(element.attribute("values").value_or(""));
      if (primitive.colorMatrixType == Type::Matrix) {
        // Twenty numbers, four rows of five. A shorter list leaves the rest zero rather than becoming the identity: the
        // format defines the matrix by those twenty numbers, and a missing one is a zero rather than a one.
        for (size_t i = 0; i < std::min(values.size(), primitive.values.size()); ++i) primitive.values[i] = values[i];
      } else if (!values.empty()) {
        primitive.values[0] = values.front();
      }
      break;
    }

    case Kind::Flood:
      if (const auto color = styleOrAttributeColor(element, "flood-color")) primitive.floodColor = *color;
      if (const auto opacity = styleOrAttributeOpacity(element, "flood-opacity")) primitive.floodOpacity = *opacity;
      break;

    case Kind::Offset:
      primitive.dx = scalarAttribute(element, "dx");
      primitive.dy = scalarAttribute(element, "dy");
      break;

    case Kind::Blur: {
      // One value means both axes -- the format's rule, and the reason the model carries two.
      const std::vector<Scalar> deviations = scalarList(element.attribute("stdDeviation").value_or("0"));
      primitive.deviationX = deviations.empty() ? 0.0f : deviations[0];
      primitive.deviationY = deviations.size() > 1 ? deviations[1] : primitive.deviationX;
      break;
    }

    case Kind::Blend: {
      using Blend = FilterPrimitive::Blend;
      const std::string mode = std::string {element.attribute("mode").value_or("normal")};
      primitive.blend = mode == "multiply"      ? Blend::Multiply
                        : mode == "screen"      ? Blend::Screen
                        : mode == "darken"      ? Blend::Darken
                        : mode == "lighten"     ? Blend::Lighten
                        : mode == "overlay"     ? Blend::Overlay
                        : mode == "color-dodge" ? Blend::ColorDodge
                        : mode == "color-burn"  ? Blend::ColorBurn
                        : mode == "hard-light"  ? Blend::HardLight
                        : mode == "soft-light"  ? Blend::SoftLight
                        : mode == "difference"  ? Blend::Difference
                        : mode == "exclusion"   ? Blend::Exclusion
                                                : Blend::Normal;
      break;
    }

    case Kind::Composite: {
      using Composite = FilterPrimitive::Composite;
      const std::string operation = std::string {element.attribute("operator").value_or("over")};
      primitive.composite = operation == "in"           ? Composite::In
                            : operation == "out"        ? Composite::Out
                            : operation == "atop"       ? Composite::Atop
                            : operation == "xor"        ? Composite::Xor
                            : operation == "arithmetic" ? Composite::Arithmetic
                                                        : Composite::Over;
      primitive.k1 = scalarAttribute(element, "k1");
      primitive.k2 = scalarAttribute(element, "k2");
      primitive.k3 = scalarAttribute(element, "k3");
      primitive.k4 = scalarAttribute(element, "k4");
      break;
    }
  }

  return primitive;
}

NodePtr build(const xml::Node& element) {
    const std::string& name = element.name();

    // `<text>` gathers its content from its descendants, including any nested `<tspan>`.
    if (name == "text") {
      auto text = std::make_unique<TextNode>();
      text->setPosition(Point {lengthAttribute(element, "x", mContext), lengthAttribute(element, "y", mContext)});
      text->setContent(element.textContent());
      finish(*text, element);
      return text;
    }

    // Gradients are definitions, but they are kept as nodes so the id index can reach them -- and they draw nothing, so
    // leaving one where it was written is harmless even outside `<defs>`.
    if (name == "linearGradient" || name == "radialGradient") {
      const auto kind = name == "linearGradient" ? Gradient::Kind::kLinear : Gradient::Kind::kRadial;
      auto gradient = std::make_unique<GradientNode>(parseGradient(element, kind));
      finish(*gradient, element);
      // A gradient element has `<stop>` children, which are not drawable nodes; the parse above consumed them.
      return gradient;
    }

    // A clip path is a definition too, and its children are geometry rather than drawable nodes -- so the element is
    // read as one path rather than built as a subtree nothing would ever render.
    if (name == "clipPath") {
      const bool objectBoundingBox =
              element.attribute("clipPathUnits").value_or("userSpaceOnUse") == "objectBoundingBox";
      auto clip = std::make_unique<ClipPathNode>(clipPathGeometry(element, mContext, objectBoundingBox),
                                                 objectBoundingBox);
      finish(*clip, element);
      return clip;
    }

    // A mask keeps its children, because its children are a picture rather than a shape: their fills, their gradients and
    // their own transforms all end up in the coverage. Only the coordinate units and the region are read from the
    // element itself.
    if (name == "mask") {
      auto maskNode = std::make_unique<MaskNode>();

      MaskNode::Region region;
      readLength(element, "x", region.x);
      readLength(element, "y", region.y);
      readLength(element, "width", region.width);
      readLength(element, "height", region.height);
      // The format's defaults are the opposite way round from what a reader expects: the *region* is in the element's box
      // unless told otherwise, and the *content* is in user space unless told otherwise.
      region.objectBoundingBox = element.attribute("maskUnits").value_or("objectBoundingBox") != "userSpaceOnUse";
      region.contentObjectBoundingBox =
              element.attribute("maskContentUnits").value_or("userSpaceOnUse") == "objectBoundingBox";

      // `mask-type` is a *property*, so it may be written either as an attribute or inside `style`. Both spellings are
      // read, and the style attribute wins, which is what the cascade says and what the SVG presentation layer already
      // does for every other property. Reading only the attribute is how a document that says `style="mask-type:alpha"`
      // comes out as a luminance mask -- and the two disagree for every mask whose colour varies while its alpha does not.
      const auto readMaskType = [](const std::string_view value, MaskMode& mode) {
        if (value == "alpha") mode = MaskMode::kAlpha;
        else if (value == "luminance") mode = MaskMode::kLuminance;
      };
      if (const auto value = element.attribute("mask-type")) readMaskType(*value, region.mode);
      if (const auto style = element.attribute("style")) {
        for (const auto& [property, value] : parseStyleAttribute(*style)) {
          if (property == "mask-type") readMaskType(value, region.mode);
        }
      }

      maskNode->setRegion(std::move(region));

      finish(*maskNode, element);
      for (const auto& child : element.children()) {
        if (!child->isElement()) continue;
        if (NodePtr built = build(*child)) maskNode->addChild(std::move(built));
      }
      return maskNode;
    }

    // An `<image>` carries its picture inside the document as a `data:` URI, so the bytes are decoded here -- once, at
    // build time, rather than on every frame a player draws.
    if (name == "image") {
      const auto href = element.attribute("href").has_value() ? element.attribute("href")
                                                             : element.attribute("xlink:href");
      if (!href) return nullptr;

      const auto bytes = decodeDataUri(*href);
      if (!bytes) return nullptr;

      Image decoded = Image::decode(*bytes);
      if (decoded.isEmpty()) return nullptr;

      const Scalar x = lengthAttribute(element, "x", mContext);
      const Scalar y = lengthAttribute(element, "y", mContext);
      const Scalar width = lengthAttribute(element, "width", mContext);
      const Scalar height = lengthAttribute(element, "height", mContext);

      auto node = std::make_unique<ImageNode>(decoded.peekBitmap(),
                                              Rect::MakeLTRB(x, y, x + width, y + height));

      // `preserveAspectRatio` defaults to `xMidYMid meet` in `PreserveAspectRatio`'s own initial values, so an element
      // that states nothing gets the format's default without a branch here.
      if (const auto value = element.attribute("preserveAspectRatio")) {
        if (const auto aspect = parsePreserveAspectRatio(*value)) node->setPreserveAspectRatio(*aspect);
      }
      finish(*node, element);
      return node;
    }

    // A pattern keeps its children, like a mask: they are a picture, and their fills and transforms all end up in the
    // tile. Only the tile rectangle and the two unit systems are read from the element itself.
    // A filter keeps its primitives, like a mask keeps its children: they are the effect, and the element referring to
    // the filter is what they are applied to. `<filter>` is already in this document's list of elements that paint
    // nothing in place, so this only has to read what the definition says.
    if (name == "filter") {
      auto filterNode = std::make_unique<FilterNode>();

      Filter filter;
      filter.objectBoundingBox = element.attribute("filterUnits").value_or("objectBoundingBox") != "userSpaceOnUse";
      readLength(element, "x", filter.x);
      readLength(element, "y", filter.y);
      readLength(element, "width", filter.width);
      readLength(element, "height", filter.height);

      for (const auto& child : element.children()) {
        if (!child->isElement()) continue;
        if (const auto primitive = readFilterPrimitive(*child)) filter.primitives.push_back(*primitive);
      }

      filterNode->setFilter(std::move(filter));
      finish(*filterNode, element);
      return filterNode;
    }

    if (name == "pattern") {
      auto patternNode = std::make_unique<PatternNode>();

      PatternNode::Tile tile;
      readLength(element, "x", tile.x);
      readLength(element, "y", tile.y);
      readLength(element, "width", tile.width);
      readLength(element, "height", tile.height);
      // The format's defaults are the opposite way round, as they are for a mask: the tile rectangle is in the element's
      // box unless told otherwise, and the content is in user space unless told otherwise.
      tile.objectBoundingBox = element.attribute("patternUnits").value_or("objectBoundingBox") != "userSpaceOnUse";
      tile.contentObjectBoundingBox =
              element.attribute("patternContentUnits").value_or("userSpaceOnUse") == "objectBoundingBox";
      if (const auto value = element.attribute("patternTransform")) {
        if (const auto transform = parseTransform(*value)) tile.transform = *transform;
      }
      patternNode->setTile(std::move(tile));

      finish(*patternNode, element);
      for (const auto& child : element.children()) {
        if (!child->isElement()) continue;
        if (NodePtr built = build(*child)) patternNode->addChild(std::move(built));
      }
      return patternNode;
    }

    if (name == "use") {
      auto use = std::make_unique<UseNode>();
      const auto href = element.attribute("href").has_value() ? element.attribute("href") : element.attribute("xlink:href");
      if (href) {
        std::string target {*href};
        // `href="#id"` in both spellings; the fragment is the id, and a bare `#` means nothing.
        if (!target.empty() && target.front() == '#') target.erase(0, 1);
        use->setTarget(std::move(target));
      }
      use->setPosition(Point {lengthAttribute(element, "x", mContext), lengthAttribute(element, "y", mContext)});
      finish(*use, element);
      return use;
    }

    // Shapes. `shapePath` answers nothing for anything that is not one, which is how the container case below is
    // reached without a second list of element names to keep in step.
    if (const auto geometry = shapePath(element, mContext)) {
      auto node = std::make_unique<ShapeNode>(std::move(*geometry));
      finish(*node, element);
      // `<line>` takes no fill in SVG; leaving the fill on would fill the implicit closure of the segment, which is a
      // thin wedge nobody drew.
      if (name == "line" && !element.hasAttribute("fill")) {
        node->presentation().fill = Paint {Paint::Type::None, Color::Black(), {}, std::nullopt};
      }
      return node;
    }

    // Everything else is a container: `<g>`, `<svg>` nested, `<a>`, `<switch>`, and any unknown element that has
    // element children. An unknown element with no children contributes nothing and is dropped.
    auto group = std::make_unique<GroupNode>();
    finish(*group, element);
    if (isDefinition(name)) group->setHidden(true);

    bool hasChild = false;
    for (const auto& child : element.children()) {
      if (!child->isElement()) continue;
      if (NodePtr built = build(*child)) {
        group->addChild(std::move(built));
        hasChild = true;
      }
    }

    // A `<defs>` block is kept even when it holds nothing drawable, because `use` may refer into it. An unknown empty
    // element is not.
    if (!hasChild && !isDefinition(name) && name != "g" && name != "a" && name != "svg") return nullptr;
    return group;
  }

  private:
  /** Reads the element's presentation and identity onto @p node. */
  void finish(svg::Node& node, const xml::Node& element) {
    // The XML layer's attribute and the presentation layer's declaration are the same thing under different member
    // names. Adapting here rather than making one depend on the other keeps the presentation layer independent of how
    // the document was read -- it would work the same from a tree built by hand.
    std::vector<Declaration> attributes;
    attributes.reserve(element.attributes().size());
    for (const xml::Attribute& attribute : element.attributes()) {
      attributes.emplace_back(attribute.name, attribute.value);
    }

    Presentation presentation;
    parsePresentation(element.name(), attributes, mSheet, mContext, presentation);
    node.setPresentation(std::move(presentation));

    if (const auto id = element.attribute("id")) node.setId(std::string {*id});

    // `display="none"` removes the element and its subtree from the picture, which is different from `visibility`: a
    // hidden element still takes part in layout, and an undisplayed one does not exist at all.
    const auto display = element.attribute("display");
    if (display && *display == "none") node.setHidden(true);
    for (const Declaration& declaration : attributes) {
      if (declaration.first != "style") continue;
      for (const auto& [property, propertyValue] : parseStyleAttribute(declaration.second)) {
        if (property == "display" && propertyValue == "none") node.setHidden(true);
      }
    }
  }

  const StyleSheet* mSheet = nullptr;
  LengthContext mContext;
};

} // namespace anonymous

std::optional<Document> buildDocument(const xml::Node& root, std::string* error) {
  if (!root.isElement() || root.name() != "svg") {
    if (error != nullptr) *error = "the root element is not <svg>";
    return std::nullopt;
  }

  // The stylesheet is collected before anything is built, because a rule applies to an element that may appear before
  // the `<style>` element that defines it -- and it is collected from anywhere in the document rather than from the
  // root's own children: putting the `<style>` inside `<defs>` is both the common placement and the one the corpus uses,
  // and a rule found only at the top level is silently lost everywhere else.
  StyleSheet sheetValue;
  const StyleSheet* sheet = nullptr;
  const auto collectSheet = [&sheetValue, &sheet](const auto& self, const xml::Node& element) -> void {
    if (sheet != nullptr) return; // the first stylesheet wins, as it does in CSS's source order
    for (const auto& child : element.children()) {
      if (!child->isElement()) continue;
      if (child->name() == "style") {
        if (const auto parsed = parseStyleSheet(child->textContent())) {
          sheetValue = *parsed;
          sheet = &sheetValue;
          return;
        }
        continue;
      }
      self(self, *child);
      if (sheet != nullptr) return;
    }
  };
  collectSheet(collectSheet, root);

  // The viewport is the root's own width and height. When either is absent the viewBox supplies it, and when neither is
  // present there is nothing to size the picture with.
  std::optional<Rect> viewBox;
  if (const auto value = root.attribute("viewBox")) viewBox = parseViewBox(*value);

  LengthContext context;
  if (viewBox) {
    context.viewportWidth = viewBox->getWidth();
    context.viewportHeight = viewBox->getHeight();
  }

  Scalar width = 0;
  Scalar height = 0;
  if (const auto value = root.attribute("width")) {
    if (const auto length = parseLength(*value)) width = length->isPercent() ? 0 : length->value;
  }
  if (const auto value = root.attribute("height")) {
    if (const auto length = parseLength(*value)) height = length->isPercent() ? 0 : length->value;
  }
  if (width <= 0 && viewBox) width = viewBox->getWidth();
  if (height <= 0 && viewBox) height = viewBox->getHeight();

  if (width <= 0 || height <= 0) {
    if (error != nullptr) *error = "the document states no usable size: neither width/height nor a viewBox";
    return std::nullopt;
  }

  // The root's own children are built with the *viewport* context, so a percentage in a child resolves against the
  // size the document declares rather than against zero.
  context.viewportWidth = width;
  context.viewportHeight = height;

  Builder builder(sheet, context);
  auto rootGroup = std::make_unique<GroupNode>();
  for (const auto& child : root.children()) {
    if (!child->isElement()) continue;
    if (child->name() == "style") continue; // consumed above; it draws nothing
    if (NodePtr built = builder.build(*child)) {
      rootGroup->addChild(std::move(built));
    }
  }

  PreserveAspectRatio aspect;
  if (const auto value = root.attribute("preserveAspectRatio")) {
    if (const auto parsed = parsePreserveAspectRatio(*value)) aspect = *parsed;
  }

  Document document;
  document.mWidth = width;
  document.mHeight = height;
  document.mViewBox = viewBox;
  document.mAspect = aspect;
  document.mRoot = std::move(rootGroup);

  // The index is built once the whole tree exists, so a `use` that names an element defined later still resolves.
  document.mIndex = IdIndex {};
  document.mRoot->indexIds(document.mIndex);

  // And the references are resolved in a second walk, for the same reason -- but through the nodes themselves rather
  // than through a list of container types written out here. The list was the bug: it knew about `GroupNode` and nothing
  // else, so a `<use>` inside a `<pattern>` or a `<mask>` was never resolved and rendered nothing.
  document.mRoot->resolveReferences(document.mIndex);

  return document;
}

} // namespace graphics::svg
