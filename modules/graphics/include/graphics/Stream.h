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

#ifndef CORAL_GRAPHICS_STREAM_H
#define CORAL_GRAPHICS_STREAM_H

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace graphics {

/**
 * A source of bytes that can be read once, or again if it says so.
 *
 * ## Why this is one class and not five
 *
 * The reference has a four-level hierarchy -- `SkStream`, `SkStreamRewindable`, `SkStreamSeekable`, `SkStreamAsset` --
 * each adding a capability, with a hand-written `onFork`/`onDuplicate` at every level so that a stream can be handed
 * out without giving away the only copy. Every one of those levels exists to solve an *ownership* problem: a reader
 * needs its own position, and the bytes have to outlive both readers.
 *
 * `shared_ptr` solves that directly. A stream is cheap to copy, the copy shares the bytes and carries its own
 * position, and there is no separate "duplicate" or "fork" to implement or to get wrong. So a capability becomes a
 * *question* rather than a type -- `hasPosition()`, `hasLength()` -- which is what a caller actually wants to ask. A
 * caller that needs seeking asks; it does not need a different type to be handed one.
 *
 * The cost is that a capability is a runtime question rather than a compile-time one. That is the right trade: the
 * alternative is five types and a virtual `onFork` on each, to prevent a mistake `hasPosition()` catches at the one
 * place it matters -- and a caller that reads from a file has to handle a non-seekable source anyway.
 *
 * ## What a stream is not
 *
 * It is not a container, and it does not own everything it can see. `read` advances and `peek` does not; a caller that
 * wants bytes without consuming them asks for a position and comes back. The exception is `memoryBase`, which a
 * memory-backed stream answers with the bytes themselves -- they are already there, and a copy would cost without
 * achieving anything.
 */
class Stream {
public:
  virtual ~Stream() = default;

  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;

  // ================================================================================================
  // The one required operation
  // ================================================================================================

  /**
   * @return the number of bytes read, which is less than @p size only at the end of the stream.
   *
   * Zero means nothing was read, and at the end of a stream that is the ordinary answer rather than an error. A caller
   * that needs exactly @p size bytes has to check, which is why a count is returned rather than a flag: the caller
   * that accepts a short read and the one that does not are both served, and neither has to guess.
   */
  virtual size_t read(void* buffer, size_t size) = 0;

  // ================================================================================================
  // Capabilities
  // ================================================================================================

  /**
   * @return the number of bytes copied into @p buffer without advancing, which may be less than @p size.
   *
   * Zero for a stream that cannot look ahead, which includes every stream whose source is not already in memory. That
   * is not a failure; it is the answer to "can you peek", and a caller that needs it checks.
   */
  [[nodiscard]] virtual size_t peek(void* buffer, size_t size) const {
    (void) buffer;
    (void) size;
    return 0;
  }

  /** @return true when there is nothing left to read. */
  [[nodiscard]] virtual bool isAtEnd() const = 0;

  /** @return true when this can be rewound to the beginning. */
  [[nodiscard]] virtual bool canRewind() const { return false; }

  /** @return true when the stream was rewound. False for a stream that cannot, so a caller must check. */
  virtual bool rewind() { return false; }

  /** @return true when this has a position that can be both read and set. */
  [[nodiscard]] virtual bool hasPosition() const { return false; }

  /** @return the byte offset of the next read, or zero for a stream without positions. */
  [[nodiscard]] virtual size_t position() const { return 0; }

  /** @return true when the stream was moved. */
  virtual bool seek(size_t offset) {
    (void) offset;
    return false;
  }

  /** @return true when this knows how many bytes it holds in total. */
  [[nodiscard]] virtual bool hasLength() const { return false; }

  /** @return the total length in bytes, or zero for a stream that does not know it. */
  [[nodiscard]] virtual size_t length() const { return 0; }

  /**
   * @return a pointer to the bytes when they are all in memory, or null when they are not.
   *
   * Zero-copy access for a caller that wants to parse a buffer it can already see. A stream backed by a file answers
   * null, because there is nothing to point at.
   */
  [[nodiscard]] virtual const void* memoryBase() const { return nullptr; }

  /**
   * @return a copy of this stream, sharing the bytes and carrying its own position.
   *
   * Cheap, and the reason the reference's `onFork` and `onDuplicate` are not needed: copying the `shared_ptr` shares
   * the source without giving it away, and the position belongs to the derived object rather than to what is shared.
   */
  [[nodiscard]] virtual std::shared_ptr<Stream> duplicate() const = 0;

  // ================================================================================================
  // Reading values
  // ================================================================================================

  /** @return true when a byte was read. @p value is left alone on a short read. */
  [[nodiscard]] bool readU8(uint8_t* value) {
    if (value == nullptr) return false;
    uint8_t byte = 0;
    if (read(&byte, 1) != 1) return false;
    *value = byte;
    return true;
  }

  /** @return true when a byte was read. Any non-zero byte means true, as the reference defines it. */
  [[nodiscard]] bool readBool(bool* value) {
    uint8_t byte = 0;
    if (!readU8(&byte)) return false;
    if (value != nullptr) *value = byte != 0;
    return true;
  }

  /** @return the little-endian value read, or `std::nullopt` at the end of the stream. */
  template <typename T>
  [[nodiscard]] std::optional<T> readValue() {
    static_assert(std::is_trivially_copyable_v<T>, "only plain values can be read from a stream");
    std::array<uint8_t, sizeof(T)> bytes {};
    if (read(bytes.data(), bytes.size()) != bytes.size()) return std::nullopt;

    // Assembled a byte at a time rather than by reinterpreting the buffer, so the value does not depend on the host's
    // endianness or on whether the bytes happened to be aligned. Bytes written on one machine have to read the same
    // on another, and a cast would make that a property of the compiler rather than of the format.
    T value {};
    for (size_t i = 0; i < bytes.size(); ++i) {
      value |= static_cast<T>(bytes[i]) << (8 * i);
    }
    return value;
  }

  /** @return exactly @p size bytes, or `std::nullopt` when fewer remain. */
  [[nodiscard]] std::optional<std::vector<uint8_t>> readBytes(const size_t size) {
    std::vector<uint8_t> bytes(size);
    if (size == 0) return bytes;
    if (read(bytes.data(), size) != size) return std::nullopt;
    return bytes;
  }

  /** @return a view of the remaining bytes when they are already in memory, or an empty view otherwise. */
  [[nodiscard]] std::string_view remainingMemory() const {
    const auto* base = static_cast<const uint8_t*>(memoryBase());
    if (base == nullptr || !hasLength()) return {};

    const size_t at = position();
    const size_t total = length();
    if (at >= total) return {};
    return { reinterpret_cast<const char*>(base) + at, total - at };
  }

  /**
   * @return a variable-length unsigned integer, or `std::nullopt` at the end of the stream.
   *
   * ## The encoding
   *
   * A small value costs one byte and a large one five, which is what makes this worth having for the counts and
   * lengths a serialised format is mostly made of:
   *
   *  - `0` to `0xFD`: the value itself, in one byte.
   *  - `0xFE`, then two bytes: a value up to `0xFFFF`.
   *  - `0xFF`, then four bytes: anything larger.
   *
   * `0xFE` and `0xFF` are sentinels rather than values, which is why the one-byte range stops at `0xFD`: a byte of
   * `0xFE` has to mean "two bytes follow" and not "254", or the encoding would be ambiguous and no decoder could
   * recover the value.
   *
   * The reference reports this through an out-parameter and a bool. An `optional` says the same thing and cannot be
   * ignored by accident.
   */
  [[nodiscard]] std::optional<size_t> readPackedUInt() {
    constexpr uint8_t kSentinelForU16 = 0xFE;
    constexpr uint8_t kSentinelForU32 = 0xFF;

    uint8_t first = 0;
    if (!readU8(&first)) return std::nullopt;

    if (first == kSentinelForU16) {
      const auto value = readValue<uint16_t>();
      if (!value.has_value()) return std::nullopt;
      return *value;
    }
    if (first == kSentinelForU32) {
      const auto value = readValue<uint32_t>();
      if (!value.has_value()) return std::nullopt;
      return *value;
    }
    return first;
  }

  // ================================================================================================
  // Building one
  // ================================================================================================

  /** @return an in-memory stream over @p bytes, which it shares rather than copies. */
  [[nodiscard]] static std::shared_ptr<Stream> MakeFromBytes(std::shared_ptr<const std::vector<uint8_t>> bytes);

  /** @return an in-memory stream that copies @p data. */
  [[nodiscard]] static std::shared_ptr<Stream> MakeFromBytes(const void* data, size_t size);

  /** @return a stream over @p path, or null when it cannot be opened. */
  [[nodiscard]] static std::shared_ptr<Stream> MakeFromFile(const std::string& path);

protected:
  Stream() = default;
};

/**
 * A destination that writes to memory.
 *
 * Kept because it is what a caller reaches for to serialise something: build the bytes, then take them. The reference
 * has no such thing -- it has a null stream for measuring and leaves the buffer to every caller -- which means every
 * caller writes the same growable buffer. It is the same reasoning as `Stream` itself being one class: the ordinary
 * case deserves a type rather than a paragraph of instructions.
 */
class MemoryWriteStream {
public:
  MemoryWriteStream() = default;
  virtual ~MemoryWriteStream() = default;

  /**
   * @return true when all @p size bytes were written.
   *
   * A memory-backed destination always succeeds. The answer is here so that a caller can write code against any
   * destination, which is the only thing an interface buys when there is one implementation.
   */
  virtual bool write(const void* buffer, size_t size) {
    if (size == 0) return true;
    if (buffer == nullptr) return false;
    const auto* bytes = static_cast<const uint8_t*>(buffer);
    mBytes.insert(mBytes.end(), bytes, bytes + size);
    return true;
  }

  /** @return the number of bytes written so far. */
  [[nodiscard]] size_t bytesWritten() const noexcept { return mBytes.size(); }

  /** @return the bytes written, which is only final once nothing more will be. */
  [[nodiscard]] const std::vector<uint8_t>& bytes() const noexcept { return mBytes; }

  /**
   * @return the bytes written, leaving this empty.
   *
   * A move rather than a copy, because the usual pattern is to build a buffer and hand it over, and copying at the end
   * would double the peak memory of every serialisation for no reason.
   */
  [[nodiscard]] std::vector<uint8_t> takeBytes() { return std::move(mBytes); }

  bool writeU8(const uint8_t value) { return write(&value, 1); }

  bool writeBool(const bool value) { return writeU8(value ? uint8_t { 1 } : uint8_t { 0 }); }

  /** Writes @p value as @c sizeof(T) little-endian bytes, the inverse of `Stream::readValue`. */
  template <typename T>
  bool writeValue(const T& value) {
    static_assert(std::is_trivially_copyable_v<T>, "only plain values can be written to a stream");
    std::array<uint8_t, sizeof(T)> bytes {};
    for (size_t i = 0; i < bytes.size(); ++i) {
      bytes[i] = static_cast<uint8_t>((static_cast<uint64_t>(value) >> (8 * i)) & 0xFF);
    }
    return write(bytes.data(), bytes.size());
  }

  /** @return how many bytes @p value takes in the packed encoding, which a caller needs to size a header. */
  [[nodiscard]] static int SizeOfPackedUInt(const size_t value) noexcept {
    if (value <= 0xFD) return 1;
    if (value <= 0xFFFF) return 3;
    return 5;
  }

  /** Writes @p value in the encoding `Stream::readPackedUInt` reads. */
  bool writePackedUInt(const size_t value) {
    if (value <= 0xFD) return writeU8(static_cast<uint8_t>(value));

    if (value <= 0xFFFF) {
      if (!writeU8(0xFE)) return false;
      return writeValue(static_cast<uint16_t>(value));
    }

    if (!writeU8(0xFF)) return false;
    return writeValue(static_cast<uint32_t>(value));
  }

  /** @return true when all @p length bytes were copied from @p source. */
  bool writeStream(Stream* source, const size_t length) {
    if (length == 0) return true;
    if (source == nullptr) return false;

    std::vector<uint8_t> buffer(length);
    if (source->read(buffer.data(), length) != length) return false;
    return write(buffer.data(), buffer.size());
  }

private:
  std::vector<uint8_t> mBytes;
};

} // namespace graphics

#endif //CORAL_GRAPHICS_STREAM_H
