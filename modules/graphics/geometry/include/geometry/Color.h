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

#ifndef CORAL_GRAPHICS_COLOR_H
#define CORAL_GRAPHICS_COLOR_H

#include <geometry/Scalar.h>
#include <geometry/Vector.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <ostream>
#include <type_traits>

/**
 * Every blend mode, in declaration order, for code that needs to visit them all.
 *
 * A macro list beside the enum rather than reflection, because C++ has none and a hand-written switch is what
 * the alternative would be. `Color.cpp` asserts at compile time that this list and the enum below agree in both
 * order and length, which is what keeps the two from drifting: adding an enumerator without adding it here
 * fails the build rather than silently shortening a name table.
 */
#define CORAL_FOR_EACH_BLEND_MODE(V) \
  V(Clear)                           \
  V(Src)                             \
  V(Dst)                             \
  V(SrcOver)                         \
  V(DstOver)                         \
  V(SrcIn)                           \
  V(DstIn)                           \
  V(SrcOut)                          \
  V(DstOut)                          \
  V(SrcATop)                         \
  V(DstATop)                         \
  V(Xor)                             \
  V(Plus)                            \
  V(Modulate)                        \
  V(Screen)                          \
  V(Overlay)                         \
  V(Darken)                          \
  V(Lighten)                         \
  V(ColorDodge)                      \
  V(ColorBurn)                       \
  V(HardLight)                       \
  V(SoftLight)                       \
  V(Difference)                      \
  V(Exclusion)                       \
  V(Multiply)                        \
  V(Hue)                             \
  V(Saturation)                      \
  V(Color)                           \
  V(Luminosity)

namespace graphics {

enum class YuvColorSpace {
  kBT601LimitedRange, kBT601FullRange
};

/**
 * How a source colour combines with what is already in the destination.
 *
 * The single definition, and it lives here because `Color` is here and the two are always used together -- a
 * blend mode is meaningless without knowing what the colour means, and the reverse. `graphics/Enums.h`
 * re-exports it, so a caller that includes only the enumerations still sees it.
 *
 * ## Why the list is longer than a GPU accepts
 *
 * The first nine entries -- through @c kDstOut -- are the modes a fixed-function pipeline implements directly,
 * and they are contiguous and start at zero so the value can be handed to a graphics API unchanged. The rest are
 * the separable and non-separable blend functions from the compositing specification: a GPU reaches those
 * through a shader and a software rasterizer implements them arithmetically.
 *
 * An earlier version of this header defined only the first nine under this name while `graphics/Enums.h` defined
 * all of them, so the two could not both be visible in one translation unit -- which is exactly what broke as
 * soon as `Paint` and `Pixmap` were used together. One definition is the fix, and it has to be the longer one: a
 * mode that exists but is unimplemented can be reported as such, while a mode that is absent cannot be asked for
 * at all.
 */
enum class BlendMode : uint8_t {
  /** The nine modes a pipeline implements directly, in the order a graphics API expects them. */
  kClear = 0,  //!< r = 0
  kSrc,        //!< r = s
  kDst,        //!< r = d
  kSrcOver,    //!< r = s + (1-sa)*d
  kDstOver,    //!< r = d + (1-da)*s
  kSrcIn,      //!< r = s * da
  kDstIn,      //!< r = d * sa
  kSrcOut,     //!< r = s * (1-da)
  kDstOut,     //!< r = d * (1-sa)

  /** The last mode a fixed-function pipeline implements, and the last the rasterizer currently handles. */
  kLastPipelineMode = kDstOut,

  kSrcATop,   //!< r = s*da + d*(1-sa)
  kDstATop,   //!< r = d*sa + s*(1-da)
  kXor,       //!< r = s*(1-da) + d*(1-sa)
  kPlus,      //!< r = min(s + d, 1)
  kModulate,  //!< r = s*d

  /** The last mode that combines the channels independently of one another. */
  kLastSeparableMode = kModulate,

  kScreen,      //!< r = s + d - s*d
  kOverlay,     //!< multiply or screen, depending on the destination
  kDarken,      //!< rc = s + d - max(s*da, d*sa), ra = kSrcOver
  kLighten,     //!< rc = s + d - min(s*da, d*sa), ra = kSrcOver
  kColorDodge,  //!< brighten the destination to reflect the source
  kColorBurn,   //!< darken the destination to reflect the source
  kHardLight,   //!< multiply or screen, depending on the source
  kSoftLight,   //!< lighten or darken, depending on the source
  kDifference,  //!< rc = s + d - 2*min(s*da, d*sa), ra = kSrcOver
  kExclusion,   //!< rc = s + d - 2*(s*d), ra = kSrcOver
  kMultiply,    //!< r = s*(1-da) + d*(1-sa) + s*d

  kHue,         //!< hue of the source, saturation and luminosity of the destination
  kSaturation,  //!< saturation of the source, hue and luminosity of the destination
  kColor,       //!< hue and saturation of the source, luminosity of the destination
  kLuminosity,  //!< luminosity of the source, hue and saturation of the destination

