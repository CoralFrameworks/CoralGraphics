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

#ifndef CORAL_GPU_GPU_RENDERER_H
#define CORAL_GPU_GPU_RENDERER_H

#include <private/gpu/ContextImpl.h>
#include <private/gpu/ImageAtlas.h>

#include <backend/RHI.h>

#include <gpu/DrawList.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace graphics {
class Bitmap;

namespace gpu {

/**
 * The sentinel a frame's placement tables carry when a command samples no texture at all.
 *
 * A named value rather than "0", because texture 0 is a real one: two commands whose textures could not be resolved must
 * not be taken for two commands sharing a texture, or they would be batched into one draw and one of them would sample
 * the other's bitmap. It is the same value the coverage route uses for an unplaced field, and deliberately so: the two
 * routes are told apart by their shader before either sentinel is read, and one value cannot be confused with a real
 * texture on either route.
 */
inline constexpr uint32_t kNoSampledTexture = 0xFFFFFFFFu;

/**
 * The one class that touches the RHI on behalf of `ContextImpl`.
 *
 * ## Why the renderer is not the context
 *
 * `ContextImpl` is the object a `Surface` holds, so its header is the contract between the canvas and
 * the device and has to stay small enough to read. Everything an implementation has to *decide* -- how
 * many pipelines exist, which buffers are reused between frames, how a layer is composited -- is here
 * instead, behind a pimpl the context never looks inside. The split also keeps the device lifetime in
 * one place: the renderer owns the device it created, so the device is a member destroyed after every
 * resource that belongs to it.
 *
 * ## The stages
 *
 * `submit` is exactly the three stages the `DrawList` header describes, and they are separated by what
 * they are allowed to touch rather than by size:
 *
 * | stage | threads | touches the device | why |
 * |---|---|---|---|
 * | precompile | the job pool, or the caller | **no** | pure CPU work over one command's own data |
 * | upload | the calling thread | yes | buffers are device objects, and creation is serialised |
 * | record | the calling thread | yes | one GL context, one command buffer, no interleaving |
 *
 * The rule that makes `precompile` safe is enforced by its signature rather than by discipline: it
 * takes a `const DrawList&` and writes only into the per-command slot it was handed, so a worker
 * thread cannot reach a device handle even by accident.
 *
 * ## What this renderer deliberately does not do
 *
 * Named here rather than left to be discovered, because a missing feature that draws *something* is
 * worse than one that reports itself. Every one of these is counted per frame and reported by
 * `diagnostics()`, and the first frame that needs it is reported once through the device's message
 * sink.
 *
 * This list is kept to what is *still* missing. It used to name gradients, images and antialiased path edges, all three
 * of which are drawn now -- through `gradient.frag`, through `image.frag` over an image atlas page, and through
 * `coverage.frag` over a coverage field -- and a list that says otherwise is the kind of stale comment that sends the
 * next reader looking for a defect that is not there (`4.1.23` records one of those).
 *
 *  - **Anti-aliased edges on a path that cannot be a coverage field.** An ordinary path is recorded as a field and its
 *    edge is the rasteriser's own coverage, but a path whose paint carries a shader (a gradient, a colour filter) or
 *    whose lattice is past the field route's size bound falls back to the stencil fill, which is hard-edged -- the
 *    coverage-free variant `path.frag`'s own comment describes. That fallback is counted as a *field request refused* by
 *    the recorder's gate rather than here, so the frame's omission line is not where it shows up.
 *  - **Shaped clips** (`Command::clipMask`) are not applied. The command is drawn with its rectangular
 *    `clipRect` only and counted; a coverage clip needs a program that samples coverage *and* the
 *    fill's own texture, and the fill programs' one sampler is already spoken for.
 *  - **An advanced blend mode on a `Rect`/`Path`/`Image` command.** Modes up to `kScreen` -- every mode
 *    with a blend-factor pair -- are handled by the blend unit, including the whole Porter-Duff family.
 *    The separable and non-separable modes above `kScreen` are handled at a *layer* boundary through
 *    `composite.frag`, which is the case that shader exists for. A per-command advanced mode would need
 *    a scratch target per command, a destination snapshot per command and a pass open and closed around
 *    each: that is not built, so such a command is counted and skipped rather than drawn wrongly.
 *  - **A path in a target with no stencil attachment.** The fan is drawn straight into the framebuffer,
 *    which is right for a convex contour and wrong for a concave one, so it is counted
 *    (`pathsWithoutStencil`) rather than silently approximated.
 *  - **An image the renderer cannot resolve.** A command naming an image the frame's table does not
 *    have, a bitmap in a layout the upload path cannot take, and a bitmap whose texture write was
 *    refused are all *omissions* rather than features: counted, drawn as nothing, and never sampled from
 *    a texture that happens to be bound.
 *  - **Multisampled targets.** `RenderTargetDescription::sampleCount` above 1 allocates a multisampled
 *    colour and depth attachment plus a single-sampled resolve image. The pass attaches the former and
 *    resolves into the latter, so snapshots, layer composites and `Surface::target()` always see resolved
 *    pixels; a device without `Feature::MultisampleResolve` refuses the request.
 */
/**
 * One vertex of a path fan or of its cover rectangle: device-space position plus the premultiplied
 * fill colour.
 *
 * The colour travels in the geometry rather than in a uniform block because the cover rectangle is six
 * vertices of one draw; a per-draw block has to be allocated, range-offset and rebound for every path,
 * while the vertex colour is already written next to the position. It also keeps the cover pass a
 * function of its own vertex buffer, which is what made the red-channel defect of the uniform route
 * go away rather than be worked around.
 */
struct PathVertex {
  float x = 0.0f;
  float y = 0.0f;
  float color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
};

class GpuRenderer {
public:
  GpuRenderer();
  ~GpuRenderer();

  GpuRenderer(const GpuRenderer&) = delete;
  GpuRenderer& operator=(const GpuRenderer&) = delete;
  GpuRenderer(GpuRenderer&&) = delete;
  GpuRenderer& operator=(GpuRenderer&&) = delete;

  /** @see ContextImpl::initialize. Never throws; every failure is a `Result`. */
  backend::Result initialize(backend::Device* borrowedDevice, backend::Backend preferred, utils::JobSystem* jobs,
    backend::DebugMessageCallback diagnostics = nullptr, void* diagnosticsUserData = nullptr);

  /** @return true when a device is live and every shader the renderer needs was found. */
  [[nodiscard]] bool isValid() const noexcept { return mDevice != nullptr && mReady; }

  [[nodiscard]] backend::Device* device() const noexcept { return mDevice; }
  /** @return true when this renderer created the device and must therefore destroy it. */
  [[nodiscard]] bool ownsDevice() const noexcept { return mOwnedDevice != nullptr; }

  /** Transfers a created device to the caller; the renderer keeps using it but no longer deletes it. */
  [[nodiscard]] backend::Device* releaseDevice();

  [[nodiscard]] std::string describeDevice() const;
  [[nodiscard]] std::string diagnostics() const;

  backend::Result createRenderTarget(const RenderTargetDescription& description, RenderTarget* outTarget);
  backend::Result createLayerTarget(uint32_t width, uint32_t height, backend::PixelFormat format, bool needsStencil,
    RenderTarget* outTarget);
  /**
   * Creates a target that renders into the **surface's own images** -- the swapchain -- instead of a texture.
   *
   * There is no colour texture (`RenderTarget::color` stays invalid), the attachment is named by
   * `defaultFramebufferView()`, and there is no depth/stencil attachment: a default framebuffer's depth belongs to the
   * window, so a pipeline that declares one is incompatible with this target. Path fills that need the stencil stay
   * offscreen.
   */
  backend::Result createWindowTarget(uint32_t width, uint32_t height, backend::PixelFormat format,
    uint32_t sampleCount, RenderTarget* outTarget);

  void destroyRenderTarget(RenderTarget& target);

  /**
   * Renders @p list into @p target, sampling the bitmaps in @p images by `CommandState::image`.
   *
   * The list is **borrowed**: it is stored as a pointer for the duration of the call and only read, so a caller may keep
   * a recording and submit it again on the next frame (`Surface::submitRecording`). It must outlive the call.
   *
   * The image table is a parameter rather than part of the list for the reason `DrawList` gives: a list that
   * owned pixels could not be replayed by another backend or written to a file. A command naming an id past
   * the end of the table is skipped and counted, never drawn with whichever texture was bound last.
   *
   * @param rootLoad what happens to the root target's existing pixels. `DontCare` -- the default, and what
   *        every `Surface` frame passes -- keeps the renderer's own rule: clear unless the recording's
   *        first command is a clear that covers the whole target (`recordFrame` says why). `Clear` forces
   *        the clear, and `Load` asks for the opposite of it: the frame is drawn **onto** what is already
   *        there, which is what a canvas stage of an effect chain is -- a sticker, a caption or a
   *        signature added to a picture some earlier pass produced. Without it the root's rule makes an
   *        overlay *replace* the picture, because a submission always leaves the target with defined
   *        contents.
   */
  /**
   * Records and submits one list.
   *
   * @param continueFrame records @p list into the command buffer an earlier call opened and returns
   *        without uploading, ending the buffer or submitting. The caller assembles one frame from
   *        consecutive lists and passes `false` on the last of the run, which closes the frame. Every
   *        list of a run shares the frame canvas block and must therefore need the same canvas (no
   *        translation); its vertices are appended after those of the previous list.
   */
  backend::Result submit(const DrawList& list, RenderTarget& target, const std::vector<Bitmap>& images,
    backend::LoadOp rootLoad = backend::LoadOp::DontCare,
    backend::SemaphoreHandle waitSemaphore = backend::SemaphoreHandle::invalid(),
    backend::SemaphoreHandle signalSemaphore = backend::SemaphoreHandle::invalid(),
    const Rect& damage = {}, const Point& translation = {}, bool continueFrame = false);

  /**
   * Runs the colour-managed presentation pass.
   *
   * A full-target draw into @p target, sampling @p source: decode sRGB when the source is encoded,
   * convert Rec.709 primaries to the destination gamut, then write PQ, extended linear or gamma-encoded
   * P3. It is a separate, synchronous submission because it writes a different target from the frame it
   * presents; the caller orders the two on the queue with the semaphores it passes.
   */
  backend::Result present(const RenderTarget& source, RenderTarget& target,
    const PresentationParameters& parameters,
    backend::SemaphoreHandle waitSemaphore = backend::SemaphoreHandle::invalid(),
    backend::SemaphoreHandle signalSemaphore = backend::SemaphoreHandle::invalid());

  /** Enables asynchronous frame submission; the caller owns one renderer per in-flight slot. */
  void setAsyncSubmissions(bool enabled) noexcept { mAsyncSubmissions = enabled; }

  /** Sets the pattern texture the SDF pattern program samples; an empty bitmap clears it. */
  /** @return true when the backend can sample a pattern; OpenGL falls back to the plain SDF program. */
  bool setSdfPattern(const Bitmap& pattern);

  /** Waits for this renderer's submitted frame, if any. */
  backend::Result waitForFrame();
  backend::Result readback(RenderTarget& target, Bitmap* outBitmap);
  backend::Result waitIdle();

  [[nodiscard]] uint64_t frameCount() const noexcept { return mFrameCount; }
  [[nodiscard]] const ContextImpl::FrameStats& stats() const noexcept { return mStats; }

  /**
   * The GPU time of the last **completed** frame, in milliseconds.
   *
   * A timestamp query pair is recorded into every frame's command buffer, so the value arrives one
   * frame late: it is read after the previous frame's fence is waited on, which is what makes it the
   * GPU's time rather than the CPU's submit time. Zero when the device has no timestamp queries.
   */
  [[nodiscard]] double gpuMilliseconds() const noexcept { return mGpuMilliseconds; }

  /** @return how many shader blobs the header this build compiled against embedded. */
  [[nodiscard]] size_t embeddedShaderCount() const noexcept { return mEmbeddedShaderCount; }

  /** @return how many pipelines are cached, for a diagnostic. */
  [[nodiscard]] size_t pipelineCount() const noexcept { return mPipelines.size(); }

private:
  // ------------------------------------------------------------------------------------------------
  // Programs and pipeline variants
  // ------------------------------------------------------------------------------------------------

