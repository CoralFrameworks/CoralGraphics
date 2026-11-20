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

#include <graphics/Typeface.h>

#include <graphics/Path.h>

#include "FontContext.h"
#include "PlatformGlyphRasterizer.h"

#include <minilog/log.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <utility>

#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H

namespace graphics {

namespace {

/**
 * @return the style a face declares, read from its tables.
 *
 * Two sources, and they disagree often enough to be worth explaining. The `OS/2` table's `usWeightClass`
 * is the modern, precise answer, but it is absent from older PostScript-flavoured fonts -- so the
 * `style_flags` on the face, which FreeType derives from the style name, is the fallback. Checking the
 * table first and the flags second is what gives bold the right weight for a font that states 600 where
 * another states 700.
 */
FontStyle readStyle(const FT_Face face) {
  int weight = FontWeight::kNormal;

  if (const auto* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2))) {
    if (os2->usWeightClass >= 1 && os2->usWeightClass <= 1000) {
      weight = static_cast<int>(os2->usWeightClass);
    }
  } else if ((face->style_flags & FT_STYLE_FLAG_BOLD) != 0) {
    weight = FontWeight::kBold;
  }

  FontStyle::Slant slant = FontStyle::Slant::Upright;
  if ((face->style_flags & FT_STYLE_FLAG_ITALIC) != 0) {
    // FreeType reports italic and oblique together, because most fonts do. The distinction is only
    // recoverable from the style name, and a font that says "Oblique" means it.
    const std::string styleName = font_detail::ftString(face->style_name);
    slant = styleName.find("Oblique") != std::string::npos ? FontStyle::Slant::Oblique
                                                           : FontStyle::Slant::Italic;
  }

  // The width class, when the font states one. A face that omits `OS/2` keeps the normal width, which
  // is the right assumption: without the table there is nothing to say it is condensed.
  int width = FontStyle::kNormalWidth;
  if (const auto* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2))) {
    if (os2->usWidthClass >= 1 && os2->usWidthClass <= 9) width = static_cast<int>(os2->usWidthClass);
  }

  return FontStyle(weight, slant, width);
}

} // namespace anonymous

void Typeface::populateFromFace(void* faceHandle) {
  const auto face = static_cast<FT_Face>(faceHandle);
  Typeface& typeface = *this;
  // `family_name` is the typographic family ("Helvetica Neue"), which is what a caller asking for a font
  // by name means. The `name` field is the legacy one and often carries the style as well.
  // `family_name` is the only name FreeType surfaces as a field. There is no `name` member -- the
  // legacy full name in the `name` table is not exposed -- so a font whose name table has no
  // typographic family leaves this null, and `ftString` answers with an empty string rather than
  // handing the caller a pointer they would have to check.
  //
  // This passage used to end "which the test asserts rather than working around". It was wrong, and
  // wrong in a way worth recording: the test loads a *real* font and asserts the opposite, that the
  // family name is non-empty, because on a font that parsed correctly an empty name means the tables
  // were found and misread. So the empty string here is a defensive answer on a path nothing
  // exercises -- a claim of coverage rather than coverage.
  typeface.mFamilyName = font_detail::ftString(face->family_name);

  typeface.mFaceName = font_detail::ftString(face->style_name);
  typeface.mStyle = readStyle(face);
  typeface.mGlyphCount = static_cast<uint32_t>(face->num_glyphs);
  typeface.mUnitsPerEm = face->units_per_EM != 0 ? static_cast<uint32_t>(face->units_per_EM) : 1000u;
  typeface.mScalable = FT_IS_SCALABLE(face) != 0;
  typeface.mMonospaced = FT_IS_FIXED_WIDTH(face) != 0;

  // The PostScript name lives in the `name` table and FreeType does not surface it as a field, so it is
  // looked up by its own accessor. The fallback matters: a caller using this to ask a platform font API
  // for the same face needs something to ask with, and the family name resolves for the common case even
  // where it is ambiguous for an unusual one.
  typeface.mPostScriptName = typeface.mFamilyName;
  if (const char* psName = FT_Get_Postscript_Name(face)) {
    typeface.mPostScriptName = psName;
  }
}

