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

#ifndef CORAL_BACKEND_METAL_CROSS_H
#define CORAL_BACKEND_METAL_CROSS_H

/**
 * @file MetalCross.h
 *
 * SPIR-V -> MSL translation, wrapping SPIRV-Cross's Metal backend.
 *
 * This is the Metal twin of `ShaderCross.h`: both consume the RHI's canonical SPIR-V and both are private to
 * `CoralGraphics`, but they differ in one structural way that shapes this interface. A GLSL shader names its
 * bindings through the source itself, while MSL has no descriptor sets at all -- `[[buffer(n)]]`, `[[texture(n)]]` and
 * `[[sampler(n)]]` are three flat tables. The `(set, binding)` pairs therefore have to be *assigned* slots before the
 * translation, which is why `MslCompileInfo` carries a binding table instead of just a dialect.
 *
 * The table comes from the pipeline layout at pipeline creation time, not from the shader module: the same module can
 * legally be linked with two different layouts, and the MSL text is only valid for the slots it was generated against.
 */

#include <backend/Types.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace backend::metal {

/** One `(set, binding)` -> MSL argument slot assignment. */
struct MslResourceBinding {
  ShaderStage stage = ShaderStage::Vertex;
  uint32_t set = 0;
  uint32_t binding = 0;
  /** Array length, used by SPIRV-Cross for arrayed bindings and for diagnostics. */
  uint32_t count = 1;

  /** `[[buffer(n)]]` slot for a uniform/storage buffer. Ignored for other kinds. */
  uint32_t mslBuffer = 0;
  /** `[[texture(n)]]` slot for a sampled/storage image. Ignored for other kinds. */
  uint32_t mslTexture = 0;
  /** `[[sampler(n)]]` slot. A combined image sampler needs both this and `mslTexture`. */
  uint32_t mslSampler = 0;
};

struct MslCompileInfo {
  /** SPIR-V words, which must outlive the call. */
  const uint32_t* spirv = nullptr;
  size_t wordCount = 0;

  ShaderStage stage = ShaderStage::Vertex;
  /** The module's entry point; "main" is the usual case and is renamed to `main0` in the generated MSL. */
  const char* entryPoint = "main";

  /** `(set, binding)` -> slot assignments, built from the pipeline layout by the caller. */
  const MslResourceBinding* bindings = nullptr;
  size_t bindingCount = 0;

  /** True when the module declares a push-constant block; it is then bound to the pinned push slot. */
  bool hasPushConstants = false;

  /** The Metal platform to generate for. It changes feature gating inside SPIRV-Cross, not just the version line. */
  bool ios = false;

  /** MSL language version, major.minor. 2.2 is the floor every supported macOS/iOS version implements. */
  uint32_t mslVersionMajor = 2;
  uint32_t mslVersionMinor = 2;
};

struct MslCompileResult {
  bool success = false;
  /** The generated MSL source. Kept on the result for diagnostics even when compilation fails. */
  std::string source;
  /** The entry-point *function* name to pass to `newFunctionWithName:` (SPIRV-Cross renames `main`). */
  std::string entryPoint;
  /**
   * The `Location` decorations of the module's stage inputs, when the stage is a vertex stage.
   *
   * A pipeline's `MTLVertexDescriptor` has to describe every attribute the vertex function reads, and Metal *refuses*
   * the pipeline when one is missing rather than returning zeros. The caller compares this list with the descriptor's
   * attributes and supplies the difference, which is what makes a shared vertex shader usable by a variant that
   * declares fewer streams -- the canvas's case.
   */
  std::vector<uint32_t> attributeLocations;

  /** Human readable diagnostic from SPIRV-Cross. Empty on success. */
  std::string error;

  explicit operator bool() const noexcept { return success; }
};

/**
 * Translates @p info's module to MSL.
 *
 * The vertex fixup is deliberately asymmetric with the GLSL translation: `flip_vert_y` is on and
 * `fixup_clipspace` is **off**. The RHI's SPIR-V is authored in Vulkan's Y-down, [0,1] clip space. Metal's NDC is Y-up
 * with a [0,1] depth range, so only the Y axis needs the flip; turning on the clip-space fixup as well (which is what
 * the GLSL path does, to widen GL's [-1,1] depth) would move every canvas z value to 0.5 for no reason.
 */
[[nodiscard]] MslCompileResult crossCompileToMsl(const MslCompileInfo& info);

/** @return true when this build has SPIRV-Cross's MSL backend available. */
[[nodiscard]] bool isMslCrossAvailable() noexcept;

} // namespace backend::metal

#endif // CORAL_BACKEND_METAL_CROSS_H
