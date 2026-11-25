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

#ifndef CORAL_GRAPHICS_GPU_CONTEXTGROUP_H
#define CORAL_GRAPHICS_GPU_CONTEXTGROUP_H

#include <backend/Device.h>

#include <functional>
#include <mutex>
#include <memory>
#include <vector>

namespace graphics::gpu {

/**
 * A group of devices whose **textures, buffers, samplers and programs are shared**.
 *
 * ## Why this exists
 *
 * A GL context is thread-affine: it must be made current on the thread that uses it, and one context cannot be current on
 * two threads at once. A renderer that wants to draw on several threads therefore needs several contexts -- and, without
 * sharing, several copies of every texture, with a copy between them at every hand-off. A group makes the object spaces
 * one, so a texture uploaded on one thread is samplable on another with no copy at all.
 *
 * The first device created in a group is the **root**: its context is what the others share with. Everything after it
 * shares the root's object space.
 *
 * ## What is shared, and what is not (GL rules, which the other backends inherit by mapping)
 *
 * | Resource | Shared |
 * |---|---|
 * | Textures, buffers, samplers, programs/shaders | yes |
 * | Framebuffer objects, vertex array objects | **no** -- they are containers whose state is per-context |
 * | The default framebuffer (a window's images) | **no** -- each context presents to its own surface |
 *
 * ## A group is one backend
 *
 * `ContextGroup` binds to one `backend::Backend`: a GL group and a Vulkan group never mix, because their rendering models
 * have nothing in common -- there is no object space to share between them, only an explicit export/import of textures
 * (see the design document, §4.5). Creating a member of the wrong backend is refused rather than half-supported.
 *
 * ## Lifetime and threading
 *
 * The group must outlive every device created in it. Creating members is expected to be **sequential** (build the group,
 * then start the threads): two threads racing to create the first device would both become roots. Destruction of resources
 * from a thread other than the owning one is the group's job -- see `DeferredRelease` -- because a GL `glDelete*` needs
 * that device's context to be current.
 */
class ContextGroup final {
 public:
  /** @return a group with no devices yet, or null when no backend is available. */
  [[nodiscard]] static std::shared_ptr<ContextGroup> make();

  ~ContextGroup();

  ContextGroup(const ContextGroup&) = delete;
  ContextGroup& operator=(const ContextGroup&) = delete;

  /** The backend every device in this group uses. */
  [[nodiscard]] backend::Backend backend() const noexcept;

  /** How many devices have been created in this group. */
  [[nodiscard]] uint32_t deviceCount() const noexcept;

  /** The platform context a new device should share with, or null when the new device will be the root. */
  [[nodiscard]] void* shareContext() const;

  /**
   * Records a device's platform context, making it the group's root when there is none yet.
   *
   * @return false when the group already has a root from a **different** backend, which is the case §4.4 refuses.
   */
  bool registerDevice(void* nativeContext, backend::Backend backend);
  /**
   * Records that a device has been destroyed.
   *
   * The root's share context is kept: a member created afterwards can still share with the root's object space, which is
   * the whole point of a group outliving one of its members. What the count is for is the assertion a test needs -- that a
   * group comes back to zero devices when its members are destroyed -- which is the observable form of "nothing leaked".
   */
  void unregisterDevice();
  /**
   * Hands a release task to the group, to run on the thread that owns @p owner.
   *
   * ## Why a release cannot simply be called from anywhere
   *
   * A GL object is deleted through a context, and that context must be **current** on the calling thread. A resource created
   * on the render thread therefore cannot be released from the UI thread by calling `glDelete*` there: the object belongs to
   * another context's object space, and the call either fails or -- worse -- deletes a different object with the same name.
   * Rather than document "destroy it on the right thread" and hope, the group holds the task until the owning device is used
   * again: `WindowSurface::beginFrame` drains the tasks whose owner is its own device, with that device's context current.
   *
   * A task whose owner is never used again stays queued, which is a bounded leak rather than a wrong deletion -- the same
   * trade the RHI makes for a device that is destroyed with frames in flight.
   */
  void deferRelease(void* owner, std::function<void()> task);

  /**
   * Runs every queued task whose owner is @p owner, on the calling thread.
   *
   * @return how many tasks ran, which is what a test asserts on: zero pending before, zero pending after, and the resource
   *         demonstrably gone.
   */
  uint32_t drainDeferredReleases(void* owner);

  /** How many tasks are waiting, over all owners. */
  [[nodiscard]] uint32_t pendingReleaseCount() const;

 private:
  ContextGroup();

  mutable std::mutex mMutex;
  void* mShareContext = nullptr;
  uint32_t mDeviceCount = 0;
  backend::Backend mBackend = backend::Backend::Null;

  struct DeferredRelease {
    void* owner = nullptr;
    std::function<void()> task;
  };
  std::vector<DeferredRelease> mDeferred;
};

} // namespace graphics::gpu

#endif // CORAL_GRAPHICS_GPU_CONTEXTGROUP_H
