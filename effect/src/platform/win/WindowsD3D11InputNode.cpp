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
 * @file WindowsD3D11InputNode.cpp
 *
 * A shared D3D11 texture as an engine input. The fast path imports the shared NT handle into the engine's
 * Vulkan device; the fallback copies through a D3D11 staging texture and uploads the pixels, which is
 * what keeps the node usable on the OpenGL backend.
 */

#include "effect/EffectInternal.h"
#include "effect/EffectRenderCore.h"

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <effect/platform/win/WindowsD3D11InputNode.h>

#include <minilog/log.h>

#include <cstdint>
#include <cstring>
#include <atomic>
#include <deque>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace coral {

struct WindowsD3D11InputNode::State {
  struct QueuedTexture {
    ComPtr<ID3D11Texture2D> texture;
    uint32_t width = 0;
    uint32_t height = 0;
    EffectPixelFormat format = EffectPixelFormat::RGBA8;
    RenderTime timestamp;
    uint32_t version = 0;
    std::shared_ptr<void> lifetime;
  };

  State(ID3D11Texture2D* source, const uint32_t w, const uint32_t h, const EffectPixelFormat f,
        const uint32_t depth)
          : texture(source), width(w), height(h), format(f), capacity(depth == 0 ? 1 : depth) {
    if (texture != nullptr) texture->AddRef();
  }

  ~State() {
    if (texture != nullptr) texture->Release();
  }

  mutable std::mutex mutex;
  ID3D11Texture2D* texture = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  EffectPixelFormat format = EffectPixelFormat::RGBA8;
  uint32_t version = 1;
  bool repeat = true;
  RenderTime timestamp;
  bool hasTimestamp = false;
  std::deque<QueuedTexture> queue;
  uint32_t capacity = 4;
  EffectBackpressureMode backpressure = EffectBackpressureMode::DropOldest;
  std::atomic_bool zeroCopy { false };
};

namespace {

backend::PixelFormat toPixelFormat(const EffectPixelFormat format) noexcept {
  switch (format) {
    case EffectPixelFormat::RGBA8:
      return backend::PixelFormat::R8G8B8A8_UNORM;
    case EffectPixelFormat::BGRA8:
      return backend::PixelFormat::B8G8R8A8_UNORM;
    default:
      return backend::PixelFormat::Undefined;
  }
}

class WindowsD3D11Runtime final : public detail::InputRuntime {
public:
  explicit WindowsD3D11Runtime(std::shared_ptr<WindowsD3D11InputNode::State> state) : mState(std::move(state)) {}

  ~WindowsD3D11Runtime() override { releaseResources(); }

