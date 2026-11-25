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

#ifndef CORAL_GRAPHICS_SVG_SVGSCANNER_H
#define CORAL_GRAPHICS_SVG_SVGSCANNER_H

#include <geometry/Scalar.h>

#include <cctype>
#include <cstdlib>
#include <optional>
#include <string_view>

/**
 * @file SvgScanner.h
 *
 * The tokeniser every SVG value parser is built on.
 *
 * It lives under `src/` rather than `include/private/` because nothing outside `src/svg/` includes it: the parsers in
 * `SvgTypes.cpp` and `SvgPathData.cpp` are its only callers, so a `src/` path is already as private as it needs to be.
 *
 * ## The one rule that makes SVG grammar different
 *
 * Separators are optional wherever the next number could not continue the previous one. `M0 0L10-10` is a move, a line,
 * and four numbers, with no space between `10` and `-10` because the minus sign ends the first number by starting the
 * second. A tokeniser that required a comma or a space between list items would reject most real SVG -- including most
 * of the corpus this library is tested against -- so `takeNumber` is defined by what a number *is* rather than by what
 * follows it.
 */

namespace graphics::svg {

/** The four characters SVG treats as whitespace, plus the form feed the grammar also allows. */
inline bool isSvgSpaceChar(const char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/** @return @p text with leading and trailing whitespace removed. */
inline std::string_view trimmed(const std::string_view text) noexcept {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && isSvgSpaceChar(text[begin])) ++begin;
  while (end > begin && isSvgSpaceChar(text[end - 1])) --end;
  return text.substr(begin, end - begin);
}

/**
 * A cursor over an SVG attribute value.
 *
 * Copies of it are cheap and are passed by value where a parse needs to look ahead and then restart -- which is how the
 * `rgb(...)` parser reads its arguments without consuming them from the caller's position.
 */
class Scanner {
  public:
  explicit Scanner(const std::string_view text) : mText(trimmed(text)) {}

  /** Skips whitespace and the commas that may separate list items. */
  void skipSpaceAndCommas() {
    while (mPosition < mText.size() && (isSvgSpaceChar(mText[mPosition]) || mText[mPosition] == ',')) {
      ++mPosition;
    }
  }

  void skipSpace() {
    while (mPosition < mText.size() && isSvgSpaceChar(mText[mPosition])) {
      ++mPosition;
    }
  }

  /** @return true once nothing but separators is left. */
  [[nodiscard]] bool atEnd() {
    skipSpaceAndCommas();
    return mPosition >= mText.size();
  }

  [[nodiscard]] char peek() const noexcept { return mPosition < mText.size() ? mText[mPosition] : '\0'; }

  [[nodiscard]] std::string_view rest() const noexcept { return mText.substr(mPosition); }

  [[nodiscard]] size_t position() const noexcept { return mPosition; }

  /**
   * @return the next number, or nothing when the cursor is not on one.
   *
   * `strtod` does the conversion because it is correctly rounded and already knows the exponent forms; the work here is
   * deciding where the number ends, which its end pointer answers exactly.
   */
  std::optional<Scalar> takeNumber() {
    skipSpaceAndCommas();
    if (mPosition >= mText.size()) return std::nullopt;

    const char* const begin = mText.data() + mPosition;
    char* end = nullptr;
    const double value = std::strtod(begin, &end);
    if (end == begin) return std::nullopt;

    mPosition += static_cast<size_t>(end - begin);
    return static_cast<Scalar>(value);
  }

  /** Consumes @p keyword when it is next, ignoring case, as SVG does for identifiers. */
  bool takeKeyword(const std::string_view keyword) {
    skipSpace();
    if (mText.size() - mPosition < keyword.size()) return false;
    for (size_t i = 0; i < keyword.size(); ++i) {
      const auto a = static_cast<unsigned char>(mText[mPosition + i]);
      const auto b = static_cast<unsigned char>(keyword[i]);
      if (std::tolower(a) != std::tolower(b)) return false;
    }
    mPosition += keyword.size();
    return true;
  }

  /** Consumes @p text exactly, case-sensitively. For syntax rather than for keywords. */
  bool take(const std::string_view text) {
    skipSpace();
    if (mText.compare(mPosition, text.size(), text) != 0) return false;
    mPosition += text.size();
    return true;
  }

  /** @return an identifier: letters, digits and the few punctuation characters SVG allows inside one. */
  std::string_view takeIdentifier() {
    skipSpace();
    const size_t begin = mPosition;
    while (mPosition < mText.size()) {
      const char c = mText[mPosition];
      const auto uc = static_cast<unsigned char>(c);
      if (std::isalnum(uc) == 0 && c != '-' && c != '_' && c != '.') break;
      ++mPosition;
    }
    return mText.substr(begin, mPosition - begin);
  }

  private:
  std::string_view mText;
  size_t mPosition = 0;
};

} // namespace graphics::svg

#endif // CORAL_GRAPHICS_SVG_SVGSCANNER_H
