/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and limitations under the License.
 */

#include <graphics/Stream.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <utility>

/**
 * @file Stream.cpp
 *
 * The two sources a stream comes from: bytes in memory, and a file.
 *
 * ## Why the memory case is the important one
 *
 * A file-backed stream cannot peek, cannot be rewound cheaply, and cannot hand out a pointer to its bytes -- so a
 * parser written against it has to be written defensively. A memory-backed one can do all three, and every format
 * decoder in this library reads from a buffer anyway: the file has already been read into memory by the time anything
 * wants to parse it.
 *
 * So the file stream exists for the case where the caller does *not* want to hold the whole file, and it is honest
 * about being the weaker of the two rather than pretending to capabilities it does not have.
 */

namespace graphics {
namespace {

/** A stream over bytes that are already in memory. */
class MemoryStream final : public Stream {
public:
  explicit MemoryStream(std::shared_ptr<const std::vector<uint8_t>> bytes) noexcept : mBytes(std::move(bytes)) {
  }

  [[nodiscard]] size_t read(void* buffer, const size_t size) override {
    if (buffer == nullptr || size == 0 || mBytes == nullptr) return 0;

    const size_t available = mBytes->size() - mOffset;
    const size_t count = std::min(size, available);
    if (count == 0) return 0;

    std::memcpy(buffer, mBytes->data() + mOffset, count);
    mOffset += count;
    return count;
  }

  [[nodiscard]] size_t peek(void* buffer, const size_t size) const override {
    if (buffer == nullptr || size == 0 || mBytes == nullptr) return 0;

    const size_t available = mBytes->size() - mOffset;
    const size_t count = std::min(size, available);
    if (count != 0) std::memcpy(buffer, mBytes->data() + mOffset, count);
    return count;
  }

  [[nodiscard]] bool isAtEnd() const override { return mBytes == nullptr || mOffset >= mBytes->size(); }

  [[nodiscard]] bool canRewind() const override { return true; }

  // Rewinding to the beginning always works, including on an empty stream: the request has been satisfied.
  bool rewind() override {
    mOffset = 0;
    return true;
  }

  [[nodiscard]] bool hasPosition() const override { return true; }
  [[nodiscard]] size_t position() const override { return mOffset; }

  bool seek(const size_t offset) override {
    if (mBytes == nullptr || offset > mBytes->size()) return false;
    mOffset = offset;
    return true;
  }

  [[nodiscard]] bool hasLength() const override { return true; }
  [[nodiscard]] size_t length() const override { return mBytes == nullptr ? 0 : mBytes->size(); }

  [[nodiscard]] const void* memoryBase() const override { return mBytes == nullptr ? nullptr : mBytes->data(); }

  [[nodiscard]] std::shared_ptr<Stream> duplicate() const override {
    // A copy that shares the bytes and starts at the same place. The copy's offset is its own, so reading from one
    // does not move the other -- which is the whole of what the reference's `onDuplicate` is for.
    auto copy = std::make_shared<MemoryStream>(mBytes);
    copy->mOffset = mOffset;
    return copy;
  }

private:
  std::shared_ptr<const std::vector<uint8_t>> mBytes;
  size_t mOffset = 0;
};

/**
 * A stream over a file.
 *
 * The four capabilities the memory stream has and this does not are answered honestly rather than approximated:
 * `peek` returns zero, `canRewind` is false, `memoryBase` is null. A caller that needs them calls `readBytes` on the
 * whole file and builds a memory stream, which is a decision about memory that belongs to the caller and not here.
 *
 * Seeking is still offered, because a file stream can do it -- which is why `hasPosition` and `hasLength` are
 * separate questions from `canRewind`. A caller that only needs to jump to an offset is served.
 */
class FileStream final : public Stream {
public:
  explicit FileStream(std::ifstream file) noexcept
    : mFile(std::move(file)) {
    // The length is taken once, at construction, because a caller asks for it repeatedly and `seekg` to the end and
    // back on every call would be a filesystem operation per question.
    mFile.seekg(0, std::ios::end);
    const auto end = mFile.tellg();
    mLength = (end > 0) ? static_cast<size_t>(end) : 0;
    mFile.seekg(0, std::ios::beg);
  }

  [[nodiscard]] size_t read(void* buffer, const size_t size) override {
    if (buffer == nullptr || size == 0 || !mFile.good()) return 0;
    mFile.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
    const auto count = mFile.gcount();
    if (count <= 0) return 0;

    // Tracked rather than asked for. `tellg` is not const, so a `position() const` cannot call it -- and asking the
    // stream where it is on every query is a library call per byte offset when a counter costs an addition.
    mPosition += static_cast<size_t>(count);
    return static_cast<size_t>(count);
  }

  [[nodiscard]] bool isAtEnd() const override { return mPosition >= mLength; }

  [[nodiscard]] bool hasPosition() const override { return true; }

  [[nodiscard]] size_t position() const override { return mPosition; }

  bool seek(const size_t offset) override {
    if (offset > mLength) return false;
    mFile.clear(); // a previous read that hit the end leaves the fail bit set, which would block the seek
    mFile.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!mFile.good()) return false;

    mPosition = offset;
    return true;
  }

  [[nodiscard]] bool hasLength() const override { return true; }
  [[nodiscard]] size_t length() const override { return mLength; }

  [[nodiscard]] std::shared_ptr<Stream> duplicate() const override {
    // A second handle on the same path. Not a copy of the open file -- a stream is not copyable -- so the caller gets
    // an independent reader at the same position, which is what duplicating is for.
    auto copy = std::make_shared<FileStream>(std::ifstream(mPath, std::ios::binary));
    copy->seek(position());
    return copy;
  }

private:
  friend std::shared_ptr<Stream> Stream::MakeFromFile(const std::string&);

  std::ifstream mFile;
  std::string mPath;
  size_t mLength = 0;
  size_t mPosition = 0;
};

} // namespace anonymous

std::shared_ptr<Stream> Stream::MakeFromBytes(std::shared_ptr<const std::vector<uint8_t>> bytes) {
  if (bytes == nullptr) return nullptr;
  return std::make_shared<MemoryStream>(std::move(bytes));
}

std::shared_ptr<Stream> Stream::MakeFromBytes(const void* data, const size_t size) {
  if (data == nullptr) return nullptr;

  auto copy = std::make_shared<std::vector<uint8_t>>(size);
  if (size != 0) std::memcpy(copy->data(), data, size);
  return std::make_shared<MemoryStream>(std::move(copy));
}

std::shared_ptr<Stream> Stream::MakeFromFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.good()) return nullptr;

  auto stream = std::make_shared<FileStream>(std::move(file));
  stream->mPath = path;
  return stream;
}

} // namespace graphics
