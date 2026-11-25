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
 * @file AndroidHardwareBufferOutputNode.cpp
 *
 * Vulkan renders the final frame into an AHardwareBuffer. The frame is then handed to MediaCodec's
 * input Surface by an EGLImage/GLES blit, which is the only public NDK bridge from an
 * AHardwareBuffer to a BufferQueue. Both halves are GPU work: the CPU never sees the pixels.
 */

// The EGL/GLES types have to be the real platform ones: the graphics backend has its own opaque
// aliases, and including its headers first would redeclare `EGLDisplay` and friends as `void*`.
#define EGL_EGLEXT_PROTOTYPES 1
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <effect/platform/android/AndroidHardwareBufferOutputNode.h>

#include <effect/platform/android/AndroidHardwareBuffer.h>

#include "effect/EffectInternal.h"
#include "effect/EffectRenderCore.h"

#include <android/log.h>

#include <atomic>
#include <mutex>
#include <string>

namespace coral {
namespace {

using detail::FrameContext;
using detail::FrameView;

const char* const kVertexSource = R"(#version 300 es
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
  gl_Position = vec4(aPosition, 0.0, 1.0);
  vTexCoord = aTexCoord;
}
)";

const char* const kFragmentSource = R"(#version 300 es
precision mediump float;
in vec2 vTexCoord;
uniform sampler2D uTexture;
out vec4 outColor;
void main() {
  outColor = vec4(texture(uTexture, vTexCoord).rgb, 1.0);
}
)";

GLuint compileShader(const GLenum type, const char* source, std::string& error) {
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled == GL_FALSE) {
    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<size_t>(length > 1 ? length : 1), '\0');
    glGetShaderInfoLog(shader, length, nullptr, log.data());
    error = "the AHardwareBuffer present shader did not compile: " + log;
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

} // namespace

struct AndroidHardwareBufferOutputNode::State {
  State(ANativeWindow* encoderWindow, const uint32_t width, const uint32_t height)
    : window(encoderWindow),
      width(width),
      height(height) {
    if (window != nullptr) {
      ANativeWindow_acquire(window);
    }
  }

  ~State() {
    if (window != nullptr) {
      ANativeWindow_release(window);
    }
  }

  ANativeWindow* window = nullptr;
  std::shared_ptr<AndroidHardwareBufferAllocation> allocation;
  uint32_t width = 0;
  uint32_t height = 0;
  mutable std::mutex mutex;
  EffectOutputConfig config;
  std::atomic<int64_t> presentationTimeUs { -1 };
  std::atomic_bool presentFailed { false };
};

namespace {

class HardwareBufferOutputRuntime final : public detail::OutputRuntime {
public:
  explicit HardwareBufferOutputRuntime(std::shared_ptr<AndroidHardwareBufferOutputNode::State> state)
    : mState(std::move(state)) {
  }

  ~HardwareBufferOutputRuntime() override {
    releaseResources();
  }

  bool targetSize(const uint32_t sourceWidth, const uint32_t sourceHeight, uint32_t& width,
    uint32_t& height) const override {
    static_cast<void>(sourceWidth);
    static_cast<void>(sourceHeight);
    width = mState->width;
    height = mState->height;
    return true;
  }

  EffectPixelFormat targetFormat() const override {
    return EffectPixelFormat::RGBA8;
  }

  EffectScaleMode targetScaleMode() const override {
    return EffectScaleMode::Stretch;
  }

  bool prepare(detail::RenderCore& core, std::string& error) override {
    backend::Device* const device = core.device();
    if (device == nullptr) {
      error = "the engine has no device";
      return false;
    }
    if (!device->supportsNativeTextureImport(backend::NativeTextureKind::AndroidHardwareBuffer)) {
      error = "the backend cannot import an AHardwareBuffer (Vulkan is required for the Android encoder output)";
      return false;
    }
    if (mState->allocation == nullptr) {
      mState->allocation = allocateAndroidHardwareBufferForGpu(mState->width, mState->height,
        AndroidHardwareBufferFormat::RGBA8, &error);
      if (mState->allocation == nullptr) {
        return false;
      }
    }
    if (!mImported.valid()) {
      backend::NativeTextureDescriptor descriptor;
      descriptor.kind = backend::NativeTextureKind::AndroidHardwareBuffer;
      descriptor.platformHandle = mState->allocation->buffer();
      descriptor.width = mState->width;
      descriptor.height = mState->height;
      descriptor.format = backend::PixelFormat::R8G8B8A8_UNORM;
      descriptor.writeback = true;
      const backend::Result result = device->importNativeTexture(descriptor, &mImported);
      if (!backend::isSuccess(result) || !mImported.valid()) {
        error = "the backend could not import the encoder's AHardwareBuffer";
        return false;
      }
    }
    mDevice = device;
    if (!ensureGl(error)) {
      return false;
    }
    return true;
  }

