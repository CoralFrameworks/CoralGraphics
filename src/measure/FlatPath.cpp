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

#include <private/graphics/FlatPath.h>

#include <cmath>

namespace graphics::measure {

Scalar FlatContour::length() const noexcept {
  if (cumulative.empty()) return 0.0f;

  // The last cumulative entry is the distance to the final point along the polyline. A closed contour has one
  // more real edge -- the one back to the start -- which nothing has accumulated because the polyline does not
  // repeat its first point.
  Scalar total = cumulative.back();
  if (closed && points.size() >= 2) {
    const Point& last = points.back();
    const Point& first = points.front();
    total += std::sqrt((last.x - first.x) * (last.x - first.x) + (last.y - first.y) * (last.y - first.y));
  }
  return total;
}

std::vector<FlatContour> flattenWithLengths(const pathops::Path& path, const Scalar tolerance) {
  std::vector<FlatContour> result;

  raster::FlattenOptions options;
  // A tolerance of zero or less would make the flatness test never true, so the recursion would descend to its
  // depth limit and produce an enormous polyline. Falling back to the default is the useful answer: a caller
  // that passed zero did not mean "infinitely fine subdivision".
  options.tolerance = tolerance > 0.0f ? tolerance : raster::kFlattenTolerance;

  const std::vector<raster::Contour> flattened = raster::flattenToPolylines(path, options);

  result.reserve(flattened.size());
  for (const raster::Contour& contour: flattened) {
    if (!contour.hasLength()) continue;  // nothing to measure

    FlatContour flat;
    flat.points = contour.points;
    flat.closed = contour.closed;
    flat.cumulative.resize(contour.points.size(), 0.0f);

    Scalar running = 0.0f;
    for (size_t i = 1; i < contour.points.size(); ++i) {
      const Point& a = contour.points[i - 1];
      const Point& b = contour.points[i];
      running += std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
      flat.cumulative[i] = running;
    }

    result.push_back(std::move(flat));
  }

  return result;
}

} // namespace graphics::measure
