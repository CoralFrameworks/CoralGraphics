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

#ifndef CORAL_BACKEND_GL_DEVICE_H
#define CORAL_BACKEND_GL_DEVICE_H

#include <backend/Device.h>
#include <backend/Handles.h>
#include <backend/Validation.h>

#include "ShaderCross.h"
#include "private/backend/gl/GlEntryPoints.h"
#include "private/backend/gl/GlPlatform.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace backend {

/**
 * Default descriptor binding reserved for push constants.
 *
 * OpenGL ES 3.0 has no push-constant concept, so the backend emulates them with one small uniform
 * buffer that is re-uploaded before each draw. A shader that wants push constants declares its
 * uniform block at this binding; the upper layer's `pushConstants()` call then fills it. The
 * emulation is invisible from the caller's side, which is the point: the same recording code runs on
 * Vulkan with real push constants and on OpenGL ES with this substitute.
 */
inline constexpr uint32_t kPushConstantBinding = 0;

/** Sentinel view handle naming the swapchain's default framebuffer (GL framebuffer 0). */
inline constexpr TextureViewHandle kDefaultFramebufferView = TextureViewHandle::invalid();

// ==================================================================================================
// Resource wrappers
// ==================================================================================================

/**
 * Each structure holds the driver object plus the metadata the backend needs to translate an RHI
 * call into GL state. Keeping the metadata here - rather than in the driver - is necessary because
 * GL objects are not self-describing: a GLuint texture does not know its format or sample count.
 */

/**
 * The subset of sampler state that can be stored on a texture object itself.
 *
 * When a descriptor binding supplies no sampler object, the backend must fall back to the texture's
 * own parameters. Comparing this struct against what is already applied lets it skip redundant
 * glTexParameteri calls on the very common "one sampler per texture" path.
 */
struct GLSamplerState {
  GLenum minFilter = GL_LINEAR;
  GLenum magFilter = GL_LINEAR;
  GLenum wrapS = GL_CLAMP_TO_EDGE;
  GLenum wrapT = GL_CLAMP_TO_EDGE;
  GLenum wrapR = GL_CLAMP_TO_EDGE;
  float maxAnisotropy = 1.0f;

  friend bool operator==(const GLSamplerState& a, const GLSamplerState& b) noexcept {
    return a.minFilter == b.minFilter && a.magFilter == b.magFilter && a.wrapS == b.wrapS && a.wrapT == b.wrapT &&
           a.wrapR == b.wrapR && a.maxAnisotropy == b.maxAnisotropy;
  }

  friend bool operator!=(const GLSamplerState& a, const GLSamplerState& b) noexcept { return !(a == b); }
};

/**
 * The part of a GL texture a *recording* thread reads.
 *
 * ## Why this is a separate struct rather than fields on GLTexture
 *
 * `GLCommandBuffer` runs on any number of worker threads and calls `mDevice.texture(handle)` to ask a
 * question of the resource - is it immutable, how many samples does it have, how big is the render
 * target. Every one of those answers is fixed when the texture is created and can never change
 * afterwards, and that is what makes the read safe without a lock. The rest of `GLTexture` is not
 * guaranteed to be fixed: `label` is assigned by `setTextureLabel`, and `appliedSampler` is written by
 * whatever path last touched the texture's parameters. Putting the two groups in one struct invited
 * exactly the bug the design document warns about - a recording thread reading a `std::string` that
 * the device thread is assigning to - because nothing in the type said which fields were which.
 *
 * The split is deliberately not enforced: these are plain aggregates with public members, and a future
 * field could be added to the wrong one. The rule to follow is stated here so that the mistake is at
 * least a documented one, and `GLTexture::meta` being the only thing the recording path is allowed to
 * name is what keeps it checkable by reading the call sites.
 */
struct GLTextureMeta {  /**
   * The GL texture name.
   *
   * Metadata, not mutable state: `createTexture` assigns it once and the only later write is the
   * teardown path zeroing it while the pool entry is being released. A recording thread has to be able
   * to read it, because the recorder resolves a view handle to the GL name it will be bound under.
   */
  GLuint name = 0;
  /** The GL target the texture name is bound to. */
  GLenum target = GL_TEXTURE_2D;
  /** The format the caller asked for. */
  PixelFormat format = PixelFormat::Undefined;
  /** The GL internal format actually allocated (differs from `format` after a downgrade). */
  GLenum internalFormat = 0;
  uint32_t width = 1;
  uint32_t height = 1;
  uint32_t depthOrLayers = 1;
  uint32_t mipLevelCount = 1;
  uint32_t sampleCount = 1;
  TextureType type = TextureType::Texture2D;
  TextureUsages usage = 0;
  /** True when the texture came from importNativeTexture and must not be deleted by us. */
  bool externallyOwned = false;
  /** True when the storage was allocated with glTexStorage* and is therefore immutable. */
  bool immutableStorage = false;
};

/** A GL texture object. */
struct GLTexture {
  /**
   * Creation-time metadata: write once here, never again, and the recording threads may read it.
   *
   * Declared first on purpose. There is no way to make the compiler prove "written once" for an
   * aggregate without turning it into a class with a private payload and a getter, which would ripple
   * through every use site in `GlDevice.cpp` and `GlCommandBuffer.cpp` for a property that only a
   * reader of those two files can check anyway. Placing the block at a fixed offset at least makes the
   * boundary structural: a field added above it is metadata, a field added below it is not. The
   * `offsetof` assertion below is what actually holds the line - it is how the `name` field above was
   * found sitting outside this block when it was first written.
   */
  GLTextureMeta meta;
  /** EGLImage backing an imported AndroidHardwareBuffer, destroyed with the texture. */
  void* eglImage = nullptr;
  /**
   * Sampler state currently applied to this texture's own parameters.
   *
   * OpenGL ES 3.0 has both sampler objects and texture-embedded parameters. A descriptor binding
   * that supplies no sampler falls back to the texture's parameters, and re-applying them on every
   * bind would cost a handful of glTexParameteri calls per draw. This records what is already in
   * place so the backend can skip the redundant work.
   *
   * Below `meta`, so it is not metadata: the recording path must not read it. It never does today -
   * the fallback is applied by the replay path, which owns the bound sampler - and keeping the field
   * on the unsafe side of the line is what keeps that true.
   */
  GLSamplerState appliedSampler = {};
  bool samplerStateValid = false;
  /**
   * Debug label. Assigned by `setTextureLabel`, so a recording thread that read this while the device
   * thread was assigning to it would be reading a `std::string` mid-reallocation - the failure mode
   * the metadata split exists to prevent. Nothing on the recording path may name it.
   */
  std::string label;
};

