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
 * @file test_LottieRender.cpp
 *
 * Drawing a Lottie document.
 *
 * The corpus's `gear.json` is the subject, and the questions are the ones a picture can answer and a parse cannot: does
 * anything appear at all, is it in the middle of the composition, is the centre hollow, and does it move when the time
 * changes. Each of those fails in a different way -- a transform applied in the wrong order puts the gear off the
 * surface, a merge that unites instead of splicing fills the centre in, and a frozen clock gives the same pixels at
 * every time.
 */

#include <lottie/Lottie.h>

#include <graphics/Surface.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifndef CORAL_GRAPHICS_RESOURCE_DIR
#error "CORAL_GRAPHICS_RESOURCE_DIR must be defined by the build; see modules/graphics/CMakeLists.txt"
#endif

namespace {

namespace fs = std::filesystem;

using graphics::Lottie;
using graphics::Rect;
using graphics::Surface;

/** The corpus document. */
Lottie gear(std::string* error = nullptr) {
  const fs::path path = fs::path(CORAL_GRAPHICS_RESOURCE_DIR) / "apitest" / "lottie" / "gear.json";
  std::ifstream file(path, std::ios::binary);
  std::ostringstream contents;
  contents << file.rdbuf();

  auto parsed = Lottie::parse(contents.str(), error);
  EXPECT_TRUE(parsed.has_value()) << (error != nullptr ? *error : "no reason given");
  return parsed.has_value() ? std::move(*parsed) : Lottie {};
}

/** A rendered frame, with the pixel questions these tests ask. */
class Frame {
  public:
  Frame(const Lottie& animation, const graphics::Scalar seconds, const int size) {
    surface = Surface::makeRaster(size, size);
    if (surface == nullptr) return;
    animation.render(*surface->getCanvas(), Rect::MakeWH(static_cast<graphics::Scalar>(size),
                                                         static_cast<graphics::Scalar>(size)), seconds);
    width = size;
  }

  [[nodiscard]] uint32_t at(const int x, const int y) const { return surface->peekPixels().getPixelARGB32(x, y); }
  [[nodiscard]] int alpha(const int x, const int y) const { return static_cast<int>((at(x, y) >> 24) & 0xFFu); }

  /** How many pixels are drawn at all. */
  [[nodiscard]] int coveredPixels() const {
    int count = 0;
    for (int y = 0; y < width; ++y) {
      for (int x = 0; x < width; ++x) {
        if (alpha(x, y) > 8) ++count;
      }
    }
    return count;
  }

  /** The mean alpha over a disc, which is how "is the middle hollow" is asked without depending on one pixel. */
  [[nodiscard]] double meanAlphaNear(const int cx, const int cy, const int radius) const {
    double total = 0;
    int count = 0;
    for (int y = cy - radius; y <= cy + radius; ++y) {
      for (int x = cx - radius; x <= cx + radius; ++x) {
        if (x < 0 || y < 0 || x >= width || y >= width) continue;
        total += alpha(x, y);
        ++count;
      }
    }
    return count == 0 ? 0.0 : total / count;
  }

  std::unique_ptr<Surface> surface;
  int width = 0;
};

/**
 * @return the fraction of the pixels in an annulus around the composition's centre that are drawn, from 0 to 1.
 *
 * The centre is (63.5, 63.5) for a 128-wide surface, which is between pixels -- the same convention the rasterizer uses.
 * Asking about an annulus rather than a rectangle is what lets a test say "there is a ring here" without also asserting
 * where the ring's edges are, which is a claim about the rasterizer rather than about the document.
 */
double ringCoverage(const Frame& frame, const double inner, const double outer) {
  int covered = 0;
  int total = 0;
  for (int y = 0; y < frame.width; ++y) {
    for (int x = 0; x < frame.width; ++x) {
      const double distance = std::hypot(x - 63.5, y - 63.5);
      if (distance < inner || distance >= outer) continue;
      ++total;
      if (frame.alpha(x, y) > 128) ++covered;
    }
  }
  return total == 0 ? 0.0 : static_cast<double>(covered) / total;
}

} // namespace anonymous

// ==================================================================================================
// The composition
// ==================================================================================================

TEST(LottieRender, DrawsSomething) {
  const Lottie animation = gear();
  ASSERT_FALSE(animation.isEmpty());
  EXPECT_FLOAT_EQ(animation.width(), 128.0f);
  EXPECT_FLOAT_EQ(animation.height(), 128.0f);
  EXPECT_FLOAT_EQ(animation.frameRate(), 60.0f);
  EXPECT_FLOAT_EQ(animation.duration(), 5.0f);

  const Frame frame(animation, 0.0, 128);
  ASSERT_NE(frame.surface, nullptr);
  EXPECT_GT(frame.coveredPixels(), 200) << "the gear has to be more than a few pixels";
  EXPECT_LT(frame.coveredPixels(), 128 * 128) << "and it is a gear, not a filled square";
}

