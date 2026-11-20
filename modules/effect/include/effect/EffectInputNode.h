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

#ifndef CORAL_EFFECT_INPUT_NODE_H
#define CORAL_EFFECT_INPUT_NODE_H

#include <effect/EffectNode.h>
#include <effect/PixelBufferView.h>

#include <memory>

namespace coral {

namespace detail {
class InputRuntime;
} // namespace detail

/**
 * The base class of nodes that introduce a frame into the graph.
 *
 * The core class only defines the lifecycle and the two questions the engine asks every frame. Turning an
 * external image into a frame the GPU can sample is the job of a concrete node - `TestPatternInputNode`
 * in the core library, a platform node in a platform target - which registers an internal
 * `detail::InputRuntime` when it is constructed. That runtime is where platform types stay: the public
 * header never names `CVPixelBufferRef`, `SurfaceTexture` or `ANativeWindow*`.
 */
class EffectInputNode : public EffectNode {
public:
  ~EffectInputNode() override;

  /** @return true when the node has a frame the engine has not consumed yet. */
  [[nodiscard]] virtual bool hasFrame() const noexcept = 0;

  /** @return true when the node repeats its last frame instead of producing an empty one. */
  [[nodiscard]] virtual bool repeatLastFrame() const noexcept = 0;

  virtual void setRepeatLastFrame(bool repeat) = 0;

  /**
   * Publishes one frame through the backend-neutral input contract.
   *
   * Concrete nodes override the paths they can consume: the CPU node copies planes, a platform node
   * imports its native texture. The default keeps older nodes source-compatible while the platform
   * implementations migrate.
   */
  virtual EffectResult submit(const PixelBufferView& view) {
    static_cast<void>(view);
    return EffectResult::Unsupported;
  }

protected:
  EffectInputNode();

  /**
   * Installs the runtime that produces frames.
   *
   * Called by a concrete node's constructor, after its own state is ready. The engine calls the runtime
   * on the render thread only; the node stays responsible for the thread-safe hand-off of submitted
   * frames.
   */
  void setInputRuntime(std::shared_ptr<detail::InputRuntime> runtime);

private:
  friend struct detail::NodeAccess;
};

} // namespace coral

#endif // CORAL_EFFECT_INPUT_NODE_H
