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

/**
 * @file lottie_player.cpp
 *
 * A Linux window that plays a Lottie animation through the GPU backend.
 *
 * This is the Linux counterpart of `examples/graphics/win/lottie_player.cpp`: the same document parser, the
 * same `Lottie::render` call, and the same `gpu::WindowSurface` frame contract. Only the window
 * system is different. The Windows player has two paths, raster and GPU; this one demonstrates the
 * GPU path only, because that is the subject -- a CPU path would need a second window-blit
 * implementation for X11 and Wayland and would not exercise the swapchain.
 *
 * The three things the platform has to provide are the same everywhere: a native surface, a clock,
 * and a loop that keeps running when there is no input. `--frames` makes a windowed run scriptable,
 * and `--verify` renders one frame offscreen so a headless build machine still tests the parser,
 * renderer and GPU device.
 */

#include <lottie/Lottie.h>

#include <backend/Types.h>
#include <graphics/Bitmap.h>
#include <graphics/Canvas.h>
#include <graphics/Pixmap.h>
#include <gpu/Surface.h>
#include <gpu/WindowSurface.h>

#include "xdg-shell-client-protocol.h"

#include <wayland-client.h>
#include <wayland-egl.h>

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <poll.h>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

constexpr const char* kTitle = "Coral graphics - Lottie player (Linux)";

struct Options {
  std::string animation;
  std::string platform = "auto";
  backend::Backend preferred = backend::Backend::Null;
  int width = 640;
  int height = 640;
  int frames = 0;
  double startTime = 0.0;
  bool paused = false;
  bool verify = false;
};

struct Player {
  graphics::Lottie animation;
  std::unique_ptr<graphics::gpu::WindowSurface> windowSurface;
  int frameLimit = 0;
  int presented = 0;
  bool paused = false;
  bool looping = true;
  bool failed = false;
  double elapsed = 0.0;
  std::chrono::steady_clock::time_point lastTick;
  int titledFrame = -1;
  int framesSinceTitle = 0;
};

graphics::Color background() { return graphics::Color::MakeRGBA8(24, 26, 36, 255); }

std::string titleFor(const Player& player) {
  std::string title = kTitle;
  if (player.animation.isEmpty()) return title + " (no animation)";
  const int frame = static_cast<int>(std::lround(player.elapsed * player.animation.frameRate()));
  const int total = static_cast<int>(std::lround(player.animation.duration() * player.animation.frameRate()));
  title += " - frame " + std::to_string(frame) + " of " + std::to_string(total);
  if (player.paused) title += " [paused]";
  return title + "   (space pauses, left/right steps, R rewinds)";
}

void advance(Player& player, const double seconds) {
  if (player.paused) return;
  player.elapsed += seconds;
  const double duration = player.animation.duration();
  if (!(duration > 0.0)) return;
  if (player.looping) {
    player.elapsed = std::fmod(player.elapsed, duration);
    if (player.elapsed < 0.0) player.elapsed += duration;
  } else {
    player.elapsed = std::min(player.elapsed, duration);
  }
}

/**
 * Draws one frame into the window surface and presents it.
 *
 * The animation is rendered at its document size and fitted with a transform, not re-rasterised at
 * the window size: resizing preserves the composition exactly, and the letterbox bars are the clear
 * colour rather than stretched artwork.
 */
