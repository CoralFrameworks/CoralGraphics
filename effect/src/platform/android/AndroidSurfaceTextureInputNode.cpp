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
 * @file AndroidSurfaceTextureInputNode.cpp
 *
 * A camera/decoder `SurfaceTexture` as an engine input.
 *
 * The Java object is driven from the render thread - `updateTexImage`, `getTransformMatrix`,
 * `getTimestamp` - because that is where the shared EGL context is current. The external OES texture is
 * converted to an ordinary RGBA8 pooled frame by one full-screen pass; the SurfaceTexture transform is
 * applied to the quad's texture coordinates on the CPU, so the conversion program needs no uniform matrix
 * and every downstream pass sees a normal `sampler2D`.
 */

#include <effect/platform/android/AndroidSurfaceTextureInputNode.h>

#include "effect/EffectInternal.h"
#include "effect/EffectRenderCore.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace coral {
struct AndroidSurfaceTextureInputNode::State {
  State(JNIEnv* env, jobject texture, const uint32_t name, const uint32_t w, const uint32_t h)
          : textureName(name),
            width(w),
            height(h) {
    if (env != nullptr) env->GetJavaVM(&vm);
    if (env != nullptr && texture != nullptr) {
      surfaceTexture = env->NewGlobalRef(texture);
      if (surfaceTexture != nullptr) {
        jclass klass = env->GetObjectClass(surfaceTexture);
        if (klass != nullptr) {
          updateTexImage = env->GetMethodID(klass, "updateTexImage", "()V");
          getTransformMatrix = env->GetMethodID(klass, "getTransformMatrix", "([F)V");
          getTimestamp = env->GetMethodID(klass, "getTimestamp", "()J");
          env->DeleteLocalRef(klass);
        }
      }
    }
  }

  ~State() {
    if (vm == nullptr || surfaceTexture == nullptr) return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
      if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true;
    }
    if (env != nullptr) env->DeleteGlobalRef(surfaceTexture);
    if (attached) vm->DetachCurrentThread();
  }

  JavaVM* vm = nullptr;
  jobject surfaceTexture = nullptr;
  jmethodID updateTexImage = nullptr;
  jmethodID getTransformMatrix = nullptr;
  jmethodID getTimestamp = nullptr;
  uint32_t textureName = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  std::atomic<bool> repeat { true };
  std::atomic<bool> frameAvailable { false };
  /** True once the application has wired `SurfaceTexture.OnFrameAvailableListener` to notifyFrameAvailable. */
  std::atomic<bool> notificationsEnabled { false };
  std::atomic<bool> producedAny { false };
  std::atomic<bool> externalUpdate { false };
};


namespace {

using detail::FrameView;
using detail::ProgramSnapshot;
using detail::RenderProgram;

/** `GL_TEXTURE_EXTERNAL_OES`, spelled out because the public header does not include GLES. */
constexpr uint32_t kTextureExternalOes = 0x8D65;

struct ConvertVertex {
  float x, y, u, v;
};

constexpr uint32_t kConvertVertexCount = 6;

/** Applies the SurfaceTexture transform matrix to one texture coordinate. */
void transformUv(const float matrix[16], const float u, const float v, float& outU, float& outV) {
  outU = matrix[0] * u + matrix[4] * v + matrix[12];
  outV = matrix[1] * u + matrix[5] * v + matrix[13];
}

class AndroidSurfaceTextureRuntime final : public detail::InputRuntime {
public:
  explicit AndroidSurfaceTextureRuntime(std::shared_ptr<AndroidSurfaceTextureInputNode::State> state)
          : mState(std::move(state)) {}

  ~AndroidSurfaceTextureRuntime() override { releaseResources(); }

