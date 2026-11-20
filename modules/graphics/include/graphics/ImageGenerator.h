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

#ifndef CORAL_GRAPHICS_IMAGE_GENERATOR_H
#define CORAL_GRAPHICS_IMAGE_GENERATOR_H

#include <graphics/Image.h>
#include <graphics/ImageInfo.h>

#include <graphics/ImageCodec.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace graphics {

/**
 * A handle to encoded image data that can produce pixels more than once.
 *
 * ## What it is for
 *
 * The case where the same bytes are decoded repeatedly or at different moments: a thumbnail cache, a list that shows
 * a hundred images, a document that is saved and reloaded. Decoding is the expensive part of showing an image and the
 * bytes are the cheap part to keep, so the useful thing to hold is the bytes plus the knowledge of what to do with
 * them.
 *
 * `Image::decode` does the decoding and is the right call when the pixels are wanted exactly once. This is what to
 * hold when they are wanted more than once, or when the *dimensions* are wanted before deciding whether to decode at
 * all -- a list that lays out a hundred rows needs the shape of each image and none of the pixels.
 *
 * ## What it does not do, and why
 *
 * The reference's generator is *lazy*: it reads a header to learn the dimensions without decoding, and decodes only
 * when asked. That is a real advantage for a format whose header is easy to read and whose decode is not.
 *
 * This one decodes on the first request for pixels, and the first request for *dimensions* also decodes -- because
 * this library reads image dimensions with the vendored decoders, which produce them as part of a decode rather than
 * separately. Reading only a PNG's header to get its size is possible, and for a large image it is worth doing; it is
 * not done here, and stating that is better than implying a laziness that is not there.
 *
 * What it does give is the property that matters for a repeated use: **the decode happens once**. A second request
 * for pixels, for dimensions or for the `Image` is answered from what the first one produced.
 *
 * ## Thread safety
 *
 * None. The cache is filled on first use and a generator used from two threads at once can decode twice -- which is
 * wasteful rather than wrong, since both decodes produce the same pixels. A caller that shares one across threads
 * should decode once up front, or hold one per thread.
 */
class ImageGenerator {
public:
  ~ImageGenerator();

  ImageGenerator(const ImageGenerator&) = delete;
  ImageGenerator& operator=(const ImageGenerator&) = delete;
  ImageGenerator(ImageGenerator&&) noexcept;
  ImageGenerator& operator=(ImageGenerator&&) noexcept;

  /**
   * @param encoded the bytes of an encoded image, copied.
   * @param options how much to decode and what to produce.
   * @return a generator for @p encoded, or null when the bytes are not a format this build can decode.
   *
   * The format is judged by the leading bytes rather than by a name, so a caller does not have to have a name and
   * cannot be misled by the wrong one. The bytes are copied, so the caller's buffer can be released -- which is the
   * point of holding a generator rather than the data.
   */
  [[nodiscard]] static std::unique_ptr<ImageGenerator> fromEncoded(const uint8_t* encoded, size_t size,
          const DecodeOptions& options = {});

  /** @return a generator for @p encoded, copied, or null when it cannot be decoded. */
  [[nodiscard]] static std::unique_ptr<ImageGenerator> fromEncoded(EncodedImage encoded,
          const DecodeOptions& options = {});

  /**
   * @return a generator over the contents of @p path, or null when the file cannot be read or decoded.
   *
   * The whole file is read into memory, because a generator has to be able to decode more than once and a file that
   * has moved or been truncated between two decodes would give two different images from one handle.
   */
  [[nodiscard]] static std::unique_ptr<ImageGenerator> fromFile(const std::string& path,
          const DecodeOptions& options = {});

  /** @return a generator over pixels already in memory, which needs no decoding. */
  [[nodiscard]] static std::unique_ptr<ImageGenerator> fromImage(Image image);

  /**
   * @return the dimensions the image will have, or an empty optional when it cannot be determined.
   *
   * This decodes on the first call. Everything a caller learns here it learns once: the second call is a lookup.
   */
  [[nodiscard]] std::optional<ImageInfo> info() const;

  /** @return true when the bytes were recognised and could be decoded. */
  [[nodiscard]] bool isValid() const;

  /** @return the format the bytes were recognised as, or `ImageFormat::Unknown`. */
  [[nodiscard]] ImageFormat format() const noexcept { return mFormat; }

  /**
   * @return the encoded bytes exactly as they were given, or an empty buffer for a generator made from pixels.
   *
   * Handed back rather than re-encoded: the caller gave these bytes and may want them for a cache key, a checksum or
   * a store, and re-encoding would produce different bytes that happen to describe the same picture. A caller that
   * wants bytes in a particular format asks the image for them.
   */
  [[nodiscard]] const EncodedImage& encodedData() const noexcept { return mEncoded; }

  /**
   * @return the decoded image, or an empty one when there is nothing to decode.
   *
   * The first call decodes and everything after it is a copy of the result, so a caller that keeps asking pays the
   * decode once. The copy is of the `Image`, which shares its pixels -- see `Image`'s own note on that.
   */
  [[nodiscard]] Image image() const;

  /**
   * @param destination receives the pixels, in @p info's format.
   * @param info the format wanted. Its dimensions must match the image's.
   * @return true when the pixels were written.
   *
   * The reference's version of this is the point of its generator being an interface: a caller says what it wants and
   * a decoder that can produce it directly -- a system decoder that supports a scaled or a 565 output, say -- does so
   * without an intermediate buffer.
   *
   * Here it is a copy through the decoder's own conversion, which is honest about being that: the wanted format is
   * passed to the decoder as its preferred output, so the conversion happens in the decoder where it is cheapest, and
   * the result is copied out. What is *not* supported is a change of dimensions, which a decoder could do by
   * subsampling; the mismatch is refused rather than silently scaled, because a caller that asked for half the width
   * and got all of it has an allocation sized for half.
   */
  bool decodeInto(void* destination, size_t rowBytes, const ImageInfo& info) const;

  struct Impl;

private:
  ImageGenerator();

  /** Decodes if it has not been done, and reports whether there is anything to decode. */
  bool ensureDecoded() const;

  EncodedImage mEncoded;
  DecodeOptions mOptions;
  ImageFormat mFormat = ImageFormat::Unknown;

  /** True for a generator made from pixels, which has nothing to decode and is always valid. */
  bool mFromPixels = false;

  /** The cache, filled once. Mutable because every accessor is logically const. */
  mutable bool mDecoded = false;
  mutable Image mImage;
};

} // namespace graphics

#endif //CORAL_GRAPHICS_IMAGE_GENERATOR_H
