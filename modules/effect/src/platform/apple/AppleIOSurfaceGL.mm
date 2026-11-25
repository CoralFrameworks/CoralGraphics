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
 * @file AppleIOSurfaceGL.mm
 *
 * The OpenGL bridge between an IOSurface and the effect graph. The two Apple GL platforms reach the same
 * surface by different routes:
 *
 *  - iOS's `CVOpenGLESTextureCache` maps a surface-backed `CVPixelBuffer` to a `GL_TEXTURE_2D` in the
 *    current EAGL context. Both directions are zero-copy.
 *  - macOS's `CGLTexImageIOSurface2D` only binds `GL_TEXTURE_RECTANGLE_ARB`, and the effect shader dialect
 *    samples `sampler2D`. The bridge owns a 2D texture and one scratch FBO pair, and normalises with
 *    `glBlitFramebuffer`. That is a GPU copy, never a CPU copy.
 *
 * Every call happens on the engine's render thread with the engine's context current; the effect runtimes
 * are the only callers.
 */

#include "AppleIOSurfaceGL.h"

#include <TargetConditionals.h>

#if TARGET_OS_IPHONE
#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES3/gl.h>
#import <OpenGLES/ES3/glext.h>

#import <CoreVideo/CVPixelBuffer.h>
#import <CoreVideo/CVOpenGLESTextureCache.h>
#else
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#endif

#include <string>
#include <utility>

