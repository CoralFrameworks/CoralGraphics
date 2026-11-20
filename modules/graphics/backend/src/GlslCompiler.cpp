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

/**
 * @file GlslCompiler.cpp
 *
 * GLSL source to SPIR-V, through shaderc.
 *
 * ## Why this exists
 *
 * The RHI's canonical shader input is SPIR-V, because it is the one form every backend can consume
 * without re-implementing a shader front end. That is the right interchange format and a poor authoring
 * format: nobody writes SPIR-V, and a shader that has to be compiled offline before it can be run is a
 * shader that cannot be hot-reloaded or debugged in place. This file closes that gap. GLSL in, SPIR-V
 * out, on the device, at load time.
 *
 * ## The version floor, and why the source is retargeted
 *
 * glslang will not emit SPIR-V from an old shader. The rule is enforced in glslang, not here, and it is
 * not configurable:
 *
 * ```
 * if (spvVersion.spv != 0) {
 *     case EEsProfile: if (version < 310) -> "ES shaders for SPIR-V require version 310 or higher"
 *     default:         if (version < 330) -> "Desktop shaders for OpenGL SPIR-V require version 330 or higher"
 * ```
 * (glslang/MachineIndependent/ShaderLang.cpp, in the `#version` parse.)
 *
 * So ESSL 1.00 and ESSL 3.00 cannot produce SPIR-V as written. Rather than refuse them, this file
 * rewrites the `#version` line to the lowest version glslang accepts and compiles the body unchanged.
 * For 3.00 source that is almost always correct, because ESSL 3.00 is a strict subset of 3.10. For
 * 1.00 source it is correct exactly when the shader avoids `attribute`, `varying`, `texture2D` and
 * `gl_FragColor`, which ES 3.00 removed; when it does not, the compiler's own diagnostics name the
 * offending line, and `retargetAdvice` below adds the reason.
 *
 * The alternative - a source-rewriting pass that modernises ES 1.00 keywords - is a shader compiler in
 * its own right, with its own bugs, and it would have to run before glslang's parser rather than with
 * it. Retargeting the version and reporting honestly what happened is the smaller, more predictable
 * thing.
 *
 * ## What is deliberately not done
 *
 * No caching here. `GlslCompileResult::sourceInfo` carries the hash and the version that a cache key is
 * built from, but where the cache lives - memory, disk, an Android asset - is the caller's decision,
 * and a compiler that silently cached would make a shader edit appear to do nothing.
 */

#include "GlslCompiler.h"
#include <backend/ShaderCompiler.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#if CORAL_BACKEND_HAS_SHADERC
#include <shaderc/shaderc.hpp>
#endif

