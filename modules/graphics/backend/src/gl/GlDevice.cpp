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

#include "private/backend/gl/GlDevice.h"

#include <backend/TestHooks.h>

#include "GlslCompiler.h"
#include "ShaderCross.h"
#include "private/backend/gl/GlCommandBuffer.h"
#include "private/backend/gl/GlEnums.h"

#include <algorithm>
#include <cstdio>
#include <ranges>
#include <sstream>
#include <thread>

#include <minilog/log.h>
#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace backend {

using namespace gl;

namespace {

/** Bytes a texture occupies at its declared size, mip chain and sample count. */
uint64_t glTextureBytes(const GLTextureMeta& meta) noexcept {
  const PixelFormatInfo& info = getPixelFormatInfo(meta.format);
  if (info.bytesPerBlock == 0) return 0;
  const uint64_t blockWidth = std::max<uint8_t>(1, info.blockWidth);
  const uint64_t blockHeight = std::max<uint8_t>(1, info.blockHeight);
  uint64_t bytes = 0;
  uint64_t width = std::max<uint32_t>(1, meta.width);
  uint64_t height = std::max<uint32_t>(1, meta.height);
  for (uint32_t level = 0; level < std::max<uint32_t>(1, meta.mipLevelCount); ++level) {
    bytes += ((width + blockWidth - 1) / blockWidth) * ((height + blockHeight - 1) / blockHeight) *
             info.bytesPerBlock;
    width = std::max<uint64_t>(1, width / 2);
    height = std::max<uint64_t>(1, height / 2);
  }
  bytes *= std::max<uint32_t>(1, meta.depthOrLayers);
  bytes *= std::max<uint32_t>(1, meta.sampleCount);
  return bytes;
}

/** FNV-1a, matching the hash used for GLSL source provenance. */
uint64_t hashBytes(const void* data, const size_t size) noexcept {
  uint64_t hash = 1469598103934665603ull;
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= 1099511628211ull;
  }
  return hash;
}

/**
 * Maps an RHI descriptor binding type onto the capability that gates it.
 *
 * Declared here rather than in GlEnums.h because it is a device-level policy question - which
 * feature a binding needs - rather than a vocabulary translation.
 */
Feature featureForBindingType(const BindingType type) {
  switch (type) {
    case BindingType::TextureSampler:
    case BindingType::Texture:
    case BindingType::Sampler:
    case BindingType::UniformBuffer:
      return Feature::None;
    case BindingType::ReadOnlyStorageBuffer:
    case BindingType::StorageBuffer:
      return Feature::ComputeShader;
  }
  return Feature::None;
}

/**
 * Maps a reflected binding type onto the public one.
 *
 * @return false when the binding is not a descriptor resource at all. Stage inputs and outputs appear
 *         in the SPIR-V resource lists like everything else, but they are wired through the vertex
 *         input state and the framebuffer rather than through a descriptor set, so publishing them
 *         would put entries in a shader's binding table that no set layout can describe.
 */
bool toPublicBindingType(const ShaderBindingType type, BindingType* out) noexcept {
  switch (type) {
    case ShaderBindingType::TextureSampler:
      *out = BindingType::TextureSampler;
      return true;
    case ShaderBindingType::Sampler:
      *out = BindingType::Sampler;
      return true;
    case ShaderBindingType::SampledTexture:
      *out = BindingType::Texture;
      return true;
    case ShaderBindingType::UniformBuffer:
    case ShaderBindingType::PushConstant:
      // Push constants are emulated with a uniform block on this backend, so they are reported as
      // one; a caller binding them that way gets the same result as on a backend where they are real.
      *out = BindingType::UniformBuffer;
      return true;
    case ShaderBindingType::StorageBuffer:
      *out = BindingType::StorageBuffer;
      return true;
    case ShaderBindingType::ReadOnlyStorageBuffer:
      *out = BindingType::ReadOnlyStorageBuffer;
      return true;
    case ShaderBindingType::StageInput:
    case ShaderBindingType::StageOutput:
      return false;
  }
  return false;
}

} // namespace anonymous

// ==================================================================================================
// Device lifecycle
// ==================================================================================================

GLDevice::GLDevice() {
  mOwnerThread = std::hash<std::thread::id>{}(std::this_thread::get_id());
}

GLDevice::~GLDevice() {
  if (!mInitialized) {
    return;
  }

  // Bring the GPU to a stop before releasing anything: deleting a GL object that is still referenced
  // by an in-flight command stream is undefined on every driver.
  waitIdle();

  // Deleting a GL object needs this thread to have the context current, and that is not a formality: a
  // WGL driver does not treat the call as a no-op, it hangs. The render core drops the device after
  // releasing shared resources and nothing forces the context to still be current, so take it back for
  // the teardown. When it cannot be taken back, leaving the objects to the driver - which is about to
  // destroy the context anyway - is the only safe choice: a leak beats a hang.
  const bool canDeleteObjects = isSuccess(mPlatform.makeCurrent());
  if (!canDeleteObjects) {
    emitMessage(MessageSeverity::Warning,
            "the device was destroyed without a current GL context; its objects are left to the driver");
  }

  if (canDeleteObjects) {
    clearVertexArrayCache();
    clearFramebufferCache();
  }

  // Command buffers outlive their recordings, so they must be released before the pools they reference. That is now a
  // property of where `mCommandBuffers` is declared -- members are destroyed in reverse order, and it comes after the
  // pools -- rather than a delete loop written here. Clearing it explicitly would be redundant; the vector and the
  // buffers it owns go when the object does. The lock is not taken because this is teardown: a device being destroyed
  // while another thread is still creating command buffers for it is already a bug the caller owns.
  mCommandBuffers.clear();

  auto& api = gl::getGLApi();

  if (canDeleteObjects) {
  mSwapchains.forEach([&api](SwapchainHandle, GLSwapchain&) { (void) api; });
  mFences.forEach([&api](FenceHandle, GLFence& fence) {
    if (fence.sync != nullptr && api.glDeleteSync != nullptr) {
      api.glDeleteSync(fence.sync);
    }
    fence.sync = nullptr;
  });
  mQuerySets.forEach([&api](QuerySetHandle, GLQuerySet& querySet) {
    if (!querySet.queries.empty() && api.glDeleteQueries != nullptr) {
      api.glDeleteQueries(GLsizei(querySet.queries.size()), querySet.queries.data());
    }
    querySet.queries.clear();
  });
  mSamplers.forEach([&api](TextureSamplerHandle, GLSampler& sampler) {
    if (sampler.name != 0 && api.glDeleteSamplers != nullptr) {
      api.glDeleteSamplers(1, &sampler.name);
    }
    sampler.name = 0;
  });
  mRenderPipelines.forEach([&api](RenderPipelineHandle, GLRenderPipeline& pipeline) {
    if (pipeline.program.program != 0 && api.glDeleteProgram != nullptr) {
      api.glDeleteProgram(pipeline.program.program);
    }
    pipeline.program.program = 0;
  });
  mComputePipelines.forEach([&api](ComputePipelineHandle, GLComputePipeline& pipeline) {
    if (pipeline.program.program != 0 && api.glDeleteProgram != nullptr) {
      api.glDeleteProgram(pipeline.program.program);
    }
    pipeline.program.program = 0;
  });
  mShaderModules.forEach([&api](ShaderModuleHandle, GLShaderModule& module) {
    if (module.shaderObject != 0 && api.glDeleteShader != nullptr) {
      api.glDeleteShader(module.shaderObject);
    }
    module.shaderObject = 0;
  });
  mBuffers.forEach([&api](BufferHandle, GLBuffer& buffer) {
    if (buffer.meta.name != 0 && api.glDeleteBuffers != nullptr) {
      api.glDeleteBuffers(1, &buffer.meta.name);
    }
    buffer.meta.name = 0;
  });
  mTextures.forEach([&api](TextureHandle, GLTexture& texture) {
    if (texture.meta.name != 0 && !texture.meta.externallyOwned && api.glDeleteTextures != nullptr) {
      api.glDeleteTextures(1, &texture.meta.name);
    }
    texture.meta.name = 0;
  });
  }

  // Releasing the thread is what lets the platform destroy the context: on WGL a context that is still
  // current cannot be deleted, and the delete fails instead.
  (void) mPlatform.releaseCurrent();
  mPlatform.shutdown();
  mInitialized = false;
}

Result GLDevice::initialize(const DeviceConfiguration& configuration) {
  const SurfaceDescriptor* surface = configuration.surface;

  // A headless device gets an empty descriptor: no window, no adopted context, and a pinned size of
  // zero so that querySurfaceSize falls through to the driver.
  SurfaceDescriptor headless;
  if (surface == nullptr) {
    surface = &headless;
  }

  // An unset dialect means "the platform's own", which is what makes a bare `DeviceConfiguration`
  // work everywhere: there is no literal default that is right on both a phone and a desktop. An
  // explicit request is honoured, which is how one platform's capability set is exercised on another's
  // hardware; if the platform cannot satisfy it, `GlPlatform::initialize` fails rather than quietly
  // coming up in the other dialect.
  const GlDialect dialect = configuration.glDialect.value_or(getPlatformGlDialect());
  const GlVersion minimum = GlVersion::minimumFor(dialect);

  // The share request travels in the surface descriptor because that is what the platform layer receives: which handle a
  // context shares through is platform-specific (HGLRC / EGLContext / GLXContext / CGL share group), and the descriptor is
  // where this codebase already keeps those handles.
  SurfaceDescriptor shareDescriptor = *surface;
  shareDescriptor.glShareContext = configuration.shareContext;
  const Result contextResult = mPlatform.initialize(shareDescriptor, dialect, minimum, configuration.enableValidation);
  if (!isSuccess(contextResult)) {
    // Without a GL context there is no device. Reporting Unsupported rather than Error lets the
    // factory move on to the next backend in the priority list.
    std::string message = "the OpenGL backend could not obtain a ";
    message += toString(dialect);
    message += " context from the platform";
    const char* missing = gl::getGLApi().missingEntryPoint;
    if (missing != nullptr) {
      message += " (the required entry point ";
      message += missing;
      message += " could not be resolved)";
    }
    emitMessage(MessageSeverity::Error, message.c_str());
    return Result::Unsupported;
  }

  mPolicy.allowSrgbDowngrade = true;
  mPolicy.allowMipReduction = true;
  mPolicy.clampTextureSize = true;

  probeCapabilities();

  // The renderer's hard requirements are checked here, before the device is handed out. Failing now
  // turns "wrong output three frames later" into a startup error with a specific missing feature.
  if (configuration.requiredFeatures != 0 &&
      (mCapabilities.features & configuration.requiredFeatures) != configuration.requiredFeatures) {
    const Features missing = configuration.requiredFeatures & ~mCapabilities.features;
    std::string message = "the OpenGL backend cannot satisfy the renderer's required features:";
    for (uint32_t bit = 0; bit < 32; ++bit) {
      const auto feature = static_cast<Feature>(1u << bit);
      if ((missing & static_cast<Features>(feature)) == 0) continue;
      message += ' ';
      message += toString(feature);
      message += " (";
      message += toString(mCapabilities.reasonFor(feature));
      message += ')';
    }
    emitMessage(MessageSeverity::Error, message.c_str());
    mPlatform.shutdown();
    return Result::Unsupported;
  }

  if (configuration.capabilityOverride != nullptr) {
    // Tests use this to exercise fallback paths on hardware that happens to support everything.
    mCapabilities = *configuration.capabilityOverride;
  }

  // One small uniform buffer backs the emulated push constants for the whole device.
  mPushConstantSize = 0;
  mPushConstantData.clear();

  mInfo.backend = Backend::OpenGL;
  // The window-system layer's name is part of this, and it is the only part a caller cannot work out for themselves.
  //
  // `GlPlatform::backendName()` is implemented per platform as "WGL", "GLX", "CGL" or "EGL", and until this line nothing
  // called it: the name was one of two fixed strings chosen by the dialect. That is the wrong axis -- "gl-desktop" says
  // which *dialect* is in use, and `DeviceInfo::apiVersion` already says that more precisely ("OpenGL 4.6" against
  // "OpenGL ES 3.2"). What a diagnostic actually wants is which of the four context-creation paths is running, because
  // that is what differs between a machine where a bug reproduces and one where it does not.
  //
  // The dialect is kept as a suffix so the existing spelling remains a prefix of the new one, and so the same platform
  // built in the other dialect cannot report the same string.
  //
  // `static` because `DeviceInfo` holds a `const char*` that must outlive the call. Concatenating into a temporary and
  // taking `c_str()` would leave the field pointing at freed memory -- the kind of bug that reads correctly and fails
  // when the allocator reuses the block. One value per platform and dialect, both fixed at compile time, so a single
  // cached object per instantiation is exactly right.
  static const std::string backendName =
          std::string(dialect == GlDialect::ES ? "coral/backend/gl-es/" : "coral/backend/gl-desktop/") +
          GlPlatform::backendName();
  mInfo.backendName = backendName.c_str();
  mInfo.acceptsSpirv = isShaderCrossAvailable();
  // The OpenGL backend is the one whose driver compiles GLSL, so it is the one backend that can take
  // source straight through. Unconditional, not gated on anything the build configured: the point of
  // the direct path is that it needs neither shaderc nor SPIRV-Cross.
  mInfo.acceptsGlslDirect = true;

  mInitialized = true;
  return Result::Success;
}

