/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and limitations under the
 * License.
 */

#include <graphics/Image.h>

#include <graphics/ImageCodec.h>

#include <utility>

namespace graphics {

Image Image::fromBitmap(const Bitmap& bitmap) {
  if (bitmap.isEmpty()) return {};

  // A deep copy, so the image is a picture rather than a window onto something the caller may redraw. This is the
  // only place the copy happens, which is what makes it worth paying for.
  Bitmap owned = bitmap.makeDeepCopy();
  if (!owned.isWritable()) return Image(std::move(owned));

  // Then stored **read-only**, which is what makes the class's promise true rather than merely stated: `Bitmap` copies
  // share their pixels, so a writable bitmap behind an image could be copied by a caller, written through `lockPixels`,
  // and the picture the image claims is fixed would change. `Bitmap::fromData` over the copy's own bytes is non-writable
  // and copies nothing, so the flag is dropped and nothing else is paid.
  Bitmap readOnly = Bitmap::fromData(owned.info(), owned.data(), owned.rowBytes());
  if (readOnly.isEmpty()) return Image(std::move(owned));
  return Image(std::move(readOnly));
}

Image Image::fromData(const ImageInfo& info, Data data, const size_t rowBytes) {
  if (!info.isValid() || data.isEmpty()) return {};

  // Routed through `Bitmap::fromData`, which is where the length and stride checks live. Repeating them here would
  // give two places that have to agree about what a buffer must hold.
  Bitmap bitmap = Bitmap::fromData(info, data, rowBytes);
  if (bitmap.isEmpty()) return {};

  // The bytes are borrowed rather than copied, which is the point of this entry point: a caller wrapping a
  // decoder's output wants the pixels it already has, not a second copy. The `Data` carries its own owner, so the
  // pixels stay alive for as long as the image does.
  return Image(std::move(bitmap));
}

Image Image::decode(const Data& encoded, std::string* outError) {
  if (encoded.isEmpty()) {
    if (outError != nullptr) *outError = "there is nothing to decode";
    return {};
  }

  // The pointer-and-length overload, so the compressed bytes are not copied into a vector first. The codec reads
  // them once and the `Data` already owns them for as long as the call takes.
  const std::optional<DecodedImage> decoded = decodeImage(encoded.bytes(), encoded.size());
  if (!decoded.has_value() || !decoded->isValid()) {
    // The codec logs the decoder's own message, which is the useful half of a failure -- "not a PNG" tells a
    // caller less than "bad adaptive filter value" does. What is added here is the context that the failure
    // happened while decoding an image rather than while encoding one.
    if (outError != nullptr) *outError = "the image codec could not decode these bytes";
    return {};
  }

  // The codec's pixels are tightly packed, which is exactly the stride an `ImageInfo` reports, so the buffer can
  // be handed straight over. Taking a copy here is unavoidable -- the codec's vector owns the bytes and the image
  // needs to outlive it -- and it is the last copy in the chain.
  Data pixels = Data::makeCopy(decoded->pixels.data(), decoded->pixels.size());
  return Image::fromData(decoded->info, std::move(pixels));
}

Data Image::encode(const ImageFormat format, const int quality, std::string* outError) const {
  if (isEmpty()) {
    if (outError != nullptr) *outError = "there are no pixels to encode";
    return {};
  }

  // The codec works from a `DecodedImage`, which is the same shape as what an `Image` holds: an `ImageInfo` and a
  // byte buffer. This copies the pixel vector, which is the one place this path is not free.
  DecodedImage source;
  source.info = mBitmap.info();
  source.pixels.assign(mBitmap.data().bytes(), mBitmap.data().bytes() + mBitmap.data().size());

  EncodeOptions options;
  options.quality = quality;

  const std::optional<EncodedImage> encoded = encodeImage(source, format, options);
  if (!encoded.has_value() || encoded->empty()) {
    if (outError != nullptr) *outError = "the image codec could not encode with this format";
    return {};
  }

  return Data::makeCopy(encoded->data(), encoded->size());
}

Rect Image::bounds() const noexcept {
  return Rect::MakeLTRB(0.0f, 0.0f, static_cast<Scalar>(width()), static_cast<Scalar>(height()));
}

bool Image::equals(const Image& other) const noexcept {
  // The format first, because it is cheap and a mismatch there makes a byte comparison meaningless: two buffers of
  // the same length in different formats are not the same picture.
  if (mBitmap.info() != other.mBitmap.info()) return false;

  // Then the contents. The same reasoning as `Data::equals` -- a cache wants "is this the same picture", not "is
  // this the same object" -- and two images decoded from one file are the same picture.
  return mBitmap.data().equals(other.mBitmap.data());
}

} // namespace graphics
