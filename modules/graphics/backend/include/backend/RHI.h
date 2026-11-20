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
 * @file RHI.h
 *
 * Umbrella header for the Coral Render Hardware Interface.
 *
 * Include this from upper layers that want the whole GPU abstraction:
 *
 * ```
 * #include <backend/RHI.h>
 *
 * backend::DeviceConfiguration configuration;
 * configuration.preferredBackend = backend::Backend::OpenGL;
 *
 * backend::Device* device = nullptr;
 * if (!backend::isSuccess(backend::createDevice(configuration, &device))) return false;
 *
 * // Ask what the hardware can do, then choose a technique accordingly.
 * if (!device->supports(backend::Feature::ComputeShader)) { ...use a fragment-shader fallback... }
 * ```
 *
 * The design contract is documented in docs/backend/README.md; the per-backend feature matrix lives in
 * docs/backend/BackendMatrix.md.
 *
 * ## Why the namespace is `backend`
 *
 * The layer is the RHI; the namespace is `backend`. "Render Hardware Interface" is the name of the
 * concept and survives in the requirement codes (`CORAL_BACKEND_SUPPORTS_*`), but the name a caller writes
 * should be the thing it is choosing between. Every GPU backend Coral gains - Vulkan, Metal, and
 * whatever follows - is written against `backend::`, and upper layers never learn which one they got:
 * they see `backend::Device`, not `VkDevice`, `GLuint` or `MTLTexture`.
 *
 * `backend::Backend` therefore reads redundantly, and deliberately so. The alternative, `rhi::Backend`,
 * names the layer rather than the selection.
 */

#ifndef CORAL_BACKEND_RHI_H
#define CORAL_BACKEND_RHI_H

#include <backend/Capabilities.h>
#include <backend/Descriptors.h>
#include <backend/Device.h>
#include <backend/Handles.h>
#include <backend/Pipelines.h>
#include <backend/Types.h>
#include <backend/Validation.h>

#endif // CORAL_BACKEND_RHI_H
