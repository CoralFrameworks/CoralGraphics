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
 * @file EffectEngine.cpp
 *
 * The engine facade, the graph facade methods, and the render thread that owns the device.
 *
 * ## Locking
 *
 * `EngineImpl::mMutex` guards the graph. `mThreadMutex` guards the command queue and the thread handle.
 * Node snapshots have their own mutexes. The order is engine-then-node only: `attach`/`detach` take
 * `mMutex` and then a node's mutex through `NodeAccess`; a setter takes only the node's mutex, and
 * `setShader` releases it before asking the engine about ports. The command queue is never taken while
 * `mMutex` is held, so the render thread may build a plan (taking `mMutex`) without racing a submitter.
 *
 * ## The frame loop
 *
 * The render thread is the only place a `RenderCore` exists. `start()` spawns it - the thread function owns a
 * strong reference to the implementation, so the object cannot be destroyed while the loop runs - and waits for
 * the device to come up; `stop()` asks it to exit, waits for the explicit `mRenderThreadExited` event and then
 * reaps the handle. `execute()` is a request-frame command with a future, which is what makes Manual mode
 * synchronous from the caller's point of view without the caller ever touching the device.
 */

#include "EffectExecutor.h"
#include "EffectRenderCore.h"
#include "EffectInternal.h"

#include <backend/ShaderCompiler.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <mutex>
#include <unordered_set>

#include <utility>

