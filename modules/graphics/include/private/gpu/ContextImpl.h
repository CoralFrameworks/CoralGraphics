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

#ifndef CORAL_GPU_CONTEXT_IMPL_H
#define CORAL_GPU_CONTEXT_IMPL_H

#include <gpu/Types.h>

#include <backend/Device.h>
#include <backend/Handles.h>
#include <backend/Types.h>
#include <backend/Validation.h>

#include <graphics/Bitmap.h>

#include <cstdint>
#include <memory>
#include <string>

namespace utils {
class JobSystem;
} // namespace utils

namespace graphics::gpu {

struct DrawList;

/**
 * The pieces of a render target a `DrawList` can be rendered into.
 *
 * A layer is one of these too: the same object describes the root target and every offscreen layer, so
 * the pass has one code path for both and a layer that is never folded away costs one entry in a vector
 * rather than a second path.
 */
struct RenderTarget {
  /** The colour image the commands are rendered into. Invalid means "the surface's own target". */
  backend::TextureHandle color = backend::TextureHandle::invalid();
  /** The view the render pass is begun with, which is how the RHI names an attachment. */
  backend::TextureViewHandle colorView = backend::TextureViewHandle::invalid();

  /**
   * The multisampled attachment when MSAA is enabled, and the single-sampled image it resolves into.
   *
   * `color`/`colorView` always name the resolved image, which is what a snapshot, a layer composite and
   * `Surface::target()` expose. `msaaColor`/`msaaColorView` are valid only when `sampleCount > 1`; the
   * render pass attaches the latter and names the former as its resolve target. A layer target is
   * single-sampled because its purpose is to be sampled by the parent pass, so these stay invalid there.
   */
  backend::TextureHandle msaaColor = backend::TextureHandle::invalid();
  backend::TextureViewHandle msaaColorView = backend::TextureViewHandle::invalid();
  uint32_t sampleCount = 1;

  /**
   * Depth/stencil image, used by the stencil-then-cover path.
   *
   * A path fill accumulates winding in the stencil buffer and then covers the result with one shader.
   * That is how a self-intersecting path, a shape with a hole, or a shape that overlaps itself renders
   * with no tessellation at all -- and it is why a target that will receive path fills needs an extra
   * three bytes per pixel that a rectangle-only target does not.
   */
  backend::TextureHandle depthStencil = backend::TextureHandle::invalid();
  backend::TextureViewHandle depthStencilView = backend::TextureViewHandle::invalid();

  uint32_t width = 0;
  uint32_t height = 0;
  backend::PixelFormat format = backend::PixelFormat::Undefined;

  /** The RHI render pass object describing this target's attachment formats. */
  backend::RenderPassHandle pass = backend::RenderPassHandle::invalid();

  /** True when the colour image can be sampled, which a layer must be and a window need not be. */
  bool sampled = false;

  /**
   * True when the target's numbers are PQ-encoded (HDR10) rather than sRGB-encoded.
   *
   * The canvas still draws sRGB-encoded colours; the *output* stage converts them (BT.2408 white,
   * PQ encode) on the way into this target. It lives on the target because it is a property of what is
   * being written, not of any one command.
   */
  bool pqOutput = false;

  [[nodiscard]] bool isValid() const noexcept { return colorView.valid(); }

