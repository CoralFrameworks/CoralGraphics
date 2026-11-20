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

#include <graphics/Surface.h>

#include <minilog/log.h>

#include <utility>

namespace graphics {

Surface::~Surface() = default;

std::unique_ptr<Surface> Surface::makeRaster(const int width, const int height) {
  return makeRaster(ImageInfo(width, height, ColorType::kRGBA_8888, AlphaType::kPremultiplied));
}

std::unique_ptr<Surface> Surface::makeRaster(const ImageInfo& info) {
  // The surface must be writable, which `Bitmap::allocate` guarantees and `Bitmap::fromData` does not. Going
  // through the allocating entry point here is what keeps `Canvas::create` from having to re-check it.
  Bitmap bitmap = Bitmap::allocate(info);
  if (bitmap.isEmpty()) {
    MINILOG_WARN("[graphics/surface] cannot allocate a {}x{} surface", info.width(), info.height());
    return nullptr;
  }

  // `new` rather than `make_unique`, because the constructor is private: the class controls how instances come
  // into being, which is what lets it guarantee a surface is never handed out without its pixels.
  return std::unique_ptr<Surface>(new Surface(std::move(bitmap)));
}

std::unique_ptr<Surface> Surface::makeRaster(const Image& image) {
  if (image.isEmpty()) return nullptr;

  // A copy of the pixels, so drawing into the surface cannot alter the image it was made from. That is the
  // promise `Image` makes, and taking its buffer directly would break it from a distance.
  Bitmap bitmap = Bitmap::allocate(image.info());
  if (bitmap.isEmpty()) {
    MINILOG_WARN("[graphics/surface] cannot copy a {}x{} image into a surface", image.width(), image.height());
    return nullptr;
  }

  // Row by row through the accessors rather than a straight `memcpy` of the buffers. The two have the same
  // stride -- both are tightly packed by `ImageInfo` -- but the accessors are what guarantee the copy is of
  // *pixels*: a raw copy would be correct today and silently wrong the first time either side grew a padded
  // stride. The cost is a per-pixel call on a path that runs once per surface, which is not the place to be
  // clever.
  const Pixmap source = image.makePixmap();
  Pixmap destination = bitmap.lockPixels();
  for (int y = 0; y < source.height(); ++y) {
    for (int x = 0; x < source.width(); ++x) {
      destination.setPixelARGB32(x, y, source.getPixelARGB32(x, y));
    }
  }

  return std::unique_ptr<Surface>(new Surface(std::move(bitmap)));
}

Rect Surface::bounds() const noexcept {
  return Rect::MakeLTRB(0.0f, 0.0f, static_cast<Scalar>(width()), static_cast<Scalar>(height()));
}

std::shared_ptr<Canvas> Surface::getCanvas() {
  if (mCanvas != nullptr) {
    // Refused rather than re-returned. A caller asking twice without a snapshot is confused about its own frame,
    // and handing back the same canvas would let it draw half of one frame and half of the next with nothing to
    // indicate that it had. A null is a bug the caller can see.
    MINILOG_WARN("[graphics/surface] getCanvas called twice without a snapshot in between; the surface is still "
                 "drawable only after snapshot or detach");
    return nullptr;
  }

  mCanvas = Canvas::create(mBitmap);
  if (mCanvas == nullptr) {
    // `Canvas::create` logs why. Nothing to add.
    return nullptr;
  }
  return mCanvas;
}

Image Surface::snapshot() {
  // The frame ends here whether or not a canvas was ever taken: a caller that drew nothing still gets a picture of
  // what the surface holds, which for a fresh surface is transparent black.
  const std::shared_ptr<Canvas> canvas = std::move(mCanvas);
  mCanvas.reset();

  // A copy, which is what makes the snapshot immutable. Handing back a view would produce an `Image` that changed
  // the next time the surface was drawn into -- the one thing an `Image` must never do.
  return Image::fromBitmap(mBitmap);
}

Bitmap Surface::detachBitmap() noexcept {
  // The canvas, if any, refers to the bitmap by pointer; releasing it first is what stops a later draw through a
  // stale canvas from writing into a bitmap this surface no longer owns.
  mCanvas.reset();
  return std::move(mBitmap);
}

} // namespace graphics
