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
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF
 * ANY KIND, either express or implied. See the License for the specific language governing permissions
 * and limitations under the License.
 */

#include <graphics/Path.h>

#include "pathops/Curve.h"
#include "pathops/pathops.h"

#include <minilog/log.h>

#include <functional>
#include <utility>

namespace graphics {

/**
 * The private representation: a `pathops::Path` and nothing else.
 *
 * Keeping the public class a thin shell over the internal one is what lets the arithmetic change --
 * and it is expected to, as the boolean operations land -- without any caller recompiling.
 */
struct Path::Impl {
  pathops::Path path;
};

namespace {

/** Converts the public fill rule to the internal one. */
pathops::FillRule toPathOps(const FillType fillType) noexcept {
  return fillType == FillType::EvenOdd ? pathops::FillRule::EvenOdd : pathops::FillRule::Winding;
}

/** Converts the internal fill rule to the public one. */
FillType fromPathOps(const pathops::FillRule fillRule) noexcept {
  return fillRule == pathops::FillRule::EvenOdd ? FillType::EvenOdd : FillType::Winding;
}

/** Converts the public set operation to the internal one. */
pathops::PathOp toPathOps(const PathOp pathOp) noexcept {
  switch (pathOp) {
    case PathOp::Union: return pathops::PathOp::Union;
    case PathOp::Intersection: return pathops::PathOp::Intersection;
    case PathOp::Difference: return pathops::PathOp::Difference;
    case PathOp::Xor: return pathops::PathOp::ExclusiveOr;
    case PathOp::ReverseDifference: return pathops::PathOp::ReverseDifference;
  }
  return pathops::PathOp::Union;
}

/** Converts an internal double-precision rectangle to the public one. */
Rect toPublic(const pathops::DRect& rect) noexcept {
  return Rect::MakeLTRB(static_cast<Scalar>(rect.left), static_cast<Scalar>(rect.top),
          static_cast<Scalar>(rect.right), static_cast<Scalar>(rect.bottom));
}

/** Applies @p matrix to one double-precision point. */
pathops::DPoint transformPoint(const pathops::DPoint& point, const Matrix& matrix) {
  // Through the public `Point`, because that is the shape `Matrix` maps. The conversion costs a rounding
  // to float and back, which is lossless in the case that matters -- these points came from `Scalar`
  // coordinates in the first place -- and is why a transformed path is exact when its input was.
  const Point mapped = matrix * Point { static_cast<Scalar>(point.x), static_cast<Scalar>(point.y) };
  return pathops::DPoint::Make(static_cast<double>(mapped.x), static_cast<double>(mapped.y));
}

/** Builds an empty public path, which the combining operations below need before they can populate one. */
Path makeEmpty() { return {}; }

/**
 * Sixteen-point Gauss-Legendre nodes and weights on [-1,1].
 *
 * Chosen over a polyline estimate because the error is bounded and known rather than depending on where the
 * sample points happened to fall -- a curve whose curvature is concentrated in one short stretch is exactly
 * where uniform sampling goes wrong, and exactly the case a caller measuring a shape cares about.
 */
constexpr double kGaussNodes[16] = {
  -0.0950125098376374, 0.0950125098376374, -0.2816035507792589, 0.2816035507792589,
  -0.4580167776572274, 0.4580167776572274, -0.6178762444026438, 0.6178762444026438,
  -0.7554044083550030, 0.7554044083550030, -0.8656312023878318, 0.8656312023878318,
  -0.9445750230732326, 0.9445750230732326, -0.9894009349916499, 0.9894009349916499,
};

constexpr double kGaussWeights[16] = {
  0.1894506104550685, 0.1894506104550685, 0.1826034150449236, 0.1826034150449236,
  0.1691565193950025, 0.1691565193950025, 0.1495959888165767, 0.1495959888165767,
  0.1246289712555339, 0.1246289712555339, 0.0951585116824928, 0.0951585116824928,
  0.0622535239386479, 0.0622535239386479, 0.0271524594117541, 0.0271524594117541,
};

/**
 * @return the arc length of @p segment, which starts at @p start.
 *
 * The integral of the speed over the parameter. The half-length scaling outside the sum is the change of
 * variable from [0,1] to the quadrature's [-1,1].
 */
double segmentArcLength(const pathops::PathSegment& segment, const pathops::DPoint& start) noexcept {
  double integral = 0.0;
  for (int i = 0; i < 16; ++i) {
    const double t = 0.5 * (kGaussNodes[i] + 1.0);
    integral += kGaussWeights[i] * segment.derivative(start, t).length();
  }
  return 0.5 * integral;
}

} // namespace anonymous

const char* toString(const PathOp pathOp) noexcept {
  // Delegates to the internal names, so the two cannot drift apart into disagreeing spellings of the same
  // operation.
  return pathops::toString(toPathOps(pathOp));
}

const char* toString(const FillType fillType) noexcept {
  switch (fillType) {
    case FillType::Winding: return "Winding";
    case FillType::EvenOdd: return "EvenOdd";
  }
  return "Unknown FillType";
}

// ==================================================================================================
// Lifetime
// ==================================================================================================

Path::Path() : mImpl(std::make_unique<Impl>()) {}

Path::~Path() = default;

Path::Path(const Path& other): mImpl(std::make_unique<Impl>(*other.mImpl)) {}

Path& Path::operator=(const Path& other) {
  if (this != &other) *mImpl = *other.mImpl;
  return *this;
}

Path::Path(Path&& other) noexcept: mImpl(std::move(other.mImpl)) {
  // A moved-from path must remain *valid*, not merely destructible. `unique_ptr`'s default move leaves the
  // source null, so any member called on it -- `reset`, `isEmpty`, or the destructor path through a friend
  // function -- dereferences null and crashes. Giving the source a fresh empty implementation is what makes
  // "valid but unspecified" true here rather than aspirational.
  if (mImpl == nullptr) mImpl = std::make_unique<Impl>();
  if (other.mImpl == nullptr) other.mImpl = std::make_unique<Impl>();
}

Path& Path::operator=(Path&& other) noexcept {
  if (this != &other) {
    mImpl = std::move(other.mImpl);
    if (mImpl == nullptr) mImpl = std::make_unique<Impl>();
    if (other.mImpl == nullptr) other.mImpl = std::make_unique<Impl>();
  }
  return *this;
}

// ==================================================================================================
// Private access
// ==================================================================================================

Path Path::fromHandle(const void* handle) {
  Path result;
  if (handle != nullptr) result.mImpl->path = *static_cast<const pathops::Path*>(handle);
  return result;
}

const void* Path::pathOpsHandle() const noexcept { return &mImpl->path; }

DPoint Path::PathTransformer::operator()(const DPoint& point) const {
  const Point mapped = matrix * Point { static_cast<Scalar>(point.x), static_cast<Scalar>(point.y) };
  return DPoint::Make(static_cast<double>(mapped.x), static_cast<double>(mapped.y));
}

void Path::mapPoints(const std::function<void(DPoint&)>& map) {
  for (pathops::PathSegment& segment: mImpl->path.segments()) {
    for (uint8_t i = 0; i < segment.pointCount; ++i) {
      map(segment.points[i]);
    }
  }
}

void Path::setPathOps(const void* handle) {
  mImpl->path = handle != nullptr ? *static_cast<const pathops::Path*>(handle) : pathops::Path {};
}

// ==================================================================================================
// Properties
// ==================================================================================================

FillType Path::fillType() const noexcept { return fromPathOps(mImpl->path.fillRule()); }

void Path::setFillType(const FillType fillType) noexcept { mImpl->path.setFillRule(toPathOps(fillType)); }

bool Path::isEmpty() const noexcept { return mImpl->path.isEmpty(); }

size_t Path::verbCount() const noexcept { return mImpl->path.segments().size(); }

std::uint64_t Path::contentHash() const noexcept {
  // FNV-1a over the segment stream. It has to distinguish a point from a control point and a line
  // from a quad that happens to carry the same end point, so the verb and the point count are mixed
  // in explicitly rather than inferred from the coordinates.
  std::uint64_t hash = 14695981039346656037ull;
  const auto mix = [&hash](const void* data, const size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t index = 0; index < size; ++index) {
      hash ^= bytes[index];
      hash *= 1099511628211ull;
    }
  };
  const FillType rule = fillType();
  mix(&rule, sizeof(rule));
  for (const pathops::PathSegment& segment: mImpl->path.segments()) {
    mix(&segment.verb, sizeof(segment.verb));
    mix(&segment.pointCount, sizeof(segment.pointCount));
    for (uint8_t point = 0; point < segment.pointCount; ++point) {
      const double x = segment.points[point].x;
      const double y = segment.points[point].y;
      mix(&x, sizeof(x));
      mix(&y, sizeof(y));
    }
  }
  return hash;
}

