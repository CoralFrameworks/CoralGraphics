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

#ifndef CORAL_GRAPHICS_COLOR_SPACE_H
#define CORAL_GRAPHICS_COLOR_SPACE_H

#include <atomic>
#include <cstdint>
#include <memory>

namespace graphics {

class Data;

/**
 * Simplified color matrix, without processing the alpha channel and offset.
 */
struct ColorMatrix {
  float values[3][3];
};

struct ColorTransFunc {
  float g, a, b, c, d, e, f;
};

struct ColorSpacePrimaries {
  float rx;
  float ry;
  float gx;
  float gy;
  float bx;
  float by;
  float wx;
  float wy;
  /**
   *  Convert primaries and a white point to a toXYZD50 matrix, the preferred color gamut
   *  representation of SkColorSpace.
   */
  bool toXYZD50(ColorMatrix* toXYZD50) const;

  friend bool operator==(const ColorSpacePrimaries& lhs, const ColorSpacePrimaries& rhs) noexcept {
    return lhs.rx == rhs.rx && lhs.ry == rhs.ry &&
           lhs.gx == rhs.gx && lhs.gy == rhs.gy &&
           lhs.bx == rhs.bx && lhs.by == rhs.by &&
           lhs.wx == rhs.wx && lhs.wy == rhs.wy;
  }
};

namespace NamedPrimaries {

/**
 * Rec. ITU-R BT.709-6, value 1.
 */
static constexpr ColorSpacePrimaries kRec709 = {
  .rx = 0.64f, .ry = 0.33f, .gx = 0.3f, .gy = 0.6f, .bx = 0.15f, .by = 0.06f, .wx = 0.3127f, .wy = 0.329f
};

/**
 * Rec. ITU-R BT.470-6 System M (historical), value 4.
 */
static constexpr ColorSpacePrimaries kRec470SystemM = {
  .rx = 0.67f, .ry = 0.33f, .gx = 0.21f, .gy = 0.71f, .bx = 0.14f, .by = 0.08f, .wx = 0.31f, .wy = 0.316f
};

/**
 * Rec. ITU-R BT.470-6 System B, G (historical), value 5.
 */
static constexpr ColorSpacePrimaries kRec470SystemBG = {
  .rx = 0.64f, .ry = 0.33f, .gx = 0.29f, .gy = 0.60f, .bx = 0.15f, .by = 0.06f, .wx = 0.3127f, .wy = 0.3290f
};

/**
 * Rec. ITU-R BT.601-7 525, value 6.
 */
static constexpr ColorSpacePrimaries kRec601 = {
  .rx = 0.630f, .ry = 0.340f, .gx = 0.310f, .gy = 0.595f, .bx = 0.155f, .by = 0.070f, .wx = 0.3127f, .wy = 0.3290f
};

/**
 * SMPTE ST 240, value 7 (functionally the same as value 6).
 */
static constexpr ColorSpacePrimaries kSMPTE_ST_240 = kRec601;

/**
 * Generic film (colour filters using Illuminant C), value 8.
 */
static constexpr ColorSpacePrimaries kGenericFilm = {
  .rx = 0.681f, .ry = 0.319f, .gx = 0.243f, .gy = 0.692f, .bx = 0.145f, .by = 0.049f, .wx = 0.310f, .wy = 0.316f
};

/**
 * Rec. ITU-R BT.2020-2, value 9.
 */
static constexpr ColorSpacePrimaries kRec2020 = {
  .rx = 0.708f, .ry = 0.292f, .gx = 0.170f, .gy = 0.797f, .bx = 0.131f, .by = 0.046f, .wx = 0.3127f, .wy = 0.3290f
};

/**
 * SMPTE ST 428-1, value 10.
 */
static constexpr ColorSpacePrimaries kSMPTE_ST_428_1 = {
  .rx = 1.f, .ry = 0.f, .gx = 0.f, .gy = 1.f, .bx = 0.f, .by = 0.f, .wx = 1.f / 3.f, .wy = 1.f / 3.f
};

/**
 * SMPTE RP 431-2, value 11.
 */
static constexpr ColorSpacePrimaries kSMPTE_RP_431_2 = {
  .rx = 0.680f, .ry = 0.320f, .gx = 0.265f, .gy = 0.690f, .bx = 0.150f, .by = 0.060f, .wx = 0.314f, .wy = 0.351f
};

/**
 * SMPTE EG 432-1, value 12.
 */
static constexpr ColorSpacePrimaries kSMPTE_EG_432_1 = {
  .rx = 0.680f, .ry = 0.320f, .gx = 0.265f, .gy = 0.690f, .bx = 0.150f, .by = 0.060f, .wx = 0.3127f, .wy = 0.3290f
};

/**
 * No corresponding industry specification identified, value 22.
 * This is sometimes referred to as EBU 3213-E, but that document doesn't
 * specify these values.
 */
static constexpr ColorSpacePrimaries kITU_T_H273_Value22 = {
  .rx = 0.630f, .ry = 0.340f, .gx = 0.295f, .gy = 0.605f, .bx = 0.155f, .by = 0.077f, .wx = 0.3127f, .wy = 0.3290f
};


// Mapping between names of color primaries and the number of the corresponding
// row in ITU-T H.273, table 2.  As above, the constants are named based on the
// first specification referenced in the value's row.
enum class CicpId : uint8_t {
  // Value 0 is reserved.
  kRec709 = 1,
  // Value 2 is unspecified.
  // Value 3 is reserved.
  kRec470SystemM = 4,
  kRec470SystemBG = 5,
  kRec601 = 6,
  kSMPTE_ST_240 = 7,
  kGenericFilm = 8,
  kRec2020 = 9,
  kSMPTE_ST_428_1 = 10,
  kSMPTE_RP_431_2 = 11,
  kSMPTE_EG_432_1 = 12,
  // Values 13-21 are reserved.
  kITU_T_H273_Value22 = 22,
  // Values 23-255 are reserved.
};

/**
 * https://www.w3.org/TR/css-color-4/#predefined-prophoto-rgb
 */
static constexpr ColorSpacePrimaries kProPhotoRGB = {
  .rx = 0.7347f, .ry = 0.2653f, .gx = 0.1596f, .gy = 0.8404f, .bx = 0.0366f, .by = 0.0001f, .wx = 0.34567f,
  .wy = 0.35850f
};

} // namespace NamedPrimaries

namespace NamedTransferFn {

static constexpr ColorTransFunc kSRGB = {
  .g = 2.4f,
  .a = static_cast<float>(1 / 1.055),
  .b = static_cast<float>(0.055 / 1.055),
  .c = static_cast<float>(1 / 12.92),
  .d = 0.04045f,
  .e = 0.0f,
  .f = 0.0f
};

static constexpr ColorTransFunc k2Dot2 = {
  .g = 2.2f, .a = 1.0f, .b = 0.0f, .c = 0.0f, .d = 0.0f, .e = 0.0f, .f = 0.0f
};

static constexpr ColorTransFunc kRec2020 = {
  .g = 2.22222f, .a = 0.909672f, .b = 0.0903276f, .c = 0.222222f, .d = 0.0812429f, .e = 0, .f = 0
};

/**
 * Rec. ITU-R BT.709-6, value 1.
 */
static constexpr ColorTransFunc kRec709 = {
  .g = 2.4f, .a = 1.f, .b = 0.f, .c = 0.f, .d = 0.f, .e = 0.f, .f = 0.f
};

/**
 * Rec. ITU-R BT.470-6 System M (historical) assumed display gamma 2.2, value 4.
 */
static constexpr ColorTransFunc kRec470SystemM = {
  .g = 2.2f, .a = 1.f, .b = 0.f, .c = 0.f, .d = 0.f, .e = 0.f, .f = 0.f
};

/**
 * Rec. ITU-R BT.470-6 System B, G (historical) assumed display gamma 2.8, value 5.
 */
static constexpr ColorTransFunc kRec470SystemBG = {
  .g = 2.8f, .a = 1.f, .b = 0.f, .c = 0.f, .d = 0.f, .e = 0.f, .f = 0.f
};

/**
 * Rec. ITU-R BT.601-7, same as kRec709, value 6.
 */
static constexpr ColorTransFunc kRec601 = kRec709;

/**
 * SMPTE ST 240, value 7.
 */
static constexpr ColorTransFunc kSMPTE_ST_240 = {
  .g = 2.222222222222f, .a = 0.899626676224f, .b = 0.100373323776f, .c = 0.25f, .d = 0.091286342118f, .e = 0.f, .f = 0.f
};

/**
 * Linear, value 8
 */
static constexpr ColorTransFunc kLinear = {
  .g = 1.0f, .a = 1.0f, .b = 0.0f, .c = 0.0f, .d = 0.0f, .e = 0.0f, .f = 0.0f
};

/**
 * IEC 61966-2-4, value 11, same as Rec709 (but is explicitly extended).
 */
static constexpr ColorTransFunc kIEC61966_2_4 = kRec709;

/**
 * IEC 61966-2-1 sRGB, value 13.
 */
static constexpr ColorTransFunc kIEC61966_2_1 = kSRGB;

/**
 * Rec. ITU-R BT.2020-2 (10-bit system), value 14.
 */
static constexpr ColorTransFunc kRec2020_10bit = kRec709;

/**
 * Rec. ITU-R BT.2020-2 (12-bit system), value 15.
 */
static constexpr ColorTransFunc kRec2020_12bit = kRec709;

/**
 * Rec. ITU-R BT.2100-2 perceptual quantization (PQ) system, value 16.
 */
static constexpr ColorTransFunc kPQ = {
  .g = -5.0f, .a = 203.f, .b = 0.0f, .c = 0.0f, .d = 0.0f, .e = 0.0f, .f = 0.0f
};

/**
 * SMPTE ST 428-1, value 17.
 */
static constexpr ColorTransFunc kSMPTE_ST_428_1 = {
  .g = 2.6f, .a = 1.034080527699f, .b = 0.f, .c = 0.f, .d = 0.f, .e = 0.f, .f = 0.f
};

/**
 * SMPTE ST 428-1, value 17.
 */
static constexpr ColorTransFunc kHLG = {
  .g = -6.0f, .a = 203.f, .b = 1000.0f, .c = 1.2f, .d = 0.0f, .e = 0.0f, .f = 0.0f
};

/**
 * Mapping between transfer function names and the number of the corresponding
 * row in ITU-T H.273, table 3.  As above, the constants are named based on the
 * first specification referenced in the value's row.
 */
enum class CicpId : uint8_t {
  // Value 0 is reserved.
  kRec709 = 1,
  // Value 2 is unspecified.
  // Value 3 is reserved.
  kRec470SystemM = 4,
  kRec470SystemBG = 5,
  kRec601 = 6,
  kSMPTE_ST_240 = 7,
  kLinear = 8,
  // Value 9 is not supported by `ColorSpace::MakeCICP`.
  // Value 10 is not supported by `ColorSpace::MakeCICP`.
  kIEC61966_2_4 = 11,
  // Value 12 is not supported by `ColorSpace::MakeCICP`.
  kIEC61966_2_1 = 13,
  kSRGB = kIEC61966_2_1,
  kRec2020_10bit = 14,
  kRec2020_12bit = 15,
  kPQ = 16,
  kSMPTE_ST_428_1 = 17,
  kHLG = 18,
  // Values 19-255 are reserved.
};

/**
 * https://w3.org/TR/css-color-4/#valdef-color-prophoto-rgb
 * "The transfer curve is a gamma function with a value of 1/1.8".
 */
static constexpr ColorTransFunc kProPhotoRGB = {
  .g = 1.8f, .a = 1.0f, .b = 0.0f, .c = 0.0f, .d = 0.0f, .e = 0.0f, .f = 0.0f
};

/**
 * https://www.w3.org/TR/css-color-4/#predefined-a98-rgb
 */
static constexpr ColorTransFunc kA98RGB = k2Dot2;

} // namespace NamedTransferFn

namespace NamedGamut {

static constexpr float UINT16ToFloat(const uint16_t x) {
  return static_cast<float>(x) * 1.52587890625e-5f;
}

static constexpr ColorMatrix kSRGB = { {
  // ICC fixed-point (16.16) representation, taken from skcms. Please keep them exactly in sync.
  // 0.436065674f, 0.385147095f, 0.143066406f,
  // 0.222488403f, 0.716873169f, 0.060607910f,
  // 0.013916016f, 0.097076416f, 0.714096069f,
  { UINT16ToFloat(0x6FA2), UINT16ToFloat(0x6299), UINT16ToFloat(0x24A0) },
  { UINT16ToFloat(0x38F5), UINT16ToFloat(0xB785), UINT16ToFloat(0x0F84) },
  { UINT16ToFloat(0x0390), UINT16ToFloat(0x18DA), UINT16ToFloat(0xB6CF) },
} };

static constexpr ColorMatrix kAdobeRGB = { {
  // ICC fixed-point (16.16) repesentation of:
  // 0.60974, 0.20528, 0.14919,
  // 0.31111, 0.62567, 0.06322,
  // 0.01947, 0.06087, 0.74457,
  { UINT16ToFloat(0x9c18), UINT16ToFloat(0x348d), UINT16ToFloat(0x2631) },
  { UINT16ToFloat(0x4fa5), UINT16ToFloat(0xa02c), UINT16ToFloat(0x102f) },
  { UINT16ToFloat(0x04fc), UINT16ToFloat(0x0f95), UINT16ToFloat(0xbe9c) },
} };

static constexpr ColorMatrix kDisplayP3 = { {
  { 0.515102f, 0.291965f, 0.157153f },
  { 0.241182f, 0.692236f, 0.0665819f },
  { -0.00104941f, 0.0418818f, 0.784378f },
} };

static constexpr ColorMatrix kRec2020 = { {
  { 0.673459f, 0.165661f, 0.125100f },
  { 0.279033f, 0.675338f, 0.0456288f },
  { -0.00193139f, 0.0299794f, 0.797162f },
} };

static constexpr ColorMatrix kXYZ = { {
  { 1.0f, 0.0f, 0.0f },
  { 0.0f, 1.0f, 0.0f },
  { 0.0f, 0.0f, 1.0f },
} };

} // namespace NamedGamut

class ColorSpace : public std::enable_shared_from_this<ColorSpace> {
public:
  /**
   * Create the sRGB color space.
   */
  static std::shared_ptr<ColorSpace> MakeSRGB();

