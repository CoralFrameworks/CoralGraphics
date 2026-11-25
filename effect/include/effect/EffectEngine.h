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

#ifndef CORAL_EFFECT_ENGINE_H
#define CORAL_EFFECT_ENGINE_H

#include <effect/EffectTypes.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <functional>
#include <string>
#include <string_view>

namespace backend {
class Device;
}

namespace coral {

class EffectNode;
class EffectPassNode;
class EffectOutputNode;
class Fence;

namespace detail {
class EngineImpl;
} // namespace detail

/**
 * Receives engine and node diagnostics.
 *
 * Invoked from whichever thread produced the message, which may be the render thread; an implementation
 * must not call back into the engine from it, and must not block.
 */
using EffectDiagnosticsCallback = void (*)(EffectSeverity severity, const char* message, void* userData);

/** Everything an engine needs to come up. */
struct EffectEngineConfig {
  EffectBackend backend = EffectBackend::Default;

  /**
   * A platform-native context to share GPU resources with, when the platform adapter creates the device.
   *
   * Not a backend handle and not interpreted by the core: Android passes an `EGLContext`, Apple an
   * `EAGLContext`/`id<MTLDevice>`, and the platform node that understands it does the sharing. The core
   * engine leaves it alone.
   */
  void* sharedContext = nullptr;

  /**
   * An existing device the engine should render through instead of creating one.
   *
   * This is the first half of the "model B" ownership split: an application (or the UI render host)
   * owns the device, and every engine on that device renders through it. The engine does **not** take
   * ownership and does not destroy it on `stop()` or `recover()`.
   *
   * The caller must keep the device alive for the engine's whole lifetime and must release its context
   * from the calling thread before `start()` (an OpenGL context can only be current on one thread). The
   * engine still owns its render thread; a later phase adds an external-drive mode so the UI render
   * thread can call `execute()` directly.
   */
  backend::Device* externalDevice = nullptr;

  /**
   * Do not create the engine's own render thread.
   *
   * The caller drives the engine by calling `execute()` (and the graph/config methods that queue
   * commands) on the thread that owns the external device. This is the second half of model B: a
   * UI render host can drive several engines on one thread and one device, which is what makes
   * multi-preview zero-copy possible with OpenGL's thread-affine context.
   *
   * Requires `externalDevice != nullptr`; `start()` refuses the combination otherwise.
   */
  bool externalDrive = false;

  /**
   * Adopt an existing EGL display/config/context instead of creating a context.
   *
   * Android uses this when the application already owns the context that produced its
   * `SurfaceTexture`; the platform adapter makes that context current on the render thread and creates
   * the output EGLSurface itself. All three handles are platform EGL handles and are not interpreted by
   * the core. When `eglContext` is null the engine creates its own context as before.
   */
  void* eglDisplay = nullptr;
  void* eglConfig = nullptr;
  void* eglContext = nullptr;

  /** The engine's render size. Required before `start()`; `setRenderSize` may change it later. */
  uint32_t renderWidth = 0;
  uint32_t renderHeight = 0;

  EffectRenderMode renderMode = EffectRenderMode::Continuous;

  /** Target frame rate; 0 follows the output surface's vsync. */
  float frameRate = 0.0f;

  /**
   * Canonical ticks per second for every `RenderTime` the engine handles.
   *
   * Input timestamps, texture-sequence start times and `setSequenceTime` all use this base, so a seek is
   * an integer tick value rather than a floating point second.
   */
  uint32_t timeBase = 60;

  /** How many frames may be recorded/submitted ahead of the GPU. */
  uint32_t maxFramesInFlight = 2;

  /** Resident budget of the frame pool. Idle frames are evicted; in-flight frames never are. */
  size_t framePoolBudgetBytes = 256u * 1024u * 1024u;

  /**
   * Soft cap for cached shader programs. Programs not resolved for a full submission ring are
   * destroyed between frames once no submission is in flight.
   */
  uint32_t maxCachedPrograms = 64;

  /** Soft budget for image/solid texture cache bytes. Oldest entries are evicted first. */
  size_t textureCacheBudgetBytes = 64u * 1024u * 1024u;

  /** Ask the backend for timestamp queries and report `EffectEngineStats::gpuMilliseconds`. */
  bool enableGpuTiming = false;

  /** Optional persistent Vulkan pipeline-cache file; empty disables it. Other backends ignore it. */
  std::string vulkanPipelineCachePath;