  bool produce(detail::FrameContext& context, FrameView& view, EffectFrame& metadata, std::string& error) override {
    if (context.core == nullptr || context.commands == nullptr || context.pool == nullptr) {
      error = "the SurfaceTexture input has no render context";
      return false;
    }
    backend::Device* const device = context.core->device();
    if (device == nullptr) {
      error = "the engine has no device";
      return false;
    }
    mDevice = device;

    // If the application opted into frame notifications, updateTexImage is only meaningful after one. An
    // empty error means "no new frame": the engine repeats the last picture or counts a drop. Before the
    // first frame, or when notifications were never wired, take the historical always-update path.
    if (mState->notificationsEnabled.load(std::memory_order_acquire) &&
        !mState->frameAvailable.exchange(false, std::memory_order_acq_rel) &&
        mState->producedAny.load(std::memory_order_acquire)) {
      return false;
    }

    float transform[16] = {};
    int64_t timestampNanoseconds = 0;
    if (!updateTexture(transform, timestampNanoseconds, error)) return false;
    if (timestampNanoseconds == 0 && mState->externalUpdate.load(std::memory_order_acquire)) {
      // The application already called updateTexImage on its own context; a producer that has stopped
      // (or a last frame after EOS) still has a valid picture. Give it a monotonic engine timestamp so
      // the sample is not rejected as "no frame".
      timestampNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    if (timestampNanoseconds == 0) {
      // The producer has not delivered a frame yet. An empty error means "no new frame" rather than a
      // failure, so the engine repeats the previous one or counts a dropped frame.
      return false;
    }

    if (!ensureProgram(context, error)) return false;
    if (!ensureExternalTexture(device, error)) return false;
    if (!ensureVertices(device, error)) return false;

    detail::FrameKey key;
    key.width = mState->width;
    key.height = mState->height;
    key.format = backend::PixelFormat::R8G8B8A8_UNORM;
    key.sampleCount = 1;

    detail::FrameLease lease = context.pool->acquire(key, error);
    if (!lease.isValid()) return false;

    ConvertVertex vertices[kConvertVertexCount];
    buildVertices(transform, vertices);
    if (!backend::isSuccess(context.commands->writeBuffer(mVertices, 0, vertices, sizeof(vertices)))) {
      error = "the SurfaceTexture conversion quad could not be uploaded";
      return false;
    }

    bool pipelineCreated = false;    const backend::RenderPipelineHandle pipeline =
        mProgram->pipelineFor(key.format, lease.get()->pass, &pipelineCreated);
    if (!pipeline.valid()) {
      error = "the SurfaceTexture conversion has no pipeline for RGBA8";
      return false;
    }
    const backend::DescriptorSetHandle set = mProgram->setFor(
        { mExternalTexture }, context.core->parameterTexture(), context.core->linearSampler(), {}, nullptr);
    if (!set.valid()) {
      error = "the SurfaceTexture conversion could not bind its source texture";
      return false;
    }

    backend::RenderPassBeginInfo begin;
    begin.renderPass = lease.get()->pass;
    begin.colorAttachmentCount = 1;
    begin.colorAttachments[0].view = lease.get()->view;
    begin.colorAttachments[0].loadOp = backend::LoadOp::Clear;
    begin.colorAttachments[0].storeOp = backend::StoreOp::Store;
    begin.colorAttachments[0].clearColor = backend::Color::transparentBlack();
    begin.renderArea = { key.width, key.height };

    const bool recorded =
        backend::isSuccess(context.commands->beginRenderPass(begin)) &&
        backend::isSuccess(context.commands->bindRenderPipeline(pipeline)) &&
        backend::isSuccess(context.commands->bindDescriptorSet(0, set)) &&
        backend::isSuccess(context.commands->setVertexBuffer(0, mVertices, 0)) &&
        backend::isSuccess(context.commands->draw(kConvertVertexCount, 1, 0, 0)) &&
        backend::isSuccess(context.commands->endRenderPass());
    if (!recorded) {
      error = "the SurfaceTexture conversion pass could not be recorded";
      return false;
    }

    const int64_t ticks = static_cast<int64_t>((static_cast<long double>(timestampNanoseconds) *
                                                static_cast<long double>(context.core->timeBase())) /
                                               1'000'000'000.0L);
    metadata = detail::FrameAccess::make(detail::allocateFrameId(context), mState->width, mState->height, EffectPixelFormat::RGBA8,
                                         EffectColorSpace::SRGB, RenderTime::fromTicks(ticks, context.core->timeBase()));
    view = lease.view();
    view.timestamp = metadata.timestamp();
    mLease = std::move(lease);
    mState->producedAny.store(true, std::memory_order_release);
    return true;
  }

  void afterSubmit(const std::shared_ptr<detail::GpuSubmission>& submission) override {
    if (mLease.isValid()) {
      mLease.markSubmitted(submission);
      mLease.reset();
    }
  }

  void afterAbort() override { mLease.reset(); }

  void releaseResources() override {
    mLease.reset();
    if (mDevice != nullptr) {
      if (mVertices.valid()) mDevice->destroyBuffer(mVertices);
      if (mExternalTexture.valid()) mDevice->destroyTexture(mExternalTexture);
    }
    mVertices = backend::BufferHandle::invalid();
    mExternalTexture = backend::TextureHandle::invalid();
    mProgram = nullptr;
    mSnapshot.reset();
    mDevice = nullptr;
  }

private:
  bool updateTexture(float transform[16], int64_t& timestampNanoseconds, std::string& error) {
    JNIEnv* env = nullptr;
    bool attached = false;
    if (mState->vm == nullptr) {
      error = "the SurfaceTexture input has no JavaVM";
      return false;
    }
    const jint state = mState->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (state == JNI_EDETACHED) {
      if (mState->vm->AttachCurrentThread(&env, nullptr) != JNI_OK || env == nullptr) {
        error = "the render thread could not attach to the JVM";
        return false;
      }
      attached = true;
    } else if (state != JNI_OK) {
      error = "the render thread has no usable JNIEnv";
      return false;
    }

    const auto detach = [this, attached] {
      if (attached) mState->vm->DetachCurrentThread();
    };

    if (!mState->externalUpdate) {
      env->CallVoidMethod(mState->surfaceTexture, mState->updateTexImage);
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
        detach();
        error = "SurfaceTexture.updateTexImage threw (was the SurfaceTexture released?)";
        return false;
      }
    }

    if (mState->getTransformMatrix != nullptr) {
      jfloatArray array = env->NewFloatArray(16);
      if (array != nullptr) {
        env->CallVoidMethod(mState->surfaceTexture, mState->getTransformMatrix, array);
        if (env->ExceptionCheck()) {
          env->ExceptionClear();
        } else {
          env->GetFloatArrayRegion(array, 0, 16, transform);
        }
        env->DeleteLocalRef(array);
      }
    }
    timestampNanoseconds = static_cast<int64_t>(env->CallLongMethod(mState->surfaceTexture, mState->getTimestamp));
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      timestampNanoseconds = 0;
    }

    detach();
    return true;
  }

