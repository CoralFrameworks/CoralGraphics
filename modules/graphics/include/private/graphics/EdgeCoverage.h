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

#ifndef CORAL_GRAPHICS_EDGE_COVERAGE_H
#define CORAL_GRAPHICS_EDGE_COVERAGE_H

#include "Rasterizer.h"

#include <graphics/Bitmap.h>
#include <graphics/Enums.h>
#include <graphics/Pixmap.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

/**
 * @file EdgeCoverage.h
 *
 * **The single definition of what an antialiased edge is**, shared by the software rasterizer and the GPU's fragment
 * stage.
 *
 * ## Why this file exists
 *
 * The two rasterizers disagreed about edge coverage and nobody had written down what "coverage" meant, so each had its
 * own answer:
 *
 *   * `raster::fillContours` computes a **box area**: exact in x and four sub-samples in y (`Rasterizer.h` states the
 *     convention at length);
 *   * the GPU's path program computes **nothing** -- it gates on the stencil, so an edge is a step;
 *   * and the coverage-image route added to the recorder produces a third answer, because it resamples the rasterizer's
 *     box area through a bilinear filter when the geometry is not on the integer lattice.
 *
 * That is why "the GPU does not antialias" was true, why "make it antialias" then made `path.svg` *worse* (1.8% to 4.5%
 * of inked pixels over the channel budget), and why the same change improved one rotated square from 0 to 85 partly
 * covered pixels. Three answers, no definition.
 *
 * This header is the definition: a **signed distance to the nearest edge**, and a coverage derived from it by one
 * formula with one constant. Both sides are meant to converge on it, and the test beside it measures how far each
 * currently is.
 *
 * ## The convention, stated once
 *
 * A pixel with index (x, y) covers the unit square `[x, x+1) x [y, y+1)` and its **centre** is `(x + 0.5, y + 0.5)`.
 * Coverage is evaluated at the centre, so a point exactly on an edge is half covered, a point half a pixel inside is
 * fully covered, and a point half a pixel outside is empty. That is the same contract `quad.frag` uses for a rounded
 * rectangle, which is the only antialiased edge the GPU already had.
 *
 * ## What this does *not* claim
 *
 * It does not claim to equal the rasterizer's box area. For an axis-aligned edge at a fractional position the two agree
 * -- and the test asserts that, because it is the case that has to hold -- but for a 45-degree edge they differ, because
 * a radial distance is not a box integral. The test *prints* that difference rather than hiding it, and closing it is
 * the decision the plan in `路径边缘抗锯齿设计与现状.md` defers to step S1's measurement.
 */

