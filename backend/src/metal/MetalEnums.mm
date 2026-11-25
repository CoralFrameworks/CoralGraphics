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

#include "private/backend/metal/MetalEnums.h"

namespace backend::metal {

MTLPixelFormat toMtlPixelFormat(const PixelFormat format) {
  switch (format) {
    case PixelFormat::Undefined:
      return MTLPixelFormatInvalid;

    case PixelFormat::R8_UNORM:
      return MTLPixelFormatR8Unorm;
    case PixelFormat::R8_SNORM:
      return MTLPixelFormatR8Snorm;
    case PixelFormat::R8_UINT:
      return MTLPixelFormatR8Uint;
    case PixelFormat::R8_SINT:
      return MTLPixelFormatR8Sint;

    case PixelFormat::R8G8_UNORM:
      return MTLPixelFormatRG8Unorm;
    case PixelFormat::R8G8_SNORM:
      return MTLPixelFormatRG8Snorm;
    case PixelFormat::R8G8_UINT:
      return MTLPixelFormatRG8Uint;
    case PixelFormat::R8G8_SINT:
      return MTLPixelFormatRG8Sint;

    case PixelFormat::R16_UNORM:
      return MTLPixelFormatR16Unorm;
    case PixelFormat::R16_SNORM:
      return MTLPixelFormatR16Snorm;
    case PixelFormat::R16_UINT:
      return MTLPixelFormatR16Uint;
    case PixelFormat::R16_SINT:
      return MTLPixelFormatR16Sint;
    case PixelFormat::R16_SFLOAT:
      return MTLPixelFormatR16Float;

    // Metal has no three-channel colour formats. A caller that asks for one is answered with Invalid here, and the
    // capability table then reports the format unsupported rather than substituting a four-channel format whose
    // sampled values and byte layout would silently differ.
    case PixelFormat::R8G8B8_UNORM:
    case PixelFormat::R8G8B8_SNORM:
    case PixelFormat::R8G8B8_SRGB:
    case PixelFormat::B8G8R8_UNORM:
    case PixelFormat::B8G8R8_SRGB:
      return MTLPixelFormatInvalid;

    case PixelFormat::R8G8B8A8_UNORM:
      return MTLPixelFormatRGBA8Unorm;
    case PixelFormat::R8G8B8A8_SNORM:
      return MTLPixelFormatRGBA8Snorm;
    case PixelFormat::R8G8B8A8_SRGB:
      return MTLPixelFormatRGBA8Unorm_sRGB;
    case PixelFormat::R8G8B8A8_UINT:
      return MTLPixelFormatRGBA8Uint;
    case PixelFormat::R8G8B8A8_SINT:
      return MTLPixelFormatRGBA8Sint;
    case PixelFormat::B8G8R8A8_UNORM:
      return MTLPixelFormatBGRA8Unorm;
    case PixelFormat::B8G8R8A8_SRGB:
      return MTLPixelFormatBGRA8Unorm_sRGB;

    case PixelFormat::R16G16_UNORM:
      return MTLPixelFormatRG16Unorm;
    case PixelFormat::R16G16_SNORM:
      return MTLPixelFormatRG16Snorm;
    case PixelFormat::R16G16_UINT:
      return MTLPixelFormatRG16Uint;
    case PixelFormat::R16G16_SINT:
      return MTLPixelFormatRG16Sint;
    case PixelFormat::R16G16_SFLOAT:
      return MTLPixelFormatRG16Float;

    case PixelFormat::R32_UINT:
      return MTLPixelFormatR32Uint;
    case PixelFormat::R32_SINT:
      return MTLPixelFormatR32Sint;
    case PixelFormat::R32_SFLOAT:
      return MTLPixelFormatR32Float;
    case PixelFormat::R10G10B10A2_UNORM:
      return MTLPixelFormatRGB10A2Unorm;
    case PixelFormat::R11G11B10_SFLOAT:
      return MTLPixelFormatRG11B10Float;

    case PixelFormat::R16G16B16A16_UNORM:
      return MTLPixelFormatRGBA16Unorm;
    case PixelFormat::R16G16B16A16_SNORM:
      return MTLPixelFormatRGBA16Snorm;
    case PixelFormat::R16G16B16A16_UINT:
      return MTLPixelFormatRGBA16Uint;
    case PixelFormat::R16G16B16A16_SINT:
      return MTLPixelFormatRGBA16Sint;
    case PixelFormat::R16G16B16A16_SFLOAT:
      return MTLPixelFormatRGBA16Float;

    case PixelFormat::R32G32_UINT:
      return MTLPixelFormatRG32Uint;
    case PixelFormat::R32G32_SINT:
      return MTLPixelFormatRG32Sint;
    case PixelFormat::R32G32_SFLOAT:
      return MTLPixelFormatRG32Float;

    // No RGB32 format exists in Metal; see the three-channel note above.
    case PixelFormat::R32G32B32_UINT:
    case PixelFormat::R32G32B32_SINT:
    case PixelFormat::R32G32B32_SFLOAT:
      return MTLPixelFormatInvalid;

    case PixelFormat::R32G32B32A32_UINT:
      return MTLPixelFormatRGBA32Uint;
    case PixelFormat::R32G32B32A32_SINT:
      return MTLPixelFormatRGBA32Sint;
    case PixelFormat::R32G32B32A32_SFLOAT:
      return MTLPixelFormatRGBA32Float;

    // Depth. `D24_UNORM_S8_UINT` is realised as 32-bit float depth plus stencil; see `toMtlDepthStencilPixelFormat`.
    case PixelFormat::D16_UNORM:
      return MTLPixelFormatDepth16Unorm;
    case PixelFormat::D24_UNORM_S8_UINT:
      return MTLPixelFormatDepth32Float_Stencil8;
    case PixelFormat::D32_SFLOAT:
      return MTLPixelFormatDepth32Float;
    case PixelFormat::D32_SFLOAT_S8_UINT:
      return MTLPixelFormatDepth32Float_Stencil8;

    case PixelFormat::BC1_RGBA_UNORM:
      return MTLPixelFormatBC1_RGBA;
    case PixelFormat::BC1_RGBA_SRGB:
      return MTLPixelFormatBC1_RGBA_sRGB;
    case PixelFormat::BC3_RGBA_UNORM:
      return MTLPixelFormatBC3_RGBA;
    case PixelFormat::BC3_RGBA_SRGB:
      return MTLPixelFormatBC3_RGBA_sRGB;
    case PixelFormat::BC4_R_UNORM:
      return MTLPixelFormatBC4_RUnorm;
    case PixelFormat::BC5_RG_UNORM:
      return MTLPixelFormatBC5_RGUnorm;
    case PixelFormat::BC7_RGBA_UNORM:
      return MTLPixelFormatBC7_RGBAUnorm;
    case PixelFormat::BC7_RGBA_SRGB:
      return MTLPixelFormatBC7_RGBAUnorm_sRGB;

    case PixelFormat::ETC2_RGB8_UNORM:
      return MTLPixelFormatETC2_RGB8;
    case PixelFormat::ETC2_RGB8_SRGB:
      return MTLPixelFormatETC2_RGB8_sRGB;
    // Metal's ETC2 support is RGB8 only; there is no RGBA8 ETC2 pixel format. A caller that asks for one is told the
    // format has no equivalent rather than being handed a two-channel-short decode.
    case PixelFormat::ETC2_RGBA8_UNORM:
    case PixelFormat::ETC2_RGBA8_SRGB:
      return MTLPixelFormatInvalid;

    case PixelFormat::ASTC_4x4_UNORM:
      return MTLPixelFormatASTC_4x4_LDR;
    case PixelFormat::ASTC_4x4_SRGB:
      return MTLPixelFormatASTC_4x4_sRGB;
    case PixelFormat::ASTC_8x8_UNORM:
      return MTLPixelFormatASTC_8x8_LDR;
    case PixelFormat::ASTC_8x8_SRGB:
      return MTLPixelFormatASTC_8x8_sRGB;
  }
  return MTLPixelFormatInvalid;
}

bool hasMtlPixelFormat(const PixelFormat format) {
  return toMtlPixelFormat(format) != MTLPixelFormatInvalid;
}

MTLPixelFormat toMtlDepthStencilPixelFormat(const PixelFormat format) {
  return toMtlPixelFormat(format);
}

MTLVertexFormat toMtlVertexFormat(const VertexFormat format) {
  switch (format) {
    case VertexFormat::Invalid:
      return MTLVertexFormatInvalid;
    case VertexFormat::Float:
      return MTLVertexFormatFloat;
    case VertexFormat::Float2:
      return MTLVertexFormatFloat2;
    case VertexFormat::Float3:
      return MTLVertexFormatFloat3;
    case VertexFormat::Float4:
      return MTLVertexFormatFloat4;
    case VertexFormat::UInt:
      return MTLVertexFormatUInt;
    case VertexFormat::UInt2:
      return MTLVertexFormatUInt2;
    case VertexFormat::UInt3:
      return MTLVertexFormatUInt3;
    case VertexFormat::UInt4:
      return MTLVertexFormatUInt4;
    case VertexFormat::SInt:
      return MTLVertexFormatInt;
    case VertexFormat::SInt2:
      return MTLVertexFormatInt2;
    case VertexFormat::SInt3:
      return MTLVertexFormatInt3;
    case VertexFormat::SInt4:
      return MTLVertexFormatInt4;
    case VertexFormat::UByte4Norm:
      return MTLVertexFormatUChar4Normalized;
    case VertexFormat::UByte4:
      return MTLVertexFormatUChar4;
    case VertexFormat::Byte4Norm:
      return MTLVertexFormatChar4Normalized;
    case VertexFormat::UByte2Norm:
      return MTLVertexFormatUChar2Normalized;
    case VertexFormat::Short2Norm:
      return MTLVertexFormatShort2Normalized;
    case VertexFormat::UShort2Norm:
      return MTLVertexFormatUShort2Normalized;
    case VertexFormat::Half2:
      return MTLVertexFormatHalf2;
    case VertexFormat::Half4:
      return MTLVertexFormatHalf4;
    case VertexFormat::UInt1010102Norm:
      return MTLVertexFormatUInt1010102Normalized;
  }
  return MTLVertexFormatInvalid;
}

MTLBlendFactor toMtlBlendFactor(const BlendFactor factor) {
  switch (factor) {
    case BlendFactor::Zero:
      return MTLBlendFactorZero;
    case BlendFactor::One:
      return MTLBlendFactorOne;
    case BlendFactor::SrcColor:
      return MTLBlendFactorSourceColor;
    case BlendFactor::OneMinusSrcColor:
      return MTLBlendFactorOneMinusSourceColor;
    case BlendFactor::DstColor:
      return MTLBlendFactorDestinationColor;
    case BlendFactor::OneMinusDstColor:
      return MTLBlendFactorOneMinusDestinationColor;
    case BlendFactor::SrcAlpha:
      return MTLBlendFactorSourceAlpha;
    case BlendFactor::OneMinusSrcAlpha:
      return MTLBlendFactorOneMinusSourceAlpha;
    case BlendFactor::DstAlpha:
      return MTLBlendFactorDestinationAlpha;
    case BlendFactor::OneMinusDstAlpha:
      return MTLBlendFactorOneMinusDestinationAlpha;
    case BlendFactor::ConstantColor:
      return MTLBlendFactorBlendColor;
    case BlendFactor::OneMinusConstantColor:
      return MTLBlendFactorOneMinusBlendColor;
    case BlendFactor::SrcAlphaSaturate:
      return MTLBlendFactorSourceAlphaSaturated;
    case BlendFactor::Src1Color:
      return MTLBlendFactorSource1Color;
    case BlendFactor::OneMinusSrc1Color:
      return MTLBlendFactorOneMinusSource1Color;
    case BlendFactor::Src1Alpha:
      return MTLBlendFactorSource1Alpha;
    case BlendFactor::OneMinusSrc1Alpha:
      return MTLBlendFactorOneMinusSource1Alpha;
  }
  return MTLBlendFactorOne;
}

MTLBlendOperation toMtlBlendOperation(const BlendOp op) {
  switch (op) {
    case BlendOp::Add:
      return MTLBlendOperationAdd;
    case BlendOp::Subtract:
      return MTLBlendOperationSubtract;
    case BlendOp::ReverseSubtract:
      return MTLBlendOperationReverseSubtract;
    case BlendOp::Min:
      return MTLBlendOperationMin;
    case BlendOp::Max:
      return MTLBlendOperationMax;
  }
  return MTLBlendOperationAdd;
}

MTLCompareFunction toMtlCompareFunction(const CompareOp op) {
  switch (op) {
    case CompareOp::Never:
      return MTLCompareFunctionNever;
    case CompareOp::Less:
      return MTLCompareFunctionLess;
    case CompareOp::Equal:
      return MTLCompareFunctionEqual;
    case CompareOp::LessEqual:
      return MTLCompareFunctionLessEqual;
    case CompareOp::Greater:
      return MTLCompareFunctionGreater;
    case CompareOp::NotEqual:
      return MTLCompareFunctionNotEqual;
    case CompareOp::GreaterEqual:
      return MTLCompareFunctionGreaterEqual;
    case CompareOp::Always:
      return MTLCompareFunctionAlways;
  }
  return MTLCompareFunctionAlways;
}

MTLStencilOperation toMtlStencilOperation(const StencilOp op) {
  switch (op) {
    case StencilOp::Keep:
      return MTLStencilOperationKeep;
    case StencilOp::Zero:
      return MTLStencilOperationZero;
    case StencilOp::Replace:
      return MTLStencilOperationReplace;
    case StencilOp::IncrementClamp:
      return MTLStencilOperationIncrementClamp;
    case StencilOp::DecrementClamp:
      return MTLStencilOperationDecrementClamp;
    case StencilOp::Invert:
      return MTLStencilOperationInvert;
    case StencilOp::IncrementWrap:
      return MTLStencilOperationIncrementWrap;
    case StencilOp::DecrementWrap:
      return MTLStencilOperationDecrementWrap;
  }
  return MTLStencilOperationKeep;
}

MTLPrimitiveType toMtlPrimitiveType(const PrimitiveTopology topology) {
  switch (topology) {
    case PrimitiveTopology::PointList:
      return MTLPrimitiveTypePoint;
    case PrimitiveTopology::LineList:
      return MTLPrimitiveTypeLine;
    case PrimitiveTopology::LineStrip:
      return MTLPrimitiveTypeLineStrip;
    case PrimitiveTopology::TriangleList:
      return MTLPrimitiveTypeTriangle;
    case PrimitiveTopology::TriangleStrip:
      return MTLPrimitiveTypeTriangleStrip;
    // Metal has no triangle fan and no patch primitive in this API's shape. Both are refused at pipeline creation
    // (fan is emulated there instead), so reaching here means the caller skipped validation.
    case PrimitiveTopology::TriangleFan:
      return MTLPrimitiveTypeTriangle;
    case PrimitiveTopology::PatchList:
      return MTLPrimitiveTypeTriangle;
  }
  return MTLPrimitiveTypeTriangle;
}

MTLIndexType toMtlIndexType(const IndexFormat format) {
  return format == IndexFormat::UInt16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
}

MTLLoadAction toMtlLoadAction(const LoadOp op) {
  switch (op) {
    case LoadOp::Load:
      return MTLLoadActionLoad;
    case LoadOp::Clear:
      return MTLLoadActionClear;
    case LoadOp::DontCare:
      return MTLLoadActionDontCare;
  }
  return MTLLoadActionClear;
}

MTLStoreAction toMtlStoreAction(const StoreOp op) {
  return op == StoreOp::Store ? MTLStoreActionStore : MTLStoreActionDontCare;
}

MTLCullMode toMtlCullMode(const CullMode mode) {
  switch (mode) {
    case CullMode::None:
      return MTLCullModeNone;
    case CullMode::Front:
      return MTLCullModeFront;
    case CullMode::Back:
      return MTLCullModeBack;
  }
  return MTLCullModeNone;
}

MTLColorWriteMask toMtlColorWriteMask(const uint8_t channels) {
  MTLColorWriteMask mask = MTLColorWriteMaskNone;
  if ((channels & 0x1u) != 0) mask |= MTLColorWriteMaskRed;
  if ((channels & 0x2u) != 0) mask |= MTLColorWriteMaskGreen;
  if ((channels & 0x4u) != 0) mask |= MTLColorWriteMaskBlue;
  if ((channels & 0x8u) != 0) mask |= MTLColorWriteMaskAlpha;
  // A zero mask means "the caller left the field alone", exactly as the Vulkan backend interprets it; writing nothing
  // would make a pipeline silently draw nothing, which is a far worse answer than writing red.
  return mask == MTLColorWriteMaskNone ? MTLColorWriteMaskRed : mask;
}

MTLTextureType toMtlTextureType(const TextureType type, const uint32_t sampleCount) {
  const bool multisample = sampleCount > 1;
  switch (type) {
    case TextureType::Texture1D:
      return MTLTextureType1D;
    case TextureType::Texture2D:
      return multisample ? MTLTextureType2DMultisample : MTLTextureType2D;
    case TextureType::Texture3D:
      return MTLTextureType3D;
    case TextureType::TextureCube:
      return MTLTextureTypeCube;
    case TextureType::Texture2DArray:
      return multisample ? MTLTextureType2DMultisampleArray : MTLTextureType2DArray;
    case TextureType::TextureCubeArray:
      return MTLTextureTypeCubeArray;
  }
  return MTLTextureType2D;
}

MTLTextureType toMtlTextureViewType(const TextureViewDimension dimension, const uint32_t arrayLayers) {
  switch (dimension) {
    case TextureViewDimension::Texture1D:
      return MTLTextureType1D;
    case TextureViewDimension::Texture2D:
      return arrayLayers > 1 ? MTLTextureType2DArray : MTLTextureType2D;
    case TextureViewDimension::Texture2DArray:
      return MTLTextureType2DArray;
    case TextureViewDimension::Texture3D:
      return MTLTextureType3D;
    case TextureViewDimension::Cube:
      return MTLTextureTypeCube;
    case TextureViewDimension::CubeArray:
      return MTLTextureTypeCubeArray;
  }
  return MTLTextureType2D;
}

MTLSamplerMinMagFilter toMtlMinMagFilter(const FilterMode mode) {
  return mode == FilterMode::Nearest ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
}

MTLSamplerMipFilter toMtlMipFilter(const MipmapFilterMode mode) {
  return mode == MipmapFilterMode::Nearest ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
}

MTLSamplerAddressMode toMtlAddressMode(const SamplerAddressMode mode) {
  switch (mode) {
    case SamplerAddressMode::ClampToEdge:
      return MTLSamplerAddressModeClampToEdge;
    case SamplerAddressMode::Repeat:
      return MTLSamplerAddressModeRepeat;
    case SamplerAddressMode::MirroredRepeat:
      return MTLSamplerAddressModeMirrorRepeat;
    case SamplerAddressMode::ClampToBorder:
      return MTLSamplerAddressModeClampToBorderColor;
  }
  return MTLSamplerAddressModeClampToEdge;
}

MTLSamplerBorderColor toMtlBorderColor(const SamplerBorderColor color) {
  switch (color) {
    case SamplerBorderColor::TransparentBlack:
      return MTLSamplerBorderColorTransparentBlack;
    case SamplerBorderColor::OpaqueBlack:
      return MTLSamplerBorderColorOpaqueBlack;
    case SamplerBorderColor::OpaqueWhite:
      return MTLSamplerBorderColorOpaqueWhite;
  }
  return MTLSamplerBorderColorTransparentBlack;
}

MTLWinding toMtlWinding(const FrontFace frontFace, const bool flipY) {
  // The shader flips Y for the RHI's Vulkan-style clip space; Metal's NDC is Y-up, so after the flip a triangle whose
  // authored winding is counter-clockwise appears clockwise. Inverting the declared face here is what keeps
  // `CullMode::Back` culling the same triangles it culls on Vulkan and OpenGL.
  const bool clockwise = frontFace == FrontFace::Clockwise;
  const bool effectiveClockwise = flipY ? !clockwise : clockwise;
  return effectiveClockwise ? MTLWindingClockwise : MTLWindingCounterClockwise;
}

bool isDepthFormat(const PixelFormat format) {
  return getPixelFormatInfo(format).depth;
}

bool isStencilFormat(const PixelFormat format) {
  return getPixelFormatInfo(format).stencil;
}

} // namespace backend::metal
