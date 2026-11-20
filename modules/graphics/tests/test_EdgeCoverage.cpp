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

/**
 * @file test_EdgeCoverage.cpp
 *
 * Step **S1** of the plan in `路径边缘抗锯齿设计与现状.md`: measure how far the software rasterizer's box-area coverage
 * is from the shared signed-distance definition, *before* any GPU work is built on the assumption that they agree.
 *
 * ## Why this is the first step and not a formality
 *
 * The GPU path's edges are hard, and the fix everyone reaches for is "compute a signed distance and map it to coverage".
 * That is only a fix if the distance formula and the rasterizer agree about what a partly-covered pixel *is*. They do not
 * obviously:
 *
 *   * the rasterizer is exact in x (a vertical edge at `x = 5.3` covers 0.7 of column 5) and four-sample in y;
 *   * a radial signed distance at the pixel centre gives `coverage = clamp(0.5 - d, 0, 1)`, which for that same edge at
 *     the same pixel centre gives something else entirely.
 *
 * If they disagree on the case that *has* to hold -- an axis-aligned edge -- then "unify on the SDF" means moving the
 * rasterizer instead, which moves every already-green comparison test. This test is where that is discovered rather
 * than assumed, and it is cheap: a scalar function and no GPU.
 */

#include <private/graphics/EdgeCoverage.h>
#include <private/graphics/Rasterizer.h>

#include <graphics/Bitmap.h>
#include <graphics/Enums.h>
#include <graphics/Pixmap.h>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using graphics::FillType;
using graphics::Pixmap;
using graphics::Point;
using graphics::Scalar;
using graphics::raster::Contour;
using graphics::raster::coverageForSignedDistance;
using graphics::raster::signedDistanceToContours;

/** @return a rectangle as one closed contour. */
Contour rectangle(const Scalar left, const Scalar top, const Scalar right, const Scalar bottom) {
  Contour contour;
  contour.points = {
    Point { left, top },
    Point { right, top },
    Point { right, bottom },
    Point { left, bottom },
  };
  return contour;
}

/**
 * @return the rasterizer's coverage of one pixel, as a fraction in `[0, 1]`.
 *
 * Runs `fillContours` over a one-pixel destination offset so that the pixel under test lands at (0, 0), and reads the
 * coverage the blend callback is given. This is the *actual* software path, not a reimplementation of it -- which is what
 * makes the comparison worth anything.
 */
Scalar rasterizerCoverage(const std::vector<Contour>& contours, const int pixelX, const int pixelY,
        const FillType fillRule = FillType::Winding) {
  // The destination spans the pixel and its neighbours, so a shape that spills over is not clipped into agreement.
  constexpr int kSize = 3;
  auto bitmap = graphics::Bitmap::allocate(kSize, kSize);
  if (bitmap.isEmpty()) return -1.0f;
  Pixmap pixels = bitmap.lockPixels();
  if (pixels.isEmpty()) return -1.0f;

  // Translated so that the pixel under test is at (1, 1): the rasterizer bounds its own loops by the destination, and a
  // shape entirely outside it reports no coverage at all rather than zero.
  std::vector<Contour> shifted = contours;
  for (Contour& contour: shifted) {
    for (Point& point: contour.points) {
      point.x -= static_cast<Scalar>(pixelX - 1);
      point.y -= static_cast<Scalar>(pixelY - 1);
    }
  }

  uint32_t reported = 0;
  bool touched = false;
  graphics::raster::fillContours(shifted, pixels, fillRule,
          [&reported, &touched](const int x, const int y, const uint32_t coverage) {
            if (x == 1 && y == 1) {
              reported = coverage;
              touched = true;
            }
          });
  return touched ? static_cast<Scalar>(reported) / 255.0f : 0.0f;
}

} // namespace anonymous

/**
 * The case that *must* agree: a vertical edge at a fractional x.
 *
 * The rasterizer is exact in x, and a vertical edge's signed distance at a pixel centre is also purely horizontal, so
 * both are the same linear ramp in x. If these differ, the unification cannot be done by changing the GPU alone.
 */
TEST(EdgeCoverage, AVerticalEdgeAgreesWithTheRasterizer) {
  // An edge at x = 5.3, spanning rows well past the samples so the y extent never contributes.
  const std::vector<Contour> shape = { rectangle(5.3f, -10.0f, 40.0f, 40.0f) };

  int compared = 0;
  Scalar worst = 0.0f;
  for (int pixel = 3; pixel <= 7; ++pixel) {
    const Scalar box = rasterizerCoverage(shape, pixel, 4);
    // The pixel centre is (pixel + 0.5, 4.5); the edge is the vertical line x = 5.3.
    const Scalar distance = 5.3f - (static_cast<Scalar>(pixel) + 0.5f);
    const Scalar sdf = coverageForSignedDistance(distance);
    const Scalar difference = std::abs(box - sdf);
    std::printf("  column %d: box %.3f, sdf %.3f, difference %.3f\n", pixel, static_cast<double>(box),
            static_cast<double>(sdf), static_cast<double>(difference));
    worst = std::max(worst, difference);
    ++compared;
  }

  EXPECT_EQ(compared, 5);
  // A quarter of a coverage level: the rasterizer reports 8-bit coverage, so 1/255 is its own quantisation and anything
  // at or below that is the same number written twice.
  EXPECT_LE(worst, 0.01f) << "a vertical edge does not agree, so the two definitions are different functions of x";
}

/**
 * The case that is *expected* to differ: a horizontal edge.
 *
 * This is the useful negative. The rasterizer samples four sub-rows and steps between them, while the signed distance is
 * continuous, so near a fractional horizontal edge the two can differ by up to half a sample. Printing it is the point:
 * the number is what a tolerance has to be set from.
 */
TEST(EdgeCoverage, AHorizontalEdgeStepsWhereTheDistanceRamps) {
  const std::vector<Contour> shape = { rectangle(-10.0f, 3.4f, 40.0f, 40.0f) };

  Scalar worst = 0.0f;
  for (int pixel = 2; pixel <= 5; ++pixel) {
    const Scalar box = rasterizerCoverage(shape, 4, pixel);
    const Scalar distance = 3.4f - (static_cast<Scalar>(pixel) + 0.5f);
    const Scalar sdf = coverageForSignedDistance(distance);
    std::printf("  row %d: box %.3f, sdf %.3f\n", pixel, static_cast<double>(box), static_cast<double>(sdf));
    worst = std::max(worst, std::abs(box - sdf));
  }
  // No assertion on the value: this test exists to *print* the disagreement, which is what S2 and S4 have to live with
  // or remove. A bound is asserted only so that a future change cannot make it arbitrarily bad unnoticed.
  EXPECT_LE(worst, 0.25f) << "the y-axis disagreement is larger than a quarter of a coverage level; re-read this test";
}

