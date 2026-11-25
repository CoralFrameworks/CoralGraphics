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

#ifndef CORAL_GPU_SHADER_H
#define CORAL_GPU_SHADER_H

#include <gpu/Types.h>

#include <backend/Descriptors.h>
#include <backend/Handles.h>
#include <backend/Types.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace graphics::gpu {

class Context;

/** The context's shared state; see gpu/Buffer.h. */
struct ContextState;

/**
 * Which shading language a source module is written in.
 *
 * Re-exported from the backend so that a caller writing a shader never has to include a second header
 * just to name the language it is written in.
 */
using ShaderLanguage = backend::ShaderSourceLanguage;

/** Human readable name of @p language, never null. */
[[nodiscard]] const char* toString(ShaderLanguage language) noexcept;

/**
 * @return true when this build can compile GLSL source to SPIR-V.
 *
 * False only when Coral was configured without shaderc. A build that returns false still consumes
 * SPIR-V, so a caller can decide between compiling offline and authoring in GLSL by asking here.
 */
[[nodiscard]] bool isShaderCompilerAvailable() noexcept;

/**
 * @return the GLSL compiler's version string, or `"unavailable"`.
 *
 * Worth logging at startup: it is what a shader cache key is derived from, and the first thing to check
 * when a shader that used to compile stops.
 */
[[nodiscard]] const char* getShaderCompilerVersion() noexcept;

/**
 * One name/location relation for a shader whose bindings cannot be recovered by reflection.
 *
 * ### Why a caller ever has to write one of these
 *
 * The facade's normal answer to "where is this binding" is reflection: a shader compiled to SPIR-V
 * carries its `set` and `binding` decorations, and `Shader::getBindings()` hands them back. A shader
 * handed to the driver as GLSL (`ShaderSource::directGlsl`) has no such thing - the compile happens
 * inside the driver, and the only description of the module is the one the caller brought with it.
 *
 * That description normally comes from the same place the shader did. A shader package delivered as
 * JSON carries the pairs the packager knew:
 *
 * ```json
 * { "shaders": [ { "stage": "fragment", "file": "tint.frag",
 *                  "bindings":   [ { "name": "uTexture", "set": 0, "binding": 0,
 *                                    "type": "textureSampler", "location": 0 } ],
 *                  "attributes": [ { "name": "aPosition", "location": 0 } ] } ] }
 * ```
 *
 * `ShaderModuleDescriptor::bindingDeclarations` is the RHI-level record of the same table, and
 * `ShaderBindingDeclaration` documents what a location means for each binding type. Reading the JSON
 * is the caller's business: the facade takes no dependency on a JSON library, so that a build which
 * never loads a manifest does not pay for one.
 */
struct ShaderBindingMapping {
  /** The uniform's name, exactly as the GLSL source spells it. */
  std::string name;

  /** Descriptor set the binding belongs to. Only set 0 exists on OpenGL. */
  uint32_t set = 0;
  /** Descriptor binding number, which is how the renderer addresses this binding. */
  uint32_t binding = 0;

  /** What the binding points at. Decides how the location below is interpreted. */
  backend::BindingType type = backend::BindingType::TextureSampler;

  /** Number of array elements, or 1 for a non-array binding. */
  uint32_t arraySize = 1;

  /**
   * The slot this binding must occupy in the linked program, or `backend::kNoShaderLocation` to let
   * the backend choose.
   *
   * A texture unit for a sampler, a uniform block binding for a block. See
   * `backend::ShaderBindingDeclaration::location`.
   */
  int32_t location = backend::kNoShaderLocation;
};

/**
 * One vertex input and the location it must occupy, for a shader that cannot declare one itself.
 *
 * The vertex-side half of `ShaderBindingMapping`, and the pair `glBindAttribLocation` needs: a
 * location is only half of an attribute's identity, the other half being the name the vertex shader
 * declares it under. `<gpu/Shader.h>` says where such a table comes from.
 */
struct ShaderAttributeMapping {
  /** The vertex input's name, exactly as the source spells it: `aPosition`, not `in aPosition`. */
  std::string name;
  /** The location the pipeline's `VertexAttribute::shaderLocation` will name. */
  uint32_t location = 0;
};

