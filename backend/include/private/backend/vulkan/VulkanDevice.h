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

#ifndef CORAL_BACKEND_VULKAN_DEVICE_H
#define CORAL_BACKEND_VULKAN_DEVICE_H

/**
 * @file VulkanDevice.h
 *
 * The Vulkan backend's device and the payload types its handles address.
 *
 * ## The shape, and why it is this one
 *
 * The RHI is already modelled on Vulkan (recording separated from submission, explicit descriptor sets,
 * load/store operations supplied at record time), so this backend is mostly a translation rather than a
 * design. Two decisions are worth stating because they are visible from the outside:
 *
 *  - **Dynamic rendering when available, render pass + framebuffer otherwise.** The RHI's `RenderPass`
 *    is documented as a compatibility key rather than a driver object, and the RHI supplies load/store
 *    operations and concrete attachments at `beginRenderPass`. That maps directly to
 *    `vkCmdBeginRendering` on Vulkan 1.3 / `VK_KHR_dynamic_rendering`. Vulkan 1.0/1.1 devices, which are
 *    still common on Android, take a fallback that creates a `VkRenderPass` per attachment shape and
 *    caches a `VkFramebuffer` per (pass, views, extent); clears are recorded with
 *    `vkCmdClearAttachments` so the render pass itself can keep `LOAD` and be reused for layered targets.
 *
 *  - **Every image lives in `VK_IMAGE_LAYOUT_GENERAL`.** The RHI's ping-pong model reads a texture the
 *    previous pass wrote, and a pass can sample a texture it is not writing. Transitioning between
 *    `COLOR_ATTACHMENT_OPTIMAL` and `SHADER_READ_ONLY_OPTIMAL` around every use is the alternative, and
 *    it is both more code and easy to get subtly wrong when a texture is sampled and rendered in the
 *    same frame. `GENERAL` is a supported layout for every use this backend has (sampled, attachment,
 *    transfer, clear), so the only layout transitions left are the ones the presentation engine forces:
 *    `GENERAL -> PRESENT_SRC_KHR` before a present and back after an acquire. The cost is that the
 *    driver cannot apply the optimisations a more specific layout allows; the correctness is worth it
 *    for a first implementation, and the note in `GPU后端支持Vulkan进度与问题记录.md` records it as a
 *    known performance item.
 *
 * ## Synchronisation
 *
 * `Device::executeCommandBuffer` and `Queue::submit` without a fence keep the OpenGL backend's
 * synchronous semantics. `Queue::submit` with a fence is asynchronous, forwards binary semaphores, and
 * lets a caller build a frame ring; `isResourceReadyToDestroy` tracks those submissions. The renderer
 * still uses the synchronous path, so an asynchronous frame ring is the remaining performance step.
 */

#include <backend/Device.h>
#include <backend/Handles.h>
#include <backend/Pipelines.h>

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include "private/backend/vulkan/VulkanX11Compat.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace backend::vulkan {

struct VulkanApi;

// ==================================================================================================
// Resource payloads. These are what a Handle addresses; they are private to the backend.
// ==================================================================================================

struct VulkanTexture {
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  VmaAllocationInfo allocationInfo{};
  /** A default view spanning every mip and layer, used when a descriptor names a bare texture handle. */
  VkImageView view = VK_NULL_HANDLE;

  PixelFormat format = PixelFormat::Undefined;
  VkFormat vkFormat = VK_FORMAT_UNDEFINED;
  TextureType type = TextureType::Texture2D;
  Extent3D extent{};
  uint32_t mipLevels = 1;
  uint32_t arrayLayers = 1;
  uint32_t sampleCount = 1;
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  TextureUsages usage = 0;
  bool depth = false;
  bool stencil = false;
  /**
   * The memory an imported texture is bound to, or `VK_NULL_HANDLE` for an image this device allocated.
   *
   * An external-memory image cannot go through VMA: the allocation *is* the producer's handle, it has to
   * be dedicated to this one image, and it is freed with `vkFreeMemory` instead of being returned to the
   * pool. Non-null is therefore what tells `destroyTexture` which of the two paths to take.
   */
  VkDeviceMemory importedMemory = VK_NULL_HANDLE;
  /** True for a caller-owned VkImage wrapped by VulkanExternalTexture. */
  bool externalImage = false;
  /**
   * The AHardwareBuffer reference this texture holds, released with the image (Android imports).
   *
   * Kept as oid* so this header does not have to include the Android hardware buffer header on every
   * platform; the import path casts it back and releases it through the libandroid.so entry point.
   */
  void* importedHardwareBuffer = nullptr;
  std::string label;
};

struct VulkanTextureView {
  TextureHandle texture = TextureHandle::invalid();
  VkImageView view = VK_NULL_HANDLE;
  VkImage image = VK_NULL_HANDLE;
  VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
  VkFormat vkFormat = VK_FORMAT_UNDEFINED;
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  PixelFormat format = PixelFormat::Undefined;
  Extent2D extent{};
  uint32_t baseMipLevel = 0;
  uint32_t mipLevelCount = 1;
  uint32_t baseArrayLayer = 0;
  uint32_t arrayLayerCount = 1;
  /** True for the sentinel that names the surface's own images; the rest of the fields are then unused. */
  bool isDefaultFramebuffer = false;
  std::string label;
};

struct VulkanSampler {
  VkSampler sampler = VK_NULL_HANDLE;
  bool comparison = false;
  std::string label;
};

/**
 * A buffer plus the allocation that backs it.
 *
 * `mappedPointer` is kept because `mapBuffer` has to hand back the same address `unmapBuffer` later
 * unmaps; asking VMA again would work but would also make two map calls on the same buffer look like
 * two independent mappings.
 */
