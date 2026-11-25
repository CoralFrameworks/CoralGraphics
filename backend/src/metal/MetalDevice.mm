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

#include "private/backend/metal/MetalDevice.h"

#include "private/backend/metal/MetalCommandBuffer.h"
#include "private/backend/metal/MetalFactory.h"

#include "GlslCompiler.h"
#include "MetalCross.h"
#include "ShaderCross.h"

#include <backend/Validation.h>

#include <minilog/log.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace backend::metal {
namespace {

std::string toStdString(NSString* string) {
  if (string == nil) return {};
  const char* utf8 = [string UTF8String];
  return utf8 != nullptr ? std::string(utf8) : std::string();
}

/** @return a driver error's description, including its domain and code, for a diagnostic that can be acted on. */
std::string describe(NSError* error) {
  if (error == nil) return "no error was reported";
  std::string message = toStdString(error.localizedDescription);
  if (message.empty()) message = "the driver reported an error with no description";
  message += " (";
  message += toStdString(error.domain);
  message += ' ';
  message += std::to_string(error.code);
  message += ')';
  return message;
}

BindingType toBindingType(const ShaderBindingType type) noexcept {
  switch (type) {
    case ShaderBindingType::TextureSampler:
      return BindingType::TextureSampler;
    case ShaderBindingType::Sampler:
      return BindingType::Sampler;
    case ShaderBindingType::SampledTexture:
      return BindingType::Texture;
    case ShaderBindingType::UniformBuffer:
      return BindingType::UniformBuffer;
    case ShaderBindingType::StorageBuffer:
      return BindingType::StorageBuffer;
    case ShaderBindingType::ReadOnlyStorageBuffer:
      return BindingType::ReadOnlyStorageBuffer;
    case ShaderBindingType::PushConstant:
    case ShaderBindingType::StageInput:
    case ShaderBindingType::StageOutput:
      break;
  }
  return BindingType::TextureSampler;
}

bool isDescriptorBinding(const ShaderBindingType type) noexcept {
  return type == ShaderBindingType::TextureSampler || type == ShaderBindingType::Sampler ||
         type == ShaderBindingType::SampledTexture || type == ShaderBindingType::UniformBuffer ||
         type == ShaderBindingType::StorageBuffer || type == ShaderBindingType::ReadOnlyStorageBuffer;
}

/** FNV-1a over the module's bytes, the same hash the Vulkan backend reports for a binary module. */
uint64_t hashBytes(const void* data, const size_t size) noexcept {
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint64_t hash = 1469598103934665603ull;
  for (size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= 1099511628211ull;
  }
  return hash;
}

bool isBcFormat(const PixelFormat format) noexcept {
  switch (format) {
    case PixelFormat::BC1_RGBA_UNORM:
    case PixelFormat::BC1_RGBA_SRGB:
    case PixelFormat::BC3_RGBA_UNORM:
    case PixelFormat::BC3_RGBA_SRGB:
    case PixelFormat::BC4_R_UNORM:
    case PixelFormat::BC5_RG_UNORM:
    case PixelFormat::BC7_RGBA_UNORM:
    case PixelFormat::BC7_RGBA_SRGB:
      return true;
    default:
      return false;
  }
}

bool isEtc2OrAstcFormat(const PixelFormat format) noexcept {
  switch (format) {
    case PixelFormat::ETC2_RGB8_UNORM:
    case PixelFormat::ETC2_RGB8_SRGB:
    case PixelFormat::ETC2_RGBA8_UNORM:
    case PixelFormat::ETC2_RGBA8_SRGB:
    case PixelFormat::ASTC_4x4_UNORM:
    case PixelFormat::ASTC_4x4_SRGB:
    case PixelFormat::ASTC_8x8_UNORM:
    case PixelFormat::ASTC_8x8_SRGB:
      return true;
    default:
      return false;
  }
}

const char* gpuFamilyName(id<MTLDevice> device) {
  if ([device supportsFamily:MTLGPUFamilyApple9]) return "Apple9";
  if ([device supportsFamily:MTLGPUFamilyApple8]) return "Apple8";
  if ([device supportsFamily:MTLGPUFamilyApple7]) return "Apple7";
  if ([device supportsFamily:MTLGPUFamilyApple6]) return "Apple6";
  if ([device supportsFamily:MTLGPUFamilyApple5]) return "Apple5";
  if ([device supportsFamily:MTLGPUFamilyApple4]) return "Apple4";
  if ([device supportsFamily:MTLGPUFamilyApple3]) return "Apple3";
  if ([device supportsFamily:MTLGPUFamilyApple2]) return "Apple2";
  if ([device supportsFamily:MTLGPUFamilyApple1]) return "Apple1";
  if ([device supportsFamily:MTLGPUFamilyMac2]) return "Mac2";
  if ([device supportsFamily:MTLGPUFamilyCommon3]) return "Common3";
  if ([device supportsFamily:MTLGPUFamilyCommon2]) return "Common2";
  return "Common1";
}

/** @return the operating system version, which is what a Metal bug report needs beside the GPU name. */
std::string operatingSystemVersion() {
#if defined(CORAL_BUILD_FOR_IOS)
  const NSOperatingSystemVersion version = [[NSProcessInfo processInfo] operatingSystemVersion];
  return "iOS " + std::to_string(version.majorVersion) + "." + std::to_string(version.minorVersion);
#else
  const NSOperatingSystemVersion version = [[NSProcessInfo processInfo] operatingSystemVersion];
  return "macOS " + std::to_string(version.majorVersion) + "." + std::to_string(version.minorVersion);
#endif
}

} // namespace anonymous