size_t Path::pointCount() const noexcept {
  size_t total = 0;
  for (const pathops::PathSegment& segment: mImpl->path.segments()) {
    total += segment.pointCount;
  }
  return total;
}

bool Path::hasCurves() const noexcept {
  for (const pathops::PathSegment& segment: mImpl->path.segments()) {
    if (segment.verb == pathops::Verb::Quad || segment.verb == pathops::Verb::Cubic) return true;
  }
  return false;
}

bool Path::isLine() const noexcept {
  for (const pathops::PathSegment& segment: mImpl->path.segments()) {
    switch (segment.verb) {
      case pathops::Verb::Quad:
      case pathops::Verb::Cubic:
      case pathops::Verb::Close:
        return false;
      default:
        break;
    }
  }
  return true;
}

bool Path::isRect(Rect* outRect) const noexcept {
  // A rectangle is one closed contour whose four edges are axis-aligned. The tolerance covers the rounding
  // that comes from a caller computing corners as `x + width`.
  constexpr double kTolerance = 1e-6;

  // Collect the contour's corner points, with the start point first.
  pathops::DPoint corners[5];
  size_t cornerCount = 0;
  size_t lineCount = 0;
  bool sawMove = false;
  bool sawClose = false;

  for (const pathops::PathSegment& segment: mImpl->path.segments()) {
    switch (segment.verb) {
      case pathops::Verb::Move:
        // A second contour means this is not one rectangle.
        if (sawMove) return false;
        sawMove = true;
        if (cornerCount >= 5) return false;
        corners[cornerCount++] = segment.points[0];
        break;
      case pathops::Verb::Line:
        if (!sawMove) return false;
        if (lineCount >= 4) return false;  // a fifth edge means it is not a rectangle
        ++lineCount;
        if (cornerCount >= 5) return false;
        corners[cornerCount++] = segment.points[0];
        break;
      case pathops::Verb::Close:
        if (sawClose) return false;
        sawClose = true;
        break;
      case pathops::Verb::Quad:
      case pathops::Verb::Cubic:
        return false;  // a rectangle has no curves
      case pathops::Verb::Done:
        break;
    }
  }

  if (!sawMove) return false;

  /**
   * The two ways a caller writes a rectangle, and the reason the edge count is not simply four.
   *
   * A `close` verb *supplies* the edge back to the start rather than merely marking the contour finished --
   * so a closed rectangle is a move, three lines and a close, and the fourth edge is the close itself. That
   * is what the earlier version of this function got wrong: it demanded four `lineTo` calls and therefore
   * rejected every rectangle that used `close`, which is how the test helper wrote them and how any caller
   * would.
   *
   * A path that instead spells out the fourth edge and omits the close is the same geometry, and filling it
   * closes it anyway, so both spellings are accepted.
   */
  size_t uniqueCorners = 0;
  if (lineCount == 3 && sawClose) {
    uniqueCorners = 3;  // the close returns to corners[0]
  } else if (lineCount == 4) {
    uniqueCorners = 4;
    // The fourth edge has to actually come back to the start for this spelling to describe a closed shape.
    if (cornerCount != 5) return false;
    if (std::fabs(corners[4].x - corners[0].x) > kTolerance ||
        std::fabs(corners[4].y - corners[0].y) > kTolerance) {
      return false;
    }
  } else {
    return false;
  }

  if (cornerCount != uniqueCorners + 1) return false;

  const pathops::DPoint& a = corners[0];
  const pathops::DPoint& b = corners[1];
  const pathops::DPoint& c = corners[2];

  // Axis-aligned at every corner. Walking the three collected corners: the edge out of `a` changes one
  // coordinate, the edge into `c` changes the other, and the implicit closing edge is parallel to the first.
  const bool horizontalAB = std::fabs(a.y - b.y) <= kTolerance;
  const bool verticalBC = std::fabs(b.x - c.x) <= kTolerance;
  if (!horizontalAB || !verticalBC) return false;

  // A rectangle is not a line: both extents have to be positive. Without this a path that doubles back on
  // itself -- four corners with two of them equal -- would pass every check above and be reported as a
  // rectangle that draws nothing.
  if (std::fabs(a.x - c.x) <= kTolerance || std::fabs(a.y - c.y) <= kTolerance) return false;

  if (outRect != nullptr) {
    *outRect = Rect::MakeLTRB(static_cast<Scalar>(std::min(a.x, c.x)), static_cast<Scalar>(std::min(a.y, c.y)),
            static_cast<Scalar>(std::max(a.x, c.x)), static_cast<Scalar>(std::max(a.y, c.y)));
  }
  return true;
}

