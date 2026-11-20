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
 * @file WindowsD3D11EncoderOutputNode.cpp
 *
 * A writeback output for the Windows hardware encoder. The engine renders on Vulkan (the Windows
 * default); the final frame is copied into a shared D3D11 texture through the backend's writeback
 * import. The encoder then reads that texture through WMF's D3D11 device manager, so the frame never
 * leaves the GPU.
 */

#include <effect/platform/win/WindowsD3D11EncoderOutputNode.h>

#include "effect/EffectInternal.h"
#include "effect/EffectRenderCore.h"

#include <minilog/log.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <mutex>
#include <string>

namespace coral {
namespace {

using Microsoft::WRL::ComPtr;
using detail::FrameView;

} // namespace

struct WindowsD3D11EncoderOutputNode::State {
  ComPtr<ID3D11Device> device;
  std::array<ComPtr<ID3D11Texture2D>, WindowsD3D11EncoderOutputNode::kTextureCount> textures;
  std::array<HANDLE, WindowsD3D11EncoderOutputNode::kTextureCount> sharedHandles {};
  std::array<backend::TextureHandle, WindowsD3D11EncoderOutputNode::kTextureCount> imported {};
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t writeIndex = 0;
  std::atomic<uint32_t> lastIndex { 0 };
  bool copied = false;
  mutable std::mutex mutex;
  EffectOutputConfig config;
};

namespace {

class EncoderOutputRuntime final : public detail::OutputRuntime {
public:
  explicit EncoderOutputRuntime(std::shared_ptr<WindowsD3D11EncoderOutputNode::State> state)
    : mState(std::move(state)) {
  }

  ~EncoderOutputRuntime() override = default;

  bool targetSize(const uint32_t sourceWidth, const uint32_t sourceHeight, uint32_t& width,
    uint32_t& height) const override {
    static_cast<void>(sourceWidth);
    static_cast<void>(sourceHeight);
    width = mState->width;
    height = mState->height;
    return true;
  }

  EffectPixelFormat targetFormat() const override {
    return EffectPixelFormat::BGRA8;
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
    if (!device->supportsNativeTextureImport(backend::NativeTextureKind::Direct3D11Texture)) {
      error = "the backend cannot import a Direct3D 11 texture (the Windows zero-copy encode path "
              "needs the Vulkan backend)";
      return false;
    }

    for (uint32_t index = 0; index < WindowsD3D11EncoderOutputNode::kTextureCount; ++index) {
      if (mState->imported[index].valid()) {
        continue;
      }
      if (mState->sharedHandles[index] == nullptr) {
        error = "the encoder texture has no shared handle";
        return false;
      }
      backend::NativeTextureDescriptor descriptor;
      descriptor.kind = backend::NativeTextureKind::Direct3D11Texture;
      descriptor.platformHandle = mState->sharedHandles[index];
      descriptor.width = mState->width;
      descriptor.height = mState->height;
      descriptor.format = backend::PixelFormat::B8G8R8A8_UNORM;
      descriptor.writeback = true;

      backend::TextureHandle imported = backend::TextureHandle::invalid();
      const backend::Result result = device->importNativeTexture(descriptor, &imported);
      if (!backend::isSuccess(result) || !imported.valid()) {
        error = "the backend could not import the encoder's Direct3D 11 texture";
        return false;
      }
      mState->imported[index] = imported;
    }
    return true;
  }

