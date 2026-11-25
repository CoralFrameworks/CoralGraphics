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
 * @file canvas_glsl_generator.cpp
 *
 * Generates the committed GLSL fallback used by the OpenGL backend when SPIRV-Cross is not part of the
 * build (`CORAL_BACKEND_SUPPORTS_SPIRV_CROSS=OFF`).
 *
 * ## Why this exists
 *
 * `shaders/canvas/*` is Vulkan-dialect GLSL: every resource is `layout(set = 0, binding = N)`. A GL
 * driver cannot compile that, and the normal build solves it by shipping SPIR-V and translating it with
 * SPIRV-Cross at module creation. A build without SPIRV-Cross has no translator, so the backend must be
 * handed GLSL the driver *can* compile - which is what this tool produces.
 *
 * It uses the same two steps the runtime uses, so the fallback is what the translator would have
 * produced:
 *
 *   1. `compileGlslToSpirv` for the OpenGL environment - the same call the canvas shader build makes;
 *   2. `crossCompileToGLSL` for the ES and desktop dialects, including `flip_vert_y`, clip-space fixup
 *      and combined-sampler handling.
 *
 * The output is committed as `src/gpu/CanvasShadersGLSL.h`, because a build that has no shader compiler
 * cannot generate it. Editing a canvas shader therefore means regenerating it:
 *
 *   cmake --build out/graphics_check --config Release --target canvas_glsl_generator
 *   out/graphics_check/bin/Release/canvas_glsl_generator.exe \
 *       --source-dir shaders/canvas --output src/gpu/CanvasShadersGLSL.h
 *
 * The header carries the same source manifest as `CanvasShadersEmbedded.h`, so a stale copy is a test
 * failure rather than a picture that differs on the machines that fall back to it.
 */

#include "GlslCompiler.h"
#include "ShaderCross.h"

#include <backend/Descriptors.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <map>
#include <regex>
#include <vector>

namespace fs = std::filesystem;
using namespace backend;

