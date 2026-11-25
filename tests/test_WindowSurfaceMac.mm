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
 * @file test_WindowSurfaceMac.mm
 *
 * The window path on macOS, including the colour-managed presentation modes.
 *
 * The test opens a borderless window whose content view owns a `CAMetalLayer`, then presents once in each
 * colour space the Metal backend offers. What it checks is the part a unit test can check without a
 * colourimeter: the swapchain granted the requested format and colour space, `present()` returns true,
 * and the frame survives the two presentation passes (sRGB canvas -> linear P3 16F -> display encoding)
 * without a backend error. A headless machine skips, for the same reason the CADisplayLink test does.
 */

#include <gpu/MetalDisplayLink.h>
#include <gpu/WindowSurface.h>

#include <graphics/Canvas.h>
#include <graphics/Paint.h>

#include <gtest/gtest.h>

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

namespace {

/** A hidden-or-offscreen window with a Metal-backed content view. */
struct TestWindow {
  NSWindow* window = nil;
  NSView* view = nil;
  CAMetalLayer* layer = nil;

  bool create() {
    if ([NSScreen mainScreen] == nil) return false;

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];

    window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 64, 64)
                                         styleMask:NSWindowStyleMaskBorderless
                                           backing:NSBackingStoreBuffered
                                             defer:NO];
    if (window == nil) return false;
    view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 64, 64)];
    [view setWantsLayer:YES];
    layer = [CAMetalLayer layer];
    layer.contentsScale = 1.0;
    layer.framebufferOnly = NO;
    view.layer = layer;
    window.contentView = view;
    [window orderFrontRegardless];
    return layer != nil;
  }

  ~TestWindow() {
    if (window != nil) [window close];
  }
};

struct SpaceCase {
  backend::SwapchainColorSpace space;
  backend::PixelFormat format;
  const char* name;
};

} // namespace

TEST(WindowSurfaceMac, PresentsInEveryGrantedColourSpace) {
  TestWindow window;
  if (!window.create()) GTEST_SKIP() << "no window server or Metal layer is available";

  const SpaceCase cases[] = {
    { backend::SwapchainColorSpace::Srgb, backend::PixelFormat::B8G8R8A8_UNORM, "sRGB" },
    { backend::SwapchainColorSpace::DisplayP3, backend::PixelFormat::B8G8R8A8_UNORM, "Display P3" },
    { backend::SwapchainColorSpace::ExtendedLinearP3, backend::PixelFormat::R16G16B16A16_SFLOAT, "EDR P3" },
    { backend::SwapchainColorSpace::Hdr10Pq, backend::PixelFormat::R10G10B10A2_UNORM, "HDR10 PQ" },
  };

  for (const SpaceCase& entry: cases) {
    graphics::gpu::WindowSurface::Configuration configuration;
    configuration.nativeWindow = (__bridge void*) window.layer;
    configuration.nativeWindowSystem = backend::NativeWindowSystem::Metal;
    configuration.width = 64;
    configuration.height = 64;
    configuration.preferred = backend::Backend::Metal;
    configuration.outputColorSpace = entry.space;

    auto surface = graphics::gpu::WindowSurface::make(configuration);
    if (surface == nullptr) {
      // A machine whose Metal device is unavailable skips rather than failing: the test is about the
      // colour-space path, not about whether this host has a GPU.
      GTEST_SKIP() << entry.name << ": " << "no Metal window surface could be created";
    }
    EXPECT_EQ(surface->displayColorSpace(), entry.space)
            << entry.name << ": the backend did not grant the requested space; diagnostics: "
            << surface->diagnostics();
    EXPECT_EQ(surface->format(), entry.format) << entry.name << ": the target format follows the granted space";

    graphics::Canvas* canvas = surface->beginFrame();
    ASSERT_NE(canvas, nullptr) << entry.name << ": " << surface->diagnostics();
    graphics::Paint paint;
    paint.color = graphics::Color::MakeRGBA8(220, 30, 90, 255);
    canvas->drawRect(graphics::Rect::MakeLTRB(0.0f, 0.0f, 64.0f, 64.0f), paint);
    EXPECT_TRUE(surface->present()) << entry.name << ": " << surface->diagnostics();
  }
}