  /**
   * Which shader program a pipeline runs.
   *
   * A program here is a *pair* of blobs plus the fixed state that cannot vary within one pipeline:
   * vertex input and topology. The blend mode is the one axis that does vary per draw and still forces
   * a second pipeline, because on every backend the RHI models it is baked into the pipeline object
   * rather than set at draw time.
   */
  enum class Program : uint8_t {
    /** `quad.vert`/`quad.frag`: an instanced rectangle, textured or flat, with analytic coverage. */
    Quad,
    /** `path.vert`/`path.frag` as a triangle fan: the stencil-accumulating half of a path fill. */
    PathFan,
    /** The same program as a triangle list: the cover rectangle of a path fill. */
    PathCover,
    /** `composite.vert`/`composite.frag`: a blend function the blend unit cannot express. */
    Composite,
    /** `composite.vert`/`mask.frag`: a layer's coverage, or a layer copied onto its parent. */
    Mask,
    /**
     * `quad.vert`/`gradient.frag`: the same instanced rectangle, filled with a gradient instead of a colour.
     *
     * The same vertex stage as `Quad`, so the geometry, the coverage and the y flip are shared, and a different
     * fragment stage -- see that shader for why the gradient is not a third branch inside `quad.frag`.
     */
    Gradient,
    /**
     * `quad.vert`/`coverage.frag`: the same instanced rectangle, filled by sampling an edge-coverage field.
     *
     * ## Its own layout and pipeline, and deliberately not reachable from `drawPath`
     *
     * The existing path fill goes through `Program::PathFan` and `Program::PathCover` on `mPathSetLayout`,
     * whose bindings are uniform buffers. This program needs a *sampler* at binding 0, so it cannot share that
     * layout -- and building a set from a layout the pipeline was not created with is the mistake that once
     * stopped the device coming up at all (see `createPipelineLayouts`).
     *
     * It also gets its own entry point rather than being folded into `drawPath`. A path fill is winding
     * accumulation plus a cover, with a stencil attachment and its own early-outs; a coverage field is a sampled
     * quad with neither. Making one function do both would put two pipelines behind one set of decisions, which is
     * where the last several defects in this area came from. Path filling is meant to move onto this route as a
     * method of its own, and until then nothing calls this program -- so adding it cannot change a picture.
     */
    CoverageField,
    /**
     * `quad.vert`/`image.frag`: the same instanced rectangle, sampling a **sub-rectangle** of its texture.
     *
     * The third program to share the quad's vertex stage, and it exists for one reason: the image path needs four more
     * numbers per command -- the sub-rectangle in normalized texture coordinates -- and `quad.frag` has none to spare
     * (`vParams` carries the rounded-rectangle parameters and two flags there, and `vRectSize` is the geometry size the
     * *vertex* stage positions the rectangle with). A fifth vertex attribute would give it those numbers and charge them
     * to every flat fill and every coverage field in the frame; a fragment stage with no rounded-rectangle path and no
     * untextured path has the whole of `vParams` free, so the sub-rectangle costs the image commands and nothing else.
     *
     * No new descriptor layout: an image's set is already created against `mQuadSetLayout`, whose binding 0 is the
     * sampler this stage reads. `CoverageField` needed a layout of its own because it *replaced* the quad's binding 2.
     */
    Image,
    /**
     * `quad.vert`/`image_cubic.frag`: the image quad, reconstructed with the Mitchell-Netravali cubic family.
     *
     * A separate program from `Image` rather than a branch inside it, because a shader's register footprint is paid by
     * every fragment that runs it: sixteen taps would have made every nearest and bilinear image in the frame carry the
     * cubic path's registers. The kernel's B and C are a uniform, so one program covers every kernel and the hardware path
     * honours any `CubicResampler` -- which is why there is one of these rather than one per named kernel.
     */
    ImageCubic,
    /** `quad.vert`/`sdf.frag`: the image quad sampling a signed distance field. */
    SdfImage,
    /** `quad.vert`/`sdf_pattern.frag`: the SDF quad with a second sampler for the pattern texture. */
    SdfPattern,
    /**
     * `composite.vert`/`present.frag`: the colour-managed presentation pass.
     *
     * A full-target quad that samples the canvas or the linear 16F intermediate and writes the display
     * encoding. It shares `composite.vert` because both passes cover the attachment and pass a texture
     * coordinate straight through; its fragment stage is where the colour transform lives.
     */
    Present,
  };

  /**
   * The stencil and blend policy of a pipeline, which is what the *variant* axis of the cache is.
   *
   * Named rather than numeric because the numbers would have to be read against `DepthStencilState` to
   * mean anything, and because a variant is a decision -- "this pass accumulates winding and writes no
   * colour" -- rather than an index.
   */
  enum class Variant : uint8_t {
    /** No stencil: the quad pass, the composite passes, and a path fill with no stencil attachment. */
    Plain,
    /**
     * Accumulate a non-zero winding number: increment on front faces and decrement on back, writing no
     * colour. `IncrementWrap`/`DecrementWrap` rather than the clamped pair so that a path crossing
     * itself more than 255 times wraps instead of saturating; the cover pass asks only whether the
     * result is zero, and a wrapped count is still non-zero.
     */
    StencilWinding,
    /** Accumulate parity: invert on both faces, which is what the even-odd rule is. */
    StencilEvenOdd,
    /** Zero the stencil again so that the next path starts from a clean buffer. */
    StencilReset,
    /** A path fill drawn straight from the fan, for a target with no stencil attachment. */
    FanFill,
    /** Cover the path's bounds where the stencil is non-zero. */
    StencilCover,
  };

  /**
   * The identity of a cached pipeline.
   *
   * `colorFormat` and `depthStencilFormat` are in the key because a pipeline is created against one
   * render pass and the RHI validates the attachment formats against it: a pipeline built for a target
   * with a stencil buffer is not compatible with one without, and putting the formats in the key is
   * what makes that a cache miss rather than a validation error at bind time.
   *
   * `blendMode` is `0xFF` for a variant whose blend state is fixed by the program, so that a caller
   * cannot accidentally key two identical pipelines apart by passing a mode the variant ignores.
   */
  struct PipelineKey {
    Program program = Program::Quad;
    Variant variant = Variant::Plain;
    uint8_t blendMode = 0xFF;
    backend::PixelFormat colorFormat = backend::PixelFormat::Undefined;
    backend::PixelFormat depthStencilFormat = backend::PixelFormat::Undefined;
    /** MSAA sample count of the target, so a single-sampled pipeline is never reused in a multisampled pass. */
    uint32_t sampleCount = 1;
    /** Destination colour write mask, bit 0 = red ... bit 3 = alpha. */
    uint8_t writeMask = 0x0F;
  };

  struct PipelineEntry {
    PipelineKey key;
    backend::RenderPipelineHandle pipeline = backend::RenderPipelineHandle::invalid();
    /**
     * The last frame that bound this pipeline, so that `FrameStats::pipelineCount` counts the pipelines
     * a frame *used* rather than the ones the cache happens to hold. A cache count would grow with the
     * number of formats and blend modes ever seen, which is the opposite of what a per-frame overlay
     * wants to show.
     */
    uint64_t lastUsedFrame = UINT64_MAX;
  };

  // ------------------------------------------------------------------------------------------------
  // Precompile
  // ------------------------------------------------------------------------------------------------

  /** One contour of a path, as the fan the stencil pass draws. */
  struct FanRange {
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
  };

  /**
   * What one command's precompile produced.
   *
   * A vector of these is indexed by command index and each element is written by exactly one task, so
   * no two threads touch the same element and no lock is needed. The `fan` points are *copied* out of
   * the draw list rather than referenced, because the frame's vertex arena is uploaded as one block and
   * a pointer into the list would be a second lifetime to reason about.
   */
  struct Precompiled {
    std::vector<PathVertex> fan;
    std::vector<FanRange> fans;
    /** Byte offset of the fan's first vertex in the frame's vertex arena. */
    uint32_t fanOffset = 0;
    /** Byte offset of the cover rectangle's six vertices in the frame's vertex arena. */
    uint32_t coverOffset = 0;
    /** Byte offset of the composite quad's six vertices, for a `CloseLayer` command. */
    uint32_t compositeOffset = 0;
    /** True when this command needs a composite quad and its region was not empty. */
    bool hasComposite = false;
    /** Wall time this command's precompile took, for the per-frame statistic. */
    double milliseconds = 0.0;
  };

  /**
   * A maximal run of consecutive commands that one instanced draw can cover.
   *
   * The state, the layer, the kind and the clip are all part of "can one draw cover these": the state is
   * what the pipeline and descriptor bindings express per *draw*, and the clip is a scissor rectangle,
   * which is also per draw. A batch spanning two clips could not be drawn correctly at all, which is
   * why the split is on the clip and not merely on the state.
   */
  struct Batch {
    uint32_t firstCommand = 0;
    uint32_t commandCount = 0;
    uint32_t layer = 0;
    DrawList::Kind kind = DrawList::Kind::Rect;
    DrawList::CommandState state;
    /** Byte offset of this batch's first vertex in the frame's vertex arena. */
    uint32_t vertexOffset = 0;
    /** The vertex record size of this batch: the quad record, or the SDF record for SdfImage. */
    uint32_t vertexStride = 56;
  };

  /** A snapshot of a target's pixels, which a mask or an advanced composite has to read. */
  struct ScratchTexture {
    uint32_t width = 0;
    uint32_t height = 0;
    backend::PixelFormat format = backend::PixelFormat::Undefined;
    bool inUse = false;
    backend::TextureHandle texture = backend::TextureHandle::invalid();
    backend::TextureViewHandle view = backend::TextureViewHandle::invalid();
  };

  /** A layer target kept between frames so that a steady-state frame allocates no textures. */
  struct PooledTarget {
    uint32_t width = 0;
    uint32_t height = 0;
    backend::PixelFormat format = backend::PixelFormat::Undefined;
    bool needsStencil = false;
    bool inUse = false;
    RenderTarget target;
  };

  /**
   * The 1x1 texture a layer composite's global alpha is read from, keyed by its quantised value.
   *
   * Cached rather than rewritten per composite because an upload is a *payload-carrying* command and the
   * backend's command arena cannot hold two of them: see `uploadBuffer`. Alpha is quantised to the eight
   * bits a colour channel has anyway, so the cache saturates at 256 entries and a steady frame issues no
   * uploads at all.
   */
  struct AlphaTexture {
    uint8_t value = 0;
    backend::TextureHandle texture = backend::TextureHandle::invalid();
    backend::TextureViewHandle view = backend::TextureViewHandle::invalid();
  };

  /**
   * The extent of one image atlas page, in texels.
   *
   * A page holds bitmaps rather than shapes, so what sizes it is the largest bitmap worth sharing a texture with: the
   * demo's image table is 559 bitmaps of which the mean is 93x93, and the tail is one photograph. A larger page holds
   * more bitmaps and wastes more of the last shelf, and 1024 is where the shelving test (`TheImageAtlasAllocatorNever
   * OverlapsAndNeverLies`) still fits 445 glyph-sized bitmaps in one page at 56% occupancy.
   */
  static constexpr uint32_t kImageAtlasExtent = 1024;

  /**
   * How many image atlas pages may exist at once, after which a new bitmap gets a texture of its own.
   *
   * The bound exists for the same reason the field atlas has one: nothing is ever freed (the RHI has no contract for
   * releasing a texture the GPU may still read), so without a cap a long session with animated bitmaps would grow
   * texture memory without a number anyone can state. 8 pages of `R8G8B8A8` at `kImageAtlasExtent` is 32 MiB, which is
   * the same order as the standalone textures the atlases replace -- and the fallback is a texture of the bitmap's own,
   * which is exactly what this route did before, so running out is a cost and never a wrong picture.
   */
  static constexpr uint32_t kImageAtlasPageBudget = 8;