  bool consume(detail::FrameContext& context, const FrameView& view, std::string& error) override {
    if (!mImported.valid() || context.commands == nullptr) {
      error = "the AHardwareBuffer output is not prepared";
      return false;
    }
    if (view.format != EffectPixelFormat::RGBA8) {
      error = "the AHardwareBuffer output needs an RGBA8 frame";
      return false;
    }
    if (view.width != mState->width || view.height != mState->height) {
      error = "the final frame does not match the encoder size";
      return false;
    }
    backend::TextureCopyRegion region;
    region.size = { mState->width, mState->height, 1 };
    if (!backend::isSuccess(context.commands->copyTextureToTexture(view.texture, mImported, region))) {
      error = "the final frame could not be copied into the AHardwareBuffer";
      return false;
    }
    mCopied = true;
    return true;
  }

  void afterSubmit(const std::shared_ptr<detail::GpuSubmission>& submission) override {
    if (submission != nullptr) {
      submission->wait();
    }
    if (!mCopied) {
      return;
    }
    mCopied = false;
    if (presentWithRecovery()) {
      mState->presentFailed.store(false, std::memory_order_release);
    } else {
      mState->presentFailed.store(true, std::memory_order_release);
      __android_log_print(ANDROID_LOG_ERROR, "CoralEffect",
        "the AHardwareBuffer present to the encoder surface failed");
    }
  }

  void afterAbort() override {
    mCopied = false;
  }

