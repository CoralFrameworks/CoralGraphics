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
 * @file test_SvgPattern.cpp
 *
 * SVG `pattern`, from the element to the pixels.
 *
 * The tests are written in two halves on purpose. The first half uses documents written here, where the tile rectangle
 * and the content are both in user space -- the unambiguous case -- and asserts what a pattern should look like. The
 * second half measures what the *corpus* produces, which is a different thing: nine of its ten patterns declare
 * `patternContentUnits="objectBoundingBox"` while writing their content in user-space numbers, so what the specification
 * says they mean is not what their author meant. Both halves are needed: one shows the feature works, the other records
 * what it does to the documents in front of it.
 */

#include <svg/Svg.h>

#include <graphics/Pixmap.h>
#include <graphics/Surface.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef CORAL_GRAPHICS_RESOURCE_DIR
#error "CORAL_GRAPHICS_RESOURCE_DIR must be defined by the build; see modules/graphics/CMakeLists.txt"
#endif

namespace {

namespace fs = std::filesystem;

using graphics::Pixmap;
using graphics::Rect;
using graphics::Surface;
using graphics::Svg;

class Rendered {
  public:
  Rendered(const std::string& document, const int width = 64, const int height = 64) {
    surface = Surface::makeRaster(width, height);
    if (surface == nullptr) return;
    auto parsed = Svg::parse(document, &error);
    if (!parsed) return;
    document_ = std::move(*parsed);
    document_->render(*surface->getCanvas());
    drawn = true;
  }

  [[nodiscard]] uint32_t at(const int x, const int y) const { return surface->peekPixels().getPixelARGB32(x, y); }
  [[nodiscard]] int alpha(const int x, const int y) const { return static_cast<int>((at(x, y) >> 24) & 0xFFu); }
  [[nodiscard]] int red(const int x, const int y) const { return static_cast<int>((at(x, y) >> 16) & 0xFFu); }

  [[nodiscard]] int coveredPixels() const {
    int count = 0;
    for (int y = 0; y < surface->height(); ++y) {
      for (int x = 0; x < surface->width(); ++x) {
        if (alpha(x, y) > 8) ++count;
      }
    }
    return count;
  }

  std::string error;
  bool drawn = false;
  std::unique_ptr<Surface> surface;
  std::optional<Svg> document_;
};

std::string document(const std::string& body, const int width = 64, const int height = 64) {
  return "<svg width=\"" + std::to_string(width) + "\" height=\"" + std::to_string(height) + "\">" + body + "</svg>";
}

/**
 * A pattern whose tile is a small two-tone square, both unit systems in user space so the meaning is unambiguous.
 *
 * The content is a black square with a white square in one corner, which is asymmetric -- so a tile that is drawn
 * mirrored, or rotated, or off by a period, is visible rather than looking like the same pattern.
 */
const char* kCheckerPattern = R"svg(
  <defs><pattern id="p" patternUnits="userSpaceOnUse" patternContentUnits="userSpaceOnUse"
                 x="0" y="0" width="8" height="8">
    <rect x="0" y="0" width="8" height="8" fill="black"/>
    <rect x="0" y="0" width="4" height="4" fill="white"/>
  </pattern></defs>
  <rect width="64" height="64" fill="url(#p)"/>)svg";

} // namespace anonymous

// ==================================================================================================
// A pattern that means what it says
// ==================================================================================================

TEST(SvgPattern, FillsAShapeWithARepeatedTile) {
  const Rendered rendered(document(kCheckerPattern));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_EQ(rendered.coveredPixels(), 64 * 64) << "the whole rectangle is painted";

  // The tile is an 8x8 square with a white 4x4 corner. So the document is white at (2,2), black at (6,2) and (2,6), and
  // white again one period along at (10,2).
  EXPECT_GT(rendered.red(2, 2), 231) << "the white corner of the first tile";
  EXPECT_LT(rendered.red(6, 2), 24) << "the black part of it";
  EXPECT_LT(rendered.red(2, 6), 24);
  EXPECT_GT(rendered.red(10, 2), 231) << "and the white corner of the next tile along";
  EXPECT_LT(rendered.red(14, 2), 24) << "with its black part too";
}

