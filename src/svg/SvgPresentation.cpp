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
 * @file SvgPresentation.cpp
 *
 * The presentation layer declared in SvgPresentation.h.
 *
 * ## The cascade, in the order it is applied
 *
 * A property's value comes from the strongest of: a stylesheet rule that matches, a `style` attribute, or a presentation
 * attribute -- in that order of strength, which is the reverse of the order they are usually written. This file applies
 * them weakest first so that a later write simply overwrites, which is easier to get right than resolving precedence
 * afterwards.
 *
 * ## Unknown properties
 *
 * A declaration whose name this renderer does not know is dropped, and that is deliberate rather than lazy. SVG has
 * properties that apply to features which are not implemented -- `marker-end`, `paint-order`, `shape-rendering` -- and a
 * document using one of them should render everything else correctly rather than fail. The alternative, rejecting a
 * document for a property that does not affect the picture, would make the corpus useless as a test.
 */

#include "svg/SvgPresentation.h"

#include "SvgScanner.h"

#include "xml/Xml.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

namespace graphics::svg {
namespace {

std::string lowered(const std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return out;
}

/** Reads a length that may be a percentage or a font-relative unit, resolving it now the viewport is known. */
std::optional<Scalar> lengthValue(const std::string_view text, const LengthContext& context) {
  const auto length = parseLength(text);
  if (!length) return std::nullopt;
  // The axis is the width, which is what a percentage of "the other length" means for the properties here: stroke
  // widths, font sizes and opacities are all fractions of the viewport's smaller dimension in practice. The corpus uses
  // no percentage on any of them, so this is a defined choice rather than a measured one.
  return context.resolve(*length, LengthUnit::Px);
}

std::optional<Scalar> opacityValue(const std::string_view text) {
  const auto value = parseNumber(text);
  if (!value) {
    // A percentage is also legal for opacity in CSS's modern spelling.
    const auto length = parseLength(text);
    if (length && length->isPercent()) return std::clamp(length->value / 100.0f, 0.0f, 1.0f);
    return std::nullopt;
  }
  return std::clamp(*value, 0.0f, 1.0f);
}

/** @return the fragment identifier inside `url(#id)`, or the text unchanged when it is already bare. */
std::optional<std::string> referencedId(const std::string_view text) {
  const std::string_view value = trimmed(text);
  if (value.empty() || value == "none") return std::nullopt;

  if (value.rfind("url(", 0) != 0) return std::string {value};

  const size_t close = value.find(')');
  if (close == std::string_view::npos) return std::nullopt;
  std::string reference {trimmed(value.substr(4, close - 4))};
  if (reference.size() >= 2 && (reference.front() == '\'' || reference.front() == '"') &&
      reference.back() == reference.front()) {
    reference = reference.substr(1, reference.size() - 2);
  }
  if (!reference.empty() && reference.front() == '#') reference.erase(0, 1);
  if (reference.empty()) return std::nullopt;
  return reference;
}

} // namespace anonymous

// ==================================================================================================
// style attributes and declarations
// ==================================================================================================

std::vector<Declaration> parseStyleAttribute(const std::string_view text) {
  std::vector<Declaration> declarations;

  size_t position = 0;
  while (position < text.size()) {
    const size_t end = text.find(';', position);
    const std::string_view statement = text.substr(position, end == std::string_view::npos ? std::string_view::npos : end - position);
    position = end == std::string_view::npos ? text.size() : end + 1;

    const std::string_view trimmedStatement = trimmed(statement);
    if (trimmedStatement.empty()) continue;

    const size_t colon = trimmedStatement.find(':');
    // A statement with no colon is not a declaration. Skipping it rather than failing the element is what a browser
    // does, and documents in the wild depend on it.
    if (colon == std::string_view::npos) continue;

    const std::string name = lowered(trimmed(trimmedStatement.substr(0, colon)));
    const std::string value {trimmed(trimmedStatement.substr(colon + 1))};
    if (name.empty() || value.empty()) continue;
    declarations.emplace_back(name, value);
  }
  return declarations;
}

void applyDeclarations(const std::vector<Declaration>& declarations, const LengthContext& lengthContext,
                       Presentation& out) {
  for (const auto& [name, value] : declarations) {
    // Paint.
    if (name == "fill") {
      if (const auto paint = parsePaint(value)) out.fill = paint;
    } else if (name == "fill-rule" || name == "clip-rule") {
      if (const auto rule = parseFillRule(value)) out.fillRule = rule;
    } else if (name == "fill-opacity") {
      if (const auto opacity = opacityValue(value)) out.fillOpacity = opacity;
    } else if (name == "stroke") {
      if (const auto paint = parsePaint(value)) out.stroke = paint;
    } else if (name == "stroke-width") {
      if (const auto width = lengthValue(value, lengthContext)) out.strokeWidth = width;
    } else if (name == "stroke-opacity") {
      if (const auto opacity = opacityValue(value)) out.strokeOpacity = opacity;
    } else if (name == "stroke-linecap") {
      if (const auto cap = parseLineCap(value)) out.strokeCap = cap;
    } else if (name == "stroke-linejoin") {
      if (const auto join = parseLineJoin(value)) out.strokeJoin = join;
    } else if (name == "stroke-miterlimit") {
      if (const auto limit = parseNumber(value)) out.strokeMiterLimit = std::max(1.0f, *limit);
    } else if (name == "stroke-dasharray") {
      if (const auto dashes = parseDashArray(value)) out.strokeDashArray = dashes;

      // Geometry and compositing.
    } else if (name == "opacity") {
      if (const auto opacity = opacityValue(value)) out.opacity = opacity;
    } else if (name == "transform") {
      if (const auto transform = parseTransform(value)) out.transform = transform;
    } else if (name == "visibility") {
      if (const auto visibility = parseVisibility(value)) out.visibility = visibility;
    } else if (name == "display") {
      out.display = lowered(value);
    } else if (name == "clip-path") {
      if (const auto id = referencedId(value)) out.clipPath = id;
    } else if (name == "mask") {
      if (const auto id = referencedId(value)) out.mask = id;
    } else if (name == "filter") {
      if (const auto id = referencedId(value)) out.filter = id;

      // Text.
    } else if (name == "color") {
      if (const auto color = parseColor(value)) out.color = color;
    } else if (name == "font-size") {
      if (const auto size = lengthValue(value, lengthContext)) out.fontSize = size;
    } else if (name == "font-family") {
      // A font stack is a comma-separated list; the first entry is the one to ask for, and the quotes around a name with
      // spaces in it are not part of the name.
      std::string family {value};
      const size_t comma = family.find(',');
      if (comma != std::string::npos) family = family.substr(0, comma);
      family = std::string {trimmed(family)};
      if (family.size() >= 2 && (family.front() == '\'' || family.front() == '"') && family.back() == family.front()) {
        family = family.substr(1, family.size() - 2);
      }
      if (!family.empty()) out.fontFamily = family;
    } else if (name == "text-anchor") {
      if (const auto anchor = parseTextAnchor(value)) out.textAnchor = anchor;
    } else if (name == "font-style") {
      if (const auto style = parseFontStyle(value)) out.fontStyle = style;
    } else if (name == "font-weight") {
      if (const auto weight = parseFontWeight(value)) out.fontWeight = weight;
    }
    // Anything else is a property this renderer does not implement; dropping it leaves the rest of the document
    // rendering correctly, which is the behaviour SVG's own error handling asks for.
  }
}

// ==================================================================================================
// The cascade
// ==================================================================================================

void parsePresentation(const std::string_view elementName, const std::vector<std::pair<std::string, std::string>>& attributes,
                       const StyleSheet* sheet, const LengthContext& lengthContext, Presentation& out) {
  // 1. Presentation attributes are the weakest, so they go first and anything later overwrites them.
  std::vector<Declaration> fromAttributes;
  std::string id;
  std::string classes;
  for (const auto& [name, value] : attributes) {
    const std::string loweredName = lowered(name);
    if (loweredName == "style") continue; // handled below, because it outranks the others
    if (loweredName == "id") {
      id = value;
      continue;
    }
    if (loweredName == "class") {
      classes = value;
      continue;
    }
    fromAttributes.emplace_back(loweredName, value);
  }
  applyDeclarations(fromAttributes, lengthContext, out);

  // 2. A stylesheet rule outranks a presentation attribute.
  if (sheet != nullptr) {
    applyDeclarations(sheet->declarationsFor(elementName, classes, id), lengthContext, out);
  }

  // 3. And a `style` attribute outranks both.
  for (const auto& [name, value] : attributes) {
    if (lowered(name) == "style") {
      applyDeclarations(parseStyleAttribute(value), lengthContext, out);
    }
  }
}

// ==================================================================================================
// The stylesheet
// ==================================================================================================

int StyleSheet::Rule::Selector::specificity() const noexcept {
  // CSS's own weighting, near enough for the subset: an id beats a class beats an element. A selector that names more
  // than one of them adds them, which is what `rect.cls-1` should do against a bare `.cls-1`.
  int score = 0;
  if (kind == Kind::Id) score += 100;
  if (kind == Kind::Class) score += 10;
  if (kind == Kind::Element) score += 1;
  if (requiredId) score += 100;
  if (requiredClass) score += 10;
  return score;
}

bool StyleSheet::Rule::Selector::matches(const std::string_view element, const std::string_view classes,
                                        const std::string_view id) const {
  if (kind == Kind::Element && !name.empty() && name != element) return false;
  if (kind == Kind::Id && name != id) return false;
  if (kind == Kind::Class) {
    // A `class` attribute is a space-separated list, so this is a membership test and not a string comparison.
    bool found = false;
    size_t position = 0;
    while (position < classes.size()) {
      size_t end = classes.find(' ', position);
      if (end == std::string_view::npos) end = classes.size();
      if (classes.substr(position, end - position) == name) {
        found = true;
        break;
      }
      position = end + 1;
    }
    if (!found) return false;
  }

  if (requiredClass) {
    bool found = false;
    size_t position = 0;
    while (position < classes.size()) {
      size_t end = classes.find(' ', position);
      if (end == std::string_view::npos) end = classes.size();
      if (classes.substr(position, end - position) == *requiredClass) {
        found = true;
        break;
      }
      position = end + 1;
    }
    if (!found) return false;
  }
  if (requiredId && *requiredId != id) return false;
  return true;
}

std::vector<Declaration> StyleSheet::declarationsFor(const std::string_view elementName,
                                                     const std::string_view classAttribute,
                                                     const std::string_view idAttribute) const {
  // Collect the matching rules with the specificity that matched, then apply weakest first so that a later, stronger
  // rule overwrites -- which is the cascade, done by writing rather than by comparing.
  struct Match {
    int specificity = 0;
    size_t order = 0;
    const Rule* rule = nullptr;
  };
  std::vector<Match> matches;
  for (size_t index = 0; index < mRules.size(); ++index) {
    const Rule& rule = mRules[index];
    for (const Rule::Selector& selector : rule.selectors) {
      if (!selector.matches(elementName, classAttribute, idAttribute)) continue;
      // A rule with several selectors takes the strongest of them, which is how `rect, .cls` behaves.
      const auto existing = std::find_if(matches.begin(), matches.end(),
                                         [&rule](const Match& m) { return m.rule == &rule; });
      if (existing != matches.end()) {
        existing->specificity = std::max(existing->specificity, selector.specificity());
      } else {
        matches.push_back(Match {selector.specificity(), index, &rule});
      }
      break;
    }
  }

  std::stable_sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) {
    if (a.specificity != b.specificity) return a.specificity < b.specificity;
    return a.order < b.order;
  });

  std::vector<Declaration> declarations;
  for (const Match& match : matches) {
    declarations.insert(declarations.end(), match.rule->declarations.begin(), match.rule->declarations.end());
  }
  return declarations;
}

