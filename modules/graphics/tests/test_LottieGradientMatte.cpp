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
 * @file test_LottieGradientMatte.cpp
 *
 * Lottie gradients, solid layers and track mattes.
 *
 * ## Why this file writes documents as well as reading assets
 *
 * Two of the three assets here are Skia's own -- `gradient_opacity.json` and `luma_matte.json`. Both turn out to be made
 * entirely of Lottie's *parametric* shapes: rectangles (`rc`), ellipses (`el`) and stars (`sr`). This library implements
 * none of those, so neither asset draws anything, and a feature cannot be verified through a document that produces no
 * pixels for an unrelated reason.
 *
 * So the work is split in two, and the split is the honest one:
 *
 *   * the real assets are asserted at the **model** level -- they parse, their layers and gradients are read correctly,
 *     and their structure is what the format says. What they *draw* is asserted to be nothing, with the missing
 *     primitives named, so those assertions fail the day the primitives land;
 *   * the features themselves are verified through small documents written here, built from `sh` -- a path, which this
 *     library does support -- so a gradient or a matte implemented wrongly fails for the right reason.
 */

#include "lottie/LottieModel.h"

#include <lottie/Lottie.h>

#include <graphics/Pixmap.h>
#include <graphics/Surface.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef CORAL_GRAPHICS_RESOURCE_DIR
#error "CORAL_GRAPHICS_RESOURCE_DIR must be defined by the build; see modules/graphics/CMakeLists.txt"
#endif

namespace {

namespace fs = std::filesystem;

using graphics::Lottie;
using graphics::Scalar;
using graphics::Surface;
using graphics::lottie::Animation;
using graphics::lottie::Layer;
using graphics::lottie::ShapeKind;
using graphics::lottie::parseAnimation;

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

std::string number(const double value) {
  char buffer[24];
  std::snprintf(buffer, sizeof(buffer), "%.4g", value);
  return buffer;
}

// ==================================================================================================
// Documents written here, out of the one shape primitive that is supported
// ==================================================================================================

/**
 * A closed polygon path from a list of vertices, with no tangents.
 *
 * `sh` is the only geometry this library draws, so every document below is built from it. `i` and `o` are the in and out
 * tangents, relative to each vertex; zeroes make each segment a straight line.
 */
std::string polygon(const std::vector<std::pair<int, int>>& vertices) {
  std::string points;
  std::string zeroes;
  for (size_t i = 0; i < vertices.size(); ++i) {
    if (i != 0) {
      points += ",";
      zeroes += ",";
    }
    points += "[" + std::to_string(vertices[i].first) + "," + std::to_string(vertices[i].second) + "]";
    zeroes += "[0,0]";
  }
  return R"({"ty":"sh","ks":{"a":0,"k":{"c":true,"v":[)" + points + R"(],"i":[)" + zeroes + R"(],"o":[)" + zeroes +
         R"(]}}})";
}

/** A solid colour fill. */
std::string fill(const int red, const int green, const int blue) {
  const auto channel = [](const int value) { return number(static_cast<double>(value) / 255.0); };
  return R"({"ty":"fl","c":{"a":0,"k":[)" + channel(red) + "," + channel(green) + "," + channel(blue) +
         R"(,1]},"o":{"a":0,"k":100},"r":1})";
}

/** The consolidated stop vector: `p` colour records of (position, r, g, b), then (position, alpha) records. */
std::string stopArray(const std::vector<std::array<double, 4>>& colors,
                      const std::vector<std::array<double, 2>>& opacities) {
  std::string stops;
  for (const auto& color : colors) {
    for (const double value : color) stops += number(value) + ",";
  }
  for (const auto& opacity : opacities) {
    for (const double value : opacity) stops += number(value) + ",";
  }
  if (!stops.empty()) stops.pop_back();
  return stops;
}

/** A gradient fill from (`startX`, `startY`) to (`endX`, `endY`). `type` 1 is linear and 2 is radial. */
std::string gradientFill(const std::vector<std::array<double, 4>>& colors,
                         const std::vector<std::array<double, 2>>& opacities, const int startX, const int startY,
                         const int endX, const int endY, const int type = 1) {
  return R"({"ty":"gf","o":{"a":0,"k":100},"r":1,"t":)" + std::to_string(type) + R"(,"g":{"p":)" +
         std::to_string(colors.size()) + R"(,"k":{"a":0,"k":[)" + stopArray(colors, opacities) +
         R"(]}},"s":{"a":0,"k":[)" + std::to_string(startX) + "," + std::to_string(startY) + R"(]},"e":{"a":0,"k":[)" +
         std::to_string(endX) + "," + std::to_string(endY) + R"(]}})";
}

/** A gradient stroke of the given width. */
std::string gradientStroke(const std::vector<std::array<double, 4>>& colors,
                           const std::vector<std::array<double, 2>>& opacities, const int startX, const int startY,
                           const int endX, const int endY, const int width) {
  return R"({"ty":"gs","o":{"a":0,"k":100},"w":{"a":0,"k":)" + std::to_string(width) + R"(},"t":1,"g":{"p":)" +
         std::to_string(colors.size()) + R"(,"k":{"a":0,"k":[)" + stopArray(colors, opacities) +
         R"(]}},"s":{"a":0,"k":[)" + std::to_string(startX) + "," + std::to_string(startY) + R"(]},"e":{"a":0,"k":[)" +
         std::to_string(endX) + "," + std::to_string(endY) + R"(]}})";
}

/** A shape layer holding @p shapes in one group, at the composition's origin. */
std::string shapeLayer(const int index, const std::string& shapes, const int trackMatte = 0) {
  const std::string matte = trackMatte == 0 ? std::string {} : R"(,"tt":)" + std::to_string(trackMatte);
  return R"({"ty":4,"ind":)" + std::to_string(index) + R"(,"ip":0,"op":60,"st":0)" + matte +
         R"(,"ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"p":{"a":0,"k":[0,0,0]},"a":{"a":0,"k":[0,0,0]},)"
         R"("s":{"a":0,"k":[100,100,100]}},"shapes":[{"ty":"gr","it":[)" +
         shapes + R"(]}]})";
}