// The metadata block must sit at the front, so that "is this field metadata?" has a structural answer
// and not only a documentary one.
static_assert(offsetof(GLTexture, meta) == 0, "GLTexture::meta must be the first member");
static_assert(std::is_trivially_copyable_v<GLTextureMeta>,
        "recording threads read the texture metadata concurrently, so it must hold no indirect state");
static_assert(std::is_standard_layout_v<GLTextureMeta>,
        "offsetof above is only meaningful for a standard-layout type");

/**
 * A texture view.
 *
 * OpenGL ES has no view objects, so a view is a *description* the backend applies at bind time: the
 * base texture plus a level/layer range and an optional format reinterpretation. The upper layer's
 * code is then identical to what it would be on Vulkan, where a view is a real object.
 */
struct GLTextureView {
  TextureHandle texture = TextureHandle::invalid();
  TextureViewDimension dimension = TextureViewDimension::Texture2D;
  PixelFormat format = PixelFormat::Undefined;
  uint32_t baseMipLevel = 0;
  uint32_t mipLevelCount = 0;
  uint32_t baseArrayLayer = 0;
  uint32_t arrayLayerCount = 0;
  /** True for the special view that names the swapchain's default framebuffer. */
  bool isDefaultFramebuffer = false;
  std::string label;
};

/**
 * The part of a GL buffer that is fixed at creation, split out of `GLBuffer` for the same reason as
 * `GLTextureMeta`: the recording path asks whether a buffer exists and where its data is, and a
 * recording thread must be able to answer that without racing a device-thread write to `mappedPointer`
 * or to the debug label. Everything here is written once by `createBuffer` and is read afterwards by
 * threads that hold the handle.
 */
struct GLBufferMeta {
  GLuint name = 0;
  GLenum target = GL_ARRAY_BUFFER;
  GLenum glUsage = GL_STATIC_DRAW;
  uint64_t size = 0;
  BufferUsages usage = 0;
  MemoryUsage memoryUsage = MemoryUsage::GpuOnly;
};

struct GLBuffer {
  /** Creation-time metadata. Write once in `createBuffer`, never again. Readable from any thread. */
  GLBufferMeta meta;
  /**
   * Pointer handed out by mapBuffer, kept so unmapBuffer can release the mapping.
   *
   * Not metadata: `mapBuffer` and `unmapBuffer` both assign it, they are device-thread calls, and the
   * recording path has no business reading it. It used to sit among the creation-time fields, which
   * made it look like one of them.
   */
  void* mappedPointer = nullptr;
  bool mapped = false;
  /**
   * Byte range of the mapping, kept so `unmapBuffer` can flush exactly what was mapped.
   *
   * `glFlushMappedBufferRange` is only legal inside the mapped range: asking it to flush more than was mapped
   * raises `GL_INVALID_VALUE`, and the error is sticky -- it sits in the queue until the next `glGetError`, so
   * the *following* call in the process, a `createTexture` say, is the one that appears to fail. Flushing a
   * whole buffer because a sub-range was mapped therefore poisons an unrelated call, which is a diagnostic
   * nobody can follow back to its cause.
   */
  uint64_t mappedOffset = 0;
  uint64_t mappedLength = 0;
  /** Debug label, assigned by `setBufferLabel`; not readable from the recording path. */
  std::string label;
};

static_assert(offsetof(GLBuffer, meta) == 0, "GLBuffer::meta must be the first member");
static_assert(std::is_trivially_copyable_v<GLBufferMeta>,
        "recording threads read the buffer metadata concurrently, so it must hold no indirect state");

struct GLSampler {
  GLuint name = 0;
  SamplerDescriptor descriptor = {};
  std::string label;
};

/**
 * A compiled (but not yet linked) GL shader object plus its reflection data.
 *
 * Deliberately *not* split the way `GLTexture` and `GLBuffer` are. Those two are reached from the
 * recording path (`requireTexture`, `writeTexture`, `beginRenderPass`), so a recording thread reads
 * their fields while the device thread may be writing others; a shader module is only ever read by
 * `linkProgram` and the `getShader*` queries, both of which are device-thread calls. Splitting it
 * would be a mechanical change with no reader to protect, and the churn would make the two splits
 * that do matter harder to find. The rule this file follows is "split what the recorder reads", not
 * "split everything with a `std::string` in it".
 */
struct GLShaderModule {
  ShaderStage stage = ShaderStage::Vertex;
  /** GLSL ES source, either supplied by the caller or produced by SPIRV-Cross. */
  std::string source;
  /** Reflection produced by SPIRV-Cross; empty for hand-written GLSL sources. */
  ShaderReflection reflection;
  /**
   * The same bindings in the public vocabulary, so `getShaderReflection` can hand them out without
   * making the caller include a private header. It owns its names, because the caller's copy has to
   * stay valid after this call returns.
   */
  std::vector<ShaderBindingDeclaration> publicBindings;
  /** Owns the storage the `const char*` names in @c publicBindings point at. */
  std::vector<std::string> bindingNames;
  /**
   * What the source was, when this module was compiled from GLSL.
   *
   * `source` holds the author's GLSL in that case rather than the GLSL generated for the context: a
   * debugger looking at a shader wants the text that was written. The generated GLSL is kept
   * separately, for the diagnostic that shows what the driver was actually asked to compile.
   */
  ShaderSourceInfo sourceInfo;
  /** True when `source` is GLSL that was compiled to SPIR-V here. */
  bool spirvCompiledFromSource = false;
  /** The GLSL emitted for this context. Only meaningful when `spirvTranslated` is set. */
  std::string generatedSource;
  /** The SPIR-V this module was compiled to, when it was compiled from source. */
  std::vector<uint32_t> compiledSpirv;
  /** The caller's SPIR-V words when this module was supplied as `Language::SPIRV`. */
  std::vector<uint32_t> inputSpirv;
  /** The GL shader object. Owned until destroyShaderModule(); a program only attaches it. */
  GLuint shaderObject = 0;
  bool spirvTranslated = false;
  /**
   * True for a `Language::GLSLDirect` module: the caller's GLSL, handed to the driver untouched.
   *
   * Kept apart from `spirvTranslated` because the two answer different questions. A module can have
   * been compiled from source (`spirvCompiledFromSource`) or translated from SPIR-V
   * (`spirvTranslated`), and a direct module is neither - it is the one case where the text the driver
   * compiled is the text the caller wrote *and* no SPIR-V was involved. The binding table below is the
   * caller's for the same reason: there was nothing to reflect.
   */
  bool directGlsl = false;
  /**
   * Vertex inputs whose locations have to be pinned before this module is linked, with the storage
   * their names live in. Only a direct module has any, because only a direct module arrives without
   * `layout(location = ...)` baked into translated GLSL.
   */
  std::vector<ShaderAttributeDeclaration> attributes;
  /** Owns the storage the `const char*` names in @c attributes point at. */
  std::vector<std::string> attributeNames;
  std::string infoLog;
  std::string label;
};

