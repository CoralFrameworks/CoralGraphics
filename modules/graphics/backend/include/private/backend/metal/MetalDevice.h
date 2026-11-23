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

#ifndef CORAL_BACKEND_METAL_DEVICE_H
#define CORAL_BACKEND_METAL_DEVICE_H

/**
 * @file MetalDevice.h
 *
 * The Metal backend's device. This is the Metal counterpart of `VulkanDevice`: one class that owns every resource
 * pool and implements `backend::Device`, with its definitions split across `MetalDevice.mm` (lifecycle and
 * capabilities), `MetalResources.mm` (textures, buffers, samplers, shaders, pipelines, descriptor sets),
 * `MetalPresentation.mm` (surface, swapchain, frame acquisition) and `MetalCommandBuffer.mm`.
 *
 * ## Why the private header names Metal types
 *
 * The public headers (`backend/`) may not mention a native API type, which is what keeps a backend a build-time
 * choice. A *private* header is different: it exists to be included only by the backend's own translation units, and
 * hiding `id<MTLTexture>` behind a `void*` there would only move every bridge cast into the middle of the code that
 * uses it. The Vulkan backend's private header names `VkImage` for the same reason.
 *
 * ## Ownership
 *
 * Every payload struct below holds an ARC-managed Metal object. ARC is what supplies the release: a `Payload` reset to
 * `Payload{}` -- which is exactly what `HandlePool::free` does -- drops the last reference. Nothing in this backend
 * calls `release` by hand, and every source file here is compiled with `-fobjc-arc` (the OpenGL backend's EAGL file is
 * not, which is why the flag is attached per source rather than to the target).
 */

#include <backend/Device.h>

#include "private/backend/metal/MetalEnums.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace backend::metal {

class MetalDevice;
class MetalCommandBuffer;

// ==================================================================================================
// Resource payloads
// ==================================================================================================

struct MetalTexture {
  id<MTLTexture> texture = nil;
  MTLTextureType type = MTLTextureType2D;
  PixelFormat format = PixelFormat::Undefined;
  Extent3D size = {};
  uint32_t mipLevelCount = 1;
  uint32_t sampleCount = 1;
  TextureUsages usage = 0;
  /** Bytes this texture's storage is charged for in `memoryStatistics`. */
  uint64_t bytes = 0;
};

/**
 * A view names either a texture this device created or the acquired swapchain image.
 *
 * The second case is the reason the payload holds an `id<MTLTexture>` rather than a `TextureHandle`: the default
 * framebuffer view has no texture handle -- it *is* whatever drawable the frame acquired -- and a view is the only
 * value the RHI has for naming it.
 */
struct MetalTextureView {
  id<MTLTexture> texture = nil;
  MTLTextureType type = MTLTextureType2D;
  PixelFormat format = PixelFormat::Undefined;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t sampleCount = 1;
};

struct MetalSampler {
  id<MTLSamplerState> state = nil;
  bool comparison = false;
};

struct MetalBuffer {
  id<MTLBuffer> buffer = nil;
  uint64_t size = 0;
  MemoryUsage memoryUsage = MemoryUsage::GpuOnly;
  /** Non-null while the buffer is mapped, so `unmapBuffer` can tell a mapped buffer from an unmapped one. */
  void* mapped = nullptr;
};

/**
 * A shader module holds the RHI's canonical input plus its reflection, not an `MTLLibrary`.
 *
 * The MSL is generated at *pipeline* creation, not here, because the translation has to be given the
 * `(set, binding)` -> argument-slot assignment and that assignment is a property of the pipeline layout. Compiling
 * early would bake in slot numbers that a different layout -- which the RHI explicitly permits, since a module may be
 * linked into several pipelines -- would silently invalidate.
 */
struct MetalShaderModule {
  ShaderStage stage = ShaderStage::Vertex;
  std::string label;
  std::string entryPoint = "main";

  /** The module's descriptor bindings, as `Device::getShaderReflection` reports them. */
  std::vector<ShaderBindingDeclaration> bindings;
  /** Owns the `const char*` names the declarations point at; reserved before the pointers are taken. */
  std::vector<std::string> bindingNames;