namespace backend {
namespace {

#if CORAL_BACKEND_HAS_SHADERC
/**
 * This thread's shaderc compiler, created on first use and deliberately never destroyed.
 *
 * Constructing `shaderc::Compiler` builds a glslang instance and its built-in symbol table: ~45 ms here, which
 * used to be paid for every shader - twice per program, for every program - and is most of what a first frame
 * spends on compilation.
 *
 * Per thread, and leaked on purpose: glslang keeps its pool allocator in thread-local storage, so one instance
 * used from two threads is not safe; and letting an instance be destroyed would call `FinalizeProcess`, which
 * tears down state another thread's instance may still be using. A compiler that is never freed has neither
 * problem, and one per thread is a few kilobytes.
 */
shaderc::Compiler& threadGlslCompiler() {
  static thread_local shaderc::Compiler* cached = nullptr;
  if (cached == nullptr) cached = new shaderc::Compiler();
  return *cached;
}
#endif

/** FNV-1a. The same function the pipeline cache uses, so the two hashes are comparable in kind. */
uint64_t hashBytes(const void* data, const size_t size) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  uint64_t hash = 1469598103934665603ull;
  for (size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= 1099511628211ull;
  }
  return hash;
}

/**
 * Strips a UTF-8 BOM.
 *
 * A BOM is legal in a GLSL file by accident of editors rather than by the specification, and glslang
 * rejects it as an unexpected token at the very start of the source - a failure whose message points at
 * column 1 of line 1 and says nothing about encoding.
 */
std::string stripBom(const std::string& source) {
  if (source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xEF &&
      static_cast<unsigned char>(source[1]) == 0xBB && static_cast<unsigned char>(source[2]) == 0xBF) {
    return source.substr(3);
  }
  return source;
}

// ==================================================================================================
// Cross-process SPIR-V cache
// ==================================================================================================
//
// Compiling costs ~45 ms of glslang initialisation per thread and about half a millisecond per shader after
// that, so the thing worth avoiding is not the compile but starting from nothing at all. A completed module is
// keyed by everything that can change it - source, stage, target environment and version, entry point,
// optimisation level and the compiler's own version string - and written to a per-user directory.
//
// Everything here is validated on read: a truncated file, a file from a different compiler version, a file
// whose payload does not match its recorded hash is a cache miss, never a broken shader. Writes go to a
// temporary file that is renamed into place, because two processes can legitimately compile the same shader at
// the same time and both will try to store it.

namespace {

constexpr uint32_t kCacheMagic = 0x43485343u; // "CSHC"
constexpr uint32_t kCacheFormatVersion = 1u;

std::atomic<uint64_t> gCacheHits { 0 };
std::atomic<uint64_t> gCacheMisses { 0 };

/** The configured directory. Empty means the cache is off; both are guarded because a setter exists. */
std::mutex& cacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::string& cacheDirectory() {
  static std::string directory = [] {
    if (const char* env = std::getenv("CORAL_SHADER_CACHE_DIR")) {
      return std::string(env);
    }
    if (std::getenv("CORAL_DISABLE_SHADER_CACHE") != nullptr) {
      return std::string();
    }
#if defined(_WIN32)
    if (const char* local = std::getenv("LOCALAPPDATA")) {
      return std::string(local) + "\\Coral\\shader-cache";
    }
    return std::string();
#elif defined(__ANDROID__)
    // The backend cannot know the application's cache directory; the app calls setShaderCacheDirectory().
    return std::string();
#else
    if (const char* xdg = std::getenv("XDG_CACHE_HOME")) {
      return std::string(xdg) + "/coral/shader-cache";
    }
    if (const char* home = std::getenv("HOME")) {
      return std::string(home) + "/.cache/coral/shader-cache";
    }
    return std::string();
#endif
  }();
  return directory;
}

/**
 * What the compiler pipeline itself contributes to a key.
 *
 * `kCacheGeneratorVersion` is bumped by hand whenever the way this file turns a shader into SPIR-V changes -
 * a new prelude, a different flag, a retargeting rule - because none of that shows up in the source text and a
 * stale entry would otherwise survive it. The caller's salt covers everything else the engine cannot see.
 */
constexpr uint32_t kCacheGeneratorVersion = 2u;

std::string& cacheSalt() {
  static std::string salt = [] {
    if (const char* env = std::getenv("CORAL_SHADER_CACHE_SALT")) return std::string(env);
    return std::string();
  }();
  return salt;
}

/** @return the key for one compilation: a hash over everything that can change the output. */
uint64_t cacheKey(const std::string& source, const std::string& stageAndTarget, const std::string& entryPoint,
                  const bool optimize) {
  std::string material = "coral-spirv-cache-v1|";
  material += std::to_string(kCacheGeneratorVersion);
  material += '|';
  material += getGlslCompilerVersion();
  material += '|';
  material += stageAndTarget;
  material += '|';
  material += entryPoint;
  material += '|';
  material += optimize ? '1' : '0';
  material += '|';
  material += cacheSalt();
  material += '|';
  material += source;
  return hashBytes(material.data(), material.size());
}

std::filesystem::path cachePath(const uint64_t key) {
  char name[32];
  std::snprintf(name, sizeof(name), "%016llx.spv", static_cast<unsigned long long>(key));
  return std::filesystem::path(cacheDirectory()) / name;
}

// ==================================================================================================
// Disk LRU
// ==================================================================================================
//
// The cache is per user and shared by every process, so it has to bound itself: a shader hash changes whenever
// the source does, and a long-lived application that iterates on shaders would otherwise accumulate entries
// forever. Recency is the file's modification time - a hit touches it, which is one syscall and no bookkeeping -
// and the eviction pass deletes the oldest entries until the directory fits its budget.
//
// The pass is not run on every write: it walks the directory, and paying that per compile would cost more than
// the cache saves. It runs on the first write of a process (so a directory some earlier run left oversized
// shrinks at start-up) and after every half-budget's worth of bytes written, which adapts to the entry size
// instead of guessing an interval in compiles.
//
// Two processes can evict at the same time, and one can delete a file the other is about to read. That is a
// cache miss, not a problem: the file is validated on read and recompiled if absent.

std::atomic<uint64_t>& cacheWrites() {
  static std::atomic<uint64_t> writes { 0 };
  return writes;
}

/** Bytes written since the last eviction pass, which is what decides when the next one runs. */
std::atomic<uint64_t>& cacheBytesSinceEviction() {
  static std::atomic<uint64_t> bytes { 0 };
  return bytes;
}

/** The budget in bytes; 0 means unbounded. */
std::atomic<uint64_t>& cacheBudgetBytes() {
  static std::atomic<uint64_t> budget { [] {
    if (const char* env = std::getenv("CORAL_SHADER_CACHE_BUDGET")) {
      const auto parsed = std::strtoull(env, nullptr, 10);
      if (parsed != 0) return static_cast<uint64_t>(parsed);
    }
    return static_cast<uint64_t>(8ull * 1024ull * 1024ull);
  }() };
  return budget;
}

/** @return the number of entries and their total size, as they are on disk right now. */
std::pair<uint64_t, uint64_t> cacheUsage() {
  uint64_t entries = 0;
  uint64_t bytes = 0;
  if (cacheDirectory().empty()) return { 0, 0 };
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(cacheDirectory(), error)) {
    if (error) break;
    if (!entry.is_regular_file(error)) continue;
    const std::string name = entry.path().filename().string();
    if (name.size() < 4 || name.compare(name.size() - 4, 4, ".spv") != 0) continue;
    ++entries;
    const auto size = entry.file_size(error);
    if (!error) bytes += size;
  }
  return { entries, bytes };
}

/** Deletes the least recently used entries until the directory fits its budget. */
void evictIfOverBudget() {
  if (cacheDirectory().empty()) return;
  const uint64_t budget = cacheBudgetBytes().load(std::memory_order_relaxed);
  if (budget == 0) return;

  std::error_code error;
  struct Entry {
    std::filesystem::path path;
    std::filesystem::file_time_type used;
    uint64_t size = 0;
  };
  std::vector<Entry> entries;
  uint64_t total = 0;
  for (const auto& entry : std::filesystem::directory_iterator(cacheDirectory(), error)) {
    if (error) return;
    if (!entry.is_regular_file(error)) continue;
    const std::string name = entry.path().filename().string();
    if (name.size() < 4 || name.compare(name.size() - 4, 4, ".spv") != 0) continue;
    const auto size = entry.file_size(error);
    if (error) continue;
    const auto used = entry.last_write_time(error);
    if (error) continue;
    entries.push_back(Entry { entry.path(), used, size });
    total += size;
  }
  if (total <= budget) return;

  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.used < b.used; });
  for (const Entry& entry : entries) {
    if (total <= budget) break;
    std::error_code removeError;
    if (std::filesystem::remove(entry.path, removeError)) total -= entry.size;
  }
}

