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

#ifndef CORAL_GRAPHICS_LOTTIE_LOTTIE_MODEL_H
#define CORAL_GRAPHICS_LOTTIE_LOTTIE_MODEL_H

#include "lottie/LottieValue.h"

#include <graphics/Path.h>

#include <geometry/Matrix.h>
#include <geometry/Point.h>
#include <geometry/Rect.h>
#include <geometry/Scalar.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * @file LottieModel.h
 *
 * A Lottie document as a tree of typed objects, and the two things the tree can be asked: what a transform is at a
 * moment, and what shape a path is.
 *
 * ## The model is a translation of the format, not of the renderer
 *
 * Every type here corresponds to something the format defines, and nothing here knows about a canvas. That separation is
 * what lets the same model drive a raster renderer now and anything else later, and it is what makes the parser
 * testable without drawing: a parsed document can be asked about a layer's matrix at frame 90 without a surface
 * existing.
 *
 * ## Units
 *
 * Lottie states a scale and an opacity in **percent**, so a scale of `[11.562, 11.562]` means the shape is drawn at
 * 11.562% -- and opacity is a percentage rather than the 0-1 fraction the canvas wants. Both are converted where they
 * are used rather than at parse time, so the model reads like the file it came from and a comparison in a test is a
 * comparison against the document.
 *
 * Rotation is in degrees and time is in frames, for the same reason.
 */

namespace graphics::lottie {

/**
 * A cubic-vertex path: the form Lottie states every shape in.
 *
 * A vertex list plus one tangent per vertex, each tangent stored *relative to its vertex* -- which is what the format
 * does and why an un-tangented path can be written with zero tangents rather than with the vertex repeated three times.
 * The two tangent arrays are the control point arriving at a vertex and the one leaving it, so the cubic from vertex i
 * to vertex i+1 uses `out[i]` and `in[i+1]`.
 */
struct VertexPath {
  std::vector<Point> vertices;
  std::vector<Point> inTangents;
  std::vector<Point> outTangents;
  bool closed = false;

  [[nodiscard]] bool isEmpty() const noexcept { return vertices.empty(); }
  [[nodiscard]] size_t size() const noexcept { return vertices.size(); }

  /** @return the outline, with a cubic segment per edge and every tangent taken absolute. */
  [[nodiscard]] Path toPath() const;
};

/** Interpolates two vertex paths vertex by vertex. See the implementation for the mismatched case. */
[[nodiscard]] VertexPath interpolate(const VertexPath& a, const VertexPath& b, Scalar t);

/**
 * A transform, exactly as the format states one.
 *
 * Every field is animatable because the format allows every field to be, and the defaults are Lottie's own: an absent
 * `a` is the origin, an absent `s` is 100%, and an absent `o` is fully opaque.
 */
struct Transform {
  Animatable<Point> anchor;
  Animatable<Point> position;
  Animatable<Scalar> rotation;
  Animatable<Scalar> opacity;
  Animatable<Scalar> skew;
  Animatable<Scalar> skewAxis;

  /** The scale as an animatable vector, which is how the format writes it: two or three numbers in percent. */
  Animatable<std::vector<Scalar>> scale;

  /**
   * @return the matrix at @p frame, mapping this node's own space into its parent's.
   *
   * The order is Skottie's, and it matters: applying the scale before the rotation, or the anchor after the position,
   * produces a picture that is plausible at the identity and wrong as soon as anything moves.
   */
  [[nodiscard]] Matrix matrixAt(Scalar frame) const;

  /** @return the opacity at @p frame as a fraction in 0-1, converting from the format's percentage. */
  [[nodiscard]] Scalar opacityAt(Scalar frame) const;
};

/** How a merge modifier combines the paths above it. The numbers are the format's `mm`. */
enum class MergeMode : uint8_t {
  /** Splice the contours together and let the fill rule decide. See `Path::appendPath`. */
  kMerge = 1,
  kUnion = 2,
  kDifference = 3,
  kIntersection = 4,
  kExclusion = 5,
};

/** What a shape item is. The values are the format's `ty`, where it has a number. */
enum class ShapeKind : uint8_t {
  Group,
  Path,
  /** `rc`: a rectangle, with corners rounded by `r`. */
  Rectangle,
  /** `el`: an ellipse. */
  Ellipse,
  /** `sr`: a star or a regular polygon, depending on `sy`. */
  Polystar,
  /** `tm`: keeps a range of the pending geometry's length and discards the rest. */
  Trim,
  /** `rp`: draws everything painted so far again, transformed. */
  Repeater,
  Merge,
  Fill,
  Stroke,
  /** `gf`: a fill painted with a gradient. */
  GradientFill,
  /** `gs`: a stroke painted with a gradient. */
  GradientStroke,
  Transform,
  Unknown,
};

struct Shape;
using ShapePtr = std::unique_ptr<Shape>;

/**
 * One shape item. A single type rather than a hierarchy, because the format's items differ in their data and not in
 * their behaviour -- and a group holds a heterogeneous list of them, which a tagged struct iterates without a cast.
 */
/**
 * Parametric geometry: `rc`, `el` and `sr`.
 *
 * One struct rather than three, because they are one idea -- a path computed from a handful of animatable numbers -- and
 * the renderer treats them identically: each produces a @c Path for a frame and nothing else. @c kind says which of the
 * numbers matter.
 *
 * The shapes are written around a **centre**, not a corner, and all three carry the same `s` and `p` meaning.
 */
struct Geometry {
  enum class Kind : uint8_t {
    /** `rc`: a rectangle, corners rounded by @c roundness. */
    Rectangle,
    /** `el`: an ellipse inscribed in the bounds. */
    Ellipse,
    /** `sr`: a star or a polygon, per @c star. */
    Polystar,
  };

