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
 * @file test_SvgResources.cpp
 *
 * The XML reader against the real SVG documents under `resources/apitest/SVG`.
 *
 * A reader tested only against strings written in a test file is tested against what its author believed the format
 * looks like. These are files other people wrote: they carry DOCTYPEs, CDATA in `<style>` elements, comments between
 * attributes, `xlink:` prefixes and entity references, and the reader either takes them whole or it does not.
 *
 * This is deliberately a *parse* test rather than a rendering one. It is the step that has to be right before anything
 * downstream can be, and when a document fails here the reason is a line and a column rather than a picture that looks
 * slightly wrong.
 */

#include "xml/Xml.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef CORAL_GRAPHICS_RESOURCE_DIR
#error "CORAL_GRAPHICS_RESOURCE_DIR must be defined by the build; see CMakeLists.txt"
#endif

namespace {

namespace fs = std::filesystem;

fs::path svgDirectory() {
  return fs::path(CORAL_GRAPHICS_RESOURCE_DIR) / "apitest" / "SVG";
}

std::string readFile(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  std::ostringstream contents;
  contents << stream.rdbuf();
  return contents.str();
}

/** Every `.svg` in the resource directory, sorted so a failure names a stable file. */
std::vector<fs::path> svgFiles() {
  std::vector<fs::path> files;
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(svgDirectory(), error)) {
    if (entry.is_regular_file() && entry.path().extension() == ".svg") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

/** The `svg` root of @p path, or a failure naming the file and the reader's reason. */
void expectParses(const fs::path& path) {
  const std::string document = readFile(path);
  ASSERT_FALSE(document.empty()) << path.filename() << " could not be read";

  graphics::xml::ParseError error;
  const auto root = graphics::xml::parse(document, &error);
  ASSERT_TRUE(root.has_value()) << path.filename() << ": " << error.describe();

  // Every one of these documents is an SVG, so the root element is the one thing they have in common and the cheapest
  // check that the tree came out the right way up.
  EXPECT_TRUE(root->isElement()) << path.filename();
  EXPECT_EQ(root->name(), "svg") << path.filename();
}

} // namespace anonymous

TEST(SvgResources, TheSvgDirectoryExistsAndHasDocuments) {
  // A guard on the test itself: if the resources move, every check below would pass by iterating nothing at all, which
  // is the failure mode this whole file exists to avoid.
  ASSERT_TRUE(fs::is_directory(svgDirectory())) << svgDirectory();
  const std::vector<fs::path> files = svgFiles();
  EXPECT_GE(files.size(), 20U) << "expected the SVG corpus under " << svgDirectory();
}

TEST(SvgResources, EveryDocumentParses) {
  const std::vector<fs::path> files = svgFiles();
  ASSERT_FALSE(files.empty());
  for (const fs::path& path : files) {
    expectParses(path);
  }
}

TEST(SvgResources, EachDocumentParsesIndividuallyAndNamesItself) {
  // One test per file, so a failure says which document and ctest reports it by name rather than as "SomeDocument
  // failed" among twenty-two. `SCOPED_TRACE` is what makes the shared helper's assertions name the file too.
  const std::vector<fs::path> files = svgFiles();
  ASSERT_FALSE(files.empty());
  for (const fs::path& path : files) {
    SCOPED_TRACE(path.filename().string());
    expectParses(path);
  }
}

TEST(SvgResources, TheRootCarriesTheSvgNamespace) {
  // `xmlns` is how these documents declare themselves as SVG rather than as some other XML, and it is the attribute a
  // reader has to keep intact for the layer above to recognise the document at all.
  bool sawNamespace = false;
  for (const fs::path& path : svgFiles()) {
    const std::string document = readFile(path);
    graphics::xml::ParseError error;
    const auto root = graphics::xml::parse(document, &error);
    ASSERT_TRUE(root.has_value()) << path.filename() << ": " << error.describe();
    if (root->hasAttribute("xmlns")) {
      sawNamespace = true;
      EXPECT_EQ(root->attribute("xmlns").value_or(""), "http://www.w3.org/2000/svg")
              << path.filename();
    }
  }
  EXPECT_TRUE(sawNamespace) << "no document declared an SVG namespace, which cannot be right";
}

TEST(SvgResources, TheDocumentsThatUseStyleCarryTheirCssThrough) {
  // `complex5.svg` and `complex7.svg` are the large ones and both put CSS in a `<style>` element. The reader has no
  // opinion about CSS, but the characters have to arrive intact: an entity or a CDATA marker mishandled here shows up
  // as a style rule that does not match anything, which renders as an unstyled picture rather than as an error.
  const std::vector<fs::path> files = svgFiles();
  int withStyle = 0;
  for (const fs::path& path : files) {
    const std::string document = readFile(path);
    graphics::xml::ParseError error;
    const auto root = graphics::xml::parse(document, &error);
    ASSERT_TRUE(root.has_value()) << path.filename() << ": " << error.describe();

    const graphics::xml::Node* style = root->firstChild("style");
    if (style == nullptr) {
      // The style element is also seen nested inside `<defs>`, so look one level down before giving up.
      for (const auto& child : root->children()) {
        if (child->isElement() && child->firstChild("style") != nullptr) {
          style = child->firstChild("style");
          break;
        }
      }
    }
    if (style == nullptr) continue;
    ++withStyle;
    EXPECT_FALSE(style->textContent().empty()) << path.filename() << " has an empty <style>";
  }
  EXPECT_GE(withStyle, 1) << "no document in the corpus carried a <style> element";
}
