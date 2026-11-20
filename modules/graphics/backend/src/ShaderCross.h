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

#ifndef CORAL_BACKEND_PRIVATE_SHADER_CROSS_H
#define CORAL_BACKEND_PRIVATE_SHADER_CROSS_H

/**
 * @file ShaderCross.h
 *
 * SPIR-V -> GLSL ES cross compilation, wrapping SPIRV-Cross (third_party/SPIRV-Cross).
 *
 * This header is private to modules/graphics: it is the mechanism that lets the OpenGL backend consume
 * the same SPIR-V module the Vulkan backend would take unchanged. Upper layers never see it.
 *
 * ## Why SPIR-V is the canonical input
 *
 * If each backend accepted its own shader source, portability would stop at the shader boundary and
 * every backend would grow its own compile pipeline. By fixing SPIR-V as the RHI's shader IR:
 *
 *  - Vulkan hands the module straight to vkCreateShaderModule.
 *  - OpenGL ES runs this translator to GLSL ES 3.00 and compiles the result with glCompileShader.
 *  - Metal will run SPIRV-Cross to MSL.
 *
 * Authoring is then done with whatever front end the project prefers (glslang, DXC, slang), and a
 * new backend never requires re-authoring shaders.
 *
 * ## Reflection
 *
 * Descriptor bindings are recovered from the SPIR-V itself, which is what removes uniform names from
 * upper layer code. A `layout(set = 0, binding = 2) uniform sampler2D albedo;` in the shader becomes
 * a `ShaderBindingInfo { set = 0, binding = 2, type = TextureSampler, name = "albedo" }`, and the
 * OpenGL backend resolves `name` to a `glUniformLocation` against the linked program. Callers
 * write descriptors by (set, binding) and never type a uniform name.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <backend/Descriptors.h>
#include <backend/Types.h>

namespace backend {

/** What a reflected shader resource turns into at the RHI level. */
enum class ShaderBindingType : uint8_t {
  TextureSampler,
  Sampler,
  SampledTexture,
  UniformBuffer,
  StorageBuffer,
  ReadOnlyStorageBuffer,
  PushConstant,
  StageInput,
  StageOutput,
};

[[nodiscard]] const char* toString(ShaderBindingType type) noexcept;

/** One resource discovered in the SPIR-V module. */
struct ShaderBindingInfo {
  ShaderBindingType type = ShaderBindingType::TextureSampler;

  /** Descriptor set index, from the SPIR-V `set` decoration. */
  uint32_t set = 0;
  /** Binding number, from the SPIR-V `binding` decoration. */
  uint32_t binding = 0;
  /** Number of array elements, 1 for a non-arrayed binding. */
  uint32_t arraySize = 1;

  /** Base name of the resource. For combined samplers this is the synthesized OpenGL name. */
  std::string name;

  /**
   * For TextureSampler bindings, the names of the image and sampler the combined name was built
   * from. Empty for the other binding types. Kept so that diagnostics can explain a surprising
   * combined name.
   */
  std::string imageName;
  std::string samplerName;

  /** Byte size of a uniform block, when the backend reported one. 0 when unknown. */
  uint32_t blockSize = 0;

  /**
   * The GL-side slot the caller asked this binding to occupy, or `kNoShaderLocation`.
   *
   * SPIR-V never carries this: a `Location` decoration is a *stage interface* location, not a
   * descriptor's, and a descriptor's place in a set is the `binding` decoration. So the field is only
   * ever set from a caller-supplied declaration (`ShaderBindingDeclaration::location`), which is how a
   * GLSL-direct module states where a sampler's texture unit or a block's uniform-block binding has to
   * land. Left at `kNoShaderLocation`, the OpenGL backend assigns both itself.
   */
  int32_t location = kNoShaderLocation;
};

/** The complete binding table of a compiled module. */
struct ShaderReflection {
  std::vector<ShaderBindingInfo> bindings;

  /** @return the binding at @p set / @p binding, or nullptr when the module does not declare it. */
  [[nodiscard]] const ShaderBindingInfo* find(uint32_t set, uint32_t binding) const noexcept;

  /** @return every binding of @p type. */
  [[nodiscard]] std::vector<const ShaderBindingInfo*> ofType(ShaderBindingType type) const;

  [[nodiscard]] uint32_t countOfType(ShaderBindingType type) const noexcept;
};

