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

#include "private/backend/metal/MetalCommandBuffer.h"

#include "MetalCross.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace backend::metal {
namespace {

std::string describe(NSError* error) {
  if (error == nil) return "no error was reported";
  std::string message;
  const char* utf8 = [error.localizedDescription UTF8String];
  if (utf8 != nullptr) message = utf8;
  if (message.empty()) message = "the driver reported an error with no description";
  message += " (";
  const char* domain = [error.domain UTF8String];
  message += domain != nullptr ? domain : "unknown domain";
  message += ' ';
  message += std::to_string(error.code);
  message += ')';
  return message;
}

MTLClearColor toClearColor(const Color& color) {
  return MTLClearColorMake(color.r, color.g, color.b, color.a);
}

uint32_t indexByteSize(const IndexFormat format) {
  return format == IndexFormat::UInt16 ? 2u : 4u;
}

} // namespace anonymous

// ==================================================================================================
// Lifecycle
// ==================================================================================================

MetalCommandBuffer::MetalCommandBuffer(MetalDevice& device, const CommandBufferDescriptor& descriptor)
        : mDevice(device),
          mDescriptor(descriptor) {
  if (descriptor.label != nullptr) mLabel = descriptor.label;
}

MetalCommandBuffer::~MetalCommandBuffer() {
  abandon();
}

void MetalCommandBuffer::abandon() {
  // Ending an encoder on a command buffer that was never committed is cheap and is the only way to leave the object in
  // a defined state; nothing encoded reaches the GPU because the command buffer is released without a commit.
  if (mRenderEncoder != nil) {
    [mRenderEncoder endEncoding];
    mRenderEncoder = nil;
  }
  if (mComputeEncoder != nil) {
    [mComputeEncoder endEncoding];
    mComputeEncoder = nil;
  }
  if (mBlitEncoder != nil) {
    [mBlitEncoder endEncoding];
    mBlitEncoder = nil;
  }
  mCommandBuffer = nil;
  mRecording = false;
  mEnded = false;
  mRenderPassOpen = false;
  mRenderPassSuspended = false;
  releaseStaging();
}

Result MetalCommandBuffer::begin() {
  if (mRecording) {
    // Starting a new recording over an unfinished one would silently discard commands the caller already recorded. The
    // RHI's contract is begin/end pairs, so this is a caller error rather than something to paper over.
    mDevice.emitMessage(MessageSeverity::Error, "a command buffer cannot be begun while it is already recording");
    return Result::InvalidArgument;
  }
  endAllEncoders(false);
  releaseStaging();

  mCommandBuffer = [mDevice.commandQueue() commandBuffer];
  if (mCommandBuffer == nil) return Result::Error;
  if (!mLabel.empty()) mCommandBuffer.label = [NSString stringWithUTF8String:mLabel.c_str()];

  resetRecordedState();
  mRecording = true;
  mEnded = false;
  mSubmitted = false;
  return Result::Success;
}

Result MetalCommandBuffer::end() {
  if (!mRecording) return Result::InvalidArgument;
  if (mRenderPassOpen) {
    mDevice.emitMessage(MessageSeverity::Error, "a render pass is still open when the command buffer ends");
    return Result::InvalidArgument;
  }
  endAllEncoders();
  mRecording = false;
  mEnded = true;
  return Result::Success;
}

Result MetalCommandBuffer::reset() {
  // The Vulkan backend's regression fix, mirrored: resetting an in-progress recording discards it rather than failing,
  // because a command-buffer ring that can become permanently unusable is worse than one that throws work away on the
  // caller's explicit request.
  endAllEncoders(false);
  releaseStaging();
  mCommandBuffer = nil;
  resetRecordedState();
  mRecording = false;
  mEnded = false;
  mSubmitted = false;
  return Result::Success;
}

void MetalCommandBuffer::dumpCommandCounts() const {
  std::printf("metal command buffer '%s': draws=%u indexed=%u indirect=%u dispatches=%u copies=%u barriers=%u\n",
          mLabel.c_str(), mCounts.draws, mCounts.indexedDraws, mCounts.indirectDraws, mCounts.dispatches,
          mCounts.copies, mCounts.barriers);
}

void MetalCommandBuffer::resetRecordedState() {
  mRenderPipeline = RenderPipelineHandle::invalid();
  mComputePipeline = ComputePipelineHandle::invalid();
  for (auto& set: mDescriptorSets) set = DescriptorSetHandle::invalid();
  for (uint32_t index = 0; index < kMaxVertexBufferLayouts; ++index) {
    mVertexBuffers[index] = BufferHandle::invalid();
    mVertexOffsets[index] = 0;
    mVertexBufferSet[index] = false;
  }
  mIndexBuffer = BufferHandle::invalid();
  mIndexFormat = IndexFormat::UInt32;
  mIndexOffset = 0;
  mIndexBufferSet = false;
  mViewport = {};
  mViewportSet = false;
  mScissor = {};
  mScissorSet = false;
  mBlendConstantSet = false;
  mBlendConstant = Color::black();
  mStencilReferenceSet = false;
  mStencilReference = 0;
  mPushConstants.clear();
  mPushConstantsSet = false;
  mDebugGroupDepth = 0;
  mCounts = Counts{};
}

void MetalCommandBuffer::releaseStaging() {
  mStagingBuffers.clear();
}

// ==================================================================================================
// Encoder management
// ==================================================================================================

Result MetalCommandBuffer::endRenderEncoder(const bool suspend) {
  if (mRenderEncoder != nil) {
    [mRenderEncoder endEncoding];
    mRenderEncoder = nil;
  }
  if (suspend && mRenderPassOpen) mRenderPassSuspended = true;
  return Result::Success;
}

Result MetalCommandBuffer::endComputeEncoder() {
  if (mComputeEncoder != nil) {
    [mComputeEncoder endEncoding];
    mComputeEncoder = nil;
  }
  return Result::Success;
}

Result MetalCommandBuffer::endBlitEncoder() {
  if (mBlitEncoder != nil) {
    [mBlitEncoder endEncoding];
    mBlitEncoder = nil;
  }
  return Result::Success;
}

Result MetalCommandBuffer::endAllEncoders(const bool suspendRenderPass) {
  endBlitEncoder();
  endComputeEncoder();
  endRenderEncoder(suspendRenderPass);
  return Result::Success;
}

Result MetalCommandBuffer::ensureBlitEncoder() {
  if (mBlitEncoder != nil) return Result::Success;
  endComputeEncoder();
  // A blit cannot be encoded while a render encoder is open, so the pass is suspended and re-created with a load
  // action after the transfer. See the note on this in MetalCommandBuffer.h.
  endRenderEncoder(true);
  mBlitEncoder = [mCommandBuffer blitCommandEncoder];
  if (mBlitEncoder == nil) return Result::Error;
  if (mDevice.validationEnabled() && !mLabel.empty()) {
    mBlitEncoder.label = [NSString stringWithFormat:@"%@/transfer", [NSString stringWithUTF8String:mLabel.c_str()]];
  }
  return Result::Success;
}

Result MetalCommandBuffer::ensureComputeEncoder() {
  if (mComputeEncoder != nil) return Result::Success;
  endBlitEncoder();
  endRenderEncoder(true);
  mComputeEncoder = [mCommandBuffer computeCommandEncoder];
  if (mComputeEncoder == nil) return Result::Error;
  return Result::Success;
}

Result MetalCommandBuffer::suspendRenderPass() {
  return endRenderEncoder(true);
}

