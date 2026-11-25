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

#ifndef CORAL_APPLE_IOSURFACE_OUTPUT_NODE_H
#define CORAL_APPLE_IOSURFACE_OUTPUT_NODE_H

#include <effect/EffectOutputNode.h>
#include <effect/EffectTypes.h>

#include <IOSurface/IOSurface.h>

#include <cstdint>
#include <memory>

namespace coral {

/**
 * An output node that renders the graph's final frame into an `IOSurface`, with no CPU copy.
 *
 * This is the Apple counterpart of `AndroidHardwareBufferOutputNode` and
 * `WindowsD3D11EncoderOutputNode`: the caller owns a surface that a VideoToolbox encoder, a Metal/OpenGL
 * compositor or another process consumes, and the graph copies its last frame into that surface.
 *
 * ## Backend support
 *
 * | Device | How the surface is written | Zero copy |
 * |---|---|---|
 * | Metal | the surface is imported as a writeback `id<MTLTexture>` and the final frame is blitted into it | yes |
 * | OpenGL ES (iOS) | `CVOpenGLESTextureCache` exposes the surface-backed `CVPixelBuffer` as a render/copy target | yes |
 * | OpenGL (macOS) | the graph copies into a 2D texture and one GPU blit pushes it into the `GL_TEXTURE_RECTANGLE` surface | **no** - no CPU copy, one GPU copy |
 *
 * As with the input node, macOS OpenGL cannot address an IOSurface as `GL_TEXTURE_2D`; the node records
 * the frame into a normal 2D texture and normalises it into the surface after the submission, on the
 * render thread. `isZeroCopy()` reports false there.
 *
 * A single-plane RGBA8/BGRA8/RGBA16F surface is supported, and the surface's layout must match the
 * format the node was created with. The graph's own frame format is converted by the engine's output
 * scale pass when it differs.
 *
 * ## Synchronisation
 *
 * The surface is written during the frame's GPU submission. A consumer must wait for that work before
 * reading the surface: call `EffectEngine::flushAndWait` or wait on the fence returned by
 * `EffectEngine::execute(fence)`. On Metal the copy is part of the frame's own submission, so that
 * fence covers it. On OpenGL the node issues the final macOS blit after the submission and finishes it
 * before the frame returns, which makes the engine-level wait sufficient there too (at the cost of a
 * GPU wait on that backend only); iOS OpenGL writes the Core Video texture directly in the submission.
 * `frameCount()` counts the frames the node copied.
 *
 * The node retains the surface and never presents it to a window; it is the recording/export path, not
 * a preview path. Presentation to a `CAMetalLayer`/window is a different output node.
 */
class AppleIOSurfaceOutputNode final : public EffectOutputNode {
public:
  /**
   * Creates an output for @p surface.
   *
   * @param surface the destination; the node takes its own reference. Null is refused.
   * @param width   pixel width, or 0 to ask the surface.
   * @param height  pixel height, or 0 to ask the surface.
   * @param format  the surface's colour layout: RGBA8, BGRA8 or RGBA16F. Video encoders take BGRA8.
   */
  static std::shared_ptr<AppleIOSurfaceOutputNode> create(IOSurfaceRef surface, uint32_t width = 0,
                                                           uint32_t height = 0,
                                                           EffectPixelFormat format = EffectPixelFormat::BGRA8);

  ~AppleIOSurfaceOutputNode() override;

  bool isReady() const noexcept override;

  /**
   * Applies the output configuration.
   *
   * The size and pixel layout are properties of the destination surface: a non-zero width/height that
   * differs from the surface is refused with `InvalidArgument`, and `config.format` is overridden with
   * the format the node was created with (a default-constructed config, for instance, must not turn a
   * BGRA surface into an RGBA one). Rotation, mirroring, scale mode and the letterbox colour are taken
   * from @p config and applied by the graph's output pass.
   */
  EffectResult configure(const EffectOutputConfig& config) override;

  /** The surface this node writes. Borrowed for the duration of the call; the node holds the reference. */
  [[nodiscard]] IOSurfaceRef surface() const noexcept;

  [[nodiscard]] uint32_t width() const noexcept;
  [[nodiscard]] uint32_t height() const noexcept;

  /** @return true when the frame was written into the surface's own storage, with no intermediate copy. */
  [[nodiscard]] bool isZeroCopy() const noexcept;

  /** @return how many frames the node has recorded into the surface. */
  [[nodiscard]] uint64_t frameCount() const noexcept;

  /** True after a frame could not be written into the surface (a lost device or a dead surface). */
  [[nodiscard]] bool hasPresentFailure() const noexcept;

  /** Implementation detail; opaque to a client. */
  struct State;

private:
  AppleIOSurfaceOutputNode(IOSurfaceRef surface, uint32_t width, uint32_t height, EffectPixelFormat format);

  std::shared_ptr<State> mState;
};

} // namespace coral

#endif // CORAL_APPLE_IOSURFACE_OUTPUT_NODE_H
