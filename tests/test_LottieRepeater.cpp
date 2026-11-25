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
 * @file test_LottieRepeater.cpp
 *
 * Lottie's repeater.
 *
 * The assertions are about **where the copies land and how big they are**, because three of the transform's four terms
 * are scaled by the copy index and each is scaled a *different* way:
 *
 *   * the position is `t * position`, so an offset of two with a position of 50 puts the copy 100 across;
 *   * the rotation is `t * rotation`, so each copy turns a further `r` degrees;
 *   * the scale is `scale^t` -- raised to the index, not multiplied by it -- so three copies at 50% are 100%, 50% and
 *     25%, and a version that multiplied would make the third the same size as the first.
 *
 * Every document here puts the repeater's **anchor at the shape's own centre**. That is not cosmetic: the transform ends
 * with `Translate(-anchor)`, so with the anchor at the origin a scaled or rotated copy also *moves*, toward the origin.
 * The first version of these tests left the anchor at the origin and assumed a scaled copy stayed where it was, so every
 * expectation about its position was wrong.
 */

#include "lottie/LottieModel.h"

#include <lottie/Lottie.h>

#include <graphics/Pixmap.h>
#include <graphics/Surface.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifndef CORAL_GRAPHICS_RESOURCE_DIR
#error "CORAL_GRAPHICS_RESOURCE_DIR must be defined by the build; see CMakeLists.txt"
#endif

namespace {

namespace fs = std::filesystem;

using graphics::Lottie;
using graphics::Rect;
using graphics::Scalar;
using graphics::Surface;
using graphics::lottie::Animation;
using graphics::lottie::Layer;
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

// ==================================================================================================
// Documents written here, where a coordinate is a pixel
// ==================================================================================================

/** The centre every document here uses, so that the arithmetic is about one rectangle. */
constexpr int kCentre = 100;

/** A rectangle of @p w by @p h centred at (@p cx, @p cy), as `rc` writes one. */
std::string rectangle(const int w, const int h, const int cx = kCentre, const int cy = kCentre) {
  return R"({"ty":"rc","s":{"a":0,"k":[)" + std::to_string(w) + "," + std::to_string(h) +
         R"(]},"p":{"a":0,"k":[)" + std::to_string(cx) + "," + std::to_string(cy) + R"(]},"r":{"a":0,"k":0},"d":1})";
}

/** An opaque fill. */
std::string fill() { return R"({"ty":"fl","c":{"a":0,"k":[1,0,0,1]},"o":{"a":0,"k":100},"r":1})"; }

/**
 * A repeater whose anchor is the shape's centre.
 *
 * @param count how many copies and @p offset the index the series starts at; the rest are the format's transform
 *        values -- position, scale as a percentage, rotation in degrees, and the two opacities as percentages.
 */
std::string repeater(const int count, const int offset = 0, const int px = 0, const int py = 0, const int scaleX = 100,
                     const int scaleY = 100, const int rotation = 0, const int startOpacity = 100,
                     const int endOpacity = 100, const int mode = 1) {
  return R"({"ty":"rp","c":{"a":0,"k":)" + std::to_string(count) + R"(},"o":{"a":0,"k":)" + std::to_string(offset) +
         R"(},"m":)" + std::to_string(mode) + R"(,"tr":{"a":{"a":0,"k":[)" + std::to_string(kCentre) + "," +
         std::to_string(kCentre) + R"(]},"p":{"a":0,"k":[)" + std::to_string(px) + "," + std::to_string(py) +
         R"(]},"s":{"a":0,"k":[)" + std::to_string(scaleX) + "," + std::to_string(scaleY) +
         R"(]},"r":{"a":0,"k":)" + std::to_string(rotation) + R"(},"so":{"a":0,"k":)" + std::to_string(startOpacity) +
         R"(},"eo":{"a":0,"k":)" + std::to_string(endOpacity) + R"(}}})";
}

/** A shape layer holding @p items in one group. */
std::string shapeLayer(const std::string& items) {
  return R"({"ty":4,"ind":1,"ip":0,"op":60,"st":0,"ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},)"
         R"("p":{"a":0,"k":[0,0,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},)"
         R"("shapes":[{"ty":"gr","it":[)" +
         items + R"(]}]})";
}

