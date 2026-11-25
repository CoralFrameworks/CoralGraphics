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
 * @file test_SvgClipLayer.cpp
 *
 * `clip-path` and group `opacity` in a document.
 *
 * Both reach the canvas through primitives that already have their own tests; what is tested here is the wiring, and the
 * two wiring mistakes that produce plausible pictures rather than obvious ones:
 *
 *   * a group's opacity folded into each child's alpha, which is right for one child and too dark where two overlap;
 *   * a clip applied once per descendant instead of once per element, which narrows the region a little more at every
 *     level of nesting until most of the drawing is gone.
 */

#include <svg/Svg.h>

#include <graphics/Surface.h>

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace {

using graphics::Svg;
using graphics::Surface;

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

std::string document(const std::string& body, const int size = 32) {
  return "<svg width=\"" + std::to_string(size) + "\" height=\"" + std::to_string(size) + "\">" + body + "</svg>";
}

} // namespace anonymous

// ==================================================================================================
// clip-path
// ==================================================================================================

TEST(SvgClipPath, ClipsAShapeToAReferencedPath) {
  const Rendered rendered(document(R"svg(
      <defs><clipPath id="c"><rect x="0" y="0" width="16" height="32"/></clipPath></defs>
      <rect width="32" height="32" fill="red" clip-path="url(#c)"/>)svg"),
                          32, 32);
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_EQ(rendered.at(8, 16).r, 255) << "inside the clip";
  EXPECT_EQ(rendered.at(24, 16).a, 0) << "and cut off at its edge";
}

TEST(SvgClipPath, ClipsAGroupOfShapes) {
  // The clip belongs to the element that names it, so it applies to everything in the group -- which is what makes it
  // useful for clipping a whole drawing rather than one shape at a time.
  const Rendered rendered(document(R"svg(
      <defs><clipPath id="c"><circle cx="16" cy="16" r="8"/></clipPath></defs>
      <g clip-path="url(#c)">
        <rect x="0" y="0" width="32" height="16" fill="red"/>
        <rect x="0" y="16" width="32" height="16" fill="blue"/>
      </g>)svg"),
                          32, 32);
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_EQ(rendered.at(16, 12).r, 255) << "the red rectangle, inside the circle";
  EXPECT_EQ(rendered.at(16, 20).b, 255) << "the blue one, inside the circle";
  EXPECT_EQ(rendered.at(2, 16).a, 0) << "and nothing outside it";
}

TEST(SvgClipPath, IsNotAppliedTwiceToADescendant) {
  // The reference is consumed by the element that names it. A descendant that inherited the id would clip a second time
  // against its own bounding box, and a `objectBoundingBox` clip would then shrink at every level of nesting.
  const Rendered rendered(document(R"svg(
      <defs><clipPath id="c" clipPathUnits="objectBoundingBox">
        <rect x="0" y="0" width="0.5" height="1"/>
      </clipPath></defs>
      <g clip-path="url(#c)"><rect width="32" height="32" fill="red"/></g>)svg"),
                          32, 32);
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  // One application of a half-width clip on a group has no box to measure against, so it is skipped -- see the note in
  // the renderer. The shape draws in full rather than being clipped by a box that was never computed.
  EXPECT_EQ(rendered.at(24, 16).r, 255) << "the group clip had no box and was skipped";
}

TEST(SvgClipPath, MeasuresObjectBoundingBoxAgainstTheShapesOwnBox) {
  // On a shape there *is* a box, and the clip's unit coordinates scale by it. A clip covering the right half of the
  // unit box therefore covers the right half of the shape -- not of the document.
  const Rendered rendered(document(R"svg(
      <defs><clipPath id="c" clipPathUnits="objectBoundingBox">
        <rect x="0.5" y="0" width="0.5" height="1"/>
      </clipPath></defs>
      <rect x="8" y="0" width="16" height="32" fill="red" clip-path="url(#c)"/>)svg"),
                          32, 32);
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_EQ(rendered.at(20, 16).r, 255) << "the right half of the shape, which starts at x = 8";
  EXPECT_EQ(rendered.at(12, 16).a, 0) << "and the left half of it is clipped away";
}

