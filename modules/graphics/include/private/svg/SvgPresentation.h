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

#ifndef CORAL_GRAPHICS_SVG_SVG_PRESENTATION_H
#define CORAL_GRAPHICS_SVG_SVG_PRESENTATION_H

#include "svg/SvgTypes.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * @file SvgPresentation.h
 *
 * The presentation properties an element can set, and the three places they can be written.
 *
 * ## Why every field is optional
 *
 * SVG's presentation attributes are inherited, and "not specified here" is different from "specified as the initial
 * value". A `<g fill="red">` around a `<path fill="blue">` gives blue; the same `<g>` around a `<path>` with no fill at
 * all gives red. Collapsing the second case to black at parse time loses the inheritance and paints the path black,
 * which is a picture that looks deliberate.
 *
 * So a `Presentation` records only what the element actually says, and the render pass merges it over the inherited
 * state. The initial values live in the render state, once, rather than being written into every node.
 *
 * ## The three places, and their order
 *
 * CSS says the cascade is, weakest first: presentation attributes, then a stylesheet, then a `style` attribute. The
 * corpus exercises all three -- `refStyle.svg` carries `.cls-1 { fill: #fcfae9 }` in a `<style>` while the sibling path
 * carries `fill="#be3c56"` inline -- so `parsePresentation` applies them in that order and later sources win.
 */

namespace graphics::svg {

class StyleSheet;

/** What an element says about how it is painted. Absent means "inherit". */
struct Presentation {
  // Paint.
  std::optional<Paint> fill;
  std::optional<FillRule> fillRule;
  std::optional<Scalar> fillOpacity;
  std::optional<Paint> stroke;
  std::optional<Scalar> strokeWidth;
  std::optional<Scalar> strokeOpacity;
  std::optional<LineCap> strokeCap;
  std::optional<LineJoin> strokeJoin;
  std::optional<Scalar> strokeMiterLimit;
  std::optional<DashArray> strokeDashArray;

  // Compositing and clipping.
  std::optional<Scalar> opacity;
  std::optional<std::string> clipPath;
  std::optional<std::string> mask;
  std::optional<std::string> filter;

  // Geometry and layout.
  std::optional<Matrix> transform;
  std::optional<Visibility> visibility;
  std::optional<std::string> display;

  // Text.
  std::optional<Color> color;
  std::optional<Scalar> fontSize;
  std::optional<std::string> fontFamily;
  std::optional<TextAnchor> textAnchor;
  std::optional<FontStyle> fontStyle;
  std::optional<FontWeight> fontWeight;
};

/** A `name: value` pair from a stylesheet or a `style` attribute, with both sides trimmed. */
using Declaration = std::pair<std::string, std::string>;

/**
 * Splits a `style` attribute into its declarations.
 *
 * The syntax is CSS's, not XML's: `fill: red; stroke: blue` with the trailing semicolon optional and whitespace
 * around either side. A declaration with no colon is dropped rather than failing the element, because a browser does
 * the same and a document in the wild relies on it.
 */
[[nodiscard]] std::vector<Declaration> parseStyleAttribute(std::string_view text);

/**
 * Applies @p declarations to @p out, ignoring names this renderer does not know.
 *
 * @param lengthContext used for the properties that may be percentages or font-relative, so a `stroke-width="10%"`
 *        is resolved where the viewport is known rather than where the text was read.
 */
void applyDeclarations(const std::vector<Declaration>& declarations, const LengthContext& lengthContext, Presentation& out);

/**
 * Builds @p out from an element's own attributes and stylesheet rules.
 *
 * @param elementName the element's tag, lower-cased by the caller (SVG's element names are lower-case).
 * @param attributes the element's attributes.
 * @param sheet the document's rules, or null when it has none.
 * @param lengthContext the viewport, for the properties that are lengths.
 */
void parsePresentation(std::string_view elementName, const std::vector<std::pair<std::string, std::string>>& attributes,
                       const StyleSheet* sheet, const LengthContext& lengthContext, Presentation& out);

/**
 * The rules from a document's `<style>` elements.
 *
 * A deliberately small subset of CSS: element, `.class` and `#id` selectors, comma-separated selector lists, and
 * declarations in braces. That is what the corpus contains -- one rule, in one file -- and implementing the rest of the
 * cascade would be inventing a requirement. Selectors the parser does not understand are skipped rather than guessed at,
 * so a rule that is not applied is one that was never claimed to be.
 */
class StyleSheet {
  public:
  struct Rule {
    /** One selector: what it matches and how specific it is. */
    struct Selector {
      enum class Kind : uint8_t { Element, Class, Id };
      Kind kind = Kind::Element;
      std::string name;
      /** A selector list may combine them, as in `rect.cls-1`, which is element *and* class. */
      std::optional<std::string> requiredClass;
      std::optional<std::string> requiredId;

      [[nodiscard]] int specificity() const noexcept;
      [[nodiscard]] bool matches(std::string_view element, std::string_view classes, std::string_view id) const;
    };

    std::vector<Selector> selectors;
    std::vector<Declaration> declarations;
  };

  /** @return the declarations that apply to an element, weakest first, in cascade order. */
  [[nodiscard]] std::vector<Declaration> declarationsFor(std::string_view elementName, std::string_view classAttribute,
                                                        std::string_view idAttribute) const;

  [[nodiscard]] bool empty() const noexcept { return mRules.empty(); }
  [[nodiscard]] size_t ruleCount() const noexcept { return mRules.size(); }

  private:
  friend std::optional<StyleSheet> parseStyleSheet(std::string_view css);
  std::vector<Rule> mRules;
};

/** Parses CSS, skipping anything it does not understand. @return nothing only when there is no rule at all. */
[[nodiscard]] std::optional<StyleSheet> parseStyleSheet(std::string_view css);

} // namespace graphics::svg

#endif // CORAL_GRAPHICS_SVG_SVG_PRESENTATION_H
