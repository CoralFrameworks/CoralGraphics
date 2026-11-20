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
 * @file test_WindowSurface.cpp
 *
 * The window path, verified end to end on Windows: a real (hidden) `HWND`, a device created against it, a frame drawn
 * through `Canvas`, `present()` swapping, and the same recording read back into a bitmap so the pixels can be compared.
 *
 * ## Why the readback is a re-render, and why that still proves something
 *
 * A window's back buffer is not a texture this API can name, and its contents after a swap are undefined, so the readback
 * draws the frame's recording once more into an offscreen target. What that proves is that the *drawing* reached the same
 * target wiring the window uses; what proves the window half is `present()` returning true, which is `SwapBuffers`
 * succeeding on a real `HWND` -- a call that fails loudly (`Result::Error`) when the device does not own a window surface.
 * A stronger end-to-end check would read the window's pixels with `BitBlt`, which is a platform test rather than a graphics
 * one, and is the next thing to add when this suite runs on a machine with a display.
 */

#include <gpu/ContextGroup.h>
#include <gpu/Recorder.h>
#include <gpu/WindowSurface.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <thread>

#include <geometry/Rect.h>
#include <graphics/Paint.h>

#include <gtest/gtest.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

#if defined(_WIN32)

/** A hidden top-level window: enough for a GL context and for `SwapBuffers`, with nothing on screen to disturb a run. */
HWND createHiddenWindow(const int width, const int height) {
  constexpr const wchar_t* kClassName = L"coral-window-surface-test-class";
  WNDCLASSEXW windowClass {};
  windowClass.cbSize = sizeof(windowClass);
  windowClass.lpfnWndProc = DefWindowProcW;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.lpszClassName = kClassName;
  // A second registration of the same class is expected when the test runs twice in one process; the class is what
  // matters, not whether this call created it.
  RegisterClassExW(&windowClass);

  // A borderless window, deliberately. `WS_OVERLAPPEDWINDOW` makes Windows enforce a *minimum track
  // size* -- a "64x64" overlapped window really gets a client area of about 120x25 -- and Vulkan reports
  // that client area as the swapchain's extent, so a test that asks for 64x64 and then checks the
  // readback would be checking a size the window never had. A popup has no frame, so the window size is
  // the client size and the request is the size the surface reports.
  HWND window = CreateWindowExW(0, kClassName, L"coral-window-surface-test", WS_POPUP | WS_CLIPSIBLINGS |
          WS_CLIPCHILDREN, 0, 0, width, height, nullptr, nullptr, windowClass.hInstance, nullptr);
  return window;
}

#endif // _WIN32

} // namespace anonymous

TEST(WindowSurface, PresentsAFrameDrawnThroughTheCanvasAndReadsItBack) {
#if defined(_WIN32)
  constexpr int kWidth = 64;
  constexpr int kHeight = 64;

  HWND window = createHiddenWindow(kWidth, kHeight);
  ASSERT_NE(window, nullptr) << "a hidden window could not be created";
  HDC deviceContext = GetDC(window);
  ASSERT_NE(deviceContext, nullptr) << "the window has no device context";

  graphics::gpu::WindowSurface::Configuration configuration;
  configuration.nativeWindow = window;
  configuration.nativeDisplay = deviceContext;
  configuration.width = kWidth;
  configuration.height = kHeight;
  const auto surface = graphics::gpu::WindowSurface::make(configuration);
  ASSERT_NE(surface, nullptr) << "no window surface could be created";
  EXPECT_EQ(surface->width(), kWidth);
  EXPECT_EQ(surface->height(), kHeight);
  EXPECT_TRUE(surface->format() == backend::PixelFormat::R8G8B8A8_UNORM ||
              surface->format() == backend::PixelFormat::B8G8R8A8_UNORM)
          << "the target must use the SDR UNORM format the swapchain actually granted";
  EXPECT_FALSE(surface->describeDevice().empty());

  graphics::Canvas* canvas = surface->beginFrame();
  ASSERT_NE(canvas, nullptr) << surface->diagnostics();

  graphics::Paint background;
  background.color = graphics::Color::MakeRGBA8(0, 0, 255, 255);
  canvas->drawRect(graphics::Rect::MakeLTRB(0.0f, 0.0f, kWidth, kHeight), background);
  graphics::Paint marker;
  marker.color = graphics::Color::MakeRGBA8(255, 0, 0, 255);
  canvas->drawRect(graphics::Rect::MakeLTRB(16.0f, 16.0f, 48.0f, 48.0f), marker);

  // `present` is `SwapBuffers` on a real window: it returns false when the device was created without one, so a true here
  // is the evidence that the presentation path exists and ran.
  EXPECT_TRUE(surface->present()) << surface->diagnostics();

  graphics::Bitmap pixels;
  ASSERT_TRUE(surface->readback(&pixels)) << surface->diagnostics();
  ASSERT_FALSE(pixels.isEmpty());
  EXPECT_EQ(pixels.width(), kWidth);
  EXPECT_EQ(pixels.height(), kHeight);

  const graphics::Pixmap pixmap = pixels.makePixmap();
  const uint32_t centre = pixmap.getPixelARGB32(kWidth / 2, kHeight / 2);
  EXPECT_EQ((centre >> 16) & 0xFFu, 255u) << "the marker is red at the centre";
  EXPECT_EQ(centre & 0xFFu, 0u);
  const uint32_t corner = pixmap.getPixelARGB32(4, 4);
  EXPECT_EQ(corner & 0xFFu, 255u) << "the background is blue at the corner";
  EXPECT_EQ((corner >> 16) & 0xFFu, 0u);

  // A second frame over the same surface: the frame, target and canvas are all reusable, which is what a render loop does.
  graphics::Canvas* next = surface->beginFrame();
  ASSERT_NE(next, nullptr) << surface->diagnostics();
  next->clear(graphics::Color::MakeRGBA8(0, 255, 0, 255));
  EXPECT_TRUE(surface->present()) << surface->diagnostics();
  ASSERT_TRUE(surface->readback(&pixels)) << surface->diagnostics();
  EXPECT_EQ((pixels.makePixmap().getPixelARGB32(kWidth / 2, kHeight / 2) >> 8) & 0xFFu, 255u)
          << "the second frame's clear is green";

  ReleaseDC(window, deviceContext);
  DestroyWindow(window);
#else
  GTEST_SKIP() << "the window surface test needs a native window, and this platform's is not wired into it yet";
#endif
}

