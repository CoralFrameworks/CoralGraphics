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

#ifndef CORAL_BACKEND_DEVICE_H
#define CORAL_BACKEND_DEVICE_H

#include <backend/Capabilities.h>
#include <backend/Descriptors.h>
#include <backend/Handles.h>
#include <backend/Pipelines.h>
#include <backend/Types.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Forward declarations of the platform object types the device needs. They stay opaque here so that
// this header never drags in EGL, Vulkan, Metal or DXGI headers.
struct ANativeWindow;
struct HWND__;
struct wl_display;
struct wl_surface;
// A translation unit that includes the real EGL headers first already owns these names; redefining
// them as void pointers would contradict `EGL/egl.h`.
#if !defined(__egl_h_)
typedef struct _EGLDisplay* EGLDisplay;
typedef struct _EGLConfig* EGLConfig;
typedef struct _EGLContext* EGLContext;
#endif

namespace backend {

class Buffer;
class Texture;
class CommandBuffer;
class Device;

// ==================================================================================================
// Device configuration
// ==================================================================================================

struct SurfaceDescriptor;

/** Knobs the application can turn when bringing up a device. */
struct DeviceConfiguration {
  /** Preferred backend. The factory falls back to the next available one when it cannot be used. */
  Backend preferredBackend = Backend::Null;

  /**
   * Ordered list of backends to try. `preferredBackend`, when not Null, is moved to the front.
   * A null-terminated array lets callers spell out a policy inline, e.g.
   * `{Backend::Vulkan, Backend::OpenGL}`.
   */
  const Backend* backendPriority = nullptr;

  /**
   * When true the factory may move down the priority list if a backend fails to initialise.
   * When false a failure is reported as-is, which is what automated tests and CI want.
   */
  bool allowFallback = true;

  /**
   * Surface to create the device for. When null the device is created headless: rendering goes to
   * offscreen targets and presentation APIs report Result::Unsupported. This is the path used by
   * unit tests and by offline video processing.
   */
  const SurfaceDescriptor* surface = nullptr;

  /**
   * The platform context this device should share resources with, or null to start a new group.
   *
   * Taken from `Device::nativeContext()` of an existing device of the same backend. Sharing is what makes a texture
   * created on one device samplable from another -- the reason a pool of render threads does not have to copy frames
   * between them.
   */
  void* shareContext = nullptr;

  /** Enable driver debug output / validation where the backend supports it. */
  bool enableValidation = false;

  /** Enable GPU timing instrumentation (timestamp queries where available). */
  bool enableGpuTiming = false;

  /**
   * Optional path for a persistent Vulkan pipeline cache. Empty disables persistence. The cache is
   * loaded when the device is created and written back when it is destroyed; other backends ignore it.
   */
  std::string pipelineCachePath;

  /**
   * Maximum frames the CPU may run ahead of the GPU. 0 lets the backend pick its default, which is
   * 1 for OpenGL ES (whose pipeline is implicitly synchronised) and 2-3 elsewhere.
   */
  uint32_t maxFramesInFlight = 0;

  /**
   * Optional device-level constraint: the renderer's list of features it cannot function without.
   * A backend that cannot provide them is rejected by the factory rather than selected and then
   * producing wrong output, which turns a runtime corruption bug into a startup error.
   */
  Features requiredFeatures = 0;

  /**
   * An explicit OpenGL dialect to ask for, when the OpenGL backend is selected.
   *
   * **Empty means "the platform's own", which is what almost every caller wants.** It has to be
   * expressible as empty rather than as a value, because there is no dialect that is a sensible
   * literal default: the right answer is `ES` on Android and iOS and `Desktop` elsewhere, and a field
   * initialised to `ES` on Windows would be indistinguishable from a caller deliberately asking for
   * ES.
   *
   * Setting it is mainly useful for testing one dialect's capability set on another platform's
   * hardware. A request the platform cannot satisfy fails rather than silently falling back, because
   * coming up in a different dialect than the renderer asked for is how capability bugs are born.
   */
  std::optional<GlDialect> glDialect = std::nullopt;

  /** Optional explicit override of the capability set, used by tests to exercise fallback paths. */
  const Capabilities* capabilityOverride = nullptr;

  const char* appName = "Coral";
};

/** How a surface presents its images. */
enum class PresentMode : uint8_t {
  /** Presentation is queued; the producer never blocks. Adds one frame of latency. */
  Immediate,
  /** VSync-locked with a FIFO of at least 2 images. The only mode guaranteed everywhere. */
  Fifo,
  /** The oldest queued image is replaced when a newer one arrives. */
  Mailbox,
};

/**
 * Platform-neutral description of a presentation surface.
 *
 * Which members matter depends on the backend and the platform:
 *
 *  - **Windows / OpenGL**: `nativeWindow` is the `HWND` and `nativeDisplay` its `HDC`. Leaving both
 *    null gives an offscreen context, which the backend satisfies with a hidden window.
 *  - **Linux / OpenGL**: for X11, `nativeDisplay` is the `Display*` and `nativeWindow` the `Window`;
 *    for Wayland, `nativeDisplay` is the `wl_display*`, `nativeWindow` the `wl_surface*`, and
 *    `nativeWindowExtra` the `wl_egl_window*` that EGL expects. Set `nativeWindowSystem` so the
 *    backend does not have to guess between the two. Leaving both null gives an offscreen context
 *    (EGL surfaceless first, GLX as the fallback).
 *  - **macOS / OpenGL**: `cglContext` adopts an existing `CGLContextObj`, or leaving it null creates a
 *    fresh offscreen one. CGL has no window of its own -- attaching the context to an `NSView` is the
 *    embedding application's job -- so `width`/`height` must be pinned.
 *  - **Android / OpenGL ES**: `nativeWindow` is the `ANativeWindow*`.
 *  - **Any platform / EGL**: `eglDisplay`/`eglSurface`/`eglContext` adopt an existing context, or
 *    `nativeWindow` creates one.
 *  - **Vulkan**: `nativeWindow` (HWND / Xlib Window / `wl_surface*` / `ANativeWindow*`) plus
 *    `instanceExtensions`; `nativeWindowSystem` selects `VK_KHR_wayland_surface` on Linux.
 *  - **Metal**: `nativeWindow` carrying the `CAMetalLayer*`.
 */
struct SurfaceDescriptor {
  /** ANativeWindow* on Android, HWND on Windows, Window/wl_surface* on Linux, CAMetalLayer* for Metal. */
  void* nativeWindow = nullptr;

  /** Display/device handle: an HDC on Windows, a Display* on Linux, an EGLDisplay elsewhere. */
  void* nativeDisplay = nullptr;

