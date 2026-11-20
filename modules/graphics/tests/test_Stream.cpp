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

/**
 * @file test_Stream.cpp
 *
 * Reading bytes, and the two things that make a stream worth having rather than an array.
 *
 * The first is that reading *advances*: a parser reads a header, then a body, and never has to carry an offset. The
 * second is that a stream is copyable without giving it away, so two readers can work over the same bytes at their own
 * positions -- which is the whole reason the reference has a four-level hierarchy, and the reason this does not need
 * one.
 *
 * Almost every assertion here is about a boundary, because that is where a stream stops being an array: a read that
 * runs past the end, a seek past the end, a `peek` that must not move the position, and a packed integer at each of
 * the three widths where the encoding changes.
 */

#include <graphics/Stream.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace graphics;

namespace {

/** @return a memory stream over @p bytes. */
std::shared_ptr<Stream> streamOf(const std::vector<uint8_t>& bytes) {
  // A copy of the vector, shared with the stream. The pointer overload is deliberately not used here: for an empty
  // vector `data()` may be null, and `MakeFromBytes(nullptr, 0)` is the refusal case rather than the empty stream.
  return Stream::MakeFromBytes(std::make_shared<const std::vector<uint8_t>>(bytes));
}

/** @return the bytes of an ASCII string, for a stream whose contents are legible in a failure. */
std::vector<uint8_t> bytesOf(const std::string& text) { return { text.begin(), text.end() }; }

} // namespace anonymous

// ==================================================================================================
// Reading
// ==================================================================================================

TEST(StreamTest, ReadingAdvancesAndStopsAtTheEnd) {
  const auto stream = streamOf(bytesOf("hello"));
  ASSERT_NE(stream, nullptr);

  EXPECT_TRUE(stream->hasLength());
  EXPECT_EQ(stream->length(), 5u);
  EXPECT_TRUE(stream->hasPosition());
  EXPECT_EQ(stream->position(), 0u);
  EXPECT_FALSE(stream->isAtEnd());

  // Reading part of it leaves the position after what was read, which is what makes a parser a sequence of reads
  // rather than a sequence of offsets.
  char first[3] = {};
  EXPECT_EQ(stream->read(first, 3), 3u);
  EXPECT_EQ(std::string(first, 3), "hel");
  EXPECT_EQ(stream->position(), 3u);
  EXPECT_FALSE(stream->isAtEnd());

  char rest[8] = {};
  EXPECT_EQ(stream->read(rest, 8), 2u) << "a read past the end returns what was there, not the amount asked for";
  EXPECT_EQ(std::string(rest, 2), "lo");
  EXPECT_EQ(stream->position(), 5u);
  EXPECT_TRUE(stream->isAtEnd());

  // And at the end, reading is zero rather than a failure.
  EXPECT_EQ(stream->read(rest, 8), 0u);
}

TEST(StreamTest, PeekingDoesNotMoveThePosition) {
  const auto stream = streamOf(bytesOf("hello"));
  char buffer[4] = {};

  EXPECT_EQ(stream->peek(buffer, 4), 4u);
  EXPECT_EQ(std::string(buffer, 4), "hell");
  EXPECT_EQ(stream->position(), 0u) << "peeking is looking, not taking";

  // Reading afterwards gives the same bytes, which is the property that makes peek useful at all.
  char read[4] = {};
  EXPECT_EQ(stream->read(read, 4), 4u);
  EXPECT_EQ(std::string(read, 4), "hell");
}

TEST(StreamTest, AnEmptyStreamIsAtItsEndImmediately) {
  const auto stream = streamOf(std::vector<uint8_t> {});
  ASSERT_NE(stream, nullptr);

  EXPECT_TRUE(stream->isAtEnd());
  EXPECT_EQ(stream->length(), 0u);

  char buffer[4] = {};
  EXPECT_EQ(stream->read(buffer, 4), 0u);

  // Rewinding an empty stream succeeds: the request has been satisfied, and failing would make a caller treat "empty"
  // as "broken".
  EXPECT_TRUE(stream->canRewind());
  EXPECT_TRUE(stream->rewind());
}

// ==================================================================================================
// Position
// ==================================================================================================

