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
 * @file TestPatternInputNode.cpp
 *
 * The deterministic core input. The pattern is generated on the CPU, uploaded once, and then repeated -
 * the cheapest possible input, which is exactly what a pixel test wants.
 */

#include <effect/TestPatternInputNode.h>

#include "EffectInternal.h"
#include "EffectRenderCore.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace coral {

namespace {

/** @return the RGBA8 pixels of @p pattern at the requested size. */
std::vector<uint8_t> generatePixels(const uint32_t width, const uint32_t height,
                                    const TestPatternInputNode::Pattern pattern) {
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4, 0);
  static constexpr uint8_t kBarColors[8][3] = {
    { 255, 255, 255 }, // white
    { 255, 255, 0 },   // yellow
    { 0, 255, 255 },   // cyan
    { 0, 255, 0 },     // green
    { 255, 0, 255 },   // magenta
    { 255, 0, 0 },     // red
    { 0, 0, 255 },     // blue
    { 0, 0, 0 },       // black
  };
  const uint32_t square = std::max(1u, std::min(width, height) / 8u);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      uint8_t* pixel = pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
      if (pattern == TestPatternInputNode::Pattern::Checkerboard) {
        const bool alternate = ((x / square) + (y / square)) % 2 != 0;
        const uint8_t value = alternate ? 235 : 20;
        pixel[0] = value;
        pixel[1] = value;
        pixel[2] = value;
      } else {
        const uint32_t bar = width != 0 ? std::min(7u, x * 8u / width) : 0u;
        pixel[0] = kBarColors[bar][0];
        pixel[1] = kBarColors[bar][1];
        pixel[2] = kBarColors[bar][2];
      }
      pixel[3] = 255;
    }
  }
  return pixels;
}

} // namespace anonymous

struct TestPatternInputNode::State {
  mutable std::mutex mutex;
  uint32_t width = 0;
  uint32_t height = 0;
  Pattern pattern = Pattern::ColorBars;
  bool repeat = true;
  uint64_t version = 0;
  std::shared_ptr<const EffectImage> image;
};

namespace {

class TestPatternRuntime final : public coral::detail::InputRuntime {
public:
  explicit TestPatternRuntime(std::shared_ptr<TestPatternInputNode::State> state) : mState(std::move(state)) {}

  bool produce(coral::detail::FrameContext& context, coral::detail::FrameView& view, EffectFrame& metadata,
               std::string& error) override {
    std::shared_ptr<const EffectImage> image;
    uint64_t version = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      image = mState->image;
      version = mState->version;
      width = mState->width;
      height = mState->height;
    }
    if (image == nullptr) {
      error = "the test pattern has no image";
      return false;
    }
    // Resolve every frame: the image cache evicts textures that no producer touches for a full
    // submission ring, so a cached handle must be refreshed (and its last-used frame stamped).
    mTexture = context.core->textureForImage(image, error);
    if (!mTexture.valid()) return false;
    mUploadedVersion = version;
    view.texture = mTexture;
    view.width = width;
    view.height = height;
    view.format = EffectPixelFormat::RGBA8;
    view.colorSpace = EffectColorSpace::SRGB;
    view.timestamp = context.timestamp;
    view.external = true;
    metadata = coral::detail::FrameAccess::make(detail::allocateFrameId(context), width, height, EffectPixelFormat::RGBA8,
                                                EffectColorSpace::SRGB, context.timestamp);
    return true;
  }

  void releaseResources() override { mTexture = backend::TextureHandle::invalid(); }

private:
  std::shared_ptr<TestPatternInputNode::State> mState;
  backend::TextureHandle mTexture = backend::TextureHandle::invalid();
  uint64_t mUploadedVersion = 0;
};

} // namespace anonymous

TestPatternInputNode::TestPatternInputNode(const uint32_t width, const uint32_t height, const Pattern pattern)
        : EffectInputNode(),
          mState(std::make_shared<State>()) {
  mState->width = width;
  mState->height = height;
  mState->pattern = pattern;

  const std::vector<uint8_t> pixels = generatePixels(width, height, pattern);
  mState->image = EffectImage::create(EffectImageDesc::rgba8(pixels.data(), width, height));
  mState->version = 1;
  setInputRuntime(std::make_shared<TestPatternRuntime>(mState));
}

TestPatternInputNode::~TestPatternInputNode() = default;

std::shared_ptr<TestPatternInputNode> TestPatternInputNode::create(const uint32_t width, const uint32_t height,
                                                                    const Pattern pattern) {
  if (width == 0 || height == 0) return nullptr;
  return std::shared_ptr<TestPatternInputNode>(new TestPatternInputNode(width, height, pattern));
}

bool TestPatternInputNode::hasFrame() const noexcept { return true; }

bool TestPatternInputNode::repeatLastFrame() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->repeat;
}

void TestPatternInputNode::setRepeatLastFrame(const bool repeat) {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->repeat = repeat;
}

TestPatternInputNode::Pattern TestPatternInputNode::pattern() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->pattern;
}

void TestPatternInputNode::setPattern(const Pattern pattern) {
  uint32_t width = 0;
  uint32_t height = 0;
  {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    if (mState->pattern == pattern) return;
    mState->pattern = pattern;
    width = mState->width;
    height = mState->height;
    const std::vector<uint8_t> pixels = generatePixels(width, height, pattern);
    mState->image = EffectImage::create(EffectImageDesc::rgba8(pixels.data(), width, height));
    ++mState->version;
  }
}

uint32_t TestPatternInputNode::width() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->width;
}

uint32_t TestPatternInputNode::height() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->height;
}

void TestPatternInputNode::invalidate() {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  ++mState->version;
}

} // namespace coral
