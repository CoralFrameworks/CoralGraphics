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

#ifndef CORAL_GRAPHICS_TYPEFACE_H
#define CORAL_GRAPHICS_TYPEFACE_H

#include <graphics/Bitmap.h>
#include <graphics/FontStyle.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace graphics {

class Font;
class Path;

/**
 * One OpenType variation axis and the range it accepts.
 *
 * A variable font is not a family with several files: it is one file whose outlines, metrics and
 * sometimes even glyph set are interpolated from the coordinates a caller chooses. `wght` (weight),
 * `wdth` (width), `opsz` (optical size) and `ital` (italic) are the registered tags; a font may declare
 * private ones. The values are in the font's own *design* units, which are also the units
 * `FontVariation::value` uses -- so a caller can pass the numbers the font documents without learning
 * a second, normalised coordinate system.
 *
 * `hidden` mirrors the `hidden` bit of the OpenType axis record: such an axis is still adjustable, but
 * a UI should not offer it in the list a user picks from.
 */
struct FontVariationAxis {
  /** The four-character OpenType axis tag, e.g. `"wght"`. */
  std::string tag;
  /** The axis name from the font's `name` table, or empty when it has none. */
  std::string name;
  float minValue = 0.0f;
  float defaultValue = 0.0f;
  float maxValue = 0.0f;
  bool hidden = false;
};

/**
 * One axis coordinate: `tag` selects the axis and `value` is its design-unit position.
 *
 * A vector of these is the whole of a variation request. The type is deliberately the same value a
 * `FontNamedInstance` reports, so choosing a named instance and setting coordinates by hand are the
 * same operation with the same validation.
 */
struct FontVariation {
  std::string tag;
  float value = 0.0f;
};

/** A pre-defined point in a variable font's design space, as the font's `fvar` table lists it. */
struct FontNamedInstance {
  /** The instance's name from the font's `name` table. */
  std::string name;
  /** The coordinates this instance names, in the same design units as `FontVariationAxis`. */
  std::vector<FontVariation> variations;
};

/**
 * The platform's ClearType-style coverage shaping.
 *
 * DirectWrite exposes these as `IDWriteRenderingParams`; the UI compositor consumes the coverage from
 * `glyphBitmap` and applies the same gamma/contrast/level so the result follows the system's ClearType
 * tuning instead of a hard-coded curve. Platforms without a platform text engine report the
 * conventional defaults.
 */
struct SubpixelRenderingParams {
  float gamma = 2.2f;
  float contrast = 0.0f;
  float clearTypeLevel = 1.0f;
  /** 0 = flat, 1 = RGB, 2 = BGR (matches `DWRITE_PIXEL_GEOMETRY`). */
  int pixelGeometry = 1;
};

/** @return the current platform ClearType parameters, or conventional defaults off Windows. */
[[nodiscard]] SubpixelRenderingParams subpixelRenderingParams();

/**
 * Drops the process-wide cached parameters so the next `subpixelRenderingParams()` re-reads the
 * platform settings. A caller that handles a Windows ClearType/display-settings change should call
 * this; existing run masks are keyed by the parameter values and rebuild on the next frame.
 */
void refreshSubpixelRenderingParams();

/**
 * One glyph rendered to pixels, for a face that stores colour bitmaps instead of outlines.
 *
 * ## Why a font needs a second glyph form
 *
 * Noto Color Emoji, and most emoji fonts, are `CBDT`/`CBLC` (or `sbix`/`COLR`) fonts: the glyph is a
 * picture, not a contour. Asking such a face for an outline returns an empty path -- that is the
 * honest answer to "what contour is this?" -- so a text run drawn only from `glyphPath` loses every
 * emoji. This is the other half of the answer.
 *
 * ## Coordinates and scale
 *
 * `left`, `top`, `width` and `height` are already in pixels at the size the caller asked for, with
 * `top` measured **upwards from the baseline** and `left` to the right of the pen, matching the
 * way `Font::shape` positions glyphs. `pixels` is the bitmap at the font's own strike resolution;
 * drawing it into the rectangle those four floats describe scales it to the requested size. That
 * split is deliberate: the pixels are immutable and cacheable, while the destination size changes
 * with every `Font::withSize`, and scaling a bitmap is a sampling decision the canvas makes anyway.
 */
struct GlyphBitmap {
  /** Premultiplied RGBA pixels at the face's strike resolution. */
  Bitmap pixels;

  /** Pen to the bitmap's left edge, in pixels at the requested size. */
  float left = 0.0f;