namespace graphics::raster {

/**
 * How many device pixels a coverage transition spans.
 *
 * One, because a pixel is one pixel: a coverage of 0.5 at the edge and 0 or 1 half a pixel to either side is what
 * "antialiased" means at this resolution. Named rather than written as a literal because two implementations have to
 * agree on it, and because a caller that wants a softer edge (a blur, a shadow) has somewhere to say so.
 */
constexpr Scalar kEdgeFeather = 1.0f;

/** @return the shortest distance from @p point to the segment @p a-@p b, in the same units as the input. */
[[nodiscard]] inline Scalar distanceToSegment(const Point& point, const Point& a, const Point& b) noexcept {
  const Scalar dx = b.x - a.x;
  const Scalar dy = b.y - a.y;
  const Scalar lengthSquared = dx * dx + dy * dy;
  if (lengthSquared <= 0.0f) {
    const Scalar px = point.x - a.x;
    const Scalar py = point.y - a.y;
    return std::sqrt(px * px + py * py);
  }
  // The projection is clamped to the segment, which is what makes this the distance to the *segment* and not to the
  // infinite line through it -- the difference matters at every corner of every contour.
  const Scalar t = std::clamp(((point.x - a.x) * dx + (point.y - a.y) * dy) / lengthSquared, 0.0f, 1.0f);
  const Scalar closestX = a.x + dx * t;
  const Scalar closestY = a.y + dy * t;
  const Scalar px = point.x - closestX;
  const Scalar py = point.y - closestY;
  return std::sqrt(px * px + py * py);
}

/**
 * @return the signed distance from @p point to the nearest edge of @p contours.
 *
 * **Positive outside**, negative inside, so `coverageForSignedDistance` reads as "how far in". The sign comes from a
 * winding count -- the same rule `fillContours` uses, which is why the two agree on inside and outside even where they
 * disagree on the edge itself.
 *
 * Cost is one pass over every segment per call, so this is for a CPU reference and for tests, not for a per-pixel loop
 * over a real path. The field generation the GPU path needs walks pixels and edges together, not this.
 */
[[nodiscard]] inline Scalar signedDistanceToContours(const std::vector<Contour>& contours, const Point& point,
        const FillType fillRule = FillType::Winding) noexcept {
  Scalar nearest = std::numeric_limits<Scalar>::max();

  // The winding count for the sign, computed with a ray to the right. The half-open rule is the rasterizer's: an edge
  // counts when the point is at or below its top and strictly above its bottom, which is what stops a vertex from being
  // counted twice.
  int winding = 0;

  for (const Contour& contour: contours) {
    const std::vector<Point>& points = contour.points;
    if (points.size() < 2) continue;

    for (size_t i = 0; i < points.size(); ++i) {
      const Point& a = points[i];
      const Point& b = points[(i + 1) % points.size()];

      nearest = std::min(nearest, distanceToSegment(point, a, b));

      const Scalar top = std::min(a.y, b.y);
      const Scalar bottom = std::max(a.y, b.y);
      if (point.y < top || point.y >= bottom) continue;
      const Scalar t = (point.y - a.y) / (b.y - a.y);
      const Scalar crossingX = a.x + (b.x - a.x) * t;
      if (!std::isfinite(crossingX)) continue;
      if (crossingX > point.x) winding += b.y > a.y ? 1 : -1;
    }
  }

  if (nearest == std::numeric_limits<Scalar>::max()) return nearest;
  const bool inside = fillRule == FillType::Winding ? winding != 0 : (winding & 1) != 0;
  return inside ? -nearest : nearest;
}

/**
 * @return coverage in `[0, 1]` for a signed distance, using the one formula both implementations share.
 *
 * Half covered exactly on the edge, one at `kEdgeFeather / 2` inside, zero at `kEdgeFeather / 2` outside. This is the
 * same expression `quad.frag` uses for a rounded rectangle's distance, which is deliberate: the GPU already had one
 * distance-to-coverage mapping and this is it, named and shared rather than duplicated.
 */
[[nodiscard]] inline Scalar coverageForSignedDistance(const Scalar signedDistance,
        const Scalar feather = kEdgeFeather) noexcept {
  if (feather <= 0.0f) return signedDistance <= 0.0f ? 1.0f : 0.0f;
  return std::clamp(0.5f - signedDistance / feather, 0.0f, 1.0f);
}

/**
 * A coverage field for @p contours: one 8-bit coverage per pixel, evaluated at pixel centres.
 *
 * **This is step S2 of the plan**: the CPU side of the unified definition, produced by exactly the function the GPU's
 * fragment stage will use, so that the two can be compared before any of it is wired into a render path.
 *
 * @param originX the device x of output pixel (0, 0). The field covers `[originX, originX + width)`.
 * @param originY likewise for y.
 *
 * ## Why it is a field and not a call per pixel
 *
 * `signedDistanceToContours` walks every segment per call, so producing a field this way is O(pixels x segments) -- far
 * too slow for a render path and fine for what this is for: a reference to measure the shape of the disagreement, and a
 * test oracle for the shader. The GPU's version samples a distance texture; a faster CPU version would bin edges by row,
 * which is the same optimisation the scanline rasterizer already does with its crossings and is deliberately not done
 * here.
 *
 * @return a `width` by `height` bitmap of coverage in the alpha channel, or an empty bitmap if the size is not
 *         allocatable.
 */
[[nodiscard]] inline Bitmap edgeCoverageField(const std::vector<Contour>& contours, const int originX, const int originY,
        const int width, const int height, const FillType fillRule = FillType::Winding,
        const Scalar feather = kEdgeFeather) {
  if (width <= 0 || height <= 0) return Bitmap {};

  Bitmap field = Bitmap::allocate(ImageInfo(width, height, ColorType::kRGBA_8888, AlphaType::kPremultiplied));
  if (field.isEmpty()) return field;
  Pixmap pixels = field.lockPixels();
  if (pixels.isEmpty()) return field;

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      // Pixel *centres*, which is the convention this header states and the rasterizer shares.
      const Point centre { static_cast<Scalar>(originX + x) + 0.5f, static_cast<Scalar>(originY + y) + 0.5f };
      const Scalar coverage = coverageForSignedDistance(signedDistanceToContours(contours, centre, fillRule), feather);
      const uint32_t level = static_cast<uint32_t>(coverage * 255.0f + 0.5f);
      // White at the coverage, premultiplied, so the alpha channel is the coverage and the value matches what the
      // recorder already writes into a coverage bitmap.
      pixels.setPixelARGB32(x, y, (level << 24) | (level << 16) | (level << 8) | level);
    }
  }
  return field;
}

