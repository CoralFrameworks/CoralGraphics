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

#include "GlslCompiler.h"
#include "MetalCross.h"
#include "ShaderCross.h"

#include <backend/Validation.h>

#include <IOSurface/IOSurfaceRef.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace backend::metal {
namespace {

std::string toStdString(NSString* string) {
  if (string == nil) return {};
  const char* utf8 = [string UTF8String];
  return utf8 != nullptr ? std::string(utf8) : std::string();
}

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

uint64_t hashBytes(const void* data, const size_t size) noexcept {
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint64_t hash = 1469598103934665603ull;
  for (size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= 1099511628211ull;
  }
  return hash;
}

/** Total storage a texture's whole mip chain occupies, which is what `memoryStatistics` charges for. */
uint64_t textureByteSize(const TextureDescriptor& descriptor) {
  uint64_t total = 0;
  uint32_t width = descriptor.size.width;
  uint32_t height = descriptor.size.height;
  const uint32_t layers = descriptor.type == TextureType::Texture3D ? 1 : std::max(1u, descriptor.size.depthOrArrayLayers);
  const uint32_t depth = descriptor.type == TextureType::Texture3D
                                 ? std::max(1u, descriptor.size.depthOrArrayLayers)
                                 : 1;
  for (uint32_t level = 0; level < std::max(1u, descriptor.mipLevelCount); ++level) {
    total += getFormatByteSize(descriptor.format, std::max(1u, width), std::max(1u, height), depth) * layers;
    width >>= 1;
    height >>= 1;
  }
  return total;
}

MTLResourceOptions storageOptionsFor(const MemoryUsage usage) {
  // A GPU-only buffer is private; every host-visible usage is shared. Metal has no separate "managed" requirement on
  // Apple silicon, and a shared buffer is coherent with the GPU on every Apple GPU, which is what the RHI's
  // MemoryUsage names promise.
  return usage == MemoryUsage::GpuOnly ? MTLResourceStorageModePrivate : MTLResourceStorageModeShared;
}

Result toCreateResult(const ValidationResult& validation) {
  if (validation.valid) return Result::Success;
  switch (validation.reason) {
    case FeatureUnavailableReason::Available:
      return Result::Success;
    case FeatureUnavailableReason::ApiVersionTooLow:
    case FeatureUnavailableReason::ExtensionMissing:
    case FeatureUnavailableReason::HardwareUnsupported:
    case FeatureUnavailableReason::ArchitecturallyImpossible:
    case FeatureUnavailableReason::NotImplementedByBackend:
    case FeatureUnavailableReason::DisabledByConfiguration:
      return Result::Unsupported;
  }
  return Result::InvalidArgument;
}

} // namespace anonymous

// ==================================================================================================
// Textures
// ==================================================================================================

Result MetalDevice::createTexture(const TextureDescriptor& descriptor, TextureHandle* outTexture) {
  if (outTexture == nullptr) return Result::InvalidArgument;
  *outTexture = TextureHandle::invalid();
  if (!mInitialized) return Result::Error;

  TextureDescriptor resolved = descriptor;
  const TextureValidationResult validation = validateTextureDescriptor(descriptor, mCapabilities);
  if (!validation.valid) {
    if (validation.resolved.format == PixelFormat::Undefined) {
      emitMessage(MessageSeverity::Error, std::string("texture rejected: ") + validation.message);
      return Result::Unsupported;
    }
    resolved = validation.resolved;
  }

  const MTLPixelFormat pixelFormat = toMtlPixelFormat(resolved.format);
  if (pixelFormat == MTLPixelFormatInvalid) {
    emitMessage(MessageSeverity::Error,
            std::string("the texture format ") + getPixelFormatInfo(resolved.format).name + " has no Metal equivalent");
    return Result::Unsupported;
  }

  const bool depth = isDepthFormat(resolved.format);
  const bool stencil = isStencilFormat(resolved.format);
  const uint32_t sampleCount = std::max(1u, resolved.sampleCount);

  MTLTextureDescriptor* native = [MTLTextureDescriptor new];
  native.textureType = toMtlTextureType(resolved.type, sampleCount);
  native.pixelFormat = pixelFormat;
  native.width = std::max(1u, resolved.size.width);
  native.height = std::max(1u, resolved.size.height);
  native.depth = resolved.type == TextureType::Texture3D ? std::max(1u, resolved.size.depthOrArrayLayers) : 1;
  native.mipmapLevelCount = sampleCount > 1 ? 1 : std::max(1u, resolved.mipLevelCount);
  native.sampleCount = sampleCount;
  native.usage = MTLTextureUsagePixelFormatView;
  if (hasUsage(resolved.usage, TextureUsage::Sampled)) native.usage |= MTLTextureUsageShaderRead;
  if (hasUsage(resolved.usage, TextureUsage::Storage)) native.usage |= MTLTextureUsageShaderWrite;
  if (hasUsage(resolved.usage, TextureUsage::ColorAttachment) ||
      hasUsage(resolved.usage, TextureUsage::DepthStencilAttachment)) {
    native.usage |= MTLTextureUsageRenderTarget;
  }
  // A texture the caller only ever uses as a render target does not need to be sampleable, but every usage the RHI
  // has for a texture implies at least one of the three flags above, so no `Unknown` default is needed.
  native.storageMode = MTLStorageModePrivate;

  if (resolved.type == TextureType::Texture2DArray || resolved.type == TextureType::TextureCubeArray) {
    native.arrayLength = std::max(1u, resolved.size.depthOrArrayLayers);
  } else if (resolved.type == TextureType::TextureCube) {
    native.arrayLength = 1;
  } else {
    native.arrayLength = 1;
  }

  id<MTLTexture> texture = [mDevice newTextureWithDescriptor:native];
  if (texture == nil) {
    emitMessage(MessageSeverity::Error,
            std::string("the Metal device could not allocate a ") + std::to_string(native.width) + "x" +
                    std::to_string(native.height) + " " + getPixelFormatInfo(resolved.format).name + " texture");
    return Result::OutOfMemory;
  }

  MetalTexture payload;
  payload.texture = texture;
  payload.type = native.textureType;
  payload.format = resolved.format;
  payload.size = resolved.size;
  payload.mipLevelCount = native.mipmapLevelCount;
  payload.sampleCount = sampleCount;
  payload.usage = resolved.usage;
  payload.bytes = textureByteSize(resolved);

  *outTexture = mTextures.allocate(std::move(payload));
  if (!outTexture->valid()) return Result::OutOfMemory;
  mResidentTextureBytes += textureByteSize(resolved);
  (void) depth;
  (void) stencil;
  return Result::Success;
}

void MetalDevice::destroyTexture(const TextureHandle textureHandle) {
  MetalTexture* texture = mTextures.get(textureHandle);
  if (texture == nullptr) return;

  // Views of this texture become invalid with it, exactly as on the other backends: a view is not a reference-counted
  // alias here, it is a second handle into the same pool whose native object the driver may recycle.
  mTextureViews.forEach([&](const TextureViewHandle viewHandle, MetalTextureView& view) {
    if (view.texture == texture->texture) {
      view.texture = nil;
      mTextureViews.free(viewHandle);
    }
  });

  mResidentTextureBytes -= std::min<uint64_t>(mResidentTextureBytes, texture->bytes);
  texture->texture = nil;
  mTextures.free(textureHandle);
}

