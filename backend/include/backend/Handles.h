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

#ifndef CORAL_BACKEND_HANDLES_H
#define CORAL_BACKEND_HANDLES_H

#include <backend/Types.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace backend {
/**
 * Type-safe, generation-checked resource handle.
 *
 * The RHI never exposes a raw driver pointer to upper layers. Instead every resource is addressed
 * by an index + generation pair:
 *
 *  - 24 bits of index address one slot in the owning device's resource pool: 16.7 million
 *    concurrently live objects, far beyond any plausible budget.
 *  - 24 bits of generation are bumped every time a slot is recycled, so a handle that outlives its
 *    resource fails validation instead of silently aliasing a new resource.
 *
 * The tag parameter is what actually buys type safety: `TextureHandle` and `BufferHandle` derive
 * from `Handle<TextureTag>` and `Handle<BufferTag>`, which are distinct types, so mixing them up is
 * a compile error rather than a driver-level crash. Because the tag is a template parameter, the two
 * types are layout-identical and trivially copyable - handles can live in vectors, be memcpy'd, and
 * travel across the engine boundary without a wrapper cost.
 */
template<typename Tag>
struct Handle {
  using TagType = Tag;

  static constexpr uint32_t kIndexBits = 24;
  static constexpr uint32_t kGenerationBits = 24;
  static constexpr uint64_t kIndexMask = (uint64_t(1) << kIndexBits) - 1;
  static constexpr uint32_t kMaxGeneration = (uint32_t(1) << kGenerationBits) - 1;
  static constexpr uint32_t kMaxIndex = uint32_t(kIndexMask);

  /** Slot number in the owning pool. Only meaningful when valid() is true. */
  uint32_t index : 24;
  /** Bumped on every recycle; compared against the pool entry to detect stale handles. */
  uint32_t generation : 24;

  // No third field: 24 + 24 bits fit exactly in the 32 bit allocation unit MSVC picks, so the whole
  // handle stays 8 bytes. Adding any reserved bits here would push it to 12 and break the
  // "handles are uint64_t sized" property that recording them into command buffers relies on.

  constexpr Handle() noexcept
          : index(0),
            generation(0) {}
  constexpr Handle(uint32_t slot, uint32_t gen) noexcept
          : index(slot),
            generation(gen) {}

  /** @return true when this handle refers to a live resource. */
  [[nodiscard]] constexpr bool valid() const noexcept { return generation != 0; }

  [[nodiscard]] constexpr uint64_t value() const noexcept {
    return (uint64_t(generation) << kIndexBits) | uint64_t(index);
  }

  friend constexpr bool operator==(Handle a, Handle b) noexcept {
    return a.index == b.index && a.generation == b.generation;
  }
  friend constexpr bool operator!=(Handle a, Handle b) noexcept { return !(a == b); }
  friend constexpr bool operator<(Handle a, Handle b) noexcept { return a.value() < b.value(); }

  /** A handle that is guaranteed to compare falsy. */
  static constexpr Handle invalid() noexcept { return Handle(); }
};

/**
 * A handle paired with the raw native object, as produced by Device::getNativeTexture and friends.
 * `object` is a driver-specific pointer or integer; `context` identifies which driver/display it
 * belongs to so that a consumer knows whether the resource is usable on the current thread.
 */
template<typename Tag>
struct NativeObject {
  void* object = nullptr;
  void* context = nullptr;

  [[nodiscard]] constexpr bool valid() const noexcept { return object != nullptr; }
  explicit constexpr operator bool() const noexcept { return valid(); }
};

// ------------------------------------------------------------------------------------------------
// Resource tags. Each one is an incomplete type: it exists only to distinguish handle types.
// ------------------------------------------------------------------------------------------------
struct TextureTag;
struct TextureViewTag;
struct TextureSamplerTag;
struct BufferTag;
struct ShaderModuleTag;
struct PipelineLayoutTag;
struct RenderPipelineTag;
struct ComputePipelineTag;
struct DescriptorSetLayoutTag;
struct DescriptorSetTag;
struct RenderPassTag;
struct FramebufferTag;
struct QuerySetTag;
struct FenceTag;
struct SemaphoreTag;
struct SwapchainTag;
struct SwapchainTargetTag;
struct SurfaceTag;

using TextureHandle = Handle<TextureTag>;
using TextureViewHandle = Handle<TextureViewTag>;