bool presentFrame(Player& player) {
  if (player.windowSurface == nullptr) return false;

  graphics::Canvas* canvas = player.windowSurface->beginFrame();
  if (canvas == nullptr) {
    std::fprintf(stderr, "lottie_player: beginFrame failed: %s\n",
      player.windowSurface->diagnostics().c_str());
    player.failed = true;
    return false;
  }

  canvas->clear(background());
  const auto windowWidth = static_cast<graphics::Scalar>(player.windowSurface->width());
  const auto windowHeight = static_cast<graphics::Scalar>(player.windowSurface->height());
  const auto animationWidth = static_cast<graphics::Scalar>(player.animation.width());
  const auto animationHeight = static_cast<graphics::Scalar>(player.animation.height());
  if (windowWidth <= 0.0f || windowHeight <= 0.0f || animationWidth <= 0.0f || animationHeight <= 0.0f) {
    player.failed = true;
    std::fprintf(stderr, "lottie_player: the window or the animation has no usable size\n");
    return false;
  }

  const graphics::Scalar scale = std::min(windowWidth / animationWidth, windowHeight / animationHeight);
  canvas->save();
  canvas->translate((windowWidth - animationWidth * scale) * 0.5f,
    (windowHeight - animationHeight * scale) * 0.5f);
  canvas->scale(scale);
  player.animation.render(*canvas, static_cast<graphics::Scalar>(player.elapsed));
  canvas->restore();

  if (!player.windowSurface->present()) {
    std::fprintf(stderr, "lottie_player: present failed: %s\n",
      player.windowSurface->diagnostics().c_str());
    player.failed = true;
    return false;
  }
  ++player.presented;
  return true;
}

bool isSoftwareRenderer(const std::string& device) {
  return device.find("llvmpipe") != std::string::npos || device.find("softpipe") != std::string::npos ||
         device.find("swrast") != std::string::npos;
}

void printDevice(const graphics::gpu::WindowSurface& surface) {
  const std::string device = surface.describeDevice();
  std::printf("  backend     : %s\n", device.c_str());
  if (isSoftwareRenderer(device)) {
    std::fprintf(stderr, "lottie_player: WARNING: the rendering device is a software rasterizer; "
                         "the animation will be slow. Make /dev/dri available for hardware rendering.\n");
  }
}

