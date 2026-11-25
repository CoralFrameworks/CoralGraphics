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

#ifndef CORAL_EFFECT_INTERNAL_H
#define CORAL_EFFECT_INTERNAL_H

/**
 * @file EffectInternal.h
 *
 * The engine's implementation types. This header lives under `src/`, which is not on any consumer's
 * include path, so the public headers can name `detail::NodeImpl`, `detail::FrameInfo` and friends as
 * incomplete types while the engine sees their definitions.
 *
 * Nothing in this header reaches an installed header: `backend::` types appear only in the render-core
 * headers (`EffectRenderCore.h`), which are included by the translation units that own the device.
 */

#include <effect/EffectEngine.h>
#include <effect/EffectFrame.h>
#include <effect/EffectInputNode.h>
#include <effect/EffectNode.h>
#include <effect/EffectOutputNode.h>
#include <effect/EffectPassNode.h>
#include <effect/EffectShader.h>
#include <effect/EffectTexture.h>
#include <effect/EffectTypes.h>
#include <effect/Fence.h>

#include "effect/ShaderCacheWatch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace coral::detail {

using NodeId = uint64_t;
inline constexpr NodeId kInvalidNodeId = 0;

class RenderCore;
class GraphExecutor;
class CallbackQueue;
struct GraphPlan;

// ==================================================================================================
// Parameter layout
// ==================================================================================================

/** Slots 0 and 1 of the parameter block are the framework's: source size and target size. */
inline constexpr uint32_t kFrameworkSlotCount = 2;
/** Effect slots, `effectSlot(2) .. effectSlot(15)`. */
inline constexpr uint32_t kEffectSlotCount = 14;
/** Total `vec4` slots in one pass's parameter block. */
inline constexpr uint32_t kParameterSlotCount = kFrameworkSlotCount + kEffectSlotCount;
/** Floats in one parameter block. */
inline constexpr uint32_t kParameterComponentCount = kParameterSlotCount * 4;
/** Every pass is drawn with the same six vertices: two triangles, spelled out. */
inline constexpr uint32_t kFullTargetVertexCount = 6;

/**
 * A pass's 16 `vec4` parameter block.
 *
 * Fixed size and inline so a per-frame parameter update allocates nothing. The framework fills slots 0
 * and 1 with the source and target sizes; a shader reads effect slot `i` at block slot `i + 2`.
 */
struct ParameterBlock {
  float values[kParameterComponentCount] = {};

  void reset() noexcept { std::fill(values, values + kParameterComponentCount, 0.0f); }

  /** @return the four components of @p slot, or null when out of range. */
  [[nodiscard]] const float* slot(uint32_t index) const noexcept {
    return index < kParameterSlotCount ? values + 4 * index : nullptr;
  }
  [[nodiscard]] float* slot(uint32_t index) noexcept {
    return index < kParameterSlotCount ? values + 4 * index : nullptr;
  }
};

/** Reads `EffectValue`'s fixed-size storage; declared as a friend in the public header. */
struct EffectValueAccess {
  [[nodiscard]] static const float* data(const EffectValue& value) noexcept { return value.mValues; }
  [[nodiscard]] static uint32_t count(const EffectValue& value) noexcept { return value.mCount; }
};

/** Reads a `TextureSource`'s payload; declared as a friend in `EffectTexture.h`. */
struct TextureSourceAccess {
  [[nodiscard]] static std::shared_ptr<const EffectImage> image(const TextureSource& source) {
    return source.mImage;
  }
  [[nodiscard]] static std::shared_ptr<EffectExternalTexture> external(const TextureSource& source) {
    return source.mExternal;
  }
  [[nodiscard]] static EffectFrame frame(const TextureSource& source) { return source.mFrame; }
  [[nodiscard]] static EffectColor color(const TextureSource& source) { return source.mColor; }
  [[nodiscard]] static uint64_t nativeTexture(const TextureSource& source) {
    return source.mNativeTexture;
  }
  [[nodiscard]] static uint32_t nativeWidth(const TextureSource& source) { return source.mNativeWidth; }
  [[nodiscard]] static uint32_t nativeHeight(const TextureSource& source) { return source.mNativeHeight; }
  [[nodiscard]] static EffectPixelFormat nativeFormat(const TextureSource& source) {
    return source.mNativeFormat;
  }
};