  /** Everything one frame's recording has to carry between the stages. */
  struct Frame {
    const DrawList* list = nullptr;
    /** Device-pixel translation the caller asked for; folded into the canvas block when recording. */
    Point translation{};
    /**
     * The recording this frame actually replays, when the submitted list was rewritten.
     *
     * A list with per-command clip masks is rewritten once at submit time into an equivalent list that
     * expresses each masked run as a masked layer (see `synthesizeClipLayers`). This owns that copy so
     * `list` can keep pointing at it for the whole submission; it is empty for the common case.
     */
    std::unique_ptr<DrawList> ownedList;
    /** The caller's target, which is layer 0. */
    RenderTarget* root = nullptr;
    /**
     * One entry per `DrawList::Layer`, holding an *index* into `mLayerPool` rather than a pointer into it.
     *
     * `kRootTarget` for layer 0, which resolves to @c root.
     *
     * ## Why an index and not a pointer
     *
     * `mLayerPool` is a `std::vector<PooledTarget>`, so growing it moves every element. This vector used to hold
     * `RenderTarget*` and was filled as layers were acquired, which means a frame with enough layers to reallocate
     * the pool left every earlier entry pointing at freed memory -- and the pool grows *while the vector is being
     * filled*, so the window is not a corner case but the normal one for any frame past the first few layers.
     *
     * The symptom was two layers removed from the cause: the pipeline cache received a garbage `colorFormat` and an
     * unresolvable `RenderPassHandle` from a dead `RenderTarget`, the backend's validation rejected it, and the
     * renderer reported a bare `Result::Unsupported` for a frame that had already recorded 129152 draws. It took a
     * named validation message and this index change to connect them.
     *
     * An index cannot dangle. `mLayerPool` is only ever appended to and cleared as a whole at teardown, so an index
     * stays valid for the frame that took it.
     */
    std::vector<int32_t> targets;
    /** The layer each layer was opened inside, recovered from the command order. */
    std::vector<uint32_t> parents;
    /** Partial-update rectangle in target pixels; empty means the whole target. */
    Rect damage {};
    bool hasDamage = false;
    /**
     * One flag per layer: true when its target is `Layer::bounds`-sized and its commands have been
     * translated into that local space (R1, step one).
     *
     * Empty means every layer is a full-target layer, which is the pre-R1 behaviour. A sized layer is
     * only ever a top-level, childless, unmasked layer for now, so its parent is the root and the
     * composite region stays in root coordinates; that restriction is what keeps the first step from
     * touching nested-layer and mask coordinate paths.
     */
    std::vector<uint8_t> sized;
    /** True once a layer's target has had a pass begun, which is what decides Load versus Clear. */
    std::vector<uint8_t> opened;
    std::vector<Batch> batches;
    std::vector<Precompiled> precompiled;
    /**
     * The frame's **resolved** coverage fields, indexed by `CommandState::coverageField`.
     *
     * `DrawList::coverageFields` holds what the recorder wrote, and for a path that is a *request*: a lattice and a fill
     * rule, with no texels. `resolveCoverageFields` turns that table into this one -- a copy of every entry that already
     * carries coverage, and a generated field for every entry that names one -- so the replay never has to ask which
     * kind of entry it is looking at. Two tables with one lookup rule, rather than one table whose meaning depends on
     * which stage is reading it.
     *
     * It is the resolve stage's **input** and nothing else reads it -- the replay samples `atlases` through `placed` --
     * so a frame that reuses the previous frame's fields leaves it as the last resolve left it rather than paying for a
     * copy nothing will read.
     */
    std::vector<DrawList::CoverageField> fields;
    /**
     * Where each resolved field's texels live in the frame's atlases, indexed like `fields`.
     *
     * Built by `packCoverageFields` at the same time as `atlases`, and read by the uniform writer, which needs the *texel
     * origin* the fragment stage subtracts from a device position.
     *
     * The atlas rectangle itself is deliberately **not** here. It was, while the plan for the next step was to put it in
     * the vertex data so that a batch could share one draw -- but what the fragment stage needs for that is this same
     * *texel origin* (a device-space value), not the rectangle, because the expression it evaluates is
     * `(device - texelOrigin) / atlasExtent`. A field's place in the atlas is therefore private to the packing, and a
     * struct member nothing reads is a member the next person has to prove is right.
     */
    struct PlacedField {
      /** Index into @c atlases. */
      uint32_t atlas = 0;
      /**
       * The field's padded rectangle in that atlas: the field's own texels start one texel in from its corner, because
       * the rectangle includes the replicated border.
       *
       * Read by the fill, which writes each band's rows straight into the atlas at this rectangle rather than into a
       * buffer of the field's own (see `placeCoverageFields`), and by the border pass, which replicates the field's edge
       * texels into the ring around it. The *shader* never sees these two: what it needs is `texelOriginX/Y` below.
       */
      uint32_t left = 0;
      uint32_t top = 0;
      /**
       * `fieldOrigin - (the atlas position of the field's texel (0, 0))`, in device pixels: exactly the value
       * `uField.zw` carries, so that `(device - value) / atlasExtent` is the atlas texel coordinate of the fragment.
       */
      float texelOriginX = 0.0f;
      float texelOriginY = 0.0f;
    };
    /**
     * The frame's placements, a **reference** into the renderer's persistent table -- see the constructor for why the two
     * field tables live there rather than here.
     */
    std::vector<PlacedField>& placed;
    /**
     * The frame's atlases: the pooled texture each one uses, and the packed bytes to upload into it.
     *
     * The bytes live on the frame rather than in the pool because the pool is keyed on the atlas's extent, and two frames
     * of the same extent hold different pictures -- the hash each entry carries is what tells them apart.
     *
     * It is a **reference** into the renderer, like `placed`, and for the same reason: the packed bytes are also what the
     * cross-frame field cache holds, so a frame that reuses the previous frame's fields must not be handed a copy of them
     * (measured at 1.5-1.8 ms for the 3.9 MB on the demo, which was the largest part of a reused frame's field stage).
     */
    struct AtlasUpload {
      /**
       * The pool key and the atlas's extent, in texels: **two** numbers, because an atlas need not be square.
       *
       * The packer fills shelves of fields left to right, so what it wants is a width that fits the widest field and a
       * height that the resulting number of shelves needs -- and those are rarely equal. A square atlas of the same area
       * wastes up to four times the texels (measured on the demo: 4096x4096 = 16.7M texels holding 3.9M of coverage),
       * every one of which is zero-filled, written, hashed and held in memory. Two axes cost nothing to carry: the
       * fragment stage already divides by a `vec2` (`uField.xy`), so the shader is unchanged.
       */
      uint32_t width = 0;
      uint32_t height = 0;
      /** A hash of @c bytes, compared against the pooled texture's to decide whether this frame uploads it at all. */
      uint64_t contentHash = 0;
      std::vector<uint8_t> bytes;
    };
    std::vector<AtlasUpload>& atlases;

    /**
     * @param atlasBytes the renderer's persistent atlas-byte table; @p placements its persistent placement table.
     *
     * ## Why these two tables are references rather than the frame's own storage
     *
     * They are the *output* of the resolve stage, and they are also exactly what the cross-frame field cache of 4.1.35
     * holds: a frame whose fields are the previous frame's must therefore find them **already in place**, with no copy in
     * either direction. Measured on the demo, copying them was 1.5-1.8 ms of a reused frame's 2.9-3.4 ms field stage,
     * against 0.6-0.8 ms for the key that decides the reuse -- so the copies were the larger half of "knowing", and this
     * arrangement removes them from both paths.
     *
     * The consequence, and the one thing a reader has to hold on to: **nothing clears these at the end of a frame.**
     * Their contents are the last resolve's output, and a resolve that runs *replaces* them (`placeCoverageFields` clears
     * `atlases` and re-assigns `placed` before it does anything else, which is also what makes a frame whose placement
     * failed draw nothing rather than the frame before's fields).
     */
    Frame(std::vector<AtlasUpload>& atlasBytes, std::vector<PlacedField>& placements) noexcept
      : placed(placements), atlases(atlasBytes) {}
    /**
     * Where each of this frame's images is sampled from, indexed like the `images` table `submit` was handed.
     *
     * Built by `placeImages` **before** `precompile` and before the arena is assembled, which is the whole reason it is
     * a table on the frame rather than a lookup inside the image draw: two commands may share one draw only if they
     * sample the same texture (that is `textureId`), and the *vertex* stage needs the sub-rectangle -- while a
     * descriptor set cannot be chosen per command inside a batch. Both facts push the answer earlier than the draw, and
     * an earlier answer is a table.
     *
     * The pixels are **not** here. A placement is a content-hashed entry in `mImagePlacements`, which outlives the
     * frame; this is the frame's copy of the few numbers the batch and the vertex writer need, so that neither has to
     * hash or look anything up.
     */
    struct PlacedImage {
      /**
       * False when the bitmap could not be made into a texture at all.
       *
       * Counted as an omission by the replay and never batched with a placed one -- the sentinel `textureId` is what
       * `canShareDraw` refuses, the same way it refuses an unplaced coverage field.
       */
      bool valid = false;
      /**
       * True once this index has been through `ensureImagePlacement`, whether or not that succeeded.
       *
       * The flag that keeps a *failure* from being retried once per command: `placeImages` walks the commands, and a
       * frame of 300 glyph draws naming one bitmap that cannot be placed would otherwise hash that bitmap 300 times.
       */
      bool resolved = false;
      /** The placement's key in `mImagePlacements`, so the upload stage can find the entry to record its pixels from. */
      uint64_t contentHash = 0;
      /** Which texture this command samples: a page's id, or a standalone texture's. Unique within the frame. */
      uint32_t textureId = kNoSampledTexture;
      /** How this image was placed, which decides whether the draw needs a cubic program. */
      SamplingOptions sampling = {};
      /** The set that binds that texture at binding 0, which is what a batch of images binds once. */
      backend::DescriptorSetHandle set = backend::DescriptorSetHandle::invalid();
      /**
       * The texture itself, for a draw that has to name it in a set of its own rather than bind `set`.
       *
       * One caller: a mask layer, whose composite binds the mask at binding 1 of the composite's set -- the set built for
       * the *image* binds it at binding 0 and pairs it with the quad's layout, so it cannot be reused there. Taken from
       * the placement (the page's texture, or the bitmap's own) because the frame's entry names only ids.
       */
      backend::TextureHandle texture = backend::TextureHandle::invalid();
      /** The view of that texture for a combined SDF-pattern set. */
      backend::TextureViewHandle view = backend::TextureViewHandle::invalid();
      /** The combined set this image uses as an SDF pattern batch, created on first use. */
      backend::DescriptorSetHandle patternSet = backend::DescriptorSetHandle::invalid();
      /**
       * The sub-rectangle `image.frag` samples, in normalized texture coordinates: `xy` origin, `zw` size.
       *
       * `(0, 0, 1, 1)` for a bitmap in a texture of its own -- the whole texture -- and the bitmap's **interior**
       * inside a page, inset by the one-texel border the page places around it. See `normalizedImageRect`.
       */
      std::array<float, 4> rect { 0.0f, 0.0f, 1.0f, 1.0f };
    };
    std::vector<PlacedImage> images;
    /** The bytes uploaded to the vertex arena this frame. */
    std::vector<uint8_t> arena;
    /** The layer whose pass is open, or -1. At most one pass is open at a time. */
    int32_t openPass = -1;
    /**
     * The command index of the `CloseLayer` being composited.
     *
     * Carried on the frame rather than passed down, because the composite's quad vertices were laid out
     * during arena assembly and keyed by command index, and threading an index through three calls that
     * each need one more parameter would be worse than one field that is only meaningful inside one
     * case of the walk.
     */
    uint32_t compositeCommandIndex = 0;
  };

  // ------------------------------------------------------------------------------------------------
  // Device and resource lifetime
  // ------------------------------------------------------------------------------------------------

  backend::Result createDeviceIfNeeded(backend::Backend preferred, backend::DebugMessageCallback diagnostics,
    void* diagnosticsUserData);
  backend::Result createPipelineLayouts();
  backend::Result createFallbackTextures();
  backend::Result uploadSdfPattern();
  backend::Result createUploadBuffers();
  /** Destroys every cached resource. Safe on a partially initialised renderer. */
  void destroyResources() noexcept;
  void destroyPooledTargets() noexcept;
  void destroyScratchTextures() noexcept;
  /**
   * Releases every texture and descriptor set the *content caches* hold.
   *
   * Separate from `destroyScratchTextures` because they are different pools with different keys: scratch textures are
   * pooled by size for a frame's own passes, while these are content-keyed and outlive a frame. Both are released at
   * teardown, and until this existed neither the image cache nor the field pool had its textures, views or descriptor
   * sets destroyed at all -- `destroyResources` walked the pipelines, the two pools, the alpha textures and the
   * samplers, and left two tables of live driver objects behind. It is one leak per renderer rather than per frame,
   * which is why no test noticed; a teardown path that misses a table is the whole reason teardown is a function rather
   * than a destructor body.
   */
  void destroyCachedTextures() noexcept;

  backend::Result ensurePipeline(const PipelineKey& key, backend::RenderPassHandle pass,
    backend::RenderPipelineHandle* outPipeline);
  backend::Result createPipeline(const PipelineKey& key, backend::RenderPassHandle pass,
    backend::RenderPipelineHandle* outPipeline);
  backend::Result ensureVertexCapacity(uint64_t bytes);
  /**
   * Makes sure the static quad index pattern covers @p indices, creating or growing the buffer if it does not.
   *
   * The pattern is `0,1,2, 0,2,3` per quad, repeated: it is the *same* for every command in every frame, which is why it
   * is a buffer of its own rather than bytes in the frame's arena. A frame therefore pays nothing for it unless a batch
   * grew past the last capacity, and the arena holds four vertices per command instead of six (measured on the demo:
   * 14.0 MB of vertex payload per frame becoming 9.4 MB, `4.1.37`).
   *
   * It is filled through `uploadBuffer`, like the vertex buffer, so the pattern reaches the device by a mapped write
   * rather than by an upload command in a frame's command buffer.
   */
  backend::Result ensureIndexCapacity(uint64_t indices);

