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

#ifndef CORAL_BACKEND_DESCRIPTORS_H
#define CORAL_BACKEND_DESCRIPTORS_H

#include <backend/Handles.h>
#include <backend/Types.h>

#include <cstdint>
#include <optional>

namespace backend {

/** Upper bound on descriptor bindings in a single set; fixed so descriptors stay POD. */
inline constexpr uint32_t kMaxDescriptorBindings = 16;
/** Upper bound on vertex buffer layouts in a single pipeline. */
inline constexpr uint32_t kMaxVertexBufferLayouts = 8;
/** Upper bound on vertex attributes in a single pipeline. */
inline constexpr uint32_t kMaxVertexAttributes = 16;
/** Upper bound on colour attachments in a single render pass / pipeline. */
inline constexpr uint32_t kMaxColorAttachments = 8;
/** Upper bound on descriptor set layouts in a pipeline layout. */
inline constexpr uint32_t kMaxDescriptorSetLayouts = 4;

// ==================================================================================================
// Shader modules
// ==================================================================================================

struct ShaderSourceInfo {
  /** The language the source was compiled as. */
  ShaderSourceLanguage language = ShaderSourceLanguage::ESSL100;
  /** The `#version` value from the source, or 100 when the source declared none. */
  uint32_t version = 100;
  /** True when @c version came from a `#version` directive rather than from the default. */
  bool versionWasDeclared = false;

  /**
   * FNV-1a hash of the source text, or of the supplied SPIR-V bytes for a `spirvInput` module.
   *
   * The cheap answer to "did this shader change". A build step can compare it against the hash stored
   * next to a cached SPIR-V blob and recompile only when it differs, which matters most on Android and
   * iOS where shader compilation happens on the device and is slow enough to be worth avoiding.
   */
  uint64_t sourceHash = 0;

  /** Bytes of source (excluding the NUL terminator), or of the supplied SPIR-V for a binary module. */
  size_t sourceSize = 0;

  /**
   * The `#version` the module was actually compiled at.
   *
   * Not necessarily `version`. glslang refuses to emit SPIR-V below GLSL ES 3.10 or desktop GLSL
   * 3.30, so an ESSL 3.00 source is retargeted upward and compiled at 310. Both numbers travel
   * together so that a caller can see the retarget rather than having to infer it from a shader that
   * behaves like a newer language than it claims to be.
   */
  uint32_t compiledVersion = 0;

  /** True when @c compiledVersion differs from @c version, i.e. the source was retargeted. */
  bool versionWasRetargeted = false;

  /** Whether the SPIR-V was run through the optimiser. Part of what a cache key must cover. */
  bool optimized = false;

  /**
   * True when the driver compiled this source as written, with no SPIR-V in between.
   *
   * The provenance of a `ShaderModuleDescriptor::Language::GLSLDirect` module, and the flag that tells
   * it apart from one that was compiled to SPIR-V here: `compiledVersion` is filled in for both, but
   * only the direct module's is a version no Coral compiler chose. `Device::getShaderSpirv` reports
   * `Result::Unsupported` for such a module, because there are no words to hand back - which is what a
   * caller asking "can I cache this" needs to know.
   */
  bool directGlsl = false;

  /**
   * True when the module was supplied as SPIR-V words rather than compiled from source here.
   *
   * For such a module `language`/`version` have no source to describe and are left at
   * `ShaderSourceLanguage::Detect`/0; `sourceSize` is the supplied byte count and `sourceHash` is the
   * FNV-1a hash of those bytes. `getShaderSpirv` returns the words the caller supplied in this case.
   */
  bool spirvInput = false;
};

/**
 * The key a compiled shader can be cached under.
 *
 * Deliberately not the same thing as `ShaderSourceInfo::sourceHash`. That hash answers "is this the
 * same source"; this key answers "is this the same compiled result", which additionally depends on the
 * target dialect and the compiler that produced it. A cache keyed only on the source would serve
 * desktop GLSL to an ES context, or stale output after a shader-compiler upgrade.
 *
 * Fold `sourceHash`, `stage`, `dialectHash` and `compilerHash` into `value` and use that single number
 * as the cache entry name; the individual fields are kept so that a cache miss can say *why*.
 */
struct ShaderCacheKey {
  uint64_t sourceHash = 0;
  /** Which stage was compiled. The same source text is a different module per stage. */
  uint32_t stage = 0;
  /**
   * Hash of the target the source was compiled for: the GL dialect, its version, and the SPIR-V
   * version emitted. Two builds targeting different dialects must not share an entry.
   */
  uint64_t dialectHash = 0;
  /**
   * Hash of the compiler that produced the result. A shaderc or glslang upgrade can change the SPIR-V
   * for identical input, so a cache that ignores it can serve output the current compiler would not
   * have produced.
   */
  uint64_t compilerHash = 0;