namespace coral {

namespace detail {

namespace {

std::string prefix(const EffectSeverity severity, const std::string& text) {
  switch (severity) {
    case EffectSeverity::Info:
      return text;
    case EffectSeverity::Warning:
      return "warning: " + text;
    case EffectSeverity::Error:
      return "error: " + text;
  }
  return text;
}

std::string labelOf(const std::shared_ptr<EffectNode>& node) {
  if (node == nullptr) return "<null>";
  const std::string name(NodeAccess::name(*node));
  return name.empty() ? "node #" + std::to_string(node->id()) : "'" + name + "'";
}

} // namespace anonymous

EngineImpl::EngineImpl(const EffectEngineConfig& config) : mConfig(config) {
  mCallbackQueue = std::make_shared<CallbackQueue>();
  mTimeBase = config.timeBase != 0 ? config.timeBase : RenderTime::kDefaultTimeBase;
  mRenderMode = config.renderMode;
  mRenderWidth.store(config.renderWidth, std::memory_order_relaxed);
  mRenderHeight.store(config.renderHeight, std::memory_order_relaxed);
  mFrameRate.store(std::isfinite(config.frameRate) && config.frameRate > 0.0f ? config.frameRate : 0.0f,
    std::memory_order_relaxed);
  mDiagnosticsCallback = config.diagnosticsCallback;
  mDiagnosticsUserData = config.diagnosticsUserData;
}

EngineImpl::~EngineImpl() {
  // The render thread's entry lambda owns a strong reference to this object, so this destructor cannot run
  // while that thread is alive. A thread handle can still be installed when the thread ended on its own -
  // an initialization failure, or a stop requested from a callback on the render thread - and destroying a
  // joinable `std::thread` would terminate the process. Reap it here instead.
  std::thread handle;
  {
    const std::lock_guard<std::mutex> lock(mThreadMutex);
    handle = std::move(mThread);
  }
  if (handle.joinable()) {
    if (handle.get_id() == std::this_thread::get_id()) {
      // The last reference was released on the render thread at the very end of its function; there is
      // nothing left to wait for and joining ourselves would throw.
      handle.detach();
    } else {
      mStopRequested.store(true, std::memory_order_release);
      mCommandCv.notify_all();
      handle.join();
    }
  }

  // A node may outlive the engine; its handle must stop claiming an attachment that no longer has a
  // runtime behind it. Detaching one at a time lets `Graph::detach` maintain the output and edges.
  std::vector<std::shared_ptr<EffectNode>> nodes;
  {
    const std::lock_guard<std::mutex> lock(mMutex);
    nodes = mGraph.nodes();
    std::string ignored;
    for (const std::shared_ptr<EffectNode>& node: nodes) {
      if (node != nullptr) mGraph.detach(node->id(), ignored);
    }
  }
  for (const std::shared_ptr<EffectNode>& node: nodes) {
    if (node != nullptr) NodeAccess::unbind(*node);
  }
}

// ==================================================================================================
// Graph
// ==================================================================================================

EffectResult EngineImpl::attach(const std::shared_ptr<EffectNode>& node) {
  if (node == nullptr) {
    setDiagnostic("attach: the node is null");
    return EffectResult::InvalidArgument;
  }
  NodeImpl& state = NodeAccess::impl(*node);
  if (state.attached.load(std::memory_order_acquire)) {
    setDiagnostic("attach: the node is already attached");
    return EffectResult::InvalidArgument;
  }

  const NodeId id = mNextNodeId.fetch_add(1, std::memory_order_relaxed);
  NodeAccess::assignId(*node, id);

  std::string diagnostic;
  EffectResult result;
  {
    const std::lock_guard<std::mutex> lock(mMutex);
    result = mGraph.attach(node, diagnostic);
  }
  if (result != EffectResult::Success) {
    NodeAccess::assignId(*node, kInvalidNodeId);
    setDiagnostic("attach: " + diagnostic);
    return result;
  }

  if (const std::shared_ptr<OutputRuntime> runtime = NodeAccess::outputRuntime(*node)) {
    runtime->setCallbackQueue(mCallbackQueue);
  }
  NodeAccess::bind(*node, id, shared_from_this(), std::string(node->name()));
  return EffectResult::Success;
}

EffectResult EngineImpl::detach(const std::shared_ptr<EffectNode>& node) {
  if (node == nullptr) {
    setDiagnostic("detach: the node is null");
    return EffectResult::InvalidArgument;
  }
  if (!NodeAccess::impl(*node).attached.load(std::memory_order_acquire)) {
    setDiagnostic("detach: the node is not attached");
    return EffectResult::NotAttached;
  }

  std::string diagnostic;
  EffectResult result;
  {
    const std::lock_guard<std::mutex> lock(mMutex);
    result = mGraph.detach(node->id(), diagnostic);
  }
  if (result != EffectResult::Success) {
    setDiagnostic("detach: " + diagnostic);
    return result;
  }

  // The node may be destroyed by the caller immediately after detach. Its runtime can own imported
  // textures and staging buffers, so hand the release to the render thread: it waits for the ring to
  // drain and only then calls releaseResources(). The command owns the runtime through the lambda, so a
  // node destructor cannot free GPU state while a submission still samples it.
  const std::shared_ptr<InputRuntime> inputRuntime = NodeAccess::inputRuntime(*node);
  const std::shared_ptr<OutputRuntime> outputRuntime = NodeAccess::outputRuntime(*node);
  if (inputRuntime != nullptr || outputRuntime != nullptr) {
    Command command;
    command.kind = Command::Kind::Retire;
    command.retire = [inputRuntime, outputRuntime] {
      if (inputRuntime != nullptr) inputRuntime->releaseResources();
      if (outputRuntime != nullptr) outputRuntime->releaseResources();
    };
    if (!postCommand(std::move(command))) {
      if (inputRuntime != nullptr) inputRuntime->releaseResources();
      if (outputRuntime != nullptr) outputRuntime->releaseResources();
    }
  }

  NodeAccess::unbind(*node);
  return EffectResult::Success;
}

EffectResult EngineImpl::connect(const std::shared_ptr<EffectNode>& from,
  const std::shared_ptr<EffectNode>& to,
  const uint32_t inputIndex) {
  if (from == nullptr || to == nullptr) {
    setDiagnostic("connect: a node is null");
    return EffectResult::InvalidArgument;
  }
  std::string diagnostic;
  EffectResult result;
  {
    const std::lock_guard<std::mutex> lock(mMutex);
    result = mGraph.connect(from->id(), to->id(), inputIndex, diagnostic);
  }
  if (result != EffectResult::Success) setDiagnostic("connect: " + diagnostic);
  return result;
}

EffectResult EngineImpl::disconnect(const std::shared_ptr<EffectNode>& from,
  const std::shared_ptr<EffectNode>& to,
  const uint32_t inputIndex) {
  if (from == nullptr || to == nullptr) {
    setDiagnostic("disconnect: a node is null");
    return EffectResult::InvalidArgument;
  }
  std::string diagnostic;
  EffectResult result;
  {
    const std::lock_guard<std::mutex> lock(mMutex);
    result = mGraph.disconnect(from->id(), to->id(), inputIndex, diagnostic);
  }
  if (result != EffectResult::Success) setDiagnostic("disconnect: " + diagnostic);
  return result;
}

EffectResult EngineImpl::setOutput(const std::shared_ptr<EffectOutputNode>& output) {
  if (output == nullptr) {
    setDiagnostic("setOutput: the node is null");
    return EffectResult::InvalidArgument;
  }
  std::string diagnostic;
  EffectResult result;
  {
    const std::lock_guard<std::mutex> lock(mMutex);
    result = mGraph.setOutput(output->id(), diagnostic);
  }
  if (result != EffectResult::Success) setDiagnostic("setOutput: " + diagnostic);
  return result;
}

EffectResult EngineImpl::addOutput(const std::shared_ptr<EffectOutputNode>& output) {
  if (output == nullptr) {
    setDiagnostic("addOutput: the node is null");
    return EffectResult::InvalidArgument;
  }
  std::string diagnostic;
  EffectResult result;
  {
    const std::lock_guard<std::mutex> lock(mMutex);
    result = mGraph.addOutput(output->id(), diagnostic);
  }
  if (result != EffectResult::Success) setDiagnostic("addOutput: " + diagnostic);
  return result;
}

EffectResult EngineImpl::removeOutput(const std::shared_ptr<EffectOutputNode>& output) {
  if (output == nullptr) {
    setDiagnostic("removeOutput: the node is null");
    return EffectResult::InvalidArgument;
  }
  std::string diagnostic;
  EffectResult result;
  {
    const std::lock_guard<std::mutex> lock(mMutex);
    result = mGraph.removeOutput(output->id(), diagnostic);
  }
  if (result != EffectResult::Success) {
    setDiagnostic("removeOutput: " + diagnostic);
    return result;
  }
  if (const std::shared_ptr<OutputRuntime> runtime = NodeAccess::outputRuntime(*output)) {
    Command command;
    command.kind = Command::Kind::Retire;
    command.retire = [runtime] { runtime->releaseResources(); };
    if (!postCommand(std::move(command))) runtime->releaseResources();
  }
  return EffectResult::Success;
}

EffectResult EngineImpl::resetGraph() {
  if (!isRunning()) return EffectResult::NotRunning;

  const bool wasPaused = isPaused();
  if (!wasPaused) pause();
  // No in-flight plan may still name a node that is about to be unbound.
  flushAndWait();

  std::vector<std::shared_ptr<EffectNode>> nodes;
  {
    const std::lock_guard<std::mutex> lock(mMutex);
    nodes = mGraph.nodes();
    std::string ignored;
    for (const std::shared_ptr<EffectNode>& node: nodes) {
      if (node != nullptr) mGraph.detach(node->id(), ignored);
    }
  }
  for (const std::shared_ptr<EffectNode>& node: nodes) {
    if (node != nullptr) NodeAccess::unbind(*node);
  }
  if (!wasPaused) resume();
  return EffectResult::Success;
}

EffectResult EngineImpl::prepareOutputs() {
  if (!isRunning()) return EffectResult::NotRunning;
  Command command;
  command.kind = Command::Kind::PrepareOutputs;
  return submitCommand(std::move(command));
}

EffectResult EngineImpl::checkInputPorts(const NodeId node, const uint32_t inputCount,
  std::string& diagnostic) const {
  const std::lock_guard<std::mutex> lock(mMutex);
  if (mGraph.countEdgesAtOrBeyond(node, inputCount) == 0) return EffectResult::Success;
  diagnostic = "the shader declares " + std::to_string(inputCount) +
               " input(s), which would orphan a connection already made to this node";
  return EffectResult::InvalidArgument;
}

EffectResult EngineImpl::buildPlan(GraphPlan& plan, std::string& error) const {
  {
    const std::lock_guard<std::mutex> lock(mMutex);
    std::string diagnostic;
    const EffectResult valid = mGraph.validate(diagnostic);
    if (valid != EffectResult::Success) {
      error = diagnostic;
      return valid;
    }
    plan.nodes = mGraph.nodeMap();
    plan.edges = mGraph.edges();
    plan.output = mGraph.output();
    plan.outputs = mGraph.outputs();
  }

  // The executor walks a DAG: a node may feed several consumers, and the plan executes it once. Terminals
  // are outputs; the validator already rejected an output with a downstream consumer.
  std::map<NodeId, uint32_t> consumers;
  for (const Edge& edge: plan.edges) ++consumers[edge.from];
  for (const NodeId output: plan.outputs) {
    const auto it = consumers.find(output);
    if (it != consumers.end() && it->second != 0) {
      error = labelOf(plan.nodes.at(output)) + " is an output but has " + std::to_string(it->second) + " consumer(s)";
      return EffectResult::InvalidArgument;
    }
  }

  // Post-order depth-first from the output: a node is appended after everything it depends on, so the
  // reversed list is a valid execution order. Edges are sorted by port so the order is deterministic.
  std::unordered_set<NodeId> visited;
  std::function<void(NodeId)> visit = [&](const NodeId id) {
    if (!visited.insert(id).second) return;
    std::vector<Edge> incoming;
    for (const Edge& edge: plan.edges) {
      if (edge.to == id) incoming.push_back(edge);
    }
    std::sort(incoming.begin(), incoming.end(), [](const Edge& a, const Edge& b) {
      if (a.inputIndex != b.inputIndex) return a.inputIndex < b.inputIndex;
      return a.from < b.from;
    });
    for (const Edge& edge: incoming) visit(edge.from);
    plan.order.push_back(id);
  };
  for (const NodeId output: plan.outputs) visit(output);
  if (visited.size() != plan.nodes.size()) {
    error = "the graph contains a node that does not reach the output";
    return EffectResult::InvalidArgument;
  }
  // Post-order appends a node after everything it depends on, so the list is already in execution order.
  return EffectResult::Success;
}

// ==================================================================================================
// Life cycle
// ==================================================================================================

EffectResult EngineImpl::start() {
  // One transition at a time: without this a `stop()` that has not yet seen the thread can return before
  // a concurrent `start()` installs one, leaving the engine running after a successful stop.
  const std::lock_guard<std::mutex> lifecycle(mLifecycleMutex);
  return startLocked();
}

EffectResult EngineImpl::startLocked() {
  bool reap = false;
  {
    const std::lock_guard<std::mutex> lock(mThreadMutex);
    // A handle whose thread is still running is an engine that is already started. A handle whose thread
    // has published its exit is reaped below; returning "already started" would keep a dead handle
    // installed, so the next `stop()` would have nothing to wait for.
    if (mThread.joinable() && !mRenderThreadExited) return EffectResult::Success;
    reap = mThread.joinable();
  }
  if (reap) {
    const EffectResult reaped = stopLocked();
    if (reaped != EffectResult::Success) return reaped;
  }

  if (renderWidth() == 0 || renderHeight() == 0) {
    setDiagnostic("start: the render size is zero; set renderWidth/renderHeight or call setRenderSize");
    return EffectResult::InvalidArgument;
  }

  GraphPlan plan;
  std::string diagnostic;
  const EffectResult validated = buildPlan(plan, diagnostic);
  if (validated != EffectResult::Success) {
    setDiagnostic("start: " + diagnostic);
    return validated;
  }

  if (mConfig.externalDrive) {
    if (mConfig.externalDevice == nullptr) {
      setDiagnostic("start: externalDrive requires an externalDevice");
      return EffectResult::InvalidArgument;
    }
    if (mRunning.load(std::memory_order_acquire)) return EffectResult::Success;
    {
      const std::lock_guard<std::mutex> lock(mThreadMutex);
      mRenderThreadId = std::this_thread::get_id();
      mRenderThreadExited = true;
    }
    mStopRequested.store(false, std::memory_order_release);
    mPaused.store(false, std::memory_order_release);
    mRunning.store(true, std::memory_order_release);
    const EffectResult initialized = initializeExternal();
    if (initialized != EffectResult::Success) {
      mRunning.store(false, std::memory_order_release);
      return initialized;
    }
    return EffectResult::Success;
  }

  // Taken before the render thread starts so the cache's own numbers for this start are a difference and not a
  // total, which is what lets the watch ask "did *this* start store anything".
  std::shared_ptr<EngineImpl> self;
  try {
    self = shared_from_this();
  } catch (const std::bad_weak_ptr&) {
    setDiagnostic("start: the engine is not owned by a shared pointer");
    return EffectResult::InvalidArgument;
  }

  {
    const std::lock_guard<std::mutex> lock(mInitMutex);
    mInitialized = false;
    mInitResult = EffectResult::Success;
  }
  mStopRequested.store(false, std::memory_order_release);
  mPaused.store(false, std::memory_order_release);
  mRunning.store(true, std::memory_order_release);
  mShaderCacheBeforeStart = sampleShaderCache();
  {
    const std::lock_guard<std::mutex> lock(mThreadMutex);
    mRenderThreadExited = false;
    mRenderThreadId = {};
    try {
      // The lambda owns a strong reference: the implementation cannot be destroyed while its render
      // thread is alive, even if the caller drops every external handle. That is what makes the render
      // loop's `this` safe, and it is why the loop below is started through `self` rather than a bare
      // pointer.
      mThread = std::thread([self] {
        self->renderLoop();
        self->markRenderThreadExited();
      });
    } catch (...) {
      mRenderThreadExited = true;
      mRunning.store(false, std::memory_order_release);
      throw;
    }
  }

  std::unique_lock<std::mutex> lock(mInitMutex);
  mInitCv.wait(lock, [this] { return mInitialized; });
  const EffectResult result = mInitResult;
  lock.unlock();
  if (result != EffectResult::Success) {
    // The lifecycle lock is already held here, so the internal form is used: calling the public `stop()`
    // would deadlock on it. It also reaps the handle the failed thread left behind.
    stopLocked();
    return result;
  }
  // One cache health sample per start. A cache that is configured but never does anything is invisible in a single
  // run - the frames still render and the counters just keep saying `misses=N` - so it is the run of starts that
  // gets a diagnostic, not the numbers.
  {
    const ShaderCacheSample after = sampleShaderCache();
    ShaderCacheSample delta;
    delta.directorySet = after.directorySet;
    delta.hits = after.hits - mShaderCacheBeforeStart.hits;
    delta.misses = after.misses - mShaderCacheBeforeStart.misses;
    delta.entries = after.entries;
    delta.bytes = after.bytes;
    delta.budget = after.budget;

    // The state is carried in a file beside the cache: most applications create the engine once per process, so a
    // run of starts is otherwise never in one place.
    const std::string cacheDirectory = backend::shaderCacheDirectory();
    const std::string watchPath = cacheDirectory.empty() ? std::string() : cacheDirectory + ".watch";
    if (!watchPath.empty() && !mShaderCacheWatchRestored) {
      mShaderCacheWatchRestored = true;
      std::ifstream in(watchPath);
      std::string line;
      if (in.good() && std::getline(in, line)) mShaderCacheWatch.restore(line);
    }
    const std::string warning = mShaderCacheWatch.record(delta);
    if (!warning.empty()) report(EffectSeverity::Warning, warning);
    if (!watchPath.empty()) {
      // Best effort: an unwritable cache directory is what is already suspected, and failing to record the
      // suspicion must not become a second failure on the way out.
      std::ofstream out(watchPath, std::ios::trunc);
      if (out.good()) out << mShaderCacheWatch.save() << '\n';
    }
  }
  return EffectResult::Success;
}

EffectResult EngineImpl::stop() {
  {
    const std::lock_guard<std::mutex> lock(mThreadMutex);
    if (mThread.joinable() && mThread.get_id() == std::this_thread::get_id()) {
      // A callback running on the render thread asked the engine to stop. Waiting for that thread's own
      // exit event would deadlock, and joining ourselves would throw: request the exit and let the loop
      // publish it. The handle is reaped by the next stop or by the destructor.
      mStopRequested.store(true, std::memory_order_release);
      mCommandCv.notify_all();
      mRunning.store(false, std::memory_order_release);
      mPaused.store(false, std::memory_order_release);
      mSupportsYuvHardwareBuffer.store(false, std::memory_order_release);
      return EffectResult::Success;
    }
  }
  const std::lock_guard<std::mutex> lifecycle(mLifecycleMutex);
  return stopLocked();
}

EffectResult EngineImpl::stopLocked() {
  if (mConfig.externalDrive) {
    processCommandsExternal();
    if (mCore != nullptr) {
      mCore->waitIdle();
      std::vector<std::shared_ptr<EffectNode>> nodes;
      {
        const std::lock_guard<std::mutex> lock(mMutex);
        nodes = mGraph.nodes();
      }
      for (const std::shared_ptr<EffectNode>& node: nodes) {
        if (node == nullptr) continue;
        if (const std::shared_ptr<InputRuntime> runtime = NodeAccess::inputRuntime(*node)) {
          runtime->releaseResources();
        }
        if (const std::shared_ptr<OutputRuntime> runtime = NodeAccess::outputRuntime(*node)) {
          runtime->releaseResources();
        }
      }
      mExecutor.reset();
      mCore->shutdown();
      mCore.reset();
    }
    mRunning.store(false, std::memory_order_release);
    mPaused.store(false, std::memory_order_release);
    mSupportsYuvHardwareBuffer.store(false, std::memory_order_release);
    {
      const std::lock_guard<std::mutex> lock(mThreadMutex);
      mRenderThreadId = {};
      mRenderThreadExited = true;
    }
    return EffectResult::Success;
  }
  std::shared_ptr<EngineImpl> self;
  {
    const std::lock_guard<std::mutex> lock(mThreadMutex);
    if (!mThread.joinable()) {
      mStopRequested.store(false, std::memory_order_release);
      mRunning.store(false, std::memory_order_release);
      mPaused.store(false, std::memory_order_release);
      mSupportsYuvHardwareBuffer.store(false, std::memory_order_release);
      return EffectResult::Success;
    }
    // The render thread holds a strong reference for as long as it runs, so `shared_from_this()` is valid
    // whenever there is a thread to stop. Keeping it keeps `this` alive through the join: the lambda's
    // reference is released as the thread function returns, potentially before `join()` does.
    self = shared_from_this();
    mStopRequested.store(true, std::memory_order_release);
  }
  mCommandCv.notify_all();

  // Wait for the thread's own exit event rather than for `join()` alone. The event is published by the
  // thread function before it returns, so it is the first point at which the loop can no longer touch the
  // engine; the join afterwards only reaps the handle.
  {
    std::unique_lock<std::mutex> lock(mThreadMutex);
    mRenderThreadExitedCv.wait(lock, [this] { return mRenderThreadExited; });
  }

  std::thread handle;
  {
    const std::lock_guard<std::mutex> lock(mThreadMutex);
    handle = std::move(mThread);
    mRenderThreadId = {};
  }
  if (handle.joinable()) handle.join();

  mStopRequested.store(false, std::memory_order_release);
  mRunning.store(false, std::memory_order_release);
  mPaused.store(false, std::memory_order_release);
  mSupportsYuvHardwareBuffer.store(false, std::memory_order_release);
  return EffectResult::Success;
}

void EngineImpl::markRenderThreadExited() {
  {
    const std::lock_guard<std::mutex> lock(mThreadMutex);
    mRenderThreadId = {};
    mRenderThreadExited = true;
  }
  mRenderThreadExitedCv.notify_all();
}

EffectResult EngineImpl::recover() {
  // The pair is one transition: a concurrent recover must not stop the device another caller just rebuilt.
  const std::lock_guard<std::mutex> lifecycle(mLifecycleMutex);
  const EffectResult stopped = stopLocked();
  if (stopped != EffectResult::Success) return stopped;
  return startLocked();
}

size_t EngineImpl::pollCallbacks(const size_t max) {
  return mCallbackQueue != nullptr ? mCallbackQueue->drain(max) : 0;
}

EffectResult EngineImpl::submitCommand(Command command) {
  if (!mRunning.load(std::memory_order_acquire)) return EffectResult::NotRunning;
  if (mConfig.externalDrive) {
    {
      const std::lock_guard<std::mutex> lock(mThreadMutex);
      mCommands.push_back(std::move(command));
    }
    return processCommandsExternal();
  }
  auto promise = std::make_shared<std::promise<EffectResult>>();
  std::future<EffectResult> future = promise->get_future();
  command.done = promise;
  {
    const std::lock_guard<std::mutex> lock(mThreadMutex);
    if (!mThread.joinable()) return EffectResult::NotRunning;
    mCommands.push_back(std::move(command));
  }
  mCommandCv.notify_one();
  return future.get();
}

bool EngineImpl::postCommand(Command command) {
  if (!mRunning.load(std::memory_order_acquire)) return false;
  if (mConfig.externalDrive) {
    const std::lock_guard<std::mutex> lock(mThreadMutex);
    mCommands.push_back(std::move(command));
    return true;
  }
  {
    const std::lock_guard<std::mutex> lock(mThreadMutex);
    if (!mThread.joinable()) return false;
    mCommands.push_back(std::move(command));
  }
  mCommandCv.notify_one();
  return true;
}

EffectResult EngineImpl::pause() {
  if (!isRunning()) return EffectResult::NotRunning;
  if (mConfig.externalDrive) {
    mPaused.store(true, std::memory_order_release);
    return EffectResult::Success;
  }
  Command command;
  command.kind = Command::Kind::Pause;
  return submitCommand(std::move(command));
}

EffectResult EngineImpl::resume() {
  if (!isRunning()) return EffectResult::NotRunning;
  if (mConfig.externalDrive) {
    mPaused.store(false, std::memory_order_release);
    return EffectResult::Success;
  }
  Command command;
  command.kind = Command::Kind::Resume;
  return submitCommand(std::move(command));
}

EffectResult EngineImpl::execute(std::shared_ptr<Fence>* const frameFence) {
  if (frameFence != nullptr) frameFence->reset();
  if (!isRunning()) return EffectResult::NotRunning;

  if (mConfig.externalDrive) {
    processCommandsExternal();
    if (mPaused.load(std::memory_order_acquire)) return EffectResult::Success;
    std::string error;
    const EffectResult result = renderFrame(error, nullptr);
    if (result != EffectResult::Success && !error.empty() && error != mLastFrameError) {
      mLastFrameError = error;
      report(EffectSeverity::Warning, error);
    }
    return result;
  }

  std::shared_ptr<std::promise<std::shared_ptr<FrameCompletion>>> fencePromise;
  if (frameFence != nullptr) fencePromise = std::make_shared<std::promise<std::shared_ptr<FrameCompletion>>>();

  Command command;
  command.kind = Command::Kind::RequestFrame;
  command.fenceDone = fencePromise;
  const EffectResult result = submitCommand(std::move(command));
  if (frameFence != nullptr && fencePromise != nullptr && result == EffectResult::Success) {
    *frameFence = detail::makeFence(fencePromise->get_future().get());
  }
  return result;
}

EffectResult EngineImpl::flush() {
  if (!isRunning()) return EffectResult::Success;
  Command command;
  command.kind = Command::Kind::Flush;
  return submitCommand(std::move(command));
}

EffectResult EngineImpl::flushAndWait() {
  if (!isRunning()) return EffectResult::Success;
  Command command;
  command.kind = Command::Kind::WaitIdle;
  return submitCommand(std::move(command));
}

EffectResult EngineImpl::drainPresents(const std::chrono::milliseconds timeout) {
  if (!isRunning()) return EffectResult::Success;
  // Nothing can be outstanding while every output presents inline, and `waitIdle` is exactly the guarantee
  // a deferred output needs to satisfy: the submissions the queued presents wait on have finished. Once a
  // backend queues a present past `execute()`, this is where its per-output drain is dispatched.
  (void) timeout;
  Command command;
  command.kind = Command::Kind::WaitIdle;
  return submitCommand(std::move(command));
}

// ==================================================================================================
// Configuration
// ==================================================================================================

EffectResult EngineImpl::setRenderSize(const uint32_t width, const uint32_t height) {
  if (width == 0 || height == 0) {
    setDiagnostic("setRenderSize: width and height must be non-zero");
    return EffectResult::InvalidArgument;
  }
  if (width > 16384 || height > 16384) {
    setDiagnostic("setRenderSize: the requested size exceeds the engine's limit");
    return EffectResult::InvalidArgument;
  }
  mRenderWidth.store(width, std::memory_order_relaxed);
  mRenderHeight.store(height, std::memory_order_relaxed);
  return EffectResult::Success;
}

EffectResult EngineImpl::setFrameRate(const float framesPerSecond) {
  if (!std::isfinite(framesPerSecond) || framesPerSecond < 0.0f) {
    setDiagnostic("setFrameRate: the rate must be finite and non-negative");
    return EffectResult::InvalidArgument;
  }
  mFrameRate.store(framesPerSecond, std::memory_order_relaxed);
  return EffectResult::Success;
}

RenderTime EngineImpl::frameTimestamp() const noexcept {
  const float fps = frameRate();
  const int64_t ticksPerFrame = fps > 0.0f
                                  ? std::max<int64_t>(1, static_cast<int64_t>(std::llround(mTimeBase / fps)))
                                  : 1;
  return RenderTime::fromTicks(static_cast<int64_t>(mFrameIndex) * ticksPerFrame, mTimeBase);
}

// ==================================================================================================
// Queries and diagnostics
// ==================================================================================================

EffectEngineStats EngineImpl::stats() const {
  EffectEngineStats result;
  {
    const std::lock_guard<std::mutex> lock(mStatsMutex);
    result = mStats;
  }
  {
    const std::lock_guard<std::mutex> lock(mMutex);
    result.nodeCount = static_cast<uint32_t>(mGraph.size());
  }
  {
    const std::lock_guard<std::mutex> lock(mThreadMutex);
    result.commandQueueDepth = static_cast<uint32_t>(mCommands.size());
  }
  return result;
}

void EngineImpl::setExternalTextureResolver(std::shared_ptr<EffectExternalTextureResolver> resolver) {
  mExternalTextureResolver = std::move(resolver);
  if (mCore != nullptr) mCore->setExternalTextureResolver(mExternalTextureResolver);
}

void EngineImpl::setOnDeviceReady(std::function<void(backend::Device&)> callback) {
  mOnDeviceReady = std::move(callback);
}

void EngineImpl::setOnBeforeFrame(std::function<void(backend::Device&)> callback) {
  mOnBeforeFrame = std::move(callback);
}

void EngineImpl::setOnAfterFrame(std::function<void(backend::Device&)> callback) {
  mOnAfterFrame = std::move(callback);
}

void EngineImpl::publishStats(const uint32_t droppedDelta) {
  const std::lock_guard<std::mutex> lock(mStatsMutex);
  mStats.frameCount = mFrameIndex;
  mStats.directFrames = mExecutor != nullptr ? mExecutor->directFrames() : 0;
  mStats.droppedFrames += droppedDelta;
  if (mCore != nullptr) {
    const FramePoolStats pool = mCore->framePool().stats();
    mStats.framePoolFree = pool.free;
    mStats.framePoolInUse = pool.inUse;
    mStats.framePoolInFlight = pool.inFlight;
    mStats.framePoolBytes = pool.bytes;
    mStats.framePoolPeakBytes = pool.peakBytes;
    mStats.framePoolHits = pool.hits;
    mStats.framePoolMisses = pool.misses;
    mStats.framePoolEvictions = pool.evictions;
    mStats.backpressureBlocked = pool.backpressureWaits;
    mStats.programCount = mCore->programCount();
    mStats.pipelineCount = mCore->pipelineCount();
    mStats.programEvictions = mCore->programEvictions();
    mStats.imageTextureEvictions = mCore->imageTextureEvictions();
    mStats.solidTextureEvictions = mCore->solidTextureEvictions();
    mStats.textureCacheBytes = mCore->textureCacheBytes();
    mStats.textureCachePeakBytes = mCore->textureCachePeakBytes();
    mStats.programCompileCount = mCore->programCompileCount();
    mStats.programCompileMilliseconds = mCore->programCompileMilliseconds();
    mStats.pipelineCreationCount = mCore->pipelineCreationCount();
    mStats.pipelineCreationMilliseconds = mCore->pipelineCreationMilliseconds();
    mStats.textureCount = mCore->textureCount();
    mStats.framesInFlight = pool.inFlight;
  }
}

std::string EngineImpl::diagnostics() const {
  const std::lock_guard<std::mutex> lock(mDiagnosticsMutex);
  return mDiagnostics;
}

void EngineImpl::report(const EffectSeverity severity, const std::string& message) {
  const std::string line = prefix(severity, message);
  {
    const std::lock_guard<std::mutex> lock(mDiagnosticsMutex);
    if (!mDiagnostics.empty()) mDiagnostics += '\n';
    mDiagnostics += line;
    constexpr size_t kMaxDiagnosticBytes = 16u * 1024u;
    if (mDiagnostics.size() > kMaxDiagnosticBytes) mDiagnostics.erase(0, mDiagnostics.size() - kMaxDiagnosticBytes);
  }
  if (mDiagnosticsCallback != nullptr) mDiagnosticsCallback(severity, line.c_str(), mDiagnosticsUserData);
}

void EngineImpl::setDiagnostic(std::string message) {
  report(EffectSeverity::Error, std::move(message));
}

// ==================================================================================================
// Render thread
// ==================================================================================================

EffectResult EngineImpl::renderFrame(std::string& error, std::shared_ptr<FrameCompletion>* const completion) {
  GraphPlan plan;
  EffectResult result = buildPlan(plan, error);
  if (result != EffectResult::Success) return result;

  std::unordered_set<NodeId> live;
  live.reserve(plan.nodes.size());
  for (const auto& entry: plan.nodes) live.insert(entry.first);
  if (mExecutor != nullptr) mExecutor->prune(live);

  mCore->beginFrame();
  // A soft cap rather than a hard limit: recently resolved programs are kept, and the trim waits for an
  // empty submission ring so in-flight command buffers keep valid pipelines.
  mCore->trimPrograms(mConfig.maxCachedPrograms);

  FrameContext context;
  context.core = mCore.get();
  context.nextFrameId = &mCore->frameIdAllocator();
  context.frameIndex = mFrameIndex;
  context.timestamp = frameTimestamp();
  context.renderWidth = renderWidth();
  context.renderHeight = renderHeight();

  if (mOnBeforeFrame != nullptr && mCore != nullptr && mCore->device() != nullptr) {
    mOnBeforeFrame(*mCore->device());
  }
  result = mExecutor->execute(plan, context, error,
    completion != nullptr ? *completion : std::shared_ptr<FrameCompletion>());
  if (mOnAfterFrame != nullptr && mCore != nullptr && mCore->device() != nullptr) {
    mOnAfterFrame(*mCore->device());
  }
  ++mFrameIndex;
  // A frame the pool could not serve under DropOldest is a dropped frame, not a failure: preview prefers
  // the next frame to an error the caller cannot act on. Under Block the caller asked to wait, so the
  // exhaustion is a failure rather than a silent drop.
  const bool dropped = result == EffectResult::Success ||
                       result == EffectResult::Busy ||
                       (result == EffectResult::FramePoolExhausted &&
                        mConfig.backpressureMode == EffectBackpressureMode::DropOldest);
  publishStats(dropped && result != EffectResult::Success ? 1u : 0u);
  if (result != EffectResult::Success && !dropped) report(EffectSeverity::Error, "frame: " + error);
  return result;
}

bool EngineImpl::recoverOnRenderThread() {
  backend::SurfaceDescriptor surface;
  bool hasSurface = false;
  std::vector<std::shared_ptr<EffectNode>> nodes;
  {
    const std::lock_guard<std::mutex> lock(mMutex);
    nodes = mGraph.nodes();
    const std::shared_ptr<EffectNode> output = mGraph.find(mGraph.output());
    if (output != nullptr) {
      if (const std::shared_ptr<OutputRuntime> runtime = NodeAccess::outputRuntime(*output)) {
        hasSurface = runtime->surfaceDescriptor(surface);
      }
    }
  }

  for (const std::shared_ptr<EffectNode>& node: nodes) {
    if (node->kind() == EffectNodeKind::Input) {
      if (const std::shared_ptr<InputRuntime> runtime = NodeAccess::inputRuntime(*node)) runtime->releaseResources();
    } else if (node->kind() == EffectNodeKind::Output) {
      if (const std::shared_ptr<OutputRuntime> runtime = NodeAccess::outputRuntime(*node)) runtime->releaseResources();
    }
  }

  mExecutor.reset();
  mCore->shutdown();
  mSupportsYuvHardwareBuffer.store(false, std::memory_order_release);
  mCore = std::make_unique<RenderCore>();
  mCore->setExternalTextureResolver(mExternalTextureResolver);
  std::string error;
  if (!mCore->initialize(mConfig, hasSurface ? &surface : nullptr, error)) {
    report(EffectSeverity::Error, "device recovery failed: " + error);
    return false;
  }
  mExecutor = std::make_unique<GraphExecutor>(*mCore);
  mSupportsYuvHardwareBuffer.store(
    mCore->device() != nullptr && mCore->device()->supportsYuvHardwareBufferImport(),
    std::memory_order_release);
  if (mOnDeviceReady && mCore->device() != nullptr) mOnDeviceReady(*mCore->device());
  return true;
}

EffectResult EngineImpl::initializeExternal() {
  backend::SurfaceDescriptor surface;
  bool hasSurface = false;
  {
    const std::lock_guard<std::mutex> lock(mMutex);
    const std::shared_ptr<EffectNode> output = mGraph.find(mGraph.output());
    if (output != nullptr) {
      if (const std::shared_ptr<OutputRuntime> runtime = NodeAccess::outputRuntime(*output)) {
        hasSurface = runtime->surfaceDescriptor(surface);
      }
    }
  }

  mCore = std::make_unique<RenderCore>();
  mCore->setExternalTextureResolver(mExternalTextureResolver);
  std::string error;
  if (!mCore->initialize(mConfig, hasSurface ? &surface : nullptr, error)) {
    report(EffectSeverity::Error, "external start: " + error);
    mCore.reset();
    return EffectResult::DeviceLost;
  }
  mExecutor = std::make_unique<GraphExecutor>(*mCore);
  mSupportsYuvHardwareBuffer.store(
    mCore->device() != nullptr && mCore->device()->supportsYuvHardwareBufferImport(),
    std::memory_order_release);
  mFrameIndex = 0;
  mPendingRequests.clear();

  std::vector<std::shared_ptr<EffectNode>> nodes;
  {
    const std::lock_guard<std::mutex> lock(mMutex);
    nodes = mGraph.nodes();
  }
  for (const std::shared_ptr<EffectNode>& node: nodes) {
    if (node == nullptr) continue;
    const std::shared_ptr<OutputRuntime> runtime = NodeAccess::outputRuntime(*node);
    if (runtime == nullptr) continue;
    std::string prepareError;
    if (!runtime->prepare(*mCore, prepareError) && !prepareError.empty()) {
      report(EffectSeverity::Warning, "external start: an output could not prepare: " + prepareError);
    }
  }
  if (mOnDeviceReady && mCore->device() != nullptr) mOnDeviceReady(*mCore->device());
  return EffectResult::Success;
}

EffectResult EngineImpl::processCommandsExternal() {
  std::deque<Command> commands;
  {
    const std::lock_guard<std::mutex> lock(mThreadMutex);
    commands.swap(mCommands);
  }
  bool paused = mPaused.load(std::memory_order_acquire);
  for (Command& command: commands) {
    switch (command.kind) {
      case Command::Kind::Stop:
        if (command.done != nullptr) command.done->set_value(EffectResult::Success);
        break;
      case Command::Kind::Flush:
        if (command.done != nullptr) command.done->set_value(EffectResult::Success);
        break;
      case Command::Kind::WaitIdle:
        if (mCore != nullptr) mCore->waitIdle();
        if (command.done != nullptr) command.done->set_value(EffectResult::Success);
        break;
      case Command::Kind::Pause:
        paused = true;
        mPaused.store(true, std::memory_order_release);
        if (command.done != nullptr) command.done->set_value(EffectResult::Success);
        break;
      case Command::Kind::Resume:
        paused = false;
        mPaused.store(false, std::memory_order_release);
        if (command.done != nullptr) command.done->set_value(EffectResult::Success);
        break;
      case Command::Kind::RequestFrame:
        // `execute()` calls `renderFrame` directly in external-drive mode; a queued request is not a
        // second frame path.
        if (command.done != nullptr) command.done->set_value(EffectResult::Success);
        if (command.fenceDone != nullptr) command.fenceDone->set_value(nullptr);
        break;
      case Command::Kind::SetRenderSize:
        mRenderWidth.store(command.width, std::memory_order_relaxed);
        mRenderHeight.store(command.height, std::memory_order_relaxed);
        if (command.done != nullptr) command.done->set_value(EffectResult::Success);
        break;
      case Command::Kind::SetFrameRate:
        mFrameRate.store(command.frameRate, std::memory_order_relaxed);
        if (command.done != nullptr) command.done->set_value(EffectResult::Success);
        break;
      case Command::Kind::PrepareOutputs: {
        EffectResult prepared = EffectResult::Success;
        if (mCore == nullptr || mCore->device() == nullptr) {
          prepared = EffectResult::NotRunning;
        } else {
          std::vector<std::shared_ptr<EffectNode>> nodes;
          {
            const std::lock_guard<std::mutex> lock(mMutex);
            nodes = mGraph.nodes();
          }
          for (const std::shared_ptr<EffectNode>& node: nodes) {
            if (node == nullptr) continue;
            const std::shared_ptr<OutputRuntime> runtime = NodeAccess::outputRuntime(*node);
            if (runtime == nullptr) continue;
            std::string prepareError;
            if (!runtime->prepare(*mCore, prepareError) && !prepareError.empty()) {
              report(EffectSeverity::Warning, "prepareOutputs: " + prepareError);
            }
          }
        }
        if (command.done != nullptr) command.done->set_value(prepared);
        break;
      }
      case Command::Kind::Retire:
        if (mCore != nullptr) {
          mCore->retireAfterGpu(std::move(command.retire));
        } else if (command.retire) {
          command.retire();
        }
        break;
    }
  }
  return EffectResult::Success;
}

void EngineImpl::failPendingRequests(const EffectResult result) {
  for (const FrameRequest& request: mPendingRequests) {
    if (request.done != nullptr) request.done->set_value(result);
    if (request.fenceDone != nullptr) request.fenceDone->set_value(nullptr);
  }
  mPendingRequests.clear();
}

void EngineImpl::renderLoop() {
  // Published before the graph is touched so a `stop()` reaching us from one of the initialization
  // callbacks can recognize that it is running on this thread and must not wait for its own exit.
  {
    const std::lock_guard<std::mutex> lock(mThreadMutex);
    mRenderThreadId = std::this_thread::get_id();
  }

  // The output node gets the first word on device creation: a platform window has to be part of the EGL
  // surface the device is created with. This is why `setOutput` for a window-backed node must happen
  // before `start()`.
  backend::SurfaceDescriptor surfaceDescriptor;
  bool hasSurface = false;
  {
    const std::lock_guard<std::mutex> lock(mMutex);
    const std::shared_ptr<EffectNode> output = mGraph.find(mGraph.output());
    if (output != nullptr) {
      if (const std::shared_ptr<OutputRuntime> runtime = NodeAccess::outputRuntime(*output)) {
        hasSurface = runtime->surfaceDescriptor(surfaceDescriptor);
      }
    }
  }

  mSupportsYuvHardwareBuffer.store(false, std::memory_order_release);
  mCore = std::make_unique<RenderCore>();
  mCore->setExternalTextureResolver(mExternalTextureResolver);
  {
    std::string error;
    const EffectEngineConfig& config = mConfig;
    if (!mCore->initialize(config, hasSurface ? &surfaceDescriptor : nullptr, error)) {
      report(EffectSeverity::Error, "start: " + error);
      {
        const std::lock_guard<std::mutex> lock(mInitMutex);
        mInitResult = EffectResult::DeviceLost;
        mInitialized = true;
      }
      mInitCv.notify_all();
      mRunning.store(false, std::memory_order_release);
      mCore.reset();
      return;
    }
  }
  mExecutor = std::make_unique<GraphExecutor>(*mCore);
  mSupportsYuvHardwareBuffer.store(
    mCore->device() != nullptr && mCore->device()->supportsYuvHardwareBufferImport(),
    std::memory_order_release);
  // Let each output create its platform target now, while nobody is waiting for a frame. On this machine
  // `vkCreateSwapchainKHR` costs ~180 ms for the first swapchain and ~25 ms for the second, which is the whole
  // reason a windowed first frame used to cost ~220 ms. A failure is not fatal here: the frame that needs the
  // target will retry and report properly, so this only records a diagnostic.
  if (mCore->device() != nullptr) {
    std::vector<std::shared_ptr<EffectNode>> nodes;
    {
      const std::lock_guard<std::mutex> lock(mMutex);
      nodes = mGraph.nodes();
    }
    for (const std::shared_ptr<EffectNode>& node: nodes) {
      if (node == nullptr) continue;
      const std::shared_ptr<OutputRuntime> runtime = NodeAccess::outputRuntime(*node);
      if (runtime == nullptr) continue;
      std::string prepareError;
      if (!runtime->prepare(*mCore, prepareError) && !prepareError.empty()) {
        report(EffectSeverity::Warning, "start: an output could not prepare its target: " + prepareError);
      }
    }
  }
  // Deferred present needs both halves: a backend whose present can be queued, and an output that knows how
  // to skip a busy target. Publishing it here makes `supportsDeferredPresent()` meaningful as soon as
  // `start()` returns.
  {
    const bool deviceCanDefer = mCore->device() != nullptr && mCore->device()->supportsDeferredPresent();
    bool outputCanDefer = false;
    if (deviceCanDefer) {
      std::vector<std::shared_ptr<EffectNode>> nodes;
      {
        const std::lock_guard<std::mutex> lock(mMutex);
        nodes = mGraph.nodes();
      }
      for (const std::shared_ptr<EffectNode>& node: nodes) {
        if (node == nullptr) continue;
        const std::shared_ptr<OutputRuntime> runtime = NodeAccess::outputRuntime(*node);
        if (runtime != nullptr && runtime->supportsDeferredPresent()) {
          outputCanDefer = true;
          break;
        }
      }
    }
    mSupportsDeferredPresent.store(deviceCanDefer && outputCanDefer, std::memory_order_release);
  }
  // Compile the pass programs now, on the render thread, where the device context is current. Without this the
  // first frame pays for it: on a GLES device the first `execute()` was measured at 184 ms against 17 ms steady
  // state, of which the driver's shader compile is about 107 ms. The same argument as the compiler warm-up:
  // nobody is waiting during `start()`, and somebody always is during a frame.
  //
  // Skipped when async compilation is on, because there the whole point is that `start()` does not block on a
  // compile - an application that asked for that should not get the opposite.
  if (mCore->device() != nullptr && !mConfig.asyncShaderCompilation) {
    std::vector<std::shared_ptr<EffectNode>> nodes;
    {
      const std::lock_guard<std::mutex> lock(mMutex);
      nodes = mGraph.nodes();
    }
    for (const std::shared_ptr<EffectNode>& node: nodes) {
      if (node == nullptr || node->kind() != EffectNodeKind::Pass) continue;
      const NodeSnapshot snapshot = NodeAccess::snapshot(*node);
      // Before the first frame the shader lives in `pendingProgram`: the executor installs it into `program` when
      // it applies the node, which is exactly the frame this warm-up is trying to take the cost out of.
      const std::shared_ptr<const ProgramSnapshot> program =
        snapshot.program != nullptr ? snapshot.program : snapshot.pendingProgram;
      if (program == nullptr) continue;
      std::string warmError;
      RenderProgram* compiled = mCore->programFor(program, warmError);
      if (compiled == nullptr) {
        if (!warmError.empty()) report(EffectSeverity::Warning,
          "start: a pass program could not be compiled: " + warmError);
        continue;
      }
      // Linking is the second half of the cost, and it is the half a driver defers to the first draw. The pool
      // hands out a frame of the pass's own output format so the link has a render pass to build against.
      const backend::PixelFormat format = detail::toPixelFormat(snapshot.outputFormat);
      if (!mCore->warmPipeline(*compiled, format, mConfig.renderWidth, mConfig.renderHeight, warmError) &&
          !warmError.empty()) {
        report(EffectSeverity::Warning, "start: a pass pipeline could not be linked: " + warmError);
      }
    }
  }
  if (mOnDeviceReady && mCore->device() != nullptr) mOnDeviceReady(*mCore->device());
  {
    const std::lock_guard<std::mutex> lock(mInitMutex);
    mInitResult = EffectResult::Success;
    mInitialized = true;
  }
  mInitCv.notify_all();

  bool paused = false;
  const auto idle = [] {
    return std::chrono::steady_clock::time_point::max();
  };
  auto nextFrame = std::chrono::steady_clock::now();
  auto lastFrameTime = std::chrono::steady_clock::time_point{};

  for (;;) {
    std::deque<Command> commands;
    {
      std::unique_lock<std::mutex> lock(mThreadMutex);
      const bool waitForFrame = !paused && mRenderMode == EffectRenderMode::Continuous;
      auto deadline = waitForFrame ? nextFrame : idle();
      if (mCore != nullptr && mCore->hasPendingCompletions()) {
        const auto poll = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
        if (poll < deadline) deadline = poll;
      }
      mCommandCv.wait_until(lock, deadline, [this] {
        return mStopRequested.load(std::memory_order_acquire) || !mCommands.empty();
      });
      if (mStopRequested.load(std::memory_order_acquire)) break;
      commands.swap(mCommands);
    }

    bool stop = false;
    for (Command& command: commands) {
      switch (command.kind) {
        case Command::Kind::Stop:
          stop = true;
          break;
        case Command::Kind::Flush:
          if (command.done != nullptr) command.done->set_value(EffectResult::Success);
          break;
        case Command::Kind::WaitIdle:
          mCore->waitIdle();
          if (command.done != nullptr) command.done->set_value(EffectResult::Success);
          break;
        case Command::Kind::Pause:
          paused = true;
          mPaused.store(true, std::memory_order_release);
          if (command.done != nullptr) command.done->set_value(EffectResult::Success);
          break;
        case Command::Kind::Resume:
          paused = false;
          mPaused.store(false, std::memory_order_release);
          nextFrame = std::chrono::steady_clock::now();
          if (command.done != nullptr) command.done->set_value(EffectResult::Success);
          break;
        case Command::Kind::RequestFrame:
          mPendingRequests.push_back(FrameRequest{ command.done, command.fenceDone });
          break;
        case Command::Kind::SetRenderSize:
          mRenderWidth.store(command.width, std::memory_order_relaxed);
          mRenderHeight.store(command.height, std::memory_order_relaxed);
          if (command.done != nullptr) command.done->set_value(EffectResult::Success);
          break;
        case Command::Kind::SetFrameRate:
          mFrameRate.store(command.frameRate, std::memory_order_relaxed);
          nextFrame = std::chrono::steady_clock::now();
          if (command.done != nullptr) command.done->set_value(EffectResult::Success);
          break;
        case Command::Kind::PrepareOutputs: {
          EffectResult prepared = EffectResult::Success;
          if (mCore == nullptr || mCore->device() == nullptr) {
            prepared = EffectResult::NotRunning;
          } else {
            std::vector<std::shared_ptr<EffectNode>> nodes;
            {
              const std::lock_guard<std::mutex> lock(mMutex);
              nodes = mGraph.nodes();
            }
            for (const std::shared_ptr<EffectNode>& node: nodes) {
              if (node == nullptr) continue;
              const std::shared_ptr<OutputRuntime> runtime = NodeAccess::outputRuntime(*node);
              if (runtime == nullptr) continue;
              std::string prepareError;
              if (!runtime->prepare(*mCore, prepareError) && !prepareError.empty()) {
                report(EffectSeverity::Warning, "prepareOutputs: " + prepareError);
              }
            }
          }
          if (command.done != nullptr) command.done->set_value(prepared);
          break;
        }
        case Command::Kind::Retire:
          // The runtime captured by `retire` may own imported textures/staging buffers. Queue it behind the
          // submission ring instead of blocking this frame.
          mCore->retireAfterGpu(std::move(command.retire));
          break;
      }
    }
    if (stop) break;

    if (mCore != nullptr) mCore->pollCompletions();
    if (paused) continue;

    const auto now = std::chrono::steady_clock::now();
    bool render = !mPendingRequests.empty();
    if (mRenderMode == EffectRenderMode::Continuous && now >= nextFrame) render = true;
    if (!render) continue;

    const auto start = std::chrono::steady_clock::now();
    std::string error;
    std::shared_ptr<FrameCompletion> completion;
    if (!mPendingRequests.empty() && mPendingRequests.front().fenceDone != nullptr) {
      completion = std::make_shared<FrameCompletion>();
    }
    EffectResult result = renderFrame(error, &completion);
    // A failed frame's reason only exists on this thread, and `execute()` has no way to hand it back. Reporting it
    // as a diagnostic is what makes "the input node 'video-b' has no frame" reachable at all; the duplicate check
    // keeps a steady-state failure from replacing the diagnostics buffer with 60 copies of itself per second.
    if (result == EffectResult::Success) {
      mLastFrameError.clear();
    } else if (!error.empty() && error != mLastFrameError) {
      mLastFrameError = error;
      report(EffectSeverity::Warning, error);
    }
    if (result == EffectResult::DeviceLost && mRenderMode == EffectRenderMode::Continuous &&
        mPendingRequests.empty()) {
      // Continuous preview has no caller to retry for it; rebuild on the render thread and keep the loop
      // alive. A failing surface descriptor stops the loop instead of spinning on a dead window.
      if (recoverOnRenderThread()) {
        result = EffectResult::Success;
      } else {
        stop = true;
      }
    }
    const auto end = std::chrono::steady_clock::now();
    {
      const std::lock_guard<std::mutex> lock(mStatsMutex);
      mStats.cpuMilliseconds = std::chrono::duration<double, std::milli>(end - start).count();
      if (mCore != nullptr) mStats.gpuMilliseconds = mCore->gpuMilliseconds();
      if (lastFrameTime != std::chrono::steady_clock::time_point{}) {
        const double seconds = std::chrono::duration<double>(end - lastFrameTime).count();
        if (seconds > 0.0) mStats.framesPerSecond = 1.0 / seconds;
      }
    }
    lastFrameTime = end;

    if (!mPendingRequests.empty()) {
      FrameRequest request = std::move(mPendingRequests.front());
      mPendingRequests.pop_front();
      if (request.done != nullptr) request.done->set_value(result);
      if (request.fenceDone != nullptr) {
        if (completion == nullptr) completion = std::make_shared<FrameCompletion>();
        if (result != EffectResult::Success) completion->signal(false);
        request.fenceDone->set_value(completion);
      }
    }
    if (stop) break;
    if (mRenderMode == EffectRenderMode::Continuous) {
      const float fps = mFrameRate.load(std::memory_order_relaxed) > 0.0f
                          ? mFrameRate.load(std::memory_order_relaxed)
                          : 60.0f;
      const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / static_cast<double>(fps)));
      // Advance the absolute deadline instead of restarting it from `now`: a wake that arrives a
      // little late then does not move the whole cadence, which used to cap continuous preview at
      // roughly 56 fps even when a frame's CPU work was only a few milliseconds.
      nextFrame = std::max(nextFrame + interval, now);
    }
  }

