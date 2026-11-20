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

#include <effect/EffectTexture.h>

#include <cstring>
#include <utility>

namespace coral {

namespace {

/** @return bytes one pixel of @p format occupies, or 0 when an image cannot carry it. */
uint32_t bytesPerPixel(const EffectPixelFormat format) noexcept {
  switch (format) {
    case EffectPixelFormat::RGBA8:
    case EffectPixelFormat::BGRA8:
      return 4;
    case EffectPixelFormat::R8:
      return 1;
    case EffectPixelFormat::RGBA16F:
      return 8;
    case EffectPixelFormat::YUV420:
      return 0;
  }
  return 0;
}

} // namespace anonymous

// ==================================================================================================
// EffectImageDesc / EffectImage
// ==================================================================================================

EffectImageDesc EffectImageDesc::rgba8(const void* const pixels, const uint32_t width, const uint32_t height,
                                       const uint32_t rowBytes) {
  EffectImageDesc desc;
  desc.width = width;
  desc.height = height;
  desc.format = EffectPixelFormat::RGBA8;
  desc.colorSpace = EffectColorSpace::SRGB;
  desc.alphaMode = EffectAlphaMode::Premultiplied;
  desc.rowBytes = rowBytes != 0 ? rowBytes : width * 4;
  desc.pixels = pixels;
  desc.byteSize = static_cast<size_t>(desc.rowBytes) * height;
  return desc;
}

struct EffectImage::Impl {
  uint32_t width = 0;
  uint32_t height = 0;
  EffectPixelFormat format = EffectPixelFormat::RGBA8;
  EffectColorSpace colorSpace = EffectColorSpace::SRGB;
  EffectAlphaMode alphaMode = EffectAlphaMode::Premultiplied;
  uint32_t rowBytes = 0;
  std::vector<uint8_t> pixels;
};

EffectImage::EffectImage(std::shared_ptr<const Impl> impl) noexcept : mImpl(std::move(impl)) {}

EffectImage::~EffectImage() = default;

std::shared_ptr<const EffectImage> EffectImage::create(const EffectImageDesc& desc) {
  if (desc.width == 0 || desc.height == 0 || desc.pixels == nullptr) return nullptr;
  const uint32_t pixelBytes = bytesPerPixel(desc.format);
  if (pixelBytes == 0) return nullptr;

  const uint64_t tightRow = static_cast<uint64_t>(desc.width) * pixelBytes;
  const uint64_t sourceRow = desc.rowBytes != 0 ? desc.rowBytes : tightRow;
  if (sourceRow < tightRow) return nullptr;
  if (sourceRow > UINT32_MAX) return nullptr;

  const uint64_t required = sourceRow * (desc.height - 1) + tightRow;
  if (desc.byteSize != 0 && desc.byteSize < required) return nullptr;

  auto impl = std::make_shared<Impl>();
  impl->width = desc.width;
  impl->height = desc.height;
  impl->format = desc.format;
  impl->colorSpace = desc.colorSpace;
  impl->alphaMode = desc.alphaMode;
  // The copy is tight even when the source had padding: an upload of a padded row would need the
  // backend's bytesPerRow for every row, and the padding is never useful once the image is ours.
  impl->rowBytes = static_cast<uint32_t>(tightRow);
  impl->pixels.resize(static_cast<size_t>(tightRow) * desc.height);

  const auto* source = static_cast<const uint8_t*>(desc.pixels);
  for (uint32_t row = 0; row < desc.height; ++row) {
    std::memcpy(impl->pixels.data() + static_cast<size_t>(row) * tightRow, source + static_cast<size_t>(row) * sourceRow,
                static_cast<size_t>(tightRow));
  }
  return std::shared_ptr<const EffectImage>(new EffectImage(std::move(impl)));
}

uint32_t EffectImage::width() const noexcept { return mImpl != nullptr ? mImpl->width : 0; }
uint32_t EffectImage::height() const noexcept { return mImpl != nullptr ? mImpl->height : 0; }
EffectPixelFormat EffectImage::format() const noexcept {
  return mImpl != nullptr ? mImpl->format : EffectPixelFormat::RGBA8;
}
EffectColorSpace EffectImage::colorSpace() const noexcept {
  return mImpl != nullptr ? mImpl->colorSpace : EffectColorSpace::SRGB;
}
EffectAlphaMode EffectImage::alphaMode() const noexcept {
  return mImpl != nullptr ? mImpl->alphaMode : EffectAlphaMode::Premultiplied;
}
uint32_t EffectImage::rowBytes() const noexcept { return mImpl != nullptr ? mImpl->rowBytes : 0; }
size_t EffectImage::byteSize() const noexcept { return mImpl != nullptr ? mImpl->pixels.size() : 0; }
const void* EffectImage::pixels() const noexcept {
  return mImpl != nullptr && !mImpl->pixels.empty() ? mImpl->pixels.data() : nullptr;
}

// ==================================================================================================
// EffectExternalTexture
// ==================================================================================================

EffectExternalTexture::~EffectExternalTexture() = default;

EffectExternalTextureResolver::~EffectExternalTextureResolver() = default;

// ==================================================================================================
// TextureSource
// ==================================================================================================

TextureSource TextureSource::fromImage(std::shared_ptr<const EffectImage> image) {
  TextureSource source;
  if (image == nullptr) return source;
  source.mKind = Kind::Image;
  source.mImage = std::move(image);
  return source;
}

TextureSource TextureSource::fromExternal(std::shared_ptr<EffectExternalTexture> texture) {
  TextureSource source;
  if (texture == nullptr) return source;
  source.mKind = Kind::External;
  source.mExternal = std::move(texture);
  return source;
}

TextureSource TextureSource::fromNativeTexture(const uint64_t textureToken, const uint32_t width,
  const uint32_t height, const EffectPixelFormat format) {
  TextureSource source;
  if (textureToken == 0 || width == 0 || height == 0) return source;
  source.mKind = Kind::Native;
  source.mNativeTexture = textureToken;
  source.mNativeWidth = width;
  source.mNativeHeight = height;
  source.mNativeFormat = format;
  return source;
}

TextureSource TextureSource::fromFrame(const EffectFrame& frame) {
  TextureSource source;
  if (!frame.isValid()) return source;
  source.mKind = Kind::Frame;
  source.mFrame = frame;
  return source;
}

TextureSource TextureSource::solid(const EffectColor color) {
  TextureSource source;
  source.mKind = Kind::Solid;
  source.mColor = color;
  return source;
}

TextureSource TextureSource::empty() { return TextureSource(); }

} // namespace coral