  /** The combined key to name a cache entry with. */
  uint64_t value = 0;

  /** @return true when this key identifies something. A zero value means "not cacheable". */
  [[nodiscard]] bool valid() const noexcept { return value != 0; }
};

/** What a descriptor binding points at. Declared before shader modules, which reference it. */
enum class BindingType : uint8_t {
  /** Combined image + sampler, the overwhelmingly common case. */
  TextureSampler,
  /** A sampler with no texture, for texture-less comparison sampling. */
  Sampler,
  /** A sampled/storage image with a separate sampler. */
  Texture,
  /** A uniform buffer. On OpenGL 3.0 this maps to the default uniform block. */
  UniformBuffer,
  /** A read-only storage buffer. Requires Capabilities::storageBuffer. */
  ReadOnlyStorageBuffer,
  /** A read/write storage buffer. Requires Capabilities::storageBuffer. */
  StorageBuffer,
};

/**
 * "No explicit location": the backend assigns this binding a slot itself.
 *
 * The value a declaration carries when the caller has no opinion about where the binding lands, and
 * what `ShaderBindingDeclaration::location` means when it is left alone. A real location is never
 * negative, so a signed field can carry both the answer and its absence in one number - which is what
 * lets a caller deserialise a manifest that simply omits the field.
 */
inline constexpr int32_t kNoShaderLocation = -1;

/**
 * One entry of a ShaderModuleDescriptor::bindingDeclarations table, used when shaders are supplied as
 * source rather than SPIR-V. `name` must be the GLSL uniform name exactly as it appears in the
 * source, which is what the OpenGL backend passes to glGetUniformLocation.
 */
struct ShaderBindingDeclaration {
  uint32_t set = 0;
  uint32_t binding = 0;
  BindingType type = BindingType::TextureSampler;
  uint32_t arraySize = 1;
  const char* name = nullptr;