  /**
   * A platform extra that belongs with @c nativeWindow, when the window systems disagree on the
   * canonical handle.
   *
   * Wayland is the reason this exists: Vulkan's WSI takes a `wl_surface*`, while EGL takes a
   * `wl_egl_window*` wrapping that same surface. `nativeWindow` is the `wl_surface*` and this is the
   * `wl_egl_window*`; the EGL platform uses this when it is set, and every other platform ignores it.
   */
  void* nativeWindowExtra = nullptr;

  /**
   * Which window system `nativeWindow`/`nativeDisplay` belong to.
   *
   * On Linux this selects GLX (`X11`) or EGL (`Wayland`). `Unknown` keeps the GLX path for a window
   * and, for a headless device, lets the platform pick a context path (EGL first, GLX as fallback).
   * The field is part of the descriptor rather than inferred so that the same handles can later drive
   * a Vulkan `VkSurfaceKHR` without a second, backend-specific convention.
   */
  NativeWindowSystem nativeWindowSystem = NativeWindowSystem::Unknown;

  /** An already-created EGL surface and context, when the embedding application owns them. */
  void* eglSurface = nullptr;
  EGLDisplay eglDisplay = nullptr;
  EGLConfig eglConfig = nullptr;
  EGLContext eglContext = nullptr;

  /** An already-created macOS CGL context (CGLContextObj), when the application owns it. */
  void* cglContext = nullptr;

  /** An already-created Linux GLX context (GLXContext), when the application owns it. */
  void* glxContext = nullptr;

  /**
   * The platform GL context to **share** with, when this device is not the first in its group.
   *
   * Obtained from `Device::nativeContext()` of an existing device; `nullptr` means "be the root of a new group". Only the
   * backends whose contexts share by an explicit handle use it: WGL (`wglShareLists`), EGL
   * (`eglCreateContext(share_context)`), GLX and CGL (share group). A Vulkan or Metal backend ignores it -- those APIs
   * have no per-context object space to share.
   */
  void* glShareContext = nullptr;

  /** CMake-style list of extra instance extensions the Vulkan backend must enable. */
  const char* const* instanceExtensions = nullptr;
  uint32_t instanceExtensionCount = 0;

  uint32_t width = 0;
  uint32_t height = 0;

  /** Preferred swapchain format; Undefined lets the backend choose an 8-bit sRGB default. */
  PixelFormat preferredFormat = PixelFormat::Undefined;
  PresentMode presentMode = PresentMode::Fifo;

  const char* label = nullptr;
};

// ==================================================================================================
// Swapchain
// ==================================================================================================

/**
 * The colour space a swapchain presents in.
 *
 * `Srgb` is the SDR default. `Hdr10Pq` asks the surface for a PQ-encoded 10-bit swapchain; the display
 * stage that writes into it applies the encode (`gpu::SurfaceConfiguration::pqOutput`), and
 * `SwapchainInfo::colorSpace` reports what was actually granted.
 */
enum class SwapchainColorSpace : uint8_t {
  /** `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`, the SDR default. */
  Srgb,
  /** `VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT`, wide-gamut SDR. */
  DisplayP3,
  /** `VK_COLOR_SPACE_HDR10_ST2084_EXT`, PQ-encoded HDR10. */
  Hdr10Pq,
};

struct SwapchainDescriptor {
  SurfaceHandle surface = SurfaceHandle::invalid();
  uint32_t width = 0;
  uint32_t height = 0;
  PixelFormat format = PixelFormat::B8G8R8A8_UNORM;
  PresentMode presentMode = PresentMode::Fifo;
  /** Requested colour space; `Hdr10Pq` asks for a PQ 10-bit swapchain when the surface offers one. */
  SwapchainColorSpace colorSpace = SwapchainColorSpace::Srgb;
  /** Requested number of images. 0 lets the backend choose (typically 2 or 3). */
  uint32_t imageCount = 0;
  const char* label = nullptr;
};

/** Reported state of an acquired frame. */
enum class SwapchainStatus : uint8_t {
  /** The frame is ready to be rendered into. */
  Ready,
  /** The frame is valid but the surface size changed; a resize is pending after presentation. */
  Suboptimal,
  /** The surface is no longer usable (window destroyed, display disconnected, device lost). */
  OutOfDate,
};

/**
 * The static HDR metadata a PQ swapchain should carry.
 *
 * HDR10 is not just "PQ numbers": the display/compositor uses the mastering display primaries, white
 * point and content light levels to decide how to tone-map the signal. Without them a compliant
 * presentation is a guess, which is measurable as content that looks brighter than the SDR reference.
 * Values are the content's, except the mastering luminance pair, which describes the assumed display.
 */
struct HdrMetadata {
  float redPrimary[2] = { 0.708f, 0.292f };
  float greenPrimary[2] = { 0.170f, 0.797f };
  float bluePrimary[2] = { 0.131f, 0.046f };
  float whitePoint[2] = { 0.3127f, 0.3290f };
  /** Mastering display peak, in cd/m^2. */
  float maxLuminance = 1000.0f;
  /** Mastering display black, in cd/m^2. */
  float minLuminance = 0.01f;
  /** Peak content light level, in cd/m^2. */
  float maxContentLightLevel = 203.0f;
  /** Average content light level, in cd/m^2. */
  float maxFrameAverageLightLevel = 203.0f;
};

struct SwapchainInfo {
  uint32_t width = 0;
  uint32_t height = 0;
  PixelFormat format = PixelFormat::Undefined;
  uint32_t imageCount = 0;
  PresentMode presentMode = PresentMode::Fifo;
  uint32_t currentImageIndex = 0;
  /** Current acquired image, for outputs that render directly into the swapchain. Invalid when unsupported. */
  TextureHandle currentImage = TextureHandle::invalid();
  /** Render pass targeting currentImage, for direct-to-surface. Invalid when unsupported. */
  RenderPassHandle currentRenderPass = RenderPassHandle::invalid();
  /** True when the swapchain images are sRGB; drives automatic linear->sRGB conversion. */
  bool srgb = false;
  /** The colour space the swapchain presents in; `Srgb` unless the backend negotiated another one. */
  SwapchainColorSpace colorSpace = SwapchainColorSpace::Srgb;
};

/** A presentable target that a final pass can render into without a pooled frame. */
struct SwapchainTargetInfo {
  /** Attachment view resolved by the backend; the default framebuffer view on Vulkan/GL. */
  TextureViewHandle view = TextureViewHandle::invalid();
  uint32_t width = 0;
  uint32_t height = 0;
  PixelFormat format = PixelFormat::Undefined;
  RenderPassHandle renderPass = RenderPassHandle::invalid();
  bool srgb = false;
};

// ==================================================================================================
// Command buffer
// ==================================================================================================

/** Descriptor for a command buffer. On OpenGL 3.0 this is close to a no-op; Vulkan allocates a pool. */
struct CommandBufferDescriptor {
  /** Hint that this command buffer is re-recorded every frame, enabling cheaper allocation. */
  bool transient = false;
  /** Hint that this command buffer is recorded once and replayed many times. */
  bool oneTimeSubmit = false;
  const char* label = nullptr;
};

/** Index of a descriptor set slot in a pipeline layout. */
using DescriptorSetIndex = uint32_t;

/**
 * Records GPU work.
 *
 * This is the RHI's most important structural decision, and it is deliberately modelled on Vulkan
 * and Metal rather than on OpenGL: recording is separated from submission. Recording touches only
 * CPU-side state - a command buffer never issues a driver call that blocks - so N worker threads can
 * each record their own command buffer and the main thread submits them in a chosen order. Porting
 * such code to Vulkan or Metal is then a mechanical translation, whereas a "draw immediately" API
 * like OpenGL could not be moved to those backends at all.
 *
 * ## Threading contract
 *
 * A command buffer is owned by exactly one thread at a time. Record on the owning thread, then hand
 * ownership to a single submission thread. Handles (`TextureHandle` and friends) are plain values
 * and may be read from any thread; only resource *creation* and *destruction* must be serialised on
 * the device.
 *
 * Reading a handle is therefore not the same as reading the handle's *value*, and the OpenGL backend
 * now guarantees the stronger of the two as well: `createCommandBuffer`/`destroyCommandBuffer` may be
 * called concurrently from any number of threads, and a recording thread may resolve a handle it owns
 * - through the validation and metadata queries the recorder performs - while the device thread is
 * creating other resources. The two things that remain single-threaded are resource
 * creation/destruction and submission. See the backend's `HandlePool` for how the resolution is made
 * safe without putting a lock on the recording path.
 *
 * ## Lifecycle
 *
 * ```
 *   buffer->begin();
 *   buffer->beginRenderPass(info);
 *   buffer->bindRenderPipeline(pipeline);
 *   buffer->draw(3, 1, 0, 0);
 *   buffer->endRenderPass();
 *   buffer->end();
 *   queue->submit(buffer);        // queue takes ownership of execution
 *   buffer->reset();              // safe once the submission has completed
 * ```
 *
 * ## OpenGL 3.0 realisation
 *
 * The OpenGL backend defers every recorded command into an internal, back-patched command list
 * and replays it inside `Queue::submit()`. That is what makes the same recording code work on a
 * backend whose native model is "issue immediately": the abstraction costs one indirection per
 * command, and it buys identical semantics - and therefore identical bugs - across backends.
 */
class CommandBuffer {
  public:
  virtual ~CommandBuffer() = default;