Result MetalCommandBuffer::resumeRenderPass() {
  if (!mRenderPassOpen) return Result::InvalidArgument;
  if (mRenderEncoder != nil) return Result::Success;

  MTLRenderPassDescriptor* descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
  for (uint32_t index = 0; index < mPassInfo.colorAttachmentCount; ++index) {
    const RenderPassAttachment& attachment = mPassInfo.colorAttachments[index];
    MetalTextureView view;
    const Result resolved = mDevice.resolveTextureView(attachment.view, view);
    if (!isSuccess(resolved)) return resolved;
    descriptor.colorAttachments[index].texture = view.texture;
    // The resumed encoder must preserve what the first half of the pass wrote, so every attachment loads rather than
    // repeats the original clear.
    descriptor.colorAttachments[index].loadAction = MTLLoadActionLoad;
    if (attachment.resolveTarget.valid()) {
      MetalTextureView resolveView;
      const Result resolveResult = mDevice.resolveTextureView(attachment.resolveTarget, resolveView);
      if (!isSuccess(resolveResult)) return resolveResult;
      descriptor.colorAttachments[index].resolveTexture = resolveView.texture;
      // `MultisampleResolve` *discards* the multisample texture; `StoreAndMultisampleResolve` keeps it. The RHI's
      // render-pass model says the attachment's contents persist unless the pass asked for `DontCare`, and later
      // passes load them -- so a resolve that threw the samples away would corrupt every later pass's resolve.
      descriptor.colorAttachments[index].storeAction = attachment.storeOp == StoreOp::Store
                                                            ? MTLStoreActionStoreAndMultisampleResolve
                                                            : MTLStoreActionMultisampleResolve;
    } else {
      descriptor.colorAttachments[index].storeAction = toMtlStoreAction(attachment.storeOp);
    }
  }

  if (mPassInfo.hasDepthStencil) {
    const RenderPassAttachment& attachment = mPassInfo.depthStencilAttachment;
    MetalTextureView view;
    const Result resolved = mDevice.resolveTextureView(attachment.view, view);
    if (!isSuccess(resolved)) return resolved;
    descriptor.depthAttachment.texture = view.texture;
    descriptor.depthAttachment.loadAction = MTLLoadActionLoad;
    descriptor.depthAttachment.storeAction = toMtlStoreAction(attachment.storeOp);
    if (isStencilFormat(mDepthStencilFormat)) {
      descriptor.stencilAttachment.texture = view.texture;
      descriptor.stencilAttachment.loadAction = MTLLoadActionLoad;
      descriptor.stencilAttachment.storeAction = toMtlStoreAction(attachment.storeOp);
    }
  }

  mRenderEncoder = [mCommandBuffer renderCommandEncoderWithDescriptor:descriptor];
  if (mRenderEncoder == nil) return Result::Error;
  mRenderPassSuspended = false;

  // Restore the viewport and scissor to the state the caller recorded; a fresh encoder starts with none.
  const Viewport viewport = mViewportSet ? mViewport
                                         : Viewport{ 0.0f, 0.0f, static_cast<float>(mRenderWidth),
                                                   static_cast<float>(mRenderHeight), 0.0f, 1.0f };
  [mRenderEncoder setViewport:MTLViewport{ viewport.x, viewport.y, viewport.width, viewport.height,
                                          viewport.minDepth, viewport.maxDepth }];
  const ScissorRect scissor = mScissorSet ? mScissor
                                          : ScissorRect{ 0, 0, mRenderWidth, mRenderHeight };
  [mRenderEncoder setScissorRect:MTLScissorRect{ static_cast<NSUInteger>(std::max(0, scissor.x)),
                                                 static_cast<NSUInteger>(std::max(0, scissor.y)),
                                                 std::min(scissor.width, mRenderWidth - static_cast<uint32_t>(std::max(0, scissor.x))),
                                                 std::min(scissor.height, mRenderHeight - static_cast<uint32_t>(std::max(0, scissor.y))) }];
  return Result::Success;
}

Result MetalCommandBuffer::ensureRenderEncoder() {
  if (mRenderEncoder != nil) return Result::Success;
  if (!mRenderPassOpen) {
    mDevice.emitMessage(MessageSeverity::Error, "a draw was recorded outside a render pass");
    return Result::InvalidArgument;
  }
  endComputeEncoder();
  endBlitEncoder();
  return resumeRenderPass();
}

Result MetalCommandBuffer::requireRecording() const {
  if (!mRecording) return Result::InvalidArgument;
  if (mCommandBuffer == nil) return Result::Error;
  return Result::Success;
}

Result MetalCommandBuffer::requireRenderPass() const {
  if (!mRenderPassOpen) {
    mDevice.emitMessage(MessageSeverity::Error, "this command requires an open render pass");
    return Result::InvalidArgument;
  }
  return Result::Success;
}

Result MetalCommandBuffer::requireRenderPipeline() const {
  if (!mRenderPipeline.valid()) {
    mDevice.emitMessage(MessageSeverity::Error, "a draw was recorded without a bound render pipeline");
    return Result::InvalidArgument;
  }
  if (mDevice.renderPipelinePayload(mRenderPipeline) == nullptr) return Result::InvalidHandle;
  return Result::Success;
}

// ==================================================================================================
// Render pass scope
// ==================================================================================================

Result MetalCommandBuffer::beginRenderPass(const RenderPassBeginInfo& info) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  if (mRenderPassOpen) {
    mDevice.emitMessage(MessageSeverity::Error, "a render pass was begun while another one is open");
    return Result::InvalidArgument;
  }

  const MetalRenderPass* pass = mDevice.renderPassPayload(info.renderPass);
  if (pass == nullptr) return Result::InvalidHandle;
  if (info.colorAttachmentCount > kMaxColorAttachments) return Result::InvalidArgument;

  endAllEncoders(false);

  MTLRenderPassDescriptor* descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
  uint32_t width = info.renderArea.width;
  uint32_t height = info.renderArea.height;

  for (uint32_t index = 0; index < info.colorAttachmentCount; ++index) {
    const RenderPassAttachment& attachment = info.colorAttachments[index];
    // The default-framebuffer sentinel resolves to the acquired drawable; a real handle resolves to its texture.
    MetalTextureView view;
    const Result resolved = mDevice.resolveTextureView(attachment.view, view);
    if (!isSuccess(resolved)) {
      mDevice.emitMessage(MessageSeverity::Error, "a render pass names a colour attachment view that is not alive");
      return resolved;
    }
    if (width == 0) width = view.width;
    if (height == 0) height = view.height;

    MTLRenderPassColorAttachmentDescriptor* color = descriptor.colorAttachments[index];
    color.texture = view.texture;
    color.loadAction = toMtlLoadAction(attachment.loadOp);
    color.clearColor = toClearColor(attachment.clearColor);
    if (attachment.resolveTarget.valid()) {
      MetalTextureView resolveView;
      const Result resolveResult = mDevice.resolveTextureView(attachment.resolveTarget, resolveView);
      if (!isSuccess(resolveResult)) return resolveResult;
      color.resolveTexture = resolveView.texture;
      // See the note in `resumeRenderPass`: a plain resolve discards the samples, and the next pass loads them.
      color.storeAction = attachment.storeOp == StoreOp::Store ? MTLStoreActionStoreAndMultisampleResolve
                                                               : MTLStoreActionMultisampleResolve;
    } else {
      color.storeAction = toMtlStoreAction(attachment.storeOp);
    }
  }

  mDepthStencilFormat = pass->depthStencilFormat;
  if (info.hasDepthStencil) {
    const RenderPassAttachment& attachment = info.depthStencilAttachment;
    MetalTextureView view;
    const Result resolved = mDevice.resolveTextureView(attachment.view, view);
    if (!isSuccess(resolved)) {
      mDevice.emitMessage(MessageSeverity::Error, "a render pass names a depth/stencil view that is not alive");
      return resolved;
    }
    if (width == 0) width = view.width;
    if (height == 0) height = view.height;
    descriptor.depthAttachment.texture = view.texture;
    descriptor.depthAttachment.loadAction = toMtlLoadAction(attachment.loadOp);
    descriptor.depthAttachment.clearDepth = attachment.clearDepthStencil.depth;
    descriptor.depthAttachment.storeAction = toMtlStoreAction(attachment.storeOp);
    if (isStencilFormat(mDepthStencilFormat)) {
      // A combined depth/stencil texture is attached twice on Metal: once for each aspect.
      descriptor.stencilAttachment.texture = view.texture;
      descriptor.stencilAttachment.loadAction = toMtlLoadAction(attachment.loadOp);
      descriptor.stencilAttachment.clearStencil = attachment.clearDepthStencil.stencil;
      descriptor.stencilAttachment.storeAction = toMtlStoreAction(attachment.storeOp);
    }
  }

  if (width == 0 || height == 0) {
    mDevice.emitMessage(MessageSeverity::Error, "a render pass has no attachment to take its extent from");
    return Result::InvalidArgument;
  }

  mRenderEncoder = [mCommandBuffer renderCommandEncoderWithDescriptor:descriptor];
  if (mRenderEncoder == nil) return Result::Error;

  mPassInfo = info;
  mRenderWidth = width;
  mRenderHeight = height;
  mRenderPassOpen = true;
  mRenderPassSuspended = false;

  if (!mLabel.empty()) {
    mRenderEncoder.label = [NSString stringWithFormat:@"%@/pass", [NSString stringWithUTF8String:mLabel.c_str()]];
  }
  if (info.label != nullptr) mRenderEncoder.label = [NSString stringWithUTF8String:info.label];

  // Full-target viewport and scissor, exactly as the Vulkan backend resets them at pass begin. Without this a caller
  // that never calls setViewport draws nothing, because a Metal encoder has no default viewport.
  [mRenderEncoder setViewport:MTLViewport{ 0.0, 0.0, static_cast<double>(mRenderWidth),
                                          static_cast<double>(mRenderHeight), 0.0, 1.0 }];
  [mRenderEncoder setScissorRect:MTLScissorRect{ 0, 0, mRenderWidth, mRenderHeight }];
  mViewportSet = false;
  mScissorSet = false;
  return Result::Success;
}

