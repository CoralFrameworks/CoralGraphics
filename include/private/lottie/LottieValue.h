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

#ifndef CORAL_GRAPHICS_LOTTIE_LOTTIE_VALUE_H
#define CORAL_GRAPHICS_LOTTIE_LOTTIE_VALUE_H

#include <geometry/Color.h>
#include <geometry/Point.h>
#include <geometry/Scalar.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

/**
 * @file LottieValue.h
 *
 * A property that changes over time, and the easing that carries it between its keyframes.
 *
 * ## Time is in frames
 *
 * Lottie states a keyframe's time in frames, and a document states how many frames there are per second. Everything
 * here works in frames and the conversion happens once, at the edge, because the alternative -- seconds in the model and
 * frames in the file -- means every keyframe time is divided by a frame rate that the model then has to carry around to
 * answer a question about itself.
 *
 * ## The easing is a cubic in *both* axes
 *
 * A Lottie keyframe carries two control points, `o` (leaving) and `i` (arriving), each an (x, y) pair. They describe a
 * cubic Bezier curve from (0,0) to (1,1): x is the fraction of the *time* between the keyframes and y is the fraction of
 * the *change*. So evaluating one is not a matter of plugging in a number -- the time fraction has to be inverted through
 * the curve's x to find the parameter, and the parameter then gives y. That inversion is what `easeProgress` does, and
 * getting it wrong is why a hand-written Lottie player produces motion that starts and stops but never quite matches the
 * designer's curve.
 *
 * The linear case is `o = (1/3, 1/3)`, `i = (2/3, 2/3)`, which makes the curve the identity -- so it needs no special
 * case, only the right defaults.
 */

namespace graphics::lottie {

/**
 * @return the eased fraction for a segment that has progressed @p t of the way through its time, where @p outTangent
 *         and @p inTangent are the keyframes' `o` and `i` control points.
 *
 * @param t in [0, 1]. Values outside are clamped, because a caller is asking about a moment within a segment by
 *        construction and a value outside means the segment index was wrong rather than that the curve should extrapolate.
 */
[[nodiscard]] Scalar easeProgress(Scalar t, Point outTangent, Point inTangent);

/**
 * These coordinates use (x, y) for two unrelated meanings -- a point in space, and a value's own dimension. There is no
 * name for the second pair that reads better, so the interface says which is which and nothing else does.
 */

// ==================================================================================================
// Interpolation, per type
// ==================================================================================================

[[nodiscard]] inline Scalar interpolate(const Scalar a, const Scalar b, const Scalar t) { return a + (b - a) * t; }

/** Points interpolate component by component, which is what a position or an anchor does. */
[[nodiscard]] inline Point interpolate(const Point& a, const Point& b, const Scalar t) {
  return Point {interpolate(a.x, b.x, t), interpolate(a.y, b.y, t)};
}

/**
 * Colours interpolate in the values as written, with no premultiplication -- the same choice `Shader` makes for gradient
 * stops, and for the same reason: a fade to transparent should not darken on the way.
 */
[[nodiscard]] inline Color interpolate(const Color& a, const Color& b, const Scalar t) {
  return Color {interpolate(a.red, b.red, t), interpolate(a.green, b.green, t), interpolate(a.blue, b.blue, t),
                interpolate(a.alpha, b.alpha, t)};
}

/** A vector of numbers, which is how Lottie writes a scale, a position in three dimensions, or a colour's components. */
[[nodiscard]] inline std::vector<Scalar> interpolate(const std::vector<Scalar>& a, const std::vector<Scalar>& b,
                                                     const Scalar t) {
  // Mismatched lengths are a malformed document. Taking the shorter is the safe reading: the extra components of the
  // longer one have no counterpart to move towards, and inventing zeros for them would animate a value to the origin.
  const size_t count = std::min(a.size(), b.size());
  std::vector<Scalar> out(count);
  for (size_t i = 0; i < count; ++i) out[i] = interpolate(a[i], b[i], t);
  return out;
}

/** One keyframe of an animatable property. */
template <typename T>
struct Keyframe {
  /** When this keyframe applies, in frames. */
  Scalar time = 0;

  /** The value at @p time. Lottie writes this as `s`. */
  T value {};

  /**
   * The value the segment *leaving* this keyframe ends at, when the document states one.
   *
   * Lottie writes this as `e`, and newer exporters omit it -- the next keyframe's `value` is then the end. Both spellings
   * appear in the wild, and a reader that only understood one would either flatten a document or animate it to the wrong
   * place.
   */
  std::optional<T> endValue;