TEST(SvgPattern, RepeatsInBothDirections) {
  const Rendered rendered(document(kCheckerPattern));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  // Three periods along and down, sampled at the white corner of each.
  for (const int x : {2, 10, 18, 26}) {
    EXPECT_GT(rendered.red(x, 2), 231) << "white at x = " << x;
    EXPECT_GT(rendered.red(2, x), 231) << "and at y = " << x;
  }
}

TEST(SvgPattern, PlacesTheTileWhereThePatternSays) {
  // A tile offset by four units moves the whole pattern, so the white corner is no longer at the origin.
  const Rendered rendered(document(R"svg(
      <defs><pattern id="p" patternUnits="userSpaceOnUse" patternContentUnits="userSpaceOnUse"
                     x="4" y="4" width="8" height="8">
        <rect x="0" y="0" width="8" height="8" fill="black"/>
        <rect x="4" y="4" width="4" height="4" fill="white"/>
      </pattern></defs>
      <rect width="64" height="64" fill="url(#p)"/>)svg"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_GT(rendered.red(6, 6), 231) << "the white square, at the offset the pattern gives it";
  EXPECT_LT(rendered.red(2, 2), 24) << "and black where it used to be";
}

TEST(SvgPattern, AppliesThePatternTransform) {
  // `patternTransform` moves the finished tile. A translation of four shifts the pattern exactly as an `x` of four
  // would, which is the check that the transform is applied to the tile rather than to the content.
  const Rendered translated(document(R"svg(
      <defs><pattern id="p" patternUnits="userSpaceOnUse" patternContentUnits="userSpaceOnUse"
                     x="0" y="0" width="8" height="8" patternTransform="translate(4,4)">
        <rect x="0" y="0" width="8" height="8" fill="black"/>
        <rect x="0" y="0" width="4" height="4" fill="white"/>
      </pattern></defs>
      <rect width="64" height="64" fill="url(#p)"/>)svg"));
  ASSERT_TRUE(translated.drawn) << translated.error;

  EXPECT_GT(translated.red(6, 6), 231) << "the white corner, moved by the transform";
  EXPECT_LT(translated.red(2, 2), 24);
}

TEST(SvgPattern, ScalesTheTileWithAnObjectBoundingBoxRectangle) {
  // `patternUnits="objectBoundingBox"` with a tile of a half by a half: the tile is half the element, so a 64-wide
  // element gets four tiles across rather than eight.
  const Rendered rendered(document(R"svg(
      <defs><pattern id="p" patternUnits="objectBoundingBox" patternContentUnits="userSpaceOnUse"
                     x="0" y="0" width="0.5" height="0.5">
        <rect x="0" y="0" width="32" height="32" fill="black"/>
        <rect x="0" y="0" width="16" height="16" fill="white"/>
      </pattern></defs>
      <rect width="64" height="64" fill="url(#p)"/>)svg"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  // The tile is 32x32, so the white corner repeats every 32 units.
  EXPECT_GT(rendered.red(8, 8), 231) << "the white corner of the first tile";
  EXPECT_GT(rendered.red(40, 8), 231) << "and of the second, one tile along";
  EXPECT_LT(rendered.red(24, 8), 24) << "with black between them";
}

// ==================================================================================================
// The cases that must not break
// ==================================================================================================

TEST(SvgPattern, APatternWithNoExtentDrawsNothing) {
  // A zero width or height is a pattern that cannot tile. The fill falls back to its flat colour, which with no fallback
  // is no paint at all -- so the shape is empty rather than filled with something invented.
  const Rendered rendered(document(R"svg(
      <defs><pattern id="p" patternUnits="userSpaceOnUse" x="0" y="0" width="0" height="0">
        <rect x="0" y="0" width="8" height="8" fill="black"/>
      </pattern></defs>
      <rect width="64" height="64" fill="url(#p)"/>)svg"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_EQ(rendered.coveredPixels(), 0);
}

TEST(SvgPattern, APatternWithNoContentDrawsNothing) {
  const Rendered rendered(document(R"svg(
      <defs><pattern id="p" patternUnits="userSpaceOnUse" x="0" y="0" width="8" height="8"/></defs>
      <rect width="64" height="64" fill="url(#p)"/>)svg"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_EQ(rendered.coveredPixels(), 0);
}

TEST(SvgPattern, AReferenceThatIsNotAPatternLeavesTheShapeInItsFallback) {
  // A `url(#id)` naming a gradient, a clip path or nothing at all resolves to no shader, and the fill's fallback colour
  // is used -- which is what the format asks for.
  const Rendered rendered(document(R"svg(
      <defs><linearGradient id="g"><stop offset="0" stop-color="red"/><stop offset="1" stop-color="red"/>
      </linearGradient></defs>
      <rect width="64" height="64" fill="url(#nope) #00ff00"/>)svg"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_EQ(rendered.alpha(32, 32), 255) << "the fallback colour is used";
  EXPECT_GT(static_cast<int>((rendered.at(32, 32) >> 8) & 0xFFu), 200) << "and it is the green one";
}

TEST(SvgPattern, DoesNotRecurseThroughItsOwnReference) {
  // A pattern whose content refers to a pattern terminates. Without clearing the reference in the content state, the
  // render would recurse until the stack ran out -- and the failure would be a crash rather than a wrong picture.
  const Rendered rendered(document(R"svg(
      <defs><pattern id="p" patternUnits="userSpaceOnUse" patternContentUnits="userSpaceOnUse"
                     x="0" y="0" width="8" height="8">
        <rect x="0" y="0" width="8" height="8" fill="black"/>
        <rect x="0" y="0" width="4" height="4" fill="url(#p)"/>
      </pattern></defs>
      <rect width="64" height="64" fill="url(#p)"/>)svg"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_GT(rendered.coveredPixels(), 0) << "it terminates and draws";
}

// ==================================================================================================
// The corpus, measured
// ==================================================================================================

TEST(SvgPattern, RendersTheCorpusDocumentsWhoseFillIsAPattern) {
  // ## What this test used to assert, and why it was wrong
  //
  // It required `jpg.svg` and `png.svg` to draw *nothing*, on the belief that their patterns contradicted themselves:
  // `patternContentUnits="objectBoundingBox"` beside an `<image width="200">` in a tile one unit across. Read that way
  // the content is two hundred bounding boxes wide and the tile cannot be built.
  //
  // The belief came from measuring the image's declared width and leaving out the `transform="scale(0.005)"` on the
  // `<use>` that carries it. Two hundred times five thousandths is one: the content is exactly the tile, which is what
  // `objectBoundingBox` content units mean. The documents were consistent all along, and a compatibility rule built on
  // the wrong measurement -- which fired on correct documents -- has been removed.
  //
  // What was actually broken was separate and is fixed: the reference resolver descended into `<g>` and nothing else, so
  // the `<use>` inside every one of these patterns was never resolved.
  const fs::path base = fs::path(CORAL_GRAPHICS_RESOURCE_DIR) / "apitest" / "SVG";
  for (const char* name : {"jpg.svg", "png.svg"}) {
    std::ifstream file(base / name, std::ios::binary);
    ASSERT_TRUE(file.good()) << name;
    std::ostringstream contents;
    contents << file.rdbuf();

    std::string error;
    auto parsed = Svg::parse(contents.str(), &error);
    ASSERT_TRUE(parsed.has_value()) << name << ": " << error;

    const auto surface = Surface::makeRaster(static_cast<int>(parsed->width()), static_cast<int>(parsed->height()));
    ASSERT_NE(surface, nullptr) << name;
    parsed->render(*surface->getCanvas());

    const auto pixels = surface->peekPixels();
    int covered = 0;
    for (int y = 0; y < pixels.height(); y += 2) {
      for (int x = 0; x < pixels.width(); x += 2) {
        if (((pixels.getPixelARGB32(x, y) >> 24) & 0xFFu) > 8) ++covered;
      }
    }

    // Each document is a 200x200 rectangle filled with a pattern whose tile is the whole rectangle, so the picture covers
    // the surface. `png.svg` is a little under all of it because its picture has transparent pixels, which is a fact
    // about the image rather than about the pattern.
    EXPECT_GT(covered, 7000) << name << " should be painted across by its pattern";
  }
}
