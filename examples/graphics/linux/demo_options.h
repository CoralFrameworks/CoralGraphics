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

#ifndef CORAL_GRAPHICS_EXAMPLES_LINUX_DEMO_OPTIONS_H
#define CORAL_GRAPHICS_EXAMPLES_LINUX_DEMO_OPTIONS_H

#include <cstdint>

namespace demo {

struct Options {
  uint32_t width = 960;
  uint32_t height = 540;
  /** Stop after this many presented frames; 0 runs until the window is closed. */
  uint32_t frames = 0;
  /** Present without waiting for vsync. Useful on a compositor that throttles a headless run. */
  bool immediate = false;
  /** "auto", "x11" or "wayland". */
  const char* platform = "auto";
  /** "auto", "opengl" or "vulkan". Auto uses the platform's backend priority, which is Vulkan first. */
  const char* backend = "auto";
};

int runX11(const Options& options);
int runWayland(const Options& options);

} // namespace demo

#endif // CORAL_GRAPHICS_EXAMPLES_LINUX_DEMO_OPTIONS_H
