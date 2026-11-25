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

#ifndef CORAL_GRAPHICS_SAMPLING_OPTIONS_H
#define CORAL_GRAPHICS_SAMPLING_OPTIONS_H

#include <algorithm>
#include <cstdint>

namespace graphics {

/** How a texel is chosen when a sample lands between texels. */
enum class FilterMode : uint8_t {
  /** The nearest texel. Sharp and aliased. */
  kNearest,
  /** A weighted blend of the surrounding texels. */
  kLinear,

  kLast = kLinear,
};

/** What to do about minification, where a texel covers more than one pixel. */
enum class MipmapMode : uint8_t {
  /** Sample the full-resolution image and accept the aliasing. */
  kNone,
  /** Sample the nearest prefiltered level. */
  kNearest,
  /** Blend the two nearest levels. */
  kLinear,

  kLast = kLinear,
};

/**
 * The B and C parameters of the Mitchell-Netravali cubic family.
 *
 * Each is between zero and one, and the pair selects a reconstruction filter from the family that interpolates
 * between sharpness and ringing. Two members of it are worth knowing by name; they are what the constants below are.
 *
 * See "Reconstruction Filters in Computer Graphics", Mitchell and Netravali, 1988.
 */
struct CubicResampler {
  float b = 0.0f;
  float c = 0.0f;

  /** A slightly soft filter with no ringing. The historic default for high-quality resampling. */
  [[nodiscard]] static constexpr CubicResampler Mitchell() noexcept { return { 1.0f / 3.0f, 1.0f / 3.0f }; }

  /** Sharper than Mitchell, and it rings slightly. The usual choice for photographic downscaling. */
  [[nodiscard]] static constexpr CubicResampler CatmullRom() noexcept { return { 0.0f, 1.0f / 2.0f }; }

  friend constexpr bool operator==(const CubicResampler&, const CubicResampler&) noexcept = default;
};

/**
 * How an image is sampled when it is drawn at a size other than its own.
 *
 * ## One type, four ways to sample
 *
 * The fields describe overlapping strategies rather than independent settings, and which one applies is decided by
 * them: an anisotropic maximum uses the minification filter, `useCubic` selects the cubic filter and ignores the
 * bilinear one, and otherwise the filter and mipmap modes are used together. That is why the constructors are easier
 * to trust than the fields -- each one produces a combination that means something, and a caller that fills the
 * fields in directly has to know which ones the sampler will look at.
 *
 * The implicit constructors from a filter mode, and from a cubic resampler, are deliberate: `drawImage(image, src,
 * dst, FilterMode::kLinear)` reads as what it does, and a named-parameter style would add a mention of the mipmap
 * mode to every call that does not care about it.
 *
 * ## What this does not carry
 *
 * Skia's version has `const` data members, which is why its assignment operator reconstructs the object in place with
 * placement new. That is a workaround for a decision that buys nothing here -- the fields are read by the sampler and
 * a caller can reasonably build one by assignment -- so they are ordinary members and the generated copy operations
 * are correct.
 */
struct SamplingOptions {
  /**
   * The most anisotropic samples to take along the axis of greatest compression; zero means isotropic sampling.
   *
   * A surface seen at a glancing angle compresses much further in one direction than the other, and isotropic
   * sampling has to pick a single level that is wrong for both. Anisotropic sampling takes several along the
   * direction the compression runs, which is what makes a receding floor look like a floor rather than a smear.
   */
  int maxAniso = 0;

  /** Whether @c cubic is used instead of the filter and mipmap modes. */
  bool useCubic = false;

  /** The cubic filter's parameters, when @c useCubic is set. */
  CubicResampler cubic{};

  /** The minification filter, and the magnification filter when @c useCubic is not set. */
  FilterMode filter = FilterMode::kNearest;

  /** How mipmap levels are chosen. */
  MipmapMode mipmap = MipmapMode::kNone;

  constexpr SamplingOptions() noexcept = default;

  constexpr SamplingOptions(const FilterMode filterMode, const MipmapMode mipmapMode) noexcept
    : filter(filterMode),
      mipmap(mipmapMode) {
  }

  /** Implicit on purpose: a filter mode alone says what the caller means. */
  constexpr SamplingOptions(const FilterMode filterMode) noexcept // NOLINT(google-explicit-constructor)
    : filter(filterMode),
      mipmap(MipmapMode::kNone) {
  }

  /** Implicit on purpose, for the same reason. */
  constexpr SamplingOptions(const CubicResampler& resampler) noexcept // NOLINT(google-explicit-constructor)
    : useCubic(true),
      cubic(resampler) {
  }

  /**
   * @return options that take up to @p maxAniso samples along the axis of greatest compression.
   *
   * A maximum below one is raised to one, because zero already means "no anisotropic sampling" and a negative
   * maximum would mean the same thing while reading as though it were a quantity.
   */
  [[nodiscard]] static constexpr SamplingOptions Aniso(const int maxAniso) noexcept {
    return SamplingOptions{ std::max(maxAniso, 1) };
  }

  /** @return true when these options ask for anisotropic sampling. */
  [[nodiscard]] constexpr bool isAniso() const noexcept { return maxAniso != 0; }

  friend constexpr bool operator==(const SamplingOptions&, const SamplingOptions&) noexcept = default;

private:
  constexpr explicit SamplingOptions(const int aniso) noexcept
    : maxAniso(aniso) {
  }
};

} // namespace graphics

#endif //CORAL_GRAPHICS_SAMPLING_OPTIONS_H