/**
 * A diagonal edge, where a radial distance is provably not a box integral.
 *
 * The rasterizer integrates along x within each sub-row, so a 45-degree edge's coverage comes out as an area; the signed
 * distance measures the perpendicular gap. They are different quantities and this test records that they are, with the
 * magnitude printed, because it is the strongest argument for why "unify on the SDF" has to mean *changing the
 * rasterizer too* rather than bending the shader to fit it.
 */
TEST(EdgeCoverage, ADiagonalEdgeIsNotABoxIntegral) {
  // A 45-degree edge through (0, 0), with the filled side to the lower-right.
  Contour triangle;
  triangle.points = { Point { 0.0f, 0.0f }, Point { 40.0f, 40.0f }, Point { 40.0f, 0.0f } };
  const std::vector<Contour> shape = { triangle };

  Scalar worst = 0.0f;
  for (int index = 0; index < 3; ++index) {
    const int x = 4 + index;
    const int y = 4;
    const Scalar box = rasterizerCoverage(shape, x, y);
    const Point centre { static_cast<Scalar>(x) + 0.5f, static_cast<Scalar>(y) + 0.5f };
    const Scalar distance = signedDistanceToContours(shape, centre);
    const Scalar sdf = coverageForSignedDistance(distance);
    std::printf("  (%d,%d): box %.3f, signed distance %.3f, sdf %.3f, difference %.3f\n", x, y,
            static_cast<double>(box), static_cast<double>(distance), static_cast<double>(sdf),
            static_cast<double>(std::abs(box - sdf)));
    worst = std::max(worst, std::abs(box - sdf));
  }
  // Recorded rather than asserted away. This is the measurement step S1 exists to produce.
  std::printf("  worst diagonal disagreement: %.3f of a coverage level\n", static_cast<double>(worst));
  EXPECT_LE(worst, 0.35f) << "the diagonal disagreement is larger than a third of a level";
}

/**
 * The sign rule must match the rasterizer's, or a fully-covered pixel would read as empty.
 *
 * This is the part the SDF and the box *can* agree on exactly, because both decide "inside" from the same winding count
 * with the same half-open y rule -- so this asserts equality rather than a tolerance.
 */
TEST(EdgeCoverage, InteriorAndExteriorAgreeExactly) {
  const std::vector<Contour> shape = { rectangle(2.0f, 2.0f, 8.0f, 8.0f) };

  // Deep inside, deep outside, and on each side of the boundary.
  const Point interior { 5.0f, 5.0f };
  const Point exterior { 20.0f, 20.0f };
  EXPECT_LT(signedDistanceToContours(shape, interior), 0.0f) << "a point inside the rectangle is not negative";
  EXPECT_GT(signedDistanceToContours(shape, exterior), 0.0f) << "a point outside the rectangle is not positive";

  EXPECT_FLOAT_EQ(rasterizerCoverage(shape, 5, 5), 1.0f) << "the rasterizer does not fully cover an interior pixel";
  EXPECT_FLOAT_EQ(rasterizerCoverage(shape, 20, 20), 0.0f) << "the rasterizer covers an exterior pixel";
  EXPECT_FLOAT_EQ(coverageForSignedDistance(signedDistanceToContours(shape, interior)), 1.0f);
  EXPECT_FLOAT_EQ(coverageForSignedDistance(signedDistanceToContours(shape, exterior)), 0.0f);
}

/**
 * The formula's own contract, so that a change to it is a visible failure rather than a shift in every other number.
 */
TEST(EdgeCoverage, TheFormulaIsHalfOnTheEdgeAndSaturatesHalfAPixelOut) {
  EXPECT_FLOAT_EQ(coverageForSignedDistance(0.0f), 0.5f) << "an edge is half covered by definition";
  EXPECT_FLOAT_EQ(coverageForSignedDistance(0.5f), 0.0f) << "half a pixel outside is empty";
  EXPECT_FLOAT_EQ(coverageForSignedDistance(-0.5f), 1.0f) << "half a pixel inside is full";
  // Monotone decreasing, which is the only other property a coverage ramp has to have.
  Scalar previous = coverageForSignedDistance(-1.0f);
  for (int step = -10; step <= 10; ++step) {
    const Scalar value = coverageForSignedDistance(static_cast<Scalar>(step) * 0.1f);
    EXPECT_LE(value, previous + 1e-6f) << "coverage is not monotone at " << step;
    previous = value;
  }
}
// ==================================================================================================
// S2: the distance field, against the rasterizer's coverage, at bitmap scale
// ==================================================================================================

namespace {

/** The field the rasterizer itself produces, as an alpha bitmap of coverage. */
graphics::Bitmap rasterizerCoverageField(const std::vector<Contour>& contours, const int originX, const int originY,
        const int width, const int height, const FillType fillRule = FillType::Winding,
        const int samplesPerPixel = graphics::raster::kSamplesPerPixel) {
  auto bitmap = graphics::Bitmap::allocate(width, height);
  if (bitmap.isEmpty()) return bitmap;
  Pixmap pixels = bitmap.lockPixels();
  if (pixels.isEmpty()) return bitmap;

  // ## Why this clears the destination first
  //
  // `fillContours` reports only the pixels a shape touches -- it `continue`s on a zero coverage, which is the common case
  // at an edge -- and `Bitmap::allocate` hands back *uninitialised* bytes. So every pixel the shape does not reach kept
  // whatever the heap held there, and a comparison of this field against another one was comparing that garbage with a
  // zero. It happened to pass because a fresh allocation is usually zeroed by the operating system, which is a test that
  // passes for a reason it cannot state.
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      pixels.setPixelARGB32(x, y, 0u);
    }
  }

  // Translated so the field's origin is the destination's origin: the rasterizer bounds its own loops by the destination.
  std::vector<Contour> shifted = contours;
  for (Contour& contour: shifted) {
    for (Point& point: contour.points) {
      point.x -= static_cast<Scalar>(originX);
      point.y -= static_cast<Scalar>(originY);
    }
  }
  graphics::raster::fillContours(shifted, pixels, fillRule,
          [&pixels](const int x, const int y, const uint32_t coverage) {
            pixels.setPixelARGB32(x, y, (coverage << 24) | (coverage << 16) | (coverage << 8) | coverage);
          });
  return bitmap;
}