/**
 * A view that names the **surface's own images** -- the swapchain -- rather than a texture the caller created.
 *
 * A window target has no colour texture to attach: the backend renders into the default framebuffer (OpenGL) or into the
 * image it acquired (Vulkan), and neither is a `TextureHandle` this API ever hands out. So the attachment is named by this
 * sentinel instead, and `Device::resolveTextureView` reports it as the default framebuffer.
 *
 * The value is deliberately the top of the index space with a generation that no allocator will ever produce: a real view
 * is allocated from a free list and its generation starts at 1, while this one has 0xFFFFFF in both fields.
 */
[[nodiscard]] constexpr TextureViewHandle defaultFramebufferView() noexcept {
  return TextureViewHandle(0xFFFFFFu, 0xFFFFFFu);
}
using TextureSamplerHandle = Handle<TextureSamplerTag>;
using BufferHandle = Handle<BufferTag>;
using ShaderModuleHandle = Handle<ShaderModuleTag>;
using PipelineLayoutHandle = Handle<PipelineLayoutTag>;
using RenderPipelineHandle = Handle<RenderPipelineTag>;
using ComputePipelineHandle = Handle<ComputePipelineTag>;
using DescriptorSetLayoutHandle = Handle<DescriptorSetLayoutTag>;
using DescriptorSetHandle = Handle<DescriptorSetTag>;
using RenderPassHandle = Handle<RenderPassTag>;
using FramebufferHandle = Handle<FramebufferTag>;
using QuerySetHandle = Handle<QuerySetTag>;
using FenceHandle = Handle<FenceTag>;
using SemaphoreHandle = Handle<SemaphoreTag>;
using SwapchainHandle = Handle<SwapchainTag>;
using SwapchainTargetHandle = Handle<SwapchainTargetTag>;
using SurfaceHandle = Handle<SurfaceTag>;

// ------------------------------------------------------------------------------------------------
// Native object aliases
// ------------------------------------------------------------------------------------------------
using NativeTexture = NativeObject<TextureTag>;
using NativeBuffer = NativeObject<BufferTag>;
using NativeSurface = NativeObject<SurfaceTag>;

/**
 * Which platform object a `NativeTextureDescriptor` names.
 *
 * There is one import entry point because the result is always the same -- a `TextureHandle` the rest
 * of the RHI samples like any other -- while the object behind it is whatever the platform's zero-copy
 * mechanism produces. Naming the kind in the descriptor, rather than letting each backend guess from a
 * union, is what lets a backend answer for the kinds it has and *refuse* the rest: a Vulkan device that
 * read a Direct3D shared handle as a GL name would fail deep inside the driver with a VUID instead of
 * reporting `Unsupported` and letting the caller fall back to an upload.
 */
enum class NativeTextureKind : uint32_t {
  /** A platform GL texture name (`glTexture` / `glTarget`), imported by name. */
  OpenGlTexture = 0,
  /**
   * A Direct3D 11 texture shared as an NT handle, as produced by `IDXGIResource1::CreateSharedHandle`
   * on a texture created with `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` and
   * `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX`.
   *
   * Windows only. The Vulkan backend imports it through `VK_KHR_external_memory_win32` with
   * `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT`; the OpenGL backend has no equivalent and
   * reports `Unsupported`.
   */
  Direct3D11Texture = 1,
  /**
   * An `AHardwareBuffer*` allocated with a GPU usage flag.
   *
   * Android only. **Not implemented yet**: both backends answer `Unsupported`, and the value exists so
   * that callers and the video layer can name the kind while the import is being built. Two things are
   * missing: the app targets API 23 while `AHardwareBuffer` starts at 26, and a decoder's YUV buffer can
   * only be sampled through a `VkSamplerYcbcrConversion`, which the RHI has no plumbing for. The
   * renderer's status document (P1-1) records the path.
   */
  AndroidHardwareBuffer = 2,
  /** A VkImage created on the same device, wrapped without owning its memory. */
  VulkanImage = 3,
  /**
   * An `IOSurfaceRef`, the Apple shared-surface object a video decoder, a camera, a
   * `CVPixelBuffer`, or another Metal/OpenGL renderer hands to this process.
   *
   * macOS and iOS only. The Metal backend imports the surface as an `id<MTLTexture>` through
   * `newTextureWithDescriptor:iosurface:plane:`, so sampling (and, with `writeback`, copying into)
   * it touches the producer's storage directly. `platformHandle` is the borrowed `IOSurfaceRef`;
   * the imported texture retains it, so the caller may release its own reference as soon as the
   * import returned.
   *
   * The surface must describe a single-plane RGBA8/BGRA8/RGBA16F image. A bi-planar YUV video
   * surface (NV12/420f) is **not** importable as one texture; the producer has to convert it, or a
   * future YUV import has to add both plane textures and a conversion sampler.
   *
   * Only the Metal backend implements this kind. macOS's OpenGL cannot bind an IOSurface as a
   * `GL_TEXTURE_2D` (`CGLTexImageIOSurface2D` accepts `GL_TEXTURE_RECTANGLE_ARB` only), so
   * `supportsNativeTextureImport(IOSurface)` is false on OpenGL and the Apple OpenGL effect nodes
   * bridge the surface themselves instead of asking the RHI to.
   */
  IOSurface = 4,
};

