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
 * @file GraphiteCanvasBackend.cpp
 *
 * The canvas backend that *records* instead of drawing.
 *
 * ## What it is for
 *
 * Every `Canvas` call this backend receives becomes an entry in a `DrawList`: device-space geometry, the
 * state that decides how it is filled, and the clip it was recorded under. Nothing touches a driver, nothing
 * allocates a texture, and nothing waits. That is the property the whole hardware-accelerated path is built
 * on -- the same separation the RHI's own `CommandBuffer` documents, one level up -- and it is what lets N
 * threads each hold a canvas and record a frame without a lock anywhere on the path.
 *
 * The list is then consumed by `gpu::ContextImpl::submit`, which precompiles it (parallel), uploads it and
 * renders it (single-threaded, because the GL context is).
 *
 * ## Why it borrows the `DrawList` rather than owning one
 *
 * A backend is created by `Canvas::createRecorderBackend`, which has no list to give it and no way to hand
 * one back. So the list lives in a `shared_ptr` the surface holds too: the surface reads it when it submits
 * and the backend appends to it as the canvas draws, which keeps the ownership where the frame's lifetime is
 * -- with whoever knows when the frame ends.
 *
 * ## What is deliberately not implemented yet
 *
 * `maskLayer` is a no-op, because a mask needs an image id and a pass that samples it, and the renderer's
 * image table is not part of this file. The canvas already refuses the call when there is no layer open, so
 * a caller that reaches here has a layer and loses its mask rather than getting a wrong picture -- and the
 * limitation is stated here rather than left to be discovered. `Canvas::target()` must not be called on a
 * canvas over this backend: there are no pixels to return before submission, which is exactly what
 * `CanvasBackend::pixels` documents.
 */

#include <backend/Types.h>

#include <graphics/Enums.h>
#include <graphics/Canvas.h>
#include <graphics/SdfGlyph.h>
#include <graphics/Paint.h>
#include <graphics/Shader.h>
#include <graphics/Surface.h>
#include <private/graphics/BlendMode.h>
#include <private/graphics/CanvasBackend.h>
#include <private/graphics/Flatten.h>
#include <private/graphics/StrokeOptions.h>
#include <private/graphics/Rasterizer.h>
#include <private/gpu/ContextImpl.h>

#include <gpu/DrawList.h>

#include "BlendRouting.h"
#include "CoverageRecord.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace graphics {
namespace {

/**
 * Largest glyph bitmap this backend will rasterise, on either axis.
 *
 * A glyph outline is bounded by the em box, so a sane glyph is at most a few hundred pixels and this limit is never
 * reached in practice. It exists so that a malformed or absurd font cannot make the recorder allocate an unbounded
 * bitmap: a glyph whose box is larger than this is skipped rather than turned into a gigabyte.
 */
constexpr int kMaxGlyphExtent = 4096;

/** @return @p value clamped to `[0, 1]`, since a paint component outside that range is a caller error. */
float clampUnit(const Scalar value) noexcept {
  if (!(value > 0.0f)) return 0.0f; // also catches NaN
  if (value > 1.0f) return 1.0f;
  return static_cast<float>(value);
}

/**
 * Copies @p shader's stops into a command's fixed-size array.
 *
 * Four stops, not an unbounded list. `DrawList::CommandState` carries four because that is what a batch's
 * uniform block holds, and a shader with more has to be approximated: the first and last are kept and the
 * two in between are sampled evenly, which is what every renderer with a fixed stop count does. The
 * approximation is recorded here rather than silently applied, because a gradient that loses a stop looks
 * like a gradient that was drawn wrong.
 */
void copyStops(const Shader& shader, gpu::DrawList::CommandState* state) {
  const std::vector<GradientStop>& stops = shader.stops();
  if (stops.empty()) return;

  const size_t count = std::min<size_t>(stops.size(), 4);
  for (size_t index = 0; index < count; ++index) {
    // Evenly resampled when there are more stops than slots, so the two endpoints are always the shader's
    // own and the interior ones keep the gradient's shape rather than its first four entries.
    const size_t source = stops.size() <= 4 ? index : (index * (stops.size() - 1)) / 3;
    const GradientStop& stop = stops[source];
    state->stops[index].offset = static_cast<float>(stop.position);
    state->stops[index].red = clampUnit(stop.color.red);
    state->stops[index].green = clampUnit(stop.color.green);
    state->stops[index].blue = clampUnit(stop.color.blue);
    state->stops[index].alpha = clampUnit(stop.color.alpha);
  }
  state->stopCount = static_cast<uint32_t>(count);
}

/**
 * Fills in the gradient geometry for @p shader.
 *
 * Two transforms meet here and it is worth being explicit about them. The gradient is written in *shader*
 * space and the shader's own local matrix maps that into *drawing* space -- which, for this path, is the
 * canvas's device space, because the geometry was already taken there. So a fragment knows a device pixel
 * and needs the gradient's coordinates, which is the **inverse** of the local matrix, and it needs it as six
 * coefficients rather than as a matrix so that the uniform block has no padding question.
 *
 * The two points are mapped through the local matrix rather than left as authored: a linear gradient whose
 * endpoints are written in a shape's bounding box has to be evaluated against device pixels, so its
 * endpoints have to be device pixels. Both ends are transformed here, once, instead of per fragment.
 */
void fillGradient(const Shader& shader, const Matrix& deviceToUser, const Point deviceOrigin,
        gpu::DrawList::CommandState* state) {
  // A fragment knows a *device* pixel and needs the gradient's own coordinates. Three transforms stand
  // between them and all three are constant across the draw, so they are composed once, here:
  //
  //   device --(deviceToUser)--> caller space --(local inverse)--> shader space
  //
  // `deviceToUser` is the canvas's inverse transform, handed in by the canvas rather than re-derived because
  // the canvas owns the transform stack and inverting it twice is how the two ends come to disagree about a
  // sub-pixel mapping. The shader's local matrix maps *shader* space into drawing space, so its inverse is
  // what takes drawing space back to shader space -- and the two together are `userToShader * deviceToUser`.
  //
  // ## And the rectangle's own origin, which is the part that has to be added rather than looked for
  //
  // The fragment stage computes its device position as `vTexCoord * vRectSize` -- the position *inside the quad*,
  // because the quad's coordinate is all `quad.vert` carries. It is not a device pixel until the quad's own device
  // origin is added, and nothing else in this chain knows where the quad is. So the origin is folded in here, on the
  // right, where it applies to the fragment's coordinate first.
  //
  // Without it the composition is missing a translation and every gradient drawn inside a translated context is
  // evaluated at the wrong place: silently, and worst of all in the cases that are easiest to get wrong, because a quad
  // at the device origin is exactly the case that works. That is why the gradient test passed while every gradient in
  // the SVG panel was wrong -- each SVG document is drawn into a cell at an offset, and the offset was being dropped.
  const Matrix& local = shader.localMatrix();
  const Matrix userToShader = local.isInvertible() ? local.invert() : Matrix {};
  const Matrix toDevice = Matrix::MakeTranslation(Vector3 { deviceOrigin.x, deviceOrigin.y, 0.0f });
  const Matrix composed = userToShader * deviceToUser * toDevice;

  state->gradient.inverseLocal[0] = composed.vec[0].x;
  state->gradient.inverseLocal[1] = composed.vec[1].x;
  state->gradient.inverseLocal[2] = composed.vec[0].y;
  state->gradient.inverseLocal[3] = composed.vec[1].y;
  state->gradient.inverseLocal[4] = composed.vec[3].x;
  state->gradient.inverseLocal[5] = composed.vec[3].y;
  state->gradient.tileMode = shader.tileMode();

  switch (shader.kind()) {
    case Shader::Kind::kLinearGradient: {
      state->shader = gpu::DrawList::ShaderKind::LinearGradient;
      // ## The endpoints are the *authored* ones, in shader space, and that is not the same thing as `local * point`
      //
      // `composed` above already takes the fragment all the way into shader space, so the endpoints it is compared
      // against have to be in shader space too -- which is where `Shader::start`/`end` are authored. Multiplying them by
      // the local matrix as well puts them in *drawing* space instead, and the two spaces coincide only when the local
      // matrix is the identity. So every gradient with a `localMatrix` -- which is every SVG gradient, since the SVG
      // layer folds `gradientTransform` and the object-bounding-box mapping into it -- was evaluated against endpoints
      // that had been moved twice, and the ramp landed in the wrong place or outside the shape entirely.
      //
      // The rasterizer has the convention right and is what settled it: `Shader::sampleAt` takes a drawing-space point,
      // applies `localMatrix.invert()` itself, and then compares against these same authored endpoints. See
      // `GpuSurface.AGradientHonoursItsLocalMatrixAndItsOrigin`.
      const Point start = shader.start();
      const Point end = shader.end();
      state->gradient.points[0] = start;
      state->gradient.points[1] = end;
      state->gradient.radius = 0.0f;
      break;
    }
    case Shader::Kind::kRadialGradient: {
      state->shader = gpu::DrawList::ShaderKind::RadialGradient;
      state->gradient.points[0] = shader.start();
      // The second point is a point *on* the radius rather than the radius itself, in the same shader space as the
      // centre. The scalar radius is what the fragment stage actually uses, and it is the right answer in this space: a
      // circle in shader space becomes the ellipse a non-uniform local matrix asks for, which a radius in drawing space
      // could not express.
      state->gradient.points[1] = Point { shader.start().x + shader.radius(), shader.start().y };
      state->gradient.radius = shader.radius();
      break;
    }
    case Shader::Kind::kImage:
      // An image shader is a repeating picture, which this path does not carry: it needs a texture and a
      // tiling fragment stage. Reported as a solid fill of the paint's own colour rather than as a shape
      // that vanishes, which is the same choice `Shader::isValid` documents for an unusable shader.
      state->shader = gpu::DrawList::ShaderKind::Solid;
      break;
  }

  copyStops(shader, state);
}

/** Fills in everything about *how* a command is drawn, from the paint and the canvas's transforms.
 *
 * @param deviceOrigin the device-space position of the quad's own coordinate origin, which only a shader needs: see
 *        `fillGradient`. A caller with no shader to sample pays nothing for it.
 */
void fillState(const Paint& paint, const Matrix& deviceToUser, const Point deviceOrigin, bool antiAlias,
        gpu::DrawList::CommandState* state) {
  state->blendMode = paint.blendMode;
  state->antiAlias = antiAlias;
  state->alpha = 1.0f;

  // The colour filter is applied to the source colour once, here, rather than per fragment: a colour matrix
  // is linear in the premultiplied components and source-over is linear in the source, so filtering the
  // colour and then blending is identical to filtering every contribution -- and it costs one call rather
  // than one per pixel, on a stage that has no filter of its own.
  Color source = paint.color;
  if (paint.colorFilter != nullptr) source = paint.colorFilter->applyTo(source);

  state->red = clampUnit(source.red);
  state->green = clampUnit(source.green);
  state->blue = clampUnit(source.blue);

  if (paint.hasShader() && paint.shader != nullptr && paint.shader->isValid()) {
    fillGradient(*paint.shader, deviceToUser, deviceOrigin, state);
    // The paint's alpha multiplies whatever the shader produced, so fading a gradient needs no second
    // shader. Kept separate from the stops rather than folded into them, because a gradient fades as a
    // whole and folding would change each stop's own alpha.
    state->alpha = clampUnit(source.alpha);
  } else {
    // No usable shader: the paint's own colour, with its alpha. A paint holding an *invalid* shader falls
    // back here too, which is what `Shader::isValid` promises -- a gradient that failed paints flat rather
    // than painting nothing.
    state->shader = gpu::DrawList::ShaderKind::Solid;
    state->alpha = clampUnit(source.alpha);
  }
}

/**
 * @return @p image with @p filter applied to every texel, or @p image itself when its pixels cannot be read.
 *
 * ## Why a bitmap's colour filter is applied to its pixels and not in the fragment stage
 *
 * Every other kind of draw can carry a filter into a shader. A solid fill's colour is one value, and the recorder filters
 * it on the CPU already (`fillState`), which is exact because a colour matrix is linear in the premultiplied components
 * and source-over is linear in the source. An *image* cannot: its colour is the texture, so the filter has to reach every
 * texel, and both places that could carry it are closed --
 *
 *  - `image.frag` has a sampler, the quad's varyings and nothing else, and its whole reason to exist is that its
 *    per-command data is exactly a sub-rectangle: `vParams` is four components and all four are the sub-rect;
 *  - a 4x5 matrix is twenty floats, so it is not per-vertex data either -- and an image batch is **one** draw covering
 *    many commands, so a uniform block could not describe them without giving the batching up.
 *
 * So the filter is applied to the pixels, which is also what the rasterizer does: it filters the texel it samples through
 * this same `ColorFilter::applyToPremultipliedArgb`. Using the filter's own eight-bit path rather than writing the matrix
 * a second time in GLSL is what makes the two backends agree *exactly* rather than approximately -- a matrix evaluated in
 * a fragment stage would be float arithmetic on a value the rasterizer has already quantised to eight bits, and the demo
 * compares the two pictures pixel by pixel.
 *
 * ## The cost, which is the trade this makes
 *
 * One pass over the bitmap's own texels per image draw. That is bounded by the *source's* area rather than the
 * destination's, where the rasterizer's per-texel filter is bounded by the destination's -- so for a magnified image this
 * is the cheaper of the two, and for a minified one it is the more expensive. The upload that follows is unaffected: it
 * is cached on the content hash of what is uploaded, so a static filtered image is uploaded once however many frames draw
 * it. A caller that redraws a large filtered bitmap every frame is paying for the filter pass each time, and the way to
 * avoid it is the way that always works -- filter the bitmap once with `ColorFilter::applyToPremultipliedArgb` and hand
 * that in.
 */
Bitmap applyColorFilter(const Bitmap& image, const ColorFilter& filter) {
  const Pixmap source = image.makePixmap();
  if (!source.isValid() || source.pixels() == nullptr) return image;

  // Allocated with the source's own `ImageInfo`, so the channels keep the layout the upload path requires. A filtered
  // copy in a different format would be a correctly filtered picture uploaded with its channels permuted, which is a
  // wrong image with no error anywhere -- `ensureImagePlacement` rejects anything that is not RGBA8888 for this reason.
  Bitmap filtered = Bitmap::allocate(image.info());
  Pixmap destination = filtered.lockPixels();
  if (destination.isEmpty() || destination.pixels() == nullptr) return image;

  for (int y = 0; y < source.height(); ++y) {
    for (int x = 0; x < source.width(); ++x) {
      destination.setPixelARGB32(x, y, filter.applyToPremultipliedArgb(source.getPixelARGB32(x, y)));
    }
  }
  return filtered;
}

/**
 * The bitmaps a recording references now live on the `DrawList` itself, as `DrawList::images`.
 *
 * They used to live in a file-static `unordered_map` keyed on the list's address, with a mutex, and the
 * comment that stood here argued for it: the map died with the process and a dead entry could not be
 * proved dead, so never erasing was the safer trade. Both halves of that were wrong. The entries never
 * being erased is a leak proportional to how much was ever drawn, and keying on an address meant a list
 * reusing a freed one read the previous list's bitmaps -- a wrong picture with no error anywhere.
 *
 * Owning the table removes the mutex, the leak and the aliasing at once, and it is what a reader assumes
 * anyway: `CommandState::image` is an index, and the table is what it indexes.
 */

/** Records @p image against @p list and returns the id a command should name it by. */uint32_t registerImage(gpu::DrawList* list, Bitmap image) {
  // No lock: the list is this recorder's own, and the recorder belongs to one canvas on one thread. The registry this
  // replaced needed a mutex because it was shared by every list in the process, which was the symptom of the ownership
  // being wrong rather than a reason to keep it.
  list->images.push_back(std::move(image));
  list->imageHashes.push_back(0);
  return static_cast<uint32_t>(list->images.size() - 1);
}

/**
 * The recording backend: turns canvas calls into `DrawList` commands.
 *
 * See the file comment for what it is for and for the two limitations it has today -- no mask layer, and no
 * pixels to hand back before submission.
 */
struct SharedSdfGlyph {
  std::shared_ptr<Bitmap> bitmap;
  float left = 0.0f;
  float top = 0.0f;
  float right = 0.0f;
  float bottom = 0.0f;
};

std::mutex gSdfCacheMutex;
std::unordered_map<uint64_t, SharedSdfGlyph> gSdfCache;

/**
 * A shaped clip's coverage, kept across frames.
 *
 * The recorder rasterises a clip path every time `Canvas::clipPath` runs, and a static clip is
 * re-rasterised identically on every frame. The key is the device-space path's content plus the
 * fill rule and the device clip it was intersected with -- not the object identity, because callers
 * rebuild the same `Path` each frame. A hit copies the coverage bytes (cheap, area-proportional) and
 * skips contour flattening, rasterisation and the per-pixel copy.
 */
struct CachedClipMask {
  uint64_t pathHash = 0;
  FillType fillType = FillType::Winding;
  Rect clip {};
  Rect bounds {};
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> pixels;
};

uint64_t mixHash(uint64_t hash, const void* data, const size_t size) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= 1099511628211ull;
  }
  return hash;
}

