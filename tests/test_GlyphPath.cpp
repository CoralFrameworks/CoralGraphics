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
 * See the License for the specific language governing permissions and limitations under the License.
 */

/**
 * @file test_GlyphPath.cpp
 *
 * Turning a glyph identifier into an outline.
 *
 * ## What is checked, and why these properties
 *
 * The exact coordinates of a glyph are not interesting to assert -- they are the font designer's, and a test that
 * pinned them would fail whenever the bundled font changed. What is interesting is the *shape of the answer*, and
 * these are the properties a broken outline extractor gets wrong while still producing something that looks like a
 * path:
 *
 *  - a letter has geometry and a space does not;
 *  - the coordinates are in font units, so their size is set by `unitsPerEm` and not by some default pixel size;
 *  - **y increases upwards**, the opposite of every drawing surface here, which is the convention most likely to be
 *    silently inverted;
 *  - a curve stays a curve, and a straight-edged letter stays straight.
 *
 * The font is the one bundled in the repository, found by walking up from the working directory, so the test does not
 * depend on what the host machine happens to have installed.
 */

#include <graphics/Path.h>
#include <graphics/Typeface.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>

using namespace graphics;

namespace {

/**
 * @return the typeface bundled with the repository, or null when it cannot be found.
 *
 * The path is walked upwards because the test runs from a build tree whose depth depends on the generator:
 * `build/rootcheck/CoralGraphics` and `build/gpucheck/graphics` are both used.
 */
std::shared_ptr<Typeface> bundledTypeface() {
  // Candidates in preference order. The vendored harfbuzz copy is first because it is tracked and is a full
  // font; the assets directory is tried second because those files are the application's bundled fonts.
  static const char* const candidates[] = {
    // The vendored Roboto: this repository's root and the application tree's framework checkout.
    "third_party/harfbuzz/perf/fonts/Roboto-Regular.ttf",
    "frameworks/CoralGraphics/third_party/harfbuzz/perf/fonts/Roboto-Regular.ttf",
    // The application tree's shared assets.
    "assets/fonts/Roboto-Medium.ttf",
    "assets/fonts/NotoSansSC-Regular.otf",
    "assets/fonts/NotoSerifSC-Regular.otf",
  };

  std::filesystem::path directory = std::filesystem::current_path();
#if defined(CORAL_GRAPHICS_RESOURCE_DIR)
  // A spawned iOS simulator process has no useful working directory, so the walk below would start at `/`
  // and never see the checkout. The build already knows where the resources are, and the repository root is
  // two levels above them; starting there makes the same search work on the simulator.
  if (!std::filesystem::exists(directory / "third_party")) {
    directory = std::filesystem::path(CORAL_GRAPHICS_RESOURCE_DIR).parent_path();
  }
#endif
  for (int depth = 0; depth < 8 && !directory.empty(); ++depth) {
    for (const char* const relative : candidates) {
      const std::filesystem::path candidate = directory / relative;
      if (std::filesystem::exists(candidate)) {
        if (auto typeface = Typeface::fromFile(candidate.string())) return typeface;
      }
    }
    directory = directory.parent_path();
  }
  return nullptr;
}

/** @return the outline of the first character of @p text, or an empty path when there is none. */
Path outlineOf(const std::shared_ptr<Typeface>& typeface, const char32_t codePoint) {
  return typeface->glyphPath(typeface->glyphForCodePoint(codePoint));
}

} // namespace anonymous

// ==================================================================================================
// A letter has geometry; a space does not
// ==================================================================================================

TEST(GlyphPath, ALetterHasAnOutline) {
  const auto typeface = bundledTypeface();
  ASSERT_NE(typeface, nullptr) << "the repository's bundled font must be findable from the test's working directory";

  const Path outline = outlineOf(typeface, U'o');
  EXPECT_FALSE(outline.isEmpty()) << "an 'o' has an outline";
  EXPECT_GT(outline.verbCount(), 2u) << "and it takes more than a couple of verbs to draw a ring";
  EXPECT_TRUE(outline.hasCurves()) << "a round letter contains curves";
}

TEST(GlyphPath, ASpaceHasNoOutline) {
  const auto typeface = bundledTypeface();
  ASSERT_NE(typeface, nullptr);

  // A space advances the pen and draws nothing. An empty path is the answer, not a failure -- and it is what a
  // renderer needs, because drawing "nothing" must not turn into drawing a box.
  EXPECT_TRUE(outlineOf(typeface, U' ').isEmpty()) << "a space has no outline to draw";
}

TEST(GlyphPath, AStraightLetterIsMadeOfLines) {
  const auto typeface = bundledTypeface();
  ASSERT_NE(typeface, nullptr);

  const Path outline = outlineOf(typeface, U'I');
  ASSERT_FALSE(outline.isEmpty());

  // An 'I' is a plain rectangle in this font, so it should contain no curve at all. This is the assertion that
  // catches a decomposer that routes every segment through the curve callback.
  EXPECT_FALSE(outline.hasCurves()) << "an 'I' has no curves";
  EXPECT_GE(outline.verbCount(), 4u) << "and it has straight edges";
}

// ==================================================================================================
// The coordinate conventions
// ==================================================================================================