/** A rectangle, as `rc` writes one: a size, a centre and a corner radius. */
std::string rectangle(const int width, const int height, const int centreX, const int centreY, const int radius = 0) {
  return R"({"ty":"rc","s":{"a":0,"k":[)" + std::to_string(width) + "," + std::to_string(height) +
         R"(]},"p":{"a":0,"k":[)" + std::to_string(centreX) + "," + std::to_string(centreY) +
         R"(]},"r":{"a":0,"k":)" + std::to_string(radius) + R"(},"d":1})";
}

/** An ellipse, as `el` writes one. */
std::string ellipse(const int width, const int height, const int centreX, const int centreY) {
  return R"({"ty":"el","s":{"a":0,"k":[)" + std::to_string(width) + "," + std::to_string(height) +
         R"(]},"p":{"a":0,"k":[)" + std::to_string(centreX) + "," + std::to_string(centreY) + R"(]},"d":1})";
}

/** A star or polygon, as `sr` writes one. */
std::string polystar(const int points, const int outer, const int inner, const int centreX, const int centreY,
                     const bool star = true) {
  return R"({"ty":"sr","sy":)" + std::to_string(star ? 1 : 2) + R"(,"pt":{"a":0,"k":)" + std::to_string(points) +
         R"(},"p":{"a":0,"k":[)" + std::to_string(centreX) + "," + std::to_string(centreY) +
         R"(]},"r":{"a":0,"k":0},"or":{"a":0,"k":)" + std::to_string(outer) + R"(},"ir":{"a":0,"k":)" +
         std::to_string(inner) + R"(},"os":{"a":0,"k":0},"is":{"a":0,"k":0}})";
}

/** A composition of @p size by @p size holding @p layers. */
std::string composition(const std::string& layers, const int size = 100) {
  return R"({"v":"5.5.2","fr":60,"ip":0,"op":60,"w":)" + std::to_string(size) + R"(,"h":)" + std::to_string(size) +
         R"(,"nm":"test","ddd":0,"layers":[)" + layers + R"(]})";
}

