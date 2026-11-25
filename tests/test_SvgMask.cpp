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
 * @file test_SvgMask.cpp
 *
 * SVG `mask`, from the element to the pixels.
 *
 * The corpus's masks are white shapes on nothing and black-to-white gradients, and the two are what these tests use,
 * because they are the two rules the format allows and the second is the one that tells them apart. A renderer that used
 * the mask's alpha for everything would pass every white-shape test -- white is opaque -- and turn every gradient mask
 * into a solid rectangle.
 *
 * The other half of the file is the coordinate units. `maskUnits` and `maskContentUnits` default to *opposite* things,
 * and the corpus mixes them, so a renderer that used one for both would put the content in the wrong place for most of
 * the documents that specify either.
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
#include <vector>
#include <utility>

#ifndef CORAL_GRAPHICS_RESOURCE_DIR
#error "CORAL_GRAPHICS_RESOURCE_DIR must be defined by the build; see CMakeLists.txt"
#endif

namespace {

namespace fs = std::filesystem;

using graphics::Pixmap;
using graphics::Rect;
using graphics::Surface;
using graphics::Svg;

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

  struct Rgba {
    int r = 0;
    int g = 0;
    int b = 0;
    int a = 0;
  };

  [[nodiscard]] Rgba at(const int x, const int y) const {
    const uint32_t pixel = surface->peekPixels().getPixelARGB32(x, y);
    return Rgba {static_cast<int>((pixel >> 16) & 0xFFu), static_cast<int>((pixel >> 8) & 0xFFu),
                 static_cast<int>(pixel & 0xFFu), static_cast<int>((pixel >> 24) & 0xFFu)};
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
// A mask hides what it does not cover
// ==================================================================================================

TEST(SvgMask, KeepsTheElementWhereTheMaskIsWhite) {
  // The commonest mask by far: a white shape on nothing. Where the mask draws, the element survives.
  const Rendered rendered(document(R"svg(
      <defs><mask id="m" maskUnits="userSpaceOnUse" x="0" y="0" width="32" height="32">
        <rect x="0" y="0" width="16" height="32" fill="white"/>
      </mask></defs>
      <rect width="32" height="32" fill="red" mask="url(#m)"/>)svg"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_EQ(rendered.at(8, 16).a, 255) << "inside the white half";
  EXPECT_EQ(rendered.at(8, 16).r, 255) << "and still red";
  EXPECT_EQ(rendered.at(24, 16).a, 0) << "and gone where the mask drew nothing";
}

TEST(SvgMask, HidesTheElementWhereTheMaskIsBlack) {
  // The case that separates luminance from alpha. The mask is a fully opaque black rectangle -- so an alpha rule would
  // keep the whole element, and the format's own rule removes it.
  const Rendered rendered(document(R"svg(
      <defs><mask id="m" maskUnits="userSpaceOnUse" x="0" y="0" width="32" height="32">
        <rect x="0" y="0" width="32" height="32" fill="black"/>
      </mask></defs>
      <rect width="32" height="32" fill="red" mask="url(#m)"/>)svg"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_EQ(rendered.at(16, 16).a, 0) << "an opaque black mask hides everything";
}

TEST(SvgMask, FadesAlongAGradientMask) {
  // And this is the case the corpus actually contains: three of its masks are black-to-white gradients. Every stop is
  // opaque, so the alpha is one from end to end -- only the luminance carries the fade.
  const Rendered rendered(document(R"svg(
      <defs>
        <linearGradient id="fade" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="32" y2="0">
          <stop offset="0" stop-color="black"/><stop offset="1" stop-color="white"/>
        </linearGradient>
        <mask id="m" maskUnits="userSpaceOnUse" x="0" y="0" width="32" height="32">
          <rect x="0" y="0" width="32" height="32" fill="url(#fade)"/>
        </mask>
      </defs>
      <rect width="32" height="32" fill="red" mask="url(#m)"/>)svg"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_LT(rendered.at(2, 16).a, 24) << "dark at the black end";
  EXPECT_GT(rendered.at(29, 16).a, 231) << "opaque at the white end";
  EXPECT_NEAR(rendered.at(16, 16).a, 128, 16) << "and fading in between";
  // The colour is red the whole way: the mask takes coverage, not colour.
  EXPECT_GT(rendered.at(16, 16).r, 240);
}

TEST(SvgMask, ATransparentWhiteMaskHidesTheElement) {
  // Luminance alone would keep it: white is white. Multiplying by the alpha is what makes a transparent mask hide
  // anything -- the mirror of the black-mask case, and the reason the rule is a product rather than a weighted sum.
  const Rendered rendered(document(R"svg(
      <defs><mask id="m" maskUnits="userSpaceOnUse" x="0" y="0" width="32" height="32">
        <rect x="0" y="0" width="32" height="32" fill="white" fill-opacity="0"/>
      </mask></defs>
      <rect width="32" height="32" fill="red" mask="url(#m)"/>)svg"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_EQ(rendered.at(16, 16).a, 0) << "white but transparent hides the element";
}

// ==================================================================================================
// The corpus
// ==================================================================================================

TEST(SvgMask, FadesTheCorpusMaskDocument) {
  // `mask.svg` is the document this whole feature was measured from, and it is the one a hand-written test cannot
  // substitute for: its mask is a black-to-white gradient at full alpha behind a red rectangle. Under an alpha rule the
  // rectangle comes out flat and opaque; under the format's own rule it fades. Nothing else in the corpus separates the
  // two as cleanly.
  const fs::path path = fs::path(CORAL_GRAPHICS_RESOURCE_DIR) / "apitest" / "SVG" / "mask.svg";
  std::ifstream file(path, std::ios::binary);
  ASSERT_TRUE(file.good()) << path.string();
  std::ostringstream contents;
  contents << file.rdbuf();

  std::string error;
  auto parsed = Svg::parse(contents.str(), &error);
  ASSERT_TRUE(parsed.has_value()) << error;

  const auto surface = Surface::makeRaster(200, 200);
  ASSERT_NE(surface, nullptr);
  parsed->render(*surface->getCanvas());

  const auto pixels = surface->peekPixels();
  const auto alphaAt = [&pixels](const int x, const int y) {
    return static_cast<int>((pixels.getPixelARGB32(x, y) >> 24) & 0xFFu);
  };

  EXPECT_LT(alphaAt(4, 100), 24) << "the black end of the gradient hides the rectangle";
  EXPECT_GT(alphaAt(195, 100), 231) << "and the white end keeps it";
  EXPECT_NEAR(alphaAt(100, 100), 128, 24) << "with a fade in between";

  // And the colour is the document's red throughout: the mask takes coverage and leaves colour alone.
  EXPECT_GT((pixels.getPixelARGB32(100, 100) >> 16) & 0xFFu, 240u);
}

TEST(SvgMask, TheCorpusMasksProduceVisibleCoverage) {
  // Every document in the corpus that names a mask, rendered: none of them may come out completely empty, which is what
  // a mask that hides everything by mistake looks like from outside.
  // Both places the corpus puts a document: `SVG/` is the numbered set, and the loose files beside it are the ones
  // written for a single feature -- `mask.svg`, `mask_gradient.svg`, `mask_image.svg` among them, which are the four
  // most relevant documents here and were being skipped.
  const fs::path base = fs::path(CORAL_GRAPHICS_RESOURCE_DIR) / "apitest";
  std::vector<fs::path> documents;
  for (const fs::path& directory : {base / "SVG", base}) {
    for (const auto& entry : fs::directory_iterator(directory)) {
      if (entry.path().extension() == ".svg") documents.push_back(entry.path());
    }
  }
  std::sort(documents.begin(), documents.end());

  int withMasks = 0;
  std::vector<std::string> empty;

  for (const fs::path& entry : documents) {
    std::ifstream file(entry, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    const std::string document = contents.str();
    if (document.find("mask=") == std::string::npos) continue;

    ++withMasks;
    std::string error;
    auto parsed = Svg::parse(document, &error);
    ASSERT_TRUE(parsed.has_value()) << entry.filename() << ": " << error;

    const auto surface = Surface::makeRaster(static_cast<int>(parsed->width()), static_cast<int>(parsed->height()));
    ASSERT_NE(surface, nullptr) << entry.filename();
    parsed->render(*surface->getCanvas());

    const auto pixels = surface->peekPixels();
    int covered = 0;
    for (int y = 0; y < pixels.height(); y += 2) {
      for (int x = 0; x < pixels.width(); x += 2) {
        if (((pixels.getPixelARGB32(x, y) >> 24) & 0xFFu) > 8) ++covered;
      }
    }
    if (covered <= 4) empty.push_back(entry.filename().string());
  }

  // Seven, measured. The bound is loose so that adding a document does not fail the test -- what it is here to catch is
  // the walk silently finding nothing, which is how a corpus test stops testing anything.
  EXPECT_GE(withMasks, 6) << "the corpus should hold documents that use a mask";
  EXPECT_LE(withMasks, static_cast<int>(documents.size())) << "and no more than there are documents";

  // ## What this assertion did, and why it reads the way it does now
  //
  // It used to require exactly one document to draw nothing: `mask_image.svg`, whose mask holds an `<image>` and a
  // `<pattern>`. When `<image>` landed, that expectation *failed on its own* -- which was the point of writing it as an
  // expectation rather than a skip. The document now draws, because the picture inside its mask now renders.
  //
  // What it draws is still not what the reference shows: the mask's other child is a rectangle filled with a
  // `url(#pattern)`, and patterns do not exist yet. So the requirement is that nothing draws *nothing* for an
  // unexplained reason -- a document that comes out empty is a document whose mask rendered empty, and the only way that
  // is currently explained is a fill that resolves to nothing.
  for (const std::string& name : empty) {
    ADD_FAILURE() << name << " came out completely empty; the mask rendered to nothing";
  }
  EXPECT_LE(empty.size(), 1U) << "at most one document may be explained by a missing feature";
}

// ==================================================================================================
// mask-type
// ==================================================================================================

TEST(SvgMask, HonoursTheAlphaModeWhenAskedFor) {
  // A black-to-white gradient at full alpha. Under the format's default the rectangle fades; under `alpha` every stop is
  // opaque, so the rectangle is drawn whole. The two rules give opposite pictures from the same document, which is the
  // whole reason the property exists.
  const Rendered rendered(document(R"svg(
      <defs>
        <linearGradient id="fade" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="32" y2="0">
          <stop offset="0" stop-color="black"/><stop offset="1" stop-color="white"/>
        </linearGradient>
        <mask id="m" maskUnits="userSpaceOnUse" x="0" y="0" width="32" height="32" mask-type="alpha">
          <rect x="0" y="0" width="32" height="32" fill="url(#fade)"/>
        </mask>
      </defs>
      <rect width="32" height="32" fill="red" mask="url(#m)"/>)svg"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_EQ(rendered.at(2, 16).a, 255) << "every stop of the gradient is opaque, so alpha keeps the dark end too";
}

TEST(SvgMask, ReadsMaskTypeFromAStyleDeclaration) {
  // The spelling the corpus actually uses. `mask-type` is a *property*, so it may be written inside `style` -- and a
  // reader that only looked at the attribute reported the corpus as never overriding the default, which was wrong.
  //
  // The same document as the test above, with the property moved into `style`, and the same expected picture.
  const Rendered rendered(document(R"svg(
      <defs>
        <linearGradient id="fade" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="32" y2="0">
          <stop offset="0" stop-color="black"/><stop offset="1" stop-color="white"/>
        </linearGradient>
        <mask id="m" maskUnits="userSpaceOnUse" x="0" y="0" width="32" height="32" style="mask-type:alpha">
          <rect x="0" y="0" width="32" height="32" fill="url(#fade)"/>
        </mask>
      </defs>
      <rect width="32" height="32" fill="red" mask="url(#m)"/>)svg"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_EQ(rendered.at(2, 16).a, 255) << "the style declaration is read";
}

TEST(SvgMask, AStyleDeclarationBeatsTheAttribute) {
  // The cascade, which is the same rule every other presentation property follows: a `style` declaration outranks an
  // attribute. A document that writes both is a document whose author fixed a mistake in the attribute's place.
  const Rendered rendered(document(R"svg(
      <defs>
        <linearGradient id="fade" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="32" y2="0">
          <stop offset="0" stop-color="black"/><stop offset="1" stop-color="white"/>
        </linearGradient>
        <mask id="m" maskUnits="userSpaceOnUse" x="0" y="0" width="32" height="32"
              mask-type="luminance" style="mask-type:alpha">
          <rect x="0" y="0" width="32" height="32" fill="url(#fade)"/>
        </mask>
      </defs>
      <rect width="32" height="32" fill="red" mask="url(#m)"/>)svg"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_EQ(rendered.at(2, 16).a, 255) << "alpha won, so the dark end survives";
}

TEST(SvgMask, DefaultsToLuminanceWhenNothingSaysOtherwise) {
  // The format's default, and the one the rest of the corpus relies on.
  const Rendered rendered(document(R"svg(
      <defs>
        <linearGradient id="fade" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="32" y2="0">
          <stop offset="0" stop-color="black"/><stop offset="1" stop-color="white"/>
        </linearGradient>
        <mask id="m" maskUnits="userSpaceOnUse" x="0" y="0" width="32" height="32">
          <rect x="0" y="0" width="32" height="32" fill="url(#fade)"/>
        </mask>
      </defs>
      <rect width="32" height="32" fill="red" mask="url(#m)"/>)svg"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_LT(rendered.at(2, 16).a, 24) << "luminance, so the dark end is hidden";
}

// ==================================================================================================
// The two coordinate systems
// ==================================================================================================

TEST(SvgMask, MasksAGroupOfShapes) {
  const Rendered rendered(document(R"svg(
      <defs><mask id="m" maskUnits="userSpaceOnUse" x="0" y="0" width="32" height="32">
        <circle cx="16" cy="16" r="10" fill="white"/>
      </mask></defs>
      <g mask="url(#m)">
        <rect x="0" y="0" width="32" height="16" fill="red"/>
        <rect x="0" y="16" width="32" height="16" fill="blue"/>
      </g>)svg"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_EQ(rendered.at(16, 12).r, 255) << "the red half, inside the circle";
  EXPECT_EQ(rendered.at(16, 20).b, 255) << "the blue half, inside it";
  EXPECT_EQ(rendered.at(2, 16).a, 0) << "and both gone outside it";
}

TEST(SvgMask, TheRegionClipsTheMask) {
  // A mask's coverage is zero outside its own region, so a region that covers only the left half removes the right --
  // even though the mask's content draws across both.
  const Rendered rendered(document(R"svg(
      <defs><mask id="m" maskUnits="userSpaceOnUse" x="0" y="0" width="16" height="32">
        <rect x="0" y="0" width="32" height="32" fill="white"/>
      </mask></defs>
      <rect width="32" height="32" fill="red" mask="url(#m)"/>)svg"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_EQ(rendered.at(8, 16).a, 255) << "inside the region";
  EXPECT_EQ(rendered.at(24, 16).a, 0) << "and cut off at the region's edge, not at the content's";
}

TEST(SvgMask, TheDefaultRegionIsLargerThanTheElement) {
  // The format's defaults are -10%, -10%, 120%, 120% of the element's box: a margin all round, so a mask that says
  // nothing about its region still covers everything it is applied to.
  const Rendered rendered(document(R"svg(
      <defs><mask id="m"><rect x="0" y="0" width="32" height="32" fill="white"/></mask></defs>
      <rect x="8" y="8" width="16" height="16" fill="red" mask="url(#m)"/>)svg"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_EQ(rendered.at(16, 16).a, 255) << "the element survives its own default region";
  EXPECT_EQ(rendered.at(16, 16).r, 255);
}

TEST(SvgMask, MeasuresAnObjectBoundingBoxRegionAgainstTheElement) {
  // With `maskUnits="objectBoundingBox"` the region's numbers are fractions of the element's box, so a region of
  // `0 0 0.5 1` covers the left half of *the element* -- which starts at x = 8 here, not at zero.
  const Rendered rendered(document(R"svg(
      <defs><mask id="m" maskUnits="objectBoundingBox" x="0" y="0" width="0.5" height="1">
        <rect x="0" y="0" width="100" height="100" fill="white"/>
      </mask></defs>
      <rect x="8" y="8" width="16" height="16" fill="red" mask="url(#m)"/>)svg"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_GT(rendered.at(12, 16).a, 200) << "the left half of the element survives";
  EXPECT_EQ(rendered.at(20, 16).a, 0) << "and the right half is removed";
}

TEST(SvgMask, ScalesObjectBoundingBoxContentAgainstTheElement) {
  // `maskContentUnits="objectBoundingBox"` is the other half, and it is *not* the default. The content's numbers become
  // fractions of the element's box, so a unit square of content covers the element exactly.
  const Rendered rendered(document(R"svg(
      <defs><mask id="m" maskUnits="objectBoundingBox" maskContentUnits="objectBoundingBox"
                  x="0" y="0" width="1" height="1">
        <rect x="0" y="0" width="1" height="1" fill="white"/>
      </mask></defs>
      <rect x="8" y="8" width="16" height="16" fill="red" mask="url(#m)"/>)svg"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_EQ(rendered.at(16, 16).a, 255) << "the unit square covers the element";
  EXPECT_EQ(rendered.at(4, 16).a, 0) << "and nothing outside it";
}

// ==================================================================================================
// Combining, and the cases that must not break
// ==================================================================================================

TEST(SvgMask, MultipliesWithTheElementsOwnOpacity) {
  // A half-opacity element under a half-luminance mask is a quarter. That requires the two to share one layer: nested
  // layers would composite the element twice and give 0.5 * 0.5 * 0.5 rather than 0.5 * 0.5.
  const Rendered rendered(document(R"svg(
      <defs><mask id="m" maskUnits="userSpaceOnUse" x="0" y="0" width="32" height="32">
        <rect x="0" y="0" width="32" height="32" fill="#808080"/>
      </mask></defs>
      <rect width="32" height="32" fill="red" opacity="0.5" mask="url(#m)"/>)svg"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  // Grey 0x80 has a luminance of about 0.502, and the element is 0.5 opaque: about a quarter.
  EXPECT_NEAR(rendered.at(16, 16).a, 64, 5);
}

TEST(SvgMask, CombinesWithAClipPath) {
  const Rendered rendered(document(R"svg(
      <defs>
        <clipPath id="c"><rect x="0" y="0" width="32" height="16"/></clipPath>
        <mask id="m" maskUnits="userSpaceOnUse" x="0" y="0" width="32" height="32">
          <rect x="0" y="0" width="16" height="32" fill="white"/>
        </mask>
      </defs>
      <rect width="32" height="32" fill="red" clip-path="url(#c)" mask="url(#m)"/>)svg"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_EQ(rendered.at(8, 8).a, 255) << "inside both";
  EXPECT_EQ(rendered.at(24, 8).a, 0) << "outside the mask";
  EXPECT_EQ(rendered.at(8, 24).a, 0) << "and outside the clip";
}

TEST(SvgMask, AMaskThatCannotBeResolvedLeavesTheElement) {
  // A reference to an id that is not there, or that names the wrong kind of element, must not hide the element -- losing
  // an effect is recoverable and losing the drawing is not.
  const Rendered missing(document(R"svg(<rect width="32" height="32" fill="red" mask="url(#nope)"/>)svg"));
  ASSERT_TRUE(missing.drawn) << missing.error;
  EXPECT_EQ(missing.at(16, 16).r, 255) << "an unresolvable mask hides nothing";

  const Rendered wrongKind(document(R"svg(
      <defs><clipPath id="c"><rect x="0" y="0" width="8" height="8"/></clipPath></defs>
      <rect width="32" height="32" fill="red" mask="url(#c)"/>)svg"));
  ASSERT_TRUE(wrongKind.drawn) << wrongKind.error;
  EXPECT_EQ(wrongKind.at(16, 16).r, 255) << "nor does one naming a clip path";
}

TEST(SvgMask, IsNotAppliedTwiceToADescendant) {
  // The reference belongs to the element that named it. A descendant inheriting the id would have the mask applied a
  // second time against its own box, which for a gradient mask would darken it twice.
  const Rendered rendered(document(R"svg(
      <defs><mask id="m" maskUnits="userSpaceOnUse" x="0" y="0" width="32" height="32">
        <rect x="0" y="0" width="32" height="32" fill="white"/>
      </mask></defs>
      <g mask="url(#m)"><rect width="32" height="32" fill="red"/></g>)svg"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_EQ(rendered.at(16, 16).a, 255) << "a white mask applied once is no change";
  EXPECT_EQ(rendered.at(16, 16).r, 255);
}
