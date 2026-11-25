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
 * @file GlEntryPoints.cpp
 *
 * Runtime resolution of the OpenGL and EGL entry points.
 *
 * The loader is deliberately dialect-aware. Requiring *every* entry point in the shared table would
 * fail on both dialects, because each is missing functions the other has: `glClearDepthf` is ES-only,
 * `glClearDepth` is desktop-only, `glMemoryBarrierByRegion` is ES 3.1+, and `glPolygonMode` does not
 * exist in ES at all. `GLApi::load` therefore checks a per-dialect required set and leaves the rest
 * optional, which keeps "device creation fails loudly" while still allowing one shared table.
 */

#include "private/backend/gl/GlEntryPoints.h"

#include <cstddef>
#include <cstring> // std::strcmp

#if defined(CORAL_BUILD_FOR_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

backend::gl::GLApi gGLApi;
EGLApi gEGLApi;

/** True once the platform library handles have been opened. */
bool gPlatformLibrariesReady = false;

/** The `eglGetProcAddress` the windowing layer supplied, or null. */
void* gEglGetProcAddress = nullptr;

#if defined(CORAL_BUILD_FOR_WIN)
HMODULE gEGL = nullptr;
HMODULE gGLES = nullptr;
HMODULE gOpenGL = nullptr;

void openPlatformLibraries() {
  if (gPlatformLibrariesReady) return;
  gPlatformLibrariesReady = true;
  gEGL = LoadLibraryA("libEGL.dll");
  gGLES = LoadLibraryA("libGLESv2.dll");
  // The desktop dialect's GL 1.1 functions live in opengl32.dll rather than in the driver's
  // extension table, and the window-system loaders hand out only the driver's half.
  gOpenGL = LoadLibraryA("opengl32.dll");
  if (gGLES == nullptr) gGLES = gEGL;
}

void* defaultLoader(const char* name) {
  openPlatformLibraries();
  void* symbol = nullptr;
  if (gEGL != nullptr) symbol = reinterpret_cast<void*>(GetProcAddress(gEGL, name));
  if (symbol == nullptr && gGLES != nullptr) symbol = reinterpret_cast<void*>(GetProcAddress(gGLES, name));
  if (symbol == nullptr && gOpenGL != nullptr) symbol = reinterpret_cast<void*>(GetProcAddress(gOpenGL, name));
  return symbol;
}
#else
void* gEGL = nullptr;
void* gGLES = nullptr;
void* gOpenGL = nullptr;

void openPlatformLibraries() {
  if (gPlatformLibrariesReady) return;
  gPlatformLibrariesReady = true;
  gEGL = dlopen("libEGL.so", RTLD_LAZY | RTLD_LOCAL);
  if (gEGL == nullptr) gEGL = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
  gGLES = dlopen("libGLESv2.so", RTLD_LAZY | RTLD_LOCAL);
  if (gGLES == nullptr) gGLES = dlopen("libGLESv2.so.2", RTLD_LAZY | RTLD_LOCAL);
  gOpenGL = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
  if (gGLES == nullptr) gGLES = gEGL;
}

void* defaultLoader(const char* name) {
  openPlatformLibraries();
  void* symbol = nullptr;
  if (gEGL != nullptr) symbol = dlsym(gEGL, name);
  if (symbol == nullptr && gGLES != nullptr) symbol = dlsym(gGLES, name);
  if (symbol == nullptr && gOpenGL != nullptr) symbol = dlsym(gOpenGL, name);
  return symbol;
}
#endif

/** Resolves @p name through @p resolve, falling back to the platform library. */
void* resolveEntryPoint(const char* name, void* (*resolve)(const char*)) {
  if (resolve != nullptr) {
    if (void* symbol = resolve(name)) return symbol;
  }
  return defaultLoader(name);
}

/** @return true when @p name appears in the null-terminated array @p list. */
bool inList(const char* const* list, const size_t count, const char* name) {
  for (size_t index = 0; index < count; ++index) {
    if (std::strcmp(list[index], name) == 0) return true;
  }
  return false;
}

/**
 * True when @p name is required for @p dialect.
 *
 * The lists below are the functions the backend calls unconditionally on that dialect. Everything
 * else in the shared table is optional: its absence means a feature is unavailable, which the
 * capability probe reports, rather than a reason to refuse the device.
 */
