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

#include "test_asserts.h"

#include <backend/Handles.h>

#include <string>
#include <vector>

using namespace backend;

namespace {

// A payload type that tracks its own lifetime, so the pool's construction/destruction behaviour is
// observable rather than assumed.
struct Tracked {
  int value = 0;
  static int constructions;
  static int destructions;

  Tracked() { ++constructions; }
  explicit Tracked(int v)
          : value(v) {
    ++constructions;
  }
  Tracked(const Tracked& other)
          : value(other.value) {
    ++constructions;
  }
  Tracked(Tracked&& other) noexcept
          : value(other.value) {
    other.value = -1;
    ++constructions;
  }
  Tracked& operator=(const Tracked&) = default;
  Tracked& operator=(Tracked&&) noexcept = default;
  ~Tracked() { ++destructions; }
};

int Tracked::constructions = 0;
int Tracked::destructions = 0;

TEST(Handles, InvalidHandleIsFalsy) {
  const TextureHandle handle;
  EXPECT_FALSE(handle.valid());
  EXPECT_EQ(handle.generation, 0u);
  EXPECT_EQ(handle, TextureHandle::invalid());
}

TEST(Handles, AllocateReturnsDistinctLiveHandles) {
  HandlePool<TextureTag, int> pool;

  const TextureHandle first = pool.allocate(10);
  const TextureHandle second = pool.allocate(20);
  const TextureHandle third = pool.allocate(30);

  EXPECT_TRUE(first.valid());
  EXPECT_TRUE(second.valid());
  EXPECT_TRUE(third.valid());
  EXPECT_NE(first, second);
  EXPECT_NE(second, third);
  EXPECT_EQ(pool.liveCount(), 3u);

  ASSERT_NE(pool.get(first), nullptr);
  EXPECT_EQ(*pool.get(first), 10);
  EXPECT_EQ(*pool.get(second), 20);
  EXPECT_EQ(*pool.get(third), 30);
}

TEST(Handles, StaleHandleIsRejectedAfterFree) {
  HandlePool<TextureTag, int> pool;

  const TextureHandle handle = pool.allocate(7);
  ASSERT_TRUE(pool.isAlive(handle));
  EXPECT_TRUE(pool.free(handle));

  // The whole point of the generation counter: a handle that outlives its resource must fail
  // validation rather than silently aliasing whatever occupies the slot next.
  EXPECT_FALSE(pool.isAlive(handle));
  EXPECT_EQ(pool.get(handle), nullptr);
  EXPECT_FALSE(pool.free(handle)) << "a second free must be rejected, not silently accepted";

  // The slot is recycled, and the new generation must differ from the old one.
  const TextureHandle recycled = pool.allocate(8);
  EXPECT_NE(recycled, handle);
  EXPECT_TRUE(pool.isAlive(recycled));
  EXPECT_FALSE(pool.isAlive(handle)) << "the stale handle must stay invalid after reuse";
  EXPECT_EQ(*pool.get(recycled), 8);
}

TEST(Handles, FreeListReusesSlotsAndBumpsGeneration) {
  HandlePool<BufferTag, int> pool;

  std::vector<BufferHandle> handles;
  for (int index = 0; index < 8; ++index) handles.push_back(pool.allocate(int(index)));
  EXPECT_EQ(pool.capacity(), 8u);
  EXPECT_EQ(pool.liveCount(), 8u);

  for (const BufferHandle& handle: handles) EXPECT_TRUE(pool.free(handle));
  EXPECT_EQ(pool.liveCount(), 0u);
  EXPECT_EQ(pool.capacity(), 8u) << "the pool must not shrink";

  // Reallocation reuses the freed slots rather than growing the pool, and each new handle carries a
  // higher generation than the one it replaced.
  std::vector<BufferHandle> reused;
  for (int index = 0; index < 8; ++index) reused.push_back(pool.allocate(100 + index));
  EXPECT_EQ(pool.capacity(), 8u) << "reallocation must reuse freed slots";

  for (const BufferHandle& handle: reused) EXPECT_TRUE(pool.isAlive(handle));
  for (const BufferHandle& handle: handles) EXPECT_FALSE(pool.isAlive(handle));
}

TEST(Handles, OutOfRangeHandleIsRejected) {
  HandlePool<TextureTag, int> pool;
  const TextureHandle real = pool.allocate(1);

  // A handle whose index was never allocated must not reach into the vector.
  const TextureHandle fabricated(9999, real.generation);
  EXPECT_FALSE(pool.isAlive(fabricated));
  EXPECT_EQ(pool.get(fabricated), nullptr);
}

TEST(Handles, ForEachVisitsOnlyLiveEntries) {
  HandlePool<TextureTag, int> pool;
  const TextureHandle a = pool.allocate(1);
  const TextureHandle b = pool.allocate(2);
  const TextureHandle c = pool.allocate(3);

  pool.free(b);

  int sum = 0;
  int visited = 0;
  pool.forEach([&](TextureHandle, int& payload) {
    sum += payload;
    ++visited;
  });

  EXPECT_EQ(visited, 2);
  EXPECT_EQ(sum, 1 + 3);
  EXPECT_TRUE(pool.isAlive(a));
  EXPECT_TRUE(pool.isAlive(c));
}

TEST(Handles, PayloadLifetimeIsManaged) {
  Tracked::constructions = 0;
  Tracked::destructions = 0;

  {
    HandlePool<TextureTag, Tracked> pool;
    const TextureHandle handle = pool.allocate(Tracked(42));
    EXPECT_EQ(pool.get(handle)->value, 42);
    pool.free(handle);
  }

  // Every construction must have a matching destruction: a pool that leaked payloads would leak
  // driver objects in a real backend, because the payload owns them.
  EXPECT_EQ(Tracked::constructions, Tracked::destructions);
}

TEST(Handles, HandlePackingRoundTrips) {
  const TextureHandle handle(1234, 5678);
  EXPECT_EQ(handle.index, 1234u);
  EXPECT_EQ(handle.generation, 5678u);
  EXPECT_TRUE(handle.valid());

  // Two handles that agree on index and generation but were built separately must compare equal:
  // handles are recorded into command lists by value, so equality has to be structural.
  const TextureHandle copy(1234, 5678);
  EXPECT_EQ(handle, copy);
  EXPECT_FALSE(handle < copy);
  EXPECT_FALSE(copy < handle);

  // Handles are trivially copyable because they end up inside command buffers that are memcpy'd
  // between threads.
  static_assert(std::is_trivially_copyable_v<TextureHandle>,
          "handles must stay trivially copyable to be recorded into command buffers");
  static_assert(sizeof(TextureHandle) <= sizeof(uint64_t),
          "two 24 bit counters must fit in 64 bits, so a handle cannot grow beyond that");
}

TEST(Handles, DistinctTagsProduceDistinctTypes) {
  // The tag parameter is what makes a texture handle unusable where a buffer handle is expected.
  // That property is a compile-time guarantee, so it can only be asserted structurally here.
  static_assert(!std::is_same_v<TextureHandle, BufferHandle>);
  static_assert(!std::is_same_v<TextureHandle, TextureViewHandle>);
  static_assert(!std::is_same_v<NativeTexture, NativeBuffer>);

  TextureHandle texture;
  BufferHandle buffer;
  EXPECT_EQ(texture, TextureHandle::invalid());
  EXPECT_EQ(buffer, BufferHandle::invalid());
}

TEST(Handles, NativeObjectValidity) {
  NativeTexture native;
  EXPECT_FALSE(native.valid());
  EXPECT_FALSE(static_cast<bool>(native));

  native.object = reinterpret_cast<void*>(0x1234);
  EXPECT_TRUE(native.valid());
  EXPECT_TRUE(static_cast<bool>(native));
}

} // namespace anonymous