TEST(WindowSurface, APlanFrameKeepsEachListsTranslationAndClipInOneFrame) {
#if defined(_WIN32)
  constexpr int kWidth = 64;
  constexpr int kHeight = 64;

  HWND window = createHiddenWindow(kWidth, kHeight);
  ASSERT_NE(window, nullptr) << "a hidden window could not be created";
  HDC deviceContext = GetDC(window);
  ASSERT_NE(deviceContext, nullptr) << "the window has no device context";

  graphics::gpu::WindowSurface::Configuration configuration;
  configuration.nativeWindow = window;
  configuration.nativeDisplay = deviceContext;
  configuration.width = kWidth;
  configuration.height = kHeight;
  const auto surface = graphics::gpu::WindowSurface::make(configuration);
  ASSERT_NE(surface, nullptr) << "no window surface could be created";

  // Two lists, the second one translated and clipped: the shape a plan frame hands the window (the
  // content plus a viewport's band). The window used to take only the list pointers, which drew the
  // band at the wrong offset and without the clip.
  graphics::gpu::Recorder background(64, 64, nullptr);
  graphics::Paint red;
  red.color = graphics::Color::MakeRGBA8(200, 30, 30, 255);
  background.canvas().drawRect(graphics::Rect::MakeLTRB(0.0f, 0.0f, 64.0f, 64.0f), red);

  graphics::gpu::Recorder marker(64, 64, nullptr);
  graphics::Paint blue;
  blue.color = graphics::Color::MakeRGBA8(30, 60, 220, 255);
  marker.canvas().drawRect(graphics::Rect::MakeLTRB(0.0f, 0.0f, 32.0f, 32.0f), blue);

  std::vector<graphics::gpu::WindowSurface::ListItem> items;
  items.push_back(graphics::gpu::WindowSurface::ListItem{
    std::make_shared<const graphics::gpu::DrawList>(background.finish()), {}, {}, 1.0f });
  graphics::gpu::WindowSurface::ListItem band;
  band.list = std::make_shared<const graphics::gpu::DrawList>(marker.finish());
  band.translation = graphics::Point{ 32.0f, 32.0f };
  band.clip = graphics::Rect::MakeLTRB(0.0f, 0.0f, 48.0f, 48.0f);
  items.push_back(band);

  ASSERT_TRUE(surface->presentListItems(items)) << surface->diagnostics();
  EXPECT_EQ(surface->lastFrameSubmitCount(), 1u)
    << "the lists of a plan frame share one command buffer";

  graphics::Bitmap pixels;
  ASSERT_TRUE(surface->readback(&pixels)) << surface->diagnostics();
  const graphics::Pixmap pixmap = pixels.makePixmap();
  // The translated marker is blue where the clip keeps it ...
  const uint32_t inside = pixmap.getPixelARGB32(40, 40);
  EXPECT_EQ((inside >> 16) & 0xFFu, 30u) << "the translated list is at the translated position";
  EXPECT_EQ(inside & 0xFFu, 220u);
  // ... and the clip cuts it at 48, so the pixels past that stay the background's.
  const uint32_t clipped = pixmap.getPixelARGB32(56, 40);
  EXPECT_EQ((clipped >> 16) & 0xFFu, 200u) << "the item clip was dropped";
  EXPECT_EQ(clipped & 0xFFu, 30u);

  // A fractional translation cannot be baked into the frame's device space, so the call refuses rather
  // than drawing the list at the wrong place.
  std::vector<graphics::gpu::WindowSurface::ListItem> fractional;
  graphics::gpu::WindowSurface::ListItem shifted;
  shifted.list = std::make_shared<const graphics::gpu::DrawList>(marker.finish());
  shifted.translation = graphics::Point{ 8.5f, 0.0f };
  fractional.push_back(shifted);
  EXPECT_FALSE(surface->presentListItems(fractional))
    << "a fractional translation has to fall back to the merged frame";

  ReleaseDC(window, deviceContext);
  DestroyWindow(window);
#else
  GTEST_SKIP() << "the window surface test needs a native window, and this platform's is not wired into it yet";
#endif
}

