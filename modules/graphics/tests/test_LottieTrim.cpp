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
 * @file test_LottieTrim.cpp
 *
 * Lottie's trim path effect.
 *
 * Two halves, as with the gradient and matte work: the arithmetic is asserted directly through `trimPath` on paths whose
 * lengths are known exactly, and the feature is asserted end to end through documents. The first half is where the
 * subtleties live -- the offset's unit, the wrap, the inverted complement -- and the second is where they become visible.
 *
 * The three assets are Skia's own. `trim_modes.json` is the pair that exists to show `m: 1` against `m: 2`, which is the
 * one place the format's naming runs opposite to what the renderer does.
 */

#include "lottie/LottieModel.h"
#include "lottie/LottieTrim.h"

#include <lottie/Lottie.h>

#include <graphics/Path.h>
#include <graphics/PathMeasure.h>
#include <graphics/Pixmap.h>
#include <graphics/Surface.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifndef CORAL_GRAPHICS_RESOURCE_DIR
#error "CORAL_GRAPHICS_RESOURCE_DIR must be defined by the build; see modules/graphics/CMakeLists.txt"
#endif

namespace {

namespace fs = std::filesystem;

using graphics::Lottie;
using graphics::Path;
using graphics::PathMeasure;
using graphics::Point;
using graphics::Rect;
using graphics::Scalar;
using graphics::Surface;
using graphics::lottie::Animation;
using graphics::lottie::Layer;
using graphics::lottie::parseAnimation;
using graphics::lottie::trimPath;

fs::path assetPath(const char* name) {
  return fs::path(CORAL_GRAPHICS_RESOURCE_DIR) / "apitest" / "lottie" / name;
}

std::string readAsset(const char* name) {
  std::ifstream file(assetPath(name), std::ios::binary);
  if (!file.good()) return {};
  std::ostringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

/** An open horizontal line @p length long. Its own length is exactly @p length, which makes the arithmetic checkable. */
Path line(const Scalar length) {
  Path path;
  path.moveTo(Point {0, 0});
  path.lineTo(Point {length, 0});
  return path;
}

/** A closed square of side @p side, wound clockwise, with a perimeter of exactly four sides. */
Path square(const Scalar side) {
  Path path;
  path.moveTo(Point {0, 0});
  path.lineTo(Point {side, 0});
  path.lineTo(Point {side, side});
  path.lineTo(Point {0, side});
  path.close();
  return path;
}

// ==================================================================================================
// Documents written here, where the geometry, the trim and the paint are all in one group
// ==================================================================================================

/** An open horizontal line from (@p x0, @p y) to (@p x1, @p y), as `sh` writes one. */
std::string lineShape(const int x0, const int x1, const int y) {
  return R"({"ty":"sh","ks":{"a":0,"k":{"c":false,"v":[[)" + std::to_string(x0) + "," + std::to_string(y) + "],[" +
         std::to_string(x1) + "," + std::to_string(y) + R"(]],"i":[[0,0],[0,0]],"o":[[0,0],[0,0]]}}})";
}

/** A stroke of @p width, which is what turns a path into something with pixels to count. */
std::string stroke(const int width) {
  // Two closing braces: one for `{"a":0,"k":width}` and one for the item itself. The first version had one.
  return R"({"ty":"st","c":{"a":0,"k":[1,0,0,1]},"o":{"a":0,"k":100},"w":{"a":0,"k":)" + std::to_string(width) + "}}";
}

/** A trim. @p mode is `m`: 1 trims each shape apart, 2 joins them first. */
std::string trim(const int start, const int end, const int offset, const int mode) {
  return R"({"ty":"tm","s":{"a":0,"k":)" + std::to_string(start) + R"(},"e":{"a":0,"k":)" + std::to_string(end) +
         R"(},"o":{"a":0,"k":)" + std::to_string(offset) + R"(},"m":)" + std::to_string(mode) + "}";
}

/** A shape layer holding @p items in one group, which is where a paint has to be to see the geometry. */
std::string shapeLayer(const std::string& items) {
  return R"({"ty":4,"ind":1,"ip":0,"op":60,"st":0,"ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},)"
         R"("p":{"a":0,"k":[0,0,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},)"
         R"("shapes":[{"ty":"gr","it":[)" +
         items + R"(]}]})";
}