/** Every corner of the composition, as one polygon. */
std::vector<std::pair<int, int>> wholeCanvas() { return {{0, 0}, {100, 0}, {100, 100}, {0, 100}}; }

// ==================================================================================================
// Reading an asset, and rendering a document
// ==================================================================================================

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

class Rendered {
  public:
  explicit Rendered(const std::string& document, const int size = 100) {
    if (document.empty()) {
      error = "empty document";
      return;
    }
    auto parsed = Lottie::parse(document, &error);
    if (!parsed) return;
    lottie = std::move(*parsed);
    loaded = true;
    draw(size);
  }

  void draw(const int size = 100) {
    surface = Surface::makeRaster(size, size);
    if (surface == nullptr) return;
    // The *three*-argument form, which fits the composition into the rectangle given. The two-argument one draws at the
    // composition's own size at the origin -- which is what its documentation says, and what an earlier version of this
    // helper got wrong: a 500x500 asset drawn 1:1 into a 200x200 surface is mostly off-screen.
    //
    // `Rect` holds floats, so the extent is converted rather than passed as an `int` -- which is what the compiler
    // warned about when it was not.
    const auto extent = static_cast<graphics::Scalar>(size);
    lottie->render(*surface->getCanvas(), graphics::Rect::MakeLTRB(0, 0, extent, extent), 0);
    rendered = true;
  }

  [[nodiscard]] uint32_t at(const int x, const int y) const { return surface->peekPixels().getPixelARGB32(x, y); }
  [[nodiscard]] int alpha(const int x, const int y) const { return static_cast<int>((at(x, y) >> 24) & 0xFFu); }
  [[nodiscard]] int channel(const int x, const int y, const int shift) const {
    return static_cast<int>((at(x, y) >> shift) & 0xFFu);
  }

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

/** A red-to-blue linear gradient running left to right, from 0 to 1 along the axis. */
std::vector<std::array<double, 4>> redToBlue() { return {{0, 1, 0, 0}, {1, 0, 0, 1}}; }
std::vector<std::array<double, 2>> opaqueEnds() { return {{0, 1}, {1, 1}}; }

} // namespace anonymous

// ==================================================================================================
// The assets: what they hold
// ==================================================================================================

TEST(LottieGradient, ParsesTheGradientAsset) {
  const Model model("gradient_opacity.json");
  ASSERT_TRUE(model.loaded) << model.error;

  int gradientFills = 0;
  int solidLayers = 0;
  for (const Layer& layer : model.animation->layers) {
    if (layer.kind == Layer::Kind::Solid) ++solidLayers;
    // Flat in the layer: this document's `shapes` is `[rc, gf]` with no wrapping group. An earlier version of this walk
    // looked one level down, because `luma_matte.json` *does* nest inside groups -- one asset matched the assumption and
    // the other did not, which is exactly how a wrong assumption survives.
    for (const auto& shape : layer.shapes) {
      if (shape->kind == ShapeKind::GradientFill) ++gradientFills;
    }
  }

  EXPECT_EQ(model.animation->layers.size(), 5U) << "every layer is kept, including the solid";
  EXPECT_EQ(solidLayers, 1) << "the fifth layer is the white solid";
  EXPECT_EQ(gradientFills, 4) << "the four shape layers each carry a gradient fill";
}

