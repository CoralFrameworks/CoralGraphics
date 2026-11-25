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

#ifndef CORAL_GRAPHICS_SHADER_H
#define CORAL_GRAPHICS_SHADER_H

#include <graphics/Bitmap.h>
#include <graphics/Enums.h>

#include <geometry/Color.h>
#include <geometry/Matrix.h>
#include <geometry/Point.h>
#include <geometry/Scalar.h>

#include <memory>
#include <vector>

/**
 * @file Shader.h
 *
 * What fills a shape, when a single colour is not enough.
 *
 * ## Why a gradient is a shader rather than a kind of paint
 *
 * A paint says *how* to draw -- fill or stroke, which blend, how wide. A shader says *what colour* at each point, and it
 * is orthogonal to all of that: the same gradient fills a rectangle and strokes a curve, and the same paint draws solid
 * in one call and through a gradient in the next. Hanging the gradient off the paint as a field is what lets a caller
 * change their mind without changing the call.
 *
 * ## The two spaces, and the matrix between them
 *
 * A gradient is written in coordinates that are natural to whatever produced it: an SVG document writes them in user
 * space, or in a 0..1 box around the shape being filled. The canvas draws in device space. `localMatrix` is what carries
 * one to the other, and it maps *shader* space to *drawing* space -- so sampling a point in drawing space goes through
 * its inverse. A document that writes its gradient in the shape's bounding box therefore supplies a matrix built from
 * that box, and nothing else has to know.
 *
 * ## Interpolation
 *
 * Stops blend channel by channel in the sRGB values they were written in, with no premultiplication first. That is what
 * SVG specifies and what a designer sees in their tool; interpolating premultiplied would darken a fade to transparent,
 * which is a difference that shows up in every gradient that ends in `stop-opacity="0"`.
 */

namespace graphics {

/** One stop of a gradient: a colour, and where along the gradient it sits. */
struct GradientStop {
  /** Where along the gradient, from zero at the start to one at the end. Values outside that range are allowed and are
   *  what `TileMode::kClamp` exists to make sense of. */
  Scalar position = 0;
  Color color;
};

/**
 * A colour that varies across the drawing.
 *
 * Not polymorphic: the two kinds are a tagged pair rather than a class hierarchy, for the same reason `ColorFilter` is
 * -- the set is closed, a caller never supplies their own, and a virtual call per pixel is a cost paid for a flexibility
 * nothing asks for. Sampling happens once per covered pixel, so it is the one place in this library where the difference
 * between a switch and a virtual call is measurable.
 */
class Shader {
public:
  enum class Kind : uint8_t {
    /** Colours vary along the line from a start point to an end point. */
    kLinearGradient,
    /** Colours vary with distance from a centre, reaching the last stop at the radius. */
    kRadialGradient,
    /** A picture, sampled through the local matrix and repeated by the tile mode. */
    kImage,
  };

  /**
   * A gradient along the line from @p start to @p end.
   *
   * A start and end that coincide describe no direction, and the shader reports itself invalid rather than dividing by
   * zero -- see @c isValid.
   */
  [[nodiscard]] static std::shared_ptr<Shader>
  LinearGradient(Point start, Point end, std::vector<GradientStop> stops, TileMode tile = TileMode::kClamp,
    const Matrix& localMatrix = Matrix{});

  /** A gradient from @p center outwards, reaching the last stop at @p radius. */
  [[nodiscard]] static std::shared_ptr<Shader>
  RadialGradient(Point center, Scalar radius, std::vector<GradientStop> stops, TileMode tile = TileMode::kClamp,
    const Matrix& localMatrix = Matrix{});

  /**
   * A picture, sampled repeatedly.
   *
   * @param image the tile. An empty one makes the shader invalid, as a gradient with no stops does -- so a caller that
   *        built one from a document gets a shape painted flat rather than a shape that vanished.
   * @param tile what happens outside the picture. A pattern is `kRepeat` almost always; the other modes are here because
   *        they are the same four the gradients use, and a caller assembling a shader from a document should not have to
   *        translate between two spellings of the same idea.
   * @param localMatrix maps the image's own coordinates -- pixels, origin at its top-left -- into the drawing. A
   *        pattern's placement and scale live here, which is what keeps them out of everything else.
   */
  [[nodiscard]] static std::shared_ptr<Shader> ImageShader(Bitmap image, TileMode tile = TileMode::kRepeat,
    const Matrix& localMatrix = Matrix{});

  [[nodiscard]] Kind kind() const noexcept { return mKind; }
  [[nodiscard]] const std::vector<GradientStop>& stops() const noexcept { return mStops; }
  [[nodiscard]] TileMode tileMode() const noexcept { return mTile; }
  [[nodiscard]] const Matrix& localMatrix() const noexcept { return mLocalMatrix; }

  /** The start point of a linear gradient, or the centre of a radial one. */
  [[nodiscard]] Point start() const noexcept { return mStart; }
  /** The end point of a linear gradient; unused by a radial one. */
  [[nodiscard]] Point end() const noexcept { return mEnd; }
  /** The radius of a radial gradient; unused by a linear one. */
  [[nodiscard]] Scalar radius() const noexcept { return mRadius; }

  /** The tile an image shader repeats. Empty for the two gradient kinds. */
  [[nodiscard]] const Bitmap& image() const noexcept { return mImage; }

  /**
   * @return true when this shader can produce a colour.
   *
   * False for a linear gradient whose endpoints coincide and for a radial one of no radius, and for anything with fewer
   * than two stops -- there is nothing to interpolate between. A paint holding an invalid shader falls back to its own
   * colour rather than drawing nothing, which is the difference between a gradient that failed and a shape that vanished.
   */
  [[nodiscard]] bool isValid() const noexcept { return mValid; }

  /**
   * @return the colour at @p point, which is in the space the shader is applied in -- drawing space, not shader space.
   *
   * Applies the inverse of the local matrix, so a caller sampling device coordinates need not know how the gradient was
   * written.
   */
  [[nodiscard]] Color sampleAt(Point point) const;

private:
  Shader() = default;

  Kind mKind = Kind::kLinearGradient;
  std::vector<GradientStop> mStops;
  TileMode mTile = TileMode::kClamp;
  Matrix mLocalMatrix;
  bool mLocalMatrixInvertible = true;
  Point mStart{ 0, 0 };
  Point mEnd{ 1, 0 };
  Scalar mRadius = 1;
  Bitmap mImage;
  bool mValid = false;
};

} // namespace graphics

#endif // CORAL_GRAPHICS_SHADER_H