bool isRequired(const backend::GlDialect dialect, const char* name) {
  static constexpr const char* kCommon[] = {
    // clang-format off
    "glActiveTexture",
    "glAttachShader",
    "glBindAttribLocation",
    "glBindBuffer",
    "glBindBufferBase",
    "glBindFramebuffer",
    "glBindRenderbuffer",
    "glBindTexture",
    "glBindVertexArray",
    "glBlendColor",
    "glBlendEquationSeparate",
    "glBlendFuncSeparate",
    "glBlitFramebuffer",
    "glBufferData",
    "glBufferSubData",
    "glCheckFramebufferStatus",
    "glClear",
    "glClearBufferfv",
    "glClearBufferfi",
    "glClearColor",
    "glClearStencil",
    "glColorMask",
    "glCompileShader",
    "glCreateProgram",
    "glCreateShader",
    "glCullFace",
    "glDeleteBuffers",
    "glDeleteFramebuffers",
    "glDeleteProgram",
    "glDeleteRenderbuffers",
    "glDeleteShader",
    "glDeleteTextures",
    "glDeleteVertexArrays",
    "glDepthFunc",
    "glDepthMask",
    "glDetachShader",
    "glDisable",
    "glDisableVertexAttribArray",
    "glDrawArrays",
    "glDrawArraysInstanced",
    "glDrawElements",
    "glDrawElementsInstanced",
    "glEnable",
    "glEnableVertexAttribArray",
    "glFinish",
    "glFlush",
    "glFramebufferRenderbuffer",
    "glFramebufferTexture2D",
    "glFrontFace",
    "glGenBuffers",
    "glGenFramebuffers",
    "glGenRenderbuffers",
    "glGenTextures",
    "glGenVertexArrays",
    "glGetAttribLocation",
    "glGetError",
    "glGetIntegerv",
    "glGetProgramInfoLog",
    "glGetProgramiv",
    "glGetShaderInfoLog",
    "glGetShaderiv",
    "glGetString",
    "glGetUniformLocation",
    "glLinkProgram",
    "glPixelStorei",
    "glPolygonOffset",
    "glReadPixels",
    "glRenderbufferStorageMultisample",
    "glScissor",
    "glShaderSource",
    "glStencilFuncSeparate",
    "glStencilMaskSeparate",
    "glStencilOpSeparate",
    "glTexImage2D",
    "glTexImage3D",
    "glTexParameterf",
    "glTexParameterfv",
    "glTexParameteri",
    "glTexSubImage2D",
    "glTexSubImage3D",
    "glUniform1f",
    "glUniform1fv",
    "glUniform1i",
    "glUniform1iv",
    "glUniform2f",
    "glUniform2fv",
    "glUniform3f",
    "glUniform3fv",
    "glUniform4f",
    "glUniform4fv",
    "glUniformBlockBinding",
    "glUniformMatrix3fv",
    "glUniformMatrix4fv",
    "glUseProgram",
    "glVertexAttribDivisor",
    "glVertexAttribIPointer",
    "glVertexAttribPointer",
    "glViewport"
    // clang-format on
  };

  if (inList(kCommon, sizeof(kCommon) / sizeof(kCommon[0]), name)) {
    return true;
  }

  // `glEGLImageTargetTexture2DOES` belongs to the Android AHardwareBuffer import path and is resolved
  // through EGL, which does not exist on WGL, GLX or CGL. It used to sit in the common list, so no desktop
  // GL device could load its entry points (121 of 122) and the engine quietly fell back to another backend
  // - on Windows the GL path never came up at all.
#if defined(__ANDROID__)
  static constexpr const char* kAndroidEgl[] = { "glEGLImageTargetTexture2DOES" };
  if (inList(kAndroidEgl, sizeof(kAndroidEgl) / sizeof(kAndroidEgl[0]), name)) {
    return true;
  }
#endif

  if (dialect == backend::GlDialect::ES) {
    // ES 3.0 core. glClearDepthf and glDepthRangef are the ES spellings; the desktop ones do not
    // exist in an ES context at all.
    static constexpr const char* kEsOnly[] = {
      // clang-format off
      "glClearDepthf",
      "glDepthRangef",
      "glTexStorage2D",
      "glTexStorage3D",
      "glGenerateMipmap",
      "glGetStringi",
      "glDrawBuffers"
      // glSampleMaski is ES 3.1, not 3.0. The draw path already checks it before use, so an ES 3.0
      // driver without it is a device that cannot set a sample mask -- not one that cannot render.
      // Apple's ES 3.0 context is exactly that case, and requiring the symbol refused the device.
      // clang-format on
    };
    return inList(kEsOnly, sizeof(kEsOnly) / sizeof(kEsOnly[0]), name);
  }

  // Desktop 4.1 core. glClearDepth and glDepthRange are the desktop spellings; the ES ones are absent
  // from a core profile context. glPolygonMode is core in desktop GL and impossible in ES, which is
  // exactly why the backend reports wireframe as available on one and not the other.
  static constexpr const char* kDesktopOnly[] = {
    // clang-format off
    "glClearDepth",
    "glDepthRange",
    "glTexStorage2D",
    "glTexStorage3D",
    "glGenerateMipmap",
    "glGetStringi",
    "glDrawBuffers",
    "glSampleMaski",
    "glBindSampler",
    "glGenSamplers",
    "glDeleteSamplers",
    "glSamplerParameteri",
    "glSamplerParameterf",
    "glSamplerParameterfv",
    "glDrawElementsBaseVertex",
    "glDrawElementsInstancedBaseVertex",
    "glDrawArraysIndirect",
    "glDrawElementsIndirect",
    "glBindBufferRange",
    "glMapBufferRange",
    "glUnmapBuffer",
    "glFlushMappedBufferRange",
    "glPolygonMode"
    // clang-format on
  };
  return inList(kDesktopOnly, sizeof(kDesktopOnly) / sizeof(kDesktopOnly[0]), name);
}

} // namespace anonymous

