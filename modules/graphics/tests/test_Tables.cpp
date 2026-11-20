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
 * @file test_Tables.cpp
 *
 * The two table containers, which are the least interesting types in the library and the easiest to get quietly wrong.
 *
 * Neither does any arithmetic, so there is nothing to compute and check. What there is to check is the boundary
 * behaviour, because that is where a container stops being a container: an empty table, an index past the end, an
 * entry of zero length, and a table built from entries of differing sizes. A lookup that read past the end would not
 * fail here -- it would fail somewhere else, later, with the bytes of whatever followed.
 */

#include <graphics/ColorTable.h>
#include <graphics/DataTable.h>

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

using namespace graphics;

// ==================================================================================================
// ColorTable
// ==================================================================================================

TEST(ColorTableTest, TheIdentityTableMapsEveryValueToItself) {
  const ColorTable identity = ColorTable::Identity();

  EXPECT_TRUE(identity.isIdentity());
  for (int i = 0; i < 256; ++i) {
    const auto value = static_cast<uint8_t>(i);
    EXPECT_EQ(identity.alpha[static_cast<size_t>(i)], value) << "at " << i;
    EXPECT_EQ(identity.red[static_cast<size_t>(i)], value) << "at " << i;
    EXPECT_EQ(identity.green[static_cast<size_t>(i)], value) << "at " << i;
    EXPECT_EQ(identity.blue[static_cast<size_t>(i)], value) << "at " << i;
  }
}

TEST(ColorTableTest, ADefaultTableIsNotTheIdentity) {
  // The default has every entry zero, which maps everything to black. That is a real filter -- it is what a caller
  // gets by filling a table in and forgetting one channel -- and it must not be mistaken for "no filter", or a
  // forgotten channel would silently pass its values through instead of showing up as a black channel.
  const ColorTable zeroed;
  EXPECT_FALSE(zeroed.isIdentity()) << "an all-zero table is a filter that produces black, not the identity";

  // And the identity is not the default.
  EXPECT_NE(zeroed, ColorTable::Identity());
}

TEST(ColorTableTest, ASingleChannelTableIsAppliedToEveryChannel) {
  std::array<uint8_t, 256> inverted {};
  for (size_t i = 0; i < 256; ++i) inverted[i] = static_cast<uint8_t>(255 - i);

  const ColorTable table = ColorTable::FromSingleChannel(inverted);
  EXPECT_FALSE(table.isIdentity());
  for (size_t i = 0; i < 256; ++i) {
    EXPECT_EQ(table.alpha[i], inverted[i]);
    EXPECT_EQ(table.red[i], inverted[i]);
    EXPECT_EQ(table.green[i], inverted[i]);
    EXPECT_EQ(table.blue[i], inverted[i]);
  }
}

TEST(ColorTableTest, ChangingOneChannelIsNotTheIdentity) {
  // The check has to look at all four tables. A version that only compared the red one would call this the identity,
  // and a caller that optimised the filter away would lose the alpha curve.
  ColorTable table = ColorTable::Identity();
  table.alpha[128] = 0;
  EXPECT_FALSE(table.isIdentity()) << "a modified alpha channel is not the identity";

  ColorTable blueChanged = ColorTable::Identity();
  blueChanged.blue[0] = 200;
  EXPECT_FALSE(blueChanged.isIdentity()) << "a modified blue channel is not the identity";
}

TEST(ColorTableTest, TablesCompareByTheirContents) {
  const ColorTable a = ColorTable::Identity();
  ColorTable b = ColorTable::Identity();
  EXPECT_EQ(a, b);

  b.green[7] = 9;
  EXPECT_NE(a, b) << "one entry differing is a different table";
}

// ==================================================================================================
// DataTable
// ==================================================================================================

TEST(DataTableTest, AnEmptyTableIsValidAndEmpty) {
  const DataTable table;
  EXPECT_TRUE(table.isEmpty());
  EXPECT_EQ(table.count(), 0u);

  // A lookup on an empty table is answered rather than undefined, because a caller in a loop should not have to
  // check the count before every access.
  EXPECT_TRUE(table.at(0).empty());
  EXPECT_EQ(table.atSize(0), 0u);
  EXPECT_TRUE(table.atText(0).empty());
}

TEST(DataTableTest, BuildsFromAFixedSizeArray) {
  const std::array<uint32_t, 4> values { 10, 20, 30, 40 };
  const DataTable table = DataTable::FromArray(values.data(), sizeof(uint32_t), values.size());

  ASSERT_EQ(table.count(), 4u);
  EXPECT_FALSE(table.isEmpty());
  EXPECT_EQ(table.elementSize(), sizeof(uint32_t));

  for (size_t i = 0; i < values.size(); ++i) {
    ASSERT_EQ(table.atSize(i), sizeof(uint32_t));
    uint32_t read = 0;
    std::memcpy(&read, table.at(i).data(), sizeof(uint32_t));
    EXPECT_EQ(read, values[i]) << "entry " << i;
  }
}

