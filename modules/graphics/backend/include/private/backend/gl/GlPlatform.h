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

#ifndef CORAL_BACKEND_GL_GL_PLATFORM_H
#define CORAL_BACKEND_GL_GL_PLATFORM_H

/**
 * @file GlPlatform.h
 *
 * The window-system half of the OpenGL backend: creating a context and a drawable, and loading
 * OpenGL entry points.
 *
 * OpenGL has no single context-creation API. Each platform has its own, with its own way of asking
 * for a version and its own way of resolving functions:
 *
 * | Platform | Context creation | Entry point loader |
 * |---|---|---|
 * | Windows | WGL (`wglCreateContextAttribsARB`) | `wglGetProcAddress` |
 * | Linux | GLX (`glXCreateContextAttribsARB`) | `glXGetProcAddressARB` |
 * | macOS | CGL (`CGLChoosePixelFormat` + `kCGLPFAOpenGLProfile`) | `dlsym` on the framework |
 * | Android | EGL (`eglCreateContext`) | `eglGetProcAddress` |
 *
 * Hiding that behind one interface is what lets `GlContext` and everything above it be written once.
 * The platform layer's whole job is "give me a current context of at least this version"; it knows
 * nothing about RHI resources.
 *
 * ### Why the version request is a hard requirement
 *
 * The RHI targets OpenGL 4.1+ on desktop and OpenGL ES 3.0+ on mobile, and it does **not** fall back
 * to an older version. A silently downgraded context produces capability reports that describe a
 * different device than the renderer planned for, and the failures that follow (a missing
 * `glTexStorage2D`, a missing VAO) surface far from their cause. Asking for the version and failing
 * loudly if it cannot be had is the cheaper contract.
 */

#include <backend/Device.h>
#include <backend/Types.h>

#include <cstdint>
#include <string>
#include <vector>

namespace backend {
namespace gl {

/** The minimum context version each dialect requires. */
struct GlVersion {
  uint32_t major = 0;
  uint32_t minor = 0;

  /** The version the RHI requires for @p dialect: 4.1 desktop, 3.0 ES. */
  static GlVersion minimumFor(const GlDialect dialect) noexcept {
    return dialect == GlDialect::Desktop ? GlVersion{ .major = 4, .minor = 1 } : GlVersion{ .major = 3, .minor = 0 };
  }

  [[nodiscard]] bool atLeast(const GlVersion other) const noexcept {
    return major > other.major || (major == other.major && minor >= other.minor);
  }

  [[nodiscard]] std::string toString() const { return std::to_string(major) + "." + std::to_string(minor); }
};

/**
 * A platform context: display, drawable and GL context, plus the loader for its entry points.
 *
 * Ownership is explicit. A handle the platform layer created is destroyed by it; one supplied by the
 * embedding application is not, because the application may still be using it for its own rendering.
 */
/** Which window-system API produced a `GlPlatformContext`. */
enum class NativeApi : uint8_t {
  Unknown,
  Glx,
  Egl,
  Wgl,
  Cgl,
};

struct GlPlatformContext {
  /**
   * The API that created this context.
   *
   * On Linux both GLX and EGL are compiled into one library so that an X11 window and a Wayland
   * window can share the backend. This field is what lets the shared entry points below call the
   * right one; on every other platform it stays at the value that platform set.
   */
  NativeApi nativeApi = NativeApi::Unknown;

  /** Native display/device handle: an HDC, a Display*, a CGLContextObj's display, an EGLDisplay. */
  void* display = nullptr;
  /** Native drawable: an HDC, a GLXDrawable, a CGLContextObj's drawable, an EGLSurface. */
  void* surface = nullptr;
  /** The GL context: an HGLRC, a GLXContext, a CGLContextObj, an EGLContext. */
  void* context = nullptr;
  /** The platform pixel/config object, when the API needs it to recreate a drawable (EGLConfig). */
  void* config = nullptr;