Typeface::~Typeface() = default;

std::shared_ptr<Typeface> Typeface::fromFile(const std::string& path) {
  const auto library = font_detail::Library::acquire();
  if (!library) {
    // FreeType itself failed to start. Logged once by the library holder; there is nothing to add.
    return nullptr;
  }

  FT_Face face = nullptr;
  const FT_Error error = FT_New_Face(library->handle(), path.c_str(), 0, &face);
  if (error != 0) {
    MINILOG_WARN("[graphics/font] could not open '{}' as a font (FreeType error {})", path,
            static_cast<int>(error));
    return nullptr;
  }

  // `new` rather than `make_shared`: the constructor is private, so the class controls how instances
  // come into being and can guarantee the implementation is attached before anyone sees one.
  auto typeface = std::shared_ptr<Typeface>(new Typeface());
  typeface->mImpl = std::make_unique<Impl>();
  typeface->mImpl->library = library;
  typeface->mImpl->face = face;
  typeface->mImpl->filePath = path;
  typeface->mImpl->faceIndex = 0;

  typeface->populateFromFace(face);

  // The face is left at its default size. Everything size-dependent is computed by `Font`, which sets
  // the size on the shared face for the duration of one call -- and `Font` is not re-entrant with
  // respect to this face, which is why the typeface is not exposed as mutable.
  return typeface;
}

std::shared_ptr<Typeface> Typeface::fromMemory(const uint8_t* data, const size_t size,
        std::shared_ptr<const void> owner) {
  if (data == nullptr || size == 0) return nullptr;

  const auto library = font_detail::Library::acquire();
  if (!library) return nullptr;

  // `FT_New_Memory_Face` takes a non-const pointer because it may need to patch the tables in place for
  // some formats. The buffer belongs to the caller and is kept alive by `owner` below; the cast is how
  // FreeType's API expresses "I promise not to write through this except where allowed".
  FT_Face face = nullptr;
  const FT_Error error = FT_New_Memory_Face(library->handle(), data, static_cast<FT_Long>(size), 0, &face);
  if (error != 0) {
    MINILOG_WARN("[graphics/font] could not read {} bytes as a font (FreeType error {})", size,
            static_cast<int>(error));
    return nullptr;
  }

  auto typeface = std::shared_ptr<Typeface>(new Typeface());
  typeface->mImpl = std::make_unique<Impl>();
  typeface->mImpl->library = library;
  typeface->mImpl->face = face;
  // Held so that the bytes cannot be freed while FreeType still reads tables out of them.
  typeface->mImpl->owner = std::move(owner);

  typeface->populateFromFace(face);
  return typeface;
}

uint32_t Typeface::glyphForCodePoint(const uint32_t codePoint) const noexcept {
  if (mImpl == nullptr || mImpl->face == nullptr) return 0;
  // `FT_Get_Char_Index` returns 0 for "not found", which is also the missing-glyph slot, so the two
  // answers coincide and no separate signal is needed.
  return static_cast<uint32_t>(FT_Get_Char_Index(mImpl->face, codePoint));
}

bool Typeface::hasGlyphForCodePoint(const uint32_t codePoint) const noexcept {
  return glyphForCodePoint(codePoint) != 0;
}

