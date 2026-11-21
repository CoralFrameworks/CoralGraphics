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

#ifndef CORAL_GRAPHICS_EXAMPLES_MAC_DEMO_OPTIONS_H
#define CORAL_GRAPHICS_EXAMPLES_MAC_DEMO_OPTIONS_H

#include <cstdint>
#include <string>

namespace demo {

/** The command line shared by the window path and the offscreen verification path. */
struct Options {
  uint32_t width = 960;
  uint32_t height = 540;
  /** Present this many frames and exit; zero runs until the window closes. */
  uint32_t frames = 0;
  /** Present without waiting for the vsync interval. */
  bool immediate = false;
  /** Render one offscreen frame instead of opening a window. */
  bool verify = false;
  /** "auto" or "opengl"; the macOS window path is CGL/OpenGL. */
  std::string backend = "auto";
  /** Optional PNG written from the window's readback, for checking the presented frame without a screenshot. */
  std::string snapshot;
};

} // namespace demo

#endif // CORAL_GRAPHICS_EXAMPLES_MAC_DEMO_OPTIONS_H
