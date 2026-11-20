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
 * Why does a clip-space quad written by the framework land the opposite way up from the one the canvas
 * renderer draws?
 *
 * This is the question the effect framework's orientation contract leaves open, and it matters
 * beyond curiosity: the framework's orientation contract was established by *measurement*
 * (`modules/graphics/tools/probes/effect_spike.cpp`), and a contract whose mechanism is unknown cannot be trusted to hold on another
 * backend -- Vulkan and Metal translate their own way, and a convention that is accidentally right on one
 * of them is exactly the kind of thing that turns into a mirrored transition nobody can explain.
 *
 * Both vertex stages are translated with `flip_vert_y = true` (`GlDevice.cpp:1298` for the SPIR-V path,
 * `:1368` for the GLSL one, both feeding `crossCompileToGLSL`), so both *should* end up with y negated.
 * The pictures say one of them does not. The generated GLSL settles it, and this program prints it:
 *
 *   1. the framework's own full-screen vertex stage, compiled from GLSL at run time (the `Language::GLSL`
 *      path: `compileGlslToSpirv` with OpenGL semantics, then `crossCompileToGLSL`);
 *   2. the canvas renderer's `quad.vert`, taken from the embedded SPIR-V the build produced (the
 *      `Language::SPIRV` path: `crossCompileToGLSL` alone).
 *
 * The lines that matter are the ones assigning `gl_Position`. A reference implementation that negates y is
 * a reference that does not; a pair that disagrees is the answer.
 *
 * It needs `backend/src` and the generated `CanvasShaders.h` on its include path, which is why it lives in
 * `tools/probes/` rather than in `tests/`: the translation entry points are the backend's internals,
 * deliberately not part of the installed headers.
 */

#include "GlslCompiler.h"
#include "ShaderCross.h"

#include <backend/RHI.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if __has_include(<CanvasShaders.h>)
namespace graphics {
namespace gpu {
namespace shaders {
[[nodiscard]] inline bool compare(const char* a, const char* b) noexcept;
} // namespace shaders
} // namespace gpu
} // namespace graphics
#include <CanvasShaders.h>
#define EFFECT_ORIENT_PROBE_HAS_CANVAS_SHADERS 1
#else
#define EFFECT_ORIENT_PROBE_HAS_CANVAS_SHADERS 0
#endif