/**
 * The cache file's fixed-size header.
 *
 * `payloadHash` is 64 bits, like the hash that produced it: the first version of this header kept only the low
 * 32 bits and every read then failed its own integrity check - a bug worth not repeating.
 */
struct CacheHeader {
  uint32_t magic = kCacheMagic;
  uint32_t version = kCacheFormatVersion;
  uint64_t key = 0;
  uint32_t wordCount = 0;
  uint32_t reserved = 0;
  uint64_t payloadHash = 0;
};

/** @return true when @p words was read from a valid cache file for @p key. */
bool readCacheFile(const uint64_t key, std::vector<uint32_t>& words) {
  if (cacheDirectory().empty()) return false;
  const std::filesystem::path path = cachePath(key);
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;

  CacheHeader header;
  file.read(reinterpret_cast<char*>(&header), sizeof(header));
  if (file.gcount() != static_cast<std::streamsize>(sizeof(header))) return false;
  if (header.magic != kCacheMagic || header.version != kCacheFormatVersion || header.key != key) return false;
  if (header.wordCount == 0 || header.wordCount > (64u * 1024u)) return false;

  words.resize(header.wordCount);
  file.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(header.wordCount) * 4);
  if (file.gcount() != static_cast<std::streamsize>(header.wordCount) * 4) {
    words.clear();
    return false;
  }
  if (words.front() != 0x07230203u) {
    words.clear();
    return false;
  }
  if (hashBytes(words.data(), words.size() * sizeof(uint32_t)) != header.payloadHash) {
    words.clear();
    return false;
  }
  // Recency for the LRU pass: a hit is a use. Best effort - a filesystem that refuses to set the time only
  // makes this entry look older than it is.
  std::error_code touchError;
  std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), touchError);
  return true;
}

void writeCacheFile(const uint64_t key, const std::vector<uint32_t>& words) {
  if (cacheDirectory().empty() || words.empty() || words.size() > (64u * 1024u)) return;
  std::error_code error;
  std::filesystem::create_directories(cacheDirectory(), error);

  const std::filesystem::path path = cachePath(key);
  const std::filesystem::path temporary =
    path.string() + ".tmp." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) return;
    CacheHeader header;
    header.key = key;
    header.wordCount = static_cast<uint32_t>(words.size());
    header.payloadHash = hashBytes(words.data(), words.size() * sizeof(uint32_t));
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(words.data()),
               static_cast<std::streamsize>(words.size() * sizeof(uint32_t)));
    if (!file) {
      file.close();
      std::filesystem::remove(temporary, error);
      return;
    }
  }
  // Two processes can compile the same shader at once; the loser's file is identical, so replace and ignore.
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    if (error) {
      std::filesystem::remove(temporary, error);
      return;
    }
  }
  // The LRU pass walks the directory, so it is not worth paying per compile. It runs on the first write of a
  // process (to shrink whatever an earlier run left oversized) and after half the budget has been written, which
  // scales with the entry size rather than with a guessed number of compiles.
  const uint64_t writes = cacheWrites().fetch_add(1, std::memory_order_relaxed) + 1;
  const uint64_t budget = cacheBudgetBytes().load(std::memory_order_relaxed);
  const uint64_t written = sizeof(CacheHeader) + static_cast<uint64_t>(words.size()) * 4u;
  const uint64_t sinceEviction = cacheBytesSinceEviction().fetch_add(written, std::memory_order_relaxed) + written;
  if (writes == 1 || (budget != 0 && sinceEviction >= budget / 2)) {
    cacheBytesSinceEviction().store(0, std::memory_order_relaxed);
    evictIfOverBudget();
  }
}

} // namespace

