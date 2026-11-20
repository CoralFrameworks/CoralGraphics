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

#ifndef CORAL_GRAPHICS_EXAMPLES_LINUX_DEMO_SCENE_H
#define CORAL_GRAPHICS_EXAMPLES_LINUX_DEMO_SCENE_H

#include <graphics/Canvas.h>

#include <cstdint>

namespace demo {

/**
 * Draws one frame of the Linux demo.
 *
 * The scene is deliberately cheap and self-contained: a few filled shapes, one stroked path and a
 * moving highlight. It is a client of the public `graphics::Canvas` API only, so the same function
 * draws through the window surface and through the offscreen `--verify` path.
 */
void drawFrame(graphics::Canvas& canvas, uint32_t width, uint32_t height, double seconds);

} // namespace demo

#endif // CORAL_GRAPHICS_EXAMPLES_LINUX_DEMO_SCENE_H
