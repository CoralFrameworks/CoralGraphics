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
 * @file test_SvgFilterRender.cpp
 *
 * A filter applied to a document, rather than to a bitmap.
 *
 * `test_SvgFilter.cpp` asserts the arithmetic of the primitives and the chain, working on buffers directly. What it
 * cannot say is whether anything *calls* any of it, or whether the element is rendered into the buffer correctly -- the
 * transform, the region, the source graphic. That is what this file is for, and it is a separate file because the
 * failure modes are separate: arithmetic that is wrong produces a wrong picture, and wiring that is absent produces the
 * *unfiltered* picture, which looks entirely plausible.
 *
 * The document used throughout is an alpha inversion -- the corpus's own, from
 * `mask_invert_picture_image.svg` -- because it is the one filter whose result is unmistakable: the element vanishes
 * and everything else in the region becomes opaque.
 */

#include <svg/Svg.h>

#include <graphics/Pixmap.h>
#include <graphics/Surface.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifndef CORAL_GRAPHICS_RESOURCE_DIR
#error "CORAL_GRAPHICS_RESOURCE_DIR must be defined by the build; see modules/graphics/CMakeLists.txt"
#endif

namespace {

namespace fs = std::filesystem;

using graphics::Surface;
using graphics::Svg;

/** The corpus's alpha-inversion matrix: `alpha' = 1 - alpha`, colour untouched. */
const char* kInvertAlpha = "1 0 0 0 0  0 1 0 0 0  0 0 1 0 0  0 0 0 -1 1";

class Rendered {
  public:
  Rendered(const std::string& document, const int size = 40) {
    surface = Surface::makeRaster(size, size);
    if (surface == nullptr) return;
    auto parsed = Svg::parse(document, &error);
    if (!parsed) return;
    parsed->render(*surface->getCanvas());
    drawn = true;
  }

  [[nodiscard]] uint32_t at(const int x, const int y) const { return surface->peekPixels().getPixelARGB32(x, y); }
  [[nodiscard]] int alpha(const int x, const int y) const { return static_cast<int>((at(x, y) >> 24) & 0xFFu); }
  [[nodiscard]] int red(const int x, const int y) const { return static_cast<int>((at(x, y) >> 16) & 0xFFu); }

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
  bool drawn = false;
  std::unique_ptr<Surface> surface;
};

/** A 40x40 document of @p body, with the inversion filter defined. */
std::string document(const std::string& body) {
  return "<svg width=\"40\" height=\"40\"><defs>"
         "<filter id=\"inv\"><feColorMatrix type=\"matrix\" values=\"" + std::string {kInvertAlpha} +
         "\"/></filter>"
         "</defs>" + body + "</svg>";
}

fs::path assetPath(const std::string& name) {
  return fs::path(CORAL_GRAPHICS_RESOURCE_DIR) / "apitest" / name;
}

std::string readAsset(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.good()) return {};
  std::ostringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

} // namespace anonymous

// ==================================================================================================
// The wiring
// ==================================================================================================

TEST(SvgFilterRender, AFilteredShapeIsNotTheShape) {
  // The comparison that a missing wire fails: without the filter the rectangle is a red square, and with it the square
  // is transparent -- its alpha has been inverted -- and the region around it has become opaque. A renderer that parsed
  // the filter and never applied it would give the unfiltered picture for both documents.
  const std::string plain = document("<rect x=\"0\" y=\"0\" width=\"20\" height=\"20\" fill=\"red\"/>");
  const std::string withFilter =
          document("<rect x=\"0\" y=\"0\" width=\"20\" height=\"20\" fill=\"red\" filter=\"url(#inv)\"/>");

  const Rendered unfiltered(plain);
  const Rendered filtered(withFilter);
  ASSERT_TRUE(unfiltered.drawn) << unfiltered.error;
  ASSERT_TRUE(filtered.drawn) << filtered.error;

  // Unfiltered: solid red inside the square, nothing outside it.
  EXPECT_GT(unfiltered.alpha(5, 5), 200) << "the plain square is opaque";
  EXPECT_GT(unfiltered.red(5, 5), 200) << "and red";
  EXPECT_EQ(unfiltered.alpha(25, 25), 0) << "and nothing outside it";

  // Filtered: the square is gone, and the margin the region covers is opaque.
  EXPECT_EQ(filtered.alpha(5, 5), 0) << "the inversion emptied the square";
  EXPECT_GT(filtered.alpha(21, 10), 200) << "and filled the margin";
  EXPECT_NE(unfiltered.at(21, 10), filtered.at(21, 10)) << "which the unfiltered document does not";
}