/** @return the `#version` value in @p source, or 0 when there is none. */
uint32_t findVersionDirective(const std::string& source, std::string* outLine) {
  // Only the first non-blank, non-comment line may carry `#version`, so the scan stops at the first
  // line that is neither. That is stricter than a substring search and is what keeps a `#version`
  // inside a comment from being mistaken for the real one.
  size_t offset = 0;
  while (offset < source.size()) {
    const size_t lineEnd = source.find('\n', offset);
    const std::string line = source.substr(offset, lineEnd == std::string::npos ? std::string::npos : lineEnd - offset);
    offset = lineEnd == std::string::npos ? source.size() : lineEnd + 1;

    const size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos) continue; // blank
    if (line[first] == '/' || line[first] == '#') {
      if (line.compare(first, 8, "#version") != 0) continue; // a comment, or a different directive
    }

    if (line.compare(first, 8, "#version") != 0) return 0; // code before any version: none declared
    if (outLine != nullptr) *outLine = line;

    // "#version" then the number. Anything that is not a digit means a malformed directive, which is
    // better treated as absent than guessed at.
    size_t index = first + 8;
    while (index < line.size() && (line[index] == ' ' || line[index] == '\t')) ++index;
    uint32_t version = 0;
    while (index < line.size() && line[index] >= '0' && line[index] <= '9') {
      version = version * 10 + static_cast<uint32_t>(line[index] - '0');
      ++index;
    }
    return version;
  }
  return 0;
}

/** @return true when @p line declares the ES profile, i.e. ends in `es`. */
bool lineDeclaresEs(const std::string& line) noexcept {
  const size_t es = line.rfind("es");
  if (es == std::string::npos) {
    return false;
  }
  // Only whitespace may follow.
  for (size_t index = es + 2; index < line.size(); ++index) {
    if (line[index] != ' ' && line[index] != '\t' && line[index] != '\r') {
      return false;
    }
  }
  return true;
}

/** Which `ShaderSourceLanguage` a declared version and profile imply. */
ShaderSourceLanguage languageFromVersion(const uint32_t version, const bool declaresEs) noexcept {
  if (declaresEs || version == 100) {
    return version >= 300 ? ShaderSourceLanguage::ESSL300 : ShaderSourceLanguage::ESSL100;
  }
  return ShaderSourceLanguage::GLSL410;
}

/** Replaces the `#version` directive with @p version / @p profile, prepending one when absent. */
std::string retargetVersion(const std::string& source, const uint32_t version, const bool es) {
  const std::string directive =
    es ? "#version " + std::to_string(version) + " es" : "#version " + std::to_string(version) + " core";

  std::string existing;
  if (findVersionDirective(source, &existing) != 0) {
    // Replace the line, preserving the file's own line endings so that compiler diagnostics keep
    // pointing at the line the author is looking at.
    const size_t start = source.find(existing);
    std::string replaced = source;
    replaced.replace(start, existing.size(), directive);
    return replaced;
  }

  // No directive: prepend one. A newline is added after it so that the original first line keeps its number.
  return directive + "\n" + source;
}

/**
 * @return advice for the caller when a retargeted compile failed, or an empty string.
 *
 * Only the ESSL 1.00 case gets one, because it is the only case where the retarget is likely to be the
 * cause rather than the caller's code. The keywords named were removed in ES 3.00, so a 1.00 shader
 * using them cannot be made to compile by changing the version alone - the source has to change, and
 * saying so here is the difference between a five-minute fix and an afternoon.
 */
std::string retargetAdvice(const ShaderSourceInfo& info, const std::string& compilerError) {
  if (info.language != ShaderSourceLanguage::ESSL100) {
    return {};
  }

  static const char* kRemovedInEs300[] = { "attribute", "varying", "texture2D", "gl_FragColor" };
  for (const char* keyword: kRemovedInEs300) {
    if (compilerError.find(keyword) == std::string::npos) {
      continue;
    }

    std::string advice = "\n--- this ESSL 1.00 source cannot become SPIR-V ---\n";
    advice += '\'';
    advice += keyword;
    advice += "' was removed in OpenGL ES 3.00, and glslang cannot emit SPIR-V below GLSL ES 3.10.\n";
    advice += "Rewrite it as ES 3.00 (in/out, texture(), an explicit out variable) and declare\n";
    advice += "#version 300 es, or keep the shader as ESSL 1.00 and do not compile it to SPIR-V.";
    return advice;
  }
  return {};
}

#if CORAL_BACKEND_HAS_SHADERC

