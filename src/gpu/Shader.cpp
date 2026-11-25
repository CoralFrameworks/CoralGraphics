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

#include <gpu/Shader.h>

#include <backend/Device.h>
#include <gpu/Context.h>

#include "ContextState.h"

#include <fstream>
#include <sstream>
#include <utility>

namespace graphics::gpu {

const char* toString(const ShaderLanguage language) noexcept { return backend::toString(language); }

bool isShaderCompilerAvailable() noexcept { return backend::isShaderCompilerAvailable(); }

const char* getShaderCompilerVersion() noexcept { return backend::getShaderCompilerVersion(); }

ShaderSource ShaderSource::vertex(std::string text, const char* label) {
  ShaderSource source;
  source.source = std::move(text);
  source.stage = backend::ShaderStage::Vertex;
  source.label = label;
  return source;
}

ShaderSource ShaderSource::fragment(std::string text, const char* label) {
  ShaderSource source;
  source.source = std::move(text);
  source.stage = backend::ShaderStage::Fragment;
  source.label = label;
  return source;
}

ShaderSource ShaderSource::directVertex(std::string text, const char* label) {
  ShaderSource source = vertex(std::move(text), label);
  source.directGlsl = true;
  return source;
}

ShaderSource ShaderSource::directFragment(std::string text, const char* label) {
  ShaderSource source = fragment(std::move(text), label);
  source.directGlsl = true;
  return source;
}

namespace {

/** @return the stage a GLSL file extension names, or false when it names none. */
bool stageFromExtension(const std::string& path, backend::ShaderStage* out) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) {
    return false;
  }
  // The dot has to belong to the file name, not to a directory: `shaders.v2/lighting` has no extension
  // at all, and reading its "extension" as `v2/lighting` would report a stage the caller never named.
  const size_t separator = path.find_last_of("/\\");
  if (separator != std::string::npos && separator > dot) {
    return false;
  }

  // Compared through a lowercase copy rather than by listing every spelling: `.VERT` on a file
  // exported from a case-insensitive filesystem is the same file, and refusing it would be a rule the
  // caller cannot see the reason for.
  std::string extension = path.substr(dot + 1);
  for (char& character: extension) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }

  if (extension == "vert") {
    *out = backend::ShaderStage::Vertex;
  } else if (extension == "frag") {
    *out = backend::ShaderStage::Fragment;
  } else if (extension == "comp") {
    *out = backend::ShaderStage::Compute;
  } else if (extension == "geom") {
    *out = backend::ShaderStage::Geometry;
  } else if (extension == "tesc") {
    *out = backend::ShaderStage::TessellationControl;
  } else if (extension == "tese") {
    *out = backend::ShaderStage::TessellationEvaluation;
  } else {
    return false;
  }
  return true;
}

/**
 * Reads @p path into @p out, removing a UTF-8 byte order mark.
 *
 * `std::ifstream` rather than a platform call, and a plain read rather than a mapping: a shader is a
 * few kilobytes, read once, and the standard library version is the one that behaves the same on
 * Windows, Android and a host test.
 */
bool readGlslFile(const std::string& path, std::string* out, std::string* outError) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    if (outError != nullptr) *outError = "cannot open " + path;
    return false;
  }

  const std::streampos end = file.tellg();
  if (end < 0) {
    if (outError != nullptr) *outError = "cannot measure " + path;
    return false;
  }

  std::string text(static_cast<size_t>(end), '\0');
  file.seekg(0);
  if (!text.empty() && !file.read(text.data(), static_cast<std::streamsize>(text.size()))) {
    if (outError != nullptr) *outError = "cannot read " + path;
    return false;
  }

  // A leading BOM is not whitespace to a GLSL parser: `\xEF\xBB\xBF#version` is a syntax error rather
  // than a directive, so a shader written by an editor that adds one fails with a message about the
  // first line. Removed here, at the only point where a file becomes a source, so that nothing
  // downstream has to know about it.
  static constexpr char kUtf8Bom[] = { '\xEF', '\xBB', '\xBF' };
  if (text.size() >= 3 && text.compare(0, 3, kUtf8Bom, 3) == 0) {
    text.erase(0, 3);
  }

  *out = std::move(text);
  return true;
}

} // namespace anonymous

