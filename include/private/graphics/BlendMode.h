/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License is distributed on
 * an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 */

#ifndef CORAL_PRIVATE_GRAPHICS_BLEND_MODE_H
#define CORAL_PRIVATE_GRAPHICS_BLEND_MODE_H

#include <geometry/Color.h>

#include <algorithm>
#include <cmath>

/**
 * @file BlendMode.h
 *
 * The blend formulas, as pure functions of unit-interval values.
 *
 * ## Why they are here and not in the rasterizer
 *
 * A blend mode is a mathematical function and nothing else: all four components in, all four out, every one between
 * zero and one. Writing them that way means each can be checked against a value worked out by hand, which is the only
 * way to be sure a formula is right -- a blend mode that is subtly wrong still produces something that looks like a
 * blend, so a test comparing two renders to each other proves nothing.
 *
 * The rasterizer then has one place that knows how to apply a function to 8-bit pixels, and this file has none that
 * knows about pixels at all.
 *
 * ## The three families
 *
 * - **Porter-Duff**, the first twelve. They say how *coverage* combines: which parts of the destination and the source
 *   survive. `kSrcOver` is the ordinary one, and the rest are the identities of compositing -- a mask (`kSrcIn`), an
 *   erase (`kDstOut`), a cut-out (`kXor`).
 * - **Separable**, through `kMultiply`. The result's colour is a function of one destination channel and the
 *   corresponding source channel, applied the same way to red, green and blue.
 * - **Non-separable**, the last four. They move between hue, saturation and luminosity, so they cannot be computed one
 *   channel at a time.
 *
 * ## Unpremultiplied, and why that matters
 *
 * The formulas are stated for unpremultiplied components, which is what `Color` holds and what this file takes.
 * Premultiplying first would change what `kMultiply` means -- it would multiply the alphas in as well -- and the
 * difference only shows where two translucent shapes meet, which is exactly where a caller is looking.
 *
 * These are the formulas the PDF specification, CSS compositing and the W3C compositing model all state, so a result
 * here can be compared against any of them.
 */

