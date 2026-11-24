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

#include <graphics/Canvas.h>

#include <private/graphics/CanvasBackend.h>
#include <private/graphics/PictureData.h>

#include <graphics/Font.h>
#include <graphics/Image.h>
#include <graphics/Picture.h>
#include <private/graphics/Blend.h>

#include "pathops/pathops.h"

#include <minilog/log.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace graphics {

namespace {

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
 * @return @p rect as a path, for a draw to place.
 *
 * The rectangle is handed on in the space it was given, which is the caller's: whichever draw receives it is the one
 * that takes it to device space, exactly as it would any other path. Converting it here would apply the transform
 * twice, and the symptom would be a rectangle that moved by the square of its own translation.
 */
pathops::Path toPathOpsRect(const Rect& rect) {
  // `pathops::Path` takes the public `Point`, in the space it is given; the conversion to its internal double
  // precision happens inside it. So no widening is needed here, only the four corners.
  pathops::Path path;
  path.moveTo(Point { rect.getLeft(), rect.getTop() });
  path.lineTo(Point { rect.getRight(), rect.getTop() });
  path.lineTo(Point { rect.getRight(), rect.getBottom() });
  path.lineTo(Point { rect.getLeft(), rect.getBottom() });
  path.close();
  return path;
}

} // namespace anonymous

/**
 * The canvas's state: the target, a stack of transform-and-clip pairs, and what a draw needs to say to a backend.
 *
 * ## What is deliberately not here
 *
 * The pixels. Every one of them belongs to the backend, which also owns the flattening, the blend arithmetic and the
 * clip's *shape*. What is left is the contract: the transform stack, the clip rectangle, the order the calls arrive in
 * and the decisions a caller can observe -- the warnings, the refusals and the recording hook. A second backend can
 * replace everything below this comment without a caller noticing, which is the whole reason for the split.
 *
 * The clip *mask* is the one thing that looks like it belongs here and does not. A state is copied on every `save`, a
 * mask is per-device-pixel and large, and the only thing the canvas does with one is answer `hasClipPath` -- so the
 * canvas keeps one bit per state and the backend keeps the coverage. Two places hold a clip, and that is the point:
 * the rectangle is what the canvas narrows, the mask is what the backend rasterises, and neither can silently
 * contradict the other because both are consulted for every pixel.
 *
 * ## The layer stack
 *
 * A layer's pixels, its parent and its paint are the backend's business now, and the state here no longer records
 * them. The canvas's half of a layer is the state it pushes and the clip it narrows, which is what keeps a
 * `save`/`restore` pair inside a layer from looking like a layer of its own -- there is simply nothing layer-shaped
 * left to copy.
 */
struct Canvas::Impl {
  /** One entry of the state stack. */
  struct State {
    Matrix transform;
    /** The clip in device space. A rectangle with no area means nothing is drawable. */
    Rect deviceClip;
  };

  /**
   * The target this canvas was created over, or null for a canvas that records rather than draws.
   *
   * A raw pointer rather than an owned value: the canvas borrows its bitmap, so a frame loop can reuse its pixels
   * without a canvas reallocating them, and `Surface` is what owns them for the common case.
   *
   * The canvas is *not* the authority on where a draw lands -- a layer sends it elsewhere -- which is why the draws
   * ask the backend for the pixels rather than following this pointer. This is here for the decisions that are about
   * the canvas's target rather than a draw's: the size of the drawing surface, and whether there is one at all.
   */
  Bitmap* target = nullptr;

  /** The size of the drawing surface, which a recorder canvas has and a bitmap-backed one derives. */
  Rect deviceBounds;

  /** The stack. The first entry is the base state, so `save` pushes above it. */
  std::vector<State> stack;

  /**
   * Whether each state's clip is shaped, tracked beside the stack rather than inside a state.
   *
   * Inside a state it would be copied by `save`, and the copy would claim the shaped clip its parent holds -- so a
   * caller that pushed a state and clipped to a shape would be told a shaped clip was in force even after popping
   * back above it. One bit per state, pushed and popped with the stack, cannot drift.
   */
  std::vector<bool> shapedClip { false };

  /**
   * The pixels and the arithmetic behind them.
   *
   * Null for a canvas that records rather than draws: a recording remembers calls instead of performing them, and a
   * replay onto a real canvas performs them then. That is also why every drawing method asks `record` first and
   * returns when it answers true -- nothing below this point is ever reached on a recording canvas.
   */
  std::unique_ptr<CanvasBackend> backend;

  /** Returned by `target()` on a recorder so the reference is never null; callers should test `isRecording()`. */
  Bitmap emptyTarget;

  /**
   * The recording this canvas is filling, or null when it is drawing rather than recording.
   *
   * A pointer rather than an owned value because the recording outlives the canvas: `PictureRecorder` owns both and
   * moves the recording into the picture, while the canvas is only a way to fill it.
   *
   * While this is set the canvas draws nothing. It has no target -- see the note on `PictureRecorder` -- so every
   * drawing call records itself and returns, which is also what keeps a recording from needing a surface.
   */
  picture_data::Recording* recording = nullptr;

  [[nodiscard]] State& current() noexcept { return stack.back(); }
  [[nodiscard]] const State& current() const noexcept { return stack.back(); }
};

Canvas::Canvas() : mImpl(std::make_unique<Impl>()) {}

void Canvas::attachRecorder(void* recording) {
  mImpl->recording = static_cast<picture_data::Recording*>(recording);
}

const void* Canvas::recordedOps() const noexcept { return mImpl->recording; }

bool Canvas::record(const picture_data::Operation& operation) {
  if (mImpl->recording == nullptr) return false;

  // Appended and reported. Every drawing method calls this first and returns when it answers true, which is what keeps
  // a recording canvas from having to own a surface: the call is remembered instead of drawn, and nothing downstream
  // ever sees a target that is not there.
  mImpl->recording->add(operation);
  return true;
}

Canvas::~Canvas() = default;

