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

/*
 * ==================================================================================================
 * The Vulkan backend demo
 * ==================================================================================================
 *
 * One program with two jobs, because "the Vulkan backend draws" is a claim that needs both a picture and
 * a number:
 *
 *   1. **Windowed** (the default): a Win32 window presented through `gpu::WindowSurface`, drawing a scene
 *      every frame with the canvas's GPU path. This is what a client does; it is the demonstration.
 *
 *   2. **Verification** (`--verify`): the same scene rendered offscreen, read back, and compared against
 *      the colour each shape was drawn with. It prints every measurement and sets the exit code, so a
 *      machine with a Vulkan driver can answer "is the backend correct on *this* GPU" without a person
 *      looking at the window. It writes a PNG as well, which is how a disagreement gets looked at.
 *
 * The scene is deliberately made of the primitives the canvas translates differently from one another - a
 * solid rectangle, a circle (a tessellated path), a filled triangle (the stencil/fan route), a
 * translucent rectangle (blending) and a group drawn through `saveLayerAlpha` (an offscreen target
 * sampled back through a composite). A backend that mis-binds a descriptor set, flips a texture
 * coordinate or loses a layer shows up in at least one of the checked pixels.
 *
 *   vulkan_demo                 # a window, until Escape
 *   vulkan_demo --verify        # offscreen, checks, exit code
 *   vulkan_demo --verify --output out/vulkan
 *
 * Exit code 77 means "no Vulkan device here", the convention the other examples and the CTest
 * registrations use for a machine that cannot run the check rather than one that failed it.
 */

#include <gpu/Context.h>
#include <gpu/Surface.h>
#include <gpu/WindowSurface.h>

#include <backend/Device.h>

#include <graphics/Bitmap.h>
#include <graphics/Canvas.h>
#include <graphics/Color.h>
#include <graphics/Data.h>
#include <graphics/Font.h>
#include <graphics/Image.h>
#include <graphics/Paint.h>
#include <graphics/Path.h>
#include <graphics/Pixmap.h>
#include <graphics/Typeface.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using graphics::Canvas;
using graphics::Color;
using graphics::Paint;
using graphics::Path;
using graphics::Point;
using graphics::Rect;
using graphics::Scalar;

constexpr uint32_t kSize = 384;

/** The directory the executable lives in, so the emoji font is found when the demo is launched from a build directory. */
std::filesystem::path g_executableDirectory;

/** What the scene is drawn at, and where the verification looks. */
struct Expectation {
  const char* name;
  int x;
  int y;
  Color expected;
  int tolerance;
};

/** The colours the scene uses, named once so the expectations and the drawing cannot disagree. */
const Color kBackground = Color::MakeRGBA8(20, 30, 40, 255);
const Color kRed = Color::MakeRGBA8(220, 40, 40, 255);
const Color kGreen = Color::MakeRGBA8(40, 200, 80, 255);
const Color kYellow = Color::MakeRGBA8(240, 200, 40, 255);
const Color kWhite = Color::MakeRGBA8(255, 255, 255, 128);

/** @return the premultiplied ARGB pixel at (@p x, @p y), or 0 when outside the bitmap. */
uint32_t pixelAt(const graphics::Bitmap& bitmap, const int x, const int y) {
  if (bitmap.isEmpty()) return 0;
  if (x < 0 || y < 0 || x >= bitmap.width() || y >= bitmap.height()) return 0;
  return bitmap.makePixmap().getPixelARGB32(x, y);
}

struct Rgba {
  uint32_t r = 0;
  uint32_t g = 0;
  uint32_t b = 0;
  uint32_t a = 0;
};

Rgba unpack(const uint32_t argb) {
  Rgba value;
  // `getPixelARGB32` is (A << 24) | (R << 16) | (G << 8) | B, which is *not* the byte order of a
  // little-endian R8G8B8A8 readback; this demo reads through the bitmap, so it uses ARGB's order.
  value.a = (argb >> 24) & 0xFFu;
  value.r = (argb >> 16) & 0xFFu;
  value.g = (argb >> 8) & 0xFFu;
  value.b = argb & 0xFFu;
  return value;
}

/** @return the result of `source` (straight alpha) drawn over `destination` with alpha @p alpha. */
Color blend(const Color& source, const Color& destination, const double alpha) {
  const auto mix = [alpha](const float from, const float to) {
    return static_cast<float>(from * alpha + to * (1.0 - alpha));
  };
  return Color(mix(source.red, destination.red), mix(source.green, destination.green),
          mix(source.blue, destination.blue), 1.0f);
}

