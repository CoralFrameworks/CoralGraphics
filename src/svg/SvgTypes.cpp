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
 * @file SvgTypes.cpp
 *
 * The value parsers declared in SvgTypes.h.
 *
 * ## One scanner, parameterised
 *
 * SVG's grammar for a list of numbers is unusual: separators are optional wherever the next number cannot continue the
 * previous one, so `M0 0L10-10` has four numbers and two commands with no space between `10` and `-10`. A tokeniser
 * that demanded a comma or a space would reject most of the corpus -- which is why `M0 0L1 1` and `M 0,0 L 1,1` and
 * `M0 0L1 1` all have to work.
 *
 * Everything here is therefore built on one scanner that knows that rule: a number starts at a digit, a sign, or a
 * decimal point, and ends when the next character cannot extend it. Every list parser is a loop around it.
 *
 * ## What is rejected rather than tolerated
 *
 * A parse either consumes the whole input or fails. `10px` is a length; `10px junk` is not a length, and returning
 * `10px` would turn a malformed document into a slightly wrong picture rather than into a fallback the caller chose.
 */

#include "svg/SvgTypes.h"

#include "SvgScanner.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

namespace graphics::svg {
namespace {

/** The characters SVG treats as whitespace, which excludes the locale's ideas about space. */
bool space(const char c) noexcept {
  return isSvgSpaceChar(c);
}

/** @return the unit a suffix spells, or nothing when it is not a unit SVG knows. */
std::optional<LengthUnit> unitFromSuffix(const std::string_view suffix) {
  if (suffix.empty()) return LengthUnit::Number;
  if (suffix == "%") return LengthUnit::Percent;
  if (suffix == "em") return LengthUnit::Em;
  if (suffix == "ex") return LengthUnit::Ex;
  if (suffix == "px") return LengthUnit::Px;
  if (suffix == "pt") return LengthUnit::Pt;
  if (suffix == "pc") return LengthUnit::Pc;
  if (suffix == "cm") return LengthUnit::Cm;
  if (suffix == "mm") return LengthUnit::Mm;
  if (suffix == "in") return LengthUnit::In;
  return std::nullopt;
}

/** The absolute units, in user units per unit. 96 px to the inch is what CSS settled on and what SVG inherited. */
constexpr Scalar kPixelsPerInch = 96;

Scalar absoluteUnitScale(const LengthUnit unit) {
  switch (unit) {
    case LengthUnit::Px:
      return 1;
    case LengthUnit::Pt:
      return kPixelsPerInch / 72;
    case LengthUnit::Pc:
      return kPixelsPerInch / 6;
    case LengthUnit::In:
      return kPixelsPerInch;
    case LengthUnit::Cm:
      return kPixelsPerInch / 2.54f;
    case LengthUnit::Mm:
      return kPixelsPerInch / 25.4f;
    default:
      return 1;
  }
}

// ==================================================================================================
// Colours
// ==================================================================================================

struct NamedColor {
  std::string_view name;
  uint32_t rgb; // 0xRRGGBB
};

// The CSS/SVG named colours. Only the ones a document spells out are reachable, but a partial table is worse than none:
// a name that is missing renders as the fallback colour with no indication that it was not understood, which is exactly
// the silent-wrong-answer this library avoids elsewhere.
constexpr std::array<NamedColor, 148> kNamedColors{ {
  // clang-format off
  { .name = "aliceblue",            .rgb = 0xF0F8FF },
  { .name = "antiquewhite",         .rgb = 0xFAEBD7 },
  { .name = "aqua",                 .rgb = 0x00FFFF },
  { .name = "aquamarine",           .rgb = 0x7FFFD4 },
  { .name = "azure",                .rgb = 0xF0FFFF },
  { .name = "beige",                .rgb = 0xF5F5DC },
  { .name = "bisque",               .rgb = 0xFFE4C4 },
  { .name = "black",                .rgb = 0x000000 },
  { .name = "blanchedalmond",       .rgb = 0xFFEBCD },
  { .name = "blue",                 .rgb = 0x0000FF },
  { .name = "blueviolet",           .rgb = 0x8A2BE2 },
  { .name = "brown",                .rgb = 0xA52A2A },
  { .name = "burlywood",            .rgb = 0xDEB887 },
  { .name = "cadetblue",            .rgb = 0x5F9EA0 },
  { .name = "chartreuse",           .rgb = 0x7FFF00 },
  { .name = "chocolate",            .rgb = 0xD2691E },
  { .name = "coral",                .rgb = 0xFF7F50 },
  { .name = "cornflowerblue",       .rgb = 0x6495ED },
  { .name = "cornsilk",             .rgb = 0xFFF8DC },
  { .name = "crimson",              .rgb = 0xDC143C },
  { .name = "cyan",                 .rgb = 0x00FFFF },
  { .name = "darkblue",             .rgb = 0x00008B },
  { .name = "darkcyan",             .rgb = 0x008B8B },
  { .name = "darkgoldenrod",        .rgb = 0xB8860B },
  { .name = "darkgray",             .rgb = 0xA9A9A9 },
  { .name = "darkgreen",            .rgb = 0x006400 },
  { .name = "darkgrey",             .rgb = 0xA9A9A9 },
  { .name = "darkkhaki",            .rgb = 0xBDB76B },
  { .name = "darkmagenta",          .rgb = 0x8B008B },
  { .name = "darkolivegreen",       .rgb = 0x556B2F },
  { .name = "darkorange",           .rgb = 0xFF8C00 },
  { .name = "darkorchid",           .rgb = 0x9932CC },
  { .name = "darkred",              .rgb = 0x8B0000 },
  { .name = "darksalmon",           .rgb = 0xE9967A },
  { .name = "darkseagreen",         .rgb = 0x8FBC8F },
  { .name = "darkslateblue",        .rgb = 0x483D8B },
  { .name = "darkslategray",        .rgb = 0x2F4F4F },
  { .name = "darkslategrey",        .rgb = 0x2F4F4F },
  { .name = "darkturquoise",        .rgb = 0x00CED1 },
  { .name = "darkviolet",           .rgb = 0x9400D3 },
  { .name = "deeppink",             .rgb = 0xFF1493 },
  { .name = "deepskyblue",          .rgb = 0x00BFFF },
  { .name = "dimgray",              .rgb = 0x696969 },
  { .name = "dimgrey",              .rgb = 0x696969 },
  { .name = "dodgerblue",           .rgb = 0x1E90FF },
  { .name = "firebrick",            .rgb = 0xB22222 },
  { .name = "floralwhite",          .rgb = 0xFFFAF0 },
  { .name = "forestgreen",          .rgb = 0x228B22 },
  { .name = "fuchsia",              .rgb = 0xFF00FF },
  { .name = "gainsboro",            .rgb = 0xDCDCDC },
  { .name = "ghostwhite",           .rgb = 0xF8F8FF },
  { .name = "gold",                 .rgb = 0xFFD700 },
  { .name = "goldenrod",            .rgb = 0xDAA520 },
  { .name = "gray",                 .rgb = 0x808080 },
  { .name = "grey",                 .rgb = 0x808080 },
  { .name = "green",                .rgb = 0x008000 },
  { .name = "greenyellow",          .rgb = 0xADFF2F },
  { .name = "honeydew",             .rgb = 0xF0FFF0 },
  { .name = "hotpink",              .rgb = 0xFF69B4 },
  { .name = "indianred",            .rgb = 0xCD5C5C },
  { .name = "indigo",               .rgb = 0x4B0082 },
  { .name = "ivory",                .rgb = 0xFFFFF0 },
  { .name = "khaki",                .rgb = 0xF0E68C },
  { .name = "lavender",             .rgb = 0xE6E6FA },
  { .name = "lavenderblush",        .rgb = 0xFFF0F5 },
  { .name = "lawngreen",            .rgb = 0x7CFC00 },
  { .name = "lemonchiffon",         .rgb = 0xFFFACD },
  { .name = "lightblue",            .rgb = 0xADD8E6 },
  { .name = "lightcoral",           .rgb = 0xF08080 },
  { .name = "lightcyan",            .rgb = 0xE0FFFF },
  { .name = "lightgoldenrodyellow", .rgb = 0xFAFAD2 },
  { .name = "lightgray",            .rgb = 0xD3D3D3 },
  { .name = "lightgreen",           .rgb = 0x90EE90 },
  { .name = "lightgrey",            .rgb = 0xD3D3D3 },
  { .name = "lightpink",            .rgb = 0xFFB6C1 },
  { .name = "lightsalmon",          .rgb = 0xFFA07A },
  { .name = "lightseagreen",        .rgb = 0x20B2AA },
  { .name = "lightskyblue",         .rgb = 0x87CEFA },
  { .name = "lightslategray",       .rgb = 0x778899 },
  { .name = "lightslategrey",       .rgb = 0x778899 },
  { .name = "lightsteelblue",       .rgb = 0xB0C4DE },
  { .name = "lightyellow",          .rgb = 0xFFFFE0 },
  { .name = "lime",                 .rgb = 0x00FF00 },
  { .name = "limegreen",            .rgb = 0x32CD32 },
  { .name = "linen",                .rgb = 0xFAF0E6 },
  { .name = "magenta",              .rgb = 0xFF00FF },
  { .name = "maroon",               .rgb = 0x800000 },
  { .name = "mediumaquamarine",     .rgb = 0x66CDAA },
  { .name = "mediumblue",           .rgb = 0x0000CD },
  { .name = "mediumorchid",         .rgb = 0xBA55D3 },
  { .name = "mediumpurple",         .rgb = 0x9370DB },
  { .name = "mediumseagreen",       .rgb = 0x3CB371 },
  { .name = "mediumslateblue",      .rgb = 0x7B68EE },
  { .name = "mediumspringgreen",    .rgb = 0x00FA9A },
  { .name = "mediumturquoise",      .rgb = 0x48D1CC },
  { .name = "mediumvioletred",      .rgb = 0xC71585 },
  { .name = "midnightblue",         .rgb = 0x191970 },
  { .name = "mintcream",            .rgb = 0xF5FFFA },
  { .name = "mistyrose",            .rgb = 0xFFE4E1 },
  { .name = "moccasin",             .rgb = 0xFFE4B5 },
  { .name = "navajowhite",          .rgb = 0xFFDEAD },
  { .name = "navy",                 .rgb = 0x000080 },
  { .name = "oldlace",              .rgb = 0xFDF5E6 },
  { .name = "olive",                .rgb = 0x808000 },
  { .name = "olivedrab",            .rgb = 0x6B8E23 },
  { .name = "orange",               .rgb = 0xFFA500 },
  { .name = "orangered",            .rgb = 0xFF4500 },
  { .name = "orchid",               .rgb = 0xDA70D6 },
  { .name = "palegoldenrod",        .rgb = 0xEEE8AA },
  { .name = "palegreen",            .rgb = 0x98FB98 },
  { .name = "paleturquoise",        .rgb = 0xAFEEEE },
  { .name = "palevioletred",        .rgb = 0xDB7093 },
  { .name = "papayawhip",           .rgb = 0xFFEFD5 },
  { .name = "peachpuff",            .rgb = 0xFFDAB9 },
  { .name = "peru",                 .rgb = 0xCD853F },
  { .name = "pink",                 .rgb = 0xFFC0CB },
  { .name = "plum",                 .rgb = 0xDDA0DD },
  { .name = "powderblue",           .rgb = 0xB0E0E6 },
  { .name = "purple",               .rgb = 0x800080 },
  { .name = "rebeccapurple",        .rgb = 0x663399 },
  { .name = "red",                  .rgb = 0xFF0000 },
  { .name = "rosybrown",            .rgb = 0xBC8F8F },
  { .name = "royalblue",            .rgb = 0x4169E1 },
  { .name = "saddlebrown",          .rgb = 0x8B4513 },
  { .name = "salmon",               .rgb = 0xFA8072 },
  { .name = "sandybrown",           .rgb = 0xF4A460 },
  { .name = "seagreen",             .rgb = 0x2E8B57 },
  { .name = "seashell",             .rgb = 0xFFF5EE },
  { .name = "sienna",               .rgb = 0xA0522D },
  { .name = "silver",               .rgb = 0xC0C0C0 },
  { .name = "skyblue",              .rgb = 0x87CEEB },
  { .name = "slateblue",            .rgb = 0x6A5ACD },
  { .name = "slategray",            .rgb = 0x708090 },
  { .name = "slategrey",            .rgb = 0x708090 },
  { .name = "snow",                 .rgb = 0xFFFAFA },
  { .name = "springgreen",          .rgb = 0x00FF7F },
  { .name = "steelblue",            .rgb = 0x4682B4 },
  { .name = "tan",                  .rgb = 0xD2B48C },
  { .name = "teal",                 .rgb = 0x008080 },
  { .name = "thistle",              .rgb = 0xD8BFD8 },
  { .name = "tomato",               .rgb = 0xFF6347 },
  { .name = "turquoise",            .rgb = 0x40E0D0 },
  { .name = "violet",               .rgb = 0xEE82EE },
  { .name = "wheat",                .rgb = 0xF5DEB3 },
  { .name = "white",                .rgb = 0xFFFFFF },
  { .name = "whitesmoke",           .rgb = 0xF5F5F5 },
  { .name = "yellow",               .rgb = 0xFFFF00 },
  { .name = "yellowgreen",          .rgb = 0x9ACD32 },
  // clang-format on
} };

std::optional<Color> colorFromRgb(const uint32_t rgb, const Scalar alpha = 1.0f) {
  const auto red = static_cast<uint8_t>((rgb >> 16U) & 0xFFU);
  const auto green = static_cast<uint8_t>((rgb >> 8U) & 0xFFU);
  const auto blue = static_cast<uint8_t>(rgb & 0xFFU);
  return Color::MakeRGBA8(red, green, blue, static_cast<uint8_t>(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255.0f)));
}

int hexDigit(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/** Parses `#rgb`, `#rgba`, `#rrggbb` and `#rrggbbaa`. */
std::optional<Color> parseHexColor(const std::string_view digits) {
  for (const char c: digits) {
    if (hexDigit(c) < 0) return std::nullopt;
  }
  const auto component = [&digits](const size_t index, const int width) {
    int value = 0;
    for (int i = 0; i < width; ++i) {
      value = value * 16 + hexDigit(digits[index + static_cast<size_t>(i)]);
    }
    // A single-digit component means the digit is repeated: `#f` is `#ff`, which is 255 and not 15.
    return width == 1 ? value * 17 : value;
  };

  switch (digits.size()) {
    case 3:
      return colorFromRgb(static_cast<uint32_t>((component(0, 1) << 16) | (component(1, 1) << 8) | component(2, 1)));
    case 4:
      return colorFromRgb(static_cast<uint32_t>((component(0, 1) << 16) | (component(1, 1) << 8) | component(2, 1)),
        static_cast<Scalar>(component(3, 1)) / 255.0f);
    case 6:
      return colorFromRgb(static_cast<uint32_t>((component(0, 2) << 16) | (component(2, 2) << 8) | component(4, 2)));
    case 8:
      return colorFromRgb(static_cast<uint32_t>((component(0, 2) << 16) | (component(2, 2) << 8) | component(4, 2)),
        static_cast<Scalar>(component(6, 2)) / 255.0f);
    default:
      return std::nullopt;
  }
}

/**
 * Parses `rgb(r,g,b)` and its modern and legacy spellings.
 *
 * Three spellings are in the wild and all three are legal: comma-separated legacy, space-separated with a slash before
 * the alpha, and percentages inside either. An alpha may be absent.
 *
 * The separator handling is the part worth being careful with. `Scanner::atEnd` skips commas as well as whitespace,
 * because that is what the SVG list grammar needs -- so asking it whether input remains *consumes* the comma that
 * introduces the alpha, and a following `take(",")` can never match. Everything here therefore skips whitespace only and
 * decides from `peek`.
 */
std::optional<Color> parseRgbFunction(Scanner scanner) {
  std::array<Scalar, 3> channels{};
  for (Scalar& channel: channels) {
    const auto value = scanner.takeNumber();
    if (!value) return std::nullopt;
    scanner.skipSpace();

    // Percentage-ness is decided per channel from what follows it, so a document that mixes the two forms -- and
    // `rgb(100% 0 0)` is one -- keeps each one's own meaning.
    if (scanner.take("%")) {
      channel = std::clamp(*value / 100.0f, 0.0f, 1.0f);
    } else {
      channel = std::clamp(*value / 255.0f, 0.0f, 1.0f);
    }
    scanner.skipSpace();
  }

  Scalar alpha = 1;
  const char separator = scanner.peek();
  if (separator == ',' || separator == '/') {
    scanner.take(std::string_view{ &separator, 1 });
    const auto value = scanner.takeNumber();
    if (!value) return std::nullopt;
    scanner.skipSpace();
    alpha = scanner.take("%")
              ? std::clamp(*value / 100.0f, 0.0f, 1.0f)
              : std::clamp(*value, 0.0f, 1.0f);
    scanner.skipSpace();
  }

  // Anything left over means this was not one of the forms above, and guessing which part to keep would turn a
  // malformed colour into a slightly wrong one.
  if (!scanner.rest().empty()) return std::nullopt;

  const auto to8 = [](const Scalar v) { return static_cast<uint8_t>(std::lround(v * 255.0f)); };
  return Color::MakeRGBA8(to8(channels[0]), to8(channels[1]), to8(channels[2]),
    static_cast<uint8_t>(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255.0f)));
}

} // namespace anonymous