backend::gl::GLApi& backend::gl::getGLApi() noexcept { return gGLApi; }

EGLApi& getEGLApi() noexcept { return gEGLApi; }

void* backend::resolveEGLSymbol(const char* name) noexcept {
  if (gEglGetProcAddress != nullptr) {
    using GetProcAddressFn = void* (*)(const char*);
    if (void* symbol = reinterpret_cast<GetProcAddressFn>(gEglGetProcAddress)(name)) return symbol;
  }
  return defaultLoader(name);
}

bool backend::gl::GLApi::load(GlDialect targetDialect, void* (*getProcAddress)(const char*)) {
  dialect = targetDialect;

  uint32_t resolved = 0;
  uint32_t required = 0;
  const char* firstMissing = nullptr;

#define CORAL_GL_RESOLVE(ret, name, args)                                                                              \
  name = reinterpret_cast<ret(*) args>(resolveEntryPoint(#name, getProcAddress));                                      \
  if (isRequired(targetDialect, #name)) {                                                                              \
    ++required;                                                                                                        \
    if (name != nullptr) {                                                                                             \
      ++resolved;                                                                                                      \
    } else if (firstMissing == nullptr) {                                                                              \
      firstMissing = #name;                                                                                            \
    }                                                                                                                  \
  }

  CORAL_GL_FUNCTIONS(CORAL_GL_RESOLVE)
#undef CORAL_GL_RESOLVE

  // A context that cannot provide every required entry point would fail later, at an unpredictable
  // point, with a null-pointer call. Failing at device creation instead reports "this device cannot
  // run the backend" once, and names the function that is missing.
  loaded = required != 0 && resolved == required;
  missingEntryPoint = loaded ? nullptr : firstMissing;
  return loaded;
}

const char* backend::gl::GLApi::errorName(const GLenum error) noexcept {
  switch (error) {
    case GL_NO_ERROR:
      return "GL_NO_ERROR";
    case GL_INVALID_ENUM:
      return "GL_INVALID_ENUM";
    case GL_INVALID_VALUE:
      return "GL_INVALID_VALUE";
    case GL_INVALID_OPERATION:
      return "GL_INVALID_OPERATION";
    case GL_OUT_OF_MEMORY:
      return "GL_OUT_OF_MEMORY";
    case GL_INVALID_FRAMEBUFFER_OPERATION:
      return "GL_INVALID_FRAMEBUFFER_OPERATION";
    default:
      return "unknown GL error";
  }
}

bool EGLApi::loadEGL(void* (*getProcAddress)(const char*)) {
  // Remember the loader so resolveEGLSymbol can use the same resolution order.
  gEglGetProcAddress = reinterpret_cast<void*>(getProcAddress);

  const auto load = [getProcAddress](const char* name) { return resolveEntryPoint(name, getProcAddress); };

  gEGLApi.eglGetDisplay = reinterpret_cast<decltype(gEGLApi.eglGetDisplay)>(load("eglGetDisplay"));
  gEGLApi.eglGetPlatformDisplay =
    reinterpret_cast<decltype(gEGLApi.eglGetPlatformDisplay)>(load("eglGetPlatformDisplay"));
  gEGLApi.eglInitialize = reinterpret_cast<decltype(gEGLApi.eglInitialize)>(load("eglInitialize"));
  gEGLApi.eglTerminate = reinterpret_cast<decltype(gEGLApi.eglTerminate)>(load("eglTerminate"));
  gEGLApi.eglChooseConfig = reinterpret_cast<decltype(gEGLApi.eglChooseConfig)>(load("eglChooseConfig"));
  gEGLApi.eglCreateWindowSurface =
    reinterpret_cast<decltype(gEGLApi.eglCreateWindowSurface)>(load("eglCreateWindowSurface"));
  gEGLApi.eglCreatePbufferSurface =
    reinterpret_cast<decltype(gEGLApi.eglCreatePbufferSurface)>(load("eglCreatePbufferSurface"));
  gEGLApi.eglCreateContext = reinterpret_cast<decltype(gEGLApi.eglCreateContext)>(load("eglCreateContext"));
  gEGLApi.eglMakeCurrent = reinterpret_cast<decltype(gEGLApi.eglMakeCurrent)>(load("eglMakeCurrent"));
  gEGLApi.eglSwapBuffers = reinterpret_cast<decltype(gEGLApi.eglSwapBuffers)>(load("eglSwapBuffers"));
  gEGLApi.eglSwapInterval = reinterpret_cast<decltype(gEGLApi.eglSwapInterval)>(load("eglSwapInterval"));
  gEGLApi.eglDestroySurface = reinterpret_cast<decltype(gEGLApi.eglDestroySurface)>(load("eglDestroySurface"));
  gEGLApi.eglDestroyContext = reinterpret_cast<decltype(gEGLApi.eglDestroyContext)>(load("eglDestroyContext"));
  gEGLApi.eglGetError = reinterpret_cast<decltype(gEGLApi.eglGetError)>(load("eglGetError"));
  gEGLApi.eglQuerySurface = reinterpret_cast<decltype(gEGLApi.eglQuerySurface)>(load("eglQuerySurface"));
  gEGLApi.eglBindAPI = reinterpret_cast<decltype(gEGLApi.eglBindAPI)>(load("eglBindAPI"));
  gEGLApi.eglGetNativeClientBufferANDROID =
    reinterpret_cast<decltype(gEGLApi.eglGetNativeClientBufferANDROID)>(load("eglGetNativeClientBufferANDROID"));
  gEGLApi.eglCreateImageKHR = reinterpret_cast<decltype(gEGLApi.eglCreateImageKHR)>(load("eglCreateImageKHR"));
  gEGLApi.eglDestroyImageKHR = reinterpret_cast<decltype(gEGLApi.eglDestroyImageKHR)>(load("eglDestroyImageKHR"));

  // The minimum needed to bring up a context. Anything beyond this is optional and tested for at the
  // point of use, so a driver without eglQuerySurface still gets a working device.
  gEGLApi.loaded = gEGLApi.eglGetDisplay != nullptr && gEGLApi.eglInitialize != nullptr &&
                   gEGLApi.eglChooseConfig != nullptr && gEGLApi.eglCreateContext != nullptr &&
                   gEGLApi.eglMakeCurrent != nullptr;
  return gEGLApi.loaded;
}

namespace backend {

const char* eglErrorName(const int32_t error) noexcept {
  switch (error) {
    case EGL_SUCCESS:
      return "EGL_SUCCESS";
    case EGL_NOT_INITIALIZED:
      return "EGL_NOT_INITIALIZED";
    case EGL_BAD_ACCESS:
      return "EGL_BAD_ACCESS";
    case EGL_BAD_ALLOC:
      return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE:
      return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONFIG:
      return "EGL_BAD_CONFIG";
    case EGL_BAD_CONTEXT:
      return "EGL_BAD_CONTEXT";
    case EGL_BAD_CURRENT_SURFACE:
      return "EGL_BAD_CURRENT_SURFACE";
    case EGL_BAD_DISPLAY:
      return "EGL_BAD_DISPLAY";
    case EGL_BAD_MATCH:
      return "EGL_BAD_MATCH";
    case EGL_BAD_NATIVE_PIXMAP:
      return "EGL_BAD_NATIVE_PIXMAP";
    case EGL_BAD_NATIVE_WINDOW:
      return "EGL_BAD_NATIVE_WINDOW";
    case EGL_BAD_PARAMETER:
      return "EGL_BAD_PARAMETER";
    case EGL_BAD_SURFACE:
      return "EGL_BAD_SURFACE";
    case EGL_CONTEXT_LOST:
      return "EGL_CONTEXT_LOST";
    default:
      return "unknown EGL error";
  }
}

} // namespace backend