/**
 * Draws the demo scene.
 *
 * @param canvas the frame's canvas.
 * @param time   seconds since the demo started, used for the one animated element. The verified pixels
 *               are all in static shapes; the rotation is there so a window shows that frames advance.
 * @param emojiFont the colour-emoji font, or null when the demo could not find one. Colour emoji are
 *               `CBDT` bitmaps with no outline at all, so they are the one part of the scene that
 *               exercises `Canvas::drawText`'s bitmap-glyph path rather than its outline path.
 */
void drawScene(Canvas& canvas, const double time, const graphics::Font* emojiFont = nullptr) {
  canvas.clear(kBackground);

  Paint red;
  red.color = kRed;
  canvas.drawRect(Rect::MakeLTRB(16.0f, 16.0f, 80.0f, 80.0f), red);

  Paint green;
  green.color = kGreen;
  canvas.drawCircle(180.0f, 48.0f, 28.0f, green);

  Path triangle;
  triangle.moveTo(Point { 128.0f, 96.0f });
  triangle.lineTo(Point { 96.0f, 160.0f });
  triangle.lineTo(Point { 160.0f, 160.0f });
  triangle.close();
  Paint yellow;
  yellow.color = kYellow;
  canvas.drawPath(triangle, yellow);

  // Straight source-over with alpha 128: the expected result is computed by `blend` rather than guessed,
  // so a wrong blend unit shows up as a number rather than as an opinion about the picture.
  Paint translucent;
  translucent.color = kWhite;
  canvas.drawRect(Rect::MakeLTRB(24.0f, 180.0f, 120.0f, 230.0f), translucent);

  // A group at half alpha. It renders into an offscreen target this backend has to sample back, which is
  // the path a wrong texture coordinate or a lost layer breaks.
  canvas.saveLayerAlpha(Rect::MakeLTRB(150.0f, 170.0f, 230.0f, 230.0f), 128);
  Paint layerRed;
  layerRed.color = kRed;
  canvas.drawRect(Rect::MakeLTRB(150.0f, 170.0f, 230.0f, 230.0f), layerRed);
  canvas.restore();

  // The colour-emoji row. Six `CBDT` glyphs at 44 px: their pixels go through the canvas as images,
  // so a backend that draws outlined text correctly can still lose every one of them.
  if (emojiFont != nullptr) {
    Paint white;
    white.color = Color::MakeRGBA8(255, 255, 255, 255);
    canvas.drawText("🤡👻🐠🤩😃🤪", 12.0f, 300.0f, *emojiFont, white);
  }

  // The animated element: a small blue square orbiting the centre. It is not part of the checks; it is
  // what makes the window demonstrate that a new frame is actually presented.
  Paint blue;
  blue.color = Color::MakeRGBA8(80, 140, 255, 255);
  const auto centreX = static_cast<float>(128.0 + 56.0 * std::cos(time));
  const auto centreY = static_cast<float>(128.0 + 56.0 * std::sin(time));
  canvas.drawRect(Rect::MakeLTRB(centreX - 8.0f, centreY - 8.0f, centreX + 8.0f, centreY + 8.0f), blue);
}

/** @return every check the scene supports, with the colour each location must hold. */
std::vector<Expectation> expectations() {
  return {
    { "the background away from every shape", 4, 4, kBackground, 2 },
    { "the solid rectangle", 32, 32, kRed, 2 },
    { "the circle's centre", 180, 48, kGreen, 4 },
    { "the triangle's interior", 128, 150, kYellow, 4 },
    { "the translucent rectangle over the background", 40, 200, blend(kWhite, kBackground, 128.0 / 255.0), 6 },
    { "the half-alpha layer over the background", 190, 200, blend(kRed, kBackground, 128.0 / 255.0), 8 },
    // The animated square is static at time 0, so it is checked too: it is drawn *after* the layer, which
    // is the part of the order that would catch a restored clip or a stale target.
    { "the animated square at t = 0", 184, 128, Color::MakeRGBA8(80, 140, 255, 255), 3 },
  };
}

bool matches(const Rgba& pixel, const Color& expected, const int tolerance) {
  // Not `near`: `<windows.h>` defines that as an empty macro for 16-bit compatibility, so the
  // declaration would vanish into `const auto = [...]`.
  const auto within = [tolerance](const uint32_t actual, const float want) {
    const auto value = static_cast<int>(std::lround(want * 255.0f));
    return std::abs(static_cast<int>(actual) - value) <= tolerance;
  };
  return within(pixel.r, expected.red) && within(pixel.g, expected.green) && within(pixel.b, expected.blue);
}

