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

#include <gpu/ContextGroup.h>

#include <backend/RHI.h>

namespace graphics::gpu {

ContextGroup::ContextGroup() = default;

ContextGroup::~ContextGroup() = default;

std::shared_ptr<ContextGroup> ContextGroup::make() {
  // The backend is decided by what the device factory can actually bring up, not by asking at creation time: a group whose
  // backend no device can satisfy would fail later and further from the cause.
  auto group = std::shared_ptr<ContextGroup>(new ContextGroup());
  group->mBackend = backend::Backend::Null;
  return group;
}

backend::Backend ContextGroup::backend() const noexcept {
  const std::lock_guard<std::mutex> lock(mMutex);
  return mBackend;
}

uint32_t ContextGroup::deviceCount() const noexcept {
  const std::lock_guard<std::mutex> lock(mMutex);
  return mDeviceCount;
}

void* ContextGroup::shareContext() const {
  const std::lock_guard<std::mutex> lock(mMutex);
  return mShareContext;
}

bool ContextGroup::registerDevice(void* nativeContext, const backend::Backend backend) {
  const std::lock_guard<std::mutex> lock(mMutex);
  if (mBackend != backend::Backend::Null && mBackend != backend) {
    // §4.4: a group is one backend. Two rendering models have no object space to share, and pretending otherwise would
    // produce a device that silently sees nothing.
    return false;
  }
  mBackend = backend;
  ++mDeviceCount;
  if (mShareContext == nullptr && nativeContext != nullptr) {
    // The first device becomes the root: everyone else shares its object space.
    mShareContext = nativeContext;
  }
  return true;
}

void ContextGroup::deferRelease(void* owner, std::function<void()> task) {
  if (!task) return;
  const std::lock_guard<std::mutex> lock(mMutex);
  mDeferred.push_back({ owner, std::move(task) });
}

uint32_t ContextGroup::drainDeferredReleases(void* owner) {
  std::vector<DeferredRelease> mine;
  {
    const std::lock_guard<std::mutex> lock(mMutex);
    for (auto entry = mDeferred.begin(); entry != mDeferred.end();) {
      if (entry->owner == owner) {
        mine.push_back(std::move(*entry));
        entry = mDeferred.erase(entry);
      } else {
        ++entry;
      }
    }
  }
  // Run outside the lock: a task calls into the RHI, which may call back into the group (a device unregistering itself, say),
  // and holding the lock across that would deadlock on a mutex nobody can see.
  for (const DeferredRelease& entry: mine) {
    entry.task();
  }
  return static_cast<uint32_t>(mine.size());
}

uint32_t ContextGroup::pendingReleaseCount() const {
  const std::lock_guard<std::mutex> lock(mMutex);
  return static_cast<uint32_t>(mDeferred.size());
}

void ContextGroup::unregisterDevice() {
  const std::lock_guard<std::mutex> lock(mMutex);
  if (mDeviceCount > 0) --mDeviceCount;
}

} // namespace graphics::gpu
