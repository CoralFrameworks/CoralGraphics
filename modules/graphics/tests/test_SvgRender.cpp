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
 * @file test_SvgRender.cpp
 *
 * Drawing SVG documents, checked by reading the pixels back.
 *
 * The parser tests prove that a document is understood; these prove that understanding reaches the canvas. That is a
 * different claim, and the gap between them is where the visible mistakes live: a transform applied twice, a percentage
 * resolved against the wrong axis, a `viewBox` fitted to the wrong corner, a fill rule that fills a hole. Every one of
 * those parses perfectly and renders wrongly.
 *
 * The assertions are on single pixels chosen to be well inside a region, because antialiasing makes an edge pixel a
 * statement about the rasterizer rather than about the SVG.
 */

#include <svg/Svg.h>

#include <graphics/Pixmap.h>
#include <graphics/Surface.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#ifndef CORAL_GRAPHICS_RESOURCE_DIR
#error "CORAL_GRAPHICS_RESOURCE_DIR must be defined by the build; see modules/graphics/CMakeLists.txt"
#endif

namespace {

namespace fs = std::filesystem;

using graphics::Pixmap;
using graphics::Rect;
using graphics::Svg;
using graphics::Surface;

/** A rendered document, with the accessors a pixel assertion needs. */
class Rendered {
  public:
  Rendered(const std::string& document, const int width, const int height) {
    surface = Surface::makeRaster(width, height);
    if (surface == nullptr) return;
    auto parsed = Svg::parse(document, &error);
    if (!parsed) return;
    parsedDocument = std::move(*parsed);
    parsedDocument->render(*surface->getCanvas());
    drawn = true;
  }

  explicit Rendered(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    const std::string document = contents.str();

    std::string parseError;
    auto parsed = Svg::parse(document, &parseError);
    if (!parsed) {
      error = parseError;
      return;
    }
    parsedDocument = std::move(*parsed);

    const int width = static_cast<int>(parsedDocument->width());
    const int height = static_cast<int>(parsedDocument->height());
    surface = Surface::makeRaster(width, height);
    if (surface == nullptr) {
      error = "the surface could not be allocated";
      return;
    }
    parsedDocument->render(*surface->getCanvas());
    drawn = true;
  }

  /** The channels at (@p x, @p y), with each named as the document would have written it. */
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
  std::optional<Svg> parsedDocument;
};

fs::path resource(const char* name) {
  return fs::path(CORAL_GRAPHICS_RESOURCE_DIR) / "apitest" / "SVG" / name;
}

} // namespace anonymous

// ==================================================================================================
// The simplest documents, so a failure has one possible cause
// ==================================================================================================