/** One declared parameter, resolved to the effect slots it occupies. */
struct ParameterSlot {
  std::string name;
  EffectValueType type = EffectValueType::Float;
  /** Effect slot of the first component, i.e. `effectSlot(index)`. */
  uint32_t index = 0;
  /** 1, or 4 for a `Matrix4`. */
  uint32_t slotCount = 1;
};

/**
 * A shader description frozen with the parameter layout it was accepted with.
 *
 * The description's `label` is a raw pointer the caller owns, so it is not retained: the name is copied
 * into `label` here and the description's pointer is cleared before the snapshot is stored.
 */
struct ProgramSnapshot {
  EffectShaderDesc desc;
  std::string label;
  std::vector<ParameterSlot> parameters;
  /**
   * Samplers the node binds by name beyond the graph inputs.
   *
   * A `Body` shader cannot declare its own uniforms, so the framework declares one sampler per bound
   * texture and the body refers to it by that name. Adding a texture therefore re-composes the program;
   * the set is part of the program's cache key for exactly that reason.
   */
  std::vector<std::string> textureNames;
  /**
   * Bit `i` means input `i` is a platform external texture (an Android `samplerExternalOES`), so the
   * composed source must declare it with that sampler type rather than `sampler2D`. Internal only: the
   * public shader description cannot say this, because it is a property of the node feeding the port,
   * not of the shader.
   */
  uint32_t externalInputMask = 0;
  uint32_t effectSlots = 0;
  uint64_t hash = 0;

  [[nodiscard]] const ParameterSlot* find(std::string_view name) const noexcept;
};

/** The GLSL text a program needs before the backend compiles it. */
struct ComposedProgramSources {
  bool body = false;
  std::string vertexSource;
  std::string fragmentSource;
};

/**
 * Builds the framework's vertex/fragment source and applies the interface transform.
 *
 * Pure CPU: no device and no cache. Shared by the synchronous RenderProgram constructor and the M9
 * worker path, so both compile exactly the same text.
 */
bool composeProgramSources(const ProgramSnapshot& program, bool directGlsl, bool esDialect, bool topDownTargets,
                           ComposedProgramSources& sources, std::string& error);

/** One texture bound by sampler name. */
struct TextureBinding {
  std::string name;
  TextureSource source;
};

/** Per-sequence playback state. The clock and the manual override are mutually exclusive by construction. */
struct SequenceState {
  std::string name;
  EffectTextureSequenceDesc desc;
  /** Incremented when the frames change, so the render thread knows to re-upload. */
  uint32_t version = 0;
  /** True while the clock drives the index; false after `setSequenceFrame`. */
  bool timeDriven = true;
  /** True once `setSequenceTime` supplied an explicit clock; false means "use the engine's frame time". */
  bool explicitTime = false;
  RenderTime time {};
  uint32_t frameIndex = 0;
};

/**
 * The application-side state of one node.
 *
 * A copy of this is taken under the node's mutex at the start of a frame; the render thread then works
 * from the copy and never touches the application's snapshot again. That is what lets every setter be a
 * short critical section rather than a wait for the render thread.
 */
struct NodeSnapshot {
  uint32_t version = 0;
  std::string name;
  bool enabled = true;
  EffectShaderState shaderState = EffectShaderState::Empty;
  std::string diagnostics;

  /** The program currently installed on the render thread, or null when none compiled yet. */
  std::shared_ptr<const ProgramSnapshot> program;
  /** A program submitted but not yet compiled/installed. */
  std::shared_ptr<const ProgramSnapshot> pendingProgram;
  /** Values for `program`. Slots outside its layout are zero. */
  ParameterBlock values;
  /** Values for `pendingProgram`, kept apart so a failed compile cannot half-update the old program. */
  ParameterBlock pendingValues;

  std::vector<TextureBinding> textures;
  std::vector<SequenceState> sequences;

  float outputScale = 1.0f;
  EffectPixelFormat outputFormat = EffectPixelFormat::RGBA8;
  uint32_t timestampInput = 0;

