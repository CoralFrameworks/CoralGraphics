/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and limitations under
 * the License.
 */

#ifndef CORAL_BACKEND_GLSL_COMPILER_H
#define CORAL_BACKEND_GLSL_COMPILER_H

#include <backend/Descriptors.h>
#include <backend/Types.h>

#include <cstdint>
#include <string>
#include <vector>

namespace backend {

/**
 * The result of compiling GLSL source to SPIR-V.
 */
struct GlslCompileResult {
  /** The emitted SPIR-V words. Empty on failure. */
  std::vector<uint32_t> spirv;

  /** What the caller's source was, as detected from it rather than as requested. */
  ShaderSourceInfo sourceInfo;

  /**
   * The `#version` the module was actually compiled at.
   *
   * Not necessarily the author's. glslang refuses to emit SPIR-V below GLSL ES 3.10 or desktop GLSL
   * 3.30, so an ESSL 3.00 source is retargeted upward and compiled at 310. Reporting both numbers is
   * what lets a caller see that its source was retargeted instead of having to infer it.
   */
  uint32_t compiledVersion = 0;

  /** True when @c compiledVersion differs from `sourceInfo.version`. */
  bool versionWasRetargeted = false;

  /** The compiler's diagnostics, empty on success. Never empty on failure. */
  std::string error;

