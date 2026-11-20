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
 * @file test_Xml.cpp
 *
 * The XML reader the SVG module parses through.
 *
 * The cases worth having are the ones where a reader and a document disagree about what the document says: an end tag
 * that closes the wrong element, an entity in an attribute value, text that is only whitespace, and a CDATA section
 * whose ampersands must survive. Each of those produces a tree that is structurally valid and wrong, which is exactly
 * the kind of mistake that shows up much later as a shape drawn in the wrong place.
 */

#include "xml/Xml.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using graphics::xml::Node;
using graphics::xml::ParseError;

/** Parses @p document, failing the test with the reader's own reason when it will not parse. */
Node parseOrFail(const std::string& document) {
  ParseError error;
  auto root = graphics::xml::parse(document, &error);
  EXPECT_TRUE(root.has_value()) << "parse failed: " << error.describe();
  return root.has_value() ? std::move(*root) : Node {};
}

} // namespace anonymous

TEST(XmlTest, ReadsAnElementWithAttributes) {
  const Node root = parseOrFail(R"(<svg width="100" height="50"></svg>)");
  EXPECT_TRUE(root.isElement());
  EXPECT_EQ(root.name(), "svg");
  EXPECT_EQ(root.attribute("width").value_or(""), "100");
  EXPECT_EQ(root.attribute("height").value_or(""), "50");
  EXPECT_FALSE(root.hasAttribute("depth"));
  EXPECT_EQ(root.attribute("depth"), std::nullopt);
}

TEST(XmlTest, ReadsNestedElementsInOrder) {
  const Node root = parseOrFail(R"(<g><rect id="a"/><rect id="b"/></g>)");
  ASSERT_EQ(root.children().size(), 2U);
  EXPECT_EQ(root.children()[0]->name(), "rect");
  EXPECT_EQ(root.children()[0]->attribute("id").value_or(""), "a");
  EXPECT_EQ(root.children()[1]->attribute("id").value_or(""), "b");

  // The lookup helper takes the first match, which is what a caller wants when it asks by name.
  const Node* first = root.firstChild("rect");
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->attribute("id").value_or(""), "a");
  EXPECT_EQ(root.firstChild("circle"), nullptr);
}

TEST(XmlTest, ReadsTextContent) {
  const Node root = parseOrFail(R"(<text>Hello, world</text>)");
  EXPECT_EQ(root.textContent(), "Hello, world");
}

TEST(XmlTest, DecodesThePredefinedEntities) {
  const Node root = parseOrFail(R"(<text>a &amp; b &lt; c &gt; d &quot;e&quot; &apos;f&apos;</text>)");
  EXPECT_EQ(root.textContent(), R"(a & b < c > d "e" 'f')");
}

TEST(XmlTest, DecodesNumericEntities) {
  // `&#65;` is A, `&#x42;` is B, and `&#x4E2D;` is a three-byte UTF-8 code point. The last one is the interesting case:
  // a reader that truncated to a byte would produce a plausible-looking single character instead.
  const Node root = parseOrFail(R"(<text>&#65;&#x42;&#x4E2D;</text>)");
  EXPECT_EQ(root.textContent(), "AB\xE4\xB8\xAD");
}

TEST(XmlTest, DecodesEntitiesInAttributeValues) {
  // The failure this catches is a decoder wired into text but not into attributes, which renders `&amp;` visibly in a
  // label and nowhere else.
  const Node root = parseOrFail(R"(<text font-family="A &amp; B" title='x &lt; y'/>)");
  EXPECT_EQ(root.attribute("font-family").value_or(""), "A & B");
  EXPECT_EQ(root.attribute("title").value_or(""), "x < y");
}

TEST(XmlTest, PreservesWhitespaceAndReportsWhichTextIsOnlyWhitespace) {
  const Node root = parseOrFail("<g>\n  <rect/>\n</g>");
  ASSERT_GE(root.children().size(), 1U);
  EXPECT_TRUE(root.children().front()->isText());
  EXPECT_TRUE(root.children().front()->isWhitespace());

  const Node text = parseOrFail("<text>  spaced  </text>");
  EXPECT_EQ(text.textContent(), "  spaced  ");
  EXPECT_FALSE(text.children().front()->isWhitespace());
}

TEST(XmlTest, KeepsCdataLiteral) {
  // The point of CDATA is that its contents are not markup and not entities, so the ampersand has to survive.
  const Node root = parseOrFail(R"(<style><![CDATA[ a & b < c ]]></style>)");
  EXPECT_EQ(root.textContent(), " a & b < c ");
}

TEST(XmlTest, SkipsCommentsDeclarationsAndProcessingInstructions) {
  const Node root = parseOrFail(
          "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          "<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.1//EN\" \"http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd\">\n"
          "<!-- a comment before -->\n"
          "<svg><!-- inside --><rect/></svg>\n"
          "<!-- a comment after -->\n");
  EXPECT_EQ(root.name(), "svg");
  // The comment inside contributes no node: a renderer that saw one would have to know to ignore it.
  ASSERT_EQ(root.children().size(), 1U);
  EXPECT_EQ(root.children()[0]->name(), "rect");
}