void GLDevice::probeCapabilities() {
  gl::GLApi& api = gl::getGLApi();

  // ---------------------------------------------------------------- identity
  mInfo.vendor = mPlatform.vendor().c_str();
  mInfo.deviceName = mPlatform.renderer().c_str();
  mInfo.apiVersion = mPlatform.apiVersion().c_str();
  mInfo.driverVersion = mPlatform.shadingLanguageVersion().c_str();
  mInfo.acceptsSpirv = isShaderCrossAvailable();
  mInfo.acceptsGlslDirect = true;

  CapabilityBuilder builder(mCapabilities);
  builder.apiVersion(mInfo.apiVersion);

  mCapabilities.acceptsSpirv = isShaderCrossAvailable();
  // A compile of the caller's own text by the driver, with no translator in the way. True on every GL
  // context, including one built without shaderc and without SPIRV-Cross.
  mCapabilities.acceptsGlslDirect = true;
  // Whether a *GLSL source* shader module can become SPIR-V, which is what shaderc is for. It was hard-coded true and
  // that was wrong the moment `-DCORAL_BACKEND_SUPPORTS_SHADERC=OFF` became a supported configuration: a caller reading
  // the capability would be told the backend compiles GLSL and then get `Result::Unsupported` from the first such
  // module. A capability is a promise, so it is derived from the build rather than assumed.
#if CORAL_BACKEND_HAS_SHADERC
  mCapabilities.runtimeShaderCompilation = true;
#else
  mCapabilities.runtimeShaderCompilation = false;
#endif

  const GlDialect dialect = mPlatform.context().dialect;
  const GlVersion version = mPlatform.context().version;
  const bool es = dialect == GlDialect::ES;
  const bool desktop = !es;

  // Version gates for each dialect, named rather than compared inline so the feature table below
  // reads as a statement about capabilities rather than a nest of arithmetic.
  const bool es31 = es && version.atLeast({ 3, 1 });
  const bool es32 = es && version.atLeast({ 3, 2 });
  // Desktop OpenGL 4.3 is where compute, storage buffers and shader storage blocks arrive. 4.1 -
  // the RHI's desktop minimum - has geometry and tessellation stages but not compute.
  const bool gl43 = desktop && version.atLeast({ 4, 3 });
  const bool gl42 = desktop && version.atLeast({ 4, 2 });
  const bool gl41 = desktop && version.atLeast({ 4, 1 });

  mCapabilities.supportsDebugMarkers =
    mPlatform.hasExtension("GL_KHR_debug") || mPlatform.hasExtension("GL_ARB_debug_output");

  // ---------------------------------------------------------------- limits
  const auto queryInt = [&api](const GLenum name, const uint32_t fallback) -> uint32_t {
    if (api.glGetIntegerv == nullptr) {
      return fallback;
    }
    GLint value = 0;
    api.glGetIntegerv(name, &value);
    return value > 0 ? static_cast<uint32_t>(value) : fallback;
  };

  Limits limits;
  const uint32_t maxTextureSize = queryInt(GL_MAX_TEXTURE_SIZE, 2048);
  limits.maxTextureDimension2D = maxTextureSize;
  limits.maxTextureDimensionCube = queryInt(GL_MAX_CUBE_MAP_TEXTURE_SIZE, maxTextureSize);
  limits.maxTextureDimension3D = queryInt(GL_MAX_3D_TEXTURE_SIZE, 256);
  limits.maxTextureDimension1D = maxTextureSize;
  limits.maxTextureArrayLayers = queryInt(GL_MAX_ARRAY_TEXTURE_LAYERS, 256);

  limits.maxVertexAttributes = queryInt(GL_MAX_VERTEX_ATTRIBS, 16);
  limits.maxVertexBuffers = std::min(limits.maxVertexAttributes, 8u);
  limits.maxColorAttachments = queryInt(GL_MAX_COLOR_ATTACHMENTS, 4);
  limits.maxSampledTexturesPerStage = queryInt(GL_MAX_TEXTURE_IMAGE_UNITS, 16);
  limits.maxSamplersPerStage = limits.maxSampledTexturesPerStage;
  limits.maxUniformBuffersPerStage = queryInt(GL_MAX_UNIFORM_BUFFER_BINDINGS, 12);
  limits.maxUniformBufferBindingSize = queryInt(GL_MAX_UNIFORM_BLOCK_SIZE, 16384);
  limits.maxBoundDescriptorSets = std::min(limits.maxUniformBuffersPerStage, 4u);

  // Storage buffers arrive with compute: GL 4.3 on desktop, ES 3.1 on mobile.
  const bool hasStorageBuffers = gl43 || es31;
  limits.maxStorageBuffersPerStage = hasStorageBuffers ? 8u : 0u;
  limits.maxStorageBufferBindingSize = hasStorageBuffers ? 128u * 1024u * 1024u : 0u;

  limits.maxFramesInFlight = 1;

  // Push constants are emulated with a uniform block, so the budget is one block minus the space the
  // driver may need for packing. 256 bytes comfortably covers the transform and palette data push
  // constants are actually used for.
  limits.maxPushConstantSize =
    limits.maxUniformBufferBindingSize > 256 ? 256u : std::min(limits.maxUniformBufferBindingSize, 256u);

  mCapabilities.limits = limits;

  // ---------------------------------------------------------------- versions and extensions
  const bool hasAnisotropy = mPlatform.hasExtension("GL_EXT_texture_filter_anisotropic") ||
                             mPlatform.hasExtension("GL_ARB_texture_filter_anisotropic");
  const bool hasAstc = mPlatform.hasExtension("GL_KHR_texture_compression_astc_ldr") ||
                       mPlatform.hasExtension("GL_OES_texture_compression_astc");
  const bool hasS3tc = mPlatform.hasExtension("GL_EXT_texture_compression_s3tc") ||
                       mPlatform.hasExtension("GL_ARB_texture_compression_bptc");
  const bool hasTextureBorder = mPlatform.hasExtension("GL_EXT_texture_border_clamp") ||
                                mPlatform.hasExtension("GL_NV_texture_border_clamp") ||
                                mPlatform.hasExtension("GL_ARB_texture_border_clamp") || desktop;
  const bool hasDrawBuffers = api.glDrawBuffers != nullptr;
  const bool hasMultisampleResolve = api.glBlitFramebuffer != nullptr;
  const bool hasSampleShading = desktop ? gl41 : mPlatform.hasExtension("GL_OES_sample_shading");
  const bool hasTimerQuery = desktop ? gl42 : mPlatform.hasExtension("GL_EXT_disjoint_timer_query");
  const bool hasFenceSync = api.glFenceSync != nullptr;
  const bool hasCubeArray = desktop ? gl41 : es32;
  // glBindFramebuffer-independent: separate sampler objects are core in desktop 3.3 and ES 3.0.
  const bool hasSeparateSamplers = api.glBindSampler != nullptr && api.glGenSamplers != nullptr;
  // Wireframe is core on desktop and impossible on ES, which is the clearest example in the whole
  // table of the two dialects not being interchangeable.
  const bool hasWireframe = api.glPolygonMode != nullptr;
  const bool hasDepthClamp = desktop || mPlatform.hasExtension("GL_EXT_depth_clamp");
  const bool hasLogicOp = desktop;

  // ---------------------------------------------------------------- features
  //
  // Every entry below is a deliberate statement about this backend, not a hardware probe alone. When
  // a feature cannot be provided the reason recorded here is what an upper layer sees when it asks why
  // its technique was disabled, so the distinction between "the API version is too low" and "the API
  // has no such concept" is worth being precise about.
  //
  // The desktop column of these answers is *not* the same as the ES column, and that is the point: a
  // renderer that gates on Feature::ComputeShader gets "yes" on a 4.3 desktop driver and "no" on an ES
  // 3.0 phone, with the same code.
  builder.feature(Feature::ComputeShader, gl43 || es31, FeatureUnavailableReason::ApiVersionTooLow)
         .feature(Feature::ComputeShaderAtomics, hasStorageBuffers, FeatureUnavailableReason::ApiVersionTooLow)
         .feature(Feature::IndirectDispatch, gl43 || es31, FeatureUnavailableReason::ApiVersionTooLow)
         .feature(Feature::ComputeWorkgroupBarrier, gl43 || es31, FeatureUnavailableReason::ApiVersionTooLow)
         // Geometry and tessellation are core in desktop 4.1 and absent below ES 3.2. Metal has no
         // geometry stage at all, so an upper layer that depends on one has to be reworked rather
         // than retried on newer hardware.
         .feature(Feature::GeometryShader, gl41 || es32, FeatureUnavailableReason::ApiVersionTooLow)
         .feature(Feature::TessellationShader, gl41 || es32, FeatureUnavailableReason::ApiVersionTooLow)
         .feature(Feature::Texture3D, true)
         .feature(Feature::TextureCube, true)
         .feature(Feature::TextureArray, true)
         .feature(Feature::TextureCubeArray, hasCubeArray,
           es ? FeatureUnavailableReason::ApiVersionTooLow : FeatureUnavailableReason::HardwareUnsupported)
         .feature(Feature::SamplerAnisotropy, hasAnisotropy, FeatureUnavailableReason::ExtensionMissing)
         .feature(Feature::SamplerBorderClamp, hasTextureBorder, FeatureUnavailableReason::ExtensionMissing)
         .feature(Feature::TextureLod, true)
         .feature(Feature::SeparateSamplers, hasSeparateSamplers, FeatureUnavailableReason::ApiVersionTooLow)
         .feature(Feature::StorageTexture, gl42 || es31, FeatureUnavailableReason::ApiVersionTooLow)
         .feature(Feature::ComparisonSampler, true)
         .feature(Feature::MultipleRenderTargets, hasDrawBuffers, FeatureUnavailableReason::ApiVersionTooLow)
         .feature(Feature::MultisampleResolve, hasMultisampleResolve, FeatureUnavailableReason::ApiVersionTooLow)
         .feature(Feature::SampleRateShading, hasSampleShading, FeatureUnavailableReason::ExtensionMissing)
         .feature(Feature::AlphaToCoverage, true)
         .feature(Feature::FillModeNonSolid, hasWireframe, FeatureUnavailableReason::ArchitecturallyImpossible)
         .feature(Feature::DepthClamp, hasDepthClamp, FeatureUnavailableReason::ArchitecturallyImpossible)
         .feature(Feature::ProgrammableBlending, false, FeatureUnavailableReason::ArchitecturallyImpossible)
         .feature(Feature::LogicOp, hasLogicOp, es
                                                  ? FeatureUnavailableReason::ArchitecturallyImpossible
                                                  : FeatureUnavailableReason::NotImplementedByBackend)
         .feature(Feature::IntegerVertexAttributes, true)
         .feature(Feature::HostWaitFence, hasFenceSync, FeatureUnavailableReason::ApiVersionTooLow)
         .feature(Feature::TimelineSemaphore, false, FeatureUnavailableReason::ArchitecturallyImpossible)
         .feature(Feature::QueryObjects, true)
         .feature(Feature::TimestampQuery, hasTimerQuery, FeatureUnavailableReason::ExtensionMissing)
         .feature(Feature::PipelineStatisticsQuery, false, FeatureUnavailableReason::ArchitecturallyImpossible)
         // The swapchain is owned by the window system, not by the RHI: OpenGL has no concept of
         // acquiring an image. The backend reports the feature so that upper layers can use the
         // uniform beginFrame/endFrame path.
         .feature(Feature::Swapchain, true)
         .feature(Feature::NativeSurface, true);

  mCapabilities.maxSamplerAnisotropy =
    hasAnisotropy
      ? static_cast<float>(queryInt(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, 1))
      : 1.0f;
  // Not a `Feature` bit -- the mask is a full 32 bits -- but the same kind of promise: whether this platform can grant
  // `PresentMode::Mailbox` rather than approximate it. False on every OpenGL platform; see `supportsMailboxPresent`.
  mCapabilities.mailboxPresent = mPlatform.supportsMailboxPresent();
  if (mCapabilities.maxSamplerAnisotropy < 1.0f) {
    mCapabilities.maxSamplerAnisotropy = 1.0f;
  }
  mCapabilities.maxTextureSize = maxTextureSize;
  mCapabilities.maxCubeMapSize = limits.maxTextureDimensionCube;
  mCapabilities.maxRenderbufferSize = queryInt(GL_MAX_RENDERBUFFER_SIZE, maxTextureSize);
  mCapabilities.maxViewportWidth = maxTextureSize;
  mCapabilities.maxViewportHeight = maxTextureSize;
  mCapabilities.maxFragmentTextureUnits = limits.maxSampledTexturesPerStage;
  mCapabilities.maxVertexTextureUnits = queryInt(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, 16);
  mCapabilities.maxCombinedTextureUnits = queryInt(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, 32);

  // ---------------------------------------------------------------- sample counts
  const uint32_t maxSamples = queryInt(GL_MAX_SAMPLES, 1);
  constexpr uint32_t sampleCandidates[] = { 1, 2, 4, 8 };
  for (uint32_t count: sampleCandidates) {
    if (count > maxSamples) {
      break;
    }
    builder.sampleCount(count);
  }
  if (!hasMultisampleResolve) {
    // Without glBlitFramebuffer a multisampled target can be rendered but never read, which is
    // useless for post-processing; MSAA is therefore reported as unavailable beyond count 1.
    mCapabilities.supportedSampleCounts = (1u << 1);
  }
  mCapabilities.maxSamples = maxSamples;

  // ---------------------------------------------------------------- texture formats
  //
  // Formats are enumerated from the RHI's own table and filtered by the dialect's baseline, rather
  // than queried one by one from the driver. glGetInternalformativ would give a precise answer per
  // format but is only core in ES 3.1 and GL 4.2, and probing ~70 formats by creating textures is far
  // too slow at startup. The baseline rules below are conservative on purpose: reporting a format as
  // unsupported costs a fallback, reporting it as supported and being wrong costs a black texture.
  const bool multisampleRenderable = hasMultisampleResolve;
  const auto addFormat = [&builder, multisampleRenderable](const PixelFormat format,
    const bool sampled,
    const bool renderable,
    const bool filterable) {
    TextureFormatCapabilities support;
    support.sampled = sampled;
    support.renderable = renderable;
    support.blendable = renderable && filterable;
    support.storage = false;
    support.multisampleResolve = renderable && multisampleRenderable;
    support.sampleCounts[1] = true;
    if (renderable && multisampleRenderable) {
      support.sampleCounts[2] = true;
      support.sampleCounts[4] = true;
    }
    builder.textureFormat(format, support);
  };

  // Mandatory in ES 3.0: renderable and filterable. Colour-renderable and texture-filterable are
  // required for every format in the first group by the ES 3.0 specification's table 3.13.
  addFormat(PixelFormat::R8_UNORM, true, true, true);
  addFormat(PixelFormat::R8G8_UNORM, true, true, true);
  addFormat(PixelFormat::R8G8B8A8_UNORM, true, true, true);
  addFormat(PixelFormat::B8G8R8A8_UNORM, true, true, true);
  addFormat(PixelFormat::R8G8B8_UNORM, true, false, true);
  addFormat(PixelFormat::R8_SNORM, true, false, true);
  addFormat(PixelFormat::R8G8_SNORM, true, false, true);
  addFormat(PixelFormat::R8G8B8A8_SNORM, true, false, true);
  addFormat(PixelFormat::R8_UINT, true, true, false);
  addFormat(PixelFormat::R8G8_UINT, true, true, false);
  addFormat(PixelFormat::R8G8B8A8_UINT, true, true, false);
  addFormat(PixelFormat::R8_SINT, true, true, false);
  addFormat(PixelFormat::R8G8_SINT, true, true, false);
  addFormat(PixelFormat::R8G8B8A8_SINT, true, true, false);
  addFormat(PixelFormat::R16_UNORM, true, false, true);
  addFormat(PixelFormat::R16G16_UNORM, true, false, true);
  addFormat(PixelFormat::R16G16B16A16_UNORM, true, false, true);
  addFormat(PixelFormat::R16_SFLOAT, true, false, true);
  addFormat(PixelFormat::R16G16_SFLOAT, true, false, true);
  addFormat(PixelFormat::R16G16B16A16_SFLOAT, true, true, true);
  addFormat(PixelFormat::R32_SFLOAT, true, false, true);
  addFormat(PixelFormat::R32G32_SFLOAT, true, false, true);
  addFormat(PixelFormat::R32G32B32A32_SFLOAT, true, true, true);
  addFormat(PixelFormat::R32_UINT, true, false, false);
  addFormat(PixelFormat::R32G32_UINT, true, false, false);
  addFormat(PixelFormat::R32G32B32A32_UINT, true, true, false);
  addFormat(PixelFormat::R32_SINT, true, false, false);
  addFormat(PixelFormat::R32G32_SINT, true, false, false);
  addFormat(PixelFormat::R32G32B32A32_SINT, true, true, false);
  addFormat(PixelFormat::R10G10B10A2_UNORM, true, true, true);
  addFormat(PixelFormat::R11G11B10_SFLOAT, true, false, true);
  addFormat(PixelFormat::R16G16B16A16_UINT, true, true, false);
  addFormat(PixelFormat::R16G16B16A16_SINT, true, true, false);
  addFormat(PixelFormat::R16G16_UINT, true, true, false);
  addFormat(PixelFormat::R16G16_SINT, true, true, false);
  addFormat(PixelFormat::R16_UINT, true, true, false);
  addFormat(PixelFormat::R16_SINT, true, true, false);

  // Depth and depth/stencil. D32 requires ES 3.0 / GL 3.0, which both dialects guarantee.
  addFormat(PixelFormat::D16_UNORM, true, true, false);
  addFormat(PixelFormat::D24_UNORM_S8_UINT, true, true, false);
  addFormat(PixelFormat::D32_SFLOAT, true, true, false);
  addFormat(PixelFormat::D32_SFLOAT_S8_UINT, true, true, false);

  // sRGB. Colour-renderable sRGB support is optional in ES 3.0 and absent on a fair number of older
  // mobile parts, which is exactly the case the DegradationPolicy sRGB downgrade exists for. It is
  // mandatory on desktop, but the sampled/renderable split is reported the same way so an upper layer
  // has one code path.
  for (PixelFormat format: { PixelFormat::R8G8B8_SRGB, PixelFormat::R8G8B8A8_SRGB }) {
    TextureFormatCapabilities support;
    support.sampled = true;
    support.renderable = true;
    support.blendable = true;
    support.sampleCounts[1] = true;
    support.sampleCounts[2] = multisampleRenderable;
    support.sampleCounts[4] = multisampleRenderable;
    builder.textureFormat(format, support);
  }

  // Compressed formats. Which families exist depends entirely on the dialect:
  //
  //   - ETC2/EAC are mandatory in ES 3.0 and are the baseline on every Android device, but desktop
  //     drivers generally do not implement them.
  //   - ASTC is an ES extension and by far the most common texture format on modern Android.
  //   - BCn is the desktop baseline and does not exist in ES at all.
  //
  // Reporting all three sets unconditionally would produce textures that fail at creation on the
  // wrong platform, which is why each family is gated on the dialect and the extension.
  if (es) {
    constexpr PixelFormat etc2Formats[] = {
      PixelFormat::ETC2_RGB8_UNORM,
      PixelFormat::ETC2_RGB8_SRGB,
      PixelFormat::ETC2_RGBA8_UNORM,
      PixelFormat::ETC2_RGBA8_SRGB
    };
    for (PixelFormat format: etc2Formats) {
      TextureFormatCapabilities support;
      support.sampled = true;
      builder.textureFormat(format, support);
    }
  }

  if (hasAstc) {
    constexpr PixelFormat astcFormats[] = {
      PixelFormat::ASTC_4x4_UNORM,
      PixelFormat::ASTC_4x4_SRGB,
      PixelFormat::ASTC_8x8_UNORM,
      PixelFormat::ASTC_8x8_SRGB
    };
    for (PixelFormat format: astcFormats) {
      TextureFormatCapabilities support;
      support.sampled = true;
      builder.textureFormat(format, support);
    }
  }

  if (desktop && hasS3tc) {
    constexpr PixelFormat bcnFormats[] = {
      PixelFormat::BC1_RGBA_UNORM,
      PixelFormat::BC1_RGBA_SRGB,
      PixelFormat::BC3_RGBA_UNORM,
      PixelFormat::BC3_RGBA_SRGB,
      PixelFormat::BC4_R_UNORM,
      PixelFormat::BC5_RG_UNORM,
      PixelFormat::BC7_RGBA_UNORM,
      PixelFormat::BC7_RGBA_SRGB
    };
    for (PixelFormat format: bcnFormats) {
      TextureFormatCapabilities support;
      support.sampled = true;
      builder.textureFormat(format, support);
    }
  }

  // Anything not listed above is left entirely unsupported, which is the honest default: a format
  // family the dialect does not implement would otherwise fail inside the driver.
  (void) gl42;
  (void) gl43;
}

// ==================================================================================================
// Queues and command buffers
// ==================================================================================================

/**
 * The single queue an OpenGL device exposes.
 *
 * GL has one implicit command stream per context, so there is no meaningful difference between a
 * graphics, compute and transfer queue. Rather than inventing a scheduler that the API cannot
 * express, the backend returns the same queue for every type and documents the collapse - upper
 * layers that need real parallelism must consult `Capabilities` and take a different path, which on
 * OpenGL means recording on multiple threads and executing the lists in a chosen order.
 *
 * Defined at namespace scope rather than inside an anonymous namespace: it is a nested class of
 * GLDevice, and a member of a class with external linkage cannot be defined in a namespace that has
 * internal linkage.
 */
class GLDevice::GLQueueImpl final : public Queue {
public:
  explicit GLQueueImpl(GLDevice& device)
    : mDevice(device) {
  }

  [[nodiscard]] QueueType type() const noexcept override { return QueueType::Graphics; }

  Result submit(CommandBuffer& commandBuffer, const FenceHandle fence, const SemaphoreHandle waitSemaphore,
    const SemaphoreHandle signalSemaphore) override {
    // Waiting on a semaphore is a no-op because the GL command stream is already serialised: by the
    // time a submission is recorded, everything previously submitted has been issued. The signal is
    // recorded as state so that a later wait on it succeeds, which keeps semaphore-based upper layer
    // code working without pretending the OpenGL backend has real GPU-side semaphores.
    (void) waitSemaphore;
    if (signalSemaphore.valid()) {
      if (GLSemaphore* semaphore = mDevice.semaphore(signalSemaphore); semaphore != nullptr) {
        semaphore->signalled = true;
      }
    }
    return mDevice.submitOnQueue(commandBuffer, fence);
  }

  Result waitIdle() override { return mDevice.waitIdle(); }

private:
  GLDevice& mDevice;
};

Queue* GLDevice::getQueue(const QueueType type) noexcept {
  // Every queue type maps onto the one GL command stream, so the type is accepted and ignored rather
  // than rejected: an upper layer asking for a compute queue gets the graphics queue and still works.
  (void) type;
  if (!mGraphicsQueue) {
    mGraphicsQueue = std::make_unique<GLQueueImpl>(*this);
  }
  return mGraphicsQueue.get();
}

Result GLDevice::createCommandBuffer(const CommandBufferDescriptor& descriptor, CommandBuffer** outCommandBuffer) {
  if (outCommandBuffer == nullptr) {
    return Result::InvalidArgument;
  }
  *outCommandBuffer = nullptr;

  // The buffer is built before the lock is taken, so a `GLCommandBuffer` constructor that allocates
  // does not hold the device's command-buffer lock while it does. See the member's comment for why
  // this lock exists and why a per-thread list would not have removed the need for it.
  auto commandBuffer = std::make_unique<GLCommandBuffer>(*this, descriptor);
  *outCommandBuffer = commandBuffer.get();

  // The device owns the buffer, and the caller gets a borrowed pointer to it. The `push_back` is the
  // only place ownership is taken, and it cannot fail after the pointer has been handed out, so a
  // concurrent `destroyCommandBuffer` either does not yet see this buffer or frees it - never both.
  const std::lock_guard<std::mutex> guard(mCommandBufferLock);
  mCommandBuffers.push_back(std::move(commandBuffer));
  return Result::Success;
}

void GLDevice::destroyCommandBuffer(CommandBuffer* commandBuffer) {
  if (commandBuffer == nullptr) {
    return;
  }
  // Erasing the owner releases the buffer, so there is no `delete` here and no way to free one twice. A buffer this
  // device does not own is left alone rather than freed, which the previous `delete` outside the erase did not do.
  const std::lock_guard<std::mutex> guard(mCommandBufferLock);
  std::erase_if(mCommandBuffers, [commandBuffer](const std::unique_ptr<GLCommandBuffer>& owned) {
    return owned.get() == static_cast<GLCommandBuffer*>(commandBuffer);
  });
}

Result GLDevice::executeCommandBuffer(CommandBuffer& commandBuffer) {
  auto& gl = static_cast<GLCommandBuffer&>(commandBuffer);
  return gl.replay();
}

Result GLDevice::submitOnQueue(CommandBuffer& commandBuffer, const FenceHandle fence) {
  if (const Result result = executeCommandBuffer(commandBuffer); !isSuccess(result)) {
    return result;
  }

  if (fence.valid()) {
    GLFence* entry = mFences.get(fence);
    if (entry == nullptr) {
      return Result::InvalidHandle;
    }

    // A GL fence object is the closest thing ES 3.0 has to a submission fence: glFenceSync inserts a
    // marker into the command stream and the object signals once the GPU reaches it.
    gl::GLApi& api = gl::getGLApi();
    if (entry->sync != nullptr && api.glDeleteSync != nullptr) {
      api.glDeleteSync(entry->sync);
      entry->sync = nullptr;
    }
    if (api.glFenceSync != nullptr) {
      entry->sync = api.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
      entry->state = entry->sync != nullptr ? GLFenceState::Unsignalled : GLFenceState::Signalled;
    } else {
      // No sync objects: the submission is implicitly complete because the GL stream is synchronous.
      entry->state = GLFenceState::Signalled;
    }

    if (api.glFlush != nullptr) {
      api.glFlush();
    }
  }

  return Result::Success;
}

// ==================================================================================================
// Textures
// ==================================================================================================