  /** The caller's words for a SPIR-V module, or the compiler's output for a GLSL module. */
  std::vector<uint32_t> inputSpirv;
  std::vector<uint32_t> compiledSpirv;
  bool compiledFromSource = false;

  ShaderSourceInfo sourceInfo;
  uint32_t pushConstantSize = 0;
};

struct MetalDescriptorBindingValue {
  uint32_t binding = 0;
  BindingType type = BindingType::TextureSampler;
  uint32_t arrayElement = 0;
  bool written = false;

  TextureHandle texture = TextureHandle::invalid();
  TextureViewHandle view = TextureViewHandle::invalid();
  TextureSamplerHandle sampler = TextureSamplerHandle::invalid();
  BufferBinding buffer = {};
};

/** A descriptor set is CPU-side state only; Metal has no set object, so the values are bound on the encoder. */
struct MetalDescriptorSet {
  DescriptorSetLayoutHandle layout = DescriptorSetLayoutHandle::invalid();
  std::vector<MetalDescriptorBindingValue> values;
  /** Bumped by every update, so a recording can tell a changed set from an unchanged one. */
  uint64_t version = 0;
};

struct MetalDescriptorSetLayout {
  DescriptorBinding bindings[kMaxDescriptorBindings] = {};
  uint32_t bindingCount = 0;
  /**
   * Index of each binding within its own resource class, counting array elements.
   *
   * The *absolute* Metal slot is `pipeline layout base + this index`, because a slot's meaning depends on which
   * pipeline layout the set is bound with -- the same set layout can be set 0 in one pipeline layout and set 1 in
   * another. Storing only the relative index here is what makes the layout reusable.
   *
   * A combined image sampler consumes one slot in both the texture and the sampler class, which is why the two are
   * tracked separately rather than through one shared index.
   */
  uint32_t textureIndex[kMaxDescriptorBindings] = {};
  uint32_t samplerIndex[kMaxDescriptorBindings] = {};
  uint32_t bufferIndex[kMaxDescriptorBindings] = {};

  uint32_t textureCount = 0;
  uint32_t samplerCount = 0;
  uint32_t bufferCount = 0;
};

/**
 * The pipeline layout is where descriptor slots become Metal argument indices.
 *
 * It owns no driver object, which is the largest structural difference from Vulkan. Its job is the assignment the MSL
 * translation and the command buffer's bind path must agree on; both read it from here, so they cannot drift.
 */
struct MetalPipelineLayout {
  DescriptorSetLayoutHandle setLayouts[kMaxDescriptorSetLayouts] = {};
  uint32_t setLayoutCount = 0;

  struct SetSlots {
    /** First texture slot this set's textures occupy. */
    uint32_t textureBase = 0;
    /** First sampler slot this set's samplers occupy. */
    uint32_t samplerBase = 0;
    /** First buffer slot this set's uniform/storage buffers occupy. */
    uint32_t bufferBase = 0;
  };
  SetSlots sets[kMaxDescriptorSetLayouts] = {};

  uint32_t textureCount = 0;
  uint32_t samplerCount = 0;
  uint32_t bufferCount = 0;
};

struct MetalRenderPipeline {
  id<MTLRenderPipelineState> state = nil;
  id<MTLDepthStencilState> depthStencilState = nil;
  PipelineLayoutHandle layout = PipelineLayoutHandle::invalid();

  /** A copy of the descriptor is what the command buffer reads for dynamic state (viewport default, blend, stencil). */
  RenderPipelineDescriptor descriptor = {};

  MTLPrimitiveType primitiveType = MTLPrimitiveTypeTriangle;
  /** True for a triangle fan, which Metal has no primitive for and which is drawn through a generated index buffer. */
  bool fanEmulation = false;

  /**
   * A vertex stream slot that exists only so missing attributes can be described, or `kNoDummyVertexSlot`.
   *
   * Metal refuses a pipeline whose vertex function reads attributes the vertex descriptor does not declare. A shared
   * vertex shader used by a variant with fewer streams therefore needs those attributes described by *something*; the
   * command buffer binds the device's zero-filled dummy buffer at this slot so the reads are defined rather than
   * out of bounds.
   */
  static constexpr uint32_t kNoDummyVertexSlot = 0xFFFFFFFFu;
  uint32_t dummyVertexSlot = kNoDummyVertexSlot;
};

