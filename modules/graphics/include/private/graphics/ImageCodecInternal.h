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

#ifndef CORAL_GRAPHICS_CODEC_INTERNAL_H
#define CORAL_GRAPHICS_CODEC_INTERNAL_H

#include <graphics/ImageCodec.h>

#include <optional>
#include <string>

/**
 * @file ImageCodecInternal.h
 *
 * The seam between the codec's public entry points and the per-format implementations.
 *
 * ## Why this is in include/private
 *
 * Four translation units in `src/codec/` include it -- the dispatcher and the three format implementations
 * -- so it is not a header that belongs next to one of them. It is the contract they agree on, and a build
 * with a format switched off has to keep them agreeing: a declaration removed here has to remove the
 * implementation and the dispatch together, or a disabled format becomes a link error rather than a
 * refusal.
 *
 * Nothing here is installed and nothing outside the project can reach it. `include/private/` is on the
 * include path of the `graphics` target only, so a translation unit that tries to include this from outside
 * fails to *find* the header rather than failing to link, which is the failure a reader can act on.
 *
 * ## The error convention
 *
 * Each entry point returns `std::nullopt` and, when @p outError is non-null, fills it with a reason. The
 * reason is a `std::string` rather than something a library's C callback writes directly, because the
 * callback cannot allocate safely on a failure path while a `longjmp` is in flight -- see the note in
 * `JpegCodec.cpp`.
 */

namespace graphics::codec_internal {

/** @return the decoded image, or @c std::nullopt with @p outError explaining why not. */
std::optional<DecodedImage>
decodePng(const uint8_t* data, size_t size, const DecodeOptions& options, std::string* outError);

/** @return the encoded bytes, or @c std::nullopt with @p outError explaining why not. */
std::optional<EncodedImage>
encodePng(const DecodedImage& image, const EncodeOptions& options, std::string* outError);

std::optional<DecodedImage>
decodeJpeg(const uint8_t* data, size_t size, const DecodeOptions& options, std::string* outError);

std::optional<EncodedImage>
encodeJpeg(const DecodedImage& image, const EncodeOptions& options, std::string* outError);

std::optional<DecodedImage>
decodeWebp(const uint8_t* data, size_t size, const DecodeOptions& options, std::string* outError);

std::optional<EncodedImage>
encodeWebp(const DecodedImage& image, const EncodeOptions& options, std::string* outError);

/**
 * @return the image decoded by the platform's own decoder.
 *
 * Compiled only on Android, where `AndroidCodec.cpp` implements it against `BitmapFactory`. Declared unconditionally
 * so that the dispatch in `ImageCodec.cpp` needs no conditional of its own: on a platform without a system decoder the
 * function does not exist, and the dispatch asks `isBackendAvailable` before it calls.
 *
 * @param outError receives a description when the decode fails. The platform's own message is not available through
 *                 the JNI surface, so this says what step failed rather than quoting the decoder.
 */
std::optional<DecodedImage>
decodeWithSystem(const uint8_t* data, size_t size, const DecodeOptions& options, std::string* outError);

/**
 * Converts a buffer of Android's `ARGB_8888` words to the RGBA_8888 layout this library uses.
 *
 * Android names the format `ARGB_8888` and packs it as `0xAARRGGBB` in a `uint32_t`, which in the little-endian byte
 * order of every device it runs on puts **blue first** in memory. So the buffer is BGRA and the name is a half-truth,
 * and reading it as RGBA swaps red and blue -- which looks like a colour-space fault rather than an indexing one, and is
 * therefore worth a test rather than a careful read.
 *
 * It is a separate function, compiled on every platform, precisely so that it *can* be tested. It was inline in a JNI
 * call in `AndroidCodec.cpp`, where only a device could reach it, and the two things it gets wrong most easily -- the
 * channel order and the row stride -- are both invisible to the type-check that file is otherwise covered by.
 *
 * @param source the platform's pixels, @p stride bytes per row.
 * @param stride the row stride **in bytes**, which is not always `width * 4`: the platform pads rows, and assuming
 *               otherwise shears the image progressively down its height.
 *
 * @return the pixels de-swizzled and with the padding removed, or an empty vector when a dimension is not positive or
 *         the stride is too small to hold a row.
 */
[[nodiscard]] std::vector<uint8_t>
argb8888ToRgba(const uint8_t* source, int32_t width, int32_t height, size_t stride);

} // namespace graphics::codec_internal

#endif // CORAL_GRAPHICS_CODEC_INTERNAL_H
