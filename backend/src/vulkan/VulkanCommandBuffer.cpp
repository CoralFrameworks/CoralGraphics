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

#include "private/backend/vulkan/VulkanCommandBuffer.h"

#include "private/backend/vulkan/VulkanEnums.h"
#include "private/backend/vulkan/VulkanEntryPoints.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace backend::vulkan {
namespace {

Result fromVk(const VkResult result) noexcept {
  switch (result) {
    case VK_SUCCESS:
      return Result::Success;
    case VK_NOT_READY:
      return Result::NotReady;
    case VK_TIMEOUT:
      return Result::Timeout;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
      return Result::OutOfMemory;
    case VK_ERROR_DEVICE_LOST:
      return Result::DeviceLost;
    default:
      return Result::Error;
  }
}

// Host access bits are deliberately absent: HOST_READ/HOST_WRITE are not supported by the
// graphics/compute/transfer stage masks these barriers use, and host visibility is ordered by
// `vkFlushMappedMemoryRanges` plus the fence the synchronous submit already waits for.
VkAccessFlags kAllWrites = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_TRANSFER_WRITE_BIT;

VkAccessFlags kAllReads = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
                          VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                          VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                          VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

VkDeviceSize alignUp(const VkDeviceSize value, const VkDeviceSize alignment) noexcept {
  if (alignment <= 1) return value;
  return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace anonymous

// ==================================================================================================
// Lifecycle
// ==================================================================================================

VulkanCommandBuffer::VulkanCommandBuffer(VulkanDevice& device, const CommandBufferDescriptor& descriptor)
        : mDevice(device), mDescriptor(descriptor) {
  if (descriptor.label != nullptr) mLabel = descriptor.label;
  mEmptySetReady.fill(false);
  mEmptySets.fill(VK_NULL_HANDLE);
}

VulkanCommandBuffer::~VulkanCommandBuffer() {
  if (mInFlight && mSubmitFence != VK_NULL_HANDLE) {
    mDevice.api().vkWaitForFences(mDevice.device(), 1, &mSubmitFence, VK_TRUE, UINT64_MAX);
  }
  if (mInFlight) markComplete();
  releaseTransientResources();
  for (VkDescriptorPool pool: mDescriptorPools) {
    if (pool != VK_NULL_HANDLE) mDevice.api().vkDestroyDescriptorPool(mDevice.device(), pool, nullptr);
  }
  mDescriptorPools.clear();
  if (mCommandPool != VK_NULL_HANDLE) {
    mDevice.api().vkDestroyCommandPool(mDevice.device(), mCommandPool, nullptr);
    mCommandPool = VK_NULL_HANDLE;
  }
}

Result VulkanCommandBuffer::initialize() {
  if (const Result pool = mDevice.createCommandPool(&mCommandPool); !isSuccess(pool)) {
    return pool;
  }

  VkCommandBufferAllocateInfo allocate{};
  allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocate.commandPool = mCommandPool;
  allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate.commandBufferCount = 1;
  if (const VkResult result = mDevice.api().vkAllocateCommandBuffers(mDevice.device(), &allocate, &mCommandBuffer);
      result != VK_SUCCESS) {
    return fromVk(result);
  }
  return Result::Success;
}

void VulkanCommandBuffer::releaseTransientResources() {
  for (StagingBlock& block: mStaging) {
    if (block.buffer != VK_NULL_HANDLE) {
      vmaDestroyBuffer(mDevice.allocator(), block.buffer, block.allocation);
    }
  }
  mStaging.clear();

  for (VkDescriptorPool pool: mDescriptorPools) {
    if (pool != VK_NULL_HANDLE) {
      mDevice.api().vkResetDescriptorPool(mDevice.device(), pool, 0);
    }
  }
  mDescriptorSnapshots.clear();
  mEmptySets.fill(VK_NULL_HANDLE);
  mEmptySetReady.fill(false);
}

Result VulkanCommandBuffer::begin() {
  if (mRecording) {
    return Result::Error;
  }
  if (mInFlight) {
    if (mSubmitFence != VK_NULL_HANDLE) {
      mDevice.api().vkWaitForFences(mDevice.device(), 1, &mSubmitFence, VK_TRUE, UINT64_MAX);
    }
    markComplete();
  }

  if (const VkResult reset = mDevice.api().vkResetCommandPool(mDevice.device(), mCommandPool, 0); reset != VK_SUCCESS) {
    return fromVk(reset);
  }
  releaseTransientResources();

  VkCommandBufferBeginInfo info{};
  info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  if (mDescriptor.transient) info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (const VkResult started = mDevice.api().vkBeginCommandBuffer(mCommandBuffer, &info); started != VK_SUCCESS) {
    return fromVk(started);
  }

  // Command-buffer state is not inherited from the previous recording: every dynamic state the pipeline
  // declares dynamic has to be set again before a draw, and the fields below are what make a missing
  // call visible as a validation message rather than as stale state from the last frame.
  mRecording = true;
  mEnded = false;
  mRenderPassOpen = false;
  mBoundRenderPipeline = RenderPipelineHandle::invalid();
  mBoundComputePipeline = ComputePipelineHandle::invalid();
  mBoundPipelineLayout = PipelineLayoutHandle::invalid();
  for (PendingSet& pending: mPendingSets) pending = PendingSet{};
  mIndexBuffer = BufferHandle::invalid();
  mIndexBufferBound = false;
  mFanIndexBound = false;
  mRenderAreaWidth = 0;
  mRenderAreaHeight = 0;
  mColorAttachmentCount = 0;
  mRenderingToSwapchain = false;
  mPendingResolveCount = 0;
  mCounts = Counts{};

  if (mDevice.validationEnabled() && mDevice.api().vkSetDebugUtilsObjectNameEXT != nullptr && !mLabel.empty()) {
    VkDebugUtilsObjectNameInfoEXT name{};
    name.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    name.objectType = VK_OBJECT_TYPE_COMMAND_BUFFER;
    name.objectHandle = reinterpret_cast<uint64_t>(mCommandBuffer);
    name.pObjectName = mLabel.c_str();
    mDevice.api().vkSetDebugUtilsObjectNameEXT(mDevice.device(), &name);
  }
  return Result::Success;
}

Result VulkanCommandBuffer::end() {
  if (!mRecording) return Result::Error;
  if (mRenderPassOpen) {
    mDevice.emitMessage(MessageSeverity::Error, "a command buffer cannot end with a render pass still open");
    return Result::Error;
  }
  if (const VkResult ended = mDevice.api().vkEndCommandBuffer(mCommandBuffer); ended != VK_SUCCESS) {
    return fromVk(ended);
  }
  mRecording = false;
  mEnded = true;
  return Result::Success;
}

Result VulkanCommandBuffer::reset() {
  // A recording buffer is *discarded*, not refused. `RenderCore::abortRecording` resets a frame that
  // failed half-way through recording, and refusing left `mRecording` set: `begin()` then returned
  // `Error` for that buffer for the rest of the session, so after one failed frame per slot the whole
  // command-buffer ring was unusable and every later frame reported "no command buffer could be
  // acquired" (a preview that went black and never came back). Vulkan allows a command pool to be reset
  // with a command buffer still in the recording state; `begin()` resets it on the next recording.
  if (mInFlight) {
    if (mSubmitFence != VK_NULL_HANDLE) {
      mDevice.api().vkWaitForFences(mDevice.device(), 1, &mSubmitFence, VK_TRUE, UINT64_MAX);
    }
    markComplete();
  }
  mRecording = false;
  releaseTransientResources();
  mEnded = false;
  mRenderPassOpen = false;
  mPendingResolveCount = 0;
  return Result::Success;
}

Result VulkanCommandBuffer::submitAndWait() {
  if (!mEnded || mInFlight) return Result::Error;
  const Result submitted = mDevice.submitAndWait(mCommandBuffer, VK_NULL_HANDLE);
  markComplete();
  return submitted;
}

void VulkanCommandBuffer::dumpCommandCounts() const {
  std::printf("[vulkan] command buffer '%s': draws=%llu indexed=%llu indirect=%llu dispatches=%llu copies=%llu "
              "barriers=%llu\n",
          mLabel.c_str(), static_cast<unsigned long long>(mCounts.draws),
          static_cast<unsigned long long>(mCounts.drawsIndexed),
          static_cast<unsigned long long>(mCounts.drawsIndirect),
          static_cast<unsigned long long>(mCounts.dispatches), static_cast<unsigned long long>(mCounts.copies),
          static_cast<unsigned long long>(mCounts.barriers));
  std::fflush(stdout);
}

// ==================================================================================================
// Validation helpers
// ==================================================================================================

Result VulkanCommandBuffer::requireRenderPass(const char* operation) const {
  if (mRenderPassOpen) return Result::Success;
  (void) operation;
  return Result::Error;
}

Result VulkanCommandBuffer::requirePipeline(const char* operation) const {
  if (mBoundRenderPipeline.valid() || mBoundComputePipeline.valid()) return Result::Success;
  (void) operation;
  return Result::Error;
}

Result VulkanCommandBuffer::requireTexture(const TextureHandle handle, const char* operation) const {
  if (mDevice.texture(handle) != nullptr) return Result::Success;
  (void) operation;
  return Result::InvalidHandle;
}

Result VulkanCommandBuffer::requireBuffer(const BufferHandle handle, const char* operation) const {
  if (mDevice.buffer(handle) != nullptr) return Result::Success;
  (void) operation;
  return Result::InvalidHandle;
}

// ==================================================================================================
// Barriers and staging
// ==================================================================================================

void VulkanCommandBuffer::memoryBarrier(const VkPipelineStageFlags srcStage, const VkPipelineStageFlags dstStage,
        const VkAccessFlags srcAccess, const VkAccessFlags dstAccess) {
  VkMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = srcAccess;
  barrier.dstAccessMask = dstAccess;
  mDevice.api().vkCmdPipelineBarrier(mCommandBuffer, srcStage, dstStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
  ++mCounts.barriers;
}

Result VulkanCommandBuffer::stage(const void* data, const size_t size, const VkDeviceSize alignment,
        VkBuffer* outBuffer, VkDeviceSize* outOffset) {
  if (data == nullptr || size == 0 || outBuffer == nullptr || outOffset == nullptr) {
    return Result::InvalidArgument;
  }

  for (StagingBlock& block: mStaging) {
    const VkDeviceSize aligned = alignUp(block.offset, alignment);
    if (aligned + size > block.capacity) continue;
    std::memcpy(block.mapped + aligned, data, size);
    if (!block.coherent) {
      vmaFlushAllocation(mDevice.allocator(), block.allocation, aligned, size);
    }
    block.offset = aligned + size;
    *outBuffer = block.buffer;
    *outOffset = aligned;
    return Result::Success;
  }

  // ## One arena per command buffer, grown rather than allocated per upload
  //
  // The canvas writes a uniform block per draw and a vertex arena per batch, so an upload per draw is
  // the normal case. A VkBuffer allocation per upload would put the allocator on the hot path; a
  // 1 MiB block that is filled and replaced costs one allocation per megabyte instead.
  StagingBlock block;
  block.capacity = std::max<VkDeviceSize>(size, 1024u * 1024u);
  block.coherent = false;

  VkBufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.size = block.capacity;
  info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VmaAllocationCreateInfo allocation{};
  allocation.usage = VMA_MEMORY_USAGE_AUTO;
  allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

  VmaAllocationInfo allocationInfo{};
  if (const VkResult created =
                  vmaCreateBuffer(mDevice.allocator(), &info, &allocation, &block.buffer, &block.allocation,
                          &allocationInfo);
      created != VK_SUCCESS) {
    return fromVk(created);
  }
  block.mapped = static_cast<uint8_t*>(allocationInfo.pMappedData);
  if (block.mapped == nullptr) {
    vmaDestroyBuffer(mDevice.allocator(), block.buffer, block.allocation);
    return Result::OutOfMemory;
  }
  {
    VkMemoryPropertyFlags properties = 0;
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    mDevice.api().vkGetPhysicalDeviceMemoryProperties(mDevice.physicalDevice(), &memoryProperties);
    if (allocationInfo.memoryType < memoryProperties.memoryTypeCount) {
      properties = memoryProperties.memoryTypes[allocationInfo.memoryType].propertyFlags;
    }
    block.coherent = (properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
  }
  block.offset = 0;

  const VkDeviceSize aligned = alignUp(block.offset, alignment);
  std::memcpy(block.mapped + aligned, data, size);
  if (!block.coherent) vmaFlushAllocation(mDevice.allocator(), block.allocation, aligned, size);
  block.offset = aligned + size;

  mStaging.push_back(block);
  *outBuffer = block.buffer;
  *outOffset = aligned;
  return Result::Success;
}

// ==================================================================================================
// Render passes
// ==================================================================================================

Result VulkanCommandBuffer::beginRenderPassFallback(const RenderPassBeginInfo& info) {
  // ## Vulkan 1.0/1.1: render pass + framebuffer
  //
  // The render pass is built with LOAD/STORE so it can be reused for every pass onto a target. A clear
  // is a `vkCmdClearAttachments` inside the pass, which keeps the "clear replaces, load preserves"
  // distinction the dynamic-rendering path expresses in `VkRenderingAttachmentInfo`. The framebuffer is
  // cached by the device on (pass, views, extent): a window image and a layer are redrawn many times and
  // creating one per pass would be the wrong default.
  VulkanRenderPass* pass = mDevice.renderPass(info.renderPass);
  if (pass == nullptr || pass->renderPass == VK_NULL_HANDLE) {
    mDevice.emitMessage(MessageSeverity::Error,
            "a Vulkan 1.0/1.1 render pass needs a compatible render pass handle");
    return Result::InvalidHandle;
  }

  std::array<VulkanTextureView, kMaxColorAttachments> colorViews{};
  std::array<VkImageView, kMaxColorAttachments> colorImageViews{};
  VkImageView depthImageView = VK_NULL_HANDLE;
  uint32_t colorCount = 0;
  uint32_t framebufferWidth = 0;
  uint32_t framebufferHeight = 0;
  uint32_t areaWidth = info.renderArea.width;
  uint32_t areaHeight = info.renderArea.height;
  mRenderingToSwapchain = false;
  mPendingResolveCount = 0;

  const auto adoptExtent = [&](const VulkanTextureView& view) {
    if (framebufferWidth == 0 || framebufferHeight == 0) {
      framebufferWidth = view.extent.width;
      framebufferHeight = view.extent.height;
    }
    if (areaWidth == 0 || areaHeight == 0) {
      areaWidth = view.extent.width;
      areaHeight = view.extent.height;
    }
  };

  for (uint32_t index = 0; index < info.colorAttachmentCount; ++index) {
    const RenderPassAttachment& attachment = info.colorAttachments[index];
    VulkanTextureView resolved;
    if (!mDevice.resolveAttachmentView(attachment.view, &resolved)) {
      mDevice.emitMessage(MessageSeverity::Error, "a render pass colour attachment does not resolve to a view");
      return Result::InvalidHandle;
    }
    colorViews[index] = resolved;
    colorImageViews[index] = resolved.view;
    adoptExtent(resolved);

    if (attachment.resolveTarget.valid()) {
      // The fallback has no resolve attachment in its `VkRenderPass` (the RHI's RenderPassDescriptor only
      // describes the multisampled shape), so it records a `vkCmdResolveImage` after the pass instead.
      // Validate the pair here rather than leaving a driver validation error at submit time.
      VulkanTextureView resolvedTarget;
      if (!mDevice.resolveAttachmentView(attachment.resolveTarget, &resolvedTarget)) {
        mDevice.emitMessage(MessageSeverity::Error, "a render pass resolve target does not resolve to a view");
        return Result::InvalidHandle;
      }
      const VulkanTexture* sourceTexture = mDevice.texture(resolved.texture);
      const VulkanTexture* destinationTexture = mDevice.texture(resolvedTarget.texture);
      const bool destinationIsDefault = resolvedTarget.isDefaultFramebuffer;
      if (sourceTexture == nullptr || (!destinationIsDefault && destinationTexture == nullptr)) {
        mDevice.emitMessage(MessageSeverity::Error, "a render pass resolve names a texture that does not exist");
        return Result::InvalidHandle;
      }
      if (sourceTexture->sampleCount <= 1 ||
          (!destinationIsDefault && destinationTexture->sampleCount != 1)) {
        mDevice.emitMessage(MessageSeverity::Error,
                "MSAA resolve needs a multisampled source and a single-sampled destination");
        return Result::InvalidArgument;
      }
      if (!destinationIsDefault && resolved.image == resolvedTarget.image) {
        mDevice.emitMessage(MessageSeverity::Error, "MSAA resolve source and destination are the same image");
        return Result::InvalidArgument;
      }
      if (sourceTexture->sampleCount != pass->sampleCount || resolved.vkFormat != resolvedTarget.vkFormat ||
          resolved.extent.width != resolvedTarget.extent.width ||
          resolved.extent.height != resolvedTarget.extent.height) {
        mDevice.emitMessage(MessageSeverity::Error,
                "the MSAA source, resolve target and render pass do not agree on samples, format or extent");
        return Result::InvalidArgument;
      }
      if (mPendingResolveCount >= kMaxColorAttachments) {
        mDevice.emitMessage(MessageSeverity::Error, "too many MSAA resolves in one render pass");
        return Result::InvalidArgument;
      }

      if (destinationIsDefault) {
        // A window MSAA target resolves into the swapchain image, which starts PRESENT_SRC_KHR.
        const VkImageLayout current = mDevice.currentSwapchainLayout();
        if (current != VK_IMAGE_LAYOUT_GENERAL) {
          VkImageMemoryBarrier barrier{};
          barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
          barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
          barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
          barrier.oldLayout = current;
          barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
          barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.image = resolvedTarget.image;
          barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
          mDevice.api().vkCmdPipelineBarrier(mCommandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
          mDevice.setCurrentSwapchainLayout(VK_IMAGE_LAYOUT_GENERAL);
        }
        mRenderingToSwapchain = true;
      }

      PendingResolve& pending = mPendingResolves[mPendingResolveCount++];
      pending.source = resolved.image;
      pending.destination = resolvedTarget.image;
      pending.region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, resolved.baseMipLevel, resolved.baseArrayLayer, 1 };
      pending.region.srcOffset = { 0, 0, 0 };
      pending.region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, resolvedTarget.baseMipLevel,
        resolvedTarget.baseArrayLayer, 1 };
      pending.region.dstOffset = { 0, 0, 0 };
      pending.region.extent = { resolved.extent.width, resolved.extent.height, 1 };
    }

    if (resolved.isDefaultFramebuffer) {
      const VkImageLayout current = mDevice.currentSwapchainLayout();
      if (current != VK_IMAGE_LAYOUT_GENERAL) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        barrier.oldLayout = current;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = resolved.image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        mDevice.api().vkCmdPipelineBarrier(mCommandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        mDevice.setCurrentSwapchainLayout(VK_IMAGE_LAYOUT_GENERAL);
      }
      mRenderingToSwapchain = true;
    }
    ++colorCount;
  }

  const bool hasDepth = info.hasDepthStencil;
  if (hasDepth) {
    VulkanTextureView resolved;
    if (!mDevice.resolveAttachmentView(info.depthStencilAttachment.view, &resolved)) {
      mDevice.emitMessage(MessageSeverity::Error, "the render pass depth attachment does not resolve to a view");
      return Result::InvalidHandle;
    }
    depthImageView = resolved.view;
    adoptExtent(resolved);
  }

  if (framebufferWidth == 0 || framebufferHeight == 0 || areaWidth == 0 || areaHeight == 0) {
    mDevice.emitMessage(MessageSeverity::Error, "a render pass has no area: no attachment reported a size");
    return Result::InvalidArgument;
  }

  const VkFramebuffer framebuffer = mDevice.framebufferFor(pass->renderPass, colorImageViews.data(), colorCount,
          depthImageView, framebufferWidth, framebufferHeight);
  if (framebuffer == VK_NULL_HANDLE) {
    mDevice.emitMessage(MessageSeverity::Error, "the Vulkan 1.0/1.1 framebuffer is unavailable");
    return Result::Error;
  }

  // Everything written before this pass has to be visible to the attachment. GENERAL means there is
  // no layout change to carry the dependency, same as the dynamic-rendering path.
  memoryBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, kAllWrites, kAllReads);

  VkRenderPassBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  begin.renderPass = pass->renderPass;
  begin.framebuffer = framebuffer;
  begin.renderArea.offset = { 0, 0 };
  begin.renderArea.extent = { areaWidth, areaHeight };
  begin.clearValueCount = 0;
  begin.pClearValues = nullptr;
  mDevice.api().vkCmdBeginRenderPass(mCommandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);

  VkClearRect clearRect{};
  clearRect.rect.offset = { 0, 0 };
  clearRect.rect.extent = { areaWidth, areaHeight };
  clearRect.baseArrayLayer = 0;
  clearRect.layerCount = 1;

  for (uint32_t index = 0; index < colorCount; ++index) {
    const RenderPassAttachment& attachment = info.colorAttachments[index];
    if (attachment.loadOp != LoadOp::Clear) continue;
    VkClearAttachment clear{};
    clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clear.colorAttachment = index;
    clear.clearValue.color.float32[0] = static_cast<float>(attachment.clearColor.r);
    clear.clearValue.color.float32[1] = static_cast<float>(attachment.clearColor.g);
    clear.clearValue.color.float32[2] = static_cast<float>(attachment.clearColor.b);
    clear.clearValue.color.float32[3] = static_cast<float>(attachment.clearColor.a);
    mDevice.api().vkCmdClearAttachments(mCommandBuffer, 1, &clear, 1, &clearRect);
  }

  if (hasDepth && info.depthStencilAttachment.loadOp == LoadOp::Clear) {
    VkClearAttachment clear{};
    clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (isStencilFormat(pass->depthStencilFormat)) clear.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    clear.clearValue.depthStencil.depth = info.depthStencilAttachment.clearDepthStencil.depth;
    clear.clearValue.depthStencil.stencil = info.depthStencilAttachment.clearDepthStencil.stencil;
    mDevice.api().vkCmdClearAttachments(mCommandBuffer, 1, &clear, 1, &clearRect);
  }

  mRenderPassOpen = true;
  mColorAttachmentCount = colorCount;
  mRenderAreaWidth = areaWidth;
  mRenderAreaHeight = areaHeight;

  VkViewport viewport{};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = static_cast<float>(areaWidth);
  viewport.height = static_cast<float>(areaHeight);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  mDevice.api().vkCmdSetViewport(mCommandBuffer, 0, 1, &viewport);

  VkRect2D scissor{};
  scissor.offset = { 0, 0 };
  scissor.extent = { areaWidth, areaHeight };
  mDevice.api().vkCmdSetScissor(mCommandBuffer, 0, 1, &scissor);

  const float blendConstants[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
  mDevice.api().vkCmdSetBlendConstants(mCommandBuffer, blendConstants);
  mDevice.api().vkCmdSetStencilReference(mCommandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, 0);

  mIndexBufferBound = false;
  mFanIndexBound = false;
  return Result::Success;
}

Result VulkanCommandBuffer::beginRenderPass(const RenderPassBeginInfo& info) {
  if (!mRecording) return Result::Error;
  if (mRenderPassOpen) {
    mDevice.emitMessage(MessageSeverity::Error, "beginRenderPass was called while a pass was already open");
    return Result::Error;
  }
  if (info.colorAttachmentCount > kMaxColorAttachments) return Result::InvalidArgument;
  if (info.renderPass.valid() && mDevice.renderPass(info.renderPass) == nullptr) {
    return Result::InvalidHandle;
  }

  if (!mDevice.usesDynamicRendering()) {
    return beginRenderPassFallback(info);
  }

  std::array<VulkanTextureView, kMaxColorAttachments> colorViews{};
  std::array<VkRenderingAttachmentInfo, kMaxColorAttachments> colorAttachments{};
  uint32_t colorCount = 0;
  uint32_t areaWidth = info.renderArea.width;
  uint32_t areaHeight = info.renderArea.height;

  const auto transitionDefaultToGeneral = [&](const VulkanTextureView& view) {
    const VkImageLayout current = mDevice.currentSwapchainLayout();
    if (current == VK_IMAGE_LAYOUT_GENERAL) return;
    // The presentation engine handed the image back in PRESENT_SRC_KHR. This is the one transition
    // the backend needs; every other image is already in GENERAL. A swapchain image can be either the
    // colour attachment (window target) or the resolve destination (MSAA window), so this is shared.
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    barrier.oldLayout = current;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = view.image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    mDevice.api().vkCmdPipelineBarrier(mCommandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    mDevice.setCurrentSwapchainLayout(VK_IMAGE_LAYOUT_GENERAL);
  };

  for (uint32_t index = 0; index < info.colorAttachmentCount; ++index) {
    const RenderPassAttachment& attachment = info.colorAttachments[index];
    VulkanTextureView resolved;
    if (!mDevice.resolveAttachmentView(attachment.view, &resolved)) {
      mDevice.emitMessage(MessageSeverity::Error, "a render pass colour attachment does not resolve to a view");
      return Result::InvalidHandle;
    }
    colorViews[index] = resolved;
    if (areaWidth == 0 || areaHeight == 0) {
      areaWidth = resolved.extent.width;
      areaHeight = resolved.extent.height;
    }

    if (resolved.isDefaultFramebuffer) {
      transitionDefaultToGeneral(resolved);
      mRenderingToSwapchain = true;
    }

    VkRenderingAttachmentInfo& target = colorAttachments[index];
    target.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    target.imageView = resolved.view;
    target.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    target.loadOp = toVkLoadOp(attachment.loadOp);
    target.storeOp = toVkStoreOp(attachment.storeOp);
    target.clearValue.color.float32[0] = static_cast<float>(attachment.clearColor.r);
    target.clearValue.color.float32[1] = static_cast<float>(attachment.clearColor.g);
    target.clearValue.color.float32[2] = static_cast<float>(attachment.clearColor.b);
    target.clearValue.color.float32[3] = static_cast<float>(attachment.clearColor.a);

    if (attachment.resolveTarget.valid()) {
      VulkanTextureView resolvedTarget;
      if (!mDevice.resolveAttachmentView(attachment.resolveTarget, &resolvedTarget)) {
        return Result::InvalidHandle;
      }
      if (resolvedTarget.isDefaultFramebuffer) {
        // A window MSAA target resolves into the swapchain image, which starts PRESENT_SRC_KHR.
        transitionDefaultToGeneral(resolvedTarget);
        mRenderingToSwapchain = true;
      }
      target.resolveImageView = resolvedTarget.view;
      target.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
      target.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    }
    ++colorCount;
  }

  VkRenderingAttachmentInfo depthAttachment{};
  VkRenderingAttachmentInfo stencilAttachment{};
  bool depthHasStencil = false;
  const bool hasDepth = info.hasDepthStencil;
  if (hasDepth) {
    VulkanTextureView resolved;
    if (!mDevice.resolveAttachmentView(info.depthStencilAttachment.view, &resolved)) {
      mDevice.emitMessage(MessageSeverity::Error, "the render pass depth attachment does not resolve to a view");
      return Result::InvalidHandle;
    }
    depthHasStencil = isStencilFormat(resolved.format);
    if (areaWidth == 0 || areaHeight == 0) {
      areaWidth = resolved.extent.width;
      areaHeight = resolved.extent.height;
    }

    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = resolved.view;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    depthAttachment.loadOp = toVkLoadOp(info.depthStencilAttachment.loadOp);
    depthAttachment.storeOp = toVkStoreOp(info.depthStencilAttachment.storeOp);
    depthAttachment.clearValue.depthStencil.depth = info.depthStencilAttachment.clearDepthStencil.depth;
    depthAttachment.clearValue.depthStencil.stencil = info.depthStencilAttachment.clearDepthStencil.stencil;
    stencilAttachment = depthAttachment;
  }

  if (areaWidth == 0 || areaHeight == 0) {
    mDevice.emitMessage(MessageSeverity::Error, "a render pass has no area: no attachment reported a size");
    return Result::InvalidArgument;
  }

  // Everything written before this pass (an upload, a previous target) has to be visible to the
  // attachment and to any texture the pass samples. The barrier is deliberately broad: the layout is
  // GENERAL everywhere, so there is no layout transition to carry the dependency, and a narrower mask
  // would have to reason about every preceding command in this command buffer.
  memoryBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, kAllWrites, kAllReads);

  VkRenderingInfo rendering{};
  rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  rendering.flags = 0;
  rendering.renderArea.offset = { 0, 0 };
  rendering.renderArea.extent = { areaWidth, areaHeight };
  rendering.layerCount = 1;
  rendering.viewMask = 0;
  rendering.colorAttachmentCount = colorCount;
  rendering.pColorAttachments = colorCount == 0 ? nullptr : colorAttachments.data();
  rendering.pDepthAttachment = hasDepth ? &depthAttachment : nullptr;
  // A combined depth/stencil format has to name the image twice; a depth-only format must not name it
  // as a stencil attachment at all, which is why the flag is derived from the resolved view's format.
  rendering.pStencilAttachment = hasDepth && depthHasStencil ? &stencilAttachment : nullptr;

  mDevice.api().vkCmdBeginRendering(mCommandBuffer, &rendering);

  mRenderPassOpen = true;
  mColorAttachmentCount = colorCount;
  mRenderAreaWidth = areaWidth;
  mRenderAreaHeight = areaHeight;

  // The RHI's viewport is top-left origin, which is Vulkan's own convention; the canvas relies on a
  // full-target default because it never calls `setViewport`.
  VkViewport viewport{};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = static_cast<float>(areaWidth);
  viewport.height = static_cast<float>(areaHeight);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  mDevice.api().vkCmdSetViewport(mCommandBuffer, 0, 1, &viewport);

  VkRect2D scissor{};
  scissor.offset = { 0, 0 };
  scissor.extent = { areaWidth, areaHeight };
  mDevice.api().vkCmdSetScissor(mCommandBuffer, 0, 1, &scissor);

  const float blendConstants[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
  mDevice.api().vkCmdSetBlendConstants(mCommandBuffer, blendConstants);
  mDevice.api().vkCmdSetStencilReference(mCommandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, 0);

  mIndexBufferBound = false;
  mFanIndexBound = false;
  return Result::Success;
}

Result VulkanCommandBuffer::endRenderPass() {
  if (!mRecording || !mRenderPassOpen) return Result::Error;

  if (mDevice.usesDynamicRendering()) {
    mDevice.api().vkCmdEndRendering(mCommandBuffer);
  } else {
    mDevice.api().vkCmdEndRenderPass(mCommandBuffer);
  }
  mRenderPassOpen = false;

  // The pass's writes have to be visible to whatever samples the target next: a composite, a mask, the
  // next ping-pong step. GENERAL means there is no layout transition to imply the dependency.
  memoryBarrier(VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, kAllWrites, kAllReads);

  if (mPendingResolveCount != 0) {
    // The fallback render pass has no resolve attachment, so the resolve is a transfer operation after
    // the pass. The barrier above made the multisampled writes visible to the transfer stage; this one
    // makes the resolved image visible to the shader/composite that reads it next.
    for (uint32_t index = 0; index < mPendingResolveCount; ++index) {
      const PendingResolve& resolve = mPendingResolves[index];
      mDevice.api().vkCmdResolveImage(mCommandBuffer, resolve.source, VK_IMAGE_LAYOUT_GENERAL, resolve.destination,
              VK_IMAGE_LAYOUT_GENERAL, 1, &resolve.region);
    }
    memoryBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, kAllReads);
    mPendingResolveCount = 0;
  }
  return Result::Success;
}

// ==================================================================================================
// Pipeline and binding state
// ==================================================================================================

Result VulkanCommandBuffer::bindRenderPipeline(const RenderPipelineHandle pipelineHandle) {
  if (!mRecording) return Result::Error;
  VulkanRenderPipeline* pipeline = mDevice.renderPipeline(pipelineHandle);
  if (pipeline == nullptr) return Result::InvalidHandle;

  mDevice.api().vkCmdBindPipeline(mCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);
  mBoundRenderPipeline = pipelineHandle;
  mBoundComputePipeline = ComputePipelineHandle::invalid();
  mBoundPipelineLayout = pipeline->layout;

  // ## The dynamic state that belongs to the pipeline
  //
  // Stencil reference and blend constants are *pipeline* state in the RHI (`DepthStencilState::
  // stencilReference`, `RasterizerState::blendConstant`), but Vulkan declares them dynamic so that a
  // pipeline does not have to be rebuilt when only the value changes. Binding a pipeline therefore has to
  // push its values into the command buffer, or a pipeline that asked for a non-zero reference would run
  // with whatever the previous draw left - which is exactly the kind of state leak the OpenGL backend
  // applies at `glStencilFunc`/`glBlendColor` time.
  const DepthStencilState& depthStencil = pipeline->descriptor.depthStencil;
  mDevice.api().vkCmdSetStencilReference(mCommandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK,
          depthStencil.stencilReference);
  const Color& blendConstant = pipeline->descriptor.rasterizer.blendConstant;
  const float constants[4] = { static_cast<float>(blendConstant.r), static_cast<float>(blendConstant.g),
    static_cast<float>(blendConstant.b), static_cast<float>(blendConstant.a) };
  mDevice.api().vkCmdSetBlendConstants(mCommandBuffer, constants);

  mIndexBufferBound = false;
  mFanIndexBound = false;
  return Result::Success;
}

Result VulkanCommandBuffer::bindComputePipeline(const ComputePipelineHandle pipelineHandle) {
  if (!mRecording) return Result::Error;
  VulkanComputePipeline* pipeline = mDevice.computePipeline(pipelineHandle);
  if (pipeline == nullptr) return Result::InvalidHandle;

  mDevice.api().vkCmdBindPipeline(mCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline);
  mBoundComputePipeline = pipelineHandle;
  mBoundRenderPipeline = RenderPipelineHandle::invalid();
  mBoundPipelineLayout = pipeline->layout;
  return Result::Success;
}

Result VulkanCommandBuffer::setVertexBuffer(const uint32_t slot, const BufferHandle bufferHandle,
        const uint64_t offset) {
  if (!mRecording) return Result::Error;
  if (const Result valid = requireBuffer(bufferHandle, "setVertexBuffer"); !isSuccess(valid)) return valid;
  const VulkanBuffer* buffer = mDevice.buffer(bufferHandle);
  if (buffer == nullptr) return Result::InvalidHandle;
  if (offset >= buffer->size) return Result::InvalidArgument;

  const VkDeviceSize vkOffset = offset;
  mDevice.api().vkCmdBindVertexBuffers(mCommandBuffer, slot, 1, &buffer->buffer, &vkOffset);
  return Result::Success;
}

Result VulkanCommandBuffer::setIndexBuffer(const BufferHandle bufferHandle, const IndexFormat format,
        const uint64_t offset) {
  if (!mRecording) return Result::Error;
  if (const Result valid = requireBuffer(bufferHandle, "setIndexBuffer"); !isSuccess(valid)) return valid;

  // Bound at the draw rather than here: a fan draw replaces the binding with the device's fixed fan
  // pattern, and re-binding here only to have the fan path overwrite it would be two calls per draw.
  mIndexBuffer = bufferHandle;
  mIndexFormat = format;
  mIndexOffset = offset;
  mIndexBufferBound = false;
  return Result::Success;
}

Result VulkanCommandBuffer::bindDescriptorSet(const DescriptorSetIndex setIndex, const DescriptorSetHandle set) {
  if (!mRecording) return Result::Error;
  if (setIndex >= kMaxDescriptorSetLayouts) return Result::InvalidArgument;
  if (set.valid() && mDevice.descriptorSet(set) == nullptr) return Result::InvalidHandle;

  mPendingSets[setIndex].index = setIndex;
  mPendingSets[setIndex].set = set;
  mPendingSets[setIndex].occupied = true;
  return Result::Success;
}

Result VulkanCommandBuffer::setViewport(const Viewport& viewport) {
  if (!mRecording) return Result::Error;
  VkViewport vkViewport{};
  vkViewport.x = viewport.x;
  vkViewport.y = viewport.y;
  vkViewport.width = viewport.width > 0.0f ? viewport.width
                                           : static_cast<float>(mRenderAreaWidth);
  vkViewport.height = viewport.height > 0.0f ? viewport.height
                                             : static_cast<float>(mRenderAreaHeight);
  vkViewport.minDepth = viewport.minDepth;
  vkViewport.maxDepth = viewport.maxDepth;
  mDevice.api().vkCmdSetViewport(mCommandBuffer, 0, 1, &vkViewport);
  return Result::Success;
}

Result VulkanCommandBuffer::setScissor(const ScissorRect& scissor) {
  if (!mRecording) return Result::Error;

  // Vulkan requires the scissor to lie inside the render area; the RHI's clip rectangles are computed
  // from the scene and can extend past a target. Clamping is the backend's job here because the GL
  // backend's driver does it implicitly.
  int32_t x = std::max<int32_t>(scissor.x, 0);
  int32_t y = std::max<int32_t>(scissor.y, 0);
  const uint32_t areaWidth = mRenderAreaWidth;
  const uint32_t areaHeight = mRenderAreaHeight;
  x = std::min<int32_t>(x, static_cast<int32_t>(areaWidth));
  y = std::min<int32_t>(y, static_cast<int32_t>(areaHeight));
  uint32_t width = scissor.width;
  uint32_t height = scissor.height;
  if (static_cast<uint32_t>(x) >= areaWidth) width = 0;
  else width = std::min<uint32_t>(width, areaWidth - static_cast<uint32_t>(x));
  if (static_cast<uint32_t>(y) >= areaHeight) height = 0;
  else height = std::min<uint32_t>(height, areaHeight - static_cast<uint32_t>(y));

  VkRect2D rect{};
  rect.offset = { x, y };
  rect.extent = { width, height };
  mDevice.api().vkCmdSetScissor(mCommandBuffer, 0, 1, &rect);
  return Result::Success;
}

Result VulkanCommandBuffer::setStencilReference(const uint32_t reference) {
  if (!mRecording) return Result::Error;
  mDevice.api().vkCmdSetStencilReference(mCommandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, reference);
  return Result::Success;
}

Result VulkanCommandBuffer::setBlendConstant(const Color& color) {
  if (!mRecording) return Result::Error;
  const float constants[4] = { static_cast<float>(color.r), static_cast<float>(color.g),
    static_cast<float>(color.b), static_cast<float>(color.a) };
  mDevice.api().vkCmdSetBlendConstants(mCommandBuffer, constants);
  return Result::Success;
}

Result VulkanCommandBuffer::pushConstants(const void* data, const uint32_t size, const uint32_t offset) {
  if (!mRecording) return Result::Error;
  if (data == nullptr && size != 0) return Result::InvalidArgument;
  if (size == 0) return Result::Success;
  if (offset + size > mDevice.pushConstantBudget()) {
    mDevice.emitMessage(MessageSeverity::Error, "a push-constant write exceeds the device's reported budget");
    return Result::InvalidArgument;
  }

  const VulkanPipelineLayout* layout = mDevice.pipelineLayout(mBoundPipelineLayout);
  if (layout == nullptr || layout->layout == VK_NULL_HANDLE) return Result::Error;

  mDevice.api().vkCmdPushConstants(mCommandBuffer, layout->layout,
          allGraphicsStages() | VK_SHADER_STAGE_COMPUTE_BIT, offset, size, data);
  return Result::Success;
}

// ==================================================================================================
// Descriptor snapshots
// ==================================================================================================

Result VulkanCommandBuffer::emptySet(const uint32_t index, VkDescriptorSet* outSet) {
  if (index >= kMaxDescriptorSetLayouts || outSet == nullptr) return Result::InvalidArgument;
  if (mEmptySetReady[index]) {
    *outSet = mEmptySets[index];
    return Result::Success;
  }

  if (mDescriptorPools.empty()) {
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (const Result created = mDevice.createDescriptorPool(&pool); !isSuccess(created)) return created;
    mDescriptorPools.push_back(pool);
  }

  const VkDescriptorSetLayout emptyLayout = mDevice.emptyDescriptorSetLayout();
  VkDescriptorSetAllocateInfo allocate{};
  allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate.descriptorPool = mDescriptorPools.back();
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &emptyLayout;
  if (const VkResult result = mDevice.api().vkAllocateDescriptorSets(mDevice.device(), &allocate, outSet);
      result != VK_SUCCESS) {
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (const Result created = mDevice.createDescriptorPool(&pool); !isSuccess(created)) return created;
    mDescriptorPools.push_back(pool);
    allocate.descriptorPool = pool;
    if (const VkResult retried = mDevice.api().vkAllocateDescriptorSets(mDevice.device(), &allocate, outSet);
        retried != VK_SUCCESS) {
      return fromVk(retried);
    }
  }

  mEmptySets[index] = *outSet;
  mEmptySetReady[index] = true;
  return Result::Success;
}

Result VulkanCommandBuffer::snapshotDescriptorSet(const DescriptorSetHandle setHandle, VkDescriptorSet* outSet) {
  VulkanDescriptorSet* set = mDevice.descriptorSet(setHandle);
  if (set == nullptr || outSet == nullptr) return Result::InvalidHandle;

  const SnapshotKey key{ setHandle.value(), set->version };
  if (const auto found = mDescriptorSnapshots.find(key); found != mDescriptorSnapshots.end()) {
    *outSet = found->second;
    return Result::Success;
  }

  const VulkanDescriptorSetLayout* layout = mDevice.descriptorSetLayout(set->layout);
  if (layout == nullptr) return Result::InvalidHandle;

  if (mDescriptorPools.empty()) {
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (const Result created = mDevice.createDescriptorPool(&pool); !isSuccess(created)) return created;
    mDescriptorPools.push_back(pool);
  }

  VkDescriptorSetAllocateInfo allocate{};
  allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate.descriptorPool = mDescriptorPools.back();
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &layout->layout;

  VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
  if (VkResult result = mDevice.api().vkAllocateDescriptorSets(mDevice.device(), &allocate, &descriptorSet);
      result != VK_SUCCESS) {
    // A full pool is the expected case for a large frame; a second pool is cheaper than a set sized for
    // the worst frame anyone ever records.
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (const Result created = mDevice.createDescriptorPool(&pool); !isSuccess(created)) return created;
    mDescriptorPools.push_back(pool);
    allocate.descriptorPool = pool;
    result = mDevice.api().vkAllocateDescriptorSets(mDevice.device(), &allocate, &descriptorSet);
    if (result != VK_SUCCESS) return fromVk(result);
  }

  std::vector<VkWriteDescriptorSet> writes;
  std::vector<VkDescriptorImageInfo> images;
  std::vector<VkDescriptorBufferInfo> buffers;
  writes.reserve(set->bindings.size());
  images.reserve(set->bindings.size());
  buffers.reserve(set->bindings.size());

  const VkImageView fallbackView = mDevice.fallbackTextureView();
  const VkSampler fallbackSampler = mDevice.fallbackSampler();

  for (const VulkanDescriptorBindingValue& binding: set->bindings) {
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptorSet;
    write.dstBinding = binding.binding;
    write.dstArrayElement = binding.arrayIndex;
    write.descriptorCount = 1;
    write.descriptorType = toVkDescriptorType(binding.type);

    switch (binding.type) {
      case BindingType::TextureSampler: {
        VkDescriptorImageInfo image{};
        const VulkanTextureView* explicitView = binding.view.valid() ? mDevice.textureView(binding.view) : nullptr;
        const VulkanTexture* texture = mDevice.texture(binding.texture);
        image.imageView = explicitView != nullptr ? explicitView->view
                : (texture != nullptr && texture->view != VK_NULL_HANDLE ? texture->view : fallbackView);
        const VulkanSampler* sampler = binding.sampler.valid() ? mDevice.sampler(binding.sampler) : nullptr;
        image.sampler = sampler != nullptr ? sampler->sampler : fallbackSampler;
        image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        images.push_back(image);
        write.pImageInfo = &images.back();
        break;
      }
      case BindingType::Texture: {
        VkDescriptorImageInfo image{};
        const VulkanTextureView* explicitView = binding.view.valid() ? mDevice.textureView(binding.view) : nullptr;
        const VulkanTexture* texture = mDevice.texture(binding.texture);
        image.imageView = explicitView != nullptr ? explicitView->view
                : (texture != nullptr && texture->view != VK_NULL_HANDLE ? texture->view : fallbackView);
        image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        images.push_back(image);
        write.pImageInfo = &images.back();
        break;
      }
      case BindingType::Sampler: {
        VkDescriptorImageInfo image{};
        const VulkanSampler* sampler = binding.sampler.valid() ? mDevice.sampler(binding.sampler) : nullptr;
        image.sampler = sampler != nullptr ? sampler->sampler : fallbackSampler;
        images.push_back(image);
        write.pImageInfo = &images.back();
        break;
      }
      case BindingType::UniformBuffer:
      case BindingType::ReadOnlyStorageBuffer:
      case BindingType::StorageBuffer: {
        VkDescriptorBufferInfo bufferInfo{};
        const VulkanBuffer* buffer = binding.buffer.buffer.valid() ? mDevice.buffer(binding.buffer.buffer) : nullptr;
        if (buffer != nullptr) {
          bufferInfo.buffer = buffer->buffer;
          bufferInfo.offset = std::min(binding.buffer.offset, buffer->size);
          const uint64_t remaining = buffer->size - bufferInfo.offset;
          bufferInfo.range = binding.buffer.size == 0 ? remaining : std::min(binding.buffer.size, remaining);
        } else {
          bufferInfo.buffer = mDevice.fallbackBuffer();
          bufferInfo.offset = 0;
          bufferInfo.range = mDevice.fallbackBufferSize();
        }
        buffers.push_back(bufferInfo);
        write.pBufferInfo = &buffers.back();
        break;
      }
    }
    writes.push_back(write);
  }

  if (!writes.empty()) {
    mDevice.api().vkUpdateDescriptorSets(mDevice.device(), static_cast<uint32_t>(writes.size()), writes.data(), 0,
            nullptr);
  }

  mDescriptorSnapshots.emplace(key, descriptorSet);
  *outSet = descriptorSet;
  return Result::Success;
}

Result VulkanCommandBuffer::applyDescriptorSets() {
  const VulkanPipelineLayout* layout = mDevice.pipelineLayout(mBoundPipelineLayout);
  if (layout == nullptr || layout->setLayoutCount == 0) {
    // A pipeline built with no set layouts binds nothing, which is the common case for the indexed-draw
    // and push-constant tests.
    return Result::Success;
  }

  std::array<VkDescriptorSet, kMaxDescriptorSetLayouts> sets{};
  for (uint32_t index = 0; index < layout->setLayoutCount; ++index) {
    if (mPendingSets[index].occupied && mPendingSets[index].set.valid()) {
      if (const Result result = snapshotDescriptorSet(mPendingSets[index].set, &sets[index]); !isSuccess(result)) {
        return result;
      }
    } else {
      if (const Result result = emptySet(index, &sets[index]); !isSuccess(result)) return result;
    }
  }

  const VkPipelineBindPoint bindPoint = mBoundComputePipeline.valid() ? VK_PIPELINE_BIND_POINT_COMPUTE
                                                                     : VK_PIPELINE_BIND_POINT_GRAPHICS;
  mDevice.api().vkCmdBindDescriptorSets(mCommandBuffer, bindPoint, layout->layout, 0, layout->setLayoutCount,
          sets.data(), 0, nullptr);
  return Result::Success;
}

void VulkanCommandBuffer::rebindIndexBuffer() {
  const VulkanBuffer* buffer = mDevice.buffer(mIndexBuffer);
  if (buffer == nullptr) return;
  mDevice.api().vkCmdBindIndexBuffer(mCommandBuffer, buffer->buffer, mIndexOffset, toVkIndexType(mIndexFormat));
  mIndexBufferBound = true;
  mFanIndexBound = false;
}

// ==================================================================================================
// Drawing and dispatch
// ==================================================================================================

Result VulkanCommandBuffer::draw(const uint32_t vertexCount, const uint32_t instanceCount, const uint32_t firstVertex,
        const uint32_t firstInstance) {
  if (const Result pass = requireRenderPass("draw"); !isSuccess(pass)) return pass;
  if (const Result pipeline = requirePipeline("draw"); !isSuccess(pipeline)) return pipeline;
  if (instanceCount == 0 || vertexCount == 0) return Result::Success;

  VulkanRenderPipeline* pipeline = mDevice.renderPipeline(mBoundRenderPipeline);
  if (pipeline == nullptr) return Result::Error;
  if (const Result descriptors = applyDescriptorSets(); !isSuccess(descriptors)) return descriptors;

  if (pipeline->fanEmulation) {
    // Vulkan has no triangle fan. The device's fixed index pattern `0, i-1, i` turns any fan into a
    // triangle list, and `firstVertex` becomes the index buffer's vertex offset so the caller's vertex
    // buffer slicing is unchanged.
    if (vertexCount < 3) return Result::Success;
    if (vertexCount > mDevice.fanIndexCapacity()) {
      mDevice.emitMessage(MessageSeverity::Error, "a triangle fan draw exceeds the backend's fan index capacity");
      return Result::Unsupported;
    }
    if (!mFanIndexBound) {
      mDevice.api().vkCmdBindIndexBuffer(mCommandBuffer, mDevice.fanIndexBuffer(), 0, VK_INDEX_TYPE_UINT16);
      mFanIndexBound = true;
      mIndexBufferBound = false;
    }
    mDevice.api().vkCmdDrawIndexed(mCommandBuffer, (vertexCount - 2) * 3, instanceCount, 0,
            static_cast<int32_t>(firstVertex), firstInstance);
    ++mCounts.drawsIndexed;
    return Result::Success;
  }

  mDevice.api().vkCmdDraw(mCommandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
  ++mCounts.draws;
  return Result::Success;
}

Result VulkanCommandBuffer::drawIndexed(const uint32_t indexCount, const uint32_t instanceCount,
        const uint32_t firstIndex, const int32_t vertexOffset, const uint32_t firstInstance) {
  if (const Result pass = requireRenderPass("drawIndexed"); !isSuccess(pass)) return pass;
  if (const Result pipeline = requirePipeline("drawIndexed"); !isSuccess(pipeline)) return pipeline;
  if (indexCount == 0 || instanceCount == 0) return Result::Success;
  if (mDevice.buffer(mIndexBuffer) == nullptr) {
    mDevice.emitMessage(MessageSeverity::Error, "drawIndexed was recorded without an index buffer");
    return Result::Error;
  }

  if (const Result descriptors = applyDescriptorSets(); !isSuccess(descriptors)) return descriptors;
  if (!mIndexBufferBound || mFanIndexBound) rebindIndexBuffer();

  mDevice.api().vkCmdDrawIndexed(mCommandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
  ++mCounts.drawsIndexed;
  return Result::Success;
}

Result VulkanCommandBuffer::drawIndirect(const BufferHandle arguments, const uint64_t offset) {
  if (const Result pass = requireRenderPass("drawIndirect"); !isSuccess(pass)) return pass;
  if (const Result pipeline = requirePipeline("drawIndirect"); !isSuccess(pipeline)) return pipeline;
  const VulkanBuffer* buffer = mDevice.buffer(arguments);
  if (buffer == nullptr) return Result::InvalidHandle;

  if (const Result descriptors = applyDescriptorSets(); !isSuccess(descriptors)) return descriptors;
  mDevice.api().vkCmdDrawIndirect(mCommandBuffer, buffer->buffer, offset, 1, 0);
  ++mCounts.drawsIndirect;
  return Result::Success;
}

Result VulkanCommandBuffer::drawIndexedIndirect(const BufferHandle arguments, const uint64_t offset) {
  if (const Result pass = requireRenderPass("drawIndexedIndirect"); !isSuccess(pass)) return pass;
  if (const Result pipeline = requirePipeline("drawIndexedIndirect"); !isSuccess(pipeline)) return pipeline;
  const VulkanBuffer* buffer = mDevice.buffer(arguments);
  if (buffer == nullptr) return Result::InvalidHandle;
  if (mDevice.buffer(mIndexBuffer) == nullptr) return Result::Error;

  if (const Result descriptors = applyDescriptorSets(); !isSuccess(descriptors)) return descriptors;
  if (!mIndexBufferBound || mFanIndexBound) rebindIndexBuffer();
  mDevice.api().vkCmdDrawIndexedIndirect(mCommandBuffer, buffer->buffer, offset, 1, 0);
  ++mCounts.drawsIndirect;
  return Result::Success;
}

Result VulkanCommandBuffer::dispatch(const uint32_t groupCountX, const uint32_t groupCountY,
        const uint32_t groupCountZ) {
  if (!mRecording) return Result::Error;
  if (!mBoundComputePipeline.valid()) return Result::Error;
  if (const Result descriptors = applyDescriptorSets(); !isSuccess(descriptors)) return descriptors;
  mDevice.api().vkCmdDispatch(mCommandBuffer, groupCountX, groupCountY, groupCountZ);
  ++mCounts.dispatches;
  return Result::Success;
}

Result VulkanCommandBuffer::dispatchIndirect(const BufferHandle arguments, const uint64_t offset) {
  if (!mRecording) return Result::Error;
  if (!mBoundComputePipeline.valid()) return Result::Error;
  const VulkanBuffer* buffer = mDevice.buffer(arguments);
  if (buffer == nullptr) return Result::InvalidHandle;
  if (const Result descriptors = applyDescriptorSets(); !isSuccess(descriptors)) return descriptors;
  mDevice.api().vkCmdDispatchIndirect(mCommandBuffer, buffer->buffer, offset);
  ++mCounts.dispatches;
  return Result::Success;
}

// ==================================================================================================
// Debug markers
// ==================================================================================================

Result VulkanCommandBuffer::insertDebugMarker(const char* label) {
  if (!mRecording || label == nullptr) return Result::InvalidArgument;
  if (!mDevice.validationEnabled() || mDevice.api().vkCmdInsertDebugUtilsLabelEXT == nullptr) {
    return Result::Success;
  }
  VkDebugUtilsLabelEXT info{};
  info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
  info.pLabelName = label;
  mDevice.api().vkCmdInsertDebugUtilsLabelEXT(mCommandBuffer, &info);
  return Result::Success;
}

Result VulkanCommandBuffer::beginDebugMarker(const char* label) {
  if (!mRecording || label == nullptr) return Result::InvalidArgument;
  if (!mDevice.validationEnabled() || mDevice.api().vkCmdBeginDebugUtilsLabelEXT == nullptr) {
    return Result::Success;
  }
  VkDebugUtilsLabelEXT info{};
  info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
  info.pLabelName = label;
  mDevice.api().vkCmdBeginDebugUtilsLabelEXT(mCommandBuffer, &info);
  return Result::Success;
}

Result VulkanCommandBuffer::endDebugMarker() {
  if (!mRecording) return Result::Error;
  if (!mDevice.validationEnabled() || mDevice.api().vkCmdEndDebugUtilsLabelEXT == nullptr) {
    return Result::Success;
  }
  mDevice.api().vkCmdEndDebugUtilsLabelEXT(mCommandBuffer);
  return Result::Success;
}

// ==================================================================================================
// Transfers
// ==================================================================================================

Result VulkanCommandBuffer::writeBuffer(const BufferHandle bufferHandle, const uint64_t offset, const void* data,
        const size_t dataSize) {
  if (!mRecording) return Result::Error;
  if (const Result valid = requireBuffer(bufferHandle, "writeBuffer"); !isSuccess(valid)) return valid;
  if (data == nullptr || dataSize == 0) return Result::InvalidArgument;

  const VulkanBuffer* buffer = mDevice.buffer(bufferHandle);
  if (buffer == nullptr) return Result::InvalidHandle;
  if (offset > buffer->size || dataSize > buffer->size - offset) return Result::InvalidArgument;

  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceSize stagingOffset = 0;
  if (const Result staged = stage(data, dataSize, 4, &staging, &stagingOffset); !isSuccess(staged)) return staged;

  VkBufferCopy copy{};
  copy.srcOffset = stagingOffset;
  copy.dstOffset = offset;
  copy.size = dataSize;
  mDevice.api().vkCmdCopyBuffer(mCommandBuffer, staging, buffer->buffer, 1, &copy);
  memoryBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
  ++mCounts.copies;
  return Result::Success;
}

Result VulkanCommandBuffer::copyBufferToBuffer(const BufferHandle source, const uint64_t sourceOffset,
        const BufferHandle destination, const uint64_t destinationOffset, const uint64_t size) {
  if (!mRecording) return Result::Error;
  const VulkanBuffer* sourceBuffer = mDevice.buffer(source);
  const VulkanBuffer* destinationBuffer = mDevice.buffer(destination);
  if (sourceBuffer == nullptr || destinationBuffer == nullptr) return Result::InvalidHandle;
  if (size == 0) return Result::InvalidArgument;
  if (sourceOffset + size > sourceBuffer->size || destinationOffset + size > destinationBuffer->size) {
    return Result::InvalidArgument;
  }

  VkBufferCopy copy{};
  copy.srcOffset = sourceOffset;
  copy.dstOffset = destinationOffset;
  copy.size = size;
  mDevice.api().vkCmdCopyBuffer(mCommandBuffer, sourceBuffer->buffer, destinationBuffer->buffer, 1, &copy);
  memoryBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
  ++mCounts.copies;
  return Result::Success;
}

Result VulkanCommandBuffer::writeTexture(const TextureHandle textureHandle, const TextureCopyRegion& region,
        const void* data, const uint64_t dataSize, const uint32_t bytesPerRow) {
  if (!mRecording) return Result::Error;
  if (const Result valid = requireTexture(textureHandle, "writeTexture"); !isSuccess(valid)) return valid;
  if (data == nullptr || dataSize == 0) return Result::InvalidArgument;

  const VulkanTexture* texture = mDevice.texture(textureHandle);
  if (texture == nullptr) return Result::InvalidHandle;
  if (isCompressed(texture->format)) {
    mDevice.emitMessage(MessageSeverity::Warning,
            "writeTexture does not support compressed formats; upload the raw blocks through a staging buffer");
    return Result::Unsupported;
  }

  const PixelFormatInfo& format = getPixelFormatInfo(texture->format);
  const uint32_t blockBytes = format.bytesPerBlock;
  if (blockBytes == 0) return Result::Unsupported;

  const uint32_t width = region.size.width;
  const uint32_t height = region.size.height;
  const uint32_t layers = std::max(region.arrayLayerCount, 1u);
  if (width == 0 || height == 0) return Result::InvalidArgument;

  const uint64_t packedRowBytes = static_cast<uint64_t>(width) * blockBytes;
  const uint64_t rowPitch = bytesPerRow != 0 ? bytesPerRow : packedRowBytes;
  const uint64_t required = rowPitch * (height - 1) + packedRowBytes;
  if (required * layers > dataSize) {
    mDevice.emitMessage(MessageSeverity::Error,
            "writeTexture was given less data than the region it describes needs");
    return Result::InvalidArgument;
  }

  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceSize stagingOffset = 0;
  if (const Result staged = stage(data, static_cast<size_t>(dataSize), 4, &staging, &stagingOffset);
      !isSuccess(staged)) {
    return staged;
  }

  VkBufferImageCopy copy{};
  copy.bufferOffset = stagingOffset;
  copy.bufferRowLength = bytesPerRow != 0 ? static_cast<uint32_t>(bytesPerRow / blockBytes) : 0;
  copy.bufferImageHeight = 0;
  copy.imageSubresource = { texture->aspect, region.mipLevel, region.baseArrayLayer, layers };
  copy.imageOffset = { static_cast<int32_t>(region.textureOrigin.x), static_cast<int32_t>(region.textureOrigin.y),
    static_cast<int32_t>(region.textureOrigin.z) };
  copy.imageExtent = { width, height, 1 };

  memoryBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, kAllWrites,
          VK_ACCESS_TRANSFER_WRITE_BIT);
  mDevice.api().vkCmdCopyBufferToImage(mCommandBuffer, staging, texture->image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
  memoryBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
          kAllReads);
  ++mCounts.copies;
  return Result::Success;
}

Result VulkanCommandBuffer::copyBufferToTexture(const BufferTextureCopy& copy) {
  if (!mRecording) return Result::Error;
  const VulkanBuffer* buffer = mDevice.buffer(copy.buffer);
  const VulkanTexture* texture = mDevice.texture(copy.texture);
  if (buffer == nullptr || texture == nullptr) return Result::InvalidHandle;

  const PixelFormatInfo& format = getPixelFormatInfo(texture->format);
  VkBufferImageCopy region{};
  region.bufferOffset = copy.bufferRegion.srcOffset;
  region.bufferRowLength =
          copy.textureRegion.bytesPerRow != 0 && format.bytesPerBlock != 0
                  ? copy.textureRegion.bytesPerRow / format.bytesPerBlock
                  : 0;
  region.bufferImageHeight = copy.textureRegion.rowsPerImage;
  region.imageSubresource = { texture->aspect, copy.textureRegion.mipLevel, copy.textureRegion.baseArrayLayer,
    std::max(copy.textureRegion.arrayLayerCount, 1u) };
  region.imageOffset = { static_cast<int32_t>(copy.textureRegion.textureOrigin.x),
    static_cast<int32_t>(copy.textureRegion.textureOrigin.y),
    static_cast<int32_t>(copy.textureRegion.textureOrigin.z) };
  region.imageExtent = { copy.textureRegion.size.width, copy.textureRegion.size.height,
    texture->type == TextureType::Texture3D ? copy.textureRegion.size.depthOrArrayLayers : 1u };

  memoryBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, kAllWrites,
          VK_ACCESS_TRANSFER_WRITE_BIT);
  mDevice.api().vkCmdCopyBufferToImage(mCommandBuffer, buffer->buffer, texture->image, VK_IMAGE_LAYOUT_GENERAL, 1,
          &region);
  memoryBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
          kAllReads);
  ++mCounts.copies;
  return Result::Success;
}

Result VulkanCommandBuffer::copyTextureToBuffer(const BufferTextureCopy& copy) {
  if (!mRecording) return Result::Error;
  const VulkanBuffer* buffer = mDevice.buffer(copy.buffer);
  const VulkanTexture* texture = mDevice.texture(copy.texture);
  if (buffer == nullptr || texture == nullptr) return Result::InvalidHandle;

  const PixelFormatInfo& format = getPixelFormatInfo(texture->format);
  VkBufferImageCopy region{};
  region.bufferOffset = copy.bufferRegion.dstOffset;
  region.bufferRowLength =
          copy.textureRegion.bytesPerRow != 0 && format.bytesPerBlock != 0
                  ? copy.textureRegion.bytesPerRow / format.bytesPerBlock
                  : 0;
  region.bufferImageHeight = copy.textureRegion.rowsPerImage;
  region.imageSubresource = { texture->aspect, copy.textureRegion.mipLevel, copy.textureRegion.baseArrayLayer,
    std::max(copy.textureRegion.arrayLayerCount, 1u) };
  region.imageOffset = { static_cast<int32_t>(copy.textureRegion.textureOrigin.x),
    static_cast<int32_t>(copy.textureRegion.textureOrigin.y),
    static_cast<int32_t>(copy.textureRegion.textureOrigin.z) };
  region.imageExtent = { copy.textureRegion.size.width, copy.textureRegion.size.height,
    texture->type == TextureType::Texture3D ? copy.textureRegion.size.depthOrArrayLayers : 1u };

  memoryBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, kAllWrites,
          VK_ACCESS_TRANSFER_READ_BIT);
  mDevice.api().vkCmdCopyImageToBuffer(mCommandBuffer, texture->image, VK_IMAGE_LAYOUT_GENERAL, buffer->buffer, 1,
          &region);
  memoryBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_ACCESS_HOST_READ_BIT);
  ++mCounts.copies;
  return Result::Success;
}

