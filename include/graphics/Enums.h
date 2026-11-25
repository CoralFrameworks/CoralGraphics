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

#ifndef CORAL_GRAPHICS_TYPES_H
#define CORAL_GRAPHICS_TYPES_H

#include <geometry/Color.h>

#include <cstdint>

namespace graphics {

/**
 * Describes how to interpret the alpha component of a pixel.
 */
enum class AlphaType : uint8_t {
  /**
   * uninitialized.
   */
  kUnknown,
  /**
   * Pixel is opaque.
   */
  kOpaque,
  /**
   * Pixel components are premultiplied by alpha.
   */
  kPremultiplied,
  /**
   * Pixel components are independent of alpha.
   */
  kUnpremultiplied,
};

/**
 * Returns true if AlphaType equals AlphaType::kOpaque.
 */
static bool AlphaTypeIsOpaque(const AlphaType at) {
  return at == AlphaType::kOpaque;
}

enum class ColorType : uint8_t {
  /**
   * Unknown or unrepresentable as a ColorType.
   */
  kUnknown = 0,
  /**
   * Single channel data (8-bit) interpreted as an alpha value. RGB are 0.
   * Bits: [A:7..0]
   */
  kAlpha_8,
  /**
   * Three channel BGR data (5 bits red, 6 bits green, 5 bits blue) packed into a LE 16-bit word.
   * NOTE: The name of this enum value does not match the standard convention for ColorType.
   * Bits: [R:15..11 G:10..5 B:4..0]
   */
  kRGB_565,
  /**
   * Four channel RGBA data (8 bits per channel) packed into a LE 32-bit word.
   * Bits: [A:31..24 B:23..16 G:15..8 R:7..0]
   */
  kRGBA_8888,
  /**
   * Four channel BGRA data (8 bits per channel) packed into a LE 32-bit word. R and B are swapped
   * relative to kRGBA_8888.
   * Bits: [A:31..24 R:23..16 G:15..8 B:7..0]
   */
  kBGRA_8888,
  /**
   * Single channel data (8-bit) interpreted as a grayscale value (e.g. replicated to RGB).
   * Bits: [G:7..0]
   */
  kGray_8,
  /**
   * Four channel RGBA data (16-bit half-float per channel) packed into a LE 64-bit word. Values
   * are assumed to be in [0.0,1.0] range, unlike kRGBA_F16.
   * Bits: [A:63..48 B:47..32 G:31..16 R:15..0]
   */
  kRGBA_F16Norm,
  /**
   * Four channel RGBA data (16-bit half-float per channel) packed into a LE 64-bit word.
   * This has extended range compared to kRGBA_F16Norm.
   * Bits: [A:63..48 B:47..32 G:31..16 R:15..0]
   */
  kRGBA_F16,
  /**
   * Four channel RGBA data (32-bit float per channel) packed into a LE 128-bit word.
   * Bits: [A:127..96 B:95..64 G:63..32 R:31..0]
   */
  kRGBA_F32,
  /**
   * Four channel RGBA data (10 bits per color, 2 bits for alpha) packed into a LE 32-bit word.
   * Bits: [A:31..30 B:29..20 G:19..10 R:9..0]
   */
  kRGBA_1010102,
  /**
   * Four channel BGRA data (10 bits per color, 2 bits for alpha) packed into a LE 32-bit word.
   * R and B are swapped relative to kRGBA_1010102.
   * Bits: [A:31..30 R:29..20 G:19..10 B:9..0]
   */
  kBGRA_1010102,
};

/**
 * The blend modes come from `geometry`, which is where `Color` lives and where the single definition is.
 *
 * Re-exported rather than defined again. Two enums with one name in one namespace cannot both be visible in a
 * translation unit, and the failure that produced was a compile error the moment a paint and a pixmap were used
 * together -- see the note on the definition. A caller including this header gets the modes without having to
 * know which header owns them.
 */
using ::graphics::BlendMode;

/**
 * For Porter-Duff SkBlendModes (those <= kLastCoeffMode), these coefficients describe the blend
 * equation used. Coefficient-based blend modes specify an equation:
 * ('dstCoeff' * dst + 'srcCoeff' * src), where the coefficient values are constants, functions of
 * the src or dst alpha, or functions of the src or dst color.
 */
enum class BlendModeCoeff : uint8_t {
  // clang-format off
  kZero, /** 0 */
  kOne,  /** 1 */
  kSC,   /** src color */
  kISC,  /** inverse src color (i.e. 1 - sc) */
  kDC,   /** dst color */
  kIDC,  /** inverse dst color (i.e. 1 - dc) */
  kSA,   /** src alpha */
  kISA,  /** inverse src alpha (i.e. 1 - sa) */
  kDA,   /** dst alpha */
  kIDA,  /** inverse dst alpha (i.e. 1 - da) */
  // clang-format on
};

/**
 * Returns true if 'mode' is a coefficient-based blend mode (<= kLastCoeffMode). If true is
 * returned, the mode's src and dst coefficient functions are set in 'src' and 'dst'.
 */
[[nodiscard]] bool BlendModeAsCoeff(BlendMode mode, BlendModeCoeff* src, BlendModeCoeff* dst) noexcept;

/**
 * Returns name of the blend mode as null-terminated C string.
 */
[[nodiscard]] const char* toString(BlendMode mode) noexcept;


enum class BlurType : uint8_t {
  // clang-format off
  kNormal = 0, //!< fuzzy inside and outside
  kSolid,      //!< solid inside, fuzzy outside
  kOuter,      //!< nothing inside, fuzzy outside
  kInner,      //!< fuzzy inside, nothing outside
  // clang-format on
};

enum class ClipOp : uint8_t {
  kDifference = 0,
  kIntersect = 1,
};

/**
 * What "inside" means where a contour crosses itself.
 *
 * The two rules agree on every simple shape and disagree only on a self-overlapping one, which is exactly
 * where a caller has to be able to say which it meant -- and why this belongs to a path rather than being
 * an argument to fill it.
 */
enum class FillType : uint8_t {
  /**
   * Inside when a ray from the point crosses the outline a non-zero number of times, counting direction.
   *
   * Overlapping contours that wind the same way stay filled, and one wound the other way cuts a hole --
   * which is what makes a letter with a counter a single path.
   */
  Winding,
  /** Inside when a ray crosses the outline an odd number of times. Any overlap cancels. */
  EvenOdd,
};

/** A set operation on two shapes. */
enum class PathOp : uint8_t {
  /** Everything in either shape. */
  Union,
  /** Only what both shapes contain. */
  Intersection,
  /** Everything in the first that the second does not contain. */
  Difference,
  /** Everything in exactly one of the two. */
  Xor,
  /** Everything in the second that the first does not contain. The mirror of @c Difference. */
  ReverseDifference,
};

/**
 * How a mask image decides what survives when it is applied to a layer.
 *
 * The two answers agree for the commonest mask -- white shapes on nothing, where white is opaque -- and disagree
 * everywhere the mask varies without varying its alpha. That is not a corner case: a mask written as a black-to-white
 * gradient has opaque stops, so its alpha is one across the whole thing and only its luminance carries the shape.
 */
enum class MaskMode : uint8_t {
  /** The mask's own alpha. */
  kAlpha = 0,
  /**
   * The mask's luminance, multiplied by its alpha.
   *
   * The coefficients are Rec. 709's, and are the ones SVG 1.1 states for a `mask` -- which defaults to this mode, so a
   * document that says nothing about `mask-type` means luminance. Using the alpha instead turns such a mask into a
   * rectangle, which is a picture that looks deliberate.
   */
  kLuminance,
};

enum class TileMode : uint8_t {
  /**
   * Replicate the edge color if the shader draws outside of its original bounds.
   */
  kClamp = 0,
  /**
   * Repeat the shader's image horizontally and vertically.
   */
  kRepeat,
  /**
   * Repeat the shader's image horizontally and vertically, alternating mirror images
   * so that adjacent images always seam.
   */
  kMirror,
  /**
   * Only draw within the original domain, return transparent-black everywhere else.
   */
  kDecal,
};

/**
 * The orientations an image can be stored in and displayed with.
 *
 * These are EXIF's orientation values, and they are not eight arbitrary cases: each is one of the four rotations
 * composed with an optional mirror, which is why the entries are named after the transformation rather than numbered.
 * A camera writes one of them into a file's metadata, and the pixels as stored are in that orientation -- so anything
 * that reads the pixels without reading this has an image that is sideways or mirrored, and no way to tell.
 *
 * `kTopLeft` is the identity and the value every other part of this library assumes when it has no metadata.
 */
enum class EncodedOrigin : uint8_t {
  kTopLeft = 1,      ///< The identity: rows top to bottom, columns left to right.
  kTopRight = 2,     ///< Mirrored horizontally.
  kBottomRight = 3,  ///< Rotated 180 degrees.
  kBottomLeft = 4,   ///< Mirrored vertically.
  kLeftTop = 5,      ///< Mirrored and rotated: the transpose.
  kRightTop = 6,     ///< Rotated 90 degrees clockwise.
  kRightBottom = 7,  ///< Mirrored and rotated: the anti-transpose.
  kLeftBottom = 8,   ///< Rotated 90 degrees counter-clockwise.