  /**
   * The GL-side slot this binding must occupy, or `kNoShaderLocation` to let the backend pick one.
   *
   * What a location *is* depends on the binding, because OpenGL has three different namespaces that a
   * portable descriptor model flattens into one:
   *
   *  - `TextureSampler` / `Texture`: the texture unit (`GL_TEXTURE0 + location`).
   *  - `UniformBuffer` / `ReadOnlyStorageBuffer` / `StorageBuffer`: the uniform block binding, i.e.
   *    the index `glBindBufferBase(GL_UNIFORM_BUFFER, index, ...)` and `glUniformBlockBinding` agree on.
   *  - `Sampler`: unused; a texture-less sampler has no OpenGL equivalent at all.
   *
   * Left at `kNoShaderLocation`, a sampler is given the next free texture unit counting from zero and a
   * block is bound to its descriptor `binding` number - the behaviour every shader had before this
   * field existed, and what a caller who does not care about the numbering wants.
   *
   * Only the OpenGL backend reads it. Everywhere else the SPIR-V decorations are authoritative and a
   * caller-supplied location is ignored rather than obeyed, because a descriptor set layout on Vulkan
   * is formed at pipeline creation and cannot be rewritten by a shader.
   */
  int32_t location = kNoShaderLocation;
};

/**
 * One entry of a ShaderModuleDescriptor::attributeDeclarations table: a vertex input named so that its
 * location can be pinned before the program is linked.
 *
 * ### Why this exists
 *
 * On the SPIR-V path a vertex input's location travels inside the module as a `Location` decoration,
 * which SPIRV-Cross turns back into `layout(location = N) in ...`. Hand-written GLSL for an OpenGL
 * context has no such guarantee: `layout(location)` is GLSL ES 3.00 and desktop GLSL 3.30, so an
 * ESSL 1.00 or `#version 120` source cannot state one, and a source written for GLSL ES 3.00 may
 * simply not bother. What is left is `glBindAttribLocation`, which binds *by name* - so the name and
 * the location have to come from somewhere, and with no SPIR-V to reflect there is nowhere else.
 *
 * A manifest that ships alongside a shader package carries exactly this pair, which is the usual
 * source of the table: `{"name": "aPosition", "location": 0}`.
 */
struct ShaderAttributeDeclaration {
  /** The attribute location the pipeline declares, i.e. `VertexAttribute::shaderLocation`. */
  uint32_t location = 0;
  /** The vertex input's name, exactly as the source spells it: `aPosition`, not `in aPosition`. */
  const char* name = nullptr;
};

/**
 * Description of a shader module.
 *
 * The RHI's canonical shader input is SPIR-V. This is the single most important decision for
 * keeping the abstraction honest across backends:
 *
 *  - Vulkan consumes the module byte for byte, with zero conversion cost and no reflection work.
 *  - Metal receives SPIR-V translated to MSL by SPIRV-Cross.
 *  - Metal will receive SPIR-V translated to MSL through the same pass.
 *  - OpenGL ES 3.0 receives SPIR-V translated down to GLSL ES 3.00 by the same SPIRV-Cross pass.
 *
 * Without a common IR each backend would need its own shader authoring path, which is precisely how
 * portability layers rot. `Capabilities::acceptsSpirv` reports whether a device can take SPIR-V
 * directly; when it is false the caller can supply pre-translated source through `sourceLanguage`.
 */
struct ShaderModuleDescriptor {
  enum class Language : uint8_t {
    /** `code` / `codeSize` point at SPIR-V words. The default and preferred input. */
    SPIRV,
    /**
     * `code` / `codeSize` point at GLSL source, which the backend compiles to SPIR-V before doing
     * anything else with it.
     *
     * Compiling through SPIR-V rather than handing the source straight to the driver is what makes a
     * GLSL module a first-class citizen: the module gets real reflection, the same dialect checking as
     * a SPIR-V module, and the same `#version` translation. The cost is one shader compilation at load
     * time, which `ShaderCacheKey` exists to let a caller avoid on subsequent runs.
     */
    GLSL,
    /**
     * `code` / `codeSize` point at GLSL source that goes straight to the driver: no SPIR-V, no
     * SPIRV-Cross, no shaderc, no retargeting. The text the caller supplies is the text the driver
     * compiles, byte for byte.
     *
     * ### Who accepts it
     *
     * Only a backend whose own shading language *is* GLSL - the OpenGL backend. Every other backend
     * reports `Result::Unsupported`, and `Capabilities::acceptsGlslDirect` says so up front so a
     * caller can choose between this and `Language::GLSL` before it calls.
     *
     * ### What the caller has to supply instead
     *
     * Reflection is the thing this path gives up: there is no SPIR-V to read, so a binding's set,
     * binding, type and name cannot be recovered from the module. `bindingDeclarations` therefore
     * *is* the module's binding table rather than an optional correction to it, and
     * `attributeDeclarations` is the only way a vertex input's location can be pinned. Both are
     * normally read out of a manifest that ships with the shader. `getShaderReflection` reports the
     * supplied table, so a renderer still builds its descriptor sets from what the shader object says.
     *
     * ### What the source has to be
     *
     * Written for the context's own dialect: `#version 300 es` (or 100) for an OpenGL ES context,
     * `#version 410 core` for a desktop one. There is no translation step left to paper over the
     * difference, which is the whole point - and the reason the driver's own error log, quoted back
     * through the device's message callback, names lines of the caller's file rather than of a
     * generated one.
     *
     * `sourceVersion` and `optimizeSource` do not apply: nothing here synthesises a preamble and
     * nothing optimises the module. `sourceLanguage` is used only to label the module when the source
     * cannot say for itself, since the driver is what decides how the text is parsed.
     */
    GLSLDirect,
  };

  /** Shader stage this module is compiled for. Required. */
  ShaderStage stage = ShaderStage::Vertex;
  Language language = Language::SPIRV;

  /**
   * Shader bytecode for SPIRV, or a NUL-terminated UTF-8 string for `Language::GLSL`.
   * The memory does not need to outlive the createShaderModule call.
   */
  const void* code = nullptr;
  /** Size of `code` in bytes. For source languages this includes the NUL terminator. */
  size_t codeSize = 0;

  /**
   * Entry point name. When empty, "main" is assumed. Vulkan requires this to be spelled out, which
   * is why it is part of the descriptor rather than a compile-time assumption.
   */
  const char* entryPoint = "main";