/**
 * A coverage field: 8-bit coverage per texel, at the texel's **centre**.
 *
 * This is what the GPU's fragment stage samples, and the CPU reference samples it through `sampleCoverageField` below.
 * The convention is stated here rather than left to a shader: texel `(i, j)` holds the coverage of device pixel
 * `(originX + i, originY + j)`, and a sample at a fractional device position interpolates between the four nearest texel
 * centres.
 *
 * ## Why coverage and not a signed distance -- the S2 finding
 *
 * The plan originally called for a signed distance field, on the assumption that the software rasterizer already computed
 * one. It does not, and measuring both against geometry neither defines
 * (`EdgeCoverage.TheRasterizerIsCloserToTheExactAreaThanTheDistanceFormula`) showed which is right:
 *
 *   | | worst difference from the exact area, on a fractional rectangle |
 *   |:--|--:|
 *   | the rasterizer's box coverage | **13** of 255 |
 *   | a radial signed distance mapped to coverage | **39** of 255 |
 *
 * The reason is visible in one pixel: for a rectangle spanning `x = 10.3 .. 30.4`, the pixel centred at `x = 30.5` is
 * *outside* and 0.1 past the edge, so the radial distance is `-0.1` and the formula reports 0.6 covered -- while the
 * exact overlap is 0.12. A single distance to the *nearest* edge cannot see that the shape is only a tenth of a pixel
 * wide on that side.
 *
 * So the unified definition is the one the rasterizer already implements, stored as coverage. Sampling it bilinearly is
 * well defined and reproduces the rasterizer rather than introducing a third answer. The cost is stated where it belongs:
 * coverage does not scale the way a distance field does, so magnifying a cached field will soften. A distance field is
 * the upgrade *if* magnified reuse is ever needed, and it would have to be a two-distance-per-axis field, because S2 is
 * the evidence that one distance is not enough.
 */
struct CoverageField {
  /** Coverage of texel (i, j) is the coverage of device pixel (originX + i, originY + j). */
  int originX = 0;
  int originY = 0;
  int width = 0;
  int height = 0;
  /** One coverage per texel, row major. */
  std::vector<uint8_t> texels;

  [[nodiscard]] bool isEmpty() const noexcept { return width <= 0 || height <= 0 || texels.empty(); }
};

