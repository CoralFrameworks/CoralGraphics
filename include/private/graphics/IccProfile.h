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

#ifndef CORAL_PRIVATE_GRAPHICS_ICC_PROFILE_H
#define CORAL_PRIVATE_GRAPHICS_ICC_PROFILE_H

#include "gcms/TransferFunction.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

/**
 * @file IccProfile.h
 *
 * Reading the colour information out of an ICC profile.
 *
 * This header is private to the project: it is included by the two translation units that parse profiles and by the
 * one that serialises them, and it is not installed. See the note on `include/private` in `docs/graphics.md` for the rule.
 *
 * ## What is deliberately *not* here
 *
 * An ICC profile can describe a great deal -- named colour lists, device-link and abstract profiles, multi-process
 * elements, float and Lab encodings, and the whole `mAB`/`mBA` pipeline-tag family. None of that is read. A profile
 * is accepted only when it is the ordinary case: a matrix/TRC RGB profile whose primaries and tone curves can be
 * recovered exactly. Anything else is rejected, and rejecting is the point.
 *
 * The reason is the same one that makes `MakeFromICC` return null rather than sRGB when it cannot parse: a caller
 * cannot tell a colour space that was read from one that was guessed. A profile that is subtly misread produces
 * images that are subtly wrong, and there is nothing in the output to point at. So the parser handles what it can
 * prove and refuses the rest.
 */

namespace graphics::icc {

/**
 * The colour information a matrix/TRC profile carries.
 *
 * The matrix is the one the profile states, scaled so that the columns sum to the white point -- which is what the
 * `wtpt` tag means and what the colorant tags do not say on their own. Device RGB to PCS XYZ is the mapping a
 * profile's `rXYZ`, `gXYZ`, `bXYZ` and `wtpt` tags describe together.
 */
struct Profile {
  /** Device RGB to XYZ, column-major: column @c i is the XYZ of channel @c i, normalised to D50. */
  std::array<std::array<float, 3>, 3> toXyz{};
  /** The three channel curves, in the order the channels appear in the profile. */
  std::array<gcms::Curve, 3> curves{};
  /** The profile's colour space signature, for a caller that wants to know what it was. */
  uint32_t dataColorSpace = 0;
  /** The rendering intent the profile was written for. */
  uint32_t renderingIntent = 0;
};

/**
 * @return the profile's colour information, or `std::nullopt` when it cannot be read.
 *
 * Every rejection is deliberate: a wrong size, a version this does not understand, a colour space that is not
 * three-channel, a missing or malformed tag, or a tag type that is not one of the two curve encodings and the one
 * matrix encoding that are handled.
 */
[[nodiscard]] std::optional<Profile> parse(const void* data, size_t size) noexcept;

/**
 * @return @p profile written as an ICC profile, or `std::nullopt` when it cannot be written.
 *
 * The inverse of `parse`, and the reason the two live in separate translation units: a writer and a reader that
 * agree with each other can both be wrong about the format, so each is written to be read against the specification
 * rather than against the other. `parse(write(p))` recovers @p p, which is the property the tests check.
 *
 * A profile is refused rather than approximated when a curve cannot be expressed in either of the format's two
 * encodings. A written file is one other software will read, and a curve that means something else there is worse
 * than no file at all.
 */
[[nodiscard]] std::optional<std::vector<uint8_t>> write(const Profile& profile);

} // namespace graphics::icc

#endif //CORAL_PRIVATE_GRAPHICS_ICC_PROFILE_H
