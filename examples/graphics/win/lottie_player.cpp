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
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file lottie_player.cpp
 *
 * A window that plays a Lottie animation.
 *
 * ## Why this is a separate program rather than part of the demo
 *
 * `graphics_demo` writes a PNG and runs anywhere: a build machine, a container, a session with no display. That is worth
 * keeping. A window needs a display and a message loop, and putting one inside the demo would make the demo fail on the
 * machines where it is most useful. So the still picture and the moving one are two programs over one library.
 *
 * ## The window is Win32, and that is a real limitation
 *
 * The project vendors no windowing toolkit -- `third_party/libsdl2` is an empty directory skeleton -- and the task asks
 * for no new dependency. So this uses the platform directly. Everything platform-specific is in this file and touches
 * nothing in `CoralGraphics`, which is what makes a second implementation for another platform a new file rather than a
 * change to the library.
 *
 * The three things the platform has to provide are a surface of pixels, a way to blit them, and a clock:
 *
 *   * a `graphics::Surface` owns the pixels, and `Pixmap::pixels()` hands them over as a `BGRA`-ordered buffer that a
 *     Win32 `DIB` section can point straight at, with no conversion and no copy;
 *   * `StretchDIBits` blits it, scaled, so the window can be resized;
 *   * `QueryPerformanceCounter` is the clock, because `GetTickCount` has a resolution of about 16 ms and a 60-frame
 *     animation would visibly stutter on it.
 *
 * ## Two presentation paths
 *
 * `--backend raster` (the default) is the one above: render into a composition-sized raster surface, then stretch the
 * pixels into the window. `--backend gpu` replaces both steps with `gpu::WindowSurface`: its swapchain is the target,
 * `beginFrame`/`present` own the frame, and the composition is fitted to the window with a canvas transform. The
 * animation and the `Canvas` calls are identical; only where the pixels come from changes. `--gpu-api gl|vulkan` picks
 * the backend, and `--frames N` presents N frames and exits, which is what makes a windowed run scriptable.
 *
 * ## The loop
 *
 * A `WM_TIMER` would be the obvious way to drive playback and is the wrong one: the system timer's finest resolution is
 * about 15 ms, which caps a smooth animation at well under 60 frames a second. Instead the loop runs as fast as it can,
 * asks the clock how much time has passed, and advances the animation by exactly that -- which is also what makes the
 * playback run at the right speed on a machine that cannot keep up, rather than in slow motion.
 *
 * While the window is being moved or resized the system enters a modal loop that starves this one. `WM_ENTERSIZEMOVE`
 * and `WM_EXITSIZEMOVE` bracket that so the animation pauses and resumes rather than jumping forward by the whole
 * duration of the drag.
 */

#include <lottie/Lottie.h>

#include <backend/Types.h>
#include <graphics/Pixmap.h>
#include <graphics/Surface.h>
#include <gpu/WindowSurface.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// The stub is forced by a build option so that it is compiled somewhere. On a Windows host `_WIN32` is always defined,
// so without this the branch below would never be built by anything.
#if defined(_WIN32) && !defined(CORAL_LOTTIE_PLAYER_STUB)

// `windows.h` defines `min` and `max` as macros, which breaks every `std::min` in this file and every header included
// after it. Defining `NOMINMAX` first is the supported way to ask for the real ones.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

/** What the window procedure needs to reach, passed through `lpCreateParams` and stored on the window. */
struct Player {
  graphics::Lottie animation;
  std::unique_ptr<graphics::Surface> surface;

  /**
   * The canvas over the surface, taken once and kept.
   *
   * `Surface::getCanvas` refuses a second call until the frame is ended by `snapshot` or `detach` -- so a caller that
   * asks per frame gets a null and draws into nothing. Keeping the one canvas is the arrangement the header describes
   * for exactly this case: a caller redrawing into the same pixels has one frame that never ends.
   */
  std::shared_ptr<graphics::Canvas> canvas;

  /**
   * The window-backed GPU surface, when `--backend gpu` was asked for.
   *
   * It owns the swapchain and presents itself; there is no `peekPixels`/`StretchDIBits` step on this
   * path. Null means the player is in its original raster mode.
   */
  std::unique_ptr<graphics::gpu::WindowSurface> windowSurface;

  /** The backend to ask for in GPU mode; `Null` means whatever the platform has. */
  backend::Backend preferred = backend::Backend::Null;