/** Maps a descriptor binding to its location in the linked program. */
struct GLUniformBinding {
  uint32_t set = 0;
  uint32_t binding = 0;
  BindingType type = BindingType::TextureSampler;
  /** glGetUniformLocation result, or -1 when the uniform was optimised out by the compiler. */
  GLint location = -1;
  /** Texture unit assigned to this binding within the program, or -1 for non-samplers. */
  GLint textureUnit = -1;
  uint32_t arraySize = 1;
  /** Uniform block index when the binding is a uniform buffer, otherwise GL_INVALID_INDEX. */
  GLuint blockIndex = 0xFFFFFFFFu;
  /**
   * The GL uniform-block binding this block occupies, which is what `glBindBufferBase` is called with.
   *
   * Normally equal to @c binding, because a shader with no opinion about the numbering gets its
   * descriptor binding number as the block's binding. A declaration that named an explicit
   * `location` moves the two apart, and the draw path has to follow the block rather than the
   * descriptor number or the buffer would be bound to a slot the program does not read.
   */
  GLuint blockBinding = 0;
};

/**
 * A linked GL program plus the binding table that maps RHI descriptor bindings onto its uniforms.
 *
 * This is where descriptor-set semantics meet OpenGL ES. Upper layers write descriptors by
 * (set, binding); the backend looks the pair up in `uniformBindings` and performs the corresponding
 * glUniform1i / glBindBufferBase call. Uniform *names* therefore never appear outside the shader.
 */
struct GLProgram {
  GLuint program = 0;
  ShaderStage stages = ShaderStage::Vertex;
  /** True when the program has a fragment stage, i.e. it can be used for drawing. */
  bool hasFragmentStage = false;
  std::vector<GLUniformBinding> uniformBindings;
  /** Texture units handed out for sampler bindings, monotonically assigned per program. */
  uint32_t usedTextureUnits = 0;
  /** Total uniform block size, used to size the emulated push-constant buffer. */
  uint32_t uniformBlockSize = 0;
  std::string label;

  /**
   * The program's push-constant block, or `GL_INVALID_INDEX` when it declares none.
   *
   * Held apart from `uniformBindings` because a push constant is **not a descriptor**: it belongs to the
   * program rather than to a set, so it is filled by `pushConstants` and bound before each draw instead of
   * being reached through whatever set a caller happens to bind. Skipping it into the binding table -- which
   * is what this field replaced -- meant the emulation never fired, because the only path to it was a
   * descriptor set that happened to name the same binding number.
   *
   * `blockBinding` is the GL uniform-block *binding point* the block was told to occupy
   * (`glUniformBlockBinding`), which is where `glBindBufferBase` has to put the emulated buffer.
   */
  GLuint pushConstantBlockIndex = 0xFFFFFFFFu;
  GLuint pushConstantBlockBinding = 0;

  /** @return the binding at @p set / @p binding, or nullptr when the program has no such binding. */
  [[nodiscard]] const GLUniformBinding* findBinding(uint32_t set, uint32_t binding) const noexcept;
};

struct GLPipelineLayout {
  DescriptorSetLayoutHandle setLayouts[kMaxDescriptorSetLayouts] = {};
  uint32_t setLayoutCount = 0;
  std::string label;
};

struct GLDescriptorSetLayout {
  DescriptorBinding bindings[kMaxDescriptorBindings] = {};
  uint32_t bindingCount = 0;
  std::string label;
};

/** One resolved binding value inside a descriptor set. */
struct GLDescriptorBinding {
  uint32_t binding = 0;
  BindingType type = BindingType::TextureSampler;
  TextureHandle texture = TextureHandle::invalid();
  TextureHandle baseTexture = TextureHandle::invalid();
  TextureSamplerHandle sampler = TextureSamplerHandle::invalid();
  BufferBinding buffer = {};
  uint32_t arraySize = 1;
  bool written = false;
  TextureViewHandle view = TextureViewHandle::invalid();
};

struct GLDescriptorSet {
  DescriptorSetLayoutHandle layout = DescriptorSetLayoutHandle::invalid();
  std::vector<GLDescriptorBinding> bindings;
  std::string label;
};

struct GLRenderPipeline {
  GLProgram program;
  PipelineLayoutHandle layout = PipelineLayoutHandle::invalid();
  VertexInputDescriptor vertexInput = {};
  RasterizerState rasterizer = {};
  DepthStencilState depthStencil = {};
  MultisampleState multisample = {};
  ColorTargetState colorTargets[kMaxColorAttachments] = {};
  uint32_t colorTargetCount = 0;
  PixelFormat depthStencilFormat = PixelFormat::Undefined;
  /** Cached color mask derived from the per-target write masks. */
  bool colorWriteMask[4] = { true, true, true, true };
  std::string label;
};

struct GLComputePipeline {
  GLProgram program;
  PipelineLayoutHandle layout = PipelineLayoutHandle::invalid();
  uint32_t workgroupSize[3] = { 1, 1, 1 };
  std::string label;
};

/**
 * A temporarily bound VAO plus the attribute wiring that was baked into it.
 *
 * Both the comparison and the hash are defined inline because these types live in namespace `backend` while
 * the cache that uses them is internal to the backend. Out-of-line definitions in `GlDevice.cpp` would
 * need external linkage, which would put a backend detail into the library's exported symbols.
 */
struct GLPipelineCacheKey {
  RenderPipelineHandle pipeline = RenderPipelineHandle::invalid();
  BufferHandle vertexBuffers[kMaxVertexBufferLayouts] = {};
  uint32_t vertexBufferCount = 0;
  uint64_t vertexBufferOffsets[kMaxVertexBufferLayouts] = {};

  friend bool operator==(const GLPipelineCacheKey& a, const GLPipelineCacheKey& b) noexcept {
    if (a.pipeline != b.pipeline || a.vertexBufferCount != b.vertexBufferCount) {
      return false;
    }
    for (uint32_t index = 0; index < a.vertexBufferCount; ++index) {
      if (a.vertexBuffers[index] != b.vertexBuffers[index]) {
        return false;
      }
      if (a.vertexBufferOffsets[index] != b.vertexBufferOffsets[index]) {
        return false;
      }
    }
    return true;
  }
};

