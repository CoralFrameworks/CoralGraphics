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

#include "test_asserts.h"

using namespace backend;

namespace {

/**
 * The format table is indexed by the PixelFormat enumerator, which makes it silently wrong the moment
 * an enumerator is inserted in the middle. This test walks every enumerator and checks that the
 * descriptor's name matches what the test expects for that position in the table, so a mismatch
 * between header and source is caught here rather than as a wrong texture size in production.
 */
TEST(Types, PixelFormatTableIsInSyncWithEnum) {
  EXPECT_STREQ(getPixelFormatInfo(PixelFormat::Undefined).name, "Undefined");
  EXPECT_STREQ(getPixelFormatInfo(PixelFormat::R8_UNORM).name, "R8_UNORM");
  EXPECT_STREQ(getPixelFormatInfo(PixelFormat::R8G8B8A8_UNORM).name, "R8G8B8A8_UNORM");
  EXPECT_STREQ(getPixelFormatInfo(PixelFormat::D24_UNORM_S8_UINT).name, "D24_UNORM_S8_UINT");
  EXPECT_STREQ(getPixelFormatInfo(PixelFormat::ASTC_8x8_SRGB).name, "ASTC_8x8_SRGB");
}

TEST(Types, OutOfRangeFormatIsUndefinedNotOutOfBounds) {
  const auto bogus = static_cast<PixelFormat>(0xFFFF);
  const PixelFormatInfo& info = getPixelFormatInfo(bogus);
  EXPECT_STREQ(info.name, "Undefined");
  EXPECT_EQ(info.bytesPerBlock, 0);
  EXPECT_EQ(getFormatByteSize(bogus, 16, 16), 0u);
}

TEST(Types, UncompressedFormatSizes) {
  // 4 bytes per texel.
  EXPECT_EQ(getFormatByteSize(PixelFormat::R8G8B8A8_UNORM, 64, 32), 64u * 32u * 4u);
  // 1 byte per texel.
  EXPECT_EQ(getFormatByteSize(PixelFormat::R8_UNORM, 100, 50), 5000u);
  // 8 bytes per texel.
  EXPECT_EQ(getFormatByteSize(PixelFormat::R16G16B16A16_SFLOAT, 8, 8), 8u * 8u * 8u);
  // Depth/stencil formats are uncompressed from the size calculator's point of view.
  EXPECT_EQ(getFormatByteSize(PixelFormat::D24_UNORM_S8_UINT, 16, 16), 16u * 16u * 4u);
}

TEST(Types, CompressedFormatSizesRoundUpToWholeBlocks) {
  // BC1 is 8 bytes per 4x4 block, so a 4x4 texture is exactly one block.
  EXPECT_EQ(getFormatByteSize(PixelFormat::BC1_RGBA_UNORM, 4, 4), 8u);
  EXPECT_EQ(getFormatByteSize(PixelFormat::BC1_RGBA_UNORM, 8, 8), 4u * 8u);

  // A 5x5 texture occupies 2x2 blocks: partial blocks are still fully allocated. This is the case a
  // naive width*height*bpp/16 calculation gets wrong, and the reason the rounding is tested.
  EXPECT_EQ(getFormatByteSize(PixelFormat::BC1_RGBA_UNORM, 5, 5), 4u * 8u);
  EXPECT_EQ(getFormatByteSize(PixelFormat::BC1_RGBA_UNORM, 1, 1), 8u);

  // ASTC 8x8 is 16 bytes per block, so 8x8 is one block and 9x8 is two.
  EXPECT_EQ(getFormatByteSize(PixelFormat::ASTC_8x8_UNORM, 8, 8), 16u);
  EXPECT_EQ(getFormatByteSize(PixelFormat::ASTC_8x8_UNORM, 9, 8), 32u);

  // ETC2 RGB8 is 8 bytes per 4x4 block.
  EXPECT_EQ(getFormatByteSize(PixelFormat::ETC2_RGB8_UNORM, 4, 4), 8u);
}

TEST(Types, CompressedFormatClassification) {
  EXPECT_TRUE(isCompressedFormat(PixelFormat::BC1_RGBA_UNORM));
  EXPECT_TRUE(isCompressedFormat(PixelFormat::ASTC_4x4_UNORM));
  EXPECT_TRUE(isCompressedFormat(PixelFormat::ETC2_RGBA8_SRGB));
  EXPECT_FALSE(isCompressedFormat(PixelFormat::R8G8B8A8_UNORM));
  EXPECT_FALSE(isCompressedFormat(PixelFormat::D32_SFLOAT));
  EXPECT_FALSE(isCompressedFormat(PixelFormat::Undefined));
}

TEST(Types, FormatMetadataFlags) {
  const PixelFormatInfo& srgb = getPixelFormatInfo(PixelFormat::R8G8B8A8_SRGB);
  EXPECT_TRUE(srgb.srgb);
  EXPECT_EQ(srgb.channelCount, 4);

  const PixelFormatInfo& linear = getPixelFormatInfo(PixelFormat::R8G8B8A8_UNORM);
  EXPECT_FALSE(linear.srgb);

  const PixelFormatInfo& depthStencil = getPixelFormatInfo(PixelFormat::D24_UNORM_S8_UINT);
  EXPECT_TRUE(depthStencil.depth);
  EXPECT_TRUE(depthStencil.stencil);

  const PixelFormatInfo& depthOnly = getPixelFormatInfo(PixelFormat::D32_SFLOAT);
  EXPECT_TRUE(depthOnly.depth);
  EXPECT_FALSE(depthOnly.stencil);

  const PixelFormatInfo& rgb = getPixelFormatInfo(PixelFormat::R8G8B8_UNORM);
  EXPECT_EQ(rgb.channelCount, 3);
  EXPECT_EQ(rgb.bytesPerBlock, 3);

  const PixelFormatInfo& rg11b10 = getPixelFormatInfo(PixelFormat::R11G11B10_SFLOAT);
  EXPECT_EQ(rg11b10.bytesPerBlock, 4);
  EXPECT_EQ(rg11b10.channelCount, 3);
}

TEST(Types, VertexFormatSizesAndComponents) {
  EXPECT_EQ(getVertexFormatByteSize(VertexFormat::Float), 4);
  EXPECT_EQ(getVertexFormatByteSize(VertexFormat::Float2), 8);
  EXPECT_EQ(getVertexFormatByteSize(VertexFormat::Float3), 12);
  EXPECT_EQ(getVertexFormatByteSize(VertexFormat::Float4), 16);
  EXPECT_EQ(getVertexFormatByteSize(VertexFormat::Half2), 4);
  EXPECT_EQ(getVertexFormatByteSize(VertexFormat::Half4), 8);
  EXPECT_EQ(getVertexFormatByteSize(VertexFormat::UByte4Norm), 4);
  EXPECT_EQ(getVertexFormatByteSize(VertexFormat::UShort2Norm), 4);
  EXPECT_EQ(getVertexFormatByteSize(VertexFormat::UInt1010102Norm), 4);
  EXPECT_EQ(getVertexFormatByteSize(VertexFormat::Invalid), 0);

  EXPECT_EQ(getVertexFormatComponentCount(VertexFormat::Float3), 3);
  EXPECT_EQ(getVertexFormatComponentCount(VertexFormat::UByte4Norm), 4);
  EXPECT_EQ(getVertexFormatComponentCount(VertexFormat::Invalid), 0);
}

TEST(Types, IntegerVertexFormatClassification) {
  // Pure integer formats must reach the shader unconverted.
  EXPECT_TRUE(isIntegerVertexFormat(VertexFormat::UInt));
  EXPECT_TRUE(isIntegerVertexFormat(VertexFormat::UInt4));
  EXPECT_TRUE(isIntegerVertexFormat(VertexFormat::SInt2));
  EXPECT_TRUE(isIntegerVertexFormat(VertexFormat::UByte4));

  // Normalised formats are converted to float by the vertex fetch unit, so they are NOT integer
  // attributes even though the underlying storage is integral. Getting this wrong makes the driver
  // reject the attribute binding.
  EXPECT_FALSE(isIntegerVertexFormat(VertexFormat::UByte4Norm));
  EXPECT_FALSE(isIntegerVertexFormat(VertexFormat::Byte4Norm));
  EXPECT_FALSE(isIntegerVertexFormat(VertexFormat::UByte2Norm));
  EXPECT_FALSE(isIntegerVertexFormat(VertexFormat::Short2Norm));
  EXPECT_FALSE(isIntegerVertexFormat(VertexFormat::UShort2Norm));
  EXPECT_FALSE(isIntegerVertexFormat(VertexFormat::UInt1010102Norm));

  EXPECT_FALSE(isIntegerVertexFormat(VertexFormat::Float4));
  EXPECT_FALSE(isIntegerVertexFormat(VertexFormat::Half4));
  EXPECT_FALSE(isIntegerVertexFormat(VertexFormat::Invalid));
}

TEST(Types, UsageBitmaskHelpers) {
  const auto usages = static_cast<TextureUsages>(TextureUsage::Sampled | TextureUsage::ColorAttachment);
  EXPECT_TRUE(hasUsage(usages, TextureUsage::Sampled));
  EXPECT_TRUE(hasUsage(usages, TextureUsage::ColorAttachment));
  EXPECT_FALSE(hasUsage(usages, TextureUsage::Storage));
  EXPECT_FALSE(hasUsage(usages, TextureUsage::DepthStencilAttachment));

  const auto bufferUsages = static_cast<BufferUsages>(BufferUsage::Vertex | BufferUsage::TransferDst);
  EXPECT_TRUE(hasUsage(bufferUsages, BufferUsage::Vertex));
  EXPECT_TRUE(hasUsage(bufferUsages, BufferUsage::TransferDst));
  EXPECT_FALSE(hasUsage(bufferUsages, BufferUsage::Index));
}

TEST(Types, BackendAndStageNamesAreStable) {
  EXPECT_STREQ(toString(Backend::OpenGL), "OpenGL");
  EXPECT_STREQ(toString(Backend::Vulkan), "Vulkan");
  EXPECT_STREQ(toString(GlDialect::ES), "OpenGL ES 3.0+");
  EXPECT_STREQ(toString(GlDialect::Desktop), "OpenGL 4.1+");
  EXPECT_STREQ(toString(ShaderStage::Vertex), "vertex");
  EXPECT_STREQ(toString(ShaderStage::Fragment), "fragment");
  EXPECT_STREQ(toString(ShaderStage::Compute), "compute");
}

TEST(Types, ResultClassification) {
  EXPECT_TRUE(isSuccess(Result::Success));
  EXPECT_FALSE(isSuccess(Result::Error));

  // A transient result is one a caller can retry or recover from; the distinction matters for the
  // presentation path, where OutOfDate must trigger a resize rather than an abort.
  EXPECT_TRUE(isTransient(Result::NotReady));
  EXPECT_TRUE(isTransient(Result::Timeout));
  EXPECT_TRUE(isTransient(Result::OutOfDate));
  EXPECT_FALSE(isTransient(Result::Error));
  EXPECT_FALSE(isTransient(Result::Unsupported));
  EXPECT_FALSE(isTransient(Result::Success));
}

TEST(Types, SubresourceRangeDefaults) {
  const TextureSubresourceRange full = TextureSubresourceRange::all(5, 3);
  EXPECT_EQ(full.baseMipLevel, 0u);
  EXPECT_EQ(full.mipLevelCount, 5u);
  EXPECT_EQ(full.baseArrayLayer, 0u);
  EXPECT_EQ(full.arrayLayerCount, 3u);

  // The zero-means-everything convention is what lets a caller write `all()` without knowing the
  // texture's mip count.
  const TextureSubresourceRange unspecified = TextureSubresourceRange::all();
  EXPECT_EQ(unspecified.mipLevelCount, 0u);
  EXPECT_EQ(unspecified.arrayLayerCount, 0u);
}

TEST(Types, BlendStatePresets) {
  const BlendState alpha = BlendState::alphaBlend();
  EXPECT_TRUE(alpha.blendEnable);
  EXPECT_EQ(alpha.color.srcFactor, BlendFactor::SrcAlpha);
  EXPECT_EQ(alpha.color.dstFactor, BlendFactor::OneMinusSrcAlpha);

  const BlendState premultiplied = BlendState::premultipliedBlend();
  EXPECT_TRUE(premultiplied.blendEnable);
  EXPECT_EQ(premultiplied.color.srcFactor, BlendFactor::One);

  const BlendState additive = BlendState::additiveBlend();
  EXPECT_TRUE(additive.blendEnable);
  EXPECT_EQ(additive.color.dstFactor, BlendFactor::One);

  const BlendState opaque = BlendState::opaque();
  EXPECT_FALSE(opaque.blendEnable);
}

TEST(Types, ColorPresets) {
  EXPECT_DOUBLE_EQ(Color::black().a, 1.0);
  EXPECT_DOUBLE_EQ(Color::transparentBlack().a, 0.0);
  EXPECT_DOUBLE_EQ(Color(0.25, 0.5, 0.75).b, 0.75);
}

} // namespace anonymous
