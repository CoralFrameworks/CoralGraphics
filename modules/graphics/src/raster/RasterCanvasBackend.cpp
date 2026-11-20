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
 * See the License for the specific language governing permissions and limitations under the
 * License.
 */

#include <private/graphics/CanvasBackend.h>

#include <private/graphics/Blend.h>
#include <private/graphics/Flatten.h>
#include <private/graphics/Rasterizer.h>

#include <minilog/log.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace graphics {

namespace {

/** Converts a colour from the shared value type to the 8-bit form the rasterizer blends in. */
raster::BlendSource toBlendSource(const Color& color) {
  // Clamped before rounding: a component outside [0,1] is a caller error that should become the nearest
  // representable channel rather than wrapping, and a negative scaled by 255 would otherwise land as a large
  // positive once shifted into the packed word.
  const auto channel = [](const Scalar value) {
    const Scalar clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<uint32_t>(std::lround(clamped * 255.0f));
  };
  return raster::BlendSource { channel(color.red), channel(color.green), channel(color.blue),
    channel(color.alpha) };
}

/**
 * @return the scale factor of @p matrix, taken as the larger of its two basis-vector lengths.
 *
 * A non-uniform transform has two different scales and the flattener takes one. The larger is chosen because the
 * errors are not symmetric: flattening too finely costs time and nothing else, while flattening too coarsely is
 * visible. Taking the smaller would leave a stretched curve visibly faceted.
 */
Scalar scaleOf(const Matrix& matrix) {
  // The first two columns of the upper-left 2x2, which are the images of the x and y unit vectors. Spelled out
  // per component rather than by indexing `vec`, which is an array of `Vector4` rows rather than of scalars.
  const Point xBasis { matrix.vec[0].x, matrix.vec[0].y };
  const Point yBasis { matrix.vec[1].x, matrix.vec[1].y };
  const Scalar xScale = std::sqrt(xBasis.x * xBasis.x + xBasis.y * xBasis.y);
  const Scalar yScale = std::sqrt(yBasis.x * yBasis.x + yBasis.y * yBasis.y);
  return std::max(xScale, yScale);
}

/**
 * The scanline rasterizer, behind the `CanvasBackend` interface.
 *
 * ## What moved here, and what deliberately did not
 *
 * This is the whole of the pixels: the flood fill, the contour fill and its per-pixel shader sampling, the
 * inverse-transform image sampler, the layer composite, the mask multiply, the clip mask and the glyph blit. The
 * arithmetic is the arithmetic that used to sit in `Canvas.cpp`, moved rather than rewritten -- `raster::fillContours`
 * still reports the same coverages, the premultiplication order is unchanged and so are the clamps, because these
 * paths are pinned by tests that compare bytes and a "harmless" tidy-up in any of them would be a behaviour change
 * that only a diff of two images could show.
 *
 * What did *not* move is anything a caller can observe as a decision: the order of the early-outs, the warnings, the
 * transform stack and the clip rectangle. Those are the contract, and they are in `Canvas.cpp` where a reading of
 * `Canvas.h` reaches them.
 *
 * ## Why the scratch contours live here
 *
 * `Canvas::Impl` used to hold one `std::vector<raster::Contour>` reused across draws, because a `std::vector`
 * allocation inside every draw is the sort of cost that shows up only in a profile. The scratch belongs on this side
 * of the split now: the canvas hands over a `pathops::Path`, which is the most it can say without knowing how the
 * geometry becomes pixels, and this file owns the flattened contours from the moment they exist. Keeping the vector
 * on the canvas would mean the canvas asking the backend to lend it one -- a pool with an owner that is not the user,
 * which is a worse thing to reason about than a reused temporary that dies with the drawing call.
 *
 * ## One entry per canvas state, and why
 *
 * The canvas's state stack and this one's are pushed and popped together, one for one, and the reason is the layer
 * composite. `onRestore` runs before the canvas pops, so a layer's composite rectangle has to be read *now*: the
 * clip the layer was drawn under is on the entry being popped, and the clip it composites into is on the entry below
 * it. Neither is available from the canvas, which has already taken the rectangle out of the state it is discarding.
 */
class RasterCanvasBackend final : public CanvasBackend {
public:
  explicit RasterCanvasBackend(Bitmap& bitmap) : target_(&bitmap), currentBitmap_(&bitmap) {}

  // ------------------------------------------------------------------------------------------------
  // Target
  // ------------------------------------------------------------------------------------------------

  [[nodiscard]] Rect deviceBounds() const noexcept override {
    return Rect::MakeLTRB(0.0f, 0.0f, static_cast<Scalar>(target_->width()),
            static_cast<Scalar>(target_->height()));
  }

  [[nodiscard]] bool isRaster() const noexcept override { return true; }

  /** True: the pixels are here now, so a draw reaches them immediately and eadback is a copy. */
  [[nodiscard]] bool hasTarget() const noexcept override { return currentBitmap_ != nullptr; }

  [[nodiscard]] Bitmap* pixels() noexcept override { return currentBitmap_; }

  // ------------------------------------------------------------------------------------------------
  // GPU-only operations
  // ------------------------------------------------------------------------------------------------
  //
  // Nothing recorded, so nothing to submit: the three below answer as the header says the raster backend does. The
  // read-back is the real one, and it is a copy for the same reason a snapshot is -- what a caller gets must not change
  // the next time the canvas is drawn into.

  [[nodiscard]] Bitmap readback() override {
    // The *current* target, like `pixels()`: a caller reading back inside a layer asked about the layer, and
    // `makeDeepCopy` is what makes the answer independent of the pixels it came from.
    return currentBitmap_->makeDeepCopy();
  }

  // ------------------------------------------------------------------------------------------------
  // State
  // ------------------------------------------------------------------------------------------------

  void onSave() override {
    // The mask is shared rather than copied and that is not an optimisation: a mask is large, a state is copied on
    // every `save`, and a mask is never modified after it is built -- only replaced by a narrower one.
    //
    // The layer is *not* inherited, and that is the whole of "a save does not re-open a layer". The pixels are
    // inherited -- a draw inside the layer has to land in the layer whether or not it is inside a nested save, which is
    // why `bitmap` is copied -- but the *ownership* is not: the entry that composited would otherwise be every nested
    // save as well as the state `beginLayer` pushed, so each inner `restore` would composite the whole layer, and a
    // half-opacity group with two children would come out at 192 and 224 instead of 128.
    entries_.push_back(entries_.back());
    entries_.back().ownsLayer = false;
  }

  bool onRestore() override {
    Entry& top = entries_.back();
    const bool closedLayer = top.ownsLayer;

    // Before the pop, because the entry being discarded is the only place the layer's clip rectangle exists.
    if (closedLayer) compositeLayer();

    if (entries_.size() <= 1) return false;

    // The entry below already is the state this pop returns to: it has been there all along, and the layer's own
    // narrowing only ever touched the entry on top. The bitmap is read *after* the pop rather than captured before,
    // because a nested save's entry carries the same layer pointer and popping it must not move the target.
    entries_.pop_back();
    currentBitmap_ = entries_.back().bitmap;
    return closedLayer;
  }

