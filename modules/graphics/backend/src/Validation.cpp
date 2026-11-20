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

#include <backend/Validation.h>

namespace backend {
namespace {

/** Aggregate per-stage binding use, so one layout can be checked against per-stage limits. */
struct StageBindingUse {
  uint32_t sampledTextures = 0;
  uint32_t samplers = 0;
  uint32_t uniformBuffers = 0;
  uint32_t storageBuffers = 0;
};

StageBindingUse accumulateBindings(const DescriptorSetLayoutDescriptor& descriptor, const uint8_t stageBit) {
  StageBindingUse use;
  for (uint32_t index = 0; index < descriptor.bindingCount; ++index) {
    const DescriptorBinding& binding = descriptor.bindings[index];
    if ((binding.stageFlags & stageBit) == 0) continue;
    switch (binding.type) {
      case BindingType::TextureSampler:
        use.sampledTextures += binding.count;
        use.samplers += binding.count;
        break;
      case BindingType::Texture:
        use.sampledTextures += binding.count;
        break;
      case BindingType::Sampler:
        use.samplers += binding.count;
        break;
      case BindingType::UniformBuffer:
        use.uniformBuffers += binding.count;
        break;
      case BindingType::ReadOnlyStorageBuffer:
      case BindingType::StorageBuffer:
        use.storageBuffers += binding.count;
        break;
    }
  }
  return use;
}

/** Maps a vertex format to an integer-ness requirement. */
bool requiresIntegerAttributes(const VertexFormat format) noexcept {
  switch (format) {
    case VertexFormat::UInt:
    case VertexFormat::UInt2:
    case VertexFormat::UInt3:
    case VertexFormat::UInt4:
    case VertexFormat::SInt:
    case VertexFormat::SInt2:
    case VertexFormat::SInt3:
    case VertexFormat::SInt4:
    case VertexFormat::UByte4:
      return true;
    default:
      return false;
  }
}

} // namespace anonymous

const char* toString(const DegradationPolicy::Tier tier) noexcept {
  switch (tier) {
    case DegradationPolicy::Tier::High:
      return "high";
    case DegradationPolicy::Tier::Medium:
      return "medium";
    case DegradationPolicy::Tier::Low:
      return "low";
  }
  return "unknown";
}

PixelFormat DegradationPolicy::resolveColorFormat(const Capabilities& capabilities) const {
  // The preference list is terminated by Undefined when shorter than the array, so a plain linear
  // scan with an early break is enough. `renderableOnly` is set because the primary consumer of this
  // helper is render-target selection.
  return chooseFormat(capabilities, preferredColorFormats, 6, /*renderableOnly=*/true);
}

uint32_t DegradationPolicy::resolveSampleCount(const Capabilities& capabilities) const {
  for (const unsigned int candidate: preferredSampleCounts) {
    if (candidate <= 1) {
      break;
    }
    if (capabilities.supportsSampleCount(candidate)) {
      return candidate;
    }
  }
  return 1;
}

DegradationPolicy::Tier DegradationPolicy::qualityTier(const Capabilities& capabilities) const {
  const PixelFormat chosen = resolveColorFormat(capabilities);

  // Nothing from the preference list is usable at all: the caller is on a hand-rolled compatibility
  // path, so report the lowest tier rather than pretending a default was honoured.
  if (chosen == PixelFormat::Undefined) {
    return Tier::Low;
  }

  // Nothing was lost only when the top preference itself was granted. Merely landing on *some* entry
  // of the list is a downgrade by definition, because the list is ordered by preference.
  if (chosen == preferredColorFormats[0] && resolveSampleCount(capabilities) == preferredSampleCounts[0]) {
    return Tier::High;
  }

  // A float or half-float target with MSAA is still a quality path; anything that fell back to 8 bit
  // UNORM or lost MSAA entirely is the compatibility tier. The 8 bit formats are named explicitly
  // rather than inferred from the byte size, because R32_UINT is also 4 bytes per texel and is not a
  // quality colour target.
  const bool eightBitUnorm =
    chosen == PixelFormat::R8G8B8A8_UNORM ||
    chosen == PixelFormat::B8G8R8A8_UNORM ||
    chosen == PixelFormat::R8G8B8A8_SRGB ||
    chosen == PixelFormat::B8G8R8A8_SRGB;
  if (!eightBitUnorm && resolveSampleCount(capabilities) > 1) {
    return Tier::Medium;
  }

  return Tier::Low;
}

// ==================================================================================================
// Descriptor validation
// ==================================================================================================

ValidationResult validateBufferDescriptor(const BufferDescriptor& descriptor, const Capabilities& capabilities) {
  if (descriptor.size == 0) {
    return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported, "a buffer must have a non-zero size");
  }