struct GLPipelineCacheKeyHash {
  size_t operator()(const GLPipelineCacheKey& key) const noexcept {
    // The handles are already dense small integers, so a plain mixing function is enough; the goal is to
    // avoid collisions between pipeline slots, not to defend against adversarial input.
    size_t hash = static_cast<size_t>(key.pipeline.index) * 2654435761u;
    for (uint32_t index = 0; index < key.vertexBufferCount; ++index) {
      hash ^= static_cast<size_t>(key.vertexBuffers[index].index) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
      hash ^= static_cast<size_t>(key.vertexBufferOffsets[index]) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
    }
    return hash;
  }
};

/** A vertex array object capturing one (pipeline, vertex buffer set) combination. */
struct GLVertexArray {
  GLuint name = 0;
};

/**
 * A render pass.
 *
 * Following the RHI's dynamic-rendering model this is a compatibility key rather than a driver
 * object. On OpenGL it additionally holds a reusable framebuffer object, because creating an FBO per
 * frame is measurably expensive on Android drivers.
 */
struct GLRenderPass {
  PixelFormat colorFormats[kMaxColorAttachments] = {};
  uint32_t colorAttachmentCount = 0;
  PixelFormat depthStencilFormat = PixelFormat::Undefined;
  uint32_t sampleCount = 1;
  bool hasDepthStencil = false;
  std::string label;
};

struct GLQuerySet {
  QueryType type = QueryType::Occlusion;
  std::vector<GLuint> queries;
  std::string label;
};

enum class GLFenceState : uint8_t {
  Unsignalled,
  Signalled,
};

struct GLFence {
  GLsync sync = nullptr;
  GLFenceState state = GLFenceState::Unsignalled;
  std::string label;
};

/** Semaphores are bookkeeping only: the GL command stream is already serialised. */
struct GLSemaphore {
  bool signalled = false;
  std::string label;
};

/** EGL-backed swapchain. On OpenGL the images are the default framebuffer, so there is no ring. */
struct GLSwapchain {
  SurfaceHandle surface = SurfaceHandle::invalid();
  uint32_t width = 0;
  uint32_t height = 0;
  PixelFormat format = PixelFormat::B8G8R8A8_UNORM;
  PresentMode presentMode = PresentMode::Fifo;
  bool srgb = false;
  /**
   * Number of images the swapchain rotates through.
   *
   * EGL owns the actual buffer ring and glSwapBuffers rotates it, so the value is reported as 1 to
   * upper layers. Naming it as a field rather than hard-coding 1 at each use keeps the "there is no
   * client-visible ring on OpenGL" fact in one place.
   */
  uint32_t imageCount = 1;
  uint32_t currentImageIndex = 0;
  bool frameAcquired = false;
  std::string label;
};

// ==================================================================================================
// Deferred command list
// ==================================================================================================

/** Discriminator for a recorded command. */
enum class GLCommandKind : uint16_t {
  BeginRenderPass,
  EndRenderPass,
  BindRenderPipeline,
  BindComputePipeline,
  SetVertexBuffer,
  SetIndexBuffer,
  BindDescriptorSet,
  SetViewport,
  SetScissor,
  SetStencilReference,
  SetBlendConstant,
  PushConstants,
  Draw,
  DrawIndexed,
  DrawIndirect,
  DrawIndexedIndirect,
  Dispatch,
  DispatchIndirect,
  WriteBuffer,
  CopyBufferToBuffer,
  WriteTexture,
  CopyBufferToTexture,
  CopyTextureToBuffer,
  CopyTextureToTexture,
  ClearColorTexture,
  ClearDepthStencilTexture,
  PipelineBarrier,
  ResetQuerySet,
  BeginQuery,
  EndQuery,
  DebugMarker,
};

/** Attachment snapshot captured when a render pass is recorded. */
struct GLRecordedAttachment {
  TextureHandle texture = TextureHandle::invalid();
  uint32_t baseMipLevel = 0;
  uint32_t baseArrayLayer = 0;
  uint32_t arrayLayerCount = 1;
  uint32_t sampleCount = 1;
  PixelFormat format = PixelFormat::Undefined;
  LoadOp loadOp = LoadOp::Clear;
  StoreOp storeOp = StoreOp::Store;
  Color clearColor = Color::black();
  DepthStencilValue clearDepthStencil = {};
  /** Multisample resolve destination, when the pass was recorded with one. */
  TextureHandle resolveTexture = TextureHandle::invalid();
  uint32_t resolveMipLevel = 0;
  bool isDefaultFramebuffer = false;
};

/** A single recorded command. One tagged union, deliberately kept small and trivially copyable. */
struct GLCommand {
  GLCommandKind kind = GLCommandKind::Draw;

  RenderPipelineHandle renderPipeline = RenderPipelineHandle::invalid();
  ComputePipelineHandle computePipeline = ComputePipelineHandle::invalid();
  BufferHandle buffer = BufferHandle::invalid();
  BufferHandle buffer2 = BufferHandle::invalid();
  TextureHandle texture = TextureHandle::invalid();
  TextureHandle texture2 = TextureHandle::invalid();
  DescriptorSetHandle descriptorSet = DescriptorSetHandle::invalid();
  QuerySetHandle querySet = QuerySetHandle::invalid();
  RenderPassHandle renderPass = RenderPassHandle::invalid();

  uint32_t uint0 = 0;
  uint32_t uint1 = 0;
  uint32_t uint2 = 0;
  uint32_t uint3 = 0;
  uint32_t uint4 = 0;
  int32_t int0 = 0;
  uint64_t offset = 0;
  uint64_t offset2 = 0;
  uint64_t size = 0;

  const void* data = nullptr;
  uint32_t dataSize = 0;

  IndexFormat indexFormat = IndexFormat::UInt16;
  Viewport viewport = {};
  ScissorRect scissor = {};
  Color color = Color::black();
  DepthStencilValue depthStencil = {};
  TextureSubresourceRange subresourceRange = {};
  TextureCopyRegion textureRegion = {};
  BufferCopyRegion bufferRegion = {};

  /** Render pass state, only meaningful for BeginRenderPass. */
  GLRecordedAttachment colorAttachments[kMaxColorAttachments] = {};
  uint32_t colorAttachmentCount = 0;
  GLRecordedAttachment depthStencilAttachment = {};
  bool hasDepthStencil = false;
  Extent2D renderArea = {};
};

