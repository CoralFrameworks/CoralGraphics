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

#ifndef CORAL_EFFECT_EXECUTOR_H
#define CORAL_EFFECT_EXECUTOR_H

/**
 * @file EffectExecutor.h
 *
 * The v1 graph executor: one linear/converging chain from a set of inputs through its passes to the
 * single output. Fan-out is refused (the DAG validator allows it, the executor does not), fan-in is
 * allowed because a multi-input pass is a single node with several upstream chains and the M4 contract
 * includes it.
 *
 * Everything here runs on the render thread and holds GPU state between frames; `prune` is how a detached
 * node's program and bindings stop being applied.
 */

#include "EffectInternal.h"
#include "EffectRenderCore.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <future>
#include <vector>

namespace coral::detail {

/** A frame's immutable view of the graph: the order to walk and the nodes/edges that order names. */
struct GraphPlan {
  std::vector<NodeId> order;
  std::map<NodeId, std::shared_ptr<EffectNode>> nodes;
  std::vector<Edge> edges;
  NodeId output = kInvalidNodeId;
  std::vector<NodeId> outputs;
};

/** Result of compiling a pending shader's sources on a worker thread. */
struct AsyncCompileResult {
  std::vector<uint32_t> vertexSpirv;
  std::vector<uint32_t> fragmentSpirv;
  std::string error;

  [[nodiscard]] bool success() const noexcept { return error.empty(); }
};

/** The render-thread state of one `EffectPassNode`. */
struct PassRuntime {
  std::shared_ptr<const ProgramSnapshot> applied;
  /** A pending program that has been tried and refused; not retried until a new one arrives. */
  std::shared_ptr<const ProgramSnapshot> rejected;
  /** The snapshot whose sources the worker is compiling, if any. */
  std::shared_ptr<const ProgramSnapshot> compiling;
  std::shared_ptr<std::future<AsyncCompileResult>> compileFuture;
  RenderProgram* program = nullptr;
  ParameterBlock values;
  std::vector<TextureBinding> textures;
  std::vector<SequenceState> sequences;
  EffectShaderState state = EffectShaderState::Empty;
  std::string diagnostics;
  uint32_t appliedVersion = 0;

  /** A compiled program waiting for its first pipeline; the previous program keeps drawing. */
  std::shared_ptr<const ProgramSnapshot> nextApplied;
  RenderProgram* nextProgram = nullptr;
  ParameterBlock nextValues;
  uint32_t nextVersion = 0;
  struct PendingPipelineState {
    std::atomic<bool> ready { false };
    std::atomic<bool> failed { false };
  };
  std::shared_ptr<PendingPipelineState> nextPipeline;
  bool nextPipelineStarted = false;

  /** Installs `nextProgram` once its pipeline is available. */
  void adoptNextProgram();

  /** Applies the node's snapshot: installs a compiled shader, or keeps the old one on failure. */
  void apply(EffectNode& node, const NodeSnapshot& snapshot, RenderCore& core, const FrameContext& context);
};

/** Executes one frame of a plan. Owned by the engine, created and destroyed on the render thread. */
class GraphExecutor final {
public:
  explicit GraphExecutor(RenderCore& core);
  ~GraphExecutor();

  GraphExecutor(const GraphExecutor&) = delete;
  GraphExecutor& operator=(const GraphExecutor&) = delete;

  /** Records and submits one frame. @return the failure that ended it, with @p error explaining. */
  EffectResult execute(const GraphPlan& plan, const FrameContext& context, std::string& error,
                       std::shared_ptr<FrameCompletion> completion = nullptr);

  /** Releases the runtimes of nodes that are no longer in the graph. */
  void prune(const std::unordered_set<NodeId>& live);

  void releaseResources();

  /** @return number of frames that used a direct output target. */
  [[nodiscard]] uint64_t directFrames() const noexcept { return mDirectFrames.load(std::memory_order_acquire); }

private:
  struct NodeState {
    std::unique_ptr<PassRuntime> pass;
    FrameView lastView;
    EffectFrame lastMetadata;
  };

  [[nodiscard]] PassRuntime& passRuntime(NodeId id);
  bool resolveTexture(RenderCore& core, const TextureSource& source,
                      const std::unordered_map<uint64_t, backend::TextureHandle>& frameTextures,
                      backend::TextureHandle& out, std::string& error) const;

  RenderCore& mCore;
  std::atomic<uint64_t> mDirectFrames { 0 };
  std::unordered_map<NodeId, NodeState> mNodes;
};

} // namespace coral::detail

#endif // CORAL_EFFECT_EXECUTOR_H