  /** The last mode with a defined meaning, for a bounds check on a value from outside. */
  kLastMode = kLuminosity,
};

/**
 * @return the name of @p blendMode, never null.
 *
 * Kept under this name because it is what the colour header's `operator<<` calls and what existing code uses.
 * `graphics::toString(BlendMode)` is the newer spelling and both are available.
 */
[[nodiscard]] const char* BlendModeToString(BlendMode blendMode);

/**
 * A 4x5 matrix for transforming a colour's components.
 *
 * Named `ColorTransform` rather than `ColorMatrix` because `graphics/ColorSpace.h` already declares a
 * `ColorMatrix` for a different thing -- a 3x3 matrix of primaries for gamut conversion -- and two types with one
 * name in one namespace cannot be visible together. This is the one that needed to move: it is used by `Color`'s
 * own operation, while the colour-space matrix is part of a published interface.
 *   [ a, b, c, d, e,
 *     f, g, h, i, j,
 *     k, l, m, n, o,
 *     p, q, r, s, t ]
 *
 * When applied to a color [R, G, B, A], the resulting color is computed as:
 *
 *    R' = a*R + b*G + c*B + d*A + e;
 *    G' = f*R + g*G + h*B + i*A + j;
 *    B' = k*R + l*G + m*B + n*A + o;
 *    A' = p*R + q*G + r*B + s*A + t;
 *
 * That resulting color [R', G', B', A'] then has each channel clamped to the 0 to 1 range.
 */
struct ColorTransform {
  Scalar array[20];
};

/**
 *  Represents a RGBA color
 */
struct Color {
  /**
   *  The red color component (0 to 1)
   */
  Scalar red = 0.0;

  /**
   *  The green color component (0 to 1)
   */
  Scalar green = 0.0;

  /**
   *  The blue color component (0 to 1)
   */
  Scalar blue = 0.0;

  /**
   *  The alpha component of the color (0 to 1)
   */
  Scalar alpha = 0.0;

  constexpr Color() = default;

  explicit Color(const Vector4& value);

  constexpr Color(const Scalar r, Scalar g, const Scalar b, const Scalar a)
      : red(r), green(g), blue(b), alpha(a) {}

  static constexpr Color MakeRGBA8(const uint8_t r, const uint8_t g, const uint8_t b, const uint8_t a) {
    return {
        static_cast<Scalar>(r) / 255.0f, static_cast<Scalar>(g) / 255.0f,
        static_cast<Scalar>(b) / 255.0f, static_cast<Scalar>(a) / 255.0f
    };
  }

  /**
   * Convert this color to a 32-bit representation.
   */
  static uint32_t toIntColor(const Color color) {
    return (((std::lround(color.alpha * 255.0f) & 0xff) << 24) |
            ((std::lround(color.red * 255.0f) & 0xff) << 16) |
            ((std::lround(color.green * 255.0f) & 0xff) << 8) |
            ((std::lround(color.blue * 255.0f) & 0xff) << 0)) & 0xFFFFFFFF;
  }

  static constexpr Color White() { return { 1.0f, 1.0f, 1.0f, 1.0f }; }

  static constexpr Color Black() { return { 0.0f, 0.0f, 0.0f, 1.0f }; }

  static constexpr Color BlackTransparent() { return { 0.0f, 0.0f, 0.0f, 0.0f }; }

  static constexpr Color Red() { return { 1.0f, 0.0f, 0.0f, 1.0f }; }

  static constexpr Color Green() { return { 0.0f, 1.0f, 0.0f, 1.0f }; }

  static constexpr Color Blue() { return { 0.0f, 0.0f, 1.0f, 1.0f }; }

  /**
   * Return a color that is linearly interpolated between colors a
   * and b, according to the value of t.
   *
   * @param a    The lower color.
   * @param b    The upper color.
   * @param t    A value between 0.0f and 1.0f, inclusive.
   * @return     Color
   */
  static Color Lerp(const Color a, const Color b, const Scalar t) {
    return a + (b - a) * t;
  }

  [[nodiscard]] Color premultiply() const {
    return { red * alpha, green * alpha, blue * alpha, alpha };
  }

  [[nodiscard]] Color unpremultiply() const {
    if (ScalarNearlyEqual(alpha, 0.0f)) {
      return Color::BlackTransparent();
    }
    return { red / alpha, green / alpha, blue / alpha, alpha };
  }

  [[nodiscard]] Color clamp() const {
    return {
        std::clamp(red, 0.0f, 1.0f), std::clamp(green, 0.0f, 1.0f),
        std::clamp(blue, 0.0f, 1.0f), std::clamp(alpha, 0.0f, 1.0f)
    };
  }

