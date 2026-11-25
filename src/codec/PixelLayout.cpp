/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License is distributed on
 * an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 */

#include <private/graphics/ImageCodecInternal.h>

#include <cstddef>

namespace graphics::codec_internal {

/**
 * Pixel-layout conversions the codecs need.
 *
 * The first one is the work the system decoder's result needs before it is a `DecodedImage`.
 *
 * It has no platform dependency and is compiled everywhere, which is the point: the Android decode path is
 * covered by a type-check against stub headers, and a type-check cannot tell BGRA from RGBA or a padded stride from a
 * tight one. Both mistakes produce a picture rather than an error -- a blue face instead of a pink one, or an image that
 * shears a little further down each row -- so the only way to be sure of them on a machine without a device is to take
 * them out of the JNI call and test them directly.
 */
std::vector<uint8_t> argb8888ToRgba(const uint8_t* source, const int32_t width, const int32_t height,
        const size_t stride) {
  std::vector<uint8_t> out;
  if (source == nullptr || width <= 0 || height <= 0) return out;

  // A stride smaller than a row cannot describe a real buffer, and using it would read the previous row's tail as this
  // row's pixels. Refused rather than clamped: the caller has told us something inconsistent about the buffer.
  const auto rowBytes = static_cast<size_t>(width) * 4u;
  if (stride < rowBytes) return out;

  out.resize(rowBytes * static_cast<size_t>(height));

  for (int32_t row = 0; row < height; ++row) {
    const uint8_t* line = source + static_cast<size_t>(row) * stride;
    uint8_t* destination = out.data() + static_cast<size_t>(row) * rowBytes;

    for (int32_t column = 0; column < width; ++column) {
      const uint8_t* pixel = line + static_cast<size_t>(column) * 4u;
      // Android's word is 0xAARRGGBB, so in memory the bytes run B, G, R, A. Reversing the first three is what makes
      // the result RGBA; the alpha is already in the right place.
      destination[0] = pixel[2];  // red
      destination[1] = pixel[1];  // green
      destination[2] = pixel[0];  // blue
      destination[3] = pixel[3];  // alpha
      destination += 4;
    }
  }

  return out;
}

} // namespace graphics::codec_internal