  /**
   * Colorspace with the sRGB primaries, but a linear (1.0) gamma.
   */
  static std::shared_ptr<ColorSpace> MakeSRGBLinear();

  /**
   * Returns the Display P3 color space.
   */
  static const std::shared_ptr<ColorSpace>& DisplayP3();

  /**
   * Create an ColorSpace from a transfer function and a row-major 3x3 transformation to XYZ.
   */
  static std::shared_ptr<ColorSpace> MakeRGB(const ColorTransFunc& transFunc, const ColorMatrix& toXYZ);

  /**
   * Create an ColorSpace from code points specified in Rec. ITU-T H.273. Null will be returned for
   * invalid or unsupported combination of code points.
   *
   * Parameters:
   *
   * - `colorPrimaries` identifies an entry in Rec. ITU-T H.273, Table 2.
   * - `transferCharacteristics` identifies an entry in Rec. ITU-T H.273, Table 3.
   *
   * `ColorSpace` only supports RGB. color spaces and therefore this function does not take a
   * `matrix_coefficients` parameter - the caller is expected to verify that `matrix_coefficients`
   * is `0`.
   *
   * Narrow range images are extremely rare - see
   * https://github.com/w3c/png/issues/312#issuecomment-2327349614. Therefore this function doesn't
   * take a `video_full_range_flag` - the caller is expected to verify that it is `1` (indicating a
   * full range image).
   */
  static std::shared_ptr<ColorSpace>
  MakeCICP(NamedPrimaries::CicpId colorPrimaries, NamedTransferFn::CicpId transCharacteristics);