  Kind kind = Kind::Rectangle;

  /** `s`: the size, which a rectangle and an ellipse both use. */
  Animatable<Point> size;
  /** `p`: the centre. */
  Animatable<Point> position;

  /** A rectangle's `r`: the corner radius, clamped so the corners cannot overlap. */
  Animatable<Scalar> roundness;

  /** `d`: 3 means counter-clockwise. A rectangle and an ellipse both carry it. */
  bool counterClockwise = false;

  // Polystar, from `sr`.
  /** `sy` is 1 for a star, which alternates an outer and an inner radius, and 2 for a plain polygon. */
  bool star = true;
  /** `pt`: how many points. */
  Animatable<Scalar> pointCount;
  /** `r`: how far the first point is turned from straight up, in degrees. */
  Animatable<Scalar> rotation;
  /** `or` and `ir`: the radii the points alternate between. */
  Animatable<Scalar> outerRadius;
  Animatable<Scalar> innerRadius;
  /**
   * `os` and `is`: the roundness of the outer and inner points.
   *
   * Read and unused, exactly as Skottie leaves them -- its `Polystar.cpp` carries `// TODO: inner/outer "roundness"?`.
   * Keeping them means the parser reports what the document said rather than silently dropping it; not applying them
   * means a rounded star is drawn with sharp points, which is visible and recorded rather than hidden.
   */
  Animatable<Scalar> outerRoundness;
  Animatable<Scalar> innerRoundness;
};

/**
 * `tm`: trimming the pending geometry to a range of its own length.
 *
 * Every shape underneath is measured and cut, so what a trim produces depends on everything before it in the group --
 * which is why the renderer applies it to the pending list rather than to a single item.
 */
struct Trim {
  /** `s` and `e`: the two ends of the kept range, as percentages of the length. Interchangeable, as a dragged handle is. */
  Animatable<Scalar> start;
  Animatable<Scalar> end;
  /** `o`: how far the range is slid along the path, in **degrees** -- a full turn being the whole path. */
  Animatable<Scalar> offset;

  /**
   * `m == 2`: join the pending geometry and trim the result, rather than trimming each piece on its own.
   *
   * Read as "the shapes are trimmed individually", which is what it looks like to the user, and which is achieved by
   * trimming them as one path. See the note in the parser for why the two spellings are the other way round.
   */
  bool trimsAsOnePath = false;
};

/**
 * `rp`: drawing everything painted so far again, `count` times.
 *
 * A repeater repeats *draws*, not geometry -- it is attached to the list of things already painted, so it multiplies
 * whatever is above it in its group rather than adding shapes. That is why it needs the renderer to defer its painting.
 */
struct Repeater {
  /** `c`: how many copies. Fractions are rounded, and Skottie caps the count at 1024. */
  Animatable<Scalar> count;
  /** `o`: where the copies start counting from, which slides the whole series along its own transform. */
  Animatable<Scalar> offset;

  // From `tr`, the repeater's own transform. Every field is scaled by the copy index -- see the note in LottieParser.cpp
  // for the exact expression, which is the part of a repeater that is easiest to get wrong.
  /** `a`: the point the copies scale and rotate about. */
  Animatable<Point> anchor;
  /** `p`: how far each copy moves from the last, as a vector rather than a distance. */
  Animatable<Point> position;
  /** `s`: the scale factor *per copy*, as a percentage. Compounding, not linear. */
  Animatable<Point> scale;
  /** `r`: how far each copy turns from the last, in degrees. */
  Animatable<Scalar> rotation;
  /** `so` and `eo`: the opacity of the first and last copies, as percentages. */
  Animatable<Scalar> startOpacity;
  Animatable<Scalar> endOpacity;