  failPendingRequests(EffectResult::NotRunning);
  // Anything queued behind the stop is answered rather than left waiting on a thread that is gone.
  {
    const std::lock_guard<std::mutex> lock(mThreadMutex);
    bool hasRetire = false;
    for (Command& command: mCommands) {
      if (command.kind == Command::Kind::Retire) {
        if (mCore != nullptr) {
          mCore->retireAfterGpu(std::move(command.retire));
          hasRetire = true;
        } else if (command.retire) {
          command.retire();
        }
        continue;
      }
      if (command.done != nullptr) command.done->set_value(EffectResult::NotRunning);
      if (command.fenceDone != nullptr) command.fenceDone->set_value(nullptr);
    }
    mCommands.clear();
    // Run deferred releases while the device is still alive; waitIdle also flushes the retired queue.
    if (hasRetire && mCore != nullptr) mCore->waitIdle();
  }

  // Node runtimes may hold staging buffers or textures that belong to the device. They are told to drop
  // them while the device is still alive; the nodes themselves outlive the engine and must not be left
  // holding a pointer into freed GPU state.
  {
    std::vector<std::shared_ptr<EffectNode>> nodes;
    {
      const std::lock_guard<std::mutex> lock(mMutex);
      nodes = mGraph.nodes();
    }
    for (const std::shared_ptr<EffectNode>& node: nodes) {
      if (node == nullptr) continue;
      if (const std::shared_ptr<InputRuntime> runtime = NodeAccess::inputRuntime(*node)) runtime->releaseResources();
      const std::shared_ptr<OutputRuntime> outputRuntime = NodeAccess::outputRuntime(*node);
      if (outputRuntime != nullptr) {
        outputRuntime->releaseResources();
      }
    }
  }