  void reset(const Rect& deviceBounds) override {
    // The target is the clip until someone says otherwise, which is what makes a backend usable the moment it exists
    // rather than only after a push. Clearing first is what makes a second `create` over the same backend
    // unambiguous; a canvas has one for its whole life, so this runs once in practice.
    entries_.clear();
    Entry base;
    base.deviceClip = deviceBounds;
    base.bitmap = target_;
    entries_.push_back(std::move(base));
    currentBitmap_ = target_;
  }

  void setDeviceClip(const Rect& deviceClip) override { entries_.back().deviceClip = deviceClip; }

  bool beginLayer(const Rect& deviceBounds, const Paint& paint) override {
    // The canvas is not asked for the bitmap here: this is the backend that owns the target, so the layer is the
    // size of *its* target and the canvas only says which region of it the layer is allowed to touch.
    Bitmap* const parent = currentBitmap_;
    if (parent == nullptr) return false;

    // ## Why the layer is the size of the whole target
    //
    // The alternative is to allocate only @p deviceBounds and carry an offset, so that every drawing call has to
    // translate device coordinates into layer-local ones. That is a change to every draw path below, and the offset
    // has to be threaded through the rasterizer's clip test, the bitmap sampler and the glyph blitter -- three places
    // to get wrong for a memory saving that only matters when a document layers a small shape over a large canvas.
    //
    // So the layer is the full size of the target and device coordinates are unchanged inside it. The trade is stated
    // rather than hidden: a layer costs four bytes per pixel of the *canvas*, not of the shape.
    Bitmap layerPixels = Bitmap::allocate(parent->info());
    if (layerPixels.isEmpty()) {
      // The canvas logs this: the decision to carry on is the canvas's, because it is the canvas that has to keep
      // the save/restore pair balanced. All this reports is that there are no layer pixels to draw into.
      return false;
    }

    // A copy of the current entry, then the fields a layer changes. The clip mask is inherited by the copy, which is
    // what makes a draw inside the layer obey a path clip applied outside it; it cannot be *narrowed* by the
    // intersection, because a coverage mask is per pixel rather than per shape, so the two masks intersect where the
    // composite reads them rather than here.
    //
    // The clip rectangle needs no work either: the canvas has already intersected it with the layer's bounds and
    // handed it over through `setDeviceClip`.
    Entry entry = entries_.back();
    entry.layer = std::make_shared<Bitmap>(std::move(layerPixels));
    entry.layerParent = parent;
    entry.layerPaint = paint;
    entry.ownsLayer = true;
    entry.bitmap = entry.layer.get();
    entries_.push_back(std::move(entry));

    currentBitmap_ = entries_.back().bitmap;
    return true;
  }

  void beginRecordingScope() override {}
  void endRecordingScope(gpu::DrawList& out) override { (void)out; }
  bool appendRecording(const gpu::DrawList& list, const Point translation) override {
    (void)list;
    (void)translation;
    // The raster backend records nothing to append into; the caller draws instead.
    return false;
  }
  [[nodiscard]] bool inRecordingScope() const override { return false; }

  void maskLayer(const Bitmap& mask, const MaskMode mode, bool alignToLayer) override {
    (void)alignToLayer; // The CPU path already stretches the mask over the layer's pixels.
    Bitmap* const layer = entries_.back().layer.get();
    // A caller that masked the canvas rather than a layer has already been warned by `Canvas::maskLayer`; all this has
    // to do is refuse to multiply the canvas's own pixels, which would erase everything already drawn.
    if (layer == nullptr) return;
    if (mask.isEmpty()) return;

    Pixmap layerPixels = layer->lockPixels();
    const Pixmap maskPixels = mask.makePixmap();
    if (layerPixels.isEmpty() || maskPixels.isEmpty()) return;

    // ## The mask is stretched to the layer
    //
    // A caller that renders a mask at its own resolution should get a defined answer rather than a mask that is
    // silently ignored or silently clipped at the top-left corner. Nearest-neighbour is the honest filter here: it
    // invents no colours, and a mask is a coverage signal rather than a picture.
    const double scaleX = static_cast<double>(maskPixels.width()) / layerPixels.width();
    const double scaleY = static_cast<double>(maskPixels.height()) / layerPixels.height();

    for (int y = 0; y < layerPixels.height(); ++y) {
      // The mask row this layer row reads, clamped: the last row of a stretched mask must not read past its end.
      const int maskY = std::clamp(static_cast<int>(y * scaleY), 0, maskPixels.height() - 1);

      for (int x = 0; x < layerPixels.width(); ++x) {
        const int maskX = std::clamp(static_cast<int>(x * scaleX), 0, maskPixels.width() - 1);

        // Un-premultiplied, because that is this accessor's contract. The three colour channels are therefore the
        // mask's own colours, which is what a luminance rule needs, and the alpha is its coverage -- which is what an
        // alpha rule needs. Reading the raw premultiplied bytes would confound the two.
        const uint32_t texel = maskPixels.getPixelARGB32(maskX, maskY);
        const auto alpha = static_cast<Scalar>((texel >> 24) & 0xFFu) / 255.0f;

        Scalar coverage = alpha;
        if (mode == MaskMode::kLuminance) {
          // Rec. 709's coefficients, which are the ones SVG 1.1 states for a mask. Note the multiplication by alpha:
          // a black pixel that is fully transparent and a black pixel that is fully opaque both have no luminance,
          // and both must hide the layer, but a *white* transparent pixel must hide it too -- so luminance alone is
          // not enough.
          const auto red = static_cast<Scalar>((texel >> 16) & 0xFFu) / 255.0f;
          const auto green = static_cast<Scalar>((texel >> 8) & 0xFFu) / 255.0f;
          const auto blue = static_cast<Scalar>(texel & 0xFFu) / 255.0f;
          coverage = (0.2125f * red + 0.7154f * green + 0.0721f * blue) * alpha;
        }

        if (coverage >= 1.0f) continue; // nothing to do, which is most of a mask's interior
        if (coverage <= 0.0f) {
          layerPixels.setPixelARGB32(x, y, 0);
          continue;
        }

        // Scaling the alpha is the whole operation. The layer is premultiplied, so scaling its alpha scales its
        // stored colour by the same factor -- and the accessors do the converting on both sides, so there is no
        // second premultiplication here to disagree with the first.
        const uint32_t pixel = layerPixels.getPixelARGB32(x, y);
        const auto layerAlpha = static_cast<Scalar>((pixel >> 24) & 0xFFu) / 255.0f;
        const auto scaled = static_cast<uint32_t>(std::lround(layerAlpha * coverage * 255.0f));
        layerPixels.setPixelARGB32(x, y, (scaled << 24) | (pixel & 0x00FFFFFFu));
      }
    }
  }

