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

#ifndef CORAL_GRAPHICS_PATH_EFFECT_H
#define CORAL_GRAPHICS_PATH_EFFECT_H

#include <graphics/Path.h>

#include <geometry/Scalar.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace graphics {

/**
 * A transformation applied to a path before it is drawn.
 *
 * ## What it is for
 *
 * Changing the *shape* of what is drawn without changing the code that draws it. A dashed border, a path drawn twice
 * at a slight offset, a corner rounded off. The effect belongs to the paint, so the same effect applies to a path, a
 * rectangle or a glyph outline, and the drawing code does not have to know which it is.
 *
 * ## How it differs from a colour filter
 *
 * A colour filter changes the value of a pixel. A path effect changes which pixels a shape covers at all, which is a
 * different stage -- it happens before rasterisation, and it can turn one contour into many or into none. So a path
 * effect is not a special case of a colour filter and neither is built on the other.
 *
 * ## What is here, and what is not
 *
 * Three of the reference's effects are implemented, chosen because each is a different *kind* of transformation
 * rather than because they are the three most used:
 *
 *  - `Dash` rewrites the path's parameterization: one contour in, many contours out.
 *  - `Sum` combines two effects by drawing both of their results, which is a boolean union of two paths.
 *  - `Compose` chains two effects, so the output of one is the input of the next.
 *
 * The corner, discrete and trim variants are compositions and parameterization of these. They are not built here
 * because each would need its own parameters and its own rounding rules, and the three above cover the shapes of the
 * problem: one to many, two to one, and one passed through another.
 *
 * ## What a filtered path loses
 *
 * A dashing of a path with curves produces a path of straight lines, because the dash boundaries are found by walking
 * a flattened copy. The error is the flattening tolerance and no worse -- the same tolerance the rasterizer already
 * flattens to -- but a caller comparing the verb counts of the input and the output should not expect them to match.
 */
class PathEffect {
public:
  ~PathEffect();

  PathEffect(const PathEffect&) = delete;
  PathEffect& operator=(const PathEffect&) = delete;
  PathEffect(PathEffect&&) noexcept;
  PathEffect& operator=(PathEffect&&) noexcept;

  /** Which transformation this effect performs. */
  enum class Kind : uint8_t {
    /** Breaks the path into alternating drawn and skipped pieces. */
    kDash,
    /** Draws the union of two effects applied to the same path. */
    kSum,
    /** Applies one effect to the result of another. */
    kCompose,
  };

  /**
   * @param intervals the lengths of the alternating drawn and skipped pieces, in caller units.
   *
   * The first interval is drawn, the second skipped, the third drawn, and so on; the list repeats for the whole path.
   * The common cases are `{8, 4}` for a dashed line and `{1, 3}` for a dotted one.
   *
   * @param phase how far into the pattern the path starts, which is what makes two abutting dashed segments line up
   *              at their join.
   *
   * @return a dashing effect, or null when @p intervals is empty or every interval is zero -- a pattern with no
   *         drawn piece would produce nothing at all, which is not an effect.
   *
   * An interval of zero is legal and means that piece of the pattern is skipped entirely. A pattern of all zeros is
   * refused because it would advance neither the path nor the pattern, which is not a dashing but a loop.
   */
  [[nodiscard]] static std::shared_ptr<PathEffect> Dash(std::vector<Scalar> intervals, Scalar phase = 0.0f);

  /**
   * @return an effect that draws everything either of @p first and @p second draws.
   *
   * The two are applied to the same input path and their results combined, so the sum of a dash and a second dash
   * with a different phase is a path drawn with both patterns. Combining is a boolean union, which is why this is
   * exact rather than an overlay: the result is a single path with no internal edges where the two overlap.
   */
  [[nodiscard]] static std::shared_ptr<PathEffect> Sum(
          const std::shared_ptr<PathEffect>& first, const std::shared_ptr<PathEffect>& second);

  /**
   * @return an effect that applies @p inner and then @p outer to the result.
   *
   * The outer effect sees only what the inner one produced, which is the difference from `Sum`: a composed dash
   * dashes an already-dashed path, while a summed one draws two dashings of the same path on top of each other.
   */
  [[nodiscard]] static std::shared_ptr<PathEffect> Compose(
          const std::shared_ptr<PathEffect>& outer, const std::shared_ptr<PathEffect>& inner);

  [[nodiscard]] Kind kind() const noexcept { return mKind; }

  /** @return the interval pattern of a dash, or an empty list for any other kind. */
  [[nodiscard]] const std::vector<Scalar>& intervals() const noexcept { return mIntervals; }

  /** @return the phase of a dash, which is zero for any other kind. */
  [[nodiscard]] Scalar phase() const noexcept { return mPhase; }

  /** @return the first operand of a sum or the outer effect of a composition, or null. */
  [[nodiscard]] const std::shared_ptr<PathEffect>& outer() const noexcept { return mOuter; }

  /** @return the second operand of a sum or the inner effect of a composition, or null. */
  [[nodiscard]] const std::shared_ptr<PathEffect>& inner() const noexcept { return mInner; }

  /**
   * @return @p source with this effect applied.
   *
   * An empty path when the effect produces nothing, which is a legitimate answer rather than a failure: a dash whose
   * pattern is longer than the path draws the whole path, and one whose pattern starts with a skipped piece longer
   * than the path draws none of it.
   *
   * The input is not modified and the result is a new path, because an effect is a function and not a mutation -- two
   * draws using the same effect on the same path must give the same answer, and a caller that kept the original must
   * still have it.
   */
  [[nodiscard]] Path filter(const Path& source) const;

  /**
   * @return true when this effect would leave every path alone.
   *
   * A caller uses it to avoid the work of filtering, which for a dash means walking the whole path. The dash with a
   * single infinite interval and the sum or composition of two identities are the cases that answer true.
   */
  [[nodiscard]] bool isIdentity() const;

private:
  PathEffect() = default;

  /** Applies a dashing to @p source, which is where all the work is. */
  [[nodiscard]] Path applyDash(const Path& source) const;

  Kind mKind = Kind::kDash;
  std::vector<Scalar> mIntervals;
  Scalar mPhase = 0.0f;
  std::shared_ptr<PathEffect> mOuter;
  std::shared_ptr<PathEffect> mInner;
};

} // namespace graphics

#endif //CORAL_GRAPHICS_PATH_EFFECT_H
