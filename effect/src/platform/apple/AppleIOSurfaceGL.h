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

#ifndef CORAL_APPLE_IOSURFACE_GL_H
#define CORAL_APPLE_IOSURFACE_GL_H

#include <effect/EffectTypes.h>

#include <IOSurface/IOSurface.h>

#include <TargetConditionals.h>

#if TARGET_OS_IPHONE
#include <CoreVideo/CoreVideo.h>
#endif

#include <cstdint>
#include <memory>
#include <string>

namespace coral::detail {

/**
 * The OpenGL half of the Apple IOSurface nodes.
 *
 * The engine's OpenGL backend samples `GL_TEXTURE_2D` only, while an IOSurface is a platform object the
 * GL context has to be told how to interpret. The two Apple GL platforms differ enough that the bridge
 * exists at all:
 *
 *  - **iOS (EAGL)** has `CVOpenGLESTextureCache`, which maps a surface-backed `CVPixelBuffer` to a
 *    `GL_TEXTURE_2D` in the current context. The map is zero-copy, and the texture is usable as a
 *    sample source and as a copy destination.
 *  - **macOS (CGL)** has `CGLTexImageIOSurface2D`, which accepts only `GL_TEXTURE_RECTANGLE_ARB`.
 *    The effect shader dialect samples `sampler2D`, so the bridge owns both textures and normalises
 *    between them with `glBlitFramebuffer` on the render thread. That is one GPU copy per frame, never
 *    a CPU copy, and `isZeroCopy()` is false.
 *
 * The bridge is thread-affine: every method runs on the engine's render thread with the engine's GL
 * context current, which is where the effect runtimes call it.
 */
class AppleGLIOSurfaceBridge final {
public:
  /**
   * Creates the GL view of @p surface.
   *
   * @param surface  the shared surface; the bridge retains it for its lifetime.
   * @param width    the surface's pixel width.
   * @param height   the surface's pixel height.
   * @param format   RGBA8, BGRA8 or RGBA16F, matching the surface's layout.
   * @param writeback true for an output (the graph writes into `texture()` and `present()` pushes it
   *                  into the surface); false for an input (`refresh()` pulls the surface into it).
   * @param error    receives a diagnostic when creation fails.
   * @return the bridge, or null with @p error set.
   */
  static std::shared_ptr<AppleGLIOSurfaceBridge> create(IOSurfaceRef surface, uint32_t width, uint32_t height,
                                                        EffectPixelFormat format, bool writeback,
                                                        std::string& error);

  ~AppleGLIOSurfaceBridge();

  AppleGLIOSurfaceBridge(const AppleGLIOSurfaceBridge&) = delete;
  AppleGLIOSurfaceBridge& operator=(const AppleGLIOSurfaceBridge&) = delete;

  /**
   * The `GL_TEXTURE_2D` name the engine imports.
   *
   * On iOS this *is* the surface-backed texture. On macOS it is the 2D normalisation target, which is
   * filled by `refresh()` (input) or flushed into the surface by `present()` (output).
   */
  [[nodiscard]] uint32_t texture() const noexcept { return mTexture; }

  /** @return true when `texture()` samples the surface's own storage (iOS). */
  [[nodiscard]] bool isZeroCopy() const noexcept { return mZeroCopy; }

  /** Input: copies the surface's current image into `texture()`. A no-op on the zero-copy path. */
  bool refresh(std::string& error);

  /** Output: copies `texture()` into the surface. A no-op (plus a cache flush) on the zero-copy path. */
  bool present(std::string& error);

private:
  AppleGLIOSurfaceBridge() = default;

  void release() noexcept;

  /**
   * Blits between the two textures through the bridge's scratch framebuffer pair.
   *
   * @param readTexture   the source name.
   * @param drawTexture   the destination name.
   * @param readIsSurface true when the source is the `GL_TEXTURE_RECTANGLE` surface (input direction);
   *                      false when the source is the 2D texture (output direction).
   */
  bool blit(uint32_t readTexture, uint32_t drawTexture, bool readIsSurface, std::string& error);

  uint32_t mWidth = 0;
  uint32_t mHeight = 0;
  bool mWriteback = false;
  bool mZeroCopy = false;

  uint32_t mTexture = 0;
  /** The `GL_TEXTURE_RECTANGLE` name that wraps the IOSurface on macOS; 0 elsewhere. */
  uint32_t mSurfaceTexture = 0;
  uint32_t mReadFramebuffer = 0;
  uint32_t mDrawFramebuffer = 0;

  IOSurfaceRef mSurface = nullptr;

#if TARGET_OS_IPHONE
  CVPixelBufferRef mPixelBuffer = nullptr;
  CVOpenGLESTextureRef mCVTexture = nullptr;
  CVOpenGLESTextureCacheRef mTextureCache = nullptr;
#endif
};

} // namespace coral::detail

#endif // CORAL_APPLE_IOSURFACE_GL_H