  bool produce(detail::FrameContext& context, detail::FrameView& view, EffectFrame& metadata,
               std::string& error) override {
    if (context.core == nullptr || context.commands == nullptr) {
      error = "the D3D11 input has no render context";
      return false;
    }
    ComPtr<ID3D11Texture2D> source;
    uint32_t width = 0;
    uint32_t height = 0;
    EffectPixelFormat format = EffectPixelFormat::RGBA8;
    uint32_t version = 0;
    RenderTime timestamp;
    bool hasTimestamp = false;
    std::shared_ptr<void> lifetime;
    {
      const std::lock_guard<std::mutex> lock(mState->mutex);
      if (!mState->queue.empty()) {
        WindowsD3D11InputNode::State::QueuedTexture queued = std::move(mState->queue.front());
        mState->queue.pop_front();
        source = std::move(queued.texture);
        width = queued.width;
        height = queued.height;
        format = queued.format;
        version = queued.version;
        timestamp = queued.timestamp;
        hasTimestamp = true;
        lifetime = std::move(queued.lifetime);
      } else {
        source = mState->texture;
        width = mState->width;
        height = mState->height;
        format = mState->format;
        version = mState->version;
        timestamp = mState->timestamp;
        hasTimestamp = mState->hasTimestamp;
      }
    }
    if (source == nullptr || width == 0 || height == 0) {
      error = "the D3D11 input has no source texture";
      return false;
    }
    mDevice = context.core->device();
    mOwner = context.core;

    if (!ensureResources(context, source.Get(), width, height, format, version, error)) return false;
    mFrameLifetime = std::move(lifetime);
    mState->zeroCopy.store(mImported.valid(), std::memory_order_relaxed);

    if (mImported.valid()) {
      view.texture = mImported;
      view.width = width;
      view.height = height;
      view.format = format;
      view.colorSpace = EffectColorSpace::SRGB;
      view.external = true;
    } else {
      if (!copyToCpu(width, height, format, error)) return false;
      backend::TextureCopyRegion region;
      region.textureOrigin = { 0, 0, 0 };
      region.size = { width, height, 1 };
      region.mipLevel = 0;
      region.baseArrayLayer = 0;
      region.arrayLayerCount = 1;
      if (!backend::isSuccess(context.commands->writeTexture(mFallback, region, mPixels.data(), mPixels.size(),
                                                             width * 4))) {
        error = "the D3D11 input pixels could not be uploaded";
        return false;
      }
      view.texture = mFallback;
      view.width = width;
      view.height = height;
      view.format = format;
      view.colorSpace = EffectColorSpace::SRGB;
      view.external = true;
    }

    const RenderTime effectiveTimestamp = hasTimestamp ? timestamp : context.timestamp;
    view.timestamp = effectiveTimestamp;
    metadata = detail::FrameAccess::make(detail::allocateFrameId(context), width, height, format, EffectColorSpace::SRGB,
                                         effectiveTimestamp);
    return true;
  }

  void setBackpressure(const EffectBackpressureMode mode) override {
    const std::lock_guard<std::mutex> lock(mState->mutex);
    mState->backpressure = mode;
  }

  void afterSubmit(const std::shared_ptr<detail::GpuSubmission>& submission) override {
    (void) submission;
    // The imported texture is cached by its D3D11 source and outlives this frame: importing the same
    // shared memory again while an earlier image is alive makes some drivers lose the device.
    mFrameLifetime.reset();
  }

  void afterAbort() override {
    mFrameLifetime.reset();
    if (mDevice != nullptr) {
      for (auto& entry: mImports) {
        if (entry.second.texture.valid()) mDevice->destroyTexture(entry.second.texture);
      }
    }
    mImports.clear();
    mImported = backend::TextureHandle::invalid();
  }

  void releaseResources() override {
    mFrameLifetime.reset();
    if (mDevice != nullptr) {
      for (auto& entry: mImports) {
        if (entry.second.texture.valid()) mDevice->destroyTexture(entry.second.texture);
      }
      if (mFallback.valid()) mDevice->destroyTexture(mFallback);
    }
    mImports.clear();
    mImported = backend::TextureHandle::invalid();
    mFallback = backend::TextureHandle::invalid();
    mFallbackWidth = 0;
    mFallbackHeight = 0;
    mSource.Reset();
    mStaging.Reset();
    mD3DDevice.Reset();
    mD3DContext.Reset();
    mDevice = nullptr;
    mOwner = nullptr;
    mPixels.clear();
    mVersion = 0;
  }

private:
  bool ensureResources(detail::FrameContext& context, ID3D11Texture2D* const texture, const uint32_t width,
                       const uint32_t height, const EffectPixelFormat format, const uint32_t version,
                       std::string& error) {
    if (version == mVersion && (mImported.valid() || mFallback.valid())) return true;

    mVersion = version;
    mWidth = width;
    mHeight = height;
    mFormat = format;
    mDevice = context.core->device();
    mOwner = context.core;

    if (mImported.valid()) {
      mImported = backend::TextureHandle::invalid();
    }
    const auto cached = mImports.find(texture);
    if (cached != mImports.end()) {
      mImported = cached->second.texture;
      return true;
    }

    HANDLE handle = nullptr;
    bool ntHandle = false;
    ComPtr<IDXGIResource1> resource1;
    if (SUCCEEDED(texture->QueryInterface(IID_PPV_ARGS(&resource1))) && resource1 != nullptr &&
        SUCCEEDED(resource1->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle)) && handle != nullptr) {
      ntHandle = true;
    } else {
      // Older producers share with the legacy handle (D3D11_RESOURCE_MISC_SHARED). It cannot be imported
      // into Vulkan, but the staging fallback can still open it; accept it rather than failing the node.
      ComPtr<IDXGIResource> resource;
      if (SUCCEEDED(texture->QueryInterface(IID_PPV_ARGS(&resource))) && resource != nullptr) {
        HANDLE legacy = nullptr;
        if (SUCCEEDED(resource->GetSharedHandle(&legacy))) handle = legacy;
      }
    }
    if (handle == nullptr) {
      error = "the D3D11 texture is not shared; create it with D3D11_RESOURCE_MISC_SHARED_NTHANDLE or "
              "D3D11_RESOURCE_MISC_SHARED";
      return false;
    }