TEST(LottieGradient, ReadsTheStopArrayAsColourRecordsThenOpacityRecords) {
  // The layout that is easiest to get wrong: `g.k` is `g.p` records of **four** floats -- position, red, green, blue --
  // followed by two-float opacity records. Reading it as a run of RGB triples shifts every stop along by one and still
  // produces a plausible-looking gradient.
  const Model model("gradient_opacity.json");
  ASSERT_TRUE(model.loaded) << model.error;

  int checked = 0;
  for (const Layer& layer : model.animation->layers) {
    for (const auto& shape : layer.shapes) {
      if (!shape->gradient) continue;
      const auto& gradient = *shape->gradient;

      ASSERT_FALSE(gradient.records.isEmpty());
      const std::vector<Scalar> values = gradient.records.at(0);
      const size_t colorSize = static_cast<size_t>(gradient.colorRecords) * 4;

      ASSERT_LE(colorSize, values.size()) << "room for the colour records the count claims";
      EXPECT_EQ((values.size() - colorSize) % 2, 0U) << "and whole opacity records after them";

      // Positions and channels are both in [0, 1], so a read at the wrong stride shows up as a value outside that range --
      // the assertion that actually catches the off-by-one the layout invites.
      for (size_t i = 0; i < static_cast<size_t>(gradient.colorRecords); ++i) {
        EXPECT_GE(values[i * 4], 0.0f) << "colour record " << i << " position";
        EXPECT_LE(values[i * 4], 1.0f) << "colour record " << i << " position";
        for (size_t channel = 1; channel < 4; ++channel) {
          EXPECT_GE(values[i * 4 + channel], -0.01f) << "record " << i << " channel " << channel;
          EXPECT_LE(values[i * 4 + channel], 1.01f) << "record " << i << " channel " << channel;
        }
      }
      EXPECT_EQ(gradient.type, 1) << "these are all linear";
      ++checked;
    }
  }
  EXPECT_EQ(checked, 4);
}

TEST(LottieMatte, ParsesTheMatteAsset) {
  const Model model("luma_matte.json");
  ASSERT_TRUE(model.loaded) << model.error;

  // Three layers, and their *kinds* matter: two of the three are solids. The parser used to skip every non-shape layer,
  // which both hid the solids and -- worse -- renumbered the list, so a matte meaning "the layer above me" would have
  // pointed at a different one.
  ASSERT_EQ(model.animation->layers.size(), 3U);
  EXPECT_EQ(model.animation->layers[0].kind, Layer::Kind::Shape);
  EXPECT_EQ(model.animation->layers[1].kind, Layer::Kind::Solid);
  EXPECT_EQ(model.animation->layers[2].kind, Layer::Kind::Solid);

  // `tt = 3` on the middle layer: a luma matte taken from the layer *above* it. Getting the direction backwards mattes
  // the star with the green square instead, which is a different picture.
  EXPECT_EQ(model.animation->layers[0].trackMatte, 0);
  EXPECT_EQ(model.animation->layers[1].trackMatte, 3);
  EXPECT_EQ(model.animation->layers[2].trackMatte, 0);
}

TEST(LottieSolid, ReadsASolidsColourFromItsHexString) {
  const Model model("luma_matte.json");
  ASSERT_TRUE(model.loaded) << model.error;

  // `sc` is "#00ff00" and "#000000". A reader that did not parse hex would leave both at their default and both solids
  // would come out the same colour -- which the matte tests would then be unable to see.
  const auto& green = model.animation->layers[1];
  EXPECT_NEAR(green.solidColor.red, 0.0f, 0.01f);
  EXPECT_NEAR(green.solidColor.green, 1.0f, 0.01f);
  EXPECT_NEAR(green.solidColor.blue, 0.0f, 0.01f);
  EXPECT_GT(green.solidWidth, 0.0f) << "and its extent was read, or it would draw nothing";

  const auto& black = model.animation->layers[2];
  EXPECT_NEAR(black.solidColor.red, 0.0f, 0.01f);
  EXPECT_NEAR(black.solidColor.green, 0.0f, 0.01f);
  EXPECT_NEAR(black.solidColor.blue, 0.0f, 0.01f);
}

// ==================================================================================================
// The assets: what they draw, which is nothing yet
// ==================================================================================================