struct VulkanBuffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  VmaAllocationInfo allocationInfo{};
  uint64_t size = 0;
  BufferUsages usage = 0;
  MemoryUsage memoryUsage = MemoryUsage::GpuOnly;
  void* mappedPointer = nullptr;
  std::string label;
};

struct VulkanShaderModule {
  VkShaderModule module = VK_NULL_HANDLE;
  ShaderStage stage = ShaderStage::Vertex;
  std::string label;
  /** The entry point the caller named; Vulkan requires it to be spelled out at pipeline creation. */
  std::string entryPoint = "main";

  /** Reflection in the public vocabulary; the pointers into `bindingNames` are stable once built. */
  std::vector<ShaderBindingDeclaration> bindings;
  std::vector<std::string> bindingNames;

  ShaderSourceInfo sourceInfo{};
  /** The words when this module was compiled from GLSL here. */
  std::vector<uint32_t> compiledSpirv;
  /** The caller's words when this module was supplied as `Language::SPIRV`. */
  std::vector<uint32_t> inputSpirv;
  bool compiledFromSource = false;

  /** Size of the module's push-constant block, as reported by reflection. 0 when it has none. */
  uint32_t pushConstantSize = 0;
};

struct VulkanDescriptorBinding {
  uint32_t binding = 0;
  BindingType type = BindingType::TextureSampler;
  uint32_t arraySize = 1;
  uint8_t stageFlags = 0;
  std::string debugName;
};

struct VulkanDescriptorSetLayout {
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  std::array<VulkanDescriptorBinding, kMaxDescriptorBindings> bindings{};
  uint32_t bindingCount = 0;
  std::string label;
};

/** One resolved value in a set; the same shape the OpenGL backend keeps. */
struct VulkanDescriptorBindingValue {
  uint32_t binding = 0;
  BindingType type = BindingType::TextureSampler;
  uint32_t arraySize = 1;
  /** Element index within an arrayed binding, filled in when the set is created. */
  uint32_t arrayIndex = 0;
  TextureHandle texture = TextureHandle::invalid();
  TextureSamplerHandle sampler = TextureSamplerHandle::invalid();
  BufferBinding buffer{};
  bool written = false;
  TextureViewHandle view = TextureViewHandle::invalid();
};

struct VulkanDescriptorSet {
  DescriptorSetLayoutHandle layout = DescriptorSetLayoutHandle::invalid();
  std::vector<VulkanDescriptorBindingValue> bindings;
  /**
   * Bumped by every `updateDescriptorSet`. A command buffer snapshots the set's contents at the draw
   * that uses it and keys the snapshot on this number, which is what makes "update a set, draw, update
   * it again, draw again" produce two different draws instead of the last one twice.
   */
  uint64_t version = 1;
  std::string label;
};

struct VulkanPipelineLayout {
  VkPipelineLayout layout = VK_NULL_HANDLE;
  std::array<VkDescriptorSetLayout, kMaxDescriptorSetLayouts> setLayouts{};
  uint32_t setLayoutCount = 0;
  std::string label;
};

struct VulkanRenderPipeline {
  VkPipeline pipeline = VK_NULL_HANDLE;
  PipelineLayoutHandle layout = PipelineLayoutHandle::invalid();
  RenderPipelineDescriptor descriptor{};
  /** True when the pipeline's topology is a fan, which Vulkan lacks and the draw path translates. */
  bool fanEmulation = false;
  std::string label;
};

struct VulkanComputePipeline {
  VkPipeline pipeline = VK_NULL_HANDLE;
  PipelineLayoutHandle layout = PipelineLayoutHandle::invalid();
  std::string label;
};

struct VulkanRenderPass {
  std::array<PixelFormat, kMaxColorAttachments> colorFormats{};
  uint32_t colorAttachmentCount = 0;
  PixelFormat depthStencilFormat = PixelFormat::Undefined;
  uint32_t sampleCount = 1;
  VkRenderPass renderPass = VK_NULL_HANDLE;
  std::string label;
};

struct VulkanFramebuffer {
  RenderPassHandle renderPass = RenderPassHandle::invalid();
  std::array<TextureViewHandle, kMaxColorAttachments> colorAttachments{};
  uint32_t colorAttachmentCount = 0;
  TextureViewHandle depthStencilAttachment = TextureViewHandle::invalid();
  uint32_t width = 0;
  uint32_t height = 0;
  std::string label;
};

/**
 * The identity of a fallback framebuffer: the render pass, the exact image views it names, and its size.
 *
 * The RHI's `RenderPassBeginInfo` carries views rather than a framebuffer handle, so the backend has to
 * derive one. A cache keyed on the views is what lets a window that presents the same swapchain image
 * and a layer that is redrawn every frame reuse their framebuffer instead of creating one per pass.
 */
struct VulkanFramebufferKey {
  VkRenderPass renderPass = VK_NULL_HANDLE;
  std::array<VkImageView, kMaxColorAttachments> colors{};
  VkImageView depth = VK_NULL_HANDLE;
  uint32_t colorCount = 0;
  uint32_t width = 0;
  uint32_t height = 0;

  [[nodiscard]] bool operator<(const VulkanFramebufferKey& other) const noexcept {
    return std::tie(renderPass, colors, depth, colorCount, width, height) <
           std::tie(other.renderPass, other.colors, other.depth, other.colorCount, other.width, other.height);
  }
};