/** A @p size by @p size composition, rendered at the same size so a coordinate is a pixel. */
std::string composition(const std::string& layers, const int size = 200) {
  return R"({"v":"5.5.2","fr":60,"ip":0,"op":60,"w":)" + std::to_string(size) + R"(,"h":)" + std::to_string(size) +
         R"(,"nm":"test","ddd":0,"layers":[)" + layers + R"(]})";
}

/** A rectangle at the centre, filled, and repeated. */
std::string repeated(const std::string& repeaterJson, const int width = 20, const int height = 20) {
  return composition(shapeLayer(rectangle(width, height) + "," + fill() + "," + repeaterJson));
}

/** A rendered frame, fitted to the surface. */
class Rendered {
  public:
  Rendered(const char* name, const Scalar seconds, const int size = 200) {
    const std::string text = readAsset(name);
    if (text.empty()) {
      error = "could not read the asset";
      return;
    }
    load(text, seconds, size);
  }

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
  [[nodiscard]] bool ink(const int x, const int y) const { return alpha(x, y) > 8; }

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

/** Every repeater in a document, found at any depth -- one of the assets nests them inside groups. */
std::vector<const graphics::lottie::Repeater*> repeatersIn(const Animation& animation) {
  std::vector<const graphics::lottie::Repeater*> found;

  const std::function<void(const std::vector<graphics::lottie::ShapePtr>&)> walk =
          [&found, &walk](const std::vector<graphics::lottie::ShapePtr>& items) {
            for (const auto& item : items) {
              if (item == nullptr) continue;
              if (item->repeater) found.push_back(&*item->repeater);
              walk(item->items);
            }
          };

  for (const Layer& layer : animation.layers) walk(layer.shapes);
  return found;
}

} // namespace anonymous

// ==================================================================================================
// The position and the offset
// ==================================================================================================

TEST(LottieRepeater, ARepeaterWithOneCopyAndNoOffsetChangesNothing) {
  // The identity case: one copy, no transform, so the rectangle is exactly where it would have been. The plain document
  // is rendered alongside, because "the same as without a repeater" is the claim and it is checkable directly.
  const Rendered plain(composition(shapeLayer(rectangle(20, 20) + "," + fill())), 0, 200);
  const Rendered withRepeater(repeated(repeater(1)), 0, 200);
  ASSERT_TRUE(plain.loaded) << plain.error;
  ASSERT_TRUE(withRepeater.loaded) << withRepeater.error;

  EXPECT_EQ(withRepeater.coveredPixels(), plain.coveredPixels());
  EXPECT_TRUE(withRepeater.ink(100, 100)) << "the rectangle's centre is still its centre";
}

TEST(LottieRepeater, EachCopyMovesByThePositionTimesItsIndex) {
  // `t * position` with a position of 50 across: three copies centred at 100, 150 and 200. The last is half off the
  // surface, which is why the sample is at 195 rather than 200.
  const Rendered rendered(repeated(repeater(3, 0, 50, 0)), 0, 200);
  ASSERT_TRUE(rendered.loaded) << rendered.error;
  ASSERT_TRUE(rendered.rendered);

  EXPECT_TRUE(rendered.ink(100, 100)) << "the first copy, where the rectangle was";
  EXPECT_TRUE(rendered.ink(150, 100)) << "the second, fifty across";
  EXPECT_TRUE(rendered.ink(195, 100)) << "and the third, a hundred across and half off the surface";

  EXPECT_FALSE(rendered.ink(125, 100)) << "nothing between the first two";
  EXPECT_FALSE(rendered.ink(175, 100)) << "and nothing between the last two";
}

