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
 * @file ImageInputNode.cpp
 *
 * The static-image input: one upload per image change, then the same texture every frame.
 */

#include <effect/ImageInputNode.h>

#include "EffectInternal.h"
#include "EffectRenderCore.h"

#include <mutex>
#include <utility>

namespace coral {

struct ImageInputNode::State {
  mutable std::mutex mutex;
  uint32_t width = 0;
  uint32_t height = 0;
  bool repeat = true;
  uint64_t version = 0;
  std::shared_ptr<const EffectImage> image;
};

namespace {

class ImageRuntime final : public coral::detail::InputRuntime {
public:
  explicit ImageRuntime(std::shared_ptr<ImageInputNode::State> state) : mState(std::move(state)) {}

  bool produce(coral::detail::FrameContext& context, coral::detail::FrameView& view, EffectFrame& metadata,
               std::string& error) override {
    std::shared_ptr<const EffectImage> image;
    uint64_t version = 0;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      image = mState->image;
      version = mState->version;
    }
    if (image == nullptr) {
      error = "the image input has no image";
      return false;
    }
    // Resolve every frame so the image cache's frame-age eviction never leaves this node holding a
    // destroyed texture; it also stamps the entry as used.
    mTexture = context.core->textureForImage(image, error);
    if (!mTexture.valid()) return false;
    mUploadedVersion = version;
    view.texture = mTexture;
    view.width = image->width();
    view.height = image->height();
    view.format = EffectPixelFormat::RGBA8;
    view.colorSpace = EffectColorSpace::SRGB;
    view.timestamp = context.timestamp;
    view.external = false;
    metadata = coral::detail::FrameAccess::make(detail::allocateFrameId(context), view.width, view.height, EffectPixelFormat::RGBA8,
                                                EffectColorSpace::SRGB, context.timestamp);
    return true;
  }

  void releaseResources() override { mTexture = backend::TextureHandle::invalid(); }

private:
  std::shared_ptr<ImageInputNode::State> mState;
  backend::TextureHandle mTexture = backend::TextureHandle::invalid();
  uint64_t mUploadedVersion = 0;
};

} // namespace

ImageInputNode::ImageInputNode(const uint32_t width, const uint32_t height)
        : EffectInputNode(),
          mState(std::make_shared<State>()) {
  mState->width = width;
  mState->height = height;
  setInputRuntime(std::make_shared<ImageRuntime>(mState));
}

ImageInputNode::~ImageInputNode() = default;

std::shared_ptr<ImageInputNode> ImageInputNode::create(const uint32_t width, const uint32_t height) {
  return std::shared_ptr<ImageInputNode>(new ImageInputNode(width, height));
}

EffectResult ImageInputNode::setImage(std::shared_ptr<const EffectImage> image) {
  if (image == nullptr) return EffectResult::InvalidArgument;
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->image = std::move(image);
  mState->width = mState->image->width();
  mState->height = mState->image->height();
  ++mState->version;
  return EffectResult::Success;
}

bool ImageInputNode::hasFrame() const noexcept { return true; }

bool ImageInputNode::repeatLastFrame() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->repeat;
}

void ImageInputNode::setRepeatLastFrame(const bool repeat) {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->repeat = repeat;
}

uint32_t ImageInputNode::width() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->width;
}

uint32_t ImageInputNode::height() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->height;
}

} // namespace coral