/**
 * Opaque, backend-owned value that carries a raw texture for zero-copy import.
 *
 * ## Ownership
 *
 * The platform object is always owned by the caller; the RHI imports it and hands back a handle whose
 * lifetime is independent of the caller's. Nothing in the RHI closes a handle it did not create.
 *
 *  - `OpenGlTexture`: the GL name belongs to the caller and is never deleted by `destroyTexture`.
 *  - `Direct3D11Texture`: `platformHandle` is a borrowed NT handle. The driver duplicates it at
 *    `vkAllocateMemory`, so the caller may close it as soon as the import returned, but it must stay
 *    open while the import call is in flight. Synchronisation with the D3D11 producer is the caller's
 *    job: either a keyed mutex or a shared fence, acquired before the import is sampled. A texture that
 *    changes while it is being read is a race the RHI cannot see.
 *  - `AndroidHardwareBuffer`: `platformHandle` would be an `AHardwareBuffer*`. No backend implements this
 *    kind yet, so the ownership rule is stated for the implementation that lands: the backend takes its
 *    own reference (`AHardwareBuffer_acquire`) for as long as the texture lives, and the caller keeps
 *    ownership of the reference it passed in.
 *  - `IOSurface`: `platformHandle` is a borrowed `IOSurfaceRef`. The imported Metal texture retains
 *    the surface itself, so the caller may release its reference once the import returned; the RHI
 *    never calls `IOSurfaceRelease`. A surface whose contents change while it is being sampled is a
 *    producer/consumer race the RHI cannot see, exactly as with `Direct3D11Texture`.
 */
struct NativeTextureDescriptor {
  uint32_t glTarget = 0;
  uint32_t glTexture = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  PixelFormat format = PixelFormat::Undefined;

  /** Which platform object the descriptor names; `OpenGlTexture` keeps the historical meaning. */
  NativeTextureKind kind = NativeTextureKind::OpenGlTexture;
  /** The platform object itself, or null when `glTexture` carries it (the OpenGL kind). */
  void* platformHandle = nullptr;
  /** Optional VkImageView for VulkanImage; the backend creates one when null. */
  void* platformView = nullptr;
  /**
   * Plane index inside the platform object: the IOSurface plane for `IOSurface`, 0 elsewhere.
   *
   * A single-plane RGBA/BGRA surface uses plane 0. A bi-planar video surface would name plane 0
   * (luma) or plane 1 (chroma), which is why the field exists even though one-texture imports only
   * use plane 0 today.
   */
  uint32_t plane = 0;
  /**
   * Set when the platform image is a YUV image (an Android hardware buffer in NV12, for instance): the
   * imported texture then has an external format and can only be read through a sampler that carries this.
   */
  std::optional<YcbcrConversionDescriptor> ycbcr;
  /**
   * Import the texture as a writeback target: the backend additionally allows copies into it.
   *
   * A decoder texture is only ever sampled, which is why the default import is not a copy destination.
   * An encoder texture is the other direction - the graph's final frame is copied into it and the
   * consumer reads it without a CPU round trip - so it needs `TransferDst` (and the platform texture
   * has to be created with the matching bind flags). For `IOSurface`, a writeback import returns a
   * texture the Metal backend accepts as a blit destination; the effect output node copies the final
   * frame into it, so the surface's own storage holds the frame.
   */
  bool writeback = false;
};

// ------------------------------------------------------------------------------------------------
// Handle pool
// ------------------------------------------------------------------------------------------------