TEST(LottieRepeater, TheOffsetMultipliesThePositionRatherThanDisplacingIt) {
  // The assertion that separates `t * position` from `position + t`. With an offset of two and a single copy, `t` is
  // two and the copy sits at `2 * 50 = 100` across -- x 190 to 210, of which the half from 190 is on the surface.
  //
  // Reading the offset as an addition would put it at 50 + 2 = 52, and ignoring the offset at 50: both land around
  // x 140 to 160, which is what the second assertion rules out.
  const Rendered rendered(repeated(repeater(1, 2, 50, 0)), 0, 200);
  ASSERT_TRUE(rendered.loaded) << rendered.error;

  EXPECT_TRUE(rendered.ink(195, 100)) << "a hundred across";
  EXPECT_FALSE(rendered.ink(150, 100)) << "so nothing where an added or ignored offset would have put it";
  EXPECT_FALSE(rendered.ink(100, 100)) << "and nothing where the rectangle started";
}

TEST(LottieRepeater, TheTwoModesAreIndistinguishableWithASingleColour) {
  // Recorded rather than asserted away, because the reason is a fact about compositing rather than a gap here.
  //
  // `m: 1` paints the copies back to front and `m: 2` front to back, so the two orders are opposite -- but they pair the
  // *same* copy index with the same position and opacity, and compositing two layers of one colour is commutative:
  // `a + b(1 - a)` equals `b + a(1 - b)`. So the two modes give identical pixels.
  //
  // Measured below rather than argued: the two documents differ only in `m`, and every pixel agrees.
  //
  // The mode becomes visible only when the repeated draws hold more than one *colour*, so that which copy is on top
  // changes which colour shows. Neither asset in the corpus does that.
  const Rendered below(repeated(repeater(2, 0, 10, 0, 100, 100, 0, 100, 0, 1)), 0, 200);
  const Rendered above(repeated(repeater(2, 0, 10, 0, 100, 100, 0, 100, 0, 2)), 0, 200);
  ASSERT_TRUE(below.loaded) << below.error;
  ASSERT_TRUE(above.loaded) << above.error;

  const auto belowPixels = below.surface->peekPixels();
  const auto abovePixels = above.surface->peekPixels();
  int differences = 0;
  for (int y = 0; y < belowPixels.height(); ++y) {
    for (int x = 0; x < belowPixels.width(); ++x) {
      if (belowPixels.getPixelARGB32(x, y) != abovePixels.getPixelARGB32(x, y)) ++differences;
    }
  }
  EXPECT_EQ(differences, 0) << "one colour, two orders, the same pixels -- which is the point of the note above";

  // The copies really are at two opacities and two positions, so the agreement is not because nothing was drawn.
  EXPECT_GT(below.coveredPixels(), 0);
  EXPECT_GT(below.alpha(95, 100), 200) << "the first copy is opaque";
  EXPECT_NEAR(below.alpha(115, 100), 128, 12) << "and the second is half, where it is not overlapped";
}

// ==================================================================================================
// The scale and the rotation, which compound
// ==================================================================================================

TEST(LottieRepeater, TheScaleCompoundsRatherThanAccumulating) {
  // `pow(scale, t)` with a scale of 50%: the copies are 100%, 50% and 25% of the rectangle, posted 40 apart. The third
  // is five pixels wide and centred on x = 180, so it spans 177.5 to 182.5.
  //
  // Multiplying the scale by the index instead would give 100%, 50% and 100% again, and that third copy would span the
  // full twenty pixels from 170 to 190. The assertion at 185 is what tells the two apart.
  const Rendered rendered(repeated(repeater(3, 0, 40, 0, 50, 50)), 0, 200);
  ASSERT_TRUE(rendered.loaded) << rendered.error;

  EXPECT_TRUE(rendered.ink(180, 100)) << "the third copy's centre, reached either way";
  EXPECT_FALSE(rendered.ink(185, 100))
          << "but not four pixels off centre: a quarter-size copy ends at 182.5, an un-compounded one would reach 190";

  EXPECT_TRUE(rendered.ink(100, 100)) << "the first copy, full size";
  EXPECT_FALSE(rendered.ink(112, 100)) << "and it really is only twenty wide";
}

