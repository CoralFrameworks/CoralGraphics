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

/**
 * @file test_Data.cpp
 *
 * Shared immutable bytes.
 *
 * The interesting properties are all about lifetime, and a lifetime bug is invisible in a passing test --
 * it shows up as a crash under a different allocator or as garbage in a release build. So the tests here
 * do what they can to make the lifetime observable: they hold a `weak_ptr` to the owner and check whether
 * it was released, and they check that a subset keeps the memory alive after the original is gone.
 */

#include <graphics/Data.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace graphics;

TEST(DataTest, EmptyByDefault) {
  const Data data;
  EXPECT_TRUE(data.isEmpty());
  EXPECT_EQ(data.size(), 0u);
  EXPECT_EQ(data.bytes(), nullptr);
  EXPECT_EQ(data[0], 0) << "reading past the end of an empty buffer is defined as zero";
  // `asText` mirrors the pointer, which is null when there is nothing -- so it is not a C string to hand to
  // anything. A caller with a length must use it.
  EXPECT_EQ(data.asText(), nullptr);
}

TEST(DataTest, CopiesBytesItIsGiven) {
  const std::string source = "hello";
  const Data data = Data::makeCopy(source.data(), source.size());

  EXPECT_FALSE(data.isEmpty());
  EXPECT_EQ(data.size(), 5u);
  EXPECT_EQ(std::string(data.asText(), data.size()), "hello");

  // The copy is real, not a view: changing the source afterwards must not show through, or every caller
  // that builds a buffer locally and hands it over has a dangling reference it cannot see.
  std::string mutableSource = "hello";
  const Data fromMutable = Data::makeCopy(mutableSource.data(), mutableSource.size());
  mutableSource[0] = 'X';
  EXPECT_EQ(fromMutable[0], 'h') << "the bytes were copied, not borrowed";
  EXPECT_EQ(std::string(fromMutable.asText(), fromMutable.size()), "hello");
}

TEST(DataTest, ContainerOverloadTakesTheWholeContainer) {
  const std::vector<uint8_t> bytes { 1, 2, 3, 4 };
  const Data data = Data::makeCopy(bytes);

  EXPECT_EQ(data.size(), 4u);
  EXPECT_EQ(data[0], 1);
  EXPECT_EQ(data[3], 4);
}

TEST(DataTest, RefusesNullOrZeroSize) {
  const uint8_t byte = 7;
  // A null pointer with a claimed size would be a buffer that reports bytes it cannot read, so it is
  // refused rather than represented.
  EXPECT_TRUE(Data::makeCopy(nullptr, 16).isEmpty());
  EXPECT_TRUE(Data::makeCopy(&byte, 0).isEmpty());
  EXPECT_TRUE(Data::fromBorrowed(nullptr, 16).isEmpty());
  EXPECT_TRUE(Data::makeUninitialised(0).isEmpty());
}

TEST(DataTest, UninitialisedIsZeroedNotIndeterminate) {
  // A caller that fills part of the buffer and reads the rest back gets defined bytes. Leaving the
  // allocation indeterminate would make that read whatever was in the heap, which is the difference
  // between a bug and a disclosure.
  const Data data = Data::makeUninitialised(64);
  ASSERT_EQ(data.size(), 64u);
  for (size_t i = 0; i < data.size(); ++i) {
    EXPECT_EQ(data[i], 0) << "byte " << i;
  }
}

TEST(DataTest, BorrowsWithoutCopying) {
  const uint8_t source[4] = { 10, 20, 30, 40 };
  const Data data = Data::fromBorrowed(source, sizeof(source));

  EXPECT_EQ(data.size(), 4u);
  EXPECT_EQ(data[0], 10);
  EXPECT_EQ(data.bytes(), source) << "borrowing must not copy; the pointer is the caller's";
}

TEST(DataTest, SubsetSharesMemoryAndClamps) {
  const uint8_t source[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
  const Data data = Data::makeCopy(source, sizeof(source));

  const Data middle = data.makeSubset(2, 3);
  ASSERT_EQ(middle.size(), 3u);
  EXPECT_EQ(middle[0], 2);
  EXPECT_EQ(middle[2], 4);
  EXPECT_EQ(middle.bytes(), data.bytes() + 2) << "a subset is a view, not a copy";

  // A length past the end is clamped rather than refused: a caller asking for "from here to the end" is
  // the common case, and a null result would need checking at every call site.
  const Data tail = data.makeSubset(5, 1000);
  ASSERT_EQ(tail.size(), 3u);
  EXPECT_EQ(tail[0], 5);
  EXPECT_EQ(tail[2], 7);

  // An offset at or past the end has nothing to give.
  EXPECT_TRUE(data.makeSubset(8, 1).isEmpty());
  EXPECT_TRUE(data.makeSubset(100, 1).isEmpty());
}

TEST(DataTest, SubsetKeepsTheOwnerAlive) {
  // The point of the owner field. A subset that pointed into memory it did not keep alive would be a
  // dangling view the moment the original went away -- and nothing about the subset would look wrong.
  std::weak_ptr<int> observer;

  Data subset;
  {
    auto owner = std::make_shared<int>(42);
    observer = owner;

    const uint8_t source[4] = { 1, 2, 3, 4 };
    const Data data = Data::fromBorrowed(source, sizeof(source), std::static_pointer_cast<const void>(owner));
    subset = data.makeSubset(1, 2);
    EXPECT_EQ(subset[0], 2);
  }

  // The shared owner was released when the original `Data` went... but the subset still holds a reference,
  // so the count is not zero yet. This is the assertion that would fail if `makeSubset` forgot to copy the
  // owner across.
  EXPECT_FALSE(observer.expired()) << "the subset must keep the borrowed memory's owner alive";

  subset = Data {};
  EXPECT_TRUE(observer.expired()) << "and must release it when the last view goes";
}

TEST(DataTest, CopyingSharesRatherThanDuplicating) {
  const uint8_t source[4] = { 1, 2, 3, 4 };

  std::weak_ptr<int> observer;
  Data copy;
  {
    auto owner = std::make_shared<int>(1);
    observer = owner;
    const Data data = Data::fromBorrowed(source, sizeof(source), std::static_pointer_cast<const void>(owner));
    copy = data;
  }

  EXPECT_FALSE(observer.expired()) << "a copy holds a reference to the same owner";

  // And the copy sees the same bytes, because it is the same buffer.
  EXPECT_EQ(copy.size(), 4u);
  EXPECT_EQ(copy[0], 1);
}

TEST(DataTest, EqualityComparesContents) {
  const uint8_t a[3] = { 1, 2, 3 };
  const uint8_t b[3] = { 1, 2, 3 };
  const uint8_t c[3] = { 1, 2, 4 };

  const Data first = Data::makeCopy(a, 3);
  const Data second = Data::makeCopy(b, 3);  // separate allocation, same bytes
  const Data third = Data::makeCopy(c, 3);

  EXPECT_TRUE(first.equals(second)) << "different buffers, same contents";
  EXPECT_FALSE(first.equals(third));
  EXPECT_TRUE(first.equals(first));

  // Different lengths are never equal, and an empty buffer equals only another empty one.
  EXPECT_FALSE(first.equals(Data::makeCopy(a, 2)));
  EXPECT_TRUE(Data {}.equals(Data {}));
  EXPECT_FALSE(first.equals(Data {}));
}

TEST(DataTest, IndexingPastTheEndIsZeroRatherThanUndefined) {
  const uint8_t source[2] = { 9, 8 };
  const Data data = Data::makeCopy(source, 2);

  EXPECT_EQ(data[2], 0);
  EXPECT_EQ(data[1000], 0);
}