  friend bool operator==(const RenderTarget& a, const RenderTarget& b) noexcept {
    return a.color == b.color && a.colorView == b.colorView && a.msaaColor == b.msaaColor &&
           a.msaaColorView == b.msaaColorView && a.sampleCount == b.sampleCount &&
           a.depthStencil == b.depthStencil && a.width == b.width && a.height == b.height &&
           a.format == b.format && a.pqOutput == b.pqOutput;
  }
  friend bool operator!=(const RenderTarget& a, const RenderTarget& b) noexcept { return !(a == b); }
};

/**
 * What a caller must say to get a render target.
 *
 * A struct rather than four positional arguments because the fields answer genuinely different
 * questions -- how big, in what format, with which attachments, for what purpose -- and a call with two
 * adjacent `uint32_t`s is one where a swapped width and height compiles.
 */
struct RenderTargetDescription {
  uint32_t width = 1;
  uint32_t height = 1;
  backend::PixelFormat format = backend::PixelFormat::R8G8B8A8_UNORM;
  /** Ask for a depth/stencil attachment, which the stencil-then-cover path requires. */
  bool needsStencil = true;
  /** Ask for a multisampled attachment, resolved into the colour image at the end of the pass. */
  uint32_t sampleCount = 1;
  /** The colour image is sampled by later draws: a layer, or a surface that doubles as a texture. */
  bool sampled = false;
  /** The target will be read back to the host: a snapshot, or an export. */
  bool readable = false;
  const char* label = nullptr;
};

/**
 * The GPU-side state one surface records into and renders with.
 *
 * ## What this owns, and what it deliberately does not
 *
 * It owns the device, the pool the precompile stage runs on, the pipeline and descriptor caches, the
 * buffers the batches are uploaded into, and the render targets behind the layers. It deliberately does
 * **not** own a `DrawList`: a draw list belongs to the `Recorder` that fills it, and a recorder belongs
 * to a thread. A context that owned the list would make two threads recording the same surface fight
 * over it, and the point of the split is that they do not have to.
 *
 * ## The frame
 *
 * ```
 *   record (N threads)    each thread fills its own DrawList through its own Recorder
 *        |
 *        v
 *   precompile            on the job system: split a list into batches, resolve pipelines, build vertices
 *        |
 *        v
 *   upload and render     on one thread: write the buffers, record the batches, end the pass
 *        |
 *        v
 *   present
 * ```
 *
 * `submit` is the only method that may touch the device, and it is documented as single-threaded for
 * exactly that reason: the RHI serialises resource creation on the device, and a context that let two
 * threads submit at once would race in the one place the RHI says not to. The parallelism this class
 * provides is in the precompile stage, which is where all the CPU work is.
 *
 * ## Lifetime
 *
 * RAII throughout. The destructor waits for the device to go idle and then releases every cached
 * resource, so a caller that drops a context while work is in flight gets a correct teardown rather than
 * a driver object freed under a live command buffer -- the failure the RHI's own documentation warns
 * about.
 */
class ContextImpl {
public:
  ContextImpl();
  ~ContextImpl();

  ContextImpl(const ContextImpl&) = delete;
  ContextImpl& operator=(const ContextImpl&) = delete;
  ContextImpl(ContextImpl&&) = delete;
  ContextImpl& operator=(ContextImpl&&) = delete;

  /**
   * Brings up the device and builds the caches.
   *
   * @param device    an existing RHI device to *borrow*, or null to create one. A caller that already
   *                  has a device -- an application sharing one GL context between its UI and its 3D
   *                  scene -- passes it and keeps ownership; the context then does not destroy it.
   * @param preferred the backend to ask for when creating one. Ignored when @p device is non-null.
   * @param jobs      the pool the precompile stage runs on, or null to run it on the calling thread.
   *                  Borrowed, never owned: a renderer that already has a pool wants its frames to
   *                  share it, and a context that owned one would start a second pool per surface.
   */
  backend::Result initialize(backend::Device* device, backend::Backend preferred, utils::JobSystem* jobs,
    backend::DebugMessageCallback diagnostics = nullptr, void* diagnosticsUserData = nullptr,
    uint32_t maxFramesInFlight = 1);

  /** Number of isolated renderer/target slots in this context. */
  [[nodiscard]] uint32_t frameSlotCount() const noexcept;

  /** Selects which slot subsequent renderer calls operate on. */
  void selectFrameSlot(uint32_t slot) noexcept;

  /** Waits for the selected slot's most recent asynchronous frame, if any. */
  backend::Result waitFrameSlot(uint32_t slot);

  /** @return the RHI device, or null when the context failed to come up. */
  [[nodiscard]] backend::Device* device() const noexcept { return mDevice; }

  /** Transfers a created device to the caller; the context keeps using it but no longer deletes it. */
  [[nodiscard]] backend::Device* releaseDevice();

  /** @return true when a device is live and the caches are built. */
  [[nodiscard]] bool isValid() const noexcept { return mDevice != nullptr; }

  /** @return one line naming the device and its backend, for a log. */
  [[nodiscard]] std::string describeDevice() const;

  // ------------------------------------------------------------------------------------------------
  // Targets
  // ------------------------------------------------------------------------------------------------

