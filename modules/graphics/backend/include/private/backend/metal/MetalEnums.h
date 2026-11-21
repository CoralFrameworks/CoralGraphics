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

#ifndef CORAL_BACKEND_METAL_ENUMS_H
#define CORAL_BACKEND_METAL_ENUMS_H

/**
 * @file MetalEnums.h
 *
 * The RHI <-> Metal vocabulary translation tables.
 *
 * Every function here is a pure mapping with no device state, which is what makes it testable without a GPU and
 * keeps the device code readable: `createTexture` says "map the format" rather than carrying a switch of forty
 * cases in the middle of its validation.
 *
 * ## The argument-table budget
 *
 * Metal has no descriptor sets, so a `(set, binding)` pair has to be turned into one of Metal's three flat argument
 * tables (`[[buffer(n)]]`, `[[texture(n)]]`, `[[sampler(n)]]`) by hand. The constants below are that convention, and
 * both the MSL translation and the command buffer's bind path read them from here so the two cannot drift apart:
 *
 * | range | contents |
 * |---|---|
 * | `buffer(0..7)` | vertex stream buffers, indexed by `VertexAttribute::bufferIndex` |
 * | `buffer(8..29)` | descriptor-set uniform, read-only and read/write buffers |
 * | `buffer(30)` | the push-constant block |
 * | `texture(0..30)` | descriptor-set sampled and storage textures |
 * | `sampler(0..15)` | descriptor-set samplers |
 *
 * The vertex range is reserved rather than shared because a vertex buffer bound with `setVertexBuffer:atIndex:`
 * occupies the *same* buffer argument table a shader's `[[buffer(n)]]` parameters index. Giving a uniform block
 * slot 1 would silently overwrite the vertex stream in slot 1, which is a wrong picture rather than an error.
 */

#include <backend/Descriptors.h>
#include <backend/Pipelines.h>
#include <backend/Types.h>

#import <Metal/Metal.h>

#include <cstdint>

namespace backend::metal {

/** First buffer argument slot a shader may name, i.e. the first after the eight vertex stream slots. */
inline constexpr uint32_t kFirstResourceBufferSlot = 8;
/** Buffer argument slot the push-constant block occupies. The last slot is deliberate: nothing else may use it. */
inline constexpr uint32_t kPushConstantBufferSlot = 30;
/** Largest texture argument slot (Metal's limit is 31 per stage on every Apple GPU family). */
inline constexpr uint32_t kMaxTextureSlots = 31;
/** Largest sampler argument slot (16 per stage). */
inline constexpr uint32_t kMaxSamplerSlots = 16;
/** Largest vertex stream slot, from `kMaxVertexBufferLayouts`. */
inline constexpr uint32_t kMaxVertexBufferSlots = 8;

/** @return the Metal pixel format for @p format, or `MTLPixelFormatInvalid` when the mapping has no equivalent. */
[[nodiscard]] MTLPixelFormat toMtlPixelFormat(PixelFormat format);

/** @return true when @p format maps to a Metal pixel format at all. */
[[nodiscard]] bool hasMtlPixelFormat(PixelFormat format);

/**
 * @return the Metal pixel format a `D24_UNORM_S8_UINT`-style request is actually realised as.
 *
 * Metal has no 24-bit depth format. The RHI's universal depth/stencil format is mapped to
 * `MTLPixelFormatDepth32Float_Stencil8`, which every Apple GPU supports and which keeps the stencil aspect -- the
 * canvas's path fills need it. The mapping is reported as support for the *requested* format, because that is what a
 * caller asked about.
 */
[[nodiscard]] MTLPixelFormat toMtlDepthStencilPixelFormat(PixelFormat format);

/** @return the vertex format for @p format, or `MTLVertexFormatInvalid`. */
[[nodiscard]] MTLVertexFormat toMtlVertexFormat(VertexFormat format);

[[nodiscard]] MTLBlendFactor toMtlBlendFactor(BlendFactor factor);
[[nodiscard]] MTLBlendOperation toMtlBlendOperation(BlendOp op);
[[nodiscard]] MTLCompareFunction toMtlCompareFunction(CompareOp op);
[[nodiscard]] MTLStencilOperation toMtlStencilOperation(StencilOp op);
[[nodiscard]] MTLPrimitiveType toMtlPrimitiveType(PrimitiveTopology topology);
[[nodiscard]] MTLIndexType toMtlIndexType(IndexFormat format);
[[nodiscard]] MTLLoadAction toMtlLoadAction(LoadOp op);
[[nodiscard]] MTLStoreAction toMtlStoreAction(StoreOp op);
[[nodiscard]] MTLCullMode toMtlCullMode(CullMode mode);
[[nodiscard]] MTLColorWriteMask toMtlColorWriteMask(uint8_t channels);
[[nodiscard]] MTLTextureType toMtlTextureType(TextureType type, uint32_t sampleCount);
[[nodiscard]] MTLTextureType toMtlTextureViewType(TextureViewDimension dimension, uint32_t arrayLayers);
[[nodiscard]] MTLSamplerMinMagFilter toMtlMinMagFilter(FilterMode mode);
[[nodiscard]] MTLSamplerMipFilter toMtlMipFilter(MipmapFilterMode mode);
[[nodiscard]] MTLSamplerAddressMode toMtlAddressMode(SamplerAddressMode mode);
[[nodiscard]] MTLSamplerBorderColor toMtlBorderColor(SamplerBorderColor color);

/**
 * @return the Metal winding that makes @p frontFace mean what the RHI says when the vertex shader flips Y.
 *
 * A Y flip mirrors the framebuffer, so a counter-clockwise triangle becomes clockwise. The flip lives in the shader
 * (SPIRV-Cross inserts it because the RHI's SPIR-V is authored in Vulkan's Y-down clip space), so the pipeline has to
 * invert the winding to compensate -- exactly what the OpenGL backend does for the same reason.
 */
[[nodiscard]] MTLWinding toMtlWinding(FrontFace frontFace, bool flipY);

/** @return true when @p format has a depth aspect. */
[[nodiscard]] bool isDepthFormat(PixelFormat format);
/** @return true when @p format has a stencil aspect. */
[[nodiscard]] bool isStencilFormat(PixelFormat format);

} // namespace backend::metal

#endif // CORAL_BACKEND_METAL_ENUMS_H