struct VulkanQuerySet {
  VkQueryPool pool = VK_NULL_HANDLE;
  QueryType type = QueryType::Occlusion;
  uint32_t count = 0;
  std::string label;
};

struct VulkanFence {
  VkFence fence = VK_NULL_HANDLE;
  std::string label;
};

struct VulkanSemaphore {
  VkSemaphore semaphore = VK_NULL_HANDLE;
  std::string label;
};

struct VulkanSurface {
  SurfaceDescriptor descriptor{};
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  std::string label;
};

struct VulkanSwapchain {
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  SurfaceHandle surfaceHandle = SurfaceHandle::invalid();
  uint32_t width = 0;
  uint32_t height = 0;
  PixelFormat format = PixelFormat::B8G8R8A8_UNORM;
  VkFormat vkFormat = VK_FORMAT_B8G8R8A8_UNORM;
  PresentMode presentMode = PresentMode::Fifo;
  bool srgb = false;
  /** The colour space this swapchain was created with; `Srgb` unless negotiated otherwise. */
  SwapchainColorSpace colorSpace = SwapchainColorSpace::Srgb;
  uint32_t imageCount = 0;
  std::vector<VkImage> images;
  std::vector<VkImageView> views;
  /** The layout each image is currently in; `GENERAL` while rendering, `PRESENT_SRC_KHR` after present. */
  std::vector<VkImageLayout> layouts;
  uint32_t currentImage = 0;
  bool frameAcquired = false;
  /** True once acquire/present reported OutOfDate/Suboptimal; the next resize must recreate even at the same size. */
  bool outOfDate = false;
  std::string label;
};

/** One acquired swapchain image exposed for direct rendering. */
struct VulkanSwapchainTarget {
  SwapchainHandle swapchain = SwapchainHandle::invalid();
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  uint32_t width = 0;
  uint32_t height = 0;
  VkFormat vkFormat = VK_FORMAT_UNDEFINED;
  PixelFormat format = PixelFormat::Undefined;
  RenderPassHandle pass = RenderPassHandle::invalid();
};

// ==================================================================================================
// Queue
// ==================================================================================================

class VulkanDevice;

class VulkanQueue final : public Queue {
  public:
  VulkanQueue(VulkanDevice& device, VkQueue queue, QueueType type) : mDevice(&device), mQueue(queue), mType(type) {}

  [[nodiscard]] QueueType type() const noexcept override { return mType; }

  Result submit(CommandBuffer& commandBuffer, FenceHandle fence, SemaphoreHandle waitSemaphore,
          SemaphoreHandle signalSemaphore) override;

  Result waitIdle() override;

  private:
  VulkanDevice* mDevice = nullptr;
  VkQueue mQueue = VK_NULL_HANDLE;
  QueueType mType = QueueType::Graphics;
};

// ==================================================================================================
// Device
// ==================================================================================================

/**
 * The Vulkan device.
 *
 * Owns the instance, the physical and logical device, the VMA allocator, the pools every resource
 * comes from, and the swapchain. Everything created through the RHI is addressed by a handle whose
 * payload is one of the structs above; nothing driver-side is ever exposed to an upper layer.
 */
class VulkanDevice final : public Device {
  public:
  VulkanDevice();
  ~VulkanDevice() override;

  VulkanDevice(const VulkanDevice&) = delete;
  VulkanDevice& operator=(const VulkanDevice&) = delete;

  /** Brings up the loader, instance, device, allocator and capability set. */
  Result initialize(const DeviceConfiguration& configuration);

  // --- Identity and capabilities ----------------------------------------------------------------

  [[nodiscard]] const DeviceInfo& getInfo() const noexcept override { return mInfo; }
  [[nodiscard]] const Capabilities& getCapabilities() const noexcept override { return mCapabilities; }
  [[nodiscard]] DeviceMemoryStatistics memoryStatistics() const noexcept override {
    DeviceMemoryStatistics statistics;
    statistics.textureBytes = mResidentTextureBytes;
    statistics.bufferBytes = mResidentBufferBytes;
    return statistics;
  }
  [[nodiscard]] Queue* getQueue(QueueType type) noexcept override;

  // --- Command buffers --------------------------------------------------------------------------

  [[nodiscard]] Result createCommandBuffer(const CommandBufferDescriptor& descriptor,
          CommandBuffer** outCommandBuffer) override;
  void destroyCommandBuffer(CommandBuffer* commandBuffer) override;

  // --- Resources --------------------------------------------------------------------------------

  Result setHdrMetadata(SwapchainHandle swapchain, const HdrMetadata& metadata) override;