  [[nodiscard]] bool success() const noexcept { return error.empty() && !spirv.empty(); }
};

/**
 * Whether GLSL source compilation is available in this build.
 *
 * False when Coral was configured with `-DCORAL_BACKEND_SUPPORTS_SHADERC=OFF`, or when the vendored
 * shaderc was not found. `compileGlslToSpirv` still exists and reports `Result::Unsupported`, so a
 * caller can write one code path and check this only when it wants a better message.
 */
[[nodiscard]] bool isGlslCompilerAvailable() noexcept;

/**
 * @return the compiler's version string, or "unavailable". Suitable for diagnostics and for folding
 *         into a `ShaderCacheKey::compilerHash`: a compiler upgrade can change the SPIR-V for
 *         identical input, so a cache that ignores the version can serve output the current compiler
 *         would not have produced.
 */
[[nodiscard]] const char* getGlslCompilerVersion() noexcept;

/**
 * @return the lowest `#version` this compiler can emit SPIR-V from for @p language.
 *
 * Exposed because it is a hard constraint a caller can hit and cannot work around: glslang refuses to
 * generate SPIR-V from anything below GLSL ES 3.10 or desktop GLSL 3.30, so ESSL 1.00 and ESSL 3.00
 * source is retargeted upward before compilation. A caller that wants to know whether its source will
 * survive that can ask here rather than by watching a compilation fail.
 */
[[nodiscard]] uint32_t getMinimumSpirvSourceVersion(ShaderSourceLanguage language) noexcept;

/**
 * @return the lowest `#version` that can be handed to a @p target context for @p language.
 *
 * The retarget aims here rather than at the source's own profile. An undeclared source is ESSL 1.00 by
 * the specification, but retargeting it to `310 es` on a desktop context produces SPIR-V for a language
 * that context cannot consume - and provokes a glslang internal error on the way, because the ES
 * profile and a desktop target env do not mix. Aiming at the target keeps one dialect in play from
 * parse to link.
 */
[[nodiscard]] uint32_t getRetargetVersion(ShaderSourceLanguage language, GlDialect target) noexcept;

/**
 * Parses the `#version` directive out of @p source.
 *
 * A source with no directive is GLSL ES 1.00 by the specification's own rule, and that is what this
 * reports: `version` 100, `versionWasDeclared` false. Getting this wrong in either direction is how a
 * shader ends up compiled as a language it was not written in, so the parse is deliberately strict -
 * only a directive in the first non-comment, non-blank line counts.
 *
 * @param source the GLSL text.
 * @param outInfo receives the language, version and whether the directive was present.
 */
void parseGlslVersion(const std::string& source, ShaderSourceInfo* outInfo);

/**
 * Compiles GLSL @p source to SPIR-V for @p stage.
 *
 * ## Known limitation: a source that declares no version
 *
 * A source with no `#version` is ESSL 1.00 by the specification's rule, and the retarget then has to
 * give it a desktop version on a desktop context. Measured behaviour of the vendored glslang on that
 * path: a body whose vertex inputs use `layout(location = ...)`, or that declares `precision`, fails
 * with an internal parse error rather than a useful diagnostic -
 *
 *     0(1) : error C0000: syntax error, unexpected '(', expecting "::" at token "("
 *
 * The `location` qualifier is required for SPIR-V output, so this combination is not one a caller can
 * simply avoid by writing a different body. **Declare a `#version` in a desktop shader.** A source that
 * says `#version 410 core` compiles cleanly, and the retarget path is then only reached by the ES
 * profiles, which it handles correctly - ESSL 1.00 and ESSL 3.00 both compile, retargeted to 310.
 *
 * This is recorded rather than worked around because the workaround would be a source-rewriting pass,
 * and because a caller who knows the constraint can satisfy it in one line.
 *
 * The source is compiled as the language it declares, retargeted up to the minimum the compiler can
 * emit SPIR-V from. `GlslCompileResult::sourceInfo` reports what the source actually was, so a caller
 * is never misled about which language it wrote.
 *
 * @param source       GLSL text. A leading BOM is tolerated.
 * @param stage        which stage to compile for.
 * @param target       which dialect the SPIR-V is destined for. This is what the version retarget aims
 *                     at, and it is not the same question as what language the source is written in: a
 *                     shader written in ES 3.00 style and compiled for a desktop context has to be
 *                     retargeted to a desktop version, or the context it is handed to cannot use it.
 * @param language     what to compile it as. `Detect` reads the source's own `#version`.
 * @param entryPoint   entry point name; `"main"` when null or empty.
 * @param optimize     ask for spirv-opt. Off by default, because the point of this path is that a
 *                     debugger can map the SPIR-V back to the source.
 * @param label        debug name, used in diagnostics.
 * @return the SPIR-V plus what the source was, or a diagnostic.
 */
GlslCompileResult compileGlslToSpirv(const std::string& source, ShaderStage stage, GlDialect target,
        ShaderSourceLanguage language = ShaderSourceLanguage::Detect, const char* entryPoint = "main",
        bool optimize = false, const char* label = nullptr);

/**
 * Compiles GLSL @p source to SPIR-V for the **Vulkan** environment.
 *
 * This is not `compileGlslToSpirv` with a different dialect. The two target environments disagree
 * about the language itself:
 *
 *  - `shaderc_target_env_opengl` accepts `layout(set = N, binding = M)` syntactically but emits no
 *    `DescriptorSet`/`Binding` decorations, because OpenGL has no descriptor sets. A module compiled
 *    that way is *unusable* on Vulkan: every resource lands at (0, 0), which is why the canvas shader
 *    set had to be compiled a second time for this backend.
 *  - `shaderc_target_env_vulkan` emits those decorations, and requires their language: GLSL ES 3.10 or
 *    desktop 4.50. A `#version 300 es` source - which is what every shader in `shaders/` declares - is
 *    therefore retargeted upward to 310 es before it is compiled, exactly as the OpenGL path retargets
 *    it for its own environment.
 *
 * The retarget is the only rewriting done. The caller's source is otherwise passed through, so the
 * driver's diagnostics keep naming its lines.
 */
GlslCompileResult compileGlslToSpirvVulkan(const std::string& source, ShaderStage stage,
        ShaderSourceLanguage language = ShaderSourceLanguage::Detect, const char* entryPoint = "main",
        bool optimize = false, const char* label = nullptr);

} // namespace backend

#endif // CORAL_BACKEND_GLSL_COMPILER_H
