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

#include <gpu/WindowSurface.h>

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <string>
#include <thread>

namespace demo {

namespace {

int ignoreXError(Display*, XErrorEvent*) {
  return 0;
}

} // namespace anonymous

int runX11(const Options& options) {
  Display* display = XOpenDisplay(nullptr);
  if (display == nullptr) {
    std::fprintf(stderr, "linux_demo: no X11 display (DISPLAY is not set or unreachable)\n");
    return 77;
  }

  // A window that goes away during a swap must not terminate the process through Xlib's default
  // handler; the demo is a client that reports the failure and exits cleanly.
  XSetErrorHandler(ignoreXError);

  const int screen = DefaultScreen(display);
  const Window root = RootWindow(display, screen);
  const Window window = XCreateSimpleWindow(display, root, 0, 0, options.width, options.height, 0,
    BlackPixel(display, screen), BlackPixel(display, screen));
  if (window == 0) {
    std::fprintf(stderr, "linux_demo: could not create an X11 window\n");
    XCloseDisplay(display);
    return 1;
  }

  XStoreName(display, window, "Coral OpenGL demo - X11");
  XSelectInput(display, window, ExposureMask | KeyPressMask | StructureNotifyMask);
  Atom wmDelete = XInternAtom(display, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(display, window, &wmDelete, 1);
  XMapWindow(display, window);
  XFlush(display);

  graphics::gpu::WindowSurface::Configuration configuration;
  configuration.nativeWindow = reinterpret_cast<void*>(static_cast<uintptr_t>(window));
  configuration.nativeDisplay = display;
  configuration.nativeWindowSystem = backend::NativeWindowSystem::X11;
  configuration.width = options.width;
  configuration.height = options.height;
  configuration.preferred = std::strcmp(options.backend, "vulkan") == 0 ? backend::Backend::Vulkan
                             : std::strcmp(options.backend, "opengl") == 0 ? backend::Backend::OpenGL
                                                                           : backend::Backend::Null;
  configuration.presentMode = options.immediate ? backend::PresentMode::Immediate : backend::PresentMode::Fifo;

  auto surface = graphics::gpu::WindowSurface::make(configuration);
  if (surface == nullptr) {
    std::fprintf(stderr, "linux_demo: no OpenGL device for this X11 window. Check that libgl-dev and a "
                         "working GLX or EGL driver are installed.\n");
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 1;
  }

  const backend::Backend expected = std::strcmp(options.backend, "vulkan") == 0 ? backend::Backend::Vulkan
                                   : std::strcmp(options.backend, "opengl") == 0 ? backend::Backend::OpenGL
                                                                                 : backend::Backend::Null;
  if (expected != backend::Backend::Null && surface->device()->getInfo().backend != expected) {
    std::fprintf(stderr, "linux_demo: asked for %s, but the window fell back to %s\n",
      backend::toString(expected), backend::toString(surface->device()->getInfo().backend));
    surface.reset();
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 1;
  }
  const std::string device = surface->describeDevice();
  const bool software = device.find("llvmpipe") != std::string::npos ||
                        device.find("softpipe") != std::string::npos ||
                        device.find("swrast") != std::string::npos;
  const bool fifo = surface->presentMode() == backend::PresentMode::Fifo;
  std::printf("linux_demo: %s\n", device.c_str());
  if (software) {
    std::fprintf(stderr, "linux_demo: WARNING: the rendering device is a software rasterizer. The picture is "
                         "correct, but animation will be slow; install a GPU driver and make /dev/dri available "
                         "for hardware rendering.\n");
  }

  bool running = true;
  bool failed = false;
  bool resized = false;
  uint32_t width = options.width;
  uint32_t height = options.height;
  uint32_t presented = 0;
  const auto started = std::chrono::steady_clock::now();
  const int connection = ConnectionNumber(display);

  while (running && (options.frames == 0 || presented < options.frames)) {
    while (XPending(display) > 0) {
      XEvent event{};
      XNextEvent(display, &event);
      if (event.type == ClientMessage && static_cast<Atom>(event.xclient.data.l[0]) == wmDelete) {
        running = false;
      } else if (event.type == DestroyNotify) {
        running = false;
      } else if (event.type == ConfigureNotify) {
        const auto newWidth = static_cast<uint32_t>(std::max(1, event.xconfigure.width));
        const auto newHeight = static_cast<uint32_t>(std::max(1, event.xconfigure.height));
        if (newWidth != width || newHeight != height) {
          width = newWidth;
          height = newHeight;
          resized = true;
        }
      } else if (event.type == KeyPress) {
        const KeySym key = XLookupKeysym(&event.xkey, 0);
        if (key == XK_Escape || key == XK_q) {
          running = false;
        }
      }
    }

    if (!running) {
      break;
    }
    if (resized) {
      resized = false;
      if (!surface->resize(width, height)) {
        std::fprintf(stderr, "linux_demo: resize failed: %s\n", surface->diagnostics().c_str());
        failed = true;
        break;
      }
    }

    graphics::Canvas* canvas = surface->beginFrame();
    if (canvas == nullptr) {
      std::fprintf(stderr, "linux_demo: beginFrame failed: %s\n", surface->diagnostics().c_str());
      failed = true;
      break;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    drawFrame(*canvas, width, height, seconds);
    if (!surface->present()) {
      std::fprintf(stderr, "linux_demo: present failed: %s\n", surface->diagnostics().c_str());
      failed = true;
      break;
    }
    ++presented;

    // A Fifo present already blocks until the display is ready for the next frame, so waiting here
    // would push the next frame past the following vblank and halve the rate. The poll is only for
    // input in that case; immediate and software paths still need a small sleep to avoid spinning.
    const int timeout = fifo && !software ? 0 : 8;
    pollfd descriptor{ connection, POLLIN, 0 };
    if (poll(&descriptor, 1, timeout) < 0 && errno != EINTR) {
      break;
    }
  }

  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::printf("linux_demo: presented %u frame(s) in %.2f s (%.1f fps)\n", presented, elapsed,
    elapsed > 0.0 ? presented / elapsed : 0.0);
  surface.reset();
  XDestroyWindow(display, window);
  XCloseDisplay(display);
  return failed ? 1 : 0;
}

} // namespace demo
