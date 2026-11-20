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

#include <graphics/PathMeasure.h>

#include "pathops/Curve.h"

#include <private/graphics/FlatPath.h>

#include <minilog/log.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace graphics {

namespace {

/** @return the distance between two points. */
Scalar distance(const Point& a, const Point& b) noexcept {
  const Scalar dx = b.x - a.x;
  const Scalar dy = b.y - a.y;
  return std::sqrt(dx * dx + dy * dy);
}

} // namespace anonymous

/**
 * The flattened path, and which contour is being walked.
 *
 * The flattening happens once, in the constructor, and every query afterwards is a search over the result. That is
 * the right division for how this is used: a caller builds one measure and then asks it many times as an animation
 * advances, so the expensive part should not be repeated.
 */
struct PathMeasure::Impl {
  /** The path, flattened once, with cumulative lengths per contour. */
  std::vector<measure::FlatContour> contours;

  /** The distance along the whole path at which each contour starts. Same length as @c contours, plus one entry. */
  std::vector<Scalar> contourStarts;

  /** Which contour is being walked. */
  size_t index = 0;

  /** @return the contour being walked, or null when there is none. */
  [[nodiscard]] const measure::FlatContour* current() const noexcept {
    return index < contours.size() ? &contours[index] : nullptr;
  }
};

PathMeasure::PathMeasure() : mImpl(std::make_unique<Impl>()) {}

PathMeasure::PathMeasure(const Path& path, const Scalar tolerance) : mImpl(std::make_unique<Impl>()) {
  const auto* pathOps = static_cast<const pathops::Path*>(path.pathOpsHandle());
  if (pathOps == nullptr) return;

  mImpl->contours = measure::flattenWithLengths(*pathOps, tolerance);

  // The cumulative starts, with one extra entry so that a contour's end and the next one's start are both
  // available without a special case for the last.
  mImpl->contourStarts.reserve(mImpl->contours.size() + 1);
  Scalar running = 0.0f;
  for (const measure::FlatContour& contour: mImpl->contours) {
    mImpl->contourStarts.push_back(running);
    running += contour.length();
  }
  mImpl->contourStarts.push_back(running);
}

PathMeasure::~PathMeasure() = default;

PathMeasure::PathMeasure(const PathMeasure& other) : mImpl(std::make_unique<Impl>(*other.mImpl)) {}

PathMeasure& PathMeasure::operator=(const PathMeasure& other) {
  if (this != &other) *mImpl = *other.mImpl;
  return *this;
}

PathMeasure::PathMeasure(PathMeasure&& other) noexcept: mImpl(std::move(other.mImpl)) {
  // A moved-from object stays valid rather than merely destructible, for the same reason `Path` does: `unique_ptr`
  // leaves the source null and any member called on it would dereference null.
  if (mImpl == nullptr) mImpl = std::make_unique<Impl>();
  if (other.mImpl == nullptr) other.mImpl = std::make_unique<Impl>();
}

PathMeasure& PathMeasure::operator=(PathMeasure&& other) noexcept {
  if (this != &other) {
    mImpl = std::move(other.mImpl);
    if (mImpl == nullptr) mImpl = std::make_unique<Impl>();
    if (other.mImpl == nullptr) other.mImpl = std::make_unique<Impl>();
  }
  return *this;
}

std::unique_ptr<PathMeasure> PathMeasure::create(const Path& path, const Scalar tolerance) {
  if (path.isEmpty()) {
    MINILOG_WARN("[graphics/measure] refusing to measure an empty path");
    return nullptr;
  }

  // `new` rather than `make_unique` because the constructor is private, which is what lets `create` be the only
  // way to build one and therefore the only place that has to decide when there is nothing to measure.
  std::unique_ptr<PathMeasure> measure(new PathMeasure(path, tolerance));

  Scalar total = 0.0f;
  for (const measure::FlatContour& contour: measure->mImpl->contours) {
    total += contour.length();
  }
  if (!(total > 0.0f)) {
    // A path of nothing but `moveTo` verbs flattens to contours with no edges. There is genuinely nothing to walk,
    // and a measure whose every query returns zero would hide the caller's mistake instead of reporting it.
    MINILOG_WARN("[graphics/measure] refusing to measure a path with no length");
    return nullptr;
  }

  return measure;
}

Scalar PathMeasure::length() const noexcept {
  const measure::FlatContour* contour = mImpl->current();
  return contour != nullptr ? contour->length() : 0.0f;
}

Scalar PathMeasure::totalLength() const noexcept {
  return mImpl->contourStarts.empty() ? 0.0f : mImpl->contourStarts.back();
}

