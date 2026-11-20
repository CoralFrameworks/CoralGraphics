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

#ifndef CORAL_GEOMETRY_SIGMA_H
#define CORAL_GEOMETRY_SIGMA_H

#include <geometry/Scalar.h>

namespace graphics {

constexpr static float kKernelRadiusPerSigma = 1.73205080757f;

struct Radius;

/**
 * In filters that use Gaussian distributions, "sigma" is a size of
 * one standard deviation in terms of the local space pixel grid of
 * the filter input. In other words, this determines how wide the
 * distribution stretches.
 */
struct Sigma {
  Scalar sigma = 0.0;

  constexpr Sigma() = default;

  explicit constexpr Sigma(Scalar p_sigma) noexcept: sigma(p_sigma) {}

  operator Radius() const;  // NOLINT(google-explicit-constructor)
};

/**
 * For convolution filters, the "radius" is the size of the
 * convolution kernel to use on the local space pixel grid of the
 * filter input.
 * For Gaussian blur kernels, this unit has a linear
 * relationship with `Sigma`. See `kKernelRadiusPerSigma` for
 * details on how this relationship works.
 */
struct Radius {
  Scalar radius = 0.0;

  constexpr Radius() = default;

  explicit constexpr Radius(const Scalar radius) noexcept: radius(radius) {}

  operator Sigma() const;  // NOLINT(google-explicit-constructor)
};

} // namespace graphics

#endif //CORAL_GEOMETRY_SIGMA_H
