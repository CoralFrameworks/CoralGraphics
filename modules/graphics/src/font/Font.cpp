/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and limitations under the
 * License.
 */

#include <graphics/Font.h>

#include "FontContext.h"

#include <minilog/log.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#if defined(CORAL_GRAPHICS_HAS_HARFBUZZ)
#include <hb-ft.h>
#endif
#if defined(CORAL_GRAPHICS_HAS_HARFBUZZ)
#include <hb-ot.h>
#endif
#if defined(CORAL_GRAPHICS_HAS_HARFBUZZ)
#include <hb.h>
#endif

namespace graphics {

namespace {

/**
 * Scales a value stated in the font's own units to the caller's units.
 *
 * Every metric a font reports is an integer count of units of an em -- 2048 of them for a typical
 * TrueType font, 1000 for a PostScript-flavoured one. This is the one conversion between those and the
 * caller's pixels, and doing it in one place is what makes `Font` worth having.
 */
float scaleFromEm(const float value, const float size, const uint32_t unitsPerEm) noexcept {
  if (unitsPerEm == 0) return value;
  return value * size / static_cast<float>(unitsPerEm);
}

/** Reads one field of the `OS/2` table, or @p fallback when the font has no such table. */
template <typename T>
float os2Field(const FT_Face face, const T& member, const float fallback) {
  if (const auto* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2))) {
    return static_cast<float>(os2->*member);
  }
  return fallback;
}

} // namespace anonymous

/**
 * The harfbuzz font, one per `graphics::Font`.
 *
 * ## Why it is created eagerly rather than lazily
 *
 * A `Font` that cannot be shaped is not much use, and finding that out at `create` time is better than
 * at the first draw. The cost is one small allocation.
 */
struct Font::Impl {
#if defined(CORAL_GRAPHICS_HAS_HARFBUZZ)
  hb_font_t* hbFont = nullptr;

  ~Impl() {
    if (hbFont != nullptr) hb_font_destroy(hbFont);
  }
#endif
};

Font::~Font() = default;

std::shared_ptr<Font> Font::create(std::shared_ptr<Typeface> typeface, const float size) {
  if (typeface == nullptr || typeface->mImpl == nullptr || typeface->mImpl->face == nullptr) {
    return nullptr;
  }

  // A non-positive size is a caller mistake, not something to paper over. See the header.
  if (!(size > 0.0f)) {
    MINILOG_WARN("[graphics/font] refusing to create a font at size {}; the size must be positive", size);
    return nullptr;
  }
  // FreeType stores sizes in 26.6 fixed point, so a size beyond this cannot be represented and would
  // wrap to something small -- a text run that silently draws at the wrong size is worse than a refusal.
  constexpr float kMaxSize = static_cast<float>(std::numeric_limits<int32_t>::max() / 64);
  if (size > kMaxSize) {
    MINILOG_WARN("[graphics/font] refusing to create a font at size {}; it exceeds what a 26.6 fixed "
                 "point size can represent",
            size);
    return nullptr;
  }

  auto font = std::shared_ptr<Font>(new Font());
  font->mTypeface = std::move(typeface);
  font->mSize = size;

  // The harfbuzz font wraps the FreeType face. `hb_ft_font_create_referenced` takes its own reference to
  // the face, which is why the typeface must outlive it -- and the `shared_ptr` member guarantees that.
  font->mImpl = std::make_unique<Impl>();
#if defined(CORAL_GRAPHICS_HAS_HARFBUZZ)
  font->mImpl->hbFont = hb_ft_font_create_referenced(font->mTypeface->mImpl->face);
  if (font->mImpl->hbFont == nullptr) {
    MINILOG_WARN("[graphics/font] harfbuzz could not wrap the face at size {}", size);
    return nullptr;
  }

  // The OpenType functions give harfbuzz access to the font's own `GPOS` kerning and mark positioning.
  // Without this it falls back to the legacy `kern` table, which many modern fonts do not carry -- so a
  // font with excellent `GPOS` kerning would render with none at all.
  hb_ot_font_set_funcs(font->mImpl->hbFont);
#endif

  font->updateMetrics();
  return font;
}

std::shared_ptr<Font> Font::withSize(const float newSize) const {
  // Delegates rather than mutating, so the sizes stay independent and a shaped result stays valid. The
  // typeface is shared, so this is cheap: no file is re-opened and no tables are re-parsed.
  return create(mTypeface, newSize);
}

