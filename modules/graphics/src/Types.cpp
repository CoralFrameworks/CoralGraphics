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

#include <graphics/Enums.h>

#include <array>

namespace graphics {

bool BlendModeAsCoeff(BlendMode mode, BlendModeCoeff* src, BlendModeCoeff* dst) noexcept {
  struct CoeffRec {
    BlendModeCoeff fSrc;
    BlendModeCoeff fDst;
  };

  static constexpr auto kCoeffs = std::to_array<CoeffRec>({
    // For Porter-Duff blend functions, color = src * src coeff + dst * dst coeff
    // src coeff                  dst coeff                     blend func
    // ----------------------     -----------------------       ----------
    { .fSrc = BlendModeCoeff::kZero, .fDst = BlendModeCoeff::kZero }, // clear
    { .fSrc = BlendModeCoeff::kOne, .fDst = BlendModeCoeff::kZero }, // src
    { .fSrc = BlendModeCoeff::kZero, .fDst = BlendModeCoeff::kOne }, // dst
    { .fSrc = BlendModeCoeff::kOne, .fDst = BlendModeCoeff::kISA }, // src-over
    { .fSrc = BlendModeCoeff::kIDA, .fDst = BlendModeCoeff::kOne }, // dst-over
    { .fSrc = BlendModeCoeff::kDA, .fDst = BlendModeCoeff::kZero }, // src-in
    { .fSrc = BlendModeCoeff::kZero, .fDst = BlendModeCoeff::kSA }, // dst-in
    { .fSrc = BlendModeCoeff::kIDA, .fDst = BlendModeCoeff::kZero }, // src-out
    { .fSrc = BlendModeCoeff::kZero, .fDst = BlendModeCoeff::kISA }, // dst-out
    { .fSrc = BlendModeCoeff::kDA, .fDst = BlendModeCoeff::kISA }, // src-atop
    { .fSrc = BlendModeCoeff::kIDA, .fDst = BlendModeCoeff::kSA }, // dst-atop
    { .fSrc = BlendModeCoeff::kIDA, .fDst = BlendModeCoeff::kISA }, // xor

    { .fSrc = BlendModeCoeff::kOne, .fDst = BlendModeCoeff::kOne }, // plus
    { .fSrc = BlendModeCoeff::kZero, .fDst = BlendModeCoeff::kSC }, // modulate
    { .fSrc = BlendModeCoeff::kOne, .fDst = BlendModeCoeff::kISC }, // screen
  });

  if (mode > BlendMode::kScreen) {
    return false;
  }
  if (src) {
    *src = kCoeffs[static_cast<int>(mode)].fSrc;
  }
  if (dst) {
    *dst = kCoeffs[static_cast<int>(mode)].fDst;
  }
  return true;
}

const char* toString(const BlendMode mode) noexcept {
  switch (mode) {
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
    default:
      return "Unknown";
  }
}

} // namespace graphics
