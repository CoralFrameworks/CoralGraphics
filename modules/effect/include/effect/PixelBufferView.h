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

#ifndef CORAL_EFFECT_PIXEL_BUFFER_VIEW_H
#define CORAL_EFFECT_PIXEL_BUFFER_VIEW_H

#include <effect/CompletionToken.h>
#include <effect/EffectTypes.h>

#include <cstdint>
#include <memory>

namespace coral {

/**
 * Where the pixels of a PixelBufferView live.
 *
 * The effect library deliberately does not include a platform graphics header. A platform input node
 * interprets the native handle it understands; a generic node consumes CPU planes.
 */
enum class PixelBufferStorageKind : uint8_t {
  Cpu,
  D3D11Texture,
  MetalTexture,
  OpenGLTexture,
  VulkanImage,
  AndroidHardwareBuffer,
  IOSurface,
  OHNativeBuffer,
  LinuxDmaBuf,
  NativeSurface,
};

/** The native handle carried by a PixelBufferView. */
enum class NativeHandleKind : uint8_t {
  None,
  D3D11Texture2D,
  MetalTexture,
  OpenGLTexture,
  VulkanImage,
  AndroidHardwareBuffer,
  IOSurface,
  OHNativeBuffer,
  LinuxDmaBuf,
  NativeSurface,
};

/** What a consumer can do with the view without a conversion. */
enum class PixelBufferCapability : uint32_t {
  CpuRead = 1u << 0,
  CpuWrite = 1u << 1,
  TextureSample = 1u << 2,
  TextureRender = 1u << 3,
  Transfer = 1u << 4,
};

constexpr PixelBufferCapability operator|(const PixelBufferCapability a, const PixelBufferCapability b) {
  return static_cast<PixelBufferCapability>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

constexpr bool hasCapability(const PixelBufferCapability value, const PixelBufferCapability wanted) {
  return (static_cast<uint32_t>(value) & static_cast<uint32_t>(wanted)) == static_cast<uint32_t>(wanted);
}

/** One CPU plane of a view. `data` is only read during submit. */
struct PixelBufferPlaneView {
  const uint8_t* data = nullptr;
  uint32_t rowBytes = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

/**
 * A backend-neutral frame handed to an EffectInputNode.
 *
 * This is the single effect-side input contract. A CPU frame fills `planes`; a platform input fills
 * `storage` and `nativeHandle`. Both carry the same color, rotation and presentation-time metadata, so
 * the graph never needs to know which kind it was given.
 *
 * `owner` keeps the producing buffer alive while the frame is queued. The consumer must not assume it
 * owns the frame or that the native handle stays valid after `owner` is released.
 */
struct PixelBufferView {
  /**
   * Keeps the producing buffer alive while the frame is queued. The effect library only holds the
   * opaque owner; it never interprets it.
   */
  std::shared_ptr<void> owner;

  /**
   * Optional producer token kept until the submission that sampled this frame completed. A platform
   * source uses it to know when the buffer may be recycled.
   */
  std::shared_ptr<void> lifetime;

  uint32_t width = 0;
  uint32_t height = 0;
  EffectPixelFormat format = EffectPixelFormat::RGBA8;
  EffectColorSpace colorSpace = EffectColorSpace::SRGB;
  PixelBufferStorageKind storage = PixelBufferStorageKind::Cpu;
  PixelBufferCapability capabilities = PixelBufferCapability::CpuRead;

  PixelBufferPlaneView planes[4];
  uint32_t planeCount = 0;

  NativeHandleKind nativeKind = NativeHandleKind::None;
  void* nativeHandle = nullptr;
  uint32_t nativeSubresource = 0;

  bool bt709 = false;
  bool fullRange = false;
  bool p010 = false;
  bool bt2020 = false;
  bool chromaCositedEven = true;
  bool hdr = false;
  EffectHdrTransfer hdrTransfer = EffectHdrTransfer::PQ;
  float hdr10PlusTargetedNits = 0.0f;
  float hdr10PlusAverageMaxRgb = 0.0f;
  float hdr10PlusFractionBrightPixels = 0.0f;
  uint32_t hdrMaxContentLightLevel = 0;
  uint32_t hdrMaxFrameAverageLightLevel = 0;

  EffectFrameSource source = EffectFrameSource::Planes;

  /** Source presentation time; zero is a valid time. */
  RenderTime timestamp;

  /**
   * Optional producer-side completion signal. A platform node that cannot sample a texture before
   * the producer finishes waits on this before submitting; null means the frame is already stable.
   */
  std::shared_ptr<CompletionToken> waitToken;

  [[nodiscard]] bool hasCpuPlanes() const noexcept {
    return storage == PixelBufferStorageKind::Cpu && planeCount > 0;
  }

  [[nodiscard]] bool hasNativeHandle() const noexcept {
    return nativeKind != NativeHandleKind::None && nativeHandle != nullptr;
  }
};

} // namespace coral

#endif // CORAL_EFFECT_PIXEL_BUFFER_VIEW_H