  kLast = kLeftBottom,
};

/** The number of orientations, which is what a table indexed by one is sized by. */
inline constexpr int kEncodedOriginCount = static_cast<int>(EncodedOrigin::kLast);

/**
 * The colour space a YUV image's chroma is expressed in.
 *
 * ## What this decides, and why it cannot be assumed
 *
 * YUV is not a colour space on its own: it is a way of storing three components whose meaning depends on the
 * coefficients used to derive them from RGB. Two encoders that disagree produce an image that is recognisably wrong --
 * skin tones turn green, or reds turn brown -- and the only place the disagreement is recorded is this value.
 *
 * The `kIdentity` entry is the one that is not a conversion: it means the "YUV" planes are really the colour channels
 * in another arrangement, which is what a video frame's planes are when they came from a graphics API rather than a
 * camera.
 *
 * The limited-range variants are the ones broadcast uses, where the values 16 to 235 carry the range rather than 0 to
 * 255. Treating those as full range produces an image with visibly crushed blacks and clipped whites.
 */
enum class YUVColorSpace : uint8_t {
  kIdentity,       ///< The planes are the channels as they are; no conversion.
  kJPEG,           ///< The full-range coefficients JFIF and JPEG use.
  kRec601,         ///< SD video, limited range.
  kRec709,         ///< HD video, limited range.
  kRec2020,        ///< UHD and HDR video, limited range.
  kBT2020ConstantLuminance,  ///< As above, with the constant-luminance form.

  kLast = kBT2020ConstantLuminance,
};

/** @return the name of @p fillType, never null. */
[[nodiscard]] const char* toString(FillType fillType) noexcept;

/** @return the name of @p pathOp, never null. */
[[nodiscard]] const char* toString(PathOp pathOp) noexcept;

/** @return the name of @p origin, never null. */
[[nodiscard]] const char* toString(EncodedOrigin origin) noexcept;

/** @return the name of @p space, never null. */
[[nodiscard]] const char* toString(YUVColorSpace space) noexcept;

} // namespace graphics

#endif //CORAL_GRAPHICS_TYPES_H