/**
 * Allocates and validates the slots addressed by Handle<Tag>.
 *
 * This is the piece of shared infrastructure every backend needs: a Vulkan backend stores
 * VkBuffer in the payload, an OpenGL backend stores a GLuint, and neither has to reimplement
 * generation counting or free-list management.
 *
 * The pool never shrinks, and generation 0 is reserved for "invalid", so a freshly created pool
 * hands out slot 0 with generation 1. When a slot is freed its generation is incremented and the
 * slot is pushed onto the free list; wrapping the generation to zero burns the slot permanently
 * rather than handing out a handle that would compare equal to the invalid handle.
 *
 * ## Thread safety
 *
 * What **is** safe, and is what makes multi-threaded recording work:
 *
 *  - `get`, the const overload of `get`, and `isAlive` may be called concurrently, from any number of
 *    threads, for a handle the caller owns (one it received from the device, or read out of its own
 *    command list). A read either returns a fully constructed payload or `nullptr`; it never returns
 *    a pointer to an entry that is still being filled in.
 *  - `allocate` and `free` may also be called concurrently with those readers, but only from the
 *    device thread, because only on the device thread is resource creation serialised against
 *    resource destruction.
 *
 * What is **not** safe, and is now a caller obligation to observe:
 *
 *  - `allocate`/`free` from two threads at once. The free list is a plain `std::vector` and is not
 *    synchronised; the existing "resource creation and destruction is serialised on the device"
 *    contract is what makes this sufficient, and adding a second lock here would put a lock on the
 *    destroy path for no benefit.
 *  - Calling `free` (or `destroy*` on the owning device) for a handle while another thread is inside
 *    `get`/`isAlive` for that same handle. `free` destroys the payload and bumps the generation; a
 *    reader that already passed the generation check is holding a pointer into a slot that is being
 *    torn down. This is not fixable by a memory order, only by the caller not doing it.
 *
 * ## How the visibility guarantee is provided
 *
 * Two things have to hold before an `Entry*` a recording thread is holding can be dereferenced
 * safely: the pointer must still be valid, and every byte behind it must be visible.
 *
 * **Validity** is why the slots live in a chunked arena rather than in a `std::vector` - or, as the
 * design document first suggested, a `std::deque`. A `std::vector` reallocates on growth, so a later
 * `allocate` frees the block a reader is walking: a use-after-free that no memory order can repair.
 * A `std::deque` looks like it fixes that, and the library reference even says a `push_back` does not
 * invalidate references to existing elements - but that promise is about *elements*, and the reader
 * does not stop at the element. `deque::operator[]` first indexes the deque's internal map, and when
 * the map is full, `push_back` **reallocates the map** and frees the old one. A reader that is inside
 * `operator[]` at that moment is reading a freed map entry, which is the same class of bug the vector
 * had, one level of indirection further down. (This was found by the threading test in
 * `tests/test_Threading.cpp`, which faulted inside `isAlive` with a poison-valued node pointer; MSVC's
 * debug deque fill is what made it a crash instead of a silent wrong answer.)
 *
 * The arena below removes that failure mode by construction. Entries are allocated in fixed-size
 * chunks; a chunk is never moved or resized once it exists, so an `Entry*` into it is valid for the
 * life of the pool. The chunk *pointers* live in a `std::vector` that is `reserve`d to its maximum
 * size in the constructor and therefore never reallocates either; every entry in it is null-initialised
 * up front, so the only element a writer ever assigns is one a reader cannot yet reach (a reader can
 * only ask for a slot below the published size, and the chunk backing the published size is created
 * before the size is published). No reader ever indexes an element of that container that is being
 * written, so there is no race on the container itself - which is precisely what the container the
 * slots used to live in could not promise.
 *
 * **Visibility** is the release/acquire pair on `mSize`. The size is stored with release ordering only
 * after the entry has been fully constructed and the chunk backing it has been created, and read with
 * acquire ordering, so a reader that observes the larger size also observes the entry behind it. The
 * `generation` and `alive` flags need no atomics of their own: they are written only by the device
 * thread, and the release/acquire pair on `mSize` is what gives a recording thread a happens-before
 * edge onto them.
 */
template<typename Tag, typename Payload>
class HandlePool {
  public:
  using HandleType = Handle<Tag>;

  /**
   * Number of slots in one arena chunk.
   *
   * 4096 slots per chunk is a compromise between two costs. The chunk count determines the size of the
   * chunk-pointer array reserved up front: at 4096 the array is 16384 pointers, or 128 KiB, which is
   * nothing next to the pool it indexes. On the other side, the tail of a partly filled chunk is
   * wasted, and the pool with the largest payload is the shader-module pool, whose `Entry` is a few
   * hundred bytes - so the worst case here is a few hundred kilobytes, and only for a device that has
   * finished creating shaders and stopped.
   */
  static constexpr uint32_t kChunkSize = 4096;

