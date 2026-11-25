#pragma once

// Public, device-free GLSL -> SPIR-V compilation entry point.
//
// This is the backend half of the M9 async shader path: it touches no Device, no command buffer and no
// graphics context, so a worker thread may call it while the render thread keeps submitting frames.
// Creating shader modules from the returned words remains a render-thread operation.

#include <backend/Types.h>

#include <cstdint>
#include <string>
#include <vector>

namespace backend {

struct ShaderCompileResult {
  /** SPIR-V words for the Vulkan environment. Empty on failure. */
  std::vector<uint32_t> spirv;
  /** Compiler diagnostics. Never empty on failure. */
  std::string error;

  [[nodiscard]] bool success() const noexcept { return error.empty() && !spirv.empty(); }
};

/** @return true when this build has a GLSL compiler (shaderc/glslang) available. */
[[nodiscard]] bool isShaderCompilerAvailable() noexcept;

/**
 * Initialises the GLSL compiler on the calling thread.
 *
 * The first compile on a thread pays about 45 ms for the compiler and its built-in symbol table, which an engine
 * would rather pay during `start()` than inside the first frame. This compiles a throwaway shader so that cost is
 * behind you; the result is discarded and the disk cache is not touched, so no dummy entry is created.
 *
 * The compiler is per thread, so a caller with a compile worker must warm that thread too - submitting this as a
 * task is exactly that.
 *
 * @return false when this build has no compiler; true otherwise (a warm-up that fails to compile still leaves the
 *         compiler initialised, which is the point).
 */
bool warmUpShaderCompiler();

/**
 * Compiles @p source to SPIR-V for @p stage.
 *
 * Safe to call from a worker thread: this function never touches a Device. The returned module targets
 * the Vulkan environment, so the render thread can hand it to `Device::createShaderModule` with
 * `Language::SPIRV`. On failure the result carries the compiler's diagnostics.
 */
Result compileShaderToSpirv(const std::string& source, ShaderStage stage, ShaderCompileResult& result);

// ==================================================================================================
// Cross-process SPIR-V cache
// ==================================================================================================

/**
 * The directory the compiler stores completed SPIR-V modules in, per user and per machine.
 *
 * Empty means "no cache": every compile pays the full glslang cost. The default is a per-user cache
 * directory (`%LOCALAPPDATA%\Coral\shader-cache` on Windows, `$XDG_CACHE_HOME` or `~/.cache` on Linux);
 * platforms where the backend cannot know a writable location - Android, where the application owns its
 * cache dir - start with no cache and call this setter once, early.
 *
 * Setting it creates the directory. Entries are keyed by everything that can change the output - the source,
 * the stage, the target environment, the entry point, the optimisation level and the compiler's own version -
 * and are validated on read, so a truncated or stale file is a cache miss rather than a broken shader.
 */
void setShaderCacheDirectory(const std::string& directory);

/** @return the current cache directory, or an empty string when the cache is off. */
[[nodiscard]] std::string shaderCacheDirectory();

/**
 * A caller-supplied string folded into every cache key; changing it invalidates every entry.
 *
 * Content changes need no salt - the source is part of the key, so a dynamically delivered shader with different
 * text is a miss and the old entry simply stops being requested. This is for the changes the source does not
 * describe: a new compiler build that still reports the same SPIR-V version, a different pipeline the engine
 * composes around the shader, or an application that wants to A/B two shader variants that happen to share their
 * text. Set it once at start-up, before compiling:
 *
 * ```cpp
 * backend::setShaderCacheSalt("pipeline-2024-11");
 * ```
 *
 * Also settable through `CORAL_SHADER_CACHE_SALT`. An empty salt is the default.
 */
void setShaderCacheSalt(const std::string& salt);

/** @return the cache salt in use. */
[[nodiscard]] std::string shaderCacheSalt();

/**
 * The disk budget for the cache, in bytes; 0 means unbounded. Default 8 MiB.
 *
 * The cache is per user and shared by every process, so it bounds itself: a hit refreshes the entry's
 * modification time, and when the directory outgrows this budget the least recently used files are deleted. The
 * pass runs on the first write of a process and then every 64 writes, because walking the directory costs more
 * than a compile. Also settable through `CORAL_SHADER_CACHE_BUDGET` (bytes).
 *
 * A budget smaller than a single entry leaves the cache permanently cold rather than over budget; entries are
 * 1-2 KB, so anything from a few tens of kilobytes upward is useful.
 */
void setShaderCacheBudget(uint64_t bytes);

/** @return the budget in bytes; 0 when unbounded. */
[[nodiscard]] uint64_t shaderCacheBudget() noexcept;

/** Entries currently on disk and their total size. Walks the directory, so treat it as a diagnostic. */
void shaderCacheUsage(uint64_t* outEntries, uint64_t* outBytes);

/** Accumulated hits and misses since the process started. Useful in a benchmark's output. */
[[nodiscard]] uint64_t shaderCacheHits() noexcept;
[[nodiscard]] uint64_t shaderCacheMisses() noexcept;

} // namespace backend