  /**
   * Create an ColorSpace from a ICC data.
   */
  static std::shared_ptr<ColorSpace> MakeFromICC(const void* data, size_t size);

  /**
   *  Returns true if the color space gamma is near enough to be approximated as sRGB.
   */
  bool gammaCloseToSRGB() const;

  /**
   *  Returns true if the color space gamma is linear.
   */
  bool gammaIsLinear() const;

  /**
   *  Sets |fn| to the transfer function from this color space. Returns true if the transfer
   *  function can be represented as coefficients to the standard ICC 7-parameter equation.
   *  Returns false otherwise (eg, PQ, HLG).
   */
  bool isNumericalTransferFn(ColorTransFunc* fn) const;

  /** The curve families `ColorSpace` can carry. PQ and HLG are the two HDR ones. */
  enum class TransferFamily : uint8_t {
    /** The ICC seven-parameter form: sRGB, gamma curves, Rec.709/2020 SDR. */
    Numerical,
    /** SMPTE ST 2084 perceptual quantiser, HDR10. */
    Pq,
    /** BT.2100 hybrid log-gamma. */
    Hlg,
  };

  /** @return which family this space's curve belongs to. */
  [[nodiscard]] TransferFamily transferFamily() const noexcept {
    return mTransFunc.g == NamedTransferFn::kPQ.g   ? TransferFamily::Pq
      : mTransFunc.g == NamedTransferFn::kHLG.g      ? TransferFamily::Hlg
                                                    : TransferFamily::Numerical;
  }