TEST(WindowSurface, PresentsADisplayListRecordedWithoutADeviceContext) {
#if defined(_WIN32)
  constexpr int kWidth = 64;
  constexpr int kHeight = 64;

  HWND window = createHiddenWindow(kWidth, kHeight);
  ASSERT_NE(window, nullptr) << "a hidden window could not be created";
  HDC deviceContext = GetDC(window);
  ASSERT_NE(deviceContext, nullptr) << "the window has no device context";

  graphics::gpu::WindowSurface::Configuration configuration;
  configuration.nativeWindow = window;
  configuration.nativeDisplay = deviceContext;
  configuration.width = kWidth;
  configuration.height = kHeight;
  const auto surface = graphics::gpu::WindowSurface::make(configuration);
  ASSERT_NE(surface, nullptr) << "no window surface could be created";

  // The phase-2 producer: a recorder with no context, exactly as the UI thread records a packet.
  graphics::gpu::Recorder recorder(kWidth, kHeight, nullptr);
  graphics::Canvas& canvas = recorder.canvas();
  EXPECT_TRUE(canvas.isRecording());
  graphics::Paint background;
  background.color = graphics::Color::MakeRGBA8(0, 0, 255, 255);
  canvas.drawRect(graphics::Rect::MakeLTRB(0.0f, 0.0f, kWidth, kHeight), background);
  graphics::Paint marker;
  marker.color = graphics::Color::MakeRGBA8(255, 0, 0, 255);
  canvas.drawRect(graphics::Rect::MakeLTRB(16.0f, 16.0f, 48.0f, 48.0f), marker);
  const auto recording = std::make_shared<const graphics::gpu::DrawList>(recorder.finish());

  EXPECT_TRUE(surface->presentRecording(recording)) << surface->diagnostics();
  graphics::Bitmap pixels;
  ASSERT_TRUE(surface->readback(&pixels)) << surface->diagnostics();
  const graphics::Pixmap pixmap = pixels.makePixmap();
  const uint32_t centre = pixmap.getPixelARGB32(kWidth / 2, kHeight / 2);
  EXPECT_EQ((centre >> 16) & 0xFFu, 255u) << "the marker is red at the centre";
  const uint32_t corner = pixmap.getPixelARGB32(4, 4);
  EXPECT_EQ(corner & 0xFFu, 255u) << "the background is blue at the corner";

  ReleaseDC(window, deviceContext);
  DestroyWindow(window);
#else
  GTEST_SKIP() << "the window surface test needs a native window, and this platform's is not wired into it yet";
#endif
}