/** How far two coverage fields differ, bucketed, with the worst pixel and where it was. */
struct FieldDifference {
  int worst = 0;
  int worstX = 0;
  int worstY = 0;
  /** Buckets of 32 levels: [0,32), [32,64), ... */
  int buckets[8] = {};
  size_t overOneLevel = 0;
  size_t overHalfSample = 0;
  size_t compared = 0;
};

FieldDifference compareFields(const graphics::Bitmap& box, const graphics::Bitmap& sdf) {
  FieldDifference difference;
  const Pixmap boxPixels = box.makePixmap();
  const Pixmap sdfPixels = sdf.makePixmap();
  for (int y = 0; y < box.height(); ++y) {
    for (int x = 0; x < box.width(); ++x) {
      const int left = static_cast<int>((boxPixels.getPixelARGB32(x, y) >> 24) & 0xFFu);
      const int right = static_cast<int>((sdfPixels.getPixelARGB32(x, y) >> 24) & 0xFFu);
      const int delta = std::abs(left - right);
      difference.buckets[std::min(delta / 32, 7)]++;
      ++difference.compared;
      if (delta > 1) ++difference.overOneLevel;
      // A "half sample" is a sixteenth of a level, so 16 of 255: the rasterizer's y axis has four sub-samples, and a
      // disagreement at or below that is its own quantisation rather than a disagreement about the shape.
      if (delta > 16) ++difference.overHalfSample;
      if (delta > difference.worst) {
        difference.worst = delta;
        difference.worstX = x;
        difference.worstY = y;
      }
    }
  }
  return difference;
}

void report(const char* name, const FieldDifference& difference) {
  std::printf("  %-16s worst %3d at (%d,%d); over 1 level %zu/%zu; buckets 0-31 %d, 32-63 %d, 64-95 %d, 96-127 %d, "
              "128-159 %d, 160-191 %d, 192-223 %d, 224-255 %d\n",
          name, difference.worst, difference.worstX, difference.worstY, difference.overOneLevel, difference.compared,
          difference.buckets[0], difference.buckets[1], difference.buckets[2], difference.buckets[3],
          difference.buckets[4], difference.buckets[5], difference.buckets[6], difference.buckets[7]);
}

/** A circle as a closed contour, so the comparison is not only about axis-aligned edges. */
Contour circle(const Scalar centreX, const Scalar centreY, const Scalar radius, const int segments = 64) {
  Contour contour;
  for (int index = 0; index < segments; ++index) {
    const Scalar angle = static_cast<Scalar>(index) / static_cast<Scalar>(segments) * 6.28318530718f;
    contour.points.push_back(Point { centreX + std::cos(angle) * radius, centreY + std::sin(angle) * radius });
  }
  return contour;
}

} // namespace anonymous

/**
 * The measurement S2 exists to produce: how far the shared distance definition is from the rasterizer, over whole shapes
 * rather than single pixels.
 *
 * The single-pixel numbers in S1 said the disagreement is on the y axis and about 0.098 of a level. That is one pixel of
 * one edge; this is every pixel of four geometries, which is what a tolerance has to be argued from. The buckets are
 * printed rather than only the worst case, because "one pixel is off by 40" and "every edge pixel is off by 40" call for
 * different decisions.
 */
TEST(EdgeCoverage, TheDistanceFieldMatchesTheRasterizerOverWholeShapes) {
  constexpr int kSize = 48;
  constexpr int kOrigin = 4;

  struct Case {
    const char* name;
    std::vector<Contour> shape;
  };
  const std::vector<Case> cases = {
    { "rectangle", { rectangle(10.0f, 10.0f, 30.0f, 30.0f) } },
    // A fractional rectangle, which is the case the GPU's stencil route renders with hard edges today.
    { "rect fractional", { rectangle(10.3f, 10.7f, 30.4f, 30.2f) } },
    { "circle", { circle(22.0f, 22.0f, 12.0f) } },
    { "triangle", { Contour { { Point { 8.0f, 34.0f }, Point { 36.0f, 34.0f }, Point { 22.0f, 8.0f } } } } },
  };

  // ## What this test measures, and the one thing it asserts
  //
  // The buckets below are the S2 measurement: how far the shared distance definition is from the
  // rasterizer over whole shapes. Three of the four agree to within 10 of 255; the fractional rectangle
  // reaches 44, and where it does is the finding -- see the ground-truth test, which compares both
  // against geometry neither of them defines.
  //
  // The sample count was swept (4, 16, 64 sub-rows) on the theory that this was the rasterizer's vertical
  // quantisation. It is not: 44 -> 38 -> 40, and a quantisation error must vanish as the quantisation
  // refines. That parameter was therefore reverted rather than kept as dead weight.

  int worst = 0;
  size_t totalOverOneLevel = 0;
  for (const Case& testCase: cases) {
    const graphics::Bitmap box = rasterizerCoverageField(testCase.shape, kOrigin, kOrigin, kSize, kSize);
    ASSERT_FALSE(box.isEmpty()) << "the rasterizer produced no field for " << testCase.name;
    const graphics::Bitmap sdf = graphics::raster::edgeCoverageField(testCase.shape, kOrigin, kOrigin, kSize, kSize);
    ASSERT_FALSE(sdf.isEmpty()) << "the distance field was not produced for " << testCase.name;

    const FieldDifference difference = compareFields(box, sdf);
    report(testCase.name, difference);
    worst = std::max(worst, difference.worst);
    totalOverOneLevel += difference.overOneLevel;
  }
  std::printf("  worst over all shapes: %d of 255; pixels differing by more than one level: %zu\n", worst,
          totalOverOneLevel);

  EXPECT_GT(totalOverOneLevel, 0u) << "the two definitions agree bit for bit everywhere, so this test measures nothing";
  EXPECT_LE(worst, 64) << "the disagreement is no longer an edge difference at all";
}

