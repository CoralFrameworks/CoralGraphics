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

#include <graphics/Paint.h>

namespace graphics {

bool Paint::hasColorFilter() const {
  if (colorFilter == nullptr) return false;

  // The identity is not a filter. Asking the filter rather than the pointer is what lets a caller assemble one
  // conditionally without checking whether the assembly produced anything.
  return !colorFilter->isIdentity();
}

bool Paint::hasShader() const noexcept {
  // Validity is asked of the shader rather than assumed from the pointer: a gradient built from degenerate geometry
  // cannot draw, and the canvas should use the flat colour instead of spending a per-pixel evaluation to find out.
  return shader != nullptr && shader->isValid();
}

} // namespace graphics