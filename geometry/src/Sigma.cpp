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

#include <geometry/Sigma.h>

namespace graphics {

Sigma::operator Radius() const {
  return Radius{ sigma > 0.5f ? (sigma - 0.5f) * kKernelRadiusPerSigma : 0.0f };
}

Radius::operator Sigma() const {
  return Sigma{ radius > 0 ? radius / kKernelRadiusPerSigma + 0.5f : 0.0f };
}

} // namespace graphics