  // --- Lifecycle --------------------------------------------------------------------------------

  /** Starts recording, discarding any previously recorded commands. */
  virtual Result begin() = 0;
  /** Finishes recording. The buffer must have no render pass open. */
  virtual Result end() = 0;

  /**
   * Prints what the recorded list holds, for a backend that can.
   *
   * A diagnostic and nothing else: the default does nothing, and a backend that has a recorded command list counts
   * the commands in it. It exists because a draw that is missing from a picture has two possible causes -- never
   * recorded, or recorded and ignored -- and no renderer statistic can separate them, since a renderer counts the
   * draws it *intends* rather than the commands that survive.
   *
   * Deliberately not part of any hot path and not called unless a caller asks through an environment switch.
   */
  virtual void dumpCommandCounts() const {}

  /**
   * Returns the buffer to its initial state. Only legal once the GPU has finished executing any
   * submission that referenced it; the backend asserts this in development builds.
   */
  virtual Result reset() = 0;

  /** @return true when recording has begun and not yet ended. */
  [[nodiscard]] virtual bool isRecording() const noexcept = 0;

  // --- Render pass scope ------------------------------------------------------------------------

  /** Begins a render pass. Backends bind or create the framebuffer described by @p info. */
  virtual Result beginRenderPass(const RenderPassBeginInfo& info) = 0;
  /** Ends the current render pass, applying stores and resolving multisampled attachments. */
  virtual Result endRenderPass() = 0;

  // --- Pipeline state ---------------------------------------------------------------------------

  virtual Result bindRenderPipeline(RenderPipelineHandle pipeline) = 0;
  virtual Result bindComputePipeline(ComputePipelineHandle pipeline) = 0;

  /** Binds a vertex buffer to @p slot, which indexes the pipeline's vertex buffer layouts. */
  Result setVertexBuffer(const uint32_t slot, const BufferHandle buffer) { return setVertexBuffer(slot, buffer, 0); }

  virtual Result setVertexBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset) = 0;

  /** Binds the index buffer. */
  Result setIndexBuffer(const BufferHandle buffer, const IndexFormat format) {
    return setIndexBuffer(buffer, format, 0);
  }

  virtual Result setIndexBuffer(BufferHandle buffer, IndexFormat format, uint64_t offset) = 0;

  /** Binds a descriptor set at @p setIndex, as declared by the pipeline layout. */
  virtual Result bindDescriptorSet(DescriptorSetIndex setIndex, DescriptorSetHandle set) = 0;

  virtual Result setViewport(const Viewport& viewport) = 0;
  virtual Result setScissor(const ScissorRect& scissor) = 0;
  virtual Result setStencilReference(uint32_t reference) = 0;
  virtual Result setBlendConstant(const Color& color) = 0;

  /**
   * Pushes @p size bytes of data to the pipeline layout's push-constant range. On backends without
   * native push constants (OpenGL in both of its dialects) the backend emulates them with a small
   * internal uniform buffer, so the call always succeeds where `Capabilities` reports
   * `maxPushConstantSize > 0`.
   */
  Result pushConstants(const void* data, const uint32_t size) { return pushConstants(data, size, 0); }

  virtual Result pushConstants(const void* data, uint32_t size, uint32_t offset) = 0;

  // --- Drawing ----------------------------------------------------------------------------------

  Result draw(const uint32_t vertexCount, const uint32_t instanceCount = 1, const uint32_t firstVertex = 0) {
    return draw(vertexCount, instanceCount, firstVertex, 0);
  }

