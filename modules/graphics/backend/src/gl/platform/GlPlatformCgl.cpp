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
 * @file GlPlatformCgl.cpp
 *
 * macOS context creation through CGL.
 *
 * macOS is the one platform where the caller's window is *not* the drawable. CGL draws into a
 * `CGLContextObj`, and attaching it to an `NSView` is the view layer's job (an `NSOpenGLContext`
 * wraps that). This file therefore creates an offscreen-capable CGL context and leaves attachment to
 * the embedding application, which is exactly what `SurfaceDescriptor::ownedSurface` signals.
 *
 * Two macOS specifics are load-bearing:
 *
 *  1. **The profile must be requested explicitly.** Without
 *     `kCGLPFAOpenGLProfile = kCGLOGLPVersion_GL4_Core`, macOS hands back a 2.1 context, silently and
 *     regardless of the version attributes -- CGL has no version attributes at all. That is why the
 *     minimum version is validated *after* the context is current rather than being passed in.
 *  2. **OpenGL is deprecated but still functional.** Apple stopped developing it after 4.1, which is
 *     exactly why the RHI's desktop target is 4.1: it is the highest version macOS will ever provide,
 *     so a renderer that works here works everywhere the desktop dialect is offered.
 *
 * The GL entry points come from `dlsym` on the OpenGL framework: macOS exports every function it
 * supports, so there is no `wglGetProcAddress`-style split between version levels.
 */

#include "private/backend/gl/GlPlatform.h"

#if defined(CORAL_BUILD_FOR_MAC) && !defined(CORAL_BUILD_FOR_IOS)

#include <dlfcn.h>

#include <OpenGL/CGLTypes.h>
#include <OpenGL/OpenGL.h>

#include <cstring>

namespace backend {
namespace gl {
namespace {

/** The loader the device uses for GL entry points. */
void* cglGlLoader(const char* name) {
  // dlopen of the framework is idempotent and cheap; the handle is cached because the backend asks
  // for well over a hundred symbols.
  static void* framework = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY | RTLD_LOCAL);
  if (framework == nullptr)
    framework = dlopen("/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL", RTLD_LAZY | RTLD_LOCAL);
  return framework != nullptr ? dlsym(framework, name) : nullptr;
}

// There is no `queryCglVersion` here, and there cannot be one. CGL has no parameter that reports the context's
// version: `CGLContextParameter`'s last member is `kCGLCPMPSwapsInFlight`, and this file used to name
// `kCGLCPMajorVersion` / `kCGLCPMinorVersion`, which exist in no Apple header -- so it had never compiled on macOS.
//
// The version does not need to come from CGL anyway. `GlPlatform` asks the driver directly with
// `glGetIntegerv(GL_MAJOR_VERSION / GL_MINOR_VERSION)` and says in as many words that it prefers that to what the
// platform layer guessed, naming CGL as a platform whose guess is unreliable. So `context->version` set below is a
// starting point that is corrected a moment later, and leaving it at its default is both honest and sufficient.

} // namespace anonymous

const char* GlPlatform::backendName() noexcept { return "CGL"; }

Result createPlatformContext(const SurfaceDescriptor& descriptor, GlDialect dialect, GlVersion minimum,
        bool debugContext, GlPlatformContext* context) {
  // macOS has no OpenGL ES driver. iOS is the ES and Metal platform; macOS is the desktop one.
  if (dialect == GlDialect::ES) {
    return Result::Unsupported;
  }
  if (context == nullptr) return Result::InvalidArgument;

  // ---------------------------------------------------------------- adopt an existing context
  if (descriptor.cglContext != nullptr) {
    context->display = descriptor.cglContext;
    context->surface = descriptor.cglContext;
    context->context = descriptor.cglContext;
    context->ownsDisplay = false;
    context->ownsSurface = false;
    context->ownsContext = false;
    context->dialect = GlDialect::Desktop;
    context->loader = &cglGlLoader;

    if (CGLSetCurrentContext(static_cast<CGLContextObj>(context->context)) != kCGLNoError) {
      return Result::Error;
    }
    context->coreProfile = true;
    return Result::Success;
  }

  // ---------------------------------------------------------------- create our own context
  // A core-profile pixel format is requested because the shader compiler emits GLSL 4.10 core: no
  // deprecated built-ins and no compatibility extensions. `kCGLPFAAccelerated` is deliberately not
  // required, so that a software renderer still works on a machine without a usable GPU.
  CGLPixelFormatAttribute attributes[] = { kCGLPFAOpenGLProfile,
    static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_GL4_Core), kCGLPFAColorSize,
    static_cast<CGLPixelFormatAttribute>(24), kCGLPFAAlphaSize, static_cast<CGLPixelFormatAttribute>(8),
    kCGLPFADepthSize, static_cast<CGLPixelFormatAttribute>(24), kCGLPFAStencilSize,
    static_cast<CGLPixelFormatAttribute>(8), kCGLPFADoubleBuffer, static_cast<CGLPixelFormatAttribute>(0) };