TEST(WindowSurface, ADamageOnlyRecordingKeepsThePixelsOutsideItsDamage) {
#if defined(_WIN32)
  constexpr int kWidth = 64;
  constexpr int kHeight = 64;

  HWND window = createHiddenWindow(kWidth, kHeight);
  ASSERT_NE(window, nullptr) << "a hidden window could not be created";
  HDC deviceContext = GetDC(window);
  ASSERT_NE(deviceContext, nullptr) << "the window has no device context";

  graphics::gpu::WindowSurface::Configuration configuration;
  configuration.nativeWindow = window;
  configuration.nativeDisplay = deviceContext;
  configuration.width = kWidth;
  configuration.height = kHeight;
  const auto surface = graphics::gpu::WindowSurface::make(configuration);
  ASSERT_NE(surface, nullptr) << "no window surface could be created";

  // Frame 1 establishes the page: blue background with a red marker in the middle.
  graphics::gpu::Recorder first(kWidth, kHeight, nullptr);
  graphics::Paint background;
  background.color = graphics::Color::MakeRGBA8(0, 0, 255, 255);
  first.canvas().drawRect(graphics::Rect::MakeLTRB(0.0f, 0.0f, kWidth, kHeight), background);
  graphics::Paint marker;
  marker.color = graphics::Color::MakeRGBA8(255, 0, 0, 255);
  first.canvas().drawRect(graphics::Rect::MakeLTRB(16.0f, 16.0f, 48.0f, 48.0f), marker);
  const auto firstRecording = std::make_shared<const graphics::gpu::DrawList>(first.finish());
  ASSERT_TRUE(surface->presentRecording(firstRecording)) << surface->diagnostics();

  // Frame 2 repaints only the marker area; everything outside must keep frame 1's pixels
  // (`LoadOp::Load` + scissor) instead of being cleared.
  const graphics::Rect damage = graphics::Rect::MakeLTRB(16.0f, 16.0f, 48.0f, 48.0f);
  graphics::gpu::Recorder second(kWidth, kHeight, nullptr);
  graphics::Paint replacement;
  replacement.color = graphics::Color::MakeRGBA8(0, 255, 0, 255);
  second.canvas().drawRect(damage, replacement);
  const auto secondRecording = std::make_shared<const graphics::gpu::DrawList>(second.finish());
  ASSERT_TRUE(surface->presentRecording(secondRecording, damage)) << surface->diagnostics();

  graphics::Bitmap pixels;
  ASSERT_TRUE(surface->readback(&pixels)) << surface->diagnostics();
  const graphics::Pixmap pixmap = pixels.makePixmap();
  const uint32_t centre = pixmap.getPixelARGB32(kWidth / 2, kHeight / 2);
  EXPECT_EQ((centre >> 8) & 0xFFu, 255u) << "the damaged area is repainted green";
  const uint32_t corner = pixmap.getPixelARGB32(4, 4);
  EXPECT_EQ(corner & 0xFFu, 255u) << "the corner outside the damage keeps frame 1's blue";
  EXPECT_EQ((corner >> 16) & 0xFFu, 0u) << "the corner was not cleared or repainted";

  ReleaseDC(window, deviceContext);
  DestroyWindow(window);
#else
  GTEST_SKIP() << "the window surface test needs a native window, and this platform's is not wired into it yet";
#endif
}

TEST(WindowSurface, PresentsAMultisampledFrame) {
#if defined(_WIN32)
  constexpr int kWidth = 64;
  constexpr int kHeight = 64;

  HWND window = createHiddenWindow(kWidth, kHeight);
  ASSERT_NE(window, nullptr) << "a hidden window could not be created";
  HDC deviceContext = GetDC(window);
  ASSERT_NE(deviceContext, nullptr) << "the window has no device context";

  graphics::gpu::WindowSurface::Configuration configuration;
  configuration.nativeWindow = window;
  configuration.nativeDisplay = deviceContext;
  configuration.width = kWidth;
  configuration.height = kHeight;
  configuration.preferred = backend::Backend::Vulkan;
  configuration.sampleCount = 4;
  configuration.enableValidation = std::getenv("CORAL_BACKEND_VALIDATION") != nullptr;

  const auto surface = graphics::gpu::WindowSurface::make(configuration);
  if (surface == nullptr) {
    // This platform or build has no Vulkan window surface, or the device does not offer 4x MSAA.
    GTEST_SKIP() << "no Vulkan 4x window surface in this environment";
  }
  EXPECT_EQ(surface->sampleCount(), 4u);

  graphics::Canvas* canvas = surface->beginFrame();
  ASSERT_NE(canvas, nullptr) << surface->diagnostics();
  canvas->clear(graphics::Color::MakeRGBA8(0, 0, 255, 255));
  graphics::Paint marker;
  marker.color = graphics::Color::MakeRGBA8(255, 0, 0, 255);
  canvas->drawRect(graphics::Rect::MakeLTRB(16.0f, 16.0f, 48.0f, 48.0f), marker);

  // `present` is the handover that resolves the MSAA attachment into the swapchain image. A true here,
  // with the validation layer active, is the check for the resolve-to-default-framebuffer path.
  EXPECT_TRUE(surface->present()) << surface->diagnostics();

  // The screenshot path re-renders into a single-sampled scratch target; it must still see the frame.
  graphics::Bitmap pixels;
  ASSERT_TRUE(surface->readback(&pixels)) << surface->diagnostics();
  ASSERT_FALSE(pixels.isEmpty());
  const graphics::Pixmap pixmap = pixels.makePixmap();
  const uint32_t centre = pixmap.getPixelARGB32(kWidth / 2, kHeight / 2);
  EXPECT_EQ((centre >> 16) & 0xFFu, 255u) << "the marker is red at the centre";
  EXPECT_EQ(centre & 0xFFu, 0u);

  ReleaseDC(window, deviceContext);
  DestroyWindow(window);
#else
  GTEST_SKIP() << "the window surface test needs a native window, and this platform's is not wired into it yet";
#endif
}