Result MetalDevice::createTextureView(const TextureViewDescriptor& descriptor, TextureViewHandle* outView) {
  if (outView == nullptr) return Result::InvalidArgument;
  *outView = TextureViewHandle::invalid();

  const MetalTexture* base = mTextures.get(descriptor.texture);
  if (base == nullptr || base->texture == nil) return Result::InvalidHandle;

  const uint32_t totalMips = base->texture.mipmapLevelCount;
  const uint32_t baseMip = std::min(descriptor.range.baseMipLevel, totalMips - 1);
  const uint32_t mipCount = descriptor.range.mipLevelCount == 0
                                    ? totalMips - baseMip
                                    : std::min(descriptor.range.mipLevelCount, totalMips - baseMip);

  uint32_t totalLayers = 1;
  switch (base->type) {
    case MTLTextureType2DArray:
    case MTLTextureType2DMultisampleArray:
    case MTLTextureTypeCubeArray:
      totalLayers = static_cast<uint32_t>(base->texture.arrayLength);
      break;
    case MTLTextureTypeCube:
      totalLayers = 6;
      break;
    default:
      totalLayers = 1;
      break;
  }
  const uint32_t baseLayer = std::min(descriptor.range.baseArrayLayer, totalLayers - 1);
  const uint32_t layerCount = descriptor.range.arrayLayerCount == 0
                                      ? totalLayers - baseLayer
                                      : std::min(descriptor.range.arrayLayerCount, totalLayers - baseLayer);

  // A view that reinterprets nothing and covers the whole texture can be the texture itself. That is the common case
  // (`createTexture` is followed by a full-range view), and skipping the driver call avoids a second native object per
  // texture.
  const bool fullRange = baseMip == 0 && mipCount == totalMips && baseLayer == 0 && layerCount == totalLayers;
  const PixelFormat viewFormat = descriptor.format == PixelFormat::Undefined ? base->format : descriptor.format;
  if (viewFormat == base->format && fullRange && descriptor.dimension == TextureViewDimension::Texture2D &&
      (base->type == MTLTextureType2D)) {
    MetalTextureView view;
    view.texture = base->texture;
    view.type = base->type;
    view.format = base->format;
    view.width = static_cast<uint32_t>(base->texture.width);
    view.height = static_cast<uint32_t>(base->texture.height);
    view.sampleCount = base->sampleCount;
    *outView = mTextureViews.allocate(std::move(view));
    return outView->valid() ? Result::Success : Result::OutOfMemory;
  }

  const MTLPixelFormat pixelFormat =
          descriptor.format == PixelFormat::Undefined ? base->texture.pixelFormat : toMtlPixelFormat(descriptor.format);
  if (pixelFormat == MTLPixelFormatInvalid) return Result::Unsupported;

  MTLTextureType viewType = toMtlTextureViewType(descriptor.dimension, layerCount);
  // A view of a multisampled texture is a cluster of "the same type, multisampled"; the RHI's dimension values say
  // nothing about sample count, so the base texture is the authority.
  if (base->sampleCount > 1) {
    viewType = base->type;
  }
  // A cube view needs six slices starting at the layer base; a 2D array view needs the layer count as slices.
  const NSUInteger slices = viewType == MTLTextureTypeCube || viewType == MTLTextureTypeCubeArray ? layerCount
                          : viewType == MTLTextureType3D                                   ? 1
                                                                                           : layerCount;
  id<MTLTexture> viewTexture = [base->texture newTextureViewWithPixelFormat:pixelFormat
                                                               textureType:viewType
                                                                   levels:NSMakeRange(baseMip, mipCount)
                                                                   slices:NSMakeRange(baseLayer, std::max<NSUInteger>(1, slices))];
  if (viewTexture == nil) {
    emitMessage(MessageSeverity::Error, "the Metal device could not create a texture view");
    return Result::Unsupported;
  }

  MetalTextureView view;
  view.texture = viewTexture;
  view.type = viewType;
  view.format = viewFormat;
  view.width = static_cast<uint32_t>(viewTexture.width);
  view.height = static_cast<uint32_t>(viewTexture.height);
  view.sampleCount = base->sampleCount;

  *outView = mTextureViews.allocate(std::move(view));
  return outView->valid() ? Result::Success : Result::OutOfMemory;
}

void MetalDevice::destroyTextureView(const TextureViewHandle viewHandle) {
  if (MetalTextureView* view = mTextureViews.get(viewHandle); view != nullptr) {
    view->texture = nil;
    mTextureViews.free(viewHandle);
  }
}

// ==================================================================================================
// Samplers
// ==================================================================================================

Result MetalDevice::createSampler(const SamplerDescriptor& descriptor, TextureSamplerHandle* outSampler) {
  if (outSampler == nullptr) return Result::InvalidArgument;
  *outSampler = TextureSamplerHandle::invalid();

  SamplerDescriptor resolved = descriptor;
  const ValidationResult validation = validateSamplerDescriptor(descriptor, mCapabilities);
  if (!validation.valid) {
    // The sampler validator repairs anisotropy by clamping rather than refusing, and only rejects genuinely impossible
    // combinations; a rejection here is therefore a real argument error.
    emitMessage(MessageSeverity::Error, std::string("sampler rejected: ") + validation.message);
    return toCreateResult(validation);
  }

  const bool comparison = resolved.filterType == FilterType::Comparison || resolved.depthCompare;

  MTLSamplerDescriptor* native = [MTLSamplerDescriptor new];
  // `setLabel:` asserts on nil rather than clearing the label, so an absent label must not be assigned at all.
  if (resolved.label != nullptr) native.label = [NSString stringWithUTF8String:resolved.label];
  native.minFilter = toMtlMinMagFilter(resolved.minFilter);
  native.magFilter = toMtlMinMagFilter(resolved.magFilter);
  native.mipFilter = toMtlMipFilter(resolved.mipmapFilter);
  native.sAddressMode = toMtlAddressMode(resolved.addressModeU);
  native.tAddressMode = toMtlAddressMode(resolved.addressModeV);
  native.rAddressMode = toMtlAddressMode(resolved.addressModeW);
  native.borderColor = toMtlBorderColor(resolved.borderColor);
  native.lodMinClamp = resolved.lodMinClamp;
  native.lodMaxClamp = std::max(resolved.lodMinClamp, resolved.lodMaxClamp);
  native.maxAnisotropy = std::clamp(resolved.maxAnisotropy, 1.0f, mCapabilities.maxSamplerAnisotropy);
  native.compareFunction = comparison ? toMtlCompareFunction(resolved.compare) : MTLCompareFunctionNever;
  // Argument buffers are not part of this backend's binding model, so a sampler that would be encoded into one is not
  // needed; the explicit `[[sampler(n)]]` table is what the generated MSL uses.

  id<MTLSamplerState> state = [mDevice newSamplerStateWithDescriptor:native];
  if (state == nil) {
    emitMessage(MessageSeverity::Error, "the Metal device could not create the sampler state");
    return Result::OutOfMemory;
  }

  MetalSampler payload;
  payload.state = state;
  payload.comparison = comparison;

  *outSampler = mSamplers.allocate(std::move(payload));
  return outSampler->valid() ? Result::Success : Result::OutOfMemory;
}

void MetalDevice::destroySampler(const TextureSamplerHandle sampler) {
  if (MetalSampler* payload = mSamplers.get(sampler); payload != nullptr) {
    payload->state = nil;
    mSamplers.free(sampler);
  }
}

// ==================================================================================================
// Buffers
// ==================================================================================================

Result MetalDevice::createBuffer(const BufferDescriptor& descriptor, BufferHandle* outBuffer) {
  if (outBuffer == nullptr) return Result::InvalidArgument;
  *outBuffer = BufferHandle::invalid();
  if (!mInitialized) return Result::Error;

  const ValidationResult validation = validateBufferDescriptor(descriptor, mCapabilities);
  if (!validation.valid) {
    emitMessage(MessageSeverity::Error, std::string("buffer rejected: ") + validation.message);
    return toCreateResult(validation);
  }
  if (descriptor.size == 0) return Result::InvalidArgument;

  const MTLResourceOptions options = storageOptionsFor(descriptor.memoryUsage);
  id<MTLBuffer> buffer = nil;
  if (descriptor.initialData != nullptr && options == MTLResourceStorageModeShared) {
    buffer = [mDevice newBufferWithBytes:descriptor.initialData
                                  length:static_cast<NSUInteger>(descriptor.size)
                                 options:options];
  } else {
    buffer = [mDevice newBufferWithLength:static_cast<NSUInteger>(descriptor.size) options:options];
  }
  if (buffer == nil) {
    emitMessage(MessageSeverity::Error,
            "the Metal device could not allocate a " + std::to_string(descriptor.size) + " byte buffer");
    return Result::OutOfMemory;
  }
  if (descriptor.label != nullptr) buffer.label = [NSString stringWithUTF8String:descriptor.label];

  MetalBuffer payload;
  payload.buffer = buffer;
  payload.size = descriptor.size;
  payload.memoryUsage = descriptor.memoryUsage;

  // A private buffer cannot be written by the CPU, so its initial contents travel through a one-shot blit. That wait is
  // at creation time, which is where the RHI already allows a backend to be slow.
  if (descriptor.initialData != nullptr && options == MTLResourceStorageModePrivate) {
    const Result uploaded = uploadToBuffer(buffer, 0, descriptor.initialData, descriptor.size);
    if (!isSuccess(uploaded)) return uploaded;
  }

  *outBuffer = mBuffers.allocate(std::move(payload));
  if (!outBuffer->valid()) return Result::OutOfMemory;
  mResidentBufferBytes += descriptor.size;
  return Result::Success;
}

void MetalDevice::destroyBuffer(const BufferHandle bufferHandle) {
  MetalBuffer* buffer = mBuffers.get(bufferHandle);
  if (buffer == nullptr) return;
  mResidentBufferBytes -= std::min<uint64_t>(mResidentBufferBytes, buffer->size);
  buffer->mapped = nullptr;
  buffer->buffer = nil;
  mBuffers.free(bufferHandle);
}

Result MetalDevice::mapBuffer(const BufferHandle bufferHandle, const uint64_t offset, const uint64_t size,
        void** outPointer) {
  if (outPointer == nullptr) return Result::InvalidArgument;
  *outPointer = nullptr;

  MetalBuffer* buffer = mBuffers.get(bufferHandle);
  if (buffer == nullptr) return Result::InvalidHandle;
  if (buffer->memoryUsage == MemoryUsage::GpuOnly) {
    emitMessage(MessageSeverity::Error, "a GPU-only buffer cannot be mapped; create it with a host-visible MemoryUsage");
    return Result::Unsupported;
  }
  if (offset > buffer->size || size > buffer->size - offset) return Result::InvalidArgument;

  buffer->mapped = static_cast<uint8_t*>(buffer->buffer.contents) + offset;
  *outPointer = buffer->mapped;
  return Result::Success;
}

void MetalDevice::unmapBuffer(const BufferHandle bufferHandle) {
  if (MetalBuffer* buffer = mBuffers.get(bufferHandle); buffer != nullptr) {
    buffer->mapped = nullptr;
  }
}

// ==================================================================================================
// Shader modules
// ==================================================================================================