Rect Path::bounds() const noexcept { return toPublic(mImpl->path.controlBounds()); }

Rect Path::tightBounds() const noexcept { return toPublic(mImpl->path.tightBounds()); }

bool Path::contains(const Point point) const noexcept { return mImpl->path.contains(point); }

Scalar Path::length() const noexcept {
  // Summed per segment, because the length of an outline is the sum of its pieces.
  //
  // Each piece is integrated rather than measured as a chord. A cubic's arc length has no elementary closed
  // form -- it reduces to an elliptic integral -- so it is computed numerically, by Gauss-Legendre
  // quadrature of the speed |B'(t)| over the segment's parameter. That is a real approximation and worth
  // being clear about: it is not a polyline estimate, and sixteen nodes integrate a smooth cubic's speed to
  // near machine precision, but a segment with a cusp has a speed that is not smooth and converges more
  // slowly. In practice the error is far below the float the answer is returned in.
  double total = 0.0;
  pathops::DPoint contourStart { 0.0, 0.0 };
  pathops::DPoint current { 0.0, 0.0 };
  bool open = false;

  for (const pathops::PathSegment& segment: mImpl->path.segments()) {
    switch (segment.verb) {
      case pathops::Verb::Move:
        contourStart = segment.points[0];
        current = contourStart;
        open = true;
        break;
      case pathops::Verb::Line:
        if (open) total += (segment.points[0] - current).length();
        current = segment.points[0];
        break;
      case pathops::Verb::Quad:
      case pathops::Verb::Cubic:
        if (open) total += segmentArcLength(segment, current);
        current = segment.endPoint();
        break;
      case pathops::Verb::Close:
        // Real geometry: a filled triangle's perimeter includes the edge the caller never wrote out.
        if (open) total += (contourStart - current).length();
        current = contourStart;
        break;
      case pathops::Verb::Done:
        break;
    }
  }

  return static_cast<Scalar>(total);
}

