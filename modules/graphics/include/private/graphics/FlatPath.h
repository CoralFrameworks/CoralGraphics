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
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the specific
 * language governing permissions and limitations under the License.
 */

#ifndef CORAL_GRAPHICS_FLAT_PATH_H
#define CORAL_GRAPHICS_FLAT_PATH_H

#include "Flatten.h"

#include <geometry/Point.h>

#include <cstddef>
#include <vector>

/**
 * @file FlatPath.h
 *
 * A path reduced to polylines, with the cumulative length along each of them.
 *
 * ## Why this exists separately from Flatten.h
 *
 * `Flatten.h` turns curves into line segments, which is all a *rasterizer* needs: it walks the edges and never
 * cares how long they are. Measuring a path needs one thing more -- the arc length from the contour's start to
 * each vertex -- and computing that in the same pass as the flattening avoids walking the polylines twice.
 *
 * The distinction matters because the two callers are in different directories (`src/raster` and `src/measure`)
 * and this header is the seam between them, which is why it lives in `include/private` rather than beside either.
 *
 * ## Length is exact where the geometry is
 *
 * A line segment's length is its Euclidean distance, exactly. A curve's is approximated by the polyline that
 * represents it, so the accuracy of every measurement here is the accuracy of the flattening -- which is a
 * quarter of a device pixel by construction, because that is the tolerance `Flatten.h` uses. Stating that here
 * matters: a caller measuring a path to place something needs to know the number is good to a fraction of a
 * pixel and not to the last bit of a float.
 */

namespace graphics::measure {

/** One contour, flattened, with the cumulative length at each vertex. */
struct FlatContour {
  /** The vertices. A closed contour has no repeated final point; the closing edge is implicit. */
  std::vector<Point> points;

  /**
   * `cumulative[i]` is the arc length from the contour's first point to `points[i]`.
   *
   * The same length as @c points, and non-decreasing. Kept alongside rather than derived so that locating a
   * point at a given distance is a binary search rather than a re-walk.
   */
  std::vector<Scalar> cumulative;

  /** True when the closing edge from the last point back to the first is part of the path. */
  bool closed = false;

  [[nodiscard]] bool isEmpty() const noexcept { return points.size() < 2; }

  /**
   * @return the total arc length of the contour, *including* the closing edge when @c closed.
   *
   * The distinction is not cosmetic and it is the thing callers get wrong: a triangle traced as three points
   * has a perimeter of three edges only if the contour is closed. Reporting the open length for a closed shape
   * makes a measurement along it stop short by exactly one edge.
   */
  [[nodiscard]] Scalar length() const noexcept;
};

/**
 * @return @p path flattened into contours, each with its cumulative lengths.
 *
 * Contours of fewer than two points are dropped: they have no length to report and would only add empty
 * entries for a caller to skip.
 *
 * @param tolerance the maximum deviation of the polyline from the curve, in the same units as the path. The
 *                  caller should pass a value derived from its own scale -- a quarter of an output pixel --
 *                  rather than accepting a default, because the right value depends on how the measurement
 *                  will be used.
 */
[[nodiscard]] std::vector<FlatContour> flattenWithLengths(const pathops::Path& path, Scalar tolerance);

} // namespace graphics::measure

#endif // CORAL_GRAPHICS_FLAT_PATH_H