  /**
   * Which language profile to compile `Language::GLSL` source as.
   *
   * `Detect` is normally right: the source's own `#version` directive says which profile it is, and a
   * source without one is ESSL 1.00. Set it explicitly only when the source cannot say - a snippet
   * with no `#version` that uses 3.00 syntax, for instance.
   */
  ShaderSourceLanguage sourceLanguage = ShaderSourceLanguage::Detect;

  /**
   * Optimisation to apply when compiling GLSL to SPIR-V.
   *
   * Off by default, because a debugger is the main reason to feed this path GLSL at all and an
   * optimised module does not resemble the source. A shipping build wants
   * `ShaderCompileOptions::optimize` set.
   */
  bool optimizeSource = false;

  /**
   * The `#version` preamble to synthesise for a hand-written GLSL module that wants one added or
   * replaced. 0 leaves the source's own directive alone.
   *
   * Superseded by `sourceLanguage` for the common case and kept for the narrow one: a source
   * fragment with no `#version` at all, which the caller wants compiled as something other than
   * ESSL 1.00 without writing the directive itself.
   */
  uint32_t sourceVersion = 0;

  /**
   * Debug name, surfaced in driver debug output and in SPIRV-Cross diagnostics. Optional.
   */
  const char* label = nullptr;

  /**
   * Optional explicit binding table, used for the source languages (`Language::GLSL` and
   * `Language::GLSLDirect`).
   *
   * For `Language::GLSL` a source is compiled to SPIR-V first, so its `layout(set = N, binding = M)`
   * decorations are recovered by reflection exactly as they are for a SPIR-V module, and this table is
   * normally unnecessary. It remains for source that cannot carry those decorations - ESSL 1.00 has no
   * layout qualifiers at all - where the caller has to state the mapping for the backend.
   *
   * For `Language::GLSLDirect` there is no reflection to fall back on, so this table is the module's
   * binding table rather than a correction to one. A direct module with no declarations has no
   * bindings, which is right for a shader that samples nothing and reads no block.
   *
   * Deliberately an escape hatch rather than the normal path: a renderer that finds itself filling
   * this in for every shader has given up the portability the RHI exists to provide.
   */
  const ShaderBindingDeclaration* bindingDeclarations = nullptr;
  uint32_t bindingDeclarationCount = 0;

  /**
   * Optional vertex-input table, used with `Language::GLSLDirect`.
   *
   * Each entry names a vertex input and the location it must occupy, and the OpenGL backend applies
   * them with `glBindAttribLocation` before linking - which is the only way a source without
   * `layout(location = ...)` can be made to agree with the pipeline's `VertexAttribute::shaderLocation`.
   * See `ShaderAttributeDeclaration`. Empty is the common case for `Language::SPIRV` and
   * `Language::GLSL`, whose locations travel inside the module - and a table supplied for one of those
   * is ignored rather than obeyed, with a warning, because a `layout(location = ...)` in the module
   * takes precedence over `glBindAttribLocation` and honouring both would put an input where nothing
   * reads it.
   */
  const ShaderAttributeDeclaration* attributeDeclarations = nullptr;
  uint32_t attributeDeclarationCount = 0;
};

// ==================================================================================================
// Textures
// ==================================================================================================

struct TextureDescriptor {
  TextureType type = TextureType::Texture2D;
  PixelFormat format = PixelFormat::Undefined;
  Extent3D size = { .width = 1, .height = 1, .depthOrArrayLayers = 1 };

  /** Mip levels including the base level. 1 disables mipmapping. */
  uint32_t mipLevelCount = 1;
  /** MSAA sample count. 1 disables multisampling. */
  uint32_t sampleCount = 1;
  /** Bitmask of TextureUsage values. */
  TextureUsages usage = 0;

  /** Optional debug name. */
  const char* label = nullptr;