std::unique_ptr<Canvas> Canvas::create(Bitmap& bitmap) {
  if (bitmap.isEmpty() || !bitmap.isWritable()) {
    // Logged as well as refused, because a null from a factory whose only argument looks perfectly good is the
    // kind of thing that takes an afternoon to track down.
    MINILOG_WARN("[graphics/canvas] cannot create a canvas over a {}x{} bitmap (writable={})", bitmap.width(),
            bitmap.height(), bitmap.isWritable());
    return nullptr;
  }

  // `new` rather than `make_unique`, because the constructor is private: the class controls how instances come
  // into being, which is what lets it guarantee the target and the base state are set before anyone sees one.
  std::unique_ptr<Canvas> canvas(new Canvas());
  canvas->mImpl->target = &bitmap;

  // The backend is asked for before the stack is built, so that a backend which cannot be created leaves nothing
  // half-initialised behind. The canvas borrows the bitmap for its whole lifetime, exactly as this factory borrows
  // it, which is why a `Bitmap&` is all the backend is given.
  canvas->mImpl->backend = createCanvasBackend(&bitmap, nullptr);
  if (canvas->mImpl->backend == nullptr) {
    MINILOG_WARN("[graphics/canvas] no backend could be created for a {}x{} canvas", bitmap.width(), bitmap.height());
    return nullptr;
  }

  canvas->mImpl->deviceBounds = Rect::MakeLTRB(0.0f, 0.0f, static_cast<Scalar>(bitmap.width()),
          static_cast<Scalar>(bitmap.height()));

  Impl::State base;
  base.transform = Matrix {};
  base.deviceClip = canvas->mImpl->deviceBounds;
  canvas->mImpl->stack.push_back(base);

  // The base entry of the backend's own stack, which every later push copies. This is the one place the canvas has to
  // tell a backend something the caller did not: the clip a canvas starts with is the whole surface.
  canvas->mImpl->backend->reset(canvas->mImpl->deviceBounds);
  return canvas;
}

std::unique_ptr<Canvas> Canvas::createRecordingBackend(const int width, const int height,
        const std::shared_ptr<gpu::ContextImpl>& context, const std::shared_ptr<gpu::DrawList>& list) {
  // No bitmap to pass, which is the whole reason this entry point exists: a recording backend appends to a
  // command list and the surface allocates the target at submit time, so there are no pixels to lend it. The
  // device bounds are still real -- a caller lays a frame out against them -- so they are what was asked for.
  //
  // The list comes from the caller rather than being made here: a frame with several recorders is submitted by
  // a surface, and a surface that could not reach the list its canvas appended to could not submit it.
  if (list == nullptr) {
    MINILOG_WARN("[graphics/canvas] a recording canvas needs a draw list to record into");
    return nullptr;
  }

  std::unique_ptr<Canvas> canvas(new Canvas());
  canvas->mImpl->backend = createCanvasBackend(nullptr, context, list);
  if (canvas->mImpl->backend == nullptr) {
    // The backend refuses only when there is no list to record into -- which the check above has already
    // caught -- or when the build has no recording backend at all. A canvas whose every call is dropped is
    // worse than a null, so this is a refusal rather than a degradation.
    MINILOG_WARN("[graphics/canvas] no recording backend could be created for a {}x{} canvas", width, height);
    return nullptr;
  }

  canvas->mImpl->deviceBounds = Rect::MakeLTRB(0.0f, 0.0f, static_cast<Scalar>(width),
          static_cast<Scalar>(height));

  Impl::State base;
  base.transform = Matrix {};
  base.deviceClip = canvas->mImpl->deviceBounds;
  canvas->mImpl->stack.push_back(base);

  canvas->mImpl->backend->reset(canvas->mImpl->deviceBounds);
  return canvas;
}

std::unique_ptr<Canvas> Canvas::createRecorderBackend(const int width, const int height,
        const std::shared_ptr<gpu::ContextImpl>& context) {
  return createRecordingBackend(width, height, context, std::make_shared<gpu::DrawList>());
}

bool Canvas::isRecording() const noexcept {
  return mImpl->backend->isRecording();
}

Bitmap& Canvas::target() noexcept {
  Bitmap* pixels = mImpl->backend->pixels();
  return pixels != nullptr ? *pixels : mImpl->emptyTarget;
}

const Bitmap& Canvas::target() const noexcept {
  const Bitmap* pixels = mImpl->backend->pixels();
  return pixels != nullptr ? *pixels : mImpl->emptyTarget;
}

Rect Canvas::deviceBounds() const noexcept { return mImpl->deviceBounds; }

// ==================================================================================================
// State
// ==================================================================================================

void Canvas::save() {
  // The state calls are recorded too. A recording is a sequence of calls and not a sequence of marks, so a save that
  // was not recorded would let a replay's transforms accumulate differently from the original's -- which is a
  // difference that only shows on the third or fourth drawing call.
  if (record([](Canvas& canvas) { canvas.save(); })) return;

  // A copy of the current state, which is what makes the stack a stack: the entry pushed is the state as it is
  // now, so a change made after it is undone by the matching restore.
  mImpl->stack.push_back(mImpl->current());
  mImpl->shapedClip.push_back(mImpl->shapedClip.back());

  // ## A save does not re-open a layer
  //
  // A state inside a layer draws into that layer's pixels, but it is not the state that *owns* them -- and there is
  // nothing layer-shaped left in a state to copy, because the ownership lives in the backend. What a `save` here must
  // not do is tell the backend a layer was opened: that is `beginLayer`, and it is called once, by `saveLayer`. A
  // push that reported a layer would make each inner `restore` composite the whole thing again -- invisible in the
  // alpha (a layer of opaque marks composited twice is still opaque) and doubling every colour that was not opaque,
  // once per nested save.
  mImpl->backend->onSave();
}

void Canvas::restore() {
  if (record([](Canvas& canvas) { canvas.restore(); })) return;

  // The base entry is never popped. Letting it go would leave the canvas with no state at all, and there is no
  // sensible transform or clip to invent -- so an over-restore is ignored, which is what the header promises.
  if (mImpl->stack.size() <= 1) return;

  // A layer has to be composited before its state goes away, because the state is what describes the clip it
  // composites through. The backend pops its own entry as part of this, which is why the order is not optional.
  mImpl->backend->onRestore();

  mImpl->stack.pop_back();
  mImpl->shapedClip.pop_back();
}

