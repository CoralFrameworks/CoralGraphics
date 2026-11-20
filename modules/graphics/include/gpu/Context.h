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

#ifndef CORAL_GPU_CONTEXT_H
#define CORAL_GPU_CONTEXT_H

#include <backend/Types.h>
#include <gpu/Shader.h>
#include <gpu/Types.h>

#include <cstdint>
#include <memory>
#include <string>

namespace backend {
class Device;
} // namespace backend

namespace graphics::gpu {

class VertexBuffer;
class IndexBuffer;
class UniformBuffer;

/**
 * The context's shared state.
 *
 * Defined in the implementation file and reachable only by value-less `shared_ptr`, which is what keeps
 * a client from naming it. Resources hold it so that dropping the last `Context` handle while a buffer
 * is still alive keeps the device running.
 */
struct ContextState;

/**
 * What a client must supply to bring up a device.
 *
 * The defaults are the ones a game wants: the platform's best backend, the platform's OpenGL dialect,
 * and validation off. Everything a caller would otherwise have to know about backend selection is a
 * field here, so that `Context::create()` with no arguments is a reasonable thing to write.
 */
struct ContextConfiguration {
  /** Ask for one backend specifically. `Undefined` lets the platform decide, which is the default. */
  enum class Preferred {
    /** Whatever the platform does best: Metal on Apple, Vulkan then OpenGL elsewhere. */
    Undefined,
    OpenGL,
    Vulkan,
    Metal,
  };

  Preferred preferred = Preferred::Undefined;

  /** Allow the factory to move on when the preferred backend cannot be initialised. */
  bool allowFallback = true;

  /** Turn on driver validation and debug output where the backend supports it. */
  bool enableValidation = false;

  /** Turn on GPU timing instrumentation where the backend supports it. */
  bool enableGpuTiming = false;

  /** Application name, reported to the driver. */
  const char* applicationName = "Coral";

  /**
   * Present to a native window: an `HWND`, an `ANativeWindow*`, a Linux `Window`, or the layer a
   * Metal context draws into, depending on the platform. When null the context is headless - rendering
   * goes to offscreen targets and presentation reports `Result::Unsupported` - which is what tests,
   * tools and offline video processing want.
   */
  void* nativeWindow = nullptr;

  /** Native display the window belongs to, when the platform separates the two. */
  void* nativeDisplay = nullptr;

  /**
   * Wayland's EGL handle (`wl_egl_window*`) when @c nativeWindow is the `wl_surface*` Vulkan needs.
   * Null on every other platform.
   */
  void* nativeWindowExtra = nullptr;

  /**
   * Which native window system @c nativeWindow belongs to.
   *
   * Linux uses this to choose GLX (`X11`) or EGL (`Wayland`); `Unknown` keeps the X11/GLX path.
   */
  backend::NativeWindowSystem nativeWindowSystem = backend::NativeWindowSystem::Unknown;

  /** Drawable size in pixels. Required for a headless context; queried from the window otherwise. */
  uint32_t width = 0;
  uint32_t height = 0;
};

/**
 * Owns the graphics device and hands out every GPU resource.
 *
 * This is the only object a client must create. Every buffer, shader and pipeline belongs to a
 * context, so there is exactly one place to look when asking what a resource is attached to.
 *
 * ### Lifetime
 *
 * Resources returned by the `create*` methods hold a reference to the context's *state*, not to the
 * `Context` object. Dropping the last `Context` handle while a vertex buffer is still alive therefore
 * does not free the device: the buffer keeps it running and the device is released once, after the
 * last resource lets go. That is deliberately more forgiving than the raw backend, where a stale
 * handle is simply refused.
 *
 * What a client must still do is call `waitIdle()` before letting the last resource go while GPU work
 * is in flight; destroying a device does not wait for the driver on its own.
 *
 * ### Threading
 *
 * A context is not thread-safe. Resource creation is serialised on the device, matching the backend's
 * external-synchronisation contract.
 */
class Context final {
public:
  Context() noexcept = default;
  ~Context();

  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  /**
   * Brings up a device.
   *
   * @param configuration what to create. An empty one means "the platform's best, headless".
   * @return the context, or null on failure. The reason is reported through the engine log, and the
   *         log is also where a rejected backend explains itself.
   */
  static std::shared_ptr<Context> create(const ContextConfiguration& configuration = {});

  /** @return true when a device is live. */
  [[nodiscard]] bool valid() const noexcept;

  /**
   * @return the device this context owns, for the caller that needs the raw RHI.
   *
   * The facade is a convenience layer, not a cage: a renderer that needs a descriptor set, a compute
   * pipeline or an explicit barrier reaches through here rather than waiting for a wrapper to exist.
   * Everything reachable this way is a documented part of the installed headers.
   */
  [[nodiscard]] backend::Device* device() noexcept;
  [[nodiscard]] const backend::Device* device() const noexcept;

  /** Vendor, device and API version as reported by the driver, one line. */
  [[nodiscard]] std::string describeDevice() const;

  /** A multi-line report of the device, its capabilities and its live resource counts. */
  [[nodiscard]] std::string diagnostics() const;

  // ------------------------------------------------------------------------------------------------
  // Capability queries
  // ------------------------------------------------------------------------------------------------
  //
  // The questions a renderer actually branches on. The full set, including the recorded reason each
  // missing feature is missing, stays available through `device()->getCapabilities()`.

  [[nodiscard]] bool supportsCompute() const noexcept;
  [[nodiscard]] bool supportsGeometryShaders() const noexcept;
  [[nodiscard]] bool supportsWireframe() const noexcept;
  [[nodiscard]] bool supportsAnisotropicFiltering() const noexcept;