ShaderSource ShaderSource::fromFile(const std::string& path, const backend::ShaderStage stage, std::string* outError) {
  ShaderSource source;
  source.stage = stage;
  // The only path a file read at run time can take without dragging a compiler into the build. A caller
  // that wants the SPIR-V path reads the text itself and leaves this alone.
  source.directGlsl = true;
  if (outError != nullptr) outError->clear();
  if (!readGlslFile(path, &source.source, outError)) {
    source.source.clear();
    return source;
  }

  // A file that exists and holds nothing is a packaging mistake rather than a shader, and it would
  // otherwise fail with no explanation at all: `Context::createShader` refuses an empty source without
  // saying which of its inputs was empty, which is the right call for a caller that passed a string and
  // the wrong one for a caller that passed a path.
  if (source.source.empty() && outError != nullptr) {
    *outError = path + " is empty";
  }
  return source;
}

ShaderSource ShaderSource::fromFile(const std::string& path, std::string* outError) {
  if (outError != nullptr) outError->clear();

  backend::ShaderStage stage = backend::ShaderStage::Vertex;
  if (!stageFromExtension(path, &stage)) {
    if (outError != nullptr) {
      *outError = "cannot tell which stage " + path + " holds from its extension; name one of "
                  ".vert, .frag, .comp, .geom, .tesc or .tese, or use the overload that takes a stage";
    }
    return ShaderSource {};
  }
  return fromFile(path, stage, outError);
}

Shader::Shader(std::shared_ptr<ContextState> state, const backend::ShaderModuleHandle handle,
        const backend::ShaderStage stage, std::string source, std::vector<uint32_t> spirv,
        const backend::ShaderSourceInfo info, const backend::ShaderCacheKey cacheKey,
        std::vector<Binding> bindings) noexcept
        : mState(std::move(state)),
          mHandle(handle),
          mStage(stage),
          mSource(std::move(source)),
          mSpirv(std::move(spirv)),
          mInfo(info),
          mCacheKey(cacheKey),
          mBindings(std::move(bindings)) {}

Shader::~Shader() {
  // See Buffer::~Buffer for why the state is checked rather than only the handle: a handle into a
  // destroyed device's pool is still "valid" by its own generation count.
  if (mState == nullptr || mState->device == nullptr) return;
  if (!mHandle.valid()) return;
  mState->device->destroyShaderModule(mHandle);
  mHandle = backend::ShaderModuleHandle::invalid();
}

std::string Shader::describeSource() const {
  if (!hasSource()) return "SPIR-V (no source)";

  std::ostringstream stream;
  stream << backend::toString(mInfo.language);

  if (mInfo.versionWasDeclared) {
    stream << " (#version " << mInfo.version;
    if (mInfo.language == ShaderLanguage::ESSL100 || mInfo.language == ShaderLanguage::ESSL300) stream << " es";
    stream << ")";
  } else {
    stream << " (no #version)";
  }

  if (mInfo.directGlsl) {
    // Named as its own provenance rather than as a retarget of something else. "compiled at 300" would
    // suggest Coral chose that version; it did not - the driver was handed the file and the file said
    // so. The distinction matters the moment the same source is run through the SPIR-V path, where the
    // version it lands on is the compiler's floor rather than the author's directive.
    stream << " compiled by the driver as written";
    return stream.str();
  }

  if (mInfo.versionWasRetargeted) {
    // Worth calling out rather than burying: a retargeted shader is compiled as a newer language than
    // it claims to be, which is the first thing to suspect when it behaves unexpectedly.
    stream << " retargeted to " << mInfo.compiledVersion;
  }
  if (mInfo.optimized) stream << ", optimised";
  return stream.str();
}

const Shader::Binding* Shader::findBinding(const uint32_t set, const uint32_t binding) const noexcept {
  for (const Binding& candidate: mBindings) {
    if (candidate.set == set && candidate.binding == binding) return &candidate;
  }
  return nullptr;
}

} // namespace graphics::gpu