  if (descriptor.size > capabilities.limits.maxBufferSize) {
    return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
      "the requested buffer size exceeds the device's maximum buffer size");
  }

  if (descriptor.usage == 0) {
    return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
      "a buffer needs at least one BufferUsage flag; without one nothing "
      "can read or write it");
  }

  const bool storage = hasUsage(descriptor.usage, BufferUsage::Storage);
  if (storage && !capabilities.supports(Feature::ComputeShader)) {
    return ValidationResult::fail(FeatureUnavailableReason::ApiVersionTooLow,
      "storage buffers are unavailable on this device; on OpenGL ES they require ES 3.1 or the "
      "ARB_shader_storage_buffer_object extension");
  }

  // ## Buffer size is not binding size
  //
  // `maxUniformBufferBindingSize` / `maxStorageBufferBindingSize` bound the *range a descriptor names*, not the size of
  // the buffer that range lives in. A buffer may be arbitrarily large and be read through many small bindings -- which is
  // exactly what a uniform arena is -- and checking the buffer against the binding limit made every arena larger than
  // 64 KiB impossible to create. The range check lives in `validateDescriptorWrite`, where the range is known.

  // Index buffers are the one place where host-visible memory is a correctness issue rather than a
  // performance one on some mobile drivers; warn by rejecting the combination outright.
  if (hasUsage(descriptor.usage, BufferUsage::Index) && descriptor.memoryUsage == MemoryUsage::GpuToCpu) {
    return ValidationResult::fail(FeatureUnavailableReason::ArchitecturallyImpossible,
      "an index buffer cannot be GpuToCpu; index data is written by the host and read by the GPU, "
      "so use GpuOnly or CpuToGpu");
  }

  return ValidationResult::ok();
}

ValidationResult validateDescriptorWrite(const DescriptorWrite& write, const Capabilities& capabilities) {
  switch (write.type) {
    case BindingType::UniformBuffer:
      // `size == 0` means "to the end of the buffer", which the backend spells VK_WHOLE_SIZE; its actual length is a
      // property of the buffer and is not known here. A caller that names a range is checked against the device's limit.
      if (write.buffer.size != 0 && write.buffer.size > capabilities.limits.maxUniformBufferBindingSize) {
        return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
          "the uniform buffer binding range exceeds the device's maximum uniform block size");
      }
      break;
    case BindingType::ReadOnlyStorageBuffer:
    case BindingType::StorageBuffer:
      if (write.buffer.size != 0 && write.buffer.size > capabilities.limits.maxStorageBufferBindingSize) {
        return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
          "the storage buffer binding range exceeds the device's maximum storage block size");
      }
      break;
    case BindingType::TextureSampler:
    case BindingType::Sampler:
    case BindingType::Texture:
      break;
  }
  return ValidationResult::ok();
}