  bool clipPath(const pathops::Path& devicePath, const FillType fillType) override {
    // The path arrives by reference from a named local in the canvas, and it has to: a handle taken from a temporary
    // dies with it, and a pointer into a destroyed path's guts reads as a clip that silently lets nothing through.
    //
    // The mask is allocated at the target's size and in its format, because it is rasterised by the ordinary path
    // filler -- which needs a destination it knows how to write one into, and the coverage is read back out of its
    // alpha. An empty allocation means no mask, and the canvas keeps the rectangular clip it already has.
    Bitmap maskPixels = Bitmap::allocate(currentBitmap_->info());
    if (maskPixels.isEmpty()) return false;

    Pixmap mask = maskPixels.lockPixels();

    // Flattened here rather than by the canvas: a curve's flattening tolerance is in device pixels, so this is the
    // one step that has to happen after the transform either way, and the backend that rasterises the result is the
    // only thing that can use it.
    const std::vector<raster::Contour> contours = raster::flattenPath(devicePath);
    raster::fillContours(contours, mask, fillType,
            [&mask](const int x, const int y, const uint32_t coverage) {
              // White at the coverage: the mask's alpha is what is read, and writing the coverage into every channel
              // keeps it premultiplied and therefore consistent with how the rest of the bitmap is stored.
              mask.setPixelARGB32(x, y, (coverage << 24) | (coverage << 16) | (coverage << 8) | coverage);
            });

    // The new mask replaces the old one only where the old one allowed. Doing this as a second pass rather than by
    // rasterising both into one buffer keeps the two independent: a path clip under a path clip is an intersection,
    // and getting that wrong is how a nested clip becomes a union.
    Entry& entry = entries_.back();
    if (entry.clipMask != nullptr) {
      for (int y = 0; y < mask.height(); ++y) {
        for (int x = 0; x < mask.width(); ++x) {
          const uint32_t narrow = (mask.getPixelARGB32(x, y) >> 24) & 0xFFu;
          if (narrow == 0) continue;
          const uint32_t existing = entry.clipMask->coverage(x, y);
          const uint32_t combined = (narrow * existing + 127u) / 255u;
          mask.setPixelARGB32(x, y, (combined << 24) | (combined << 16) | (combined << 8) | combined);
        }
      }
    }

    auto maskHolder = std::make_shared<ClipMask>();
    maskHolder->pixels = std::move(maskPixels);
    // The mask's own extent, kept because it is the only thing that can bound a *shaped* clip. The per-pixel
    // coverage test still decides, so this being loose costs a few rejected samples and never drops one; it exists so
    // that the layer composite iterates the same rectangle it used to, which is what keeps an unsupported blend mode
    // on an entirely clipped-out layer warning exactly as often as it did before.
    maskHolder->bounds = Rect::MakeLTRB(0.0f, 0.0f, static_cast<Scalar>(mask.width()),
            static_cast<Scalar>(mask.height()));
    entry.clipMask = std::move(maskHolder);

    // The canvas's clip rectangle stays as it is -- it is the fast reject, and the mask is the exact answer.
    // Narrowing it to the path's bounds here would be an optimisation that the coverage test already performs.
    return true;
  }

  // ------------------------------------------------------------------------------------------------
  // Drawing
  // ------------------------------------------------------------------------------------------------

  void clear(const Color& color) override { target_->erase(color); }

  void drawColor(const Color& color, const BlendMode blendMode) override {
    Pixmap pixmap = currentBitmap_->lockPixels();
    if (pixmap.isEmpty()) return;

    const raster::BlendSource source = toBlendSource(color);
    const Rect& clip = entries_.back().deviceClip;

    // Clamped to the target in whole pixels, because the clip is a float rectangle and iterating it directly would
    // either drop the partially covered edge pixels or run past the end of the buffer.
    const int firstX = std::max(0, static_cast<int>(std::floor(clip.getLeft())));
    const int firstY = std::max(0, static_cast<int>(std::floor(clip.getTop())));
    const int lastX = std::min(pixmap.width() - 1, static_cast<int>(std::ceil(clip.getRight())) - 1);
    const int lastY = std::min(pixmap.height() - 1, static_cast<int>(std::ceil(clip.getBottom())) - 1);

    for (int y = firstY; y <= lastY; ++y) {
      for (int x = firstX; x <= lastX; ++x) {
        // A flood fill is clipped like anything else. Skipping this leaves a shaped clip doing nothing to
        // `drawColor`, which is the one draw that does not go through the rasterizer and so the one that would be
        // forgotten.
        const uint32_t coverage = clipCoverage(x, y);
        if (coverage == 0) continue;
        const uint32_t destination = pixmap.getPixelARGB32(x, y);
        pixmap.setPixelARGB32(x, y, raster::blendWith(blendMode, source, destination, coverage));
      }
    }
  }

  void drawRect(const Rect& deviceRect, const Paint& paint) override {
    // Through the same contour fill a path takes, so that a dedicated rectangle entry point cannot drift from it. The
    // canvas routes its own `drawRect` through `drawPath` -- see the note there -- which is why this is the
    // straight-line implementation rather than a second pipeline. The transform is the identity because a rectangle
    // arrives already in device space and has no caller space left to sample a shader in.
    const std::vector<raster::Contour> contours = flattenRect(deviceRect);
    fill(contours, FillType::Winding, paint, Matrix {});
  }

  void drawPath(const pathops::Path& devicePath, const FillType fillType, const Scalar scale,
          const Matrix& deviceToUser, const Paint& paint) override {
    if (devicePath.isEmpty()) return;

    // Geometry is taken to device space by the canvas *before* flattening, so the flattening tolerance is a constant
    // in pixels. Flattening first and scaling after would give a zoomed-in curve too few segments, and it would look
    // faceted while the same curve at 1:1 looked smooth -- the error has to be measured in the units the viewer sees.
    std::vector<raster::Contour> contours;
    if (paint.style == Paint::Style::kStroke) {
      // The width is scaled into device space. Under a rotation or a uniform scale that is exact; under a non-uniform
      // one it is an approximation, because a stroke of constant width in caller space is an ellipse in general and
      // this produces a circle. Stated here rather than silently done, since the difference is visible on a strongly
      // skewed transform and invisible everywhere else.
      contours = raster::strokePath(devicePath, paint.strokeWidth * scale);
    } else {
      contours = raster::flattenPath(devicePath);
    }

    fill(contours, fillType, paint, deviceToUser);
  }

  void drawGlyphRun(const pathops::Path& deviceOutline, const Paint& paint,
    uint64_t glyphCacheKey = 0) override {
    static_cast<void>(glyphCacheKey);
    if (deviceOutline.isEmpty()) return;

    // A glyph run is a fill and nothing else: `Canvas::drawTextBlob` hands over an outline it has already placed in
    // device space, and paints it with the fill style, so the scale a stroke would need has no meaning here.
    const std::vector<raster::Contour> contours = raster::flattenPath(deviceOutline);
    fill(contours, FillType::Winding, paint, Matrix {});
  }