// ==================================================================================================
// Small helpers
// ==================================================================================================

std::string_view trim(const std::string_view text) noexcept {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && space(text[begin])) ++begin;
  while (end > begin && space(text[end - 1])) --end;
  return text.substr(begin, end - begin);
}

bool isSvgSpace(const char c) noexcept { return space(c); }

// ==================================================================================================
// Length
// ==================================================================================================

Scalar LengthContext::diagonal() const noexcept {
  return std::sqrt(viewportWidth * viewportWidth + viewportHeight * viewportHeight) / std::sqrt(2.0f);
}

std::optional<Scalar> LengthContext::resolve(const Length& length, const LengthUnit axis) const {
  switch (length.unit) {
    case LengthUnit::Number:
    case LengthUnit::Px:
      return length.value;
    case LengthUnit::Percent: {
      // Which dimension a percentage is a fraction of is the caller's business, expressed by passing the axis as the
      // unit of a *length*: width, height, diagonal. The three cases are the whole of SVG's rule.
      if (axis == LengthUnit::Px) return length.value / 100.0f * viewportWidth;
      if (axis == LengthUnit::Ex) return length.value / 100.0f * viewportHeight;
      return length.value / 100.0f * diagonal();
    }
    case LengthUnit::Em:
      return length.value * fontSize;
    case LengthUnit::Ex:
      return length.value * xHeight;
    default:
      return length.value * absoluteUnitScale(length.unit);
  }
}

