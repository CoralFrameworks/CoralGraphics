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

#include "pathops/pathops.h"

namespace graphics::pathops {

const char* toString(const PathOp op) noexcept {
  switch (op) {
    case PathOp::Difference: return "Difference";
    case PathOp::Intersection: return "Intersection";
    case PathOp::Union: return "Union";
    case PathOp::ExclusiveOr: return "ExclusiveOr";
    case PathOp::ReverseDifference: return "ReverseDifference";
  }
  return "Unknown PathOp";
}

// `op`, `simplify` and `asWinding` are defined in `Boolean.cpp`, which is where the arrangement machinery and its
// reasoning live. Splitting them across two files would put the comments describing the algorithm away from the
// code, and two definitions of the same function is a link error waiting for the first executable that names them.

std::optional<Rect> tightBounds(const Path& path) noexcept {
  if (path.isEmpty()) return std::nullopt;

  const DRect bounds = path.tightBounds();
  // A path whose segments are all non-finite produces an inverted rectangle, which is the marker for
  // "no bounds" rather than a rectangle a caller should try to use. A path that collapsed to a single
  // point is *not* that: the point is the answer.
  if (bounds.isDegenerate()) return std::nullopt;
  return bounds.toRect();
}

// ==================================================================================================
// OpBuilder
// ==================================================================================================

void OpBuilder::add(const Path& path, const PathOp op) {
  mPaths.push_back(path);
  mOps.push_back(op);
}

void OpBuilder::reset() {
  mPaths.clear();
  mOps.clear();
}

std::optional<Path> OpBuilder::resolve() {
  if (mPaths.empty()) {
    reset();
    return std::nullopt;
  }

  // A single operand with no operation applied to it is just itself, which is worth handling because
  // it is the common case for "add one shape" and needs none of the boolean machinery.
  if (mPaths.size() == 1) {
    Path result = mPaths.front();
    reset();
    return result;
  }

  Path accumulator = mPaths.front();
  for (size_t i = 1; i < mPaths.size(); ++i) {
    const auto step = op(accumulator, mPaths[i], mOps[i]);
    if (!step) {
      reset();
      return std::nullopt;
    }
    accumulator = *step;
  }

  reset();
  return accumulator;
}

} // namespace graphics::pathops