  /** Only meaningful on an output node. */
  EffectOutputConfig outputConfig;
};

// ==================================================================================================
// Node internals
// ==================================================================================================

class EngineImpl;
class FrameCompletion;

/** Creates a public handle for one frame completion. */
[[nodiscard]] std::shared_ptr<Fence> makeFence(std::shared_ptr<FrameCompletion> completion);

/**
 * Produces frames for an input node on the render thread.
 *
 * Declared here and defined (with `FrameView`) in `EffectRenderCore.h`; a concrete node's `.cpp` includes
 * that header, which is how platform types stay out of the public headers.
 */
class InputRuntime;

/** Consumes the final frame for an output node on the render thread. @see InputRuntime. */
class OutputRuntime;

/** The state behind one `EffectNode`. */
struct NodeImpl {
  std::atomic<NodeId> id { kInvalidNodeId };
  EffectNodeKind kind = EffectNodeKind::Pass;

  /** Immutable after construction: the number of ports the graph understands. */
  std::atomic<uint32_t> inputCount { 0 };
  std::atomic<bool> attached { false };
  std::atomic<EffectShaderState> shaderState { EffectShaderState::Empty };
  std::atomic<bool> enabled { true };
  std::atomic<bool> directToSurface { false };

  /** Set at attach, cleared at detach. Weak, so a node may outlive its engine. Guarded by `mutex`. */
  std::weak_ptr<EngineImpl> engine;

  mutable std::mutex mutex;
  NodeSnapshot snapshot;

  /** Installed by a concrete input/output node's constructor. */
  std::shared_ptr<InputRuntime> inputRuntime;
  std::shared_ptr<OutputRuntime> outputRuntime;
};

/**
 * The engine's view of a node's private state.
 *
 * A friend struct rather than public methods: the four translation units under `src/effect/` need the
 * snapshot and the runtime, and nothing outside them does. It also keeps `EffectNode`'s public surface to
 * what the design describes.
 */
struct NodeAccess {
  static NodeImpl& impl(const EffectNode& node) noexcept { return *node.mImpl; }
  static NodeImpl& impl(EffectNode& node) noexcept { return *node.mImpl; }

  [[nodiscard]] static NodeSnapshot snapshot(const EffectNode& node) {
    NodeImpl& state = impl(node);
    const std::lock_guard<std::mutex> lock(state.mutex);
    return state.snapshot;
  }

  [[nodiscard]] static std::string name(const EffectNode& node) {
    NodeImpl& state = impl(node);
    const std::lock_guard<std::mutex> lock(state.mutex);
    return state.snapshot.name;
  }

  [[nodiscard]] static std::shared_ptr<InputRuntime> inputRuntime(const EffectNode& node) {
    NodeImpl& state = impl(node);
    const std::lock_guard<std::mutex> lock(state.mutex);
    return state.inputRuntime;
  }

  [[nodiscard]] static std::shared_ptr<OutputRuntime> outputRuntime(const EffectNode& node) {
    NodeImpl& state = impl(node);
    const std::lock_guard<std::mutex> lock(state.mutex);
    return state.outputRuntime;
  }

  static void setInputRuntime(EffectNode& node, std::shared_ptr<InputRuntime> runtime) {
    NodeImpl& state = impl(node);
    const std::lock_guard<std::mutex> lock(state.mutex);
    state.inputRuntime = std::move(runtime);
  }

  static void setOutputRuntime(EffectNode& node, std::shared_ptr<OutputRuntime> runtime) {
    NodeImpl& state = impl(node);
    const std::lock_guard<std::mutex> lock(state.mutex);
    state.outputRuntime = std::move(runtime);
  }

  static void setDiagnostics(EffectNode& node, std::string text) {
    NodeImpl& state = impl(node);
    const std::lock_guard<std::mutex> lock(state.mutex);
    state.snapshot.diagnostics = std::move(text);
  }

  /** Publishes the shader's lifecycle state from the render thread. */
  static void setShaderState(EffectNode& node, const EffectShaderState value) {
    impl(node).shaderState.store(value, std::memory_order_release);
  }