Result MetalDevice::createShaderModule(const ShaderModuleDescriptor& descriptor, ShaderModuleHandle* outModule) {
  if (outModule == nullptr) return Result::InvalidArgument;
  *outModule = ShaderModuleHandle::invalid();
  if (descriptor.code == nullptr || descriptor.codeSize == 0) {
    emitMessage(MessageSeverity::Error, "the shader module descriptor carries no code");
    return Result::InvalidArgument;
  }

  MetalShaderModule module;
  module.stage = descriptor.stage;
  if (descriptor.label != nullptr) module.label = descriptor.label;
  if (descriptor.entryPoint != nullptr && *descriptor.entryPoint != '\0') module.entryPoint = descriptor.entryPoint;

  std::vector<uint32_t> words;
  switch (descriptor.language) {
    case ShaderModuleDescriptor::Language::SPIRV: {
      if (descriptor.codeSize % sizeof(uint32_t) != 0) {
        emitMessage(MessageSeverity::Error, "the SPIR-V module's size is not a whole number of words");
        return Result::InvalidArgument;
      }
      const auto* source = static_cast<const uint32_t*>(descriptor.code);
      words.assign(source, source + descriptor.codeSize / sizeof(uint32_t));
      module.inputSpirv = words;
      module.sourceInfo.spirvInput = true;
      module.sourceInfo.language = ShaderSourceLanguage::Detect;
      module.sourceInfo.version = 0;
      module.sourceInfo.sourceSize = descriptor.codeSize;
      module.sourceInfo.sourceHash = hashBytes(words.data(), words.size() * sizeof(uint32_t));
      break;
    }
    case ShaderModuleDescriptor::Language::GLSL: {
      std::string source(static_cast<const char*>(descriptor.code), descriptor.codeSize);
      if (!source.empty() && source.back() == '\0') source.pop_back();

      // The Vulkan environment, deliberately: Metal's `(set, binding)` assignments come from the SPIR-V decorations,
      // and a module compiled for the OpenGL environment carries none -- every resource would land at (0, 0).
      const GlslCompileResult compiled = compileGlslToSpirvVulkan(source, descriptor.stage,
              descriptor.sourceLanguage, descriptor.entryPoint, descriptor.optimizeSource,
              module.label.empty() ? nullptr : module.label.c_str());
      if (!compiled.success()) {
        std::string message = "GLSL source compilation failed";
        if (!module.label.empty()) {
          message += " for ";
          message += module.label;
        }
        message += ": ";
        message += compiled.error;
        emitMessage(MessageSeverity::Error, message);
        return Result::ShaderCompilationFailed;
      }
      module.sourceInfo = compiled.sourceInfo;
      module.compiledSpirv = compiled.spirv;
      module.compiledFromSource = true;
      words = compiled.spirv;
      break;
    }
    case ShaderModuleDescriptor::Language::GLSLDirect:
      emitMessage(MessageSeverity::Error,
              "the Metal backend takes SPIR-V (or GLSL compiled to it here); Language::GLSLDirect is an OpenGL path");
      return Result::Unsupported;
  }

  if (words.empty()) {
    emitMessage(MessageSeverity::Error, "the shader module contains no SPIR-V");
    return Result::ShaderCompilationFailed;
  }
  constexpr uint32_t kSpirvMagicNumber = 0x07230203u;
  if (words.front() != kSpirvMagicNumber) {
    emitMessage(MessageSeverity::Error, "the supplied data is not a SPIR-V module (the magic number is missing)");
    return Result::ShaderCompilationFailed;
  }

  uint32_t pushConstantSize = 0;
  const ShaderReflection reflection = reflectSpirvModule(words.data(), words.size(), &pushConstantSize);
  module.pushConstantSize = pushConstantSize;

  module.bindingNames.reserve(reflection.bindings.size());
  for (const ShaderBindingInfo& binding: reflection.bindings) {
    if (!isDescriptorBinding(binding.type)) continue;
    module.bindingNames.push_back(binding.name);

    ShaderBindingDeclaration declaration;
    declaration.set = binding.set;
    declaration.binding = binding.binding;
    declaration.type = toBindingType(binding.type);
    declaration.arraySize = std::max(binding.arraySize, 1u);
    declaration.location = kNoShaderLocation;
    declaration.name = nullptr; // set below, after the name vector is final
    module.bindings.push_back(declaration);
  }
  // The pointers are taken after the vector reached its final size; a reallocation would leave every `name` dangling.
  for (size_t index = 0; index < module.bindings.size(); ++index) {
    module.bindings[index].name = module.bindingNames[index].c_str();
  }

  *outModule = mShaderModules.allocate(std::move(module));
  return outModule->valid() ? Result::Success : Result::OutOfMemory;
}

void MetalDevice::destroyShaderModule(const ShaderModuleHandle moduleHandle) {
  if (MetalShaderModule* module = mShaderModules.get(moduleHandle); module != nullptr) {
    mShaderModules.free(moduleHandle);
  }
}

Result MetalDevice::getShaderReflection(const ShaderModuleHandle moduleHandle,
        std::vector<ShaderBindingDeclaration>* outBindings) {
  if (outBindings == nullptr) return Result::InvalidArgument;
  const MetalShaderModule* module = mShaderModules.get(moduleHandle);
  if (module == nullptr) return Result::InvalidHandle;
  *outBindings = module->bindings;
  return Result::Success;
}

Result MetalDevice::getShaderSourceInfo(const ShaderModuleHandle moduleHandle, ShaderSourceInfo* outInfo) {
  if (outInfo == nullptr) return Result::InvalidArgument;
  const MetalShaderModule* module = mShaderModules.get(moduleHandle);
  if (module == nullptr) return Result::InvalidHandle;
  if (!module->compiledFromSource && module->inputSpirv.empty()) return Result::Unsupported;
  *outInfo = module->sourceInfo;
  return Result::Success;
}

Result MetalDevice::getShaderSpirv(const ShaderModuleHandle moduleHandle, std::vector<uint32_t>* outWords) {
  if (outWords == nullptr) return Result::InvalidArgument;
  const MetalShaderModule* module = mShaderModules.get(moduleHandle);
  if (module == nullptr) return Result::InvalidHandle;
  if (module->compiledFromSource) {
    *outWords = module->compiledSpirv;
  } else if (!module->inputSpirv.empty()) {
    *outWords = module->inputSpirv;
  } else {
    return Result::Unsupported;
  }
  return Result::Success;
}

// ==================================================================================================
// Descriptor set layouts and sets
// ==================================================================================================

Result MetalDevice::createDescriptorSetLayout(const DescriptorSetLayoutDescriptor& descriptor,
        DescriptorSetLayoutHandle* outLayout) {
  if (outLayout == nullptr) return Result::InvalidArgument;
  *outLayout = DescriptorSetLayoutHandle::invalid();

  const ValidationResult validation = validateDescriptorSetLayout(descriptor, mCapabilities);
  if (!validation.valid) {
    emitMessage(MessageSeverity::Error, std::string("descriptor set layout rejected: ") + validation.message);
    return toCreateResult(validation);
  }

  MetalDescriptorSetLayout layout;
  layout.bindingCount = descriptor.bindingCount;
  for (uint32_t index = 0; index < descriptor.bindingCount; ++index) {
    const DescriptorBinding& binding = descriptor.bindings[index];
    layout.bindings[index] = binding;
    const uint32_t count = std::max(binding.count, 1u);
    switch (binding.type) {
      case BindingType::TextureSampler:
        layout.textureIndex[index] = layout.textureCount;
        layout.textureCount += count;
        layout.samplerIndex[index] = layout.samplerCount;
        layout.samplerCount += count;
        break;
      case BindingType::Texture:
        layout.textureIndex[index] = layout.textureCount;
        layout.textureCount += count;
        break;
      case BindingType::Sampler:
        layout.samplerIndex[index] = layout.samplerCount;
        layout.samplerCount += count;
        break;
      case BindingType::UniformBuffer:
      case BindingType::ReadOnlyStorageBuffer:
      case BindingType::StorageBuffer:
        layout.bufferIndex[index] = layout.bufferCount;
        layout.bufferCount += count;
        break;
    }
  }

  *outLayout = mDescriptorSetLayouts.allocate(std::move(layout));
  return outLayout->valid() ? Result::Success : Result::OutOfMemory;
}

void MetalDevice::destroyDescriptorSetLayout(const DescriptorSetLayoutHandle layout) {
  mDescriptorSetLayouts.free(layout);
}

Result MetalDevice::createDescriptorSet(const DescriptorSetLayoutHandle layoutHandle, DescriptorSetHandle* outSet) {
  if (outSet == nullptr) return Result::InvalidArgument;
  *outSet = DescriptorSetHandle::invalid();

  const MetalDescriptorSetLayout* layout = mDescriptorSetLayouts.get(layoutHandle);
  if (layout == nullptr) return Result::InvalidHandle;

  MetalDescriptorSet set;
  set.layout = layoutHandle;
  for (uint32_t index = 0; index < layout->bindingCount; ++index) {
    const DescriptorBinding& binding = layout->bindings[index];
    const uint32_t count = std::max(binding.count, 1u);
    for (uint32_t element = 0; element < count; ++element) {
      MetalDescriptorBindingValue value;
      value.binding = binding.binding;
      value.type = binding.type;
      value.arrayElement = element;
      set.values.push_back(value);
    }
  }

  *outSet = mDescriptorSets.allocate(std::move(set));
  return outSet->valid() ? Result::Success : Result::OutOfMemory;
}

