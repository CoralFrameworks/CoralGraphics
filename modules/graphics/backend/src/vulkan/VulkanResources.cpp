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

/**
 * @file VulkanResources.cpp
 *
 * Descriptor sets, pipeline layouts, pipelines and render-pass keys.
 *
 * The translation is mostly one-to-one, and the places where it is not are worth naming:
 *
 *  - A `RenderPass` is a compatibility key, not a driver object. The RHI follows the dynamic-rendering
 *    model, so this file only validates the shape and records it; the attachments and load operations
 *    arrive at `beginRenderPass`.
 *  - `PrimitiveTopology::TriangleFan` has no Vulkan equivalent. The pipeline is created as a triangle
 *    list and the *draw* is turned into an indexed list against the device's fan index buffer, which
 *    is why the pipeline records the flag rather than failing.
 *  - A pipeline layout declares one 128 byte push-constant range visible to every stage. The RHI's
 *    pipeline-layout descriptor has no push-constant range field because OpenGL has no such concept;
 *    the budget is a device property (`Limits::maxPushConstantSize`), so the two agree by
 *    construction.
 */

#include "private/backend/vulkan/VulkanDevice.h"

#include "private/backend/vulkan/VulkanEnums.h"
#include "private/backend/vulkan/VulkanAndroidHardwareBuffer.h"
#include "private/backend/vulkan/VulkanEntryPoints.h"

#include <backend/Validation.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <string>