  virtual Result draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) = 0;

  Result drawIndexed(const uint32_t indexCount, const uint32_t instanceCount = 1, const uint32_t firstIndex = 0,
          const int32_t vertexOffset = 0) {
    return drawIndexed(indexCount, instanceCount, firstIndex, vertexOffset, 0);
  }

  virtual Result drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset,
          uint32_t firstInstance) = 0;

  /** Indirect draw; requires Feature::IndirectDraw (always present on ES 3.0). */
  Result drawIndirect(const BufferHandle arguments) { return drawIndirect(arguments, 0); }

  virtual Result drawIndirect(BufferHandle arguments, uint64_t offset) = 0;

  Result drawIndexedIndirect(const BufferHandle arguments) { return drawIndexedIndirect(arguments, 0); }

  virtual Result drawIndexedIndirect(BufferHandle arguments, uint64_t offset) = 0;

  Result dispatch(const uint32_t groupCountX, const uint32_t groupCountY = 1) {
    return dispatch(groupCountX, groupCountY, 1);
  }

  virtual Result dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) = 0;

  Result dispatchIndirect(const BufferHandle arguments) { return dispatchIndirect(arguments, 0); }

  virtual Result dispatchIndirect(BufferHandle arguments, uint64_t offset) = 0;

  /** Inserts a debug marker. Ignored unless the backend supports annotations. */
  virtual Result insertDebugMarker(const char* label) = 0;
  virtual Result beginDebugMarker(const char* label) = 0;
  virtual Result endDebugMarker() = 0;

  // --- Transfer ---------------------------------------------------------------------------------

  /** Uploads `dataSize` bytes into @p buffer at @p offset. */
  virtual Result writeBuffer(BufferHandle buffer, uint64_t offset, const void* data, size_t dataSize) = 0;

  /** Copies between buffers. */
  virtual Result copyBufferToBuffer(BufferHandle src, uint64_t srcOffset, BufferHandle dst, uint64_t dstOffset,
          uint64_t size) = 0;

  /** Uploads texel data into a texture region. */
  Result writeTexture(const TextureHandle texture, const TextureCopyRegion& region, const void* data,
          const uint64_t dataSize) {
    return writeTexture(texture, region, data, dataSize, 0);
  }

  virtual Result writeTexture(TextureHandle texture, const TextureCopyRegion& region, const void* data,
          uint64_t dataSize, uint32_t bytesPerRow) = 0;

  virtual Result copyBufferToTexture(const BufferTextureCopy& copy) = 0;
  virtual Result copyTextureToBuffer(const BufferTextureCopy& copy) = 0;
  virtual Result copyTextureToTexture(TextureHandle src, TextureHandle dst, const TextureCopyRegion& region) = 0;

  /** Clears a colour texture region without needing a render pass. */
  virtual Result clearColorTexture(TextureHandle texture, const TextureSubresourceRange& range, const Color& color) = 0;
  virtual Result clearDepthStencilTexture(TextureHandle texture, const TextureSubresourceRange& range,
          const DepthStencilValue& value) = 0;

  /** Inserts a pipeline barrier. Mostly a no-op on OpenGL; essential later on Vulkan. */
  virtual Result pipelineBarrier() = 0;

  // --- Queries ----------------------------------------------------------------------------------
  Result resetQuerySet(const QuerySetHandle querySet, const uint32_t firstQuery = 0) {
    return resetQuerySet(querySet, firstQuery, 0);
  }

  virtual Result resetQuerySet(QuerySetHandle querySet, uint32_t firstQuery, uint32_t count) = 0;

  Result beginQuery(const QuerySetHandle querySet) { return beginQuery(querySet, 0); }

  virtual Result beginQuery(QuerySetHandle querySet, uint32_t query) = 0;

  Result endQuery(const QuerySetHandle querySet) { return endQuery(querySet, 0); }

  virtual Result endQuery(QuerySetHandle querySet, uint32_t query) = 0;

  /** Debug name, used by capture tools. */
  virtual void setLabel(const char* label) = 0;
};

// ==================================================================================================
// Queue
// ==================================================================================================

/** Kind of work a queue accepts. Backends without real queues map everything onto one. */
enum class QueueType : uint8_t {
  Graphics,
  Compute,
  Transfer,
};

/**
 * Submission endpoint.
 *
 * A command buffer is submitted to exactly one queue. The RHI exposes no more than three queues
 * (one per QueueType) because that is what every backend can promise: OpenGL ES has a single
 * implicit queue, and pretending otherwise would make the abstraction leak.
 */
class Queue {
  public:
  virtual ~Queue() = default;

  [[nodiscard]] virtual QueueType type() const noexcept = 0;

  /**
   * Submits @p commandBuffer. When @p fence is valid it is signalled once the submission completes.
   * @p waitSemaphores / @p signalSemaphores implement GPU-side ordering; on OpenGL they are validated
   * and then ignored, because the GL command stream is already serialised.
   */
  virtual Result submit(CommandBuffer& commandBuffer, FenceHandle fence, SemaphoreHandle waitSemaphore,
          SemaphoreHandle signalSemaphore) = 0;

  Result submit(CommandBuffer& commandBuffer, const FenceHandle fence) {
    return submit(commandBuffer, fence, SemaphoreHandle::invalid(), SemaphoreHandle::invalid());
  }

  Result submit(CommandBuffer& commandBuffer) { return submit(commandBuffer, FenceHandle::invalid()); }

  /** Blocks until every previously submitted command buffer has completed. */
  virtual Result waitIdle() = 0;
};

// ==================================================================================================
// Device
// ==================================================================================================

/** Where a device-initiated error came from, for filtering in the debug callback. */
enum class MessageSeverity : uint8_t {
  Verbose,
  Info,
  Warning,
  Error,
  Fatal,
};

/** Receives backend diagnostics. Only invoked when DeviceConfiguration::enableValidation is set. */
using DebugMessageCallback = void (*)(MessageSeverity severity, const char* message, void* userData);

/**
 * The RHI device: owns every resource, knows what the hardware can do, and hands out command buffers.
 *
 * ## Ownership and lifetime
 *
 * Everything created through a Device is owned by it and destroyed with it; handles become invalid
 * the moment their resource is destroyed, and a stale handle is rejected with Result::InvalidHandle
 * rather than dereferencing freed driver state. Destruction of individual resources is also
 * possible, but the backend may defer the actual release until the GPU has finished with it, which
 * is why the API asks the caller to poll `isResourceReadyToDestroy` (or simply call
 * `waitIdle`) instead of promising immediate reclamation.
 *
 * ## Threading contract
 *
 * Resource creation/destruction is serialised by the device and must not be called concurrently
 * from multiple threads unless the backend documents otherwise. Command *recording* is freely
 * concurrent across command buffers. This split matches Vulkan's external-synchronisation model,
 * so multi-threaded renderers port unchanged.
 */
class Device {
  public:
  virtual ~Device() = default;

  // --- Identity and capabilities ----------------------------------------------------------------

