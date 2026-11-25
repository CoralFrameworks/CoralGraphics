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
 * @file LottieParserUnavailable.cpp
 *
 * `parseAnimation` in a build with no JSON reader.
 *
 * The real parser includes `<nlohmann/json.hpp>`, so it cannot be compiled when that library is not configured -- and
 * `CMakeLists.txt` adds it as a source only when the target exists, which is the same arrangement the
 * codec uses for WebP. This file is what is compiled instead.
 *
 * It has to exist rather than the symbol simply being absent, because `Lottie.cpp` calls `parseAnimation` and a
 * translation unit that references a function nothing defines fails to *link* -- which names the symbol rather than the
 * missing dependency, and does it at the end of a build rather than at the point where the choice was made.
 *
 * Reporting the absence is also more useful than not building: `Lottie::parse` returns nothing with a sentence
 * explaining why, so a caller sees the reason instead of a program that will not start.
 */

#include "lottie/LottieModel.h"

namespace graphics::lottie {

std::optional<Animation> parseAnimation(std::string_view, std::string* error) {
  if (error != nullptr) {
    *error = "this build has no JSON reader, so a Lottie document cannot be read. Configure with "
             "CORAL_GRAPHICS_SUPPORTS_JSON=ON and third_party/json present.";
  }
  return std::nullopt;
}

} // namespace graphics::lottie