/**
 * @return a field of the given lattice with its texels allocated and zeroed, and **nothing filled in yet**.
 *
 * Half of `makeCoverageField`, split out because a field whose scanline work is spread across threads needs its
 * destination before any of them runs: one allocation, then `fillCoverageRows` per band. A field built this way and left
 * unfilled is a valid value -- a blank one -- so a caller that abandons a band gets an empty field rather than a torn one.
 */
[[nodiscard]] inline CoverageField makeEmptyCoverageField(const int originX, const int originY, const int width,
        const int height) {
  CoverageField field;
  field.originX = originX;
  field.originY = originY;
  field.width = width;
  field.height = height;
  if (width <= 0 || height <= 0) return field;
  field.texels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
  return field;
}

/**
 * Fills rows @p firstRow through @p lastRow (inclusive) of @p texels from @p contours **already in field space**.
 *
 * "Field space" is the destination's own coordinates -- the field's origin subtracted -- because that is the space
 * `raster::fillContours` bounds its loops by. A caller with device-space contours wants `fillCoverageRowsShifted` below,
 * which is the same function with the shift done for it; the two are separate rather than one with a flag so that the
 * requirement is in the name of the function a caller is reading, instead of in a comment they may not be.
 *
 * The destination is the caller's buffer rather than a returned field, and that is the point: a large field can be filled
 * a band at a time by different threads, all writing disjoint rows of one allocation. This is also the entry that costs
 * nothing to call -- no allocation, no copy of the contours -- which is what lets the renderer fill forty thousand fields
 * a frame without forty thousand copies of their geometry. The bounds are checked here as well as in the rasterizer: a
 * wrong row range would otherwise write outside the buffer rather than leave a band blank, and a band that misses the
 * shape entirely is a legitimate thing to ask for.
 */
inline void fillCoverageRows(const std::vector<Contour>& contours, std::vector<uint8_t>& texels, const int width,
        const int height, const FillType fillRule, const int firstRow, const int lastRow) {
  if (width <= 0 || height <= 0) return;
  const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height);
  if (texels.size() < expected) return;
  if (firstRow > lastRow || lastRow < 0 || firstRow >= height) return;

  raster::fillContours(contours, width, height, fillRule, firstRow, lastRow,
          [&texels, width, height](const int x, const int y, const uint32_t coverage) {
            if (x < 0 || y < 0 || x >= width || y >= height) return;
            texels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] =
                    static_cast<uint8_t>(coverage > 255u ? 255u : coverage);
          });
}

/**
 * The same fill, writing into a **larger buffer** whose `(destX, destY)` texel is the field's `(0, 0)`.
 *
 * This is the entry the GPU route uses, and the reason is measured rather than aesthetic: fields are rasterised straight
 * into the frame's field atlas, so there is no per-field buffer to allocate, no second pass to copy one, and no small
 * scattered writes to share cache lines between threads. A field's own extent still bounds the row range and the write,
 * so an off-by-one in the destination is a wrong picture rather than a buffer overrun.
 */
