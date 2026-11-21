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

#include <backend/Device.h>

#include "GlslCompiler.h"

#include <cstdlib>
#include <cstring>
#include <string>

#if CORAL_BACKEND_HAS_OPENGL
#include "private/backend/gl/GlDevice.h"
#endif

#if CORAL_BACKEND_HAS_VULKAN
#include "private/backend/vulkan/VulkanDevice.h"
#endif

#if CORAL_BACKEND_HAS_METAL
// A plain C++ header: the Metal implementation is Objective-C++ and must not leak Metal types into this translation
// unit. See MetalFactory.h.
#include "private/backend/metal/MetalFactory.h"
#endif

namespace backend {
namespace {

/**
 * Which backends were compiled into this build.
 *
 * A backend that is compiled out is reported as unavailable rather than absent, so the factory can
 * skip it silently. A priority list is a statement of preference: asking for Vulkan on a build
 * without it should fall through to OpenGL, not fail.
 */
struct BackendAvailability {
  Backend backend;
  bool available;
  [[maybe_unused]] const char* name;
};

constexpr BackendAvailability kBackends[] = {
#if CORAL_BACKEND_HAS_OPENGL
  { Backend::OpenGL, true, "gl" },
#else
  { Backend::OpenGL, false, "gl" },
#endif
#if CORAL_BACKEND_HAS_VULKAN
  { Backend::Vulkan, true, "vulkan" },
#else
  { Backend::Vulkan, false, "vulkan" },
#endif
#if CORAL_BACKEND_HAS_METAL
  { Backend::Metal, true, "metal" },
#else
  { Backend::Metal, false, "metal" },
#endif
};

/** @return the entry for @p backend, or nullptr when it is not in the table. */
const BackendAvailability* findBackend(Backend backend) {
  for (const BackendAvailability& entry: kBackends) {
    if (entry.backend == backend) {
      return &entry;
    }
  }
  return nullptr;
}

/**
 * Default backend priority for the platform this build targets.
 *
 * The list is ordered by what the platform does best, and the factory walks it until one backend
 * succeeds:
 *
 *  - **Windows**: Vulkan, then OpenGL 4.1+. Those two between them cover every device the engine can
 *    run on, which is why Direct3D is deliberately absent rather than merely unwritten.
 *  - **Linux**: Vulkan, then OpenGL 4.1+.
 *  - **macOS**: Metal, then Vulkan (via MoltenVK), then OpenGL 4.1+. OpenGL is last because Apple
 *    deprecated it, but it is still the only always-present option on older systems.
 *  - **Android**: Vulkan, then OpenGL ES 3.0+.
 *  - **iOS**: Metal, then OpenGL ES 3.0+.
 *
 * The OpenGL entry means whichever dialect the platform has - 4.1+ core on the desktop platforms, ES
 * 3.0+ on the mobile ones. The list does not name a dialect because there is nothing to choose: see
 * `getPlatformGlDialect()`.
 */
const Backend* defaultPriority(uint32_t* outCount) {
  // ## A diagnostic override, so a regression run can pin the backend
  //
  // `CORAL_RHI_BACKEND=opengl|vulkan|metal` replaces the platform's preference order with a one-element
  // list. That is what makes it possible to run the *same* suite against each backend in a build where
  // more than one is compiled in - the platform default can only name one of them, and a test that
  // silently exercised whichever backend came first would leave the other unverified. It is read once
  // and deliberately does not fall back: pinning a backend is a statement about which one to test.
  static const Backend pinned = [] {
    const char* name = std::getenv("CORAL_RHI_BACKEND");
    if (name == nullptr) return Backend::Null;
    if (std::strcmp(name, "opengl") == 0 || std::strcmp(name, "gl") == 0) return Backend::OpenGL;
    if (std::strcmp(name, "vulkan") == 0 || std::strcmp(name, "vk") == 0) return Backend::Vulkan;
    if (std::strcmp(name, "metal") == 0) return Backend::Metal;
    return Backend::Null;
  }();
  if (pinned != Backend::Null) {
    static const Backend pinnedList[] = { pinned, Backend::Null };
    *outCount = 1;
    return pinnedList;
  }

  static constexpr Backend hostPriority[] = {
#if defined(CORAL_BUILD_FOR_WIN)
    Backend::Vulkan,
    Backend::OpenGL,
#elif defined(CORAL_BUILD_FOR_ANDROID)
    Backend::Vulkan,
    Backend::OpenGL,
#elif defined(CORAL_BUILD_FOR_IOS)
    Backend::Metal,
    Backend::OpenGL,
#elif defined(CORAL_BUILD_FOR_MAC)
    Backend::Metal,
    Backend::Vulkan,
    Backend::OpenGL,
#else
    // Linux and anything else unix-like.
    Backend::Vulkan,
    Backend::OpenGL,
#endif
  };
  *outCount = std::size(hostPriority);
  return hostPriority;
}

bool backendAvailable(const Backend backend) {
  const BackendAvailability* entry = findBackend(backend);
  return entry != nullptr && entry->available;
}

/** Creates a device for one backend. Returns Result::Unsupported when the backend cannot be built. */
Result createBackendDevice(const Backend backend, const DeviceConfiguration& configuration, Device** outDevice) {
  switch (backend) {
    case Backend::OpenGL: {
#if CORAL_BACKEND_HAS_OPENGL
      // Owned while it is being built, and handed over only once it works. With `new` and a `delete` on the failure path
      // the buffer leaked if `initialize` threw -- unlikely, and exactly the kind of thing that is invisible until it is
      // not. `release` at the end is the handover, and it is the only place ownership leaves this function.
      auto device = std::make_unique<GLDevice>();
      const Result result = device->initialize(configuration);
      if (!isSuccess(result)) {
        return result;
      }
      *outDevice = device.release();
      return Result::Success;
#else
      return Result::Unsupported;
#endif
    }

    case Backend::Vulkan: {
#if CORAL_BACKEND_HAS_VULKAN
      // Ownership is handed over only once `initialize` succeeded, for the same reason as the OpenGL
      // branch: a failure that leaked the half-built device would be invisible until it mattered.
      auto device = std::make_unique<vulkan::VulkanDevice>();
      const Result result = device->initialize(configuration);
      if (!isSuccess(result)) {
        return result;
      }
      *outDevice = device.release();
      return Result::Success;
#else
      return Result::Unsupported;
#endif
    }

    case Backend::Metal: {
#if CORAL_BACKEND_HAS_METAL
      // The construction lives in MetalDevice.mm, behind a C++ factory, because this translation unit cannot include a
      // header that imports Metal. The ownership handover is the same as the two branches above.
      return metal::createMetalDevice(configuration, outDevice);
#else
      return Result::Unsupported;
#endif
    }

    case Backend::Null:
      // Not implemented, and not expected: `Null` is the factory's "no preference" sentinel, never a device.
      return Result::Unsupported;
  }
  return Result::Unsupported;
}

} // namespace anonymous

const char* getShaderCompilerVersion() noexcept { return getGlslCompilerVersion(); }

bool isShaderCompilerAvailable() noexcept { return isGlslCompilerAvailable(); }

bool isBackendAvailable(const Backend backend) noexcept { return backendAvailable(backend); }

Backend getDefaultBackend() noexcept {
  uint32_t count = 0;
  const Backend* priority = defaultPriority(&count);
  for (uint32_t index = 0; index < count; ++index) {
    if (backendAvailable(priority[index])) {
      return priority[index];
    }
  }
  return Backend::Null;
}

const char* getDefaultBackendName() noexcept { return toString(getDefaultBackend()); }

Result createDevice(const DeviceConfiguration& configuration, Device** outDevice, DebugMessageCallback debugCallback,
        void* userData) {
  if (outDevice == nullptr) {
    return Result::InvalidArgument;
  }
  *outDevice = nullptr;

  // Build the attempt order: the caller's explicit priority list when supplied, otherwise the
  // platform default. A non-Null preferredBackend is moved to the front so that "prefer X but fall
  // back" needs only one field.
  Backend order[8] = {};
  uint32_t orderCount = 0;

  if (configuration.backendPriority != nullptr) {
    for (uint32_t index = 0; index < 8 && configuration.backendPriority[index] != Backend::Null; ++index) {
      order[orderCount++] = configuration.backendPriority[index];
    }
  }
  if (orderCount == 0) {
    uint32_t count = 0;
    const Backend* priority = defaultPriority(&count);
    for (uint32_t index = 0; index < count && index < 8; ++index) order[orderCount++] = priority[index];
  }

  if (configuration.preferredBackend != Backend::Null) {
    // Promote the preference to the front, shifting the rest. Duplicates are collapsed so the same
    // backend is never attempted twice.
    Backend promoted[8] = { configuration.preferredBackend };
    uint32_t promotedCount = 1;
    for (uint32_t index = 0; index < orderCount && promotedCount < 8; ++index) {
      if (order[index] == configuration.preferredBackend) {
        continue;
      }
      promoted[promotedCount++] = order[index];
    }
    for (uint32_t index = 0; index < promotedCount; ++index) {
      order[index] = promoted[index];
    }
    orderCount = promotedCount;
  }

  std::string failures;
  const auto report = [&](const MessageSeverity severity, const std::string& message) {
    if (debugCallback != nullptr) {
      debugCallback(severity, message.c_str(), userData);
    }
  };

  for (uint32_t index = 0; index < orderCount; ++index) {
    const Backend backend = order[index];
    if (backend == Backend::Null) {
      continue;
    }
    if (!backendAvailable(backend)) {
      continue;
    }

    Device* device = nullptr;
    const Result result = createBackendDevice(backend, configuration, &device);
    if (isSuccess(result) && device != nullptr) {
      *outDevice = device;
      return Result::Success;
    }

    if (!failures.empty()) {
      failures += "; ";
    }
    failures += toString(backend);
    failures += " failed (";
    switch (result) {
      case Result::Unsupported:
        failures += "unsupported on this platform or missing required features";
        break;
      case Result::OutOfMemory:
        failures += "out of memory";
        break;
      case Result::DeviceLost:
        failures += "device lost";
        break;
      default:
        failures += "driver error";
        break;
    }
    failures += ')';

    if (!configuration.allowFallback) {
      break;
    }
  }

  std::string message = "no RHI backend could be created";
  if (!failures.empty()) {
    message += ": ";
    message += failures;
  }
  report(MessageSeverity::Error, message);
  return Result::Unsupported;
}

} // namespace backend
