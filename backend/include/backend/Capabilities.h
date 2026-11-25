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

#ifndef CORAL_BACKEND_CAPABILITIES_H
#define CORAL_BACKEND_CAPABILITIES_H

#include <backend/Descriptors.h>
#include <backend/Types.h>

#include <cstdint>
#include <string>

namespace backend {

/** Largest number of PixelFormat enumerators the capability table can describe. */
inline constexpr uint32_t kMaxPixelFormats = 128;
/** Largest sample count the capability tables track. */
inline constexpr uint32_t kMaxSampleCount = 8;

/**
 * Discrete hardware/API features. Each one is a promise that the corresponding RHI entry point will
 * work; when the bit is absent the upper layer must take a fallback path or disable the effect.
 *
 * The set is intentionally small and semantic rather than a mirror of Vulkan's feature structs: the
 * RHI only needs to answer questions the renderer actually asks. Feature parity between backends is
 * documented in docs/backend/BackendMatrix.md.
 */
enum class Feature : uint32_t {
  None = 0,

  // --- Compute ----------------------------------------------------------------------------------
  /** Compute pipelines and dispatch. OpenGL ES 3.1+, not 3.0. */
  ComputeShader = 1u << 0,
  /** Atomic operations on storage buffer/image data inside compute shaders. */
  ComputeShaderAtomics = 1u << 1,
  /** Indirect dispatch arguments. */
  IndirectDispatch = 1u << 2,
  /** Multiple compute workgroups can share groupshared memory with explicit barriers. */
  ComputeWorkgroupBarrier = 1u << 3,

  // --- Geometry stages --------------------------------------------------------------------------
  /** Geometry shader stage. Desktop GL / OpenGL 3.2 / D3D / Vulkan, not Metal. */
  GeometryShader = 1u << 4,
  /** Tessellation control + evaluation stages. */
  TessellationShader = 1u << 5,

  // --- Texturing --------------------------------------------------------------------------------
  /** 3D textures as sampleable resources. */
  Texture3D = 1u << 6,
  /** Cube map textures. */
  TextureCube = 1u << 7,
  /** Array textures (texture2DArray). */
  TextureArray = 1u << 8,
  /** Cube map arrays. */
  TextureCubeArray = 1u << 9,
  /** Anisotropic filtering. */
  SamplerAnisotropy = 1u << 10,
  /** Clamp-to-border addressing, which needs a real border colour. */
  SamplerBorderClamp = 1u << 11,
  /** Sampling a texture with an explicit LOD in a fragment shader (textureLod). */
  TextureLod = 1u << 12,
  /** Separate sampler objects decoupled from textures. */
  SeparateSamplers = 1u << 13,
  /** Writing to images/storage textures from a shader. */
  StorageTexture = 1u << 14,
  /** Comparison samplers for shadow maps. */
  ComparisonSampler = 1u << 15,

  // --- Render targets and rasterisation ---------------------------------------------------------
  /** Multiple simultaneous render targets (drawBuffers). ES 3.0 guarantees at least 4. */
  MultipleRenderTargets = 1u << 16,
  /** MSAA resolve from a multisampled attachment into a single-sampled one. */
  MultisampleResolve = 1u << 17,
  /** Per-sample shading. */
  SampleRateShading = 1u << 18,
  /** Alpha-to-coverage. */
  AlphaToCoverage = 1u << 19,
  /** Non-solid fill modes (wireframe). */
  FillModeNonSolid = 1u << 20,
  /** Depth clamping instead of clipping. */
  DepthClamp = 1u << 21,
  /** Programmable blend state. */
  ProgrammableBlending = 1u << 22,
  /** Logic operations in the blend stage (glLogicOp). */
  LogicOp = 1u << 23,
  /** Signed integer vertex attributes fed straight to the shader. */
  IntegerVertexAttributes = 1u << 24,

  // --- Synchronisation --------------------------------------------------------------------------
  /** Fences/sync objects the host can wait on with a timeout. */
  HostWaitFence = 1u << 25,
  /** GPU-side timeline semaphores. */
  TimelineSemaphore = 1u << 26,
  /** Query objects for occlusion and timestamps. */
  QueryObjects = 1u << 27,
  /** Timestamp queries with meaningful resolution. */
  TimestampQuery = 1u << 28,
  /** Pipeline statistics queries. */
  PipelineStatisticsQuery = 1u << 29,

