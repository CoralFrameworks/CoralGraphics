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

#include "demo_options.h"
#include "demo_scene.h"

#include <gpu/Surface.h>

#include <graphics/Bitmap.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void printUsage(const char* program) {
  std::printf(
    "usage: %s [options]\n"
    "  --platform auto|x11|wayland  window system (default: auto; Wayland when WAYLAND_DISPLAY is set)\n"
    "  --backend auto|opengl|vulkan  rendering backend (default: auto, Vulkan first on Linux)\n"
    "  --width N                    window width in pixels (default 960)\n"
    "  --height N                   window height in pixels (default 540)\n"
    "  --frames N                   present N frames and exit; 0 runs until the window closes\n"
    "  --immediate                  present without waiting for vsync\n"
    "  --verify                     render one offscreen frame instead of opening a window\n"
    "  --help                       print this text\n"
    "\n"
    "The window path uses the public graphics::gpu::WindowSurface API, and the same scene is drawn\n"
    "through an offscreen graphics::gpu::Surface for --verify, so the latter runs on a headless host.\n",
    program);
}

uint32_t parseDimension(const char* text, const uint32_t fallback) {
  if (text == nullptr) {
    return fallback;
  }
  char* end = nullptr;
  const unsigned long value = std::strtoul(text, &end, 10);
  if (end == text || *end != '\0' || value == 0) {
    return fallback;
  }
  return static_cast<uint32_t>(value);
}

uint32_t parseFrames(const char* text) {
  if (text == nullptr) {
    return 0;
  }
  char* end = nullptr;
  const unsigned long value = std::strtoul(text, &end, 10);
  if (end == text || *end != '\0') {
    return 0;
  }
  return static_cast<uint32_t>(value);
}

int verifyOffscreen(const demo::Options& options) {
  graphics::gpu::SurfaceConfiguration configuration;
  configuration.width = options.width;
  configuration.height = options.height;
  if (std::strcmp(options.backend, "vulkan") == 0) {
    configuration.preferred = backend::Backend::Vulkan;
  } else if (std::strcmp(options.backend, "opengl") == 0) {
    configuration.preferred = backend::Backend::OpenGL;
  }
  configuration.stencil = true;

  auto surface = graphics::gpu::Surface::make(configuration);
  if (surface == nullptr) {
    std::fprintf(stderr, "linux_demo --verify: no OpenGL device. This is a host/driver problem, not a "
                         "window-system one.\n");
    return 1;
  }

  if (std::strcmp(options.backend, "vulkan") == 0 &&
      surface->device()->getInfo().backend != backend::Backend::Vulkan) {
    std::fprintf(stderr, "linux_demo --verify: asked for Vulkan, but the device fell back to %s\n",
      backend::toString(surface->device()->getInfo().backend));
    return 1;
  }
  if (std::strcmp(options.backend, "opengl") == 0 &&
      surface->device()->getInfo().backend != backend::Backend::OpenGL) {
    std::fprintf(stderr, "linux_demo --verify: asked for OpenGL, but the device fell back to %s\n",
      backend::toString(surface->device()->getInfo().backend));
    return 1;
  }
  std::printf("linux_demo --verify: %s\n", surface->describeDevice().c_str());
  graphics::Canvas* canvas = surface->beginFrame();
  if (canvas == nullptr) {
    std::fprintf(stderr, "linux_demo --verify: beginFrame failed\n");
    return 1;
  }
  demo::drawFrame(*canvas, options.width, options.height, 0.0);
  if (!surface->submit()) {
    std::fprintf(stderr, "linux_demo --verify: submit failed: %s\n", surface->diagnostics().c_str());
    return 1;
  }
  const auto bitmap = surface->snapshot();
  if (bitmap.width() != options.width || bitmap.height() != options.height) {
    std::fprintf(stderr, "linux_demo --verify: the snapshot is %ux%u, expected %ux%u\n", bitmap.width(),
      bitmap.height(), options.width, options.height);
    return 1;
  }
  std::printf("linux_demo --verify: drew and read back %ux%u pixels\n", bitmap.width(), bitmap.height());
  return 0;
}

} // namespace anonymous

int main(int argc, char** argv) {
  demo::Options options;
  bool verify = false;

  for (int index = 1; index < argc; ++index) {
    const char* argument = argv[index];
    const bool hasValue = index + 1 < argc;
    if (std::strcmp(argument, "--help") == 0 || std::strcmp(argument, "-h") == 0) {
      printUsage(argv[0]);
      return 0;
    }
    if (std::strcmp(argument, "--verify") == 0) {
      verify = true;
    } else if (std::strcmp(argument, "--immediate") == 0) {
      options.immediate = true;
    } else if (std::strcmp(argument, "--platform") == 0 && hasValue) {
      options.platform = argv[++index];
    } else if (std::strcmp(argument, "--backend") == 0 && hasValue) {
      options.backend = argv[++index];
    } else if (std::strcmp(argument, "--width") == 0 && hasValue) {
      options.width = parseDimension(argv[++index], options.width);
    } else if (std::strcmp(argument, "--height") == 0 && hasValue) {
      options.height = parseDimension(argv[++index], options.height);
    } else if (std::strcmp(argument, "--frames") == 0 && hasValue) {
      options.frames = parseFrames(argv[++index]);
    } else {
      std::fprintf(stderr, "linux_demo: unknown argument '%s'\n", argument);
      printUsage(argv[0]);
      return 2;
    }
  }

  if (verify) {
    return verifyOffscreen(options);
  }

  const char* platform = options.platform;
  if (std::strcmp(platform, "auto") == 0) {
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    platform = wayland != nullptr && wayland[0] != '\0' ? "wayland" : "x11";
  }

  if (std::strcmp(options.backend, "auto") != 0 && std::strcmp(options.backend, "opengl") != 0 &&
      std::strcmp(options.backend, "vulkan") != 0) {
    std::fprintf(stderr, "linux_demo: unknown backend '%s' (expected auto, opengl or vulkan)\n", options.backend);
    return 2;
  }

  if (std::strcmp(platform, "x11") == 0) {
    return demo::runX11(options);
  }
  if (std::strcmp(platform, "wayland") == 0) {
    return demo::runWayland(options);
  }

  std::fprintf(stderr, "linux_demo: unknown platform '%s' (expected auto, x11 or wayland)\n", platform);
  return 2;
}