bool writePng(const graphics::Bitmap& pixels, const std::filesystem::path& path) {
  const graphics::Image image = graphics::Image::fromBitmap(pixels);
  std::string error;
  const graphics::Data encoded = image.encode(graphics::ImageFormat::PNG, 100, &error);
  if (encoded.size() == 0) {
    std::printf("  could not encode %s: %s\n", path.filename().string().c_str(), error.c_str());
    return false;
  }
  std::error_code ignored;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ignored);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file.good()) return false;
  file.write(reinterpret_cast<const char*>(encoded.bytes()), static_cast<std::streamsize>(encoded.size()));
  std::printf("  wrote %-40s %7zu bytes\n", path.filename().string().c_str(), encoded.size());
  return true;
}

void printDevice(const backend::Device& device) {
  const backend::DeviceInfo& info = device.getInfo();
  std::printf("  backend : %s\n", info.backendName != nullptr ? info.backendName : "?");
  std::printf("  device  : %s (%s)\n", info.deviceName != nullptr ? info.deviceName : "?",
          info.vendor != nullptr ? info.vendor : "?");
  std::printf("  api     : %s, driver %s\n", info.apiVersion != nullptr ? info.apiVersion : "?",
          info.driverVersion != nullptr ? info.driverVersion : "?");
}

/**
 * @return the first colour-emoji font that exists beside this build, or an empty string.
 *
 * The candidates cover the places this demo is run from: the repository root
 * (`modules/graphics/resources/...`), `modules/graphics` itself (`resources/...`, for a manual run), and a
 * build directory one or more levels below the root (which is where CTest starts it). A demo that
 * silently skipped the emoji check because it looked in one place would be a check that passes for
 * the wrong reason.
 */
std::string findEmojiFont() {
  const std::vector<std::filesystem::path> candidates = {
    std::filesystem::path("resources") / "fonts" / "NotoColorEmoji.ttf",
    std::filesystem::path("modules/graphics/resources") / "fonts" / "NotoColorEmoji.ttf",
    std::filesystem::path("../modules/graphics/resources") / "fonts" / "NotoColorEmoji.ttf",
    std::filesystem::path("../../modules/graphics/resources") / "fonts" / "NotoColorEmoji.ttf",
    std::filesystem::path("../../../modules/graphics/resources") / "fonts" / "NotoColorEmoji.ttf",
  };
  for (const std::filesystem::path& candidate: candidates) {
    if (std::filesystem::exists(candidate)) return candidate.string();
  }
  if (!g_executableDirectory.empty()) {
    // `out/examples/bin/Release` and `out/examples/Release` are both plausible places to run the
    // executable from; walk up to the repository root and try the source resource directory.
    for (const char* up: { "../../../../", "../../../" }) {
      const std::filesystem::path candidate =
        g_executableDirectory / up / "modules" / "graphics" / "resources" / "fonts" / "NotoColorEmoji.ttf";
      if (std::filesystem::exists(candidate)) return candidate.lexically_normal().string();
    }
  }
  return {};
}

/** What the colour-emoji row covers, measured rather than assumed. */
struct EmojiInk {
  size_t coloured = 0;
  size_t distinct = 0;
  int left = static_cast<int>(kSize);
  int right = 0;
};

/** @return the coloured pixels in the lower strip, where the emoji row is drawn. */
EmojiInk measureEmoji(const graphics::Bitmap& pixels) {
  EmojiInk ink;
  bool seen[4096] = {};
  const int left = 8;
  const int right = static_cast<int>(kSize) - 8;
  const int top = 240;
  const int bottom = static_cast<int>(kSize) - 8;
  for (int y = top; y < bottom && y < pixels.height(); ++y) {
    for (int x = left; x < right && x < pixels.width(); ++x) {
      const Rgba value = unpack(pixelAt(pixels, x, y));
      if (value.a < 200) continue;
      const int maximum = static_cast<int>(std::max({ value.r, value.g, value.b }));
      const int minimum = static_cast<int>(std::min({ value.r, value.g, value.b }));
      // The background is a blue-grey whose channels differ by 20; a colour glyph is far past that.
      if (maximum - minimum < 32) continue;
      ++ink.coloured;
      seen[((value.r >> 4) << 8) | ((value.g >> 4) << 4) | (value.b >> 4)] = true;
      ink.left = std::min(ink.left, x);
      ink.right = std::max(ink.right, x);
    }
  }
  for (const bool entry: seen) {
    if (entry) ++ink.distinct;
  }
  return ink;
}