struct MetalComputePipeline {
  id<MTLComputePipelineState> state = nil;
  PipelineLayoutHandle layout = PipelineLayoutHandle::invalid();
  uint32_t workgroupSizeX = 1;
  uint32_t workgroupSizeY = 1;
  uint32_t workgroupSizeZ = 1;
};

/** A render pass is a compatibility key: formats and sample count, which Metal's pipeline state is created against. */
struct MetalRenderPass {
  PixelFormat colorFormats[kMaxColorAttachments] = {};
  uint32_t colorAttachmentCount = 0;
  PixelFormat depthStencilFormat = PixelFormat::Undefined;
  uint32_t sampleCount = 1;
};

/** A framebuffer is a validated record; Metal's equivalent is the render pass descriptor built at record time. */
struct MetalFramebuffer {
  RenderPassHandle renderPass = RenderPassHandle::invalid();
  TextureViewHandle colorAttachments[kMaxColorAttachments] = {};
  uint32_t colorAttachmentCount = 0;
  TextureViewHandle depthStencilAttachment = TextureViewHandle::invalid();
  uint32_t width = 0;
  uint32_t height = 0;
};

struct MetalQuerySet {
  QueryType type = QueryType::Occlusion;
  uint32_t count = 1;
  /** Timestamp results come from a counter sample buffer. */
  id<MTLCounterSampleBuffer> counterSampleBuffer = nil;
  /** Occlusion counts come from a visibility result buffer, one 64-bit counter per query. */
  id<MTLBuffer> visibilityBuffer = nil;
};

/**
 * A fence is backed by a shared event and a monotonically increasing value.
 *
 * Metal has no CPU-waitable fence object, but `MTLSharedEvent` is exactly the primitive needed: the GPU signals a value
 * from the command buffer and the host waits for it, with a timeout. `target` is the value the most recent submission
 * will signal; `pending` counts the submissions that have encoded that signal and that `waitForFence` has not yet
 * accounted for.
 */
struct MetalFence {
  id<MTLSharedEvent> event = nil;
  uint64_t target = 0;
  uint32_t pending = 0;
  bool signalled = false;
};

/** A binary semaphore: the shared event's value is the number of signals encoded so far. */
struct MetalSemaphore {
  id<MTLSharedEvent> event = nil;
  uint64_t value = 0;
};

struct MetalSurface {
  SurfaceDescriptor descriptor = {};
  CAMetalLayer* layer = nil;
};

struct MetalSwapchain {
  SurfaceHandle surface = SurfaceHandle::invalid();
  uint32_t width = 0;
  uint32_t height = 0;
  PixelFormat format = PixelFormat::Undefined;
  MTLPixelFormat mtlFormat = MTLPixelFormatInvalid;
  PresentMode presentMode = PresentMode::Fifo;
  bool srgb = false;
  uint32_t imageCount = 2;
  bool outOfDate = false;

  /** The drawable acquired by `beginFrame`, or nil between frames. */
  id<CAMetalDrawable> drawable = nil;
  bool frameAcquired = false;
};

// ==================================================================================================
// Queue
// ==================================================================================================

/** Submission endpoint. Metal has one command queue; compute and transfer work rides the same queue. */
class MetalQueue final : public Queue {
  public:
  MetalQueue(MetalDevice& device, QueueType type) noexcept;

  [[nodiscard]] QueueType type() const noexcept override { return mType; }
  Result submit(CommandBuffer& commandBuffer, FenceHandle fence, SemaphoreHandle waitSemaphore,
          SemaphoreHandle signalSemaphore) override;
  Result waitIdle() override;

  private:
  MetalDevice& mDevice;
  QueueType mType;
};

// ==================================================================================================
// Device
// ==================================================================================================

class MetalDevice final : public Device {
  public:
  MetalDevice();
  ~MetalDevice() override;

  MetalDevice(const MetalDevice&) = delete;
  MetalDevice& operator=(const MetalDevice&) = delete;

  /** Brings the device up. Called through `backend::createDevice`; ownership is handed over only on success. */
  Result initialize(const DeviceConfiguration& configuration);

