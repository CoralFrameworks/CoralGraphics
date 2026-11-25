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

#include "EffectInternal.h"

#include <utility>

namespace coral {

EffectNode::EffectNode(const EffectNodeKind kind, const uint32_t inputCount) {
  mImpl = std::make_unique<detail::NodeImpl>();
  mImpl->kind = kind;
  mImpl->inputCount.store(inputCount, std::memory_order_relaxed);
}

EffectNode::~EffectNode() = default;

EffectNodeKind EffectNode::kind() const noexcept { return mImpl->kind; }

uint64_t EffectNode::id() const noexcept { return mImpl->id; }

std::string_view EffectNode::name() const noexcept {
  // The returned view is valid until the next `setName` on this node. Publishing a view is what the
  // design specifies, and the alternative - a copy - would allocate in a per-frame log path.
  const std::lock_guard<std::mutex> lock(mImpl->mutex);
  return mImpl->snapshot.name;
}

EffectResult EffectNode::setName(const std::string_view name) {
  const std::lock_guard<std::mutex> lock(mImpl->mutex);
  if (mImpl->snapshot.name == name) return EffectResult::Success;
  mImpl->snapshot.name.assign(name);
  ++mImpl->snapshot.version;
  return EffectResult::Success;
}

bool EffectNode::isAttached() const noexcept { return mImpl->attached.load(std::memory_order_acquire); }

bool EffectNode::isEnabled() const noexcept { return mImpl->enabled.load(std::memory_order_acquire); }

EffectResult EffectNode::setEnabled(const bool enabled) {
  if (!isAttached()) return EffectResult::NotAttached;
  const std::lock_guard<std::mutex> lock(mImpl->mutex);
  if (mImpl->snapshot.enabled == enabled) return EffectResult::Success;
  mImpl->snapshot.enabled = enabled;
  mImpl->enabled.store(enabled, std::memory_order_release);
  ++mImpl->snapshot.version;
  return EffectResult::Success;
}

uint32_t EffectNode::inputCount() const noexcept { return mImpl->inputCount.load(std::memory_order_relaxed); }

std::string EffectNode::diagnostics() const {
  const std::lock_guard<std::mutex> lock(mImpl->mutex);
  return mImpl->snapshot.diagnostics;
}

} // namespace coral