namespace backend::vulkan {
namespace {

/** Maps a Vulkan result onto the RHI's vocabulary. Shared with the device file by intent, not by link. */
Result toResult(const VkResult result) noexcept {
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

const char* topologyName(const PrimitiveTopology topology) noexcept {
  switch (topology) {
    case PrimitiveTopology::PointList:
      return "PointList";
    case PrimitiveTopology::LineList:
      return "LineList";
    case PrimitiveTopology::LineStrip:
      return "LineStrip";
    case PrimitiveTopology::TriangleList:
      return "TriangleList";
    case PrimitiveTopology::TriangleStrip:
      return "TriangleStrip";
    case PrimitiveTopology::TriangleFan:
      return "TriangleFan";
    case PrimitiveTopology::PatchList:
      return "PatchList";
  }
  return "Unknown";
}

} // namespace anonymous

// ==================================================================================================
// Descriptor set layouts and sets
// ==================================================================================================

Result VulkanDevice::createDescriptorSetLayout(const DescriptorSetLayoutDescriptor& descriptor,
        DescriptorSetLayoutHandle* outLayout) {
  if (outLayout == nullptr) return Result::InvalidArgument;
  *outLayout = DescriptorSetLayoutHandle::invalid();

  const ValidationResult validation = validateDescriptorSetLayout(descriptor, mCapabilities);
  if (!validation.valid) {
    emitMessage(MessageSeverity::Error, validation.message);
    return Result::Unsupported;
  }

  VulkanDescriptorSetLayout layout;
  layout.bindingCount = descriptor.bindingCount;

  std::array<VkDescriptorSetLayoutBinding, kMaxDescriptorBindings> bindings{};
  for (uint32_t index = 0; index < descriptor.bindingCount; ++index) {
    const DescriptorBinding& binding = descriptor.bindings[index];
    VulkanDescriptorBinding stored;
    stored.binding = binding.binding;
    stored.type = binding.type;
    stored.arraySize = std::max(binding.count, 1u);
    stored.stageFlags = binding.stageFlags;
    if (binding.debugName != nullptr) stored.debugName = binding.debugName;
    layout.bindings[index] = stored;

    VkDescriptorSetLayoutBinding& vkBinding = bindings[index];
    vkBinding.binding = binding.binding;
    vkBinding.descriptorType = toVkDescriptorType(binding.type);
    vkBinding.descriptorCount = stored.arraySize;
    vkBinding.stageFlags = toVkStageFlags(binding.stageFlags);
    vkBinding.pImmutableSamplers = nullptr;
  }
  if (descriptor.label != nullptr) layout.label = descriptor.label;

  VkDescriptorSetLayoutCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  info.bindingCount = descriptor.bindingCount;
  info.pBindings = descriptor.bindingCount == 0 ? nullptr : bindings.data();

  const VkResult created = mApi->vkCreateDescriptorSetLayout(mDevice, &info, nullptr, &layout.layout);
  if (created != VK_SUCCESS) {
    emitMessage(MessageSeverity::Error, "the descriptor set layout was rejected by the driver");
    return toResult(created) == Result::Success ? Result::Error : toResult(created);
  }

  *outLayout = mDescriptorSetLayouts.allocate(std::move(layout));
  return outLayout->valid() ? Result::Success : Result::OutOfMemory;
}

void VulkanDevice::destroyDescriptorSetLayout(const DescriptorSetLayoutHandle layoutHandle) {
  if (VulkanDescriptorSetLayout* layout = mDescriptorSetLayouts.get(layoutHandle); layout != nullptr) {
    if (layout->layout != VK_NULL_HANDLE) mApi->vkDestroyDescriptorSetLayout(mDevice, layout->layout, nullptr);
    layout->layout = VK_NULL_HANDLE;
    mDescriptorSetLayouts.free(layoutHandle);
  }
}

Result VulkanDevice::createDescriptorSet(const DescriptorSetLayoutHandle layoutHandle, DescriptorSetHandle* outSet) {
  if (outSet == nullptr) return Result::InvalidArgument;
  *outSet = DescriptorSetHandle::invalid();

  const VulkanDescriptorSetLayout* layout = mDescriptorSetLayouts.get(layoutHandle);
  if (layout == nullptr) return Result::InvalidHandle;

  // A set is CPU-side state, exactly as it is on the OpenGL backend: the driver object is allocated at
  // the draw that uses it, because the RHI lets a caller refill a set between draws and Vulkan does not
  // allow a bound descriptor set's contents to change while a command buffer refers to it.
  VulkanDescriptorSet set;
  set.layout = layoutHandle;
  set.label = layout->label;
  for (uint32_t index = 0; index < layout->bindingCount; ++index) {
    const VulkanDescriptorBinding& binding = layout->bindings[index];
    for (uint32_t element = 0; element < binding.arraySize; ++element) {
      VulkanDescriptorBindingValue value;
      value.binding = binding.binding;
      value.type = binding.type;
      value.arraySize = binding.arraySize;
      value.arrayIndex = element;
      set.bindings.push_back(value);
    }
  }

  *outSet = mDescriptorSets.allocate(std::move(set));
  return outSet->valid() ? Result::Success : Result::OutOfMemory;
}

void VulkanDevice::destroyDescriptorSet(const DescriptorSetHandle set) { mDescriptorSets.free(set); }

Result VulkanDevice::updateDescriptorSet(const DescriptorSetHandle setHandle, const DescriptorWrite* writes,
        const uint32_t writeCount) {
  VulkanDescriptorSet* set = mDescriptorSets.get(setHandle);
  if (set == nullptr) return Result::InvalidHandle;
  if (writes == nullptr && writeCount != 0) return Result::InvalidArgument;

  for (uint32_t index = 0; index < writeCount; ++index) {
    const DescriptorWrite& write = writes[index];
    // A buffer may be far larger than the block size; what has to fit the limit is the range this write names.
    if (const ValidationResult validation = validateDescriptorWrite(write, mCapabilities); !validation.valid) {
      emitMessage(MessageSeverity::Error, validation.message);
      return Result::InvalidArgument;
    }

    VulkanDescriptorBindingValue* target = nullptr;
    for (VulkanDescriptorBindingValue& candidate: set->bindings) {
      if (candidate.binding == write.binding && candidate.arrayIndex == write.arrayElement) {
        target = &candidate;
        break;
      }
    }
    if (target == nullptr) {
      std::string message = "no descriptor binding ";
      message += std::to_string(write.binding);
      message += " element ";
      message += std::to_string(write.arrayElement);
      message += " exists in this set";
      emitMessage(MessageSeverity::Error, message);
      return Result::InvalidArgument;
    }

    switch (write.type) {
      case BindingType::TextureSampler: {
        if (write.view.valid() && mTextureViews.get(write.view) == nullptr) return Result::InvalidHandle;
        if (!write.view.valid() && mTextures.get(write.texture) == nullptr) return Result::InvalidHandle;
        if (write.sampler.valid() && mSamplers.get(write.sampler) == nullptr) return Result::InvalidHandle;
        target->texture = write.texture;
        target->view = write.view;
        target->sampler = write.sampler;
        break;
      }
      case BindingType::Texture: {
        if (mTextures.get(write.texture) == nullptr) return Result::InvalidHandle;
        target->texture = write.texture;
        if (write.sampler.valid()) target->sampler = write.sampler;
        break;
      }
      case BindingType::Sampler: {
        if (mSamplers.get(write.sampler) == nullptr) return Result::InvalidHandle;
        target->sampler = write.sampler;
        break;
      }
      case BindingType::UniformBuffer:
      case BindingType::ReadOnlyStorageBuffer:
      case BindingType::StorageBuffer: {
        if (write.buffer.buffer.valid() && mBuffers.get(write.buffer.buffer) == nullptr) {
          return Result::InvalidHandle;
        }
        target->buffer = write.buffer;
        break;
      }
    }
    target->written = true;
  }

  // A version rather than a dirty flag: two draws that use the set at two different versions must see
  // the two different contents, so the command buffer keys its descriptor snapshots on it.
  ++set->version;
  return Result::Success;
}

// ==================================================================================================
// Pipeline layouts
// ==================================================================================================

Result VulkanDevice::createPipelineLayout(const PipelineLayoutDescriptor& descriptor,
        PipelineLayoutHandle* outLayout) {
  if (outLayout == nullptr) return Result::InvalidArgument;
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

  VulkanPipelineLayout layout;
  layout.setLayoutCount = descriptor.setLayoutCount;

  std::array<VkDescriptorSetLayout, kMaxDescriptorSetLayouts> setLayouts{};
  for (uint32_t index = 0; index < descriptor.setLayoutCount; ++index) {
    if (descriptor.setLayouts[index].valid()) {
      const VulkanDescriptorSetLayout* setLayout = mDescriptorSetLayouts.get(descriptor.setLayouts[index]);
      if (setLayout == nullptr) return Result::InvalidHandle;
      setLayouts[index] = setLayout->layout;
      layout.setLayouts[index] = setLayout->layout;
    } else {
      // An invalid entry means "this set index is unused"; Vulkan still needs a layout for the slot.
      setLayouts[index] = mEmptyDescriptorSetLayout;
      layout.setLayouts[index] = mEmptyDescriptorSetLayout;
    }
  }
  if (descriptor.label != nullptr) layout.label = descriptor.label;

  VkPushConstantRange pushConstant{};
  pushConstant.stageFlags = allGraphicsStages() | VK_SHADER_STAGE_COMPUTE_BIT;
  pushConstant.offset = 0;
  pushConstant.size = mPushConstantBudget;

  VkPipelineLayoutCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  info.setLayoutCount = descriptor.setLayoutCount;
  info.pSetLayouts = descriptor.setLayoutCount == 0 ? nullptr : setLayouts.data();
  if (mPushConstantBudget >= 4) {
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &pushConstant;
  }

  if (const VkResult created = mApi->vkCreatePipelineLayout(mDevice, &info, nullptr, &layout.layout);
      created != VK_SUCCESS) {
    emitMessage(MessageSeverity::Error, "the pipeline layout was rejected by the driver");
    return toResult(created) == Result::Success ? Result::Error : toResult(created);
  }

  *outLayout = mPipelineLayouts.allocate(std::move(layout));
  return outLayout->valid() ? Result::Success : Result::OutOfMemory;
}

void VulkanDevice::destroyPipelineLayout(const PipelineLayoutHandle layoutHandle) {
  if (VulkanPipelineLayout* layout = mPipelineLayouts.get(layoutHandle); layout != nullptr) {
    if (layout->layout != VK_NULL_HANDLE) mApi->vkDestroyPipelineLayout(mDevice, layout->layout, nullptr);
    layout->layout = VK_NULL_HANDLE;
    mPipelineLayouts.free(layoutHandle);
  }
}

// ==================================================================================================
// Render passes and framebuffers
// ==================================================================================================

Result VulkanDevice::createRenderPass(const RenderPassDescriptor& descriptor, RenderPassHandle* outPass) {
  if (outPass == nullptr) return Result::InvalidArgument;
  *outPass = RenderPassHandle::invalid();

  const ValidationResult validation = validateRenderPassDescriptor(descriptor, mCapabilities);
  if (!validation.valid) {
    emitMessage(MessageSeverity::Error, validation.message);
    return Result::Unsupported;
  }

  VulkanRenderPass pass;
  pass.colorAttachmentCount = descriptor.colorAttachmentCount;
  for (uint32_t index = 0; index < descriptor.colorAttachmentCount; ++index) {
    pass.colorFormats[index] = descriptor.colorFormats[index];
  }
  pass.depthStencilFormat = descriptor.depthStencilFormat;
  pass.sampleCount = std::max(descriptor.sampleCount, 1u);
  if (descriptor.label != nullptr) pass.label = descriptor.label;

  // ## The Vulkan 1.0/1.1 fallback
  //
  // Dynamic rendering is Vulkan 1.3 core or an extension; a 1.1 device needs the render-pass route.
  // The pass is built with LOAD/STORE and GENERAL layouts, and a clear is issued with
  // `vkCmdClearAttachments` when a caller asks for one. Baking CLEAR into the attachment description
  // would make every second pass onto the same target wipe it, which is exactly what a layer or a
  // ping-pong target must not do.
  if (!usesDynamicRendering()) {
    std::array<VkAttachmentDescription, kMaxColorAttachments + 1> attachments{};
    std::array<VkAttachmentReference, kMaxColorAttachments> colorReferences{};
    uint32_t attachmentCount = 0;
    const VkSampleCountFlagBits samples = static_cast<VkSampleCountFlagBits>(pass.sampleCount);

    for (uint32_t index = 0; index < descriptor.colorAttachmentCount; ++index) {
      VkAttachmentDescription& attachment = attachments[attachmentCount];
      attachment.format = toVkFormat(descriptor.colorFormats[index]);
      attachment.samples = samples;
      attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachment.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
      attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
      colorReferences[index] = { attachmentCount, VK_IMAGE_LAYOUT_GENERAL };
      ++attachmentCount;
    }

    bool hasDepth = descriptor.depthStencilFormat != PixelFormat::Undefined;
    VkAttachmentReference depthReference{};
    if (hasDepth) {
      VkAttachmentDescription& attachment = attachments[attachmentCount];
      attachment.format = toVkFormat(descriptor.depthStencilFormat);
      attachment.samples = samples;
      attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachment.stencilLoadOp = isStencilFormat(descriptor.depthStencilFormat) ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                                                : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachment.stencilStoreOp = isStencilFormat(descriptor.depthStencilFormat) ? VK_ATTACHMENT_STORE_OP_STORE
                                                                                 : VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachment.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
      attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
      depthReference = { attachmentCount, VK_IMAGE_LAYOUT_GENERAL };
      ++attachmentCount;
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = descriptor.colorAttachmentCount;
    subpass.pColorAttachments = descriptor.colorAttachmentCount == 0 ? nullptr : colorReferences.data();
    subpass.pDepthStencilAttachment = hasDepth ? &depthReference : nullptr;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = dependency.srcStageMask;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = attachmentCount;
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    if (const VkResult created = mApi->vkCreateRenderPass(mDevice, &info, nullptr, &pass.renderPass);
        created != VK_SUCCESS) {
      emitMessage(MessageSeverity::Error, "the Vulkan 1.0/1.1 render pass could not be created");
      return toResult(created);
    }
  }

  const VkRenderPass createdPass = pass.renderPass;
  *outPass = mRenderPasses.allocate(std::move(pass));
  if (!outPass->valid()) {
    if (createdPass != VK_NULL_HANDLE && mApi->vkDestroyRenderPass != nullptr) {
      mApi->vkDestroyRenderPass(mDevice, createdPass, nullptr);
    }
    return Result::OutOfMemory;
  }
  return Result::Success;
}

void VulkanDevice::destroyRenderPass(const RenderPassHandle renderPass) {
  if (VulkanRenderPass* pass = mRenderPasses.get(renderPass); pass != nullptr) {
    releaseFramebuffersFor(pass->renderPass);
    if (pass->renderPass != VK_NULL_HANDLE && mApi->vkDestroyRenderPass != nullptr) {
      mApi->vkDestroyRenderPass(mDevice, pass->renderPass, nullptr);
      pass->renderPass = VK_NULL_HANDLE;
    }
    mRenderPasses.free(renderPass);
  }
}

VkFramebuffer VulkanDevice::framebufferFor(const VkRenderPass renderPass, const VkImageView* colors,
        const uint32_t colorCount, const VkImageView depth, const uint32_t width, const uint32_t height) {
  if (mDevice == VK_NULL_HANDLE || renderPass == VK_NULL_HANDLE || colorCount > kMaxColorAttachments ||
      mApi->vkCreateFramebuffer == nullptr) {
    return VK_NULL_HANDLE;
  }

  std::lock_guard<std::mutex> lock(mFramebufferMutex);
  VulkanFramebufferKey key;
  key.renderPass = renderPass;
  key.colorCount = colorCount;
  key.width = width;
  key.height = height;
  for (uint32_t index = 0; index < colorCount; ++index) key.colors[index] = colors[index];
  key.depth = depth;

  if (const auto found = mFallbackFramebuffers.find(key); found != mFallbackFramebuffers.end()) {
    return found->second;
  }

  std::array<VkImageView, kMaxColorAttachments + 1> attachments{};
  uint32_t attachmentCount = 0;
  for (uint32_t index = 0; index < colorCount; ++index) attachments[attachmentCount++] = colors[index];
  if (depth != VK_NULL_HANDLE) attachments[attachmentCount++] = depth;

  VkFramebufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  info.renderPass = renderPass;
  info.attachmentCount = attachmentCount;
  info.pAttachments = attachments.data();
  info.width = width;
  info.height = height;
  info.layers = 1;

  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  if (const VkResult created = mApi->vkCreateFramebuffer(mDevice, &info, nullptr, &framebuffer);
      created != VK_SUCCESS) {
    emitMessage(MessageSeverity::Error, "the Vulkan 1.0/1.1 framebuffer could not be created");
    return VK_NULL_HANDLE;
  }
  mFallbackFramebuffers.emplace(key, framebuffer);
  return framebuffer;
}

void VulkanDevice::releaseFramebuffersFor(const VkRenderPass renderPass) {
  std::lock_guard<std::mutex> lock(mFramebufferMutex);
  for (auto it = mFallbackFramebuffers.begin(); it != mFallbackFramebuffers.end();) {
    if (it->first.renderPass != renderPass) {
      ++it;
      continue;
    }
    if (it->second != VK_NULL_HANDLE && mApi->vkDestroyFramebuffer != nullptr) {
      mApi->vkDestroyFramebuffer(mDevice, it->second, nullptr);
    }
    it = mFallbackFramebuffers.erase(it);
  }
}

void VulkanDevice::releaseFramebuffersUsing(const VkImageView view) {
  if (view == VK_NULL_HANDLE) return;
  std::lock_guard<std::mutex> lock(mFramebufferMutex);
  for (auto it = mFallbackFramebuffers.begin(); it != mFallbackFramebuffers.end();) {
    bool usesView = it->first.depth == view;
    for (uint32_t index = 0; index < it->first.colorCount && !usesView; ++index) {
      usesView = it->first.colors[index] == view;
    }
    if (!usesView) {
      ++it;
      continue;
    }
    if (it->second != VK_NULL_HANDLE && mApi->vkDestroyFramebuffer != nullptr) {
      mApi->vkDestroyFramebuffer(mDevice, it->second, nullptr);
    }
    it = mFallbackFramebuffers.erase(it);
  }
}

void VulkanDevice::releaseFramebuffers() {
  std::lock_guard<std::mutex> lock(mFramebufferMutex);
  for (auto& entry: mFallbackFramebuffers) {
    if (entry.second != VK_NULL_HANDLE && mApi->vkDestroyFramebuffer != nullptr) {
      mApi->vkDestroyFramebuffer(mDevice, entry.second, nullptr);
    }
  }
  mFallbackFramebuffers.clear();
}

Result VulkanDevice::createFramebuffer(const FramebufferDescriptor& descriptor, FramebufferHandle* outFramebuffer) {
  if (outFramebuffer == nullptr) return Result::InvalidArgument;
  *outFramebuffer = FramebufferHandle::invalid();

  if (mRenderPasses.get(descriptor.renderPass) == nullptr) return Result::InvalidHandle;
  if (descriptor.colorAttachmentCount > kMaxColorAttachments) return Result::InvalidArgument;
  for (uint32_t index = 0; index < descriptor.colorAttachmentCount; ++index) {
    VulkanTextureView resolved;
    if (!resolveAttachmentView(descriptor.colorAttachments[index], &resolved)) return Result::InvalidHandle;
  }
  if (descriptor.depthStencilAttachment.valid()) {
    VulkanTextureView resolved;
    if (!resolveAttachmentView(descriptor.depthStencilAttachment, &resolved)) return Result::InvalidHandle;
  }

  VulkanFramebuffer framebuffer;
  framebuffer.renderPass = descriptor.renderPass;
  framebuffer.colorAttachmentCount = descriptor.colorAttachmentCount;
  for (uint32_t index = 0; index < descriptor.colorAttachmentCount; ++index) {
    framebuffer.colorAttachments[index] = descriptor.colorAttachments[index];
  }
  framebuffer.depthStencilAttachment = descriptor.depthStencilAttachment;
  framebuffer.width = descriptor.width;
  framebuffer.height = descriptor.height;
  if (descriptor.label != nullptr) framebuffer.label = descriptor.label;

  *outFramebuffer = mFramebuffers.allocate(std::move(framebuffer));
  return outFramebuffer->valid() ? Result::Success : Result::OutOfMemory;
}

void VulkanDevice::destroyFramebuffer(const FramebufferHandle framebuffer) { mFramebuffers.free(framebuffer); }

// ==================================================================================================
// Render pipelines
// ==================================================================================================

Result VulkanDevice::buildRenderPipeline(const RenderPipelineDescriptor& descriptor,
        VulkanRenderPipeline& outPipeline) {
  if (!mInitialized) return Result::Error;

  RenderPassDescriptor resolvedPass;
  const RenderPassDescriptor* pass = nullptr;
  const VulkanRenderPass* compatiblePass = nullptr;
  if (const VulkanRenderPass* entry = mRenderPasses.get(descriptor.compatibleRenderPass); entry != nullptr) {
    compatiblePass = entry;
    resolvedPass.colorAttachmentCount = entry->colorAttachmentCount;
    for (uint32_t index = 0; index < entry->colorAttachmentCount; ++index) {
      resolvedPass.colorFormats[index] = entry->colorFormats[index];
    }
    resolvedPass.depthStencilFormat = entry->depthStencilFormat;
    resolvedPass.sampleCount = entry->sampleCount;
    pass = &resolvedPass;
  }

  const ValidationResult validation = validateRenderPipeline(descriptor, mCapabilities, pass);
  if (!validation.valid) {
    std::string message = validation.message;
    message += " [colour target 0 format = ";
    message += std::to_string(descriptor.colorTargetCount > 0
                                      ? static_cast<int>(descriptor.colorTargets[0].format)
                                      : -1);
    message += ", targets = ";
    message += std::to_string(descriptor.colorTargetCount);
    message += ", depthStencilFormat = ";
    message += std::to_string(static_cast<int>(descriptor.depthStencilFormat));
    message += pass != nullptr ? ", compatible pass resolved]" : ", compatible pass NOT FOUND]";
    emitMessage(MessageSeverity::Error, message);
    return Result::Unsupported;
  }

  const VulkanShaderModule* vertexModule = mShaderModules.get(descriptor.vertexModule);
  const VulkanShaderModule* fragmentModule = mShaderModules.get(descriptor.fragmentModule);
  if (descriptor.vertexModule.valid() && vertexModule == nullptr) return Result::InvalidHandle;
  if (descriptor.fragmentModule.valid() && fragmentModule == nullptr) return Result::InvalidHandle;
  if (vertexModule == nullptr) {
    emitMessage(MessageSeverity::Error, "a render pipeline needs a vertex shader module");
    return Result::InvalidArgument;
  }

  VulkanPipelineLayout* layout = mPipelineLayouts.get(descriptor.layout);
  if (descriptor.layout.valid() && layout == nullptr) return Result::InvalidHandle;
  const VkPipelineLayout vkLayout = layout != nullptr ? layout->layout : mEmptyPipelineLayout;

  // ---------------------------------------------------------------- stages
  std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
  uint32_t stageCount = 0;
  stages[stageCount].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[stageCount].stage = toVkStage(vertexModule->stage);
  stages[stageCount].module = vertexModule->module;
  stages[stageCount].pName = vertexModule->entryPoint.c_str();
  ++stageCount;
  if (fragmentModule != nullptr) {
    stages[stageCount].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[stageCount].stage = toVkStage(fragmentModule->stage);
    stages[stageCount].module = fragmentModule->module;
    stages[stageCount].pName = fragmentModule->entryPoint.c_str();
    ++stageCount;
  }

  // ---------------------------------------------------------------- vertex input
  std::array<VkVertexInputBindingDescription, kMaxVertexBufferLayouts> vertexBindings{};
  std::array<VkVertexInputAttributeDescription, kMaxVertexAttributes> vertexAttributes{};
  const uint32_t vertexBindingCount = std::min(descriptor.vertexInput.bufferCount, kMaxVertexBufferLayouts);
  for (uint32_t index = 0; index < vertexBindingCount; ++index) {
    const VertexBufferLayout& buffer = descriptor.vertexInput.buffers[index];
    vertexBindings[index].binding = index;
    vertexBindings[index].stride = static_cast<uint32_t>(buffer.stride);
    vertexBindings[index].inputRate =
            buffer.stepMode == VertexStepMode::Instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
  }
  const uint32_t vertexAttributeCount = std::min(descriptor.vertexInput.attributeCount, kMaxVertexAttributes);
  for (uint32_t index = 0; index < vertexAttributeCount; ++index) {
    const VertexAttribute& attribute = descriptor.vertexInput.attributes[index];
    const VkFormat format = toVkVertexFormat(attribute.format);
    if (format == VK_FORMAT_UNDEFINED) {
      emitMessage(MessageSeverity::Error, "a vertex attribute uses a format Vulkan has no equivalent for");
      return Result::Unsupported;
    }
    vertexAttributes[index].location = attribute.shaderLocation;
    vertexAttributes[index].binding = attribute.bufferIndex;
    vertexAttributes[index].format = format;
    vertexAttributes[index].offset = static_cast<uint32_t>(attribute.offset);
  }

  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInput.vertexBindingDescriptionCount = vertexBindingCount;
  vertexInput.pVertexBindingDescriptions = vertexBindingCount == 0 ? nullptr : vertexBindings.data();
  vertexInput.vertexAttributeDescriptionCount = vertexAttributeCount;
  vertexInput.pVertexAttributeDescriptions = vertexAttributeCount == 0 ? nullptr : vertexAttributes.data();

  // ---------------------------------------------------------------- assembly
  const bool fanEmulation = descriptor.rasterizer.topology == PrimitiveTopology::TriangleFan;
  VkPipelineInputAssemblyStateCreateInfo assembly{};
  assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  assembly.topology = toVkTopology(descriptor.rasterizer.topology);
  assembly.primitiveRestartEnable = VK_FALSE;

  // ---------------------------------------------------------------- rasterisation
  VkPipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.depthClampEnable = descriptor.rasterizer.depthClampEnable &&
                                        mCapabilities.supports(Feature::DepthClamp)
                                  ? VK_TRUE
                                  : VK_FALSE;
  rasterizer.rasterizerDiscardEnable = VK_FALSE;
  rasterizer.polygonMode = descriptor.rasterizer.wireframe && mCapabilities.supports(Feature::FillModeNonSolid)
                             ? VK_POLYGON_MODE_LINE
                             : VK_POLYGON_MODE_FILL;
  rasterizer.cullMode = toVkCullMode(descriptor.rasterizer.cullMode);
  rasterizer.frontFace = toVkFrontFace(descriptor.rasterizer.frontFace);
  rasterizer.depthBiasEnable =
          descriptor.rasterizer.depthBiasConstant != 0.0f || descriptor.rasterizer.depthBiasSlope != 0.0f ? VK_TRUE
                                                                                                         : VK_FALSE;
  rasterizer.depthBiasConstantFactor = descriptor.rasterizer.depthBiasConstant;
  rasterizer.depthBiasSlopeFactor = descriptor.rasterizer.depthBiasSlope;
  rasterizer.depthBiasClamp = 0.0f;
  rasterizer.lineWidth = 1.0f;

  // Viewport and scissor are dynamic; the counts still have to be stated.
  VkPipelineViewportStateCreateInfo viewportState{};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.scissorCount = 1;

  const uint32_t sampleCount = std::max(descriptor.multisample.sampleCount, pass != nullptr ? pass->sampleCount : 1u);
  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = static_cast<VkSampleCountFlagBits>(sampleCount);
  multisample.sampleShadingEnable = VK_FALSE;
  multisample.minSampleShading = 1.0f;
  multisample.pSampleMask = nullptr;
  multisample.alphaToCoverageEnable =
          descriptor.multisample.alphaToCoverageEnable && mCapabilities.supports(Feature::AlphaToCoverage)
                  ? VK_TRUE
                  : VK_FALSE;
  multisample.alphaToOneEnable = VK_FALSE;

  // ---------------------------------------------------------------- depth / stencil
  const PixelFormat depthFormat = descriptor.depthStencilFormat != PixelFormat::Undefined
                                    ? descriptor.depthStencilFormat
                                    : pass != nullptr ? pass->depthStencilFormat : PixelFormat::Undefined;
  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  const bool hasDepth = depthFormat != PixelFormat::Undefined;
  if (hasDepth) {
    depthStencil.depthTestEnable =
            descriptor.depthStencil.depthTestEnable ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = descriptor.depthStencil.depthWriteEnable ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = toVkCompareOp(descriptor.depthStencil.depthCompare);
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = descriptor.depthStencil.stencilEnable ? VK_TRUE : VK_FALSE;
    depthStencil.front.failOp = toVkStencilOp(descriptor.depthStencil.front.failOp);
    depthStencil.front.depthFailOp = toVkStencilOp(descriptor.depthStencil.front.depthFailOp);
    depthStencil.front.passOp = toVkStencilOp(descriptor.depthStencil.front.passOp);
    depthStencil.front.compareOp = toVkCompareOp(descriptor.depthStencil.front.compare);
    depthStencil.front.compareMask = descriptor.depthStencil.front.readMask;
    depthStencil.front.writeMask = descriptor.depthStencil.front.writeMask;
    depthStencil.front.reference = descriptor.depthStencil.stencilReference;
    depthStencil.back = depthStencil.front;
    depthStencil.back.failOp = toVkStencilOp(descriptor.depthStencil.back.failOp);
    depthStencil.back.depthFailOp = toVkStencilOp(descriptor.depthStencil.back.depthFailOp);
    depthStencil.back.passOp = toVkStencilOp(descriptor.depthStencil.back.passOp);
    depthStencil.back.compareOp = toVkCompareOp(descriptor.depthStencil.back.compare);
    depthStencil.back.compareMask = descriptor.depthStencil.back.readMask;
    depthStencil.back.writeMask = descriptor.depthStencil.back.writeMask;
    depthStencil.back.reference = descriptor.depthStencil.stencilReference;
    depthStencil.minDepthBounds = 0.0f;
    depthStencil.maxDepthBounds = 1.0f;
  }

  // ---------------------------------------------------------------- colour blend
  std::array<VkPipelineColorBlendAttachmentState, kMaxColorAttachments> blendAttachments{};
  std::array<VkFormat, kMaxColorAttachments> colorFormats{};
  uint32_t colorCount = std::min(descriptor.colorTargetCount, kMaxColorAttachments);
  if (colorCount == 0 && pass != nullptr) {
    colorCount = pass->colorAttachmentCount;
    for (uint32_t index = 0; index < colorCount; ++index) {
      colorFormats[index] = toVkFormat(pass->colorFormats[index]);
    }
  } else {
    for (uint32_t index = 0; index < colorCount; ++index) {
      colorFormats[index] = toVkFormat(descriptor.colorTargets[index].format);
    }
  }
  for (uint32_t index = 0; index < colorCount; ++index) {
    const ColorTargetState& target = descriptor.colorTargets[index];
    VkPipelineColorBlendAttachmentState& attachment = blendAttachments[index];
    attachment.blendEnable = target.blend.blendEnable ? VK_TRUE : VK_FALSE;
    attachment.srcColorBlendFactor = toVkBlendFactor(target.blend.color.srcFactor);
    attachment.dstColorBlendFactor = toVkBlendFactor(target.blend.color.dstFactor);
    attachment.colorBlendOp = toVkBlendOp(target.blend.color.operation);
    attachment.srcAlphaBlendFactor = toVkBlendFactor(target.blend.alpha.srcFactor);
    attachment.dstAlphaBlendFactor = toVkBlendFactor(target.blend.alpha.dstFactor);
    attachment.alphaBlendOp = toVkBlendOp(target.blend.alpha.operation);
    attachment.colorWriteMask = toVkColorWriteMask(target.writeMask);
    if (attachment.colorWriteMask == 0) attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
  }

  VkPipelineColorBlendStateCreateInfo blend{};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend.logicOpEnable = VK_FALSE;
  blend.attachmentCount = colorCount;
  blend.pAttachments = colorCount == 0 ? nullptr : blendAttachments.data();
  blend.blendConstants[0] = static_cast<float>(descriptor.rasterizer.blendConstant.r);
  blend.blendConstants[1] = static_cast<float>(descriptor.rasterizer.blendConstant.g);
  blend.blendConstants[2] = static_cast<float>(descriptor.rasterizer.blendConstant.b);
  blend.blendConstants[3] = static_cast<float>(descriptor.rasterizer.blendConstant.a);

  const std::array<VkDynamicState, 4> dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
    VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_BLEND_CONSTANTS };
  VkPipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
  dynamicState.pDynamicStates = dynamicStates.data();

  VkPipelineRenderingCreateInfo rendering{};
  rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  rendering.viewMask = 0;
  rendering.colorAttachmentCount = colorCount;
  rendering.pColorAttachmentFormats = colorCount == 0 ? nullptr : colorFormats.data();
  rendering.depthAttachmentFormat = hasDepth ? toVkFormat(depthFormat) : VK_FORMAT_UNDEFINED;
  rendering.stencilAttachmentFormat = hasDepth && isStencilFormat(depthFormat) ? toVkFormat(depthFormat)
                                                                              : VK_FORMAT_UNDEFINED;

  VkGraphicsPipelineCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  info.stageCount = stageCount;
  info.pStages = stages.data();
  info.pVertexInputState = &vertexInput;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewportState;
  info.pRasterizationState = &rasterizer;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = hasDepth ? &depthStencil : nullptr;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamicState;
  info.layout = vkLayout;
  info.subpass = 0;
  if (usesDynamicRendering()) {
    info.pNext = &rendering;
    info.renderPass = VK_NULL_HANDLE;
  } else {
    // A pipeline created for a render pass is only usable with a compatible pass; the caller must have
    // supplied one (the canvas always does). Refusing here is better than creating a pipeline that
    // cannot be bound to the pass the command buffer will begin.
    if (compatiblePass == nullptr || compatiblePass->renderPass == VK_NULL_HANDLE) {
      emitMessage(MessageSeverity::Error,
              "a graphics pipeline on a Vulkan 1.0/1.1 device needs a compatible render pass");
      return Result::Unsupported;
    }
    info.pNext = nullptr;
    info.renderPass = compatiblePass->renderPass;
  }

  VulkanRenderPipeline pipeline;
  pipeline.layout = descriptor.layout;
  pipeline.descriptor = descriptor;
  pipeline.fanEmulation = fanEmulation;
  if (descriptor.label != nullptr) pipeline.label = descriptor.label;

  VkResult created = VK_SUCCESS;
  {
    const std::lock_guard<std::mutex> cacheLock(mPipelineCacheMutex);
    created = mApi->vkCreateGraphicsPipelines(mDevice, mPipelineCache, 1, &info, nullptr, &pipeline.pipeline);
  }
  if (created != VK_SUCCESS) {
    std::string message = "the graphics pipeline was rejected by the driver (VkResult ";
    message += std::to_string(static_cast<int>(created));
    message += ", topology ";
    message += topologyName(descriptor.rasterizer.topology);
    message += ", colours ";
    message += std::to_string(colorCount);
    message += ')';
    emitMessage(MessageSeverity::Error, message);
    return toResult(created);
  }

  outPipeline = std::move(pipeline);
  return Result::Success;
}

Result VulkanDevice::createRenderPipeline(const RenderPipelineDescriptor& descriptor,
        RenderPipelineHandle* outPipeline) {
  if (outPipeline == nullptr) return Result::InvalidArgument;
  *outPipeline = RenderPipelineHandle::invalid();
  VulkanRenderPipeline pipeline;
  const Result built = buildRenderPipeline(descriptor, pipeline);
  if (!isSuccess(built)) return built;
  *outPipeline = mRenderPipelines.allocate(std::move(pipeline));
  return outPipeline->valid() ? Result::Success : Result::OutOfMemory;
}

void VulkanDevice::destroyRenderPipeline(const RenderPipelineHandle pipelineHandle) {
  if (VulkanRenderPipeline* pipeline = mRenderPipelines.get(pipelineHandle); pipeline != nullptr) {
    if (pipeline->pipeline != VK_NULL_HANDLE) mApi->vkDestroyPipeline(mDevice, pipeline->pipeline, nullptr);
    pipeline->pipeline = VK_NULL_HANDLE;
    mRenderPipelines.free(pipelineHandle);
  }
}

bool VulkanDevice::createRenderPipelineAsync(const RenderPipelineDescriptor& descriptor,
        PipelineCompletion completion) {
  if (!mInitialized || !completion) return false;
  AsyncPipelineRequest request;
  request.descriptor = descriptor;
  request.label = descriptor.label != nullptr ? descriptor.label : "";
  request.completion = std::move(completion);
  {
    const std::lock_guard<std::mutex> lock(mPipelineQueueMutex);
    if (!mPipelineWorker.joinable()) {
      mPipelineWorkerStop = false;
      try {
        mPipelineWorker = std::thread([this] { pipelineWorkerMain(); });
      } catch (...) {
        return false;
      }
    }
    mPipelineRequests.push_back(std::move(request));
  }
  mPipelineQueueCondition.notify_one();
  return true;
}

bool VulkanDevice::createShaderModuleAsync(const ShaderModuleDescriptor& descriptor,
        ShaderModuleCompletion completion) {
  if (!mInitialized || !completion || descriptor.code == nullptr || descriptor.codeSize == 0) return false;
  AsyncShaderRequest request;
  request.descriptor = descriptor;
  request.code.assign(static_cast<const uint8_t*>(descriptor.code),
          static_cast<const uint8_t*>(descriptor.code) + descriptor.codeSize);
  request.label = descriptor.label != nullptr ? descriptor.label : "";
  request.entryPoint = descriptor.entryPoint != nullptr ? descriptor.entryPoint : "";
  request.completion = std::move(completion);
  {
    const std::lock_guard<std::mutex> lock(mPipelineQueueMutex);
    if (!mPipelineWorker.joinable()) {
      mPipelineWorkerStop = false;
      try {
        mPipelineWorker = std::thread([this] { pipelineWorkerMain(); });
      } catch (...) {
        return false;
      }
    }
    mShaderModuleRequests.push_back(std::move(request));
  }
  mPipelineQueueCondition.notify_one();
  return true;
}

void VulkanDevice::pollAsyncOperations() {
  std::deque<AsyncShaderResult> shaderModules;
  std::deque<AsyncPipelineResult> pipelines;
  {
    const std::lock_guard<std::mutex> lock(mPipelineQueueMutex);
    shaderModules.swap(mShaderModuleResults);
    pipelines.swap(mPipelineResults);
  }
  for (AsyncShaderResult& result : shaderModules) {
    if (!result.completion) continue;
    if (!isSuccess(result.result)) {
      result.completion(ShaderModuleHandle::invalid(), result.result, std::move(result.error));
      continue;
    }
    const ShaderModuleHandle handle = mShaderModules.allocate(std::move(result.module));
    result.completion(handle, handle.valid() ? Result::Success : Result::OutOfMemory,
            handle.valid() ? std::string{} : "the shader-module handle pool is exhausted");
  }
  for (AsyncPipelineResult& result : pipelines) {
    if (!result.completion) continue;
    if (!isSuccess(result.result)) {
      result.completion(RenderPipelineHandle::invalid(), result.result, std::move(result.error));
      continue;
    }
    const RenderPipelineHandle handle = mRenderPipelines.allocate(std::move(result.pipeline));
    result.completion(handle, handle.valid() ? Result::Success : Result::OutOfMemory,
            handle.valid() ? std::string{} : "the render-pipeline handle pool is exhausted");
  }
}

void VulkanDevice::pipelineWorkerMain() {
  for (;;) {
    AsyncShaderRequest shaderRequest;
    AsyncPipelineRequest pipelineRequest;
    bool shader = false;
    {
      std::unique_lock<std::mutex> lock(mPipelineQueueMutex);
      mPipelineQueueCondition.wait(lock, [this] {
        return mPipelineWorkerStop || !mShaderModuleRequests.empty() || !mPipelineRequests.empty();
      });
      if (!mShaderModuleRequests.empty()) {
        shaderRequest = std::move(mShaderModuleRequests.front());
        mShaderModuleRequests.pop_front();
        shader = true;
      } else if (!mPipelineRequests.empty()) {
        pipelineRequest = std::move(mPipelineRequests.front());
        mPipelineRequests.pop_front();
      } else if (mPipelineWorkerStop) {
        return;
      } else {
        continue;
      }
    }

    if (shader) {
      AsyncShaderResult result;
      try {
        shaderRequest.descriptor.code = shaderRequest.code.empty() ? nullptr : shaderRequest.code.data();
        shaderRequest.descriptor.codeSize = shaderRequest.code.size();
        shaderRequest.descriptor.label = shaderRequest.label.empty() ? nullptr : shaderRequest.label.c_str();
        shaderRequest.descriptor.entryPoint =
                shaderRequest.entryPoint.empty() ? nullptr : shaderRequest.entryPoint.c_str();
        result.result = buildShaderModule(shaderRequest.descriptor, result.module);
        if (!isSuccess(result.result)) {
          result.error = "the shader module could not be created (result ";
          result.error += std::to_string(static_cast<int>(result.result));
          result.error += ')';
        }
      } catch (const std::exception& exception) {
        result.result = Result::Error;
        result.error = exception.what();
      } catch (...) {
        result.result = Result::Error;
        result.error = "the shader module creation threw";
      }
      result.completion = std::move(shaderRequest.completion);
      {
        const std::lock_guard<std::mutex> lock(mPipelineQueueMutex);
        mShaderModuleResults.push_back(std::move(result));
      }
      continue;
    }

    AsyncPipelineResult result;
    try {
      pipelineRequest.descriptor.label = pipelineRequest.label.empty() ? nullptr : pipelineRequest.label.c_str();
      result.result = buildRenderPipeline(pipelineRequest.descriptor, result.pipeline);
      if (!isSuccess(result.result)) {
        result.error = "the graphics pipeline could not be created (result ";
        result.error += std::to_string(static_cast<int>(result.result));
        result.error += ')';
      }
    } catch (const std::exception& exception) {
      result.result = Result::Error;
      result.error = exception.what();
    } catch (...) {
      result.result = Result::Error;
      result.error = "the graphics pipeline creation threw";
    }
    result.completion = std::move(pipelineRequest.completion);
    {
      const std::lock_guard<std::mutex> lock(mPipelineQueueMutex);
      mPipelineResults.push_back(std::move(result));
    }
  }
}
void VulkanDevice::stopPipelineWorker() {
  {
    const std::lock_guard<std::mutex> lock(mPipelineQueueMutex);
    if (!mPipelineWorker.joinable()) return;
    mPipelineWorkerStop = true;
  }
  mPipelineQueueCondition.notify_all();
  mPipelineWorker.join();
}

Result VulkanDevice::createComputePipeline(const ComputePipelineDescriptor& descriptor,
        ComputePipelineHandle* outPipeline) {
  if (outPipeline == nullptr) return Result::InvalidArgument;
  *outPipeline = ComputePipelineHandle::invalid();

  const ValidationResult validation = validateComputePipeline(descriptor, mCapabilities);
  if (!validation.valid) {
    emitMessage(MessageSeverity::Error, validation.message);
    return Result::Unsupported;
  }

  const VulkanShaderModule* module = mShaderModules.get(descriptor.computeModule);
  if (module == nullptr) return Result::InvalidHandle;
  VulkanPipelineLayout* layout = mPipelineLayouts.get(descriptor.layout);
  if (descriptor.layout.valid() && layout == nullptr) return Result::InvalidHandle;
  const VkPipelineLayout vkLayout = layout != nullptr ? layout->layout : mEmptyPipelineLayout;

  VkComputePipelineCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  info.stage.module = module->module;
  info.stage.pName = module->entryPoint.c_str();
  info.layout = vkLayout;

  VulkanComputePipeline pipeline;
  pipeline.layout = descriptor.layout;
  if (descriptor.label != nullptr) pipeline.label = descriptor.label;

  if (const VkResult created = mApi->vkCreateComputePipelines(mDevice, mPipelineCache, 1, &info, nullptr,
              &pipeline.pipeline);
      created != VK_SUCCESS) {
    emitMessage(MessageSeverity::Error, "the compute pipeline was rejected by the driver");
    return toResult(created);
  }

  *outPipeline = mComputePipelines.allocate(std::move(pipeline));
  return outPipeline->valid() ? Result::Success : Result::OutOfMemory;
}

void VulkanDevice::destroyComputePipeline(const ComputePipelineHandle pipelineHandle) {
  if (VulkanComputePipeline* pipeline = mComputePipelines.get(pipelineHandle); pipeline != nullptr) {
    if (pipeline->pipeline != VK_NULL_HANDLE) mApi->vkDestroyPipeline(mDevice, pipeline->pipeline, nullptr);
    pipeline->pipeline = VK_NULL_HANDLE;
    mComputePipelines.free(pipelineHandle);
  }
}

// ==================================================================================================
// Resource lifetime, native interop and labels
// ==================================================================================================

bool VulkanDevice::isResourceReadyToDestroy(const TextureHandle handle) const {
  // Synchronous submissions are complete by the time they return. An asynchronous `Queue::submit`
  // with a fence keeps a counter until the command buffer is reset/begun or `waitIdle` is called.
  return !hasAsyncSubmissions() && mTextures.isAlive(handle);
}

bool VulkanDevice::isResourceReadyToDestroy(const BufferHandle handle) const {
  return !hasAsyncSubmissions() && mBuffers.isAlive(handle);
}

NativeTexture VulkanDevice::getNativeTexture(const TextureHandle handle) const {
  const VulkanTexture* texture = mTextures.get(handle);
  if (texture == nullptr) return NativeTexture{};
  NativeTexture native;
  native.object = handlePointer(texture->image);
  native.context = handlePointer(mDevice);
  return native;
}

NativeBuffer VulkanDevice::getNativeBuffer(const BufferHandle handle) const {
  const VulkanBuffer* buffer = mBuffers.get(handle);
  if (buffer == nullptr) return NativeBuffer{};
  NativeBuffer native;
  native.object = handlePointer(buffer->buffer);
  native.context = handlePointer(mDevice);
  return native;
}

bool VulkanDevice::supportsNativeTextureImport(const NativeTextureKind kind) const {
  switch (kind) {
    case NativeTextureKind::OpenGlTexture:
      // A GL name is not a Vulkan object. Importing one would mean "this number happens to be a VkImage",
      // which is never true across two APIs, so it is refused even when a caller passes it by mistake.
      return false;
    case NativeTextureKind::Direct3D11Texture:
#if defined(_WIN32)
      // The import path needs the extension *and* the entry point that tells us which memory types the
      // handle can be bound to; a loader that exposes the extension without the function would otherwise
      // crash on the first import.
      return mHasExternalMemoryWin32 && mApi != nullptr && mApi->vkGetMemoryWin32HandlePropertiesKHR != nullptr &&
             mApi->vkGetPhysicalDeviceImageFormatProperties2 != nullptr;
#else
      // A Direct3D 11 handle is a Windows resource and the entry point that reads it only exists in a
      // Windows build of the loader, so no other platform can satisfy this kind.
      return false;
#endif
    case NativeTextureKind::VulkanImage:
      return true;
    case NativeTextureKind::AndroidHardwareBuffer:
#if defined(__ANDROID__)
      // The import needs the extension, the entry point that describes the buffer, and the runtime that can
      // take a reference on it: without the last one an image could outlive the buffer a producer recycles.
      return mHasExternalMemoryAndroidHardwareBuffer && mApi != nullptr &&
             mApi->vkGetAndroidHardwareBufferPropertiesANDROID != nullptr &&
             mApi->vkGetPhysicalDeviceImageFormatProperties2 != nullptr && androidHardwareBufferApi().available;
#else
      // An `AHardwareBuffer` is an Android object, and the entry points that read it only exist in an
      // Android build of the loader.
      return false;
#endif
  }
  return false;
}

bool VulkanDevice::supportsYuvHardwareBufferImport() const {
#if defined(__ANDROID__)
  // The YUV path additionally needs the sampler conversion entry point; the AHB import alone is not
  // enough, and a device that cannot create the conversion must be reported before the producer chooses
  // an ImageReader-backed zero-copy path.
  return mHasExternalMemoryAndroidHardwareBuffer && mApi != nullptr &&
         mApi->vkGetAndroidHardwareBufferPropertiesANDROID != nullptr &&
         mApi->vkGetPhysicalDeviceImageFormatProperties2 != nullptr &&
         mApi->vkCreateSamplerYcbcrConversion != nullptr && androidHardwareBufferApi().available;
#else
  return false;
#endif
}

Result VulkanDevice::importNativeTexture(const NativeTextureDescriptor& descriptor, TextureHandle* outTexture) {
  if (outTexture == nullptr) return Result::InvalidArgument;
  *outTexture = TextureHandle::invalid();
  if (!mInitialized) return Result::Error;

  // The descriptor names a platform object; every kind but the platform's own is refused here, before any
  // of it is interpreted, so that a foreign pointer never reaches the driver as the handle it expects.
  const bool known = descriptor.kind == NativeTextureKind::Direct3D11Texture ||
                     descriptor.kind == NativeTextureKind::AndroidHardwareBuffer ||
                     descriptor.kind == NativeTextureKind::VulkanImage;
  if (!known) return Result::Unsupported;

  if (descriptor.kind == NativeTextureKind::VulkanImage) {
    if (descriptor.platformHandle == nullptr) return Result::InvalidArgument;
    VulkanTexture texture;
    texture.image = reinterpret_cast<VkImage>(descriptor.platformHandle);
    texture.vkFormat = toVkFormat(descriptor.format);
    if (texture.vkFormat == VK_FORMAT_UNDEFINED) return Result::Unsupported;
    texture.format = descriptor.format;
    texture.extent = { descriptor.width, descriptor.height, 1 };
    texture.usage = static_cast<TextureUsages>(TextureUsage::Sampled);
    texture.externalImage = true;
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = texture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = texture.vkFormat;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (mApi == nullptr || mApi->vkCreateImageView(mDevice, &viewInfo, nullptr, &texture.view) != VK_SUCCESS) {
      return Result::Error;
    }
    *outTexture = mTextures.allocate(std::move(texture));
    return outTexture->valid() ? Result::Success : Result::OutOfMemory;
  }
  VulkanTexture texture;
  if (const Result created = createExternalImage(descriptor, &texture); !isSuccess(created)) {
    return created;
  }
  *outTexture = mTextures.allocate(std::move(texture));
  return outTexture->valid() ? Result::Success : Result::OutOfMemory;
}

Result VulkanDevice::importNativeYuvTexture(const NativeTextureDescriptor& descriptor, TextureHandle* outTexture,
        YcbcrConversionDescriptor* outConversion) {
  if (outTexture == nullptr || outConversion == nullptr) return Result::InvalidArgument;
  *outTexture = TextureHandle::invalid();
  if (!mInitialized) return Result::Error;
  if (descriptor.kind != NativeTextureKind::AndroidHardwareBuffer) {
    return Result::Unsupported;
  }
  if (!descriptor.ycbcr.has_value()) {
    return Result::InvalidArgument;
  }
#if defined(__ANDROID__)
  VulkanTexture texture;
  YcbcrConversionDescriptor resolved = *descriptor.ycbcr;
  if (const Result created = createExternalHardwareBufferImage(descriptor, &texture, &resolved);
          !isSuccess(created)) {
    return created;
  }
  *outTexture = mTextures.allocate(std::move(texture));
  if (!outTexture->valid()) return Result::OutOfMemory;
  *outConversion = resolved;
  return Result::Success;
#else
  return Result::Unsupported;
#endif
}

void VulkanDevice::setTextureLabel(const TextureHandle handle, const char* label) {
  VulkanTexture* texture = mTextures.get(handle);
  if (texture == nullptr) return;
  texture->label = label != nullptr ? label : "";
  if (mHasDebugUtils && mApi->vkSetDebugUtilsObjectNameEXT != nullptr && texture->image != VK_NULL_HANDLE) {
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = VK_OBJECT_TYPE_IMAGE;
    info.objectHandle = handleValue(texture->image);
    info.pObjectName = texture->label.c_str();
    mApi->vkSetDebugUtilsObjectNameEXT(mDevice, &info);
  }
}

void VulkanDevice::setBufferLabel(const BufferHandle handle, const char* label) {
  VulkanBuffer* buffer = mBuffers.get(handle);
  if (buffer == nullptr) return;
  buffer->label = label != nullptr ? label : "";
  if (mHasDebugUtils && mApi->vkSetDebugUtilsObjectNameEXT != nullptr && buffer->buffer != VK_NULL_HANDLE) {
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = VK_OBJECT_TYPE_BUFFER;
    info.objectHandle = handleValue(buffer->buffer);
    info.pObjectName = buffer->label.c_str();
    mApi->vkSetDebugUtilsObjectNameEXT(mDevice, &info);
  }
}

void VulkanDevice::beginDebugRegion(const char* label) {
  // Vulkan has no host-side debug region: labels belong to a command buffer. Routing the name to the
  // message sink keeps the diagnostic visible without pretending there is an annotation to open.
  if (label != nullptr) emitMessage(MessageSeverity::Verbose, label);
}

void VulkanDevice::endDebugRegion() {}

} // namespace backend::vulkan