  /**
   * The platform context this device renders through, for sharing it with another device.
   *
   * Opaque on purpose: its meaning is the backend's (`HGLRC`, `EGLContext`, `GLXContext`, `CGLContextObj`), and the only
   * thing a caller may do with it is pass it back in `DeviceConfiguration::shareContext`. Null when the backend has no
   * such object (Vulkan, Metal, Null) or when the device failed to come up.
   */
  [[nodiscard]] virtual void* nativeContext() const noexcept { return nullptr; }
  /** Opaque `VkInstance` of a Vulkan device, or null for other backends. */
  [[nodiscard]] virtual void* nativeInstance() const noexcept { return nullptr; }
  /** Opaque `VkPhysicalDevice` of a Vulkan device. */
  [[nodiscard]] virtual void* nativePhysicalDevice() const noexcept { return nullptr; }
  /** Opaque native device of the backend (VkDevice), or null. */
  [[nodiscard]] virtual void* nativeDevice() const noexcept { return nullptr; }
  /** Opaque graphics queue of the backend (VkQueue), or null. */
  [[nodiscard]] virtual void* nativeQueue() const noexcept { return nullptr; }
  /** Index of the graphics queue family, or zero when the backend has no such concept. */
  [[nodiscard]] virtual uint32_t nativeQueueFamily() const noexcept { return 0; }
  /**
   * Makes this device's context current on the calling thread.
   *
   * A context is thread-affine and can be current on one thread at a time, so a caller that owns several devices must say
   * which one it is about to use -- a window surface does that in `beginFrame`, and a shared-context test does it explicitly.
   * Backends whose "context" is implicit (Vulkan, Metal) accept and ignore it.
   */
  virtual Result makeCurrent() { return Result::Success; }

  /** Releases the calling thread's current context for this device. */
  virtual void releaseCurrent() {}

  [[nodiscard]] virtual const DeviceInfo& getInfo() const noexcept = 0;

  /**
   * Device memory the backend currently holds live, in bytes.
   *
   * The default reports zero, which is what a backend without resource tables can honestly say. The
   * Vulkan and OpenGL backends count every texture and buffer they keep in their tables, so a budget
   * gate can ask "how much VRAM is this scene staying in" rather than "how many bytes did it upload".
   */
  [[nodiscard]] virtual DeviceMemoryStatistics memoryStatistics() const noexcept { return {}; }

  /**
   * The device's capability set. Captured once during device creation and immutable afterwards, so
   * upper layers may cache decisions derived from it.
   */
  [[nodiscard]] virtual const Capabilities& getCapabilities() const noexcept = 0;

  /** @return true when every feature in @p required is available. */
  [[nodiscard]] bool supports(const Features required) const noexcept { return getCapabilities().supports(required); }

  /** @return true when @p format can be used as a render target. */
  [[nodiscard]] bool supportsRenderTarget(const PixelFormat format) const noexcept {
    return getCapabilities().queryTextureFormat(format).renderable;
  }

  // --- Queues -----------------------------------------------------------------------------------

  /** @return the queue for @p type, or nullptr when the device exposes no such queue. */
  [[nodiscard]] virtual Queue* getQueue(QueueType type) noexcept = 0;

  /** Convenience for the graphics queue, which every usable device provides. */
  [[nodiscard]] Queue* graphicsQueue() noexcept { return getQueue(QueueType::Graphics); }

  // --- Command buffers --------------------------------------------------------------------------

  /**
   * Creates a command buffer. Prefer one command buffer per recording thread plus one per frame in
   * flight; they are cheap on every backend.
   */
  [[nodiscard]] virtual Result createCommandBuffer(const CommandBufferDescriptor& descriptor,
          CommandBuffer** outCommandBuffer) = 0;
  virtual void destroyCommandBuffer(CommandBuffer* commandBuffer) = 0;

  // --- Resources --------------------------------------------------------------------------------

  virtual Result createTexture(const TextureDescriptor& descriptor, TextureHandle* outTexture) = 0;
  virtual void destroyTexture(TextureHandle texture) = 0;

  virtual Result createTextureView(const TextureViewDescriptor& descriptor, TextureViewHandle* outView) = 0;
  virtual void destroyTextureView(TextureViewHandle view) = 0;

  virtual Result createSampler(const SamplerDescriptor& descriptor, TextureSamplerHandle* outSampler) = 0;
  virtual void destroySampler(TextureSamplerHandle sampler) = 0;

  virtual Result createBuffer(const BufferDescriptor& descriptor, BufferHandle* outBuffer) = 0;
  virtual void destroyBuffer(BufferHandle buffer) = 0;

  virtual Result createShaderModule(const ShaderModuleDescriptor& descriptor, ShaderModuleHandle* outModule) = 0;
  virtual void destroyShaderModule(ShaderModuleHandle module) = 0;

  /**
   * Reports the descriptor bindings @p module declares, as recovered from its SPIR-V.
   *
   * This is the query that removes uniform names and binding numbers from a renderer's source: the
   * shader already says which set and binding each resource lives at, and the backend has already
   * resolved the name the driver will accept for it. A caller that restates either by hand has two
   * places to keep in sync, and only one of them is checked by the driver.
   *
   * The `name` pointers are owned by the device and stay valid until the module is destroyed.
   *
   * Available for a module created from GLSL as well as one created from SPIR-V, because the GLSL is
   * compiled to SPIR-V before anything else looks at it. The reflection therefore describes what the
   * *compiler produced*, which is the more useful answer: a caller that wrote `bind` in ESSL 1.00 finds
   * out here that the generated module calls it `bind_1` after the compiler's renaming.
   *
   * @param outBindings receives the bindings. Anything already in it is replaced.
   * @return `Result::Unsupported` when the backend has no reflection for the module, which happens only
   *         for a module it could not compile.
   */
  virtual Result getShaderReflection(ShaderModuleHandle module, std::vector<ShaderBindingDeclaration>* outBindings) = 0;

  /**
   * Reports the source provenance of @p module.
   *
   * A module compiled here from GLSL reports its language, version and source hash. A module supplied
   * as SPIR-V has no source: it reports `ShaderSourceInfo::spirvInput = true`, a zero language/version
   * and the hash/size of the supplied words. Only a `Language::GLSLDirect` module has no inspectable
   * provenance and returns `Result::Unsupported`.
   *
   * @param outInfo receives the language, version, source hash and source size.
   */
  virtual Result getShaderSourceInfo(ShaderModuleHandle module, ShaderSourceInfo* outInfo) = 0;

  /**
   * Reports the SPIR-V words of @p module.
   *
   * For a module compiled from GLSL here this is the compiler output, which a caller can store and
   * hand back as a `Language::SPIRV` module. For a module supplied as SPIR-V it is the caller's own
   * words, returned as a copy so the query is uniform across provenance.
   *
   * @param outWords receives the words. Anything already in it is replaced.
   * @return `Result::Unsupported` for a `Language::GLSLDirect` module, which never had words, or
   *         `Result::InvalidHandle` for one that is gone.
   */
  virtual Result getShaderSpirv(ShaderModuleHandle module, std::vector<uint32_t>* outWords) = 0;

