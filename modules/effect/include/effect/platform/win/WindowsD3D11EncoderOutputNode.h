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

#ifndef CORAL_WINDOWS_D3D11_ENCODER_OUTPUT_NODE_H
#define CORAL_WINDOWS_D3D11_ENCODER_OUTPUT_NODE_H

#include <effect/EffectOutputNode.h>

#include <cstdint>
#include <memory>

// The public header only names these through pointers; the implementation includes d3d11.h.
struct ID3D11Device;
struct ID3D11Texture2D;

namespace coral {

/**
 * Writes the graph's final frame into a Direct3D 11 texture an encoder can consume directly.
 *
 * The engine renders on its own backend - Vulkan by default on Windows, OpenGL only when the
 * application asks for it. The output node therefore does not try to make the engine render into a
 * D3D11 texture. Instead it creates a shared D3D11 texture, imports that texture into the running
 * backend as a writeback target, and records a GPU copy from the final pooled frame into it. No pixels
 * cross the CPU boundary: the Vulkan copy and the D3D11 encoder read the same memory.
 *
 * This is why the node needs the Vulkan backend: the OpenGL backend has no equivalent import for a
 * D3D11 shared texture and reports `Unsupported`. A caller that gets `nullptr` from `create()` or a
 * failing `start()` should fall back to `ReadbackOutputNode`.
 *
 * ## Synchronisation
 *
 * The copy is recorded into the frame's command buffer, so it completes when the frame's `Fence` is
 * signalled. The encoder must observe that fence before it reads the texture; the exporter does this
 * by waiting for the frame fence returned by `EffectEngine::execute()`.
 *
 * ## Lifetime
 *
 * The node owns the D3D11 texture. The pointer returned by `texture()` is borrowed and stays valid
 * until the node is destroyed; the encoder may keep its own reference.
 */
class WindowsD3D11EncoderOutputNode final : public EffectOutputNode {
public:
  /**
   * @param d3d11Device an `ID3D11Device*` the encoder also uses. The node takes its own reference.
   * @param width       output width in pixels; must not be zero.
   * @param height      output height in pixels; must not be zero.
   * @return the node, or nullptr when the device is missing or the texture cannot be created.
   */
  static std::shared_ptr<WindowsD3D11EncoderOutputNode> create(void* d3d11Device, uint32_t width,
    uint32_t height);

  ~WindowsD3D11EncoderOutputNode() override;

  [[nodiscard]] bool isReady() const noexcept override;

  EffectResult configure(const EffectOutputConfig& config) override;

  /** The number of textures the node cycles through. */
  static constexpr uint32_t kTextureCount = 3;

  /**
   * The BGRA8 texture in slot @p index.
   *
   * Each is created with `D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX`
   * and bound for render-target and shader reads, which is what both the Vulkan import and a
   * hardware encoder need. The encoder can still hold a texture after `consume()` returns, so the
   * exporter must read the slot reported by `lastTextureIndex()` and never assume slot 0.
   */
  [[nodiscard]] ID3D11Texture2D* texture(uint32_t index) const noexcept;

  /** The texture of slot 0; kept for callers that only need one. */
  [[nodiscard]] ID3D11Texture2D* texture() const noexcept;

  /** The slot the last completed `consume()` wrote. */
  [[nodiscard]] uint32_t lastTextureIndex() const noexcept;

  [[nodiscard]] uint32_t width() const noexcept;
  [[nodiscard]] uint32_t height() const noexcept;

  /** True when the backend imported the texture and the copy is on the GPU. */
  [[nodiscard]] bool isZeroCopy() const noexcept;

  /** Implementation detail; opaque to a client. */
  struct State;

private:
  WindowsD3D11EncoderOutputNode(void* d3d11Device, uint32_t width, uint32_t height);

  std::shared_ptr<State> mState;
};

} // namespace coral

#endif // CORAL_WINDOWS_D3D11_ENCODER_OUTPUT_NODE_H