// ==================================================================================================
// Queue
// ==================================================================================================

MetalQueue::MetalQueue(MetalDevice& device, const QueueType type) noexcept
        : mDevice(device),
          mType(type) {}

Result MetalQueue::submit(CommandBuffer& commandBuffer, const FenceHandle fence, const SemaphoreHandle waitSemaphore,
        const SemaphoreHandle signalSemaphore) {
  auto* metalBuffer = dynamic_cast<MetalCommandBuffer*>(&commandBuffer);
  if (metalBuffer == nullptr) return Result::InvalidArgument;
  if (!metalBuffer->isEnded()) {
    mDevice.emitMessage(MessageSeverity::Error, "a command buffer must be ended before it is submitted");
    return Result::InvalidArgument;
  }
  return mDevice.submitCommandBuffer(*metalBuffer, fence, waitSemaphore, signalSemaphore);
}

Result MetalQueue::waitIdle() {
  return mDevice.waitIdle();
}

// ==================================================================================================
// Lifecycle
// ==================================================================================================

MetalDevice::MetalDevice()
        : mGraphicsQueueObject(*this, QueueType::Graphics) {}

MetalDevice::~MetalDevice() {
  if (!mInitialized) {
    // A device that never came up still owns its Metal objects (the creation path allocates them before it knows
    // whether the rest of initialisation succeeds). ARC releases those when the members fall out of scope, which is
    // exactly the RAII contract, so there is nothing to do here beyond not touching the GPU.
    return;
  }

  // Wait for the GPU before tearing anything down: a command buffer still in flight holds references to resources
  // whose pools are about to be released, and Metal's own validation would report the overlap rather than protect
  // against it.
  waitIdle();

  // Each pool releases its payloads as it is destroyed, and `HandlePool::free` is not needed for teardown -- the arena
  // owns every slot. Declared in reverse dependency order (pools before the fallbacks, fallbacks before the device) by
  // the member order in the header.
  mFanIndexBuffer = nil;
  mFallbackBuffer = nil;
  mFallbackSampler = nil;
  mFallbackTexture = nil;

  mCompletionEvent = nil;
  mQueue = nil;
  mDevice = nil;
  mInitialized = false;
}

Result MetalDevice::initialize(const DeviceConfiguration& configuration) {
  if (mInitialized) return Result::Success;

  mMaxFramesInFlight = resolveMaxFramesInFlight(configuration);
  mValidation = configuration.enableValidation;

  const Result created = createDevice(configuration);
  if (!isSuccess(created)) return created;

  buildCapabilities(configuration);

  // A backend that cannot provide what the caller cannot live without is rejected here, with the *feature names*, so
  // that a wrong technique is a startup error rather than a corrupt frame. The Vulkan backend does the same.
  if (configuration.requiredFeatures != 0) {
    const Features missing = configuration.requiredFeatures & ~mCapabilities.features;
    if (missing != 0) {
      std::string message = "the Metal device cannot provide required feature(s):";
      for (uint32_t bit = 0; bit < 32; ++bit) {
        const auto feature = static_cast<Feature>(1u << bit);
        if ((missing & static_cast<Features>(feature)) == 0) continue;
        message += ' ';
        message += toString(feature);
        message += " (";
        message += toString(mCapabilities.reasonFor(feature));
        message += ')';
      }
      emitMessage(MessageSeverity::Error, message);
      return Result::Unsupported;
    }
  }

  if (configuration.capabilityOverride != nullptr) {
    mCapabilities = *configuration.capabilityOverride;
    mInfo.apiVersion = mCapabilities.apiVersion != nullptr ? mCapabilities.apiVersion : mInfo.apiVersion;
  }

  const Result fallbacks = createFallbackResources();
  if (!isSuccess(fallbacks)) return fallbacks;

  mInitialized = true;
  emitMessage(MessageSeverity::Info,
          std::string("Metal device created: ") + mInfo.deviceName + " (" + mInfo.apiVersion + ", MSL 2.2)");
  return Result::Success;
}

uint32_t MetalDevice::resolveMaxFramesInFlight(const DeviceConfiguration& configuration) const {
  // Metal does not require the CPU to cap in-flight work at all -- the command queue and the drawable pool bound it --
  // so an unset request means the platform's own answer rather than a number this backend invents.
  return configuration.maxFramesInFlight != 0 ? configuration.maxFramesInFlight : 2u;
}

