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
 * @file test_PublicHeaders.cpp
 *
 * The public value types, exercised without an engine or a device. These cases are the contract a client
 * depends on before it has attached anything: time conversion, typed values, image copying and the
 * discriminating accessors of a texture source.
 *
 * The *compilability* of each header on its own is checked by the `effect_header_check` target, which
 * generates one translation unit per installed header.
 */

#include <effect/EffectEngine.h>
#include <effect/EffectFrame.h>
#include <effect/EffectInputNode.h>
#include <effect/EffectNode.h>
#include <effect/EffectOutputNode.h>
#include <effect/EffectPassNode.h>
#include <effect/EffectShader.h>
#include <effect/EffectTexture.h>
#include <effect/EffectTypes.h>
#include <effect/Fence.h>

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <vector>

namespace {

TEST(EffectTypes, TimeConversionsUseIntegerTicks) {
  const coral::RenderTime twoTicksAt60 = coral::RenderTime::fromTicks(2, 60);
  EXPECT_EQ(twoTicksAt60.toTicks(60), 2);
  EXPECT_EQ(twoTicksAt60.toTicks(30), 1);

  const coral::RenderTime oneTickAt30 { 1, 30 };
  EXPECT_EQ(oneTickAt30.toTicks(60), 2);

  // A 29.97-ish time base rounds to nearest rather than truncating.
  const coral::RenderTime oneTickAt1000 { 1, 1000 };
  EXPECT_EQ(oneTickAt1000.toTicks(60), 0);
  const coral::RenderTime thirtyThreeAt1000 { 33, 1000 };
  EXPECT_EQ(thirtyThreeAt1000.toTicks(60), 2);

  EXPECT_TRUE(twoTicksAt60.isValid());
  EXPECT_FALSE((coral::RenderTime { 1, 0 }).isValid());
}

TEST(EffectTypes, ValuesCarryTheirDeclaredType) {
  EXPECT_EQ(coral::EffectValue::fromFloat(1.0f).type(), coral::EffectValueType::Float);
  EXPECT_EQ(coral::EffectValue::fromInt(1).type(), coral::EffectValueType::Int);
  EXPECT_EQ(coral::EffectValue::fromVec2({ 1.0f, 2.0f }).type(), coral::EffectValueType::Vec2);
  EXPECT_EQ(coral::EffectValue::fromVec3({ 1.0f, 2.0f, 3.0f }).type(), coral::EffectValueType::Vec3);
  EXPECT_EQ(coral::EffectValue::fromVec4({ 1.0f, 2.0f, 3.0f, 4.0f }).type(), coral::EffectValueType::Vec4);
  EXPECT_EQ(coral::EffectValue::fromColor({ 1.0f, 0.5f, 0.25f, 1.0f }).type(), coral::EffectValueType::Color);
  EXPECT_EQ(coral::EffectValue::fromMatrix4(coral::EffectMatrix4 {}).type(), coral::EffectValueType::Matrix4);

  EXPECT_EQ(coral::effectValueSlotCount(coral::EffectValueType::Matrix4), 4u);
  EXPECT_EQ(coral::effectValueSlotCount(coral::EffectValueType::Float), 1u);
}

TEST(EffectTexture, AnImageOwnsACopyOfItsPixels) {
  std::vector<uint8_t> pixels(4 * 2 * 4, 0);
  for (size_t index = 0; index < pixels.size(); ++index) pixels[index] = static_cast<uint8_t>(index);

  auto image = coral::EffectImage::create(coral::EffectImageDesc::rgba8(pixels.data(), 4, 2));
  ASSERT_NE(image, nullptr);
  EXPECT_EQ(image->width(), 4u);
  EXPECT_EQ(image->height(), 2u);
  EXPECT_EQ(image->format(), coral::EffectPixelFormat::RGBA8);
  EXPECT_EQ(image->rowBytes(), 16u);
  EXPECT_EQ(image->byteSize(), 32u);

  // `create` copies, so mutating the caller's buffer afterwards must not change the image.
  std::memset(pixels.data(), 0xCD, pixels.size());
  const auto* stored = static_cast<const uint8_t*>(image->pixels());
  ASSERT_NE(stored, nullptr);
  for (size_t index = 0; index < 32; ++index) EXPECT_EQ(stored[index], static_cast<uint8_t>(index));
}

TEST(EffectTexture, InvalidImageDescriptionsAreRefused) {
  uint8_t pixel[4] = { 1, 2, 3, 4 };
  EXPECT_EQ(coral::EffectImage::create(coral::EffectImageDesc::rgba8(nullptr, 1, 1)), nullptr);
  EXPECT_EQ(coral::EffectImage::create(coral::EffectImageDesc::rgba8(pixel, 0, 1)), nullptr);
  EXPECT_EQ(coral::EffectImage::create(coral::EffectImageDesc::rgba8(pixel, 1, 1, 3)), nullptr);
}

TEST(EffectTexture, TextureSourcesDiscriminateTheirKind) {
  EXPECT_EQ(coral::TextureSource::empty().kind(), coral::TextureSource::Kind::Empty);
  EXPECT_EQ(coral::TextureSource::solid({ 1.0f, 0.0f, 0.0f, 1.0f }).kind(), coral::TextureSource::Kind::Solid);

  auto image = coral::EffectImage::create(coral::EffectImageDesc::rgba8(std::vector<uint8_t>(4, 0xFF).data(), 1, 1));
  ASSERT_NE(image, nullptr);
  EXPECT_EQ(coral::TextureSource::fromImage(image).kind(), coral::TextureSource::Kind::Image);

  EXPECT_EQ(coral::TextureSource::fromFrame(coral::EffectFrame {}).kind(), coral::TextureSource::Kind::Empty);
}

TEST(EffectFrame, ADefaultFrameIsInvalid) {
  const coral::EffectFrame frame;
  EXPECT_FALSE(frame.isValid());
  EXPECT_EQ(frame.id(), 0u);
  EXPECT_EQ(frame.width(), 0u);
  EXPECT_EQ(frame.height(), 0u);
}

TEST(EffectNode, KindsAndDefaultPortCountsAreAvailableWithoutAnEngine) {
  struct Input final : coral::EffectInputNode {
    bool hasFrame() const noexcept override { return false; }
    bool repeatLastFrame() const noexcept override { return true; }
    void setRepeatLastFrame(bool) override {}
  };
  struct Output final : coral::EffectOutputNode {
    bool isReady() const noexcept override { return true; }
    coral::EffectResult configure(const coral::EffectOutputConfig&) override { return coral::EffectResult::Success; }
  };

  const auto input = std::make_shared<Input>();
  const auto output = std::make_shared<Output>();
  EXPECT_EQ(input->kind(), coral::EffectNodeKind::Input);
  EXPECT_EQ(input->inputCount(), 0u);
  EXPECT_EQ(output->kind(), coral::EffectNodeKind::Output);
  EXPECT_EQ(output->inputCount(), 1u);
}

} // namespace anonymous