  mExecutor.reset();
  if (mCore != nullptr) mCore->shutdown();
  mCore.reset();
  mRunning.store(false, std::memory_order_release);
  mPaused.store(false, std::memory_order_release);
}

} // namespace detail

// ==================================================================================================
// EffectEngine
// ==================================================================================================

EffectEngine::EffectEngine() = default;

EffectEngine::~EffectEngine() {
  // Stop explicitly before dropping the implementation. The render thread owns a strong reference to the
  // implementation while it runs, so without this request an engine released without `stop()` would never
  // destroy it and would leave the device and the thread alive for the rest of the process.
  if (mImpl != nullptr) mImpl->stop();
}

std::shared_ptr<EffectEngine> EffectEngine::create(const EffectEngineConfig& config) {
  if (config.timeBase == 0) {
    if (config.diagnosticsCallback != nullptr) {
      config.diagnosticsCallback(EffectSeverity::Error, "EffectEngineConfig::timeBase must be non-zero",
        config.diagnosticsUserData);
    }
    return nullptr;
  }
  if (!std::isfinite(config.frameRate) || config.frameRate < 0.0f) {
    if (config.diagnosticsCallback != nullptr) {
      config.diagnosticsCallback(EffectSeverity::Error, "EffectEngineConfig::frameRate must be finite and non-negative",
        config.diagnosticsUserData);
    }
    return nullptr;
  }
  if (config.maxFramesInFlight == 0) {
    if (config.diagnosticsCallback != nullptr) {
      config.diagnosticsCallback(EffectSeverity::Warning, "EffectEngineConfig::maxFramesInFlight is 0; using 1",
        config.diagnosticsUserData);
    }
  }

  auto engine = std::shared_ptr<EffectEngine>(new EffectEngine());
  engine->mImpl = std::make_shared<detail::EngineImpl>(config);
  return engine;
}