/** The offscreen verification path. @return the process exit code. */
int verify(const std::filesystem::path& output, const bool validation) {
  graphics::gpu::SurfaceConfiguration configuration;
  configuration.width = kSize;
  configuration.height = kSize;
  configuration.preferred = backend::Backend::Vulkan;

  // `SurfaceConfiguration` has no validation switch, so a validating run creates the device through a
  // `gpu::Context` and lends it to the surface. That is also the public API's answer to "I already have a
  // device": `SurfaceConfiguration::device`.
  std::shared_ptr<graphics::gpu::Context> validatingContext;
  if (validation) {
    graphics::gpu::ContextConfiguration contextConfiguration;
    contextConfiguration.preferred = graphics::gpu::ContextConfiguration::Preferred::Vulkan;
    contextConfiguration.allowFallback = false;
    contextConfiguration.enableValidation = true;
    contextConfiguration.applicationName = "vulkan-demo";
    validatingContext = graphics::gpu::Context::create(contextConfiguration);
    if (validatingContext == nullptr || !validatingContext->valid()) {
      std::printf("vulkan_demo: validation was requested but a validating device could not be created\n");
      return 77;
    }
    configuration.device = validatingContext->device();
  }

  const auto surface = graphics::gpu::Surface::make(configuration);
  if (surface == nullptr) {
    std::printf("vulkan_demo: no Vulkan device could be brought up here (exit 77)\n");
    return 77;
  }
  // `SurfaceConfiguration` has no `allowFallback`, so a build with both backends falls through to
  // OpenGL if Vulkan cannot come up. Refusing here is what keeps this program a Vulkan check: reporting
  // success on another backend would be the most misleading possible result.
  if (surface->device() == nullptr || surface->device()->getInfo().backend != backend::Backend::Vulkan) {
    std::printf("vulkan_demo: the device that came up is not Vulkan (exit 77)\n");
    return 77;
  }

  std::printf("vulkan_demo --verify\n");
  printDevice(*surface->device());

  // The emoji row needs a real colour font: NotoColorEmoji is `CBDT`, so it is also the only part of
  // this scene that proves the bitmap-glyph path reached the GPU rather than the outline path.
  const std::string emojiFontPath = findEmojiFont();
  std::shared_ptr<graphics::Font> emojiFont;
  if (!emojiFontPath.empty()) {
    const auto typeface = graphics::Typeface::fromFile(emojiFontPath);
    if (typeface != nullptr) emojiFont = graphics::Font::create(typeface, 44.0f);
  }
  if (emojiFont != nullptr) {
    std::printf("  emoji font: %s\n", emojiFontPath.c_str());
  } else {
    std::printf("  [skip] colour emoji: NotoColorEmoji.ttf was not found beside this build\n");
  }

  Canvas* canvas = surface->beginFrame();
  if (canvas == nullptr) {
    std::printf("vulkan_demo: the surface refused a frame\n");
    return 1;
  }
  drawScene(*canvas, 0.0, emojiFont.get());
  if (!surface->submit()) {
    std::printf("vulkan_demo: the frame was refused: %s\n", surface->diagnostics().c_str());
    return 1;
  }

  const graphics::Bitmap pixels = surface->snapshot();
  if (pixels.isEmpty()) {
    std::printf("vulkan_demo: the frame could not be read back\n");
    return 1;
  }

  int failures = 0;
  for (const Expectation& expectation: expectations()) {
    const Rgba actual = unpack(pixelAt(pixels, expectation.x, expectation.y));
    const bool ok = matches(actual, expectation.expected, expectation.tolerance);
    std::printf("  %-4s %-52s (%3d,%3d) = rgba(%3u,%3u,%3u) expected rgba(%3u,%3u,%3u) +/-%d\n",
            ok ? "ok" : "FAIL", expectation.name, expectation.x, expectation.y, actual.r, actual.g, actual.b,
            static_cast<uint32_t>(std::lround(expectation.expected.red * 255.0f)),
            static_cast<uint32_t>(std::lround(expectation.expected.green * 255.0f)),
            static_cast<uint32_t>(std::lround(expectation.expected.blue * 255.0f)), expectation.tolerance);
    if (!ok) ++failures;
  }

  // A scene where every pixel is the background is a scene that drew nothing, and every single-pixel
  // check above can pass while that is true. The count is the guard against it.
  int distinctColours = 0;
  {
    uint32_t seen[8] = {};
    for (int y = 2; y < static_cast<int>(kSize); y += 7) {
      for (int x = 2; x < static_cast<int>(kSize); x += 7) {
        const uint32_t colour = pixelAt(pixels, x, y);
        bool known = false;
        for (uint32_t& entry: seen) {
          if (entry == colour && colour != 0) {
            known = true;
            break;
          }
        }
        if (!known && distinctColours < 8) seen[distinctColours++] = colour;
      }
    }
  }
  std::printf("  %-4s the frame holds more than one colour          distinct sampled colours = %d\n",
          distinctColours > 2 ? "ok" : "FAIL", distinctColours);
  if (distinctColours <= 2) ++failures;

  if (emojiFont != nullptr) {
    const EmojiInk emoji = measureEmoji(pixels);
    const int span = emoji.right - emoji.left;
    const bool ok = emoji.coloured > 2000 && emoji.distinct > 12 && span > 200;
    std::printf("  %-4s colour emoji produced ink                    %zu coloured pixels, %zu colours, span %d\n",
            ok ? "ok" : "FAIL", emoji.coloured, emoji.distinct, span);
    if (!ok) ++failures;
  }

  if (!output.empty()) writePng(pixels, output / "vulkan_demo.png");

  if (failures != 0) {
    std::printf("vulkan_demo: %d check(s) failed\n", failures);
    return 1;
  }
  std::printf("vulkan_demo: all checks passed\n");
  return 0;
}

