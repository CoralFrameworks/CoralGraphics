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
 * @file test_SvgGradient.cpp
 *
 * SVG gradients, from the element to the pixels.
 *
 * The interesting property is the one that forces the shader to be built at draw time rather than at parse time: the
 * same `linearGradient` fills shapes of different sizes, and under the default `objectBoundingBox` units each one gets a
 * gradient of its own size. A renderer that resolved the gradient while parsing gives the second shape the first shape's
 * gradient -- which still looks like a gradient, and is wrong in a way only a second shape reveals.
 */

#include <svg/Svg.h>

#include <graphics/Surface.h>

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace {

using graphics::Svg;
using graphics::Surface;

/** A document rendered into a surface its own size. */
class Rendered {
  public:
  Rendered(const std::string& document, const int width, const int height) {
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

/** Wraps @p body in an `<svg>` of the given size, so the tests read as the element under test. */
std::string document(const int width, const int height, const std::string& body) {
  return "<svg width=\"" + std::to_string(width) + "\" height=\"" + std::to_string(height) + "\">" + body + "</svg>";
}

/** Black to white, left to right across whatever box it is given. */
const char* kBlackToWhite = R"svg(
  <linearGradient id="g"><stop offset="0" stop-color="black"/><stop offset="1" stop-color="white"/></linearGradient>)svg";

} // namespace anonymous

// ==================================================================================================
// The element
// ==================================================================================================

TEST(SvgGradient, FillsAShapeAcrossTheShapesOwnBox) {
  // `objectBoundingBox` is the default, so the gradient spans the rectangle rather than the whole document: the left
  // edge of the rectangle is the dark end and its right edge is the light end, whatever the document's size is.
  const Rendered rendered(document(64, 32, std::string("<defs>") + kBlackToWhite +
                                                   R"svg(</defs><rect x="16" y="0" width="32" height="32" fill="url(#g)"/>)svg"),
                          64, 32);
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_LE(rendered.at(17, 16).r, 16) << "the left edge of the rectangle is the dark end";
  EXPECT_GE(rendered.at(46, 16).r, 239) << "and its right edge is the light end";
  EXPECT_LE(rendered.at(2, 16).a, 0) << "outside the rectangle nothing is drawn";
}

TEST(SvgGradient, GivesEachShapeItsOwnBoxForTheSameGradient) {
  // Two rectangles of different sizes sharing one gradient. This is the case that a shader built at parse time gets
  // wrong: it would have sized the gradient to the first shape and reused it for the second.
  const std::string body = std::string("<defs>") + kBlackToWhite +
                           R"svg(</defs>
      <rect x="0" y="0" width="32" height="16" fill="url(#g)"/>
      <rect x="0" y="16" width="64" height="16" fill="url(#g)"/>)svg";
  const Rendered rendered(document(64, 32, body), 64, 32);
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  // The narrow rectangle reaches white by its own right edge at x = 32.
  EXPECT_GE(rendered.at(30, 8).r, 230) << "the small rectangle completes its gradient by x = 32";
  // The wide one is only halfway along at that point.
  EXPECT_NEAR(rendered.at(30, 24).r, 120, 16) << "the large one is still halfway at x = 32";
  EXPECT_GE(rendered.at(62, 24).r, 239) << "and completes by x = 64";
}

TEST(SvgGradient, HonoursUserSpaceOnUse) {
  // With `userSpaceOnUse` the coordinates are in the document's own units, so a gradient from 0 to 64 spans the
  // document however big the shape is. The rectangle covers only the left half, so it shows only the dark half.
  const std::string body = R"svg(
      <defs><linearGradient id="g" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="64" y2="0">
        <stop offset="0" stop-color="black"/><stop offset="1" stop-color="white"/></linearGradient></defs>
      <rect x="0" y="0" width="32" height="32" fill="url(#g)"/>)svg";
  const Rendered rendered(document(64, 32, body), 64, 32);
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_LE(rendered.at(2, 16).r, 16) << "the left end";
  // At x = 30, `userSpaceOnUse` has travelled 30 of 64, so about half -- not the light end the box would have given.
  EXPECT_NEAR(rendered.at(30, 16).r, 120, 16) << "still midway at the rectangle's right edge";
}

TEST(SvgGradient, ReadsStopsFromTheStyleAttributeAsWellAsAttributes) {
  const std::string body = R"svg(
      <defs><linearGradient id="g">
        <stop offset="0" style="stop-color:#000000;stop-opacity:1"/>
        <stop offset="1" style="stop-color:#ffffff"/>
      </linearGradient></defs>
      <rect width="32" height="32" fill="url(#g)"/>)svg";
  const Rendered rendered(document(32, 32, body), 32, 32);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_LE(rendered.at(1, 16).r, 16) << "the first stop, written as CSS";
  EXPECT_GE(rendered.at(30, 16).r, 239) << "the last stop";
}