uint64_t clipMaskKey(const uint64_t pathHash, const FillType fillType, const Rect& clip) {
  uint64_t key = mixHash(pathHash, &fillType, sizeof(fillType));
  const float edges[4] = { clip.getLeft(), clip.getTop(), clip.getRight(), clip.getBottom() };
  return mixHash(key, edges, sizeof(edges));
}

uint64_t clipPathHash(const pathops::Path& path) {
  uint64_t hash = 14695981039346656037ull;
  for (const pathops::PathSegment& segment: path.segments()) {
    hash = mixHash(hash, &segment.verb, sizeof(segment.verb));
    hash = mixHash(hash, &segment.pointCount, sizeof(segment.pointCount));
    for (uint8_t point = 0; point < segment.pointCount; ++point) {
      hash = mixHash(hash, &segment.points[point].x, sizeof(double));
      hash = mixHash(hash, &segment.points[point].y, sizeof(double));
    }
  }
  return hash;
}

bool sameClipRect(const Rect& a, const Rect& b) {
  return a.getLeft() == b.getLeft() && a.getTop() == b.getTop() &&
         a.getRight() == b.getRight() && a.getBottom() == b.getBottom();
}

std::vector<CachedClipMask>& clipMaskCache() {
  // Recording happens on one thread at a time; a thread-local cache also keeps a recorder on a job
  // thread from racing a canvas on another one.
  static thread_local std::vector<CachedClipMask> cache;
  return cache;
}

CachedClipMask* findCachedClipMask(const uint64_t pathHash, const FillType fillType,
  const Rect& clip) {
  for (CachedClipMask& entry: clipMaskCache()) {
    if (entry.pathHash == pathHash && entry.fillType == fillType && sameClipRect(entry.clip, clip)) {
      return &entry;
    }
  }
  return nullptr;
}

void storeCachedClipMask(CachedClipMask entry) {
  constexpr size_t kMaxEntries = 32;
  constexpr size_t kMaxBytes = 16u * 1024u * 1024u;
  std::vector<CachedClipMask>& cache = clipMaskCache();
  cache.insert(cache.begin(), std::move(entry));
  size_t bytes = 0;
  for (const CachedClipMask& cached: cache) bytes += cached.pixels.size();
  while (cache.size() > kMaxEntries || (bytes > kMaxBytes && cache.size() > 1)) {
    bytes -= cache.back().pixels.size();
    cache.pop_back();
  }
}

class GraphiteCanvasBackend final: public CanvasBackend {
public:
  GraphiteCanvasBackend(std::shared_ptr<gpu::ContextImpl> context, std::shared_ptr<gpu::DrawList> list)
    : mContext(std::move(context)), mList(std::move(list)) {
    // Index 0 is always the root target, so `Command::layer` needs no sentinel and a layer index is stable
    // for the life of the list. Every command this backend records names its layer, including the ones that
    // are not in one.
    gpu::Layer root;
    root.label = "root";
    mList->layers.push_back(std::move(root));
    mDrawLayer = 0;
  }

  // --- Target -------------------------------------------------------------------------------------

  [[nodiscard]] Rect deviceBounds() const noexcept override { return mDeviceBounds; }

  /**
   * False: this backend has not drawn anything by the time a call returns, it has *recorded* it.
   *
   * The distinction is what a caller branches on to know whether `readback` is a copy of pixels that already
   * exist or a read from a render target that has to be submitted first. Answering true here would make
   * `Surface::snapshot` skip the submission and copy an empty bitmap.
   */
  [[nodiscard]] bool isRaster() const noexcept override { return false; }
[[nodiscard]] bool isRecording() const noexcept override { return true; }

  /**
   * True: a recorded command reaches a command list, which is somewhere to draw.
   *
   * pixels() answers null for this backend and that is correct -- there are no pixels until the frame is
   * submitted -- which is why the canvas asks *this* question before a draw and the other one only when it
   * needs to read the result.
   */
  [[nodiscard]] bool hasTarget() const noexcept override { return true; }

  /**
   * An *empty* bitmap, and deliberately not a null pointer.
   *
   * A recording backend has no pixels: the list is geometry and the surface allocates the target at submit time. The
   * canvas uses this to decide several things -- whether a layer can be opened by copying pixels, whether
   * `maskLayer` has anything to mask, whether a filter can render into a scratch buffer -- and the empty bitmap gives
   * the right answer to all of them: a layer is an `OpenLayer` command rather than a copy, a mask is not applied, and
   * a filter cannot be pre-rendered so it falls back to drawing unfiltered.
   *
   * Returning `nullptr` here was a latent crash rather than a working design. `Canvas::target()` is
   * `*backend->pixels()`, so a reference to an empty bitmap and a null pointer dereference look identical to the
   * canvas -- until a caller reads the reference. `svg::drawFiltered` does exactly that (it reads `target()` to size
   * its scratch buffer), and the crash surfaced only when an SVG document containing a filter was drawn to the GPU
   * canvas. Nothing in the raster tests could reach it, because the rasterizer's `pixels()` is a real bitmap.
   */
  [[nodiscard]] Bitmap* pixels() noexcept override { return &mEmptyTarget; }