/**
 * The recorded command stream of one command buffer.
 *
 * ## Why the payload arena is chunked rather than one vector
 *
 * `copyToStorage` used to append to a single `std::vector<uint8_t>` and return `storage.data() + offset`. That
 * pointer is stored *in a recorded command*, and the next append can reallocate the vector -- invalidating
 * every pointer handed out before it. The symptom is neither a crash nor an error: the replayed command reads
 * whatever the allocator left there, so a uniform block arrives as garbage and the draw quietly produces
 * nothing. It takes *two* payload-carrying commands for the corruption to appear, which is why a test with one
 * upload passes and a real frame does not, and why reserving more space only moves the threshold.
 *
 * The arena is therefore a list of fixed-size chunks, and a pointer into an earlier chunk stays valid for the
 * life of the list. That is the property the defer-until-submit design depends on: a recorded command outlives
 * the call that recorded it, so everything it points at must outlive it too.
 */
struct GLCommandList {
  /** One arena chunk. Large enough that a normal frame uses a handful, small enough to waste little when it does not. */
  static constexpr size_t kStorageChunkSize = 64 * 1024;

  std::vector<GLCommand> commands;

  /**
   * The arena's chunks.
   *
   * The vector may itself reallocate as chunks are added; the arrays it points at never move, which is the
   * whole reason the payloads live in separately allocated chunks rather than in one growing buffer.
   */
  std::vector<std::unique_ptr<uint8_t[]>> chunks;
  /** The chunk currently being filled, or null before the first payload. */
  uint8_t* currentChunk = nullptr;
  /** How much of @c currentChunk is used. */
  size_t storageOffset = 0;

  /** Appends @p bytes to the arena and returns a stable pointer to the copy. */
  const void* copyToStorage(const void* bytes, size_t byteCount);

  /** @return how many bytes of payload the arena holds, across every chunk. Diagnostics only. */
  [[nodiscard]] size_t storageByteCount() const noexcept {
    return chunks.empty() ? 0 : (chunks.size() - 1) * kStorageChunkSize + storageOffset;
  }

  void clear() {
    commands.clear();
    chunks.clear();
    currentChunk = nullptr;
    storageOffset = 0;
  }
};

// ==================================================================================================
// GL context
// ==================================================================================================

// ==================================================================================================
// Device
// ==================================================================================================

class GLCommandBuffer;

/**
 * The OpenGL ES 3.0 backend.
 *
 * Design notes that matter for portability:
 *
 *  - Recording is deferred. `CommandBuffer::draw` appends to a `GLCommandList`; nothing is sent to
 *    the driver until the list is executed. This is what gives the RHI Vulkan-like semantics on an
 *    API whose native model is "issue immediately", and it is what makes multi-threaded recording
 *    work: worker threads only touch their own command list.
 *  - Absent features are declared, not faked. Compute, storage textures, geometry and tessellation
 *    stages, timeline semaphores and query types beyond occlusion/timestamps are all reported as
 *    unavailable with a specific reason, so `Capabilities` tells the truth and upper layers pick
 *    their fallback path deliberately.
 *  - Push constants are emulated with a small internal uniform block, because ES 3.0 has no
 *    push-constant concept. From the caller's side the two are indistinguishable.
 */
class GLDevice final : public Device {
  public:
  GLDevice();
  ~GLDevice() override;

  GLDevice(const GLDevice&) = delete;
  GLDevice& operator=(const GLDevice&) = delete;

  /** Builds the context, resolves entry points and probes the hardware. */
  Result initialize(const DeviceConfiguration& configuration);

  // --- Device -----------------------------------------------------------------------------------

  /** The platform context object, for DeviceConfiguration::shareContext. */
  [[nodiscard]] void* nativeContext() const noexcept override { return mPlatform.context().context; }

  Result makeCurrent() override;
  void releaseCurrent() override;

  [[nodiscard]] const DeviceInfo& getInfo() const noexcept override { return mInfo; }
  [[nodiscard]] DeviceMemoryStatistics memoryStatistics() const noexcept override {
    DeviceMemoryStatistics statistics;
    statistics.textureBytes = mResidentTextureBytes;
    statistics.bufferBytes = mResidentBufferBytes;
    return statistics;
  }

  [[nodiscard]] const Capabilities& getCapabilities() const noexcept override { return mCapabilities; }

  [[nodiscard]] Queue* getQueue(QueueType type) noexcept override;
  [[nodiscard]] Result createCommandBuffer(const CommandBufferDescriptor& descriptor,
          CommandBuffer** outCommandBuffer) override;
  void destroyCommandBuffer(CommandBuffer* commandBuffer) override;

  Result createTexture(const TextureDescriptor& descriptor, TextureHandle* outTexture) override;
  void destroyTexture(TextureHandle texture) override;

  Result createTextureView(const TextureViewDescriptor& descriptor, TextureViewHandle* outView) override;
  void destroyTextureView(TextureViewHandle view) override;

  Result createSampler(const SamplerDescriptor& descriptor, TextureSamplerHandle* outSampler) override;
  void destroySampler(TextureSamplerHandle sampler) override;

  Result createBuffer(const BufferDescriptor& descriptor, BufferHandle* outBuffer) override;
  void destroyBuffer(BufferHandle bufferHandle) override;

  Result createShaderModule(const ShaderModuleDescriptor& descriptor, ShaderModuleHandle* outModule) override;
  void destroyShaderModule(ShaderModuleHandle module) override;
  Result getShaderReflection(ShaderModuleHandle module, std::vector<ShaderBindingDeclaration>* outBindings) override;
  Result getShaderSourceInfo(ShaderModuleHandle module, ShaderSourceInfo* outInfo) override;
  Result getShaderSpirv(ShaderModuleHandle module, std::vector<uint32_t>* outWords) override;

  Result createDescriptorSetLayout(const DescriptorSetLayoutDescriptor& descriptor,
          DescriptorSetLayoutHandle* outLayout) override;
  void destroyDescriptorSetLayout(DescriptorSetLayoutHandle layout) override;

  Result createDescriptorSet(DescriptorSetLayoutHandle layout, DescriptorSetHandle* outSet) override;
  void destroyDescriptorSet(DescriptorSetHandle set) override;
  Result updateDescriptorSet(DescriptorSetHandle set, const DescriptorWrite* writes, uint32_t writeCount) override;

  Result createPipelineLayout(const PipelineLayoutDescriptor& descriptor, PipelineLayoutHandle* outLayout) override;
  void destroyPipelineLayout(PipelineLayoutHandle layout) override;

  Result createRenderPipeline(const RenderPipelineDescriptor& descriptor, RenderPipelineHandle* outPipeline) override;
  void destroyRenderPipeline(RenderPipelineHandle pipeline) override;

  Result createComputePipeline(const ComputePipelineDescriptor& descriptor,
          ComputePipelineHandle* outPipeline) override;
  void destroyComputePipeline(ComputePipelineHandle pipeline) override;

