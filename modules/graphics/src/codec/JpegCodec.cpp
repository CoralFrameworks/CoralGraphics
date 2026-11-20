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

/**
 * @file JpegCodec.cpp
 *
 * JPEG through libjpeg-turbo.
 *
 * ## The setjmp discipline
 *
 * libjpeg reports an error by calling a handler that `longjmp`s back to the caller. That is the same
 * C idiom libpng uses, and it means the jump skips every destructor between the library and the
 * landing pad. The rule here is the same one `PngCodec.cpp` follows: **no C++ object with a
 * non-trivial destructor may be live across the `setjmp`**. The vector that receives the decoded
 * pixels is therefore constructed only after the decode has succeeded, and the encode direction keeps
 * libjpeg's own `malloc`'d output buffer as a raw pointer that the failure path can free.
 *
 * ## Why `jpeg_mem_src` and `jpeg_mem_dest`
 *
 * A hand-written source manager was tried first and was wrong in a way that was not obvious: the
 * refill callback has to hand libjpeg a pointer to the *remaining* buffer and record how much it gave,
 * and getting the bookkeeping off by one chunk made libjpeg read past a valid image. `jpeg_mem_src`
 * and `jpeg_mem_dest` are libjpeg-turbo's own in-memory managers, are exercised by everything else
 * that decodes a JPEG from a buffer, and remove the callbacks from this file entirely. `WITH_MEM_SRCDST`
 * is forced on in the vendoring wrapper precisely so they exist.
 *
 * ## Colour handling
 *
 * JPEG has no alpha channel and no colour space tag worth trusting, so a decoded image is reported
 * opaque with whatever colour space the caller supplied as a fallback. Greyscale and CMYK are both
 * converted on the way out, because a consumer that has to ask "is this one channel or three" for
 * every image has not really been given an image.
 */

#include <private/graphics/ImageCodecInternal.h>

#include <csetjmp>
#include <cstdlib>
#include <cstring>

#include <jpeglib.h>