  /** The control point leaving this keyframe, in normalized (time, value) space. */
  Point outTangent {1.0f / 3.0f, 1.0f / 3.0f};

  /** The control point arriving at the *next* keyframe, in the same space. */
  Point inTangent {2.0f / 3.0f, 2.0f / 3.0f};

  /** When set, the value jumps at the next keyframe instead of interpolating: Lottie's `h`. */
  bool hold = false;
};

/**
 * A property, either fixed or animated.
 *
 * A document's overwhelmingly common case is a constant -- most properties of most layers never change -- so the constant
 * form is a distinct representation rather than a one-entry keyframe list. That is not only a saving: it makes
 * `isAnimated` answerable without looking at what was read, and it means a property that was never meant to move cannot
 * acquire a curve through a rounding error in the parser.
 */
template <typename T>
class Animatable {
  public:
  Animatable() = default;

  static Animatable constant(T value) {
    Animatable result;
    result.mFrames = {Keyframe<T> {0, std::move(value), std::nullopt, {0, 0}, {0, 0}, false}};
    result.mAnimated = false;
    return result;
  }

  static Animatable animated(std::vector<Keyframe<T>> frames) {
    Animatable result;
    // Sorted because Lottie does not promise keyframes are written in order, and every lookup below assumes it. A stable
    // sort keeps two keyframes at the same time in the order they were written, which is the one a document means.
    std::stable_sort(frames.begin(), frames.end(),
                     [](const Keyframe<T>& a, const Keyframe<T>& b) { return a.time < b.time; });
    result.mFrames = std::move(frames);
    result.mAnimated = result.mFrames.size() > 1;
    return result;
  }

  [[nodiscard]] bool isAnimated() const noexcept { return mAnimated; }
  [[nodiscard]] bool isEmpty() const noexcept { return mFrames.empty(); }
  [[nodiscard]] const std::vector<Keyframe<T>>& keyframes() const noexcept { return mFrames; }

  /** @return the value at @p frameTime, clamped to the keyframes' own range. */
  [[nodiscard]] T at(const Scalar frameTime) const {
    if (mFrames.empty()) return T {};
    if (mFrames.size() == 1 || !mAnimated) return mFrames.front().value;

    // Before the first and after the last: hold the end values rather than extrapolating. A document that starts its
    // keyframes after frame zero means "this is the value until then", not "carry on backwards".
    //
    // Strictly *before*, not at-or-before. Two keyframes at the same time are a jump, and the value at that instant is
    // the later one -- the same answer the search below gives for a zero-length segment. Clamping with `<=` would return
    // the earlier value at exactly that frame and the later one a moment afterwards, which is a one-frame flicker back
    // to a value the document had already left.
    if (frameTime < mFrames.front().time) return mFrames.front().value;
    if (frameTime >= mFrames.back().time) return mFrames.back().value;

    // The last keyframe at or before @frameTime. A linear scan from the start would be the obvious thing and is what a
    // document with a handful of keyframes costs nothing for; the search is here because a scrubbed playhead asks this
    // question once per property per frame, and a long rotation has hundreds of keyframes.
    size_t low = 0;
    size_t high = mFrames.size() - 1;
    while (low + 1 < high) {
      const size_t middle = (low + high) / 2;
      if (mFrames[middle].time <= frameTime) {
        low = middle;
      } else {
        high = middle;
      }
    }

    const Keyframe<T>& from = mFrames[low];
    const Keyframe<T>& to = mFrames[high];
    if (from.hold) return from.value;

    const Scalar span = to.time - from.time;
    // Two keyframes at the same time is a jump, not a division by zero. Taking the later value is what the document's
    // own ordering means.
    if (!(span > 0)) return to.value;

    const Scalar t = (frameTime - from.time) / span;
    return interpolate(from.value, endValueOf(from, to), easeProgress(t, from.outTangent, from.inTangent));
  }

  private:
  /** The value a segment leaving @p from ends at: its own `e` when stated, and the next keyframe's value otherwise. */
  [[nodiscard]] static T endValueOf(const Keyframe<T>& from, const Keyframe<T>& next) {
    return from.endValue ? *from.endValue : next.value;
  }

  std::vector<Keyframe<T>> mFrames;
  bool mAnimated = false;
};

} // namespace graphics::lottie

#endif // CORAL_GRAPHICS_LOTTIE_LOTTIE_VALUE_H
