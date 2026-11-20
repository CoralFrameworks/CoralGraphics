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
 * @file GlPlatformWin32.cpp
 *
 * Windows context creation through WGL.
 *
 * Three things make this more involved than it looks:
 *
 *  1. **A dummy context is mandatory.** `wglCreateContextAttribsARB` is not exported by
 *     `opengl32.dll`; it is a driver extension reached through `wglGetProcAddress`, and that function
 *     only returns anything useful once *a* context is current. So a throwaway context is created
 *     first, its extension pointers are resolved, and only then is the real 4.1 core context
 *     requested.
 *  2. **`SetPixelFormat` may be called only once per window.** Calling it twice on the same HDC fails
 *     with no useful diagnostic, so the pixel format is set once on a hidden helper window that the
 *     application's window never sees.
 *  3. **A window is needed even for offscreen work.** WGL has no pbuffer-free path to a context, so
 *     an offscreen request gets a hidden 1x1 window and simply never presents.
 *
 * The public interface hides all of this; `GlPlatform` asks for "4.1 core, maybe offscreen" and gets
 * a current context or a specific failure.
 */

#include "private/backend/gl/GlPlatform.h"

#if defined(CORAL_BUILD_FOR_WIN)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
#include <windows.h>
// clang-format on

#include <cstring>

namespace backend {
namespace gl {
namespace {

// ------------------------------------------------------------------------------------------------
// WGL extension entry points
//
// Declared by hand rather than by including <wingdi.h>'s wglext additions, which are not present in
// every SDK. Calling conventions matter: WINAPI is __stdcall on 32 bit Windows, and getting it wrong
// corrupts the stack in a way that looks like driver instability.
// ------------------------------------------------------------------------------------------------

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_FLAGS_ARB 0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_DEBUG_BIT_ARB 0x0001
#define WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB 0x0002
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x0001
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x0002

/** The WGL extension functions the backend needs after a context exists. */
struct WglExtensions {
  HGLRC(WINAPI* CreateContextAttribsARB)(HDC, HGLRC, const int*) = nullptr;
  BOOL(WINAPI* SwapIntervalEXT)(int) = nullptr;