/** @return the document's text, or an empty string with @p error set. */
std::string readFile(const std::filesystem::path& path, std::string* error) {
  std::ifstream file(path, std::ios::binary);
  if (!file.good()) {
    *error = "could not open " + path.string();
    return {};
  }
  std::ostringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

/** @return the animation file to play: the argument, or the corpus document found by walking up from here. */
std::filesystem::path resolveAnimation(const std::string& requested) {
  if (!requested.empty()) return requested;

  std::filesystem::path directory = std::filesystem::current_path();
  for (int depth = 0; depth < 8 && !directory.empty(); ++depth) {
    const std::vector<std::filesystem::path> candidates {
      directory / "resources" / "apitest" / "lottie" / "gear.json",
      directory / "libs" / "graphics" / "resources" / "apitest" / "lottie" / "gear.json",
    };
    for (const std::filesystem::path& candidate: candidates) {
      std::error_code failure;
      if (std::filesystem::is_regular_file(candidate, failure)) return candidate;
    }
    directory = directory.parent_path();
  }
  return {};
}

backend::Backend parseGpuApi(const std::string& name, bool* recognised) {
  *recognised = true;
  if (name == "auto" || name == "null") return backend::Backend::Null;
  if (name == "gl" || name == "opengl" || name == "gles") return backend::Backend::OpenGL;
  if (name == "vulkan" || name == "vk") return backend::Backend::Vulkan;
  *recognised = false;
  return backend::Backend::Null;
}

int parseSizeDimension(const std::string& value, const int fallback) {
  const int parsed = std::atoi(value.c_str());
  return parsed >= 64 ? parsed : fallback;
}

void printUsage(const char* program) {
  std::printf(
    "Coral graphics Lottie player (Linux GPU)\n\n"
    "  %s [options]\n\n"
    "  --animation FILE  the Lottie JSON to play (default: resources/apitest/lottie/gear.json)\n"
    "  --size WxH        the window's initial size (default: 640x640)\n"
    "  --backend NAME    gpu (the only implemented backend; raster rejects)\n"
    "  --gpu-api NAME    preferred backend: auto, gl/opengl or vulkan; may fall back\n"
    "  --platform NAME   auto, x11 or wayland (default: auto)\n"
    "  --frames N        present N frames and exit (default: 0, until the window closes)\n"
    "  --time SECONDS    start at this point on the timeline\n"
    "  --paused          start paused; useful with --time for a fixed frame\n"
    "  --verify          render one frame offscreen and exit (no display required)\n"
    "  --help            this text\n\n"
    "While it is open: space pauses, the arrow keys step a frame, R rewinds, L toggles looping,\n"
    "escape closes. The window manager's close button also works.\n",
    program);
}

/**
 * Parses arguments. Returns 0 on success, or a process exit code to return immediately.
 */
int parseOptions(const int argc, char** argv, Options& options) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto next = [&index, argc, argv](const char* name) -> std::string {
      if (index + 1 >= argc) {
        std::fprintf(stderr, "lottie_player: %s needs a value\n", name);
        return {};
      }
      return argv[++index];
    };

    if (argument == "--help" || argument == "-h") {
      printUsage(argv[0]);
      return -1;
    }
    if (argument == "--animation" || argument == "-a") {
      options.animation = next("--animation");
    } else if (argument == "--size") {
      const std::string value = next("--size");
      const size_t cross = value.find('x');
      if (cross != std::string::npos) {
        options.width = parseSizeDimension(value.substr(0, cross), options.width);
        options.height = parseSizeDimension(value.substr(cross + 1), options.height);
      }
    } else if (argument == "--backend") {
      const std::string name = next("--backend");
      if (name == "raster" || name == "cpu" || name == "software") {
        std::fprintf(stderr, "lottie_player: the Linux example implements the GPU backend only; use --backend gpu\n");
        return 2;
      }
      if (name != "gpu" && name != "hardware") {
        std::fprintf(stderr, "lottie_player: unrecognised backend '%s' (expected gpu)\n", name.c_str());
        return 2;
      }
    } else if (argument == "--gpu-api") {
      bool recognised = false;
      options.preferred = parseGpuApi(next("--gpu-api"), &recognised);
      if (!recognised) {
        std::fprintf(stderr, "lottie_player: unrecognised GPU API (expected auto, gl, opengl or vulkan)\n");
        return 2;
      }
    } else if (argument == "--platform") {
      options.platform = next("--platform");
    } else if (argument == "--frames") {
      options.frames = std::max(0, std::atoi(next("--frames").c_str()));
    } else if (argument == "--time") {
      options.startTime = std::max(0.0, std::atof(next("--time").c_str()));
    } else if (argument == "--paused") {
      options.paused = true;
    } else if (argument == "--verify") {
      options.verify = true;
    } else {
      std::fprintf(stderr, "lottie_player: unknown argument '%s'\n", argument.c_str());
      printUsage(argv[0]);
      return 2;
    }
  }
  return 0;
}

