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

#ifndef CORAL_APPLE_IOSURFACE_INPUT_NODE_H
#define CORAL_APPLE_IOSURFACE_INPUT_NODE_H

#include <effect/EffectInputNode.h>
#include <effect/EffectTypes.h>

#include <IOSurface/IOSurfaceRef.h>

#include <cstdint>
#include <memory>

namespace coral {

/**
 * An input node that samples an `IOSurface` without a CPU copy.
 *
 * This is the Apple counterpart of `AndroidHardwareBufferInputNode` and `WindowsD3D11InputNode`: the
 * producer keeps its own GPU allocation (a VideoToolbox decoder, a camera pipeline, a Metal/OpenGL
 * renderer, another process through a surface-backed `CVPixelBuffer`) and the node hands that storage
 * to the graph.
 *
 * ## Backend support
 *
 * | Device | How the surface is reached | Zero copy |
 * |---|---|---|
 * | Metal | `newTextureWithDescriptor:iosurface:plane:` wraps the surface as an `id<MTLTexture>` | yes |
 * | OpenGL ES (iOS) | `CVOpenGLESTextureCache` maps the surface-backed `CVPixelBuffer` to a `GL_TEXTURE_2D` | yes |
 * | OpenGL (macOS) | `CGLTexImageIOSurface2D` binds it as `GL_TEXTURE_RECTANGLE`, then one GPU blit normalises it into a 2D texture | **no** - no CPU copy, but one GPU copy |
 * | Metal + NV12 | plane 0 and plane 1 are imported as R8 and R8G8 textures, then one conversion pass produces RGBA | no CPU copy; one GPU conversion |
 *
 * macOS's OpenGL cannot bind an IOSurface as a `GL_TEXTURE_2D` (`CGLTexImageIOSurface2D` only accepts
 * `GL_TEXTURE_RECTANGLE_ARB`), and the effect shader dialect samples `sampler2D`. The node therefore
 * refreshes a 2D image with `glBlitFramebuffer` once per frame on that backend. The pixels never visit
 * the CPU, but `isZeroCopy()` reports false there so a caller that needs strict zero-copy can choose
 * Metal.
 *
 * Core Video does not map an IOSurface to a GLES texture in the iOS simulator
 * (`CVOpenGLESTextureCacheCreateTextureFromImage` reports -6683 there), so the iOS OpenGL path is a
 * device path; the Metal path runs on both the simulator and a device.
 *
 * Two layouts are supported: a single-plane RGBA8/BGRA8/RGBA16F surface, and an 8-bit bi-planar
 * NV12 (`420v`/`420f`) surface. NV12 is a Metal feature: the node imports the two planes and runs one
 * conversion pass to RGBA. The OpenGL/ES backend cannot import the two planes yet, so a caller that
 * may run on OpenGL should ask the decoder for BGRA (`MediaFormat::processFormat`) instead.
 *
 * ## Lifetime and synchronisation
 *
 * The node **retains** the surface for as long as it may still sample it (the input queue and the last
 * produced frame), so the caller may release its own reference immediately after `submit`. It never
 * writes to the surface.
 *
 * An IOSurface has no fence. The producer must not overwrite the pixels of a surface the engine is
 * still reading: `submit` accepts one surface per frame, and the engine's frame serialisation protects
 * the node's own state, not the producer's GPU work. Publish a surface only when its frame is complete -
 * a decoder's `CVPixelBuffer` callback is the usual point - and keep a small pool of surfaces so the
 * producer and the engine do not write and read the same image.
 */
class AppleIOSurfaceInputNode final : public EffectInputNode {
public:
  /**
   * Creates a node for @p surface.
   *
   * @param surface   the iOS/macOS shared surface; the node takes its own reference. Null is refused.
   * @param width     pixel width, or 0 to ask the surface. A non-zero value that disagrees with the
   *                  surface is refused rather than silently importing a different image.
   * @param height    pixel height, or 0 to ask the surface.
   * @param format    the surface's colour layout: RGBA8, BGRA8, RGBA16F or YUV420 (NV12). Video frames
   *                  are normally BGRA8 (`kCVPixelFormatType_32BGRA`) or NV12 (`420v`/`420f`).
   * @param queueDepth how many submitted frames may wait for the render thread before the backpressure
   *                  policy applies. 0 chooses 4.
   */
  static std::shared_ptr<AppleIOSurfaceInputNode> create(IOSurfaceRef surface, uint32_t width = 0,
                                                          uint32_t height = 0,
                                                          EffectPixelFormat format = EffectPixelFormat::BGRA8,
                                                          uint32_t queueDepth = 4);

  /** Creates the node before the producer has a surface; call `submit`/`setSurface` later. */
  static std::shared_ptr<AppleIOSurfaceInputNode> create(uint32_t width, uint32_t height,
                                                          EffectPixelFormat format = EffectPixelFormat::BGRA8,
                                                          uint32_t queueDepth = 4);

  ~AppleIOSurfaceInputNode() override;

  bool hasFrame() const noexcept override;
  bool repeatLastFrame() const noexcept override;
  void setRepeatLastFrame(bool repeat) override;

  [[nodiscard]] uint32_t width() const noexcept;
  [[nodiscard]] uint32_t height() const noexcept;

  /** Replaces the surface without a timestamp; the next frame uses the engine's frame timestamp. */
  EffectResult setSurface(IOSurfaceRef surface, uint32_t width = 0, uint32_t height = 0,
                          EffectPixelFormat format = EffectPixelFormat::BGRA8);

  /**
   * Publishes one surface and its presentation time as one atomic update.
   *
   * @param lifetime optional producer token held until the submission that sampled this frame
   *                 completed. A decoder that recycles surfaces uses it to know when one is free.
   */
  EffectResult submit(IOSurfaceRef surface, uint32_t width, uint32_t height, EffectPixelFormat format,
                      RenderTime timestamp, std::shared_ptr<void> lifetime = nullptr);

  /** The unified effect input entry point for a `PixelBufferView` whose native handle is an IOSurface. */
  EffectResult submit(const PixelBufferView& view) override;

  /** Sets the presentation time carried with the next frame. */
  void setTimestamp(RenderTime timestamp);

  /**
   * @return true when the last produced frame sampled the surface's own storage.
   *
   * False on the macOS OpenGL backend, where the surface is normalised through one GPU blit. NV12 on
   * Metal reports true even though it runs one GPU conversion, because the decoder's surface and its
   * pixels never pass through the CPU; see the class documentation. Meaningless before the first frame.
   */
  [[nodiscard]] bool isZeroCopy() const noexcept;

  /** Implementation detail; opaque to a client. */
  struct State;

private:
  AppleIOSurfaceInputNode(IOSurfaceRef surface, uint32_t width, uint32_t height, EffectPixelFormat format,
                          uint32_t queueDepth);

  std::shared_ptr<State> mState;
};

} // namespace coral

#endif // CORAL_APPLE_IOSURFACE_INPUT_NODE_H