Result GLDevice::createTexture(const TextureDescriptor& descriptor, TextureHandle* outTexture) {
  if (outTexture == nullptr) {
    return Result::InvalidArgument;
  }
  *outTexture = TextureHandle::invalid();
  if (!mInitialized) {
    return Result::Error;
  }

  // Validation runs before the driver sees anything, so an unsupported request is either repaired
  // with an explicit note or rejected with a reason - never silently accepted and then wrong.
  TextureDescriptor resolved = descriptor;
  const TextureValidationResult validation = validateTextureDescriptor(descriptor, mCapabilities);
  if (!validation.valid && validation.resolved.format == PixelFormat::Undefined) {
    emitMessage(MessageSeverity::Error, validation.message);
    return Result::Unsupported;
  }

  if (!validation.valid) {
    // Validator found a specific problem; the degradation policy decides whether it is repairable.
    const TextureResolution downgraded = resolveTextureDescriptor(descriptor, mCapabilities, mPolicy);
    if (downgraded.descriptor.format == PixelFormat::Undefined) {
      emitMessage(MessageSeverity::Error, downgraded.note != nullptr ? downgraded.note : validation.message);
      return Result::Unsupported;
    }
    resolved = downgraded.descriptor;
    if (downgraded.degraded && downgraded.note != nullptr) {
      emitMessage(MessageSeverity::Warning, downgraded.note);
    }
  }

  const GLFormatDesc format = describeFormat(resolved.format);
  if (format.internalFormat == 0) {
    emitMessage(MessageSeverity::Error, "the requested texture format has no equivalent in this GL dialect");
    return Result::Unsupported;
  }

  gl::GLApi& api = gl::getGLApi();
  GLTexture texture;
  texture.meta.target = toGLTarget(resolved.type);
  texture.meta.format = resolved.format;
  texture.meta.internalFormat = format.internalFormat;
  texture.meta.width = resolved.size.width;
  texture.meta.height = resolved.size.height;
  texture.meta.depthOrLayers = resolved.size.depthOrArrayLayers;
  texture.meta.mipLevelCount = resolved.mipLevelCount;
  texture.meta.sampleCount = resolved.sampleCount;
  texture.meta.type = resolved.type;
  texture.meta.usage = resolved.usage;
  if (resolved.label != nullptr) texture.label = resolved.label;

  api.glGenTextures(1, &texture.meta.name);
  if (texture.meta.name == 0) return Result::OutOfMemory;
  api.glBindTexture(texture.meta.target, texture.meta.name);

  // A shorthand for the target, nothing more: it is read a dozen more times below and `texture.meta.`
  // on every one of them would bury the calls that matter. Not a copy, so the metadata is still
  // written in exactly one place.
  GLenum& target = texture.meta.target;

  const bool isDepth = getPixelFormatInfo(resolved.format).depth;

  // A render target must be mip-complete only if it is sampled, and a colour-renderable format that
  // cannot be filtered must use NEAREST. The defaults below make a freshly created texture usable
  // without the caller having to remember either rule.
  //
  // Storage is always allocated for every level the caller asked for, whether or not the texture is also a render
  // target. An earlier version computed the "is an attachment" test here and then did not branch on it; the allocation
  // is unconditional, so the test was removed rather than left as a variable that reads as an applied policy.
  GLSamplerState& state = texture.appliedSampler;
  state.magFilter = GL_LINEAR;
  state.minFilter = gl::toGLMinFilter(FilterMode::Linear, MipmapFilterMode::Linear, resolved.mipLevelCount);
  state.wrapS = state.wrapT = state.wrapR = GL_CLAMP_TO_EDGE;
  state.maxAnisotropy = 1.0f;

  if (resolved.sampleCount > 1) {
    // Multisampled textures are always immutable storage in ES 3.0 and cannot be mipped. glTexStorage
    // is required: glTexImage2D rejects a multisample request.
    if (target == GL_TEXTURE_2D && api.glTexStorage2D != nullptr) {
      api.glTexStorage2D(GL_TEXTURE_2D, 1, format.internalFormat, static_cast<GLsizei>(resolved.size.width),
        static_cast<GLsizei>(resolved.size.height));
      texture.meta.immutableStorage = true;
      if (const GLenum storageError = api.glGetError(); storageError != GL_NO_ERROR) {
        std::string storageMessage = "glTexStorage for ";
        storageMessage += std::to_string(resolved.size.width) + "x" + std::to_string(resolved.size.height);
        storageMessage += " mips=" + std::to_string(resolved.mipLevelCount) + " failed with ";
        storageMessage += gl::GLApi::errorName(storageError);
        emitMessage(MessageSeverity::Error, storageMessage.c_str());
      }
    } else {
      api.glDeleteTextures(1, &texture.meta.name);
      emitMessage(MessageSeverity::Error, "multisampled textures are only supported for 2D targets in OpenGL");
      return Result::Unsupported;
    }
  } else {
    // Uploaded data, if any, is supplied for the base level only, so the remaining levels are
    // allocated empty. glTexStorage2D gives an immutable, mip-complete texture in one call, which is
    // both faster and avoids the classic "incomplete texture samples black" trap.
    const bool canUseStorage =
      api.glTexStorage2D != nullptr && api.glTexStorage3D != nullptr && target != GL_TEXTURE_1D;
    bool storageAllocated = false;
    if (canUseStorage) {
      if (target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY ||
          target == GL_TEXTURE_CUBE_MAP_ARRAY) {
        api.glTexStorage3D(target, static_cast<GLsizei>(resolved.mipLevelCount), format.internalFormat,
          static_cast<GLsizei>(resolved.size.width), static_cast<GLsizei>(resolved.size.height),
          static_cast<GLsizei>(resolved.size.depthOrArrayLayers));
      } else {
        api.glTexStorage2D(target, static_cast<GLsizei>(resolved.mipLevelCount), format.internalFormat,
          static_cast<GLsizei>(resolved.size.width), static_cast<GLsizei>(resolved.size.height));
      }
      if (const GLenum storageError = api.glGetError(); storageError != GL_NO_ERROR) {
        // Some GLES drivers reject immutable storage for otherwise valid small textures (the
        // Maleoon 910 rejects an 8x8 RGBA8 glTexStorage2D with GL_INVALID_OPERATION). Fall through
        // to the per-level glTexImage2D path instead of leaving an incomplete texture behind.
        std::string storageMessage = "glTexStorage for ";
        storageMessage += std::to_string(resolved.size.width) + "x" + std::to_string(resolved.size.height);
        storageMessage += " mips=" + std::to_string(resolved.mipLevelCount) + " failed with ";
        storageMessage += gl::GLApi::errorName(storageError);
        storageMessage += "; falling back to glTexImage2D";
        emitMessage(MessageSeverity::Warning, storageMessage.c_str());
      } else {
        texture.meta.immutableStorage = true;
        storageAllocated = true;
      }
    }
    if (!storageAllocated) {
      // A driver that rejected glTexStorage may still have made the object immutable; delete and
      // recreate it before using the mutable glTexImage2D path.
      if (canUseStorage) {
        api.glDeleteTextures(1, &texture.meta.name);
        api.glGenTextures(1, &texture.meta.name);
        api.glBindTexture(target, texture.meta.name);
      }
      // Fallback for drivers that declined immutable storage, or that declined a particular texture:
      // allocate each level explicitly so the texture is still mip-complete.
      const uint32_t levels = std::max(resolved.mipLevelCount, 1u);
      for (uint32_t level = 0; level < levels; ++level) {
        const uint32_t width = std::max(resolved.size.width >> level, 1u);
        const uint32_t height = std::max(resolved.size.height >> level, 1u);
        if (target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY) {
          api.glTexImage3D(target, static_cast<GLint>(level), static_cast<GLint>(format.internalFormat),
            static_cast<GLsizei>(width), static_cast<GLsizei>(height),
            static_cast<GLsizei>(resolved.size.depthOrArrayLayers), 0, format.format, format.type, nullptr);
        } else if (target == GL_TEXTURE_CUBE_MAP) {
          for (GLenum face = GL_TEXTURE_CUBE_MAP_POSITIVE_X; face <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z; ++face) {
            api.glTexImage2D(face, static_cast<GLint>(level), static_cast<GLint>(format.internalFormat),
              static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0, format.format, format.type, nullptr);
          }
        } else {
          api.glTexImage2D(target, static_cast<GLint>(level), static_cast<GLint>(format.internalFormat),
            static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0, format.format, format.type, nullptr);
        }
      }
    }
  }

  // Sampler state on the texture object. Filtering a float or integer attachment with GL_LINEAR is
  // allowed but pointless, and filtering a 32-bit integer format is an error, so integer formats are
  // pinned to NEAREST here.
  if (format.format == GL_RED_INTEGER ||
      format.format == GL_RG_INTEGER ||
      format.format == GL_RGB_INTEGER ||
      format.format == GL_RGBA_INTEGER) {
    texture.appliedSampler.minFilter = GL_NEAREST;
    texture.appliedSampler.magFilter = GL_NEAREST;
  }

  api.glTexParameteri(target, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(texture.appliedSampler.minFilter));
  api.glTexParameteri(target, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(texture.appliedSampler.magFilter));
  api.glTexParameteri(target, GL_TEXTURE_WRAP_S, static_cast<GLint>(texture.appliedSampler.wrapS));
  api.glTexParameteri(target, GL_TEXTURE_WRAP_T, static_cast<GLint>(texture.appliedSampler.wrapT));
  if (target == GL_TEXTURE_3D || target == GL_TEXTURE_CUBE_MAP_ARRAY) {
    api.glTexParameteri(target, GL_TEXTURE_WRAP_R, static_cast<GLint>(texture.appliedSampler.wrapR));
  }
  // The view's subresource range is expressed through the base/max level parameters. Setting them to
  // the full chain keeps the whole texture visible by default; createTextureView narrows it.
  api.glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
  api.glTexParameteri(target, GL_TEXTURE_MAX_LEVEL,
    static_cast<GLint>(std::max(resolved.mipLevelCount, 1u) - 1));

  if (isDepth) {
    // A depth texture that will be sampled as a shadow map must default to comparison mode off;
    // SamplerDescriptor::depthCompare turns it on when the binding asks for it.
    api.glTexParameteri(target, GL_TEXTURE_COMPARE_MODE, GL_NONE);
  }

  api.glBindTexture(target, 0);

  // Draining the error queue here is what turns a driver-side rejection into a specific message
  // instead of a failure surfacing at the first draw call that samples the texture.
  if (const GLenum error = api.glGetError(); error != GL_NO_ERROR) {
    api.glDeleteTextures(1, &texture.meta.name);
    std::string message = "glGenTextures/glTexStorage failed with ";
    message += gl::GLApi::errorName(error);
    message += " (target=" + std::to_string(static_cast<unsigned>(texture.meta.target));
    message += ", internal=0x" + std::to_string(static_cast<unsigned>(format.internalFormat));
    message += ", size=" + std::to_string(resolved.size.width) + "x" + std::to_string(resolved.size.height);
    message += ", mips=" + std::to_string(resolved.mipLevelCount);
    message += ", samples=" + std::to_string(resolved.sampleCount);
    message += ", label=" + texture.label + ")";
    emitMessage(MessageSeverity::Error, message.c_str());
    return error == GL_OUT_OF_MEMORY ? Result::OutOfMemory : Result::Error;
  }

  const uint64_t residentBytes = glTextureBytes(texture.meta);
  *outTexture = mTextures.allocate(std::move(texture));
  if (!outTexture->valid()) {
    return Result::OutOfMemory;
  }
  mResidentTextureBytes += residentBytes;
  return Result::Success;
}

void GLDevice::destroyTexture(TextureHandle texture) {
  GLTexture* entry = mTextures.get(texture);
  if (entry == nullptr) {
    return;
  }

  // The framebuffers built for this texture go first, and they have to: the name is about to be handed back
  // to the driver, and a cached framebuffer still attached to it would be *found* by a later render pass
  // whose attachment lands on the same name. Such a framebuffer is incomplete, so binding it raises
  // `GL_INVALID_FRAMEBUFFER_OPERATION` and draws nothing -- and because that error is sticky, it surfaces on
  // an unrelated call later (a `createTexture`, typically, which then reports a failure about itself). See
  // `GpuTargetLifetime.ATargetCanBeDestroyedAndAnotherCreatedInItsPlace`, which reproduced both symptoms.
  discardFramebuffersUsing(entry->meta.name);

  if (entry->eglImage != nullptr) {
    EGLApi& egl = getEGLApi();
    if (egl.eglDestroyImageKHR != nullptr) egl.eglDestroyImageKHR(mPlatform.context().display, entry->eglImage);
    entry->eglImage = nullptr;
  }
  if (entry->meta.name != 0 && !entry->meta.externallyOwned && gl::getGLApi().glDeleteTextures != nullptr) {
    gl::getGLApi().glDeleteTextures(1, &entry->meta.name);
  }
  entry->meta.name = 0;

  // Views of a destroyed texture become meaningless; releasing them here prevents a view from
  // keeping a recycled GL name alive by accident. The framebuffer cache is keyed by GL name, so a
  // recycled name resolves to a texture that is at least correctly attached - but the old entry's
  // first colour slot may no longer match, hence the invalidation.
  mTextureViews.forEach([texture](TextureViewHandle, GLTextureView& view) {
    if (view.texture == texture) {
      view.texture = TextureHandle::invalid();
    }
  });

  // Imported textures borrow the application's GL object, so their bytes were never counted.
  if (!entry->meta.externallyOwned) {
    mResidentTextureBytes -= std::min(mResidentTextureBytes, glTextureBytes(entry->meta));
  }
  mTextures.free(texture);
}

Result GLDevice::createTextureView(const TextureViewDescriptor& descriptor, TextureViewHandle* outView) {
  if (outView == nullptr) {
    return Result::InvalidArgument;
  }
  *outView = TextureViewHandle::invalid();

  if (!mTextures.isAlive(descriptor.texture)) {
    return Result::InvalidHandle;
  }

  GLTextureView view;
  view.texture = descriptor.texture;
  view.dimension = descriptor.dimension;
  view.format = descriptor.format;
  view.baseMipLevel = descriptor.range.baseMipLevel;
  view.mipLevelCount = descriptor.range.mipLevelCount;
  view.baseArrayLayer = descriptor.range.baseArrayLayer;
  view.arrayLayerCount = descriptor.range.arrayLayerCount;
  if (descriptor.label != nullptr) {
    view.label = descriptor.label;
  }

  *outView = mTextureViews.allocate(std::move(view));
  return outView->valid() ? Result::Success : Result::OutOfMemory;
}

void GLDevice::destroyTextureView(const TextureViewHandle view) { mTextureViews.free(view); }

GLDevice::ResolvedView GLDevice::resolveTextureView(const TextureViewHandle view) const {
  ResolvedView resolved;

  // The sentinel names the surface's own images: there is no texture entry to look up, and the replay turns it into
  // `glBindFramebuffer(GL_FRAMEBUFFER, 0)` (see `GlCommandBuffer`'s `hasDefaultFramebuffer`). This is the one attachment
  // that exists for a window target and not for an offscreen one.
  if (view == defaultFramebufferView()) {
    resolved.isDefaultFramebuffer = true;
    resolved.valid = true;
    return resolved;
  }

  const GLTextureView* entry = mTextureViews.get(view);
  if (entry == nullptr) {
    return resolved;
  }
  if (entry->isDefaultFramebuffer) {
    resolved.isDefaultFramebuffer = true;
    resolved.valid = true;
    return resolved;
  }

  const GLTexture* texture = mTextures.get(entry->texture);
  if (texture == nullptr) {
    return resolved;
  }

  resolved.textureName = texture->meta.name;
  // A view may reinterpret the dimension (a 2D array slice viewed as a 2D texture, for instance),
  // which changes the GL target a sampler binding must use.
  resolved.target = toGLTarget(entry->dimension);
  resolved.format = entry->format != PixelFormat::Undefined ? entry->format : texture->meta.format;
  resolved.valid = true;
  return resolved;
}

// ==================================================================================================
// Samplers
// ==================================================================================================