void MetalDevice::destroyDescriptorSet(const DescriptorSetHandle setHandle) {
  mDescriptorSets.free(setHandle);
}

Result MetalDevice::updateDescriptorSet(const DescriptorSetHandle setHandle, const DescriptorWrite* writes,
        const uint32_t writeCount) {
  if (writes == nullptr && writeCount != 0) return Result::InvalidArgument;
  MetalDescriptorSet* set = mDescriptorSets.get(setHandle);
  if (set == nullptr) return Result::InvalidHandle;
  const MetalDescriptorSetLayout* layout = mDescriptorSetLayouts.get(set->layout);
  if (layout == nullptr) return Result::InvalidHandle;

  // Validate the whole batch before applying any of it: a half-applied update would leave a set that is neither the old
  // one nor the new one, and the next draw would fail for a reason unrelated to the write that was rejected.
  for (uint32_t index = 0; index < writeCount; ++index) {
    const DescriptorWrite& write = writes[index];
    const ValidationResult validation = validateDescriptorWrite(write, mCapabilities);
    if (!validation.valid) {
      emitMessage(MessageSeverity::Error, std::string("descriptor write rejected: ") + validation.message);
      return toCreateResult(validation);
    }

    bool found = false;
    for (const DescriptorBinding& binding: layout->bindings) {
      if (binding.binding == write.binding) {
        found = true;
        break;
      }
    }
    if (!found) {
      emitMessage(MessageSeverity::Error,
              "descriptor write named binding " + std::to_string(write.binding) + ", which the set's layout does not declare");
      return Result::InvalidArgument;
    }
    if (write.type == BindingType::TextureSampler || write.type == BindingType::Texture) {
      const MetalTexture* texture = mTextures.get(write.texture);
      if (texture == nullptr && write.view == TextureViewHandle::invalid()) {
        emitMessage(MessageSeverity::Error, "descriptor write named a texture handle that is not alive");
        return Result::InvalidHandle;
      }
    }
    if (write.type == BindingType::TextureSampler || write.type == BindingType::Sampler) {
      if (mSamplers.get(write.sampler) == nullptr && write.type == BindingType::TextureSampler) {
        emitMessage(MessageSeverity::Error, "descriptor write named a sampler handle that is not alive");
        return Result::InvalidHandle;
      }
    }
    if (write.type == BindingType::UniformBuffer || write.type == BindingType::ReadOnlyStorageBuffer ||
        write.type == BindingType::StorageBuffer) {
      if (mBuffers.get(write.buffer.buffer) == nullptr) {
        emitMessage(MessageSeverity::Error, "descriptor write named a buffer handle that is not alive");
        return Result::InvalidHandle;
      }
    }
  }

  for (uint32_t index = 0; index < writeCount; ++index) {
    const DescriptorWrite& write = writes[index];
    const auto existing = std::find_if(set->values.begin(), set->values.end(), [&write](const MetalDescriptorBindingValue& value) {
      return value.binding == write.binding && value.arrayElement == write.arrayElement;
    });
    if (existing == set->values.end()) continue;
    existing->type = write.type;
    existing->texture = write.texture;
    existing->view = write.view;
    existing->sampler = write.sampler;
    existing->buffer = write.buffer;
    existing->written = true;
  }
  ++set->version;
  return Result::Success;
}

// ==================================================================================================
// Pipeline layouts
// ==================================================================================================

Result MetalDevice::createPipelineLayout(const PipelineLayoutDescriptor& descriptor, PipelineLayoutHandle* outLayout) {
  if (outLayout == nullptr) return Result::InvalidArgument;
  *outLayout = PipelineLayoutHandle::invalid();

  if (descriptor.setLayoutCount > kMaxDescriptorSetLayouts) {
    emitMessage(MessageSeverity::Error, "a pipeline layout may declare at most 4 descriptor set layouts");
    return Result::InvalidArgument;
  }

  MetalPipelineLayout layout;
  layout.setLayoutCount = descriptor.setLayoutCount;
  uint32_t textureCount = 0;
  uint32_t samplerCount = 0;
  uint32_t bufferCount = 0;

  for (uint32_t setIndex = 0; setIndex < descriptor.setLayoutCount; ++setIndex) {
    layout.setLayouts[setIndex] = descriptor.setLayouts[setIndex];
    const MetalDescriptorSetLayout* setLayout = mDescriptorSetLayouts.get(descriptor.setLayouts[setIndex]);
    // Buffer slots start after the eight vertex-stream slots; textures and samplers start at zero because Metal has a
    // separate argument table for each. Getting the buffer base wrong is not a validation error -- slot 0 is a legal
    // buffer argument -- it is a draw that silently reads the vertex stream as its uniform block.
    const uint32_t bufferBase = kFirstResourceBufferSlot + bufferCount;
    if (setLayout == nullptr) {
      // The Vulkan backend substitutes an empty layout here rather than refusing: a pipeline that binds no set at that
      // slot is legal, and refusing would make an otherwise usable pipeline fail for an unused slot.
      layout.sets[setIndex] = MetalPipelineLayout::SetSlots{ textureCount, samplerCount, bufferBase };
      continue;
    }
    layout.sets[setIndex] = MetalPipelineLayout::SetSlots{ textureCount, samplerCount, bufferBase };
    textureCount += setLayout->textureCount;
    samplerCount += setLayout->samplerCount;
    bufferCount += setLayout->bufferCount;
  }

  if (textureCount > kMaxTextureSlots) {
    emitMessage(MessageSeverity::Error,
            "the pipeline layout needs " + std::to_string(textureCount) + " texture slots, and Metal allows " +
                    std::to_string(kMaxTextureSlots) + " per stage");
    return Result::Unsupported;
  }
  if (samplerCount > kMaxSamplerSlots) {
    emitMessage(MessageSeverity::Error,
            "the pipeline layout needs " + std::to_string(samplerCount) + " sampler slots, and Metal allows " +
                    std::to_string(kMaxSamplerSlots) + " per stage");
    return Result::Unsupported;
  }
  // The last buffer slot is reserved for push constants; slots below the first resource slot belong to vertex streams.
  if (kFirstResourceBufferSlot + bufferCount > kPushConstantBufferSlot) {
    emitMessage(MessageSeverity::Error,
            "the pipeline layout needs " + std::to_string(bufferCount) + " buffer slots, and only " +
                    std::to_string(kPushConstantBufferSlot - kFirstResourceBufferSlot) +
                    " are available after the vertex-stream slots and before the push-constant slot");
    return Result::Unsupported;
  }

  layout.textureCount = textureCount;
  layout.samplerCount = samplerCount;
  layout.bufferCount = bufferCount;

  *outLayout = mPipelineLayouts.allocate(std::move(layout));
  return outLayout->valid() ? Result::Success : Result::OutOfMemory;
}

void MetalDevice::destroyPipelineLayout(const PipelineLayoutHandle layout) {
  mPipelineLayouts.free(layout);
}

// ==================================================================================================
// Pipelines
// ==================================================================================================