  /** The marker `Frame::targets` uses for layer 0, which is the caller's own target rather than a pooled one. */
  static constexpr int32_t kRootTarget = -1;

  /**
   * Resolves a `Frame::targets` entry to the target it names.
   *
   * The one place an index is turned back into a reference, so no caller has to know that layer 0 is special or
   * that the rest are pooled. Callers hold the reference for the duration of one draw; they must not keep it,
   * because a later `acquireLayerTarget` can still move the pool -- the index is what stays valid, not the
   * reference.
   */
  [[nodiscard]] RenderTarget& targetAt(const Frame& frame, const int32_t index) noexcept {
    return index == kRootTarget ? *frame.root : mLayerPool[static_cast<size_t>(index)].target;
  }

  [[nodiscard]] const RenderTarget& targetAt(const Frame& frame, const int32_t index) const noexcept {
    return index == kRootTarget ? *frame.root : mLayerPool[static_cast<size_t>(index)].target;
  }

  backend::Result ensureCommandBuffer();
  backend::Result ensureUploadCommandBuffer();
  /** Writes @p size bytes at @p offset by mapping the buffer; see the implementation for why. */
  backend::Result uploadBuffer(backend::BufferHandle buffer, uint64_t bufferSize, uint64_t offset, const void* data,
    size_t size);
  /** Makes the 1x1 texture for @p alpha current, creating and uploading it on first use. */
  backend::Result updateConstantAlphaTexture(float alpha);

  /** Creates the six-vertex presentation quad and the set the presentation pass binds. */
  backend::Result createPresentationResources();
  /** Appends a diagnostic, keeping only the last few so that a steady frame cannot grow without bound. */
  void note(backend::MessageSeverity severity, std::string message);

  /** Creates the timestamp query set when the device can time; failure leaves timing disabled. */
  void createTimingQuerySet();
  /** Records the frame's start marker (a timestamp, or the start of an elapsed timer on GL). */
  void beginTimingPass();
  /** Records the end marker; the result is read when the frame's fence has signalled. */
  void endTimingPass();
  /** Reads the previous frame's timestamps into `mGpuMilliseconds`. */
  void readTimingResults();
  backend::Result reportOmissions(const DrawList& list);

  /** @return the index of a usable full-size target, or -1 when one could not be created. */
  [[nodiscard]] int32_t acquireLayerTarget(uint32_t width, uint32_t height, backend::PixelFormat format,
    bool needsStencil);
  /** @return the index of a usable snapshot texture, or -1 when one could not be created. */
  [[nodiscard]] int32_t acquireScratchTexture(uint32_t width, uint32_t height, backend::PixelFormat format);
  void releaseFrameResources() noexcept;

  // ------------------------------------------------------------------------------------------------
  // The stages
  // ------------------------------------------------------------------------------------------------

  void precompile(const DrawList& list, std::vector<Precompiled>& out, std::vector<uint8_t>& startsBatch);
  void buildBatches(const DrawList& list, const std::vector<uint8_t>& startsBatch, std::vector<Batch>& out);
  void resolveLayerParents(const DrawList& list, std::vector<uint32_t>& parents);
  backend::Result assembleArena(Frame& frame);
  backend::Result recordFrame(Frame& frame, const std::vector<Bitmap>& images, backend::LoadOp rootLoad,
    bool keepPassOpen = false);
  backend::Result drawRectBatch(Frame& frame, const Batch& batch);

  /**
   * Draws one **batch** of `Kind::Image` commands: a textured quad per command, in one instanced draw.
   *
   * Every command in the batch samples the **same texture** -- `canShareDraw` grouped them by `textureId` -- so the
   * program, the descriptor set and the scissor are bound once and a single `draw(6 x N)` covers the batch, exactly as
   * `drawGradientBatch` and `drawCoverageFieldBatch` do. What still differs per command is *which sub-rectangle* of
   * that texture it samples, and that travels per vertex (`image.frag` reads it from `vParams`), which is what makes
   * the batch expressible: while each glyph had a texture of its own, this route was 561 draws in 561 batches on the
   * demo scene, and 43 runs of commands is what it would be if the image stopped mattering (`4.1.28`).
   *
   * A command whose bitmap has no placement is counted as an omission rather than drawn: with the set coming from the
   * batch, drawing one would sample whichever texture the batch bound -- a wrong picture that looks deliberate.
   */
  backend::Result drawImageBatch(Frame& frame, const Batch& batch);

  /**
   * Draws a batch of rectangles filled with a gradient, through `gradient.frag`.
   *
   * A whole batch shares one gradient state, so one uniform write serves every command in it -- which is what makes a
   * gradient no more expensive per draw than a flat fill.
   */
  backend::Result drawGradientBatch(Frame& frame, const Batch& batch);
  /**
   * A **field atlas**: one texture holding a whole frame's coverage fields, and the set that binds it.
   *
   * ## Why an atlas rather than a texture per field
   *
   * The per-field pool it replaced was bounded (by size class) and that bound was the whole point, because a field is a
   * per-shape quantity and an animation produces a new one every frame. Measured on the demo scene -- 41000 paths, so
   * 41000 fields and a pool of 30 textures -- the pool did its job and the frame still cost **35k texture uploads**:
   * every field missed, because a bounded pool cannot remember 41000 distinct contents and consecutive fields evict each
   * other. At ~11 us of driver time per upload that was ~385 ms of a ~1300 ms frame, spent on calls whose payloads
   * averaged **21 bytes**.
   *
   * Packing the frame's fields into one texture turns that into **one upload of the whole atlas, and only when the
   * packed bytes change**. Three properties fall out of it, and each is the reason for one of the rules below:
   *
   *   1. a frame that draws the same scene as the last one uploads **nothing** -- what the image cache has always done
   *      for glyphs, and what a per-field pool can never do for fields;
   *   2. texture memory is a handful of atlases rather than a function of how many distinct shapes a scene has ever
   *      drawn, which is bounded memory *and* hit-carrying uploads at the same time;
   *   3. the frame's field payloads are one command instead of one per path, which is where the measured cost was.
   *
   * ## The one-texel border, which is what keeps the picture identical
   *
   * A field's texels are placed with **one texel of replicated edge** all round, and the fragment stage samples the
   * padded rect. That is not cosmetic: a texture of its own clamps to the field's edge texel, while an atlas would hand
   * back whatever field happens to be next to it. The tap outside the field has weight ~0 at a device pixel's centre --
   * that is the property `coverage.frag` states -- but "~0" is a rounding argument, and the border makes the answer
   * *exactly* the clamp-to-edge value instead of nearly it. It costs `2*(w+h)+4` texels per field, which measured on the
   * demo is 0.9 MiB for 41000 fields against the 858 KiB their texels occupy.
   *
   * ## Why the atlas is pooled by its extent and never destroyed
   *
   * A frame's fields are packed into the smallest atlas they fit in (see `packCoverageFields`), so the extent is a
   * property of the frame rather than a constant; pooling by it means a scene that draws 1000 fields and then 90000
   * reuses its textures instead of reallocating. Nothing is ever destroyed for the reason the previous pool was never
   * destroyed: the RHI has no contract for releasing a texture the GPU may still be reading
   * (`Device::destroyTexture` says nothing about resources in flight), so an eviction policy has nowhere safe to run.
   */
  struct FieldAtlas {
    backend::TextureHandle texture = backend::TextureHandle::invalid();
    backend::TextureViewHandle view = backend::TextureViewHandle::invalid();
    /** The set binding @c texture at binding 0, created against `mCoverageFieldSetLayout`. */
    backend::DescriptorSetHandle set = backend::DescriptorSetHandle::invalid();
    /** The atlas's extent in texels, and what the fragment stage divides a device position by to get a texel coordinate. */
    uint32_t width = 0;
    uint32_t height = 0;
    /** A hash of the bytes currently *in* @c texture, which is what decides whether a frame uploads it. */
    uint64_t contentHash = 0;
    /**
     * True once @c contentHash describes what is in @c texture.
     *
     * False while the texture holds only the zero fill its creation records, which is why it is a flag and not a sentinel
     * hash: every 64-bit value is a legal FNV result, so "0 means nothing uploaded" would be wrong exactly as often as a
     * frame's packed bytes happened to hash to it.
     */
    bool hasContent = false;
  };

  /**
   * @return the pooled atlas of extent @p width by @p height, creating it (and zeroing it once) if the pool has none.
   *
   * Keyed on the extent -- see `FieldAtlas` for why it is pooled at all and why nothing is ever destroyed.
   */
  FieldAtlas* ensureFieldAtlas(uint32_t width, uint32_t height);

  /**
   * Fills `Frame::fields` for @p frame: every coverage field the list names, with its coverage.
   *
   * **The first of the two stages step S5 splits the route into**, and the one that can be parallel: the fields are
   * independent of each other, so the whole table is cut into ranges and handed to `mJobs` (or run here, in order, when
   * there is no pool). The second stage is the replay's `drawCoverageFieldCommand`, which uploads and draws -- single
   * threaded because one GL context is.
   *
   * The contours a request is generated from are the ones the *command* that names it carries, which is why this walks
   * commands to build a field-to-command map before the parallel loop rather than searching inside it: two commands
   * naming one request field would otherwise race to write the same texels, and a map built front to back is
   * deterministic about which one wins.
   *
   * @return the result of the frame's bookkeeping; a field that could not be generated is counted in
   *         `Unsupported::coverageFields` and left empty, which the replay then skips -- a counted omission rather than
   *         a blank rectangle that looks deliberate.
   */
  backend::Result resolveCoverageFields(Frame& frame);
 public:
  /** Cumulative count of coveraged fields served from the byte cache; for the test that pins the win. */
  [[nodiscard]] uint64_t fieldByteCacheHits() const noexcept {
    return mFieldByteCacheHits;
  }
 private:

  /**
   * Places every one of @p frame's fields in the frame's atlases and sizes those atlases: `Frame::atlases`,
   * `Frame::placed`.
   *
   * Shelf packing, fields sorted by height, each with a one-texel replicated border -- see `FieldAtlas` for why the
   * border is not decoration and why the packing is a *frame* rather than a resource. It reads only the fields' **lattices**
   * and never their contents, which is the property the whole pipeline hangs on: it is what allows the atlases to exist
   * before the pixels do, so the scanline fill can write straight into them (see `resolveCoverageFields`).
   *
   * @return false when a field could not be placed at all, which the caller counts as an omission rather than drawing a
   *         field that samples somebody else's coverage. A refusal is all-or-nothing: the atlases it leaves behind are not
   *         a frame's worth of coverage and nothing is drawn from them.
   */
  bool placeCoverageFields(Frame& frame);

  /**
   * Fills `Frame::atlases`'s content hashes, which is what decides whether a frame uploads them at all.
   *
   * Since 4.1.48 it reads no atlas bytes for a field the fill derived: what the device samples is a pure function of the
   * field's content key and its placement, so the hash is an XOR of exactly those. A caller's own texels are the exception
   * (`hasCoverage`), and those boxes -- a handful -- are hashed for real by `hashFieldBox`.
   */
  void hashFieldAtlases(Frame& frame);

  /**
   * @return what one field contributes to its atlas's content hash: its content key (or its box's bytes, for a caller's own
   *         texels) mixed with the placement it was given. Zero for a field that draws nothing.
   *
   * The value is stable across frames for a field that was kept -- same content, same placement -- which is what lets the
   * running sums below be maintained instead of recomputed (4.1.49).
   */
  [[nodiscard]] uint64_t fieldContribution(const Frame& frame, uint32_t field) const;

  /**
   * Brings the per-atlas XOR sums up to date for this frame -- see the definition for why a partial refill only touches the
   * fields it refilled.
   *
   * @param refill        the fields this frame wrote, in frame order.
   * @param placementKept true when the placement is the one the sums were built from (a partial refill); false after a
   *                      rebuild, where every field was placed anew and every contribution has to be recomputed.
   */
  void updateFieldAtlasSums(const Frame& frame, const std::vector<uint32_t>& refill, bool placementKept, uint32_t count);

  /**
   * @return a hash of one field's **box** in the atlas: the bytes the shader can sample, with the box's identity mixed in.
   *
   * Used for the fields whose bytes cannot be derived from the list -- a caller's own texels -- and as the fallback for a
   * field with no identity of its own. A static member because it reads no renderer state: only the frame it is given.
   */
  static uint64_t hashFieldBox(const Frame& frame, uint32_t field);

