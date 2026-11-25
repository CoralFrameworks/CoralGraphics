/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and limitations under
 * the License.
 */

#include <gpu/Context.h>

#include <backend/Descriptors.h>
#include <backend/Device.h>
#include <gpu/Buffer.h>
#include <gpu/Shader.h>

#include "ContextState.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace graphics::gpu {
namespace {

/** Maps the facade's preference onto the backend's enum. */
backend::Backend toBackend(const ContextConfiguration::Preferred preferred) noexcept {
  switch (preferred) {
    case ContextConfiguration::Preferred::Undefined:
      // Null is the backend's "no preference": the factory then walks the platform's priority list.
      return backend::Backend::Null;
    case ContextConfiguration::Preferred::OpenGL:
      return backend::Backend::OpenGL;
    case ContextConfiguration::Preferred::Vulkan:
      return backend::Backend::Vulkan;
    case ContextConfiguration::Preferred::Metal:
      return backend::Backend::Metal;
  }
  return backend::Backend::Null;
}

} // namespace anonymous

Context::~Context() = default;

std::shared_ptr<Context> Context::create(const ContextConfiguration& configuration) {
  backend::DeviceConfiguration backendConfiguration;
  backendConfiguration.preferredBackend = toBackend(configuration.preferred);
  backendConfiguration.allowFallback = configuration.allowFallback;
  backendConfiguration.enableValidation = configuration.enableValidation;
  backendConfiguration.enableGpuTiming = configuration.enableGpuTiming;
  backendConfiguration.appName = configuration.applicationName != nullptr ? configuration.applicationName : "Coral";

  // A surface is only described when the caller supplied a window or an explicit size. A headless
  // context gets none, which is what makes "rendering to an offscreen target" the default rather than
  // something a caller has to opt into by leaving fields blank.
  backend::SurfaceDescriptor surface;
  if (configuration.nativeWindow != nullptr || configuration.width != 0 || configuration.height != 0) {
    surface.nativeWindow = configuration.nativeWindow;
    surface.nativeDisplay = configuration.nativeDisplay;
    surface.nativeWindowExtra = configuration.nativeWindowExtra;
    surface.nativeWindowSystem = configuration.nativeWindowSystem;
    surface.width = configuration.width;
    surface.height = configuration.height;
    surface.label = backendConfiguration.appName;
    backendConfiguration.surface = &surface;
  }

  auto context = std::make_shared<Context>();
  context->mState = std::make_shared<ContextState>();
  if (!backend::isSuccess(backend::createDevice(backendConfiguration, &context->mState->device))) {
    // The wrapper is dropped rather than returned half-built: `valid()` would be false anyway, but a
    // caller that forgot to check would then behave differently from one that did, which is how a
    // startup failure turns into a crash three frames later.
    return nullptr;
  }
  return context;
}

bool Context::valid() const noexcept { return mState != nullptr && mState->device != nullptr; }

backend::Device* Context::device() noexcept { return mState != nullptr ? mState->device : nullptr; }

const backend::Device* Context::device() const noexcept { return mState != nullptr ? mState->device : nullptr; }

std::string Context::describeDevice() const {
  const backend::Device* rhi = device();
  if (rhi == nullptr) return "no device";

  const backend::DeviceInfo& info = rhi->getInfo();
  std::string text = info.deviceName != nullptr ? info.deviceName : "";
  text += " (";
  text += info.vendor != nullptr ? info.vendor : "";
  text += ", ";
  text += info.apiVersion != nullptr ? info.apiVersion : "";
  text += ", ";
  text += info.backendName != nullptr ? info.backendName : "";
  text += ')';
  return text;
}

std::string Context::diagnostics() const {
  const backend::Device* rhi = device();
  if (rhi == nullptr) return "no device";

  std::string out;
  rhi->appendDiagnostics(out);
  return out;
}

bool Context::supportsCompute() const noexcept {
  const backend::Device* rhi = device();
  return rhi != nullptr && rhi->supports(backend::Features(backend::Feature::ComputeShader));
}

bool Context::supportsGeometryShaders() const noexcept {
  const backend::Device* rhi = device();
  return rhi != nullptr && rhi->supports(backend::Features(backend::Feature::GeometryShader));
}