inline void fillCoverageRowsAt(const std::vector<Contour>& contours, std::vector<uint8_t>& destination,
        const int destinationWidth, const int destinationHeight, const int destX, const int destY, const int width,
        const int height, const FillType fillRule, const int firstRow, const int lastRow,
        raster::FillScratch& scratch) {
  if (width <= 0 || height <= 0 || destinationWidth <= 0 || destinationHeight <= 0) return;
  if (destination.size() < static_cast<size_t>(destinationWidth) * static_cast<size_t>(destinationHeight)) return;
  if (firstRow > lastRow || lastRow < 0 || firstRow >= height) return;

  // ## Clear the band before filling it
  //
  // `fillContours` writes only the texels the shape covers, so a destination that already holds a previous
  // frame's field keeps every texel the new shape does not touch. That is correct for a freshly zeroed
  // atlas and wrong for the partial-refill path, where a field keeps its slot across frames: the old
  // coverage stayed around the new shape and was composited as a second outline at the previous
  // orientation. The slot is where the field's texels belong, so the part of it this band owns is cleared
  // first -- the border pass replicates the edges afterwards.
  const int clearFirstRow = std::max(0, destY + firstRow);
  const int clearLastRow = std::min(destinationHeight - 1, destY + lastRow);
  const int clearFirstColumn = std::max(0, destX);
  const int clearLastColumn = std::min(destinationWidth, destX + width);
  if (clearFirstColumn < clearLastColumn) {
    const size_t columns = static_cast<size_t>(clearLastColumn - clearFirstColumn);
    for (int row = clearFirstRow; row <= clearLastRow; ++row) {
      std::fill_n(destination.begin() + static_cast<size_t>(row) * static_cast<size_t>(destinationWidth) +
                          static_cast<size_t>(clearFirstColumn),
              columns, uint8_t { 0 });
    }
  }

  raster::fillContours(contours, width, height, fillRule, firstRow, lastRow, scratch,
          [&destination, destinationWidth, destinationHeight, destX, destY, width, height](
                  const int x, const int y, const uint32_t coverage) {
            const int targetX = destX + x;
            const int targetY = destY + y;
            if (x < 0 || y < 0 || x >= width || y >= height) return;
            if (targetX < 0 || targetY < 0 || targetX >= destinationWidth || targetY >= destinationHeight) return;
            destination[static_cast<size_t>(targetY) * static_cast<size_t>(destinationWidth) +
                        static_cast<size_t>(targetX)] = static_cast<uint8_t>(coverage > 255u ? 255u : coverage);
          });
}

/**
 * The same, for @p contours in **device** space: shifts them by @p originX / @p originY and then fills the rows.
 *
 * The shift is a copy of every point, so this is the entry for a caller that has its contours once and fills one field --
 * `makeCoverageField` -- rather than for one filling forty thousand fields, which shifts each field's contours itself
 * while it copies them out of the list it is reading them from.
 */
inline void fillCoverageRowsShifted(const std::vector<Contour>& contours, std::vector<uint8_t>& texels, const int width,
        const int height, const int originX, const int originY, const FillType fillRule, const int firstRow,
        const int lastRow) {
  if (width <= 0 || height <= 0) return;
  if (firstRow > lastRow) return;

  // The contours are in device space, so they are shifted so that the field's origin is the destination's origin -- the
  // rasterizer bounds its own loops by the destination and would skip geometry outside it.
  std::vector<Contour> shifted = contours;
  for (Contour& contour: shifted) {
    for (Point& point: contour.points) {
      point.x -= static_cast<Scalar>(originX);
      point.y -= static_cast<Scalar>(originY);
    }
  }

  fillCoverageRows(shifted, texels, width, height, fillRule, firstRow, lastRow);
}

/**
 * @return a coverage field for @p contours, filled by the **software rasterizer itself**.
 *
 * ## Why this delegates instead of computing an area here
 *
 * The point of a unified definition is that both sides run *one* implementation, not two that agree by inspection. So
 * this fills a one-texel-per-pixel destination through `raster::fillContours` and copies out its 8-bit coverage. A second
 * implementation of box coverage, however careful, would be a second thing to keep in step -- and the defect being fixed
 * here is precisely two implementations of one idea.
 *
 * The cost is that a field inherits the rasterizer's four-sample vertical quantisation. That is deliberate: the goal is to
 * *match* the rasterizer, not to be smoother than it.
 *
 * It is now literally the two steps above in sequence -- allocate, then fill every row -- so that the single-threaded and
 * the banded paths cannot drift apart: the parallel caller in `GpuRenderer::resolveCoverageFields` runs the same
 * `fillCoverageRows` over a range of rows, and correctness there is a property of the ranges being disjoint.
 */
[[nodiscard]] inline CoverageField makeCoverageField(const std::vector<Contour>& contours, const int originX,
        const int originY, const int width, const int height, const FillType fillRule = FillType::Winding) {
  CoverageField field = makeEmptyCoverageField(originX, originY, width, height);
  fillCoverageRowsShifted(contours, field.texels, field.width, field.height, field.originX, field.originY, fillRule, 0,
          field.height - 1);
  return field;
}