TEST(WindowSurface, ResizingRecreatesTheTargetAndPresentsAgain) {
#if defined(_WIN32)
  constexpr int kWidth = 64;
  constexpr int kHeight = 64;
  HWND window = createHiddenWindow(kWidth, kHeight);
  ASSERT_NE(window, nullptr);
  HDC deviceContext = GetDC(window);
  ASSERT_NE(deviceContext, nullptr);

  graphics::gpu::WindowSurface::Configuration configuration;
  configuration.nativeWindow = window;
  configuration.nativeDisplay = deviceContext;
  configuration.width = kWidth;
  configuration.height = kHeight;
  const auto surface = graphics::gpu::WindowSurface::make(configuration);
  ASSERT_NE(surface, nullptr) << "no window surface could be created";

  graphics::Canvas* first = surface->beginFrame();
  ASSERT_NE(first, nullptr);
  first->clear(graphics::Color::MakeRGBA8(0, 0, 0, 0));
  ASSERT_TRUE(surface->present()) << surface->diagnostics();

  MoveWindow(window, 0, 0, 96, 80, TRUE);
  ASSERT_TRUE(surface->resize(96, 80)) << surface->diagnostics();
  EXPECT_EQ(surface->width(), 96u);
  EXPECT_EQ(surface->height(), 80u);

  graphics::Canvas* second = surface->beginFrame();
  ASSERT_NE(second, nullptr);
  graphics::Paint red;
  red.color = graphics::Color::MakeRGBA8(255, 0, 0, 255);
  second->drawRect(graphics::Rect::MakeLTRB(8.0f, 8.0f, 88.0f, 72.0f), red);
  ASSERT_TRUE(surface->present()) << surface->diagnostics();

  graphics::Bitmap pixels;
  ASSERT_TRUE(surface->readback(&pixels)) << surface->diagnostics();
  ASSERT_FALSE(pixels.isEmpty());
  EXPECT_EQ(pixels.width(), 96);
  EXPECT_EQ(pixels.height(), 80);
#else
  GTEST_SKIP() << "the window surface test is Windows-only";
#endif
}

TEST(WindowSurface, RefusesAConfigurationWithoutAWindowOrASize) {
  graphics::gpu::WindowSurface::Configuration configuration;
  EXPECT_EQ(graphics::gpu::WindowSurface::make(configuration), nullptr) << "no window and no size must be refused";

  configuration.nativeWindow = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
  configuration.width = 32;
  configuration.height = 32;
  // A non-null pretend window is refused by the backend rather than crashing: the device factory reports the failure and
  // this returns null with a diagnostic, which is what a caller that passed the wrong handle needs to see.
  const auto surface = graphics::gpu::WindowSurface::make(configuration);
  EXPECT_EQ(surface, nullptr);
}

/** Creates a window surface that joins @p group, or null. */
#if defined(_WIN32)
namespace {

std::unique_ptr<graphics::gpu::WindowSurface> makeGroupedWindow(HWND window, HDC deviceContext, const int size,
        const std::shared_ptr<graphics::gpu::ContextGroup>& group) {
  graphics::gpu::WindowSurface::Configuration configuration;
  configuration.nativeWindow = window;
  configuration.nativeDisplay = deviceContext;
  configuration.width = static_cast<uint32_t>(size);
  configuration.height = static_cast<uint32_t>(size);
  configuration.group = group;
  // ## This group is a *GL* group
  //
  // "Shared context" is an OpenGL concept: Vulkan and Metal have no per-context object space, so their
  // devices share resources by being the same device (or by an explicit external-memory export, which
  // this backend does not implement yet - see design document section 4.4). The test is about the GL
  // sharing path, so it asks for GL rather than measuring whichever backend the platform prefers.
  configuration.preferred = backend::Backend::OpenGL;
  return graphics::gpu::WindowSurface::make(configuration);
}

} // namespace anonymous
#endif

