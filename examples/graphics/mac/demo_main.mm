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
 * @file demo_main.mm
 *
 * The macOS window demo.
 *
 * The scene is the same one the Linux demo draws (a cheap animated panel of shapes and one stroked path), and it
 * goes through the public `graphics::gpu::WindowSurface` API. The only macOS-specific part is the window, and it has
 * two forms:
 *
 *   - **Metal** (the default, and the platform's own backend): a plain `NSView` whose backing layer is a
 *     `CAMetalLayer`. The layer is handed to `WindowSurface` as `nativeWindow`, the RHI acquires a drawable per frame,
 *     and `present()` queues it. No CGL context exists in this path at all.
 *   - **OpenGL** (`--backend opengl`): an `NSOpenGLView` and its `NSOpenGLContext`, whose `CGLContextObj` is handed to
 *     `WindowSurface` through `Configuration::cglContext`. CGL has no call that creates a drawable, so the view owns it
 *     and the backend adopts the context.
 *
 * `--verify` renders the same scene through an offscreen `gpu::Surface`, which is what a build machine or CI runs; the
 * backend there is whatever `--backend` names, defaulting to Metal.
 *
 *   out/examples/bin/mac_demo
 *   out/examples/bin/mac_demo --frames 120 --immediate
 *   out/examples/bin/mac_demo --backend opengl
 *   out/examples/bin/mac_demo --verify --backend metal
 */

#include "demo_options.h"
#include "demo_scene.h"

#include <graphics/ImageCodec.h>

#include <gpu/Surface.h>
#include <gpu/WindowSurface.h>

#include <backend/Types.h>

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <OpenGL/OpenGL.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// ------------------------------------------------------------------------------------------------
// The views
// ------------------------------------------------------------------------------------------------
// Both views answer the same two questions the frame loop asks: whether the user asked to stop, and what their
// backing-pixel size is. The stop flag is a protocol rather than a shared base class because one view must be an
// `NSOpenGLView` (there is no other way to own a CGL drawable) and the other must be a plain `NSView` (there is no
// reason to create a GL context for a Metal window).

@protocol CoralDemoStoppable <NSObject>
- (BOOL)shouldStop;
@end

/** The CGL/OpenGL view: Esc, Q or closing the window ends the loop. */
@interface CoralDemoView : NSOpenGLView <CoralDemoStoppable>
@property(nonatomic) BOOL shouldStop;
@end

@implementation CoralDemoView

- (BOOL)acceptsFirstResponder {
  return YES;
}

- (void)keyDown:(NSEvent*)event {
  [self handleStopKey:event];
}

- (void)handleStopKey:(NSEvent*)event {
  NSString* characters = [event charactersIgnoringModifiers];
  if (characters.length == 0) {
    return;
  }
  const unichar character = [characters characterAtIndex:0];
  if (character == 27 /* Esc */ || character == 'q' || character == 'Q') {
    self.shouldStop = YES;
  }
}

@end

/** The Metal view: its backing layer is a `CAMetalLayer`, which is the drawable the RHI presents. */
@interface CoralMetalView : NSView <CoralDemoStoppable>
@property(nonatomic) BOOL shouldStop;
@end

@implementation CoralMetalView

- (BOOL)acceptsFirstResponder {
  return YES;
}

- (CALayer*)makeBackingLayer {
  // `wantsLayer = YES` installs this as the view's backing layer. Its size follows the view automatically; the
  // drawable size in *pixels* is the surface's business and is set by the backend from the size the demo reports.
  return [CAMetalLayer layer];
}

- (void)keyDown:(NSEvent*)event {
  NSString* characters = [event charactersIgnoringModifiers];
  if (characters.length == 0) {
    return;
  }
  const unichar character = [characters characterAtIndex:0];
  if (character == 27 /* Esc */ || character == 'q' || character == 'Q') {
    self.shouldStop = YES;
  }
}

@end

namespace {

struct BackendSelection {
  bool metal = true;
  const char* name = "metal";
};

/** Resolves `auto` to the platform's own choice: Metal when the build has it, otherwise OpenGL. */
BackendSelection resolveBackend(const demo::Options& options) {
  BackendSelection selection;
  if (options.backend == "opengl") {
    selection.metal = false;
    selection.name = "opengl";
  } else if (options.backend == "metal") {
    selection.metal = true;
    selection.name = "metal";
  } else {
    selection.metal = backend::isBackendAvailable(backend::Backend::Metal);
    selection.name = selection.metal ? "metal" : "opengl";
  }
  return selection;
}

void printUsage(const char* program) {
  std::printf(
    "usage: %s [options]\n"
    "  --backend auto|metal|opengl  rendering backend (default: auto, which is Metal on macOS)\n"
    "  --width N              window width in points (default 960)\n"
    "  --height N             window height in points (default 540)\n"
    "  --frames N             present N frames and exit; 0 runs until the window closes\n"
    "  --immediate            present without waiting for vsync\n"
    "  --verify               render one offscreen frame instead of opening a window\n"
    "  --snapshot FILE        also write the presented frame to a PNG (window path)\n"
    "  --help                 print this text\n"
    "\n"
    "The window path uses graphics::gpu::WindowSurface: a CAMetalLayer for Metal, an NSOpenGLView/CGL\n"
    "context for OpenGL. The same scene is drawn through an offscreen graphics::gpu::Surface for --verify,\n"
    "so that mode runs on a machine with no display.\n",
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

/** Writes @p surface's current frame as a PNG, so a presented window can be checked without a screenshot. */
bool writeSnapshot(graphics::gpu::WindowSurface& surface, const std::string& path) {
  graphics::Bitmap bitmap;
  if (!surface.readback(&bitmap)) {
    std::fprintf(stderr, "mac_demo: window readback failed: %s\n", surface.diagnostics().c_str());
    return false;
  }
  graphics::DecodedImage image;
  image.info = bitmap.info();
  const size_t rowBytes = static_cast<size_t>(bitmap.width()) * 4;
  image.pixels.resize(rowBytes * static_cast<size_t>(bitmap.height()));
  for (int row = 0; row < bitmap.height(); ++row) {
    std::memcpy(image.pixels.data() + static_cast<size_t>(row) * rowBytes,
      bitmap.data().bytes() + static_cast<size_t>(row) * bitmap.rowBytes(), rowBytes);
  }
  const auto encoded = graphics::encodeImage(image, graphics::ImageFormat::PNG, {});
  if (!encoded.has_value()) {
    std::fprintf(stderr, "mac_demo: the frame could not be encoded as PNG\n");
    return false;
  }
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(encoded->data()), static_cast<std::streamsize>(encoded->size()));
  if (!output) {
    std::fprintf(stderr, "mac_demo: could not write '%s'\n", path.c_str());
    return false;
  }
  std::printf("mac_demo: wrote %s\n", path.c_str());
  return true;
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

backend::Backend backendFromSelection(const BackendSelection selection) {
  return selection.metal ? backend::Backend::Metal : backend::Backend::OpenGL;
}

int verifyOffscreen(const demo::Options& options) {
  const BackendSelection selection = resolveBackend(options);
  const backend::Backend wanted = backendFromSelection(selection);

  graphics::gpu::SurfaceConfiguration configuration;
  configuration.width = options.width;
  configuration.height = options.height;
  configuration.preferred = wanted;
  configuration.stencil = true;

  auto surface = graphics::gpu::Surface::make(configuration);
  if (surface == nullptr) {
    std::fprintf(stderr, "mac_demo --verify: no %s device could be created on this machine.\n",
      backend::toString(wanted));
    return 77;
  }
  if (surface->device()->getInfo().backend != wanted) {
    std::fprintf(stderr, "mac_demo --verify: asked for %s, but the device came up on %s\n",
      backend::toString(wanted), backend::toString(surface->device()->getInfo().backend));
    return 1;
  }
  std::printf("mac_demo --verify: %s\n", surface->describeDevice().c_str());
  graphics::Canvas* canvas = surface->beginFrame();
  if (canvas == nullptr) {
    std::fprintf(stderr, "mac_demo --verify: beginFrame failed: %s\n", surface->diagnostics().c_str());
    return 1;
  }
  demo::drawFrame(*canvas, options.width, options.height, 0.0);
  if (!surface->submit()) {
    std::fprintf(stderr, "mac_demo --verify: submit failed: %s\n", surface->diagnostics().c_str());
    return 1;
  }
  const auto bitmap = surface->snapshot();
  if (bitmap.width() != static_cast<int>(options.width) ||
      bitmap.height() != static_cast<int>(options.height)) {
    std::fprintf(stderr, "mac_demo --verify: the snapshot is %dx%d, expected %ux%u\n", bitmap.width(),
      bitmap.height(), options.width, options.height);
    return 1;
  }
  std::printf("mac_demo --verify: drew and read back %dx%d pixels\n", bitmap.width(), bitmap.height());
  return 0;
}

/** @return the backing-pixel size of @p view, which is what the GPU target must match. */
void backingSize(NSView* view, uint32_t* width, uint32_t* height) {
  const NSRect backing = [view convertRectToBacking:[view bounds]];
  *width = static_cast<uint32_t>(std::max(1.0, std::round(backing.size.width)));
  *height = static_cast<uint32_t>(std::max(1.0, std::round(backing.size.height)));
}

/** Creates the application menu and the window both window paths share. */
NSWindow* createWindow(const demo::Options& options, const char* title) {
  [NSApplication sharedApplication];
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

  // A minimal application menu, so Cmd-Q works like every other macOS app.
  NSMenu* menuBar = [[NSMenu alloc] init];
  NSMenuItem* applicationItem = [[NSMenuItem alloc] init];
  [menuBar addItem:applicationItem];
  NSMenu* applicationMenu = [[NSMenu alloc] init];
  [applicationMenu addItemWithTitle:@"Quit Coral Demo" action:@selector(terminate:) keyEquivalent:@"q"];
  [applicationItem setSubmenu:applicationMenu];
  [NSApp setMainMenu:menuBar];

  const NSRect frame = NSMakeRect(0, 0, options.width, options.height);
  NSWindow* window = [[NSWindow alloc]
      initWithContentRect:frame
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                          NSWindowStyleMaskResizable
                  backing:NSBackingStoreBuffered
                    defer:NO];
  [window setTitle:[NSString stringWithUTF8String:title]];
  [window center];
  return window;
}

/**
 * The frame loop both window paths share: pump events, follow the view's backing size, draw, present.
 *
 * @return 0 on success, 1 when a frame failed.
 */
int runWindowLoop(graphics::gpu::WindowSurface& surface, NSView* view, NSWindow* window,
    const demo::Options& options, uint32_t width, uint32_t height) {
  id<CoralDemoStoppable> stoppable = (id<CoralDemoStoppable>) view;
  const double frameInterval = options.immediate ? 0.0 : 1.0 / 60.0;
  const auto started = std::chrono::steady_clock::now();
  uint32_t presented = 0;
  bool failed = false;

  while (![stoppable shouldStop] && window.isVisible &&
         (options.frames == 0 || presented < options.frames)) {
    @autoreleasepool {
      // Sleep until the next frame unless an event arrives first, then drain everything that is pending so a
      // burst of input cannot starve the renderer.
      NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                          untilDate:[NSDate dateWithTimeIntervalSinceNow:frameInterval]
                                             inMode:NSDefaultRunLoopMode
                                            dequeue:YES];
      while (event != nil) {
        [NSApp sendEvent:event];
        event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                    untilDate:[NSDate distantPast]
                                       inMode:NSDefaultRunLoopMode
                                      dequeue:YES];
      }
      if ([stoppable shouldStop] || !window.isVisible) {
        break;
      }

      uint32_t currentWidth = 0;
      uint32_t currentHeight = 0;
      backingSize(view, &currentWidth, &currentHeight);
      if (currentWidth != width || currentHeight != height) {
        if (!surface.resize(currentWidth, currentHeight)) {
          std::fprintf(stderr, "mac_demo: resize failed: %s\n", surface.diagnostics().c_str());
          failed = true;
          break;
        }
        width = currentWidth;
        height = currentHeight;
      }

      graphics::Canvas* canvas = surface.beginFrame();
      if (canvas == nullptr) {
        std::fprintf(stderr, "mac_demo: beginFrame failed: %s\n", surface.diagnostics().c_str());
        failed = true;
        break;
      }
      const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      demo::drawFrame(*canvas, width, height, seconds);
      if (!surface.present()) {
        std::fprintf(stderr, "mac_demo: present failed: %s\n", surface.diagnostics().c_str());
        failed = true;
        break;
      }
      ++presented;
      if (!options.snapshot.empty() && !writeSnapshot(surface, options.snapshot)) {
        failed = true;
        break;
      }
    }
  }

  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::printf("mac_demo: presented %u frame(s) in %.2f s (%.1f fps)\n", presented, elapsed,
    elapsed > 0.0 ? presented / elapsed : 0.0);
  return failed ? 1 : 0;
}

int runWindowMetal(const demo::Options& options) {
  NSWindow* window = createWindow(options, "Coral macOS demo - Metal / CAMetalLayer");

  CoralMetalView* view = [[CoralMetalView alloc] initWithFrame:NSMakeRect(0, 0, options.width, options.height)];
  [view setWantsLayer:YES];
  CALayer* backing = view.layer;
  if (![backing isKindOfClass:[CAMetalLayer class]]) {
    std::fprintf(stderr, "mac_demo: the view's backing layer is not a CAMetalLayer\n");
    return 1;
  }
  CAMetalLayer* layer = (CAMetalLayer*) backing;
  // The layer's contents scale decides the default drawable size; the surface's pixel size is set from the backing
  // bounds below, so this only has to agree with the display the window is on.
  layer.contentsScale = window.backingScaleFactor;

  [window setContentView:view];
  [window makeFirstResponder:view];
  [window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
  [NSApp finishLaunching];

  uint32_t width = 0;
  uint32_t height = 0;
  backingSize(view, &width, &height);

  graphics::gpu::WindowSurface::Configuration configuration;
  // A CAMetalLayer is the drawable: the backend acquires and presents through it, and there is no context to adopt.
  configuration.nativeWindow = (__bridge void*) layer;
  configuration.nativeWindowSystem = backend::NativeWindowSystem::Metal;
  configuration.width = width;
  configuration.height = height;
  configuration.preferred = backend::Backend::Metal;
  configuration.presentMode = options.immediate ? backend::PresentMode::Immediate : backend::PresentMode::Fifo;

  auto surface = graphics::gpu::WindowSurface::make(configuration);
  if (surface == nullptr) {
    std::fprintf(stderr,
      "mac_demo: no Metal window surface could be created for this CAMetalLayer (is a Metal device available?)\n");
    return 1;
  }
  if (surface->device()->getInfo().backend != backend::Backend::Metal) {
    std::fprintf(stderr, "mac_demo: asked for Metal, but the window's device came up on %s\n",
      backend::toString(surface->device()->getInfo().backend));
    return 1;
  }
  std::printf("mac_demo: %s\n", surface->describeDevice().c_str());
  std::printf("mac_demo: present mode %s, %ux%u pixels\n",
    surface->presentMode() == backend::PresentMode::Fifo ? "Fifo" : "Immediate", width, height);

  const int result = runWindowLoop(*surface, view, window, options, width, height);
  surface.reset();
  return result;
}

int runWindowOpenGL(const demo::Options& options) {
  NSWindow* window = createWindow(options, "Coral macOS demo - CGL / OpenGL 4.1");

  // The 4.1 core profile is the highest macOS provides and the profile the RHI targets. There is no 4.2/4.3 fallback
  // to ask for; a failure here is a machine without a usable GL driver.
  NSOpenGLPixelFormatAttribute attributes[] = {
      NSOpenGLPFAOpenGLProfile,
      NSOpenGLProfileVersion4_1Core,
      NSOpenGLPFAColorSize,
      24,
      NSOpenGLPFAAlphaSize,
      8,
      NSOpenGLPFADepthSize,
      24,
      NSOpenGLPFAStencilSize,
      8,
      NSOpenGLPFADoubleBuffer,
      NSOpenGLPFAAccelerated,
      0};
  NSOpenGLPixelFormat* pixelFormat = [[NSOpenGLPixelFormat alloc] initWithAttributes:attributes];
  if (pixelFormat == nil) {
    std::fprintf(stderr, "mac_demo: no OpenGL 4.1 core pixel format is available\n");
    return 1;
  }

  const NSRect frame = NSMakeRect(0, 0, options.width, options.height);
  CoralDemoView* view = [[CoralDemoView alloc] initWithFrame:frame pixelFormat:pixelFormat];
  [view setWantsBestResolutionOpenGLSurface:YES];
  [window setContentView:view];
  [window makeFirstResponder:view];
  [window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
  [NSApp finishLaunching];

  NSOpenGLContext* context = [view openGLContext];
  if (context == nil) {
    std::fprintf(stderr, "mac_demo: the view has no OpenGL context\n");
    return 1;
  }
  [context makeCurrentContext];
  GLint swapInterval = options.immediate ? 0 : 1;
  [context setValues:&swapInterval forParameter:NSOpenGLContextParameterSwapInterval];

  uint32_t width = 0;
  uint32_t height = 0;
  backingSize(view, &width, &height);

  graphics::gpu::WindowSurface::Configuration configuration;
  // CGL ignores the window handle, but the facade uses it as the surface's identity. The drawable is the
  // NSOpenGLView the context is attached to; the context is what the backend adopts.
  configuration.nativeWindow = (__bridge void*) view;
  configuration.cglContext = (void*) [context CGLContextObj];
  configuration.width = width;
  configuration.height = height;
  configuration.preferred = backend::Backend::OpenGL;
  configuration.presentMode = options.immediate ? backend::PresentMode::Immediate : backend::PresentMode::Fifo;

  auto surface = graphics::gpu::WindowSurface::make(configuration);
  if (surface == nullptr) {
    std::fprintf(stderr, "mac_demo: no OpenGL window surface could be created for this view\n");
    return 1;
  }
  if (surface->device()->getInfo().backend != backend::Backend::OpenGL) {
    std::fprintf(stderr, "mac_demo: asked for OpenGL, but the window's device came up on %s\n",
      backend::toString(surface->device()->getInfo().backend));
    return 1;
  }
  std::printf("mac_demo: %s\n", surface->describeDevice().c_str());
  std::printf("mac_demo: present mode %s, %ux%u pixels\n",
    surface->presentMode() == backend::PresentMode::Fifo ? "Fifo" : "Immediate", width, height);

  const int result = runWindowLoop(*surface, view, window, options, width, height);
  surface.reset();
  [context clearDrawable];
  return result;
}

} // namespace anonymous

int main(int argc, char** argv) {
  demo::Options options;
  for (int index = 1; index < argc; ++index) {
    const char* argument = argv[index];
    const bool hasValue = index + 1 < argc;
    if (std::strcmp(argument, "--help") == 0 || std::strcmp(argument, "-h") == 0) {
      printUsage(argv[0]);
      return 0;
    }
    if (std::strcmp(argument, "--verify") == 0) {
      options.verify = true;
    } else if (std::strcmp(argument, "--immediate") == 0) {
      options.immediate = true;
    } else if (std::strcmp(argument, "--backend") == 0 && hasValue) {
      options.backend = argv[++index];
    } else if (std::strcmp(argument, "--width") == 0 && hasValue) {
      options.width = parseDimension(argv[++index], options.width);
    } else if (std::strcmp(argument, "--height") == 0 && hasValue) {
      options.height = parseDimension(argv[++index], options.height);
    } else if (std::strcmp(argument, "--frames") == 0 && hasValue) {
      options.frames = parseFrames(argv[++index]);
    } else if (std::strcmp(argument, "--snapshot") == 0 && hasValue) {
      options.snapshot = argv[++index];
    } else {
      std::fprintf(stderr, "mac_demo: unknown argument '%s'\n", argument);
      printUsage(argv[0]);
      return 2;
    }
  }

  if (options.backend != "auto" && options.backend != "metal" && options.backend != "opengl") {
    std::fprintf(stderr, "mac_demo: unknown backend '%s' (expected auto, metal or opengl)\n",
      options.backend.c_str());
    return 2;
  }
  const BackendSelection selection = resolveBackend(options);
  if (options.backend == "metal" && !backend::isBackendAvailable(backend::Backend::Metal)) {
    std::fprintf(stderr, "mac_demo: this build has no Metal backend\n");
    return 2;
  }
  if (options.backend == "opengl" && !backend::isBackendAvailable(backend::Backend::OpenGL)) {
    std::fprintf(stderr, "mac_demo: this build has no OpenGL backend\n");
    return 2;
  }

  @autoreleasepool {
    if (options.verify) {
      return verifyOffscreen(options);
    }
    return selection.metal ? runWindowMetal(options) : runWindowOpenGL(options);
  }
}
