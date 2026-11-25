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

#ifndef CORAL_GPU_DRAW_LIST_H
#define CORAL_GPU_DRAW_LIST_H

#include <backend/Handles.h>

#include <gpu/Layer.h>

#include <geometry/Point.h>
#include <geometry/Rect.h>
#include <graphics/Bitmap.h>
#include <graphics/Enums.h>
#include <graphics/ExternalImage.h>
#include <graphics/Paint.h>
#include <graphics/SamplingOptions.h>

#include <cstdint>
#include <string>
#include <vector>

namespace graphics::gpu {

/**
 * The intermediate representation one recording thread produces.
 *
 * ## Why a draw list rather than a command buffer
 *
 * The RHI's `backend::CommandBuffer` is a *stream*: it records driver-level state changes and draws in
 * the order a caller issues them, so a renderer that wants to choose an order -- which is every
 * renderer, because the cheapest draw is the one that changes no state -- cannot use it directly. A
 * `DrawList` is the level above: it holds drawing *intent* and leaves the decision about how to realise
 * each item, and in what order, to the pass that consumes it. That split is Skia Graphite's, and it is
 * why Graphite can batch across a whole frame where a renderer that recorded straight into a command
 * buffer can only batch between two calls.
 *
 * ## The stages a draw list is consumed in
 *
 * | stage | thread | what it does |
 * |---|---|---|
 * | `precompile` | worker threads | splits the list into layers and batches, resolves each batch's pipeline state, and tessellates the paths that need it |
 * | `upload` | submit thread | writes the batches' vertex data and the clip masks into RHI resources |
 * | `render` | submit thread | walks the batches in order and records them into one command buffer |
 *
 * The split is the point: `precompile` is pure CPU work over data no other thread touches, which is
 * exactly the part that parallelises, while `render` touches the one GL context and cannot. A renderer
 * that fused them would have nothing to parallelise at all.
 *
 * ## Threading and ownership
 *
 * A draw list is filled by exactly one thread -- the `Recorder` that owns it -- and is then *moved* to
 * the submit thread, which is the only thread that may read it afterwards. Nothing here shares mutable
 * state and nothing takes a lock: the concurrency lives in the hand-off, not inside the type.
 *
 * ## Coordinates
 *
 * Every rectangle and point is in **device pixels**, origin top-left, y increasing downwards. Caller
 * space geometry is transformed by the `Recorder` as a command is issued, and a path is flattened
 * *after* that transform, because a curve's flattening tolerance is a pixel count.
 *
 * ## Backend independence
 *
 * Nothing here names a backend, an RHI handle or a driver object. A bitmap is named by an `ImageId` the
 * surface keeps the mapping for, and every other field is a plain value. That is what would let this
 * representation be replayed by a second backend, or written to a file, without any code above it
 * changing -- and it is the property a GUI toolkit built on top needs.
 */
struct DrawList {
  /** Discriminator for a recorded command. */
  enum class Kind : uint8_t {
    /**
     * A device-space rectangle filled with the command's shader.
     *
     * Its own kind rather than a four-point path because a rectangle is by far the most common shape a
     * UI draws -- every background, border and clipping band -- and the fast path for one is a single
     * instanced quad with no tessellation and no coverage pass.
     */
    Rect,
    /**
     * A path, already flattened into contours by the recorder.
     *
     * Flattening happens at record time because it is the most expensive CPU step in the pipeline and it
     * depends only on the path and the transform, both of which the recording thread already owns.
     */
    Path,
    /**
     * A device-space quadrilateral sampling a bitmap.
     *
     * Four corners rather than a rectangle because that is what carries a rotated or skewed transform:
     * a bounding box would lose the mapping, and the rasterizer's inverse lookup is the same information
     * written the other way round.
     */
    Image,
    /**
     * A `Rect`/`Path` that is also the source of the *next* commands' offscreen pixels.
     *
     * Not drawn on its own: it opens a layer, and the commands after it are recorded into that layer
     * until the matching `CloseLayer`. Recorded as a command rather than as a field on every command so
     * that a batch can be built by walking the list once, in order, with no second index to consult.
     */
    OpenLayer,
    /**
     * Closes the innermost open layer and composites it onto its parent.
     *
     * `Command::state` carries the composite's alpha and blend mode, and `Command::geometry[0]` the
     * device-space region to composite -- which is a rectangle because whatever shape the layer's
     * contents had is already baked into its pixels.
     */
    CloseLayer,
    /**
     * A device-space quadrilateral sampling a texture this recording did **not** create.
     *
     * The texture belongs to another producer on the same device -- an effect graph's final frame, a
     * video decoder's output, a UI canvas handed between windows -- and the renderer binds its view as
     * the batch's texture instead of uploading a bitmap. That is what lets a page composite a live
     * preview with no readback: the pixels never leave the GPU.
     *
     * `CommandState::image` indexes `externalImages` for this kind, and the geometry, texture
     * coordinates and sampling are laid out exactly as `Image`'s, so the image program draws it.
     *
     * ## Lifetime is the recorder's promise
     *
     * The view must stay valid until the frame that names it has finished on the GPU. A producer that
     * reuses a ring of textures (the usual arrangement) must therefore not recycle a slot before the
     * consumer's submission has completed -- which is what `TextureOutputNode`'s ring does by keeping
     * its slots in flight.
     */
    ExternalImage,
  };

