/*
 * Copyright (C) 2026 The Coral Project
 *
 * A one-off: shape a string and report what came back, so a rendering problem can be traced to shaping, to glyph
 * lookup, or to the drawing. Build it through the example project's tools target.
 *
 * Usage:  shape_dump <font> <text>
 */

#include <graphics/Font.h>
#include <graphics/Path.h>
#include <graphics/TextBlob.h>
#include <graphics/Typeface.h>

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: shape_dump <font> <text>\n");
    return 1;
  }

  const auto typeface = graphics::Typeface::fromFile(argv[1]);
  if (typeface == nullptr) {
    std::fprintf(stderr, "could not load %s\n", argv[1]);
    return 1;
  }

  std::fprintf(stderr, "typeface: family='%s' glyphs=%u unitsPerEm=%u\n", typeface->familyName().c_str(),
          typeface->glyphCount(), typeface->unitsPerEm());

  const auto font = graphics::Font::create(typeface, 16.0f);
  if (font == nullptr) {
    std::fprintf(stderr, "could not create a font\n");
    return 1;
  }

  const std::string text = argv[2];
  std::fprintf(stderr, "measureWidth('%s') = %.3f\n", text.c_str(), static_cast<double>(font->measureWidth(text)));

  const graphics::TextBlob blob = font->shape(text);
  std::fprintf(stderr, "blob: %zu glyphs, advance=(%.2f, %.2f)\n", blob.count(),
          static_cast<double>(blob.advance().x), static_cast<double>(blob.advance().y));

  // `advanceForGlyph` and the blob's own advance should agree, since the second is the sum of the first. Printing
  // both next to the font's em size is what says which units each is in: an advance near `unitsPerEm` is font units,
  // one near the point size is pixels.
  std::fprintf(stderr, "unitsPerEm=%u  size=%.1f\n", typeface->unitsPerEm(), static_cast<double>(font->size()));
  size_t index = 0;
  for (const graphics::Glyph& glyph: blob.glyphs()) {
    const graphics::Path outline = typeface->glyphPath(glyph.id);
    std::fprintf(stderr,
            "  [%zu] id=%-6u advance=(%10.4f,%8.4f) advanceForGlyph=%10.4f offset=(%8.4f,%8.4f) outlineVerbs=%zu\n",
            index, glyph.id, static_cast<double>(glyph.advance.x), static_cast<double>(glyph.advance.y),
            static_cast<double>(font->advanceForGlyph(glyph.id)), static_cast<double>(glyph.offset.x),
            static_cast<double>(glyph.offset.y), outline.verbCount());
    ++index;
  }

  // The outline's own extent, in font units, so the ratio between what is drawn and how far the pen moves is visible.
  if (!blob.glyphs().empty()) {
    const graphics::Path first = typeface->glyphPath(blob.glyphs().front().id);
    const graphics::Rect bounds = first.bounds();
    std::fprintf(stderr, "first outline bounds: %.1f x %.1f font units\n",
            static_cast<double>(bounds.getRight() - bounds.getLeft()),
            static_cast<double>(bounds.getBottom() - bounds.getTop()));
  }

  return 0;
}