  /**
   * `m == 1`: the copies are drawn back to front, so the later ones are painted under the earlier ones.
   *
   * Skottie names the two modes `kBelow` and `kAbove` and maps 1 to `kBelow`; anything else is `kAbove`. The mode
   * decides both the drawing order and which end of the opacity ramp each copy gets, so getting it backwards reverses
   * the fade as well as the stacking.
   */
  bool below = true;
};

/**
 * A gradient, as `gf` and `gs` describe one.
 *
 * The stops arrive as two independent runs -- colours, then opacities -- rather than as a list of (position, colour)
 * pairs. Reconciling the two runs' positions is the renderer's job; see @c gradientStops.
 */
struct Gradient {
  /** `t`: 1 is linear and 2 is radial. Anything else is read as linear, as Skottie does. */
  int type = 1;

  /** `s` and `e`, the gradient's axis. */
  Animatable<Point> start;
  Animatable<Point> end;

  /** `g.p`, the number of colour records. The opacity record count is implicit in the array's length. */
  int colorRecords = 0;

  /**
   * `g.k`: @c colorRecords records of (position, red, green, blue), followed by (length - colorRecords * 4) / 2 records
   * of (position, alpha).
   */
  Animatable<std::vector<Scalar>> records;

  /** Radial only. `h` and `a`: how far the focus sits along the axis, and the angle it is turned by. */
  Scalar highlightLength = 0;
  Scalar highlightAngle = 0;
};

struct Shape {
  ShapeKind kind = ShapeKind::Unknown;
  std::string name;
  bool hidden = false;

  // Group: its children, and its own transform.
  std::vector<ShapePtr> items;
  Transform transform;

  // Path.
  Animatable<VertexPath> path;

  // Merge.
  MergeMode merge = MergeMode::kMerge;

  // Fill and stroke. The format states a fill rule as `r`: 1 is nonzero and 2 is even-odd, which is the opposite
  // numbering to SVG's, where nonzero is the default spelled `nonzero` and even-odd is `evenodd`.
  Animatable<Color> color;
  Animatable<Scalar> colorOpacity;
  Animatable<Scalar> strokeWidth;
  FillType rule = FillType::Winding;

  // Gradient fill and stroke. The stroke width is shared with a plain stroke, because the format shares it too.
  std::optional<Gradient> gradient;

  // Parametric geometry. Exactly one shape kind fills this in.
  std::optional<Geometry> geometry;

  // A trim effect, which replaces the pending geometry rather than adding to it.
  std::optional<Trim> trim;

  // A repeater, which replaces the pending *draws* rather than the geometry.
  std::optional<Repeater> repeater;

  /** @return true when this item contributes geometry rather than paint or structure. */
  [[nodiscard]] bool isGeometry() const noexcept {
    return kind == ShapeKind::Path || kind == ShapeKind::Rectangle || kind == ShapeKind::Ellipse ||
           kind == ShapeKind::Polystar;
  }
};

/**
 * One layer.
 *
 * Two of the format's kinds are modelled: a shape layer (`ty` 4) and a solid (`ty` 1). The rest -- a precomposition, an
 * image, a null, text -- are @c Other and contribute nothing, which a caller can see from @c kind rather than having to
 * infer it from an empty shape list.
 */
struct Layer {
  enum class Kind : uint8_t {
    Shape,
    Solid,
    /** A kind this library does not draw. Kept as a layer so that layer *order* survives, which track mattes need. */
    Other,
  };

  std::string name;
  int index = 0;
  Kind kind = Kind::Other;
  Scalar inPoint = 0;
  Scalar outPoint = 0;
  Scalar startTime = 0;
  Transform transform;
  std::vector<ShapePtr> shapes;

  /** A solid's colour, `sc`, and its extent, `sw` and `sh`. */
  Color solidColor;
  Scalar solidWidth = 0;
  Scalar solidHeight = 0;

  /**
   * `tt`: the track matte this layer takes from the layer *above* it.
   *
   * 0 is none, 1 alpha, 2 inverted alpha, 3 luma, 4 inverted luma. The direction is the part that is easy to get
   * backwards: the matte is not the layer holding `tt`, it is the one before it in the list.
   */
  int trackMatte = 0;
};

/** A parsed document. */
struct Animation {
  std::string name;
  Scalar width = 0;
  Scalar height = 0;
  Scalar frameRate = 60;
  Scalar inPoint = 0;
  Scalar outPoint = 0;

  std::vector<Layer> layers;

  /** @return the length in seconds, which is what a player counts in. */
  [[nodiscard]] Scalar duration() const noexcept {
    return frameRate > 0 ? (outPoint - inPoint) / frameRate : 0;
  }
};

/**
 * Parses a Lottie document.
 *
 * @param error filled in when the text is not JSON, is not a Lottie document, or holds no shape layers; may be null.
 * @return the document, or nothing.
 */
[[nodiscard]] std::optional<Animation> parseAnimation(std::string_view jsonText, std::string* error = nullptr);

} // namespace graphics::lottie

#endif // CORAL_GRAPHICS_LOTTIE_LOTTIE_MODEL_H
