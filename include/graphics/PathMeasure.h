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

#ifndef CORAL_GRAPHICS_PATH_MEASURE_H
#define CORAL_GRAPHICS_PATH_MEASURE_H

#include <graphics/Path.h>

#include <geometry/Point.h>
#include <geometry/Rect.h>

#include <memory>

namespace graphics {

/**
 * Walking along a path: how long it is, and what is at a given distance.
 *
 * ## What it is for
 *
 * Positioning something *on* a shape rather than at a coordinate. Text along a curve, an arrowhead at the end of
 * an arc, a dash pattern, an animation following an outline -- all of them need the same two answers, and neither
 * can be derived from the geometry without walking it. The distance along a curve is not a parameter: half way
 * along a cubic is at t = 0.5 only for a symmetric one, and using the parameter instead puts the label visibly in
 * the wrong place on any curve that is not.
 *
 * ## Contours, and the distance that spans them
 *
 * A path may hold several contours, and this walks them in order as though they were joined. That is what makes a
 * single distance meaningful for a whole path, and it is why @c nextContour exists: a caller treating each
 * contour separately steps through them, while one laying a dash pattern along a letter wants the running total.
 *
 * ## Cost
 *
 * The path is flattened once, when the measure is constructed, and every query afterwards is a search over that
 * polyline. Construction is therefore the expensive part and querying is cheap -- the right division for how this
 * is used, since a caller typically builds one and then asks it many times as an animation advances.
 *
 * ## Accuracy
 *
 * Every length here is the length of the flattened polyline, so its accuracy is the flattening tolerance: a
 * quarter of a unit by default. That is well below what a caller can see, and it is stated because a caller
 * measuring a very large or very small shape should pass a tolerance proportional to it -- the tolerance is an
 * absolute distance, not a fraction of the shape.
 */
class PathMeasure {
public:
  PathMeasure();
  ~PathMeasure();

  PathMeasure(const PathMeasure&);
  PathMeasure& operator=(const PathMeasure&);
  PathMeasure(PathMeasure&&) noexcept;
  PathMeasure& operator=(PathMeasure&&) noexcept;

  /**
   * @return a measure over @p path, or null when the path has no length to walk.
   *
   * An empty path, or one made only of `moveTo` verbs, has nothing to measure. A null is a better answer than a
   * measure whose every query returns zero, because a caller that forgot to build the path finds out at
   * construction rather than from a label drawn in the corner.
   */
  [[nodiscard]] static std::unique_ptr<PathMeasure> create(const Path& path, Scalar tolerance = 0.25f);

  /** @return the length of the contour currently being walked. */
  [[nodiscard]] Scalar length() const noexcept;

  /**
   * @return the total length of every contour, which is what a caller positioning across a whole path wants.
   *
   * Equal to @c length only for a single-contour path. The distinction is the thing about this class that catches
   * people out, so both are named explicitly rather than one being a hidden default.
   */
  [[nodiscard]] Scalar totalLength() const noexcept;

  /**
   * @return true when there is a point @p distance along the current contour, writing it to @p position.
   *
   * @param distance is *not* clamped. A distance past the end reports failure rather than returning the end
   *                 point, because the two mean different things to a caller laying out a dash pattern: running
   *                 off the end should stop the pattern, not pile it up at the last point.
   *
   * @param tangent when non-null and the answer is true, receives the unit direction of travel -- or zero at a
   *                cusp, where there is no single direction. Taken from the flattened polyline, so it is the
   *                direction of the chord the point lies on and is accurate to the same fraction of a pixel as
   *                everything else here.
   */
  [[nodiscard]] bool getPosition(Scalar distance, Point* position, Point* tangent = nullptr) const noexcept;

  /**
   * Moves to the next contour.
   *
   * @return false when there is none, leaving the measure on the last contour. Reporting the end this way lets a
   *         caller write `while (measure->nextContour())`, which is how every use of it reads.
   */
  bool nextContour() noexcept;

  /** @return which contour is being walked, counting from zero. */
  [[nodiscard]] size_t contourIndex() const noexcept;

  /** @return how many contours the path has, or zero when it has no length. */
  [[nodiscard]] size_t contourCount() const noexcept;

  /**
   * @return true when the contour being walked is closed, i.e. when the final edge returns to the first point.
   *
   * Part of the geometry rather than a detail: a closed contour is one edge longer than its vertices suggest, and a
   * caller treating two ends of a range as one piece -- as an inverted trim does -- has to know whether the piece it is
   * cutting comes back on itself.
   */
  [[nodiscard]] bool isClosed() const noexcept;

  /**
   * @return the cumulative range of the current contour within the whole path.
   *
   * @param start receives the distance along the path at which this contour begins.
   * @param end receives the distance at which it ends.
   *
   * This is what lets a caller map a distance along one contour onto a distance along the path, which a dash
   * pattern spanning contours needs.
   */
  void getSegmentRange(Scalar* start, Scalar* end) const noexcept;

  /**
   * @return true when any part of the current contour lies between the two distances, appending it to @p out.
   *
   * The distances are along the current contour and are clamped to it; an empty or inverted range appends nothing and
   * reports false. @p start and @p stop may be given in either order.
   *
   * @param moveTo when true the result starts its own contour, and when false it continues whatever @p out already
   *        holds. The distinction matters for a wrapped range -- the two halves of one closed contour have to join
   *        without a seam, and a `moveTo` between them is a visible break in a stroke.
   *
   * The result is a **polyline**, exact with respect to the flattened contour and therefore to the tolerance this
   * measure was built with. Skia returns the original curves because its contour measure keeps them; here the
   * flattening is the representation. The difference is under what a rasteriser shows at an ordinary scale, and it is
   * named because it is real.
   */
  bool getSegment(Scalar start, Scalar stop, Path* out, bool moveTo = true) const;

  /**
   * @return the bounding rectangle of the outline between two distances along the current contour.
   *
   * @return a rectangle with no area when the range is empty or inverted.
   *
   * Distinct from the path's own bounds: this is the bounds of the *travelled* part, which is what a caller
   * animating a stroke along a path needs in order to damage only the region that changed.
   */
  [[nodiscard]] Rect getSegmentBounds(Scalar start, Scalar end) const noexcept;

private:
  PathMeasure(const Path& path, Scalar tolerance);

  struct Impl;
  std::unique_ptr<Impl> mImpl;
};

} // namespace graphics

#endif //CORAL_GRAPHICS_PATH_MEASURE_H