std::shared_ptr<EffectEngine> EffectEngine::create(const EffectBackend backend, void* const sharedContext) {
  EffectEngineConfig config;
  config.backend = backend;
  config.sharedContext = sharedContext;
  return create(config);
}

// ---- graph ---------------------------------------------------------------------------------------

namespace {
std::mutex gGraphObserverMutex;
EffectEngine::GraphCallObserver gGraphObserver;
} // namespace

void EffectEngine::setGraphCallObserver(GraphCallObserver observer) {
  const std::lock_guard<std::mutex> lock(gGraphObserverMutex);
  gGraphObserver = std::move(observer);
}

namespace {
void notifyGraphCall(const EffectEngine::GraphCall call) {
  EffectEngine::GraphCallObserver observer;
  {
    const std::lock_guard<std::mutex> lock(gGraphObserverMutex);
    observer = gGraphObserver;
  }
  if (observer) observer(call);
}
} // namespace

EffectResult EffectEngine::attach(const std::shared_ptr<EffectNode>& node) {
  notifyGraphCall(GraphCall::Attach);
  return mImpl->attach(node);
}

EffectResult EffectEngine::detach(const std::shared_ptr<EffectNode>& node) {
  return mImpl->detach(node);
}

EffectResult EffectEngine::connect(const std::shared_ptr<EffectNode>& from,
  const std::shared_ptr<EffectNode>& to,
  const uint32_t inputIndex) {
  notifyGraphCall(GraphCall::Connect);
  return mImpl->connect(from, to, inputIndex);
}