namespace {

/**
 * Builds a `Path` from FreeType's outline callbacks.
 *
 * ## The three decisions this makes
 *
 * **The y axis is not flipped here.** A glyph is described with y increasing upwards and this returns it that way,
 * because the flip belongs with the other half of the transform -- the size and the caller's own matrix -- and
 * applying it here would mean a path that could not be composed with anything else. It is also what the reference
 * does: Skia negates y in its own decomposer only because it has already folded the size in at that point.
 *
 * **Conic curves become quads, not cubics.** FreeType's `conic_to` is a quadratic with an implied control point --
 * the curve passes through the midpoint of the two points it is given -- and `Path::quadTo` is exactly that shape.
 * Converting to a cubic would be a lossless description of the same curve but twice the storage and a different
 * path for every glyph, so the two would not compare equal.
 *
 * **Degenerate segments are dropped.** A `line_to` to the point the pen is already at, or a curve whose control and
 * end points coincide, contributes nothing; FreeType emits them around contour joins, and keeping them would leave
 * zero-length segments in the path that later stages would have to special-case.
 */
class OutlineSink {
public:
  explicit OutlineSink(Path& path) noexcept: mPath(path) {}

  static int moveTo(const FT_Vector* to, void* context) {
    auto& self = *static_cast<OutlineSink*>(context);
    self.finishContour();
    self.mCurrent = *to;
    return 0;
  }

  static int lineTo(const FT_Vector* to, void* context) {
    auto& self = *static_cast<OutlineSink*>(context);
    if (self.mCurrent.x == to->x && self.mCurrent.y == to->y) return 0;
    self.startContourIfNeeded();
    self.mPath.lineTo(toPoint(*to));
    self.mCurrent = *to;
    return 0;
  }

  static int conicTo(const FT_Vector* control, const FT_Vector* to, void* context) {
    auto& self = *static_cast<OutlineSink*>(context);
    if (self.mCurrent.x == control->x && self.mCurrent.y == control->y && self.mCurrent.x == to->x &&
        self.mCurrent.y == to->y) {
      return 0;
    }
    self.startContourIfNeeded();
    self.mPath.quadTo(toPoint(*control), toPoint(*to));
    self.mCurrent = *to;
    return 0;
  }

  static int cubicTo(const FT_Vector* control1, const FT_Vector* control2, const FT_Vector* to, void* context) {
    auto& self = *static_cast<OutlineSink*>(context);
    self.startContourIfNeeded();
    self.mPath.cubicTo(toPoint(*control1), toPoint(*control2), toPoint(*to));
    self.mCurrent = *to;
    return 0;
  }

  /** Ends the contour started by the last @c moveTo, without emitting anything for it. */
  void finishContour() noexcept { mStarted = false; }

private:
  /** @return @p vector as a point. The fixed-point shift is zero, so the values are already in font units. */
  static Point toPoint(const FT_Vector& vector) noexcept {
    return Point { static_cast<Scalar>(vector.x), static_cast<Scalar>(vector.y) };
  }

  /**
   * Emits the pending `moveTo`.
   *
   * Deferred because a contour whose first segment is degenerate would otherwise leave a lone `moveTo` in the path --
   * a contour of one point, which no fill or stroke can do anything with and which would make two equal glyphs
   * compare unequal.
   */
  void startContourIfNeeded() {
    if (mStarted) return;
    mPath.moveTo(toPoint(mCurrent));
    mStarted = true;
  }

  Path& mPath;
  FT_Vector mCurrent { 0, 0 };
  bool mStarted = false;
};

} // namespace anonymous

