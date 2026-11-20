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
 * @file shader_compiler.cpp
 *
 * GLSL source to SPIR-V, on the build machine.
 *
 * ## Why this exists
 *
 * The RHI's canonical shader input is SPIR-V: Vulkan consumes it verbatim, and the OpenGL backend
 * translates it - to GLSL ES 3.00 on mobile and to GLSL 4.10 core on desktop - through SPIRV-Cross.
 * That makes SPIR-V the right thing to *ship*, and a poor thing to write. This tool is the other half
 * of that decision: shaders are authored in `shaders/`, compiled here, and only the `.spv` reaches a
 * device.
 *
 * ## Why it is not a second compiler
 *
 * It calls `backend::compileGlslToSpirv`, which is the same function the RHI calls when a shader
 * arrives as source at run time. **That is the point.** A tool that re-implemented the settings would
 * drift from the runtime, and the drift would be invisible: the SPIR-V would be valid, the shader
 * would render, and the two paths would disagree about the target environment or the SPIR-V version
 * only in the cases nobody tests. Sharing the function makes the settings identical by construction
 * rather than by discipline.
 *
 * The one difference is *when* the work happens, and that is the whole reason to prefer this path:
 * compiling here means the device does not carry glslang and SPIRV-Tools, which together are an order
 * of magnitude larger than SPIRV-Cross.
 *
 * ## What it does not do
 *
 * No `#include`. The runtime's compiler has no includer, so supporting one here would make a shader
 * that this tool accepts and the RHI rejects - a difference that would surface as "it works in the
 * build and fails on device", which is exactly the class of problem sharing the function avoids. If
 * includes are wanted later, they belong in `GlslCompiler.cpp` so that both paths gain them at once.
 */

#include "GlslCompiler.h"
#include "ShaderCross.h"

#include <backend/Types.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

using backend::GlDialect;
using backend::GlslCompileResult;
using backend::ShaderSourceLanguage;
using backend::ShaderStage;

/** What the command line asked for. */
struct Options {
  /** Explicit inputs, compiled one file each. */
  std::vector<fs::path> inputs;

  /** A directory to compile every shader under, paired with @c outputDir. */
  std::optional<fs::path> sourceDir;
  std::optional<fs::path> outputDir;

  /** Output for a single input. Empty means "beside the input, with a `.spv` extension". */
  std::optional<fs::path> output;

  /** Empty means "from the file extension". */
  std::optional<ShaderStage> stage;
  std::optional<ShaderSourceLanguage> language;
  std::optional<GlDialect> target;

  /**
   * True to compile for the Vulkan environment rather than the OpenGL one.
   *
   * The two are not interchangeable: the OpenGL environment accepts `layout(set = N, binding = M)` but
   * emits no descriptor decorations for it, so a module compiled that way cannot back a Vulkan
   * pipeline. The canvas shader set is compiled twice for exactly this reason.
   */
  bool vulkanEnvironment = false;

  std::string entryPoint = "main";
  std::string label;
  bool optimize = false;
  bool list = false;
  bool verify = false;
  bool showHelp = false;
  bool showVersion = false;
};

void printUsage(FILE* out) {
  std::fprintf(out,
          "shader_compiler - compile GLSL shaders to the SPIR-V the RHI consumes\n"
          "\n"
          "Usage:\n"
          "  shader_compiler [options] <shader>...\n"
          "  shader_compiler [options] --source-dir <dir> --output-dir <dir>\n"
          "\n"
          "Options:\n"
          "  -o, --output <file>     output path for a single input\n"
          "      --source-dir <dir>  compile every shader found under <dir>\n"
          "      --output-dir <dir>  where a --source-dir run writes; the tree is mirrored\n"
          "      --stage <stage>     vertex|fragment|compute|geometry|tess-control|tess-evaluation\n"
          "                          (default: from the file extension)\n"
          "      --entry <name>      entry point (default: main)\n"
          "      --language <lang>   detect|essl100|essl300|glsl410 (default: detect, which reads\n"
          "                          the shader's own `#version`)\n"
          "      --target <dialect>  es|desktop (default: es)\n"
          "      --environment <env> opengl|vulkan (default: opengl)\n"
          "                          The Vulkan environment emits DescriptorSet/Binding decorations, which\n"
          "                          is what a VkDescriptorSetLayout needs; the OpenGL one does not, and its\n"
          "                          module is consumed through SPIRV-Cross instead.\n"
          "      --label <name>      debug name used in diagnostics (default: the file name)\n"
          "      --optimize          run spirv-opt; off by default so the SPIR-V still maps to the\n"
          "                          source, which is what makes a shader debuggable\n"
          "      --verify            translate the result with SPIRV-Cross exactly as the OpenGL\n"
          "                          backend would, and report the emitted GLSL version and the\n"
          "                          reflected bindings; fails when the module does not translate\n"
          "      --list              print what would be compiled and stop\n"
          "  -v, --version           print the compiler's version and stop\n"
          "  -h, --help              this text\n"
          "\n"
          "Recognised extensions: .vert .frag .comp .geom .tesc .tese (a .glsl needs --stage).\n"
          "\n"
          "The compile settings are not options here on purpose: they come from\n"
          "`backend::compileGlslToSpirv`, the same function the RHI calls at run time, so the two cannot\n"
          "disagree about the target environment or the SPIR-V version.\n");
}