  /** Creates a render target. @p outTarget receives an empty target on failure. */
  [[nodiscard]] backend::Result createRenderTarget(const RenderTargetDescription& description,
    RenderTarget* outTarget);

  /**
   * Creates an offscreen target that can be sampled as a texture, which a layer must be.
   *
   * Separate from `createRenderTarget` because a layer is composited onto its parent and a surface's own
   * target need not be: asking for one property the other does not need is how a target ends up
   * allocated with a texture flag it never uses, or missing one it does.
   */
  [[nodiscard]] backend::Result createLayerTarget(uint32_t width, uint32_t height, backend::PixelFormat format,
    bool needsStencil, RenderTarget* outTarget);

  /** Releases a target. Safe on an already-empty one, and leaves it empty. */
  /** Creates a target that renders into the surface's own images. See `GpuRenderer::createWindowTarget`. */
  [[nodiscard]] backend::Result createWindowTarget(uint32_t width, uint32_t height, backend::PixelFormat format,
    uint32_t sampleCount, RenderTarget* outTarget);

  void destroyRenderTarget(RenderTarget& target);

  /** Sets the pattern texture used by the SDF text program. */
  bool setSdfPattern(const Bitmap& pattern);

  // ------------------------------------------------------------------------------------------------
  // Submission
  // ------------------------------------------------------------------------------------------------

  /**
   * Renders @p list into @p target.
   *
   * **Single-threaded.** Call from one thread, and never while another call is in flight.
   *
   * The list is **borrowed, not consumed**: the renderer stores a pointer to it for the duration of this call and only
   * reads it, so a caller may keep the recording and submit it again -- which is what `Surface::submitRecording` exists
   * for, and why the parameter is a `const&` rather than the `&&` it used to be. The list must outlive the call; the
   * caller recycling it *after* the call returns is the same as before.
   *
   * @param images the bitmaps a `Kind::Image` command samples, indexed by `CommandState::image`, in the list's own order
   *        (`DrawList::images`). Passed alongside the list rather than referenced from it because a `DrawList` is
   *        deliberately a plain value that names a bitmap by an opaque id -- which is what keeps it free of pixel
   *        ownership, and therefore replayable by a backend, or written to a file, with no image table at all. The
   *        vector is borrowed for the duration of the call and need not outlive it: the pixels are uploaded here.
   *
   *        A list whose commands name an id past the end of @p images has those commands **skipped and
   *        counted** rather than drawing whichever texture happened to be bound last, because a picture built
   *        from the wrong texture is far harder to diagnose than a missing one.
   *
   * @param rootLoad what happens to @p target's existing pixels, defaulting to the renderer's own rule
   *        (clear, unless the recording clears the whole target itself). `Load` draws **onto** what is
   *        already there, which is what an overlay stage of an effect chain needs; see
   *        `GpuRenderer::submit`.
   */
  /** Forwarded from the renderer, for the test that pins the field byte cache. */
  [[nodiscard]] uint64_t fieldByteCacheHits() const noexcept;

  [[nodiscard]] backend::Result submit(const DrawList& list, RenderTarget& target, const std::vector<Bitmap>& images = {},
    backend::LoadOp rootLoad = backend::LoadOp::DontCare,
    backend::SemaphoreHandle waitSemaphore = backend::SemaphoreHandle::invalid(),
    backend::SemaphoreHandle signalSemaphore = backend::SemaphoreHandle::invalid(),
    const Rect& damage = {}, const Point& translation = {}, bool continueFrame = false);

  /**
   * Copies @p target's pixels into @p outBitmap.
   *
   * Waits for the device first, because a read has to see a complete frame rather than a partial one.
   * That wait is why `Surface::snapshot` documents itself as expensive, and why the rasterizer
   * comparison in the tests is the only place that pays it per frame.
   */
  [[nodiscard]] backend::Result readback(RenderTarget& target, Bitmap* outBitmap);

  /** Blocks until every submitted frame has completed. */
  [[nodiscard]] backend::Result waitIdle();

  /** @return the pool the precompile stage runs on, which is null when recording is single-threaded. */
  [[nodiscard]] utils::JobSystem* jobSystem() const noexcept { return mJobs; }

  /** @return how many frames have been submitted, for diagnostics. */
  [[nodiscard]] uint64_t frameCount() const noexcept { return mFrameCount; }

