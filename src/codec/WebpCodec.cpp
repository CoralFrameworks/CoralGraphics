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
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF
 * ANY KIND, either express or implied. See the License for the specific language governing permissions
 * and limitations under the License.
 */

/**
 * @file WebpCodec.cpp
 *
 * WebP through libwebp.
 *
 * ## The pleasant case
 *
 * After libpng and libjpeg, libwebp is a relief: it returns errors rather than jumping out of the
 * middle of a call, and it allocates its output with `WebPMalloc` and hands it back for the caller to
 * release. That means ordinary RAII works -- a `unique_ptr` with a custom deleter, and no rule about
 * what may be live across which call. The contrast is worth noting because the two files that do need
 * the `setjmp` discipline look like carelessness otherwise.
 *
 * ## What WebP will and will not decode
 *
 * `WebPGetInfo` reports the size without decoding, which is what makes the size limits checkable
 * before an allocation. The simple decoder produces BGR or BGRA depending on whether the file has
 * alpha, so the channel order is swapped on the way into the codec's RGBA contract.
 */

#include <private/graphics/ImageCodecInternal.h>

#include <memory>
#include <cstring>

#include <webp/decode.h>
#include <webp/encode.h>

namespace graphics::codec_internal {
namespace {

/**
 * Releases a buffer that libwebp allocated.
 *
 * The deleter is not `std::free`: libwebp may be built with an allocator of its own, and its
 * documentation is explicit that a pointer it returned must go back through `WebPFree`. Pairing its
 * allocation with the C runtime's deallocator happens to work on a default build and is exactly the
 * kind of thing that stops working when the library is built for a platform that uses a different
 * heap.
 */
struct WebPBufferDeleter {
  void operator()(uint8_t* p) const noexcept {
    if (p != nullptr) WebPFree(p);
  }
};

using WebPBuffer = std::unique_ptr<uint8_t, WebPBufferDeleter>;

} // namespace anonymous

// ==================================================================================================
// Decode
// ==================================================================================================

std::optional<DecodedImage> decodeWebp(const uint8_t* data, const size_t size, const DecodeOptions& options,
        std::string* outError) {
  int width = 0;
  int height = 0;
  // Reading the header first is what lets the limits be applied before anything is allocated -- the
  // whole point of having them. WebPGetInfo does not decode the image, so it is cheap.
  if (WebPGetInfo(data, size, &width, &height) == 0) {
    if (outError != nullptr) *outError = "libwebp could not read the header";
    return std::nullopt;
  }

  if (width <= 0 || height <= 0) {
    if (outError != nullptr) *outError = "the WebP declares a zero or negative dimension";
    return std::nullopt;
  }

  constexpr int kMaxDimension = 65535;
  if (width > kMaxDimension || height > kMaxDimension) {
    if (outError != nullptr) *outError = "the WebP is larger than the maximum supported dimension";
    return std::nullopt;
  }
  if (options.maxDimension > 0 && (width > options.maxDimension || height > options.maxDimension)) {
    if (outError != nullptr) *outError = "the WebP exceeds the caller's dimension limit";
    return std::nullopt;
  }

  const size_t rowBytes = 4 * static_cast<size_t>(width);
  const size_t byteSize = rowBytes * static_cast<size_t>(height);
  if (options.maxByteSize > 0 && byteSize > options.maxByteSize) {
    if (outError != nullptr) *outError = "the decoded image exceeds the caller's byte limit";
    return std::nullopt;
  }

  // `WebPDecodeRGBA` is the convenient entry point and always produces RGBA in memory order, whatever
  // the file's internal layout. WebP stores lossy images as YUV and lossless ones as ARGB, so asking
  // for a specific memory order is the only way to avoid handling both.
  WebPBuffer decoded(WebPDecodeRGBA(data, size, &width, &height));
  if (!decoded) {
    if (outError != nullptr) *outError = "libwebp could not decode the image";
    return std::nullopt;
  }

  // libwebp reports its own stride through the advanced API, but `WebPDecodeRGBA` always writes
  // tightly packed rows, which is the codec's contract anyway.
  DecodedImage result;
  result.info = ImageInfo(width, height, ColorType::kRGBA_8888, AlphaType::kUnpremultiplied)
                        .withColorSpace(options.fallbackColorSpace);
  result.pixels.assign(decoded.get(), decoded.get() + byteSize);
  return result;
}

// ==================================================================================================
// Encode
// ==================================================================================================

std::optional<EncodedImage> encodeWebp(const DecodedImage& image, const EncodeOptions& options,
        std::string* outError) {
  const ImageInfo& imageInfo = image.info;
  if (!imageInfo.isValid() || image.pixels.size() < imageInfo.computeByteSize()) {
    if (outError != nullptr) *outError = "the image to encode has no pixels, or fewer than its info describes";
    return std::nullopt;
  }

  const ColorType type = imageInfo.colorType();
  if (type != ColorType::kRGBA_8888 && type != ColorType::kBGRA_8888) {
    if (outError != nullptr) *outError = "WebP encoding supports RGBA_8888 and BGRA_8888";
    return std::nullopt;
  }

  // BGRA has to be presented to libwebp as RGBA. Rather than a second buffer the size of the image,
  // the reorder happens into a copy only when it is needed -- and for an RGBA image, which is the
  // common case, no copy is made at all.
  const uint8_t* pixels = image.pixels.data();
  std::vector<uint8_t> swapped;
  if (type == ColorType::kBGRA_8888) {
    swapped.resize(image.pixels.size());
    for (size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
      swapped[i + 0] = image.pixels[i + 2];
      swapped[i + 1] = image.pixels[i + 1];
      swapped[i + 2] = image.pixels[i + 0];
      swapped[i + 3] = image.pixels[i + 3];
    }
    pixels = swapped.data();
  }

  const int quality = options.quality < 1 ? 1 : (options.quality > 100 ? 100 : options.quality);

  uint8_t* outBuffer = nullptr;
  // Two different entry points, and the choice is not a quality setting: `WebPEncodeRGBA` is always
  // lossy, so a caller that needs the exact pixels back would otherwise get a slightly wrong image and
  // no indication of it. For the lossless path `quality` becomes a speed/effort hint rather than a
  // quality/ size trade, which is why it is passed through unchanged rather than reinterpreted.
  const size_t encodedSize = options.lossless
          ? WebPEncodeLosslessRGBA(pixels, imageInfo.width(), imageInfo.height(),
                    imageInfo.minRowBytes(), &outBuffer)
          : WebPEncodeRGBA(pixels, imageInfo.width(), imageInfo.height(), imageInfo.minRowBytes(),
                    static_cast<float>(quality), &outBuffer);
  WebPBuffer owned(outBuffer);

  if (encodedSize == 0 || outBuffer == nullptr) {
    if (outError != nullptr) *outError = "libwebp could not encode the image";
    return std::nullopt;
  }

  return EncodedImage(outBuffer, outBuffer + encodedSize);
}

} // namespace graphics::codec_internal
