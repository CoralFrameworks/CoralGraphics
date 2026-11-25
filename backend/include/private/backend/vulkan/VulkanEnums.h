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

#ifndef CORAL_BACKEND_VULKAN_ENUMS_H
#define CORAL_BACKEND_VULKAN_ENUMS_H

/**
 * @file VulkanEnums.h
 *
 * The RHI vocabulary translated into Vulkan's, in one place.
 *
 * ## Why a header of inline functions rather than a table
 *
 * Every one of these is a total mapping with no per-device choice: `BlendOp::ReverseSubtract` means
 * `VK_BLEND_OP_REVERSE_SUBTRACT` on every device. Keeping them inline and out of the device class makes
 * them available to the command-buffer translation unit without a link dependency on the device, which
 * is what lets the two files stay independent.
 *
 * The mapping that *does* depend on the device - whether a format can be sampled, whether a sample
 * count is framebuffer-supported - is a query, not a translation, and lives in the device.
 */

#include <backend/Descriptors.h>
#include <backend/Pipelines.h>
#include <backend/Types.h>

#include <vulkan/vulkan.h>

#include "private/backend/vulkan/VulkanX11Compat.h"

#include <cstdint>
#include <type_traits>

namespace backend::vulkan {

/**
 * The numeric value of a Vulkan handle.
 *
 * A non-dispatchable handle is a pointer type in the standard headers when the platform has 64-bit
 * pointers, and a `uint64_t` when it does not, so a plain cast is wrong on one of the two. A
 * `if constexpr` here is the one place that difference is stated.
 */
template<typename Handle>
[[nodiscard]] uint64_t handleValue(const Handle handle) noexcept {
  if constexpr (std::is_pointer_v<Handle>) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
  } else {
    return static_cast<uint64_t>(handle);
  }
}

/** A Vulkan handle as the `void*` the RHI's native-object escape hatch carries. */
template<typename Handle>
[[nodiscard]] void* handlePointer(const Handle handle) noexcept {
  if constexpr (std::is_pointer_v<Handle>) {
    return const_cast<void*>(static_cast<const void*>(handle));
  } else {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(handle));
  }
}

/** @return the VkFormat for @p format, or VK_FORMAT_UNDEFINED when the RHI format has no equivalent. */
[[nodiscard]] VkFormat toVkFormat(PixelFormat format) noexcept;

/** @return true when @p format is a depth or depth/stencil format. */
[[nodiscard]] bool isDepthFormat(PixelFormat format) noexcept;

/** @return true when @p format carries a stencil component. */
[[nodiscard]] bool isStencilFormat(PixelFormat format) noexcept;

/** @return true when @p format is a compressed block format. */
[[nodiscard]] bool isCompressed(PixelFormat format) noexcept;

/** @return the aspect mask a view of @p format needs. */
[[nodiscard]] VkImageAspectFlags aspectFor(PixelFormat format) noexcept;

/** @return the Vulkan image type a texture type needs. */
[[nodiscard]] VkImageType toVkImageType(TextureType type) noexcept;

/** @return the Vulkan view type a view dimension needs. */
[[nodiscard]] VkImageViewType toVkImageViewType(TextureViewDimension dimension) noexcept;

/** @return the layer count implied by a texture type and its declared depth/array extent. */
[[nodiscard]] uint32_t layerCountFor(TextureType type, const Extent3D& extent) noexcept;

/** @return the Vulkan vertex format for @p format, or VK_FORMAT_UNDEFINED when there is none. */
[[nodiscard]] VkFormat toVkVertexFormat(VertexFormat format) noexcept;

/** @return the VkShaderStageFlagBits for @p stage. */
[[nodiscard]] VkShaderStageFlagBits toVkStage(ShaderStage stage) noexcept;

/** @return a stage mask with every graphics stage, used for push constants and descriptor visibility. */
[[nodiscard]] VkShaderStageFlags allGraphicsStages() noexcept;

/**
 * Expands an RHI stage-flags byte into Vulkan's.
 *
 * The RHI's byte is deliberately small: bit 0 is the vertex stage, bit 1 the fragment stage, bit 2 the
 * compute stage, bit 3 geometry, bit 4 tessellation control and bit 5 tessellation evaluation. A zero
 * byte means "the caller did not say", which is treated as all graphics stages so that a descriptor is
 * visible rather than silently ignored.
 */
[[nodiscard]] VkShaderStageFlags toVkStageFlags(uint8_t stageFlags) noexcept;

[[nodiscard]] VkPrimitiveTopology toVkTopology(PrimitiveTopology topology) noexcept;
[[nodiscard]] VkCullModeFlags toVkCullMode(CullMode mode) noexcept;
[[nodiscard]] VkFrontFace toVkFrontFace(FrontFace face) noexcept;
[[nodiscard]] VkCompareOp toVkCompareOp(CompareOp op) noexcept;
[[nodiscard]] VkStencilOp toVkStencilOp(StencilOp op) noexcept;
[[nodiscard]] VkBlendFactor toVkBlendFactor(BlendFactor factor) noexcept;
[[nodiscard]] VkBlendOp toVkBlendOp(BlendOp op) noexcept;

[[nodiscard]] VkFilter toVkFilter(FilterMode mode) noexcept;
[[nodiscard]] VkSamplerMipmapMode toVkMipmapMode(MipmapFilterMode mode) noexcept;
[[nodiscard]] VkSamplerAddressMode toVkAddressMode(SamplerAddressMode mode) noexcept;
[[nodiscard]] VkBorderColor toVkBorderColor(SamplerBorderColor color) noexcept;
[[nodiscard]] VkSamplerYcbcrModelConversion toVkYcbcrModel(YcbcrModelConversion model) noexcept;
[[nodiscard]] VkSamplerYcbcrRange toVkYcbcrRange(YcbcrRange range) noexcept;
[[nodiscard]] VkChromaLocation toVkChromaLocation(ChromaLocation location) noexcept;
[[nodiscard]] VkIndexType toVkIndexType(IndexFormat format) noexcept;
[[nodiscard]] VkQueryType toVkQueryType(QueryType type) noexcept;
[[nodiscard]] VkDescriptorType toVkDescriptorType(BindingType type) noexcept;
[[nodiscard]] VkColorComponentFlags toVkColorWriteMask(uint8_t mask) noexcept;
[[nodiscard]] VkAttachmentLoadOp toVkLoadOp(LoadOp op) noexcept;
[[nodiscard]] VkAttachmentStoreOp toVkStoreOp(StoreOp op) noexcept;

} // namespace backend::vulkan

#endif // CORAL_BACKEND_VULKAN_ENUMS_H