bool Context::supportsWireframe() const noexcept {
  const backend::Device* rhi = device();
  return rhi != nullptr && rhi->supports(backend::Features(backend::Feature::FillModeNonSolid));
}

bool Context::supportsAnisotropicFiltering() const noexcept {
  const backend::Device* rhi = device();
  return rhi != nullptr && rhi->supports(backend::Features(backend::Feature::SamplerAnisotropy));
}

bool Context::supportsDirectGlsl() const noexcept {
  const backend::Device* rhi = device();
  // Read from the capability rather than from the backend name: a future backend whose driver compiles
  // GLSL reports it there, and this query keeps working without a second list to maintain.
  return rhi != nullptr && rhi->getCapabilities().acceptsGlslDirect;
}

// ==================================================================================================
// Buffers
// ==================================================================================================

std::shared_ptr<VertexBuffer> Context::createVertexBuffer(const size_t vertexCount, const size_t vertexStride,
        const void* data, const char* label) {
  if (device() == nullptr || vertexStride == 0) return nullptr;

  const size_t size = vertexCount * vertexStride;
  backend::BufferDescriptor descriptor = backend::BufferDescriptor::vertex(size, data);
  descriptor.label = label;
  if (data == nullptr) {
    // Created empty, so the caller most likely intends to fill it, possibly every frame. Device-local
    // memory would make that impossible, so the memory type follows the evident intent instead of
    // making the caller discover a rejection later.
    descriptor.memoryUsage = backend::MemoryUsage::CpuToGpuDynamic;
  }

  backend::BufferHandle handle = backend::BufferHandle::invalid();
  if (!backend::isSuccess(mState->device->createBuffer(descriptor, &handle))) return nullptr;

  return std::shared_ptr<VertexBuffer>(new VertexBuffer(state(), handle, size, vertexCount, vertexStride));
}

std::shared_ptr<IndexBuffer> Context::createIndexBuffer(const size_t indexCount, const IndexFormat format,
        const void* data, const char* label) {
  if (device() == nullptr) return nullptr;

  const size_t elementSize = format == IndexFormat::UInt16 ? sizeof(uint16_t) : sizeof(uint32_t);
  const size_t size = indexCount * elementSize;

  backend::BufferDescriptor descriptor = backend::BufferDescriptor::index(size, data);
  descriptor.label = label;
  if (data == nullptr) descriptor.memoryUsage = backend::MemoryUsage::CpuToGpuDynamic;

  backend::BufferHandle handle = backend::BufferHandle::invalid();
  if (!backend::isSuccess(mState->device->createBuffer(descriptor, &handle))) return nullptr;

  return std::shared_ptr<IndexBuffer>(new IndexBuffer(state(), handle, size, indexCount, format));
}

std::shared_ptr<UniformBuffer> Context::createUniformBuffer(const size_t size, const char* label) {
  if (device() == nullptr || size == 0) return nullptr;

  backend::BufferDescriptor descriptor = backend::BufferDescriptor::uniform(size);
  descriptor.label = label;

  backend::BufferHandle handle = backend::BufferHandle::invalid();
  if (!backend::isSuccess(mState->device->createBuffer(descriptor, &handle))) return nullptr;

  return std::shared_ptr<UniformBuffer>(new UniformBuffer(state(), handle, size));
}

// ==================================================================================================
// Shaders
// ==================================================================================================