  Result createTexture(const TextureDescriptor& descriptor, TextureHandle* outTexture) override;
  void destroyTexture(TextureHandle texture) override;
  Result createTextureView(const TextureViewDescriptor& descriptor, TextureViewHandle* outView) override;
  void destroyTextureView(TextureViewHandle view) override;
  Result createSampler(const SamplerDescriptor& descriptor, TextureSamplerHandle* outSampler) override;
  void destroySampler(TextureSamplerHandle sampler) override;
  Result createBuffer(const BufferDescriptor& descriptor, BufferHandle* outBuffer) override;
  void destroyBuffer(BufferHandle buffer) override;
  Result createShaderModule(const ShaderModuleDescriptor& descriptor, ShaderModuleHandle* outModule) override;
  void destroyShaderModule(ShaderModuleHandle module) override;
  Result getShaderReflection(ShaderModuleHandle module, std::vector<ShaderBindingDeclaration>* outBindings) override;
  Result getShaderSourceInfo(ShaderModuleHandle module, ShaderSourceInfo* outInfo) override;
  Result getShaderSpirv(ShaderModuleHandle module, std::vector<uint32_t>* outWords) override;
  Result createDescriptorSetLayout(const DescriptorSetLayoutDescriptor& descriptor,
          DescriptorSetLayoutHandle* outLayout) override;
  void destroyDescriptorSetLayout(DescriptorSetLayoutHandle layout) override;
  Result createDescriptorSet(DescriptorSetLayoutHandle layout, DescriptorSetHandle* outSet) override;
  void destroyDescriptorSet(DescriptorSetHandle set) override;
  Result updateDescriptorSet(DescriptorSetHandle set, const DescriptorWrite* writes, uint32_t writeCount) override;
  Result createPipelineLayout(const PipelineLayoutDescriptor& descriptor, PipelineLayoutHandle* outLayout) override;
  void destroyPipelineLayout(PipelineLayoutHandle layout) override;
  Result createRenderPipeline(const RenderPipelineDescriptor& descriptor, RenderPipelineHandle* outPipeline) override;
  void destroyRenderPipeline(RenderPipelineHandle pipeline) override;
  bool createRenderPipelineAsync(const RenderPipelineDescriptor& descriptor, PipelineCompletion completion) override;
  void pollAsyncOperations() override;
  bool createShaderModuleAsync(const ShaderModuleDescriptor& descriptor, ShaderModuleCompletion completion) override;
  Result createComputePipeline(const ComputePipelineDescriptor& descriptor, ComputePipelineHandle* outPipeline) override;
  void destroyComputePipeline(ComputePipelineHandle pipeline) override;
  Result createRenderPass(const RenderPassDescriptor& descriptor, RenderPassHandle* outPass) override;
  void destroyRenderPass(RenderPassHandle renderPass) override;
  Result createFramebuffer(const FramebufferDescriptor& descriptor, FramebufferHandle* outFramebuffer) override;
  void destroyFramebuffer(FramebufferHandle framebuffer) override;
  Result createQuerySet(const QuerySetDescriptor& descriptor, QuerySetHandle* outQuerySet) override;
  void destroyQuerySet(QuerySetHandle querySet) override;
  Result createFence(const FenceDescriptor& descriptor, FenceHandle* outFence) override;
  void destroyFence(FenceHandle fence) override;
  Result createSemaphore(const SemaphoreDescriptor& descriptor, SemaphoreHandle* outSemaphore) override;
  void destroySemaphore(SemaphoreHandle semaphore) override;
  Result createSwapchain(const SwapchainDescriptor& descriptor, SwapchainHandle* outSwapchain) override;
  void destroySwapchain(SwapchainHandle swapchain) override;

  // --- Execution --------------------------------------------------------------------------------

  Result executeCommandBuffer(CommandBuffer& commandBuffer) override;

  // --- Buffer access ----------------------------------------------------------------------------

  Result mapBuffer(BufferHandle buffer, uint64_t offset, uint64_t size, void** outPointer) override;
  void unmapBuffer(BufferHandle buffer) override;

  // --- Queries ----------------------------------------------------------------------------------

  [[nodiscard]] Result getQuerySetCount(QuerySetHandle querySet, uint32_t* outCount) const override;
  Result getQueryResults(QuerySetHandle querySet, uint32_t firstQuery, uint32_t count, void* outData,
          size_t outDataSize) override;
  [[nodiscard]] bool isQueryResultAvailable(QuerySetHandle querySet, uint32_t query) const override;

  // --- Synchronisation --------------------------------------------------------------------------

  [[nodiscard]] Result getFenceStatus(FenceHandle fenceHandle) const override;
  Result waitForFence(FenceHandle fenceHandle, uint64_t timeoutNanoseconds) override;
  Result resetFence(FenceHandle fence) override;
  Result waitIdle() override;
  [[nodiscard]] bool isResourceReadyToDestroy(TextureHandle handle) const override;
  [[nodiscard]] bool isResourceReadyToDestroy(BufferHandle handle) const override;

  // --- Presentation -----------------------------------------------------------------------------

  Result createSurface(const SurfaceDescriptor& descriptor, SurfaceHandle* outSurface) override;
  void destroySurface(SurfaceHandle surface) override;
  Result getSwapchainInfo(SwapchainHandle swapchain, SwapchainInfo* outInfo) override;
  Result beginFrame(SwapchainHandle swapchain, SwapchainInfo* outInfo, SemaphoreHandle signalSemaphore) override;
  Result tryBeginFrame(SwapchainHandle swapchain, SwapchainInfo* outInfo) override;
  [[nodiscard]] bool supportsDeferredPresent() const noexcept override { return true; }

  /**
   * Acquires the next image, waiting at most @p timeoutNanoseconds.
   *
   * Both public entry points are this call with a different timeout: `beginFrame` waits indefinitely and
   * `tryBeginFrame` does not wait at all.
   */
  Result acquireFrame(SwapchainHandle swapchain, SwapchainInfo* outInfo, SemaphoreHandle signalSemaphore,
          uint64_t timeoutNanoseconds);
  Result endFrame(SwapchainHandle swapchain) override;
  Result endFrame(SwapchainHandle swapchain, SemaphoreHandle waitSemaphore) override;
  Result resizeSwapchain(SwapchainHandle swapchain, uint32_t width, uint32_t height) override;

  // --- Native interop ---------------------------------------------------------------------------

