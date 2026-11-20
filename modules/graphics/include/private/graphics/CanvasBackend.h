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

#ifndef CORAL_GRAPHICS_PRIVATE_CANVAS_BACKEND_H
#define CORAL_GRAPHICS_PRIVATE_CANVAS_BACKEND_H

#include <graphics/Bitmap.h>
#include <graphics/Enums.h>
#include <graphics/ExternalImage.h>
#include <graphics/Paint.h>
#include <graphics/SamplingOptions.h>
#include <graphics/Path.h>

#include <geometry/Color.h>
#include <geometry/Point.h>
#include <geometry/Rect.h>
#include <private/graphics/Flatten.h>

#include <gpu/DrawList.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace graphics {

class Font;

namespace pathops {
/**
 * The internal path representation, forward declared because the backend is handed device-space geometry and nothing
 * more: the definition lives in an internal header, and naming it by reference is all this interface needs. A
 * translation unit that only records draws -- a GPU backend's, say -- therefore does not compile the pathops
 * arithmetic into itself.
 */
class Path;
} // namespace pathops

namespace gpu {
class ContextImpl;
} // namespace gpu

/**
 * What a `Canvas` delegates the *effect* of a drawing call to.
 *
 * ## Why this exists
 *
 * `Canvas` owns two things that have nothing to do with each other: the drawing *contract* -- a stack of
 * transforms and clips, the order a caller's calls arrive in, when a layer opens and closes -- and the
 * machinery that turns one call into pixels. Only the first is what a caller depends on. The second has
 * two implementations today (scanline rasterisation into a `Bitmap`, and a `DrawList` recorded for a GPU
 * pass) and will have more, and the contract is the part worth keeping still while they change.
 *
 * The split is deliberately at the *semantic* level rather than at the pixel level: a backend is told
 * "fill this path with this paint", not "here are fourteen spans". A span-level interface would fix
 * the scanline algorithm into the abstraction and force a GPU backend to implement a loop it has no use
 * for. Geometry is handed over in device space and still as a `pathops::Path`, because taking it to
 * device space is the canvas's job -- it owns the transform stack -- while flattening it is the
 * backend's, and a curve's flattening tolerance is a pixel count either way.
 *
 * ## What the canvas keeps
 *
 * Everything a caller can observe as a decision: the order of the early-outs, the warnings for an
 * unsupported blend mode or a layer that could not be allocated, the recording hook, and the clip
 * *rectangle*. The backend is told the rectangle and owns the clip's shape, so neither has to ask the
 * other what the clip is: the canvas narrows a rectangle, the backend rasterises a mask, and every draw
 * consults both.
 *
 * ## Threading
 *
 * One backend per canvas, one canvas per recording thread. Nothing here is shared and nothing locks. A
 * GPU backend records into a command list rather than calling the driver, exactly as the RHI's own
 * `CommandBuffer` does, which is what lets N threads record at once while only one ever touches the GL
 * context.
 *
 * ## Lifetime
 *
 * The canvas owns its backend through a `unique_ptr`, so the backend's destructor runs when the canvas's
 * does and RAII is the whole story. `gpu::ContextImpl` is forward declared rather than included because
 * the GPU backend is the only thing that names the RHI, and `Canvas.cpp` must not drag it in.
 */
class CanvasBackend {
public:
  virtual ~CanvasBackend() = default;

  CanvasBackend(const CanvasBackend&) = delete;
  CanvasBackend& operator=(const CanvasBackend&) = delete;

protected:
  /**
   * A backend is created by the factory below and exists only as the base of a concrete one.
   *
   * Declared rather than left implicit because declaring the destructor above suppresses the implicit default
   * constructor: without this, no derived class could be constructed at all, and the error names the base rather than
   * the missing default constructor, which is a diagnostic worth paying one line to avoid.
   */
  CanvasBackend() = default;

public:
  // ------------------------------------------------------------------------------------------------
  // Target
  // ------------------------------------------------------------------------------------------------

  /** @return the size of the drawing surface in device pixels. */
  [[nodiscard]] virtual Rect deviceBounds() const noexcept = 0;

  /** @return true when this backend has already drawn, so `readback` is a copy rather than a read. */
  [[nodiscard]] virtual bool isRaster() const noexcept = 0;

  /**
   * @return true when drawing calls are recorded into a command list.
   *
   * A recording backend does not need a GPU context to answer this: the phase-2 UI records its frame
   * on the UI thread with a `gpu::Recorder` that deliberately carries no context, and the list is
   * submitted later on the render thread. `isRecording()` therefore describes the destination (a
   * DrawList), not the presence of a device; a context-backed recording backend and a context-free
   * one both answer true.
   */
  [[nodiscard]] virtual bool isRecording() const noexcept {
    return false;
  }