std::shared_ptr<Shader> Context::createShader(const uint32_t* code, const size_t wordCount,
        const backend::ShaderStage stage) {
  if (device() == nullptr || code == nullptr || wordCount == 0) {
    return nullptr;
  }

  backend::ShaderModuleDescriptor descriptor;
  descriptor.stage = stage;
  descriptor.language = backend::ShaderModuleDescriptor::Language::SPIRV;
  descriptor.code = code;
  descriptor.codeSize = wordCount * sizeof(uint32_t);
  descriptor.label = "gpu::Shader";

  backend::ShaderModuleHandle handle = backend::ShaderModuleHandle::invalid();
  if (!backend::isSuccess(mState->device->createShaderModule(descriptor, &handle))) return nullptr;

  // The reflection is copied into the facade's own vocabulary so that reading a shader's bindings does
  // not require a private header. The copy also outlives the module if the caller keeps the shader and
  // drops the device handle, which is the more forgiving behaviour for no real cost.
  std::vector<Shader::Binding> bindings;
  std::vector<backend::ShaderBindingDeclaration> declarations;
  if (backend::isSuccess(mState->device->getShaderReflection(handle, &declarations))) {
    bindings.reserve(declarations.size());
    for (const backend::ShaderBindingDeclaration& declaration: declarations) {
      Shader::Binding binding;
      binding.set = declaration.set;
      binding.binding = declaration.binding;
      binding.arraySize = declaration.arraySize;
      // Always `kNoShaderLocation` on this path - a SPIR-V module has no such decoration - but read
      // through rather than assumed, so the two reflection copies cannot drift apart.
      binding.location = declaration.location;
      binding.name = declaration.name != nullptr ? declaration.name : "";
      bindings.push_back(std::move(binding));
    }
  }

  // The words are the caller's and stay the caller's: this overload exists precisely because a module
  // that is already SPIR-V needs no compiling, so the facade does not copy the blob back out.
  return std::shared_ptr<Shader>(new Shader(state(), handle, stage, std::string{}, std::vector<uint32_t>{},
          backend::ShaderSourceInfo{}, backend::ShaderCacheKey{}, std::move(bindings)));
}

namespace {

/** FNV-1a over a byte range, matching the hash the pipeline cache uses. */
uint64_t hashBytes(const void* data, const size_t size) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  uint64_t hash = 1469598103934665603ull;
  for (size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= 1099511628211ull;
  }
  return hash;
}

/** FNV-1a over a string, for values that are not POD. */
uint64_t hashString(const char* text) noexcept {
  return text != nullptr ? hashBytes(text, std::strlen(text)) : 0;
}

/** The running compiler's identity, so a cache key changes when the compiler does. */
uint64_t compilerHash() noexcept {
  static const uint64_t hash = hashString(backend::getShaderCompilerVersion());
  return hash;
}

} // namespace anonymous