  /**
   * Installs @p program as the node's active one.
   *
   * Called on the render thread after a successful compile: the pending description and its parameter
   * block become the active ones, so the application's next `setParameter` writes the layout the render
   * thread is now using. Doing this on the application's snapshot is what keeps the two halves in step.
   */
  static void promote(EffectNode& node, const std::shared_ptr<const ProgramSnapshot>& program) {
    NodeImpl& state = impl(node);
    const std::lock_guard<std::mutex> lock(state.mutex);
    if (state.snapshot.pendingProgram == program) {
      state.snapshot.program = program;
      state.snapshot.values = state.snapshot.pendingValues;
      state.snapshot.pendingProgram.reset();
      state.snapshot.pendingValues.reset();
      state.snapshot.shaderState = EffectShaderState::Ready;
      ++state.snapshot.version;
    }
    state.shaderState.store(EffectShaderState::Ready, std::memory_order_release);
  }

  /**
   * Records that @p program failed to compile and drops it from the snapshot.
   *
   * A rejected description must not stay pending. While it is pending, `EffectPassNode::setParameter`
   * writes its block - and the executor, having already failed that description once, never compiles it
   * again, so the write is silently dropped and the pass keeps drawing its previous program with its
   * previous values. Dropping it returns the node to the program that is actually installed, which is
   * also the one the application's next parameter write has to address.
   */
  static void reject(EffectNode& node, const std::shared_ptr<const ProgramSnapshot>& program) {
    NodeImpl& state = impl(node);
    const std::lock_guard<std::mutex> lock(state.mutex);
    if (state.snapshot.pendingProgram == program) {
      state.snapshot.pendingProgram.reset();
      state.snapshot.pendingValues.reset();
      ++state.snapshot.version;
    }
    state.snapshot.shaderState = EffectShaderState::Failed;
    state.shaderState.store(EffectShaderState::Failed, std::memory_order_release);
  }

  /** @return the engine this node is attached to, or null once it has been detached. */
  [[nodiscard]] static std::shared_ptr<EngineImpl> engine(const EffectNode& node) {
    NodeImpl& state = impl(node);
    const std::lock_guard<std::mutex> lock(state.mutex);
    return state.engine.lock();
  }

  /** Binds @p node to @p engine under @p id. Called by `EngineImpl::attach`. */
  static void bind(EffectNode& node, const NodeId id, std::shared_ptr<EngineImpl> engine, std::string name) {
    NodeImpl& state = impl(node);
    const std::lock_guard<std::mutex> lock(state.mutex);
    state.id.store(id, std::memory_order_relaxed);
    state.engine = std::move(engine);
    if (!name.empty()) state.snapshot.name = std::move(name);
    state.snapshot.enabled = state.enabled.load(std::memory_order_relaxed);
    ++state.snapshot.version;
    state.attached.store(true, std::memory_order_release);
  }

  /** Assigns the id the graph will know the node by, before `bind` publishes the attachment. */
  static void assignId(EffectNode& node, const NodeId id) {
    impl(node).id.store(id, std::memory_order_relaxed);
  }

  /** Unbinds @p node, leaving its snapshot intact so the application may re-attach it elsewhere. */
  static void unbind(EffectNode& node) {
    NodeImpl& state = impl(node);
    const std::lock_guard<std::mutex> lock(state.mutex);
    state.attached.store(false, std::memory_order_release);
    state.engine.reset();
    state.id.store(kInvalidNodeId, std::memory_order_relaxed);
    ++state.snapshot.version;
  }
};

// ==================================================================================================
// Frame metadata
// ==================================================================================================

/** What `EffectFrame` exposes. Immutable once published. */
struct FrameInfo {
  uint64_t id = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  EffectPixelFormat format = EffectPixelFormat::RGBA8;
  EffectColorSpace colorSpace = EffectColorSpace::SRGB;
  RenderTime timestamp {};
};

struct FrameAccess {
  static EffectFrame make(uint64_t id, uint32_t width, uint32_t height, EffectPixelFormat format,
                          EffectColorSpace colorSpace, RenderTime timestamp) {
    auto info = std::make_shared<FrameInfo>();
    info->id = id;
    info->width = width;
    info->height = height;
    info->format = format;
    info->colorSpace = colorSpace;
    info->timestamp = timestamp;
    EffectFrame frame;
    frame.mInfo = std::move(info);
    return frame;
  }
};