Result GLDevice::createSampler(const SamplerDescriptor& descriptor, TextureSamplerHandle* outSampler) {
  if (outSampler == nullptr) {
    return Result::InvalidArgument;
  }
  *outSampler = TextureSamplerHandle::invalid();

  if (const ValidationResult validation = validateSamplerDescriptor(descriptor, mCapabilities); !validation.valid) {
    emitMessage(MessageSeverity::Error, validation.message);
    return Result::Unsupported;
  }

  gl::GLApi& api = gl::getGLApi();
  GLSampler sampler;
  sampler.descriptor = descriptor;
  if (descriptor.label != nullptr) {
    sampler.label = descriptor.label;
  }

  if (api.glGenSamplers != nullptr) {
    api.glGenSamplers(1, &sampler.name);
    if (sampler.name == 0) {
      return Result::OutOfMemory;
    }

    // Sampler objects carry the state a texture would otherwise carry, so the mip filter choice is
    // deferred: glSamplerParameteri's mipmapping variants are simply always correct here because the
    // sampler is used with whatever texture is bound, and GL resolves completeness at draw time.
    const SamplerDescriptor& d = descriptor;
    const GLenum minFilter = d.minFilter == FilterMode::Nearest
                               ? (d.mipmapFilter == MipmapFilterMode::Nearest
                                    ? GL_NEAREST_MIPMAP_NEAREST
                                    : GL_NEAREST_MIPMAP_LINEAR)
                               : (d.mipmapFilter == MipmapFilterMode::Nearest
                                    ? GL_LINEAR_MIPMAP_NEAREST
                                    : GL_LINEAR_MIPMAP_LINEAR);
    api.glSamplerParameteri(sampler.name, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(minFilter));
    api.glSamplerParameteri(sampler.name, GL_TEXTURE_MAG_FILTER,
      d.magFilter == FilterMode::Nearest ? GL_NEAREST : GL_LINEAR);
    api.glSamplerParameteri(sampler.name, GL_TEXTURE_WRAP_S, static_cast<GLint>(toGLAddressMode(d.addressModeU)));
    api.glSamplerParameteri(sampler.name, GL_TEXTURE_WRAP_T, static_cast<GLint>(toGLAddressMode(d.addressModeV)));
    api.glSamplerParameteri(sampler.name, GL_TEXTURE_WRAP_R, static_cast<GLint>(toGLAddressMode(d.addressModeW)));
    if (d.maxAnisotropy > 1.0f && mCapabilities.supports(Feature::SamplerAnisotropy)) {
      api.glSamplerParameterf(sampler.name, GL_TEXTURE_MAX_ANISOTROPY_EXT, d.maxAnisotropy);
    }

    if (d.filterType == FilterType::Comparison) {
      api.glSamplerParameteri(sampler.name, GL_TEXTURE_COMPARE_MODE, static_cast<GLint>(GL_COMPARE_REF_TO_TEXTURE));
      api.glSamplerParameteri(sampler.name, GL_TEXTURE_COMPARE_FUNC, static_cast<GLint>(toGLCompare(d.compare)));
    } else {
      api.glSamplerParameteri(sampler.name, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    }

    if (d.addressModeU == SamplerAddressMode::ClampToBorder ||
        d.addressModeV == SamplerAddressMode::ClampToBorder ||
        d.addressModeW == SamplerAddressMode::ClampToBorder) {
      const float border[4] = {
        d.borderColor == SamplerBorderColor::OpaqueWhite ? 1.0f : 0.0f,
        d.borderColor == SamplerBorderColor::OpaqueWhite ? 1.0f : 0.0f,
        d.borderColor == SamplerBorderColor::OpaqueWhite ? 1.0f : 0.0f,
        d.borderColor == SamplerBorderColor::OpaqueBlack ? 1.0f : 0.0f
      };
      api.glSamplerParameterfv(sampler.name, GL_TEXTURE_BORDER_COLOR, border);
    }
  }
  // When glGenSamplers is unavailable the descriptor is still recorded, and the binding path applies
  // it to the texture's own parameters instead. That is the classic GL style, and it keeps a single
  // SamplerDescriptor abstraction working across both.

  *outSampler = mSamplers.allocate(std::move(sampler));
  return outSampler->valid() ? Result::Success : Result::OutOfMemory;
}

void GLDevice::destroySampler(const TextureSamplerHandle sampler) {
  GLSampler* entry = mSamplers.get(sampler);
  if (entry == nullptr) {
    return;
  }
  if (entry->name != 0 && gl::getGLApi().glDeleteSamplers != nullptr) {
    gl::getGLApi().glDeleteSamplers(1, &entry->name);
  }
  entry->name = 0;
  mSamplers.free(sampler);
}

// ==================================================================================================
// Buffers
// ==================================================================================================

Result GLDevice::createBuffer(const BufferDescriptor& descriptor, BufferHandle* outBuffer) {
  if (outBuffer == nullptr) {
    return Result::InvalidArgument;
  }
  *outBuffer = BufferHandle::invalid();
  if (!mInitialized) {
    return Result::Error;
  }

  if (const ValidationResult validation = validateBufferDescriptor(descriptor, mCapabilities); !validation.valid) {
    emitMessage(MessageSeverity::Error, validation.message);
    return Result::Unsupported;
  }

  gl::GLApi& api = gl::getGLApi();
  GLBuffer buffer;
  buffer.meta.size = descriptor.size;
  buffer.meta.usage = descriptor.usage;
  buffer.meta.memoryUsage = descriptor.memoryUsage;
  if (descriptor.label != nullptr) {
    buffer.label = descriptor.label;
  }

  // The GL target is only a hint used for creation and for the default binding; the buffer is bound
  // to whichever target a command needs, because the same buffer may be a vertex source in one pass
  // and a uniform buffer in another.
  if (hasUsage(descriptor.usage, BufferUsage::Index)) {
    buffer.meta.target = GL_ELEMENT_ARRAY_BUFFER;
  } else if (hasUsage(descriptor.usage, BufferUsage::Uniform)) {
    buffer.meta.target = GL_UNIFORM_BUFFER;
  } else if (hasUsage(descriptor.usage, BufferUsage::Indirect)) {
    buffer.meta.target = GL_DRAW_INDIRECT_BUFFER;
  } else {
    buffer.meta.target = GL_ARRAY_BUFFER;
  }

  switch (descriptor.memoryUsage) {
    case MemoryUsage::GpuOnly:
    case MemoryUsage::CpuToGpu:
      buffer.meta.glUsage = GL_STATIC_DRAW;
      break;
    case MemoryUsage::GpuToCpu:
      buffer.meta.glUsage = GL_STREAM_READ;
      break;
    case MemoryUsage::CpuToGpuDynamic:
      buffer.meta.glUsage = GL_DYNAMIC_DRAW;
      break;
  }

  // Clear any error the previous operation left behind before judging this allocation: a stale
  // GL_INVALID_OPERATION made a perfectly good buffer look like a failure in the X11 snapshot path.
  while (api.glGetError() != GL_NO_ERROR) {
  }
  api.glGenBuffers(1, &buffer.meta.name);
  if (buffer.meta.name == 0) {
    return Result::OutOfMemory;
  }

  api.glBindBuffer(buffer.meta.target, buffer.meta.name);
  // glBufferData both allocates and, when `initialData` is non-null, uploads. Creating the store up
  // front means the buffer is immediately usable, which removes the "buffer with no store bound to a
  // vertex attribute" class of bug that costs an hour to find on OpenGL.
  api.glBufferData(buffer.meta.target, static_cast<GLsizeiptr>(descriptor.size), descriptor.initialData,
    buffer.meta.glUsage);
  api.glBindBuffer(buffer.meta.target, 0);

  if (const GLenum error = api.glGetError(); error != GL_NO_ERROR) {
    api.glDeleteBuffers(1, &buffer.meta.name);
    std::string message = "buffer creation failed with ";
    message += gl::GLApi::errorName(error);
    message += " (target 0x";
    char hex[16] = {};
    std::snprintf(hex, sizeof(hex), "%x", buffer.meta.target);
    message += hex;
    message += ", size ";
    message += std::to_string(descriptor.size);
    message += ")";
    emitMessage(MessageSeverity::Error, message.c_str());
    return error == GL_OUT_OF_MEMORY ? Result::OutOfMemory : Result::Error;
  }

  const uint64_t residentBytes = buffer.meta.size;
  *outBuffer = mBuffers.allocate(std::move(buffer));
  if (!outBuffer->valid()) {
    return Result::OutOfMemory;
  }
  mResidentBufferBytes += residentBytes;
  return Result::Success;
}

void GLDevice::destroyBuffer(const BufferHandle bufferHandle) {
  GLBuffer* entry = mBuffers.get(bufferHandle);
  if (entry == nullptr) {
    return;
  }

  if (entry->mapped && entry->meta.name != 0 && gl::getGLApi().glUnmapBuffer != nullptr) {
    gl::getGLApi().glUnmapBuffer(entry->meta.target);
    entry->mapped = false;
    entry->mappedPointer = nullptr;
  }
  if (entry->meta.name != 0 && gl::getGLApi().glDeleteBuffers != nullptr) {
    gl::getGLApi().glDeleteBuffers(1, &entry->meta.name);
  }
  entry->meta.name = 0;
  mResidentBufferBytes -= std::min(mResidentBufferBytes, entry->meta.size);
  mBuffers.free(bufferHandle);
}

Result GLDevice::mapBuffer(const BufferHandle bufferHandle, const uint64_t offset, const uint64_t size,
  void** outPointer) {
  if (outPointer == nullptr) {
    return Result::InvalidArgument;
  }
  *outPointer = nullptr;

  GLBuffer* entry = mBuffers.get(bufferHandle);
  if (entry == nullptr) return Result::InvalidHandle;
  if (entry->meta.memoryUsage == MemoryUsage::GpuOnly) {
    emitMessage(MessageSeverity::Error,
      "mapBuffer was called on a GpuOnly buffer; create it with MemoryUsage::CpuToGpu or "
      "CpuToGpuDynamic, or upload through CommandBuffer::writeBuffer");
    return Result::Unsupported;
  }
  if (offset + size > entry->meta.size) {
    return Result::InvalidArgument;
  }

  gl::GLApi& api = gl::getGLApi();
  if (api.glMapBufferRange == nullptr) {
    // There is no map in this dialect. Reporting Unsupported is honest: the caller must fall back to
    // CommandBuffer::writeBuffer, which works everywhere.
    return Result::Unsupported;
  }

  GLbitfield access = GL_MAP_WRITE_BIT;
  if (entry->meta.memoryUsage == MemoryUsage::GpuToCpu) {
    access = GL_MAP_READ_BIT;
  }
  if (entry->meta.memoryUsage == MemoryUsage::CpuToGpuDynamic) {
    access |= GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT;
  }

  api.glBindBuffer(entry->meta.target, entry->meta.name);
  void* pointer = api.glMapBufferRange(entry->meta.target, GLintptr(offset), GLsizeiptr(size), access);
  api.glBindBuffer(entry->meta.target, 0);

  if (pointer == nullptr) {
    return Result::OutOfMemory;
  }

  entry->mapped = true;
  entry->mappedPointer = pointer;
  // The range is recorded so that `unmapBuffer` can flush exactly it. Flushing more than was mapped is
  // `GL_INVALID_VALUE`, and the error is sticky -- see the note there.
  entry->mappedOffset = offset;
  entry->mappedLength = size;
  *outPointer = pointer;
  return Result::Success;
}

void GLDevice::unmapBuffer(const BufferHandle bufferHandle) {
  GLBuffer* entry = mBuffers.get(bufferHandle);
  if (entry == nullptr || !entry->mapped) {
    return;
  }

  gl::GLApi& api = gl::getGLApi();
  api.glBindBuffer(entry->meta.target, entry->meta.name);
  if (entry->meta.memoryUsage == MemoryUsage::CpuToGpuDynamic && api.glFlushMappedBufferRange != nullptr) {
    // The mapping was taken with FLUSH_EXPLICIT, so the range must be flushed before unmap or the driver is
    // free to ignore the writes.
    //
    // The range flushed is the range that was *mapped*, not the whole buffer. `glFlushMappedBufferRange` is
    // only legal inside the mapping, and a call past its end raises `GL_INVALID_VALUE` -- which is sticky, so
    // the error surfaces on whatever call checks next and looks like an unrelated failure. A helper that maps
    // a 256-byte range of a large buffer would otherwise break the next `createTexture` in the process.
    api.glFlushMappedBufferRange(entry->meta.target, GLintptr(entry->mappedOffset), GLsizeiptr(entry->mappedLength));
  }
  if (api.glUnmapBuffer != nullptr) {
    api.glUnmapBuffer(entry->meta.target);
  }
  api.glBindBuffer(entry->meta.target, 0);

  entry->mapped = false;
  entry->mappedPointer = nullptr;
  entry->mappedOffset = 0;
  entry->mappedLength = 0;
}

// ==================================================================================================
// Shader modules
// ==================================================================================================

Result GLDevice::createShaderModule(const ShaderModuleDescriptor& descriptor, ShaderModuleHandle* outModule) {
  if (outModule == nullptr) {
    return Result::InvalidArgument;
  }
  *outModule = ShaderModuleHandle::invalid();

  if (descriptor.code == nullptr || descriptor.codeSize == 0) {
    emitMessage(MessageSeverity::Error, "the shader module descriptor carries no code");
    return Result::InvalidArgument;
  }

  GLShaderModule module;
  module.stage = descriptor.stage;
  if (descriptor.label != nullptr) {
    module.label = descriptor.label;
  }

  switch (descriptor.language) {
    case ShaderModuleDescriptor::Language::SPIRV: {
      // The whole point of the SPIR-V canonical input: the module the Vulkan backend would consume
      // unchanged is translated here to the dialect's GLSL, and its descriptor bindings fall out of
      // the reflection rather than being restated by the caller.
      if (!isShaderCrossAvailable()) {
        emitMessage(MessageSeverity::Error,
          "SPIR-V shader modules require SPIRV-Cross, which this build does not include; "
          "supply GLSL source instead or rebuild with CORAL_BACKEND_SUPPORTS_SPIRV_CROSS=ON");
        return Result::Unsupported;
      }
      if (descriptor.codeSize % sizeof(uint32_t) != 0) {
        emitMessage(MessageSeverity::Error, "the SPIR-V module's size is not a whole number of words");
        return Result::InvalidArgument;
      }
      module.inputSpirv.assign(static_cast<const uint32_t*>(descriptor.code),
              static_cast<const uint32_t*>(descriptor.code) + descriptor.codeSize / sizeof(uint32_t));
      module.sourceInfo.spirvInput = true;
      module.sourceInfo.language = ShaderSourceLanguage::Detect;
      module.sourceInfo.version = 0;
      module.sourceInfo.sourceSize = descriptor.codeSize;
      module.sourceInfo.sourceHash = hashBytes(module.inputSpirv.data(),
              module.inputSpirv.size() * sizeof(uint32_t));

      ShaderCrossCompileInfo info = ShaderCrossCompileInfo::forDialect(mPlatform.context().dialect, descriptor.stage);
      info.spirv = static_cast<const uint32_t*>(descriptor.code);
      info.wordCount = descriptor.codeSize / sizeof(uint32_t);
      info.flipVertexY = true;
      info.fixupClipSpace = true;
      info.combineImageSamplers = true;

      const ShaderCrossCompileResult compiled = crossCompileToGLSL(info);
      if (!compiled.success) {
        emitMessage(MessageSeverity::Error, compiled.error.c_str());
        return Result::ShaderCompilationFailed;
      }

      module.source = compiled.source;
      module.reflection = compiled.reflection;
      module.spirvTranslated = true;
      break;
    }

    case ShaderModuleDescriptor::Language::GLSL: {
      // A NUL-terminated string is expected; codeSize includes the terminator.
      module.source.assign(static_cast<const char*>(descriptor.code), descriptor.codeSize);
      if (!module.source.empty() && module.source.back() == '\0') module.source.pop_back();

      if (!isGlslCompilerAvailable()) {
        emitMessage(MessageSeverity::Error,
          "GLSL shader modules require shaderc, which this build does not include; supply SPIR-V "
          "instead or rebuild with CORAL_BACKEND_SUPPORTS_SHADERC=ON");
        return Result::Unsupported;
      }
      if (!isShaderCrossAvailable()) {
        // GLSL becomes SPIR-V first and is then translated like any other SPIR-V module, so the
        // translator is as necessary here as it is on the SPIR-V path.
        emitMessage(MessageSeverity::Error,
          "GLSL shader modules are compiled to SPIR-V and then translated, which needs "
          "SPIRV-Cross; supply SPIR-V instead or rebuild with CORAL_BACKEND_SUPPORTS_SPIRV_CROSS=ON");
        return Result::Unsupported;
      }

      // Compile the author's GLSL to SPIR-V. This is what makes a GLSL module a first-class citizen
      // rather than a second path through the backend: from here on it is SPIR-V, so it gets the same
      // reflection and the same dialect translation as a module that arrived as SPIR-V.
      // The context's own dialect is what the source is retargeted toward, because it is the dialect
      // that will have to compile the result.
      const GlslCompileResult glsl = compileGlslToSpirv(module.source, descriptor.stage, mPlatform.context().dialect,
        descriptor.sourceLanguage, descriptor.entryPoint, descriptor.optimizeSource,
        module.label.empty() ? nullptr : module.label.c_str());
      if (!glsl.success()) {
        // The compiler's own diagnostics name the offending line of the author's source, because the
        // source is what it parsed - unless the version was retargeted, in which case the advice
        // appended by the compiler explains why the line numbers still refer to the original text.
        std::string message = "GLSL source compilation failed";
        if (!module.label.empty()) {
          message += " for ";
          message += module.label;
        }
        message += ": ";
        message += glsl.error;
        emitMessage(MessageSeverity::Error, message.c_str());
        return Result::ShaderCompilationFailed;
      }

      module.sourceInfo = glsl.sourceInfo;
      module.spirvCompiledFromSource = true;
      // Kept so that a caller can cache the words and skip this compiler next run. It costs a copy of
      // a few kilobytes per shader, paid once at load.
      module.compiledSpirv = glsl.spirv;

      // Translate it with exactly the options the SPIR-V case uses. Sharing the path is the point;
      // a separate one would drift.
      ShaderCrossCompileInfo info = ShaderCrossCompileInfo::forDialect(mPlatform.context().dialect, descriptor.stage);
      info.spirv = glsl.spirv.data();
      info.wordCount = glsl.spirv.size();
      info.flipVertexY = true;
      info.fixupClipSpace = true;
      info.combineImageSamplers = true;

      const ShaderCrossCompileResult compiled = crossCompileToGLSL(info);
      if (!compiled.success) {
        std::string message = "the GLSL compiled to SPIR-V but the SPIR-V could not be translated for a ";
        message += toString(mPlatform.context().dialect);
        message += " context";
        if (!module.label.empty()) {
          message += " (";
          message += module.label;
          message += ")";
        }
        message += ": ";
        message += compiled.error;
        emitMessage(MessageSeverity::Error, message.c_str());
        return Result::ShaderCompilationFailed;
      }

      // `module.source` stays the author's GLSL: it is the "translated" GLSL for the context that is
      // thrown away, because a debugger wants the source the author wrote. The generated GLSL is still
      // available for diagnostics, which is where it belongs.
      module.reflection = compiled.reflection;
      module.generatedSource = compiled.source;
      module.spirvTranslated = true;
      break;
    }

    case ShaderModuleDescriptor::Language::GLSLDirect: {
      // A NUL-terminated string is expected; codeSize includes the terminator. Exactly as on the GLSL
      // path, and for the same reason: the two source languages differ in what happens *after* the
      // text is read, not in how it arrives.
      module.source.assign(static_cast<const char*>(descriptor.code), descriptor.codeSize);
      if (!module.source.empty() && module.source.back() == '\0') module.source.pop_back();

      if (module.source.empty()) {
        emitMessage(MessageSeverity::Error, "a GLSLDirect shader module carries an empty source");
        return Result::InvalidArgument;
      }

      // Nothing is consulted about the compiler here on purpose. This path exists for the caller that
      // has GLSL the driver can compile and does not want a compiler in the middle of it; refusing it
      // because shaderc or SPIRV-Cross is missing would be refusing the one path that needs neither.
      //
      // The source is not rewritten either. A `#version` the driver cannot accept, or a body written
      // for the other dialect, is a compile error whose log names the caller's own line numbers -
      // which is a better diagnostic than a preamble this backend synthesised behind the caller's
      // back, and the reason `sourceVersion` is documented as not applying here.
      module.directGlsl = true;

      // The version is parsed for the module's provenance, not to decide anything: `getShaderSourceInfo`
      // has to answer what the source was, and a caller comparing a manifest against a shader needs it.
      parseGlslVersion(module.source, &module.sourceInfo);
      if (descriptor.sourceLanguage != ShaderSourceLanguage::Detect) {
        // The caller knows better than the text does - a snippet with no `#version` handed to an ES
        // context is the usual case. Both fields move together, as they do on the compile path: a
        // language and a version that disagreed would make `getShaderSourceInfo` report a shader that
        // cannot exist, and the label is all this changes - the driver still decides how the text is
        // parsed.
        module.sourceInfo.language = descriptor.sourceLanguage;
        module.sourceInfo.version = descriptor.sourceLanguage == ShaderSourceLanguage::GLSL410
                                      ? 410
                                      : descriptor.sourceLanguage == ShaderSourceLanguage::ESSL300 ? 300 : 100;
      }
      // The driver compiles the source as the version it declares, so there is no retarget to report.
      // `compiledVersion` is filled in rather than left at zero because zero means "not compiled from
      // source here", and this module *is* compiled from the caller's source - just not by Coral.
      module.sourceInfo.compiledVersion = module.sourceInfo.version;
      module.sourceInfo.versionWasRetargeted = false;
      module.sourceInfo.optimized = false;
      module.sourceInfo.directGlsl = true;

      if (descriptor.sourceVersion != 0 || descriptor.optimizeSource) {
        // Not fatal, and worth saying out loud: a caller that set these on a direct module has
        // misunderstood which path it took, and silently ignoring them is how a shader ends up
        // optimised in one build and not in another.
        emitMessage(MessageSeverity::Warning,
          "sourceVersion and optimizeSource do not apply to a GLSLDirect module; the source is handed "
          "to the driver as written");
      }

      // The names have to outlive this call, so the table is copied here rather than referenced. The
      // `const char*` fixup happens below, once the name vector has stopped growing.
      if (descriptor.attributeDeclarations != nullptr) {
        module.attributeNames.reserve(descriptor.attributeDeclarationCount);
        for (uint32_t index = 0; index < descriptor.attributeDeclarationCount; ++index) {
          const ShaderAttributeDeclaration& declaration = descriptor.attributeDeclarations[index];
          if (declaration.name == nullptr || declaration.name[0] == '\0') {
            std::string message = "attribute declaration ";
            message += std::to_string(index);
            message += " names no vertex input";
            emitMessage(MessageSeverity::Error, message.c_str());
            return Result::InvalidArgument;
          }

          ShaderAttributeDeclaration copy;
          copy.location = declaration.location;
          copy.name = nullptr; // filled in below, from `attributeNames`
          module.attributeNames.emplace_back(declaration.name);
          module.attributes.push_back(copy);
        }
        for (size_t index = 0; index < module.attributes.size(); ++index) {
          module.attributes[index].name = module.attributeNames[index].c_str();
        }
      }
      break;
    }
  }

  // A vertex-input table is only meaningful for a direct module, and for the opposite reason to the
  // binding table above: on every other path the locations *are* in the module - as the `Location`
  // decoration that SPIRV-Cross turns back into `layout(location = N) in ...` - and a layout qualifier
  // wins over a `glBindAttribLocation`, so honouring the caller's table would produce a program whose
  // inputs sit somewhere the pipeline does not look. Reported rather than obeyed, because a caller that
  // filled one in for a compiled module has misunderstood which path it took.
  if (descriptor.attributeDeclarations != nullptr &&
      descriptor.language != ShaderModuleDescriptor::Language::GLSLDirect) {
    emitMessage(MessageSeverity::Warning,
      "attributeDeclarations was supplied for a module that is not GLSLDirect; that module carries its "
      "own vertex input locations, so the table is ignored");
  }

  // The explicit binding table is consulted for the source languages only; a SPIR-V module carries
  // its own `set` and `binding` decorations and reflecting them is the point of taking SPIR-V.
  //
  // For `GLSLDirect` this is not a correction but the whole table: there is no SPIR-V here to reflect,
  // so these declarations are what `getShaderReflection` reports and what `buildBindingTable` resolves
  // against the linked program. For `GLSL` it stays the escape hatch it was - a source whose
  // decorations the caller has to restate, which for ESSL 1.00 is every source.
  if (descriptor.bindingDeclarations != nullptr) {
    if (descriptor.language == ShaderModuleDescriptor::Language::SPIRV) {
      // Reported rather than obeyed. Applying a caller's table *on top of* reflected decorations would
      // publish two entries per binding, and the descriptor-set layout built from them would depend on
      // which one a lookup happened to reach first.
      emitMessage(MessageSeverity::Warning,
        "bindingDeclarations was supplied for a SPIR-V shader module, whose bindings are reflected from "
        "the module itself; the table is ignored");
    } else {
      for (uint32_t index = 0; index < descriptor.bindingDeclarationCount; ++index) {
        const ShaderBindingDeclaration& declaration = descriptor.bindingDeclarations[index];
        ShaderBindingInfo info;
        info.set = declaration.set;
        info.binding = declaration.binding;
        info.arraySize = declaration.arraySize;
        info.location = declaration.location;
        info.name = declaration.name != nullptr ? declaration.name : "";
        switch (declaration.type) {
          case BindingType::TextureSampler:
            info.type = ShaderBindingType::TextureSampler;
            break;
          case BindingType::Sampler:
            info.type = ShaderBindingType::Sampler;
            break;
          case BindingType::Texture:
            info.type = ShaderBindingType::SampledTexture;
            break;
          case BindingType::UniformBuffer:
            info.type = ShaderBindingType::UniformBuffer;
            break;
          case BindingType::ReadOnlyStorageBuffer:
          case BindingType::StorageBuffer:
            info.type = ShaderBindingType::StorageBuffer;
            break;
        }
        module.reflection.bindings.push_back(std::move(info));
      }
    }
  }

  // The reflection is republished in the public vocabulary while the module is still being built. It
  // is done here rather than on demand because this is the point where the private
  // `ShaderBindingType` is in scope, and because a query that allocated on every call would make the
  // reflection unusable from a per-frame code path.
  module.bindingNames.reserve(module.reflection.bindings.size());
  module.publicBindings.reserve(module.reflection.bindings.size());
  for (const ShaderBindingInfo& reflected: module.reflection.bindings) {
    BindingType publicType = BindingType::TextureSampler;
    if (!toPublicBindingType(reflected.type, &publicType)) continue;

    module.bindingNames.push_back(reflected.name);

    ShaderBindingDeclaration declaration;
    declaration.set = reflected.set;
    declaration.binding = reflected.binding;
    declaration.type = publicType;
    declaration.arraySize = reflected.arraySize;
    declaration.location = reflected.location;
    declaration.name = nullptr;
    module.publicBindings.push_back(declaration);
  }
  // The name pointers are taken only once the string vector is complete, because growing it would
  // otherwise invalidate addresses taken during the loop above.
  for (size_t index = 0; index < module.publicBindings.size(); ++index) {
    module.publicBindings[index].name = module.bindingNames[index].c_str();
  }

  gl::GLApi& api = gl::getGLApi();
  const GLenum shaderType = toGLShaderType(descriptor.stage);
  module.shaderObject = api.glCreateShader(shaderType);
  if (module.shaderObject == 0) {
    return Result::Error;
  }

  // The source a driver compiles must satisfy the driver's own rules, and one of them is not the translator's to
  // keep: a module that names its binding points carries `layout(binding = N)`, which a desktop core context only
  // accepts at `#version 420` or above or with `GL_ARB_shading_language_420pack` enabled. SPIRV-Cross emits the
  // extension when its option is on, but "when its option is on" is not a guarantee this backend can rest on --
  // measured, the same module translated here reached the driver without the directive and was rejected with
  //   `0(5) : error C7532: layout(binding) requires "#version 420" or later
  //    0(5) : error C0000: ... or #extension GL_ARB_shading_language_420pack : enable`
  // So the directive is added here, at the one place every path funnels through, and only when the qualifier is
  // actually present and no directive is.
  // What the driver compiles is the translated GLSL whenever there is one. The author's source is kept for
  // diagnostics and reflection (a GLSL module was compiled to SPIR-V to get there), but it is written for the
  // compiler and not for this driver: a varying with `layout(location)` is required by SPIR-V and rejected by
  // GLSL ES 3.00, and the translator is the only place that distinction is resolved. Handing the author's text
  // to the driver instead made every effect shader fail on a device whose compiler enforces the ES 3.00 rule.
  std::string driverSource = module.generatedSource.empty() ? module.source : module.generatedSource;
  if (mPlatform.context().dialect == GlDialect::Desktop &&
      driverSource.find("layout(binding") != std::string::npos &&
      driverSource.find("GL_ARB_shading_language_420pack") == std::string::npos) {
    const size_t versionEnd = driverSource.find('\n');
    // Directly after the `#version` line, which is the only position a directive may take. `enable` rather than
    // `require`: a driver without the extension should warn and try, not refuse the module outright.
    if (versionEnd != std::string::npos) {
      driverSource.insert(versionEnd, "\n#extension GL_ARB_shading_language_420pack : enable");
    }
  }

  const GLchar* source = driverSource.c_str();
  api.glShaderSource(module.shaderObject, 1, &source, nullptr);
  api.glCompileShader(module.shaderObject);

  GLint status = 0;
  api.glGetShaderiv(module.shaderObject, GL_COMPILE_STATUS, &status);
  if (status == 0) {
    GLint logLength = 0;
    api.glGetShaderiv(module.shaderObject, GL_INFO_LOG_LENGTH, &logLength);
    std::string log(static_cast<size_t>(logLength > 1 ? logLength : 1), '\0');
    api.glGetShaderInfoLog(module.shaderObject, static_cast<GLsizei>(log.size()), nullptr, log.data());

    // The GLSL source is echoed on failure. Without it a shader compile error from a translated
    // SPIR-V module is essentially undebuggable, because the source the driver saw is generated.
    std::string message = "shader compilation failed (";
    message += toString(descriptor.stage);
    message += "): ";
    message += log;
    if (module.spirvTranslated) {
      message += "\n--- generated GLSL ES ---\n";
      message += driverSource;
    } else if (module.directGlsl) {
      // The source is *not* echoed: the caller wrote it and still has it, and the driver's log already
      // carries its own line numbers, which for this path are line numbers of the caller's file. What
      // is worth adding is that fact, because it is the one thing that differs from the translated
      // path, where the same-looking log refers to a file nobody wrote.
      message += "\n(the line numbers above refer to the source that was supplied, which the driver "
                 "compiled as written)";
    }
    emitMessage(MessageSeverity::Error, message.c_str());

    api.glDeleteShader(module.shaderObject);
    module.shaderObject = 0;
    module.infoLog = std::move(log);
    return Result::ShaderCompilationFailed;
  }

  *outModule = mShaderModules.allocate(std::move(module));
  return outModule->valid() ? Result::Success : Result::OutOfMemory;
}

Result GLDevice::getShaderReflection(const ShaderModuleHandle module,
  std::vector<ShaderBindingDeclaration>* outBindings) {
  if (outBindings == nullptr) return Result::InvalidArgument;

  const GLShaderModule* entry = mShaderModules.get(module);
  if (entry == nullptr) return Result::InvalidHandle;

  // A module supplied as GLSL source has no reflection: recovering bindings from source would mean
  // parsing GLSL, which is exactly the work SPIR-V exists to avoid. Reporting Unsupported rather than
  // an empty table lets a caller tell "no bindings" from "cannot tell you".
  //
  // A `GLSLDirect` module is the exception, and it is not a contradiction: its table was handed in by
  // the caller rather than recovered, so reporting it *is* reporting something known. For that path the
  // declarations are the contract - what is in the table is what the module has, and an empty table
  // means a shader with no bindings rather than one whose bindings could not be read. That is what lets
  // a renderer build its descriptor set layouts the same way it does for a SPIR-V module.
  if (!entry->spirvTranslated && !entry->directGlsl) return Result::Unsupported;

  *outBindings = entry->publicBindings;
  return Result::Success;
}

Result GLDevice::getShaderSourceInfo(const ShaderModuleHandle module, ShaderSourceInfo* outInfo) {
  if (outInfo == nullptr) return Result::InvalidArgument;

  const GLShaderModule* entry = mShaderModules.get(module);
  if (entry == nullptr) return Result::InvalidHandle;

  // A source module reports its parsed GLSL provenance; a SPIR-V input reports `spirvInput` plus the
  // hash/size of the words. A direct-GLSL module also counts: its text was parsed for the version and
  // hashed, so every field of the report is a real answer rather than a placeholder.
  if (!entry->spirvCompiledFromSource && !entry->directGlsl && entry->inputSpirv.empty()) {
    return Result::Unsupported;
  }

  *outInfo = entry->sourceInfo;
  return Result::Success;
}

Result GLDevice::getShaderSpirv(const ShaderModuleHandle module, std::vector<uint32_t>* outWords) {
  if (outWords == nullptr) return Result::InvalidArgument;

  const GLShaderModule* entry = mShaderModules.get(module);
  if (entry == nullptr) return Result::InvalidHandle;

  // Unsupported only for a direct-GLSL module: it never had SPIR-V words. A caller-supplied SPIR-V
  // module gets its own words back, matching the Vulkan backend's uniform provenance query.
  if (!entry->spirvCompiledFromSource && entry->inputSpirv.empty()) return Result::Unsupported;

  *outWords = entry->spirvCompiledFromSource ? entry->compiledSpirv : entry->inputSpirv;
  return Result::Success;
}

void GLDevice::destroyShaderModule(const ShaderModuleHandle module) {
  GLShaderModule* entry = mShaderModules.get(module);
  if (entry == nullptr) {
    return;
  }
  if (entry->shaderObject != 0 && gl::getGLApi().glDeleteShader != nullptr) {
    gl::getGLApi().glDeleteShader(entry->shaderObject);
  }
  entry->shaderObject = 0;
  mShaderModules.free(module);
}

// ==================================================================================================
// Programs
// ==================================================================================================

const GLUniformBinding* GLProgram::findBinding(const uint32_t set, const uint32_t binding) const noexcept {
  for (const GLUniformBinding& entry: uniformBindings) {
    if (entry.set == set && entry.binding == binding) {
      return &entry;
    }
  }
  return nullptr;
}

Result GLDevice::linkProgram(const GLShaderModule& vertexModule, const GLShaderModule& fragmentModule,
  GLProgram* outProgram) const {
  gl::GLApi& api = gl::getGLApi();
  const GLuint program = api.glCreateProgram();
  if (program == 0) {
    return Result::Error;
  }

  const auto attach = [&api, program](const GLShaderModule& module, bool* attached) {
    if (module.shaderObject == 0) {
      return;
    }
    api.glAttachShader(program, module.shaderObject);
    *attached = true;
  };

  bool vertexAttached = false;
  bool fragmentAttached = false;
  attach(vertexModule, &vertexAttached);
  attach(fragmentModule, &fragmentAttached);

  if (!vertexAttached && !fragmentAttached) {
    api.glDeleteProgram(program);
    emitMessage(MessageSeverity::Error, "a render pipeline needs at least one usable shader stage");
    return Result::ShaderCompilationFailed;
  }

  // Vertex input locations, pinned *before* the link because that is the only moment glBindAttribLocation
  // has any effect - the assignment is baked in when the program is linked, and calling it afterwards is
  // a silent no-op.
  //
  // Only a `GLSLDirect` module has any of these, and it needs them: a translated module carries
  // `layout(location = N)` from the SPIR-V decoration, while a source handed straight to the driver may
  // have no way to state one at all (ESSL 1.00 has no layout qualifiers) or may simply not have written
  // one. Without this the location would fall to whatever the driver assigned, which is neither what the
  // pipeline's `VertexAttribute::shaderLocation` says nor the same from driver to driver.
  for (const ShaderAttributeDeclaration& attribute: vertexModule.attributes) {
    if (attribute.name == nullptr || api.glBindAttribLocation == nullptr) {
      continue;
    }
    api.glBindAttribLocation(program, attribute.location, attribute.name);
  }

  api.glLinkProgram(program);

  GLint status = 0;
  api.glGetProgramiv(program, GL_LINK_STATUS, &status);
  if (status == 0) {
    GLint logLength = 0;
    api.glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
    std::string log(size_t(logLength > 1 ? logLength : 1), '\0');
    api.glGetProgramInfoLog(program, GLsizei(log.size()), nullptr, log.data());
    std::string message = "shader program link failed: ";
    message += log;
    emitMessage(MessageSeverity::Error, message.c_str());
    api.glDeleteProgram(program);
    return Result::ShaderCompilationFailed;
  }

  // Detach, but do not delete. A shader module is an input that more than one pipeline may name --
  // the canvas reuses one vertex module across every program that shares its vertex layout -- and
  // deleting the object on the first link left the second pipeline with no vertex stage at all. It
  // still linked, because a fragment-only program is legal, and drew nothing; that is the failure
  // the indexed-draw test caught on Mesa. The module owns the object until destroyShaderModule().
  if (vertexAttached) {
    api.glDetachShader(program, vertexModule.shaderObject);
  }
  if (fragmentAttached) {
    api.glDetachShader(program, fragmentModule.shaderObject);
  }

  // ## Verifying the caller's name -> location table, which is otherwise silently wrong
  //
  // `glBindAttribLocation` ignores a name the vertex shader does not declare, and so does the driver's
  // own assignment: the attribute simply never reaches the pipeline. The result is geometry read from
  // the wrong buffer or from none at all, with no error anywhere - a manifest with `"aPosition"` where
  // the shader spells `aPos` fails exactly this way, and it is the failure the whole declaration table
  // invites, because both halves of the pair are now typed by hand.
  //
  // So each declaration is checked against the linked program. A name that did not survive is reported
  // at pipeline creation, where the caller can see which shader it came from, instead of at the first
  // draw as an empty screen.
  if (vertexAttached && api.glGetAttribLocation != nullptr) {
    for (const ShaderAttributeDeclaration& attribute: vertexModule.attributes) {
      if (attribute.name == nullptr) {
        continue;
      }
      const GLint resolved = api.glGetAttribLocation(program, attribute.name);
      if (resolved < 0) {
        std::string message = "the vertex shader declares no attribute named \"";
        message += attribute.name;
        message += "\", so the location declared for it (";
        message += std::to_string(attribute.location);
        message += ") was not applied";
        if (!vertexModule.label.empty()) {
          message += " [";
          message += vertexModule.label;
          message += ']';
        }
        emitMessage(MessageSeverity::Warning, message.c_str());
      } else if (resolved != static_cast<GLint>(attribute.location)) {
        // Cannot normally happen - the binding is issued before the link - but if a driver ignored the
        // call, the pipeline's vertex input state is about to disagree with the program, and that is
        // worth a line rather than a mystery.
        std::string message = "attribute \"";
        message += attribute.name;
        message += "\" resolved to location ";
        message += std::to_string(resolved);
        message += " rather than the declared ";
        message += std::to_string(attribute.location);
        emitMessage(MessageSeverity::Warning, message.c_str());
      }
    }
  }

  outProgram->program = program;
  outProgram->hasFragmentStage = fragmentAttached;
  return Result::Success;
}

Result GLDevice::linkComputeProgram(const GLShaderModule& computeModule, GLProgram* outProgram) const {
  gl::GLApi& api = gl::getGLApi();
  if (!mCapabilities.supports(Feature::ComputeShader)) {
    return Result::Unsupported;
  }

  const GLuint program = api.glCreateProgram();
  if (program == 0) return Result::Error;

  api.glAttachShader(program, computeModule.shaderObject);
  api.glLinkProgram(program);

  GLint status = 0;
  api.glGetProgramiv(program, GL_LINK_STATUS, &status);
  if (status == 0) {
    GLint logLength = 0;
    api.glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
    std::string log(static_cast<size_t>(logLength > 1 ? logLength : 1), '\0');
    api.glGetProgramInfoLog(program, static_cast<GLsizei>(log.size()), nullptr, log.data());
    std::string message = "compute program link failed: ";
    message += log;
    emitMessage(MessageSeverity::Error, message.c_str());
    api.glDeleteProgram(program);
    return Result::ShaderCompilationFailed;
  }

  // Detached, not deleted: the module may be linked into another pipeline. See linkProgram.
  api.glDetachShader(program, computeModule.shaderObject);

  outProgram->program = program;
  outProgram->hasFragmentStage = false;
  return Result::Success;
}

Result GLDevice::buildBindingTable(const GLuint program, const GLShaderModule* vertexModule,
  const GLShaderModule* fragmentModule, const GLShaderModule* computeModule, GLProgram* outProgram) const {
  gl::GLApi& api = gl::getGLApi();

  // Reflection from both stages is merged, because a binding consumed by the vertex stage and
  // another by the fragment stage share one descriptor set namespace in the upper layer's view but
  // two separate namespaces in GLSL.
  const GLShaderModule* modules[3] = { vertexModule, fragmentModule, computeModule };

  uint32_t nextTextureUnit = 0;
  const uint32_t maxTextureUnits = std::min(mCapabilities.maxCombinedTextureUnits, 32u);

  /**
   * Which texture units this program has already handed out.
   *
   * A caller-supplied declaration may name a unit, and the automatic assignment has to leave it alone:
   * two sampler bindings on one unit is a program that samples the wrong texture, and it is the kind of
   * mistake that produces a plausible-looking image rather than an error. So the units are tracked
   * rather than counted, and both routes go through the same allocator.
   */
  std::vector<bool> claimedTextureUnits(maxTextureUnits, false);

  /**
   * @return the texture unit to put @p reflected in, or -1 when the program cannot have one.
   *
   * The declared location wins when there is one, because a manifest that names a unit is expressing a
   * requirement - usually that the caller has already bound something there - rather than a preference.
   */
  const auto acquireTextureUnit = [&](const ShaderBindingInfo& reflected) -> GLint {
    if (reflected.location != kNoShaderLocation) {
      if (reflected.location < 0 || static_cast<uint32_t>(reflected.location) >= maxTextureUnits) {
        std::string message = "the declaration for \"";
        message += reflected.name;
        message += "\" asks for texture unit ";
        message += std::to_string(reflected.location);
        message += ", which this device does not have (it provides ";
        message += std::to_string(maxTextureUnits);
        message += "); the binding is left unbound";
        emitMessage(MessageSeverity::Warning, message.c_str());
        return -1;
      }
      const auto unit = static_cast<uint32_t>(reflected.location);
      if (claimedTextureUnits[unit]) {
        std::string message = "texture unit ";
        message += std::to_string(unit);
        message += " is declared by more than one binding; \"";
        message += reflected.name;
        message += "\" and the earlier one will sample the same texture";
        emitMessage(MessageSeverity::Warning, message.c_str());
      }
      claimedTextureUnits[unit] = true;
      return static_cast<GLint>(unit);
    }

    while (nextTextureUnit < maxTextureUnits && claimedTextureUnits[nextTextureUnit]) {
      ++nextTextureUnit;
    }
    if (nextTextureUnit >= maxTextureUnits) {
      emitMessage(MessageSeverity::Warning,
        "the program needs more texture units than the device provides; the "
        "excess bindings will read an unbound texture");
      return -1;
    }
    claimedTextureUnits[nextTextureUnit] = true;
    return static_cast<GLint>(nextTextureUnit++);
  };

  for (const GLShaderModule* module: modules) {
    if (module == nullptr) {
      continue;
    }

    for (const ShaderBindingInfo& reflected: module->reflection.bindings) {
      switch (reflected.type) {
        case ShaderBindingType::StageInput:
        case ShaderBindingType::StageOutput:
          // Stage interfaces are wired up as vertex attributes.
          continue;
        case ShaderBindingType::PushConstant:
          // Handled by its own case below rather than skipped: see `GLProgram::pushConstantBlockIndex`.
          break;
        default:
          break;
      }

      // A binding declared by both stages is one descriptor as far as the upper layer is concerned,
      // so the first occurrence wins and the second is skipped.
      if (outProgram->findBinding(reflected.set, reflected.binding) != nullptr) {
        continue;
      }

      GLUniformBinding binding;
      binding.set = reflected.set;
      binding.binding = reflected.binding;
      binding.arraySize = reflected.arraySize;
      // The GL binding point a block occupies, before any declaration overrides it. Defaulted here
      // rather than in each block case below because a storage buffer and a uniform block both need it
      // and only one of them used to set it: the draw path calls `glBindBufferBase` with this number for
      // both, so a default of zero would have bound every storage buffer to binding point 0.
      binding.blockBinding = reflected.binding;

      switch (reflected.type) {
        case ShaderBindingType::PushConstant: {
          // Emulated, and *not* a descriptor: the block is recorded on the program so the replay can fill it
          // from whatever `pushConstants` recorded, before each draw and independently of any set. It used
          // to be skipped with a note saying it was "resolved through the uniform buffer path below", which
          // no path did -- so the advertised budget and the validation were the whole of the feature, and a
          // shader's push data silently read as zeros.
          const GLuint blockIndex = api.glGetUniformBlockIndex != nullptr
                                      ? api.glGetUniformBlockIndex(program, reflected.name.c_str())
                                      : 0xFFFFFFFFu;
          outProgram->pushConstantBlockIndex = blockIndex;
          outProgram->pushConstantBlockBinding = reflected.location != kNoShaderLocation
                                                   ? static_cast<GLuint>(reflected.location)
                                                   : kPushConstantBinding;
          {
            std::string message = "push constant block \"";
            message += reflected.name;
            message += "\" -> uniform block index ";
            message += std::to_string(blockIndex);
            message += ", binding point ";
            message += std::to_string(outProgram->pushConstantBlockBinding);
            emitMessage(MessageSeverity::Verbose, message.c_str());
          }
          if (blockIndex != 0xFFFFFFFFu && api.glUniformBlockBinding != nullptr) {
            api.glUniformBlockBinding(program, blockIndex, outProgram->pushConstantBlockBinding);
          }
          continue;
        }
        case ShaderBindingType::UniformBuffer: {
          binding.type = BindingType::UniformBuffer;
          const GLuint blockIndex = api.glGetUniformBlockIndex != nullptr
                                      ? api.glGetUniformBlockIndex(program, reflected.name.c_str())
                                      : 0xFFFFFFFFu;
          binding.blockIndex = blockIndex;
          /**
           * Which GL uniform-block binding this block occupies.
           *
           * Its descriptor binding number by default, which is what lets `bindDescriptorSet` call
           * `glBindBufferBase` with the binding number directly. A declaration that named a location
           * overrides it, and then the two are different numbers on purpose: the descriptor set is
           * still indexed by (set, binding), while the driver is told the block lives at the location
           * the manifest asked for.
           */
          if (reflected.location != kNoShaderLocation) {
            binding.blockBinding = static_cast<GLuint>(reflected.location);
          }
          if (blockIndex != 0xFFFFFFFFu && api.glUniformBlockBinding != nullptr) {
            api.glUniformBlockBinding(program, blockIndex, binding.blockBinding);
            GLint blockSize = 0;
            api.glGetActiveUniformBlockiv(program, blockIndex, GL_UNIFORM_BLOCK_DATA_SIZE, &blockSize);
            // A permanent diagnostic, not a temporary one: the *shader's* view of a block is not derivable from the host
            // struct, and the two have disagreed here before. `GradientUniforms` was 288 bytes to the driver against 208
            // to C++, because `std140` rounds a scalar array's element stride up to the alignment of a `vec4` and the
            // host had written the six matrix coefficients tightly packed -- so every member after them was read from
            // the wrong byte and a two-stop gradient painted flat black while every CPU-side trace said otherwise. The
            // block declares only `vec4` members now and the two agree; this print is what says so, and what will say so
            // again if either side moves.
            //
            //   set CORAL_GL_TRACE_BLOCKS=1
            if (std::getenv("CORAL_GL_TRACE_BLOCKS") != nullptr) {
              std::fprintf(stderr, "[block] binding=%u name=%s index=0x%08x size=%d stage=%d\n", reflected.binding,
                      reflected.name.c_str(), blockIndex, blockSize, static_cast<int>(module->stage));
              // ## And every member's offset, which is the other half of "does the host agree with the shader"
              //
              // A block's *size* says whether a descriptor range can cover it; a member's offset says whether the host's
              // `offsetof` matches where the shader compiler actually put it. The two are independent, and the second is
              // what a `std140` mismatch looks like: the range is big enough, the write happens, and the shader reads a
              // neighbouring member (`shaders/canvas/gradient.frag`'s note says exactly this, and asks for these numbers).
              //
              // `GL_UNIFORM_ARRAY_STRIDE` is printed because it is the classic gap: `std140` rounds an array's element
              // stride up to sixteen bytes, so `float uInverseLocal[6]` is 96 bytes to a strict compiler and 24 to a host
              // that writes it flat.
              GLint memberCount = 0;
              api.glGetActiveUniformBlockiv(program, blockIndex, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &memberCount);
              if (api.glGetActiveUniformsiv == nullptr) {
                std::fprintf(stderr, "[block]   the member offsets need glGetActiveUniformsiv, which this device did not "
                                     "resolve (active uniforms: %d)\n",
                        memberCount);
              } else {
                std::vector<GLuint> indices(static_cast<size_t>(std::max(0, memberCount)), 0);
                if (memberCount > 0) {
                  api.glGetActiveUniformBlockiv(program, blockIndex, GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES,
                          reinterpret_cast<GLint*>(indices.data()));
                }
                std::vector<GLint> offsets(indices.size(), 0);
                std::vector<GLint> strides(indices.size(), 0);
                std::vector<GLint> sizes(indices.size(), 0);
                if (!indices.empty()) {
                  api.glGetActiveUniformsiv(program, static_cast<GLsizei>(indices.size()), indices.data(),
                          GL_UNIFORM_OFFSET, offsets.data());
                  api.glGetActiveUniformsiv(program, static_cast<GLsizei>(indices.size()), indices.data(),
                          GL_UNIFORM_ARRAY_STRIDE, strides.data());
                  api.glGetActiveUniformsiv(program, static_cast<GLsizei>(indices.size()), indices.data(),
                          GL_UNIFORM_SIZE, sizes.data());
                }
                for (size_t member = 0; member < indices.size(); ++member) {
                  GLsizei nameLength = 0;
                  GLint nameSize = 0;
                  GLenum nameType = 0;
                  char name[128] = {};
                  api.glGetActiveUniform(program, indices[member], static_cast<GLsizei>(sizeof(name) - 1), &nameLength,
                          &nameSize, &nameType, name);
                  std::fprintf(stderr, "[block]   %-24s offset=%-5d stride=%-5d size=%-4d\n", name, offsets[member],
                          strides[member], sizes[member]);
                }
              }
              std::fflush(stderr);
            }
            if (blockSize > 0)
              outProgram->uniformBlockSize = std::max(outProgram->uniformBlockSize,
                static_cast<uint32_t>(blockSize));
          }
          break;
        }

        case ShaderBindingType::SampledTexture:
        case ShaderBindingType::TextureSampler: {
          binding.type = BindingType::TextureSampler;
          binding.location = api.glGetUniformLocation(program, reflected.name.c_str());
          if (binding.location < 0 && reflected.arraySize > 1) {
            // Some drivers only expose the "name[0]" form for an arrayed sampler uniform.
            std::string elementName = reflected.name + "[0]";
            binding.location = api.glGetUniformLocation(program, elementName.c_str());
          }
          if (binding.location < 0) {
            // The uniform did not survive the link, which for a caller-supplied table usually means the
            // name does not match the source. Worth a line for the source samplers: the binding will
            // silently read an unbound texture otherwise, and "the sampler is black" is not a diagnosable
            // symptom. Optional samplers are different: the effect common header always declares `uParams`,
            // but a shader that never calls `effectSlot` leaves it unused and the linker removes it.
            if (!reflected.name.empty() && !module->spirvTranslated
                && reflected.name.rfind("uInput", 0) == 0) {
              std::string message = "the linked program has no sampler uniform named \"";
              message += reflected.name;
              message += "\", so that binding cannot be given a texture unit";
              emitMessage(MessageSeverity::Warning, message.c_str());
            }
          } else {
            const GLint unit = acquireTextureUnit(reflected);
            if (unit >= 0) {
              binding.textureUnit = unit;
              api.glUseProgram(program);
              api.glUniform1i(binding.location, binding.textureUnit);
              api.glUseProgram(0);
            }
          }
          break;
        }

        case ShaderBindingType::Sampler: {
          binding.type = BindingType::Sampler;
          binding.location = api.glGetUniformLocation(program, reflected.name.c_str());
          break;
        }

        case ShaderBindingType::StorageBuffer:
        case ShaderBindingType::ReadOnlyStorageBuffer: {
          binding.type = BindingType::StorageBuffer;
          // Storage buffers need glShaderStorageBlockBinding, which is ES 3.1 only. When the device
          // lacks it the binding is recorded so the failure is reported at bind time rather than
          // silently producing an empty buffer.
          const GLuint blockIndex = api.glGetUniformBlockIndex != nullptr
                                      ? api.glGetUniformBlockIndex(program, reflected.name.c_str())
                                      : 0xFFFFFFFFu;
          binding.blockIndex = blockIndex;
          break;
        }

        default:
          continue;
      }

      outProgram->uniformBindings.push_back(binding);
    }
  }

  // Counted from the claimed units rather than from `nextTextureUnit`, which is only a search cursor
  // and stops short of an explicitly declared unit it happened to skip over.
  outProgram->usedTextureUnits = static_cast<uint32_t>(
          std::count(claimedTextureUnits.begin(), claimedTextureUnits.end(), true));
  return Result::Success;
}

Result GLDevice::createRenderPipeline(const RenderPipelineDescriptor& descriptor, RenderPipelineHandle* outPipeline) {
  if (outPipeline == nullptr) {
    return Result::InvalidArgument;
  }
  *outPipeline = RenderPipelineHandle::invalid();
  if (!mInitialized) {
    return Result::Error;
  }

  const RenderPassDescriptor* passDescriptor = nullptr;
  RenderPassDescriptor resolvedPass;
  if (const GLRenderPass* pass = mRenderPasses.get(descriptor.compatibleRenderPass)) {
    resolvedPass.colorAttachmentCount = pass->colorAttachmentCount;
    for (uint32_t index = 0; index < pass->colorAttachmentCount; ++index) {
      resolvedPass.colorFormats[index] = pass->colorFormats[index];
    }
    resolvedPass.depthStencilFormat = pass->depthStencilFormat;
    resolvedPass.sampleCount = pass->sampleCount;
    passDescriptor = &resolvedPass;
  }

  const ValidationResult validation = validateRenderPipeline(descriptor, mCapabilities, passDescriptor);
  if (!validation.valid) {
    // A validation message that names the field is what turns the renderer's bare `Unsupported` into something
    // actionable. The format is printed as a number *and* as its name, because a number no caller recognises is
    // exactly the case that hides a garbage value: an in-range-but-wrong format and an out-of-range one have the
    // same symptom and completely different causes.
    char detail[192];
    std::snprintf(detail, sizeof(detail),
            " [colour target 0 format = %d, targets = %u, depthStencilFormat = %d, compatible pass = %s]",
            descriptor.colorTargetCount > 0 ? static_cast<int>(descriptor.colorTargets[0].format) : -1,
            descriptor.colorTargetCount, static_cast<int>(descriptor.depthStencilFormat),
            passDescriptor != nullptr ? "resolved" : "NOT FOUND");
    const std::string message = std::string(validation.message) + detail;
    emitMessage(MessageSeverity::Error, message.c_str());
    return Result::Unsupported;
  }

  GLShaderModule* vertexModule = mShaderModules.get(descriptor.vertexModule);
  GLShaderModule* fragmentModule = mShaderModules.get(descriptor.fragmentModule);
  if (descriptor.vertexModule.valid() && vertexModule == nullptr) return Result::InvalidHandle;
  if (descriptor.fragmentModule.valid() && fragmentModule == nullptr) return Result::InvalidHandle;

  GLRenderPipeline pipeline;
  pipeline.layout = descriptor.layout;
  pipeline.vertexInput = descriptor.vertexInput;
  pipeline.rasterizer = descriptor.rasterizer;
  pipeline.depthStencil = descriptor.depthStencil;
  pipeline.multisample = descriptor.multisample;
  pipeline.colorTargetCount = descriptor.colorTargetCount;
  pipeline.depthStencilFormat = descriptor.depthStencilFormat;
  for (uint32_t index = 0; index < descriptor.colorTargetCount; ++index) {
    pipeline.colorTargets[index] = descriptor.colorTargets[index];
  }
  for (uint32_t index = 0; index < 4; ++index) {
    pipeline.colorWriteMask[index] =
      index < descriptor.colorTargetCount ? (descriptor.colorTargets[index].writeMask & 0x1) != 0 : true;
    if (index < descriptor.colorTargetCount) {
      pipeline.colorWriteMask[index] = true; // resolved per channel at bind time instead
    }
  }
  if (descriptor.label != nullptr) {
    pipeline.label = descriptor.label;
  }

  const Result linkResult = linkProgram(vertexModule != nullptr ? *vertexModule : GLShaderModule{},
    fragmentModule != nullptr ? *fragmentModule : GLShaderModule{}, &pipeline.program);
  if (!isSuccess(linkResult)) {
    return linkResult;
  }
  pipeline.program.label = pipeline.label;

  // Vertex attribute locations are taken from the shader itself, not forced with glBindAttribLocation.
  //
  // SPIRV-Cross preserves the SPIR-V `Location` decoration as `layout(location = N) in ...` when
  // targeting GLSL ES 3.00, and the RHI's VertexAttribute::shaderLocation is derived from that same
  // decoration by the shader authoring tool. The two agree by construction, so binding by name would
  // be redundant - and worse, it would only work for GLSL that follows a naming convention.
  //
  // The one shader that gets its locations forced is a `GLSLDirect` module, and it is forced in
  // `linkProgram` rather than here because `glBindAttribLocation` only takes effect before a link -
  // which has already happened by the time this function runs. Such a module has no SPIR-V decoration
  // to inherit a location from, so the caller's declaration table is the only statement of where its
  // inputs live; `linkProgram` applies it and warns about any name the vertex stage does not declare.
  //
  // The locations are verified below instead: a mismatch between the pipeline's declared
  // shaderLocation and what the linked program actually exposes is reported here, at pipeline
  // creation, rather than producing garbage geometry at the first draw.
  gl::GLApi& api = gl::getGLApi();
  if (vertexModule != nullptr && vertexModule->shaderObject == 0) {
    // Already linked or already consumed; the reflection data below is still valid.
  }

  for (uint32_t index = 0; index < descriptor.vertexInput.attributeCount; ++index) {
    const VertexAttribute& attribute = descriptor.vertexInput.attributes[index];
    const uint32_t location = attribute.shaderLocation;
    if (location >= mCapabilities.limits.maxVertexAttributes) {
      std::string message = "vertex attribute location ";
      message += std::to_string(location);
      message += " exceeds the device's maximum attribute count";
      emitMessage(MessageSeverity::Error, message.c_str());
      api.glDeleteProgram(pipeline.program.program);
      return Result::Unsupported;
    }
  }

  const Result tableResult =
    buildBindingTable(pipeline.program.program, vertexModule, fragmentModule, nullptr, &pipeline.program);
  if (!isSuccess(tableResult)) {
    api.glDeleteProgram(pipeline.program.program);
    return tableResult;
  }

  *outPipeline = mRenderPipelines.allocate(std::move(pipeline));
  return outPipeline->valid() ? Result::Success : Result::OutOfMemory;
}

void GLDevice::destroyRenderPipeline(RenderPipelineHandle pipelineHandle) {
  GLRenderPipeline* entry = mRenderPipelines.get(pipelineHandle);
  if (entry == nullptr) {
    return;
  }
  if (entry->program.program != 0 && gl::getGLApi().glDeleteProgram != nullptr) {
    gl::getGLApi().glDeleteProgram(entry->program.program);
  }
  entry->program.program = 0;
  mRenderPipelines.free(pipelineHandle);
}

Result GLDevice::createComputePipeline(const ComputePipelineDescriptor& descriptor,
  ComputePipelineHandle* outPipeline) {
  if (outPipeline == nullptr) {
    return Result::InvalidArgument;
  }
  *outPipeline = ComputePipelineHandle::invalid();

  // Compute is reported as unavailable on the ES 3.0 baseline, and the validator says exactly why.
  // Returning Unsupported here rather than pretending means an upper layer's `supports(ComputeShader)`
  // check and the actual create call can never disagree.
  const ValidationResult validation = validateComputePipeline(descriptor, mCapabilities);
  if (!validation.valid) {
    emitMessage(MessageSeverity::Error, validation.message);
    return Result::Unsupported;
  }

  GLShaderModule* module = mShaderModules.get(descriptor.computeModule);
  if (module == nullptr) {
    return Result::InvalidHandle;
  }

  GLComputePipeline pipeline;
  pipeline.layout = descriptor.layout;
  pipeline.workgroupSize[0] = descriptor.workgroupSizeX;
  pipeline.workgroupSize[1] = descriptor.workgroupSizeY;
  pipeline.workgroupSize[2] = descriptor.workgroupSizeZ;
  if (descriptor.label != nullptr) {
    pipeline.label = descriptor.label;
  }

  const Result linkResult = linkComputeProgram(*module, &pipeline.program);
  if (!isSuccess(linkResult)) {
    return linkResult;
  }

  const Result tableResult = buildBindingTable(pipeline.program.program, nullptr, nullptr, module, &pipeline.program);
  if (!isSuccess(tableResult)) {
    gl::getGLApi().glDeleteProgram(pipeline.program.program);
    return tableResult;
  }

  *outPipeline = mComputePipelines.allocate(std::move(pipeline));
  return outPipeline->valid() ? Result::Success : Result::OutOfMemory;
}

void GLDevice::destroyComputePipeline(ComputePipelineHandle pipelineHandle) {
  GLComputePipeline* entry = mComputePipelines.get(pipelineHandle);
  if (entry == nullptr) {
    return;
  }
  if (entry->program.program != 0 && gl::getGLApi().glDeleteProgram != nullptr) {
    gl::getGLApi().glDeleteProgram(entry->program.program);
  }
  entry->program.program = 0;
  mComputePipelines.free(pipelineHandle);
}

// ==================================================================================================
// Descriptor sets
// ==================================================================================================

Result GLDevice::createDescriptorSetLayout(const DescriptorSetLayoutDescriptor& descriptor,
  DescriptorSetLayoutHandle* outLayout) {
  if (outLayout == nullptr) {
    return Result::InvalidArgument;
  }
  *outLayout = DescriptorSetLayoutHandle::invalid();

  const ValidationResult validation = validateDescriptorSetLayout(descriptor, mCapabilities);
  if (!validation.valid) {
    emitMessage(MessageSeverity::Error, validation.message);
    return Result::Unsupported;
  }

  GLDescriptorSetLayout layout;
  layout.bindingCount = descriptor.bindingCount;
  for (uint32_t index = 0; index < descriptor.bindingCount; ++index) {
    layout.bindings[index] = descriptor.bindings[index];

    const Feature required = featureForBindingType(descriptor.bindings[index].type);
    if (required != Feature::None && !mCapabilities.supports(required)) {
      std::string message = "descriptor binding ";
      message += std::to_string(descriptor.bindings[index].binding);
      message += " needs feature ";
      message += toString(required);
      message += ", which is unavailable: ";
      message += toString(mCapabilities.reasonFor(required));
      emitMessage(MessageSeverity::Error, message.c_str());
      return Result::Unsupported;
    }
  }
  if (descriptor.label != nullptr) {
    layout.label = descriptor.label;
  }

  *outLayout = mDescriptorSetLayouts.allocate(std::move(layout));
  return outLayout->valid() ? Result::Success : Result::OutOfMemory;
}

void GLDevice::destroyDescriptorSetLayout(const DescriptorSetLayoutHandle layout) {
  mDescriptorSetLayouts.free(layout);
}

Result GLDevice::createDescriptorSet(const DescriptorSetLayoutHandle layout, DescriptorSetHandle* outSet) {
  if (outSet == nullptr) {
    return Result::InvalidArgument;
  }
  *outSet = DescriptorSetHandle::invalid();

  const GLDescriptorSetLayout* layoutEntry = mDescriptorSetLayouts.get(layout);
  if (layoutEntry == nullptr) {
    return Result::InvalidHandle;
  }

  GLDescriptorSet set;
  set.layout = layout;

  // A OpenGL descriptor set is a plain CPU-side array: OpenGL has no descriptor set object, so the
  // backend keeps the resolved values and applies them at draw time. Nothing is allocated on the
  // device, which makes set creation and destruction essentially free.
  for (uint32_t index = 0; index < layoutEntry->bindingCount; ++index) {
    const DescriptorBinding& declaration = layoutEntry->bindings[index];
    for (uint32_t element = 0; element < std::max(declaration.count, 1u); ++element) {
      GLDescriptorBinding binding;
      binding.binding = declaration.binding;
      binding.type = declaration.type;
      binding.arraySize = declaration.count;
      set.bindings.push_back(binding);
    }
  }
  if (layoutEntry->label.empty()) {
    set.label = layoutEntry->label;
  }

  *outSet = mDescriptorSets.allocate(std::move(set));
  return outSet->valid() ? Result::Success : Result::OutOfMemory;
}

void GLDevice::destroyDescriptorSet(const DescriptorSetHandle set) { mDescriptorSets.free(set); }

Result GLDevice::updateDescriptorSet(const DescriptorSetHandle setHandle, const DescriptorWrite* writes,
  const uint32_t writeCount) {
  GLDescriptorSet* set = mDescriptorSets.get(setHandle);
  if (set == nullptr) {
    return Result::InvalidHandle;
  }
  if (writes == nullptr && writeCount != 0) {
    return Result::InvalidArgument;
  }

  for (uint32_t index = 0; index < writeCount; ++index) {
    const DescriptorWrite& write = writes[index];
    // The buffer may be larger than the block size; the *range* is what has to fit, and a range past it is
    // `GL_INVALID_VALUE` at draw time rather than here.
    if (const ValidationResult validation = validateDescriptorWrite(write, mCapabilities); !validation.valid) {
      emitMessage(MessageSeverity::Error, validation.message);
      return Result::InvalidArgument;
    }

    // Each write targets one element of one binding; the element index selects the entry that was
    // expanded for an arrayed binding at set creation time.
    GLDescriptorBinding* target = nullptr;
    uint32_t element = 0;
    for (GLDescriptorBinding& candidate: set->bindings) {
      if (candidate.binding != write.binding) continue;
      if (element == write.arrayElement) {
        target = &candidate;
        break;
      }
      ++element;
    }

    if (target == nullptr) {
      std::string message = "no descriptor binding ";
      message += std::to_string(write.binding);
      message += " exists in this set";
      emitMessage(MessageSeverity::Error, message.c_str());
      return Result::InvalidArgument;
    }

    switch (write.type) {
      case BindingType::TextureSampler: {
        // `DescriptorWrite::view` takes precedence over `texture` -- that is the contract the record
        // type states, and it is what lets a caller bind a texture it does not own the handle of. GL
        // has no separate view object to bind, so the view is resolved to the texture it names here.
        // An external texture written as a view-only write used to fail validation on this backend,
        // which made the draw sample nothing with no error at the call site.
        TextureHandle textureHandle = write.texture;
        if (write.view.valid()) {
          const GLTextureView* view = textureView(write.view);
          if (view == nullptr) {
            std::string message = "descriptor binding ";
            message += std::to_string(write.binding);
            message += " names a view this device does not own";
            emitMessage(MessageSeverity::Error, message.c_str());
            return Result::InvalidHandle;
          }
          textureHandle = view->texture;
        }
        if (const GLTexture* texture = mTextures.get(textureHandle); texture == nullptr) {
          std::string message = "descriptor binding ";
          message += std::to_string(write.binding);
          message += " names texture ";
          message += std::to_string(static_cast<uint64_t>(textureHandle.index));
          message += " which this device does not own";
          emitMessage(MessageSeverity::Error, message.c_str());
          return Result::InvalidHandle;
        }
        if (write.sampler.valid() && mSamplers.get(write.sampler) == nullptr) {
          std::string message = "descriptor binding ";
          message += std::to_string(write.binding);
          message += " names a sampler this device does not own";
          emitMessage(MessageSeverity::Error, message.c_str());
          return Result::InvalidHandle;
        }
        target->texture = textureHandle;
        target->baseTexture = textureHandle;
        target->view = write.view;
        target->sampler = write.sampler;
        break;
      }
      case BindingType::Texture: {
        if (mTextures.get(write.texture) == nullptr) {
          return Result::InvalidHandle;
        }
        target->texture = write.texture;
        target->baseTexture = write.texture;
        if (write.sampler.valid()) {
          target->sampler = write.sampler;
        }
        break;
      }
      case BindingType::Sampler: {
        if (mSamplers.get(write.sampler) == nullptr) {
          return Result::InvalidHandle;
        }
        target->sampler = write.sampler;
        break;
      }
      case BindingType::UniformBuffer:
      case BindingType::ReadOnlyStorageBuffer:
      case BindingType::StorageBuffer: {
        // A null buffer is legal and means "unbind", which is how a renderer releases a per-frame
        // buffer at the end of a frame.
        if (write.buffer.buffer.valid() && mBuffers.get(write.buffer.buffer) == nullptr) {
          return Result::InvalidHandle;
        }
        target->buffer = write.buffer;
        break;
      }
    }

    target->written = true;
  }

  return Result::Success;
}

Result GLDevice::createPipelineLayout(const PipelineLayoutDescriptor& descriptor, PipelineLayoutHandle* outLayout) {
  if (outLayout == nullptr) {
    return Result::InvalidArgument;
  }
  *outLayout = PipelineLayoutHandle::invalid();

  if (descriptor.setLayoutCount > kMaxDescriptorSetLayouts) {
    emitMessage(MessageSeverity::Error, "the pipeline layout declares too many descriptor sets");
    return Result::InvalidArgument;
  }
  if (descriptor.setLayoutCount > mCapabilities.limits.maxBoundDescriptorSets) {
    emitMessage(MessageSeverity::Error,
      "the pipeline layout needs more simultaneously bound descriptor sets than the device supports");
    return Result::Unsupported;
  }

  GLPipelineLayout layout;
  layout.setLayoutCount = descriptor.setLayoutCount;
  for (uint32_t index = 0; index < descriptor.setLayoutCount; ++index) {
    // An invalid entry is allowed: it means "this set index is unused", which is common when a
    // renderer reserves indices for optional feature sets.
    if (descriptor.setLayouts[index].valid() && mDescriptorSetLayouts.get(descriptor.setLayouts[index]) == nullptr) {
      return Result::InvalidHandle;
    }
    layout.setLayouts[index] = descriptor.setLayouts[index];
  }
  if (descriptor.label != nullptr) {
    layout.label = descriptor.label;
  }

  *outLayout = mPipelineLayouts.allocate(std::move(layout));
  return outLayout->valid() ? Result::Success : Result::OutOfMemory;
}

void GLDevice::destroyPipelineLayout(const PipelineLayoutHandle layout) { mPipelineLayouts.free(layout); }

// ==================================================================================================
// Render passes and framebuffers
// ==================================================================================================

Result GLDevice::createRenderPass(const RenderPassDescriptor& descriptor, RenderPassHandle* outPass) {
  if (outPass == nullptr) {
    return Result::InvalidArgument;
  }
  *outPass = RenderPassHandle::invalid();

  const ValidationResult validation = validateRenderPassDescriptor(descriptor, mCapabilities);
  if (!validation.valid) {
    emitMessage(MessageSeverity::Error, validation.message);
    return Result::Unsupported;
  }

  GLRenderPass pass;
  pass.colorAttachmentCount = descriptor.colorAttachmentCount;
  for (uint32_t index = 0; index < descriptor.colorAttachmentCount; ++index) {
    pass.colorFormats[index] = descriptor.colorFormats[index];
  }
  pass.depthStencilFormat = descriptor.depthStencilFormat;
  pass.hasDepthStencil = descriptor.depthStencilFormat != PixelFormat::Undefined;
  pass.sampleCount = descriptor.sampleCount;
  if (descriptor.label != nullptr) {
    pass.label = descriptor.label;
  }

  *outPass = mRenderPasses.allocate(std::move(pass));
  return outPass->valid() ? Result::Success : Result::OutOfMemory;
}

void GLDevice::destroyRenderPass(const RenderPassHandle renderPass) { mRenderPasses.free(renderPass); }

uint64_t GLDevice::makeFramebufferKey(const GLuint* colorAttachments, const GLuint depthStencilName,
  const uint32_t colorCount) noexcept {
  // FNV-1a over the attachment names. Collisions would bind the wrong framebuffer, so the
  // attachments are also compared when an entry is found - see acquireFramebuffer.
  uint64_t hash = 1469598103934665603ull;
  const auto mix = [&hash](const uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  mix(colorCount);
  for (uint32_t index = 0; index < colorCount; ++index) {
    mix(colorAttachments[index]);
  }
  mix(depthStencilName);
  return hash;
}

Result GLDevice::acquireFramebuffer(const GLuint* colorAttachments, GLuint depthStencilName, uint32_t colorCount,
  uint64_t key, GLuint* outFbo) {
  gl::GLApi& api = gl::getGLApi();
  *outFbo = 0;

  const auto cached = mFramebufferCache.find(key);
  if (cached != mFramebufferCache.end()) {
    // The key is a hash of the attachments, so a hit is confirmed against the names it was built for: a
    // collision would otherwise bind a framebuffer belonging to a different attachment set, which draws
    // into the wrong texture with no error at all.
    bool matches = cached->second.colorCount == colorCount && cached->second.depthStencil == depthStencilName;
    for (uint32_t index = 0; matches && index < colorCount; ++index) {
      matches = cached->second.colorAttachments[index] == colorAttachments[index];
    }
    if (matches) {
      *outFbo = cached->second.framebuffer;
      return Result::Success;
    }
  }

  GLuint framebuffer = 0;
  api.glGenFramebuffers(1, &framebuffer);
  if (framebuffer == 0) {
    return Result::OutOfMemory;
  }

  // GL_READ_FRAMEBUFFER and GL_DRAW_FRAMEBUFFER are aliases of GL_FRAMEBUFFER in ES 3.0; drawing to
  // GL_DRAW_FRAMEBUFFER keeps a later glBlitFramebuffer resolve from rebinding the draw target.
  api.glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

  for (uint32_t index = 0; index < colorCount; ++index) {
    if (colorAttachments[index] == 0) continue;
    api.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + index, GL_TEXTURE_2D, colorAttachments[index], 0);
  }

  if (depthStencilName != 0) {
    // The attachment point depends on whether the format carries a stencil channel, which is why the
    // caller passes the texture and this function decides between GL_DEPTH_ATTACHMENT and
    // GL_DEPTH_STENCIL_ATTACHMENT.
    api.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depthStencilName, 0);
  }

  if (colorCount > 1 && api.glDrawBuffers != nullptr) {
    GLenum buffers[kMaxColorAttachments] = {};
    for (uint32_t index = 0; index < colorCount; ++index) {
      buffers[index] = colorAttachments[index] != 0 ? GL_COLOR_ATTACHMENT0 + index : GL_NONE;
    }
    api.glDrawBuffers(static_cast<GLsizei>(colorCount), buffers);
  }

  const GLenum status = api.glCheckFramebufferStatus(GL_FRAMEBUFFER);
  api.glBindFramebuffer(GL_FRAMEBUFFER, 0);

  if (status != GL_FRAMEBUFFER_COMPLETE) {
    api.glDeleteFramebuffers(1, &framebuffer);
    std::string message = "the framebuffer for this render pass is incomplete (status 0x";
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%04X", status);
    message += buffer;
    message += "); the attachment formats or sample counts are not a valid combination on this device";
    emitMessage(MessageSeverity::Error, message.c_str());
    return Result::Unsupported;
  }

  CachedFramebuffer entry;
  entry.framebuffer = framebuffer;
  entry.colorCount = std::min(colorCount, kMaxColorAttachments);
  for (uint32_t index = 0; index < entry.colorCount; ++index) {
    entry.colorAttachments[index] = colorAttachments[index];
  }
  entry.depthStencil = depthStencilName;
  mFramebufferCache.emplace(key, entry);
  *outFbo = framebuffer;
  return Result::Success;
}