Result VulkanCommandBuffer::copyTextureToTexture(const TextureHandle source, const TextureHandle destination,
        const TextureCopyRegion& region) {
  if (!mRecording) return Result::Error;
  const VulkanTexture* sourceTexture = mDevice.texture(source);
  const VulkanTexture* destinationTexture = mDevice.texture(destination);
  if (sourceTexture == nullptr || destinationTexture == nullptr) return Result::InvalidHandle;

  VkImageCopy copy{};
  copy.srcSubresource = { sourceTexture->aspect, region.mipLevel, region.baseArrayLayer,
    std::max(region.arrayLayerCount, 1u) };
  copy.dstSubresource = copy.srcSubresource;
  copy.srcOffset = { static_cast<int32_t>(region.textureOrigin.x), static_cast<int32_t>(region.textureOrigin.y),
    static_cast<int32_t>(region.textureOrigin.z) };
  copy.dstOffset = copy.srcOffset;
  copy.extent = { region.size.width, region.size.height,
    sourceTexture->type == TextureType::Texture3D ? region.size.depthOrArrayLayers : 1u };

  memoryBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, kAllWrites,
          VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
  mDevice.api().vkCmdCopyImage(mCommandBuffer, sourceTexture->image, VK_IMAGE_LAYOUT_GENERAL,
          destinationTexture->image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
  memoryBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
          kAllReads);
  ++mCounts.copies;
  return Result::Success;
}

