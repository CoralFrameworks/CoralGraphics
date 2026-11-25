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

#ifndef CORAL_GEOMETRY_FORMULA_H
#define CORAL_GEOMETRY_FORMULA_H

#include <geometry/Point.h>
#include <geometry/Scalar.h>

namespace graphics {

class Formula {
public:
  /**
   * Returns the minimum number of evenly spaced (in the parametric sense) line segments that the cubic
   * must be chopped into in order to guarantee all lines stay within a distance of "1/intolerance"
   * pixels from the true curve.
   *
   * The scaleFactor should be the max basis XY of the current transform.
   */
  static Scalar ComputeCubicSubdivisions(Scalar scaleFactor, Point p0, Point p1, Point p2, Point p3);

  /**
   * Returns the minimum number of evenly spaced (in the parametric sense) line segments that the quadratic
   * must be chopped into in order to guarantee all lines stay within a distance of "1/intolerance"
   * pixels from the true curve.
   *
   * The scaleFactor should be the max basis XY of the current transform.
   */
  static Scalar ComputeQuadraticSubdivisions(Scalar scaleFactor, Point p0, Point p1, Point p2);

  /**
   * Returns the minimum number of evenly spaced (in the parametric sense) line segments that the conic
   * must be chopped into in order to guarantee all lines stay within a distance of "1/intolerance"
   * pixels from the true curve.
   *
   * The scaleFactor should be the max basis XY of the current transform.
   */
  static Scalar ComputeConicSubdivisions(Scalar scaleFactor, Point p0, Point p1, Point p2, Scalar w);

private:
  Formula() = default;
  ~Formula() = default;
};

} // namespace graphics

#endif //CORAL_GEOMETRY_FORMULA_H
