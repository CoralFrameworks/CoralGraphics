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

#ifndef CORAL_WINDOWS_D3D11_INPUT_NODE_H
#define CORAL_WINDOWS_D3D11_INPUT_NODE_H

#include <effect/EffectInputNode.h>
#include <effect/EffectTypes.h>

#include <cstdint>
#include <memory>

// The COM type is only named through a pointer, so the public header does not pull in <d3d11.h>.
struct ID3D11Texture2D;

namespace coral {

/**
 * An input node fed by a Direct3D 11 shared texture (a decoder, a capture engine, another compositor).
 *
 * The texture should be created with `D3D11_RESOURCE_MISC_SHARED_NTHANDLE`; the node turns it into an
 * NT handle and either imports it zero-copy into the engine's Vulkan device or, when the backend cannot
 * import, copies it through a D3D11 staging texture and uploads the pixels. A legacy
 * `D3D11_RESOURCE_MISC_SHARED` texture is also accepted, but only through the staging fallback because
 * Vulkan cannot import a legacy handle.
 *
 * ## Synchronisation
 *
 * The node does not own the producer's device and cannot know when a new frame is complete. The caller
 * must only call `setTexture` or start sampling a texture whose contents are stable; `setTexture` while
 * frames are in flight is refused at the graph level by the engine's frame serialisation only for the
 * node's own state, not for the GPU. This is the same contract the RHI's native-texture import states.
 */
class WindowsD3D11InputNode final : public EffectInputNode {
public:
  /**
   * @param texture a shared D3D11 texture; the node takes its own reference.
   * @param format  the texture's colour format: RGBA8 (DXGI R8G8B8A8) or BGRA8.
   */
  static std::shared_ptr<WindowsD3D11InputNode> create(ID3D11Texture2D* texture, uint32_t width, uint32_t height,
                                                       EffectPixelFormat format = EffectPixelFormat::RGBA8,
                                                       uint32_t queueDepth = 4);

  ~WindowsD3D11InputNode() override;

  /** Replaces the source texture. Takes a reference; the previous one is released. */
  EffectResult setTexture(ID3D11Texture2D* texture, uint32_t width, uint32_t height,
                          EffectPixelFormat format = EffectPixelFormat::RGBA8);

  /**
   * Publishes a decoded frame: its texture and source PTS as one atomic update.
   *
   * This is the preferred producer entry point when frames carry timestamps. `setTexture` + `setTimestamp`
   * remain available for callers that set the PTS once and then swap textures.
   */
  EffectResult submit(ID3D11Texture2D* texture, uint32_t width, uint32_t height, EffectPixelFormat format,
                      RenderTime timestamp, std::shared_ptr<void> lifetime = nullptr);

  /** The unified effect input entry point for a D3D11 texture view. */
  EffectResult submit(const PixelBufferView& view) override;

  /**
   * Sets the source presentation time carried with subsequent frames.
   *
   * D3D11 textures do not carry a timestamp, so a decoder/capture producer has to pass its PTS here. Until
   * this is called the node uses the engine clock.
   */
  void setTimestamp(RenderTime timestamp);

  bool hasFrame() const noexcept override;
  bool repeatLastFrame() const noexcept override;
  void setRepeatLastFrame(bool repeat) override;

  [[nodiscard]] uint32_t width() const noexcept;
  [[nodiscard]] uint32_t height() const noexcept;

  /** Whether the last produced frame was imported into Vulkan instead of copied through staging. */
  [[nodiscard]] bool isZeroCopy() const noexcept;

  /** Implementation detail; opaque to a client. */
  struct State;

private:
  WindowsD3D11InputNode(ID3D11Texture2D* texture, uint32_t width, uint32_t height, EffectPixelFormat format,
                        uint32_t queueDepth);

  std::shared_ptr<State> mState;
};

} // namespace coral

#endif // CORAL_WINDOWS_D3D11_INPUT_NODE_H