  /**
   * @return true when a drawing call reaches somewhere: a bitmap, or a recorded command list.
   *
   * This is what the canvas asks before doing per-draw work, and it exists because that question cannot be
   * answered by testing `pixels()` for null. A recording canvas has no pixels *by design*, so a canvas that
   * gated its drawing on the pixel pointer dropped every path and every clip on the GPU path -- silently, since
   * the calls still returned successfully and the frame simply came out missing its shapes. That is precisely
   * the class of bug a separate predicate prevents: the two questions look alike and are not.
   *
   * False only for a canvas whose work has nowhere to go.
   */
  [[nodiscard]] virtual bool hasTarget() const noexcept = 0;

  /**
   * @return the pixels the canvas is drawing into, or null when there are none.
   *
   * This is what `Canvas::target()` hands back, and it is the *current* target rather than the one the canvas was
   * created over: inside a layer it is the layer's pixels, which is what makes `Canvas::maskLayer` -- and any caller
   * reading the target mid-frame -- see the same buffer a draw would reach. The pointer is borrowed and lives as
   * long as the canvas, exactly as the bitmap a raster canvas was created over does.
   *
   * A backend that has no bitmap at all answers null, which is what tells the canvas that a layer cannot be opened
   * here: a GPU backend records into a command list and allocates its pixels at submit time, so there is nothing to
   * hand back and nothing to composite by hand.
   */
  [[nodiscard]] virtual Bitmap* pixels() noexcept { return nullptr; }

  // ------------------------------------------------------------------------------------------------
  // State
  // ------------------------------------------------------------------------------------------------

  /**
   * Reacts to the canvas pushing a state entry.
   *
   * The raster backend pushes a copy of its clip entry and nothing else: the transform is entirely the canvas's
   * business, so a push here is a bookmark for the clip rectangle and the mask the canvas is about to narrow. A
   * GPU backend may want to record a marker, and a backend that kept its own device clip stack would need the
   * notification -- which is why it is a call rather than something only the canvas knows about.
   */
  virtual void onSave() = 0;

  /**
   * Starts the backend's stack at @p deviceBounds, discarding anything left from a previous canvas.
   *
   * Called once, by the factory that creates the canvas, before anything is drawn. It exists as its own call rather
   * than as the first `onSave` because there is a window between a backend being constructed and a canvas being ready
   * to draw in which nothing has described the base clip yet -- and a backend whose stack is empty cannot answer the
   * coverage question every draw begins with.
   */
  virtual void reset(const Rect& deviceBounds) = 0;

  /**
   * Reacts to the canvas popping a state entry, after the pop has happened.
   *
   * @return true when the pop closed a layer, so the canvas knows a composite has happened. The raster
   *         backend composites there and then; the GPU backend records the composite and returns true.
   */
  virtual bool onRestore() = 0;

  /**
   * Tells the backend the canvas's clip rectangle, which it needs for two things: the per-pixel coverage test every
   * draw makes, and the rectangle a layer is composited through.
   *
   * The canvas calls this wherever the rectangle changes -- as it sets up its base state, as it narrows the clip,
   * and as it opens a layer. The backend already knows the clip's *shape*, because `clipPath` handed it the coverage
   * mask, so between them the two hold the whole clip without either having to ask the other for it. A rectangle
   * rather than a whole clip description because the mask is already here: the rectangle is the fast reject, and this
   * is the only half of the clip the canvas is still the authority on.
   */
  virtual void setDeviceClip(const Rect& deviceClip) = 0;

  /**
   * Opens an offscreen layer covering @p deviceBounds.
   *
   * @param deviceBounds the layer's region, already in device space and already intersected with the
   *        parent's clip by the canvas.
   * @param paint only its alpha and its blend mode are read, as `Canvas::saveLayer` documents.
   * @return false when the layer could not be opened, in which case the canvas keeps drawing into its
   *         parent. A refused layer is a wrong picture but a stable one -- the trade the raster backend
   *         already documents for a failed allocation -- and it beats an unbalanced save/restore pair.
   */
  virtual bool beginLayer(const Rect& deviceBounds, const Paint& paint) = 0;

