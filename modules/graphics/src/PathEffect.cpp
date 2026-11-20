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

#include <graphics/PathEffect.h>

#include <graphics/PathMeasure.h>

#include <algorithm>
#include <cmath>
#include <utility>

/**
 * @file PathEffect.cpp
 *
 * Dashing a path, and the three things that make it less simple than walking a length.
 *
 * ## A path has no parameter to walk
 *
 * The reference implements its dash by asking the path for points at distances, which is what `PathMeasure` is for.
 * What matters here is what happens at a *corner*: a dash interval that begins before a sharp turn and ends after it
 * is two straight pieces, not one chord that cuts the corner off. So a dash is walked in small steps and its corners
 * are kept, rather than sampled at the interval boundaries only -- which would round off every corner of a dashed
 * rectangle, a mistake that looks deliberate and is therefore hard to notice.
 *
 * ## The pattern has to make progress
 *
 * A pattern of all zeros advances neither the distance along the path nor the index in the pattern, which is an
 * infinite loop rather than a dash. Zero-length intervals are skipped when advancing, and a pattern that is entirely
 * zero is refused at construction. A pattern containing a zero is legal and means that piece is skipped -- a common
 * way to write "dash, then nothing, then dash".
 *
 * ## Each contour is dashed from the start of the pattern
 *
 * The phase is not carried from one contour to the next. A letter with two strokes is two contours, and dashing them
 * independently gives both strokes the pattern from their own beginning; carrying the phase over would make the
 * second stroke's pattern depend on the length of the first, which is nothing a caller could predict.
 */