// ==================================================================================================
// S2's finding: which of the two definitions is right, decided by geometry neither of them defines
// ==================================================================================================

/**
 * Ground truth for an **axis-aligned rectangle at fractional coordinates**: a pixel's exact coverage is the area of the
 * overlap between the pixel's unit square and the rectangle. That is elementary geometry rather than either rasterizer's
 * opinion, which is what makes it usable as a referee.
 *
 * ## Why this test exists, and what it found
 *
 * S1 concluded the disagreement was the rasterizer's four-sample y axis. The sweep in the test above **refuted that**: at
 * 64 sub-samples the worst disagreement was still 40 of 255 against 44 at four, and a quantisation error has to vanish as
 * the quantisation refines.
 *
 * So both definitions were compared against the exact area. Measured on a rectangle with all four edges fractional:
 *
 *   | | worst difference from the exact area |
 *   |:--|--:|
 *   | the rasterizer's box coverage | **13** of 255 |
 *   | the shared distance formula | **39** of 255 |
 *
 * **The rasterizer is closer to the truth, and the distance formula is the one that is wrong** for this geometry. That
 * inverts the plan's assumption: "unify on the SDF" as originally written would make the picture *worse*, which is
 * exactly what happened in rounds 67-77, where the coverage-image route improved one rotated square and degraded
 * `path.svg` from 1.8% to 4.5%.
 */
TEST(EdgeCoverage, TheRasterizerIsCloserToTheExactAreaThanTheDistanceFormula) {
  const Scalar left = 10.3f;
  const Scalar top = 10.7f;
  const Scalar right = 30.4f;
  const Scalar bottom = 30.2f;
  const std::vector<Contour> shape = { rectangle(left, top, right, bottom) };

  constexpr int kOrigin = 4;
  constexpr int kSize = 40;

  /** The exact area of the overlap between pixel (x, y) and the rectangle, in units of one pixel. */
  const auto exactCoverage = [&](const int x, const int y) {
    const Scalar pixelLeft = static_cast<Scalar>(kOrigin + x);
    const Scalar pixelTop = static_cast<Scalar>(kOrigin + y);
    const Scalar overlapX = std::max(0.0f, std::min(pixelLeft + 1.0f, right) - std::max(pixelLeft, left));
    const Scalar overlapY = std::max(0.0f, std::min(pixelTop + 1.0f, bottom) - std::max(pixelTop, top));
    return overlapX * overlapY;
  };

  const graphics::Bitmap box = rasterizerCoverageField(shape, kOrigin, kOrigin, kSize, kSize);
  const graphics::Bitmap sdf = graphics::raster::edgeCoverageField(shape, kOrigin, kOrigin, kSize, kSize);
  ASSERT_FALSE(box.isEmpty());
  ASSERT_FALSE(sdf.isEmpty());
  const Pixmap boxPixels = box.makePixmap();
  const Pixmap sdfPixels = sdf.makePixmap();

  int worstBox = 0;
  int worstSdf = 0;
  int boxX = 0;
  int boxY = 0;
  int sdfX = 0;
  int sdfY = 0;
  size_t boundaryPixels = 0;
  for (int y = 0; y < kSize; ++y) {
    for (int x = 0; x < kSize; ++x) {
      const Scalar exact = exactCoverage(x, y);
      // Only the boundary is interesting: interior and exterior are 1 and 0 for all three, and including them would
      // drown the comparison in agreement.
      if (exact <= 0.001f || exact >= 0.999f) continue;
      ++boundaryPixels;
      const int exactLevel = static_cast<int>(exact * 255.0f + 0.5f);
      const int boxLevel = static_cast<int>((boxPixels.getPixelARGB32(x, y) >> 24) & 0xFFu);
      const int sdfLevel = static_cast<int>((sdfPixels.getPixelARGB32(x, y) >> 24) & 0xFFu);
      if (std::abs(boxLevel - exactLevel) > worstBox) {
        worstBox = std::abs(boxLevel - exactLevel);
        boxX = x;
        boxY = y;
      }
      if (std::abs(sdfLevel - exactLevel) > worstSdf) {
        worstSdf = std::abs(sdfLevel - exactLevel);
        sdfX = x;
        sdfY = y;
      }
    }
  }

  std::printf("  %zu boundary pixels; box worst %d of 255 at (%d,%d); sdf worst %d of 255 at (%d,%d)\n", boundaryPixels,
          worstBox, boxX, boxY, worstSdf, sdfX, sdfY);
  const auto probe = [&](const char* which, const int x, const int y) {
    const int exactLevel = static_cast<int>(exactCoverage(x, y) * 255.0f + 0.5f);
    std::printf("    worst for %s at (%d,%d): exact %d, box %d, sdf %d\n", which, x, y, exactLevel,
            static_cast<int>((boxPixels.getPixelARGB32(x, y) >> 24) & 0xFFu),
            static_cast<int>((sdfPixels.getPixelARGB32(x, y) >> 24) & 0xFFu));
  };
  probe("box", boxX, boxY);
  probe("sdf", sdfX, sdfY);

  EXPECT_GT(boundaryPixels, 50u) << "no boundary pixels were compared, so this measures nothing";
  EXPECT_LE(worstBox, 16) << "the rasterizer is no longer close to the exact area, which would change the conclusion";
  EXPECT_GT(worstSdf, worstBox)
          << "the distance formula is now at least as good as the rasterizer, so the unification can proceed as the "
             "plan originally described after all";
  EXPECT_LE(worstSdf, 48) << "the distance formula is further from the truth than this measurement recorded";
}

/**
 * Where the distance formula fails, isolated from corners, from thin shapes and from the other axis.
 *
 * The ground-truth test put the worst distance pixel at `(26,6)` on a rectangle spanning `x = 10.3 .. 30.4`. That pixel's
 * centre is `x = 30.5` -- *outside* the rectangle and 0.1 past its right edge -- so the radial distance is `-0.1` and the
 * formula reports 0.6 covered, while the exact overlap is 0.12 because the pixel is mostly outside. A single radial
 * distance to the **nearest** edge cannot see that the shape is only 0.1 of a pixel wide on that side.
 *
 * This test is the other half of that argument: the same formula is **exact** for a plain vertical edge through a wide
 * shape. So the failure is not the axis convention and not a corner -- it is the definition being insufficient, and the
 * two tests together bound exactly where it stops working.
 */
