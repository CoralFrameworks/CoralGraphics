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

#ifndef CORAL_BACKEND_VULKAN_ENTRY_POINTS_H
#define CORAL_BACKEND_VULKAN_ENTRY_POINTS_H

/**
 * @file VulkanEntryPoints.h
 *
 * The Vulkan entry points, resolved at run time.
 *
 * ## Why the loader is opened by name instead of linked
 *
 * The Vulkan loader is not part of any platform SDK that a build may assume: Windows has no
 * `vulkan-1.lib` unless the Vulkan SDK is installed, and Android has the loader on the device but
 * not necessarily an import library for the ABI the app targets. Linking against it would make a
 * Vulkan-enabled build depend on how the build machine was provisioned, and the RHI's whole point is
 * that a backend is selected by configuration rather than by what happened to be installed.
 *
 * Opening `vulkan-1.dll` / `libvulkan.so.1` and resolving through `vkGetInstanceProcAddr` keeps the
 * build dependency-free. It is also what the OpenGL backend already does with `wglGetProcAddress`, so
 * the two backends fail in the same shape on a machine with no driver: the library opens, the first
 * call reports why, and the factory falls through to the other backend.
 *
 * ## What is loaded
 *
 * Everything the backend calls, and nothing else. `vkGetPhysicalDeviceFeatures2` and the dynamic
 * rendering entry points are *not* guaranteed to exist on an older loader, so a missing pointer is a
 * capability answer rather than a loader failure: the device probes `VK_KHR_dynamic_rendering` (or
 * core 1.3) before it uses the pointer, and the Vulkan 1.0/1.1 fallback uses the core render-pass
 * entry points instead.
 */

#include <backend/Types.h>

#include <vulkan/vulkan.h>

#include "private/backend/vulkan/VulkanX11Compat.h"

#include <cstdint>

namespace backend::vulkan {

/**
 * The function pointers this backend uses.
 *
 * Declared through one macro list so that the declaration, the resolution and the error message that
 * names a missing symbol cannot disagree. A hand-written second list is how a pointer is declared and
 * then never loaded, which reads as a null dereference inside the driver.
 */
#define CORAL_VK_GLOBAL_FUNCTIONS(X)     \
  X(vkEnumerateInstanceVersion)          \
  X(vkEnumerateInstanceExtensionProperties) \
  X(vkEnumerateInstanceLayerProperties)  \
  X(vkCreateInstance)

#define CORAL_VK_INSTANCE_FUNCTIONS(X)          \
  X(vkDestroyInstance)                          \
  X(vkEnumeratePhysicalDevices)                 \
  X(vkEnumerateDeviceExtensionProperties)       \
  X(vkGetPhysicalDeviceProperties)              \
  X(vkGetPhysicalDeviceFeatures)                \
  X(vkGetPhysicalDeviceFeatures2)               \
  X(vkGetPhysicalDeviceQueueFamilyProperties)   \
  X(vkGetPhysicalDeviceMemoryProperties)        \
  X(vkGetPhysicalDeviceFormatProperties)        \
  X(vkGetPhysicalDeviceImageFormatProperties2)  \
  X(vkGetPhysicalDeviceSurfaceSupportKHR)       \
  X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)  \
  X(vkGetPhysicalDeviceSurfaceFormatsKHR)       \
  X(vkGetPhysicalDeviceSurfacePresentModesKHR)  \
  X(vkDestroySurfaceKHR)                        \
  X(vkCreateDevice)                             \
  X(vkGetDeviceProcAddr)                        \
  X(vkCreateDebugUtilsMessengerEXT)             \
  X(vkDestroyDebugUtilsMessengerEXT)