#if defined(_WIN32)

/**
 * The windowed demonstration.
 *
 * A plain native window plus `gpu::WindowSurface`: the backend owns the swapchain, `present` presents,
 * and a resize re-creates the window target. Nothing here reaches into the RHI directly, which is the
 * point - this is the path a client uses.
 *
 * @return the process exit code.
 */
int runWindow(const backend::PresentMode presentMode, const uint32_t frames) {
  const wchar_t* kClassName = L"coral-vulkan-demo";
  WNDCLASSEXW windowClass{};
  windowClass.cbSize = sizeof(windowClass);
  windowClass.style = CS_HREDRAW | CS_VREDRAW;
  windowClass.lpfnWndProc = DefWindowProcW;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.lpszClassName = kClassName;
  windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
  RegisterClassExW(&windowClass);

  RECT desired { 0, 0, 640, 480 };
  AdjustWindowRect(&desired, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowExW(0, kClassName, L"Coral Vulkan backend demo", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
          CW_USEDEFAULT, desired.right - desired.left, desired.bottom - desired.top, nullptr, nullptr,
          windowClass.hInstance, nullptr);
  if (window == nullptr) {
    std::printf("vulkan_demo: the window could not be created\n");
    return 1;
  }
  ShowWindow(window, SW_SHOW);

  graphics::gpu::WindowSurface::Configuration configuration;
  configuration.nativeWindow = window;
  configuration.nativeDisplay = GetDC(window);
  configuration.width = 640;
  configuration.height = 480;
  configuration.preferred = backend::Backend::Vulkan;
  configuration.presentMode = presentMode;

  const auto surface = graphics::gpu::WindowSurface::make(configuration);
  if (surface == nullptr) {
    std::printf("vulkan_demo: no window surface could be created (no Vulkan device?)\n");
    return 77;
  }
  if (surface->device() == nullptr || surface->device()->getInfo().backend != backend::Backend::Vulkan) {
    std::printf("vulkan_demo: the window came up on a backend other than Vulkan (exit 77)\n");
    return 77;
  }

  std::printf("vulkan_demo: %s\n", surface->describeDevice().c_str());
  std::printf("  present mode: %s\n",
          surface->presentMode() == backend::PresentMode::Mailbox
                  ? "mailbox"
                  : surface->presentMode() == backend::PresentMode::Immediate ? "immediate" : "fifo");
  std::printf("  Escape closes the window; V verifies the current frame and prints the measurements\n");

  const std::string emojiFontPath = findEmojiFont();
  std::shared_ptr<graphics::Font> emojiFont;
  if (!emojiFontPath.empty()) {
    const auto typeface = graphics::Typeface::fromFile(emojiFontPath);
    if (typeface != nullptr) emojiFont = graphics::Font::create(typeface, 44.0f);
  }
  if (emojiFont == nullptr) {
    std::printf("  [skip] colour emoji: NotoColorEmoji.ttf was not found beside this build\n");
  }

  const LARGE_INTEGER frequency = [] {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value;
  }();
  const LARGE_INTEGER start = [] {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value;
  }();

  uint32_t presented = 0;
  bool running = true;
  while (running) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) running = false;
      if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) running = false;
      if (message.message == WM_KEYDOWN && message.wParam == 'V') {
        graphics::Bitmap pixels;
        if (surface->readback(&pixels)) {
          int failures = 0;
          for (const Expectation& expectation: expectations()) {
            const Rgba actual = unpack(pixelAt(pixels, expectation.x, expectation.y));
            const bool ok = matches(actual, expectation.expected, expectation.tolerance);
            std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", expectation.name);
            if (!ok) ++failures;
          }
          std::printf("  verification of the presented frame: %s\n",
                  failures == 0 ? "all checks passed" : "some checks failed");
        }
      }
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }

    // Follow the client area: a resized window has to be a resized surface, and the backend refuses a
    // frame whose target no longer matches.
    RECT client{};
    GetClientRect(window, &client);
    if (client.right > 0 && client.bottom > 0 &&
        (static_cast<uint32_t>(client.right) != surface->width() ||
                static_cast<uint32_t>(client.bottom) != surface->height())) {
      surface->resize(static_cast<uint32_t>(client.right), static_cast<uint32_t>(client.bottom));
    }

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const double seconds = static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);

    Canvas* canvas = surface->beginFrame();
    if (canvas == nullptr) {
      std::printf("vulkan_demo: the frame could not start: %s\n", surface->diagnostics().c_str());
      return 1;
    }
    drawScene(*canvas, seconds, emojiFont.get());
    if (!surface->present()) {
      std::printf("vulkan_demo: the frame could not be presented: %s\n", surface->diagnostics().c_str());
      return 1;
    }
    ++presented;
    if (frames != 0 && presented >= frames) running = false;
    Sleep(1);
  }

  std::printf("vulkan_demo: presented %u frame(s), last frame took %.3f ms\n", presented,
          surface->lastFrameStages().total);
  ReleaseDC(window, static_cast<HDC>(configuration.nativeDisplay));
  DestroyWindow(window);
  return 0;
}