namespace graphics {
namespace {

/**
 * How finely a dash walks its path.
 *
 * Chosen to match the tolerance `PathMeasure` flattens to, so that walking in these steps introduces no error beyond
 * the flattening that has already happened. Smaller would cost time for nothing; larger would start cutting the
 * corners this walk exists to keep.
 */
constexpr Scalar kWalkStep = 0.25f;

/** @return the total of @p intervals, which is the length of one repetition of the pattern. */
Scalar patternLength(const std::vector<Scalar>& intervals) noexcept {
  Scalar total = 0.0f;
  for (const Scalar interval: intervals) total += interval;
  return total;
}

/** @return true when @p value is a finite number greater than zero. */
bool isPositive(const Scalar value) noexcept { return std::isfinite(value) && value > 0.0f; }

/**
 * Appends the piece of @p measure between @p start and @p end to @p destination, keeping its corners.
 *
 * The walk is in steps of `kWalkStep`, so a corner inside the piece is visited and becomes a vertex. The first point
 * is taken exactly at @p start and the last exactly at @p end, so a dash's ends land where the pattern says rather
 * than up to a step away.
 */
void appendPiece(const PathMeasure& measure, const Scalar start, const Scalar end, Path& destination) {
  if (!(end > start)) return;

  Point point;
  if (!measure.getPosition(start, &point)) return;
  destination.moveTo(point);

  // One step past the start, then every step, then the end. Written as a loop with an explicit final point so that a
  // piece shorter than one step still gets both of its ends.
  const Scalar step = std::min(kWalkStep, end - start);
  for (Scalar at = start + step; at < end; at += step) {
    if (measure.getPosition(at, &point)) destination.lineTo(point);
  }

  if (measure.getPosition(end, &point)) destination.lineTo(point);
}

} // namespace anonymous

PathEffect::~PathEffect() = default;

PathEffect::PathEffect(PathEffect&&) noexcept = default;
PathEffect& PathEffect::operator=(PathEffect&&) noexcept = default;

// ==================================================================================================
// Building one
// ==================================================================================================

std::shared_ptr<PathEffect> PathEffect::Dash(std::vector<Scalar> intervals, const Scalar phase) {
  // A negative interval is not a length. Refused rather than clamped, because a caller that passed one has a
  // computation wrong upstream and silently taking its absolute value would hide it until the pattern looked wrong.
  for (const Scalar interval: intervals) {
    if (!std::isfinite(interval) || interval < 0.0f) return nullptr;
  }

  // Nothing but zeros advances neither the path nor the pattern.
  if (intervals.empty() || !isPositive(patternLength(intervals))) return nullptr;

  auto effect = std::shared_ptr<PathEffect>(new PathEffect());
  effect->mKind = Kind::kDash;
  effect->mIntervals = std::move(intervals);

  // The phase is folded into the pattern rather than kept as a separate offset, so that the walk has one number to
  // advance. A negative phase is folded the same way, since starting part way through a pattern is the same whether
  // the caller described it as before the start or after.
  const Scalar total = patternLength(effect->mIntervals);
  effect->mPhase = std::fmod(std::fmod(phase, total) + total, total);
  return effect;
}

std::shared_ptr<PathEffect> PathEffect::Sum(
        const std::shared_ptr<PathEffect>& first, const std::shared_ptr<PathEffect>& second) {
  if (first == nullptr) return second;
  if (second == nullptr) return first;

  // Summing with a filter that does nothing is the other filter.
  if (first->isIdentity()) return second;
  if (second->isIdentity()) return first;

  auto effect = std::shared_ptr<PathEffect>(new PathEffect());
  effect->mKind = Kind::kSum;
  effect->mOuter = first;
  effect->mInner = second;
  return effect;
}

std::shared_ptr<PathEffect> PathEffect::Compose(
        const std::shared_ptr<PathEffect>& outer, const std::shared_ptr<PathEffect>& inner) {
  if (outer == nullptr) return inner;
  if (inner == nullptr) return outer;

  // Chaining with a filter that does nothing is the other filter, which is the same law the colour filter's
  // composition satisfies -- so a caller assembling either kind can drop the no-ops the same way.
  if (outer->isIdentity()) return inner;
  if (inner->isIdentity()) return outer;

  auto effect = std::shared_ptr<PathEffect>(new PathEffect());
  effect->mKind = Kind::kCompose;
  effect->mOuter = outer;
  effect->mInner = inner;
  return effect;
}

// ==================================================================================================
// Applying one
// ==================================================================================================

Path PathEffect::applyDash(const Path& source) const {
  Path result;
  if (source.isEmpty() || mIntervals.empty()) return result;

  const auto measure = PathMeasure::create(source);
  if (measure == nullptr) return result;

  const size_t count = mIntervals.size();

  /**
   * Walks the pattern forward by @p distance, returning where it lands.
   *
   * One function rather than two copies of the same loop -- the phase has to be consumed before a contour is walked,
   * and the pattern has to be restarted at each contour, and those are the same walk from a different starting
   * distance. Zero-length intervals are stepped over without consuming anything, which is what keeps the walk from
   * stopping: a pattern of all zeros cannot reach here, because `Dash` refuses to build one.
   */
  const auto advance = [this, count](Scalar distance) {
    size_t index = 0;
    Scalar into = 0.0f;

    // Bounded by the pattern length in steps: every iteration either consumes a positive interval or moves to the
    // next interval, and the pattern's total length is positive, so the distance is consumed in finitely many steps.
    // The bound is a backstop against a bug, not a limit on legitimate patterns.
    for (int guard = 0; distance > 0.0f && guard <= static_cast<int>(count) * 4 + 4; ++guard) {
      const Scalar interval = mIntervals[index];
      if (interval > 0.0f) {
        const Scalar take = std::min(distance, interval);
        distance -= take;
        into = take;
        if (into >= interval) {
          index = (index + 1) % count;
          into = 0.0f;
        }
      } else {
        index = (index + 1) % count;
        into = 0.0f;
      }
    }
    return std::pair<size_t, Scalar> { index, into };
  };

  // Where the pattern is when the first contour begins.
  const auto [phaseIndex, phaseInto] = advance(mPhase);

  const size_t contourCount = measure->contourCount();
  for (size_t contour = 0; contour < contourCount; ++contour) {
    if (contour != 0 && !measure->nextContour()) break;

    const Scalar length = measure->length();
    if (!(length > 0.0f)) continue;

    // Every contour starts the pattern over. See the note at the top of the file.
    size_t patternIndex = phaseIndex;
    Scalar used = phaseInto;
    Scalar at = 0.0f;

    while (at < length) {
      const Scalar interval = mIntervals[patternIndex];

      if (!(interval > 0.0f)) {
        // A skipped piece of the pattern. It consumes no distance, so it must advance the index to make progress --
        // and it does, which is why a pattern containing zeros terminates.
        patternIndex = (patternIndex + 1) % count;
        used = 0.0f;
        continue;
      }

      const Scalar remaining = interval - used;
      const Scalar pieceEnd = std::min(at + remaining, length);
      if ((patternIndex % 2) == 0) appendPiece(*measure, at, pieceEnd, result);

      at = pieceEnd;
      used = 0.0f;
      patternIndex = (patternIndex + 1) % count;
    }
  }

  return result;
}

Path PathEffect::filter(const Path& source) const {
  switch (mKind) {
    case Kind::kDash:
      return applyDash(source);

    case Kind::kCompose: {
      // The outer sees only the inner's output, so an inner that produces nothing gives nothing -- which is why the
      // empty case is checked here rather than being left to the outer effect to discover.
      const Path inner = mInner->filter(source);
      if (inner.isEmpty()) return inner;
      return mOuter->filter(inner);
    }

    case Kind::kSum: {
      const Path first = mOuter->filter(source);
      const Path second = mInner->filter(source);

      // One of the two producing nothing is the common case -- a dash that skipped a stretch the other drew -- and
      // the answer is then the other one. A union with an empty path is that path, and computing it would be a
      // boolean operation over nothing.
      if (first.isEmpty()) return second;
      if (second.isEmpty()) return first;

      // A union, not two overlapping contours. A caller that drew the two results one after the other would see the
      // same pixels for an opaque fill but not for a stroke, where the internal edges would show -- so combining
      // them is what makes the sum a single shape.
      Path combined = op(first, second, PathOp::Union);
      if (!combined.isEmpty()) return combined;

      // A union can fail: two crossing lines have no area together, so there is no region to return. The answer is
      // then the first result, and both are drawn by the caller's stroke anyway -- a dash pattern applied to a line
      // is exactly this case, and losing half of it would be the wrong answer to a question that has one.
      Path both = first;
      both.addPath(second);
      return both;
    }
  }
  return {};
}

bool PathEffect::isIdentity() const {
  switch (mKind) {
    case Kind::kDash:
      // A dash leaves a path alone only when nothing is ever skipped, which needs a pattern of one drawn interval
      // and no phase. Anything else changes some path, even if it happens to leave this one alone.
      return mIntervals.size() == 1 && mPhase == 0.0f;

    case Kind::kCompose:
      return mOuter->isIdentity() && mInner->isIdentity();

    case Kind::kSum:
      return mOuter->isIdentity() && mInner->isIdentity();
  }
  return false;
}

} // namespace graphics