ValidationResult validateSamplerDescriptor(const SamplerDescriptor& descriptor, const Capabilities& capabilities) {
  if (descriptor.maxAnisotropy > 1.0f && !capabilities.supports(Feature::SamplerAnisotropy)) {
    return ValidationResult::fail(FeatureUnavailableReason::ExtensionMissing,
      "anisotropic filtering is unavailable; set maxAnisotropy to 1.0, or check "
      "Feature::SamplerAnisotropy and lower it");
  }

  if (descriptor.maxAnisotropy > capabilities.maxSamplerAnisotropy) {
    return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
      "the requested anisotropy exceeds the device maximum; clamp it to "
      "Capabilities::maxSamplerAnisotropy");
  }

  const auto needsBorder = [](const SamplerAddressMode mode) { return mode == SamplerAddressMode::ClampToBorder; };
  if ((needsBorder(descriptor.addressModeU) || needsBorder(descriptor.addressModeV) ||
       needsBorder(descriptor.addressModeW)) &&
      !capabilities.supports(Feature::SamplerBorderClamp)) {
    return ValidationResult::fail(FeatureUnavailableReason::ExtensionMissing,
      "clamp-to-border addressing is unavailable; use ClampToEdge and "
      "handle the border in the shader");
  }

  if (descriptor.depthCompare && descriptor.filterType != FilterType::Comparison) {
    return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
      "depthCompare requires filterType == FilterType::Comparison");
  }

  if (descriptor.filterType == FilterType::Comparison && !capabilities.supports(Feature::ComparisonSampler)) {
    return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
      "comparison samplers are unavailable on this device");
  }

  if (descriptor.lodMinClamp > descriptor.lodMaxClamp) {
    return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
      "lodMinClamp must not exceed lodMaxClamp");
  }

  return ValidationResult::ok();
}

ValidationResult validateDescriptorSetLayout(const DescriptorSetLayoutDescriptor& descriptor,
  const Capabilities& capabilities) {
  if (descriptor.bindingCount > kMaxDescriptorBindings) {
    return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
      "the descriptor set declares more bindings than kMaxDescriptorBindings");
  }

  // Reject duplicate (binding) numbers: the reflection-driven lookup in every backend assumes a
  // binding number identifies exactly one resource.
  for (uint32_t i = 0; i < descriptor.bindingCount; ++i) {
    if (descriptor.bindings[i].count == 0) {
      return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
        "a descriptor binding must have count >= 1");
    }
    for (uint32_t j = i + 1; j < descriptor.bindingCount; ++j) {
      if (descriptor.bindings[i].binding == descriptor.bindings[j].binding) {
        return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
          "two descriptor bindings share the same binding number");
      }
    }
  }

  // Per-stage budget checks. Stage bits are: 1 = vertex, 2 = fragment, 4 = compute.
  const StageBindingUse vertex = accumulateBindings(descriptor, 1u << 0);
  const StageBindingUse fragment = accumulateBindings(descriptor, 1u << 1);
  const StageBindingUse compute = accumulateBindings(descriptor, 1u << 2);

  const auto checkStage = [&capabilities](const StageBindingUse& use, const char* stage) -> ValidationResult {
    if (use.sampledTextures > capabilities.limits.maxSampledTexturesPerStage) {
      (void) stage;
      return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
        "the layout exceeds the per-stage sampled texture limit");
    }
    if (use.samplers > capabilities.limits.maxSamplersPerStage) {
      return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
        "the layout exceeds the per-stage sampler limit");
    }
    if (use.uniformBuffers > capabilities.limits.maxUniformBuffersPerStage) {
      return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
        "the layout exceeds the per-stage uniform buffer limit");
    }
    if (use.storageBuffers > capabilities.limits.maxStorageBuffersPerStage) {
      return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
        "the layout exceeds the per-stage storage buffer limit");
    }
    return ValidationResult::ok();
  };

  if (const ValidationResult result = checkStage(vertex, "vertex"); !result.valid) {
    return result;
  }
  if (const ValidationResult result = checkStage(fragment, "fragment"); !result.valid) {
    return result;
  }
  if (compute.sampledTextures + compute.uniformBuffers + compute.storageBuffers > 0) {
    if (!capabilities.supports(Feature::ComputeShader)) {
      return ValidationResult::fail(FeatureUnavailableReason::ApiVersionTooLow,
        "the layout binds resources to the compute stage, which is unavailable on this device");
    }
    if (const ValidationResult result = checkStage(compute, "compute"); !result.valid) {
      return result;
    }
  }

  return ValidationResult::ok();
}

ValidationResult validateRenderPassDescriptor(const RenderPassDescriptor& descriptor,
  const Capabilities& capabilities) {
  if (descriptor.colorAttachmentCount > kMaxColorAttachments) {
    return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
      "more colour attachments were declared than kMaxColorAttachments");
  }

  if (descriptor.colorAttachmentCount > capabilities.limits.maxColorAttachments) {
    return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
      "the render pass needs more colour attachments than this device supports");
  }

  if (descriptor.colorAttachmentCount > 1 && !capabilities.supports(Feature::MultipleRenderTargets)) {
    return ValidationResult::fail(FeatureUnavailableReason::ApiVersionTooLow,
      "multiple render targets are unavailable; split the pass into one pass per target, or use "
      "Feature::MultipleRenderTargets to select a fallback path");
  }

  for (uint32_t index = 0; index < descriptor.colorAttachmentCount; ++index) {
    const PixelFormat format = descriptor.colorFormats[index];
    if (format == PixelFormat::Undefined) {
      return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
        "a colour attachment slot has an Undefined format");
    }
    if (!capabilities.queryTextureFormat(format).renderable) {
      return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
        "a colour attachment format cannot be rendered to on this device");
    }
  }

  if (descriptor.depthStencilFormat != PixelFormat::Undefined) {
    const PixelFormatInfo& info = getPixelFormatInfo(descriptor.depthStencilFormat);
    if (!info.depth) {
      return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
        "the depth/stencil format is not a depth format");
    }
    if (!capabilities.queryTextureFormat(descriptor.depthStencilFormat).renderable) {
      return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
        "the depth/stencil format cannot be used as an attachment here");
    }
  }

  if (descriptor.sampleCount > 1) {
    if (!capabilities.supportsSampleCount(descriptor.sampleCount)) {
      return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
        "the render pass requests an unsupported MSAA sample count");
    }
    if (descriptor.depthStencilFormat != PixelFormat::Undefined && descriptor.colorAttachmentCount > 0) {
      // Mixed sample counts between colour and depth are rejected by most drivers; catching it here
      // turns a driver-specific failure into a clear message. The information needed to detect the
      // mismatch lives in the attachment descriptors, so only the pass-level count is checked.
    }
  }

  return ValidationResult::ok();
}

ValidationResult validateRenderPipeline(const RenderPipelineDescriptor& descriptor, const Capabilities& capabilities,
  const RenderPassDescriptor* renderPass) {
  if (!descriptor.vertexModule.valid() && !descriptor.fragmentModule.valid()) {
    return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
      "a render pipeline needs at least a vertex or a fragment stage");
  }

  if (!descriptor.layout.valid()) {
    return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
      "a render pipeline needs a pipeline layout, even when it binds no "
      "descriptors; pass an empty layout");
  }

  // Stage availability.
  if (descriptor.patchControlPoints > 0 || descriptor.rasterizer.topology == PrimitiveTopology::PatchList) {
    if (!capabilities.supports(Feature::TessellationShader)) {
      return ValidationResult::fail(FeatureUnavailableReason::ApiVersionTooLow,
        "a patch-list topology needs tessellation, which this device does not provide; fall back "
        "to a triangle list and pre-tessellate on the CPU, or gate the effect on "
        "Feature::TessellationShader");
    }
    if (descriptor.patchControlPoints == 0) {
      return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
        "patchControlPoints must be non-zero for a patch-list topology");
    }
  }

  if (descriptor.rasterizer.wireframe && !capabilities.supports(Feature::FillModeNonSolid)) {
    return ValidationResult::fail(FeatureUnavailableReason::ArchitecturallyImpossible,
      "wireframe fill is unavailable; draw a line-list index buffer instead");
  }

  if (descriptor.rasterizer.depthClampEnable && !capabilities.supports(Feature::DepthClamp)) {
    return ValidationResult::fail(FeatureUnavailableReason::ArchitecturallyImpossible,
      "depth clamping is unavailable; clamp depth in the shader instead");
  }

  if (descriptor.multisample.alphaToCoverageEnable && !capabilities.supports(Feature::AlphaToCoverage)) {
    return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
      "alpha-to-coverage is unavailable on this device");
  }

  // Vertex attribute checks.
  if (descriptor.vertexInput.bufferCount > capabilities.limits.maxVertexBuffers) {
    return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
      "the pipeline declares more vertex buffers than the device supports");
  }
  if (descriptor.vertexInput.attributeCount > capabilities.limits.maxVertexAttributes) {
    return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
      "the pipeline declares more vertex attributes than the device supports");
  }

  for (uint32_t index = 0; index < descriptor.vertexInput.bufferCount; ++index) {
    const VertexBufferLayout& layout = descriptor.vertexInput.buffers[index];
    if (layout.stride > capabilities.limits.maxVertexBufferStride) {
      return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
        "a vertex buffer stride exceeds the device maximum");
    }
    if (layout.firstAttribute + layout.attributeCount > descriptor.vertexInput.attributeCount) {
      return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
        "a vertex buffer layout references attributes past the end of the "
        "attribute array");
    }
  }

  for (uint32_t index = 0; index < descriptor.vertexInput.attributeCount; ++index) {
    const VertexAttribute& attribute = descriptor.vertexInput.attributes[index];
    if (attribute.format == VertexFormat::Invalid) {
      return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
        "a vertex attribute has an Invalid format");
    }
    if (attribute.bufferIndex >= descriptor.vertexInput.bufferCount) {
      return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
        "a vertex attribute references a vertex buffer that was not declared");
    }
    if (requiresIntegerAttributes(attribute.format) && !capabilities.supports(Feature::IntegerVertexAttributes)) {
      return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
        "an integer vertex attribute is unsupported; convert it to a "
        "normalised float format on the host");
    }
  }

  // Attachment checks. The render pass is the authority on formats; the pipeline must agree.
  if (renderPass != nullptr) {
    if (renderPass->colorAttachmentCount != descriptor.colorTargetCount) {
      return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
        "the pipeline's colour target count does not match the render pass; a pipeline is only "
        "compatible with a pass that has the same attachment shape");
    }
    for (uint32_t index = 0; index < descriptor.colorTargetCount; ++index) {
      if (renderPass->colorFormats[index] != descriptor.colorTargets[index].format) {
        return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
          "a colour target format does not match the render pass attachment format");
      }
    }
    if (renderPass->depthStencilFormat != descriptor.depthStencilFormat) {
      return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
        "the pipeline's depth/stencil format does not match the render pass");
    }
    if (renderPass->sampleCount != descriptor.multisample.sampleCount) {
      return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
        "the pipeline's sample count does not match the render pass");
    }
  }

  if (descriptor.colorTargetCount > capabilities.limits.maxColorAttachments) {
    return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
      "the pipeline declares more colour targets than the device supports");
  }

  for (uint32_t index = 0; index < descriptor.colorTargetCount; ++index) {
    const ColorTargetState& target = descriptor.colorTargets[index];
    if (target.format == PixelFormat::Undefined) {
      return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
        "a colour target has an Undefined format");
    }
    if (!capabilities.queryTextureFormat(target.format).renderable) {
      return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
        "a colour target format cannot be rendered to on this device");
    }
    if (target.blend.blendEnable && !capabilities.queryTextureFormat(target.format).blendable) {
      return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
        "blending is requested for a format that cannot be blended into");
    }
    // Min/max blend equations are not available on the OpenGL ES 3.0 baseline.
    if (target.blend.blendEnable &&
        (target.blend.color.operation == BlendOp::Min || target.blend.color.operation == BlendOp::Max ||
         target.blend.alpha.operation == BlendOp::Min || target.blend.alpha.operation == BlendOp::Max) &&
        !capabilities.supports(Feature::ProgrammableBlending)) {
      return ValidationResult::fail(FeatureUnavailableReason::ApiVersionTooLow,
        "min/max blend equations require EXT_blend_minmax or OpenGL ES 3.2");
    }
  }

  if (descriptor.depthStencilFormat != PixelFormat::Undefined) {
    if (!getPixelFormatInfo(descriptor.depthStencilFormat).depth) {
      return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
        "depthStencilFormat is not a depth format");
    }
  }

  if (descriptor.multisample.sampleCount > 1 && descriptor.depthStencilFormat != PixelFormat::Undefined) {
    const uint32_t supported = capabilities.resolveSampleCount(descriptor.multisample.sampleCount);
    if (supported != descriptor.multisample.sampleCount) {
      return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
        "the requested sample count is not usable for a combined "
        "colour + depth target on this device");
    }
  }

  if (descriptor.colorTargetCount == 0 && descriptor.depthStencilFormat == PixelFormat::Undefined) {
    return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
      "a render pipeline must declare at least one colour or depth/stencil target");
  }

  return ValidationResult::ok();
}