void Font::updateMetrics() {
  FT_Face face = mTypeface->mImpl->face;
  const uint32_t unitsPerEm = mTypeface->mUnitsPerEm;

  // Scaling the face is what makes FreeType report pixel values, but everything below is computed from
  // the *unscaled* design metrics instead: `face->size->metrics` is in 26.6 fixed point, whose
  // one-sixty-fourth resolution shows up as a visible rounding error at small sizes. The `units_per_EM`
  // fields are exact integers, so scaling those in float is both simpler and more accurate.
  mMetrics.ascent = scaleFromEm(static_cast<float>(face->ascender), mSize, unitsPerEm);
  mMetrics.descent = std::fabs(scaleFromEm(static_cast<float>(face->descender), mSize, unitsPerEm));
  mMetrics.lineGap = scaleFromEm(static_cast<float>(face->height - face->ascender + face->descender), mSize,
          unitsPerEm);

  // `OS/2` carries the metrics that matter for layout and is absent from some older fonts, so each has a
  // fallback derived from something FreeType always provides.
  // `sCapHeight`/`sxHeight` exist only in OS/2 version 2 and later. FreeType returns the table for an
  // older version too, with those members zero, so a zero has to fall back to the ascender the way a
  // missing table does -- otherwise a perfectly scalable font reports a cap height of zero and the
  // text layout loses its first line.
  const float os2CapHeight = os2Field(face, &TT_OS2::sCapHeight, 0.0f);
  const float os2XHeight = os2Field(face, &TT_OS2::sxHeight, 0.0f);
  mMetrics.capHeight = scaleFromEm(os2CapHeight > 0.0f ? os2CapHeight : static_cast<float>(face->ascender),
          mSize, unitsPerEm);
  mMetrics.xHeight = scaleFromEm(
          os2XHeight > 0.0f ? os2XHeight : static_cast<float>(face->ascender) * 0.5f, mSize, unitsPerEm);

  // The underline comes from `FT_Face` rather than the `OS/2` table, because `TT_OS2` has no underline
  // fields -- FreeType normalises `post` and `OS/2` into these two members itself, which is exactly the
  // kind of cross-table reconciliation worth taking rather than redoing.
  //
  // `underline_position` is negative for a position below the baseline, so it is negated to match the
  // "distance below the baseline, positive" convention every other metric here uses. Getting this sign
  // wrong draws the underline above the text.
  mMetrics.underlineOffset = std::fabs(scaleFromEm(static_cast<float>(face->underline_position), mSize,
          unitsPerEm));
  mMetrics.underlineThickness = std::fabs(scaleFromEm(static_cast<float>(face->underline_thickness), mSize,
          unitsPerEm));

  // Strike-through, which FreeType does not surface as `face` fields and only `OS/2` carries. A font
  // without the table leaves zero, which a caller can detect and choose to synthesise.
  mMetrics.strikeThroughOffset = std::fabs(scaleFromEm(
          os2Field(face, &TT_OS2::yStrikeoutPosition, static_cast<float>(face->ascender) * 0.5f), mSize,
          unitsPerEm));
  mMetrics.strikeThroughThickness = scaleFromEm(
          os2Field(face, &TT_OS2::yStrikeoutSize, 0.0f), mSize, unitsPerEm);

  // The harfbuzz scale, which decides the units it reports advances in.
  //
  // It is set to the font's own em rather than to the size, so that an advance comes back in the same units as the
  // outline `Typeface::glyphPath` produces -- and one conversion serves both. Setting it to the size instead made
  // advances pixels while outlines stayed in font units, and the two were then scaled as if they agreed.
  //
  // An integer em is exact, where a fractional size is not: harfbuzz keeps positions in 26.6 fixed point internally,
  // so a scale of 16.5 would round before the caller ever saw it.
#if defined(CORAL_GRAPHICS_HAS_HARFBUZZ)
  hb_font_set_scale(mImpl->hbFont, static_cast<int>(unitsPerEm), static_cast<int>(unitsPerEm));
#endif
}

uint32_t Font::glyphForCodePoint(const uint32_t codePoint) const noexcept {
  return mTypeface->glyphForCodePoint(codePoint);
}

bool Font::hasGlyphForCodePoint(const uint32_t codePoint) const noexcept {
  return mTypeface->hasGlyphForCodePoint(codePoint);
}