std::shared_ptr<Shader> Context::createShader(const ShaderSource& source) {
  if (device() == nullptr) {
    return nullptr;
  }
  if (source.source.empty()) {
    // A null return rather than a log line: the backend reports everything it can diagnose, and an
    // empty string here is the caller passing nothing rather than a compiler refusing something.
    return nullptr;
  }

  // The mapping tables are translated into the backend's vocabulary here and live only for the length
  // of the `createShaderModule` call, which is all the backend promises to read them for. The names the
  // declarations point at belong to `source`, which outlives the call by construction: the caller owns
  // it and passed it by const reference.
  std::vector<backend::ShaderBindingDeclaration> declarations;
  declarations.reserve(source.bindings.size());
  for (const ShaderBindingMapping& mapping: source.bindings) {
    backend::ShaderBindingDeclaration declaration;
    declaration.set = mapping.set;
    declaration.binding = mapping.binding;
    declaration.type = mapping.type;
    declaration.arraySize = mapping.arraySize;
    declaration.location = mapping.location;
    declaration.name = mapping.name.c_str();
    declarations.push_back(declaration);
  }

  std::vector<backend::ShaderAttributeDeclaration> attributeDeclarations;
  attributeDeclarations.reserve(source.attributes.size());
  for (const ShaderAttributeMapping& mapping: source.attributes) {
    backend::ShaderAttributeDeclaration declaration;
    declaration.location = mapping.location;
    declaration.name = mapping.name.c_str();
    attributeDeclarations.push_back(declaration);
  }

  backend::ShaderModuleDescriptor descriptor;
  descriptor.stage = source.stage;
  descriptor.language = source.directGlsl ? backend::ShaderModuleDescriptor::Language::GLSLDirect
                                          : backend::ShaderModuleDescriptor::Language::GLSL;
  descriptor.code = source.source.c_str();
  // For a source module `codeSize` includes the NUL terminator, which is how the backend tells a
  // source string apart from a SPIR-V blob.
  descriptor.codeSize = source.source.size() + 1;
  descriptor.entryPoint = source.entryPoint;
  descriptor.sourceLanguage = source.language;
  descriptor.optimizeSource = source.optimize;
  descriptor.label = source.label != nullptr ? source.label : "gpu::Shader";
  if (!declarations.empty()) {
    descriptor.bindingDeclarations = declarations.data();
    descriptor.bindingDeclarationCount = static_cast<uint32_t>(declarations.size());
  }
  if (!attributeDeclarations.empty()) {
    descriptor.attributeDeclarations = attributeDeclarations.data();
    descriptor.attributeDeclarationCount = static_cast<uint32_t>(attributeDeclarations.size());
  }

  backend::ShaderModuleHandle handle = backend::ShaderModuleHandle::invalid();
  if (!backend::isSuccess(mState->device->createShaderModule(descriptor, &handle))) {
    // The backend has already reported the compiler's own diagnostics, which name the offending line of
    // the source that was passed in. There is nothing useful left to add.
    return nullptr;
  }

  std::vector<Shader::Binding> bindings;
  std::vector<backend::ShaderBindingDeclaration> reflected;
  if (backend::isSuccess(mState->device->getShaderReflection(handle, &reflected))) {
    bindings.reserve(reflected.size());
    for (const backend::ShaderBindingDeclaration& declaration: reflected) {
      Shader::Binding binding;
      binding.set = declaration.set;
      binding.binding = declaration.binding;
      binding.arraySize = declaration.arraySize;
      binding.location = declaration.location;
      binding.name = declaration.name != nullptr ? declaration.name : "";
      bindings.push_back(std::move(binding));
    }
  }

  backend::ShaderSourceInfo info;
  if (!backend::isSuccess(mState->device->getShaderSourceInfo(handle, &info))) {
    // Should not happen for a module that was just compiled from source here. If it does, the shader is
    // still perfectly usable and only its provenance is missing, so it is not worth failing over.
    info = backend::ShaderSourceInfo{};
  }

  // Copied out of the backend rather than referenced, because the reason to keep the words at all is
  // that a caller can write them to a cache; a pointer into a module that dies with the context would
  // be useless for exactly that.
  std::vector<uint32_t> spirv;
  if (!backend::isSuccess(mState->device->getShaderSpirv(handle, &spirv))) spirv.clear();

  backend::ShaderCacheKey cacheKey;
  if (!info.directGlsl) {
    // The cache key folds in the target the source was compiled for, because the same source compiled for
    // ES and for desktop core produces different SPIR-V and must not share a cache entry. The dialect is
    // read back from the device's own reported API version rather than assumed, so a device that came up
    // in an unexpected dialect cannot poison a cache built for another one.
    //
    // A direct module gets no key at all rather than one derived from its source: the driver compiles
    // that text every time this device asks for it, and there is no compiled form a cache could hold.
    // `Shader::getSourceHash()` still answers "is this the same source as before", which is the question
    // a hot-reload path actually has.
    const backend::DeviceInfo& deviceInfo = mState->device->getInfo();
    const uint64_t dialectHash = hashString(deviceInfo.apiVersion);

    cacheKey.sourceHash = info.sourceHash;
    cacheKey.stage = static_cast<uint32_t>(source.stage);
    cacheKey.dialectHash = dialectHash;
    cacheKey.compilerHash = compilerHash();

    {
      // One mixing pass over the fields, so that the combined key changes whenever any of them does.
      uint64_t value = 1469598103934665603ull;
      const uint64_t parts[] = { cacheKey.sourceHash, cacheKey.dialectHash, cacheKey.compilerHash,
        static_cast<uint64_t>(cacheKey.stage) };
      value = hashBytes(parts, sizeof(parts));
      cacheKey.value = value;
    }
  }

  return std::shared_ptr<Shader>(new Shader(state(), handle, source.stage, source.source, std::move(spirv), info,
          cacheKey, std::move(bindings)));
}

Result Context::waitIdle() const {
  if (mState == nullptr || mState->device == nullptr) {
    return Result::DeviceLost;
  }
  return mState->device->waitIdle();
}

} // namespace graphics::gpu
