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

#ifndef CORAL_GRAPHICS_BLEND_H
#define CORAL_GRAPHICS_BLEND_H

#include <graphics/Enums.h>
#include <private/graphics/BlendMode.h>

#include <cstdint>

/**
 * @file Blend.h
 *
 * Combining a source colour with what is already in a pixel.
 *
 * ## The units this works in
 *
 * Everything here is 8-bit non-premultiplied channels and a 0-255 coverage, except the destination, which is
 * whatever the pixel format is. The conversion to and from the destination's layout belongs to `Pixmap`,
 * which already does it -- so this file never has to know about channel order or bit depth, and adding a
 * format does not mean touching the blending code.
 *
 * ## Why the coverage is a separate parameter from the alpha
 *
 * They multiply together but they are not the same thing, and conflating them is a classic source of
 * antialiasing bugs. The colour's alpha says how transparent the *paint* is; the coverage says how much of
 * this particular pixel the shape covers. A fully opaque red shape over half a pixel of the edge is neither
 * fully red nor fully transparent -- it is half red -- and both numbers are needed to say so.
 */

namespace graphics::raster {

/** @return @p value clamped to [0, 255]. */
constexpr uint32_t clampChannel(const int value) noexcept {
  if (value < 0) return 0;
  if (value > 255) return 255;
  return static_cast<uint32_t>(value);
}

/**
 * A source colour ready to composite: unpremultiplied channels plus an alpha, all 8-bit.
 *
 * Kept as a small struct rather than four loose parameters so that the per-pixel blend below takes one
 * argument, which matters because it is called once per covered pixel.
 */
struct BlendSource {
  uint32_t red = 0;
  uint32_t green = 0;
  uint32_t blue = 0;
  uint32_t alpha = 255;
};

// The source-over fast path that used to sit here -- `blendSourceOver`, and the `blendChannel` it alone called -- is
// gone. It was called by nothing: every composite goes through `blendWith`, and the two call sites that reached for the
// fast path were moved onto the general one on purpose. Its documentation nevertheless ended "A test asserts the two
// agree", and no such test existed -- so the comment described a safety net that was never built, which is worse than
// having no fast path at all, because a reader would take the agreement as checked.
//
// The cost it existed to avoid is one switch per covered pixel. If that ever matters, reintroduce the fast path and its
// agreement test together, so that the claim is true at the moment it is made.

/**
 * @return @p source composited with @p destination using @p mode, at @p coverage.
 *
 * ## The compositing model, and where each family fits
 *
 * A blend mode does two things, and every mode in `BlendMode` does both:
 *
 *  1. **Combine the colours.** Either a Porter-Duff operator, which selects between the two colours, or a colour
 *     function `B(d, s)` -- separable, applied per channel, or non-separable, applied in hue/saturation/luminosity.
 *  2. **Combine the coverage.** Always the source-over rule: the source's contribution is scaled by its alpha and the
 *     destination's by what the source does not cover.
 *
 * So the general form is `result = (1 - a) * destination + a * blendedColour`, where `a` is the coverage-scaled source
 * alpha and `blendedColour` is whatever the mode's colour combination produced. Source-over is the case where
 * `blendedColour` is the source itself, which is why it is also the fastest: the general form collapses.
 *
 * ## Why the colour terms carry the destination's alpha
 *
 * For the Porter-Duff operators the destination's contribution is scaled by its own alpha, because a transparent
 * destination has no colour to contribute however opaque the source is. That is what the `<da>` factors below are.
 * Leaving them out happens to be right for an opaque destination -- which is most tests -- and wrong for a translucent
 * one, so a test with a translucent destination is the only thing that catches it.
 *
 * ## Coverage
 *
 * The coverage of a partially covered pixel enters the same way as the source's alpha, which is exact for source-over
 * and the standard approximation for the rest. The alternative is to blend per *subpixel* sample and then average,
 * which is what a renderer with coverage masks does; with one coverage value per pixel, this is what the specification
 * prescribes.
 */
[[nodiscard]] inline uint32_t blendWith(const BlendMode mode, const BlendSource& source, const uint32_t destination,
        const uint32_t coverage) noexcept {
  // The coverage and the paint's own alpha multiply. `source.alpha * coverage` is at most 65025, and the division is by
  // 255 rather than a shift because the intermediate has to stay exact.
  const int a = (static_cast<int>(source.alpha) * static_cast<int>(coverage) + 127) / 255;

  const int da = static_cast<int>((destination >> 24) & 0xFFu);
  const int dr = static_cast<int>((destination >> 16) & 0xFFu);
  const int dg = static_cast<int>((destination >> 8) & 0xFFu);
  const int db = static_cast<int>(destination & 0xFFu);

  const int sr = static_cast<int>(source.red);
  const int sg = static_cast<int>(source.green);
  const int sb = static_cast<int>(source.blue);

  // The output alpha. Every mode here composites coverage the same way, because a colour blend does not change which
  // pixels are covered -- only what colour they are. The formula is source-over's: `a + da * (1 - a)`.
  const int outAlpha = a + (da * (255 - a) + 127) / 255;

  // A fully transparent source leaves the destination alone. Saying so early saves every branch below from having to
  // consider it, and it is the common case: most of a shape's bounding box is outside the shape.
  if (a == 0) return destination;

  // ## Where the two families part
  //
  // The first twelve modes select between the source and the destination -- they are about *coverage*, and their
  // colour is a weighted sum of two known colours. The rest compute a new colour from the two: `kPlus` and `kModulate`
  // among them, which is why the test is not "is it Porter-Duff".
  const bool selectsColours = mode <= BlendMode::kXor;

  if (selectsColours) {
    const PorterDuffWeights weights = porterDuffWeights(
            mode, static_cast<float>(a) / 255.0f, static_cast<float>(da) / 255.0f);

    // A Porter-Duff operator's weights are stated for *premultiplied* colours: `result = ws*s + wd*d`, where `s` and
    // `d` already carry their alphas. So each channel is premultiplied here, weighted, and then divided by the output
    // alpha to come back to the unpremultiplied form a `Pixmap` holds.
    //
    // Leaving that division out is right whenever the output is opaque, which is most tests and most pictures -- and
    // wrong for every translucent result, which is what a mask or an erase produces.
    // The operator's own alpha, which is *not* always the source-over value: `kClear` has both weights zero and has to
    // come out fully transparent. Deriving it from the weights makes the operator's definition the only thing that has
    // to be right -- the first version used the source-over alpha for all twelve, which is correct for the eight that
    // agree with source-over and wrong for the four that do not.
    const double sa = static_cast<double>(a) / 255.0;
    const double daf = static_cast<double>(da) / 255.0;

    // The operator's own alpha, which is *not* always the source-over value: `kClear` has both weights zero and has to
    // come out fully transparent. Deriving it from the weights makes the operator's definition the only thing that has
    // to be right -- using the source-over alpha for all twelve is correct for the eight that agree with it and wrong
    // for the four that do not.
    const double weightedAlpha = sa * static_cast<double>(weights.source) + daf * static_cast<double>(weights.destination);
    if (weightedAlpha <= 0.0) return 0;

    const int alpha255 = static_cast<int>(std::lround(
            std::min(1.0, std::max(0.0, weightedAlpha)) * 255.0));

    // Each colour is premultiplied by its alpha, weighted, summed, and divided by the result's alpha -- the last step
    // because the storage is unpremultiplied. Done in doubles: the integer form scaled the weights to 1024ths, which
    // truncated them and moved a channel by up to two units. That is not a rounding detail a caller can ignore, since
    // it is visible on a gradient.
    const auto channel = [&](const int s, const int d, const float sourceWeight, const float destinationWeight) {
      const double premultiplied = static_cast<double>(s) / 255.0 * sa * static_cast<double>(sourceWeight) +
                                   static_cast<double>(d) / 255.0 * daf * static_cast<double>(destinationWeight);
      const double unpremultiplied = premultiplied / weightedAlpha;
      return static_cast<uint32_t>(std::lround(std::min(1.0, std::max(0.0, unpremultiplied)) * 255.0));
    };

    const uint32_t cr2 = channel(sr, dr, weights.source, weights.destination);
    const uint32_t cg2 = channel(sg, dg, weights.source, weights.destination);
    const uint32_t cb2 = channel(sb, db, weights.source, weights.destination);
    return (static_cast<uint32_t>(alpha255) << 24) | (cr2 << 16) | (cg2 << 8) | cb2;
  }

  // ## The colour functions
  //
  // Both remaining families compute a colour in the unit interval and then composite it with source-over:
  //
  //     result = (1 - a) * d * da + a * blended
  //
  // The separable ones work a channel at a time; the non-separable ones move between hue, saturation and luminosity,
  // which is not a channel-at-a-time operation at all. Both are done in floats so that the compositing step below is
  // one implementation rather than two.
  // Everything below is double, which matters more than it looks: `float` cannot hold 0.3 exactly, and red's luminance
  // of 0.3f is 0.300000011920928955 -- whose product with 255 is 76.500003 and so rounds *up* to 77 where the exact
  // value rounds down to 76. One unit, and it made a correct formula look wrong.
  const double sa = static_cast<double>(a) / 255.0;
  const double daf = static_cast<double>(da) / 255.0;

  const double sr01 = static_cast<double>(sr) / 255.0;
  const double sg01 = static_cast<double>(sg) / 255.0;
  const double sb01 = static_cast<double>(sb) / 255.0;
  const double dr01 = static_cast<double>(dr) / 255.0;
  const double dg01 = static_cast<double>(dg) / 255.0;
  const double db01 = static_cast<double>(db) / 255.0;

  double cr = sr01;
  double cg = sg01;
  double cb = sb01;

  switch (mode) {
    case BlendMode::kPlus:
      // W3C `plus-lighter`: the colours add and saturate. Not a weighted sum, so it is not in the table above.
      cr = clampUnit(dr01 + sr01);
      cg = clampUnit(dg01 + sg01);
      cb = clampUnit(db01 + sb01);
      break;

    case BlendMode::kModulate:
      // A plain product of the two colours, which is the multiply a caller gets from `kMultiply` when the destination
      // is opaque -- and differs from it where it is not, which is the reason both exist.
      cr = clampUnit(dr01 * sr01);
      cg = clampUnit(dg01 * sg01);
      cb = clampUnit(db01 * sb01);
      break;

    default:
      if (isNonSeparable(mode)) {
        // The destination, moved towards the source in whichever of the three attributes the mode names.
        cr = dr01;
        cg = dg01;
        cb = db01;
        nonSeparableBlend(mode, cr, cg, cb, sr01, sg01, sb01);
      } else {
        cr = clampUnit(separableBlendChannel(mode, dr01, sr01));
        cg = clampUnit(separableBlendChannel(mode, dg01, sg01));
        cb = clampUnit(separableBlendChannel(mode, db01, sb01));
      }
      break;
  }

  if (outAlpha <= 0) return 0;

  // Source-over with the blended colour. The destination's contribution is weighted by its alpha, because a
  // transparent destination has no colour to give however opaque the source is.
  const double mixedR = (1.0 - sa) * dr01 * daf + sa * cr;
  const double mixedG = (1.0 - sa) * dg01 * daf + sa * cg;
  const double mixedB = (1.0 - sa) * db01 * daf + sa * cb;

  // And back to unpremultiplied 8-bit: the mix above produced premultiplied values, so each is divided by the output
  // alpha. This is the same division the Porter-Duff branch performs, and it is what every mode needs -- the storage
  // is unpremultiplied whatever the mode did.
  const double outA = static_cast<double>(outAlpha) / 255.0;
  const auto toByte = [&](const double value) {
    return static_cast<uint32_t>(clampChannel(static_cast<int>(std::lround(clampUnit(value / outA) * 255.0))));
  };

  return (static_cast<uint32_t>(clampChannel(outAlpha)) << 24) | (toByte(mixedR) << 16) | (toByte(mixedG) << 8) |
         toByte(mixedB);
}

/** @return true when this layer can composite with @p blendMode, rather than approximating it. */
[[nodiscard]] bool isBlendModeSupported(BlendMode blendMode) noexcept;

/** @return the name of @p blendMode, never null. */
[[nodiscard]] const char* toString(BlendMode blendMode) noexcept;

} // namespace graphics::raster

#endif // CORAL_GRAPHICS_BLEND_H