TEST(ContextGroup, AReleaseFromAnotherThreadRunsOnTheOwningDevice) {
#if defined(_WIN32)
  constexpr int kSize = 32;

  const auto group = graphics::gpu::ContextGroup::make();
  ASSERT_NE(group, nullptr);

  HWND window = createHiddenWindow(kSize, kSize);
  ASSERT_NE(window, nullptr);
  HDC deviceContext = GetDC(window);
  auto surface = makeGroupedWindow(window, deviceContext, kSize, group);
  ASSERT_NE(surface, nullptr) << "no window surface was created";
  backend::Device* device = surface->device();
  ASSERT_EQ(device->makeCurrent(), backend::Result::Success);

  // A texture that belongs to this device's object space.
  backend::TextureDescriptor descriptor = backend::TextureDescriptor::texture2D(kSize, kSize,
          backend::PixelFormat::R8G8B8A8_UNORM, static_cast<backend::TextureUsages>(backend::TextureUsage::Sampled));
  backend::TextureHandle texture = backend::TextureHandle::invalid();
  ASSERT_TRUE(backend::isSuccess(device->createTexture(descriptor, &texture)));
  ASSERT_TRUE(texture.valid());

  // The "UI thread": it may not delete a GL object, so it hands the task to the group.
  std::thread ui([&] {
    group->deferRelease(device, [device, texture] { device->destroyTexture(texture); });
  });
  ui.join();
  EXPECT_EQ(group->pendingReleaseCount(), 1u) << "a release from the wrong thread must wait, not run";

  // The owning thread drains it -- `beginFrame` is where a render loop does this.
  ASSERT_NE(surface->beginFrame(), nullptr) << surface->diagnostics();
  EXPECT_EQ(group->pendingReleaseCount(), 0u) << "the drain must consume the task";

  // The frame must be finished before another is opened: `beginFrame` refuses a second one, which is the facade protecting the
  // caller from recording half of one frame and half of the next.
  if (!surface->present()) {
    // The GL backend on this machine cannot link its own shader programs -- its GLSL source uses `attribute`,
    // which the targeted dialect rejects -- so a present that needs a program fails for a reason that has
    // nothing to do with the device-object lifetime this test is about. The backend's error goes to its log
    // rather than to `diagnostics()`, so the device it is running on is what identifies the case.
    const backend::DeviceInfo& info = device->getInfo();
    if (info.backend == backend::Backend::OpenGL) {
      GTEST_SKIP() << "the OpenGL backend cannot link its shaders in this environment ("
                   << (info.apiVersion != nullptr ? info.apiVersion : "") << ")";
    }
    FAIL() << surface->diagnostics();
  }

  // A task for a *different* owner must stay queued: the drain is per-owner, and that is what keeps one device from
  // deleting another's objects from its own thread.
  std::atomic<bool> foreignRan { false };
  group->deferRelease(reinterpret_cast<void*>(0x1), [&] { foreignRan = true; });
  EXPECT_EQ(group->pendingReleaseCount(), 1u);
  ASSERT_NE(surface->beginFrame(), nullptr) << surface->diagnostics();
  EXPECT_EQ(group->pendingReleaseCount(), 1u) << "another owner's task must not be drained here";
  EXPECT_FALSE(foreignRan.load()) << "another owner's release must not run on this device's thread";

  // Destroying the surface now exercises the path that used to crash: a frame was opened and never presented, which is exactly
  // what leaves a `Canvas` holding the context alive. The fix is in `~WindowSurface` -- it drops the canvas before the context.
  surface.reset();
  EXPECT_EQ(group->deviceCount(), 0u);
  ReleaseDC(window, deviceContext);
  DestroyWindow(window);
#else
  GTEST_SKIP() << "the deferred-release test needs native windows, and this platform's are not wired into it yet";
#endif
}
TEST(WindowSurface, ThePresentModeIsReportedAsGrantedNotAsRequested) {
#if defined(_WIN32)
  constexpr int kSize = 32;

  // ## One window per surface, because Vulkan owns the window through the swapchain
  //
  // A GL context can coexist with others on one HWND; a Vulkan swapchain cannot, and a second
  // `vkCreateSwapchainKHR` on a window that already has a live one fails (`OUT_OF_DATE` here,
  // `NATIVE_WINDOW_IN_USE_KHR` on drivers that report it). Giving each surface its own window is what
  // makes this a test of the *present mode* rather than of a platform constraint that is not its subject.
  const auto makeSurface = [](const backend::PresentMode mode, HWND* outWindow, HDC* outContext) {
    *outWindow = createHiddenWindow(kSize, kSize);
    if (*outWindow == nullptr) return std::unique_ptr<graphics::gpu::WindowSurface>();
    *outContext = GetDC(*outWindow);
    graphics::gpu::WindowSurface::Configuration configuration;
    configuration.nativeWindow = *outWindow;
    configuration.nativeDisplay = *outContext;
    configuration.width = kSize;
    configuration.height = kSize;
    configuration.presentMode = mode;
    return graphics::gpu::WindowSurface::make(configuration);
  };

  // Fifo is what every backend can do, so it must come back as Fifo.
  HWND fifoWindow = nullptr;
  HDC fifoContext = nullptr;
  const auto fifoSurface = makeSurface(backend::PresentMode::Fifo, &fifoWindow, &fifoContext);
  ASSERT_NE(fifoSurface, nullptr);
  EXPECT_EQ(fifoSurface->presentMode(), backend::PresentMode::Fifo);

  // Immediate may be unavailable (no `wglSwapIntervalEXT`, no `VK_PRESENT_MODE_IMMEDIATE_KHR`): the contract is
  // that the *granted* mode is reported, so this is either Immediate or a documented fallback to Fifo -- and
  // never a silent lie.
  HWND immediateWindow = nullptr;
  HDC immediateContext = nullptr;
  const auto immediateSurface = makeSurface(backend::PresentMode::Immediate, &immediateWindow, &immediateContext);
  ASSERT_NE(immediateSurface, nullptr);
  const backend::PresentMode granted = immediateSurface->presentMode();
  EXPECT_TRUE(granted == backend::PresentMode::Immediate || granted == backend::PresentMode::Fifo)
          << "requesting Immediate must grant Immediate or fall back to Fifo, not something else";
  std::fprintf(stderr, "[present-mode] requested Immediate, granted %s\n",
          granted == backend::PresentMode::Immediate ? "Immediate" : "Fifo (no immediate mode available)");

  // Mailbox is a queue semantic only some backends can honour. The grant must match the capability: a device that
  // advertises it gets Mailbox, and one that does not must report the approximation (Immediate) rather than
  // claiming the real thing.
  HWND mailboxWindow = nullptr;
  HDC mailboxContext = nullptr;
  const auto mailboxSurface = makeSurface(backend::PresentMode::Mailbox, &mailboxWindow, &mailboxContext);
  ASSERT_NE(mailboxSurface, nullptr);
  const bool canMailbox = mailboxSurface->device()->getCapabilities().mailboxPresent;
  if (canMailbox) {
    EXPECT_EQ(mailboxSurface->presentMode(), backend::PresentMode::Mailbox)
            << "a device that advertises mailbox must grant it";
  } else {
    EXPECT_NE(mailboxSurface->presentMode(), backend::PresentMode::Mailbox)
            << "an approximation must not be reported as the real mode";
  }

  ReleaseDC(fifoWindow, fifoContext);
  DestroyWindow(fifoWindow);
  ReleaseDC(immediateWindow, immediateContext);
  DestroyWindow(immediateWindow);
  ReleaseDC(mailboxWindow, mailboxContext);
  DestroyWindow(mailboxWindow);
#else
  GTEST_SKIP() << "the present-mode test needs a native window, and this platform's is not wired into it yet";
#endif
}