TEST(SvgRender, DrawsAFilledRectangle) {
  const Rendered rendered(R"html(<svg width="20" height="10"><rect width="20" height="10" fill="#ff0000"/></svg>)html", 20, 10);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  const auto pixel = rendered.at(10, 5);
  EXPECT_EQ(pixel.r, 255);
  EXPECT_EQ(pixel.g, 0);
  EXPECT_EQ(pixel.b, 0);
  EXPECT_EQ(pixel.a, 255);
}

TEST(SvgRender, LeavesTheBackgroundUntouchedWhereNothingIsDrawn) {
  const Rendered rendered(R"html(<svg width="20" height="10"><rect width="10" height="10" fill="red"/></svg>)html", 20, 10);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_EQ(rendered.at(2, 5).r, 255) << "inside the rectangle";
  // The untouched half is transparent black, not white: a document says nothing about what is behind it.
  EXPECT_EQ(rendered.at(18, 5).a, 0) << "outside the rectangle";
}

TEST(SvgRender, ResolvesPercentageLengthsAgainstTheViewport) {
  // `width="100%"` on a 20-wide viewport is 20. Getting the axis wrong makes it 10 and the right-hand half transparent.
  const Rendered rendered(R"html(<svg width="20" height="10"><rect width="100%" height="100%" fill="blue"/></svg>)html", 20, 10);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_EQ(rendered.at(19, 9).b, 255) << "the rectangle should reach the far corner";
}

TEST(SvgRender, DrawsACircleWithItsCentreWhereItSaysAndItsEdgeWhereItSays) {
  const Rendered rendered(
          R"html(<svg width="40" height="40"><circle cx="20" cy="20" r="10" fill="#00ff00"/></svg>)html", 40, 40);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_EQ(rendered.at(20, 20).g, 255) << "the centre";
  EXPECT_EQ(rendered.at(20, 8).a, 0) << "eight units above the centre is outside a radius of ten";
  EXPECT_EQ(rendered.at(20, 12).g, 255) << "and eight in is inside";
}

TEST(SvgRender, DrawsAnEllipseWithTwoDifferentRadii) {
  const Rendered rendered(
          R"html(<svg width="60" height="20"><ellipse cx="30" cy="10" rx="25" ry="8" fill="red"/></svg>)html", 60, 20);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_EQ(rendered.at(50, 10).r, 255) << "twenty to the right, inside the x radius of twenty-five";
  EXPECT_EQ(rendered.at(50, 18).a, 0) << "and near the bottom, outside the y radius of eight";
}

TEST(SvgRender, FillsAnOpenPathAsIfItWereClosed) {
  // A filled open contour is closed implicitly. Not doing so leaves the shape unfilled, which is a blank picture rather
  // than a slightly wrong one.
  const Rendered rendered(
          R"html(<svg width="20" height="20"><path d="M2 2 L18 2 L18 18 Z" fill="red"/></svg>)html", 20, 20);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_EQ(rendered.at(14, 6).r, 255) << "inside the triangle";
  EXPECT_EQ(rendered.at(4, 16).a, 0) << "outside it";
}

TEST(SvgRender, HonoursTheFillRule) {
  // The two rules disagree only where contours overlap, which is the whole reason the property exists. A five-pointed
  // star has to be *self-crossing* to show the difference: a ten-vertex polygon traced around the perimeter is simple,
  // never crosses itself, and both rules fill it -- so the test would pass whatever the renderer did with the property.
  // The {5/2} pentagram connects every second point, winds twice around the middle, and is the shape that tells them
  // apart.
  const char* star = R"html(<svg width="100" height="100"><path fill-rule="%s" fill="red"
      d="M50 5 L76.5 86.4 L7.2 36.1 L92.8 36.1 L23.5 86.4 Z"/></svg>)html";
  const std::string nonzero = [&] {
    std::string text = star;
    return text.replace(text.find("%s"), 2, "nonzero");
  }();
  const std::string evenOdd = [&] {
    std::string text = star;
    return text.replace(text.find("%s"), 2, "evenodd");
  }();

  const Rendered withNonzero(nonzero, 100, 100);
  const Rendered withEvenOdd(evenOdd, 100, 100);
  ASSERT_TRUE(withNonzero.drawn) << withNonzero.error;
  ASSERT_TRUE(withEvenOdd.drawn) << withEvenOdd.error;

  EXPECT_EQ(withNonzero.at(50, 40).r, 255) << "the middle of a star is filled under nonzero";
  EXPECT_EQ(withEvenOdd.at(50, 40).a, 0) << "and hollow under evenodd";
}

TEST(SvgRender, AppliesATransformToTheGeometry) {
  const Rendered rendered(
          R"html(<svg width="40" height="40"><rect x="0" y="0" width="10" height="10" fill="red"
                    transform="translate(20,20)"/></svg>)html",
          40, 40);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_EQ(rendered.at(25, 25).r, 255) << "the translated copy";
  EXPECT_EQ(rendered.at(5, 5).a, 0) << "the untranslated position is empty";
}

TEST(SvgRender, ComposesTransformsDownTheTree) {
  // A group's transform applies to its children *and* a child's applies on top. Applying only the innermost is the
  // mistake, and it puts the shape at the wrong place rather than at no place.
  const Rendered rendered(
          R"html(<svg width="60" height="60"><g transform="translate(10,10)">
                 <rect width="10" height="10" fill="red" transform="translate(30,30)"/></g></svg>)html",
          60, 60);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_EQ(rendered.at(45, 45).r, 255) << "10 + 30 from the origin";
  EXPECT_EQ(rendered.at(35, 35).a, 0) << "the group's translation alone is not enough";
}

