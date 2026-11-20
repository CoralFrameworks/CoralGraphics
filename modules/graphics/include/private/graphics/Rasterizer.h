/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and limitations under the
 * License.
 */

#ifndef CORAL_GRAPHICS_RASTERIZER_H
#define CORAL_GRAPHICS_RASTERIZER_H

#include "Flatten.h"

#include <graphics/Enums.h>
#include <graphics/Pixmap.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <utility>
#include <vector>

/**
 * @file Rasterizer.h
 *
 * Filling closed contours into pixels.
 *
 * ## The algorithm, and why this one
 *
 * A scanline rasterizer: for each row of the destination, find where the contours' edges cross that row,
 * sort the crossings by x, and sweep left to right keeping a running winding number. Where the winding says
 * "inside", the pixels are covered. That is the classic approach and it is O(edges) per row rather than
 * O(pixels * edges), which matters because a thick stroke can easily have thousands of edges.
 *
 * Anti-aliasing is by supersampling in y and exact coverage in x: each output row is sampled at
 * `kSamplesPerPixel` sub-rows, and within a sub-row the partial coverage of the two edge pixels comes from
 * the edge's exact x position. The alternative -- supersampling in both axes -- needs `k^2` samples and a
 * full-size buffer; this gets the same visible quality on the edges that matter (near-horizontal edges, where
 * y-supersampling alone would band) for a quarter of the work.
 *
 * ## What it does not do
 *
 * There is no clipping to a rectangle here: contours are expected to be device-space and already clipped by
 * the caller if they need it. The rasterizer bounds its own loops by the destination, so geometry outside the
 * canvas is skipped rather than drawn, which is the only clipping it needs to be correct.
 */

namespace graphics::raster {

/**
 * Sub-rows sampled per output row.
 *
 * Four gives seventeen coverage levels, which is past the point where a flat edge looks stepped. Eight would
 * be smoother on a nearly-horizontal edge and would double the cost of every fill, which is a bad trade for a
 * first implementation -- and it is a compile-time constant precisely so that the trade is visible rather than
 * buried.
 */
constexpr int kSamplesPerPixel = 4;

/**
 * Fills @p contours into @p destination, calling @p blend for each pixel the shape touches.
 *
 * @param blend invoked as `blend(x, y, coverage)` with a coverage in 0-255. Coverage is reported even for the
 *              fully covered interior, so the caller does not have to special-case it -- and the blend it
 *              chooses already short-circuits on a zero coverage, which is the common case at the edges.
 *
 * @param fillRule which rule decides inside. `Winding` counts edge directions and so keeps a doubly-wound
 *                 region filled; `EvenOdd` counts crossings and so empties it. The two agree everywhere
 *                 except where contours overlap, which is the whole reason the path carries the rule.
 *
 * Overlapping contours are handled by the winding number rather than by compositing them separately, so a
 * shape drawn as two overlapping subpaths gets a single flat fill rather than a darker seam where they meet.
 * That is what the nonzero rule means and it is what a caller drawing a letter with a counter relies on.
 */
template<typename Blend>
void fillContours(const std::vector<Contour>& contours, Pixmap& destination, FillType fillRule, Blend&& blend) {
  fillContours(contours, destination.width(), destination.height(), fillRule, 0, destination.height() - 1,
          std::forward<Blend>(blend));
}

template<typename Blend>
void fillContours(const std::vector<Contour>& contours, const int width, const int height, FillType fillRule,
        const int requestedFirstRow, const int requestedLastRow, Blend&& blend);

/**
 * The two buffers one fill needs, which a caller that fills many shapes should own and reuse.
 *
 * ## Why this is not an implementation detail
 *
 * A fill walks the destination's rows and, for each of the four sub-samples of each row, collects the edges crossing that
 * line and accumulates coverage along the row. Those are two vectors, and both are sized by the *shape* rather than by a
 * constant -- so both used to be constructed inside the fill and destroyed at its end. For one shape that is nothing. For
 * the GPU coverage route it is the hot loop: the demo fills 41084 fields a frame, almost every one of them as a single
 * band, so that was **eighty thousand allocations a frame**, all of them inside the parallel region and therefore all of
 * them contending for the one global allocator. The route's fill stage measured 40 ms serially and 29 ms with a pool --
 * *flat* from two worker threads to eight, which is what a shared serial resource looks like and what sent 4.1.19 looking
 * here.
 *
 * The reuse is also why the struct is public rather than hidden: the caller keeps one per worker (a task in
 * `GpuRenderer::resolveCoverageFields` declares it beside the contour buffer it already reuses), so the storage is
 * per-worker without a lock, a thread-local, or an allocation per call.
 */
struct FillScratch {
  /** One crossing of a scanline by an edge, with the edge's direction, which is what the winding rule needs. */
  struct Crossing {
    Scalar x;
    int direction; // +1 downward, -1 upward
  };