std::optional<Length> parseLength(const std::string_view text) {
  Scanner scanner(text);
  const auto value = scanner.takeNumber();
  if (!value) return std::nullopt;

  scanner.skipSpace();
  const std::string_view suffix = trim(scanner.rest());
  const auto unit = unitFromSuffix(suffix);
  if (!unit) return std::nullopt;

  return Length{ *value, *unit };
}

std::optional<Scalar> parseNumber(const std::string_view text) {
  Scanner scanner(text);
  const auto value = scanner.takeNumber();
  if (!value || !scanner.atEnd()) return std::nullopt;
  return value;
}

std::optional<std::vector<Length>> parseLengthList(const std::string_view text) {
  Scanner scanner(text);
  std::vector<Length> lengths;
  while (!scanner.atEnd()) {
    const auto value = scanner.takeNumber();
    if (!value) return std::nullopt;
    // A unit may follow without a separator, and may not be separated by one either, so the suffix is read from the
    // rest of the input up to the next character that cannot be part of a unit.
    std::string_view remainder = scanner.rest();
    size_t suffixLength = 0;
    while (suffixLength < remainder.size() && (std::isalpha(static_cast<unsigned char>(remainder[suffixLength])) !=
                                               0)) {
      ++suffixLength;
    }
    if (suffixLength < remainder.size() && remainder[suffixLength] == '%') ++suffixLength;

    const auto unit = unitFromSuffix(remainder.substr(0, suffixLength));
    if (!unit) return std::nullopt;
    for (size_t i = 0; i < suffixLength; ++i) scanner.skipSpace(), scanner.take(remainder.substr(i, 1));

    lengths.push_back(Length{ *value, *unit });
  }
  return lengths;
}

