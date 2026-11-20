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

#include <geometry/Quaternion.h>

namespace graphics {

Quaternion Quaternion::slerp(const Quaternion& to, double time) const {
  double cosine = dot(to);
  if (fabs(cosine) < 1.0 - 1e-3 /* epsilon */) {
    /*
     *  Spherical Interpolation.
     */
    auto sine = sqrt(1.0 - cosine * cosine);
    auto angle = atan2(sine, cosine);
    auto sineInverse = 1.0 / sine;
    auto c0 = sin((1.0 - time) * angle) * sineInverse;
    auto c1 = sin(time * angle) * sineInverse;
    return *this * c0 + to * c1;
  } else {
    /*
     *  Linear Interpolation.
     */
    return (*this * (1.0 - time) + to * time).normalize();
  }
}

} // namespace graphics
