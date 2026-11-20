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

#include <geometry/Gradient.h>

#include <utils/Assert.h>

namespace graphics {

void Gradient::AppendColor(const Color& color, GradientData* gradientData) {
  if (gradientData == nullptr) {
    return;
  }
  const auto converted = color.toR8G8B8A8();
  gradientData->colors.push_back(converted[0]);
  gradientData->colors.push_back(converted[1]);
  gradientData->colors.push_back(converted[2]);
  gradientData->colors.push_back(converted[3]);
}

GradientData Gradient::CreateGradientBuffer(
  const std::vector<Color>& colors,
  const std::vector<Scalar>& stops) {
  CORAL_ASSERT(stops.size() == colors.size());

  uint32_t textureSize;
  if (stops.size() == 2) {
    textureSize = colors.size();
  } else {
    auto minimumDelta = 1.0;
    for (size_t i = 1; i < stops.size(); ++i) {
      const auto value = stops[i] - stops[i - 1];
      // Smaller than kEhCloseEnough
      if (value < 0.0001) {
        continue;
      }
      if (value < minimumDelta) {
        minimumDelta = value;
      }
    }
    textureSize = std::min(static_cast<uint32_t>(std::round(1.0 / minimumDelta)) + 1, 1024u);
  }
  GradientData data = {
    .colors = {},
    .textureSize = textureSize,
  };
  data.colors.reserve(textureSize * 4);

  if (textureSize == colors.size() && colors.size() <= 1024) {
    for (auto i = 0u; i < colors.size(); i++) {
      AppendColor(colors[i], &data);
    }
  } else {
    auto previousColor = colors[0];
    auto previousStop = 0.0;
    auto previousColorIndex = 0;

    // The first index is always equal to the first color, exactly.
    AppendColor(previousColor, &data);

    for (auto i = 1u; i < textureSize - 1; ++i) {
      auto scaledIndex = i / (textureSize - 1.0);
      auto nextColor = colors[previousColorIndex + 1];
      auto nextStop = stops[previousColorIndex + 1];
      // We're almost exactly equal to the next stop.
      if (ScalarNearlyEqual(scaledIndex, nextStop)) {
        AppendColor(nextColor, &data);

        previousColor = nextColor;
        previousStop = nextStop;
        previousColorIndex += 1;
      } else if (scaledIndex < nextStop) {
        // We're still between the current stop and the next stop.
        auto t = (scaledIndex - previousStop) / (nextStop - previousStop);
        auto mixedColor = Color::Lerp(previousColor, nextColor, t);

        AppendColor(mixedColor, &data);
      } else {
        // We've slightly overshot the previous stop.
        previousColor = nextColor;
        previousStop = nextStop;
        previousColorIndex += 1;
        nextColor = colors[previousColorIndex + 1];
        nextStop = stops[previousColorIndex + 1];

        auto t = (scaledIndex - previousStop) / (nextStop - previousStop);
        auto mixedColor = Color::Lerp(previousColor, nextColor, t);

        AppendColor(mixedColor, &data);
      }
    }
    // The last index is always equal to the last color, exactly.
    AppendColor(colors.back(), &data);
  }
  return data;
}

} // namespace graphics