float Font::advanceForGlyph(const uint32_t glyphId) const noexcept {
#if !defined(CORAL_GRAPHICS_HAS_HARFBUZZ)
  FT_Face face = mTypeface != nullptr && mTypeface->mImpl != nullptr ? mTypeface->mImpl->face : nullptr;
  if (face == nullptr || FT_Load_Glyph(face, glyphId, FT_LOAD_NO_SCALE) != 0) return 0.0f;
  return scaleFromEm(static_cast<float>(face->glyph->metrics.horiAdvance), mSize, mTypeface->mUnitsPerEm);
#else
  if (mImpl == nullptr || mImpl->hbFont == nullptr) return 0.0f;

  // harfbuzz reports in the font's em, because that is the scale it was given, so this scales to pixels -- which is
  // what the caller asked for and what `measureWidth` returns.
  const float fontUnits = static_cast<float>(hb_font_get_glyph_h_advance(mImpl->hbFont, glyphId));
  return scaleFromEm(fontUnits, mSize, mTypeface->mUnitsPerEm);
#endif
}

TextBlob Font::shape(std::string_view text, const Direction direction) const {
  TextBlob blob;
#if !defined(CORAL_GRAPHICS_HAS_HARFBUZZ)
  std::vector<Glyph> glyphs;
  glyphs.reserve(text.size());
  for (size_t index = 0; index < text.size(); ++index) {
    const uint32_t codePoint = static_cast<uint32_t>(static_cast<unsigned char>(text[index]));
    Glyph glyph;
    glyph.id = glyphForCodePoint(codePoint);
    glyph.cluster = static_cast<uint32_t>(index);
    glyph.advance = Point { advanceForGlyph(glyph.id), 0.0f };
    glyphs.push_back(glyph);
  }
  return TextBlob(std::move(glyphs));
#else
  if (text.empty() || mImpl == nullptr || mImpl->hbFont == nullptr) return blob;

  hb_buffer_t* buffer = hb_buffer_create();
  if (buffer == nullptr) return blob;

  // Every early return below has to destroy the buffer, so it is wrapped rather than released by hand.
  struct BufferGuard {
    hb_buffer_t* buffer;
    ~BufferGuard() { hb_buffer_destroy(buffer); }
  } guard { buffer };

  hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()), 0, static_cast<int>(text.size()));

  // The direction and script are guessed from the text when not stated. Guessing is what harfbuzz is
  // built for -- the alternative is every caller having to run its own script detection.
  switch (direction) {
    case Direction::LeftToRight:
      hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
      break;
    case Direction::RightToLeft:
      hb_buffer_set_direction(buffer, HB_DIRECTION_RTL);
      break;
    case Direction::Auto:
      hb_buffer_guess_segment_properties(buffer);
      break;
  }

  // Cluster level `MONOTONE_CHARACTERS` makes each character start a cluster, so the cluster indices
  // line up with character boundaries in the input. The default level merges clusters aggressively,
  // which is cheaper but makes the mapping from a glyph back to a character in the string coarser --
  // and that mapping is what hit testing needs.
  hb_buffer_set_cluster_level(buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);

  hb_shape(mImpl->hbFont, buffer, nullptr, 0);

  unsigned int glyphCount = 0;
  const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &glyphCount);
  const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &glyphCount);

  std::vector<Glyph> glyphs;
  glyphs.reserve(glyphCount);
  for (unsigned int i = 0; i < glyphCount; ++i) {
    Glyph glyph;
    glyph.id = infos[i].codepoint;
    glyph.cluster = infos[i].cluster;

    // Positions come back in the font's em, which is the scale that was set, so there is no conversion here.
    //
    // It used to be `... / 64.0f`, on the reasoning that a FreeType-backed `hb_font` reports 26.6 fixed point. That is
    // true only when its scale is 64; the scale is the em, so the division made every advance sixty-four times too
    // small and a line of text drew itself as one pile of overlapping glyphs. The `hb_font_set_scale` above and this
    // have to be read together: one says what the numbers mean and the other must not reinterpret them.
    glyph.offset = Point { static_cast<float>(positions[i].x_offset), static_cast<float>(positions[i].y_offset) };
    glyph.advance = Point { static_cast<float>(positions[i].x_advance), static_cast<float>(positions[i].y_advance) };
    glyphs.push_back(glyph);
  }

  return TextBlob(std::move(glyphs));
#endif
}

float Font::measureWidth(const std::string_view text, const Direction direction) const {
  const TextBlob blob = shape(text, direction);

  // The blob's advance is in font units, like the advances it sums and the outlines they go with; the caller asked for
  // a layout width, so this is where it becomes pixels. Without the conversion the answer for a 16px font was under
  // one pixel per word.
  return scaleFromEm(blob.advance().x, mSize, mTypeface->mUnitsPerEm);
}

} // namespace graphics