Result MetalDevice::createDevice(const DeviceConfiguration& configuration) {
  (void) configuration;

#if defined(CORAL_BUILD_FOR_IOS)
  mIos = true;
#else
  mIos = false;
#endif

  mDevice = MTLCreateSystemDefaultDevice();
  if (mDevice == nil) {
    emitMessage(MessageSeverity::Error,
            "no Metal device is available: MTLCreateSystemDefaultDevice returned nil. On a Mac this means the "
            "machine has no Metal-capable GPU; in a simulator it means the simulator has no Metal support");
    return Result::Unsupported;
  }

  mQueue = [mDevice newCommandQueue];
  if (mQueue == nil) {
    emitMessage(MessageSeverity::Error, "the Metal device could not create a command queue");
    return Result::Unsupported;
  }
  mQueue.label = @"coral/backend/metal";

  // A value-based shared event is the device's completion timeline: every submission signals the next value, and
  // `waitIdle` waits for the latest one. It is also what fences and semaphores are built from.
  mCompletionEvent = [mDevice newSharedEvent];
  if (mCompletionEvent == nil) {
    emitMessage(MessageSeverity::Error, "the Metal device could not create a shared event for synchronisation");
    return Result::Unsupported;
  }
  mCompletionEvent.label = @"coral/metal/completion";

  // A CAMetalLayer can be configured for Display P3 and for PQ/EDR on every Metal device; whether the
  // *display* has the gamut or the headroom is a property of the surface, not of the device, and the
  // swapchain's granted colour space reports the difference. These flags are what a caller uses to
  // decide whether to offer the controls at all, so reporting them here keeps that answer from being
  // "nothing, because the backend never said".
  mCapabilities.displayP3Swapchain = true;
  mCapabilities.hdr10Swapchain = true;

  mBackendName = "coral/backend/metal";
  return Result::Success;
}

