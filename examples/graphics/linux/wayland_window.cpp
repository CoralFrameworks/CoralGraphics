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

#include "xdg-shell-client-protocol.h"

#include <gpu/WindowSurface.h>

#include <wayland-client.h>
#include <wayland-egl.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <poll.h>
#include <unistd.h>

namespace demo {

namespace {

struct WaylandState {
  wl_display* display = nullptr;
  wl_registry* registry = nullptr;
  wl_compositor* compositor = nullptr;
  xdg_wm_base* wmBase = nullptr;
  wl_surface* surface = nullptr;
  xdg_surface* xdgSurface = nullptr;
  xdg_toplevel* toplevel = nullptr;
  wl_egl_window* eglWindow = nullptr;
  std::unique_ptr<graphics::gpu::WindowSurface> windowSurface;
  uint32_t width = 0;
  uint32_t height = 0;
  bool configured = false;
  bool running = true;
  bool backendMismatch = false;
  uint32_t requestedWidth = 960;
  uint32_t requestedHeight = 540;
  bool immediate = false;
  backend::Backend preferred = backend::Backend::Null;
};

void wmBasePing(void*, xdg_wm_base* wmBase, const uint32_t serial) {
  xdg_wm_base_pong(wmBase, serial);
}

const xdg_wm_base_listener kWmBaseListener = { wmBasePing };

void toplevelConfigure(void* data,
  xdg_toplevel*,
  const int32_t width,
  const int32_t height,
  wl_array*) {
  auto& state = *static_cast<WaylandState*>(data);
  if (width > 0) {
    state.requestedWidth = static_cast<uint32_t>(width);
  }
  if (height > 0) {
    state.requestedHeight = static_cast<uint32_t>(height);
  }
}

void toplevelClose(void* data, xdg_toplevel*) {
  static_cast<WaylandState*>(data)->running = false;
}

const xdg_toplevel_listener kToplevelListener = { toplevelConfigure, toplevelClose };

void xdgSurfaceConfigure(void* data, xdg_surface* xdgSurface, const uint32_t serial) {
  auto& state = *static_cast<WaylandState*>(data);
  xdg_surface_ack_configure(xdgSurface, serial);

  state.width = std::max(1u, state.requestedWidth);
  state.height = std::max(1u, state.requestedHeight);

  if (state.eglWindow == nullptr) {
    state.eglWindow = wl_egl_window_create(state.surface, static_cast<int>(state.width),
      static_cast<int>(state.height));
    if (state.eglWindow == nullptr) {
      state.running = false;
      return;
    }

    graphics::gpu::WindowSurface::Configuration configuration;
    // Vulkan's WSI wants the wl_surface; EGL wants the wl_egl_window that wraps it. Carrying both
    // lets --backend auto/opengl/vulkan use the same WindowSurface configuration.
    configuration.nativeWindow = state.surface;
    configuration.nativeWindowExtra = state.eglWindow;
    configuration.nativeDisplay = state.display;
    configuration.nativeWindowSystem = backend::NativeWindowSystem::Wayland;
    configuration.width = state.width;
    configuration.height = state.height;
    configuration.preferred = state.preferred;
    configuration.presentMode = state.immediate ? backend::PresentMode::Immediate : backend::PresentMode::Fifo;
    state.windowSurface = graphics::gpu::WindowSurface::make(configuration);
    if (state.windowSurface == nullptr) {
      state.running = false;
      return;
    }
    if (state.preferred != backend::Backend::Null &&
        state.windowSurface->device()->getInfo().backend != state.preferred) {
      std::fprintf(stderr, "linux_demo: asked for %s, but the window fell back to %s\n",
        backend::toString(state.preferred), backend::toString(state.windowSurface->device()->getInfo().backend));
      state.backendMismatch = true;
      state.running = false;
      return;
    }
    const std::string device = state.windowSurface->describeDevice();
    std::printf("linux_demo: %s\n", device.c_str());
    if (device.find("llvmpipe") != std::string::npos || device.find("softpipe") != std::string::npos ||
        device.find("swrast") != std::string::npos) {
      std::fprintf(stderr, "linux_demo: WARNING: the rendering device is a software rasterizer. The picture "
                           "is correct, but animation will be slow; install a GPU driver and make /dev/dri "
                           "available for hardware rendering.\n");
    }
  } else {
    wl_egl_window_resize(state.eglWindow, static_cast<int>(state.width), static_cast<int>(state.height), 0, 0);
    if (!state.windowSurface->resize(state.width, state.height)) {
      state.running = false;
      return;
    }
  }

  state.configured = true;
}

const xdg_surface_listener kXdgSurfaceListener = { xdgSurfaceConfigure };

void registryGlobal(void* data,
  wl_registry* registry,
  const uint32_t name,
  const char* interface,
  const uint32_t version) {
  auto& state = *static_cast<WaylandState*>(data);
  if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
    state.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface,
      std::min(version, 4u)));
  } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
    state.wmBase = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface,
      std::min(version, 1u)));
    xdg_wm_base_add_listener(state.wmBase, &kWmBaseListener, &state);
  }
}