Result VulkanCommandBuffer::clearColorTexture(const TextureHandle textureHandle, const TextureSubresourceRange& range,
        const Color& color) {
  if (!mRecording) return Result::Error;
  if (const Result valid = requireTexture(textureHandle, "clearColorTexture"); !isSuccess(valid)) return valid;
  const VulkanTexture* texture = mDevice.texture(textureHandle);
  if (texture == nullptr) return Result::InvalidHandle;
  if (texture->depth) return Result::InvalidArgument;

  VkClearColorValue value{};
  value.float32[0] = static_cast<float>(color.r);
  value.float32[1] = static_cast<float>(color.g);
  value.float32[2] = static_cast<float>(color.b);
  value.float32[3] = static_cast<float>(color.a);

  VkImageSubresourceRange subresource{};
  subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  subresource.baseMipLevel = range.baseMipLevel;
  subresource.levelCount = range.mipLevelCount != 0 ? range.mipLevelCount : texture->mipLevels - range.baseMipLevel;
  subresource.baseArrayLayer = range.baseArrayLayer;
  subresource.layerCount = range.arrayLayerCount != 0 ? range.arrayLayerCount
                                                      : texture->arrayLayers - range.baseArrayLayer;

  memoryBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, kAllWrites,
          VK_ACCESS_TRANSFER_WRITE_BIT);
  mDevice.api().vkCmdClearColorImage(mCommandBuffer, texture->image, VK_IMAGE_LAYOUT_GENERAL, &value, 1,
          &subresource);
  memoryBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
          kAllReads);
  ++mCounts.copies;
  return Result::Success;
}

