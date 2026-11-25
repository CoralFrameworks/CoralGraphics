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

#ifndef CORAL_EFFECT_SHADER_H
#define CORAL_EFFECT_SHADER_H

#include <effect/EffectTypes.h>

#include <cstdint>
#include <string>
#include <vector>

namespace coral {

/** One parameter an effect declares, in the order the shader reads it. */
struct EffectParameterDesc {
  std::string name;
  EffectValueType type = EffectValueType::Float;
};

/** How a `EffectShaderDesc`'s fragment stage is supplied. */
enum class EffectShaderAuthoring : uint8_t {
  /** `fragmentSource` is the body of `vec4 effectMain(vec2 uv)`; the framework composes the rest. */
  Body,
  /** `fragmentSpirv` is a pre-compiled fragment module. */
  SpirvModule,
};

/** Where a node's shader is in its compile/install lifecycle. */
enum class EffectShaderState : uint8_t {
  /** No shader has been set. */
  Empty,
  /** Submitted and waiting for the render thread to compile and install it. */
  Pending,
  /** The current shader is usable. */
  Ready,
  /** The most recent submission failed to compile; the previous shader is still in use. */
  Failed,
};

/**
 * Everything needed to compile one effect pass.
 *
 * The description is a value: `setShader` copies it into the node's snapshot, and the render thread
 * compiles from that copy. A caller may therefore reuse or destroy the description as soon as the call
 * returns.
 */
struct EffectShaderDesc {
  /** A name for diagnostics, the program cache and a GPU capture. */
  std::string name;

  /** The vertex stage, or empty for the framework's built-in full-screen quad. */
  std::string vertexSource;

  /**
   * A pre-compiled vertex module. When non-empty it replaces `vertexSource` for compilation; keep
   * `vertexSource` populated when the framework must derive the fragment interface from it.
   */
  std::vector<uint32_t> vertexSpirv;

  /** The body of `vec4 effectMain(vec2 uv)`, for `EffectShaderAuthoring::Body`. */
  std::string fragmentSource;

  /** The fragment module's SPIR-V words, for `EffectShaderAuthoring::SpirvModule`. */
  std::vector<uint32_t> fragmentSpirv;

  EffectShaderAuthoring authoring = EffectShaderAuthoring::Body;

  /**
   * The parameters the shader reads, in slot order.
   *
   * The first declaration occupies effect slot 0 (parameter block slot 2, the two slots before it are
   * the framework's source and target sizes), and a `Matrix4` consumes four slots.
   */
  std::vector<EffectParameterDesc> parameters;

  /** How many graph inputs the pass samples: `uInput0` .. `uInput<N-1>`. */
  uint32_t inputCount = 1;

  /**
   * Set when input 0 is an external `samplerExternalOES` texture (a SurfaceTexture or an AHB-backed
   * EGLImage). External inputs require direct GLSL and an OpenGL ES context; Vulkan reports an error.
   */
  bool externalInput = false;

  /** Debug name used in compiler diagnostics. May be null. */
  const char* label = nullptr;
};

} // namespace coral

#endif // CORAL_EFFECT_SHADER_H