Result MetalDevice::createRenderPipeline(const RenderPipelineDescriptor& descriptor,
        RenderPipelineHandle* outPipeline) {
  if (outPipeline == nullptr) return Result::InvalidArgument;
  *outPipeline = RenderPipelineHandle::invalid();
  if (!mInitialized) return Result::Error;

  // The compatible pass, when supplied, is what the pipeline's attachment formats have to agree with; the validator
  // uses it for that and for the sample-count check.
  const MetalRenderPass* compatiblePass = mRenderPasses.get(descriptor.compatibleRenderPass);
  RenderPassDescriptor passDescriptor;
  if (compatiblePass != nullptr) {
    passDescriptor.colorAttachmentCount = compatiblePass->colorAttachmentCount;
    for (uint32_t index = 0; index < compatiblePass->colorAttachmentCount; ++index) {
      passDescriptor.colorFormats[index] = compatiblePass->colorFormats[index];
    }
    passDescriptor.depthStencilFormat = compatiblePass->depthStencilFormat;
    passDescriptor.sampleCount = compatiblePass->sampleCount;
  }

  const ValidationResult validation = validateRenderPipeline(descriptor, mCapabilities,
          compatiblePass != nullptr ? &passDescriptor : nullptr);
  if (!validation.valid) {
    emitMessage(MessageSeverity::Error, std::string("render pipeline rejected: ") + validation.message);
    return toCreateResult(validation);
  }

  MetalShaderModule* vertexModule = mShaderModules.get(descriptor.vertexModule);
  if (vertexModule == nullptr) {
    emitMessage(MessageSeverity::Error, "the render pipeline's vertex module handle is not alive");
    return Result::InvalidHandle;
  }
  MetalShaderModule* fragmentModule = descriptor.fragmentModule.valid() ? mShaderModules.get(descriptor.fragmentModule)
                                                                       : nullptr;
  if (descriptor.fragmentModule.valid() && fragmentModule == nullptr) {
    emitMessage(MessageSeverity::Error, "the render pipeline's fragment module handle is not alive");
    return Result::InvalidHandle;
  }

  MetalPipelineLayout emptyLayout;
  const MetalPipelineLayout* layout = mPipelineLayouts.get(descriptor.layout);
  PipelineLayoutHandle layoutHandle = descriptor.layout;
  if (layout == nullptr) {
    // A pipeline with no layout binds no descriptor sets; the empty layout still gives push constants their slot.
    layout = &emptyLayout;
    layoutHandle = PipelineLayoutHandle::invalid();
  }

  // ------------------------------------------------------------------------------------------------
  // MSL translation, per stage, against this pipeline's slot assignment
  // ------------------------------------------------------------------------------------------------
  const auto compileStage = [&](MetalShaderModule& module, id<MTLFunction>* outFunction,
                                std::vector<uint32_t>* outAttributes, std::string* outError) -> Result {
    std::vector<MslResourceBinding> mappings;
    mappings.reserve(module.bindings.size());
    bool missing = false;
    for (const ShaderBindingDeclaration& declaration: module.bindings) {
      MslResourceBinding mapping;
      mapping.stage = module.stage;
      mapping.set = declaration.set;
      mapping.binding = declaration.binding;
      mapping.count = std::max(declaration.arraySize, 1u);

      const MetalDescriptorSetLayout* setLayout = nullptr;
      if (declaration.set < layout->setLayoutCount) {
        setLayout = mDescriptorSetLayouts.get(layout->setLayouts[declaration.set]);
      }
      int bindingIndex = -1;
      if (setLayout != nullptr) {
        for (uint32_t index = 0; index < setLayout->bindingCount; ++index) {
          if (setLayout->bindings[index].binding == declaration.binding) {
            bindingIndex = static_cast<int>(index);
            break;
          }
        }
      } else if (declaration.set < layout->setLayoutCount) {
        // A declared set whose layout handle no longer resolves is a caller error the bind path cannot repair.
        missing = true;
      }

      if (bindingIndex >= 0) {
        const MetalPipelineLayout::SetSlots& slots = layout->sets[declaration.set];
        switch (declaration.type) {
          case BindingType::TextureSampler:
            mapping.mslTexture = slots.textureBase + setLayout->textureIndex[bindingIndex];
            mapping.mslSampler = slots.samplerBase + setLayout->samplerIndex[bindingIndex];
            break;
          case BindingType::Texture:
            mapping.mslTexture = slots.textureBase + setLayout->textureIndex[bindingIndex];
            break;
          case BindingType::Sampler:
            mapping.mslSampler = slots.samplerBase + setLayout->samplerIndex[bindingIndex];
            break;
          case BindingType::UniformBuffer:
          case BindingType::ReadOnlyStorageBuffer:
          case BindingType::StorageBuffer:
            mapping.mslBuffer = slots.bufferBase + setLayout->bufferIndex[bindingIndex];
            break;
        }
      } else {
        // The module declares a resource the layout does not. A shader that does not read it is harmless, and a
        // resource that *is* read would have been unbound anyway; assigning distinct reserved slots keeps the MSL
        // valid and leaves a diagnostic that names the pair.
        missing = true;
        mapping.mslBuffer = kPushConstantBufferSlot - 1;
        mapping.mslTexture = kMaxTextureSlots - 1;
        mapping.mslSampler = kMaxSamplerSlots - 1;
      }
      mappings.push_back(mapping);
    }

    if (missing) {
      emitMessage(MessageSeverity::Warning,
              "shader module " + (module.label.empty() ? std::string("(unnamed)") : module.label) +
                      " declares a descriptor binding the pipeline layout does not, so it was assigned a reserved slot "
                      "and will read the fallback resource if the shader is actually used");
    }

    MslCompileInfo info;
    info.spirv = module.compiledFromSource ? module.compiledSpirv.data() : module.inputSpirv.data();
    info.wordCount = module.compiledFromSource ? module.compiledSpirv.size() : module.inputSpirv.size();
    info.stage = module.stage;
    info.entryPoint = module.entryPoint.c_str();
    info.bindings = mappings.data();
    info.bindingCount = mappings.size();
    info.hasPushConstants = module.pushConstantSize > 0;
    info.ios = mIos;

    const MslCompileResult compiled = crossCompileToMsl(info);
    if (!compiled) {
      *outError = compiled.error;
      return Result::ShaderCompilationFailed;
    }
    if (outAttributes != nullptr) *outAttributes = compiled.attributeLocations;

    MTLCompileOptions* options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion2_2;
    // A shader whose arithmetic can be reassociated is exactly what a canvas cannot have: the gradient and blend maths
    // are compared against the CPU rasterizer.
    options.fastMathEnabled = NO;

    NSString* source = [NSString stringWithUTF8String:compiled.source.c_str()];
    if (source == nil) {
      *outError = "the generated MSL is not valid UTF-8";
      return Result::ShaderCompilationFailed;
    }
    NSError* error = nil;
    id<MTLLibrary> library = [mDevice newLibraryWithSource:source options:options error:&error];
    if (library == nil) {
      *outError = std::string("the Metal shader compiler rejected the generated MSL: ") + describe(error) +
                  (module.label.empty() ? std::string() : " (module " + module.label + ')');
      return Result::ShaderCompilationFailed;
    }

    NSString* entry = [NSString stringWithUTF8String:compiled.entryPoint.c_str()];
    id<MTLFunction> function = [library newFunctionWithName:entry];
    if (function == nil) {
      *outError = "the Metal shader compiler produced no function named " + compiled.entryPoint;
      return Result::ShaderCompilationFailed;
    }
    *outFunction = function;
    return Result::Success;
  };

  id<MTLFunction> vertexFunction = nil;
  id<MTLFunction> fragmentFunction = nil;
  std::vector<uint32_t> compiledVertexAttributes;
  std::string compileError;
  Result compiled = compileStage(*vertexModule, &vertexFunction, &compiledVertexAttributes, &compileError);
  if (!isSuccess(compiled)) {
    emitMessage(MessageSeverity::Error, compileError);
    return compiled;
  }
  if (fragmentModule != nullptr) {
    compiled = compileStage(*fragmentModule, &fragmentFunction, nullptr, &compileError);
    if (!isSuccess(compiled)) {
      emitMessage(MessageSeverity::Error, compileError);
      return compiled;
    }
  }

  // ------------------------------------------------------------------------------------------------
  // Vertex descriptor
  // ------------------------------------------------------------------------------------------------
  MTLVertexDescriptor* vertexDescriptor = [MTLVertexDescriptor vertexDescriptor];
  if (descriptor.vertexInput.bufferCount > kMaxVertexBufferSlots) {
    emitMessage(MessageSeverity::Error,
            "the pipeline declares " + std::to_string(descriptor.vertexInput.bufferCount) +
                    " vertex buffers; this backend reserves 8 buffer argument slots for vertex streams");
    return Result::Unsupported;
  }
  for (uint32_t index = 0; index < descriptor.vertexInput.bufferCount; ++index) {
    const VertexBufferLayout& buffer = descriptor.vertexInput.buffers[index];
    vertexDescriptor.layouts[index].stride = static_cast<NSUInteger>(buffer.stride);
    vertexDescriptor.layouts[index].stepFunction =
            buffer.stepMode == VertexStepMode::Instance ? MTLVertexStepFunctionPerInstance
                                                        : MTLVertexStepFunctionPerVertex;
    vertexDescriptor.layouts[index].stepRate = 1;
  }
  for (uint32_t index = 0; index < descriptor.vertexInput.attributeCount; ++index) {
    const VertexAttribute& attribute = descriptor.vertexInput.attributes[index];
    const MTLVertexFormat format = toMtlVertexFormat(attribute.format);
    if (format == MTLVertexFormatInvalid) {
      emitMessage(MessageSeverity::Error, "the pipeline declares a vertex attribute whose format has no Metal equivalent");
      return Result::Unsupported;
    }
    if (attribute.shaderLocation >= 31) {
      emitMessage(MessageSeverity::Error, "a vertex attribute location must be below 31 on Metal");
      return Result::InvalidArgument;
    }
    vertexDescriptor.attributes[attribute.shaderLocation].format = format;
    vertexDescriptor.attributes[attribute.shaderLocation].offset = static_cast<NSUInteger>(attribute.offset);
    vertexDescriptor.attributes[attribute.shaderLocation].bufferIndex = attribute.bufferIndex;
  }

  // ## Attributes the descriptor does not declare
  //
  // A vertex shader can be shared by pipeline variants that supply different vertex streams. `quad.vert` is exactly
  // that: the SDF variant declares nine attributes and the plain variant four, while the module reads all nine and
  // forwards them. OpenGL tolerates the absent ones (they read as whatever the disabled array left behind); Metal
  // refuses to create the pipeline, with "Vertex attribute aSdf(4) is missing from the vertex descriptor".
  //
  // The missing locations are described against a dummy stream slot and bound to a zero-filled buffer at draw time,
  // which makes the variant's reads defined and empty rather than a pipeline-creation failure. `attributeLocations`
  // comes from the module's own SPIR-V, so only attributes the vertex function actually reads are filled.
  uint32_t dummySlot = MetalRenderPipeline::kNoDummyVertexSlot;
  std::vector<uint32_t> missingAttributes;
  for (const uint32_t location: compiledVertexAttributes) {
    if (location < 31 && vertexDescriptor.attributes[location].format == MTLVertexFormatInvalid) {
      missingAttributes.push_back(location);
    }
  }
  if (!missingAttributes.empty()) {
    uint32_t unusedSlot = descriptor.vertexInput.bufferCount;
    if (unusedSlot >= kMaxVertexBufferSlots) {
      // Every reserved stream slot is in use. Falling back to slot 0 keeps the pipeline creatable; the values read are
      // then whatever that stream's first four bytes hold, which is the same "undefined but in bounds" contract the
      // variant has on the other backends.
      unusedSlot = 0;
    }
    // The layout has to exist before an attribute can name it, and a layout no attribute names makes Metal assert at
    // pipeline creation -- so it is created only when something is actually missing.
    vertexDescriptor.layouts[unusedSlot].stride = std::max<NSUInteger>(vertexDescriptor.layouts[unusedSlot].stride, 16);
    vertexDescriptor.layouts[unusedSlot].stepFunction = MTLVertexStepFunctionPerVertex;
    vertexDescriptor.layouts[unusedSlot].stepRate = 1;
    for (const uint32_t location: missingAttributes) {
      vertexDescriptor.attributes[location].format = MTLVertexFormatFloat4;
      vertexDescriptor.attributes[location].offset = 0;
      vertexDescriptor.attributes[location].bufferIndex = unusedSlot;
    }
    dummySlot = unusedSlot;
  }

  // ------------------------------------------------------------------------------------------------
  // Pipeline state
  // ------------------------------------------------------------------------------------------------
  const PixelFormat depthFormat = descriptor.depthStencilFormat != PixelFormat::Undefined
                                          ? descriptor.depthStencilFormat
                                          : (compatiblePass != nullptr ? compatiblePass->depthStencilFormat
                                                                       : PixelFormat::Undefined);
  const uint32_t sampleCount = std::max({ descriptor.multisample.sampleCount,
          compatiblePass != nullptr ? compatiblePass->sampleCount : 1u, 1u });

  MTLRenderPipelineDescriptor* pipelineDescriptor = [MTLRenderPipelineDescriptor new];
  // `setLabel:` asserts on nil rather than clearing the label, so an absent label must not be assigned.
  if (descriptor.label != nullptr) pipelineDescriptor.label = [NSString stringWithUTF8String:descriptor.label];
  pipelineDescriptor.vertexFunction = vertexFunction;
  pipelineDescriptor.fragmentFunction = fragmentFunction;
  pipelineDescriptor.vertexDescriptor = vertexDescriptor;
  pipelineDescriptor.rasterSampleCount = sampleCount;
  pipelineDescriptor.alphaToCoverageEnabled = descriptor.multisample.alphaToCoverageEnable;

  const uint32_t colorTargetCount = descriptor.colorTargetCount > 0
                                            ? descriptor.colorTargetCount
                                            : (compatiblePass != nullptr ? compatiblePass->colorAttachmentCount : 0);
  for (uint32_t index = 0; index < colorTargetCount; ++index) {
    const PixelFormat format = descriptor.colorTargets[index].format != PixelFormat::Undefined
                                       ? descriptor.colorTargets[index].format
                                       : (compatiblePass != nullptr ? compatiblePass->colorFormats[index]
                                                                    : PixelFormat::Undefined);
    const MTLPixelFormat mtlFormat = toMtlPixelFormat(format);
    if (mtlFormat == MTLPixelFormatInvalid) {
      emitMessage(MessageSeverity::Error, "a colour attachment's format has no Metal equivalent");
      return Result::Unsupported;
    }
    MTLRenderPipelineColorAttachmentDescriptor* attachment = pipelineDescriptor.colorAttachments[index];
    attachment.pixelFormat = mtlFormat;
    attachment.writeMask = toMtlColorWriteMask(descriptor.colorTargets[index].writeMask);

    const BlendState& blend = descriptor.colorTargets[index].blend;
    attachment.blendingEnabled = blend.blendEnable;
    if (blend.blendEnable) {
      attachment.sourceRGBBlendFactor = toMtlBlendFactor(blend.color.srcFactor);
      attachment.destinationRGBBlendFactor = toMtlBlendFactor(blend.color.dstFactor);
      attachment.rgbBlendOperation = toMtlBlendOperation(blend.color.operation);
      attachment.sourceAlphaBlendFactor = toMtlBlendFactor(blend.alpha.srcFactor);
      attachment.destinationAlphaBlendFactor = toMtlBlendFactor(blend.alpha.dstFactor);
      attachment.alphaBlendOperation = toMtlBlendOperation(blend.alpha.operation);
    }
  }

  if (depthFormat != PixelFormat::Undefined) {
    const MTLPixelFormat mtlDepth = toMtlDepthStencilPixelFormat(depthFormat);
    if (mtlDepth == MTLPixelFormatInvalid) return Result::Unsupported;
    pipelineDescriptor.depthAttachmentPixelFormat = mtlDepth;
    if (isStencilFormat(depthFormat)) pipelineDescriptor.stencilAttachmentPixelFormat = mtlDepth;
  }

  NSError* pipelineError = nil;
  id<MTLRenderPipelineState> state = [mDevice newRenderPipelineStateWithDescriptor:pipelineDescriptor
                                                                              error:&pipelineError];
  if (state == nil) {
    std::string message = "the Metal device could not create the render pipeline: " + describe(pipelineError);
    if (descriptor.label != nullptr) {
      message += " (pipeline ";
      message += descriptor.label;
      message += ')';
    }
    emitMessage(MessageSeverity::Error, message);
    return Result::Error;
  }
  // ------------------------------------------------------------------------------------------------
  // Depth/stencil state
  // ------------------------------------------------------------------------------------------------
  MTLDepthStencilDescriptor* depthDescriptor = [MTLDepthStencilDescriptor new];
  const bool depthEnabled = depthFormat != PixelFormat::Undefined && descriptor.depthStencil.depthTestEnable;
  depthDescriptor.depthCompareFunction =
          depthEnabled ? toMtlCompareFunction(descriptor.depthStencil.depthCompare) : MTLCompareFunctionAlways;
  depthDescriptor.depthWriteEnabled = depthEnabled && descriptor.depthStencil.depthWriteEnable;
  if (depthFormat != PixelFormat::Undefined && isStencilFormat(depthFormat) &&
      descriptor.depthStencil.stencilEnable) {
    const auto makeStencil = [](const StencilFaceState& face) {
      MTLStencilDescriptor* stencil = [MTLStencilDescriptor new];
      stencil.stencilCompareFunction = toMtlCompareFunction(face.compare);
      stencil.stencilFailureOperation = toMtlStencilOperation(face.failOp);
      stencil.depthFailureOperation = toMtlStencilOperation(face.depthFailOp);
      stencil.depthStencilPassOperation = toMtlStencilOperation(face.passOp);
      stencil.readMask = face.readMask;
      stencil.writeMask = face.writeMask;
      return stencil;
    };
    depthDescriptor.frontFaceStencil = makeStencil(descriptor.depthStencil.front);
    depthDescriptor.backFaceStencil = makeStencil(descriptor.depthStencil.back);
  }
  id<MTLDepthStencilState> depthState = [mDevice newDepthStencilStateWithDescriptor:depthDescriptor];
  if (depthState == nil) return Result::OutOfMemory;

  MetalRenderPipeline pipeline;
  pipeline.state = state;
  pipeline.depthStencilState = depthState;
  pipeline.layout = layoutHandle;
  pipeline.descriptor = descriptor;
  pipeline.primitiveType = toMtlPrimitiveType(descriptor.rasterizer.topology);
  pipeline.fanEmulation = descriptor.rasterizer.topology == PrimitiveTopology::TriangleFan;
  pipeline.dummyVertexSlot = dummySlot;
  if (descriptor.rasterizer.topology == PrimitiveTopology::PatchList) {
    emitMessage(MessageSeverity::Error, "tessellation patches are not implemented by the Metal backend");
    return Result::Unsupported;
  }

  *outPipeline = mRenderPipelines.allocate(std::move(pipeline));
  return outPipeline->valid() ? Result::Success : Result::OutOfMemory;
}