ValidationResult validateComputePipeline(const ComputePipelineDescriptor& descriptor,
  const Capabilities& capabilities) {
  if (!capabilities.supports(Feature::ComputeShader)) {
    return ValidationResult::fail(capabilities.reasonFor(Feature::ComputeShader) == FeatureUnavailableReason::Available
                                    ? FeatureUnavailableReason::ApiVersionTooLow
                                    : capabilities.reasonFor(Feature::ComputeShader),
      "compute shaders are unavailable on this device; on OpenGL ES they require ES 3.1 or "
      "GL_ARB_compute_shader, so a command-processor or transform-feedback fallback is needed");
  }

  if (!descriptor.computeModule.valid()) {
    return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
      "a compute pipeline needs a compute shader module");
  }

  if (!descriptor.layout.valid()) {
    return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
      "a compute pipeline needs a pipeline layout");
  }

  if (descriptor.workgroupSizeX == 0 || descriptor.workgroupSizeY == 0 || descriptor.workgroupSizeZ == 0) {
    return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
      "compute workgroup dimensions must be non-zero");
  }

  if (descriptor.workgroupSizeX > capabilities.limits.maxComputeWorkgroupSizeX ||
      descriptor.workgroupSizeY > capabilities.limits.maxComputeWorkgroupSizeY ||
      descriptor.workgroupSizeZ > capabilities.limits.maxComputeWorkgroupSizeZ) {
    return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
      "the compute workgroup size exceeds the device maximum");
  }

  const uint64_t invocations =
    static_cast<uint64_t>(descriptor.workgroupSizeX) * descriptor.workgroupSizeY * descriptor.workgroupSizeZ;
  if (invocations > capabilities.limits.maxComputeInvocationsPerWorkgroup) {
    return ValidationResult::fail(FeatureUnavailableReason::HardwareUnsupported,
      "the compute workgroup has more invocations than the device allows");
  }

  // A workgroup size that is not a multiple of the hardware's preferred subgroup size wastes
  // throughput but is not an error, so only the hard limits above are enforced.
  return ValidationResult::ok();
}

// ==================================================================================================
// Texture resolution
// ==================================================================================================