TEST(StreamTest, CanRewindAndSeek) {
  const auto stream = streamOf(bytesOf("abcdef"));
  char buffer[3] = {};

  ASSERT_EQ(stream->read(buffer, 3), 3u);
  EXPECT_TRUE(stream->rewind());
  EXPECT_EQ(stream->position(), 0u);

  ASSERT_EQ(stream->read(buffer, 3), 3u);
  EXPECT_EQ(std::string(buffer, 3), "abc") << "rewinding reads the same bytes again";

  EXPECT_TRUE(stream->seek(2));
  EXPECT_EQ(stream->position(), 2u);
  ASSERT_EQ(stream->read(buffer, 3), 3u);
  EXPECT_EQ(std::string(buffer, 3), "cde");

  // A seek to the end is legal and is where a stream that has been fully read already is.
  EXPECT_TRUE(stream->seek(6));
  EXPECT_TRUE(stream->isAtEnd());

  // Past the end is refused rather than clamped, because clamping would put the position somewhere the caller did not
  // ask for and turn a bug into a wrong answer.
  EXPECT_FALSE(stream->seek(7));
  EXPECT_EQ(stream->position(), 6u) << "a refused seek leaves the position alone";
}

// ==================================================================================================
// Duplicating
// ==================================================================================================

TEST(StreamTest, ADuplicateSharesTheBytesAndHasItsOwnPosition) {
  const auto original = streamOf(bytesOf("abcdef"));
  char buffer[3] = {};

  ASSERT_EQ(original->read(buffer, 3), 3u);
  ASSERT_EQ(original->position(), 3u);

  const auto copy = original->duplicate();
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(copy->position(), 3u) << "a duplicate starts where the original is";

  // Reading from one does not move the other. This is the whole point of duplicating: two parsers over the same bytes.
  ASSERT_EQ(copy->read(buffer, 3), 3u);
  EXPECT_EQ(std::string(buffer, 3), "def");
  EXPECT_EQ(original->position(), 3u) << "the original did not move";
  EXPECT_EQ(copy->position(), 6u);

  // And they do share the bytes: a duplicate's length is the original's, not a copy of what remained.
  EXPECT_EQ(copy->length(), 6u);
  EXPECT_EQ(copy->memoryBase(), original->memoryBase()) << "the bytes are shared, not copied";
}

// ==================================================================================================
// Reading values
// ==================================================================================================

TEST(StreamTest, ReadsValuesInLittleEndian) {
  // Written out as bytes rather than built from a host integer, so the test states the format rather than restating
  // the implementation. 0x12345678 little-endian is 78 56 34 12, whatever this machine happens to be.
  const std::vector<uint8_t> bytes { 0x78, 0x56, 0x34, 0x12, 0xEF, 0xBE };
  const auto stream = streamOf(bytes);

  const auto value = stream->readValue<uint32_t>();
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, 0x12345678u);

  const auto half = stream->readValue<uint16_t>();
  ASSERT_TRUE(half.has_value());
  EXPECT_EQ(*half, 0xBEEFu);

  // A value that does not fit is refused rather than partly read, and the position is left where the failed read
  // started so a caller can try something else.
  EXPECT_FALSE(stream->readValue<uint32_t>().has_value());
}

TEST(StreamTest, ReadsBooleansAndAByteAtATime) {
  const std::vector<uint8_t> bytes { 0x01, 0x00, 0x02, 0x41 };
  const auto stream = streamOf(bytes);

  bool value = false;
  ASSERT_TRUE(stream->readBool(&value));
  EXPECT_TRUE(value);

  ASSERT_TRUE(stream->readBool(&value));
  EXPECT_FALSE(value);

  // Any non-zero byte is true, not only one. A format that writes a flag as its own value rather than normalising it
  // would otherwise read as false.
  ASSERT_TRUE(stream->readBool(&value));
  EXPECT_TRUE(value) << "two is as true as one is";

  uint8_t byte = 0;
  ASSERT_TRUE(stream->readU8(&byte));
  EXPECT_EQ(byte, 0x41);

  EXPECT_FALSE(stream->readU8(&byte)) << "at the end, reading a byte fails";
  EXPECT_EQ(byte, 0x41) << "and the output is left alone rather than half-written";
}

