/*
 * Copyright (C) 2025 The Coral Project
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

#ifndef CORAL_GRAPHICS_PAINT_H
#define CORAL_GRAPHICS_PAINT_H

#include <graphics/ColorFilter.h>
#include <graphics/Shader.h>

#include <geometry/Color.h>
#include <geometry/Scalar.h>

#include <cstdint>

namespace graphics {

class Bitmap;

/**
 * The Paint class holds the style and color information about how to draw geometries,
 * text and bitmaps.
 *
 * The colour and blend types come from `geometry`, which is why they are qualified: the graphics
 * backend has a `Color` of its own (`backend::Color`, four doubles, a clear-colour value) and it is a
 * different type with a different purpose. Naming the namespace here is what keeps a header that both
 * layers can see from silently picking one over the other.
 */
struct Paint {

  enum class Style : uint8_t {
    kFill,
    kStroke,
  };

  /**
   * How the end of an open subpath is finished.
   *
   * The three are not decoration: a butt cap ends exactly at the last point, a square cap extends half the width
   * beyond it, and a round cap extends half the width in a semicircle. Which one is used changes the geometry that
   * gets filled, and therefore the coverage of the pixels at the end.
   */
  enum class Cap : uint8_t {
    kButt,
    kRound,
    kSquare,
  };

  /**
   * How the corner where two segments meet is finished.
   *
   * A miter is the sharp corner, extended until the two edges cross; when that extension would run further than
   * `strokeMiter` times the stroke width the corner is bevelled instead, because otherwise a nearly straight
   * direction change produces a spike that reaches across the drawing.
   */
  enum class Join : uint8_t {
    kMiter,
    kRound,
    kBevel,
  };

  Color color = Color::Black();
  Style style = Style::kFill;
  graphics::BlendMode blendMode = graphics::BlendMode::kSrcOver;

  /**
   * The stroke width, in the space the geometry is given in.
   *
   * Ignored when @c style is @c kFill. Defaults to one rather than zero, because a zero-width stroke draws
   * nothing at all -- and a caller that set @c style to @c kStroke and forgot the width would get a blank
   * canvas with no clue why, whereas one unit is visible and obviously too thin.
   */
  Scalar strokeWidth = 1.0f;

  /** The cap applied to the ends of open subpaths. Ignored when @c style is @c kFill. */
  Cap strokeCap = Cap::kButt;

  /** The join applied where two segments meet. Ignored when @c style is @c kFill. */
  Join strokeJoin = Join::kMiter;

  /**
   * The miter limit: the multiple of the stroke width past which a miter is bevelled instead of extended.
   *
   * One would bevel every corner the moment the direction changed at all, so the useful range starts above it. Four
   * is the common default and the value SVG and the OpenType specification both use.
   */
  Scalar strokeMiter = 4.0f;

  /**
   * Whether to antialias.
   *
   * Honoured by the software canvas as supersampling, so turning it off is genuinely faster as well as
   * genuinely harder-edged. Kept on the paint rather than on the canvas because it is a property of the mark
   * being made: a caller drawing an axis-aligned rectangle at integer coordinates wants it off, and the next
   * draw in the same frame wants it on.
   */
  bool antiAlias = true;
  /**
   * Which destination colour channels this draw may write, bit 0 = red ... bit 3 = alpha.
   *
   * `0x0F` is the normal "write every channel" value. Subpixel text uses one pass per RGB channel with
   * a single bit set, so per-channel coverage can be composited with the ordinary source-over blend
   * instead of a blend equation the fixed-function hardware cannot express.
   */
  uint8_t channelMask = 0x0F;
  /**
   * Optional known backdrop for linear-light subpixel text.
   *
   * When set, the text painter solves a per-channel alpha against this colour so the final
   * source-over result is gamma-correct on that backdrop. Without it the painter has to assume a
   * light-on-dark composite (the historical single-alpha approximation). Callers that know the
   * solid background under a text layer should set both fields; an arbitrary image/gradient still
   * needs a layer-level linear-light composite.
   */
  bool hasTextBackground = false;
  Color textBackground = Color::Black();
  /**
   * Optional device-space backdrop image for arbitrary backgrounds/gradients.
   *
   * Non-owning: the caller keeps it alive while drawing. It takes precedence over `textBackground`
   * when set; the text painter samples it per channel at the pixel being composed, so an arbitrary
   * image or gradient gets the same linear-light treatment as a solid colour.
   */
  const Bitmap* textBackgroundBitmap = nullptr;
  /**
   * Opt into an uncached, exact fractional glyph phase for static labels. This bypasses the
   * 1/16-pixel glyph cache; normal scrolling/animated text should leave it false.
   */
  bool exactSubpixelPhase = false;
  /** Draw coverage-only masks into a linear-light layer (used by TextLayer's layer path). */
  bool coverageOnly = false;
  /** Marks a layer whose RGB is per-channel text coverage, composited linearily over its backdrop. */
  bool linearLightLayer = false;
  float linearLightGamma = 2.2f;
  float linearLightContrast = 0.0f;
  /** Whether glyphs are sampled from a signed distance field instead of a coverage bitmap. */
  bool sdfText = false;
  /** SDF styling: outline width, shadow offset (normalized texture units), glow radius. */
  float sdfOutlineWidth = 0.0f;
  float sdfShadowX = 0.0f;
  float sdfShadowY = 0.0f;
  float sdfGlow = 0.0f;
  /** SDF outline and shadow colours, ARGB; 0 keeps the default (black). */
  uint32_t sdfOutlineColor = 0xFF000000u;
  uint32_t sdfShadowColor = 0xFF000000u;
  /** Procedural border: 0 none, 1 stripes, 2 dots; then frequency, angle, strength. */
  float sdfPattern = 0.0f;
  float sdfPatternFrequency = 8.0f;
  float sdfPatternAngle = 0.0f;
  float sdfPatternStrength = 0.0f;
  /** Opacity of the glyph fill; 0 leaves the interior transparent so a background shows through. */
  float sdfFillAlpha = 1.0f;

  /**
   * A transformation applied to every colour this paint draws.
   *
   * Null for no filter, which is the ordinary case. A filter belongs to the paint rather than to the drawing call, so
   * it applies to whatever the paint is used on without the code that draws having to know.
   */
  std::shared_ptr<ColorFilter> colorFilter;

  /**
   * What colours the shape, when one flat colour is not enough.
   *
   * Null for a solid fill, which is the ordinary case. When it is set it supplies the colour at every covered pixel and
   * @c color is used only for its alpha, which is what lets a caller fade a gradient without rebuilding it -- and what
   * the SVG layer relies on when it folds `opacity` into the paint.
   *
   * A shader that reports itself invalid is ignored and the solid colour used instead: a gradient built from degenerate
   * geometry should leave the shape flat rather than invisible.
   */
  std::shared_ptr<Shader> shader;

  /**
   * @return true when this paint draws through a filter that would change something.
   *
   * A filter that is present but the identity answers false, because the question a caller is really asking is whether
   * the extra work is needed -- and a filter built conditionally, that turned out to be nothing, should not cost a
   * per-pixel pass to discover that at draw time.
   */
  [[nodiscard]] bool hasColorFilter() const;

  /** @return true when this paint draws through a usable shader rather than a flat colour. */
  [[nodiscard]] bool hasShader() const noexcept;

private:
};

} // namespace graphics

#endif //CORAL_GRAPHICS_PAINT_H
