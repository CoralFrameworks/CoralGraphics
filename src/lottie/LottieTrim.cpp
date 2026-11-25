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
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

/**
 * @file LottieTrim.cpp
 *
 * The trim declared in LottieTrim.h, translated from Skottie's `TrimPaths.cpp` and Skia's `SkTrimPathEffect.cpp`.
 */

#include "lottie/LottieTrim.h"

#include <graphics/PathMeasure.h>

#include <algorithm>
#include <cmath>

namespace graphics::lottie {
namespace {

/**
 * Appends the part of @p source between two distances along its *whole* length, walking contour by contour.
 *
 * @return how many contours were iterated, which the caller needs: a single one means the two spans of an inverted trim
 *         are two halves of the same contour rather than two separate shapes.
 *
 * The distances are cumulative across contours, which is the whole reason this function exists -- a caller wanting "the
 * length from here" has one number for a path of several contours, and turning that into a per-contour distance means
 * knowing where each contour starts. A measure is built here rather than taken as a parameter because the walk leaves it
 * at the end, and an inverted trim walks twice.
 */
size_t addSegments(const Path& source, const Scalar start, const Scalar stop, Path* const out, const bool moveTo) {
  const auto measure = PathMeasure::create(source);
  if (measure == nullptr) return 0;

  Scalar currentOffset = 0;
  size_t contours = 1;

  do {
    const Scalar nextOffset = currentOffset + measure->length();

    // Only once the requested start has been reached. A span beginning before this contour's start passes a *negative*
    // distance, which the segment gets clamped, so the first contour of a multi-contour span starts at its own origin.
    if (start < nextOffset) {
      measure->getSegment(start - currentOffset, stop - currentOffset, out, moveTo);
      // Done as soon as the span has been covered. Without this the walk would keep visiting contours and appending
      // clamped segments to the end of the path, which would make a trim on a short span swallow everything after it.
      if (stop <= nextOffset) break;
    }

    contours++;
    currentOffset = nextOffset;
  } while (measure->nextContour());

  return contours;
}

} // namespace anonymous

Path trimPath(const Path& source, const Scalar startPercent, const Scalar endPercent, const Scalar offsetDegrees) {
  // ## What to keep
  //
  // The offset is in degrees, so a full turn is the whole path. That is the format's unit and Skottie marks it as odd in
  // its own source; dividing by 100 here instead would make every offset 3.6 times too large.
  const Scalar start = startPercent / 100.0f;
  const Scalar end = endPercent / 100.0f;
  const Scalar offset = offsetDegrees / 360.0f;

  // Taken as a range and then slid, so that dragging the start past the end does not reverse the trim -- the two ends
  // are interchangeable, which is what a user dragging a handle expects.
  Scalar startT = std::min(start, end) + offset;
  Scalar stopT = std::max(start, end) + offset;
  bool inverted = false;

  if (stopT - startT < 1.0f) {
    // Fewer than one whole path: wrapped into [0, 1) so that sliding past the end comes back to the beginning.
    startT -= std::floor(startT);
    stopT -= std::floor(stopT);

    if (startT > stopT) {
      // Wrapping carried the start past the end, which means the range crossed the path's own start. What is kept is
      // then everything *outside* the range -- the complement -- which is a different operation and a different mode.
      std::swap(startT, stopT);
      inverted = true;
    }
  } else {
    // One whole path or more: there is nothing to leave out.
    startT = 0;
    stopT = 1;
  }

  // Nothing to do. Returning the source rather than a copy is not an optimisation -- it preserves whatever the path
  // already was, curves included, instead of replacing it with the flattened version a walk would produce.
  if (startT <= 0.0f && stopT >= 1.0f && !inverted) return source;

  startT = std::clamp(startT, 0.0f, 1.0f);
  stopT = std::clamp(stopT, 0.0f, 1.0f);
  // An empty range keeps nothing. Reported as an empty path rather than as a failure, because a trim animated from zero
  // to full passes through this state on its way.
  if (startT >= stopT) return {};

  const auto measure = PathMeasure::create(source);
  if (measure == nullptr) return {};

  const Scalar length = measure->totalLength();
  const Scalar arcStart = length * startT;
  const Scalar arcStop = length * stopT;

  Path out;
  if (!inverted) {
    // One span.
    if (arcStart < arcStop) addSegments(source, arcStart, arcStop, &out, true);
    return out;
  }

  // ## The inverted range: one logical span that wraps, so two actual ones
  //
  // The tail is appended *first*. That is not an ordering detail: it is what makes the two pieces meet at the wrap
  // point, and the two spans are then [arcStop, end) followed by [0, arcStart).
  //
  // A single closed contour is not two shapes but one shape cut in two, so the second piece continues the first rather
  // than starting its own contour. A `moveTo` between them is a break in a stroke at exactly the point the user dragged
  // the trim handle to, which is the most visible place it could be.
  bool requiresMoveTo = true;
  if (arcStop < length) {
    const size_t contours = addSegments(source, arcStop, length, &out, true);
    if (contours == 1 && measure->contourCount() == 1 && measure->isClosed()) requiresMoveTo = false;
  }
  if (arcStart > 0.0f) addSegments(source, 0.0f, arcStart, &out, requiresMoveTo);
  return out;
}

} // namespace graphics::lottie