void MetalDevice::destroyRenderPipeline(const RenderPipelineHandle pipeline) {
  if (MetalRenderPipeline* payload = mRenderPipelines.get(pipeline); payload != nullptr) {
    payload->state = nil;
    payload->depthStencilState = nil;
    mRenderPipelines.free(pipeline);
  }
}

Result MetalDevice::createComputePipeline(const ComputePipelineDescriptor& descriptor,
        ComputePipelineHandle* outPipeline) {
  if (outPipeline == nullptr) return Result::InvalidArgument;
  *outPipeline = ComputePipelineHandle::invalid();

  const ValidationResult validation = validateComputePipeline(descriptor, mCapabilities);
  if (!validation.valid) {
    emitMessage(MessageSeverity::Error, std::string("compute pipeline rejected: ") + validation.message);
    return toCreateResult(validation);
  }

  MetalShaderModule* module = mShaderModules.get(descriptor.computeModule);
  if (module == nullptr) return Result::InvalidHandle;

  MetalPipelineLayout emptyLayout;
  const MetalPipelineLayout* layout = mPipelineLayouts.get(descriptor.layout);
  PipelineLayoutHandle layoutHandle = descriptor.layout;
  if (layout == nullptr) {
    layout = &emptyLayout;
    layoutHandle = PipelineLayoutHandle::invalid();
  }

  std::vector<MslResourceBinding> mappings;
  for (const ShaderBindingDeclaration& declaration: module->bindings) {
    MslResourceBinding mapping;
    mapping.stage = ShaderStage::Compute;
    mapping.set = declaration.set;
    mapping.binding = declaration.binding;
    mapping.count = std::max(declaration.arraySize, 1u);
    const MetalDescriptorSetLayout* setLayout = nullptr;
    if (declaration.set < layout->setLayoutCount) {
      setLayout = mDescriptorSetLayouts.get(layout->setLayouts[declaration.set]);
    }
    int bindingIndex = -1;
    if (setLayout != nullptr) {
      for (uint32_t index = 0; index < setLayout->bindingCount; ++index) {
        if (setLayout->bindings[index].binding == declaration.binding) {
          bindingIndex = static_cast<int>(index);
          break;
        }
      }
    }
    if (bindingIndex >= 0) {
      const MetalPipelineLayout::SetSlots& slots = layout->sets[declaration.set];
      switch (declaration.type) {
        case BindingType::TextureSampler:
          mapping.mslTexture = slots.textureBase + setLayout->textureIndex[bindingIndex];
          mapping.mslSampler = slots.samplerBase + setLayout->samplerIndex[bindingIndex];
          break;
        case BindingType::Texture:
          mapping.mslTexture = slots.textureBase + setLayout->textureIndex[bindingIndex];
          break;
        case BindingType::Sampler:
          mapping.mslSampler = slots.samplerBase + setLayout->samplerIndex[bindingIndex];
          break;
        case BindingType::UniformBuffer:
        case BindingType::ReadOnlyStorageBuffer:
        case BindingType::StorageBuffer:
          mapping.mslBuffer = slots.bufferBase + setLayout->bufferIndex[bindingIndex];
          break;
      }
    } else {
      mapping.mslBuffer = kPushConstantBufferSlot - 1;
      mapping.mslTexture = kMaxTextureSlots - 1;
      mapping.mslSampler = kMaxSamplerSlots - 1;
    }
    mappings.push_back(mapping);
  }

  MslCompileInfo info;
  info.spirv = module->compiledFromSource ? module->compiledSpirv.data() : module->inputSpirv.data();
  info.wordCount = module->compiledFromSource ? module->compiledSpirv.size() : module->inputSpirv.size();
  info.stage = ShaderStage::Compute;
  info.entryPoint = module->entryPoint.c_str();
  info.bindings = mappings.data();
  info.bindingCount = mappings.size();
  info.hasPushConstants = module->pushConstantSize > 0;
  info.ios = mIos;

  const MslCompileResult compiled = crossCompileToMsl(info);
  if (!compiled) {
    emitMessage(MessageSeverity::Error, compiled.error);
    return Result::ShaderCompilationFailed;
  }

  MTLCompileOptions* options = [MTLCompileOptions new];
  options.languageVersion = MTLLanguageVersion2_2;
  options.fastMathEnabled = NO;
  NSError* error = nil;
  id<MTLLibrary> library = [mDevice newLibraryWithSource:[NSString stringWithUTF8String:compiled.source.c_str()]
                                                options:options
                                                  error:&error];
  if (library == nil) {
    emitMessage(MessageSeverity::Error,
            std::string("the Metal shader compiler rejected the generated compute MSL: ") + describe(error));
    return Result::ShaderCompilationFailed;
  }
  id<MTLFunction> function =
          [library newFunctionWithName:[NSString stringWithUTF8String:compiled.entryPoint.c_str()]];
  if (function == nil) return Result::ShaderCompilationFailed;

  NSError* pipelineError = nil;
  id<MTLComputePipelineState> state = [mDevice newComputePipelineStateWithFunction:function error:&pipelineError];
  if (state == nil) {
    emitMessage(MessageSeverity::Error,
            std::string("the Metal device could not create the compute pipeline: ") + describe(pipelineError));
    return Result::Error;
  }

  MetalComputePipeline pipeline;
  pipeline.state = state;
  pipeline.layout = layoutHandle;
  pipeline.workgroupSizeX = std::max(descriptor.workgroupSizeX, 1u);
  pipeline.workgroupSizeY = std::max(descriptor.workgroupSizeY, 1u);
  pipeline.workgroupSizeZ = std::max(descriptor.workgroupSizeZ, 1u);

  *outPipeline = mComputePipelines.allocate(std::move(pipeline));
  return outPipeline->valid() ? Result::Success : Result::OutOfMemory;
}

