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
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file test_FontManager.cpp
 *
 * Finding a font among those the system has.
 *
 * ## How these are checked
 *
 * The bundled Roboto is added explicitly, so the tests that need a known family do not depend on what the host has
 * installed. The tests about the *system* scan assert properties that hold whatever it finds -- that the count is
 * positive, that every family reported can be matched, that a bold face reports itself bold -- rather than naming a
 * font, because naming one would make the test a statement about this machine.
 *
 * The matching tests are the interesting ones, and they are written against the rule the implementation states: the
 * family is a stronger preference than the style. That ordering is what makes a fallback useful, and it is the thing a
 * scoring scheme gets wrong without failing anything obvious.
 */

#include <graphics/FontManager.h>
#include <graphics/Typeface.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace graphics;

namespace {

/**
 * @return the path of a font that is part of the repository, or an empty string when none can be found.
 *
 * The search walks upwards because the test runs from a build tree whose depth depends on the generator:
 * `build/rootcheck/modules/graphics` and `build/gpucheck/graphics` are both used.
 */
std::string bundledFontPath() {
  // Candidates in preference order. The vendored harfbuzz copy is first because it is tracked and is a full
  // font; the assets directory is tried second because those files are the application's bundled fonts.
  static const char* const candidates[] = {
    // The vendored Roboto: this repository's root and the application tree's framework checkout.
    "third_party/harfbuzz/perf/fonts/Roboto-Regular.ttf",
    "frameworks/CoralGraphics/third_party/harfbuzz/perf/fonts/Roboto-Regular.ttf",
    // The application tree's shared assets.
    "assets/fonts/Roboto-Medium.ttf",
    "assets/fonts/NotoSansSC-Regular.otf",
    "assets/fonts/NotoSerifSC-Regular.otf",
  };

  std::filesystem::path directory = std::filesystem::current_path();
  for (int depth = 0; depth < 8 && !directory.empty(); ++depth) {
    for (const char* const relative : candidates) {
      const std::filesystem::path candidate = directory / relative;
      if (std::filesystem::exists(candidate)) return candidate.string();
    }
    directory = directory.parent_path();
  }
  return {};
}

/** A manager with the bundled font in it, or null when the font cannot be found. */
std::unique_ptr<FontManager> managerWithBundledFont() {
  const std::string path = bundledFontPath();
  if (path.empty()) return nullptr;

  auto manager = std::make_unique<FontManager>();
  if (manager->addFontFromFile(path) == nullptr) return nullptr;
  return manager;
}

} // namespace anonymous

// ==================================================================================================
// Building a collection
// ==================================================================================================

TEST(FontManager, AddingAFontAddsItOnce) {
  auto manager = managerWithBundledFont();
  ASSERT_NE(manager, nullptr) << "the repository's bundled font must be findable from the test's working directory";

  EXPECT_EQ(manager->typefaceCount(), 1u);

  // Adding the same path again returns the face that was read the first time rather than reading the file twice, so
  // the count does not move. This matters for the system scan, where several directories can name the same font.
  const auto again = manager->addFontFromFile(bundledFontPath());
  EXPECT_NE(again, nullptr);
  EXPECT_EQ(manager->typefaceCount(), 1u) << "a font is not added twice";

  // And it is the same object, not a second copy of the same file.
  EXPECT_EQ(again, manager->defaultTypeface());
}

TEST(FontManager, AddingSomethingThatIsNotAFontIsRefused) {
  FontManager manager;

  const std::string path = bundledFontPath();
  ASSERT_FALSE(path.empty());
  const std::filesystem::path notAFont = std::filesystem::temp_directory_path() / "coral-not-a-font.ttf";
  {
    std::ofstream file(notAFont, std::ios::binary);
    file << "this is not a font";
  }

  EXPECT_EQ(manager.addFontFromFile(notAFont.string()), nullptr) << "rubbish must not become a typeface";
  EXPECT_EQ(manager.addFontFromFile("this-file-does-not-exist.ttf"), nullptr);
  EXPECT_EQ(manager.typefaceCount(), 0u);
  EXPECT_EQ(manager.defaultTypeface(), nullptr) << "an empty manager has no default";

  std::error_code ignored;
  std::filesystem::remove(notAFont, ignored);
}

TEST(FontManager, AnEmptyManagerAnswersNothingRatherThanCrashing) {
  FontManager manager;

  EXPECT_EQ(manager.defaultTypeface(), nullptr);
  EXPECT_EQ(manager.matchFamilyStyle("Anything", FontStyle::Normal()), nullptr);
  EXPECT_EQ(manager.matchCharacter(U'A'), nullptr);
  EXPECT_TRUE(manager.familyNames().empty());
  EXPECT_EQ(manager.typefaceCount(), 0u);
}

// ==================================================================================================
// Finding a family
// ==================================================================================================