void Canvas::saveLayer(const Rect& bounds, const Paint& paint) {
  if (record([bounds, paint](Canvas& canvas) { canvas.saveLayer(bounds, paint); })) return;

  Impl::State next = mImpl->current();

  // ## The clip is narrowed first, and for every backend
  //
  // The bounds are a caller-space rectangle, so they are transformed and taken as a bounding box, exactly as
  // `clipRect` does -- and for the same reason: too loose is safe, too tight silently cuts off what was asked
  // for. This belongs *before* any backend question because the clip is part of the state a layer is drawn
  // *and* composited under, and because a layer that a backend refuses has still left the canvas with the clip
  // it would have drawn under.
  //
  // It used to sit after a `pixels() == nullptr` early-out, which meant a backend without pixels got a layer
  // state with no narrowed clip -- and, worse, that early-out returned before `beginLayer`, so a recording
  // backend never learned a layer had opened and the canvas drew its contents into the parent. The GPU path
  // lost every `saveLayer` silently; that is what the mark on the backend's save stack now records.
  const Point corners[4] = {
    next.transform * Point {bounds.getLeft(), bounds.getTop()},
    next.transform * Point {bounds.getRight(), bounds.getTop()},
    next.transform * Point {bounds.getRight(), bounds.getBottom()},
    next.transform * Point {bounds.getLeft(), bounds.getBottom()},
  };
  Scalar left = corners[0].x;
  Scalar top = corners[0].y;
  Scalar right = corners[0].x;
  Scalar bottom = corners[0].y;
  for (const Point& corner: corners) {
    left = std::min(left, corner.x);
    top = std::min(top, corner.y);
    right = std::max(right, corner.x);
    bottom = std::max(bottom, corner.y);
  }
  next.deviceClip = Rect::MakeLTRB(std::max(next.deviceClip.getLeft(), left),
          std::max(next.deviceClip.getTop(), top), std::min(next.deviceClip.getRight(), right),
          std::min(next.deviceClip.getBottom(), bottom));

  // ## The backend opens the layer before its clip is narrowed
  //
  // The layer's bounds narrow the clip for everything drawn *inside* the layer, but the state a matching
  // `restore` returns to is the clip outside it. Asking the backend to open the layer first is what makes its
  // saved entry hold that outer clip: `beginLayer` intersects the current clip with the bounds itself, so it
  // computes the same region either way, while the entry it pushes is the one `restore` will put back. The
  // ordering used to be the other way round, and then any draw after a layer was clipped to the layer's own
  // bounds - invisible as long as the layer covered the whole target, which is why most of the corpus never
  // noticed it.
  const bool opened = mImpl->backend->beginLayer(bounds, paint);
  mImpl->backend->setDeviceClip(next.deviceClip);

  // The backend decides whether it can open a layer at all. A recording backend opens one by recording an
  // `OpenLayer` command; the rasterizer allocates pixels. Neither is something the canvas can predict, which is
  // why the question is asked rather than assumed from the presence of a target.
  if (!opened) {
    // The backend refused -- an allocation failed, or the layer's region is entirely clipped away. Opening it
    // anyway would draw into the parent and then composite the parent onto itself, which would double every
    // mark; so the state is still pushed, which keeps `restore` balanced and sends the layer's contents to the
    // parent. That is a wrong picture but a stable one.
    //
    // The message names the *device bounds* rather than the target's size, because a recording backend has no
    // target: reading `mImpl->target->width()` here was a null dereference on the GPU path, and the size the
    // caller actually asked for is the more useful number anyway.
    MINILOG_WARN("[graphics/canvas] the backend refused a {}x{} layer; drawing without it",
            static_cast<int>(mImpl->deviceBounds.getRight()), static_cast<int>(mImpl->deviceBounds.getBottom()));
  }

  mImpl->stack.push_back(std::move(next));
  mImpl->shapedClip.push_back(mImpl->shapedClip.back());
}

void Canvas::saveLayerAlpha(const Rect& bounds, const uint8_t alpha) {
  Paint paint;
  // The colour is irrelevant -- only its alpha is read -- but leaving it black would be misleading, so it is set to
  // white with the requested alpha and the layer's colours are multiplied by it alone.
  paint.color = Color::MakeRGBA8(255, 255, 255, alpha);
  saveLayer(bounds, paint);
}

void Canvas::maskLayer(const Bitmap& mask, const MaskMode mode, const bool alignToLayer) {
  // The bitmap is captured by value so the recording holds pixels rather than a reference to a caller's -- the same
  // reasoning every other recorded call uses.
  if (record([mask, mode, alignToLayer](Canvas& canvas) {
        canvas.maskLayer(mask, mode, alignToLayer);
      })) return;

  if (mImpl->backend->pixels() == nullptr) {
    // Not an error worth refusing the whole draw for, but it is a caller who has misunderstood the API: there is no
    // layer here to mask, and doing this to the canvas instead would erase it.
    MINILOG_WARN("[graphics/canvas] maskLayer was called with no layer open; nothing is masked");
    return;
  }
  if (mask.isEmpty()) return;

  mImpl->backend->maskLayer(mask, mode, alignToLayer);
}

void Canvas::clipPath(const Path& path) {
  if (record([path](Canvas& canvas) { canvas.clipPath(path); })) return;
  if (isClipEmpty() || path.isEmpty() || !mImpl->backend->hasTarget()) return;

  const Impl::State& state = mImpl->current();

  // The path is rasterised into a coverage mask in device space, and the mask is intersected with whatever clip is
  // already in force. Coverage rather than a boolean, so a clip with a soft or curved edge antialiases like everything
  // else -- a clip that were hard-edged would show a staircase along a diagonal where the shape inside it is smooth.
  //
  // The transformed path is held in a named local before its handle is taken. `pathOpsHandle` points *into* the `Path`,
  // so asking a temporary for it and letting the temporary die at the end of the statement leaves a pointer to a
  // destroyed path's guts -- which reads as a clip that silently lets nothing through.
  const Path devicePath = path.transformed(state.transform);
  const auto* const devicePathOps = static_cast<const pathops::Path*>(devicePath.pathOpsHandle());
  if (devicePathOps == nullptr) return;

  // The backend answers whether a shaped clip is now in force, and the canvas keeps that bit rather than assuming it:
  // a mask that could not be allocated leaves the rectangle as the clip, and `hasClipPath` has to agree with what is
  // actually drawn.
  mImpl->shapedClip.back() = mImpl->backend->clipPath(*devicePathOps, path.fillType());

  // The rectangle stays as it is -- it is the fast reject, and the mask is the exact answer. Narrowing it to the path's
  // bounds here would be an optimisation that the coverage test already performs.
}

int Canvas::saveCount() const noexcept { return static_cast<int>(mImpl->stack.size()) - 1; }

void Canvas::setTransform(const Matrix& matrix) { mImpl->current().transform = matrix; }

void Canvas::concat(const Matrix& matrix) {
  if (record([matrix](Canvas& canvas) { canvas.concat(matrix); })) return;

  Impl::State& state = mImpl->current();
  // The new transform maps caller space through `matrix` first and then through whatever was already there, so
  // it is the old matrix multiplied by the new one. The other order makes scale-then-translate behave like
  // translate-then-scale, which is the classic transform-order bug.
  state.transform = state.transform * matrix;
}

Matrix Canvas::transform() const noexcept { return mImpl->current().transform; }

Scalar Canvas::transformScale() const noexcept { return scaleOf(mImpl->current().transform); }

