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

#ifndef CORAL_BACKEND_VULKAN_ANDROID_HARDWARE_BUFFER_H
#define CORAL_BACKEND_VULKAN_ANDROID_HARDWARE_BUFFER_H

/**
 * @file VulkanAndroidHardwareBuffer.h
 *
 * The `AHardwareBuffer` reference counting the Vulkan import needs, resolved at run time.
 *
 * ## Why `dlopen` and not a link
 *
 * The app's minimum API level is 23, and `AHardwareBuffer_acquire` / `AHardwareBuffer_release` only exist
 * from 26. Linking them directly would make every device below 26 fail to load the library, which is the
 * whole app rather than the import. The Vulkan extension that makes the import possible
 * (`VK_ANDROID_external_memory_android_hardware_buffer`) is only exposed from Android 8 anyway, so the calls
 * are unreachable on the devices that lack the symbols -- but "unreachable" is not a link-time statement,
 * which is exactly why they are looked up here instead.
 *
 * Only the two reference-counting calls are needed: the format, the memory types and whether the buffer is
 * importable at all come from `vkGetAndroidHardwareBufferPropertiesANDROID`, which the driver provides.
 *
 * On every other platform the table is simply unavailable, so the import is refused where it is asked for
 * rather than where it is used.
 */

namespace backend::vulkan {

/** The `libandroid.so` entry points the import uses. */
struct AndroidHardwareBufferApi {
  /** Takes a reference on the buffer; the frame that imported it owns that reference. */
  void (*acquire)(void* buffer) = nullptr;
  /** Drops the reference `acquire` took. */
  void (*release)(void* buffer) = nullptr;
  bool available = false;
};

/**
 * @return the entry points, resolving them on first use.
 *
 * The answer is a property of the running system, not of the build: a device below Android 8 reports
 * unavailable, and the Vulkan device then answers `supportsNativeTextureImport(AndroidHardwareBuffer)` with
 * false as well, because it never enabled the extension.
 */
[[nodiscard]] const AndroidHardwareBufferApi& androidHardwareBufferApi();

} // namespace backend::vulkan

#endif // CORAL_BACKEND_VULKAN_ANDROID_HARDWARE_BUFFER_H