namespace {

std::string readFile(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream.good()) return {};
  return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

/** @return the identifier used for a shader's C++ namespace: `quad.frag` -> `quad_frag`. */
std::string identifierFor(const std::string& name) {
  std::string result;
  result.reserve(name.size());
  for (const char character: name) {
    result += (std::isalnum(static_cast<unsigned char>(character)) != 0)
                      ? static_cast<char>(std::tolower(static_cast<unsigned char>(character)))
                      : '_';
  }
  return result;
}

std::optional<ShaderStage> stageForExtension(const std::string& extension) {
  if (extension == ".vert") return ShaderStage::Vertex;
  if (extension == ".frag") return ShaderStage::Fragment;
  if (extension == ".comp") return ShaderStage::Compute;
  return std::nullopt;
}

const char* bindingTypeName(const ShaderBindingType type) {
  switch (type) {
    case ShaderBindingType::TextureSampler:
      return "backend::BindingType::TextureSampler";
    case ShaderBindingType::Sampler:
      return "backend::BindingType::Sampler";
    case ShaderBindingType::SampledTexture:
      return "backend::BindingType::Texture";
    case ShaderBindingType::UniformBuffer:
      return "backend::BindingType::UniformBuffer";
    case ShaderBindingType::StorageBuffer:
      return "backend::BindingType::StorageBuffer";
    case ShaderBindingType::ReadOnlyStorageBuffer:
      return "backend::BindingType::ReadOnlyStorageBuffer";
    case ShaderBindingType::PushConstant:
    case ShaderBindingType::StageInput:
    case ShaderBindingType::StageOutput:
      break;
  }
  return "backend::BindingType::TextureSampler";
}

bool isDescriptorBinding(const ShaderBindingType type) {
  return type == ShaderBindingType::TextureSampler || type == ShaderBindingType::Sampler ||
         type == ShaderBindingType::SampledTexture || type == ShaderBindingType::UniformBuffer ||
         type == ShaderBindingType::StorageBuffer || type == ShaderBindingType::ReadOnlyStorageBuffer;
}

std::string stageName(const ShaderStage stage) {
  switch (stage) {
    case ShaderStage::Vertex:
      return "backend::ShaderStage::Vertex";
    case ShaderStage::Fragment:
      return "backend::ShaderStage::Fragment";
    case ShaderStage::Compute:
      return "backend::ShaderStage::Compute";
    default:
      return "backend::ShaderStage::Vertex";
  }
}

/**
 * @return the source manifest in the same recipe the embed script and the runtime test use:
 *         `name:size:checksum;` per source, sorted by name.
 */
std::string sourceManifest(const fs::path& directory) {
  std::vector<fs::path> files;
  for (const auto& entry: fs::directory_iterator(directory)) {
    const auto extension = entry.path().extension().string();
    if (extension == ".vert" || extension == ".frag" || extension == ".comp") files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end(), [](const fs::path& left, const fs::path& right) {
    return left.filename().string() < right.filename().string();
  });

  std::string manifest;
  for (const fs::path& file: files) {
    const std::string bytes = readFile(file);
    uint64_t checksum = 0;
    for (size_t index = 0; index < bytes.size(); ++index) {
      checksum = (checksum + static_cast<uint64_t>(static_cast<uint8_t>(bytes[index])) *
                                     static_cast<uint64_t>(index + 1)) %
                 4294967296ULL;
    }
    if (!manifest.empty()) manifest += ';';
    manifest += file.filename().string() + ':' + std::to_string(bytes.size()) + ':' +
                std::to_string(checksum);
  }
  return manifest;
}

/** @return true when @p text begins with @p versionLine. */
bool hasVersionLine(const std::string& text, const char* versionLine) {
  return text.rfind(versionLine, 0) == 0;
}

std::string trim(const std::string& text) {
  const size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const size_t last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

/** @return true when @p token is a `binding = N` or `set = N` layout qualifier. */
bool isBindingQualifier(const std::string& token) {
  const auto names = [&token](const char* name) {
    const size_t length = std::strlen(name);
    if (token.size() < length || token.compare(0, length, name) != 0) return false;
    if (token.size() == length) return true;
    const char next = token[length];
    return next == ' ' || next == '\t' || next == '=';
  };
  return names("binding") || names("set");
}

/** @return true when any `layout(...)` in @p source still names `binding` or `set`. */
bool hasBindingQualifier(const std::string& source) {
  size_t cursor = 0;
  while ((cursor = source.find("layout(", cursor)) != std::string::npos) {
    const size_t open = cursor + 6;
    const size_t close = source.find(')', open);
    if (close == std::string::npos) return false;
    const std::string list = source.substr(open, close - open);
    size_t start = 0;
    while (start <= list.size()) {
      const size_t comma = list.find(',', start);
      const std::string token =
              trim(list.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
      if (!token.empty() && isBindingQualifier(token)) return true;
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    cursor = close + 1;
  }
  return false;
}

/**
 * Removes `binding`/`set` layout qualifiers from @p source.
 *
 * The desktop translation carries them, because SPIRV-Cross emits the binding a GL SL object was
 * compiled with and enables `GL_ARB_shading_language_420pack` for it. A `GLSLDirect` module must not:
 * the OpenGL backend assigns texture units and uniform-block bindings itself from the declaration table
 * (`glUniform1i` / `glUniformBlockBinding`), and a source-level binding would override that assignment
 * and point a sampler at the wrong unit. If a layout list becomes empty - `layout(binding = 0) uniform
 * sampler2D` - the whole `layout(...)` prefix is dropped, because `layout()` is not legal GLSL.
 */
std::string stripBindingQualifiers(const std::string& source) {
  std::string result;
  result.reserve(source.size());

  size_t cursor = 0;
  while (cursor < source.size()) {
    const size_t layoutAt = source.find("layout", cursor);
    if (layoutAt == std::string::npos) {
      result.append(source, cursor, std::string::npos);
      break;
    }
    result.append(source, cursor, layoutAt - cursor);

    const size_t open = source.find('(', layoutAt);
    if (open == std::string::npos) {
      result.append(source, layoutAt, std::string::npos);
      break;
    }
    const size_t close = source.find(')', open);
    if (close == std::string::npos) {
      result.append(source, layoutAt, std::string::npos);
      break;
    }

    const std::string list = source.substr(open + 1, close - open - 1);
    std::string kept;
    size_t start = 0;
    while (start <= list.size()) {
      const size_t comma = list.find(',', start);
      const std::string token = trim(list.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
      if (!token.empty() && !isBindingQualifier(token)) {
        if (!kept.empty()) kept += ", ";
        kept += token;
      }
      if (comma == std::string::npos) break;
      start = comma + 1;
    }

    if (!kept.empty()) {
      result += "layout(";
      result += kept;
      result += ')';
    }
    cursor = close + 1;
  }
  return result;
}

} // namespace anonymous

int main(int argc, char** argv) {
  fs::path sourceDirectory = "shaders/canvas";
  fs::path output = "CanvasShadersGLSL.h";
  fs::path dumpDirectory;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index] != nullptr ? argv[index] : "";
    if (argument == "--source-dir" && index + 1 < argc) {
      sourceDirectory = argv[++index];
    } else if (argument == "--output" && index + 1 < argc) {
      output = argv[++index];
    } else if (argument == "--dump-dir" && index + 1 < argc) {
      dumpDirectory = argv[++index];
    } else if (argument == "--help" || argument == "-h") {
      std::printf("canvas_glsl_generator [--source-dir <dir>] [--output <header>] [--dump-dir <dir>]\n");
      return 0;
    }
  }

  if (!fs::is_directory(sourceDirectory)) {
    std::fprintf(stderr, "error: %s is not a directory\n", sourceDirectory.string().c_str());
    return 1;
  }

  std::vector<fs::path> files;
  for (const auto& entry: fs::directory_iterator(sourceDirectory)) {
    const auto extension = entry.path().extension().string();
    if (extension == ".vert" || extension == ".frag" || extension == ".comp") files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end(), [](const fs::path& left, const fs::path& right) {
    return left.filename().string() < right.filename().string();
  });
  if (files.empty()) {
    std::fprintf(stderr, "error: no shader sources under %s\n", sourceDirectory.string().c_str());
    return 1;
  }

  std::string header;
  header += "/*\n";
  header += " * Copyright (C) 2026 The Coral Project\n";
  header += " *\n";
  header += " * Licensed under the Apache License, Version 2.0 (the \"License\");\n";
  header += " * you may not use this file except in compliance with the License.\n";
  header += " * You may obtain a copy of the License at\n";
  header += " *\n";
  header += " *      http://www.apache.org/licenses/LICENSE-2.0\n";
  header += " *\n";
  header += " * Unless required by applicable law or agreed to in writing, software\n";
  header += " * distributed under the License is distributed on an \"AS IS\" BASIS,\n";
  header += " * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.\n";
  header += " * See the License for the specific language governing permissions and\n";
  header += " * limitations under the License.\n";
  header += " */\n\n";
  header += "/*\n";
  header += " * GENERATED FILE - DO NOT EDIT.\n";
  header += " *\n";
  header += " * Produced by tools/canvas_glsl_generator from the GLSL under shaders/canvas/.\n";
  header += " *\n";
  header += " * This is the OpenGL backend's canvas shader set for builds without SPIRV-Cross: the\n";
  header += " * sources are the same shaders compiled to SPIR-V for the OpenGL environment and then\n";
  header += " * translated to each driver dialect, exactly as the runtime translator would have done.\n";
  header += " * Each entry carries the binding names the GL backend resolves with\n";
  header += " * `glGetUniformLocation`/`glGetUniformBlockIndex`, because a direct module has no\n";
  header += " * reflection to read them from.\n";
  header += " */\n\n";
  header += "#ifndef CORAL_GPU_CANVAS_SHADERS_GLSL_H\n";
  header += "#define CORAL_GPU_CANVAS_SHADERS_GLSL_H\n\n";
  header += "#include <backend/Descriptors.h>\n\n";
  header += "#include <cstddef>\n";
  header += "#include <cstdint>\n\n";

  const std::string manifest = sourceManifest(sourceDirectory);
  header += "/** The same source stamp as the committed SPIR-V copy; a stale fallback fails a test. */\n";
  header += "#ifndef CORAL_CANVAS_GLSL_SHADERS_SOURCE_MANIFEST\n";
  header += "#define CORAL_CANVAS_GLSL_SHADERS_SOURCE_MANIFEST \"" + manifest + "\"\n";
  header += "#endif\n\n";

  header += "namespace graphics {\nnamespace gpu {\nnamespace shaders {\nnamespace glsl {\n\n";
  header += "/** One shader, in the two dialects the OpenGL backend can come up in. */\n";
  header += "struct Source {\n";
  header += "  backend::ShaderStage stage = backend::ShaderStage::Vertex;\n";
  header += "  const char* name = nullptr;\n";
  header += "  const char* es = nullptr;\n";
  header += "  const char* desktop = nullptr;\n";
  header += "  const backend::ShaderBindingDeclaration* bindings = nullptr;\n";
  header += "  size_t bindingCount = 0;\n";
  header += "\n";
  header += "  [[nodiscard]] bool isValid() const noexcept { return es != nullptr && desktop != nullptr; }\n";
  header += "};\n\n";

  size_t emitted = 0;
  for (const fs::path& file: files) {
    const std::string name = file.filename().string();
    const auto stage = stageForExtension(file.extension().string());
    if (!stage) {
      std::fprintf(stderr, "error: %s has no stage\n", name.c_str());
      return 1;
    }

    const std::string source = readFile(file);
    if (source.empty()) {
      std::fprintf(stderr, "error: cannot read %s\n", file.string().c_str());
      return 1;
    }

    // The glslang ES profile rejects a `set = N` qualifier (Vulkan-only), while the canvas sources
    // are Vulkan-dialect GLSL. The set is irrelevant to the direct-GLSL output because the backend
    // resolves every sampler by name, so drop it before compiling and keep the binding.
    std::string compileSource = source;
    for (int set = 0; set < 4; ++set) {
      const std::string prefix = "set = " + std::to_string(set) + ",";
      for (size_t at = compileSource.find(prefix); at != std::string::npos;
           at = compileSource.find(prefix)) {
        compileSource.erase(at, prefix.size());
      }
    }
    const GlslCompileResult compiled =
            compileGlslToSpirv(compileSource, *stage, GlDialect::ES, ShaderSourceLanguage::Detect, "main", false,
              name.c_str());
    if (!compiled.success()) {
      std::fprintf(stderr, "error: %s: %s\n", name.c_str(), compiled.error.c_str());
      return 1;
    }

    const auto translate = [&](const GlDialect dialect) {
      ShaderCrossCompileInfo info = ShaderCrossCompileInfo::forDialect(dialect, *stage);
      info.spirv = compiled.spirv.data();
      info.wordCount = compiled.spirv.size();
      info.flipVertexY = true;
      info.fixupClipSpace = true;
      info.combineImageSamplers = true;
      return crossCompileToGLSL(info);
    };

    const ShaderCrossCompileResult es = translate(GlDialect::ES);
    const ShaderCrossCompileResult desktop = translate(GlDialect::Desktop);
    if (!es.success || !desktop.success) {
      std::fprintf(stderr, "error: %s could not be translated: %s / %s\n", name.c_str(), es.error.c_str(),
              desktop.error.c_str());
      return 1;
    }

    // The desktop translation carries `layout(binding = N)` because SPIRV-Cross emits the explicit
    // binding and enables the 420pack extension for it. See `stripBindingQualifiers`.
    const std::string esSource = stripBindingQualifiers(es.source);
    const std::string desktopSource = stripBindingQualifiers(desktop.source);
    if (!dumpDirectory.empty()) {
      std::error_code error;
      fs::create_directories(dumpDirectory, error);
      std::ofstream(dumpDirectory / (name + ".es.glsl"), std::ios::binary | std::ios::trunc) << esSource;
      std::ofstream(dumpDirectory / (name + ".desktop.glsl"), std::ios::binary | std::ios::trunc) << desktopSource;
    }
    if (!hasVersionLine(esSource, "#version 300 es")) {
      std::fprintf(stderr, "error: %s: the ES source starts with '%s'\n", name.c_str(),
              esSource.substr(0, esSource.find('\n')).c_str());
      return 1;
    }
    if (!hasVersionLine(desktopSource, "#version 410")) {
      std::fprintf(stderr, "error: %s: the desktop source starts with '%s'\n", name.c_str(),
              desktopSource.substr(0, desktopSource.find('\n')).c_str());
      return 1;
    }

    // A direct module is compiled by the driver as written, so it must not carry `set`/`binding`
    // qualifiers: the GL backend assigns texture units and uniform-block bindings itself from the
    // declaration table below. This is a guard after stripping, not an expected remaining state.
    if (hasBindingQualifier(esSource) || hasBindingQualifier(desktopSource) ||
        esSource.find("layout()") != std::string::npos ||
        desktopSource.find("layout()") != std::string::npos) {
      std::fprintf(stderr,
              "error: %s: the translated source still carries set/binding qualifiers, which a direct GL "
              "module must not; the binding table would disagree with the driver\n",
              name.c_str());
      return 1;
    }
    if (esSource.find(")GLSL\"") != std::string::npos || desktopSource.find(")GLSL\"") != std::string::npos) {
      std::fprintf(stderr, "error: %s contains the raw-string delimiter; choose another one\n", name.c_str());
      return 1;
    }

    const std::string identifier = identifierFor(name);
    header += "// " + name + ": " + std::to_string(esSource.size()) + " ES bytes, " +
              std::to_string(desktopSource.size()) + " desktop bytes.\n";
    header += "namespace " + identifier + " {\n";

    // The translated GLSL has no descriptor sets, so SPIRV-Cross reports every sampler as set 0. The
    // source still carries the original `layout(set = N, binding = M)`; keep those numbers in the
    // declaration table so the GL backend can tell `uTexture` (set 0) from `uPattern` (set 1).
    std::map<std::string, std::pair<uint32_t, uint32_t>> declaredBindings;
    {
      static const std::regex pattern(
        R"(layout\s*\(\s*set\s*=\s*([0-9]+)\s*,\s*binding\s*=\s*([0-9]+)\s*\)\s*uniform\s+(?:sampler2D|sampler3D|samplerCube)\s+([A-Za-z_][A-Za-z0-9_]*))");
      for (std::sregex_iterator it(source.begin(), source.end(), pattern), end; it != end; ++it) {
        declaredBindings[(*it)[3].str()] = {
          static_cast<uint32_t>(std::stoul((*it)[1].str())),
          static_cast<uint32_t>(std::stoul((*it)[2].str())),
        };
      }
    }

    std::vector<const ShaderBindingInfo*> bindings;
    for (const ShaderBindingInfo& binding: es.reflection.bindings) {
      if (isDescriptorBinding(binding.type)) bindings.push_back(&binding);
    }
    if (bindings.empty()) {
      header += "inline constexpr const backend::ShaderBindingDeclaration* kBindings = nullptr;\n";
      header += "inline constexpr size_t kBindingCount = 0;\n";
    } else {
      header += "inline constexpr backend::ShaderBindingDeclaration kBindings[] = {\n";
      for (const ShaderBindingInfo* binding: bindings) {
        uint32_t set = binding->set;
        uint32_t number = binding->binding;
        const auto declared = declaredBindings.find(binding->name);
        if (declared != declaredBindings.end()) {
          set = declared->second.first;
          number = declared->second.second;
        }
        header += "  { " + std::to_string(set) + ", " + std::to_string(number) + ", " +
                  bindingTypeName(binding->type) + ", " + std::to_string(std::max(binding->arraySize, 1u)) + ", \"" +
                  binding->name + "\" },\n";
      }
      header += "};\n";
      header += "inline constexpr size_t kBindingCount = sizeof(kBindings) / sizeof(kBindings[0]);\n";
    }

    header += "inline constexpr const char* kEs = R\"GLSL(" + esSource + ")GLSL\";\n";
    header += "inline constexpr const char* kDesktop = R\"GLSL(" + desktopSource + ")GLSL\";\n";
    header += "inline constexpr Source kSource{ " + stageName(*stage) + ", \"" + name + "\", kEs, kDesktop, ";
    if (bindings.empty()) {
      header += "nullptr, 0 };\n";
    } else {
      header += "kBindings, kBindingCount };\n";
    }
    header += "} // namespace " + identifier + "\n\n";
    ++emitted;
  }

  header += "/** @return true when @p a and @p b are the same text, without <cstring>. */\n";
  header += "[[nodiscard]] inline bool compare(const char* a, const char* b) noexcept {\n";
  header += "  size_t index = 0;\n";
  header += "  while (a[index] != '\\0' && a[index] == b[index]) ++index;\n";
  header += "  return a[index] == b[index];\n";
  header += "}\n\n";
  header += "/** @return the source for @p name, or an invalid one when it is not embedded. */\n";
  header += "[[nodiscard]] inline Source find(const char* name) noexcept {\n";
  header += "  if (name == nullptr) return Source{};\n";
  for (const fs::path& file: files) {
    const std::string name = file.filename().string();
    header += "  if (compare(name, \"" + name + "\")) return " + identifierFor(name) + "::kSource;\n";
  }
  header += "  return Source{};\n";
  header += "}\n\n";
  header += "/** @return how many shaders are embedded, which a diagnostic reports. */\n";
  header += "[[nodiscard]] inline size_t count() noexcept { return " + std::to_string(emitted) + "; }\n\n";
  header += "} // namespace glsl\n} // namespace shaders\n} // namespace gpu\n} // namespace graphics\n\n";
  header += "#endif // CORAL_GPU_CANVAS_SHADERS_GLSL_H\n";

  if (output.has_parent_path()) {
    std::error_code error;
    fs::create_directories(output.parent_path(), error);
  }
  std::ofstream stream(output, std::ios::binary | std::ios::trunc);
  if (!stream.good()) {
    std::fprintf(stderr, "error: cannot write %s\n", output.string().c_str());
    return 1;
  }
  stream.write(header.data(), static_cast<std::streamsize>(header.size()));
  std::printf("wrote %s (%zu shader(s), %zu bytes)\n", output.string().c_str(), emitted, header.size());
  return 0;
}