Result MetalCommandBuffer::endRenderPass() {
  if (!mRenderPassOpen) return Result::InvalidArgument;
  if (mRenderEncoder != nil) {
    [mRenderEncoder endEncoding];
    mRenderEncoder = nil;
  }
  // When the pass was suspended for a transfer, its encoder was already ended; the attachments were stored then and
  // the resumed encoder stored them again, so there is nothing left to do.
  mRenderPassOpen = false;
  mRenderPassSuspended = false;
  mPassInfo = RenderPassBeginInfo{};
  return Result::Success;
}

// ==================================================================================================
// Pipeline state
// ==================================================================================================

Result MetalCommandBuffer::bindRenderPipeline(const RenderPipelineHandle pipeline) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  if (mDevice.renderPipelinePayload(pipeline) == nullptr) return Result::InvalidHandle;
  mRenderPipeline = pipeline;
  return Result::Success;
}

Result MetalCommandBuffer::bindComputePipeline(const ComputePipelineHandle pipeline) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  if (mDevice.computePipelinePayload(pipeline) == nullptr) return Result::InvalidHandle;
  mComputePipeline = pipeline;
  return Result::Success;
}

Result MetalCommandBuffer::setVertexBuffer(const uint32_t slot, const BufferHandle buffer, const uint64_t offset) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  if (slot >= kMaxVertexBufferLayouts) return Result::InvalidArgument;
  const MetalBuffer* payload = mDevice.bufferPayload(buffer);
  if (payload == nullptr) return Result::InvalidHandle;
  if (offset >= payload->size) return Result::InvalidArgument;
  mVertexBuffers[slot] = buffer;
  mVertexOffsets[slot] = offset;
  mVertexBufferSet[slot] = true;
  return Result::Success;
}

Result MetalCommandBuffer::setIndexBuffer(const BufferHandle buffer, const IndexFormat format, const uint64_t offset) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  const MetalBuffer* payload = mDevice.bufferPayload(buffer);
  if (payload == nullptr) return Result::InvalidHandle;
  if (offset >= payload->size) return Result::InvalidArgument;
  mIndexBuffer = buffer;
  mIndexFormat = format;
  mIndexOffset = offset;
  mIndexBufferSet = true;
  return Result::Success;
}

Result MetalCommandBuffer::bindDescriptorSet(const DescriptorSetIndex setIndex, const DescriptorSetHandle set) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  if (setIndex >= kMaxDescriptorSetLayouts) return Result::InvalidArgument;
  if (mDevice.descriptorSetPayload(set) == nullptr) return Result::InvalidHandle;
  mDescriptorSets[setIndex] = set;
  return Result::Success;
}

Result MetalCommandBuffer::setViewport(const Viewport& viewport) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  mViewport = viewport;
  if (mViewport.width == 0.0f) mViewport.width = static_cast<float>(mRenderWidth);
  if (mViewport.height == 0.0f) mViewport.height = static_cast<float>(mRenderHeight);
  mViewportSet = true;
  return Result::Success;
}

Result MetalCommandBuffer::setScissor(const ScissorRect& scissor) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  // The scissor is clamped to the pass extent, which Metal requires and Vulkan also enforces; a caller's rectangle that
  // reaches outside the attachment is a common off-by-one and clamping is the behaviour every backend agrees on.
  const int32_t x = std::max(0, std::min(scissor.x, static_cast<int32_t>(mRenderWidth)));
  const int32_t y = std::max(0, std::min(scissor.y, static_cast<int32_t>(mRenderHeight)));
  mScissor.x = x;
  mScissor.y = y;
  mScissor.width = std::min(scissor.width, mRenderWidth - static_cast<uint32_t>(x));
  mScissor.height = std::min(scissor.height, mRenderHeight - static_cast<uint32_t>(y));
  mScissorSet = true;
  return Result::Success;
}

Result MetalCommandBuffer::setStencilReference(const uint32_t reference) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  mStencilReference = reference;
  mStencilReferenceSet = true;
  if (mRenderEncoder != nil) [mRenderEncoder setStencilReferenceValue:reference];
  return Result::Success;
}

Result MetalCommandBuffer::setBlendConstant(const Color& color) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  mBlendConstant = color;
  mBlendConstantSet = true;
  if (mRenderEncoder != nil) {
    [mRenderEncoder setBlendColorRed:color.r green:color.g blue:color.b alpha:color.a];
  }
  return Result::Success;
}

Result MetalCommandBuffer::pushConstants(const void* data, const uint32_t size, const uint32_t offset) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  if (data == nullptr && size != 0) return Result::InvalidArgument;
  const uint32_t budget = mDevice.getCapabilities().limits.maxPushConstantSize;
  if (offset + size > budget) {
    mDevice.emitMessage(MessageSeverity::Error,
            "pushConstants wrote " + std::to_string(offset + size) + " bytes, and this device's budget is " +
                    std::to_string(budget));
    return Result::InvalidArgument;
  }
  if (mPushConstants.size() < offset + size) mPushConstants.resize(offset + size, 0);
  if (size != 0) std::memcpy(mPushConstants.data() + offset, data, size);
  mPushConstantsSet = true;
  return Result::Success;
}

// ==================================================================================================
// Applying state
// ==================================================================================================

void MetalCommandBuffer::applyDescriptorSet(id<MTLRenderCommandEncoder> encoder, const uint32_t setIndex,
        const DescriptorSetHandle setHandle, const MetalPipelineLayout& layout) {
  const MetalDescriptorSet* set = mDevice.descriptorSetPayload(setHandle);
  if (set == nullptr) return;
  const MetalDescriptorSetLayout* setLayout = mDevice.descriptorSetLayoutPayload(set->layout);
  if (setLayout == nullptr) return;
  if (setIndex >= layout.setLayoutCount) return;

  const MetalPipelineLayout::SetSlots& slots = layout.sets[setIndex];
  for (uint32_t index = 0; index < setLayout->bindingCount; ++index) {
    const DescriptorBinding& binding = setLayout->bindings[index];
    const uint32_t count = std::max(binding.count, 1u);
    for (uint32_t element = 0; element < count; ++element) {
      const auto value = std::find_if(set->values.begin(), set->values.end(),
              [&](const MetalDescriptorBindingValue& candidate) {
                return candidate.binding == binding.binding && candidate.arrayElement == element;
              });

      const auto resolveTexture = [&]() {
        id<MTLTexture> texture = mDevice.fallbackTexture();
        if (value != set->values.end() && value->written) {
          if (value->view.valid()) {
            MetalTextureView view;
            if (isSuccess(mDevice.resolveTextureView(value->view, view))) texture = view.texture;
          } else if (value->texture.valid()) {
            if (const MetalTexture* payload = mDevice.texturePayload(value->texture); payload != nullptr) {
              texture = payload->texture;
            }
          }
        }
        return texture;
      };
      const auto resolveSampler = [&]() {
        id<MTLSamplerState> sampler = mDevice.fallbackSampler();
        if (value != set->values.end() && value->written) {
          if (const MetalSampler* payload = mDevice.samplerPayload(value->sampler); payload != nullptr) {
            sampler = payload->state;
          }
        }
        return sampler;
      };
      switch (binding.type) {
        case BindingType::TextureSampler: {
          // A combined image sampler is two MSL arguments, and the RHI's descriptor write carries both halves.
          const uint32_t textureSlot = slots.textureBase + setLayout->textureIndex[index] + element;
          const uint32_t samplerSlot = slots.samplerBase + setLayout->samplerIndex[index] + element;
          const id<MTLTexture> texture = resolveTexture();
          const id<MTLSamplerState> sampler = resolveSampler();
          [encoder setVertexTexture:texture atIndex:textureSlot];
          [encoder setFragmentTexture:texture atIndex:textureSlot];
          [encoder setVertexSamplerState:sampler atIndex:samplerSlot];
          [encoder setFragmentSamplerState:sampler atIndex:samplerSlot];
          break;
        }
        case BindingType::Texture: {
          const uint32_t slot = slots.textureBase + setLayout->textureIndex[index] + element;
          const id<MTLTexture> texture = resolveTexture();
          [encoder setVertexTexture:texture atIndex:slot];
          [encoder setFragmentTexture:texture atIndex:slot];
          break;
        }
        case BindingType::Sampler: {
          const uint32_t slot = slots.samplerBase + setLayout->samplerIndex[index] + element;
          const id<MTLSamplerState> sampler = resolveSampler();
          [encoder setVertexSamplerState:sampler atIndex:slot];
          [encoder setFragmentSamplerState:sampler atIndex:slot];
          break;
        }
        case BindingType::UniformBuffer:
        case BindingType::ReadOnlyStorageBuffer:
        case BindingType::StorageBuffer: {
          id<MTLBuffer> buffer = mDevice.fallbackBuffer();
          uint64_t offset = 0;
          if (value != set->values.end() && value->written) {
            if (const MetalBuffer* payload = mDevice.bufferPayload(value->buffer.buffer); payload != nullptr) {
              buffer = payload->buffer;
              offset = value->buffer.offset;
            }
          }
          const uint32_t slot = slots.bufferBase + setLayout->bufferIndex[index] + element;
          [encoder setVertexBuffer:buffer offset:static_cast<NSUInteger>(offset) atIndex:slot];
          [encoder setFragmentBuffer:buffer offset:static_cast<NSUInteger>(offset) atIndex:slot];
          break;
        }
      }
    }
  }
}