  /**
   * Writes each changed field's box into @p atlas on its own, instead of the whole atlas.
   *
   * @return false when the caller must upload the whole thing (a box that is not where the placement says, or a write the
   *         device refused), true when every changed box in @p atlasIndex was written.
   *
   * The bulk of the atlas is unchanged on a partial refill by construction, and the texture already holds it, so the only
   * bytes it can be missing are the changed boxes -- which is why this is not just cheaper but correct (see the call site).
   */
  bool uploadChangedFieldBoxes(const Frame& frame, uint32_t atlasIndex, FieldAtlas& atlas);

  /**
   * How many fields a partial refill may have before uploading boxes stops being worth it.
   *
   * A box costs one `writeTexture` (a driver call and a staging copy) against the whole atlas's one; at some set size the
   * per-call overhead overtakes the bytes saved. 256 is far above the handful a real animation produces -- eight on the demo
   * -- and far below the 41084 a rebuild has, so the choice is not close.
   */
  static constexpr size_t kFieldBoxUploadLimit = 256;

  /** Row-gathered box bytes for `uploadChangedFieldBoxes`, reused across boxes and frames. */
  std::vector<uint8_t> mFieldUploadScratch;
  /** True when this frame kept the previous placement, which is what makes a box upload correct. */
  bool mFieldPlacementKept = false;

  /**
   * @return the atlas a coverage command's field was placed in, or a sentinel when it has none.
   *
   * The one piece of frame state the batching decision needs and a command does not carry: a command names a *field*, and
   * where that field's texels ended up is the packing's answer (`Frame::placed`). `sameCommandState` cannot see it, which
   * is why `canShareDraw` takes it as an argument and this is what computes it.
   *
   * The sentinel is returned for a command that is not a coverage field at all, and for a coverage command whose field
   * was omitted -- two such commands must never be taken for two commands in the same atlas.
   */
  [[nodiscard]] static uint32_t atlasOfCommand(const DrawList::Command& command,
          const std::vector<Frame::PlacedField>& placed) noexcept;

  /**
   * Draws one **batch** of coverage-field commands: makes sure the batch's atlas is uploaded, binds the program and the
   * atlas's descriptor set once, and issues one draw for every command in the batch.
   *
   * The replay's half of the coverage-field route, and the only half that lives here. The *recording* half -- turning a
   * shape into a field request and appending the command that names it -- is `GraphiteCanvasBackend::recordCoverageField`
   * for a path and `Surface::drawCoverageField` for a caller that already has a field; both go through
   * `gpu::appendCoverageFieldCommand` so that there is one construction of "a command that draws a field" rather than
   * one per caller. There used to be a third appender here, taking a `Frame&`; nothing could call it, because by the
   * time a `Frame` exists the geometry has been laid out and a command appended then is never drawn -- so it was
   * removed rather than left as a plausible-looking entry point that does nothing.
   *
   * The batch shape is what the atlas bought: with each field's place in the atlas in its *vertices*, a coverage command
   * has nothing left that differs per command, so a run of them is one draw -- which is where the route's cost was, since
   * the frame is bound by draw calls rather than by uploads.
   */
  backend::Result drawCoverageFieldBatch(Frame& frame, const Batch& batch);

  /**
   * One **image atlas page**: a texture shared by many bitmaps, and the allocator that places them.
   *
   * ## Why the image route needs one, and the field route's atlas does not answer it
   *
   * A coverage field is a per-*frame* quantity, so its atlas is packed from scratch every frame and its identity is
   * the frame. A glyph's bitmap is the same every time it is drawn, so the image route's cache is keyed on **content**
   * and lives for the session -- and that is exactly why it could not batch: `sameCommandState` compared
   * `state.image`, so two adjacent glyphs -- which necessarily name different images -- could never share a draw, and
   * the route ran at one draw per glyph (measured: 559 commands in 559 batches, `4.1.26`).
   *
   * A page replaces "the texture of one bitmap" with "a rectangle in a texture many bitmaps share", which is the same
   * move the field atlas made, with two differences that come from the lifetime:
   *
   *   1. the page is **persistent and content-addressed**, so a bitmap is placed and uploaded **once in a session**
   *      rather than once per frame -- measured on the demo, the image table is 18.8 MB of RGBA, so re-packing it every
   *      frame (the field atlas's shape) would cost tens of milliseconds a frame to save 7 ms of draws (`4.1.31`);
   *   2. nothing is ever freed, for the reason `ImageShelfAllocator` states, so the page set only grows: a full page
   *      stays as it is and a new one is started, and past `kImageAtlasPageBudget` a bitmap falls back to a texture of
   *      its own.
   *
   * ## Why the page's own texture is what the batch binds
   *
   * One descriptor set per page, created against `mQuadSetLayout` (whose binding 0 is the sampler `image.frag` reads),
   * and every command in a batch samples that one texture -- at different sub-rectangles, which is per-vertex data. A
   * bitmap too large for a page keeps the old arrangement: a texture, a view and a set of its own, with `textureId`
   * taken from the same counter so that `canShareDraw` cannot mistake the two kinds for each other.
   */
  struct ImageAtlasPage {
    ImageAtlasPage(const uint32_t pageWidth, const uint32_t pageHeight) noexcept
      : allocator(pageWidth, pageHeight), width(pageWidth), height(pageHeight) {}

    backend::TextureHandle texture = backend::TextureHandle::invalid();
    backend::TextureViewHandle view = backend::TextureViewHandle::invalid();
    /** The set binding @c texture at binding 0, created against mQuadSetLayout, sampling **linear**. */
    backend::DescriptorSetHandle set = backend::DescriptorSetHandle::invalid();
    /**
     * The same set with a **nearest** sampler.
     *
     * A page holds bitmaps drawn with either filter, and the sampler lives in the set, so a page needs one of each --
     * that is what lets a nearest image stay in the atlas instead of needing a texture of its own.
     */
    backend::DescriptorSetHandle setNearest = backend::DescriptorSetHandle::invalid();
    /** Where the next bitmap goes; see `ImageShelfAllocator` for why it never frees and why it is a shelf. */
    ImageShelfAllocator allocator;
    /** The page's extent in texels, and what a sub-rectangle is normalized against. */
    uint32_t width = 0;
    uint32_t height = 0;
    /**
     * The page's identity for batching, unique among pages **and** standalone textures.
     *
     * A separate counter rather than the page's index, because the two kinds share one namespace in `canShareDraw`:
     * a bitmap in a texture of its own must not compare equal to a page because both happen to be "number 3".
     */
    uint32_t textureId = 0;
  };

  /**
   * Where one bitmap lives: a rectangle in a page, or a texture of its own.
   *
   * The value side of `mImagePlacements`, which is keyed on a hash of the bitmap's pixels -- so this is a **cache
   * entry**, and the one thing it must never do is describe a different bitmap than the one it was created for.
   */
  struct ImagePlacement {
    /** Which texture this bitmap is sampled from, for `canShareDraw`. */
    uint32_t textureId = kNoSampledTexture;
    /** True when the bitmap is in a page (in which case @c texture/@c view/@c set are invalid). */
    bool inPage = false;
    /** The index into `mImagePages` when @c inPage. */
    uint32_t page = 0;
    /**
     * The **padded** rectangle's corner inside that texture: the bitmap's own texels start one texel in from it,
     * because the rectangle the allocator reserved carries the replicated border.
     */
    uint32_t x = 0;
    uint32_t y = 0;
    /** How this placement samples, and part of its cache key. */
    SamplingOptions sampling = {};
    /** How many levels the texture has. 1 for everything that did not ask for mipmapping. */
    uint32_t mipLevelCount = 1;
    /** The bitmap's own size, which is what the sampled sub-rectangle is. */
    uint32_t width = 0;
    uint32_t height = 0;
    /** `(u0, v0, du, dv)` for `image.frag`, normalized against the **texture** the bitmap lives in. */
    std::array<float, 4> rect { 0.0f, 0.0f, 1.0f, 1.0f };
    /**
     * True once the bitmap's pixels have been recorded for upload.
     *
     * The one piece of per-placement state the frame consults: `uploadPlacedImages` walks the frame's placements and
     * records an upload for each entry that is not yet up, so the upload happens exactly once per distinct bitmap per
     * session -- and, since a placement is shared by every image index with the same pixels, exactly once between two
     * frames that draw the same text.
     */
    bool uploaded = false;
    /** A hash of the pixels, so that a placement knows what it was made for. */
    uint64_t contentHash = 0;
    /** The bitmap's own texture, used when it is not in a page. */
    backend::TextureHandle texture = backend::TextureHandle::invalid();
    backend::TextureViewHandle view = backend::TextureViewHandle::invalid();
    backend::DescriptorSetHandle set = backend::DescriptorSetHandle::invalid();
  };

  /**
   * Fills `Frame::images` for @p frame: every bitmap the frame's image commands name, placed somewhere samplable.
   *
   * **The image route's placement stage**, and the reason it exists as a stage rather than as a lookup inside the draw:
   * the vertex writer needs each command's sub-rectangle and the batching needs each command's texture *before* the
   * arena and the batches exist, and the descriptor set a batch binds is one per texture rather than one per command.
   * Nothing is uploaded here -- this stage only decides where things go and creates the page a bitmap needs -- so it
   * touches no command buffer and can run before one is open.
   *
   * A bitmap that cannot be placed at all leaves its frame entry `valid` false, which the replay counts as an omission;
   * a page that cannot be created is not fatal to the frame.
   */
  void placeImages(Frame& frame, const std::vector<Bitmap>& images);

  /**
   * @return where @p image should be sampled from, placing it (and creating a page if that is what it takes) on the
   *         first sight of those pixels, or null when no texture could be made for it.
   *
   * Keyed on a hash of the pixels, which is the same content key the per-image texture cache used and for the same
   * reason: a frame numbers its images from zero and allocates a fresh table, so an index-keyed cache can never hit
   * across frames and every glyph was re-uploaded every frame.
   */
  ImagePlacement* ensureImagePlacement(const Bitmap& image, const SamplingOptions& sampling,
    uint64_t knownHash = 0);

  /**
   * @return a sampler for @p sampling, created on first use and cached.
   *
   * One sampler per distinct filter combination rather than a descriptor rebuilt per draw: a frame asks for a handful of
   * combinations at most. The `MipmapMode` becomes the sampler's mipmap filter, and it only has an effect on a texture
   * that actually carries levels -- everything else samples level 0, which is what it did before.
   */
  backend::TextureSamplerHandle samplerFor(const SamplingOptions& sampling);

  /** Samplers by filter combination; see `samplerFor`. */
  std::unordered_map<uint32_t, backend::TextureSamplerHandle> mSamplers;

  /**
   * @return the page a bitmap should go in, growing the page set by one when no existing page has room, or null when
   *         the budget is spent or the page could not be created.
   *
   * @param outIndex/@p outX/@p outY the placement the allocator made, written only on success. An existing page is tried
   *        first, in creation order, so a page fills before the next one exists.
   */
  ImageAtlasPage* pageWithRoomFor(uint32_t paddedWidth, uint32_t paddedHeight, uint32_t* outIndex, uint32_t* outX,
          uint32_t* outY);

  /** Releases whatever device objects @p placement owns and marks it unplaced. Safe to call twice. */
  void releaseImagePlacement(ImagePlacement& placement) noexcept;

  /**
   * Records the upload of every bitmap this frame placed whose pixels are not yet in a texture.
   *
   * The image route's second stage, and the one that is deliberately **marginal**: a bitmap is uploaded the first frame
   * a session draws it and not again, so a frame that draws the same text as the last one records no texture write at
   * all. It has to run while the frame's command buffer is open -- the write is ordered with the draws that sample it,
   * the same rule every other upload here follows -- and it has to finish before the first image batch is drawn, which
   * is why `recordFrame` calls it before the batch walk.
   *
   * A bitmap in a page is written as **one sub-rectangle** covering the bitmap plus its replicated border, so the page
   * is never re-uploaded whole and the border costs one extra texel of payload per row. A bitmap whose upload fails is
   * turned into an omission rather than drawn: the write is what puts the pixels there, and a rectangle the driver never
   * touched is whatever it left in the page.
   */
  void uploadPlacedImages(Frame& frame, const std::vector<Bitmap>& images);

  /**
   * @return which texture a command samples, or `kNoSampledTexture` when the frame could not resolve one.
   *
   * The one piece of per-draw state that lives in the **frame** rather than in the command, and therefore the only
   * thing `canShareDraw` cannot read off the two commands it compares: a coverage command names a *field* and where
   * that field's texels ended up is the packing's answer, while an image command names a *bitmap* and where that bitmap
   * lives is the placement's. Both are resolved here so that the batching rule has one shape for both routes.
   */
  [[nodiscard]] static uint32_t sampledTextureOfCommand(const DrawList::Command& command, const Frame& frame) noexcept;

