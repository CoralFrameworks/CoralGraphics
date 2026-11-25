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

#ifndef CORAL_BACKEND_GL_COMMAND_BUFFER_H
#define CORAL_BACKEND_GL_COMMAND_BUFFER_H

#include <backend/Device.h>

#include "private/backend/gl/GlDevice.h"

namespace backend {

/**
 * Records GPU work for the OpenGL backend without touching the driver.
 *
 * Every virtual from `CommandBuffer` appends one entry to a `GLCommandList`. Execution happens in
 * `GLDevice::executeCommandBuffer`, which walks the list and issues the corresponding GL calls.
 *
 * ## Why defer instead of calling GL directly
 *
 * It would be shorter to call `glDrawArrays` from `draw()`. Doing so would break three things the RHI
 * promises:
 *
 *  1. **Thread safety.** A GL context is current on one thread at a time, so immediate mode forces
 *     all recording onto that thread. Deferring lets N worker threads record independently.
 *  2. **Backend parity.** Vulkan and Metal cannot execute a command at record time at all. If OpenGL
 *     executed eagerly, upper layer code could accidentally depend on that and stop being portable.
 *  3. **Render pass semantics.** `beginRenderPass` on OpenGL needs to create or bind a framebuffer
 *     object and apply load operations; doing that eagerly would make "record a pass, discard it"
 *     impossible, which is exactly what occlusion-culling passes do.
 *
 * The cost is one `memcpy` of a small POD per command plus one switch dispatch at execution, which is
 * negligible next to the driver call it eventually becomes.
 */
class GLCommandBuffer final : public CommandBuffer {
  public:
  explicit GLCommandBuffer(GLDevice& device, const CommandBufferDescriptor& descriptor);
  ~GLCommandBuffer() override;

  GLCommandBuffer(const GLCommandBuffer&) = delete;
  GLCommandBuffer& operator=(const GLCommandBuffer&) = delete;

  // --- Lifecycle --------------------------------------------------------------------------------

  Result begin() override;
  Result end() override;
  Result reset() override;
  [[nodiscard]] bool isRecording() const noexcept override { return mRecording; }

  /**
   * Prints how many commands of each kind the recorded list holds.
   *
   * A diagnostic, called from `GpuRenderer` when `CORAL_GL_TRACE_COMMANDS` is set. It answers the one question a
   * missing draw leaves open: whether the draw was never recorded, or recorded and then ignored. The renderer's own
   * statistics cannot tell those apart, because it counts draws on the way past rather than in the list.
   */
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

  /** Executes the recorded commands against the GL context. Called from the device. */
  Result replay();

  /** @return the recorded command list; used by tests and by the device for diagnostics. */
  [[nodiscard]] const GLCommandList& commandList() const noexcept { return mCommandList; }

  /** True while the GPU may still be executing a previous submission of this buffer. */
  [[nodiscard]] bool isInFlight() const noexcept { return mInFlight; }

  void setInFlight(const bool inFlight) noexcept { mInFlight = inFlight; }

  [[nodiscard]] const char* label() const noexcept { return mLabel.c_str(); }

  private:
  /** Appends @p command, returning a pointer to the stored copy so callers can fill in fields. */
  GLCommand& pushCommand(GLCommandKind kind);

  /** @return Result::Error when no render pass is open, since drawing outside a pass is undefined. */
  [[nodiscard]] Result requireRenderPass(const char* operation) const;

  /** @return Result::Success when @p handle is live, otherwise Result::InvalidHandle. */
  [[nodiscard]] Result requireTexture(TextureHandle handle, const char* operation) const;
  [[nodiscard]] Result requireBuffer(BufferHandle handle, const char* operation) const;

  GLDevice& mDevice;
  GLCommandList mCommandList;
  CommandBufferDescriptor mDescriptor = {};
  bool mRecording = false;
  bool mRenderPassOpen = false;
  bool mEnded = false;
  bool mInFlight = false;
  std::string mLabel;
};

} // namespace backend

#endif // CORAL_BACKEND_GL_COMMAND_BUFFER_H
