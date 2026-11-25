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
 * @file GlPlatformEgl.cpp
 *
 * EGL context creation, used on Android and as the fallback where no window-system backend
 * applies.
 *
 * EGL is the one context-creation API that is genuinely cross-platform, which makes it the right
 * choice for the mobile targets and for offscreen work in tests. It also has the cleanest version
 * story of the four: `EGL_CONTEXT_MAJOR_VERSION` / `EGL_CONTEXT_MINOR_VERSION` are attributes, so the
 * requested version is expressed directly instead of being read back afterwards.
 *
 * Two details matter:
 *
 *  1. **`EGL_OPENGL_ES3_BIT` must be in the config attributes on Android.** An ES 2.0-renderable
 *     config will happily create an ES 3.0 context on some drivers and quietly fail on others.
 *  2. **A pbuffer is the offscreen drawable.** EGL has one, unlike WGL, so the offscreen path needs no
 *     hidden window at all.
 */

#include "private/backend/gl/GlPlatform.h"

#include "private/backend/gl/GlEntryPoints.h"

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#if defined(CORAL_BUILD_FOR_ANDROID) || defined(CORAL_BUILD_FOR_LINUX)

#if defined(CORAL_BUILD_FOR_LINUX)
// Linux compiles this file beside GlPlatformGlx.cpp so that one binary can drive an X11 window
// through GLX and a Wayland window through EGL. The two implementations cannot both define the
// platform entry points, so this translation unit names its copy `egl...` and GlPlatformLinux.cpp
// dispatches between them.
#define createPlatformContext createEglPlatformContext
#define destroyPlatformContext destroyEglPlatformContext
#define makePlatformContextCurrent makeEglPlatformContextCurrent
#define releasePlatformContextCurrent releaseEglPlatformContextCurrent
#define swapPlatformContextBuffers swapEglPlatformContextBuffers
#define setPlatformSwapInterval setEglPlatformSwapInterval
#define queryPlatformSurfaceSize queryEglPlatformSurfaceSize
#define platformSupportsMailboxPresent platformEglSupportsMailboxPresent
#endif

#include <cstring>

#if defined(CORAL_BUILD_FOR_ANDROID)
#include <android/native_window.h>
#endif