/** A 100x100 composition. The surface is the same size, so a coordinate is a pixel. */
std::string composition(const std::string& layers) {
  return R"({"v":"5.5.2","fr":60,"ip":0,"op":60,"w":100,"h":100,"nm":"test","ddd":0,"layers":[)" + layers + R"(]})";
}

/** A document holding one line stroked, trimmed from 0 to @p end percent. */
std::string trimmedLine(const int end, const int offset = 0, const int mode = 1) {
  return composition(shapeLayer(lineShape(0, 100, 50) + "," + trim(0, end, offset, mode) + "," + stroke(20)));
}

/** A model of an asset, for the structural assertions. */
class Model {
  public:
  explicit Model(const char* name) {
    const std::string text = readAsset(name);
    if (text.empty()) {
      error = "could not read the asset";
      return;
    }
    auto parsed = parseAnimation(text, &error);
    if (!parsed) return;
    animation = std::move(*parsed);
    loaded = true;
  }

  std::string error;
  bool loaded = false;
  std::optional<Animation> animation;
};

/** A rendered frame, fitted to the surface. */
class Rendered {
  public:
  /** Renders the asset of that name. */
  Rendered(const char* name, const Scalar seconds, const int size = 200) {
    const std::string text = readAsset(name);
    if (text.empty()) {
      error = "could not read the asset";
      return;
    }
    load(text, seconds, size);
  }

  /** Renders a document written in the test, where a coordinate can be a pixel and the assertion is about arithmetic. */
  Rendered(const std::string& document, const Scalar seconds, const int size = 200) { load(document, seconds, size); }

  void load(const std::string& text, const Scalar seconds, const int size) {
    auto parsed = Lottie::parse(text, &error);
    if (!parsed) return;
    lottie = std::move(*parsed);
    loaded = true;
    draw(seconds, size);
  }

  void draw(const Scalar seconds, const int size = 200) {
    surface = Surface::makeRaster(size, size);
    if (surface == nullptr) return;
    const auto extent = static_cast<Scalar>(size);
    lottie->render(*surface->getCanvas(), Rect::MakeLTRB(0, 0, extent, extent), seconds);
    rendered = true;
  }

  [[nodiscard]] uint32_t at(const int x, const int y) const { return surface->peekPixels().getPixelARGB32(x, y); }
  [[nodiscard]] int alpha(const int x, const int y) const { return static_cast<int>((at(x, y) >> 24) & 0xFFu); }

  [[nodiscard]] int coveredPixels() const {
    int count = 0;
    const auto pixels = surface->peekPixels();
    for (int y = 0; y < pixels.height(); ++y) {
      for (int x = 0; x < pixels.width(); ++x) {
        if (((pixels.getPixelARGB32(x, y) >> 24) & 0xFFu) > 8) ++count;
      }
    }
    return count;
  }

  std::string error;
  bool loaded = false;
  bool rendered = false;
  std::optional<Lottie> lottie;
  std::unique_ptr<Surface> surface;
};

} // namespace anonymous

// ==================================================================================================
// The arithmetic, on a path whose length is known exactly
// ==================================================================================================

TEST(LottieTrim, KeepsTheRangeBetweenTheTwoEnds) {
  // A 100-long line trimmed to 25%..75% keeps the middle fifty. This is the base case everything else modifies.
  const Path trimmed = trimPath(line(100), 25, 75, 0);
  ASSERT_FALSE(trimmed.isEmpty());
  EXPECT_NEAR(trimmed.length(), 50.0f, 0.2f);
  EXPECT_NEAR(trimmed.bounds().getLeft(), 25.0f, 0.2f) << "starting a quarter of the way along";
  EXPECT_NEAR(trimmed.bounds().getRight(), 75.0f, 0.2f) << "and ending three quarters of the way";
}

TEST(LottieTrim, TakesTheTwoEndsInEitherOrder) {
  // A user dragging a handle past the other one should not reverse the trim. The ends are taken as a range, so 75..25
  // is the same as 25..75.
  const Path forwards = trimPath(line(100), 25, 75, 0);
  const Path backwards = trimPath(line(100), 75, 25, 0);
  ASSERT_FALSE(forwards.isEmpty());
  ASSERT_FALSE(backwards.isEmpty());
  EXPECT_NEAR(forwards.length(), backwards.length(), 0.01f);
  EXPECT_NEAR(forwards.bounds().getLeft(), backwards.bounds().getLeft(), 0.01f);
}