  backend::Result drawPath(Frame& frame, const DrawList::Command& command, const Precompiled& precompiled);
  backend::Result compositeLayer(Frame& frame, const DrawList::Command& command);
  backend::Result compositeThroughShader(Frame& frame, uint32_t parentIndex, uint32_t sourceIndex, const Rect& region,
    float alpha, uint8_t blendMode, const Layer& layer);

  backend::Result beginPassFor(Frame& frame, uint32_t layerIndex, bool clearOnFirstOpen);
  backend::Result endPass(Frame& frame);
  backend::Result setScissor(Frame& frame, const Rect& clip);

  // ------------------------------------------------------------------------------------------------
  // State
  // ------------------------------------------------------------------------------------------------

  backend::Device* mDevice = nullptr;
  /**
   * The device this renderer created, when it created one.
   *
   * A `unique_ptr` rather than a raw pointer and a flag, because the RHI's only way to release a device
   * is to destroy it, and a renderer that forgot on one early-return path would leak a GL context --
   * which on Windows is a hidden window's worth of driver state, not a few bytes.
   *
   * Declared *before* every cached resource below, so that it is destroyed *last*: members are
   * destroyed in reverse declaration order, and every resource in this class belongs to the device.
   */
  std::unique_ptr<backend::Device> mOwnedDevice;
  utils::JobSystem* mJobs = nullptr;

  bool mReady = false;
  size_t mEmbeddedShaderCount = 0;

  // --- cached device objects ---
  backend::DescriptorSetLayoutHandle mQuadSetLayout = backend::DescriptorSetLayoutHandle::invalid();
  backend::DescriptorSetLayoutHandle mSdfPatternSetLayout = backend::DescriptorSetLayoutHandle::invalid();
  backend::PipelineLayoutHandle mSdfPatternPipelineLayout = backend::PipelineLayoutHandle::invalid();
  backend::TextureHandle mSdfPatternTexture = backend::TextureHandle::invalid();
  backend::TextureViewHandle mSdfPatternView = backend::TextureViewHandle::invalid();
  Bitmap mSdfPatternBitmap;
  bool mSdfPatternDirty = false;
  bool mSdfPatternEnabled = false;
  backend::DescriptorSetLayoutHandle mPathSetLayout = backend::DescriptorSetLayoutHandle::invalid();
  backend::DescriptorSetLayoutHandle mTexturedSetLayout = backend::DescriptorSetLayoutHandle::invalid();
  backend::PipelineLayoutHandle mQuadPipelineLayout = backend::PipelineLayoutHandle::invalid();
  backend::PipelineLayoutHandle mPathPipelineLayout = backend::PipelineLayoutHandle::invalid();
  backend::PipelineLayoutHandle mTexturedPipelineLayout = backend::PipelineLayoutHandle::invalid();

  backend::DescriptorSetHandle mQuadSet = backend::DescriptorSetHandle::invalid();
  /**
   * The path families' set: the canvas block at binding 1 and nothing else.
   *
   * The fill colour is a vertex attribute now, so every path draw in a frame binds this one set; there
   * is no per-draw block to allocate or range-offset.
   */
  backend::DescriptorSetHandle mPathSet = backend::DescriptorSetHandle::invalid();

  /**
   * The presentation pass's set: the source texture at 0, the white fallback at 1 and its block at 2,
   * on `mTexturedSetLayout`.
   *
   * There is one because every presentation submission is executed synchronously -- `present` submits a
   * command buffer and waits -- so a set rewritten for the next pass cannot be read by a command buffer
   * still in flight. A backend with an asynchronous submit would need a ring here, which is why the
   * lifetime rule is stated rather than left to the reader.
   */
  backend::DescriptorSetHandle mPresentSet = backend::DescriptorSetHandle::invalid();
  /** The six-vertex full-target quad the presentation pass draws, built once. */
  backend::BufferHandle mPresentVertexBuffer = backend::BufferHandle::invalid();

  /**
   * The gradient's own set layout, with binding 1 naming the canvas block and binding 2 the gradient block.
   *
   * Its own layout rather than a reuse of the quad's, even though both declare a block at binding 1, because a set is
   * only valid with a *compatible* layout: the gradient program is created against this layout (it shares `quad.vert`),
   * while the quad's layout declares a sampler at binding 2 where the gradient needs its block. The two happen to describe
   * the same binding *number*, and relying on that is how a program ends up reading a buffer it was never given -- which
   * is what the gradient did, painting `sampleStops(0)`'s fallback while every CPU-side trace said the stops were there.
   */
  backend::DescriptorSetLayoutHandle mGradientSetLayout = backend::DescriptorSetLayoutHandle::invalid();
  backend::PipelineLayoutHandle mGradientPipelineLayout = backend::PipelineLayoutHandle::invalid();
  // The coverage field's own set layout and pipeline layout: a sampler at binding 0 and its block at 1, which
  // neither the gradient's pair (two blocks) nor the quad's (a sampler and a differently sized block) can serve.
  backend::DescriptorSetLayoutHandle mCoverageFieldSetLayout = backend::DescriptorSetLayoutHandle::invalid();
  backend::PipelineLayoutHandle mCoverageFieldPipelineLayout = backend::PipelineLayoutHandle::invalid();
  /** The set the coverage-field route binds: its sampler at 0 and this frame's block at 2. */
  backend::DescriptorSetHandle mCoverageFieldSet = backend::DescriptorSetHandle::invalid();

  /**
   * The descriptor sets that name one per-draw uniform slot, indexed by slot.
   *
   * ## Why one set per slot rather than one per program shape
   *
   * Both real backends resolve a draw's descriptor set *late*: the OpenGL backend replays a recorded set handle and reads
   * its contents when the command list executes, while the Vulkan backend allocates a descriptor snapshot per
   * `(set, version)` at draw time. A single set refilled for draw 2 therefore hands draw 1 draw 2's block on OpenGL, and a
   * single *range* shared by two draws has the same problem on Vulkan once the writes stop being ordered commands.
   *
   * ## Why the writes are no longer ordered commands
   *
   * The blocks used to be uploaded by `CommandBuffer::writeBuffer`, which records a `vkCmdCopyBuffer` into the frame's
   * command buffer. That is legal outside a render pass and **illegal inside one** -- Vulkan forbids `vkCmdCopyBuffer`
   * (and the barrier the backend inserts after it) inside a render pass instance, and the canvas writes uniforms in the
   * middle of a layer pass. The validation layer reported exactly that (VUID-vkCmdCopyBuffer-renderpass).
   *
   * The fix is to stop putting uploads in the command stream: each draw takes its own slot in a host-visible arena, the
   * bytes are staged on the CPU, and one mapped write per frame delivers the whole arena before the frame is submitted.
   * A slot is never written again within the frame, so the missing ordering between a mapped write and the draws does not
   * matter -- by execution time every slot holds exactly its own draw's data.
   */
  std::vector<backend::DescriptorSetHandle> mGradientSlotSets;

  /**
   * One descriptor set per composite *position*, created once and reused across frames.
   *
   * A composite cannot share a set with another composite -- the backend resolves a draw's set contents at replay
   * while the fill happens during recording, so a shared set gives every composite the last fill; see
   * `compositeSetFor`. Indexed by command index rather than created per composite so the count follows the frame's
   * *shape* rather than its content, which is the same property the layer pool has.
   *
   * All three composite programs draw through this pool, and they can share it because at most one of them runs per
   * closing command: the destination-as-a-texture composite (`compositeThroughShader`), the blend-unit copy on the mask
   * program, and the mask itself. Each writes the bindings its own program reads, overwriting whatever the previous
   * frame's composite at that position left.
   */
  std::vector<backend::DescriptorSetHandle> mCompositeSets;

  /**
   * @return the set the composite at @p compositeIndex draws through, creating it against the textured layout on first
   *         use.
   *
   * ## Why one set per composite, stated once
   *
   * The backend records a draw's descriptor sets as a *handle* and resolves the set's contents at replay, while
   * `updateDescriptorSet` writes the set on the CPU during recording. So a single set refilled for composite 2 and again
   * for composite 3 holds composite 3's fill by the time *any* of them executes: every composite reads the last layer.
   * A trace of three layered rectangles showed all three binding `set 4`, and the frame came out as one patch in the
   * first rectangle's region carrying the last layer's content.
   *
   * There is no arrangement of a single shared set that fixes that, so each composite takes its own. That is the same
   * rule the image path already follows (one set per image) and the same rule the per-draw uniform sets need
   * (`mGradientSlotSets`): a resource a draw reads must not be rewritten before that draw has executed.
   *
   * ## And why it is now shared by all three composite programs
   *
   * The destination-as-a-texture path used to keep one set of its own (`mCompositeSet`) and refill it per composite,
   * which is precisely the arrangement above that cannot work. It looked correct because the *last* composite of a frame
   * read the fill the set still held -- measured on the demo's blend panel, ten of twelve swatches composited one
   * layer's pixels and only the twelfth was right. The measured cost of the pooling (10.4% of inked pixels against the
   * rasterizer on `path.svg`, against 2.1% for the shared set) is recorded at the call site; it is taken because the
   * alternative is a composite that silently reads another layer.
   */
  backend::Result compositeSetFor(uint32_t compositeIndex, backend::DescriptorSetHandle* outSet);

  /**
   * One live uniform buffer: the GPU-visible resource and its size in bytes.
   *
   * ## Why there is a list of them rather than one buffer
   *
   * The arena is sized from the frame it has to serve, and a later frame may need more slots than the buffer that was
   * created first. Growing by destroying and recreating one buffer would invalidate every descriptor set ever built
   * against it, so growth appends a larger buffer instead and the frame's staged bytes are written to **every** live
   * buffer. A set built against an older, smaller buffer still names a slot below that buffer's size, so it still reads
   * this frame's data; sets built from then on name the newest buffer. The list is bounded by the doubling steps and is
   * released with everything else at teardown.
   */
  struct UniformBuffer {
    backend::BufferHandle handle = backend::BufferHandle::invalid();
    uint64_t bytes = 0;
  };
  std::vector<UniformBuffer> mUniformBuffers;

  /** The frame's CPU-side staging area: `kUniformSlotSize` bytes per allocated slot. */
  std::vector<uint8_t> mUniformArena;
  /** How many slots are used this frame; slot 0 is always the canvas block. */
  uint32_t mUniformSlotsUsed = 0;

  backend::BufferHandle mVertexBuffer = backend::BufferHandle::invalid();
  uint64_t mVertexCapacity = 0;
  /**
   * The static quad index pattern, and its capacity in indices -- see `ensureIndexCapacity`.
   *
   * Its indices are **batch-relative**: a draw sets the vertex buffer to its own batch's `vertexOffset` and then uses
   * indices `0 .. 4N-1`, so one pattern of `6 x (the largest batch's command count)` serves every batch in the frame.
   * That is also why the capacity follows the largest *batch* rather than the frame: a frame of ten thousand batches of
   * one command needs six indices, and a frame of one batch of ten thousand commands needs sixty thousand.
   */
  backend::BufferHandle mIndexBuffer = backend::BufferHandle::invalid();
  uint64_t mIndexCapacity = 0;

  /** @return the buffer new descriptor sets should name: the newest one, which is large enough for this frame. */
  [[nodiscard]] backend::BufferHandle uniformBuffer() const noexcept {
    return mUniformBuffers.empty() ? backend::BufferHandle::invalid() : mUniformBuffers.back().handle;
  }

  /**
   * Stages one block into a fresh slot of the frame's uniform arena.
   *
   * @param data the block's bytes; @p size must fit one slot.
   * @param outSlot the slot index to name in a descriptor set. Blocks are never reused within a frame, which is what
   *                replaces the ordering a recorded upload used to provide.
   *
   * Host-side rather than a command: see `mGradientSlotSets` for why an upload may not be recorded inside a render pass.
   */
  backend::Result allocateUniformSlot(const void* data, size_t size, uint32_t* outSlot);

  /**
   * Ensures the arena can hold @p slots, appending a larger uniform buffer when it cannot.
   *
   * Called once per frame before recording, from an upper bound over the frame's batches: a buffer that appeared in the
   * middle of recording would leave the sets earlier draws captured pointing at a different buffer.
   */
  backend::Result ensureUniformCapacity(uint32_t slots);

  /**
   * Delivers the frame's staged arena to every live uniform buffer.
   *
   * One mapped write per buffer, called after the frame is recorded and before it is submitted. This is the only moment
   * the uniform bytes cross to the GPU, and because it is host-side it is not a command in the render pass.
   */
  backend::Result uploadUniforms();