EffectResult EffectEngine::disconnect(const std::shared_ptr<EffectNode>& from,
  const std::shared_ptr<EffectNode>& to,
  const uint32_t inputIndex) {
  notifyGraphCall(GraphCall::Disconnect);
  return mImpl->disconnect(from, to, inputIndex);
}

EffectResult EffectEngine::setOutput(const std::shared_ptr<EffectOutputNode>& output) {
  return mImpl->setOutput(output);
}

EffectResult EffectEngine::addOutput(const std::shared_ptr<EffectOutputNode>& output) {
  return mImpl->addOutput(output);
}

EffectResult EffectEngine::removeOutput(const std::shared_ptr<EffectOutputNode>& output) {
  return mImpl->removeOutput(output);
}

EffectResult EffectEngine::prepareOutputs() {
  return mImpl->prepareOutputs();
}

std::shared_ptr<EffectPassNode> EffectEngine::createEffectNode(const std::string_view name) {
  auto node = std::shared_ptr<EffectPassNode>(new EffectPassNode());
  if (!name.empty()) node->setName(name);
  if (attach(node) != EffectResult::Success) {
    return nullptr;
  }
  return node;
}

// ---- life cycle ----------------------------------------------------------------------------------

EffectResult EffectEngine::start() {
  return mImpl->start();
}