#else

int runWindow(backend::PresentMode, uint32_t) {
  std::printf("vulkan_demo: the window path is Windows-only in this example; use --verify for the offscreen check\n");
  return 77;
}

#endif

/** Parses `--present <mode>`; returns Fifo when absent or unknown. */
backend::PresentMode presentModeFor(const std::string& name) {
  if (name == "immediate") return backend::PresentMode::Immediate;
  if (name == "mailbox") return backend::PresentMode::Mailbox;
  return backend::PresentMode::Fifo;
}

} // namespace anonymous

int main(int argc, char** argv) {
  if (argc > 0 && argv[0] != nullptr) {
    std::error_code error;
    const std::filesystem::path executable = std::filesystem::absolute(argv[0], error);
    if (!error) g_executableDirectory = executable.parent_path();
  }
  bool offscreen = false;
  bool validation = false;
  std::filesystem::path output;
  backend::PresentMode presentMode = backend::PresentMode::Fifo;
  uint32_t frames = 0;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index] != nullptr ? argv[index] : "";
    if (argument == "--verify" || argument == "--only-checks") {
      offscreen = true;
    } else if (argument == "--window") {
      offscreen = false;
    } else if (argument == "--validation") {
      validation = true;
    } else if (argument == "--output" && index + 1 < argc) {
      output = argv[++index];
    } else if (argument == "--present" && index + 1 < argc) {
      presentMode = presentModeFor(argv[++index]);
    } else if (argument == "--frames" && index + 1 < argc) {
      frames = static_cast<uint32_t>(std::strtoul(argv[++index], nullptr, 10));
    } else if (argument == "--help" || argument == "-h") {
      std::printf("vulkan_demo [--verify] [--validation] [--output <dir>] [--present fifo|immediate|mailbox] [--frames N]\n\n"
                  "  --verify      render offscreen, check the pixels, write a PNG, and set the exit code\n"
                  "  --validation  ask the backend for driver validation and debug output\n"
                  "  --window      the default: open a window and present frames until Escape\n"
                  "  --output      where the verification PNG is written\n"
                  "  --present     requested swapchain mode; the granted one is printed\n"
                  "  --frames      stop the window after N frames (0 runs until Escape)\n");
      return 0;
    }
  }

  return offscreen ? verify(output, validation) : runWindow(presentMode, frames);
}
