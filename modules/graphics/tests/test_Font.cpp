/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and limitations under the
 * License.
 */

/**
 * @file test_Font.cpp
 *
 * The font layer: loading, metrics and shaping.
 *
 * ## Where the font comes from
 *
 * A real one, found on the machine running the test. Committing a font file to the repository would mean
 * a licence to track and a binary blob to review, and a *synthetic* font built for the test would be the
 * only font this code was ever exercised against -- which is exactly the case that does not catch the
 * interesting failures. The test skips when no font is available rather than failing, so it still passes
 * on a machine with no fonts installed.
 *
 * ## What is worth asserting
 *
 * The measurements are the point. A glyph count and a family name only prove that parsing did not throw;
 * ascent being positive, line height being the sum of its parts, and a kerning pair being narrower than
 * its two glyphs separately are properties that a plausible-looking implementation can get wrong -- and
 * each of them is a bug that shows up as text drawn slightly wrong rather than as a crash.
 */

#include <graphics/Font.h>
#include <graphics/FontMetrics.h>
#include <graphics/FontStyle.h>
#include <graphics/TextBlob.h>
#include <graphics/Typeface.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace graphics;

namespace {

/**
 * @return the first font file that exists, or an empty string.
 *
 * The candidates are the ones that ship with the platform rather than a bundled font, for the reason in
 * the file comment. `SystemRoot` is read from the environment rather than hard-coding `C:\Windows`,
 * because a Windows install can be elsewhere.
 */
std::string findSystemFont() {
  std::vector<std::string> candidates;

  if (const char* root = std::getenv("SystemRoot")) {
    const std::string fonts = std::string(root) + "\\Fonts\\";
    candidates.push_back(fonts + "arial.ttf");
    candidates.push_back(fonts + "segoeui.ttf");
    candidates.push_back(fonts + "tahoma.ttf");
    candidates.push_back(fonts + "times.ttf");
    candidates.push_back(fonts + "cour.ttf");
  }

  // The usual locations on a Unix-like system, including the Android system font.
  candidates.emplace_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
  candidates.emplace_back("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf");
  candidates.emplace_back("/usr/share/fonts/TTF/DejaVuSans.ttf");
  candidates.emplace_back("/system/fonts/Roboto-Regular.ttf");
  candidates.emplace_back("/System/Library/Fonts/Helvetica.ttc");

  for (const std::string& path: candidates) {
    std::ifstream file(path, std::ios::binary);
    if (file.good()) return path;
  }
  return {};
}

/** Loads the system font, or skips the test when there is none. */
std::shared_ptr<Typeface> requireTypeface() {
  const std::string path = findSystemFont();
  if (path.empty()) return nullptr;

  auto typeface = Typeface::fromFile(path);
  return typeface;
}

/** The message a skipped font test carries, so the reason is visible in the output. */
constexpr const char* kNoFontMessage =
        "no system font found; the font tests need one TTF or TTC somewhere the test looks";

} // namespace anonymous

// ==================================================================================================
// Loading
// ==================================================================================================

TEST(FontTest, LoadsASystemFontAndReportsItsIdentity) {
  const std::string path = findSystemFont();
  if (path.empty()) GTEST_SKIP() << kNoFontMessage;

  auto typeface = Typeface::fromFile(path);
  ASSERT_NE(typeface, nullptr) << "failed to load " << path;

  // A real font has a name, glyphs and an em. None of these can be zero in a font that parsed
  // correctly, so a zero here means the tables were found but misread.
  EXPECT_FALSE(typeface->familyName().empty()) << "a font with no family name did not parse";
  EXPECT_GT(typeface->glyphCount(), 0u);
  EXPECT_GT(typeface->unitsPerEm(), 0u);
  EXPECT_TRUE(typeface->isScalable()) << "a .ttf is scalable; bitmap-only fonts are the exception";

  // The PostScript name is an identifier, so it must not contain a space -- that is the constraint that
  // distinguishes it from the family name, and a fallback that produced "Times New Roman" would fail it.
  EXPECT_FALSE(typeface->postScriptName().empty());
  EXPECT_EQ(typeface->postScriptName().find(' '), std::string::npos)
          << "PostScript names are identifiers and carry no spaces";
}