TEST(LottieGradient, DrawsTheSkiaGradientAsset) {
  // This test used to require the opposite: that the document came out *flat*, because its four shape layers are each a
  // group holding a rectangle beside a gradient fill and the rectangle could not be built. The assertion was written to
  // fail when the geometry landed, and it did -- which is the only reason this test now reads as a positive one.
  //
  // The four layers are a red, a green, a blue and a gradient bar, each a rectangle at a different height. What is
  // asserted is that the picture varies across the composition in both directions, which a flat fill cannot do.
  const Rendered rendered(readAsset("gradient_opacity.json"), 200);
  ASSERT_TRUE(rendered.loaded) << rendered.error;
  ASSERT_TRUE(rendered.rendered);

  EXPECT_GT(rendered.coveredPixels(), 1000) << "the composition is painted";

  // Compare the colour above and below a boundary between two of the bars, and left against right within one.
  bool variesDownThePicture = false;
  bool variesAcrossThePicture = false;
  for (int y = 20; y < 180; y += 4) {
    const uint32_t here = rendered.at(20, y);
    const uint32_t below = rendered.at(20, y + 4);
    if (here != below) variesDownThePicture = true;

    const uint32_t left = rendered.at(20, y);
    const uint32_t right = rendered.at(180, y);
    if (left != right) variesAcrossThePicture = true;
  }
  EXPECT_TRUE(variesDownThePicture) << "the four bars differ from each other";
  EXPECT_TRUE(variesAcrossThePicture) << "and each gradient bar varies along its own axis";
}

TEST(LottieMatte, TheSkiaMatteAssetShowsItsContentThroughTheMattesBrightness) {
  // The document is a green solid under a black one, taking a luma matte from a shape layer holding a **blue star**, a
  // **green ellipse** and a **red rectangle**, each at 75% opacity. A luma matte reads luminance, so the three differ
  // enormously: green is 0.7154, red 0.2125 and blue 0.0721.
  //
  // That makes one document say three things at once, and it is why this asset is worth having: at the ellipse the
  // content survives strongly, at the star it barely survives, and where the matte has no geometry it does not survive
  // at all. An implementation that ignored the matte, or inverted it, or read alpha instead of luminance, fails a
  // different one of the three.
  //
  // This asset was unbuildable until `rc`, `el` and `sr` existed, and its assertion used to require the whole picture to
  // be black.
  const Rendered rendered(readAsset("luma_matte.json"), 200);
  ASSERT_TRUE(rendered.loaded) << rendered.error;
  ASSERT_TRUE(rendered.rendered);

  const auto pixels = rendered.surface->peekPixels();
  const auto greenAt = [&pixels](const int x, const int y) {
    return static_cast<int>((pixels.getPixelARGB32(x, y) >> 8) & 0xFFu);
  };

  // The green ellipse: luminance 0.7154 times 75% is about 0.54, so the green survives at roughly half.
  EXPECT_GT(greenAt(140, 100), 90) << "the content survives where the matte is a bright green ellipse";
  EXPECT_LT(greenAt(140, 100), 190) << "and it does not survive *fully*, because the matte is only 75% opaque";

  // The blue star: luminance 0.0721 times 75% is about 0.05, so only a trace.
  //
  // Sampled straight up from the centre at device (100, 60), which is composition (250, 150). The obvious place -- the
  // star's own centre -- is where all three shapes meet: the ellipse's left edge and the rectangle's top-right corner are
  // both there, so the first version of this test read a mixture and measured 87 instead of a trace.
  const int atStar = greenAt(100, 60);
  EXPECT_LT(atStar, 40) << "a blue matte passes almost nothing";
  EXPECT_LT(atStar, greenAt(140, 100)) << "and much less than the green one does";

  // Outside the matte's geometry there is nothing at all, and the black solid shows through.
  EXPECT_LT(greenAt(4, 4), 12) << "the corner is outside the matte";
  EXPECT_LT(greenAt(196, 196), 12) << "and so is the opposite one";

  // The picture is opaque everywhere, because the black solid fills the composition behind the masked content.
  EXPECT_EQ(rendered.coveredPixels(), 200 * 200) << "the black solid is not itself masked";
}

// ==================================================================================================
// Gradients, through documents made of paths
// ==================================================================================================

