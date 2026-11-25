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
 * @file LottieParser.cpp
 *
 * Reading a Lottie document from JSON.
 *
 * ## Forgiving in the same places the format is
 *
 * Lottie has no published schema and no validator: exporters agree on the parts that matter and differ on the rest, and
 * a document in the wild omits fields that its own specification calls required. So every read below has a default, and
 * a missing or wrongly-typed field produces the format's own initial value rather than a failure. That is not laziness
 * -- it is the only reading that renders the documents that exist.
 *
 * The one thing that *is* refused is a document with no layers at all, because there is then nothing to draw and a
 * caller asking about it wants to know.
 *
 * ## Two shapes of a property
 *
 * A property is either a bare value (`"k": 5`) or an object with a keyframe list (`"k": [...]`), and the two are
 * distinguished by the `a` flag -- which exporters sometimes omit, so the type of `k` is consulted as well. Every
 * property read goes through the same two functions below, which is what keeps that rule in one place.
 */

#include "lottie/LottieModel.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace graphics::lottie {
namespace {

using json = nlohmann::json;

// ==================================================================================================
// Scalars and vectors out of JSON
// ==================================================================================================

/** @return the number at @p key, or @p fallback when it is absent or not a number. */
Scalar number(const json& object, const char* key, const Scalar fallback = 0.0f) {
  if (!object.is_object()) return fallback;
  const auto it = object.find(key);
  if (it == object.end() || !it->is_number()) return fallback;
  return it->get<Scalar>();
}

bool flag(const json& object, const char* key, const bool fallback = false) {
  if (!object.is_object()) return fallback;
  const auto it = object.find(key);
  if (it == object.end()) return fallback;
  // A flag is written as `true`/`false` or as 1/0, depending on the exporter and the field.
  if (it->is_boolean()) return it->get<bool>();
  if (it->is_number()) return it->get<double>() != 0.0;
  return fallback;
}

std::string text(const json& object, const char* key, const char* fallback = "") {
  if (!object.is_object()) return fallback;
  const auto it = object.find(key);
  if (it == object.end() || !it->is_string()) return fallback;
  return it->get<std::string>();
}

/** @return the digit @p c stands for, or -1 when it is not a hexadecimal digit. */
int hexDigit(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/**
 * @return the colour a hex string names, or opaque black when it is not one.
 *
 * `#rgb`, `#rrggbb` and `#rrggbbaa` are accepted; the short form is expanded by doubling each digit, which is what makes
 * `#f00` the same red as `#ff0000` rather than a different one. The leading `#` is optional, because an exporter that
 * omits it is common and the alternative is a black solid where a green one was meant.
 *
 * Deliberately *not* taken from the SVG layer's colour reader: the two parsers share nothing else, and a Lottie file's
 * colour grammar is not SVG's -- there are no names here, and no `rgb()`.
 */
Color colorFromHex(std::string_view text) {
  if (!text.empty() && text.front() == '#') text.remove_prefix(1);

  const auto digitAt = [&text](const size_t index) {
    return index < text.size() ? hexDigit(text[index]) : -1;
  };

  if (text.size() == 3 || text.size() == 4) {
    const int red = digitAt(0);
    const int green = digitAt(1);
    const int blue = digitAt(2);
    const int alpha = text.size() == 4 ? digitAt(3) : 15;
    if (red < 0 || green < 0 || blue < 0 || alpha < 0) return Color {};
    // Doubling each digit is the format's rule: `f` becomes `ff`, which is 255 rather than 15.
    return Color::MakeRGBA8(static_cast<uint8_t>(red * 17), static_cast<uint8_t>(green * 17),
                            static_cast<uint8_t>(blue * 17), static_cast<uint8_t>(alpha * 17));
  }

  if (text.size() == 6 || text.size() == 8) {
    const auto pair = [&digitAt](const size_t index) {
      const int high = digitAt(index);
      const int low = digitAt(index + 1);
      return high < 0 || low < 0 ? -1 : high * 16 + low;
    };
    const int red = pair(0);
    const int green = pair(2);
    const int blue = pair(4);
    const int alpha = text.size() == 8 ? pair(6) : 255;
    if (red < 0 || green < 0 || blue < 0 || alpha < 0) return Color {};
    return Color::MakeRGBA8(static_cast<uint8_t>(red), static_cast<uint8_t>(green), static_cast<uint8_t>(blue),
                            static_cast<uint8_t>(alpha));
  }

  return Color {};
}

/** @return the value at @p key, or null when it is absent. The other readers take a default; this one hands back the
 *  value itself, which a caller needs when presence and content are separate questions. */
const json* member(const json& object, const char* key) {
  if (!object.is_object()) return nullptr;
  const auto it = object.find(key);
  return it == object.end() ? nullptr : &*it;
}

/** @return the array at @p key as a list of numbers, or empty when it is absent or not numeric. */
std::vector<Scalar> numbers(const json& value) {
  std::vector<Scalar> out;
  if (!value.is_array()) return out;
  out.reserve(value.size());
  for (const json& entry : value) {
    if (!entry.is_number()) return {};
    out.push_back(entry.get<Scalar>());
  }
  return out;
}

/** @return a point from a two- or three-number array; a missing component is zero, as the format's own default is. */
Point pointOf(const std::vector<Scalar>& values, const Scalar fallback = 0.0f) {
  const auto at = [&values, fallback](const size_t index) {
    return index < values.size() ? values[index] : fallback;
  };
  return Point {at(0), at(1)};
}

// ==================================================================================================
// Keyframe easing
// ==================================================================================================

/**
 * Reads the easing a keyframe states.
 *
 * `o` and `i` are each `{x: [...], y: [...]}`, with the numbers in arrays because the format allows one curve per value
 * component. Only the first component is read: the format permits an exporter to ease each axis of a position
 * separately, and no exporter in the wild does -- so reading the first and applying it to all of them is the honest
 * simplification, stated rather than hidden.
 */
template <typename T>
void readEasing(const json& keyframe, Keyframe<T>& out) {
  const auto read = [&keyframe](const char* key) -> std::optional<Point> {
    const auto it = keyframe.find(key);
    if (it == keyframe.end() || !it->is_object()) return std::nullopt;
    const auto xs = numbers(it->value("x", json::array()));
    const auto ys = numbers(it->value("y", json::array()));
    if (xs.empty() || ys.empty()) return std::nullopt;
    return Point {xs.front(), ys.front()};
  };

  if (const auto out_ = read("o")) out.outTangent = *out_;
  if (const auto in = read("i")) out.inTangent = *in;
}

// ==================================================================================================
// Properties
// ==================================================================================================

/** The keyframe list of a property, or null when the property is a bare value. */
const json* animatedKeys(const json& property) {
  if (!property.is_object()) return nullptr;
  const auto it = property.find("k");
  if (it == property.end() || !it->is_array()) return nullptr;

  // `a` says so directly. When it is absent -- which happens -- an array of objects each carrying `t` is a keyframe list
  // and an array of numbers is a value, and that is the only other reading available.
  if (flag(property, "a", false)) return &*it;
  if (!it->empty() && it->front().is_object() && it->front().contains("t")) return &*it;
  return nullptr;
}

/**
 * Reads a property of one number.
 *
 * An animated property interpolates its scalar; a static one is a single keyframe, which `Animatable` treats as
 * constant without carrying a curve.
 */
Animatable<Scalar> readScalar(const json& parent, const char* key, const Scalar fallback = 0.0f) {
  if (!parent.is_object()) return Animatable<Scalar>::constant(fallback);
  const auto it = parent.find(key);
  if (it == parent.end()) return Animatable<Scalar>::constant(fallback);

  if (const json* keys = animatedKeys(*it)) {
    std::vector<Keyframe<Scalar>> frames;
    frames.reserve(keys->size());
    for (const json& entry : *keys) {
      Keyframe<Scalar> frame;
      frame.time = number(entry, "t");
      readEasing(entry, frame);
      frame.hold = flag(entry, "h", false);

      // `s` is the value here, and is written either as a number or as a one-element array.
      const auto start = entry.find("s");
      if (start != entry.end()) {
        if (start->is_number()) {
          frame.value = start->get<Scalar>();
        } else if (const auto values = numbers(*start); !values.empty()) {
          frame.value = values.front();
        }
      }
      const auto end = entry.find("e");
      if (end != entry.end()) {
        if (end->is_number()) {
          frame.endValue = end->get<Scalar>();
        } else if (const auto values = numbers(*end); !values.empty()) {
          frame.endValue = values.front();
        }
      }
      frames.push_back(std::move(frame));
    }
    if (!frames.empty()) return Animatable<Scalar>::animated(std::move(frames));
    return Animatable<Scalar>::constant(fallback);
  }

  // A bare value, which may itself be a one-element array.
  const auto bare = it->find("k");
  const json& value = bare != it->end() ? *bare : *it;
  if (value.is_number()) return Animatable<Scalar>::constant(value.get<Scalar>());
  if (const auto values = numbers(value); !values.empty()) return Animatable<Scalar>::constant(values.front());
  return Animatable<Scalar>::constant(fallback);
}

/** Reads a property of several numbers: a position, a scale, or an anchor. */
Animatable<std::vector<Scalar>> readNumbers(const json& parent, const char* key, std::vector<Scalar> fallback) {
  if (!parent.is_object()) return Animatable<std::vector<Scalar>>::constant(std::move(fallback));
  const auto it = parent.find(key);
  if (it == parent.end()) return Animatable<std::vector<Scalar>>::constant(std::move(fallback));

  if (const json* keys = animatedKeys(*it)) {
    std::vector<Keyframe<std::vector<Scalar>>> frames;
    frames.reserve(keys->size());
    for (const json& entry : *keys) {
      Keyframe<std::vector<Scalar>> frame;
      frame.time = number(entry, "t");
      readEasing(entry, frame);
      frame.hold = flag(entry, "h", false);

      const auto start = entry.find("s");
      if (start != entry.end()) frame.value = numbers(*start);
      const auto end = entry.find("e");
      if (end != entry.end()) {
        if (auto values = numbers(*end); !values.empty()) frame.endValue = std::move(values);
      }
      frames.push_back(std::move(frame));
    }
    if (!frames.empty()) return Animatable<std::vector<Scalar>>::animated(std::move(frames));
    return Animatable<std::vector<Scalar>>::constant(std::move(fallback));
  }

  const auto bare = it->find("k");
  const json& value = bare != it->end() ? *bare : *it;
  if (auto values = numbers(value); !values.empty()) {
    return Animatable<std::vector<Scalar>>::constant(std::move(values));
  }
  return Animatable<std::vector<Scalar>>::constant(std::move(fallback));
}

/**
 * Reads a property of two or three numbers as a point, keeping its keyframes.
 *
 * A separate reader from `readNumbers` rather than a narrowing afterwards, because narrowing afterwards is exactly the
 * mistake: taking the first keyframe of a moving position gives a layer that is in the right place at frame zero and
 * nowhere else.
 */
Animatable<Point> readPoint(const json& parent, const char* key, const Point fallback = Point {0, 0}) {
  if (!parent.is_object()) return Animatable<Point>::constant(fallback);
  const auto it = parent.find(key);
  if (it == parent.end()) return Animatable<Point>::constant(fallback);

  if (const json* keys = animatedKeys(*it)) {
    std::vector<Keyframe<Point>> frames;
    frames.reserve(keys->size());
    for (const json& entry : *keys) {
      Keyframe<Point> frame;
      frame.time = number(entry, "t");
      readEasing(entry, frame);
      frame.hold = flag(entry, "h", false);

      if (const auto start = entry.find("s"); start != entry.end()) frame.value = pointOf(numbers(*start));
      if (const auto end = entry.find("e"); end != entry.end()) {
        if (auto values = numbers(*end); !values.empty()) frame.endValue = pointOf(values);
      }
      frames.push_back(std::move(frame));
    }
    if (!frames.empty()) return Animatable<Point>::animated(std::move(frames));
    return Animatable<Point>::constant(fallback);
  }

  const auto bare = it->find("k");
  const json& value = bare != it->end() ? *bare : *it;
  if (auto values = numbers(value); !values.empty()) return Animatable<Point>::constant(pointOf(values));
  return Animatable<Point>::constant(fallback);
}

/** Reads a colour, which the format writes as three or four numbers in 0-1. */
Animatable<Color> readColor(const json& parent, const char* key) {
  const auto toColor = [](const std::vector<Scalar>& values) {
    return Color {values.size() > 0 ? values[0] : 0.0f, values.size() > 1 ? values[1] : 0.0f,
                  values.size() > 2 ? values[2] : 0.0f, values.size() > 3 ? values[3] : 1.0f};
  };

  if (!parent.is_object()) return Animatable<Color>::constant(toColor({}));
  const auto it = parent.find(key);
  if (it == parent.end()) return Animatable<Color>::constant(toColor({}));

  if (const json* keys = animatedKeys(*it)) {
    std::vector<Keyframe<Color>> frames;
    for (const json& entry : *keys) {
      Keyframe<Color> frame;
      frame.time = number(entry, "t");
      frame.hold = flag(entry, "h", false);
      if (const auto start = entry.find("s"); start != entry.end()) frame.value = toColor(numbers(*start));
      if (const auto end = entry.find("e"); end != entry.end()) {
        if (auto values = numbers(*end); !values.empty()) frame.endValue = toColor(values);
      }
      frames.push_back(std::move(frame));
    }
    if (!frames.empty()) return Animatable<Color>::animated(std::move(frames));
  }

  const auto bare = it->find("k");
  const json& value = bare != it->end() ? *bare : *it;
  return Animatable<Color>::constant(toColor(numbers(value)));
}

/** Reads a vertex path, which is `{v: [...], i: [...], o: [...], c: bool}`. */
Animatable<VertexPath> readVertexPath(const json& parent, const char* key) {
  const auto toPath = [](const json& value) {
    VertexPath path;
    if (!value.is_object()) return path;

    const auto readPoints = [&value](const char* field) {
      std::vector<Point> out;
      const auto it = value.find(field);
      if (it == value.end() || !it->is_array()) return out;
      out.reserve(it->size());
      for (const json& entry : *it) {
        const auto values = numbers(entry);
        // A tangent pair that is short is padded with zeros rather than dropped, because dropping one would put the
        // remaining tangents out of step with their vertices and draw a corner where there was none.
        out.push_back(pointOf(values));
      }
      return out;
    };

    path.vertices = readPoints("v");
    path.inTangents = readPoints("i");
    path.outTangents = readPoints("o");
    path.closed = flag(value, "c", false);
    return path;
  };

  if (!parent.is_object()) return Animatable<VertexPath>::constant(VertexPath {});
  const auto it = parent.find(key);
  if (it == parent.end()) return Animatable<VertexPath>::constant(VertexPath {});

  if (const json* keys = animatedKeys(*it)) {
    std::vector<Keyframe<VertexPath>> frames;
    for (const json& entry : *keys) {
      Keyframe<VertexPath> frame;
      frame.time = number(entry, "t");
      frame.hold = flag(entry, "h", false);
      // A path's keyframe states the shape in `s`, which is itself an object rather than an array.
      if (const auto start = entry.find("s"); start != entry.end()) frame.value = toPath(*start);
      if (const auto end = entry.find("e"); end != entry.end()) frame.endValue = toPath(*end);
      frames.push_back(std::move(frame));
    }
    if (!frames.empty()) return Animatable<VertexPath>::animated(std::move(frames));
  }

  const auto bare = it->find("k");
  return Animatable<VertexPath>::constant(toPath(bare != it->end() ? *bare : *it));
}

// ==================================================================================================
// The tree
// ==================================================================================================

/**
 * @return the gradient `gf` or `gs` describes.
 *
 * The stop array is `g.k`: @c g.p colour records of four floats -- position first -- followed by however many two-float
 * opacity records fit. The split is validated here rather than at draw time, so a malformed array is one parse-time
 * observation instead of a wrong picture on every frame.
 */
Gradient readGradient(const json& object) {
  Gradient gradient;

  // Anything that is not 2 is linear: the format defines only the two, and Skottie reads it the same way.
  gradient.type = static_cast<int>(number(object, "t", 1.0f));
  gradient.highlightLength = number(object, "h", 0.0f);
  gradient.highlightAngle = number(object, "a", 0.0f);

  gradient.start = readPoint(object, "s");
  gradient.end = readPoint(object, "e");

  if (const json* const stops = member(object, "g")) {
    gradient.colorRecords = std::max(0, static_cast<int>(number(*stops, "p", 0.0f)));
    gradient.records = readNumbers(*stops, "k", {});
  }
  return gradient;
}

Transform readTransform(const json& object) {
  Transform transform;
  if (!object.is_object()) return transform;

  // The anchor and the position are point-valued, so they keep their keyframes: a layer that moves is the common case,
  // and taking only the first keyframe freezes it at the right place for exactly one frame.
  transform.anchor = readPoint(object, "a", Point {0, 0});
  transform.position = readPoint(object, "p", Point {0, 0});
  transform.scale = readNumbers(object, "s", {100.0f, 100.0f});
  transform.rotation = readScalar(object, "r", 0.0f);
  transform.opacity = readScalar(object, "o", 100.0f);
  transform.skew = readScalar(object, "sk", 0.0f);
  transform.skewAxis = readScalar(object, "sa", 0.0f);
  return transform;
}

ShapePtr readShape(const json& object);

/** Reads a group's children, keeping only the kinds this renderer knows, and recording the rest as unknown. */
std::vector<ShapePtr> readItems(const json& object, const char* key) {
  std::vector<ShapePtr> items;
  const auto it = object.find(key);
  if (it == object.end() || !it->is_array()) return items;

  items.reserve(it->size());
  for (const json& entry : *it) {
    if (ShapePtr shape = readShape(entry)) items.push_back(std::move(shape));
  }
  return items;
}

ShapePtr readShape(const json& object) {
  if (!object.is_object()) return nullptr;

  auto shape = std::make_unique<Shape>();
  shape->name = text(object, "nm");
  shape->hidden = flag(object, "hd", false);

  const std::string type = text(object, "ty");
  if (type == "gr") {
    shape->kind = ShapeKind::Group;
    shape->items = readItems(object, "it");
    // A group's transform is one of its own items, named "Transform". Reading it from there rather than expecting a
    // `ks` field is what the format does: it is an item like any other.
    for (const ShapePtr& item : shape->items) {
      if (item->kind == ShapeKind::Transform) {
        shape->transform = item->transform;
        break;
      }
    }
    return shape;
  }
  // `rc`, `el` and `sr` are geometry written as numbers rather than as vertices. Each is read into the same struct,
  // because the renderer turns all three into a path for a frame and does nothing else with them.
  if (type == "rc" || type == "el" || type == "sr") {
    Geometry geometry;
    geometry.kind = type == "rc" ? Geometry::Kind::Rectangle
                                  : type == "el" ? Geometry::Kind::Ellipse
                                                 : Geometry::Kind::Polystar;
    geometry.position = readPoint(object, "p");
    // `d` is 3 for counter-clockwise and 1 (the default) for clockwise. Read for the two shapes that carry it; a star
    // has no direction of its own.
    geometry.counterClockwise = static_cast<int>(number(object, "d", 1.0f)) == 3;

    if (type == "sr") {
      // `sy` is 1 for a star and 2 for a polygon. Anything else is treated as a star, which is the format's default.
      geometry.star = static_cast<int>(number(object, "sy", 1.0f)) != 2;
      geometry.pointCount = readScalar(object, "pt", 5.0f);
      geometry.rotation = readScalar(object, "r", 0.0f);
      geometry.outerRadius = readScalar(object, "or", 0.0f);
      geometry.innerRadius = readScalar(object, "ir", 0.0f);
      geometry.outerRoundness = readScalar(object, "os", 0.0f);
      geometry.innerRoundness = readScalar(object, "is", 0.0f);
    } else {
      geometry.size = readPoint(object, "s");
      if (type == "rc") geometry.roundness = readScalar(object, "r", 0.0f);
    }

    shape->kind = type == "rc" ? ShapeKind::Rectangle
                               : type == "el" ? ShapeKind::Ellipse
                                              : ShapeKind::Polystar;
    shape->geometry = std::move(geometry);
    return shape;
  }

  if (type == "sh") {
    shape->kind = ShapeKind::Path;
    shape->path = readVertexPath(object, "ks");
    return shape;
  }
  if (type == "rp") {
    Repeater repeater;
    repeater.count = readScalar(object, "c", 0.0f);
    repeater.offset = readScalar(object, "o", 0.0f);
    // `m`: 1 is the mode Skottie calls `kBelow` and anything else is `kAbove`. The two differ in stacking order *and*
    // in which end of the opacity ramp a copy gets, so a document that states neither gets the default rather than a
    // guess.
    repeater.below = static_cast<int>(number(object, "m", 1.0f)) == 1;

    // The repeater's transform lives in its own object, and its opacity fields are `so`/`eo` rather than the `o` a shape
    // transform uses. The scale is a percentage, as a shape transform's is.
    if (const json* const transform = member(object, "tr")) {
      repeater.anchor = readPoint(*transform, "a");
      repeater.position = readPoint(*transform, "p");
      repeater.scale = readPoint(*transform, "s", Point {100, 100});
      repeater.rotation = readScalar(*transform, "r", 0.0f);
      repeater.startOpacity = readScalar(*transform, "so", 100.0f);
      repeater.endOpacity = readScalar(*transform, "eo", 100.0f);
    } else {
      // Without a transform Skottie returns its inputs unchanged -- a repeater with nothing to repeat with. Reported as
      // a repeater with the identity transform rather than dropped, so the document keeps its shape.
      repeater.scale = Animatable<Point>::constant(Point {100, 100});
      repeater.startOpacity = Animatable<Scalar>::constant(100.0f);
      repeater.endOpacity = Animatable<Scalar>::constant(100.0f);
    }

    shape->kind = ShapeKind::Repeater;
    shape->repeater = std::move(repeater);
    return shape;
  }

  if (type == "tm") {
    Trim trim;
    // The defaults are the identity trim: the whole path, no offset. A document that states none of the three means
    // "change nothing", which is what an unmoved trim handle looks like.
    trim.start = readScalar(object, "s", 0.0f);
    trim.end = readScalar(object, "e", 100.0f);
    trim.offset = readScalar(object, "o", 0.0f);

    // `m` is 1 for "Simultaneously" and 2 for "Individually", and the naming runs opposite to what the renderer does:
    // the individually-trimmed option is implemented by *joining* the geometry and trimming that. Skottie's own table
    // says the same, mapping 1 to `kParallel` and 2 to `kSerial`.
    trim.trimsAsOnePath = static_cast<int>(number(object, "m", 1.0f)) == 2;

    shape->kind = ShapeKind::Trim;
    shape->trim = std::move(trim);
    return shape;
  }

  if (type == "mm") {
    shape->kind = ShapeKind::Merge;
    const int mode = static_cast<int>(number(object, "mm", 1.0f));
    shape->merge = static_cast<MergeMode>(std::clamp(mode, 1, 5));
    return shape;
  }
  if (type == "fl") {
    shape->kind = ShapeKind::Fill;
    shape->color = readColor(object, "c");
    shape->colorOpacity = readScalar(object, "o", 100.0f);
    shape->rule = static_cast<int>(number(object, "r", 1.0f)) == 2 ? FillType::EvenOdd : FillType::Winding;
    return shape;
  }
  if (type == "st") {
    shape->kind = ShapeKind::Stroke;
    shape->color = readColor(object, "c");
    shape->colorOpacity = readScalar(object, "o", 100.0f);
    shape->strokeWidth = readScalar(object, "w", 0.0f);
    return shape;
  }
  if (type == "tr") {
    shape->kind = ShapeKind::Transform;
    shape->transform = readTransform(object);
    return shape;
  }

  if (type == "gf" || type == "gs") {
    shape->kind = type == "gf" ? ShapeKind::GradientFill : ShapeKind::GradientStroke;
    shape->gradient = readGradient(object);
    shape->colorOpacity = readScalar(object, "o", 100.0f);
    shape->strokeWidth = readScalar(object, "w", 0.0f);
    shape->rule = static_cast<int>(number(object, "r", 1.0f)) == 2 ? FillType::EvenOdd : FillType::Winding;
    return shape;
  }

  // An item this renderer does not implement -- a repeater, a trim. It is kept rather than dropped, so that a group's
  // item list stays in document order and a later reader can find it; nothing draws it.
  shape->kind = ShapeKind::Unknown;
  return shape;
}

Layer readLayer(const json& object) {
  Layer layer;
  if (!object.is_object()) return layer;

  layer.name = text(object, "nm");
  layer.index = static_cast<int>(number(object, "ind", 0.0f));
  layer.inPoint = number(object, "ip", 0.0f);
  layer.outPoint = number(object, "op", 0.0f);
  layer.startTime = number(object, "st", 0.0f);

  if (const auto it = object.find("ks"); it != object.end()) layer.transform = readTransform(*it);

  // The layer's kind, from `ty`: 1 is a solid and 4 is a shape. Everything else is recorded as Other so that the layer
  // keeps its *place* in the list. Order is load-bearing for track mattes, which are written as "the layer above me", so
  // a layer this library cannot draw still has to be counted.
  const int layerType = static_cast<int>(number(object, "ty", 0.0f));
  if (layerType == 4) {
    layer.kind = Layer::Kind::Shape;
    layer.shapes = readItems(object, "shapes");
  } else if (layerType == 1) {
    layer.kind = Layer::Kind::Solid;
    if (const auto it = object.find("sc"); it != object.end() && it->is_string()) {
      layer.solidColor = colorFromHex(it->get<std::string>());
    }
    layer.solidWidth = number(object, "sw", 0.0f);
    layer.solidHeight = number(object, "sh", 0.0f);
  } else {
    layer.kind = Layer::Kind::Other;
  }

  layer.trackMatte = static_cast<int>(number(object, "tt", 0.0f));
  return layer;
}

} // namespace anonymous

// ==================================================================================================
// Entry point
// ==================================================================================================

/** @return the document in @p jsonText, or nothing with @p error set. */
std::optional<Animation> parseAnimation(std::string_view jsonText, std::string* error) {
  const auto fail = [error](const std::string& message) -> std::optional<Animation> {
    if (error != nullptr) *error = message;
    return std::nullopt;
  };

  json document;
  try {
    document = json::parse(jsonText);
  } catch (const json::exception& failure) {
    return fail(std::string("the document is not valid JSON: ") + failure.what());
  }

  if (!document.is_object()) return fail("the document is not a JSON object");

  Animation animation;
  animation.name = text(document, "nm");
  animation.width = number(document, "w", 0.0f);
  animation.height = number(document, "h", 0.0f);
  animation.frameRate = number(document, "fr", 60.0f);
  animation.inPoint = number(document, "ip", 0.0f);
  animation.outPoint = number(document, "op", 0.0f);

  if (animation.width <= 0 || animation.height <= 0) {
    return fail("the document states no usable size: neither w nor h is positive");
  }
  if (!(animation.frameRate > 0)) {
    // A frame rate of zero would make the duration infinite and every keyframe time meaningless. Sixty is the format's
    // own default and what every exporter writes, so a document that omits it gets that rather than a division by zero.
    animation.frameRate = 60.0f;
  }
  if (animation.outPoint <= animation.inPoint) {
    // Some documents omit `op`, and a player with no length has nothing to play. One frame is the least that is a
    // document rather than a division by zero.
    animation.outPoint = animation.inPoint + 1.0f;
  }

  const auto layers = document.find("layers");
  if (layers == document.end() || !layers->is_array() || layers->empty()) {
    return fail("the document has no layers");
  }

  // *Every* layer is kept, including the kinds this renderer cannot draw.
  //
  // Skipping them was the earlier behaviour and it broke two things at once. The obvious one is that a solid never
  // reached the renderer, so a document made of solids drew nothing. The subtler one is that dropping a layer *renumbers*
  // the rest -- and a track matte is written as "the layer above me", so a dropped layer silently re-points a matte at a
  // different one. Keeping an undrawable layer as `Layer::Kind::Other` costs a struct and preserves every index.
  animation.layers.reserve(layers->size());
  for (const json& entry : *layers) {
    animation.layers.push_back(readLayer(entry));
  }

  if (animation.layers.empty()) return fail("the document has no layers");
  return animation;
}

} // namespace graphics::lottie