  /** How a command's colour varies. */
  enum class ShaderKind : uint8_t {
    /** One colour for the whole command. */
    Solid,
    /** A linear gradient between two or more stops. */
    LinearGradient,
    /** A radial gradient. */
    RadialGradient,
    /** The bitmap named by `Command::image`, sampled through the command's texture coordinates. */
    Image,
    /** A bitmap's alpha sampled for coverage, which is what a shaped clip and a mask are. */
    Coverage,
  };

  /**
   * A gradient's sampling geometry.
   *
   * `points` and `radius` are in *device* space, because that is where the shape was flattened, while
   * the six coefficients are the inverse of the shader's local matrix composed with the device
   * transform: a fragment evaluating a gradient in the shape's own coordinates needs the transform that
   * maps a device pixel back into them, and computing a matrix inverse per fragment would be an inverse
   * in the inner loop. The recorder takes it once.
   *
   * Six scalars rather than a `Matrix` so that a batch's uniform block is a plain float array with no
   * padding question, and so that a serialised form has no ABI to agree on.
   */
  struct GradientInfo {
    /** Linear: the gradient's start and end. Radial: the centre and a point at its radius. */
    Point points[2] = { Point { 0.0f, 0.0f }, Point { 1.0f, 0.0f } };
    /** Radial gradient radius, or 0 for a linear one. */
    float radius = 0.0f;
    /** Inverse local-matrix coefficients, row major: (m00, m01, m10, m11, tx, ty). */
    float inverseLocal[6] = { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
    /** How the gradient behaves outside its own domain. */
    TileMode tileMode = TileMode::kClamp;
  };

  /** One gradient stop, unpacked so the shader's uniform layout is what it says it is. */
  struct Stop {
    float offset = 0.0f;
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float alpha = 1.0f;
  };

  /** One contour of a flattened path, in device pixels. */
  struct Contour {
    /** Index of this contour's first point in `DrawList::points`. */
    uint32_t firstPoint = 0;
    uint32_t pointCount = 0;
  };

  /** Everything that decides *how* a command is drawn, and nothing about where. */
  struct CommandState {
    ShaderKind shader = ShaderKind::Solid;
    GradientInfo gradient;
    /** Up to four stops. A linear or radial gradient uses the first two at least. */
    Stop stops[4] = {};
    uint32_t stopCount = 0;

    /**
     * The colour a `Solid` command draws, or the alpha a `Coverage` command paints with.
     *
     * Non-premultiplied, as `Paint::color` is; the shader premultiplies. Storing it here rather than
     * resolving it into the vertex data at record time is deliberate: a batch's colour is per *command*,
     * while its vertex data is per instance, and a batch holds many commands.
     */
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float alpha = 1.0f;

    BlendMode blendMode = BlendMode::kSrcOver;
    /**
     * Destination colour channels this command may write, bit 0 = red ... bit 3 = alpha.
     *
     * It is part of the command state because the pipeline that draws it carries the mask; two
     * otherwise identical image commands with different masks cannot share a draw call.
     */
    uint8_t channelMask = 0x0F;
    FillType fillType = FillType::Winding;
    /** Whether the command's edges are feathered. A paint may ask for a hard edge. */
    bool antiAlias = true;
    /** Which bitmap to sample, or `kNoImage`. */
    uint32_t image = kNoImage;
    /** Whether the sampled bitmap is a signed distance field rather than coverage/colour. */
    bool sdf = false;
    /** SDF styling: outline width, shadow offset, glow radius. */
    float sdfOutline = 0.0f;
    float sdfShadowX = 0.0f;
    float sdfShadowY = 0.0f;
    float sdfGlow = 0.0f;
    uint32_t sdfOutlineColor = 0xFF000000u;
    uint32_t sdfShadowColor = 0xFF000000u;
    float sdfPattern = 0.0f;
    float sdfPatternFrequency = 8.0f;
    float sdfPatternAngle = 0.0f;
    float sdfPatternStrength = 0.0f;
    float sdfFillAlpha = 1.0f;

