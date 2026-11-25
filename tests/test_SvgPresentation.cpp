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
 * @file test_SvgPresentation.cpp
 *
 * The presentation layer: what an element says, and which source wins when several say something.
 *
 * The cases worth having are the cascade ones. A property written in three places at once, a `style` attribute that must
 * beat a stylesheet rule, a class list that has to be matched by membership rather than by string equality, and a
 * property that is not understood sitting next to one that is. Each of those produces a picture that is painted the
 * wrong colour with no error anywhere -- which is the failure this layer exists to prevent.
 */

#include "svg/SvgPresentation.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using graphics::svg::Declaration;
using graphics::svg::LengthContext;
using graphics::svg::Paint;
using graphics::svg::Presentation;
using graphics::svg::StyleSheet;
using graphics::svg::parsePresentation;
using graphics::svg::parseStyleAttribute;
using graphics::svg::parseStyleSheet;

/** The attributes of a hypothetical element, as the DOM would pass them. */
using Attributes = std::vector<std::pair<std::string, std::string>>;

Presentation presentation(const std::string& element, const Attributes& attributes, const StyleSheet* sheet = nullptr) {
  LengthContext context;
  context.viewportWidth = 200;
  context.viewportHeight = 100;
  Presentation out;
  parsePresentation(element, attributes, sheet, context, out);
  return out;
}

/** Runs @p css through the parser, failing the test when it produces no rules at all. */
StyleSheet sheetOrFail(const std::string& css) {
  const auto sheet = parseStyleSheet(css);
  EXPECT_TRUE(sheet.has_value()) << "no rules parsed from: " << css;
  return sheet.has_value() ? *sheet : StyleSheet {};
}

int red8(const graphics::Color& color) { return static_cast<int>(std::lround(color.red * 255.0f)); }

} // namespace anonymous

// ==================================================================================================
// The style attribute
// ==================================================================================================

TEST(SvgStyleAttribute, SplitsDeclarations) {
  const auto declarations = parseStyleAttribute("fill: red; stroke: blue");
  ASSERT_EQ(declarations.size(), 2U);
  EXPECT_EQ(declarations[0].first, "fill");
  EXPECT_EQ(declarations[0].second, "red");
  EXPECT_EQ(declarations[1].first, "stroke");
  EXPECT_EQ(declarations[1].second, "blue");
}

TEST(SvgStyleAttribute, ToleratesWhitespaceAndAMissingTrailingSemicolon) {
  const auto declarations = parseStyleAttribute("  fill :  red  ;  stroke:blue  ");
  ASSERT_EQ(declarations.size(), 2U);
  EXPECT_EQ(declarations[0].first, "fill");
  EXPECT_EQ(declarations[0].second, "red");
  EXPECT_EQ(declarations[1].second, "blue");
}

TEST(SvgStyleAttribute, LowerCasesPropertyNames) {
  // CSS property names are case-insensitive, and a document that spells `FILL` means the same thing.
  const auto declarations = parseStyleAttribute("FILL: RED");
  ASSERT_EQ(declarations.size(), 1U);
  EXPECT_EQ(declarations[0].first, "fill");
}

TEST(SvgStyleAttribute, DropsAStatementWithNoColon) {
  // A browser ignores it, and failing the element over it would reject documents that render fine.
  const auto declarations = parseStyleAttribute("fill: red; nonsense; stroke: blue");
  ASSERT_EQ(declarations.size(), 2U);
  EXPECT_EQ(declarations[1].first, "stroke");
}

// ==================================================================================================
// Reading an element's own attributes
// ==================================================================================================

TEST(SvgPresentation, ReadsPaintProperties) {
  const Presentation p = presentation("path", {{"fill", "#ff0000"}, {"stroke", "blue"}, {"stroke-width", "4"}});
  ASSERT_TRUE(p.fill.has_value());
  EXPECT_EQ(p.fill->type, Paint::Type::Color);
  ASSERT_TRUE(p.stroke.has_value());
  ASSERT_TRUE(p.strokeWidth.has_value());
  EXPECT_FLOAT_EQ(*p.strokeWidth, 4.0f);
}

TEST(SvgPresentation, LeavesUnsetPropertiesUnset) {
  // The whole point of the optional fields: `fill` absent must stay absent so the parent's value is inherited, rather
  // than becoming the initial value here.
  const Presentation p = presentation("path", {{"stroke", "blue"}});
  EXPECT_FALSE(p.fill.has_value());
  EXPECT_FALSE(p.opacity.has_value());
  EXPECT_FALSE(p.transform.has_value());
}

TEST(SvgPresentation, ReadsTheStrokeStyleProperties) {
  const Presentation p = presentation("path", {{"stroke-linecap", "round"},
                                               {"stroke-linejoin", "bevel"},
                                               {"stroke-miterlimit", "8"},
                                               {"stroke-dasharray", "4 2"}});
  ASSERT_TRUE(p.strokeCap.has_value());
  EXPECT_EQ(*p.strokeCap, graphics::svg::LineCap::Round);
  ASSERT_TRUE(p.strokeJoin.has_value());
  EXPECT_EQ(*p.strokeJoin, graphics::svg::LineJoin::Bevel);
  ASSERT_TRUE(p.strokeMiterLimit.has_value());
  EXPECT_FLOAT_EQ(*p.strokeMiterLimit, 8.0f);
  ASSERT_TRUE(p.strokeDashArray.has_value());
  EXPECT_EQ(p.strokeDashArray->values.size(), 2U);
}

TEST(SvgPresentation, ReadsTheTransform) {
  const Presentation p = presentation("g", {{"transform", "translate(10,20)"}});
  ASSERT_TRUE(p.transform.has_value());
  const graphics::Point mapped = *p.transform * graphics::Point {0, 0};
  EXPECT_NEAR(mapped.x, 10.0f, 1e-4f);
  EXPECT_NEAR(mapped.y, 20.0f, 1e-4f);
}

TEST(SvgPresentation, ReadsClipPathMaskAndFilterReferencesAsIds) {
  // `url(#id)` becomes the bare id here, because the renderer looks the node up by id and re-parsing the wrapper at
  // every use would be work for nothing.
  const Presentation p = presentation("rect", {{"clip-path", "url(#clip)"}, {"mask", "url(#m)"}, {"filter", "url(#f)"}});
  ASSERT_TRUE(p.clipPath.has_value());
  EXPECT_EQ(*p.clipPath, "clip");
  ASSERT_TRUE(p.mask.has_value());
  EXPECT_EQ(*p.mask, "m");
  ASSERT_TRUE(p.filter.has_value());
  EXPECT_EQ(*p.filter, "f");
}

TEST(SvgPresentation, ReadsTextProperties) {
  const Presentation p = presentation("text", {{"font-size", "60"}, {"font-family", "'Helvetica Neue', sans-serif"},
                                                {"text-anchor", "middle"}, {"fill", "white"}});
  ASSERT_TRUE(p.fontSize.has_value());
  EXPECT_FLOAT_EQ(*p.fontSize, 60.0f);
  ASSERT_TRUE(p.fontFamily.has_value());
  // The first entry of the stack, with the quotes stripped: they are CSS syntax, not part of the name.
  EXPECT_EQ(*p.fontFamily, "Helvetica Neue");
  ASSERT_TRUE(p.textAnchor.has_value());
  EXPECT_EQ(*p.textAnchor, graphics::svg::TextAnchor::Middle);
}

TEST(SvgPresentation, IgnoresAPropertyItDoesNotImplement) {
  // `marker-end` and `paint-order` are real SVG properties for features this renderer does not have. Dropping them
  // leaves the rest of the element rendering correctly.
  const Presentation p = presentation("path", {{"marker-end", "url(#arrow)"}, {"fill", "red"}, {"paint-order", "stroke"}});
  ASSERT_TRUE(p.fill.has_value());
  EXPECT_EQ(red8(p.fill->color), 255);
}

TEST(SvgPresentation, ReadsOpacityAsANumberOrAPercentage) {
  const Presentation half = presentation("rect", {{"opacity", "0.5"}});
  ASSERT_TRUE(half.opacity.has_value());
  EXPECT_FLOAT_EQ(*half.opacity, 0.5f);

  const Presentation percent = presentation("rect", {{"opacity", "25%"}});
  ASSERT_TRUE(percent.opacity.has_value());
  EXPECT_FLOAT_EQ(*percent.opacity, 0.25f);

  // Clamped: a value outside the range is a document error, and clamping is what CSS does with it.
  const Presentation tooBig = presentation("rect", {{"opacity", "5"}});
  ASSERT_TRUE(tooBig.opacity.has_value());
  EXPECT_FLOAT_EQ(*tooBig.opacity, 1.0f);
}

// ==================================================================================================
// The stylesheet
// ==================================================================================================

TEST(SvgStyleSheet, ParsesAClassRule) {
  // The exact rule in the corpus: `refStyle.svg` carries this and a path that names the class.
  const StyleSheet sheet = sheetOrFail(".cls-1 { fill: #fcfae9 }");
  EXPECT_EQ(sheet.ruleCount(), 1U);

  const Presentation p = presentation("path", {{"class", "cls-1"}}, &sheet);
  ASSERT_TRUE(p.fill.has_value());
  EXPECT_EQ(red8(p.fill->color), 0xfc);
}

TEST(SvgStyleSheet, MatchesAClassFromASpaceSeparatedList) {
  const StyleSheet sheet = sheetOrFail(".cls-1 { fill: red }");
  // A `class` attribute is a list, so the rule applies when the name is one of several.
  const Presentation p = presentation("path", {{"class", "other cls-1 more"}}, &sheet);
  EXPECT_TRUE(p.fill.has_value());

  const Presentation noMatch = presentation("path", {{"class", "cls-11"}}, &sheet);
  EXPECT_FALSE(noMatch.fill.has_value()) << "a class name is matched whole, not as a prefix";
}

TEST(SvgStyleSheet, MatchesAnIdRule) {
  const StyleSheet sheet = sheetOrFail("#Layer_1 { fill: green }");
  const Presentation p = presentation("g", {{"id", "Layer_1"}}, &sheet);
  EXPECT_TRUE(p.fill.has_value());

  const Presentation noMatch = presentation("g", {{"id", "Layer_2"}}, &sheet);
  EXPECT_FALSE(noMatch.fill.has_value());
}

TEST(SvgStyleSheet, MatchesAnElementRule) {
  const StyleSheet sheet = sheetOrFail("rect { fill: blue }");
  EXPECT_TRUE(presentation("rect", {}, &sheet).fill.has_value());
  EXPECT_FALSE(presentation("circle", {}, &sheet).fill.has_value());
}

TEST(SvgStyleSheet, MatchesAnElementAndClassTogether) {
  const StyleSheet sheet = sheetOrFail("rect.cls-1 { fill: blue }");
  EXPECT_TRUE(presentation("rect", {{"class", "cls-1"}}, &sheet).fill.has_value());
  // The element has to match as well as the class.
  EXPECT_FALSE(presentation("circle", {{"class", "cls-1"}}, &sheet).fill.has_value());
  EXPECT_FALSE(presentation("rect", {{"class", "other"}}, &sheet).fill.has_value());
}

TEST(SvgStyleSheet, MatchesAnyOfACommaSeparatedList) {
  const StyleSheet sheet = sheetOrFail(".a, .b { fill: red }");
  EXPECT_TRUE(presentation("path", {{"class", "a"}}, &sheet).fill.has_value());
  EXPECT_TRUE(presentation("path", {{"class", "b"}}, &sheet).fill.has_value());
  EXPECT_FALSE(presentation("path", {{"class", "c"}}, &sheet).fill.has_value());
}

TEST(SvgStyleSheet, SkipsASelectorItDoesNotUnderstandWithoutLosingTheRule) {
  // A descendant combinator is not implemented. The rule's *other* selector still has to apply: failing the document
  // over a selector this subset does not handle would be worse than ignoring that one selector.
  const StyleSheet sheet = sheetOrFail(".a, g .b { fill: red }");
  EXPECT_TRUE(presentation("path", {{"class", "a"}}, &sheet).fill.has_value());
  EXPECT_FALSE(presentation("path", {{"class", "b"}}, &sheet).fill.has_value());
}

TEST(SvgStyleSheet, SkipsCommentsAndAtRules) {
  const StyleSheet sheet = sheetOrFail("/* a comment */\n@media print { .x { fill: red } }\n.cls-1 { fill: #fcfae9 }");
  EXPECT_EQ(sheet.ruleCount(), 1U);
  EXPECT_TRUE(presentation("path", {{"class", "cls-1"}}, &sheet).fill.has_value());
}

