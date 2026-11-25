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

#ifndef CORAL_GEOMETRY_COMPILER_H
#define CORAL_GEOMETRY_COMPILER_H

#include <type_traits>

#if defined (WIN32)

#ifdef max
#undef max
#endif

#ifdef min
#undef min
#endif

#ifdef far
#undef far
#endif

#ifdef near
#undef near
#endif

#ifdef ERROR
#undef ERROR
#endif

#ifdef OPAQUE
#undef OPAQUE
#endif

#ifdef TRANSPARENT
#undef TRANSPARENT
#endif

#ifdef PURE
#undef PURE
#endif

#endif

#ifdef _MSC_VER
#define GEOMETRY_EMPTY_BASES __declspec(empty_bases)

// MSVC does not support loop unrolling hints
#define GEOMETRY_NOUNROLL

// About value initialization, the C++ standard says:
//   if T is a class type with a default constructor that is neither user-provided nor deleted
//   (that is, it may be a class with an implicitly-defined or defaulted constructor),
//   the object is zero-initialized, and then it is default-initialized
//   if it has a non-trivial default constructor;
// Unfortunately, MSVC always calls the default constructor, even if it is trivial, which
// breaks constexpr-ness. To work around this, we're always zero-initializing TVecN<>
#define GEOMETRY_CONSTEXPR_INIT {}
#define GEOMETRY_DEFAULT_CTOR {}
#define GEOMETRY_DEFAULT_CTOR_CONSTEXPR constexpr
#define CONSTEXPR_IF_NOT_MSVC // when declared constexpr, msvc fails with "failure was caused by cast of object of dynamic type"

#else // _MSC_VER

#define GEOMETRY_EMPTY_BASES

// C++11 allows pragmas to be specified as part of defines using the _Pragma syntax.
#define GEOMETRY_NOUNROLL _Pragma("nounroll")

#define GEOMETRY_CONSTEXPR_INIT
#define GEOMETRY_DEFAULT_CTOR = default;
#define GEOMETRY_DEFAULT_CTOR_CONSTEXPR
#define CONSTEXPR_IF_NOT_MSVC constexpr

#endif // _MSC_VER

#endif //CORAL_GEOMETRY_COMPILER_H
