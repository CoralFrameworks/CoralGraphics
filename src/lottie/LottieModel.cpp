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
 * @file LottieModel.cpp
 *
 * The two operations the model itself answers: a vertex path as an outline, and a transform at a moment.
 */

#include "lottie/LottieModel.h"

#include <algorithm>
#include <cmath>

namespace graphics::lottie {
namespace {

/** Degrees to radians, as the matrix factories want them. */
Radians toRadians(const Scalar degrees) { return Radians {degrees * 3.14159265358979323846f / 180.0f}; }

/**
 * Lottie's skew, as a matrix.
 *
 * The format's skew is CSS's `skewX` with an explicit axis: rotate the axis to the x axis, shear, rotate back. Written
 * out rather than approximated, because a shear about the wrong axis is a picture that looks like a rendering artefact
 * rather than like a wrong transform.
 *
 * The shear itself is `MakeSkew(0, tan(skew))`, which gives `x' = x + tan(skew) * y`. That is Skia's `Skew(tan(skew), 0)`
 * with the convention flipped: Skia multiplies row vectors, so the term that lands in x is the one this library's
 * `MakeSkew` calls `sy`.
 *
 * The angle is clamped to 85 degrees, which is Skottie's own limit: at 90 the tangent is infinite and the matrix is not
 * invertible, and a document that states one means "as sheared as possible" rather than "collapse the picture".
 */
Matrix skewMatrix(const Scalar skewDegrees, const Scalar axisDegrees) {
  if (skewDegrees == 0.0f) return Matrix {};

  constexpr Scalar kMaxSkewDegrees = 85.0f;
  const Scalar clamped = std::clamp(skewDegrees, -kMaxSkewDegrees, kMaxSkewDegrees);
  const Scalar shear = std::tan(-toRadians(clamped).radians);

  return Matrix::MakeRotationZ(toRadians(axisDegrees)) * Matrix::MakeSkew(0.0f, shear) *
         Matrix::MakeRotationZ(toRadians(-axisDegrees));
}

/** A scale in percent, as the multiplier the matrix wants. An absent or short scale is 100% on every axis. */
Vector3 scaleAtFrame(const Animatable<std::vector<Scalar>>& scale, const Scalar frame) {
  const std::vector<Scalar> values = scale.at(frame);
  const auto component = [&values](const size_t index) {
    return index < values.size() ? values[index] / 100.0f : 1.0f;
  };
  // The third component is a percentage on the z axis, which is 100 for a 2D document. It is read rather than forced so
  // that a 3D document is not silently flattened, though nothing here projects it.
  return Vector3 {component(0), component(1), values.size() > 2 ? component(2) : 1.0f};
}

} // namespace anonymous

// ==================================================================================================
// A vertex path as an outline
// ==================================================================================================

Path VertexPath::toPath() const {
  Path path;
  if (vertices.size() < 2) {
    // A single vertex is a dot, which Lottie draws as nothing: a contour needs an edge. Returning the empty path is
    // what makes a degenerate shape invisible rather than a blob at the right place.
    return path;
  }

  const auto tangent = [](const std::vector<Point>& tangents, const size_t index) {
    return index < tangents.size() ? tangents[index] : Point {0, 0};
  };

  path.moveTo(vertices.front());

  // The segment count is the number of edges, which for a closed path is the vertex count and for an open one is one
  // fewer -- the closing edge is the contour's own.
  const size_t edges = closed ? vertices.size() : vertices.size() - 1;
  for (size_t i = 0; i < edges; ++i) {
    const size_t next = (i + 1) % vertices.size();
    // Both control points are absolute here: the tangents are stated relative to their own vertex, which is what the
    // format stores and what makes a straight edge a pair of zero tangents rather than a repeated point.
    const Point control1 {vertices[i].x + tangent(outTangents, i).x, vertices[i].y + tangent(outTangents, i).y};
    const Point control2 {vertices[next].x + tangent(inTangents, next).x,
                          vertices[next].y + tangent(inTangents, next).y};
    path.cubicTo(control1, control2, vertices[next]);
  }

  if (closed) path.close();
  return path;
}

VertexPath interpolate(const VertexPath& a, const VertexPath& b, const Scalar t) {
  // ## Mismatched vertex counts hold rather than interpolate
  //
  // Lottie animates a shape morph by pairing vertex i of one keyframe with vertex i of the next, which requires the two
  // to have the same number of vertices. When they do not -- which the format permits and which a designer produces by
  // editing a shape mid-animation -- there is no pairing, and every reading of it is a guess.
  //
  // The guess this makes is to *hold* the first shape until the next keyframe, which is what Skottie does. Interpolating
  // by index up to the shorter count and dropping the rest would produce a shape that is neither of the two the document
  // describes, and it would do it smoothly, which is worse: a wrong shape that moves looks deliberate.
  if (a.size() != b.size() || a.inTangents.size() != b.inTangents.size() ||
      a.outTangents.size() != b.outTangents.size()) {
    return a;
  }

  VertexPath result;
  result.closed = a.closed;
  result.vertices.resize(a.vertices.size());
  result.inTangents.resize(a.inTangents.size());
  result.outTangents.resize(a.outTangents.size());

  for (size_t i = 0; i < a.vertices.size(); ++i) {
    result.vertices[i] = interpolate(a.vertices[i], b.vertices[i], t);
    result.inTangents[i] = interpolate(a.inTangents[i], b.inTangents[i], t);
    result.outTangents[i] = interpolate(a.outTangents[i], b.outTangents[i], t);
  }
  return result;
}

// ==================================================================================================
// A transform at a moment
// ==================================================================================================

Matrix Transform::matrixAt(const Scalar frame) const {
  const Point anchorPoint = anchor.at(frame);
  const Point positionPoint = position.at(frame);
  const Scalar degrees = rotation.at(frame);
  const Vector3 scaleVector = scaleAtFrame(scale, frame);

  // The order is Skottie's, and each term is load-bearing:
  //
  //   * the anchor is subtracted *last* in reading order and therefore applied first to the geometry, which is what
  //     makes it the point that stays still under the rotation and the scale;
  //   * the scale comes before the rotation, so a non-uniform scale is applied in the shape's own axes rather than in
  //     the rotated ones -- the difference between a squashed gear and a sheared one;
  //   * the position is added last, in the parent's coordinates.
  //
  // Written as one product with the rightmost applied first, which is this library's column-vector convention.
  return Matrix::MakeTranslation(Vector3 {positionPoint.x, positionPoint.y, 0}) *
         Matrix::MakeRotationZ(toRadians(degrees)) * skewMatrix(skew.at(frame), skewAxis.at(frame)) *
         Matrix::MakeScale(scaleVector) *
         Matrix::MakeTranslation(Vector3 {-anchorPoint.x, -anchorPoint.y, 0});
}

Scalar Transform::opacityAt(const Scalar frame) const {
  // The format states opacity as a percentage, and a document that omits it means fully opaque rather than fully
  // transparent -- so an absent property is 100 and not the zero a default-constructed Animatable would give.
  const Animatable<Scalar>& property = opacity.isEmpty() ? Animatable<Scalar>::constant(100.0f) : opacity;
  return std::clamp(property.at(frame) / 100.0f, 0.0f, 1.0f);
}

} // namespace graphics::lottie