/** @return the stage a shader with @p extension is, or nothing when the extension says nothing. */
std::optional<ShaderStage> stageForExtension(const std::string& extension) {
  if (extension == ".vert") return ShaderStage::Vertex;
  if (extension == ".frag") return ShaderStage::Fragment;
  if (extension == ".comp") return ShaderStage::Compute;
  if (extension == ".geom") return ShaderStage::Geometry;
  if (extension == ".tesc") return ShaderStage::TessellationControl;
  if (extension == ".tese") return ShaderStage::TessellationEvaluation;
  return std::nullopt;
}

/** @return true when @p path has an extension this tool treats as a shader. */
bool isShaderFile(const fs::path& path) {
  return stageForExtension(path.extension().string()).has_value();
}

std::optional<ShaderStage> stageForName(const std::string& name) {
  if (name == "vertex") return ShaderStage::Vertex;
  if (name == "fragment") return ShaderStage::Fragment;
  if (name == "compute") return ShaderStage::Compute;
  if (name == "geometry") return ShaderStage::Geometry;
  if (name == "tess-control") return ShaderStage::TessellationControl;
  if (name == "tess-evaluation") return ShaderStage::TessellationEvaluation;
  return std::nullopt;
}

std::optional<ShaderSourceLanguage> languageForName(const std::string& name) {
  if (name == "detect") return ShaderSourceLanguage::Detect;
  if (name == "essl100") return ShaderSourceLanguage::ESSL100;
  if (name == "essl300") return ShaderSourceLanguage::ESSL300;
  if (name == "glsl410") return ShaderSourceLanguage::GLSL410;
  return std::nullopt;
}

std::optional<GlDialect> dialectForName(const std::string& name) {
  if (name == "es") return GlDialect::ES;
  if (name == "desktop") return GlDialect::Desktop;
  return std::nullopt;
}

/** @return the argument after @p index, or nothing when the option was given without one. */
std::optional<std::string> valueAfter(const std::vector<std::string>& arguments, size_t& index) {
  if (index + 1 >= arguments.size()) return std::nullopt;
  ++index;
  return arguments[index];
}