TEST(SvgFilterRender, AFilterOnAGroupFiltersTheWholeGroup) {
  // `<g>` is where the corpus puts seven of its twelve filter references, and a group has no geometry -- so this is also
  // the case where an `objectBoundingBox` region has no box to be a fraction of and has to fall back to the viewport.
  //
  // The group is rendered whole and then filtered, which is what the format says: the union of the children, filtered
  // once. Filtering each child in turn would leave the second child's region unfiltered where the first had already
  // emptied it.
  const Rendered rendered(document(
          "<g filter=\"url(#inv)\">"
          "<rect x=\"0\" y=\"0\" width=\"10\" height=\"10\" fill=\"red\"/>"
          "<rect x=\"10\" y=\"0\" width=\"10\" height=\"10\" fill=\"blue\"/>"
          "</g>"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_EQ(rendered.alpha(5, 5), 0) << "the first child is emptied";
  EXPECT_EQ(rendered.alpha(15, 5), 0) << "and so is the second, so both were in the same buffer";
  EXPECT_GT(rendered.alpha(21, 10), 200) << "and the margin is filled once, not twice";
}

TEST(SvgFilterRender, AFilteredShapeInsideATransformKeepsItsPlace) {
  // The element is drawn into the buffer with the canvas's transform in force, and the result is drawn back with the
  // transform reset. Getting either wrong moves the picture: applying the transform twice doubles it, and not applying
  // it at all leaves the filter's output in the wrong place.
  //
  // It is also where the object bounds meet the region: they arrive in the coordinates the element's geometry was written
  // in and are mapped into device space before the region is derived from them.
  const Rendered rendered(document(
          "<rect x=\"0\" y=\"0\" width=\"20\" height=\"20\" fill=\"red\" transform=\"translate(10,10)\" "
          "filter=\"url(#inv)\"/>"));
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_EQ(rendered.alpha(15, 15), 0) << "the square, where the transform put it, is emptied";
  EXPECT_GT(rendered.alpha(31, 15), 200) << "and the margin beyond it is filled";
}

TEST(SvgFilterRender, AnEmptyFilterLeavesTheElementAlone) {
  // A `<filter>` with no primitives cannot be built, and the format's rule is that an unusable filter leaves the element
  // as it was -- the opposite of a mask, where a missing one hides the element. The two are easy to conflate and the
  // pictures are opposites.
  const Rendered rendered(
          "<svg width=\"40\" height=\"40\"><defs><filter id=\"empty\"/></defs>"
          "<rect x=\"0\" y=\"0\" width=\"20\" height=\"20\" fill=\"red\" filter=\"url(#empty)\"/></svg>");
  ASSERT_TRUE(rendered.drawn) << rendered.error;

  EXPECT_GT(rendered.alpha(5, 5), 200) << "the element is still drawn";
  EXPECT_GT(rendered.red(5, 5), 200) << "and still red";
}

TEST(SvgFilterRender, AnUnresolvableFilterReferenceLeavesTheElementAlone) {
  // `url(#nothing)` is a document error. The element draws as though the attribute were absent rather than vanishing.
  const Rendered rendered(
          "<svg width=\"40\" height=\"40\">"
          "<rect x=\"0\" y=\"0\" width=\"20\" height=\"20\" fill=\"red\" filter=\"url(#nothing)\"/></svg>");
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_GT(rendered.alpha(5, 5), 200);
}





// ==================================================================================================
// The corpus
// ==================================================================================================

TEST(SvgFilterRender, DrawsTheCorpusDocumentWhoseFilterStandsAlone) {
  // `mask_invert_picture_image.svg` is the simplest filter in the corpus: one `feColorMatrix`, no `in`, and no region
  // attributes at all -- so it also exercises the region defaults, which are the part of a filter that is easiest to
  // leave out. It was unrenderable before this work and was asserted as incomplete.
  const std::string text = readAsset(assetPath("mask_invert_picture_image.svg"));
  ASSERT_FALSE(text.empty()) << "the asset was read";

  const Rendered rendered(text, 200);
  ASSERT_TRUE(rendered.drawn) << rendered.error;
  EXPECT_GT(rendered.coveredPixels(), 0) << "it draws something";
}

TEST(SvgFilterRender, DrawsTheDocumentsThatUseAShadowOrABlur) {
  // The remaining four: `blur.svg` (flood, blend, blur), `complex2.svg` (five shadows), `complex4.svg` (a drop shadow)
  // and `complex7.svg` (three stacked shadows and four background blurs). Asserted to draw rather than compared, because
  // these are the documents a filter turns *nearly* upside down and there is no reference picture to compare against --
  // what is worth pinning is that a filter over a real document does not produce nothing.
  for (const char* name : {"blur.svg", "complex2.svg", "complex4.svg", "complex7.svg"}) {
    const std::string text = readAsset(assetPath(std::string {"SVG/"} + name));
    ASSERT_FALSE(text.empty()) << name << " was read";

    const Rendered rendered(text, 200);
    ASSERT_TRUE(rendered.drawn) << name << ": " << rendered.error;
    EXPECT_GT(rendered.coveredPixels(), 0) << name << " draws something";
  }
}