namespace graphics::raster {

/** @return @p value confined to the unit interval. A blend can leave it, since the formulas are not all bounded. */
[[nodiscard]] inline double clampUnit(const double value) noexcept {
  return std::clamp(value, 0.0, 1.0);
}

/**
 * @return one branch of hard light, which overlay also uses with its arguments swapped.
 *
 * Written once and called twice so the two modes cannot drift apart: `overlay(d, s) == hardLight(s, d)` is the whole
 * of the relationship between them, and expressing it as a call is what makes that true by construction rather than by
 * two copies of the same arithmetic agreeing today.
 */
[[nodiscard]] inline double hardLightChannel(const double base, const double blend) noexcept {
  return (blend <= 0.5) ? (2.0 * blend * base) : (1.0 - 2.0 * (1.0 - blend) * (1.0 - base));
}

// ==================================================================================================
// Porter-Duff: how coverage combines
// ==================================================================================================

/**
 * The weights the twelve compositing operators put on the source and the destination.
 *
 * Every Porter-Duff operator is a pair of coefficients, and writing them as a table rather than as a switch with
 * arithmetic in each branch is what makes the family visible: a reader can check a row against the specification
 * without reading any code.
 */
struct PorterDuffWeights {
  /** Weight on the source colour. */
  float source = 1.0f;
  /** Weight on the destination colour. */
  float destination = 1.0f;
};

/** @param sourceAlpha the source's alpha, 0 to 1. @param destinationAlpha the destination's, 0 to 1. */
[[nodiscard]] inline PorterDuffWeights porterDuffWeights(
  const BlendMode mode, const float sourceAlpha, const float destinationAlpha) noexcept {
  const float sa = sourceAlpha;
  const float da = destinationAlpha;

  switch (mode) {
    case BlendMode::kClear:
      return { .source = 0.0f, .destination = 0.0f };
    case BlendMode::kSrc:
      return { .source = 1.0f, .destination = 0.0f };
    case BlendMode::kDst:
      return { .source = 0.0f, .destination = 1.0f };
    case BlendMode::kSrcOver:
      return { .source = 1.0f, .destination = 1.0f - sa };
    case BlendMode::kDstOver:
      return { .source = 1.0f - da, .destination = 1.0f };
    case BlendMode::kSrcIn:
      return { .source = da, .destination = 0.0f };
    case BlendMode::kDstIn:
      return { .source = 0.0f, .destination = sa };
    case BlendMode::kSrcOut:
      return { .source = 1.0f - da, .destination = 0.0f };
    case BlendMode::kDstOut:
      return { .source = 0.0f, .destination = 1.0f - sa };
    case BlendMode::kSrcATop:
      return { .source = da, .destination = 1.0f - sa };
    case BlendMode::kDstATop:
      return { .source = 1.0f - da, .destination = sa };
    case BlendMode::kXor:
      return { .source = 1.0f - da, .destination = 1.0f - sa };

    // The two colour families are not compositing operators. Their coverage is source-over, which is the answer a
    // caller expects from "multiply this over that": the shapes still overlap the way they always did, and only the
    // colours combine differently.
    default:
      return { .source = 1.0f, .destination = 1.0f - sa };
  }
}

/** @return true when @p mode is one of the twelve compositing operators rather than a colour blend. */
[[nodiscard]] inline bool isPorterDuff(const BlendMode mode) noexcept {
  switch (mode) {
    case BlendMode::kClear:
    case BlendMode::kSrc:
    case BlendMode::kDst:
    case BlendMode::kSrcOver:
    case BlendMode::kDstOver:
    case BlendMode::kSrcIn:
    case BlendMode::kDstIn:
    case BlendMode::kSrcOut:
    case BlendMode::kDstOut:
    case BlendMode::kSrcATop:
    case BlendMode::kDstATop:
    case BlendMode::kXor:
      return true;
    default:
      return false;
  }
}

// ==================================================================================================
// The separable colour functions
// ==================================================================================================

/**
 * @return the blend of one destination channel with the corresponding source channel, both in the unit interval.
 *
 * This is `B` in the compositing model, applied per channel for every separable mode -- which is what "separable"
 * means: red is combined with red and never with green.
 */
[[nodiscard]] inline double separableBlendChannel(
  const BlendMode mode, const double destination, const double source) noexcept {
  const double d = destination;
  const double s = source;

  switch (mode) {
    case BlendMode::kMultiply:
      return s * d;

    case BlendMode::kScreen:
      // The dual of multiply: screening is multiplying the complements. Written as the sum rather than as
      // `1 - (1-s)(1-d)`, which is the same in exact arithmetic and loses less of the top of the range to rounding.
      return s + d - s * d;

    case BlendMode::kOverlay:
      // Multiply where the *destination* is dark, screen where it is light. That the choice is made on the destination
      // is the whole difference from hard light, and the shared function is called with the arguments the other way
      // round to say so.
      return hardLightChannel(d, s);

    case BlendMode::kHardLight:
      return hardLightChannel(s, d);

    case BlendMode::kDarken:
      return std::min(s, d);

    case BlendMode::kLighten:
      return std::max(s, d);

    case BlendMode::kColorDodge:
      // Brighten the destination by the source. The endpoint is handled first: `d / (1 - s)` divides by zero at
      // `s == 1`, where the limit is one.
      if (d <= 0.0) return 0.0;
      if (s >= 1.0) return 1.0;
      return std::min(1.0, d / (1.0 - s));

    case BlendMode::kColorBurn:
      // The dual, with the same care at the other endpoint.
      if (d >= 1.0) return 1.0;
      if (s <= 0.0) return 0.0;
      return 1.0 - std::min(1.0, (1.0 - d) / s);

    case BlendMode::kSoftLight: {
      // A gentler overlay: the destination is lightened or darkened by an amount that grows with how far the source is
      // from one half, and a source of exactly one half leaves the destination alone. The two branches are the
      // specification's; the second is what keeps the curve's slope continuous at a destination of one quarter.
      if (s <= 0.5) return d - (1.0 - 2.0 * s) * d * (1.0 - d);
      const double curve = (d <= 0.25) ? ((16.0 * d - 12.0) * d + 4.0) * d : std::sqrt(d);
      return d + (2.0 * s - 1.0) * (curve - d);
    }

    case BlendMode::kDifference:
      return std::fabs(s - d);

    case BlendMode::kExclusion:
      // Difference without the doubling: the two agree at the endpoints and differ in the middle.
      return s + d - 2.0 * s * d;

    default:
      // The Porter-Duff family does not come through here, and for those the colour function is the identity -- the
      // operators change *where* a colour applies, not what it is.
      return s;
  }
}

// ==================================================================================================
// The non-separable colour functions
// ==================================================================================================

/**
 * @return the luminosity of an unpremultiplied colour.
 *
 * The one place the weights appear. `kLuminosity` uses it directly and `setLuminosity` uses it as the target, so
 * defining it twice -- even with the same numbers -- would be a way for the two to drift.
 *
 * The coefficients are those of Rec. 601, which is what this family of blends is conventionally defined against and
 * what a caller comparing against a CSS result will be comparing with. `gcms` has the more modern Rec. 709 weights for
 * colour *management*, which is a different question: this is about how much a blend moves a colour, not about how a
 * display should reproduce it.
 */
[[nodiscard]] inline double luminosityOf(const double red, const double green, const double blue) noexcept {
  return 0.3 * red + 0.59 * green + 0.11 * blue;
}

/** @return the saturation of an unpremultiplied colour, which is its largest channel less its smallest. */
[[nodiscard]] inline double saturationOf(const double red, const double green, const double blue) noexcept {
  return std::max({ red, green, blue }) - std::min({ red, green, blue });
}

/**
 * Moves @p red, @p green and @p blue to @p luminosity, keeping their hue and saturation.
 *
 * Moving a colour's luminosity can push a channel out of range, and the specification says to bring it back by
 * *desaturating towards the grey of that luminosity* rather than by clamping. Clamping would shift the hue as well,
 * which shows as a colour cast on a saturated blend -- the kind of wrong that looks like the blend mode being
 * misimplemented when it is the gamut mapping that is.
 */
inline void setLuminosity(double& red, double& green, double& blue, const double luminosity) noexcept {
  const double delta = luminosity - luminosityOf(red, green, blue);

  double r = red + delta;
  double g = green + delta;
  double b = blue + delta;

  // Pull each channel back by moving the others, which is a step towards the grey and so changes no hue. The weights
  // are the luminosity coefficients, so the luminosity is what stays fixed while the channel moves.
  if (r < 0.0) {
    g -= r * 0.59 / 0.3;
    b -= r * 0.11 / 0.3;
    r = 0.0;
  }
  if (g < 0.0) {
    r -= g * 0.3 / 0.59;
    b -= g * 0.11 / 0.59;
    g = 0.0;
  }
  if (b < 0.0) {
    r -= b * 0.3 / 0.11;
    g -= b * 0.59 / 0.11;
    b = 0.0;
  }

  const double maximum = std::max({ r, g, b });
  if (maximum > 1.0) {
    const double scale = 1.0 / maximum;
    r *= scale;
    g *= scale;
    b *= scale;
  }

  red = r;
  green = g;
  blue = b;
}

/**
 * Moves @p red, @p green and @p blue to @p saturation, keeping their hue and luminosity.
 *
 * Done by scaling the colour about its own smallest channel, which leaves the hue alone because hue depends on the
 * *ratios* between the differences of the channels rather than on their sizes.
 */
inline void setSaturation(double& red, double& green, double& blue, const double saturation) noexcept {
  const double current = saturationOf(red, green, blue);
  if (current <= 0.0) {
    // A grey has no hue to preserve, so the only colour at this saturation and that luminosity is the grey itself.
    red = green = blue = luminosityOf(red, green, blue);
    return;
  }

  const double scale = saturation / current;
  const double minimum = std::min({ red, green, blue });

  double r = minimum + (red - minimum) * scale;
  double g = minimum + (green - minimum) * scale;
  double b = minimum + (blue - minimum) * scale;

  // The requested saturation may not be reachable at this luminosity; the specification's answer is the most
  // saturated colour that is, which is what desaturating back to the original luminosity gives.
  if (r > 1.0 || g > 1.0 || b > 1.0) setLuminosity(r, g, b, luminosityOf(red, green, blue));

  red = r;
  green = g;
  blue = b;
}

/**
 * @return the result of a non-separable blend: the source and destination moved between hue, saturation and luminosity.
 *
 * The four modes take one attribute from each colour:
 *
 * | mode | takes from the source | takes from the destination |
 * |---|---|---|
 * | `kHue` | hue | saturation, luminosity |
 * | `kSaturation` | saturation | hue, luminosity |
 * | `kColor` | hue, saturation | luminosity |
 * | `kLuminosity` | luminosity | hue, saturation |
 *
 * The order of the calls is what implements the table: setting the saturation first and then the luminosity leaves the
 * hue alone throughout, so a mode that wants the destination's hue never has it disturbed.
 */
inline void nonSeparableBlend(const BlendMode mode, double& red, double& green, double& blue,
  const double sourceRed, const double sourceGreen, const double sourceBlue) noexcept {
  switch (mode) {
    case BlendMode::kHue:
      setSaturation(red, green, blue, saturationOf(sourceRed, sourceGreen, sourceBlue));
      setLuminosity(red, green, blue, luminosityOf(red, green, blue));
      break;

    case BlendMode::kSaturation:
      setSaturation(red, green, blue, saturationOf(sourceRed, sourceGreen, sourceBlue));
      break;

    case BlendMode::kColor:
      // The source's hue and saturation at the *destination's* luminosity: the source's colour, at the brightness the
      // destination already had.
      setLuminosity(red, green, blue, luminosityOf(sourceRed, sourceGreen, sourceBlue));
      break;

    case BlendMode::kLuminosity:
      // The source's luminosity with the *destination's* hue and saturation: the source's brightness, in the
      // destination's colour.
      setLuminosity(red, green, blue, luminosityOf(red, green, blue));
      break;

    default:
      break;
  }
}

/** @return true when @p mode is one of the four that work in hue, saturation and luminosity. */
[[nodiscard]] inline bool isNonSeparable(const BlendMode mode) noexcept {
  switch (mode) {
    case BlendMode::kHue:
    case BlendMode::kSaturation:
    case BlendMode::kColor:
    case BlendMode::kLuminosity:
      return true;
    default:
      return false;
  }
}

} // namespace graphics::raster

#endif // CORAL_PRIVATE_GRAPHICS_BLEND_MODE_H
