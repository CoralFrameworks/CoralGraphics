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

#ifndef CORAL_GEOMETRY_GRADIENT_H
#define CORAL_GEOMETRY_GRADIENT_H

#include <geometry/Color.h>
#include <geometry/Scalar.h>

#include <vector>

namespace graphics {

// If textureSize is 0 then the gradient is invalid.
struct GradientData {
  std::vector<uint8_t> colors;
  uint32_t textureSize;
};

class Gradient {
public:
  static GradientData CreateGradientBuffer(const std::vector<Color>& colors,
    const std::vector<Scalar>& stops);

private:
  Gradient() = default;
  ~Gradient() = default;

  static void AppendColor(const Color& color, GradientData* gradientData);
};

} // namespace graphics

#endif //CORAL_GEOMETRY_GRADIENT_H
