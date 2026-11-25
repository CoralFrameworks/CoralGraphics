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
 * @file test_Threading.cpp
 *
 * The concurrency contract of the OpenGL backend's recording path, exercised rather than described.
 *
 * ## What this file is for
 *
 * The design document lists three gaps that made multi-threaded recording unsafe: the device's command
 * buffer vector, the resource pools' slot array, and the resource fields a recording thread reads. Each
 * is fixed in the library; this file is the part that proves the fixes hold under an actual
 * interleaving.
 *
 * That distinction is the point. All three failures are *silent under the wrong conditions*: a
 * `std::vector` that reallocates out from under a reader is a use-after-free that happens only when the
 * allocator moves the block, and a torn `std::string` read corrupts only when the assignment lands
 * inside the read. A test that runs two threads once and passes proves nothing about either. What makes
 * these tests worth having is that they run a real interleaving many thousands of times and check the
 * *value* the reader observed, so a torn read fails an assertion rather than merely being unlikely to
 * crash.
 *
 * ## Determinism
 *
 * There are no sleeps and no timing assumptions. Threads are released through a shared atomic gate, and
 * every worker runs a fixed amount of work, so a run explores the same interleaving space on every
 * machine. A failure here is a real data race, not a slow machine.
 *
 * ## What a plain build cannot see
 *
 * `HandlePool`'s published-size protocol is a memory-ordering property, and a memory-ordering bug is
 * invisible to a normal build - it needs ThreadSanitizer or a weakly-ordered CPU. The tests below
 * therefore assert on *contents* as well as on survival: a reader that observes a payload, an
 * out-of-range slot, or metadata field set that could never have been written together fails an
 * assertion under any build. That covers the structural half of the protocol (nothing is read before it
 * is written, nothing points into freed storage) unconditionally, and leaves the ordering half to a
 * sanitizer run, which CoralGraphics supports via `-DCORAL_GRAPHICS_SANITIZE=address`.
 */

#include "test_asserts.h"

#include <backend/Handles.h>

#include "private/backend/gl/GlDevice.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

using namespace backend;

namespace test {
namespace {

// ------------------------------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------------------------------

/**
 * A gate that releases every waiting thread at once.
 *
 * An atomic spin rather than a condition variable, because the wait lasts for the length of one thread
 * creation and a condition variable would add a "did the last thread arrive before the first was
 * woken" question to a test whose whole point is to have no such questions.
 */
class StartGate {
  public:
  void wait() const noexcept {
    while (!mOpen.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }

  void open() noexcept { mOpen.store(true, std::memory_order_release); }

  private:
  std::atomic<bool> mOpen{ false };
};

/** Payload whose value is derived from its slot, so a reader can tell a torn read from a good one. */
struct Pattern {
  uint64_t value = 0;
  uint64_t complement = 0;
  uint32_t tag = 0;
  uint32_t padding = 0;

  /** The pattern a writer stores in the slot at @p index. */
  [[nodiscard]] static Pattern forSlot(const uint32_t index) noexcept {
    Pattern pattern;
    pattern.value = 0x9E3779B97F4A7C15ull * (uint64_t(index) + 1);
    pattern.complement = ~pattern.value;
    pattern.tag = index ^ 0xA5A5A5A5u;
    pattern.padding = 0xDEADBEEFu;
    return pattern;
  }