  /** The statistics the last `submit` produced: what a renderer's overlay shows. */
  struct FrameStats {
    uint32_t commandCount = 0;
    uint32_t batchCount = 0;
    uint32_t drawCallCount = 0;
    uint32_t layerCount = 0;
    uint32_t pipelineCount = 0;
    uint64_t vertexCount = 0;
    uint64_t uploadBytes = 0;
    /**
     * The frame's draw calls by route, so that "where do the draws go" has an answer without a profiler.
     *
     * The three routes worth separating are the ones with a *different* cost per command: the coverage field atlas draws
     * one call per **batch** (many commands at once), the glyph and bitmap route draws one per **command**, and the
     * stencil route draws one per contour plus a cover. Everything else -- rectangles, gradients, composites, masks -- is
     * `drawCallCount` minus these three.
     *
     * Measured on the demo scene before the field route batched: 48252 draws, 35395 of them the coverage route's one per
     * path. After it batched: 8099 draws, and these numbers are what say where the remaining ones are.
     */
    uint32_t coverageDrawCalls = 0;
    uint32_t imageDrawCalls = 0;
    uint32_t pathDrawCalls = 0;
    /**
     * The image and glyph route's **batches**, which is what its draws would become if the route could share one texture.
     *
     * An image batch draws once per command today, so this is smaller than @c imageDrawCalls by exactly the amount an
     * image atlas would save: 559 draws against 500-odd batches on the demo scene (4.1.26).
     */
    uint32_t imageBatchCount = 0;
    /**
     * Milliseconds the coverage-field resolve stage took, **as wall clock** rather than summed over threads, split into
     * the two halves that scale differently: rasterising the fields (`coverageFieldMilliseconds`) and packing them into
     * the frame's atlas (`coveragePackMilliseconds`, which is `packCoverageFields`).
     *
     * The split exists because the two have opposite bottlenecks and the sum hides that: rasterising is per *texel* of
     * shape and parallel, while packing is per *atlas* texel and serial -- and the atlas is a power-of-two square, so its
     * area can be several times the fields' own. Measured on the demo, that ratio is why the stage's wall time stopped
     * following the rasteriser's speed-up.
     */
    double coverageFieldMilliseconds = 0.0;
    double coveragePackMilliseconds = 0.0;
    /**
     * Textures uploaded over the renderer's life, cumulative rather than per frame.
     *
     * Cumulative because that is what makes the image cache's key visible: with an index-keyed cache this grew by every
     * image in every frame, and with a content-keyed one it stops growing once every distinct bitmap has been seen. A
     * per-frame count could not show that difference.
     */
    uint64_t textureUploadCount = 0;
    /**
     * How many of this frame's coverage fields were **reused** from the previous frame rather than rasterised.
     *
     * Zero whenever anything about the frame's fields changed, so a non-zero value is the statement "this frame asked
     * for exactly the fields the last one did, and the atlas that already held them was still valid" -- which is the
     * static-UI case, and the one that pays for the cache: measured on the demo, the field stage is 14 ms of a 28 ms
     * frame with the pool and 50 ms of 64 ms without it, and a reused frame spends none of it.
     *
     * Counted per *field* rather than as a flag because the number has to be read against `coverageFieldCount`: a frame
     * that asked for 41084 fields and reused 0 is a frame the cache does not describe, and one that reused 41084 is a
     * frame it described completely. A flag could not tell a full hit from a partial one, and a partial hit cannot
     * happen -- the key is over the whole frame -- so the two numbers together are the honest pair.
     */
    uint32_t coverageFieldsReused = 0;
    /** Milliseconds the precompile stage took, summed over the threads that ran it. */
    double precompileMilliseconds = 0.0;
    /** How many coverage fields the resolve stage generated this frame. */
    uint32_t coverageFieldCount = 0;
    /**
     * The texels those fields cover, summed: `width * height` over every field generated this frame.
     *
     * The field route's bandwidth, stated as the thing that decides it rather than as a byte count derived elsewhere: a
     * field is uploaded at `coverageFieldArea` bytes per frame now that the texture is `R8_UNORM`, and it would have been
     * four times that as premultiplied RGBA. It is also what says whether a frame is bandwidth bound at all -- measured
     * on the demo scene, dropping three quarters of the payload moved the frame's time by nothing, because the frame's
     * time goes on 48k draw calls and 35k per-upload driver calls rather than on the bytes.
     */
    uint64_t coverageFieldArea = 0;
    /**
     * Where the last frame's `submit` spent its wall clock -- see `gpu::FrameStages` for what each stage is and why it
     * is a struct rather than one accessor per number.
     *
     * Filled by the renderer as the stages run, so the numbers describe the frame that just finished. `stages.fields` is
     * the whole coverage-field stage and the pair below splits that same stage into its two halves -- rasterising, which
     * is per texel and parallel, and packing, which is per atlas texel and serial. One measurement, two views: the split
     * is what says which half a change moved.
     */
    FrameStages stages;