TEST(EdgeCoverage, AVerticalEdgeThroughAWideShapeIsExactForBoth) {
  Scalar worstBox = 0.0f;
  Scalar worstSdf = 0.0f;
  std::printf("  edge x   pixel   exact   box    sdf\n");

  for (int step = 0; step < 5; ++step) {
    const Scalar edge = 12.0f + static_cast<Scalar>(step) * 0.2f;
    // A tall rectangle, so its horizontal edges are far from the pixel under test.
    const std::vector<Contour> shape = { rectangle(edge, 0.0f, 40.0f, 40.0f) };

    const int column = static_cast<int>(std::floor(edge));
    // The part of that column inside the shape, which is the exact coverage of a vertical edge.
    const Scalar exact = static_cast<Scalar>(column) + 1.0f - edge;
    const Scalar centreX = static_cast<Scalar>(column) + 0.5f;
    const Scalar sdf = coverageForSignedDistance(edge - centreX);

    // The rasterizer's value for that column, taken from a one-row strip far from the ends.
    const graphics::Bitmap box = rasterizerCoverageField(shape, 0, 20, 48, 1);
    const Scalar boxCoverage = static_cast<Scalar>((box.makePixmap().getPixelARGB32(column, 0) >> 24) & 0xFFu) / 255.0f;

    std::printf("  %6.2f  %5d  %5.3f  %5.3f  %5.3f\n", static_cast<double>(edge), column, static_cast<double>(exact),
            static_cast<double>(boxCoverage), static_cast<double>(sdf));
    worstBox = std::max(worstBox, std::abs(boxCoverage - exact));
    worstSdf = std::max(worstSdf, std::abs(sdf - exact));
  }

  std::printf("  worst: box %.3f, sdf %.3f\n", static_cast<double>(worstBox), static_cast<double>(worstSdf));
  // Both are exact here, which is what makes the ground-truth failure a statement about *thin* geometry rather than
  // about the formula being wrong everywhere.
  EXPECT_LE(worstBox, 0.02f) << "the rasterizer is not exact for a plain vertical edge through a wide shape";
  EXPECT_LE(worstSdf, 0.02f) << "the distance formula is not exact for a plain vertical edge through a wide shape";
}

// ==================================================================================================
// S3: the shader's expression, evaluated on the CPU, against the CPU reference sampler
// ==================================================================================================

/**
 * The fragment stage's sampling expression, transcribed from `shaders/canvas/coverage.frag` and evaluated here.
 *
 * ## Why transcribe instead of calling the reference
 *
 * S3's acceptance is that the *shader* and the CPU agree. Calling `sampleCoverageField` on both sides would test nothing:
 * it is one function. So this is a second implementation, deliberately, written from the shader's GLSL line by line --
 * `coordinate = (device - fieldOrigin) / fieldSize`, then a bilinear tap -- and the test asserts the two agree. If the
 * shader's arithmetic changes and this does not, the test fails, which is the only mechanism available for keeping a GPU
 * program and a header in step.
 *
 * A shader cannot include `EdgeCoverage.h` and a test cannot run GLSL, so this transcription *is* the link. It is stated
 * as a weakness rather than presented as a solution.
 *
 * ## The half-texel term that used to be here, and what it did
 *
 * Both this transcription and the shader carried `+ 0.5 / fieldSize`, which places a texel's value on a device pixel
 * *corner*. Texel `i` is the coverage of the whole pixel `[originX + i, originX + i + 1)`, so its value belongs at that
 * pixel's centre, and with the corner placement a fragment at a pixel centre sat exactly between two texels and read
 * their average -- a two-texel box blur. The shape's interior is unaffected and its every edge is halved onto the
 * neighbouring row or column.
 *
 * The two agreed with each other the whole time, which is exactly why S3's acceptance could not catch it: it proves the
 * shader and this file are one function, not that the function samples the field where the field's values live.
 *
 * ## The texture's extent, which is not always the field's
 *
 * `textureWidth`/`textureHeight` model `uField.xy`, which is the extent of the **texture** the field lives in rather than
 * the field's own: the renderer pools field textures in power-of-two size classes, so a 20x14 field is held in a 32x16
 * texture with the field's texels in the top-left corner and zeroed padding beside them. Zero (the default) means "the
 * texture is exactly the field", which is the unpadded case the grid test below uses; the padded case is
 * `APaddedTextureSamplesLikeTheFieldAtPixelCentres`.
 */
Scalar shaderExpressionSample(const graphics::raster::CoverageField& field, const Scalar deviceX, const Scalar deviceY,
        const int textureWidth = 0, const int textureHeight = 0, const std::vector<uint8_t>* texture = nullptr) {
  if (field.isEmpty()) return 0.0f;
  const int texelExtentX = textureWidth > 0 ? textureWidth : field.width;
  const int texelExtentY = textureHeight > 0 ? textureHeight : field.height;
  const Scalar fieldWidth = static_cast<Scalar>(texelExtentX);
  const Scalar fieldHeight = static_cast<Scalar>(texelExtentY);

  // `vec2 coordinate = (device - uField.uField.zw) / uField.uField.xy;`
  // GLSL has no `double`, so the divisions are `highp float` -- which is what this models with `Scalar`.
  const Scalar coordinateX = (deviceX - static_cast<Scalar>(field.originX)) / fieldWidth;
  const Scalar coordinateY = (deviceY - static_cast<Scalar>(field.originY)) / fieldHeight;

  // `texture(uCoverageField, coordinate).a` with a linear sampler: the texel index is `coordinate * size - 0.5`, which
  // is where a texel *centre* lands, and GL clamps to the edge. Substituting the coordinate above gives
  // `device - origin - 0.5`, i.e. an integer at a pixel centre -- which is what `sampleCoverageField` computes. Note
  // that the `size` cancels out of that substitution: the index does not depend on which extent is divided by, which is
  // exactly why a padded texture is safe.
  const Scalar texelX = coordinateX * fieldWidth - 0.5f;
  const Scalar texelY = coordinateY * fieldHeight - 0.5f;
  const Scalar floorX = std::floor(texelX);
  const Scalar floorY = std::floor(texelY);
  const Scalar fractionX = texelX - floorX;
  const Scalar fractionY = texelY - floorY;

  // The texture's own storage, if one was given (a padded buffer with the field in its top-left corner), read with
  // clamp-to-edge over the *texture* -- which is what GL does, and why the padding has to be a known value.
  const auto texel = [&field, texelExtentX, texelExtentY, texture](const int x, const int y) {
    const int clampedX = std::clamp(x, 0, texelExtentX - 1);
    const int clampedY = std::clamp(y, 0, texelExtentY - 1);
    if (texture != nullptr) {
      return static_cast<Scalar>((*texture)[static_cast<size_t>(clampedY) * static_cast<size_t>(texelExtentX) +
                                             static_cast<size_t>(clampedX)]) /
             255.0f;
    }
    // No texture given: the field *is* the texture, so a tap outside it is clamp-to-edge over the field.
    const int fieldX = std::clamp(clampedX, 0, field.width - 1);
    const int fieldY = std::clamp(clampedY, 0, field.height - 1);
    return static_cast<Scalar>(field.texels[static_cast<size_t>(fieldY) * static_cast<size_t>(field.width) +
                                             static_cast<size_t>(fieldX)]) /
           255.0f;
  };

  const int x0 = static_cast<int>(floorX);
  const int y0 = static_cast<int>(floorY);
  const Scalar top = texel(x0, y0) + (texel(x0 + 1, y0) - texel(x0, y0)) * fractionX;
  const Scalar bottom = texel(x0, y0 + 1) + (texel(x0 + 1, y0 + 1) - texel(x0, y0 + 1)) * fractionX;
  return top + (bottom - top) * fractionY;
}