bool PathMeasure::getPosition(const Scalar distanceAlong, Point* position, Point* tangent) const noexcept {
  if (position == nullptr) return false;

  const measure::FlatContour* contour = mImpl->current();
  if (contour == nullptr || contour->points.size() < 2) return false;

  // Not clamped. A distance past the end is a failure rather than the end point, because a caller laying out a
  // dash pattern wants it to stop rather than pile up at the last vertex -- and a caller that meant "the end" can
  // ask for `length()` exactly.
  const Scalar total = contour->length();
  if (distanceAlong < 0.0f || distanceAlong > total) return false;

  // Find the edge the distance falls on. A binary search over the cumulative lengths, which is why they are stored
  // rather than recomputed: a caller advancing along a path asks this once per frame and a linear scan would make
  // the cost of a long path quadratic in the number of frames.
  const auto begin = contour->cumulative.begin();
  const auto end = contour->cumulative.end();
  const auto found = std::upper_bound(begin, end, distanceAlong);
  size_t edge = static_cast<size_t>(std::distance(begin, found));

  // `upper_bound` gives the first entry strictly greater than the distance. A distance of exactly zero lands on
  // index 0, which is the first vertex, and there is no preceding edge to interpolate along.
  if (edge == 0) {
    *position = contour->points.front();
    if (tangent != nullptr) {
      const Point direction = contour->points[1] - contour->points[0];
      const Scalar magnitude = std::sqrt(direction.x * direction.x + direction.y * direction.y);
      *tangent = magnitude > 0.0f ? Point { direction.x / magnitude, direction.y / magnitude } : Point { 0.0f, 0.0f };
    }
    return true;
  }

  // `edge` is one past the vertex that starts the segment, so the segment runs from `edge - 1` to `edge`.
  const size_t lastVertex = contour->points.size();
  if (edge >= lastVertex) edge = lastVertex - 1;
  if (edge == 0) {
    *position = contour->points.front();
    return true;
  }

  const Point& a = contour->points[edge - 1];
  const Point& b = contour->points[edge];
  const Scalar segmentStart = contour->cumulative[edge - 1];
  const Scalar segmentLength = contour->cumulative[edge] - segmentStart;

  // The fraction along this one segment. A zero-length segment -- two coincident vertices, which a cusp produces --
  // has no fraction to compute, so the point is the vertex itself.
  const Scalar fraction = segmentLength > 0.0f ? (distanceAlong - segmentStart) / segmentLength : 0.0f;

  position->x = a.x + (b.x - a.x) * fraction;
  position->y = a.y + (b.y - a.y) * fraction;

  if (tangent != nullptr) {
    const Point direction = b - a;
    const Scalar magnitude = std::sqrt(direction.x * direction.x + direction.y * direction.y);
    // A cusp has no single direction, reported as zero rather than as an arbitrary one of the two it meets at.
    *tangent = magnitude > 0.0f ? Point { direction.x / magnitude, direction.y / magnitude } : Point { 0.0f, 0.0f };
  }
  return true;
}

bool PathMeasure::nextContour() noexcept {
  if (mImpl->index + 1 >= mImpl->contours.size()) {
    // Left on the last contour rather than moved past it, so a caller that ignores the answer still has a usable
    // measure.
    return false;
  }
  ++mImpl->index;
  return true;
}

size_t PathMeasure::contourIndex() const noexcept { return mImpl->index; }

size_t PathMeasure::contourCount() const noexcept { return mImpl->contours.size(); }

bool PathMeasure::isClosed() const noexcept {
  const measure::FlatContour* contour = mImpl->current();
  return contour != nullptr && contour->closed;
}

void PathMeasure::getSegmentRange(Scalar* start, Scalar* end) const noexcept {
  // The array has one more entry than there are contours, so `index` and `index + 1` are both always in range.
  if (mImpl->contourStarts.size() < 2) {
    if (start != nullptr) *start = 0.0f;
    if (end != nullptr) *end = 0.0f;
    return;
  }

  const size_t index = std::min(mImpl->index, mImpl->contours.size() - 1);
  if (start != nullptr) *start = mImpl->contourStarts[index];
  if (end != nullptr) *end = mImpl->contourStarts[index + 1];
}