std::optional<StyleSheet> parseStyleSheet(const std::string_view css) {
  StyleSheet sheet;

  size_t position = 0;
  while (position < css.size()) {
    // Comments first, because a commented-out rule is common in a hand-written stylesheet.
    if (css.compare(position, 2, "/*") == 0) {
      const size_t end = css.find("*/", position + 2);
      position = end == std::string_view::npos ? css.size() : end + 2;
      continue;
    }

    // A closing brace here belongs to a block that was skipped -- the body of an `@media`, which this subset does not
    // implement. Skipping it matters rather than being tidy: left in place it becomes part of the *next* selector, and a
    // selector reading `}\n.cls-1` parses as "an element named `}` with class cls-1" and matches nothing.
    if (isSvgSpaceChar(css[position]) || css[position] == '}') {
      ++position;
      continue;
    }

    const size_t open = css.find('{', position);
    if (open == std::string_view::npos) break;
    const size_t close = css.find('}', open);
    if (close == std::string_view::npos) break;

    const std::string_view selectorText = trimmed(css.substr(position, open - position));
    const std::string_view body = css.substr(open + 1, close - open - 1);
    position = close + 1;

    // `@media` and friends have a nested block; the subset here does not implement them and skipping is honest.
    if (selectorText.empty() || selectorText.front() == '@') continue;

    StyleSheet::Rule rule;
    rule.declarations = parseStyleAttribute(body);
    if (rule.declarations.empty()) continue;

    size_t selectorPosition = 0;
    while (selectorPosition < selectorText.size()) {
      const size_t comma = selectorText.find(',', selectorPosition);
      const std::string_view piece =
              trimmed(selectorText.substr(selectorPosition, comma == std::string_view::npos ? std::string_view::npos
                                                                                          : comma - selectorPosition));
      selectorPosition = comma == std::string_view::npos ? selectorText.size() : comma + 1;
      if (piece.empty()) continue;

      StyleSheet::Rule::Selector selector;
      std::string_view rest = piece;

      // The tag part, when the selector starts with one. A selector may also start with `.` or `#`, in which case there
      // is no element constraint and the kind is decided by the first character.
      if (rest.front() != '.' && rest.front() != '#') {
        size_t end = 0;
        while (end < rest.size() && rest[end] != '.' && rest[end] != '#') ++end;
        selector.kind = StyleSheet::Rule::Selector::Kind::Element;
        selector.name = lowered(trimmed(rest.substr(0, end)));
        rest = rest.substr(end);
      }

      // Then at most one class and one id, which is all `rect.cls-1` and `#Layer_1.cls-1` need.
      bool understood = true;
      while (!rest.empty()) {
        const char marker = rest.front();
        if (marker != '.' && marker != '#') {
          // A descendant combinator, a pseudo-class or an attribute selector. This subset does not implement them, and
          // *this selector* is skipped rather than the rule or the sheet: a rule written `.shape, .shape:hover` should
          // still apply through its first selector, and failing the document over the second would be worse than
          // ignoring it.
          understood = false;
          break;
        }
        rest = rest.substr(1);
        size_t end = 0;
        while (end < rest.size() && rest[end] != '.' && rest[end] != '#' && rest[end] != ':' && rest[end] != ' ') ++end;
        const std::string name {rest.substr(0, end)};
        rest = rest.substr(end);
        if (name.empty()) {
          understood = false;
          break;
        }
        if (marker == '.') {
          if (selector.kind == StyleSheet::Rule::Selector::Kind::Element) {
            selector.requiredClass = name;
          } else {
            selector.kind = StyleSheet::Rule::Selector::Kind::Class;
            selector.name = name;
          }
        } else {
          if (selector.kind == StyleSheet::Rule::Selector::Kind::Element) {
            selector.requiredId = name;
          } else {
            selector.kind = StyleSheet::Rule::Selector::Kind::Id;
            selector.name = name;
          }
        }
      }

      if (!understood) continue;
      if (selector.name.empty() && !selector.requiredClass && !selector.requiredId) continue;
      rule.selectors.push_back(std::move(selector));
    }

    if (!rule.selectors.empty()) sheet.mRules.push_back(std::move(rule));
  }

  if (sheet.mRules.empty()) return std::nullopt;
  return sheet;
}

} // namespace graphics::svg