  /**
   * Compile new pass shaders on a worker thread where the backend accepts SPIR-V. The previous program
   * keeps drawing until the new one is ready. Off by default so the first frame can still be synchronous.
   */
  bool asyncShaderCompilation = false;

  /** Ask Vulkan to create render pipelines on the backend worker. Requires asyncShaderCompilation. */
  bool asyncPipelineCreation = false;

  /**
   * Initialise the GLSL compiler during `start()` instead of during the first frame.
   *
   * The first compile on a thread pays ~45 ms to build the compiler and its built-in symbol table - but only
   * when the shader is not already in the on-disk cache, where a hit costs about half a millisecond and never
   * touches the compiler at all. Warming up moves that 45 ms into `start()`: on a cold cache the first frame gets
   * ~45 ms faster and the start block gets ~45 ms slower, and on a warm cache it is spent for nothing.
   *
   * On when the device takes SPIR-V, because that is the only configuration where this compiler exists. Set it to
   * false for a tool that starts an engine but never compiles a shader.
   */
  bool warmUpShaderCompiler = true;

  /** Preview drops the oldest frame; an offline export blocks instead. */
  EffectBackpressureMode backpressureMode = EffectBackpressureMode::DropOldest;

  bool enableValidation = false;

  void (*diagnosticsCallback)(EffectSeverity severity, const char* message, void* userData) = nullptr;
  void* diagnosticsUserData = nullptr;

  const char* label = "coral::EffectEngine";
};

/** A read-only snapshot of the engine's counters, taken by `EffectEngine::stats()`. */
struct EffectEngineStats {
  uint64_t frameCount = 0;
  /** Frames whose last pass rendered directly into the output target. */
  uint64_t directFrames = 0;
  double framesPerSecond = 0.0;
  double cpuMilliseconds = 0.0;
  double gpuMilliseconds = 0.0;
  uint32_t framesInFlight = 0;
  uint32_t droppedFrames = 0;
  uint32_t nodeCount = 0;
  uint32_t programCount = 0;
  uint32_t pipelineCount = 0;
  uint32_t textureCount = 0;
  uint32_t commandQueueDepth = 0;

  uint32_t framePoolFree = 0;
  uint32_t framePoolInUse = 0;
  uint32_t framePoolInFlight = 0;
  uint64_t framePoolBytes = 0;
  uint64_t framePoolPeakBytes = 0;
  uint64_t framePoolHits = 0;
  uint64_t framePoolMisses = 0;
  uint64_t framePoolEvictions = 0;
  uint64_t backpressureBlocked = 0;
  uint64_t programEvictions = 0;
  uint64_t imageTextureEvictions = 0;
  uint64_t solidTextureEvictions = 0;
  uint64_t textureCacheBytes = 0;
  uint64_t textureCachePeakBytes = 0;
  uint64_t programCompileCount = 0;
  double programCompileMilliseconds = 0.0;
  uint64_t pipelineCreationCount = 0;
  double pipelineCreationMilliseconds = 0.0;
};

/**
 * The engine: the single entry point, the owner of the render thread and of every GPU resource.
 *
 * ## Lifetime and threads
 *
 * `create` and the destructor run on the application thread. By default the engine owns a dedicated
 * render thread; every GPU object - programs, pipelines, frame textures - is created, used and destroyed
 * there. The public methods below are safe to call from any application thread: control operations are
 * serialized onto the render thread, and data operations (`setParameter`, `setTexture`, ...) publish
 * snapshots that the render thread reads once per frame.
 *
 * With `EffectEngineConfig::externalDrive` the engine creates **no** render thread: the caller owns the
 * thread and the external device, calls `start()` on it, drives `execute()` synchronously, and must call
 * `stop()` on that same thread. This is what lets a UI render host run several engines on one device.
 *
 * A node may outlive the engine. Its handle stays valid, but it reports `isAttached() == false` and its
 * setters return `NotAttached`; no GPU object is touched from the application thread.
 *
 * The render thread owns a strong reference to the implementation while it runs, so the engine cannot be
 * destroyed underneath it. Destroying the last handle requests the stop and waits for the thread to exit
 * before the graph is released; `stop()` may therefore also be called by an owner that never stopped the
 * engine explicitly, and calling it twice is safe.
 *
 * The graph is a DAG: `connect` accepts any acyclic topology (fan-out included), the executor runs shared
 * prefixes once and supports multiple terminal outputs. A node that is not reachable from an input or
 * cannot reach an output is rejected by the graph validator.
 */
class EffectExternalTextureResolver;

class EffectEngine final {
public:
  /** Test/diagnostic observer for graph mutation calls; invoked on the calling thread. */
  enum class GraphCall { Attach, Connect, Disconnect };