/**
 * S3's acceptance: the shader's expression and the CPU reference are the same function, over a grid of samples.
 *
 * The tolerance is 2/255, which is the plan's number and is tighter than one coverage level: two implementations of one
 * bilinear expression should agree to rounding, and anything larger means they are not the same expression.
 */
TEST(EdgeCoverage, TheShaderExpressionMatchesTheReference) {
  // A field with distinct neighbours, so an interpolation error cannot hide behind equal texels: the shape is a
  // fractional rectangle, which gives a gradient of coverage along every edge.
  const std::vector<Contour> shape = { rectangle(10.3f, 10.7f, 30.4f, 30.2f) };
  constexpr int kOrigin = 8;
  constexpr int kWidth = 28;
  constexpr int kHeight = 28;
  const graphics::raster::CoverageField field = graphics::raster::makeCoverageField(shape, kOrigin, kOrigin, kWidth, kHeight);
  ASSERT_FALSE(field.isEmpty()) << "the coverage field was not produced";

  Scalar worst = 0.0f;
  Scalar worstX = 0.0f;
  Scalar worstY = 0.0f;
  int compared = 0;
  int differingTexels = 0;
  // Every half-pixel position across the field, so the samples land on texel centres *and* between them. Sampling only
  // on centres would compare the taps and never the interpolation.
  for (int step = 0; step <= kWidth * 2; ++step) {
    for (int column = 0; column <= kWidth * 2; ++column) {
      const Scalar deviceX = static_cast<Scalar>(kOrigin) + static_cast<Scalar>(column) * 0.5f;
      const Scalar deviceY = static_cast<Scalar>(kOrigin) + static_cast<Scalar>(step) * 0.5f;

      const Scalar reference = graphics::raster::sampleCoverageField(field, deviceX, deviceY);
      const Scalar shader = shaderExpressionSample(field, deviceX, deviceY);
      const Scalar difference = std::abs(reference - shader);
      if (difference > worst) {
        worst = difference;
        worstX = deviceX;
        worstY = deviceY;
      }
      // One coverage level, which is the quantisation both sides share.
      if (difference > 1.0f / 255.0f) ++differingTexels;
      ++compared;
    }
  }

  std::printf("  %d samples; worst disagreement %.6f at (%.1f,%.1f); samples over one level: %d\n", compared,
          static_cast<double>(worst), static_cast<double>(worstX), static_cast<double>(worstY), differingTexels);

  EXPECT_GT(compared, 1000) << "too few samples to be meaningful";
  // The plan's tolerance, expressed in coverage units: 2 of 255.
  EXPECT_LE(worst, 2.0f / 255.0f)
          << "the shader's expression and the CPU reference are not the same function, so the field would render "
             "differently from the way it was verified";
}

/**
 * The property a **padded** field texture rests on: at a pixel centre, the extent in the divisor does not matter.
 *
 * The renderer pools field textures in power-of-two size classes, so the texture is usually larger than the field and
 * the fragment stage divides by the *texture's* extent rather than the field's (`uField.xy` is the texture's). That is
 * only sound because of one thing, and this is it: for a device pixel `origin + i`, the coordinate is `(i + 0.5) /
 * extent`, so the linear sampler's texel index is `i + 0.5 - 0.5 = i` -- the extent cancels, and the tap has weight one
 * on that texel and zero on the padding beside it.
 *
 * ## What would go wrong without it, and what this would look like if it did
 *
 * An extent that leaked into the *index* would scale the picture: a 20x14 field in a 32x32 texture would draw the shape
 * 1.6x too large. An extent that leaked into the *origin* would shift it. Neither is subtle, so the failure this test
 * guards against is really the *third* possibility -- that the padding gets a non-zero weight from a tap whose
 * fractional part is a rounding error rather than exactly zero, which is why the renderer zeroes the padding once when
 * it creates the texture.
 *
 * ## Why this is a sibling of `TheShaderExpressionMatchesTheReference` and not part of it
 *
 * That test compares two implementations of one function on a texture the size of its field. This one compares the
 * *same* function against the *same* values through a texture that is larger than the field -- which is a different
 * claim, and the one the pool depends on.
 */