  /**
   * Multiplies the layer currently open by @p mask.
   *
   * @param mask coverage in device space. A mask whose size differs from the layer's is stretched to it,
   *        which is what `Canvas::maskLayer` promises.
   * @param mode whether the mask's alpha or its luminance decides the coverage.
   */
  virtual void maskLayer(const Bitmap& mask, MaskMode mode, bool alignToLayer) = 0;

  /**
   * Starts a device-space sub-recording. Used by the UI's per-layer cache: calls made between this
   * and `endRecordingScope` go into a list that can be appended to the parent later, without
   * re-applying the current transform or clip (they are already baked into the commands).
   */
  virtual void beginRecordingScope() = 0;
  /** Ends the scope and moves its list out. A backend without scopes returns an empty list. */
  virtual void endRecordingScope(gpu::DrawList& out) = 0;
  /** Appends a device-space list into the current layer. Returns false when unsupported. */
  virtual bool appendRecording(const gpu::DrawList& list, Point translation) = 0;
  /** @return true while a recording scope is active, so a cache cannot start a nested one. */
  [[nodiscard]] virtual bool inRecordingScope() const = 0;

  /**
   * Intersects the clip with the coverage of @p devicePath.
   *
   * The raster backend flattens the path and rasterises it into a coverage mask; the GPU backend records it so the
   * pass can upload the mask as a texture. Both end up with the same semantics, which is the point: a path clip under
   * a path clip intersects rather than replacing, and getting that wrong is how a nested clip becomes a union.
   *
   * The path arrives as a @c pathops::Path rather than as flattened contours because flattening is the one step both
   * backends need and neither should be handed twice: the canvas has already taken it to device space, so this is the
   * last point at which the two agree about what the clip *is* and before they disagree about how to rasterise it.
   *
   * @return true when a shaped clip is now in force, which is what `Canvas::hasClipPath` reports. False when the
   *         mask could not be allocated, or when the path has no representation to rasterise: the rectangle that is
   *         already in force is still the clip, and a canvas that claimed a shaped clip it never applied would let a
   *         caller's own `clipPath`-aware code disagree with what is drawn.
   */
  virtual bool clipPath(const pathops::Path& devicePath, FillType fillType) = 0;

  // ------------------------------------------------------------------------------------------------
  // Drawing
  // ------------------------------------------------------------------------------------------------

  /** Fills the whole target, ignoring the transform and the clip, as `Canvas::clear` documents. */
  virtual void clear(const Color& color) = 0;

  /** Floods the current clip, which the canvas has already tested for emptiness. */
  virtual void drawColor(const Color& color, BlendMode blendMode) = 0;

  /**
   * Fills a device-space rectangle.
   *
   * Worth its own entry point rather than being expressed as a four-point path: a rectangle is the most
   * common shape any UI draws -- every background, border and clipping band -- and both backends have a
   * dramatically cheaper path for one.
   */
  virtual void drawRect(const Rect& deviceRect, const Paint& paint) = 0;

  /**
   * Fills or strokes @p devicePath, which the canvas has already taken to device space.
   *
   * @param fillType which rule decides inside, as the path carries it.
   * @param scale how many device pixels one unit of @p devicePath spans, which is what turns the paint's stroke width
   *        into a device-space one: a curve is flattened to a tolerance in *pixels*, and a stroke's outline has to be
   *        flattened to the same one or a stroked curve and a filled one would disagree about where the curve is.
   * @param deviceToUser the inverse of the transform @p devicePath was produced by. It is what a shader is sampled
   *        through -- a gradient is written in the caller's coordinates and knows nothing about a transform, so the
   *        inverse is applied on its behalf -- and it is passed in rather than re-derived because the canvas owns the
   *        transform stack and inverting it twice is how the two ends come to disagree about a sub-pixel mapping.
   */
  virtual void drawPath(const pathops::Path& devicePath, FillType fillType, Scalar scale,
          const Matrix& deviceToUser, const Paint& paint) = 0;

  /**
   * Composites a device-space quad sampling @p image.
   *
   * The corners are in device space in the order top-left, top-right, bottom-right, bottom-left. Four
   * corners rather than a rectangle because that is what carries a rotated or skewed transform: a
   * bounding box would lose the mapping, and the raster backend's inverse lookup is the same information
   * written the other way round.
   *
   * @param deviceBounds the bounding box of the quad, which is what both backends clip against.
   * @param deviceToUser the inverse of the transform the quad was produced by, which is what the raster
   *        backend samples @p image through. Handed over rather than recomputed because the canvas is the
   *        one that owns the transform stack, and inverting a matrix twice is how the two ends come to
   *        disagree about a sub-pixel mapping.
   */
  virtual void drawBitmap(const Bitmap& image, const Point deviceQuad[4], const Rect& deviceBounds,
          const Matrix& deviceToUser, const SamplingOptions& sampling, const Paint& paint) = 0;