  bool consume(detail::FrameContext& context, const FrameView& view, std::string& error) override {
    const uint32_t slot = mState->writeIndex;
    if (!mState->imported[slot].valid()) {
      error = "the encoder texture was not imported";
      return false;
    }
    if (view.format != EffectPixelFormat::BGRA8 && view.format != EffectPixelFormat::RGBA8) {
      error = "the encoder output needs a BGRA8 or RGBA8 frame";
      return false;
    }
    if (view.width != mState->width || view.height != mState->height) {
      error = "the final frame does not match the encoder size";
      return false;
    }
    if (context.commands == nullptr) {
      error = "the frame has no command buffer";
      return false;
    }

    backend::TextureCopyRegion region;
    region.size = { mState->width, mState->height, 1 };
    const backend::Result result =
      context.commands->copyTextureToTexture(view.texture, mState->imported[slot], region);
    if (!backend::isSuccess(result)) {
      error = "the final frame could not be copied into the encoder texture";
      return false;
    }
    mState->lastIndex.store(slot, std::memory_order_relaxed);
    mState->writeIndex = (slot + 1) % WindowsD3D11EncoderOutputNode::kTextureCount;
    mState->copied = true;
    return true;
  }

private:
  std::shared_ptr<WindowsD3D11EncoderOutputNode::State> mState;
};

} // namespace

WindowsD3D11EncoderOutputNode::WindowsD3D11EncoderOutputNode(void* d3d11Device, const uint32_t width,
  const uint32_t height)
  : mState(std::make_shared<State>()) {
  mState->width = width;
  mState->height = height;

  auto* const source = static_cast<ID3D11Device*>(d3d11Device);
  if (source != nullptr) {
    source->AddRef();
    mState->device.Attach(source);
  }
  if (mState->device == nullptr || width == 0 || height == 0) {
    return;
  }

  D3D11_TEXTURE2D_DESC description{};
  description.Width = width;
  description.Height = height;
  description.MipLevels = 1;
  description.ArraySize = 1;
  description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  description.SampleDesc.Count = 1;
  description.Usage = D3D11_USAGE_DEFAULT;
  description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  // A shared NT handle needs the keyed-mutex flag on the D3D11 runtime this project targets; the
  // mutex itself is not taken because the frame fence orders Vulkan's copy before the encoder read.
  description.MiscFlags =
    D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

  for (uint32_t index = 0; index < kTextureCount; ++index) {
    const HRESULT created =
      mState->device->CreateTexture2D(&description, nullptr, &mState->textures[index]);
    if (FAILED(created) || mState->textures[index] == nullptr) {
      MINILOG_ERROR("the encoder output could not create its D3D11 texture {}: 0x{:08x}", index,
        static_cast<unsigned>(created));
      return;
    }
    ComPtr<IDXGIResource1> resource;
    const HRESULT queried = mState->textures[index].As(&resource);
    if (FAILED(queried) || resource == nullptr) {
      MINILOG_ERROR("the encoder output texture {} has no IDXGIResource1: 0x{:08x}", index,
        static_cast<unsigned>(queried));
      return;
    }
    const HRESULT shared =
      resource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &mState->sharedHandles[index]);
    if (FAILED(shared) || mState->sharedHandles[index] == nullptr) {
      MINILOG_ERROR("the encoder output texture {} could not be shared: 0x{:08x}", index,
        static_cast<unsigned>(shared));
      return;
    }

  }

  setOutputRuntime(std::make_shared<EncoderOutputRuntime>(mState));
}

WindowsD3D11EncoderOutputNode::~WindowsD3D11EncoderOutputNode() {
  for (HANDLE& handle: mState->sharedHandles) {
    if (handle != nullptr) {
      CloseHandle(handle);
      handle = nullptr;
    }
  }
  for (auto& texture: mState->textures) {
    texture.Reset();
  }
  mState->device.Reset();
}

std::shared_ptr<WindowsD3D11EncoderOutputNode> WindowsD3D11EncoderOutputNode::create(
  void* const d3d11Device, const uint32_t width, const uint32_t height) {
  if (d3d11Device == nullptr || width == 0 || height == 0) {
    return nullptr;
  }
  auto node = std::shared_ptr<WindowsD3D11EncoderOutputNode>(
    new WindowsD3D11EncoderOutputNode(d3d11Device, width, height));
  for (uint32_t index = 0; index < kTextureCount; ++index) {
    if (node->mState->textures[index] == nullptr ||
        node->mState->sharedHandles[index] == nullptr) {
      return nullptr;
    }
  }
  return node;
}

bool WindowsD3D11EncoderOutputNode::isReady() const noexcept {
  return mState->imported[0].valid() && mState->imported[1].valid() &&
         mState->imported[2].valid();
}

EffectResult WindowsD3D11EncoderOutputNode::configure(const EffectOutputConfig& config) {
  if (config.format != EffectPixelFormat::BGRA8 && config.format != EffectPixelFormat::RGBA8) {
    return EffectResult::Unsupported;
  }
  if (config.width != 0 && config.width != mState->width) {
    return EffectResult::InvalidArgument;
  }
  if (config.height != 0 && config.height != mState->height) {
    return EffectResult::InvalidArgument;
  }
  const std::lock_guard<std::mutex> lock(mState->mutex);
  mState->config = config;
  return EffectResult::Success;
}

ID3D11Texture2D* WindowsD3D11EncoderOutputNode::texture(const uint32_t index) const noexcept {
  return index < kTextureCount ? mState->textures[index].Get() : nullptr;
}

ID3D11Texture2D* WindowsD3D11EncoderOutputNode::texture() const noexcept {
  return texture(0);
}

uint32_t WindowsD3D11EncoderOutputNode::lastTextureIndex() const noexcept {
  return mState->lastIndex.load(std::memory_order_relaxed);
}

uint32_t WindowsD3D11EncoderOutputNode::width() const noexcept {
  return mState->width;
}

uint32_t WindowsD3D11EncoderOutputNode::height() const noexcept {
  return mState->height;
}

bool WindowsD3D11EncoderOutputNode::isZeroCopy() const noexcept {
  return isReady();
}

} // namespace coral