namespace graphics::codec_internal {
namespace {

/**
 * libjpeg's error manager, extended with a place to put the message.
 *
 * libjpeg requires that the *first* member be a `jpeg_error_mgr`, because it casts between the two.
 * This is the documented way to attach state, and it is what lets the message survive the jump instead
 * of being lost with the library's own state.
 *
 * ## Where the jump buffer lives
 *
 * Not here. Under the v6b ABI -- the one the vendoring wrapper pins, and the one every other libjpeg
 * consumer expects -- there is no `jump_buffer` member on the error manager; the `jmp_buf` belongs to
 * the decompress or compress struct and is reached through `jpeg_set_longjmp_fn`, which is also what
 * the `jpeg_jmpbuf`-style accessors expand to. Putting a `jmp_buf` in this struct as well, and jumping
 * to that, would work and would be wrong: libjpeg resets its own buffer at defined points, and a
 * private copy would not see those resets.
 *
 * A `std::vector` would be the natural way to own the message, but the error handler runs while a jump
 * is in flight and must not allocate. A fixed array is the honest choice, and the message is truncated
 * rather than dynamic.
 */
struct ErrorManager {
  jpeg_error_mgr base;
  /**
   * Where the error handler jumps to.
   *
   * Under the v6b ABI libjpeg does not provide this: `jpeg_error_mgr` has no `jmp_buf`, and the
   * `jpeg_set_longjmp_fn` accessor that the v7 and v8 ABIs added does not exist. The documented v6b
   * pattern is exactly this -- the caller extends the error manager with a buffer of its own, and its
   * `error_exit` handler jumps to it. The layout constraint that makes it work is that
   * `jpeg_error_mgr` must be the first member, because libjpeg casts between the two.
   */
  jmp_buf jump;
  char message[JMSG_LENGTH_MAX] = {};
};

/**
 * @return @p info as an error manager.
 *
 * `jpeg_error_mgr` is stored in the struct's `err` field, so this is a direct reference rather than a
 * search -- the cast only exists because `j_common_ptr` is the type the callback is handed.
 */
ErrorManager* managerOf(j_common_ptr info) { return reinterpret_cast<ErrorManager*>(info->err); }

void onError(j_common_ptr info) {
  auto* manager = managerOf(info);
  // `format_message` is libjpeg's own formatter and fills in the specifics -- which marker, which
  // coefficient -- that a hand-written message would lose.
  (*info->err->format_message)(info, manager->message);
  longjmp(manager->jump, 1);
}

/** Swallows warning output. A warning is not a failure. */
void onEmitMessage(j_common_ptr, int) {}

/** @return true when JPEG encoding can produce an image of this type. */
bool isEncodableColorType(const ColorType type) noexcept {
  return type == ColorType::kRGBA_8888 || type == ColorType::kBGRA_8888 || type == ColorType::kGray_8;
}

} // namespace anonymous

// ==================================================================================================
// Decode
// ==================================================================================================

std::optional<DecodedImage> decodeJpeg(const uint8_t* data, const size_t size, const DecodeOptions& options,
        std::string* outError) {
  jpeg_decompress_struct info {};
  ErrorManager manager;
  info.err = jpeg_std_error(&manager.base);
  manager.base.error_exit = onError;
  manager.base.emit_message = onEmitMessage;

  // --- The landing pad. Nothing owning may be constructed before it. ---
  if (setjmp(manager.jump) != 0) {
    jpeg_destroy_decompress(&info);
    if (outError != nullptr) {
      *outError = manager.message[0] != '\0' ? manager.message : "libjpeg reported an error with no message";
    }
    return std::nullopt;
  }

  // `info.err` is set above, so a failure here is reported through `onError` rather than by
  // dereferencing an unset error manager -- which is the trap that makes the order of these two lines
  // load-bearing.
  jpeg_create_decompress(&info);
  jpeg_mem_src(&info, data, static_cast<unsigned long>(size));
  jpeg_read_header(&info, TRUE);

  // The output colour space. libjpeg performs the conversion, which is faster and far better tested
  // than doing it by hand, and it also handles the YCbCr-to-RGB step that JPEG's storage format needs.
  // `JCS_EXT_*` variants exist in libjpeg-turbo and would let libjpeg write RGBA directly, but the
  // plain RGB path is what the v6b ABI guarantees, and the widening below is a cheap loop.
  const bool wantGrayscale = options.preferredColorType == ColorType::kGray_8;
  info.out_color_space = wantGrayscale ? JCS_GRAYSCALE : JCS_RGB;

  jpeg_start_decompress(&info);

  const uint32_t width = info.output_width;
  const uint32_t height = info.output_height;
  const int channels = info.output_components;

  if (width == 0 || height == 0) {
    jpeg_destroy_decompress(&info);
    if (outError != nullptr) *outError = "the JPEG declares a zero dimension";
    return std::nullopt;
  }

  constexpr uint32_t kMaxDimension = 65535;
  if (width > kMaxDimension || height > kMaxDimension) {
    jpeg_destroy_decompress(&info);
    if (outError != nullptr) *outError = "the JPEG is larger than the maximum supported dimension";
    return std::nullopt;
  }
  if (options.maxDimension > 0 && (width > static_cast<uint32_t>(options.maxDimension) ||
                                          height > static_cast<uint32_t>(options.maxDimension))) {
    jpeg_destroy_decompress(&info);
    if (outError != nullptr) *outError = "the JPEG exceeds the caller's dimension limit";
    return std::nullopt;
  }

  // JPEG has no alpha, so the RGB libjpeg produces is widened to RGBA with an opaque channel. The
  // caller's request for kGray_8 is honoured exactly, because greyscale is a real decoding mode here
  // rather than a conversion.
  const size_t outputChannels = wantGrayscale ? 1 : 4;
  const size_t rowBytes = outputChannels * static_cast<size_t>(width);
  const size_t byteSize = rowBytes * static_cast<size_t>(height);

  if (options.maxByteSize > 0 && byteSize > options.maxByteSize) {
    jpeg_destroy_decompress(&info);
    if (outError != nullptr) *outError = "the decoded image exceeds the caller's byte limit";
    return std::nullopt;
  }

  // A raw buffer rather than a vector, because this is allocated while the jump is still live. The
  // copy into a vector happens after `jpeg_destroy_decompress`, past the end of the risky region.
  auto* pixels = static_cast<uint8_t*>(std::malloc(byteSize));
  // One scanline of libjpeg's output, also raw for the same reason.
  auto* scanline = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(channels) * static_cast<size_t>(width)));
  if (pixels == nullptr || scanline == nullptr) {
    std::free(pixels);
    std::free(scanline);
    jpeg_destroy_decompress(&info);
    if (outError != nullptr) *outError = "the decoded image could not be allocated";
    return std::nullopt;
  }

  while (info.output_scanline < info.output_height) {
    JSAMPROW rows[1] = { scanline };
    jpeg_read_scanlines(&info, rows, 1);

    const size_t y = info.output_scanline - 1;
    uint8_t* dst = pixels + y * rowBytes;

    if (wantGrayscale) {
      std::memcpy(dst, scanline, width);
    } else {
      for (uint32_t x = 0; x < width; ++x) {
        dst[x * 4 + 0] = scanline[x * 3 + 0];
        dst[x * 4 + 1] = scanline[x * 3 + 1];
        dst[x * 4 + 2] = scanline[x * 3 + 2];
        dst[x * 4 + 3] = 255;
      }
    }
  }

  jpeg_finish_decompress(&info);
  jpeg_destroy_decompress(&info);
  std::free(scanline);

  // Past the dangerous region, so owning types are safe again.
  DecodedImage result;
  result.info = ImageInfo(static_cast<int>(width), static_cast<int>(height),
          wantGrayscale ? ColorType::kGray_8 : ColorType::kRGBA_8888, AlphaType::kOpaque)
                         .withColorSpace(options.fallbackColorSpace);
  result.pixels.assign(pixels, pixels + byteSize);
  std::free(pixels);
  return result;
}

