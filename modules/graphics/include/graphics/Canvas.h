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

#ifndef CORAL_GRAPHICS_CANVAS_H
#define CORAL_GRAPHICS_CANVAS_H

#include <graphics/Bitmap.h>
#include <graphics/Enums.h>
#include <graphics/ExternalImage.h>
#include <graphics/Paint.h>
#include <graphics/SamplingOptions.h>
#include <graphics/Path.h>
#include <graphics/TextBlob.h>

#include <geometry/Arc.h>
#include <geometry/Matrix.h>
#include <geometry/Rect.h>
#include <geometry/RoundRect.h>

#include <functional>
#include <memory>
#include <string>

namespace graphics {

class Font;

/**
 * The GPU device a recorder canvas records into.
 *
 * Forward declared rather than included, because the RHI is a private dependency of the canvas's backend: a caller
 * names `Canvas`, and nothing about the type it records into belongs in this header. The canvas that has one is made
 * by a private factory below, so no caller ever has to know this name.
 */
namespace gpu {
class ContextImpl;
struct DrawList;
} // namespace gpu

/**
 * Where drawing commands go.
 *
 * ## What is behind this interface
 *
 * Every draw is performed by a backend: today the scanline rasterizer in `src/raster`, on the CPU, into a `Bitmap`;
 * a GPU backend that records the same calls into a command list is the next one, and it is why this interface exists
 * in the shape it does. The arithmetic that decides *what* to draw -- the transform stack, the clip, the fill rule,
 * the stroke outline -- is the same either way and is the part worth getting right first, so it lives here, in the
 * canvas, while the backend is told what to fill and with what.
 *
 * The split is at the semantic level and not the pixel level: a backend is handed device-space geometry and a paint,
 * never a row of spans. See the backend's own header for why.
 *
 * ## The state stack
 *
 * Transform and clip are stack state, and @c save and @c restore bracket a change to them. That is not
 * bookkeeping convenience: a caller that transforms and draws must be able to get back to where it was without
 * inverting the matrix by hand, and an unbalanced @c save is a much easier bug to find than a transform that
 * has silently drifted. The stack belongs to the canvas, so an unbalanced save cannot leak past the end of a
 * frame.
 *
 * ## Coordinates
 *
 * Everything a caller passes is in the canvas's *current* space -- whatever the transform stack says. Device
 * pixels are an implementation detail and the transform to them is applied once per draw rather than exposed.
 * The one place device pixels leak through is the clip, which is intersected in device space because an
 * axis-aligned device rectangle is the only clip this canvas represents exactly.
 */
class Image;
class Picture;

class Canvas {
public:
  ~Canvas();

  Canvas(const Canvas&) = delete;
  Canvas& operator=(const Canvas&) = delete;

  /**
   * @return a canvas drawing into @p bitmap, or null when the bitmap has no writable pixels.
   *
   * The canvas borrows the bitmap rather than owning it, so the caller decides its lifetime -- a canvas that
   * owned its target could not be reused across frames without reallocating the pixels, which is exactly what
   * a frame loop wants to avoid.
   *
   * A read-only bitmap is refused rather than accepted with drawing disabled. A canvas that silently drew
   * nothing would make every call in a frame a no-op with no indication why, which is a far worse diagnostic
   * than a null.
   */
  [[nodiscard]] static std::unique_ptr<Canvas> create(Bitmap& bitmap);

  /**
   * @return a canvas whose drawing is recorded for a GPU pass rather than rasterised into pixels, or null.
   *
   * There is no bitmap behind this canvas and there cannot be: a GPU backend records into a command list and allocates
   * its target at submit time, so the pixels a raster canvas would be lent do not exist yet. That is what makes this a
   * second factory rather than an overload of @c create -- the interface a caller draws through is identical, and the
   * difference is entirely in what happens to a call after it returns.
   *
   * **@c target must not be called on a canvas made this way.** It returns a reference, so there is nothing it could
   * return that means "no pixels"; a caller that needs pixels from a GPU canvas asks the backend to read them back
   * through @c Surface instead. @c deviceBounds answers from the rectangle given here, so laying a frame out against
   * this canvas works exactly as it does against a raster one.
   *
   * @param width  the drawing surface's width in device pixels.
   * @param height the drawing surface's height in device pixels.
   * @param context the device to record into. A null one is refused: a canvas with neither pixels nor a context could
   *        not draw at all, and a canvas whose every call was silently dropped is worse than a null.
   */
  [[nodiscard]] static std::unique_ptr<Canvas> createRecorderBackend(int width, int height,
          const std::shared_ptr<gpu::ContextImpl>& context);