  /** True when frames go through `windowSurface` rather than the raster surface. */
  bool gpu = false;

  /** Stop after this many presented frames, for a run that has to finish without a human closing the window. */
  int frameLimit = 0;
  int presentedFrames = 0;

  /** Set when a frame could not be started or presented, so a scripted run does not wait forever. */
  bool failed = false;

  /** How far into the animation the picture is, in seconds. */
  double elapsed = 0;
  bool paused = false;
  bool looping = true;

  /** The clock reading at the start of the current frame, so a frame advances by how long the last one took. */
  LARGE_INTEGER lastTick {};
  LARGE_INTEGER frequency {};

  /** True between `WM_ENTERSIZEMOVE` and `WM_EXITSIZEMOVE`, when the system's own loop owns the thread. */
  bool dragging = false;

  /** The frame number the title was last written for, and how many frames to wait before writing it again. */
  int titledFrame = -1;
  int framesSinceTitle = 0;

  /** @return the seconds since the last call, and records the new time. */
  [[nodiscard]] double tick() {
    // Before `WM_CREATE` has run the frequency is zero, and dividing by it gives an infinity that would propagate
    // through the elapsed time into a frame number that is not one. Answering zero is right: no time has passed that
    // anything can measure.
    if (frequency.QuadPart == 0) {
      QueryPerformanceFrequency(&frequency);
      QueryPerformanceCounter(&lastTick);
      return 0.0;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double seconds = static_cast<double>(now.QuadPart - lastTick.QuadPart) /
                           static_cast<double>(frequency.QuadPart);
    lastTick = now;
    return seconds;
  }
};

/**
 * Renders the current moment into the player's surface and stretches it into @p device.
 *
 * The surface is the animation's own size, so the render is at the document's resolution and the scaling is the
 * window's -- which is the right way round: rendering at the window's size would re-rasterise every path on every
 * resize, and rendering at the composition's means the same pixels are stretched, as a video player does.
 */
void presentRaster(Player& player, const HDC device, const int width, const int height) {
  if (player.surface == nullptr) return;

  if (player.canvas == nullptr) return;

  // Clear first: the animation is drawn on transparency, and without this the previous frame shows through wherever the
  // new one is transparent -- which for a spinning gear is a smear rather than a trail.
  player.canvas->clear(graphics::Color::MakeRGBA8(24, 26, 36, 255));
  player.animation.render(*player.canvas, static_cast<graphics::Scalar>(player.elapsed));

  const graphics::Pixmap pixels = player.surface->peekPixels();
  if (pixels.isEmpty()) return;

  // The blit. `graphics` stores premultiplied `RGBA`, and Win32's 32-bit `DIB` layout is `BGRA` in memory -- which on a
  // little-endian machine reads a red-green-blue-alpha byte order as a blue-green-red-alpha word. So the channels line
  // up if the source is asked for as `BGRA` and the destination for 32-bit colour; nothing is swapped by hand and no
  // conversion buffer exists.
  BITMAPINFO info {};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = pixels.width();
  // A negative height means a top-down `DIB`, which is the order the surface's rows are in. A positive one would draw
  // the animation upside down.
  info.bmiHeader.biHeight = -pixels.height();
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;

  // Letterboxed: the animation keeps its aspect ratio whatever shape the window is, and the bars are left as the
  // cleared background rather than stretched into.
  const double scale = std::min(static_cast<double>(width) / pixels.width(),
                               static_cast<double>(height) / pixels.height());
  const int drawnWidth = std::max(1, static_cast<int>(pixels.width() * scale));
  const int drawnHeight = std::max(1, static_cast<int>(pixels.height() * scale));
  const int x = (width - drawnWidth) / 2;
  const int y = (height - drawnHeight) / 2;

  SetStretchBltMode(device, COLORONCOLOR);
  StretchDIBits(device, x, y, drawnWidth, drawnHeight, 0, 0, pixels.width(), pixels.height(),
                pixels.pixels(), &info, DIB_RGB_COLORS, SRCCOPY);
  ++player.presentedFrames;
}

/** The title, which is also where the player reports its frame rate and the keys. */
std::string titleFor(const Player& player) {
  std::string title = "Coral graphics - Lottie player";
  if (player.animation.isEmpty()) return title + " (no animation)";

  const int frame = static_cast<int>(std::lround(player.elapsed * player.animation.frameRate()));
  const int total = static_cast<int>(std::lround(player.animation.duration() * player.animation.frameRate()));
  title += " - frame " + std::to_string(frame) + " of " + std::to_string(total);
  if (player.paused) title += " [paused]";
  return title + "   (space pauses, left/right steps, R rewinds)";
}

/**
 * Moves the animation forward by @p seconds.
 *
 * Separate from drawing, and that separation is the point: this is called from the message loop, not from `WM_PAINT`. A
 * timeline advanced by paint messages stops whenever the system decides not to send one -- for a window that is hidden,
 * minimised, fully occluded, or on a session with no interactive desktop -- and a player frozen at frame zero looks
 * exactly like a player whose animation did not parse.
 */
void advance(Player& player, const double seconds) {
  if (player.paused || player.dragging) return;
  player.elapsed += seconds;

  const double duration = player.animation.duration();
  if (!(duration > 0)) return;
  if (player.looping) {
    // `fmod` rather than a subtraction: a frame that took longer than the whole animation -- which happens after a
    // breakpoint -- would still land in the right place, where a single subtraction would not.
    player.elapsed = std::fmod(player.elapsed, duration);
    if (player.elapsed < 0) player.elapsed += duration;
  } else {
    player.elapsed = std::min(player.elapsed, duration);
  }
}

/** Draws and presents one frame through the window-backed GPU surface. */
void presentGpu(Player& player) {
  if (!player.windowSurface) return;
  graphics::Canvas* canvas = player.windowSurface->beginFrame();
  if (canvas == nullptr) {
    player.failed = true;
    std::cerr << "the GPU surface refused a frame: " << player.windowSurface->diagnostics() << "\n";
    return;
  }

  canvas->clear(graphics::Color::MakeRGBA8(24, 26, 36, 255));

  // The window is the target, so the composition is fitted into it rather than rendered at the
  // composition's size and stretched by the CPU. Preserve the aspect ratio and centre the result: a
  // 640x640 window showing a 512x512 document should letterbox, not distort it.
  const auto windowWidth = static_cast<graphics::Scalar>(player.windowSurface->width());
  const auto windowHeight = static_cast<graphics::Scalar>(player.windowSurface->height());
  const auto animationWidth = static_cast<graphics::Scalar>(player.animation.width());
  const auto animationHeight = static_cast<graphics::Scalar>(player.animation.height());
  if (windowWidth <= 0.0f || windowHeight <= 0.0f || animationWidth <= 0.0f || animationHeight <= 0.0f) {
    return;
  }
  const graphics::Scalar scale = std::min(windowWidth / animationWidth, windowHeight / animationHeight);
  const graphics::Scalar offsetX = (windowWidth - animationWidth * scale) * 0.5f;
  const graphics::Scalar offsetY = (windowHeight - animationHeight * scale) * 0.5f;

  canvas->save();
  canvas->translate(offsetX, offsetY);
  canvas->scale(scale);
  player.animation.render(*canvas, static_cast<graphics::Scalar>(player.elapsed));
  canvas->restore();

  if (!player.windowSurface->present()) {
    player.failed = true;
    std::cerr << "the GPU surface could not present: " << player.windowSurface->diagnostics() << "\n";
    return;
  }
  ++player.presentedFrames;
}

/** Draws the current moment into @p window. Called from `WM_PAINT`, so it does not touch the clock. */
void drawFrame(Player& player, const HWND window) {
  if (player.gpu) {
    // `WM_PAINT` still has to be acknowledged even though the GPU path does not blit: without
    // `BeginPaint`/`EndPaint` the update region stays dirty, `PeekMessage` keeps returning `WM_PAINT`,
    // and the message pump never reaches the code that advances the timeline or honours `--frames`.
    PAINTSTRUCT paint;
    BeginPaint(window, &paint);
    presentGpu(player);
    EndPaint(window, &paint);
    return;
  }

  PAINTSTRUCT paint;
  const HDC device = BeginPaint(window, &paint);
  RECT client {};
  GetClientRect(window, &client);
  presentRaster(player, device, client.right - client.left, client.bottom - client.top);
  EndPaint(window, &paint);
}

LRESULT CALLBACK windowProc(const HWND window, const UINT message, const WPARAM wparam, const LPARAM lparam) {
  auto* player = reinterpret_cast<Player*>(GetWindowLongPtr(window, GWLP_USERDATA));

  switch (message) {
    case WM_CREATE: {
      const auto* create = reinterpret_cast<const CREATESTRUCT*>(lparam);
      player = static_cast<Player*>(create->lpCreateParams);
      SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(player));
      QueryPerformanceFrequency(&player->frequency);
      QueryPerformanceCounter(&player->lastTick);
      return 0;
    }

    case WM_PAINT:
      if (player != nullptr) drawFrame(*player, window);
      return 0;

    case WM_ERASEBKGND:
      // Nothing: `WM_PAINT` clears the surface it draws into, and letting the system erase as well makes the window
      // flicker between the two.
      return 1;

    case WM_ENTERSIZEMOVE:
      if (player != nullptr) player->dragging = true;
      return 0;

    case WM_EXITSIZEMOVE:
      if (player != nullptr) {
        player->dragging = false;
        // The clock is reset rather than left to catch up: the time spent dragging is not time the animation was
        // playing.
        QueryPerformanceCounter(&player->lastTick);
      }
      return 0;

    case WM_KEYDOWN:
      if (player != nullptr) {
        const double duration = player->animation.duration();
        const double frame = duration > 0 ? 1.0 / player->animation.frameRate() : 0.0;
        switch (wparam) {
          case VK_SPACE: player->paused = !player->paused; break;
          case VK_LEFT: player->elapsed = std::max(0.0, player->elapsed - frame); break;
          case VK_RIGHT: player->elapsed = std::min(duration, player->elapsed + frame); break;
          case 'R': player->elapsed = 0; break;
          case 'L': player->looping = !player->looping; break;
          case VK_ESCAPE: PostMessage(window, WM_CLOSE, 0, 0); return 0;
          default: break;
        }
        SetWindowText(window, titleFor(*player).c_str());
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;

    case WM_SIZE:
      if (player != nullptr && player->gpu && player->windowSurface != nullptr) {
        const UINT width = LOWORD(lparam);
        const UINT height = HIWORD(lparam);
        if (width > 0 && height > 0 &&
            (width != player->windowSurface->width() || height != player->windowSurface->height())) {
          if (!player->windowSurface->resize(width, height)) {
            player->failed = true;
            std::cerr << "the GPU surface could not resize: " << player->windowSurface->diagnostics() << "\n";
          }
        }
      }
      InvalidateRect(window, nullptr, FALSE);
      return 0;

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;

    default:
      break;
  }
  return DefWindowProc(window, message, wparam, lparam);
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
    std::vector<std::filesystem::path> candidates {
      directory / "resources" / "apitest" / "lottie" / "gear.json",
    };
    for (const std::filesystem::path& candidate : candidates) {
      std::error_code failure;
      if (std::filesystem::is_regular_file(candidate, failure)) return candidate;
    }
    directory = directory.parent_path();
  }
  return {};
}

} // namespace anonymous