namespace coral::detail {
namespace {

struct GLFormat {
  GLint internalFormat = 0;
  GLenum format = 0;
  GLenum type = 0;
};

#if TARGET_OS_IPHONE
// GLES 3.0 has no `GL_BGRA` in its core header; the value is the same one `GL_BGRA_EXT` and Core Video use.
constexpr GLenum kGLBGRA = 0x80E1;
#else
constexpr GLenum kGLBGRA = GL_BGRA;
#endif

bool describeFormat(const EffectPixelFormat format, GLFormat& out) noexcept {
  switch (format) {
    case EffectPixelFormat::RGBA8:
      // Unsized RGBA rather than GL_RGBA8: `CGLTexImageIOSurface2D`'s supported table names the unsized internal
      // formats, and Core Video's texture cache takes them too.
      out = { GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE };
      return true;
    case EffectPixelFormat::BGRA8:
#if TARGET_OS_IPHONE
      out = { GL_RGBA, kGLBGRA, GL_UNSIGNED_BYTE };
#else
      out = { GL_RGBA, kGLBGRA, GL_UNSIGNED_INT_8_8_8_8_REV };
#endif
      return true;
    case EffectPixelFormat::RGBA16F:
      out = { GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT };
      return true;
    default:
      return false;
  }
}

std::string describeGLError(const GLenum error) noexcept {
  switch (error) {
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
      return "GL error " + std::to_string(static_cast<uint32_t>(error));
  }
}

bool checkError(const char* const what, std::string& error) {
  const GLenum glError = glGetError();
  if (glError == GL_NO_ERROR) return true;
  error = std::string(what) + " failed with " + describeGLError(glError);
  return false;
}

bool framebufferComplete(const GLenum target, const char* const label, std::string& error) {
  const GLenum status = glCheckFramebufferStatus(target);
  if (status == GL_FRAMEBUFFER_COMPLETE) return true;
  error = std::string("the IOSurface ") + label + " framebuffer is incomplete (" +
          std::to_string(static_cast<uint32_t>(status)) + ")";
  return false;
}

} // namespace anonymous

AppleGLIOSurfaceBridge::~AppleGLIOSurfaceBridge() { release(); }

std::shared_ptr<AppleGLIOSurfaceBridge> AppleGLIOSurfaceBridge::create(IOSurfaceRef surface,
                                                                       const uint32_t width,
                                                                       const uint32_t height,
                                                                       const EffectPixelFormat format,
                                                                       const bool writeback,
                                                                       std::string& error) {
  if (surface == nullptr || width == 0 || height == 0) {
    error = "an IOSurface bridge needs a surface and a non-zero extent";
    return nullptr;
  }
  GLFormat glFormat;
  if (!describeFormat(format, glFormat)) {
    error = "the IOSurface bridge supports RGBA8, BGRA8 and RGBA16F only";
    return nullptr;
  }

  auto bridge = std::shared_ptr<AppleGLIOSurfaceBridge>(new AppleGLIOSurfaceBridge());
  bridge->mSurface = surface;
  CFRetain(surface);
  bridge->mWidth = width;
  bridge->mHeight = height;
  bridge->mWriteback = writeback;

#if TARGET_OS_IPHONE
  EAGLContext* const context = [EAGLContext currentContext];
  if (context == nil) {
    error = "the IOSurface bridge has no current EAGL context";
    bridge->release();
    return nullptr;
  }

  // The compatibility key is what makes Core Video keep the surface mapped for OpenGL ES instead of
  // copying it under the cache. A surface that cannot take it is still tried without, which some
  // producer-created CVPixelBuffers require. A CF dictionary rather than an Objective-C one keeps this
  // file free of Foundation and of autorelease lifetime questions.
  CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
          kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  if (attributes != nullptr) {
    CFDictionaryAddValue(attributes, kCVPixelBufferOpenGLESCompatibilityKey, kCFBooleanTrue);
  }
  CVReturn result = CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, surface, attributes,
          &bridge->mPixelBuffer);
  if (attributes != nullptr) CFRelease(attributes);
  if (result != kCVReturnSuccess || bridge->mPixelBuffer == nullptr) {
    bridge->mPixelBuffer = nullptr;
    result = CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, surface, nullptr, &bridge->mPixelBuffer);
  }
  if (result != kCVReturnSuccess || bridge->mPixelBuffer == nullptr) {
    error = "the IOSurface could not be wrapped as a CVPixelBuffer (" + std::to_string(static_cast<int>(result)) + ")";
    bridge->release();
    return nullptr;
  }
  if (CVPixelBufferGetWidth(bridge->mPixelBuffer) != width || CVPixelBufferGetHeight(bridge->mPixelBuffer) != height) {
    error = "the CVPixelBuffer's extent does not match the IOSurface extent";
    bridge->release();
    return nullptr;
  }

  result = CVOpenGLESTextureCacheCreate(kCFAllocatorDefault, nullptr, context, nullptr, &bridge->mTextureCache);
  if (result != kCVReturnSuccess || bridge->mTextureCache == nullptr) {
    error = "a CVOpenGLESTextureCache could not be created (" + std::to_string(static_cast<int>(result)) + ")";
    bridge->release();
    return nullptr;
  }

  result = CVOpenGLESTextureCacheCreateTextureFromImage(kCFAllocatorDefault, bridge->mTextureCache,
          bridge->mPixelBuffer, nullptr, GL_TEXTURE_2D, glFormat.internalFormat, width, height, glFormat.format,
          glFormat.type, 0, &bridge->mCVTexture);
  if (result != kCVReturnSuccess || bridge->mCVTexture == nullptr) {
    error = "CVOpenGLESTextureCache could not map the surface (" + std::to_string(static_cast<int>(result)) + ")";
    bridge->release();
    return nullptr;
  }

  bridge->mTexture = CVOpenGLESTextureGetName(bridge->mCVTexture);
  if (bridge->mTexture == 0) {
    error = "the Core Video texture has no GL name";
    bridge->release();
    return nullptr;
  }
  glBindTexture(GL_TEXTURE_2D, bridge->mTexture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  // A Core Video texture has exactly one level; saying so keeps a mipmapped sampler object from making it
  // incomplete (the driver then samples black and logs that the 2D texture index is unloadable).
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  if (!checkError("binding the Core Video texture", error)) {
    bridge->release();
    return nullptr;
  }
  bridge->mZeroCopy = true;
#else
  CGLContextObj const context = CGLGetCurrentContext();
  if (context == nullptr) {
    error = "the IOSurface bridge has no current CGL context";
    bridge->release();
    return nullptr;
  }

  glGenTextures(1, &bridge->mSurfaceTexture);
  glGenTextures(1, &bridge->mTexture);
  glGenFramebuffers(1, &bridge->mReadFramebuffer);
  glGenFramebuffers(1, &bridge->mDrawFramebuffer);
  if (bridge->mSurfaceTexture == 0 || bridge->mTexture == 0 || bridge->mReadFramebuffer == 0 ||
      bridge->mDrawFramebuffer == 0) {
    error = "the IOSurface bridge could not allocate its GL textures";
    bridge->release();
    return nullptr;
  }

  // `CGLTexImageIOSurface2D` is the only public CGL entry point that binds a surface, and it accepts
  // GL_TEXTURE_RECTANGLE only. Its storage is the IOSurface itself; no glTexImage2D call is allowed after this.
  glBindTexture(GL_TEXTURE_RECTANGLE, bridge->mSurfaceTexture);
  const CGLError surfaceError = CGLTexImageIOSurface2D(context, GL_TEXTURE_RECTANGLE, glFormat.internalFormat,
          static_cast<GLsizei>(width), static_cast<GLsizei>(height), glFormat.format, glFormat.type, surface, 0);
  if (surfaceError != kCGLNoError) {
    error = "CGLTexImageIOSurface2D refused the surface (CGL error " + std::to_string(surfaceError) + ")";
    glBindTexture(GL_TEXTURE_RECTANGLE, 0);
    bridge->release();
    return nullptr;
  }
  glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_RECTANGLE, 0);

  // The normalisation target. It is a normal sampled 2D texture, which the engine imports like any other
  // externally owned GL name and which a shader can sample through the engine's usual sampler2D bindings.
  glBindTexture(GL_TEXTURE_2D, bridge->mTexture);
  glTexImage2D(GL_TEXTURE_2D, 0, glFormat.internalFormat, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
          0, glFormat.format, glFormat.type, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  // One level only. A sampler object with a mipmap minification filter would otherwise make the texture
  // incomplete and the driver would sample black, which is exactly the "GLD_TEXTURE_INDEX_2D is unloadable"
  // failure a mipmapped sampler against a single-level texture produces.
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  if (!checkError("allocating the IOSurface normalisation texture", error)) {
    bridge->release();
    return nullptr;
  }
  bridge->mZeroCopy = false;
#endif
  return bridge;
}