  /**
   * A canvas that records into a draw list the caller already holds.
   *
   * The difference from @c createRecorderBackend is ownership of the list, and it is the difference a
   * multi-recorder frame needs: when N threads each record and one surface submits, the surface has to own the
   * list a canvas appends to -- it could not submit a list it cannot reach. So this form is handed the list
   * rather than making one, and it exists for exactly that caller.
   *
   * @param list the list to append to. Null is refused, because a recording backend with nowhere to record is
   *        a canvas whose every call is dropped.
   */
  [[nodiscard]] static std::unique_ptr<Canvas> createRecordingBackend(int width, int height,
          const std::shared_ptr<gpu::ContextImpl>& context, const std::shared_ptr<gpu::DrawList>& list);

  /**
   * The bitmap this canvas draws into, which the canvas does not own.
   *
   * Inside a layer this is the layer's pixels rather than the surface's, which is what makes a caller reading the
   * target mid-frame see the same buffer a draw would reach. Not meaningful on a canvas from
   * @c createRecorderBackend, which has no pixels at all.
   */
  /** True when calls are recorded for a later GPU submit rather than rasterised immediately. */
  [[nodiscard]] bool isRecording() const noexcept;

  [[nodiscard]] Bitmap& target() noexcept;
  [[nodiscard]] const Bitmap& target() const noexcept;

  /**
   * @return the size of the target in device pixels.
   *
   * Device pixels rather than the current transform's space, because this is what a caller lays a frame out
   * against and a frame's size is physical. A caller needing a logical extent divides by its own scale, which
   * it knows and this does not.
   *
   * Answered from the rectangle the canvas was created with rather than from an allocated bitmap, so that a
   * recording canvas -- which has no pixels until it is submitted -- still reports a size to lay out against.
   */
  [[nodiscard]] Rect deviceBounds() const noexcept;

  // ================================================================================================
  // State
  // ================================================================================================

  /** Pushes the current transform and clip. Every call must be matched by @c restore. */
  void save();

  /**
   * Pops back to the most recent @c save.
   *
   * A @c restore with nothing to pop is ignored rather than treated as an error. The alternatives -- asserting,
   * or corrupting the stack -- turn a caller's off-by-one into a crash somewhere else, and there is nothing
   * useful a canvas can do about it: the state it would restore is already current.
   */
  void restore();

  /** @return how many @c save calls are outstanding. Diagnostics only; a caller should not need this. */
  [[nodiscard]] int saveCount() const noexcept;

  /**
   * Replaces the transform.
   *
   * Replaces rather than concatenates, because a caller setting a transform means "from here on, this is the
   * coordinate system", and a concatenating `setTransform` would make the result depend on how many times it
   * had been called -- which is not what the name says.
   */
  void setTransform(const Matrix& matrix);

  /** Applies @p matrix on top of the current transform: the new transform is @p matrix times the old. */
  void concat(const Matrix& matrix);

  /** @return the current transform, from caller space to device pixels. */
  [[nodiscard]] Matrix transform() const noexcept;

  /**
   * @return the total scale factor of the current transform.
   *
   * Used to decide how finely to flatten curves: the flattening tolerance is in device pixels, so a curve under
   * a 4x scale needs four times the segments to look as smooth as the same curve at 1:1. Reported as one number
   * rather than as the matrix because that is all the flattener needs, and a non-uniform transform's two scales
   * would have to be reconciled into one anyway.
   */
  [[nodiscard]] Scalar transformScale() const noexcept;

  /**
   * Intersects the clip with @p rect, given in the current space.
   *
   * Intersection only, never replacement: a clip that could be widened could not be represented as a single
   * rectangle here, and the ability to widen it is what a scrolled viewport needs -- which is exactly what
   * @c save and @c restore provide.
   */
  void clipRect(const Rect& rect);

  /**
   * Intersects the clip with a path, in the caller's coordinates.
   *
   * The clip stops being a rectangle and becomes a coverage mask, so a curved or antialiased edge clips as smoothly as
   * the shape inside it is drawn. Two path clips in a row intersect, which is what nesting them means -- not a union,
   * and not the second replacing the first.
   *
   * The path's fill type decides what is inside it, so a clip written even-odd keeps its holes.
   */
  void clipPath(const Path& path);

