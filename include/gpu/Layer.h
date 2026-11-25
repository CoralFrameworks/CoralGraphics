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

#ifndef CORAL_GPU_LAYER_H
#define CORAL_GPU_LAYER_H

#include <backend/Types.h>

#include <geometry/Color.h>
#include <geometry/Rect.h>

#include <cmath>
#include <cstdint>
#include <string>

namespace graphics::gpu {

/**
 * An offscreen render target a draw list renders *through*.
 *
 * ## What a layer is for
 *
 * Group opacity, a mask, a filter, and a blend mode that the fixed-function blend unit cannot express
 * all need the same thing: draw a group of commands into their own pixels, then composite the result
 * once. Drawing the group directly and multiplying each command's alpha is not the same picture --
 * where two of the group's children overlap, the overlap is composited twice and comes out darker than
 * the rest -- which is precisely the defect an offscreen layer exists to avoid.
 *
 * `graphics::Canvas::saveLayer` is the public spelling of this, and it is also how a mask is applied:
 * `maskLayer` multiplies the layer's coverage before the composite.
 *
 * ## Why this is a description rather than a resource
 *
 * A layer here records *what was asked for* -- a rectangle, a scale, whether a filter follows -- and not
 * a texture handle. The surface that consums a `DrawList` decides what to allocate, how many layers can
 * be live at once, and whether a layer can be folded into its parent when nothing actually needs it
 * isolated. Putting an RHI handle here would fix all three of those decisions at record time, on the
 * recording thread, which does not own the device.
 *
 * ## Nested layers
 *
 * A layer's `bounds` is in device pixels and is intersected with its parent's, so a nested layer is
 * never larger than the layer containing it. The `DrawList` records layers in the order `Canvas::saveLayer`
 * opened them, and a command names its layer by index -- so "which layer am I in" is one integer rather
 * than a stack the pass has to reconstruct.
 */
struct Layer {
  /**
   * The region the layer covers, in device pixels.
   *
   * `Canvas::saveLayer` takes its bounds in caller space; the recorder transforms them and stores the
   * device-space result. The surface *may* allocate only this region and translate inside it, which is
   * the difference between a UI that layers a 32x32 icon and one that allocates a full-screen target
   * for it. Whether it does is the surface's decision, so the offset is a field here rather than
   * something the commands bake in.
   */
  Rect bounds = {};

  /**
   * Resolution scale, 1.0 for a layer at device resolution.
   *
   * A caller that knows a layer will be blurred or scaled may ask for a smaller one; the composite
   * scales it back up. Stored rather than applied because applying it would have to rewrite every
   * command's device-space geometry, which is the recorder's job and not the description's.
   */
  float scale = 1.0f;

  /** Pixel format of the layer's storage. `Undefined` means "whatever the target uses". */
  backend::PixelFormat format = backend::PixelFormat::Undefined;

  /**
   * Whether the layer holds premultiplied alpha.
   *
   * True for every layer this library creates: the raster path stores premultiplied pixels and the GPU
   * path's shaders output premultiplied colour, so a composite is `src + dst * (1 - src.a)` and needs
   * no unpremultiply step. The field exists so that a caller-composited layer -- a video frame, a
   * platform buffer -- can say otherwise, and because a composite that guessed would be wrong in a way
   * that only shows on translucent content.
   */
  bool premultiplied = true;

  /**
   * Whether the composite that closes this layer reads the layer's colour or only its coverage.
   *
   * A mask layer is the second case: the coverage multiplies the parent's existing content rather than
   * being drawn over it. `Canvas::maskLayer` sets this.
   */
  bool asMask = false;

  /** The sentinel for @c maskImage: this layer has no mask and is composited as it is. */
  static constexpr uint32_t kNoMaskImage = 0xFFFFFFFFu;

  /**
   * The mask's index in the list's image table, when @c asMask is set.
   *
   * The mask is an ordinary bitmap as far as the recording is concerned -- `Canvas::maskLayer` takes one -- so it travels
   * the way every other bitmap does, through `DrawList::images`, and the surface uploads it with the rest. A field here
   * rather than a command of its own because a mask is a property of the *layer*: it is applied once, when the layer
   * closes, and not per draw.
   */
  uint32_t maskImage = kNoMaskImage;