Path Typeface::glyphPath(const uint32_t glyphId) const {
  Path path;
  if (mImpl == nullptr || mImpl->face == nullptr) return path;

  // The face has one glyph slot and one selected size, so the outline load and the colour-bitmap load
  // below must not overlap: both write the same slot.
  std::lock_guard<std::mutex> lock(mImpl->glyphMutex);



  // The outline is wanted in font units, so the glyph is loaded with no scaling at all. `FT_LOAD_NO_SCALE` asks for
  // exactly that, and `FT_LOAD_NO_HINTING` keeps the grid-fitting out of it: a hinted outline depends on the size it
  // was hinted for, and this path is size-independent by design.
  const FT_Int32 flags = FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP;
  if (FT_Load_Glyph(mImpl->face, static_cast<FT_UInt>(glyphId), flags) != 0) return path;

  if (mImpl->face->glyph == nullptr || mImpl->face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
    // A bitmap-only glyph, or a slot FreeType did not fill. An empty path is the honest answer: there is no outline.
    return path;
  }

  OutlineSink sink(path);
  const FT_Outline_Funcs functions {
    &OutlineSink::moveTo, &OutlineSink::lineTo, &OutlineSink::conicTo, &OutlineSink::cubicTo, 0, 0,
  };

  if (FT_Outline_Decompose(&mImpl->face->glyph->outline, &functions, &sink) != 0) {
    // Decomposition failed part-way. What was emitted is a partial outline, so it is discarded rather than returned:
    // a path that describes half a letter is worse than no path, because it still draws.
    return Path {};
  }

  // FreeType leaves the final contour open, and a glyph's contours are closed by definition -- a fill would close
  // them anyway, but a stroke would not, and a stroked glyph is a real use.
  //
  // Only when there is something to close, though. A glyph with no contours -- a space is the ordinary case -- comes
  // back from the decomposer without ever calling the move callback, and closing unconditionally would then leave a
  // path consisting of a single `close` verb. That is not empty, so `isEmpty` would say a space has geometry and a
  // renderer would try to draw it.
  if (path.verbCount() > 0) path.close();
  return path;
}

GlyphBitmap Typeface::glyphBitmap(const uint32_t glyphId, const float pixelSize) const {
  return glyphBitmap(glyphId, pixelSize, false, 0.0f);
}

GlyphBitmap Typeface::glyphBitmapExact(const uint32_t glyphId, const float pixelSize,
    const bool subpixel, const float subpixelPhase) const {
  if (mImpl == nullptr || mImpl->face == nullptr || !(pixelSize > 0.0f)) return {};
  if (subpixel && !mImpl->filePath.empty() && !FT_HAS_COLOR(mImpl->face)) {
    GlyphBitmap platform;
    if (font_detail::rasterizePlatformGlyph(mImpl->filePath, mImpl->faceIndex, glyphId, pixelSize,
          subpixelPhase, platform)) {
      return platform;
    }
  }
  // FreeType has no uncached platform phase hook here; the transform path is still quantised to the
  // 26.6 delta grid, which is finer than a pixel.
  return glyphBitmap(glyphId, pixelSize, subpixel, subpixelPhase);
}