/** Renders one frame offscreen and reports the pixels. Used by CTest and by a headless machine. */
int verifyOffscreen(const Options& options, const std::string& text, const std::filesystem::path& path) {
  std::string error;
  auto parsed = graphics::Lottie::parse(text, &error);
  if (!parsed) {
    std::fprintf(stderr, "lottie_player: %s: %s\n", path.string().c_str(), error.c_str());
    return 1;
  }

  const auto width = static_cast<uint32_t>(std::max(1, static_cast<int>(std::ceil(parsed->width()))));
  const auto height = static_cast<uint32_t>(std::max(1, static_cast<int>(std::ceil(parsed->height()))));
  graphics::gpu::SurfaceConfiguration configuration;
  configuration.width = width;
  configuration.height = height;
  configuration.preferred = options.preferred;
  configuration.stencil = true;
  auto surface = graphics::gpu::Surface::make(configuration);
  if (surface == nullptr) {
    std::fprintf(stderr, "lottie_player: no GPU device for --verify\n");
    return 77;
  }

  const std::string device = surface->describeDevice();
  const bool gotVulkan = device.find("Vulkan") != std::string::npos;
  const bool gotOpenGL = device.find("OpenGL") != std::string::npos;
  if ((options.preferred == backend::Backend::Vulkan && !gotVulkan) ||
      (options.preferred == backend::Backend::OpenGL && !gotOpenGL)) {
    std::fprintf(stderr, "lottie_player: asked for %s, but the device reported '%s'\n",
      backend::toString(options.preferred), device.c_str());
    return 1;
  }

  graphics::Canvas* canvas = surface->beginFrame();
  if (canvas == nullptr) {
    std::fprintf(stderr, "lottie_player: --verify could not begin a frame\n");
    return 1;
  }
  canvas->clear(background());
  parsed->render(*canvas, static_cast<graphics::Scalar>(options.startTime));
  if (!surface->submit()) {
    std::fprintf(stderr, "lottie_player: --verify could not submit: %s\n", surface->diagnostics().c_str());
    return 1;
  }
  const graphics::Bitmap bitmap = surface->snapshot();
  if (bitmap.width() != static_cast<int>(width) || bitmap.height() != static_cast<int>(height) ||
      bitmap.isEmpty()) {
    std::fprintf(stderr, "lottie_player: --verify read back a %dx%d bitmap, expected %ux%u\n",
      bitmap.width(), bitmap.height(), width, height);
    return 1;
  }

  const graphics::Pixmap pixels = bitmap.makePixmap();
  const uint32_t backgroundPixel = graphics::Color::toIntColor(background());
  size_t different = 0;
  for (int y = 0; y < bitmap.height(); y += 4) {
    for (int x = 0; x < bitmap.width(); x += 4) {
      if (pixels.getPixelARGB32(x, y) != backgroundPixel) ++different;
    }
  }
  std::printf("lottie_player --verify: %s\n", device.c_str());
  std::printf("lottie_player --verify: rendered %ux%u, %zu sampled pixel(s) differ from the background\n",
    width, height, different);
  return different == 0 ? 1 : 0;
}

// ==================================================================================================
// X11
// ==================================================================================================

int ignoreXError(Display*, XErrorEvent*) { return 0; }