bool PathMeasure::getSegment(const Scalar start, const Scalar stop, Path* const out, const bool moveTo) const {
  if (out == nullptr) return false;

  const measure::FlatContour* contour = mImpl->current();
  if (contour == nullptr || contour->isEmpty()) return false;

  const Scalar total = contour->length();
  const Scalar low = std::clamp(std::min(start, stop), 0.0f, total);
  const Scalar high = std::clamp(std::max(start, stop), 0.0f, total);
  if (!(high > low)) return false;

  const size_t vertexCount = contour->points.size();
  // A closed contour has one more edge than an open one: the last edge returns to the first vertex. The flattening
  // never repeats that vertex, so the edge has to be counted rather than looked for.
  const size_t edgeCount = contour->closed ? vertexCount : vertexCount - 1;

  // `vertexAt` wraps, which is what makes the closing edge the same expression as every other edge.
  const auto vertexAt = [contour, vertexCount](const size_t index) { return contour->points[index % vertexCount]; };

  // Where along the contour a distance falls: which edge, and how far along it. Mirrors `getPosition`, including its
  // binary search over the cumulative lengths, and extends it past the last vertex onto the closing edge.
  struct Located {
    size_t edge = 0;
    Scalar fraction = 0;
  };

  const auto locate = [&](const Scalar distance) -> Located {
    const auto found = std::upper_bound(contour->cumulative.begin(), contour->cumulative.end(), distance);
    const size_t index = static_cast<size_t>(std::distance(contour->cumulative.begin(), found));

    if (index == 0) return {0, 0.0f};

    if (index >= vertexCount) {
      // Past the last vertex. On an open contour that is the end of the last edge; on a closed one it is the closing
      // edge, which `cumulative` does not cover because it never repeats the first vertex.
      if (!contour->closed) return {edgeCount - 1, 1.0f};
      const Scalar from = contour->cumulative.back();
      const Scalar length = total - from;
      return {edgeCount - 1, length > 0.0f ? std::clamp((distance - from) / length, 0.0f, 1.0f) : 0.0f};
    }

    const size_t edge = index - 1;
    const Scalar from = contour->cumulative[edge];
    const Scalar length = contour->cumulative[index] - from;
    // A zero-length edge -- two coincident vertices, which a cusp produces -- has no fraction to compute.
    return {edge, length > 0.0f ? std::clamp((distance - from) / length, 0.0f, 1.0f) : 0.0f};
  };

  const auto pointOn = [&](const size_t edge, const Scalar fraction) {
    const Point a = vertexAt(edge);
    const Point b = vertexAt(edge + 1);
    return Point {a.x + (b.x - a.x) * fraction, a.y + (b.y - a.y) * fraction};
  };

  const Located first = locate(low);
  const Located last = locate(high);

  const Point head = pointOn(first.edge, first.fraction);
  if (moveTo) {
    out->moveTo(head);
  } else {
    // Continuing an existing contour: reaching the head is a segment like any other, and a `moveTo` here would break a
    // stroke at the wrap point of a closed shape.
    out->lineTo(head);
  }

  // Every whole vertex strictly between the two ends. `vertexAt(edge)` is the start of `edge` and therefore the end of
  // the edge before it, so one point per crossed edge is enough.
  for (size_t edge = first.edge + 1; edge <= last.edge && edge < edgeCount; ++edge) {
    out->lineTo(vertexAt(edge));
  }

  // The end, unless it is exactly a vertex the loop has already emitted.
  if (last.fraction > 0.0f) out->lineTo(pointOn(last.edge, last.fraction));
  return true;
}

Rect PathMeasure::getSegmentBounds(const Scalar start, const Scalar end) const noexcept {
  const measure::FlatContour* contour = mImpl->current();
  if (contour == nullptr || contour->points.size() < 2) return Rect::MakeLTRB(0.0f, 0.0f, 0.0f, 0.0f);

  const Scalar low = std::min(start, end);
  const Scalar high = std::max(start, end);
  const Scalar total = contour->length();
  if (low >= total || high <= 0.0f) return Rect::MakeLTRB(0.0f, 0.0f, 0.0f, 0.0f);

  // An empty range covers a single point, which has no area. Reporting the point's own coordinates would be a
  // rectangle of zero width at a non-zero position -- and a caller testing "is there anything to damage" would
  // have to compare both dimensions against zero rather than reading a rectangle with no extent. A rectangle at
  // the origin is what "nothing here" looks like throughout this layer.
  if (high - low <= 0.0f) return Rect::MakeLTRB(0.0f, 0.0f, 0.0f, 0.0f);

  // Walked rather than bounded from the vertices, because the travelled part of a curve is not a vertex range: the
  // start and end distances almost always fall part way along an edge, so the bounds have to include the
  // interpolated points at both ends.
  Scalar left = 0.0f;
  Scalar top = 0.0f;
  Scalar right = 0.0f;
  Scalar bottom = 0.0f;
  bool any = false;

  const auto include = [&left, &top, &right, &bottom, &any](const Point& point) {
    if (!any) {
      left = right = point.x;
      top = bottom = point.y;
      any = true;
      return;
    }
    left = std::min(left, point.x);
    top = std::min(top, point.y);
    right = std::max(right, point.x);
    bottom = std::max(bottom, point.y);
  };

  // The start and end points themselves, which are the two that a vertex walk would miss.
  Point position;
  if (getPosition(std::max(low, 0.0f), &position)) include(position);
  if (getPosition(std::min(high, total), &position)) include(position);

  // And every vertex in between.
  for (size_t i = 0; i < contour->points.size(); ++i) {
    const Scalar at = contour->cumulative[i];
    if (at >= low && at <= high) include(contour->points[i]);
  }

  if (!any) return Rect::MakeLTRB(0.0f, 0.0f, 0.0f, 0.0f);
  return Rect::MakeLTRB(left, top, right, bottom);
}

} // namespace graphics