  using GraphCallObserver = std::function<void(GraphCall)>;
  static void setGraphCallObserver(GraphCallObserver observer);

  /**
   * Creates an engine. @return null when the configuration itself is invalid; a GPU device is not
   * required until `start()`, which reports `DeviceLost` (with `diagnostics()`) when none can come up.
   *
   * Creation is cheap on purpose: a node graph can be built and inspected on a machine with no graphics
   * stack, and the render thread - the thing that owns the device - is started explicitly.
   */
  static std::shared_ptr<EffectEngine> create(const EffectEngineConfig& config);
  static std::shared_ptr<EffectEngine> create(EffectBackend backend, void* sharedContext = nullptr);

  /** Registers the platform resolver used by TextureSource::Kind::External. */
  void setExternalTextureResolver(std::shared_ptr<EffectExternalTextureResolver> resolver);

  /** Called on the render thread after a device is created or recovered. */
  void setOnDeviceReady(std::function<void(backend::Device&)> callback);

  /**
   * Sets a callback that runs on the render thread before every frame, with the device current.
   *
   * A producer of GPU textures that the graph samples uses it to submit its recording and make the
   * texture current before the passes read it; the transport thread only records.
   */
  void setOnBeforeFrame(std::function<void(backend::Device&)> callback);

  /**
   * Sets a callback that runs on the render thread after every frame, with the device current.
   *
   * A consumer that samples this engine's output texture and records its own UI (an editor page)
   * needs this thread, not the caller's transport thread: the device context is current here and the
   * output submission is already recorded.
   */
  void setOnAfterFrame(std::function<void(backend::Device&)> callback);

  /**
   * Sets a callback that runs on the render thread after every frame, with a fence for its GPU
   * completion.
   *
   * <p>The fence may already be signalled by the time the callback runs; waiting on it is still
   * valid and returns `kConditionSatisfied`. It must not be waited on from the render thread.
   */
  void setOnFrameSubmitted(
      std::function<void(backend::Device&, std::shared_ptr<Fence>)> callback);

  ~EffectEngine();

  EffectEngine(const EffectEngine&) = delete;
  EffectEngine& operator=(const EffectEngine&) = delete;

  // ---- graph -------------------------------------------------------------------------------------

  EffectResult attach(const std::shared_ptr<EffectNode>& node);
  EffectResult detach(const std::shared_ptr<EffectNode>& node);
  EffectResult connect(const std::shared_ptr<EffectNode>& from, const std::shared_ptr<EffectNode>& to,
    uint32_t inputIndex = 0);
  EffectResult disconnect(const std::shared_ptr<EffectNode>& from, const std::shared_ptr<EffectNode>& to,
    uint32_t inputIndex = 0);
  EffectResult setOutput(const std::shared_ptr<EffectOutputNode>& output);

  /** Adds a secondary terminal output; the primary output from `setOutput` remains the device surface. */
  EffectResult addOutput(const std::shared_ptr<EffectOutputNode>& output);
  EffectResult removeOutput(const std::shared_ptr<EffectOutputNode>& output);

  /**
   * Recreates the platform target of every current output on the render thread.
   *
   * <p>Use this after replacing an output while the engine is running: `start()` prepares the
   * outputs once, and a node attached later otherwise has no swapchain until it fails a frame.
   */
  EffectResult prepareOutputs();

  /** Creates a pass node, attaches it, and returns it. @return null when the engine is unusable. */
  std::shared_ptr<EffectPassNode> createEffectNode(std::string_view name = {});

  // ---- life cycle --------------------------------------------------------------------------------

  EffectResult start();
  EffectResult pause();
  EffectResult resume();
  /** Stops the render thread. Nodes, pool and caches survive, so `start()` may be called again. */
  EffectResult stop();

  /**
   * Recreates the render thread and GPU device, keeping the graph and nodes.
   *
   * This is the recovery entry point after `DeviceLost`: it stops the current thread (when running),
   * releases GPU-side node resources, then starts a fresh device. Window outputs must still have their
   * native surface alive. A recovery invalidates every outstanding `Fence`.
   *
   * `execute()` already retries once through this path when it observes `DeviceLost`; a Continuous-mode
   * application does not call `execute()`, so it must call `recover()` itself when its diagnostics or
   * frame callback report the loss.
   */
  EffectResult recover();