/** @return the parsed options, or nothing when an argument is unknown or malformed. */
std::optional<Options> parseArguments(const std::vector<std::string>& arguments) {
  Options options;

  for (size_t i = 0; i < arguments.size(); ++i) {
    const std::string& argument = arguments[i];

    if (argument == "-h" || argument == "--help") {
      options.showHelp = true;
      continue;
    }
    if (argument == "-v" || argument == "--version") {
      options.showVersion = true;
      continue;
    }
    if (argument == "--list") {
      options.list = true;
      continue;
    }
    if (argument == "--verify") {
      options.verify = true;
      continue;
    }
    if (argument == "--optimize") {
      options.optimize = true;
      continue;
    }

    if (argument == "-o" || argument == "--output" || argument == "--source-dir" || argument == "--output-dir" ||
        argument == "--stage" || argument == "--entry" || argument == "--language" || argument == "--target" ||
        argument == "--label" || argument == "--environment") {
      const auto value = valueAfter(arguments, i);
      if (!value) {
        std::fprintf(stderr, "error: %s needs a value\n", argument.c_str());
        return std::nullopt;
      }

      if (argument == "-o" || argument == "--output") {
        options.output = *value;
      } else if (argument == "--source-dir") {
        options.sourceDir = *value;
      } else if (argument == "--output-dir") {
        options.outputDir = *value;
      } else if (argument == "--entry") {
        options.entryPoint = *value;
      } else if (argument == "--label") {
        options.label = *value;
      } else if (argument == "--stage") {
        options.stage = stageForName(*value);
        if (!options.stage) {
          std::fprintf(stderr, "error: unknown stage '%s'\n", value->c_str());
          return std::nullopt;
        }
      } else if (argument == "--language") {
        options.language = languageForName(*value);
        if (!options.language) {
          std::fprintf(stderr, "error: unknown language '%s'\n", value->c_str());
          return std::nullopt;
        }
      } else if (argument == "--target") {
        options.target = dialectForName(*value);
        if (!options.target) {
          std::fprintf(stderr, "error: unknown target '%s'\n", value->c_str());
          return std::nullopt;
        }
      } else if (argument == "--environment") {
        if (*value == "vulkan") {
          options.vulkanEnvironment = true;
        } else if (*value == "opengl") {
          options.vulkanEnvironment = false;
        } else {
          std::fprintf(stderr, "error: unknown environment '%s'\n", value->c_str());
          return std::nullopt;
        }
      }
      continue;
    }

    if (!argument.empty() && argument[0] == '-') {
      std::fprintf(stderr, "error: unknown option '%s'\n", argument.c_str());
      return std::nullopt;
    }

    options.inputs.emplace_back(argument);
  }

  return options;
}

std::optional<std::string> readFile(const fs::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.good()) return std::nullopt;

  const auto size = file.tellg();
  std::string contents(static_cast<size_t>(size), '\0');
  file.seekg(0, std::ios::beg);
  file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  return contents;
}

/**
 * Writes SPIR-V @p words to @p path.
 *
 * The words are written little-endian rather than as the host's `uint32_t` array. SPIR-V is defined as
 * a stream of little-endian words, and writing the host's bytes directly would silently produce a
 * big-endian file on a big-endian machine - a portability bug that no machine Coral runs on would
 * reveal, which is the worst kind to leave in.
 */
bool writeSpirv(const fs::path& path, const std::vector<uint32_t>& words) {
  std::error_code error;
  if (path.has_parent_path()) fs::create_directories(path.parent_path(), error);

  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file.good()) return false;

  for (const uint32_t word : words) {
    const uint8_t bytes[4] = {static_cast<uint8_t>(word & 0xFFu), static_cast<uint8_t>((word >> 8) & 0xFFu),
                              static_cast<uint8_t>((word >> 16) & 0xFFu), static_cast<uint8_t>((word >> 24) & 0xFFu)};
    file.write(reinterpret_cast<const char*>(bytes), 4);
  }
  return file.good();
}

/**
 * @return @p input with `.spv` appended, the name used when no output path was given.
 *
 * Appended rather than substituted, so `present.vert` becomes `present.vert.spv`. Replacing the
 * extension makes `present.vert` and `present.frag` both produce `present.spv`, and the collision
 * surfaces as one of them silently winning rather than as anything that names the mistake.
 */
fs::path defaultOutputFor(const fs::path& input) {
  return fs::path(input.string() + ".spv");
}

/**
 * Runs compiled SPIR-V through the same SPIRV-Cross pass the RHI runs, and reports what came out.
 *
 * Compiling to SPIR-V and *being loadable* are two different questions, and only the second one matters
 * to a caller. A module can be valid SPIR-V and still fail to translate - an unsupported capability, a
 * stage SPIRV-Cross has no path for - and the whole point of compiling ahead of time is to find that
 * here rather than on a device.
 *
 * The options mirror `GlDevice`'s, so what is verified is what a device would do rather than a
 * neighbouring configuration that happens to share a name.
 *
 * @return false when the module does not translate.
 */
