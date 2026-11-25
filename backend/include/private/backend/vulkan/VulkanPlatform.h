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

#ifndef CORAL_BACKEND_VULKAN_PLATFORM_H
#define CORAL_BACKEND_VULKAN_PLATFORM_H

/**
 * @file VulkanPlatform.h
 *
 * The window-system half of the Vulkan backend: turning the RHI's `SurfaceDescriptor` into a
 * `VkSurfaceKHR` and naming the instance extension that has to be enabled for it.
 *
 * This is the same split the OpenGL backend uses (`GlPlatform*`): everything that differs between
 * Windows, Android and Linux is in one translation unit per platform, and the device never contains a
 * `#if defined(_WIN32)` around a driver call.
 *
 * Vulkan has no "context", so the platform layer has nothing to create up front: a headless device
 * needs no platform extension at all, and a window is described only when `createSurface` is called.
 * That is why this is a pair of functions rather than an object with a lifetime.
 */

#include <backend/Device.h>

#include <vulkan/vulkan.h>

#include "private/backend/vulkan/VulkanX11Compat.h"

namespace backend::vulkan {

struct VulkanApi;

/**
 * @return the `VK_KHR_*_surface` extension this platform renders through, or null when the platform
 *         has no Vulkan window-system integration in this build.
 *
 * @param descriptor the surface that will be created, when it is known. Linux uses it to choose
 *        between the Xlib and Wayland surfaces rather than assuming Xlib.
 */
[[nodiscard]] const char* platformSurfaceExtensionName(const SurfaceDescriptor* descriptor = nullptr) noexcept;

/** @return "win32", "android", "xlib", "wayland" or "none", for diagnostics. */
[[nodiscard]] const char* platformWindowSystemName(NativeWindowSystem system = NativeWindowSystem::Unknown) noexcept;

/**
 * Creates a `VkSurfaceKHR` for @p descriptor.
 *
 * @return `Result::Success`, `Result::Unsupported` when this build has no platform integration, or
 *         `Result::InvalidArgument` when the descriptor names no window.
 */
Result createPlatformSurface(const VulkanApi& api, VkInstance instance, const SurfaceDescriptor& descriptor,
        VkSurfaceKHR* outSurface);

} // namespace backend::vulkan

#endif // CORAL_BACKEND_VULKAN_PLATFORM_H
