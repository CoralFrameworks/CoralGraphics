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

#include "private/backend/vulkan/VulkanEntryPoints.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace backend::vulkan {
namespace {

#if defined(_WIN32)

void* openLibrary(const char* name) {
  return reinterpret_cast<void*>(::LoadLibraryA(name));
}

void* findSymbol(void* library, const char* name) {
  if (library == nullptr) return nullptr;
  return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(library), name));
}

void closeLibrary(void* library) {
  if (library != nullptr) ::FreeLibrary(static_cast<HMODULE>(library));
}

#else

void* openLibrary(const char* name) {
  return ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
}

void* findSymbol(void* library, const char* name) {
  return library != nullptr ? ::dlsym(library, name) : nullptr;
}

void closeLibrary(void* library) {
  if (library != nullptr) ::dlclose(library);
}

#endif

} // namespace anonymous

const char* loaderLibraryName() noexcept {
#if defined(_WIN32)
  return "vulkan-1.dll";
#elif defined(__APPLE__)
  return "libvulkan.dylib";
#else
  return "libvulkan.so.1";
#endif
}

Result VulkanApi::loadLibrary() {
  if (library != nullptr) {
    return Result::Success;
  }

  const char* names[3] = { loaderLibraryName(), nullptr, nullptr };
#if defined(__ANDROID__)
  names[1] = "libvulkan.so";
#elif defined(__linux__)
  names[1] = "libvulkan.so";
#elif defined(__APPLE__)
  names[1] = "libvulkan.1.dylib";
  names[2] = "libvulkan.dylib";
#elif defined(_WIN32)
  names[1] = "vulkan-1.dll";
#endif

  for (const char* name: names) {
    if (name == nullptr) continue;
    library = openLibrary(name);
    if (library != nullptr) break;
  }
  if (library == nullptr) {
    return Result::Unsupported;
  }

  vkGetInstanceProcAddr =
          reinterpret_cast<PFN_vkGetInstanceProcAddr>(findSymbol(library, "vkGetInstanceProcAddr"));
  if (vkGetInstanceProcAddr == nullptr) {
    unload();
    return Result::Unsupported;
  }
  return Result::Success;
}

Result VulkanApi::loadGlobalFunctions() {
  if (vkGetInstanceProcAddr == nullptr) {
    return Result::Error;
  }

#define CORAL_VK_LOAD_GLOBAL(name) \
  name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(nullptr, #name));
  CORAL_VK_GLOBAL_FUNCTIONS(CORAL_VK_LOAD_GLOBAL)
#undef CORAL_VK_LOAD_GLOBAL

  // `vkEnumerateInstanceVersion` is Vulkan 1.1; a 1.0 loader returns null and the caller then assumes
  // 1.0, which is the honest reading rather than a failure.
  return vkCreateInstance != nullptr ? Result::Success : Result::Unsupported;
}

void VulkanApi::loadInstanceFunctions(const VkInstance instance) {
  if (vkGetInstanceProcAddr == nullptr) {
    return;
  }

#define CORAL_VK_LOAD_INSTANCE(name) \
  name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name));
  CORAL_VK_INSTANCE_FUNCTIONS(CORAL_VK_LOAD_INSTANCE)
#undef CORAL_VK_LOAD_INSTANCE

  // Device-level entry points resolve through the instance as well, and the returned pointers are
  // valid for every device created from it. Loading them here means a device object never has to
  // carry a second lookup table.
#define CORAL_VK_LOAD_DEVICE(name) \
  name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name));
  CORAL_VK_DEVICE_FUNCTIONS(CORAL_VK_LOAD_DEVICE)
#undef CORAL_VK_LOAD_DEVICE

#if defined(_WIN32)
  vkCreateWin32SurfaceKHR =
          reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
  vkGetMemoryWin32HandlePropertiesKHR = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
          vkGetInstanceProcAddr(instance, "vkGetMemoryWin32HandlePropertiesKHR"));
#elif defined(__ANDROID__)
  vkCreateAndroidSurfaceKHR = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(
          vkGetInstanceProcAddr(instance, "vkCreateAndroidSurfaceKHR"));
  vkGetAndroidHardwareBufferPropertiesANDROID = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
          vkGetInstanceProcAddr(instance, "vkGetAndroidHardwareBufferPropertiesANDROID"));
#elif defined(__linux__)
  vkCreateXlibSurfaceKHR =
          reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(vkGetInstanceProcAddr(instance, "vkCreateXlibSurfaceKHR"));
  vkCreateWaylandSurfaceKHR = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(
          vkGetInstanceProcAddr(instance, "vkCreateWaylandSurfaceKHR"));
#endif
}

void VulkanApi::unload() {
  closeLibrary(library);
  library = nullptr;
  vkGetInstanceProcAddr = nullptr;
}

bool VulkanApi::hasRequiredFunctions() const noexcept {
  return vkCreateInstance != nullptr && vkDestroyInstance != nullptr && vkEnumeratePhysicalDevices != nullptr &&
         vkCreateDevice != nullptr && vkGetDeviceQueue != nullptr && vkQueueSubmit != nullptr &&
         vkCreateSwapchainKHR != nullptr && vkCmdDraw != nullptr && vkCreateGraphicsPipelines != nullptr &&
         vkCreateRenderPass != nullptr && vkDestroyRenderPass != nullptr && vkCreateFramebuffer != nullptr &&
         vkDestroyFramebuffer != nullptr && vkCmdBeginRenderPass != nullptr && vkCmdEndRenderPass != nullptr &&
         vkCmdClearAttachments != nullptr && vkCmdResolveImage != nullptr;
}

} // namespace backend::vulkan