bool verifyTranslatable(const std::vector<uint32_t>& spirv, const ShaderStage stage, const GlDialect dialect,
        const std::string& label) {
  if (!backend::isShaderCrossAvailable()) {
    // Not a failure of the shader: this build simply cannot answer the question. Said out loud rather
    // than passed over, because a `--verify` that silently verifies nothing is worse than no flag.
    std::fprintf(stderr,
            "  warning: %s: this build has no SPIRV-Cross, so the module was not verified. "
            "Rebuild with -DCORAL_BACKEND_SUPPORTS_SPIRV_CROSS=ON.\n",
            label.c_str());
    return true;
  }

  backend::ShaderCrossCompileInfo info = backend::ShaderCrossCompileInfo::forDialect(dialect, stage);
  info.spirv = spirv.data();
  info.wordCount = spirv.size();
  info.flipVertexY = true;
  info.fixupClipSpace = true;
  info.combineImageSamplers = true;

  const backend::ShaderCrossCompileResult result = backend::crossCompileToGLSL(info);
  if (!result.success) {
    std::fprintf(stderr, "error: %s compiles to SPIR-V but does not translate for %s: %s\n", label.c_str(),
            backend::toString(dialect), result.error.c_str());
    return false;
  }

  // The emitted `#version` line is the contract with the driver, and for the ES dialect it is the whole
  // question this tool exists to answer: that the artifact is GLSL ES 3.00 or newer.
  const std::string& source = result.source;
  const size_t end = source.find('\n');
  const std::string versionLine = source.substr(0, end == std::string::npos ? source.size() : end);

  std::printf("    verified: \"%s\", %zu binding(s)\n", versionLine.c_str(), result.reflection.bindings.size());
  for (const backend::ShaderBindingInfo& binding : result.reflection.bindings) {
    std::printf("      set %u binding %u  %-18s %s\n", binding.set, binding.binding, backend::toString(binding.type),
            binding.name.c_str());
  }
  return true;
}

/**
 * Compiles one shader.
 *
 * @return true on success. Diagnostics go to stderr; the retarget notice goes to stdout because it is
 *         not a problem, only something a reader of the build log should know happened.
 */
bool compileOne(const fs::path& input, const fs::path& output, const Options& options) {
  const auto source = readFile(input);
  if (!source) {
    std::fprintf(stderr, "error: cannot read %s\n", input.string().c_str());
    return false;
  }

  auto stage = options.stage;
  if (!stage) stage = stageForExtension(input.extension().string());
  if (!stage) {
    std::fprintf(stderr, "error: %s has no stage; pass --stage\n", input.string().c_str());
    return false;
  }

  const std::string label = options.label.empty() ? input.filename().string() : options.label;

  const GlslCompileResult result = options.vulkanEnvironment
          ? backend::compileGlslToSpirvVulkan(*source, *stage,
                  options.language.value_or(ShaderSourceLanguage::Detect), options.entryPoint.c_str(),
                  options.optimize, label.c_str())
          : backend::compileGlslToSpirv(*source, *stage,
                  options.target.value_or(GlDialect::ES), options.language.value_or(ShaderSourceLanguage::Detect),
                  options.entryPoint.c_str(), options.optimize, label.c_str());

  if (!result.success()) {
    std::fprintf(stderr, "error: %s: %s\n", input.string().c_str(), result.error.c_str());
    return false;
  }

  // Reported rather than left to be inferred, for the same reason `Shader::wasRetargeted()` exists: a
  // source written for one version and compiled as another is worth seeing in a build log, because it
  // is the difference between "the shader compiled" and "the shader compiled as something else".
  if (result.versionWasRetargeted) {
    std::printf("  note: %s declares %u and was compiled as %u (%s)\n", label.c_str(), result.sourceInfo.version,
            result.compiledVersion, backend::toString(result.sourceInfo.language));
  }

  if (!writeSpirv(output, result.spirv)) {
    std::fprintf(stderr, "error: cannot write %s\n", output.string().c_str());
    return false;
  }

  std::printf("  %-40s -> %-40s %6zu bytes\n", input.string().c_str(), output.string().c_str(),
          result.spirv.size() * sizeof(uint32_t));

  if (options.verify) {
    if (options.vulkanEnvironment) {
      // A Vulkan-environment module is consumed by the driver as it is, not translated to GLSL; the
      // useful check is that its decorations came out, so the reflection is printed instead.
      const backend::ShaderReflection reflection =
              backend::reflectSpirvModule(result.spirv.data(), result.spirv.size());
      std::printf("    reflected: %zu binding(s)\n", reflection.bindings.size());
      for (const backend::ShaderBindingInfo& binding: reflection.bindings) {
        std::printf("      set %u binding %u  %-18s %s\n", binding.set, binding.binding,
                backend::toString(binding.type), binding.name.c_str());
      }
      return true;
    }
    return verifyTranslatable(result.spirv, *stage, options.target.value_or(GlDialect::ES), label);
  }
  return true;
}

