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
 * @file PngCodec.cpp
 *
 * PNG through libpng.
 *
 * ## The awkward part, and how it is handled
 *
 * libpng reports an error by calling a function that is expected to `longjmp` out of the library and
 * back to a `setjmp` the caller set up before the call. That is a C idiom, and it is the opposite of
 * RAII: a jump out of the middle of a decode skips every destructor between here and the landing pad,
 * so a `std::vector` or a `unique_ptr` on the stack would simply leak.
 *
 * The approach here is the standard one for wrapping such a library: the `setjmp` is confined to one
 * short function per operation, and **no C++ object with a non-trivial destructor is live across it**.
 * Everything libpng needs is a raw pointer that the same function frees on both the success and the
 * failure path, and all the owning types are constructed only after the jump target has been passed.
 * The alternative -- letting a `longjmp` cross an `std::vector` -- would be undefined behaviour that
 * happens to work until the compiler decides to keep something in a register.
 */

#include <private/graphics/ImageCodecInternal.h>

#include <csetjmp>
#include <cstring>
#include <limits>
#include <vector>

#include <png.h>

namespace graphics::codec_internal {
namespace {

/**
 * Where libpng hands its error message on the way out.
 *
 * Reached through `png_get_error_ptr`, because the `setjmp` landing pad cannot take an argument. The
 * buffer belongs to the caller of the decode and outlives the jump, which is what makes it safe to
 * write to from inside the library's own error callback.
 */
struct ErrorSink {
  char message[256] = {};
};

void onError(png_structp png, png_const_charp message) {
  if (auto* sink = static_cast<ErrorSink*>(png_get_error_ptr(png))) {
    // Truncation is acceptable: this exists to say why a decode failed, and the first line of
    // libpng's message normally carries the reason while the rest is a backtrace of its internals.
    std::snprintf(sink->message, sizeof(sink->message), "%s", message != nullptr ? message : "unknown error");
  }
  longjmp(png_jmpbuf(png), 1);
}

void onWarning(png_structp, png_const_charp) {
  // A warning is not a failure. libpng emits them for legal-but-unusual files -- an unknown ancillary
  // chunk, a slightly out-of-spec gamma -- and treating those as errors would reject images that
  // every other decoder shows.
}

/**
 * Where a decode has got to in the caller's buffer.
 *
 * Passed to libpng as its opaque `io_ptr` and read back in the callback, rather than the more obvious
 * "pointer to a cursor" -- a `png_bytep*` whose pointee the callback advances. That indirection was
 * tried first and does not work: libpng reads the input in a different sequence when the `io_ptr` is
 * a pointer-to-pointer, and the decode fails partway through a valid file with "unsigned integer out
 * of range". The cause was not identified, so the form that is known to work is the one used, and a
 * named struct documents the state rather than hiding it behind two levels of pointer.
 */
struct ReadState {
  const uint8_t* data = nullptr;
  size_t offset = 0;