  // ------------------------------------------------------------------------------------------------
  // Image sampling: nearest, bilinear, cubic, and the mip chain
  // ------------------------------------------------------------------------------------------------

  /** One level of a mip chain: tightly packed premultiplied ARGB. */
  struct MipPlane {
    const uint32_t* pixels = nullptr;
    int width = 0;
    int height = 0;
  };

  /** The levels built for the current draw, and the storage they share. */
  std::vector<MipPlane> mipPlanes_;
  std::vector<uint32_t> mipPixels_;
  int mipLevels_ = 0;

  static uint32_t texelAt(const Pixmap& pixmap, const int x, const int y) {
    return pixmap.getPixelARGB32(std::clamp(x, 0, pixmap.width() - 1), std::clamp(y, 0, pixmap.height() - 1));
  }

  static uint32_t texelAt(const MipPlane& plane, const int x, const int y) {
    return plane.pixels[static_cast<size_t>(std::clamp(y, 0, plane.height - 1)) *
                          static_cast<size_t>(plane.width) +
                        static_cast<size_t>(std::clamp(x, 0, plane.width - 1))];
  }

  /** Per-channel blend of two premultiplied texels, which is what makes a transparent edge fade rather than tint. */
  static uint32_t blendTexels(const uint32_t from, const uint32_t to, const Scalar t) {
    uint32_t out = 0;
    for (int shift = 0; shift <= 24; shift += 8) {
      const auto a = static_cast<Scalar>((from >> shift) & 0xFFu);
      const auto b = static_cast<Scalar>((to >> shift) & 0xFFu);
      out |= static_cast<uint32_t>(std::clamp(a + (b - a) * t, 0.0f, 255.0f) + 0.5f) << shift;
    }
    return out;
  }

  template <typename Source>
  static uint32_t sampleNearest(const Source& source, const Scalar u, const Scalar v) {
    return texelAt(source, static_cast<int>(std::floor(u)), static_cast<int>(std::floor(v)));
  }

  template <typename Source>
  static uint32_t sampleBilinear(const Source& source, const Scalar u, const Scalar v) {
    // Texel centres sit at `index + 0.5`, so the filter's coordinate is the source coordinate less half a texel.
    const Scalar x = u - 0.5f;
    const Scalar y = v - 0.5f;
    const auto x0 = static_cast<int>(std::floor(x));
    const auto y0 = static_cast<int>(std::floor(y));
    const Scalar fx = x - static_cast<Scalar>(x0);
    const Scalar fy = y - static_cast<Scalar>(y0);
    const uint32_t top = blendTexels(texelAt(source, x0, y0), texelAt(source, x0 + 1, y0), fx);
    const uint32_t bottom = blendTexels(texelAt(source, x0, y0 + 1), texelAt(source, x0 + 1, y0 + 1), fx);
    return blendTexels(top, bottom, fy);
  }

  /** The Mitchell-Netravali cubic kernel, in `CubicResampler`'s own two parameters. */
  static Scalar cubicWeight(const Scalar x, const Scalar b, const Scalar c) {
    const Scalar ax = std::abs(x);
    if (ax < 1.0f) {
      return ((12.0f - 9.0f * b - 6.0f * c) * ax * ax * ax + (-18.0f + 12.0f * b + 6.0f * c) * ax * ax +
              (6.0f - 2.0f * b)) / 6.0f;
    }
    if (ax < 2.0f) {
      return ((-b - 6.0f * c) * ax * ax * ax + (6.0f * b + 30.0f * c) * ax * ax +
              (-12.0f * b - 48.0f * c) * ax + (8.0f * b + 24.0f * c)) / 6.0f;
    }
    return 0.0f;
  }

  template <typename Source>
  static uint32_t sampleCubic(const Source& source, const Scalar u, const Scalar v, const CubicResampler cubic) {
    // Separable 4x4: four weights across, four down. Normalised by the sum of each axis so that a flat region stays
    // flat where the taps clamp to the edge -- without that the border comes out darker, which is the classic "cubic
    // darkens the edge" artifact.
    const Scalar x = u - 0.5f;
    const Scalar y = v - 0.5f;
    const auto x0 = static_cast<int>(std::floor(x));
    const auto y0 = static_cast<int>(std::floor(y));
    const Scalar fx = x - static_cast<Scalar>(x0);
    const Scalar fy = y - static_cast<Scalar>(y0);

    Scalar wx[4] = {};
    Scalar wy[4] = {};
    Scalar sumX = 0.0f;
    Scalar sumY = 0.0f;
    for (int i = 0; i < 4; ++i) {
      wx[i] = cubicWeight(static_cast<Scalar>(i) - 1.0f - fx, cubic.b, cubic.c);
      wy[i] = cubicWeight(static_cast<Scalar>(i) - 1.0f - fy, cubic.b, cubic.c);
      sumX += wx[i];
      sumY += wy[i];
    }
    if (sumX == 0.0f || sumY == 0.0f) return sampleBilinear(source, u, v);
    for (int i = 0; i < 4; ++i) {
      wx[i] /= sumX;
      wy[i] /= sumY;
    }

    Scalar channels[4] = {};
    for (int j = 0; j < 4; ++j) {
      for (int i = 0; i < 4; ++i) {
        const uint32_t texel = texelAt(source, x0 - 1 + i, y0 - 1 + j);
        const Scalar weight = wx[i] * wy[j];
        for (int channel = 0; channel < 4; ++channel) {
          channels[channel] += weight * static_cast<Scalar>((texel >> (8 * channel)) & 0xFFu);
        }
      }
    }
    uint32_t out = 0;
    for (int channel = 0; channel < 4; ++channel) {
      out |= static_cast<uint32_t>(std::clamp(channels[channel], 0.0f, 255.0f) + 0.5f) << (8 * channel);
    }
    return out;
  }