  bool ensureProgram(detail::FrameContext& context, std::string& error) {
    if (mProgram != nullptr) return true;
    if (mSnapshot == nullptr) {
      auto snapshot = std::make_shared<ProgramSnapshot>();
      snapshot->desc.name = "android/surface-texture-copy";
      snapshot->desc.fragmentSource = "  return texture(uInput0, uv);";
      snapshot->desc.inputCount = 1;
      snapshot->externalInputMask = 1;
      snapshot->hash = detail::hashProgram(snapshot->desc, snapshot->parameters, snapshot->textureNames, 1);
      mSnapshot = std::move(snapshot);
    }
    const std::shared_ptr<const ProgramSnapshot> stored = mSnapshot;
    std::string diagnostic;
    mProgram = context.core->programFor(stored, diagnostic);
    if (mProgram == nullptr) {
      error = "the SurfaceTexture conversion program did not compile: " + diagnostic;
      return false;
    }
    return true;
  }

  bool ensureExternalTexture(backend::Device* device, std::string& error) {
    if (mExternalTexture.valid()) {
      return true;
    }
    backend::NativeTextureDescriptor descriptor;
    descriptor.kind = backend::NativeTextureKind::OpenGlTexture;
    descriptor.glTarget = kTextureExternalOes;
    descriptor.glTexture = mState->textureName;
    descriptor.width = mState->width;
    descriptor.height = mState->height;
    descriptor.format = backend::PixelFormat::R8G8B8A8_UNORM;
    if (!backend::isSuccess(device->importNativeTexture(descriptor, &mExternalTexture))) {
      error = "the SurfaceTexture's OES texture could not be imported";
      return false;
    }
    return true;
  }

