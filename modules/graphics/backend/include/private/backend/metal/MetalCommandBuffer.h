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

#ifndef CORAL_BACKEND_METAL_COMMAND_BUFFER_H
#define CORAL_BACKEND_METAL_COMMAND_BUFFER_H

/**
 * @file MetalCommandBuffer.h
 *
 * The Metal command buffer.
 *
 * ## Recording model
 *
 * Metal records eagerly, like Vulkan and unlike the OpenGL backend's deferred list: every call becomes an encoder call
 * as it is made. What is *not* eager is the encoder's state. The RHI's recording order is "bind pipeline, bind sets,
 * set vertex buffers, draw", while a `MTLRenderCommandEncoder` requires the pipeline state before the first primitive
 * but accepts the rest in any order. Keeping the bindings as recorded state and applying them once before the draw has
 * two benefits: a caller that binds the same set for ten draws costs one call per draw rather than ten, and a binding
 * that arrives after the draw it belongs to cannot silently affect the previous one.
 *
 * ## Suspending a pass
 *
 * Metal allows one encoder at a time. A transfer (or a timestamp sample) recorded inside an open render pass therefore
 * has to end the render encoder, do the blit, and start a new render encoder against the same attachments with
 * `MTLLoadActionLoad`. That is what `suspendRenderPass`/`resumeRenderPass` do, and it is the only place this backend
 * re-creates an encoder: `endRenderPass` still applies the caller's store/resolve actions exactly once.
 */

#include <backend/Device.h>

#include "private/backend/metal/MetalDevice.h"

#import <Metal/Metal.h>

#include <cstdint>
#include <string>
#include <vector>

namespace backend::metal {

class MetalCommandBuffer final : public CommandBuffer {
  public:
  MetalCommandBuffer(MetalDevice& device, const CommandBufferDescriptor& descriptor);
  ~MetalCommandBuffer() override;

  MetalCommandBuffer(const MetalCommandBuffer&) = delete;
  MetalCommandBuffer& operator=(const MetalCommandBuffer&) = delete;

  // --- Lifecycle --------------------------------------------------------------------------------
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

  // --- Backend-internal -------------------------------------------------------------------------
  /** @return true when `end()` has run and the recorded work has not been discarded by `reset()`. */
  [[nodiscard]] bool isEnded() const noexcept { return mEnded; }

  /**
   * Encodes the semaphore and fence signals and commits the native command buffer.
   *
   * With no fence this waits for completion, which is what every RHI submission without a fence promises (the Vulkan
   * backend does the same). With a fence it returns as soon as the command buffer is queued, and `waitForFence` is what
   * observes completion.
   */
  Result submit(FenceHandle fence, SemaphoreHandle waitSemaphore, SemaphoreHandle signalSemaphore);

  /** Discards the recording. Used by the device's destructor, which must not execute unsubmitted work. */
  void abandon();

  private:
  // Encoder management. Each `end*` leaves the corresponding pointer nil; `ensure*` ends whatever is active that
  // cannot coexist and creates the requested encoder.
  Result endRenderEncoder(bool suspend);
  Result endComputeEncoder();
  Result endBlitEncoder();
  Result endAllEncoders(bool suspendRenderPass = true);
  Result ensureRenderEncoder();
  Result ensureComputeEncoder();
  Result ensureBlitEncoder();
  Result suspendRenderPass();
  Result resumeRenderPass();

  Result requireRecording() const;
  Result requireRenderPass() const;
  Result requireRenderPipeline() const;

  /** Applies the recorded pipeline and binding state to the current render encoder. */
  Result applyRenderState();
  /** Applies the recorded descriptor sets and push constants to a compute encoder. */
  void applyComputeBindings(id<MTLComputeCommandEncoder> encoder);
  /** Applies one descriptor set's values to the render encoder at the slots the pipeline layout assigns. */
  void applyDescriptorSet(id<MTLRenderCommandEncoder> encoder, uint32_t setIndex, DescriptorSetHandle set,
          const MetalPipelineLayout& layout);
  void applyDescriptorSetCompute(id<MTLComputeCommandEncoder> encoder, uint32_t setIndex, DescriptorSetHandle set,
          const MetalPipelineLayout& layout);

  /** Creates a native buffer that stays alive until `reset()`, and copies @p data into it. */
  Result stage(const void* data, size_t size, size_t alignment, id<MTLBuffer>* outBuffer, uint64_t* outOffset);

  Result clearTextureWithRenderPass(TextureHandle texture, const TextureSubresourceRange& range,
          const Color& color, bool depthStencil, const DepthStencilValue& depthStencilValue);

  void resetRecordedState();
  void releaseStaging();

  MetalDevice& mDevice;
  CommandBufferDescriptor mDescriptor;
  std::string mLabel;

  id<MTLCommandBuffer> mCommandBuffer = nil;
  id<MTLRenderCommandEncoder> mRenderEncoder = nil;
  id<MTLComputeCommandEncoder> mComputeEncoder = nil;
  id<MTLBlitCommandEncoder> mBlitEncoder = nil;

  bool mRecording = false;
  bool mEnded = false;
  bool mSubmitted = false;

  // Render pass state, kept across a suspend/resume cycle.
  bool mRenderPassOpen = false;
  bool mRenderPassSuspended = false;
  RenderPassBeginInfo mPassInfo = {};
  uint32_t mRenderWidth = 0;
  uint32_t mRenderHeight = 0;
  PixelFormat mDepthStencilFormat = PixelFormat::Undefined;

  // Recorded state, applied lazily at the next draw. The pipeline is kept as a handle rather than a pointer so a
  // pipeline destroyed between the bind and the draw is reported as `InvalidHandle` instead of dereferenced.
  RenderPipelineHandle mRenderPipeline = RenderPipelineHandle::invalid();
  ComputePipelineHandle mComputePipeline = ComputePipelineHandle::invalid();

  DescriptorSetHandle mDescriptorSets[kMaxDescriptorSetLayouts] = {};
  BufferHandle mVertexBuffers[kMaxVertexBufferLayouts] = {};
  uint64_t mVertexOffsets[kMaxVertexBufferLayouts] = {};
  bool mVertexBufferSet[kMaxVertexBufferLayouts] = {};

  BufferHandle mIndexBuffer = BufferHandle::invalid();
  IndexFormat mIndexFormat = IndexFormat::UInt32;
  uint64_t mIndexOffset = 0;
  bool mIndexBufferSet = false;

  Viewport mViewport = {};
  bool mViewportSet = false;
  ScissorRect mScissor = {};
  bool mScissorSet = false;
  bool mBlendConstantSet = false;
  Color mBlendConstant = Color::black();
  bool mStencilReferenceSet = false;
  uint32_t mStencilReference = 0;

  std::vector<uint8_t> mPushConstants;
  bool mPushConstantsSet = false;

  /** Nesting depth of `beginDebugMarker`, so an unmatched `endDebugMarker` is refused rather than crashing. */
  uint32_t mDebugGroupDepth = 0;

  /** Native buffers created for this recording, released only when the recording is reset. */
  std::vector<id<MTLBuffer>> mStagingBuffers;

  struct Counts {
    uint32_t draws = 0;
    uint32_t indexedDraws = 0;
    uint32_t indirectDraws = 0;
    uint32_t dispatches = 0;
    uint32_t copies = 0;
    uint32_t barriers = 0;
  } mCounts;
};

} // namespace backend::metal

#endif // CORAL_BACKEND_METAL_COMMAND_BUFFER_H