void GLDevice::discardFramebuffersUsing(const GLuint name) {
  if (name == 0 || mFramebufferCache.empty()) return;

  gl::GLApi& api = gl::getGLApi();
  for (auto entry = mFramebufferCache.begin(); entry != mFramebufferCache.end();) {
    if (!entry->second.uses(name)) {
      ++entry;
      continue;
    }
    if (entry->second.framebuffer != 0 && api.glDeleteFramebuffers != nullptr) {
      api.glDeleteFramebuffers(1, &entry->second.framebuffer);
    }
    entry = mFramebufferCache.erase(entry);
  }
}

void GLDevice::clearFramebufferCache() {
  gl::GLApi& api = gl::getGLApi();
  if (api.glDeleteFramebuffers != nullptr) {
    for (const auto& entry: mFramebufferCache) {
      GLuint framebuffer = entry.second.framebuffer;
      if (framebuffer != 0) {
        api.glDeleteFramebuffers(1, &framebuffer);
      }
    }
  }
  mFramebufferCache.clear();
}

void GLDevice::clearVertexArrayCache() {
  gl::GLApi& api = gl::getGLApi();
  if (api.glDeleteVertexArrays != nullptr) {
    for (const auto& entry: mVertexArrayCache) {
      GLuint vertexArray = entry.second.name;
      if (vertexArray != 0) {
        api.glDeleteVertexArrays(1, &vertexArray);
      }
    }
  }
  mVertexArrayCache.clear();
}

