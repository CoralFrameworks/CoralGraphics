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
 * @file GlPlatformGlx.cpp
 *
 * Linux context creation through GLX.
 *
 * The shape mirrors the Win32 path -- probe context first, then the real one -- but GLX makes the first
 * step slightly cheaper: `glXGetProcAddressARB` works without a current context, so
 * `glXCreateContextAttribsARB` can be resolved up front. A context is still created before that to
 * establish a connection to the X server, which the loader needs on some drivers.
 *
 * Two GLX-specific hazards are handled explicitly:
 *
 *  1. **`glXChooseFBConfig` must be probed without `GLX_DOUBLEBUFFER` when a window is not available.**
 *     Asking for a double-buffered visual for an offscreen drawable fails on many servers, so an
 *     offscreen request gets a pbuffer config instead.
 *  2. **`glXCreateContextAttribsARB` may return a context of a lower version than requested** rather
 *     than failing. The version is therefore read back with `glGetIntegerv(GL_MAJOR_VERSION)` after
 *     the context is current and compared against the minimum; a downgrade is an error.
 */

#include "private/backend/gl/GlPlatform.h"

#if defined(CORAL_BUILD_FOR_LINUX)

// Linux compiles EGL beside this file for Wayland. The public platform entry points are defined once
// by GlPlatformLinux.cpp, so this translation unit names its copy `glx...` and the dispatcher picks
// the one the descriptor asks for.
#define createPlatformContext createGlxPlatformContext
#define destroyPlatformContext destroyGlxPlatformContext
#define makePlatformContextCurrent makeGlxPlatformContextCurrent
#define releasePlatformContextCurrent releaseGlxPlatformContextCurrent
#define swapPlatformContextBuffers swapGlxPlatformContextBuffers
#define setPlatformSwapInterval setGlxPlatformSwapInterval
#define queryPlatformSurfaceSize queryGlxPlatformSurfaceSize
#define platformSupportsMailboxPresent platformGlxSupportsMailboxPresent

#include <GL/glx.h>
#include <X11/Xlib.h>

// `Success` must go: X11/X.h defines it as a macro, so every `Result::Success` below would expand to
// `Result::0`. `None`, `True` and `False` are deliberately kept -- the GLX attribute lists end on
// `None`, and removing it is what left four of them with an undeclared identifier.
#ifdef Success
#undef Success
#endif

// No `#undef None` / `#undef Status` here, unlike `GlPlatformWin32.cpp`. Those are needed there because `<windows.h>`
// defines both as macros and that file has to remove them before it can use GLX's `None` as an attribute-list
// terminator. This file includes no Windows header on any path, so there is nothing to undefine -- and the undefs were
// actively wrong, because `None` is *defined* by the `<X11/Xlib.h>` two lines up, so removing it left every attribute
// list below ending on an undeclared identifier. Four lists end that way.
//
// Guarding them with `#if defined(_WIN32)` does not work either: `_WIN32` is defined by the compiler rather than by an
// included header, so it is set even when this translation unit is being built as Linux.

#include <cstring>
#include <dlfcn.h>