// ==================================================================================================
// Colour and paint
// ==================================================================================================

std::optional<Color> parseColor(const std::string_view text) {
  const std::string_view value = trim(text);
  if (value.empty()) return std::nullopt;

  if (value.front() == '#') return parseHexColor(value.substr(1));

  // The functional forms, matched on the name up to the parenthesis rather than by trying each in turn.
  const size_t open = value.find('(');
  if (open != std::string_view::npos && value.back() == ')') {
    std::string_view name = trim(value.substr(0, open));
    std::string lower;
    lower.reserve(name.size());
    for (const char c: name) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    const std::string_view arguments = value.substr(open + 1, value.size() - open - 2);
    if (lower == "rgb" || lower == "rgba") {
      Scanner scanner(arguments);
      return parseRgbFunction(scanner);
    }
    // `hsl` and `hsla` are legal SVG and are not in the corpus. Returning nothing makes the property fall back, which
    // is the documented behaviour for a value the renderer does not understand, and is honest about what happened.
    return std::nullopt;
  }

  // A named colour. Case is significant in neither CSS nor SVG.
  std::string lower;
  lower.reserve(value.size());
  for (const char c: value) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  for (const NamedColor& named: kNamedColors) {
    if (named.name == lower) return colorFromRgb(named.rgb);
  }

  // `transparent` is a colour rather than an absence: it fills with alpha zero, and the difference matters for
  // `fill="transparent"` against `fill="none"` only in that the first still takes part in a gradient's stop list.
  if (lower == "transparent") return Color::MakeRGBA8(0, 0, 0, 0);

  return std::nullopt;
}

