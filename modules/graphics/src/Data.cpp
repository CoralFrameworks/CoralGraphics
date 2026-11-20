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
 * implied. See the License for the specific language governing permissions and limitations under the
 * License.
 */

#include <graphics/Data.h>

#include <algorithm>
#include <cstring>

namespace graphics {
namespace {

/**
 * A counted allocation, so that a `shared_ptr` can own the bytes.
 *
 * The allocation is a `unique_ptr<uint8_t[]>`, which owns it and zeroes it, so there is no `delete[]` to write and no
 * destructor to get wrong. A `shared_ptr` cannot hold an array `unique_ptr` before C++20, which is why the array is owned
 * by this small object and the object is what gets shared -- but the *member* being a `unique_ptr` is available on every
 * standard this project targets, and the earlier note here conflated the two.
 */
struct Block {
  std::unique_ptr<uint8_t[]> data;

  // No destructor and no deleted copies: `unique_ptr` supplies both, and writing them out by hand is where a class like
  // this goes wrong.
};

/** @return a block owning @p size zeroed bytes. */
std::shared_ptr<Block> allocateBlock(const size_t size) {
  auto block = std::make_shared<Block>();
  // Zeroed rather than left indeterminate: a caller that fills only part of the buffer then reads it back
  // gets defined bytes instead of whatever was in the heap, which is the difference between a bug and a
  // security report. `make_unique` for an array value-initialises, which for `uint8_t` is zero.
  block->data = std::make_unique<uint8_t[]>(size);
  return block;
}

/** Binds a block's lifetime to a pointer into it, so both are released together. */
std::shared_ptr<const void> shareBlock(const std::shared_ptr<Block>& block) {
  return std::shared_ptr<const void>(block, block->data.get());
}

} // namespace anonymous

Data::~Data() = default;

Data Data::makeCopy(const void* data, const size_t size) {
  if (data == nullptr || size == 0) return {};

  const auto block = allocateBlock(size);
  std::memcpy(block->data.get(), data, size);

  Data result;
  result.mBytes = block->data.get();
  result.mSize = size;
  // The block is held rather than the raw pointer, so the allocation is released exactly once, when the
  // last `Data` referring to it goes.
  result.mOwner = shareBlock(block);
  return result;
}

Data Data::makeUninitialised(const size_t size) {
  if (size == 0) return {};

  const auto block = allocateBlock(size);

  Data result;
  result.mBytes = block->data.get();
  result.mSize = size;
  result.mOwner = shareBlock(block);
  return result;
}

Data Data::fromBorrowed(const void* data, const size_t size, std::shared_ptr<const void> owner) {
  if (data == nullptr || size == 0) return {};

  Data result;
  result.mBytes = static_cast<const uint8_t*>(data);
  result.mSize = size;
  result.mOwner = std::move(owner);
  return result;
}

Data Data::makeSubset(const size_t offset, size_t length) const {
  if (mBytes == nullptr || offset >= mSize) return {};

  // Clamped rather than refused. A caller slicing at a computed offset wants the part that exists, and a
  // null result would need checking at every call site; a length past the end is also how a caller says
  // "from here to the end".
  length = std::min(length, mSize - offset);

  Data result;
  result.mBytes = mBytes + offset;
  result.mSize = length;
  // The same owner, so a subset cannot outlive the memory it points into. This is why `fromBorrowed`
  // requires an owner at all: without one, a subset of borrowed memory would be a dangling view with
  // nothing to hold on to.
  result.mOwner = mOwner;
  return result;
}

bool Data::equals(const Data& other) const noexcept {
  if (mSize != other.mSize) return false;
  if (mBytes == other.mBytes) return true;  // the same buffer, which also covers both being empty
  if (mBytes == nullptr || other.mBytes == nullptr) return mSize == 0;
  return std::memcmp(mBytes, other.mBytes, mSize) == 0;
}

} // namespace graphics