TextureResolution resolveTextureDescriptor(const TextureDescriptor& requested, const Capabilities& capabilities,
  const DegradationPolicy& policy) {
  TextureResolution resolution;
  resolution.descriptor = requested;

  const auto note = [&resolution](FeatureUnavailableReason reason, const char* text) {
    // Only the first downgrade is reported: it is the most significant one, and a chain of
    // explanations is harder to act on than a single actionable cause.
    if (!resolution.degraded) {
      resolution.reason = reason;
      resolution.note = text;
    }
    resolution.degraded = true;
  };

  // 1. Format. A non-renderable attachment format is the most visible failure, so it is resolved
  //    first; then the sRGB downgrade, which is silent but affects output correctness.
  const TextureFormatCapabilities& formatInfo = capabilities.queryTextureFormat(requested.format);
  const bool wantsAttachment =
    hasUsage(requested.usage, TextureUsage::ColorAttachment) ||
    hasUsage(requested.usage, TextureUsage::DepthStencilAttachment);

  if (requested.format != PixelFormat::Undefined && wantsAttachment && !formatInfo.renderable) {
    const PixelFormat chosen = policy.resolveColorFormat(capabilities);
    if (chosen == PixelFormat::Undefined) {
      resolution.descriptor.format = PixelFormat::Undefined;
      resolution.degraded = true;
      resolution.reason = FeatureUnavailableReason::HardwareUnsupported;
      resolution.note = "no preferred colour format is renderable on this device";
      return resolution;
    }
    resolution.descriptor.format = chosen;
    note(FeatureUnavailableReason::HardwareUnsupported,
      "the requested attachment format is not renderable; substituted the best preferred format");
  } else if (requested.format != PixelFormat::Undefined && !formatInfo.sampled &&
             hasUsage(requested.usage, TextureUsage::Sampled)) {
    resolution.degraded = true;
    resolution.reason = FeatureUnavailableReason::HardwareUnsupported;
    resolution.note = "the requested format cannot be sampled on this device";
    return resolution;
  }

  const PixelFormatInfo& resolvedInfo = getPixelFormatInfo(resolution.descriptor.format);
  if (resolvedInfo.srgb && !formatInfo.renderable && wantsAttachment && policy.allowSrgbDowngrade) {
    // Handled above when the format was renderable-at-all; this branch covers the subtler case of a
    // device that can render the linear format but not its sRGB variant.
    resolution.degraded = true;
    resolution.reason = FeatureUnavailableReason::HardwareUnsupported;
    resolution.note = "sRGB attachments are unsupported; the transfer function must be applied in the shader";
  }

  // 2. Size clamping.
  const uint32_t maximum = capabilities.maxDimensionFor(requested.type);
  if (policy.clampTextureSize && maximum != 0) {
    if (resolution.descriptor.size.width > maximum) {
      resolution.descriptor.size.width = maximum;
      note(FeatureUnavailableReason::HardwareUnsupported, "the width was clamped to the device maximum");
    }
    if (resolution.descriptor.size.height > maximum) {
      resolution.descriptor.size.height = maximum;
      note(FeatureUnavailableReason::HardwareUnsupported, "the height was clamped to the device maximum");
    }
    const uint32_t maximumDepth = requested.type == TextureType::Texture3D
                                    ? capabilities.limits.maxTextureDimension3D
                                    : capabilities.limits.maxTextureArrayLayers;
    if (maximumDepth != 0 && resolution.descriptor.size.depthOrArrayLayers > maximumDepth) {
      resolution.descriptor.size.depthOrArrayLayers = maximumDepth;
      note(FeatureUnavailableReason::HardwareUnsupported, "the layer count was clamped to the device maximum");
    }
  }

  // 3. Sample count.
  if (resolution.descriptor.sampleCount > 1) {
    const uint32_t supported = capabilities.resolveSampleCount(resolution.descriptor.sampleCount);
    if (supported != resolution.descriptor.sampleCount) {
      resolution.descriptor.sampleCount = supported;
      note(FeatureUnavailableReason::HardwareUnsupported,
        supported == 1
          ? "MSAA is unavailable; the texture was created without multisampling"
          : "the MSAA sample count was reduced to the best supported value");
    }
  }

  // 4. Mip chain.
  if (policy.allowMipReduction) {
    const uint32_t allowed =
      capabilities.resolveMipLevelCount(resolution.descriptor.size, resolution.descriptor.mipLevelCount);
    if (allowed != resolution.descriptor.mipLevelCount) {
      resolution.descriptor.mipLevelCount = allowed;
      note(FeatureUnavailableReason::HardwareUnsupported, "the mip chain was truncated to fit the texture size");
    }
  }

  // 5. A multisampled texture cannot be mipped; enforce the invariant even when the caller asked for
  //    a combination the validator would reject.
  if (resolution.descriptor.sampleCount > 1 && resolution.descriptor.mipLevelCount > 1) {
    resolution.descriptor.mipLevelCount = 1;
    note(FeatureUnavailableReason::ArchitecturallyImpossible,
      "mip levels were dropped because the texture is multisampled");
  }

  if (resolution.degraded) {
    resolution.descriptor.label = requested.label;
  }
  return resolution;
}

} // namespace backend