TEST(LottieGradient, PaintsAGradientAcrossAPath) {
  // A red-to-blue gradient from the left edge to the right, over the whole composition. The two ends must differ --
  // which a renderer that misread the stops, or used one stop for everything, would not do.
  const Rendered rendered(composition(shapeLayer(
          1, polygon(wholeCanvas()) + "," + gradientFill(redToBlue(), opaqueEnds(), 0, 50, 100, 50))));
  ASSERT_TRUE(rendered.loaded) << rendered.error;

  EXPECT_GT(rendered.coveredPixels(), 9000) << "the whole composition is painted";
  EXPECT_GT(rendered.channel(4, 50, 16), 200) << "red at the gradient's start";
  EXPECT_LT(rendered.channel(4, 50, 0), 60);
  EXPECT_GT(rendered.channel(95, 50, 0), 200) << "blue at its end";
  EXPECT_LT(rendered.channel(95, 50, 16), 60);
}

TEST(LottieGradient, InterpolatesThroughTheMiddle) {
  // The middle is neither end and lies between them in both channels -- which is what tells an interpolated gradient from
  // a hard split at the halfway point.
  const Rendered rendered(composition(shapeLayer(
          1, polygon(wholeCanvas()) + "," + gradientFill(redToBlue(), opaqueEnds(), 0, 50, 100, 50))));
  ASSERT_TRUE(rendered.loaded) << rendered.error;

  const int middleRed = rendered.channel(50, 50, 16);
  const int middleBlue = rendered.channel(50, 50, 0);
  EXPECT_GT(middleRed, 90) << "partway from red towards blue";
  EXPECT_LT(middleRed, 165);
  EXPECT_GT(middleBlue, 90);
  EXPECT_LT(middleBlue, 165);
}

TEST(LottieGradient, HonoursTheOpacityRun) {
  // The same gradient with an alpha run that fades both ends to transparent. The colour run is fully opaque, so a renderer
  // that read only the colour records would paint a solid band -- and the two readings differ most at the ends.
  const Rendered rendered(composition(shapeLayer(
          1, polygon(wholeCanvas()) + "," + gradientFill(redToBlue(), {{0, 0}, {1, 0}}, 0, 50, 100, 50))));
  ASSERT_TRUE(rendered.loaded) << rendered.error;

  EXPECT_LT(rendered.alpha(1, 50), 24) << "the start is faded out by the opacity run";
  EXPECT_LT(rendered.alpha(98, 50), 24) << "and so is the end";
  EXPECT_LT(rendered.coveredPixels(), 100) << "so almost nothing is drawn";
}

TEST(LottieGradient, PaintsAGradientStroke) {
  // `gs` is the same gradient as a stroke with a width. The outline is drawn and the middle is not -- which tells a
  // stroke from a fill.
  const Rendered rendered(composition(shapeLayer(
          1, polygon(wholeCanvas()) + "," +
                     gradientStroke(redToBlue(), opaqueEnds(), 0, 50, 100, 50, 8))));
  ASSERT_TRUE(rendered.loaded) << rendered.error;

  EXPECT_GT(rendered.alpha(50, 2), 200) << "the top edge is stroked";
  EXPECT_GT(rendered.alpha(2, 50), 200) << "and the left edge";
  EXPECT_EQ(rendered.alpha(50, 50), 0) << "and the middle is not filled";
}

TEST(LottieGradient, AStrokeOfNoWidthDrawsNothing) {
  // A zero width is what a document writes for a stroke it has faded out rather than removed. Drawing it would put a
  // hairline where the reference has nothing.
  const Rendered rendered(composition(shapeLayer(
          1, polygon(wholeCanvas()) + "," + gradientStroke(redToBlue(), opaqueEnds(), 0, 50, 100, 50, 0))));
  ASSERT_TRUE(rendered.loaded) << rendered.error;
  EXPECT_EQ(rendered.coveredPixels(), 0);
}

TEST(LottieGradient, AMalformedStopArrayDrawsNothing) {
  // The array claims two colour records but carries one, so there is no second stop and nothing coherent to paint.
  // Refused rather than guessed at, which is the difference between a missing picture and a wrong one.
  const Rendered rendered(composition(shapeLayer(
          1, polygon(wholeCanvas()) +
                     R"(,{"ty":"gf","o":{"a":0,"k":100},"r":1,"t":1,"g":{"p":2,"k":{"a":0,"k":[0,1,0,0]}},)"
                     R"("s":{"a":0,"k":[0,50]},"e":{"a":0,"k":[100,50]}})")));
  ASSERT_TRUE(rendered.loaded) << rendered.error;
  EXPECT_EQ(rendered.coveredPixels(), 0);
}

