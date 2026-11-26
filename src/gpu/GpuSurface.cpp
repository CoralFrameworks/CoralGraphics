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

/**
 * @file GpuSurface.cpp
 *
 * The GPU-backed drawing surface: a render target, a canvas over it, and the frame those two make.
 *
 * The whole of the interesting work is in three places and each has a comment that says why it is where it
 * is: `beginFrame` creates the canvas over a list this object owns, `addDrawList` merges a second recording
 * into that list, and `submit` hands the result to the context. Everything else is bookkeeping.
 */

#include <gpu/Surface.h>

#include <gpu/Recorder.h>

#include <graphics/Canvas.h>
#include <graphics/Image.h>
#include <gpu/Types.h>
#include <private/graphics/CanvasBackend.h>
#include <private/gpu/ContextImpl.h>
#include <private/gpu/ListBake.h>

#include "CoverageRecord.h"
#include "ExternalImageRecord.h"

#include <backend/Device.h>

#include <minilog/log.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace graphics {

/**
 * @return the images a recording references, indexed by `CommandState::image`.
 *
 * Declared rather than included: the recording backend lives in its own translation unit and the surface has no
 * business naming its class, only this one function. It is how a frame that merged several recordings still
 * knows which bitmaps the merged commands sample.
 */
[[nodiscard]] const std::vector<Bitmap>& recordedImages(const gpu::DrawList* list);

/**
 * Drops the image table registered against @p list, once the list has been consumed.
 *
 * The registry is keyed by a list's *address*, and a frame allocates a fresh list each time. Without this,
 * every frame leaks its bitmaps and -- worse than the leak -- a later list allocated at a recycled address
 * inherits the previous frame's images, so an image draw resolves to whatever picture happened to occupy that
 * address before. That is a wrong picture with no relationship to the code that produced it, which is the
 * hardest kind of defect to trace.
 */
void releaseRecordedImages(const gpu::DrawList* list);