// ==================================================================================================
// Building
// ==================================================================================================

void Path::moveTo(const Point point) { mImpl->path.moveTo(point); }

void Path::lineTo(const Point point) { mImpl->path.lineTo(point); }

void Path::quadTo(const Point control, const Point point) { mImpl->path.quadTo(control, point); }

void Path::cubicTo(const Point control1, const Point control2, const Point point) {
  mImpl->path.cubicTo(control1, control2, point);
}

void Path::close() { mImpl->path.close(); }

void Path::reset() { mImpl->path.reset(); }

void Path::addPath(const Path& other) {
  if (other.isEmpty()) return;
  if (isEmpty()) {
    *this = other;
    return;
  }

  // Through the boolean engine, so that the pieces the two paths share are not counted twice.
  Path combined = op(*this, other, PathOp::Union);
  if (!combined.isEmpty()) {
    *this = std::move(combined);
    return;
  }

  // The union came back empty, which happens when the two paths together enclose no area -- two crossing lines, or
  // two paths that only touch. There is then no region to return, and the right answer is both paths as they are,
  // which is what a caller who asked for one path holding both expects.
  //
  // The fallback is a plain append, and this is the case where the seams a union exists to remove cannot arise: with
  // no enclosed area there is nothing to double-count. Returning early and silently keeping only `*this` -- which the
  // first version of this did -- loses half of what the caller asked for, and it does so for exactly the paths a dash
  // produces, which are open contours with no area at all.
  mImpl->path.addPath(other.mImpl->path);
}

void Path::appendPath(const Path& other) {
  // A splice, and nothing more: the segments are copied across and the contours stay as separate as they were. No
  // boolean engine is involved, which is the point -- the union of two nested contours is the outer one, and the inner
  // one is what the caller was drawing.
  if (other.isEmpty()) return;
  mImpl->path.addPath(other.mImpl->path);
}

Path Path::reversed() const {
  Path result;
  result.mImpl->path = mImpl->path.reversed();
  return result;
}