  /**
   * Begins an offscreen layer: everything drawn until the matching @c restore goes into separate pixels and is then
   * composited onto the canvas once, through @p paint.
   *
   * This is what group opacity needs, and what a mask needs. Drawing a group at half opacity by halving each child's
   * alpha is not the same picture: where two children overlap, the overlap is composited twice and comes out darker
   * than the rest of the group. A layer makes the group one object, so the overlap is composited once.
   *
   * @param bounds the region the layer is expected to touch, in the caller's coordinates. It narrows the layer's clip
   *        and therefore what is composited back; a bound that is too small loses part of the drawing.
   * @param paint only its alpha and its blend mode are read. The alpha multiplies the whole layer, and the blend mode
   *        decides how the layer meets what is underneath.
   *
   * The layer's pixels are the size of the canvas rather than of @p bounds, so a layer costs four bytes per canvas
   * pixel whatever its bounds. That is a deliberate simplification, recorded with its reasoning in the implementation.
   */
  void saveLayer(const Rect& bounds, const Paint& paint);

  /** @c saveLayer with a uniform alpha, which is the common case. */
  void saveLayerAlpha(const Rect& bounds, uint8_t alpha);

  /**
   * Multiplies the layer currently open by @p mask.
   *
   * This is what a mask is: the layer keeps its colours and loses its coverage wherever the mask is dark or transparent.
   * It is deliberately an operation on a *layer* rather than on the canvas -- masking a canvas with no layer open would
   * erase everything already drawn on it, which is not what "mask this element" means in any document format.
   *
   * Call it between @c saveLayer and @c restore, after the element has been drawn. With no layer open it does nothing
   * and says so, rather than erasing the canvas.
   *
   * @param mask the coverage, in device space. A mask whose size differs from the layer's is stretched to it, so a caller
   *        that renders a mask at its own resolution gets a defined answer rather than a dropped mask -- though an exact
   *        match is what keeps it sharp, and a layer is the size of the canvas.
   * @param mode whether the mask's alpha or its luminance decides. See @c MaskMode: the two differ for any mask whose
   *        colour varies while its alpha does not.
   */
  /**
   * @param alignToLayer true when @p mask describes the layer's own region (0..1 across it), false
   *        when it is a mask rendered at target resolution in device coordinates. The default matches
   *        the historical contract: a mask of any size is stretched over the layer, but its sampling
   *        is in the layer's target space. `Layer`'s own clip masks pass true because they are built at
   *        layer resolution and drawn in layer-local coordinates.
   */
  void maskLayer(const Bitmap& mask, MaskMode mode = MaskMode::kAlpha, bool alignToLayer = false);

  /** @return true when a clip path is in force, so a caller can tell a shaped clip from a rectangular one. */
  [[nodiscard]] bool hasClipPath() const noexcept;

  /** @return true when the clip is nowhere, so every subsequent draw is a no-op. */
  [[nodiscard]] bool isClipEmpty() const noexcept;

  /** @return the clip in device space, the intersection of everything applied so far. */
  [[nodiscard]] Rect deviceClipBounds() const noexcept;

  // ================================================================================================
  // Drawing
  // ================================================================================================

  /**
   * Fills the whole device area with @p color, ignoring both the transform and the clip.
   *
   * Ignores the clip because clearing is what a caller does at the start of a frame, before any clip has been
   * established, and a clear that honoured a leftover clip would leave the parts outside it holding the previous
   * frame -- the classic double-buffering artefact.
   */
  void clear(const Color& color);

  /** Fills the clipped device area with @p color, honouring the clip but not the transform. */
  void drawColor(const Color& color, BlendMode blendMode = BlendMode::kSrcOver);

  /** Fills @p rect, which is in the current space. */
  void drawRect(const Rect& rect, const Paint& paint);

  /** Outlines @p rect, with the stroke width taken from @p paint. */
  void drawRectStroke(const Rect& rect, const Paint& paint);

  /** Fills or strokes @p path according to @p paint. */
  void drawPath(const Path& path, const Paint& paint);