  virtual Result createDescriptorSetLayout(const DescriptorSetLayoutDescriptor& descriptor,
          DescriptorSetLayoutHandle* outLayout) = 0;
  virtual void destroyDescriptorSetLayout(DescriptorSetLayoutHandle layout) = 0;

  /** Allocates a descriptor set conforming to @p layout. */
  virtual Result createDescriptorSet(DescriptorSetLayoutHandle layout, DescriptorSetHandle* outSet) = 0;
  virtual void destroyDescriptorSet(DescriptorSetHandle set) = 0;

  /**
   * Applies descriptor writes. The backend converts each write into concrete driver state:
   * on the OpenGL backend a texture/sampler write resolves to a texture unit binding plus a
   * `glUniform1i` of the sampler location, discovered by reflecting the linked program at shader
   * module creation time.
   */
  virtual Result updateDescriptorSet(DescriptorSetHandle set, const DescriptorWrite* writes, uint32_t writeCount) = 0;

  virtual Result createPipelineLayout(const PipelineLayoutDescriptor& descriptor, PipelineLayoutHandle* outLayout) = 0;
  virtual void destroyPipelineLayout(PipelineLayoutHandle layout) = 0;

  /** Creates a render pipeline. Slow on every backend; create once and cache by state hash. */
  virtual Result createRenderPipeline(const RenderPipelineDescriptor& descriptor,
          RenderPipelineHandle* outPipeline) = 0;
  virtual void destroyRenderPipeline(RenderPipelineHandle pipeline) = 0;

  /** Completion for createRenderPipelineAsync; runs on the thread that calls pollAsyncOperations. */
  using PipelineCompletion = std::function<void(RenderPipelineHandle, Result, std::string)>;

  /**
   * Requests an asynchronous render-pipeline creation.
   *
   * The backend must keep recording/simulation untouched: pipeline creation is resource creation, not
   * command recording. A backend that cannot do this returns false and the caller must use
   * createRenderPipeline synchronously. On success the completion is invoked from
   * pollAsyncOperations on the polling thread; the handle is valid only when the result is Success.
   *
   * The default implementation reports "not supported", so existing backends keep their synchronous
   * contract without changes.
   *
   * The descriptor's shader modules, pipeline layout and compatible render pass must stay alive until
   * the completion has run; only the descriptor value itself is copied.
   */
  virtual bool createRenderPipelineAsync(const RenderPipelineDescriptor& /*descriptor*/,
          PipelineCompletion /*completion*/) {
    return false;
  }

  /** Drains completed asynchronous resource operations; call once per frame on the render thread. */
  virtual void pollAsyncOperations() {}

  using ShaderModuleCompletion = std::function<void(ShaderModuleHandle, Result, std::string)>;

  /**
   * Requests asynchronous shader-module creation. Returns false when the backend has no async path.
   * Completion runs from pollAsyncOperations; the descriptor code is copied before this returns.
   */
  virtual bool createShaderModuleAsync(const ShaderModuleDescriptor& /*descriptor*/,
          ShaderModuleCompletion /*completion*/) {
    return false;
  }

  virtual Result createComputePipeline(const ComputePipelineDescriptor& descriptor,
          ComputePipelineHandle* outPipeline) = 0;
  virtual void destroyComputePipeline(ComputePipelineHandle pipeline) = 0;

  virtual Result createRenderPass(const RenderPassDescriptor& descriptor, RenderPassHandle* outPass) = 0;
  virtual void destroyRenderPass(RenderPassHandle renderPass) = 0;

  virtual Result createFramebuffer(const FramebufferDescriptor& descriptor, FramebufferHandle* outFramebuffer) = 0;
  virtual void destroyFramebuffer(FramebufferHandle framebuffer) = 0;

  virtual Result createQuerySet(const QuerySetDescriptor& descriptor, QuerySetHandle* outQuerySet) = 0;
  virtual void destroyQuerySet(QuerySetHandle querySet) = 0;

  virtual Result createFence(const FenceDescriptor& descriptor, FenceHandle* outFence) = 0;
  virtual void destroyFence(FenceHandle fence) = 0;

  virtual Result createSemaphore(const SemaphoreDescriptor& descriptor, SemaphoreHandle* outSemaphore) = 0;
  virtual void destroySemaphore(SemaphoreHandle semaphore) = 0;

  virtual Result createSwapchain(const SwapchainDescriptor& descriptor, SwapchainHandle* outSwapchain) = 0;

  /**
   * Attaches static HDR metadata to a PQ swapchain.
   *
   * A backend without the extension reports `Result::Unsupported`; the presentation is then a PQ
   * signal with default metadata, which is what the basic stage had before this call existed.
   */
  virtual Result setHdrMetadata(SwapchainHandle swapchain, const HdrMetadata& metadata) {
    (void)swapchain;
    (void)metadata;
    return Result::Unsupported;
  }
  virtual void destroySwapchain(SwapchainHandle swapchain) = 0;

  // --- Command buffer execution -----------------------------------------------------------------

  /**
   * Executes a recorded command buffer. This is the OpenGL backend's replay point: the recorded
   * command list is walked here and turned into GL calls, followed by an implicit flush.
   *
   * Equivalent to `graphicsQueue()->submit(buffer)`, and provided because on OpenGL ES there is only
   * one context and introducing a queue object to call it would be ceremony without meaning.
   */
  virtual Result executeCommandBuffer(CommandBuffer& commandBuffer) = 0;

  // --- Buffer access ----------------------------------------------------------------------------

  /**
   * Maps @p buffer for host access. @p offset must respect the backend's alignment requirement
   * (`Capabilities::limits` reports none, but backends round up internally).
   *
   * Only valid for MemoryUsage::CpuToGpu / GpuToCpu / CpuToGpuDynamic buffers. The returned pointer
   * is valid until `unmapBuffer`, and may be nullptr for a device-local buffer.
   */
  virtual Result mapBuffer(BufferHandle buffer, uint64_t offset, uint64_t size, void** outPointer) = 0;
  virtual void unmapBuffer(BufferHandle buffer) = 0;

  // --- Queries ----------------------------------------------------------------------------------

  /** @return the number of queries in @p querySet. */
  [[nodiscard]] virtual Result getQuerySetCount(QuerySetHandle querySet, uint32_t* outCount) const = 0;

  /**
   * Reads back query results. @p outDataSize should be `count * 8` bytes for 64-bit results.
   *
   * `QueryType::Timestamp` results are **nanoseconds** on every backend: OpenGL's timer queries
   * already report that unit and Vulkan's GPU ticks are multiplied by the device's
   * `timestampPeriod`. `Occlusion` and `PipelineStatistics` results are counts, not durations.
   */
  virtual Result getQueryResults(QuerySetHandle querySet, uint32_t firstQuery, uint32_t count, void* outData,
          size_t outDataSize) = 0;