/**
 * A shader written in GLSL, ready to be compiled.
 *
 * The source is kept and handed back by `Shader::getSource()` after compilation, which is the point of
 * authoring in GLSL in the first place: when a shader misbehaves, the first question is always whether
 * the code that ran is the code that was written, and a module that discards its input cannot answer
 * it.
 *
 * ### Two ways to be GLSL, and how to choose
 *
 *  - **Through SPIR-V** (the default). The source is compiled to SPIR-V and follows the same path as
 *    any other module: real reflection, dialect translation between ES and desktop, and the same module
 *    on every backend. It requires a build with shaderc, and it cannot compile GLSL that glslang will
 *    not emit SPIR-V from - ESSL 1.00 that uses `attribute`, `varying`, `texture2D` or `gl_FragColor`,
 *    for instance.
 *  - **Straight to the driver** (`directGlsl`). On an OpenGL device the source is handed to
 *    `glShaderSource` as written. Nothing is translated, nothing is reflected, and neither shaderc nor
 *    SPIRV-Cross has to be present. The price is that the source has to be written for the context's
 *    own dialect, and that the bindings have to be described by the caller - see
 *    `ShaderBindingMapping`. `Context::supportsDirectGlsl()` says whether this device can take it.
 *
 * ### Versions
 *
 * `language` is normally left as `Detect`: the source's own `#version` directive says which language
 * it is, and a source without one is ESSL 1.00 by the specification's rule. Set it explicitly for a
 * snippet that cannot say. On the direct path it only labels the module - the driver decides.
 *
 * ### A constraint worth knowing before you hit it
 *
 * For the SPIR-V path, glslang will not emit SPIR-V from an old shader. The floor is GLSL ES 3.10 and
 * desktop GLSL 3.30, so **ESSL 1.00 and ESSL 3.00 source cannot become SPIR-V as written**. Coral
 * retargets such a source to 310 by rewriting only its `#version` line. That is correct for ESSL 3.00,
 * which is a strict subset of 3.10, and correct for ESSL 1.00 exactly when the shader avoids the
 * keywords ES 3.00 removed - `attribute`, `varying`, `texture2D`, `gl_FragColor`. When it does not,
 * compilation fails with the compiler's own diagnostics plus a note naming the offending keyword.
 * `directGlsl` is the way past that floor on an OpenGL device, because the driver has no such floor.
 *
 * `Shader::wasRetargeted()` reports whether a retarget happened, so it is visible rather than inferred.
 */
struct ShaderSource {
  /** The GLSL text. Required. */
  std::string source;

  /** Which stage to compile for. Required. */
  backend::ShaderStage stage = backend::ShaderStage::Vertex;

  /** Which language profile to compile as. `Detect` reads the source's `#version`. */
  ShaderLanguage language = ShaderLanguage::Detect;

  /** Entry point name. Empty means `main`. */
  const char* entryPoint = "main";

  /**
   * Run the SPIR-V through the optimiser.
   *
   * Off by default, deliberately. The reason to feed this path GLSL rather than SPIR-V is that a
   * debugger should be able to map the module back to what was written, and spirv-opt is precisely the
   * pass that destroys that correspondence.
   *
   * Ignored on the direct path, which produces no SPIR-V to optimise.
   */
  bool optimize = false;

  /**
   * Hand this source to the OpenGL driver as written, instead of compiling it to SPIR-V first.
   *
   * False by default, because the SPIR-V path is the one that works on every backend and needs nothing
   * of the caller. Set it for a shader that is already written for the target context - which is what a
   * shader package built for a GL or GLES target contains, and the only way to run ESSL 1.00 source
   * that glslang refuses.
   *
   * On a device that does not accept it the creation fails and reports `Result::Unsupported` through
   * the device's message callback; `Context::supportsDirectGlsl()` answers the question up front.
   * `Shader::isDirectGlsl()` reports afterwards which path a module actually took.
   */
  bool directGlsl = false;

