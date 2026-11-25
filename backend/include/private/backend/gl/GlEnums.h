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

#ifndef CORAL_BACKEND_GL_ENUMS_H
#define CORAL_BACKEND_GL_ENUMS_H

/**
 * @file GlEnums.h
 *
 * Translation from the RHI's vocabulary into OpenGL ES constants.
 *
 * These live in one header because both halves of the backend need them and they must agree: the
 * device uses them when it allocates storage and creates pipelines, the command buffer uses them when
 * it replays a recorded draw. A divergence between the two - a texture allocated as GL_RGBA8 but
 * written through GL_RGB, say - produces silent corruption rather than an error, so a single
 * definition is the only safe arrangement.
 *
 * Every function here is `constexpr` and header-only, so the compiler folds the whole translation
 * away and the replay loop stays a switch over constants.
 */

#include <backend/Types.h>

#include "private/backend/gl/GlEntryPoints.h"

#include <cstdint>

namespace backend {
namespace gl {

/** A texture format expressed as the (internal, format, type) triple the texel upload path needs. */
struct GLFormatDesc {
  /** Sized internal format used for allocation. 0 when the format has no ES equivalent. */
  GLenum internalFormat = 0;
  /** Client-side format used when uploading or reading back texels. */
  GLenum format = 0;
  GLenum type = 0;
  /** True when the format can be attached to a framebuffer. */
  bool renderable = false;
};

/**
 * Maps a PixelFormat to its OpenGL ES representation.
 *
 * Every internal format is a *sized* format, because glTexStorage2D - the immutable-allocation entry
 * point the backend prefers - rejects unsized formats. Formats with no ES 3.0 equivalent (the desktop
 * BCn family, and 3-channel float) return a zero internalFormat, which the caller treats as
 * unsupported; the capability table never advertises them, so reaching that case means a descriptor
 * bypassed validation.
 */
constexpr GLFormatDesc describeFormat(const PixelFormat format) {
  switch (format) {
    case PixelFormat::R8_UNORM:
      return { .internalFormat = GL_R8, .format = GL_RED, .type = GL_UNSIGNED_BYTE, .renderable = true };
    case PixelFormat::R8_SNORM:
      return { .internalFormat = GL_R8_SNORM, .format = GL_RED, .type = GL_BYTE, .renderable = false };
    case PixelFormat::R8_UINT:
      return { .internalFormat = GL_R8UI, .format = GL_RED_INTEGER, .type = GL_UNSIGNED_BYTE, .renderable = true };
    case PixelFormat::R8_SINT:
      return { .internalFormat = GL_R8I, .format = GL_RED_INTEGER, .type = GL_BYTE, .renderable = true };
    case PixelFormat::R8G8_UNORM:
      return { .internalFormat = GL_RG8, .format = GL_RG, .type = GL_UNSIGNED_BYTE, .renderable = true };
    case PixelFormat::R8G8_SNORM:
      return { .internalFormat = GL_RG8_SNORM, .format = GL_RG, .type = GL_BYTE, .renderable = false };
    case PixelFormat::R8G8_UINT:
      return { .internalFormat = GL_RG8UI, .format = GL_RG_INTEGER, .type = GL_UNSIGNED_BYTE, .renderable = true };
    case PixelFormat::R8G8_SINT:
      return { .internalFormat = GL_RG8I, .format = GL_RG_INTEGER, .type = GL_BYTE, .renderable = true };
    case PixelFormat::R16_UNORM:
      return { .internalFormat = GL_R16, .format = GL_RED, .type = GL_UNSIGNED_SHORT, .renderable = true };
    case PixelFormat::R16_SNORM:
      return { .internalFormat = GL_R16_SNORM, .format = GL_RED, .type = GL_SHORT, .renderable = false };
    case PixelFormat::R16_UINT:
      return { .internalFormat = GL_R16UI, .format = GL_RED_INTEGER, .type = GL_UNSIGNED_SHORT, .renderable = true };
    case PixelFormat::R16_SINT:
      return { .internalFormat = GL_R16I, .format = GL_RED_INTEGER, .type = GL_SHORT, .renderable = true };
    case PixelFormat::R16_SFLOAT:
      return { .internalFormat = GL_R16F, .format = GL_RED, .type = GL_HALF_FLOAT, .renderable = true };
    case PixelFormat::R8G8B8_UNORM:
      return { .internalFormat = GL_RGB8, .format = GL_RGB, .type = GL_UNSIGNED_BYTE, .renderable = true };
    case PixelFormat::R8G8B8_SNORM:
      return { .internalFormat = GL_RGB8_SNORM, .format = GL_RGB, .type = GL_BYTE, .renderable = false };
    case PixelFormat::R8G8B8_SRGB:
      return { .internalFormat = GL_SRGB8, .format = GL_RGB, .type = GL_UNSIGNED_BYTE, .renderable = true };
    case PixelFormat::B8G8R8_UNORM:
      return { .internalFormat = GL_RGB8, .format = GL_BGR, .type = GL_UNSIGNED_BYTE, .renderable = true };
    case PixelFormat::B8G8R8_SRGB:
      return { .internalFormat = GL_SRGB8, .format = GL_BGR, .type = GL_UNSIGNED_BYTE, .renderable = true };
    case PixelFormat::R8G8B8A8_UNORM:
      return { .internalFormat = GL_RGBA8, .format = GL_RGBA, .type = GL_UNSIGNED_BYTE, .renderable = true };
    case PixelFormat::R8G8B8A8_SNORM:
      return { .internalFormat = GL_RGBA8_SNORM, .format = GL_RGBA, .type = GL_BYTE, .renderable = false };
    case PixelFormat::R8G8B8A8_SRGB:
      return { .internalFormat = GL_SRGB8_ALPHA8, .format = GL_RGBA, .type = GL_UNSIGNED_BYTE, .renderable = true };
    case PixelFormat::R8G8B8A8_UINT:
      return { .internalFormat = GL_RGBA8UI, .format = GL_RGBA_INTEGER, .type = GL_UNSIGNED_BYTE, .renderable = true };
    case PixelFormat::R8G8B8A8_SINT:
      return { .internalFormat = GL_RGBA8I, .format = GL_RGBA_INTEGER, .type = GL_BYTE, .renderable = true };
    case PixelFormat::B8G8R8A8_UNORM:
      return { .internalFormat = GL_RGBA8, .format = GL_BGRA, .type = GL_UNSIGNED_BYTE, .renderable = true };
    case PixelFormat::B8G8R8A8_SRGB:
      return { .internalFormat = GL_SRGB8_ALPHA8, .format = GL_BGRA, .type = GL_UNSIGNED_BYTE, .renderable = true };
    case PixelFormat::R16G16_UNORM:
      return { .internalFormat = GL_RG16, .format = GL_RG, .type = GL_UNSIGNED_SHORT, .renderable = true };
    case PixelFormat::R16G16_SNORM:
      return { .internalFormat = GL_RG16_SNORM, .format = GL_RG, .type = GL_SHORT, .renderable = false };
    case PixelFormat::R16G16_UINT:
      return { .internalFormat = GL_RG16UI, .format = GL_RG_INTEGER, .type = GL_UNSIGNED_SHORT, .renderable = true };
    case PixelFormat::R16G16_SINT:
      return { .internalFormat = GL_RG16I, .format = GL_RG_INTEGER, .type = GL_SHORT, .renderable = true };
    case PixelFormat::R16G16_SFLOAT:
      return { .internalFormat = GL_RG16F, .format = GL_RG, .type = GL_HALF_FLOAT, .renderable = true };
    case PixelFormat::R32_UINT:
      return { .internalFormat = GL_R32UI, .format = GL_RED_INTEGER, .type = GL_UNSIGNED_INT, .renderable = true };
    case PixelFormat::R32_SINT:
      return { .internalFormat = GL_R32I, .format = GL_RED_INTEGER, .type = GL_INT, .renderable = true };
    case PixelFormat::R32_SFLOAT:
      return { .internalFormat = GL_R32F, .format = GL_RED, .type = GL_FLOAT, .renderable = true };
    case PixelFormat::R10G10B10A2_UNORM:
      return { .internalFormat = GL_RGB10_A2,
        .format = GL_RGBA,
        .type = GL_UNSIGNED_INT_2_10_10_10_REV,
        .renderable = true };
    case PixelFormat::R11G11B10_SFLOAT:
      return { .internalFormat = GL_R11F_G11F_B10F, .format = GL_RGB, .type = GL_FLOAT, .renderable = true };
    case PixelFormat::R16G16B16A16_UNORM:
      return { .internalFormat = GL_RGBA16, .format = GL_RGBA, .type = GL_UNSIGNED_SHORT, .renderable = true };
    case PixelFormat::R16G16B16A16_SNORM:
      return { .internalFormat = GL_RGBA16_SNORM, .format = GL_RGBA, .type = GL_SHORT, .renderable = false };
    case PixelFormat::R16G16B16A16_UINT:
      return { .internalFormat = GL_RGBA16UI,
        .format = GL_RGBA_INTEGER,
        .type = GL_UNSIGNED_SHORT,
        .renderable = true };
    case PixelFormat::R16G16B16A16_SINT:
      return { .internalFormat = GL_RGBA16I, .format = GL_RGBA_INTEGER, .type = GL_SHORT, .renderable = true };
    case PixelFormat::R16G16B16A16_SFLOAT:
      return { .internalFormat = GL_RGBA16F, .format = GL_RGBA, .type = GL_HALF_FLOAT, .renderable = true };
    case PixelFormat::R32G32_UINT:
      return { .internalFormat = GL_RG32UI, .format = GL_RG_INTEGER, .type = GL_UNSIGNED_INT, .renderable = true };
    case PixelFormat::R32G32_SINT:
      return { .internalFormat = GL_RG32I, .format = GL_RG_INTEGER, .type = GL_INT, .renderable = true };
    case PixelFormat::R32G32_SFLOAT:
      return { .internalFormat = GL_RG32F, .format = GL_RG, .type = GL_FLOAT, .renderable = true };
    case PixelFormat::R32G32B32_UINT:
      return { .internalFormat = GL_RGB32UI, .format = GL_RGB_INTEGER, .type = GL_UNSIGNED_INT, .renderable = false };
    case PixelFormat::R32G32B32_SINT:
      return { .internalFormat = GL_RGB32I, .format = GL_RGB_INTEGER, .type = GL_INT, .renderable = false };
    case PixelFormat::R32G32B32_SFLOAT:
      return { .internalFormat = GL_RGB32F, .format = GL_RGB, .type = GL_FLOAT, .renderable = false };
    case PixelFormat::R32G32B32A32_UINT:
      return { .internalFormat = GL_RGBA32UI, .format = GL_RGBA_INTEGER, .type = GL_UNSIGNED_INT, .renderable = true };
    case PixelFormat::R32G32B32A32_SINT:
      return { .internalFormat = GL_RGBA32I, .format = GL_RGBA_INTEGER, .type = GL_INT, .renderable = true };
    case PixelFormat::R32G32B32A32_SFLOAT:
      return { .internalFormat = GL_RGBA32F, .format = GL_RGBA, .type = GL_FLOAT, .renderable = true };
    case PixelFormat::D16_UNORM:
      return { .internalFormat = GL_DEPTH_COMPONENT16,
        .format = GL_DEPTH_COMPONENT,
        .type = GL_UNSIGNED_SHORT,
        .renderable = true };
    case PixelFormat::D24_UNORM_S8_UINT:
      return { .internalFormat = GL_DEPTH24_STENCIL8,
        .format = GL_DEPTH_STENCIL,
        .type = GL_UNSIGNED_INT_24_8,
        .renderable = true };
    case PixelFormat::D32_SFLOAT:
      return { .internalFormat = GL_DEPTH_COMPONENT32F,
        .format = GL_DEPTH_COMPONENT,
        .type = GL_FLOAT,
        .renderable = true };
    case PixelFormat::D32_SFLOAT_S8_UINT:
      return { .internalFormat = GL_DEPTH32F_STENCIL8,
        .format = GL_DEPTH_STENCIL,
        .type = GL_FLOAT_32_UNSIGNED_INT_24_8_REV,
        .renderable = true };
    // Compressed formats have no client-side format: they are uploaded as raw blocks, so `format`
    // and `type` stay zero and the upload path uses glCompressedTexSubImage*.
    case PixelFormat::ETC2_RGB8_UNORM:
      return { .internalFormat = GL_COMPRESSED_RGB8_ETC2, .format = 0, .type = 0, .renderable = false };
    case PixelFormat::ETC2_RGB8_SRGB:
      return { .internalFormat = GL_COMPRESSED_SRGB8_ETC2, .format = 0, .type = 0, .renderable = false };
    case PixelFormat::ETC2_RGBA8_UNORM:
      return { .internalFormat = GL_COMPRESSED_RGBA8_ETC2_EAC, .format = 0, .type = 0, .renderable = false };
    case PixelFormat::ETC2_RGBA8_SRGB:
      return { .internalFormat = GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC, .format = 0, .type = 0, .renderable = false };
    case PixelFormat::ASTC_4x4_UNORM:
      return { .internalFormat = GL_COMPRESSED_RGBA_ASTC_4x4_KHR, .format = 0, .type = 0, .renderable = false };
    case PixelFormat::ASTC_4x4_SRGB:
      return { .internalFormat = GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR, .format = 0, .type = 0, .renderable = false };
    case PixelFormat::ASTC_8x8_UNORM:
      return { .internalFormat = GL_COMPRESSED_RGBA_ASTC_8x8_KHR, .format = 0, .type = 0, .renderable = false };
    case PixelFormat::ASTC_8x8_SRGB:
      return { .internalFormat = GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x8_KHR, .format = 0, .type = 0, .renderable = false };
    // The desktop BCn family. The capability probe only reports these on a desktop dialect with the
    // S3TC extension, so reaching one of these cases on an ES context means validation was bypassed.
    case PixelFormat::BC1_RGBA_UNORM:
      return { .internalFormat = GL_COMPRESSED_RGB_S3TC_DXT1_EXT, .format = 0, .type = 0, .renderable = false };
    case PixelFormat::BC1_RGBA_SRGB:
      return { .internalFormat = GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT, .format = 0, .type = 0, .renderable = false };
    case PixelFormat::BC3_RGBA_UNORM:
      return { .internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, .format = 0, .type = 0, .renderable = false };
    case PixelFormat::BC3_RGBA_SRGB:
      return { .internalFormat = GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT, .format = 0, .type = 0, .renderable = false };
    // BC4, BC5 and BC7 need GL_ARB_texture_compression_rgtc / _bptc rather than plain S3TC. They are
    // deliberately left without an internal format until that extension is probed, so the backend
    // refuses them instead of handing the driver a format it may not know.
    default:
      return { .internalFormat = 0, .format = 0, .type = 0, .renderable = false };
  }
}

/** Maps an RHI texture type to its GL target. */
constexpr GLenum toGLTarget(const TextureType type) {
  switch (type) {
    case TextureType::Texture1D:
      return GL_TEXTURE_1D;
    case TextureType::Texture2D:
      return GL_TEXTURE_2D;
    case TextureType::Texture3D:
      return GL_TEXTURE_3D;
    case TextureType::TextureCube:
      return GL_TEXTURE_CUBE_MAP;
    case TextureType::Texture2DArray:
      return GL_TEXTURE_2D_ARRAY;
    case TextureType::TextureCubeArray:
      return GL_TEXTURE_CUBE_MAP_ARRAY;
  }
  return GL_TEXTURE_2D;
}

/** Maps a view dimension to the GL target its sampler bindings must use. */
constexpr GLenum toGLTarget(const TextureViewDimension dimension) {
  switch (dimension) {
    case TextureViewDimension::Texture1D:
      return GL_TEXTURE_1D;
    case TextureViewDimension::Texture2D:
      return GL_TEXTURE_2D;
    case TextureViewDimension::Texture2DArray:
      return GL_TEXTURE_2D_ARRAY;
    case TextureViewDimension::Texture3D:
      return GL_TEXTURE_3D;
    case TextureViewDimension::Cube:
      return GL_TEXTURE_CUBE_MAP;
    case TextureViewDimension::CubeArray:
      return GL_TEXTURE_CUBE_MAP_ARRAY;
  }
  return GL_TEXTURE_2D;
}

/** The (size, type, normalised) triple glVertexAttribPointer wants. */
struct GLVertexFormat {
  GLint componentCount = 0;
  GLenum type = GL_FLOAT;
  GLboolean normalized = GL_FALSE;
};

/**
 * Maps a vertex format for glVertexAttribPointer.
 *
 * Integer formats are deliberately absent: they must go through glVertexAttribIPointer instead, and a
 * caller that reaches here with one has made a mistake the `componentCount == 0` result surfaces.
 */
constexpr GLVertexFormat toGLVertexFormat(const VertexFormat format) {
  switch (format) {
    case VertexFormat::Float:
      return { .componentCount = 1, .type = GL_FLOAT, .normalized = GL_FALSE };
    case VertexFormat::Float2:
      return { .componentCount = 2, .type = GL_FLOAT, .normalized = GL_FALSE };
    case VertexFormat::Float3:
      return { .componentCount = 3, .type = GL_FLOAT, .normalized = GL_FALSE };
    case VertexFormat::Float4:
      return { .componentCount = 4, .type = GL_FLOAT, .normalized = GL_FALSE };
    case VertexFormat::UByte4Norm:
      return { .componentCount = 4, .type = GL_UNSIGNED_BYTE, .normalized = GL_TRUE };
    case VertexFormat::Byte4Norm:
      return { .componentCount = 4, .type = GL_BYTE, .normalized = GL_TRUE };
    case VertexFormat::UByte2Norm:
      return { .componentCount = 2, .type = GL_UNSIGNED_BYTE, .normalized = GL_TRUE };
    case VertexFormat::Short2Norm:
      return { .componentCount = 2, .type = GL_SHORT, .normalized = GL_TRUE };
    case VertexFormat::UShort2Norm:
      return { .componentCount = 2, .type = GL_UNSIGNED_SHORT, .normalized = GL_TRUE };
    case VertexFormat::Half2:
      return { .componentCount = 2, .type = GL_HALF_FLOAT, .normalized = GL_FALSE };
    case VertexFormat::Half4:
      return { .componentCount = 4, .type = GL_HALF_FLOAT, .normalized = GL_FALSE };
    case VertexFormat::UInt1010102Norm:
      return { .componentCount = 4, .type = GL_UNSIGNED_INT_2_10_10_10_REV, .normalized = GL_TRUE };
    default:
      return { .componentCount = 0, .type = GL_FLOAT, .normalized = GL_FALSE };
  }
}

constexpr GLenum toGLTopology(const PrimitiveTopology topology) {
  switch (topology) {
    case PrimitiveTopology::PointList:
      return GL_POINTS;
    case PrimitiveTopology::LineList:
      return GL_LINES;
    case PrimitiveTopology::LineStrip:
      return GL_LINE_STRIP;
    case PrimitiveTopology::TriangleList:
      return GL_TRIANGLES;
    case PrimitiveTopology::TriangleStrip:
      return GL_TRIANGLE_STRIP;
    case PrimitiveTopology::TriangleFan:
      return GL_TRIANGLE_FAN;
    case PrimitiveTopology::PatchList:
      return GL_PATCHES;
  }
  return GL_TRIANGLES;
}

constexpr GLenum toGLCompare(const CompareOp operation) {
  switch (operation) {
    case CompareOp::Never:
      return GL_NEVER;
    case CompareOp::Less:
      return GL_LESS;
    case CompareOp::Equal:
      return GL_EQUAL;
    case CompareOp::LessEqual:
      return GL_LEQUAL;
    case CompareOp::Greater:
      return GL_GREATER;
    case CompareOp::NotEqual:
      return GL_NOTEQUAL;
    case CompareOp::GreaterEqual:
      return GL_GEQUAL;
    case CompareOp::Always:
      return GL_ALWAYS;
  }
  return GL_ALWAYS;
}

constexpr GLenum toGLStencilOp(const StencilOp operation) {
  switch (operation) {
    case StencilOp::Keep:
      return GL_KEEP;
    case StencilOp::Zero:
      return GL_ZERO;
    case StencilOp::Replace:
      return GL_REPLACE;
    case StencilOp::IncrementClamp:
      return GL_INCR;
    case StencilOp::DecrementClamp:
      return GL_DECR;
    case StencilOp::Invert:
      return GL_INVERT;
    case StencilOp::IncrementWrap:
      return GL_INCR_WRAP;
    case StencilOp::DecrementWrap:
      return GL_DECR_WRAP;
  }
  return GL_KEEP;
}

constexpr GLenum toGLBlendFactor(const BlendFactor factor) {
  switch (factor) {
    case BlendFactor::Zero:
      return GL_ZERO;
    case BlendFactor::One:
      return GL_ONE;
    case BlendFactor::SrcColor:
      return GL_SRC_COLOR;
    case BlendFactor::OneMinusSrcColor:
      return GL_ONE_MINUS_SRC_COLOR;
    case BlendFactor::DstColor:
      return GL_DST_COLOR;
    case BlendFactor::OneMinusDstColor:
      return GL_ONE_MINUS_DST_COLOR;
    case BlendFactor::SrcAlpha:
      return GL_SRC_ALPHA;
    case BlendFactor::OneMinusSrcAlpha:
      return GL_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DstAlpha:
      return GL_DST_ALPHA;
    case BlendFactor::OneMinusDstAlpha:
      return GL_ONE_MINUS_DST_ALPHA;
    case BlendFactor::ConstantColor:
      return GL_CONSTANT_COLOR;
    case BlendFactor::OneMinusConstantColor:
      return GL_ONE_MINUS_CONSTANT_COLOR;
    case BlendFactor::SrcAlphaSaturate:
      return GL_SRC_ALPHA_SATURATE;
    case BlendFactor::Src1Color:
      return GL_SRC1_COLOR;
    case BlendFactor::OneMinusSrc1Color:
      return GL_ONE_MINUS_SRC1_COLOR;
    case BlendFactor::Src1Alpha:
      return GL_SRC1_ALPHA;
    case BlendFactor::OneMinusSrc1Alpha:
      return GL_ONE_MINUS_SRC1_ALPHA;
  }
  return GL_ONE;
}

constexpr GLenum toGLBlendOp(const BlendOp operation) {
  switch (operation) {
    case BlendOp::Add:
      return GL_FUNC_ADD;
    case BlendOp::Subtract:
      return GL_FUNC_SUBTRACT;
    case BlendOp::ReverseSubtract:
      return GL_FUNC_REVERSE_SUBTRACT;
    case BlendOp::Min:
      return GL_MIN;
    case BlendOp::Max:
      return GL_MAX;
  }
  return GL_FUNC_ADD;
}

constexpr GLenum toGLAddressMode(const SamplerAddressMode mode) {
  switch (mode) {
    case SamplerAddressMode::ClampToEdge:
      return GL_CLAMP_TO_EDGE;
    case SamplerAddressMode::Repeat:
      return GL_REPEAT;
    case SamplerAddressMode::MirroredRepeat:
      return GL_MIRRORED_REPEAT;
    case SamplerAddressMode::ClampToBorder:
      return GL_CLAMP_TO_BORDER;
  }
  return GL_CLAMP_TO_EDGE;
}

/**
 * Chooses the GL minification filter.
 *
 * A single-level texture must not use a mipmapping filter: GL then considers it incomplete and
 * samples black, which is the single most common "why is my texture black" bug. The mip level count
 * is therefore part of the decision rather than something the caller has to remember.
 */
constexpr GLenum toGLMinFilter(const FilterMode minFilter, const MipmapFilterMode mipmapFilter,
        const uint32_t mipLevelCount) {
  if (mipLevelCount <= 1) {
    return minFilter == FilterMode::Nearest ? GL_NEAREST : GL_LINEAR;
  }
  switch (mipmapFilter) {
    case MipmapFilterMode::Nearest:
      return minFilter == FilterMode::Nearest ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_NEAREST;
    case MipmapFilterMode::Linear:
      return minFilter == FilterMode::Nearest ? GL_NEAREST_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_LINEAR;
  }
  return GL_LINEAR;
}

constexpr GLenum toGLShaderType(const ShaderStage stage) {
  switch (stage) {
    case ShaderStage::Vertex:
      return GL_VERTEX_SHADER;
    case ShaderStage::Fragment:
      return GL_FRAGMENT_SHADER;
    case ShaderStage::Compute:
      return GL_COMPUTE_SHADER;
    case ShaderStage::Geometry:
      return GL_GEOMETRY_SHADER;
    case ShaderStage::TessellationControl:
      return GL_TESS_CONTROL_SHADER;
    case ShaderStage::TessellationEvaluation:
      return GL_TESS_EVALUATION_SHADER;
  }
  return GL_VERTEX_SHADER;
}

/** GL_INVALID_INDEX, spelled out because it is not in the ES 3.0 constant subset we declare. */
inline constexpr GLuint kInvalidIndex = 0xFFFFFFFFu;

} // namespace gl
} // namespace backend

#endif // CORAL_BACKEND_GL_ENUMS_H
