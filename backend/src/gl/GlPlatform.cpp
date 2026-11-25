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
 * @file GlPlatform.cpp
 *
 * The window-system-independent half of the OpenGL backend.
 *
 * Everything platform-specific lives in `src/gl/platform/`: creating a context, making it current and
 * presenting. This file owns the parts that are the same everywhere:
 *
 *  - validating the requested version against what the driver actually granted,
 *  - querying and parsing the version and extension strings,
 *  - deciding which GLSL version the context accepts.
 *
 * The version check is the important one. Only WGL and GLX fail a too-old request outright; CGL and
 * some EGL drivers hand back an older context without complaint. Reading the version back after the
 * context is current is therefore the only reliable way to know what was granted, and comparing it
 * against the minimum is what stops the backend from reporting the capabilities of a different device
 * than the renderer planned for.
 */

#include "private/backend/gl/GlPlatform.h"

#include "private/backend/gl/GlEntryPoints.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ranges>
#include <sstream>

namespace backend {
namespace gl {
namespace {

/** Reads a GL string, returning an empty string when the driver reports nothing. */
std::string readGLString(const GLenum name) {
  const GLApi& api = getGLApi();
  const GLubyte* value = api.glGetString != nullptr ? api.glGetString(name) : nullptr;
  return value != nullptr ? std::string(reinterpret_cast<const char*>(value)) : std::string();
}

/**
 * Reads the extension list.
 *
 * GL 3.0+ and ES 3.0 expose GL_NUM_EXTENSIONS and `glGetStringi`, which is both faster and immune to
 * the GL_EXTENSIONS string length limit that has bitten drivers in the past. The legacy string is the
 * fallback for a context that predates the indexed form, which matters because the version check
 * below may already have failed by the time this runs.
 */
std::vector<std::string> readExtensions(const GlDialect dialect) {
  std::vector<std::string> extensions;
  const GLApi& api = getGLApi();

  const bool indexedAvailable = (dialect == GlDialect::Desktop || dialect == GlDialect::ES) &&
                                api.glGetStringi != nullptr && api.glGetIntegerv != nullptr;
  if (indexedAvailable) {
    GLint count = 0;
    api.glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    if (count > 0) {
      extensions.reserve(static_cast<size_t>(count));
      for (GLint index = 0; index < count; ++index) {
        const GLubyte* value = api.glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(index));
        if (value != nullptr) extensions.emplace_back(reinterpret_cast<const char*>(value));
      }
      if (!extensions.empty()) return extensions;
    }
  }

  if (api.glGetString == nullptr) return extensions;
  const GLubyte* value = api.glGetString(GL_EXTENSIONS);
  if (value == nullptr) return extensions;