  /** @return true when the result of the query at @p query is available. */
  [[nodiscard]] virtual bool isQueryResultAvailable(QuerySetHandle querySet, uint32_t query) const = 0;

  // --- Synchronisation --------------------------------------------------------------------------

  /** Polls a fence without blocking. */
  [[nodiscard]] virtual Result getFenceStatus(FenceHandle fenceHandle) const = 0;

  Result waitForFence(const FenceHandle fenceHandle) { return waitForFence(fenceHandle, UINT64_MAX); }

  /**
   * Blocks until @p fence is signalled or @p timeoutNanoseconds elapses (UINT64_MAX waits forever).
   * @return Result::Success, Result::NotReady on timeout, or an error.
   */
  virtual Result waitForFence(FenceHandle fenceHandle, uint64_t timeoutNanoseconds) = 0;

  virtual Result resetFence(FenceHandle fence) = 0;

  /** Blocks until all GPU work submitted to this device has completed. */
  virtual Result waitIdle() = 0;

  /**
   * @return true once the GPU has finished with @p handle so it is safe to destroy. Lets a renderer
   * reclaim resources without a full `waitIdle`, which is the difference between a smooth frame and
   * a hitch when a texture is replaced.
   */
  [[nodiscard]] virtual bool isResourceReadyToDestroy(TextureHandle handle) const = 0;
  [[nodiscard]] virtual bool isResourceReadyToDestroy(BufferHandle handle) const = 0;

  // --- Presentation -----------------------------------------------------------------------------

  /** Creates a platform surface. Only meaningful for backends with Feature::NativeSurface. */
  virtual Result createSurface(const SurfaceDescriptor& descriptor, SurfaceHandle* outSurface) = 0;
  virtual void destroySurface(SurfaceHandle surface) = 0;

  /**
   * Acquires the next frame to render into.
   *
   * The returned info describes the images the client should render to. On OpenGL ES this maps to
   * the default framebuffer, so `beginRenderPass` with a surface-colour attachment targets the
   * window directly; there is no separate image to fetch. On Vulkan this is vkAcquireNextImageKHR.
   */
  /**
   * Reports @p swapchain's current state without acquiring a frame.
   *
   * `presentMode` here is what the backend **granted**, not what was requested: a driver with no swap-interval control renders
   * at Fifo whatever was asked for, and a caller that needs to know the difference has to be able to read it back.
   */
  virtual Result getSwapchainInfo(SwapchainHandle swapchain, SwapchainInfo* outInfo) {
    (void) swapchain;
    (void) outInfo;
    return Result::Unsupported;
  }

  /**
   * Acquires the next image for @p swapchain.
   *
   * When @p signalSemaphore is valid the acquire signals it and returns without waiting; the render
   * submission must wait on it. When it is invalid the Vulkan backend waits on a temporary fence
   * before returning, preserving the historical synchronous contract.
   */
  virtual Result beginFrame(SwapchainHandle swapchain, SwapchainInfo* outInfo, SemaphoreHandle signalSemaphore) = 0;

  /**
   * Begins the frame with a drawable the caller already owns.
   *
   * `CAMetalDisplayLink` hands its delegate a `CAMetalDrawable` at the display's target presentation
   * time. Acquiring a second drawable from the layer would either block until the display link's
   * drawable is presented or present two images for one tick, so the Metal backend accepts the one the
   * display link supplied through this call.
   *
   * @p nativeDrawable is a backend-owned object: `id<CAMetalDrawable>` on Metal. It is **borrowed** for
   * the frame and presented by `endFrame`, which is the same lifetime rule `beginFrame` gives the
   * layer-owned drawable.
   *
   * Default: unsupported. A backend whose presentation is not driven by an external display link has
   * no use for it, and returning Unsupported is what keeps the absent path from looking like a
   * successful one.
   */
  virtual Result beginFrameWithDrawable(SwapchainHandle swapchain, void* nativeDrawable, SwapchainInfo* outInfo) {
    (void) swapchain;
    (void) nativeDrawable;
    (void) outInfo;
    return Result::Unsupported;
  }

  /** Acquires a direct-render target for the current swapchain image. Default: unsupported. */
  virtual Result acquireSwapchainTarget(SwapchainHandle swapchain, SwapchainTargetHandle* outTarget,
          SwapchainTargetInfo* outInfo) {
    (void) swapchain;
    (void) outInfo;
    if (outTarget != nullptr) *outTarget = SwapchainTargetHandle::invalid();
    return Result::Unsupported;
  }

  /** Releases/presents a target from acquireSwapchainTarget. Default: no-op. */
  virtual Result presentSwapchainTarget(SwapchainHandle swapchain, SwapchainTargetHandle target) {
    (void) swapchain;
    (void) target;
    return Result::Unsupported;
  }

  Result beginFrame(const SwapchainHandle swapchain, SwapchainInfo* outInfo) {
    return beginFrame(swapchain, outInfo, SemaphoreHandle::invalid());
  }

  /**
   * Acquires the next image only if one is free, without waiting for the display.
   *
   * This is what makes a present skippable: with a FIFO swapchain, `beginFrame` waits until a queued image
   * has been displayed, which is the whole cost of a frame at a fixed refresh rate. A caller willing to
   * drop the frame asks here instead.
   *
   * @return `Result::NotReady` when no image is available, `Success` otherwise.
   *
   * Default: no non-blocking path, so behave exactly like `beginFrame`. A backend whose present is a
   * blocking swap (OpenGL) cannot offer one - there the wait is in `endFrame`, not in the acquire.
   */
  virtual Result tryBeginFrame(const SwapchainHandle swapchain, SwapchainInfo* outInfo) {
    return beginFrame(swapchain, outInfo);
  }

  /**
   * Whether `endFrame` can be queued without waiting for the display.
   *
   * False unless the backend has a present queue of its own: Vulkan's `vkQueuePresentKHR` returns once the
   * image is queued, while an OpenGL `SwapBuffers` waits for the compositor. An engine uses this to decide
   * whether a deferred present request can be honoured, and reports it to the application rather than
   * silently pretending.
   */
  [[nodiscard]] virtual bool supportsDeferredPresent() const noexcept { return false; }

  /** Presents the frame acquired by beginFrame. On OpenGL this is the EGL swap. */
  virtual Result endFrame(SwapchainHandle swapchain) = 0;