  void releaseResources() override {
    if (mTexture != 0) {
      glDeleteTextures(1, &mTexture);
      mTexture = 0;
    }
    if (mVao != 0) {
      glDeleteVertexArrays(1, &mVao);
      mVao = 0;
    }
    if (mVbo != 0) {
      glDeleteBuffers(1, &mVbo);
      mVbo = 0;
    }
    if (mProgram != 0) {
      glDeleteProgram(mProgram);
      mProgram = 0;
    }
    if (mImage != EGL_NO_IMAGE_KHR && mDisplay != EGL_NO_DISPLAY) {
      eglDestroyImageKHR(mDisplay, mImage);
      mImage = EGL_NO_IMAGE_KHR;
    }
    if (mDisplay != EGL_NO_DISPLAY) {
      eglMakeCurrent(mDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (mSurface != EGL_NO_SURFACE) {
        eglDestroySurface(mDisplay, mSurface);
        mSurface = EGL_NO_SURFACE;
      }
      if (mContext != EGL_NO_CONTEXT) {
        eglDestroyContext(mDisplay, mContext);
        mContext = EGL_NO_CONTEXT;
      }
      eglTerminate(mDisplay);
      mDisplay = EGL_NO_DISPLAY;
    }
    if (mDevice != nullptr && mImported.valid()) {
      mDevice->destroyTexture(mImported);
    }
    mImported = backend::TextureHandle::invalid();
    mDevice = nullptr;
  }

private:
  bool ensureGl(std::string& error) {
    if (mGlReady) {
      return true;
    }
    if (mDisplay == EGL_NO_DISPLAY) {
      mDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
      if (mDisplay == EGL_NO_DISPLAY || eglInitialize(mDisplay, nullptr, nullptr) == EGL_FALSE) {
        error = "the EGL display could not be initialized";
        return false;
      }
      eglBindAPI(EGL_OPENGL_ES_API);
      const EGLint configAttributes[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
      };
      EGLint configCount = 0;
      if (eglChooseConfig(mDisplay, configAttributes, &mConfig, 1, &configCount) == EGL_FALSE ||
          configCount == 0) {
        error = "the EGL configuration for the encoder surface was not found";
        return false;
      }
      const EGLint contextAttributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
      mContext = eglCreateContext(mDisplay, mConfig, EGL_NO_CONTEXT, contextAttributes);
      if (mContext == EGL_NO_CONTEXT) {
        error = "the EGL context for the encoder surface could not be created";
        return false;
      }
      mSurface = eglCreateWindowSurface(mDisplay, mConfig, mState->window, nullptr);
      if (mSurface == EGL_NO_SURFACE) {
        error = "the EGL surface for the encoder window could not be created";
        return false;
      }
      if (eglMakeCurrent(mDisplay, mSurface, mSurface, mContext) == EGL_FALSE) {
        error = "the EGL context for the encoder window could not be made current";
        return false;
      }
    } else if (eglMakeCurrent(mDisplay, mSurface, mSurface, mContext) == EGL_FALSE) {
      error = "the EGL context for the encoder window could not be made current";
      return false;
    }

    if (mImage == EGL_NO_IMAGE_KHR) {
      using GetNativeClientBufferFn = EGLClientBuffer (*)(const AHardwareBuffer*);
      static const auto getNativeClientBuffer = reinterpret_cast<GetNativeClientBufferFn>(
        eglGetProcAddress("eglGetNativeClientBufferANDROID"));
      if (getNativeClientBuffer == nullptr) {
        error = "the EGL driver has no eglGetNativeClientBufferANDROID";
        return false;
      }
      const EGLClientBuffer clientBuffer = getNativeClientBuffer(mState->allocation->buffer());
      if (clientBuffer == nullptr) {
        error = "the AHardwareBuffer could not become an EGL client buffer";
        return false;
      }
      const EGLint imageAttributes[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
      mImage = eglCreateImageKHR(mDisplay, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, clientBuffer,
        imageAttributes);
      if (mImage == EGL_NO_IMAGE_KHR) {
        error = "the AHardwareBuffer EGLImage could not be created";
        return false;
      }
      glGenTextures(1, &mTexture);
      glBindTexture(GL_TEXTURE_2D, mTexture);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, mImage);
    }

    if (mProgram == 0) {
      const GLuint vertex = compileShader(GL_VERTEX_SHADER, kVertexSource, error);
      if (vertex == 0) return false;
      const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, kFragmentSource, error);
      if (fragment == 0) {
        glDeleteShader(vertex);
        return false;
      }
      mProgram = glCreateProgram();
      glAttachShader(mProgram, vertex);
      glAttachShader(mProgram, fragment);
      glLinkProgram(mProgram);
      glDeleteShader(vertex);
      glDeleteShader(fragment);
      GLint linked = GL_FALSE;
      glGetProgramiv(mProgram, GL_LINK_STATUS, &linked);
      if (linked == GL_FALSE) {
        glDeleteProgram(mProgram);
        mProgram = 0;
        error = "the AHardwareBuffer present program did not link";
        return false;
      }
      glUseProgram(mProgram);
      glUniform1i(glGetUniformLocation(mProgram, "uTexture"), 0);

      constexpr float kQuad[] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
      };
      glGenVertexArrays(1, &mVao);
      glBindVertexArray(mVao);
      glGenBuffers(1, &mVbo);
      glBindBuffer(GL_ARRAY_BUFFER, mVbo);
      glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
      glEnableVertexAttribArray(0);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, nullptr);
      glEnableVertexAttribArray(1);
      glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4,
        reinterpret_cast<const void*>(sizeof(float) * 2));
    }
    mGlReady = true;
    return true;
  }

  bool presentWithRecovery() {
    if (present()) {
      return true;
    }
    if (mState->window == nullptr || mDisplay == EGL_NO_DISPLAY || mConfig == nullptr) {
      return false;
    }
    // A lost or stale window surface is recoverable without rebuilding the graph: tear the EGL
    // surface down, attach the same encoder window again, and redraw the frame that is still in
    // the AHardwareBuffer.
    eglMakeCurrent(mDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (mSurface != EGL_NO_SURFACE) {
      eglDestroySurface(mDisplay, mSurface);
      mSurface = EGL_NO_SURFACE;
    }
    mSurface = eglCreateWindowSurface(mDisplay, mConfig, mState->window, nullptr);
    if (mSurface == EGL_NO_SURFACE ||
        eglMakeCurrent(mDisplay, mSurface, mSurface, mContext) == EGL_FALSE) {
      return false;
    }
    return present();
  }

  bool present() {
    if (!mGlReady || mTexture == 0) {
      return false;
    }
    if (eglMakeCurrent(mDisplay, mSurface, mSurface, mContext) == EGL_FALSE) {
      return false;
    }
    glViewport(0, 0, static_cast<GLsizei>(mState->width), static_cast<GLsizei>(mState->height));
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(mProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mTexture);
    glBindVertexArray(mVao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    int64_t timeUs = mState->presentationTimeUs.exchange(-1, std::memory_order_relaxed);
    if (timeUs < 0) {
      // A failed swap consumes the time stamp; keep it for the retry.
      timeUs = mRetryPresentationTimeUs;
    } else {
      mRetryPresentationTimeUs = timeUs;
    }
    if (timeUs >= 0) {
      eglPresentationTimeANDROID(mDisplay, mSurface, timeUs * 1000);
    }
    if (eglSwapBuffers(mDisplay, mSurface) == EGL_TRUE) {
      mRetryPresentationTimeUs = -1;
      return true;
    }
    return false;
  }

  std::shared_ptr<AndroidHardwareBufferOutputNode::State> mState;
  backend::Device* mDevice = nullptr;
  backend::TextureHandle mImported = backend::TextureHandle::invalid();
  EGLDisplay mDisplay = EGL_NO_DISPLAY;
  EGLConfig mConfig = nullptr;
  EGLContext mContext = EGL_NO_CONTEXT;
  EGLSurface mSurface = EGL_NO_SURFACE;
  EGLImageKHR mImage = EGL_NO_IMAGE_KHR;
  GLuint mTexture = 0;
  GLuint mProgram = 0;
  GLuint mVao = 0;
  GLuint mVbo = 0;
  bool mGlReady = false;
  bool mCopied = false;
  int64_t mRetryPresentationTimeUs = -1;
};

} // namespace