  // --- State --------------------------------------------------------------------------------------

  /**
   * What `pixels()` hands out. Never written, never read as pixels, and `isEmpty()` is true.
   *
   * A member rather than a function-local static because `pixels()` returns a mutable pointer: a shared static would
   * let one canvas's caller scribble on what another canvas would then see.
   */
  Bitmap mEmptyTarget;

  void reset(const Rect& bounds) override {
    // The base clip is the whole surface, and it is set *here* rather than left to the canvas's first
    // setDeviceClip. The canvas does call that next, but a shaped clip consults the clip rectangle before any
    // drawing call -- so a backend whose rectangle was still default would intersect every path clip against a
    // zero-area rectangle and refuse all of them. That is a canvas that silently has no shaped clips at all,
    // which is exactly what happened before this line existed.
    mDeviceBounds = bounds;
    mDeviceClip = bounds;
    mDrawLayer = 0;
    mActiveMask = gpu::DrawList::kNoClipMask;
  }

  /**
   * Reacts to the canvas pushing a state entry for an ordinary `save`.
   *
   * The entry is marked as *not* a layer, and that mark is why this stack holds a struct rather than a layer
   * index. `Canvas::saveLayer` does **not** call `onSave` -- it calls `beginLayer` -- so the two ways a state
   * entry can be pushed arrive through different calls, and only one of them opens something a `restore` has
   * to close. A stack that could not tell them apart closes a layer on every nested `restore`; the version
   * this replaced compared the saved index against the current one and, for the common case where they were
   * equal, never closed a layer at all -- which is a frame that silently loses its layer, not an error.
   */
  void onSave() override {
    mSavedStack.push_back(SavedState { mDrawLayer, false, mDeviceClip, mActiveMask });
  }

  /**
   * Reacts to the canvas popping a state entry.
   *
   * @return true when the pop closed a layer, which is what tells the canvas a composite happened.
   */
  bool onRestore() override {
    if (mSavedStack.empty()) return false;

    const SavedState saved = mSavedStack.back();
    mSavedStack.pop_back();

    // ## The clip is part of the state a restore returns to
    //
    // `Canvas::saveLayer` narrows this backend's clip through `setDeviceClip`, and nothing ever widened it again:
    // `Canvas::restore` pops its own state and calls this, and this restored the layer but not the clip. So after
    // any SVG document -- every group of which opens a layer -- the clip stayed narrowed to the last layer's bounds,
    // and every rectangle drawn afterwards was clipped away.
    //
    // That is what removed the backing from twenty of the corpus grid's cells while leaving the cells drawn *before*
    // each document intact, and it is why the erasure could not be found in the composite: the rectangles never
    // reached it. The rasterizer does not have the bug because its backend keeps a stack of clips and pops one per
    // restore; this backend keeps a single rectangle, so the restore has to put it back.
    //
    // The value is the one saved when this entry was pushed, which `Canvas::save*` already narrowed for a layer and
    // left alone for a plain save -- so restoring it is correct in both cases.
    mDeviceClip = saved.deviceClip;
    // The shaped clip is part of the state too. It used to be a single member that `restore` left
    // alone, so every command after a clip's `restore` still named that clip's coverage mask: the
    // shape appeared to leak past its own scope, and with the clip rewritten as a masked layer it
    // hid everything drawn after it.
    mActiveMask = saved.activeMask;

    if (!saved.openedLayer) {
      // A plain save. Restoring the layer is what keeps a `restore` made *inside* a layer from leaving it: the
      // layer stays open and the next command still belongs to it.
      mDrawLayer = saved.layer;
      return false;
    }

    // The layer being closed is the one that is *current*, and the close is recorded against it rather than
    // against its parent: a compositor reading `Command::layer` needs to know which offscreen image to take its
    // source from, while the destination -- the parent -- is the layer it is about to become. Recording the
    // close against the parent, which is what assigning `mDrawLayer` first would do, names the destination twice
    // and the source never, so the composite has nothing to read.
    recordCloseLayer(mDrawLayer);
    mDrawLayer = saved.layer;
    return true;
  }

  void setDeviceClip(const Rect& deviceClip) override { mDeviceClip = deviceClip; }

  bool beginLayer(const Rect& deviceBounds, const Paint& paint) override {
    const Rect clipped = intersect(mDeviceClip, deviceBounds);
    if (clipped.getRight() <= clipped.getLeft() || clipped.getBottom() <= clipped.getTop()) {
      // A layer with no visible pixels cannot be opened and does not need to be: the canvas keeps drawing
      // into its parent and every command inside it will be rejected by the clip anyway.
      return false;
    }

    gpu::Layer layer;
    layer.bounds = clipped;
    layer.format = backend::PixelFormat::R8G8B8A8_UNORM;
    layer.premultiplied = true;
    // The caller's alpha, which the composite reads when this layer closes. `saveLayerAlpha` reaches `beginLayer`
    // through the paint, and `fillState` does not carry a plain paint's alpha -- so it is taken here, from the paint
    // the canvas handed over, and the closing command reads it from the layer.
    layer.alpha = clampUnit(paint.color.alpha);
    // The mode the composite will combine the layer with, taken from the same paint and for the same reason as the
    // alpha: it is the caller's answer to "how does this group meet what is already there", and only the composite can
    // apply it. Without this the closing command's hard-coded `kSrcOver` won the argument -- see `Layer::blendMode`.
    layer.blendMode = paint.blendMode;
    layer.linearLightText = paint.linearLightLayer;
    layer.textColor = paint.color;
    layer.linearLightGamma = paint.linearLightGamma;
    layer.linearLightContrast = paint.linearLightContrast;
    layer.label = "canvas-layer";

    gpu::DrawList::Command command;
    command.kind = gpu::DrawList::Kind::OpenLayer;
    command.geometry[0] = clipped;
    command.clipRect = mDeviceClip;
    command.layer = mDrawLayer;
    fillState(paint, Matrix {}, Point {}, paint.antiAlias, &command.state);
    mList->commands.push_back(command);

    mList->layers.push_back(std::move(layer));
    const uint32_t opened = static_cast<uint32_t>(mList->layers.size() - 1);
    // The entry records the layer it *came from*, which is where the matching restore returns to, and marks
    // itself as one that opened a layer so that onRestore knows to emit the close.
    mSavedStack.push_back(SavedState { mDrawLayer, true, mDeviceClip, mActiveMask });
    mDrawLayer = opened;
    return true;
  }

  /**
   * Opens an implicit layer for a draw whose paint carries a blend mode the fixed-function unit cannot express.
   *
   * ## Why a draw needs a layer at all
   *
   * Modes above `kScreen` are defined by a function of *both* colours -- `kMultiply` is `s * d`, `kOverlay` branches on
   * the destination, `kColorDodge` divides by the source -- so they cannot be written as a factor pair, and no fragment
   * stage can evaluate them from its own inputs: the destination is the image being written. The only way to get `d` into
   * a shader is to stop writing into the destination and render the shape somewhere else first, which is a layer. This
   * is the same construction `Layer`'s own note gives for group opacity and masks, and it reuses the composite that
   * already serves all fourteen of those modes (`composite.frag`), rather than adding a blend stage to every program.
   *
   * ## What the caller gets back
   *
   * @param outPaint receives the paint the draw itself should be recorded with: the same paint with `kSrcOver`, because
   *        the layer's composite applies the mode instead. Recording it with the original mode would ask the renderer for
   *        a batch it cannot serve -- it would be counted as an unsupported omission, or drawn source-over, which is the
   *        defect this exists to remove.
   *
   * The layer's own alpha is forced to one while the paint's alpha stays on the source, and that split is deliberate: a
   * translucent `kMultiply` shape must blend `s * d` with the alpha applied to `s`, not blend `s * d` and then scale the
   * result. Keeping the alpha on the draw inside the layer also keeps a colour filter's effect on that alpha, which
   * moving it to the layer would quietly change.
   *
   * @return true when a layer was opened, in which case the caller must call `onRestore` once the draw is recorded.
   */
  bool beginBlendLayer(const Rect& deviceBounds, const Paint& paint, Paint* outPaint) {
    *outPaint = paint;
    if (gpu::isBlendUnitMode(paint.blendMode)) {
      // The blend unit serves this mode directly, so there is nothing to isolate and the draw keeps its own mode.
      //
      // Testing this *before* touching `outPaint` is load-bearing, and getting it the other way round is a mistake
      // this function made and its own test caught: clearing the mode first turned every blend-unit mode into
      // source-over, so `kScreen` -- which the unit expresses exactly -- came out as the source colour composited
      // with one-minus-alpha. The pipeline trace said it plainly: only modes 1 and 3 were ever created for a scene
      // that asked for `kScreen`.
      return false;
    }

    // The group is pure: `beginLayer` takes the layer's alpha from this paint, and the draw inside the layer is what
    // carries the caller's alpha.
    Paint group = paint;
    group.color.alpha = 1.0f;
    if (!beginLayer(deviceBounds, group)) {
      return false;
    }
    // The layer's composite applies the mode, so the draw inside it must not ask for it again.
    outPaint->blendMode = BlendMode::kSrcOver;
    return true;
  }

  void beginRecordingScope() override {
    if (mScopeActive) return;
    mScopeSavedList = mList;
    mScopeSavedImages = std::move(mImageIndices);
    mScopeSavedLayer = mDrawLayer;
    mScopeSavedMask = mActiveMask;
    mScopeSavedStack = mSavedStack.size();
    mList = std::make_shared<gpu::DrawList>();
    mList->label = "layer-cache";
    gpu::Layer root;
    root.label = "root";
    mList->layers.push_back(std::move(root));
    mDrawLayer = 0;
    mActiveMask = gpu::DrawList::kNoClipMask;
    mImageIndices.clear();
    mScopeActive = true;
  }

  void endRecordingScope(gpu::DrawList& out) override {
    if (!mScopeActive) return;
    out = std::move(*mList);
    mList = std::move(mScopeSavedList);
    mScopeSavedList.reset();
    mImageIndices = std::move(mScopeSavedImages);
    mDrawLayer = mScopeSavedLayer;
    mActiveMask = mScopeSavedMask;
    mSavedStack.resize(mScopeSavedStack);
    mScopeActive = false;
  }

  [[nodiscard]] bool inRecordingScope() const override { return mScopeActive; }

