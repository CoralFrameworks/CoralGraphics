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

#include <backend/Types.h>

namespace backend {
namespace {

/**
 * Pixel format descriptors, indexed by the PixelFormat enumerator.
 *
 * `bytesPerBlock` is the size of one sampling block: the texel size for uncompressed formats and the
 * size of one 4x4 (or 8x8) block for the compressed ones. `getFormatByteSize` combines this with the
 * block dimensions, which is what lets callers size an upload buffer without a switch statement over
 * a hundred formats.
 *
 * Static assertions in Types.cpp verify that this table stays in sync with the enumeration.
 */
constexpr PixelFormatInfo kFormats[] = {
  // Descriptors are written with designated initialisers so that the row order can never
  // drift from the struct's declaration order: adding a field to PixelFormatInfo forces a
  // compile error on every row until it is given a value, instead of silently shifting.
  { .bytesPerBlock = 0,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 0,
      .depth = false,
      .stencil = false,
      .filterable = false,
      .srgb = false,
      .renderable = false,
      .name = "Undefined" },
  { .bytesPerBlock = 1,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 1,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "R8_UNORM" },
  { .bytesPerBlock = 1,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 1,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "R8_SNORM" },
  { .bytesPerBlock = 1,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 1,
      .depth = false,
      .stencil = false,
      .filterable = false,
      .srgb = false,
      .renderable = true,
      .name = "R8_UINT" },
  { .bytesPerBlock = 1,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 1,
      .depth = false,
      .stencil = false,
      .filterable = false,
      .srgb = false,
      .renderable = true,
      .name = "R8_SINT" },
  { .bytesPerBlock = 2,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 2,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "R8G8_UNORM" },
  { .bytesPerBlock = 2,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 2,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "R8G8_SNORM" },
  { .bytesPerBlock = 2,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 2,
      .depth = false,
      .stencil = false,
      .filterable = false,
      .srgb = false,
      .renderable = true,
      .name = "R8G8_UINT" },
  { .bytesPerBlock = 2,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 2,
      .depth = false,
      .stencil = false,
      .filterable = false,
      .srgb = false,
      .renderable = true,
      .name = "R8G8_SINT" },
  { .bytesPerBlock = 2,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 1,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "R16_UNORM" },
  { .bytesPerBlock = 2,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 1,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "R16_SNORM" },
  { .bytesPerBlock = 2,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 1,
      .depth = false,
      .stencil = false,
      .filterable = false,
      .srgb = false,
      .renderable = true,
      .name = "R16_UINT" },
  { .bytesPerBlock = 2,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 1,
      .depth = false,
      .stencil = false,
      .filterable = false,
      .srgb = false,
      .renderable = true,
      .name = "R16_SINT" },
  { .bytesPerBlock = 2,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 1,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "R16_SFLOAT" },
  { .bytesPerBlock = 3,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 3,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "R8G8B8_UNORM" },
  { .bytesPerBlock = 3,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 3,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "R8G8B8_SNORM" },
  { .bytesPerBlock = 3,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 3,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = true,
      .renderable = true,
      .name = "R8G8B8_SRGB" },
  { .bytesPerBlock = 3,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 3,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "B8G8R8_UNORM" },
  { .bytesPerBlock = 3,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 3,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = true,
      .renderable = true,
      .name = "B8G8R8_SRGB" },
  { .bytesPerBlock = 4,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "R8G8B8A8_UNORM" },
  { .bytesPerBlock = 4,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "R8G8B8A8_SNORM" },
  { .bytesPerBlock = 4,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = true,
      .renderable = true,
      .name = "R8G8B8A8_SRGB" },
  { .bytesPerBlock = 4,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = false,
      .srgb = false,
      .renderable = true,
      .name = "R8G8B8A8_UINT" },
  { .bytesPerBlock = 4,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = false,
      .srgb = false,
      .renderable = true,
      .name = "R8G8B8A8_SINT" },
  { .bytesPerBlock = 4,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "B8G8R8A8_UNORM" },
  { .bytesPerBlock = 4,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = true,
      .renderable = true,
      .name = "B8G8R8A8_SRGB" },
  { .bytesPerBlock = 4,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 2,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "R16G16_UNORM" },
  { .bytesPerBlock = 4,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 2,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "R16G16_SNORM" },
  { .bytesPerBlock = 4,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 2,
      .depth = false,
      .stencil = false,
      .filterable = false,
      .srgb = false,
      .renderable = true,
      .name = "R16G16_UINT" },
  { .bytesPerBlock = 4,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 2,
      .depth = false,
      .stencil = false,
      .filterable = false,
      .srgb = false,
      .renderable = true,
      .name = "R16G16_SINT" },
  { .bytesPerBlock = 4,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 2,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "R16G16_SFLOAT" },
  { .bytesPerBlock = 4,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 1,
      .depth = false,
      .stencil = false,
      .filterable = false,
      .srgb = false,
      .renderable = true,
      .name = "R32_UINT" },
  { .bytesPerBlock = 4,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 1,
      .depth = false,
      .stencil = false,
      .filterable = false,
      .srgb = false,
      .renderable = true,
      .name = "R32_SINT" },
  { .bytesPerBlock = 4,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 1,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "R32_SFLOAT" },
  { .bytesPerBlock = 4,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "R10G10B10A2_UNORM" },
  { .bytesPerBlock = 4,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 3,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "R11G11B10_SFLOAT" },
  { .bytesPerBlock = 8,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "R16G16B16A16_UNORM" },
  { .bytesPerBlock = 8,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "R16G16B16A16_SNORM" },
  { .bytesPerBlock = 8,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = false,
      .srgb = false,
      .renderable = true,
      .name = "R16G16B16A16_UINT" },
  { .bytesPerBlock = 8,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = false,
      .srgb = false,
      .renderable = true,
      .name = "R16G16B16A16_SINT" },
  { .bytesPerBlock = 8,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "R16G16B16A16_SFLOAT" },
  { .bytesPerBlock = 8,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 2,
      .depth = false,
      .stencil = false,
      .filterable = false,
      .srgb = false,
      .renderable = true,
      .name = "R32G32_UINT" },
  { .bytesPerBlock = 8,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 2,
      .depth = false,
      .stencil = false,
      .filterable = false,
      .srgb = false,
      .renderable = true,
      .name = "R32G32_SINT" },
  { .bytesPerBlock = 8,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 2,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "R32G32_SFLOAT" },
  { .bytesPerBlock = 12,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 3,
      .depth = false,
      .stencil = false,
      .filterable = false,
      .srgb = false,
      .renderable = true,
      .name = "R32G32B32_UINT" },
  { .bytesPerBlock = 12,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 3,
      .depth = false,
      .stencil = false,
      .filterable = false,
      .srgb = false,
      .renderable = true,
      .name = "R32G32B32_SINT" },
  { .bytesPerBlock = 12,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 3,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "R32G32B32_SFLOAT" },
  { .bytesPerBlock = 16,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = false,
      .srgb = false,
      .renderable = true,
      .name = "R32G32B32A32_UINT" },
  { .bytesPerBlock = 16,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = false,
      .srgb = false,
      .renderable = true,
      .name = "R32G32B32A32_SINT" },
  { .bytesPerBlock = 16,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "R32G32B32A32_SFLOAT" },
  { .bytesPerBlock = 2,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 1,
      .depth = true,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "D16_UNORM" },
  { .bytesPerBlock = 4,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 2,
      .depth = true,
      .stencil = true,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "D24_UNORM_S8_UINT" },
  { .bytesPerBlock = 4,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 1,
      .depth = true,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = true,
      .name = "D32_SFLOAT" },
  { .bytesPerBlock = 8,
      .blockWidth = 1,
      .blockHeight = 1,
      .channelCount = 2,
      .depth = true,
      .stencil = true,
      .filterable = false,
      .srgb = false,
      .renderable = true,
      .name = "D32_SFLOAT_S8_UINT" },
  { .bytesPerBlock = 8,
      .blockWidth = 4,
      .blockHeight = 4,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = false,
      .name = "BC1_RGBA_UNORM" },
  { .bytesPerBlock = 8,
      .blockWidth = 4,
      .blockHeight = 4,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = true,
      .renderable = false,
      .name = "BC1_RGBA_SRGB" },
  { .bytesPerBlock = 16,
      .blockWidth = 4,
      .blockHeight = 4,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = false,
      .name = "BC3_RGBA_UNORM" },
  { .bytesPerBlock = 16,
      .blockWidth = 4,
      .blockHeight = 4,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = true,
      .renderable = false,
      .name = "BC3_RGBA_SRGB" },
  { .bytesPerBlock = 8,
      .blockWidth = 4,
      .blockHeight = 4,
      .channelCount = 1,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = false,
      .name = "BC4_R_UNORM" },
  { .bytesPerBlock = 16,
      .blockWidth = 4,
      .blockHeight = 4,
      .channelCount = 2,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = false,
      .name = "BC5_RG_UNORM" },
  { .bytesPerBlock = 16,
      .blockWidth = 4,
      .blockHeight = 4,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = false,
      .name = "BC7_RGBA_UNORM" },
  { .bytesPerBlock = 16,
      .blockWidth = 4,
      .blockHeight = 4,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = true,
      .renderable = false,
      .name = "BC7_RGBA_SRGB" },
  { .bytesPerBlock = 8,
      .blockWidth = 4,
      .blockHeight = 4,
      .channelCount = 3,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = false,
      .name = "ETC2_RGB8_UNORM" },
  { .bytesPerBlock = 8,
      .blockWidth = 4,
      .blockHeight = 4,
      .channelCount = 3,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = true,
      .renderable = false,
      .name = "ETC2_RGB8_SRGB" },
  { .bytesPerBlock = 16,
      .blockWidth = 4,
      .blockHeight = 4,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = false,
      .name = "ETC2_RGBA8_UNORM" },
  { .bytesPerBlock = 16,
      .blockWidth = 4,
      .blockHeight = 4,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = true,
      .renderable = false,
      .name = "ETC2_RGBA8_SRGB" },
  { .bytesPerBlock = 16,
      .blockWidth = 4,
      .blockHeight = 4,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = false,
      .name = "ASTC_4x4_UNORM" },
  { .bytesPerBlock = 16,
      .blockWidth = 4,
      .blockHeight = 4,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = true,
      .renderable = false,
      .name = "ASTC_4x4_SRGB" },
  { .bytesPerBlock = 16,
      .blockWidth = 8,
      .blockHeight = 8,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = false,
      .renderable = false,
      .name = "ASTC_8x8_UNORM" },
  { .bytesPerBlock = 16,
      .blockWidth = 8,
      .blockHeight = 8,
      .channelCount = 4,
      .depth = false,
      .stencil = false,
      .filterable = true,
      .srgb = true,
      .renderable = false,
      .name = "ASTC_8x8_SRGB" },
};

constexpr size_t kFormatCount = sizeof(kFormats) / sizeof(kFormats[0]);

/**
 * Paranoia that keeps the table honest: if a PixelFormat enumerator is added without a matching
 * table row, the count assertion fires and every later lookup would silently be off by one.
 */
static_assert(kFormatCount == static_cast<size_t>(PixelFormat::ASTC_8x8_SRGB) + 1,
        "kFormats is out of sync with the PixelFormat enumeration - add the missing row");

/** Undefined descriptor returned for out-of-range enum values. */
constexpr PixelFormatInfo kUndefinedFormat = { .bytesPerBlock = 0,
  .blockWidth = 1,
  .blockHeight = 1,
  .channelCount = 0,
  .depth = false,
  .stencil = false,
  .filterable = false,
  .srgb = false,
  .renderable = false,
  .name = "Undefined" };

/** Vertex format descriptors: byte size, component count, integer-ness. */
struct VertexFormatInfo {
  uint8_t byteSize;
  uint8_t componentCount;
  bool integer;
};

constexpr VertexFormatInfo kVertexFormats[] = {
  // Invalid
  { .byteSize = 0, .componentCount = 0, .integer = false },
  // Float
  { .byteSize = 4, .componentCount = 1, .integer = false }, { .byteSize = 8, .componentCount = 2, .integer = false },
  { .byteSize = 12, .componentCount = 3, .integer = false }, { .byteSize = 16, .componentCount = 4, .integer = false },
  // UInt
  { .byteSize = 4, .componentCount = 1, .integer = true }, { .byteSize = 8, .componentCount = 2, .integer = true },
  { .byteSize = 12, .componentCount = 3, .integer = true }, { .byteSize = 16, .componentCount = 4, .integer = true },
  // SInt
  { .byteSize = 4, .componentCount = 1, .integer = true }, { .byteSize = 8, .componentCount = 2, .integer = true },
  { .byteSize = 12, .componentCount = 3, .integer = true }, { .byteSize = 16, .componentCount = 4, .integer = true },
  // Packed / normalised
  { .byteSize = 4, .componentCount = 4, .integer = true },  // UByte4Norm
  { .byteSize = 4, .componentCount = 4, .integer = true },  // UByte4
  { .byteSize = 4, .componentCount = 4, .integer = true },  // Byte4Norm
  { .byteSize = 2, .componentCount = 2, .integer = true },  // UByte2Norm
  { .byteSize = 4, .componentCount = 2, .integer = true },  // Short2Norm
  { .byteSize = 4, .componentCount = 2, .integer = true },  // UShort2Norm
  { .byteSize = 4, .componentCount = 2, .integer = false }, // Half2
  { .byteSize = 8, .componentCount = 4, .integer = false }, // Half4
  { .byteSize = 4, .componentCount = 4, .integer = true },  // UInt1010102Norm
};

constexpr size_t kVertexFormatCount = sizeof(kVertexFormats) / sizeof(kVertexFormats[0]);

static_assert(kVertexFormatCount == static_cast<size_t>(VertexFormat::UInt1010102Norm) + 1,
        "kVertexFormats is out of sync with the VertexFormat enumeration");

} // namespace anonymous

const char* toString(const Backend backend) noexcept {
  switch (backend) {
    case Backend::Null:
      return "Null";
    case Backend::OpenGL:
      return "OpenGL";
    case Backend::Vulkan:
      return "Vulkan";
    case Backend::Metal:
      return "Metal";
  }
  return "Unknown";
}

const char* toString(const ShaderSourceLanguage language) noexcept {
  switch (language) {
    case ShaderSourceLanguage::Detect:
      return "Detect";
    case ShaderSourceLanguage::ESSL100:
      return "ESSL 1.00";
    case ShaderSourceLanguage::ESSL300:
      return "ESSL 3.00";
    case ShaderSourceLanguage::GLSL410:
      return "GLSL 4.10 core";
  }
  return "Unknown shader source language";
}

const char* toString(const GlDialect dialect) noexcept {
  switch (dialect) {
    case GlDialect::ES:
      return "OpenGL ES 3.0+";
    case GlDialect::Desktop:
      return "OpenGL 4.1+";
  }
  return "Unknown OpenGL dialect";
}

GlDialect getPlatformGlDialect() noexcept {
  // Android and iOS have no desktop OpenGL, so their OpenGL backend is the ES flavour. Every other
  // platform Coral targets has a desktop GL 4.1 implementation.
  //
  // This is a compile-time decision on purpose: a renderer needs to know at build time which shader
  // version it will be authoring against, and a dialect that could change at run time would make
  // every capability question a run-time one too.
#if defined(CORAL_BUILD_FOR_ANDROID) || defined(CORAL_BUILD_FOR_IOS)
  return GlDialect::ES;
#else
  return GlDialect::Desktop;
#endif
}

const char* getPlatformGlDialectName() noexcept { return toString(getPlatformGlDialect()); }

const char* toString(const ShaderStage stage) noexcept {
  switch (stage) {
    case ShaderStage::Vertex:
      return "vertex";
    case ShaderStage::Fragment:
      return "fragment";
    case ShaderStage::Compute:
      return "compute";
    case ShaderStage::Geometry:
      return "geometry";
    case ShaderStage::TessellationControl:
      return "tessellation-control";
    case ShaderStage::TessellationEvaluation:
      return "tessellation-evaluation";
  }
  return "unknown";
}

const PixelFormatInfo& getPixelFormatInfo(PixelFormat format) noexcept {
  const auto index = static_cast<size_t>(format);
  if (index >= kFormatCount) {
    return kUndefinedFormat;
  }
  return kFormats[index];
}

bool isCompressedFormat(const PixelFormat format) noexcept {
  return getPixelFormatInfo(format).blockWidth > 1 || getPixelFormatInfo(format).blockHeight > 1;
}

size_t getFormatByteSize(const PixelFormat format, const uint32_t width, const uint32_t height,
        const uint32_t depth) noexcept {
  const PixelFormatInfo& info = getPixelFormatInfo(format);
  if (info.bytesPerBlock == 0) {
    return 0;
  }

  const uint32_t blockWidth = info.blockWidth;
  const uint32_t blockHeight = info.blockHeight;
  // Round up to whole blocks: a 5x5 BC1 texture occupies 2x2 blocks, not 1.25x1.25.
  const uint64_t blocksWide = (static_cast<uint64_t>(width) + blockWidth - 1) / blockWidth;
  const uint64_t blocksHigh = (static_cast<uint64_t>(height) + blockHeight - 1) / blockHeight;
  const uint64_t blockCount = blocksWide * blocksHigh * (depth == 0 ? 1u : depth);
  return blockCount * info.bytesPerBlock;
}

uint8_t getVertexFormatByteSize(VertexFormat format) noexcept {
  const auto index = static_cast<size_t>(format);
  if (index >= kVertexFormatCount) {
    return 0;
  }
  return kVertexFormats[index].byteSize;
}

uint8_t getVertexFormatComponentCount(VertexFormat format) noexcept {
  const auto index = static_cast<size_t>(format);
  if (index >= kVertexFormatCount) {
    return 0;
  }
  return kVertexFormats[index].componentCount;
}

bool isIntegerVertexFormat(VertexFormat format) noexcept {
  const auto index = static_cast<size_t>(format);
  if (index >= kVertexFormatCount) {
    return false;
  }
  switch (format) {
    // Normalised packed formats are converted to float by the vertex fetch unit, so they are not
    // integer attributes even though a pure-integer variant of the same width exists.
    case VertexFormat::UByte4Norm:
    case VertexFormat::Byte4Norm:
    case VertexFormat::UByte2Norm:
    case VertexFormat::Short2Norm:
    case VertexFormat::UShort2Norm:
    case VertexFormat::UInt1010102Norm:
      return false;
    default:
      return kVertexFormats[index].integer;
  }
}

} // namespace backend
