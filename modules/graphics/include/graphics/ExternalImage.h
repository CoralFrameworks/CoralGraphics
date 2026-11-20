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

#ifndef CORAL_GRAPHICS_EXTERNALIMAGE_H
#define CORAL_GRAPHICS_EXTERNALIMAGE_H

#include <backend/Handles.h>

#include <cstdint>

namespace graphics {

/**
 * A texture another producer on the same device owns, named for one draw.
 *
 * This is the CPU-side description a canvas records when an application wants a live preview -- an
 * effect graph's frame, a video decoder's output, another window's surface -- composited as part of its
 * own drawing. Nothing is copied and no readback happens: the command names the view and the GPU binds
 * it at submit time.
 *
 * ## Why it lives in `graphics` rather than in `gpu`
 *
 * The drawing API is `graphics::Canvas`, and a caller that records an external image has to be able to
 * name it without depending on the GPU recorder's internals. `gpu::DrawList` reuses this type (it is an
 * alias there), so one definition serves both the recording command list and the public canvas call.
 *
 * ## Lifetime
 *
 * The texture belongs to the producer. It must stay valid until the frame that names it has finished on
 * the GPU -- for a `gpu::Surface` producer that means until its next `submit`, and for a ring texture it
 * means the producer must not recycle the slot before then. Neither the canvas nor the renderer owns,
 * creates or destroys it.
 *
 * ## Coordinates
 *
 * `width`/`height` are the texture's size in texels; the drawing rectangle is a parameter of the draw
 * call, so a producer does not have to know where the UI put it.
 */
struct ExternalImage {
  /**
   * The texture to sample, when the caller has its handle.
   *
   * Either this or @p view is required: a producer that owns the texture (a `gpu::Surface` target, an
   * effect graph's frame) can hand both, while a consumer that only received a view passes the view and
   * leaves this invalid. Both are used when both are set -- the backend that resolves a view natively
   * takes the view, and one that binds textures resolves the view itself.
   */
  backend::TextureHandle texture = backend::TextureHandle::invalid();

  /** The view to sample. A same-device texture view; the recorder never creates or destroys it. */
  backend::TextureViewHandle view = backend::TextureViewHandle::invalid();

  /** The texture's size in texels, which is what a caller's layout arithmetic needs. */
  uint32_t width = 0;
  uint32_t height = 0;

  /** The view's format. Used to choose the sampler path; sampled as normalised colour. */
  backend::PixelFormat format = backend::PixelFormat::Undefined;

  /**
   * Whether the texels are premultiplied by alpha.
   *
   * Video and effect output are opaque and this is false; a UI layer composited through an external
   * texture is usually true, which is what makes source-over blend it correctly.
   */
  bool premultiplied = false;

  [[nodiscard]] bool isValid() const noexcept {
    return (texture.valid() || view.valid()) && width > 0 && height > 0 &&
      format != backend::PixelFormat::Undefined;
  }
};

} // namespace graphics

#endif // CORAL_GRAPHICS_EXTERNALIMAGE_H