  // --- Identity and capabilities ----------------------------------------------------------------
  [[nodiscard]] void* nativeDevice() const noexcept override;
  [[nodiscard]] void* nativeQueue() const noexcept override;
  [[nodiscard]] uint32_t nativeQueueFamily() const noexcept override { return 0; }

  [[nodiscard]] const DeviceInfo& getInfo() const noexcept override { return mInfo; }
  [[nodiscard]] const Capabilities& getCapabilities() const noexcept override { return mCapabilities; }
  [[nodiscard]] DeviceMemoryStatistics memoryStatistics() const noexcept override;
  [[nodiscard]] Queue* getQueue(QueueType type) noexcept override;
  [[nodiscard]] bool supportsDeferredPresent() const noexcept override { return true; }

  // --- Command buffers --------------------------------------------------------------------------
  Result createCommandBuffer(const CommandBufferDescriptor& descriptor, CommandBuffer** outCommandBuffer) override;
  void destroyCommandBuffer(CommandBuffer* commandBuffer) override;

  // --- Textures, views, samplers ----------------------------------------------------------------
  Result createTexture(const TextureDescriptor& descriptor, TextureHandle* outTexture) override;
  void destroyTexture(TextureHandle texture) override;
  Result createTextureView(const TextureViewDescriptor& descriptor, TextureViewHandle* outView) override;
  void destroyTextureView(TextureViewHandle view) override;
  Result createSampler(const SamplerDescriptor& descriptor, TextureSamplerHandle* outSampler) override;
  void destroySampler(TextureSamplerHandle sampler) override;

  // --- Buffers ----------------------------------------------------------------------------------
  Result createBuffer(const BufferDescriptor& descriptor, BufferHandle* outBuffer) override;
  void destroyBuffer(BufferHandle buffer) override;
  Result mapBuffer(BufferHandle buffer, uint64_t offset, uint64_t size, void** outPointer) override;
  void unmapBuffer(BufferHandle buffer) override;

  // --- Shader modules ---------------------------------------------------------------------------
  Result createShaderModule(const ShaderModuleDescriptor& descriptor, ShaderModuleHandle* outModule) override;
  void destroyShaderModule(ShaderModuleHandle module) override;
  Result getShaderReflection(ShaderModuleHandle module, std::vector<ShaderBindingDeclaration>* outBindings) override;
  Result getShaderSourceInfo(ShaderModuleHandle module, ShaderSourceInfo* outInfo) override;
  Result getShaderSpirv(ShaderModuleHandle module, std::vector<uint32_t>* outWords) override;

  // --- Descriptor sets and layouts --------------------------------------------------------------
  Result createDescriptorSetLayout(const DescriptorSetLayoutDescriptor& descriptor,
          DescriptorSetLayoutHandle* outLayout) override;
  void destroyDescriptorSetLayout(DescriptorSetLayoutHandle layout) override;
  Result createDescriptorSet(DescriptorSetLayoutHandle layout, DescriptorSetHandle* outSet) override;
  void destroyDescriptorSet(DescriptorSetHandle set) override;
  Result updateDescriptorSet(DescriptorSetHandle set, const DescriptorWrite* writes, uint32_t writeCount) override;

  Result createPipelineLayout(const PipelineLayoutDescriptor& descriptor, PipelineLayoutHandle* outLayout) override;
  void destroyPipelineLayout(PipelineLayoutHandle layout) override;

  // --- Pipelines --------------------------------------------------------------------------------
  Result createRenderPipeline(const RenderPipelineDescriptor& descriptor, RenderPipelineHandle* outPipeline) override;
  void destroyRenderPipeline(RenderPipelineHandle pipeline) override;
  Result createComputePipeline(const ComputePipelineDescriptor& descriptor,
          ComputePipelineHandle* outPipeline) override;
  void destroyComputePipeline(ComputePipelineHandle pipeline) override;

  // --- Render passes and framebuffers -----------------------------------------------------------
  Result createRenderPass(const RenderPassDescriptor& descriptor, RenderPassHandle* outPass) override;
  void destroyRenderPass(RenderPassHandle renderPass) override;
  Result createFramebuffer(const FramebufferDescriptor& descriptor, FramebufferHandle* outFramebuffer) override;
  void destroyFramebuffer(FramebufferHandle framebuffer) override;