TEST(SvgRender, DrawsAStrokeWithoutFilling) {
  const Rendered rendered(
          R"html(<svg width="40" height="40"><rect x="10" y="10" width="20" height="20" fill="none"
                    stroke="#0000ff" stroke-width="4"/></svg>)html",
          40, 40);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_EQ(rendered.at(20, 10).b, 255) << "on the top edge";
  EXPECT_EQ(rendered.at(20, 20).a, 0) << "the interior is not filled";
}

TEST(SvgRender, AppliesOpacityToTheFill) {
  const Rendered rendered(
          R"html(<svg width="20" height="20"><rect width="20" height="20" fill="red" opacity="0.5"/></svg>)html", 20, 20);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  const auto pixel = rendered.at(10, 10);
  // Half-covered opaque red over transparent black. `getPixelARGB32` reports *un-premultiplied* channels, so the red
  // comes back full and the half is carried by the alpha -- which is what that accessor's own note says, and the reason
  // a test reads coverage through the alpha rather than through a colour channel.
  EXPECT_NEAR(pixel.r, 255, 2);
  EXPECT_NEAR(pixel.a, 128, 2);
}

TEST(SvgRender, HonoursNoneByNotFilling) {
  const Rendered rendered(
          R"html(<svg width="20" height="20"><rect width="20" height="20" fill="none"/></svg>)html", 20, 20);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_EQ(rendered.at(10, 10).a, 0);
}

TEST(SvgRender, InheritsFillFromAGroup) {
  const Rendered rendered(
          R"html(<svg width="20" height="20"><g fill="#00ff00"><rect width="20" height="20"/></g></svg>)html", 20, 20);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_EQ(rendered.at(10, 10).g, 255) << "the group's fill reaches the rectangle";
}

TEST(SvgRender, OverridesAnInheritedFillOnTheChild) {
  const Rendered rendered(
          R"html(<svg width="20" height="20"><g fill="red"><rect width="20" height="20" fill="blue"/></g></svg>)html", 20, 20);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_EQ(rendered.at(10, 10).b, 255) << "the child's own fill wins";
}

TEST(SvgRender, SkipsAnElementThatIsNotDisplayed) {
  const Rendered rendered(
          R"html(<svg width="20" height="20"><rect width="20" height="20" fill="red" display="none"/></svg>)html", 20, 20);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_EQ(rendered.at(10, 10).a, 0);
}

TEST(SvgRender, RendersAUseOfAnotherElement) {
  const Rendered rendered(
          R"html(<svg width="40" height="40"><defs><rect id="box" width="10" height="10" fill="red"/></defs>
                 <use href="#box" x="20" y="20"/></svg>)html",
          40, 40);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_EQ(rendered.at(25, 25).r, 255) << "the referenced rectangle, at the use's position";
  EXPECT_EQ(rendered.at(5, 5).a, 0) << "and not drawn where it is defined";
}

TEST(SvgRender, AppliesAStylesheetRuleToTheElementItMatches) {
  const Rendered rendered(
          R"html(<svg width="20" height="20"><defs><style>.box { fill: #00ff00 }</style></defs>
                 <rect class="box" width="20" height="20"/></svg>)html",
          20, 20);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_EQ(rendered.at(10, 10).g, 255) << "the class rule supplied the fill";
}

// ==================================================================================================
// Fitting into the destination
// ==================================================================================================

TEST(SvgRender, FitsADocumentWithNoViewBoxIntoTheDestination) {
  // A document whose size comes from `width`/`height` rather than a viewBox still has to be *fitted*: a 20x20 document
  // drawn into a 40x40 rectangle fills it. Leaving the transform at the identity for this case draws it at 20x20 and
  // only moves it, which is correct only when the two sizes happen to agree -- and it is the bug a montage of documents
  // in cells of a different size finds immediately.
  const auto surface = Surface::makeRaster(40, 40);
  ASSERT_NE(surface, nullptr);
  const auto canvas = surface->getCanvas();

  auto parsed = Svg::parse(R"html(<svg width="20" height="20"><rect width="20" height="20" fill="red"/></svg>)html");
  ASSERT_TRUE(parsed.has_value());
  parsed->render(*canvas, Rect::MakeLTRB(0, 0, 40, 40));

  const Pixmap pixels = surface->peekPixels();
  EXPECT_EQ((pixels.getPixelARGB32(38, 38) >> 16) & 0xFFu, 255u) << "the far corner of the destination is filled";
  EXPECT_EQ((pixels.getPixelARGB32(2, 2) >> 16) & 0xFFu, 255u) << "and so is the near one";
}

