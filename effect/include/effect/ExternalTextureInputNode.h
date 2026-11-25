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

#ifndef CORAL_EXTERNAL_TEXTURE_INPUT_NODE_H
#define CORAL_EXTERNAL_TEXTURE_INPUT_NODE_H

#include <effect/EffectInputNode.h>
#include <effect/EffectTypes.h>

#include <cstdint>
#include <memory>

namespace coral {

/**
 * An input node sampling a platform OpenGL texture by name, without a CPU copy.
 *
 * The name belongs to the caller and is only imported by the RHI; the node never deletes it. The caller
 * must keep the texture alive until the node is destroyed or a new texture is submitted, and must ensure
 * the contents are stable while the engine may sample them. The OpenGL backend imports it directly; a
 * Vulkan device answers `Unsupported`.
 *
 * This is the generic platform-external texture path; a camera/decoder can publish its own texture and
 * then feed this node.
 */
class ExternalTextureInputNode final : public EffectInputNode {
public:
  static std::shared_ptr<ExternalTextureInputNode> create(uint32_t width, uint32_t height,
                                                          EffectPixelFormat format = EffectPixelFormat::RGBA8);

  ~ExternalTextureInputNode() override;

  bool hasFrame() const noexcept override;
  bool repeatLastFrame() const noexcept override;
  void setRepeatLastFrame(bool repeat) override;

  [[nodiscard]] uint32_t width() const noexcept;
  [[nodiscard]] uint32_t height() const noexcept;

  /** @param glTexture the GL texture name; @param glTarget e.g. `GL_TEXTURE_2D`. */
  EffectResult setTexture(uint32_t glTexture, uint32_t glTarget, uint32_t width, uint32_t height,
                         EffectPixelFormat format = EffectPixelFormat::RGBA8);

  /** Replaces the texture and carries the producer's PTS atomically with it. */
  EffectResult submit(uint32_t glTexture, uint32_t glTarget, uint32_t width, uint32_t height,
                      EffectPixelFormat format, RenderTime timestamp);

  void setTimestamp(RenderTime timestamp);

  /** Implementation detail; opaque to a client. */
  struct State;

private:
  ExternalTextureInputNode(uint32_t width, uint32_t height, EffectPixelFormat format);
  std::shared_ptr<State> mState;
};

} // namespace coral

#endif // CORAL_EXTERNAL_TEXTURE_INPUT_NODE_H