std::optional<Paint> parsePaint(const std::string_view text) {
  const std::string_view value = trim(text);
  if (value.empty()) return std::nullopt;

  std::string lower;
  lower.reserve(std::min<size_t>(value.size(), 12));
  for (size_t i = 0; i < value.size() && i < 12; ++i) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value[i]))));
  }

  if (lower == "none") return Paint{ Paint::Type::None, Color::MakeRGBA8(0, 0, 0, 0), {}, std::nullopt };
  if (lower == "currentcolor") return Paint{ Paint::Type::CurrentColor, Color::Black(), {}, std::nullopt };

  // `url(#id)` optionally followed by a fallback colour, as in `fill="url(#g) #eee"`.
  if (lower.rfind("url(", 0) == 0) {
    const size_t close = value.find(')');
    if (close == std::string_view::npos) return std::nullopt;

    std::string reference{ trim(value.substr(4, close - 4)) };
    // The reference may be quoted: `url('#id')` and `url("#id")` are both accepted.
    if (reference.size() >= 2 && (reference.front() == '\'' || reference.front() == '"') &&
        reference.back() == reference.front()) {
      reference = reference.substr(1, reference.size() - 2);
    }
    if (!reference.empty() && reference.front() == '#') reference.erase(0, 1);

    Paint paint;
    paint.type = Paint::Type::IRI;
    paint.iri = std::move(reference);

    const std::string_view remainder = trim(value.substr(close + 1));
    if (!remainder.empty()) {
      const auto fallback = parseColor(remainder);
      if (!fallback) return std::nullopt;
      paint.fallback = *fallback;
    }
    return paint;
  }

  if (const auto color = parseColor(value)) {
    return Paint{ Paint::Type::Color, *color, {}, std::nullopt };
  }
  return std::nullopt;
}