  std::istringstream stream(reinterpret_cast<const char*>(value));
  std::string token;
  while (stream >> token) extensions.push_back(token);
  return extensions;
}

/** Parses the first "major.minor" in @p text, e.g. "4.1 ATI-4.6.20" or "OpenGL ES 3.2 Mesa". */
void parseVersionString(const std::string& text, uint32_t* major, uint32_t* minor) {
  *major = 0;
  *minor = 0;

  size_t index = 0;
  while (index < text.size() && !std::isdigit(static_cast<unsigned char>(text[index]))) ++index;
  if (index >= text.size()) {
    return;
  }

  *major = static_cast<uint32_t>(std::atoi(text.c_str() + index));
  while (index < text.size() && text[index] != '.') ++index;
  if (index >= text.size()) {
    return;
  }
  ++index;
  *minor = static_cast<uint32_t>(std::atoi(text.c_str() + index));
}

/**
 * @return the GLSL version a context of @p version accepts, in SPIRV-Cross's numbering.
 *
 * The values are the ones that appear in a `#version` directive: 300 for GLSL ES 3.00, 410 for GLSL
 * 4.10 core, and so on. Rounding *down* to a version the RHI knows is deliberate: asking for GLSL
 * 4.60 from a 4.60 context would work, but SPIRV-Cross's output is checked against 4.10 semantics, so
 * the target is pinned to what has been verified.
 */
uint32_t glslVersionFor(const GlDialect dialect, const GlVersion version) {
  if (dialect == GlDialect::ES) {
    if (version.atLeast({ .major = 3, .minor = 2 })) {
      return 320;
    }
    if (version.atLeast({ .major = 3, .minor = 1 })) {
      return 310;
    }
    return 300;
  }

  if (version.atLeast({ .major = 4, .minor = 6 })) {
    return 460;
  }
  if (version.atLeast({ .major = 4, .minor = 5 })) {
    return 450;
  }
  if (version.atLeast({ .major = 4, .minor = 4 })) {
    return 440;
  }
  if (version.atLeast({ .major = 4, .minor = 3 })) {
    return 430;
  }
  if (version.atLeast({ .major = 4, .minor = 2 })) {
    return 420;
  }
  return 410;
}

} // namespace anonymous

GlPlatform::GlPlatform() = default;

GlPlatform::~GlPlatform() { shutdown(); }

Result GlPlatform::initialize(const SurfaceDescriptor& descriptor, const GlDialect dialect, const GlVersion minimum,
  const bool debugContext) {
  mDescriptor = descriptor;

  const Result created = createPlatformContext(descriptor, dialect, minimum, debugContext, &mContext);
  if (!isSuccess(created)) {
    return created;
  }

  // The context must be current before anything can be queried about it.
  const Result current = makeCurrent();
  if (!isSuccess(current)) {
    destroyPlatformContext(&mContext);
    return current;
  }

  // Load the entry points for the dialect. This happens after the context is current because the
  // window-system loaders only return driver functions for a context that exists.
  GLApi& api = getGLApi();
  if (!api.load(dialect, mContext.loader)) {
    destroyPlatformContext(&mContext);
    return Result::Unsupported;
  }

  // ---------------------------------------------------------------- identify the context
  mVendor = readGLString(GL_VENDOR);
  mRenderer = readGLString(GL_RENDERER);
  mVersionString = readGLString(GL_VERSION);
  mShadingLanguageVersion = readGLString(GL_SHADING_LANGUAGE_VERSION);
  mExtensions = readExtensions(dialect);

  // GL 3.0+ and ES 3.0 report the version numerically, which is exact; the version string is the
  // fallback for anything older. A driver that reports neither is treated as unusable, because the
  // RHI needs at least one of the two to make any capability claim.
  uint32_t major = 0;
  uint32_t minor = 0;
  if (api.glGetIntegerv != nullptr) {
    GLint value = 0;
    api.glGetIntegerv(GL_MAJOR_VERSION, &value);
    if (value > 0) major = static_cast<uint32_t>(value);
    api.glGetIntegerv(GL_MINOR_VERSION, &value);
    if (value > 0) minor = static_cast<uint32_t>(value);
  }
  if (major == 0) parseVersionString(mVersionString, &major, &minor);

  mContext.dialect = dialect;
  // Prefer the version the driver reports over the one the platform layer guessed: CGL and some EGL
  // drivers grant a different version than requested.
  if (major != 0) {
    mContext.version = { .major = major, .minor = minor };
  }
  mContext.glslVersion = glslVersionFor(dialect, mContext.version);

  {
    std::ostringstream stream;
    stream << (dialect == GlDialect::ES ? "OpenGL ES " : "OpenGL ") << mContext.version.major << "."
      << mContext.version.minor;
    if (mContext.coreProfile && dialect == GlDialect::Desktop) {
      stream << " core";
    }
    mApiVersion = stream.str();
  }

  // ---------------------------------------------------------------- enforce the minimum
  if (!mContext.version.atLeast(minimum)) {
    // This is the case the whole file exists for. A context below the minimum would report
    // capabilities for a different device than the renderer planned for, and the failures that
    // follow surface far from their cause, so the mismatch is refused here instead.
    destroyPlatformContext(&mContext);
    return Result::Unsupported;
  }

  return Result::Success;
}

void GlPlatform::shutdown() {
  destroyPlatformContext(&mContext);
  mContext = GlPlatformContext{};
  mExtensions.clear();
  mVendor.clear();
  mRenderer.clear();
  mVersionString.clear();
  mShadingLanguageVersion.clear();
  mApiVersion.clear();
}

Result GlPlatform::makeCurrent() {
  if (mLost) {
    return Result::DeviceLost;
  }

  const Result result = makePlatformContextCurrent(mContext);
  if (result == Result::DeviceLost) {
    markLost();
  }
  return result;
}

Result GlPlatform::bindSurface(const SurfaceDescriptor& descriptor) {
  if (mLost) {
    return Result::DeviceLost;
  }
  const Result result = bindPlatformSurface(mContext, descriptor);
  if (isSuccess(result)) {
    mDescriptor = descriptor;
  } else if (result == Result::DeviceLost) {
    markLost();
  }
  return result;
}

Result GlPlatform::releaseCurrent() {
  if (mLost) {
    return Result::DeviceLost;
  }
  // Safe on a thread with nothing current: every platform's release is a no-op there.
  return releasePlatformContextCurrent(mContext);
}

bool GlPlatform::supportsMailboxPresent() const noexcept {
  return platformSupportsMailboxPresent();
}

Result GlPlatform::setSwapInterval(const int interval) {
  if (mLost) {
    return Result::DeviceLost;
  }
  return setPlatformSwapInterval(mContext, interval);
}

Result GlPlatform::swapBuffers() {
  if (mLost) {
    return Result::DeviceLost;
  }

  const Result result = swapPlatformContextBuffers(mContext);
  if (result == Result::DeviceLost) {
    markLost();
  }
  return result;
}

Extent2D GlPlatform::querySurfaceSize() const {
  // A pinned size wins: the caller told us what the surface is, and re-querying every frame is both
  // slower and racy against a concurrent window resize.
  if (mDescriptor.width != 0 && mDescriptor.height != 0) {
    return { .width = mDescriptor.width, .height = mDescriptor.height };
  }
  return queryPlatformSurfaceSize(mContext, mDescriptor);
}

void GlPlatform::queryContextInfo(std::string* vendor, std::string* renderer, std::string* versionString,
  std::string* shadingLanguageVersion, std::vector<std::string>* extensions) const {
  if (vendor != nullptr) {
    *vendor = mVendor;
  }
  if (renderer != nullptr) {
    *renderer = mRenderer;
  }
  if (versionString != nullptr) {
    *versionString = mVersionString;
  }
  if (shadingLanguageVersion != nullptr) {
    *shadingLanguageVersion = mShadingLanguageVersion;
  }
  if (extensions != nullptr) {
    *extensions = mExtensions;
  }
}

bool GlPlatform::hasExtension(const char* extension) const noexcept {
  if (extension == nullptr) {
    return false;
  }
  return std::ranges::any_of(mExtensions, [extension](const std::string& candidate) { return candidate == extension; });
}

void GlPlatform::appendExtensions(std::string& out) const {
  for (const std::string& extension: mExtensions) {
    out += "    ";
    out += extension;
    out += '\n';
  }
  if (mExtensions.empty()) out += "    (none reported)\n";
}

} // namespace gl
} // namespace backend