  // --- Queries and synchronisation --------------------------------------------------------------
  Result createQuerySet(const QuerySetDescriptor& descriptor, QuerySetHandle* outQuerySet) override;
  void destroyQuerySet(QuerySetHandle querySet) override;
  [[nodiscard]] Result getQuerySetCount(QuerySetHandle querySet, uint32_t* outCount) const override;
  Result getQueryResults(QuerySetHandle querySet, uint32_t firstQuery, uint32_t count, void* outData,
          size_t outDataSize) override;
  [[nodiscard]] bool isQueryResultAvailable(QuerySetHandle querySet, uint32_t query) const override;

  Result createFence(const FenceDescriptor& descriptor, FenceHandle* outFence) override;
  void destroyFence(FenceHandle fence) override;
  [[nodiscard]] Result getFenceStatus(FenceHandle fenceHandle) const override;
  Result waitForFence(FenceHandle fenceHandle, uint64_t timeoutNanoseconds) override;
  Result resetFence(FenceHandle fence) override;
  Result createSemaphore(const SemaphoreDescriptor& descriptor, SemaphoreHandle* outSemaphore) override;
  void destroySemaphore(SemaphoreHandle semaphore) override;

  // --- Execution --------------------------------------------------------------------------------
  Result executeCommandBuffer(CommandBuffer& commandBuffer) override;
  Result waitIdle() override;

  [[nodiscard]] bool isResourceReadyToDestroy(TextureHandle handle) const override;
  [[nodiscard]] bool isResourceReadyToDestroy(BufferHandle handle) const override;

  // --- Presentation -----------------------------------------------------------------------------
  Result createSurface(const SurfaceDescriptor& descriptor, SurfaceHandle* outSurface) override;
  void destroySurface(SurfaceHandle surface) override;

  Result getSwapchainInfo(SwapchainHandle swapchain, SwapchainInfo* outInfo) override;
  Result createSwapchain(const SwapchainDescriptor& descriptor, SwapchainHandle* outSwapchain) override;
  void destroySwapchain(SwapchainHandle swapchain) override;
  Result beginFrame(SwapchainHandle swapchain, SwapchainInfo* outInfo, SemaphoreHandle signalSemaphore) override;
  Result beginFrameWithDrawable(SwapchainHandle swapchain, void* nativeDrawable, SwapchainInfo* outInfo) override;
  Result tryBeginFrame(SwapchainHandle swapchain, SwapchainInfo* outInfo) override;
  Result endFrame(SwapchainHandle swapchain) override;
  Result endFrame(SwapchainHandle swapchain, SemaphoreHandle waitSemaphore) override;
  Result resizeSwapchain(SwapchainHandle swapchain, uint32_t width, uint32_t height) override;

  // --- Native interop ---------------------------------------------------------------------------
  [[nodiscard]] NativeTexture getNativeTexture(TextureHandle texture) const override;
  [[nodiscard]] NativeBuffer getNativeBuffer(BufferHandle buffer) const override;
  [[nodiscard]] Result importNativeTexture(const NativeTextureDescriptor& descriptor,
          TextureHandle* outTexture) override;
  [[nodiscard]] bool supportsNativeTextureImport(NativeTextureKind kind) const override;

  // --- Diagnostics ------------------------------------------------------------------------------
  void setDebugMessageCallback(DebugMessageCallback callback, void* userData) override;
  void setTextureLabel(TextureHandle texture, const char* label) override;
  void setBufferLabel(BufferHandle buffer, const char* label) override;
  void beginDebugRegion(const char* label) override;
  void endDebugRegion() override;
  void appendDiagnostics(std::string& out) const override;

  // --- Backend-internal surface, used by the command buffer --------------------------------------
  /** Emits a diagnostic through the installed callback, or the engine log when there is none. */
  void emitMessage(MessageSeverity severity, const std::string& message) const;

  [[nodiscard]] id<MTLDevice> metalDevice() const noexcept { return mDevice; }
  [[nodiscard]] id<MTLCommandQueue> commandQueue() const noexcept { return mQueue; }
  [[nodiscard]] bool isIos() const noexcept { return mIos; }
  [[nodiscard]] bool validationEnabled() const noexcept { return mValidation; }