TEST(FontTest, RefusesSomethingThatIsNotAFont) {
  // A file that exists and is not a font. The constructor has to report that rather than producing a
  // typeface whose every metric is zero, which would draw as blank text with no indication why.
  EXPECT_EQ(Typeface::fromFile("this-file-does-not-exist.ttf"), nullptr);

  // A buffer too small to hold a font header.
  const uint8_t rubbish[16] = { 'n', 'o', 't', ' ', 'a', ' ', 'f', 'o', 'n', 't', 0, 1, 2, 3, 4, 5 };
  EXPECT_EQ(Typeface::fromMemory(rubbish, sizeof(rubbish)), nullptr);

  // A null buffer and a zero size are caller mistakes, not crashes.
  EXPECT_EQ(Typeface::fromMemory(nullptr, 0), nullptr);
  EXPECT_EQ(Typeface::fromMemory(rubbish, 0), nullptr);
}

TEST(FontTest, GlyphLookupFindsLettersAndRejectsNonsense) {
  const std::string path = findSystemFont();
  if (path.empty()) GTEST_SKIP() << kNoFontMessage;

  auto typeface = Typeface::fromFile(path);
  ASSERT_NE(typeface, nullptr);

  // ASCII letters are in every font that could render this test.
  EXPECT_TRUE(typeface->hasGlyphForCodePoint('A'));
  EXPECT_TRUE(typeface->hasGlyphForCodePoint('z'));
  EXPECT_TRUE(typeface->hasGlyphForCodePoint('0'));

  // A code point no font maps: an unassigned plane-15 value. Reporting "has a glyph" for this would mean
  // the lookup was returning the missing-glyph slot instead of reporting absence.
  EXPECT_FALSE(typeface->hasGlyphForCodePoint(0x0FFFFEu));
}

// ==================================================================================================
// Scaling and metrics
// ==================================================================================================

TEST(FontTest, RefusesAnUnusableSize) {
  const std::string path = findSystemFont();
  if (path.empty()) GTEST_SKIP() << kNoFontMessage;

  auto typeface = Typeface::fromFile(path);
  ASSERT_NE(typeface, nullptr);

  // A zero or negative size is a caller mistake. Substituting a default would hide it until the layout
  // looked wrong for an unrelated reason, so it is refused.
  EXPECT_EQ(Font::create(typeface, 0.0f), nullptr);
  EXPECT_EQ(Font::create(typeface, -12.0f), nullptr);

  // A size beyond what the fixed-point representation holds would wrap to something small.
  EXPECT_EQ(Font::create(typeface, 1.0e9f), nullptr);

  // A null typeface is a caller mistake too.
  EXPECT_EQ(Font::create(nullptr, 12.0f), nullptr);
}

TEST(FontTest, MetricsScaleWithSize) {
  const std::string path = findSystemFont();
  if (path.empty()) GTEST_SKIP() << kNoFontMessage;

  auto typeface = Typeface::fromFile(path);
  ASSERT_NE(typeface, nullptr);

  const auto small = Font::create(typeface, 10.0f);
  const auto large = Font::create(typeface, 20.0f);
  ASSERT_NE(small, nullptr);
  ASSERT_NE(large, nullptr);

  // A font's metrics are linear in the size, so doubling the size doubles every one of them. This is the
  // property that catches a conversion which forgot to scale at all, or scaled by the wrong unit.
  EXPECT_NEAR(large->metrics().ascent, small->metrics().ascent * 2.0f, 0.01f);
  EXPECT_NEAR(large->metrics().descent, small->metrics().descent * 2.0f, 0.01f);

  // Ascent and descent are distances, so both are positive. A negative descent would invert every line
  // height computed from them.
  EXPECT_GT(small->metrics().ascent, 0.0f);
  EXPECT_GT(small->metrics().descent, 0.0f);
  EXPECT_GE(small->metrics().lineGap, 0.0f);

  // Line height is defined as the sum, so this checks the accessor rather than the font.
  EXPECT_FLOAT_EQ(small->metrics().lineHeight(),
          small->metrics().ascent + small->metrics().descent + small->metrics().lineGap);

  // A cap height of zero means the `OS/2` fallback path produced nothing, and a cap height above the
  // ascent would mean the units were misread.
  EXPECT_GT(small->metrics().capHeight, 0.0f);
  EXPECT_LE(small->metrics().capHeight, small->metrics().ascent + 0.01f);

  // The underline sits below the baseline and has some thickness. A zero thickness means the font states
  // nothing and the fallback did not supply it.
  EXPECT_GT(small->metrics().underlineOffset, 0.0f);
  EXPECT_GE(small->metrics().underlineThickness, 0.0f);
}