TEST(DataTableTest, AFixedSizeTableReturnsASliceOfTheBuffer) {
  // The point of a fixed-size table is that an entry is a view into the buffer rather than a copy. A caller that
  // modified what it was given afterwards must not change what the table reports, though, because the table copied.
  std::vector<uint8_t> source { 1, 2, 3, 4, 5, 6 };
  const DataTable table = DataTable::FromArray(source.data(), 2, 3);
  ASSERT_EQ(table.count(), 3u);

  EXPECT_EQ(table.at(0).size(), 2u);
  EXPECT_EQ(static_cast<uint8_t>(table.at(0)[0]), 1);
  EXPECT_EQ(static_cast<uint8_t>(table.at(1)[0]), 3);
  EXPECT_EQ(static_cast<uint8_t>(table.at(2)[0]), 5);

  source[0] = 99;
  EXPECT_EQ(static_cast<uint8_t>(table.at(0)[0]), 1) << "the table owns its bytes, so the source cannot change it";
}

TEST(DataTableTest, BuildsFromEntriesOfDifferentSizes) {
  const std::string first = "alpha";
  const std::string second = "be";
  const std::string third = "a much longer entry than the others";

  const void* pointers[] = { first.data(), second.data(), third.data() };
  const size_t sizes[] = { first.size(), second.size(), third.size() };
  const DataTable table = DataTable::FromEntries(pointers, sizes, 3);

  ASSERT_EQ(table.count(), 3u);
  EXPECT_EQ(table.elementSize(), 0u) << "a mixed-size table has no single element size";
  EXPECT_EQ(table.at(0), first);
  EXPECT_EQ(table.at(1), second);
  EXPECT_EQ(table.at(2), third) << "the last entry is bounded by the end of the buffer, not by a following offset";
}

TEST(DataTableTest, AnIndexPastTheEndIsEmptyRatherThanFatal) {
  const std::array<uint8_t, 4> values { 1, 2, 3, 4 };
  const DataTable table = DataTable::FromArray(values.data(), 1, 4);
  ASSERT_EQ(table.count(), 4u);

  // Just past the end, and far past it. Both are answered with an empty view, which cannot be read past -- this is the
  // check that a table reading from the wrong offset would fail, and it is why the accessor is checked at all.
  EXPECT_TRUE(table.at(4).empty());
  EXPECT_TRUE(table.at(1000).empty());
  EXPECT_EQ(table.atSize(4), 0u);
}

TEST(DataTableTest, AnEntryOfZeroLengthDoesNotShiftTheOthers) {
  // A zero-length entry is legal and has to occupy a slot rather than being skipped, or every index after it would
  // address the wrong bytes. This is the case a table built from optional records hits.
  const std::string second = "second";
  const void* pointers[] = { nullptr, second.data(), nullptr };
  const size_t sizes[] = { 0, second.size(), 0 };
  const DataTable table = DataTable::FromEntries(pointers, sizes, 3);

  ASSERT_EQ(table.count(), 3u) << "a zero-length entry is still an entry";
  EXPECT_TRUE(table.at(0).empty());
  EXPECT_EQ(table.at(1), second);
  EXPECT_TRUE(table.at(2).empty());
}

TEST(DataTableTest, BuildsNothingFromNothing) {
  EXPECT_TRUE(DataTable::FromArray(nullptr, 4, 4).isEmpty());
  EXPECT_TRUE(DataTable::FromArray(nullptr, 0, 4).isEmpty());
  EXPECT_TRUE(DataTable::FromEntries(nullptr, nullptr, 4).isEmpty());

  // A count of zero is an empty table however valid the pointers are.
  const std::array<uint8_t, 4> values { 1, 2, 3, 4 };
  EXPECT_TRUE(DataTable::FromArray(values.data(), 1, 0).isEmpty());

  const void* pointers[] = { values.data() };
  const size_t sizes[] = { values.size() };
  EXPECT_TRUE(DataTable::FromEntries(pointers, sizes, 0).isEmpty());
}

TEST(DataTableTest, TextEntriesAreReportedWithoutTheirTerminator) {
  // An entry built from a C string carries its terminator in the stored length, because that is what the caller
  // passed. Reporting it back would give text with a stray character that compares unequal to the original for no
  // visible reason.
  const char* text = "hello";
  const void* pointers[] = { text };
  const size_t sizes[] = { 6 };  // including the terminator
  const DataTable table = DataTable::FromEntries(pointers, sizes, 1);

  ASSERT_EQ(table.count(), 1u);
  EXPECT_EQ(table.at(0).size(), 6u) << "the bytes are what was copied in";
  EXPECT_EQ(table.atText(0), "hello") << "but the text drops the terminator";
  EXPECT_EQ(table.atText(0).size(), 5u);
}