TEST(EdgeCoverage, APaddedTextureSamplesLikeTheFieldAtPixelCentres) {
  // An extent that is not a power of two in either axis, so a class has to pad it.
  const Scalar left = 6.3f;
  const Scalar top = 5.7f;
  const Scalar right = 26.4f;
  const Scalar bottom = 22.2f;
  const std::vector<Contour> shape = { rectangle(left, top, right, bottom) };

  constexpr int kOriginX = 5;
  constexpr int kOriginY = 4;
  constexpr int kWidth = 27;
  constexpr int kHeight = 20;
  // What `coverageTextureExtent` allocates for those extents: the smallest power of two that is at least each.
  constexpr int kTextureWidth = 32;
  constexpr int kTextureHeight = 32;

  const graphics::raster::CoverageField field = graphics::raster::makeCoverageField(shape, kOriginX, kOriginY, kWidth,
          kHeight);
  ASSERT_FALSE(field.isEmpty()) << "the coverage field was not produced";

  // The padded texture: the field's texels in the top-left corner, everything else zero -- exactly what the renderer
  // uploads after zeroing the newly created texture.
  std::vector<uint8_t> padded(static_cast<size_t>(kTextureWidth) * static_cast<size_t>(kTextureHeight), 0);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      padded[static_cast<size_t>(y) * kTextureWidth + static_cast<size_t>(x)] =
              field.texels[static_cast<size_t>(y) * kWidth + static_cast<size_t>(x)];
    }
  }

  int compared = 0;
  int differing = 0;
  Scalar worst = 0.0f;
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const Scalar deviceX = static_cast<Scalar>(kOriginX + x) + 0.5f;
      const Scalar deviceY = static_cast<Scalar>(kOriginY + y) + 0.5f;
      const Scalar reference = graphics::raster::sampleCoverageField(field, deviceX, deviceY);
      // The shader's expression, with the *texture's* extent as the divisor and the padded buffer as the storage.
      const Scalar paddedSample = shaderExpressionSample(field, deviceX, deviceY, kTextureWidth, kTextureHeight, &padded);
      worst = std::max(worst, std::abs(reference - paddedSample));
      if (std::abs(reference - paddedSample) > 1.0f / 255.0f) ++differing;
      ++compared;
    }
  }

  std::printf("  %d pixel centres of a %dx%d field in a %dx%d texture: %d differ from the reference, worst %.6f\n",
          compared, kWidth, kHeight, kTextureWidth, kTextureHeight, differing, static_cast<double>(worst));

  EXPECT_GT(compared, 400) << "too few pixels to be meaningful";
  EXPECT_EQ(differing, 0) << "the padded texture does not sample like the field, so a pooled texture would render the "
                             "picture at the wrong scale or shifted";
}

/**
 * The other half of S3: the field itself *is* the rasterizer's coverage, or the two definitions are still two.
 *
 * This is cheap and it is the assertion that keeps the unification honest. `makeCoverageField` delegates to
 * `fillContours`, so this can only fail if that delegation breaks -- which is exactly the regression worth catching,
 * because a second implementation of box coverage is the tempting thing to write when this one is inconvenient.
 */
TEST(EdgeCoverage, TheFieldIsExactlyTheRasterizersCoverage) {
  const std::vector<Contour> shapes[] = {
    { rectangle(10.0f, 10.0f, 30.0f, 30.0f) },
    { rectangle(10.3f, 10.7f, 30.4f, 30.2f) },
    { circle(22.0f, 22.0f, 12.0f) },
  };
  constexpr int kOrigin = 8;
  constexpr int kSize = 28;

  for (const std::vector<Contour>& shape: shapes) {
    // The same geometry through the rasterizer's own field generator and through the unified one.
    const graphics::Bitmap box = rasterizerCoverageField(shape, kOrigin, kOrigin, kSize, kSize);
    ASSERT_FALSE(box.isEmpty());
    const Pixmap boxPixels = box.makePixmap();
    const graphics::raster::CoverageField field = graphics::raster::makeCoverageField(shape, kOrigin, kOrigin, kSize, kSize);
    ASSERT_FALSE(field.isEmpty());

    size_t differing = 0;
    int worst = 0;
    for (int y = 0; y < kSize; ++y) {
      for (int x = 0; x < kSize; ++x) {
        const int left = static_cast<int>((boxPixels.getPixelARGB32(x, y) >> 24) & 0xFFu);
        const int right = static_cast<int>(field.texels[static_cast<size_t>(y) * static_cast<size_t>(kSize) +
                                                       static_cast<size_t>(x)]);
        if (left != right) ++differing;
        worst = std::max(worst, std::abs(left - right));
      }
    }
    std::printf("  field against the rasterizer: %zu of %d texels differ, worst %d of 255\n", differing, kSize * kSize,
            worst);
    EXPECT_EQ(differing, 0u) << "the field is not bit-for-bit the rasterizer's coverage, so there are two definitions";
  }
}

/**
 * The property the GPU route actually depends on: **sampling the field at a device pixel's centre returns that pixel's
 * coverage.**
 *
 * This is the test the half-texel error walked through, and it is here because everything S3 asserted was true while the
 * picture was wrong. S3 compared the shader's expression with `sampleCoverageField`, which proves the two are one
 * function -- and they were, at both conventions. What it could not prove is that the function samples the field *where
 * the field's values live*. Both sides carried `+ 0.5 / fieldSize`, which puts texel `i`'s value on the pixel *corner* at
 * `originX + i` instead of its centre at `originX + i + 0.5`, so a fragment at a pixel centre read the average of the
 * pixel's own coverage and its neighbour's.
 *
 * That is a 2x2 box blur of coverage that is already a per-pixel average. Interior pixels are unaffected (all four taps
 * are 255) and every edge pixel is: a horizontal band's last covered row (coverage 1) came out at 0.5, and the one-pixel
 * margin of a field bled a quarter of the shape's interior onto pixels the shape never touches.
 *
 * ## Why the shape is a band with a fractional bottom edge
 *
 * The blur is symmetric, so a shape whose edges all land on integers hides it entirely -- every tap is 0 or 255 and the
 * average of 255, 255, 0, 0 is only wrong where a *single* row differs, which is exactly the last row of a band. The band
 * here is fractional in both axes so that every edge column and row is exercised, and the assertion is equality with the
 * rasterizer rather than a tolerance: at pixel centres there is nothing to interpolate, so there is nothing to round.
 */