// ==================================================================================================
// Track mattes, through documents made of paths
// ==================================================================================================

/** Two layers: a matte over the left half, and a green square beneath it that takes the matte. */
std::string halfMatteDocument(const int trackMatte, const int matteLevel = 255) {
  const std::string upper = shapeLayer(1, polygon({{0, 0}, {50, 0}, {50, 100}, {0, 100}}) + "," +
                                               fill(matteLevel, matteLevel, matteLevel));
  const std::string lower = shapeLayer(2, polygon(wholeCanvas()) + "," + fill(0, 255, 0), trackMatte);
  // The matte is first and the layer that takes it second: that is what "the layer above" means.
  return composition(upper + "," + lower);
}

TEST(LottieMatte, AnAlphaMatteShowsTheContentOnlyWhereTheMatteIsOpaque) {
  const Rendered rendered(halfMatteDocument(1));
  ASSERT_TRUE(rendered.loaded) << rendered.error;
  ASSERT_TRUE(rendered.rendered);

  // The matte is an opaque white square over the left half, so the green content survives there...
  EXPECT_GT(rendered.channel(25, 50, 8), 200) << "green inside the matte";
  EXPECT_LT(rendered.channel(25, 50, 16), 60) << "and not red";
  // ...and is gone on the right, where the matte has nothing.
  EXPECT_EQ(rendered.alpha(75, 50), 0) << "nothing outside the matte";
}

TEST(LottieMatte, AnInvertedAlphaMatteShowsTheContentOnlyOutsideIt) {
  // Mode 2 is the inverted alpha matte, and this is what separates "a matte" from "a matte with the right polarity": the
  // two modes draw complementary pictures from the same document.
  const Rendered rendered(halfMatteDocument(2));
  ASSERT_TRUE(rendered.loaded) << rendered.error;

  EXPECT_EQ(rendered.alpha(25, 50), 0) << "nothing inside the matte";
  EXPECT_GT(rendered.channel(75, 50, 8), 200) << "green outside it";
}

TEST(LottieMatte, ALumaMatteFollowsTheMattesBrightness) {
  // Mode 3 reads luminance rather than alpha, so a black matte passes nothing even though it is fully opaque.
  const Rendered bright(halfMatteDocument(3, 255));
  ASSERT_TRUE(bright.loaded) << bright.error;
  const Rendered dark(halfMatteDocument(3, 0));
  ASSERT_TRUE(dark.loaded) << dark.error;

  EXPECT_GT(bright.channel(25, 50, 8), 200) << "a white matte passes the content";
  EXPECT_EQ(dark.alpha(25, 50), 0) << "a black matte passes nothing";
}

TEST(LottieMatte, ALumaMatteIsNotAnAlphaMatte) {
  // The distinction the two modes exist for, stated directly: an opaque black matte is *opaque*, so an alpha matte keeps
  // the content while a luma matte drops it. If the two were wired to the same rule, one of these fails.
  const Rendered alphaRule(halfMatteDocument(1, 0));
  ASSERT_TRUE(alphaRule.loaded) << alphaRule.error;
  const Rendered lumaRule(halfMatteDocument(3, 0));
  ASSERT_TRUE(lumaRule.loaded) << lumaRule.error;

  EXPECT_GT(alphaRule.channel(25, 50, 8), 200) << "an opaque black matte under an alpha rule still passes";
  EXPECT_EQ(lumaRule.alpha(25, 50), 0) << "and under a luma rule does not";
}