  [[nodiscard]] NativeTexture getNativeTexture(TextureHandle texture) const override;
  [[nodiscard]] NativeBuffer getNativeBuffer(BufferHandle buffer) const override;
  [[nodiscard]] Result importNativeTexture(const NativeTextureDescriptor& descriptor, TextureHandle* outTexture) override;
  [[nodiscard]] Result importNativeYuvTexture(const NativeTextureDescriptor& descriptor, TextureHandle* outTexture,
          YcbcrConversionDescriptor* outConversion) override;
  [[nodiscard]] bool supportsNativeTextureImport(NativeTextureKind kind) const override;
  [[nodiscard]] bool supportsYuvHardwareBufferImport() const override;

  // --- Diagnostics ------------------------------------------------------------------------------

  void setDebugMessageCallback(DebugMessageCallback callback, void* userData) override;
  void setTextureLabel(TextureHandle texture, const char* label) override;
  void setBufferLabel(BufferHandle buffer, const char* label) override;
  void beginDebugRegion(const char* label) override;
  void endDebugRegion() override;
  void appendDiagnostics(std::string& out) const override;

  [[nodiscard]] void* nativeInstance() const noexcept override {
    return reinterpret_cast<void*>(mInstance);
  }
  [[nodiscard]] void* nativePhysicalDevice() const noexcept override {
    return reinterpret_cast<void*>(mPhysicalDevice);
  }
  [[nodiscard]] void* nativeDevice() const noexcept override {
    return reinterpret_cast<void*>(mDevice);
  }
  [[nodiscard]] void* nativeQueue() const noexcept override {
    return reinterpret_cast<void*>(mGraphicsQueue);
  }
  [[nodiscard]] uint32_t nativeQueueFamily() const noexcept override {
    return mGraphicsQueueFamily;
  }

  // --- Backend internals used by the command buffer ---------------------------------------------

  [[nodiscard]] const VulkanApi& api() const noexcept { return *mApi; }
  [[nodiscard]] VkInstance instance() const noexcept { return mInstance; }
  [[nodiscard]] VkPhysicalDevice physicalDevice() const noexcept { return mPhysicalDevice; }
  [[nodiscard]] VkDevice device() const noexcept { return mDevice; }
  [[nodiscard]] VmaAllocator allocator() const noexcept { return mAllocator; }
  [[nodiscard]] VkQueue graphicsQueue() const noexcept { return mGraphicsQueue; }
  [[nodiscard]] uint32_t graphicsQueueFamily() const noexcept { return mGraphicsQueueFamily; }
  /** The queue family that presents to the swapchain; equal to the graphics family when it can present. */
  [[nodiscard]] uint32_t presentQueueFamily() const noexcept { return mPresentQueueFamily; }
  [[nodiscard]] VkPipelineLayout emptyPipelineLayout() const noexcept { return mEmptyPipelineLayout; }
  [[nodiscard]] VkDescriptorSetLayout emptyDescriptorSetLayout() const noexcept { return mEmptyDescriptorSetLayout; }

  [[nodiscard]] VulkanTexture* texture(TextureHandle handle) { return mTextures.get(handle); }
  [[nodiscard]] const VulkanTexture* texture(TextureHandle handle) const { return mTextures.get(handle); }
  [[nodiscard]] VulkanTextureView* textureView(TextureViewHandle handle) { return mTextureViews.get(handle); }
  [[nodiscard]] const VulkanTextureView* textureView(TextureViewHandle handle) const {
    return mTextureViews.get(handle);
  }
  [[nodiscard]] VulkanSampler* sampler(TextureSamplerHandle handle) { return mSamplers.get(handle); }
  [[nodiscard]] VulkanBuffer* buffer(BufferHandle handle) { return mBuffers.get(handle); }
  [[nodiscard]] const VulkanBuffer* buffer(BufferHandle handle) const { return mBuffers.get(handle); }
  [[nodiscard]] VulkanDescriptorSetLayout* descriptorSetLayout(DescriptorSetLayoutHandle handle) {
    return mDescriptorSetLayouts.get(handle);
  }
  [[nodiscard]] VulkanDescriptorSet* descriptorSet(DescriptorSetHandle handle) { return mDescriptorSets.get(handle); }
  [[nodiscard]] VulkanPipelineLayout* pipelineLayout(PipelineLayoutHandle handle) {
    return mPipelineLayouts.get(handle);
  }
  [[nodiscard]] VulkanRenderPipeline* renderPipeline(RenderPipelineHandle handle) {
    return mRenderPipelines.get(handle);
  }
  [[nodiscard]] VulkanComputePipeline* computePipeline(ComputePipelineHandle handle) {
    return mComputePipelines.get(handle);
  }
  [[nodiscard]] VulkanRenderPass* renderPass(RenderPassHandle handle) { return mRenderPasses.get(handle); }
  [[nodiscard]] VulkanQuerySet* querySet(QuerySetHandle handle) { return mQuerySets.get(handle); }
  [[nodiscard]] VulkanFence* fence(FenceHandle handle) { return mFences.get(handle); }
  [[nodiscard]] VulkanSemaphore* semaphore(SemaphoreHandle handle) { return mSemaphores.get(handle); }
  [[nodiscard]] VulkanSwapchain* swapchain(SwapchainHandle handle) { return mSwapchains.get(handle); }

  /**
   * @return true when this device uses `vkCmdBeginRendering` rather than a `VkRenderPass`/`VkFramebuffer`.
   *
   * The fallback is for Vulkan 1.0/1.1 devices, which are still common on Android. A dynamic-rendering
   * device keeps the cheaper path; a 1.1 device builds render passes and framebuffers per target.
   */
  [[nodiscard]] bool usesDynamicRendering() const noexcept {
    return mDynamicRenderingCore || mDynamicRenderingExtension;
  }

