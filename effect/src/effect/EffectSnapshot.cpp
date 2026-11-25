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
 * @file EffectSnapshot.cpp
 *
 * Device-free helpers for the snapshot protocol: hashing a shader description, resolving its parameter
 * layout, and writing a typed value into a parameter block. None of it touches the RHI, which is why it
 * is testable without a GPU and shared by the node and the render core.
 */

#include "EffectInternal.h"

#include <cstring>
#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace coral::detail {

namespace {

constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

const char* authoringName(const EffectShaderAuthoring authoring) noexcept {
  return authoring == EffectShaderAuthoring::Body ? "body" : "spirv";
}

const char* valueTypeName(const EffectValueType type) noexcept {
  switch (type) {
    case EffectValueType::Float:
      return "float";
    case EffectValueType::Int:
      return "int";
    case EffectValueType::Vec2:
      return "vec2";
    case EffectValueType::Vec3:
      return "vec3";
    case EffectValueType::Vec4:
      return "vec4";
    case EffectValueType::Color:
      return "color";
    case EffectValueType::Matrix4:
      return "matrix4";
  }
  return "unknown";
}

} // namespace anonymous

uint64_t hashText(const std::string_view text, const uint64_t seed) noexcept {
  uint64_t hash = seed;
  for (const char character : text) {
    hash ^= static_cast<uint64_t>(static_cast<unsigned char>(character));
    hash *= kFnvPrime;
  }
  return hash;
}

uint64_t hashValue(const uint64_t seed, const uint64_t value) noexcept {
  uint64_t hash = seed;
  for (int shift = 0; shift < 64; shift += 8) {
    hash ^= (value >> shift) & 0xFFu;
    hash *= kFnvPrime;
  }
  return hash;
}

uint64_t hashProgram(const EffectShaderDesc& shader, const std::vector<ParameterSlot>& parameters,
                     const std::vector<std::string>& textureNames, const uint32_t externalInputMask) noexcept {
  uint64_t hash = kFnvOffset;
  hash = hashText(authoringName(shader.authoring), hash);
  hash = hashText(shader.vertexSource, hash);
  hash = hashText(shader.fragmentSource, hash);
  // The SPIR-V words are hashed bytewise, which is what makes two modules that differ only in a
  // constant value different cache entries rather than one stale one.
  for (const uint32_t word : shader.fragmentSpirv) hash = hashValue(hash, word);
  for (const uint32_t word : shader.vertexSpirv) hash = hashValue(hash, word);
  hash = hashValue(hash, shader.inputCount);
  hash = hashValue(hash, externalInputMask);
  for (const ParameterSlot& parameter : parameters) {
    hash = hashText(parameter.name, hash);
    hash = hashValue(hash, static_cast<uint64_t>(parameter.type));
  }
  for (const std::string& name : textureNames) hash = hashText(name, hash);
  return hash;
}

namespace {

bool isIdentifier(const std::string_view name) noexcept {
  if (name.empty()) return false;
  const auto first = static_cast<unsigned char>(name.front());
  if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '_')) return false;
  for (const char character : name) {
    const auto value = static_cast<unsigned char>(character);
    if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
          value == '_')) {
      return false;
    }
  }
  return true;
}

} // namespace anonymous

const ParameterSlot* ProgramSnapshot::find(const std::string_view name) const noexcept {
  for (const ParameterSlot& parameter : parameters) {
    if (parameter.name == name) return &parameter;
  }
  return nullptr;
}

const ParameterSlot* findParameter(const std::shared_ptr<const ProgramSnapshot>& program,
                                   const std::string_view name) noexcept {
  return program != nullptr ? program->find(name) : nullptr;
}

std::shared_ptr<const ProgramSnapshot> buildProgramSnapshot(const EffectShaderDesc& shader,
                                                            const std::vector<std::string>& textureNames,
                                                            std::string& diagnostic) {
  if (shader.authoring == EffectShaderAuthoring::Body) {
    if (shader.fragmentSource.empty()) {
      diagnostic = "a Body shader needs a non-empty fragmentSource";
      return nullptr;
    }
  } else if (shader.fragmentSpirv.empty()) {
    diagnostic = "a SpirvModule shader needs a non-empty fragmentSpirv";
    return nullptr;
  }

  auto program = std::make_shared<ProgramSnapshot>();
  program->desc = shader;
  program->externalInputMask = shader.externalInput ? 1u : 0u;
  // `label` is a raw pointer into caller memory. Copy it and clear the pointer before the description is
  // retained, so a snapshot can outlive the stack frame that built the description.
  program->label = shader.label != nullptr ? shader.label : "";
  program->desc.label = nullptr;
  program->desc.name = shader.name.empty() ? "effect" : shader.name;

  uint32_t effectSlots = 0;
  program->parameters.reserve(shader.parameters.size());
  for (const EffectParameterDesc& declaration : shader.parameters) {
    if (declaration.name.empty()) {
      diagnostic = "a parameter declaration has an empty name";
      return nullptr;
    }
    if (program->find(declaration.name) != nullptr) {
      diagnostic = "the parameter '" + declaration.name + "' is declared more than once";
      return nullptr;
    }
    const uint32_t slotCount = static_cast<uint32_t>(effectValueSlotCount(declaration.type));
    if (effectSlots + slotCount > kEffectSlotCount) {
      diagnostic = "the parameters need " + std::to_string(effectSlots + slotCount) + " effect slots but only " +
                   std::to_string(kEffectSlotCount) + " are available";
      return nullptr;
    }
    ParameterSlot slot;
    slot.name = declaration.name;
    slot.type = declaration.type;
    slot.index = effectSlots;
    slot.slotCount = slotCount;
    effectSlots += slotCount;
    program->parameters.push_back(std::move(slot));
  }
  program->effectSlots = effectSlots;

  // A Body shader cannot declare uniforms itself, so each bound texture is declared by the framework and
  // referred to by this name from inside the body. A name that is not an identifier would produce source
  // that does not parse; refusing here keeps the failure at the setter rather than at a compiler message
  // about generated code the caller never wrote.
  program->textureNames = textureNames;
  for (const std::string& name : program->textureNames) {
    if (!isIdentifier(name)) {
      diagnostic = "the texture name '" + name + "' is not a GLSL identifier";
      return nullptr;
    }
  }

  program->hash = hashProgram(shader, program->parameters, program->textureNames);
  return program;
}

bool writeParameter(ParameterBlock& block, const ParameterSlot& parameter, const EffectValue& value,
                    std::string& diagnostic) {
  if (value.type() != parameter.type) {
    diagnostic = "the parameter '" + parameter.name + "' is declared as " + valueTypeName(parameter.type) +
                 " but was given a " + valueTypeName(value.type());
    return false;
  }
  const uint32_t count = EffectValueAccess::count(value);
  const uint32_t expected = parameter.slotCount * 4;
  if (count > expected || count == 0) {
    diagnostic = "the parameter '" + parameter.name + "' carries " + std::to_string(count) + " component(s), which " +
                 valueTypeName(parameter.type) + " cannot hold";
    return false;
  }

  // The block's first two slots are the framework's sizes; effect slot `i` is block slot `i + 2`.
  float* first = block.slot(parameter.index + kFrameworkSlotCount);
  if (first == nullptr) {
    diagnostic = "the parameter '" + parameter.name + "' addresses a slot outside the parameter block";
    return false;
  }
  std::memset(first, 0, sizeof(float) * expected);
  std::memcpy(first, EffectValueAccess::data(value), sizeof(float) * count);
  return true;
}

} // namespace coral::detail