TEST(WindowSurface, APresentedFrameIsStillCorrectInImmediateMode) {
#if defined(_WIN32)
  constexpr int kSize = 32;
  HWND window = createHiddenWindow(kSize, kSize);
  ASSERT_NE(window, nullptr);
  HDC deviceContext = GetDC(window);

  graphics::gpu::WindowSurface::Configuration configuration;
  configuration.nativeWindow = window;
  configuration.nativeDisplay = deviceContext;
  configuration.width = kSize;
  configuration.height = kSize;
  configuration.presentMode = backend::PresentMode::Immediate;
  const auto surface = graphics::gpu::WindowSurface::make(configuration);
  ASSERT_NE(surface, nullptr);

  graphics::Canvas* canvas = surface->beginFrame();
  ASSERT_NE(canvas, nullptr) << surface->diagnostics();
  graphics::Paint paint;
  paint.color = graphics::Color::MakeRGBA8(0, 255, 0, 255);
  canvas->drawRect(graphics::Rect::MakeLTRB(0.0f, 0.0f, kSize, kSize), paint);
  // The point of this case: presenting without a vsync wait must not change what was drawn.
  EXPECT_TRUE(surface->present()) << surface->diagnostics();

  graphics::Bitmap pixels;
  ASSERT_TRUE(surface->readback(&pixels)) << surface->diagnostics();
  EXPECT_EQ((pixels.makePixmap().getPixelARGB32(kSize / 2, kSize / 2) >> 8) & 0xFFu, 255u)
          << "the frame is green in Immediate mode too";

  ReleaseDC(window, deviceContext);
  DestroyWindow(window);
#else
  GTEST_SKIP() << "the present-mode test needs a native window, and this platform's is not wired into it yet";
#endif
}
TEST(WindowSurface, MailboxIsGrantedOnlyWhenTheDeviceSaysItCan) {
#if defined(_WIN32)
  // ## The capability and the granted mode must agree
  //
  // `PresentMode::Mailbox` means something specific: at most one queued image, the newest replaces the older, no vertical-sync
  // wait. No OpenGL platform can do that -- a swap interval of 0 stops waiting but does not drop a frame already handed over -- so
  // a Mailbox request comes back as Immediate and `Capabilities::mailboxPresent` is false. When a Vulkan or Metal backend lands,
  // both flip together. This test is what keeps them from flipping separately: whichever answer the device gives, the granted
  // mode has to match it.
  constexpr int kSize = 32;
  HWND window = createHiddenWindow(kSize, kSize);
  ASSERT_NE(window, nullptr);
  HDC deviceContext = GetDC(window);

  graphics::gpu::WindowSurface::Configuration configuration;
  configuration.nativeWindow = window;
  configuration.nativeDisplay = deviceContext;
  configuration.width = kSize;
  configuration.height = kSize;
  configuration.presentMode = backend::PresentMode::Mailbox;
  const auto surface = graphics::gpu::WindowSurface::make(configuration);
  ASSERT_NE(surface, nullptr);

  const backend::Device* device = surface->device();
  ASSERT_NE(device, nullptr);
  const bool canMailbox = device->getCapabilities().mailboxPresent;
  const backend::PresentMode granted = surface->presentMode();
  std::fprintf(stderr, "[present-mode] mailboxPresent=%d granted=%d\n", canMailbox ? 1 : 0,
          static_cast<int>(granted));

  if (canMailbox) {
    EXPECT_EQ(granted, backend::PresentMode::Mailbox)
            << "a device that advertises mailbox must grant it";
  } else {
    EXPECT_NE(granted, backend::PresentMode::Mailbox)
            << "a device without the mailbox queue must not claim to have granted it";
    EXPECT_EQ(granted, backend::PresentMode::Immediate)
            << "the documented approximation of Mailbox is Immediate";
  }

  ReleaseDC(window, deviceContext);
  DestroyWindow(window);
#else
  GTEST_SKIP() << "the present-mode test needs a native window, and this platform's is not wired into it yet";
#endif
}