// ==================================================================================================
// Graph
// ==================================================================================================

/** One input connection: `from`'s output feeds `to`'s port @p inputIndex. */
struct Edge {
  NodeId from = kInvalidNodeId;
  NodeId to = kInvalidNodeId;
  uint32_t inputIndex = 0;

  friend bool operator==(const Edge& a, const Edge& b) noexcept {
    return a.from == b.from && a.to == b.to && a.inputIndex == b.inputIndex;
  }
};

/**
 * The node graph.
 *
 * The validator is a DAG validator: ports must exist, input counts must match, cycles are refused, and
 * fan-out is allowed. The v1 executor additionally requires a linear chain; that check belongs to the
 * executor, not here, so the data structure is already the one v2 needs.
 *
 * Not thread-safe on its own: `EngineImpl` owns the only instance and guards it with its mutex.
 */
class Graph {
public:
  EffectResult attach(const std::shared_ptr<EffectNode>& node, std::string& diagnostic);
  EffectResult detach(NodeId node, std::string& diagnostic);
  EffectResult connect(NodeId from, NodeId to, uint32_t inputIndex, std::string& diagnostic);
  EffectResult disconnect(NodeId from, NodeId to, uint32_t inputIndex, std::string& diagnostic);
  EffectResult setOutput(NodeId output, std::string& diagnostic);
  /** Adds a secondary terminal output; the primary output remains `output()`. */
  EffectResult addOutput(NodeId output, std::string& diagnostic);
  EffectResult removeOutput(NodeId output, std::string& diagnostic);

  [[nodiscard]] std::shared_ptr<EffectNode> find(NodeId node) const;
  [[nodiscard]] bool contains(NodeId node) const { return find(node) != nullptr; }
  [[nodiscard]] NodeId output() const noexcept { return mOutput; }
  [[nodiscard]] std::vector<NodeId> outputs() const;
  [[nodiscard]] const std::vector<Edge>& edges() const noexcept { return mEdges; }
  [[nodiscard]] const std::map<NodeId, std::shared_ptr<EffectNode>>& nodeMap() const noexcept { return mNodes; }
  [[nodiscard]] size_t size() const noexcept { return mNodes.size(); }

  /** Nodes in ascending id order, which is the deterministic order every traversal starts from. */
  [[nodiscard]] std::vector<std::shared_ptr<EffectNode>> nodes() const;

  /**
   * Checks the structural rules that must hold before a frame can be rendered: an output exists, every
   * input port is filled, every node lies on a path from some input to the output, and no cycle exists.
   */
  EffectResult validate(std::string& diagnostic) const;

  /** @return the number of incoming edges whose port index is >= @p inputCount. */
  [[nodiscard]] uint32_t countEdgesAtOrBeyond(NodeId node, uint32_t inputCount) const;

private:
  [[nodiscard]] bool reaches(NodeId from, NodeId target) const;

  std::map<NodeId, std::shared_ptr<EffectNode>> mNodes;
  std::vector<Edge> mEdges;
  NodeId mOutput = kInvalidNodeId;
  std::vector<NodeId> mExtraOutputs;
};

// ==================================================================================================
// Engine
// ==================================================================================================

/**
 * The engine's implementation.
 *
 * M1 owns the graph, node attachment and the snapshot protocol. The render thread, command queue,
 * render core and frame pool are added by M2/M3; their hooks are declared here so the public facade does
 * not change when they land.
 */
class EngineImpl final : public std::enable_shared_from_this<EngineImpl> {
public:
  explicit EngineImpl(const EffectEngineConfig& config);
  ~EngineImpl();

  EngineImpl(const EngineImpl&) = delete;
  EngineImpl& operator=(const EngineImpl&) = delete;

  // ---- configuration -----------------------------------------------------------------------------

  [[nodiscard]] uint32_t timeBase() const noexcept { return mTimeBase; }
  [[nodiscard]] EffectRenderMode renderMode() const noexcept { return mRenderMode; }

  // ---- graph -------------------------------------------------------------------------------------

