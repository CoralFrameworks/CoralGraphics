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

#include "private/backend/vulkan/VulkanPlatform.h"

#include "private/backend/vulkan/VulkanEntryPoints.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__ANDROID__)
#include <android/native_window.h>
#elif defined(__linux__)
#include <X11/Xlib.h>
// X11/X.h's macros collide with the RHI's enumerators. VulkanX11Compat.h cannot be used a second
// time here because its include guard has already fired, so the same list is repeated for the direct
// include above; see that header for why these three in particular.
#ifdef Success
#undef Success
#endif
#ifdef None
#undef None
#endif
#ifdef Always
#undef Always
#endif
#endif

namespace backend::vulkan {

const char* platformSurfaceExtensionName(const SurfaceDescriptor* descriptor) noexcept {
#if defined(_WIN32)
  return VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#elif defined(__ANDROID__)
  return VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
#elif defined(__linux__)
  if (descriptor != nullptr && descriptor->nativeWindowSystem == NativeWindowSystem::Wayland) {
    return VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
  }
  return VK_KHR_XLIB_SURFACE_EXTENSION_NAME;
#else
  return nullptr;
#endif
}

const char* platformWindowSystemName(const NativeWindowSystem system) noexcept {
#if defined(_WIN32)
  (void) system;
  return "win32";
#elif defined(__ANDROID__)
  (void) system;
  return "android";
#elif defined(__linux__)
  return system == NativeWindowSystem::Wayland ? "wayland" : "xlib";
#else
  (void) system;
  return "none";
#endif
}

Result createPlatformSurface(const VulkanApi& api, const VkInstance instance, const SurfaceDescriptor& descriptor,
        VkSurfaceKHR* outSurface) {
  if (outSurface == nullptr) {
    return Result::InvalidArgument;
  }
  *outSurface = VK_NULL_HANDLE;
  if (descriptor.nativeWindow == nullptr) {
    // A headless device is a supported configuration: it renders into offscreen targets and never
    // presents, which is what the unit tests use. Naming no window is therefore an argument error on
    // `createSurface`, not a device-creation failure.
    return Result::InvalidArgument;
  }

#if defined(_WIN32)
  if (api.vkCreateWin32SurfaceKHR == nullptr) {
    return Result::Unsupported;
  }
  VkWin32SurfaceCreateInfoKHR info{};
  info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
  info.hinstance = ::GetModuleHandleW(nullptr);
  info.hwnd = static_cast<HWND>(descriptor.nativeWindow);
  const VkResult result = api.vkCreateWin32SurfaceKHR(instance, &info, nullptr, outSurface);
  return result == VK_SUCCESS ? Result::Success : Result::Error;
#elif defined(__ANDROID__)
  if (api.vkCreateAndroidSurfaceKHR == nullptr) {
    return Result::Unsupported;
  }
  VkAndroidSurfaceCreateInfoKHR info{};
  info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
  info.window = static_cast<ANativeWindow*>(descriptor.nativeWindow);
  const VkResult result = api.vkCreateAndroidSurfaceKHR(instance, &info, nullptr, outSurface);
  return result == VK_SUCCESS ? Result::Success : Result::Error;
#elif defined(__linux__)
  if (descriptor.nativeWindowSystem == NativeWindowSystem::Wayland) {
    if (api.vkCreateWaylandSurfaceKHR == nullptr) {
      return Result::Unsupported;
    }
    VkWaylandSurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    info.display = static_cast<wl_display*>(descriptor.nativeDisplay);
    info.surface = static_cast<wl_surface*>(descriptor.nativeWindow);
    if (info.display == nullptr || info.surface == nullptr) {
      return Result::InvalidArgument;
    }
    const VkResult result = api.vkCreateWaylandSurfaceKHR(instance, &info, nullptr, outSurface);
    return result == VK_SUCCESS ? Result::Success : Result::Error;
  }
  if (api.vkCreateXlibSurfaceKHR == nullptr) {
    return Result::Unsupported;
  }
  VkXlibSurfaceCreateInfoKHR info{};
  info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
  info.dpy = static_cast<Display*>(descriptor.nativeDisplay);
  info.window = static_cast<::Window>(reinterpret_cast<uintptr_t>(descriptor.nativeWindow));
  if (info.dpy == nullptr || info.window == 0) {
    return Result::InvalidArgument;
  }
  const VkResult result = api.vkCreateXlibSurfaceKHR(instance, &info, nullptr, outSurface);
  return result == VK_SUCCESS ? Result::Success : Result::Error;
#else
  (void) api;
  (void) instance;
  (void) descriptor;
  return Result::Unsupported;
#endif
}

} // namespace backend::vulkan
