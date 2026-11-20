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

/**
 * @file VulkanAllocator.cpp
 *
 * The one translation unit that compiles Vulkan Memory Allocator.
 *
 * ## Why VMA at all
 *
 * The backend allocates buffers and images on nearly every frame (the renderer pools most of them, but
 * a text frame still creates transient uploads), and Vulkan does not allocate memory for a resource:
 * it allocates a `VkDeviceMemory` block and binds resources into it. Getting that right means tracking
 * memory types, suballocation, alignment and defragmentation. VMA is the library every serious Vulkan
 * application uses for it, is vendored under `third_party/vkmemalloc`, and is header-only apart from
 * this definition.
 *
 * ## Why the function pointers are dynamic
 *
 * `vmaCreateAllocator` is given `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr` and resolves the rest
 * itself, which matches the backend's loader-by-name design: with `VMA_STATIC_VULKAN_FUNCTIONS` left
 * at its default, VMA would reference the global `vkCreateBuffer` and friends and the backend would
 * have to link against a Vulkan loader import library that not every build machine has.
 */

#if defined(_MSC_VER)
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif
