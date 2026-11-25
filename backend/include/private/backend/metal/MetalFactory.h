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

#ifndef CORAL_BACKEND_METAL_FACTORY_H
#define CORAL_BACKEND_METAL_FACTORY_H

/**
 * @file MetalFactory.h
 *
 * The Metal backend's creation entry point, declared in a plain C++ header.
 *
 * `backend::createDevice` in `src/Device.cpp` is a C++ translation unit and must not include a header that imports
 * Metal -- the same reason the Vulkan backend keeps its device header out of `Device.cpp`'s include list except behind
 * `CORAL_BACKEND_HAS_VULKAN`. This header is the whole of what a C++ caller needs: a function that returns a
 * `backend::Device*` and owns the Metal-specific construction. The definition lives in `MetalDevice.mm`.
 */

#include <backend/Device.h>

namespace backend::metal {

/**
 * Creates and initialises a Metal device.
 *
 * @param configuration what the caller needs; see `DeviceConfiguration`.
 * @param outDevice     receives the device, or is left null on failure.
 * @return `Result::Success`, or the reason the device could not come up.
 */
Result createMetalDevice(const DeviceConfiguration& configuration, Device** outDevice);

} // namespace backend::metal

#endif // CORAL_BACKEND_METAL_FACTORY_H