    /**
     * Which coverage field to sample, or `kNoCoverageField`.
     *
     * A `Coverage` command names a field the same way an `Image` command names a bitmap, and the two indices are
     * independent because a command is one or the other. The field is what makes an edge antialiased on the GPU: the
     * fragment stage samples it bilinearly, which is the definition shared with the software rasterizer -- see
     * `覆盖率` in `路径边缘抗锯齿设计与现状.md` and the S1-S3 steps in it.
     */
    uint32_t coverageField = kNoCoverageField;

    friend bool operator==(const CommandState& a, const CommandState& b) noexcept;
    friend bool operator!=(const CommandState& a, const CommandState& b) noexcept { return !(a == b); }
  };

  /** One recorded command: how to draw it, where, and under what clip. */
  struct Command {
    Kind kind = Kind::Rect;
    CommandState state;

    /**
     * Device-space geometry.
     *
     * `Rect` and `CloseLayer`: `[0]` is the rectangle. `Image`: the four corners in the order top-left,
     * top-right, bottom-right, bottom-left, paired index for index with `texCoords`. `Path`: `[0]` is
     * the path's bounding box, which sizes the coverage pass and is the reject test. `OpenLayer`: `[0]`
     * is the layer's region.
     */
    Rect geometry[4] = {};

    /** Texture coordinates for `Kind::Image`, matching `geometry` index for index. */
    Point texCoords[4] = {};

    /** Where this command's contours start in `contours`. */
    uint32_t firstContour = 0;
    uint32_t contourCount = 0;

    /** The clip in force, as a device-space rectangle: the reject test every command shares. */
    Rect clipRect = {};
    /** Index into `clipMasks`, or `kNoClipMask` when the clip is rectangular. */
    uint32_t clipMask = kNoClipMask;

    /** Which layer this command draws into: an index into `layers`. */
    uint32_t layer = 0;

    /**
     * How `Kind::Image` samples its image.
     *
     * Per command rather than per renderer, because it is a property of the *draw*: the same bitmap drawn nearest once
     * and linear once has to look the way each call asked.
     *
     * @warning changing the size of this struct requires a **clean rebuild**. An incremental build can leave some
     *          translation units laid out for the old size and others for the new one, and the resulting binary corrupts
     *          recordings in ways that look like a logic bug in the recorder -- which is exactly what a probe field here
     *          produced twice before a full rebuild showed it to be an ODR problem.
     */
    graphics::SamplingOptions sampling = {};
  };

  /** A shaped clip's coverage, one byte per device pixel. */
  struct ClipMask {
    uint32_t firstPixel = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    /** The device-space rectangle the mask covers, at 1:1 with its pixels. */
    Rect bounds = {};
    /** Stable content key (path + fill + device clip), so a replay can skip re-hashing pixels. */
    uint64_t contentHash = 0;
  };

  /** The commands, in paint order. Order is never rearranged; only the *batching* of them is. */
  std::vector<Command> commands;
  /** Every command's contours, concatenated. Indexed by `Command::firstContour`. */
  std::vector<Contour> contours;
  /** Every path point, concatenated. Indexed by `Contour::firstPoint`. */
  std::vector<Point> points;
  /** Coverage for the shaped clips, concatenated. Indexed by `ClipMask::firstPixel`. */
  std::vector<uint8_t> maskPixels;
  /** The shaped clips, in the order they were recorded. */
  std::vector<ClipMask> clipMasks;

  /**
   * The layers this list renders into, in the order they must be created.
   *
   * Index 0 is always the list's root target, so `Command::layer` needs no sentinel for "no layer" and a
   * layer index is stable for the life of the list. A layer is a *description* rather than a resource:
   * the pass decides what to allocate for one, which is what keeps a `DrawList` replayable by a backend
   * that has no textures at all.
   */
  std::vector<Layer> layers;

  /** Bounds of everything drawn, in device pixels. Sizes scissor rectangles and scratch buffers. */
  Rect bounds = {};