int runX11(const Options& options, Player& player) {
  Display* display = XOpenDisplay(nullptr);
  if (display == nullptr) {
    std::fprintf(stderr, "lottie_player: no X11 display (DISPLAY is not set or unreachable)\n");
    return 77;
  }
  XSetErrorHandler(ignoreXError);

  const int screen = DefaultScreen(display);
  const Window root = RootWindow(display, screen);
  const Window window = XCreateSimpleWindow(display, root, 0, 0, static_cast<unsigned>(options.width),
    static_cast<unsigned>(options.height), 0, BlackPixel(display, screen), BlackPixel(display, screen));
  if (window == 0) {
    std::fprintf(stderr, "lottie_player: could not create an X11 window\n");
    XCloseDisplay(display);
    return 1;
  }

  XStoreName(display, window, titleFor(player).c_str());
  XSelectInput(display, window, ExposureMask | KeyPressMask | StructureNotifyMask);
  Atom wmDelete = XInternAtom(display, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(display, window, &wmDelete, 1);
  XMapWindow(display, window);
  XFlush(display);

  XWindowAttributes attributes {};
  XGetWindowAttributes(display, window, &attributes);
  uint32_t width = static_cast<uint32_t>(std::max(1, attributes.width));
  uint32_t height = static_cast<uint32_t>(std::max(1, attributes.height));

  graphics::gpu::WindowSurface::Configuration configuration;
  configuration.nativeWindow = reinterpret_cast<void*>(static_cast<uintptr_t>(window));
  configuration.nativeDisplay = display;
  configuration.nativeWindowSystem = backend::NativeWindowSystem::X11;
  configuration.width = width;
  configuration.height = height;
  configuration.preferred = options.preferred;
  configuration.presentMode = backend::PresentMode::Fifo;
  player.windowSurface = graphics::gpu::WindowSurface::make(configuration);
  if (player.windowSurface == nullptr) {
    std::fprintf(stderr, "lottie_player: no GPU device for this X11 window\n");
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 1;
  }
  if (options.preferred != backend::Backend::Null &&
      player.windowSurface->device()->getInfo().backend != options.preferred) {
    std::fprintf(stderr, "lottie_player: asked for %s, but the window fell back to %s\n",
      backend::toString(options.preferred),
      backend::toString(player.windowSurface->device()->getInfo().backend));
    player.windowSurface.reset();
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 1;
  }

  printDevice(*player.windowSurface);
  const bool fifo = player.windowSurface->presentMode() == backend::PresentMode::Fifo;
  const bool software = isSoftwareRenderer(player.windowSurface->describeDevice());

  bool running = true;
  bool resized = false;
  const auto started = std::chrono::steady_clock::now();
  player.lastTick = started;
  const int connection = ConnectionNumber(display);

  while (running && (player.frameLimit == 0 || player.presented < player.frameLimit)) {
    while (XPending(display) > 0) {
      XEvent event {};
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
        const double frame = player.animation.frameRate() > 0.0 ? 1.0 / player.animation.frameRate() : 0.0;
        if (key == XK_Escape) {
          running = false;
        } else if (key == XK_space) {
          player.paused = !player.paused;
        } else if (key == XK_Left) {
          player.elapsed = std::max(0.0, player.elapsed - frame);
        } else if (key == XK_Right) {
          player.elapsed = std::min(static_cast<double>(player.animation.duration()), player.elapsed + frame);
        } else if (key == XK_r || key == XK_R) {
          player.elapsed = 0.0;
        } else if (key == XK_l || key == XK_L) {
          player.looping = !player.looping;
        }
        XStoreName(display, window, titleFor(player).c_str());
      }
    }

    if (!running) break;
    if (resized) {
      resized = false;
      if (!player.windowSurface->resize(width, height)) {
        std::fprintf(stderr, "lottie_player: resize failed: %s\n",
          player.windowSurface->diagnostics().c_str());
        player.failed = true;
        break;
      }
    }

    const auto now = std::chrono::steady_clock::now();
    advance(player, std::chrono::duration<double>(now - player.lastTick).count());
    player.lastTick = now;
    if (!presentFrame(player)) break;

    if (++player.framesSinceTitle >= 15) {
      player.framesSinceTitle = 0;
      const int frame = static_cast<int>(std::lround(player.elapsed * player.animation.frameRate()));
      if (frame != player.titledFrame) {
        player.titledFrame = frame;
        XStoreName(display, window, titleFor(player).c_str());
      }
    }

    const int timeout = fifo && !software ? 0 : 8;
    pollfd descriptor { connection, POLLIN, 0 };
    if (poll(&descriptor, 1, timeout) < 0 && errno != EINTR) break;
  }

  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::printf("lottie_player: presented %d frame(s) in %.2f s (%.1f fps)\n", player.presented, elapsed,
    elapsed > 0.0 ? player.presented / elapsed : 0.0);
  player.windowSurface.reset();
  XDestroyWindow(display, window);
  XCloseDisplay(display);
  return player.failed ? 1 : 0;
}

// ==================================================================================================
// Wayland
// ==================================================================================================

struct WaylandState {
  wl_display* display = nullptr;
  wl_registry* registry = nullptr;
  wl_compositor* compositor = nullptr;
  wl_seat* seat = nullptr;
  wl_keyboard* keyboard = nullptr;
  xdg_wm_base* wmBase = nullptr;
  wl_surface* surface = nullptr;
  xdg_surface* xdgSurface = nullptr;
  xdg_toplevel* toplevel = nullptr;
  wl_egl_window* eglWindow = nullptr;
  Player* player = nullptr;
  backend::Backend preferred = backend::Backend::Null;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t requestedWidth = 640;
  uint32_t requestedHeight = 640;
  bool immediate = false;
  bool configured = false;
  bool running = true;
  bool failed = false;
};

void keyboardKeymap(void*, wl_keyboard*, const uint32_t format, const int fd, const uint32_t) {
  // The player does not use the keymap -- it maps the stable evdev codes below -- but the fd is
  // owned by the client and must be closed even when it is ignored.
  if (fd >= 0) ::close(fd);
  (void) format;
}

void keyboardEnter(void*, wl_keyboard*, const uint32_t, wl_surface*, wl_array*) {}
void keyboardLeave(void*, wl_keyboard*, const uint32_t, wl_surface*) {}
void keyboardModifiers(void*, wl_keyboard*, const uint32_t, const uint32_t, const uint32_t,
  const uint32_t, const uint32_t) {}