  Result createRenderPass(const RenderPassDescriptor& descriptor, RenderPassHandle* outPass) override;
  void destroyRenderPass(RenderPassHandle renderPass) override;

  Result createFramebuffer(const FramebufferDescriptor& descriptor, FramebufferHandle* outFramebuffer) override;
  void destroyFramebuffer(FramebufferHandle framebuffer) override;

  Result createQuerySet(const QuerySetDescriptor& descriptor, QuerySetHandle* outQuerySet) override;
  void destroyQuerySet(QuerySetHandle querySet) override;

  Result createFence(const FenceDescriptor& descriptor, FenceHandle* outFence) override;
  void destroyFence(FenceHandle fence) override;

  Result createSemaphore(const SemaphoreDescriptor& descriptor, SemaphoreHandle* outSemaphore) override;
  void destroySemaphore(SemaphoreHandle semaphore) override;

  Result createSwapchain(const SwapchainDescriptor& descriptor, SwapchainHandle* outSwapchain) override;
  void destroySwapchain(SwapchainHandle swapchain) override;

  Result executeCommandBuffer(CommandBuffer& commandBuffer) override;

  Result mapBuffer(BufferHandle bufferHandle, uint64_t offset, uint64_t size, void** outPointer) override;
  void unmapBuffer(BufferHandle bufferHandle) override;

  [[nodiscard]] Result getQuerySetCount(QuerySetHandle querySet, uint32_t* outCount) const override;
  Result getQueryResults(QuerySetHandle querySet, uint32_t firstQuery, uint32_t count, void* outData,
          size_t outDataSize) override;
  [[nodiscard]] bool isQueryResultAvailable(QuerySetHandle querySet, uint32_t query) const override;

  [[nodiscard]] Result getFenceStatus(FenceHandle fenceHandle) const override;
  Result waitForFence(FenceHandle fenceHandle, uint64_t timeoutNanoseconds) override;
  Result resetFence(FenceHandle fenceHandle) override;

  Result waitIdle() override;

  [[nodiscard]] bool isResourceReadyToDestroy(TextureHandle handle) const override;
  [[nodiscard]] bool isResourceReadyToDestroy(BufferHandle handle) const override;

  Result createSurface(const SurfaceDescriptor& descriptor, SurfaceHandle* outSurface) override;
  void destroySurface(SurfaceHandle surface) override;
  Result getSwapchainInfo(SwapchainHandle swapchain, SwapchainInfo* outInfo) override;
  Result beginFrame(SwapchainHandle swapchain, SwapchainInfo* outInfo, SemaphoreHandle signalSemaphore) override;
  Result endFrame(SwapchainHandle swapchain) override;
  Result resizeSwapchain(SwapchainHandle swapchain, uint32_t width, uint32_t height) override;

  [[nodiscard]] NativeTexture getNativeTexture(TextureHandle texture) const override;
  [[nodiscard]] NativeBuffer getNativeBuffer(BufferHandle buffer) const override;
  [[nodiscard]] Result importNativeTexture(const NativeTextureDescriptor& descriptor,
          TextureHandle* outTexture) override;
  /** A GL device imports GL names; a shared Direct3D or hardware buffer is not a name it can bind. */
  [[nodiscard]] bool supportsNativeTextureImport(NativeTextureKind kind) const override;

  /**
   * Installs the diagnostic callback.
   *
   * ## Threading contract
   *
   * This may be called from any thread, at any time, including while other threads are recording. The
   * callback and its user data are two independent atomic pointer-sized values, each stored with
   * release ordering here and loaded with acquire ordering in @c emitMessage, so an emitter observes
   * either the whole previous pair or the whole new one.
   *
   * That is stronger than the design document asked for. It suggested documenting "install once after
   * device creation, never replace while recording", which is what the previous non-atomic fields
   * required and could not enforce. A documented rule that nothing checks is a rule that gets broken
   * by the first diagnostic overlay that is toggled at runtime, and the failure - a recording thread
   * calling through a half-updated function pointer - is a crash in the one code path whose entire job
   * is to explain a different problem. Two relaxed-ish atomic stores are cheaper than that.
   *
   * The callback itself is still expected to be thread-safe on its own terms: it can be entered
   * concurrently, from the device thread and from any recording thread at once.
   */
  void setDebugMessageCallback(DebugMessageCallback callback, void* userData) override;
  void setTextureLabel(TextureHandle texture, const char* label) override;
  void setBufferLabel(BufferHandle buffer, const char* label) override;
  void beginDebugRegion(const char* label) override;
  void endDebugRegion() override;

  void appendDiagnostics(std::string& out) const override;

  // --- Backend internals used by the command buffer ---------------------------------------------

  [[nodiscard]] gl::GlPlatform& platform() noexcept { return mPlatform; }

  [[nodiscard]] const gl::GlPlatform& platform() const noexcept { return mPlatform; }

  [[nodiscard]] const DegradationPolicy& degradationPolicy() const noexcept { return mPolicy; }

  [[nodiscard]] GLTexture* texture(const TextureHandle handle) noexcept { return mTextures.get(handle); }


  [[nodiscard]] const GLTexture* texture(const TextureHandle handle) const noexcept { return mTextures.get(handle); }

  [[nodiscard]] GLTextureView* textureView(const TextureViewHandle handle) noexcept {
    return mTextureViews.get(handle);
  }

  [[nodiscard]] GLBuffer* buffer(const BufferHandle handle) noexcept { return mBuffers.get(handle); }

  [[nodiscard]] const GLBuffer* buffer(const BufferHandle handle) const noexcept { return mBuffers.get(handle); }

  [[nodiscard]] GLSampler* sampler(const TextureSamplerHandle handle) noexcept { return mSamplers.get(handle); }

  [[nodiscard]] GLShaderModule* shaderModule(const ShaderModuleHandle handle) noexcept {
    return mShaderModules.get(handle);
  }

  [[nodiscard]] GLDescriptorSet* descriptorSet(const DescriptorSetHandle handle) noexcept {
    return mDescriptorSets.get(handle);
  }

  [[nodiscard]] GLDescriptorSetLayout* descriptorSetLayout(const DescriptorSetLayoutHandle handle) noexcept {
    return mDescriptorSetLayouts.get(handle);
  }

  [[nodiscard]] GLPipelineLayout* pipelineLayout(const PipelineLayoutHandle handle) noexcept {
    return mPipelineLayouts.get(handle);
  }

  [[nodiscard]] GLRenderPass* renderPass(const RenderPassHandle handle) noexcept { return mRenderPasses.get(handle); }

  [[nodiscard]] GLRenderPipeline* renderPipeline(const RenderPipelineHandle handle) noexcept {
    return mRenderPipelines.get(handle);
  }

