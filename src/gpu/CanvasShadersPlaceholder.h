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

/*
 * PLACEHOLDER - the build replaces this file.
 *
 * `CMakeLists.txt` compiles the GLSL under `shaders/canvas/` to SPIR-V with
 * `tools/shader_compiler` and then runs `tools/embed_shaders.ps1` over the results, writing
 * the generated header *to this path*. This file exists only so that the GPU renderer's translation unit
 * has something to include when the shader toolchain is unavailable -- a configure with
 * `-DCORAL_BACKEND_SUPPORTS_SHADERC=OFF`, or a build that could not find `shader_compiler`.
 *
 * That case is not an error and must not become one: a build without shaderc cannot compile GLSL to SPIR-V
 * at all, so a GPU path that needs shaders has nothing to run. Reporting an invalid blob for every name is
 * how the renderer finds out, and it reports `Result::Unsupported` rather than drawing nothing.
 *
 * **If this file is still the placeholder in a build directory where `shaders` compiled, the generation step
 * did not run** -- check the configure log for the message that says so.
 */

#ifndef CORAL_GPU_CANVAS_SHADERS_PLACEHOLDER_H
#define CORAL_GPU_CANVAS_SHADERS_PLACEHOLDER_H

#include <cstddef>
#include <cstdint>

namespace graphics::gpu::shaders {

/** Which stage a blob is for, mirroring `backend::ShaderStage` without including the backend. */
enum class Stage : uint8_t { Vertex, Fragment, Compute };

/** One compiled shader: a pointer into static storage and a word count, not a byte count. */
struct Blob {
  const uint32_t* words = nullptr;
  size_t wordCount = 0;
  Stage stage = Stage::Vertex;
  const char* name = nullptr;

  [[nodiscard]] bool isValid() const noexcept { return words != nullptr && wordCount > 0; }
};

/**
 * @return the blob a shader was embedded under, or an invalid one when the name is unknown.
 *
 * Always invalid here: this is the placeholder, and it knows no shaders.
 */
[[nodiscard]] inline Blob find(const char* name) noexcept {
  (void) name;
  return Blob {};
}

/** @return how many shaders are embedded, which a diagnostic reports. */
[[nodiscard]] inline size_t count() noexcept { return 0; }

} // namespace graphics::gpu::shaders

#endif // CORAL_GPU_CANVAS_SHADERS_PLACEHOLDER_H