TEST(LottieTrim, TheOffsetIsInDegreesRatherThanPercent) {
  // The format's oddity and the one arithmetic mistake that would be invisible in the middle of a range: `o` is degrees,
  // so 360 is the whole path and 90 is a quarter of it. Dividing by 100 instead would make every offset 3.6 times too
  // large -- an offset of 90 would move the range most of the way round instead of a quarter.
  const Path quarterTurn = trimPath(line(100), 0, 50, 90);
  ASSERT_FALSE(quarterTurn.isEmpty());
  EXPECT_NEAR(quarterTurn.bounds().getLeft(), 25.0f, 0.2f) << "a quarter of the way along";
  EXPECT_NEAR(quarterTurn.bounds().getRight(), 75.0f, 0.2f) << "and still fifty long";

  const Path fullTurn = trimPath(line(100), 0, 50, 360);
  ASSERT_FALSE(fullTurn.isEmpty());
  EXPECT_NEAR(fullTurn.bounds().getLeft(), 0.0f, 0.2f) << "a whole turn is no offset at all";
}

TEST(LottieTrim, WrapsAnOffsetThatRunsOffTheEnd) {
  // An offset of 180 degrees moves the range half way along a 100-long line, so 0..50 becomes 50..100 and, sliding
  // further, wraps back to the start. A version that clamped instead of wrapping would pile the range up at the end.
  const Path wrapped = trimPath(line(100), 0, 25, 270);
  ASSERT_FALSE(wrapped.isEmpty());
  EXPECT_NEAR(wrapped.length(), 25.0f, 0.2f) << "still a quarter of the line";
  EXPECT_NEAR(wrapped.bounds().getLeft(), 75.0f, 0.3f) << "the last quarter, reached by wrapping";
}

TEST(LottieTrim, AWholePathIsReturnedUnchanged) {
  // 0..100 with no offset keeps everything. Returned as the *source* rather than rebuilt, which matters for a curved
  // path: a walk would replace its curves with a flattened approximation.
  const Path source = line(100);
  const Path trimmed = trimPath(source, 0, 100, 0);
  EXPECT_EQ(trimmed.bounds().getLeft(), source.bounds().getLeft());
  EXPECT_EQ(trimmed.bounds().getRight(), source.bounds().getRight());

  // More than a whole path is the same answer, from the other side.
  const Path more = trimPath(source, 0, 250, 0);
  EXPECT_NEAR(more.length(), source.length(), 0.01f);
}

TEST(LottieTrim, AnEmptyRangeKeepsNothing) {
  // A trim animated from nothing to everything passes through this state, so it is an ordinary answer rather than an
  // error: an empty path, which the renderer draws as nothing.
  EXPECT_TRUE(trimPath(line(100), 40, 40, 0).isEmpty());
  EXPECT_TRUE(trimPath(line(100), 0, 0, 0).isEmpty());
}

TEST(LottieTrim, KeepsTheComplementWhenTheRangeWrapsPastTheStart) {
  // The inverted mode. On a closed square of side 40 (perimeter 160), a range from 75% to 25% with no offset becomes
  // 0.75..1.25, which wraps to 0.75..1 then 0..0.25 -- and because the start lands *before* the end, what is kept is
  // everything outside: 0.25..0.75, the middle half.
  const Path trimmed = trimPath(square(40), 75, 25, 0);
  ASSERT_FALSE(trimmed.isEmpty());
  EXPECT_NEAR(trimmed.length(), 80.0f, 1.0f) << "half the perimeter";
}