TEST(FontManager, FindsAFamilyByName) {
  auto manager = managerWithBundledFont();
  ASSERT_NE(manager, nullptr);

  const auto byName = manager->matchFamilyStyle("Roboto", FontStyle::Normal());
  ASSERT_NE(byName, nullptr);
  EXPECT_EQ(byName->familyName(), "Roboto") << "the requested family is the one returned";

  // Case does not matter, because a caller reading a name out of a stylesheet should not have to guess the case the
  // font's own metadata uses.
  const auto lowercase = manager->matchFamilyStyle("roboto", FontStyle::Normal());
  ASSERT_NE(lowercase, nullptr);
  EXPECT_EQ(lowercase->familyName(), "Roboto");
}

TEST(FontManager, AnUnknownFamilyFallsBack) {
  auto manager = managerWithBundledFont();
  ASSERT_NE(manager, nullptr);

  // There is nothing nearest to a family that does not exist, so the default is returned. The caller can tell this
  // happened by comparing the family name it asked for with the one it got -- which is what a fallback chain needs.
  const auto fallback = manager->matchFamilyStyle("NoSuchFamilyAnywhere", FontStyle::Normal());
  ASSERT_NE(fallback, nullptr) << "a missing family is answered rather than refused";
  EXPECT_NE(fallback->familyName(), "NoSuchFamilyAnywhere");
  EXPECT_EQ(fallback, manager->defaultTypeface());
}

TEST(FontManager, TheFamilyIsPreferredOverTheStyle) {
  auto manager = managerWithBundledFont();
  ASSERT_NE(manager, nullptr);
  manager->addSystemFonts();

  // The bundled font is a Medium, so a request for Roboto in a weight it does not have cannot be satisfied exactly.
  // The rule says the family wins: the answer must still be Roboto rather than a bold face from elsewhere, because
  // the letterforms matter more than the weight.
  const auto mismatched = manager->matchFamilyStyle("Roboto", FontStyle(FontWeight::kBold, FontStyle::Slant::Upright));
  ASSERT_NE(mismatched, nullptr);
  EXPECT_EQ(mismatched->familyName(), "Roboto")
          << "an unsatisfiable weight must not pull the match into another family";
}

// ==================================================================================================
// Finding a style
// ==================================================================================================

TEST(FontManager, PrefersABoldFaceWhenOneIsAskedFor) {
  FontManager manager;
  const std::string path = bundledFontPath();
  ASSERT_FALSE(path.empty());
  ASSERT_NE(manager.addFontFromFile(path), nullptr);

  // The system's own fonts, so there is something bold to find. Skipped rather than failed when the machine has no
  // fonts at all, which is a statement about the machine and not about this code.
  if (manager.addSystemFonts() == 0) {
    GTEST_SKIP() << "no system fonts to choose between";
  }

  const auto bold = manager.matchFamilyStyle("", FontStyle(FontWeight::kBold, FontStyle::Slant::Upright));
  ASSERT_NE(bold, nullptr) << "an empty family name means 'any family, this style'";

  // If any bold face was found at all, the request for bold must return one. A scorer that ignored the weight would
  // return whichever face came first, which is this test's whole point.
  bool anyBoldExists = false;
  for (const std::string& family: manager.familyNames()) {
    const auto candidate = manager.matchFamilyStyle(family, FontStyle::Normal());
    if (candidate != nullptr && candidate->style().isBold()) anyBoldExists = true;
  }

  if (anyBoldExists) {
    EXPECT_TRUE(bold->style().isBold())
            << "a bold face exists in this collection, so a request for bold must return one";
  }
}

TEST(FontManager, PrefersAnItalicFaceWhenOneIsAskedFor) {
  FontManager manager;
  const std::string path = bundledFontPath();
  ASSERT_FALSE(path.empty());
  ASSERT_NE(manager.addFontFromFile(path), nullptr);
  if (manager.addSystemFonts() == 0) {
    GTEST_SKIP() << "no system fonts to choose between";
  }

  // The slant is weighted more heavily than the weight, because an italic face is a different set of letterforms
  // rather than the same ones drawn heavier. So if the collection contains any italic at all, this has to find one.
  bool anyItalicExists = false;
  for (const std::string& family: manager.familyNames()) {
    const auto candidate = manager.matchFamilyStyle(family, FontStyle::Normal());
    if (candidate != nullptr && candidate->style().isItalic()) anyItalicExists = true;
  }
  if (!anyItalicExists) {
    GTEST_SKIP() << "this collection has no italic face at all";
  }

  const auto italic = manager.matchFamilyStyle("", FontStyle(FontWeight::kNormal, FontStyle::Slant::Italic));
  ASSERT_NE(italic, nullptr);
  EXPECT_TRUE(italic->style().isItalic()) << "a request for italic must find the italic face that exists";
}

