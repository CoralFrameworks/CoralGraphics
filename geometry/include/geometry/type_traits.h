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

#ifndef CORAL_GEOMETRY_TYPE_TRAITS_H
#define CORAL_GEOMETRY_TYPE_TRAITS_H

#include <type_traits>

namespace graphics {

template<class F,
    class I,
    class = std::enable_if_t<std::is_floating_point_v<F> && std::is_integral_v<I>>>
struct MixedOp_ : public std::true_type {
};

template<class F, class I>
using MixedOp = typename MixedOp_<F, I>::type;

} // namespace graphics

#endif //CORAL_GEOMETRY_TYPE_TRAITS_H
