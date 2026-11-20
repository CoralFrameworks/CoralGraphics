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
 * @file GlPlatformNull.cpp
 *
 * The window-system layer for platforms the backend has no implementation for.
 *
 * It exists so that "this platform has no OpenGL" is a run-time result rather than a build error. That
 * matters because the RHI's device factory walks a priority list: a platform whose first choice is
 * unavailable is supposed to move on to the next backend, and a build that refuses to configure takes
 * that decision away from the application.
 *
 * This file is compiled only when no platform implementation matched (see the `elseif` chain in
 * modules/graphics/CMakeLists.txt). Everything returns `Result::Unsupported`, and it deliberately does not fall
 * back to a default loader: resolving GL entry points through a library that is not there would crash
 * rather than report.
 */

#include "private/backend/gl/GlPlatform.h"

#include <backend/Types.h>

namespace backend {
namespace gl {

// The name of this window-system layer, which every other implementation also provides. It was missing here, and only
// here, which went unnoticed because nothing called it: a platform that reached this file would have failed to link the
// moment anything did. "None" rather than a platform name because there is no window system involved -- see the note at
// the top of the file for why that is a runtime result rather than a build failure.
const char* GlPlatform::backendName() noexcept { return "None"; }

Result createPlatformContext(const SurfaceDescriptor& descriptor, GlDialect dialect, GlVersion minimum,
        bool debugContext, GlPlatformContext* context) {
  (void) descriptor;
  (void) dialect;
  (void) minimum;
  (void) debugContext;
  if (context != nullptr) *context = GlPlatformContext{};
  return Result::Unsupported;
}

void destroyPlatformContext(GlPlatformContext* context) {
  if (context != nullptr) *context = GlPlatformContext{};
}

Result makePlatformContextCurrent(const GlPlatformContext& context) {
  (void) context;
  return Result::Unsupported;
}

bool platformSupportsMailboxPresent() noexcept {
  // False on purpose: a swap interval of 0 stops waiting for vertical sync, it does not drop a frame that the compositor has
  // not consumed yet. Reporting Mailbox from here would be a promise this platform cannot keep.
  return false;
}

Result setPlatformSwapInterval(const GlPlatformContext&, const int) {
  // Nothing presents on this platform, so every interval is already satisfied.
  return Result::Success;
}

Result swapPlatformContextBuffers(const GlPlatformContext& context) {
  (void) context;
  return Result::Unsupported;
}

Extent2D queryPlatformSurfaceSize(const GlPlatformContext& context, const SurfaceDescriptor& descriptor) {
  (void) context;
  return Extent2D{ descriptor.width, descriptor.height };
}

Result bindPlatformSurface(GlPlatformContext& context, const SurfaceDescriptor& descriptor) {
  (void) context;
  (void) descriptor;
  return Result::Unsupported;
}

} // namespace gl
} // namespace backend
