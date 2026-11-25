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

#include <geometry/Color.h>

#include <array>
#include <cmath>
#include <cstdarg>
#include <functional>
#include <type_traits>
#include <vector>

namespace graphics {

#define CORAL_ASSERT_BLEND_MODE(blend_mode)                                \
  auto enum_##blend_mode = static_cast<std::underlying_type_t<BlendMode>>( \
      BlendMode::k##blend_mode);                                           \
  if (i != enum_##blend_mode) {                                            \
    return false;                                                          \
  }                                                                        \
  ++i;


static constexpr inline bool ValidateBlendModes() {
  std::underlying_type_t<BlendMode> i = 0;
  // Ensure the order of the blend modes match.
  CORAL_FOR_EACH_BLEND_MODE(CORAL_ASSERT_BLEND_MODE)
  // Ensure the total number of blend modes match.
  if (i - 1 != static_cast<std::underlying_type_t<BlendMode>>(BlendMode::kLastMode)) {
    return false;
  }
  return true;
}

static_assert(ValidateBlendModes(), "CORAL_FOR_EACH_BLEND_MODE must match graphics::BlendMode.");


#define CORAL_BLEND_MODE_NAME_LIST(blend_mode) #blend_mode,

static constexpr const char* kBlendModeNames[] = {
    CORAL_FOR_EACH_BLEND_MODE(CORAL_BLEND_MODE_NAME_LIST)
};

const char* BlendModeToString(const BlendMode blendMode) {
  // The table is generated from the macro list, which the `static_assert` above proves agrees with the enum in
  // both order and length. Indexing it is therefore in range for every valid mode -- which is the property the
  // earlier nine-entry version of this table did not have, and it read past the end for anything beyond
  // `kDstOut` rather than reporting a problem.
  return kBlendModeNames[static_cast<std::underlying_type_t<BlendMode>>(blendMode)];
}

Color::Color(const Vector4& value)
    : red(value.x), green(value.y), blue(value.z), alpha(value.w) {}

Color Color::applyColorMatrix(const ColorTransform& colorMatrix) const {
  auto* c = colorMatrix.array;
  return Color(
      c[0] * red + c[1] * green + c[2] * blue + c[3] * alpha + c[4],
      c[5] * red + c[6] * green + c[7] * blue + c[8] * alpha + c[9],
      c[10] * red + c[11] * green + c[12] * blue + c[13] * alpha + c[14],
      c[15] * red + c[16] * green + c[17] * blue + c[18] * alpha + c[19])
      .clamp();
}

Color Color::linearToSRGB() const {
  static auto conversion = [](const Scalar component) {
    if (component <= 0.0031308) {
      return component * 12.92;
    }
    return 1.055 * pow(component, (1.0 / 2.4)) - 0.055;
  };

  return Color(conversion(red), conversion(green), conversion(blue), alpha);
}

Color Color::sRGBToLinear() const {
  static auto conversion = [](const Scalar component) {
    if (component <= 0.04045) {
      return component / 12.92;
    }
    return pow((component + 0.055) / 1.055, 2.4);
  };

  return Color(conversion(red), conversion(green), conversion(blue), alpha);
}

/**
 * @return @p format applied to @p args, or an empty string when the formatting fails.
 *
 * Written with an inline buffer and a `std::vector` for the overflow case, rather than the usual `vsnprintf`-twice
 * idiom that measures with one `va_list` and formats with a copy. That idiom needs `va_copy`, a `malloc` whose failure
 * is easy to leave unhandled, and a matching `free` on every path -- three things to get right in a function whose whole
 * job is to produce a string, in a module whose first requirement is RAII.
 *
 * It also sidesteps a trap in the measuring call. `vsnprintf(nullptr, 0, ...)` is passed a null buffer, and the standard
 * permits that to return -1, which an ordinary error check would read as "the formatting failed" rather than "there was
 * no buffer to write into". Formatting into a real buffer avoids having to reason about it.
 *
 * The output is unchanged: the same format string, the same `%.1f` conversions, the same characters.
 */
static std::string Color_printColor(const char* format, ...) {
  std::array<char, 128> inlineBuffer {};
  std::vector<char> overflowBuffer;

  va_list args;
  va_start(args, format);
  const int length = std::vsnprintf(inlineBuffer.data(), inlineBuffer.size(), format, args);
  va_end(args);

  if (length < 0) return {};

  char* buffer = inlineBuffer.data();
  if (static_cast<size_t>(length) >= inlineBuffer.size()) {
    // The first attempt returned the length it *would* have written, so the buffer is sized from that and formatted
    // again. A second `va_list` is unavoidable here, because the first was consumed by the measuring call.
    overflowBuffer.resize(static_cast<size_t>(length) + 1);
    va_start(args, format);
    std::vsnprintf(overflowBuffer.data(), overflowBuffer.size(), format, args);
    va_end(args);
    buffer = overflowBuffer.data();
  }

  return { buffer, static_cast<size_t>(length) };
}

std::string Color::toString() const {
  return Color_printColor("{R=%.1f,G=%.1f,B=%.1f,A=%.1f}", red, green, blue, alpha);
}

} // namespace graphics