TEST(StreamTest, ReadsExactlyTheBytesAskedFor) {
  const auto stream = streamOf(bytesOf("hello"));

  const auto first = stream->readBytes(5);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(std::string(first->begin(), first->end()), "hello");

  // Asking for more than remains fails, because a caller that asked for exactly five bytes and got three has a
  // parsing bug that would otherwise surface as a wrong value much later.
  const auto tooMany = stream->readBytes(5);
  EXPECT_FALSE(tooMany.has_value());

  // Zero bytes is satisfiable at the end of a stream, and is not an error.
  EXPECT_TRUE(stream->readBytes(0).has_value());
}

// ==================================================================================================
// The packed integer
// ==================================================================================================

TEST(StreamTest, ReadsPackedIntegersAtEveryWidth) {
  // The three encodings, each chosen at the boundary where the format changes: the largest single-byte value, the
  // largest two-byte one, and something that needs four.
  MemoryWriteStream writer;
  ASSERT_TRUE(writer.writePackedUInt(0));
  ASSERT_TRUE(writer.writePackedUInt(0xFD)) << "the largest value that fits in one byte";
  ASSERT_TRUE(writer.writePackedUInt(0xFE)) << "one more, which needs the two-byte form";
  ASSERT_TRUE(writer.writePackedUInt(0xFFFF));
  ASSERT_TRUE(writer.writePackedUInt(0x10000)) << "one more, which needs the four-byte form";
  ASSERT_TRUE(writer.writePackedUInt(0x100000));

  // The sizes the header would need, which is what `SizeOfPackedUInt` exists for.
  EXPECT_EQ(MemoryWriteStream::SizeOfPackedUInt(0xFD), 1);
  EXPECT_EQ(MemoryWriteStream::SizeOfPackedUInt(0xFE), 3);
  EXPECT_EQ(MemoryWriteStream::SizeOfPackedUInt(0xFFFF), 3);
  EXPECT_EQ(MemoryWriteStream::SizeOfPackedUInt(0x10000), 5);

  EXPECT_EQ(writer.bytesWritten(), 1u + 1u + 3u + 3u + 5u + 5u);

  const auto bytes = writer.bytes();
  const auto stream = streamOf(bytes);
  for (const size_t expected: { size_t { 0 }, size_t { 0xFD }, size_t { 0xFE }, size_t { 0xFFFF }, size_t { 0x10000 },
             size_t { 0x100000 } }) {
    const auto value = stream->readPackedUInt();
    ASSERT_TRUE(value.has_value()) << "reading " << expected;
    EXPECT_EQ(*value, expected) << "the value written must be the value read";
  }
  EXPECT_TRUE(stream->isAtEnd());
}

TEST(StreamTest, APackedIntegerThatIsCutOffIsRefused) {
  // A sentinel with nothing after it. The stream knows it is short and says so rather than returning a value made of
  // whatever was there.
  const std::vector<uint8_t> truncated { 0xFE, 0x01 };
  const auto stream = streamOf(truncated);
  EXPECT_FALSE(stream->readPackedUInt().has_value()) << "a two-byte sentinel needs two bytes";

  const std::vector<uint8_t> fourByteTruncated { 0xFF, 0x01, 0x02 };
  const auto stream2 = streamOf(fourByteTruncated);
  EXPECT_FALSE(stream2->readPackedUInt().has_value()) << "a four-byte sentinel needs four bytes";

  const auto empty = streamOf(std::vector<uint8_t> {});
  EXPECT_FALSE(empty->readPackedUInt().has_value());
}

TEST(StreamTest, ThePackedEncodingDoesNotDependOnTheHost) {
  // The bytes of the two-byte form, checked rather than assumed. A version that memcpy'd a host integer would come out
  // the other way round on a big-endian machine, and no test that only round-tripped would notice.
  MemoryWriteStream writer;
  ASSERT_TRUE(writer.writePackedUInt(0x0201));
  const auto bytes = writer.bytes();
  ASSERT_EQ(bytes.size(), 3u);
  EXPECT_EQ(bytes[0], 0xFE) << "the sentinel means two bytes follow";
  EXPECT_EQ(bytes[1], 0x01) << "least significant byte first";
  EXPECT_EQ(bytes[2], 0x02);
}

// ==================================================================================================
// Writing
// ==================================================================================================