#define CORAL_VK_DEVICE_FUNCTIONS(X)             \
  X(vkDestroyDevice)                             \
  X(vkGetDeviceQueue)                            \
  X(vkQueueSubmit)                               \
  X(vkQueueWaitIdle)                             \
  X(vkQueuePresentKHR)                           \
  X(vkDeviceWaitIdle)                            \
  X(vkCreateCommandPool)                         \
  X(vkDestroyCommandPool)                        \
  X(vkResetCommandPool)                          \
  X(vkAllocateCommandBuffers)                    \
  X(vkFreeCommandBuffers)                        \
  X(vkBeginCommandBuffer)                        \
  X(vkEndCommandBuffer)                          \
  X(vkResetCommandBuffer)                        \
  X(vkCmdBeginRendering)                         \
  X(vkCmdEndRendering)                           \
  X(vkCreateRenderPass)                          \
  X(vkDestroyRenderPass)                         \
  X(vkCreateFramebuffer)                         \
  X(vkDestroyFramebuffer)                        \
  X(vkCmdBeginRenderPass)                        \
  X(vkCmdEndRenderPass)                          \
  X(vkCmdClearAttachments)                       \
  X(vkCmdResolveImage)                           \
  X(vkCmdBindPipeline)                           \
  X(vkCmdBindVertexBuffers)                      \
  X(vkCmdBindIndexBuffer)                        \
  X(vkCmdBindDescriptorSets)                     \
  X(vkCmdSetViewport)                            \
  X(vkCmdSetScissor)                             \
  X(vkCmdSetStencilReference)                    \
  X(vkCmdSetBlendConstants)                      \
  X(vkCmdPushConstants)                          \
  X(vkCmdDraw)                                   \
  X(vkCmdDrawIndexed)                            \
  X(vkCmdDrawIndirect)                           \
  X(vkCmdDrawIndexedIndirect)                    \
  X(vkCmdDispatch)                               \
  X(vkCmdDispatchIndirect)                       \
  X(vkCmdCopyBuffer)                             \
  X(vkCmdCopyBufferToImage)                      \
  X(vkCmdCopyImageToBuffer)                      \
  X(vkCmdCopyImage)                              \
  X(vkCmdClearColorImage)                        \
  X(vkCmdClearDepthStencilImage)                 \
  X(vkCmdPipelineBarrier)                        \
  X(vkCmdResetQueryPool)                         \
  X(vkCmdBeginQuery)                             \
  X(vkCmdEndQuery)                               \
  X(vkCmdWriteTimestamp)                         \
  X(vkCmdInsertDebugUtilsLabelEXT)               \
  X(vkCmdBeginDebugUtilsLabelEXT)                \
  X(vkCmdEndDebugUtilsLabelEXT)                  \
  X(vkCreateBuffer)                              \
  X(vkDestroyBuffer)                             \
  X(vkCreateImage)                               \
  X(vkDestroyImage)                              \
  X(vkGetImageMemoryRequirements)                \
  X(vkAllocateMemory)                            \
  X(vkFreeMemory)                                \
  X(vkBindImageMemory)                           \
  X(vkCreateImageView)                           \
  X(vkDestroyImageView)                          \
  X(vkCreateSampler)                             \
  X(vkDestroySampler)                            \
  X(vkCreateSamplerYcbcrConversion)              \
  X(vkDestroySamplerYcbcrConversion)             \
  X(vkCreateShaderModule)                        \
  X(vkDestroyShaderModule)                       \
  X(vkCreateDescriptorSetLayout)                 \
  X(vkDestroyDescriptorSetLayout)                \
  X(vkCreateDescriptorPool)                      \
  X(vkDestroyDescriptorPool)                     \
  X(vkResetDescriptorPool)                       \
  X(vkAllocateDescriptorSets)                    \
  X(vkUpdateDescriptorSets)                      \
  X(vkCreatePipelineLayout)                      \
  X(vkDestroyPipelineLayout)                     \
  X(vkCreateGraphicsPipelines)                   \
  X(vkCreateComputePipelines)                    \
  X(vkCreatePipelineCache)                       \
  X(vkDestroyPipelineCache)                      \
  X(vkGetPipelineCacheData)                      \
  X(vkDestroyPipeline)                           \
  X(vkCreateQueryPool)                           \
  X(vkDestroyQueryPool)                          \
  X(vkGetQueryPoolResults)                       \
  X(vkCreateFence)                               \
  X(vkDestroyFence)                              \
  X(vkGetFenceStatus)                            \
  X(vkWaitForFences)                             \
  X(vkResetFences)                               \
  X(vkCreateSemaphore)                           \
  X(vkDestroySemaphore)                          \
  X(vkCreateSwapchainKHR)                        \
  X(vkDestroySwapchainKHR)                       \
  X(vkGetSwapchainImagesKHR)                     \
  X(vkAcquireNextImageKHR)                       \
  X(vkSetDebugUtilsObjectNameEXT)

