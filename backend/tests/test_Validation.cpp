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

#include <backend/Validation.h>

using namespace backend;

namespace {

/** Minimal capability set standing in for an OpenGL ES 3.0 device. */
Capabilities makeCapabilities() {
  Capabilities capabilities;
  CapabilityBuilder builder(capabilities);
  builder.apiVersion("OpenGL ES 3.0")
          .feature(Feature::TextureCube, true)
          .feature(Feature::Texture3D, true)
          .feature(Feature::TextureArray, true)
          .feature(Feature::MultipleRenderTargets, true)
          .feature(Feature::MultisampleResolve, true)
          .feature(Feature::IntegerVertexAttributes, true)
          .feature(Feature::AlphaToCoverage, true)
          .feature(Feature::ComparisonSampler, true)
          .feature(Feature::SamplerAnisotropy, true)
          .feature(Feature::ComputeShader, false, FeatureUnavailableReason::ApiVersionTooLow)
          .feature(Feature::StorageTexture, false, FeatureUnavailableReason::ApiVersionTooLow)
          .feature(Feature::TessellationShader, false, FeatureUnavailableReason::ApiVersionTooLow)
          .feature(Feature::GeometryShader, false, FeatureUnavailableReason::ApiVersionTooLow)
          .feature(Feature::FillModeNonSolid, false, FeatureUnavailableReason::ArchitecturallyImpossible)
          .feature(Feature::DepthClamp, false, FeatureUnavailableReason::ArchitecturallyImpossible)
          .feature(Feature::SamplerBorderClamp, false, FeatureUnavailableReason::ExtensionMissing)
          .feature(Feature::ProgrammableBlending, false, FeatureUnavailableReason::ArchitecturallyImpossible);

  builder.sampleCount(1).sampleCount(2).sampleCount(4);
  capabilities.maxSamples = 4;
  capabilities.maxSamplerAnisotropy = 8.0f;

  const auto colour = [] {
    TextureFormatCapabilities support;
    support.sampled = true;
    support.renderable = true;
    support.blendable = true;
    support.sampleCounts[1] = true;
    support.sampleCounts[2] = true;
    support.sampleCounts[4] = true;
    return support;
  };
  builder.textureFormat(PixelFormat::R8G8B8A8_UNORM, colour());
  builder.textureFormat(PixelFormat::R16G16B16A16_SFLOAT, colour());
  builder.textureFormat(PixelFormat::R8G8B8A8_SRGB, colour());
  builder.textureFormat(PixelFormat::D24_UNORM_S8_UINT, colour());

  TextureFormatCapabilities sampledOnly;
  sampledOnly.sampled = true;
  sampledOnly.sampleCounts[1] = true;
  builder.textureFormat(PixelFormat::ETC2_RGB8_UNORM, sampledOnly);

  capabilities.limits.maxTextureDimension2D = 4096;
  capabilities.limits.maxTextureDimension3D = 256;
  capabilities.limits.maxTextureDimensionCube = 4096;
  capabilities.limits.maxTextureArrayLayers = 256;
  capabilities.limits.maxBufferSize = 64u * 1024u * 1024u;
  capabilities.limits.maxUniformBufferBindingSize = 16384;
  capabilities.limits.maxStorageBufferBindingSize = 0;
  capabilities.limits.maxColorAttachments = 4;
  capabilities.limits.maxVertexBuffers = 8;
  capabilities.limits.maxVertexAttributes = 16;
  capabilities.limits.maxVertexBufferStride = 2048;
  capabilities.limits.maxSampledTexturesPerStage = 16;
  capabilities.limits.maxSamplersPerStage = 16;
  capabilities.limits.maxUniformBuffersPerStage = 12;
  capabilities.limits.maxStorageBuffersPerStage = 0;
  capabilities.limits.maxComputeWorkgroupSizeX = 128;
  capabilities.limits.maxComputeWorkgroupSizeY = 128;
  capabilities.limits.maxComputeWorkgroupSizeZ = 64;
  capabilities.limits.maxComputeInvocationsPerWorkgroup = 128;
  return capabilities;
}

/** A pipeline layout handle only needs to be non-zero to pass the "layout required" check. */
PipelineLayoutHandle dummyLayout() { return PipelineLayoutHandle(0, 1); }
ShaderModuleHandle dummyModule() { return ShaderModuleHandle(0, 1); }

TEST(Validation, BufferDescriptorChecks) {
  const Capabilities capabilities = makeCapabilities();

  auto descriptor = BufferDescriptor::vertex(1024);
  EXPECT_TRUE(validateBufferDescriptor(descriptor, capabilities).valid);

  // Zero size is always a bug: nothing can be bound or read.
  descriptor.size = 0;
  const ValidationResult zeroSize = validateBufferDescriptor(descriptor, capabilities);
  EXPECT_FALSE(zeroSize.valid);
  EXPECT_NE(zeroSize.message, nullptr);

  // No usage flag means nothing can legally access the buffer.
  BufferDescriptor noUsage;
  noUsage.size = 256;
  noUsage.usage = 0;
  EXPECT_FALSE(validateBufferDescriptor(noUsage, capabilities).valid);

  // Oversized buffers are rejected rather than truncated, because a truncated buffer would produce
  // out-of-range reads.
  BufferDescriptor huge;
  huge.size = uint64_t(capabilities.limits.maxBufferSize) + 1;
  huge.usage = static_cast<BufferUsages>(BufferUsage::Vertex);
  EXPECT_FALSE(validateBufferDescriptor(huge, capabilities).valid);

  // A uniform buffer may be larger than the maximum block size: the limit applies to the *range* a descriptor names, not
  // to the buffer that holds it. A uniform arena of many small blocks is the shape that needs this.
  BufferDescriptor bigUniform;
  bigUniform.size = uint64_t(capabilities.limits.maxUniformBufferBindingSize) * 4u;
  bigUniform.usage = static_cast<BufferUsages>(BufferUsage::Uniform);
  EXPECT_TRUE(validateBufferDescriptor(bigUniform, capabilities).valid);

  // The range, however, must fit -- and the check names the binding rather than the buffer now.
  DescriptorWrite bigRange;
  bigRange.type = BindingType::UniformBuffer;
  bigRange.buffer.size = uint64_t(capabilities.limits.maxUniformBufferBindingSize) + 1;
  EXPECT_FALSE(validateDescriptorWrite(bigRange, capabilities).valid);
  bigRange.buffer.size = capabilities.limits.maxUniformBufferBindingSize;
  EXPECT_TRUE(validateDescriptorWrite(bigRange, capabilities).valid);
  // `size == 0` is "to the end of the buffer", whose length this check cannot know.
  bigRange.buffer.size = 0;
  EXPECT_TRUE(validateDescriptorWrite(bigRange, capabilities).valid);

  // Storage buffers need compute-class features that this device lacks.
  BufferDescriptor storage;
  storage.size = 1024;
  storage.usage = static_cast<BufferUsages>(BufferUsage::Storage);
  const ValidationResult storageResult = validateBufferDescriptor(storage, capabilities);
  EXPECT_FALSE(storageResult.valid);
  EXPECT_EQ(storageResult.reason, FeatureUnavailableReason::ApiVersionTooLow);

  // An index buffer that claims to be written by the GPU is a contradiction.
  BufferDescriptor badIndex;
  badIndex.size = 1024;
  badIndex.usage = static_cast<BufferUsages>(BufferUsage::Index);
  badIndex.memoryUsage = MemoryUsage::GpuToCpu;
  EXPECT_FALSE(validateBufferDescriptor(badIndex, capabilities).valid);
}

TEST(Validation, SamplerDescriptorChecks) {
  const Capabilities capabilities = makeCapabilities();

  SamplerDescriptor sampler = SamplerDescriptor::linearClamp();
  EXPECT_TRUE(validateSamplerDescriptor(sampler, capabilities).valid);

  // Anisotropy is available but capped; a request beyond the cap must be reported so the caller can
  // clamp it deliberately instead of the driver silently ignoring it.
  sampler.maxAnisotropy = 4.0f;
  EXPECT_TRUE(validateSamplerDescriptor(sampler, capabilities).valid);
  sampler.maxAnisotropy = 16.0f;
  const ValidationResult tooMuch = validateSamplerDescriptor(sampler, capabilities);
  EXPECT_FALSE(tooMuch.valid);
  EXPECT_EQ(tooMuch.reason, FeatureUnavailableReason::HardwareUnsupported);

  // On a device without the extension, any anisotropy at all is a failure with a different reason.
  Capabilities noAnisotropy = makeCapabilities();
  CapabilityBuilder(noAnisotropy)
          .feature(Feature::SamplerAnisotropy, false, FeatureUnavailableReason::ExtensionMissing);
  noAnisotropy.maxSamplerAnisotropy = 1.0f;
  SamplerDescriptor anisotropic = SamplerDescriptor::linearClamp();
  anisotropic.maxAnisotropy = 2.0f;
  const ValidationResult noAniso = validateSamplerDescriptor(anisotropic, noAnisotropy);
  EXPECT_FALSE(noAniso.valid);
  EXPECT_EQ(noAniso.reason, FeatureUnavailableReason::ExtensionMissing);

  // Clamp-to-border needs its own extension on this device.
  SamplerDescriptor border = SamplerDescriptor::linearClamp();
  border.addressModeU = SamplerAddressMode::ClampToBorder;
  const ValidationResult borderResult = validateSamplerDescriptor(border, capabilities);
  EXPECT_FALSE(borderResult.valid);
  EXPECT_EQ(borderResult.reason, FeatureUnavailableReason::ExtensionMissing);

  // A comparison sampler must declare itself as one, and the lod range must be ordered.
  SamplerDescriptor compareMismatch = SamplerDescriptor::linearClamp();
  compareMismatch.depthCompare = true;
  EXPECT_FALSE(validateSamplerDescriptor(compareMismatch, capabilities).valid);

  SamplerDescriptor badLod = SamplerDescriptor::linearClamp();
  badLod.lodMinClamp = 5.0f;
  badLod.lodMaxClamp = 1.0f;
  EXPECT_FALSE(validateSamplerDescriptor(badLod, capabilities).valid);
}

TEST(Validation, TextureDescriptorRepairsSrgbAttachments) {
  const Capabilities capabilities = makeCapabilities();

  // Fully supported: nothing to repair.
  const TextureDescriptor good = TextureDescriptor::texture2D(256, 256, PixelFormat::R16G16B16A16_SFLOAT,
          static_cast<TextureUsages>(TextureUsage::Sampled | TextureUsage::ColorAttachment));
  const TextureValidationResult goodResult = validateTextureDescriptor(good, capabilities);
  EXPECT_TRUE(goodResult.valid);

  // An Undefined format cannot be guessed at.
  TextureDescriptor undefined = good;
  undefined.format = PixelFormat::Undefined;
  EXPECT_FALSE(validateTextureDescriptor(undefined, capabilities).valid);

  // A sampled-only format used as an attachment must be refused with a usable message.
  TextureDescriptor notRenderable = good;
  notRenderable.format = PixelFormat::ETC2_RGB8_UNORM;
  const TextureValidationResult attachmentResult = validateTextureDescriptor(notRenderable, capabilities);
  EXPECT_FALSE(attachmentResult.valid);
  EXPECT_NE(attachmentResult.message[0], '\0');

  // Oversized textures are reported rather than silently clamped by the validator; clamping is the
  // degradation policy's job, which keeps the decision with the caller.
  TextureDescriptor oversized = good;
  oversized.size.width = 8192;
  EXPECT_FALSE(validateTextureDescriptor(oversized, capabilities).valid);

  // A mip chain longer than the texture permits is truncated.
  TextureDescriptor tooManyMips = good;
  tooManyMips.mipLevelCount = 20;
  const TextureValidationResult mipResult = validateTextureDescriptor(tooManyMips, capabilities);
  EXPECT_FALSE(mipResult.valid);
  EXPECT_LT(mipResult.resolved.mipLevelCount, 20u);

  // An unsupported MSAA count is refused with a pointer to the resolution helper.
  TextureDescriptor badSamples = good;
  badSamples.sampleCount = 8;
  EXPECT_FALSE(validateTextureDescriptor(badSamples, capabilities).valid);

  // Storage textures need compute-class support that this device lacks on both counts, so the
  // validation reports the stronger reason: the technique is not expressible, and the caller has to
  // rework the descriptor rather than wait for newer hardware.
  TextureDescriptor storage = good;
  storage.usage = static_cast<TextureUsages>(TextureUsage::Sampled | TextureUsage::Storage);
  const TextureValidationResult storageResult = validateTextureDescriptor(storage, capabilities);
  EXPECT_FALSE(storageResult.valid);
  EXPECT_EQ(storageResult.reason, FeatureUnavailableReason::ArchitecturallyImpossible);

  // A device whose backend *could* provide storage textures, but whose running API version is too
  // low, reports the softer reason so the caller knows the effect is merely disabled for now.
  Capabilities newerApi = makeCapabilities();
  CapabilityBuilder(newerApi).feature(Feature::StorageTexture, true);
  const TextureValidationResult newerApiResult = validateTextureDescriptor(storage, newerApi);
  EXPECT_FALSE(newerApiResult.valid);
  EXPECT_EQ(newerApiResult.reason, FeatureUnavailableReason::ApiVersionTooLow);
}

TEST(Validation, TextureResolutionAppliesTheDegradationPolicy) {
  const Capabilities capabilities = makeCapabilities();
  DegradationPolicy policy;

  // Oversized and over-miapped, with an unsupported sample count: every relaxable aspect must be
  // repaired in one pass rather than failing on the first problem.
  TextureDescriptor request = TextureDescriptor::texture2D(16384, 8192, PixelFormat::R16G16B16A16_SFLOAT,
          static_cast<TextureUsages>(TextureUsage::Sampled | TextureUsage::ColorAttachment));
  request.mipLevelCount = 40;
  request.sampleCount = 8;

  const TextureResolution resolution = resolveTextureDescriptor(request, capabilities, policy);
  EXPECT_TRUE(resolution.degraded);
  EXPECT_EQ(resolution.descriptor.size.width, 4096u);
  EXPECT_EQ(resolution.descriptor.size.height, 4096u);
  EXPECT_EQ(resolution.descriptor.sampleCount, 4u);
  EXPECT_EQ(resolution.descriptor.mipLevelCount, 1u) << "a multisampled texture cannot have mip levels";
  EXPECT_NE(resolution.note, nullptr);
  EXPECT_NE(resolution.note[0], '\0');

  // A descriptor that needs no repair must come back with degraded == false and the label preserved.
  TextureDescriptor perfect = TextureDescriptor::texture2D(512, 512, PixelFormat::R8G8B8A8_UNORM);
  perfect.label = "gBuffer";
  const TextureResolution untouched = resolveTextureDescriptor(perfect, capabilities, policy);
  EXPECT_FALSE(untouched.degraded);
  EXPECT_EQ(untouched.descriptor.size.width, 512u);
  EXPECT_STREQ(untouched.descriptor.label, "gBuffer");
}

TEST(Validation, DescriptorSetLayoutChecks) {
  const Capabilities capabilities = makeCapabilities();

  DescriptorSetLayoutDescriptor layout;
  layout.bindingCount = 2;
  layout.bindings[0].binding = 0;
  layout.bindings[0].type = BindingType::TextureSampler;
  layout.bindings[0].stageFlags = 1u << 1; // fragment
  layout.bindings[1].binding = 1;
  layout.bindings[1].type = BindingType::UniformBuffer;
  layout.bindings[1].stageFlags = 1u << 1;
  EXPECT_TRUE(validateDescriptorSetLayout(layout, capabilities).valid);

  // Duplicate binding numbers make the reflection-driven lookup ambiguous, so they are rejected.
  DescriptorSetLayoutDescriptor duplicate = layout;
  duplicate.bindings[1].binding = 0;
  EXPECT_FALSE(validateDescriptorSetLayout(duplicate, capabilities).valid);

  // A zero count is meaningless.
  DescriptorSetLayoutDescriptor zeroCount = layout;
  zeroCount.bindings[0].count = 0;
  EXPECT_FALSE(validateDescriptorSetLayout(zeroCount, capabilities).valid);

  // Exceeding the per-stage texture budget must be caught before the driver silently drops bindings.
  DescriptorSetLayoutDescriptor tooMany;
  tooMany.bindingCount = 2;
  tooMany.bindings[0].binding = 0;
  tooMany.bindings[0].type = BindingType::TextureSampler;
  tooMany.bindings[0].count = 20; // device provides 16
  tooMany.bindings[0].stageFlags = 1u << 1;
  tooMany.bindings[1].binding = 1;
  tooMany.bindings[1].type = BindingType::TextureSampler;
  tooMany.bindings[1].count = 1;
  tooMany.bindings[1].stageFlags = 1u << 1;
  EXPECT_FALSE(validateDescriptorSetLayout(tooMany, capabilities).valid);

  // A compute-stage binding on a device with no compute must be rejected with the API-version reason.
  DescriptorSetLayoutDescriptor computeLayout;
  computeLayout.bindingCount = 1;
  computeLayout.bindings[0].binding = 0;
  computeLayout.bindings[0].type = BindingType::UniformBuffer;
  computeLayout.bindings[0].stageFlags = 1u << 2; // compute
  const ValidationResult computeResult = validateDescriptorSetLayout(computeLayout, capabilities);
  EXPECT_FALSE(computeResult.valid);
  EXPECT_EQ(computeResult.reason, FeatureUnavailableReason::ApiVersionTooLow);
}

TEST(Validation, RenderPassDescriptorChecks) {
  const Capabilities capabilities = makeCapabilities();

  RenderPassDescriptor pass;
  pass.colorAttachmentCount = 1;
  pass.colorFormats[0] = PixelFormat::R8G8B8A8_UNORM;
  pass.depthStencilFormat = PixelFormat::D24_UNORM_S8_UINT;
  pass.sampleCount = 4;
  EXPECT_TRUE(validateRenderPassDescriptor(pass, capabilities).valid);

  // An Undefined colour format is always a caller error.
  RenderPassDescriptor undefined = pass;
  undefined.colorFormats[0] = PixelFormat::Undefined;
  EXPECT_FALSE(validateRenderPassDescriptor(undefined, capabilities).valid);

  // A sampled-only format cannot be a render target.
  RenderPassDescriptor notRenderable = pass;
  notRenderable.colorFormats[0] = PixelFormat::ETC2_RGB8_UNORM;
  EXPECT_FALSE(validateRenderPassDescriptor(notRenderable, capabilities).valid);

  // More attachments than the device supports.
  RenderPassDescriptor tooMany = pass;
  tooMany.colorAttachmentCount = 6;
  for (uint32_t index = 0; index < 6; ++index) tooMany.colorFormats[index] = PixelFormat::R8G8B8A8_UNORM;
  EXPECT_FALSE(validateRenderPassDescriptor(tooMany, capabilities).valid);

  // A colour format used as a depth attachment is a type error.
  RenderPassDescriptor badDepth = pass;
  badDepth.depthStencilFormat = PixelFormat::R8G8B8A8_UNORM;
  EXPECT_FALSE(validateRenderPassDescriptor(badDepth, capabilities).valid);

  // An unsupported sample count.
  RenderPassDescriptor badSamples = pass;
  badSamples.sampleCount = 8;
  EXPECT_FALSE(validateRenderPassDescriptor(badSamples, capabilities).valid);
}

TEST(Validation, RenderPipelineChecks) {
  const Capabilities capabilities = makeCapabilities();

  RenderPipelineDescriptor pipeline;
  pipeline.layout = dummyLayout();
  pipeline.vertexModule = dummyModule();
  pipeline.colorTargetCount = 1;
  pipeline.colorTargets[0].format = PixelFormat::R8G8B8A8_UNORM;
  pipeline.multisample.sampleCount = 1;
  EXPECT_TRUE(validateRenderPipeline(pipeline, capabilities).valid);

  // A layout is mandatory even when no descriptors are bound, because the backend needs the shape.
  RenderPipelineDescriptor noLayout = pipeline;
  noLayout.layout = PipelineLayoutHandle::invalid();
  EXPECT_FALSE(validateRenderPipeline(noLayout, capabilities).valid);

  // At least one of vertex/fragment must be present.
  RenderPipelineDescriptor noStages = pipeline;
  noStages.vertexModule = ShaderModuleHandle::invalid();
  noStages.fragmentModule = ShaderModuleHandle::invalid();
  EXPECT_FALSE(validateRenderPipeline(noStages, capabilities).valid);

  // A pipeline with no attachments at all cannot render anything.
  RenderPipelineDescriptor noTargets = pipeline;
  noTargets.colorTargetCount = 0;
  EXPECT_FALSE(validateRenderPipeline(noTargets, capabilities).valid);

  // Patch-list topology needs tessellation, which this device lacks; the message must point at the
  // alternative technique rather than just refusing.
  RenderPipelineDescriptor patches = pipeline;
  patches.rasterizer.topology = PrimitiveTopology::PatchList;
  patches.patchControlPoints = 3;
  const ValidationResult patchResult = validateRenderPipeline(patches, capabilities);
  EXPECT_FALSE(patchResult.valid);
  EXPECT_EQ(patchResult.reason, FeatureUnavailableReason::ApiVersionTooLow);

  // Wireframe is architecturally impossible here, so a different reason is reported.
  RenderPipelineDescriptor wireframe = pipeline;
  wireframe.rasterizer.wireframe = true;
  EXPECT_EQ(validateRenderPipeline(wireframe, capabilities).reason,
          FeatureUnavailableReason::ArchitecturallyImpossible);

  // Depth clamping likewise.
  RenderPipelineDescriptor clamped = pipeline;
  clamped.rasterizer.depthClampEnable = true;
  EXPECT_EQ(validateRenderPipeline(clamped, capabilities).reason, FeatureUnavailableReason::ArchitecturallyImpossible);

  // A colour target that is not renderable.
  RenderPipelineDescriptor badFormat = pipeline;
  badFormat.colorTargets[0].format = PixelFormat::ETC2_RGB8_UNORM;
  EXPECT_FALSE(validateRenderPipeline(badFormat, capabilities).valid);
}

TEST(Validation, RenderPipelineVertexInputChecks) {
  const Capabilities capabilities = makeCapabilities();

  RenderPipelineDescriptor pipeline;
  pipeline.layout = dummyLayout();
  pipeline.vertexModule = dummyModule();
  pipeline.colorTargetCount = 1;
  pipeline.colorTargets[0].format = PixelFormat::R8G8B8A8_UNORM;
  pipeline.vertexInput.bufferCount = 1;
  pipeline.vertexInput.buffers[0].stride = 32;
  pipeline.vertexInput.buffers[0].firstAttribute = 0;
  pipeline.vertexInput.buffers[0].attributeCount = 2;
  pipeline.vertexInput.attributeCount = 2;
  pipeline.vertexInput.attributes[0] = { 0, 0, VertexFormat::Float3, 0 };
  pipeline.vertexInput.attributes[1] = { 1, 0, VertexFormat::Float2, 12 };
  EXPECT_TRUE(validateRenderPipeline(pipeline, capabilities).valid);

  // A stride above the device maximum.
  RenderPipelineDescriptor wideStride = pipeline;
  wideStride.vertexInput.buffers[0].stride = 4096;
  EXPECT_FALSE(validateRenderPipeline(wideStride, capabilities).valid);

  // A buffer layout that references attributes past the end of the array.
  RenderPipelineDescriptor badRange = pipeline;
  badRange.vertexInput.buffers[0].attributeCount = 4;
  EXPECT_FALSE(validateRenderPipeline(badRange, capabilities).valid);

  // An attribute whose format is Invalid.
  RenderPipelineDescriptor badFormat = pipeline;
  badFormat.vertexInput.attributes[0].format = VertexFormat::Invalid;
  EXPECT_FALSE(validateRenderPipeline(badFormat, capabilities).valid);

  // An attribute pointing at a vertex buffer that was never declared.
  RenderPipelineDescriptor badBufferIndex = pipeline;
  badBufferIndex.vertexInput.attributes[0].bufferIndex = 3;
  EXPECT_FALSE(validateRenderPipeline(badBufferIndex, capabilities).valid);

  // Integer attributes need the feature; without it the caller must convert on the host.
  Capabilities noIntegerAttributes = makeCapabilities();
  CapabilityBuilder(noIntegerAttributes)
          .feature(Feature::IntegerVertexAttributes, false, FeatureUnavailableReason::HardwareUnsupported);
  RenderPipelineDescriptor integerAttributes = pipeline;
  integerAttributes.vertexInput.attributes[0].format = VertexFormat::UInt4;
  const ValidationResult integerResult = validateRenderPipeline(integerAttributes, noIntegerAttributes);
  EXPECT_FALSE(integerResult.valid);
  EXPECT_EQ(integerResult.reason, FeatureUnavailableReason::HardwareUnsupported);
}

TEST(Validation, RenderPipelineMustMatchItsRenderPass) {
  const Capabilities capabilities = makeCapabilities();

  RenderPassDescriptor pass;
  pass.colorAttachmentCount = 1;
  pass.colorFormats[0] = PixelFormat::R8G8B8A8_UNORM;
  pass.sampleCount = 2;

  RenderPipelineDescriptor pipeline;
  pipeline.layout = dummyLayout();
  pipeline.vertexModule = dummyModule();
  pipeline.colorTargetCount = 1;
  pipeline.colorTargets[0].format = PixelFormat::R8G8B8A8_UNORM;
  pipeline.multisample.sampleCount = 2;
  EXPECT_TRUE(validateRenderPipeline(pipeline, capabilities, &pass).valid);

  // A format mismatch must be caught at pipeline creation rather than producing undefined rendering.
  RenderPipelineDescriptor wrongFormat = pipeline;
  wrongFormat.colorTargets[0].format = PixelFormat::R16G16B16A16_SFLOAT;
  EXPECT_FALSE(validateRenderPipeline(wrongFormat, capabilities, &pass).valid);

  // A sample count mismatch is equally fatal.
  RenderPipelineDescriptor wrongSamples = pipeline;
  wrongSamples.multisample.sampleCount = 1;
  EXPECT_FALSE(validateRenderPipeline(wrongSamples, capabilities, &pass).valid);

  // So is a differing attachment count.
  RenderPipelineDescriptor wrongCount = pipeline;
  wrongCount.colorTargetCount = 2;
  wrongCount.colorTargets[1].format = PixelFormat::R8G8B8A8_UNORM;
  EXPECT_FALSE(validateRenderPipeline(wrongCount, capabilities, &pass).valid);
}

TEST(Validation, ComputePipelineIsRefusedWithoutComputeSupport) {
  const Capabilities capabilities = makeCapabilities();

  ComputePipelineDescriptor descriptor;
  descriptor.layout = dummyLayout();
  descriptor.computeModule = dummyModule();
  descriptor.workgroupSizeX = 8;
  descriptor.workgroupSizeY = 8;
  descriptor.workgroupSizeZ = 1;

  const ValidationResult result = validateComputePipeline(descriptor, capabilities);
  EXPECT_FALSE(result.valid);
  EXPECT_EQ(result.reason, FeatureUnavailableReason::ApiVersionTooLow);
  EXPECT_NE(std::string(result.message).find("3.1"), std::string::npos)
          << "the message must say what the device would need";

  // With compute present, the workgroup limits are enforced.
  Capabilities withCompute = makeCapabilities();
  CapabilityBuilder(withCompute).feature(Feature::ComputeShader, true);
  EXPECT_TRUE(validateComputePipeline(descriptor, withCompute).valid);

  ComputePipelineDescriptor tooLarge = descriptor;
  tooLarge.workgroupSizeX = 1024;
  tooLarge.workgroupSizeY = 1024;
  EXPECT_FALSE(validateComputePipeline(tooLarge, withCompute).valid);

  ComputePipelineDescriptor zeroSize = descriptor;
  zeroSize.workgroupSizeX = 0;
  EXPECT_FALSE(validateComputePipeline(zeroSize, withCompute).valid);
}

TEST(Validation, FormatChoiceFollowsPreferenceOrder) {
  const Capabilities capabilities = makeCapabilities();

  const PixelFormat candidates[] = { PixelFormat::ETC2_RGB8_UNORM, // sampled only
    PixelFormat::R16G16B16A16_SFLOAT,                              // renderable
    PixelFormat::R8G8B8A8_UNORM };
  EXPECT_EQ(chooseFormat(capabilities, candidates, 3, /*renderableOnly=*/true), PixelFormat::R16G16B16A16_SFLOAT);
  EXPECT_EQ(chooseFormat(capabilities, candidates, 3, /*renderableOnly=*/false), PixelFormat::ETC2_RGB8_UNORM);

  // An Undefined entry terminates the list, which is what lets a caller write a shorter preference
  // list in a fixed-size array.
  const PixelFormat terminated[] = { PixelFormat::Undefined, PixelFormat::R8G8B8A8_UNORM };
  EXPECT_EQ(chooseFormat(capabilities, terminated, 2, true), PixelFormat::Undefined);

  // Nothing supported at all.
  const PixelFormat unsupported[] = { PixelFormat::BC7_RGBA_UNORM, PixelFormat::Undefined };
  EXPECT_EQ(chooseFormat(capabilities, unsupported, 2, false), PixelFormat::Undefined);
}

} // namespace anonymous