  [[nodiscard]] bool valid() const noexcept { return CreateContextAttribsARB != nullptr; }
};

WglExtensions gWglExtensions;

/**
 * A hidden helper window.
 *
 * The window and its device context belong to the thread that creates them: Windows destroys a
 * window with its owning thread, and WGL requires a valid HDC of the calling thread. A render task
 * is also thread-scoped, so the helper is thread-local rather than process-global; a context created
 * by one task's render thread cannot be created again on the next task's thread through a helper
 * whose owner has exited.
 *
 * Created once per thread and reused for every context on that thread, because `SetPixelFormat` may
 * only be called once per HDC and Windows destroys a window's pixel format with the window.
 */
struct HelperWindow {
  HWND window = nullptr;
  HDC deviceContext = nullptr;
  bool pixelFormatSet = false;
};

thread_local HelperWindow gHelper;

/** Registers the window class and creates the hidden window, idempotently. */
Result ensureHelperWindow() {
  if (gHelper.deviceContext != nullptr) return Result::Success;

  static const char* kClassName = "CoralGlHelperWindow";
  static bool classRegistered = false;

  HINSTANCE instance = GetModuleHandleA(nullptr);

  if (!classRegistered) {
    WNDCLASSA windowClass = {};
    windowClass.style = CS_OWNDC;
    windowClass.lpfnWndProc = DefWindowProcA;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kClassName;
    if (RegisterClassA(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      return Result::Unsupported;
    }
    classRegistered = true;
  }

  gHelper.window = CreateWindowExA(0, kClassName, "coral-gl-helper", WS_OVERLAPPEDWINDOW, 0, 0, 1, 1, nullptr, nullptr,
          instance, nullptr);
  if (gHelper.window == nullptr) return Result::Unsupported;

  gHelper.deviceContext = GetDC(gHelper.window);
  return gHelper.deviceContext != nullptr ? Result::Success : Result::Unsupported;
}

/** Applies a pixel format to @p deviceContext, succeeding if one is already set. */
Result applyPixelFormat(HDC deviceContext) {
  PIXELFORMATDESCRIPTOR descriptor = {};
  descriptor.nSize = sizeof(descriptor);
  descriptor.nVersion = 1;
  descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  descriptor.iPixelType = PFD_TYPE_RGBA;
  descriptor.cColorBits = 32;
  descriptor.cAlphaBits = 8;
  descriptor.cDepthBits = 24;
  descriptor.cStencilBits = 8;
  descriptor.iLayerType = PFD_MAIN_PLANE;

  const int format = ChoosePixelFormat(deviceContext, &descriptor);
  if (format == 0) {
    return Result::Unsupported;
  }

  // A window's pixel format can only be set once. If the application already chose one (typically
  // because it owns its own GL context), reuse it instead of failing: WGL context creation does not
  // require the format to be the one we would have chosen, and refusing here broke embedding into an
  // existing GL window. The only format we must not touch is one that was never set.
  const int current = GetPixelFormat(deviceContext);
  if (current != 0) {
    return Result::Success;
  }

  return SetPixelFormat(deviceContext, format, &descriptor) != FALSE ? Result::Success : Result::Unsupported;
}

/** Resolves the WGL extension entry points, which requires a current context. */
void loadWglExtensions() {
  auto load = [](const char* name) -> void* {
    void* symbol = reinterpret_cast<void*>(wglGetProcAddress(name));
    // wglGetProcAddress returns one of a few sentinel values rather than null when a function is
    // absent, and those sentinels must be treated as failure.
    if (symbol == reinterpret_cast<void*>(1) || symbol == reinterpret_cast<void*>(2) ||
            symbol == reinterpret_cast<void*>(3) || symbol == reinterpret_cast<void*>(-1)) {
      return nullptr;
    }
    if (symbol != nullptr) {
      return symbol;
    }

    // OpenGL 1.1 functions live in opengl32.dll, not in the driver's extension table.
    static HMODULE opengl32 = LoadLibraryA("opengl32.dll");
    return opengl32 != nullptr ? reinterpret_cast<void*>(GetProcAddress(opengl32, name)) : nullptr;
  };

  gWglExtensions.CreateContextAttribsARB =
          reinterpret_cast<decltype(gWglExtensions.CreateContextAttribsARB)>(load("wglCreateContextAttribsARB"));
  gWglExtensions.SwapIntervalEXT =
          reinterpret_cast<decltype(gWglExtensions.SwapIntervalEXT)>(load("wglSwapIntervalEXT"));
}

/** The loader the device uses for GL entry points. */
void* win32GlLoader(const char* name) {
  void* symbol = reinterpret_cast<void*>(wglGetProcAddress(name));
  if (symbol == reinterpret_cast<void*>(1) || symbol == reinterpret_cast<void*>(2) ||
          symbol == reinterpret_cast<void*>(3) || symbol == reinterpret_cast<void*>(-1)) {
    symbol = nullptr;
  }
  if (symbol != nullptr) {
    return symbol;
  }

  static HMODULE opengl32 = LoadLibraryA("opengl32.dll");
  return opengl32 != nullptr ? reinterpret_cast<void*>(GetProcAddress(opengl32, name)) : nullptr;
}

} // namespace anonymous

const char* GlPlatform::backendName() noexcept { return "WGL"; }

Result createPlatformContext(const SurfaceDescriptor& descriptor, const GlDialect dialect, const GlVersion minimum,
        const bool debugContext, GlPlatformContext* context) {
  // Windows has no OpenGL ES. The ES dialect is available only through ANGLE or a similar shim, which
  // the RHI does not depend on, so the request is refused with a clear reason rather than silently
  // producing a desktop context the caller did not ask for.
  if (dialect == GlDialect::ES) {
    return Result::Unsupported;
  }

  if (context == nullptr) {
    return Result::InvalidArgument;
  }

  // ---------------------------------------------------------------- adopt an existing context
  //
  // `SurfaceDescriptor` documents the Win32 pairing as `nativeWindow` = `HWND` and `nativeDisplay` = its `HDC`, and a
  // *window-attached* device is created from exactly that. Adoption is the other case: an application that already owns a
  // GL context passes it as `nativeDisplay`. The two are distinguishable, because an HDC answers `WindowFromDC` and an
  // HGLRC does not -- and without that test a caller who followed the documented pairing had its HDC adopted as a context,
  // so `wglMakeCurrent` failed on a window/context pair that was never one.
  const bool adoptsExistingContext = descriptor.nativeWindow != nullptr && descriptor.nativeDisplay != nullptr &&
          WindowFromDC(static_cast<HDC>(descriptor.nativeDisplay)) == nullptr;
  if (adoptsExistingContext) {
    // The embedding application already created a context and wants the RHI to render into it. Only
    // the handles are adopted; the application keeps ownership.
    context->display = descriptor.nativeDisplay;
    context->surface = descriptor.nativeDisplay;
    context->context = descriptor.nativeWindow;
    context->ownsDisplay = false;
    context->ownsSurface = false;
    context->ownsContext = false;
    context->dialect = GlDialect::Desktop;
    context->loader = &win32GlLoader;

    if (!wglMakeCurrent(static_cast<HDC>(context->display), static_cast<HGLRC>(context->context))) {
      return Result::Error;
    }
    loadWglExtensions();
    return Result::Success;
  }

  // ---------------------------------------------------------------- create our own context
  const Result helperResult = ensureHelperWindow();
  if (!isSuccess(helperResult)) return helperResult;

  HDC deviceContext = static_cast<HDC>(descriptor.nativeDisplay);
  HWND window = static_cast<HWND>(descriptor.nativeWindow);
  bool ownsDeviceContext = false;

  if (deviceContext == nullptr) {
    if (window != nullptr) {
      // A window was supplied without a device context; deriving one is the application's job, so
      // this is a descriptor the caller assembled wrongly.
      return Result::InvalidArgument;
    }
    // Offscreen: use the hidden helper window. Presentation becomes a no-op.
    deviceContext = gHelper.deviceContext;
  } else {
    ownsDeviceContext = false;
  }

  const Result formatResult = applyPixelFormat(deviceContext);
  if (!isSuccess(formatResult)) {
    return formatResult;
  }

  // Step 1: a throwaway context, needed only to resolve wglCreateContextAttribsARB.
  HGLRC probeContext = wglCreateContext(deviceContext);
  if (probeContext == nullptr) {
    return Result::Unsupported;
  }
  if (!wglMakeCurrent(deviceContext, probeContext)) {
    wglDeleteContext(probeContext);
    return Result::Error;
  }

  loadWglExtensions();
  const bool haveCreateContextAttribs = gWglExtensions.valid();

  wglMakeCurrent(nullptr, nullptr);
  wglDeleteContext(probeContext);

  if (!haveCreateContextAttribs) {
    // Without the extension there is no way to ask for a specific version, and the default context
    // Windows hands out is 1.1. The RHI needs 4.1, so this is a hard failure rather than a quiet
    // downgrade.
    return Result::Unsupported;
  }

  // Step 2: the real context.
  const int flags = debugContext ? WGL_CONTEXT_DEBUG_BIT_ARB : 0;
  const int attributes[] = { WGL_CONTEXT_MAJOR_VERSION_ARB, static_cast<int>(minimum.major),
    WGL_CONTEXT_MINOR_VERSION_ARB, static_cast<int>(minimum.minor), WGL_CONTEXT_PROFILE_MASK_ARB,
    // Core profile is what the shader compiler targets: no deprecated built-ins, no default
    // uniform block for most state, GLSL 4.10 without compatibility extensions.
    WGL_CONTEXT_CORE_PROFILE_BIT_ARB, WGL_CONTEXT_FLAGS_ARB, flags, 0 };

  HGLRC renderContext = gWglExtensions.CreateContextAttribsARB(deviceContext, nullptr, attributes);
  if (renderContext == nullptr) {
    // Try again without the debug flag: some drivers reject the debug bit outright rather than
    // ignoring it, and losing validation output is better than losing the device.
    const int fallbackAttributes[] = { WGL_CONTEXT_MAJOR_VERSION_ARB, static_cast<int>(minimum.major),
      WGL_CONTEXT_MINOR_VERSION_ARB, static_cast<int>(minimum.minor), WGL_CONTEXT_PROFILE_MASK_ARB,
      WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0 };
    renderContext = gWglExtensions.CreateContextAttribsARB(deviceContext, nullptr, fallbackAttributes);
    if (renderContext == nullptr) return Result::Unsupported;
  }

  // Sharing: `wglShareLists` merges the two contexts' object spaces. It has to run before the new context is used, and it
  // is WGL's only sharing mechanism -- EGL and GLX take the share context when the context is created. A refusal is a
  // warning rather than a failure: an unshared device still renders, and losing the group is recoverable.
  if (descriptor.glShareContext != nullptr) {
    // `wglShareLists` refuses to run while either context is current, and the root usually still is (its device made it
    // current when it was created). Unbinding here is what makes the share succeed; this only affects the calling thread,
    // which is why group members are meant to be created on one thread.
    (void) wglMakeCurrent(nullptr, nullptr);
    // A refusal is not fatal: the device still renders, it simply cannot see the group's textures. The result is deliberately
    // not reported from here because this file has no message sink (the EGL platform's `emitMessage` is a different
    // translation unit's). Surfacing it as a capability bit is the next step, alongside the facade's `ContextGroup`.
    (void) wglShareLists(static_cast<HGLRC>(descriptor.glShareContext), renderContext);
  }
  if (!wglMakeCurrent(deviceContext, renderContext)) {
    wglDeleteContext(renderContext);
    return Result::Error;
  }

  context->display = deviceContext;
  context->surface = deviceContext;
  context->context = renderContext;
  context->ownsDisplay = ownsDeviceContext;
  context->ownsSurface = ownsDeviceContext;
  context->ownsContext = true;
  context->dialect = GlDialect::Desktop;
  context->loader = &win32GlLoader;
  context->coreProfile = true;
  return Result::Success;
}

void destroyPlatformContext(GlPlatformContext* context) {
  if (context == nullptr) {
    return;
  }

  if (context->context != nullptr && context->ownsContext) {
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(static_cast<HGLRC>(context->context));
  }
  context->context = nullptr;
  context->surface = nullptr;
  context->display = nullptr;
  context->ownsContext = false;
  context->ownsSurface = false;
  context->ownsDisplay = false;
}

Result releasePlatformContextCurrent(const GlPlatformContext&) {
  // Per-thread by definition: on Windows this only unbinds the calling thread's context, which is exactly what a
  // `wglShareLists` needs and exactly what another thread cannot do on this thread's behalf.
  return wglMakeCurrent(nullptr, nullptr) != FALSE ? Result::Success : Result::Error;
}

Result makePlatformContextCurrent(const GlPlatformContext& context) {
  if (context.display == nullptr || context.context == nullptr) return Result::Unsupported;
  return wglMakeCurrent(static_cast<HDC>(context.display), static_cast<HGLRC>(context.context)) != FALSE
                 ? Result::Success
                 : Result::Error;
}

bool platformSupportsMailboxPresent() noexcept {
  // False on purpose: a swap interval of 0 stops waiting for vertical sync, it does not drop a frame that the compositor has
  // not consumed yet. Reporting Mailbox from here would be a promise this platform cannot keep.
  return false;
}

Result setPlatformSwapInterval(const GlPlatformContext&, const int interval) {
  // `wglSwapIntervalEXT` is a driver extension, not core WGL: a driver without it cannot express "do not wait for vsync", and
  // saying so is what lets the caller report Fifo instead of pretending it got Immediate.
  if (gWglExtensions.SwapIntervalEXT == nullptr) {
    return Result::Unsupported;
  }
  return gWglExtensions.SwapIntervalEXT(interval) != FALSE ? Result::Success : Result::Unsupported;
}

Result swapPlatformContextBuffers(const GlPlatformContext& context) {
  // An offscreen context has no drawable to present, which is a success rather than a failure: the
  // caller asked for rendering, not for presentation.
  if (context.surface == nullptr) {
    return Result::Success;
  }
  return SwapBuffers(static_cast<HDC>(context.surface)) != FALSE ? Result::Success : Result::Error;
}

Extent2D queryPlatformSurfaceSize(const GlPlatformContext& context, const SurfaceDescriptor& descriptor) {
  if (descriptor.width != 0 && descriptor.height != 0) {
    return { .width = descriptor.width, .height = descriptor.height };
  }

  if (descriptor.nativeWindow != nullptr) {
    RECT rect = {};
    if (GetClientRect(static_cast<HWND>(descriptor.nativeWindow), &rect) != FALSE) {
      const auto width = static_cast<uint32_t>(rect.right - rect.left);
      const auto height = static_cast<uint32_t>(rect.bottom - rect.top);
      if (width != 0 && height != 0) {
        return { .width = width, .height = height };
      }
    }
  }

  // A hidden helper window is 1x1, which is the correct answer for an offscreen context whose size
  // the caller is expected to pin.
  (void) context;
  return { .width = descriptor.width, .height = descriptor.height };
}

Result bindPlatformSurface(GlPlatformContext& context, const SurfaceDescriptor& descriptor) {
  (void) context;
  (void) descriptor;
  return Result::Unsupported;
}

} // namespace gl
} // namespace backend

#endif // CORAL_BUILD_FOR_WIN