Result GLDevice::createFramebuffer(const FramebufferDescriptor& descriptor, FramebufferHandle* outFramebuffer) {
  if (outFramebuffer == nullptr) {
    return Result::InvalidArgument;
  }
  *outFramebuffer = FramebufferHandle::invalid();

  GLRenderPass* pass = mRenderPasses.get(descriptor.renderPass);
  if (pass == nullptr) {
    return Result::InvalidHandle;
  }

  GLuint colorNames[kMaxColorAttachments] = {};
  for (uint32_t index = 0; index < descriptor.colorAttachmentCount; ++index) {
    const ResolvedView view = resolveTextureView(descriptor.colorAttachments[index]);
    if (!view.valid) {
      continue;
    }
    colorNames[index] = view.textureName;
  }

  const ResolvedView depthView = resolveTextureView(descriptor.depthStencilAttachment);
  const uint64_t key = makeFramebufferKey(colorNames, depthView.textureName, descriptor.colorAttachmentCount);

  GLuint framebuffer = 0;
  const Result result =
    acquireFramebuffer(colorNames, depthView.textureName, descriptor.colorAttachmentCount, key, &framebuffer);
  if (!isSuccess(result)) {
    return result;
  }

  // The framebuffer object itself is cached and shared; the handle records it so that destroyFramebuffer
  // becomes a bookkeeping operation rather than a driver call.
  *outFramebuffer = mFramebuffers.allocate(static_cast<GLuint>(framebuffer));
  return outFramebuffer->valid() ? Result::Success : Result::OutOfMemory;
}