/** Maps an RHI stage onto shaderc's kind. */
shaderc_shader_kind toShaderKind(const ShaderStage stage) noexcept {
  switch (stage) {
    case ShaderStage::Vertex:
      return shaderc_glsl_vertex_shader;
    case ShaderStage::Fragment:
      return shaderc_glsl_fragment_shader;
    case ShaderStage::Compute:
      return shaderc_glsl_compute_shader;
    case ShaderStage::Geometry:
      return shaderc_glsl_geometry_shader;
    case ShaderStage::TessellationControl:
      return shaderc_glsl_tess_control_shader;
    case ShaderStage::TessellationEvaluation:
      return shaderc_glsl_tess_evaluation_shader;
  }
  return shaderc_glsl_vertex_shader;
}

#endif // CORAL_BACKEND_HAS_SHADERC

} // namespace anonymous

bool isGlslCompilerAvailable() noexcept {
#if CORAL_BACKEND_HAS_SHADERC
  return true;
#else
  return false;
#endif
}

bool warmUpShaderCompiler() {
#if CORAL_BACKEND_HAS_SHADERC
  // The point is `threadGlslCompiler()`: building the compiler is the 45 ms, and this call is what pays it. The
  // shader itself is deliberately trivial, and the disk cache is not consulted or written - a warm-up that left a
  // dummy entry behind would be the sort of thing this cache exists to avoid.
  shaderc::Compiler& compiler = threadGlslCompiler();
  if (!compiler.IsValid()) return false;

  shaderc::CompileOptions options;
  options.SetSourceLanguage(shaderc_source_language_glsl);
  options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);
  options.SetTargetSpirv(shaderc_spirv_version_1_0);
  options.SetOptimizationLevel(shaderc_optimization_level_zero);

  static const char* kSource = "#version 310 es\nvoid main() { gl_Position = vec4(0.0); }\n";
  const shaderc::SpvCompilationResult compilation = compiler.CompileGlslToSpv(
          kSource, shaderc_vertex_shader, "coral/warm-up", "main", options);
  // A failure here is not a failure of the warm-up: the compiler exists and is initialised, which is all this
  // promises. Reporting success keeps a caller from treating a toolchain quirk as "no compiler".
  (void) compilation;
  return true;
#else
  return false;
#endif
}

Result compileShaderToSpirv(const std::string& source, const ShaderStage stage, ShaderCompileResult& result) {
  result.spirv.clear();
  result.error.clear();
  if (!isGlslCompilerAvailable()) {
    result.error = "this build has no GLSL compiler";
    return Result::Unsupported;
  }
  GlslCompileResult compiled = compileGlslToSpirvVulkan(source, stage);
  if (!compiled.success()) {
    result.error = compiled.error.empty() ? "shader compilation failed" : compiled.error;
    return Result::Error;
  }
  result.spirv = std::move(compiled.spirv);
  return Result::Success;
}

const char* getGlslCompilerVersion() noexcept {
#if CORAL_BACKEND_HAS_SHADERC
  // shaderc does not expose its own version at run time; the SPIR-V version it targets is the closest
  // stable identifier it does expose, and it moves when the vendored tree moves for any reason that
  // matters to a cache.
  unsigned int version = 0;
  unsigned int revision = 0;
  shaderc_get_spv_version(&version, &revision);
  static const std::string text = "shaderc/spv" + std::to_string(version) + "." + std::to_string(revision);
  return text.c_str();
#else
  return "unavailable";
#endif
}

uint32_t getMinimumSpirvSourceVersion(const ShaderSourceLanguage language) noexcept {
  // The floor glslang enforces, per profile. The two are genuinely different numbers, and using one for
  // both is a bug in the direction that matters: a desktop source below 330 fails to compile at all, so
  // a shared floor of 310 would let it through to a compiler that then refuses it while the retargeting
  // logic never fired.
  switch (language) {
    case ShaderSourceLanguage::ESSL100:
    case ShaderSourceLanguage::ESSL300:
      return 310;
    case ShaderSourceLanguage::GLSL410:
      // 410 rather than glslang's 330, because 410 is this dialect's own baseline: the backend asks the
      // driver for a 4.1 core context, so handing it a module compiled at 3.30 would be a mismatch Coral
      // introduced rather than one it inherited. Reporting a retarget is the honest outcome.
      return 410;
    case ShaderSourceLanguage::Detect:
      // A source that has not said which profile it is gets the ES floor, because ES is the
      // specification's default for a source with no `#version`. The desktop case is only reached by
      // asking for it explicitly.
      return 310;
  }
  return 310;
}

/**
 * The version a source is compiled at for @p target, which is not the version the driver sees.
 *
 * ## Why desktop is 420 rather than the 410 the context itself provides
 *
 * The source and the driver's module are two different things: this number decides what **glslang** is
 * asked to accept, and `crossCompileToGLSL` emits the driver's dialect afterwards regardless
 * (`ShaderCrossCompileInfo::forDialect`, and `enable_420pack_extension` is off below 420, so the generated
 * GLSL carries no `binding` qualifiers at all -- the backend binds the points itself). So compiling at a
 * higher version than the context costs nothing at run time.
 *
 * What it buys is the qualifier every shader of the canvas's shape uses. Measured, and pinned by
 * `GlslCompiler.AcceptsTheBindingQualifierAtTheDesktopFloor`: at 410 glslang rejects
 * `layout(binding = 0) uniform sampler2D` **and** `layout(binding = 1) uniform Block` -- with
 * `'binding' : not supported for this version` for the first and
 * `'binding' : uniform/buffer blocks require layout(binding=X)` for a block that omits it, so a block
 * cannot be declared at all. At 420 both compile and the reflection reports real `(set, binding)` pairs,
 * which is what makes a descriptor set layout derivable from a run-time shader instead of restated by the
 * caller.
 *
 * The ES floor stays 310 because that is already where ES gained the qualifier (`tools/probes/effect_orient_probe.exe`
 * prints the same four cases).
 */