void Canvas::clipRect(const Rect& rect) {
  if (record([rect](Canvas& canvas) { canvas.clipRect(rect); })) return;

  Impl::State& state = mImpl->current();

  // The rectangle is transformed to device space and then taken as its bounding box. That is an *over*-estimate
  // whenever the transform rotates, so the clip is looser than the caller asked for -- which is the safe
  // direction: a clip that were too tight would silently cut off geometry the caller expected to see, and
  // nothing downstream could detect it.
  const Point corners[4] = {
    state.transform * Point { rect.getLeft(), rect.getTop() },
    state.transform * Point { rect.getRight(), rect.getTop() },
    state.transform * Point { rect.getRight(), rect.getBottom() },
    state.transform * Point { rect.getLeft(), rect.getBottom() },
  };

  Scalar left = corners[0].x;
  Scalar top = corners[0].y;
  Scalar right = corners[0].x;
  Scalar bottom = corners[0].y;
  for (const Point& corner: corners) {
    left = std::min(left, corner.x);
    top = std::min(top, corner.y);
    right = std::max(right, corner.x);
    bottom = std::max(bottom, corner.y);
  }

  const Rect& existing = state.deviceClip;
  state.deviceClip = Rect::MakeLTRB(std::max(existing.getLeft(), left), std::max(existing.getTop(), top),
          std::min(existing.getRight(), right), std::min(existing.getBottom(), bottom));

  // Handed over rather than recomputed: a backend that derived the clip from the geometry would have to re-derive the
  // bounding box above, and two derivations of a rectangle with a rotation in it is how a clip comes to be a pixel
  // wider on one side than what the caller asked for.
  mImpl->backend->setDeviceClip(state.deviceClip);
}

bool Canvas::hasClipPath() const noexcept { return mImpl->shapedClip.back(); }

bool Canvas::isClipEmpty() const noexcept {
  const Rect& clip = mImpl->current().deviceClip;
  return clip.getRight() <= clip.getLeft() || clip.getBottom() <= clip.getTop();
}

Rect Canvas::deviceClipBounds() const noexcept { return mImpl->current().deviceClip; }

// ==================================================================================================
// Drawing
// ==================================================================================================

void Canvas::clear(const Color& color) {
  if (record([color](Canvas& canvas) { canvas.clear(color); })) return;
  mImpl->backend->clear(color);
}

void Canvas::drawColor(const Color& color, const BlendMode blendMode) {
  if (record([color, blendMode](Canvas& canvas) { canvas.drawColor(color, blendMode); })) return;
  if (isClipEmpty()) return;
  if (!raster::isBlendModeSupported(blendMode)) {
    // Refused rather than drawn as source-over. A caller that asked for multiply and got source-over would see a
    // picture that is plausible and wrong, with no way to tell which of the two it was looking at.
    MINILOG_WARN("[graphics/canvas] blend mode {} is not a mode; the draw is skipped", toString(blendMode));
    return;
  }

  mImpl->backend->drawColor(color, blendMode);
}

void Canvas::drawRect(const Rect& rect, const Paint& paint) {
  // The paint is captured by value: a recording holds the paint as it was when the call was made, so a caller that
  // reuses one paint for a second draw does not retroactively change the first.
  if (record([rect, paint](Canvas& canvas) { canvas.drawRect(rect, paint); })) return;
  Paint filled = paint;
  filled.style = Paint::Style::kFill;

  // Through `drawPath`, and deliberately: the four lines of a rectangle are the geometry a path draw already handles,
  // and going through it is what makes a rectangle obey the same clip test, the same blend-mode refusal and the same
  // shader sampling as any other shape. The backend keeps an explicit `drawRect` entry point because a GPU backend has
  // a cheaper way to draw one -- a quad with no flattening -- and a rectangle is the most common shape any UI draws.
  const pathops::Path rectPath = toPathOpsRect(rect);
  drawPath(Path::fromHandle(&rectPath), filled);
}

void Canvas::drawRectStroke(const Rect& rect, const Paint& paint) {
  if (record([rect, paint](Canvas& canvas) { canvas.drawRectStroke(rect, paint); })) return;
  Paint stroked = paint;
  stroked.style = Paint::Style::kStroke;
  const pathops::Path rectPath = toPathOpsRect(rect);
  drawPath(Path::fromHandle(&rectPath), stroked);
}

// ==================================================================================================
// Shapes, text and pictures: conveniences over `drawPath`, `drawTextBlob` and `Picture::playback`
// ==================================================================================================
//
// Each of these is a *conversion*, never a new drawing operation: a circle becomes a path of four cubics, an arc becomes
// one cubic per quarter turn, a picture becomes its recorded commands replayed into this canvas. So they cannot disagree
// with `drawPath` about the clip, the blend mode, the shader or the stroke join, and the backend learns nothing new --
// which is why adding them needs no `DrawList` command and no backend change.

