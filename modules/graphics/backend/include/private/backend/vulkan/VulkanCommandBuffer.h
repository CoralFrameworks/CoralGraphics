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

#ifndef CORAL_BACKEND_VULKAN_COMMAND_BUFFER_H
#define CORAL_BACKEND_VULKAN_COMMAND_BUFFER_H

/**
 * @file VulkanCommandBuffer.h
 *
 * The command buffer, recording straight into a `VkCommandBuffer`.
 *
 * ## Why this one records eagerly when the OpenGL backend defers
 *
 * The OpenGL backend has to defer: its API executes immediately, so deferring is the only way to give
 * recording and submission separate lifetimes. Vulkan's API *is* deferred, so the natural translation
 * records each RHI call into the driver's command buffer at the moment it is made. That is also the
 * only translation that satisfies Vulkan's own rules: a descriptor set that is updated and then used by
 * a second draw before submission cannot be re-bound later, so the contents have to be captured at the
 * draw. The two places where recording is not a direct call are consequences, not designs:
 *
 *  - **Descriptor sets** are snapshotted at the draw that binds them (see `snapshotDescriptors`). The
 *    RHI's `updateDescriptorSet` is a CPU-side write, so a set that is refilled between draws must not
 *    make the earlier draw change retroactively. One descriptor set per (set, version) is allocated
 *    from this command buffer's pool and cached; the pool is reset when the buffer is reused, which is
 *    safe because a reset waits for the previous submission before reusing the arena.
 *  - **Uploads** are staged. `writeTexture` and `writeBuffer` carry a host pointer, and a `VkBuffer`
 *    copy needs memory that stays valid until the GPU runs, so each command buffer owns a growable
 *    host-visible staging arena that lives until it is reset.
 *
 * ## Render passes
 *
 * `beginRenderPass` issues `vkCmdBeginRendering` with the attachments and load/store operations the RHI
 * supplied. The `RenderPassHandle` is a compatibility key, exactly as the RHI documents, and is used to
 * validate attachment shapes rather than to create a driver object.
 */

#include <backend/Device.h>
#include <backend/Types.h>

#include "private/backend/vulkan/VulkanDevice.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace backend::vulkan {

class VulkanCommandBuffer final : public CommandBuffer {
  public:
  VulkanCommandBuffer(VulkanDevice& device, const CommandBufferDescriptor& descriptor);
  ~VulkanCommandBuffer() override;

  VulkanCommandBuffer(const VulkanCommandBuffer&) = delete;
  VulkanCommandBuffer& operator=(const VulkanCommandBuffer&) = delete;

  // --- Lifecycle --------------------------------------------------------------------------------

  /** Creates the command pool and the command buffer. Called by the device's createCommandBuffer. */
  Result initialize();

  /** Submits the finished recording and waits for it. Called by Device::executeCommandBuffer. */
  Result submitAndWait();

  Result begin() override;
  Result end() override;
  Result reset() override;
  [[nodiscard]] bool isRecording() const noexcept override { return mRecording; }
  void dumpCommandCounts() const override;

  // --- Render pass scope ------------------------------------------------------------------------

  Result beginRenderPass(const RenderPassBeginInfo& info) override;
  Result endRenderPass() override;

  // --- Pipeline state ---------------------------------------------------------------------------