namespace {

/**
 * The framework's full-screen vertex stage, copied from the effect framework's own pass source.
 *
 * Copied rather than linked to on purpose: this probe asks what the *translator* does with a source of that
 * shape, and a probe that linked the framework would be asking a different question -- whether the
 * framework's current source is translated the way it expects, which the effect suite's rendering cases
 * already answer.
 */
constexpr const char* kFrameworkVertexSource = R"(#version 300 es
precision highp float;
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;
layout(location = 0) out vec2 vUV;
void main() {
  vUV = aTexCoord;
  gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";

/** Prints every line of @p source that touches `gl_Position`, with the line number. */
void printPositionLines(const char* label, const std::string& source) {
  std::printf("  %s -- lines assigning gl_Position:\n", label);
  size_t lineNumber = 1;
  size_t start = 0;
  bool found = false;
  while (start <= source.size()) {
    const size_t end = source.find('\n', start);
    const std::string line = source.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (line.find("gl_Position") != std::string::npos) {
      std::printf("    %4zu: %s\n", lineNumber, line.c_str());
      found = true;
    }
    if (end == std::string::npos) break;
    start = end + 1;
    ++lineNumber;
  }
  if (!found) {
    std::printf("    (none: no assignment to gl_Position reached the driver)\n");
  }
}

/** Prints the `#version` line and the vertex inputs, which is where a path difference would also show. */
void printHeader(const char* label, const std::string& source) {
  std::printf("  %s -- %zu byte(s), first line: ", label, source.size());
  const size_t end = source.find('\n');
  std::printf("%s\n", source.substr(0, end == std::string::npos ? source.size() : end).c_str());
}

/** Runs the GLSL -> SPIR-V -> GLSL path the framework's shaders take at run time. */
bool translateFromGlsl(const char* label, const backend::GlDialect dialect) {
  const backend::GlslCompileResult compiled = backend::compileGlslToSpirv(kFrameworkVertexSource,
    backend::ShaderStage::Vertex, dialect, backend::ShaderSourceLanguage::Detect, "main", false, label);
  if (!compiled.success()) {
    std::printf("  %s: the GLSL did not compile to SPIR-V: %s\n", label, compiled.error.c_str());
    return false;
  }
  std::printf("  %s: GLSL %u compiled as %u%s\n", label, compiled.sourceInfo.version,
    compiled.compiledVersion, compiled.sourceInfo.versionWasRetargeted ? " (retargeted)" : "");

  backend::ShaderCrossCompileInfo info = backend::ShaderCrossCompileInfo::forDialect(dialect,
    backend::ShaderStage::Vertex);
  info.spirv = compiled.spirv.data();
  info.wordCount = compiled.spirv.size();
  // The three flags `GLDevice::createShaderModule` sets, verbatim: a probe that chose its own would be
  // answering a question about the probe.
  info.flipVertexY = true;
  info.fixupClipSpace = true;
  info.combineImageSamplers = true;

  const backend::ShaderCrossCompileResult translated = backend::crossCompileToGLSL(info);
  if (!translated.success) {
    std::printf("  %s: the SPIR-V did not translate: %s\n", label, translated.error.c_str());
    return false;
  }
  printHeader(label, translated.source);
  printPositionLines(label, translated.source);
  return true;
}

} // namespace anonymous

namespace {

/**
 * A source that needs the qualifiers GLSL 410 does not have, at a version that does.
 *
 * This is the experiment behind the fix for the progress note's 4.5 and 4.6: the runtime GLSL path compiles
 * with OpenGL semantics at a version retargeted for the platform's dialect, and on a desktop context that
 * floor is 410 -- where a uniform block may not carry a `binding` and may not omit one either, and a sampler
 * may not carry one at all. If 420 accepts both, then the floor is what stands between a runtime effect and
 * the same declarations the canvas shaders use, and raising it is a one-line fix rather than a redesign.
 *
 * `set = N` is deliberately absent: it is Vulkan's spelling and the OpenGL environment rejects it at every
 * version, which is why the framework's own convention is `layout(binding = N)` with the set implied.
 */
constexpr const char* kQualifiedSource = R"(#version %s
precision highp float;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uInput;
layout(binding = 1) uniform ProbeParams {
  vec4 uColour;
} params;
void main() {
  oColor = texture(uInput, vUV) * params.uColour;
}
)";

/**
 * Can a run-time source declare a push constant at all? (the progress note's 4.2)
 *
 * `push_constant` is Vulkan's spelling, so whether the OpenGL environment accepts it decides what the
 * backend's push-constant *emulation* can ever be used for: if the front end refuses the declaration, then
 * the emulation is reachable only by a module whose SPIR-V came from a Vulkan-environment compiler, and the
 * fix to the draw path (which now fills the block instead of waiting for a descriptor set to name it) is
 * about that case rather than about run-time GLSL.
 */
constexpr const char* kPushConstantSource = R"(#version %s
precision highp float;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(push_constant) uniform Params { vec4 uColour; } params;
void main() {
  oColor = params.uColour + vec4(vUV, 0.0, 0.0);
}
)";

void tryPushConstant(const char* versionLine, const backend::GlDialect dialect) {
  char source[1024];
  std::snprintf(source, sizeof(source), kPushConstantSource, versionLine);
  const backend::GlslCompileResult compiled = backend::compileGlslToSpirv(source,
    backend::ShaderStage::Fragment, dialect, backend::ShaderSourceLanguage::Detect, "main", false, versionLine);
  if (compiled.success()) {
    std::printf("  %-8s -> accepted (%zu word(s))\n", versionLine, compiled.spirv.size());
    return;
  }
  const std::string error = compiled.error.empty() ? "no message" : compiled.error;
  std::printf("  %-8s -> REJECTED: %s\n", versionLine, error.substr(0, error.find('\n')).c_str());
}