  /**
   * A texture a command samples that this recording did not create.
   *
   * The definition is public as `graphics::ExternalImage`, because `Canvas::drawExternalImage` records one
   * and a caller must be able to name it without including this header. Here it is the same type, so the
   * command list and the canvas cannot drift apart.
   */
  using ExternalImage = graphics::ExternalImage;

  /**
   * Every bitmap a command may sample, indexed by `CommandState::image`.
   *
   * Owned here rather than in a registry beside the recorder, and that is a correctness fix rather than a
   * tidy-up. The images used to live in a file-static `unordered_map<const DrawList*, std::vector<Bitmap>>`
   * keyed on the list's **address**: nothing ever removed an entry, so every list that was ever recorded left
   * its bitmaps behind for the life of the process -- and a new list allocated at a freed address would have
   * read the *previous* list's images, which is a wrong picture with no error anywhere.
   *
   * Owning them makes both problems disappear: the images are freed with the list, and an index can only ever
   * name an image of the list it belongs to. It also makes the table part of what a `DrawList` *is*, which is
   * what a reader would assume anyway -- `CommandState::image` is documented as an index, and this is the thing
   * it indexes.
   */
  std::vector<Bitmap> images;

  /**
   * Optional content key per image, aligned with @p images. A non-zero entry lets the renderer skip
   * hashing that bitmap; synthesized clip masks set it from their coverage key.
   */
  std::vector<uint64_t> imageHashes;

  /**
   * Every texture a `Kind::ExternalImage` command samples, indexed by `CommandState::image`.
   *
   * Separate from `images` because the two have opposite ownership: a `Bitmap` is pixels this list
   * owns, while an external image is a view somebody else owns and this list only borrows for the
   * frame. Keeping them in one table would make "does this list own its images?" a per-element
   * question.
   */
  std::vector<ExternalImage> externalImages;

  /**
   * A coverage field: 8-bit coverage per texel, the texel *centre* at the device pixel it covers.
   *
   * Plain data rather than the rasterizer's `CoverageField`, for the same reason `images` holds `Bitmap`s:
   * this header is public and the rasterizer is private. `originX`/`originY` are the device pixel of texel
   * (0, 0), and `texels` is row major with one byte per texel -- the same convention `EdgeCoverage.h` uses,
   * stated here so that the public type does not need the private one to be understood.
   *
   * ## An entry with no texels is a *request*, and the renderer fills it in
   *
   * `Surface::drawCoverageField` hands over a field that is already rasterised, so its `texels` are set. A path goes
   * the other way: `GraphiteCanvasBackend::recordCoverageField` records the field's **lattice** -- the device rectangle
   * it covers, which must be pixel-aligned -- and names the contours to rasterise, leaving `texels` empty for the
   * renderer's resolve stage to fill. That is step S5 of the plan: the record thread stops rasterising, and the
   * rasterising becomes one explicit stage that can run on the job pool.
   *
   * The request is therefore *self-describing* -- lattice, fill rule and contours -- rather than a reference to the
   * command that names it. That matters for the parallel loop that reads it: a field generated from "whatever command
   * names me" would make the stage's output depend on a second lookup that two commands could disagree about, while a
   * range of points in this same list cannot.
   *
   * So `texels.empty()` is not "an empty field": it means "not generated yet". `isEmpty()` asks about the extent and
   * `hasCoverage()` about the texels, because a request answers those two questions differently and collapsing them is
   * how a request would be dropped as "empty".
   */
  struct CoverageField {
    int32_t originX = 0;
    int32_t originY = 0;
    int32_t width = 0;
    int32_t height = 0;
    /** One byte per texel, row major, or empty for a field the renderer still has to generate. */
    std::vector<uint8_t> texels;
    /**
     * The fill rule a *generated* field is filled with. Read only when @c texels is empty -- a field that already
     * carries coverage was filled by whoever built it, and this member says nothing about it.
     */
    FillType fillType = FillType::Winding;
    /**
     * The contours a *generated* field is rasterised from: a range in `DrawList::contours`, indexed like
     * `Command::firstContour`. Read only when @c texels is empty.
     */
    uint32_t firstContour = 0;
    /** How many contours @c firstContour names. */
    uint32_t contourCount = 0;

    /** @return true when the field covers no device pixels at all, which is the only case a caller must reject. */
    [[nodiscard]] bool isEmpty() const noexcept { return width <= 0 || height <= 0; }

