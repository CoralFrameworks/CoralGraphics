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

#ifndef CORAL_GRAPHICS_FONT_STYLE_H
#define CORAL_GRAPHICS_FONT_STYLE_H

#include <cstdint>

namespace graphics {

/**
 * The standard weight names, with the numeric values that correspond to them.
 *
 * These are the CSS and OpenType values, not arbitrary integers: a font's `OS/2` table states a weight
 * in this range, and a request for a weight has to be comparable with what the font declares. Naming
 * the conventional stops rather than exposing raw numbers is what makes `FontWeight::kBold` readable
 * and a literal `713` visibly unusual.
 */
namespace FontWeight {

inline constexpr int kThin = 100;
inline constexpr int kExtraLight = 200;
inline constexpr int kLight = 300;
inline constexpr int kNormal = 400;
inline constexpr int kMedium = 500;
inline constexpr int kSemiBold = 600;
inline constexpr int kBold = 700;
inline constexpr int kExtraBold = 800;
inline constexpr int kBlack = 900;

} // namespace FontWeight

/**
 * How a face is drawn: its weight, its width and its slant.
 *
 * A value type with no resources, which is why it is copied freely and compared with `==`. It
 * describes a *request* as much as a fact -- a caller asks for "bold italic" and the font manager
 * decides which file that is -- so equality is the question "would these two requests be satisfied by
 * the same face". The width is part of that, because an extended bold is a different request from a
 * condensed bold even though both are bold.
 */
class FontStyle {
public:
  /** How far a face leans. */
  enum class Slant : uint8_t {
    /** Upright. */
    Upright,
    /** Leaning, usually with a separate set of outlines. */
    Italic,
    /**
     * Upright outlines, slanted by the renderer.
     *
     * Distinct from italic, and not merely a rendering detail: many families ship no italic at all, and
     * a renderer that synthesizes a slant produces visibly different letterforms from one that has the
     * real thing. Keeping them apart lets a caller say which it would accept.
     */
    Oblique,
  };

  /** The OpenType `usWidthClass` value for a face of normal proportions. */
  static constexpr int kNormalWidth = 5;

  constexpr FontStyle() noexcept = default;

  constexpr FontStyle(const int weight, const Slant slant, const int width = kNormalWidth) noexcept
    : mWeight(weight),
      mWidth(width),
      mSlant(slant) {
  }

  /** The common case: normal weight, upright, normal width. */
  [[nodiscard]] static constexpr FontStyle Normal() noexcept {
    return { FontWeight::kNormal, Slant::Upright };
  }

  [[nodiscard]] constexpr int weight() const noexcept { return mWeight; }
  [[nodiscard]] constexpr int width() const noexcept { return mWidth; }
  [[nodiscard]] constexpr Slant slant() const noexcept { return mSlant; }

  [[nodiscard]] constexpr bool isBold() const noexcept { return mWeight >= FontWeight::kBold; }
  [[nodiscard]] constexpr bool isItalic() const noexcept { return mSlant != Slant::Upright; }

  friend constexpr bool operator==(const FontStyle& lhs, const FontStyle& rhs) noexcept {
    return lhs.mWeight == rhs.mWeight && lhs.mWidth == rhs.mWidth && lhs.mSlant == rhs.mSlant;
  }

  friend constexpr bool operator!=(const FontStyle& lhs, const FontStyle& rhs) noexcept {
    return !(lhs == rhs);
  }

private:
  int mWeight = FontWeight::kNormal;
  int mWidth = kNormalWidth;
  Slant mSlant = Slant::Upright;
};

} // namespace graphics

#endif //CORAL_GRAPHICS_FONT_STYLE_H