  /**
   * @return the linear value for an encoded channel value, for **every** family.
   *
   * The companion to `isNumericalTransferFn`: that one tells a caller whether the seven-parameter form
   * can reproduce the curve, and this one evaluates the curve regardless. A colour-managed conversion
   * needs the second question answered for PQ and HLG content too, which is why both exist.
   */
  [[nodiscard]] float decodeTransfer(float encoded) const;

  /** @return the encoded value for a linear one, for every family. */
  [[nodiscard]] float encodeTransfer(float linear) const;

  /**
   *  Returns true and sets |toXYZD50|.
   */
  bool toXYZD50(ColorMatrix* toXYZD50) const;

  /**
   * Returns a hash of the gamut transformation to XYZ D50. Allows for fast equality checking of
   * gamuts, at the (very small) risk of collision.
   */
  uint32_t toXYZD50Hash() const {
    return mToXYZD50Hash;
  }

  /**
   * Returns a color space with the same gamut as this one, but with a linear gamma.
   */
  std::shared_ptr<ColorSpace> makeLinearGamma() const;

  /**
   *  Returns a color space with the same gamut as this one, but with the sRGB transfer
   *  function.
   */
  std::shared_ptr<ColorSpace> makeSRGBGamma() const;

  /**
   *  Returns a color space with the same transfer function as this one, but with the primary
   *  colors rotated. In other words, this produces a new color space that maps RGB to GBR
   *  (when applied to a source), and maps RGB to BRG (when applied to a destination).
   *
   *  This is used for testing, to construct color spaces that have severe and testable behavior.
   */
  std::shared_ptr<ColorSpace> makeColorSpin() const;