namespace backend {
namespace gl {
namespace {

/** @return the process-wide EGL entry points, loading them on first use. */
EGLApi& egl() {
  EGLApi& api = getEGLApi();
  if (!api.loaded) api.loadEGL(nullptr);
  return api;
}

/**
 * The loader the device uses for GL entry points.
 *
 * `eglGetProcAddress` is only required to return *extension* entry points, and several drivers return
 * null for core ES functions. The platform library symbol is therefore consulted as well, through the
 * same fallback the entry point table uses for EGL itself.
 */
void* eglGlLoader(const char* name) { return resolveEGLSymbol(name); }

} // namespace anonymous

#if !defined(CORAL_BUILD_FOR_LINUX)
const char* GlPlatform::backendName() noexcept { return "EGL"; }
#endif

Result createPlatformContext(const SurfaceDescriptor& descriptor, GlDialect dialect, GlVersion minimum,
        bool debugContext, GlPlatformContext* context) {
  if (context == nullptr) return Result::InvalidArgument;

  EGLApi& api = egl();
  if (!api.loaded) return Result::Unsupported;

  // ---------------------------------------------------------------- adopt an existing context
  if (descriptor.eglContext != nullptr || descriptor.eglSurface != nullptr) {
    if (descriptor.eglDisplay == nullptr || descriptor.eglContext == nullptr) {
      return Result::InvalidArgument;
    }
    void* surface = descriptor.eglSurface;
    bool ownsSurface = false;
    // An adopted context may have been created for an offscreen pbuffer. The engine still renders to
    // the output ANativeWindow, so create a window surface with the adopted display/config/context.
    if (surface == nullptr && descriptor.nativeWindow != nullptr) {
      if (api.eglCreateWindowSurface == nullptr) {
        return Result::Unsupported;
      }
      EGLConfigHandle chosenConfig = nullptr;
      EGLConfig config = descriptor.eglConfig;
      if (config == nullptr && api.eglChooseConfig != nullptr) {
        const EGLint configAttributes[] = {
          EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
          EGL_RENDERABLE_TYPE, dialect == GlDialect::ES ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_BIT,
          EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
          EGL_NONE
        };
        EGLint count = 0;
        if (api.eglChooseConfig(descriptor.eglDisplay, configAttributes, &chosenConfig, 1, &count) == EGL_FALSE ||
            count == 0) {
          return Result::Unsupported;
        }
        config = static_cast<EGLConfig>(chosenConfig);
      }
      if (config == nullptr) return Result::Unsupported;
      const EGLint surfaceAttributes[] = { EGL_NONE };
      surface = api.eglCreateWindowSurface(descriptor.eglDisplay, config,
              descriptor.nativeWindow, surfaceAttributes);
      if (surface == nullptr || surface == EGL_NO_SURFACE) {
        return Result::Error;
      }
      ownsSurface = true;
    }
    context->display = descriptor.eglDisplay;
    context->surface = surface;
    context->context = descriptor.eglContext;
    context->ownsDisplay = false;
    context->ownsSurface = ownsSurface;
    context->ownsContext = false;
    context->dialect = dialect;
    context->nativeApi = NativeApi::Egl;
    context->loader = &eglGlLoader;

    if (api.eglMakeCurrent != nullptr && context->display != nullptr &&
            !api.eglMakeCurrent(context->display, context->surface, context->surface, context->context)) {
      if (ownsSurface && api.eglDestroySurface != nullptr) {
        api.eglDestroySurface(context->display, context->surface);
      }
      return Result::Error;
    }
    return Result::Success;
  }

  // ---------------------------------------------------------------- create our own context
  EGLDisplayHandle display = EGL_NO_DISPLAY;

#if defined(CORAL_BUILD_FOR_LINUX)
  if (api.eglGetPlatformDisplay != nullptr) {
    uint32_t platform = EGL_PLATFORM_SURFACELESS_MESA;
    void* nativeDisplay = descriptor.nativeDisplay;
    if (descriptor.nativeWindowSystem == NativeWindowSystem::Wayland) {
      platform = EGL_PLATFORM_WAYLAND_KHR;
    } else if (descriptor.nativeWindowSystem == NativeWindowSystem::X11) {
      platform = EGL_PLATFORM_X11_KHR;
    } else if (descriptor.nativeWindow != nullptr || descriptor.nativeDisplay != nullptr) {
      // An unspecified kind with a window/display is the historical X11 caller.
      platform = EGL_PLATFORM_X11_KHR;
    } else {
      // Offscreen: EGL's surfaceless platform is what Mesa exposes for a context with no display.
      // It is the path a headless framework build uses on a machine with no X server, and it is also what
      // lets a Wayland/X11 window fall back when GLX is unavailable.
      platform = EGL_PLATFORM_SURFACELESS_MESA;
      nativeDisplay = nullptr;
    }
    display = api.eglGetPlatformDisplay(platform, nativeDisplay, nullptr);
  }
#endif

  if (display == EGL_NO_DISPLAY) {
    display = api.eglGetDisplay(descriptor.nativeDisplay);
  }
  if (display == nullptr || display == EGL_NO_DISPLAY) return Result::Unsupported;

  EGLint major = 0;
  EGLint minor = 0;
  if (!api.eglInitialize(display, &major, &minor)) return Result::Unsupported;

  // EGL's default client API is OpenGL ES. A desktop request that did not bind OpenGL first is
  // rejected by Mesa with EGL_BAD_MATCH, which is exactly what an offscreen Linux device hit before
  // this call was added.
  if (api.eglBindAPI != nullptr &&
      !api.eglBindAPI(dialect == GlDialect::Desktop ? EGL_OPENGL_API : EGL_OPENGL_ES_API)) {
    api.eglTerminate(display);
    return Result::Unsupported;
  }

  // The renderable-type bit differs between the dialects, and getting it wrong is the classic cause
  // of "context creation succeeded but nothing renders": an ES 2.0-renderable config will accept an
  // ES 3.0 context request and then refuse to give it the features it needs.
  const EGLint renderableType = dialect == GlDialect::ES ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_BIT;
  const bool forWindow = descriptor.nativeWindow != nullptr;

  const EGLint configAttributes[] = { EGL_SURFACE_TYPE, forWindow ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT,
    EGL_RENDERABLE_TYPE, renderableType, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE };

  EGLConfigHandle config = nullptr;
  EGLint configCount = 0;
  if (!api.eglChooseConfig(display, configAttributes, &config, 1, &configCount) || configCount == 0) {
    api.eglTerminate(display);
    return Result::Unsupported;
  }

  // The drawable.
  void* surface = nullptr;
  bool ownsSurface = false;
  if (forWindow && api.eglCreateWindowSurface != nullptr) {
    const EGLint surfaceAttributes[] = { EGL_NONE };
    // Wayland separates the Vulkan `wl_surface*` from the EGL `wl_egl_window*` that wraps it; the
    // descriptor carries both, and the window-system extra is the one EGL wants.
    void* eglNativeWindow = descriptor.nativeWindowExtra != nullptr ? descriptor.nativeWindowExtra
                                                                    : descriptor.nativeWindow;
    surface = api.eglCreateWindowSurface(display, config, eglNativeWindow, surfaceAttributes);
    ownsSurface = surface != nullptr && surface != EGL_NO_SURFACE;
  } else if (api.eglCreatePbufferSurface != nullptr) {
    const uint32_t width = descriptor.width != 0 ? descriptor.width : 1;
    const uint32_t height = descriptor.height != 0 ? descriptor.height : 1;
    const EGLint pbufferAttributes[] = { EGL_WIDTH, EGLint(width), EGL_HEIGHT, EGLint(height), EGL_NONE };
    surface = api.eglCreatePbufferSurface(display, config, pbufferAttributes);
    ownsSurface = surface != nullptr && surface != EGL_NO_SURFACE;
  }

  if (surface == nullptr || surface == EGL_NO_SURFACE) {
    api.eglTerminate(display);
    return Result::Unsupported;
  }

  // The version attributes are the portable request. `EGL_CONTEXT_OPENGL_PROFILE_MASK` is
  // deliberately not sent: Mesa rejects it on the surfaceless platform even though it accepts the
  // same 4.1 request without it, and a 4.1 context is core by construction on the drivers this
  // backend runs on. The version check in GlPlatform is what verifies what was actually granted.
  const EGLint contextAttributes[] = { EGL_CONTEXT_MAJOR_VERSION, EGLint(minimum.major), EGL_CONTEXT_MINOR_VERSION,
    EGLint(minimum.minor), EGL_NONE };

  // EGL takes the share context at creation, so a group is expressed by passing the root's `EGLContext` here. If the driver
  // refuses the share it is retried unshared rather than failed: a device that cannot share still renders, and losing the
  // group is recoverable while losing the device is not.
  auto* shareContext = static_cast<EGLContext>(descriptor.glShareContext);
#if defined(__ANDROID__)
  __android_log_print(ANDROID_LOG_INFO, "CoralGlEgl", "creating context share=%p window=%p", descriptor.glShareContext,
          descriptor.nativeWindow);
#endif
  void* renderContext = api.eglCreateContext != nullptr
                                ? api.eglCreateContext(display, config, shareContext != nullptr ? shareContext : EGL_NO_CONTEXT,
                                        contextAttributes)
                                : nullptr;
  if ((renderContext == nullptr || renderContext == EGL_NO_CONTEXT) && shareContext != nullptr) {
    // Not reported: this translation unit has no message sink. The facade's `sharedWithGroup()` is what a caller checks,
    // and the retry below is what keeps a refused share from costing the device.
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_ERROR, "CoralGlEgl",
            "shared EGL context was refused (error 0x%x); retrying unshared", api.eglGetError());
#endif
    renderContext = api.eglCreateContext != nullptr ? api.eglCreateContext(display, config, EGL_NO_CONTEXT,
            contextAttributes) : nullptr;
  }
  if (renderContext == nullptr || renderContext == EGL_NO_CONTEXT) {
    if (ownsSurface) api.eglDestroySurface(display, surface);
    api.eglTerminate(display);
    return Result::Unsupported;
  }

