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
 * @file test_SvgImage.cpp
 *
 * `<image>`, from the embedded bytes to the pixels.
 *
 * Every picture in the corpus is a base64 `data:` URI, so a renderer that only resolved file paths would draw nothing for
 * any of them -- which is why the first tests here build a document with a picture in it rather than pointing at a file.
 *
 * The interesting assertions are about *placement*: the format's default is to fit the picture inside its rectangle and
 * centre it, not to stretch it. A renderer that stretched would pass any test whose rectangle happened to match the
 * picture's proportions, which is why the tests below deliberately use rectangles that do not.
 */

#include <svg/Svg.h>

#include <graphics/Pixmap.h>
#include <graphics/Surface.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef CORAL_GRAPHICS_RESOURCE_DIR
#error "CORAL_GRAPHICS_RESOURCE_DIR must be defined by the build; see CMakeLists.txt"
#endif

namespace {

namespace fs = std::filesystem;

using graphics::Pixmap;
using graphics::Rect;
using graphics::Surface;
using graphics::Svg;

/**
 * A 2x2 RGBA PNG as a base64 `data:` URI: red and green on the top row, blue and white on the bottom.
 *
 * Four different pixels are what make a placement test meaningful -- a solid colour could be drawn anywhere, upside
 * down, and still look right.
 *
 * This constant was *written out by hand* in the first version, and it looked entirely plausible: a valid signature, a
 * valid IHDR, and a corrupt IDAT. Eight tests then failed with "nothing was drawn", and the fault looked like the
 * decoder's. It is generated and CRC-checked now, which is the only way to be sure of binary data.
 */
const char* kQuadPng =
        "data:image/png;base64,"
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAEklEQVR42mP4z8DwHwyBNBgAAEnICfcD2WTxAAAA"
        "AElFTkSuQmCC";

class Rendered {
  public:
  Rendered(const std::string& document, const int width = 32, const int height = 32) {
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

  /** How many pixels are drawn at all. */
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

std::string document(const std::string& body, const int width = 32, const int height = 32) {
  return "<svg width=\"" + std::to_string(width) + "\" height=\"" + std::to_string(height) + "\">" + body + "</svg>";
}

} // namespace anonymous

// ==================================================================================================
// Drawing one
// ==================================================================================================

TEST(SvgImage, DrawsAnEmbeddedPicture) {
  // A 2x2 picture placed in the whole 32x32 document. It has to be scaled up by sixteen, which is what makes this a test
  // of the placement as well as of the decode.
  const Rendered rendered(document(std::string("<image x=\"0\" y=\"0\" width=\"32\" height=\"32\" xlink:href=\"") +
                                   kQuadPng + "\"/>"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_GT(rendered.coveredPixels(), 900) << "the picture fills the document";
  EXPECT_EQ(rendered.alpha(2, 2), 255) << "opaque at a corner";
  EXPECT_EQ(rendered.alpha(30, 30), 255);
}

TEST(SvgImage, DrawsItsFourDifferentPixelsInTheRightQuadrants) {
  // The picture is red, green, blue, white. If it is drawn the right way up, the top-left quadrant is red and the
  // top-right is green -- so a picture drawn upside down or mirrored fails here, and one drawn solid anywhere would not
  // have four colours to check.
  const Rendered rendered(document(std::string("<image x=\"0\" y=\"0\" width=\"32\" height=\"32\" xlink:href=\"") +
                                   kQuadPng + "\"/>"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  const auto red = [&rendered](const int x, const int y) { return static_cast<int>((rendered.at(x, y) >> 16) & 0xFFu); };
  const auto green = [&rendered](const int x, const int y) { return static_cast<int>((rendered.at(x, y) >> 8) & 0xFFu); };

  // Sampled away from the seam, because the scaling is nearest-neighbour and the boundary lands mid-pixel.
  const auto blue = [&rendered](const int x, const int y) { return static_cast<int>(rendered.at(x, y) & 0xFFu); };

  EXPECT_GT(red(8, 8), 200) << "the top-left quadrant is red";
  EXPECT_LT(green(8, 8), 60);
  EXPECT_GT(green(24, 8), 200) << "the top-right quadrant is green";
  // Blue, so on the blue channel -- an earlier version of this checked red plus green here, which is zero for a blue
  // pixel and made a correct render look like a missing one.
  EXPECT_GT(blue(8, 24), 200) << "the bottom-left quadrant is blue";
  EXPECT_LT(red(8, 24) + green(8, 24), 60) << "and carries neither of the other two";
  EXPECT_GT(red(24, 24) + green(24, 24) + blue(24, 24), 600) << "and the bottom-right is near white";
}

TEST(SvgImage, FitsThePictureInsideARectangleOfADifferentShape) {
  // The format's default is `xMidYMid meet`: a square picture in a wide rectangle is letterboxed and centred, not
  // stretched. A renderer that stretched would fill the whole rectangle and fail the margin assertions here.
  const Rendered rendered(document(std::string("<image x=\"0\" y=\"0\" width=\"32\" height=\"8\" xlink:href=\"") +
                                   kQuadPng + "\"/>"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  // The picture is square, so it is drawn as an 8x8 square in the middle of the 32x8 rectangle: x from 12 to 20.
  EXPECT_EQ(rendered.alpha(16, 4), 255) << "the middle is drawn";
  EXPECT_EQ(rendered.alpha(1, 4), 0) << "the letterbox on the left is empty";
  EXPECT_EQ(rendered.alpha(30, 4), 0) << "and on the right";

  // And the drawn width really is eight rather than thirty-two.
  int leftmost = 32;
  int rightmost = -1;
  for (int x = 0; x < 32; ++x) {
    if (rendered.alpha(x, 4) > 8) {
      leftmost = std::min(leftmost, x);
      rightmost = std::max(rightmost, x);
    }
  }
  EXPECT_NEAR(rightmost - leftmost + 1, 8, 2) << "the square picture is drawn eight wide";
}

TEST(SvgImage, CoversTheRectangleWhenAskedToSlice) {
  // `slice` is the cover: the picture is scaled to fill the rectangle and the overflow is cut off. The same document
  // with `meet` letterboxes; with `slice` it must not.
  const Rendered rendered(
          document(std::string("<image x=\"0\" y=\"0\" width=\"32\" height=\"8\" preserveAspectRatio=\"xMidYMid slice\" "
                               "xlink:href=\"") +
                   kQuadPng + "\"/>"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_EQ(rendered.alpha(1, 4), 255) << "slice reaches the edge that meet left empty";
  EXPECT_EQ(rendered.alpha(30, 4), 255);
}

TEST(SvgImage, PlacesThePictureWhereItIsAskedTo) {
  const Rendered rendered(document(std::string("<image x=\"16\" y=\"16\" width=\"16\" height=\"16\" xlink:href=\"") +
                                   kQuadPng + "\"/>"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_EQ(rendered.alpha(24, 24), 255) << "inside the rectangle";
  EXPECT_EQ(rendered.alpha(4, 4), 0) << "and nothing where it was not asked for";
}

TEST(SvgImage, FollowsATransform) {
  const Rendered rendered(
          document(std::string("<g transform=\"translate(16,16)\"><image x=\"0\" y=\"0\" width=\"16\" height=\"16\" "
                               "xlink:href=\"") +
                   kQuadPng + "\"/></g>"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_EQ(rendered.alpha(24, 24), 255) << "the picture is drawn where the transform puts it";
  EXPECT_EQ(rendered.alpha(4, 4), 0);
}

TEST(SvgImage, FadesWithTheElementsOpacity) {
  const Rendered rendered(document(std::string("<image x=\"0\" y=\"0\" width=\"32\" height=\"32\" opacity=\"0.5\" "
                                               "xlink:href=\"") +
                                   kQuadPng + "\"/>"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_NEAR(rendered.alpha(16, 16), 128, 6);
}

TEST(SvgImage, IsNotDrawnForANonPositiveSize) {
  // A zero width or height disables the element, as it does for a rectangle -- rather than drawing the picture at its own
  // size, which is a different picture from the one the document asked for.
  const Rendered zeroWidth(document(std::string("<image x=\"0\" y=\"0\" width=\"0\" height=\"32\" xlink:href=\"") +
                                    kQuadPng + "\"/>"));
  ASSERT_TRUE(zeroWidth.drawn) << zeroWidth.error;
  EXPECT_EQ(zeroWidth.coveredPixels(), 0);

  const Rendered zeroHeight(document(std::string("<image x=\"0\" y=\"0\" width=\"32\" height=\"0\" xlink:href=\"") +
                                     kQuadPng + "\"/>"));
  ASSERT_TRUE(zeroHeight.drawn) << zeroHeight.error;
  EXPECT_EQ(zeroHeight.coveredPixels(), 0);
}

// ==================================================================================================
// What it refuses, and what that costs
// ==================================================================================================

TEST(SvgImage, AReferenceThatCannotBeDecodedLeavesTheDocumentAlone) {
  // The element is kept and draws nothing. Losing a picture is recoverable; failing the whole document over one is not.
  const Rendered plain(document(R"svg(<rect width="32" height="32" fill="red"/>)svg"));
  ASSERT_TRUE(plain.drawn);

  const Rendered withBadImage(document(
          R"svg(<image x="0" y="0" width="8" height="8" xlink:href="data:image/png;base64,not-base64!"/>
              <rect width="32" height="32" fill="red"/>)svg"));
  ASSERT_TRUE(withBadImage.drawn) << withBadImage.error;

  EXPECT_EQ(withBadImage.alpha(16, 16), 255) << "the rest of the document still draws";
  EXPECT_EQ(withBadImage.at(16, 16), plain.at(16, 16)) << "and identically";
}

TEST(SvgImage, ALinkedPathIsNotResolvedAndLeavesTheDocumentAlone) {
  // A relative path needs a base directory that a document does not carry, so it is refused rather than guessed at --
  // and the rest of the document still draws.
  const Rendered rendered(document(R"svg(<image x="0" y="0" width="8" height="8" xlink:href="picture.png"/>
      <rect width="32" height="32" fill="red"/>)svg"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_EQ(rendered.alpha(16, 16), 255);
}

// ==================================================================================================
// The corpus
// ==================================================================================================

TEST(SvgImage, DrawsTheCorpusDocumentsWhosePictureIsReachedThroughAPattern) {
  // `jpg.svg` and `png.svg` are a rectangle filled with a pattern, whose content is a `<use>` of an embedded picture.
  // Reaching the picture therefore takes two indirections, and both were broken: the `<use>` inside a pattern was never
  // resolved, and the pattern itself did not exist.
  //
  // This test used to assert that both documents drew *nothing*, on a mistaken reading of their units -- see the note in
  // test_SvgPattern.cpp. They draw their picture now.
  const fs::path base = fs::path(CORAL_GRAPHICS_RESOURCE_DIR) / "apitest" / "SVG";
  for (const char* name : {"jpg.svg", "png.svg"}) {
    std::ifstream file(base / name, std::ios::binary);
    ASSERT_TRUE(file.good()) << name;
    std::ostringstream contents;
    contents << file.rdbuf();

    std::string error;
    auto parsed = Svg::parse(contents.str(), &error);
    ASSERT_TRUE(parsed.has_value()) << name << ": " << error;

    const auto surface = Surface::makeRaster(200, 200);
    ASSERT_NE(surface, nullptr) << name;
    parsed->render(*surface->getCanvas());

    // The picture fills the rectangle, so the middle of the surface carries ink -- and it is the picture's colour rather
    // than a flat fill, which is what a tile of one magnified pixel would have given.
    const auto pixels = surface->peekPixels();
    int distinctColours = 0;
    bool seen[8] = {};
    for (int y = 40; y < 160; y += 8) {
      for (int x = 40; x < 160; x += 8) {
        const uint32_t pixel = pixels.getPixelARGB32(x, y);
        if (((pixel >> 24) & 0xFFu) < 8) continue;
        // A crude bucket on the red channel: a flat fill lands in one, a picture in several.
        const size_t bucket = std::min<size_t>(7, ((pixel >> 16) & 0xFFu) / 32);
        if (!seen[bucket]) {
          seen[bucket] = true;
          ++distinctColours;
        }
      }
    }
    EXPECT_GT(distinctColours, 1) << name << " should show a picture, not one magnified pixel";
  }
}

TEST(SvgImage, DrawsTheCorpusPictureThatIsReferencedThroughAUse) {
  // ## The one corpus document where `<image>` is visible on its own
  //
  // `displayp3.svg` is a 400x400 canvas holding a 100x100 green rectangle at (20,20) *and* a `<use>` that references a
  // 400x400 image scaled by a quarter and placed at (260,20). So it is 10,000 pixels of rectangle, 10,000 of picture, and
  // the picture is the half that did not draw before this feature existed.
  //
  // The measurement is what makes it a test rather than a smoke check: sampled every second pixel, 20,000 device pixels
  // is 5,000 samples, and a renderer that drew only the rectangle would come out at half that.
  const fs::path path = fs::path(CORAL_GRAPHICS_RESOURCE_DIR) / "apitest" / "SVG" / "displayp3.svg";
  std::ifstream file(path, std::ios::binary);
  ASSERT_TRUE(file.good()) << path.string();
  std::ostringstream contents;
  contents << file.rdbuf();

  std::string error;
  auto parsed = Svg::parse(contents.str(), &error);
  ASSERT_TRUE(parsed.has_value()) << error;

  const auto surface = Surface::makeRaster(400, 400);
  ASSERT_NE(surface, nullptr);
  parsed->render(*surface->getCanvas());
  const auto pixels = surface->peekPixels();

  // The rectangle, which drew before as well.
  EXPECT_GT((pixels.getPixelARGB32(60, 60) >> 24) & 0xFFu, 200u) << "the green rectangle";
  EXPECT_GT((pixels.getPixelARGB32(60, 60) >> 8) & 0xFFu, 200u) << "and it is green";

  // The picture, at (260,20) to (360,120). This is the assertion the feature exists for.
  EXPECT_GT((pixels.getPixelARGB32(310, 70) >> 24) & 0xFFu, 200u) << "the referenced picture is drawn";
  EXPECT_EQ((pixels.getPixelARGB32(310, 70) >> 8) & 0xFFu == 255u &&
                    (pixels.getPixelARGB32(310, 70) >> 16) & 0xFFu == 0u,
            false)
          << "and it is the picture rather than the rectangle's green";

  // And nothing in the corner between them, so the picture has not simply been stretched across the canvas.
  EXPECT_EQ((pixels.getPixelARGB32(200, 300) >> 24) & 0xFFu, 0u) << "nothing where neither was placed";

  // The whole thing: about 20,000 device pixels is about 5,000 samples.
  int covered = 0;
  for (int y = 0; y < 400; y += 2) {
    for (int x = 0; x < 400; x += 2) {
      if (((pixels.getPixelARGB32(x, y) >> 24) & 0xFFu) > 8) ++covered;
    }
  }
  EXPECT_NEAR(covered, 5000, 400) << "the rectangle and the picture together";
}