TEST(LottieRepeater, TheRotationCompounds) {
  // `t * rotation` at 90 degrees, on a rectangle twice as wide as it is tall. The first copy spans x 80 to 120 and
  // y 95 to 105; the second, turned a quarter about the centre, spans x 95 to 105 and y 80 to 120.
  //
  // So a point above the first copy but inside the second proves the rotation happened, and a point inside the first
  // proves it is still there.
  const Rendered rendered(repeated(repeater(2, 0, 0, 0, 100, 100, 90), 40, 10), 0, 200);
  ASSERT_TRUE(rendered.loaded) << rendered.error;

  EXPECT_TRUE(rendered.ink(85, 100)) << "the first copy, wide";
  EXPECT_FALSE(rendered.ink(85, 85)) << "but not tall: y 85 is outside its ten-pixel height";
  EXPECT_TRUE(rendered.ink(100, 85)) << "the second copy is tall, so it reaches y 85";
  EXPECT_FALSE(rendered.ink(115, 85)) << "but it is only ten wide, so it does not reach x 115";
}

TEST(LottieRepeater, ARotationThatDidNotCompoundWouldMissTheThirdCopy) {
  // Three copies a third of a turn apart in total: 0, 120 and 240 degrees. An un-scaled rotation would put all three
  // at 120 degrees, so the union would be that one rotated copy -- which covers strictly less than the union of three.
  const Rendered compounding(repeated(repeater(3, 0, 0, 0, 100, 100, 120), 40, 10), 0, 200);
  const Rendered single(repeated(repeater(1, 0, 0, 0, 100, 100, 120), 40, 10), 0, 200);
  ASSERT_TRUE(compounding.loaded) << compounding.error;
  ASSERT_TRUE(single.loaded) << single.error;

  EXPECT_GT(compounding.coveredPixels(), single.coveredPixels())
          << "the union of three different angles covers more than one of them";
}

// ==================================================================================================
// The count and the opacity ramp
// ==================================================================================================

TEST(LottieRepeater, TheCountIsRoundedRatherThanTruncated) {
  // Skottie adds a half before the cast, so 2.6 copies is three. Truncating would give two, and the documents below
  // differ by exactly one copy of a 20x20 rectangle -- half of which is off the surface, since the copies are fifty
  // apart and the third sits at x 200.
  const Rendered two(repeated(repeater(2, 0, 50, 0)), 0, 200);
  const Rendered three(repeated(repeater(3, 0, 50, 0)), 0, 200);
  ASSERT_TRUE(two.loaded) << two.error;
  ASSERT_TRUE(three.loaded) << three.error;

  EXPECT_GT(three.coveredPixels(), two.coveredPixels()) << "a third copy adds ink";
  EXPECT_NEAR(three.coveredPixels() - two.coveredPixels(), 400 / 2, 60) << "and it is the visible half of one";
}

TEST(LottieRepeater, TheOpacityRampsFromTheStartToTheEnd) {
  // `so` at 100 and `eo` at 0 over two copies. Skottie's step is `(0 - 100) / 100 / 2 = -0.5`, so the copies are at
  // 100% and 50% -- divided by `count` rather than `count - 1`, which is AE's own off-by-one and is reproduced here.
  const Rendered rendered(repeated(repeater(2, 0, 50, 0, 100, 100, 0, 100, 0, 2)), 0, 200);
  ASSERT_TRUE(rendered.loaded) << rendered.error;

  const int first = rendered.alpha(100, 100);
  const int second = rendered.alpha(150, 100);
  EXPECT_GT(first, 240) << "the first copy is fully opaque";
  EXPECT_NEAR(second, 128, 12) << "and the second is halfway down the ramp";
  EXPECT_LT(second, first);
}

TEST(LottieRepeater, AnOpacityOfZeroOnEveryCopyDrawsNothing) {
  // Both ends at zero is every copy fully transparent, which should leave the surface empty -- and the copies are
  // skipped rather than drawn at zero alpha, so this is the guard on that path too.
  const Rendered rendered(repeated(repeater(3, 0, 50, 0, 100, 100, 0, 0, 0, 2)), 0, 200);
  ASSERT_TRUE(rendered.loaded) << rendered.error;
  EXPECT_EQ(rendered.coveredPixels(), 0);
}