void MetalDevice::buildCapabilities(const DeviceConfiguration& configuration) {
  (void) configuration;

  mVendorName = [mDevice supportsFamily:MTLGPUFamilyApple1] ? "Apple" : "unknown vendor";
  mDeviceName = toStdString(mDevice.name);
  mApiVersion = "Metal 2.2+";
  mDriverVersion = operatingSystemVersion();

  mInfo = DeviceInfo{};
  mInfo.backend = Backend::Metal;
  mInfo.vendor = mVendorName.c_str();
  mInfo.deviceName = mDeviceName.c_str();
  mInfo.apiVersion = mApiVersion.c_str();
  mInfo.driverVersion = mDriverVersion.c_str();
  mInfo.backendName = mBackendName.c_str();
  mInfo.acceptsSpirv = true;
  mInfo.acceptsGlslDirect = false;

  Limits limits;
  // Metal's documented maxima. The device has no query for most of them, and a limit that is *reported* lower than the
  // hardware's is a performance ceiling rather than a correctness bug, so the conservative documented floor is used.
  limits.maxTextureDimension1D = 16384;
  limits.maxTextureDimension2D = 16384;
  limits.maxTextureDimension3D = 2048;
  limits.maxTextureDimensionCube = 16384;
  limits.maxTextureArrayLayers = 2048;

  limits.maxVertexBuffers = kMaxVertexBufferSlots;
  limits.maxVertexAttributes = 16;
  limits.maxVertexBufferStride = 2048;
  limits.maxBufferSize = 256u * 1024u * 1024u;
  // A `constant` address space buffer can be at most 64 KiB per stage; that is the real ceiling for a uniform block.
  limits.maxUniformBufferBindingSize = 64u * 1024u;
  limits.maxStorageBufferBindingSize = 128u * 1024u * 1024u;

  limits.maxUniformBuffersPerStage = 12;
  limits.maxStorageBuffersPerStage = 8;
  limits.maxSampledTexturesPerStage = 16;
  limits.maxStorageTexturesPerStage = 8;
  limits.maxSamplersPerStage = 16;
  limits.maxBoundDescriptorSets = kMaxDescriptorSetLayouts;

  limits.maxPushConstantSize = 128;
  limits.maxColorAttachments = kMaxColorAttachments;

  const MTLSize threadgroup = mDevice.maxThreadsPerThreadgroup;
  limits.maxComputeWorkgroupSizeX = std::min<uint32_t>(static_cast<uint32_t>(threadgroup.width), 1024);
  limits.maxComputeWorkgroupSizeY = std::min<uint32_t>(static_cast<uint32_t>(threadgroup.height), 1024);
  limits.maxComputeWorkgroupSizeZ = std::min<uint32_t>(static_cast<uint32_t>(threadgroup.depth), 64);
  limits.maxComputeInvocationsPerWorkgroup =
          static_cast<uint32_t>(threadgroup.width * threadgroup.height * threadgroup.depth);
  limits.maxComputeWorkgroupCountX = 65535;
  limits.maxComputeWorkgroupCountY = 65535;
  limits.maxComputeWorkgroupCountZ = 65535;
  limits.maxFramesInFlight = mMaxFramesInFlight;

  CapabilityBuilder builder(mCapabilities);
  builder.limit(limits);
  builder.apiVersion(mApiVersion.c_str());

  // The backend samples timestamps with a blit encoder, so the blit sampling point -- not the stage or draw one -- is
  // what decides whether the feature exists. Asking about the wrong point reported the feature present on a device
  // whose blit encoder then asserted, which is exactly the check-that-cannot-fail shape this codebase keeps finding.
  const bool counterSampling = [mDevice supportsCounterSampling:MTLCounterSamplingPointAtBlitBoundary];
  const bool multisample = [mDevice supportsTextureSampleCount:2] || [mDevice supportsTextureSampleCount:4];

  builder
          // Compute is core Metal on every device this backend runs on.
          .feature(Feature::ComputeShader, true)
          .feature(Feature::ComputeShaderAtomics, true)
          .feature(Feature::IndirectDispatch, true)
          .feature(Feature::ComputeWorkgroupBarrier, true)
          // There is no geometry or tessellation stage in this backend. Metal's tessellation is a separate API the RHI
          // has no vocabulary for, so both are recorded as absences a caller can act on rather than as "unsupported".
          .feature(Feature::GeometryShader, false, FeatureUnavailableReason::ArchitecturallyImpossible)
          .feature(Feature::TessellationShader, false, FeatureUnavailableReason::NotImplementedByBackend)
          // Texturing.
          .feature(Feature::Texture3D, true)
          .feature(Feature::TextureCube, true)
          .feature(Feature::TextureArray, true)
          .feature(Feature::TextureCubeArray, true)
          .feature(Feature::SamplerAnisotropy, true)
          .feature(Feature::SamplerBorderClamp, true)
          .feature(Feature::TextureLod, true)
          .feature(Feature::SeparateSamplers, true)
          .feature(Feature::StorageTexture, true)
          .feature(Feature::ComparisonSampler, true)
          // Render targets and rasterisation.
          .feature(Feature::MultipleRenderTargets, true)
          .feature(Feature::MultisampleResolve, multisample)
          // Per-sample shading would need `[[sample_id]]`/`[[sample_mask]]` plumbing this backend does not generate.
          .feature(Feature::SampleRateShading, false, FeatureUnavailableReason::NotImplementedByBackend)
          .feature(Feature::AlphaToCoverage, true)
          // Wireframe fill is a macOS-only Metal feature; on iOS the setter does not exist, so claiming it would make
          // the command buffer's guarded call the only thing standing between a caller and a wrong picture.
          .feature(Feature::FillModeNonSolid, !mIos,
                  mIos ? FeatureUnavailableReason::ArchitecturallyImpossible
                       : FeatureUnavailableReason::Available)
          .feature(Feature::DepthClamp, false, FeatureUnavailableReason::ArchitecturallyImpossible)
          .feature(Feature::ProgrammableBlending, false, FeatureUnavailableReason::ArchitecturallyImpossible)
          .feature(Feature::LogicOp, false, FeatureUnavailableReason::ArchitecturallyImpossible)
          .feature(Feature::IntegerVertexAttributes, true)
          // Synchronisation.
          .feature(Feature::HostWaitFence, true)
          // `MTLSharedEvent` is a timeline semaphore by construction, and it is what fences and semaphores are built
          // from here, so this is a statement about the primitive rather than about an RHI entry point (there is none).
          .feature(Feature::TimelineSemaphore, true)
          .feature(Feature::QueryObjects, true)
          .feature(Feature::TimestampQuery, counterSampling)
          .feature(Feature::PipelineStatisticsQuery, false, FeatureUnavailableReason::NotImplementedByBackend)
          // Presentation.
          .feature(Feature::Swapchain, true)
          .feature(Feature::NativeSurface, true);

  // Sample counts. 1 is always available; the rest is the device's own answer, which is what makes a 4x request on a
  // device that only does 2x degrade rather than fail.
  builder.sampleCount(1);
  uint32_t maxSamples = 1;
  for (const uint32_t count: { 2u, 4u, 8u }) {
    if ([mDevice supportsTextureSampleCount:count]) {
      builder.sampleCount(count);
      maxSamples = std::max(maxSamples, count);
    }
  }

  // Per-format support. Metal has no query for "can this format be a colour attachment"; the mapping table is the
  // authority, and the platform gating below is what keeps a format this GPU cannot read from being claimed.
  const bool appleGpu = [mDevice supportsFamily:MTLGPUFamilyApple2];
  for (uint32_t raw = 1; raw <= static_cast<uint32_t>(PixelFormat::ASTC_8x8_SRGB); ++raw) {
    const auto format = static_cast<PixelFormat>(raw);
    const MTLPixelFormat mtl = toMtlPixelFormat(format);
    if (mtl == MTLPixelFormatInvalid) continue;
    // BC is a macOS format; ETC2 and ASTC are Apple-GPU formats (an Intel Mac's Metal device has neither).
    if (mIos && isBcFormat(format)) continue;
    if (!appleGpu && isEtc2OrAstcFormat(format)) continue;

    const PixelFormatInfo& info = getPixelFormatInfo(format);
    TextureFormatCapabilities support;
    support.sampled = !info.depth;
    support.renderable = info.renderable;
    support.blendable = info.renderable && !info.depth;
    support.storage = !info.depth && !isCompressedFormat(format);
    support.multisampleResolve = support.renderable;
    if (support.renderable) {
      support.sampleCounts[1] = true;
      for (const uint32_t count: { 2u, 4u, 8u }) {
        if ([mDevice supportsTextureSampleCount:count]) support.sampleCounts[count] = true;
      }
    }
    builder.textureFormat(format, support);
  }

  mCapabilities.maxTextureSize = limits.maxTextureDimension2D;
  mCapabilities.maxCubeMapSize = limits.maxTextureDimensionCube;
  mCapabilities.maxRenderbufferSize = limits.maxTextureDimension2D;
  mCapabilities.maxViewportWidth = limits.maxTextureDimension2D;
  mCapabilities.maxViewportHeight = limits.maxTextureDimension2D;
  mCapabilities.maxFragmentTextureUnits = 31;
  mCapabilities.maxVertexTextureUnits = 31;
  mCapabilities.maxCombinedTextureUnits = 31;
  mCapabilities.maxSamplerAnisotropy = 16.0f;
  mCapabilities.maxSamples = maxSamples;

  mCapabilities.acceptsSpirv = true;
  mCapabilities.acceptsGlslDirect = false;
  mCapabilities.runtimeShaderCompilation = isGlslCompilerAvailable();
  mCapabilities.supportsDebugMarkers = true;
  // Mailbox is granted by disabling display sync and allowing a three-deep drawable pool. `displaySyncEnabled` does not
  // exist on iOS, where presentation is always in step with the display, so there the capability is false and a request
  // is reported as the Fifo it actually got.
  mCapabilities.mailboxPresent = !mIos;

  if (!mCapabilities.queryTextureFormat(PixelFormat::R8G8B8A8_UNORM).sampled ||
      !mCapabilities.queryTextureFormat(PixelFormat::R8G8B8A8_UNORM).renderable) {
    emitMessage(MessageSeverity::Warning,
            "this Metal device does not support R8G8B8A8_UNORM as a sampled render target; the canvas cannot draw");
  }
}