  // --- Presentation -----------------------------------------------------------------------------
  /** The backend owns a swapchain it can present into. */
  Swapchain = 1u << 30,
  /** The backend can render directly into an OS-provided surface without a copy. */
  NativeSurface = 1u << 31,
};

/** Bitmask of Feature values. */
using Features = uint32_t;

[[nodiscard]] constexpr Features operator|(Feature a, Feature b) noexcept {
  return static_cast<Features>(a) | static_cast<Features>(b);
}

[[nodiscard]] constexpr bool hasFeature(const Features set, const Feature feature) noexcept {
  return (set & static_cast<Features>(feature)) != 0;
}

/** Human readable name of @p feature, never null. */
[[nodiscard]] const char* toString(Feature feature) noexcept;

/**
 * Why a feature is missing. This is what turns a capability query from a boolean into an actionable
 * degradation decision: an effect that is missing because the GPU is too old is worth retrying on
 * another device, while one missing because the API has no equivalent never will be.
 */
enum class FeatureUnavailableReason : uint8_t {
  Available,
  /** The running API version is too low (e.g. OpenGL ES 3.0 asked for compute). */
  ApiVersionTooLow,
  /** The driver reports no support for the required extension. */
  ExtensionMissing,
  /** The hardware lacks the capability, but a newer API version would expose it. */
  HardwareUnsupported,
  /** The API has no equivalent concept; a different rendering technique is required. */
  ArchitecturallyImpossible,
  /** The backend has not implemented it yet. */
  NotImplementedByBackend,
  /** Disabled by configuration, e.g. a reduced-feature build. */
  DisabledByConfiguration,
};

[[nodiscard]] const char* toString(FeatureUnavailableReason reason) noexcept;

/**
 * The complete capability description of a device.
 *
 * Upper layers are expected to branch on exactly two things: `features` for coarse technique
 * selection, and `queryTextureFormat()` for per-format decisions. Everything else is advisory.
 */
struct Capabilities {
  /** Bitmask of supported Feature values. */
  Features features = 0;

  Limits limits = {};

  /** Texture format support, indexed by PixelFormat. */
  TextureFormatCapabilities formats[kMaxPixelFormats] = {};

  /** Sample counts usable for textures and render targets (bit i set == count i supported). */
  uint32_t supportedSampleCounts = 0;

  /** Maximum anisotropy, or 1 when Feature::SamplerAnisotropy is absent. */
  float maxSamplerAnisotropy = 1.0f;
  /** Largest supported texture dimension in any axis, regardless of texture type. */
  uint32_t maxTextureSize = 4096;
  uint32_t maxCubeMapSize = 4096;
  uint32_t maxRenderbufferSize = 4096;
  uint32_t maxViewportWidth = 4096;
  uint32_t maxViewportHeight = 4096;

  /** Number of texture image units the fragment stage can sample simultaneously. */
  uint32_t maxFragmentTextureUnits = 16;
  uint32_t maxVertexTextureUnits = 16;
  uint32_t maxCombinedTextureUnits = 32;

  /** Largest MSAA sample count the device offers, regardless of which one a caller picks. */
  uint32_t maxSamples = 1;

  /** True when driver debug output / validation layers can be enabled. */
  bool supportsDebugMarkers = false;
  /** True when the backend takes SPIR-V bytecode directly (Vulkan). */
  bool acceptsSpirv = false;
  /**
   * True when the backend compiles GLSL source itself, with no SPIR-V in between
   * (`ShaderModuleDescriptor::Language::GLSLDirect`).
   *
   * True on the OpenGL backend, which is the one whose native shading language is GLSL. False
   * everywhere else, where a shader written in GLSL has to arrive as `Language::GLSL` or as SPIR-V.
   */
  bool acceptsGlslDirect = false;
  /** True when the backend compiles shader source at runtime rather than loading bytecode. */
  bool runtimeShaderCompilation = true;
  /**
   * True when `PresentMode::Mailbox` can be **granted**, not merely requested.
   *
   * Mailbox is a real queue semantic -- at most one queued image, the newest replaces the older, no vertical-sync wait -- and
   * OpenGL has no way to express it: all it has is the swap interval, so an interval of 0 is *Immediate* (the frame may tear)
   * rather than Mailbox (the frame is complete but superseded). A GL backend therefore reports this false, and a Mailbox request
   * comes back as `PresentMode::Immediate` in `SwapchainInfo::presentMode`.
   *
   * Vulkan grants it with `VK_PRESENT_MODE_MAILBOX_KHR`, Metal with `displaySyncEnabled = false` and a drawable pool of three.
   * A boolean rather than a `Feature` bit because the feature mask is a full 32 bits.
   */
  bool mailboxPresent = false;

  /**
   * The surface's swapchain offers a wide-gamut (Display-P3) colour space.
   *
   * Reported from the surface's own format list, so it describes this display rather than the device.
   * False on GL and on any surface that only lists `SRGB_NONLINEAR`; the canvas still presents in
   * sRGB, which is why this is a capability rather than a setting.
   */
  bool displayP3Swapchain = false;

  /** The surface's swapchain offers an HDR10 (PQ) colour space. Same sourcing as `displayP3Swapchain`. */
  bool hdr10Swapchain = false;

  /** The API version string this capability set was gathered against. */
  const char* apiVersion = "";

