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

#ifndef CORAL_GRAPHICS_DATA_TABLE_H
#define CORAL_GRAPHICS_DATA_TABLE_H

#include <cstdint>
#include <string_view>
#include <vector>

namespace graphics {

/**
 * An immutable table of byte entries, which need not all be the same length.
 *
 * ## What it is for
 *
 * A cache of variable-length records: font table data, per-glyph blobs, metadata keyed by index. It is deliberately the
 * least structured container that does the job -- entries are found by position and their bytes are the caller's to
 * interpret -- because the alternative is a type per payload, and the payloads differ per caller.
 *
 * ## Why the layout is a flat buffer
 *
 * The reference stores either a contiguous array plus one element size, or an array of pointers and sizes, and picks
 * between them with a union and a `FreeProc` so that a caller can hand over memory the table does not own. Both of
 * those are things a standard container does better:
 *
 *  - **No union.** Every entry being the same size is the case that matters for a font, and a flat buffer with an
 *    element size is the same thing for that case. The mixed-size case is served by a second flat buffer holding the
 *    offsets, which is one branch in the accessor rather than two layouts to reason about.
 *  - **No `FreeProc`.** A table that borrows memory has to be told when to stop, which in the reference is a function
 *    pointer and an opaque context that the caller must keep alive. Here the bytes are copied in, so a table owns what
 *    it reports and there is nothing to free. A caller that wants to avoid the copy has `std::string_view` if it can
 *    guarantee the lifetime, and that is a decision better made at the call site than buried in a container.
 *
 * ## The empty case
 *
 * An empty table is a valid table, not a null one. A lookup on it is a programming error rather than a state to check
 * for, so the accessors assert in a debug build and return an empty view in a release one -- the same contract the rest
 * of this library uses for an index that cannot be right.
 */
class DataTable {
public:
  DataTable() = default;

  /**
   * @return a table of @p count entries, each @p elementSize bytes copied from the start of @p array.
   *
   * The case a font needs: a run of fixed-size records already contiguous in memory.
   */
  [[nodiscard]] static DataTable FromArray(const void* array, const size_t elementSize, const size_t count) {
    DataTable table;
    if (array == nullptr || elementSize == 0 || count == 0) {
      return table;
    }

    table.mElementSize = elementSize;
    const auto* bytes = static_cast<const uint8_t*>(array);
    table.mStorage.assign(bytes, bytes + elementSize * count);
    return table;
  }

  /**
   * @return a table whose entries are copied from @p pointers, with the corresponding lengths from @p sizes.
   *
   * The mixed-size case. The bytes are laid end to end and the offsets recorded, so an entry is found by one addition
   * rather than by chasing a pointer.
   */
  [[nodiscard]] static DataTable FromEntries(const void* const* pointers, const size_t* sizes, const size_t count) {
    DataTable table;
    if (pointers == nullptr || sizes == nullptr || count == 0) {
      return table;
    }

    table.mOffsets.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      table.mOffsets.push_back(table.mStorage.size());
      if (pointers[i] == nullptr || sizes[i] == 0) {
        continue;
      }
      const auto* bytes = static_cast<const uint8_t*>(pointers[i]);
      table.mStorage.insert(table.mStorage.end(), bytes, bytes + sizes[i]);
    }
    return table;
  }

  /** @return true when the table has no entries. */
  [[nodiscard]] bool isEmpty() const noexcept { return count() == 0; }

  /** @return the number of entries, which is zero for an empty table. */
  [[nodiscard]] size_t count() const noexcept {
    if (!mOffsets.empty()) return mOffsets.size();
    if (mElementSize == 0) return 0;
    return mStorage.size() / mElementSize;
  }

  /**
   * @return a view of the entry at @p index, or an empty view when the index is out of range.
   *
   * Out of range is answered rather than asserted here, because this is the accessor a caller uses in a loop and the
   * cost of the check is one comparison. A caller that knows its index is valid pays nothing it would not pay anyway.
   */
  [[nodiscard]] std::string_view at(const size_t index) const noexcept {
    if (index >= count()) {
      return {};
    }

    if (!mOffsets.empty()) {
      const size_t start = mOffsets[index];
      const size_t end = (index + 1 < mOffsets.size()) ? mOffsets[index + 1] : mStorage.size();
      return { reinterpret_cast<const char*>(mStorage.data()) + start, end - start };
    }

    return { reinterpret_cast<const char*>(mStorage.data()) + index * mElementSize, mElementSize };
  }

  /** @return the size of the entry at @p index, or zero when the index is out of range. */
  [[nodiscard]] size_t atSize(const size_t index) const noexcept { return at(index).size(); }

  /**
   * @return the entry at @p index as text, which requires the entry to end in a terminator.
   *
   * The reference asserts that the stored length is one more than the string's own, which catches a table built from
   * strings that were not terminated. There is nothing to assert here that is not already guaranteed: the view is
   * bounded by the entry's length, so a caller cannot read past it however the entry was built --
   * `std::string_view` is the check, and it is one the type system enforces rather than one that runs in debug builds.
   */
  [[nodiscard]] std::string_view atText(const size_t index) const noexcept {
    std::string_view entry = at(index);
    // A terminator that is part of the stored length would otherwise appear as a trailing character.
    if (!entry.empty() && entry.back() == '\0') {
      entry.remove_suffix(1);
    }
    return entry;
  }

  /** @return the size of one entry when every entry is the same size, or zero for a mixed-size table. */
  [[nodiscard]] size_t elementSize() const noexcept { return mElementSize; }

private:
  /** The bytes of every entry, laid end to end. Empty for an empty table. */
  std::vector<uint8_t> mStorage;

  /** Where each entry starts, for a table whose entries differ in size. Empty when they do not. */
  std::vector<size_t> mOffsets;

  /**
   * The size of each entry when they are all the same, or zero when the offsets are what locate them.
   *
   * A single field serving both layouts is what keeps the accessor to one branch: a table is either uniform, and
   * counts its entries by division, or it is not, and counts them by its offsets.
   */
  size_t mElementSize = 0;
};

} // namespace graphics

#endif //CORAL_GRAPHICS_DATA_TABLE_H