namespace {

/**
 * The cubic control-point length that makes four Béziers approximate a circle, as a fraction of the radius:
 * `4/3 * tan(pi/8)`, to the precision a `Scalar` carries.
 *
 * Spelled here rather than shared with the other two copies in this library (`LottieGeometry.cpp:48`,
 * `SvgDom.cpp:93`), because those build the geometry of *their own* coordinate dialects -- an SVG arc's control points
 * are in its document's space and a Lottie shape's in its layer's -- and one shared helper would have to know both. The
 * number is the same because the geometry is.
 */
constexpr Scalar kCircleConstant = 0.5522847498f;

/** @return @p degrees in radians. */
Scalar radiansOf(const Scalar degrees) { return degrees * 3.14159265358979323846f / 180.0f; }

/**
 * Appends one elliptical arc from @p startDegrees sweeping @p sweepDegrees around the ellipse inscribed in @p bounds.
 *
 * Split into segments of at most a quarter turn, because the cubic approximation is only accurate over that span: the
 * control length for a segment is `4/3 * tan(sweep/4)`, which is `kCircleConstant` at 90 degrees and grows without bound
 * as the span approaches a half turn. Segmenting is what keeps a 350-degree arc a curve rather than a spiral.
 */
void arcPath(Path& path, const Rect& bounds, const Scalar startDegrees, const Scalar sweepDegrees,
  const bool forceMoveTo) {
  const Scalar left = bounds.getLeft();
  const Scalar top = bounds.getTop();
  const Scalar centreX = (left + bounds.getRight()) * 0.5f;
  const Scalar centreY = (top + bounds.getBottom()) * 0.5f;
  const Scalar radiusX = (bounds.getRight() - left) * 0.5f;
  const Scalar radiusY = (bounds.getBottom() - top) * 0.5f;
  if (radiusX <= 0.0f || radiusY <= 0.0f) return;

  const Scalar sweep = radiansOf(sweepDegrees);
  const int segments = std::max(1, static_cast<int>(std::ceil(std::abs(sweepDegrees) / 90.0f)));
  const Scalar segmentSweep = sweep / static_cast<Scalar>(segments);
  // `4/3 * tan(segment/4)` on each side, which is the circle constant for a quarter turn.
  const Scalar control = 4.0f / 3.0f * std::tan(segmentSweep * 0.25f);

  Scalar angle = radiansOf(startDegrees);
  Point point{ centreX + std::cos(angle) * radiusX, centreY + std::sin(angle) * radiusY };
  if (forceMoveTo) {
    path.moveTo(point);
  } else {
    path.lineTo(point);
  }

  for (int segment = 0; segment < segments; ++segment) {
    const Scalar next = angle + segmentSweep;
    const Point first{ point.x - std::sin(angle) * radiusX * control,
      point.y + std::cos(angle) * radiusY * control };
    const Point end{ centreX + std::cos(next) * radiusX, centreY + std::sin(next) * radiusY };
    const Point second{ end.x + std::sin(next) * radiusX * control,
      end.y - std::cos(next) * radiusY * control };
    path.cubicTo(first, second, end);
    angle = next;
    point = end;
  }
}

/** @return the path of the ellipse inscribed in @p bounds. */
Path ovalPath(const Rect& bounds) {
  Path path;
  arcPath(path, bounds, 0.0f, 360.0f, true);
  path.close();
  return path;
}

/** @return the path of @p rect: one quarter ellipse per corner, per-corner and elliptical radii honoured. */
Path roundRectPath(const RoundRect& rect) {
  const Rect& bounds = rect.getBounds();
  const RoundingRadii& radii = rect.getRadii();

  // `RoundRect` scales radii down when they do not fit, so what it hands back is already drawable: a corner whose radii
  // exceed the rectangle comes back smaller rather than as a self-intersecting path.
  const Scalar left = bounds.getLeft();
  const Scalar top = bounds.getTop();
  const Scalar right = bounds.getRight();
  const Scalar bottom = bounds.getBottom();
  const Size topLeft = radii.top_left;
  const Size topRight = radii.top_right;
  const Size bottomRight = radii.bottom_right;
  const Size bottomLeft = radii.bottom_left;

  Path path;
  path.moveTo(Point{ left + topLeft.width, top });
  path.lineTo(Point{ right - topRight.width, top });
  if (topRight.width > 0.0f || topRight.height > 0.0f) {
    path.cubicTo(Point{ right - topRight.width + topRight.width * kCircleConstant, top },
      Point{ right, top + topRight.height - topRight.height * kCircleConstant },
      Point{ right, top + topRight.height });
  }
  path.lineTo(Point{ right, bottom - bottomRight.height });
  if (bottomRight.width > 0.0f || bottomRight.height > 0.0f) {
    path.cubicTo(Point{ right, bottom - bottomRight.height + bottomRight.height * kCircleConstant },
      Point{ right - bottomRight.width + bottomRight.width * kCircleConstant, bottom },
      Point{ right - bottomRight.width, bottom });
  }
  path.lineTo(Point{ left + bottomLeft.width, bottom });
  if (bottomLeft.width > 0.0f || bottomLeft.height > 0.0f) {
    path.cubicTo(Point{ left + bottomLeft.width - bottomLeft.width * kCircleConstant, bottom },
      Point{ left, bottom - bottomLeft.height + bottomLeft.height * kCircleConstant },
      Point{ left, bottom - bottomLeft.height });
  }
  path.lineTo(Point{ left, top + topLeft.height });
  if (topLeft.width > 0.0f || topLeft.height > 0.0f) {
    path.cubicTo(Point{ left, top + topLeft.height - topLeft.height * kCircleConstant },
      Point{ left + topLeft.width - topLeft.width * kCircleConstant, top },
      Point{ left + topLeft.width, top });
  }
  path.close();
  return path;
}

} // namespace anonymous

void Canvas::drawLine(const Point from, const Point to, const Paint& paint) {
  // A two-point open path, and that is the whole implementation: the path draw already owns the stroke width, the cap,
  // the join, the clip test and the blend-mode refusal, so a line cannot behave differently from a path that looks like
  // one. A *filled* line has no area and draws nothing -- the same answer `drawPath` gives for the same path -- which is
  // why the declaration says a stroke paint is what is needed rather than silently promoting the paint.
  Path path;
  path.moveTo(from);
  path.lineTo(to);
  drawPath(path, paint);
}

void Canvas::drawCircle(const Scalar centreX, const Scalar centreY, const Scalar radius, const Paint& paint) {
  drawOval(Rect::MakeLTRB(centreX - radius, centreY - radius, centreX + radius, centreY + radius), paint);
}

void Canvas::drawOval(const Rect& bounds, const Paint& paint) { drawPath(ovalPath(bounds), paint); }

void Canvas::drawRoundRect(const RoundRect& rect, const Paint& paint) { drawPath(roundRectPath(rect), paint); }

void Canvas::drawArc(const Rect& bounds, const Scalar startDegrees, const Scalar sweepDegrees, const bool useCenter,
  const Paint& paint) {
  // A full turn is the ellipse for either style: the endpoints meet, so `useCenter` has nothing left to add and an open
  // path would leave a stroke seam at the start angle.
  if (std::abs(sweepDegrees) >= 360.0f) {
    drawOval(bounds, paint);
    return;
  }

  Path path;
  arcPath(path, bounds, startDegrees, sweepDegrees, true);
  if (useCenter) {
    path.lineTo(Point{ (bounds.getLeft() + bounds.getRight()) * 0.5f,
      (bounds.getTop() + bounds.getBottom()) * 0.5f });
    path.close();
  }
  drawPath(path, paint);
}

void Canvas::drawArc(const Arc& arc, const Paint& paint) {
  // A delegation and not a second implementation: `Arc` holds the same four things the numeric overload takes, and a
  // separate path builder here would be a second answer to "where does a 90-degree wedge sit" that could drift.
  drawArc(arc.getOvalBounds(), arc.getStart().degrees, arc.getSweep().degrees, arc.includeCenter(), paint);
}

void Canvas::drawText(const std::string& text, const Scalar x, const Scalar y, const Font& font,
  const Paint& paint) {
  if (text.empty()) return;
  drawTextBlob(font.shape(text), x, y, font, paint);
}

void Canvas::drawPicture(const Picture& picture) { picture.playback(this); }

void Canvas::beginRecordingScope() { mImpl->backend->beginRecordingScope(); }

void Canvas::endRecordingScope(gpu::DrawList& out) { mImpl->backend->endRecordingScope(out); }

