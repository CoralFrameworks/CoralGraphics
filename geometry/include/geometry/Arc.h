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

#ifndef CORAL_GEOMETRY_ARC_H
#define CORAL_GEOMETRY_ARC_H

#include <geometry/Rect.h>
#include <geometry/Scalar.h>
#include <utils/Assert.h>

namespace graphics {

struct Arc {
  /**
   * A structure to describe the iteration through a set of angle vectors
   * in a |Trigs| structure to render the points along an arc. The start
   * and end vectors and each iteration's axis vector are all unit vectors
   * that point in the direction of the point on the circle to be emitted.
   *
   * Each vector should be rendered by multiplying it by the radius of the
   * circle, or in the case of a stroked arc, by the inner and outer radii
   * of the sides of the stroke.
   *
   * - The start vector will always be rendered first.
   * - Then each quadrant will be iterated by composing the trigs vectors
   *   with the given axis vector, iterating from the start index (inclusive)
   *   to the end index (exclusive) of the vector of |Trig| values.
   * - Finally the end vector will be rendered.
   * For example:
   *   Insert(arc_iteration.start * radius);
   *   for (size_t i = 0u; i < arc_iteration.quadrant_count; i++) {
   *     Quadrant quadrant = arc_iteration.quadrants[i];
   *     for (j = quadrant.start_index; j < quadrant.end_index; j++) {
   *       Insert(trigs[j] * quadrant.axis * radius);
   *     }
   *   }
   *   Insert(arc_iteration.end * radius);
   *
   * The rendering routine may adjust the manner/order in which those vertices
   * are inserted into the vertex buffer to optimally match the vertex triangle
   * mode it plans to use, but the description above represents the basic
   * technique to compute the points along the actual curve.
   */
  struct Iteration {
    // The axis to multiply by each |Trig| value and the half-open [start, end)
    // range of indices into the associated |Trig| vector over which to compute.
    struct Quadrant {
      Vector2 axis;
      size_t start_index = 0u;
      size_t end_index = 0u;

      [[nodiscard]] size_t getPointCount() const {
        CORAL_ASSERT(start_index < end_index);
        return end_index - start_index;
      }
    };

    // The true begin and end angles of the arc, expressed as unit direction
    // vectors.
    Vector2 start;
    Vector2 end;

    // The variable number of quadrants that have to be iterated and
    // cross-referenced with values in a |Trigs| object.
    size_t quadrant_count = 0u;

    // Normally, we have at most 5 |Quadrant| entries when an arc starts
    // and ends in the same quadrant with the start angle later in the
    // quadrant than the end angle.
    //
    // Worst case:
    // - First iteration goes from the start angle to the end of that quadrant.
    // - Then 3 full iterations for the 3 other full quarter circles.
    // - Then a last iteration that goes from the start of that quadrant to the
    //   end angle.
    //
    // However, when we have an arc that sweeps past a full circle, then we
    // can have up to 9 |Quadrant| entries. The extra quadrants are only
    // interesting in the case where the arc is stroked, and we are including
    // the center. Such an arc should look like a complete circle with an
    // additional pie sliced cut into it, but not removed. Expressing that
    // case with one continuous path may require up to 7 full quadrants and
    // 2 partial quadrants for 9 total quadrants in this degenerate stroking
    // case.
    //
    // We can also have 0 quadrants for arcs that are smaller than the
    // step size of the pixel-radius |Trigs| vector.
    Quadrant quadrants[9];

    [[nodiscard]] size_t getPointCount() const;
  };

  Arc(const Rect& bounds, Degrees start, Degrees sweep, bool includeCenter);

  /// Return the bounds of the oval in which this arc is inscribed.
  [[nodiscard]] const Rect& getOvalBounds() const { return mBounds; }

  /// Returns the center of the oval bounds.
  [[nodiscard]] Point getOvalCenter() const { return mBounds.getCenter(); }

  /// Returns the size of the oval bounds.
  [[nodiscard]] Size getOvalSize() const { return mBounds.getSize(); }

  /// Return the tight bounds of the arc taking into account its specific
  /// geometry such as the start and end angles and the center (if included).
  [[nodiscard]] Rect getTightArcBounds() const;

  [[nodiscard]] constexpr Degrees getStart() const { return mStart; }

  [[nodiscard]] constexpr Degrees getSweep() const { return mSweep; }

  [[nodiscard]] constexpr bool includeCenter() const { return mIncludeCenter; }

  [[nodiscard]] constexpr bool isPerfectCircle() const { return mBounds.isSquare(); }

  [[nodiscard]] constexpr bool isFullCircle() const { return mSweep.degrees >= 360.0f; }

  /// Return an |ArcIteration| that explains how to generate vertices for
  /// the arc with the indicated number of steps in each full quadrant.
  /// The step_count is typically chosen based on the size of the bounds
  /// and the scale at which the arc is being drawn and so the computation
  /// of the step_count requirements is left to the caller.
  ///
  /// If the sweep is more than 360 degrees then the code may simplify
  /// the iteration to a simple circle, but only if the simplify_360
  /// parameter is true.
  [[nodiscard]] Iteration computeIterations(size_t stepCount, bool simplify_360 = true) const;

private:
  Rect mBounds;
  Degrees mStart;
  Degrees mSweep;
  bool mIncludeCenter;

  static Iteration ComputeCircleArcIterations(size_t stepCount);
};

} // namespace graphics

#endif //CORAL_GEOMETRY_ARC_H