TEST(SvgGradient, FadesThroughStopOpacity) {
  const std::string body = R"svg(
      <defs><linearGradient id="g">
        <stop offset="0" stop-color="red" stop-opacity="1"/>
        <stop offset="1" stop-color="red" stop-opacity="0"/>
      </linearGradient></defs>
      <rect width="32" height="32" fill="url(#g)"/>)svg";
  const Rendered rendered(document(32, 32, body), 32, 32);
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_GE(rendered.at(1, 16).a, 240) << "opaque at the first stop";
  EXPECT_LE(rendered.at(30, 16).a, 16) << "transparent at the last";
  EXPECT_NEAR(rendered.at(16, 16).a, 128, 16) << "halfway";
  // And the colour is red the whole way: the fade is in the alpha, not toward black.
  EXPECT_GE(rendered.at(16, 16).r, 240);
}

TEST(SvgGradient, ReadsAPercentageOffset) {
  const std::string body = R"svg(
      <defs><linearGradient id="g">
        <stop offset="0%" stop-color="black"/><stop offset="100%" stop-color="white"/>
      </linearGradient></defs>
      <rect width="32" height="32" fill="url(#g)"/>)svg";
  const Rendered rendered(document(32, 32, body), 32, 32);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_LE(rendered.at(1, 16).r, 16);
  EXPECT_GE(rendered.at(30, 16).r, 239);
}

TEST(SvgGradient, HonoursGradientTransform) {
  // `gradientTransform` shifts the gradient inside the shape's box, so the dark end is no longer at the left edge.
  const std::string body = R"svg(
      <defs><linearGradient id="g" gradientTransform="translate(1,0)">
        <stop offset="0" stop-color="black"/><stop offset="1" stop-color="white"/>
      </linearGradient></defs>
      <rect width="32" height="32" fill="url(#g)"/>)svg";
  const Rendered rendered(document(32, 32, body), 32, 32);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  // The gradient now spans the box from 32 to 64, so the whole rectangle is in the first half of it and never gets
  // near white.
  EXPECT_LT(rendered.at(30, 16).r, 200) << "shifted right, so the light end is off the shape";
}

TEST(SvgGradient, SpreadsWithReflectWhenAskedTo) {
  // `reflect` tiles the gradient, so a shape wider than the gradient's own extent shows it running backwards in the
  // second period rather than holding the end colour as `pad` would.
  const std::string body = R"svg(
      <defs><linearGradient id="g" spreadMethod="reflect">
        <stop offset="0" stop-color="black"/><stop offset="1" stop-color="white"/>
      </linearGradient></defs>
      <rect width="64" height="16" fill="url(#g)" transform="scale(1)"/>)svg";
  // The gradient spans the box (0..64), so `reflect` has no second period here -- the point is that the first period
  // still reads correctly rather than being disturbed by the spread mode.
  const Rendered rendered(document(64, 16, body), 64, 16);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_LE(rendered.at(1, 8).r, 16);
  EXPECT_GE(rendered.at(62, 8).r, 239);
}

TEST(SvgGradient, HonoursSpreadMethodRepeat) {
  // Repeat is easier to observe through `userSpaceOnUse`, where the gradient's extent is set independently of the shape.
  const std::string body = R"svg(
      <defs><linearGradient id="g" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="16" y2="0"
                            spreadMethod="repeat">
        <stop offset="0" stop-color="black"/><stop offset="1" stop-color="white"/>
      </linearGradient></defs>
      <rect width="64" height="16" fill="url(#g)"/>)svg";
  const Rendered rendered(document(64, 16, body), 64, 16);
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  // Each 16-unit period runs dark to light again.
  EXPECT_LE(rendered.at(1, 8).r, 24) << "the start of the first period";
  EXPECT_GE(rendered.at(14, 8).r, 231) << "the end of it";
  EXPECT_LE(rendered.at(17, 8).r, 24) << "and the second period starts dark again";
  EXPECT_GE(rendered.at(30, 8).r, 231) << "and ends light";
}