TEST(SvgClipPath, UnionsThePathsChildren) {
  // A clip path is the union of its children, so a shape overlapping two of them is kept where either one covers it.
  const Rendered rendered(document(R"svg(
      <defs><clipPath id="c">
        <rect x="0" y="0" width="16" height="16"/>
        <rect x="16" y="16" width="16" height="16"/>
      </clipPath></defs>
      <rect width="32" height="32" fill="red" clip-path="url(#c)"/>)svg"),
                          32, 32);
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_EQ(rendered.at(8, 8).r, 255) << "inside the first child";
  EXPECT_EQ(rendered.at(24, 24).r, 255) << "inside the second";
  EXPECT_EQ(rendered.at(24, 8).a, 0) << "and in neither";
}

TEST(SvgClipPath, HonoursATransformOnTheClippingChild) {
  const Rendered rendered(document(R"svg(
      <defs><clipPath id="c">
        <rect x="0" y="0" width="8" height="8" transform="translate(16,16)"/>
      </clipPath></defs>
      <rect width="32" height="32" fill="red" clip-path="url(#c)"/>)svg"),
                          32, 32);
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_EQ(rendered.at(20, 20).r, 255) << "the translated rectangle";
  EXPECT_EQ(rendered.at(4, 4).a, 0) << "and not where it was written";
}

