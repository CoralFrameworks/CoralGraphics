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

#include <graphics/ImageGenerator.h>

#include <graphics/Bitmap.h>
#include <graphics/Pixmap.h>

#include <cstring>
#include <fstream>
#include <utility>

#include <minilog/log.h>

namespace graphics {

struct ImageGenerator::Impl {};

ImageGenerator::ImageGenerator() = default;

ImageGenerator::~ImageGenerator() = default;

ImageGenerator::ImageGenerator(ImageGenerator&&) noexcept = default;
ImageGenerator& ImageGenerator::operator=(ImageGenerator&&) noexcept = default;

// ==================================================================================================
// Building one
// ==================================================================================================

std::unique_ptr<ImageGenerator> ImageGenerator::fromEncoded(const uint8_t* encoded, const size_t size,
        const DecodeOptions& options) {
  if (encoded == nullptr || size == 0) return nullptr;

  EncodedImage bytes(encoded, encoded + size);
  return fromEncoded(std::move(bytes), options);
}

std::unique_ptr<ImageGenerator> ImageGenerator::fromEncoded(EncodedImage encoded, const DecodeOptions& options) {
  if (encoded.empty()) return nullptr;

  // Recognised by content rather than by a name, because the caller may have no name and cannot be relied on to have
  // the right one. A generator over bytes that are not an image at all is refused here rather than at the first
  // decode, so that a caller learns immediately instead of at the point where it has already committed to a layout.
  const ImageFormat format = sniffFormat(encoded.data(), encoded.size());
  if (format == ImageFormat::Unknown || !isDecodeSupported(format)) {
    MINILOG_WARN("[graphics/generator] the bytes are not a decodable image");
    return nullptr;
  }

  auto generator = std::unique_ptr<ImageGenerator>(new ImageGenerator());
  generator->mEncoded = std::move(encoded);
  generator->mOptions = options;
  generator->mFormat = format;
  return generator;
}

std::unique_ptr<ImageGenerator> ImageGenerator::fromFile(const std::string& path, const DecodeOptions& options) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.good()) {
    MINILOG_WARN("[graphics/generator] cannot open '{}'", path);
    return nullptr;
  }

  const auto end = file.tellg();
  if (end <= 0) {
    MINILOG_WARN("[graphics/generator] '{}' is empty", path);
    return nullptr;
  }

  EncodedImage bytes(static_cast<size_t>(end));
  file.seekg(0, std::ios::beg);
  file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (file.gcount() != static_cast<std::streamsize>(bytes.size())) {
    MINILOG_WARN("[graphics/generator] '{}' could not be read in full", path);
    return nullptr;
  }

  return fromEncoded(std::move(bytes), options);
}

std::unique_ptr<ImageGenerator> ImageGenerator::fromImage(Image image) {
  if (image.isEmpty()) return nullptr;

  auto generator = std::unique_ptr<ImageGenerator>(new ImageGenerator());
  generator->mFromPixels = true;
  generator->mDecoded = true;
  generator->mImage = std::move(image);
  // A generator over pixels has no encoded bytes, so the format is what a caller would get by encoding it -- which is
  // not known without doing the encode. Reported as unknown rather than guessed.
  generator->mFormat = ImageFormat::Unknown;
  return generator;
}

// ==================================================================================================
// Decoding, once
// ==================================================================================================

bool ImageGenerator::ensureDecoded() const {
  if (mDecoded) return !mImage.isEmpty();

  // The cache is marked as filled before the work, so a decode that fails is not attempted again on every accessor.
  // A caller that asks three times about a broken image should not pay for three failures.
  mDecoded = true;

  if (mFromPixels) return !mImage.isEmpty();
  if (mEncoded.empty()) return false;

  const auto decoded = decodeImage(mEncoded.data(), mEncoded.size(), mOptions);
  if (!decoded.has_value() || !decoded->isValid()) {
    MINILOG_WARN("[graphics/generator] the image could not be decoded");
    return false;
  }

  // Through a bitmap, because that is what an `Image` is made of. The rows the decoder produced are tightly packed,
  // so the bitmap takes them at their natural row length.
  const auto* pixels = decoded->pixels.data();
  const size_t rowBytes = decoded->info.minRowBytes();

  Bitmap bitmap = Bitmap::fromData(decoded->info, Data::makeCopy(pixels, decoded->pixels.size()), rowBytes);
  mImage = Image::fromBitmap(bitmap);
  return !mImage.isEmpty();
}

std::optional<ImageInfo> ImageGenerator::info() const {
  if (!ensureDecoded()) return std::nullopt;
  return mImage.info();
}

bool ImageGenerator::isValid() const { return ensureDecoded(); }

Image ImageGenerator::image() const {
  if (!ensureDecoded()) return {};
  // A copy of the `Image`, which shares its pixels with this generator's. A caller that draws it does not hold the
  // generator alive and does not pay for a second copy of the pixels.
  return mImage;
}

// ==================================================================================================
// Decoding into a caller's buffer
// ==================================================================================================

bool ImageGenerator::decodeInto(void* destination, const size_t rowBytes, const ImageInfo& info) const {
  if (destination == nullptr) return false;
  if (!ensureDecoded()) return false;

  const ImageInfo& source = mImage.info();

  // Dimensions must match. A decoder could honour a request for a smaller image by subsampling, and this does not --
  // which is refused rather than ignored, because a caller that asked for half the width and was given all of it has
  // allocated for half and would have its buffer written past the end.
  if (info.width() != source.width() || info.height() != source.height()) {
    MINILOG_WARN("[graphics/generator] decodeInto cannot change dimensions ({}x{} asked, {}x{} available)",
            info.width(), info.height(), source.width(), source.height());
    return false;
  }

  const size_t wanted = info.minRowBytes();
  if (rowBytes < wanted) {
    MINILOG_WARN("[graphics/generator] the destination row is {} bytes and needs {}", rowBytes, wanted);
    return false;
  }
  if (info.colorType() != source.colorType()) {
    // Reached only when the caller asked for a format the decode did not produce. The decode was given the caller's
    // preference, so this means the decoder could not honour it -- reported rather than converted, because a
    // conversion here would be a second implementation of something the codec already does.
    MINILOG_WARN("[graphics/generator] the decoded colour type is not the one asked for");
    return false;
  }

  // Copied row by row, because the destination's row length is the caller's and may exceed the image's. A single
  // `memcpy` of `wanted * height` bytes would be correct only when the two happen to agree, which is the case that
  // hides the bug rather than the case that proves the code.
  const Pixmap pixmap = mImage.makePixmap();
  if (pixmap.isEmpty()) return false;

  for (int y = 0; y < source.height(); ++y) {
    const void* from = pixmap.addr(0, y);
    if (from == nullptr) return false;
    std::memcpy(static_cast<uint8_t*>(destination) + static_cast<size_t>(y) * rowBytes, from, wanted);
  }

  return true;
}

} // namespace graphics
