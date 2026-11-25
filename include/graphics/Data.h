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

#ifndef CORAL_GRAPHICS_DATA_H
#define CORAL_GRAPHICS_DATA_H

#include <cstdint>
#include <memory>

namespace graphics {

/**
 * A block of bytes that several parts of the graphics layer can share.
 *
 * ## Why the bytes are shared rather than copied
 *
 * A font, an encoded image and a decoded image are all "some bytes" as far as the layer above is
 * concerned, and all three are large enough that copying them shows up in a profile. Passing a
 * `std::vector` by value copies it; passing it by reference ties the lifetime of everything downstream
 * to a container the caller may move or destroy. `Data` answers both: a reference-counted buffer with
 * read-only access, which can be handed around and kept for as long as anything needs it.
 *
 * ## Immutable on purpose
 *
 * There is no writable accessor. A buffer that several holders can see must not be mutable, or the
 * compiler's guarantees stop meaning anything. A caller that needs to modify bytes makes a copy with
 * @c makeCopy and writes to that. Immutability is also what lets a `Data` be shared across threads
 * without a lock.
 *
 * ## Borrowing as well as owning
 *
 * @c fromBorrowed wraps memory somebody else owns -- a memory-mapped font, a static table, a buffer the
 * caller keeps alive for its own reasons. Nothing is copied and nothing is freed by the `Data`. The
 * distinction between owning and borrowing is why the implementation holds both a pointer and an owner
 * rather than only a pointer.
 */
class Data {
public:
  Data() noexcept = default;
  ~Data();

  Data(const Data&) noexcept = default;
  Data& operator=(const Data&) noexcept = default;

  /**
   * Takes ownership of @p size bytes, copying them out of @p data.
   *
   * The copy is unconditional, which is the point: a caller that built a buffer locally can hand it over
   * and forget about it, and that is the common case worth making the default. A caller that already
   * owns memory it can share should use @c fromBorrowed instead.
   *
   * @return an empty `Data` when @p data is null or @p size is zero, rather than a buffer that claims a
   *         size it cannot read.
   */
  [[nodiscard]] static Data makeCopy(const void* data, size_t size);

  /** Convenience overload for a container of bytes. */
  template <typename Container>
  [[nodiscard]] static Data makeCopy(const Container& container) {
    return makeCopy(container.data(), container.size() * sizeof(typename Container::value_type));
  }

  /** @return a `Data` owning @p size zeroed bytes, for a caller about to fill them in. */
  [[nodiscard]] static Data makeUninitialised(size_t size);

  /**
   * Wraps memory that @p owner keeps alive.
   *
   * **@p data must remain valid for as long as any `Data` referring to it exists.** The @p owner is held
   * so that the requirement is enforceable when the caller has something to hold -- a `shared_ptr` to the
   * buffer, for instance. Passing no owner is a promise by the caller, appropriate for a static table or
   * a memory mapping that outlives everything.
   */
  [[nodiscard]] static Data fromBorrowed(const void* data, size_t size, std::shared_ptr<const void> owner = nullptr);

  /** @return a read-only pointer to the first byte, or null when this is empty. */
  [[nodiscard]] const uint8_t* bytes() const noexcept { return mBytes; }

  /** @return the number of bytes. Zero for a default-constructed or empty `Data`. */
  [[nodiscard]] size_t size() const noexcept { return mSize; }

  [[nodiscard]] bool isEmpty() const noexcept { return mSize == 0; }

  /** @return the byte at @p index, or zero when out of range. */
  [[nodiscard]] uint8_t operator[](size_t index) const noexcept {
    return index < mSize ? mBytes[index] : 0;
  }

  /**
   * @return a sub-range as a new `Data` sharing the same memory.
   *
   * Out-of-range requests are clamped rather than refused, so a caller slicing a buffer at a computed
   * offset gets a shorter result instead of a null one it has to check for at every use. The result
   * borrows from the same owner, so the original's lifetime governs both.
   */
  [[nodiscard]] Data makeSubset(size_t offset, size_t length) const;

  /**
   * @return true when the two hold the same bytes.
   *
   * A comparison of contents, not of identity: two `Data` values copied from the same source are equal.
   * That matters for a cache, which is the same reasoning as `ImageInfo::operator==`.
   */
  [[nodiscard]] bool equals(const Data& other) const noexcept;

  /**
   * @return the bytes as characters, for text that arrived as a buffer.
   *
   * Not null-terminated: the length is @c size, and a caller that needs a string must construct one from
   * the pointer and the length rather than treating this as a C string.
   */
  [[nodiscard]] const char* asText() const noexcept { return reinterpret_cast<const char*>(mBytes); }

private:
  /** The first byte, or null when empty. */
  const uint8_t* mBytes = nullptr;
  /** The length in bytes. */
  size_t mSize = 0;
  /**
   * Keeps the memory alive, whether this owns it or only borrows it.
   *
   * One `shared_ptr` covers both cases: for an owned buffer it points at the allocation, and for a
   * borrowed one it holds whatever the caller supplied -- possibly nothing, in which case the caller has
   * promised the memory outlives this.
   */
  std::shared_ptr<const void> mOwner;
};

} // namespace graphics

#endif //CORAL_GRAPHICS_DATA_H