bool Canvas::appendRecording(const gpu::DrawList& list, const Point translation) {
  return mImpl->backend->appendRecording(list, translation);
}

bool Canvas::inRecordingScope() const { return mImpl->backend->inRecordingScope(); }

void Canvas::translate(const Scalar dx, const Scalar dy) {
  concat(Matrix::MakeTranslation(Vector3{ dx, dy, 0.0f }));
}

void Canvas::rotate(const Scalar degrees) {
  concat(Matrix::MakeRotationZ(Radians(radiansOf(degrees))));
}

// The parameter is not called `scale`: this is an overload set, and a parameter that shadows the name would make the
// two-argument call below resolve to the parameter rather than to the member (MSVC C2064).
void Canvas::scale(const Scalar factor) { Canvas::scale(factor, factor); }

void Canvas::scale(const Scalar scaleX, const Scalar scaleY) {
  concat(Matrix::MakeScale(Vector2{ scaleX, scaleY }));
}

void Canvas::drawPath(const Path& path, const Paint& paint) {
  if (record([path, paint](Canvas& canvas) { canvas.drawPath(path, paint); })) return;
  if (path.isEmpty() || isClipEmpty()) return;

  if (!raster::isBlendModeSupported(paint.blendMode)) {
    // Checked once per draw rather than left for the rasterizer to discover per pixel, and refused rather than
    // approximated: see the note in `drawColor`.
    MINILOG_WARN("[graphics/canvas] blend mode {} is not a mode; the draw is skipped",
            toString(paint.blendMode));
    return;
  }

  const Impl::State& state = mImpl->current();

  // Geometry is taken to device space *before* flattening, so the flattening tolerance is a constant in pixels.
  // Flattening first and scaling after would give a zoomed-in curve too few segments, and it would look faceted
  // while the same curve at 1:1 looked smooth -- the error has to be measured in the units the viewer sees.
  Path devicePath = path.transformed(state.transform);

  const Scalar scale = scaleOf(state.transform);
  const auto* devicePathOps = static_cast<const pathops::Path*>(devicePath.pathOpsHandle());
  if (devicePathOps == nullptr) return;

  // A stroke's outline is a union of overlapping pieces and therefore always nonzero, whatever fill rule the path
  // carries: the rule answers how to fill the path, and the stroke is not that path. Every backend repeats this clamp
  // for a caller that reaches `CanvasBackend::drawPath` directly; doing it here as well means a recorded list already
  // says what it means rather than relying on the renderer to reinterpret it.
  const FillType fillType = paint.style == Paint::Style::kStroke ? FillType::Winding : path.fillType();

  // The transform goes with the contours because a shader is sampled in the space the geometry came *from*: the backend
  // has the device-space shape, and only the canvas knows the matrix that would put a gradient's own coordinates back
  // where the caller wrote them. It is handed over inverted -- device back to caller -- because that is the direction a
  // sampler needs, and taking the inverse here rather than there keeps the one transform stack in one place.
  mImpl->backend->drawPath(*devicePathOps, fillType, scale, state.transform.invert(), paint);
}

void Canvas::drawImage(const Image& image, const Scalar x, const Scalar y, const Paint& paint) {
  // Through `peekBitmap` and not `makeBitmap`: an image is a picture, so the drawing path takes a read-only view of the
  // pixels it already holds rather than a deep copy of them. This is the whole reason the method exists -- the copy in
  // `makeBitmap` is right for a caller about to *modify* pixels and wrong for one about to draw them.
  if (image.isEmpty()) return;
  drawImage(image, x, y, SamplingOptions{}, paint);
}

void Canvas::drawImage(const Image& image, const Scalar x, const Scalar y, const SamplingOptions& sampling,
  const Paint& paint) {
  // Still through `peekBitmap`: the sampling decides how the pixels are read, not whether they are copied.
  if (image.isEmpty()) return;
  drawBitmap(image.peekBitmap(), x, y, sampling, paint);
}

void Canvas::drawBitmap(const Bitmap& image, const Scalar x, const Scalar y, const Paint& paint) {
  drawBitmap(image, x, y, SamplingOptions{}, paint);
}

void Canvas::drawBitmap(const Bitmap& image, const Scalar x, const Scalar y, const SamplingOptions& sampling,
  const Paint& paint) {
  // The sampling is captured by the recording too, so a `Picture` replay samples the way the recording asked. A GPU
  // recording cannot yet -- `DrawList`'s image command has no field for it -- which the declaration says.
  if (record([image, x, y, sampling, paint](Canvas& canvas) { canvas.drawBitmap(image, x, y, sampling, paint); }))
    return;
  if (image.isEmpty() || isClipEmpty()) return;

  const Pixmap source = image.makePixmap();
  if (source.isEmpty()) return;

  const Impl::State& state = mImpl->current();
  if (!state.transform.isInvertible()) {
    // A singular transform collapses the image to nothing, so there is no pixel to sample for any destination.
    // Testing here avoids a division by zero in the inverse below, which would produce NaNs that the bounds
    // checks happen to reject -- correct by accident, which is not the same as correct.
    return;
  }

  // The source's rectangle in caller space, mapped to device space and taken as its bounding box. The box is an
  // over-estimate under rotation, which costs a few rejected samples and never drops a pixel.
  const Point corners[4] = {
    state.transform * Point { x, y },
    state.transform * Point { x + static_cast<Scalar>(image.width()), y },
    state.transform * Point { x + static_cast<Scalar>(image.width()), y + static_cast<Scalar>(image.height()) },
    state.transform * Point { x, y + static_cast<Scalar>(image.height()) },
  };

  // The inverse is taken here, once per draw, and handed over. The backend samples through it for every destination
  // pixel, so it is the same matrix the loop would have derived -- and deriving it in one place rather than two is what
  // keeps a rotated cell reading the same texel either way.
  mImpl->backend->drawBitmap(image, corners, state.deviceClip, state.transform.invert(), sampling, paint);
}

void Canvas::drawBitmapRect(const Bitmap& image, const Rect& source, const Rect& destination,
  const Paint& paint) {
  drawBitmapRect(image, source, destination, SamplingOptions{}, paint);
}