TEST(SvgStyleSheet, AppliesTheStrongerOfTwoMatchingRules) {
  // An id beats a class, and both beat an element. Applying them weakest first is what makes the last write win.
  const StyleSheet sheet = sheetOrFail("path { fill: red } .cls-1 { fill: green } #target { fill: blue }");
  const Presentation p = presentation("path", {{"class", "cls-1"}, {"id", "target"}}, &sheet);
  ASSERT_TRUE(p.fill.has_value());
  // Blue is 0 in red, so the red channel distinguishes blue from red and green from... green has red 0 too. Compare the
  // green channel: green is 255, blue is 0.
  EXPECT_EQ(static_cast<int>(std::lround(p.fill->color.green * 255.0f)), 0) << "the id rule should have won";
}

TEST(SvgStyleSheet, AppliesLaterRulesOfEqualStrengthLast) {
  // `green` in CSS is #008000, so its green channel is 128 and not 255 -- `lime` is the one that is #00ff00. The
  // assertions below compare against 128 for that reason.
  const StyleSheet sheet = sheetOrFail(".a { fill: red } .a { fill: green }");
  const Presentation p = presentation("path", {{"class", "a"}}, &sheet);
  ASSERT_TRUE(p.fill.has_value());
  EXPECT_EQ(static_cast<int>(std::lround(p.fill->color.green * 255.0f)), 128) << "the later rule should have won";
}

TEST(SvgStyleSheet, ReturnsNothingWhenThereAreNoRules) {
  EXPECT_FALSE(parseStyleSheet("").has_value());
  EXPECT_FALSE(parseStyleSheet("/* only a comment */").has_value());
  EXPECT_FALSE(parseStyleSheet("@media print { }").has_value());
}

// ==================================================================================================
// The cascade across all three sources
// ==================================================================================================

TEST(SvgCascade, AStyleAttributeBeatsAStylesheetRule) {
  const StyleSheet sheet = sheetOrFail(".cls-1 { fill: #fcfae9 }");
  const Presentation p = presentation("path", {{"class", "cls-1"}, {"style", "fill: #be3c56"}}, &sheet);
  ASSERT_TRUE(p.fill.has_value());
  // This is the corpus's own arrangement: a class rule and an inline style on the style attribute's sibling element.
  // The inline one wins, which is what CSS says and what the reference image shows.
  EXPECT_EQ(red8(p.fill->color), 0xbe);
}

TEST(SvgCascade, AStylesheetRuleBeatsAPresentationAttribute) {
  const StyleSheet sheet = sheetOrFail("rect { fill: green }");
  const Presentation p = presentation("rect", {{"fill", "red"}}, &sheet);
  ASSERT_TRUE(p.fill.has_value());
  // Green and not red: the stylesheet outranks the presentation attribute. `green` is #008000, so 128 rather than 255.
  EXPECT_EQ(static_cast<int>(std::lround(p.fill->color.green * 255.0f)), 128)
          << "the stylesheet outranks a presentation attribute";
  EXPECT_EQ(red8(p.fill->color), 0);
}

TEST(SvgCascade, APresentationAttributeStillAppliesWhenNoRuleMatches) {
  const StyleSheet sheet = sheetOrFail(".other { fill: green }");
  const Presentation p = presentation("rect", {{"fill", "red"}}, &sheet);
  ASSERT_TRUE(p.fill.has_value());
  EXPECT_EQ(red8(p.fill->color), 255);
}

TEST(SvgCascade, AStyleAttributeAppliesWithNoStylesheetAtAll) {
  const Presentation p = presentation("rect", {{"style", "fill: #00ff00; opacity: 0.5"}});
  ASSERT_TRUE(p.fill.has_value());
  ASSERT_TRUE(p.opacity.has_value());
  EXPECT_FLOAT_EQ(*p.opacity, 0.5f);
}

TEST(SvgCascade, TheIdAndClassAttributesAreNotThemselvesProperties) {
  // `id` and `class` are not presentation attributes and must not be read as one. Reading `id` as a property would be
  // harmless here, but the same loop reads every attribute, so a property whose name collides with a structural
  // attribute would be silently applied -- which is why they are excluded explicitly.
  const Presentation p = presentation("path", {{"id", "x"}, {"class", "y"}});
  EXPECT_FALSE(p.fill.has_value());
  EXPECT_FALSE(p.transform.has_value());
  EXPECT_FALSE(p.opacity.has_value());
}