/** Compiles @p source for @p dialect and reports whether it produced SPIR-V and whether it reflected. */
void tryQualified(const char* versionLine, const backend::GlDialect dialect) {  char source[2048];
  std::snprintf(source, sizeof(source), kQualifiedSource, versionLine);

  const backend::GlslCompileResult compiled = backend::compileGlslToSpirv(source,
    backend::ShaderStage::Fragment, dialect, backend::ShaderSourceLanguage::Detect, "main", false, versionLine);
  std::printf("  %-14s -> ", versionLine);
  if (!compiled.success()) {
    std::printf("compile FAILED: %s\n", compiled.error.c_str());
    return;
  }
  std::printf("compiled as %u%s; ",
    compiled.compiledVersion, compiled.sourceInfo.versionWasRetargeted ? " (retargeted)" : "");

  backend::ShaderCrossCompileInfo info = backend::ShaderCrossCompileInfo::forDialect(dialect,
    backend::ShaderStage::Fragment);
  info.spirv = compiled.spirv.data();
  info.wordCount = compiled.spirv.size();
  info.flipVertexY = true;
  info.fixupClipSpace = true;
  info.combineImageSamplers = true;
  const backend::ShaderCrossCompileResult translated = backend::crossCompileToGLSL(info);
  if (!translated.success) {
    std::printf("translate FAILED: %s\n", translated.error.c_str());
    return;
  }

  std::printf("reflection:");
  for (const backend::ShaderBindingInfo& binding: translated.reflection.bindings) {
    if (binding.type == backend::ShaderBindingType::StageInput ||
        binding.type == backend::ShaderBindingType::StageOutput) {
      continue;
    }
    std::printf(" (set=%u binding=%u type=%d name=%s)", binding.set, binding.binding,
      static_cast<int>(binding.type), binding.name.c_str());
  }
  std::printf("\n");

  // The whole generated source, because the question "does it carry the 420pack directive" is answered by
  // reading it, and the driver's error only says the qualifier is illegal without it.
  std::printf("    ---- generated source (%zu byte(s)) ----\n", translated.source.size());
  size_t lineNumber = 1;
  size_t start = 0;
  while (start <= translated.source.size()) {
    const size_t end = translated.source.find('\n', start);
    const std::string line = translated.source.substr(start,
      end == std::string::npos ? std::string::npos : end - start);
    std::printf("    %2zu| %s\n", lineNumber, line.c_str());
    if (end == std::string::npos) break;
    start = end + 1;
    ++lineNumber;
  }
  std::printf("    --------------------------------------\n");
}

} // namespace anonymous

int main() {
  std::printf("Which vertex paths have their y flipped?\n\n");

  // The platform's own dialect is the one a device on this machine comes up in, so the desktop answer is
  // the one that explains what was measured.
  const backend::GlDialect dialect = backend::getPlatformGlDialect();
  std::printf("platform dialect: %s\n", backend::toString(dialect));

  std::printf("\n1. the framework's full-screen stage, from GLSL at run time:\n");
  translateFromGlsl("effect/fullscreen.vert", dialect);

#if EFFECT_ORIENT_PROBE_HAS_CANVAS_SHADERS
  std::printf("\n2. the canvas renderer's quad.vert, from the embedded SPIR-V:\n");
  const graphics::gpu::shaders::Blob quad = graphics::gpu::shaders::find("quad.vert");
  if (quad.words == nullptr || quad.wordCount == 0) {
    std::printf("  quad.vert is not in this build's embedded shaders\n");
  } else {
    backend::ShaderCrossCompileInfo info = backend::ShaderCrossCompileInfo::forDialect(dialect,
      backend::ShaderStage::Vertex);
    info.spirv = quad.words;
    info.wordCount = quad.wordCount;
    info.flipVertexY = true;
    info.fixupClipSpace = true;
    info.combineImageSamplers = true;
    const backend::ShaderCrossCompileResult translated = backend::crossCompileToGLSL(info);
    if (!translated.success) {
      std::printf("  quad.vert did not translate: %s\n", translated.error.c_str());
    } else {
      printHeader("canvas/quad.vert", translated.source);
      printPositionLines("canvas/quad.vert", translated.source);
      std::printf("\n  the canvas stage's own mapping, for reference: it writes\n"
                  "    device * uScale + uOffset  with uOffset = (-1, -1)\n"
                  "  so device row 0 becomes clip y = -1 before the flip.\n");
    }
  }
#else
  std::printf("\n2. the embedded canvas shaders are not available to this build, so quad.vert cannot be\n"
              "   translated here. Reconfigure with -DCORAL_GRAPHICS_BUILD_DEV_TOOLS=ON and a shader\n"
              "   compiler so the canvas shaders are embedded, then rebuild.\n");
#endif

  std::printf("\nRead the two lists against each other: a negation in one and not the other is the answer.\n");

  // ------------------------------------------------------------------------------------------------
  // Can runtime GLSL declare a uniform block at all? (The progress note's 4.5 and 4.6)
  // ------------------------------------------------------------------------------------------------
  std::printf("\n3. a uniform block and a bound sampler, at the versions that might allow them:\n");
  tryQualified("410", dialect);
  tryQualified("420", dialect);
  tryQualified("450", dialect);
  tryQualified("310 es", backend::GlDialect::ES);
  tryQualified("320 es", backend::GlDialect::ES);

  std::printf("\n4. can a run-time source declare a push constant?\n");
  tryPushConstant("420", dialect);
  tryPushConstant("450", dialect);
  tryPushConstant("310 es", backend::GlDialect::ES);
  std::printf("  (the platform's dialect is %s, so the lines above that name another dialect are\n"
              "   what the *other* kind of context would accept -- the point is to see which spellings\n"
              "   are legal at all, and the retarget floor the backend imposes decides which one a\n"
              "   run-time shader actually gets.)\n", backend::toString(dialect));
  return 0;
}