  /** Number of chunks needed to address every slot the handle index field can name. */
  static constexpr uint32_t kChunkCount = HandleType::kMaxIndex / kChunkSize + 1;

  HandlePool() {
    // Reserved once, never resized: see the class comment. `reserve` is what makes "the chunk pointer
    // array never reallocates" a property of the type rather than a hope about usage patterns.
    mChunks.reserve(kChunkCount);
    // Null-initialised up front. A reader that somehow reached an unpublished chunk would find a null
    // pointer and report the handle as stale, rather than reading whatever the allocation left there.
    mChunks.assign(kChunkCount, nullptr);
    mSize.store(0, std::memory_order_relaxed);
  }

  ~HandlePool() {
    for (Entry* const chunk: mChunks) delete[] chunk;
  }

  HandlePool(const HandlePool&) = delete;
  HandlePool& operator=(const HandlePool&) = delete;

  /**
   * Claims a slot and stores @p payload in it.
   * @return a valid handle, or an invalid handle when the pool is exhausted (generation wrapped).
   *
   * Device thread only - see the class comment.
   */
  HandleType allocate(Payload&& payload) {
    uint32_t slot;
    if (!mFreeList.empty()) {
      // A recycled slot keeps its generation and its position, so the arena does not change and there
      // is nothing to publish: a reader that could already see this slot can already see it.
      slot = mFreeList.back();
      mFreeList.pop_back();

      Entry& entry = slotEntry(slot);
      entry.payload = std::move(payload);
      entry.alive = true;
      return HandleType(slot, entry.generation);
    }

    const size_t next = mSlotCount;
    if (next > HandleType::kMaxIndex) {
      return HandleType::invalid(); // pool exhausted: 2^24 live objects, beyond any real budget
    }
    slot = static_cast<uint32_t>(next);

    // The chunk is created before the slot is published. A reader can only ask for a slot below the
    // published size, and this chunk is what backs the slot about to be published, so the store below
    // is what makes the chunk pointer visible - and the reader never indexes a chunk pointer that is
    // still null.
    const uint32_t chunkIndex = slot / kChunkSize;
    if (mChunks[chunkIndex] == nullptr) {
      mChunks[chunkIndex] = new Entry[kChunkSize]();
    }

    Entry& entry = slotEntry(slot);
    entry.payload = std::move(payload);
    entry.alive = true;

    ++mSlotCount;
    // Release, not relaxed: every byte written to `entry` above must be visible to a reader that
    // learns about this slot from the store.
    mSize.store(next + 1, std::memory_order_release);
    return HandleType(slot, entry.generation);
  }

  /**
   * Claims a slot and copies @p payload into it.
   *
   * Overloaded rather than folded into the rvalue-reference form, because template argument deduction
   * cannot bind an lvalue to a `Payload&&` parameter: without this, `pool.allocate(someInt)` would
   * fail to compile and callers would have to remember `std::move` or an explicit cast.
   */
  HandleType allocate(const Payload& payload) {
    Payload copy = payload;
    return allocate(std::move(copy));
  }

  /**
   * Releases @p handle's slot. The payload is destroyed, the generation is bumped, and the slot
   * becomes available for reuse. Stale copies of @p handle stop validating from this point on.
   *
   * Device thread only, and only once no other thread is inside `get`/`isAlive` for @p handle - see
   * the class comment for why that second half cannot be enforced here.
   *
   * @return true when the handle was live and has now been destroyed.
   */
  bool free(HandleType handle) {
    if (!isAlive(handle)) return false;

    Entry& entry = slotEntry(handle.index);
    entry.payload = Payload{};
    entry.alive = false;
    if (entry.generation >= HandleType::kMaxGeneration) {
      // Burning the slot is the only safe option: wrapping to zero would produce a handle that
      // compares equal to Handle::invalid() and would validate against a future occupant.
      entry.generation = 0;
      entry.burned = true;
    } else {
      entry.generation++;
      mFreeList.push_back(handle.index);
    }
    return true;
  }