TEST(LottieTrim, TrimsAClosedContourAcrossItsClosingEdge) {
  // A square is written as four vertices and a close, so its perimeter includes an edge that no vertex pair states. A
  // range that reaches into it exercises the arithmetic that a path of only lines never would: 90%..100% of 160 is the
  // last sixteen units, which run up the left edge and across the closing edge back to the origin.
  const Path trimmed = trimPath(square(40), 90, 100, 0);
  ASSERT_FALSE(trimmed.isEmpty());
  EXPECT_NEAR(trimmed.length(), 16.0f, 0.5f);

  // The closing edge runs from vertex (0, 40) back to vertex (0, 0). Its cumulative span is 120..160, so 144..160 is the
  // last 60% of it: the walk starts at y = 40 + (0 - 40) * 0.6 = 16 and finishes at y = 0. Both endpoints written down as
  // coordinates, because describing it as "from the top downwards" is what put the two the wrong way round in the first
  // version of this test -- and in the `getSegment` test earlier in this round.
  EXPECT_NEAR(trimmed.bounds().getLeft(), 0.0f, 0.5f) << "along the left edge";
  EXPECT_NEAR(trimmed.bounds().getRight(), 0.0f, 0.5f) << "which is vertical";
  EXPECT_NEAR(trimmed.bounds().getTop(), 0.0f, 0.5f) << "ending at the origin";
  EXPECT_NEAR(trimmed.bounds().getBottom(), 16.0f, 0.5f) << "and starting sixteen units up from it";
}

TEST(LottieTrim, TrimsEachContourOfAMultiContourPath) {
  // Two separate lines joined into one path, total length 200. The middle half is the second half of the first line and
  // the first half of the second, so the result has two pieces -- and the distances are cumulative across them, which is
  // the whole reason the walk tracks where each contour starts.
  Path joined;
  joined.appendPath(line(100));
  Path second;
  second.moveTo(Point {0, 50});
  second.lineTo(Point {100, 50});
  joined.appendPath(second);

  const auto measure = PathMeasure::create(joined);
  ASSERT_NE(measure, nullptr);
  ASSERT_NEAR(measure->totalLength(), 200.0f, 0.5f);

  const Path trimmed = trimPath(joined, 25, 75, 0);
  ASSERT_FALSE(trimmed.isEmpty());
  EXPECT_NEAR(trimmed.length(), 100.0f, 1.0f) << "half of two hundred";
  // Both lines contribute, so the result spans both of their y positions.
  EXPECT_FLOAT_EQ(trimmed.bounds().getTop(), 0.0f) << "the first line's y";
  EXPECT_FLOAT_EQ(trimmed.bounds().getBottom(), 50.0f) << "and the second's";
}

// ==================================================================================================
// The assets: what they hold
// ==================================================================================================

TEST(LottieTrim, ParsesTheTrimModeAsset) {
  // The pair that exists to show the two `m` values. One trim is simultaneous and the other individual, and they are
  // the only difference between the two layers -- so reading `m` the wrong way round swaps which is which.
  const Model model("trim_modes.json");
  ASSERT_TRUE(model.loaded) << model.error;

  std::vector<bool> trimsAsOnePath;
  for (const Layer& layer : model.animation->layers) {
    for (const auto& group : layer.shapes) {
      for (const auto& shape : group->items) {
        if (shape->trim) trimsAsOnePath.push_back(shape->trim->trimsAsOnePath);
      }
    }
  }

  ASSERT_EQ(trimsAsOnePath.size(), 2U) << "one trim per layer";
  EXPECT_NE(trimsAsOnePath[0], trimsAsOnePath[1])
          << "the two layers have opposite modes, which is the point of the asset";
}

TEST(LottieTrim, ReadsTheThreeValuesAndDefaultsTheRest) {
  const Model model("trim_modes.json");
  ASSERT_TRUE(model.loaded) << model.error;

  bool found = false;
  for (const Layer& layer : model.animation->layers) {
    for (const auto& group : layer.shapes) {
      for (const auto& shape : group->items) {
        if (!shape->trim) continue;
        const auto& trim = *shape->trim;
        EXPECT_FALSE(trim.end.isEmpty()) << "the asset animates `e`";
        EXPECT_FALSE(trim.start.isEmpty());
        EXPECT_FALSE(trim.offset.isEmpty());
        found = true;
      }
    }
  }
  EXPECT_TRUE(found);
}

// ==================================================================================================
// The assets, drawn
// ==================================================================================================