  // ================================================================================================
  // Shapes, text and pictures: conveniences over `drawPath`, `drawTextBlob` and `Picture::playback`
  // ================================================================================================
  //
  // Every one of these is a **conversion**, not a new drawing operation: a circle becomes a path of four cubics, an
  // arc becomes one cubic per quarter turn, a picture becomes its recorded commands played back into this canvas. The
  // backend therefore does not learn about any of them -- `DrawList` keeps its `Rect`/`Path`/`Image`/`Text`/layer
  // commands, and a path is flattened at *record* time either way -- so these cost nothing at execution and cannot
  // disagree with `drawPath` about the clip, the blend mode, the shader or the stroke join.
  //
  // They exist because the alternative was for every caller to write the same four cubics. That is what the framework's
  // own examples did before these existed, and a sticker or a rounded panel is exactly the case where a caller would
  // otherwise get the control points slightly wrong.

  /**
   * Strokes the segment from @p from to @p to.
   *
   * The stroke width, cap and join come from @p paint, so this needs `Paint::Style::kStroke` to be visible: a **filled**
   * line has no area and draws nothing, which is the same answer `drawPath` gives for a two-point open path.
   */
  void drawLine(Point from, Point to, const Paint& paint);

  /** Fills or strokes the circle at (@p centreX, @p centreY) with @p radius. */
  void drawCircle(Scalar centreX, Scalar centreY, Scalar radius, const Paint& paint);

  /** Fills or strokes the ellipse inscribed in @p bounds. */
  void drawOval(const Rect& bounds, const Paint& paint);

  /**
   * Fills or strokes @p rect, a rounded rectangle from `geometry`.
   *
   * Per-corner radii are honoured, and they may be elliptical: `RoundingRadii` holds a `Size` per corner, and each
   * corner is drawn as a quarter ellipse rather than a quarter circle. Radii larger than the rectangle are scaled down
   * by `RoundRect` itself, so an over-rounded rect draws rather than folding inside out.
   *
   * There is deliberately no `Skia`-style `RRect` type of this library's own: `geometry::RoundRect` and
   * `geometry::RoundingRadii` are the types, and inventing a second one would give callers two ways to say the same
   * thing.
   */
  void drawRoundRect(const RoundRect& rect, const Paint& paint);

  /**
   * Draws an arc of the ellipse inscribed in @p bounds.
   *
   * @param startDegrees where the arc begins, measured from the positive x axis, clockwise in the canvas's y-down space
   *        (so 0 is the right edge, 90 the bottom, 180 the left, 270 the top).
   * @param sweepDegrees how far it runs; negative sweeps counter-clockwise, and a magnitude of 360 or more is a closed
   *        ellipse.
   * @param useCenter true to draw a pie: the arc is closed through the centre of @p bounds. With a stroke paint, false
   *        leaves an open arc whose ends are capped by `Paint::strokeCap`.
   */
  void drawArc(const Rect& bounds, Scalar startDegrees, Scalar sweepDegrees, bool useCenter, const Paint& paint);

  /**
   * Draws @p arc: the same geometry, described by the `geometry` type instead of four loose arguments.
   *
   * `Arc` carries exactly what the numeric overload takes -- the oval it is inscribed in, the start angle, the sweep and
   * whether the centre is included -- so this is a delegation, not a second implementation, and the two cannot disagree
   * about where a 90-degree wedge sits. Worth having because the angles are the part callers get wrong: an `Arc` built
   * once can be measured (`getTightArcBounds`), drawn and reused, while four numbers at a call site are four chances to
   * swap the sweep for the start.
   */
  void drawArc(const Arc& arc, const Paint& paint);

  /**
   * Shapes @p text with @p font and draws it at (@p x, @p y) on the baseline.
   *
   * The convenience over `drawTextBlob`: callers with a string should not have to know that shaping is a separate step.
   * A caller that draws the same string every frame should shape it once and keep the blob -- the blob is immutable, and
   * shaping is the expensive half.
   */
  void drawText(const std::string& text, Scalar x, Scalar y, const Font& font, const Paint& paint);

  /**
   * Plays @p picture's recorded commands into this canvas, under the current transform and clip.
   *
   * @param picture the recording; its commands are replayed rather than copied, so the picture must outlive a replay
   *        (a recording canvas captures the call and replays it later, which is the same requirement `drawTextBlob`
   *        states for its font).
   */
  void drawPicture(const Picture& picture);