  EffectResult attach(const std::shared_ptr<EffectNode>& node);
  EffectResult detach(const std::shared_ptr<EffectNode>& node);
  EffectResult connect(const std::shared_ptr<EffectNode>& from, const std::shared_ptr<EffectNode>& to,
                       uint32_t inputIndex);
  EffectResult disconnect(const std::shared_ptr<EffectNode>& from, const std::shared_ptr<EffectNode>& to,
                          uint32_t inputIndex);
  EffectResult setOutput(const std::shared_ptr<EffectOutputNode>& output);
  EffectResult addOutput(const std::shared_ptr<EffectOutputNode>& output);
  EffectResult removeOutput(const std::shared_ptr<EffectOutputNode>& output);
  /** Detaches and unbinds every node while keeping the device, render thread and program caches. */
  EffectResult resetGraph();
  /** Creates the platform target of every current output on the render thread. */
  EffectResult prepareOutputs();

  /** @return `InvalidArgument` when a connection already occupies a port >= @p inputCount. */
  EffectResult checkInputPorts(NodeId node, uint32_t inputCount, std::string& diagnostic) const;

  // ---- life cycle --------------------------------------------------------------------------------

  EffectResult start();
  EffectResult pause();
  EffectResult resume();
  EffectResult stop();
  EffectResult recover();
  size_t pollCallbacks(size_t max);
  EffectResult execute(std::shared_ptr<Fence>* frameFence);

  EffectResult flush();
  EffectResult flushAndWait();

  void setExternalTextureResolver(std::shared_ptr<EffectExternalTextureResolver> resolver);
  void setOnDeviceReady(std::function<void(backend::Device&)> callback);
  void setOnBeforeFrame(std::function<void(backend::Device&)> callback);
  void setOnAfterFrame(std::function<void(backend::Device&)> callback);
  void setOnFrameSubmitted(
    std::function<void(backend::Device&, std::shared_ptr<Fence>)> callback);
  EffectResult setRenderSize(uint32_t width, uint32_t height);
  EffectResult setFrameRate(float framesPerSecond);

  [[nodiscard]] bool isRunning() const noexcept { return mRunning.load(std::memory_order_acquire); }
  [[nodiscard]] bool isPaused() const noexcept { return mPaused.load(std::memory_order_acquire); }
  /** Valid after `start()`; false before the render thread has created a device. */
  [[nodiscard]] bool supportsYuvHardwareBuffer() const noexcept {
    return mSupportsYuvHardwareBuffer.load(std::memory_order_acquire);
  }

  /** Whether the running outputs can queue a present instead of waiting inside `execute()`. */
  [[nodiscard]] bool supportsDeferredPresent() const noexcept {
    return mSupportsDeferredPresent.load(std::memory_order_acquire);
  }

  /** Blocks until the frames handed to `Deferred` outputs have reached the display. */
  EffectResult drainPresents(std::chrono::milliseconds timeout);

  [[nodiscard]] EffectEngineStats stats() const;
  [[nodiscard]] std::string diagnostics() const;

  // ---- diagnostics -------------------------------------------------------------------------------

  void report(EffectSeverity severity, const std::string& message);
  void setDiagnostic(std::string message);

  // ---- render-thread internals -------------------------------------------------------------------

  /** Runs until stopped. Called on the render thread only. */
  void renderLoop();

  /** Builds a frame's execution plan. @return `Unsupported` for a fan-out graph. */
  EffectResult buildPlan(GraphPlan& plan, std::string& error) const;

  [[nodiscard]] RenderCore* core() const noexcept { return mCore.get(); }
  [[nodiscard]] uint32_t renderWidth() const noexcept { return mRenderWidth.load(std::memory_order_relaxed); }
  [[nodiscard]] uint32_t renderHeight() const noexcept { return mRenderHeight.load(std::memory_order_relaxed); }
  [[nodiscard]] float frameRate() const noexcept { return mFrameRate.load(std::memory_order_relaxed); }
  [[nodiscard]] uint64_t frameIndex() const noexcept { return mFrameIndex; }

  /** @return the canonical time for the frame being rendered. */
  [[nodiscard]] RenderTime frameTimestamp() const noexcept;