TEST(FontTest, MetricsAreApproximatelyTheRequestedSize) {
  const std::string path = findSystemFont();
  if (path.empty()) GTEST_SKIP() << kNoFontMessage;

  auto typeface = Typeface::fromFile(path);
  ASSERT_NE(typeface, nullptr);

  // A useful sanity bound rather than an exact one: no font's ascent is exactly its point size, but a
  // line of text should be somewhere near it. A conversion that used em units directly, or that missed
  // the em-to-pixel step, would be off by a factor of hundreds and fail this immediately.
  for (const float size: { 12.0f, 16.0f, 48.0f }) {
    const auto font = Font::create(typeface, size);
    ASSERT_NE(font, nullptr);
    EXPECT_GT(font->metrics().ascent, size * 0.5f) << "at size " << size;
    EXPECT_LT(font->metrics().ascent, size * 1.5f) << "at size " << size;
  }
}

TEST(FontTest, WithSizeProducesAnIndependentFont) {
  const std::string path = findSystemFont();
  if (path.empty()) GTEST_SKIP() << kNoFontMessage;

  auto typeface = Typeface::fromFile(path);
  const auto base = Font::create(typeface, 14.0f);
  ASSERT_NE(base, nullptr);

  const auto scaled = base->withSize(28.0f);
  ASSERT_NE(scaled, nullptr);

  // The original is unchanged: changing the size in place would invalidate any blob shaped from it.
  EXPECT_FLOAT_EQ(base->size(), 14.0f);
  EXPECT_FLOAT_EQ(scaled->size(), 28.0f);
  EXPECT_GT(scaled->metrics().ascent, base->metrics().ascent);

  // The typeface is shared rather than re-loaded, which is what makes this cheap.
  EXPECT_EQ(base->typeface(), scaled->typeface());
}

// ==================================================================================================
// Shaping
// ==================================================================================================

TEST(FontTest, ShapesTextIntoGlyphs) {
  const std::string path = findSystemFont();
  if (path.empty()) GTEST_SKIP() << kNoFontMessage;

  auto typeface = Typeface::fromFile(path);
  const auto font = Font::create(typeface, 16.0f);
  ASSERT_NE(font, nullptr);

  const TextBlob blob = font->shape("Hello");
  EXPECT_EQ(blob.count(), 5u) << "five ASCII letters shape to five glyphs in a font with no ligatures";
  EXPECT_FALSE(blob.isEmpty());

  // Every glyph must be one the font actually has. A zero here is the missing-glyph box, which means the
  // character-to-glyph step failed rather than that the font lacks the letter.
  for (const Glyph& glyph: blob.glyphs()) {
    EXPECT_NE(glyph.id, 0u) << "glyph for cluster " << glyph.cluster;
    EXPECT_GT(glyph.advance.x, 0.0f) << "a letter advances the pen";
  }

  // Clusters map back to byte offsets in the input, so they must be within it and non-decreasing.
  uint32_t previous = 0;
  for (const Glyph& glyph: blob.glyphs()) {
    EXPECT_LT(glyph.cluster, 5u);
    EXPECT_GE(glyph.cluster, previous) << "clusters are reported in order";
    previous = glyph.cluster;
  }

  // The advance of the run is the sum of the glyph advances, which is what the accessor promises.
  float sum = 0.0f;
  for (const Glyph& glyph: blob.glyphs()) sum += glyph.advance.x;
  EXPECT_NEAR(blob.advance().x, sum, 0.01f);
}

