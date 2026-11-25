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

#ifndef CORAL_GRAPHICS_SURFACE_H
#define CORAL_GRAPHICS_SURFACE_H

#include <graphics/Bitmap.h>
#include <graphics/Canvas.h>
#include <graphics/Image.h>
#include <graphics/ImageInfo.h>
#include <graphics/Pixmap.h>

#include <geometry/Rect.h>

#include <memory>
#include <utility>

namespace graphics {

/**
 * A destination for drawing, and the pictures that come out of it.
 *
 * ## Why this is separate from Canvas
 *
 * A `Canvas` knows how to draw and nothing about what it draws into; a `Surface` owns pixels and hands out a
 * canvas to draw into them. Keeping them apart is what would let the same canvas code drive a bitmap now and a
 * GPU texture later, and it is why `Canvas::create` takes a `Bitmap&` rather than a surface.
 *
 * ## The frame, as a sequence of states
 *
 * A surface is *drawable*, and @c getCanvas returns the canvas that draws into it. @c snapshot ends the frame: it
 * produces an `Image` of what has been drawn and makes the surface drawable again for the next one. Asking for a
 * canvas twice without a snapshot between is refused rather than returning the same one -- a caller doing that is
 * confused about its own frame, and silently sharing the canvas would let it draw half of one frame and half of
 * the next with no indication.
 *
 * Nothing forces a snapshot, so a caller drawing a static picture may take a canvas once and keep it. The state
 * machine exists to catch the mistake, not to impose a frame loop.
 *
 * ## This is the software surface
 *
 * The pixels are a `Bitmap` in this process's memory. A GPU-backed surface would present the same interface, and
 * the state machine above -- which is about *when* drawing is allowed rather than *where* it goes -- is the part
 * that would carry over unchanged.
 */
class Surface {
public:
  ~Surface();

  Surface(const Surface&) = delete;
  Surface& operator=(const Surface&) = delete;

  /**
   * @return a surface of @p width by @p height pixels, or null when that cannot be allocated.
   *
   * The format is 8-bit RGBA, premultiplied -- what a compositor wants, and what every operation in this layer is
   * written against. The format is stated here rather than inferred from the size, because an implicit choice of
   * pixel layout is the kind that produces a picture nobody can explain.
   */
  [[nodiscard]] static std::unique_ptr<Surface> makeRaster(int width, int height);

  /** A raster surface in an explicitly chosen format. */
  [[nodiscard]] static std::unique_ptr<Surface> makeRaster(const ImageInfo& info);

  /**
   * A raster surface over pixels that already exist.
   *
   * The pixels are copied rather than referenced, because a surface is a place to draw and drawing into an image
   * somebody else holds would break the promise that an `Image` never changes.
   */
  [[nodiscard]] static std::unique_ptr<Surface> makeRaster(const Image& image);

  [[nodiscard]] int width() const noexcept { return mBitmap.width(); }
  [[nodiscard]] int height() const noexcept { return mBitmap.height(); }
  [[nodiscard]] const ImageInfo& info() const noexcept { return mBitmap.info(); }

  /** The rectangle from the origin to the surface's size. */
  [[nodiscard]] Rect bounds() const noexcept;

  /**
   * @return the canvas to draw into, or null when the surface already has one outstanding.
   *
   * The canvas stays valid until the next @c snapshot. Drawing through it after that is a use of a stale object,
   * which the returned `shared_ptr` at least keeps alive -- the surface simply stops consulting it, so the draw
   * has no effect rather than corrupting anything.
   */
  [[nodiscard]] std::shared_ptr<Canvas> getCanvas();

  /** @return true when a canvas from @c getCanvas is currently usable. */
  [[nodiscard]] bool isDrawable() const noexcept { return mCanvas == nullptr; }

  /**
   * @return an image of everything drawn so far, and ends the frame.
   *
   * A copy of the pixels, so the image does not change when the surface is drawn into again -- which is the whole
   * point of an `Image`, and the reason this cannot hand back a view.
   *
   * A snapshot with no canvas outstanding is still valid: it captures whatever is on the surface, including a
   * picture that was never drawn into. Refusing would make a caller's first frame fail for no useful reason.
   */
  [[nodiscard]] Image snapshot();

  /**
   * @return a read-only view of the current pixels, without ending the frame and without copying.
   *
   * For a caller that wants to look at what it has drawn -- to test against it, or to upload part of it -- and
   * does not need the immutability a snapshot provides. **The view is invalidated by the next draw**, because it
   * points into the surface's own buffer.
   */
  [[nodiscard]] Pixmap peekPixels() const noexcept { return mBitmap.makePixmap(); }

  /**
   * @return the surface's own bitmap, for a caller that has finished drawing and wants to keep the storage.
   *
   * Moves the pixels out, leaving the surface empty and unusable. A caller wanting to keep drawing should use
   * @c snapshot instead; this is for handing the storage onward when the surface is done with.
   */
  [[nodiscard]] Bitmap detachBitmap() noexcept;

private:
  explicit Surface(Bitmap bitmap) noexcept
    : mBitmap(std::move(bitmap)) {
  }

  Bitmap mBitmap;

  /**
   * The canvas, or null when the surface is drawable.
   *
   * Held so that @c isDrawable can answer without the canvas having to know about the surface, and cleared by
   * @c snapshot. A `shared_ptr` because the caller keeps its own reference: the canvas has to stay alive for as
   * long as the caller uses it, even after the surface has moved on to the next frame.
   */
  std::shared_ptr<Canvas> mCanvas;
};

} // namespace graphics

#endif //CORAL_GRAPHICS_SURFACE_H