TEST(EdgeCoverage, SamplingAtAPixelCentreReturnsThatPixelsCoverage) {
  // The recorder's own lattice: one pixel of margin, then the field covers floor(bounds) - 1 .. ceil(bounds) + 1.
  const Scalar left = 6.3f;
  const Scalar top = 5.7f;
  const Scalar right = 30.4f;
  const Scalar bottom = 22.2f;
  const std::vector<Contour> shape = { rectangle(left, top, right, bottom) };

  const int originX = static_cast<int>(std::floor(left)) - 1;
  const int originY = static_cast<int>(std::floor(top)) - 1;
  const int width = static_cast<int>(std::ceil(right)) + 1 - originX;
  const int height = static_cast<int>(std::ceil(bottom)) + 1 - originY;

  const graphics::raster::CoverageField field = graphics::raster::makeCoverageField(shape, originX, originY, width, height);
  ASSERT_FALSE(field.isEmpty());
  const graphics::Bitmap box = rasterizerCoverageField(shape, originX, originY, width, height);
  ASSERT_FALSE(box.isEmpty());
  const Pixmap boxPixels = box.makePixmap();

  int compared = 0;
  int differing = 0;
  int worst = 0;
  int worstX = 0;
  int worstY = 0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const Scalar deviceX = static_cast<Scalar>(originX + x) + 0.5f;
      const Scalar deviceY = static_cast<Scalar>(originY + y) + 0.5f;
      const int sampled = static_cast<int>(graphics::raster::sampleCoverageField(field, deviceX, deviceY) * 255.0f + 0.5f);
      const int expected = static_cast<int>((boxPixels.getPixelARGB32(x, y) >> 24) & 0xFFu);
      ++compared;
      const int delta = std::abs(sampled - expected);
      if (delta > 0) ++differing;
      if (delta > worst) {
        worst = delta;
        worstX = x;
        worstY = y;
      }
    }
  }

  // The band's last covered row, reported by name: this is the pixel the blur was found on. It is the lowest row at the
  // middle column that the rasterizer fills completely, which for a band ending at a fractional y is the row whose own
  // pixel is wholly inside.
  int lastRow = -1;
  for (int y = height - 1; y >= 0; --y) {
    if (((boxPixels.getPixelARGB32(width / 2, y) >> 24) & 0xFFu) == 255u) {
      lastRow = y;
      break;
    }
  }
  int bottomEdgeSampled = -1;
  int bottomEdgeExpected = -1;
  if (lastRow >= 0) {
    bottomEdgeSampled = static_cast<int>(graphics::raster::sampleCoverageField(field,
                                      static_cast<Scalar>(originX + width / 2) + 0.5f,
                                      static_cast<Scalar>(originY + lastRow) + 0.5f) *
                                      255.0f + 0.5f);
    bottomEdgeExpected = static_cast<int>((boxPixels.getPixelARGB32(width / 2, lastRow) >> 24) & 0xFFu);
  }

  std::printf("  sampled at %d pixel centres: %d differ, worst %d of 255 at (%d,%d); last covered row %d: sampled %d, "
              "rasterizer %d\n",
          compared, differing, worst, worstX, worstY, lastRow, bottomEdgeSampled, bottomEdgeExpected);

  EXPECT_GT(compared, 400) << "too few pixels to be meaningful";
  EXPECT_EQ(differing, 0) << "sampling the field at a pixel centre does not return that pixel's coverage, so the fragment "
                             "stage renders a blurred edge that no other test would call wrong";
}

/**
 * A field filled a band of rows at a time is **byte-for-byte** the field filled in one call.
 *
 * This is the property `GpuRenderer::resolveCoverageFields` rests on when it splits a large field across threads: the
 * rasterizer carries nothing from one row to the next (`Rasterizer.h` states why -- each row rebuilds its own crossings
 * and its own winding), so two disjoint row ranges cannot disagree about a row they do not share, and *all* the ranges
 * together have to equal the whole. The first half of that is an argument; this is the measurement.
 *
 * The bands here are deliberately uneven, out of order, and one of them misses the shape entirely. In-order, equal bands
 * would pass for a rasterizer that carried state between rows as long as it was handed the rows in sequence -- which is
 * exactly the implementation this test is meant to rule out.
 */
TEST(EdgeCoverage, BandsOfRowsFilledSeparatelyMatchTheWholeField) {
  constexpr int kOriginX = 5;
  constexpr int kOriginY = 7;
  constexpr int kWidth = 420;
  constexpr int kHeight = 380;

  // Big enough to be banded by the renderer's own rule (16 KiB of texels a band), and with a curved and a straight edge
  // so that every row's crossings differ from its neighbours'.
  const std::vector<Contour> shape = {
    circle(180.0f, 170.0f, 150.0f),
    rectangle(-40.0f, 60.0f, 260.0f, 300.0f),
  };

  const graphics::raster::CoverageField whole = graphics::raster::makeCoverageField(shape, kOriginX, kOriginY, kWidth,
          kHeight);
  ASSERT_FALSE(whole.isEmpty());
  ASSERT_EQ(whole.texels.size(), static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight));

  graphics::raster::CoverageField banded = graphics::raster::makeEmptyCoverageField(kOriginX, kOriginY, kWidth, kHeight);
  ASSERT_FALSE(banded.isEmpty());
  // Bands in an order no sequential implementation would survive, and one that covers only rows the shape never reaches.
  const std::pair<int, int> bands[] = {
    { 260, 379 }, { 0, 41 }, { 200, 259 }, { 42, 119 }, { 120, 199 },
  };
  for (const auto& [first, last]: bands) {
    graphics::raster::fillCoverageRowsShifted(shape, banded.texels, kWidth, kHeight, kOriginX, kOriginY,
            graphics::FillType::Winding, first, last);
  }

  size_t differing = 0;
  int worst = 0;
  for (size_t index = 0; index < whole.texels.size(); ++index) {
    const int delta = std::abs(static_cast<int>(whole.texels[index]) - static_cast<int>(banded.texels[index]));
    if (delta != 0) ++differing;
    worst = std::max(worst, delta);
  }
  std::printf("  %dx%d field in %zu bands: %zu of %zu texels differ, worst %d of 255\n", kWidth, kHeight,
          std::size(bands), differing, whole.texels.size(), worst);

  EXPECT_EQ(differing, 0u) << "filling rows in bands does not equal filling them in one call, so the renderer's split of "
                              "a large field across threads produces a different picture from the serial one";
  // And the bands covered the whole field: a bug that dropped the last band would pass the comparison above if the shape
  // did not reach those rows, so the shape's own extent is asserted too.
  EXPECT_EQ(static_cast<int>(graphics::raster::sampleCoverageField(banded, 180.0f + 0.5f, 170.0f + 0.5f) * 255.0f
                            + 0.5f), 255)
          << "the centre of the circle is not fully covered, so the bands missed part of the shape";
}