  /**
   * Resolves @p handle to the Metal texture it names, including the default-framebuffer sentinel.
   *
   * A sentinel resolves to the current frame's drawable; when no frame is acquired it fails with
   * `Result::InvalidHandle`, which is what a caller drawing into a window before `beginFrame` deserves.
   */
  Result resolveTextureView(TextureViewHandle handle, MetalTextureView& out) const;

  /** @return a small zeroed buffer bound to a pipeline's dummy vertex slot. */
  [[nodiscard]] id<MTLBuffer> dummyVertexBuffer() const noexcept { return mFallbackBuffer; }

  /** @return the fallback 1x1 texture/sampler/buffer used for an unwritten descriptor binding. */
  [[nodiscard]] id<MTLTexture> fallbackTexture() const noexcept { return mFallbackTexture; }
  [[nodiscard]] id<MTLSamplerState> fallbackSampler() const noexcept { return mFallbackSampler; }
  [[nodiscard]] id<MTLBuffer> fallbackBuffer() const noexcept { return mFallbackBuffer; }

  /** @return the generated triangle-fan index buffer, creating it on first use. */
  Result ensureFanIndexBuffer(id<MTLBuffer>* outBuffer);
  /** @return how many vertices the fan index buffer covers. */
  [[nodiscard]] uint32_t fanIndexCapacity() const noexcept { return mFanIndexCapacity; }

  /** @return a new, monotonically increasing value for the device's completion timeline. */
  uint64_t nextCompletionValue() noexcept;
  /** The shared event every submission signals, so `waitIdle` has one value to wait for. */
  [[nodiscard]] id<MTLSharedEvent> completionEvent() const noexcept { return mCompletionEvent; }
  void noteAsyncSubmission(uint32_t count = 1) noexcept;
  void acknowledgeAsyncSubmission(uint32_t count) noexcept;

  /** Copies bytes into @p buffer with a one-shot blit. Used by `createBuffer`'s `initialData` path. */
  Result uploadToBuffer(id<MTLBuffer> buffer, uint64_t offset, const void* data, size_t size);

  // Payload accessors. Deliberately not part of `backend::Device`: a command buffer is the only other caller.
  MetalTexture* texturePayload(TextureHandle handle) noexcept { return mTextures.get(handle); }
  MetalTextureView* textureViewPayload(TextureViewHandle handle) noexcept { return mTextureViews.get(handle); }
  MetalBuffer* bufferPayload(BufferHandle handle) noexcept { return mBuffers.get(handle); }
  MetalSampler* samplerPayload(TextureSamplerHandle handle) noexcept { return mSamplers.get(handle); }
  MetalShaderModule* shaderModulePayload(ShaderModuleHandle handle) noexcept { return mShaderModules.get(handle); }
  MetalDescriptorSetLayout* descriptorSetLayoutPayload(DescriptorSetLayoutHandle handle) noexcept {
    return mDescriptorSetLayouts.get(handle);
  }
  MetalDescriptorSet* descriptorSetPayload(DescriptorSetHandle handle) noexcept { return mDescriptorSets.get(handle); }
  MetalPipelineLayout* pipelineLayoutPayload(PipelineLayoutHandle handle) noexcept {
    return mPipelineLayouts.get(handle);
  }
  MetalRenderPipeline* renderPipelinePayload(RenderPipelineHandle handle) noexcept {
    return mRenderPipelines.get(handle);
  }
  MetalComputePipeline* computePipelinePayload(ComputePipelineHandle handle) noexcept {
    return mComputePipelines.get(handle);
  }
  MetalRenderPass* renderPassPayload(RenderPassHandle handle) noexcept { return mRenderPasses.get(handle); }
  MetalQuerySet* querySetPayload(QuerySetHandle handle) noexcept { return mQuerySets.get(handle); }
  MetalFence* fencePayload(FenceHandle handle) noexcept { return mFences.get(handle); }
  MetalSemaphore* semaphorePayload(SemaphoreHandle handle) noexcept { return mSemaphores.get(handle); }
  MetalSwapchain* swapchainPayload(SwapchainHandle handle) noexcept { return mSwapchains.get(handle); }
  MetalSurface* surfacePayload(SurfaceHandle handle) noexcept { return mSurfaces.get(handle); }
  const MetalFence* fencePayload(FenceHandle handle) const noexcept { return mFences.get(handle); }
  const MetalQuerySet* querySetPayload(QuerySetHandle handle) const noexcept { return mQuerySets.get(handle); }