/**
 * @return the coverage at device position (@p deviceX, @p deviceY), bilinearly interpolated between texel centres.
 *
 * **This is the definition the shader samples**, and it is the whole of S3. A position exactly on a texel's centre returns
 * that texel; a position between four centres blends them by area. That is what GL's `texture()` with `GL_LINEAR` does, so
 * the fragment stage needs no arithmetic beyond the coordinate conversion -- and the test beside this asserts the CPU
 * version and the shader's expression are the same function, which is the only way two implementations of bilinear
 * sampling stay equal.
 *
 * ## Where a texel's centre is, and why that is not a detail
 *
 * Texel `i` holds the rasterizer's coverage of the **whole** device pixel `[originX + i, originX + i + 1)`, so the value
 * belongs at that pixel's centre -- device `originX + i + 0.5` -- and not at either of its corners. Putting it on a corner
 * instead (the `+ 0.5 / size` term an earlier version of this function and of `coverage.frag` both carried) makes a
 * fragment at a pixel *centre* land exactly between two texels and read their average: a two-texel box blur applied to a
 * quantity that is already an average. That is invisible in a shape's interior, where every texel is 255, and wrong at
 * every edge -- a band's last covered row came out at half coverage, and a field's one-pixel margin bled a quarter of the
 * interior into pixels the shape never reaches.
 *
 * So the continuous texel index is `deviceX - originX - 0.5`, and a fragment at a pixel centre lands on an integer: the
 * very texel the rasterizer filled for that pixel, unblurred.
 *
 * Outside the field the edge texel is held, which is clamp-to-edge sampling. A field is allocated to cover the geometry
 * with a margin, so a caller sampling outside it has a geometry bug rather than a sampling one.
 */
[[nodiscard]] inline Scalar sampleCoverageField(const CoverageField& field, const Scalar deviceX,
        const Scalar deviceY) noexcept {
  if (field.isEmpty()) return 0.0f;

  // Device position to texel-centre space: texel i's centre is the centre of device pixel `originX + i`, which is device
  // `originX + i + 0.5`, so the continuous index is `deviceX - originX - 0.5` and the two texels to blend are its floor
  // and that plus one.
  const Scalar continuousX = deviceX - static_cast<Scalar>(field.originX) - 0.5f;
  const Scalar continuousY = deviceY - static_cast<Scalar>(field.originY) - 0.5f;
  const Scalar floorX = std::floor(continuousX);
  const Scalar floorY = std::floor(continuousY);
  const Scalar fractionX = continuousX - floorX;
  const Scalar fractionY = continuousY - floorY;

  const auto texel = [&field](const int x, const int y) {
    // Clamp-to-edge, both for the interpolation's four taps and for a sample wholly outside the field.
    const int clampedX = std::clamp(x, 0, field.width - 1);
    const int clampedY = std::clamp(y, 0, field.height - 1);
    return static_cast<Scalar>(field.texels[static_cast<size_t>(clampedY) * static_cast<size_t>(field.width) +
                                             static_cast<size_t>(clampedX)]) /
           255.0f;
  };

  const int x0 = static_cast<int>(floorX);
  const int y0 = static_cast<int>(floorY);
  const Scalar topLeft = texel(x0, y0);
  const Scalar topRight = texel(x0 + 1, y0);
  const Scalar bottomLeft = texel(x0, y0 + 1);
  const Scalar bottomRight = texel(x0 + 1, y0 + 1);

  const Scalar top = topLeft + (topRight - topLeft) * fractionX;
  const Scalar bottom = bottomLeft + (bottomRight - bottomLeft) * fractionX;
  return top + (bottom - top) * fractionY;
}
} // namespace graphics::raster

#endif // CORAL_GRAPHICS_EDGE_COVERAGE_H