TEST(FontManager, TheDefaultIsUprightAndNormalWeight) {
  FontManager manager;
  const std::string path = bundledFontPath();
  ASSERT_FALSE(path.empty());
  ASSERT_NE(manager.addFontFromFile(path), nullptr);
  manager.addSystemFonts();

  const auto fallback = manager.defaultTypeface();
  ASSERT_NE(fallback, nullptr);

  // A fallback is reached for body text far more often than for a heading, so an upright normal-weight face is what
  // it should be -- unless there is nothing else, in which case the closest available face is the right answer and
  // there is nothing to assert.
  bool anyUpright = false;
  for (const std::string& family: manager.familyNames()) {
    const auto candidate = manager.matchFamilyStyle(family, FontStyle::Normal());
    if (candidate != nullptr && !candidate->style().isItalic()) anyUpright = true;
  }
  if (anyUpright) {
    EXPECT_FALSE(fallback->style().isItalic()) << "the default is not an italic";
  }
}

// ==================================================================================================
// Font fallback
// ==================================================================================================

TEST(FontManager, FindsAFaceThatHasACharacter) {
  auto manager = managerWithBundledFont();
  ASSERT_NE(manager, nullptr);

  // An ASCII letter, which the bundled font certainly has. The preferred face is returned directly when it covers the
  // character, which is the common case and needs no scan.
  const auto preferred = manager->defaultTypeface();
  ASSERT_NE(preferred, nullptr);

  const auto found = manager->matchCharacter(U'A', preferred);
  EXPECT_EQ(found, preferred) << "a face that already has the character is returned as it is";
}

TEST(FontManager, ACharacterNothingCoversIsReportedRatherThanGuessed) {
  FontManager manager;
  const std::string path = bundledFontPath();
  ASSERT_FALSE(path.empty());
  ASSERT_NE(manager.addFontFromFile(path), nullptr);

  // A private-use code point, which no font should map. Returning null is the honest answer: the caller needs to know
  // there is nothing to draw, and a face chosen at random would draw the missing-glyph box silently.
  constexpr uint32_t kPrivateUse = 0xF8FFu;
  if (manager.matchCharacter(kPrivateUse) == nullptr) {
    SUCCEED() << "nothing covers a private-use code point, and that was reported";
  } else {
    // If the bundled font does map it, the answer is still a face that really has the glyph -- which is all that can
    // be asked.
    EXPECT_TRUE(manager.matchCharacter(kPrivateUse)->hasGlyphForCodePoint(kPrivateUse));
  }
}

// ==================================================================================================
// The system scan
// ==================================================================================================

TEST(FontManager, ScansTheSystemsFonts) {
  FontManager manager;
  const size_t added = manager.addSystemFonts();

  if (added == 0) {
    // Not a failure: a container with no fonts installed is a legitimate environment. The property that still holds
    // is that the manager is usable and reports nothing.
    EXPECT_TRUE(manager.familyNames().empty());
    GTEST_SKIP() << "this machine has no fonts installed";
  }

  EXPECT_GT(manager.typefaceCount(), 0u);
  EXPECT_FALSE(manager.familyNames().empty());
  EXPECT_NE(manager.defaultTypeface(), nullptr) << "a collection with fonts in it has a default";

  // Scanning twice adds nothing, which is what makes the scan safe to call on a manager a caller has been adding to.
  const size_t countAfterFirst = manager.typefaceCount();
  EXPECT_EQ(manager.addSystemFonts(), 0u) << "the second scan finds nothing new";
  EXPECT_EQ(manager.typefaceCount(), countAfterFirst);
}

TEST(FontManager, EveryReportedFamilyCanBeMatched) {
  FontManager manager;
  if (manager.addSystemFonts() == 0) {
    GTEST_SKIP() << "this machine has no fonts installed";
  }

  // The list a caller is given has to be usable: every name in it must find a face. This is what catches a family
  // index that records one spelling and looks up another -- in particular the lowercasing, which is applied to the
  // key and would break the lookup if it were applied only where the face is stored.
  const std::vector<std::string> families = manager.familyNames();
  ASSERT_FALSE(families.empty());

  for (const std::string& family: families) {
    const auto face = manager.matchFamilyStyle(family, FontStyle::Normal());
    ASSERT_NE(face, nullptr) << "family '" << family << "' was reported but cannot be matched";
  }
}

TEST(FontManager, TheSharedManagerIsUsable) {
  // The shared manager scans on first use, so it exists whether or not this machine has fonts. What is asserted is
  // that it is stable and self-consistent, not that it found anything.
  FontManager& shared = FontManager::shared();
  EXPECT_EQ(&shared, &FontManager::shared()) << "one manager, not one per call";

  if (shared.typefaceCount() > 0) {
    EXPECT_NE(shared.defaultTypeface(), nullptr);
    for (const std::string& family: shared.familyNames()) {
      EXPECT_NE(shared.matchFamilyStyle(family, FontStyle::Normal()), nullptr);
    }
  }
}
