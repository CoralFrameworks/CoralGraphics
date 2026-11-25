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

#include <graphics/Bitmap.h>

#include <algorithm>
#include <cstring>

namespace graphics {

Bitmap Bitmap::allocate(const ImageInfo& info) {
  const size_t byteSize = info.computeByteSize();
  if (byteSize == 0) {
    // The size is zero for every reason a caller could get this wrong -- a negative dimension, an unknown
    // colour type, a total that overflows -- and `computeByteSize` is where that judgement lives. Refusing on
    // its answer means the reasoning is in one place rather than repeated here.
    return {};
  }

  Bitmap bitmap;
  bitmap.mInfo = info;
  bitmap.mData = Data::makeUninitialised(byteSize);
  bitmap.mRowBytes = info.minRowBytes();
  // The allocation is the only place a writable bitmap comes from, so the flag is set here and nowhere else.
  bitmap.mWritable = true;
  return bitmap;
}

Bitmap Bitmap::allocate(const int width, const int height) {
  return allocate(ImageInfo(width, height, ColorType::kRGBA_8888, AlphaType::kPremultiplied));
}

Bitmap Bitmap::fromData(const ImageInfo& info, const Data& data, const size_t rowBytes) {
  Bitmap bitmap;

  // The buffer has to cover the geometry, or every later bounds check would agree with a false premise. The
  // stride is checked separately because a stride narrower than a row would make two rows overlap.
  const size_t minimumStride = info.minRowBytes();
  const size_t stride = rowBytes == 0 ? minimumStride : rowBytes;
  if (stride < minimumStride || !info.isValid()) return {};

  const size_t required = stride * static_cast<size_t>(info.height() - 1) + minimumStride;
  if (data.size() < required) return {};

  bitmap.mInfo = info;
  bitmap.mData = data;
  bitmap.mRowBytes = stride;
  // Borrowed bytes are read-only: they may be a decoder's output, a memory mapping or a shared constant, and
  // there is no way to tell from here which. Writing through a view of them would corrupt the source.
  bitmap.mWritable = false;
  return bitmap;
}

Bitmap Bitmap::makeDeepCopy() const {
  if (isEmpty()) return {};

  Bitmap copy;
  copy.mInfo = mInfo;
  // A copy of the bytes, not of the `Data`, which is what makes the two independent.
  copy.mData = Data::makeCopy(mData.bytes(), mData.size());
  copy.mRowBytes = mRowBytes;
  copy.mWritable = true;
  return copy;
}

Pixmap Bitmap::makePixmap() const noexcept {
  if (isEmpty()) return {};
  return Pixmap(mInfo, mData);
}

Pixmap Bitmap::lockPixels() noexcept {
  if (isEmpty() || !mWritable) return {};
  return Pixmap(mInfo, mData);
}

bool Bitmap::erase(const Color& color) noexcept {
  Pixmap pixmap = lockPixels();
  if (pixmap.isEmpty()) return false;

  // Through the pixel accessor rather than by filling bytes, because the byte pattern for a colour depends on
  // the format: an `RGB_565` bitmap's white is not an `RGBA_8888` bitmap's white, and filling with a byte
  // pattern would give one of them the wrong colour while looking like it worked.
  const uint32_t packed = Color::toIntColor(color);
  for (int y = 0; y < pixmap.height(); ++y) {
    for (int x = 0; x < pixmap.width(); ++x) {
      pixmap.setPixelARGB32(x, y, packed);
    }
  }
  return true;
}

} // namespace graphics