// ==================================================================================================
// Encode
// ==================================================================================================

std::optional<EncodedImage> encodeJpeg(const DecodedImage& image, const EncodeOptions& options,
        std::string* outError) {
  const ImageInfo& imageInfo = image.info;
  if (!imageInfo.isValid() || image.pixels.size() < imageInfo.computeByteSize()) {
    if (outError != nullptr) *outError = "the image to encode has no pixels, or fewer than its info describes";
    return std::nullopt;
  }
  if (!isEncodableColorType(imageInfo.colorType())) {
    if (outError != nullptr) *outError = "JPEG encoding supports RGBA_8888, BGRA_8888 and Gray_8";
    return std::nullopt;
  }
  if (options.lossless) {
    // Baseline JPEG has no lossless mode. Refusing is the only honest answer: a caller that asked for
    // exact pixels and received a lossy file has a bug that will show up as a slowly degrading image
    // after a re-encode cycle, with nothing pointing at the cause.
    if (outError != nullptr) *outError = "JPEG has no lossless mode; use PNG or WebP for a lossless encode";
    return std::nullopt;
  }

  const bool grayscale = imageInfo.colorType() == ColorType::kGray_8;
  const bool bgra = imageInfo.colorType() == ColorType::kBGRA_8888;

  jpeg_compress_struct info {};
  ErrorManager manager;
  info.err = jpeg_std_error(&manager.base);
  manager.base.error_exit = onError;
  manager.base.emit_message = onEmitMessage;

  // libjpeg's own output buffer, allocated by `jpeg_mem_dest`. A raw pointer because the failure path
  // has to be able to release it after a jump.
  unsigned char* outBuffer = nullptr;
  unsigned long outSize = 0;

  if (setjmp(manager.jump) != 0) {
    jpeg_destroy_compress(&info);
    std::free(outBuffer);
    if (outError != nullptr) {
      *outError = manager.message[0] != '\0' ? manager.message : "libjpeg reported an error with no message";
    }
    return std::nullopt;
  }

  jpeg_create_compress(&info);
  jpeg_mem_dest(&info, &outBuffer, &outSize);

  info.image_width = static_cast<JDIMENSION>(imageInfo.width());
  info.image_height = static_cast<JDIMENSION>(imageInfo.height());
  info.input_components = grayscale ? 1 : 3;
  info.in_color_space = grayscale ? JCS_GRAYSCALE : JCS_RGB;

  jpeg_set_defaults(&info);
  // The quality is clamped rather than rejected. A caller passing 0 means "as bad as possible" and
  // libjpeg would read it as "use the default", silently ignoring what was asked for.
  const int quality = options.quality < 1 ? 1 : (options.quality > 100 ? 100 : options.quality);
  jpeg_set_quality(&info, quality, TRUE);
  jpeg_start_compress(&info, TRUE);

  // libjpeg wants a row with three channels and no alpha, so RGBA has to be condensed and BGRA
  // reordered. Done one scanline at a time into this reusable row, so the image is never copied whole.
  auto* scratch = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(imageInfo.width()) * 3));
  if (scratch == nullptr) {
    jpeg_destroy_compress(&info);
    std::free(outBuffer);
    if (outError != nullptr) *outError = "the scanline buffer could not be allocated";
    return std::nullopt;
  }

  const size_t sourceRowBytes = imageInfo.minRowBytes();
  while (info.next_scanline < info.image_height) {
    const size_t y = info.next_scanline;
    const uint8_t* sourceRow = image.pixels.data() + y * sourceRowBytes;

    JSAMPROW rows[1];
    if (grayscale) {
      rows[0] = const_cast<JSAMPROW>(sourceRow);
    } else {
      for (int x = 0; x < imageInfo.width(); ++x) {
        const uint8_t* pixel = sourceRow + static_cast<size_t>(x) * 4;
        // BGRA has blue first, RGBA has red first. Both drop their alpha.
        scratch[x * 3 + 0] = bgra ? pixel[2] : pixel[0];
        scratch[x * 3 + 1] = pixel[1];
        scratch[x * 3 + 2] = bgra ? pixel[0] : pixel[2];
      }
      rows[0] = scratch;
    }
    jpeg_write_scanlines(&info, rows, 1);
  }

  jpeg_finish_compress(&info);
  jpeg_destroy_compress(&info);
  std::free(scratch);

  EncodedImage result(outBuffer, outBuffer + outSize);
  std::free(outBuffer);
  return result;
}

} // namespace graphics::codec_internal