  /**
   * @return a set for @p slot, created against @p layout on first use and cached in @p cache.
   *
   * The set names the canvas block at binding 1 and the slot's block at binding 2 -- the shape both the path and the
   * gradient layouts declare. Created on the CPU during recording, which is legal: only *commands* are restricted inside
   * a render pass, not the descriptor bookkeeping that precedes a draw.
   */
  backend::Result uniformSetForSlot(std::vector<backend::DescriptorSetHandle>& cache,
          backend::DescriptorSetLayoutHandle layout, uint32_t slot, backend::DescriptorSetHandle* outSet);

  /**
   * @return the descriptor set that binds @p view at binding 0, created on first use and cached.
   *
   * An external image's texture belongs to another producer, so it has no `ImagePlacement` and no
   * atlas page -- only a view. The set is created against `mQuadSetLayout`, the same layout an image's
   * set uses, and cached by the view's handle value because a ring of producer textures repeats its
   * views every frame. The caller destroys the cached sets with the rest of the device resources.
   *
   * @return an invalid set when the device refuses to create or fill it; the command that needed it is
   *         then counted as an omission rather than drawn with another texture bound.
   */
  backend::DescriptorSetHandle externalSetFor(backend::TextureHandle texture,
          backend::TextureViewHandle view);

  backend::TextureSamplerHandle mLinearClamp = backend::TextureSamplerHandle::invalid();
  backend::TextureSamplerHandle mNearestClamp = backend::TextureSamplerHandle::invalid();

  /** A 1x1 opaque white texture: what the quad pass binds where it has nothing else to sample. */
  backend::TextureHandle mWhiteTexture = backend::TextureHandle::invalid();
  backend::TextureViewHandle mWhiteTextureView = backend::TextureViewHandle::invalid();
  /** The 1x1 coverage texture the current layer composite reads; one of `mAlphaTextures`. */
  backend::TextureHandle mAlphaTexture = backend::TextureHandle::invalid();
  backend::TextureViewHandle mAlphaTextureView = backend::TextureViewHandle::invalid();

  backend::CommandBuffer* mCommandBuffer = nullptr;
  /**
   * A second command buffer, for the uploads that cannot be recorded into the frame's.
   *
   * A texture upload is a payload-carrying command, and the frame's command buffer must contain none of
   * them (see `uploadBuffer`); an upload that happens to be needed while the frame is being recorded --
   * a layer whose alpha has not been seen before -- therefore goes into this one, which is executed
   * immediately so the texture is ready before the frame runs.
   */
  backend::CommandBuffer* mUploadCommandBuffer = nullptr;

  std::vector<PipelineEntry> mPipelines;
  std::vector<PooledTarget> mLayerPool;
  std::vector<ScratchTexture> mScratchPool;
  std::vector<AlphaTexture> mAlphaTextures;

  // --- per-frame state, kept so that a steady frame allocates nothing structural ---
  /**
   * The field tables `mFrame` refers to: the resolve stage's output, and the cross-frame field cache's contents.
   *
   * Declared **before** `mFrame`, which holds references to them, because members are destroyed in reverse declaration
   * order: the frame's references are gone before their targets are. That is the same argument `mOwnedDevice` carries for
   * being the first member of this class, and it is the reason these two are here rather than beside `mFieldCache`.
   *
   * Their contents are the last resolve stage's output -- see `Frame`'s constructor for what that means for a frame that
   * reuses them and for one that replaces them.
   */
  std::vector<Frame::AtlasUpload> mFieldAtlasBytes;
  std::vector<Frame::PlacedField> mFieldPlaced;
  Frame mFrame;
  uint64_t mFrameCount = 0;
  bool mAsyncSubmissions = false;
  bool mFrameInFlight = false;
  /**
   * Bytes of the vertex buffer the lists recorded earlier in this frame already used.
   *
   * A frame assembled from several lists appends each list arena after the previous one and adds this
   * base to every vertex offset of that list, so one command buffer can hold them all.
   */
  uint64_t mVertexBase = 0;
  /** A multi-list frame is open: the command buffer and the arenas stay open between its lists. */
  bool mFrameOpen = false;
  /**
   * The open frame's stage timings, summed over the lists it is made of.
   *
   * `mStats` is reset at the start of every `submit`, so on a multi-list frame it only ever showed the
   * last list's work -- exactly the frame a plan-built frame is. The totals are what say which stage the
   * frame actually spends its CPU in, and they are reported through `lastFrameStats()` once the frame is
   * submitted.
   */
  FrameStages mFrameStageTotals{};
  /** Draw and batch counts summed over the open frame, for the same reason as the stage totals. */
  struct FrameCounts {
    uint32_t batches = 0;
    uint32_t drawCalls = 0;
    uint32_t commands = 0;
    uint32_t pathDraws = 0;
    uint32_t imageDraws = 0;
  };
  FrameCounts mFrameCounts{};
  /** The uniform arena keeps the slots that an earlier list of this frame allocated. */
  bool mUniformArenaContinues = false;
  bool mFrameFenceSignalled = false;
  backend::FenceHandle mFrameFence = backend::FenceHandle::invalid();
  /**
   * How many textures have been uploaded over this renderer's life.
   *
   * The number the content-keyed image cache exists to keep small. With an index-keyed cache it grew by every image and
   * every glyph in every frame -- a demo frame with a few hundred glyphs added a few hundred uploads -- and with the
   * content key the same glyph in the next frame finds its texture already there.
   */
  uint64_t mImageUploads = 0;

public:
  /** @return textures uploaded over this renderer's whole life, which is the image cache's observable. */
  [[nodiscard]] uint64_t textureUploadCount() const noexcept { return mImageUploads; }

  /**
   * @return how many field atlases the pool holds.
   *
   * The number to read beside a frame's upload count: the atlas route means a frame uploads **one texture per atlas whose
   * packed bytes changed**, so a frame that draws the same scene as the last one uploads nothing at all -- see
   * `FieldAtlas`.
   */
  [[nodiscard]] uint32_t pooledFieldAtlasCount() const noexcept {
    return static_cast<uint32_t>(mFieldAtlases.size());
  }

  /**
   * @return how many image atlas pages exist.
   *
   * The number to read beside a frame's image draw count: the route's whole point is that many glyphs share one texture,
   * so a scene's text should cost a page or two rather than one texture per glyph -- and a number that grows every frame
   * would say that bitmaps are being placed as if they were new.
   */
  [[nodiscard]] uint32_t imageAtlasPageCount() const noexcept {
    return static_cast<uint32_t>(mImagePages.size());
  }

private:
  ContextImpl::FrameStats mStats;
  /** The timestamp query set for `gpuMilliseconds`; invalid when the device cannot time. */
  backend::QuerySetHandle mTimingSet = backend::QuerySetHandle::invalid();
  bool mTimingEnabled = false;
  /** A pair was recorded into the frame that is in flight, so its result is not readable yet. */
  bool mTimingPending = false;
  double mGpuMilliseconds = 0.0;
  /**
   * Where the bitmaps a session has drawn live, keyed by a hash of their pixels.
   *
   * ## Why the key is content and not the image index
   *
   * A `DrawList` numbers its images from zero, and a frame allocates a fresh list -- so the same index names a different
   * bitmap in the next frame. An index-keyed cache therefore cannot ever hit across frames: every glyph and every image
   * was re-uploaded every frame, and the entry left behind had to be destroyed or it leaked. Measured on the demo that
   * is ~500 GPU texture uploads per frame for text alone.
   *
   * Keyed on the pixels, the *same* glyph in the next frame finds its place in a page and uploads nothing. The glyph
   * bitmaps a font produces are few and repeated, which is exactly the access pattern a content key is for.
   *
   * ## What it costs
   *
   * An entry is never evicted, so the table grows with the number of *distinct* bitmaps ever drawn rather than with the
   * number of draws. That is bounded and small for text and for the images a UI uses, and unbounded for a caller that
   * feeds a new video frame every frame -- which is why this is stated here rather than left to be discovered. Eviction
   * needs a policy and a frame at which a texture is provably unused, and neither exists yet.
   */
  std::unordered_map<uint64_t, ImagePlacement> mImagePlacements;

  /**
   * The last frame's resolved coverage fields, kept so that a frame asking for the same ones does not rasterise them
   * again.
   *
   * ## Why a cache is worth having now, when it was measured as not worth having before
   *
   * 4.1.10 asked this question and answered it from a measurement: *"生成的场没有跨帧缓存…生成只占一帧的 2%，不值得的正是
   * 这一项"*. That measurement was made on a 1300 ms frame whose cost was 48252 draw calls, and the premise -- that
   * generating fields is a rounding error -- has since been removed by two rounds that made the frame 46x shorter: the
   * stage accounting of 4.1.35 puts the field stage at **50% of a steady frame with the job pool** (14 of 28 ms) and 77%
   * without it (50 of 64 ms), because the draws it was hiding behind went away.
   *
   * ## What makes it safe: the key covers everything the bytes depend on, and errs toward missing
   *
   * The atlas bytes are a pure function of the fields' lattices in order (which is what the placement is computed from)
   * and of each field's coverage (which is a function of its polygons and its fill rule). `fieldFrameKey` folds all of
   * that -- every request's lattice, fill rule and contour range, every contour in the list, and the whole points array
   * -- plus a rule that a request carrying a caller's own texels makes the frame **uncacheable** rather than hashable.
   * The fold is deliberately wider than the fields actually use: a change to contours or points no field names is a
   * *miss*, and a miss costs a fill, while the opposite mistake is the defect this area keeps meeting -- an atlas one
   * frame stale is a picture of the frame before, with nothing reported anywhere.
   *
   * ## Why the bytes are not stored here
   *
   * They are `mFieldAtlasBytes`, the table `Frame::atlases` refers to: one owner, in place across frames, so a hit costs
   * neither a rasterise nor a copy in either direction. An earlier version of this cache held its own copy and handed it
   * to the frame on a hit -- correct, and 1.5-1.8 ms of a 2.9-3.4 ms stage for the privilege (4.1.35 measured it). What
   * remains here is only what a hit has to *know*: the key, whether it is valid, and the two numbers that let a reused
   * frame report the same field count and texel area as the frame that generated them.
   */
  /**
   * How much room a field's slot gets beyond the field itself, in texels.
   *
   * Four, because that is what the measurement said is enough: with slots rounded to four, **every** changed field of both
   * animated runs fitted the box it had (4 of 4 under `--animate`, 141 of 141 under `--jitter`), and with exact-size slots
   * *none* of them did. It is the spatial granularity of the atlas's slots -- bigger wastes atlas area, smaller loses the
   * reuse it exists for.
   */
  static constexpr uint32_t kFieldSlotGranularity = 4;

  /**
   * What one coverage field is, for the cross-frame cache: the lattice, which decides **where** its bytes live, and a
   * content key, which decides **what** they are.
   *
   * ## Why this replaced a single key over the whole frame (4.1.45)
   *
   * The frame key folded every point in the list, so one field's geometry changing answered "different" for all 41084
   * fields and the stage re-rasterised every one of them. 4.1.43 measured what that costs in an animation where one panel
   * moves: **99.98%** of the fields are byte-identical to the previous frame's, and the frame-level cache served **0%** of
   * them -- 16.1 ms of a 32.7 ms frame. Per field, the question has two parts, and they are the two this struct carries:
   *
   *   * the **lattice** -- origin, extent, fill rule -- because that is what the placement packs, and a field can keep its
   *     bytes only if its slot still holds them;
   *   * the **content key** -- the lattice again plus the points of the contours the field names -- because that is what
   *     the rasteriser reads, and equal keys mean byte-equal coverage.
   *
   * A caller's own texels (`hasCoverage`) are never keyed: their bytes are in the request and can change without anything
   * else changing, which is what a moving mask is. Such a field is refilled every frame, and that is a property of the
   * field rather than of the frame it arrived in.
   */
  struct FieldLattice {
    int32_t originX = 0;
    int32_t originY = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fillType = 0;
    /** A request with no extent draws nothing, and is skipped by both the placement and the fill. */
    bool empty = true;
    bool hasCoverage = false;
    uint64_t contentKey = 0;

    /**
     * @return true when a slot allocated for **this** lattice still holds @p other's padded box.
     *
     * The slot, not the box: the placement rounds every box up to `kFieldSlotGranularity`, and that slack is exactly what
     * lets an animated field grow by a texel and stay where it is. Comparing against the unrounded box instead would make
     * every growing field invalidate the whole placement -- measured, and the reason this reads `roundUp`.
     */
    [[nodiscard]] bool slotHolds(const FieldLattice& other) const noexcept {
      const uint32_t slotWidth = (width + 2 + kFieldSlotGranularity - 1) / kFieldSlotGranularity * kFieldSlotGranularity;
      const uint32_t slotHeight =
              (height + 2 + kFieldSlotGranularity - 1) / kFieldSlotGranularity * kFieldSlotGranularity;
      return other.width + 2 <= slotWidth && other.height + 2 <= slotHeight;
    }