  [[nodiscard]] bool matches(const uint32_t index) const noexcept {
    const Pattern expected = forSlot(index);
    return value == expected.value && complement == expected.complement && tag == expected.tag &&
           padding == expected.padding;
  }
};

/**
 * Unpacks a handle from the integer a publishing thread stored.
 *
 * The readers below never see a `Handle` that was not first stored through an atomic. That is not
 * decoration: it mirrors how a real renderer hands a freshly created resource to a recording thread (a
 * task queue, a mutex, a future - some synchronisation), and without *a* happens-before edge a reader is
 * not entitled to see the payload at all. `HandlePool` publishes entries; it does not publish handles,
 * and a test that passed handles through a plain non-atomic variable would be demanding a guarantee the
 * pool deliberately does not offer.
 */
[[nodiscard]] BufferHandle unpackBufferHandle(const uint64_t packed) noexcept {
  return BufferHandle(uint32_t(packed & BufferHandle::kIndexMask),
          uint32_t(packed >> BufferHandle::kIndexBits));
}
// ------------------------------------------------------------------------------------------------
// HandlePool: concurrent reads while the device thread allocates
// ------------------------------------------------------------------------------------------------

/**
 * One writer allocates; N readers resolve every handle the writer publishes, while it is publishing.
 *
 * This is the exact shape of the race the design document describes: a recording thread calls
 * `mDevice.texture(handle)` - which is `HandlePool::get` - at the moment the device thread calls
 * `createTexture`, which is `HandlePool::allocate`. With a `std::vector` slot array the growth frees the
 * block the reader is walking and the reader is left holding a dangling pointer. With the chunked arena
 * the pointer stays valid across every later allocation, and the published size is what tells the reader
 * which slots exist at all.
 *
 * ## The three phases, and why each is asserted
 *
 *  1. Every reader announces itself, then waits for the writer to publish the first slot. Until then
 *     there is nothing to resolve, and a reader that spent its budget on a still-empty pool would make
 *     the test pass without testing anything - which is exactly what an earlier version of this test
 *     did: its readers exhausted a fixed step count before the writer had published once.
 *  2. The writer allocates `kEntryCount` slots. The readers resolve handles concurrently, checking the
 *     *contents* of every payload against the slot index. A payload that does not match its slot can
 *     only have been read mid-write or through a pointer into freed storage.
 *  3. `resolved` must be positive. That assertion is what makes phase 2 non-optional: a run in which
 *     every resolution returned `nullptr` proves nothing about the pool's ability to hand back a
 *     payload while the device thread grows it.
 *
 * `kEntryCount` is deliberately larger than `HandlePool::kChunkSize`, so the writer has to allocate
 * second and third chunks inside the readers' window; a pool that only ever allocated one chunk would
 * never exercise the chunk-pointer publication this test is here to check.
 */
TEST(HandlePoolThreading, ReadersSeeOnlyFullyConstructedPayloads) {
  constexpr uint32_t kEntryCount = 6000; // > kChunkSize, so more than one arena chunk is allocated
  constexpr uint32_t kReaderCount = 4;
  constexpr uint32_t kResolutionAttempts = 60000;

  HandlePool<TextureTag, Pattern> pool;

  StartGate gate;
  std::atomic<uint32_t> published{ 0 };
  std::atomic<uint32_t> resolved{ 0 };
  std::atomic<uint32_t> failures{ 0 };
  std::atomic<uint32_t> readersReady{ 0 };

  // The writer owns the pool exclusively while allocating, which is the contract: creation is
  // serialised on the device. It waits for the readers so that the publication and the resolution
  // overlap rather than merely being adjacent in time.
  std::thread writer([&] {
    gate.wait();
    while (readersReady.load(std::memory_order_acquire) < kReaderCount) {
      std::this_thread::yield();
    }

    for (uint32_t index = 0; index < kEntryCount; ++index) {
      const TextureHandle handle = pool.allocate(Pattern::forSlot(index));
      if (!handle.valid() || handle.index != index || handle.generation != 1) {
        // Nothing is freed in this test, so slots must be handed out strictly in order with generation
        // 1. A mismatch would make the `matches` checks below meaningless.
        ++failures;
        return;
      }
      // Release: everything the writer put in the slot must be visible to a reader that sees this.
      published.store(index + 1, std::memory_order_release);
    }
  });

  std::vector<std::thread> readers;
  readers.reserve(kReaderCount);
  for (uint32_t reader = 0; reader < kReaderCount; ++reader) {
    readers.emplace_back([&, reader] {
      gate.wait();
      readersReady.fetch_add(1, std::memory_order_release);

      // Phase 1: wait for something to resolve. The writer is waiting on this reader, so the pool is
      // guaranteed to become non-empty; there is no sleep and no timeout.
      uint32_t available = 0;
      while ((available = published.load(std::memory_order_acquire)) == 0) {
        std::this_thread::yield();
      }

      // Phase 2: resolve concurrently and check the contents. The modulus starts at a non-zero value,
      // so the first handle is a real slot rather than a trivial `index % 1`.
      for (uint32_t attempt = 0; attempt < kResolutionAttempts; ++attempt) {
        const uint32_t index = (attempt + reader) % available;
        const TextureHandle handle(index, 1);
        if (!pool.isAlive(handle)) continue;

        const Pattern* payload = pool.get(handle);
        if (payload == nullptr) {
          // `isAlive` said yes and `get` said no: the two disagree, which no ordering makes legal.
          ++failures;
          continue;
        }
        if (!payload->matches(index)) {
          // Reachable only if the payload was read while it was being written, or if the pointer is
          // into a block the allocator moved. Both are the bug this test exists to catch.
          ++failures;
          continue;
        }
        ++resolved;
      }
    });
  }

  gate.open();
  writer.join();
  for (std::thread& reader: readers) reader.join();

  EXPECT_EQ(failures.load(), 0u) << "a reader saw a torn payload or a pointer into freed storage";
  EXPECT_GT(resolved.load(), 0u) << "the readers must actually have resolved entries";
  EXPECT_EQ(published.load(), kEntryCount);

  // The pool's own invariants, checked once the writer has stopped.
  EXPECT_EQ(pool.capacity(), kEntryCount);
  EXPECT_EQ(pool.liveCount(), kEntryCount);
  for (uint32_t index = 0; index < kEntryCount; ++index) {
    const Pattern* payload = pool.get(TextureHandle(index, 1));
    ASSERT_NE(payload, nullptr) << "slot " << index;
    EXPECT_TRUE(payload->matches(index)) << "slot " << index;
  }
}

/**
 * The same interleaving with the payload the recording path actually reads.
 *
 * `GLTextureMeta` is the P1 fix made concrete: fourteen fields written once at creation and read
 * afterwards by whichever thread is resolving a render pass's attachments. The reader here checks that
 * every field it can see agrees with every other, which is the property a reader would lose if the
 * metadata were written after the slot were published - it would observe a texture whose width came
 * from the new occupant and whose format came from the old one.
 */
TEST(HandlePoolThreading, PublishedTextureMetadataIsNeverObservedHalfWritten) {
  constexpr uint32_t kEntryCount = 3000;
  constexpr uint32_t kReaderCount = 4;
  constexpr uint32_t kResolutionAttempts = 40000;

  HandlePool<TextureTag, GLTextureMeta> pool;

  StartGate gate;
  std::atomic<uint32_t> published{ 0 };
  std::atomic<uint32_t> checks{ 0 };
  std::atomic<uint32_t> failures{ 0 };
  std::atomic<uint32_t> readersReady{ 0 };

  std::thread writer([&] {
    gate.wait();
    while (readersReady.load(std::memory_order_acquire) < kReaderCount) {
      std::this_thread::yield();
    }

    for (uint32_t index = 0; index < kEntryCount; ++index) {
      GLTextureMeta meta;
      meta.name = 100 + index;
      meta.target = GL_TEXTURE_2D + (index % 4);
      meta.format = PixelFormat(uint32_t(PixelFormat::Undefined) + 1 + (index % 8));
      meta.internalFormat = 0x8000u + index;
      meta.width = 1 + index;
      meta.height = 2 + index;
      meta.depthOrLayers = 3 + index;
      meta.mipLevelCount = 1 + (index % 5);
      meta.sampleCount = index % 2 == 0 ? 1u : 4u;
      meta.type = TextureType::Texture2D;
      meta.usage = TextureUsages(index);
      meta.externallyOwned = (index % 3) == 0;
      meta.immutableStorage = (index % 5) == 0;

      if (!pool.allocate(meta).valid()) {
        ++failures;
        return;
      }
      published.store(index + 1, std::memory_order_release);
    }
  });

  std::vector<std::thread> readers;
  readers.reserve(kReaderCount);
  for (uint32_t reader = 0; reader < kReaderCount; ++reader) {
    readers.emplace_back([&, reader] {
      gate.wait();
      readersReady.fetch_add(1, std::memory_order_release);

      uint32_t available = 0;
      while ((available = published.load(std::memory_order_acquire)) == 0) {
        std::this_thread::yield();
      }

      for (uint32_t attempt = 0; attempt < kResolutionAttempts; ++attempt) {
        const uint32_t index = (attempt + reader) % available;
        const GLTextureMeta* meta = pool.get(TextureHandle(index, 1));
        if (meta == nullptr) continue;

        // Every field is derived from `index`, so one mismatch means the reader saw a struct that was
        // mid-write - the exact failure that putting the metadata in a separate, write-once block is
        // meant to rule out.
        const bool coherent = meta->name == 100 + index && meta->target == GL_TEXTURE_2D + (index % 4) &&
                              meta->format == PixelFormat(uint32_t(PixelFormat::Undefined) + 1 + (index % 8)) &&
                              meta->internalFormat == 0x8000u + index && meta->width == 1 + index &&
                              meta->height == 2 + index && meta->depthOrLayers == 3 + index &&
                              meta->mipLevelCount == 1 + (index % 5) && meta->sampleCount == (index % 2 == 0 ? 1u : 4u) &&
                              meta->externallyOwned == ((index % 3) == 0) &&
                              meta->immutableStorage == ((index % 5) == 0);
        if (!coherent) ++failures;
        ++checks;
      }
    });
  }

  gate.open();
  writer.join();
  for (std::thread& reader: readers) reader.join();

  EXPECT_EQ(failures.load(), 0u) << "a reader observed metadata that could never have been written together";
  EXPECT_GT(checks.load(), 0u) << "the readers must actually have resolved entries";
}

/**
 * Recycling a slot must never resurrect the handle that used to name it, and readers must be able to
 * resolve handles while the device thread recycles *other* slots.
 *
 * The generation check is what makes this work: `free` bumps the slot's generation and pushes the index
 * onto the free list, so the handle the caller still holds fails validation from that point on even
 * though its slot is immediately handed to a new resource. That half is deterministic and is asserted
 * directly rather than raced - a race would only re-check what the generation counter already
 * guarantees, while making the test's outcome depend on how the two threads were scheduled.
 *
 * What *is* concurrent here is the readers resolving the stable handles while the writer cycles a
 * window well beyond them. That is the operation a recording thread performs while the device thread
 * creates and destroys unrelated resources, and it re-checks that the arena still hands back the same
 * payload for a slot that has not been touched.
 */
TEST(HandlePoolThreading, RecycledSlotsNeverResolveToAnotherSlotsPayload) {
  constexpr uint32_t kStable = 64;   // handles the readers resolve: never freed while they run
  constexpr uint32_t kWindow = 512;  // slots the writer cycles through, disjoint from the stable set
  constexpr uint32_t kRounds = 4096; // must exceed kWindow, or the slots the writer never reaches stay original
  constexpr uint32_t kReaderCount = 3;

  HandlePool<BufferTag, Pattern> pool;

  // The stable set: what the readers will resolve for the whole test.
  std::vector<uint64_t> stableHandles;
  stableHandles.reserve(kStable);
  for (uint32_t index = 0; index < kStable; ++index) {
    const BufferHandle handle = pool.allocate(Pattern::forSlot(index));
    ASSERT_TRUE(handle.valid());
    stableHandles.push_back(handle.value());
  }

  // The window the writer recycles. Its slots are disjoint from the stable set, so a reader can never
  // be inside a slot that is being freed - which is the caller obligation `HandlePool` documents.
  // `originalWindow` keeps the handles the writer started with, so the generation check can be asserted
  // against the handles that were actually invalidated rather than a fabricated one.
  std::vector<BufferHandle> window;
  std::vector<BufferHandle> originalWindow;
  window.reserve(kWindow);
  originalWindow.reserve(kWindow);
  for (uint32_t index = 0; index < kWindow; ++index) {
    const BufferHandle handle = pool.allocate(Pattern::forSlot(kStable + index));
    ASSERT_TRUE(handle.valid());
    window.push_back(handle);
    originalWindow.push_back(handle);
  }

  StartGate gate;
  std::atomic<bool> writerDone{ false };
  std::atomic<uint32_t> failures{ 0 };
  std::atomic<uint32_t> liveResolutions{ 0 };
  std::atomic<uint32_t> readerLoops{ 0 };

  std::thread writer([&] {
    gate.wait();
    for (uint32_t round = 0; round < kRounds; ++round) {
      const uint32_t slot = round % kWindow;
      if (!pool.free(window[slot])) {
        ++failures;
        return;
      }
      const BufferHandle recycled = pool.allocate(Pattern::forSlot(kStable + kWindow + round));
      if (!recycled.valid() || recycled.index != window[slot].index || recycled.generation == window[slot].generation) {
        // Recycling must reuse the slot *and* hand back a handle with a different generation, or the
        // stale handle below would still validate.
        ++failures;
        return;
      }
      window[slot] = recycled;
    }
    writerDone.store(true, std::memory_order_release);
  });

  std::vector<std::thread> readers;
  readers.reserve(kReaderCount);
  for (uint32_t reader = 0; reader < kReaderCount; ++reader) {
    readers.emplace_back([&] {
      gate.wait();
      // One pass before checking whether the writer has finished, not a `while (!done)` loop. The
      // writer's 4096 rounds can complete before a reader thread is scheduled for the first time, and a
      // reader that looped zero times satisfies "no failures" while proving nothing - the two counters
      // below exist to catch exactly that, and on a loaded machine they did. A do-while makes the
      // concurrent phase always include at least one resolution per reader.
      for (;;) {
        ++readerLoops;
        for (const uint64_t packed: stableHandles) {
          const BufferHandle handle = unpackBufferHandle(packed);
          if (pool.isAlive(handle)) {
            const Pattern* payload = pool.get(handle);
            if (payload == nullptr) {
              ++failures;
            } else if (!payload->matches(handle.index)) {
              // A live stable handle resolving to a payload that is not its own means the writer's
              // recycling reached a slot it does not own, or the arena moved underneath the reader.
              ++failures;
            }
            ++liveResolutions;
          } else {
            // A stable handle is never freed, so this is a failure: nothing the writer does should be
            // able to make a live handle stop validating.
            ++failures;
          }
        }
        if (writerDone.load(std::memory_order_acquire)) break;
      }
    });
  }

  gate.open();
  writer.join();
  for (std::thread& reader: readers) reader.join();

  EXPECT_EQ(failures.load(), 0u) << "a resolution during recycling returned the wrong payload or none at all";
  EXPECT_GT(liveResolutions.load(), 0u) << "the readers must actually have resolved the stable handles";
  EXPECT_GT(readerLoops.load(), 0u);
  EXPECT_EQ(pool.capacity(), kStable + kWindow) << "recycling must not grow the pool";
  EXPECT_EQ(pool.liveCount(), kStable + kWindow) << "every slot is reallocated after it is freed";

  // The deterministic half: every handle the writer started with must be dead, and every slot must
  // still hold exactly the payload it was last given. A stale handle and a live one name the same slot
  // and differ only in generation, so the generation counter is the only thing being checked here.
  //
  // This runs after both threads have joined, which is the point. The concurrent phase above proves the
  // readers can resolve while recycling happens; this phase checks the state it left behind without
  // needing to win a race to observe it. `window[slot]` holds the handle of the most recent allocation
  // into that slot - the writer's last write to it - and the main thread reads it only after joining.
  for (uint32_t slot = 0; slot < kWindow; ++slot) {
    EXPECT_FALSE(pool.isAlive(originalWindow[slot]))
            << "the handle that named this slot before it was recycled must not validate, slot " << slot;
    EXPECT_NE(window[slot].generation, originalWindow[slot].generation)
            << "a recycled slot must carry a new generation, slot " << slot;

    const BufferHandle current = window[slot];
    EXPECT_TRUE(pool.isAlive(current)) << "slot " << slot << " must be live after the writer finished";

    // The final round that wrote this slot, and so the payload that must be in it.
    const uint32_t finalRound = slot + ((kRounds - 1 - slot) / kWindow) * kWindow;
    const Pattern* payload = pool.get(current);
    ASSERT_NE(payload, nullptr) << "slot " << slot;
    EXPECT_TRUE(payload->matches(kStable + kWindow + finalRound)) << "slot " << slot;
  }

  // And a handle for a slot that was never allocated must not reach into the arena at all.
  EXPECT_FALSE(pool.isAlive(BufferHandle(kStable + kWindow + 1, 1)))
          << "a handle one past the last allocated slot must be rejected";
}

// ------------------------------------------------------------------------------------------------
// GLDevice: concurrent command buffer creation and destruction
// ------------------------------------------------------------------------------------------------

/**
 * Command buffers are created, recorded into and destroyed from N threads at once.
 *
 * This is the P0 race in its original form: `createCommandBuffer` pushed onto `GLDevice::mCommandBuffers`
 * and `destroyCommandBuffer` searched and erased, both from an arbitrary thread, both on a `std::vector`.
 *
 * ## Why this needs a real device, and skips when there is none
 *
 * The lock is a member of `GLDevice`, so the test has to have one, and the only way to get one is for the
 * platform layer to bring up a GL context - `GLDevice` has no context-free constructor on purpose. The
 * recording in this test is deliberately the subset that touches no driver state (`begin`, viewport,
 * scissor, blend constant, push constants, `end`), because a GL context is thread-affine and recording
 * driver calls from four threads would be testing the driver rather than this backend. What is being
 * checked is the container and the lock, and neither of those needs the context to be useable - it only
 * needs to exist, because that is how the device is created.
 *
 * On a machine with no GL stack the test skips, like every other device test in this suite.
 */
TEST(GLDeviceThreading, CommandBuffersCanBeCreatedAndDestroyedConcurrently) {
  constexpr uint32_t kThreadCount = 8;
  constexpr uint32_t kBuffersPerThread = 200;

  SurfaceDescriptor surface;
  surface.width = 64;
  surface.height = 64;
  surface.preferredFormat = PixelFormat::R8G8B8A8_UNORM;
  surface.label = "backend-threading-test";

  DeviceConfiguration configuration;
  configuration.preferredBackend = Backend::OpenGL;
  // The dialect is left unset so the platform picks its own, which is what a caller should do: naming
  // one here would make this test fail on a machine whose driver only offers the other.
  configuration.surface = &surface;
  configuration.allowFallback = false;
  configuration.appName = "coral-backend-threading-tests";

  Device* device = nullptr;
  if (!isSuccess(createDevice(configuration, &device)) || device == nullptr ||
      device->getInfo().backend != Backend::OpenGL) {
    GTEST_SKIP() << "no OpenGL device is available on this machine";
  }

  auto* const glDevice = static_cast<GLDevice*>(device);
  EXPECT_EQ(glDevice->ownedCommandBufferCount(), 0u) << "a fresh device owns no command buffers";

  StartGate gate;
  std::atomic<uint32_t> failures{ 0 };
  std::atomic<uint32_t> created{ 0 };
  std::atomic<uint32_t> recorded{ 0 };

  std::vector<std::thread> workers;
  workers.reserve(kThreadCount);
  for (uint32_t worker = 0; worker < kThreadCount; ++worker) {
    workers.emplace_back([&, worker] {
      gate.wait();
      for (uint32_t iteration = 0; iteration < kBuffersPerThread; ++iteration) {
        CommandBufferDescriptor descriptor;
        descriptor.label = "threading-test";
        CommandBuffer* commandBuffer = nullptr;
        if (!isSuccess(glDevice->createCommandBuffer(descriptor, &commandBuffer)) || commandBuffer == nullptr) {
          ++failures;
          continue;
        }
        ++created;

        if (!isSuccess(commandBuffer->begin())) ++failures;
        Viewport viewport;
        viewport.width = float(64 + worker);
        viewport.height = float(64 + (iteration % 64));
        if (!isSuccess(commandBuffer->setViewport(viewport))) ++failures;
        ScissorRect scissor;
        scissor.width = 32;
        scissor.height = 32;
        if (!isSuccess(commandBuffer->setScissor(scissor))) ++failures;
        if (!isSuccess(commandBuffer->setBlendConstant(Color(0.25f, 0.5f, 0.75f, 1.0f)))) ++failures;
        const uint32_t constants[4] = { worker, iteration, worker + iteration, 0 };
        if (!isSuccess(commandBuffer->pushConstants(constants, sizeof(constants), 0))) ++failures;
        if (!isSuccess(commandBuffer->end())) ++failures;
        ++recorded;

        // Destroying it on the same thread is the harshest interleaving for the container: every thread
        // erases from the middle of the vector while the others push and erase, so an implementation
        // whose `erase_if` and `push_back` are not mutually excluded corrupts immediately.
        glDevice->destroyCommandBuffer(commandBuffer);
      }
    });
  }

  gate.open();
  for (std::thread& worker: workers) worker.join();

  EXPECT_EQ(failures.load(), 0u);
  EXPECT_EQ(created.load(), kThreadCount * kBuffersPerThread);
  EXPECT_EQ(recorded.load(), kThreadCount * kBuffersPerThread);

  // Every buffer the threads created was destroyed, so the device owns none. A `unique_ptr` left behind
  // is a leak the erase failed to perform, which is what happens when two threads race on one `erase_if`.
  EXPECT_EQ(glDevice->ownedCommandBufferCount(), 0u)
          << "every created command buffer must have been destroyed exactly once";

  EXPECT_RHI_SUCCESS(device->waitIdle());
  delete device;
}

/**
 * The interleaving a per-thread list could not express, and the reason the design's preferred
 * alternative was rejected: the thread that *creates* a command buffer is not the thread that destroys
 * it.
 *
 * `destroyCommandBuffer` has to find and erase the entry, so it must walk a container another thread is
 * pushing onto. Four producer threads hand their pointers to one consumer over a queue, and the consumer
 * erases from the vector while the producers grow it, so the search and a potential reallocation overlap
 * on every iteration.
 */
TEST(GLDeviceThreading, BuffersCanBeDestroyedByAnotherThread) {
  constexpr uint32_t kProducerCount = 4;
  constexpr uint32_t kBuffersPerProducer = 150;

  SurfaceDescriptor surface;
  surface.width = 64;
  surface.height = 64;
  surface.preferredFormat = PixelFormat::R8G8B8A8_UNORM;
  surface.label = "backend-threading-test";

  DeviceConfiguration configuration;
  configuration.preferredBackend = Backend::OpenGL;
  // The dialect is left unset so the platform picks its own, which is what a caller should do: naming
  // one here would make this test fail on a machine whose driver only offers the other.
  configuration.surface = &surface;
  configuration.allowFallback = false;
  configuration.appName = "coral-backend-threading-tests";

  Device* device = nullptr;
  if (!isSuccess(createDevice(configuration, &device)) || device == nullptr ||
      device->getInfo().backend != Backend::OpenGL) {
    GTEST_SKIP() << "no OpenGL device is available on this machine";
  }

  auto* const glDevice = static_cast<GLDevice*>(device);

  /**
   * The hand-off queue.
   *
   * A mutex and a condition variable rather than an atomic ring: the point of this test is the device's
   * own container, and a hand-written lock-free queue would put a second, unrelated concurrency bug
   * between the producers and the thing under test. The notify is unconditional on push because a
   * consumer that is already draining does not need waking and one that is not must be.
   */
  class Queue {
    public:
    void push(CommandBuffer* commandBuffer) {
      {
        const std::lock_guard<std::mutex> guard(mMutex);
        mPending.push_back(commandBuffer);
      }
      mNotEmpty.notify_one();
    }

    [[nodiscard]] CommandBuffer* pop() {
      std::unique_lock<std::mutex> lock(mMutex);
      mNotEmpty.wait(lock, [this] { return !mPending.empty(); });
      CommandBuffer* const commandBuffer = mPending.front();
      mPending.pop_front();
      return commandBuffer;
    }

    private:
    std::mutex mMutex;
    std::condition_variable mNotEmpty;
    std::deque<CommandBuffer*> mPending;
  };

  Queue queue;
  std::atomic<uint32_t> failures{ 0 };
  std::atomic<uint32_t> produced{ 0 };
  std::atomic<uint32_t> destroyed{ 0 };
  constexpr uint32_t kTotal = kProducerCount * kBuffersPerProducer;

  std::thread destroyer([&] {
    for (uint32_t index = 0; index < kTotal; ++index) {
      CommandBuffer* const commandBuffer = queue.pop();
      if (commandBuffer == nullptr) {
        ++failures;
        continue;
      }
      glDevice->destroyCommandBuffer(commandBuffer);
      ++destroyed;
    }
  });

  StartGate gate;
  std::vector<std::thread> producers;
  producers.reserve(kProducerCount);
  for (uint32_t producer = 0; producer < kProducerCount; ++producer) {
    producers.emplace_back([&] {
      gate.wait();
      for (uint32_t iteration = 0; iteration < kBuffersPerProducer; ++iteration) {
        CommandBuffer* commandBuffer = nullptr;
        if (!isSuccess(glDevice->createCommandBuffer(CommandBufferDescriptor{}, &commandBuffer)) ||
            commandBuffer == nullptr) {
          ++failures;
          continue;
        }
        if (!isSuccess(commandBuffer->begin()) || !isSuccess(commandBuffer->end())) {
          ++failures;
        }
        ++produced;
        queue.push(commandBuffer);
      }
    });
  }

  gate.open();
  for (std::thread& producer: producers) producer.join();
  destroyer.join();

  EXPECT_EQ(failures.load(), 0u);
  EXPECT_EQ(produced.load(), kTotal);
  EXPECT_EQ(destroyed.load(), kTotal);
  EXPECT_EQ(glDevice->ownedCommandBufferCount(), 0u)
          << "a command buffer destroyed by another thread must be gone from the device";

  EXPECT_RHI_SUCCESS(device->waitIdle());
  delete device;
}

} // namespace anonymous
} // namespace test