    if (ntHandle && mDevice != nullptr &&
        mDevice->supportsNativeTextureImport(backend::NativeTextureKind::Direct3D11Texture)) {
      backend::NativeTextureDescriptor descriptor;
      descriptor.kind = backend::NativeTextureKind::Direct3D11Texture;
      descriptor.platformHandle = handle;
      descriptor.width = width;
      descriptor.height = height;
      descriptor.format = toPixelFormat(format);
      if (backend::isSuccess(mDevice->importNativeTexture(descriptor, &mImported))) {
        CloseHandle(handle);
        texture->AddRef();
        mImports.emplace(texture, CachedImport{ ComPtr<ID3D11Texture2D>(texture), mImported });
        MINILOG_INFO("the D3D11 input imported the shared texture {}x{} (zero-copy)", width, height);
        return true;
      }
      mImported = backend::TextureHandle::invalid();
      MINILOG_WARN("the D3D11 input could not import the shared texture {}x{}; using staging",
        width, height);
    }

    // The import is unavailable (OpenGL), the handle is legacy, or the driver refused the format: copy
    // through D3D11.
    if (!ensureD3D11Device(error)) {
      if (ntHandle) CloseHandle(handle);
      return false;
    }
    ComPtr<ID3D11Device1> device1;
    const HRESULT opened =
        ntHandle
            ? (SUCCEEDED(mD3DDevice.As(&device1)) && device1 != nullptr
                   ? device1->OpenSharedResource1(handle, IID_PPV_ARGS(&mSource))
                   : E_NOINTERFACE)
            : mD3DDevice->OpenSharedResource(handle, IID_PPV_ARGS(&mSource));
    if (FAILED(opened) || mSource == nullptr) {
      if (ntHandle) CloseHandle(handle);
      error = "the shared D3D11 texture could not be opened";
      return false;
    }
    if (ntHandle) CloseHandle(handle);

    D3D11_TEXTURE2D_DESC sourceDescription{};
    mSource->GetDesc(&sourceDescription);
    D3D11_TEXTURE2D_DESC staging = sourceDescription;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;
    if (FAILED(mD3DDevice->CreateTexture2D(&staging, nullptr, &mStaging)) || mStaging == nullptr) {
      error = "the D3D11 staging texture could not be created";
      return false;
    }