  [[nodiscard]] Graph& graph() noexcept { return mGraph; }
  [[nodiscard]] const Graph& graph() const noexcept { return mGraph; }
  [[nodiscard]] std::mutex& mutex() const noexcept { return mMutex; }

  /** Publishes this frame's counters. Called on the render thread. */
  void publishStats(uint32_t droppedDelta);

private:
  struct Command {
    enum class Kind : uint8_t {
      Flush,
      WaitIdle,
      Stop,
      Pause,
      Resume,
      RequestFrame,
      SetRenderSize,
      SetFrameRate,
      PrepareOutputs,
      Retire,
    };

    Kind kind = Kind::Flush;
    uint32_t width = 0;
    uint32_t height = 0;
    float frameRate = 0.0f;
    std::shared_ptr<std::promise<EffectResult>> done;
    /** Set only for `RequestFrame` when the caller asked for a per-frame completion. */
    std::shared_ptr<std::promise<std::shared_ptr<FrameCompletion>>> fenceDone;
    /** Set only for `Retire`: GPU resources released after the submission ring drains. */
    std::function<void()> retire;
  };

  /** One queued manual frame: its result promise and the optional completion promise. */
  struct FrameRequest {
    std::shared_ptr<std::promise<EffectResult>> done;
    std::shared_ptr<std::promise<std::shared_ptr<FrameCompletion>>> fenceDone;
  };

  /** @return a completed future for @p kind, waiting for the render thread's answer. */
  EffectResult submitCommand(Command command);

  /** Queues a command without waiting; used by detach to retire runtime resources. */
  bool postCommand(Command command);

  /** Renders one frame from the current graph. Called on the render thread. */
  EffectResult renderFrame(std::string& error, std::shared_ptr<FrameCompletion>* completion = nullptr);

  /** Tears down and recreates the device on the render thread after a Continuous-mode DeviceLost. */
  bool recoverOnRenderThread();

  /** Completes every queued request with @p result, so a stopping thread cannot leave a waiter blocked. */
  void failPendingRequests(EffectResult result);

  /** Runs on the render thread; clears the thread identity and publishes the explicit exit event. */
  void markRenderThreadExited();

  /**
   * The body of `start()`/`stop()`/`recover()`, called with `mLifecycleMutex` held.
   *
   * Serializing the transitions is what stops a `stop()` that observes no thread from returning while a
   * concurrent `start()` is about to install one. Neither helper may be called from the render thread:
   * `stop()` would have to wait for its own caller to exit.
   */
  EffectResult startLocked();
  EffectResult stopLocked();

  /**
   * Initializes `mCore`/`mExecutor` on the calling thread for `externalDrive` mode.
   *
   * The render host owns the device and calls `execute()`; there is no engine render thread to wait
   * on, so this is the synchronous counterpart of the render loop's startup path.
   */
  EffectResult initializeExternal();
  /** Drains control commands queued by graph/parameter setters on the external-driving thread. */
  EffectResult processCommandsExternal();

  uint32_t mTimeBase = 60;
  EffectRenderMode mRenderMode = EffectRenderMode::Manual;
  EffectEngineConfig mConfig;

  /** Watches the shader cache across starts; see `ShaderCacheWatch`. */
  ShaderCacheWatch mShaderCacheWatch;
  /** Whether the watch's state has been read back from the cache directory, which happens once per engine. */
  bool mShaderCacheWatchRestored = false;
  /** The cache counters when the current `start()` began, so its own hits and misses can be told apart. */
  ShaderCacheSample mShaderCacheBeforeStart;

  mutable std::mutex mMutex;
  Graph mGraph;

  std::atomic<uint32_t> mRenderWidth { 0 };
  std::atomic<uint32_t> mRenderHeight { 0 };
  std::atomic<float> mFrameRate { 0.0f };

  mutable std::mutex mDiagnosticsMutex;
  std::string mDiagnostics;
  /** The last failed frame's reason, so a steady-state failure is reported once instead of every frame. */
  std::string mLastFrameError;

  mutable std::mutex mStatsMutex;
  EffectEngineStats mStats;