/** @return every shader under @p directory, in a stable order so a build is reproducible. */
std::vector<fs::path> collectShaders(const fs::path& directory) {
  std::vector<fs::path> shaders;

  std::error_code error;
  for (const auto& entry : fs::recursive_directory_iterator(directory, error)) {
    if (!entry.is_regular_file()) continue;
    if (isShaderFile(entry.path())) shaders.push_back(entry.path());
  }

  // `recursive_directory_iterator` gives no order, and a build whose output order changes between runs
  // is a build whose logs cannot be compared.
  std::sort(shaders.begin(), shaders.end());
  return shaders;
}

int run(const Options& options) {
  if (options.showHelp) {
    printUsage(stdout);
    return 0;
  }
  if (options.showVersion) {
    std::printf("shader_compiler\n  compiler : %s\n  available: %s\n", backend::getGlslCompilerVersion(),
            backend::isGlslCompilerAvailable() ? "yes" : "no");
    return backend::isGlslCompilerAvailable() ? 0 : 1;
  }

  if (!backend::isGlslCompilerAvailable()) {
    std::fprintf(stderr,
            "error: this build has no GLSL compiler. Coral was configured with "
            "-DCORAL_BACKEND_SUPPORTS_SHADERC=OFF, or the vendored shaderc was not found.\n");
    return 1;
  }

  // The two modes are exclusive, and saying so is better than defining a precedence nobody remembers.
  if (options.sourceDir && !options.inputs.empty()) {
    std::fprintf(stderr, "error: --source-dir and explicit inputs are different modes; pick one\n");
    return 1;
  }
  if (options.sourceDir && !options.outputDir) {
    std::fprintf(stderr, "error: --source-dir needs --output-dir\n");
    return 1;
  }
  if (!options.sourceDir && options.inputs.empty()) {
    printUsage(stderr);
    return 1;
  }
  if (options.inputs.size() > 1 && options.output) {
    std::fprintf(stderr, "error: -o names one file, so it cannot be used with several inputs\n");
    return 1;
  }

  std::vector<std::pair<fs::path, fs::path>> work;

  if (options.sourceDir) {
    if (!fs::is_directory(*options.sourceDir)) {
      std::fprintf(stderr, "error: %s is not a directory\n", options.sourceDir->string().c_str());
      return 1;
    }
    for (const fs::path& shader : collectShaders(*options.sourceDir)) {
      // The tree is mirrored rather than flattened, so two shaders with the same file name in different
      // subdirectories stay distinct, and the stage stays in the name - `present.vert.spv`. This has to
      // agree with what `shaders/CMakeLists.txt` declares as the command's OUTPUT, or the build would
      // recompile a file it then cannot find.
      const fs::path relative = fs::relative(shader, *options.sourceDir);
      work.emplace_back(shader, *options.outputDir / fs::path(relative.string() + ".spv"));
    }
  } else {
    for (const fs::path& input : options.inputs) {
      work.emplace_back(input, options.output.value_or(defaultOutputFor(input)));
    }
  }

  if (options.list) {
    for (const auto& [input, output] : work) std::printf("  %s -> %s\n", input.string().c_str(), output.string().c_str());
    std::printf("  %zu shader(s)\n", work.size());
    return 0;
  }

  if (work.empty()) {
    std::printf("  no shaders found\n");
    return 0;
  }

  int failures = 0;
  for (const auto& [input, output] : work) {
    if (!compileOne(input, output, options)) ++failures;
  }

  if (failures > 0) {
    std::fprintf(stderr, "error: %d of %zu shader(s) failed to compile\n", failures, work.size());
    return 1;
  }
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<size_t>(argc > 0 ? argc - 1 : 0));
  for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);

  const auto options = parseArguments(arguments);
  if (!options) {
    std::fprintf(stderr, "\n");
    printUsage(stderr);
    return 1;
  }

  return run(*options);
}