  /** The crossings of the row being sampled; cleared per sub-sample, so its capacity is kept across the whole fill. */
  std::vector<Crossing> crossings;
  /** The coverage accumulated along the row being filled, indexed by column relative to the first covered column. */
  std::vector<uint32_t> rowCoverage;
};

/**
 * Fills only rows @p firstRow through @p lastRow (inclusive) of a `width x height` destination.
 *
 * ## Why a row range is a safe thing to expose
 *
 * This rasteriser carries **nothing** from one row to the next: each row builds its own crossings from the contours at
 * its own sub-sample heights, accumulates them into its own row buffer, and calls `blend` for that row alone. There is no
 * edge table to keep sorted, no active-edge list to advance, and no winding that a row inherits from the row above -- the
 * winding is recomputed *along* the row from its crossings. So two disjoint row ranges of the same shape are independent
 * by construction, which is what lets a large field's scanline work be split across threads: nothing has to be merged, and
 * the ranges cannot disagree about a row they do not share.
 *
 * The destination is `width x height` rather than a `Pixmap` for the same reason a band needs nothing else: the pixmap's
 * *dimensions* are the only thing this function ever reads from it (every pixel goes back through @p blend), so a caller
 * that fills bands into one buffer needs no destination object at all -- and allocating a megabyte of bitmap per band to
 * read two integers off it would be the kind of cost that hides behind a profiler.
 *
 * The requested range is intersected with the shape's own bounds and with the destination, so a band that misses the
 * shape entirely -- a band above its top edge, or a range a caller guessed wrong -- simply does nothing.
 */
template<typename Blend>
void fillContours(const std::vector<Contour>& contours, const int width, const int height, FillType fillRule,
        const int requestedFirstRow, const int requestedLastRow, Blend&& blend) {
  // The scratch is the caller's if the caller has one; a caller that does not gets one built here, which is a couple of
  // allocations per call -- see `FillScratch` for why that matters at forty thousand fills a frame and not at one.
  FillScratch owned;
  fillContours(contours, width, height, fillRule, requestedFirstRow, requestedLastRow, owned,
          std::forward<Blend>(blend));
}

/**
 * The same fill, with the scratch supplied by the caller: **no allocation at all**.
 *
 * `rowCoverage` is resized to the columns this call actually covers, and `crossings` only ever grows -- a shape with more
 * edges than the last one reallocates, which is a geometric term rather than a per-call one.
 */
template<typename Blend>
void fillContours(const std::vector<Contour>& contours, const int width, const int height, FillType fillRule,
        const int requestedFirstRow, const int requestedLastRow, FillScratch& scratch, Blend&& blend) {
  if (contours.empty()) return;
  if (width <= 0 || height <= 0) return;

  // The rows and columns actually touched, clamped to the destination. Geometry off-canvas is skipped by
  // narrowing the loops rather than by testing every pixel, which keeps a small shape on a large canvas cheap.
  Rect shapeBounds = contours.front().bounds();
  for (const Contour& contour: contours) {
    const Rect bounds = contour.bounds();
    shapeBounds = Rect::MakeLTRB(std::min(shapeBounds.getLeft(), bounds.getLeft()),
      std::min(shapeBounds.getTop(), bounds.getTop()),
      std::max(shapeBounds.getRight(), bounds.getRight()),
      std::max(shapeBounds.getBottom(), bounds.getBottom()));
  }

  const int firstRow = std::max(std::max(0, requestedFirstRow), static_cast<int>(std::floor(shapeBounds.getTop())));
  const int lastRow = std::min(std::min(height - 1, requestedLastRow),
          static_cast<int>(std::ceil(shapeBounds.getBottom())));
  if (firstRow > lastRow) return;

  const int firstColumn = std::max(0, static_cast<int>(std::floor(shapeBounds.getLeft())));
  const int lastColumn = std::min(width - 1, static_cast<int>(std::ceil(shapeBounds.getRight())));
  if (firstColumn > lastColumn) return;

  std::vector<FillScratch::Crossing>& crossings = scratch.crossings;
  std::vector<uint32_t>& rowCoverage = scratch.rowCoverage;
  rowCoverage.resize(static_cast<size_t>(lastColumn - firstColumn + 1));

  // ------------------------------------------------------------------------------------------------
  // The coordinate convention, stated once because everything below depends on it.
  //
  // A pixel with index (x, y) covers the unit square [x, x+1) x [y, y+1) in device space, and its centre is at
  // (x + 0.5, y + 0.5). The two axes are therefore sampled differently and that is not a mistake:
  //
  //   * In x, an edge crossing is a device-space x, which is already an edge coordinate. A crossing at x = 4 is
  //     the boundary between columns 3 and 4.
  //   * In y, a scanline is a *row of pixels*, not a boundary, so the section is sampled at a pixel's vertical
  //     centre, `y + 0.5`. Sampling at an integer y would put the sample exactly where a horizontal edge is,
  //     and a horizontal edge produces no crossing at all -- so a shape's flat top and bottom would be sampled
  //     inconsistently from its sides.
  //
  // Coverage is then measured along x in edge coordinates, which is the same space the crossings come back in.
  // Mixing the two -- measuring coverage from `x - 0.5`, say -- shifts every span half a pixel and drops the
  // partly covered pixel at a fractional edge, which is the defect this note exists to prevent.
  // ------------------------------------------------------------------------------------------------

  for (int row = firstRow; row <= lastRow; ++row) {
    std::fill(rowCoverage.begin(), rowCoverage.end(), 0u);

    for (int sample = 0; sample < kSamplesPerPixel; ++sample) {
      // Sampled at the *centres* of the sub-rows. Sampling at their boundaries would put a sample exactly on the
      // integer y where a horizontal edge can be, and a horizontal edge produces no crossing at all -- so a
      // shape's flat top and bottom would be sampled inconsistently from its sides.
      const Scalar sampleY = static_cast<Scalar>(row) +
                             (static_cast<Scalar>(sample) + 0.5f) / static_cast<Scalar>(kSamplesPerPixel);
      crossings.clear();

      for (const Contour& contour: contours) {
        const std::vector<Point>& points = contour.points;
        if (points.size() < 3) continue;

        for (size_t i = 0; i < points.size(); ++i) {
          const Point& a = points[i];
          const Point& b = points[(i + 1) % points.size()];

          // Half-open in y: an edge counts when the sample is at or below its top and strictly above its bottom.
          // A vertex where two edges meet is therefore counted exactly once, which is what stops a shape's
          // outline from being double-counted at a corner and inverting the winding parity.
          const Scalar top = std::min(a.y, b.y);
          const Scalar bottom = std::max(a.y, b.y);
          if (sampleY < top || sampleY >= bottom) continue;

          const Scalar t = (sampleY - a.y) / (b.y - a.y);
          const Scalar x = a.x + (b.x - a.x) * t;
          if (!std::isfinite(x)) continue;

          crossings.push_back(FillScratch::Crossing{ x, b.y > a.y ? 1 : -1 });
        }
      }

      if (crossings.empty()) continue;

      std::sort(crossings.begin(), crossings.end(),
        [](const FillScratch::Crossing& lhs, const FillScratch::Crossing& rhs) { return lhs.x < rhs.x; });

      int winding = 0;
      Scalar spanStart = 0.0f;

      for (const FillScratch::Crossing& crossing: crossings) {
        const bool wasInside = fillRule == FillType::Winding ? winding != 0 : (winding & 1) != 0;
        winding += crossing.direction;
        const bool isInside = fillRule == FillType::Winding ? winding != 0 : (winding & 1) != 0;

        if (!wasInside && isInside) {
          spanStart = crossing.x; // entering the shape
          continue;
        }
        if (!wasInside || isInside) continue;

        // Leaving the shape: cover from the span's start to here, clipped to the destination.
        //
        // Both are device-space x values, and a pixel with index x covers [x, x+1), so the span and the pixels
        // are directly comparable with no half-pixel adjustment anywhere.
        const Scalar start = std::max(spanStart, static_cast<Scalar>(firstColumn));
        const Scalar end = std::min(crossing.x, static_cast<Scalar>(lastColumn) + 1.0f);
        if (end <= start) continue;

        // The span [start, end) covers the pixels from `floor(start)` to `ceil(end) - 1`. The upper bound is
        // exclusive, so a rectangle from 4 to 12 covers columns 4 through 11 and nothing in column 12 -- which is
        // the property that catches a half-pixel bias, and the one an earlier `ceil(end - 0.5) - 1` got wrong by
        // discarding exactly the partly covered pixel at a fractional edge.
        const int spanFirst = std::max(firstColumn, static_cast<int>(std::floor(start)));
        const int spanLast = std::min(lastColumn, static_cast<int>(std::ceil(end)) - 1);

        for (int x = spanFirst; x <= spanLast; ++x) {
          const Scalar pixelLeft = static_cast<Scalar>(x);
          const Scalar pixelRight = static_cast<Scalar>(x) + 1.0f;
          const Scalar covered = std::min(end, pixelRight) - std::max(start, pixelLeft);
          if (covered <= 0.0f) continue;

          // Fractions of a sample scaled to 0-255, so a full sample adds 255 and the final divide by the sample
          // count turns the sum into a coverage. Integer accumulation with a rounded add is deterministic, which
          // a float accumulator is not across platforms.
          const uint32_t amount = static_cast<uint32_t>(std::min(covered, 1.0f) * 255.0f + 0.5f);
          rowCoverage[static_cast<size_t>(x - firstColumn)] += amount;
        }
      }
    }

    const uint32_t sampleCount = static_cast<uint32_t>(kSamplesPerPixel) * 255u;
    for (int x = firstColumn; x <= lastColumn; ++x) {
      const uint32_t accumulated = rowCoverage[static_cast<size_t>(x - firstColumn)];
      if (accumulated == 0) continue;

      // Rounded to the nearest 0-255 coverage. Scaling by 255 rather than shifting by 8 and dropping the
      // remainder keeps a fully covered interior at exactly 255 instead of 254, which would leave every fill a
      // hair translucent.
      const uint32_t coverage = std::min(255u, (accumulated * 255u + sampleCount / 2) / sampleCount);
      if (coverage == 0) continue;

      blend(x, row, coverage);
    }
  }
}

} // namespace graphics::raster

#endif // CORAL_GRAPHICS_RASTERIZER_H