uint32_t getRetargetVersion(const ShaderSourceLanguage language, const GlDialect target) noexcept {
  if (target == GlDialect::ES) {
    return 310;
  }
  // Desktop. 420 is the lowest desktop version at which `layout(binding = N)` is legal, which is what a
  // shader written in the canvas's style needs; see the note above.
  (void) language;
  return 420;
}

void parseGlslVersion(const std::string& source, ShaderSourceInfo* outInfo) {
  if (outInfo == nullptr) {
    return;
  }
  *outInfo = ShaderSourceInfo{};

  const std::string text = stripBom(source);
  outInfo->sourceSize = text.size();
  outInfo->sourceHash = hashBytes(text.data(), text.size());

  std::string line;
  const uint32_t declared = findVersionDirective(text, &line);
  if (declared == 0) {
    // The specification's own default: no `#version` means GLSL ES 1.00.
    outInfo->language = ShaderSourceLanguage::ESSL100;
    outInfo->version = 100;
    outInfo->versionWasDeclared = false;
    return;
  }

  outInfo->language = languageFromVersion(declared, lineDeclaresEs(line));
  outInfo->version = declared;
  outInfo->versionWasDeclared = true;
}

GlslCompileResult compileGlslToSpirv(const std::string& source, const ShaderStage stage, const GlDialect target,
  const ShaderSourceLanguage language, const char* entryPoint, const bool optimize, const char* label) {
  GlslCompileResult result;

  const std::string text = stripBom(source);
  if (text.empty()) {
    result.error = "the shader source is empty";
    return result;
  }

  parseGlslVersion(text, &result.sourceInfo);

  // An explicit request overrides what the source says. That is the whole purpose of the parameter: a
  // snippet with no `#version` that uses modern syntax would otherwise be compiled as ES 1.00 and
  // rejected for reasons the author cannot see.
  if (language != ShaderSourceLanguage::Detect) {
    result.sourceInfo.language = language;
    result.sourceInfo.version = language == ShaderSourceLanguage::GLSL410
                                  ? 410
                                  : language == ShaderSourceLanguage::ESSL300
                                  ? 300
                                  : 100;
  }

  // The retarget aims at the *target*, not at the source's own profile. Those differ in the case that
  // matters: an undeclared source is ESSL 1.00 by the specification, but on a desktop context it has to
  // become desktop GLSL, because ES profile SPIR-V is not what a core-profile context consumes - glslang
  // fails to parse its own built-ins when the two are mixed, which surfaces as a confusing internal
  // error rather than as a version complaint.
  const bool targetIsEs = target == GlDialect::ES;
  const uint32_t minimum = getRetargetVersion(result.sourceInfo.language, target);
  result.compiledVersion = std::max(result.sourceInfo.version, minimum);
  result.versionWasRetargeted = result.sourceInfo.version < minimum;
  const bool es = targetIsEs;

  // Published into the source info as well, so that a single value describes everything a cache key
  // has to cover and a caller does not have to join two structures to build one.
  result.sourceInfo.compiledVersion = result.compiledVersion;
  result.sourceInfo.versionWasRetargeted = result.versionWasRetargeted;
  result.sourceInfo.optimized = optimize;

#if !CORAL_BACKEND_HAS_SHADERC
  (void) entryPoint;
  (void) optimize;
  (void) label;
  result.error = "this build has no GLSL compiler: shaderc was not found or "
    "-DCORAL_BACKEND_SUPPORTS_SHADERC=OFF was given. Supply the shader as SPIR-V instead.";
  return result;
#else
  // A desktop-profile source must say so itself.
  //
  // This is not a stylistic preference; it is a defect workaround with a reason. The obvious way to tell
  // the compiler "treat this as desktop GLSL 4.10" is shaderc's `SetForcedVersionProfile`, and that is
  // exactly what must not be done for a desktop profile: the hook makes glslang parse its built-in
  // symbols with no profile selected, which fails with
  //
  //     ERROR: 0:334: 'double' : not supported with this profile: none
  //     INTERNAL ERROR: Unable to parse built-ins
  //
  // and then reports the misleading `Desktop shaders for OpenGL SPIR-V require version 330 or higher`.
  // Rewriting the source's own `#version` line avoids the hook entirely and produces the right module.
  //
  // The cost is that a desktop shader with no `#version` at all is refused instead of being guessed at.
  // That is the correct trade: a desktop shader without a version directive is already ambiguous, and
  // silently compiling it as 4.10 would be exactly the substitution this file exists to avoid.
  // A desktop *source* that names itself desktop must declare a version; a source being retargeted to a
  // desktop context from an ES profile is a different case and is handled by the rewrite above.
  if (!targetIsEs && result.sourceInfo.language == ShaderSourceLanguage::GLSL410 &&
      !result.sourceInfo.versionWasDeclared) {
    result.error = "a desktop GLSL shader must declare its own `#version` (for example `#version 410 core`); "
      "the compiler cannot infer a desktop profile, and guessing one would compile the shader as a "
      "language it did not declare.";
    return result;
  }

  // Retarget only when the source declares a version the compiler cannot emit SPIR-V from. Rewriting a
  // version that would have worked would change the language the author wrote, which is exactly the
  // kind of silent substitution this path exists to avoid.
  const std::string compiledSource =
    result.versionWasRetargeted ? retargetVersion(text, result.compiledVersion, es) : text;

  const std::string inputName = label != nullptr && *label != '\0' ? label : "shader";
  const char* entryChar = entryPoint != nullptr && *entryPoint != '\0' ? entryPoint : "main";
  const std::string entry = entryChar;

  // A cache hit skips building the compiler at all, which is the whole 45 ms.
  std::string stageAndTarget = "opengl|";
  stageAndTarget += std::to_string(static_cast<int>(stage));
  stageAndTarget += '|';
  stageAndTarget += std::to_string(static_cast<int>(result.compiledVersion));
  const uint64_t key = cacheKey(compiledSource, stageAndTarget, entry, optimize);
  if (readCacheFile(key, result.spirv)) {
    gCacheHits.fetch_add(1, std::memory_order_relaxed);
    return result;
  }
  gCacheMisses.fetch_add(1, std::memory_order_relaxed);

  shaderc::Compiler& compiler = threadGlslCompiler();
  if (!compiler.IsValid()) {
    result.error = "the GLSL compiler could not be initialised";
    return result;
  }

  shaderc::CompileOptions options;
  options.SetSourceLanguage(shaderc_source_language_glsl);
  // OpenGL semantics, not Vulkan: the SPIR-V is consumed by SPIRV-Cross and then by a GL driver, and
  // the Vulkan environment would emit bindings and coordinate conventions that the OpenGL path then has
  // to undo.
  options.SetTargetEnvironment(shaderc_target_env_opengl, 0);
  // 1.0 is what SPIRV-Cross and every OpenGL driver accept. Asking for more buys nothing here and
  // narrows the set of consumers.
  options.SetTargetSpirv(shaderc_spirv_version_1_0);
  options.SetOptimizationLevel(optimize ? shaderc_optimization_level_performance : shaderc_optimization_level_zero);

  const shaderc::SpvCompilationResult compilation =
    compiler.CompileGlslToSpv(compiledSource, toShaderKind(stage), inputName.c_str(), entryChar, options);

  if (compilation.GetCompilationStatus() != shaderc_compilation_status_success) {
    result.error = compilation.GetErrorMessage();
    if (result.error.empty()) result.error = "the GLSL compiler reported a failure with no message";

    const std::string advice = retargetAdvice(result.sourceInfo, result.error);
    if (!advice.empty()) result.error += advice;
    return result;
  }

  result.spirv.assign(compilation.cbegin(), compilation.cend());
  if (result.spirv.empty()) {
    result.error = "the GLSL compiler produced no SPIR-V and reported no error";
    return result;
  }
  // The OpenGL path has to write what it reads. It was reading the cache and counting misses but never storing
  // the result, so on a GL device - where SPIR-V goes through SPIRV-Cross - every start compiled again and the
  // counters read `misses=4 entries=0` forever.
  writeCacheFile(key, result.spirv);
  return result;
#endif
}

