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

#include <gpu/Buffer.h>

#include <backend/Device.h>
#include <gpu/Context.h>

#include "ContextState.h"

#include <cstring>

namespace graphics::gpu {

Buffer::Buffer(std::shared_ptr<ContextState> state, const backend::BufferHandle handle, const size_t size) noexcept
  : mState(std::move(state)),
    mHandle(handle),
    mSizeInBytes(size),
    mCreationComplete(handle.valid()) {
}

Buffer::~Buffer() {
  // A buffer whose state outlived the device - possible when an application destroys the device by
  // hand through `Context::device()` - must not touch the handle. The check is on the state rather
  // than on the handle because a valid handle into a dead pool is exactly the case this guards.
  if (mState == nullptr || mState->device == nullptr) return;
  if (!mHandle.valid()) return;
  mState->device->destroyBuffer(mHandle);
  mHandle = backend::BufferHandle::invalid();
}

Result Buffer::update(const void* data, const size_t size, const size_t offset) const {
  if (mState == nullptr || mState->device == nullptr) {
    return Result::DeviceLost;
  }
  if (!mHandle.valid()) {
    return Result::InvalidHandle;
  }
  if (data == nullptr || size == 0) {
    return Result::InvalidArgument;
  }
  if (offset + size > mSizeInBytes) {
    return Result::InvalidArgument;
  }

  // Through map/unmap rather than a recording command buffer: this is a host write into memory the
  // buffer was created host-visible for, so it needs no GPU work and no submission. A caller that
  // wanted the write ordered against a command stream would record it into one instead.
  void* destination = nullptr;
  const Result mapped = mState->device->mapBuffer(mHandle, offset, size, &destination);
  if (!backend::isSuccess(mapped)) {
    return mapped;
  }
  if (destination == nullptr) {
    // A device-local buffer. The facade creates those only when the caller supplied initial data, so
    // this means the caller asked to rewrite a buffer that was filled at creation.
    mState->device->unmapBuffer(mHandle);
    return Result::Unsupported;
  }

  std::memcpy(destination, data, size);
  mState->device->unmapBuffer(mHandle);
  return Result::Success;
}

VertexBuffer::VertexBuffer(std::shared_ptr<ContextState> state,
  const backend::BufferHandle handle,
  const size_t size,
  const size_t vertexCount,
  const size_t vertexStride) noexcept
  : Buffer(std::move(state), handle, size),
    mVertexCount(vertexCount),
    mVertexStride(vertexStride) {
}

IndexBuffer::IndexBuffer(std::shared_ptr<ContextState> state,
  const backend::BufferHandle handle,
  const size_t size,
  const size_t indexCount,
  const IndexFormat format) noexcept
  : Buffer(std::move(state), handle, size),
    mIndexCount(indexCount),
    mFormat(format) {
}

UniformBuffer::UniformBuffer(std::shared_ptr<ContextState> state,
  const backend::BufferHandle handle,
  const size_t size) noexcept
  : Buffer(std::move(state), handle, size) {
}

} // namespace graphics::gpu