  [[nodiscard]] GLComputePipeline* computePipeline(const ComputePipelineHandle handle) noexcept {
    return mComputePipelines.get(handle);
  }

  [[nodiscard]] GLQuerySet* querySet(const QuerySetHandle handle) noexcept { return mQuerySets.get(handle); }

  [[nodiscard]] GLSwapchain* swapchain(const SwapchainHandle handle) noexcept { return mSwapchains.get(handle); }

  [[nodiscard]] GLSemaphore* semaphore(const SemaphoreHandle handle) noexcept { return mSemaphores.get(handle); }

  /** Turns a texture-view handle into the GL texture plus the state needed to bind it. */
  struct ResolvedView {
    GLuint textureName = 0;
    GLenum target = GL_TEXTURE_2D;
    PixelFormat format = PixelFormat::Undefined;
    bool isDefaultFramebuffer = false;
    bool valid = false;
  };

  [[nodiscard]] ResolvedView resolveTextureView(TextureViewHandle view) const;

  /**
   * Returns a cached framebuffer object for the given attachment set, creating it when absent.
   *
   * @param colorAttachments  GL texture names for each colour slot.
   * @param depthStencilName  GL texture name of the depth/stencil attachment, or 0.
   * @param colorCount        number of valid entries in @p colorAttachments.
   * @param key               cache key, normally built by `makeFramebufferKey`.
   * @param outFbo            receives the framebuffer name, or 0 for the default framebuffer.
   */
  [[nodiscard]] Result acquireFramebuffer(const GLuint* colorAttachments, GLuint depthStencilName, uint32_t colorCount,
          uint64_t key, GLuint* outFbo);

  /** Builds the cache key used by acquireFramebuffer. */
  [[nodiscard]] static uint64_t makeFramebufferKey(const GLuint* colorAttachments, GLuint depthStencilName,
          uint32_t colorCount) noexcept;

  /**
   * One entry of the framebuffer cache: the object, and **the attachments it was built for**.
   *
   * The key alone cannot answer "does this framebuffer reference GL name N" -- it is a hash, and the depth
   * attachment and the attachment count are folded into it -- so the names are kept beside it. They are
   * what `discardFramebuffersUsing` needs, and without them a destroyed texture would leave a framebuffer
   * attached to a deleted name, which is the defect this field exists to let the backend find.
   */
  struct CachedFramebuffer {
    GLuint framebuffer = 0;
    GLuint colorAttachments[kMaxColorAttachments] = {};
    uint32_t colorCount = 0;
    GLuint depthStencil = 0;

    /** @return true when @p name is one of the attachments this framebuffer was built for. */
    [[nodiscard]] bool uses(const GLuint name) const noexcept {
      if (name == 0) return false;
      if (depthStencil == name) return true;
      for (uint32_t index = 0; index < colorCount; ++index) {
        if (colorAttachments[index] == name) return true;
      }
      return false;
    }
  };

  /**
   * Deletes every cached framebuffer built for @p name, and drops its cache entries.
   *
   * Called when a texture is destroyed. A GL name is recycled by the next `glGenTextures`, so a cache entry
   * left behind would be *found* by a later render pass whose attachment happens to land on the same name --
   * and a framebuffer whose attachment has been deleted is incomplete, so binding it raises
   * `GL_INVALID_FRAMEBUFFER_OPERATION` and renders nothing. The error is sticky too: it surfaces on the next
   * call that checks, which is a `createTexture`, so the symptom is a texture that cannot be created for a
   * reason that has nothing to do with it.
   */
  void discardFramebuffersUsing(GLuint name);

  /** Reports a driver error through the debug callback, if one is installed. */
  void reportGLError(const char* operation) const;

  /** Deletes every cached framebuffer object and clears the cache. */
  void clearFramebufferCache();

  /** Deletes every cached vertex array object and clears the cache. */
  void clearVertexArrayCache();

  /**
   * Finds or creates a vertex array object for @p key.
   *
   * The VAO wires the attribute pointers for one (pipeline, vertex buffer set) combination and caches
   * them, so a steady-state frame binds one VAO per pipeline change instead of re-specifying every
   * attribute for every draw. Creation is delegated to @p configure, which is invoked with the new VAO
   * already bound - the caller knows the attribute layout, the device only knows how to cache.
   */
  template<typename ConfigureFunction>
  GLuint acquireVertexArray(const GLPipelineCacheKey& key, ConfigureFunction&& configure) {
    const auto cached = mVertexArrayCache.find(key);
    if (cached != mVertexArrayCache.end()) {
      return cached->second.name;
    }

    gl::GLApi& api = gl::getGLApi();
    if (api.glGenVertexArrays == nullptr) {
      return 0;
    }

    GLuint vertexArray = 0;
    api.glGenVertexArrays(1, &vertexArray);
    if (vertexArray == 0) {
      return 0;
    }

    api.glBindVertexArray(vertexArray);
    configure(vertexArray);
    api.glBindVertexArray(0);
    // The element array binding is part of VAO state, so leaving it set here would leak into the next
    // draw; the replay path re-binds it when it needs it.
    api.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    mVertexArrayCache.emplace(key, GLVertexArray{ vertexArray });
    return vertexArray;
  }

  /**
   * Emits a diagnostic message through the debug callback, if one is installed.
   *
   * Callable from any thread, including a recording thread reporting a validation failure. The
   * callback pointer is loaded once, with acquire ordering, so the callback is never observed as
   * replaced half-way through; see @c setDebugMessageCallback.
   */
  void emitMessage(MessageSeverity severity, const char* message) const;

  /**
   * Executes a recorded command buffer and optionally signals @p fence when the GPU reaches the end.
   *
   * Split out from executeCommandBuffer so that the queue can attach a fence to a submission without
   * duplicating the replay logic.
   */
  Result submitOnQueue(CommandBuffer& commandBuffer, FenceHandle fence);

  /** Formats a GL enum as a format name for diagnostics. */
  [[nodiscard]] static const char* glErrorName(const GLenum error) noexcept { return gl::GLApi::errorName(error); }

  /**
   * The command buffer container, exposed so a test can assert that destroy actually removed the entry.
   *
   * A const accessor rather than a count, because a count would invite a caller to treat it as a load
   * indicator and the lock it would have to take is the point: a reader of `mCommandBuffers` must hold
   * the same lock the writers do, and this accessor makes that the only way to look.
   *
   * Public rather than behind `friend`, because there is nothing here for a friend declaration to
   * protect: the container is a private implementation detail, and a caller that can read it still
   * cannot hand a `unique_ptr` back or keep a reference past the next destroy. Exposing a device-level
   * ownership count on the public `Device` interface was the alternative, and it would have put a
   * debugging affordance into the API that upper layers would reasonably start depending on.
   */
  [[nodiscard]] size_t ownedCommandBufferCount() const {
    const std::lock_guard<std::mutex> guard(mCommandBufferLock);
    return mCommandBuffers.size();
  }

