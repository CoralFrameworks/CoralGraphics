/*
 * Copyright (C) 2025 The Coral Project
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

#ifndef CORAL_GEOMETRY_MATRIX_DECOMPOSITION_H
#define CORAL_GEOMETRY_MATRIX_DECOMPOSITION_H

#include <geometry/Quaternion.h>
#include <geometry/Shear.h>
#include <geometry/Vector.h>

namespace graphics {

struct MatrixDecomposition {
  Vector3 translation;
  Vector3 scale;
  Shear shear;
  Vector4 perspective;
  Quaternion rotation;

  enum class Component {
    kTranslation = 1 << 0,
    kScale = 1 << 1,
    kShear = 1 << 2,
    kPerspective = 1 << 3,
    kRotation = 1 << 4,
  };

  [[nodiscard]] uint64_t getComponentsMask() const;
};

} // namespace graphics

#endif //CORAL_GEOMETRY_MATRIX_DECOMPOSITION_H