void registryGlobalRemove(void*, wl_registry*, uint32_t) {
}

const wl_registry_listener kRegistryListener = { registryGlobal, registryGlobalRemove };

void destroyState(WaylandState& state) {
  state.windowSurface.reset();
  if (state.eglWindow != nullptr) {
    wl_egl_window_destroy(state.eglWindow);
    state.eglWindow = nullptr;
  }
  if (state.toplevel != nullptr) {
    xdg_toplevel_destroy(state.toplevel);
    state.toplevel = nullptr;
  }
  if (state.xdgSurface != nullptr) {
    xdg_surface_destroy(state.xdgSurface);
    state.xdgSurface = nullptr;
  }
  if (state.surface != nullptr) {
    wl_surface_destroy(state.surface);
    state.surface = nullptr;
  }
  if (state.wmBase != nullptr) {
    xdg_wm_base_destroy(state.wmBase);
    state.wmBase = nullptr;
  }
  if (state.compositor != nullptr) {
    wl_compositor_destroy(state.compositor);
    state.compositor = nullptr;
  }
  if (state.registry != nullptr) {
    wl_registry_destroy(state.registry);
    state.registry = nullptr;
  }
  if (state.display != nullptr) {
    wl_display_disconnect(state.display);
    state.display = nullptr;
  }
}

} // namespace anonymous