  /**
   * Starts/ends a device-space sub-recording for a retained layer.
   *
   * The calls between these two go into `out` rather than the frame's list. The transform and clip in
   * force are baked into the commands, which is why appending the finished list later must use the
   * same state; the UI's per-layer cache compares both. A backend that cannot scope returns an empty
   * list, and `appendRecording` returns false so the caller can fall back to drawing directly.
   */
  void beginRecordingScope();
  void endRecordingScope(gpu::DrawList& out);
  /**
   * Appends @p list to the current recording, shifted by @p translation device pixels.
   *
   * The lists are device-space, so a caller that wants to reuse one at a different position has two
   * options: re-record, or ask for it here. The second is what a scrolled viewport uses -- its content list
   * stays valid while the scroll offset changes, which is the difference between 27 ms and 0.1 ms a frame.
   *
   * A translation with a fractional part is refused (the coverage fields are placed on whole device
   * pixels and cannot carry one), so the caller falls back to drawing.
   *
   * @return false when the backend cannot append, or when the translation is not expressible.
   */
  bool appendRecording(const gpu::DrawList& list, Point translation = {});
  /** @return true while a device-space sub-recording is active. */
  [[nodiscard]] bool inRecordingScope() const;

  /** Concatenates a translation. Equivalent to `concat(Matrix::MakeTranslation({dx, dy, 0}))`. */
  void translate(Scalar dx, Scalar dy);

  /** Rotates clockwise by @p degrees about the current origin. */
  void rotate(Scalar degrees);

  /** Scales both axes by @p scale. */
  void scale(Scalar scale);

  /** Scales each axis separately. */
  void scale(Scalar scaleX, Scalar scaleY);

  /**
   * Composites @p image with its top-left corner at (@p x, @p y) in the current space.
   *
   * Nearest-neighbour sampling only. A scaled draw is approximated rather than filtered: proper resampling
   * needs a filter kernel and a decision about what to do outside the source, and an approximation that looks
   * aliased is recognisable as one, whereas a casually written bilinear filter is subtly wrong in ways that take
   * much longer to notice.
   */
  /**
   * Composites @p image with its top-left corner at (@p x, @p y) in the current space.
   *
   * Nearest-neighbour sampling, which is `drawBitmap`'s historical behaviour and the default of `SamplingOptions`. The
   * overload below takes the sampling explicitly; this one exists so that the twenty-odd existing call sites keep the
   * picture they have.
   */
  void drawBitmap(const Bitmap& image, Scalar x, Scalar y, const Paint& paint);

  /**
   * Composites @p image with the given sampling.
   *
   * The four parameters of `SamplingOptions` are not all honoured here, and the limits are stated rather than
   * approximated:
   *
   *   * `FilterMode::kNearest` and `kLinear` are implemented on the raster path. Linear is a real bilinear filter over
   *     the four neighbouring texels, clamped at the edges, blended on **premultiplied** values -- which is why a
   *     transparent edge does not bleed its colour into the edge pixels.
   *   * `MipmapMode` and the cubic resampler are implemented **on the raster path**. Mipmapping builds only the levels the
   *     draw's level of detail asks for, with a box filter that makes a minified checkerboard converge to its average;
   *     cubic is the Mitchell-Netravali family, honouring `CubicResampler`'s B and C and normalised so that a flat region
   *     stays flat at the edges.
   *   * The **recorded** path (a GPU surface) honours nearest, linear, `MipmapMode` and the full cubic family. A mipmapped
   *     draw gets a texture with the levels box filtered into it and the sampler's mipmap filter chooses between them; a
   *     cubic draw runs a sixteen-tap program whose B and C arrive per image batch, so **any** `CubicResampler` is honoured
   *     there, not just the named members of the family. The cheap path stays a separate program so that a nearest or
   *     bilinear image does not carry the cubic taps' registers. Mip levels are generated once per image rather than per
   *     frame, so an image whose pixels change every frame needs a new placement. A hardware draw may differ from the
   *     rasterizer in the last bit or two; it is not allowed to do something different.
   */
  void drawBitmap(const Bitmap& image, Scalar x, Scalar y, const SamplingOptions& sampling, const Paint& paint);

  /**
   * Composites a sub-rectangle of @p image into @p destination.
   *
   * This is the atlas entry point: @p source is in the image's pixels and @p destination is in the current space.
   * A recording backend keeps the whole image and records the normalized source rectangle on the command, so every
   * glyph from one atlas page shares a texture and the GPU can batch them into one instanced draw. The raster
   * backend samples only the source rectangle.
   */
  void drawBitmapRect(const Bitmap& image, const Rect& source, const Rect& destination, const Paint& paint);
  void drawBitmapRect(const Bitmap& image, const Rect& source, const Rect& destination,
    const SamplingOptions& sampling, const Paint& paint);