  /**
   * Reason a feature is unavailable, or Available. Populated for every Feature so that diagnostics
   * can explain *why* a technique was disabled.
   */
  FeatureUnavailableReason reasons[32] = {};

  /** @return true when every feature in @p required is present. */
  [[nodiscard]] bool supports(const Features required) const noexcept {
    return (features & required) == required;
  }

  [[nodiscard]] bool supports(Feature feature) const noexcept {
    return (features & static_cast<Features>(feature)) != 0;
  }

  /** @return why @p feature is unavailable, or Available when it is present. */
  [[nodiscard]] FeatureUnavailableReason reasonFor(Feature feature) const noexcept;

  /**
   * Looks up per-format support. @return a default-constructed (all false) entry for an out of
   * range or Undefined format, so callers never need a bounds check.
   */
  [[nodiscard]] const TextureFormatCapabilities& queryTextureFormat(PixelFormat format) const noexcept;

  /** @return true when @p count is a usable MSAA sample count. */
  [[nodiscard]] bool supportsSampleCount(const uint32_t count) const noexcept {
    return count != 0 && count < kMaxSampleCount && (supportedSampleCounts & (1u << count)) != 0;
  }

  /**
   * Picks the largest supported sample count not exceeding @p desired, falling back to 1 (no MSAA)
   * when nothing else is available. This is the canonical degradation helper.
   */
  [[nodiscard]] uint32_t resolveSampleCount(uint32_t desired) const noexcept;

  /**
   * Clamps @p requested to a mip count the device will accept for a texture of @p size.
   * @return at least 1.
   */
  [[nodiscard]] uint32_t resolveMipLevelCount(const Extent3D& size, uint32_t requested) const noexcept;

  /** @return the largest texture dimension allowed for @p type. */
  [[nodiscard]] uint32_t maxDimensionFor(TextureType type) const noexcept;

  /**
   * Appends a multi-line human readable report of this capability set. Used by the debug overlay and
   * by crash reports; never called on a hot path.
   */
  void appendReport(std::string& out) const;
};

/**
 * Validates a texture descriptor against a capability set before the backend ever sees it.
 *
 * The RHI's degradation philosophy is "fail early and explain": rather than letting the driver
 * produce a black texture and an obscure GL error, the upper layer calls this (or lets the default
 * createTexture path call it) and receives a specific reason plus, where a sensible substitution
 * exists, a repaired descriptor.
 */
struct TextureValidationResult {
  bool valid = true;
  FeatureUnavailableReason reason = FeatureUnavailableReason::Available;
  /** Human readable explanation, empty when valid. */
  const char* message = "";

  /**
   * The descriptor with unsupported aspects downgraded: mipped, size-clamped, sample count reduced
   * to the best supported value, and the format substituted when a close equivalent exists
   * (e.g. R8G8B8A8_SRGB -> R8G8B8A8_UNORM on targets with no sRGB support).
   */
  TextureDescriptor resolved = {};

  explicit operator bool() const noexcept { return valid; }
};

/** Validates and, where possible, repairs @p descriptor against @p capabilities. */
[[nodiscard]] TextureValidationResult validateTextureDescriptor(const TextureDescriptor& descriptor,
  const Capabilities& capabilities);

/**
 * Chooses the best available format from @p candidates, in the caller's order of preference.
 * @return Undefined when none of the candidates is supported.
 *
 * This is the primary graceful-degradation tool for render targets: hand it a preference list
 * (e.g. R16G16B16A16_SFLOAT, then R11G11B10_SFLOAT, then R8G8B8A8_UNORM) and it selects the best
 * one this device can actually render into.
 */
[[nodiscard]] PixelFormat chooseFormat(const Capabilities& capabilities, const PixelFormat* candidates,
  uint32_t candidateCount, bool renderableOnly);

/**
 * Small helper that captures a backend's capabilities by construction, so the OpenGL backend can
 * describe itself declaratively instead of through a wall of if-statements at the call site.
 */
class CapabilityBuilder {
public:
  explicit CapabilityBuilder(Capabilities& capabilities) noexcept
    : mCapabilities(capabilities) {
  }

  /** Marks @p feature supported, or records @p reason for why it is not. */
  CapabilityBuilder& feature(Feature feature, bool supported,
    FeatureUnavailableReason reason = FeatureUnavailableReason::HardwareUnsupported) noexcept;

  CapabilityBuilder& limit(const Limits& limits) noexcept;
  CapabilityBuilder& sampleCount(uint32_t count) noexcept;
  CapabilityBuilder& textureFormat(PixelFormat format, TextureFormatCapabilities support) noexcept;
  CapabilityBuilder& apiVersion(const char* version) noexcept;

private:
  Capabilities& mCapabilities;
};

} // namespace backend

#endif // CORAL_BACKEND_CAPABILITIES_H