  /**
   * Convert to R8G8B8A8 representation.
   *
   * @return constexpr std::array<uint8_t, 4>
   */
  [[nodiscard]] std::array<uint8_t, 4> toR8G8B8A8() const {
    const auto r = static_cast<uint8_t>(std::round(red * 255.0f));
    const auto g = static_cast<uint8_t>(std::round(green * 255.0f));
    const auto b = static_cast<uint8_t>(std::round(blue * 255.0f));
    const auto a = static_cast<uint8_t>(std::round(alpha * 255.0f));
    return { r, g, b, a };
  }

  /**
   * Convert to ARGB 32 bit color.
   *
   * @return constexpr uint32_t
   */
  [[nodiscard]] uint32_t toARGB() const {
    const std::array<uint8_t, 4> result = toR8G8B8A8();
    return result[3] << 24 | result[0] << 16 | result[1] << 8 | result[2];
  }

  /**
   * A color filter that transforms colors through a 4x5 color matrix.
   *
   * This filter can be used to change the saturation of pixels, convert
   * from YUV to RGB, etc.
   *
   * Each channel of the output color is clamped to the 0 to 1 range.
   */
  [[nodiscard]] Color applyColorMatrix(const ColorTransform& colorMatrix) const;

  /**
   * Convert the color from linear space to sRGB space.
   *
   * The color is assumed to be unpremultiplied. If the color is premultipled,
   * the conversion output will be incorrect.
   */
  [[nodiscard]] Color linearToSRGB() const;

  /**
   * Convert the color from sRGB space to linear space.
   *
   * The color is assumed to be unpremultiplied. If the color is premultipled,
   * the conversion output will be incorrect.
   */
  [[nodiscard]] Color sRGBToLinear() const;

  [[nodiscard]] constexpr bool isTransparent() const { return alpha == 0.0f; }

  [[nodiscard]] constexpr bool isOpaque() const { return alpha == 1.0f; }

  [[nodiscard]] std::string toString() const;

  bool operator==(const Color& c) const {
    return ScalarNearlyEqual(red, c.red) && ScalarNearlyEqual(green, c.green) &&
           ScalarNearlyEqual(blue, c.blue) && ScalarNearlyEqual(alpha, c.alpha);
  }

  Color operator+(const Color& c) const {
    return { red + c.red, green + c.green, blue + c.blue, alpha + c.alpha };
  }

  template<class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
  Color operator+(T value) const {
    const auto v = static_cast<Scalar>(value);
    return { red + v, green + v, blue + v, alpha + v };
  }

  Color operator-(const Color& c) const {
    return { red - c.red, green - c.green, blue - c.blue, alpha - c.alpha };
  }

  template<class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
  Color operator-(T value) const {
    const auto v = static_cast<Scalar>(value);
    return { red - v, green - v, blue - v, alpha - v };
  }

  Color operator*(const Color& c) const {
    return { red * c.red, green * c.green, blue * c.blue, alpha * c.alpha };
  }

  template<class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
  Color operator*(T value) const {
    const auto v = static_cast<Scalar>(value);
    return { red * v, green * v, blue * v, alpha * v };
  }

  Color operator/(const Color& c) const {
    return { red / c.red, green / c.green, blue / c.blue, alpha / c.alpha };
  }

  template<class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
  Color operator/(T value) const {
    const auto v = static_cast<Scalar>(value);
    return { red / v, green / v, blue / v, alpha / v };
  }
};

template<class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
Color operator+(T value, const Color& c) {
  return c + static_cast<Scalar>(value);
}

template<class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
Color operator-(T value, const Color& c) {
  const auto v = static_cast<Scalar>(value);
  return { v - c.red, v - c.green, v - c.blue, v - c.alpha };
}

template<class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
Color operator*(T value, const Color& c) {
  return c * static_cast<Scalar>(value);
}

template<class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
Color operator/(T value, const Color& c) {
  const auto v = static_cast<Scalar>(value);
  return { v / c.red, v / c.green, v / c.blue, v / c.alpha };
}

// A free `ColorToString` used to be declared here, and was defined nowhere -- a public header promising a symbol the
// library cannot supply. Nothing called it, so nothing failed: the declaration compiled cleanly, and a downstream
// translation unit that used it would have got a link error in code this library's own build never saw. `Color::toString()`
// above is the string conversion, and it is a member rather than a free function because it is a property of the value.
//
// Two formats exist on purpose and they are not the same: `toString()` produces `{R=1.0,G=2.0,B=3.0,A=4.0}` for a log
// line, and `operator<<` below produces `(1, 2, 3, 4)` for a stream. `test_geometry.cpp` pins both, so the difference is
// a decision on the record rather than an accident waiting to be tidied away.

static_assert(sizeof(Color) == 4 * sizeof(Scalar));

inline std::ostream& operator<<(std::ostream& out, const Color& c) {
  out << "(" << c.red << ", " << c.green << ", " << c.blue << ", " << c.alpha << ")";
  return out;
}

inline std::ostream& operator<<(std::ostream& out, const BlendMode& mode) {
  out << "BlendMode::k" << BlendModeToString(mode);
  return out;
}

} // namespace graphics

#endif //CORAL_GRAPHICS_COLOR_H