  bool ensureVertices(backend::Device* device, std::string& error) {
    if (mVertices.valid()) return true;
    backend::BufferDescriptor descriptor;
    descriptor.size = sizeof(ConvertVertex) * kConvertVertexCount;
    descriptor.usage = static_cast<backend::BufferUsages>(backend::BufferUsage::Vertex);
    descriptor.memoryUsage = backend::MemoryUsage::CpuToGpuDynamic;
    descriptor.label = "effect/android-surface-texture-vertices";
    if (!backend::isSuccess(device->createBuffer(descriptor, &mVertices))) {
      error = "the SurfaceTexture conversion vertex buffer could not be created";
      return false;
    }
    return true;
  }

  void buildVertices(const float matrix[16], ConvertVertex out[kConvertVertexCount]) const {
    const float corners[kConvertVertexCount][2] = {
      { -1.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, -1.0f },
      { -1.0f, 1.0f }, { 1.0f, -1.0f }, { -1.0f, -1.0f },
    };
    const float uvs[kConvertVertexCount][2] = {
      { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f },
      { 0.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f },
    };
    for (uint32_t index = 0; index < kConvertVertexCount; ++index) {
      out[index].x = corners[index][0];
      out[index].y = corners[index][1];
      transformUv(matrix, uvs[index][0], uvs[index][1], out[index].u, out[index].v);
    }
  }

  std::shared_ptr<AndroidSurfaceTextureInputNode::State> mState;
  backend::Device* mDevice = nullptr;
  backend::BufferHandle mVertices = backend::BufferHandle::invalid();
  backend::TextureHandle mExternalTexture = backend::TextureHandle::invalid();
  std::shared_ptr<const ProgramSnapshot> mSnapshot;
  RenderProgram* mProgram = nullptr;
  detail::FrameLease mLease;
};

} // namespace



AndroidSurfaceTextureInputNode::AndroidSurfaceTextureInputNode(JNIEnv* env, jobject surfaceTexture,
                                                               const uint32_t textureName, const uint32_t width,
                                                               const uint32_t height)
        : EffectInputNode(),
          mState(std::make_shared<State>(env, surfaceTexture, textureName, width, height)) {
  setInputRuntime(std::make_shared<AndroidSurfaceTextureRuntime>(mState));
}

AndroidSurfaceTextureInputNode::~AndroidSurfaceTextureInputNode() = default;

std::shared_ptr<AndroidSurfaceTextureInputNode> AndroidSurfaceTextureInputNode::create(
        JNIEnv* env, jobject surfaceTexture, const uint32_t textureName, const uint32_t width, const uint32_t height) {
  if (env == nullptr || surfaceTexture == nullptr || textureName == 0 || width == 0 || height == 0) return nullptr;
  return std::shared_ptr<AndroidSurfaceTextureInputNode>(
          new AndroidSurfaceTextureInputNode(env, surfaceTexture, textureName, width, height));
}

bool AndroidSurfaceTextureInputNode::hasFrame() const noexcept {
  if (!mState->notificationsEnabled.load(std::memory_order_acquire)) return true;
  return mState->frameAvailable.load(std::memory_order_acquire) ||
         !mState->producedAny.load(std::memory_order_acquire);
}

bool AndroidSurfaceTextureInputNode::repeatLastFrame() const noexcept {
  return mState->repeat.load(std::memory_order_relaxed);
}

void AndroidSurfaceTextureInputNode::setRepeatLastFrame(const bool repeat) {
  mState->repeat.store(repeat, std::memory_order_relaxed);
}

uint32_t AndroidSurfaceTextureInputNode::width() const noexcept { return mState->width; }

uint32_t AndroidSurfaceTextureInputNode::height() const noexcept { return mState->height; }

void AndroidSurfaceTextureInputNode::notifyFrameAvailable() noexcept {
  mState->notificationsEnabled.store(true, std::memory_order_release);
  mState->frameAvailable.store(true, std::memory_order_release);
}

void AndroidSurfaceTextureInputNode::setUpdateTextureFromApp(const bool enabled) noexcept {
  mState->externalUpdate.store(enabled, std::memory_order_release);
}

} // namespace coral