TEST(LottieMatte, TheMatteLayerIsNotAlsoDrawnOnItsOwn) {
  // A layer consumed as somebody's matte must not also be drawn. The matte is *white*, so if it were drawn as well the
  // left half would come out white rather than green.
  const Rendered rendered(halfMatteDocument(1));
  ASSERT_TRUE(rendered.loaded) << rendered.error;

  EXPECT_GT(rendered.channel(25, 50, 8), 200) << "the content's green, not the matte's white";
  EXPECT_LT(rendered.channel(25, 50, 16), 60);
}

TEST(LottieMatte, ALayerWithNoMatteIsNotHidden) {
  // The other direction, which a matte implementation that hid the layer above unconditionally would fail: with no `tt`
  // anywhere, the white square and the green square both draw, and the white one is on top.
  const std::string upper = shapeLayer(1, polygon({{0, 0}, {50, 0}, {50, 100}, {0, 100}}) + "," + fill(255, 255, 255));
  const std::string lower = shapeLayer(2, polygon(wholeCanvas()) + "," + fill(0, 255, 0));
  const Rendered rendered(composition(upper + "," + lower));
  ASSERT_TRUE(rendered.loaded) << rendered.error;

  EXPECT_GT(rendered.channel(25, 50, 16), 200) << "the upper layer is drawn where it covers";
  EXPECT_GT(rendered.channel(75, 50, 8), 200) << "and the lower one where it does not";
}

// ==================================================================================================
// The asset that was already there
// ==================================================================================================


// ==================================================================================================
// The parametric shapes, drawn
// ==================================================================================================

TEST(LottieGeometryRender, DrawsARectangle) {
  const Rendered rendered(composition(shapeLayer(1, rectangle(60, 40, 50, 50) + "," + fill(255, 0, 0))));
  ASSERT_TRUE(rendered.loaded) << rendered.error;
  ASSERT_TRUE(rendered.rendered);

  EXPECT_GT(rendered.channel(50, 50, 16), 200) << "red at the centre";
  EXPECT_EQ(rendered.alpha(2, 2), 0) << "and nothing in the corner, outside it";
}

TEST(LottieGeometryRender, DrawsAnEllipse) {
  const Rendered rendered(composition(shapeLayer(1, ellipse(80, 80, 50, 50) + "," + fill(0, 255, 0))));
  ASSERT_TRUE(rendered.loaded) << rendered.error;

  EXPECT_GT(rendered.channel(50, 50, 8), 200) << "green at the centre";
  EXPECT_EQ(rendered.alpha(2, 2), 0) << "and nothing at the corner of its bounding box, which an ellipse misses";
}

TEST(LottieGeometryRender, DrawsAStar) {
  // A five-pointed star of outer radius 40 about the centre. The point straight up is inside; a corner of the
  // composition is not.
  const Rendered rendered(composition(shapeLayer(1, polystar(5, 40, 15, 50, 50) + "," + fill(0, 0, 255))));
  ASSERT_TRUE(rendered.loaded) << rendered.error;

  EXPECT_GT(rendered.channel(50, 12, 0), 200) << "blue on the point that aims up";
  EXPECT_EQ(rendered.alpha(3, 3), 0) << "and nothing in the corner";
}

TEST(LottieGeometryRender, DrawsAPolygonRatherThanAStar) {
  // `sy: 2` is a plain polygon: the same outer radius with no inner one, so it fills the notches a star would leave.
  const Rendered star(composition(shapeLayer(1, polystar(5, 40, 15, 50, 50, true) + "," + fill(0, 0, 255))));
  const Rendered polygon(composition(shapeLayer(1, polystar(5, 40, 15, 50, 50, false) + "," + fill(0, 0, 255))));
  ASSERT_TRUE(star.loaded) << star.error;
  ASSERT_TRUE(polygon.loaded) << polygon.error;

  EXPECT_GT(polygon.coveredPixels(), star.coveredPixels()) << "the polygon fills the notches the star leaves";
}

TEST(LottieMatte, StillPlaysTheGearAnimation) {
  // The regression guard: the asset the PC demo plays must be untouched by any of this.
  const Rendered rendered(readAsset("gear.json"), 200);
  ASSERT_TRUE(rendered.loaded) << rendered.error;
  EXPECT_GT(rendered.coveredPixels(), 100) << "gear.json still draws";
}