TEST(WindowSurfaceMac, RendersIntoTheDrawableCAMetalDisplayLinkSupplies) {
  TestWindow window;
  if (!window.create()) GTEST_SKIP() << "no window server or Metal layer is available";

  auto link = graphics::gpu::MetalDisplayLink::make((__bridge void*) window.layer);
  if (link == nullptr) GTEST_SKIP() << "CAMetalDisplayLink is unavailable on this OS";

  // CAMetalDisplayLink needs the layer to be configured (a device and a pixel format) before it will
  // schedule updates. The real renderer does that when it creates its surface; this test creates one
  // once on the main thread and drops it, so the layer is ready before the link's thread starts.
  {
    graphics::gpu::WindowSurface::Configuration warmup;
    warmup.nativeWindow = (__bridge void*) window.layer;
    warmup.nativeWindowSystem = backend::NativeWindowSystem::Metal;
    warmup.width = 64;
    warmup.height = 64;
    warmup.preferred = backend::Backend::Metal;
    warmup.outputColorSpace = backend::SwapchainColorSpace::ExtendedLinearP3;
    auto prepared = graphics::gpu::WindowSurface::make(warmup);
    if (prepared == nullptr) GTEST_SKIP() << "the Metal layer could not be prepared";
  }

  struct State {
    std::mutex mutex;
    std::condition_variable condition;
    std::unique_ptr<graphics::gpu::WindowSurface> surface;
    int frames = 0;
    bool failed = false;
    std::string diagnostics;
  } state;

  const bool started = link->start([&state, &window](const graphics::gpu::MetalDisplayLinkUpdate& update) {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.failed) return;
    if (state.surface == nullptr) {
      graphics::gpu::WindowSurface::Configuration configuration;
      configuration.nativeWindow = (__bridge void*) window.layer;
      configuration.nativeWindowSystem = backend::NativeWindowSystem::Metal;
      configuration.width = 64;
      configuration.height = 64;
      configuration.preferred = backend::Backend::Metal;
      configuration.outputColorSpace = backend::SwapchainColorSpace::ExtendedLinearP3;
      state.surface = graphics::gpu::WindowSurface::make(configuration);
      if (state.surface == nullptr) {
        state.failed = true;
        state.diagnostics = "the display-link thread could not create the window surface";
        state.condition.notify_all();
        return;
      }
    }

    graphics::Canvas* canvas = state.surface->beginFrame(update.drawable);
    if (canvas == nullptr) {
      state.failed = true;
      state.diagnostics = state.surface->diagnostics();
      state.condition.notify_all();
      return;
    }
    graphics::Paint paint;
    paint.color = graphics::Color::MakeRGBA8(30, 160, 220, 255);
    canvas->drawRect(graphics::Rect::MakeLTRB(0.0f, 0.0f, 64.0f, 64.0f), paint);
    if (!state.surface->present()) {
      state.failed = true;
      state.diagnostics = state.surface->diagnostics();
      state.condition.notify_all();
      return;
    }
    ++state.frames;
    state.condition.notify_all();
  });
  ASSERT_TRUE(started) << "the CAMetalDisplayLink run loop did not start";

  {
    std::unique_lock<std::mutex> lock(state.mutex);
    state.condition.wait_for(lock, std::chrono::seconds(5),
            [&state] { return state.frames >= 2 || state.failed; });
    EXPECT_FALSE(state.failed) << state.diagnostics;
    EXPECT_GE(state.frames, 2);
  }

  // `stop` joins the display-link thread. The surface was created and used on that thread; releasing it
  // afterwards is safe on Metal, whose resources are not thread-affine the way a GL context is.
  link->stop();
  link.reset();
  state.surface.reset();
}