  /** True when this layer created the handle and must destroy it. */
  bool ownsDisplay = false;
  bool ownsSurface = false;
  bool ownsContext = false;

  /** The dialect the context was actually created with. */
  GlDialect dialect = GlDialect::ES;

  /** The version the context actually reports, which may exceed the requested minimum. */
  GlVersion version = {};

  /** The GLSL version this context accepts, as the shader compiler's target. */
  uint32_t glslVersion = 300;

  /** True when the context is a core profile, which desktop 3.2+ normally is. */
  bool coreProfile = false;

  /**
   * The window-system loader to resolve GL entry points with: `wglGetProcAddress`,
   * `glXGetProcAddressARB` or `eglGetProcAddress`. Never null for a successfully created context.
   */
  void* (*loader)(const char*) = nullptr;
};

/**
 * The window-system implementation.
 *
 * One implementation is compiled per platform; the others are excluded by the preprocessor. Every
 * method returns a `Result`, so an unsupported or unavailable platform surfaces as
 * `Result::Unsupported` rather than as a link error.
 */
class GlPlatform {
  public:
  GlPlatform();
  ~GlPlatform();

  GlPlatform(const GlPlatform&) = delete;
  GlPlatform& operator=(const GlPlatform&) = delete;

  /**
   * Brings up a context of at least @p minimum version in dialect @p dialect.
   *
   * When @p descriptor carries a ready-made native context (for example an `HDC` and `HGLRC` an
   * embedding application already created), that context is adopted instead of creating one, which is
   * what an application that already owns GL state needs.
   *
   * @param descriptor the surface description from `DeviceConfiguration`.
   * @param dialect    the dialect to request.
   * @param minimum    the minimum acceptable version.
   * @param debugContext request a debug/validation context where the platform supports it.
   */
  Result initialize(const SurfaceDescriptor& descriptor, GlDialect dialect, GlVersion minimum, bool debugContext);

  /** Releases everything this object created. Adopted handles are left alone. */
  void shutdown();

  /** Makes the context current on the calling thread. */
  Result makeCurrent();

  /**
   * Replaces the context's drawable with @p descriptor's native window, keeping the same GL
   * context and all its resources. This is how an output node attached to an existing device
   * changes its SurfaceView/window without recreating the device.
   */
  Result bindSurface(const SurfaceDescriptor& descriptor);

  /**
   * Releases the calling thread's current context.
   *
   * Needed before `wglShareLists`, which refuses to run while either context is current -- and needed generally by a caller
   * that wants to hand a thread over to another device.
   */
  Result releaseCurrent();

  /** Presents the current frame. A no-op for an offscreen or headless context. */
  Result swapBuffers();
  /**
   * Sets the swap interval for the current context: 1 waits for vertical sync, 0 does not.
   *
   * @return Result::Unsupported when the driver has no swap-interval control. That is not fatal and must not be hidden: the
   *         caller reports the mode it actually got, because a request for Immediate that silently becomes Fifo is the kind
   *         of "it works on my machine" this API exists to prevent.
   */
  Result setSwapInterval(int interval);
  /**
   * @return true when this platform can grant `PresentMode::Mailbox` rather than approximating it.
   *
   * False for every OpenGL platform today: WGL, GLX, CGL and EGL all expose only a swap interval, which chooses between waiting
   * for vertical sync and not waiting -- neither of which drops a superseded frame, which is what Mailbox means. The hook exists
   * so that a future platform (or a Vulkan/Metal backend) can answer truthfully instead of the device guessing.
   */
  [[nodiscard]] bool supportsMailboxPresent() const noexcept;

  /** @return the drawable size, querying the window system when the descriptor did not pin one. */
  [[nodiscard]] Extent2D querySurfaceSize() const;

  /** @return the platform context handles and dialect. */
  [[nodiscard]] const GlPlatformContext& context() const noexcept { return mContext; }

  /** @return true when the context has no drawable, so nothing can be presented. */
  [[nodiscard]] bool isHeadless() const noexcept { return mContext.surface == nullptr; }