namespace gpu {

namespace {

/** @return the smallest rectangle containing both, treating a rectangle with no area as absent. */
[[nodiscard]] Rect uniteRects(const Rect& a, const Rect& b) noexcept {
  const bool empty = a.getRight() <= a.getLeft() || a.getBottom() <= a.getTop();
  if (empty) return b;
  return Rect::MakeLTRB(std::min(a.getLeft(), b.getLeft()), std::min(a.getTop(), b.getTop()),
          std::max(a.getRight(), b.getRight()), std::max(a.getBottom(), b.getBottom()));
}

/**
 * Moves every command of @p source into @p destination, re-basing the indices they carry.
 *
 * ## Why a merge is not a concatenation
 *
 * A command does not hold its geometry directly: it holds *indices* into the list's `contours`, `points`,
 * `maskPixels` and `layers` arrays. Appending one list's commands to another's without rewriting those
 * indices would make every contour of the second recording point into the first recording's points -- which
 * produces a picture built from the wrong geometry rather than a crash, and only for the second list of a
 * multi-recorder frame. That is exactly the case this function exists for and exactly the case a single-
 * threaded test would never reach.
 *
 * ## The order the offsets are taken in, which is load-bearing
 *
 * Each offset is read *before* the corresponding array is extended, because after the extension the base is
 * the old size -- which is what the offset has to be. Reading it afterwards would double-count and the first
 * command of every merge would be wrong.
 *
 * Layers are the one array with a wrinkle: index 0 of every list is that list's root, and merging two roots
 * into one means the source's root becomes the destination's root. The source's root entry is therefore
 * dropped and its commands keep index 0, while every *other* layer is appended and its index shifted. A
 * source command inside a layer is remapped to the destination's corresponding layer.
 *
 * `coverageFields` has a second-order version of the same problem, and it is the one a four-thread recording
 * reaches: a coverage field the recorder wrote is a **request** that names its own contour range (see
 * `DrawList::CoverageField`), so the range has to be re-based with the contours rather than left pointing at
 * whatever occupied those indices in the destination. A field a caller built names no contours and is left
 * alone -- adding the base to it would send it to an arbitrary range.
 */
void mergeInto(DrawList& destination, DrawList&& source) {
  const auto contourBase = static_cast<uint32_t>(destination.contours.size());
  const auto pointBase = static_cast<uint32_t>(destination.points.size());
  const auto maskBase = static_cast<uint32_t>(destination.clipMasks.size());
  const auto pixelBase = static_cast<uint32_t>(destination.maskPixels.size());
  const auto fieldBase = static_cast<uint32_t>(destination.coverageFields.size());
  // An image index is per *list*, exactly like a coverage field's, because `registerImage` appends to the list the
  // recorder wrote into and hands back that index (`DrawList::images`). So the base has to be taken here and added
  // below, or the second recording's bitmaps resolve to the first recording's -- a picture made of the wrong bitmaps,
  // with nothing reported anywhere.
  //
  // It was missing until 4.1.41, and the reason it survived is the shape of the coverage: the one test that submits
  // several recordings draws **rectangles**, so no list after the first ever named an image. The image table is
  // concatenated in this same order (`Surface::submit` gathers `frameImages` list by list), which is what makes the
  // re-based index the right one.
  const auto imageBase = static_cast<uint32_t>(destination.images.size());

  // The source's layers after its root, which are the ones that get appended. `layerBase` is where the first
  // of them lands, so a source layer index of `index` becomes `index - 1 + layerBase`.
  const auto layerBase = static_cast<uint32_t>(destination.layers.size());
  const size_t sourcedLayers = source.layers.size();

  for (size_t index = 1; index < sourcedLayers; ++index) {
    gpu::Layer layer = std::move(source.layers[index]);
    // A layer's mask is an image index too, and it has to move with the same base as a command's
    // `state.image`. Without this the layer kept pointing at the source list's image table, so the
    // mask sampled whatever bitmap happened to occupy that slot in the destination -- glyph atlases,
    // most visibly, which is what a rounded clip's mask looked like when it was merged.
    if (layer.maskImage != gpu::Layer::kNoMaskImage) layer.maskImage += imageBase;
    destination.layers.push_back(std::move(layer));
  }

  const auto remapLayer = [layerBase](const uint32_t layer) noexcept -> uint32_t {
    // Index 0 is the root in both lists, so it stays 0; anything else shifts by where the appended layers
    // began, less the source's own root.
    // Index 0 is the root in both lists, so it stays 0. The sentinel DrawList names is for a shaped clip,
    // not for a layer: a layer index is never kNoClipMask.
    return layer == 0 ? 0u : (layer - 1 + layerBase);
  };

  for (DrawList::Command& command: source.commands) {
    if (command.contourCount != 0) command.firstContour += contourBase;
    if (command.clipMask != DrawList::kNoClipMask) command.clipMask += maskBase;
    // A coverage field index is re-based exactly like a contour's, and for the same reason: the merged list owns the
    // fields, so a command that named index 0 in its own recording has to name wherever that field landed here.
    if (command.state.coverageField != DrawList::kNoCoverageField) command.state.coverageField += fieldBase;
    // And an image index like the fields', for the same reason and with the same failure mode.
    if (command.state.image != DrawList::kNoImage) command.state.image += imageBase;
    command.layer = remapLayer(command.layer);
    destination.commands.push_back(std::move(command));
  }

  // The point indices live *inside* the contours, so the contours are re-based as they are appended rather
  // than after -- re-basing them afterwards would need a second pass over the destination's whole array.
  for (DrawList::Contour& contour: source.contours) {
    contour.firstPoint += pointBase;
    destination.contours.push_back(contour);
  }
  destination.points.insert(destination.points.end(), source.points.begin(), source.points.end());

  for (DrawList::ClipMask& mask: source.clipMasks) {
    mask.firstPixel += pixelBase;
    destination.clipMasks.push_back(mask);
  }
  destination.maskPixels.insert(destination.maskPixels.end(), source.maskPixels.begin(), source.maskPixels.end());

  // A coverage field the *recorder* wrote is a request that names its own contour range -- so the range has to be
  // re-based with everything else that indexes `contours`. A field a caller built carries its texels and names no
  // contours, which is why the base is added only when there is a range to move; adding it unconditionally would take a
  // caller's field to an arbitrary range that some other recording happened to occupy.
  for (DrawList::CoverageField& field: source.coverageFields) {
    if (field.contourCount != 0) field.firstContour += contourBase;
    destination.coverageFields.push_back(std::move(field));
  }

  // The images last, and in the same order `Surface::submit` concatenates their tables: the indices re-based above
  // count from `imageBase`, which is where this insert begins.
  destination.images.insert(destination.images.end(), std::make_move_iterator(source.images.begin()),
          std::make_move_iterator(source.images.end()));

  destination.bounds = uniteRects(destination.bounds, source.bounds);
}


} // namespace anonymous

/**
 * The surface's GPU state.
 *
 * The context is a `shared_ptr` rather than a member because a `Recorder` borrows it, and a recorder may
 * outlive the surface in a caller that builds a frame and hands it on. Sharing is the cheap way to make that
 * safe; a raw pointer would make the surface's destruction a precondition of the recorder's.
 */
struct Surface::Impl {
  std::shared_ptr<ContextImpl> context;
  std::vector<RenderTarget> targets;
  uint32_t activeTarget = 0;
  bool hasFrame = false;
  RenderTarget target;
  /** The images the recording references, kept alive for the submission. */
  std::vector<Bitmap> images;
  /** The canvas's own recording, which is list 0 of every frame. */
  std::shared_ptr<DrawList> recording;
  /** Extra recordings handed in by other recorders, in the order they were added. */
  std::vector<DrawList> pending;
  /**
   * Recordings **borrowed** for the next submission, in the order they were added.
   *
   * A pointer rather than a value: the caller keeps the recording, which is the difference `submitRecording` exists to
   * provide. The list must outlive the `submit()` that consumes this entry, and the pointer is dropped there -- which is
   * all this vector holds, so nothing here can outlive the call either.
   */
  std::vector<std::shared_ptr<const DrawList>> borrowed;
  /** Images of the retained canvas recording, kept alive with it. */
  std::vector<Bitmap> retainedImages;
  bool retainedValid = false;
  /** True while a canvas from `beginFrame` is outstanding. */
  bool frameOpen = false;
  /** Command buffers the last `submitLists`/`submit` produced; a plan frame wants this to be 1. */
  uint32_t lastSubmitCount = 0;
  /** The canvas eginFrame handed out, held so its lifetime matches the frame's. */
  std::shared_ptr<graphics::Canvas> canvas;
};

Surface::Surface() : mImpl(std::make_unique<Impl>()) {}

Surface::~Surface() {
  if (mImpl == nullptr || mImpl->context == nullptr) return;

  // Idle first, then release. The reverse order is the same requirement the RHI documents: a driver object
  // freed under a live command buffer is undefined behaviour on every backend, and the GL backend in
  // particular defers its work to the command list.
  // ## The canvas goes first, explicitly rather than by declaration order
  //
  // `Canvas` holds a `shared_ptr` to the context, so the renderer lives as long as any canvas does. This destructor used to
  // rely on the canvas being declared *last* in `Impl` (and therefore destroyed first) -- correct today, silently wrong the
  // moment the member order changes. `WindowSurface` had exactly that bug on the path where a frame was opened and never
  // submitted: the device was deleted, and the renderer's destructor then called `waitIdle` on freed memory. Writing the
  // order out here is what keeps the two classes from drifting apart again.
  mImpl->canvas.reset();
  mImpl->recording.reset();

  mImpl->context->waitIdle();
  for (RenderTarget& slot: mImpl->targets) mImpl->context->destroyRenderTarget(slot);
  mImpl->targets.clear();
  mImpl->target = RenderTarget {};
}

std::unique_ptr<Surface> Surface::make(const SurfaceConfiguration& configuration) {
  if (configuration.width == 0 || configuration.height == 0) {
    MINILOG_WARN("[graphics/gpu/surface] a GPU surface needs a size; {}x{} was asked for", configuration.width,
            configuration.height);
    return nullptr;
  }

  auto surface = std::unique_ptr<Surface>(new Surface());

  surface->mImpl->context = std::make_shared<ContextImpl>();
  const backend::Result initialized =
          surface->mImpl->context->initialize(configuration.device, configuration.preferred, configuration.jobSystem,
                  configuration.diagnosticsCallback, configuration.diagnosticsUserData,
                  configuration.maxFramesInFlight);
  if (!backend::isSuccess(initialized)) {
    MINILOG_WARN("[graphics/gpu/surface] no GPU backend could be brought up ({})", static_cast<int>(initialized));
    return nullptr;
  }

  RenderTargetDescription description;
  description.width = configuration.width;
  description.height = configuration.height;
  description.format = backend::PixelFormat::R8G8B8A8_UNORM;
  description.needsStencil = configuration.stencil;
  description.sampleCount = configuration.sampleCount == 0 ? 1 : configuration.sampleCount;
  // The surface reads its own target back, so it has to be readable; the renderer may also sample it, and
  // asking for both is what makes a snapshot and an in-frame read of the same pixels agree.
  description.readable = true;
  description.sampled = true;
  description.label = configuration.label;

  const uint32_t slotCount = std::max<uint32_t>(1u, surface->mImpl->context->frameSlotCount());
  surface->mImpl->targets.resize(slotCount);
  for (uint32_t slot = 0; slot < slotCount; ++slot) {
    surface->mImpl->context->selectFrameSlot(slot);
    const backend::Result created =
            surface->mImpl->context->createRenderTarget(description, &surface->mImpl->targets[slot]);
    if (!backend::isSuccess(created)) {
      MINILOG_WARN("[graphics/gpu/surface] could not allocate a {}x{} render target ({})", configuration.width,
              configuration.height, static_cast<int>(created));
      return nullptr;
    }
    // The output encoding is a property of every target this surface rotates through, not of slot 0.
    surface->mImpl->targets[slot].pqOutput = configuration.pqOutput;
  }
  surface->mImpl->context->selectFrameSlot(0);
  surface->mImpl->target = surface->mImpl->targets[0];

  surface->mWidth = configuration.width;
  surface->mHeight = configuration.height;
  surface->mImpl->recording = std::make_shared<DrawList>();
  return surface;
}

std::unique_ptr<Surface> Surface::makeOn(backend::Device* device,
    const SurfaceConfiguration& configuration) {
  if (device == nullptr) return nullptr;
  SurfaceConfiguration borrowed = configuration;
  borrowed.device = device;
  return make(borrowed);
}

std::unique_ptr<Surface> Surface::make(const uint32_t width, const uint32_t height) {
  SurfaceConfiguration configuration;
  configuration.width = width;
  configuration.height = height;
  return make(configuration);
}

Rect Surface::bounds() const noexcept {
  return Rect::MakeLTRB(0.0f, 0.0f, static_cast<Scalar>(mWidth), static_cast<Scalar>(mHeight));
}

uint32_t Surface::sampleCount() const noexcept {
  return mImpl != nullptr ? mImpl->target.sampleCount : 1;
}

std::string Surface::describeDevice() const {
  if (mImpl == nullptr || mImpl->context == nullptr) return "no device";
  return mImpl->context->describeDevice();
}

std::string Surface::diagnostics() const {
  if (mImpl == nullptr || mImpl->context == nullptr) return "no device";
  return mImpl->context->diagnostics();
}

graphics::Canvas* Surface::beginFrame() {
  if (mImpl == nullptr || mImpl->context == nullptr) return nullptr;
  if (mImpl->frameOpen) {
    // Refused rather than re-returned, for the same reason `graphics::Surface::getCanvas` refuses: asking
    // twice means the caller has lost track of its own frame, and handing back the same canvas would let it
    // record half of one frame and half of the next with nothing to indicate that it had.
    MINILOG_WARN("[graphics/gpu/surface] beginFrame called twice without a submit in between");
    return nullptr;
  }

  if (mImpl->targets.empty()) return nullptr;
  if (mImpl->retainedValid && mImpl->recording != nullptr) {
    // A new frame supersedes the retained one; its image registry has to go with it.
    releaseRecordedImages(mImpl->recording.get());
    mImpl->retainedValid = false;
    mImpl->retainedImages.clear();
  }
  mImpl->activeTarget = mImpl->hasFrame
          ? (mImpl->activeTarget + 1u) % static_cast<uint32_t>(mImpl->targets.size())
          : 0u;
  mImpl->hasFrame = true;
  mImpl->context->selectFrameSlot(mImpl->activeTarget);
  mImpl->target = mImpl->targets[mImpl->activeTarget];

  // ## Activate this surface's device before the frame
  //
  // A thread has one current context, so a caller that owns two surfaces would otherwise draw on whichever device happened to
  // be created last. This is the same call the window surface makes in its `beginFrame`, and it is what makes two surfaces on
  // one thread usable without the caller having to know about activation at all.
  if (mImpl->context->device() != nullptr && !backend::isSuccess(mImpl->context->device()->makeCurrent())) {
    MINILOG_WARN("[graphics/gpu/surface] the device's context could not be made current");
    return nullptr;
  }

  // A fresh list per frame, rather than clearing the old one: a `DrawList` that is still referenced by a
  // submission in flight must not be reused, and allocating one small list per frame is cheaper than the
  // fence that would otherwise be needed to know the previous one is free.
  mImpl->recording = std::make_shared<DrawList>();
  mImpl->recording->label = "gpu-surface-frame";

  mImpl->canvas = graphics::Canvas::createRecordingBackend(static_cast<int>(mWidth), static_cast<int>(mHeight),
          mImpl->context, mImpl->recording);
  if (mImpl->canvas == nullptr) {
    MINILOG_WARN("[graphics/gpu/surface] no recording backend could be created; the GPU canvas is unavailable");
    return nullptr;
  }

  mImpl->frameOpen = true;
  return mImpl->canvas.get();
}

bool Surface::addDrawList(DrawList&& list) {
  if (mImpl == nullptr || mImpl->context == nullptr) return false;
  mImpl->pending.push_back(std::move(list));
  return true;
}

bool Surface::submitRecording(const DrawList& list) {
  if (mImpl == nullptr || mImpl->context == nullptr) return false;
  // A caller that keeps the list alive itself: the wrapper is non-owning, so nothing here extends its lifetime and the
  // requirement stays the caller's. The other overload is the one that removes the requirement -- see the header.
  mImpl->borrowed.push_back(std::shared_ptr<const DrawList>(&list, [](const DrawList*) {}));
  return true;
}

bool Surface::submitRecording(const std::shared_ptr<const DrawList>& list) {
  if (mImpl == nullptr || mImpl->context == nullptr) return false;
  if (list == nullptr) return false;
  mImpl->borrowed.push_back(list);
  return true;
}

bool Surface::submitLists(const std::vector<ListItem>& items) {
  if (mImpl == nullptr || mImpl->context == nullptr) return false;
  if (items.empty()) return submit();

  // Exactly one plain borrowed list: the retained path already submits that without a copy.
  const bool singlePlain = items.size() == 1 && items.front().list != nullptr &&
    items.front().opacity == 1.0f && items.front().clip.isEmpty() &&
    items.front().translation.x == 0.0f && items.front().translation.y == 0.0f &&
    mImpl->recording != nullptr &&
    mImpl->recording->isEmpty() && mImpl->pending.empty() && mImpl->borrowed.empty();
  if (singlePlain) {
    if (!submitRecording(items.front().list)) return false;
    return submit();
  }

  // The sequential path: the canvas recording (the content) goes first and clears the target, then each
  // borrowed list is submitted with `Load`, so the picture is what drawing them in order into one canvas
  // would produce -- with no copy of any list. A list that asks for a composite state this path cannot
  // honour is refused rather than drawn wrong.
  for (const ListItem& item: items) {
    if (item.list == nullptr) continue;
    // A clip is the submission's scissor and a translation rides the canvas block; a per-list opacity
    // would need an offscreen composite, so only that is refused.
    if (item.opacity != 1.0f) return false;
  }
  mImpl->canvas.reset();
  mImpl->frameOpen = false;
  mImpl->context->selectFrameSlot(mImpl->activeTarget);

  // ## Everything recorded before this call is content, in the order it was recorded
  //
  // `submitLists` is the frame's last step: the canvas recording, the lists a recorder added and the
  // lists a caller lent (`submitRecording`) all come before the items, and all of them are submitted with
  // the target cleared by the first. Keeping them here rather than in the caller is what lets a render
  // thread take a packet that has both.
  std::vector<std::shared_ptr<const DrawList>> leading;
  if (mImpl->recording != nullptr && !mImpl->recording->isEmpty()) {
    leading.push_back(mImpl->recording);
  }
  for (DrawList& list: mImpl->pending) {
    if (!list.isEmpty()) leading.push_back(std::make_shared<const DrawList>(std::move(list)));
  }
  for (const std::shared_ptr<const DrawList>& list: mImpl->borrowed) {
    if (list != nullptr && !list->isEmpty()) leading.push_back(list);
  }
  mImpl->pending.clear();
  mImpl->borrowed.clear();

  int submitCalls = 0;
  // A frame is opened by the first submission and closed by the one that passes `continueFrame` false,
  // so counting closes counts command buffers -- which is the cost a plan frame minimizes.
  int frameCount = 0;
  bool submittedAny = false;

  // ## Consecutive lists share one command buffer, including translated ones
  //
  // Every `Context::submit` opens its own GPU frame with its own synchronisation, so a plan-built frame
  // used to pay one frame per lent list -- the viewport's band, the scrollbar, every panel -- and that
  // fixed cost, not the drawing, was what kept present above a millisecond. A frame's canvas block
  // carries one translation, so a list that needs a different one used to open a frame of its own; it is
  // moved into the frame's space instead (`bakeListState`), which keeps paint order and costs one
  // pass over its commands rather than a render pass. A list that cannot be moved -- a fractional
  // translation, a shaped clip mask, an external image -- still opens its own frame rather than drawing
  // at the wrong place.
  //
  // The lists recorded before this call (the content) are the first entries of the same run rather than
  // submissions of their own: a content list that closed its frame would force every lent list after it
  // into a frame of its own, which is exactly the cost this run exists to remove.
  std::vector<std::shared_ptr<const DrawList>> run(leading.begin(), leading.end());

  const auto flushRun = [&]() -> backend::Result {
    backend::Result outcome = backend::Result::Success;

    for (std::size_t index = 0; index < run.size(); ++index) {
      const std::shared_ptr<const DrawList>& entry = run[index];
      const bool last = index + 1 == run.size();

      ++submitCalls;
      if (last) ++frameCount;
      outcome = mImpl->context->submit(*entry, mImpl->target, recordedImages(entry.get()),
        submittedAny ? backend::LoadOp::Load : backend::LoadOp::Clear,
        backend::SemaphoreHandle::invalid(), backend::SemaphoreHandle::invalid(), Rect{}, Point{},
        !last);

      if (!backend::isSuccess(outcome)) break;
      submittedAny = true;
    }

    run.clear();
    return outcome;
  };
  for (const ListItem& item: items) {
    if (item.list == nullptr || item.list->isEmpty()) continue;
    std::shared_ptr<const DrawList> list = item.list;
    Point translation = item.translation;
    bool shareable = true;
    if (!item.clip.isEmpty() || translation.x != 0.0f || translation.y != 0.0f) {
      // A translation and a clip are frame state in the old shape; baking them into the list is what
      // lets the whole frame stay one command buffer. A list that cannot be baked keeps a frame.
      if (const std::shared_ptr<const DrawList> baked = bakeListState(*list, item.clip, translation)) {
        list = baked;
        translation = Point{};
      } else {
        shareable = false;
      }
    }
    if (shareable) {
      run.push_back(std::move(list));
      continue;
    }
    const backend::Result flushed = flushRun();
    if (!backend::isSuccess(flushed)) {
      MINILOG_WARN("[graphics/gpu/surface] a frame list was refused ({})", static_cast<int>(flushed));
      return false;
    }
    ++frameCount;
    const backend::Result submitted = mImpl->context->submit(*list, mImpl->target,
      recordedImages(list.get()),
      submittedAny ? backend::LoadOp::Load : backend::LoadOp::Clear,
      backend::SemaphoreHandle::invalid(), backend::SemaphoreHandle::invalid(), item.clip,
      translation);
    if (!backend::isSuccess(submitted)) {
      MINILOG_WARN("[graphics/gpu/surface] a frame list was refused ({}); see the device diagnostics",
              static_cast<int>(submitted));
      return false;
    }
    submittedAny = true;
  }
  const backend::Result flushed = flushRun();
  if (!backend::isSuccess(flushed)) {
    MINILOG_WARN("[graphics/gpu/surface] a frame list was refused ({})", static_cast<int>(flushed));
    return false;
  }
  mImpl->lastSubmitCount = static_cast<uint32_t>(frameCount);

  // The frame's own recording is finished with; the next `beginFrame` starts a new one.
  if (mImpl->recording != nullptr) releaseRecordedImages(mImpl->recording.get());
  mImpl->recording = std::make_shared<DrawList>();
  mImpl->pending.clear();
  mImpl->borrowed.clear();
  mImpl->retainedValid = false;
  mImpl->retainedImages.clear();
  return submittedAny;
}

bool Surface::drawCoverageField(const DrawList::CoverageField& field, const Rect& deviceBounds,
        const Paint& paint) {
  // ## The field is appended to the frame's own recording, and the renderer draws it at replay
  //
  // The geometry is laid out during precompile, which is why a draw cannot be issued from here: by the time a `Frame`
  // exists its vertex arena has been uploaded, so a command appended then names a buffer nothing was written into. So
  // this writes the field and the command that names it into the list the surface owns, and `submit` does the rest like
  // any other recorded command -- `GpuRenderer::drawCoverageFieldCommand` is what the replay calls for it.
  if (mImpl == nullptr || mImpl->recording == nullptr) {
    return false;
  }
  if (field.isEmpty()) {
    return false;
  }
  // A caller-supplied field must already *carry* coverage: the renderer generates a field from the contours of the
  // command that names it, and this entry point names no contours. An entry with no texels is a request, and a request
  // from here would be generated from nothing -- drawn as a transparent rectangle, which is a wrong picture that looks
  // deliberate. So it is refused, and a caller with geometry to rasterise draws a path instead.
  if (!field.hasCoverage()) {
    return false;
  }

  // ## The clip rectangle, which is not optional
  //
  // `Command::clipRect` defaults to a *zero* rect, and the replay turns it into a scissor. A command that never sets it
  // therefore clips every fragment away and draws nothing -- with no error anywhere, because an empty scissor is a
  // perfectly legal state. That is exactly how this route failed: the draw was issued, the geometry was correct, the
  // pipeline existed, and the target stayed transparent.
  //
  // The recorder fills this in from the canvas's own clip (`mDeviceClip`), which is why a path never has to think about
  // it. There is no canvas here to ask and this call has no caller-supplied clip, so the whole target is the correct
  // answer: the surface's own extent is the largest region anything may draw into.
  DrawList& list = *mImpl->recording;
  const Rect targetExtent = Rect::MakeLTRB(0.0f, 0.0f, static_cast<Scalar>(mImpl->target.width),
          static_cast<Scalar>(mImpl->target.height));
  gpu::appendCoverageFieldCommand(list, field, deviceBounds, targetExtent, DrawList::kNoClipMask, 0, paint);
  list.bounds = uniteRects(list.bounds, deviceBounds);
  return true;
}

bool Surface::drawExternalImage(const DrawList::ExternalImage& image, const Rect& deviceBounds,
        const Paint& paint) {
  // ## A texture another producer owns, drawn by the frame's own recording
  //
  // The command is appended to the recording `beginFrame` opened, so the replay sees it exactly where
  // the caller put it among the canvas's draws -- which is what makes "background, content, chrome" a
  // matter of call order rather than of a second pass. The view is borrowed: `Kind::ExternalImage`'s
  // documentation states the lifetime the caller promises.
  if (mImpl == nullptr || mImpl->recording == nullptr) {
    return false;
  }
  if (!image.isValid()) {
    return false;
  }
  if (deviceBounds.getWidth() <= 0.0f || deviceBounds.getHeight() <= 0.0f) {
    return false;
  }
  DrawList& list = *mImpl->recording;
  // There is no canvas here to ask for a clip, and `Command::clipRect` defaults to a zero rect that
  // the replay turns into a zero-sized scissor -- a draw that reports success and paints nothing. The
  // surface's own extent is the correct clip for this entry point.
  const Rect targetExtent = Rect::MakeLTRB(0.0f, 0.0f, static_cast<Scalar>(mImpl->target.width),
          static_cast<Scalar>(mImpl->target.height));
  gpu::appendExternalImageCommand(list, image, deviceBounds, targetExtent, DrawList::kNoClipMask, 0,
          paint);
  list.bounds = uniteRects(list.bounds, deviceBounds);
  return true;
}

bool Surface::setSdfPattern(const graphics::Bitmap& pattern) {
  return mImpl != nullptr && mImpl->context != nullptr &&
         mImpl->context->setSdfPattern(pattern);
}

bool Surface::submit() {
  if (mImpl == nullptr || mImpl->context == nullptr) {
    return false;
  }
  mImpl->lastSubmitCount = 1;

  // The frame ends here whether or not a canvas was taken: a caller that drew nothing still gets a cleared
  // surface, which is what a caller expecting a transparent frame wants rather than a stale one.
  mImpl->canvas.reset();
  mImpl->frameOpen = false;
  mImpl->context->selectFrameSlot(mImpl->activeTarget);

  // ## A frame that is one borrowed recording is submitted *as it is*
  //
  // The retained path: a caller that keeps a display list hands the same list every frame. There is nothing to merge --
  // the surface's own recording is empty and no other recorder is involved -- so the list goes to the renderer as it
  // stands, with no copy at all. Measured on the demo scene, cloning that list instead costs 3.6-4.3 ms a frame against
  // the ~130 ms of recording it replaces (`路径边缘抗锯齿设计与现状.md` 4.1.40/4.1.41); this path removes even that.
  if (!mImpl->borrowed.empty() && mImpl->recording->isEmpty() && mImpl->pending.empty() &&
      mImpl->borrowed.size() == 1) {
    // The reference is held in a local **across the submit**, and that is load-bearing with the owning overload: clearing the
    // vector would drop the last `shared_ptr`, so a caller that released its own list -- the case that overload exists for --
    // would have its recording freed a line before the renderer reads it. The test that drops the caller's reference before
    // submitting is what found this.
    const std::shared_ptr<const DrawList> held = mImpl->borrowed.front();
    const DrawList& list = *held;
    mImpl->borrowed.clear();
    const backend::Result submitted = mImpl->context->submit(list, mImpl->target, recordedImages(held.get()));
    if (!backend::isSuccess(submitted)) {
      MINILOG_WARN("[graphics/gpu/surface] the frame was refused ({}); see the device diagnostics",
              static_cast<int>(submitted));
      return false;
    }
    return true;
  }

  // ## A frame that is only the canvas recording is retained, not moved
  //
  // The window's static path replays this list on later frames. Moving it into `merged` below would
  // destroy the recording the retained path needs, and cloning it costs more than the recording it
  // would save. The image registry stays registered against the recording for the same reason.
  if (mImpl->borrowed.empty() && mImpl->pending.empty() && mImpl->recording != nullptr &&
      !mImpl->recording->isEmpty()) {
    mImpl->retainedImages = recordedImages(mImpl->recording.get());
    mImpl->retainedValid = true;
    const backend::Result submitted = mImpl->context->submit(*mImpl->recording, mImpl->target,
      mImpl->retainedImages);
    if (!backend::isSuccess(submitted)) {
      MINILOG_WARN("[graphics/gpu/surface] the frame was refused ({}); see the device diagnostics",
              static_cast<int>(submitted));
      mImpl->retainedValid = false;
      return false;
    }
    return true;
  }

  // ## The images are gathered before the merge, and that order is load-bearing
  //
  // A recording's bitmaps are registered under the address of the *list it recorded into*, so they have to be
  // collected from the source lists rather than from the merged one -- the merged list is a new object and
  // nothing is registered against it. Gathering after the merge would find nothing and every image draw would
  // be skipped, which is a picture with the photographs missing rather than an error.
  //
  // The table is passed alongside the list rather than embedded in it because a `DrawList` owns no pixels, and
  // that is deliberate: it is what would let a list be replayed by another backend or written to a file. See
  // the note on `ContextImpl::submit`.
  //
  // The order here and the order `mergeInto` re-bases image indices against are the same order, and that is what makes
  // a list's own image index still name its own bitmap once it has been merged into another list.
  std::vector<Bitmap> frameImages;
  {
    const std::vector<Bitmap>& initial = recordedImages(mImpl->recording.get());
    frameImages.insert(frameImages.end(), initial.begin(), initial.end());
  }
  for (const DrawList& list: mImpl->pending) {
    const std::vector<Bitmap>& images = recordedImages(&list);
    frameImages.insert(frameImages.end(), images.begin(), images.end());
  }
  for (const std::shared_ptr<const DrawList>& list: mImpl->borrowed) {
    const std::vector<Bitmap>& images = recordedImages(list.get());
    frameImages.insert(frameImages.end(), images.begin(), images.end());
  }
  // The canvas's own recording first, then every recorder's in the order they were added. Paint order is the
  // caller's, and a merge that reordered would be a compositor that draws its background last.
  //
  // A borrowed list is the one entry that cannot be moved into the merge -- the caller still owns it -- so it is
  // cloned. That is the price of mixing a retained list with other recordings, and it is why the single-list case
  // above is a path of its own rather than a special case of this one.
  mImpl->retainedValid = false;
  mImpl->retainedImages.clear();
  DrawList merged = std::move(*mImpl->recording);
  for (DrawList& list: mImpl->pending) {
    mergeInto(merged, std::move(list));
  }
  for (const std::shared_ptr<const DrawList>& list: mImpl->borrowed) {
    mergeInto(merged, list->clone());
  }
  mImpl->pending.clear();
  mImpl->borrowed.clear();

  const backend::Result submitted = mImpl->context->submit(merged, mImpl->target, frameImages);

  // The lists are finished with, so the image tables registered against them are dropped. Doing this *after*
  // the submission is what keeps the bitmaps alive for the whole of it, and doing it at all is what stops a
  // later frame's list -- which may be allocated at a recycled address -- from inheriting this frame's images.
  releaseRecordedImages(mImpl->recording.get());
  for (const DrawList& list: mImpl->pending) {
    releaseRecordedImages(&list);
  }

  if (!backend::isSuccess(submitted)) {
    MINILOG_WARN("[graphics/gpu/surface] the frame was refused ({}); see the device diagnostics",
            static_cast<int>(submitted));
    return false;
  }
  return true;
}

bool Surface::submitDamage(const Rect& damage) {
  if (mImpl == nullptr || mImpl->context == nullptr || mImpl->recording == nullptr ||
      damage.getWidth() <= 0.0f || damage.getHeight() <= 0.0f) {
    return false;
  }
  mImpl->canvas.reset();
  mImpl->frameOpen = false;
  mImpl->retainedValid = false;
  mImpl->retainedImages.clear();
  const std::vector<Bitmap>& images = recordedImages(mImpl->recording.get());
  const backend::Result submitted = mImpl->context->submit(*mImpl->recording, mImpl->target, images,
    backend::LoadOp::Load, backend::SemaphoreHandle::invalid(), backend::SemaphoreHandle::invalid(),
    damage);
  return backend::isSuccess(submitted);
}

bool Surface::submitRetained() {
  if (mImpl == nullptr || mImpl->context == nullptr || !mImpl->retainedValid ||
      mImpl->recording == nullptr) {
    return false;
  }
  const backend::Result submitted = mImpl->context->submit(*mImpl->recording, mImpl->target,
    mImpl->retainedImages);
  if (!backend::isSuccess(submitted)) {
    MINILOG_WARN("[graphics/gpu/surface] the retained frame was refused ({}); see the diagnostics",
            static_cast<int>(submitted));
    return false;
  }
  return true;
}

bool Surface::hasRetainedRecording() const noexcept {
  return mImpl != nullptr && mImpl->retainedValid && mImpl->recording != nullptr &&
         !mImpl->recording->isEmpty();
}

Bitmap Surface::snapshot() {
  if (mImpl == nullptr || mImpl->context == nullptr) {
    return Bitmap {};
  }

  // A snapshot with a frame still open submits it first, because a read has to see a complete frame rather
  // than a partial one -- and a caller that asked for a picture of what it drew means what it drew so far.
  if (mImpl->frameOpen) {
    submit();
  }

  Bitmap pixels;
  mImpl->context->selectFrameSlot(mImpl->activeTarget);
  if (const auto read = mImpl->context->readback(mImpl->target, &pixels); !backend::isSuccess(read)) {
    MINILOG_WARN("[graphics/gpu/surface] the readback failed ({})", static_cast<int>(read));
    return Bitmap {};
  }
  return pixels;
}

uint32_t Surface::lastFrameBatchCount() const noexcept {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->lastFrameStats().batchCount : 0;
}

uint32_t Surface::lastFrameDrawCallCount() const noexcept {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->lastFrameStats().drawCallCount : 0;
}

uint32_t Surface::lastFrameSubmitCount() const noexcept {
  return mImpl != nullptr ? mImpl->lastSubmitCount : 0;
}

uint32_t Surface::lastFrameCoverageDrawCallCount() const noexcept {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->lastFrameStats().coverageDrawCalls : 0;
}

uint32_t Surface::lastFrameImageDrawCallCount() const noexcept {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->lastFrameStats().imageDrawCalls : 0;
}

uint32_t Surface::lastFrameImageBatchCount() const noexcept {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->lastFrameStats().imageBatchCount : 0;
}

uint32_t Surface::lastFramePathDrawCallCount() const noexcept {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->lastFrameStats().pathDrawCalls : 0;
}

uint64_t Surface::textureUploadCount() const noexcept {
  // Asked of the *renderer*, not of the last frame's statistics. `FrameStats` describes one frame and is reset at the
  // start of each, so reading the counter from there gave zero on every frame after the first -- which is how this
  // accessor first worked, and the test that reads it caught the difference immediately.
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->textureUploadCount() : 0;
}

uint32_t Surface::lastFramePipelineCount() const noexcept {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->lastFrameStats().pipelineCount : 0;
}

uint32_t Surface::imageAtlasPageCount() const noexcept {
  // Asked of the context, which asks the renderer, for the reason `textureUploadCount` gives: the pages belong to the
  // renderer and outlive every frame, so reading them from the last frame's statistics would report zero.
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->imageAtlasPageCount() : 0;
}

FrameStages Surface::lastFrameStages() const noexcept {
  // A copy rather than a reference into the context, deliberately: the stages describe one frame, and a caller that kept
  // a reference would see it change under it at the next `submit`. Nine numbers is not a copy worth avoiding.
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->lastFrameStages() : FrameStages{};
}

uint64_t Surface::coverageFieldByteCacheHits() const noexcept {
  if (mImpl == nullptr || mImpl->context == nullptr) return 0;
  return mImpl->context->fieldByteCacheHits();
}
uint32_t Surface::lastFrameCoverageFieldsReused() const noexcept {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->lastFrameStats().coverageFieldsReused : 0;
}

uint32_t Surface::lastFrameFieldReuseWindowHits() const noexcept {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->lastFrameStats().fieldReuseWindowHits : 0;
}

uint32_t Surface::lastFrameFieldReuseWindowRequests() const noexcept {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->lastFrameStats().fieldReuseWindowRequests : 0;
}

uint32_t Surface::lastFrameFieldReuseWindowDepth() const noexcept {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->lastFrameStats().fieldReuseWindowDepth : 0;
}

double Surface::lastFramePrecompileMilliseconds() const noexcept {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->lastFrameStats().precompileMilliseconds : 0.0;
}

double Surface::lastFrameRenderMilliseconds() const noexcept {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->lastFrameStats().renderMilliseconds : 0.0;
}

double Surface::lastFrameGpuMilliseconds() const noexcept {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->lastFrameGpuMilliseconds() : 0.0;
}

uint64_t Surface::residentBytes() const noexcept {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->residentBytes() : 0;
}

uint64_t Surface::lastFrameUploadBytes() const noexcept {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->lastFrameStats().uploadBytes : 0;
}

double Surface::lastFrameCoverageFieldMilliseconds() const noexcept {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->lastFrameStats().coverageFieldMilliseconds
                                                       : 0.0;
}

double Surface::lastFrameCoveragePackMilliseconds() const noexcept {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->lastFrameStats().coveragePackMilliseconds
                                                       : 0.0;
}

uint32_t Surface::lastFrameCoverageFieldCount() const noexcept {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->lastFrameStats().coverageFieldCount : 0;
}

uint64_t Surface::lastFrameCoverageFieldArea() const noexcept {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->lastFrameStats().coverageFieldArea : 0;
}

uint32_t Surface::pooledFieldAtlasCount() const noexcept {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->pooledFieldAtlasCount() : 0;
}

backend::Device* Surface::device() noexcept {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->device() : nullptr;
}

backend::Device* Surface::releaseDevice() {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->releaseDevice() : nullptr;
}

backend::TextureHandle Surface::target() const noexcept {
  // The renderer's own handle, not a copy: it is released with the renderer, so a caller shares it rather than owning it.
  // Before `beginFrame` the target has still been created -- `Surface::make` allocates it -- so this is valid as soon as
  // the surface exists, which is what lets a caller wrap a platform buffer around it before the first frame.
  return mImpl != nullptr ? mImpl->target.color : backend::TextureHandle::invalid();
}

bool Surface::waitIdle() {
  if (mImpl == nullptr || mImpl->context == nullptr) {
    return false;
  }
  return backend::isSuccess(mImpl->context->waitIdle());
}

} // namespace gpu
} // namespace graphics