void Canvas::drawBitmapRect(const Bitmap& image, const Rect& source, const Rect& destination,
  const SamplingOptions& sampling, const Paint& paint) {
  if (record([image, source, destination, sampling, paint](Canvas& canvas) {
        canvas.drawBitmapRect(image, source, destination, sampling, paint);
      })) {
    return;
  }
  if (image.isEmpty() || isClipEmpty()) return;
  const float imageRight = static_cast<float>(image.width());
  const float imageBottom = static_cast<float>(image.height());
  const float clippedLeft = std::max(0.0f, source.getLeft());
  const float clippedTop = std::max(0.0f, source.getTop());
  const float clippedRight = std::min(imageRight, source.getRight());
  const float clippedBottom = std::min(imageBottom, source.getBottom());
  if (clippedRight <= clippedLeft || clippedBottom <= clippedTop) return;
  const Rect clipped = Rect::MakeLTRB(clippedLeft, clippedTop, clippedRight, clippedBottom);

  const Impl::State& state = mImpl->current();
  if (!state.transform.isInvertible()) return;
  const Point corners[4] = {
    state.transform * Point { destination.getLeft(), destination.getTop() },
    state.transform * Point { destination.getRight(), destination.getTop() },
    state.transform * Point { destination.getRight(), destination.getBottom() },
    state.transform * Point { destination.getLeft(), destination.getBottom() },
  };
  mImpl->backend->drawBitmapRect(image, clipped, corners, state.deviceClip,
    state.transform.invert(), sampling, paint);
}

void Canvas::drawExternalImage(const ExternalImage& image, const Rect& destination,
  const Paint& paint) {
  drawExternalImage(image, destination, SamplingOptions{}, paint);
}

void Canvas::drawExternalImage(const ExternalImage& image, const Rect& destination,
  const SamplingOptions& sampling, const Paint& paint) {
  // A `Picture` recording is not supported here on purpose: it stores calls to replay later, while an
  // external image is a borrowed GPU view whose lifetime is one frame. Replaying a picture that named
  // it after the producer recycled the texture would sample freed memory, so the call is not recorded
  // and the picture simply does not contain it.
  if (!isRecording() || !image.isValid() || destination.isEmpty() || isClipEmpty()) return;

  const Impl::State& state = mImpl->current();
  // The destination is in the caller's space; the backend wants device-space corners, whichever way
  // the transform rotated or scaled them. Unlike a bitmap draw this needs no inverse, because the
  // renderer samples the texture with fixed (0,0)..(1,1) coordinates.
  const Point corners[4] = {
    state.transform * Point { destination.getLeft(), destination.getTop() },
    state.transform * Point { destination.getRight(), destination.getTop() },
    state.transform * Point { destination.getRight(), destination.getBottom() },
    state.transform * Point { destination.getLeft(), destination.getBottom() },
  };

  // The bounding box is the over-estimate a rotated quad is clipped and batched against. It never
  // drops a pixel; it only makes the batch bounds wider than the picture.
  Rect deviceBounds = Rect::MakeLTRB(corners[0].x, corners[0].y, corners[0].x, corners[0].y);
  for (const Point& corner: corners) {
    deviceBounds = Rect::MakeLTRB(std::min(deviceBounds.getLeft(), corner.x),
      std::min(deviceBounds.getTop(), corner.y), std::max(deviceBounds.getRight(), corner.x),
      std::max(deviceBounds.getBottom(), corner.y));
  }
  mImpl->backend->drawExternalImage(image, corners, deviceBounds, sampling, paint);
}