  /** Marks the context as lost, so subsequent operations report `Result::DeviceLost`. */
  void markLost() noexcept { mLost = true; }
  [[nodiscard]] bool isLost() const noexcept { return mLost; }

  /** Queries the driver strings and version. Called by the device after a successful initialize. */
  void queryContextInfo(std::string* vendor, std::string* renderer, std::string* versionString,
          std::string* shadingLanguageVersion, std::vector<std::string>* extensions) const;

  /** @return true when @p extension appears in the context's extension list. */
  [[nodiscard]] bool hasExtension(const char* extension) const noexcept;

  /** Appends every extension, one per line, for the diagnostics report. */
  void appendExtensions(std::string& out) const;

  /** @return the number of extensions reported by the driver. */
  [[nodiscard]] size_t extensionCount() const noexcept { return mExtensions.size(); }

  [[nodiscard]] const std::string& vendor() const noexcept { return mVendor; }
  [[nodiscard]] const std::string& renderer() const noexcept { return mRenderer; }
  [[nodiscard]] const std::string& versionString() const noexcept { return mVersionString; }
  [[nodiscard]] const std::string& shadingLanguageVersion() const noexcept { return mShadingLanguageVersion; }
  [[nodiscard]] const std::string& apiVersion() const noexcept { return mApiVersion; }

  /** @return the surface description this context was created for. */
  [[nodiscard]] const SurfaceDescriptor& descriptor() const noexcept { return mDescriptor; }

  /** @return a human readable name of the window-system backend in use, e.g. "WGL". */
  [[nodiscard]] static const char* backendName() noexcept;

  private:
  SurfaceDescriptor mDescriptor = {};
  GlPlatformContext mContext = {};
  bool mLost = false;

  std::string mVendor;
  std::string mRenderer;
  std::string mVersionString;
  std::string mShadingLanguageVersion;
  std::string mApiVersion;
  std::vector<std::string> mExtensions;
};

// ------------------------------------------------------------------------------------------------
// Platform entry points
//
// Implemented once per platform in src/gl/platform/. Declared here so GlPlatform can be written the
// same way everywhere and a missing implementation is a link error on exactly one platform.
// ------------------------------------------------------------------------------------------------

/**
 * Creates a context in @p context, filling in the handles, the actual version and the loader.
 *
 * @return Result::Success, Result::Unsupported when the platform cannot provide the request, or a
 *         driver error.
 */
Result createPlatformContext(const SurfaceDescriptor& descriptor, GlDialect dialect, GlVersion minimum,
        bool debugContext, GlPlatformContext* context);

/** Destroys the handles @p context says it owns. */
void destroyPlatformContext(GlPlatformContext* context);

/** Makes @p context current on the calling thread. */
Result makePlatformContextCurrent(const GlPlatformContext& context);

/** Replaces the context's drawable with @p descriptor's window, keeping the context. */
Result bindPlatformSurface(GlPlatformContext& context, const SurfaceDescriptor& descriptor);

/**
 * Releases whatever context is current on the calling thread for this platform.
 *
 * Per-thread by construction: it cannot release a context that is current on *another* thread, which is why a share request
 * has to be made from the thread that owns the root. Safe when nothing is current.
 */
Result releasePlatformContextCurrent(const GlPlatformContext& context);

/** Presents @p context's drawable. */
Result swapPlatformContextBuffers(const GlPlatformContext& context);
/** Sets @p context's swap interval; see `GlPlatform::setSwapInterval` for the contract. */
Result setPlatformSwapInterval(const GlPlatformContext& context, int interval);

/** @return true when this platform's presentation can grant Mailbox rather than approximate it. */
bool platformSupportsMailboxPresent() noexcept;

/** @return the drawable size, or {0, 0} when it cannot be queried. */
Extent2D queryPlatformSurfaceSize(const GlPlatformContext& context, const SurfaceDescriptor& descriptor);

} // namespace gl
} // namespace backend

#endif // CORAL_BACKEND_GL_GL_PLATFORM_H