void MetalCommandBuffer::applyDescriptorSetCompute(id<MTLComputeCommandEncoder> encoder, const uint32_t setIndex,
        const DescriptorSetHandle setHandle, const MetalPipelineLayout& layout) {
  const MetalDescriptorSet* set = mDevice.descriptorSetPayload(setHandle);
  if (set == nullptr) return;
  const MetalDescriptorSetLayout* setLayout = mDevice.descriptorSetLayoutPayload(set->layout);
  if (setLayout == nullptr) return;
  if (setIndex >= layout.setLayoutCount) return;

  const MetalPipelineLayout::SetSlots& slots = layout.sets[setIndex];
  for (uint32_t index = 0; index < setLayout->bindingCount; ++index) {
    const DescriptorBinding& binding = setLayout->bindings[index];
    const uint32_t count = std::max(binding.count, 1u);
    for (uint32_t element = 0; element < count; ++element) {
      const auto value = std::find_if(set->values.begin(), set->values.end(),
              [&](const MetalDescriptorBindingValue& candidate) {
                return candidate.binding == binding.binding && candidate.arrayElement == element;
              });
      const auto resolveTexture = [&]() {
        id<MTLTexture> texture = mDevice.fallbackTexture();
        if (value != set->values.end() && value->written) {
          if (value->view.valid()) {
            MetalTextureView view;
            if (isSuccess(mDevice.resolveTextureView(value->view, view))) texture = view.texture;
          } else if (value->texture.valid()) {
            if (const MetalTexture* payload = mDevice.texturePayload(value->texture); payload != nullptr) {
              texture = payload->texture;
            }
          }
        }
        return texture;
      };
      const auto resolveSampler = [&]() {
        id<MTLSamplerState> sampler = mDevice.fallbackSampler();
        if (value != set->values.end() && value->written) {
          if (const MetalSampler* payload = mDevice.samplerPayload(value->sampler); payload != nullptr) {
            sampler = payload->state;
          }
        }
        return sampler;
      };
      switch (binding.type) {
        case BindingType::TextureSampler: {
          [encoder setTexture:resolveTexture()
                      atIndex:slots.textureBase + setLayout->textureIndex[index] + element];
          [encoder setSamplerState:resolveSampler()
                           atIndex:slots.samplerBase + setLayout->samplerIndex[index] + element];
          break;
        }
        case BindingType::Texture: {
          [encoder setTexture:resolveTexture()
                      atIndex:slots.textureBase + setLayout->textureIndex[index] + element];
          break;
        }
        case BindingType::Sampler: {
          [encoder setSamplerState:resolveSampler()
                           atIndex:slots.samplerBase + setLayout->samplerIndex[index] + element];
          break;
        }
        case BindingType::UniformBuffer:
        case BindingType::ReadOnlyStorageBuffer:
        case BindingType::StorageBuffer: {
          id<MTLBuffer> buffer = mDevice.fallbackBuffer();
          uint64_t offset = 0;
          if (value != set->values.end() && value->written) {
            if (const MetalBuffer* payload = mDevice.bufferPayload(value->buffer.buffer); payload != nullptr) {
              buffer = payload->buffer;
              offset = value->buffer.offset;
            }
          }
          [encoder setBuffer:buffer
                      offset:static_cast<NSUInteger>(offset)
                     atIndex:slots.bufferBase + setLayout->bufferIndex[index] + element];
          break;
        }
      }
    }
  }
}

void MetalCommandBuffer::applyComputeBindings(id<MTLComputeCommandEncoder> encoder) {
  const MetalComputePipeline* pipeline = mDevice.computePipelinePayload(mComputePipeline);
  if (pipeline == nullptr) return;
  const MetalPipelineLayout* layout = mDevice.pipelineLayoutPayload(pipeline->layout);
  static const MetalPipelineLayout kEmptyLayout;
  if (layout == nullptr) layout = &kEmptyLayout;
  for (uint32_t setIndex = 0; setIndex < layout->setLayoutCount; ++setIndex) {
    applyDescriptorSetCompute(encoder, setIndex, mDescriptorSets[setIndex], *layout);
  }
  if (mPushConstantsSet && !mPushConstants.empty()) {
    [encoder setBytes:mPushConstants.data() length:mPushConstants.size() atIndex:kPushConstantBufferSlot];
  }
}