  if (!api.eglMakeCurrent(display, surface, surface, renderContext)) {
    api.eglDestroyContext(display, renderContext);
    if (ownsSurface) api.eglDestroySurface(display, surface);
    api.eglTerminate(display);
    return Result::Error;
  }

  context->display = display;
  context->surface = surface;
  context->context = renderContext;
  context->config = config;
  context->ownsDisplay = true;
  context->ownsSurface = ownsSurface;
  context->ownsContext = true;
  context->dialect = dialect;
  context->nativeApi = NativeApi::Egl;
  context->loader = &eglGlLoader;
  context->coreProfile = dialect == GlDialect::Desktop;

  // A debug context needs `EGL_CONTEXT_OPENGL_DEBUG` (EGL 1.5) or the KHR_debug extension; the
  // backend reports debug output as available through GL_KHR_debug instead of requesting it here.
  (void) debugContext;
  return Result::Success;
}

void destroyPlatformContext(GlPlatformContext* context) {
  if (context == nullptr) return;

  EGLApi& api = egl();
  if (!api.loaded || context->display == nullptr) return;

  if (api.eglMakeCurrent != nullptr) {
    api.eglMakeCurrent(context->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
  if (context->ownsContext && context->context != nullptr && api.eglDestroyContext != nullptr) {
    api.eglDestroyContext(context->display, context->context);
  }
  if (context->ownsSurface && context->surface != nullptr && api.eglDestroySurface != nullptr) {
    api.eglDestroySurface(context->display, context->surface);
  }
  if (context->ownsDisplay && api.eglTerminate != nullptr) {
    api.eglTerminate(context->display);
  }

  context->context = nullptr;
  context->surface = nullptr;
  context->display = nullptr;
  context->ownsContext = false;
  context->ownsSurface = false;
  context->ownsDisplay = false;
}

Result releasePlatformContextCurrent(const GlPlatformContext& context) {
  // EGL's unbind: no draw/read surface and no context, on the display this context belongs to. Harmless when the thread has
  // none current -- `eglMakeCurrent` with no surface is legal and simply detaches.
  EGLApi& api = egl();
  if (!api.loaded || api.eglMakeCurrent == nullptr || context.display == nullptr) return Result::Unsupported;
  return api.eglMakeCurrent(context.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) ? Result::Success
                                                                                            : Result::Error;
}

Result makePlatformContextCurrent(const GlPlatformContext& context) {
  EGLApi& api = egl();
  if (!api.loaded || api.eglMakeCurrent == nullptr || context.display == nullptr) return Result::Unsupported;

  if (!api.eglMakeCurrent(context.display, context.surface, context.surface, context.context)) {
    const EGLint error = api.eglGetError != nullptr ? api.eglGetError() : 0;
    if (error == EGL_CONTEXT_LOST) return Result::DeviceLost;
    return Result::Error;
  }
  return Result::Success;
}

Result bindPlatformSurface(GlPlatformContext& context, const SurfaceDescriptor& descriptor) {
  EGLApi& api = egl();
  if (!api.loaded || api.eglCreateWindowSurface == nullptr || api.eglMakeCurrent == nullptr ||
      context.display == nullptr || context.context == nullptr || context.config == nullptr ||
      descriptor.nativeWindow == nullptr) {
    return Result::Unsupported;
  }

  EGLDisplay display = static_cast<EGLDisplay>(context.display);
  EGLConfig config = static_cast<EGLConfig>(context.config);
  const EGLint attributes[] = { EGL_NONE };
  // Wayland hands EGL a `wl_egl_window`, not the `wl_surface`; the creation path already picks the
  // extra handle, and binding a second window to a shared device has to use the same one.
  void* eglNativeWindow = descriptor.nativeWindowExtra != nullptr
    ? descriptor.nativeWindowExtra : descriptor.nativeWindow;
  void* surface = api.eglCreateWindowSurface(display, config, eglNativeWindow, attributes);
  if (surface == nullptr || surface == EGL_NO_SURFACE) return Result::Error;

  if (!api.eglMakeCurrent(display, surface, surface, static_cast<EGLContext>(context.context))) {
    api.eglDestroySurface(display, surface);
    return Result::Error;
  }

  if (context.ownsSurface && context.surface != nullptr && context.surface != surface) {
    api.eglDestroySurface(display, context.surface);
  }
  context.surface = surface;
  context.ownsSurface = true;
  return Result::Success;
}

bool platformSupportsMailboxPresent() noexcept {
  // False on purpose: a swap interval of 0 stops waiting for vertical sync, it does not drop a frame that the compositor has
  // not consumed yet. Reporting Mailbox from here would be a promise this platform cannot keep.
  return false;
}

Result setPlatformSwapInterval(const GlPlatformContext& context, const int interval) {
  EGLApi& api = egl();
  if (!api.loaded || api.eglSwapInterval == nullptr || context.display == nullptr) return Result::Unsupported;
  // EGL applies the interval to the current draw surface of the display, which is why it must be called with this context's
  // display named explicitly rather than whatever is current.
  return api.eglSwapInterval(context.display, interval) ? Result::Success : Result::Unsupported;
}

Result swapPlatformContextBuffers(const GlPlatformContext& context) {
  EGLApi& api = egl();
  if (!api.loaded || api.eglSwapBuffers == nullptr) return Result::Unsupported;

  // A pbuffer has nothing to present, which is a success rather than a failure.
  if (context.display == nullptr || context.surface == nullptr) return Result::Success;

  if (!api.eglSwapBuffers(context.display, context.surface)) {
    const EGLint error = api.eglGetError != nullptr ? api.eglGetError() : 0;
    if (error == EGL_CONTEXT_LOST) return Result::DeviceLost;
    if (error == EGL_BAD_SURFACE || error == EGL_BAD_NATIVE_WINDOW) return Result::OutOfDate;
    return Result::Error;
  }
  return Result::Success;
}

Extent2D queryPlatformSurfaceSize(const GlPlatformContext& context, const SurfaceDescriptor& descriptor) {
  // A pinned size wins: the caller told us what the surface is, and re-querying every frame is both
  // slower and racy against a concurrent window resize.
  if (descriptor.width != 0 && descriptor.height != 0) {
    return Extent2D{ descriptor.width, descriptor.height };
  }

  EGLApi& api = egl();
  if (api.loaded && api.eglQuerySurface != nullptr && context.display != nullptr && context.surface != nullptr) {
    EGLint width = 0;
    EGLint height = 0;
    if (api.eglQuerySurface(context.display, context.surface, EGL_WIDTH, &width) &&
            api.eglQuerySurface(context.display, context.surface, EGL_HEIGHT, &height) && width > 0 && height > 0) {
      return Extent2D{ uint32_t(width), uint32_t(height) };
    }
  }

#if defined(CORAL_BUILD_FOR_ANDROID)
  if (descriptor.nativeWindow != nullptr) {
    auto* window = static_cast<ANativeWindow*>(descriptor.nativeWindow);
    const int32_t width = ANativeWindow_getWidth(window);
    const int32_t height = ANativeWindow_getHeight(window);
    if (width > 0 && height > 0) return Extent2D{ uint32_t(width), uint32_t(height) };
  }
#endif

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

#endif // CORAL_BUILD_FOR_ANDROID || CORAL_BUILD_FOR_LINUX