Result VulkanCommandBuffer::clearDepthStencilTexture(const TextureHandle textureHandle,
        const TextureSubresourceRange& range, const DepthStencilValue& value) {
  if (!mRecording) return Result::Error;
  if (const Result valid = requireTexture(textureHandle, "clearDepthStencilTexture"); !isSuccess(valid)) return valid;
  const VulkanTexture* texture = mDevice.texture(textureHandle);
  if (texture == nullptr) return Result::InvalidHandle;
  if (!texture->depth) return Result::InvalidArgument;

  VkClearDepthStencilValue clear{};
  clear.depth = value.depth;
  clear.stencil = value.stencil;

  VkImageSubresourceRange subresource{};
  subresource.aspectMask = texture->aspect;
  subresource.baseMipLevel = range.baseMipLevel;
  subresource.levelCount = range.mipLevelCount != 0 ? range.mipLevelCount : texture->mipLevels - range.baseMipLevel;
  subresource.baseArrayLayer = range.baseArrayLayer;
  subresource.layerCount = range.arrayLayerCount != 0 ? range.arrayLayerCount
                                                      : texture->arrayLayers - range.baseArrayLayer;

  memoryBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, kAllWrites,
          VK_ACCESS_TRANSFER_WRITE_BIT);
  mDevice.api().vkCmdClearDepthStencilImage(mCommandBuffer, texture->image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1,
          &subresource);
  memoryBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
          kAllReads);
  return Result::Success;
}

