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

#include "private/backend/vulkan/VulkanEnums.h"

namespace backend::vulkan {

VkFormat toVkFormat(const PixelFormat format) noexcept {
  switch (format) {
    case PixelFormat::Undefined:
      return VK_FORMAT_UNDEFINED;

    case PixelFormat::R8_UNORM:
      return VK_FORMAT_R8_UNORM;
    case PixelFormat::R8_SNORM:
      return VK_FORMAT_R8_SNORM;
    case PixelFormat::R8_UINT:
      return VK_FORMAT_R8_UINT;
    case PixelFormat::R8_SINT:
      return VK_FORMAT_R8_SINT;
    case PixelFormat::R8G8_UNORM:
      return VK_FORMAT_R8G8_UNORM;
    case PixelFormat::R8G8_SNORM:
      return VK_FORMAT_R8G8_SNORM;
    case PixelFormat::R8G8_UINT:
      return VK_FORMAT_R8G8_UINT;
    case PixelFormat::R8G8_SINT:
      return VK_FORMAT_R8G8_SINT;
    case PixelFormat::R16_UNORM:
      return VK_FORMAT_R16_UNORM;
    case PixelFormat::R16_SNORM:
      return VK_FORMAT_R16_SNORM;
    case PixelFormat::R16_UINT:
      return VK_FORMAT_R16_UINT;
    case PixelFormat::R16_SINT:
      return VK_FORMAT_R16_SINT;
    case PixelFormat::R16_SFLOAT:
      return VK_FORMAT_R16_SFLOAT;
    case PixelFormat::R8G8B8_UNORM:
      return VK_FORMAT_R8G8B8_UNORM;
    case PixelFormat::R8G8B8_SNORM:
      return VK_FORMAT_R8G8B8_SNORM;
    case PixelFormat::R8G8B8_SRGB:
      return VK_FORMAT_R8G8B8_SRGB;
    case PixelFormat::B8G8R8_UNORM:
      return VK_FORMAT_B8G8R8_UNORM;
    case PixelFormat::B8G8R8_SRGB:
      return VK_FORMAT_B8G8R8_SRGB;
    case PixelFormat::R8G8B8A8_UNORM:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case PixelFormat::R8G8B8A8_SNORM:
      return VK_FORMAT_R8G8B8A8_SNORM;
    case PixelFormat::R8G8B8A8_SRGB:
      return VK_FORMAT_R8G8B8A8_SRGB;
    case PixelFormat::R8G8B8A8_UINT:
      return VK_FORMAT_R8G8B8A8_UINT;
    case PixelFormat::R8G8B8A8_SINT:
      return VK_FORMAT_R8G8B8A8_SINT;
    case PixelFormat::B8G8R8A8_UNORM:
      return VK_FORMAT_B8G8R8A8_UNORM;
    case PixelFormat::B8G8R8A8_SRGB:
      return VK_FORMAT_B8G8R8A8_SRGB;
    case PixelFormat::R16G16_UNORM:
      return VK_FORMAT_R16G16_UNORM;
    case PixelFormat::R16G16_SNORM:
      return VK_FORMAT_R16G16_SNORM;
    case PixelFormat::R16G16_UINT:
      return VK_FORMAT_R16G16_UINT;
    case PixelFormat::R16G16_SINT:
      return VK_FORMAT_R16G16_SINT;
    case PixelFormat::R16G16_SFLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case PixelFormat::R32_UINT:
      return VK_FORMAT_R32_UINT;
    case PixelFormat::R32_SINT:
      return VK_FORMAT_R32_SINT;
    case PixelFormat::R32_SFLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case PixelFormat::R10G10B10A2_UNORM:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case PixelFormat::R11G11B10_SFLOAT:
      return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case PixelFormat::R16G16B16A16_UNORM:
      return VK_FORMAT_R16G16B16A16_UNORM;
    case PixelFormat::R16G16B16A16_SNORM:
      return VK_FORMAT_R16G16B16A16_SNORM;
    case PixelFormat::R16G16B16A16_UINT:
      return VK_FORMAT_R16G16B16A16_UINT;
    case PixelFormat::R16G16B16A16_SINT:
      return VK_FORMAT_R16G16B16A16_SINT;
    case PixelFormat::R16G16B16A16_SFLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case PixelFormat::R32G32_UINT:
      return VK_FORMAT_R32G32_UINT;
    case PixelFormat::R32G32_SINT:
      return VK_FORMAT_R32G32_SINT;
    case PixelFormat::R32G32_SFLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    case PixelFormat::R32G32B32_UINT:
      return VK_FORMAT_R32G32B32_UINT;
    case PixelFormat::R32G32B32_SINT:
      return VK_FORMAT_R32G32B32_SINT;
    case PixelFormat::R32G32B32_SFLOAT:
      return VK_FORMAT_R32G32B32_SFLOAT;
    case PixelFormat::R32G32B32A32_UINT:
      return VK_FORMAT_R32G32B32A32_UINT;
    case PixelFormat::R32G32B32A32_SINT:
      return VK_FORMAT_R32G32B32A32_SINT;
    case PixelFormat::R32G32B32A32_SFLOAT:
      return VK_FORMAT_R32G32B32A32_SFLOAT;

    case PixelFormat::D16_UNORM:
      return VK_FORMAT_D16_UNORM;
    case PixelFormat::D24_UNORM_S8_UINT:
      return VK_FORMAT_D24_UNORM_S8_UINT;
    case PixelFormat::D32_SFLOAT:
      return VK_FORMAT_D32_SFLOAT;
    case PixelFormat::D32_SFLOAT_S8_UINT:
      return VK_FORMAT_D32_SFLOAT_S8_UINT;

    case PixelFormat::BC1_RGBA_UNORM:
      return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case PixelFormat::BC1_RGBA_SRGB:
      return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case PixelFormat::BC3_RGBA_UNORM:
      return VK_FORMAT_BC3_UNORM_BLOCK;
    case PixelFormat::BC3_RGBA_SRGB:
      return VK_FORMAT_BC3_SRGB_BLOCK;
    case PixelFormat::BC4_R_UNORM:
      return VK_FORMAT_BC4_UNORM_BLOCK;
    case PixelFormat::BC5_RG_UNORM:
      return VK_FORMAT_BC5_UNORM_BLOCK;
    case PixelFormat::BC7_RGBA_UNORM:
      return VK_FORMAT_BC7_UNORM_BLOCK;
    case PixelFormat::BC7_RGBA_SRGB:
      return VK_FORMAT_BC7_SRGB_BLOCK;
    case PixelFormat::ETC2_RGB8_UNORM:
      return VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
    case PixelFormat::ETC2_RGB8_SRGB:
      return VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK;
    case PixelFormat::ETC2_RGBA8_UNORM:
      return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
    case PixelFormat::ETC2_RGBA8_SRGB:
      return VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK;
    case PixelFormat::ASTC_4x4_UNORM:
      return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    case PixelFormat::ASTC_4x4_SRGB:
      return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
    case PixelFormat::ASTC_8x8_UNORM:
      return VK_FORMAT_ASTC_8x8_UNORM_BLOCK;
    case PixelFormat::ASTC_8x8_SRGB:
      return VK_FORMAT_ASTC_8x8_SRGB_BLOCK;
  }
  return VK_FORMAT_UNDEFINED;
}

bool isDepthFormat(const PixelFormat format) noexcept { return getPixelFormatInfo(format).depth; }

bool isStencilFormat(const PixelFormat format) noexcept { return getPixelFormatInfo(format).stencil; }

bool isCompressed(const PixelFormat format) noexcept { return isCompressedFormat(format); }

VkImageAspectFlags aspectFor(const PixelFormat format) noexcept {
  const PixelFormatInfo& info = getPixelFormatInfo(format);
  if (info.depth && info.stencil) return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  if (info.depth) return VK_IMAGE_ASPECT_DEPTH_BIT;
  return VK_IMAGE_ASPECT_COLOR_BIT;
}

VkImageType toVkImageType(const TextureType type) noexcept {
  switch (type) {
    case TextureType::Texture1D:
      return VK_IMAGE_TYPE_1D;
    case TextureType::Texture3D:
      return VK_IMAGE_TYPE_3D;
    case TextureType::Texture2D:
    case TextureType::Texture2DArray:
    case TextureType::TextureCube:
    case TextureType::TextureCubeArray:
      return VK_IMAGE_TYPE_2D;
  }
  return VK_IMAGE_TYPE_2D;
}

VkImageViewType toVkImageViewType(const TextureViewDimension dimension) noexcept {
  switch (dimension) {
    case TextureViewDimension::Texture1D:
      return VK_IMAGE_VIEW_TYPE_1D;
    case TextureViewDimension::Texture2D:
      return VK_IMAGE_VIEW_TYPE_2D;
    case TextureViewDimension::Texture2DArray:
      return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case TextureViewDimension::Texture3D:
      return VK_IMAGE_VIEW_TYPE_3D;
    case TextureViewDimension::Cube:
      return VK_IMAGE_VIEW_TYPE_CUBE;
    case TextureViewDimension::CubeArray:
      return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
  }
  return VK_IMAGE_VIEW_TYPE_2D;
}

uint32_t layerCountFor(const TextureType type, const Extent3D& extent) noexcept {
  switch (type) {
    case TextureType::TextureCube:
      return 6;
    case TextureType::TextureCubeArray:
      return 6 * (extent.depthOrArrayLayers == 0 ? 1u : extent.depthOrArrayLayers);
    case TextureType::Texture2DArray:
    case TextureType::Texture3D:
      return extent.depthOrArrayLayers == 0 ? 1u : extent.depthOrArrayLayers;
    case TextureType::Texture1D:
    case TextureType::Texture2D:
      return 1;
  }
  return 1;
}

VkFormat toVkVertexFormat(const VertexFormat format) noexcept {
  switch (format) {
    case VertexFormat::Invalid:
      return VK_FORMAT_UNDEFINED;
    case VertexFormat::Float:
      return VK_FORMAT_R32_SFLOAT;
    case VertexFormat::Float2:
      return VK_FORMAT_R32G32_SFLOAT;
    case VertexFormat::Float3:
      return VK_FORMAT_R32G32B32_SFLOAT;
    case VertexFormat::Float4:
      return VK_FORMAT_R32G32B32A32_SFLOAT;
    case VertexFormat::UInt:
      return VK_FORMAT_R32_UINT;
    case VertexFormat::UInt2:
      return VK_FORMAT_R32G32_UINT;
    case VertexFormat::UInt3:
      return VK_FORMAT_R32G32B32_UINT;
    case VertexFormat::UInt4:
      return VK_FORMAT_R32G32B32A32_UINT;
    case VertexFormat::SInt:
      return VK_FORMAT_R32_SINT;
    case VertexFormat::SInt2:
      return VK_FORMAT_R32G32_SINT;
    case VertexFormat::SInt3:
      return VK_FORMAT_R32G32B32_SINT;
    case VertexFormat::SInt4:
      return VK_FORMAT_R32G32B32A32_SINT;
    case VertexFormat::UByte4Norm:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case VertexFormat::UByte4:
      return VK_FORMAT_R8G8B8A8_UINT;
    case VertexFormat::Byte4Norm:
      return VK_FORMAT_R8G8B8A8_SNORM;
    case VertexFormat::UByte2Norm:
      return VK_FORMAT_R8G8_UNORM;
    case VertexFormat::Short2Norm:
      return VK_FORMAT_R16G16_SNORM;
    case VertexFormat::UShort2Norm:
      return VK_FORMAT_R16G16_UNORM;
    case VertexFormat::Half2:
      return VK_FORMAT_R16G16_SFLOAT;
    case VertexFormat::Half4:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VertexFormat::UInt1010102Norm:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  }
  return VK_FORMAT_UNDEFINED;
}

VkShaderStageFlagBits toVkStage(const ShaderStage stage) noexcept {
  switch (stage) {
    case ShaderStage::Vertex:
      return VK_SHADER_STAGE_VERTEX_BIT;
    case ShaderStage::Fragment:
      return VK_SHADER_STAGE_FRAGMENT_BIT;
    case ShaderStage::Compute:
      return VK_SHADER_STAGE_COMPUTE_BIT;
    case ShaderStage::Geometry:
      return VK_SHADER_STAGE_GEOMETRY_BIT;
    case ShaderStage::TessellationControl:
      return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    case ShaderStage::TessellationEvaluation:
      return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
  }
  return VK_SHADER_STAGE_VERTEX_BIT;
}

VkShaderStageFlags allGraphicsStages() noexcept {
  return VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
         VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_GEOMETRY_BIT |
         VK_SHADER_STAGE_FRAGMENT_BIT;
}

VkShaderStageFlags toVkStageFlags(const uint8_t stageFlags) noexcept {
  if (stageFlags == 0) {
    // "Not stated" rather than "no stage": a descriptor that no stage could see is a binding the driver
    // is free to optimise away, and the silent-unbound-texture failure that follows is much worse than
    // making the binding visible to every graphics stage.
    return allGraphicsStages() | VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkShaderStageFlags flags = 0;
  if ((stageFlags & 0x01u) != 0) flags |= VK_SHADER_STAGE_VERTEX_BIT;
  if ((stageFlags & 0x02u) != 0) flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
  if ((stageFlags & 0x04u) != 0) flags |= VK_SHADER_STAGE_COMPUTE_BIT;
  if ((stageFlags & 0x08u) != 0) flags |= VK_SHADER_STAGE_GEOMETRY_BIT;
  if ((stageFlags & 0x10u) != 0) flags |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
  if ((stageFlags & 0x20u) != 0) flags |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
  return flags;
}

VkPrimitiveTopology toVkTopology(const PrimitiveTopology topology) noexcept {
  switch (topology) {
    case PrimitiveTopology::PointList:
      return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case PrimitiveTopology::LineList:
      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PrimitiveTopology::LineStrip:
      return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PrimitiveTopology::TriangleList:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PrimitiveTopology::TriangleStrip:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PrimitiveTopology::TriangleFan:
      // Vulkan has no fan. The pipeline is created as a list and the *draw* is translated into an
      // indexed list against a pre-filled fan index buffer; see VulkanCommandBuffer::draw.
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PrimitiveTopology::PatchList:
      return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
  }
  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkCullModeFlags toVkCullMode(const CullMode mode) noexcept {
  switch (mode) {
    case CullMode::None:
      return VK_CULL_MODE_NONE;
    case CullMode::Front:
      return VK_CULL_MODE_FRONT_BIT;
    case CullMode::Back:
      return VK_CULL_MODE_BACK_BIT;
  }
  return VK_CULL_MODE_NONE;
}

VkFrontFace toVkFrontFace(const FrontFace face) noexcept {
  return face == FrontFace::Clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
}

VkCompareOp toVkCompareOp(const CompareOp op) noexcept {
  switch (op) {
    case CompareOp::Never:
      return VK_COMPARE_OP_NEVER;
    case CompareOp::Less:
      return VK_COMPARE_OP_LESS;
    case CompareOp::Equal:
      return VK_COMPARE_OP_EQUAL;
    case CompareOp::LessEqual:
      return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareOp::Greater:
      return VK_COMPARE_OP_GREATER;
    case CompareOp::NotEqual:
      return VK_COMPARE_OP_NOT_EQUAL;
    case CompareOp::GreaterEqual:
      return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareOp::Always:
      return VK_COMPARE_OP_ALWAYS;
  }
  return VK_COMPARE_OP_ALWAYS;
}

VkStencilOp toVkStencilOp(const StencilOp op) noexcept {
  switch (op) {
    case StencilOp::Keep:
      return VK_STENCIL_OP_KEEP;
    case StencilOp::Zero:
      return VK_STENCIL_OP_ZERO;
    case StencilOp::Replace:
      return VK_STENCIL_OP_REPLACE;
    case StencilOp::IncrementClamp:
      return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case StencilOp::DecrementClamp:
      return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case StencilOp::Invert:
      return VK_STENCIL_OP_INVERT;
    case StencilOp::IncrementWrap:
      return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case StencilOp::DecrementWrap:
      return VK_STENCIL_OP_DECREMENT_AND_WRAP;
  }
  return VK_STENCIL_OP_KEEP;
}

VkBlendFactor toVkBlendFactor(const BlendFactor factor) noexcept {
  switch (factor) {
    case BlendFactor::Zero:
      return VK_BLEND_FACTOR_ZERO;
    case BlendFactor::One:
      return VK_BLEND_FACTOR_ONE;
    case BlendFactor::SrcColor:
      return VK_BLEND_FACTOR_SRC_COLOR;
    case BlendFactor::OneMinusSrcColor:
      return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case BlendFactor::DstColor:
      return VK_BLEND_FACTOR_DST_COLOR;
    case BlendFactor::OneMinusDstColor:
      return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case BlendFactor::SrcAlpha:
      return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::OneMinusSrcAlpha:
      return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DstAlpha:
      return VK_BLEND_FACTOR_DST_ALPHA;
    case BlendFactor::OneMinusDstAlpha:
      return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case BlendFactor::ConstantColor:
      return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case BlendFactor::OneMinusConstantColor:
      return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case BlendFactor::SrcAlphaSaturate:
      return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case BlendFactor::Src1Color:
      return VK_BLEND_FACTOR_SRC1_COLOR;
    case BlendFactor::OneMinusSrc1Color:
      return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case BlendFactor::Src1Alpha:
      return VK_BLEND_FACTOR_SRC1_ALPHA;
    case BlendFactor::OneMinusSrc1Alpha:
      return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
  }
  return VK_BLEND_FACTOR_ONE;
}

VkBlendOp toVkBlendOp(const BlendOp op) noexcept {
  switch (op) {
    case BlendOp::Add:
      return VK_BLEND_OP_ADD;
    case BlendOp::Subtract:
      return VK_BLEND_OP_SUBTRACT;
    case BlendOp::ReverseSubtract:
      return VK_BLEND_OP_REVERSE_SUBTRACT;
    case BlendOp::Min:
      return VK_BLEND_OP_MIN;
    case BlendOp::Max:
      return VK_BLEND_OP_MAX;
  }
  return VK_BLEND_OP_ADD;
}

VkFilter toVkFilter(const FilterMode mode) noexcept {
  return mode == FilterMode::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

VkSamplerMipmapMode toVkMipmapMode(const MipmapFilterMode mode) noexcept {
  return mode == MipmapFilterMode::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}

VkSamplerAddressMode toVkAddressMode(const SamplerAddressMode mode) noexcept {
  switch (mode) {
    case SamplerAddressMode::ClampToEdge:
      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case SamplerAddressMode::Repeat:
      return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case SamplerAddressMode::MirroredRepeat:
      return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case SamplerAddressMode::ClampToBorder:
      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  }
  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}

VkBorderColor toVkBorderColor(const SamplerBorderColor color) noexcept {
  switch (color) {
    case SamplerBorderColor::TransparentBlack:
      return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    case SamplerBorderColor::OpaqueBlack:
      return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    case SamplerBorderColor::OpaqueWhite:
      return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
  }
  return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
}

VkSamplerYcbcrModelConversion toVkYcbcrModel(const YcbcrModelConversion model) noexcept {
  switch (model) {
    case YcbcrModelConversion::Bt601:
      return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
    case YcbcrModelConversion::Bt709:
      return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    case YcbcrModelConversion::Bt2020:
      return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020;
    case YcbcrModelConversion::RgbIdentity:
      return VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
  }
  return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
}

VkSamplerYcbcrRange toVkYcbcrRange(const YcbcrRange range) noexcept {
  return range == YcbcrRange::Full ? VK_SAMPLER_YCBCR_RANGE_ITU_FULL : VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
}

VkChromaLocation toVkChromaLocation(const ChromaLocation location) noexcept {
  return location == ChromaLocation::Midpoint ? VK_CHROMA_LOCATION_MIDPOINT : VK_CHROMA_LOCATION_COSITED_EVEN;
}

VkIndexType toVkIndexType(const IndexFormat format) noexcept {
  return format == IndexFormat::UInt16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
}

VkQueryType toVkQueryType(const QueryType type) noexcept {
  switch (type) {
    case QueryType::Occlusion:
      return VK_QUERY_TYPE_OCCLUSION;
    case QueryType::Timestamp:
      return VK_QUERY_TYPE_TIMESTAMP;
    case QueryType::PipelineStatistics:
      return VK_QUERY_TYPE_PIPELINE_STATISTICS;
  }
  return VK_QUERY_TYPE_OCCLUSION;
}

VkDescriptorType toVkDescriptorType(const BindingType type) noexcept {
  switch (type) {
    case BindingType::TextureSampler:
      return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case BindingType::Sampler:
      return VK_DESCRIPTOR_TYPE_SAMPLER;
    case BindingType::Texture:
      return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case BindingType::UniformBuffer:
      return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case BindingType::ReadOnlyStorageBuffer:
    case BindingType::StorageBuffer:
      return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  }
  return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

VkColorComponentFlags toVkColorWriteMask(const uint8_t mask) noexcept {
  VkColorComponentFlags flags = 0;
  if ((mask & 0x01u) != 0) flags |= VK_COLOR_COMPONENT_R_BIT;
  if ((mask & 0x02u) != 0) flags |= VK_COLOR_COMPONENT_G_BIT;
  if ((mask & 0x04u) != 0) flags |= VK_COLOR_COMPONENT_B_BIT;
  if ((mask & 0x08u) != 0) flags |= VK_COLOR_COMPONENT_A_BIT;
  return flags;
}

VkAttachmentLoadOp toVkLoadOp(const LoadOp op) noexcept {
  switch (op) {
    case LoadOp::Load:
      return VK_ATTACHMENT_LOAD_OP_LOAD;
    case LoadOp::Clear:
      return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case LoadOp::DontCare:
      return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  }
  return VK_ATTACHMENT_LOAD_OP_LOAD;
}

VkAttachmentStoreOp toVkStoreOp(const StoreOp op) noexcept {
  return op == StoreOp::DontCare ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
}

} // namespace backend::vulkan