TEST(XmlTest, HandlesADoctypeWithAnInternalSubset) {
  // The subset contains brackets and a `>` of its own, so a reader that stopped at the first `>` would leave the rest of
  // the declaration to be parsed as content.
  const Node root = parseOrFail(
          "<!DOCTYPE svg [\n"
          "  <!ENTITY foo \"bar\">\n"
          "]>\n"
          "<svg><rect/></svg>");
  EXPECT_EQ(root.name(), "svg");
  ASSERT_EQ(root.children().size(), 1U);
  EXPECT_EQ(root.children()[0]->name(), "rect");
}

TEST(XmlTest, HandlesSelfClosingAndEmptyElements) {
  const Node root = parseOrFail("<svg><rect/><g></g></svg>");
  ASSERT_EQ(root.children().size(), 2U);
  EXPECT_TRUE(root.children()[0]->children().empty());
  EXPECT_TRUE(root.children()[1]->children().empty());
}

TEST(XmlTest, ReadsAnAttributeNameWithAColon) {
  // SVG puts `xlink:href` on `use` and `image`, and namespace resolution is deliberately not this reader's job.
  const Node root = parseOrFail(
          R"(<svg xmlns:xlink="http://www.w3.org/1999/xlink"><image xlink:href="a.png"/></svg>)");
  EXPECT_EQ(root.attribute("xmlns:xlink").value_or(""), "http://www.w3.org/1999/xlink");
  const Node* image = root.firstChild("image");
  ASSERT_NE(image, nullptr);
  EXPECT_EQ(image->attribute("xlink:href").value_or(""), "a.png");
}

TEST(XmlTest, LeavesAnUnknownEntityAsWritten) {
  // A document relying on a DTD this reader does not process. Passing the text through is deliberate: a visible
  // `&nbsp;` is easier to notice than a document that refuses to load.
  const Node root = parseOrFail("<text>a&nbsp;b</text>");
  EXPECT_EQ(root.textContent(), "a&nbsp;b");
}

TEST(XmlTest, TreatsAStrayAmpersandAsText) {
  const Node root = parseOrFail("<text>a & b</text>");
  EXPECT_EQ(root.textContent(), "a & b");
}

// ---------------------------------------------------------------------------------------------------
// The failures. Every one of these is a document a reader could plausibly accept and do the wrong thing with.
// ---------------------------------------------------------------------------------------------------

TEST(XmlTest, RejectsAnEndTagForTheWrongElement) {
  ParseError error;
  EXPECT_FALSE(graphics::xml::parse("<g><rect></g>", &error).has_value());
  EXPECT_FALSE(error.message.empty());
  // The line matters: a caller fixing a document needs to be told where, not just that.
  EXPECT_GE(error.line, 1U);
}

TEST(XmlTest, RejectsAnUnclosedElement) {
  ParseError error;
  EXPECT_FALSE(graphics::xml::parse("<g><rect/>", &error).has_value());
  EXPECT_EQ(error.message.find("unclosed"), 0U) << error.describe();
}

TEST(XmlTest, RejectsAnUnterminatedAttributeValue) {
  ParseError error;
  EXPECT_FALSE(graphics::xml::parse(R"(<svg width="100></svg>)", &error).has_value());
  EXPECT_FALSE(error.message.empty());
}

TEST(XmlTest, RejectsContentAfterTheRootElement) {
  ParseError error;
  EXPECT_FALSE(graphics::xml::parse("<a/><b/>", &error).has_value());
  EXPECT_FALSE(error.message.empty());
}

TEST(XmlTest, RejectsAnEmptyDocument) {
  ParseError error;
  EXPECT_FALSE(graphics::xml::parse("", &error).has_value());
  EXPECT_FALSE(error.message.empty());
  // Nothing but a comment is still nothing: there is no root, so there is no tree.
  EXPECT_FALSE(graphics::xml::parse("<!-- just a comment -->", &error).has_value());
}

TEST(XmlTest, ReportsTheLineAndColumnOfAFailure) {
  ParseError error;
  EXPECT_FALSE(graphics::xml::parse("<svg>\n  <g>\n    <rect>\n</svg>", &error).has_value());
  EXPECT_EQ(error.line, 4U) << error.describe();
  EXPECT_FALSE(error.describe().empty());
}

TEST(XmlTest, AcceptsADocumentWithNoDeclaration) {
  // SVG files in the wild frequently start straight at the root element, and requiring a declaration would reject them.
  const Node root = parseOrFail("<svg><rect/></svg>");
  EXPECT_EQ(root.name(), "svg");
}

TEST(XmlTest, ReadsTheRootsChildrenWhenTheRootHasAttributesAndNewlines) {
  // Closer to a real file: the declaration, a root with several attributes across lines, and indented children.
  const Node root = parseOrFail(
          "<?xml version=\"1.0\"?>\n"
          "<svg xmlns=\"http://www.w3.org/2000/svg\"\n"
          "     width=\"128\" height=\"128\"\n"
          "     viewBox=\"0 0 128 128\">\n"
          "  <g id=\"layer\">\n"
          "    <path d=\"M 0 0 L 10 10 Z\"/>\n"
          "  </g>\n"
          "</svg>\n");
  EXPECT_EQ(root.attribute("viewBox").value_or(""), "0 0 128 128");
  const Node* group = root.firstChild("g");
  ASSERT_NE(group, nullptr);
  const Node* path = group->firstChild("path");
  ASSERT_NE(path, nullptr);
  EXPECT_EQ(path->attribute("d").value_or(""), "M 0 0 L 10 10 Z");
}