void Canvas::drawTextBlob(const TextBlob& blob, const Scalar x, const Scalar y, const Font& font,
        const Paint& paint) {
  // The font is captured by address rather than by value, because a `Font` is not copyable: it holds a typeface and a
  // size and is made to be shared through a `shared_ptr`, so a copy would be a second font rather than the same one.
  //
  // The pointer is the caller's, and the recording depends on it outliving the replay. That is a real requirement and
  // it is stated rather than worked around: a caller that records text usually holds its fonts for as long as its
  // scene, and inventing a copy would make the replay draw in a font the caller no longer controls.
  if (record([blob, x, y, &font, paint](Canvas& canvas) { canvas.drawTextBlob(blob, x, y, font, paint); })) return;
  if (blob.isEmpty()) return;

  const auto typeface = font.typeface();
  if (typeface == nullptr) return;

  // ## From glyph outlines to the baseline
  //
  // A glyph outline is in font units with y increasing upwards -- see `Typeface::glyphPath` -- and the caller has
  // asked for it at the font's size, in a space where y increases downwards. Three things have to happen, and they
  // are applied in this order:
  //
  //  1. **Scale** by size / unitsPerEm, turning font units into the units the canvas works in.
  //  2. **Flip y and place on the baseline**, which is the same operation: negate the scale's y term, then
  //     translate by the pen position and the baseline.
  //  3. **The canvas's own transform**, which `drawPath` applies. It is not folded in here, so a caller's matrix
  //     composes correctly rather than being overwritten.
  //
  // The pen advances along the baseline as each glyph is drawn, which is what makes the result follow the shaping: a
  // right-to-left run comes out in the order the blob holds it, not reversed, because the blob is already in visual
  // order.
  const float em = static_cast<float>(typeface->unitsPerEm());
  const Scalar scale = (em > 0.0f) ? static_cast<Scalar>(font.size() / em) : 1.0f;

  // The blob's advances and offsets are in the font's units, like the outlines they accompany, so they are scaled by
  // the same factor. Advancing the pen by the raw value moved it a fraction of a pixel per glyph -- see the note in
  // `Font::shape` -- while the glyph itself was drawn at its full size.
  const Scalar fontToDevice = scale;

  Scalar penX = x;

  for (const Glyph& glyph: blob.glyphs()) {
    // A glyph with no advance -- a combining mark, or a space -- contributes nothing to draw. Skipping it by its
    // advance rather than by testing the outline keeps this cheap: the outline of a space is empty anyway, and
    // asking for it would load a glyph for every space in a paragraph.
    if (glyph.advance.x > 0.0f && glyph.id != 0) {
      const Path outline = typeface->glyphPath(glyph.id);
      if (!outline.isEmpty()) {
        // The glyph's own transform: scale, flip y, then move to where the pen is.
        const Scalar originX = penX + static_cast<Scalar>(glyph.offset.x) * fontToDevice;
        const Matrix place = Matrix::MakeTranslation(Vector3 { originX, y, 0.0f }) *
                             Matrix::MakeScale(Vector2 { scale, -scale });

        // ## Through the backend's glyph entry point, which is what makes the edges smooth
        //
        // This used to be `save`/`concat(place)`/`drawPath`/`restore`, and that is the reason hardware text had hard
        // edges: a glyph reached the GPU backend as a *path*, and that renderer's path coverage is a binary stencil
        // test. Measured on `Hamburgefonstiv` at 28 device pixels, the rasterizer produced 708 partly-covered edge
        // pixels and the hardware path produced **0**.
        //
        // `CanvasBackend::drawGlyphRun` takes the outline **already in device space**, so the placement and the canvas
        // transform are composed here and the result is flattened -- the same two steps `drawPath` performs below, in
        // the same order and for the same reason: geometry goes to device space before flattening so that the
        // tolerance is a constant in pixels.
        //
        // The gradient caveat the old comment recorded no longer applies. It said a folded transform would sample a
        // gradient in the glyph's space rather than the canvas's -- true when a shader is sampled through the
        // geometry. A glyph is now rasterised to a coverage bitmap and drawn as an *image*, and an image carries no
        // shader sampling at all, so there is no frame of reference left for a gradient to disagree about.
        Paint filled = paint;
        filled.style = Paint::Style::kFill;

        const Matrix toDevice = place * mImpl->current().transform;
        const Path deviceOutline = outline.transformed(toDevice);
        const auto* devicePathOps = static_cast<const pathops::Path*>(deviceOutline.pathOpsHandle());
        if (devicePathOps != nullptr) {
          // A stable identity for the glyph at this device scale lets the GPU backend keep its coverage
          // bitmap and skip both the FreeType outline load and the rasterisation on later draws.
          uint64_t glyphKey = 0;
          if (paint.style == Paint::Style::kFill) {
            const Scalar scaleX = std::sqrt(toDevice.m[0] * toDevice.m[0] + toDevice.m[1] * toDevice.m[1]);
            const Scalar scaleY = std::sqrt(toDevice.m[4] * toDevice.m[4] + toDevice.m[5] * toDevice.m[5]);
            if (std::fabs(toDevice.m[1]) < 1e-4f && std::fabs(toDevice.m[4]) < 1e-4f && scaleX > 0.0f &&
                std::fabs(scaleX - scaleY) < 1e-3f * scaleX) {
              const uint64_t scaleQ = static_cast<uint64_t>(std::lround(scaleX * 16.0f));
              glyphKey = reinterpret_cast<uintptr_t>(typeface.get()) * 0x9E3779B97F4A7C15ull;
              glyphKey ^= (static_cast<uint64_t>(glyph.id) << 32) | scaleQ;
              // The cached coverage bitmap bakes the glyph's **subpixel position** into its pixels and its
              // quad is recorded in absolute device space, so a cache entry is only valid for the device
              // position it was rasterised at. Without the position in the key, the second occurrence of
              // a glyph reused the first occurrence's quad and was drawn at the first occurrence's
              // position -- the rest of the run then read as missing letters (a GPU-only text defect:
              // the rasterizer does not cache, so it was never affected). Quantised to 1/64 px, which is
              // finer than any subpixel offset the coverage rasteriser resolves.
              const Point deviceOrigin = toDevice * Point { originX, y };
              const int64_t positionX = static_cast<int64_t>(std::llround(deviceOrigin.x * 64.0f));
              const int64_t positionY = static_cast<int64_t>(std::llround(deviceOrigin.y * 64.0f));
              glyphKey ^= static_cast<uint64_t>(positionX) * 0xC2B2AE3D27D4EB4Full;
              glyphKey ^= static_cast<uint64_t>(positionY) * 0x9E3779B97F4A7C15ull;
            }
          }
          mImpl->backend->drawGlyphRun(*devicePathOps, filled, glyphKey);
        }
      } else {
        // ## A bitmap-only glyph: colour emoji, and the fonts that draw a glyph as a picture
        //
        // `NotoColorEmoji` is `CBDT`/`CBLC`: its glyphs are bitmaps with no outline at all, so the
        // `glyphPath` above is empty for every one of them. The fallback asks the typeface for the
        // pixels instead and records them as an image, which both backends already know how to draw.
        //
        // The placement is the bitmap's own bearings rather than the glyph's outline box: `left` and
        // `top` are already scaled to the font's size by `Typeface::glyphBitmap`, and `top` is
        // measured upwards from the baseline, so the y term is subtracted. Recording the four corners
        // in device space is the same contract `CanvasBackend::drawBitmap` documents for
        // `Canvas::drawBitmap`, and doing it here rather than calling `Canvas::drawBitmap` avoids
        // applying the canvas transform twice.
        const GlyphBitmap bitmap = typeface->glyphBitmap(glyph.id, font.size());
        if (bitmap.isValid()) {
          const Impl::State& state = mImpl->current();
          if (state.transform.isInvertible() && !isClipEmpty()) {
            const Scalar originX = penX + static_cast<Scalar>(glyph.offset.x) * fontToDevice;
            const Scalar left = originX + bitmap.left;
            const Scalar top = y - bitmap.top;
            const Scalar right = left + bitmap.width;
            const Scalar bottom = top + bitmap.height;
            const Point corners[4] = {
              state.transform * Point { left, top },
              state.transform * Point { right, top },
              state.transform * Point { right, bottom },
              state.transform * Point { left, bottom },
            };

            // A colour glyph keeps its own colours: multiplying it by the text colour would turn every
            // emoji into a silhouette, which is not what a caller who drew white text asked for. An
            // alpha mask is the opposite -- it is meant to be tinted -- so only the colour case resets
            // the paint to white-and-alpha.
            Paint glyphPaint = paint;
            glyphPaint.style = Paint::Style::kFill;
            if (bitmap.color) {
              glyphPaint.color = Color(1.0f, 1.0f, 1.0f, paint.color.alpha);
            }

            // Linear rather than the nearest texel: an emoji strike is much larger than the text size
            // (109 px for Noto Color Emoji), so a nearest sample would drop most of the picture.
            //
            // `deviceToUser` is not just the canvas inverse here. `RasterCanvasBackend::drawBitmap`
            // recovers source coordinates by mapping the device pixel back through this matrix and
            // subtracting the mapped top-left corner, so it expects the result to be in *source pixel*
            // units. The glyph quad is already scaled to the font size, while the bitmap is at the
            // strike resolution; without the extra scale the rasterizer samples only the top-left
            // corner of every emoji. The GPU path uses normalized texture coordinates and ignores this
            // matrix, which is why the same glyph looked correct there and sliced on the rasterizer.
            const Scalar sourceScaleX = static_cast<Scalar>(bitmap.pixels.width()) / bitmap.width;
            const Scalar sourceScaleY = static_cast<Scalar>(bitmap.pixels.height()) / bitmap.height;
            const Matrix deviceToSource =
              Matrix::MakeScale(Vector2 { sourceScaleX, sourceScaleY }) * state.transform.invert();
            mImpl->backend->drawBitmap(bitmap.pixels, corners, state.deviceClip, deviceToSource,
                    SamplingOptions(FilterMode::kLinear), glyphPaint);
          }
        }
      }
    }
    penX += static_cast<Scalar>(glyph.advance.x) * fontToDevice;
  }
}

} // namespace graphics