Result MetalDevice::createFallbackResources() {
  // A draw that samples a descriptor the caller never wrote must still bind *something*: Metal's validation fails an
  // unbound texture or sampler, and the Vulkan backend has the same policy for the same reason.
  MTLTextureDescriptor* textureDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                                width:1
                                                                                               height:1
                                                                                            mipmapped:NO];
  textureDescriptor.usage = MTLTextureUsageShaderRead;
  // A private texture cannot be filled with `replaceRegion:`. The 1x1 fallback is uploaded once from the
  // CPU, and on Apple's paravirtual Metal device the private upload path faults instead of failing
  // validation; shared storage is the correct mode for this CPU-written texture.
  textureDescriptor.storageMode = MTLStorageModeShared;
  mFallbackTexture = [mDevice newTextureWithDescriptor:textureDescriptor];
  if (mFallbackTexture == nil) return Result::OutOfMemory;
  mFallbackTexture.label = @"coral/fallback-texture";
  const uint8_t pixel[4] = { 0, 0, 0, 0 };
  [mFallbackTexture replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:pixel bytesPerRow:4];

  MTLSamplerDescriptor* samplerDescriptor = [MTLSamplerDescriptor new];
  samplerDescriptor.minFilter = MTLSamplerMinMagFilterLinear;
  samplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
  samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
  samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
  mFallbackSampler = [mDevice newSamplerStateWithDescriptor:samplerDescriptor];
  if (mFallbackSampler == nil) return Result::OutOfMemory;

  // A uniform buffer binding must point at a buffer of at least the range the shader reads; 1 KiB of zeros is enough
  // for the small blocks the canvas declares, and a shader that reads more of an unwritten block is reading garbage
  // either way.
  mFallbackBuffer = [mDevice newBufferWithLength:1024 options:MTLResourceStorageModeShared];
  if (mFallbackBuffer == nil) return Result::OutOfMemory;
  std::memset(mFallbackBuffer.contents, 0, 1024);

  // The fan index buffer is created here rather than lazily because a command buffer may be recorded on any thread:
  // a lazy creation would be a write to device state from the recording path, which is exactly the kind of race the
  // RHI's "creation is device-thread only" rule exists to prevent.
  id<MTLBuffer> fanBuffer = nil;
  if (const Result fan = ensureFanIndexBuffer(&fanBuffer); !isSuccess(fan)) return fan;
  return Result::Success;
}

// ==================================================================================================
// Identity, queues, command buffers
// ==================================================================================================

void* MetalDevice::nativeDevice() const noexcept {
  return (__bridge void*) mDevice;
}

void* MetalDevice::nativeQueue() const noexcept {
  return (__bridge void*) mQueue;
}

DeviceMemoryStatistics MetalDevice::memoryStatistics() const noexcept {
  DeviceMemoryStatistics statistics;
  statistics.textureBytes = mResidentTextureBytes;
  statistics.bufferBytes = mResidentBufferBytes;
  return statistics;
}

Queue* MetalDevice::getQueue(const QueueType type) noexcept {
  return type == QueueType::Graphics ? &mGraphicsQueueObject : nullptr;
}

Result MetalDevice::createCommandBuffer(const CommandBufferDescriptor& descriptor,
        CommandBuffer** outCommandBuffer) {
  if (outCommandBuffer == nullptr) return Result::InvalidArgument;
  *outCommandBuffer = nullptr;
  auto commandBuffer = std::make_unique<MetalCommandBuffer>(*this, descriptor);
  *outCommandBuffer = commandBuffer.release();
  return Result::Success;
}

void MetalDevice::destroyCommandBuffer(CommandBuffer* commandBuffer) {
  if (commandBuffer == nullptr) return;
  auto* metalCommandBuffer = dynamic_cast<MetalCommandBuffer*>(commandBuffer);
  if (metalCommandBuffer == nullptr) return;
  // Abandoning an unsubmitted recording is deliberate: the device is the owner, and a caller that dropped a recorded
  // buffer expected it to be discarded, not executed.
  metalCommandBuffer->abandon();
  delete metalCommandBuffer;
}