  bool appendRecording(const gpu::DrawList& source, const Point translation) override {
    if (mList == nullptr || !source.externalImages.empty()) return false;
    // A translated replay: the list is device space, so every device-space field moves with it. The
    // coverage fields are placed on whole device pixels, so a fractional translation cannot be expressed
    // and the caller is told to draw instead.
    if (translation.x != 0.0f || translation.y != 0.0f) {
      if (translation.x != std::floor(translation.x) || translation.y != std::floor(translation.y)) {
        return false;
      }
    }
    const Rect shift = Rect::MakeXYWH(translation.x, translation.y, 0.0f, 0.0f);
    const auto moved = [&translation](Rect rect) {
      return Rect::MakeLTRB(rect.getLeft() + translation.x, rect.getTop() + translation.y,
        rect.getRight() + translation.x, rect.getBottom() + translation.y);
    };
    (void)shift;
    gpu::DrawList& destination = *mList;
    const uint32_t rootLayer = mDrawLayer;
    const auto contourBase = static_cast<uint32_t>(destination.contours.size());
    const auto pointBase = static_cast<uint32_t>(destination.points.size());
    const auto maskBase = static_cast<uint32_t>(destination.clipMasks.size());
    const auto pixelBase = static_cast<uint32_t>(destination.maskPixels.size());
    const auto fieldBase = static_cast<uint32_t>(destination.coverageFields.size());

    std::vector<uint32_t> layerMap(source.layers.size(), rootLayer);
    for (size_t index = 1; index < source.layers.size(); ++index) {
      layerMap[index] = static_cast<uint32_t>(destination.layers.size());
      gpu::Layer layer = source.layers[index];
      layer.bounds = moved(layer.bounds);
      destination.layers.push_back(std::move(layer));
    }

    std::vector<uint32_t> imageMap(source.images.size(), gpu::DrawList::kNoImage);
    for (size_t index = 0; index < source.images.size(); ++index) {
      const graphics::Bitmap& image = source.images[index];
      const uint64_t key = reinterpret_cast<uintptr_t>(image.data().bytes()) ^
        (static_cast<uint64_t>(image.data().size()) << 1) ^
        (static_cast<uint64_t>(image.width()) << 17) ^
        (static_cast<uint64_t>(image.height()) << 33) ^
        (static_cast<uint64_t>(image.rowBytes()) << 49);
      if (const auto found = mImageIndices.find(key); found != mImageIndices.end()) {
        imageMap[index] = found->second;
      } else {
        const uint32_t placed = static_cast<uint32_t>(destination.images.size());
        destination.imageHashes.resize(destination.images.size(), 0);
        destination.images.push_back(image);
        destination.imageHashes.push_back(source.imageHashes.size() > index ? source.imageHashes[index] : 0);
        mImageIndices.emplace(key, placed);
        imageMap[index] = placed;
      }
    }

    for (gpu::DrawList::Command command: source.commands) {
      if (command.contourCount != 0) command.firstContour += contourBase;
      if (command.clipMask != gpu::DrawList::kNoClipMask) command.clipMask += maskBase;
      if (command.state.coverageField != gpu::DrawList::kNoCoverageField) {
        command.state.coverageField += fieldBase;
      }
      if (command.state.image != gpu::DrawList::kNoImage) {
        command.state.image = imageMap[command.state.image];
      }
      if (command.kind == gpu::DrawList::Kind::ExternalImage) return false;
      command.layer = layerMap[command.layer];
      for (Rect& geometry: command.geometry) geometry = moved(geometry);
      command.clipRect = moved(command.clipRect);
      destination.commands.push_back(std::move(command));
    }

    for (gpu::DrawList::Contour contour: source.contours) {
      contour.firstPoint += pointBase;
      destination.contours.push_back(contour);
    }
    for (Point point: source.points) {
      point.x += translation.x;
      point.y += translation.y;
      destination.points.push_back(point);
    }
    for (gpu::DrawList::ClipMask mask: source.clipMasks) {
      mask.firstPixel += pixelBase;
      destination.clipMasks.push_back(mask);
    }
    destination.maskPixels.insert(destination.maskPixels.end(), source.maskPixels.begin(),
      source.maskPixels.end());
    for (gpu::DrawList::CoverageField field: source.coverageFields) {
      field.firstContour += contourBase;
      // Field origins are whole device pixels, which is why a fractional translation is refused above.
      field.originX += static_cast<int32_t>(std::lround(translation.x));
      field.originY += static_cast<int32_t>(std::lround(translation.y));
      destination.coverageFields.push_back(std::move(field));
    }
    destination.bounds = unite(destination.bounds, moved(source.bounds));
    return true;
  }

  void maskLayer(const Bitmap& mask, const MaskMode mode, const bool alignToLayer) override {
    if (mask.isEmpty()) return;
    if (mDrawLayer == 0 || mDrawLayer >= mList->layers.size()) return;

    gpu::Layer& layer = mList->layers[mDrawLayer];
    layer.asMask = true;
    layer.maskRule = mode == MaskMode::kLuminance ? gpu::Layer::MaskRule::Luminance
                                                  : gpu::Layer::MaskRule::Alpha;
    layer.maskLocal = alignToLayer;
    layer.maskImage = registerImage(mList.get(), mask);
  }