/**
 * Parses the arguments and plays, returning a process exit code.
 *
 * A function rather than main because the Windows build is a GUI-subsystem program -- so that it does not open a
 * console window alongside the animation -- and the entry point the C runtime looks for in that subsystem is WinMain.
 * The two entry points below are then five lines each and the body is shared with the stub for other platforms.
 */
static int runPlayer(int argc, char** argv) {
  std::string requested;
  int windowWidth = 640;
  int windowHeight = 640;
  bool useGpu = false;
  backend::Backend preferred = backend::Backend::Null;
  int frameLimit = 0;
  double startTime = 0.0;
  bool paused = false;

  const auto parseBackend = [](const std::string& name, bool* recognised) {
    *recognised = true;
    if (name == "null") return backend::Backend::Null;
    if (name == "gl" || name == "opengl" || name == "gles") return backend::Backend::OpenGL;
    if (name == "vulkan") return backend::Backend::Vulkan;
    if (name == "metal") return backend::Backend::Metal;
    *recognised = false;
    return backend::Backend::Null;
  };

  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    const auto next = [&i, argc, argv](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << name << " needs a value\n";
        return {};
      }
      return argv[++i];
    };

    if (argument == "--animation" || argument == "-a") {
      requested = next("--animation");
    } else if (argument == "--size") {
      const std::string value = next("--size");
      const size_t cross = value.find('x');
      if (cross != std::string::npos) {
        windowWidth = std::max(64, std::atoi(value.substr(0, cross).c_str()));
        windowHeight = std::max(64, std::atoi(value.substr(cross + 1).c_str()));
      }
    } else if (argument == "--backend") {
      const std::string name = next("--backend");
      if (name == "raster" || name == "cpu" || name == "software") {
        useGpu = false;
      } else if (name == "gpu" || name == "hardware") {
        useGpu = true;
      } else {
        std::cerr << "unrecognised backend: " << name << " (expected raster or gpu)\n";
        return EXIT_FAILURE;
      }
    } else if (argument == "--gpu-api") {
      bool recognised = false;
      preferred = parseBackend(next("--gpu-api"), &recognised);
      if (!recognised) {
        std::cerr << "unrecognised GPU API (expected null, gl, vulkan or metal)\n";
        return EXIT_FAILURE;
      }
    } else if (argument == "--frames") {
      frameLimit = std::max(0, std::atoi(next("--frames").c_str()));
    } else if (argument == "--time") {
      startTime = std::max(0.0, std::atof(next("--time").c_str()));
    } else if (argument == "--paused") {
      paused = true;
    } else if (argument == "--help" || argument == "-h") {
      std::cout << "Coral graphics Lottie player\n\n"
                   "  lottie_player [options]\n\n"
                   "  --animation FILE  the Lottie JSON to play (default: resources/apitest/lottie/gear.json)\n"
                   "  --size WxH        the window's initial size (default: 640x640)\n"
                   "  --backend NAME    raster (default) or gpu -- which renderer presents the frames\n"
                   "  --gpu-api NAME    preferred backend: null, gl, vulkan or metal; may fall back\n"
                   "  --frames N        present N frames and exit (default: 0, until the window closes)\n"
                   "  --time SECONDS    start at this point on the timeline (for a reproducible still)\n"
                   "  --paused          start paused; useful with --time for a fixed frame\n"
                   "  --help            this text\n\n"
                   "While it is open: space pauses, the arrow keys step a frame, R rewinds, L toggles looping,\n"
                   "escape closes.\n";
      return 0;
    }
  }

  Player player;

  player.gpu = useGpu;
  player.preferred = preferred;
  player.frameLimit = frameLimit;
  player.elapsed = startTime;
  player.paused = paused;

  const std::filesystem::path path = resolveAnimation(requested);
  if (path.empty()) {
    std::cerr << "no animation was found; pass --animation FILE\n";
    return EXIT_FAILURE;
  }

  std::string error;
  const std::string text = readFile(path, &error);
  if (!error.empty()) {
    std::cerr << error << "\n";
    return EXIT_FAILURE;
  }

  auto animation = graphics::Lottie::parse(text, &error);
  if (!animation) {
    std::cerr << path.string() << ": " << error << "\n";
    return EXIT_FAILURE;
  }
  player.animation = std::move(*animation);

  // The raster surface is the composition's own size, rounded up: a fractional composition would
  // otherwise clip its last row. In GPU mode the target is the window and this surface is not created.
  const int surfaceWidth = std::max(1, static_cast<int>(std::ceil(player.animation.width())));
  const int surfaceHeight = std::max(1, static_cast<int>(std::ceil(player.animation.height())));
  if (!player.gpu) {
    player.surface = graphics::Surface::makeRaster(surfaceWidth, surfaceHeight);
    if (player.surface == nullptr) {
      std::cerr << "could not allocate a " << surfaceWidth << "x" << surfaceHeight << " surface\n";
      return EXIT_FAILURE;
    }

    // Taken once, here, and never again: see the note on the field.
    player.canvas = player.surface->getCanvas();
    if (player.canvas == nullptr) {
      std::cerr << "the surface refused a canvas\n";
      return EXIT_FAILURE;
    }
  }

  std::cout << "playing " << path.string() << "\n"
            << "  composition : " << surfaceWidth << "x" << surfaceHeight << "\n"
            << "  duration    : " << player.animation.duration() << "s at " << player.animation.frameRate()
            << " fps\n";

  const HINSTANCE instance = GetModuleHandle(nullptr);

  WNDCLASS windowClass {};
  windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
  windowClass.lpfnWndProc = windowProc;
  windowClass.hInstance = instance;
  windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
  windowClass.lpszClassName = "CoralLottiePlayer";
  if (RegisterClass(&windowClass) == 0) {
    std::cerr << "could not register the window class\n";
    return EXIT_FAILURE;
  }

  // `AdjustWindowRect` makes the *client* area the requested size, rather than the window including its border -- which
  // is the difference between a 640x640 picture and a 624x601 one.
  RECT frame {0, 0, windowWidth, windowHeight};
  AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);

  const HWND window = CreateWindowEx(0, windowClass.lpszClassName, titleFor(player).c_str(), WS_OVERLAPPEDWINDOW,
                                     CW_USEDEFAULT, CW_USEDEFAULT, frame.right - frame.left, frame.bottom - frame.top,
                                     nullptr, nullptr, instance, &player);
  if (window == nullptr) {
    std::cerr << "could not create the window\n";
    return EXIT_FAILURE;
  }

  if (player.gpu) {
    RECT client {};
    GetClientRect(window, &client);
    graphics::gpu::WindowSurface::Configuration configuration;
    configuration.nativeWindow = window;
    configuration.nativeDisplay = GetDC(window);
    configuration.width = static_cast<uint32_t>(std::max(1L, client.right - client.left));
    configuration.height = static_cast<uint32_t>(std::max(1L, client.bottom - client.top));
    configuration.preferred = player.preferred;
    configuration.presentMode = backend::PresentMode::Fifo;
    player.windowSurface = graphics::gpu::WindowSurface::make(configuration);
    if (player.windowSurface == nullptr) {
      std::cerr << "could not create the GPU window surface\n";
      return EXIT_FAILURE;
    }
    std::cout << "  backend     : " << player.windowSurface->describeDevice() << "\n";
  }

  ShowWindow(window, SW_SHOW);
  UpdateWindow(window);

  // ## The loop
  //
  // `PeekMessage` rather than `GetMessage`: the thread has to keep running when there is no message, because the next
  // frame is due whether or not anybody clicked anything. `GetMessage` blocks until there is one, which would animate
  // only as often as the mouse moved.
  for (;;) {
    MSG message;
    while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) return 0;
      TranslateMessage(&message);
      DispatchMessage(&message);
    }

    // The timeline is the loop's job, not the paint handler's. `tick` reports the real time the last iteration took, so
    // the playback runs at the right speed whatever the machine can manage -- on a slow one the frames are dropped rather
    // than the animation played in slow motion.
    advance(player, player.tick());

    // The title carries the frame number, which is the only way to see from outside that the playback is running rather
    // than showing a still. Four updates a second: often enough to look live, rare enough not to be a message per frame.
    const int frame = static_cast<int>(std::lround(player.elapsed * player.animation.frameRate()));
    if (++player.framesSinceTitle >= 15) {
      player.framesSinceTitle = 0;
      if (frame != player.titledFrame) {
        player.titledFrame = frame;
        SetWindowText(window, titleFor(player).c_str());
      }
    }

    // Repainting through the message queue rather than by drawing here keeps one path that draws: `WM_PAINT` arrives
    // when the queue is empty, which is every iteration.
    InvalidateRect(window, nullptr, FALSE);
    UpdateWindow(window);

    if (player.failed) return EXIT_FAILURE;
    if (player.frameLimit > 0 && player.presentedFrames >= player.frameLimit) return 0;

    // A short sleep, because a loop with no wait at all spins a core at 100% to draw at a rate the display cannot show.
    // One millisecond leaves the animation advancing by real elapsed time, so the playback stays correct.
    Sleep(1);
  }
}

/**
 * The GUI-subsystem entry point.
 *
 * __argc and __argv are the CRT's own parsed command line, which is what a GUI program has instead of the arguments
 * main would be handed. Using them rather than calling GetCommandLine and splitting it by hand keeps the quoting
 * rules in one place -- and they are the same rules the console build gets.
 */
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return runPlayer(__argc, __argv); }

#else // _WIN32 and not forced to the stub

/**
 * The player on a platform this file does not implement.
 *
 * A stub rather than a silent success, because a program that exits zero having played nothing is a program whose build
 * looks like it works. The graphics library itself is platform-independent; what is missing is a window, and saying so
 * is more useful than pretending.
 */
int main() {
  std::cerr << "lottie_player needs a window implementation for this platform.\n"
               "The library and the parser are portable; only the window is not. See lottie_player.cpp for the three\n"
               "things a port has to provide: a surface, a way to blit it, and a clock.\n";
  return EXIT_FAILURE;
}

#endif // _WIN32