TEST(StreamTest, TheWriteStreamGrowsAndHandsOverItsBytes) {
  MemoryWriteStream writer;
  EXPECT_EQ(writer.bytesWritten(), 0u);

  ASSERT_TRUE(writer.writeU8(0x41));
  ASSERT_TRUE(writer.writeValue<uint16_t>(0x0201));
  ASSERT_TRUE(writer.writeBool(true));
  ASSERT_TRUE(writer.writeBool(false));

  EXPECT_EQ(writer.bytesWritten(), 5u);
  const auto bytes = writer.bytes();
  EXPECT_EQ(bytes[0], 0x41);
  EXPECT_EQ(bytes[1], 0x01) << "little-endian, as on the reading side";
  EXPECT_EQ(bytes[2], 0x02);
  EXPECT_EQ(bytes[3], 0x01) << "true is one";
  EXPECT_EQ(bytes[4], 0x00) << "false is zero";

  // Taking the bytes empties the stream, and the result is what was there.
  const auto taken = writer.takeBytes();
  EXPECT_EQ(taken.size(), 5u);
  EXPECT_EQ(writer.bytesWritten(), 0u);
}

TEST(StreamTest, WritingAStreamCopiesExactlyTheLengthAsked) {
  const auto source = streamOf(bytesOf("abcdefgh"));

  MemoryWriteStream writer;
  ASSERT_TRUE(writer.writeStream(source.get(), 3));
  EXPECT_EQ(writer.bytes(), bytesOf("abc"));
  EXPECT_EQ(source->position(), 3u) << "the source advanced by what was taken";

  // Asking for more than the source holds fails rather than writing a short run, because a caller that asked for a
  // length and got less has a format bug that a short write would hide.
  EXPECT_FALSE(writer.writeStream(source.get(), 100));
}

// ==================================================================================================
// Files
// ==================================================================================================

TEST(StreamTest, ReadsAFile) {
  // Written next to the test binary's working directory rather than in the system temp directory: a
  // sandboxed or redirected TEMP makes the ofstream fail, and the test then fails on a file it never
  // wrote instead of on what it is testing. The name is unique per run so parallel suites cannot
  // collide on it.
  const std::filesystem::path path = std::filesystem::current_path() /
    ("coral-stream-test-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bin");
  const std::vector<uint8_t> content { 1, 2, 3, 4, 5, 6, 7, 8 };
  {
    std::ofstream file(path, std::ios::binary);
    ASSERT_TRUE(file.is_open()) << "the test could not create its fixture at " << path.string();
    file.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
    ASSERT_TRUE(file.good()) << "the fixture write failed";
  }

  // The stream lives in an inner scope so it is destroyed -- and its file handle closed -- before the
  // fixture is removed. Windows refuses to delete a file another handle still has open, which is why
  // the original version of this test left its fixture behind in the temp directory.
  {
    const auto stream = Stream::MakeFromFile(path.string());
    ASSERT_NE(stream, nullptr);

    EXPECT_TRUE(stream->hasLength());
    EXPECT_EQ(stream->length(), content.size());

    // A file stream can seek but cannot peek or hand out its bytes: those two need the whole thing in memory, and
    // claiming them would be a lie a parser would build on.
    EXPECT_TRUE(stream->hasPosition());
    EXPECT_EQ(stream->peek(nullptr, 8), 0u) << "a file cannot look ahead without reading";
    EXPECT_FALSE(stream->canRewind());
    EXPECT_EQ(stream->memoryBase(), nullptr);

    std::vector<uint8_t> read(content.size());
    EXPECT_EQ(stream->read(read.data(), read.size()), content.size());
    EXPECT_EQ(read, content);
    EXPECT_TRUE(stream->isAtEnd());

    ASSERT_TRUE(stream->seek(4));
    EXPECT_EQ(stream->position(), 4u);
    uint8_t byte = 0;
    ASSERT_TRUE(stream->readU8(&byte));
    EXPECT_EQ(byte, 5) << "seeking moved the file to the fifth byte";
  }

  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

TEST(StreamTest, AFileThatIsNotThereIsRefused) {
  EXPECT_EQ(Stream::MakeFromFile("this-file-does-not-exist-at-all.bin"), nullptr);
}

TEST(StreamTest, AStreamOverNothingIsRefused) {
  EXPECT_EQ(Stream::MakeFromBytes(nullptr, 0), nullptr);
}
