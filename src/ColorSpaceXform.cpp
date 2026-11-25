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

#include <graphics/ColorSpaceXform.h>

#include <algorithm>
#include <cmath>

namespace graphics {

namespace {

constexpr size_t kDecodeEntries = 256;
constexpr size_t kEncodeEntries = 4096;

/** Peak luminance PQ is defined against, in cd/m^2. */
constexpr float kPqPeakNits = 10000.0f;

uint8_t toByte(const float value) noexcept {
  const float clamped = std::clamp(value, 0.0f, 1.0f);
  return static_cast<uint8_t>(std::lround(clamped * 255.0f));
}

} // namespace

ColorSpaceXform::ColorSpaceXform(std::array<float, kDecodeEntries> sourceDecode,
    std::array<uint8_t, kEncodeEntries> destinationEncode, const ColorMatrix& gamutMatrix,
    const bool hdrSource, const float sdrScale) noexcept
    : mSourceDecode(sourceDecode), mDestinationEncode(destinationEncode), mGamut(gamutMatrix),
      mHdrSource(hdrSource), mSdrScale(sdrScale) {
}

std::unique_ptr<ColorSpaceXform> ColorSpaceXform::Make(const ColorSpace& source,
    const ColorSpace& destination) {
  return Make(source, destination, ColorSpaceXformOptions{});
}

std::unique_ptr<ColorSpaceXform> ColorSpaceXform::Make(const ColorSpace& source,
    const ColorSpace& destination, const ColorSpaceXformOptions& options) {
  if (source == destination || (source.isSRGB() && destination.isSRGB())) return nullptr;
  if (!(options.sdrWhiteNits > 0.0f)) return nullptr;

  // Encoding into an HDR curve is the output stage's job; a pixel conversion writes SDR bytes.
  if (destination.transferFamily() != ColorSpace::TransferFamily::Numerical) return nullptr;

  const ColorSpace::TransferFamily sourceFamily = source.transferFamily();
  if (sourceFamily == ColorSpace::TransferFamily::Hlg) return nullptr;

  ColorMatrix gamut{};
  source.gamutTransformTo(&destination, &gamut);

  std::array<float, kDecodeEntries> sourceDecode{};
  for (size_t index = 0; index < kDecodeEntries; ++index) {
    sourceDecode[index] = source.decodeTransfer(static_cast<float>(index) / 255.0f);
  }

  std::array<uint8_t, kEncodeEntries> destinationEncode{};
  for (size_t index = 0; index < kEncodeEntries; ++index) {
    const float linear = static_cast<float>(index) / static_cast<float>(kEncodeEntries - 1);
    destinationEncode[index] = toByte(destination.encodeTransfer(linear));
  }

  const bool hdrSource = sourceFamily == ColorSpace::TransferFamily::Pq;
  const float sdrScale = hdrSource ? kPqPeakNits / options.sdrWhiteNits : 1.0f;
  return std::unique_ptr<ColorSpaceXform>(new ColorSpaceXform(sourceDecode, destinationEncode, gamut,
    hdrSource, sdrScale));
}

void ColorSpaceXform::applyRgba8(uint8_t* pixels, const size_t pixelCount) const noexcept {
  if (pixels == nullptr) return;
  const auto encode = [this](const float linear) {
    const float clamped = std::clamp(linear, 0.0f, 1.0f);
    const size_t index = static_cast<size_t>(clamped * static_cast<float>(kEncodeEntries - 1) + 0.5f);
    return mDestinationEncode[std::min(index, kEncodeEntries - 1)];
  };
  for (size_t index = 0; index < pixelCount; ++index) {
    uint8_t* pixel = pixels + index * 4;
    float linear[3] = { mSourceDecode[pixel[0]], mSourceDecode[pixel[1]], mSourceDecode[pixel[2]] };
    if (mHdrSource) {
      // PQ decodes to absolute light; `sdrWhiteNits` becomes 1.0 and everything above it clips.
      for (float& channel: linear) channel = std::min(1.0f, channel * mSdrScale);
    }
    const float converted[3] = {
      mGamut.values[0][0] * linear[0] + mGamut.values[0][1] * linear[1] + mGamut.values[0][2] * linear[2],
      mGamut.values[1][0] * linear[0] + mGamut.values[1][1] * linear[1] + mGamut.values[1][2] * linear[2],
      mGamut.values[2][0] * linear[0] + mGamut.values[2][1] * linear[1] + mGamut.values[2][2] * linear[2],
    };
    pixel[0] = encode(converted[0]);
    pixel[1] = encode(converted[1]);
    pixel[2] = encode(converted[2]);
    // Alpha is not colour and is copied through untouched.
  }
}

} // namespace graphics