  /**
   * @return true when this device hands GLSL source to its driver as written.
   *
   * The question to ask before setting `ShaderSource::directGlsl`, and the one a shader-package loader
   * asks once at startup: a package of GLSL for a GL target works here and nowhere else, and a caller
   * that finds `false` has to take the SPIR-V path - or leave the effect out - rather than discovering
   * it at the first shader that fails to load.
   *
   * True on OpenGL and OpenGL ES, false on every other backend.
   */
  [[nodiscard]] bool supportsDirectGlsl() const noexcept;

  // ------------------------------------------------------------------------------------------------
  // Buffers
  // ------------------------------------------------------------------------------------------------

  /**
   * A vertex buffer holding @p vertexCount vertices of @p vertexStride bytes each.
   *
   * @param vertexCount  number of vertices. `getCount()` reports this back.
   * @param vertexStride bytes per vertex, i.e. `sizeof` the client's vertex struct.
   * @param data         optional initial contents, `vertexCount * vertexStride` bytes. Copied during
   *                     creation, so the caller's memory is free immediately afterwards. Passing null
   *                     creates the buffer host-visible so it can be filled later; passing data
   *                     creates it device-local, which is cheaper to read and cannot be rewritten.
   * @param label        optional debug name, shown in a GPU capture.
   */
  std::shared_ptr<VertexBuffer>
  createVertexBuffer(size_t vertexCount, size_t vertexStride, const void* data = nullptr, const char* label = nullptr);

  /**
   * An index buffer holding @p indexCount indices of @p format.
   *
   * @param indexCount number of indices. `getCount()` reports this back, not the byte size.
   * @param format     `IndexFormat::UInt16` or `UInt32`.
   * @param data       optional initial contents, `indexCount * sizeof(index)` bytes.
   * @param label      The label of the IndexBuffer
   */
  std::shared_ptr<IndexBuffer>
  createIndexBuffer(size_t indexCount, IndexFormat format = IndexFormat::UInt32, const void* data = nullptr,
    const char* label = nullptr);

  /**
   * A uniform buffer of @p size bytes, host-visible so it can be rewritten every frame.
   *
   * The facade's answer to a small per-draw block. It is CPU-writable by construction, because a
   * uniform buffer that cannot be updated is not useful to the caller that asked for one.
   */
  std::shared_ptr<UniformBuffer>
  createUniformBuffer(size_t size, const char* label = nullptr);

  // ------------------------------------------------------------------------------------------------
  // Shaders
  // ------------------------------------------------------------------------------------------------

  /**
   * Compiles a shader from SPIR-V.
   *
   * SPIR-V is the RHI's canonical input: the same words work on every backend, and the reflection that
   * falls out of them is what lets a descriptor set be updated by a binding that the shader declares
   * rather than by one a caller restated.
   *
   * @param code      SPIR-V words.
   * @param wordCount number of `uint32_t` words.
   * @param stage     which stage this module is for.
   * @return the shader, or null when translation or compilation failed. The compiler log is reported
   *         through the engine log, and it includes the generated GLSL.
   */
  std::shared_ptr<Shader> createShader(const uint32_t* code, size_t wordCount, backend::ShaderStage stage);

  /**
   * Compiles a shader written in GLSL.
   *
   * By default the source is compiled to SPIR-V first and then follows exactly the same path as a module
   * supplied as SPIR-V, so it gets real reflection and real dialect translation rather than being handed
   * to the driver raw. The author's GLSL is kept on the resulting `Shader` in full, which is the reason
   * to author in GLSL at all: a shader that misbehaves can be read back byte for byte.
   *
   * Requires a build with shaderc (`-DCORAL_BACKEND_SUPPORTS_SHADERC=ON`, the default). Without it the
   * call reports `Result::Unsupported` through the log and returns null.
   *
   * With `ShaderSource::directGlsl` set the source is not compiled to SPIR-V at all: on a device that
   * reports `supportsDirectGlsl()` it is handed to the driver as written, and the bindings the caller
   * declared in `ShaderSource::bindings` and `ShaderSource::attributes` are what the resulting shader
   * reports and what the OpenGL backend resolves against the linked program. On a device that does not
   * accept it, the call fails with `Result::Unsupported` rather than falling back silently - a caller
   * that wanted the portable path should not have asked for this one, and a caller that has GLSL only a
   * GL driver can compile has nowhere to fall back to.
   *
   * @param source the GLSL and what to compile it as. See `ShaderSource` for the version constraint
   *               that applies to the SPIR-V path, and for what a direct module has to supply instead.
   * @return the shader, or null on failure. The diagnostics are reported to the device's message
   *         callback and name the offending line of the source that was passed in.
   */
  std::shared_ptr<Shader> createShader(const ShaderSource& source);

  // ------------------------------------------------------------------------------------------------
  // Frame lifecycle
  // ------------------------------------------------------------------------------------------------

  /**
   * Waits for every submitted command buffer to finish.
   *
   * The one call a client must not forget before destroying resources or letting the last handle go
   * while work is in flight. On OpenGL it is nearly free because the driver already serialises; on
   * Vulkan it is a real wait.
   */
  [[nodiscard]] Result waitIdle() const;

private:
  /** @return the state, for passing to a resource being created. */
  [[nodiscard]] std::shared_ptr<ContextState> state() const noexcept { return mState; }

  std::shared_ptr<ContextState> mState;

  friend class VertexBuffer;
  friend class IndexBuffer;
  friend class UniformBuffer;
  friend class Shader;
  friend class Buffer;
};

} // namespace graphics::gpu


#endif // CORAL_GPU_CONTEXT_H