Result MetalCommandBuffer::applyRenderState() {
  if (const Result encoderResult = ensureRenderEncoder(); !isSuccess(encoderResult)) return encoderResult;
  const MetalRenderPipeline* pipeline = mDevice.renderPipelinePayload(mRenderPipeline);
  if (pipeline == nullptr) return Result::InvalidHandle;

  const RenderPipelineDescriptor& state = pipeline->descriptor;
  [mRenderEncoder setRenderPipelineState:pipeline->state];
  [mRenderEncoder setDepthStencilState:pipeline->depthStencilState];
  [mRenderEncoder setStencilReferenceValue:mStencilReferenceSet ? mStencilReference
                                                                : state.depthStencil.stencilReference];

  [mRenderEncoder setCullMode:toMtlCullMode(state.rasterizer.cullMode)];
  // The MSL translation flips Y (Vulkan-style clip space into Metal's Y-up NDC), so the declared winding has to be
  // inverted to keep the face the caller named the face that is culled.
  [mRenderEncoder setFrontFacingWinding:toMtlWinding(state.rasterizer.frontFace, true)];
  if (state.rasterizer.wireframe && mDevice.getCapabilities().supports(Feature::FillModeNonSolid)) {
    [mRenderEncoder setTriangleFillMode:MTLTriangleFillModeLines];
  } else {
    [mRenderEncoder setTriangleFillMode:MTLTriangleFillModeFill];
  }
  if (state.rasterizer.depthBiasConstant != 0.0f || state.rasterizer.depthBiasSlope != 0.0f) {
    [mRenderEncoder setDepthBias:state.rasterizer.depthBiasConstant slopeScale:state.rasterizer.depthBiasSlope clamp:0.0f];
  } else {
    [mRenderEncoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
  }

  const Viewport& viewport = mViewportSet ? mViewport
                                          : Viewport{ 0.0f, 0.0f, static_cast<float>(mRenderWidth),
                                                    static_cast<float>(mRenderHeight), 0.0f, 1.0f };
  [mRenderEncoder setViewport:MTLViewport{ viewport.x, viewport.y, viewport.width, viewport.height,
                                          viewport.minDepth, viewport.maxDepth }];
  const ScissorRect scissor = mScissorSet ? mScissor : ScissorRect{ 0, 0, mRenderWidth, mRenderHeight };
  [mRenderEncoder setScissorRect:MTLScissorRect{ static_cast<NSUInteger>(scissor.x),
                                                 static_cast<NSUInteger>(scissor.y), scissor.width, scissor.height }];

  const Color blend = mBlendConstantSet ? mBlendConstant : state.rasterizer.blendConstant;
  [mRenderEncoder setBlendColorRed:blend.r green:blend.g blue:blend.b alpha:blend.a];

  for (uint32_t slot = 0; slot < kMaxVertexBufferLayouts; ++slot) {
    if (!mVertexBufferSet[slot]) {
      // Unbinding is deliberate: a slot the caller did not set must not keep whatever the previous draw left there.
      [mRenderEncoder setVertexBuffer:nil offset:0 atIndex:slot];
      continue;
    }
    const MetalBuffer* buffer = mDevice.bufferPayload(mVertexBuffers[slot]);
    if (buffer == nullptr) return Result::InvalidHandle;
    [mRenderEncoder setVertexBuffer:buffer->buffer offset:static_cast<NSUInteger>(mVertexOffsets[slot]) atIndex:slot];
  }

  // A pipeline with missing attributes describes them against a dummy stream slot; it has to be a real buffer, because
  // Metal validation reports an unbound stream as an error at draw time.
  if (pipeline->dummyVertexSlot != MetalRenderPipeline::kNoDummyVertexSlot &&
      pipeline->dummyVertexSlot < kMaxVertexBufferSlots) {
    [mRenderEncoder setVertexBuffer:mDevice.dummyVertexBuffer()
                            offset:0
                           atIndex:pipeline->dummyVertexSlot];
  }

  const MetalPipelineLayout* layout = mDevice.pipelineLayoutPayload(pipeline->layout);
  static const MetalPipelineLayout kEmptyLayout;
  if (layout == nullptr) layout = &kEmptyLayout;
  for (uint32_t setIndex = 0; setIndex < layout->setLayoutCount; ++setIndex) {
    applyDescriptorSet(mRenderEncoder, setIndex, mDescriptorSets[setIndex], *layout);
  }

  if (mPushConstantsSet && !mPushConstants.empty()) {
    [mRenderEncoder setVertexBytes:mPushConstants.data() length:mPushConstants.size() atIndex:kPushConstantBufferSlot];
    [mRenderEncoder setFragmentBytes:mPushConstants.data() length:mPushConstants.size() atIndex:kPushConstantBufferSlot];
  }
  return Result::Success;
}

// ==================================================================================================
// Drawing
// ==================================================================================================

Result MetalCommandBuffer::draw(const uint32_t vertexCount, const uint32_t instanceCount, const uint32_t firstVertex,
        const uint32_t firstInstance) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  if (const Result pass = requireRenderPass(); !isSuccess(pass)) return pass;
  if (const Result pipeline = requireRenderPipeline(); !isSuccess(pipeline)) return pipeline;
  if (const Result state = applyRenderState(); !isSuccess(state)) return state;

  const MetalRenderPipeline* renderPipeline = mDevice.renderPipelinePayload(mRenderPipeline);
  if (renderPipeline == nullptr) return Result::InvalidHandle;

  if (renderPipeline->fanEmulation) {
    if (vertexCount < 3) return Result::InvalidArgument;
    id<MTLBuffer> fanBuffer = nil;
    if (const Result fan = mDevice.ensureFanIndexBuffer(&fanBuffer); !isSuccess(fan)) return fan;
    if (firstVertex + vertexCount > mDevice.fanIndexCapacity()) {
      mDevice.emitMessage(MessageSeverity::Error,
              "a triangle fan of " + std::to_string(firstVertex + vertexCount) + " vertices exceeds the " +
                      std::to_string(mDevice.fanIndexCapacity()) + "-vertex fan emulation capacity");
      return Result::Unsupported;
    }
    [mRenderEncoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                               indexCount:(vertexCount - 2) * 3
                                indexType:MTLIndexTypeUInt16
                              indexBuffer:fanBuffer
                        indexBufferOffset:0
                            instanceCount:instanceCount
                               baseVertex:static_cast<NSInteger>(firstVertex)
                             baseInstance:firstInstance];
  } else {
    [mRenderEncoder drawPrimitives:renderPipeline->primitiveType
                       vertexStart:firstVertex
                       vertexCount:vertexCount
                     instanceCount:instanceCount
                      baseInstance:firstInstance];
  }
  ++mCounts.draws;
  return Result::Success;
}

Result MetalCommandBuffer::drawIndexed(const uint32_t indexCount, const uint32_t instanceCount,
        const uint32_t firstIndex, const int32_t vertexOffset, const uint32_t firstInstance) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  if (const Result pass = requireRenderPass(); !isSuccess(pass)) return pass;
  if (const Result pipeline = requireRenderPipeline(); !isSuccess(pipeline)) return pipeline;
  if (!mIndexBufferSet) {
    mDevice.emitMessage(MessageSeverity::Error, "an indexed draw was recorded without an index buffer");
    return Result::InvalidArgument;
  }
  if (const Result state = applyRenderState(); !isSuccess(state)) return state;

  const MetalRenderPipeline* renderPipeline = mDevice.renderPipelinePayload(mRenderPipeline);
  if (renderPipeline == nullptr) return Result::InvalidHandle;
  const MetalBuffer* indices = mDevice.bufferPayload(mIndexBuffer);
  if (indices == nullptr) return Result::InvalidHandle;

  // Unlike Vulkan, the index buffer is not encoder state on Metal: it is passed to the draw call, so a preceding fan
  // draw has nothing to restore and the caller's buffer is always the one read.

  [mRenderEncoder drawIndexedPrimitives:renderPipeline->primitiveType
                             indexCount:indexCount
                              indexType:toMtlIndexType(mIndexFormat)
                            indexBuffer:indices->buffer
                      indexBufferOffset:static_cast<NSUInteger>(mIndexOffset + firstIndex * indexByteSize(mIndexFormat))
                          instanceCount:instanceCount
                             baseVertex:vertexOffset
                           baseInstance:firstInstance];
  ++mCounts.indexedDraws;
  return Result::Success;
}

Result MetalCommandBuffer::drawIndirect(const BufferHandle arguments, const uint64_t offset) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  if (const Result pass = requireRenderPass(); !isSuccess(pass)) return pass;
  if (const Result pipeline = requireRenderPipeline(); !isSuccess(pipeline)) return pipeline;
  const MetalBuffer* buffer = mDevice.bufferPayload(arguments);
  if (buffer == nullptr) return Result::InvalidHandle;
  if (const Result state = applyRenderState(); !isSuccess(state)) return state;
  const MetalRenderPipeline* renderPipeline = mDevice.renderPipelinePayload(mRenderPipeline);
  if (renderPipeline == nullptr) return Result::InvalidHandle;

  [mRenderEncoder drawPrimitives:renderPipeline->primitiveType
                  indirectBuffer:buffer->buffer
            indirectBufferOffset:static_cast<NSUInteger>(offset)];
  ++mCounts.indirectDraws;
  return Result::Success;
}

Result MetalCommandBuffer::drawIndexedIndirect(const BufferHandle arguments, const uint64_t offset) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  if (const Result pass = requireRenderPass(); !isSuccess(pass)) return pass;
  if (const Result pipeline = requireRenderPipeline(); !isSuccess(pipeline)) return pipeline;
  if (!mIndexBufferSet) return Result::InvalidArgument;
  const MetalBuffer* buffer = mDevice.bufferPayload(arguments);
  const MetalBuffer* indices = mDevice.bufferPayload(mIndexBuffer);
  if (buffer == nullptr || indices == nullptr) return Result::InvalidHandle;
  if (const Result state = applyRenderState(); !isSuccess(state)) return state;
  const MetalRenderPipeline* renderPipeline = mDevice.renderPipelinePayload(mRenderPipeline);
  if (renderPipeline == nullptr) return Result::InvalidHandle;

  [mRenderEncoder drawIndexedPrimitives:renderPipeline->primitiveType
                              indexType:toMtlIndexType(mIndexFormat)
                            indexBuffer:indices->buffer
                      indexBufferOffset:static_cast<NSUInteger>(mIndexOffset)
                         indirectBuffer:buffer->buffer
                   indirectBufferOffset:static_cast<NSUInteger>(offset)];
  ++mCounts.indirectDraws;
  return Result::Success;
}

