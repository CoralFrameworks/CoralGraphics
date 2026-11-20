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

#ifndef CORAL_GRAPHICS_COLOR_SPACE_XFORM_H
#define CORAL_GRAPHICS_COLOR_SPACE_XFORM_H

#include <graphics/ColorSpace.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace graphics {

/** Knobs for the conversion policy. */
struct ColorSpaceXformOptions {
  /**
   * The luminance that maps to SDR white when the source is HDR, in cd/m^2.
   *
   * BT.2408 puts diffuse white at 203 cd/m^2. HDR content above it is clipped rather than rolled off:
   * this class is a colour transform, and a highlight roll-off is a look, not a conversion. 203 is
   * the value a display that presents SDR content in an HDR signal uses, so an HDR still converted
   * with the default lands where a player would put it.
   */
  float sdrWhiteNits = 203.0f;
};

/**
 * Converts 8-bit RGBA pixels from one colour space to another.
 *
 * The renderer's working space is sRGB, so a bitmap whose `ImageInfo` carries another colour space --
 * a Display-P3 photo with an ICC profile, or an HDR10 still with a PQ curve -- would otherwise be
 * uploaded as if its numbers were sRGB: the picture is not *wrong* in the sense of a crash, it is
 * quietly the wrong colours, which is the failure a colour-managed pipeline exists to prevent.
 * `GpuRenderer` fetches an xform with `Make(sourceSpace, sRGBSpace)` and converts the pixels once, when
 * the placement is uploaded.
 *
 * ## What it supports, and what it refuses
 *
 * - **Sources:** the ICC seven-parameter curve (sRGB, gamma curves) and PQ. A PQ source is decoded to
 *   linear light, scaled so `sdrWhiteNits` maps to 1.0, and clipped there. HLG is refused: mapping a
 *   scene-referred HLG signal onto an SDR white needs a display-referred decision this class has no
 *   business making.
 * - **Destinations:** the seven-parameter curve only. Encoding *into* PQ is the HDR output stage's
 *   job, not a pixel conversion.
 * - **Clipping, not perceptual mapping.** A colour outside the destination gamut is clamped per
 *   channel after the matrix. That is the simplest honest policy: it is exact for every in-gamut
 *   colour and it keeps a wide-gamut image's saturated content recognisable rather than shifting the
 *   whole picture. A perceptual or relative-colorimetric intent is a later, separate policy.
 * - **No alpha conversion.** Alpha is not colour; it is copied through untouched.
 *
 * The conversion is tabulated in `Make` (256 decode entries, 4096 encode entries), so the per-pixel
 * cost is a lookup and a matrix multiply rather than a power per channel -- which is what makes it
 * affordable to convert a full-resolution photo on upload.
 *
 * The object is immutable after `Make`, so one xform can be shared across threads.
 */
class ColorSpaceXform {
public:
  /**
   * @return an xform converting @p source to @p destination, or null when the two spaces are equal
   *         (nothing to do) or the conversion is not representable (see the class comment).
   */
  [[nodiscard]] static std::unique_ptr<ColorSpaceXform> Make(const ColorSpace& source,
    const ColorSpace& destination);

  /** As above, with an explicit conversion policy. */
  [[nodiscard]] static std::unique_ptr<ColorSpaceXform> Make(const ColorSpace& source,
    const ColorSpace& destination, const ColorSpaceXformOptions& options);

  ~ColorSpaceXform() = default;

  ColorSpaceXform(const ColorSpaceXform&) = delete;
  ColorSpaceXform& operator=(const ColorSpaceXform&) = delete;

  /**
   * Converts @p pixelCount tightly packed RGBA8 pixels in place.
   *
   * The destination is the colour space `Make` was given; alpha is preserved. Passing a buffer with a
   * different layout is the caller's mistake: it converts every fourth byte as alpha and the RGB
   * triplet before it, which is why the function is documented in pixels rather than bytes.
   */
  void applyRgba8(uint8_t* pixels, size_t pixelCount) const noexcept;

private:
  ColorSpaceXform(std::array<float, 256> sourceDecode, std::array<uint8_t, 4096> destinationEncode,
    const ColorMatrix& gamutMatrix, bool hdrSource, float sdrScale) noexcept;

  std::array<float, 256> mSourceDecode;
  std::array<uint8_t, 4096> mDestinationEncode;
  ColorMatrix mGamut;
  /** True when the source decodes to HDR light, so its linear values are scaled before the matrix. */
  bool mHdrSource = false;
  /** The multiplier that takes HDR light to SDR white (10000 / sdrWhiteNits for PQ). */
  float mSdrScale = 1.0f;
};

} // namespace graphics

#endif // CORAL_GRAPHICS_COLOR_SPACE_XFORM_H