/** Inputs of a cross-compilation. */
struct ShaderCrossCompileInfo {
  /** SPIR-V words. Both the bytecode and its word count are required. */
  const uint32_t* spirv = nullptr;
  size_t wordCount = 0;

  /** Stage, needed for the vertex/fragment specific fixups. */
  ShaderStage stage = ShaderStage::Vertex;

  /**
   * Which GLSL flavour to emit.
   *
   * The two dialects need genuinely different output, not just a different version number. Desktop
   * GLSL 4.10 has no `precision` statements and needs `#version 410 core`; GLSL ES 3.00 requires a
   * default float precision and needs `#version 300 es`. Emitting the wrong one is a compile error at
   * best and silently different precision behaviour at worst, which is why the target is explicit
   * rather than inferred from a version number.
   */
  GlDialect glDialect = GlDialect::ES;

  /**
   * Target GLSL version.
   *
   * 300 for OpenGL ES 3.0+, 410 for desktop OpenGL 4.1 core. The backend passes the version that
   * matches its context, so a 4.5 context can ask for 450 and get newer syntax.
   */
  uint32_t glslVersion = 300;

  /**
   * Rewrite `gl_Position.y` so that a Vulkan-style top-left origin NDC still renders upright under
   * OpenGL's bottom-left origin. Enabled by default; disable only when the upper layer already
   * compensates in its projection matrix.
   */
  bool flipVertexY = true;

  /**
   * Remap the Vulkan depth range [0, 1] to OpenGL's [-1, 1]. Enabled by default and required for
   * depth to behave identically across backends.
   */
  bool fixupClipSpace = true;

  /**
   * Merge separate image/sampler pairs into combined `sampler2D` uniforms.
   *
   * OpenGL ES 3.0 has no separate sampler objects in its core specification, so this is required
   * there. Desktop OpenGL 4.1 has them, but the RHI's binding model treats a combined image+sampler as
   * the common case on both, so the conversion stays on by default.
   */
  bool combineImageSamplers = true;

  /** Convenience: fills in the dialect-appropriate version and target. */
  static ShaderCrossCompileInfo forDialect(const GlDialect dialect, const ShaderStage stage) {
    ShaderCrossCompileInfo info;
    info.glDialect = dialect;
    info.stage = stage;
    info.glslVersion = dialect == GlDialect::ES ? 300u : 410u;
    return info;
  }
};

/** Outputs of a cross-compilation. */
struct ShaderCrossCompileResult {
  bool success = false;
  /** Generated GLSL source for the requested dialect. May contain partial output on failure. */
  std::string source;
  /** Reflection data for the module. Populated even when compilation fails, when possible. */
  ShaderReflection reflection;
  /** Human readable diagnostic from SPIRV-Cross. Empty on success. */
  std::string error;
  /** The dialect the source was generated for, echoed back for diagnostics. */
  GlDialect dialect = GlDialect::ES;

  explicit operator bool() const noexcept { return success; }
};

/**
 * Translates SPIR-V into GLSL and reflects its resource bindings.
 *
 * @param info the SPIR-V module and target configuration.
 * @return the translated source plus reflection data, or a diagnostic on failure.
 */
[[nodiscard]] ShaderCrossCompileResult crossCompileToGLSL(const ShaderCrossCompileInfo& info);

/**
 * Reads a module's resource bindings without translating it.
 *
 * The Vulkan backend hands the SPIR-V to the driver unchanged, so it has no translated source to
 * reflect alongside - and it must not run the combined-sampler pass, because a Vulkan module's
 * separate images and samplers are exactly what its descriptor set layout declares. This entry point
 * is the reflection half of `crossCompileToGLSL` on its own.
 *
 * @param spirv        the module's words.
 * @param wordCount    number of words.
 * @param outPushConstantSize receives the declared size of the module's push-constant block, or 0.
 * @return the binding table; empty when the module declares no resources or SPIRV-Cross is absent.
 */
[[nodiscard]] ShaderReflection reflectSpirvModule(const uint32_t* spirv, size_t wordCount,
        uint32_t* outPushConstantSize = nullptr);

/** @return true when this build was compiled with SPIRV-Cross support available. */
[[nodiscard]] bool isShaderCrossAvailable() noexcept;

/** @return the SPIRV-Cross version string, or "unavailable". */
[[nodiscard]] const char* getShaderCrossVersion() noexcept;

} // namespace backend

#endif // CORAL_BACKEND_PRIVATE_SHADER_CROSS_H