  bool clipPath(const pathops::Path& devicePath, const FillType fillType) override {
    const uint64_t pathHash = clipPathHash(devicePath);
    if (const CachedClipMask* cached = findCachedClipMask(pathHash, fillType, mDeviceClip)) {
      gpu::DrawList::ClipMask mask;
      mask.firstPixel = static_cast<uint32_t>(mList->maskPixels.size());
      mask.width = cached->width;
      mask.height = cached->height;
      mask.bounds = cached->bounds;
      mask.contentHash = clipMaskKey(pathHash, fillType, mDeviceClip);
      mList->maskPixels.insert(mList->maskPixels.end(), cached->pixels.begin(), cached->pixels.end());
      mList->clipMasks.push_back(mask);
      mActiveMask = static_cast<uint32_t>(mList->clipMasks.size() - 1);
      return true;
    }

    std::vector<raster::Contour> contours = raster::flattenPath(devicePath);
    if (contours.empty()) return false;

    // The mask is rasterised here, on the recording thread, because it is coverage rather than geometry: a
    // shaped clip is a per-pixel quantity, and the renderer's job is to sample it, not to derive it. Doing it
    // here also means the work is done in parallel across recorders, which is where the CPU budget is.
    Rect bounds = contours.front().bounds();
    for (const raster::Contour& contour: contours) {
      const Rect contourBounds = contour.bounds();
      bounds = Rect::MakeLTRB(std::min(bounds.getLeft(), contourBounds.getLeft()),
              std::min(bounds.getTop(), contourBounds.getTop()),
              std::max(bounds.getRight(), contourBounds.getRight()),
              std::max(bounds.getBottom(), contourBounds.getBottom()));
    }
    bounds = intersect(bounds, mDeviceClip);
    const int width = static_cast<int>(std::ceil(bounds.getRight())) - static_cast<int>(std::floor(bounds.getLeft()));
    const int height = static_cast<int>(std::ceil(bounds.getBottom())) - static_cast<int>(std::floor(bounds.getTop()));
    if (width <= 0 || height <= 0) return false;

    gpu::DrawList::ClipMask mask;
    mask.firstPixel = static_cast<uint32_t>(mList->maskPixels.size());
    mask.width = static_cast<uint32_t>(width);
    mask.height = static_cast<uint32_t>(height);
    mask.bounds = Rect::MakeLTRB(static_cast<Scalar>(std::floor(bounds.getLeft())),
            static_cast<Scalar>(std::floor(bounds.getTop())),
            static_cast<Scalar>(std::floor(bounds.getLeft()) + width),
            static_cast<Scalar>(std::floor(bounds.getTop()) + height));
    mask.contentHash = clipMaskKey(pathHash, fillType, mDeviceClip);
    mList->maskPixels.resize(mList->maskPixels.size() + static_cast<size_t>(width) * static_cast<size_t>(height), 0);

    // ## The scratch is the size of the *mask*, not of the device
    //
    // `raster::fillContours` reports coverage in the coordinate space of the contours it is given, and these
    // contours are already in device space -- so rasterising them into a bitmap the size of their bounding box
    // would write nothing at all, because every pixel it reports is outside it. The way out is to move the
    // contours into the mask's own space first, which is what `recordCoverageImage` does a hundred lines below
    // for exactly the same reason; the shift is in place, so it costs no second copy of the geometry.
    //
    // This used to allocate a **device-sized** scratch -- 1000x1160, 4.6 MB, zeroed -- for every shaped clip,
    // on the argument that the allocation was cheaper than the shift. It is not, and it is not free either: the
    // demo scene rasterises six shaped clips a frame (4.1.39), so that was 27 MB of allocation and zeroing per
    // frame for masks whose own rectangles are a few thousand texels. The values written are identical either
    // way -- the same coverage round trip through the same pixel accessor, one bitmap size apart.
    const int originX = static_cast<int>(std::floor(bounds.getLeft()));
    const int originY = static_cast<int>(std::floor(bounds.getTop()));
    for (raster::Contour& contour: contours) {
      for (Point& point: contour.points) {
        point.x -= static_cast<Scalar>(originX);
        point.y -= static_cast<Scalar>(originY);
      }
    }

    Bitmap scratch = Bitmap::allocate(ImageInfo(width, height, ColorType::kRGBA_8888, AlphaType::kPremultiplied));
    if (scratch.isEmpty()) return false;
    Pixmap pixels = scratch.lockPixels();
    raster::fillContours(contours, pixels, fillType,
            [&pixels](const int x, const int y, const uint32_t coverage) {
              // White at the coverage: the mask's alpha is what is read, and writing it into every channel keeps
              // it premultiplied and therefore consistent with how the rest of a bitmap is stored.
              pixels.setPixelARGB32(x, y, (coverage << 24) | (coverage << 16) | (coverage << 8) | coverage);
            });

    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const uint32_t texel = pixels.getPixelARGB32(x, y);
        mList->maskPixels[mask.firstPixel + static_cast<size_t>(y) * static_cast<size_t>(width) +
                          static_cast<size_t>(x)] = static_cast<uint8_t>((texel >> 24) & 0xFFu);
      }
    }

    mList->clipMasks.push_back(mask);
    mActiveMask = static_cast<uint32_t>(mList->clipMasks.size() - 1);

    CachedClipMask entry;
    entry.pathHash = pathHash;
    entry.fillType = fillType;
    entry.clip = mDeviceClip;
    entry.bounds = mask.bounds;
    entry.width = mask.width;
    entry.height = mask.height;
    entry.pixels.assign(mList->maskPixels.begin() + mask.firstPixel, mList->maskPixels.end());
    storeCachedClipMask(std::move(entry));
    return true;
  }

  // --- Drawing ------------------------------------------------------------------------------------

  void clear(const Color& color) override {
    // `Canvas::clear` ignores the transform and the clip, so this is not a rectangle: it is a command the
    // renderer turns into a load operation rather than into a draw. Recorded as a clipped-everything
    // rectangle would be wrong -- a clear that honoured a leftover clip leaves the parts outside it holding
    // the previous frame, which is the classic double-buffering artefact.
    gpu::DrawList::Command command;
    command.kind = gpu::DrawList::Kind::Rect;
    command.geometry[0] = mDeviceBounds;
    command.clipRect = mDeviceBounds;
    command.layer = mDrawLayer;
    command.state.shader = gpu::DrawList::ShaderKind::Solid;
    command.state.red = clampUnit(color.red);
    command.state.green = clampUnit(color.green);
    command.state.blue = clampUnit(color.blue);
    command.state.alpha = clampUnit(color.alpha);
    command.state.blendMode = BlendMode::kSrc; // a clear replaces rather than composites
    command.state.antiAlias = false;
    mList->commands.push_back(command);
    mList->bounds = unite(mList->bounds, mDeviceBounds);
  }

  void drawColor(const Color& color, const BlendMode blendMode) override {
    Paint paint;
    paint.color = color;
    paint.blendMode = blendMode;
    // A flood fill of the clip is a rectangle over the clip with no antialiasing: the clip's own edges are
    // already the shape, and feathering them would leave a translucent rim inside the fill.
    //
    // The layer, when the mode needs one, covers the clip: that *is* the shape here.
    Paint inner;
    const bool layered = beginBlendLayer(mDeviceClip, paint, &inner);
    gpu::DrawList::Command command;
    command.kind = gpu::DrawList::Kind::Rect;
    command.geometry[0] = mDeviceClip;
    command.clipRect = mDeviceClip;
    command.clipMask = mActiveMask;
    command.layer = mDrawLayer;
    fillState(inner, Matrix {}, Point { mDeviceClip.getLeft(), mDeviceClip.getTop() }, false, &command.state);
    mList->commands.push_back(command);
    mList->bounds = unite(mList->bounds, mDeviceClip);
    if (layered) onRestore();
  }

  void drawRect(const Rect& deviceRect, const Paint& paint) override {
    Paint inner;
    const bool layered = beginBlendLayer(deviceRect, paint, &inner);
    gpu::DrawList::Command command;
    command.kind = gpu::DrawList::Kind::Rect;
    command.geometry[0] = deviceRect;
    command.clipRect = mDeviceClip;
    command.clipMask = mActiveMask;
    command.layer = mDrawLayer;
    fillState(inner, Matrix {}, Point { deviceRect.getLeft(), deviceRect.getTop() }, inner.antiAlias,
            &command.state);
    mList->commands.push_back(command);
    mList->bounds = unite(mList->bounds, deviceRect);
    if (layered) onRestore();
  }

  void drawPath(const pathops::Path& devicePath, const FillType fillType, const Scalar scale,
          const Matrix& deviceToUser, const Paint& paint) override {
    // The outline of a stroke is a union of overlapping pieces -- one quad per edge, a join per vertex, a cap per open
    // end -- so it must be rasterised with the nonzero rule whatever fill rule the path itself carries. An even-odd
    // stroked line is the case that makes this visible: its pieces overlap in pairs, and parity cancels every one of
    // them. The fill rule of the path answers a question about filling the path, not about filling its outline.
    const bool stroked = paint.style == Paint::Style::kStroke;
    const FillType effectiveFillType = stroked ? raster::strokeFillType() : fillType;
    const std::vector<raster::Contour> contours = stroked
            ? raster::strokePath(devicePath, raster::strokeStyleFor(paint, scale))
            : raster::flattenPath(devicePath);
    if (contours.empty()) return;

    gpu::DrawList::Command command;
    command.kind = gpu::DrawList::Kind::Path;
    command.firstContour = static_cast<uint32_t>(mList->contours.size());
    command.contourCount = static_cast<uint32_t>(contours.size());
    command.clipRect = mDeviceClip;
    command.clipMask = mActiveMask;
    command.layer = mDrawLayer;

    // The contours are copied into the list's flat arrays rather than kept as vectors of vectors, because a
    // batch's vertex data is built by walking one contiguous run of points -- and because a `DrawList` that
    // held `std::vector` per contour would be a list of allocations rather than a list of data.
    Rect bounds = contours.front().bounds();
    for (const raster::Contour& contour: contours) {
      gpu::DrawList::Contour entry;
      entry.firstPoint = static_cast<uint32_t>(mList->points.size());
      entry.pointCount = static_cast<uint32_t>(contour.points.size());
      mList->contours.push_back(entry);
      mList->points.insert(mList->points.end(), contour.points.begin(), contour.points.end());
      const Rect contourBounds = contour.bounds();
      bounds = Rect::MakeLTRB(std::min(bounds.getLeft(), contourBounds.getLeft()),
              std::min(bounds.getTop(), contourBounds.getTop()),
              std::max(bounds.getRight(), contourBounds.getRight()),
              std::max(bounds.getBottom(), contourBounds.getBottom()));
    }

    command.geometry[0] = bounds;

    // ## A blend mode the blend unit cannot express isolates this path in its own layer
    //
    // Opened here rather than at the top of the function because the layer's bounds are the shape's, and the shape's
    // bounds are only known once the path has been flattened -- which is what the loop above just did. `command.layer`
    // is assigned *after* this, so the command is recorded against the new layer rather than against the parent.
    Paint inner;
    const bool layered = beginBlendLayer(bounds, paint, &inner);
    command.layer = mDrawLayer;

    command.state.fillType = effectiveFillType;
    fillState(inner, deviceToUser, Point { bounds.getLeft(), bounds.getTop() }, inner.antiAlias, &command.state);

    // ## A small, solid, antialiased path is recorded as a coverage *field* instead of as a stencil fill
    //
    // `GpuRenderer::drawPath` fills a path by accumulating winding in the stencil and then covering it. The cover pass
    // gates on the stencil -- a boolean -- so **every path edge is a hard step**, which is why SVG outlines and Lottie
    // strokes measured 0 partly-covered pixels against the rasterizer's ~200. Glyphs had the same defect and were fixed
    // by taking them *out* of the path program: `drawGlyphRun` rasterises the outline into an antialiased coverage
    // bitmap and draws it as an image, where sampling interpolates.
    //
    // This is that same idea for a path, but through the definition both rasterisers now share rather than through a
    // bitmap. The field is rasterised by the software rasterizer's own `fillContours` -- so the numbers are the
    // reference's, not an approximation of them -- and drawn through `coverage.frag`, which samples the field at the
    // fragment's device pixel. The bitmap route was the first version of this and is what glyphs still use; a path moved
    // to the field because a field is the *one* definition (`EdgeCoverage.h`) that the CPU reference, the tests and the
    // fragment stage all name, where a bitmap was a third answer resampled by the driver's own filter. The rasterising
    // itself happens in `GpuRenderer::resolveCoverageFields` -- see `recordCoverageField` for why it is not done here.
    //
    // The gates are the same ones the bitmap route was taken under:
    //
    //   * **antialiased** -- a caller that asked for a hard edge gets the stencil, and pays nothing for this;
    //   * **no shader and no colour filter** -- a gradient is sampled through the geometry's inverse transform, which a
    //     sampled coverage field does not carry. Those keep the stencil, which is why the gradient work matters
    //     independently;
    //   * **bounded** -- a field is device-sized, so this is for the shapes a UI actually draws (icons, strokes,
    //     glyph-like outlines) and not for a full-screen background, where rasterising a device-sized mask would cost
    //     more than the aliasing it fixes.
    //
    // ## The gate that is *gone*, and the measurement that kept it for a round
    //
    // An earlier version of this route also required the shape's device bounds to land on integers, because the bitmap
    // it drew was exact only as a 1:1 blit of an integer lattice. Measured without that gate the bitmap route made
    // `path.svg` worse -- 32-63 bucket 1171 to 4361 -- while measurably improving an isolated rotated square from 0 to
    // 85 partly-covered pixels, so the gate was kept and the disagreement recorded as unexplained.
    //
    // It is not needed by the field route and it is not here, because the field's texel *is* the device pixel it covers:
    // `coverage.frag` converts a fragment's device position to a texel index without any snap to a lattice, so a
    // fractional shape reaches the route with its subpixel offset intact inside the coverage values. That is the whole
    // of S1-S3, and it is the reason the fractional case could be re-opened here rather than left gated.
    //
    // ## The size bound, and why it is 1024 rather than the 256 it was
    //
    // The bound exists because the two routes are paid for differently. A field's cost is proportional to the shape's
    // **area**: the CPU rasterises every texel of it through the scanline rasteriser and the atlas has to hold them, which
    // measured about 15 ns a texel on the demo scene. The stencil route's cost is a flat two or three draws whatever the
    // shape's size. A small, complex shape -- an icon, a glyph-like outline, a stroke -- is therefore cheap as a field and
    // expensive as three draws, while a full-screen background is the other way round, and that is the trade this number
    // draws a line through.
    //
    // Where the line goes was measured rather than argued, once the field atlas and the batching had made a field much
    // cheaper than it was when 256 was chosen (4.1.12 and 4.1.13). On the demo scene -- 22 SVG corpus documents, Lottie,
    // text and a panel, 41000 paths -- against the rasteriser's own output:
    //
    //   | bound | draws | stencil paths |  field texels | generation | 2nd, 3rd frame | pixels over the budget |
    //   |------:|------:|--------------:|-------------:|-----------:|---------------:|----------------------:|
    //   |   256 |  8099 |          6595 |      858 KiB |    38.8 ms |        172 ms  |                 32221 |
    //   |  1024 |  1588 |            33 |     3831 KiB |    74.9 ms |        104 ms  |                 30405 |
    //   |  4096 |  1588 |            33 |     3831 KiB |    74.6 ms |        102 ms  |                 30405 |
    //
    // Two things fall out of it. Everything the stencil route was still doing was there because of *this* bound and not
    // because of a shader or a colour filter -- 6595 of the frame's 8099 draws became 33 -- so the frame got 1.7x faster;
    // and the picture got closer to the rasteriser, because those large shapes now have antialiased edges. 1024 is the
    // knee: it reaches what an unbounded route reaches on this scene, while keeping any one shape's field under about a
    // megabyte of atlas (some 15 ms of CPU), which is what the bound is for. A frame of many screen-sized paths would
    // still be slow, and `diagnostics` reports the frame's field area so that it is visible rather than mysterious.
    constexpr Scalar kMaxCoverageExtent = 1024.0f;
    const bool coverage = command.state.antiAlias && !paint.hasShader() && !paint.hasColorFilter() &&
                          bounds.getWidth() <= kMaxCoverageExtent && bounds.getHeight() <= kMaxCoverageExtent;
    if (coverage && recordCoverageField(contours, command.firstContour, bounds, effectiveFillType, inner)) {
      mList->bounds = unite(mList->bounds, bounds);
      if (layered) onRestore();
      return;
    }

    mList->commands.push_back(command);
    mList->bounds = unite(mList->bounds, bounds);
    if (layered) onRestore();
  }

  /**
   * Records @p contours as a **coverage field request**: the field's lattice, and the command that names it.
   *
   * This is what `drawPath` records a small antialiased fill as. The coverage itself is *not* rasterised here -- step S5
   * of the plan moved that to the renderer's resolve stage, which fills every request in the frame on the job pool. So
   * what this writes is a `DrawList::CoverageField` with no texels, and the contours the field is generated from travel
   * in the command's own contour range, which the recorder has already filled in.
   *
   * ## Why the coverage is deferred rather than computed here
   *
   * The record thread is the application's thread, and rasterising a field is the most expensive thing one path does:
   * up to 258x258 = 66k texels through the scanline rasteriser, per path, up to a few hundred paths per frame. Doing it
   * here meant the UI thread did it, one path at a time, inside `drawPath` -- where nothing can be parallelised because
   * the next path has not been recorded yet. Deferring it makes it one explicit stage over the whole frame
   * (`GpuRenderer::resolveCoverageFields`), which is the shape the plan's S5 asks for: generation is CPU work over a
   * known list, and upload-and-render is the single-threaded stage that follows.
   *
   * ## Why the route is not cosmetic
   *
   * The bitmap route it replaced is a *second* implementation of "sample coverage at a device position": GL's bilinear
   * filter over an 8-bit texture, whose convention for where a texel's value lives is a property of the driver rather
   * than of this library. The field route makes that convention explicit -- texel `i` is device pixel `originX + i`,
   * sampled at that pixel's centre -- and the CPU reference, the shader and the tests all name the one expression. It is
   * also exact at a pixel centre, so a path drawn through it is the rasteriser's own coverage rather than a filtered
   * version of it.
   *
   * ## The quad is the field's lattice, and that is not the margin bug it looks like
   *
   * The field is anchored at `floor(bounds) - 1` with a one-pixel margin, and the quad covers the field's whole lattice --
   * margin included. An earlier round read the symptoms that follow from covering the margin (`ASecondFrameReusesTheSurface`
   * reading 64/255 at (1,1), `TheTwoRasterisersPutAnEdgeInTheSameRow` losing the band's last row) as the quad being
   * anchored on the wrong rectangle, and the proposed fix was to quad the shape's own `bounds` instead.
   *
   * It was the sampling, not the quad. Both the shader and the CPU sampler carried a half-texel offset that placed a
   * texel's value on a device pixel *corner*, so a fragment at a pixel centre read the average of that pixel's coverage
   * and its neighbour's -- which is how a margin texel of 0 blended with an interior 255 into 64/255 one pixel outside
   * the shape, and how a fully covered row halved into 0.5. With the offset removed
   * (`EdgeCoverage.SamplingAtAPixelCentreReturnsThatPixelsCoverage`) the margin contributes exactly the coverage the
   * rasteriser computed for it, which is zero wherever the geometry does not reach.
   *
   * Quadring the shape's own `bounds` instead would be actively wrong, and the reason is the rasteriser's own pixel rule:
   * a fragment exists for a pixel whose **centre** is inside the quad. A shape ending at `x = 48.4` has a partly covered
   * pixel 48 whose centre is 48.5, so a quad ending at 48.4 drops it and the shape reads a column thin -- the very defect
   * the one-pixel margin exists to avoid. Measured, by putting that variant in and reading the comparison tests:
   *
   *   | | field lattice (kept) | the shape's own `bounds` |
   *   |:--|--:|--:|
   *   | Lottie `gear.json`, partly covered per moment | **208 / 204 / 202 / 199** | 200 / 196 / 194 / 191 |
   *   | Lottie, pixels over the 32/255 budget per moment | **0** | 8 |
   *   | rotated square, partly covered | **85** (the rasteriser's 85) | 78 |
   *   | rotated square, over budget | **1** | 7 |
   *   | `path.svg`, inked pixels over budget | **916** | 1023 |
   *
   * Eight pixels lost per Lottie moment and seven on the rotated square are the outer partial row and column of each
   * shape, which is exactly what the rule above predicts. The field's lattice costs a margin of transparent overdraw and
   * loses nothing.
   *
   * @return false when the shape cannot be expressed as a field at all -- no contours, or a lattice past the route's
   *         size bound -- in which case the caller falls back to the stencil fill. Generation failing later (an
   *         allocation in the resolve stage) is counted there instead, because by then the command is recorded.
   */
  bool recordCoverageField(const std::vector<raster::Contour>& contours, const uint32_t firstContour,
          const Rect& bounds, const FillType fillType, const Paint& paint) {
    if (contours.empty()) return false;

    // One pixel of margin, because an antialiased edge extends up to a pixel beyond the geometry's own box -- the same
    // reason `drawGlyphRun` adds one. Without it the outermost partial pixels are clipped off and the shape reads thin.
    const int left = static_cast<int>(std::floor(bounds.getLeft())) - 1;
    const int top = static_cast<int>(std::floor(bounds.getTop())) - 1;
    const int right = static_cast<int>(std::ceil(bounds.getRight())) + 1;
    const int bottom = static_cast<int>(std::ceil(bounds.getBottom())) + 1;
    const int width = right - left;
    const int height = bottom - top;
    if (width <= 0 || height <= 0 || width > kMaxGlyphExtent || height > kMaxGlyphExtent) return false;

    // The request, and it is self-describing: the lattice the renderer rasterises into, the fill rule to rasterise with,
    // and the contours to rasterise -- a range in the list's own flat arrays, so the resolve stage needs no reference
    // back to the command that names the field. `texels` stays empty, which is what marks this entry as its work.
    gpu::DrawList::CoverageField request;
    request.originX = left;
    request.originY = top;
    request.width = width;
    request.height = height;
    request.fillType = fillType;
    request.firstContour = firstContour;
    request.contourCount = static_cast<uint32_t>(contours.size());

    // ## The quad covers the same device lattice the field does
    //
    // The field's texel `(i, j)` *is* the device pixel `[left+i, left+i+1) x [top+j, top+j+1)`, because that is what the
    // resolve stage is given as its origin. So the quad is exactly that lattice -- no snap, no shrink to the geometry's
    // own box (see the note on this function for why shrinking it would cost the outer partial pixels) -- and the
    // fragment stage's device-to-texel conversion lands on the pixel's own texel with the shape's subpixel offset
    // already inside the coverage values.
    const Rect quad = Rect::MakeLTRB(static_cast<Scalar>(left), static_cast<Scalar>(top), static_cast<Scalar>(right),
            static_cast<Scalar>(bottom));
    gpu::appendCoverageFieldCommand(*mList, std::move(request), quad, mDeviceClip, mActiveMask, mDrawLayer, paint);
    return true;
  }

  /**
   * Rasterises @p contours into a coverage bitmap at @p bounds and records it as an image.
   *
   * @return true when the image was recorded, false when the caller should fall back to the stencil fill.
   *
   * This is the **glyph** route: a glyph outline is repeated across a run and across frames, so rasterising it once into a
   * bitmap that the content-hashed image cache can serve again is the right trade, and it is why `drawGlyphRun` calls
   * this. A path does the same work every frame with different geometry, so a path goes to `recordCoverageField` above,
   * where the rasteriser's coverage is drawn through the shared definition rather than through a bitmap.
   *
   * The coverage is computed by `raster::fillContours` -- the rasterizer the CPU backend uses -- so the edges are the
   * *same* coverage the reference produces rather than an approximation of it. That is the whole point: the defect this
   * fixes is a disagreement with the rasterizer, and using the rasterizer's own scanline coverage removes the
   * disagreement instead of narrowing it.
   */
  struct CachedGlyph {
    Bitmap bitmap;
    Scalar left = 0.0f;
    Scalar top = 0.0f;
    Scalar right = 0.0f;
    Scalar bottom = 0.0f;
  };

  static constexpr size_t kGlyphCacheLimit = 1024;
  std::unordered_map<uint64_t, CachedGlyph> mGlyphCache;

  bool recordCoverageImage(const std::vector<raster::Contour>& contours, const Rect& bounds, const FillType fillType,
          const Paint& paint, const uint64_t glyphCacheKey = 0) {
    if (contours.empty()) return false;

    // One pixel of margin, because an antialiased edge extends up to a pixel beyond the geometry's own box -- the same
    // reason `drawGlyphRun` adds one. Without it the outermost partial pixels are clipped off and the shape reads thin.
    const int left = static_cast<int>(std::floor(bounds.getLeft())) - 1;
    const int top = static_cast<int>(std::floor(bounds.getTop())) - 1;
    const int right = static_cast<int>(std::ceil(bounds.getRight())) + 1;
    const int bottom = static_cast<int>(std::ceil(bounds.getBottom())) + 1;
    const int width = right - left;
    const int height = bottom - top;
    if (width <= 0 || height <= 0 || width > kMaxGlyphExtent || height > kMaxGlyphExtent) return false;

    if (glyphCacheKey != 0) {
      const auto found = mGlyphCache.find(glyphCacheKey);
      if (found != mGlyphCache.end()) {
        const CachedGlyph& cached = found->second;
        const Point quad[4] = {
          Point { cached.left, cached.top },
          Point { cached.right, cached.top },
          Point { cached.right, cached.bottom },
          Point { cached.left, cached.bottom },
        };
        recordImage(cached.bitmap, quad, Rect::MakeLTRB(cached.left, cached.top, cached.right,
          cached.bottom), SamplingOptions(FilterMode::kLinear), paint);
        return true;
      }
    }

    Bitmap coverage = Bitmap::allocate(ImageInfo(width, height, ColorType::kRGBA_8888, AlphaType::kPremultiplied));
    if (coverage.isEmpty()) return false;
    Pixmap pixels = coverage.lockPixels();
    if (pixels.isEmpty()) return false;

    // The contours are shifted so the shape's box lands at the bitmap's origin. A translation of already-flattened
    // device geometry, not another transform: flattening happens in device space so that the tolerance is a constant in
    // pixels, and transforming afterwards would undo that.
    std::vector<raster::Contour> local = contours;
    for (raster::Contour& contour: local) {
      for (Point& point: contour.points) {
        point.x -= static_cast<Scalar>(left);
        point.y -= static_cast<Scalar>(top);
      }
    }

    // White at the coverage, premultiplied -- so the alpha channel *is* the coverage and the paint's colour is applied
    // by the image command, which is what lets one shape serve any colour.
    raster::fillContours(local, pixels, fillType,
            [&pixels](const int x, const int y, const uint32_t value) {
              if (value == 0) return;
              const uint32_t alpha = value > 255u ? 255u : value;
              pixels.setPixelARGB32(x, y, (alpha << 24) | (alpha << 16) | (alpha << 8) | alpha);
            });

    // ## The quad carries the subpixel offset
    //
    // The bitmap is anchored at the integer `left`/`top` and the contours were shifted by exactly that, so bitmap pixel
    // `(i, j)` is the unit square `[left+i, left+i+1) x [top+j, top+j+1)` of device space -- the two grids coincide.
    // The quad therefore covers the *same* integer lattice the bitmap does, and the shape's subpixel offset is already
    // inside the coverage values. Placing it at the shape's own fractional `bounds` instead would apply that offset a
    // second time and shift the shape by up to a pixel.
    //
    // This is the whole reason the offset does not need a gate: nothing is snapped, because nothing needed to be. An
    // earlier version placed the quad at `left` while the shapes were only used when aligned, which hid the question
    // rather than answering it.
    const Point quad[4] = {
      Point { static_cast<Scalar>(left), static_cast<Scalar>(top) },
      Point { static_cast<Scalar>(right), static_cast<Scalar>(top) },
      Point { static_cast<Scalar>(right), static_cast<Scalar>(bottom) },
      Point { static_cast<Scalar>(left), static_cast<Scalar>(bottom) },
    };
    if (glyphCacheKey != 0) {
      if (mGlyphCache.size() >= kGlyphCacheLimit) mGlyphCache.clear();
      mGlyphCache.emplace(glyphCacheKey, CachedGlyph { coverage,
        static_cast<Scalar>(left), static_cast<Scalar>(top), static_cast<Scalar>(right),
        static_cast<Scalar>(bottom) });
    }
    recordImage(coverage, quad, Rect::MakeLTRB(left, top, right, bottom), SamplingOptions(FilterMode::kLinear), paint);
    return true;
  }

  void drawBitmap(const Bitmap& image, const Point deviceQuad[4], const Rect& deviceBounds,
          const Matrix& deviceToUser, const SamplingOptions& sampling, const Paint& paint) override {
    if (image.isEmpty()) return;
    // sampling is not carried: the DrawList image command has no field for it yet, so a recorded draw samples with
    // the renderer's fixed mode. Stated here rather than ignored silently -- see the note in Canvas::drawBitmap.
    recordImage(image, deviceQuad, deviceBounds, sampling, paint);
  }

  void drawBitmapRect(const Bitmap& image, const Rect& source, const Point deviceQuad[4],
      const Rect& deviceBounds, const Matrix& /*deviceToUser*/, const SamplingOptions& sampling,
      const Paint& paint) override {
    if (image.isEmpty() || source.isEmpty()) return;
    recordImage(image, deviceQuad, deviceBounds, sampling, paint, false, source);
  }

  /**
   * Records a command that samples a texture another producer owns.
   *
   * The command is the same shape an image draw is -- four device-space corners and normalized texture
   * coordinates -- so the renderer's image batching, clip handling and layer compositing all apply.
   * What differs is only where the texture comes from: this entry appends to `externalImages` instead
   * of registering pixels, so nothing is uploaded.
   */
  void drawExternalImage(const ExternalImage& image, const Point deviceQuad[4],
      const Rect& deviceBounds, const SamplingOptions& sampling, const Paint& paint) override {
    if (!image.isValid()) return;

    // A blend mode the blend unit cannot express is isolated in a layer, exactly as a bitmap draw is:
    // the mode then applies to the group once instead of to each source pixel.
    Paint inner;
    const bool layered = beginBlendLayer(deviceBounds, paint, &inner);

    mList->externalImages.push_back(image);
    const uint32_t imageIndex = static_cast<uint32_t>(mList->externalImages.size() - 1);

    gpu::DrawList::Command command;
    command.kind = gpu::DrawList::Kind::ExternalImage;
    command.sampling = sampling;
    for (int index = 0; index < 4; ++index) {
      command.geometry[index] = Rect::MakeLTRB(deviceQuad[index].x, deviceQuad[index].y,
        deviceQuad[index].x, deviceQuad[index].y);
    }
    // The whole texture is the source, so the coordinates are the unit square rather than an atlas
    // sub-rectangle. A producer that wants a sub-rectangle records one command per rectangle.
    command.texCoords[0] = Point { 0.0f, 0.0f };
    command.texCoords[1] = Point { 1.0f, 0.0f };
    command.texCoords[2] = Point { 1.0f, 1.0f };
    command.texCoords[3] = Point { 0.0f, 1.0f };
    command.clipRect = mDeviceClip;
    command.clipMask = mActiveMask;
    command.layer = mDrawLayer;
    command.state.shader = gpu::DrawList::ShaderKind::Image;
    command.state.image = imageIndex;
    command.state.blendMode = inner.blendMode;
    command.state.channelMask = inner.channelMask;
    command.state.red = clampUnit(inner.color.red);
    command.state.green = clampUnit(inner.color.green);
    command.state.blue = clampUnit(inner.color.blue);
    command.state.alpha = clampUnit(inner.color.alpha);

    mList->commands.push_back(command);
    mList->bounds = unite(mList->bounds, deviceBounds);
    if (layered) onRestore();
  }

  /**
   * Registers @p image once per recording and returns the existing index for the same pixels.
   *
   * A glyph atlas draws every glyph with the same page; without this dedupe each glyph would append a copy and the
   * renderer would place/hash the page once per glyph instead of once per line. The key is the pixel pointer plus
   * geometry, because a mutable atlas grows in place and a pointer identifies that backing store exactly.
   */
  uint32_t registerImageCached(const Bitmap& image) {
    const void* bytes = image.data().bytes();
    const uint64_t key = reinterpret_cast<uintptr_t>(bytes) ^
      (static_cast<uint64_t>(image.data().size()) << 1) ^
      (static_cast<uint64_t>(image.width()) << 17) ^
      (static_cast<uint64_t>(image.height()) << 33) ^
      (static_cast<uint64_t>(image.rowBytes()) << 49);
    if (const auto found = mImageIndices.find(key); found != mImageIndices.end()) return found->second;
    mList->images.push_back(image);
    mList->imageHashes.push_back(0);
    const uint32_t index = static_cast<uint32_t>(mList->images.size() - 1);
    mImageIndices.emplace(key, index);
    return index;
  }

  /**
   * Records an image command from a bitmap and a *device-space* quad.
   *
   * Extracted so that `drawBitmap` -- which is handed a quad by the canvas -- and `drawGlyphRun` -- which derives one
   * from a glyph's device bounding box -- produce the same command. The alternative was to have the glyph path call
   * `Canvas::drawBitmap`, and that maps device coordinates through the canvas transform a second time, because the
   * outline it starts from is already in device space.
   */
  void recordImage(const Bitmap& image, const Point deviceQuad[4], const Rect& deviceBounds,
    const SamplingOptions& sampling, const Paint& paint, const bool sdf = false,
    const Rect& sourceRect = Rect{}) {
    if (image.isEmpty()) return;

    // A blend mode above `kScreen` isolates this image in a layer, for the reason `beginBlendLayer` gives. `inner` is
    // what the image command is recorded with, and the layer's composite is what applies the mode.
    Paint inner;
    const bool layered = beginBlendLayer(deviceBounds, paint, &inner);

    // The image is copied into the list rather than referenced, because a `DrawList` outlives the call that
    // produced it and a caller's bitmap does not have to: `Canvas::drawBitmap` is documented as taking the
    // picture by value for exactly this reason, and a recording that pointed at the caller's pixels would be
    // a race the moment the recording crossed a thread boundary.
    // ## The colour filter, which for an image is a pass over its pixels
    //
    // Recorded here rather than left to the renderer because it is the same place the rasterizer does it and the same
    // arithmetic (`ColorFilter::applyToPremultipliedArgb`), so the two backends agree exactly. See `applyColorFilter`
    // for why the fragment stage cannot carry it. `hasColorFilter` already answers "and is it not the identity", so an
    // identity filter costs nothing.
    const Bitmap pixels = inner.hasColorFilter() ? applyColorFilter(image, *inner.colorFilter) : image;

    gpu::DrawList::Command command;
    command.kind = gpu::DrawList::Kind::Image;
    command.sampling = sampling;

    // ## How four corners and four texture coordinates are stored
    //
    // `Command` carries four `Rect`s for geometry because that is what a rectangle and a layer region need,
    // and a quad reuses them as four points: corner *i* is `(geometry[i].Left, geometry[i].Top)`. That is
    // worth stating rather than leaving to be inferred -- `Rect` is not a point, and a reader who assumes the
    // last two fields are the far corner will compute a bounding box that is wrong by exactly the offset the
    // corners carry.
    for (int index = 0; index < 4; ++index) {
      command.geometry[index] = Rect::MakeLTRB(deviceQuad[index].x, deviceQuad[index].y,
              deviceQuad[index].x, deviceQuad[index].y);
    }

    // The texture coordinates are normalized image coordinates, not a unit basis: a full-image draw is (0,0)..(1,1),
    // while an atlas sub-rectangle carries its own u/v range. The quad corners supply the destination geometry.
    const Rect source = sourceRect.isEmpty()
      ? Rect::MakeWH(static_cast<float>(image.width()), static_cast<float>(image.height()))
      : sourceRect;
    const float imageWidth = std::max(1.0f, static_cast<float>(image.width()));
    const float imageHeight = std::max(1.0f, static_cast<float>(image.height()));
    const float u0 = source.getLeft() / imageWidth;
    const float v0 = source.getTop() / imageHeight;
    const float u1 = source.getRight() / imageWidth;
    const float v1 = source.getBottom() / imageHeight;
    command.texCoords[0] = Point { u0, v0 };
    command.texCoords[1] = Point { u1, v0 };
    command.texCoords[2] = Point { u1, v1 };
    command.texCoords[3] = Point { u0, v1 };

    command.clipRect = mDeviceClip;
    command.clipMask = mActiveMask;
    command.layer = mDrawLayer;
    // An image draw is an image whatever the paint says, because the *shape* is what carries the sampling here. The
    // paint's colour and alpha still apply, and they are the only parts of the state an image uses.
    //
    // `fillState` is deliberately not called: it needs the device-to-user matrix to fill in a gradient's sampling
    // matrix, and neither this entry point nor `drawGlyphRun` has one to give -- a bitmap carries its own sampling and
    // a glyph is rasterised coverage. Calling it with an identity would put a wrong matrix in a block nothing reads.
    command.state.shader = gpu::DrawList::ShaderKind::Image;
    command.state.sdf = sdf;
    command.state.sdfOutline = paint.sdfOutlineWidth;
    command.state.sdfShadowX = paint.sdfShadowX;
    command.state.sdfShadowY = paint.sdfShadowY;
    command.state.sdfGlow = paint.sdfGlow;
    command.state.sdfOutlineColor = paint.sdfOutlineColor;
    command.state.sdfShadowColor = paint.sdfShadowColor;
    command.state.sdfPattern = paint.sdfPattern;
    command.state.sdfPatternFrequency = paint.sdfPatternFrequency;
    command.state.sdfPatternAngle = paint.sdfPatternAngle;
    command.state.sdfPatternStrength = paint.sdfPatternStrength;
    command.state.sdfFillAlpha = paint.sdfFillAlpha;
    command.state.blendMode = inner.blendMode;
    command.state.channelMask = inner.channelMask;
    command.state.red = clampUnit(inner.color.red);
    command.state.green = clampUnit(inner.color.green);
    command.state.blue = clampUnit(inner.color.blue);
    command.state.alpha = clampUnit(inner.color.alpha);

    command.state.image = registerImageCached(pixels);

    mList->commands.push_back(command);
    mList->bounds = unite(mList->bounds, deviceBounds);
    if (layered) onRestore();
  }


  void recordSdfGlyph(const pathops::Path& deviceOutline, const Paint& paint,
      const uint64_t glyphCacheKey) {
    if (glyphCacheKey != 0) {
      {
        const std::lock_guard<std::mutex> lock(gSdfCacheMutex);
        const auto found = gSdfCache.find(glyphCacheKey);
        if (found != gSdfCache.end()) {
          const SharedSdfGlyph& cached = found->second;
          const Point quad[4] = {
            Point { cached.left, cached.top }, Point { cached.right, cached.top },
            Point { cached.right, cached.bottom }, Point { cached.left, cached.bottom },
          };
          recordImage(*cached.bitmap, quad, Rect::MakeLTRB(cached.left, cached.top, cached.right,
            cached.bottom), SamplingOptions(FilterMode::kLinear), paint, true);
          return;
        }
      }
    }

    const graphics::SdfGlyph field =
      graphics::makeSdfGlyph(graphics::Path::fromExternalHandle(&deviceOutline), 6.0f);
    if (field.isEmpty()) return;

    Bitmap bitmap = Bitmap::allocate(static_cast<int>(field.width), static_cast<int>(field.height));
    Pixmap pixels = bitmap.lockPixels();
    if (pixels.isEmpty()) return;
    for (uint32_t y = 0; y < field.height; ++y) {
      for (uint32_t x = 0; x < field.width; ++x) {
        const uint8_t d = field.distance[static_cast<size_t>(y) * field.width + x];
        pixels.setPixelARGB32(static_cast<int>(x), static_cast<int>(y),
          (0xFFu << 24) | (d << 16) | (d << 8) | d);
      }
    }

    SharedSdfGlyph entry;
    entry.bitmap = std::make_shared<Bitmap>(std::move(bitmap));
    entry.left = field.left;
    entry.top = field.top;
    entry.right = field.left + static_cast<float>(field.width);
    entry.bottom = field.top + static_cast<float>(field.height);
    if (glyphCacheKey != 0) {
      const std::lock_guard<std::mutex> lock(gSdfCacheMutex);
      if (gSdfCache.size() >= kGlyphCacheLimit) gSdfCache.clear();
      gSdfCache.emplace(glyphCacheKey, entry);
    }
    const Point quad[4] = {
      Point { entry.left, entry.top }, Point { entry.right, entry.top },
      Point { entry.right, entry.bottom }, Point { entry.left, entry.bottom },
    };
    recordImage(*entry.bitmap, quad, Rect::MakeLTRB(entry.left, entry.top, entry.right, entry.bottom),
      SamplingOptions(FilterMode::kLinear), paint, true);
  }

  void drawGlyphRun(const pathops::Path& deviceOutline, const Paint& paint,
    const uint64_t glyphCacheKey = 0) override {
    if (paint.sdfText) {
      recordSdfGlyph(deviceOutline, paint, glyphCacheKey);
      return;
    }
    // ## Why a glyph is not drawn as a path
    //
    // It was, and that is the whole of the text-quality defect: this renderer's path coverage is a binary stencil test,
    // so every glyph edge was a step from background to ink with nothing in between. Measured on `Hamburgefonstiv` at 28
    // device pixels: the rasterizer produced **708** partly-covered pixels along the glyph edges and this path produced
    // **0**, while both drew the same text.
    //
    // The fix is the one every production GPU text path uses, and Skia's Graphite is the reference: rasterise the glyph
    // into a coverage bitmap with an antialiased rasterizer -- which is what `raster::fillContours` already is -- and
    // then draw that bitmap as an image. An image sample interpolates, so the antialiased edge survives, and the path
    // program is never involved.
    //
    // The outline arrives in **device** space, because the canvas has already applied the glyph's own placement and the
    // caller's transform. So the bitmap is allocated at the outline's device bounding box and the command records device
    // corners directly.
    //
    // The rasterisation itself is `recordCoverageImage`, which is the same construction a path used before it moved to
    // the coverage *field*: flatten, bound with a one-pixel margin, fill through the rasterizer, draw as an image. It was
    // written out twice -- once here and once there -- and one of the two copies is how a fix reaches a glyph and not a
    // path. Glyphs keep the bitmap rather than moving to the field for the reason the plan gives: a glyph is repeated
    // within a run and across frames, so its bitmap is cached by content and uploaded once, while a path is different
    // every frame.
    const std::vector<raster::Contour> contours = raster::flattenPath(deviceOutline);
    if (contours.empty()) return;

    Rect bounds = contours.front().bounds();
    for (const raster::Contour& contour: contours) {
      const Rect contourBounds = contour.bounds();
      bounds = Rect::MakeLTRB(std::min(bounds.getLeft(), contourBounds.getLeft()),
              std::min(bounds.getTop(), contourBounds.getTop()),
              std::max(bounds.getRight(), contourBounds.getRight()),
              std::max(bounds.getBottom(), contourBounds.getBottom()));
    }
    if (bounds.getRight() <= bounds.getLeft() || bounds.getBottom() <= bounds.getTop()) return;

    recordCoverageImage(contours, bounds, FillType::Winding, paint, glyphCacheKey);
  }

  // --- GPU-only -----------------------------------------------------------------------------------

  [[nodiscard]] gpu::ContextImpl* gpuContext() noexcept override { return mContext.get(); }

  bool flush() override { return true; }

  bool finish() override { return true; }

  [[nodiscard]] Bitmap readback() override {
    // The backend has nothing to read: the pixels belong to the render target the surface owns, and the
    // surface is what submits the list and reads the result. An empty bitmap is the honest answer, and it is
    // what tells the surface to go through its own context rather than trusting this one.
    return Bitmap {};
  }

  /** @return the list recorded so far, which is what the surface submits. */
  [[nodiscard]] const std::shared_ptr<gpu::DrawList>& drawList() const noexcept { return mList; }