  /** Composites a sub-rectangle of @p image (source pixels) into the device-space quad. */
  virtual void drawBitmapRect(const Bitmap& image, const Rect& source, const Point deviceQuad[4],
          const Rect& deviceBounds, const Matrix& deviceToUser, const SamplingOptions& sampling,
          const Paint& paint) = 0;

  /**
   * Composites a texture another producer on the same device owns into the device-space quad.
   *
   * The recording backend appends a `Kind::ExternalImage` command, so the texture is bound at submit
   * time and no pixels cross the CPU. The raster backend has no device to sample from and draws
   * nothing; that is stated rather than silently ignored, because a caller that asks a software
   * canvas for an external image has asked for something the software path cannot represent.
   *
   * @param deviceQuad the four device-space corners, top-left, top-right, bottom-right, bottom-left.
   * @param deviceBounds the quad's bounding box, which is what the clip and batch bounds use.
   */
  virtual void drawExternalImage(const ExternalImage& image, const Point deviceQuad[4],
          const Rect& deviceBounds, const SamplingOptions& sampling, const Paint& paint) = 0;

  /**
   * Draws a glyph run whose outlines the canvas has already placed in device space.
   *
   * @param deviceOutline one glyph's outline, in device space, filled with @p paint. One call per glyph rather than
   *        one for the run, because each glyph carries its own placement and the canvas is the only thing that knows
   *        where the pen had reached by the time it got there.
   */
  virtual void drawGlyphRun(const pathops::Path& deviceOutline, const Paint& paint,
    uint64_t glyphCacheKey = 0) = 0;

  // ------------------------------------------------------------------------------------------------
  // GPU-only operations
  // ------------------------------------------------------------------------------------------------
  //
  // A canvas on a GPU backend has to be *finished*: its calls are recorded and something has to submit
  // them. The raster backend has already drawn by the time a call returns, so these are no-ops there --
  // which is what makes one `Canvas` usable both by a test that wants pixels now and by an application
  // that wants a frame later.

  /** @return the GPU context this backend records into, or null for the raster backend. */
  [[nodiscard]] virtual gpu::ContextImpl* gpuContext() noexcept { return nullptr; }

  /**
   * Hands the recorded work to the context, which precompiles, uploads, renders and presents it.
   *
   * @return false when a GPU backend could not submit. The raster backend reports true, having already
   *         succeeded at record time.
   */
  virtual bool flush() { return true; }

  /** Blocks until everything recorded so far has reached the target. */
  virtual bool finish() { return true; }

  /**
   * @return the target's pixels.
   *
   * A raster backend copies what it already has. A GPU backend has to submit, wait and read back, which
   * is why `Surface::snapshot` documents itself as expensive -- and why every test that compares a GPU
   * render against the rasterizer goes through here.
   */
  [[nodiscard]] virtual Bitmap readback() = 0;
};

/**
 * Creates the backend a canvas draws with.
 *
 * One entry point rather than two, because the two forms differ only in what they are handed and each ignores
 * the other's argument: a raster backend needs the pixels and ignores the context, a recording backend needs
 * the context and has no pixels at all. Encoding that as a null pointer is what keeps the canvas's own two
 * callers -- `Canvas::create` and `Canvas::createRecorderBackend` -- down to one line each, instead of a second
 * factory that would have to repeat the same null-checking.
 *
 * @param bitmap the pixels to draw into, or null for a recording backend. A caller with a context passes null,
 *        because a recording backend has no pixels until submission -- which is exactly what `pixels()` is
 *        documented to answer null for.
 * @param impl   the GPU context to record into, or null for the scanline rasterizer.
 * @param list   the draw list to append to, or null to let the backend make one. A frame with several
 *        recorders needs the surface to own the list -- it could not submit one it cannot reach -- so the
 *        surface passes one; a single-recorder frame does not care and lets the backend make its own.
 * @return the backend, or null when neither form can be created.
 */
[[nodiscard]] std::unique_ptr<CanvasBackend> createCanvasBackend(Bitmap* bitmap,
        const std::shared_ptr<gpu::ContextImpl>& impl, const std::shared_ptr<gpu::DrawList>& list = nullptr);

} // namespace graphics

#endif // CORAL_GRAPHICS_PRIVATE_CANVAS_BACKEND_H