  /**
   * The bindings this shader declares, for a direct module.
   *
   * Required information rather than a hint: there is no SPIR-V here to reflect, so this table is what
   * `Shader::getBindings()` reports and what the renderer builds its descriptor sets from. Left empty,
   * the module is understood to declare no bindings at all.
   *
   * Ignored on the SPIR-V path, whose bindings come from the module itself.
   */
  std::vector<ShaderBindingMapping> bindings;

  /**
   * The vertex inputs this shader declares and where each one lives, for a direct module.
   *
   * Only needed when the vertex source does not carry `layout(location = ...)` - which is every
   * ESSL 1.00 source, and any ES 3.00 source whose author left them out. Without either, the location
   * a shader input lands at is whatever the driver picks, and the pipeline's
   * `VertexAttribute::shaderLocation` would have nothing to agree with.
   */
  std::vector<ShaderAttributeMapping> attributes;

  /** Debug name, used in compiler diagnostics. */
  const char* label = nullptr;

  /** Convenience: a vertex shader in whatever language the source declares. */
  static ShaderSource vertex(std::string text, const char* label = nullptr);

  /** Convenience: a fragment shader in whatever language the source declares. */
  static ShaderSource fragment(std::string text, const char* label = nullptr);

  /** Convenience: a vertex shader handed to the driver as written. `directGlsl` set. */
  static ShaderSource directVertex(std::string text, const char* label = nullptr);

  /** Convenience: a fragment shader handed to the driver as written. `directGlsl` set. */
  static ShaderSource directFragment(std::string text, const char* label = nullptr);

  /**
   * Reads a GLSL file and returns it as a source for @p stage, ready to hand to the driver.
   *
   * How a caller loads a shader out of a package: the file on disk is the shader, with no offline
   * compilation step in between. The result has `directGlsl` set, because that is the only path a file
   * read at run time can take without a compiler - a caller that wants the SPIR-V path reads the text
   * itself and leaves the flag alone.
   *
   * A UTF-8 byte order mark is removed. Drivers reject it in front of `#version` rather than skipping
   * it, and a file saved by an editor that adds one otherwise fails with a diagnostic that points at
   * the first line of a file that looks correct. What is returned is therefore the file's text as the
   * *driver* should see it; a caller that wants the bytes on disk can read them itself.
   *
   * @param path     file to read.
   * @param stage    which stage the file holds.
   * @param outError optional; receives the reason when the file cannot be read.
   * @return the source, or one with an empty `source` when the file could not be read. An empty source
   *         is refused by `Context::createShader`, so a caller that ignores @p outError still fails
   *         safe - it just cannot say why.
   */
  static ShaderSource fromFile(const std::string& path, backend::ShaderStage stage, std::string* outError = nullptr);

  /**
   * Reads a GLSL file, taking the stage from its extension.
   *
   * Recognises `.vert`, `.frag`, `.comp`, `.geom`, `.tesc` and `.tese`, the six names the GLSL
   * specification's own convention uses. An unrecognised extension is reported through @p outError
   * rather than guessed at, because a vertex shader that silently became a fragment shader fails at
   * link time with a message about a missing `main` in a stage nobody asked about.
   */
  static ShaderSource fromFile(const std::string& path, std::string* outError = nullptr);
};

/**
 * A compiled shader stage: the GLSL that was written, and - on the usual path - the SPIR-V it became.
 *
 * Created through `Context::createShader`. Both forms stay reachable afterwards:
 *
 *  - `getSource()` is the author's GLSL, byte for byte, for a module compiled from source. A debugger,
 *    a hot-reload path or an error report can show it.
 *  - `getSpirv()` is the SPIR-V the module compiled to, for a caller that wants to cache it.
 *
 * Keeping both together is the difference between "the shader runs" and "I can find out why it does
 * not".
 *
 * A module created from a `ShaderSource` with `directGlsl` set has the first and not the second: the
 * driver compiled the text and no SPIR-V ever existed. `isDirectGlsl()` says so, and the binding table
 * such a module reports is the one that came in with the source rather than one recovered from a
 * compiled module.
 *
 * ### Reflection
 *
 * The descriptor bindings the shader declares are recovered from the compiled SPIR-V and reported by
 * `getBindings()`, so a renderer builds its descriptor set layout from what the shader says rather than
 * from a hand-written table that can disagree with it. On the direct path there is nothing to recover
 * and the caller's table is reported instead - the same table the module was created with, which is
 * what keeps one description of a shader's interface rather than two.
 */