  CGLPixelFormatObj pixelFormat = nullptr;
  GLint pixelFormatCount = 0;
  if (CGLChoosePixelFormat(attributes, &pixelFormat, &pixelFormatCount) != kCGLNoError || pixelFormat == nullptr) {
    // Retry without a depth buffer: some virtualised GPUs advertise the profile but not the depth
    // sizes, and a context without a depth buffer is still usable for offscreen work.
    CGLPixelFormatAttribute minimal[] = { kCGLPFAOpenGLProfile,
      static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_GL4_Core), static_cast<CGLPixelFormatAttribute>(0) };
    if (CGLChoosePixelFormat(minimal, &pixelFormat, &pixelFormatCount) != kCGLNoError || pixelFormat == nullptr) {
      return Result::Unsupported;
    }
  }

  CGLContextObj renderContext = nullptr;
  if (CGLCreateContext(pixelFormat, nullptr, &renderContext) != kCGLNoError || renderContext == nullptr) {
    CGLDestroyPixelFormat(pixelFormat);
    return Result::Unsupported;
  }
  CGLDestroyPixelFormat(pixelFormat);

  if (CGLSetCurrentContext(renderContext) != kCGLNoError) {
    CGLDestroyContext(renderContext);
    return Result::Error;
  }

  context->display = renderContext;
  context->surface = renderContext;
  context->context = renderContext;
  context->ownsDisplay = true;
  context->ownsSurface = true;
  context->ownsContext = true;
  context->dialect = GlDialect::Desktop;
  context->loader = &cglGlLoader;
  context->coreProfile = true;

  // A debug context is a driver feature on macOS with no CGL attribute; the backend reports debug
  // support as unavailable rather than pretending it was requested successfully.
  (void) debugContext;
  (void) descriptor;
  return Result::Success;
}

void destroyPlatformContext(GlPlatformContext* context) {
  if (context == nullptr) return;

  if (context->context != nullptr && context->ownsContext) {
    CGLSetCurrentContext(nullptr);
    CGLDestroyContext(static_cast<CGLContextObj>(context->context));
  }
  context->context = nullptr;
  context->surface = nullptr;
  context->display = nullptr;
  context->ownsContext = false;
  context->ownsSurface = false;
  context->ownsDisplay = false;
}

Result makePlatformContextCurrent(const GlPlatformContext& context) {
  if (context.context == nullptr) return Result::Unsupported;
  return CGLSetCurrentContext(static_cast<CGLContextObj>(context.context)) == kCGLNoError ? Result::Success
                                                                                          : Result::Error;
}

bool platformSupportsMailboxPresent() noexcept {
  // False on purpose: a swap interval of 0 stops waiting for vertical sync, it does not drop a frame that the compositor has
  // not consumed yet. Reporting Mailbox from here would be a promise this platform cannot keep.
  return false;
}

Result setPlatformSwapInterval(const GlPlatformContext& context, const int interval) {
  if (context.context == nullptr) return Result::Unsupported;
  const GLint value = interval;
  return CGLSetParameter(static_cast<CGLContextObj>(context.context), kCGLCPSwapInterval, &value) == kCGLNoError
    ? Result::Success
    : Result::Unsupported;
}

Result swapPlatformContextBuffers(const GlPlatformContext& context) {
  if (context.context == nullptr) return Result::Unsupported;

  // A CGL context only has a drawable once the view layer has attached one. When it has not, there is
  // nothing to present, which is the offscreen case: a success, not a failure.
  CGLContextObj cglContext = static_cast<CGLContextObj>(context.context);
  if (CGLGetPixelFormat(cglContext) == nullptr) return Result::Success;

  const CGLError error = CGLFlushDrawable(cglContext);
  // kCGLBadDrawable means no drawable is attached yet, which is the offscreen case again.
  if (error == kCGLBadDrawable) return Result::Success;
  return error == kCGLNoError ? Result::Success : Result::Error;
}

Extent2D queryPlatformSurfaceSize(const GlPlatformContext& context, const SurfaceDescriptor& descriptor) {
  // CGL cannot report a drawable size: the drawable belongs to the view layer. The caller pins the
  // size in the descriptor, which is why surface sizing is a descriptor field rather than something
  // the backend probes on this platform.
  (void) context;
  return Extent2D{ descriptor.width, descriptor.height };
}

Result bindPlatformSurface(GlPlatformContext& context, const SurfaceDescriptor& descriptor) {
  (void) context;
  (void) descriptor;
  return Result::Unsupported;
}

} // namespace gl
} // namespace backend

#endif // CORAL_BUILD_FOR_MAC && !CORAL_BUILD_FOR_IOS
