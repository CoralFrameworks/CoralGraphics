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

#include <string>

using namespace backend;

namespace {

/**
 * Builds a capability set that describes an OpenGL ES 3.0 class device: no compute, no storage
 * textures, 4x MSAA, a handful of colour formats. This is the baseline the degradation tests are
 * written against, and it is constructed rather than queried so the tests do not need a GPU.
 */
Capabilities makeES30Capabilities() {
  Capabilities capabilities;
  CapabilityBuilder builder(capabilities);
  builder.apiVersion("OpenGL ES 3.0");

  builder.feature(Feature::TextureCube, true)
          .feature(Feature::Texture3D, true)
          .feature(Feature::TextureArray, true)
          .feature(Feature::MultipleRenderTargets, true)
          .feature(Feature::MultisampleResolve, true)
          .feature(Feature::ComparisonSampler, true)
          // Everything a 3.0 baseline cannot do, with the reason an upper layer can act on.
          .feature(Feature::ComputeShader, false, FeatureUnavailableReason::ApiVersionTooLow)
          .feature(Feature::StorageTexture, false, FeatureUnavailableReason::ApiVersionTooLow)
          .feature(Feature::GeometryShader, false, FeatureUnavailableReason::ApiVersionTooLow)
          .feature(Feature::TessellationShader, false, FeatureUnavailableReason::ApiVersionTooLow)
          .feature(Feature::SamplerAnisotropy, false, FeatureUnavailableReason::ExtensionMissing)
          .feature(Feature::TextureCubeArray, false, FeatureUnavailableReason::ApiVersionTooLow)
          .feature(Feature::FillModeNonSolid, false, FeatureUnavailableReason::ArchitecturallyImpossible)
          .feature(Feature::TimelineSemaphore, false, FeatureUnavailableReason::ArchitecturallyImpossible)
          .feature(Feature::TimestampQuery, false, FeatureUnavailableReason::ExtensionMissing)
          .feature(Feature::TextureLod, true)
          .feature(Feature::SampleRateShading, false, FeatureUnavailableReason::ExtensionMissing)
          .feature(Feature::AlphaToCoverage, true)
          .feature(Feature::IntegerVertexAttributes, true)
          .feature(Feature::HostWaitFence, true)
          .feature(Feature::QueryObjects, true)
          .feature(Feature::Swapchain, true)
          .feature(Feature::NativeSurface, true)
          .feature(Feature::DepthClamp, false, FeatureUnavailableReason::ArchitecturallyImpossible);

  builder.sampleCount(1).sampleCount(2).sampleCount(4);
  capabilities.maxSamples = 4;

  const auto colour = [](bool srgb) {
    TextureFormatCapabilities support;
    support.sampled = true;
    support.renderable = !srgb; // deliberately absent on this fake device
    support.blendable = !srgb;
    support.sampleCounts[1] = true;
    support.sampleCounts[2] = true;
    support.sampleCounts[4] = true;
    return support;
  };

  builder.textureFormat(PixelFormat::R8G8B8A8_UNORM, colour(false));
  builder.textureFormat(PixelFormat::R8G8B8A8_SRGB, colour(true));
  builder.textureFormat(PixelFormat::R16G16B16A16_SFLOAT, colour(false));
  builder.textureFormat(PixelFormat::R11G11B10_SFLOAT, colour(false));

  TextureFormatCapabilities sampledOnly;
  sampledOnly.sampled = true;
  sampledOnly.sampleCounts[1] = true;
  builder.textureFormat(PixelFormat::ETC2_RGB8_UNORM, sampledOnly);
  builder.textureFormat(PixelFormat::R8G8B8_UNORM, sampledOnly);

  capabilities.limits.maxTextureDimension2D = 4096;
  capabilities.limits.maxTextureDimension3D = 256;
  capabilities.limits.maxTextureDimensionCube = 4096;
  capabilities.limits.maxTextureArrayLayers = 256;
  capabilities.limits.maxColorAttachments = 4;
  capabilities.limits.maxUniformBufferBindingSize = 16384;
  capabilities.limits.maxSampledTexturesPerStage = 16;
  capabilities.limits.maxSamplersPerStage = 16;
  capabilities.limits.maxUniformBuffersPerStage = 12;
  capabilities.limits.maxStorageBuffersPerStage = 0;
  capabilities.limits.maxBoundDescriptorSets = 4;
  capabilities.limits.maxVertexAttributes = 16;
  capabilities.limits.maxVertexBuffers = 8;
  capabilities.maxTextureSize = 4096;

  return capabilities;
}

TEST(Capabilities, FeatureQueryAndReasons) {
  const Capabilities capabilities = makeES30Capabilities();

  EXPECT_TRUE(capabilities.supports(Feature::TextureCube));
  EXPECT_FALSE(capabilities.supports(Feature::ComputeShader));

  EXPECT_EQ(capabilities.reasonFor(Feature::ComputeShader), FeatureUnavailableReason::ApiVersionTooLow);
  EXPECT_EQ(capabilities.reasonFor(Feature::SamplerAnisotropy), FeatureUnavailableReason::ExtensionMissing);
  EXPECT_EQ(capabilities.reasonFor(Feature::FillModeNonSolid), FeatureUnavailableReason::ArchitecturallyImpossible);
  EXPECT_EQ(capabilities.reasonFor(Feature::TextureCube), FeatureUnavailableReason::Available);
}

TEST(Capabilities, MultiFeatureRequirement) {
  const Capabilities capabilities = makeES30Capabilities();

  EXPECT_TRUE(capabilities.supports(Features(Feature::TextureCube) | Features(Feature::TextureArray)));
  EXPECT_FALSE(capabilities.supports(Features(Feature::TextureCube) | Features(Feature::ComputeShader)));

  // A zero requirement is trivially satisfied, which is what lets DeviceConfiguration default
  // requiredFeatures to 0 and mean "no hard requirements".
  EXPECT_TRUE(capabilities.supports(Features{ 0 }));
}

TEST(Capabilities, TextureFormatQueryIsBoundsSafe) {
  const Capabilities capabilities = makeES30Capabilities();

  EXPECT_TRUE(capabilities.queryTextureFormat(PixelFormat::R8G8B8A8_UNORM).renderable);
  EXPECT_TRUE(capabilities.queryTextureFormat(PixelFormat::R8G8B8A8_UNORM).sampled);
  EXPECT_FALSE(capabilities.queryTextureFormat(PixelFormat::R8G8B8A8_UNORM).storage);

  // Formats that were never declared report all-false rather than reading past the table.
  EXPECT_FALSE(capabilities.queryTextureFormat(PixelFormat::BC7_RGBA_UNORM).sampled);
  EXPECT_FALSE(capabilities.queryTextureFormat(PixelFormat::Undefined).sampled);
  const auto bogus = static_cast<PixelFormat>(0xFFFF);
  EXPECT_FALSE(capabilities.queryTextureFormat(bogus).sampled);
}

TEST(Capabilities, SampleCountResolution) {
  const Capabilities capabilities = makeES30Capabilities();

  EXPECT_TRUE(capabilities.supportsSampleCount(1));
  EXPECT_TRUE(capabilities.supportsSampleCount(2));
  EXPECT_TRUE(capabilities.supportsSampleCount(4));
  EXPECT_FALSE(capabilities.supportsSampleCount(8));

  EXPECT_EQ(capabilities.resolveSampleCount(4), 4u);
  EXPECT_EQ(capabilities.resolveSampleCount(2), 2u);

  // A request above the maximum degrades to the highest supported value rather than failing, and a
  // request of 1 stays 1.
  EXPECT_EQ(capabilities.resolveSampleCount(8), 4u);
  EXPECT_EQ(capabilities.resolveSampleCount(1), 1u);
  EXPECT_EQ(capabilities.resolveSampleCount(0), 1u) << "0 is not a valid sample count";
}

TEST(Capabilities, MipLevelResolution) {
  const Capabilities capabilities = makeES30Capabilities();

  // A 256x256 texture can have 9 levels: 256, 128, ..., 1.
  EXPECT_EQ(capabilities.resolveMipLevelCount(Extent3D{ 256, 256, 1 }, 9), 9u);
  // Asking for more than the size permits is clamped, because a longer chain cannot be allocated.
  EXPECT_EQ(capabilities.resolveMipLevelCount(Extent3D{ 256, 256, 1 }, 20), 9u);
  // Asking for 0 means "the full chain".
  EXPECT_EQ(capabilities.resolveMipLevelCount(Extent3D{ 256, 256, 1 }, 0), 9u);
  EXPECT_EQ(capabilities.resolveMipLevelCount(Extent3D{ 1, 1, 1 }, 0), 1u);
  EXPECT_EQ(capabilities.resolveMipLevelCount(Extent3D{ 0, 0, 0 }, 5), 1u);
  EXPECT_EQ(capabilities.resolveMipLevelCount(Extent3D{ 4, 4, 1 }, 1), 1u);
}

TEST(Capabilities, MaxDimensionPerTextureType) {
  const Capabilities capabilities = makeES30Capabilities();

  EXPECT_EQ(capabilities.maxDimensionFor(TextureType::Texture2D), 4096u);
  EXPECT_EQ(capabilities.maxDimensionFor(TextureType::Texture3D), 256u);
  EXPECT_EQ(capabilities.maxDimensionFor(TextureType::TextureCube), 4096u);
  EXPECT_EQ(capabilities.maxDimensionFor(TextureType::Texture2DArray), 4096u);
}

TEST(Capabilities, DegradationPolicyPicksBestRenderableFormat) {
  DegradationPolicy policy;
  const Capabilities capabilities = makeES30Capabilities();

  // The default preference list starts with R16G16B16A16_SFLOAT, which this device can render to.
  EXPECT_EQ(policy.resolveColorFormat(capabilities), PixelFormat::R16G16B16A16_SFLOAT);

  // Remove the first two preferences and the policy must fall through to the 8 bit UNORM entry.
  DegradationPolicy reduced;
  reduced.preferredColorFormats[0] = PixelFormat::D32_SFLOAT;      // not renderable
  reduced.preferredColorFormats[1] = PixelFormat::ETC2_RGB8_UNORM; // not renderable
  reduced.preferredColorFormats[2] = PixelFormat::R8G8B8A8_UNORM;
  reduced.preferredColorFormats[3] = PixelFormat::Undefined;
  EXPECT_EQ(reduced.resolveColorFormat(capabilities), PixelFormat::R8G8B8A8_UNORM);

  // Nothing renderable at all must report Undefined rather than guessing.
  DegradationPolicy impossible;
  impossible.preferredColorFormats[0] = PixelFormat::ETC2_RGB8_UNORM;
  impossible.preferredColorFormats[1] = PixelFormat::Undefined;
  EXPECT_EQ(impossible.resolveColorFormat(capabilities), PixelFormat::Undefined);
}

TEST(Capabilities, QualityTierReflectsWhatWasLost) {
  const Capabilities capabilities = makeES30Capabilities();

  // The default policy asks for a half-float target at 4x MSAA, both of which are available.
  DegradationPolicy full;
  EXPECT_EQ(full.qualityTier(capabilities), DegradationPolicy::Tier::High);

  // Landing on the 8 bit UNORM fallback is the compatibility tier.
  DegradationPolicy low;
  low.preferredColorFormats[0] = PixelFormat::ETC2_RGB8_UNORM; // not renderable
  low.preferredColorFormats[1] = PixelFormat::R8G8B8A8_UNORM;
  low.preferredColorFormats[2] = PixelFormat::Undefined;
  low.preferredSampleCounts[0] = 1;
  EXPECT_EQ(low.qualityTier(capabilities), DegradationPolicy::Tier::Low);

  // Asking for a compressed-only format and being handed an 11/11/10 render target instead is a real
  // loss of precision, but it is still a quality path rather than a compatibility one.
  DegradationPolicy medium;
  medium.preferredColorFormats[0] = PixelFormat::ETC2_RGB8_UNORM; // sampled only, not renderable
  medium.preferredColorFormats[1] = PixelFormat::R11G11B10_SFLOAT;
  medium.preferredColorFormats[2] = PixelFormat::Undefined;
  EXPECT_EQ(medium.qualityTier(capabilities), DegradationPolicy::Tier::Medium);

  // Getting the first preference is High by definition: nothing was lost, even if the preference
  // itself is modest.
  DegradationPolicy satisfied;
  satisfied.preferredColorFormats[0] = PixelFormat::R11G11B10_SFLOAT;
  satisfied.preferredColorFormats[1] = PixelFormat::Undefined;
  EXPECT_EQ(satisfied.qualityTier(capabilities), DegradationPolicy::Tier::High);

  // A float target with no MSAA available drops to the compatibility tier.
  DegradationPolicy noMsaa;
  noMsaa.preferredColorFormats[0] = PixelFormat::ETC2_RGB8_UNORM;
  noMsaa.preferredColorFormats[1] = PixelFormat::R11G11B10_SFLOAT;
  noMsaa.preferredColorFormats[2] = PixelFormat::Undefined;
  noMsaa.preferredSampleCounts[0] = 1;
  EXPECT_EQ(noMsaa.qualityTier(capabilities), DegradationPolicy::Tier::Low);
}

TEST(Capabilities, DegradationPolicySampleCount) {
  const Capabilities capabilities = makeES30Capabilities();

  DegradationPolicy policy;
  EXPECT_EQ(policy.resolveSampleCount(capabilities), 4u);

  DegradationPolicy conservative;
  conservative.preferredSampleCounts[0] = 8; // unsupported
  conservative.preferredSampleCounts[1] = 2;
  conservative.preferredSampleCounts[2] = 1;
  EXPECT_EQ(conservative.resolveSampleCount(capabilities), 2u);

  DegradationPolicy none;
  none.preferredSampleCounts[0] = 1;
  EXPECT_EQ(none.resolveSampleCount(capabilities), 1u) << "1 means no MSAA and is always valid";
}

TEST(Capabilities, ReportMentionsBothSupportedAndUnsupportedFeatures) {
  const Capabilities capabilities = makeES30Capabilities();

  std::string report;
  capabilities.appendReport(report);

  EXPECT_NE(report.find("TextureCube"), std::string::npos);
  EXPECT_NE(report.find("ComputeShader"), std::string::npos);
  // The report is the mechanism by which a disabled effect is explained, so it must name the reason.
  EXPECT_NE(report.find("API version too low"), std::string::npos);
  EXPECT_NE(report.find("no equivalent in this API"), std::string::npos);
  EXPECT_NE(report.find("Sample counts"), std::string::npos);
}

TEST(Capabilities, ReasonLookupForUnsetFeatureIsNeverAvailable) {
  // A capability set where nothing was recorded must still answer with a usable reason rather than
  // claiming the feature is available.
  Capabilities capabilities;
  EXPECT_FALSE(capabilities.supports(Feature::ComputeShader));
  EXPECT_NE(capabilities.reasonFor(Feature::ComputeShader), FeatureUnavailableReason::Available);

  // Feature::None is a sentinel, not a real feature, and must not be reported as available.
  EXPECT_FALSE(capabilities.supports(Feature::None));
}

} // namespace anonymous