  /**
   * @return the view a `RenderPassBeginInfo` attachment names, resolving the default-framebuffer
   *         sentinel to the swapchain image acquired for this frame.
   */
  [[nodiscard]] bool resolveAttachmentView(TextureViewHandle handle, VulkanTextureView* outView);

  /**
   * @return the `VkFramebuffer` for a fallback pass, creating and caching it on first use.
   *
   * Only meaningful when `usesDynamicRendering()` is false. The key is the render pass, the exact views,
   * and the extent: the RHI begins a pass by naming views, not a framebuffer, so this is where the
   * object the API needs is derived.
   */
  [[nodiscard]] VkFramebuffer framebufferFor(VkRenderPass renderPass, const VkImageView* colors, uint32_t colorCount,
          VkImageView depth, uint32_t width, uint32_t height);

  /**
   * Records a transition of the default framebuffer's image into the layout rendering needs.
   *
   * @return false when no frame is open, which is a pass onto nothing.
   */
  Result acquireSwapchainTarget(SwapchainHandle swapchain, SwapchainTargetHandle* outTarget,
          SwapchainTargetInfo* outInfo) override;
  Result presentSwapchainTarget(SwapchainHandle swapchain, SwapchainTargetHandle target) override;

  [[nodiscard]] bool currentSwapchainImage(VkImage* outImage, VkImageView* outView, Extent2D* outExtent,
          VkFormat* outFormat);

  /** @return the image layout the current swapchain image is in, or UNDEFINED when there is none. */
  [[nodiscard]] VkImageLayout currentSwapchainLayout();

  /** Marks the current swapchain image as being in @p layout. Called by the command buffer's transitions. */
  void setCurrentSwapchainLayout(VkImageLayout layout);

  /** The 1x1 stand-ins a descriptor uses when a binding was never written. */
  [[nodiscard]] VkImageView fallbackTextureView() const noexcept { return mFallbackView; }
  [[nodiscard]] VkSampler fallbackSampler() const noexcept { return mFallbackSampler; }
  [[nodiscard]] VkBuffer fallbackBuffer() const noexcept { return mFallbackBuffer; }
  [[nodiscard]] VkDeviceSize fallbackBufferSize() const noexcept { return kFallbackBufferSize; }

  /** The index buffer a fan draw is translated through; filled once with the fixed fan pattern. */
  [[nodiscard]] VkBuffer fanIndexBuffer() const noexcept { return mFanIndexBuffer; }
  [[nodiscard]] uint32_t fanIndexCapacity() const noexcept { return kFanIndexCapacity; }

  /** Creates a VkCommandPool for one command buffer. */
  [[nodiscard]] Result createCommandPool(VkCommandPool* outPool) const;
  /** Creates a descriptor pool sized for one command buffer's snapshots. */
  [[nodiscard]] Result createDescriptorPool(VkDescriptorPool* outPool) const;

  /** Submits @p commandBuffer and waits for it. The synchronous path the RHI's execute uses. */
  Result submitAndWait(VkCommandBuffer commandBuffer, VkFence fence,
          VkSemaphore waitSemaphore = VK_NULL_HANDLE, VkSemaphore signalSemaphore = VK_NULL_HANDLE);

  /** Submits without waiting; the caller owns the fence. */
  Result submit(VkCommandBuffer commandBuffer, VkFence fence,
          VkSemaphore waitSemaphore = VK_NULL_HANDLE, VkSemaphore signalSemaphore = VK_NULL_HANDLE);

  /** Tracks asynchronous queue submissions so `isResourceReadyToDestroy` can be honest. */
  void noteAsyncSubmission() noexcept { mAsyncSubmissions.fetch_add(1, std::memory_order_relaxed); }
  void noteAsyncCompletion() noexcept {
    uint32_t current = mAsyncSubmissions.load(std::memory_order_relaxed);
    while (current != 0 &&
           !mAsyncSubmissions.compare_exchange_weak(current, current - 1, std::memory_order_relaxed)) {
    }
  }
  [[nodiscard]] bool hasAsyncSubmissions() const noexcept {
    return mAsyncSubmissions.load(std::memory_order_relaxed) != 0;
  }

  /**
   * Runs @p record in a one-shot command buffer and waits for it.
   *
   * Used for the work that has no recording scope of its own: a buffer's initial contents, an image's
   * first transition into `GENERAL`, and the swapchain's layout transition before a present.
   */
  Result oneShot(const char* label, const std::function<void(VkCommandBuffer)>& record);

  /** Emits a diagnostic through the installed callback, or the log when there is none. */
  void emitMessage(MessageSeverity severity, const char* message) const;

  /** Emits and formats a message. */
  void emitMessage(MessageSeverity severity, const std::string& message) const {
    emitMessage(severity, message.c_str());
  }

  [[nodiscard]] Result createBufferInternal(const BufferDescriptor& descriptor, VulkanBuffer* outBuffer);
  [[nodiscard]] Result createImageInternal(const TextureDescriptor& descriptor, VulkanTexture* outTexture);

  /** @return the push-constant budget this device reports and its pipeline layouts declare. */
  [[nodiscard]] uint32_t pushConstantBudget() const noexcept { return mPushConstantBudget; }

  /** True when validation was requested and the debug-utils extension is available. */
  [[nodiscard]] bool validationEnabled() const noexcept { return mDebugMessenger != VK_NULL_HANDLE; }

  private:
  Result createInstance(const DeviceConfiguration& configuration);
  Result pickPhysicalDevice(const DeviceConfiguration& configuration);
  Result createLogicalDevice();
  Result createAllocator();
  void probeCapabilities();
  void createFallbackResources();
  void destroyFallbackResources();
  Result createSwapchainObjects(const SwapchainDescriptor& descriptor, VulkanSwapchain* swapchain);
  void destroySwapchainObjects(VulkanSwapchain* swapchain);