TEST(SvgClipPath, ANamingThatResolvesToNothingLeavesTheDocumentAlone) {
  // A reference to an id that is not there, or that names the wrong kind of element, must not clip everything away.
  const Rendered missing(document(R"svg(<rect width="32" height="32" fill="red" clip-path="url(#nope)"/>)svg"), 32, 32);
  ASSERT_TRUE(missing.drawn) << missing.error;
  EXPECT_EQ(missing.at(16, 16).r, 255) << "an unresolvable clip clips nothing";

  const Rendered wrongKind(document(R"svg(
      <defs><linearGradient id="g"><stop offset="0" stop-color="red"/><stop offset="1" stop-color="red"/>
      </linearGradient></defs>
      <rect width="32" height="32" fill="red" clip-path="url(#g)"/>)svg"),
                            32, 32);
  ASSERT_TRUE(wrongKind.drawn) << wrongKind.error;
  EXPECT_EQ(wrongKind.at(16, 16).r, 255) << "and neither does one naming a gradient";
}

// ==================================================================================================
// Group opacity
// ==================================================================================================

TEST(SvgGroupOpacity, FadesAGroupOnceRatherThanEachShape) {
  // The case the layer exists for. Two overlapping opaque rectangles in a group at half opacity: folded into each
  // child's alpha the overlap composites twice and reaches about 75%; through a layer it stays at 50%.
  const Rendered rendered(document(R"svg(
      <g opacity="0.5">
        <rect x="4" y="4" width="24" height="24" fill="red"/>
        <rect x="4" y="4" width="24" height="24" fill="red"/>
      </g>)svg"),
                          32, 32);
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_NEAR(rendered.at(16, 16).a, 128, 3) << "the overlap, faded once";
  EXPECT_NEAR(rendered.at(6, 16).a, 128, 3) << "and the rest of the group is the same shade";
}

TEST(SvgGroupOpacity, MultipliesWithAShapesOwnOpacity) {
  // A half-opacity shape inside a half-opacity group is a quarter, applied once each -- not a half, and not a half
  // applied twice at the same level.
  const Rendered rendered(document(R"svg(
      <g opacity="0.5"><rect width="32" height="32" fill="red" opacity="0.5"/></g>)svg"),
                          32, 32);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_NEAR(rendered.at(16, 16).a, 64, 4) << "a quarter";
}

TEST(SvgGroupOpacity, LeavesAFullyOpaqueGroupExactlyAlone) {
  // The layer is only opened when it is needed. An opaque group must draw the *same* pixels it would without one, so
  // this is the check that the layer path has not changed the ordinary case -- antialiased edges included.
  const Rendered withGroup(document(R"svg(<g opacity="1"><circle cx="16" cy="16" r="10" fill="red"/></g>)svg"), 32, 32);
  const Rendered without(document(R"svg(<circle cx="16" cy="16" r="10" fill="red"/>)svg"), 32, 32);
  ASSERT_TRUE(withGroup.drawn) << withGroup.error;
  ASSERT_TRUE(without.drawn) << without.error;

  for (int y = 0; y < 32; y += 3) {
    for (int x = 0; x < 32; x += 3) {
      EXPECT_EQ(withGroup.at(x, y).a, without.at(x, y).a) << "at (" << x << "," << y << ")";
      EXPECT_EQ(withGroup.at(x, y).r, without.at(x, y).r) << "at (" << x << "," << y << ")";
    }
  }
}

TEST(SvgGroupOpacity, AppliesInsideAClipPath) {
  // The two features together, and in the order that matters: the clip narrows the region and the layer fades what is
  // inside it. A layer that dropped the clip would let the faded drawing escape its bounds.
  const Rendered rendered(document(R"svg(
      <defs><clipPath id="c"><rect x="0" y="0" width="16" height="32"/></clipPath></defs>
      <g opacity="0.5" clip-path="url(#c)"><rect width="32" height="32" fill="red"/></g>)svg"),
                          32, 32);
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_NEAR(rendered.at(8, 16).a, 128, 3) << "faded, inside the clip";
  EXPECT_EQ(rendered.at(24, 16).a, 0) << "and clipped away outside it";
}

TEST(SvgGroupOpacity, NestsTwoGroups) {
  const Rendered rendered(document(R"svg(
      <g opacity="0.5"><g opacity="0.5"><rect width="32" height="32" fill="red"/></g></g>)svg"),
                          32, 32);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_NEAR(rendered.at(16, 16).a, 64, 4) << "half of a half";
}

TEST(SvgGroupOpacity, FadesAStrokeAndAFillWithoutDarkeningTheirOverlap) {
  // A shape with both a fill and a stroke draws its outline twice where they meet. Folding the alpha into each paint
  // would darken the join; a layer composites the finished shape once.
  const Rendered rendered(document(R"svg(
      <rect x="4" y="4" width="24" height="24" fill="red" stroke="red" stroke-width="4" opacity="0.5"/>)svg"),
                          32, 32);
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  // The stroke straddles the edge, so x = 4 is the middle of the top-left band where fill and stroke overlap.
  EXPECT_NEAR(rendered.at(4, 16).a, 128, 3) << "the overlap of fill and stroke";
  EXPECT_NEAR(rendered.at(16, 16).a, 128, 3) << "and the plain interior";
}

TEST(SvgGroupOpacity, FadesAGroupContainingAGradient) {
  // The layer and the shader are independent, so a gradient inside a faded group keeps its colours and loses only its
  // alpha. Asserting against the *unfaded* gradient rather than against 255 is the point: a pixel partway along a
  // gradient is not white, and a test that expected white where the gradient is grey would be testing the wrong thing.
  const std::string defs = R"svg(<defs><linearGradient id="g">
        <stop offset="0" stop-color="black"/><stop offset="1" stop-color="white"/>
      </linearGradient></defs>)svg";
  const std::string rect = R"svg(<rect width="32" height="32" fill="url(#g)"/>)svg";

  // The definitions stay outside the group in both, so the two documents differ only in the opacity.
  const Rendered plain(document(defs + rect), 32, 32);
  const Rendered faded(document(defs + "<g opacity=\"0.5\">" + rect + "</g>"), 32, 32);
  ASSERT_TRUE(plain.drawn) << plain.error;
  ASSERT_TRUE(faded.drawn) << faded.error;

  for (const int x : {2, 16, 30}) {
    EXPECT_NEAR(faded.at(x, 16).r, plain.at(x, 16).r, 3) << "the colour is unchanged at x = " << x;
    EXPECT_NEAR(faded.at(x, 16).a, plain.at(x, 16).a / 2, 3) << "and the alpha is halved at x = " << x;
  }

  // The gradient is still a gradient: the faded version ramps rather than flattening to one shade.
  EXPECT_LT(faded.at(2, 16).r, faded.at(30, 16).r) << "it still runs dark to light";
}
