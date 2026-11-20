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

#ifndef CORAL_GRAPHICS_FONT_MANAGER_H
#define CORAL_GRAPHICS_FONT_MANAGER_H

#include <graphics/FontStyle.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace graphics {

class Typeface;

/**
 * Finds typefaces by family name and style.
 *
 * ## The job
 *
 * Turning "the sans-serif face, bold" into a `Typeface` requires three things: knowing which fonts exist, being able
 * to read each one's family name and style, and a rule for what to do when the exact request is not present. This
 * class is the first and the third; reading a font's identity is `Typeface`'s.
 *
 * ## Nearest match, not exact match
 *
 * A request that cannot be satisfied exactly is answered with the closest thing rather than with nothing. Skia does
 * the same, and the reason is that text is almost always more useful drawn slightly wrong than not drawn: a caller
 * asking for a bold face and receiving the regular one produces text that looks thin, whereas receiving nothing
 * produces a blank page. The one case that is *not* approximated is a family that does not exist at all -- there is
 * nothing nearest to it -- so that falls back to the default face, and the caller can tell the two apart by comparing
 * the returned face's family name with the one it asked for.
 *
 * ## What "the fonts on this system" means per platform
 *
 * There is no portable way to enumerate installed fonts, so the directories are listed per platform and every font
 * file in them is offered to `Typeface`. That is a scan rather than a query of the platform's font database, which
 * costs a little on first use and is cached afterwards; the alternative on each platform is a different API, and this
 * keeps one implementation with three short lists of paths.
 *
 * On Android the fonts live under `/system/fonts` and are reachable, so the same scan works there without JNI.
 */
class FontManager {
public:
  /**
   * @return the shared manager, scanning the system's font directories on first use.
   *
   * A singleton because the scan is expensive and the answer is the same for every caller. The returned reference is
   * valid for the life of the process.
   */
  [[nodiscard]] static FontManager& shared();

  /** A manager with no fonts in it, for a caller that wants to add its own and match only against those. */
  FontManager();
  ~FontManager();

  FontManager(const FontManager&) = delete;
  FontManager& operator=(const FontManager&) = delete;
  FontManager(FontManager&&) = delete;
  FontManager& operator=(FontManager&&) = delete;

  // ================================================================================================
  // Building a collection
  // ================================================================================================

  /**
   * Adds the font at @p path, returning it, or null when it cannot be read.
   *
   * The added face is remembered, so a later match can find it. A path that has already been added returns the face
   * that was read the first time rather than reading the file again.
   */
  std::shared_ptr<Typeface> addFontFromFile(const std::string& path);

  /**
   * Adds a font held in memory.
   *
   * @param owner keeps the bytes alive for as long as the face is used; FreeType reads tables out of the buffer on
   *              demand rather than copying them. See `Typeface::fromMemory`.
   */
  std::shared_ptr<Typeface> addFontFromMemory(const uint8_t* data, size_t size, std::shared_ptr<const void> owner);

  /**
   * Scans the platform's font directories and adds every font found.
   *
   * @return how many faces were added. Called by `shared()`; a caller with its own manager can call it to opt in, and
   *         calling it twice adds nothing the second time.
   */
  size_t addSystemFonts();

  // ================================================================================================
  // Finding one
  // ================================================================================================

  /**
   * @return the face that best matches @p familyName and @p style.
   *
   * The family name is matched case-insensitively, and a request with an empty family name means "any family" -- which
   * is what a caller that only cares about the style wants. When nothing matches, the default face is returned; it is
   * never null once any font has been added.
   *
   * The match is by family only for the family's own faces: a bold request is answered from the regular and bold faces
   * of the *same* family where one exists, before any other family is considered. That is the ordering that makes a
   * fallback useful -- the shape of the letters is a bigger difference than their weight.
   */
  [[nodiscard]] std::shared_ptr<Typeface> matchFamilyStyle(
          const std::string& familyName, const FontStyle& style) const;

  /**
   * @return the face to use when nothing else fits.
   *
   * Never null once any font has been added. Chosen to be an upright, normal-weight face, because a fallback is
   * reached for body text far more often than for a heading.
   */
  [[nodiscard]] std::shared_ptr<Typeface> defaultTypeface() const;

  /**
   * Scans for a font that has a glyph for @p codePoint.
   *
   * This is font *fallback*: the character is missing from the chosen face, so a face that has it is needed instead.
   * @p preferred is tried first, so a caller that knows which family suits the text can have it honoured when that
   * family turns out to cover the character after all.
   *
   * @return the first face found, or null when none of them has the character.
   */
  [[nodiscard]] std::shared_ptr<Typeface> matchCharacter(
          uint32_t codePoint, const std::shared_ptr<Typeface>& preferred = nullptr) const;

  /** @return every family name known to this manager, sorted and without duplicates. */
  [[nodiscard]] std::vector<std::string> familyNames() const;

  /** @return how many faces have been added. */
  [[nodiscard]] size_t typefaceCount() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> mImpl;
};

} // namespace graphics

#endif //CORAL_GRAPHICS_FONT_MANAGER_H