  /**
   * The layer's global alpha, which the composite that closes it scales the result by.
   *
   * A field here rather than a property of the closing command, because it is a property of the *layer*: the
   * caller gives it to `Canvas::saveLayer` when the layer opens, and everything drawn inside is composited once at
   * that alpha when it closes. `Canvas::saveLayerAlpha` is the common way to set it.
   *
   * The recorder used to write the alpha onto the `OpenLayer` command's state and leave the closing command at 1.0,
   * which meant a translucent layer composited opaque -- the composite reads its alpha from the command that closes
   * it. Storing it on the layer puts it where both commands can see it and where a reader looking for "how strong is
   * this layer" finds it.
   */
  float alpha = 1.0f;

  /**
   * The blend mode the composite that closes this layer uses, when the layer's contents are combined with what was
   * already in the parent.
   *
   * A field here for the same reason `alpha` is: it is a property of the *layer*, given when the layer opens, and the
   * composite reads it when the layer closes. `Canvas::saveLayer` takes it from the paint it is handed, which is what
   * makes "draw this group with `kMultiply`" expressible at all -- the mode cannot be applied per command, because a
   * command that has already written the destination cannot read it back to blend against.
   *
   * The default is `kSrcOver` because that is what a layer with nothing else asked of it does: draw the group, then put
   * it over the parent.
   *
   * ## The recorder used to drop this, and what that looked like
   *
   * The closing command was written with `kSrcOver` hard-coded, so a layer opened with any other mode composited as
   * source-over -- silently, and for every mode above `kScreen` that meant the feature simply did not exist on the GPU
   * path. Measured on the demo's blend panel: ten of twelve swatches painted the same yellow, which is the source-over
   * answer, while the rasterizer produced the ten distinct colours the modes name.
   */
  BlendMode blendMode = BlendMode::kSrcOver;

  /**
   * True when the layer's RGB is per-channel text coverage and the composite must solve linear light
   * against the destination snapshot instead of source-over.
   */
  bool linearLightText = false;
  graphics::Color textColor = graphics::Color::Black();
  float linearLightGamma = 2.2f;
  float linearLightContrast = 0.0f;

  /** The mask's rule when `asMask` is set. */
  enum class MaskRule : uint8_t {
    /** The mask's alpha is the coverage. */
    Alpha,
    /** Rec. 709 luminance times the mask's alpha is the coverage. */
    Luminance,
  };
  MaskRule maskRule = MaskRule::Alpha;
  /**
   * True when `maskImage` describes the layer's own region (0..1 across the layer) rather than the
   * whole target at device coordinates.
   *
   * `Layer::setClipRadius`/`setClipPath` build a mask the size of the layer, so the composite maps its
   * 0..1 range onto the layer region. `Canvas::maskLayer`'s general contract is a mask rendered at any
   * resolution and stretched over the layer; SVG and Lottie build theirs at target resolution and rely
   * on device-space sampling, which is the default (`false`).
   */
  bool maskLocal = false;

  /** A debug name, shown in a GPU capture. */
  std::string label;

  /**
   * @return the layer's width in device pixels, rounded up.
   *
   * Rounded up rather than to nearest, because a layer covers its bounds: a bound whose right edge is at 10.4
   * needs an eleventh pixel to hold the part of it that is covered, and rounding down would clip it. The
   * offsets are not applied here -- where the layer sits is `bounds`' own origin, which is what the pass
   * translates by.
   */
  [[nodiscard]] int pixelWidth() const noexcept {
    const float width = bounds.getRight() - bounds.getLeft();
    return width > 0.0f ? static_cast<int>(std::ceil(width)) : 0;
  }

  /** @return the layer's height in device pixels, rounded up. See `pixelWidth` for why up. */
  [[nodiscard]] int pixelHeight() const noexcept {
    const float height = bounds.getBottom() - bounds.getTop();
    return height > 0.0f ? static_cast<int>(std::ceil(height)) : 0;
  }

  /** @return true when the layer covers no device pixels and should be skipped entirely. */
  [[nodiscard]] bool isEmpty() const noexcept { return pixelWidth() == 0 || pixelHeight() == 0; }
};

} // namespace graphics::gpu

#endif // CORAL_GPU_LAYER_H
