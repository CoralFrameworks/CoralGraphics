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
 * implied. See the License for the specific language governing permissions and limitations under
 * the License.
 */

#ifndef CORAL_GRAPHICS_GCMS_GAMUT_H
#define CORAL_GRAPHICS_GCMS_GAMUT_H

#include "gcms/Numeric.h"

#include <optional>

/**
 * @file Gamut.h
 *
 * Turning a set of chromaticities into the matrix that maps a colour space's RGB into XYZ.
 *
 * ## Why this is not a constant table
 *
 * The obvious implementation is a lookup table of the gamuts that matter -- sRGB, Display P3, Rec.2020
 * -- with the matrices copied from the specification. skcms does exactly that for the profiles it ships
 * (`NamedGamut::kSRGB` and friends are 16.16 fixed-point constants).
 *
 * That works for known gamuts and fails for the one case that actually needs arithmetic: an ICC profile
 * that states its chromaticities as numbers rather than naming a standard. A display can report any
 * primaries it likes, and a table cannot cover it. Computing the matrix from xy coordinates handles
 * both, so the table becomes redundant rather than the other way round.
 */

namespace gcms {

/**
 * @return the matrix that adapts XYZ from the white point (@p wx, @p wy) to D50.
 *
 * D50 is the profile connection space: every ICC profile states its gamut relative to it, so a gamut
 * measured against its own white point has to be adapted before it can be compared with another.
 *
 * The Bradford method is used, which is what Adobe's tools use and what the ICC specification's own
 * chromatic adaptation tag (`chad`) is generally built from -- matching it matters more than any
 * argument about which adaptation is perceptually better, because a profile's `chad` tag is what the
 * pipeline will actually apply.
 */
[[nodiscard]] std::optional<Matrix3x3> adaptToXYZD50(float wx, float wy) noexcept;

/**
 * @return the matrix mapping RGB to XYZ (D50), given the primaries and white point as xy coordinates.
 *
 * This is the standard construction: build a matrix whose columns are the primary chromaticities as
 * XYZ, solve it for the white point to find the relative luminances of the three primaries, scale the
 * columns by those luminances, then adapt to D50.
 *
 * @return `std::nullopt` when a coordinate is outside [0,1] -- which no real chromaticity is -- or when
 *         the primaries are degenerate, for example three points on a line, which gives a singular
 *         matrix and no meaningful gamut.
 */
[[nodiscard]] std::optional<Matrix3x3> primariesToXYZD50(
        float rx, float ry, float gx, float gy, float bx, float by, float wx, float wy) noexcept;

} // namespace gcms

#endif // CORAL_GRAPHICS_GCMS_GAMUT_H