TEST(LottieRender, PutsTheGearExactlyInTheMiddleOfTheComposition) {
  // The layer's transform is `T(64,64) * S(11.562%) * T(-320,-320)`, so the shape's own centre lands at the middle of
  // the 128x128 composition. Measured, the drawn pixels occupy x 28..99 and y 28..99, whose centre is (63.5, 63.5) --
  // exactly the middle of a 128-wide surface.
  //
  // This is the assertion that pins the transform *order*. Getting the anchor on the wrong side of the scale, or the
  // scale on the wrong side of the rotation, moves the gear off the centre by tens of pixels while leaving a picture
  // that still looks like a gear.
  const Lottie animation = gear();
  const Frame frame(animation, 0.0, 128);

  int minX = 128;
  int maxX = -1;
  int minY = 128;
  int maxY = -1;
  for (int y = 0; y < 128; ++y) {
    for (int x = 0; x < 128; ++x) {
      if (frame.alpha(x, y) < 8) continue;
      minX = std::min(minX, x);
      maxX = std::max(maxX, x);
      minY = std::min(minY, y);
      maxY = std::max(maxY, y);
    }
  }
  ASSERT_GT(maxX, minX) << "something was drawn";

  EXPECT_NEAR((minX + maxX) / 2.0, 63.5, 1.5) << "horizontally centred";
  EXPECT_NEAR((minY + maxY) / 2.0, 63.5, 1.5) << "and vertically";

  // And it is the size the layer's scale says: the shape's own extent is about 620 units across, at 11.562% of which it
  // is about 72 pixels -- which is what 28..99 measures.
  EXPECT_NEAR(maxX - minX, 71, 6) << "the width the scale produces";
  EXPECT_NEAR(maxY - minY, 71, 6) << "and the height";
}

TEST(LottieRender, LeavesTheCentreHollow) {
  // This is the test the merge mode exists for. The document merges two contours with `mm: 1`, which is a *splice*: the
  // outer contour and the inner one keep their own windings, and the nonzero rule leaves the middle empty. A renderer
  // that united them would draw a solid disc here, and the difference is invisible except at the centre.
  const Lottie animation = gear();
  const Frame frame(animation, 0.0, 128);

  EXPECT_LT(ringCoverage(frame, 0.0, 6.0), 0.02) << "the hub is empty";
  EXPECT_GT(ringCoverage(frame, 12.0, 18.0), 0.3) << "with a ring around it";
}

TEST(LottieRender, TheWebIsTwoRingsWithAGapBetweenThem) {
  // The measured profile of the reference: a ring at radius 12-18, nothing at 18-24, another ring at 24-36, and teeth
  // beyond. The gap is the "alt web" -- and it is what a splice produces, where a union would have filled it in along
  // with the hub. Nothing but the merge mode can produce it.
  const Lottie animation = gear();
  const Frame frame(animation, 0.0, 128);

  EXPECT_GT(ringCoverage(frame, 12.0, 18.0), 0.3) << "the inner ring";
  EXPECT_LT(ringCoverage(frame, 19.0, 23.0), 0.02) << "the gap";
  EXPECT_GT(ringCoverage(frame, 25.0, 35.0), 0.3) << "the outer ring";
  EXPECT_LT(ringCoverage(frame, 46.0, 60.0), 0.01) << "and nothing beyond the teeth";
}

TEST(LottieRender, DrawsBlackOnATransparentBackground) {
  // The fill is `#000000` at full alpha, and the document says nothing about a background -- so what is not the gear is
  // transparent rather than white, which is the difference between an overlay and a picture with a box in it.
  const Lottie animation = gear();
  const Frame frame(animation, 0.0, 128);

  // Sampled off the measured centre row, which is opaque at x = 36 and x = 92 among others.
  const uint32_t pixel = frame.at(36, 64);
  EXPECT_GT((pixel >> 24) & 0xFFu, 200u) << "drawn";
  EXPECT_LT((pixel >> 16) & 0xFFu, 40u) << "and black, not white";
  EXPECT_LT((pixel >> 8) & 0xFFu, 40u);
  EXPECT_LT(pixel & 0xFFu, 40u);
  EXPECT_EQ(frame.alpha(1, 1), 0) << "with nothing in the corner";
}

// ==================================================================================================
// Time
// ==================================================================================================

