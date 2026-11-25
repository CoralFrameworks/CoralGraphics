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

#ifndef CORAL_EFFECT_NODE_H
#define CORAL_EFFECT_NODE_H

#include <effect/EffectTypes.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace coral {

namespace detail {
struct NodeImpl;
struct NodeAccess;
class EngineImpl;
} // namespace detail

/** The three roles a node can play in the graph. */
enum class EffectNodeKind : uint8_t {
  Input,
  Pass,
  Output,
};

/**
 * The base class of every node in an effect graph.
 *
 * A node is a **facade**: it owns the application-side snapshot (name, enabled flag, shader description,
 * parameter values, texture bindings) and no GPU object at all. The engine's render thread owns the
 * matching runtime - programs, pipelines, textures - and reads the snapshot once per frame.
 *
 * Consequently every method here is cheap and thread-safe with respect to the engine; the ones that
 * change state (`setName`, `setEnabled`) only publish a new snapshot version. A node must be attached to
 * an engine before it can be connected, and its setters return `NotAttached` once it is detached.
 *
 * Instances are created by the engine (`createEffectNode`) or, for input/output nodes, by the
 * application's chosen node class; they are then held as `std::shared_ptr` and attached with
 * `EffectEngine::attach`.
 */
class EffectNode {
public:
  virtual ~EffectNode();

  EffectNode(const EffectNode&) = delete;
  EffectNode& operator=(const EffectNode&) = delete;

  /** @return the role this node plays; fixed for its lifetime. */
  [[nodiscard]] EffectNodeKind kind() const noexcept;

  /** @return the identity the engine assigned at `attach`, or 0 while detached. */
  [[nodiscard]] uint64_t id() const noexcept;

  /**
   * @return the node's display name, empty when none was set.
   *
   * The view points into the node's snapshot and is valid until the next `setName` on this node; a caller
   * that keeps it across a rename should copy it.
   */
  [[nodiscard]] std::string_view name() const noexcept;

  EffectResult setName(std::string_view name);

  [[nodiscard]] bool isAttached() const noexcept;

  [[nodiscard]] bool isEnabled() const noexcept;
  EffectResult setEnabled(bool enabled);

  /** @return how many input ports the node has: 0 for input nodes, 1 for output nodes. */
  [[nodiscard]] uint32_t inputCount() const noexcept;

  /** The most recent node-level diagnostic: shader compilation, parameter type, or pool failure. */
  [[nodiscard]] std::string diagnostics() const;

protected:
  /**
   * @param kind       the node's role.
   * @param inputCount initial number of input ports. An `EffectPassNode` starts at 1 and follows its
   *                   shader's declared input count.
   */
  explicit EffectNode(EffectNodeKind kind, uint32_t inputCount = 0);

private:
  friend struct detail::NodeAccess;
  friend class EffectEngine;

  std::unique_ptr<detail::NodeImpl> mImpl;
};

} // namespace coral

#endif // CORAL_EFFECT_NODE_H