  /** Baseline to the bitmap's top edge, in pixels at the requested size, positive upwards. */
  float top = 0.0f;

  /** Destination width, in pixels at the requested size. */
  float width = 0.0f;

  /** Destination height, in pixels at the requested size. */
  float height = 0.0f;

  /**
   * True when the pixels carry colour, false when they are an alpha mask.
   *
   * A caller drawing a colour glyph should not tint it with the text colour -- that would turn every
   * emoji into a monochrome silhouette -- while an alpha mask is *meant* to be tinted. Saying which
   * one this is here avoids the caller guessing from the font's name or from a pixel scan.
   */
  bool color = false;

  /**
   * True when `pixels` carries per-channel (RGB) coverage rather than a single alpha mask.
   *
   * The red, green and blue channels are three independent coverage values for a horizontally
   * subpixel-positioned glyph; alpha is 255. This is the data a ClearType-style renderer needs, and it
   * is deliberately distinct from `color`: a subpixel glyph is still tinted by the text colour, it is
   * just composited per channel. A renderer that cannot do that must fall back to a grayscale glyph
   * rather than treating this as an ordinary alpha mask.
   */
  bool subpixel = false;

  [[nodiscard]] bool isValid() const noexcept {
    return !pixels.isEmpty() && width > 0.0f && height > 0.0f;
  }
};

/**
 * A font file: one face's outlines and tables, at no particular size.
 *
 * ## Face, not family
 *
 * A "font family" is a name like *Helvetica* with several files behind it -- regular, bold, italic,
 * bold-italic. A `Typeface` is one of those files. Asking it for its family name gives the group it
 * belongs to; asking for its style says which member of the group it is. Resolving "Helvetica bold" to
 * a file belongs to `FontManager`, not here.
 *
 * ## Why it is not copyable
 *
 * The face behind it is a FreeType object with a single mutable state -- the current size, the current
 * transformation. Several `Font`s sharing one `Typeface` are fine, because a `Font` sets the size and
 * immediately reads what it needs. Two *owners* of the same face are not, because they would fight
 * over that state. `shared_ptr` therefore hands out shared access, and the class is neither copyable
 * nor movable so that ownership is never ambiguous.
 *
 * ## Polygonal outlines, and the CoreText gap
 *
 * `glyphPath` produces outlines through FreeType, on every platform including Apple. That is correct
 * everywhere and is what Flutter does on Linux and Windows.
 *
 * Flutter uses CoreText on Apple instead, and this does not. The reason is not a preference: the face
 * this class holds *is* a FreeType face -- `FontContext.h` names `FT_Face` and the metrics and shaping
 * paths reach through it -- so a CoreText outline would need a second backend behind the same interface,
 * selected at build time, with the face and every metric that reads it split in two. That is a real
 * piece of work and it is deferred rather than pretended, because the FreeType path is not wrong on
 * Apple: it produces the same outlines, from a different engine, at the cost of not sharing the system's
 * own font cache.
 *
 * What would change: an `FT_Face`/`CTFontRef` split in the private font context, and a
 * `glyphPath` implemented against `CTFontCreatePathForGlyph`. Everything above it -- metrics, shaping,
 * matching -- reads numbers the two engines agree on, so the split is confined to the private layer.
 */
class Typeface {
public:
  ~Typeface();

  Typeface(const Typeface&) = delete;
  Typeface& operator=(const Typeface&) = delete;

  /**
   * @return the face in @p path, or null when the file is missing or is not a font.
   *
   * The file is opened and parsed now rather than lazily, so a caller learns that a font is unusable
   * at the point of loading it instead of part-way through drawing a line of text.
   */
  [[nodiscard]] static std::shared_ptr<Typeface> fromFile(const std::string& path);

  /**
   * @return the face described by @p data, or null when it is not a font.
   *
   * The memory is referenced rather than copied -- FreeType reads the tables out of it on demand, which
   * is what makes loading a font cheap. **@p data must outlive the returned typeface.** The safe things
   * to pass are a static buffer or one already owned by a `shared_ptr`; pass that owner as @p owner and
   * the typeface will keep it alive, which is the only way to make the requirement enforceable rather
   * than merely documented. Copying a font into a local buffer and handing it here produces a
   * use-after-free that reads as garbage glyphs rather than as a crash.
   */
  [[nodiscard]] static std::shared_ptr<Typeface> fromMemory(const uint8_t* data, size_t size,
          std::shared_ptr<const void> owner = nullptr);

  /** The family name as the font states it, which is not always the file name. */
  [[nodiscard]] const std::string& familyName() const noexcept { return mFamilyName; }

