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

/**
 * @file Lottie.cpp
 *
 * The public wrapper around the document model.
 *
 * `Lottie` holds a pointer to an incomplete type for the same reason `Svg` does: the model's headers are private, and a
 * client that included them could depend on the shape of a document this library intends to be free to change.
 */

#include <lottie/Lottie.h>

#include <graphics/DecodeLimits.h>

#include "lottie/LottieModel.h"
#include "lottie/LottieRenderer.h"

#include <cmath>
#include <utility>

namespace graphics {

namespace {

/**
 * @return true when @p json nests objects/arrays deeper than @p maxDepth.
 *
 * A scan rather than a parse: the JSON library would recurse while building the document, so the depth
 * has to be refused before it is handed the text. String contents are skipped, because a brace inside a
 * label is not nesting.
 */
[[nodiscard]] bool exceedsDepth(const std::string_view json, const std::size_t maxDepth) {
  std::size_t depth = 0;
  bool inString = false;
  bool escaped = false;
  for (const char character: json) {
    if (inString) {
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        inString = false;
      }
      continue;
    }
    if (character == '"') {
      inString = true;
    } else if (character == '{' || character == '[') {
      if (++depth > maxDepth) return true;
    } else if ((character == '}' || character == ']') && depth > 0) {
      --depth;
    }
  }
  return false;
}

} // namespace

struct Lottie::Impl {
  lottie::Animation animation;
};

Lottie::Lottie() = default;
Lottie::Lottie(Lottie&&) noexcept = default;
Lottie& Lottie::operator=(Lottie&&) noexcept = default;
Lottie::~Lottie() = default;

std::optional<Lottie> Lottie::parse(const std::string_view json, std::string* error) {
  if (const std::size_t maxBytes = decodeLimits().maxBytes;
      maxBytes != 0 && json.size() > maxBytes) {
    if (error != nullptr) *error = "animation is larger than the decode limit";
    return std::nullopt;
  }
  if (const std::size_t maxDepth = decodeLimits().maxDepth; maxDepth != 0) {
    if (exceedsDepth(json, maxDepth)) {
      if (error != nullptr) *error = "animation nesting is deeper than the decode limit";
      return std::nullopt;
    }
  }
  auto parsed = lottie::parseAnimation(json, error);
  if (!parsed) return std::nullopt;

  Lottie animation;
  animation.mImpl = std::make_unique<Impl>();
  animation.mImpl->animation = std::move(*parsed);
  return animation;
}

bool Lottie::isEmpty() const noexcept { return mImpl == nullptr || mImpl->animation.layers.empty(); }

Scalar Lottie::width() const noexcept { return mImpl == nullptr ? 0 : mImpl->animation.width; }
Scalar Lottie::height() const noexcept { return mImpl == nullptr ? 0 : mImpl->animation.height; }
Scalar Lottie::frameRate() const noexcept { return mImpl == nullptr ? 0 : mImpl->animation.frameRate; }
Scalar Lottie::duration() const noexcept { return mImpl == nullptr ? 0 : mImpl->animation.duration(); }

void Lottie::render(Canvas& canvas, const Rect& bounds, const Scalar seconds) const {
  if (mImpl == nullptr) return;

  // Seconds to frames, once, here. The model counts in frames because the document does, and a caller counts in seconds
  // because that is what a clock gives. Converting at this boundary is what keeps either side from carrying the other's
  // unit around and having to remember which one it holds.
  const Scalar frame = mImpl->animation.inPoint + seconds * mImpl->animation.frameRate;
  lottie::renderFrame(mImpl->animation, canvas, bounds, frame);
}

void Lottie::render(Canvas& canvas, const Scalar seconds) const {
  if (mImpl == nullptr) return;
  const Scalar frame = mImpl->animation.inPoint + seconds * mImpl->animation.frameRate;
  lottie::renderFrame(mImpl->animation, canvas, frame);
}

} // namespace graphics
