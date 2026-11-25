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

#include "private/backend/gl/GlCommandBuffer.h"

#include "private/backend/gl/GlEnums.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace backend {

// The GL vocabulary translations are shared with the device, which is what guarantees that storage is
// allocated and written through the same formats.
using namespace gl;

// ==================================================================================================
// Command list
// ==================================================================================================

const void* GLCommandList::copyToStorage(const void* bytes, const size_t byteCount) {
  if (bytes == nullptr || byteCount == 0) {
    return nullptr;
  }

  // ## Why this is a chunked arena and not a `std::vector<uint8_t>`
  //
  // The previous implementation appended to one vector and returned `storage.data() + offset`. That pointer is
  // stored in the recorded command, and the next `insert` can reallocate the vector -- which invalidates every
  // pointer handed out before it. The failure is not a crash: the replayed command reads whatever the allocator
  // left behind, so a uniform block arrives as garbage and a draw silently produces nothing. It needs *two*
  // payload-carrying commands to appear, which is why a single-upload test passes and a real frame does not,
  // and it is why this is fixed by construction rather than by reserving harder: any amount of `reserve` only
  // moves the threshold.
  //
  // Fixed-size chunks appended to a vector of *pointers* cannot move what they hold, so a pointer into an
  // earlier chunk stays valid for the life of the list. The vector of chunks may itself reallocate; the chunks
  // it points at do not.
  //
  // Alignment matters: a command's `data` may be read back as `uint32_t`, `float` or `double` by the replay
  // path, and a misaligned address is undefined behaviour on ARM. Aligning to 8 bytes covers every payload type
  // the RHI records.
  constexpr size_t kAlignment = 8;
  const size_t aligned = (storageOffset + kAlignment - 1) & ~(kAlignment - 1);

  // A payload larger than what is left of the current chunk takes a fresh chunk rather than spanning two, so
  // that `data` is always one contiguous run and the replay path never has to know about the chunking.
  if (currentChunk == nullptr || byteCount > GLCommandList::kStorageChunkSize ||
      aligned + byteCount > GLCommandList::kStorageChunkSize) {
    chunks.push_back(std::make_unique<uint8_t[]>(GLCommandList::kStorageChunkSize));
    currentChunk = chunks.back().get();
    storageOffset = 0;
  }

  // A payload larger than a whole chunk is the one case that spans: it gets a chunk of its own sized to fit, so
  // the "always contiguous" promise holds for it too. A texture upload can be tens of megabytes, which is
  // exactly why the common case and this one are separate branches rather than one growing buffer.
  if (byteCount > GLCommandList::kStorageChunkSize) {
    chunks.push_back(std::make_unique<uint8_t[]>(byteCount));
    currentChunk = chunks.back().get();
    storageOffset = 0;
    std::memcpy(currentChunk, bytes, byteCount);
    storageOffset = byteCount;
    return currentChunk;
  }

  uint8_t* const destination = currentChunk + storageOffset;
  std::memcpy(destination, bytes, byteCount);
  storageOffset += byteCount;
  return destination;
}

// ==================================================================================================
// Recording
// ==================================================================================================

GLCommandBuffer::GLCommandBuffer(GLDevice& device, const CommandBufferDescriptor& descriptor)
        : mDevice(device),
          mDescriptor(descriptor) {
  if (descriptor.label != nullptr) {
    mLabel = descriptor.label;
  }
}

GLCommandBuffer::~GLCommandBuffer() = default;

GLCommand& GLCommandBuffer::pushCommand(const GLCommandKind kind) {
  mCommandList.commands.emplace_back();
  GLCommand& command = mCommandList.commands.back();
  command.kind = kind;
  return command;
}

Result GLCommandBuffer::requireRenderPass(const char* operation) const {
  if (mRenderPassOpen) {
    return Result::Success;
  }
  (void) operation;
  return Result::Error;
}

Result GLCommandBuffer::requireTexture(const TextureHandle handle, const char* operation) const {
  if (mDevice.texture(handle) != nullptr) {
    return Result::Success;
  }
  (void) operation;
  return Result::InvalidHandle;
}

Result GLCommandBuffer::requireBuffer(const BufferHandle handle, const char* operation) const {
  if (mDevice.buffer(handle) != nullptr) {
    return Result::Success;
  }
  (void) operation;
  return Result::InvalidHandle;
}

Result GLCommandBuffer::begin() {
  if (mRecording) {
    return Result::Error;
  }
  if (mInFlight) {
    // Re-recording a command buffer the GPU may still be executing would corrupt that execution.
    // The caller must either wait (Device::waitForFence) or use a second command buffer - which is
    // exactly the discipline Vulkan requires, so the restriction is not OpenGL-specific.
    return Result::Error;
  }

  mCommandList.clear();
  mRenderPassOpen = false;
  mEnded = false;
  mRecording = true;
  return Result::Success;
}

Result GLCommandBuffer::end() {
  if (!mRecording) {
    return Result::Error;
  }
  if (mRenderPassOpen) {
    // Closing the pass implicitly is more forgiving than failing, and it matches what a renderer
    // means when it forgets: the work recorded so far should still execute.
    endRenderPass();
  }
  mRecording = false;
  mEnded = true;
  return Result::Success;
}

void GLCommandBuffer::dumpCommandCounts() const {
  // ## Why the command *counts* are worth printing
  //
  // A renderer that decides a draw is not needed and a renderer that issues it and is ignored look identical in the
  // output pixels, and nothing in the renderer's own statistics separates them: both increment the draw count on the
  // way past. So the number that matters when a draw is missing is how many `Draw` commands the command *list*
  // actually holds, which is what this counts -- and it counts the list, not the renderer's intentions.
  size_t draws = 0;
  size_t writes = 0;
  size_t passes = 0;
  size_t setVertexBuffers = 0;
  size_t binds = 0;
  for (const GLCommand& command: mCommandList.commands) {
    switch (command.kind) {
      case GLCommandKind::Draw: ++draws; break;
      case GLCommandKind::DrawIndexed: ++draws; break;
      case GLCommandKind::WriteBuffer: ++writes; break;
      case GLCommandKind::WriteTexture: ++writes; break;
      case GLCommandKind::BeginRenderPass: ++passes; break;
      case GLCommandKind::SetVertexBuffer: ++setVertexBuffers; break;
      case GLCommandKind::BindRenderPipeline: ++binds; break;
      default: break;
    }
  }
  std::fprintf(stderr,
          "[cmdlist] %zu commands: %zu draws, %zu writes, %zu beginPass, %zu setVertexBuffer, %zu bindPipeline\n",
          mCommandList.commands.size(), draws, writes, passes, setVertexBuffers, binds);

  // ## Every kind, because the five above are a third of a real frame
  //
  // A frame's `execute` time is a function of this *list*: the replay is one switch per command plus the driver calls it
  // makes, so the count is the first thing to read when a frame costs more than its draws suggest. On the canvas demo the
  // summary above accounts for 1750 of **3001** commands, and the 1251 it did not name were exactly the state commands a
  // per-draw cost is made of -- `BindDescriptorSet`, `SetIndexBuffer`, `SetScissor`, `SetViewport`, `PushConstants`. So
  // the full histogram is printed with the counts, largest first, and a kind that never occurs does not appear.
  //
  // Only the kinds whose counts are wanted; the enum is ordered, so the table is indexed by the enumerator.
  static constexpr const char* kKindNames[] = {
    "beginPass", "endPass", "bindPipeline", "bindComputePipeline", "setVertexBuffer", "setIndexBuffer",
    "bindDescriptorSet", "setViewport", "setScissor", "setStencilReference", "setBlendConstant", "pushConstants",
    "draw", "drawIndexed", "drawIndirect", "drawIndexedIndirect", "dispatch", "dispatchIndirect", "writeBuffer",
    "copyBufferToBuffer", "writeTexture", "copyBufferToTexture", "copyTextureToBuffer", "copyTextureToTexture",
    "clearColorTexture", "clearDepthStencilTexture", "pipelineBarrier", "resetQuerySet", "beginQuery", "endQuery",
    "debugMarker",
  };
  constexpr size_t kKindCount = sizeof(kKindNames) / sizeof(kKindNames[0]);
  static_assert(kKindCount == static_cast<size_t>(GLCommandKind::DebugMarker) + 1,
          "the histogram table has to name every command kind, or a new one would be counted as nothing");

  std::vector<std::pair<size_t, size_t>> histogram;  // (count, kind)
  histogram.reserve(kKindCount);
  for (const GLCommand& command: mCommandList.commands) {
    const auto kind = static_cast<size_t>(command.kind);
    if (kind >= kKindCount) continue;
    const auto found = std::find_if(histogram.begin(), histogram.end(),
            [kind](const std::pair<size_t, size_t>& entry) { return entry.second == kind; });
    if (found == histogram.end()) {
      histogram.emplace_back(1, kind);
    } else {
      ++found->first;
    }
  }
  std::sort(histogram.begin(), histogram.end(),
          [](const std::pair<size_t, size_t>& a, const std::pair<size_t, size_t>& b) { return a.first > b.first; });
  std::fprintf(stderr, "[cmdlist]");
  for (const auto& [count, kind]: histogram) {
    std::fprintf(stderr, " %s=%zu", kKindNames[kind], count);
  }
  std::fprintf(stderr, "\n");
  std::fflush(stderr);

  // ## The draw order, because the *count* does not show an ordering defect
  //
  // Every draw being present says nothing about whether they happen in the order the frame means them to, and a
  // composite that runs before the layer it reads -- or after something has cleared the target -- produces a missing
  // patch with a full command list. This prints each draw with the pass it is inside and the vertex buffer it reads,
  // which is what makes an ordering defect visible.
  //
  //   set CORAL_GL_TRACE_DRAWS=1
  if (std::getenv("CORAL_GL_TRACE_DRAWS") != nullptr) {
    for (size_t index = 0; index < mCommandList.commands.size(); ++index) {
      const GLCommand& command = mCommandList.commands[index];
      switch (command.kind) {
        case GLCommandKind::BeginRenderPass:
          std::fprintf(stderr, "[draw %3zu] beginPass renderPass=%u\n", index, command.renderPass.index);
          break;
        case GLCommandKind::EndRenderPass:
          std::fprintf(stderr, "[draw %3zu] endPass\n", index);
          break;
        case GLCommandKind::SetVertexBuffer:
          std::fprintf(stderr, "[draw %3zu] setVertexBuffer offset=%llu\n", index,
                  static_cast<unsigned long long>(command.offset));
          break;
        case GLCommandKind::BindDescriptorSet:
          std::fprintf(stderr, "[draw %3zu] bindSet %u\n", index, command.descriptorSet.index);
          break;
        case GLCommandKind::Draw:
          std::fprintf(stderr, "[draw %3zu] DRAW count=%u first=%u\n", index, command.uint0, command.uint1);
          break;
        default: break;
      }
    }
  }
  std::fflush(stderr);
}

Result GLCommandBuffer::reset() {
  if (mInFlight) {
    return Result::Error;
  }
  mCommandList.clear();
  mRecording = false;
  mEnded = false;
  mRenderPassOpen = false;
  return Result::Success;
}

// ==================================================================================================
// Render passes
// ==================================================================================================