  Result submitCommandBuffer(MetalCommandBuffer& commandBuffer, FenceHandle fence, SemaphoreHandle waitSemaphore,
          SemaphoreHandle signalSemaphore);
  Result waitForFenceValue(MetalFence& fence, uint64_t timeoutNanoseconds);

  private:
  friend class MetalCommandBuffer;

  Result createDevice(const DeviceConfiguration& configuration);
  void buildCapabilities(const DeviceConfiguration& configuration);
  Result createFallbackResources();
  uint32_t resolveMaxFramesInFlight(const DeviceConfiguration& configuration) const;

  id<MTLDevice> mDevice = nil;
  id<MTLCommandQueue> mQueue = nil;

  bool mIos = false;
  bool mValidation = false;
  bool mInitialized = false;
  std::string mBackendName;
  std::string mVendorName;
  std::string mDeviceName;
  std::string mApiVersion;
  std::string mDriverVersion;
  DeviceInfo mInfo {};
  Capabilities mCapabilities {};
  uint32_t mMaxFramesInFlight = 1;

  mutable std::mutex mQueueMutex;
  std::atomic<uint32_t> mAsyncSubmissions { 0 };

  id<MTLSharedEvent> mCompletionEvent = nil;
  std::atomic<uint64_t> mCompletionValue { 0 };

  /**
   * The swapchain whose drawable the current frame is rendering into, or null between frames.
   *
   * Only one frame can be in flight on a device at a time (`beginFrame` refuses a second one before `endFrame`), so a
   * single pointer is enough for the default-framebuffer sentinel to resolve to the right drawable.
   */
  MetalSwapchain* mCurrentFrameSwapchain = nullptr;

  /** Installed by the caller and read on the emitting thread, so both halves are atomic. */
  std::atomic<DebugMessageCallback> mDebugCallbackAtomic { nullptr };
  std::atomic<void*> mDebugUserDataAtomic { nullptr };

  MetalQueue mGraphicsQueueObject;

  // Fallback resources: Metal validation fails a draw that samples an unbound texture, so an unwritten binding has to
  // resolve to something real -- the same policy the Vulkan backend follows.
  id<MTLTexture> mFallbackTexture = nil;
  id<MTLSamplerState> mFallbackSampler = nil;
  id<MTLBuffer> mFallbackBuffer = nil;
  id<MTLBuffer> mFanIndexBuffer = nil;
  uint32_t mFanIndexCapacity = 0;

  uint64_t mResidentTextureBytes = 0;
  uint64_t mResidentBufferBytes = 0;

  HandlePool<TextureTag, MetalTexture> mTextures;
  HandlePool<TextureViewTag, MetalTextureView> mTextureViews;
  HandlePool<TextureSamplerTag, MetalSampler> mSamplers;
  HandlePool<BufferTag, MetalBuffer> mBuffers;
  HandlePool<ShaderModuleTag, MetalShaderModule> mShaderModules;
  HandlePool<DescriptorSetLayoutTag, MetalDescriptorSetLayout> mDescriptorSetLayouts;
  HandlePool<DescriptorSetTag, MetalDescriptorSet> mDescriptorSets;
  HandlePool<PipelineLayoutTag, MetalPipelineLayout> mPipelineLayouts;
  HandlePool<RenderPipelineTag, MetalRenderPipeline> mRenderPipelines;
  HandlePool<ComputePipelineTag, MetalComputePipeline> mComputePipelines;
  HandlePool<RenderPassTag, MetalRenderPass> mRenderPasses;
  HandlePool<FramebufferTag, MetalFramebuffer> mFramebuffers;
  HandlePool<QuerySetTag, MetalQuerySet> mQuerySets;
  HandlePool<FenceTag, MetalFence> mFences;
  HandlePool<SemaphoreTag, MetalSemaphore> mSemaphores;
  HandlePool<SurfaceTag, MetalSurface> mSurfaces;
  HandlePool<SwapchainTag, MetalSwapchain> mSwapchains;
};

} // namespace backend::metal

#endif // CORAL_BACKEND_METAL_DEVICE_H