Result MetalCommandBuffer::dispatch(const uint32_t groupCountX, const uint32_t groupCountY,
        const uint32_t groupCountZ) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  const MetalComputePipeline* pipeline = mDevice.computePipelinePayload(mComputePipeline);
  if (pipeline == nullptr) {
    mDevice.emitMessage(MessageSeverity::Error, "a dispatch was recorded without a bound compute pipeline");
    return Result::InvalidArgument;
  }
  if (const Result encoder = ensureComputeEncoder(); !isSuccess(encoder)) return encoder;
  [mComputeEncoder setComputePipelineState:pipeline->state];
  applyComputeBindings(mComputeEncoder);
  [mComputeEncoder dispatchThreadgroups:MTLSizeMake(groupCountX, groupCountY, groupCountZ)
                 threadsPerThreadgroup:MTLSizeMake(pipeline->workgroupSizeX, pipeline->workgroupSizeY,
                         pipeline->workgroupSizeZ)];
  ++mCounts.dispatches;
  return Result::Success;
}

Result MetalCommandBuffer::dispatchIndirect(const BufferHandle arguments, const uint64_t offset) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  const MetalComputePipeline* pipeline = mDevice.computePipelinePayload(mComputePipeline);
  if (pipeline == nullptr) return Result::InvalidArgument;
  const MetalBuffer* buffer = mDevice.bufferPayload(arguments);
  if (buffer == nullptr) return Result::InvalidHandle;
  if (const Result encoder = ensureComputeEncoder(); !isSuccess(encoder)) return encoder;
  [mComputeEncoder setComputePipelineState:pipeline->state];
  applyComputeBindings(mComputeEncoder);
  [mComputeEncoder dispatchThreadgroupsWithIndirectBuffer:buffer->buffer
                                    indirectBufferOffset:static_cast<NSUInteger>(offset)
                                   threadsPerThreadgroup:MTLSizeMake(pipeline->workgroupSizeX,
                                           pipeline->workgroupSizeY, pipeline->workgroupSizeZ)];
  ++mCounts.indirectDraws;
  return Result::Success;
}

Result MetalCommandBuffer::insertDebugMarker(const char* label) {
  if (label == nullptr) return Result::Success;
  NSString* name = [NSString stringWithUTF8String:label];
  if (mRenderEncoder != nil) [mRenderEncoder insertDebugSignpost:name];
  else if (mComputeEncoder != nil) [mComputeEncoder insertDebugSignpost:name];
  return Result::Success;
}

Result MetalCommandBuffer::beginDebugMarker(const char* label) {
  if (label == nullptr) return Result::Success;
  NSString* name = [NSString stringWithUTF8String:label];
  if (mRenderEncoder != nil) [mRenderEncoder pushDebugGroup:name];
  else if (mComputeEncoder != nil) [mComputeEncoder pushDebugGroup:name];
  mDebugGroupDepth += 1;
  return Result::Success;
}

Result MetalCommandBuffer::endDebugMarker() {
  if (mDebugGroupDepth == 0) return Result::InvalidArgument;
  if (mRenderEncoder != nil) [mRenderEncoder popDebugGroup];
  else if (mComputeEncoder != nil) [mComputeEncoder popDebugGroup];
  mDebugGroupDepth -= 1;
  return Result::Success;
}

// ==================================================================================================
// Transfer
// ==================================================================================================

Result MetalCommandBuffer::stage(const void* data, const size_t size, const size_t alignment,
        id<MTLBuffer>* outBuffer, uint64_t* outOffset) {
  (void) alignment;
  if (outBuffer == nullptr || outOffset == nullptr) return Result::InvalidArgument;
  if (size == 0) {
    *outBuffer = nil;
    *outOffset = 0;
    return Result::Success;
  }
  // One buffer per staged write. A bump arena would be faster, but a blit that outlives the recording would then pin
  // the whole arena; per-write buffers are released together when the recording is reset, which is the same lifetime
  // with a simpler failure mode.
  id<MTLBuffer> staging = [mDevice.metalDevice() newBufferWithBytes:data
                                                             length:size
                                                            options:MTLResourceStorageModeShared];
  if (staging == nil) return Result::OutOfMemory;
  staging.label = @"coral/staging";
  mStagingBuffers.push_back(staging);
  *outBuffer = staging;
  *outOffset = 0;
  return Result::Success;
}

Result MetalCommandBuffer::writeBuffer(const BufferHandle bufferHandle, const uint64_t offset, const void* data,
        const size_t dataSize) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  const MetalBuffer* buffer = mDevice.bufferPayload(bufferHandle);
  if (buffer == nullptr) return Result::InvalidHandle;
  if (data == nullptr || dataSize == 0) return Result::InvalidArgument;
  if (offset > buffer->size || dataSize > buffer->size - offset) return Result::InvalidArgument;

  id<MTLBuffer> staging = nil;
  uint64_t stagingOffset = 0;
  if (const Result staged = stage(data, dataSize, 4, &staging, &stagingOffset); !isSuccess(staged)) return staged;
  if (const Result encoder = ensureBlitEncoder(); !isSuccess(encoder)) return encoder;
  [mBlitEncoder copyFromBuffer:staging
                  sourceOffset:static_cast<NSUInteger>(stagingOffset)
                      toBuffer:buffer->buffer
             destinationOffset:static_cast<NSUInteger>(offset)
                          size:dataSize];
  ++mCounts.copies;
  return Result::Success;
}

Result MetalCommandBuffer::copyBufferToBuffer(const BufferHandle src, const uint64_t srcOffset, const BufferHandle dst,
        const uint64_t dstOffset, const uint64_t size) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  const MetalBuffer* source = mDevice.bufferPayload(src);
  const MetalBuffer* destination = mDevice.bufferPayload(dst);
  if (source == nullptr || destination == nullptr) return Result::InvalidHandle;
  if (srcOffset > source->size || size > source->size - srcOffset) return Result::InvalidArgument;
  if (dstOffset > destination->size || size > destination->size - dstOffset) return Result::InvalidArgument;
  if (const Result encoder = ensureBlitEncoder(); !isSuccess(encoder)) return encoder;
  [mBlitEncoder copyFromBuffer:source->buffer
                  sourceOffset:static_cast<NSUInteger>(srcOffset)
                      toBuffer:destination->buffer
             destinationOffset:static_cast<NSUInteger>(dstOffset)
                          size:size];
  ++mCounts.copies;
  return Result::Success;
}

Result MetalCommandBuffer::writeTexture(const TextureHandle textureHandle, const TextureCopyRegion& region,
        const void* data, const uint64_t dataSize, const uint32_t bytesPerRow) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  const MetalTexture* texture = mDevice.texturePayload(textureHandle);
  if (texture == nullptr || texture->texture == nil) return Result::InvalidHandle;
  if (data == nullptr) return Result::InvalidArgument;

  const PixelFormatInfo& format = getPixelFormatInfo(texture->format);
  if (format.bytesPerBlock == 0 || isCompressedFormat(texture->format)) {
    mDevice.emitMessage(MessageSeverity::Error,
            "writeTexture does not support compressed formats; upload the block data with copyBufferToTexture");
    return Result::Unsupported;
  }

  const uint32_t width = region.size.width != 0 ? region.size.width : static_cast<uint32_t>(texture->texture.width);
  const uint32_t height = region.size.height != 0 ? region.size.height : 1;
  const uint32_t layers = std::max(1u, region.arrayLayerCount);
  const uint32_t tightPitch = width * format.bytesPerBlock;
  const uint32_t sourcePitch = bytesPerRow != 0 ? bytesPerRow : tightPitch;
  const uint64_t required = static_cast<uint64_t>(sourcePitch) * height * layers;
  if (dataSize < required) {
    mDevice.emitMessage(MessageSeverity::Error, "writeTexture was given fewer bytes than the region needs");
    return Result::InvalidArgument;
  }

  // Metal's blit copy requires a row pitch that is a multiple of four. A caller uploading a one-byte-per-pixel texture
  // whose width is not a multiple of four gets a repacked staging buffer rather than a validation failure.
  id<MTLBuffer> staging = nil;
  uint64_t stagingOffset = 0;
  const uint32_t alignedPitch = (sourcePitch + 3u) & ~3u;
  if (alignedPitch == sourcePitch) {
    if (const Result staged = stage(data, static_cast<size_t>(required), 4, &staging, &stagingOffset); !isSuccess(staged)) {
      return staged;
    }
  } else {
    const size_t stagedSize = static_cast<size_t>(alignedPitch) * height * layers;
    id<MTLBuffer> repacked = [mDevice.metalDevice() newBufferWithLength:stagedSize
                                                               options:MTLResourceStorageModeShared];
    if (repacked == nil) return Result::OutOfMemory;
    const auto* source = static_cast<const uint8_t*>(data);
    auto* destination = static_cast<uint8_t*>(repacked.contents);
    for (uint32_t layer = 0; layer < layers; ++layer) {
      for (uint32_t row = 0; row < height; ++row) {
        std::memcpy(destination + static_cast<size_t>(layer * height + row) * alignedPitch,
                source + static_cast<size_t>(layer * height + row) * sourcePitch, sourcePitch);
      }
    }
    repacked.label = @"coral/staging-repacked";
    mStagingBuffers.push_back(repacked);
    staging = repacked;
    stagingOffset = 0;
  }

  if (const Result encoder = ensureBlitEncoder(); !isSuccess(encoder)) return encoder;
  for (uint32_t layer = 0; layer < layers; ++layer) {
    [mBlitEncoder
            copyFromBuffer:staging
              sourceOffset:static_cast<NSUInteger>(stagingOffset + static_cast<uint64_t>(layer) * alignedPitch * height)
          sourceBytesPerRow:alignedPitch
        sourceBytesPerImage:static_cast<NSUInteger>(alignedPitch) * height
                 sourceSize:MTLSizeMake(width, height, 1)
                  toTexture:texture->texture
           destinationSlice:region.baseArrayLayer + layer
           destinationLevel:region.mipLevel
          destinationOrigin:MTLOriginMake(region.textureOrigin.x, region.textureOrigin.y, region.textureOrigin.z)];
  }
  ++mCounts.copies;
  return Result::Success;
}

