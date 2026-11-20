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

#include <graphics/ImageCodec.h>

#include <graphics/DecodeLimits.h>

#include <private/graphics/ImageCodecInternal.h>

#include <minilog/log.h>

#include <cmath>
#include <cstring>

namespace graphics {
namespace {

/** The eight bytes that identify each container, and how many of them are significant. */
struct Magic {
  ImageFormat format;
  uint8_t bytes[12];
  size_t length;
};

// Ordered longest-signature first so that a format whose marker begins with another's cannot be
// shadowed. WebP is the case that matters: it is RIFF-based, and a naive "RIFF" check would also match
// a WAV, so the check extends to the WEBP tag at offset 8.
constexpr Magic kMagics[] = {
  { ImageFormat::PNG, { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A }, 8 },
  { ImageFormat::WebP, { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P' }, 12 },
  { ImageFormat::JPEG, { 0xFF, 0xD8, 0xFF }, 3 },
};

/** @return the format of @p data, or `Unknown`. */
ImageFormat detect(const uint8_t* data, const size_t size) noexcept {
  if (data == nullptr) return ImageFormat::Unknown;

  for (const Magic& magic: kMagics) {
    if (size < magic.length) continue;

    if (magic.format == ImageFormat::WebP) {
      // The four size bytes in the middle of a RIFF header are skipped; only the two tags matter.
      if (std::memcmp(data, magic.bytes, 4) == 0 && std::memcmp(data + 8, magic.bytes + 8, 4) == 0) {
        return ImageFormat::WebP;
      }
      continue;
    }
    if (std::memcmp(data, magic.bytes, magic.length) == 0) return magic.format;
  }
  return ImageFormat::Unknown;
}

} // namespace anonymous

const char* toString(const ImageFormat format) noexcept {
  switch (format) {
    case ImageFormat::Unknown: return "Unknown";
    case ImageFormat::PNG: return "PNG";
    case ImageFormat::JPEG: return "JPEG";
    case ImageFormat::WebP: return "WebP";
  }
  return "Unknown";
}

ImageFormat sniffFormat(const uint8_t* data, const size_t size) noexcept { return detect(data, size); }

bool isDecodeSupported(const ImageFormat format) noexcept {
  switch (format) {
    case ImageFormat::PNG:
      // PNG is always available: libpng is a hard dependency of the codec rather than an option,
      // because it is the format every asset pipeline emits and a build without it is not useful.
      return true;
    case ImageFormat::JPEG:
    case ImageFormat::WebP:
      // Each follows the CMake option that added its library, *or* the platform decoder where there is one. Reporting
      // only the vendored macro was wrong on Android: the platform decodes JPEG, PNG and WebP through
      // `BitmapFactory`, so a caller asking "can this build read a JPEG?" was told no by a build that reads them all.
      // The question this function answers is what a caller can do, not what is linked.
#if defined(CORAL_BUILD_FOR_ANDROID)
      return true;
#elif defined(CORAL_GRAPHICS_HAS_JPEG)
      return true;
#else
      return false;
#endif
    case ImageFormat::Unknown:
      return false;
  }
  return false;
}

bool isEncodeSupported(const ImageFormat format) noexcept {
  // The same set in both directions: each of these libraries does encode as well as decode, and a
  // build that has one has both.
  return isDecodeSupported(format);
}

bool isBackendAvailable(const CodecBackend backend) noexcept {
  switch (backend) {
    case CodecBackend::Default:
      // Always available: on a host that means the vendored libraries, on Android the platform decoder.
      return true;
    case CodecBackend::Vendored:
      // Available exactly when the vendored libraries are linked. This used to be unconditionally true, which was
      // another symptom of the sources being listed without regard for what was linked: on Android a caller could ask
      // for the vendored backend, be told it was there, and get an undefined symbol.
#if defined(CORAL_GRAPHICS_HAS_LIBPNG) || defined(CORAL_GRAPHICS_HAS_JPEG) || defined(CORAL_GRAPHICS_HAS_WEBP)
      return true;
#else
      return false;
#endif
    case CodecBackend::System:
      // The platform decoder exists only where the platform provides one. Coral targets Android's
      // `BitmapFactory` for this, which is compiled in only for that platform -- see the file comment
      // in ImageCodec.h for why a device decoder is worth reaching for at all.
#if defined(CORAL_BUILD_FOR_ANDROID)
      return true;
#else
      return false;
#endif
  }
  return false;
}

std::optional<DecodedImage> decodeImage(const uint8_t* data, const size_t size,
        const DecodeOptions& requested) {
  if (data == nullptr || size == 0) {
    MINILOG_WARN("[graphics/codec] asked to decode {} bytes at {}; nothing to decode", size,
            static_cast<const void*>(data));
    return std::nullopt;
  }

  // The process-wide bounds fill in whatever the caller left at zero: a decoder is the last place to
  // notice that a header claims 30000x30000, and the caller who did not think about it gets the
  // product's limit rather than none.
  const DecodeLimits limits = decodeLimits();
  if (limits.maxBytes != 0 && size > limits.maxBytes) {
    MINILOG_WARN("[graphics/codec] refusing {} encoded bytes, over the decode limit", size);
    return std::nullopt;
  }
  DecodeOptions options = requested;
  if (options.maxByteSize == 0 && limits.maxPixels != 0) {
    // Four bytes per pixel is the worst case the decoders produce (RGBA8888).
    options.maxByteSize = limits.maxPixels * 4u;
  }
  if (options.maxDimension == 0 && limits.maxPixels != 0) {
    options.maxDimension = static_cast<int>(std::sqrt(static_cast<double>(limits.maxPixels)));
  }

  const ImageFormat format = detect(data, size);
  if (format == ImageFormat::Unknown) {
    MINILOG_WARN("[graphics/codec] the data does not match any supported image signature");
    return std::nullopt;
  }

  if (options.backend == CodecBackend::System && !isBackendAvailable(CodecBackend::System)) {
    MINILOG_WARN("[graphics/codec] the system decoder was requested for {} but is not available on this platform",
            toString(format));
    return std::nullopt;
  }

  std::string error;
  std::optional<DecodedImage> result;

  // ## The system decoder is asked first when it was named
  //
  // `CodecBackend::System` means "use the platform's decoder", and on a platform that has one the answer is whatever
  // it returns -- including a failure, which is *not* retried against the vendored libraries. Retrying would make the
  // option mean "prefer the system decoder", and a caller that chose it did so to avoid linking the other two: a
  // silent fallback would hide the fact that the platform rejected an image the caller expects it to handle.
#if defined(CORAL_BUILD_FOR_ANDROID)
  if (options.backend == CodecBackend::System) {
    result = codec_internal::decodeWithSystem(data, size, options, &error);
    if (!result) {
      MINILOG_WARN("[graphics/codec] the system decoder failed on {}: {}", toString(format), error);
    }
    return result;
  }
#endif

  // Each case is guarded by the same condition that compiles its implementation, because an unguarded call to a
  // function that is not there is an undefined symbol at link time rather than a compile error. On Android the
  // vendored sources are not compiled at all -- see the note in CMakeLists.txt -- so only the system decoder exists.
  switch (format) {
#if defined(CORAL_GRAPHICS_HAS_LIBPNG)
    case ImageFormat::PNG:
      result = codec_internal::decodePng(data, size, options, &error);
      break;
#endif
#if defined(CORAL_GRAPHICS_HAS_JPEG)
    case ImageFormat::JPEG:
      result = codec_internal::decodeJpeg(data, size, options, &error);
      break;
#endif
#if defined(CORAL_GRAPHICS_HAS_WEBP)
    case ImageFormat::WebP:
      result = codec_internal::decodeWebp(data, size, options, &error);
      break;
#endif
    default:
      // Reached when this build has no decoder for the format and the system one was not asked for. `Default` means
      // "whichever is best here", so on Android it routes to the platform rather than refusing.
#if defined(CORAL_BUILD_FOR_ANDROID)
      if (options.backend == CodecBackend::Default) {
        result = codec_internal::decodeWithSystem(data, size, options, &error);
        break;
      }
#endif
      MINILOG_WARN("[graphics/codec] this build has no {} decoder", toString(format));
      return std::nullopt;
  }

  if (!result) {
    // The library's own message is the useful part and is passed through verbatim: libpng names the
    // chunk it choked on, which is what turns "this image will not load" into something fixable.
    MINILOG_WARN("[graphics/codec] failed to decode {}: {}", toString(format), error);
  }
  return result;
}

std::optional<DecodedImage> decodeImage(const EncodedImage& data, const DecodeOptions& options) {
  return decodeImage(data.data(), data.size(), options);
}

std::optional<EncodedImage> encodeImage(const DecodedImage& image, const ImageFormat format,
        const EncodeOptions& options) {
  if (!image.isValid()) {
    MINILOG_WARN("[graphics/codec] asked to encode an image with no pixels");
    return std::nullopt;
  }

  std::string error;
  std::optional<EncodedImage> result;

  // Deliberately not silently falling back to another format. A caller that asked for JPEG and
  // received a PNG has a file with the wrong name and ten times the size, and would rather have the
  // failure.
  // Guarded like the decoders above, and for the same reason. There is no system *encoder* to fall back to: the
  // platform hook this library uses is a decoder, so a build without the vendored libraries cannot encode at all and
  // says so rather than failing to link.
  switch (format) {
#if defined(CORAL_GRAPHICS_HAS_LIBPNG)
    case ImageFormat::PNG:
      result = codec_internal::encodePng(image, options, &error);
      break;
#endif
#if defined(CORAL_GRAPHICS_HAS_JPEG)
    case ImageFormat::JPEG:
      result = codec_internal::encodeJpeg(image, options, &error);
      break;
#endif
#if defined(CORAL_GRAPHICS_HAS_WEBP)
    case ImageFormat::WebP:
      result = codec_internal::encodeWebp(image, options, &error);
      break;
#endif
    default:
      MINILOG_WARN("[graphics/codec] this build cannot encode {}; no encoder is linked", toString(format));
      return std::nullopt;
  }

  if (!result) {
    MINILOG_WARN("[graphics/codec] failed to encode {}: {}", toString(format), error);
  }
  return result;
}

} // namespace graphics