private:
  /** @return the intersection of @p a and @p b, which is the clip a command is recorded under. */
  [[nodiscard]] static Rect intersect(const Rect& a, const Rect& b) noexcept {
    const Scalar left = std::max(a.getLeft(), b.getLeft());
    const Scalar top = std::max(a.getTop(), b.getTop());
    const Scalar right = std::min(a.getRight(), b.getRight());
    const Scalar bottom = std::min(a.getBottom(), b.getBottom());
    // A degenerate rectangle rather than an empty one: `Rect` has no empty state, and a zero-area rectangle
    // is the same reject test everywhere it is used.
    return Rect::MakeLTRB(left, top, std::max(left, right), std::max(top, bottom));
  }

  /** @return the smallest rectangle containing @p a and @p b, or @p b when @p a has no area. */
  [[nodiscard]] static Rect unite(const Rect& a, const Rect& b) noexcept {
    const bool aEmpty = a.getRight() <= a.getLeft() || a.getBottom() <= a.getTop();
    if (aEmpty) return b;
    return Rect::MakeLTRB(std::min(a.getLeft(), b.getLeft()), std::min(a.getTop(), b.getTop()),
            std::max(a.getRight(), b.getRight()), std::max(a.getBottom(), b.getBottom()));
  }

  void recordCloseLayer(const uint32_t layerIndex) {
    gpu::DrawList::Command command;
    command.kind = gpu::DrawList::Kind::CloseLayer;
    command.geometry[0] = mList->layers[layerIndex].bounds;
    command.clipRect = mDeviceClip;
    command.layer = layerIndex;
    // The composite's blend mode and its global alpha.
    //
    // The alpha comes from the layer's own `OpenLayer` command, not from a value written here. `saveLayerAlpha`
    // carries the caller's alpha on the paint it passes to `beginLayer`, and `OpenLayer` is the only command that
    // records it -- so a close that wrote `1.0` (which this used to do) made every `saveLayerAlpha` composite at full
    // opacity. A half-transparent group came out opaque, and that is what three rectangles at `alpha 128` measured.
    //
    // The blend mode comes from the layer for exactly the same reason and was the same defect one field over: this line
    // used to write `kSrcOver` unconditionally, so a layer opened with `kMultiply` -- or any other mode above `kScreen`,
    // which is every mode the blend unit cannot express -- composited as source-over. The mode is not something the
    // recorder can decide, because only the composite has both the group's pixels and the parent's.
    command.state.blendMode = mList->layers[layerIndex].blendMode;
    // The layer's own alpha, recorded where the layer is recorded rather than here.
    //
    // `fillState` sets `alpha` to 1.0 and only takes the paint's alpha from a *shader*'s colour, so the `OpenLayer`
    // command -- which is filled by `fillState` -- carries no alpha for a plain `saveLayerAlpha`. That is why this
    // reads the value `beginLayer` was handed from the layer itself rather than from the open command's state.
    command.state.alpha = mList->layers[layerIndex].alpha;
    mList->commands.push_back(command);
  }

  std::shared_ptr<gpu::ContextImpl> mContext;
  std::shared_ptr<gpu::DrawList> mList;
  /** State saved while a device-space recording scope is active. */
  std::shared_ptr<gpu::DrawList> mScopeSavedList;
  std::unordered_map<uint64_t, uint32_t> mScopeSavedImages;
  uint32_t mScopeSavedLayer = 0;
  uint32_t mScopeSavedMask = gpu::DrawList::kNoClipMask;
  size_t mScopeSavedStack = 0;
  bool mScopeActive = false;
  /** Image registrations for this recording, keyed by backing pixels and geometry. */
  std::unordered_map<uint64_t, uint32_t> mImageIndices;

  Rect mDeviceBounds {};
  Rect mDeviceClip {};
  /** The layer new commands are recorded into: an index into `mList->layers`. */
  uint32_t mDrawLayer = 0;
  /** The shaped clip in force, or `gpu::DrawList::kNoClipMask`. */
  uint32_t mActiveMask = gpu::DrawList::kNoClipMask;
  /**
   * One entry per saved state: the layer it was recorded in, and whether it opened a layer of its own.
   *
   * Two fields rather than one because save and saveLayer arrive through different calls and only the
   * second one has something to close -- see the note on onSave. The mark is what makes that distinction
   * available at the restore that needs it.
   */
  struct SavedState {
    uint32_t layer = 0;
    bool openedLayer = false;
    /** The device clip in force when this entry was pushed, restored by `onRestore`. */
    Rect deviceClip {};
    /** The shaped clip in force when this entry was pushed, restored by `onRestore`. */
    uint32_t activeMask = gpu::DrawList::kNoClipMask;
  };

  std::vector<SavedState> mSavedStack;
};

} // namespace anonymous