void MetalDevice::destroyComputePipeline(const ComputePipelineHandle pipeline) {
  if (MetalComputePipeline* payload = mComputePipelines.get(pipeline); payload != nullptr) {
    payload->state = nil;
    mComputePipelines.free(pipeline);
  }
}

// ==================================================================================================
// Render passes and framebuffers
// ==================================================================================================

Result MetalDevice::createRenderPass(const RenderPassDescriptor& descriptor, RenderPassHandle* outPass) {
  if (outPass == nullptr) return Result::InvalidArgument;
  *outPass = RenderPassHandle::invalid();

  const ValidationResult validation = validateRenderPassDescriptor(descriptor, mCapabilities);
  if (!validation.valid) {
    emitMessage(MessageSeverity::Error, std::string("render pass rejected: ") + validation.message);
    return toCreateResult(validation);
  }

  MetalRenderPass pass;
  pass.colorAttachmentCount = descriptor.colorAttachmentCount;
  for (uint32_t index = 0; index < descriptor.colorAttachmentCount; ++index) {
    pass.colorFormats[index] = descriptor.colorFormats[index];
  }
  pass.depthStencilFormat = descriptor.depthStencilFormat;
  pass.sampleCount = std::max(descriptor.sampleCount, 1u);

  *outPass = mRenderPasses.allocate(std::move(pass));
  return outPass->valid() ? Result::Success : Result::OutOfMemory;
}

void MetalDevice::destroyRenderPass(const RenderPassHandle renderPass) {
  mRenderPasses.free(renderPass);
}

Result MetalDevice::createFramebuffer(const FramebufferDescriptor& descriptor, FramebufferHandle* outFramebuffer) {
  if (outFramebuffer == nullptr) return Result::InvalidArgument;
  *outFramebuffer = FramebufferHandle::invalid();

  const MetalRenderPass* pass = mRenderPasses.get(descriptor.renderPass);
  if (pass == nullptr) return Result::InvalidHandle;
  if (descriptor.colorAttachmentCount != pass->colorAttachmentCount) return Result::InvalidArgument;

  MetalFramebuffer framebuffer;
  framebuffer.renderPass = descriptor.renderPass;
  framebuffer.colorAttachmentCount = descriptor.colorAttachmentCount;
  framebuffer.width = descriptor.width;
  framebuffer.height = descriptor.height;
  for (uint32_t index = 0; index < descriptor.colorAttachmentCount; ++index) {
    TextureViewHandle view = descriptor.colorAttachments[index];
    // The default-framebuffer sentinel is resolved now so an invalid one is refused at creation rather than at the
    // first draw; the resolved object is deliberately not stored.
    if (view.valid()) {
      MetalTextureView resolved;
      if (!isSuccess(resolveTextureView(view, resolved))) {
        emitMessage(MessageSeverity::Error, "the framebuffer names a colour attachment view that is not alive");
        return Result::InvalidHandle;
      }
    }
    framebuffer.colorAttachments[index] = view;
  }
  framebuffer.depthStencilAttachment = descriptor.depthStencilAttachment;
  if (descriptor.depthStencilAttachment.valid()) {
    MetalTextureView resolved;
    if (!isSuccess(resolveTextureView(descriptor.depthStencilAttachment, resolved))) {
      emitMessage(MessageSeverity::Error, "the framebuffer names a depth/stencil view that is not alive");
      return Result::InvalidHandle;
    }
  }

  *outFramebuffer = mFramebuffers.allocate(std::move(framebuffer));
  return outFramebuffer->valid() ? Result::Success : Result::OutOfMemory;
}

void MetalDevice::destroyFramebuffer(const FramebufferHandle framebuffer) {
  mFramebuffers.free(framebuffer);
}

// ==================================================================================================
// Queries
// ==================================================================================================

Result MetalDevice::createQuerySet(const QuerySetDescriptor& descriptor, QuerySetHandle* outQuerySet) {
  if (outQuerySet == nullptr) return Result::InvalidArgument;
  *outQuerySet = QuerySetHandle::invalid();
  if (descriptor.count == 0) return Result::InvalidArgument;

  MetalQuerySet querySet;
  querySet.type = descriptor.type;
  querySet.count = descriptor.count;

  switch (descriptor.type) {
    case QueryType::Occlusion: {
      // Metal's visibility result buffer is one 64-bit counter per query, written by a render encoder.
      querySet.visibilityBuffer = [mDevice newBufferWithLength:descriptor.count * sizeof(uint64_t)
                                                       options:MTLResourceStorageModeShared];
      if (querySet.visibilityBuffer == nil) return Result::OutOfMemory;
      std::memset(querySet.visibilityBuffer.contents, 0, descriptor.count * sizeof(uint64_t));
      if (descriptor.label != nullptr) {
        querySet.visibilityBuffer.label = [NSString stringWithUTF8String:descriptor.label];
      }
      break;
    }
    case QueryType::Timestamp: {
      if (!mCapabilities.supports(Feature::TimestampQuery) || mDevice.counterSets.count == 0) {
        emitMessage(MessageSeverity::Error, "this Metal device has no counter sample buffer support for timestamps");
        return Result::Unsupported;
      }
      MTLCounterSampleBufferDescriptor* sampleDescriptor = [MTLCounterSampleBufferDescriptor new];
      sampleDescriptor.counterSet = mDevice.counterSets.firstObject;
      sampleDescriptor.storageMode = MTLStorageModeShared;
      sampleDescriptor.sampleCount = descriptor.count;
      NSError* error = nil;
      querySet.counterSampleBuffer = [mDevice newCounterSampleBufferWithDescriptor:sampleDescriptor error:&error];
      if (querySet.counterSampleBuffer == nil) {
        emitMessage(MessageSeverity::Error,
                std::string("the Metal device could not create a counter sample buffer: ") + describe(error));
        return Result::Unsupported;
      }
      break;
    }
    case QueryType::PipelineStatistics:
      emitMessage(MessageSeverity::Error, "pipeline statistics queries are not implemented by the Metal backend");
      return Result::Unsupported;
  }

  *outQuerySet = mQuerySets.allocate(std::move(querySet));
  return outQuerySet->valid() ? Result::Success : Result::OutOfMemory;
}