TEST(SvgRender, FitsADocumentIntoARectangleThatIsNotAtTheOrigin) {
  // The same, into a cell offset inside a larger surface: the document fills the cell and does not touch what is around
  // it. This is what the demo's montage relies on.
  const auto surface = Surface::makeRaster(60, 60);
  ASSERT_NE(surface, nullptr);
  const auto canvas = surface->getCanvas();

  auto parsed = Svg::parse(R"html(<svg width="20" height="20"><rect width="20" height="20" fill="#00ff00"/></svg>)html");
  ASSERT_TRUE(parsed.has_value());
  parsed->render(*canvas, Rect::MakeLTRB(20, 20, 40, 40));

  const Pixmap pixels = surface->peekPixels();
  EXPECT_EQ((pixels.getPixelARGB32(22, 22) >> 8) & 0xFFu, 255u) << "inside the cell";
  EXPECT_EQ((pixels.getPixelARGB32(38, 38) >> 8) & 0xFFu, 255u) << "and up to its far corner";
  EXPECT_EQ((pixels.getPixelARGB32(55, 30) >> 24) & 0xFFu, 0u) << "nothing to the right of it";
  EXPECT_EQ((pixels.getPixelARGB32(30, 55) >> 24) & 0xFFu, 0u) << "nor below";
  EXPECT_EQ((pixels.getPixelARGB32(5, 5) >> 24) & 0xFFu, 0u) << "nor above";
}

TEST(SvgRender, DoesNotDrawOutsideTheRectangleItWasGiven) {
  // SVG permits content to overflow the viewport, and a document that does would otherwise spill across whatever the
  // caller drew next to it. Drawing *into* a rectangle means being bounded by it.
  const auto surface = Surface::makeRaster(60, 40);
  ASSERT_NE(surface, nullptr);
  const auto canvas = surface->getCanvas();

  // A 20x20 document containing a rectangle that runs far past its own width. Fitted into a 20x20 destination at the
  // origin, everything beyond x = 20 has to be cut off.
  auto parsed = Svg::parse(
          R"html(<svg width="20" height="20"><rect x="0" y="0" width="200" height="20" fill="red"/></svg>)html");
  ASSERT_TRUE(parsed.has_value());
  parsed->render(*canvas, Rect::MakeLTRB(0, 0, 20, 20));

  const Pixmap pixels = surface->peekPixels();
  EXPECT_EQ((pixels.getPixelARGB32(10, 10) >> 16) & 0xFFu, 255u) << "inside the destination";
  EXPECT_EQ((pixels.getPixelARGB32(19, 10) >> 24) & 0xFFu, 255u) << "up to its edge";
  EXPECT_EQ((pixels.getPixelARGB32(30, 10) >> 24) & 0xFFu, 0u) << "and nothing past it";
  EXPECT_EQ((pixels.getPixelARGB32(10, 30) >> 24) & 0xFFu, 0u) << "nor below it";
}

// ==================================================================================================
// The viewBox
// ==================================================================================================

TEST(SvgRender, FitsAViewBoxIntoTheDestination) {
  // A viewBox of 10x10 drawn into a document 20x20 scales by two, so a rectangle covering the whole viewBox covers the
  // whole surface. Ignoring the viewBox leaves a quarter of it painted.
  const Rendered rendered(
          R"html(<svg width="20" height="20" viewBox="0 0 10 10"><rect width="10" height="10" fill="red"/></svg>)html",
          20, 20);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_EQ(rendered.at(19, 19).r, 255) << "the viewBox was scaled to fill";
  EXPECT_EQ(rendered.at(2, 2).r, 255) << "including the top-left, which a mis-signed fit would miss";
}

TEST(SvgRender, CentresAViewBoxWhoseAspectRatioDiffers) {
  // `xMidYMid meet` is the default: a square viewBox in a wide viewport is centred with bars either side, not stretched.
  const Rendered rendered(
          R"html(<svg width="40" height="20" viewBox="0 0 10 10"><rect width="10" height="10" fill="red"/></svg>)html",
          40, 20);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_EQ(rendered.at(20, 10).r, 255) << "the centre is painted";
  EXPECT_EQ(rendered.at(1, 10).a, 0) << "the left bar is not, because meet does not stretch";
  EXPECT_EQ(rendered.at(38, 10).a, 0) << "nor the right";
}

TEST(SvgRender, StretchesAViewBoxWhenAskedTo) {
  const Rendered rendered(
          R"html(<svg width="40" height="20" viewBox="0 0 10 10" preserveAspectRatio="none">
                 <rect width="10" height="10" fill="red"/></svg>)html",
          40, 20);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_EQ(rendered.at(1, 1).r, 255) << "none stretches to the whole viewport";
  EXPECT_EQ(rendered.at(38, 18).r, 255);
}