void GLDevice::destroyFramebuffer(const FramebufferHandle framebuffer) {
  // The cache owns the GL object; releasing the handle only drops this reference to it.
  mFramebuffers.free(framebuffer);
}

// ==================================================================================================
// Queries
// ==================================================================================================

Result GLDevice::createQuerySet(const QuerySetDescriptor& descriptor, QuerySetHandle* outQuerySet) {
  if (outQuerySet == nullptr) {
    return Result::InvalidArgument;
  }
  *outQuerySet = QuerySetHandle::invalid();

  if (!mCapabilities.supports(Feature::QueryObjects)) {
    return Result::Unsupported;
  }
  if (descriptor.count == 0) {
    return Result::InvalidArgument;
  }

  if (descriptor.type == QueryType::Timestamp && !mCapabilities.supports(Feature::TimestampQuery)) {
    emitMessage(MessageSeverity::Error,
      "timestamp queries need GL_EXT_disjoint_timer_query, which this device does not "
      "expose; gate the profiling path on Feature::TimestampQuery");
    return Result::Unsupported;
  }
  if (descriptor.type == QueryType::PipelineStatistics) {
    return Result::Unsupported;
  }

  gl::GLApi& api = gl::getGLApi();
  GLQuerySet querySet;
  querySet.type = descriptor.type;
  querySet.queries.resize(descriptor.count, 0);
  if (descriptor.label != nullptr) querySet.label = descriptor.label;

  api.glGenQueries(static_cast<GLsizei>(descriptor.count), querySet.queries.data());
  for (const GLuint query: querySet.queries) {
    if (query == 0) {
      api.glDeleteQueries(static_cast<GLsizei>(querySet.queries.size()), querySet.queries.data());
      return Result::OutOfMemory;
    }
  }

  *outQuerySet = mQuerySets.allocate(std::move(querySet));
  return outQuerySet->valid() ? Result::Success : Result::OutOfMemory;
}

void GLDevice::destroyQuerySet(const QuerySetHandle querySet) {
  GLQuerySet* entry = mQuerySets.get(querySet);
  if (entry == nullptr) {
    return;
  }
  if (!entry->queries.empty() && gl::getGLApi().glDeleteQueries != nullptr) {
    gl::getGLApi().glDeleteQueries(static_cast<GLsizei>(entry->queries.size()), entry->queries.data());
  }
  entry->queries.clear();
  mQuerySets.free(querySet);
}

Result GLDevice::getQuerySetCount(const QuerySetHandle querySet, uint32_t* outCount) const {
  if (outCount == nullptr) {
    return Result::InvalidArgument;
  }
  const GLQuerySet* entry = mQuerySets.get(querySet);
  if (entry == nullptr) {
    return Result::InvalidHandle;
  }
  *outCount = static_cast<uint32_t>(entry->queries.size());
  return Result::Success;
}

bool GLDevice::isQueryResultAvailable(const QuerySetHandle querySet, const uint32_t query) const {
  const GLQuerySet* entry = mQuerySets.get(querySet);
  if (entry == nullptr || query >= entry->queries.size()) {
    return false;
  }

  gl::GLApi& api = gl::getGLApi();
  GLuint available = 0;
  api.glGetQueryObjectuiv(entry->queries[query], GL_QUERY_RESULT_AVAILABLE, &available);
  return available != 0;
}

Result GLDevice::getQueryResults(const QuerySetHandle querySet, const uint32_t firstQuery, const uint32_t count,
  void* outData, const size_t outDataSize) {
  GLQuerySet* entry = mQuerySets.get(querySet);
  if (entry == nullptr) {
    return Result::InvalidHandle;
  }
  if (outData == nullptr) {
    return Result::InvalidArgument;
  }
  if (firstQuery + count > entry->queries.size()) {
    return Result::InvalidArgument;
  }
  if (outDataSize < count * sizeof(uint64_t)) {
    return Result::InvalidArgument;
  }

  gl::GLApi& api = gl::getGLApi();
  auto* results = static_cast<uint64_t*>(outData);

  for (uint32_t index = 0; index < count; ++index) {
    const GLuint query = entry->queries[firstQuery + index];
    if (entry->type == QueryType::Timestamp) {
      // `GL_TIME_ELAPSED_EXT` / `GL_TIMESTAMP` are defined in nanoseconds, so this branch already
      // satisfies the RHI's unit contract. Vulkan converts its GPU ticks in `VulkanDevice::getQueryResults`.
      GLuint64 value = 0;
      if (api.glGetQueryObjectui64v != nullptr) {
        api.glGetQueryObjectui64v(query, GL_QUERY_RESULT, &value);
      } else if (api.glGetQueryObjectuiv != nullptr) {
        GLuint truncated = 0;
        api.glGetQueryObjectuiv(query, GL_QUERY_RESULT, &truncated);
        value = truncated;
      }
      results[index] = value;
    } else {
      GLuint value = 0;
      if (api.glGetQueryObjectuiv != nullptr) {
        api.glGetQueryObjectuiv(query, GL_QUERY_RESULT, &value);
      }
      results[index] = value;
    }
  }

  return Result::Success;
}

// ==================================================================================================
// Fences and semaphores
// ==================================================================================================

Result GLDevice::createFence(const FenceDescriptor& descriptor, FenceHandle* outFence) {
  if (outFence == nullptr) return Result::InvalidArgument;
  *outFence = FenceHandle::invalid();

  GLFence fence;
  // A fence created in the signalled state reports Success from the first poll without the GPU
  // having to do anything, which is what makes "create signalled, then reset" usable as a barrier.
  fence.state = descriptor.flag == FenceFlag::Signalled ? GLFenceState::Signalled : GLFenceState::Unsignalled;
  if (descriptor.label != nullptr) {
    fence.label = descriptor.label;
  }
  fence.sync = nullptr;

  *outFence = mFences.allocate(std::move(fence));
  return outFence->valid() ? Result::Success : Result::OutOfMemory;
}

void GLDevice::destroyFence(const FenceHandle fenceHandle) {
  GLFence* entry = mFences.get(fenceHandle);
  if (entry == nullptr) {
    return;
  }
  if (entry->sync != nullptr && gl::getGLApi().glDeleteSync != nullptr) {
    gl::getGLApi().glDeleteSync(entry->sync);
  }
  entry->sync = nullptr;
  mFences.free(fenceHandle);
}

Result GLDevice::createSemaphore(const SemaphoreDescriptor& descriptor, SemaphoreHandle* outSemaphore) {
  if (outSemaphore == nullptr) {
    return Result::InvalidArgument;
  }
  *outSemaphore = SemaphoreHandle::invalid();

  GLSemaphore semaphore;
  if (descriptor.label != nullptr) {
    semaphore.label = descriptor.label;
  }

  *outSemaphore = mSemaphores.allocate(std::move(semaphore));
  return outSemaphore->valid() ? Result::Success : Result::OutOfMemory;
}

void GLDevice::destroySemaphore(const SemaphoreHandle semaphore) { mSemaphores.free(semaphore); }

Result GLDevice::getFenceStatus(const FenceHandle fenceHandle) const {
  const GLFence* entry = mFences.get(fenceHandle);
  if (entry == nullptr) {
    return Result::InvalidHandle;
  }
  if (entry->state == GLFenceState::Signalled) {
    return Result::Success;
  }
  if (entry->sync == nullptr) {
    // No sync object was created, which only happens when the submission could not allocate one;
    // treat it as complete because the GL command stream provides the ordering anyway.
    return Result::Success;
  }

  gl::GLApi& api = gl::getGLApi();
  const GLenum status = api.glClientWaitSync(entry->sync, 0, 0);
  if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) {
    return Result::Success;
  }
  if (status == GL_TIMEOUT_EXPIRED) {
    return Result::NotReady;
  }
  if (status == GL_WAIT_FAILED) {
    const GLenum error = api.glGetError();
    if (error == GL_INVALID_OPERATION) {
      return Result::DeviceLost;
    }
    return Result::Error;
  }
  return Result::NotReady;
}

Result GLDevice::waitForFence(const FenceHandle fenceHandle, const uint64_t timeoutNanoseconds) {
  GLFence* entry = mFences.get(fenceHandle);
  if (entry == nullptr) {
    return Result::InvalidHandle;
  }
  if (entry->state == GLFenceState::Signalled) {
    return Result::Success;
  }
  if (entry->sync == nullptr) {
    entry->state = GLFenceState::Signalled;
    return Result::Success;
  }

  gl::GLApi& api = gl::getGLApi();
  const GLbitfield flags = GL_SYNC_FLUSH_COMMANDS_BIT;
  const GLuint64 timeout = timeoutNanoseconds == UINT64_MAX ? GL_TIMEOUT_IGNORED : GLuint64(timeoutNanoseconds);

  const GLenum status = api.glClientWaitSync(entry->sync, flags, timeout);
  switch (status) {
    case GL_ALREADY_SIGNALED:
    case GL_CONDITION_SATISFIED:
      entry->state = GLFenceState::Signalled;
      return Result::Success;
    case GL_TIMEOUT_EXPIRED:
      return Result::NotReady;
    case GL_WAIT_FAILED: {
      const GLenum error = api.glGetError();
      if (error == GL_INVALID_OPERATION) {
        return Result::DeviceLost;
      }
      return Result::Error;
    }
    default:
      return Result::Error;
  }
}

Result GLDevice::resetFence(const FenceHandle fenceHandle) {
  GLFence* entry = mFences.get(fenceHandle);
  if (entry == nullptr) {
    return Result::InvalidHandle;
  }

  if (entry->sync != nullptr && gl::getGLApi().glDeleteSync != nullptr) {
    gl::getGLApi().glDeleteSync(entry->sync);
    entry->sync = nullptr;
  }
  entry->state = GLFenceState::Unsignalled;
  return Result::Success;
}