GlyphBitmap Typeface::glyphBitmap(const uint32_t glyphId, const float pixelSize,
    const bool subpixel, const float subpixelPhase) const {
  GlyphBitmap result;
  if (mImpl == nullptr || mImpl->face == nullptr || !(pixelSize > 0.0f)) return result;

  // The cache is keyed in 26.6 pixels, the same quantisation FreeType uses for sizes, so two `Font`s that
  // differ by less than a 1/64th of a pixel share one bitmap. The subpixel flag and the 1/16-pixel
  // phase are part of the key: neither an LCD rasterisation nor a different phase is interchangeable
  // with the origin one.
  const auto quantised = static_cast<uint64_t>(std::llround(static_cast<double>(pixelSize) * 64.0));
  const auto sizeKey = static_cast<uint32_t>(std::min<uint64_t>(quantised, 0xFFFFFFFFull));
  int phaseIndex = static_cast<int>(std::lround(std::clamp(subpixelPhase, 0.0f, 0.999f) * 16.0f));
  phaseIndex = std::clamp(phaseIndex, 0, 15);
  const uint64_t key = (static_cast<uint64_t>(glyphId & 0xFFFFu) << 40) |
                       (subpixel ? (1ull << 39) : 0ull) |
                       (static_cast<uint64_t>(phaseIndex) << 32) | sizeKey;

  std::lock_guard<std::mutex> lock(mImpl->glyphMutex);
  if (const auto found = mImpl->glyphBitmaps.find(key); found != mImpl->glyphBitmaps.end()) {
    return found->second;
  }

  // The platform's own text engine is the preferred ClearType/CoreText source. It is consulted only
  // for a file-backed face: an in-memory face has no path for the platform API to reopen, so that
  // case keeps the FreeType LCD/grayscale path below. A colour font (emoji) must stay on FreeType:
  // its glyphs are bitmaps with their own colours, and a coverage analyser would turn them into a
  // tinted silhouette.
  if (subpixel && !mImpl->filePath.empty() && !FT_HAS_COLOR(mImpl->face)) {
    GlyphBitmap platform;
    if (font_detail::rasterizePlatformGlyph(mImpl->filePath, mImpl->faceIndex, glyphId, pixelSize,
          subpixelPhase, platform)) {
      mImpl->glyphBitmaps.emplace(key, platform);
      return platform;
    }
  }

  FT_Face face = mImpl->face;
  const bool bitmapStrike = face->num_fixed_sizes > 0 && face->available_sizes != nullptr;
  int best = 0;
  float strikeScale = 1.0f;
  if (bitmapStrike) {
    // A bitmap face has one strike per size it was designed for, and FreeType will not rescale a
    // `CBDT` bitmap to an arbitrary size. Pick the closest strike and let the canvas scale the pixels.
    float bestDistance = std::numeric_limits<float>::max();
    for (int index = 0; index < face->num_fixed_sizes; ++index) {
      const FT_Bitmap_Size& strike = face->available_sizes[index];
      const float ppem = strike.y_ppem > 0 ? static_cast<float>(strike.y_ppem) / 64.0f
                                           : static_cast<float>(strike.height);
      if (!(ppem > 0.0f)) continue;
      const float distance = std::fabs(ppem - pixelSize);
      if (distance < bestDistance) {
        bestDistance = distance;
        best = index;
      }
    }
    if (FT_Select_Size(face, best) != 0) return result;
    const FT_Bitmap_Size& strike = face->available_sizes[best];
    const float strikePpem = strike.y_ppem > 0 ? static_cast<float>(strike.y_ppem) / 64.0f
                                               : static_cast<float>(strike.height);
    strikeScale = strikePpem > 0.0f ? pixelSize / strikePpem : 1.0f;
  } else {
    // Outline faces take the hinted raster path. `FT_LOAD_TARGET_LIGHT` grid-fits stems and the
    // baseline without the vertical-only compromises of TARGET_MONO, which keeps CJK at small sizes
    // legible while still producing a reusable 8-bit coverage mask.
    const FT_F26Dot6 size26_6 = static_cast<FT_F26Dot6>(std::llround(pixelSize * 64.0));
    if (FT_Set_Char_Size(face, 0, size26_6, 0, 0) != 0) return result;
  }

  // `FT_LOAD_COLOR` asks for the embedded colour bitmap rather than an outline, and `FT_LOAD_RENDER`
  // fills the slot's bitmap. For an outline face TARGET_LIGHT asks FreeType to grid-fit the outline
  // before rendering it; TARGET_LCD asks for horizontal subpixel coverage instead. A bitmap strike
  // has no outline to hint or subpixel-render, so it keeps its own pixels either way.
  FT_Int32 loadFlags = FT_LOAD_COLOR | FT_LOAD_RENDER;
  if (!bitmapStrike) {
    loadFlags |= subpixel ? FT_LOAD_TARGET_LCD : FT_LOAD_TARGET_LIGHT;
  }
  // FreeType has no per-call phase argument; a delta transform moves the outline inside the pixel.
  // Restore the face immediately after the render so the next glyph starts from the identity again.
  const bool phaseTransform = subpixel && !bitmapStrike && phaseIndex != 0;
  if (phaseTransform) {
    FT_Matrix matrix{ 0x10000, 0, 0, 0x10000 };
    FT_Vector delta{ static_cast<FT_Pos>(phaseIndex * 4), 0 };
    FT_Set_Transform(face, &matrix, &delta);
  }
  const FT_Error loadError = FT_Load_Glyph(face, static_cast<FT_UInt>(glyphId), loadFlags);
  if (phaseTransform) FT_Set_Transform(face, nullptr, nullptr);
  if (loadError != 0) return result;
  const FT_GlyphSlot slot = face->glyph;
  if (slot == nullptr || slot->format != FT_GLYPH_FORMAT_BITMAP) return result;
  const FT_Bitmap& bitmap = slot->bitmap;
  if (bitmap.width == 0 || bitmap.rows == 0 || bitmap.buffer == nullptr) return result;
  const bool lcd = bitmap.pixel_mode == FT_PIXEL_MODE_LCD;
  if (bitmap.pixel_mode != FT_PIXEL_MODE_BGRA && bitmap.pixel_mode != FT_PIXEL_MODE_GRAY && !lcd) {
    return result;
  }

  const float scale = strikeScale;

  // An LCD bitmap is three coverage bytes per pixel, so its FreeType width is three times the glyph's
  // logical width; the three channels are kept as RGB coverage and alpha stays opaque.
  const unsigned glyphWidth = lcd ? bitmap.width / 3u : bitmap.width;
  if (glyphWidth == 0) return result;
  Bitmap pixels = Bitmap::allocate(static_cast<int>(glyphWidth), static_cast<int>(bitmap.rows));
  if (pixels.isEmpty()) return result;
  Pixmap view = pixels.lockPixels();
  if (!view.isValid()) return result;

  const int pitch = static_cast<int>(bitmap.pitch);
  const int step = pitch < 0 ? -pitch : pitch;
  if (step <= 0) return result;
  const bool color = bitmap.pixel_mode == FT_PIXEL_MODE_BGRA;
  for (unsigned row = 0; row < bitmap.rows; ++row) {
    const unsigned sourceRow = pitch < 0 ? bitmap.rows - 1 - row : row;
    const uint8_t* source = bitmap.buffer + static_cast<size_t>(sourceRow) * static_cast<size_t>(step);
    for (unsigned column = 0; column < glyphWidth; ++column) {
      uint32_t argb = 0;
      if (color) {
        // `FT_PIXEL_MODE_BGRA` is premultiplied BGRA, which is the same byte order as a little-endian
        // ARGB word read as bytes, so the conversion is a reassembly rather than a multiply.
        const uint8_t* texel = source + static_cast<size_t>(column) * 4;
        argb = (static_cast<uint32_t>(texel[3]) << 24) | (static_cast<uint32_t>(texel[2]) << 16) |
               (static_cast<uint32_t>(texel[1]) << 8) | static_cast<uint32_t>(texel[0]);
      } else if (lcd) {
        const uint8_t* texel = source + static_cast<size_t>(column) * 3;
        argb = (255u << 24) | (static_cast<uint32_t>(texel[0]) << 16) |
               (static_cast<uint32_t>(texel[1]) << 8) | static_cast<uint32_t>(texel[2]);
      } else {
        // A grey bitmap is an alpha mask. Premultiplied white is what lets the canvas tint it with the
        // text colour without darkening the edges.
        const auto alpha = static_cast<uint32_t>(source[column]);
        argb = (alpha << 24) | (alpha << 16) | (alpha << 8) | alpha;
      }
      view.setPixelARGB32(static_cast<int>(column), static_cast<int>(row), argb);
    }
  }

  result.pixels = std::move(pixels);
  result.left = static_cast<float>(slot->bitmap_left) * scale;
  result.top = static_cast<float>(slot->bitmap_top) * scale;
  result.width = static_cast<float>(glyphWidth) * scale;
  result.height = static_cast<float>(bitmap.rows) * scale;
  result.color = color;
  result.subpixel = lcd;

  // Returning the cached copy is safe: `Bitmap` copies share their pixels, and neither FreeType nor this
  // class writes them again.
  mImpl->glyphBitmaps.emplace(key, result);
  return result;
}

} // namespace graphics