TEST(GlyphPath, CoordinatesAreInFontUnits) {
  const auto typeface = bundledTypeface();
  ASSERT_NE(typeface, nullptr);

  const Path outline = outlineOf(typeface, U'H');
  ASSERT_FALSE(outline.isEmpty());

  // In font units an uppercase letter is a large fraction of `unitsPerEm` -- a thousand or two for a typical font --
  // and nowhere near the tens a caller would see if a default pixel size had been applied. The check is a range
  // rather than a value because the exact height is the designer's.
  const Rect bounds = outline.bounds();
  const float height = bounds.getBottom() - bounds.getTop();
  const float em = static_cast<float>(typeface->unitsPerEm());

  EXPECT_GT(height, em * 0.5f) << "an 'H' should be most of an em tall in font units";
  EXPECT_LT(height, em * 1.5f) << "and not several ems, which is what a size applied twice would give";
}

TEST(GlyphPath, YIncreasesUpwards) {
  const auto typeface = bundledTypeface();
  ASSERT_NE(typeface, nullptr);

  // ## The convention most likely to be silently inverted
  //
  // A font describes a glyph with y increasing upwards, and every drawing surface in this library uses the opposite.
  // A path built in the wrong one is not obviously broken -- it is upside down, which for a letter like 'b' or 'p'
  // would show as a different letter, and for a symmetric one would not show at all.
  //
  // An uppercase 'H' sits on the baseline and rises above it, so its *smallest* y is at or just below zero and its
  // largest y is most of an em above. In a y-down path those two would be swapped, and `bounds().getTop()` -- which
  // reports the smaller y regardless of which edge that is -- would be the baseline rather than the cap.
  const Path outline = outlineOf(typeface, U'H');
  ASSERT_FALSE(outline.isEmpty());

  const Rect bounds = outline.bounds();
  const float em = static_cast<float>(typeface->unitsPerEm());

  // In a y-up glyph the cap height is the larger y, so `getBottom()` -- the larger y, by this API's naming -- has to
  // be the one that is most of an em above the baseline.
  EXPECT_GT(bounds.getBottom(), em * 0.5f) << "the cap of an 'H' rises most of an em above the baseline";
  EXPECT_LT(std::fabs(bounds.getTop()), em * 0.2f) << "and its foot is at the baseline, so near zero";
}

TEST(GlyphPath, GlyphsSitOnTheBaseline) {
  const auto typeface = bundledTypeface();
  ASSERT_NE(typeface, nullptr);

  // Both letters rest on the same baseline, so their lower bounds must agree. This is what fails if the extraction
  // mixed up per-glyph origins -- a wrong origin shows as letters that do not line up when drawn together.
  const Path h = outlineOf(typeface, U'H');
  const Path o = outlineOf(typeface, U'o');
  ASSERT_FALSE(h.isEmpty());
  ASSERT_FALSE(o.isEmpty());

  const float em = static_cast<float>(typeface->unitsPerEm());

  // An 'o' overshoots the baseline slightly, because a round letter drawn exactly to the line looks as though it
  // floats. The tolerance allows for that overshoot rather than demanding equality.
  EXPECT_NEAR(o.bounds().getTop(), h.bounds().getTop(), em * 0.06f) << "both letters rest on the same baseline";
}

// ==================================================================================================
// What happens when there is nothing to draw
// ==================================================================================================

TEST(GlyphPath, AnUnknownGlyphIsEmptyRatherThanFatal) {
  const auto typeface = bundledTypeface();
  ASSERT_NE(typeface, nullptr);

  // A glyph identifier past the end of the face. The answer has to be an empty path rather than a crash or a
  // half-built one: a caller that asked for the wrong glyph should get nothing drawn, not undefined behaviour.
  EXPECT_TRUE(typeface->glyphPath(typeface->glyphCount() + 5000u).isEmpty())
          << "an identifier the face does not have yields nothing to draw";
}

TEST(GlyphPath, TheSameGlyphTwiceGivesTheSamePath) {
  const auto typeface = bundledTypeface();
  ASSERT_NE(typeface, nullptr);

  // The extraction loads the glyph into a FreeType slot shared by every caller, so a second call must produce the
  // same answer as the first. This catches state left behind by the previous call -- a stale transform, or a contour
  // not finished -- which would show as a path that depends on what was drawn before it.
  const uint32_t glyph = typeface->glyphForCodePoint(U'g');
  const Path first = typeface->glyphPath(glyph);
  const Path second = typeface->glyphPath(glyph);

  ASSERT_FALSE(first.isEmpty());
  EXPECT_EQ(first.verbCount(), second.verbCount());
  EXPECT_EQ(first.pointCount(), second.pointCount());
  EXPECT_EQ(first.bounds().getTop(), second.bounds().getTop());
  EXPECT_EQ(first.bounds().getBottom(), second.bounds().getBottom());
  EXPECT_EQ(first.bounds().getLeft(), second.bounds().getLeft());
  EXPECT_EQ(first.bounds().getRight(), second.bounds().getRight());
}

TEST(GlyphPath, ADifferentGlyphGivesADifferentPath) {
  // A sanity check on the whole exercise: if every glyph produced the same outline, the outline extraction would be
  // returning the face's first slot rather than the glyph that was asked for, and every assertion above would still
  // pass on the letters it happened to look at.
  const auto typeface = bundledTypeface();
  ASSERT_NE(typeface, nullptr);

  const Path h = outlineOf(typeface, U'H');
  const Path o = outlineOf(typeface, U'o');
  ASSERT_FALSE(h.isEmpty());
  ASSERT_FALSE(o.isEmpty());

  EXPECT_NE(h.bounds().getRight() - h.bounds().getLeft(), o.bounds().getRight() - o.bounds().getLeft())
          << "an 'H' and an 'o' are not the same width";
  EXPECT_NE(h.hasCurves(), o.hasCurves()) << "one is made of lines and the other of curves";
}