// ==================================================================================================
// The assets
// ==================================================================================================

TEST(LottieRepeater, ParsesTheRepeaterAsset) {
  // `repeater.json` puts its repeater beside two *groups* at the layer level rather than inside either, so it repeats
  // both groups' output. That is why a repeater needs the draws and not the geometry: the geometry belongs to the groups
  // and has already been consumed by their fills.
  const Model model("repeater.json");
  ASSERT_TRUE(model.loaded) << model.error;

  const auto repeaters = repeatersIn(*model.animation);
  ASSERT_EQ(repeaters.size(), 1U);

  const auto& repeater = *repeaters.front();
  EXPECT_TRUE(repeater.count.isAnimated()) << "the asset animates the copy count";
  EXPECT_FALSE(repeater.below) << "`m: 2` is the mode Skottie calls kAbove";
  ASSERT_FALSE(repeater.scale.isEmpty());
  EXPECT_NEAR(repeater.scale.at(0).x, 95.0f, 0.5f) << "a scale of 95% per copy";
  ASSERT_FALSE(repeater.rotation.isEmpty());
  EXPECT_NEAR(repeater.rotation.at(0), 20.0f, 0.5f) << "and twenty degrees per copy";
}

TEST(LottieRepeater, ParsesTheBulgeAsset) {
  // Two repeaters, one inside each group and after the fill it repeats. Fifty copies each, starting at index -25.
  const Model model("repeater_bulge.json");
  ASSERT_TRUE(model.loaded) << model.error;

  const auto repeaters = repeatersIn(*model.animation);
  ASSERT_EQ(repeaters.size(), 2U);
  for (const auto* repeater : repeaters) {
    ASSERT_FALSE(repeater->count.isEmpty());
    EXPECT_NEAR(repeater->count.at(0), 50.0f, 0.5f) << "fifty copies";
    ASSERT_FALSE(repeater->offset.isEmpty());
    EXPECT_NEAR(repeater->offset.at(0), -25.0f, 0.5f) << "starting at index minus twenty-five";
    EXPECT_TRUE(repeater->below) << "`m: 1`";
  }

  // The two march in different directions, which is what makes the asset a bulge rather than a smear.
  ASSERT_FALSE(repeaters[0]->position.isEmpty());
  ASSERT_FALSE(repeaters[1]->position.isEmpty());
  EXPECT_NE(repeaters[0]->position.at(0).x, repeaters[1]->position.at(0).x);
  EXPECT_NE(repeaters[0]->position.at(0).y, repeaters[1]->position.at(0).y);
}

TEST(LottieRepeater, DrawsTheRepeaterAsset) {
  const Rendered rendered("repeater.json", 0.0f, 200);
  ASSERT_TRUE(rendered.loaded) << rendered.error;
  ASSERT_TRUE(rendered.rendered);
  EXPECT_GT(rendered.coveredPixels(), 100) << "it draws";
}

TEST(LottieRepeater, DrawsTheBulgeAsset) {
  // Fifty copies of each of two rectangles, marching ten at a time. Fifty copies cover far more than one, which is the
  // assertion that the count is honoured rather than dropped.
  const Rendered rendered("repeater_bulge.json", 0.0f, 200);
  ASSERT_TRUE(rendered.loaded) << rendered.error;
  ASSERT_TRUE(rendered.rendered);
  EXPECT_GT(rendered.coveredPixels(), 1000) << "fifty copies are not one copy";
}

// ==================================================================================================
// The asset that was already there
// ==================================================================================================

TEST(LottieRepeater, StillPlaysTheGearAnimation) {
  // The regression guard, and it matters more than usual this time: deferring the painting touched every drawing path
  // in the renderer, and the draw order within a group changed with it.
  const Rendered rendered("gear.json", 0.0f, 200);
  ASSERT_TRUE(rendered.loaded) << rendered.error;
  EXPECT_GT(rendered.coveredPixels(), 100);
}
