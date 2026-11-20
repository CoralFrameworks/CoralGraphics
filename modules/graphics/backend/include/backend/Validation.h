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

#ifndef CORAL_BACKEND_VALIDATION_H
#define CORAL_BACKEND_VALIDATION_H

#include <backend/Capabilities.h>
#include <backend/Descriptors.h>
#include <backend/Pipelines.h>
#include <backend/Types.h>

#include <cstdint>

namespace backend {

/**
 * Outcome of validating a descriptor against a device's capability set.
 *
 * The RHI's degradation philosophy is "decide early, explain why". A renderer that asks for a
 * feature the hardware does not have should learn that at descriptor-validation time, with a reason
 * it can act on, rather than discovering a black screen three frames later.
 */
struct ValidationResult {
  /** True when the descriptor can be used as-is. */
  bool valid = true;
  /** The specific reason the descriptor was rejected or downgraded. */
  FeatureUnavailableReason reason = FeatureUnavailableReason::Available;
  /** Human readable explanation; empty when `valid` is true. Never null. */
  const char* message = "";

  explicit operator bool() const noexcept { return valid; }

  static ValidationResult ok() noexcept { return ValidationResult{}; }

  static ValidationResult fail(const FeatureUnavailableReason reason, const char* message) noexcept {
    ValidationResult result;
    result.valid = false;
    result.reason = reason;
    result.message = message;
    return result;
  }
};

/** Validates a buffer descriptor, checking size limits and usage/feature agreement. */
[[nodiscard]] ValidationResult validateBufferDescriptor(const BufferDescriptor& descriptor,
        const Capabilities& capabilities);

/**
 * Validates one descriptor write, in particular the byte range a buffer binding names.
 *
 * A buffer may be larger than the device's uniform/storage block size as long as each binding range fits: an arena that
 * many draws index into is the normal shape, and a binding that names a whole 1 MiB arena would be the mistake this
 * catches. `BufferBinding::size == 0` means "to the end of the buffer" and is therefore not checked here.
 */
[[nodiscard]] ValidationResult validateDescriptorWrite(const DescriptorWrite& write,
        const Capabilities& capabilities);

/** Validates a sampler descriptor, clamping anisotropy and rejecting unsupported address modes. */
[[nodiscard]] ValidationResult validateSamplerDescriptor(const SamplerDescriptor& descriptor,
        const Capabilities& capabilities);

/** Validates a descriptor set layout against per-stage binding limits. */
[[nodiscard]] ValidationResult validateDescriptorSetLayout(const DescriptorSetLayoutDescriptor& descriptor,
        const Capabilities& capabilities);

/**
 * Validates a render pipeline against a capability set and, when supplied, a render pass.
 *
 * Checks performed: that the requested stages are available at all, that the topology's stage
 * requirements are met, that the colour attachment formats are renderable, that the sample counts
 * agree between the pipeline and the render pass, and that vertex attribute formats do not need an
 * unsupported feature.
 *
 * @param descriptor   the pipeline to validate.
 * @param capabilities the device's capabilities.
 * @param renderPass   optional render pass to check attachment compatibility against; may be null.
 */
[[nodiscard]] ValidationResult validateRenderPipeline(const RenderPipelineDescriptor& descriptor,
        const Capabilities& capabilities, const RenderPassDescriptor* renderPass = nullptr);

[[nodiscard]] ValidationResult validateComputePipeline(const ComputePipelineDescriptor& descriptor,
        const Capabilities& capabilities);

/** Validates a render pass descriptor (attachment counts, formats, sample consistency). */
[[nodiscard]] ValidationResult validateRenderPassDescriptor(const RenderPassDescriptor& descriptor,
        const Capabilities& capabilities);

/**
 * A renderer's declared fallback preferences, so that degradation is a policy decision made once at
 * startup instead of an ad-hoc `if` scattered through the renderer.
 *
 * Example: a deferred renderer states that it wants a 16 bit float GBuffer, will accept an 11/11/10
 * one, and can live with 8 bit UNORM at reduced quality. `resolveGBufferFormat` then picks the best
 * one this device supports, and `qualityTier()` reports how much was lost.
 */
struct DegradationPolicy {
  /** Preferred render target formats, best first. Undefined terminates the list. */
  PixelFormat preferredColorFormats[6] = { PixelFormat::R16G16B16A16_SFLOAT, PixelFormat::R11G11B10_SFLOAT,
    PixelFormat::R8G8B8A8_UNORM, PixelFormat::Undefined, PixelFormat::Undefined, PixelFormat::Undefined };

  /** Preferred MSAA sample counts, best first. 1 (no MSAA) is always an acceptable fallback. */
  uint32_t preferredSampleCounts[4] = { 4, 2, 1, 1 };

  /** Accept an sRGB-capable format's UNORM equivalent when sRGB is unavailable. */
  bool allowSrgbDowngrade = true;

  /** Reduce mip level count when the device cannot store the requested chain. */
  bool allowMipReduction = true;

  /** Clamp oversized textures to the device limit instead of failing. */
  bool clampTextureSize = true;

  /** Pick the best supported format from `preferredColorFormats`. Undefined if none is renderable. */
  [[nodiscard]] PixelFormat resolveColorFormat(const Capabilities& capabilities) const;

  /** Pick the best supported sample count from `preferredSampleCounts`. Never returns 0. */
  [[nodiscard]] uint32_t resolveSampleCount(const Capabilities& capabilities) const;

  /**
   * How much of the preferred configuration survived.
   *  - High:   the first preference was available.
   *  - Medium: a lower-quality but still high-precision path was used.
   *  - Low:    a compatibility path (8 bit UNORM, no MSAA) was used.
   */
  enum class Tier : uint8_t {
    High,
    Medium,
    Low,
  };

  [[nodiscard]] Tier qualityTier(const Capabilities& capabilities) const;
};

[[nodiscard]] const char* toString(DegradationPolicy::Tier tier) noexcept;

/**
 * Describes a texture descriptor that was downgraded, so a renderer can report or compensate for the
 * loss. Produced by `resolveTextureDescriptor`.
 */
struct TextureResolution {
  /** The descriptor to actually create, with unsupported aspects relaxed. */
  TextureDescriptor descriptor = {};
  /** True when `descriptor` differs from the requested one. */
  bool degraded = false;
  /** Reason for the first, most significant downgrade. */
  FeatureUnavailableReason reason = FeatureUnavailableReason::Available;
  /** Human readable summary of what changed; empty when nothing did. */
  const char* note = "";
};

/**
 * Repairs @p requested so that it can be created on @p capabilities, honouring @p policy.
 *
 * This is the single entry point a renderer should use when it wants "the best texture this device
 * can give me for this purpose" instead of an exact specification. The OpenGL backend calls it
 * internally from `createTexture` when `DeviceConfiguration::allowFallback` is set, so a renderer can
 * also simply pass an aspirational descriptor and get a working one back.
 */
[[nodiscard]] TextureResolution resolveTextureDescriptor(const TextureDescriptor& requested,
        const Capabilities& capabilities, const DegradationPolicy& policy);

} // namespace backend

#endif // CORAL_BACKEND_VALIDATION_H
