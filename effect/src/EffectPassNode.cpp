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
 * @file EffectPassNode.cpp
 *
 * The application-side half of a pass: shader submission, parameter type checking, texture binding and
 * sequence state. Everything here only writes the node's snapshot; the render thread compiles and draws
 * from a copy of it.
 */

#include "EffectInternal.h"

#include <cmath>
#include <cstring>
#include <utility>

namespace coral {

namespace {

/** Copies the values of parameters that survive into @p destination, by name and type. */
void carryParameters(const std::shared_ptr<const detail::ProgramSnapshot>& source, const detail::ParameterBlock& values,
                     const std::shared_ptr<const detail::ProgramSnapshot>& destination,
                     detail::ParameterBlock& destinationValues) {
  destinationValues.reset();
  if (source == nullptr || destination == nullptr) return;
  for (const detail::ParameterSlot& parameter : destination->parameters) {
    const detail::ParameterSlot* previous = source->find(parameter.name);
    if (previous == nullptr || previous->type != parameter.type) continue;
    const float* from = values.slot(previous->index + detail::kFrameworkSlotCount);
    float* to = destinationValues.slot(parameter.index + detail::kFrameworkSlotCount);
    if (from == nullptr || to == nullptr) continue;
    std::memcpy(to, from, sizeof(float) * 4 * parameter.slotCount);
  }
}

/**
 * Re-composes the pending program after the set of bound texture names changed.
 *
 * Adding a texture adds a sampler declaration to the generated source, so the shader that was compiled
 * before the texture existed cannot sample it. The old program keeps rendering until this one compiles,
 * which is the same rule a failed `setShader` follows.
 */
void rebuildForTextures(detail::NodeImpl& state, const std::shared_ptr<const detail::ProgramSnapshot>& base) {
  if (base == nullptr) return;

  std::vector<std::string> names;
  names.reserve(state.snapshot.textures.size() + state.snapshot.sequences.size());
  for (const detail::TextureBinding& binding : state.snapshot.textures) names.push_back(binding.name);
  for (const detail::SequenceState& sequence : state.snapshot.sequences) names.push_back(sequence.name);

  std::string diagnostic;
  std::shared_ptr<const detail::ProgramSnapshot> program =
      detail::buildProgramSnapshot(base->desc, names, diagnostic);
  if (program == nullptr) {
    state.snapshot.diagnostics = std::move(diagnostic);
    return;
  }

  const bool hasPending = state.snapshot.pendingProgram != nullptr;
  detail::ParameterBlock carried;
  carryParameters(base, hasPending ? state.snapshot.pendingValues : state.snapshot.values, program, carried);
  state.snapshot.pendingValues = carried;
  state.snapshot.pendingProgram = std::move(program);
  state.snapshot.shaderState = EffectShaderState::Pending;
  state.shaderState.store(EffectShaderState::Pending, std::memory_order_release);
  state.snapshot.diagnostics.clear();
}

} // namespace anonymous

EffectPassNode::EffectPassNode() : EffectNode(EffectNodeKind::Pass, 1) {
  detail::NodeImpl& state = detail::NodeAccess::impl(*this);
  const std::lock_guard<std::mutex> lock(state.mutex);
  state.snapshot.shaderState = EffectShaderState::Empty;
}

EffectPassNode::~EffectPassNode() = default;

EffectResult EffectPassNode::setShader(const EffectShaderDesc& shader) {
  if (!isAttached()) return EffectResult::NotAttached;

  // The node's bound textures are part of the composed program: a Body shader refers to one by the name
  // the caller bound, and the framework has to declare the sampler before the body is compiled. Read them
  // before taking the engine lock, so the two locks are never held together.
  std::vector<std::string> textureNames;
  {
    detail::NodeImpl& state = detail::NodeAccess::impl(*this);
    const std::lock_guard<std::mutex> lock(state.mutex);
    for (const detail::TextureBinding& binding : state.snapshot.textures) textureNames.push_back(binding.name);
    for (const detail::SequenceState& sequence : state.snapshot.sequences) textureNames.push_back(sequence.name);
  }

  std::string diagnostic;
  std::shared_ptr<const detail::ProgramSnapshot> program =
      detail::buildProgramSnapshot(shader, textureNames, diagnostic);
  if (program == nullptr) {
    detail::NodeAccess::setDiagnostics(*this, diagnostic);
    return EffectResult::InvalidArgument;
  }

  // A shader may not orphan an existing connection: shrinking the input count below an occupied port
  // would leave an edge the executor can neither bind nor ignore.
  const std::shared_ptr<detail::EngineImpl> engine = detail::NodeAccess::engine(*this);
  if (engine == nullptr) return EffectResult::NotAttached;
  const EffectResult ports = engine->checkInputPorts(id(), shader.inputCount, diagnostic);
  if (ports != EffectResult::Success) {
    detail::NodeAccess::setDiagnostics(*this, diagnostic);
    return ports;
  }

  detail::NodeImpl& state = detail::NodeAccess::impl(*this);
  const std::lock_guard<std::mutex> lock(state.mutex);
  detail::NodeSnapshot& snapshot = state.snapshot;

  // While a shader is pending, the newest values live there; otherwise they live in the installed
  // program's block. Carrying them over is what lets a caller change a shader without every animated
  // parameter snapping back to zero.
  const bool hasPending = snapshot.pendingProgram != nullptr;
  const std::shared_ptr<const detail::ProgramSnapshot> source =
      hasPending ? snapshot.pendingProgram : snapshot.program;
  const detail::ParameterBlock values = hasPending ? snapshot.pendingValues : snapshot.values;

  detail::ParameterBlock carried;
  carryParameters(source, values, program, carried);
  snapshot.pendingValues = carried;
  snapshot.pendingProgram = std::move(program);
  snapshot.shaderState = EffectShaderState::Pending;
  snapshot.diagnostics.clear();
  ++snapshot.version;

  state.shaderState.store(EffectShaderState::Pending, std::memory_order_release);
  state.inputCount.store(shader.inputCount, std::memory_order_relaxed);
  return EffectResult::Success;
}

EffectShaderState EffectPassNode::shaderState() const noexcept {
  return detail::NodeAccess::impl(*this).shaderState.load(std::memory_order_acquire);
}

EffectResult EffectPassNode::setParameter(const std::string_view name, const EffectValue& value) {
  if (!isAttached()) return EffectResult::NotAttached;

  detail::NodeImpl& state = detail::NodeAccess::impl(*this);
  const std::lock_guard<std::mutex> lock(state.mutex);
  detail::NodeSnapshot& snapshot = state.snapshot;

  const bool hasPending = snapshot.pendingProgram != nullptr;
  const std::shared_ptr<const detail::ProgramSnapshot>& program =
      hasPending ? snapshot.pendingProgram : snapshot.program;
  if (program == nullptr) {
    snapshot.diagnostics = snapshot.shaderState == EffectShaderState::Failed
                               ? "the last shader this node was given did not compile, so it declares no parameter"
                               : "no shader has been set, so no parameter is declared";
    return EffectResult::InvalidArgument;
  }

  const detail::ParameterSlot* parameter = program->find(name);
  if (parameter == nullptr) {
    snapshot.diagnostics = "the shader declares no parameter called '" + std::string(name) + "'";
    return EffectResult::InvalidArgument;
  }

  detail::ParameterBlock& block = hasPending ? snapshot.pendingValues : snapshot.values;
  std::string diagnostic;
  if (!detail::writeParameter(block, *parameter, value, diagnostic)) {
    snapshot.diagnostics = std::move(diagnostic);
    return EffectResult::InvalidArgument;
  }
  // A successful write does not erase the message of a shader that failed to compile: that message is
  // the only record of why the pass is still drawing its previous program.
  if (snapshot.shaderState != EffectShaderState::Failed) snapshot.diagnostics.clear();
  ++snapshot.version;
  return EffectResult::Success;
}

EffectResult EffectPassNode::setParameter(const std::string_view name, const float value) {
  return setParameter(name, EffectValue::fromFloat(value));
}

EffectResult EffectPassNode::setParameter(const std::string_view name, const int32_t value) {
  return setParameter(name, EffectValue::fromInt(value));
}

EffectResult EffectPassNode::setParameter(const std::string_view name, const EffectVec2& value) {
  return setParameter(name, EffectValue::fromVec2(value));
}

EffectResult EffectPassNode::setParameter(const std::string_view name, const EffectVec3& value) {
  return setParameter(name, EffectValue::fromVec3(value));
}

EffectResult EffectPassNode::setParameter(const std::string_view name, const EffectVec4& value) {
  return setParameter(name, EffectValue::fromVec4(value));
}

EffectResult EffectPassNode::setParameter(const std::string_view name, const EffectColor& value) {
  return setParameter(name, EffectValue::fromColor(value));
}

EffectResult EffectPassNode::setParameter(const std::string_view name, const EffectMatrix4& value) {
  return setParameter(name, EffectValue::fromMatrix4(value));
}

EffectResult EffectPassNode::setTexture(const std::string_view name, const TextureSource& texture) {
  if (!isAttached()) return EffectResult::NotAttached;
  if (name.empty()) return EffectResult::InvalidArgument;

  detail::NodeImpl& state = detail::NodeAccess::impl(*this);
  const std::lock_guard<std::mutex> lock(state.mutex);
  for (detail::TextureBinding& binding : state.snapshot.textures) {
    if (binding.name == name) {
      binding.source = texture;
      ++state.snapshot.version;
      return EffectResult::Success;
    }
  }
  state.snapshot.textures.push_back(detail::TextureBinding{ std::string(name), texture });
  const std::shared_ptr<const detail::ProgramSnapshot> base =
      state.snapshot.pendingProgram != nullptr ? state.snapshot.pendingProgram : state.snapshot.program;
  rebuildForTextures(state, base);
  ++state.snapshot.version;
  return EffectResult::Success;
}

EffectResult EffectPassNode::clearTexture(const std::string_view name) {
  if (!isAttached()) return EffectResult::NotAttached;

  detail::NodeImpl& state = detail::NodeAccess::impl(*this);
  const std::lock_guard<std::mutex> lock(state.mutex);
  auto& bindings = state.snapshot.textures;
  const auto it = std::find_if(bindings.begin(), bindings.end(),
                               [name](const detail::TextureBinding& binding) { return binding.name == name; });
  if (it == bindings.end()) return EffectResult::NotFound;
  bindings.erase(it);
  // Removing a sampler changes the generated source just as adding one did, so the program is recomposed
  // without it. The installed program keeps rendering until the new one compiles.
  {
    const std::shared_ptr<const detail::ProgramSnapshot> base =
        state.snapshot.pendingProgram != nullptr ? state.snapshot.pendingProgram : state.snapshot.program;
    rebuildForTextures(state, base);
  }
  ++state.snapshot.version;
  return EffectResult::Success;
}

EffectResult EffectPassNode::setTextureSequence(const std::string_view name, EffectTextureSequenceDesc sequence) {
  if (!isAttached()) return EffectResult::NotAttached;
  if (name.empty() || sequence.frames.empty()) return EffectResult::InvalidArgument;
  if (sequence.frameDuration.timescale == 0 || sequence.frameDuration.value <= 0) return EffectResult::InvalidArgument;

  detail::NodeImpl& state = detail::NodeAccess::impl(*this);
  const std::lock_guard<std::mutex> lock(state.mutex);
  auto& sequences = state.snapshot.sequences;
  for (detail::SequenceState& existing : sequences) {
    if (existing.name == name) {
      existing.desc = std::move(sequence);
      existing.timeDriven = true;
      existing.explicitTime = false;
      existing.frameIndex = 0;
      ++existing.version;
      ++state.snapshot.version;
      return EffectResult::Success;
    }
  }
  detail::SequenceState fresh;
  fresh.name = std::string(name);
  fresh.desc = std::move(sequence);
  fresh.timeDriven = true;
  fresh.explicitTime = false;
  fresh.version = 1;
  sequences.push_back(std::move(fresh));
  // A sequence is a sampler too, so the generated program has to declare it before the body can read it.
  const std::shared_ptr<const detail::ProgramSnapshot> base =
      state.snapshot.pendingProgram != nullptr ? state.snapshot.pendingProgram : state.snapshot.program;
  rebuildForTextures(state, base);
  ++state.snapshot.version;
  return EffectResult::Success;
}

namespace {

detail::SequenceState* findSequence(detail::NodeSnapshot& snapshot, const std::string_view name) noexcept {
  for (detail::SequenceState& sequence : snapshot.sequences) {
    if (sequence.name == name) return &sequence;
  }
  return nullptr;
}

} // namespace anonymous

EffectResult EffectPassNode::setSequenceTime(const std::string_view name, const RenderTime& time) {
  if (!isAttached()) return EffectResult::NotAttached;
  if (!time.isValid()) return EffectResult::InvalidArgument;

  detail::NodeImpl& state = detail::NodeAccess::impl(*this);
  const std::lock_guard<std::mutex> lock(state.mutex);
  detail::SequenceState* sequence = findSequence(state.snapshot, name);
  if (sequence == nullptr) return EffectResult::NotFound;
  sequence->time = time;
  sequence->timeDriven = true;
  sequence->explicitTime = true;
  ++state.snapshot.version;
  return EffectResult::Success;
}

EffectResult EffectPassNode::setSequenceFrame(const std::string_view name, const uint32_t frameIndex) {
  if (!isAttached()) return EffectResult::NotAttached;

  detail::NodeImpl& state = detail::NodeAccess::impl(*this);
  const std::lock_guard<std::mutex> lock(state.mutex);
  detail::SequenceState* sequence = findSequence(state.snapshot, name);
  if (sequence == nullptr) return EffectResult::NotFound;
  if (frameIndex >= sequence->desc.frames.size()) return EffectResult::InvalidArgument;
  sequence->frameIndex = frameIndex;
  sequence->timeDriven = false;
  ++state.snapshot.version;
  return EffectResult::Success;
}

EffectResult EffectPassNode::clearSequenceOverride(const std::string_view name) {
  if (!isAttached()) return EffectResult::NotAttached;

  detail::NodeImpl& state = detail::NodeAccess::impl(*this);
  const std::lock_guard<std::mutex> lock(state.mutex);
  detail::SequenceState* sequence = findSequence(state.snapshot, name);
  if (sequence == nullptr) return EffectResult::NotFound;
  sequence->timeDriven = true;
  sequence->explicitTime = false;
  ++state.snapshot.version;
  return EffectResult::Success;
}

EffectResult EffectPassNode::setOutputScale(const float scale) {
  if (!isAttached()) return EffectResult::NotAttached;
  if (!std::isfinite(scale) || scale <= 0.0f) return EffectResult::InvalidArgument;

  detail::NodeImpl& state = detail::NodeAccess::impl(*this);
  const std::lock_guard<std::mutex> lock(state.mutex);
  state.snapshot.outputScale = scale;
  ++state.snapshot.version;
  return EffectResult::Success;
}

EffectResult EffectPassNode::setOutputFormat(const EffectPixelFormat format) {
  if (!isAttached()) return EffectResult::NotAttached;
  if (format != EffectPixelFormat::RGBA8 && format != EffectPixelFormat::RGBA16F) {
    detail::NodeAccess::setDiagnostics(*this, "pass outputs support RGBA8 and RGBA16F");
    return EffectResult::Unsupported;
  }
  detail::NodeImpl& state = detail::NodeAccess::impl(*this);
  const std::lock_guard<std::mutex> lock(state.mutex);
  state.snapshot.outputFormat = format;
  ++state.snapshot.version;
  return EffectResult::Success;
}

EffectResult EffectPassNode::setDirectToSurface(const bool enabled) {
  if (!isAttached()) return EffectResult::NotAttached;
  detail::NodeImpl& state = detail::NodeAccess::impl(*this);
  state.directToSurface.store(enabled, std::memory_order_release);
  return EffectResult::Success;
}

bool EffectPassNode::directToSurface() const noexcept {
  return detail::NodeAccess::impl(*this).directToSurface.load(std::memory_order_acquire);
}

EffectResult EffectPassNode::setTimestampSource(const uint32_t inputIndex) {
  if (!isAttached()) return EffectResult::NotAttached;
  detail::NodeImpl& state = detail::NodeAccess::impl(*this);
  const std::lock_guard<std::mutex> lock(state.mutex);
  state.snapshot.timestampInput = inputIndex;
  ++state.snapshot.version;
  return EffectResult::Success;
}

std::string EffectPassNode::diagnostics() const { return EffectNode::diagnostics(); }

} // namespace coral