  /**
   * Builds levels 0..@p maxLevel, which is all a minifying draw needs.
   *
   * Level 0 is copied so that every level is a plain plane, and the storage is reused through `mipPixels_` so a steady
   * state of minified draws does not allocate. Only the levels the LOD asks for are built: a 4K image drawn at half size
   * needs one, not eleven.
   */
  void buildMipChain(const Pixmap& source, const int maxLevel) {
    mipPlanes_.clear();
    mipLevels_ = 0;

    int total = 0;
    int width = source.width();
    int height = source.height();
    for (int level = 0; level <= maxLevel; ++level) {
      total += width * height;
      if (width == 1 && height == 1) break;
      width = std::max(1, width / 2);
      height = std::max(1, height / 2);
    }
    mipPixels_.assign(static_cast<size_t>(total), 0u);

    size_t offset = 0;
    width = source.width();
    height = source.height();
    for (int level = 0; level <= maxLevel; ++level) {
      mipPlanes_.push_back(MipPlane { mipPixels_.data() + offset, width, height });
      ++mipLevels_;
      if (level == 0) {
        for (int y = 0; y < height; ++y) {
          for (int x = 0; x < width; ++x) {
            mipPixels_[offset + static_cast<size_t>(y) * static_cast<size_t>(width) +
                       static_cast<size_t>(x)] = source.getPixelARGB32(x, y);
          }
        }
      } else {
        // A box filter over the four texels of the level above: the standard choice, and the one that makes a
        // checkerboard converge to its average instead of to whichever texel the grid happened to land on.
        const MipPlane& finer = mipPlanes_[static_cast<size_t>(level) - 1];
        for (int y = 0; y < height; ++y) {
          for (int x = 0; x < width; ++x) {
            const uint32_t a = texelAt(finer, x * 2, y * 2);
            const uint32_t b = texelAt(finer, x * 2 + 1, y * 2);
            const uint32_t c = texelAt(finer, x * 2, y * 2 + 1);
            const uint32_t d = texelAt(finer, x * 2 + 1, y * 2 + 1);
            uint32_t average = 0;
            for (int shift = 0; shift <= 24; shift += 8) {
              const auto sum = static_cast<uint32_t>(((a >> shift) & 0xFFu) + ((b >> shift) & 0xFFu) +
                                                     ((c >> shift) & 0xFFu) + ((d >> shift) & 0xFFu));
              average |= ((sum + 2u) / 4u) << shift;
            }
            mipPixels_[offset + static_cast<size_t>(y) * static_cast<size_t>(width) +
                       static_cast<size_t>(x)] = average;
          }
        }
      }
      offset += static_cast<size_t>(width) * static_cast<size_t>(height);
      if (width == 1 && height == 1) break;
      width = std::max(1, width / 2);
      height = std::max(1, height / 2);
    }
  }

  /** Trilinear: the filter mode picks within a level, the mipmap mode picks between levels. */
  uint32_t sampleMipmapped(const Scalar u, const Scalar v, const Scalar lod, const SamplingOptions& sampling) const {
    if (mipLevels_ == 0) return 0;
    const int last = mipLevels_ - 1;
    const int level = std::clamp(static_cast<int>(std::floor(lod)), 0, last);
    const auto sample = [&sampling](const MipPlane& plane, const Scalar su, const Scalar sv) {
      if (sampling.useCubic) return sampleCubic(plane, su, sv, sampling.cubic);
      if (sampling.filter == FilterMode::kLinear) return sampleBilinear(plane, su, sv);
      return sampleNearest(plane, su, sv);
    };
    const uint32_t base = sample(mipPlanes_[static_cast<size_t>(level)], u, v);
    if (sampling.mipmap != MipmapMode::kLinear) return base;
    const int next = std::min(level + 1, last);
    if (next == level) return base;
    const Scalar t = std::clamp(lod - static_cast<Scalar>(level), 0.0f, 1.0f);
    return blendTexels(base, sample(mipPlanes_[static_cast<size_t>(next)], u, v), t);
  }

  void drawBitmap(const Bitmap& image, const Point deviceQuad[4], const Rect& deviceBounds,
          const Matrix& deviceToUser, const SamplingOptions& sampling, const Paint& paint) override {
    drawBitmapRect(image, Rect::MakeWH(static_cast<Scalar>(image.width()),
      static_cast<Scalar>(image.height())), deviceQuad, deviceBounds, deviceToUser, sampling, paint);
  }

  /**
   * A raster canvas has no device, so it cannot sample a texture another producer owns.
   *
   * Deliberately a no-op rather than an error: the caller is the same UI code that runs on both paths,
   * and the picture it asked for simply does not exist on a software canvas. A caller that needs the
   * content on the CPU path asks its producer for a `Bitmap` instead (a readback), not for this call.
   */
  void drawExternalImage(const ExternalImage& /*image*/, const Point /*deviceQuad*/[4],
          const Rect& /*deviceBounds*/, const SamplingOptions& /*sampling*/,
          const Paint& /*paint*/) override {}