TEST(SvgGradient, DrawsARadialGradient) {
  const std::string body = R"svg(
      <defs><radialGradient id="g">
        <stop offset="0" stop-color="white"/><stop offset="1" stop-color="black"/>
      </radialGradient></defs>
      <rect width="64" height="64" fill="url(#g)"/>)svg";
  const Rendered rendered(document(64, 64, body), 64, 64);
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  // The default centre is the middle of the box and the default radius half its height, so the centre of a square
  // document is the light end and the corners are past the dark end.
  EXPECT_GE(rendered.at(32, 32).r, 239) << "light at the centre";
  EXPECT_LE(rendered.at(2, 32).r, 32) << "dark at the left edge";
  EXPECT_LE(rendered.at(32, 2).r, 32) << "and at the top";
}

TEST(SvgGradient, UsesTheFallbackColourWhenTheReferenceIsMissing) {
  // SVG's own rule: `url(#nope) red` paints red when the reference cannot be resolved. A renderer that dropped the
  // fallback would leave the shape unfilled, which is a blank picture rather than a slightly wrong colour.
  const Rendered rendered(document(32, 32, R"svg(<rect width="32" height="32" fill="url(#nope) #00ff00"/>)svg"), 32, 32);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_EQ(rendered.at(16, 16).g, 255);
}

TEST(SvgGradient, LeavesTheShapeUnfilledWhenTheReferenceIsMissingAndThereIsNoFallback) {
  const Rendered rendered(document(32, 32, R"svg(<rect width="32" height="32" fill="url(#nope)"/>)svg"), 32, 32);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_EQ(rendered.at(16, 16).a, 0);
}

TEST(SvgGradient, AnInheritedColoursAlphaDoesNotFadeTheGradient) {
  // ## A reference is not a fallback, and it is not an inherited colour either
  //
  // `fill` is inherited as a whole *value*, so an element that says `fill="url(#g)"` does not inherit its parent's colour
  // -- the paint server supplies the colour, and the fill's alpha comes from `fill-opacity` and `opacity` alone. This
  // renderer leaves the parent's colour in place when the reference has no fallback (see `applyPaint`), and for a
  // shader-masked fill the paint's colour is exactly what carries the alpha into the shader (`Paint::shader`: "`color` is
  // used only for its alpha"). So a translucent parent faded every gradient inside it, and the document never asked for
  // that.
  const std::string body = std::string("<defs>") + kBlackToWhite + R"svg(</defs>
      <g fill="rgba(255, 0, 0, 0.5)"><rect width="32" height="32" fill="url(#g)"/></g>)svg";
  const Rendered rendered(document(32, 32, body), 32, 32);
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  const auto middle = rendered.at(16, 16);
  EXPECT_EQ(middle.a, 255) << "the gradient is opaque: naming a paint server replaces the inherited paint";
  EXPECT_NEAR(middle.r, 128, 3) << "and it is the gradient's own mid grey, not the inherited red";
  EXPECT_NEAR(middle.g, 128, 3);
}

TEST(SvgGradient, StrokesThroughAGradient) {
  const std::string body = std::string("<defs>") + kBlackToWhite +
                           R"svg(</defs>
      <rect x="8" y="8" width="16" height="16" fill="none" stroke="url(#g)" stroke-width="4"/>)svg";
  const Rendered rendered(document(32, 32, body), 32, 32);
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  // The stroke is drawn and its colour varies along the box, so one end of the top edge is darker than the other.
  const auto left = rendered.at(9, 8);
  const auto right = rendered.at(23, 8);
  EXPECT_EQ(left.a, 255) << "the stroke is drawn";
  EXPECT_LT(left.r, right.r) << "and varies along the gradient";
}

TEST(SvgGradient, ScalesTheGradientWithTheShapeUnderATransform) {
  // The gradient is built in user space and the canvas's transform carries it to the device, so a shape scaled up has a
  // gradient scaled with it -- not a gradient stretched over the transformed box's bounds, which would be a different
  // picture.
  const std::string body = R"svg(
      <defs><linearGradient id="g">
        <stop offset="0" stop-color="black"/><stop offset="1" stop-color="white"/>
      </linearGradient></defs>
      <rect width="16" height="16" fill="url(#g)" transform="scale(2)"/>)svg";
  const Rendered rendered(document(32, 32, body), 32, 32);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_LE(rendered.at(1, 16).r, 16) << "dark at the scaled left edge";
  EXPECT_GE(rendered.at(30, 16).r, 239) << "light at the scaled right edge";
  EXPECT_NEAR(rendered.at(16, 16).r, 128, 16) << "and halfway in between";
}