  /** Convenience constructor for the common 2D case. */
  static TextureDescriptor texture2D(const uint32_t width, const uint32_t height, const PixelFormat format,
          const TextureUsages usage = static_cast<TextureUsages>(TextureUsage::Sampled)) {
    TextureDescriptor descriptor;
    descriptor.type = TextureType::Texture2D;
    descriptor.format = format;
    descriptor.size = { .width = width, .height = height, .depthOrArrayLayers = 1 };
    descriptor.usage = usage;
    return descriptor;
  }
};

/**
 * A reinterpretation of an existing texture's storage. Creating a view is free on Vulkan and Metal;
 * on OpenGL ES it is emulated by carrying the base texture plus format/range overrides into
 * glTexParameteri and the sampler state, so the abstraction stays uniform.
 */
struct TextureViewDescriptor {
  TextureHandle texture = TextureHandle::invalid();
  TextureViewDimension dimension = TextureViewDimension::Texture2D;
  /** Format to reinterpret as, or Undefined to inherit the texture's own format. */
  PixelFormat format = PixelFormat::Undefined;
  TextureSubresourceRange range = TextureSubresourceRange::all();
  const char* label = nullptr;

  /**
   * Set when @p texture was imported with an external (YUV) format: a view of such an image cannot be
   * created without the conversion, so a backend that supports them requires it here.
   */
  std::optional<YcbcrConversionDescriptor> ycbcr;
};

/** Fully describes how a texture is sampled. Immutable once created, so backends can cache it. */
struct SamplerDescriptor {
  FilterMode magFilter = FilterMode::Linear;
  FilterMode minFilter = FilterMode::Linear;
  MipmapFilterMode mipmapFilter = MipmapFilterMode::Linear;

  SamplerAddressMode addressModeU = SamplerAddressMode::ClampToEdge;
  SamplerAddressMode addressModeV = SamplerAddressMode::ClampToEdge;
  SamplerAddressMode addressModeW = SamplerAddressMode::ClampToEdge;

  /** Anisotropic filtering; 1.0 disables it. Clamped to Capabilities::maxSamplerAnisotropy. */
  float maxAnisotropy = 1.0f;

  /** Used when the sampler is bound as a comparison sampler in a shadow-mapped pipeline. */
  CompareOp compare = CompareOp::Never;
  FilterType filterType = FilterType::NonComparison;

  float lodMinClamp = 0.0f;
  float lodMaxClamp = 1000.0f;
  SamplerBorderColor borderColor = SamplerBorderColor::TransparentBlack;

  /** Set when the sampled texture is a depth format; drives GL_TEXTURE_COMPARE_MODE on OpenGL. */
  bool depthCompare = false;

  const char* label = nullptr;

  /**
   * Set when this sampler reads a YUV image. It has to match the conversion the image's view was created
   * with: Vulkan requires the sampler and the view to name the same conversion object.
   */
  std::optional<YcbcrConversionDescriptor> ycbcr;

  static SamplerDescriptor linearClamp() noexcept {
    SamplerDescriptor descriptor;
    descriptor.magFilter = FilterMode::Linear;
    descriptor.minFilter = FilterMode::Linear;
    descriptor.mipmapFilter = MipmapFilterMode::Linear;
    return descriptor;
  }

  static SamplerDescriptor nearestClamp() noexcept {
    SamplerDescriptor descriptor;
    descriptor.magFilter = FilterMode::Nearest;
    descriptor.minFilter = FilterMode::Nearest;
    descriptor.mipmapFilter = MipmapFilterMode::Nearest;
    return descriptor;
  }
};

// ==================================================================================================
// Buffers
// ==================================================================================================

struct BufferDescriptor {
  /** Size in bytes. Required. */
  uint64_t size = 0;
  /** Bitmask of BufferUsage values. */
  BufferUsages usage = 0;
  MemoryUsage memoryUsage = MemoryUsage::GpuOnly;
  /**
   * Optional initial contents. When non-null the backend uploads `size` bytes from this pointer
   * during creation and the buffer is ready to use without a staging copy.
   */
  const void* initialData = nullptr;

  const char* label = nullptr;

  static BufferDescriptor vertex(const uint64_t size, const void* data = nullptr) {
    return { .size = size,
      .usage = static_cast<BufferUsages>(BufferUsage::Vertex),
      .memoryUsage = MemoryUsage::GpuOnly,
      .initialData = data,
      .label = nullptr };
  }

  static BufferDescriptor index(const uint64_t size, const void* data = nullptr) {
    return { .size = size,
      .usage = static_cast<BufferUsages>(BufferUsage::Index),
      .memoryUsage = MemoryUsage::GpuOnly,
      .initialData = data,
      .label = nullptr };
  }