  void drawBitmapRect(const Bitmap& image, const Rect& sourceRect, const Point deviceQuad[4],
          const Rect& deviceBounds, const Matrix& deviceToUser, const SamplingOptions& sampling,
          const Paint& paint) override {
    const Pixmap source = image.makePixmap();
    if (source.isEmpty()) return;
    if (sourceRect.isEmpty()) return;

    Pixmap destination = currentBitmap_->lockPixels();
    if (destination.isEmpty()) return;

    // The caller-space origin of the image, which is the one piece of the placement that the device-space quad does
    // not carry: the source's pixel grid is indexed from the image's own top-left, so the lookup has to subtract it
    // before flooring. Recovered from the quad rather than passed separately so that the placement the canvas
    // composed is the only description of where the image landed.
    const Point origin = deviceToUser * deviceQuad[0];
    const Point rightEdge = deviceToUser * deviceQuad[1] - origin;
    const Point downEdge = deviceToUser * deviceQuad[3] - origin;
    const Scalar determinant = rightEdge.x * downEdge.y - rightEdge.y * downEdge.x;
    if (std::abs(determinant) < 1e-6f) return;

    // The quad's bounding box, taken as an over-estimate under rotation. That costs a few rejected samples and never
    // drops a pixel.
    Scalar left = deviceQuad[0].x;
    Scalar top = deviceQuad[0].y;
    Scalar right = deviceQuad[0].x;
    Scalar bottom = deviceQuad[0].y;
    for (int i = 1; i < 4; ++i) {
      left = std::min(left, deviceQuad[i].x);
      top = std::min(top, deviceQuad[i].y);
      right = std::max(right, deviceQuad[i].x);
      bottom = std::max(bottom, deviceQuad[i].y);
    }

    const Rect& clip = deviceBounds;
    const int firstX = std::max(0, static_cast<int>(std::floor(std::max(left, clip.getLeft()))));
    const int firstY = std::max(0, static_cast<int>(std::floor(std::max(top, clip.getTop()))));
    const int lastX = std::min(destination.width() - 1,
            static_cast<int>(std::ceil(std::min(right, clip.getRight()))) - 1);
    const int lastY = std::min(destination.height() - 1,
            static_cast<int>(std::ceil(std::min(bottom, clip.getBottom()))) - 1);

    // ## The level of detail, and why it is measured from the quad
    //
    // Mipmapping is a *minification* decision, so the LOD comes from how much smaller the quad is than the source: the
    // ratio of source texels to device pixels along each of the quad's two edges, and the larger of the two because the
    // finer axis is the one that aliases. A magnification draw computes zero and builds nothing, which keeps the common
    // case -- a sticker drawn at its own size -- free of both the chain and the allocation.
    const Scalar edgeRight = std::hypot(deviceQuad[1].x - deviceQuad[0].x, deviceQuad[1].y - deviceQuad[0].y);
    const Scalar edgeDown = std::hypot(deviceQuad[3].x - deviceQuad[0].x, deviceQuad[3].y - deviceQuad[0].y);
    Scalar lod = 0.0f;
    if (edgeRight > 0.0f && edgeDown > 0.0f) {
      const Scalar scaleX = static_cast<Scalar>(sourceRect.getWidth()) / edgeRight;
      const Scalar scaleY = static_cast<Scalar>(sourceRect.getHeight()) / edgeDown;
      lod = std::max(0.0f, std::log2(std::max(scaleX, scaleY)));
    }
    mipLevels_ = 0;
    if (sampling.mipmap != MipmapMode::kNone && lod > 0.0f) {
      buildMipChain(source, static_cast<int>(std::ceil(lod)));
    }

    for (int deviceY = firstY; deviceY <= lastY; ++deviceY) {
      for (int deviceX = firstX; deviceX <= lastX; ++deviceX) {
        // Back through the inverse to caller space, then into the source's own coordinates by subtracting its
        // origin -- so a rotated cell samples its neighbour's pixel rather than smearing along a row.
        const Point inCallerSpace = deviceToUser * Point { static_cast<Scalar>(deviceX) + 0.5f,
                                                           static_cast<Scalar>(deviceY) + 0.5f };
        const Scalar deltaX = inCallerSpace.x - origin.x;
        const Scalar deltaY = inCallerSpace.y - origin.y;
        const Scalar localX = (deltaX * downEdge.y - deltaY * downEdge.x) / determinant;
        const Scalar localY = (rightEdge.x * deltaY - rightEdge.y * deltaX) / determinant;
        const Scalar sourceU = sourceRect.getLeft() + localX * sourceRect.getWidth();
        const Scalar sourceV = sourceRect.getTop() + localY * sourceRect.getHeight();

        // Reject a sample outside the source sub-rectangle: clamping there would smear the atlas neighbour into the
        // glyph. The bound is in *image* coordinates, where a texel occupies `[index, index + 1)`.
        if (sourceU < sourceRect.getLeft() || sourceV < sourceRect.getTop() ||
            sourceU >= sourceRect.getRight() || sourceV >= sourceRect.getBottom()) {
          continue;
        }

        uint32_t texel = 0;
        if (mipLevels_ > 0) {
          texel = sampleMipmapped(sourceU, sourceV, lod, sampling);
        } else if (sampling.useCubic) {
          texel = sampleCubic(source, sourceU, sourceV, sampling.cubic);
        } else if (sampling.filter == FilterMode::kLinear) {
          texel = sampleBilinear(source, sourceU, sourceV);
        } else {
          texel = sampleNearest(source, sourceU, sourceV);
        }

        // The colour filter, applied per texel because an image has a different colour in every pixel -- unlike a
        // shape, where one filtered colour serves the whole draw. `applyToPremultipliedArgb` is the filter's own 8-bit
        // path, so this is the same arithmetic a shape's colour goes through rather than a second implementation of
        // it.
        //
        // This was missing entirely: a filter on the paint changed a rectangle and did nothing to an image, so the
        // same paint behaved differently depending on what it was drawing.
        if (paint.colorFilter != nullptr) texel = paint.colorFilter->applyToPremultipliedArgb(texel);

        if (paint.color.alpha < 1.0f) {
          // The paint's alpha scales the image's own, which is what a caller tinting an icon expects. The result is
          // clamped to a byte before being shifted into place: `imageAlpha * paint.color.alpha` is a value up to
          // 255, and shifting that into the top byte wraps for anything above 1 -- so an alpha of 0.1 turned a
          // pixel's alpha into a large number and produced a colour nobody asked for.
          const auto imageAlpha = static_cast<uint32_t>((texel >> 24) & 0xFFu);
          const uint32_t scaled = std::min(255u, static_cast<uint32_t>(std::lround(imageAlpha * paint.color.alpha)));
          texel = (texel & 0x00FFFFFFu) | (scaled << 24);
        }

        const uint32_t coverage = clipCoverage(deviceX, deviceY);
        if (coverage == 0) continue;

        const raster::BlendSource texelSource { (texel >> 16) & 0xFFu, (texel >> 8) & 0xFFu, texel & 0xFFu,
          (texel >> 24) & 0xFFu };
        const uint32_t destinationPixel = destination.getPixelARGB32(deviceX, deviceY);
        uint32_t outPixel = raster::blendWith(paint.blendMode, texelSource, destinationPixel, coverage);
        if (paint.channelMask != 0x0F) {
          // A per-channel write mask. Subpixel text draws the same coverage three times with one bit
          // set, so the ordinary source-over blend computes each channel against its own coverage and
          // the other channels keep the destination exactly as it was.
          const uint32_t mask = ((paint.channelMask & 0x1u) != 0 ? 0x00FF0000u : 0u) |
                                ((paint.channelMask & 0x2u) != 0 ? 0x0000FF00u : 0u) |
                                ((paint.channelMask & 0x4u) != 0 ? 0x000000FFu : 0u) |
                                ((paint.channelMask & 0x8u) != 0 ? 0xFF000000u : 0u);
          outPixel = (destinationPixel & ~mask) | (outPixel & mask);
        }
        destination.setPixelARGB32(deviceX, deviceY, outPixel);
      }
    }
  }

private:
  /**
   * A clip that is not a rectangle: one coverage byte per device pixel.
   *
   * Shared rather than copied because a state is copied on every `save`, and a mask is large. Two states that share a
   * clip share the same mask, which is correct -- a mask is never modified after it is built, only replaced by a
   * narrower one.
   */
  struct ClipMask {
    Bitmap pixels;

    /** The mask's own extent in device space, which is the only bound a shaped clip has. */
    Rect bounds;

    /** Coverage at a device pixel, or zero when the pixel is outside the mask. */
    [[nodiscard]] uint32_t coverage(const int x, const int y) const {
      if (x < 0 || y < 0 || x >= pixels.width() || y >= pixels.height()) return 0;
      // The mask is stored as premultiplied RGBA so it can be rasterised by the ordinary path filler, which knows
      // how to write one. Only the alpha carries the coverage, and a premultiplied pixel with zero alpha has no
      // colour, so the alpha *is* the coverage.
      return (pixels.makePixmap().getPixelARGB32(x, y) >> 24) & 0xFFu;
    }
  };

  /** One entry of the clip and layer stack, mirroring one entry of the canvas's state stack. */
  struct Entry {
    /** The clip in device space. A rectangle with no area means nothing is drawable. */
    Rect deviceClip;

    /** The clip's shape when it is not a rectangle, or null when the rectangle is the whole story. */
    std::shared_ptr<ClipMask> clipMask;

    /**
     * The offscreen pixels this entry draws into, when it began a layer.
     *
     * Null for an entry that merely inherited them, and shared so that a copy of an entry keeps the layer alive.
     */
    std::shared_ptr<Bitmap> layer;

    /**
     * Whether *this* entry opened the layer, and is therefore the one that composites it.
     *
     * Distinct from @c layer being non-null, which every state inside the layer also reports. Only the entry
     * `beginLayer` pushed owns the pixels, so only its pop composites -- see the note in `onSave`.
     */
    bool ownsLayer = false;

    /** Where the layer is composited when it is popped. Null when this entry is not a layer. */
    Bitmap* layerParent = nullptr;