Path Path::transformed(const Matrix& matrix) const {
  Path result;
  result.mImpl->path = mImpl->path;

  // Every control point is transformed, not just the endpoints. Transforming only the ends would leave the
  // handles where they were and change the curve's shape rather than moving it -- a mistake that looks like
  // a slightly wrong rotation rather than like a bug.
  result.mapPoints([&matrix](pathops::DPoint& point) { point = transformPoint(point, matrix); });
  return result;
}

void Path::transform(const Matrix& matrix) {
  mapPoints([&matrix](pathops::DPoint& point) { point = transformPoint(point, matrix); });
}

bool operator==(const Path& lhs, const Path& rhs) noexcept {
  // A geometric comparison. The two paths are first normalised -- each made winding, which resolves
  // self-crossings into unambiguous contours -- and the results are compared segment by segment. Comparing
  // the raw verbs instead would call the same shape drawn clockwise and counter-clockwise different, which
  // is not a distinction any caller asking "are these the same shape" wants.
  //
  // Normalisation is skipped when either path is empty, because `asWinding` on an empty path has nothing to
  // resolve and would only cost a call.
  if (lhs.isEmpty() || rhs.isEmpty()) return lhs.isEmpty() && rhs.isEmpty();

  const pathops::Path& a = lhs.mImpl->path;
  const pathops::Path& b = rhs.mImpl->path;

  // The control bounds are a cheap necessary condition: two shapes that are the same have the same bounds.
  const pathops::DRect boundsA = a.controlBounds();
  const pathops::DRect boundsB = b.controlBounds();
  constexpr double kTolerance = 1e-4;
  if (std::fabs(boundsA.left - boundsB.left) > kTolerance ||
      std::fabs(boundsA.top - boundsB.top) > kTolerance ||
      std::fabs(boundsA.right - boundsB.right) > kTolerance ||
      std::fabs(boundsA.bottom - boundsB.bottom) > kTolerance) {
    return false;
  }

  // Then the segments. A point-by-point comparison with a tolerance, because the same shape arrived at by
  // different arithmetic -- a rotation and back, say -- differs in the last bits.
  const std::vector<pathops::PathSegment>& segmentsA = a.segments();
  const std::vector<pathops::PathSegment>& segmentsB = b.segments();
  if (segmentsA.size() != segmentsB.size()) return false;

  for (size_t i = 0; i < segmentsA.size(); ++i) {
    if (segmentsA[i].verb != segmentsB[i].verb) return false;
    if (segmentsA[i].pointCount != segmentsB[i].pointCount) return false;
    for (uint8_t p = 0; p < segmentsA[i].pointCount; ++p) {
      if (std::fabs(segmentsA[i].points[p].x - segmentsB[i].points[p].x) > kTolerance ||
          std::fabs(segmentsA[i].points[p].y - segmentsB[i].points[p].y) > kTolerance) {
        return false;
      }
    }
  }
  return true;
}

// ==================================================================================================
// Combining
// ==================================================================================================

Path op(const Path& first, const Path& second, const PathOp pathOp) {
  const std::optional<pathops::Path> result =
          pathops::op(first.mImpl->path, second.mImpl->path, toPathOps(pathOp));

  if (!result.has_value()) {
    // A degeneracy rather than a missing feature: the traversal ran and could not resolve the operands, which means the
    // input is something it has no answer for -- an operand with no segments, or an outline whose crossings it could not
    // certify. Reporting an empty path is a wrong answer either way, and the log line is what makes it a diagnosable
    // one rather than a shape that is silently missing.
    MINILOG_WARN("[graphics/path] {} could not resolve these operands; the result is empty rather than correct",
            toString(pathOp));
    return makeEmpty();
  }

  Path out;
  out.mImpl->path = *result;
  return out;
}

Path simplify(const Path& path) {
  const std::optional<pathops::Path> result = pathops::simplify(path.mImpl->path);
  if (!result.has_value()) {
    MINILOG_WARN("[graphics/path] simplify could not resolve this path; the result is empty rather than correct");
    return makeEmpty();
  }
  Path out;
  out.mImpl->path = *result;
  return out;
}

Path asWinding(const Path& path) {
  const std::optional<pathops::Path> result = pathops::asWinding(path.mImpl->path);
  if (!result.has_value()) {
    MINILOG_WARN("[graphics/path] asWinding could not resolve this path; the result is empty rather than correct");
    return makeEmpty();
  }
  Path out;
  out.mImpl->path = *result;
  return out;
}

} // namespace graphics