    /** @return true when @c texels holds the coverage, rather than describing a field still to be generated. */
    [[nodiscard]] bool hasCoverage() const noexcept { return !texels.empty(); }
  };

  /**
   * Coverage fields recorded this frame, indexed by `CommandState::coverageField`.
   *
   * Owned here for the same reason `images` is: a recording outlives the call that produced it, so a
   * command cannot point at a caller's buffer. An entry may be a fully resolved field or a request the
   * renderer generates -- see `CoverageField`.
   */
  std::vector<CoverageField> coverageFields;

  /** A label for a capture tool: where this recording came from, not a resource name. */
  std::string label;

  /** Sentinel for `Command::clipMask`. */
  static constexpr uint32_t kNoClipMask = 0xFFFFFFFFu;
  /** Sentinel for `CommandState::coverageField`. */
  static constexpr uint32_t kNoCoverageField = 0xFFFFFFFFu;
  /** Sentinel for `CommandState::image`. */
  static constexpr uint32_t kNoImage = 0xFFFFFFFFu;

  DrawList() = default;

  // The list is moved to the submit thread and never copied: a copy would be a second owner of several
  // vectors' worth of geometry, and the hand-off is a move by construction.
  DrawList(const DrawList&) = delete;
  DrawList& operator=(const DrawList&) = delete;
  DrawList(DrawList&&) noexcept = default;
  DrawList& operator=(DrawList&&) noexcept = default;

  /**
   * @return a copy of this recording, kept by the caller.
   *
   * ## Why a copy has to be asked for by name
   *
   * The copy constructor above is deleted on purpose -- a recording is several vectors' worth of geometry and the
   * hand-off to a submit thread is a move -- but a caller that wants to submit the **same** recording on every frame (a
   * UI with a display list) needs the copy the deleted constructor refuses to make by accident. So it is one explicit
   * method, and the caller pays for it visibly. Measured on the demo scene, cloning a 41849-command list costs
   * 3.6-4.3 ms against the ~130 ms of recording it replaces (`路径边缘抗锯齿设计与现状.md` 4.1.40).
   *
   * `Surface::submitRecording` is the other answer to the same need and it is cheaper still: it *borrows* a list for one
   * submission instead of copying it, so a caller that can keep its list alive until `submit` returns should prefer it
   * and needs no clone at all.
   *
   * ## It lists every member, and that is a rule rather than a style
   *
   * A member added to this struct has to be added here too, or a clone would silently drop it -- a picture that is
   * wrong in whatever way that member describes. The deleted copy constructor is what makes the rule necessary; this
   * comment is what makes it findable. The bitmaps are copied as `Bitmap`, which owns its pixels through a refcounted
   * `Data`, so a clone shares the pixels rather than duplicating them.
   */
  [[nodiscard]] DrawList clone() const {
    DrawList copy;
    copy.commands = commands;
    copy.contours = contours;
    copy.points = points;
    copy.maskPixels = maskPixels;
    copy.clipMasks = clipMasks;
    copy.layers = layers;
    copy.bounds = bounds;
    copy.images = images;
    copy.imageHashes = imageHashes;
    copy.coverageFields = coverageFields;
    copy.label = label;
    return copy;
  }

  [[nodiscard]] bool isEmpty() const noexcept { return commands.empty(); }

  /** @return the number of layers, always at least one: the root. */
  [[nodiscard]] size_t layerCount() const noexcept { return layers.size(); }

  /** Drops every command and every layer but the root, keeping the capacity for the next frame. */
  void clear() noexcept {
    commands.clear();
    contours.clear();
    points.clear();
    maskPixels.clear();
    clipMasks.clear();
    layers.clear();
    // The images go with the commands that name them. An index left in the table after its command is gone would be
    // read by the *next* frame's first image draw, which would sample a bitmap from the frame before.
    images.clear();
    // The coverage fields go the same way, and for the same reason: a stale index would make the next frame's first
    // coverage draw sample the previous frame's field.
    coverageFields.clear();
    bounds = Rect {};
  }

  /** @return how many bytes of geometry and commands the list holds, for a per-frame diagnostic. */
  [[nodiscard]] size_t byteSize() const noexcept {
    return commands.size() * sizeof(Command) + contours.size() * sizeof(Contour) + points.size() * sizeof(Point) +
           maskPixels.size() + clipMasks.size() * sizeof(ClipMask);
  }
};

} // namespace graphics::gpu

#endif // CORAL_GPU_DRAW_LIST_H