  /**
   * Creates a `VkImage` over memory a platform producer owns, and fills @p outTexture with it.
   *
   * The result is *not* transitioned into `GENERAL` the way `createImageInternal` does for an image the
   * device allocates: the producer wrote the contents already, and a transition from `UNDEFINED` is
   * allowed to discard them. Every other use of a texture in this backend assumes `GENERAL`, and an
   * imported image is created straight into that layout instead.
   *
   * On failure nothing is left allocated: the image, the memory and the producer's reference are all
   * released through the error paths, so a caller only has to handle the `Result`.
   */
  Result createExternalImage(const NativeTextureDescriptor& descriptor, VulkanTexture* outTexture);

  /**
   * Creates the view of an imported image and claims the GENERAL layout every consumer samples it in.
   *
   * Shared by the platform import paths because the tail is identical: an imported texture is a single
   * level 2D image whose storage already holds the producer's picture, so the view and the ownership
   * barrier do not depend on where the memory came from.
   */
  Result completeImportedImage(VulkanTexture* texture,
          const std::optional<YcbcrConversionDescriptor>& ycbcrDescription = std::nullopt);

  /**
   * @return the cached conversion for @p descriptor, creating it on first use.
   *
   * VK_NULL_HANDLE means this device cannot convert YUV: a Vulkan 1.0 device without
   * `VK_KHR_sampler_ycbcr_conversion`, or a driver that refused the format.
   */
  VkSamplerYcbcrConversion ycbcrConversionFor(const YcbcrConversionDescriptor& descriptor);

#if defined(__ANDROID__)
  /** Creates a VkImage over an AHardwareBuffer's memory (Android import). */
  Result createExternalHardwareBufferImage(const NativeTextureDescriptor& descriptor,
          VulkanTexture* outTexture, YcbcrConversionDescriptor* outConversion);
#endif

  /**
   * @return the memory type index satisfying both @p imageRequirements and @p compatibleMemoryTypes, or
   *         a value >= `VK_MAX_MEMORY_TYPES` when the two have no type in common.
   *
   * Device-local is preferred because an imported video frame is sampled, not read on the host; the
   * first compatible type is the fallback for a producer whose handle is pinned to host-visible memory.
   */
  [[nodiscard]] uint32_t findImportedMemoryType(uint32_t compatibleMemoryTypes,
          const VkMemoryRequirements& imageRequirements) const;

  void releaseFramebuffersFor(VkRenderPass renderPass);
  void releaseFramebuffersUsing(VkImageView view);
  void releaseFramebuffers();

  Result captureDebugMessenger();