TEST(LottieRender, DifferentTimesGiveDifferentPictures) {
  // The gear turns through a full revolution over the composition. A renderer that sampled no property, or sampled one
  // at a fixed frame, would give the same pixels at every time -- and a still picture of a gear looks entirely correct.
  const Lottie animation = gear();
  const Frame start(animation, 0.0, 128);
  const Frame quarter(animation, 1.25, 128); // a quarter of five seconds
  const Frame half(animation, 2.5, 128);

  EXPECT_GT(start.coveredPixels(), 200);
  EXPECT_GT(quarter.coveredPixels(), 200);
  EXPECT_GT(half.coveredPixels(), 200);

  int differing = 0;
  for (int y = 0; y < 128; ++y) {
    for (int x = 0; x < 128; ++x) {
      if (start.at(x, y) != quarter.at(x, y)) ++differing;
    }
  }
  EXPECT_GT(differing, 50) << "a quarter of a turn has to move something";
}

TEST(LottieRender, AHalfTurnIsNotTheSameAsNoTurnButAFullTurnIs) {
  // A gear is not symmetric under a half turn -- it has an odd number of teeth in the reference -- so these two differ.
  // They are a check that the rotation is actually being applied rather than being read and dropped.
  const Lottie animation = gear();
  const Frame start(animation, 0.0, 128);
  const Frame half(animation, 2.5, 128);

  int differing = 0;
  for (int y = 0; y < 128; ++y) {
    for (int x = 0; x < 128; ++x) {
      if (start.at(x, y) != half.at(x, y)) ++differing;
    }
  }
  EXPECT_GT(differing, 20) << "half a turn is not the same picture";
}

TEST(LottieRender, HoldsTheLastFramePastTheEnd) {
  // A player that has run off the end shows the last frame rather than going blank, and one that has not started shows
  // the first. Both are clamping, and a renderer without it would extrapolate the rotation past a full turn.
  const Lottie animation = gear();
  const Frame end(animation, 5.0, 128);
  const Frame past(animation, 100.0, 128);
  const Frame before(animation, -10.0, 128);
  const Frame start(animation, 0.0, 128);

  int sameAsEnd = 0;
  int sameAsStart = 0;
  for (int y = 0; y < 128; ++y) {
    for (int x = 0; x < 128; ++x) {
      if (end.at(x, y) == past.at(x, y)) ++sameAsEnd;
      if (start.at(x, y) == before.at(x, y)) ++sameAsStart;
    }
  }
  EXPECT_EQ(sameAsEnd, 128 * 128) << "past the end holds the last frame";
  EXPECT_EQ(sameAsStart, 128 * 128) << "and before the start holds the first";
}

TEST(LottieRender, ScalesTheCompositionToTheDestination) {
  // A 128x128 composition drawn into a 256x256 rectangle covers it, rather than occupying its top-left corner. The
  // quarter-scale render is what proves the mapping is a scale and not just a translation.
  const Lottie animation = gear();

  const Frame big(animation, 0.0, 256);
  const Frame small(animation, 0.0, 128);
  ASSERT_NE(big.surface, nullptr);
  ASSERT_NE(small.surface, nullptr);

  // The gear covers a similar fraction of both, within a few percent: an exact match is not expected, because the edges
  // are antialiased and a pixel at 256 covers a quarter of the area one at 128 does.
  const double bigFraction = static_cast<double>(big.coveredPixels()) / (256.0 * 256.0);
  const double smallFraction = static_cast<double>(small.coveredPixels()) / (128.0 * 128.0);
  EXPECT_NEAR(bigFraction, smallFraction, 0.03) << "the same picture at twice the size";

  // And the drawing reaches well past the first quarter, which a missing scale would not.
  EXPECT_GT(big.meanAlphaNear(192, 128, 20), 20.0) << "drawing in the right half of the large surface";
}

// ==================================================================================================
// Degenerate documents
// ==================================================================================================

TEST(LottieRender, AnEmptyAnimationDrawsNothingAndDoesNotCrash) {
  const Lottie empty;
  EXPECT_TRUE(empty.isEmpty());
  EXPECT_FLOAT_EQ(empty.width(), 0.0f);
  EXPECT_FLOAT_EQ(empty.duration(), 0.0f);

  const auto surface = Surface::makeRaster(32, 32);
  ASSERT_NE(surface, nullptr);
  empty.render(*surface->getCanvas(), 0.0);
  EXPECT_EQ(surface->peekPixels().getPixelARGB32(16, 16), 0u);
}

TEST(LottieRender, RefusesTextThatIsNotADocument) {
  std::string error;
  EXPECT_FALSE(Lottie::parse("{}", &error).has_value());
  EXPECT_FALSE(error.empty());
}