  /**
   * Presents after waiting for @p waitSemaphore on the GPU.
   *
   * The default ignores the semaphore and calls the one-argument overload, which is correct for a
   * backend whose present is already ordered after rendering. Vulkan overrides this to pass the
   * semaphore to `vkQueuePresentKHR`, so a render submission and the present need not be separated by
   * `vkDeviceWaitIdle`.
   */
  virtual Result endFrame(SwapchainHandle swapchain, SemaphoreHandle waitSemaphore) {
    (void) waitSemaphore;
    return endFrame(swapchain);
  }

  /** Rebuilds the swapchain after a surface size change or an OutOfDate result. */
  virtual Result resizeSwapchain(SwapchainHandle swapchain, uint32_t width, uint32_t height) = 0;

  // --- Native interop ---------------------------------------------------------------------------

  /**
   * Exposes the underlying driver object so that existing code (a video decoder, a platform
   * compositor) can share the texture without a copy.
   *
   * This is an escape hatch, not a core part of the abstraction: callers must know which backend is
   * active. `Capabilities::acceptsSpirv`-style honesty applies here too - a backend that cannot
   * export a resource returns a `NativeObject` with a null `object`.
   */
  [[nodiscard]] virtual NativeTexture getNativeTexture(TextureHandle texture) const = 0;
  [[nodiscard]] virtual NativeBuffer getNativeBuffer(BufferHandle buffer) const = 0;

  /**
   * Imports an externally created texture (e.g. an AHardwareBuffer or a decoder output surface).
   *
   * @param descriptor names the platform object and the extent and format it holds. The caller keeps
   *                   ownership of that object; see `NativeTextureDescriptor` for what each kind
   *                   promises.
   *
   * An imported texture is sampled and copied from, never rendered into: its storage belongs to the
   * producer, so a caller that wants a private copy renders into a texture it allocated itself. Returns
   * `Result::Unsupported` for a kind this backend has no import for, which `supportsNativeTextureImport`
   * answers for in advance.
   */
  [[nodiscard]] virtual Result importNativeTexture(const NativeTextureDescriptor& descriptor,
          TextureHandle* outTexture) = 0;

  /**
   * Imports a YUV platform image and reports how it has to be read.
   *
   * A YUV image has no pixel format: the driver describes it in terms of an opaque *external format*, and
   * converts it to RGB while it is sampled. The caller cannot know that format -- only the import can ask
   * the driver -- so the resolved conversion comes back with the texture. Every sampler that reads the
   * imported texture, and the view it reads it through, must be built from it.
   *
   * @param descriptor names the platform object and the extent; its `ycbcr` names the coefficients, the
   *                   range and the chroma siting, and its `externalFormat` is ignored because the driver
   *                   reports the authoritative one.
   * @param outTexture receives the imported texture.
   * @param outConversion receives the conversion, with `externalFormat` filled from the driver.
   *
   * The default is `Result::Unsupported`: a backend that cannot sample YUV has nothing to report.
   */
  [[nodiscard]] virtual Result importNativeYuvTexture([[maybe_unused]] const NativeTextureDescriptor& descriptor,
          [[maybe_unused]] TextureHandle* outTexture,
          [[maybe_unused]] YcbcrConversionDescriptor* outConversion) {
    return Result::Unsupported;
  }

  /**
   * @return true when `importNativeTexture` can accept a descriptor of @p kind on this device.
   *
   * The default is `false`: importing is a per-backend, per-platform capability and a device that has
   * not implemented one has to say so rather than let a caller build a descriptor that will fail. The
   * answer is a capability of the *running* device, not of the backend in general -- a Vulkan device
   * whose driver lacks `VK_KHR_external_memory_win32` answers false where one with it answers true.
   */
  [[nodiscard]] virtual bool supportsNativeTextureImport([[maybe_unused]] NativeTextureKind kind) const {
    return false;
  }

  /**
   * @return true when `importNativeYuvTexture` can read an Android YUV `AHardwareBuffer` on this device.
   *
   * This is deliberately separate from `supportsNativeTextureImport(AndroidHardwareBuffer)`: the colour
   * path additionally needs a sampler YCbCr conversion, and a backend may import RGBA buffers without
   * offering one. A caller that wants a YUV buffer should check this before choosing the AHB producer
   * path; when it answers false, fall back to the CPU plane path.
   */
  [[nodiscard]] virtual bool supportsYuvHardwareBufferImport() const { return false; }

  // --- Diagnostics ------------------------------------------------------------------------------

  virtual void setDebugMessageCallback(DebugMessageCallback callback, void* userData) = 0;

  /** Attaches an application supplied label to a resource, visible in GPU capture tools. */
  virtual void setTextureLabel(TextureHandle texture, const char* label) = 0;
  virtual void setBufferLabel(BufferHandle buffer, const char* label) = 0;
  virtual void beginDebugRegion(const char* label) = 0;
  virtual void endDebugRegion() = 0;

  /** Appends a device + capability report. Used by overlays and by bug reports. */
  virtual void appendDiagnostics(std::string& out) const = 0;
};

// ==================================================================================================
// Device creation
// ==================================================================================================

/**
 * Creates a device for @p configuration.
 *
 * The factory walks the backend priority list, creating each candidate and validating it against
 * `configuration.requiredFeatures`. A backend that cannot be created, or that cannot satisfy the
 * requirements, is skipped when `allowFallback` is set; otherwise creation fails with the reason
 * reported through @p debugCallback (when non-null).
 *
 * @param configuration  what the caller needs; see DeviceConfiguration.
 * @param outDevice      receives the device. Untouched on failure.
 * @param debugCallback  optional sink for creation-time diagnostics.
 * @param userData       passed through to @p debugCallback.
 * @return Result::Success, Result::Unsupported when no backend can satisfy the request, or an
 *         error from the failing driver call.
 */
Result createDevice(const DeviceConfiguration& configuration, Device** outDevice,
        DebugMessageCallback debugCallback = nullptr, void* userData = nullptr);

/**
 * @return the GLSL-to-SPIR-V compiler's version string, or "unavailable" when the build has none.
 *
 * Public because it belongs in a cache key: a compiler upgrade can change the SPIR-V produced from
 * identical source, so a cache that ignores the version can serve output the current compiler would not
 * have produced. It is also the first thing to check when a shader that used to work stops compiling.
 */
[[nodiscard]] const char* getShaderCompilerVersion() noexcept;

/** @return true when this build can compile GLSL source to SPIR-V. */
[[nodiscard]] bool isShaderCompilerAvailable() noexcept;

/** @return true when @p backend was compiled into this build. */
[[nodiscard]] bool isBackendAvailable(Backend backend) noexcept;

/** @return the backend that would be chosen on this platform, ignoring configuration. */
[[nodiscard]] Backend getDefaultBackend() noexcept;

/** Name of the backend compiled in by default, for logging. */
[[nodiscard]] const char* getDefaultBackendName() noexcept;

} // namespace backend

#endif // CORAL_BACKEND_DEVICE_H