Result MetalDevice::executeCommandBuffer(CommandBuffer& commandBuffer) {
  auto* metalCommandBuffer = dynamic_cast<MetalCommandBuffer*>(&commandBuffer);
  if (metalCommandBuffer == nullptr) return Result::InvalidArgument;
  if (!metalCommandBuffer->isEnded()) {
    emitMessage(MessageSeverity::Error, "executeCommandBuffer requires a command buffer that has been ended");
    return Result::InvalidArgument;
  }
  std::lock_guard lock(mQueueMutex);
  return metalCommandBuffer->submit(FenceHandle::invalid(), SemaphoreHandle::invalid(), SemaphoreHandle::invalid());
}

Result MetalDevice::submitCommandBuffer(MetalCommandBuffer& commandBuffer, const FenceHandle fence,
        const SemaphoreHandle waitSemaphore, const SemaphoreHandle signalSemaphore) {
  std::lock_guard lock(mQueueMutex);
  return commandBuffer.submit(fence, waitSemaphore, signalSemaphore);
}

// ==================================================================================================
// Synchronisation
// ==================================================================================================

uint64_t MetalDevice::nextCompletionValue() noexcept {
  return mCompletionValue.fetch_add(1, std::memory_order_acq_rel) + 1;
}

void MetalDevice::noteAsyncSubmission(const uint32_t count) noexcept {
  mAsyncSubmissions.fetch_add(count, std::memory_order_acq_rel);
}

void MetalDevice::acknowledgeAsyncSubmission(const uint32_t count) noexcept {
  uint32_t current = mAsyncSubmissions.load(std::memory_order_acquire);
  while (true) {
    const uint32_t next = current > count ? current - count : 0;
    if (mAsyncSubmissions.compare_exchange_weak(current, next, std::memory_order_acq_rel)) return;
  }
}

Result MetalDevice::createFence(const FenceDescriptor& descriptor, FenceHandle* outFence) {
  if (outFence == nullptr) return Result::InvalidArgument;
  *outFence = FenceHandle::invalid();

  MetalFence fence;
  fence.event = [mDevice newSharedEvent];
  if (fence.event == nil) return Result::OutOfMemory;
  fence.target = 1;
  fence.signalled = descriptor.flag == FenceFlag::Signalled;
  if (fence.signalled) {
    // "Signalled" is one value already reached, so a wait returns immediately and the first submission signals two.
    fence.event.signaledValue = 1;
  }
  if (descriptor.label != nullptr) fence.event.label = [NSString stringWithUTF8String:descriptor.label];

  *outFence = mFences.allocate(std::move(fence));
  return outFence->valid() ? Result::Success : Result::OutOfMemory;
}

void MetalDevice::destroyFence(const FenceHandle fence) {
  if (MetalFence* payload = mFences.get(fence); payload != nullptr) {
    payload->event = nil;
    mFences.free(fence);
  }
}

Result MetalDevice::getFenceStatus(const FenceHandle fenceHandle) const {
  const MetalFence* fence = mFences.get(fenceHandle);
  if (fence == nullptr) return Result::InvalidHandle;
  if (fence->signalled) return Result::Success;
  return fence->event.signaledValue >= fence->target ? Result::Success : Result::NotReady;
}

Result MetalDevice::waitForFenceValue(MetalFence& fence, const uint64_t timeoutNanoseconds) {
  if (fence.signalled) {
    acknowledgeAsyncSubmission(fence.pending);
    fence.pending = 0;
    return Result::Success;
  }
  if (fence.event == nil) return Result::InvalidHandle;

  // `waitUntilSignaledValue:` takes milliseconds. UINT64_MAX means "wait forever" in the RHI, which is spelled here as
  // the largest value the API accepts rather than as a zero timeout.
  const uint64_t milliseconds = timeoutNanoseconds == std::numeric_limits<uint64_t>::max()
                                        ? std::numeric_limits<uint32_t>::max()
                                        : std::min<uint64_t>(timeoutNanoseconds / 1000000ull + 1ull,
                                                  std::numeric_limits<uint32_t>::max());
  const bool reached = [fence.event waitUntilSignaledValue:fence.target timeoutMS:milliseconds];
  if (!reached) return timeoutNanoseconds == 0 ? Result::NotReady : Result::Timeout;

  fence.signalled = true;
  acknowledgeAsyncSubmission(fence.pending);
  fence.pending = 0;
  return Result::Success;
}

Result MetalDevice::waitForFence(const FenceHandle fenceHandle, const uint64_t timeoutNanoseconds) {
  MetalFence* fence = mFences.get(fenceHandle);
  if (fence == nullptr) return Result::InvalidHandle;
  return waitForFenceValue(*fence, timeoutNanoseconds);
}

Result MetalDevice::resetFence(const FenceHandle fenceHandle) {
  MetalFence* fence = mFences.get(fenceHandle);
  if (fence == nullptr) return Result::InvalidHandle;
  // The event's value only grows, so "unsignalled" is a target past the current value. A reset while a submission that
  // refers to this fence is still in flight is a caller error, exactly as it is on Vulkan.
  fence->target = fence->event.signaledValue + 1;
  fence->signalled = false;
  return Result::Success;
}