GlslCompileResult compileGlslToSpirvVulkan(const std::string& source, const ShaderStage stage,
        const ShaderSourceLanguage language, const char* entryPoint, const bool optimize, const char* label) {
  GlslCompileResult result;

  const std::string text = stripBom(source);
  if (text.empty()) {
    result.error = "the shader source is empty";
    return result;
  }

  parseGlslVersion(text, &result.sourceInfo);
  if (language != ShaderSourceLanguage::Detect) {
    result.sourceInfo.language = language;
    result.sourceInfo.version = language == ShaderSourceLanguage::GLSL410
                                  ? 450
                                  : language == ShaderSourceLanguage::ESSL300
                                  ? 300
                                  : 100;
  }

  // ## The Vulkan floor, per profile
  //
  // glslang refuses to emit Vulkan-environment SPIR-V below GLSL ES 3.10 or desktop GLSL 4.50. The
  // sources in this tree are all `#version 300 es`, so the ES case is the one that actually fires;
  // the desktop floor is stated for a caller who writes a desktop shader as well.
  const bool es = result.sourceInfo.language != ShaderSourceLanguage::GLSL410;
  const uint32_t minimum = es ? 310u : 450u;
  result.compiledVersion = std::max(result.sourceInfo.version, minimum);
  result.versionWasRetargeted = result.sourceInfo.version < minimum;
  result.sourceInfo.compiledVersion = result.compiledVersion;
  result.sourceInfo.versionWasRetargeted = result.versionWasRetargeted;
  result.sourceInfo.optimized = optimize;

#if !CORAL_BACKEND_HAS_SHADERC
  (void) entryPoint;
  (void) optimize;
  (void) label;
  result.error = "this build has no GLSL compiler: shaderc was not found or "
    "-DCORAL_BACKEND_SUPPORTS_SHADERC=OFF was given. The Vulkan backend requires shaderc because its "
    "platform API accepts SPIR-V only.";
  return result;
#else
  const std::string compiledSource =
    result.versionWasRetargeted ? retargetVersion(text, result.compiledVersion, es) : text;

  const std::string inputName = label != nullptr && *label != '\0' ? label : "shader";
  const char* entryChar = entryPoint != nullptr && *entryPoint != '\0' ? entryPoint : "main";
  const std::string entry = entryChar;

  // A cache hit skips building the compiler at all, which is the whole 45 ms.
  std::string stageAndTarget = "vulkan|";
  stageAndTarget += std::to_string(static_cast<int>(stage));
  stageAndTarget += '|';
  stageAndTarget += std::to_string(static_cast<int>(result.compiledVersion));
  const uint64_t key = cacheKey(compiledSource, stageAndTarget, entry, optimize);
  if (readCacheFile(key, result.spirv)) {
    gCacheHits.fetch_add(1, std::memory_order_relaxed);
    return result;
  }
  gCacheMisses.fetch_add(1, std::memory_order_relaxed);

  shaderc::Compiler& compiler = threadGlslCompiler();
  if (!compiler.IsValid()) {
    result.error = "the GLSL compiler could not be initialised";
    return result;
  }

  shaderc::CompileOptions options;
  options.SetSourceLanguage(shaderc_source_language_glsl);
  // The whole reason this entry point exists. The Vulkan environment is what emits the
  // `DescriptorSet`/`Binding` decorations a `VkDescriptorSetLayout` has to agree with.
  options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);
  options.SetTargetSpirv(shaderc_spirv_version_1_0);
  options.SetOptimizationLevel(optimize ? shaderc_optimization_level_performance : shaderc_optimization_level_zero);

  const shaderc::SpvCompilationResult compilation =
    compiler.CompileGlslToSpv(compiledSource, toShaderKind(stage), inputName.c_str(), entryChar, options);

  if (compilation.GetCompilationStatus() != shaderc_compilation_status_success) {
    result.error = compilation.GetErrorMessage();
    if (result.error.empty()) result.error = "the GLSL compiler reported a failure with no message";
    if (result.versionWasRetargeted) {
      result.error += "\n(note: the source declares GLSL ";
      result.error += std::to_string(result.sourceInfo.version);
      result.error += " and was compiled as ";
      result.error += std::to_string(result.compiledVersion);
      result.error += es ? " es" : " core";
      result.error += ", which is the lowest version that can become Vulkan SPIR-V)";
    }
    return result;
  }

  result.spirv.assign(compilation.cbegin(), compilation.cend());
  if (result.spirv.empty()) {
    result.error = "the GLSL compiler produced no SPIR-V and reported no error";
    return result;
  }
  writeCacheFile(key, result.spirv);
  return result;