  Result bindRenderPipeline(RenderPipelineHandle pipeline) override;
  Result bindComputePipeline(ComputePipelineHandle pipeline) override;
  Result setVertexBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset) override;
  Result setIndexBuffer(BufferHandle buffer, IndexFormat format, uint64_t offset) override;
  Result bindDescriptorSet(DescriptorSetIndex setIndex, DescriptorSetHandle set) override;
  Result setViewport(const Viewport& viewport) override;
  Result setScissor(const ScissorRect& scissor) override;
  Result setStencilReference(uint32_t reference) override;
  Result setBlendConstant(const Color& color) override;
  Result pushConstants(const void* data, uint32_t size, uint32_t offset) override;

  // --- Drawing ----------------------------------------------------------------------------------

  Result draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override;
  Result drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset,
          uint32_t firstInstance) override;
  Result drawIndirect(BufferHandle arguments, uint64_t offset) override;
  Result drawIndexedIndirect(BufferHandle arguments, uint64_t offset) override;
  Result dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;
  Result dispatchIndirect(BufferHandle arguments, uint64_t offset) override;

  Result insertDebugMarker(const char* label) override;
  Result beginDebugMarker(const char* label) override;
  Result endDebugMarker() override;

  // --- Transfer ---------------------------------------------------------------------------------

  Result writeBuffer(BufferHandle buffer, uint64_t offset, const void* data, size_t dataSize) override;
  Result copyBufferToBuffer(BufferHandle src, uint64_t srcOffset, BufferHandle dst, uint64_t dstOffset,
          uint64_t size) override;
  Result writeTexture(TextureHandle texture, const TextureCopyRegion& region, const void* data, uint64_t dataSize,
          uint32_t bytesPerRow) override;
  Result copyBufferToTexture(const BufferTextureCopy& copy) override;
  Result copyTextureToBuffer(const BufferTextureCopy& copy) override;
  Result copyTextureToTexture(TextureHandle src, TextureHandle dst, const TextureCopyRegion& region) override;
  Result clearColorTexture(TextureHandle texture, const TextureSubresourceRange& range, const Color& color) override;
  Result clearDepthStencilTexture(TextureHandle texture, const TextureSubresourceRange& range,
          const DepthStencilValue& value) override;
  Result pipelineBarrier() override;

  // --- Queries ----------------------------------------------------------------------------------

  Result resetQuerySet(QuerySetHandle querySet, uint32_t firstQuery, uint32_t count) override;
  Result beginQuery(QuerySetHandle querySet, uint32_t query) override;
  Result endQuery(QuerySetHandle querySet, uint32_t query) override;

  void setLabel(const char* label) override;

  // --- Backend internals ------------------------------------------------------------------------

  /**
   * Marks the buffer as submitted under @p fence.
   *
   * A synchronous submission clears this immediately; an asynchronous `Queue::submit` with a fence
   * keeps it until `begin`/`reset` waits. `asynchronous` makes completion release the device's
   * in-flight budget used by `isResourceReadyToDestroy`.
   */
  void markSubmitted(VkFence fence, const bool asynchronous = false) noexcept {
    mInFlight = true;
    mSubmitFence = fence;
    mAsyncInFlight = asynchronous;
  }

  void markComplete() noexcept {
    if (mAsyncInFlight) mDevice.noteAsyncCompletion();
    mInFlight = false;
    mAsyncInFlight = false;
    mSubmitFence = VK_NULL_HANDLE;
  }

  [[nodiscard]] bool isInFlight() const noexcept { return mInFlight; }
  [[nodiscard]] VkCommandBuffer handle() const noexcept { return mCommandBuffer; }
  [[nodiscard]] const char* label() const noexcept { return mLabel.c_str(); }

  /** True when `end()` has succeeded and the recorded commands are ready to submit. */
  [[nodiscard]] bool isReady() const noexcept { return mEnded && !mRecording; }

  private:
  struct StagingBlock {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    uint8_t* mapped = nullptr;
    VkDeviceSize capacity = 0;
    VkDeviceSize offset = 0;
    bool coherent = false;
  };

  /**
   * A resolve recorded by the Vulkan 1.0/1.1 fallback.
   *
   * The fallback cannot use a resolve attachment because `RenderPassDescriptor` describes only the
   * multisampled attachment; it ends the pass and resolves with `vkCmdResolveImage` after it. The source
   * and destination images are stored rather than only the region because the command is recorded later.
   */
  struct PendingResolve {
    VkImage source = VK_NULL_HANDLE;
    VkImage destination = VK_NULL_HANDLE;
    VkImageResolve region{};
  };

  /** The Vulkan 1.0/1.1 path: begin a `VkRenderPass` and clear the attachments that asked for it. */
  Result beginRenderPassFallback(const RenderPassBeginInfo& info);

  /** @return Result::Error when no render pass is open, since drawing outside one is undefined. */
  [[nodiscard]] Result requireRenderPass(const char* operation) const;  [[nodiscard]] Result requirePipeline(const char* operation) const;
  [[nodiscard]] Result requireTexture(TextureHandle handle, const char* operation) const;
  [[nodiscard]] Result requireBuffer(BufferHandle handle, const char* operation) const;

  /** Stages @p data and returns the buffer and offset it landed at. */
  Result stage(const void* data, size_t size, VkDeviceSize alignment, VkBuffer* outBuffer, VkDeviceSize* outOffset);

  /** Frees the staging blocks and resets the descriptor pools. Legal only when nothing is in flight. */
  void releaseTransientResources();

  /** Records a full memory dependency from @p src to @p dst. Layouts stay GENERAL; only memory moves. */
  void memoryBarrier(VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage, VkAccessFlags srcAccess,
          VkAccessFlags dstAccess);

  /** Transitions the default-framebuffer image if it is not already in GENERAL. */
  Result prepareSwapchainImage();

  /** Binds the descriptor sets pending at this draw, snapshotting each one first. */
  Result applyDescriptorSets();

  /** @return a descriptor set whose contents are the RHI set's values at the moment of this call. */
  Result snapshotDescriptorSet(DescriptorSetHandle set, VkDescriptorSet* outSet);

  /** @return an empty set for a pipeline-layout slot the caller did not bind. */
  Result emptySet(uint32_t index, VkDescriptorSet* outSet);

  /** Binds mIndexBuffer again after a fan draw replaced it with the device's fan index buffer. */
  void rebindIndexBuffer();

  VulkanDevice& mDevice;
  CommandBufferDescriptor mDescriptor{};
  std::string mLabel;

  VkCommandPool mCommandPool = VK_NULL_HANDLE;
  VkCommandBuffer mCommandBuffer = VK_NULL_HANDLE;
  std::vector<VkDescriptorPool> mDescriptorPools;
  std::array<PendingResolve, kMaxColorAttachments> mPendingResolves{};
  uint32_t mPendingResolveCount = 0;

  bool mRecording = false;
  bool mEnded = false;
  bool mRenderPassOpen = false;
  bool mInFlight = false;
  bool mAsyncInFlight = false;
  VkFence mSubmitFence = VK_NULL_HANDLE;

  // --- Pass state -------------------------------------------------------------------------------

  uint32_t mRenderAreaWidth = 0;
  uint32_t mRenderAreaHeight = 0;
  uint32_t mColorAttachmentCount = 0;
  bool mRenderingToSwapchain = false;

  // --- Pipeline and binding state ---------------------------------------------------------------

  RenderPipelineHandle mBoundRenderPipeline = RenderPipelineHandle::invalid();
  ComputePipelineHandle mBoundComputePipeline = ComputePipelineHandle::invalid();
  PipelineLayoutHandle mBoundPipelineLayout = PipelineLayoutHandle::invalid();

  struct PendingSet {
    uint32_t index = 0;
    DescriptorSetHandle set = DescriptorSetHandle::invalid();
    bool occupied = false;
  };
  std::array<PendingSet, kMaxDescriptorSetLayouts> mPendingSets{};

  BufferHandle mIndexBuffer = BufferHandle::invalid();
  IndexFormat mIndexFormat = IndexFormat::UInt32;
  uint64_t mIndexOffset = 0;
  bool mIndexBufferBound = false;
  /** True when the *driver* has the fan index buffer bound rather than mIndexBuffer. */
  bool mFanIndexBound = false;

  struct SnapshotKey {
    uint64_t set = 0;
    uint64_t version = 0;

    friend bool operator<(const SnapshotKey& a, const SnapshotKey& b) noexcept {
      return a.set < b.set || (a.set == b.set && a.version < b.version);
    }
  };
  std::map<SnapshotKey, VkDescriptorSet> mDescriptorSnapshots;
  std::array<VkDescriptorSet, kMaxDescriptorSetLayouts> mEmptySets{};
  std::array<bool, kMaxDescriptorSetLayouts> mEmptySetReady{};

  std::vector<StagingBlock> mStaging;

  // --- Diagnostics ------------------------------------------------------------------------------

  struct Counts {
    uint64_t draws = 0;
    uint64_t drawsIndexed = 0;
    uint64_t drawsIndirect = 0;
    uint64_t dispatches = 0;
    uint64_t copies = 0;
    uint64_t barriers = 0;
  };
  mutable Counts mCounts{};
};

} // namespace backend::vulkan

#endif // CORAL_BACKEND_VULKAN_COMMAND_BUFFER_H