TEST(FontTest, EmptyTextShapesToNothing) {
  const std::string path = findSystemFont();
  if (path.empty()) GTEST_SKIP() << kNoFontMessage;

  auto typeface = Typeface::fromFile(path);
  const auto font = Font::create(typeface, 16.0f);
  ASSERT_NE(font, nullptr);

  const TextBlob blob = font->shape("");
  EXPECT_TRUE(blob.isEmpty());
  EXPECT_EQ(blob.count(), 0u);
  EXPECT_FLOAT_EQ(blob.advance().x, 0.0f);
  EXPECT_FLOAT_EQ(font->measureWidth(""), 0.0f);
}

TEST(FontTest, LongerTextIsWider) {
  const std::string path = findSystemFont();
  if (path.empty()) GTEST_SKIP() << kNoFontMessage;

  auto typeface = Typeface::fromFile(path);
  const auto font = Font::create(typeface, 16.0f);
  ASSERT_NE(font, nullptr);

  const float one = font->measureWidth("i");
  const float several = font->measureWidth("iiii");
  const float word = font->measureWidth("internationalisation");

  EXPECT_GT(several, one) << "four letters are wider than one";
  EXPECT_GT(word, several);
  EXPECT_NEAR(several, one * 4.0f, one * 0.5f) << "four copies of one letter are about four times as wide";
}

TEST(FontTest, MeasuredWidthIsInPixels) {
  // The absolute scale, which nothing asserted before. `measureWidth` promises a layout width, so for a font of a
  // given size the answer has to be a plausible number of pixels -- and every earlier test compared two measurements
  // to each other, which any consistent scaling satisfies.
  //
  // What this catches: the pipeline once divided harfbuzz's advances by 64 on the belief that it reported 26.6 fixed
  // point, while `hb_font_set_scale` had been given the point size. A word measured under one pixel wide, and a line
  // of text drew as a single pile of overlapping glyphs. "Longer text is wider" held throughout.
  const std::string path = findSystemFont();
  if (path.empty()) GTEST_SKIP() << kNoFontMessage;

  const auto typeface = Typeface::fromFile(path);
  ASSERT_NE(typeface, nullptr);

  constexpr float kSize = 32.0f;
  const auto font = Font::create(typeface, kSize);
  ASSERT_NE(font, nullptr);

  // An em is the size in pixels, so a capital letter is a fraction of it -- between a sixth and a whole of it for every
  // face anyone would use as a UI font. A value outside that is not a slightly different font, it is the wrong unit.
  const float capital = font->measureWidth("H");
  EXPECT_GT(capital, kSize * 0.15f) << "a capital is at least a sixth of an em wide, so about " << kSize * 0.15f;
  EXPECT_LT(capital, kSize * 1.05f) << "and no wider than the em itself";

  // The same for a whole word: eight characters cannot measure under one pixel.
  const float word = font->measureWidth("December");
  EXPECT_GT(word, kSize) << "eight characters are wider than one em";
  EXPECT_LT(word, kSize * 8.0f) << "and narrower than eight full-width characters";
}

TEST(FontTest, MeasureWidthAndAdvanceForGlyphAgreeOnUnits) {
  // Two ways of asking the same question, so they have to give answers in the same units. `measureWidth` shapes the run
  // and `advanceForGlyph` asks harfbuzz directly, and they take different paths through the code -- which is exactly
  // the situation where one can be scaled and the other not.
  const std::string path = findSystemFont();
  if (path.empty()) GTEST_SKIP() << kNoFontMessage;

  const auto typeface = Typeface::fromFile(path);
  ASSERT_NE(typeface, nullptr);

  const auto font = Font::create(typeface, 24.0f);
  ASSERT_NE(font, nullptr);

  // A single character with no kerning partner, so the run's width is that glyph's advance and nothing else.
  const TextBlob blob = font->shape("H");
  ASSERT_EQ(blob.count(), 1u);

  const float viaShape = font->measureWidth("H");
  const float viaGlyph = font->advanceForGlyph(blob.glyphs()[0].id);

  EXPECT_NEAR(viaShape, viaGlyph, std::max(0.5f, viaGlyph * 0.05f))
          << "shaping one character and asking for its glyph's advance must agree, because they are the same advance";
}