    /**
     * What the animation-reuse probe measured: how many of this frame's fields are content-identical to a field of one
     * of the last `fieldReuseWindowDepth` frames.
     *
     * Zero unless `CORAL_TRACE_FIELD_REUSE=<depth>` was set, because the probe costs a hash of every field's geometry
     * plus a window of hash sets -- which is why it is a measurement and not a cache. It exists to answer the one
     * question the frame-level cache cannot (`coverageFieldsReused` is all-or-nothing over the *whole* frame): in an
     * animation, where one thing moves and the rest of the UI does not, how much of the field work would a table keyed on
     * each field's own content have saved? See 4.1.43, which is the round that asked it and the answer it got.
     */
    uint32_t fieldReuseWindowHits = 0;
    /** How many fields that frame asked for, so the hits can be read as a fraction. */
    uint32_t fieldReuseWindowRequests = 0;
    /** How many frames the window covered; 0 when the probe was off. */
    uint32_t fieldReuseWindowDepth = 0;

    /** Milliseconds the upload-and-record stage took. */
    double renderMilliseconds = 0.0;
  };

  [[nodiscard]] const FrameStats& lastFrameStats() const noexcept { return mStats; }

  /** @return where the last frame's `submit` spent its wall clock; see `gpu::FrameStages`. */
  [[nodiscard]] const FrameStages& lastFrameStages() const noexcept { return mStats.stages; }

  /**
   * @return GPU time of the last completed frame, in milliseconds; 0 when the device has no timestamp
   *         queries. One frame behind by design: the result is read after that frame's fence signals.
   */
  [[nodiscard]] double lastFrameGpuMilliseconds() const noexcept;

  /** @return bytes of texture and buffer memory the device currently holds live (VRAM residency). */
  [[nodiscard]] uint64_t residentBytes() const noexcept;

  /** @return textures uploaded over this context's whole life, from the renderer rather than from one frame. */
  [[nodiscard]] uint64_t textureUploadCount() const noexcept;

  /**
   * @return how many coverage-field textures the renderer's pool holds, which is the bound a frame's field uploads are
   *         read against.
   *
   * A pool smaller than the frame's field count is a pool that re-uploaded -- the price of the bound, not a defect; see
   * `GpuRenderer::CoverageFieldTexture`.
   */
  [[nodiscard]] uint32_t pooledFieldAtlasCount() const noexcept;

  /**
   * @return how many image atlas pages the renderer holds, which is what a session's bitmaps are actually stored in.
   *
   * The number an image route's batching is read against: many glyphs share one page, so a scene of text should cost one
   * or two pages rather than a texture per glyph -- and a count that grows every frame would say that bitmaps are being
   * placed as if they had never been seen. See `GpuRenderer::ImageAtlasPage`.
   */
  [[nodiscard]] uint32_t imageAtlasPageCount() const noexcept;

  /** A multi-line report of the device, the caches and the last frame's statistics. */
  [[nodiscard]] std::string diagnostics() const;

private:
  class Renderer;

  std::vector<std::unique_ptr<Renderer>> mRenderers;
  Renderer* mRenderer = nullptr;
  uint32_t mActiveFrameSlot = 0;

  backend::Device* mDevice = nullptr;
  /** True when this context created the device and must therefore destroy it. */
  bool mOwnsDevice = false;
  utils::JobSystem* mJobs = nullptr;
  uint64_t mFrameCount = 0;
  FrameStats mStats;
};

} // namespace graphics::gpu

#endif // CORAL_GPU_CONTEXT_IMPL_H