  /**
   * Composites @p image -- a texture another producer on the same device owns -- into @p destination.
   *
   * This is the content-embedding entry point: an effect graph, a video decoder or another window's
   * surface renders into a texture, and a page samples it as part of its own drawing. Nothing is
   * copied and no readback happens. The command is recorded at the current point in the frame, so the
   * current transform, clip, layer (group opacity) and blend mode all apply to it exactly as they
   * would to a bitmap draw; the texture is bound at submit time.
   *
   * The **raster** backend draws nothing: it has no device to sample from, and a software canvas cannot
   * represent a borrowed GPU view. A caller that needs the picture on the CPU path uses a producer that
   * reads back into a `Bitmap` and draws that instead.
   *
   * The caller owns the texture and must keep it alive until the frame that names it has finished on
   * the GPU. For a `gpu::Surface` producer that is its next `submit`; for a ring it is the slot's reuse
   * point. See `graphics::ExternalImage`.
   */
  void drawExternalImage(const ExternalImage& image, const Rect& destination,
    const SamplingOptions& sampling, const Paint& paint);

  /** The default-sampling form of `drawExternalImage`. */
  void drawExternalImage(const ExternalImage& image, const Rect& destination, const Paint& paint);

  /**
   * Composites @p image with its top-left corner at (@p x, @p y) in the current space.
   *
   * The entry point for a picture rather than a mutable bitmap, and the one that costs nothing extra: an `Image` is
   * immutable, so this draws through `Image::peekBitmap` -- a read-only view of the pixels the image already owns --
   * instead of the deep copy `makeBitmap` would make. Same sampling and same paint semantics as `drawBitmap`.
   */
  void drawImage(const Image& image, Scalar x, Scalar y, const Paint& paint);

  /** The `SamplingOptions` form of `drawImage`: same limits as `drawBitmap`'s, and the same defaults. */
  void drawImage(const Image& image, Scalar x, Scalar y, const SamplingOptions& sampling, const Paint& paint);

  /**
   * Draws @p blob at (@p x, @p y) using @p font.
   *
   * Each glyph's outline is scaled to the font's size, placed at the pen position on the baseline, and filled. The
   * extraction is the FreeType path described by the font requirements; the CoreText half of that split is still to
   * come, and it is `Typeface::glyphPath` that a platform implementation would replace.
   *
   * The pen advances along the baseline as the glyphs are drawn, in the order the blob holds them: a right-to-left
   * run comes out correctly because shaping has already put the blob in visual order, and reversing it here would
   * undo that.
   *
   * The glyph's own offset is applied, which is what makes a combining mark land above its base letter rather than
   * on the baseline.
   */
  void drawTextBlob(const TextBlob& blob, Scalar x, Scalar y, const Font& font, const Paint& paint);

private:
  friend class PictureRecorder;
  friend class Picture;

  Canvas();

  /**
   * Points this canvas at a recording, or at nothing.
   *
   * The pointer is opaque because the recording's type is a private header's: `Canvas` is installed and
   * `PictureData.h` is not, so naming it here would drag a private type into a public signature. `PictureRecorder` is
   * the only caller and holds the `void*` it passes, which is the same arrangement `Path` uses for its representation.
   */
  void attachRecorder(void* recording);

  /** @return the recording this canvas has been filling, or null when it is not recording. */
  [[nodiscard]] const void* recordedOps() const noexcept;

  /**
   * @return true when @p operation was recorded, meaning the caller must not draw it as well.
   *
   * Taking the closure by const reference and letting the recording copy it is what keeps the call sites short: each
   * one writes what it would do later, and this decides whether it also does it now.
   */
  bool record(const std::function<void(Canvas&)>& operation);

  /**
   * Composites the layer at the top of the state stack onto the canvas underneath it.
   *
   * This is the backend's work now, not the canvas's: the layer's pixels, its parent and its paint all live on the
   * backend's side of the split, and the canvas's half of a layer is the state it pushes and the clip it narrows. The
   * declaration is gone rather than kept as a forwarding call, because a method that only forwards is a second place
   * for the two halves of a layer to disagree about when a composite happens.
   */
  struct Impl;

  std::unique_ptr<Impl> mImpl;
};

} // namespace graphics

#endif //CORAL_GRAPHICS_CANVAS_H
