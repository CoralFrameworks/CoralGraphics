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
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CORAL_GRAPHICS_LOTTIE_LOTTIEGEOMETRY_H
#define CORAL_GRAPHICS_LOTTIE_LOTTIEGEOMETRY_H

#include "lottie/LottieModel.h"

#include <graphics/Path.h>

/**
 * @file LottieGeometry.h
 *
 * Turning a parametric shape into a path.
 *
 * Separate from the renderer because it is a unit of its own that is worth testing on its own: the shapes are described
 * by numbers -- a centre, a size, a radius, a point count -- and getting from those to a path is arithmetic that can be
 * wrong in ways a rendered pixel makes hard to name. An ellipse with the wrong Bezier constant is a slightly odd oval;
 * a star whose inner radius is applied to the wrong parity is a completely different shape, and only an assertion about
 * vertices says which.
 *
 * The algorithms are translations of Skottie's `Rectangle.cpp`, `Ellipse.cpp` and `Polystar.cpp`.
 */

namespace graphics::lottie {

/**
 * @return the path @p geometry describes at @p frame, in the shape's own coordinates.
 *
 * An empty path means the shape has no extent -- a zero size, or a point count of zero -- which the caller draws as
 * nothing rather than as something degenerate.
 */
[[nodiscard]] Path geometryPath(const Geometry& geometry, Scalar frame);

} // namespace graphics::lottie

#endif // CORAL_GRAPHICS_LOTTIE_LOTTIEGEOMETRY_H
