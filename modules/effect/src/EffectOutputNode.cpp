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

EffectOutputNode::EffectOutputNode() : EffectNode(EffectNodeKind::Output, 1) {}

EffectOutputNode::~EffectOutputNode() = default;

void EffectOutputNode::setOutputRuntime(std::shared_ptr<detail::OutputRuntime> runtime) {
  detail::NodeAccess::setOutputRuntime(*this, std::move(runtime));
}

void EffectOutputNode::release() {
  // Dropping the runtime destroys it, and its destructor releases textures/views through the device
  // captured when the slot ring was created.
  setOutputRuntime(nullptr);
}

} // namespace coral