    const bool reuseFallback = mFallback.valid() && mFallbackWidth == width && mFallbackHeight == height &&
                               mFallbackFormat == format;
    if (!reuseFallback) {
      if (mFallback.valid()) mDevice->destroyTexture(mFallback);
      backend::TextureDescriptor descriptor = backend::TextureDescriptor::texture2D(
          width, height, toPixelFormat(format),
          static_cast<backend::TextureUsages>(backend::TextureUsage::Sampled));
      descriptor.label = "effect/win-d3d11-input";
      if (!backend::isSuccess(mDevice->createTexture(descriptor, &mFallback))) {
        error = "the engine texture for the D3D11 input could not be created";
        return false;
      }
      mFallbackWidth = width;
      mFallbackHeight = height;
      mFallbackFormat = format;
    }
    mPixels.resize(static_cast<size_t>(width) * height * 4);
    return true;
  }

  bool ensureD3D11Device(std::string& error) {
    if (mD3DDevice != nullptr) return true;
    // Do not request 11_1: on a runtime without the 11.1 interface D3D11CreateDevice rejects the whole
    // call with E_INVALIDARG, which would turn a usable 11.0 device into "no D3D11 here".
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    HRESULT created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                        levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &mD3DDevice,
                                        &level, &mD3DContext);
    if (FAILED(created)) {
      created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                  static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &mD3DDevice, &level,
                                  &mD3DContext);
    }
    if (FAILED(created) || mD3DDevice == nullptr || mD3DContext == nullptr) {
      error = "a D3D11 device for the staging fallback could not be created";
      return false;
    }
    return true;
  }

  bool copyToCpu(const uint32_t width, const uint32_t height, const EffectPixelFormat format, std::string& error) {
    if (mD3DContext == nullptr || mSource == nullptr || mStaging == nullptr) {
      error = "the D3D11 staging path is not initialised";
      return false;
    }
    mD3DContext->CopyResource(mStaging.Get(), mSource.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(mD3DContext->Map(mStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || mapped.pData == nullptr) {
      error = "the D3D11 staging texture could not be mapped";
      return false;
    }
    const uint32_t rowBytes = width * 4;
    for (uint32_t row = 0; row < height; ++row) {
      std::memcpy(mPixels.data() + static_cast<size_t>(row) * rowBytes,
                  static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch, rowBytes);
    }
    mD3DContext->Unmap(mStaging.Get(), 0);
    (void) format;
    return true;
  }

  std::shared_ptr<WindowsD3D11InputNode::State> mState;
  backend::Device* mDevice = nullptr;
  detail::RenderCore* mOwner = nullptr;
  ComPtr<ID3D11Device> mD3DDevice;
  ComPtr<ID3D11DeviceContext> mD3DContext;
  ComPtr<ID3D11Texture2D> mSource;
  ComPtr<ID3D11Texture2D> mStaging;
  struct CachedImport {
    ComPtr<ID3D11Texture2D> source;
    backend::TextureHandle texture = backend::TextureHandle::invalid();
  };

  backend::TextureHandle mImported = backend::TextureHandle::invalid();
  backend::TextureHandle mFallback = backend::TextureHandle::invalid();
  uint32_t mFallbackWidth = 0;
  uint32_t mFallbackHeight = 0;
  EffectPixelFormat mFallbackFormat = EffectPixelFormat::RGBA8;
  std::unordered_map<ID3D11Texture2D*, CachedImport> mImports;
  std::shared_ptr<void> mFrameLifetime;
  std::vector<uint8_t> mPixels;
  uint32_t mVersion = 0;
  uint32_t mWidth = 0;
  uint32_t mHeight = 0;
  EffectPixelFormat mFormat = EffectPixelFormat::RGBA8;
};

} // namespace

WindowsD3D11InputNode::WindowsD3D11InputNode(ID3D11Texture2D* const texture, const uint32_t width,
                                             const uint32_t height, const EffectPixelFormat format,
                                             const uint32_t queueDepth)
        : EffectInputNode(),
          mState(std::make_shared<State>(texture, width, height, format, queueDepth)) {
  setInputRuntime(std::make_shared<WindowsD3D11Runtime>(mState));
}

WindowsD3D11InputNode::~WindowsD3D11InputNode() = default;