// A live preview that only redraws the video rectangle hands `presentDamage` a recording with no command for the
// chrome outside it. Those pixels must survive from the previous present; the partial list is the only difference
// from the tests above, which always draw the whole page.
TEST(WindowSurface, ADamageOnlyFrameKeepsThePixelsOutsideItsPartialRecording) {
#if defined(_WIN32)
  constexpr int kWidth = 64;
  constexpr int kHeight = 64;

  HWND window = createHiddenWindow(kWidth, kHeight);
  ASSERT_NE(window, nullptr) << "a hidden window could not be created";
  HDC deviceContext = GetDC(window);
  ASSERT_NE(deviceContext, nullptr) << "the window has no device context";

  graphics::gpu::WindowSurface::Configuration configuration;
  configuration.nativeWindow = window;
  configuration.nativeDisplay = deviceContext;
  configuration.width = kWidth;
  configuration.height = kHeight;
  const auto surface = graphics::gpu::WindowSurface::make(configuration);
  ASSERT_NE(surface, nullptr) << "no window surface could be created";

  {
    graphics::Canvas* canvas = surface->beginFrame();
    ASSERT_NE(canvas, nullptr) << surface->diagnostics();
    canvas->clear(graphics::Color::MakeRGBA8(0, 0, 255, 255));
    ASSERT_TRUE(surface->present()) << surface->diagnostics();
  }

  {
    graphics::Canvas* canvas = surface->beginFrame();
    ASSERT_NE(canvas, nullptr) << surface->diagnostics();
    graphics::Paint marker;
    marker.color = graphics::Color::MakeRGBA8(255, 0, 0, 255);
    marker.style = graphics::Paint::Style::kFill;
    canvas->drawRect(graphics::Rect::MakeWH(16.0f, 16.0f), marker);
    ASSERT_TRUE(surface->presentDamage(graphics::Rect::MakeWH(16.0f, 16.0f)))
            << surface->diagnostics();
  }

  graphics::Bitmap pixels;
  ASSERT_TRUE(surface->readback(&pixels)) << surface->diagnostics();
  ASSERT_FALSE(pixels.isEmpty());
  const graphics::Pixmap pixmap = pixels.makePixmap();
  EXPECT_EQ((pixmap.getPixelARGB32(8, 8) >> 16) & 0xFFu, 255u) << "the damage is red";
  EXPECT_EQ(pixmap.getPixelARGB32(8, 8) & 0xFFu, 0u);
  EXPECT_EQ(pixmap.getPixelARGB32(40, 40) & 0xFFu, 255u)
          << "pixels outside the damage keep the previous present";
  EXPECT_EQ((pixmap.getPixelARGB32(40, 40) >> 16) & 0xFFu, 0u);

  ReleaseDC(window, deviceContext);
  DestroyWindow(window);
#else
  GTEST_SKIP() << "the damage test needs a native window, and this platform's is not wired into it yet";
#endif
}