void MetalDevice::destroyQuerySet(const QuerySetHandle querySetHandle) {
  if (MetalQuerySet* querySet = mQuerySets.get(querySetHandle); querySet != nullptr) {
    querySet->counterSampleBuffer = nil;
    querySet->visibilityBuffer = nil;
    mQuerySets.free(querySetHandle);
  }
}

Result MetalDevice::getQuerySetCount(const QuerySetHandle querySetHandle, uint32_t* outCount) const {
  if (outCount == nullptr) return Result::InvalidArgument;
  const MetalQuerySet* querySet = mQuerySets.get(querySetHandle);
  if (querySet == nullptr) return Result::InvalidHandle;
  *outCount = querySet->count;
  return Result::Success;
}

Result MetalDevice::getQueryResults(const QuerySetHandle querySetHandle, const uint32_t firstQuery,
        const uint32_t count, void* outData, const size_t outDataSize) {
  if (outData == nullptr) return Result::InvalidArgument;
  MetalQuerySet* querySet = mQuerySets.get(querySetHandle);
  if (querySet == nullptr) return Result::InvalidHandle;
  if (firstQuery + count > querySet->count) return Result::InvalidArgument;
  if (outDataSize < static_cast<size_t>(count) * sizeof(uint64_t)) return Result::InvalidArgument;

  if (querySet->type == QueryType::Timestamp) {
    if (querySet->counterSampleBuffer == nil) return Result::Unsupported;
    NSData* resolved = [querySet->counterSampleBuffer resolveCounterRange:NSMakeRange(firstQuery, count)];
    if (resolved == nil) return Result::NotReady;
    const size_t bytes = std::min<size_t>(outDataSize, resolved.length);
    std::memcpy(outData, resolved.bytes, bytes);
    return bytes == static_cast<size_t>(count) * sizeof(uint64_t) ? Result::Success : Result::NotReady;
  }

  if (querySet->visibilityBuffer == nil) return Result::Unsupported;
  std::memcpy(outData, static_cast<const uint8_t*>(querySet->visibilityBuffer.contents) + firstQuery * sizeof(uint64_t),
          count * sizeof(uint64_t));
  return Result::Success;
}

bool MetalDevice::isQueryResultAvailable(const QuerySetHandle querySetHandle, const uint32_t query) const {
  const MetalQuerySet* querySet = mQuerySets.get(querySetHandle);
  if (querySet == nullptr || query >= querySet->count) return false;
  // Metal has no per-query availability signal. The results are read from a buffer the GPU has written by the time the
  // submission that recorded the query completed, and the RHI requires the caller to have observed that completion
  // before asking; reporting "available" is the same answer the OpenGL backend gives.
  return true;
}

// ==================================================================================================
// Native interop
// ==================================================================================================

NativeTexture MetalDevice::getNativeTexture(const TextureHandle textureHandle) const {
  NativeTexture native;
  if (const MetalTexture* texture = mTextures.get(textureHandle); texture != nullptr) {
    native.object = (__bridge void*) texture->texture;
    native.context = (__bridge void*) mDevice;
  }
  return native;
}

NativeBuffer MetalDevice::getNativeBuffer(const BufferHandle bufferHandle) const {
  NativeBuffer native;
  if (const MetalBuffer* buffer = mBuffers.get(bufferHandle); buffer != nullptr) {
    native.object = (__bridge void*) buffer->buffer;
    native.context = (__bridge void*) mDevice;
  }
  return native;
}

Result MetalDevice::importNativeTexture(const NativeTextureDescriptor& descriptor, TextureHandle* outTexture) {
  if (outTexture == nullptr) return Result::InvalidArgument;
  *outTexture = TextureHandle::invalid();

  if (descriptor.kind != NativeTextureKind::IOSurface) {
    // The RHI's other importable kinds are a GL name, a Direct3D 11 shared handle and an AHardwareBuffer, none of
    // which exists on an Apple platform. Answering honestly is what lets a caller fall back to an upload instead of
    // building a descriptor that can only fail.
    emitMessage(MessageSeverity::Warning,
            "the Metal backend imports IOSurface only; the requested NativeTextureKind is not available here");
    return Result::Unsupported;
  }
  if (descriptor.platformHandle == nullptr) return Result::InvalidArgument;

  IOSurfaceRef surface = static_cast<IOSurfaceRef>(descriptor.platformHandle);
  const MTLPixelFormat pixelFormat = toMtlPixelFormat(descriptor.format);
  if (pixelFormat == MTLPixelFormatInvalid) {
    emitMessage(MessageSeverity::Error,
            "the Metal backend cannot import an IOSurface in the requested pixel format (supported: RGBA8, BGRA8, "
            "RGBA16F, R8, R8G8)");
    return Result::Unsupported;
  }

  // A bi-planar surface exposes the luma and chroma planes as separate extents. Plane 0 is the full
  // image; a later plane has its own (usually half-resolution) width and height, and Metal's
  // `newTextureWithDescriptor:iosurface:plane:` wants the descriptor to name that plane's extent.
  const uint32_t plane = descriptor.plane;
  const uint32_t surfaceWidth = plane == 0
          ? static_cast<uint32_t>(IOSurfaceGetWidth(surface))
          : static_cast<uint32_t>(IOSurfaceGetWidthOfPlane(surface, plane));
  const uint32_t surfaceHeight = plane == 0
          ? static_cast<uint32_t>(IOSurfaceGetHeight(surface))
          : static_cast<uint32_t>(IOSurfaceGetHeightOfPlane(surface, plane));
  if (descriptor.width != 0 && descriptor.width != surfaceWidth) {
    emitMessage(MessageSeverity::Error,
            "the IOSurface plane is " + std::to_string(surfaceWidth) + " pixels wide but the descriptor says " +
                    std::to_string(descriptor.width));
    return Result::InvalidArgument;
  }
  if (descriptor.height != 0 && descriptor.height != surfaceHeight) {
    emitMessage(MessageSeverity::Error,
            "the IOSurface plane is " + std::to_string(surfaceHeight) + " pixels high but the descriptor says " +
                    std::to_string(descriptor.height));
    return Result::InvalidArgument;
  }
  if (surfaceWidth == 0 || surfaceHeight == 0) return Result::InvalidArgument;

  MTLTextureDescriptor* native = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixelFormat
                                                                                   width:surfaceWidth
                                                                                  height:surfaceHeight
                                                                               mipmapped:NO];
  // Sampling is the only usage the RHI promises for an imported texture. A writeback import is filled by a blit
  // encoder, which needs no usage flag, so asking for `ShaderWrite` would only narrow the set of surfaces the
  // driver accepts without enabling anything this path uses.
  native.usage = MTLTextureUsageShaderRead;
  // An IOSurface is host-visible memory owned by the producer, which is what makes the import zero-copy.
  native.storageMode = MTLStorageModeShared;

  id<MTLTexture> texture = [mDevice newTextureWithDescriptor:native
                                                    iosurface:surface
                                                        plane:plane];
  if (texture == nil) {
    emitMessage(MessageSeverity::Error,
            "the Metal device could not wrap the IOSurface (" + std::to_string(surfaceWidth) + "x" +
                    std::to_string(surfaceHeight) + ", " + getPixelFormatInfo(descriptor.format).name +
                    ") as a texture; the driver rejects that pixel format, plane or row alignment");
    return Result::Unsupported;
  }

  MetalTexture payload;
  payload.texture = texture;
  payload.type = MTLTextureType2D;
  payload.format = descriptor.format;
  payload.size = { surfaceWidth, surfaceHeight, 1 };
  payload.mipLevelCount = 1;
  payload.sampleCount = 1;
  TextureUsages importedUsage = TextureUsage::Sampled | TextureUsage::External;
  if (descriptor.writeback) importedUsage = importedUsage | TextureUsage::Storage;
  payload.usage = static_cast<TextureUsages>(importedUsage);
  payload.bytes = getFormatByteSize(descriptor.format, surfaceWidth, surfaceHeight);
  // The texture object keeps a reference to the IOSurface, so the caller's own reference is not needed once the import
  // has returned; that is the ownership rule `NativeTextureDescriptor` documents for this kind.

  *outTexture = mTextures.allocate(std::move(payload));
  if (!outTexture->valid()) return Result::OutOfMemory;
  mResidentTextureBytes += getFormatByteSize(descriptor.format, surfaceWidth, surfaceHeight);
  return Result::Success;
}

bool MetalDevice::supportsNativeTextureImport(const NativeTextureKind kind) const {
  return kind == NativeTextureKind::IOSurface;
}

} // namespace backend::metal