std::optional<DashArray> parseDashArray(const std::string_view text) {
  const std::string_view value = trim(text);
  if (value.empty()) return std::nullopt;

  // `none` is a dash array with no dashes, which is the same as not having one. Returning an empty array rather than
  // nothing lets the caller replace an inherited array with "solid", which is the difference `none` makes.
  std::string lower;
  for (const char c: value) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  if (lower == "none") return DashArray{};

  const auto lengths = parseLengthList(value);
  if (!lengths) return std::nullopt;
  // A list of one value means an alternating pattern of that value twice, per the SVG grammar.
  DashArray dashes;
  dashes.values = *lengths;
  if (dashes.values.size() == 1) dashes.values.push_back(dashes.values.front());
  return dashes;
}

Scalar DashArray::totalLength(const LengthContext& context) const {
  Scalar total = 0;
  for (const Length& length: values) {
    // A negative or unresolvable dash makes the whole array invalid, which SVG says renders as a solid line rather than
    // as dashes of length zero.
    const auto resolved = context.resolve(length, LengthUnit::Px);
    if (!resolved || *resolved < 0) return std::numeric_limits<Scalar>::infinity();
    total += *resolved;
  }
  return values.empty() ? std::numeric_limits<Scalar>::infinity() : total;
}

// ==================================================================================================
// Points and transforms
// ==================================================================================================

std::optional<std::vector<Point>> parsePoints(const std::string_view text) {
  Scanner scanner(text);
  std::vector<Point> points;
  for (;;) {
    const auto x = scanner.takeNumber();
    if (!x) break;
    const auto y = scanner.takeNumber();
    if (!y) return std::nullopt; // an odd number of coordinates is a malformed list
    points.emplace_back(*x, *y);
  }
  if (!scanner.atEnd()) return std::nullopt;
  return points;
}

std::optional<Matrix> parseTransform(const std::string_view text) {
  Scanner scanner(text);
  // `Matrix` default-constructs to the identity, which is what an empty transform list means.
  Matrix result;

  for (;;) {
    scanner.skipSpace();
    if (scanner.rest().empty()) break;

    std::string_view name = scanner.takeIdentifier();
    if (name.empty()) return std::nullopt;
    if (!scanner.take("(")) return std::nullopt;

    std::vector<Scalar> arguments;
    // The argument list runs to the closing parenthesis, and the scanner's number rule handles the commas and spaces.
    while (!scanner.take(")")) {
      const auto value = scanner.takeNumber();
      if (!value) return std::nullopt;
      arguments.push_back(*value);
    }

    constexpr Scalar kDegreesToRadians = 3.14159265358979323846f / 180.0f;
    Matrix step;

    if (name == "matrix") {
      if (arguments.size() != 6) return std::nullopt;
      // The six values are the 2D affine (a, b, c, d, e, f), which maps a point to
      //
      //     x' = a*x + c*y + e
      //     y' = b*x + d*y + f
      //
      // and `Matrix` multiplies a column vector, so the values land in the first two columns and the last row. Written
      // as the sixteen the constructor takes, that is (a, b, 0, 0), (c, d, 0, 0), (0, 0, 1, 0), (e, f, 0, 1) -- and
      // getting the interleaving wrong transposes the transform, which shows up as a shape rotated the wrong way
      // rather than as an error.
      step = Matrix(arguments[0], arguments[1], 0, 0, //
        arguments[2], arguments[3], 0, 0, //
        0, 0, 1, 0, //
        arguments[4], arguments[5], 0, 1);
    } else if (name == "translate") {
      if (arguments.empty() || arguments.size() > 2) return std::nullopt;
      step = Matrix::MakeTranslation(Vector3{ arguments[0], arguments.size() > 1 ? arguments[1] : 0, 0 });
    } else if (name == "scale") {
      if (arguments.empty() || arguments.size() > 2) return std::nullopt;
      const Scalar sy = arguments.size() > 1 ? arguments[1] : arguments[0];
      step = Matrix::MakeScale(Vector3{ arguments[0], sy, 1 });
    } else if (name == "rotate") {
      if (arguments.empty() || arguments.size() > 3) return std::nullopt;
      const Matrix rotation = Matrix::MakeRotationZ(Radians{ arguments[0] * kDegreesToRadians });
      if (arguments.size() == 3) {
        // A rotation about a point is the translate-rotate-translate product, which is what the three-argument form is
        // for. `A * B` applies B first, so the translation to the origin is written last.
        const Vector3 centre{ arguments[1], arguments[2], 0 };
        step = Matrix::MakeTranslation(centre) * rotation * Matrix::MakeTranslation(-centre);
      } else {
        step = rotation;
      }
    } else if (name == "skewX") {
      if (arguments.size() != 1) return std::nullopt;
      step = Matrix::MakeSkew(std::tan(arguments[0] * kDegreesToRadians), 0);
    } else if (name == "skewY") {
      if (arguments.size() != 1) return std::nullopt;
      step = Matrix::MakeSkew(0, std::tan(arguments[0] * kDegreesToRadians));
    } else {
      return std::nullopt;
    }

    // SVG lists transforms outermost first: `transform="A B"` maps a point as A(B(p)), and `A * B` is the same thing
    // in this convention. So each step is appended on the right, and reversing the two silently mirrors the whole
    // transform about the line through the origin at the midway angle -- which looks plausible for a single rotate and
    // is wrong for any pair that does not commute.
    result = result * step;
  }

  return result;
}