void keyboardRepeatInfo(void*, wl_keyboard*, const int32_t, const int32_t) {}

void keyboardKey(void* data, wl_keyboard*, const uint32_t, const uint32_t, const uint32_t key,
  const uint32_t state) {
  auto& wayland = *static_cast<WaylandState*>(data);
  if (state != WL_KEYBOARD_KEY_STATE_PRESSED || wayland.player == nullptr) return;

  Player& player = *wayland.player;
  const double frame = player.animation.frameRate() > 0.0 ? 1.0 / player.animation.frameRate() : 0.0;
  switch (key) {
    case 1: // KEY_ESC
      wayland.running = false;
      return;
    case 57: // KEY_SPACE
      player.paused = !player.paused;
      break;
    case 105: // KEY_LEFT
      player.elapsed = std::max(0.0, player.elapsed - frame);
      break;
    case 106: // KEY_RIGHT
      player.elapsed = std::min(static_cast<double>(player.animation.duration()), player.elapsed + frame);
      break;
    case 19: // KEY_R
      player.elapsed = 0.0;
      break;
    case 38: // KEY_L
      player.looping = !player.looping;
      break;
    default:
      return;
  }
  if (wayland.toplevel != nullptr) xdg_toplevel_set_title(wayland.toplevel, titleFor(player).c_str());
}

const wl_keyboard_listener kKeyboardListener = {
  keyboardKeymap, keyboardEnter, keyboardLeave, keyboardKey, keyboardModifiers, keyboardRepeatInfo
};

void seatCapabilities(void* data, wl_seat* seat, const uint32_t capabilities) {
  auto& state = *static_cast<WaylandState*>(data);
  const bool hasKeyboard = (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
  if (hasKeyboard && state.keyboard == nullptr) {
    state.keyboard = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(state.keyboard, &kKeyboardListener, &state);
  } else if (!hasKeyboard && state.keyboard != nullptr) {
    wl_keyboard_release(state.keyboard);
    state.keyboard = nullptr;
  }
}
void seatName(void*, wl_seat*, const char*) {}
const wl_seat_listener kSeatListener = { seatCapabilities, seatName };

void wmBasePing(void*, xdg_wm_base* wmBase, const uint32_t serial) {
  xdg_wm_base_pong(wmBase, serial);
}
const xdg_wm_base_listener kWmBaseListener = { wmBasePing };

void toplevelConfigure(void* data, xdg_toplevel*, const int32_t width, const int32_t height, wl_array*) {
  auto& state = *static_cast<WaylandState*>(data);
  if (width > 0) state.requestedWidth = static_cast<uint32_t>(width);
  if (height > 0) state.requestedHeight = static_cast<uint32_t>(height);
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
      state.failed = true;
      state.running = false;
      return;
    }

    graphics::gpu::WindowSurface::Configuration configuration;
    // Vulkan wants the wl_surface; EGL wants the wl_egl_window wrapping it.
    configuration.nativeWindow = state.surface;
    configuration.nativeWindowExtra = state.eglWindow;
    configuration.nativeDisplay = state.display;
    configuration.nativeWindowSystem = backend::NativeWindowSystem::Wayland;
    configuration.width = state.width;
    configuration.height = state.height;
    configuration.preferred = state.preferred;
    configuration.presentMode = state.immediate ? backend::PresentMode::Immediate : backend::PresentMode::Fifo;
    state.player->windowSurface = graphics::gpu::WindowSurface::make(configuration);
    if (state.player->windowSurface == nullptr) {
      std::fprintf(stderr, "lottie_player: no GPU device for this Wayland surface\n");
      state.failed = true;
      state.running = false;
      return;
    }
    if (state.preferred != backend::Backend::Null &&
        state.player->windowSurface->device()->getInfo().backend != state.preferred) {
      std::fprintf(stderr, "lottie_player: asked for %s, but the window fell back to %s\n",
        backend::toString(state.preferred),
        backend::toString(state.player->windowSurface->device()->getInfo().backend));
      state.failed = true;
      state.running = false;
      return;
    }
    printDevice(*state.player->windowSurface);
  } else {
    wl_egl_window_resize(state.eglWindow, static_cast<int>(state.width), static_cast<int>(state.height), 0, 0);
    if (!state.player->windowSurface->resize(state.width, state.height)) {
      std::fprintf(stderr, "lottie_player: resize failed: %s\n",
        state.player->windowSurface->diagnostics().c_str());
      state.failed = true;
      state.running = false;
      return;
    }
  }
  state.configured = true;
}
const xdg_surface_listener kXdgSurfaceListener = { xdgSurfaceConfigure };