    /** How the layer meets its parent when it is popped, and how much of it. */
    Paint layerPaint;

    /** The pixels this entry draws into, which is the layer's for a layer entry and the target's otherwise. */
    Bitmap* bitmap = nullptr;
  };

  /**
   * @return how much of the device pixel (@p x, @p y) survives the clip, from zero to 255.
   *
   * The single place a draw asks whether it is allowed to write, so that a rectangular clip and a shaped one cannot
   * disagree -- which they would if each drawing call tested the rectangle itself and then forgot the mask, or the
   * other way round.
   */
  [[nodiscard]] uint32_t clipCoverage(const int x, const int y) const {
    const Entry& entry = entries_.back();
    const Rect& clip = entry.deviceClip;

    // Pixel centres, to match the coverage the rasterizer reports: a pixel whose centre is outside the clip is not
    // drawn, and the boundary pixels are decided the same way the rasterizer decides a shape's edge.
    const Scalar centreX = static_cast<Scalar>(x) + 0.5f;
    const Scalar centreY = static_cast<Scalar>(y) + 0.5f;
    if (centreX < clip.getLeft() || centreX > clip.getRight()) return 0;
    if (centreY < clip.getTop() || centreY > clip.getBottom()) return 0;

    if (entry.clipMask == nullptr) return 255;
    return entry.clipMask->coverage(x, y);
  }

  /** @return @p rect as the four lines a fill can consume. */
  [[nodiscard]] static std::vector<raster::Contour> flattenRect(const Rect& rect) {
    raster::Contour contour;
    contour.points = { Point { rect.getLeft(), rect.getTop() }, Point { rect.getRight(), rect.getTop() },
      Point { rect.getRight(), rect.getBottom() }, Point { rect.getLeft(), rect.getBottom() } };
    contour.closed = true;
    return { std::move(contour) };
  }

  /**
   * Fills @p contours into the current target, one pixel at a time.
   *
   * Everything a fill and a glyph run have in common lives here: the empty-contour refusal, the pixel lock, the
   * filtered colour taken once, the shader's matrix inverse taken once, and the loop that asks the clip and blends.
   *
   * @param deviceToUser the inverse of the transform @p contours were produced by, which is the space a shader is
   *        sampled in. It is only read when there is a shader to sample, so a caller with no gradient may pass the
   *        identity and pay nothing.
   */
  void fill(const std::vector<raster::Contour>& contours, const FillType fillType, const Paint& paint,
          const Matrix& deviceToUser) {
    if (contours.empty()) return;

    Pixmap pixmap = currentBitmap_->lockPixels();
    if (pixmap.isEmpty()) return;

    // The filter is applied to the source colour once, not per pixel. That is exact rather than an optimisation: a
    // colour matrix is linear in the premultiplied components, and source-over is linear in the source, so filtering
    // the colour and then blending is the same as filtering every contribution the same way. A filter that were not
    // linear -- a table applied per channel is not, across pixels -- would still be exact here, because every pixel of
    // one draw goes through the same filter and the same colour.
    Color sourceColor = paint.color;
    if (paint.colorFilter != nullptr) sourceColor = paint.colorFilter->applyTo(sourceColor);

    const raster::BlendSource source = toBlendSource(sourceColor);

    // ## A shader costs one matrix inverse and one sample per pixel
    //
    // The inverse is taken once, outside the loop, because a gradient's local matrix does not change across a draw --
    // and taking it per pixel would be a division and a determinant inside the inner loop for a value that is
    // constant. A transform that is not invertible collapses the shape to nothing, so the identity stands in: nothing
    // it can sample will be written where the coverage is zero.
    const bool shaded = paint.hasShader();
    const Matrix deviceToShader = shaded && deviceToUser.isInvertible() ? deviceToUser : Matrix {};
    const Scalar shadeAlpha = sourceColor.alpha;
    const Shader* const shader = shaded ? paint.shader.get() : nullptr;

    // One call per pixel the shape touches, with the coverage already computed. The clip is tested here rather
    // than being passed to the rasterizer, because the rasterizer's job is coverage and the clip is a property of
    // the canvas -- keeping them separate is what lets the clip become a real path later without touching this.
    raster::fillContours(contours, pixmap, fillType,
            [this, &pixmap, &source, &paint, shader, &deviceToShader, shadeAlpha](const int x, const int y,
                                                                                 const uint32_t coverage) {
              // One test for both kinds of clip. Asking the backend rather than comparing against the rectangle here
              // is what keeps a shaped clip from being forgotten in one draw path out of four.
              const uint32_t clipped = (coverage * clipCoverage(x, y) + 127u) / 255u;
              if (clipped == 0) return;

              raster::BlendSource pixelSource = source;
              if (shader != nullptr) {
                // Pixel centres, so a gradient across a shape is symmetric about its edges rather than offset by
                // half a pixel -- which is visible as a gradient that looks slightly shifted from the geometry it
                // fills.
                const Point device { static_cast<Scalar>(x) + 0.5f, static_cast<Scalar>(y) + 0.5f };
                Color sampled = shader->sampleAt(deviceToShader * device);
                // The paint's alpha is a global multiplier on whatever the shader produced, so fading a gradient
                // needs no second shader.
                if (paint.colorFilter != nullptr) sampled = paint.colorFilter->applyTo(sampled);
                sampled.alpha *= shadeAlpha;
                pixelSource = toBlendSource(sampled);
              }

              const uint32_t destination = pixmap.getPixelARGB32(x, y);
              pixmap.setPixelARGB32(x, y, raster::blendWith(paint.blendMode, pixelSource, destination, clipped));
            });
  }