Result VulkanCommandBuffer::pipelineBarrier() {
  if (!mRecording) return Result::Error;
  memoryBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, kAllWrites, kAllReads);
  return Result::Success;
}

// ==================================================================================================
// Queries
// ==================================================================================================

Result VulkanCommandBuffer::resetQuerySet(const QuerySetHandle querySetHandle, const uint32_t firstQuery,
        const uint32_t count) {
  if (!mRecording) return Result::Error;
  VulkanQuerySet* querySet = mDevice.querySet(querySetHandle);
  if (querySet == nullptr) return Result::InvalidHandle;
  const uint32_t actual = count != 0 ? count : querySet->count - firstQuery;
  mDevice.api().vkCmdResetQueryPool(mCommandBuffer, querySet->pool, firstQuery, actual);
  return Result::Success;
}

Result VulkanCommandBuffer::beginQuery(const QuerySetHandle querySetHandle, const uint32_t query) {
  if (!mRecording) return Result::Error;
  VulkanQuerySet* querySet = mDevice.querySet(querySetHandle);
  if (querySet == nullptr) return Result::InvalidHandle;
  if (querySet->type == QueryType::Timestamp) {
    // A timestamp query has no begin/end pair: the write of the timestamp *is* the whole query. Doing
    // nothing here keeps the same call sequence valid for a caller that does not branch on the type.
    return Result::Success;
  }
  mDevice.api().vkCmdBeginQuery(mCommandBuffer, querySet->pool, query, 0);
  return Result::Success;
}