  /**
   * The length of @c data.
   *
   * Carried because libpng will ask for more bytes than remain whenever the file is truncated -- which it does for a
   * file cut off mid-chunk, and which an attacker can arrange deliberately. Without this the callback copied from
   * whatever followed the buffer in memory.
   */
  size_t size = 0;
};

/**
 * Reads from a caller-owned buffer, since the input is already in memory.
 *
 * ## Why the short read has to be reported
 *
 * libpng does not check that the callback gave it what it asked for: the callback either supplies @p count bytes or
 * the decode continues with whatever was in the destination. So a file that ends early -- truncated in transit, or
 * crafted so that a chunk header claims more than the file holds -- makes libpng request bytes that are not there.
 *
 * Copying anyway read past the end of the caller's buffer. That is a buffer over-read on the least trustworthy input
 * in the stack, found by running the suite under the address sanitizer, and it is why the length is carried in the
 * state now.
 *
 * `png_error` is the correct refusal rather than a flag: it calls the error handler that was installed, which records
 * the message and jumps back to the `setjmp` the decode set up. That is the same route every other libpng failure
 * takes, so a truncated file produces the ordinary "could not decode" with a reason rather than a special case.
 */
void readFromMemory(png_structp png, png_bytep out, png_size_t count) {
  auto* state = static_cast<ReadState*>(png_get_io_ptr(png));

  if (state == nullptr || state->offset > state->size || count > state->size - state->offset) {
    png_error(png, "the PNG data ends before the image does");
    return;
  }

  std::memcpy(out, state->data + state->offset, count);
  state->offset += count;
}

/** Writes into an append-only vector, for the encode direction. */
void writeToVector(png_structp png, png_bytep data, png_size_t count) {
  auto* out = static_cast<std::vector<uint8_t>*>(png_get_io_ptr(png));
  out->insert(out->end(), data, data + count);
}

void flushVector(png_structp) {
  // Nothing to do: the vector is written through directly rather than buffered by the stream.
}

} // namespace anonymous

// ==================================================================================================
// Decode
// ==================================================================================================

std::optional<DecodedImage> decodePng(const uint8_t* data, const size_t size, const DecodeOptions& options,
        std::string* outError) {
  if (size < 8 || png_sig_cmp(data, 0, 8) != 0) {
    if (outError != nullptr) *outError = "not a PNG: the signature does not match";
    return std::nullopt;
  }

  // --- Everything from here to the landing pad must be a raw pointer. See the file comment. ---
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (png == nullptr) {
    if (outError != nullptr) *outError = "libpng could not allocate a read context";
    return std::nullopt;
  }

  png_infop info = png_create_info_struct(png);
  if (info == nullptr) {
    png_destroy_read_struct(&png, nullptr, nullptr);
    if (outError != nullptr) *outError = "libpng could not allocate an info structure";
    return std::nullopt;
  }

  ErrorSink sink;
  png_set_error_fn(png, &sink, onError, onWarning);

  uint8_t* pixels = nullptr;
  png_bytep* rowPointers = nullptr;

  if (setjmp(png_jmpbuf(png)) != 0) {
    // libpng bailed out. Nothing owning is live yet, so releasing the raw buffers and the two structs
    // is the whole of the cleanup.
    delete[] pixels;
    delete[] rowPointers;
    png_destroy_read_struct(&png, &info, nullptr);
    if (outError != nullptr) {
      *outError = sink.message[0] != '\0' ? sink.message : "libpng reported an error with no message";
    }
    return std::nullopt;
  }

  // The length goes in with the pointer, so the read callback can tell a short file from a long one. See the note on
  // `readFromMemory`: libpng does not check that its callback supplied what it asked for.
  ReadState state { data, 0, size };
  png_set_read_fn(png, &state, readFromMemory);
  png_read_info(png, info);

  const png_uint_32 width = png_get_image_width(png, info);
  const png_uint_32 height = png_get_image_height(png, info);

  if (width == 0 || height == 0) {
    png_destroy_read_struct(&png, &info, nullptr);
    if (outError != nullptr) *outError = "the PNG declares a zero dimension";
    return std::nullopt;
  }

  // The size checks happen before any allocation, which is the point of having them: the dimensions
  // in a header are attacker-controlled and this is the only place that can refuse them cheaply.
  constexpr png_uint_32 kMaxDimension = 65535;
  if (width > kMaxDimension || height > kMaxDimension) {
    png_destroy_read_struct(&png, &info, nullptr);
    if (outError != nullptr) *outError = "the PNG is larger than the maximum supported dimension";
    return std::nullopt;
  }
  if (options.maxDimension > 0 &&
          (width > static_cast<png_uint_32>(options.maxDimension) ||
                  height > static_cast<png_uint_32>(options.maxDimension))) {
    png_destroy_read_struct(&png, &info, nullptr);
    if (outError != nullptr) *outError = "the PNG exceeds the caller's dimension limit";
    return std::nullopt;
  }

  // The output format. PNG is decoded to 8-bit RGBA and converted from there if the caller asked for
  // something else, because expanding inside libpng is both faster and better tested than doing it by
  // hand, and the common case is RGBA anyway.
  const bool wantGrayscale = options.preferredColorType == ColorType::kGray_8;
  const bool inputHasAlpha = (png_get_color_type(png, info) & PNG_COLOR_MASK_ALPHA) != 0 ||
                             png_get_valid(png, info, PNG_INFO_tRNS) != 0;

  // Normalising the input: palettes and sub-byte grays are expanded to 8-bit, a tRNS chunk becomes a
  // real alpha channel, and 16-bit samples are reduced. Without these the pixel layout depends on the
  // file, and every consumer would need to handle every combination.
  if (png_get_bit_depth(png, info) == 16) png_set_strip_16(png);
  if (png_get_color_type(png, info) == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
  if (png_get_color_type(png, info) == PNG_COLOR_TYPE_GRAY && png_get_bit_depth(png, info) < 8) {
    png_set_expand_gray_1_2_4_to_8(png);
  }
  if (png_get_valid(png, info, PNG_INFO_tRNS) != 0) png_set_tRNS_to_alpha(png);

  if (wantGrayscale) {
    if (inputHasAlpha) {
      // Grey plus alpha has no 8-bit single-channel representation, so the request is not honourable.
      // Reporting that is better than quietly returning RGBA under a kGray_8 label.
      png_destroy_read_struct(&png, &info, nullptr);
      if (outError != nullptr) *outError = "an 8-bit grayscale result was requested for an image with alpha";
      return std::nullopt;
    }
    // No transform is needed: the expansions above have already widened the samples to eight bits, and
    // a greyscale file is left as it is. Asking libpng to convert grey to grey is not a thing it offers,
    // and the earlier version of this file called a function that does not exist.
  } else {
    // Both of these ensure the output is RGBA in memory order regardless of the file's channel order.
    if (png_get_color_type(png, info) == PNG_COLOR_TYPE_GRAY ||
            png_get_color_type(png, info) == PNG_COLOR_TYPE_GRAY_ALPHA) {
      png_set_gray_to_rgb(png);
    }
    if (!inputHasAlpha) {
      // A caller asking for RGBA gets an opaque alpha channel rather than three channels it would
      // have to interpret, which keeps the layout promise unconditional.
      png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    }
  }

  png_set_interlace_handling(png);
  png_read_update_info(png, info);

  const size_t channels = wantGrayscale ? 1 : 4;
  const size_t rowBytes = png_get_rowbytes(png, info);
  const size_t expectedRowBytes = channels * static_cast<size_t>(width);
  if (rowBytes != expectedRowBytes) {
    // The transformations above were supposed to guarantee this. If it does not hold, the assumption
    // the rest of the function is built on is wrong, and continuing would read the wrong pixels.
    png_destroy_read_struct(&png, &info, nullptr);
    if (outError != nullptr) *outError = "libpng produced an unexpected row size after conversion";
    return std::nullopt;
  }

  const size_t byteSize = rowBytes * static_cast<size_t>(height);
  if (options.maxByteSize > 0 && byteSize > options.maxByteSize) {
    png_destroy_read_struct(&png, &info, nullptr);
    if (outError != nullptr) *outError = "the decoded image exceeds the caller's byte limit";
    return std::nullopt;
  }

  pixels = new (std::nothrow) uint8_t[byteSize];
  rowPointers = new (std::nothrow) png_bytep[height];
  if (pixels == nullptr || rowPointers == nullptr) {
    delete[] pixels;
    delete[] rowPointers;
    png_destroy_read_struct(&png, &info, nullptr);
    if (outError != nullptr) *outError = "the decoded image could not be allocated";
    return std::nullopt;
  }

  for (png_uint_32 y = 0; y < height; ++y) {
    rowPointers[y] = pixels + static_cast<size_t>(y) * rowBytes;
  }

  png_read_image(png, rowPointers);
  png_read_end(png, nullptr);
  png_destroy_read_struct(&png, &info, nullptr);
  delete[] rowPointers;

  // Past the jump target, so owning types are safe again.
  DecodedImage result;
  result.info = ImageInfo(static_cast<int>(width), static_cast<int>(height),
          wantGrayscale ? ColorType::kGray_8 : ColorType::kRGBA_8888,
          inputHasAlpha ? AlphaType::kUnpremultiplied : AlphaType::kOpaque)
                         .withColorSpace(options.fallbackColorSpace);
  result.pixels.assign(pixels, pixels + byteSize);
  delete[] pixels;
  return result;
}

// ==================================================================================================
// Encode
// ==================================================================================================

std::optional<EncodedImage> encodePng(const DecodedImage& image, const EncodeOptions& options, std::string* outError) {
  (void) options;
  const ImageInfo& info = image.info;
  if (!info.isValid() || image.pixels.size() < info.computeByteSize()) {
    if (outError != nullptr) *outError = "the image to encode has no pixels, or fewer than its info describes";
    return std::nullopt;
  }

  // PNG stores RGBA or a single gray channel. Anything else is converted before it gets here, so this
  // refuses rather than writing a file whose contents do not match its header.
  const ColorType type = info.colorType();
  const bool grayscale = type == ColorType::kGray_8;
  if (!grayscale && type != ColorType::kRGBA_8888 && type != ColorType::kBGRA_8888) {
    if (outError != nullptr) *outError = "PNG encoding supports RGBA_8888, BGRA_8888 and Gray_8";
    return std::nullopt;
  }

  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (png == nullptr) {
    if (outError != nullptr) *outError = "libpng could not allocate a write context";
    return std::nullopt;
  }
  png_infop infoPtr = png_create_info_struct(png);
  if (infoPtr == nullptr) {
    png_destroy_write_struct(&png, nullptr);
    if (outError != nullptr) *outError = "libpng could not allocate an info structure";
    return std::nullopt;
  }

  ErrorSink sink;
  png_set_error_fn(png, &sink, onError, onWarning);

  // The row pointers are the only thing needing cleanup on the failure path, and they can point
  // straight into the caller's buffer, so no copy is made and nothing owns anything across the jump.
  png_bytep* rowPointers = nullptr;

  if (setjmp(png_jmpbuf(png)) != 0) {
    delete[] rowPointers;
    png_destroy_write_struct(&png, &infoPtr);
    if (outError != nullptr) {
      *outError = sink.message[0] != '\0' ? sink.message : "libpng reported an error with no message";
    }
    return std::nullopt;
  }

  EncodedImage out;
  png_set_write_fn(png, &out, writeToVector, flushVector);
  png_set_IHDR(png, infoPtr, static_cast<png_uint_32>(info.width()), static_cast<png_uint_32>(info.height()),
          8, grayscale ? PNG_COLOR_TYPE_GRAY : PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE,
          PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

  png_write_info(png, infoPtr);

  // BGRA has to be handed to libpng as RGBA. libpng can do the swap itself through
  // `png_set_bgr` for the write direction, which avoids a second buffer the size of the image.
  if (type == ColorType::kBGRA_8888) png_set_bgr(png);

  const size_t rowBytes = info.minRowBytes();
  rowPointers = new (std::nothrow) png_bytep[static_cast<size_t>(info.height())];
  if (rowPointers == nullptr) {
    png_destroy_write_struct(&png, &infoPtr);
    if (outError != nullptr) *outError = "the row table could not be allocated";
    return std::nullopt;
  }

  // The pixels are const, but libpng's write API is not const-correct. The cast is safe because
  // `png_write_image` only reads, which is documented but not expressed in its signature.
  for (int y = 0; y < info.height(); ++y) {
    rowPointers[y] = const_cast<png_bytep>(image.pixels.data() + static_cast<size_t>(y) * rowBytes);
  }

  png_write_image(png, rowPointers);
  png_write_end(png, nullptr);
  png_destroy_write_struct(&png, &infoPtr);
  delete[] rowPointers;

  return out;
}

} // namespace graphics::codec_internal