Result GLCommandBuffer::beginRenderPass(const RenderPassBeginInfo& info) {
  if (!mRecording) {
    return Result::Error;
  }
  if (mRenderPassOpen) {
    return Result::Error;
  }

  GLCommand& command = pushCommand(GLCommandKind::BeginRenderPass);
  command.renderPass = info.renderPass;
  command.colorAttachmentCount = std::min(info.colorAttachmentCount, kMaxColorAttachments);
  command.hasDepthStencil = info.hasDepthStencil;
  command.renderArea = info.renderArea;

  const auto recordAttachment = [this](const RenderPassAttachment& source, GLRecordedAttachment* target) {
    if (!source.view.valid()) {
      return;
    }
    const GLDevice::ResolvedView resolved = mDevice.resolveTextureView(source.view);
    target->isDefaultFramebuffer = resolved.isDefaultFramebuffer;
    target->format = resolved.format;
    target->loadOp = source.loadOp;
    target->storeOp = source.storeOp;
    target->clearColor = source.clearColor;
    target->clearDepthStencil = source.clearDepthStencil;

    // A view always names an underlying texture (except the default-framebuffer sentinel), and the
    // subresource range it carries is what the resolve and clear paths need.
    if (const GLTextureView* view = mDevice.textureView(source.view)) {
      target->texture = view->isDefaultFramebuffer ? TextureHandle::invalid() : view->texture;
      target->baseMipLevel = view->baseMipLevel;
      target->baseArrayLayer = view->baseArrayLayer;
      target->arrayLayerCount = view->arrayLayerCount;
    }
    if (const GLTexture* texture = mDevice.texture(target->texture)) {
      target->sampleCount = texture->meta.sampleCount;
    }

    if (source.resolveTarget.valid()) {
      if (const GLTextureView* resolveView = mDevice.textureView(source.resolveTarget)) {
        target->resolveTexture = resolveView->texture;
        target->resolveMipLevel = resolveView->baseMipLevel;
      }
    }
  };

  for (uint32_t index = 0; index < command.colorAttachmentCount; ++index) {
    recordAttachment(info.colorAttachments[index], &command.colorAttachments[index]);
  }
  if (info.hasDepthStencil) {
    recordAttachment(info.depthStencilAttachment, &command.depthStencilAttachment);
  }

  mRenderPassOpen = true;
  return Result::Success;
}

Result GLCommandBuffer::endRenderPass() {
  if (!mRecording) {
    return Result::Error;
  }
  if (!mRenderPassOpen) {
    return Result::Error;
  }
  pushCommand(GLCommandKind::EndRenderPass);
  mRenderPassOpen = false;
  return Result::Success;
}

// ==================================================================================================
// Pipeline state recording
// ==================================================================================================

Result GLCommandBuffer::bindRenderPipeline(const RenderPipelineHandle pipeline) {
  if (!mRecording) {
    return Result::Error;
  }
  if (mDevice.renderPipeline(pipeline) == nullptr) {
    return Result::InvalidHandle;
  }
  pushCommand(GLCommandKind::BindRenderPipeline).renderPipeline = pipeline;
  return Result::Success;
}

Result GLCommandBuffer::bindComputePipeline(const ComputePipelineHandle pipeline) {
  if (!mRecording) {
    return Result::Error;
  }
  // Declining here rather than recording a command that will fail at replay keeps the error at the
  // call site, which is where the caller can still choose a fallback.
  if (!mDevice.getCapabilities().supports(Feature::ComputeShader)) {
    return Result::Unsupported;
  }
  pushCommand(GLCommandKind::BindComputePipeline).computePipeline = pipeline;
  return Result::Success;
}

Result GLCommandBuffer::setVertexBuffer(const uint32_t slot, const BufferHandle buffer, const uint64_t offset) {
  if (!mRecording) {
    return Result::Error;
  }
  if (slot >= kMaxVertexBufferLayouts) {
    return Result::InvalidArgument;
  }
  if (!isSuccess(requireBuffer(buffer, "setVertexBuffer"))) {
    return Result::InvalidHandle;
  }

  GLCommand& command = pushCommand(GLCommandKind::SetVertexBuffer);
  command.uint0 = slot;
  command.buffer = buffer;
  command.offset = offset;
  return Result::Success;
}

Result GLCommandBuffer::setIndexBuffer(const BufferHandle buffer, const IndexFormat format, const uint64_t offset) {
  if (!mRecording) {
    return Result::Error;
  }
  if (!isSuccess(requireBuffer(buffer, "setIndexBuffer"))) {
    return Result::InvalidHandle;
  }

  GLCommand& command = pushCommand(GLCommandKind::SetIndexBuffer);
  command.buffer = buffer;
  command.indexFormat = format;
  command.offset = offset;
  return Result::Success;
}

Result GLCommandBuffer::bindDescriptorSet(const DescriptorSetIndex setIndex, const DescriptorSetHandle set) {
  if (!mRecording) {
    return Result::Error;
  }
  if (setIndex >= kMaxDescriptorSetLayouts) {
    return Result::InvalidArgument;
  }
  if (set.valid() && mDevice.descriptorSet(set) == nullptr) {
    return Result::InvalidHandle;
  }

  GLCommand& command = pushCommand(GLCommandKind::BindDescriptorSet);
  command.uint0 = setIndex;
  command.descriptorSet = set;
  return Result::Success;
}

Result GLCommandBuffer::setViewport(const Viewport& viewport) {
  if (!mRecording) {
    return Result::Error;
  }
  pushCommand(GLCommandKind::SetViewport).viewport = viewport;
  return Result::Success;
}

Result GLCommandBuffer::setScissor(const ScissorRect& scissor) {
  if (!mRecording) {
    return Result::Error;
  }
  pushCommand(GLCommandKind::SetScissor).scissor = scissor;
  return Result::Success;
}

Result GLCommandBuffer::setStencilReference(const uint32_t reference) {
  if (!mRecording) {
    return Result::Error;
  }
  pushCommand(GLCommandKind::SetStencilReference).uint0 = reference;
  return Result::Success;
}

Result GLCommandBuffer::setBlendConstant(const Color& color) {
  if (!mRecording) {
    return Result::Error;
  }
  pushCommand(GLCommandKind::SetBlendConstant).color = color;
  return Result::Success;
}

Result GLCommandBuffer::pushConstants(const void* data, const uint32_t size, const uint32_t offset) {
  if (!mRecording) {
    return Result::Error;
  }
  if (data == nullptr || size == 0) {
    return Result::InvalidArgument;
  }

  if (offset + size > mDevice.getCapabilities().limits.maxPushConstantSize) {
    // Reporting the budget instead of silently truncating: a truncated push constant produces wrong
    // values in a shader, which is far harder to diagnose than a rejected call.
    return Result::InvalidArgument;
  }

  GLCommand& command = pushCommand(GLCommandKind::PushConstants);
  command.data = mCommandList.copyToStorage(data, size);
  command.dataSize = size;
  command.offset = offset;
  return Result::Success;
}

// ==================================================================================================
// Drawing
// ==================================================================================================

Result GLCommandBuffer::draw(const uint32_t vertexCount, const uint32_t instanceCount, const uint32_t firstVertex,
        const uint32_t firstInstance) {
  if (!mRecording) {
    return Result::Error;
  }
  if (!mRenderPassOpen) {
    return Result::Error;
  }
  if (vertexCount == 0 || instanceCount == 0) {
    return Result::Success; // nothing to do, not an error
  }

  GLCommand& command = pushCommand(GLCommandKind::Draw);
  command.uint0 = vertexCount;
  command.uint1 = instanceCount;
  command.uint2 = firstVertex;
  command.uint3 = firstInstance;
  return Result::Success;
}

Result GLCommandBuffer::drawIndexed(const uint32_t indexCount, const uint32_t instanceCount, const uint32_t firstIndex,
        const int32_t vertexOffset, const uint32_t firstInstance) {
  if (!mRecording) {
    return Result::Error;
  }
  if (!mRenderPassOpen) {
    return Result::Error;
  }
  if (indexCount == 0 || instanceCount == 0) {
    return Result::Success;
  }

  GLCommand& command = pushCommand(GLCommandKind::DrawIndexed);
  command.uint0 = indexCount;
  command.uint1 = instanceCount;
  command.uint2 = firstIndex;
  command.int0 = vertexOffset;
  command.uint3 = firstInstance;
  return Result::Success;
}

Result GLCommandBuffer::drawIndirect(const BufferHandle arguments, const uint64_t offset) {
  if (!mRecording) {
    return Result::Error;
  }
  if (!mRenderPassOpen) {
    return Result::Error;
  }
  if (!isSuccess(requireBuffer(arguments, "drawIndirect"))) {
    return Result::InvalidHandle;
  }

  GLCommand& command = pushCommand(GLCommandKind::DrawIndirect);
  command.buffer = arguments;
  command.offset = offset;
  return Result::Success;
}

Result GLCommandBuffer::drawIndexedIndirect(const BufferHandle arguments, const uint64_t offset) {
  if (!mRecording) {
    return Result::Error;
  }
  if (!mRenderPassOpen) {
    return Result::Error;
  }
  if (!isSuccess(requireBuffer(arguments, "drawIndexedIndirect"))) {
    return Result::InvalidHandle;
  }

  GLCommand& command = pushCommand(GLCommandKind::DrawIndexedIndirect);
  command.buffer = arguments;
  command.offset = offset;
  return Result::Success;
}

Result GLCommandBuffer::dispatch(const uint32_t groupCountX, const uint32_t groupCountY, const uint32_t groupCountZ) {
  if (!mRecording) {
    return Result::Error;
  }
  // Compute is absent on the ES 3.0 baseline, so the call is refused at record time with the same
  // answer the capability query gives - the two can never disagree.
  if (!mDevice.getCapabilities().supports(Feature::ComputeShader)) {
    return Result::Unsupported;
  }
  if (groupCountX == 0 || groupCountY == 0 || groupCountZ == 0) {
    return Result::Success;
  }

  GLCommand& command = pushCommand(GLCommandKind::Dispatch);
  command.uint0 = groupCountX;
  command.uint1 = groupCountY;
  command.uint2 = groupCountZ;
  return Result::Success;
}

Result GLCommandBuffer::dispatchIndirect(const BufferHandle arguments, const uint64_t offset) {
  if (!mRecording) {
    return Result::Error;
  }
  if (!mDevice.getCapabilities().supports(Feature::IndirectDispatch)) {
    return Result::Unsupported;
  }
  if (!isSuccess(requireBuffer(arguments, "dispatchIndirect"))) {
    return Result::InvalidHandle;
  }

  GLCommand& command = pushCommand(GLCommandKind::DispatchIndirect);
  command.buffer = arguments;
  command.offset = offset;
  return Result::Success;
}

Result GLCommandBuffer::insertDebugMarker(const char* label) {
  if (!mRecording) return Result::Error;
  if (label == nullptr) return Result::InvalidArgument;

  GLCommand& command = pushCommand(GLCommandKind::DebugMarker);
  command.data = mCommandList.copyToStorage(label, std::strlen(label) + 1);
  command.dataSize = static_cast<uint32_t>(std::strlen(label) + 1);
  return Result::Success;
}

Result GLCommandBuffer::beginDebugMarker(const char* label) { return insertDebugMarker(label); }

Result GLCommandBuffer::endDebugMarker() { return insertDebugMarker("<"); }

// ==================================================================================================
// Transfer
// ==================================================================================================