Result VulkanCommandBuffer::endQuery(const QuerySetHandle querySetHandle, const uint32_t query) {
  if (!mRecording) return Result::Error;
  VulkanQuerySet* querySet = mDevice.querySet(querySetHandle);
  if (querySet == nullptr) return Result::InvalidHandle;
  if (querySet->type == QueryType::Timestamp) {
    mDevice.api().vkCmdWriteTimestamp(mCommandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, querySet->pool, query);
    return Result::Success;
  }
  mDevice.api().vkCmdEndQuery(mCommandBuffer, querySet->pool, query);
  return Result::Success;
}

void VulkanCommandBuffer::setLabel(const char* label) {
  if (label == nullptr) return;
  mLabel = label;
  if (!mRecording || !mDevice.validationEnabled() || mDevice.api().vkSetDebugUtilsObjectNameEXT == nullptr) {
    return;
  }
  VkDebugUtilsObjectNameInfoEXT info{};
  info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
  info.objectType = VK_OBJECT_TYPE_COMMAND_BUFFER;
  info.objectHandle = reinterpret_cast<uint64_t>(mCommandBuffer);
  info.pObjectName = mLabel.c_str();
  mDevice.api().vkSetDebugUtilsObjectNameEXT(mDevice.device(), &info);
}

} // namespace backend::vulkan