Result GLDevice::waitIdle() {
  if (!mInitialized) return Result::Success;

  gl::GLApi& api = gl::getGLApi();
  if (api.glFinish != nullptr) {
    api.glFinish();
  }

  if (mPlatform.isLost()) {
    return Result::DeviceLost;
  }
  return Result::Success;
}

bool GLDevice::isResourceReadyToDestroy(TextureHandle handle) const {
  // The OpenGL backend executes a recorded command list synchronously inside executeCommandBuffer,
  // and submit() flushes the command stream, so by the time control returns to the caller the driver
  // has already consumed every reference the list held. Reporting "ready" is therefore accurate, and
  // it is what lets a renderer replace a texture per frame without an artificial waitIdle.
  //
  // This is the one place where the OpenGL backend is materially simpler than Vulkan would be; the
  // Vulkan backend would track submission indices against a fence.
  return mTextures.isAlive(handle);
}

bool GLDevice::isResourceReadyToDestroy(const BufferHandle handle) const { return mBuffers.isAlive(handle); }

// ==================================================================================================
// Swapchain and presentation
// ==================================================================================================

Result GLDevice::createSurface(const SurfaceDescriptor& descriptor, SurfaceHandle* outSurface) {
  if (outSurface == nullptr) {
    return Result::InvalidArgument;
  }
  *outSurface = SurfaceHandle::invalid();
  *outSurface = mSurfaces.allocate(SurfaceDescriptor(descriptor));
  return outSurface->valid() ? Result::Success : Result::OutOfMemory;
}

void GLDevice::destroySurface(const SurfaceHandle surface) { mSurfaces.free(surface); }

Result GLDevice::createSwapchain(const SwapchainDescriptor& descriptor, SwapchainHandle* outSwapchain) {
  if (outSwapchain == nullptr) {
    return Result::InvalidArgument;
  }
  *outSwapchain = SwapchainHandle::invalid();

  GLSwapchain swapchain;
  swapchain.surface = descriptor.surface;
  swapchain.width = descriptor.width;
  swapchain.height = descriptor.height;
  swapchain.format = descriptor.format;
  swapchain.presentMode = descriptor.presentMode;
  if (descriptor.label != nullptr) {
    swapchain.label = descriptor.label;
  }

  // OpenGL has no swapchain images: EGL owns the buffer ring and glSwapBuffers rotates it. The RHI's
  // swapchain abstraction still applies so that upper layers are written once, but imageCount is
  // reported as 1 and the "images" are the default framebuffer.
  swapchain.imageCount = 1;

  const auto [width, height] = mPlatform.querySurfaceSize();
  if (swapchain.width == 0) {
    swapchain.width = width;
  }
  if (swapchain.height == 0) {
    swapchain.height = height;
  }
  if (swapchain.width == 0 || swapchain.height == 0) {
    emitMessage(MessageSeverity::Error,
      "the swapchain has no size: pass width and height in the descriptor, or attach a "
      "surface the platform can query");
    return Result::InvalidArgument;
  }

  const PixelFormatInfo& info = getPixelFormatInfo(swapchain.format);
  swapchain.srgb = info.srgb;

  // ## Turning the request into behaviour, and reporting what was granted
  //
  // OpenGL has one knob for this: the swap interval. 1 waits for vertical sync (Fifo), 0 does not (Immediate). There is no
  // true Mailbox -- the closest GL can do is "do not wait", which is what a Mailbox request is approximated with, and the
  // swapchain then reports Immediate so the caller can see that it did not get Mailbox. A driver without `wglSwapIntervalEXT`
  // (or the EGL/GLX equivalents) cannot express either, and keeps reporting Fifo.
  // Mailbox is not something OpenGL can express: the closest it has is "do not wait for vsync", which is Immediate. The
  // granted mode therefore says Immediate, so a caller that asked for Mailbox can see it got the approximation rather than
  // believing it got the real thing.
  PresentMode granted = swapchain.presentMode;
  if (granted == PresentMode::Mailbox && !mPlatform.supportsMailboxPresent()) {
    // ## Mailbox is a queue semantic, and OpenGL has no queue semantics
    //
    // Vulkan's mailbox keeps at most one queued image and replaces it with the newest -- no vertical-sync wait, and a complete
    // frame either way. A swap interval of 0 gives the first half of that (no wait) and not the second (a frame already handed
    // over is not dropped), so what this backend would deliver is Immediate. Reporting Immediate is the honest answer:
    // `Capabilities::mailboxPresent` and `SwapchainInfo::presentMode` then agree.
    granted = PresentMode::Immediate;
  }
  const int interval = granted == PresentMode::Fifo ? 1 : 0;
  const Result intervalResult = mPlatform.setSwapInterval(interval);
  if (!isSuccess(intervalResult) && granted != PresentMode::Fifo) {
    emitMessage(MessageSeverity::Warning,
      "the driver has no swap-interval control; the requested present mode is reported as Fifo");
    granted = PresentMode::Fifo;
  }
  swapchain.presentMode = granted;

  *outSwapchain = mSwapchains.allocate(std::move(swapchain));
  return outSwapchain->valid() ? Result::Success : Result::OutOfMemory;
}

void GLDevice::destroySwapchain(const SwapchainHandle swapchain) { mSwapchains.free(swapchain); }

Result GLDevice::getSwapchainInfo(const SwapchainHandle swapchainHandle, SwapchainInfo* outInfo) {
  if (outInfo == nullptr) {
    return Result::InvalidArgument;
  }
  GLSwapchain* swapchain = mSwapchains.get(swapchainHandle);
  if (swapchain == nullptr) {
    return Result::InvalidHandle;
  }
  outInfo->width = swapchain->width;
  outInfo->height = swapchain->height;
  outInfo->format = swapchain->format;
  outInfo->imageCount = swapchain->imageCount;
  outInfo->presentMode = swapchain->presentMode;
  outInfo->currentImageIndex = swapchain->currentImageIndex;
  outInfo->srgb = swapchain->srgb;
  return Result::Success;
}

Result GLDevice::beginFrame(const SwapchainHandle swapchainHandle, SwapchainInfo* outInfo,
  const SemaphoreHandle signalSemaphore) {
  if (backend::test::consumeDeviceLossInjection()) return Result::DeviceLost;
  GLSwapchain* swapchain = mSwapchains.get(swapchainHandle);
  if (swapchain == nullptr) {
    return Result::InvalidHandle;
  }

  // Semaphores are bookkeeping only: GL has no acquire operation, so a "signal" is recorded and the
  // upper layer's wait on it is honoured by the fact that the command stream is already ordered.
  if (signalSemaphore.valid()) {
    GLSemaphore* semaphore = mSemaphores.get(signalSemaphore);
    if (semaphore != nullptr) {
      semaphore->signalled = true;
    }
  }

  if (mPlatform.isLost()) {
    return Result::DeviceLost;
  }

  // The context is shared by every surface. An output node attached later binds its window here,
  // on the render thread, instead of recreating the device for a new drawable.
  if (const SurfaceDescriptor* surface = mSurfaces.get(swapchain->surface)) {
    if (surface->nativeWindow != nullptr &&
        surface->nativeWindow != mPlatform.descriptor().nativeWindow) {
      const Result bound = mPlatform.bindSurface(*surface);
      if (!isSuccess(bound)) return bound;
    }
  }

  // The surface may have been resized since the swapchain was created. Reporting Suboptimal rather
  // than silently resizing keeps the decision with the upper layer, which is the only place that
  // knows whether its render targets can be reallocated mid-frame.
  auto status = SwapchainStatus::Ready;
  const auto [width, height] = mPlatform.querySurfaceSize();
  if (width != 0 && height != 0 && (width != swapchain->width || height != swapchain->height)) {
    status = SwapchainStatus::Suboptimal;
  }

  swapchain->currentImageIndex = 0;
  swapchain->frameAcquired = true;

  if (outInfo != nullptr) {
    outInfo->width = swapchain->width;
    outInfo->height = swapchain->height;
    outInfo->format = swapchain->format;
    outInfo->imageCount = swapchain->imageCount;
    outInfo->presentMode = swapchain->presentMode;
    outInfo->currentImageIndex = 0;
    outInfo->srgb = swapchain->srgb;
  }
  (void) status;
  return Result::Success;
}

Result GLDevice::endFrame(const SwapchainHandle swapchainHandle) {
  GLSwapchain* swapchain = mSwapchains.get(swapchainHandle);
  if (swapchain == nullptr) {
    return Result::InvalidHandle;
  }
  if (!swapchain->frameAcquired) {
    return Result::Error;
  }

  swapchain->frameAcquired = false;

  if (mPlatform.context().surface == nullptr) {
    // Headless: nothing to present, and that is not an error.
    return Result::Success;
  }

  return mPlatform.swapBuffers();
}

Result GLDevice::resizeSwapchain(const SwapchainHandle swapchainHandle, const uint32_t width, const uint32_t height) {
  GLSwapchain* swapchain = mSwapchains.get(swapchainHandle);
  if (swapchain == nullptr) {
    return Result::InvalidHandle;
  }
  if (width == 0 || height == 0) {
    return Result::InvalidArgument;
  }

  swapchain->width = width;
  swapchain->height = height;
  // EGL resizes the native window's buffers itself; the RHI only needs to forget any cached size.
  // Render targets the upper layer created from the old size are its own responsibility to rebuild,
  // which is why endFrame surfaces OutOfDate rather than silently reallocating.
  return Result::Success;
}

// ==================================================================================================
// Native interop
// ==================================================================================================

NativeTexture GLDevice::getNativeTexture(const TextureHandle handle) const {
  NativeTexture native;
  const GLTexture* texture = mTextures.get(handle);
  if (texture == nullptr) {
    return native;
  }

  // The GL name is an integer, so it is carried in the pointer slot. A consumer that knows the
  // backend is running can reinterpret it; one that does not must not.
  native.object = reinterpret_cast<void*>(static_cast<uintptr_t>(texture->meta.name));
  native.context = mPlatform.context().display;
  return native;
}

NativeBuffer GLDevice::getNativeBuffer(const BufferHandle handle) const {
  NativeBuffer native;
  const GLBuffer* buffer = mBuffers.get(handle);
  if (buffer == nullptr) {
    return native;
  }
  native.object = reinterpret_cast<void*>(static_cast<uintptr_t>(buffer->meta.name));
  native.context = mPlatform.context().display;
  return native;
}

bool GLDevice::supportsNativeTextureImport(const NativeTextureKind kind) const {
  if (kind == NativeTextureKind::OpenGlTexture) return true;
  if (kind != NativeTextureKind::AndroidHardwareBuffer) return false;
  const EGLApi& egl = getEGLApi();
  return egl.eglGetNativeClientBufferANDROID != nullptr && egl.eglCreateImageKHR != nullptr &&
         egl.eglDestroyImageKHR != nullptr && gl::getGLApi().glEGLImageTargetTexture2DOES != nullptr;
}

Result GLDevice::importNativeTexture(const NativeTextureDescriptor& descriptor, TextureHandle* outTexture) {
  if (outTexture == nullptr) {
    return Result::InvalidArgument;
  }
  *outTexture = TextureHandle::invalid();
  if (descriptor.kind == NativeTextureKind::AndroidHardwareBuffer) {
    if (descriptor.platformHandle == nullptr) return Result::InvalidArgument;
    if (!supportsNativeTextureImport(descriptor.kind)) return Result::Unsupported;
    EGLApi& egl = getEGLApi();
    void* clientBuffer = egl.eglGetNativeClientBufferANDROID(descriptor.platformHandle);
    if (clientBuffer == nullptr) return Result::Unsupported;
    void* image = egl.eglCreateImageKHR(mPlatform.context().display, nullptr, EGL_NATIVE_BUFFER_ANDROID,
            clientBuffer, nullptr);
    if (image == nullptr) return Result::Unsupported;    GLuint name = 0;
    gl::getGLApi().glGenTextures(1, &name);
    if (name == 0) {
      egl.eglDestroyImageKHR(mPlatform.context().display, image);
      return Result::OutOfMemory;
    }
    gl::getGLApi().glBindTexture(GL_TEXTURE_EXTERNAL_OES, name);
    // An external texture has no mip chain; leaving the default MIN_FILTER (mipmap) makes it
    // incomplete and sampling returns black even though EGLImage import succeeded.
    gl::getGLApi().glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl::getGLApi().glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl::getGLApi().glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl::getGLApi().glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl::getGLApi().glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);
    gl::getGLApi().glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl::getGLApi().glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl::getGLApi().glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl::getGLApi().glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl::getGLApi().glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    GLTexture texture;
    texture.meta.name = name;
    texture.meta.target = GL_TEXTURE_EXTERNAL_OES;
    texture.meta.format = PixelFormat::R8G8B8A8_UNORM;
    texture.meta.internalFormat = describeFormat(PixelFormat::R8G8B8A8_UNORM).internalFormat;
    texture.meta.width = descriptor.width;
    texture.meta.height = descriptor.height;
    texture.meta.type = TextureType::Texture2D;
    texture.meta.usage = static_cast<TextureUsages>(TextureUsage::Sampled | TextureUsage::External);
    texture.meta.externallyOwned = false;
    texture.meta.immutableStorage = true;
    texture.eglImage = image;
    texture.label = "imported AHardwareBuffer";
    *outTexture = mTextures.allocate(std::move(texture));
    if (!outTexture->valid()) {
      gl::getGLApi().glDeleteTextures(1, &name);
      egl.eglDestroyImageKHR(mPlatform.context().display, image);
      return Result::OutOfMemory;
    }
    return Result::Success;
  }
  if (descriptor.kind != NativeTextureKind::OpenGlTexture) {
    return Result::Unsupported;
  }
  if (descriptor.glTexture == 0) {
    return Result::InvalidArgument;
  }

  GLTexture texture;
  texture.meta.name = descriptor.glTexture;
  texture.meta.target = descriptor.glTarget != 0 ? descriptor.glTarget : GL_TEXTURE_2D;
  texture.meta.format = descriptor.format;
  texture.meta.internalFormat = describeFormat(descriptor.format).internalFormat;
  texture.meta.width = descriptor.width;
  texture.meta.height = descriptor.height;
  texture.meta.type = texture.meta.target == GL_TEXTURE_3D
                        ? TextureType::Texture3D
                        : texture.meta.target == GL_TEXTURE_2D_ARRAY
                        ? TextureType::Texture2DArray
                        : texture.meta.target == GL_TEXTURE_CUBE_MAP
                        ? TextureType::TextureCube
                        : TextureType::Texture2D;
  texture.meta.usage = static_cast<TextureUsages>(TextureUsage::Sampled | TextureUsage::External);

  // The texture is borrowed: destroying the RHI handle must not delete a GL object the application
  // (a video decoder, an EGL image consumer) still owns.
  texture.meta.externallyOwned = true;
  texture.meta.immutableStorage = true;
  texture.label = "imported";

  *outTexture = mTextures.allocate(std::move(texture));
  return outTexture->valid() ? Result::Success : Result::OutOfMemory;
}

// ==================================================================================================
// Diagnostics
// ==================================================================================================

Result GLDevice::makeCurrent() {
  // The platform owns the context object; this is the one call that makes it current on the calling thread.
  return mPlatform.makeCurrent();
}

void GLDevice::releaseCurrent() {
  // Not reported: releasing a thread that has nothing current is not a failure, and there is no useful reaction to one.
  (void) mPlatform.releaseCurrent();
}

void GLDevice::setDebugMessageCallback(const DebugMessageCallback callback, void* userData) {
  // Release so that everything the installing thread did before this call is visible to a recording
  // thread that observes the callback; the acquire in emitMessage is the other half of the pair.
  mDebugCallback.store(callback, std::memory_order_release);
  mDebugUserData.store(userData, std::memory_order_release);
}

void GLDevice::emitMessage(const MessageSeverity severity, const char* message) const {
  if (message == nullptr) {
    return;
  }
  // One acquire load, into a local, so the pointer and its user data cannot be torn apart by a
  // concurrent setDebugMessageCallback: a caller that replaces the callback while a recording thread
  // is emitting a validation message gets either the old pair or the new one, never the callback of
  // one with the user data of the other.
  const DebugMessageCallback callback = mDebugCallback.load(std::memory_order_acquire);
  if (callback != nullptr) {
    callback(severity, message, mDebugUserData.load(std::memory_order_acquire));
    return;
  }

  // With no callback installed the message goes to the engine log, so a capability downgrade or a
  // validation rejection is never silent.
  switch (severity) {
    case MessageSeverity::Verbose:
    case MessageSeverity::Info:
      MINILOG_INFO("[backend/gl] {}", message);
      break;
    case MessageSeverity::Warning:
      MINILOG_WARN("[backend/gl] {}", message);
      break;
    case MessageSeverity::Error:
    case MessageSeverity::Fatal:
      MINILOG_ERROR("[backend/gl] {}", message);
      break;
  }
}

void GLDevice::reportGLError(const char* operation) const {
  gl::GLApi& api = gl::getGLApi();
  const GLenum error = api.glGetError();
  if (error == GL_NO_ERROR) {
    return;
  }

  std::string message = operation != nullptr ? operation : "an OpenGL call";
  message += " failed with ";
  message += gl::GLApi::errorName(error);
  emitMessage(MessageSeverity::Error, message.c_str());
}

void GLDevice::setTextureLabel(const TextureHandle handle, const char* label) {
  GLTexture* texture = mTextures.get(handle);
  if (texture == nullptr) {
    return;
  }
  texture->label = label != nullptr ? label : "";
}

void GLDevice::setBufferLabel(const BufferHandle handle, const char* label) {
  GLBuffer* buffer = mBuffers.get(handle);
  if (buffer == nullptr) {
    return;
  }
  buffer->label = label != nullptr ? label : "";
}

void GLDevice::beginDebugRegion(const char* label) {
  // ES 3.0 has no annotation entry point (KHR_debug's glPushDebugGroup is ES 3.2 / extension only).
  // The marker is routed to the debug callback instead, so a capture tool or a log still shows where
  // a frame's passes begin.
  if (label == nullptr) return;
  std::string message = "> ";
  message += label;
  emitMessage(MessageSeverity::Verbose, message.c_str());
}

void GLDevice::endDebugRegion() { emitMessage(MessageSeverity::Verbose, "<"); }

void GLDevice::appendDiagnostics(std::string& out) const {
  const GlDialect dialect = mPlatform.context().dialect;
  const GlVersion minimum = GlVersion::minimumFor(dialect);

  out += "RHI backend: ";
  out += dialect == GlDialect::ES ? "OpenGL ES" : "OpenGL";
  out += "\n  vendor:       ";
  out += mInfo.vendor != nullptr ? mInfo.vendor : "";
  out += "\n  renderer:     ";
  out += mInfo.deviceName != nullptr ? mInfo.deviceName : "";
  out += "\n  GL version:   ";
  out += mPlatform.versionString();
  out += "\n  GLSL version: ";
  out += mPlatform.shadingLanguageVersion();
  out += "\n  SPIR-V input: ";
  out += isShaderCrossAvailable() ? "yes (SPIRV-Cross)" : "no";
  // The other half of the same question, and not implied by the line above: a build with neither
  // shaderc nor SPIRV-Cross still takes GLSL source, because the driver is what compiles it.
  out += "\n  GLSL input:   yes (driver compiles the source as written)";
  // The dialect and the version it was required to reach are the two facts that explain every
  // capability answer below, so both are reported rather than only the version the driver returned.
  out += "\n  dialect:      ";
  out += toString(dialect);
  out += " (minimum ";
  out += minimum.toString();
  out += ')';
  out += "\n  profile:      ";
  out += mPlatform.context().coreProfile ? "core" : "compatibility";
  // The window-system layer, which is the one line here that differs between two machines running the same build. The
  // dialect above says *what* was created; this says through which of WGL, GLX, CGL or EGL, which is what a bug report
  // needs and what cannot be worked out from anything else printed.
  out += "\n  window system: ";
  out += gl::GlPlatform::backendName();
  out += "\n\n";

  out += "Extensions (";
  out += std::to_string(mPlatform.extensionCount());
  out += "):\n";
  mPlatform.appendExtensions(out);

  out += '\n';
  mCapabilities.appendReport(out);

  out += "\n  Live resources: textures=";
  out += std::to_string(mTextures.liveCount());
  out += " buffers=";
  out += std::to_string(mBuffers.liveCount());
  out += " renderPipelines=";
  out += std::to_string(mRenderPipelines.liveCount());
  out += " programs=";
  out += std::to_string(mRenderPipelines.liveCount() + mComputePipelines.liveCount());
  out += " cachedFramebuffers=";
  out += std::to_string(mFramebufferCache.size());
  out += " cachedVertexArrays=";
  out += std::to_string(mVertexArrayCache.size());
  out += '\n';
}

} // namespace backend



