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

#ifndef CORAL_GRAPHICS_COLOR_TABLE_H
#define CORAL_GRAPHICS_COLOR_TABLE_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace graphics {

/**
 * The per-channel lookup tables that define a table colour filter.
 *
 * ## What it is
 *
 * Four tables of 256 entries, one per channel, each mapping an 8-bit input value to an 8-bit output value. Applying one
 * means looking each channel of each pixel up in its table, which is the cheapest way to express an arbitrary
 * per-channel transfer -- a film curve, a channel swap, a threshold.
 *
 * ## Why it is a value type here
 *
 * The reference makes this a reference-counted object, because its table lives behind a bitmap and a client shares one
 * with a colour filter. Neither reason survives the translation: the four tables are four hundred and twenty-four bytes,
 * which is small enough to hold directly, and copying them costs less than the indirection that would avoid the copy.
 * So this is an ordinary value with a defaulted copy, and a filter that uses one holds its own.
 *
 * A channel's table is fixed at 256 entries because the input is always an 8-bit value. That is the point of the
 * representation; a table of another length would be a different kind of filter.
 */
struct ColorTable {
  /** The table applied to the alpha channel. */
  std::array<uint8_t, 256> alpha {};

  /** The table applied to the red channel. */
  std::array<uint8_t, 256> red {};

  /** The table applied to the green channel. */
  std::array<uint8_t, 256> green {};

  /** The table applied to the blue channel. */
  std::array<uint8_t, 256> blue {};

  /**
   * @return a table that applies @p table to all four channels.
   *
   * The common case: a curve meant for the luminance of an image, applied to each channel the same way.
   */
  [[nodiscard]] static ColorTable FromSingleChannel(const std::array<uint8_t, 256>& table) noexcept {
    return ColorTable { table, table, table, table };
  }

  /**
   * @return the identity table, which maps every value to itself.
   *
   * Built rather than stored as a constant so that it is exactly what it says: a table whose entry @c i is @c i for
   * every @c i. A hand-written table would be the same thing with more places to be wrong.
   */
  [[nodiscard]] static ColorTable Identity() noexcept {
    ColorTable table;
    for (size_t i = 0; i < 256; ++i) {
      const auto value = static_cast<uint8_t>(i);
      table.alpha[i] = value;
      table.red[i] = value;
      table.green[i] = value;
      table.blue[i] = value;
    }
    return table;
  }

  /** @return true when every channel's table is the identity, so applying this changes nothing. */
  [[nodiscard]] bool isIdentity() const noexcept {
    for (size_t i = 0; i < 256; ++i) {
      const auto value = static_cast<uint8_t>(i);
      if (alpha[i] != value || red[i] != value || green[i] != value || blue[i] != value) {
        return false;
      }
    }
    return true;
  }

  friend bool operator==(const ColorTable&, const ColorTable&) noexcept = default;
};

} // namespace graphics

#endif //CORAL_GRAPHICS_COLOR_TABLE_H
