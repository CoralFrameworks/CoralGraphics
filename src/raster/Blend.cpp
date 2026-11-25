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

#include <private/graphics/Blend.h>

namespace graphics::raster {

bool isBlendModeSupported(const BlendMode blendMode) noexcept {
  // Every mode in `BlendMode` is now implemented, so this answers whether the value is a mode at all.
  //
  // It used to answer "is it source-over", because only source-over was implemented and approximating the rest would
  // have been worse than refusing them: a picture that is plausible and wrong is harder to diagnose than one that
  // refuses to draw. That reasoning was right while it was true, and it stopped being true when the formulas landed in
  // `include/private/graphics/BlendMode.h` -- so the predicate is derived from the enum's own bounds rather than
  // restated, and a mode added to `BlendMode` and not to the blend is the one case it cannot catch.
  return blendMode <= BlendMode::kLastMode;
}

const char* toString(const BlendMode blendMode) noexcept {
  switch (blendMode) {
    case BlendMode::kClear:
      return "Clear";
    case BlendMode::kSrc:
      return "Src";
    case BlendMode::kDst:
      return "Dst";
    case BlendMode::kSrcOver:
      return "SrcOver";
    case BlendMode::kDstOver:
      return "DstOver";
    case BlendMode::kSrcIn:
      return "SrcIn";
    case BlendMode::kDstIn:
      return "DstIn";
    case BlendMode::kSrcOut:
      return "SrcOut";
    case BlendMode::kDstOut:
      return "DstOut";
    case BlendMode::kSrcATop:
      return "SrcATop";
    case BlendMode::kDstATop:
      return "DstATop";
    case BlendMode::kXor:
      return "Xor";
    case BlendMode::kPlus:
      return "Plus";
    case BlendMode::kModulate:
      return "Modulate";
    case BlendMode::kScreen:
      return "Screen";
    case BlendMode::kOverlay:
      return "Overlay";
    case BlendMode::kDarken:
      return "Darken";
    case BlendMode::kLighten:
      return "Lighten";
    case BlendMode::kColorDodge:
      return "ColorDodge";
    case BlendMode::kColorBurn:
      return "ColorBurn";
    case BlendMode::kHardLight:
      return "HardLight";
    case BlendMode::kSoftLight:
      return "SoftLight";
    case BlendMode::kDifference:
      return "Difference";
    case BlendMode::kExclusion:
      return "Exclusion";
    case BlendMode::kMultiply:
      return "Multiply";
    case BlendMode::kHue:
      return "Hue";
    case BlendMode::kSaturation:
      return "Saturation";
    case BlendMode::kColor:
      return "Color";
    case BlendMode::kLuminosity:
      return "Luminosity";
  }
  return "Unknown BlendMode";
}

} // namespace graphics::raster