  private:
  friend class GLCommandBuffer;

  /** Creates and links a program from two compiled shader modules, building the binding table. */
  Result linkProgram(const GLShaderModule& vertexModule, const GLShaderModule& fragmentModule,
          GLProgram* outProgram) const;

  /** Creates and links a compute program. */
  Result linkComputeProgram(const GLShaderModule& computeModule, GLProgram* outProgram) const;

  /** Builds the binding table for a freshly linked @p program from the modules' reflection data. */
  Result buildBindingTable(GLuint program, const GLShaderModule* vertexModule, const GLShaderModule* fragmentModule,
          const GLShaderModule* computeModule, GLProgram* outProgram) const;

  /** Probes the running GL and fills in mCapabilities / mInfo. */
  void probeCapabilities();

  DeviceInfo mInfo = {};
  Capabilities mCapabilities = {};
  DegradationPolicy mPolicy = {};
  gl::GlPlatform mPlatform;

  HandlePool<TextureTag, GLTexture> mTextures;
  HandlePool<TextureViewTag, GLTextureView> mTextureViews;
  HandlePool<TextureSamplerTag, GLSampler> mSamplers;
  /** Live device memory, counted at the texture and buffer tables; see `Device::memoryStatistics`. */
  uint64_t mResidentTextureBytes = 0;
  uint64_t mResidentBufferBytes = 0;
  HandlePool<BufferTag, GLBuffer> mBuffers;
  HandlePool<ShaderModuleTag, GLShaderModule> mShaderModules;
  HandlePool<DescriptorSetLayoutTag, GLDescriptorSetLayout> mDescriptorSetLayouts;
  HandlePool<DescriptorSetTag, GLDescriptorSet> mDescriptorSets;
  HandlePool<PipelineLayoutTag, GLPipelineLayout> mPipelineLayouts;
  HandlePool<RenderPipelineTag, GLRenderPipeline> mRenderPipelines;
  HandlePool<ComputePipelineTag, GLComputePipeline> mComputePipelines;
  HandlePool<RenderPassTag, GLRenderPass> mRenderPasses;
  HandlePool<FramebufferTag, GLuint> mFramebuffers;
  HandlePool<QuerySetTag, GLQuerySet> mQuerySets;
  HandlePool<FenceTag, GLFence> mFences;
  HandlePool<SemaphoreTag, GLSemaphore> mSemaphores;
  HandlePool<SwapchainTag, GLSwapchain> mSwapchains;
  HandlePool<SurfaceTag, SurfaceDescriptor> mSurfaces;

  /** Cached framebuffer objects keyed by attachment set. */
  std::unordered_map<uint64_t, CachedFramebuffer> mFramebufferCache;
  /** Vertex array objects keyed by (pipeline, vertex buffers). */
  std::unordered_map<GLPipelineCacheKey, GLVertexArray, GLPipelineCacheKeyHash> mVertexArrayCache;
  /**
   * Command buffers allocated by this device, owned here.
   *
   * A `unique_ptr` each, so teardown needs no manual `delete` and an early return cannot leak one. The member is declared
   * *after* the pools above on purpose: members are destroyed in reverse order, so these are released before the pools
   * they reference, which is the ordering teardown needs. That used to be enforced by an explicit delete loop; it is now
   * a property of where the member sits.
   *
   * ## Threading contract
   *
   * `createCommandBuffer` and `destroyCommandBuffer` may be called concurrently, from any number of
   * threads, and `mCommandBufferLock` is what makes that true. The design document's preferred
   * alternative - a per-thread list of command buffers, so no lock is needed - was rejected for a
   * reason that is easy to miss: `destroyCommandBuffer` has to *find* the entry to erase it, so
   * per-thread storage only removes the lock if the caller also promises to destroy each buffer on
   * the thread that created it. That promise is not one this API can ask for, because the natural
   * owner of a finished submission is the submission thread, and a `CommandBuffer*` that cannot be
   * handed back is a worse contract than a mutex.
   *
   * The lock is nowhere near a draw: it is taken twice per command buffer - once at creation, once at
   * destruction - and a command buffer lives for a whole frame or more, so the contention is bounded
   * by "frames in flight" rather than by anything in the recording loop. `std::vector` stays the
   * container for the same reason: the operations that would need it to be lock-free are the ones the
   * lock already covers.
   *
   * A reader that walks this vector while another thread pushes is what the lock forbids - `push_back`
   * reallocates and frees the old block, so a walk that is not under the same lock is a
   * use-after-free, not merely a stale read. Nothing outside this class touches `mCommandBuffers`;
   * `GLCommandBuffer` addresses the device through the pools.
   */
  mutable std::mutex mCommandBufferLock;
  std::vector<std::unique_ptr<GLCommandBuffer>> mCommandBuffers;

  /** Identity of the thread that created the device; GL contexts are thread affine. */
  uint64_t mOwnerThread = 0;
  /** Currently bound render pipeline, tracked to avoid redundant glUseProgram calls. */
  RenderPipelineHandle mCurrentRenderPipeline = RenderPipelineHandle::invalid();
  ComputePipelineHandle mCurrentComputePipeline = ComputePipelineHandle::invalid();
  /** Currently bound descriptor sets per pipeline layout slot. */
  DescriptorSetHandle mBoundDescriptorSets[kMaxDescriptorSetLayouts] = {};

  /** Push-constant emulation: a small uniform block uploaded as plain uniforms. */
  std::vector<uint8_t> mPushConstantData;
  uint32_t mPushConstantSize = 0;

  /**
   * The debug callback and its user data.
   *
   * Atomics rather than plain members, because `emitMessage` runs on whatever thread recorded the
   * failing command. See `setDebugMessageCallback` for the ordering and why the alternative - an
   * install-once rule in prose - was not good enough.
   */
  std::atomic<DebugMessageCallback> mDebugCallback { nullptr };
  std::atomic<void*> mDebugUserData { nullptr };

  /**
   * The single queue the device exposes.
   *
   * Declared as a `void*`-free unique_ptr to a forward declared type, so the queue implementation can
   * stay in the .cpp file where it belongs.
   */
  class GLQueueImpl;
  std::unique_ptr<GLQueueImpl> mGraphicsQueue;

  bool mInitialized = false;
};

} // namespace backend

#endif // CORAL_BACKEND_GL_DEVICE_H


