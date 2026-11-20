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

#ifndef CORAL_EFFECT_PASS_NODE_H
#define CORAL_EFFECT_PASS_NODE_H

#include <effect/EffectNode.h>
#include <effect/EffectShader.h>
#include <effect/EffectTexture.h>
#include <effect/EffectTypes.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace coral {

/**
 * A node that renders one full-screen fragment pass.
 *
 * The node holds a **snapshot** - the shader description, its parameter declarations and values, and its
 * texture bindings - and nothing else. Compilation, pipeline creation and drawing happen on the engine's
 * render thread, which reads the snapshot at the start of a frame. That split is what lets an application
 * call `setParameter` at input-frame rate without ever touching a GPU object.
 *
 * A pass reads its graph inputs through `connect` (as `uInput0` .. `uInputN-1`) and its own textures
 * through `setTexture` (by sampler name). The two are separate: a graph input changes when the topology
 * changes, a bound texture when the application changes it.
 */
class EffectPassNode : public EffectNode {
public:
  ~EffectPassNode() override;

  /**
   * Submits a shader description.
   *
   * `Success` means the description was accepted into the snapshot, **not** that it has compiled: the
   * render thread compiles it before the next frame. While compilation is pending the previous shader
   * keeps rendering, and if compilation fails it keeps rendering still - a failed effect must not blank
   * the picture. `shaderState()` and `diagnostics()` report what happened.
   *
   * The call is refused with `InvalidArgument` when the description is structurally invalid (an empty
   * body in `Body` mode, an empty SPIR-V module in `SpirvModule` mode, duplicate parameter names, more
   * parameters than the block holds, or an `inputCount` that would orphan an existing connection). In
   * that case the current shader is unchanged.
   */
  EffectResult setShader(const EffectShaderDesc& shader);

  /** @return where the most recently submitted shader is in its lifecycle. */
  [[nodiscard]] EffectShaderState shaderState() const noexcept;

  EffectResult setParameter(std::string_view name, float value);
  EffectResult setParameter(std::string_view name, int32_t value);
  EffectResult setParameter(std::string_view name, const EffectVec2& value);
  EffectResult setParameter(std::string_view name, const EffectVec3& value);
  EffectResult setParameter(std::string_view name, const EffectVec4& value);
  EffectResult setParameter(std::string_view name, const EffectColor& value);
  EffectResult setParameter(std::string_view name, const EffectMatrix4& value);
  EffectResult setParameter(std::string_view name, const EffectValue& value);

  /** Binds a texture to the sampler called @p name. @return `NotFound` when the shader declares none. */
  EffectResult setTexture(std::string_view name, const TextureSource& texture);
  EffectResult clearTexture(std::string_view name);

  /** Starts playing @p name's texture sequence. The frames are uploaded lazily by the render thread. */
  EffectResult setTextureSequence(std::string_view name, EffectTextureSequenceDesc sequence);

  /** Drives @p name's sequence from the clock again (and sets the current time). */
  EffectResult setSequenceTime(std::string_view name, const RenderTime& time);

  /** Pins @p name's sequence to @p frameIndex until `setSequenceTime` or `clearSequenceOverride`. */
  EffectResult setSequenceFrame(std::string_view name, uint32_t frameIndex);

  /** Returns @p name's sequence to time-driven playback without changing its time. */
  EffectResult clearSequenceOverride(std::string_view name);

  /** Scale of the output relative to the engine's render size, e.g. 0.5 for a half-resolution pass. */
  EffectResult setOutputScale(float scale);

  /** Format of this pass's output frame: `RGBA8` or `RGBA16F`. */
  EffectResult setOutputFormat(EffectPixelFormat format);

  /** Requests direct rendering into the final output target when the executor supports it. */
  EffectResult setDirectToSurface(bool enabled);

  /** Selects which input supplies this pass output timestamp (default 0). */
  EffectResult setTimestampSource(uint32_t inputIndex);

  /** true when this pass asked for direct-to-surface rendering. */
  [[nodiscard]] bool directToSurface() const noexcept;

  /** The most recent shader compilation, parameter or texture diagnostic. */
  [[nodiscard]] std::string diagnostics() const;

protected:
  // Created by `EffectEngine::createEffectNode`; the public creation path is the engine factory.
  EffectPassNode();

private:
  friend class EffectEngine;
  friend struct detail::NodeAccess;
};

} // namespace coral

#endif // CORAL_EFFECT_PASS_NODE_H
