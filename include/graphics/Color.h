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

#ifndef CORAL_GRAPHICS_COLOR_H
#define CORAL_GRAPHICS_COLOR_H

/**
 * This header exists only to make `graphics::Color` reachable as `<graphics/Color.h>`.
 *
 * The type itself lives in `geometry`, which is where the shared value types are. There is nothing to add to it
 * here -- and nothing *may* be added, because `geometry` is the lower layer and this one is above it. It
 * re-exports and stops.
 *
 * ## The include guard is not the obvious one
 *
 * It used to be `CORAL_GRAPHICS_COLOR_H`, which is the guard `geometry/Color.h` uses. Two headers with one guard
 * macro means whichever is included first silently suppresses the other, so including both gave whichever arrived
 * first and the compiler reported the types from the second as missing. The failure looked like a missing type
 * rather than a name collision, which is why it survived until something included both.
 */
#include <geometry/Color.h>

#endif //CORAL_GRAPHICS_COLOR_H
