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

#include "private/backend/vulkan/VulkanAndroidHardwareBuffer.h"

#if defined(__ANDROID__)

#include <dlfcn.h>

#include <mutex>

#endif

namespace backend::vulkan {

#if defined(__ANDROID__)

namespace {

std::mutex& apiMutex() {
  static std::mutex instance;
  return instance;
}

AndroidHardwareBufferApi& apiState() {
  static AndroidHardwareBufferApi instance;
  return instance;
}

/** Resolves into @p api. Keeps the library open on success, because the pointers live in it. */
bool resolve(AndroidHardwareBufferApi& api) {
  // `dlopen` on a library that is already loaded returns a handle to it, so this does not load a second
  // copy of the platform.
  void* library = ::dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
  if (library == nullptr) {
    return false;
  }
  api.acquire = reinterpret_cast<void (*)(void*)>(::dlsym(library, "AHardwareBuffer_acquire"));
  api.release = reinterpret_cast<void (*)(void*)>(::dlsym(library, "AHardwareBuffer_release"));
  if (api.acquire == nullptr || api.release == nullptr) {
    api = AndroidHardwareBufferApi{};
    ::dlclose(library);
    return false;
  }
  // The library is deliberately not closed: the two pointers would dangle, and the platform library is
  // part of every Android process anyway.
  return true;
}

} // namespace anonymous

const AndroidHardwareBufferApi& androidHardwareBufferApi() {
  AndroidHardwareBufferApi& api = apiState();
  std::lock_guard<std::mutex> lock(apiMutex());
  if (api.available) {
    return api;
  }
  // A failure is not cached: the first caller is not necessarily the one that should decide for the
  // process, and resolving twice costs nothing.
  api.available = resolve(api);
  return api;
}

#else

const AndroidHardwareBufferApi& androidHardwareBufferApi() {
  // No Android, no hardware buffer: the table stays empty, which is what makes the import answer
  // `Unsupported` on this platform.
  static const AndroidHardwareBufferApi unavailable;
  return unavailable;
}

#endif

} // namespace backend::vulkan