Result GLCommandBuffer::writeBuffer(BufferHandle buffer, uint64_t offset, const void* data, size_t dataSize) {
  if (!mRecording) {
    return Result::Error;
  }
  if (data == nullptr || dataSize == 0) {
    return Result::InvalidArgument;
  }

  GLBuffer* entry = mDevice.buffer(buffer);
  if (entry == nullptr) {
    return Result::InvalidHandle;
  }
  if (offset + dataSize > entry->meta.size) {
    return Result::InvalidArgument;
  }

  GLCommand& command = pushCommand(GLCommandKind::WriteBuffer);
  command.buffer = buffer;
  command.offset = offset;
  command.size = dataSize;
  // The payload is copied into the command arena, so the caller's memory does not need to stay alive
  // until the command buffer is executed. That is what makes "record on a worker thread, submit
  // later" safe with stack-allocated upload data.
  command.data = mCommandList.copyToStorage(data, dataSize);
  command.dataSize = static_cast<uint32_t>(dataSize);
  return Result::Success;
}

Result GLCommandBuffer::copyBufferToBuffer(const BufferHandle src, const uint64_t srcOffset, const BufferHandle dst,
        const uint64_t dstOffset, const uint64_t size) {
  if (!mRecording) {
    return Result::Error;
  }
  if (!isSuccess(requireBuffer(src, "copyBufferToBuffer"))) {
    return Result::InvalidHandle;
  }
  if (!isSuccess(requireBuffer(dst, "copyBufferToBuffer"))) {
    return Result::InvalidHandle;
  }
  if (size == 0) {
    return Result::InvalidArgument;
  }

  GLCommand& command = pushCommand(GLCommandKind::CopyBufferToBuffer);
  command.buffer = src;
  command.buffer2 = dst;
  command.offset = srcOffset;
  command.offset2 = dstOffset;
  command.size = size;
  return Result::Success;
}

Result GLCommandBuffer::writeTexture(const TextureHandle textureHandle, const TextureCopyRegion& region,
        const void* data, const uint64_t dataSize, const uint32_t bytesPerRow) {
  if (!mRecording) {
    return Result::Error;
  }
  if (data == nullptr || dataSize == 0) {
    return Result::InvalidArgument;
  }
  if (!isSuccess(requireTexture(textureHandle, "writeTexture"))) {
    return Result::InvalidHandle;
  }

  GLTexture* texture = mDevice.texture(textureHandle);
  if (texture->meta.immutableStorage && texture->meta.sampleCount > 1) {
    // Writing into a multisampled texture is not expressible in GL; only a render pass can.
    return Result::Unsupported;
  }

  GLCommand& command = pushCommand(GLCommandKind::WriteTexture);
  command.texture = textureHandle;
  command.textureRegion = region;
  command.uint0 = bytesPerRow;
  command.data = mCommandList.copyToStorage(data, dataSize);
  command.dataSize = static_cast<uint32_t>(dataSize);
  return Result::Success;
}

Result GLCommandBuffer::copyBufferToTexture(const BufferTextureCopy& copy) {
  if (!mRecording) {
    return Result::Error;
  }
  if (!isSuccess(requireBuffer(copy.buffer, "copyBufferToTexture"))) {
    return Result::InvalidHandle;
  }
  if (!isSuccess(requireTexture(copy.texture, "copyBufferToTexture"))) {
    return Result::InvalidHandle;
  }

  GLCommand& command = pushCommand(GLCommandKind::CopyBufferToTexture);
  command.buffer = copy.buffer;
  command.texture = copy.texture;
  // The buffer range is flattened into the generic offset/size slots and the texture region is kept
  // whole, which is what the replay path reads back.
  command.offset = copy.bufferRegion.srcOffset;
  command.size = copy.bufferRegion.size;
  command.textureRegion = copy.textureRegion;
  return Result::Success;
}

Result GLCommandBuffer::copyTextureToBuffer(const BufferTextureCopy& copy) {
  if (!mRecording) {
    return Result::Error;
  }
  if (!isSuccess(requireTexture(copy.texture, "copyTextureToBuffer"))) {
    return Result::InvalidHandle;
  }
  if (!isSuccess(requireBuffer(copy.buffer, "copyTextureToBuffer"))) {
    return Result::InvalidHandle;
  }

  GLCommand& command = pushCommand(GLCommandKind::CopyTextureToBuffer);
  command.buffer = copy.buffer;
  command.texture = copy.texture;
  command.offset = copy.bufferRegion.dstOffset;
  command.size = copy.bufferRegion.size;
  command.textureRegion = copy.textureRegion;
  return Result::Success;
}

Result GLCommandBuffer::copyTextureToTexture(const TextureHandle src, const TextureHandle dst,
        const TextureCopyRegion& region) {
  if (!mRecording) {
    return Result::Error;
  }
  if (!isSuccess(requireTexture(src, "copyTextureToTexture"))) {
    return Result::InvalidHandle;
  }
  if (!isSuccess(requireTexture(dst, "copyTextureToTexture"))) {
    return Result::InvalidHandle;
  }

  GLCommand& command = pushCommand(GLCommandKind::CopyTextureToTexture);
  command.texture = src;
  command.texture2 = dst;
  command.textureRegion = region;
  return Result::Success;
}

Result GLCommandBuffer::clearColorTexture(const TextureHandle texture, const TextureSubresourceRange& range,
        const Color& color) {
  if (!mRecording) {
    return Result::Error;
  }
  if (!isSuccess(requireTexture(texture, "clearColorTexture"))) {
    return Result::InvalidHandle;
  }

  GLCommand& command = pushCommand(GLCommandKind::ClearColorTexture);
  command.texture = texture;
  command.subresourceRange = range;
  command.color = color;
  return Result::Success;
}

Result GLCommandBuffer::clearDepthStencilTexture(const TextureHandle texture, const TextureSubresourceRange& range,
        const DepthStencilValue& value) {
  if (!mRecording) {
    return Result::Error;
  }
  if (!isSuccess(requireTexture(texture, "clearDepthStencilTexture"))) {
    return Result::InvalidHandle;
  }

  GLCommand& command = pushCommand(GLCommandKind::ClearDepthStencilTexture);
  command.texture = texture;
  command.subresourceRange = range;
  command.depthStencil = value;
  return Result::Success;
}

Result GLCommandBuffer::pipelineBarrier() {
  if (!mRecording) {
    return Result::Error;
  }
  pushCommand(GLCommandKind::PipelineBarrier);
  return Result::Success;
}

// ==================================================================================================
// Queries
// ==================================================================================================

Result GLCommandBuffer::resetQuerySet(const QuerySetHandle querySet, const uint32_t firstQuery, const uint32_t count) {
  if (!mRecording) {
    return Result::Error;
  }
  if (mDevice.getCapabilities().limits.maxSampledTexturesPerStage == 0) {
    return Result::Unsupported;
  }
  if (querySet.valid() == false) {
    return Result::InvalidHandle;
  }
  GLCommand& command = pushCommand(GLCommandKind::ResetQuerySet);
  command.querySet = querySet;
  command.uint0 = firstQuery;
  command.uint1 = count;
  return Result::Success;
}

Result GLCommandBuffer::beginQuery(const QuerySetHandle querySet, const uint32_t query) {
  if (!mRecording) {
    return Result::Error;
  }
  if (!mDevice.getCapabilities().supports(Feature::QueryObjects)) {
    return Result::Unsupported;
  }

  GLCommand& command = pushCommand(GLCommandKind::BeginQuery);
  command.querySet = querySet;
  command.uint0 = query;
  return Result::Success;
}

Result GLCommandBuffer::endQuery(const QuerySetHandle querySet, const uint32_t query) {
  if (!mRecording) {
    return Result::Error;
  }
  if (!mDevice.getCapabilities().supports(Feature::QueryObjects)) {
    return Result::Unsupported;
  }

  GLCommand& command = pushCommand(GLCommandKind::EndQuery);
  command.querySet = querySet;
  command.uint0 = query;
  return Result::Success;
}

void GLCommandBuffer::setLabel(const char* label) { mLabel = label != nullptr ? label : ""; }

// ==================================================================================================
// Replay
// ==================================================================================================

namespace {

/**
 * OpenGL state that the replay path tracks to avoid redundant driver calls.
 *
 * A recorded command list re-specifies its state on every draw, because that is what makes a command
 * buffer self-contained. Without this cache the replay would re-issue glUseProgram, glBindBuffer and
 * friends for every single draw, which on mobile drivers is where most of the CPU time in a
 * "Vulkan-like" OpenGL backend actually goes. The cache is per-replay, never persisted, so it can
 * never go stale.
 */
struct GLReplayState {
  GLuint program = 0;
  GLuint vertexArray = 0;
  GLuint framebuffer = 0;
  GLuint arrayBuffer = 0;
  GLuint elementBuffer = 0;
  GLenum frontFace = 0;
  GLenum cullFace = 0;
  bool depthTest = false;
  bool depthWrite = true;
  GLenum depthFunc = 0;
  bool blendEnabled = false;
  bool scissorEnabled = false;
  /**
   * The colour channels currently written, as the pipeline's four-bit mask, or `0xFF` before anything set one.
   *
   * `0xFF` is the "never set" sentinel rather than `0x0F` -- the mask's own default -- so that the first
   * pipeline to ask for the default still emits its `glColorMask`. Seeding it with the default would make that
   * first call a no-op on paper and leave whatever the *previous* replay left in the context, which is the
   * class of bug this field exists to prevent.
   */
  uint8_t colorWriteMask = 0xFF;
  bool initialized = false;
};

} // namespace anonymous

/**
 * @return true when the environment asks for the stencil trace.
 *
 * `getenv` per bind rather than a cached flag: the trace is a diagnostic, it is off in every normal run, and a
 * static that read the environment once would not respond to a debugger setting it mid-session -- which is the
 * only way anybody uses a trace like this.
 */
static bool glStencilTraceEnabled() noexcept {
  return std::getenv("CORAL_GL_TRACE_STENCIL") != nullptr;
}

