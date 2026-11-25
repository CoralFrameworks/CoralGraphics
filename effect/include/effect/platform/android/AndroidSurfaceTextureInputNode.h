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

#ifndef CORAL_ANDROID_SURFACE_TEXTURE_INPUT_NODE_H
#define CORAL_ANDROID_SURFACE_TEXTURE_INPUT_NODE_H

#include <effect/EffectInputNode.h>

#include <jni.h>

#include <cstdint>
#include <memory>

namespace coral {

/**
 * An input node fed by a Java `android.graphics.SurfaceTexture` (a camera, a decoder, a `MediaPlayer`).
 *
 * ## Ownership and threading
 *
 * The node takes a JNI global reference to @p surfaceTexture and the caller keeps ownership of its own
 * reference. `updateTexImage()` and `getTransformMatrix()` are called on the engine's render thread, which
 * is where the shared EGL context is current; the app must therefore create the `SurfaceTexture` on a
 * context in the engine's share group (pass that `EGLContext` as `EffectEngineConfig::sharedContext`).
 *
 * @p textureName is the `GL_TEXTURE_EXTERNAL_OES` name that `SurfaceTexture` updates. It is stable for the
 * SurfaceTexture's lifetime and is imported once.
 *
 * The external texture is converted to an ordinary RGBA8 pooled frame by the node (one full-screen pass
 * that also applies the SurfaceTexture transform), so every effect pass samples a normal `sampler2D` and
 * the graph does not have to know that its first node is a camera.
 */
class AndroidSurfaceTextureInputNode final : public EffectInputNode {
public:
  /**
   * @param env           JNI environment of the calling thread.
   * @param surfaceTexture a `android.graphics.SurfaceTexture`, non-null.
   * @param textureName   the OES texture name the SurfaceTexture updates.
   * @param width         frame width in pixels.
   * @param height        frame height in pixels.
   */
  static std::shared_ptr<AndroidSurfaceTextureInputNode> create(JNIEnv* env, jobject surfaceTexture,
                                                                uint32_t textureName, uint32_t width,
                                                                uint32_t height);

  ~AndroidSurfaceTextureInputNode() override;

  bool hasFrame() const noexcept override;
  bool repeatLastFrame() const noexcept override;
  void setRepeatLastFrame(bool repeat) override;

  [[nodiscard]] uint32_t width() const noexcept;
  [[nodiscard]] uint32_t height() const noexcept;

  /**
   * Marks that the SurfaceTexture's `OnFrameAvailableListener` fired.
   *
   * Optional. Before the first call the node keeps its historical behaviour: `updateTexImage()` once per
   * produced frame. Once called, the node only calls `updateTexImage()` for a notified frame; with no new
   * frame the engine repeats the last picture (or drops it, per `repeatLastFrame`). Wire the listener to
   * this method for real camera/video sources so an idle source does not re-render stale content.
   */
  void notifyFrameAvailable() noexcept;

  /**
   * Tells the node that the application calls `SurfaceTexture.updateTexImage()` itself, on the thread and
   * EGL context that created the texture.
   *
   * Some drivers require `updateTexImage` on the creating context even when the render context shares
   * with it. In that mode the engine only reads `getTimestamp`/`getTransformMatrix` and samples the OES
   * texture; the caller must update the texture before each render. Default is false.
   */
  void setUpdateTextureFromApp(bool enabled) noexcept;

  /** Implementation detail; opaque to a client. */
  struct State;

private:
  AndroidSurfaceTextureInputNode(JNIEnv* env, jobject surfaceTexture, uint32_t textureName, uint32_t width,
                                 uint32_t height);

  std::shared_ptr<State> mState;
};

} // namespace coral

#endif // CORAL_ANDROID_SURFACE_TEXTURE_INPUT_NODE_H