namespace backend {
namespace gl {
namespace {

#ifndef GLX_CONTEXT_MAJOR_VERSION_ARB
#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092
#define GLX_CONTEXT_FLAGS_ARB 0x2094
#define GLX_CONTEXT_PROFILE_MASK_ARB 0x9126
#define GLX_CONTEXT_DEBUG_BIT_ARB 0x0001
#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB 0x0001
#endif

using GlXCreateContextAttribsARBProc = GLXContext (*)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
using GlXSwapIntervalEXTProc = void (*)(Display*, GLXDrawable, int);

GlXCreateContextAttribsARBProc gCreateContextAttribsARB = nullptr;
GlXSwapIntervalEXTProc gSwapIntervalEXT = nullptr;

bool gExtensionsLoaded = false;

/** Resolves the GLX extension entry points. Unlike WGL this needs no current context. */
void loadGlxExtensions() {
  if (gExtensionsLoaded) return;
  gExtensionsLoaded = true;

  // `glXGetProcAddressARB` returns `__GLXextFuncPtr`, a function pointer, and a function pointer does not convert to
  // another function pointer type without a cast. Each one is taken in two steps -- to the exact type first, then to
  // the one wanted -- so the intermediate is a function pointer rather than a `void*`. The single-step form that was
  // here cast a function pointer straight to `void*`, which MSVC rejects and GCC and Clang accept as an extension,
  // which is the only reason it survived: this file had never been given to a second compiler.
  gCreateContextAttribsARB = reinterpret_cast<GlXCreateContextAttribsARBProc>(reinterpret_cast<__GLXextFuncPtr>(
          glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXCreateContextAttribsARB"))));
  gSwapIntervalEXT = reinterpret_cast<GlXSwapIntervalEXTProc>(
          reinterpret_cast<__GLXextFuncPtr>(glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXSwapIntervalEXT"))));
}

/** The loader the device uses for GL entry points. */
/**
 * The loader the device calls for every GL entry point.
 *
 * A driver resolves almost everything itself. The exceptions are the GL 1.1 functions, which are exported by libGL
 * directly rather than through the extension mechanism -- so a null from `glXGetProcAddressARB` is not a failure, and
 * `dlsym` is consulted before giving up.
 *
 * The handle is a function-local static, so it is opened on first use and closed at exit. Nothing closes it earlier
 * because a symbol from it may be in use for the process's whole life.
 */
void* glxGlLoader(const char* name) {
  // Taken as a function pointer and converted once, rather than straight to `void*`: the latter is the form MSVC
  // rejects and GCC accepts, and it is what this line used to be.
  const __GLXextFuncPtr resolved = glXGetProcAddressARB(reinterpret_cast<const GLubyte*>(name));
  if (resolved != nullptr) return reinterpret_cast<void*>(resolved);

  static void* libGL = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
  return libGL != nullptr ? dlsym(libGL, name) : nullptr;
}

/**
 * An Xlib error sink scoped to context creation.
 *
 * GLX reports a bad fbconfig/window as an X protocol error, and Xlib's default handler calls exit(1).
 * That is fatal to a process whose job is to try several backends -- the device factory treats a
 * failed OpenGL attempt as "move on to the next backend" -- so creation installs a handler that
 * records the error and lets the call return null instead. The previous handler is restored when the
 * scope ends; nothing outside this file sees the temporary one.
 *
 * Xlib's handler is process-global, which is why this is kept to the one call that creates a
 * context. The backend's device creation is externally synchronised, so no second thread is inside
 * it at the same time.
 */
int gLastXError = 0;

int recordXError(Display*, XErrorEvent* event) {
  gLastXError = event != nullptr ? event->error_code : 0;
  return 0;
}

class ScopedXErrorHandler {
public:
  explicit ScopedXErrorHandler(Display* display) : mDisplay(display) {
    gLastXError = 0;
    mPrevious = XSetErrorHandler(recordXError);
  }

  ~ScopedXErrorHandler() {
    if (mDisplay != nullptr) XSync(mDisplay, False);
    XSetErrorHandler(mPrevious);
  }

  ScopedXErrorHandler(const ScopedXErrorHandler&) = delete;
  ScopedXErrorHandler& operator=(const ScopedXErrorHandler&) = delete;

  /** The display is opened by the caller on the path that owns one; bind it before the GLX calls. */
  void setDisplay(Display* display) {
    mDisplay = display;
  }

  /** Flushes pending X errors and reports whether any arrived. */
  bool failed() {
    if (mDisplay != nullptr) XSync(mDisplay, False);
    return gLastXError != 0;
  }

private:
  Display* mDisplay = nullptr;
  XErrorHandler mPrevious = nullptr;
};

/** A hidden 1x1 window used when the caller wants an offscreen context. */
struct HelperWindow {
  Display* display = nullptr;
  Window window = 0;
  Colormap colormap = 0;
};

HelperWindow gHelper;

/** Creates the hidden helper window and its colormap, idempotently. */
Result ensureHelperWindow(Display* display, int screen) {
  if (gHelper.window != 0) return Result::Success;

  XSetWindowAttributes attributes = {};
  gHelper.colormap = XCreateColormap(display, RootWindow(display, screen), DefaultVisual(display, screen), AllocNone);
  attributes.colormap = gHelper.colormap;
  attributes.event_mask = StructureNotifyMask;

  gHelper.window = XCreateWindow(display, RootWindow(display, screen), 0, 0, 1, 1, 0, DefaultDepth(display, screen),
          InputOutput, DefaultVisual(display, screen), CWColormap | CWEventMask, &attributes);
  if (gHelper.window == 0) {
    XFreeColormap(display, gHelper.colormap);
    gHelper.colormap = 0;
    return Result::Unsupported;
  }

  gHelper.display = display;
  return Result::Success;
}

/** Chooses an FBConfig, preferring a window-capable double-buffered one when a window exists. */
GLXFBConfig chooseFbConfig(Display* display, int screen, bool forWindow) {
  // The attribute list must be terminated by None (0), which is why each branch repeats it.
  const int windowAttributes[] = { GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_RENDER_TYPE,
    GLX_RGBA_BIT, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8, GLX_DEPTH_SIZE, 24,
    GLX_STENCIL_SIZE, 8, GLX_DOUBLEBUFFER, True, None };

  const int pbufferAttributes[] = { GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT, GLX_RENDER_TYPE,
    GLX_RGBA_BIT, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8, GLX_DEPTH_SIZE, 24,
    GLX_STENCIL_SIZE, 8, None };

  int configCount = 0;
  GLXFBConfig* configs =
          glXChooseFBConfig(display, screen, forWindow ? windowAttributes : pbufferAttributes, &configCount);
  if (configs == nullptr || configCount == 0) {
    // Fall back to the other kind: a server that refuses a pbuffer config may still accept a window
    // one, and vice versa.
    configs = glXChooseFBConfig(display, screen, forWindow ? pbufferAttributes : windowAttributes, &configCount);
    if (configs == nullptr || configCount == 0) return nullptr;
  }

  const GLXFBConfig chosen = configs[0];
  XFree(configs);
  return chosen;
}

} // namespace anonymous

Result createPlatformContext(const SurfaceDescriptor& descriptor, GlDialect dialect, GlVersion minimum,
        bool debugContext, GlPlatformContext* context) {
  // Linux desktops have no OpenGL ES driver; the ES dialect is available only through a shim such as
  // ANGLE or a Mesa ES-specific libGLESv2, which the RHI does not depend on. Refusing beats silently
  // handing back a desktop context the caller did not ask for.
  if (dialect == GlDialect::ES) {
    return Result::Unsupported;
  }
  if (context == nullptr) return Result::InvalidArgument;

  // Every X call from here until the guard goes out of scope may report a protocol error instead of
  // returning a status. See ScopedXErrorHandler.
  ScopedXErrorHandler xErrors(static_cast<Display*>(descriptor.nativeDisplay));

  loadGlxExtensions();

  // ---------------------------------------------------------------- adopt an existing context
  if (descriptor.nativeWindow != nullptr && descriptor.nativeDisplay != nullptr) {
    context->display = descriptor.nativeDisplay;
    context->surface = descriptor.nativeWindow;
    context->context = descriptor.glxContext;
    context->ownsDisplay = false;
    context->ownsSurface = false;
    context->ownsContext = false;
    context->dialect = GlDialect::Desktop;
    context->nativeApi = NativeApi::Glx;
    context->loader = &glxGlLoader;

    if (glXMakeCurrent(static_cast<Display*>(context->display), static_cast<GLXDrawable>(uintptr_t(context->surface)),
                static_cast<GLXContext>(context->context)) == False ||
        xErrors.failed()) {
      return Result::Error;
    }
    return Result::Success;
  }

  // ---------------------------------------------------------------- create our own context
  Display* display = static_cast<Display*>(descriptor.nativeDisplay);
  bool ownsDisplay = false;
  if (display == nullptr) {
    display = XOpenDisplay(nullptr);
    if (display == nullptr) return Result::Unsupported;
    ownsDisplay = true;
  }
  xErrors.setDisplay(display);

  const int screen = DefaultScreen(display);
  const bool forWindow = descriptor.nativeWindow != nullptr;

  GLXFBConfig config = chooseFbConfig(display, screen, forWindow);
  if (config == nullptr) {
    if (ownsDisplay) XCloseDisplay(display);
    return Result::Unsupported;
  }

  // GLX_ARB_create_context is required: without it there is no way to request a version, and the
  // default context a server hands out may be as old as 1.4.
  if (gCreateContextAttribsARB == nullptr) {
    if (ownsDisplay) XCloseDisplay(display);
    return Result::Unsupported;
  }

  const int attributes[] = { GLX_CONTEXT_MAJOR_VERSION_ARB, static_cast<int>(minimum.major),
    GLX_CONTEXT_MINOR_VERSION_ARB, static_cast<int>(minimum.minor),
    // Core profile: no deprecated built-ins, GLSL 4.10 without compatibility extensions. This
    // is what the shader compiler targets.
    GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB, GLX_CONTEXT_FLAGS_ARB,
    debugContext ? GLX_CONTEXT_DEBUG_BIT_ARB : 0, None };

  GLXContext renderContext = gCreateContextAttribsARB(display, config, nullptr, True, attributes);
  if (renderContext == nullptr && debugContext) {
    // Some drivers reject the debug bit rather than ignoring it; losing validation output is better
    // than losing the device.
    const int fallbackAttributes[] = { GLX_CONTEXT_MAJOR_VERSION_ARB, static_cast<int>(minimum.major),
      GLX_CONTEXT_MINOR_VERSION_ARB, static_cast<int>(minimum.minor), GLX_CONTEXT_PROFILE_MASK_ARB,
      GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None };
    renderContext = gCreateContextAttribsARB(display, config, nullptr, True, fallbackAttributes);
  }

  if (renderContext == nullptr || xErrors.failed()) {
    if (renderContext != nullptr) glXDestroyContext(display, renderContext);
    if (ownsDisplay) XCloseDisplay(display);
    return Result::Unsupported;
  }

  // The drawable: the caller's window, or a pbuffer when there is none.
  GLXDrawable drawable = 0;
  bool ownsDrawable = false;

  if (descriptor.nativeWindow != nullptr) {
    drawable = static_cast<GLXDrawable>(uintptr_t(descriptor.nativeWindow));
  } else {
    const int pbufferAttributes[] = { GLX_PBUFFER_WIDTH, 1, GLX_PBUFFER_HEIGHT, 1, None };
    drawable = glXCreatePbuffer(display, config, pbufferAttributes);
    ownsDrawable = drawable != 0;
  }

  if (drawable == 0 || xErrors.failed()) {
    if (drawable != 0) glXDestroyPbuffer(display, drawable);
    glXDestroyContext(display, renderContext);
    if (ownsDisplay) XCloseDisplay(display);
    return Result::Unsupported;
  }

  if (glXMakeCurrent(display, drawable, renderContext) == False || xErrors.failed()) {
    if (ownsDrawable) glXDestroyPbuffer(display, drawable);
    glXDestroyContext(display, renderContext);
    if (ownsDisplay) XCloseDisplay(display);
    return Result::Error;
  }

  if (gSwapIntervalEXT != nullptr) gSwapIntervalEXT(display, drawable, 1);

  context->display = display;
  context->surface = reinterpret_cast<void*>(uintptr_t(drawable));
  context->context = renderContext;
  context->ownsDisplay = ownsDisplay;
  context->ownsSurface = ownsDrawable;
  context->ownsContext = true;
  context->dialect = GlDialect::Desktop;
  context->nativeApi = NativeApi::Glx;
  context->loader = &glxGlLoader;
  context->coreProfile = true;
  return Result::Success;
}

void destroyPlatformContext(GlPlatformContext* context) {
  if (context == nullptr) return;

  Display* display = static_cast<Display*>(context->display);
  if (display == nullptr) return;

  if (context->context != nullptr && context->ownsContext) {
    glXMakeCurrent(display, 0, nullptr);
    glXDestroyContext(display, static_cast<GLXContext>(context->context));
  }
  if (context->surface != nullptr && context->ownsSurface) {
    glXDestroyPbuffer(display, static_cast<GLXDrawable>(uintptr_t(context->surface)));
  }
  if (context->ownsDisplay) XCloseDisplay(display);

  context->context = nullptr;
  context->surface = nullptr;
  context->display = nullptr;
  context->ownsContext = false;
  context->ownsSurface = false;
  context->ownsDisplay = false;
}

Result releasePlatformContextCurrent(const GlPlatformContext& context) {
  // GLX's unbind is `glXMakeCurrent(display, None, nullptr)`: no drawable and no context. It is
  // per-thread, which is the contract this function documents -- it cannot unbind a context that is
  // current on another thread. Returning Unsupported without a display keeps the "nothing current"
  // case safe instead of dereferencing a null Display.
  Display* display = static_cast<Display*>(context.display);
  if (display == nullptr) return Result::Unsupported;
  return glXMakeCurrent(display, 0, nullptr) != False ? Result::Success : Result::Error;
}

Result makePlatformContextCurrent(const GlPlatformContext& context) {
  if (context.display == nullptr || context.context == nullptr) return Result::Unsupported;
  const Bool result = glXMakeCurrent(static_cast<Display*>(context.display),
          static_cast<GLXDrawable>(uintptr_t(context.surface)), static_cast<GLXContext>(context.context));
  return result != False ? Result::Success : Result::Error;
}

bool platformSupportsMailboxPresent() noexcept {
  // False on purpose: a swap interval of 0 stops waiting for vertical sync, it does not drop a frame that the compositor has
  // not consumed yet. Reporting Mailbox from here would be a promise this platform cannot keep.
  return false;
}

Result setPlatformSwapInterval(const GlPlatformContext& context, const int interval) {
  if (gSwapIntervalEXT == nullptr || context.display == nullptr) return Result::Unsupported;
  // `surface` is the GLXDrawable, stored as an integer in a `void*` at creation (see `createPlatformContext`).
  gSwapIntervalEXT(static_cast<Display*>(context.display),
    static_cast<GLXDrawable>(reinterpret_cast<uintptr_t>(context.surface)), interval);
  return Result::Success;
}

Result swapPlatformContextBuffers(const GlPlatformContext& context) {
  if (context.display == nullptr || context.surface == nullptr) return Result::Success;
  glXSwapBuffers(static_cast<Display*>(context.display), static_cast<GLXDrawable>(uintptr_t(context.surface)));
  return Result::Success;
}

Extent2D queryPlatformSurfaceSize(const GlPlatformContext& context, const SurfaceDescriptor& descriptor) {
  if (descriptor.width != 0 && descriptor.height != 0) {
    return Extent2D{ descriptor.width, descriptor.height };
  }

  Display* display = static_cast<Display*>(context.display);
  if (display != nullptr && descriptor.nativeWindow != nullptr) {
    XWindowAttributes attributes = {};
    if (XGetWindowAttributes(display, static_cast<Window>(uintptr_t(descriptor.nativeWindow)), &attributes) != 0) {
      if (attributes.width > 0 && attributes.height > 0) {
        return Extent2D{ uint32_t(attributes.width), uint32_t(attributes.height) };
      }
    }
  }
  return Extent2D{ descriptor.width, descriptor.height };
}

} // namespace gl
} // namespace backend

#if defined(CORAL_BUILD_FOR_LINUX)
#undef createPlatformContext
#undef destroyPlatformContext
#undef makePlatformContextCurrent
#undef releasePlatformContextCurrent
#undef swapPlatformContextBuffers
#undef setPlatformSwapInterval
#undef queryPlatformSurfaceSize
#undef platformSupportsMailboxPresent
#endif

#endif // CORAL_BUILD_FOR_LINUX