EffectResult EffectEngine::pause() {
  return mImpl->pause();
}

EffectResult EffectEngine::resume() {
  return mImpl->resume();
}

EffectResult EffectEngine::stop() {
  return mImpl->stop();
}

EffectResult EffectEngine::recover() {
  return mImpl->recover();
}

size_t EffectEngine::pollCallbacks(const size_t max) {
  return mImpl->pollCallbacks(max);
}

EffectResult EffectEngine::execute() {
  EffectResult result = mImpl->execute(nullptr);
  if (result == EffectResult::DeviceLost) {
    // Manual/export calls get one transparent recovery attempt at the API boundary. The graph and nodes
    // stay alive; a window output re-registers its still-live native surface on the new device.
    result = mImpl->recover() == EffectResult::Success ? mImpl->execute(nullptr) : EffectResult::DeviceLost;
  }
  return result;
}

EffectResult EffectEngine::execute(std::shared_ptr<Fence>* const frameFence) {
  EffectResult result = mImpl->execute(frameFence);
  if (result == EffectResult::DeviceLost) {
    result = mImpl->recover() == EffectResult::Success ? mImpl->execute(frameFence) : EffectResult::DeviceLost;
  }
  return result;
}

// ---- synchronization -----------------------------------------------------------------------------

void EffectEngine::setExternalTextureResolver(std::shared_ptr<EffectExternalTextureResolver> resolver) {
  mImpl->setExternalTextureResolver(std::move(resolver));
}

