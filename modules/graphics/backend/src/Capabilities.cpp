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

#include <backend/Capabilities.h>

#include <algorithm>
#include <string>

namespace backend {
namespace {

/** @return the index of the single set bit in @p value, or -1 when @p value is 0 or not a power of 2. */
constexpr int bitIndex(Feature value) noexcept {
  const auto raw = static_cast<uint32_t>(value);
  if (raw == 0 || (raw & (raw - 1)) != 0) {
    return -1;
  }
  int index = 0;
  uint32_t probe = raw;
  while ((probe >>= 1) != 0) {
    ++index;
  }
  return index;
}

/**
 * The no-sRGB fallback for each sRGB format, used when a device cannot create sRGB attachments.
 *
 * This is the single most common real-world capability gap on the OpenGL ES 3.0 baseline: sRGB
 * render targets are optional in ES 3.0 and absent on a number of older mobile GPUs, while sRGB
 * sampling of *textures* is mandatory. Falling back to the UNORM equivalent keeps a renderer running
 * with slightly wrong gamma rather than not running at all - and the renderer is told, so it can
 * apply the transfer function in its shader instead.
 */
constexpr PixelFormat linearEquivalentOf(const PixelFormat format) noexcept {
  switch (format) {
    case PixelFormat::R8G8B8_SRGB:
      return PixelFormat::R8G8B8_UNORM;
    case PixelFormat::B8G8R8_SRGB:
      return PixelFormat::B8G8R8_UNORM;
    case PixelFormat::R8G8B8A8_SRGB:
      return PixelFormat::R8G8B8A8_UNORM;
    case PixelFormat::B8G8R8A8_SRGB:
      return PixelFormat::B8G8R8A8_UNORM;
    case PixelFormat::BC1_RGBA_SRGB:
      return PixelFormat::BC1_RGBA_UNORM;
    case PixelFormat::BC3_RGBA_SRGB:
      return PixelFormat::BC3_RGBA_UNORM;
    case PixelFormat::BC7_RGBA_SRGB:
      return PixelFormat::BC7_RGBA_UNORM;
    case PixelFormat::ETC2_RGB8_SRGB:
      return PixelFormat::ETC2_RGB8_UNORM;
    case PixelFormat::ETC2_RGBA8_SRGB:
      return PixelFormat::ETC2_RGBA8_UNORM;
    case PixelFormat::ASTC_4x4_SRGB:
      return PixelFormat::ASTC_4x4_UNORM;
    case PixelFormat::ASTC_8x8_SRGB:
      return PixelFormat::ASTC_8x8_UNORM;
    default:
      return PixelFormat::Undefined;
  }
}

} // namespace anonymous

const char* toString(const Feature feature) noexcept {
  switch (feature) {
    case Feature::None:
      return "None";
    case Feature::ComputeShader:
      return "ComputeShader";
    case Feature::ComputeShaderAtomics:
      return "ComputeShaderAtomics";
    case Feature::IndirectDispatch:
      return "IndirectDispatch";
    case Feature::ComputeWorkgroupBarrier:
      return "ComputeWorkgroupBarrier";
    case Feature::GeometryShader:
      return "GeometryShader";
    case Feature::TessellationShader:
      return "TessellationShader";
    case Feature::Texture3D:
      return "Texture3D";
    case Feature::TextureCube:
      return "TextureCube";
    case Feature::TextureArray:
      return "TextureArray";
    case Feature::TextureCubeArray:
      return "TextureCubeArray";
    case Feature::SamplerAnisotropy:
      return "SamplerAnisotropy";
    case Feature::SamplerBorderClamp:
      return "SamplerBorderClamp";
    case Feature::TextureLod:
      return "TextureLod";
    case Feature::SeparateSamplers:
      return "SeparateSamplers";
    case Feature::StorageTexture:
      return "StorageTexture";
    case Feature::ComparisonSampler:
      return "ComparisonSampler";
    case Feature::MultipleRenderTargets:
      return "MultipleRenderTargets";
    case Feature::MultisampleResolve:
      return "MultisampleResolve";
    case Feature::SampleRateShading:
      return "SampleRateShading";
    case Feature::AlphaToCoverage:
      return "AlphaToCoverage";
    case Feature::FillModeNonSolid:
      return "FillModeNonSolid";
    case Feature::DepthClamp:
      return "DepthClamp";
    case Feature::ProgrammableBlending:
      return "ProgrammableBlending";
    case Feature::LogicOp:
      return "LogicOp";
    case Feature::IntegerVertexAttributes:
      return "IntegerVertexAttributes";
    case Feature::HostWaitFence:
      return "HostWaitFence";
    case Feature::TimelineSemaphore:
      return "TimelineSemaphore";
    case Feature::QueryObjects:
      return "QueryObjects";
    case Feature::TimestampQuery:
      return "TimestampQuery";
    case Feature::PipelineStatisticsQuery:
      return "PipelineStatisticsQuery";
    case Feature::Swapchain:
      return "Swapchain";
    case Feature::NativeSurface:
      return "NativeSurface";
  }
  return "UnknownFeature";
}

const char* toString(FeatureUnavailableReason reason) noexcept {
  switch (reason) {
    case FeatureUnavailableReason::Available:
      return "available";
    case FeatureUnavailableReason::ApiVersionTooLow:
      return "API version too low";
    case FeatureUnavailableReason::ExtensionMissing:
      return "driver extension missing";
    case FeatureUnavailableReason::HardwareUnsupported:
      return "not supported by the GPU";
    case FeatureUnavailableReason::ArchitecturallyImpossible:
      return "no equivalent in this API";
    case FeatureUnavailableReason::NotImplementedByBackend:
      return "not implemented by this backend";
    case FeatureUnavailableReason::DisabledByConfiguration:
      return "disabled by configuration";
  }
  return "unknown reason";
}

FeatureUnavailableReason Capabilities::reasonFor(const Feature feature) const noexcept {
  if (supports(feature)) {
    return FeatureUnavailableReason::Available;
  }
  const int index = bitIndex(feature);
  if (index < 0 || index >= 32) {
    return FeatureUnavailableReason::HardwareUnsupported;
  }
  const FeatureUnavailableReason recorded = reasons[index];
  // A backend that forgot to record a reason still deserves a sensible answer.
  return recorded == FeatureUnavailableReason::Available ? FeatureUnavailableReason::HardwareUnsupported : recorded;
}

const TextureFormatCapabilities& Capabilities::queryTextureFormat(PixelFormat format) const noexcept {
  static constexpr TextureFormatCapabilities kUnsupported = {};
  const auto index = static_cast<size_t>(format);
  if (format == PixelFormat::Undefined || index >= kMaxPixelFormats) {
    return kUnsupported;
  }
  return formats[index];
}

uint32_t Capabilities::resolveSampleCount(const uint32_t desired) const noexcept {
  // Walk down from the request so the best *supported* count at or below it wins. Capping at the
  // request rather than picking the global maximum matters: silently upgrading a caller from 2x to
  // 8x MSAA would quadruple bandwidth use.
  for (uint32_t candidate = std::min(desired, kMaxSampleCount - 1); candidate > 1; --candidate) {
    if (supportsSampleCount(candidate)) {
      return candidate;
    }
  }
  return 1;
}

uint32_t Capabilities::resolveMipLevelCount(const Extent3D& size, const uint32_t requested) const noexcept {
  const uint32_t largest = std::max(size.width, std::max(size.height, size.depthOrArrayLayers));
  if (largest == 0) {
    return 1;
  }

  uint32_t maximum = 1;
  for (uint32_t dimension = largest; dimension > 1; dimension >>= 1) {
    ++maximum;
  }

  const uint32_t clamped = requested == 0 ? maximum : std::min(requested, maximum);
  return std::max(clamped, 1u);
}

uint32_t Capabilities::maxDimensionFor(const TextureType type) const noexcept {
  switch (type) {
    case TextureType::Texture1D:
      return limits.maxTextureDimension1D;
    case TextureType::Texture2D:
    case TextureType::Texture2DArray:
      return limits.maxTextureDimension2D;
    case TextureType::Texture3D:
      return limits.maxTextureDimension3D;
    case TextureType::TextureCube:
    case TextureType::TextureCubeArray:
      return limits.maxTextureDimensionCube;
  }
  return limits.maxTextureDimension2D;
}

void Capabilities::appendReport(std::string& out) const {
  out += "  API: ";
  out += apiVersion != nullptr ? apiVersion : "";
  out += "\n  SPIR-V input: ";
  out += acceptsSpirv ? "yes" : "no";
  out += "\n  Debug markers: ";
  out += supportsDebugMarkers ? "yes" : "no";
  out += "\n\n  Supported features:\n";

  for (uint32_t bit = 0; bit < 32; ++bit) {
    const auto feature = static_cast<Feature>(1u << bit);
    if (!supports(feature)) continue;
    out += "    + ";
    out += toString(feature);
    out += '\n';
  }

  out += "\n  Unsupported features:\n";
  for (uint32_t bit = 0; bit < 32; ++bit) {
    const auto feature = static_cast<Feature>(1u << bit);
    if (supports(feature)) continue;
    out += "    - ";
    out += toString(feature);
    out += " (";
    out += toString(reasonFor(feature));
    out += ")\n";
  }

  out += "\n  Limits:\n";
  const auto appendLimit = [&out](const char* name, uint64_t value) {
    out += "    ";
    out += name;
    out += " = ";
    out += std::to_string(value);
    out += '\n';
  };
  appendLimit("maxTextureDimension2D", limits.maxTextureDimension2D);
  appendLimit("maxTextureDimension3D", limits.maxTextureDimension3D);
  appendLimit("maxTextureArrayLayers", limits.maxTextureArrayLayers);
  appendLimit("maxVertexAttributes", limits.maxVertexAttributes);
  appendLimit("maxBufferSize", limits.maxBufferSize);
  appendLimit("maxUniformBufferBindingSize", limits.maxUniformBufferBindingSize);
  appendLimit("maxBoundDescriptorSets", limits.maxBoundDescriptorSets);
  appendLimit("maxColorAttachments", limits.maxColorAttachments);
  appendLimit("maxFramesInFlight", limits.maxFramesInFlight);
  appendLimit("maxPushConstantSize", limits.maxPushConstantSize);

  out += "\n  Sample counts: ";
  bool first = true;
  for (uint32_t count = 1; count < kMaxSampleCount; ++count) {
    if (!supportsSampleCount(count)) continue;
    if (!first) out += ", ";
    out += std::to_string(count);
    first = false;
  }
  if (first) out += "1 (MSAA unavailable)";
  out += '\n';
}

// ==================================================================================================
// Degradation
// ==================================================================================================

TextureValidationResult validateTextureDescriptor(const TextureDescriptor& descriptor,
        const Capabilities& capabilities) {
  TextureValidationResult result;
  result.resolved = descriptor;

  if (descriptor.format == PixelFormat::Undefined) {
    return TextureValidationResult{ .valid = false,
      .reason = FeatureUnavailableReason::HardwareUnsupported,
      .message = "the texture format is Undefined; set PixelFormat explicitly (there is no meaningful "
                 "default, because the right choice depends on whether the data is colour, depth or data)",
      .resolved = descriptor };
  }

  const TextureFormatCapabilities& formatInfo = capabilities.queryTextureFormat(descriptor.format);
  const bool wantsAttachment = hasUsage(descriptor.usage, TextureUsage::ColorAttachment) ||
                               hasUsage(descriptor.usage, TextureUsage::DepthStencilAttachment);
  const bool wantsStorage = hasUsage(descriptor.usage, TextureUsage::Storage);

  // 1. Format viability. A colour attachment format that cannot be rendered into is a hard failure;
  //    a sampled format that cannot be sampled is too. Both get a specific message so the caller
  //    knows which usage to drop.
  if (wantsAttachment && !formatInfo.renderable) {
    const PixelFormat substitute = linearEquivalentOf(descriptor.format);
    if (substitute != PixelFormat::Undefined && capabilities.queryTextureFormat(substitute).renderable) {
      result.resolved.format = substitute;
      result.valid = false;
      result.reason = FeatureUnavailableReason::HardwareUnsupported;
      result.message = "the requested format cannot be a render target on this device; substituted its linear "
                       "equivalent, so the caller must apply the sRGB transfer function itself";
      return result;
    }
    return TextureValidationResult{ false, FeatureUnavailableReason::HardwareUnsupported,
      "the requested format cannot be used as a render target on this device", descriptor };
  }

  if (wantsStorage && !formatInfo.storage) {
    // The reason distinguishes two very different situations for the caller. When the backend could
    // provide storage textures but the running device's API version is too low, newer hardware fixes
    // it, so the caller should disable the effect for now. When the technique is not expressible at
    // all, the caller has to rework the descriptor into a colour attachment or a compute output.
    const bool impossible =
            !capabilities.supports(Feature::ComputeShader) && !capabilities.supports(Feature::StorageTexture);
    return TextureValidationResult{ false,
      impossible ? FeatureUnavailableReason::ArchitecturallyImpossible : FeatureUnavailableReason::ApiVersionTooLow,
      impossible ? "storage textures (image load/store) are unavailable on this device, so the "
                   "descriptor must be reworked to use a colour attachment or a compute output"
                 : "storage textures need a newer API version than this device provides",
      descriptor };
  }

  // 2. Size. Clamping is explicitly opt-in via DegradationPolicy in the normal path; the raw
  //    validator reports the violation so a caller can decide whether to clamp, rescale or fail.
  const uint32_t maximum = capabilities.maxDimensionFor(descriptor.type);
  if (maximum != 0 && descriptor.size.width > maximum) {
    return TextureValidationResult{ false, FeatureUnavailableReason::HardwareUnsupported,
      "the requested width exceeds the device's maximum texture dimension", descriptor };
  }

  // 3. Sample count.
  if (descriptor.sampleCount > 1 && !capabilities.supportsSampleCount(descriptor.sampleCount)) {
    return TextureValidationResult{ false, FeatureUnavailableReason::HardwareUnsupported,
      "the requested MSAA sample count is not supported; use Capabilities::resolveSampleCount to "
      "pick the best available one",
      descriptor };
  }
  if (descriptor.sampleCount > 1 && descriptor.mipLevelCount > 1) {
    return TextureValidationResult{ false, FeatureUnavailableReason::ArchitecturallyImpossible,
      "multisampled textures cannot have mip levels on any supported API", descriptor };
  }

  // 4. Mip chain length.
  const uint32_t allowedMips = capabilities.resolveMipLevelCount(descriptor.size, descriptor.mipLevelCount);
  if (allowedMips != descriptor.mipLevelCount) {
    result.resolved.mipLevelCount = allowedMips;
    result.valid = false;
    result.reason = FeatureUnavailableReason::HardwareUnsupported;
    result.message = "the requested mip chain is longer than the texture size permits; it was truncated";
    return result;
  }

  // 5. Usage/feature agreement.
  if (hasUsage(descriptor.usage, TextureUsage::Sampled) && !formatInfo.sampled) {
    return TextureValidationResult{ false, FeatureUnavailableReason::HardwareUnsupported,
      "the requested format cannot be sampled on this device", descriptor };
  }

  return result;
}

PixelFormat chooseFormat(const Capabilities& capabilities, const PixelFormat* candidates, const uint32_t candidateCount,
        const bool renderableOnly) {
  if (candidates == nullptr) {
    return PixelFormat::Undefined;
  }

  for (uint32_t index = 0; index < candidateCount; ++index) {
    const PixelFormat candidate = candidates[index];
    if (candidate == PixelFormat::Undefined) {
      break;
    }

    const TextureFormatCapabilities& info = capabilities.queryTextureFormat(candidate);
    if (renderableOnly) {
      if (info.renderable) {
        return candidate;
      }
    } else if (info.sampled) {
      return candidate;
    }
  }
  return PixelFormat::Undefined;
}

// ==================================================================================================
// CapabilityBuilder
// ==================================================================================================

CapabilityBuilder& CapabilityBuilder::feature(Feature feature, const bool supported,
        const FeatureUnavailableReason reason) noexcept {
  const auto raw = static_cast<uint32_t>(feature);
  if (supported) {
    mCapabilities.features |= raw;
    if (const int index = bitIndex(feature); index >= 0 && index < 32) {
      mCapabilities.reasons[index] = FeatureUnavailableReason::Available;
    }
  } else if (raw != 0) {
    // Clearing is important for the "probe, then downgrade" pattern: a backend may claim a feature
    // early and revoke it once it has actually queried the driver.
    mCapabilities.features &= ~raw;
    if (const int index = bitIndex(feature); index >= 0 && index < 32) {
      mCapabilities.reasons[index] = reason;
    }
  }
  return *this;
}

CapabilityBuilder& CapabilityBuilder::limit(const Limits& limits) noexcept {
  mCapabilities.limits = limits;
  return *this;
}

CapabilityBuilder& CapabilityBuilder::sampleCount(const uint32_t count) noexcept {
  if (count != 0 && count < kMaxSampleCount) {
    mCapabilities.supportedSampleCounts |= (1u << count);
    if (count > 1) {
      const int index = bitIndex(Feature::MultisampleResolve);
      if (index >= 0) {
        mCapabilities.reasons[index] = FeatureUnavailableReason::Available;
      }
      mCapabilities.features |= static_cast<uint32_t>(Feature::MultisampleResolve);
    }
  }
  return *this;
}

CapabilityBuilder& CapabilityBuilder::textureFormat(PixelFormat format,
        const TextureFormatCapabilities support) noexcept {
  if (const auto index = static_cast<size_t>(format); index < kMaxPixelFormats) {
    mCapabilities.formats[index] = support;
  }
  return *this;
}

CapabilityBuilder& CapabilityBuilder::apiVersion(const char* version) noexcept {
  mCapabilities.apiVersion = version != nullptr ? version : "";
  return *this;
}

} // namespace backend