  std::atomic<bool> mRunning { false };
  std::atomic<bool> mPaused { false };
  std::atomic<bool> mSupportsYuvHardwareBuffer { false };
  std::atomic<bool> mSupportsDeferredPresent { false };
  std::atomic<uint64_t> mNextNodeId { 1 };

  // ---- render thread -----------------------------------------------------------------------------

  /**
   * Serializes `start()`/`stop()`/`recover()`.
   *
   * Separate from `mThreadMutex` because a transition waits for the render thread (initialization or
   * exit) and the render loop takes `mThreadMutex` on every iteration: holding that mutex while waiting
   * would deadlock. Lock order is lifecycle-then-thread.
   */
  std::mutex mLifecycleMutex;

  mutable std::mutex mThreadMutex;
  std::condition_variable mCommandCv;
  /** Signalled when the render thread function has returned, before its thread handle is reaped. */
  std::condition_variable mRenderThreadExitedCv;
  std::deque<Command> mCommands;
  std::deque<FrameRequest> mPendingRequests;
  std::thread mThread;
  /** Guarded by `mThreadMutex`; set by the render thread at entry and cleared when it publishes exit. */
  std::thread::id mRenderThreadId;

  /**
   * Guarded by `mThreadMutex`. True when no render thread is running.
   *
   * `stop()` waits for this rather than calling `join()` alone: a thread that has not yet reached its
   * loop must still be observed leaving it, and a handle may be non-joinable while the thread is alive
   * in every path that reaps it.
   */
  bool mRenderThreadExited = true;
  std::atomic<bool> mStopRequested { false };

  std::mutex mInitMutex;
  std::condition_variable mInitCv;
  bool mInitialized = false;
  EffectResult mInitResult = EffectResult::Success;

  std::unique_ptr<RenderCore> mCore;
  std::shared_ptr<EffectExternalTextureResolver> mExternalTextureResolver;
  std::function<void(backend::Device&)> mOnBeforeFrame;
  std::function<void(backend::Device&)> mOnAfterFrame;
  std::function<void(backend::Device&, std::shared_ptr<Fence>)> mOnFrameSubmitted;
  std::function<void(backend::Device&)> mOnDeviceReady;
  std::shared_ptr<CallbackQueue> mCallbackQueue;
  std::unique_ptr<GraphExecutor> mExecutor;
  uint64_t mFrameIndex = 0;

  EffectDiagnosticsCallback mDiagnosticsCallback = nullptr;
  void* mDiagnosticsUserData = nullptr;
};

// ==================================================================================================
// Helpers shared by the node translation units
// ==================================================================================================

/** @return a hash of everything that decides a compiled program: source, authoring, inputs and layout. */
[[nodiscard]] uint64_t hashProgram(const EffectShaderDesc& shader, const std::vector<ParameterSlot>& parameters,
                                   const std::vector<std::string>& textureNames,
                                   uint32_t externalInputMask = 0) noexcept;

/** @return the FNV-1a hash of @p text. */
[[nodiscard]] uint64_t hashText(std::string_view text, uint64_t seed = 1469598103934665603ull) noexcept;

/** @return @p value folded into @p seed. */
[[nodiscard]] uint64_t hashValue(uint64_t seed, uint64_t value) noexcept;

/**
 * Builds the validated program snapshot for @p shader.
 *
 * @param textureNames the samplers the node binds by name, in binding order.
 * @param diagnostic   receives the reason when the description is refused.
 * @return null when the description is structurally invalid.
 */
[[nodiscard]] std::shared_ptr<const ProgramSnapshot> buildProgramSnapshot(const EffectShaderDesc& shader,
                                                                         const std::vector<std::string>& textureNames,
                                                                         std::string& diagnostic);

/** Writes @p value into @p block at @p parameter's slots. */
bool writeParameter(ParameterBlock& block, const ParameterSlot& parameter, const EffectValue& value,
                    std::string& diagnostic);

/** Looks @p name up in @p program. */
[[nodiscard]] const ParameterSlot* findParameter(const std::shared_ptr<const ProgramSnapshot>& program,
                                                std::string_view name) noexcept;

} // namespace coral::detail

#endif // CORAL_EFFECT_INTERNAL_H