std::unique_ptr<CanvasBackend> createGraphiteCanvasBackend(const std::shared_ptr<gpu::ContextImpl>& context,
        const std::shared_ptr<gpu::DrawList>& existing) {
  // A null context is *not* refused. This backend's job is to fill a list, and it never dereferences the
  // context while recording -- so a caller that only wants the command stream, a test being the obvious one,
  // can have it without a device. Refusing here would make the recording path untestable without hardware,
  // which is the opposite of what separating recording from submission is for. The context is needed at
  // *submission*, and that is the surface's business rather than this factory's.

  // The surface may own the list -- a frame with several recorders needs it to, because it could not submit a
  // list it cannot reach -- so the caller's list is used when there is one and a fresh one is made otherwise.
  // A `DrawList` is cheap to make and a frame that does not need one should not be handed somebody else's.
  auto list = existing != nullptr ? existing : std::make_shared<gpu::DrawList>();
  return std::make_unique<GraphiteCanvasBackend>(context, std::move(list));
}

const std::vector<Bitmap>& recordedImages(const gpu::DrawList* list) {
  // The list owns its bitmaps, so this is a read of a member rather than a lookup in a table beside it. The empty
  // vector for a null list is a real answer, not a placeholder: a caller with no list has no images.
  static const std::vector<Bitmap> kNone;
  return list == nullptr ? kNone : list->images;
}

void releaseRecordedImages(const gpu::DrawList* list) {
  // Nothing to do. The bitmaps live on the list and are freed with it, so the call that used to erase a global map
  // entry has no counterpart here. It is kept because the surface's submit path reads as a pair -- gather, then
  // release -- and removing half of a pair is how the other half gets forgotten at the next frame boundary.
  (void)list;
}

} // namespace graphics



