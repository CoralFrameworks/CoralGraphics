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

#ifndef CORAL_IMAGE_INPUT_NODE_H
#define CORAL_IMAGE_INPUT_NODE_H

#include <effect/EffectInputNode.h>
#include <effect/EffectTexture.h>

#include <cstdint>
#include <memory>

namespace coral {

/**
 * An input node whose frame is a CPU image.
 *
 * The image is uploaded once and repeated until another is set. That is the cheap path for a sticker, an
 * overlay, a still frame of an export, or - on Android - a canvas snapshot handed to the engine. Changing
 * the image costs one upload; nothing in the graph is rebuilt.
 */
class ImageInputNode final : public EffectInputNode {
public:
  /** @param width/height the size reported before an image is set; 0 is accepted and follows the image. */
  static std::shared_ptr<ImageInputNode> create(uint32_t width = 0, uint32_t height = 0);

  ~ImageInputNode() override;

  /** Sets the frame to produce from now on. The image is copied by reference; its pixels are immutable. */
  EffectResult setImage(std::shared_ptr<const EffectImage> image);

  bool hasFrame() const noexcept override;
  bool repeatLastFrame() const noexcept override;
  void setRepeatLastFrame(bool repeat) override;

  [[nodiscard]] uint32_t width() const noexcept;
  [[nodiscard]] uint32_t height() const noexcept;

  /** Implementation detail; opaque to a client. */
  struct State;

private:
  ImageInputNode(uint32_t width, uint32_t height);

  std::shared_ptr<State> mState;
};

} // namespace coral

#endif // CORAL_IMAGE_INPUT_NODE_H