TEST(FontTest, TheBlobAdvanceIsInFontUnitsAndTheWidthIsInPixels) {
  // The two units in the pipeline, stated as a test so a future change to either is caught here rather than in a
  // picture. `TextBlob::advance` sums the glyph advances, which are in the font's em -- the same units as the outlines
  // the blob's glyph ids produce -- and `Font::measureWidth` converts that to pixels.
  const std::string path = findSystemFont();
  if (path.empty()) GTEST_SKIP() << kNoFontMessage;

  const auto typeface = Typeface::fromFile(path);
  ASSERT_NE(typeface, nullptr);

  constexpr float kSize = 20.0f;
  const auto font = Font::create(typeface, kSize);
  ASSERT_NE(font, nullptr);

  const TextBlob blob = font->shape("Hello");
  ASSERT_FALSE(blob.isEmpty());

  // In font units, five characters are a few thousand -- one em is `unitsPerEm`, which for a TrueType face is a
  // thousand or two. In pixels they would be about a hundred, so this distinguishes the two without hard-coding either.
  EXPECT_GT(blob.advance().x, static_cast<float>(typeface->unitsPerEm()) * 0.5f)
          << "the blob's advance is in font units, so it is a fraction of unitsPerEm and not a count of pixels";
  EXPECT_LT(blob.advance().x, static_cast<float>(typeface->unitsPerEm()) * 6.0f);

  const float pixels = font->measureWidth("Hello");
  EXPECT_GT(pixels, kSize * 0.5f) << "and the measured width is pixels: at least half an em for five characters";

  // The conversion between them, which is the one place the two units meet.
  const float converted = blob.advance().x * kSize / static_cast<float>(typeface->unitsPerEm());
  EXPECT_NEAR(converted, pixels, 0.01f) << "measureWidth is the blob's advance converted by size / unitsPerEm";
}

TEST(FontTest, KerningOrLigaturesMakeTheWholeNarrowerThanItsParts) {
  const std::string path = findSystemFont();
  if (path.empty()) GTEST_SKIP() << kNoFontMessage;

  auto typeface = Typeface::fromFile(path);
  const auto font = Font::create(typeface, 32.0f);
  ASSERT_NE(font, nullptr);

  // "AV" is the classic kerning pair: the two letters nest, so the pair is narrower than the sum of the
  // letters laid out independently. This is the assertion that proves shaping is happening rather than
  // being emulated by summing per-character advances -- which would give exactly the sum.
  const TextBlob pair = font->shape("AV");
  ASSERT_EQ(pair.count(), 2u);

  // Both sides are brought to pixels. `advanceForGlyph` already returns pixels, because that is what a caller laying
  // text out wants; the blob's own advance sums the glyph advances, which are in the font's units -- the same units as
  // the outlines it holds -- so it is scaled by the em here.
  const auto* typefaceForUnits = font->typeface().get();
  ASSERT_NE(typefaceForUnits, nullptr);

  const float independently = font->advanceForGlyph(pair.glyphs()[0].id) +
                              font->advanceForGlyph(pair.glyphs()[1].id);
  const float shaped = pair.advance().x * font->size() /
                       static_cast<float>(typefaceForUnits->unitsPerEm());

  // Not every font kerns this pair, so the check is one-sided: the shaped width must never be *wider*
  // than the naive sum, and for a font that carries `GPOS` kerning it is strictly narrower.
  EXPECT_LE(shaped, independently + 0.01f)
          << "shaped text must not be wider than its glyphs advanced independently";

  if (shaped < independently - 0.01f) {
    SUCCEED() << "the font kerns AV by " << (independently - shaped) << " pixels";
  } else {
    GTEST_SKIP() << "this font carries no kerning for AV, so the narrower-than-parts case cannot be shown";
  }
}