    [[nodiscard]] bool sameLattice(const FieldLattice& other) const noexcept {
      return originX == other.originX && originY == other.originY && width == other.width &&
             height == other.height && fillType == other.fillType;
    }
  };

  /**
   * The frame-to-frame cache of resolved coverage fields -- see `FieldFrameCache`.
   *
   * One entry, not a table, because the question it answers is "is this frame's field work the same as the last one's"
   * -- and because the frame key folds every point in the list, a frame in which one shape moved answers *no* for all
   * of its fields.
   *
   * ## What the measurement changed about that argument (4.1.43, 4.1.45)
   *
   * This note used to say that a table keyed on content would grow with every distinct shape an animation produces,
   * which is the unbounded-cache problem the image route solved with a page budget. That argument is about a *table of
   * shapes*, and 4.1.43 measured that the shape worth having is not that:
   *
   *   * an animated frame of the demo is **99.98%** content-identical fields to the frame before it (`--animate`, one
   *     panel moving) and **99.57%** in the worst case measured (`--jitter`), while this cache served **0%**;
   *   * a window of **one** frame captures all of it -- windows of 1, 4, 16 and 64 frames give identical numbers, so
   *     nothing needs to be remembered longer than the previous frame;
   *   * and the reused bytes are the **atlas that is already retained** here, so the only new memory is one identity per
   *     field (41084 x 32 bytes, about 1.3 MB a frame).
   *
   * So what this entry holds is the previous frame's **identities** rather than one key, and a frame whose fields all
   * still fit the slots they had is *partially refilled*: the atlas keeps every byte it had, and only the fields whose
   * content key changed -- 8 of 41084 on the demo's animation -- are rasterised again. 4.1.45 measured the precondition
   * that makes that possible: with each slot rounded up to a granularity of four texels, **every** changed field in both
   * animated runs still fitted the box it had (4 of 4 under `--animate`, 141 of 141 under `--jitter`).
   */
  struct FieldFrameCache {
    /** True once `lattices` describes what `mFieldAtlasBytes` and `mFieldPlaced` currently hold. */
    bool valid = false;
    /**
     * How many fields that frame's atlas holds, how many fields the frame itself asked for, and the texels filled.
     *
     * `fieldCount` is what the stage *generated* (a frame with a field it could not place or a caller's field with too few
     * texels generates fewer than it asks for), `frameFieldCount` is what it asked for, and `fieldArea` is the texels the
     * fill covered. The last two exist so a frame that changes nothing can report the same numbers as the frame that did
     * the work -- which is what the statistics mean: `coverageFieldCount` is the fields the atlas holds, and
     * `coverageFieldsReused` is how many of them nobody rasterised this time.
     */
    uint32_t fieldCount = 0;
    uint32_t frameFieldCount = 0;
    uint64_t fieldArea = 0;
    /** One entry per field of the frame that produced the atlas, in the frame's own order. */
    std::vector<FieldLattice> lattices;
  };

  /**
   * The frame-to-frame cache of resolved coverage fields -- see `FieldFrameCache`.
   *
   * One entry, not a table, because the question it answers is "is this frame's field work the same as the last one's"
   * -- and because the frame key folds every point in the list, a frame in which one shape moved answers *no* for all
   * of its fields.
   *
   * ## What the measurement changed about that argument (4.1.43)
   *
   * This note used to say that a table keyed on content would grow with every distinct shape an animation produces,
   * which is the unbounded-cache problem the image route solved with a page budget. That argument is about a *table of
   * shapes*, and 4.1.43 measured that the shape worth having is not that:
   *
   *   * an animated frame of the demo is **99.98%** content-identical fields to the frame before it (`--animate`, one
   *     panel moving) and **99.57%** in the worst case measured (`--jitter`), while this cache serves **0%**;
   *   * a window of **one** frame captures all of it -- windows of 1, 4, 16 and 64 frames give identical numbers, so
   *     nothing needs to be remembered longer than the previous frame;
   *   * and the reused bytes are the **atlas that is already retained** here, so the only new memory is one key per
   *     field (41084 x 8 bytes, about 328 KB a frame).
   *
   * So the thing to build is not a cache of distinct shapes but a **per-field partial refill** of the atlas this note
   * belongs to: compare each field's own key, refill only the ones that changed (8 of 41084 on the demo's animation,
   * 175 under `--jitter`), and place those again. The cost it removes is the whole field stage -- 16.1 ms of a 32.7 ms
   * animated frame. The remaining design problem is placement, not the key: a field whose lattice changed may not fit
   * the slot it had.
   */
  /**
   * Rasterised coverage bytes, keyed by the field's content key.
   *
   * The fill stage is 15-16 ms on a text-heavy list, and it runs again on every frame that does not match
   * the remembered one -- which, in a frame assembled from several lists, is every list. The bytes are a
   * pure function of the shape, the fill rule and the field's size, so they are cached and copied into
   * whichever atlas the frame places the field in. Reads happen during the fill (from a snapshot taken
   * before it) and writes after, so the job pool never touches the map.
   */
  struct CachedFieldBytes {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> bytes;
  };
  std::unordered_map<uint64_t, CachedFieldBytes> mFieldByteCache;
  /** Fields whose bytes came out of the cache instead of the rasteriser, cumulative. */
  uint64_t mFieldByteCacheHits = 0;
  std::size_t mFieldByteCacheBytes = 0;
  static constexpr std::size_t kFieldByteCacheLimit = 32u * 1024u * 1024u;
  FieldFrameCache mFieldCache;

  /**
   * This frame's per-field identities, and the fields that have to be filled.
   *
   * Members rather than locals for the reason the two tables above them are: they are reused every frame, and a vector
   * allocated and freed 41084 entries at a time is work the stage would otherwise pay for twice. The refill set is the
   * whole frame on a full path and the handful of changed fields on a partial one -- see 4.1.45.
   */
  std::vector<FieldLattice> mFieldIdentities;
  std::vector<uint32_t> mFieldRefill;

  /**
   * The running per-atlas XOR of the fields' hash contributions, and each field's own contribution.
   *
   * What `hashFieldAtlases` used to recompute for every field every frame -- 0.35-0.51 ms of a 1.5 ms field stage -- is a
   * number that only changes when a field's content or placement does, so it is kept. A partial refill XORs the old value out
   * and the new one in for the fields it wrote; a rebuild rebuilds both arrays, because it re-places everything (4.1.49).
   */
  std::vector<uint64_t> mFieldAtlasSums;
  std::vector<uint64_t> mFieldContributions;

  /**
   * The animation-reuse probe: the per-field content keys of the last `depth` frames, and this frame's hit count.
   *
   * ## Why a probe rather than a cache, and what it is deciding
   *
   * `mFieldCache` is one entry over the *whole* frame, and the frame key folds every point in the list -- so a frame in
   * which **anything** moved cannot hit, including the fields of everything that did not move. The case that would pay
   * for a bounded table keyed on each field's own content is therefore "an animation of one thing inside a mostly static
   * UI", and whether it pays is a ratio nobody had measured (`路径边缘抗锯齿设计与现状.md` 4.1.35, 4.1.37, 4.1.38 and
   * 4.1.42 each deferred it). This measures the ratio and nothing else: it fills no atlas, caches no texels, and is off
   * unless `CORAL_TRACE_FIELD_REUSE=<depth>` asks for it, because hashing every field's geometry every frame is work a
   * cache would not do.
   *
   * A hit means "a field with byte-identical rasterisation input appeared in one of the last `depth` frames", which is
   * exactly what a table keyed on content could have served. It is an **upper bound** on what such a table would save,
   * because a table would also have to hold the texels of every field it kept, and the atlas places fields in a packed
   * layout that a partially-reused frame would have to rebuild around the ones it kept.
   */
  struct FieldReuseProbe {
    /** The window, in frames: `frames.size()` is the depth, and 0 means the probe is off. */
    std::vector<std::unordered_set<uint64_t>> frames;
    /** Where the next frame's keys go, so the window is a ring rather than a growing history. */
    size_t next = 0;
    /** The environment is read once, on the first frame that asks, so the flag cannot change mid-run. */
    bool initialised = false;
    /** This frame's measurements, copied into `FrameStats` by the resolve stage. */
    uint32_t hits = 0;
    uint32_t requests = 0;

    /**
     * ## The second question, which decides whether the reuse is reachable at all (4.1.45)
     *
     * A field's bytes can only be kept if its **slot** still holds them, and `placeCoverageFields` repacks the whole
     * atlas from scratch, so any field whose lattice moved re-places every field after it in shelf order. The probe's
     * per-field keys say the *content* is reusable; these counters say whether the *placement* would let it be reused:
     *
     *   * how many fields' lattices changed (that is the repack trigger),
     *   * how much they changed by, and
     *   * how many of them would still fit the padded box they had, for a slot allocator that rounds each box up to a
     *     granularity -- `mFieldPlaced` still holds the previous frame's boxes when the probe runs, because the probe runs
     *     before `placeCoverageFields`.
     *
     * A field that fits its old box needs no new placement and no repack: its slot keeps its position and only its own
     * rectangle is refilled. A field that does not fit needs somewhere to go, which is what a bump allocator over the
     * atlas's unused space (or a repack) would provide.
     */
    std::vector<FieldLattice> lattices;
    uint32_t latticeChanged = 0;
    uint32_t contentChanged = 0;
    int32_t maxWidthDelta = 0;
    int32_t maxHeightDelta = 0;
    int32_t maxOriginDelta = 0;
    /** How many of the changed fields still fit the box they had, per slot granularity. */
    uint32_t fitsGranularity[6] = { 0, 0, 0, 0, 0, 0 };
  };

  FieldReuseProbe mFieldReuse;

  /**
   * Fills @c mFieldReuse for @p list: hashes each field's own rasterisation input, counts the ones the window already
   * holds, and pushes this frame's set in. A no-op when the probe is off.
   */
  void probeFieldReuse(const DrawList& list);

  /**
   * The image atlas pages, in the order they were created -- see `ImageAtlasPage`.
   *
   * A vector rather than a map keyed on an extent, unlike `mFieldAtlases`: the field atlas's key is its extent because a
   * frame picks one, while a page is filled over a session and its "key" is only its identity. Appended to and never
   * removed, so an index is stable for the life of the renderer -- which is what lets a placement name its page by
   * index without the reference-invalidation hazard the layer pool's note describes.
   */
  std::vector<ImageAtlasPage> mImagePages;

  /**
   * The next texture identity, handed to pages and to standalone textures from one counter.
   *
   * One counter rather than two, because `canShareDraw` compares these numbers across both kinds and a page "0" must not
   * equal a standalone texture "0".
   */
  uint32_t mNextImageTextureId = 0;
  /**
   * Descriptor sets for external textures, keyed by the view's handle value.
   *
   * Distinct from `mImagePlacements`, which is keyed on pixels: an external texture has no pixels on
   * this side, and two frames that name the same view must find the same set.
   */
  std::unordered_map<uint64_t, backend::DescriptorSetHandle> mExternalImageSets;

  /**
   * Field atlases, pooled by extent -- see `FieldAtlas` for why they are pooled, why the key is a size, and why nothing
   * is ever destroyed. The key is the width in the high half and the height in the low half.
   */
  std::unordered_map<uint64_t, FieldAtlas> mFieldAtlases;

  /** Honestly-counted omissions for the last frame, reported by `diagnostics`. */
  struct Unsupported {
    uint32_t gradients = 0;
    uint32_t images = 0;
    uint32_t clipMasks = 0;
    uint32_t advancedBlends = 0;
    uint32_t pathsWithoutStencil = 0;
    /**
     * Coverage fields the resolve stage was asked for and could not generate.
     *
     * Counted rather than drawn as an empty field: an empty field is a transparent texture, so the shape would simply
     * be missing -- a picture that looks deliberate. In practice this is an allocation failure for a field of up to
     * 258x258 texels, which is the one thing that can stop `makeCoverageField` producing coverage.
     */
    uint32_t coverageFields = 0;
  };

  Unsupported mUnsupported;
  /** The omissions already reported, so that a steady frame does not repeat the same message. */
  Unsupported mReported;

  /** Diagnostics from the last few operations: device creation, and the omissions above. */
  std::vector<std::string> mMessages;
  /** The device factory's own explanation when it could not bring a device up. */
  std::string mDeviceCreationLog;
};

} // namespace gpu
} // namespace graphics

#endif // CORAL_GPU_GPU_RENDERER_H