  static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
          VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT* data, void* userData);

  std::unique_ptr<VulkanApi> mApi;
  VkInstance mInstance = VK_NULL_HANDLE;
  VkPhysicalDevice mPhysicalDevice = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties mProperties{};
  VkPhysicalDeviceMemoryProperties mMemoryProperties{};
  VkDevice mDevice = VK_NULL_HANDLE;
  VkPipelineCache mPipelineCache = VK_NULL_HANDLE;
  std::string mPipelineCachePath;
  std::mutex mPipelineCacheMutex;

  struct AsyncPipelineRequest {
    RenderPipelineDescriptor descriptor{};
    std::string label;
    PipelineCompletion completion;
  };
  struct AsyncPipelineResult {
    VulkanRenderPipeline pipeline;
    Result result = Result::Error;
    std::string error;
    PipelineCompletion completion;
  };
  Result buildRenderPipeline(const RenderPipelineDescriptor& descriptor, VulkanRenderPipeline& outPipeline);
  Result buildShaderModule(const ShaderModuleDescriptor& descriptor, VulkanShaderModule& outModule);
  void pipelineWorkerMain();
  void stopPipelineWorker();

  struct AsyncShaderRequest {
    ShaderModuleDescriptor descriptor{};
    std::vector<uint8_t> code;
    std::string label;
    std::string entryPoint;
    ShaderModuleCompletion completion;
  };
  struct AsyncShaderResult {
    VulkanShaderModule module;
    Result result = Result::Error;
    std::string error;
    ShaderModuleCompletion completion;
  };

  std::thread mPipelineWorker;
  std::mutex mPipelineQueueMutex;
  std::condition_variable mPipelineQueueCondition;
  std::deque<AsyncPipelineRequest> mPipelineRequests;
  std::deque<AsyncPipelineResult> mPipelineResults;
  std::deque<AsyncShaderRequest> mShaderModuleRequests;
  std::deque<AsyncShaderResult> mShaderModuleResults;
  bool mPipelineWorkerStop = false;
  VkQueue mGraphicsQueue = VK_NULL_HANDLE;
  uint32_t mGraphicsQueueFamily = 0;
  /** The present queue may be a different family from the graphics queue on a device that partitions them. */
  VkQueue mPresentQueue = VK_NULL_HANDLE;
  uint32_t mPresentQueueFamily = 0;
  /** A temporary surface used only to ask `vkGetPhysicalDeviceSurfaceSupportKHR` before the device exists. */
  VkSurfaceKHR mProbeSurface = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT mDebugMessenger = VK_NULL_HANDLE;
  VmaAllocator mAllocator = nullptr;

  VkPipelineLayout mEmptyPipelineLayout = VK_NULL_HANDLE;
  VkDescriptorSetLayout mEmptyDescriptorSetLayout = VK_NULL_HANDLE;

  VkImageView mFallbackView = VK_NULL_HANDLE;
  VkImage mFallbackImage = VK_NULL_HANDLE;
  VmaAllocation mFallbackAllocation = nullptr;
  VkSampler mFallbackSampler = VK_NULL_HANDLE;
  VkBuffer mFallbackBuffer = VK_NULL_HANDLE;
  VmaAllocation mFallbackBufferAllocation = nullptr;
  VkBuffer mFanIndexBuffer = VK_NULL_HANDLE;
  VmaAllocation mFanIndexAllocation = nullptr;

  static constexpr uint32_t kFanIndexCapacity = 1u << 16;
  static constexpr VkDeviceSize kFallbackBufferSize = 1024;

  /** The largest push-constant range any pipeline layout declares; also `Limits::maxPushConstantSize`. */
  uint32_t mPushConstantBudget = 128;

  bool mDynamicRenderingCore = false;
  bool mDynamicRenderingExtension = false;
  bool mHasSwapchainExtension = false;
  /**
   * `VK_KHR_external_memory_win32` is enabled, so a Direct3D 11 NT handle can be imported as an image.
   * Windows only; always false elsewhere.
   */
  bool mHasExternalMemoryWin32 = false;
  /**
   * VK_ANDROID_external_memory_android_hardware_buffer is enabled, so an AHardwareBuffer can be imported.
   * Android only; always false elsewhere.
   */
  bool mHasExternalMemoryAndroidHardwareBuffer = false;
  /**
   * `VkSamplerYcbcrConversion` objects, one per descriptor, kept for the life of the device.
   *
   * A conversion is immutable and Vulkan compares conversions *by object*, so the view of a YUV image and
   * every sampler that reads it have to be handed the same one; caching by description is what makes that
   * possible without the caller owning the handle.
   */
  std::vector<std::pair<YcbcrConversionDescriptor, VkSamplerYcbcrConversion>> mYcbcrConversions;
  bool mHasDebugUtils = false;
  bool mHasValidationLayer = false;
  std::vector<std::string> mDeviceExtensions;
  uint32_t mDeviceApiVersion = 0;
  std::string mValidationLayerName;
  bool mInitialized = false;
  uint32_t mMaxFramesInFlight = 1;
  std::atomic<uint32_t> mAsyncSubmissions{ 0 };

  DeviceInfo mInfo{};
  Capabilities mCapabilities{};

  std::string mVendor;
  std::string mDeviceName;
  std::string mApiVersion;
  std::string mDriverVersion;
  std::string mBackendName;
  /** The window system this device was created for, used only to name the backend in diagnostics. */
  NativeWindowSystem mNativeWindowSystem = NativeWindowSystem::Unknown;
  std::string mCachedDiagnostics;

  std::atomic<DebugMessageCallback> mDebugCallback{ nullptr };
  std::atomic<void*> mDebugUserData{ nullptr };

  /** Serialises submits and one-shot recordings; a VkQueue is not externally synchronised by Vulkan. */
  std::mutex mQueueMutex;
  std::mutex mOneShotMutex;

  /** `vkSetHdrMetadataEXT`, null when the device did not enable VK_EXT_hdr_metadata. */
  PFN_vkSetHdrMetadataEXT mSetHdrMetadata = nullptr;

  HandlePool<TextureTag, VulkanTexture> mTextures;
  HandlePool<TextureViewTag, VulkanTextureView> mTextureViews;
  HandlePool<TextureSamplerTag, VulkanSampler> mSamplers;
  HandlePool<BufferTag, VulkanBuffer> mBuffers;
  /** Live device memory, counted at the texture and buffer tables; see `Device::memoryStatistics`. */
  uint64_t mResidentTextureBytes = 0;
  uint64_t mResidentBufferBytes = 0;
  HandlePool<ShaderModuleTag, VulkanShaderModule> mShaderModules;
  HandlePool<DescriptorSetLayoutTag, VulkanDescriptorSetLayout> mDescriptorSetLayouts;
  HandlePool<DescriptorSetTag, VulkanDescriptorSet> mDescriptorSets;
  HandlePool<PipelineLayoutTag, VulkanPipelineLayout> mPipelineLayouts;
  HandlePool<RenderPipelineTag, VulkanRenderPipeline> mRenderPipelines;
  HandlePool<ComputePipelineTag, VulkanComputePipeline> mComputePipelines;
  HandlePool<RenderPassTag, VulkanRenderPass> mRenderPasses;
  HandlePool<SwapchainTargetTag, VulkanSwapchainTarget> mSwapchainTargets;
  HandlePool<FramebufferTag, VulkanFramebuffer> mFramebuffers;
  /** Fallback framebuffers, keyed by the pass, views and extent a `beginRenderPass` named. */
  std::map<VulkanFramebufferKey, VkFramebuffer> mFallbackFramebuffers;
  /** Command buffers can be recorded on more than one thread; the cache above is shared. */
  mutable std::mutex mFramebufferMutex;
  HandlePool<QuerySetTag, VulkanQuerySet> mQuerySets;
  HandlePool<FenceTag, VulkanFence> mFences;
  HandlePool<SemaphoreTag, VulkanSemaphore> mSemaphores;
  HandlePool<SurfaceTag, VulkanSurface> mSurfaces;
  HandlePool<SwapchainTag, VulkanSwapchain> mSwapchains;

  VulkanQueue mGraphicsQueueObject{ *this, VK_NULL_HANDLE, QueueType::Graphics };};

} // namespace backend::vulkan

#endif // CORAL_BACKEND_VULKAN_DEVICE_H

