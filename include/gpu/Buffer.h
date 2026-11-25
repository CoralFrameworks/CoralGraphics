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

#ifndef CORAL_GPU_BUFFER_H
#define CORAL_GPU_BUFFER_H

#include <gpu/Types.h>

#include <backend/Handles.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace graphics::gpu {

class Context;

/**
 * The context's shared state, defined in Context.cpp. A resource holds one of these so that it keeps
 * the device alive without keeping the `Context` wrapper alive. Named here rather than nested inside
 * `Context` so that each resource header does not have to include the context header.
 */
struct ContextState;

/**
 * A block of GPU memory.
 *
 * Not constructed directly: `Context::createVertexBuffer` and friends return the subclass that
 * matches the intended use, because a buffer's usage is fixed when it is created, and a type that
 * could be anything would let a caller bind a uniform block as vertex data and only find out at
 * validation time.
 *
 * ### What is deliberately absent
 *
 * There is no `map()`. Mapping a GPU buffer is a backend-specific question - whether the memory is
 * host-visible, whether the range must be flushed, what happens when the GPU is already reading it -
 * and a facade that exposed one would either be wrong on some driver or hide a stall. `update()` is
 * the operation a caller actually wants, and it is defined as "put these bytes in that range" rather
 * than in terms of memory.
 */
class Buffer {
public:
  virtual ~Buffer();

  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;

  /** Size of the buffer in bytes. */
  [[nodiscard]] size_t getSizeInBytes() const noexcept { return mSizeInBytes; }

  /**
   * True once the resource exists on the GPU.
   *
   * Always true today: the RHI creates buffers synchronously and this facade adds no background
   * upload path. It is in the interface because an asynchronous creation path would make it false, and
   * a caller that assumed otherwise would be reading a half-built buffer.
   */
  [[nodiscard]] bool isCreationComplete() const noexcept { return mCreationComplete; }

  /**
   * The value that names this buffer in the lower level API.
   *
   * Reachable so a caller can bind the buffer through `context.device()` without waiting for a
   * wrapper to exist for every operation. It is a small generation-checked value, not a driver
   * pointer: using it after this object is destroyed fails as `Result::InvalidHandle` rather than as
   * undefined behaviour.
   */
  [[nodiscard]] backend::BufferHandle handle() const noexcept { return mHandle; }

  /**
   * Replaces @p size bytes at @p offset.
   *
   * The buffer must have been created for host updates. A vertex or index buffer created *with*
   * initial data is device-local and rejects this, because a buffer written once during creation is
   * cheaper to leave where only the GPU can reach it; create it without data if it must be rewritten.
   *
   * @return `Result::Success`, or a specific failure. `Result::InvalidHandle` means the buffer is
   *         gone; anything else came from the driver.
   */
  Result update(const void* data, size_t size, size_t offset = 0) const;

protected:
  /**
   * @param state  the context's shared state, kept alive for as long as this buffer.
   * @param handle backend handle.
   * @param size   size in bytes.
   */
  Buffer(std::shared_ptr<ContextState> state, backend::BufferHandle handle, size_t size) noexcept;

  std::shared_ptr<ContextState> mState;
  backend::BufferHandle mHandle = backend::BufferHandle::invalid();
  size_t mSizeInBytes = 0;
  bool mCreationComplete = false;
};

/**
 * A vertex buffer: the geometry of a renderable.
 *
 * A renderable's geometry is a set of vertex attributes - position, colour, normal, tangent - and
 * there is no requirement that they live in separate buffers. One buffer can carry several attributes
 * interleaved, which is normally the faster layout and is why `Context::createVertexBuffer` takes a
 * stride rather than an attribute list.
 *
 * Buffers are GPU resources, so mutating one costs more than mutating client memory. Keep constant
 * data and per-frame data in separate buffers, and share a buffer between several renderables where
 * the geometry allows it.
 */
class VertexBuffer final : public Buffer {
public:
  /** Number of vertices. */
  [[nodiscard]] size_t getCount() const noexcept { return mVertexCount; }

  /** Bytes per vertex, i.e. the size of one client vertex struct. */
  [[nodiscard]] size_t getStride() const noexcept { return mVertexStride; }

private:
  friend class Context;

  /**
   * The context's shared state, defined in Context.cpp. A resource holds one of these so that it keeps
   * the device alive without keeping the `Context` wrapper alive. Named here rather than nested inside
   * `Context` so that each resource header does not have to include the context header.
   */

  VertexBuffer(std::shared_ptr<ContextState> state, backend::BufferHandle handle, size_t size, size_t vertexCount,
    size_t vertexStride) noexcept;

  size_t mVertexCount = 0;
  size_t mVertexStride = 0;
};

/**
 * A buffer of indices into a vertex buffer.
 *
 * Indices may be 16 or 32 bit. The buffer is a GPU resource, so mutating it is comparatively slow;
 * index buffers are almost always written once. Sharing one between several renderables is normal.
 */
class IndexBuffer final : public Buffer {
public:
  /** Number of indices, not bytes. */
  [[nodiscard]] size_t getCount() const noexcept { return mIndexCount; }

  [[nodiscard]] IndexFormat getFormat() const noexcept { return mFormat; }

private:
  friend class Context;

  /**
   * The context's shared state, defined in Context.cpp. A resource holds one of these so that it keeps
   * the device alive without keeping the `Context` wrapper alive. Named here rather than nested inside
   * `Context` so that each resource header does not have to include the context header.
   */

  IndexBuffer(std::shared_ptr<ContextState> state, backend::BufferHandle handle, size_t size, size_t indexCount,
    IndexFormat format) noexcept;

  size_t mIndexCount = 0;
  IndexFormat mFormat = IndexFormat::UInt32;
};

/**
 * A small block of constants, bound to a descriptor set and rewritten as often as the renderer needs.
 *
 * Host-visible by construction: the point of asking for a uniform buffer through the facade is to be
 * able to update it, so it is created with the memory property that allows that.
 */
class UniformBuffer final : public Buffer {
public:
  /**
   * Uploads @p value as the whole block.
   *
   * Convenience for the common case, where the caller's struct *is* the block.
   */
  template<typename T>
  Result set(const T& value) {
    return update(&value, sizeof(T), 0);
  }

private:
  friend class Context;

  /**
   * The context's shared state, defined in Context.cpp. A resource holds one of these so that it keeps
   * the device alive without keeping the `Context` wrapper alive. Named here rather than nested inside
   * `Context` so that each resource header does not have to include the context header.
   */

  UniformBuffer(std::shared_ptr<ContextState> state, backend::BufferHandle handle, size_t size) noexcept;
};

} // namespace graphics::gpu

#endif // CORAL_GPU_BUFFER_H