  void compositeLayer() {
    const Entry& layer = entries_.back();
    if (layer.layer == nullptr || layer.layerParent == nullptr) return;

    Bitmap& destination = *layer.layerParent;
    currentBitmap_ = layer.layerParent;

    Pixmap destinationPixels = destination.lockPixels();
    const Pixmap sourcePixels = layer.layer->makePixmap();
    if (destinationPixels.isEmpty() || sourcePixels.isEmpty()) return;

    // Any shape the layer had is already baked into its pixels, so the composite is the rectangle the layer was given
    // plus whatever clip the entry below imposes -- which is exactly the rectangle the canvas handed over when the
    // layer was opened, because `setDeviceClip` has only ever narrowed it since.
    //
    // A shaped clip has no rectangle of its own, so the mask's extent stands in for it. Using the *parent's* rectangle
    // instead would be looser in the one case that matters: a clipPath applied inside the layer can widen the
    // rectangle back out to the parent's, and a composite loop over a region where the layer is entirely clipped
    // would then reach pixels that the old code short-circuited before it ever tested the blend mode.
    const Entry& parent = entries_[entries_.size() - 2];
    Rect clip = layer.deviceClip;
    if (parent.clipMask != nullptr) {
      clip = Rect::MakeLTRB(std::max(clip.getLeft(), parent.clipMask->bounds.getLeft()),
              std::max(clip.getTop(), parent.clipMask->bounds.getTop()),
              std::min(clip.getRight(), parent.clipMask->bounds.getRight()),
              std::min(clip.getBottom(), parent.clipMask->bounds.getBottom()));
    }
    const uint32_t globalAlpha =
            static_cast<uint32_t>(std::lround(std::clamp(layer.layerPaint.color.alpha, 0.0f, 1.0f) * 255.0f));
    if (globalAlpha == 0) return;

    const int firstX = std::max(0, static_cast<int>(std::floor(clip.getLeft())));
    const int firstY = std::max(0, static_cast<int>(std::floor(clip.getTop())));
    const int lastX = std::min(std::min(destinationPixels.width(), sourcePixels.width()) - 1,
                               static_cast<int>(std::ceil(clip.getRight())) - 1);
    const int lastY = std::min(std::min(destinationPixels.height(), sourcePixels.height()) - 1,
                               static_cast<int>(std::ceil(clip.getBottom())) - 1);
    if (firstX > lastX || firstY > lastY) return;

    const BlendMode mode = layer.layerPaint.blendMode;
    if (layer.layerPaint.linearLightLayer) {
      const float gamma = std::max(layer.layerPaint.linearLightGamma, 0.1f);
      const float contrast = std::clamp(layer.layerPaint.linearLightContrast, 0.0f, 1.0f);
      const float textAlpha = std::clamp(layer.layerPaint.color.alpha, 0.0f, 1.0f);
      const float text[3] = { layer.layerPaint.color.red, layer.layerPaint.color.green,
        layer.layerPaint.color.blue };
      for (int y = firstY; y <= lastY; ++y) {
        for (int x = firstX; x <= lastX; ++x) {
          const uint32_t sourcePixel = sourcePixels.getPixelARGB32(x, y);
          const uint32_t destinationPixel = destinationPixels.getPixelARGB32(x, y);
          const float destinationAlpha = static_cast<float>((destinationPixel >> 24) & 0xFFu) / 255.0f;
          float shaped[3] = { 0.0f, 0.0f, 0.0f };
          for (int channel = 0; channel < 3; ++channel) {
            const int shift = 16 - channel * 8;
            const float coverage = std::clamp(
              static_cast<float>((sourcePixel >> shift) & 0xFFu) / 255.0f * textAlpha, 0.0f, 1.0f);
            shaped[channel] = std::pow(std::max(coverage, 0.0f), 1.0f / gamma);
            shaped[channel] = (shaped[channel] * (1.0f + contrast)) /
                              (shaped[channel] * contrast + 1.0f);
          }
          const float textCoverage = std::max(shaped[0], std::max(shaped[1], shaped[2]));
          uint32_t output = 0;
          for (int channel = 0; channel < 3; ++channel) {
            const int shift = 16 - channel * 8;
            const float background = static_cast<float>((destinationPixel >> shift) & 0xFFu) / 255.0f;
            const float premultiplied = text[channel] * shaped[channel] +
                                        background * destinationAlpha * (1.0f - shaped[channel]);
            const uint32_t encoded = static_cast<uint32_t>(
              std::lround(std::clamp(premultiplied, 0.0f, 1.0f) * 255.0f));
            output |= encoded << shift;
          }
          const float outAlpha = textCoverage + destinationAlpha * (1.0f - textCoverage);
          output |= static_cast<uint32_t>(
            std::lround(std::clamp(outAlpha, 0.0f, 1.0f) * 255.0f)) << 24;
          destinationPixels.setPixelARGB32(x, y, output);
        }
      }
      return;
    }
    if (!raster::isBlendModeSupported(mode)) {
      MINILOG_WARN("[graphics/canvas] layer blend mode {} is not a mode; the layer is not composited", toString(mode));
      return;
    }

    for (int y = firstY; y <= lastY; ++y) {
      for (int x = firstX; x <= lastX; ++x) {
        const uint32_t pixel = sourcePixels.getPixelARGB32(x, y);
        const uint32_t sourceAlpha = (pixel >> 24) & 0xFFu;
        if (sourceAlpha == 0) continue; // nothing was drawn here, which is most of the layer most of the time

        // The layer's alpha, then the pixel's own. Read from the un-premultiplied accessor and re-premultiplied by
        // `toBlendSource`, so the two multiplications happen in the same order the rasterizer uses.
        const uint32_t alpha = (sourceAlpha * globalAlpha + 127u) / 255u;
        const auto unpremultiply = [sourceAlpha](const uint32_t channel) {
          return sourceAlpha == 0 ? 0u : std::min(255u, (channel * 255u + sourceAlpha / 2u) / sourceAlpha);
        };
        const raster::BlendSource source {unpremultiply((pixel >> 16) & 0xFFu), unpremultiply((pixel >> 8) & 0xFFu),
                                          unpremultiply(pixel & 0xFFu), alpha};

        destinationPixels.setPixelARGB32(x, y,
                raster::blendWith(mode, source, destinationPixels.getPixelARGB32(x, y), 255));
      }
    }
  }

  /** The target the canvas was created over, which a layer does not change. */
  Bitmap* target_ = nullptr;

  /** The pixels the current entry draws into: the target, or the layer on top of the stack. */
  Bitmap* currentBitmap_ = nullptr;

  /** The stack. The first entry is the base state, so a canvas `save` pushes above it. */
  std::vector<Entry> entries_;
};

} // namespace anonymous

// Declared rather than included: the GPU backend lives in `src/gpu`, and a raster translation unit that
// included its header would drag the RHI's device types into a file that has no business seeing them. One
// forward declaration is the whole of the coupling, and it is the direction a backend should depend in --
// the factory knows both, and neither knows the other.
std::unique_ptr<CanvasBackend> createGraphiteCanvasBackend(const std::shared_ptr<gpu::ContextImpl>& context,
        const std::shared_ptr<gpu::DrawList>& list);

std::unique_ptr<CanvasBackend> createCanvasBackend(Bitmap* bitmap,
        const std::shared_ptr<gpu::ContextImpl>& impl, const std::shared_ptr<gpu::DrawList>& list) {
  // ## What decides which backend, and why it is the *list* rather than the context
  //
  // A caller asking for a recording canvas supplies a list; a caller asking for pixels supplies a bitmap. The
  // context is a separate question -- whether the recording can later be *submitted* -- and it is deliberately
  // not what selects the backend, because a recording that will be submitted elsewhere is a legitimate thing
  // to make. Keying off the context instead is what made a null-context recording impossible, and with it any
  // test of the command stream that did not need a GPU: a recording backend never dereferences the context, so
  // requiring one was a requirement the code did not have.
  if (list != nullptr) return createGraphiteCanvasBackend(impl, list);

  // A raster backend needs pixels. A null bitmap here is a caller that asked for neither form, and refusing is
  // the honest answer: a canvas whose every call is dropped is the worst thing to hand back.
  if (bitmap == nullptr) return nullptr;
  return std::make_unique<RasterCanvasBackend>(*bitmap);
}

} // namespace graphics