// ==================================================================================================
// viewBox and preserveAspectRatio
// ==================================================================================================

std::optional<Rect> parseViewBox(const std::string_view text) {
  Scanner scanner(text);
  std::array<Scalar, 4> values{};
  for (Scalar& value: values) {
    const auto number = scanner.takeNumber();
    if (!number) return std::nullopt;
    value = *number;
  }
  if (!scanner.atEnd()) return std::nullopt;

  // A negative width or height makes the viewBox empty, which SVG says disables rendering of the element. Reporting it
  // as an empty rectangle lets the caller apply that rule rather than this parser deciding to fail.
  return Rect::MakeXYWH(values[0], values[1], std::max(0.0f, values[2]), std::max(0.0f, values[3]));
}

std::optional<PreserveAspectRatio> parsePreserveAspectRatio(const std::string_view text) {
  Scanner scanner(text);
  PreserveAspectRatio result;

  if (scanner.takeKeyword("defer")) scanner.skipSpace();

  if (scanner.takeKeyword("none")) {
    result.none = true;
    return scanner.atEnd() ? std::optional<PreserveAspectRatio>{ result } : std::nullopt;
  }

  const std::string_view align = scanner.takeIdentifier();
  if (align.empty()) return std::nullopt;

  static constexpr std::array<std::pair<std::string_view, PreserveAspectRatio::Align>, 9> kAlignments{ {
    { "xMinYMin", PreserveAspectRatio::Align::XMinYMin },
    { "xMidYMin", PreserveAspectRatio::Align::XMidYMin },
    { "xMaxYMin", PreserveAspectRatio::Align::XMaxYMin },
    { "xMinYMid", PreserveAspectRatio::Align::XMinYMid },
    { "xMidYMid", PreserveAspectRatio::Align::XMidYMid },
    { "xMaxYMid", PreserveAspectRatio::Align::XMaxYMid },
    { "xMinYMax", PreserveAspectRatio::Align::XMinYMax },
    { "xMidYMax", PreserveAspectRatio::Align::XMidYMax },
    { "xMaxYMax", PreserveAspectRatio::Align::XMaxYMax },
  } };
  bool found = false;
  for (const auto& [spelling, alignment]: kAlignments) {
    if (align == spelling) {
      result.align = alignment;
      found = true;
      break;
    }
  }
  if (!found) return std::nullopt;

  // The meet-or-slice keyword is optional and defaults to `meet`.
  if (scanner.takeKeyword("slice")) {
    result.slice = true;
  } else {
    scanner.takeKeyword("meet");
  }

  return scanner.atEnd() ? std::optional<PreserveAspectRatio>{ result } : std::nullopt;
}