int runWayland(const Options& options) {
  WaylandState state;
  state.requestedWidth = options.width;
  state.requestedHeight = options.height;
  state.immediate = options.immediate;
  state.preferred = std::strcmp(options.backend, "vulkan") == 0 ? backend::Backend::Vulkan
                     : std::strcmp(options.backend, "opengl") == 0 ? backend::Backend::OpenGL
                                                                   : backend::Backend::Null;
  // A compositor without a DRM node cannot complete a fifo swap: eglSwapBuffers waits for a frame
  // callback that never arrives. That is the shape of a container that has the Wayland socket but
  // not /dev/dri. Immediate presentation is the only mode that can make progress there, so choose
  // it unless the caller explicitly asked for fifo.
  if (!state.immediate && ::access("/dev/dri", F_OK) != 0) {
    state.immediate = true;
    std::fprintf(stderr, "linux_demo: /dev/dri is not visible; presenting immediately. This is a software "
                         "compositor path, not a hardware one.\n");
  }
  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    std::fprintf(stderr, "linux_demo: no Wayland compositor (WAYLAND_DISPLAY is not set or unreachable)\n");
    return 77;
  }

  state.registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(state.registry, &kRegistryListener, &state);
  if (wl_display_roundtrip(state.display) < 0) {
    std::fprintf(stderr, "linux_demo: the Wayland registry could not be read\n");
    destroyState(state);
    return 1;
  }
  if (state.compositor == nullptr || state.wmBase == nullptr) {
    std::fprintf(stderr, "linux_demo: the compositor does not provide wl_compositor and xdg_wm_base\n");
    destroyState(state);
    return 1;
  }

  state.surface = wl_compositor_create_surface(state.compositor);
  state.xdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, state.surface);
  xdg_surface_add_listener(state.xdgSurface, &kXdgSurfaceListener, &state);
  state.toplevel = xdg_surface_get_toplevel(state.xdgSurface);
  xdg_toplevel_add_listener(state.toplevel, &kToplevelListener, &state);
  xdg_toplevel_set_title(state.toplevel, "Coral OpenGL demo - Wayland");
  xdg_toplevel_set_app_id(state.toplevel, "org.coral.graphics.demo");
  wl_surface_commit(state.surface);

  const auto started = std::chrono::steady_clock::now();
  uint32_t presented = 0;
  bool fifo = true;
  bool software = false;
  bool detailsKnown = false;
  while (state.running && (options.frames == 0 || presented < options.frames)) {
    if (wl_display_dispatch_pending(state.display) < 0) {
      break;
    }
    if (wl_display_flush(state.display) < 0 && errno != EAGAIN) {
      break;
    }

    // Poll for already-arrived events without waiting. A Fifo `present()` below is the frame clock;
    // waiting 16 ms before each frame -- which is what this loop used to do -- made the next frame
    // miss the following compositor callback, giving the irregular 16/33 ms pacing seen on Wayland.
    pollfd descriptor{ wl_display_get_fd(state.display), POLLIN, 0 };
    const int ready = poll(&descriptor, 1, 0);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (ready > 0) {
      if (wl_display_dispatch(state.display) < 0) {
        break;
      }
    }

    if (!state.configured || state.windowSurface == nullptr) {
      // Nothing to draw until the initial configure; do not spin while waiting for it.
      pollfd waitForConfigure{ wl_display_get_fd(state.display), POLLIN, 0 };
      const int configuredReady = poll(&waitForConfigure, 1, 16);
      if (configuredReady > 0 && wl_display_dispatch(state.display) < 0) {
        break;
      }
      if (configuredReady < 0 && errno != EINTR) {
        break;
      }
      continue;
    }

    if (!detailsKnown) {
      const std::string device = state.windowSurface->describeDevice();
      software = device.find("llvmpipe") != std::string::npos ||
                 device.find("softpipe") != std::string::npos ||
                 device.find("swrast") != std::string::npos;
      fifo = state.windowSurface->presentMode() == backend::PresentMode::Fifo;
      detailsKnown = true;
    }

    graphics::Canvas* canvas = state.windowSurface->beginFrame();
    if (canvas == nullptr) {
      std::fprintf(stderr, "linux_demo: beginFrame failed: %s\n", state.windowSurface->diagnostics().c_str());
      break;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    drawFrame(*canvas, state.width, state.height, seconds);
    if (!state.windowSurface->present()) {
      std::fprintf(stderr, "linux_demo: present failed: %s\n", state.windowSurface->diagnostics().c_str());
      break;
    }
    ++presented;

    if (!fifo || software) {
      // Immediate presentation has no frame callback to clock the loop; a tiny wait keeps it from
      // spinning at thousands of frames per second and burning a core for no visible benefit.
      pollfd idle{ wl_display_get_fd(state.display), POLLIN, 0 };
      const int idleReady = poll(&idle, 1, 8);
      if (idleReady > 0 && wl_display_dispatch(state.display) < 0) {
        break;
      }
      if (idleReady < 0 && errno != EINTR) {
        break;
      }
    }
  }

  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::printf("linux_demo: presented %u frame(s) in %.2f s (%.1f fps)\n", presented, elapsed,
    elapsed > 0.0 ? presented / elapsed : 0.0);
  const bool failed = !state.configured || state.windowSurface == nullptr || state.backendMismatch;
  if (failed) {
    std::fprintf(stderr, "linux_demo: no OpenGL device for this Wayland surface. Check that libegl-dev, "
                         "libwayland-dev and a working EGL driver are installed.\n");
  }
  state.running = false;
  destroyState(state);
  return failed ? 1 : 0;
}

} // namespace demo