class Shader final {
public:
  ~Shader();

  Shader(const Shader&) = delete;
  Shader& operator=(const Shader&) = delete;

  /** Which stage this module was compiled for. */
  [[nodiscard]] backend::ShaderStage getStage() const noexcept { return mStage; }

  /**
   * The value that names this shader in the lower level API.
   *
   * Exposed for the caller that links pipelines through `context.device()`. Like every handle in the
   * facade it is generation-checked, so a stale value is refused rather than dereferenced.
   */
  [[nodiscard]] backend::ShaderModuleHandle handle() const noexcept { return mHandle; }

  // ------------------------------------------------------------------------------------------------
  // Source and version
  // ------------------------------------------------------------------------------------------------

  /** True when this module was compiled from GLSL rather than supplied as SPIR-V. */
  [[nodiscard]] bool hasSource() const noexcept { return !mSource.empty(); }

  /** The GLSL this shader was compiled from, or empty for a SPIR-V module. */
  [[nodiscard]] const std::string& getSource() const noexcept { return mSource; }

  /**
   * True when the driver compiled `getSource()` as written, with no SPIR-V in between.
   *
   * The distinction a caller checks before expecting any of the SPIR-V-shaped answers below:
   * `hasCompiledSpirv()` is false and `getCacheKey()` is invalid for such a module, because there are
   * no words to keep and none to cache. What remains is exactly the author's text and the binding table
   * the caller supplied with it - which is the point of the path, and why a shader from a package is
   * still fully described after it has been handed over.
   */
  [[nodiscard]] bool isDirectGlsl() const noexcept { return mInfo.directGlsl; }

  /** The language the source was written in. Meaningless when `hasSource()` is false. */
  [[nodiscard]] ShaderLanguage getLanguage() const noexcept { return mInfo.language; }

  /**
   * The `#version` the source declared, or 100 when it declared none.
   *
   * What the author wrote, not what the compiler used. See `getCompiledVersion()`.
   */
  [[nodiscard]] uint32_t getSourceVersion() const noexcept { return mInfo.version; }

  /** True when the source carried a `#version` directive rather than defaulting to ESSL 1.00. */
  [[nodiscard]] bool hadVersionDirective() const noexcept { return mInfo.versionWasDeclared; }

  /**
   * The `#version` the module was compiled at.
   *
   * Differs from `getSourceVersion()` when the source was below the compiler's SPIR-V floor, which is
   * the case for every ESSL 1.00 and ESSL 3.00 shader. Zero for a module supplied as SPIR-V, since
   * nothing was compiled here.
   */
  [[nodiscard]] uint32_t getCompiledVersion() const noexcept { return mInfo.compiledVersion; }

  /** True when the source was retargeted to a newer version in order to compile it. */
  [[nodiscard]] bool wasRetargeted() const noexcept { return mInfo.versionWasRetargeted; }

  /** Whether the SPIR-V was optimised. Part of what the cache key covers. */
  [[nodiscard]] bool wasOptimized() const noexcept { return mInfo.optimized; }

  /**
   * A stable hash of the source text.
   *
   * The cheap answer to "has this shader changed", for a reload path or a build step. Zero for a
   * SPIR-V module.
   */
  [[nodiscard]] uint64_t getSourceHash() const noexcept { return mInfo.sourceHash; }

  /**
   * A one-line description of the source, for logs.
   *
   * Example: `ESSL 3.00 (#version 300 es) compiled at 310`, or `SPIR-V (no source)`.
   */
  [[nodiscard]] std::string describeSource() const;

  // ------------------------------------------------------------------------------------------------
  // SPIR-V and caching
  // ------------------------------------------------------------------------------------------------