#endif
}

} // namespace backend

namespace backend {

void setShaderCacheDirectory(const std::string& directory) {
  const std::lock_guard<std::mutex> lock(cacheMutex());
  cacheDirectory() = directory;
  if (directory.empty()) return;
  std::error_code error;
  std::filesystem::create_directories(directory, error);
}

std::string shaderCacheDirectory() {
  const std::lock_guard<std::mutex> lock(cacheMutex());
  return cacheDirectory();
}

void setShaderCacheSalt(const std::string& salt) {
  const std::lock_guard<std::mutex> lock(cacheMutex());
  cacheSalt() = salt;
}

std::string shaderCacheSalt() {
  const std::lock_guard<std::mutex> lock(cacheMutex());
  return cacheSalt();
}

void setShaderCacheBudget(const uint64_t bytes) {
  cacheBudgetBytes().store(bytes, std::memory_order_relaxed);
  // Shrink now rather than at the next 64 writes: a caller that lowers the budget wants it to take effect.
  evictIfOverBudget();
}

uint64_t shaderCacheBudget() noexcept {
  return cacheBudgetBytes().load(std::memory_order_relaxed);
}

void shaderCacheUsage(uint64_t* outEntries, uint64_t* outBytes) {
  const auto usage = cacheUsage();
  if (outEntries != nullptr) *outEntries = usage.first;
  if (outBytes != nullptr) *outBytes = usage.second;
}

uint64_t shaderCacheHits() noexcept {
  return gCacheHits.load(std::memory_order_relaxed);
}

uint64_t shaderCacheMisses() noexcept {
  return gCacheMisses.load(std::memory_order_relaxed);
}

} // namespace backend
