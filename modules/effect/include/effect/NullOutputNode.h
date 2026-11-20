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

#ifndef CORAL_NULL_OUTPUT_NODE_H
#define CORAL_NULL_OUTPUT_NODE_H

#include <effect/EffectOutputNode.h>

#include <memory>

namespace coral {

/**
 * An output node that consumes the frame and produces nothing.
 *
 * The graph still needs a sink, and "render but do not present" is a real configuration: an offline
 * calibration, a benchmark, or a chain whose last pass is being measured without a window.
 */
class NullOutputNode final : public EffectOutputNode {
public:
  static std::shared_ptr<NullOutputNode> create();

  ~NullOutputNode() override;

  [[nodiscard]] bool isReady() const noexcept override;

  EffectResult configure(const EffectOutputConfig& config) override;

private:
  NullOutputNode();
};

} // namespace coral

#endif // CORAL_NULL_OUTPUT_NODE_H
