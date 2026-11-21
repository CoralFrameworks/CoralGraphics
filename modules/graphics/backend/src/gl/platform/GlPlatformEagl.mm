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
 * @file GlPlatformEagl.mm
 *
 * iOS context creation through EAGL.
 *
 * EAGL is not EGL. It has no display, no config and no surface object: an `EAGLContext` is the context, and
 * the drawable is a `CAEAGLLayer` the view layer owns. The backend therefore creates a context and leaves
 * presentation to the embedding application, which is the same arrangement the macOS CGL layer uses. An
 * offscreen device -- the one the test suites and the effect renderer use -- needs nothing else: rendering
 * goes into the backend's own framebuffer objects rather than the default one.
 *
 * The entry points are exported by the OpenGLES framework, so `dlsym` on it is the loader, as on macOS.
 */

#include "private/backend/gl/GlPlatform.h"

#if defined(CORAL_BUILD_FOR_IOS)

#import <Foundation/Foundation.h>
#import <OpenGLES/EAGL.h>

#include <dlfcn.h>

namespace backend {
namespace gl {
namespace {

/** The loader the device uses for GL entry points. */
void* eaglGlLoader(const char* name) {
  static void* framework = dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_LAZY | RTLD_LOCAL);
  if (framework == nullptr) {
    framework = dlopen("/System/Library/Frameworks/OpenGLES.framework/Versions/A/OpenGLES", RTLD_LAZY | RTLD_LOCAL);
  }
  void* symbol = framework != nullptr ? dlsym(framework, name) : nullptr;
  // The framework is linked, so its symbols are also in the process image. The fallback is what makes a
  // simulator runtime whose framework path differs from the device still resolve every entry point.
  if (symbol == nullptr) symbol = dlsym(RTLD_DEFAULT, name);
  return symbol;
}

/** Retains @p context as the opaque handle `GlPlatformContext` carries. */
void* retainContext(EAGLContext* context) {
#if __has_feature(objc_arc)
  return (void*) CFBridgingRetain(context);
#else
  return (void*) [context retain];
#endif
}

/** Releases a handle that came from `retainContext`. */
void releaseContext(void* handle) {
  if (handle == nullptr) return;
  EAGLContext* context = (__bridge EAGLContext*) handle;
#if __has_feature(objc_arc)
  CFBridgingRelease(handle);
#else
  [context release];
#endif
}

} // namespace anonymous

const char* GlPlatform::backendName() noexcept { return "EAGL"; }

Result createPlatformContext(const SurfaceDescriptor& descriptor, GlDialect dialect, GlVersion minimum,
        bool debugContext, GlPlatformContext* context) {
  if (context == nullptr) return Result::InvalidArgument;
  // iOS has no desktop OpenGL driver. The desktop dialect is a programming error here rather than a fallback.
  if (dialect != GlDialect::ES) return Result::Unsupported;

  // ES 3 is this backend's floor. The API constant asks for a 3.0 context; asking for 3.2 instead would
  // exclude the devices the RHI's ES 3.0 dialect is meant to cover, and the version is read back from the
  // driver afterwards anyway (`GlPlatform::initialize`).
  EAGLContext* created = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];
  if (created == nil) {
    return Result::Unsupported;
  }
  if (![EAGLContext setCurrentContext:created]) {
#if !__has_feature(objc_arc)
    [created release];
#endif
    return Result::Error;
  }

  context->nativeApi = NativeApi::Eagl;
  // EAGL has no display or surface object. Presenting is the view layer's job, so this platform reports
  // itself as headless and `swapBuffers` is a no-op rather than a fabricated success on a drawable.
  context->display = nullptr;
  context->surface = nullptr;
  context->context = retainContext(created);
#if !__has_feature(objc_arc)
  // `retainContext` took the reference the handle needs; the allocator's reference is this function's.
  [created release];
#endif
  context->config = nullptr;
  context->ownsDisplay = false;
  context->ownsSurface = false;
  context->ownsContext = true;
  context->dialect = GlDialect::ES;
  context->version = minimum;
  context->glslVersion = 300;
  context->coreProfile = false;
  context->loader = &eaglGlLoader;

  (void) descriptor;
  (void) debugContext;
  return Result::Success;
}

void destroyPlatformContext(GlPlatformContext* context) {
  if (context == nullptr) return;

  if (context->context != nullptr && context->ownsContext) {
    EAGLContext* created = (__bridge EAGLContext*) context->context;
    if ([EAGLContext currentContext] == created) {
      [EAGLContext setCurrentContext:nil];
    }
    releaseContext(context->context);
  }
  context->context = nullptr;
  context->surface = nullptr;
  context->display = nullptr;
  context->config = nullptr;
  context->ownsContext = false;
  context->ownsSurface = false;
  context->ownsDisplay = false;
}

Result makePlatformContextCurrent(const GlPlatformContext& context) {
  if (context.context == nullptr) return Result::Unsupported;
  return [EAGLContext setCurrentContext:(__bridge EAGLContext*) context.context] ? Result::Success : Result::Error;
}

Result releasePlatformContextCurrent(const GlPlatformContext& context) {
  (void) context;
  // Per-thread by construction: `setCurrentContext:nil` clears only the calling thread's context.
  return [EAGLContext setCurrentContext:nil] ? Result::Success : Result::Error;
}

bool platformSupportsMailboxPresent() noexcept {
  // EAGL presents through Core Animation, which offers no swap-interval or mailbox control to this layer.
  return false;
}

Result setPlatformSwapInterval(const GlPlatformContext& context, const int interval) {
  (void) context;
  (void) interval;
  // The swap interval belongs to the `CAEAGLLayer`, which the application owns.
  return Result::Unsupported;
}

Result swapPlatformContextBuffers(const GlPlatformContext& context) {
  if (context.context == nullptr) return Result::Unsupported;
  // An offscreen context has no drawable to present. A view-backed one is flushed by its owner.
  return Result::Success;
}

Result bindPlatformSurface(GlPlatformContext& context, const SurfaceDescriptor& descriptor) {
  (void) context;
  (void) descriptor;
  // Rebinding an EAGL drawable means reconstructing the layer's renderbuffers, which belongs to the view
  // layer; the backend keeps the context and all its resources.
  return Result::Unsupported;
}

Extent2D queryPlatformSurfaceSize(const GlPlatformContext& context, const SurfaceDescriptor& descriptor) {
  (void) context;
  return Extent2D{ descriptor.width, descriptor.height };
}

} // namespace gl
} // namespace backend

#endif // CORAL_BUILD_FOR_IOS