TEST(LottieTrim, DrawsTheTrimModeAsset) {
  // A stroke trimmed to a range draws *some* of its outline. Before this feature the trim was kept as an unknown item
  // and the whole outline was drawn, so counting covered pixels is what tells the two apart: a trimmed stroke covers
  // strictly less than an untrimmed one.
  const Rendered trimmed("trim_modes.json", 5.0f, 200);
  ASSERT_TRUE(trimmed.loaded) << trimmed.error;
  ASSERT_TRUE(trimmed.rendered);
  EXPECT_GT(trimmed.coveredPixels(), 0) << "it draws something";
}

TEST(LottieTrim, TheModeAssetDrawsOnlyItsBackground) {
  // Recorded rather than asserted away, because the reason is the useful part. `trim_modes.json` holds each layer's
  // three shapes and its trim **inside a group**, and the stroke that would paint them as a **sibling of that group at
  // the layer level**. Geometry does not escape a group -- Skia's `ShapeLayer.cpp` passes the parent's geometry vector
  // into the recursive call, but never reads it, and declares `geos` locally on every call -- so the stroke finds an
  // empty stack and draws nothing. The layer's white solid is all there is.
  //
  // This fails the day either the scoping or the asset changes, which is the point of writing it down.
  const Rendered rendered("trim_modes.json", 0.0f, 200);
  ASSERT_TRUE(rendered.loaded) << rendered.error;
  ASSERT_TRUE(rendered.rendered);

  const auto pixels = rendered.surface->peekPixels();
  int opaque = 0;
  for (int y = 0; y < pixels.height(); ++y) {
    for (int x = 0; x < pixels.width(); ++x) {
      if (((pixels.getPixelARGB32(x, y) >> 24) & 0xFFu) > 250) ++opaque;
    }
  }
  EXPECT_EQ(opaque, 200 * 200) << "the background solid covers everything and nothing is drawn over it";
  EXPECT_EQ(rendered.coveredPixels(), 200 * 200) << "so coverage is the background's and cannot track the trim";
}

TEST(LottieTrim, ATrimmedStrokeIsShorterThanTheUntrimmedOne) {
  // A hundred-long line stroked twenty wide. Untrimmed it covers a band two thousand pixels long; trimmed to half it
  // covers half of that. This is the assertion that fails if the trim is ignored, which is what the feature's absence
  // looked like: the whole stroke drawn at every time.
  // Time zero, a 100x100 surface: the composition is the same size, so one unit is one pixel and the expected
  // coverage is arithmetic rather than a guess.
  const Rendered whole(trimmedLine(100), 0, 100);
  const Rendered half(trimmedLine(50), 0, 100);
  ASSERT_TRUE(whole.loaded) << whole.error;
  ASSERT_TRUE(half.loaded) << half.error;

  EXPECT_NEAR(whole.coveredPixels(), 100 * 20, 60) << "the whole band";

  // The half-length is compared against *half of the whole* rather than against 1,000, because a trimmed stroke has a
  // free end where an untrimmed one has only the two at the surface edge. This library's default cap extends half the
  // stroke width past the last point, so the trimmed end adds about `20/2 * 20 = 200` pixels that the band alone does
  // not account for -- measured at 166, which is that figure within the antialiasing at the cap.
  EXPECT_NEAR(half.coveredPixels(), whole.coveredPixels() / 2, 250)
          << "about half the ink, plus the cap on the end the trim created";
  EXPECT_LT(half.coveredPixels(), whole.coveredPixels()) << "and strictly less than the whole";
}

TEST(LottieTrim, KeepsTheStartOfThePathAndDiscardsTheEnd) {
  // Which end survives is as much a part of the effect as how much does. Trimming to the first quarter keeps x from 0
  // to 25 and leaves the rest empty.
  const Rendered quarter(trimmedLine(25), 0, 100);
  ASSERT_TRUE(quarter.loaded) << quarter.error;

  EXPECT_GT(quarter.alpha(10, 50), 200) << "ink in the first quarter";
  EXPECT_EQ(quarter.alpha(50, 50), 0) << "and none in the middle";
  EXPECT_EQ(quarter.alpha(90, 50), 0) << "and none at the far end";
}