void EffectEngine::setOnDeviceReady(std::function<void(backend::Device&)> callback) {
  mImpl->setOnDeviceReady(std::move(callback));
}

void EffectEngine::setOnBeforeFrame(std::function<void(backend::Device&)> callback) {
  mImpl->setOnBeforeFrame(std::move(callback));
}

void EffectEngine::setOnAfterFrame(std::function<void(backend::Device&)> callback) {
  mImpl->setOnAfterFrame(std::move(callback));
}

EffectResult EffectEngine::flush() {
  return mImpl->flush();
}

EffectResult EffectEngine::flushAndWait() {
  return mImpl->flushAndWait();
}

EffectResult EffectEngine::drainPresents(const std::chrono::milliseconds timeout) {
  return mImpl->drainPresents(timeout);
}

// ---- configuration -------------------------------------------------------------------------------

EffectResult EffectEngine::setRenderSize(const uint32_t width, const uint32_t height) {
  return mImpl->setRenderSize(width, height);
}

EffectResult EffectEngine::setFrameRate(const float framesPerSecond) {
  return mImpl->setFrameRate(framesPerSecond);
}

EffectResult EffectEngine::resetGraph() {
  return mImpl->resetGraph();
}

// ---- queries -------------------------------------------------------------------------------------

uint32_t EffectEngine::timeBase() const noexcept {
  return mImpl->timeBase();
}

EffectBackend EffectEngine::backend() const noexcept {
  if (mImpl == nullptr) {
    return EffectBackend::Default;
  }
  const auto* core = mImpl->core();
  const auto* device = core != nullptr ? core->device() : nullptr;
  if (device == nullptr) {
    return EffectBackend::Default;
  }
  switch (device->getInfo().backend) {
    case backend::Backend::OpenGL:
      return EffectBackend::OpenGL;
    case backend::Backend::Vulkan:
      return EffectBackend::Vulkan;
    case backend::Backend::Metal:
      return EffectBackend::Metal;
    default:
      return EffectBackend::Default;
  }
}

backend::Device* EffectEngine::device() const noexcept {
  if (mImpl == nullptr) return nullptr;
  const auto* core = mImpl->core();
  return core != nullptr ? core->device() : nullptr;
}

bool EffectEngine::isRunning() const noexcept {
  return mImpl->isRunning();
}

bool EffectEngine::isPaused() const noexcept {
  return mImpl->isPaused();
}

bool EffectEngine::supportsYuvHardwareBuffer() const noexcept {
  return mImpl->supportsYuvHardwareBuffer();
}

bool EffectEngine::supportsDeferredPresent() const noexcept {
  return mImpl->supportsDeferredPresent();
}

EffectEngineStats EffectEngine::stats() const {
  return mImpl->stats();
}

std::string EffectEngine::diagnostics() const {
  return mImpl->diagnostics();
}

} // namespace coral