Result MetalCommandBuffer::copyBufferToTexture(const BufferTextureCopy& copy) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  const MetalBuffer* buffer = mDevice.bufferPayload(copy.buffer);
  const MetalTexture* texture = mDevice.texturePayload(copy.texture);
  if (buffer == nullptr || texture == nullptr) return Result::InvalidHandle;

  const PixelFormatInfo& format = getPixelFormatInfo(texture->format);
  const uint32_t width = copy.textureRegion.size.width;
  const uint32_t height = copy.textureRegion.size.height;
  const uint32_t rowPitch = copy.textureRegion.bytesPerRow != 0 ? copy.textureRegion.bytesPerRow
                                                                : width * format.bytesPerBlock;
  const uint32_t depth = texture->type == MTLTextureType3D ? std::max(1u, copy.textureRegion.size.depthOrArrayLayers) : 1;
  const uint32_t layers = texture->type == MTLTextureType3D || texture->type == MTLTextureType2D ||
                                          texture->type == MTLTextureType2DMultisample
                                  ? 1
                                  : std::max(1u, copy.textureRegion.arrayLayerCount);

  if (const Result encoder = ensureBlitEncoder(); !isSuccess(encoder)) return encoder;
  for (uint32_t layer = 0; layer < layers; ++layer) {
    [mBlitEncoder copyFromBuffer:buffer->buffer
                    sourceOffset:static_cast<NSUInteger>(copy.bufferRegion.srcOffset +
                            static_cast<uint64_t>(layer) * rowPitch * height)
                sourceBytesPerRow:rowPitch
              sourceBytesPerImage:static_cast<NSUInteger>(rowPitch) * height
                       sourceSize:MTLSizeMake(width, height, depth)
                        toTexture:texture->texture
                 destinationSlice:copy.textureRegion.baseArrayLayer + layer
                 destinationLevel:copy.textureRegion.mipLevel
                destinationOrigin:MTLOriginMake(copy.textureRegion.textureOrigin.x, copy.textureRegion.textureOrigin.y,
                        copy.textureRegion.textureOrigin.z)];
  }
  ++mCounts.copies;
  return Result::Success;
}

Result MetalCommandBuffer::copyTextureToBuffer(const BufferTextureCopy& copy) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  const MetalBuffer* buffer = mDevice.bufferPayload(copy.buffer);
  const MetalTexture* texture = mDevice.texturePayload(copy.texture);
  if (buffer == nullptr || texture == nullptr) return Result::InvalidHandle;

  const PixelFormatInfo& format = getPixelFormatInfo(texture->format);
  const uint32_t width = copy.textureRegion.size.width;
  const uint32_t height = copy.textureRegion.size.height;
  const uint32_t rowPitch = copy.textureRegion.bytesPerRow != 0 ? copy.textureRegion.bytesPerRow
                                                                : width * format.bytesPerBlock;
  if (rowPitch % 4 != 0) {
    mDevice.emitMessage(MessageSeverity::Error,
            "copyTextureToBuffer needs a row pitch that is a multiple of 4 on Metal; pass TextureCopyRegion::bytesPerRow");
    return Result::Unsupported;
  }
  if (copy.bufferRegion.dstOffset > buffer->size ||
      static_cast<uint64_t>(rowPitch) * height * std::max(1u, copy.textureRegion.arrayLayerCount) >
              buffer->size - copy.bufferRegion.dstOffset) {
    return Result::InvalidArgument;
  }

  const uint32_t layers = std::max(1u, copy.textureRegion.arrayLayerCount);
  const uint32_t depth = texture->type == MTLTextureType3D ? std::max(1u, copy.textureRegion.size.depthOrArrayLayers) : 1;
  if (const Result encoder = ensureBlitEncoder(); !isSuccess(encoder)) return encoder;
  // One blit per array slice: a 2D-array copy names one slice at a time on Metal, and the destination offset advances
  // by the image size the caller's pitch implies.
  for (uint32_t layer = 0; layer < layers; ++layer) {
    [mBlitEncoder copyFromTexture:texture->texture
                      sourceSlice:copy.textureRegion.baseArrayLayer + layer
                      sourceLevel:copy.textureRegion.mipLevel
                     sourceOrigin:MTLOriginMake(copy.textureRegion.textureOrigin.x, copy.textureRegion.textureOrigin.y,
                             copy.textureRegion.textureOrigin.z)
                       sourceSize:MTLSizeMake(width, height, depth)
                         toBuffer:buffer->buffer
                destinationOffset:static_cast<NSUInteger>(copy.bufferRegion.dstOffset +
                        static_cast<uint64_t>(layer) * rowPitch * height)
             destinationBytesPerRow:rowPitch
           destinationBytesPerImage:static_cast<NSUInteger>(rowPitch) * height];
  }
  ++mCounts.copies;
  return Result::Success;
}

Result MetalCommandBuffer::copyTextureToTexture(const TextureHandle src, const TextureHandle dst,
        const TextureCopyRegion& region) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  const MetalTexture* source = mDevice.texturePayload(src);
  const MetalTexture* destination = mDevice.texturePayload(dst);
  if (source == nullptr || destination == nullptr) return Result::InvalidHandle;
  if (const Result encoder = ensureBlitEncoder(); !isSuccess(encoder)) return encoder;
  [mBlitEncoder copyFromTexture:source->texture
                    sourceSlice:region.baseArrayLayer
                    sourceLevel:region.mipLevel
                   sourceOrigin:MTLOriginMake(region.textureOrigin.x, region.textureOrigin.y, region.textureOrigin.z)
                     sourceSize:MTLSizeMake(region.size.width, region.size.height,
                             std::max(1u, region.size.depthOrArrayLayers))
                      toTexture:destination->texture
               destinationSlice:region.baseArrayLayer
               destinationLevel:region.mipLevel
              destinationOrigin:MTLOriginMake(region.textureOrigin.x, region.textureOrigin.y, region.textureOrigin.z)];
  ++mCounts.copies;
  return Result::Success;
}