void registryGlobal(void* data, wl_registry* registry, const uint32_t name, const char* interface,
  const uint32_t version) {
  auto& state = *static_cast<WaylandState*>(data);
  if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
    state.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name,
      &wl_compositor_interface, std::min(version, 4u)));
  } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
    state.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface,
      std::min(version, 7u)));
    wl_seat_add_listener(state.seat, &kSeatListener, &state);
  } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
    state.wmBase = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name,
      &xdg_wm_base_interface, std::min(version, 1u)));
    xdg_wm_base_add_listener(state.wmBase, &kWmBaseListener, &state);
  }
}
void registryGlobalRemove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistryListener = { registryGlobal, registryGlobalRemove };

void destroyWayland(WaylandState& state) {
  if (state.player != nullptr) state.player->windowSurface.reset();
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
  if (state.keyboard != nullptr) {
    wl_keyboard_release(state.keyboard);
    state.keyboard = nullptr;
  }
  if (state.seat != nullptr) {
    wl_seat_destroy(state.seat);
    state.seat = nullptr;
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

int runWayland(const Options& options, Player& player) {
  WaylandState state;
  state.player = &player;
  state.preferred = options.preferred;
  state.requestedWidth = static_cast<uint32_t>(options.width);
  state.requestedHeight = static_cast<uint32_t>(options.height);
  state.immediate = false;

  if (::access("/dev/dri", F_OK) != 0) {
    // Without a DRM node a fifo eglSwapBuffers can wait forever for a frame callback. The same
    // fallback as the main Linux demo keeps the example usable in a container.
    state.immediate = true;
    std::fprintf(stderr, "lottie_player: /dev/dri is not visible; presenting immediately\n");
  }

  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    std::fprintf(stderr, "lottie_player: no Wayland compositor (WAYLAND_DISPLAY is unset or unreachable)\n");
    return 77;
  }

  state.registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(state.registry, &kRegistryListener, &state);
  if (wl_display_roundtrip(state.display) < 0) {
    std::fprintf(stderr, "lottie_player: the Wayland registry could not be read\n");
    destroyWayland(state);
    return 1;
  }
  if (state.compositor == nullptr || state.wmBase == nullptr) {
    std::fprintf(stderr, "lottie_player: the compositor does not provide wl_compositor and xdg_wm_base\n");
    destroyWayland(state);
    return 1;
  }

  state.surface = wl_compositor_create_surface(state.compositor);
  state.xdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, state.surface);
  xdg_surface_add_listener(state.xdgSurface, &kXdgSurfaceListener, &state);
  state.toplevel = xdg_surface_get_toplevel(state.xdgSurface);
  xdg_toplevel_add_listener(state.toplevel, &kToplevelListener, &state);
  xdg_toplevel_set_title(state.toplevel, titleFor(player).c_str());
  xdg_toplevel_set_app_id(state.toplevel, "org.coral.graphics.lottie");
  wl_surface_commit(state.surface);

  const auto started = std::chrono::steady_clock::now();
  player.lastTick = started;
  bool fifo = true;
  bool software = false;
  bool detailsKnown = false;

  while (state.running && (player.frameLimit == 0 || player.presented < player.frameLimit)) {
    if (wl_display_dispatch_pending(state.display) < 0) break;
    if (wl_display_flush(state.display) < 0 && errno != EAGAIN) break;

    pollfd descriptor { wl_display_get_fd(state.display), POLLIN, 0 };
    const int ready = poll(&descriptor, 1, 0);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ready > 0 && wl_display_dispatch(state.display) < 0) break;

    if (!state.configured || state.player->windowSurface == nullptr) {
      pollfd wait { wl_display_get_fd(state.display), POLLIN, 0 };
      const int configuredReady = poll(&wait, 1, 16);
      if (configuredReady > 0 && wl_display_dispatch(state.display) < 0) break;
      if (configuredReady < 0 && errno != EINTR) break;
      continue;
    }

    if (!detailsKnown) {
      software = isSoftwareRenderer(state.player->windowSurface->describeDevice());
      fifo = state.player->windowSurface->presentMode() == backend::PresentMode::Fifo;
      detailsKnown = true;
    }

    const auto now = std::chrono::steady_clock::now();
    advance(player, std::chrono::duration<double>(now - player.lastTick).count());
    player.lastTick = now;
    if (!presentFrame(player)) break;

    if (++player.framesSinceTitle >= 15) {
      player.framesSinceTitle = 0;
      const int frame = static_cast<int>(std::lround(player.elapsed * player.animation.frameRate()));
      if (frame != player.titledFrame) {
        player.titledFrame = frame;
        xdg_toplevel_set_title(state.toplevel, titleFor(player).c_str());
      }
    }

    if (!fifo || software) {
      pollfd idle { wl_display_get_fd(state.display), POLLIN, 0 };
      const int idleReady = poll(&idle, 1, 8);
      if (idleReady > 0 && wl_display_dispatch(state.display) < 0) break;
      if (idleReady < 0 && errno != EINTR) break;
    }
  }

  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::printf("lottie_player: presented %d frame(s) in %.2f s (%.1f fps)\n", player.presented, elapsed,
    elapsed > 0.0 ? player.presented / elapsed : 0.0);
  const bool failed = state.failed || !state.configured || state.player->windowSurface == nullptr || player.failed;
  destroyWayland(state);
  return failed ? 1 : 0;
}

// ==================================================================================================
// Entry point
// ==================================================================================================

int run(const Options& options) {
  const std::filesystem::path path = resolveAnimation(options.animation);
  if (path.empty()) {
    std::fprintf(stderr, "lottie_player: no animation was found; pass --animation FILE\n");
    return 2;
  }
  std::string error;
  const std::string text = readFile(path, &error);
  if (!error.empty()) {
    std::fprintf(stderr, "lottie_player: %s\n", error.c_str());
    return 2;
  }
  if (options.verify) return verifyOffscreen(options, text, path);

  auto parsed = graphics::Lottie::parse(text, &error);
  if (!parsed) {
    std::fprintf(stderr, "lottie_player: %s: %s\n", path.string().c_str(), error.c_str());
    return 2;
  }

  Player player;
  player.animation = std::move(*parsed);
  player.frameLimit = options.frames;
  player.elapsed = options.startTime;
  player.paused = options.paused;

  std::printf("playing %s\n", path.string().c_str());
  std::printf("  composition : %.1fx%.1f\n", player.animation.width(), player.animation.height());
  std::printf("  duration    : %.3fs at %.3f fps\n", player.animation.duration(), player.animation.frameRate());
  std::printf("  window      : %dx%d\n", options.width, options.height);

  const std::string platform = options.platform;
  if (platform != "auto" && platform != "x11" && platform != "wayland") {
    std::fprintf(stderr, "lottie_player: unknown platform '%s' (expected auto, x11 or wayland)\n",
      platform.c_str());
    return 2;
  }
  if (platform == "x11" || (platform == "auto" && std::getenv("WAYLAND_DISPLAY") == nullptr)) {
    return runX11(options, player);
  }
  return runWayland(options, player);
}

} // namespace anonymous

int main(int argc, char** argv) {
  Options options;
  if (const int parsed = parseOptions(argc, argv, options); parsed != 0) {
    return parsed < 0 ? 0 : parsed;
  }
  return run(options);
}