  /**
   * The SPIR-V this shader compiled to.
   *
   * Empty for a module supplied *as* SPIR-V: the backend consumes those words directly and does not
   * copy them, rather than duplicating a buffer that can be hundreds of kilobytes per shader. A caller
   * that needs the words of a SPIR-V module already has them.
   */
  [[nodiscard]] const std::vector<uint32_t>& getSpirv() const noexcept { return mSpirv; }

  /** True when `getSpirv()` holds the words this module compiled to. */
  [[nodiscard]] bool hasCompiledSpirv() const noexcept { return !mSpirv.empty(); }

  /**
   * The key to cache this shader's SPIR-V under.
   *
   * Covers everything that changes the compiled result: the source text, the stage, the target dialect
   * and the compiler that produced it. Two builds that agree on this key may share a cache entry; two
   * that disagree must not, which is why it is not simply the source hash.
   *
   * `valid()` is false for a module supplied as SPIR-V, because there is nothing to cache - the caller
   * brought the words with it. It is also false for a direct module (`isDirectGlsl()`), for the same
   * reason in the other direction: the driver compiles that source every time, and there is no compiled
   * form for a cache to hold.
   */
  [[nodiscard]] const backend::ShaderCacheKey& getCacheKey() const noexcept { return mCacheKey; }

  // ------------------------------------------------------------------------------------------------
  // Reflection
  // ------------------------------------------------------------------------------------------------

  /**
   * One resource the shader declares.
   *
   * `set` and `binding` are the numbers the shader's `layout` qualifiers carry; `name` is the
   * identifier the backend resolved against the linked program. Both are needed: the numbers build the
   * descriptor set layout, the name is what the driver's uniform lookup accepted.
   *
   * A direct module has no `layout` qualifiers to read, so its entries come from the table the caller
   * supplied with the source (`ShaderSource::bindings`) and `location` is the slot that table asked
   * for, if it asked for one.
   */
  struct Binding {
    uint32_t set = 0;
    uint32_t binding = 0;
    /** Number of array elements, or 1 for a non-array binding. */
    uint32_t arraySize = 1;
    /** Reflected name, empty when the backend could not recover one. */
    std::string name;
    /**
     * The GL-side slot the caller asked this binding to occupy, or `backend::kNoShaderLocation`.
     *
     * Never set by reflection: SPIR-V has no such decoration. It is carried through so that a renderer
     * that built a descriptor set layout from a manifest can see what the shader was created with,
     * which is what makes the two halves of a package debuggable against each other.
     */
    int32_t location = backend::kNoShaderLocation;
  };

  /** Every resource the shader declares, in the order the reflection produced them. */
  [[nodiscard]] const std::vector<Binding>& getBindings() const noexcept { return mBindings; }

  /**
   * @return the binding declared at @p set and @p binding, or null when the shader has none.
   *
   * The lookup a renderer does when wiring a material: it knows which slot it wants and needs the name
   * the driver will accept for it.
   */
  [[nodiscard]] const Binding* findBinding(uint32_t set, uint32_t binding) const noexcept;

private:
  friend class Context;

  Shader(std::shared_ptr<ContextState> state, backend::ShaderModuleHandle handle, backend::ShaderStage stage,
    std::string source, std::vector<uint32_t> spirv, backend::ShaderSourceInfo info,
    backend::ShaderCacheKey cacheKey, std::vector<Binding> bindings) noexcept;

  std::shared_ptr<ContextState> mState;
  backend::ShaderModuleHandle mHandle = backend::ShaderModuleHandle::invalid();
  backend::ShaderStage mStage = backend::ShaderStage::Vertex;

  /** The author's GLSL. Empty for a module supplied as SPIR-V. */
  std::string mSource;
  /** The SPIR-V this module compiled to. Empty for a module supplied as SPIR-V. */
  std::vector<uint32_t> mSpirv;
  backend::ShaderSourceInfo mInfo;
  backend::ShaderCacheKey mCacheKey;

  std::vector<Binding> mBindings;
};

} // namespace graphics::gpu

#endif // CORAL_GPU_SHADER_H