Result MetalCommandBuffer::clearTextureWithRenderPass(const TextureHandle textureHandle,
        const TextureSubresourceRange& range, const Color& color, const bool depthStencil,
        const DepthStencilValue& depthStencilValue) {
  const MetalTexture* texture = mDevice.texturePayload(textureHandle);
  if (texture == nullptr || texture->texture == nil) return Result::InvalidHandle;

  const uint32_t totalMips = texture->texture.mipmapLevelCount;
  uint32_t totalLayers = 1;
  if (texture->type == MTLTextureType2DArray || texture->type == MTLTextureTypeCubeArray ||
      texture->type == MTLTextureType2DMultisampleArray) {
    totalLayers = static_cast<uint32_t>(texture->texture.arrayLength);
  } else if (texture->type == MTLTextureTypeCube) {
    totalLayers = 6;
  }
  const uint32_t baseMip = std::min(range.baseMipLevel, totalMips - 1);
  const uint32_t mipCount = range.mipLevelCount == 0 ? totalMips - baseMip : range.mipLevelCount;
  const uint32_t baseLayer = std::min(range.baseArrayLayer, totalLayers - 1);
  const uint32_t layerCount = range.arrayLayerCount == 0 ? totalLayers - baseLayer : range.arrayLayerCount;
  if (baseMip != 0 || mipCount != totalMips || baseLayer != 0 || layerCount != totalLayers) {
    // Metal's clear is a render-pass load action and always covers the whole attachment; a partial clear would need a
    // scissored pass, which is a different operation with different hazards. Refusing keeps the caller's range honest.
    mDevice.emitMessage(MessageSeverity::Error,
            "clearColorTexture/clearDepthStencilTexture on Metal clears the whole texture; a subresource range is refused");
    return Result::Unsupported;
  }

  // A render pass with only a clear is how Metal expresses an image clear. The pass is suspended if one is open; its
  // attachments were stored, and the caller's next draw resumes it with a load action.
  if (mRenderPassOpen) {
    if (const Result suspended = suspendRenderPass(); !isSuccess(suspended)) return suspended;
  }
  endAllEncoders(false);

  MTLRenderPassDescriptor* descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
  if (depthStencil) {
    if (!isDepthFormat(texture->format)) return Result::InvalidArgument;
    descriptor.depthAttachment.texture = texture->texture;
    descriptor.depthAttachment.loadAction = MTLLoadActionClear;
    descriptor.depthAttachment.clearDepth = depthStencilValue.depth;
    descriptor.depthAttachment.storeAction = MTLStoreActionStore;
    if (isStencilFormat(texture->format)) {
      descriptor.stencilAttachment.texture = texture->texture;
      descriptor.stencilAttachment.loadAction = MTLLoadActionClear;
      descriptor.stencilAttachment.clearStencil = depthStencilValue.stencil;
      descriptor.stencilAttachment.storeAction = MTLStoreActionStore;
    }
  } else {
    descriptor.colorAttachments[0].texture = texture->texture;
    descriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
    descriptor.colorAttachments[0].clearColor = toClearColor(color);
    descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
  }
  id<MTLRenderCommandEncoder> encoder = [mCommandBuffer renderCommandEncoderWithDescriptor:descriptor];
  if (encoder == nil) return Result::Error;
  [encoder endEncoding];
  mCounts.copies += 1;
  return Result::Success;
}

Result MetalCommandBuffer::clearColorTexture(const TextureHandle texture, const TextureSubresourceRange& range,
        const Color& color) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  return clearTextureWithRenderPass(texture, range, color, false, {});
}

Result MetalCommandBuffer::clearDepthStencilTexture(const TextureHandle texture, const TextureSubresourceRange& range,
        const DepthStencilValue& value) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  return clearTextureWithRenderPass(texture, range, Color::black(), true, value);
}

Result MetalCommandBuffer::pipelineBarrier() {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  // Metal tracks resource hazards itself; an explicit barrier is only needed for untracked resources, which this backend
  // never creates. The call is still counted so `dumpCommandCounts` shows that a recorder asked for one.
  ++mCounts.barriers;
  return Result::Success;
}

// ==================================================================================================
// Queries
// ==================================================================================================

Result MetalCommandBuffer::resetQuerySet(const QuerySetHandle querySetHandle, const uint32_t firstQuery,
        const uint32_t count) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  MetalQuerySet* querySet = mDevice.querySetPayload(querySetHandle);
  if (querySet == nullptr) return Result::InvalidHandle;
  if (firstQuery + count > querySet->count) return Result::InvalidArgument;
  if (querySet->type == QueryType::Occlusion && querySet->visibilityBuffer != nil) {
    std::memset(static_cast<uint8_t*>(querySet->visibilityBuffer.contents) + firstQuery * sizeof(uint64_t), 0,
            count * sizeof(uint64_t));
  }
  return Result::Success;
}

Result MetalCommandBuffer::beginQuery(const QuerySetHandle querySetHandle, const uint32_t query) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  MetalQuerySet* querySet = mDevice.querySetPayload(querySetHandle);
  if (querySet == nullptr) return Result::InvalidHandle;
  if (query >= querySet->count) return Result::InvalidArgument;
  if (querySet->type != QueryType::Occlusion) {
    // A timestamp is sampled by `endQuery`; there is nothing to begin.
    return Result::Success;
  }
  if (const Result encoder = ensureRenderEncoder(); !isSuccess(encoder)) return encoder;
  [mRenderEncoder setVisibilityResultMode:MTLVisibilityResultModeCounting offset:query * sizeof(uint64_t)];
  return Result::Success;
}

Result MetalCommandBuffer::endQuery(const QuerySetHandle querySetHandle, const uint32_t query) {
  if (const Result recording = requireRecording(); !isSuccess(recording)) return recording;
  MetalQuerySet* querySet = mDevice.querySetPayload(querySetHandle);
  if (querySet == nullptr) return Result::InvalidHandle;
  if (query >= querySet->count) return Result::InvalidArgument;

  if (querySet->type == QueryType::Occlusion) {
    if (const Result encoder = ensureRenderEncoder(); !isSuccess(encoder)) return encoder;
    [mRenderEncoder setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
    return Result::Success;
  }

  if (querySet->counterSampleBuffer == nil) return Result::Unsupported;
  if (const Result encoder = ensureBlitEncoder(); !isSuccess(encoder)) return encoder;
  [mBlitEncoder sampleCountersInBuffer:querySet->counterSampleBuffer atSampleIndex:query withBarrier:NO];
  return Result::Success;
}

void MetalCommandBuffer::setLabel(const char* label) {
  if (label == nullptr) return;
  mLabel = label;
  if (mCommandBuffer != nil) mCommandBuffer.label = [NSString stringWithUTF8String:label];
}

// ==================================================================================================
// Submission
// ==================================================================================================

Result MetalCommandBuffer::submit(const FenceHandle fence, const SemaphoreHandle waitSemaphore,
        const SemaphoreHandle signalSemaphore) {
  if (!mEnded || mCommandBuffer == nil || mSubmitted) {
    mDevice.emitMessage(MessageSeverity::Error, "a command buffer can only be submitted once, after end()");
    return Result::InvalidArgument;
  }

  // Every encoder must be closed before the command buffer is committed.
  endAllEncoders(false);

  if (waitSemaphore.valid()) {
    MetalSemaphore* semaphore = mDevice.semaphorePayload(waitSemaphore);
    if (semaphore == nullptr) return Result::InvalidHandle;
    [mCommandBuffer encodeWaitForEvent:semaphore->event value:semaphore->value];
  }
  if (signalSemaphore.valid()) {
    MetalSemaphore* semaphore = mDevice.semaphorePayload(signalSemaphore);
    if (semaphore == nullptr) return Result::InvalidHandle;
    semaphore->value += 1;
    [mCommandBuffer encodeSignalEvent:semaphore->event value:semaphore->value];
  }

  MetalFence* fencePayload = nullptr;
  if (fence.valid()) {
    fencePayload = mDevice.fencePayload(fence);
    if (fencePayload == nullptr) return Result::InvalidHandle;
    fencePayload->target = std::max(fencePayload->target, fencePayload->event.signaledValue) + 1;
    fencePayload->signalled = false;
    fencePayload->pending += 1;
    [mCommandBuffer encodeSignalEvent:fencePayload->event value:fencePayload->target];
  }

  // The device's completion timeline is signalled by every submission, which is what gives `waitIdle` one value to wait
  // for rather than a list of command buffers to remember.
  const uint64_t completion = mDevice.nextCompletionValue();
  [mCommandBuffer encodeSignalEvent:mDevice.completionEvent() value:completion];

  mSubmitted = true;
  [mCommandBuffer commit];

  if (fencePayload == nullptr) {
    [mCommandBuffer waitUntilCompleted];
    if (mCommandBuffer.status == MTLCommandBufferStatusError) {
      mDevice.emitMessage(MessageSeverity::Error,
              std::string("a Metal submission failed: ") + describe(mCommandBuffer.error));
      return Result::Error;
    }
  } else {
    mDevice.noteAsyncSubmission(1);
  }
  return Result::Success;
}

} // namespace backend::metal