  /** The face's own style, as the font states it. */
  [[nodiscard]] const FontStyle& style() const noexcept { return mStyle; }

  /** The name of this particular face, e.g. "Helvetica-Bold", or empty when the font omits it. */
  [[nodiscard]] const std::string& faceName() const noexcept { return mFaceName; }

  /** The number of glyphs in the face, which is also the range of valid glyph identifiers. */
  [[nodiscard]] uint32_t glyphCount() const noexcept { return mGlyphCount; }

  /** True when the face declares that its outlines are scalable, rather than bitmap-only. */
  [[nodiscard]] bool isScalable() const noexcept { return mScalable; }

  /** True when the face declares that all of its glyphs are monospaced. */
  [[nodiscard]] bool isMonospaced() const noexcept { return mMonospaced; }

  /**
   * True when the face has an `fvar` table and can be varied.
   *
   * A static face reports false and has no axes; asking it for variations is a caller error rather
   * than a silent identity, which is why `withVariations` returns null in that case.
   */
  [[nodiscard]] bool hasVariations() const noexcept { return !mVariationAxes.empty(); }

  /** The axes this face declares, in the font's own order. Empty for a static face. */
  [[nodiscard]] const std::vector<FontVariationAxis>& variationAxes() const noexcept {
    return mVariationAxes;
  }

  /** The named instances the face declares. Empty for a static face or one without named instances. */
  [[nodiscard]] const std::vector<FontNamedInstance>& namedInstances() const noexcept {
    return mNamedInstances;
  }

  /**
   * The face's current design-space position, one entry per axis.
   *
   * The default position for a face that has never been varied; the coordinates the font was cloned
   * at for a face `withVariations` produced. Empty for a static face.
   */
  [[nodiscard]] const std::vector<FontVariation>& variations() const noexcept { return mVariations; }

  /**
   * @return a new face from the same file at @p variations, or null when the request is invalid.
   *
   * ## Why this returns a new face instead of changing this one
   *
   * A FreeType face has one mutable design position and one glyph slot, which is exactly the state two
   * `Font`s sharing a `Typeface` would fight over. Cloning the face is what lets a UI animate a weight
   * axis on one text run while another run keeps the default; the file's bytes are shared through the
   * same owner, so the cost is a face object rather than a copy of the font.
   *
   * Axes not named in @p variations keep their current value, so `withVariations({{"wght", 700}})` on
   * a face already at `opsz` 12 preserves the optical size. Unknown tags, a tag that is not four
   * characters, and values outside the axis's range are refused: FreeType would clamp the last one, and
   * a silent clamp is the kind of "it worked" that hides a layout that asked for the wrong axis. The
   * error, when @p error is non-null, names the offending tag.
   */
  [[nodiscard]] std::shared_ptr<Typeface> withVariations(const std::vector<FontVariation>& variations,
          std::string* error = nullptr) const;

  /**
   * The units one em is divided into, as the font's own tables state it.
   *
   * Fonts state their metrics in integer units of an em -- typically 1000 for a PostScript-flavoured
   * font and 2048 for a TrueType one. Everything a `Font` reports is already converted to the size it
   * was created at, so this matters only to a caller doing its own arithmetic. It is also the number
   * that explains why a metric that "should" be exactly 12 comes out as 11.99.
   */
  [[nodiscard]] uint32_t unitsPerEm() const noexcept { return mUnitsPerEm; }

  /**
   * @return the glyph identifier for @p codePoint, or 0 when the face has no glyph for it.
   *
   * Glyph 0 is the "missing glyph" box by convention, so zero doubles as "not found" without needing a
   * separate answer. A `Font` is the better entry point for anything beyond a single lookup, because
   * shaping a script is not a per-character question.
   */
  [[nodiscard]] uint32_t glyphForCodePoint(uint32_t codePoint) const noexcept;

  /** @return true when the face has a glyph for @p codePoint, so the text will not show boxes. */
  [[nodiscard]] bool hasGlyphForCodePoint(uint32_t codePoint) const noexcept;