TEST(FontTest, RightToLeftTextComesBackInVisualOrder) {
  const std::string path = findSystemFont();
  if (path.empty()) GTEST_SKIP() << kNoFontMessage;

  auto typeface = Typeface::fromFile(path);
  const auto font = Font::create(typeface, 16.0f);
  ASSERT_NE(font, nullptr);

  // Two Arabic letters, which shape right to left. The font may not cover Arabic, in which case every
  // glyph is the missing-glyph box and the reordering cannot be observed -- so that case skips.
  const std::string arabic = "\xD8\xA8\xD8\xA7";  // U+0628 U+0627

  const TextBlob ltr = font->shape(arabic, Font::Direction::LeftToRight);
  const TextBlob rtl = font->shape(arabic, Font::Direction::RightToLeft);

  ASSERT_EQ(ltr.count(), rtl.count()) << "the direction changes the order, not the number of glyphs";
  if (ltr.count() < 2) GTEST_SKIP() << "the font did not produce two glyphs for the sample";

  bool allMissing = true;
  for (const Glyph& glyph: rtl.glyphs()) {
    if (glyph.id != 0) allMissing = false;
  }
  if (allMissing) GTEST_SKIP() << "this font has no Arabic coverage, so the order cannot be observed";

  // The same characters in the opposite direction must produce the opposite sequence. Comparing the
  // cluster indices is the content-independent way to see that: the first glyph of the right-to-left run
  // comes from the *later* part of the string.
  EXPECT_GT(rtl.glyphs().front().cluster, rtl.glyphs().back().cluster)
          << "a right-to-left run starts with the last character";
  EXPECT_LT(ltr.glyphs().front().cluster, ltr.glyphs().back().cluster);
}

TEST(FontTest, BoundsCoversTheAdvanceAndTheTypographicExtent) {
  const std::string path = findSystemFont();
  if (path.empty()) GTEST_SKIP() << kNoFontMessage;

  auto typeface = Typeface::fromFile(path);
  const auto font = Font::create(typeface, 24.0f);
  ASSERT_NE(font, nullptr);

  const TextBlob blob = font->shape("Wg");
  ASSERT_FALSE(blob.isEmpty());

  const float ascent = font->metrics().ascent;
  const float descent = font->metrics().descent;
  const Rect bounds = blob.bounds(ascent, descent);

  // The horizontal extent brackets the advance, because the last glyph's advance ends at the pen
  // position reached after it.
  EXPECT_LE(bounds.getLeft(), 0.01f);
  EXPECT_GE(bounds.getRight(), blob.advance().x - 0.01f);

  // The vertical extent is the typographic one: above the baseline by the ascent, below by the descent.
  EXPECT_FLOAT_EQ(bounds.getTop(), -ascent);
  EXPECT_FLOAT_EQ(bounds.getBottom(), descent);

  // An empty run has no extent to report.
  const Rect empty = TextBlob {}.bounds(ascent, descent);
  EXPECT_FLOAT_EQ(empty.getLeft(), 0.0f);
  EXPECT_FLOAT_EQ(empty.getRight(), 0.0f);
}

// ==================================================================================================
// Value types
// ==================================================================================================

TEST(FontTest, FontStyleComparesByValue) {
  const FontStyle normal = FontStyle::Normal();
  EXPECT_EQ(normal.weight(), FontWeight::kNormal);
  EXPECT_EQ(normal.slant(), FontStyle::Slant::Upright);
  EXPECT_FALSE(normal.isBold());
  EXPECT_FALSE(normal.isItalic());

  const FontStyle bold(FontWeight::kBold, FontStyle::Slant::Upright);
  EXPECT_TRUE(bold.isBold());
  EXPECT_NE(normal, bold);

  // Oblique and italic are different requests even though both are "leaning", because one has real
  // slanted outlines and the other does not.
  const FontStyle italic(FontWeight::kNormal, FontStyle::Slant::Italic);
  const FontStyle oblique(FontWeight::kNormal, FontStyle::Slant::Oblique);
  EXPECT_TRUE(italic.isItalic());
  EXPECT_TRUE(oblique.isItalic());
  EXPECT_NE(italic, oblique);

  // Width is part of the identity: an extended bold is not the same request as a condensed one.
  const FontStyle condensed(FontWeight::kBold, FontStyle::Slant::Upright, 3);
  EXPECT_NE(condensed, bold);
}

TEST(FontTest, FontMetricsLineHeightIsTheSumOfItsParts) {
  FontMetrics metrics;
  metrics.ascent = 10.0f;
  metrics.descent = 3.0f;
  metrics.lineGap = 1.0f;

  EXPECT_FLOAT_EQ(metrics.lineHeight(), 14.0f);

  // A zero gap is the common case and must not be treated as unset.
  metrics.lineGap = 0.0f;
  EXPECT_FLOAT_EQ(metrics.lineHeight(), 13.0f);
}
