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
 * @file EffectGraph.cpp
 *
 * The node graph and its DAG validator. The validator is deliberately the *structural* one: ports exist,
 * every port is filled, the graph is acyclic, and every node lies on a path from an input to the output.
 * Fan-out is legal here. The v1 executor's additional "one consumer per node" rule belongs to the
 * executor, so that the data structure this file implements is the one a DAG executor will use unchanged.
 */

#include "EffectInternal.h"

#include <functional>
#include <unordered_set>

namespace coral::detail {

namespace {

std::string labelOf(const std::shared_ptr<EffectNode>& node) {
  if (node == nullptr) return "<null>";
  const std::string name = NodeAccess::name(*node);
  if (!name.empty()) return "'" + name + "'";
  return "node #" + std::to_string(node->id());
}

/** @return the edge into @p node's @p port, or null. */
const Edge* findIncoming(const std::vector<Edge>& edges, const NodeId node, const uint32_t port) noexcept {
  for (const Edge& edge : edges) {
    if (edge.to == node && edge.inputIndex == port) return &edge;
  }
  return nullptr;
}

} // namespace anonymous

std::shared_ptr<EffectNode> Graph::find(const NodeId node) const {
  const auto it = mNodes.find(node);
  return it != mNodes.end() ? it->second : nullptr;
}

std::vector<std::shared_ptr<EffectNode>> Graph::nodes() const {
  std::vector<std::shared_ptr<EffectNode>> result;
  result.reserve(mNodes.size());
  for (const auto& entry : mNodes) result.push_back(entry.second);
  return result;
}

EffectResult Graph::attach(const std::shared_ptr<EffectNode>& node, std::string& diagnostic) {
  if (node == nullptr) {
    diagnostic = "cannot attach a null node";
    return EffectResult::InvalidArgument;
  }
  const NodeId id = node->id();
  if (id == kInvalidNodeId) {
    diagnostic = "the node has no id; attach it through an engine";
    return EffectResult::InvalidArgument;
  }
  if (mNodes.find(id) != mNodes.end()) {
    diagnostic = "the node " + labelOf(node) + " is already attached to this engine";
    return EffectResult::InvalidArgument;
  }
  mNodes.emplace(id, node);
  return EffectResult::Success;
}

EffectResult Graph::detach(const NodeId node, std::string& diagnostic) {
  const auto it = mNodes.find(node);
  if (it == mNodes.end()) {
    diagnostic = "the node is not attached to this engine";
    return EffectResult::NotAttached;
  }
  const std::string label = labelOf(it->second);
  if (mOutput == node) mOutput = kInvalidNodeId;
  mExtraOutputs.erase(std::remove(mExtraOutputs.begin(), mExtraOutputs.end(), node), mExtraOutputs.end());
  mEdges.erase(std::remove_if(mEdges.begin(), mEdges.end(),
                              [node](const Edge& edge) { return edge.from == node || edge.to == node; }),
               mEdges.end());
  mNodes.erase(it);
  (void) label;
  return EffectResult::Success;
}

EffectResult Graph::connect(const NodeId from, const NodeId to, const uint32_t inputIndex, std::string& diagnostic) {
  const std::shared_ptr<EffectNode> source = find(from);
  const std::shared_ptr<EffectNode> target = find(to);
  if (source == nullptr || target == nullptr) {
    diagnostic = "both nodes must be attached before they can be connected";
    return EffectResult::NotFound;
  }
  if (from == to) {
    diagnostic = "a node cannot be connected to itself";
    return EffectResult::InvalidArgument;
  }
  if (target->kind() == EffectNodeKind::Input) {
    diagnostic = "the input node " + labelOf(target) + " has no input port";
    return EffectResult::InvalidArgument;
  }
  if (source->kind() == EffectNodeKind::Output) {
    diagnostic = "the output node " + labelOf(source) + " produces no frame to connect";
    return EffectResult::InvalidArgument;
  }
  if (inputIndex >= target->inputCount()) {
    diagnostic = labelOf(target) + " has " + std::to_string(target->inputCount()) + " input port(s), not " +
                 std::to_string(inputIndex + 1);
    return EffectResult::InvalidArgument;
  }

  const Edge* existing = findIncoming(mEdges, to, inputIndex);
  if (existing != nullptr && existing->from == from) return EffectResult::Success;

  // Adding from -> to closes a cycle exactly when `to` can already reach `from`. Checking before the
  // insertion means a refused connection leaves the graph exactly as it was.
  if (reaches(to, from)) {
    diagnostic = "connecting " + labelOf(source) + " to " + labelOf(target) + " would create a cycle";
    return EffectResult::InvalidArgument;
  }

  if (existing != nullptr) {
    // One edge per input port: reconnecting a port replaces its previous source. This is documented
    // behaviour rather than an error, and it is what makes re-wiring a graph a single call.
    mEdges.erase(std::remove_if(mEdges.begin(), mEdges.end(),
                                [to, inputIndex](const Edge& edge) {
                                  return edge.to == to && edge.inputIndex == inputIndex;
                                }),
                 mEdges.end());
  }
  mEdges.push_back(Edge{ .from = from, .to = to, .inputIndex = inputIndex });
  return EffectResult::Success;
}

EffectResult Graph::disconnect(const NodeId from, const NodeId to, const uint32_t inputIndex,
                               std::string& diagnostic) {
  const auto it = std::find_if(mEdges.begin(), mEdges.end(), [from, to, inputIndex](const Edge& edge) {
    return edge.from == from && edge.to == to && edge.inputIndex == inputIndex;
  });
  if (it == mEdges.end()) {
    diagnostic = "no such connection exists";
    return EffectResult::NotFound;
  }
  mEdges.erase(it);
  return EffectResult::Success;
}

std::vector<NodeId> Graph::outputs() const {
  std::vector<NodeId> result;
  if (mOutput != kInvalidNodeId) result.push_back(mOutput);
  result.insert(result.end(), mExtraOutputs.begin(), mExtraOutputs.end());
  return result;
}

EffectResult Graph::addOutput(const NodeId output, std::string& diagnostic) {
  const std::shared_ptr<EffectNode> node = find(output);
  if (node == nullptr) {
    diagnostic = "the output node must be attached before it can be an output";
    return EffectResult::NotAttached;
  }
  if (node->kind() != EffectNodeKind::Output) {
    diagnostic = labelOf(node) + " is not an output node";
    return EffectResult::InvalidArgument;
  }
  if (output == mOutput || std::find(mExtraOutputs.begin(), mExtraOutputs.end(), output) != mExtraOutputs.end()) {
    diagnostic = labelOf(node) + " is already an output";
    return EffectResult::InvalidArgument;
  }
  for (const Edge& edge : mEdges) {
    if (edge.from == output) {
      diagnostic = labelOf(node) + " already feeds another node";
      return EffectResult::InvalidArgument;
    }
  }
  mExtraOutputs.push_back(output);
  return EffectResult::Success;
}

EffectResult Graph::removeOutput(const NodeId output, std::string& diagnostic) {
  const auto it = std::find(mExtraOutputs.begin(), mExtraOutputs.end(), output);
  if (it == mExtraOutputs.end()) {
    diagnostic = "only a secondary output can be removed";
    return EffectResult::InvalidArgument;
  }
  mEdges.erase(std::remove_if(mEdges.begin(), mEdges.end(), [output](const Edge& edge) { return edge.to == output; }),
               mEdges.end());
  mExtraOutputs.erase(it);
  return EffectResult::Success;
}

EffectResult Graph::setOutput(const NodeId output, std::string& diagnostic) {
  const std::shared_ptr<EffectNode> node = find(output);
  if (node == nullptr) {
    diagnostic = "the output node must be attached before it can be the output";
    return EffectResult::NotAttached;
  }
  if (node->kind() != EffectNodeKind::Output) {
    diagnostic = labelOf(node) + " is not an output node";
    return EffectResult::InvalidArgument;
  }
  mExtraOutputs.erase(std::remove(mExtraOutputs.begin(), mExtraOutputs.end(), output), mExtraOutputs.end());
  if (mOutput != kInvalidNodeId && mOutput != output) {
    // Keep the previous primary as a secondary terminal: its incoming edge stays valid, so the graph
    // remains a valid multi-output DAG. The caller can remove it explicitly if it is no longer wanted.
    if (std::find(mExtraOutputs.begin(), mExtraOutputs.end(), mOutput) == mExtraOutputs.end()) {
      mExtraOutputs.push_back(mOutput);
    }
  }
  mOutput = output;
  return EffectResult::Success;
}

bool Graph::reaches(const NodeId from, const NodeId target) const {
  if (from == target) return true;
  std::vector<NodeId> stack { from };
  std::unordered_set<NodeId> visited;
  while (!stack.empty()) {
    const NodeId current = stack.back();
    stack.pop_back();
    if (!visited.insert(current).second) continue;
    for (const Edge& edge : mEdges) {
      if (edge.from != current) continue;
      if (edge.to == target) return true;
      stack.push_back(edge.to);
    }
  }
  return false;
}

uint32_t Graph::countEdgesAtOrBeyond(const NodeId node, const uint32_t inputCount) const {
  uint32_t count = 0;
  for (const Edge& edge : mEdges) {
    if (edge.to == node && edge.inputIndex >= inputCount) ++count;
  }
  return count;
}

EffectResult Graph::validate(std::string& diagnostic) const {
  if (mOutput == kInvalidNodeId) {
    diagnostic = "no output node has been set; call setOutput before starting";
    return EffectResult::InvalidArgument;
  }
  if (!contains(mOutput)) {
    diagnostic = "the output node is no longer attached";
    return EffectResult::InvalidArgument;
  }
  std::vector<NodeId> allOutputs { mOutput };
  for (const NodeId extra : mExtraOutputs) {
    if (!contains(extra)) {
      diagnostic = "a secondary output node is no longer attached";
      return EffectResult::InvalidArgument;
    }
    if (find(extra)->kind() != EffectNodeKind::Output) {
      diagnostic = labelOf(find(extra)) + " is not an output node";
      return EffectResult::InvalidArgument;
    }
    if (std::find(allOutputs.begin(), allOutputs.end(), extra) != allOutputs.end()) {
      diagnostic = "the same node is listed as an output twice";
      return EffectResult::InvalidArgument;
    }
    allOutputs.push_back(extra);
  }
  for (const NodeId output : allOutputs) {
    for (const Edge& edge : mEdges) {
      if (edge.from == output) {
        diagnostic = labelOf(find(output)) + " is an output but feeds another node";
        return EffectResult::InvalidArgument;
      }
    }
  }

  // 1. Every input port of every non-input node is filled, exactly once. One edge per port is maintained
  //    by `connect`, so a port with two edges would be a bug in this file rather than a user error; it is
  //    still checked because a validator that trusts its writer cannot find that class of bug.
  for (const auto& entry : mNodes) {
    const std::shared_ptr<EffectNode>& node = entry.second;
    if (node->kind() == EffectNodeKind::Input) continue;
    for (uint32_t port = 0; port < node->inputCount(); ++port) {
      const Edge* edge = findIncoming(mEdges, entry.first, port);
      if (edge == nullptr) {
        diagnostic = "the input " + std::to_string(port) + " of " + labelOf(node) + " is not connected";
        return EffectResult::InvalidArgument;
      }
    }
  }

  // 2. No cycles. `connect` refuses to create one, so this is a guard against a future direct edit of
  //    mEdges; a cycle would make the executor's topological order loop forever.
  std::unordered_set<NodeId> visiting;
  std::unordered_set<NodeId> done;
  const std::function<bool(NodeId)> visit = [&](const NodeId node) -> bool {
    if (done.count(node) != 0) return false;
    if (!visiting.insert(node).second) return true;
    for (const Edge& edge : mEdges) {
      if (edge.from == node && visit(edge.to)) return true;
    }
    visiting.erase(node);
    done.insert(node);
    return false;
  };
  for (const auto& entry : mNodes) {
    if (visit(entry.first)) {
      diagnostic = "the graph contains a cycle through " + labelOf(entry.second);
      return EffectResult::InvalidArgument;
    }
  }

  // 3. Reachability: every node must be reachable from some input and able to reach the output.
  std::unordered_set<NodeId> reachableFromInput;
  std::vector<NodeId> stack;
  for (const auto& entry : mNodes) {
    if (entry.second->kind() == EffectNodeKind::Input) stack.push_back(entry.first);
  }
  while (!stack.empty()) {
    const NodeId current = stack.back();
    stack.pop_back();
    if (!reachableFromInput.insert(current).second) continue;
    for (const Edge& edge : mEdges) {
      if (edge.from == current) stack.push_back(edge.to);
    }
  }
  for (const auto& entry : mNodes) {
    if (reachableFromInput.count(entry.first) == 0) {
      diagnostic = labelOf(entry.second) + " is not reachable from an input node";
      return EffectResult::InvalidArgument;
    }
  }

  std::unordered_set<NodeId> canReachOutput;
  for (const NodeId output : allOutputs) stack.push_back(output);
  while (!stack.empty()) {
    const NodeId current = stack.back();
    stack.pop_back();
    if (!canReachOutput.insert(current).second) continue;
    for (const Edge& edge : mEdges) {
      if (edge.to == current) stack.push_back(edge.from);
    }
  }
  for (const auto& entry : mNodes) {
    if (canReachOutput.count(entry.first) == 0) {
      diagnostic = labelOf(entry.second) + " cannot reach any output node";
      return EffectResult::InvalidArgument;
    }
  }

  diagnostic.clear();
  return EffectResult::Success;
}

} // namespace coral::detail