  /**
   * Returns true if the color space is sRGB.
   * Returns false otherwise.
   *
   * This allows a little bit of tolerance, given that we might see small numerical error in some
   * cases: converting ICC fixed point to float, converting white point to D50, rounding decisions
   * on transfer function and matrix.
   *
   * This does not consider a 2.2f exponential transfer function to be sRGB. While these functions
   * are similar (and it is sometimes useful to consider them together), this function checks for
   * logical equality.
   */
  bool isSRGB() const;

  std::shared_ptr<Data> serialize() const;

  ColorTransFunc transFunc() const;
  ColorTransFunc invTransFunc() const;
  void gamutTransformTo(const ColorSpace* dst, ColorMatrix* srcToDst) const;

  uint32_t transFuncHash() const { return mTransFuncHash; }
  uint64_t hash() const { return static_cast<uint64_t>(mTransFuncHash) << 32 | mToXYZD50Hash; }

  friend bool operator==(const ColorSpace& lhs, const ColorSpace& rhs) noexcept {
    if (lhs.hash() == rhs.hash()) {
      return true;
    }
    return false;
  }

  friend bool operator!=(const ColorSpace& lhs, const ColorSpace& rhs) noexcept {
    return !(lhs == rhs);
  }

private:
  constexpr ColorSpace(const ColorTransFunc& transFunc, const ColorMatrix& toXYZ) noexcept;

  void computeLazyDstFields() const;

  uint32_t mTransFuncHash;
  uint32_t mToXYZD50Hash;
  ColorTransFunc mTransFunc;
  ColorMatrix mToXYZD50;

  mutable ColorTransFunc mInvTransFunc;
  mutable ColorMatrix mFromXYZD50;
  mutable std::atomic<uint8_t> mLazyDstFields{ 0 };
};

} // namespace graphics

#endif //CORAL_GRAPHICS_COLOR_SPACE_H