/**
 * One resolved entry point set.
 *
 * A struct of pointers rather than a global table of dlsym results, because a device may be created,
 * destroyed and created again against a different loader in the same process; the pointers belong to
 * the instance and travel with it.
 */
struct VulkanApi {
#define CORAL_VK_DECLARE(name) PFN_##name name = nullptr;
  CORAL_VK_GLOBAL_FUNCTIONS(CORAL_VK_DECLARE)
  CORAL_VK_INSTANCE_FUNCTIONS(CORAL_VK_DECLARE)
  CORAL_VK_DEVICE_FUNCTIONS(CORAL_VK_DECLARE)
#undef CORAL_VK_DECLARE

  /** The loader's `vkGetInstanceProcAddr`, which is the one symbol every other one is found through. */
  PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;

  /**
   * The window-system entry point, which only exists in the headers of the platform it belongs to.
   *
   * `PFN_vkCreateWin32SurfaceKHR` is not declared at all on Linux, so the three cannot live in the
   * macro list above without a preprocessor conditional inside a macro. One member per platform, one
   * conditional load, and no way for a caller to name the wrong one.
   */
#if defined(_WIN32)
  PFN_vkCreateWin32SurfaceKHR vkCreateWin32SurfaceKHR = nullptr;
  /**
   * The import half of `VK_KHR_external_memory_win32`: asks the driver which memory types a shared NT
   * handle can be bound to. Only present when the device enabled that extension.
   */
  PFN_vkGetMemoryWin32HandlePropertiesKHR vkGetMemoryWin32HandlePropertiesKHR = nullptr;
#elif defined(__ANDROID__)
  PFN_vkCreateAndroidSurfaceKHR vkCreateAndroidSurfaceKHR = nullptr;
  /**
   * The import half of `VK_ANDROID_external_memory_android_hardware_buffer`: asks the driver what an
   * `AHardwareBuffer` maps onto (format, memory types, allocation size). Only present when the device
   * enabled that extension.
   */
  PFN_vkGetAndroidHardwareBufferPropertiesANDROID vkGetAndroidHardwareBufferPropertiesANDROID = nullptr;
#elif defined(__linux__)
  PFN_vkCreateXlibSurfaceKHR vkCreateXlibSurfaceKHR = nullptr;
  PFN_vkCreateWaylandSurfaceKHR vkCreateWaylandSurfaceKHR = nullptr;
#endif

  /** An opaque handle to the loaded loader library (HMODULE / void* from dlopen); null when none. */
  void* library = nullptr;

  /**
   * Opens the platform loader. Reports `Result::Unsupported` when no Vulkan loader is installed, which
   * is the answer that makes the factory try OpenGL.
   */
  Result loadLibrary();

  /** Resolves the global functions. Legal only after `loadLibrary`. */
  Result loadGlobalFunctions();

  /**
   * Resolves every instance and device function through @p instance.
   *
   * A pointer that the loader does not return is left null; the caller decides whether that entry point
   * is required (the core ones are) or optional (dynamic rendering, debug utils).
   */
  void loadInstanceFunctions(VkInstance instance);

  /** Closes the loader. Safe to call more than once. */
  void unload();

  /** @return true when every core function the backend requires is resolved. */
  [[nodiscard]] bool hasRequiredFunctions() const noexcept;
};

/** @return the name of the loader library this platform uses, for diagnostics. */
[[nodiscard]] const char* loaderLibraryName() noexcept;

} // namespace backend::vulkan

#endif // CORAL_BACKEND_VULKAN_ENTRY_POINTS_H