Result MetalDevice::createSemaphore(const SemaphoreDescriptor& descriptor, SemaphoreHandle* outSemaphore) {
  if (outSemaphore == nullptr) return Result::InvalidArgument;
  *outSemaphore = SemaphoreHandle::invalid();

  MetalSemaphore semaphore;
  semaphore.event = [mDevice newSharedEvent];
  if (semaphore.event == nil) return Result::OutOfMemory;
  if (descriptor.label != nullptr) semaphore.event.label = [NSString stringWithUTF8String:descriptor.label];

  *outSemaphore = mSemaphores.allocate(std::move(semaphore));
  return outSemaphore->valid() ? Result::Success : Result::OutOfMemory;
}

void MetalDevice::destroySemaphore(const SemaphoreHandle semaphore) {
  if (MetalSemaphore* payload = mSemaphores.get(semaphore); payload != nullptr) {
    payload->event = nil;
    mSemaphores.free(semaphore);
  }
}

Result MetalDevice::waitIdle() {
  std::lock_guard lock(mQueueMutex);
  const uint64_t target = mCompletionValue.load(std::memory_order_acquire);
  if (target != 0 && mCompletionEvent != nil) {
    [mCompletionEvent waitUntilSignaledValue:target timeoutMS:std::numeric_limits<uint32_t>::max()];
  }
  mAsyncSubmissions.store(0, std::memory_order_release);
  return Result::Success;
}

bool MetalDevice::isResourceReadyToDestroy(const TextureHandle handle) const {
  return mAsyncSubmissions.load(std::memory_order_acquire) == 0 && mTextures.isAlive(handle);
}

bool MetalDevice::isResourceReadyToDestroy(const BufferHandle handle) const {
  return mAsyncSubmissions.load(std::memory_order_acquire) == 0 && mBuffers.isAlive(handle);
}

// ==================================================================================================
// Fallbacks and helpers shared with the command buffer
// ==================================================================================================

Result MetalDevice::ensureFanIndexBuffer(id<MTLBuffer>* outBuffer) {
  if (outBuffer == nullptr) return Result::InvalidArgument;
  if (mFanIndexBuffer != nil) {
    *outBuffer = mFanIndexBuffer;
    return Result::Success;
  }

  // Metal has no triangle-fan primitive. The translation every portable renderer uses is the same one the Vulkan
  // backend performs: a fan of n vertices becomes n-2 triangles with indices (0, i-1, i), drawn from a shared index
  // buffer with `baseVertex` carrying the caller's first vertex.
  constexpr uint32_t kMaxVertices = 65535;
  constexpr uint32_t kIndexCount = (kMaxVertices - 2) * 3;
  std::vector<uint16_t> indices(kIndexCount);
  for (uint32_t fan = 2; fan < kMaxVertices; ++fan) {
    const uint32_t base = (fan - 2) * 3;
    indices[base + 0] = 0;
    indices[base + 1] = static_cast<uint16_t>(fan - 1);
    indices[base + 2] = static_cast<uint16_t>(fan);
  }
  mFanIndexBuffer = [mDevice newBufferWithBytes:indices.data()
                                        length:indices.size() * sizeof(uint16_t)
                                       options:MTLResourceStorageModeShared];
  if (mFanIndexBuffer == nil) return Result::OutOfMemory;
  mFanIndexBuffer.label = @"coral/fan-index-buffer";
  mFanIndexCapacity = kMaxVertices;
  *outBuffer = mFanIndexBuffer;
  return Result::Success;
}

Result MetalDevice::uploadToBuffer(id<MTLBuffer> buffer, const uint64_t offset, const void* data, const size_t size) {
  if (buffer == nil || data == nullptr || size == 0) return Result::InvalidArgument;
  if (offset + size > buffer.length) return Result::InvalidArgument;

  id<MTLCommandBuffer> commandBuffer = [mQueue commandBuffer];
  id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
  if (blit == nil) return Result::Error;
  // A small inline upload would need a staging buffer of its own; creating one is cheap and keeps the path uniform for
  // any size. It is released by ARC once the command buffer has completed.
  id<MTLBuffer> staging = [mDevice newBufferWithBytes:data length:size options:MTLResourceStorageModeShared];
  if (staging == nil) return Result::OutOfMemory;
  [blit copyFromBuffer:staging sourceOffset:0 toBuffer:buffer destinationOffset:offset size:size];
  [blit endEncoding];
  [commandBuffer commit];
  [commandBuffer waitUntilCompleted];
  if (commandBuffer.status == MTLCommandBufferStatusError) {
    emitMessage(MessageSeverity::Error,
            std::string("a Metal buffer upload failed: ") + describe(commandBuffer.error));
    return Result::Error;
  }
  return Result::Success;
}

Result MetalDevice::resolveTextureView(const TextureViewHandle handle, MetalTextureView& out) const {
  if (!handle.valid()) return Result::InvalidHandle;
  if (handle == defaultFramebufferView()) {
    // The sentinel names the current frame's drawable. It is resolved here rather than stored in the view pool
    // because a drawable is only valid between `beginFrame` and `endFrame`.
    if (mCurrentFrameSwapchain == nullptr || !mCurrentFrameSwapchain->frameAcquired) {
      return Result::InvalidHandle;
    }
    id<CAMetalDrawable> drawable = mCurrentFrameSwapchain->drawable;
    if (drawable == nil) return Result::InvalidHandle;
    out = MetalTextureView{};
    out.texture = drawable.texture;
    out.type = drawable.texture.textureType;
    out.format = mCurrentFrameSwapchain->format;
    out.width = static_cast<uint32_t>(drawable.texture.width);
    out.height = static_cast<uint32_t>(drawable.texture.height);
    out.sampleCount = 1;
    return Result::Success;
  }

  const MetalTextureView* view = mTextureViews.get(handle);
  if (view == nullptr) return Result::InvalidHandle;
  out = *view;
  return Result::Success;
}

