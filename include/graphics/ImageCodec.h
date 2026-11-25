/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and limitations under the
 * License.
 */

#ifndef CORAL_GRAPHICS_IMAGE_CODEC_H
#define CORAL_GRAPHICS_IMAGE_CODEC_H

#include <graphics/ImageInfo.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

/**
 * @file ImageCodec.h
 *
 * Decoding and encoding image files.
 *
 * ## One interface, three libraries, and a platform implementation
 *
 * PNG, JPEG and WebP are handled by three unrelated C libraries with three unrelated calling
 * conventions. What a caller wants from all of them is the same thing -- "here are some bytes, give me
 * pixels" -- so that is the interface, and the libraries live behind it.
 *
 * On Android the decoding path deliberately does not go through those libraries at all. A `.so` that
 * contains libpng, libjpeg-turbo and libwebp is several megabytes larger than one that does not, and
 * every Android device already ships a system decoder that is maintained, hardware-accelerated on
 * some devices, and free. `CodecBackend::System` selects it; on other platforms it is not available
 * and the vendored libraries are used. The caller does not have to know which happened, but can ask.
 *
 * ## Why the buffers are byte vectors rather than a Pixmap
 *
 * A decoder's natural output is "a size, a format and some bytes". Turning that into a `Pixmap`
 * involves deciding on a row stride and an owning allocation, which is a separate concern and belongs
 * to the caller that wants one. Handing back bytes keeps the codec usable from a load path that is
 * about to upload straight to a texture without ever materialising a bitmap.
 */

namespace graphics {

/** A container format. */
enum class ImageFormat : uint8_t {
  /** Could not be determined from the data. */
  Unknown,
  PNG,
  JPEG,
  WebP,
};

/** Human readable name of @p format, never null. */
[[nodiscard]] const char* toString(ImageFormat format) noexcept;

/**
 * Which implementation performs the work.
 *
 * Exposed because the choice is visible in behaviour, not only in binary size: a system decoder can
 * refuse a format, produce a different colour space tag, or apply the device's own colour management.
 * A caller debugging a colour difference needs to be able to ask.
 */
enum class CodecBackend : uint8_t {
  /** Whatever this build considers best for the platform. */
  Default,
  /** The vendored libraries. Always available. */
  Vendored,
  /** The platform's own decoder. Android only; other platforms report unavailability. */
  System,
};

/** Plain byte buffers, in the two directions. */
using EncodedImage = std::vector<uint8_t>;

/** A decoded image: tightly packed rows, no padding. */
struct DecodedImage {
  ImageInfo info;
  /** `info.computeByteSize()` bytes, or empty when nothing was decoded. */
  std::vector<uint8_t> pixels;

  [[nodiscard]] bool isValid() const noexcept { return info.isValid() && !pixels.empty(); }
};

/**
 * How much to try to decode.
 *
 * A decoder is handed data from a file or a network response, which is the least trustworthy input in
 * a graphics stack. The size fields in an image header are attacker-controlled, so a caller has to be
 * able to say "no larger than this" before the allocation happens rather than after.
 */
struct DecodeOptions {
  /** The colour type to produce. `kUnknown` means "whatever the file is", which only a system
   *  decoder and a PNG with 8-bit channels can honour exactly. */
  ColorType preferredColorType = ColorType::kRGBA_8888;

  /** Attach this colour space to the result when the file does not carry one of its own. */
  std::shared_ptr<ColorSpace> fallbackColorSpace;

  /** Refuse an image larger than this in either dimension. 0 means no limit. */
  int maxDimension = 0;

  /**
   * Refuse an image whose decoded size would exceed this many bytes. 0 means no limit.
   *
   * The dimension limit alone is not enough: a 30000x30000 PNG is within any sane per-axis bound and
   * is still three gigabytes of pixels. This is the limit that actually protects an allocation.
   */
  size_t maxByteSize = 0;

  CodecBackend backend = CodecBackend::Default;
};

/** Options for encoding. */
struct EncodeOptions {
  /** Quality for a lossy format, 0-100. Ignored by a lossless encode and by PNG. */
  int quality = 90;

  /**
   * Ask for a lossless encode.
   *
   * Explicit rather than implied by a quality of 100, because the two are not the same thing and the
   * difference is not detectable from the result. WebP has separate entry points for the lossless and
   * lossy encoders, and `WebPEncodeRGBA` is *always* lossy -- passing it a quality of 100 makes the
   * output larger and better, not exact. A caller that needs the pixels back unchanged has to say so.
   *
   * PNG is lossless by nature and ignores this. JPEG has no lossless mode in its baseline form, so a
   * request for one is refused rather than silently downgraded -- see `encodeImage`.
   */
  bool lossless = false;

  CodecBackend backend = CodecBackend::Default;
};

/**
 * @return the format of @p data, judged by its leading bytes rather than by a file name.
 *
 * A file name is a hint from whoever wrote the file; the magic number is what the file actually is,
 * and a decoder that trusts the name is a decoder that can be told to parse a PNG as JPEG.
 */
[[nodiscard]] ImageFormat sniffFormat(const uint8_t* data, size_t size) noexcept;

/** @return true when this build can decode @p format. */
[[nodiscard]] bool isDecodeSupported(ImageFormat format) noexcept;

/** @return true when this build can encode @p format. */
[[nodiscard]] bool isEncodeSupported(ImageFormat format) noexcept;

/**
 * @return true when @p backend is usable on this platform.
 *
 * `CodecBackend::System` is available only where the platform provides a decoder, which today means
 * Android. Asking is cheaper than discovering it from a failed decode.
 */
[[nodiscard]] bool isBackendAvailable(CodecBackend backend) noexcept;

/**
 * @return the decoded image, or `std::nullopt` when the data is not a supported image.
 *
 * Fails rather than throwing, and for a reason that matters at this layer: image data comes from
 * files, network responses and user content, so "this is not a valid PNG" is an ordinary outcome on a
 * path that must not unwind through a renderer. Diagnostics go through `utils`' logging like the rest
 * of the graphics layer, and include the decoder's own message where the library provides one.
 */
[[nodiscard]] std::optional<DecodedImage> decodeImage(const uint8_t* data, size_t size,
        const DecodeOptions& options = {});

/** Convenience overload for an encoded buffer. */
[[nodiscard]] std::optional<DecodedImage> decodeImage(const EncodedImage& data, const DecodeOptions& options = {});

/**
 * @return @p image encoded as @p format, or `std::nullopt` when it cannot be encoded.
 *
 * PNG is lossless and ignores @c quality; JPEG and WebP are lossy and honour it.
 */
[[nodiscard]] std::optional<EncodedImage> encodeImage(const DecodedImage& image, ImageFormat format,
        const EncodeOptions& options = {});

} // namespace graphics

#endif // CORAL_GRAPHICS_IMAGE_CODEC_H
