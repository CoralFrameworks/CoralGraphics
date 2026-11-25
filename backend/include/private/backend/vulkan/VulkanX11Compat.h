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

#ifndef CORAL_BACKEND_VULKAN_X11_COMPAT_H
#define CORAL_BACKEND_VULKAN_X11_COMPAT_H

/**
 * @file VulkanX11Compat.h
 *
 * Undoes the Xlib macros that collide with the RHI's own enumerators.
 *
 * On Linux the Vulkan headers are included with `VK_USE_PLATFORM_XLIB_KHR`, because the backend has
 * to create a `VkSurfaceKHR` from an Xlib window. `<vulkan/vulkan.h>` then pulls in `<X11/Xlib.h>`,
 * and `<X11/X.h>` defines a handful of names as object-like macros. Three of them are also enumerator
 * names in `backend`:
 *
 *   | Xlib macro | RHI enumerator |
 *   |---|---|
 *   | `Success` | `Result::Success` |
 *   | `None`    | `CullMode::None` |
 *   | `Always`  | `CompareOp::Always` |
 *
 * A macro is expanded regardless of the `::` next to it, so `Result::Success` becomes `Result::0` and
 * the translation unit stops parsing. The macros are of no use to this backend, which never calls
 * Xlib directly outside `VulkanPlatform.cpp`, so they are removed here. The Xlib type macros (`Bool`,
 * `Status`, `True`, `False`) are left alone: they do not collide and the platform file may rely on
 * them.
 *
 * Include this **after** `<vulkan/vulkan.h>` and before the first use of an affected enumerator.
 */

#ifdef Success
#undef Success
#endif

#ifdef None
#undef None
#endif

#ifdef Always
#undef Always
#endif

#endif // CORAL_BACKEND_VULKAN_X11_COMPAT_H
