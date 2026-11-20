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
 * @file GlPlatformLinux.cpp
 *
 * The Linux window-system dispatcher.
 *
 * Linux is the one platform with two native window systems in current use. X11 keeps the GLX path
 * (the historical implementation, and the one an XWayland session also reaches); a native Wayland
 * surface is created through EGL. Both implementations are compiled into this library and this file
 * picks between them from `SurfaceDescriptor::nativeWindowSystem`.
 *
 * An offscreen device tries EGL's surfaceless platform first. That is what lets the backend run on a
 * machine with no X server and no `/dev/dri` -- Mesa's llvmpipe serves a surfaceless context -- and
 * GLX remains the fallback for a driver that only implements the X11 path.
 *
 * This is also the shape the Vulkan backend will need: its surface creation has to choose between
 * `VK_KHR_xlib_surface` and `VK_KHR_wayland_surface` from the same field, rather than assuming Xlib
 * because the platform is Linux.
 */

#include "private/backend/gl/GlPlatform.h"

#if defined(CORAL_BUILD_FOR_LINUX)

namespace backend {
namespace gl {

// Each implementation is compiled in its own translation unit under these names. The declarations
// are kept here rather than in a header because only this file is allowed to call them: everything
// above the platform layer sees the platform-neutral entry points the header declares.
Result createGlxPlatformContext(const SurfaceDescriptor& descriptor, GlDialect dialect, GlVersion minimum,
        bool debugContext, GlPlatformContext* context);
void destroyGlxPlatformContext(GlPlatformContext* context);
Result makeGlxPlatformContextCurrent(const GlPlatformContext& context);
Result releaseGlxPlatformContextCurrent(const GlPlatformContext& context);
Result swapGlxPlatformContextBuffers(const GlPlatformContext& context);
Result setGlxPlatformSwapInterval(const GlPlatformContext& context, int interval);
bool platformGlxSupportsMailboxPresent() noexcept;
Extent2D queryGlxPlatformSurfaceSize(const GlPlatformContext& context, const SurfaceDescriptor& descriptor);

Result createEglPlatformContext(const SurfaceDescriptor& descriptor, GlDialect dialect, GlVersion minimum,
        bool debugContext, GlPlatformContext* context);
void destroyEglPlatformContext(GlPlatformContext* context);
Result makeEglPlatformContextCurrent(const GlPlatformContext& context);
Result releaseEglPlatformContextCurrent(const GlPlatformContext& context);
Result swapEglPlatformContextBuffers(const GlPlatformContext& context);
Result setEglPlatformSwapInterval(const GlPlatformContext& context, int interval);
bool platformEglSupportsMailboxPresent() noexcept;
Extent2D queryEglPlatformSurfaceSize(const GlPlatformContext& context, const SurfaceDescriptor& descriptor);

namespace {

bool wantsEgl(const SurfaceDescriptor& descriptor) {
  return descriptor.nativeWindowSystem == NativeWindowSystem::Wayland ||
         descriptor.eglDisplay != nullptr || descriptor.eglSurface != nullptr ||
         descriptor.eglContext != nullptr;
}

} // namespace anonymous

const char* GlPlatform::backendName() noexcept {
  return "GLX/EGL";
}

Result createPlatformContext(const SurfaceDescriptor& descriptor, const GlDialect dialect, const GlVersion minimum,
        const bool debugContext, GlPlatformContext* context) {
  if (wantsEgl(descriptor)) {
    return createEglPlatformContext(descriptor, dialect, minimum, debugContext, context);
  }

  // A window with an explicit X11 (or unspecified) window system is GLX first. An unspecified kind
  // is historical behaviour: Linux windows were X11 before Wayland existed in this backend. EGL is
  // the fallback, which is what lets an X11 session whose driver only exposes EGL still run.
  if (descriptor.nativeWindow != nullptr) {
    if (const Result glxResult = createGlxPlatformContext(descriptor, dialect, minimum, debugContext, context);
        glxResult == Result::Success) {
      return glxResult;
    }
    return createEglPlatformContext(descriptor, dialect, minimum, debugContext, context);
  }

  // Offscreen. EGL surfaceless is the path that works without an X server and without a DRM node;
  // GLX is kept as the fallback for drivers that only expose their GL through X11.
  if (const Result eglResult = createEglPlatformContext(descriptor, dialect, minimum, debugContext, context);
      eglResult == Result::Success) {
    return eglResult;
  }
  return createGlxPlatformContext(descriptor, dialect, minimum, debugContext, context);
}

void destroyPlatformContext(GlPlatformContext* context) {
  if (context == nullptr) return;
  if (context->nativeApi == NativeApi::Egl) {
    destroyEglPlatformContext(context);
  } else {
    destroyGlxPlatformContext(context);
  }
}

Result makePlatformContextCurrent(const GlPlatformContext& context) {
  return context.nativeApi == NativeApi::Egl ? makeEglPlatformContextCurrent(context)
                                             : makeGlxPlatformContextCurrent(context);
}

Result releasePlatformContextCurrent(const GlPlatformContext& context) {
  return context.nativeApi == NativeApi::Egl ? releaseEglPlatformContextCurrent(context)
                                             : releaseGlxPlatformContextCurrent(context);
}

Result swapPlatformContextBuffers(const GlPlatformContext& context) {
  return context.nativeApi == NativeApi::Egl ? swapEglPlatformContextBuffers(context)
                                             : swapGlxPlatformContextBuffers(context);
}

Result setPlatformSwapInterval(const GlPlatformContext& context, const int interval) {
  return context.nativeApi == NativeApi::Egl ? setEglPlatformSwapInterval(context, interval)
                                             : setGlxPlatformSwapInterval(context, interval);
}

bool platformSupportsMailboxPresent() noexcept {
  // GLX and EGL both only stop waiting for vsync when the interval is 0; neither drops a frame the
  // compositor has not consumed. Reporting Mailbox would be a promise neither keeps.
  return false;
}

Extent2D queryPlatformSurfaceSize(const GlPlatformContext& context, const SurfaceDescriptor& descriptor) {
  return context.nativeApi == NativeApi::Egl ? queryEglPlatformSurfaceSize(context, descriptor)
                                             : queryGlxPlatformSurfaceSize(context, descriptor);
}

} // namespace gl
} // namespace backend

#endif // CORAL_BUILD_FOR_LINUX