// ==================================================================================================
// Diagnostics
// ==================================================================================================

void MetalDevice::setDebugMessageCallback(const DebugMessageCallback callback, void* userData) {
  mDebugCallbackAtomic.store(callback, std::memory_order_release);
  mDebugUserDataAtomic.store(userData, std::memory_order_release);
}

void MetalDevice::emitMessage(const MessageSeverity severity, const std::string& message) const {
  const DebugMessageCallback callback = mDebugCallbackAtomic.load(std::memory_order_acquire);
  if (callback != nullptr) {
    callback(severity, message.c_str(), mDebugUserDataAtomic.load(std::memory_order_acquire));
    return;
  }
  switch (severity) {
    case MessageSeverity::Verbose:
    case MessageSeverity::Info:
      MINILOG_INFO("[backend/metal] {}", message);
      break;
    case MessageSeverity::Warning:
      MINILOG_WARN("[backend/metal] {}", message);
      break;
    case MessageSeverity::Error:
    case MessageSeverity::Fatal:
      MINILOG_ERROR("[backend/metal] {}", message);
      break;
  }
}

void MetalDevice::setTextureLabel(const TextureHandle texture, const char* label) {
  if (MetalTexture* payload = mTextures.get(texture); payload != nullptr && label != nullptr) {
    payload->texture.label = [NSString stringWithUTF8String:label];
  }
}

void MetalDevice::setBufferLabel(const BufferHandle buffer, const char* label) {
  if (MetalBuffer* payload = mBuffers.get(buffer); payload != nullptr && label != nullptr) {
    payload->buffer.label = [NSString stringWithUTF8String:label];
  }
}

void MetalDevice::beginDebugRegion(const char* label) {
  // A Metal debug group is scoped to a command buffer or encoder, and the device has neither. Emitting it as a message
  // keeps the call visible in a log rather than pretending it reached a capture.
  if (label != nullptr) emitMessage(MessageSeverity::Verbose, std::string("debug region: ") + label);
}

void MetalDevice::endDebugRegion() {
  emitMessage(MessageSeverity::Verbose, "debug region end");
}

void MetalDevice::appendDiagnostics(std::string& out) const {
  out += "RHI backend: Metal\n  device:        ";
  out += mInfo.deviceName != nullptr ? mInfo.deviceName : "";
  out += "\n  vendor:        ";
  out += mInfo.vendor != nullptr ? mInfo.vendor : "";
  out += "\n  API version:   ";
  out += mInfo.apiVersion != nullptr ? mInfo.apiVersion : "";
  out += "\n  driver:        ";
  out += mInfo.driverVersion != nullptr ? mInfo.driverVersion : "";
  out += "\n  GPU family:    ";
  out += gpuFamilyName(mDevice);
  out += "\n  unified memory: ";
  out += mDevice.hasUnifiedMemory ? "yes" : "no";
  out += "\n  platform:      ";
  out += mIos ? "iOS" : "macOS";
  out += "\n  shaders:       SPIR-V translated to MSL at pipeline creation";
  out += "\n  GLSL input:    ";
  out += isGlslCompilerAvailable() ? "yes (compiled to Vulkan SPIR-V, then MSL)" : "no (shaderc absent)";
  out += "\n  validation:    ";
  out += mValidation ? "requested (Metal validation is a scheme setting, not a runtime flag)" : "no";
  out += "\n  frames in flight: ";
  out += std::to_string(mMaxFramesInFlight);
  out += "\n\n";
  mCapabilities.appendReport(out);

  out += "\n  Live resources: textures=";
  out += std::to_string(mTextures.liveCount());
  out += " buffers=";
  out += std::to_string(mBuffers.liveCount());
  out += " samplers=";
  out += std::to_string(mSamplers.liveCount());
  out += " shaderModules=";
  out += std::to_string(mShaderModules.liveCount());
  out += " renderPipelines=";
  out += std::to_string(mRenderPipelines.liveCount());
  out += " surfaces=";
  out += std::to_string(mSurfaces.liveCount());
  out += " swapchains=";
  out += std::to_string(mSwapchains.liveCount());
  out += '\n';
}

// ==================================================================================================
// Factory
// ==================================================================================================

Result createMetalDevice(const DeviceConfiguration& configuration, Device** outDevice) {
  if (outDevice == nullptr) return Result::InvalidArgument;
  *outDevice = nullptr;

  // Ownership is handed over only once `initialize` succeeded, exactly as the Vulkan and OpenGL branches of the factory
  // do: a failure that leaked a half-built device would be invisible until it mattered.
  auto device = std::make_unique<MetalDevice>();
  const Result result = device->initialize(configuration);
  if (!isSuccess(result)) return result;
  *outDevice = device.release();
  return Result::Success;
}

} // namespace backend::metal