std::shared_ptr<WindowsD3D11InputNode> WindowsD3D11InputNode::create(ID3D11Texture2D* const texture,
                                                                     const uint32_t width, const uint32_t height,
                                                                     const EffectPixelFormat format,
                                                                     const uint32_t queueDepth) {
  if (texture == nullptr || width == 0 || height == 0) return nullptr;
  if (format != EffectPixelFormat::RGBA8 && format != EffectPixelFormat::BGRA8) return nullptr;
  return std::shared_ptr<WindowsD3D11InputNode>(
          new WindowsD3D11InputNode(texture, width, height, format, queueDepth));
}

EffectResult WindowsD3D11InputNode::setTexture(ID3D11Texture2D* const texture, const uint32_t width,
                                               const uint32_t height, const EffectPixelFormat format) {
  if (texture == nullptr || width == 0 || height == 0) return EffectResult::InvalidArgument;
  if (format != EffectPixelFormat::RGBA8 && format != EffectPixelFormat::BGRA8) return EffectResult::Unsupported;

  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->queue.clear();
  texture->AddRef();
  if (mState->texture != nullptr) mState->texture->Release();
  mState->texture = texture;
  mState->width = width;
  mState->height = height;
  mState->format = format;
  ++mState->version;
  return EffectResult::Success;
}

EffectResult WindowsD3D11InputNode::submit(const PixelBufferView& view) {
  if (view.nativeKind != NativeHandleKind::D3D11Texture2D || view.nativeHandle == nullptr) {
    return EffectResult::Unsupported;
  }
  if (view.waitToken != nullptr && !view.waitToken->wait(2000000000ull)) {
    return EffectResult::Busy;
  }
  return submit(static_cast<ID3D11Texture2D*>(view.nativeHandle), view.width, view.height,
    view.format, view.timestamp, view.lifetime);
}

EffectResult WindowsD3D11InputNode::submit(ID3D11Texture2D* const texture, const uint32_t width,
                                           const uint32_t height, const EffectPixelFormat format,
                                           const RenderTime timestamp,
                                           std::shared_ptr<void> lifetime) {
  if (texture == nullptr || width == 0 || height == 0) return EffectResult::InvalidArgument;
  if (format != EffectPixelFormat::RGBA8 && format != EffectPixelFormat::BGRA8) return EffectResult::Unsupported;

  const std::lock_guard<std::mutex> lock(mState->mutex);
  if (mState->queue.size() >= mState->capacity) {
    if (mState->backpressure == EffectBackpressureMode::Block) return EffectResult::Busy;
    mState->queue.pop_front();
  }
  State::QueuedTexture queued;
  queued.texture = texture;
  queued.width = width;
  queued.height = height;
  queued.format = format;
  queued.timestamp = timestamp;
  queued.version = ++mState->version;
  queued.lifetime = std::move(lifetime);
  mState->width = width;
  mState->height = height;
  mState->format = format;
  mState->timestamp = timestamp;
  mState->hasTimestamp = timestamp.isValid();
  mState->queue.push_back(std::move(queued));
  return EffectResult::Success;
}

void WindowsD3D11InputNode::setTimestamp(const RenderTime timestamp) {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->timestamp = timestamp;
  mState->hasTimestamp = timestamp.isValid();
}

bool WindowsD3D11InputNode::hasFrame() const noexcept { return true; }

bool WindowsD3D11InputNode::repeatLastFrame() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->repeat;
}

void WindowsD3D11InputNode::setRepeatLastFrame(const bool repeat) {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->repeat = repeat;
}

uint32_t WindowsD3D11InputNode::width() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->width;
}

uint32_t WindowsD3D11InputNode::height() const noexcept {
  const std::lock_guard<std::mutex> lock(mState->mutex);
  return mState->height;
}

bool WindowsD3D11InputNode::isZeroCopy() const noexcept {
  return mState->zeroCopy.load(std::memory_order_relaxed);
}

} // namespace coral