TEST(SvgRender, OffsetsAViewBoxWithANonZeroOrigin) {
  const Rendered rendered(
          R"html(<svg width="20" height="20" viewBox="10 10 10 10"><rect x="10" y="10" width="10" height="10"
                 fill="red"/></svg>)html",
          20, 20);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_EQ(rendered.at(10, 10).r, 255) << "viewBox coordinate 10,10 is the surface origin";
}

// ==================================================================================================
// The real documents
// ==================================================================================================

TEST(SvgRender, RendersTheEmojiDocument) {
  // A red background, a green circle on it, and white text. The three are enough to check that percentages, a circle and
  // the paint stack all reach the canvas in the right order.
  const Rendered rendered(resource("emoji.svg"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  ASSERT_EQ(rendered.surface->width(), 300);
  ASSERT_EQ(rendered.surface->height(), 200);

  const auto background = rendered.at(5, 5);
  EXPECT_EQ(background.r, 255) << "the 100% by 100% red rectangle";
  EXPECT_EQ(background.g, 0);

  // Below the text baseline and inside the circle of radius 80 centred at (150,100). The fill is `green`, which in CSS
  // and in SVG is #008000 and not #00ff00 -- that one is `lime` -- so the channel is 128.
  const auto circle = rendered.at(150, 165);
  EXPECT_EQ(circle.g, 128) << "the green circle, which is #008000";
  EXPECT_EQ(circle.r, 0);
  // And the circle stops at its radius: 78 below the centre is inside, 82 below is not.
  EXPECT_EQ(rendered.at(150, 178).g, 128) << "inside the radius";
  EXPECT_EQ(rendered.at(150, 182).r, 255) << "outside it, so the red background shows";
}

TEST(SvgRender, EveryCorpusDocumentRendersWithoutFailing) {
  // Not a picture comparison -- that would need reference images this repository does not have -- but a check that no
  // document makes the renderer give up or produce nothing at all. A document that renders entirely empty is a bug in
  // the renderer even when the reason is an unimplemented feature, because the fallbacks are meant to leave something.
  const fs::path directory = fs::path(CORAL_GRAPHICS_RESOURCE_DIR) / "apitest" / "SVG";
  int rendered = 0;
  for (const auto& entry : fs::directory_iterator(directory)) {
    if (entry.path().extension() != ".svg") continue;

    std::ifstream file(entry.path(), std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();

    std::string error;
    auto parsed = Svg::parse(contents.str(), &error);
    ASSERT_TRUE(parsed.has_value()) << entry.path().filename() << ": " << error;
    ASSERT_GT(parsed->width(), 0) << entry.path().filename();
    ASSERT_GT(parsed->height(), 0) << entry.path().filename();

    const auto surface = Surface::makeRaster(static_cast<int>(parsed->width()), static_cast<int>(parsed->height()));
    ASSERT_NE(surface, nullptr) << entry.path().filename();
    parsed->render(*surface->getCanvas());
    ++rendered;
  }
  EXPECT_GE(rendered, 20) << "the corpus should be there";
}