bool AppleGLIOSurfaceBridge::refresh(std::string& error) {
  if (mWriteback) {
    error = "an IOSurface writeback bridge has nothing to refresh";
    return false;
  }
#if TARGET_OS_IPHONE
  // The texture *is* the surface, so there is nothing to copy. Core Video asks for a flush when the
  // contents were produced outside the GL context the cache belongs to.
  if (mTextureCache != nullptr) CVOpenGLESTextureCacheFlush(mTextureCache, 0);
  (void) error;
  return true;
#else
  // Surface -> normalised 2D target. The surface's contents are producer-owned and may change every frame,
  // so this runs on every produce, not only when the surface is replaced.
  return blit(mSurfaceTexture, mTexture, true, error);
#endif
}

bool AppleGLIOSurfaceBridge::present(std::string& error) {
  if (!mWriteback) {
    error = "an IOSurface read bridge has nothing to present";
    return false;
  }
#if TARGET_OS_IPHONE
  if (mTextureCache != nullptr) CVOpenGLESTextureCacheFlush(mTextureCache, 0);
  glFlush();
  (void) error;
  return true;
#else
  // Normalised 2D image -> the surface the consumer owns.
  const bool copied = blit(mTexture, mSurfaceTexture, false, error);
  if (copied) {
    // The blit is issued after the graph's submission, so the engine's fence does not cover it. Finishing here is
    // what makes a later `EffectEngine::flushAndWait()` (or a direct IOSurface read) actually observe the frame.
    // The output path is recording/export, where correctness beats the wait; Metal needs no equivalent because its
    // copy is part of the frame's own submission.
    glFinish();
  }
  return copied;
#endif
}

#if !TARGET_OS_IPHONE
bool AppleGLIOSurfaceBridge::blit(const uint32_t readTexture, const uint32_t drawTexture,
                                   const bool readIsSurface, std::string& error) {
  GLint previousRead = 0;
  GLint previousDraw = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);

  glBindFramebuffer(GL_READ_FRAMEBUFFER, mReadFramebuffer);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
          readIsSurface ? GL_TEXTURE_RECTANGLE : GL_TEXTURE_2D, readTexture, 0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mDrawFramebuffer);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
          readIsSurface ? GL_TEXTURE_2D : GL_TEXTURE_RECTANGLE, drawTexture, 0);

  if (!framebufferComplete(GL_READ_FRAMEBUFFER, "source", error) ||
      !framebufferComplete(GL_DRAW_FRAMEBUFFER, "destination", error)) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDraw));
    return false;
  }

  // A straight pixel-for-pixel copy. Both images are in the same row order, so the surface's row 0 stays the
  // frame's row 0: a producer writes top-down and the consumer reads top-down, which is the IOSurface contract.
  glBlitFramebuffer(0, 0, static_cast<GLint>(mWidth), static_cast<GLint>(mHeight),
          0, 0, static_cast<GLint>(mWidth), static_cast<GLint>(mHeight), GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDraw));
  glFlush();
  return checkError("the IOSurface blit", error);
}
#endif

void AppleGLIOSurfaceBridge::release() noexcept {
  if (mSurface != nullptr) {
    CFRelease(mSurface);
    mSurface = nullptr;
  }
#if TARGET_OS_IPHONE
  if (mCVTexture != nullptr) {
    CFRelease(mCVTexture);
    mCVTexture = nullptr;
  }
  if (mPixelBuffer != nullptr) {
    CFRelease(mPixelBuffer);
    mPixelBuffer = nullptr;
  }
  if (mTextureCache != nullptr) {
    CFRelease(mTextureCache);
    mTextureCache = nullptr;
  }
  mTexture = 0;
  mZeroCopy = false;
#else
  if (mSurfaceTexture != 0 || mTexture != 0 || mReadFramebuffer != 0 || mDrawFramebuffer != 0) {
    if (mSurfaceTexture != 0) glDeleteTextures(1, &mSurfaceTexture);
    if (mTexture != 0) glDeleteTextures(1, &mTexture);
    if (mReadFramebuffer != 0) glDeleteFramebuffers(1, &mReadFramebuffer);
    if (mDrawFramebuffer != 0) glDeleteFramebuffers(1, &mDrawFramebuffer);
    mSurfaceTexture = 0;
    mTexture = 0;
    mReadFramebuffer = 0;
    mDrawFramebuffer = 0;
  }
#endif
}

} // namespace coral::detail
