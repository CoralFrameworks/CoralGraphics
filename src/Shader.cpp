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
 * @file Shader.cpp
 *
 * Gradient evaluation.
 *
 * ## What sampling costs
 *
 * Two multiplies for the point, one subtract, and a binary search for the bracketing stops -- or a linear scan, since a
 * gradient with more than a handful of stops is rare and the search would cost more than it saves. Once per covered
 * pixel, in the inner loop of the rasterizer, which is why nothing here allocates and why the stops are a flat vector
 * walked by index.
 *
 * ## The tile modes
 *
 * `t` is the position along the gradient, zero at the start and one at the end. What happens outside that range is the
 * tile mode, and the four are different answers to a question a caller genuinely has:
 *
 *   * `kClamp` holds the end colours, which is what a stop list that does not reach zero or one means;
 *   * `kRepeat` and `kMirror` tile the gradient, and a gradient with `kRepeat` is how a striped fill is written;
 *   * `kDecal` draws nothing outside, which is the only one that produces transparency.
 */

#include <graphics/Shader.h>

#include <algorithm>
#include <cmath>

namespace graphics {
namespace {

/**
 * Maps a pixel index onto one the image actually has, according to @p tile.
 *
 * @param out the index to read, valid only when this returns true.
 * @return false when @p tile is `kDecal` and @p index is outside the image -- there is no pixel to read.
 *
 * Integer arithmetic on whole indices, deliberately. A first version worked on a normalized coordinate and multiplied it
 * by the size again, which normalized twice and put every sample one pixel to the left.
 */
bool tilePixel(const int index, const int size, const TileMode tile, int& out) {
  switch (tile) {
    case TileMode::kClamp:
      out = std::clamp(index, 0, size - 1);
      return true;

    case TileMode::kRepeat:
      // Positive modulo. `%` in C++ keeps the sign of the dividend, so a negative index would stay negative and index
      // before the start of the image.
      out = ((index % size) + size) % size;
      return true;

    case TileMode::kMirror: {
      // A period of two images: the second half runs backwards.
      const int period = 2 * size;
      const int folded = ((index % period) + period) % period;
      out = folded < size ? folded : period - 1 - folded;
      return true;
    }

    case TileMode::kDecal:
      if (index < 0 || index >= size) return false;
      out = index;
      return true;
  }
  return false;
}

/** One channel of an un-premultiplied pixel, by shift: 24 alpha, 16 red, 8 green, 0 blue. */
Scalar channelOf(const uint32_t pixel, const int shift) {
  return static_cast<Scalar>((pixel >> shift) & 0xFFu) / 255.0f;
}

/**
 * @return the colour at a point in the image's own coordinates, or transparent black outside a `decal` tile.
 *
 * Bilinear because a tile is nearly always drawn at a scale other than one -- that is what a pattern is for -- and
 * nearest-neighbour at a non-unit scale either blocks or aliases depending on the direction.
 *
 * The taps are read un-premultiplied, mixed, and left un-premultiplied: the rasteriser premultiplies when it blends, and
 * doing it here as well would do it twice.
 */
Color sampleImage(const Pixmap& pixmap, const Scalar x, const Scalar y, const TileMode tile) {
  // Pixel units. Pixel `i` covers [i, i+1) and is sampled at its centre, `i + 0.5`, so a sample's neighbours are the
  // whole indices either side of `x - 0.5` and the weights are its fractional part.
  const Scalar u = x - 0.5f;
  const Scalar v = y - 0.5f;

  const int baseX = static_cast<int>(std::floor(u));
  const int baseY = static_cast<int>(std::floor(v));
  const Scalar fractionX = u - static_cast<Scalar>(baseX);
  const Scalar fractionY = v - static_cast<Scalar>(baseY);

  Scalar red = 0;
  Scalar green = 0;
  Scalar blue = 0;
  Scalar alpha = 0;

  for (int row = 0; row < 2; ++row) {
    for (int column = 0; column < 2; ++column) {
      int pixelX = 0;
      int pixelY = 0;
      // Each tap goes through the tile mode on its own. Applying the mode once to the sample point instead leaves a seam
      // at every tile boundary, where the interpolated tap came from the wrong side of it.
      if (!tilePixel(baseX + column, pixmap.width(), tile, pixelX)) continue;
      if (!tilePixel(baseY + row, pixmap.height(), tile, pixelY)) continue;

      const Scalar weight =
              (column == 0 ? 1.0f - fractionX : fractionX) * (row == 0 ? 1.0f - fractionY : fractionY);
      if (weight <= 0.0f) continue;

      const uint32_t texel = pixmap.getPixelARGB32(pixelX, pixelY);
      red += channelOf(texel, 16) * weight;
      green += channelOf(texel, 8) * weight;
      blue += channelOf(texel, 0) * weight;
      alpha += channelOf(texel, 24) * weight;
    }
  }

  return Color {red, green, blue, alpha};
}

/** Clamps, wraps or reflects @p t into [0, 1] according to @p tile, and reports whether the result is drawn at all. */
bool tilePosition(Scalar& t, const TileMode tile) {
  switch (tile) {
    case TileMode::kClamp:
      t = std::clamp(t, 0.0f, 1.0f);
      return true;

    case TileMode::kRepeat:
      // `t - floor(t)` is the fractional part and is correct for negatives, which `fmod` is not: fmod(-0.25, 1) is
      // -0.25, which would leave the gradient sampling outside its own range on one side only.
      t -= std::floor(t);
      return true;

    case TileMode::kMirror: {
      t -= std::floor(t / 2.0f) * 2.0f; // into [0, 2)
      if (t > 1.0f) t = 2.0f - t;       // and fold the top half back
      return true;
    }

    case TileMode::kDecal:
      return t >= 0.0f && t <= 1.0f;
  }
  return true;
}

/** Blends the stops at @p t, which is already in [0, 1]. */
Color sampleStops(const std::vector<GradientStop>& stops, const Scalar t) {
  if (stops.empty()) return Color::MakeRGBA8(0, 0, 0, 0);
  if (stops.size() == 1 || t <= stops.front().position) return stops.front().color;
  if (t >= stops.back().position) return stops.back().color;

  // A linear walk: the list is short, sorted, and a binary search would cost more in branches than it saves in
  // comparisons at the sizes gradients actually come in.
  for (size_t i = 1; i < stops.size(); ++i) {
    const GradientStop& upper = stops[i];
    if (t > upper.position) continue;

    const GradientStop& lower = stops[i - 1];
    const Scalar span = upper.position - lower.position;
    // Coincident stops are a hard edge, not a division by zero: a stop list with two stops at the same position is how
    // a document writes a sharp transition.
    const Scalar f = span > 0 ? (t - lower.position) / span : 1.0f;

    // Channel by channel in the values as written, with no premultiplication. See the note in the header.
    const auto mix = [f](const Scalar a, const Scalar b) { return a + (b - a) * f; };
    return Color {mix(lower.color.red, upper.color.red), mix(lower.color.green, upper.color.green),
                  mix(lower.color.blue, upper.color.blue), mix(lower.color.alpha, upper.color.alpha)};
  }
  return stops.back().color;
}

/** Sorts the stops and drops any that share a position, keeping the last -- which is the one CSS keeps. */
void normaliseStops(std::vector<GradientStop>& stops) {
  std::stable_sort(stops.begin(), stops.end(),
                   [](const GradientStop& a, const GradientStop& b) { return a.position < b.position; });
}

} // namespace anonymous

std::shared_ptr<Shader> Shader::LinearGradient(const Point start, const Point end, std::vector<GradientStop> stops,
                                               const TileMode tile, const Matrix& localMatrix) {
  auto shader = std::shared_ptr<Shader>(new Shader());
  shader->mKind = Kind::kLinearGradient;
  shader->mStart = start;
  shader->mEnd = end;
  shader->mStops = std::move(stops);
  shader->mTile = tile;
  shader->mLocalMatrix = localMatrix;

  normaliseStops(shader->mStops);

  // A gradient along a line of no length has no direction to vary along, and evaluating it would divide by zero. The
  // shader is still returned, marked invalid, so a caller that built it from data can ask why rather than get a null.
  const Scalar dx = end.x - start.x;
  const Scalar dy = end.y - start.y;
  shader->mValid = shader->mStops.size() >= 2 && (dx * dx + dy * dy) > 0 && localMatrix.isInvertible();
  shader->mLocalMatrixInvertible = localMatrix.isInvertible();
  return shader;
}

std::shared_ptr<Shader> Shader::RadialGradient(const Point center, const Scalar radius,
                                               std::vector<GradientStop> stops, const TileMode tile,
                                               const Matrix& localMatrix) {
  auto shader = std::shared_ptr<Shader>(new Shader());
  shader->mKind = Kind::kRadialGradient;
  shader->mStart = center;
  shader->mEnd = Point {center.x + radius, center.y};
  shader->mRadius = radius;
  shader->mStops = std::move(stops);
  shader->mTile = tile;
  shader->mLocalMatrix = localMatrix;

  normaliseStops(shader->mStops);

  shader->mValid = shader->mStops.size() >= 2 && radius > 0 && localMatrix.isInvertible();
  shader->mLocalMatrixInvertible = localMatrix.isInvertible();
  return shader;
}

std::shared_ptr<Shader> Shader::ImageShader(Bitmap image, const TileMode tile, const Matrix& localMatrix) {
  auto shader = std::shared_ptr<Shader>(new Shader());
  shader->mKind = Kind::kImage;
  shader->mImage = std::move(image);
  shader->mTile = tile;
  shader->mLocalMatrix = localMatrix;
  shader->mLocalMatrixInvertible = localMatrix.isInvertible();

  // An empty tile has nothing to sample, and a local matrix with no inverse cannot be asked where a point came from.
  // Either way the shader is returned marked invalid, so a caller that built it from a document gets the flat colour its
  // paint already holds rather than a missing shape.
  shader->mValid = !shader->mImage.isEmpty() && localMatrix.isInvertible();
  return shader;
}

Color Shader::sampleAt(const Point point) const {
  if (!mValid) return Color::MakeRGBA8(0, 0, 0, 0);

  // Drawing space back to shader space. The matrix was checked invertible when the shader was built, so this is safe
  // here -- testing it per pixel would be a branch on a condition that cannot have changed.
  const Point local = mLocalMatrixInvertible ? mLocalMatrix.invert() * point : point;

  // The picture is sampled in its own coordinates, which the local matrix has just produced -- no tile position and no
  // stop list, so it returns before either is computed.
  if (mKind == Kind::kImage) return sampleImage(mImage.makePixmap(), local.x, local.y, mTile);

  Scalar t = 0;
  if (mKind == Kind::kLinearGradient) {
    const Scalar dx = mEnd.x - mStart.x;
    const Scalar dy = mEnd.y - mStart.y;
    const Scalar lengthSquared = dx * dx + dy * dy;
    t = ((local.x - mStart.x) * dx + (local.y - mStart.y) * dy) / lengthSquared;
  } else {
    const Scalar dx = local.x - mStart.x;
    const Scalar dy = local.y - mStart.y;
    t = std::sqrt(dx * dx + dy * dy) / mRadius;
  }

  if (!tilePosition(t, mTile)) return Color::MakeRGBA8(0, 0, 0, 0);
  return sampleStops(mStops, t);
}

} // namespace graphics