  static BufferDescriptor uniform(const uint64_t size, const MemoryUsage memory = MemoryUsage::CpuToGpuDynamic) {
    return { .size = size,
      .usage = static_cast<BufferUsages>(BufferUsage::Uniform),
      .memoryUsage = memory,
      .initialData = nullptr,
      .label = nullptr };
  }
};

/** A byte range inside a buffer, used for writes and for descriptor bindings. */
struct BufferBinding {
  BufferHandle buffer = BufferHandle::invalid();
  uint64_t offset = 0;
  /** Number of bytes to bind; 0 means "to the end of the buffer". */
  uint64_t size = 0;

  [[nodiscard]] bool valid() const noexcept { return buffer.valid(); }
};

/**
 * A buffer <-> texture transfer: the two resources plus the regions inside each.
 *
 * The handles are kept separate from the regions rather than nested inside them, because a region
 * describes *where* within a resource a transfer happens and has no business naming the resource
 * itself. Keeping them apart also lets a caller reuse one region across several resources, which is
 * the common case when uploading a mip chain.
 */
struct BufferTextureCopy {
  /** Buffer participating in the transfer. */
  BufferHandle buffer = BufferHandle::invalid();
  /** Texture participating in the transfer. */
  TextureHandle texture = TextureHandle::invalid();

  /** Byte range inside the buffer. */
  BufferCopyRegion bufferRegion = {};
  /** Texel region inside the texture. */
  TextureCopyRegion textureRegion = {};
};

// ==================================================================================================
// Descriptor set layouts
// ==================================================================================================

/**
 * One entry of a descriptor set layout.
 *
 * `binding` is the SPIR-V binding number, and it is authoritative: the OpenGL backend builds a
 * binding -> glUniformLocation table at shader-module creation time by reflecting the linked
 * program, so uniform names never appear in upper layer code.
 */
struct DescriptorBinding {
  uint32_t binding = 0;
  BindingType type = BindingType::TextureSampler;
  /** Number of array elements at this binding; 1 for a non-arrayed binding. */
  uint32_t count = 1;
  /** Stages that will access the binding. Used for validation and for OpenGL uniform lookup. */
  uint8_t stageFlags = 0;
  /**
   * Optional explicit name override for the OpenGL uniform lookup. When null the backend derives the
   * name from SPIRV-Cross reflection data, which is the recommended path.
   */
  const char* debugName = nullptr;
};

struct DescriptorSetLayoutDescriptor {
  DescriptorBinding bindings[kMaxDescriptorBindings] = {};
  uint32_t bindingCount = 0;

  const char* label = nullptr;
};

/** One concrete value assigned to a binding when a descriptor set is updated. */
struct DescriptorWrite {
  uint32_t binding = 0;
  BindingType type = BindingType::TextureSampler;

  TextureHandle texture = TextureHandle::invalid();
  TextureSamplerHandle sampler = TextureSamplerHandle::invalid();
  BufferBinding buffer = {};

  /** Element index within an arrayed binding. */
  uint32_t arrayElement = 0;
  /** Optional explicit view; takes precedence over @p texture when valid. */
  TextureViewHandle view = TextureViewHandle::invalid();
};

// ==================================================================================================
// Pipeline layouts
// ==================================================================================================

struct PipelineLayoutDescriptor {
  DescriptorSetLayoutHandle setLayouts[kMaxDescriptorSetLayouts] = {};
  uint32_t setLayoutCount = 0;

  const char* label = nullptr;
};

// ==================================================================================================
// Queries and synchronisation primitives
// ==================================================================================================

enum class QueryType : uint8_t {
  Occlusion,
  /** GPU timestamps; `Device::getQueryResults` reports nanoseconds on every backend. */
  Timestamp,
  /** Pipeline statistics; requires Capabilities::pipelineStatisticsQuery. */
  PipelineStatistics,
};

struct QuerySetDescriptor {
  QueryType type = QueryType::Occlusion;
  uint32_t count = 1;
  const char* label = nullptr;
};

/** Creation flag for fences. A signalled fence is created ready to be waited on. */
enum class FenceFlag : uint8_t {
  Unsignalled,
  Signalled,
};

struct FenceDescriptor {
  FenceFlag flag = FenceFlag::Unsignalled;
  const char* label = nullptr;
};

struct SemaphoreDescriptor {
  const char* label = nullptr;
};

} // namespace backend

#endif // CORAL_BACKEND_DESCRIPTORS_H


