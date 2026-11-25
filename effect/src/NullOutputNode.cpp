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

#include <effect/NullOutputNode.h>

#include "EffectInternal.h"
#include "EffectRenderCore.h"

#include <mutex>

namespace coral {

namespace {

class NullRuntime final : public coral::detail::OutputRuntime {
public:
  bool consume(coral::detail::FrameContext& /*context*/, const coral::detail::FrameView& view,
               std::string& error) override {
    if (!view.isValid()) {
      error = "the null output received an invalid frame";
      return false;
    }
    return true;
  }

};

} // namespace anonymous

NullOutputNode::NullOutputNode() {
  setOutputRuntime(std::make_shared<NullRuntime>());
}

NullOutputNode::~NullOutputNode() = default;

std::shared_ptr<NullOutputNode> NullOutputNode::create() {
  return std::shared_ptr<NullOutputNode>(new NullOutputNode());
}

bool NullOutputNode::isReady() const noexcept { return true; }

EffectResult NullOutputNode::configure(const EffectOutputConfig& config) {
  if (config.format != EffectPixelFormat::RGBA8) return EffectResult::Unsupported;
  return EffectResult::Success;
}

} // namespace coral