  /**
   * @return the outline of @p glyphId as a path, or an empty path when the glyph has no outline.
   *
   * ## Coordinates
   *
   * The outline is in the font's own units -- `unitsPerEm` to the em -- with **y increasing upwards**, which is how
   * a font describes a glyph and the opposite of the y-down convention every drawing surface here uses. The caller
   * applies the flip, because the caller is the only one that knows the size and the transform: baking a size into
   * the path would mean rebuilding it whenever the size changed, and baking the flip in would mean the path could
   * not be composed with anything else.
   *
   * A glyph with no outline -- a space, or a mark drawn only by positioning -- returns an empty path rather than a
   * null one, because "this glyph is blank" is an answer and not a failure.
   *
   * A glyph identifier the face does not have returns an empty path. FreeType rejects the load, and there is no
   * outline to report; a caller that wants the missing-glyph box should ask for glyph 0 by name, which is the
   * identifier the format reserves for it.
   */
  [[nodiscard]] Path glyphPath(uint32_t glyphId) const;

  /**
   * @return @p glyphId as a grayscale/colour bitmap at @p pixelSize.
   *
   * This is the compatibility overload for callers that do not opt into LCD coverage; it forwards to
   * the three-argument form with `subpixel = false`.
   */
  [[nodiscard]] GlyphBitmap glyphBitmap(uint32_t glyphId, float pixelSize) const;

  /**
   * @return @p glyphId as a bitmap at @p pixelSize, or an invalid one when it has no bitmap.
   *
   * `pixelSize` is the font size in pixels per em, which is what `Font::size` reports. A face with
   * several bitmap strikes gets the one closest to it, and the result is scaled to the requested
   * size by the caller -- FreeType does not synthesise an outline or rescale a `CBDT` bitmap itself.
   *
   * The result is cached per (glyph, size, subpixel), because a text run is drawn every frame while
   * the pixels do not change.
   *
   * When @p subpixel is true and the face has an outline, the glyph is rasterised with FreeType's
   * horizontal LCD filter and the result has `GlyphBitmap::subpixel` set. The face may still return a
   * grayscale bitmap (a bitmap strike, a glyph FreeType refuses to render in LCD mode), and a caller
   * that cannot composite RGB coverage must check the flag and fall back.
   */
  /**
   * @param subpixelPhase horizontal position inside the pixel, 0..1, used by the platform's
   *        ClearType/CoreText rasteriser. It is quantised into the glyph cache key at 1/16-pixel
   *        resolution, so a caller should pass a stable phase (TextPainter uses 1/16-pixel steps).
   */
  [[nodiscard]] GlyphBitmap glyphBitmap(uint32_t glyphId, float pixelSize, bool subpixel,
    float subpixelPhase = 0.0f) const;

  /**
   * Rasterises a subpixel glyph at the exact fractional phase without consulting the phase cache.
   *
   * This is the opt-in non-quantised path: `TextPainter` uses it when
   * `Paint::exactSubpixelPhase` is set. It is slower because the platform rasteriser runs on every
   * build, so prefer `glyphBitmap` for normal text.
   */
  [[nodiscard]] GlyphBitmap glyphBitmapExact(uint32_t glyphId, float pixelSize, bool subpixel,
    float subpixelPhase) const;

  /**
   * @return the PostScript name, which is the identifier a platform font API wants.
   *
   * Falls back to the family name when the font omits it, since a caller almost always wants
   * *something* usable rather than an empty string to special-case.
   */
  [[nodiscard]] const std::string& postScriptName() const noexcept { return mPostScriptName; }

private:
  friend class Font;

  Typeface() = default;

  struct Impl;

  /**
   * Reads everything this class reports out of an already-accepted FreeType face.
   *
   * A private member rather than a free function in the implementation file, because it writes every
   * private field: a free function would need friendship, which is a wider grant than one method.
   */
  void populateFromFace(void* face);

  std::string mFamilyName;
  std::string mFaceName;
  std::string mPostScriptName;
  FontStyle mStyle;
  uint32_t mGlyphCount = 0;
  uint32_t mUnitsPerEm = 0;
  bool mScalable = false;
  bool mMonospaced = false;

  /** The face's `fvar` axes, empty for a static face. */
  std::vector<FontVariationAxis> mVariationAxes;
  /** The face's named instances, as its `fvar`/`name` tables state them. */
  std::vector<FontNamedInstance> mNamedInstances;
  /** The design position this face was created or cloned at, one entry per axis. */
  std::vector<FontVariation> mVariations;

  /**
   * The FreeType and harfbuzz objects, hidden here rather than in the header.
   *
   * A `unique_ptr` to an incomplete type keeps `<ft2build.h>` and `<hb.h>` out of a public header. That
   * matters more than the usual compile-time argument: those headers define macros that would then leak
   * into every translation unit including a font header, and a client's own use of FreeType would
   * collide with them.
   */
  std::unique_ptr<Impl> mImpl;
};

} // namespace graphics

#endif //CORAL_GRAPHICS_TYPEFACE_H