  /**
   * Runs queued application callbacks (currently readback callbacks) on the calling thread.
   *
   * @return how many callbacks ran. Call from the application/UI thread, typically once per frame.
   */
  size_t pollCallbacks(size_t max = 64);

  /** Renders exactly one frame in `Manual` mode; in `Continuous` mode, requests an extra frame. */
  EffectResult execute();

  /**
   * Renders one frame and returns a fence that is signalled when that frame's GPU work completes.
   *
   * The handle is valid on `Success` and becomes invalid once the completion has been observed; if the
   * engine stops before the frame completes, waiting reports `kError`. Waiting must not be done on the
   * render thread.
   */
  EffectResult execute(std::shared_ptr<Fence>* frameFence);

  // ---- synchronization ---------------------------------------------------------------------------

  /** Waits until every control operation issued before this call has been seen by the render thread. */
  EffectResult flush();

  /** Flushes and then waits until the GPU has finished every frame submitted before this call. */
  EffectResult flushAndWait();

  /**
   * Waits until the frames handed to `Deferred` outputs have reached the display.
   *
   * `Deferred` deliberately lets `execute()` return before a frame is on screen, which is what makes it
   * cheap; this is the point where a caller that needs the picture visible - a screenshot, an export
   * finishing, an application exiting - synchronises. Outputs that present inline have nothing outstanding,
   * so the call returns immediately and is safe to use unconditionally.
   *
   * @param timeout how long to wait; a still-busy output makes the call return `Busy`.
   */
  EffectResult drainPresents(std::chrono::milliseconds timeout = std::chrono::milliseconds(100));

  // ---- configuration -----------------------------------------------------------------------------

  EffectResult setRenderSize(uint32_t width, uint32_t height);
  EffectResult setFrameRate(float framesPerSecond);

  /**
   * Detaches every node from the graph while keeping the device, render thread and cached programs.
   *
   * An owner that reuses one engine across item graphs (a player attaching a new item) calls this
   * before building the replacement graph. The engine is paused and flushed so no in-flight plan still
   * references a detached node.
   */
  EffectResult resetGraph();

  // ---- queries -----------------------------------------------------------------------------------

  /** The canonical ticks-per-second every `RenderTime` in this engine uses. */
  [[nodiscard]] uint32_t timeBase() const noexcept;

  /**
   * The backend the running device actually uses.
   *
   * This can differ from the requested backend when the RHI fell back (for example Vulkan requested on
   * a Windows machine whose driver cannot create a device). Callers that choose texture-origin
   * conventions or platform input paths from the backend must use this, not the configuration.
   */
  [[nodiscard]] EffectBackend backend() const noexcept;

  /** The device the running engine renders with, or null before `start()`. */
  [[nodiscard]] backend::Device* device() const noexcept;

  [[nodiscard]] bool isRunning() const noexcept;
  [[nodiscard]] bool isPaused() const noexcept;

  /**
   * @return true when the running device can import a YUV `AHardwareBuffer` through
   *         `VideoFrameInputNode::submitHardwareBuffer` / `AndroidHardwareBufferInputNode`.
   *
   * This is a device capability, so it is only meaningful after `start()` (a Continuous engine starts on
   * the first submission). When it answers false, a producer should keep the CPU plane path instead of
   * creating an ImageReader-backed AHB path. It says the import is available, not that the driver converts
   * colours correctly; a device whose driver ignores the sampler conversion still needs a CPU/GL fallback.
   */
  [[nodiscard]] bool supportsYuvHardwareBuffer() const noexcept;

  /**
   * @return true when the running outputs can queue a present instead of waiting inside `execute()`.
   *
   * `EffectPresentPolicy::Deferred` is a request, not a promise: a backend whose present blocks (WGL's
   * `SwapBuffers`, EGL and GLX) reports false and runs `Deferred` as `Blocking`. An application that needs
   * to know whether the wait actually moved should ask this rather than assume the enum took effect.
   */
  [[nodiscard]] bool supportsDeferredPresent() const noexcept;

  /** A weak-consistent snapshot; safe to call from any thread. */
  [[nodiscard]] EffectEngineStats stats() const;

  /** The engine's accumulated diagnostics, including the most recent graph validation failure. */
  [[nodiscard]] std::string diagnostics() const;

private:
  EffectEngine();

  // Shared rather than unique because attached nodes hold a weak reference to the implementation; that
  // is what lets a node outlive the engine without dereferencing freed state.
  std::shared_ptr<detail::EngineImpl> mImpl;
};

} // namespace coral

#endif // CORAL_EFFECT_ENGINE_H