ViewBoxTransform fitViewBox(const Rect& viewBox, const Scalar width, const Scalar height,
  const PreserveAspectRatio& aspect) {
  ViewBoxTransform result;
  if (viewBox.getWidth() <= 0 || viewBox.getHeight() <= 0) return result;

  const Scalar scaleX = width / viewBox.getWidth();
  const Scalar scaleY = height / viewBox.getHeight();

  Scalar scale = 1;
  if (aspect.none) {
    // `none` stretches, which is the one case where the two axes are allowed to differ -- and a single `scale` cannot
    // describe it, so the matrix is built from the two and the reported scale is left at one.
    result.matrix = Matrix::MakeScale(Vector3{ scaleX, scaleY, 1 }) *
                    Matrix::MakeTranslation(Vector3{ -viewBox.getLeft(), -viewBox.getTop(), 0 });
    return result;
  }

  scale = aspect.slice ? std::max(scaleX, scaleY) : std::min(scaleX, scaleY);

  const Scalar renderedWidth = viewBox.getWidth() * scale;
  const Scalar renderedHeight = viewBox.getHeight() * scale;

  Scalar dx = 0;
  Scalar dy = 0;
  switch (aspect.align) {
    case PreserveAspectRatio::Align::None:
    case PreserveAspectRatio::Align::XMinYMin:
      break;
    case PreserveAspectRatio::Align::XMidYMin:
      dx = (width - renderedWidth) / 2;
      break;
    case PreserveAspectRatio::Align::XMaxYMin:
      dx = width - renderedWidth;
      break;
    case PreserveAspectRatio::Align::XMinYMid:
      dy = (height - renderedHeight) / 2;
      break;
    case PreserveAspectRatio::Align::XMidYMid:
      dx = (width - renderedWidth) / 2;
      dy = (height - renderedHeight) / 2;
      break;
    case PreserveAspectRatio::Align::XMaxYMid:
      dx = width - renderedWidth;
      dy = (height - renderedHeight) / 2;
      break;
    case PreserveAspectRatio::Align::XMinYMax:
      dy = height - renderedHeight;
      break;
    case PreserveAspectRatio::Align::XMidYMax:
      dx = (width - renderedWidth) / 2;
      dy = height - renderedHeight;
      break;
    case PreserveAspectRatio::Align::XMaxYMax:
      dx = width - renderedWidth;
      dy = height - renderedHeight;
      break;
  }

  result.matrix = Matrix::MakeTranslation(Vector3{ dx, dy, 0 }) * Matrix::MakeScale(Vector3{ scale, scale, 1 }) *
                  Matrix::MakeTranslation(Vector3{ -viewBox.getLeft(), -viewBox.getTop(), 0 });
  result.scale = scale;
  return result;
}

// ==================================================================================================
// Keywords
// ==================================================================================================

#define CORAL_SVG_KEYWORD_PARSER(Function, Type, ...)                       \
  std::optional<Type> Function(const std::string_view text) {               \
    const std::string_view value = trim(text);                              \
    static constexpr std::pair<std::string_view, Type> kTable[] = {__VA_ARGS__}; \
    for (const auto& [spelling, result] : kTable) {                         \
      if (value == spelling) return result;                                 \
    }                                                                       \
    return std::nullopt;                                                    \
  }

// clang-format off
CORAL_SVG_KEYWORD_PARSER(parseLineCap,      LineCap,      {"butt", LineCap::Butt},                   {"round", LineCap::Round},                       {"square", LineCap::Square})
CORAL_SVG_KEYWORD_PARSER(parseLineJoin,     LineJoin,     {"miter", LineJoin::Miter},                {"round", LineJoin::Round},                      {"bevel", LineJoin::Bevel})
CORAL_SVG_KEYWORD_PARSER(parseFillRule,     FillRule,     {"nonzero", FillRule::NonZero},            {"evenodd", FillRule::EvenOdd})
CORAL_SVG_KEYWORD_PARSER(parseSpreadMethod, SpreadMethod, {"pad", SpreadMethod::Pad},                {"reflect", SpreadMethod::Reflect},              {"repeat", SpreadMethod::Repeat})
CORAL_SVG_KEYWORD_PARSER(parseVisibility,   Visibility,   {"visible", Visibility::Visible},          {"hidden", Visibility::Hidden},                  {"collapse", Visibility::Collapse})
CORAL_SVG_KEYWORD_PARSER(parseUnits,        Units,        {"userSpaceOnUse", Units::UserSpaceOnUse}, {"objectBoundingBox", Units::ObjectBoundingBox})
CORAL_SVG_KEYWORD_PARSER(parseTextAnchor,   TextAnchor,   {"start", TextAnchor::Start},              {"middle", TextAnchor::Middle},                  {"end", TextAnchor::End})
CORAL_SVG_KEYWORD_PARSER(parseFontStyle,    FontStyle,    {"normal", FontStyle::Normal},             {"italic", FontStyle::Italic},                   {"oblique", FontStyle::Oblique})
// clang-format on

#undef CORAL_SVG_KEYWORD_PARSER

std::optional<FontWeight> parseFontWeight(const std::string_view text) {
  const std::string_view value = trim(text);
  static constexpr std::pair<std::string_view, FontWeight> kTable[] = {
    // clang-format off
    { "normal",  FontWeight::Normal  },
    { "bold",    FontWeight::Bold    },
    { "bolder",  FontWeight::Bolder  },
    { "lighter", FontWeight::Lighter },
    { "100",     FontWeight::W100    },
    { "200",     FontWeight::W200    },
    { "300",     FontWeight::W300    },
    { "400",     FontWeight::W400    },
    { "500",     FontWeight::W500    },
    { "600",     FontWeight::W600    },
    { "700",     FontWeight::W700    },
    { "800",     FontWeight::W800    },
    { "900",     FontWeight::W900    },
    // clang-format on
  };
  for (const auto& [spelling, result]: kTable) {
    if (value == spelling) return result;
  }
  return std::nullopt;
}

} // namespace graphics::svg