AndroidHardwareBufferOutputNode::AndroidHardwareBufferOutputNode(ANativeWindow* const encoderWindow,
  const uint32_t width, const uint32_t height)
  : mState(std::make_shared<State>(encoderWindow, width, height)) {
  setOutputRuntime(std::make_shared<HardwareBufferOutputRuntime>(mState));
}

AndroidHardwareBufferOutputNode::~AndroidHardwareBufferOutputNode() = default;

std::shared_ptr<AndroidHardwareBufferOutputNode> AndroidHardwareBufferOutputNode::create(
  ANativeWindow* const encoderWindow, const uint32_t width, const uint32_t height) {
#if !CORAL_BACKEND_HAS_VULKAN
  // The buffer is imported through the Vulkan backend, and the node presents it through Vulkan's
  // writeback: a build without that backend cannot serve this output, and the caller has to take its
  // surface output instead. Reporting it here rather than failing the first frame is what lets
  // `AssetExportSession` fall back - it only retries when the node cannot be created.
  static_cast<void>(encoderWindow);
  static_cast<void>(width);
  static_cast<void>(height);
  return nullptr;
#else
  if (encoderWindow == nullptr || width == 0 || height == 0) {
    return nullptr;
  }
  auto allocation = allocateAndroidHardwareBufferForGpu(width, height,
    AndroidHardwareBufferFormat::RGBA8, nullptr);
  if (allocation == nullptr) {
    return nullptr;
  }
  auto node = std::shared_ptr<AndroidHardwareBufferOutputNode>(
    new AndroidHardwareBufferOutputNode(encoderWindow, width, height));
  node->mState->allocation = std::move(allocation);
  return node;
#endif
}

bool AndroidHardwareBufferOutputNode::isReady() const noexcept {
  return mState->allocation != nullptr && mState->window != nullptr;
}

EffectResult AndroidHardwareBufferOutputNode::configure(const EffectOutputConfig& config) {
  if (config.format != EffectPixelFormat::RGBA8) {
    detail::NodeAccess::setDiagnostics(*this,
      "the AHardwareBuffer output accepts RGBA8 frames only");
    return EffectResult::Unsupported;
  }
  if ((config.width != 0 && config.width != mState->width) ||
      (config.height != 0 && config.height != mState->height)) {
    detail::NodeAccess::setDiagnostics(*this,
      "the AHardwareBuffer output size does not match the encoder");
    return EffectResult::InvalidArgument;
  }
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->config = config;
  return EffectResult::Success;
}

AHardwareBuffer* AndroidHardwareBufferOutputNode::hardwareBuffer() const noexcept {
  return mState->allocation != nullptr ? mState->allocation->buffer() : nullptr;
}

void AndroidHardwareBufferOutputNode::setPresentationTimeUs(const int64_t timeUs) noexcept {
  mState->presentationTimeUs.store(timeUs, std::memory_order_relaxed);
}

uint32_t AndroidHardwareBufferOutputNode::width() const noexcept {
  return mState->width;
}

uint32_t AndroidHardwareBufferOutputNode::height() const noexcept {
  return mState->height;
}

bool AndroidHardwareBufferOutputNode::isZeroCopy() const noexcept {
  return isReady();
}

bool AndroidHardwareBufferOutputNode::hasPresentFailure() const noexcept {
  return mState->presentFailed.load(std::memory_order_acquire);
}

} // namespace coral