TEST(LottieTrim, TheOffsetSlidesTheKeptRange) {
  // An offset of 90 degrees is a quarter of the path, so a range that would sit at 0..25 moves to 25..50. This is the
  // assertion that pins the offset's *unit*: dividing by 100 instead of 360 would move it to 0.9..1.15, which wraps to
  // the very end of the line rather than a quarter of the way along it.
  const Rendered offset(trimmedLine(25, 90), 0, 100);
  ASSERT_TRUE(offset.loaded) << offset.error;

  EXPECT_EQ(offset.alpha(10, 50), 0) << "nothing where the range used to be";
  EXPECT_GT(offset.alpha(35, 50), 200) << "and ink a quarter of the way along, where it moved to";
  EXPECT_EQ(offset.alpha(60, 50), 0) << "but no further";
}

TEST(LottieTrim, TheTwoModesDifferOnASecondShape) {
  // The `m` flag, and the only assertion in this file that separates the two values. Two parallel lines of a hundred
  // each, trimmed to the first half:
  //
  //   * `m: 1` trims each line on its own, so both lines are half drawn;
  //   * `m: 2` joins them into one two-hundred-long path and trims *that*, so the half drawn is all of the first line
  //     and none of the second.
  //
  // The two produce the *same total* coverage, which is why counting pixels would not tell them apart. Where the ink is
  // does.
  const std::string both = lineShape(0, 100, 30) + "," + lineShape(0, 100, 70) + ",";
  const Rendered apart(composition(shapeLayer(both + trim(0, 50, 0, 1) + "," + stroke(20))), 0, 100);
  const Rendered joined(composition(shapeLayer(both + trim(0, 50, 0, 2) + "," + stroke(20))), 0, 100);
  ASSERT_TRUE(apart.loaded) << apart.error;
  ASSERT_TRUE(joined.loaded) << joined.error;

  // Trimmed apart: both lines have their first half.
  EXPECT_GT(apart.alpha(25, 30), 200) << "the first line's first half";
  EXPECT_EQ(apart.alpha(75, 30), 0) << "and not its second";
  EXPECT_GT(apart.alpha(25, 70), 200) << "the second line's first half as well";
  EXPECT_EQ(apart.alpha(75, 70), 0);

  // Trimmed as one path: the whole first line, and none of the second.
  EXPECT_GT(joined.alpha(25, 30), 200) << "the first line, all of it";
  EXPECT_GT(joined.alpha(75, 30), 200) << "including its second half";
  EXPECT_EQ(joined.alpha(25, 70), 0) << "and nothing of the second line at all";
  EXPECT_EQ(joined.alpha(75, 70), 0);

  // The totals are *not* equal, and the reason is the caps rather than the trim. Trimming each line apart leaves four
  // free ends and joining them leaves two, so the difference is exactly twice one cap -- measured at 332 against the 166
  // above, which is the same figure once more. The length of ink is what matches; the caps are what does not, and
  // asserting equality here was asserting something the format does not do.
  EXPECT_NEAR(apart.coveredPixels() - joined.coveredPixels(), 332, 120)
          << "two extra free ends, so about two caps' worth more ink";
}

TEST(LottieTrim, DrawsTheTrimFillAsset) {
  // `trim_fill.json` is four groups of strokes trimmed at once, over a filled shape. It is the larger asset and it
  // exists to show trims on several shapes at the same time, which is where the two modes differ.
  const Rendered rendered("trim_fill.json", 3.0f, 200);
  ASSERT_TRUE(rendered.loaded) << rendered.error;
  ASSERT_TRUE(rendered.rendered);
  EXPECT_GT(rendered.coveredPixels(), 0) << "it draws";
}

TEST(LottieTrim, DrawsTheSampleSearchAsset) {
  // A single `sh` path with a trim whose start animates. The only one of the three whose geometry is a path rather than
  // a rectangle, an ellipse or a star.
  const Rendered rendered("trim_sample.json", 0.5f, 200);
  ASSERT_TRUE(rendered.loaded) << rendered.error;
  ASSERT_TRUE(rendered.rendered);
  EXPECT_GT(rendered.coveredPixels(), 0) << "it draws";
}

// ==================================================================================================
// The asset that was already there
// ==================================================================================================


TEST(LottieTrim, StillPlaysTheGearAnimation) {
  // The regression guard: the asset the PC demo plays must be untouched by any of this.
  const Rendered rendered("gear.json", 0.0f, 200);
  ASSERT_TRUE(rendered.loaded) << rendered.error;
  EXPECT_GT(rendered.coveredPixels(), 100);
}