  /**
   * @return true when @p handle addresses a live, non-recycled slot.
   *
   * Safe to call from any thread for a handle the caller owns. `mSize` is the only synchronisation it
   * needs: the acquire load pairs with the release store in `allocate`, so seeing the slot means
   * seeing the entry behind it. Nothing else here is shared mutable state - `mChunks` is reserved and
   * populated by the constructor, and the entry a published slot names was written before the store.
   */
  [[nodiscard]] bool isAlive(HandleType handle) const noexcept {
    if (!handle.valid()) return false;
    if (handle.index >= mSize.load(std::memory_order_acquire)) return false;
    const Entry& entry = slotEntry(handle.index);
    return entry.alive && !entry.burned && entry.generation == handle.generation;
  }

  /**
   * @return the payload for a live @p handle, or nullptr when the handle is stale.
   *
   * The pointer stays valid until the slot is freed, which is what a recording thread needs: it holds
   * the pointer across the device thread's later allocations. The arena is what guarantees that, and
   * neither of the containers this used to be (a `std::vector`, then a `std::deque`) could - see the
   * class comment.
   */
  [[nodiscard]] Payload* get(HandleType handle) noexcept {
    return isAlive(handle) ? &slotEntry(handle.index).payload : nullptr;
  }

  [[nodiscard]] const Payload* get(HandleType handle) const noexcept {
    return isAlive(handle) ? &slotEntry(handle.index).payload : nullptr;
  }

  /** Number of currently live resources. Device thread only: it reads the free list. */
  [[nodiscard]] size_t liveCount() const noexcept { return mSlotCount - mFreeList.size(); }

  /** Number of slots ever allocated (including freed and burned ones). */
  [[nodiscard]] size_t capacity() const noexcept { return mSlotCount; }

  /** Invokes @p fn(handle, payload) for every live resource. Used by device teardown, so device thread only. */
  template<typename Function>
  void forEach(Function&& fn) {
    for (size_t i = 0; i < mSlotCount; ++i) {
      Entry& entry = slotEntry(uint32_t(i));
      if (entry.alive && !entry.burned) fn(HandleType(uint32_t(i), entry.generation), entry.payload);
    }
  }

  private:
  struct Entry {
    Payload payload{};
    uint32_t generation = 1; // 0 is reserved for Handle::invalid()
    bool alive = false;
    bool burned = false;
  };

  /** @return the entry for @p slot, which must be below the slot count. */
  [[nodiscard]] Entry& slotEntry(const uint32_t slot) noexcept {
    return mChunks[slot / kChunkSize][slot % kChunkSize];
  }

  [[nodiscard]] const Entry& slotEntry(const uint32_t slot) const noexcept {
    return mChunks[slot / kChunkSize][slot % kChunkSize];
  }

  // The bound is on the pool's per-slot overhead, not on the payload: the pool is the index every
  // recording thread walks on every `require*` check, so a slot whose bookkeeping dwarfed a small
  // payload would quietly turn that walk into a cache miss per lookup. The largest payload the
  // OpenGL backend stores is `GLShaderModule`, and the largest of the trivial ones is a `GLuint`, so
  // the two ends of the range are what the bound has to hold for. It is a loose sanity bound on
  // purpose - a number nobody can accidentally exceed while refactoring, not a claim about layout.
  static_assert(sizeof(Entry) <= sizeof(Payload) + 256,
          "HandlePool's per-slot bookkeeping must stay small relative to the payload it indexes");

  /**
   * Chunk pointers, one per `kChunkSize` slots.
   *
   * `reserve`d to its full size and null-filled by the constructor, and never resized afterwards, so
   * a reader indexing it cannot race a writer reallocating it. That is the property `std::vector` as
   * the slot array did not have, and the reason a reader can walk an entry while `allocate` runs. A
   * `new Entry[kChunkSize]()` per chunk, freed in the destructor: the chunks themselves are never
   * moved, so an `Entry*` a recording thread holds stays valid for the life of the pool.
   */
  std::vector<Entry*> mChunks;
  /**
   * Number of slots published to recording threads, and the bound they validate themselves against.
   * Written with release ordering after the slot is fully constructed and its chunk exists; read with
   * acquire ordering. Everything below the value is guaranteed to be complete and backed by storage.
   */
  std::atomic<size_t> mSize { 0 };
  /**
   * Number of slots ever handed out - the high-water mark of `mSize`, kept separately because
   * `mSize` bounds reads rather than counting allocated slots and a chunk's tail is not addressable
   * through it. Device thread only: `capacity`, `liveCount` and `forEach` read it, and all three are
   * device-thread calls.
   */
  size_t mSlotCount = 0;
  std::vector<uint32_t> mFreeList;
};

} // namespace backend

#endif // CORAL_BACKEND_HANDLES_H