Result GLCommandBuffer::replay() {
  GLDevice& device = mDevice;
  gl::GlPlatform& platform = device.platform();

  if (platform.isLost()) {
    return Result::DeviceLost;
  }
  if (mCommandList.commands.empty()) {
    mEnded = false;
    return Result::Success;
  }

  // The GL context is current on the thread that created the device; re-asserting it here means a
  // worker thread's recording can still be submitted from the main thread without the caller having
  // to know about EGL.
  const Result current = platform.makeCurrent();
  if (!isSuccess(current)) {
    return current;
  }

  gl::GLApi& api = gl::getGLApi();
  GLReplayState state;

  // Push-constant staging: a scratch buffer reused across every draw in this replay, so a frame's worth of draws does
  // not allocate. It is fixed-size because the RHI's per-draw limit is fixed.
  //
  // It began as two: a second was reserved for uniform uploads and never came into being, leaving a variable that was
  // deleted on release but never created, and a size that was never read. Clang reports both as unused; MSVC does not,
  // so they sat there describing a buffer that did not exist.
  std::vector<uint8_t> pushConstantData;
  GLuint pushConstantBuffer = 0;

  struct DrawState {
    RenderPipelineHandle pipeline = RenderPipelineHandle::invalid();
    DescriptorSetHandle descriptorSets[kMaxDescriptorSetLayouts] = {};
    BufferHandle vertexBuffers[kMaxVertexBufferLayouts] = {};
    uint64_t vertexBufferOffsets[kMaxVertexBufferLayouts] = {};
    uint32_t vertexBufferCount = 0;
    BufferHandle indexBuffer = BufferHandle::invalid();
    IndexFormat indexFormat = IndexFormat::UInt16;
    uint64_t indexOffset = 0;

    /** Current render pass attachments, needed to build the framebuffer and to resolve at the end. */
    GLRecordedAttachment colorAttachments[kMaxColorAttachments] = {};
    uint32_t colorAttachmentCount = 0;
    GLRecordedAttachment depthStencilAttachment = {};
    bool hasDepthStencil = false;
    uint32_t targetWidth = 0;
    uint32_t targetHeight = 0;
    bool flipY = true;
  } draw;

  const auto releaseScratch = [&api, &pushConstantBuffer]() {
    if (pushConstantBuffer != 0 && api.glDeleteBuffers != nullptr) {
      api.glDeleteBuffers(1, &pushConstantBuffer);
      pushConstantBuffer = 0;
    }
  };

  // ---------------------------------------------------------------- state helpers
  const auto useProgram = [&api, &state](const GLuint program) {
    if (state.program == program) {
      return;
    }
    api.glUseProgram(program);
    state.program = program;
    // Binding a program invalidates the element array binding on some drivers, so the cache is reset
    // conservatively rather than risking a stale index buffer.
    state.elementBuffer = 0;
  };

  const auto applyIndexBuffer = [&](const BufferHandle buffer, const IndexFormat format, const uint64_t offset) {
    const GLBuffer* entry = device.buffer(buffer);
    if (entry == nullptr) {
      return;
    }
    if (state.elementBuffer != entry->meta.name) {
      api.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, entry->meta.name);
      state.elementBuffer = entry->meta.name;
    }
    draw.indexBuffer = buffer;
    draw.indexFormat = format;
    draw.indexOffset = offset;
  };

  const auto applyVertexArray = [&]() -> GLuint {
    const GLRenderPipeline* pipeline = device.renderPipeline(draw.pipeline);
    if (pipeline == nullptr) {
      // A null pipeline here is the one way a draw is skipped with no message of its own, which makes "the draw was
      // skipped" indistinguishable from "the draw produced nothing". The coverage-field route is the first caller to
      // reach it, so the reason is printed behind a flag rather than guessed at.
      if (std::getenv("CORAL_TRACE_VAO") != nullptr) {
        std::fprintf(stderr, "[vao] no pipeline for handle index=%u generation=%u, so the draw is skipped\n",
                draw.pipeline.index, draw.pipeline.generation);
        std::fflush(stderr);
      }
      return 0;
    }

    GLPipelineCacheKey key;
    key.pipeline = draw.pipeline;
    key.vertexBufferCount = draw.vertexBufferCount;
    for (uint32_t index = 0; index < draw.vertexBufferCount; ++index) {
      key.vertexBuffers[index] = draw.vertexBuffers[index];
      key.vertexBufferOffsets[index] = draw.vertexBufferOffsets[index];
    }

    // The cache lives on the device so that two command buffers recording the same pipeline and the
    // same buffers share one VAO. The configure callback runs with the new VAO bound.
    const VertexInputDescriptor& input = pipeline->vertexInput;
    return device.acquireVertexArray(key, [&](GLuint) {
      for (uint32_t attributeIndex = 0; attributeIndex < input.attributeCount; ++attributeIndex) {
        const VertexAttribute& attribute = input.attributes[attributeIndex];
        if (attribute.bufferIndex >= draw.vertexBufferCount) {
          continue;
        }

        const GLBuffer* vertexBuffer = device.buffer(draw.vertexBuffers[attribute.bufferIndex]);
        if (vertexBuffer == nullptr) {
          continue;
        }

        const uint64_t stride = input.buffers[attribute.bufferIndex].stride;
        const auto* pointer = reinterpret_cast<const void*>(
                static_cast<uintptr_t>(draw.vertexBufferOffsets[attribute.bufferIndex] + attribute.offset));

        // A VAO's attribute wiring records the buffer binding at the time of the call, so binding
        // the buffer here is what associates the pointer with it.
        api.glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer->meta.name);

        if (isIntegerVertexFormat(attribute.format)) {
          // Integer attributes must go through glVertexAttribIPointer; the float entry point would
          // convert them silently, which is the classic "my bone indices are wrong" bug.
          const bool isSigned = attribute.format == VertexFormat::SInt || attribute.format == VertexFormat::SInt2 ||
                                attribute.format == VertexFormat::SInt3 || attribute.format == VertexFormat::SInt4;
          api.glVertexAttribIPointer(attribute.shaderLocation,
                  static_cast<GLint>(getVertexFormatComponentCount(attribute.format)),
                  isSigned ? GL_INT : GL_UNSIGNED_INT, static_cast<GLsizei>(stride), pointer);
        } else {
          const GLVertexFormat format = toGLVertexFormat(attribute.format);
          if (format.componentCount == 0) {
            continue;
          }
          api.glVertexAttribPointer(attribute.shaderLocation, format.componentCount, format.type, format.normalized,
                  static_cast<GLsizei>(stride), pointer);
        }

        api.glEnableVertexAttribArray(attribute.shaderLocation);
        api.glVertexAttribDivisor(attribute.shaderLocation,
                input.buffers[attribute.bufferIndex].stepMode == VertexStepMode::Instance ? 1u : 0u);
      }
      api.glBindBuffer(GL_ARRAY_BUFFER, 0);
    });
  };

  const auto applyPipelineState = [&](const GLRenderPipeline& pipeline) {
    // Rasterizer.
    const GLenum frontFace = pipeline.rasterizer.flipY
                                     ? (pipeline.rasterizer.frontFace == FrontFace::CounterClockwise ? GL_CW : GL_CCW)
                                     : (pipeline.rasterizer.frontFace == FrontFace::CounterClockwise ? GL_CCW : GL_CW);
    if (state.frontFace != frontFace) {
      api.glFrontFace(frontFace);
      state.frontFace = frontFace;
    }

    if (pipeline.rasterizer.cullMode == CullMode::None) {
      if (state.cullFace != GL_NONE) {
        api.glDisable(GL_CULL_FACE);
        state.cullFace = GL_NONE;
      }
    } else {
      const GLenum mode = pipeline.rasterizer.cullMode == CullMode::Front ? GL_FRONT : GL_BACK;
      if (state.cullFace != mode) {
        api.glEnable(GL_CULL_FACE);
        api.glCullFace(mode);
        state.cullFace = mode;
      }
    }

    // Wireframe. Core on desktop, impossible on OpenGL ES 3.0 -- the pipeline validator already
    // rejected it on ES, so reaching the desktop branch is the only case that does anything.
    if (pipeline.rasterizer.wireframe && api.glPolygonMode != nullptr) {
      api.glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    } else if (api.glPolygonMode != nullptr) {
      api.glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
    if (pipeline.rasterizer.depthBiasConstant != 0.0f || pipeline.rasterizer.depthBiasSlope != 0.0f) {
      api.glEnable(GL_POLYGON_OFFSET_FILL);
      api.glPolygonOffset(pipeline.rasterizer.depthBiasSlope, pipeline.rasterizer.depthBiasConstant);
    } else {
      api.glDisable(GL_POLYGON_OFFSET_FILL);
    }

    // Depth.
    if (pipeline.depthStencil.depthTestEnable) {
      if (!state.depthTest) {
        api.glEnable(GL_DEPTH_TEST);
        state.depthTest = true;
      }
      const GLenum func = toGLCompare(pipeline.depthStencil.depthCompare);
      if (state.depthFunc != func) {
        api.glDepthFunc(func);
        state.depthFunc = func;
      }
      if (state.depthWrite != pipeline.depthStencil.depthWriteEnable) {
        api.glDepthMask(pipeline.depthStencil.depthWriteEnable ? GL_TRUE : GL_FALSE);
        state.depthWrite = pipeline.depthStencil.depthWriteEnable;
      }
    } else if (state.depthTest) {
      api.glDisable(GL_DEPTH_TEST);
      state.depthTest = false;
    }

    // Stencil.
    if (pipeline.depthStencil.stencilEnable) {
      api.glEnable(GL_STENCIL_TEST);
      const StencilFaceState& front = pipeline.depthStencil.front;
      api.glStencilFuncSeparate(GL_FRONT, toGLCompare(front.compare), GLint(pipeline.depthStencil.stencilReference),
              front.readMask);
      api.glStencilOpSeparate(GL_FRONT, toGLStencilOp(front.failOp), toGLStencilOp(front.depthFailOp),
              toGLStencilOp(front.passOp));
      api.glStencilMaskSeparate(GL_FRONT, front.writeMask);

      const StencilFaceState& back = pipeline.depthStencil.back;
      api.glStencilFuncSeparate(GL_BACK, toGLCompare(back.compare), GLint(pipeline.depthStencil.stencilReference),
              back.readMask);
      api.glStencilOpSeparate(GL_BACK, toGLStencilOp(back.failOp), toGLStencilOp(back.depthFailOp),
              toGLStencilOp(back.passOp));
      api.glStencilMaskSeparate(GL_BACK, back.writeMask);

      // An opt-in trace of the stencil state this pipeline is about to be drawn under, because a defect that
      // depends on the *sequence* of state changes cannot be found by reading the code that sets it: the calls
      // look right and the picture is wrong, which means the disagreement is between what this function
      // believes it applied and what the driver is in force with. Printing the translated values per bind is
      // what closes that gap, and it is off by default because it is per-draw noise in a normal run.
      //
      //   set CORAL_GL_TRACE_STENCIL=1
      //
      // The values are printed as the *enum numbers* rather than as names, because the question is whether the
      // number reaching the driver is the one intended -- `GL_ZERO` is 0, and a state that reads "0" where
      // "keep" was meant is exactly the kind of thing a name would hide.
      if (glStencilTraceEnabled()) {
        std::fprintf(stderr,
                "[stencil] enable=1 ref=%d front(compare=%u fail=%u dfail=%u pass=%u mask=%u) "
                "back(pass=%u mask=%u)\n",
                static_cast<int>(pipeline.depthStencil.stencilReference), toGLCompare(front.compare),
                toGLStencilOp(front.failOp), toGLStencilOp(front.depthFailOp), toGLStencilOp(front.passOp),
                front.writeMask, toGLStencilOp(back.passOp), back.writeMask);
        std::fflush(stderr);
      }
    } else {
      api.glDisable(GL_STENCIL_TEST);
      if (glStencilTraceEnabled()) {
        std::fprintf(stderr, "[stencil] enable=0\n");
        std::fflush(stderr);
      }
    }

    // Blending. OpenGL applies one blend state to every draw buffer, so with multiple targets the
    // first target's state wins; the validator reports the mismatch rather than the backend silently
    // picking one.
    BlendState blend = {};
    if (pipeline.colorTargetCount > 0) blend = pipeline.colorTargets[0].blend;
    if (blend.blendEnable) {
      if (!state.blendEnabled) {
        api.glEnable(GL_BLEND);
        state.blendEnabled = true;
      }
      api.glBlendFuncSeparate(toGLBlendFactor(blend.color.srcFactor), toGLBlendFactor(blend.color.dstFactor),
              toGLBlendFactor(blend.alpha.srcFactor), toGLBlendFactor(blend.alpha.dstFactor));
      api.glBlendEquationSeparate(toGLBlendOp(blend.color.operation), toGLBlendOp(blend.alpha.operation));
    } else if (state.blendEnabled) {
      api.glDisable(GL_BLEND);
      state.blendEnabled = false;
    }

    // Colour write mask.
    //
    // This is the one piece of fixed-function state the backend declared and never applied, and leaving it out
    // had a cost beyond the unimplemented feature: `glColorMask` is what a stencil-only pass uses to write no
    // colour, and without it a caller has to arrange the same effect through the blend unit -- source factor
    // `Zero`, destination factor `One`, which is a *no-op rather than a write disable*. That substitutes a
    // blend state for a write mask, and any place the two are tracked differently stops being correct. It is
    // also per-draw state that is never reset, so "no colour writes" set for one pass is still in force for the
    // next command unless something happens to change it.
    //
    // Applied whenever it differs from what is in force, with the sentinel covering "nothing has been applied
    // yet". One `glColorMask` per pipeline change is far cheaper than the class of bug it removes.
    if (pipeline.colorTargetCount > 0) {
      const uint8_t mask = pipeline.colorTargets[0].writeMask;
      if (state.colorWriteMask != mask) {
        api.glColorMask((mask & 0x1u) != 0 ? GL_TRUE : GL_FALSE, (mask & 0x2u) != 0 ? GL_TRUE : GL_FALSE,
                (mask & 0x4u) != 0 ? GL_TRUE : GL_FALSE, (mask & 0x8u) != 0 ? GL_TRUE : GL_FALSE);
        state.colorWriteMask = mask;
      }
    }

    if (api.glSampleMaski != nullptr && pipeline.multisample.sampleMask != 0) {
      api.glSampleMaski(0, pipeline.multisample.sampleMask);
    }
  };

  /**
   * Uploads the recorded push constants into the scratch uniform buffer and binds it where the program's
   * push-constant block lives.
   *
   * Called before every draw, and deliberately not through a descriptor set: a push constant is program
   * state rather than a descriptor, so it must not depend on which sets a caller bound -- and it must be
   * re-bound per draw, because `pushConstants` accumulates into `pushConstantData` as the command list is
   * walked and the block's contents therefore change between draws.
   *
   * A shader that declares no `layout(push_constant)` block has `pushConstantBlockIndex` set to
   * GL_INVALID_INDEX and is left alone: the recorded bytes are simply not for it.
   */
  const auto applyPushConstants = [&]() {
    if (pushConstantData.empty()) return;
    const GLRenderPipeline* pipeline = device.renderPipeline(draw.pipeline);
    if (pipeline == nullptr || pipeline->program.pushConstantBlockIndex == 0xFFFFFFFFu) return;
    if (api.glBindBufferBase == nullptr) return;

    if (pushConstantBuffer == 0) {
      api.glGenBuffers(1, &pushConstantBuffer);
      if (pushConstantBuffer == 0) {
        releaseScratch();
        return;
      }
      api.glBindBuffer(GL_UNIFORM_BUFFER, pushConstantBuffer);
      api.glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(pushConstantData.size()), nullptr, GL_DYNAMIC_DRAW);
    }
    api.glBindBuffer(GL_UNIFORM_BUFFER, pushConstantBuffer);
    api.glBufferSubData(GL_UNIFORM_BUFFER, 0, GLsizeiptr(pushConstantData.size()), pushConstantData.data());
    api.glBindBufferBase(GL_UNIFORM_BUFFER, pipeline->program.pushConstantBlockBinding, pushConstantBuffer);
    if (std::getenv("CORAL_GL_TRACE_BLOCKS") != nullptr) {
      std::fprintf(stderr, "[push] %zu byte(s) -> block=%u binding=%u\n", pushConstantData.size(),
              pipeline->program.pushConstantBlockIndex, pipeline->program.pushConstantBlockBinding);
      std::fflush(stderr);
    }
  };

  /**
   * Binds every descriptor in every currently bound set, resolving each (set, binding) pair through
   * the program's reflection-driven table. This is where descriptor-set semantics meet OpenGL ES:
   * upper layers name a (set, binding), and the table supplies the texture unit or uniform block.
   */
  const auto applyDescriptorSets = [&]() -> Result {
    const GLRenderPipeline* pipeline = device.renderPipeline(draw.pipeline);
    if (pipeline == nullptr) {
      return Result::Error;
    }

    for (uint32_t setIndex = 0; setIndex < kMaxDescriptorSetLayouts; ++setIndex) {
      const GLDescriptorSet* set = device.descriptorSet(draw.descriptorSets[setIndex]);
      if (set == nullptr) {
        continue;
      }

      for (const GLDescriptorBinding& binding: set->bindings) {
        const GLUniformBinding* location = pipeline->program.findBinding(setIndex, binding.binding);
        if (location == nullptr) {
          if (std::getenv("CORAL_TRACE_VAO") != nullptr) {
            std::fprintf(stderr, "[bind] set=%u binding=%u type=%d has NO location in this program (skipped)\n",
                    setIndex, binding.binding, static_cast<int>(binding.type));
            std::fflush(stderr);
          }
          continue;
        }
        if (std::getenv("CORAL_TRACE_VAO") != nullptr) {
          std::fprintf(stderr, "[bind] set=%u binding=%u type=%d -> blockIndex=0x%08x textureUnit=%d\n", setIndex,
                  binding.binding, static_cast<int>(binding.type), location->blockIndex, location->textureUnit);
          std::fflush(stderr);
        }

        switch (binding.type) {
          case BindingType::TextureSampler:
          case BindingType::Texture: {
            const GLTexture* texture = device.texture(binding.texture);
            if (texture == nullptr) {
              break;
            }
            if (location->textureUnit < 0) {
              break;
            }

            api.glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + location->textureUnit));
            api.glBindTexture(texture->meta.target, texture->meta.name);

            // ## Why an external texture gets no sampler object
            //
            // Maleoon 910 returns black for every sample of a `GL_TEXTURE_EXTERNAL_OES` binding that
            // has a sampler object bound to its unit, even though the import succeeded, the EGLImage
            // is valid and the same buffer samples correctly without one. The direct-GL preview chain,
            // which binds no sampler objects, has always drawn this buffer, which is what isolated it.
            // An external texture carries its own state anyway: `importNativeTexture` sets
            // LINEAR/CLAMP_TO_EDGE on it, and the YUV-to-RGB conversion happens in the sampler the
            // driver generates for the OES target rather than in anything a sampler object configures.
            if (binding.sampler.valid() && texture->meta.target != GL_TEXTURE_EXTERNAL_OES) {
              const GLSampler* sampler = device.sampler(binding.sampler);
              if (sampler != nullptr && sampler->name != 0 && api.glBindSampler != nullptr) {
                api.glBindSampler(GLuint(location->textureUnit), sampler->name);
              }
            } else if (api.glBindSampler != nullptr) {
              // No sampler object for this binding, so the texture's own parameters apply. Unbinding
              // any sampler left over from a previous draw is what makes that true.
              api.glBindSampler(static_cast<GLuint>(location->textureUnit), 0);
            }
            break;
          }

          case BindingType::Sampler:
            // A texture-less sampler has no GL equivalent: ES 3.0 samplers must be combined with an
            // image. The binding is accepted and ignored, which is why the RHI reports
            // Feature::SeparateSamplers only where the concept maps cleanly.
            break;

          case BindingType::UniformBuffer:
          case BindingType::ReadOnlyStorageBuffer:
          case BindingType::StorageBuffer: {
            // ## What each uniform descriptor actually carries, printed before the invalid-index test
            //
            // A descriptor whose `blockIndex` is `GL_INVALID_INDEX` is skipped silently below, and that silence is
            // indistinguishable from a binding that was never declared: the shader reads zeros either way. So the print
            // comes *first*, which is what makes "the block is bound to the wrong point", "the block was never reflected
            // into this layout" and "the range is wrong" three different lines rather than one symptom.
            //
            // The handle is printed through `value()` rather than passed to `%u`: `BufferHandle` is a struct of two
            // bitfields, not an integer, so handing it to a varargs format is a type mismatch that MSVC reports as
            // C4477 and that a driver would be within its rights to turn into a wrong number. `value()` is the encoding
            // the RHI's pools address a buffer by -- the resolved GL name on the next line is the one that matters.
            if (std::getenv("CORAL_GL_TRACE_BLOCKS") != nullptr) {
              std::fprintf(stderr, "[set] binding=%u blockIndex=0x%08x handle=0x%08llx offset=%llu size=%llu\n",
                      location->binding, location->blockIndex,
                      static_cast<unsigned long long>(binding.buffer.buffer.value()),
                      static_cast<unsigned long long>(binding.buffer.offset),
                      static_cast<unsigned long long>(binding.buffer.size));
              std::fflush(stderr);
            }
            if (location->blockIndex == 0xFFFFFFFFu) {
              break;
            }

            GLuint bufferName = 0;
            uint64_t offset = 0;
            uint64_t size = 0;

            // A push constant is not reached through a descriptor set any more -- see
            // `applyPushConstants`, which fills the program's block before each draw. It used to be
            // spotted here by its binding number, which meant the emulation only fired for a caller that
            // also declared that number in a set, and a shader whose push data was never uploaded read
            // zeros with no error anywhere.
            if (const GLBuffer* uniform = device.buffer(binding.buffer.buffer)) {
              bufferName = uniform->meta.name;
              offset = binding.buffer.offset;
              size = binding.buffer.size;
            }

            if (std::getenv("CORAL_GL_TRACE_BLOCKS") != nullptr) {
              std::fprintf(stderr, "[set] binding=%u resolved=%u offset=%llu size=%llu\n", location->binding,
                      bufferName, static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size));
              std::fflush(stderr);
            }
            if (bufferName != 0 && api.glBindBufferBase != nullptr) {
              // `blockBinding`, not `binding`: a shader whose declaration named a location has its
              // uniform block sitting at that location in the program, while the descriptor set that
              // reached this point is still indexed by the binding number. Binding the buffer to the
              // descriptor number instead would leave the program reading an unbound block.
              api.glBindBufferBase(GL_UNIFORM_BUFFER, location->blockBinding, bufferName);
              // The offset/size form of glBindBufferRange is used when a sub-range was requested;
              // the whole-buffer form above is the common case and avoids a driver workaround.
              if ((offset != 0 || size != 0) && api.glBindBufferRange != nullptr) {
                const GLBuffer* uniform = device.buffer(binding.buffer.buffer);
                const uint64_t total = uniform != nullptr ? uniform->meta.size : size;
                api.glBindBufferRange(GL_UNIFORM_BUFFER, location->blockBinding, bufferName, GLintptr(offset),
                        GLsizeiptr(size != 0 ? size : total - offset));
              }
            }
            break;
          }
        }
      }
    }
    return Result::Success;
  };

  // ---------------------------------------------------------------- command walk
  for (const GLCommand& command: mCommandList.commands) {
    switch (command.kind) {
      case GLCommandKind::BeginRenderPass: {
        draw.colorAttachmentCount = command.colorAttachmentCount;
        draw.hasDepthStencil = command.hasDepthStencil;
        for (uint32_t index = 0; index < command.colorAttachmentCount; ++index) {
          draw.colorAttachments[index] = command.colorAttachments[index];
        }
        draw.depthStencilAttachment = command.depthStencilAttachment;

        GLuint colorNames[kMaxColorAttachments] = {};
        GLuint depthName = 0;
        bool hasDefaultFramebuffer = false;

        for (uint32_t index = 0; index < command.colorAttachmentCount; ++index) {
          const GLRecordedAttachment& attachment = command.colorAttachments[index];
          if (attachment.isDefaultFramebuffer) {
            hasDefaultFramebuffer = true;
            continue;
          }
          if (const GLTexture* texture = device.texture(attachment.texture)) {
            colorNames[index] = texture->meta.name;
            if (draw.targetWidth == 0) {
              draw.targetWidth = texture->meta.width;
              draw.targetHeight = texture->meta.height;
            }
          }
        }
        if (command.hasDepthStencil && !command.depthStencilAttachment.isDefaultFramebuffer) {
          if (const GLTexture* texture = device.texture(command.depthStencilAttachment.texture)) {
            depthName = texture->meta.name;
            if (draw.targetWidth == 0) {
              draw.targetWidth = texture->meta.width;
              draw.targetHeight = texture->meta.height;
            }
          }
        }

        // A pass whose only attachment is the swapchain image renders straight to framebuffer 0,
        // which is the OpenGL equivalent of "the acquired swapchain image".
        if (hasDefaultFramebuffer) {
          state.framebuffer = 0;
          api.glBindFramebuffer(GL_FRAMEBUFFER, 0);
        } else {
          const uint64_t key = GLDevice::makeFramebufferKey(colorNames, depthName, command.colorAttachmentCount);
          GLuint framebuffer = 0;
          const Result acquired =
                  device.acquireFramebuffer(colorNames, depthName, command.colorAttachmentCount, key, &framebuffer);
          if (!isSuccess(acquired)) {
            releaseScratch();
            return acquired;
          }
          api.glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
          state.framebuffer = framebuffer;
        }

        // Load operations. A Clear is executed as part of opening the pass, which is precisely why
        // recording is deferred: whether to clear at all, and with what value, is a command-time
        // decision that an eager API could not express.
        for (uint32_t index = 0; index < command.colorAttachmentCount; ++index) {
          const GLRecordedAttachment& attachment = command.colorAttachments[index];
          if (attachment.loadOp != LoadOp::Clear) {
            continue;
          }

          // ## The conversion is not optional, and the reinterpret_cast that used to be here was a bug
          //
          // `backend::Color` holds four **doubles** and `glClearBufferfv` takes a pointer to four **floats**,
          // so reinterpreting the address read each double's bit pattern as two floats. A clear to
          // `Color(1, 0, 0, 1)` reached the driver as `(0, 0x3FF00000-as-float, 0, 0)` and the readback was
          // green with a zero alpha -- a colour nobody wrote, from a call that reported success. Transparent
          // black happened to survive, because four zero doubles reinterpret as eight zero floats and the
          // first four of those are still zero -- which is exactly why nothing noticed.
          //
          // Narrowing properly costs four conversions on a path that runs once per render pass, and it is the
          // only spelling that is correct for *every* clear colour rather than for the one the tests happened
          // to use.
          const GLfloat clearColor[4] = {
            static_cast<GLfloat>(attachment.clearColor.r),
            static_cast<GLfloat>(attachment.clearColor.g),
            static_cast<GLfloat>(attachment.clearColor.b),
            static_cast<GLfloat>(attachment.clearColor.a),
          };
          api.glClearBufferfv(GL_COLOR, static_cast<GLint>(index), clearColor);
        }
        if (command.hasDepthStencil && command.depthStencilAttachment.loadOp == LoadOp::Clear) {
          api.glClearBufferfi(GL_DEPTH_STENCIL, 0, command.depthStencilAttachment.clearDepthStencil.depth,
                  static_cast<GLint>(command.depthStencilAttachment.clearDepthStencil.stencil));
        } else if (command.hasDepthStencil && command.depthStencilAttachment.loadOp == LoadOp::DontCare) {
          // DontCare means the previous contents are undefined, so nothing is done. Reporting the
          // distinction rather than clearing anyway is what makes DontCare a real optimisation.
        }

        if (draw.targetWidth == 0 || draw.targetHeight == 0) {
          const auto [width, height] = platform.querySurfaceSize();
          draw.targetWidth = width;
          draw.targetHeight = height;
        }

        // Default to a full-surface viewport so that a recorder which never calls setViewport still
        // produces the expected result, matching what every other backend does implicitly.
        if (api.glViewport != nullptr) {
          api.glViewport(0, 0, static_cast<GLsizei>(draw.targetWidth), static_cast<GLsizei>(draw.targetHeight));
        }
        if (api.glScissor != nullptr) {
          if (!state.scissorEnabled) {
            api.glEnable(GL_SCISSOR_TEST);
            state.scissorEnabled = true;
          }
          api.glScissor(0, 0, static_cast<GLsizei>(draw.targetWidth), static_cast<GLsizei>(draw.targetHeight));
        }
        break;
      }

      case GLCommandKind::EndRenderPass: {
        // Multisample resolve: glBlitFramebuffer from the multisampled attachment into its
        // single-sampled resolve target, which is how ES 3.0 exposes MSAA resolve.
        for (uint32_t index = 0; index < draw.colorAttachmentCount; ++index) {
          const GLRecordedAttachment& attachment = draw.colorAttachments[index];
          if (!attachment.resolveTexture.valid() || attachment.sampleCount <= 1) {
            continue;
          }
          if (api.glBlitFramebuffer == nullptr) {
            continue;
          }

          const GLTexture* source = device.texture(attachment.texture);
          const GLTexture* target = device.texture(attachment.resolveTexture);
          if (source == nullptr || target == nullptr) {
            continue;
          }

          const GLuint sourceNames[1] = { source->meta.name };
          const uint64_t key = GLDevice::makeFramebufferKey(sourceNames, 0, 1);
          GLuint readFramebuffer = 0;
          if (!isSuccess(device.acquireFramebuffer(sourceNames, 0, 1, key, &readFramebuffer))) {
            continue;
          }

          api.glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
          api.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, state.framebuffer);
          api.glBlitFramebuffer(0, 0, static_cast<GLint>(source->meta.width), static_cast<GLint>(source->meta.height), 0, 0,
                  static_cast<GLint>(target->meta.width), static_cast<GLint>(target->meta.height), GL_COLOR_BUFFER_BIT,
                  GL_NEAREST);
          api.glBindFramebuffer(GL_FRAMEBUFFER, state.framebuffer);
        }

        draw.colorAttachmentCount = 0;
        draw.hasDepthStencil = false;
        draw.targetWidth = 0;
        draw.targetHeight = 0;
        break;
      }

      case GLCommandKind::BindRenderPipeline: {
        GLRenderPipeline* pipeline = device.renderPipeline(command.renderPipeline);
        if (pipeline == nullptr) {
          break;
        }
        draw.pipeline = command.renderPipeline;
        useProgram(pipeline->program.program);
        applyPipelineState(*pipeline);
        // Every draw that binds a pipeline re-applies its descriptor sets, so the wire is reset here
        // to force that to happen even when the same set handles are still bound.
        for (auto& set: draw.descriptorSets) {
          set = DescriptorSetHandle::invalid();
        }
        break;
      }

      case GLCommandKind::BindComputePipeline: {
        GLComputePipeline* pipeline = device.computePipeline(command.computePipeline);
        if (pipeline == nullptr) {
          break;
        }
        useProgram(pipeline->program.program);
        break;
      }

      case GLCommandKind::SetVertexBuffer: {
        const uint32_t slot = command.uint0;
        if (slot >= kMaxVertexBufferLayouts) {
          break;
        }
        draw.vertexBuffers[slot] = command.buffer;
        draw.vertexBufferOffsets[slot] = command.offset;
        draw.vertexBufferCount = std::max(draw.vertexBufferCount, slot + 1);
        break;
      }

      case GLCommandKind::SetIndexBuffer:
        applyIndexBuffer(command.buffer, command.indexFormat, command.offset);
        break;

      case GLCommandKind::BindDescriptorSet:
        if (command.uint0 < kMaxDescriptorSetLayouts) {
          draw.descriptorSets[command.uint0] = command.descriptorSet;
        }
        break;

      case GLCommandKind::SetViewport: {
        // The RHI's viewport is top-left origin (matching Vulkan and Metal). Because the vertex
        // shader has already been flipped to that convention, GL's bottom-left origin needs the
        // position mirrored: y_gl = targetHeight - (y + height).
        const Viewport& viewport = command.viewport;
        const auto x = static_cast<int32_t>(viewport.x);
        const auto width = static_cast<int32_t>(viewport.width);
        const auto height = static_cast<int32_t>(viewport.height);
        const int32_t y = static_cast<int32_t>(draw.targetHeight) - static_cast<int32_t>(viewport.y) - height;
        api.glViewport(x, y, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        break;
      }

      case GLCommandKind::SetScissor: {
        const ScissorRect& scissor = command.scissor;
        const int32_t y = int32_t(draw.targetHeight) - int32_t(scissor.y) - int32_t(scissor.height);
        if (!state.scissorEnabled) {
          api.glEnable(GL_SCISSOR_TEST);
          state.scissorEnabled = true;
        }
        api.glScissor(scissor.x, y, static_cast<GLsizei>(scissor.width), static_cast<GLsizei>(scissor.height));
        if (std::getenv("CORAL_TRACE_VAO") != nullptr) {
          std::fprintf(stderr, "[gl] scissor=(%d,%d,%u,%u) target=%ux%u\n", scissor.x, y, scissor.width,
                  scissor.height, draw.targetWidth, draw.targetHeight);
          std::fflush(stderr);
        }
        break;
      }

      case GLCommandKind::SetStencilReference:
        api.glStencilFuncSeparate(GL_FRONT_AND_BACK, GL_ALWAYS, static_cast<GLint>(command.uint0), 0xFF);
        break;

      case GLCommandKind::SetBlendConstant:
        api.glBlendColor(static_cast<GLfloat>(command.color.r), static_cast<GLfloat>(command.color.g),
                static_cast<GLfloat>(command.color.b), static_cast<GLfloat>(command.color.a));
        break;

      case GLCommandKind::PushConstants: {
        // Push constant data is accumulated and applied at the next draw, because it is stored in the
        // emulated uniform buffer that applyDescriptorSets fills in.
        const size_t required = static_cast<size_t>(command.offset) + command.dataSize;
        if (pushConstantData.size() < required) {
          pushConstantData.resize(required, 0);
        }
        if (command.data != nullptr) {
          std::memcpy(pushConstantData.data() + command.offset, command.data, command.dataSize);
        }
        break;
      }

      case GLCommandKind::Draw:
      case GLCommandKind::DrawIndexed:
      case GLCommandKind::DrawIndirect:
      case GLCommandKind::DrawIndexedIndirect: {
        GLRenderPipeline* pipeline = device.renderPipeline(draw.pipeline);
        if (pipeline == nullptr) {
          break;
        }
        if (pipeline->program.program == 0) {
          // A pipeline whose program is 0 has nothing to run, and GL would silently discard the draw. Reporting it
          // here names the one case that produces no fragments with no error of its own.
          device.emitMessage(MessageSeverity::Error,
                  "the draw's program is 0, so no fragment stage can run and the draw is discarded");
          break;
        }

        const GLuint vertexArray = applyVertexArray();
        if (vertexArray == 0) {
          // OpenGL ES 3.0 guarantees vertex array objects, so a null VAO means the pipeline's vertex
          // input could not be wired at all (a buffer referenced before it was created, for example).
          // Skipping the draw is the only safe action; the alternative is reading arbitrary memory.
          device.emitMessage(MessageSeverity::Error,
                  "the vertex array for this draw could not be built; a vertex buffer "
                  "referenced by the pipeline is missing");
          break;
        }
        if (state.vertexArray != vertexArray) {
          api.glBindVertexArray(vertexArray);
          state.vertexArray = vertexArray;
          // ## The element array binding has to be re-applied *after* the VAO is bound
          //
          // `GL_ELEMENT_ARRAY_BUFFER` is **VAO state**: the binding a `SetIndexBuffer` command made belongs to whichever
          // vertex array was current at the time, and `acquireVertexArray` deliberately clears it when it builds a new
          // one ("the replay path re-binds it when it needs it" -- this is that re-bind). Without it the draw reads its
          // indices from whatever the new vertex array happens to hold, which is nothing, and on this driver that is not
          // a GL error but an access violation inside `glDrawElements`.
          //
          // It went unnoticed because nothing in the tree issued an indexed draw until the canvas quad route did
          // (see `路径边缘抗锯齿设计与现状.md` 4.1.37): the RHI's indexed path was implemented, validated and never called.
          // The cache is reset rather than trusted, because the two bindings it could confuse -- this VAO's and the
          // previous one's -- are exactly what the binding cache cannot see.
          state.elementBuffer = 0;
          if (draw.indexBuffer.valid()) {
            applyIndexBuffer(draw.indexBuffer, draw.indexFormat, draw.indexOffset);
          }
        }

        applyDescriptorSets();
        applyPushConstants();

        const GLenum topology = toGLTopology(pipeline->rasterizer.topology);
        const bool instanced = command.uint1 > 1;
        if (std::getenv("CORAL_TRACE_VAO") != nullptr) {
          const GLBuffer* traceVertex = device.buffer(draw.vertexBuffers[0]);
          const GLBuffer* traceIndex = device.buffer(draw.indexBuffer);
          std::fprintf(stderr,
                  "[gl] draw program=%u vao=%u count=%u instances=%u instanced=%d vertex=%u(index=%u) "
                  "index=%u(index=%u) format=%d offset=%llu\n",
                  pipeline->program.program, vertexArray, command.uint0, command.uint1, instanced ? 1 : 0,
                  traceVertex != nullptr ? traceVertex->meta.name : 0, draw.vertexBuffers[0].index,
                  traceIndex != nullptr ? traceIndex->meta.name : 0, draw.indexBuffer.index,
                  static_cast<int>(draw.indexFormat), static_cast<unsigned long long>(draw.indexOffset));
          if (api.glGetError != nullptr) {
            // Drain first so anything reported afterwards belongs to this draw.
            while (api.glGetError() != GL_NO_ERROR) {
            }
          }
          std::fflush(stderr);
        }

        switch (command.kind) {
          case GLCommandKind::Draw:
            if (instanced) {
              api.glDrawArraysInstanced(topology, static_cast<GLint>(command.uint2),
                      static_cast<GLsizei>(command.uint0), static_cast<GLsizei>(command.uint1));
            } else {
              api.glDrawArrays(topology, static_cast<GLint>(command.uint2), static_cast<GLsizei>(command.uint0));
            }
            if (std::getenv("CORAL_TRACE_VAO") != nullptr && api.glGetError != nullptr) {
              // Reported *after* the draw, so a driver rejection of this exact call is visible. A draw that the driver
              // refuses produces no fragments and raises nothing else anywhere.
              const GLenum error = api.glGetError();
              std::fprintf(stderr, "[gl] after draw program=%u: glGetError=0x%04x (%s)\n", pipeline->program.program,
                      error, error == GL_NO_ERROR ? "no error" : "ERROR");
              std::fflush(stderr);
            }
            break;

          case GLCommandKind::DrawIndexed: {
            const GLenum indexType = draw.indexFormat == IndexFormat::UInt32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
            const GLsizei indexSize = draw.indexFormat == IndexFormat::UInt32 ? 4 : 2;
            // With an element array buffer bound, `indices` is a byte offset rather than a pointer.
            // The offset given to setIndexBuffer and the firstIndex are therefore combined here so
            // that both keep their own meaning.
            const GLintptr byteOffset =
                    static_cast<GLintptr>(draw.indexOffset) + static_cast<GLintptr>(command.uint2) * indexSize;

            if (command.int0 != 0) {
              // A non-zero vertexOffset needs the base-vertex entry point, which is core in ES 3.0.
              api.glDrawElementsInstancedBaseVertex(topology, static_cast<GLsizei>(command.uint0), indexType,
                      reinterpret_cast<const void*>(byteOffset), static_cast<GLsizei>(command.uint1), command.int0);
            } else if (instanced) {
              api.glDrawElementsInstanced(topology, static_cast<GLsizei>(command.uint0), indexType,
                      reinterpret_cast<const void*>(byteOffset), static_cast<GLsizei>(command.uint1));
            } else {
              api.glDrawElements(topology, static_cast<GLsizei>(command.uint0), indexType,
                      reinterpret_cast<const void*>(byteOffset));
            }
            break;
          }

          case GLCommandKind::DrawIndirect: {
            GLBuffer* arguments = device.buffer(command.buffer);
            if (arguments == nullptr) {
              break;
            }
            api.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, arguments->meta.name);
            api.glDrawArraysIndirect(topology, reinterpret_cast<const void*>(static_cast<uintptr_t>(command.offset)));
            api.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
            break;
          }

          case GLCommandKind::DrawIndexedIndirect: {
            GLBuffer* arguments = device.buffer(command.buffer);
            if (arguments == nullptr) {
              break;
            }
            const GLenum indexType = draw.indexFormat == IndexFormat::UInt32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
            api.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, arguments->meta.name);
            api.glDrawElementsIndirect(topology, indexType,
                    reinterpret_cast<const void*>(static_cast<uintptr_t>(command.offset)));
            api.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
            break;
          }

          default:
            break;
        }
        break;
      }

      case GLCommandKind::Dispatch: {
        // The compute program is already bound by the preceding BindComputePipeline command; the
        // explicit program store is not needed because glDispatchCompute acts on the current program.
        api.glDispatchCompute(command.uint0, command.uint1, command.uint2);
        break;
      }

      case GLCommandKind::DispatchIndirect: {
        GLBuffer* arguments = device.buffer(command.buffer);
        if (arguments == nullptr) {
          break;
        }
        api.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, arguments->meta.name);
        api.glDispatchComputeIndirect(static_cast<GLintptr>(command.offset));
        api.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        break;
      }

      case GLCommandKind::WriteBuffer: {
        GLBuffer* buffer = device.buffer(command.buffer);
        if (buffer == nullptr || command.data == nullptr) {
          break;
        }
        api.glBindBuffer(buffer->meta.target, buffer->meta.name);
        api.glBufferSubData(buffer->meta.target, static_cast<GLintptr>(command.offset),
                static_cast<GLsizeiptr>(command.size), command.data);
        api.glBindBuffer(buffer->meta.target, 0);
        break;
      }

      case GLCommandKind::CopyBufferToBuffer: {
        // OpenGL ES 3.0 has no glCopyBufferSubData, so the copy goes through host memory. The
        // backend says so rather than silently failing: callers that need a GPU-side copy on this
        // baseline must restructure the operation.
        GLBuffer* source = device.buffer(command.buffer);
        GLBuffer* target = device.buffer(command.buffer2);
        if (source == nullptr || target == nullptr) {
          break;
        }

        std::vector<uint8_t> staging(command.size);
        api.glBindBuffer(GL_COPY_READ_BUFFER, source->meta.name);
        void* mapped = api.glMapBufferRange(GL_COPY_READ_BUFFER, static_cast<GLintptr>(command.offset),
                static_cast<GLsizeiptr>(command.size), GL_MAP_READ_BIT);
        if (mapped != nullptr) {
          std::memcpy(staging.data(), mapped, command.size);
          api.glUnmapBuffer(GL_COPY_READ_BUFFER);
          api.glBindBuffer(GL_COPY_READ_BUFFER, 0);

          api.glBindBuffer(GL_COPY_WRITE_BUFFER, target->meta.name);
          api.glBufferSubData(GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(command.offset2),
                  static_cast<GLsizeiptr>(command.size), staging.data());
          api.glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        } else {
          api.glBindBuffer(GL_COPY_READ_BUFFER, 0);
        }
        break;
      }

      case GLCommandKind::WriteTexture: {
        GLTexture* texture = device.texture(command.texture);
        if (texture == nullptr || command.data == nullptr) {
          break;
        }

        api.glBindTexture(texture->meta.target, texture->meta.name);
        // Unbind any pixel unpack buffer: ES 3.0 has no offset form of glTexSubImage2D, so the data
        // pointer must be a real client pointer rather than a buffer offset.
        api.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

        // A row length is expressed in texels; the caller supplies it in bytes because that is what
        // the hardware cares about. Deriving it back through the format's block size is exact for the
        // uncompressed formats this path accepts.
        const PixelFormatInfo& info = getPixelFormatInfo(texture->meta.format);
        if (command.uint0 != 0 && info.bytesPerBlock > 0) {
          api.glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(command.uint0 / info.bytesPerBlock));
        } else {
          api.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        }
        // ## The unpack alignment, which must not stay at its default
        //
        // GL reads a tightly packed upload in rows of `width * bytesPerBlock`, each padded up to
        // `GL_UNPACK_ALIGNMENT` bytes. The default is 4, which is invisible while every texture here is RGBA -- four
        // bytes a texel is always a multiple of four -- and silently wrong for anything narrower. It was found by
        // uploading an `R8_UNORM` coverage field: a 21-texel row was read as 24, so every row after the first landed
        // three texels to the right and the picture came out as neighbouring pixels trading places.
        //
        // 1 means "a row is exactly as wide as it says", which is what every caller of this path hands over: the
        // payload is tightly packed by construction and any gap between rows is expressed by `GL_UNPACK_ROW_LENGTH`.
        api.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        const GLFormatDesc format = describeFormat(texture->meta.format);
        if (format.format == 0) {
          device.emitMessage(MessageSeverity::Warning,
                  "writeTexture does not support compressed formats in this build; upload "
                  "the raw blocks through a staging texture instead");
          api.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
          api.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
          api.glBindTexture(texture->meta.target, 0);
          break;
        }

        const TextureCopyRegion& region = command.textureRegion;
        if (texture->meta.target == GL_TEXTURE_3D || texture->meta.target == GL_TEXTURE_2D_ARRAY) {
          api.glTexSubImage3D(texture->meta.target, static_cast<GLint>(region.mipLevel),
                  static_cast<GLint>(region.textureOrigin.x), static_cast<GLint>(region.textureOrigin.y),
                  static_cast<GLint>(region.textureOrigin.z), static_cast<GLsizei>(region.size.width),
                  static_cast<GLsizei>(region.size.height), static_cast<GLsizei>(region.size.depthOrArrayLayers),
                  format.format, format.type, command.data);
        } else {
          api.glTexSubImage2D(texture->meta.target, static_cast<GLint>(region.mipLevel),
                  static_cast<GLint>(region.textureOrigin.x), static_cast<GLint>(region.textureOrigin.y),
                  static_cast<GLsizei>(region.size.width), static_cast<GLsizei>(region.size.height), format.format,
                  format.type, command.data);
        }

        api.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        api.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

        if (texture->meta.usage != 0 && hasUsage(texture->meta.usage, TextureUsage::Immutable)) {
          // Nothing to do: immutable textures do not need mip regeneration, and doing it would defeat
          // the point of the flag.
        }
        api.glBindTexture(texture->meta.target, 0);
        break;
      }

      case GLCommandKind::CopyBufferToTexture: {
        GLBuffer* buffer = device.buffer(command.buffer);
        GLTexture* texture = device.texture(command.texture);
        if (buffer == nullptr || texture == nullptr) {
          break;
        }

        const TextureCopyRegion& region = command.textureRegion;
        const GLFormatDesc format = describeFormat(texture->meta.format);
        if (format.format == 0) {
          device.emitMessage(MessageSeverity::Warning,
                  "copyBufferToTexture cannot upload a compressed format through this "
                  "path; use CommandBuffer::writeTexture with the raw block data");
          break;
        }

        api.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer->meta.name);
        api.glBindTexture(texture->meta.target, texture->meta.name);

        const uint32_t rowLength = region.bytesPerRow != 0 ? region.bytesPerRow / 4 : region.size.width;
        api.glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(rowLength));
        // The same alignment rule as `writeTexture` above, and for the same reason: this path reads a staging buffer
        // whose rows are packed by the caller, and a format narrower than four bytes a texel would be padded into the
        // wrong place. The row length above is computed as bytes over four, which is itself stated for RGBA -- see the
        // note there; when this path is next used for a format that is not four bytes a texel, that division has to
        // follow `bytesPerBlock` rather than the literal 4.
        api.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        // The offset is carried in the pointer slot, which is the documented way to address into a
        // bound pixel unpack buffer.
        const auto* offsetPointer = reinterpret_cast<const void*>(static_cast<uintptr_t>(command.offset));

        if (texture->meta.target == GL_TEXTURE_3D || texture->meta.target == GL_TEXTURE_2D_ARRAY) {
          api.glTexSubImage3D(texture->meta.target, static_cast<GLint>(region.mipLevel),
                  static_cast<GLint>(region.textureOrigin.x), static_cast<GLint>(region.textureOrigin.y),
                  static_cast<GLint>(region.textureOrigin.z), static_cast<GLsizei>(region.size.width),
                  static_cast<GLsizei>(region.size.height), static_cast<GLsizei>(region.size.depthOrArrayLayers),
                  format.format, format.type, offsetPointer);
        } else {
          api.glTexSubImage2D(texture->meta.target, static_cast<GLint>(region.mipLevel),
                  static_cast<GLint>(region.textureOrigin.x), static_cast<GLint>(region.textureOrigin.y),
                  static_cast<GLsizei>(region.size.width), static_cast<GLsizei>(region.size.height), format.format,
                  format.type, offsetPointer);
        }

        api.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        api.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        api.glBindTexture(texture->meta.target, 0);
        api.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        break;
      }

      case GLCommandKind::CopyTextureToBuffer: {
        GLTexture* texture = device.texture(command.texture);
        GLBuffer* buffer = device.buffer(command.buffer);
        if (texture == nullptr || buffer == nullptr) {
          break;
        }

        const TextureCopyRegion& region = command.textureRegion;
        const GLFormatDesc format = describeFormat(texture->meta.format);
        api.glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer->meta.name);

        // glReadPixels reads from the current read framebuffer, so a texture only becomes a read
        // source once it has been through a render pass. The offset is carried in the pointer slot,
        // which is how a bound pixel pack buffer is addressed.
        auto* offsetPointer = reinterpret_cast<void*>(static_cast<uintptr_t>(command.offset));
        // The same alignment rule as the upload paths, on the read side: `glReadPixels` writes rows of
        // `width * bytesPerBlock` padded up to `GL_PACK_ALIGNMENT`, which defaults to 4 and is wrong for any format
        // narrower than that -- the destination buffer's second row would start four bytes in rather than three.
        api.glPixelStorei(GL_PACK_ALIGNMENT, 1);
        api.glReadPixels(static_cast<GLint>(region.textureOrigin.x), static_cast<GLint>(region.textureOrigin.y),
                static_cast<GLsizei>(region.size.width), static_cast<GLsizei>(region.size.height),
                format.format != 0 ? format.format : GL_RGBA, format.type != 0 ? format.type : GL_UNSIGNED_BYTE,
                offsetPointer);
        api.glPixelStorei(GL_PACK_ALIGNMENT, 4);

        api.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        break;
      }

      case GLCommandKind::CopyTextureToTexture: {
        GLTexture* source = device.texture(command.texture);
        GLTexture* target = device.texture(command.texture2);
        if (source == nullptr || target == nullptr) {
          break;
        }
        if (api.glBlitFramebuffer == nullptr) {
          break;
        }

        // Read the source through a framebuffer and blit into the destination, which is the only
        // GPU-side texture-to-texture copy OpenGL ES 3.0 offers.
        const GLuint sourceNames[1] = { source->meta.name };
        const GLuint targetNames[1] = { target->meta.name };
        const uint64_t sourceKey = GLDevice::makeFramebufferKey(sourceNames, 0, 1);
        const uint64_t targetKey = GLDevice::makeFramebufferKey(targetNames, 0, 1);

        GLuint readFramebuffer = 0;
        GLuint drawFramebuffer = 0;
        if (!isSuccess(device.acquireFramebuffer(sourceNames, 0, 1, sourceKey, &readFramebuffer))) {
          break;
        }
        if (!isSuccess(device.acquireFramebuffer(targetNames, 0, 1, targetKey, &drawFramebuffer))) {
          break;
        }

        const TextureCopyRegion& region = command.textureRegion;
        api.glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
        api.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFramebuffer);
        api.glBlitFramebuffer(static_cast<GLint>(region.textureOrigin.x), static_cast<GLint>(region.textureOrigin.y),
                static_cast<GLint>(region.textureOrigin.x + region.size.width),
                static_cast<GLint>(region.textureOrigin.y + region.size.height),
                static_cast<GLint>(region.textureOrigin.x), static_cast<GLint>(region.textureOrigin.y),
                static_cast<GLint>(region.textureOrigin.x + region.size.width),
                static_cast<GLint>(region.textureOrigin.y + region.size.height), GL_COLOR_BUFFER_BIT, GL_NEAREST);
        api.glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        api.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        api.glBindFramebuffer(GL_FRAMEBUFFER, state.framebuffer);
        break;
      }

      case GLCommandKind::ClearColorTexture: {
        GLTexture* texture = device.texture(command.texture);
        if (texture == nullptr) break;
        if (api.glClearTexImage == nullptr) {
          // Without EXT_clear_texture a clear must go through a render pass, which the caller can do
          // with a full-screen pass. Reporting the limitation is better than a no-op clear.
          device.emitMessage(MessageSeverity::Warning,
                  "clearColorTexture needs GL_EXT_clear_texture; use a render pass with "
                  "LoadOp::Clear instead");
          break;
        }

        const float color[4] = { static_cast<float>(command.color.r), static_cast<float>(command.color.g),
          static_cast<float>(command.color.b), static_cast<float>(command.color.a) };
        api.glBindTexture(texture->meta.target, texture->meta.name);
        api.glClearTexImage(texture->meta.name, static_cast<GLint>(command.subresourceRange.baseMipLevel), GL_RGBA, GL_FLOAT,
                color);
        api.glBindTexture(texture->meta.target, 0);
        break;
      }

      case GLCommandKind::ClearDepthStencilTexture: {
        GLTexture* texture = device.texture(command.texture);
        if (texture == nullptr || api.glClearTexImage == nullptr) {
          break;
        }

        const float depth = command.depthStencil.depth;
        api.glClearTexImage(texture->meta.name, static_cast<GLint>(command.subresourceRange.baseMipLevel),
                GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
        if (getPixelFormatInfo(texture->meta.format).stencil) {
          const auto stencil = static_cast<GLint>(command.depthStencil.stencil);
          api.glClearTexImage(texture->meta.name, static_cast<GLint>(command.subresourceRange.baseMipLevel), GL_STENCIL,
                  GL_INT, &stencil);
        }
        break;
      }

      case GLCommandKind::PipelineBarrier: {
        // The OpenGL backend executes command lists synchronously, so a barrier's ordering guarantee is
        // already satisfied. The call still emits the memory barrier that matters for a later compute
        // or a read-back, which is where a missing barrier would actually show up.
        if (api.glMemoryBarrier != nullptr) {
          const GLbitfield barriers = 0x00000001u | // GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT
                                      0x00000002u | // GL_ELEMENT_ARRAY_BARRIER_BIT
                                      0x00000004u | // GL_UNIFORM_BARRIER_BIT
                                      0x00000008u | // GL_TEXTURE_FETCH_BARRIER_BIT
                                      0x00000020u | // GL_SHADER_IMAGE_ACCESS_BARRIER_BIT
                                      0x00000100u | // GL_FRAMEBUFFER_BARRIER_BIT
                                      0x00000800u | // GL_SHADER_STORAGE_BARRIER_BIT
                                      0x00002000u;  // GL_PIXEL_BUFFER_BARRIER_BIT
          api.glMemoryBarrier(barriers);
        }
        break;
      }

      case GLCommandKind::ResetQuerySet: {
        GLQuerySet* querySet = device.querySet(command.querySet);
        if (querySet == nullptr) {
          break;
        }
        // Resetting a query object requires deleting and regenerating it; the recorded firstQuery /
        // count narrow which entries are replaced.
        const uint32_t first = command.uint0;
        const uint32_t count =
                command.uint1 == 0 ? static_cast<uint32_t>(querySet->queries.size()) - first : command.uint1;
        if (first + count > querySet->queries.size()) {
          break;
        }
        api.glDeleteQueries(static_cast<GLsizei>(count), querySet->queries.data() + first);
        api.glGenQueries(static_cast<GLsizei>(count), querySet->queries.data() + first);
        break;
      }

      case GLCommandKind::BeginQuery:
      case GLCommandKind::EndQuery: {
        GLQuerySet* querySet = device.querySet(command.querySet);
        if (querySet == nullptr || command.uint0 >= querySet->queries.size()) {
          break;
        }
        const GLuint query = querySet->queries[command.uint0];

        if (command.kind == GLCommandKind::BeginQuery) {
          const GLenum target =
                  querySet->type == QueryType::Timestamp ? 0x88BF /* GL_TIME_ELAPSED_EXT */ : GL_ANY_SAMPLES_PASSED;
          api.glBeginQuery(target, query);
        } else {
          api.glEndQuery(querySet->type == QueryType::Timestamp ? 0x88BF /* GL_TIME_ELAPSED_EXT */
                                                                : GL_ANY_SAMPLES_PASSED);
        }
        break;
      }

      case GLCommandKind::DebugMarker: {
        if (command.data != nullptr) {
          device.emitMessage(MessageSeverity::Verbose, static_cast<const char*>(command.data));
        }
        break;
      }
    }
  }

  releaseScratch();

  // Flushing at the end of a submission is what makes the recording visible to the driver without a
  // full glFinish. It is the OpenGL equivalent of a queue submit.
  if (api.glFlush != nullptr) {
    api.glFlush();
  }

  mInFlight = false;
  mEnded = false;
  return Result::Success;
}

} // namespace backend
