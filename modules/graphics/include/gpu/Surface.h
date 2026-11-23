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

#ifndef CORAL_GPU_SURFACE_H
#define CORAL_GPU_SURFACE_H

#include <gpu/DrawList.h>

#include <graphics/Bitmap.h>
#include <gpu/Types.h>

#include <backend/Device.h>
#include <backend/Handles.h>
#include <backend/Types.h>

#include <graphics/Canvas.h>

#include <cstdint>
#include <memory>
#include <vector>
#include <string>

namespace backend {
class Device;
} // namespace backend

namespace utils {
class JobSystem;
} // namespace utils

namespace graphics::gpu {

class ContextImpl;
class Recorder;
struct RenderTarget;
struct RenderTargetDescription;

/**
 * What a caller must say to get a GPU surface.
 *
 * The defaults are the ones an application wants: the platform's own backend, a size it has to give, and a
 * stencil buffer because path fills need one. Everything a caller would otherwise have to know about device
 * selection is a field here rather than a second factory, so `gpu::Surface::make(configuration)` is the whole
 * of the API.
 */
struct SurfaceConfiguration {
  /** Drawing surface size in device pixels. Required: a GPU surface has no window to ask. */
  uint32_t width = 0;
  uint32_t height = 0;

  /**
   * The backend to ask for, or `Undefined` to let the platform decide.
   *
   * This is the field that makes the same demo runnable against more than one GPU backend -- the point of
   * naming a backend rather than sniffing one, and the reason a test can compare two backends' output.
   */
  backend::Backend preferred = backend::Backend::Null;

  /**
   * Ask for a depth/stencil attachment.
   *
   * On by default because the stencil buffer is how a path fill is drawn: winding is accumulated in the
   * stencil and the result is covered with one shader, which is what lets a self-intersecting path or a shape
   * with a hole render with no tessellation at all. Turning it off saves three bytes per pixel and costs the
   * ability to fill a path -- a trade worth stating rather than deciding.
   */
  bool stencil = true;

  /**
   * MSAA sample count for this surface's colour attachment; 1 disables it.
   *
   * A value above 1 asks the backend for a multisampled colour attachment plus a single-sampled
   * resolve target. The snapshot and any layer composite read the resolved target, so the option is
   * transparent to the `Canvas` API. Only offscreen `gpu::Surface` targets use this; a `WindowSurface`
   * presents directly and leaves the swapchain single-sampled.
   *
   * The request is exact rather than best-effort: `make` fails with a diagnostic when the backend or
   * format does not support the count. Use `backend::Capabilities::supportsSampleCount` to choose from
   * the device's counts, or leave it at 1.
   */
  uint32_t sampleCount = 1;

  /**
   * Write PQ-encoded numbers (HDR10) instead of sRGB-encoded ones.
   *
   * The canvas still records sRGB-encoded colours; this flag makes the output stage apply the BT.2408
   * white point (203 cd/m^2) and the PQ curve on the way into the target. It is the **offscreen** test
   * path: a window uses `WindowSurface::Configuration::outputColorSpace` and the linear 16F presentation
   * pass instead, because an 8-bit target cannot carry values above diffuse white. Here the encoded byte
   * is the same number a 10-bit swapchain carries, only at a coarser precision.
   */
  bool pqOutput = false;

  /**
   * The pool the precompile stage runs on, or null for single-threaded precompilation.
   *
   * Borrowed, never owned: a renderer that already has a pool wants its frames to share it, and a surface that
   * started its own would quietly multiply the thread count by the number of surfaces. This is also the switch
   * a test flips to prove the parallel and serial paths produce the same pixels.
   */
  utils::JobSystem* jobSystem = nullptr;

  /**
   * Maximum frames the CPU may run ahead of the GPU; 1 keeps the historical synchronous path.
   *
   * Values above 1 are honoured by the Vulkan backend, which owns that many isolated renderer/target
   * slots and submits each frame asynchronously. The OpenGL backend stays at one slot: its queue
   * replays synchronously, so extra slots would only duplicate resources. `snapshot()` waits for the
   * frame it samples; `submit()` itself returns as soon as the frame is submitted.
   */
  uint32_t maxFramesInFlight = 1;

  /** An existing device to borrow instead of creating one. Null creates one, and owns it. */
  backend::Device* device = nullptr;

  /**
   * Receives the backend's own explanation when a surface cannot be created, or null to discard it.
   *
   * ## Why this is a field rather than something read off a failed surface
   *
   * `make` returns `nullptr` on failure, so there is no surface to ask -- and the reason is the most useful thing the
   * call produces. It was being discarded, which left a caller with "a GPU surface could not be brought up" and a
   * numeric result code, and no way to tell a missing driver from a build without embedded shaders from a bad
   * configuration. Getting the second of those diagnosed took reading the renderer's source, which is exactly the work
   * this field exists to remove.
   *
   * The callback is invoked with the same severities the RHI uses, and `userData` is passed through untouched. It is
   * called only on the creation path, on the calling thread, and never again after `make` returns.
   */
  void (*diagnosticsCallback)(backend::MessageSeverity severity, const char* message, void* userData) = nullptr;

  /** Passed to @c diagnosticsCallback untouched. */
  void* diagnosticsUserData = nullptr;

  /** A name for the device and for GPU captures. */
  const char* label = "graphics::gpu::Surface";
};

/**
 * A GPU-backed drawing surface, and the frame it hands out.
 *
 * ## The shape of a frame
 *
 * ```
 *   auto surface = graphics::gpu::Surface::make({.width = 1024, .height = 768});
 *   graphics::Canvas& canvas = surface->beginFrame();      // the same Canvas the rasterizer implements
 *   svg.render(canvas);                                   // or lottie.draw(canvas), or hand-written calls
 *   surface->submit();                                    // one thread renders; every recorder may have recorded
 *   graphics::Image image = surface->snapshot();          // optional: a readback, for a test or an export
 * ```
 *
 * The canvas is the ordinary `graphics::Canvas`, so every caller of the drawing API -- SVG, Lottie, text, the
 * rasterizer's own tests -- reaches the GPU backend with no change at all. That is the property worth having:
 * the difference between the CPU and GPU paths is *where the pixels come from*, not which calls a caller makes.
 *
 * ## The stages, which are Skia Graphite's
 *
 * | stage | thread | what it does |
 * |---|---|---|
 * | record | one per recorder | `beginFrame` and every canvas call append to a `DrawList`; nothing touches a driver |
 * | precompile | the job system | split the list into batches, resolve their pipelines, tessellate the paths |
 * | upload and render | one thread | write the buffers, record the batches, end the pass |
 *
 * The middle stage is the one that parallelises, and it is the reason the first two are separate: a recording
 * thread must never touch the GL context, and the precompile stage has nothing else to do. `submit` is
 * therefore documented as single-threaded, because that is where the context is.
 *
 * ## Offscreen by construction, which is what a transcoder wants
 *
 * There is no window here and no swapchain: the surface renders into a texture it owns and `snapshot` reads it
 * back. That is what makes it usable for video editing and transcoding -- a background renderer wants exactly
 * this and nothing else -- and it is why the surface does not present. A caller that has a window hands the
 * same kind of texture to the platform itself, which is the `NativeTexture` escape hatch the RHI documents.
 *
 * ## Why this is not `graphics::Surface`
 *
 * `graphics::Surface` owns a `Bitmap` and is the software surface; this owns a render target and is the
 * hardware one. They share the *contract* -- a frame, a canvas, a snapshot -- and not the storage, and
 * merging the two would put an RHI handle inside a class whose whole design is that it has none. The GPU
 * surface is the second implementation the software one's documentation already anticipated.
 *
 * ## Lifetime
 *
 * RAII throughout. The context, the recorder and the render target are members; the destructor waits for the
 * device to go idle and releases everything in reverse order. A caller that lets a surface go while work is in
 * flight gets a correct teardown rather than a driver object freed under a live command buffer.
 */
class Surface {
public:
  ~Surface();

  Surface(const Surface&) = delete;
  Surface& operator=(const Surface&) = delete;

  /**
   * Brings up a device and allocates the render target.
   *
   * @return the surface, or null when no backend could be brought up or the target could not be allocated.
   *         The reason is reported through the RHI's own diagnostics, which name the backend and the driver.
   */
  [[nodiscard]] static std::unique_ptr<Surface> make(const SurfaceConfiguration& configuration);

  /**
   * Creates a surface that borrows @p device instead of creating one.
   *
   * Same contract as `WindowSurface::makeOn`: the device must outlive the surface, and the caller is
   * responsible for keeping its context current on this thread.
   */
  [[nodiscard]] static std::unique_ptr<Surface> makeOn(backend::Device* device,
    const SurfaceConfiguration& configuration);

  /** Convenience for the common case: the platform's backend, a stencil buffer, no pool. */
  [[nodiscard]] static std::unique_ptr<Surface> make(uint32_t width, uint32_t height);

  /** @return the size of the surface in device pixels. */
  [[nodiscard]] uint32_t width() const noexcept { return mWidth; }
  [[nodiscard]] uint32_t height() const noexcept { return mHeight; }

  /** The sample count the render target actually uses; 1 when MSAA is off. */
  [[nodiscard]] uint32_t sampleCount() const noexcept;

  /** The rectangle from the origin to the surface's size. */
  [[nodiscard]] Rect bounds() const noexcept;

  /** @return one line naming the device and its backend, for a log or a title bar. */
  [[nodiscard]] std::string describeDevice() const;

  /** @return a multi-line report of the device, the caches and the last frame's statistics. */
  [[nodiscard]] std::string diagnostics() const;

  // ------------------------------------------------------------------------------------------------
  // The frame
  // ------------------------------------------------------------------------------------------------

  /**
   * @return the canvas to record this frame through, or null when a frame is already open.
   *
   * Refused rather than re-returned, for the same reason `graphics::Surface::getCanvas` refuses: asking twice
   * means the caller has lost track of its own frame, and handing back the same canvas would let it record
   * half of one frame and half of the next with nothing to indicate that it had.
   *
   * The canvas stays valid until the next `submit` or `snapshot`. Drawing through it after that records into a
   * list nobody will render, which is a draw that has no effect rather than a corruption.
   */
  [[nodiscard]] graphics::Canvas* beginFrame();

  /**
   * Renders the recorded frame and ends it.
   *
   * **Single-threaded**: call from one thread and never while another call is in flight. This is the only
   * method that touches the device, which is exactly why the *recording* is not restricted this way -- N
   * threads may each hold a recorder and a canvas, and each hand its `DrawList` here in turn.
   *
   * ## What the frame is made of, and in what order it is painted
   *
   * Three sources, painted in this order, each in the order its lists were given:
   *
   *   1. what the canvas from `beginFrame` recorded -- nothing at all if `beginFrame` was never called or drawn
   *      through (the canvas's own list is then empty and contributes nothing);
   *   2. every list handed to `addDrawList`, in call order;
   *   3. every list lent to `submitRecording`, in call order -- so a lent list is painted **over** the other two,
   *      which is what lets a retained display list overdraw a background recorded through the canvas.
   *
   * `beginFrame` is therefore *optional*, and that is the shape the retained path has: a caller with a recording it
   * keeps hands it to `submitRecording` and calls `submit`, with no canvas and no re-recording. All three sources may
   * be combined, and combining them is correct -- but see `submitRecording` for what a combination costs.
   *
   * A frame with nothing recorded is still submitted: it clears the target, which is what a caller that wants
   * a transparent surface expects rather than a stale one.
   *
   * @return false when the device refused the submission. The reason is in `diagnostics`.
   */
  bool submit();

  /**
   * Submits the frame as a partial update: only @p damage is written, the rest of the target is
   * loaded from its previous contents. The recording is not retained, because a list that covers only
   * one damage rectangle is not a complete frame.
   */
  bool submitDamage(const Rect& damage);

  /**
   * One entry of a multi-list frame: a borrowed list plus the state it is composited with.
   *
   * The lists are painted in the order given, after the canvas recording of the frame (if the caller
   * opened one). A default-constructed `clip` means the whole target, and an identity `transform` with
   * `opacity == 1` means the list is drawn directly, with no offscreen composite.
   */
  struct ListItem {
    std::shared_ptr<const DrawList> list;
    /** Device-pixel clip; an empty rect means the whole target. */
    Rect clip{};
    /**
     * Device-pixel translation applied when the list is submitted.
     *
     * The executor carries it in the canvas block instead of rewriting the list's device-space
     * geometry, so a scrolled viewport can lend the same list at a different offset without copying it.
     */
    Point translation{};
    float opacity = 1.0f;
  };

  /**
   * Submits a frame as an ordered list of borrowed lists.
   *
   * The point of this entry point is the frame a retained UI wants to hand over: a root list plus one
   * list per repaint boundary, in paint order, none of them copied. A frame that is exactly one borrowed
   * list takes the existing zero-copy path. Until the renderer records several lists into one command
   * buffer (milestone M1b of `docs/R3R4帧装配与合成层项目.md`), every other frame is merged into one
   * list first, and that merge is the cost this entry point exists to remove.
   *
   * A non-empty `clip` is applied as the submission's scissor and a non-zero `translation` to the
   * canvas block, which is what lets a viewport lend its content without clipping the canvas and
   * without rewriting its geometry.
   *
   * @return false when the device refused the submission, or when an item asks for a composite state
   *         this path cannot honour (a per-list opacity below 1).
   */
  bool submitLists(const std::vector<ListItem>& items);

  /**
   * Re-renders the last submitted canvas recording without recording it again.
   *
   * This is the retained-display-list path for a window whose scene has not changed: the recording and
   * its images are still held by the surface, so a frame costs one renderer submission and nothing on
   * the recording thread. It is true only for a frame that was a single canvas recording -- a frame
   * merged with `addDrawList`/`submitRecording` is not kept, because the caller owns pieces of it.
   *
   * `beginFrame` invalidates the retained recording; `submitRetained` therefore has to be called
   * instead of starting a frame. `hasRetainedRecording` tells a scheduler whether it may try.
   */
  bool submitRetained();

  /** @return true when the last submitted canvas recording can be replayed by `submitRetained`. */
  [[nodiscard]] bool hasRetainedRecording() const noexcept;

  /** Sets the pattern texture sampled by SDF text; an empty bitmap disables the pattern program. */
  /** @return false when the backend has no pattern sampler (OpenGL), in which case SDF text still draws. */
  bool setSdfPattern(const graphics::Bitmap& pattern);

  /**
   * Draws @p field into @p deviceBounds through the coverage-field program, sampling it bilinearly.
   *
   * ## What this is for
   *
   * Antialiased path edges on the GPU. The software rasterizer already produces a coverage field per shape
   * (`raster::makeCoverageField`), and this is the GPU half of that definition: the field is uploaded and the
   * fragment stage samples it, so an edge is feathered the same way on both backends rather than being a step
   * on one and a gradient on the other.
   *
   * **A path no longer needs this call.** `GraphiteCanvasBackend::drawPath` records its own coverage field for a
   * small antialiased fill, so drawing a path through the canvas is the route an application uses. This entry
   * point is for a caller that already *has* a field -- a cached one, a mask built once -- and hands it over to be
   * drawn; the two go through one construction of the command (`gpu::appendCoverageFieldCommand`), which is what
   * keeps a fix from reaching one and not the other.
   *
   * Must be called between `beginFrame` and `submit`, like every other recording call.
   *
   * @param field the coverage to draw: one byte per texel, with texel (0, 0) at (`originX`, `originY`) in
   *              device pixels. Built by `raster::makeCoverageField` from the same geometry the rasterizer
   *              renders, which is what makes the two agree. It must already **carry** its coverage
   *              (`hasCoverage()`): a field with no texels is a *request* the renderer generates from a
   *              recorded path's contours, and this call has no contours to give it -- so a request is
   *              refused rather than drawn as a transparent rectangle. A caller with geometry to rasterise
   *              should draw a path and let `GraphiteCanvasBackend` record the request.
   * @param deviceBounds where the field lands, in device pixels. It must match the field's own extent, because
   *                     the fragment stage converts a position within it into a texel coordinate -- a quad that
   *                     did not match would sample the wrong part of the field rather than stretch it.
   *
   * @return false when the field is empty or no frame is open.
   */
  bool drawCoverageField(const DrawList::CoverageField& field, const Rect& deviceBounds, const Paint& paint);

  /**
   * Draws @p image -- a texture this surface's device did not create -- into @p deviceBounds.
   *
   * The zero-copy half of content embedding: an effect graph, a decoder or another canvas renders into
   * a texture on the same device, and this call makes the page sample it. No pixels are copied and no
   * CPU readback happens, which is the difference between this and handing the surface a `Bitmap`.
   *
   * The view is **borrowed for the frame**: it must stay valid until this surface's `submit` has
   * completed on the GPU. A producer that reuses a ring of frames must not recycle the slot the
   * command names before then, which a `TextureOutputNode` guarantees by keeping its slots in flight.
   *
   * Must be called between `beginFrame` and `submit`, like every other recording call. The command is
   * appended to the frame's recording, so it paints in call order with the canvas's own draws; a caller
   * that wants chrome over the content draws the chrome after this call (or hands it as another list).
   *
   * @return false when no frame is open or @p image is not a usable view.
   */
  bool drawExternalImage(const DrawList::ExternalImage& image, const Rect& deviceBounds, const Paint& paint);

  /** @return the pixels of the last submitted frame, or an empty bitmap when the read failed. */
  [[nodiscard]] Bitmap snapshot();

  /**
   * Hands a list recorded by another `Recorder` to this surface's frame.
   *
   * The parallel-recording path: N threads each own a `Recorder`, and their lists are handed here in paint
   * order before `submit`. The lists are consumed, so a recorder may recycle its storage immediately.
   *
   * **Consuming is the difference from `submitRecording`**, which lends a list and lets the caller keep it: use this
   * one when the recording is finished with (one frame per list, the parallel case above), and that one when the same
   * recording is drawn again on later frames. A caller that has a list it wants to keep can also hand a copy here
   * (`DrawList::clone`), which costs 3.6-4.3 ms on the demo scene against borrowing's nothing (4.1.40/4.1.41).
   */
  bool addDrawList(DrawList&& list);

  /**
   * Renders @p list in this surface's next frame **without taking ownership of it**.
   *
   * The retained path: a caller that keeps a display list -- recorded once, drawn every frame -- lends the same list here
   * on each frame instead of re-recording the scene, and instead of giving the list up. Measured on the demo scene
   * (`路径边缘抗锯齿设计与现状.md` 4.1.40), re-recording costs ~130 ms a frame against `submit`'s ~13 ms, and the copy
   * `addDrawList` would need costs 3.6-4.3 ms -- so lending is the cheapest of the three, and a frame that is one lent
   * list is submitted exactly as it stands, with no copy anywhere (4.1.41).
   *
   * ## Where the list comes from
   *
   * A `Recorder` (whose `finish()` hands over the list -- a **null device** is fine there, and is what the demo uses) or
   * `Canvas::createRecordingBackend` with a list the caller already holds. Either way the list outlives the recorder, and
   * it is the caller's from then on.
   *
   * ## The one requirement, which the type system cannot state
   *
   * @p list must stay alive until the `submit()` that draws it returns. It is stored as a pointer, and the renderer reads
   * it through that pointer for the whole of the submission -- so destroying it, or **recording into it** (a `Recorder`
   * is reusable, and `finish()` moves the list out from under a caller that does this by mistake), before that `submit`
   * returns is the one way to be wrong here. That is the trade for not copying: the caller keeps the ownership and
   * therefore the lifetime.
   *
   * Lending the same list twice before a `submit` draws it twice -- that is the rule `addDrawList` follows too, and not a
   * mistake this can detect for the caller.
   *
   * ## What it costs when the frame is not just this list
   *
   * A frame that is *only* one lent list goes straight to the renderer, with no copy. Anything else -- a canvas recorded
   * through `beginFrame` as well, an `addDrawList` list, or a second lent list -- has to be merged, and a merge renumbers
   * the indices a `DrawList` carries, so each lent list is cloned for it. Mixing is therefore correct but pays a copy,
   * which is stated here rather than discovered from a profile.
   *
   * ## The other retained route, and when it is the better one
   *
   * Lending keeps the *vector* recording, so the device re-renders it at full quality every frame. A caller whose content
   * changes rarely and whose per-frame budget is tighter can instead render it into a bitmap once and place that bitmap
   * with `Recorder::drawImage` -- cheaper per frame, at a fixed resolution. The two are not alternatives to be guessed
   * between: this one is for a display list that *is* the frame's content, and that one for a picture composited into a
   * frame that is re-recorded anyway.
   */
  bool submitRecording(const DrawList& list);

  /**
   * The same, for a caller whose recording is a `shared_ptr` -- and this overload is the one that puts the lifetime
   * requirement **in the type**.
   *
   * `submitRecording(const DrawList&)` stores a pointer, so the caller owes the list's lifetime until `submit` returns. That
   * is a rule the compiler cannot check, and the failure it protects against is the worst kind: not a crash at the misuse,
   * but drawing whatever the freed memory now holds. Taking a `shared_ptr` instead removes the rule: the surface holds a
   * reference for as long as it needs the list, so the caller may drop its own at any point -- including between the call and
   * `submit` -- and the frame is still drawn from the right bytes.
   *
   * Choose by how the list is owned: a caller with one list on the stack or in a member that outlives the frame can pass a
   * reference, and a caller whose display list is shared (or handed around) should hold it in a `shared_ptr` and pass that.
   *
   * @return false when the surface has no device, or when @p list is null -- a frame with a null recording is a mistake
   *         rather than an empty frame, which is what `submit` with nothing recorded already means.
   */
  bool submitRecording(const std::shared_ptr<const DrawList>& list);

  /** The statistics of the last submitted frame: what a renderer's overlay shows. */
  [[nodiscard]] uint32_t lastFrameBatchCount() const noexcept;
  [[nodiscard]] uint32_t lastFrameDrawCallCount() const noexcept;
  /**
   * Command buffers the last `submitLists`/`submit` produced.
   *
   * A plan-built frame lends several lists, and each submission is its own GPU frame; this is the number
   * a caller minimizes. It drops to 1 when every list fits one frame, including lists that carry a
   * whole-pixel translation (they are moved into the frame's space rather than opening a frame each).
   */
  [[nodiscard]] uint32_t lastFrameSubmitCount() const noexcept;
  /**
   * The last frame's draw calls by route: the coverage field atlases (one per batch), the glyph and bitmap route (one per
   * command), and the stencil route (one per contour plus a cover).
   *
   * `lastFrameDrawCallCount()` minus these three is everything else -- rectangles, gradients, composites, masks -- so the
   * four numbers together are a breakdown rather than four separate facts. They exist because "the frame is bound by draw
   * calls" is only actionable once it says *which* draws, which is the measurement behind the field atlas and the
   * batching that followed it (`路径边缘抗锯齿设计与现状.md` 4.1.12 and 4.1.13).
   */
  [[nodiscard]] uint32_t lastFrameCoverageDrawCallCount() const noexcept;
  [[nodiscard]] uint32_t lastFrameImageDrawCallCount() const noexcept;
  [[nodiscard]] uint32_t lastFrameImageBatchCount() const noexcept;
  [[nodiscard]] uint32_t lastFramePathDrawCallCount() const noexcept;
  [[nodiscard]] uint32_t lastFramePipelineCount() const noexcept;
  [[nodiscard]] double lastFramePrecompileMilliseconds() const noexcept;
  [[nodiscard]] double lastFrameRenderMilliseconds() const noexcept;

  /**
   * @return GPU time of the last completed frame, in milliseconds; 0 when the device has no timestamp
   *         queries. The value is one frame behind: it becomes readable once that frame's fence signals.
   */
  [[nodiscard]] double lastFrameGpuMilliseconds() const noexcept;

  /** @return bytes of texture and buffer memory the device holds live (VRAM residency). */
  [[nodiscard]] uint64_t residentBytes() const noexcept;

  /**
   * @return bytes the last frame uploaded to the device: vertex and uniform data, and every texture's payload.
   *
   * The frame's *bandwidth*, which is the number a texture-upload change moves and the frame's time may not: a demo
   * frame of thousands of paths re-uploads its coverage fields, and this says how much that costs before anyone argues
   * about whether it is worth it. `GpuRenderer` counts it as the frame's arena, which is where every payload is copied
   * on its way to the driver.
   */
  [[nodiscard]] uint64_t lastFrameUploadBytes() const noexcept;

  /**
   * @return milliseconds the last frame's **coverage-field generation** took, as wall clock.
   *
   * The first of the two stages the coverage-field route is split into (see `覆盖率` in
   * `路径边缘抗锯齿设计与现状.md`, step S5): a path is recorded as a field *request*, and the coverage itself is
   * rasterised by the renderer -- on the job pool when the surface was configured with one -- before anything is drawn.
   *
   * Wall clock rather than the sum over tasks that `lastFramePrecompileMilliseconds` reports, because the question this
   * answers is whether generating the fields in parallel made the frame shorter; a sum over threads grows when the work
   * is spread out and would answer the opposite question.
   */
  [[nodiscard]] double lastFrameCoverageFieldMilliseconds() const noexcept;
  [[nodiscard]] double lastFrameCoveragePackMilliseconds() const noexcept;

  /** @return how many coverage fields the last frame generated, so the time above has a denominator. */
  [[nodiscard]] uint32_t lastFrameCoverageFieldCount() const noexcept;

  /**
   * @return the texels the last frame's coverage fields covered: `width * height` summed over every field generated.
   *
   * The field route's bandwidth in its own terms. The field texture is `R8_UNORM`, so this *is* the frame's field payload
   * in bytes -- and it would have been four times that as premultiplied RGBA, which is what the first version uploaded.
   */
  [[nodiscard]] uint64_t lastFrameCoverageFieldArea() const noexcept;

  /**
   * @return the number of textures this surface's renderer has uploaded over its whole life, cumulative.
   *
   * Cumulative rather than per frame, because that is what makes the image cache's key observable: with the cache keyed
   * on an image *index* -- which a fresh frame's list renumbers from zero -- this grew by every image and every glyph in
   * every frame, and keyed on the pixels it stops growing once every distinct bitmap has been seen. A per-frame count
   * could not tell "nothing needed uploading" from "the cache is empty".
   */
  [[nodiscard]] uint64_t textureUploadCount() const noexcept;

  /**
   * @return how many coverage-field textures the renderer's pool holds.
   *
   * The bound that a frame's field uploads are read against: fields are pooled by size class
   * (`GpuRenderer::CoverageFieldTexture`), so a pool far smaller than the number of fields a frame drew is a pool that
   * re-uploaded them -- which is the price of the bound rather than a defect, and the number that says whether the bound
   * is costing anything on a real scene.
   */
  [[nodiscard]] uint32_t pooledFieldAtlasCount() const noexcept;

  /**
   * @return where the last frame's `submit` spent its wall clock, stage by stage.
   *
   * The number a frame's *total* has to be read against: `lastFrameRenderMilliseconds` says a frame cost 70 ms and this
   * says which part of it did, which is the difference between a frame that is bound by the rasteriser, by the arena's
   * bytes, or by the driver. See `gpu::FrameStages` for what each stage is, and for why the stages are one value.
   */
  [[nodiscard]] FrameStages lastFrameStages() const noexcept;

  /**
   * @return how many of the last frame's coverage fields were reused from the frame before rather than rasterised.
   *
   * Read against `lastFrameCoverageFieldCount()`: the two are equal when the frame asked for exactly the fields the
   * previous one did -- the static-UI case -- and the reuse count is zero whenever any of the geometry, the lattice or
   * the fill rule changed. A value in between cannot happen, because the decision is taken for the whole frame at once.
   */
  [[nodiscard]] uint32_t lastFrameCoverageFieldsReused() const noexcept;

  /**
   * @return how many coverage fields have been served from the rasterised-byte cache since the surface was created.
   *
   * A field's coverage bytes depend only on its shape, its fill rule and its size, so a frame that asks for a
   * shape an earlier frame -- or an earlier list of the same frame -- already rasterised copies the bytes instead
   * of filling them. This counter is what pins that win: without the cache the fill stage was 16 ms on a
   * text-heavy list, and a test that only compared pictures would not notice it coming back.
   */
  [[nodiscard]] uint64_t coverageFieldByteCacheHits() const noexcept;

  /**
   * The animation-reuse probe's answer for the last frame: how many of its coverage fields are content-identical to a
   * field of one of the last `lastFrameFieldReuseWindowDepth` frames.
   *
   * Zero unless the probe was asked for with `CORAL_TRACE_FIELD_REUSE=<depth>`, because it hashes every field's geometry
   * every frame -- it is a measurement of what a bounded content-keyed field table *could* serve, not such a table. The
   * frame-level cache (`lastFrameCoverageFieldsReused`) is all-or-nothing over the whole frame, so a frame in which one
   * thing moved reuses nothing at all; these two numbers are what say how much of that is unavoidable. Measured on the
   * demo: see 4.1.43, which is the round that asked and the answer it got.
   */
  [[nodiscard]] uint32_t lastFrameFieldReuseWindowHits() const noexcept;
  /** How many fields that frame asked for, so the hits above can be read as a fraction. */
  [[nodiscard]] uint32_t lastFrameFieldReuseWindowRequests() const noexcept;
  /** How many frames the probe's window covered; 0 when it was off. */
  [[nodiscard]] uint32_t lastFrameFieldReuseWindowDepth() const noexcept;

  /**
   * @return how many image atlas pages the renderer holds.
   *
   * The image route's counterpart of the field atlas count, and the number its batching is read against: an image command
   * is one *batch* per run of commands that share a texture, so many glyphs in one page is what turns a draw per glyph
   * into a draw per run -- while a page count that grows every frame would mean bitmaps were being placed as if they had
   * never been seen before. See `GpuRenderer::ImageAtlasPage`.
   */
  [[nodiscard]] uint32_t imageAtlasPageCount() const noexcept;

  /** @return the RHI device, for an operation the facade does not wrap. */
  [[nodiscard]] backend::Device* device() noexcept;

  /**
   * Transfers ownership of a created device to the caller.
   *
   * The surface keeps rendering through it but no longer deletes it. Returns null for a surface
   * created with `makeOn` or after a previous release.
   */
  [[nodiscard]] backend::Device* releaseDevice();


  /**
   * @return the texture this surface renders into, or an invalid handle before the surface exists.
   *
   * ## Why this is part of the interface rather than an internal detail
   *
   * The class comment says a caller that has a window "hands the same kind of texture to the platform itself, which is
   * the `NativeTexture` escape hatch the RHI documents" -- and until this accessor existed there was **no way to name
   * that texture**, so the sentence described a path no caller could take. `device()->getNativeTexture(target())` is
   * that path: it yields the driver object a compositor, a video encoder or another API can share without a copy.
   *
   * `device()` was already public for the same reason, and this is the handle it needs. The pair is what hides the
   * platform detail: a caller writes `getNativeTexture` and one of them is the GL name, the Metal texture or the Vulkan
   * image, decided by the backend and not by the caller.
   *
   * The handle is not a copy and not a new reference: it belongs to the surface's renderer and is released with it, so a
   * caller must not destroy it. A backend that cannot export a resource answers with a null object rather than an error,
   * which is the RHI's documented honesty about interop.
   */
  [[nodiscard]] backend::TextureHandle target() const noexcept;

  /** Blocks until every submitted frame has completed. */
  [[nodiscard]] bool waitIdle();

private:
  Surface();

  /** The GPU state: the context, the recorder, the target and the pending lists. */
  struct Impl;
  std::unique_ptr<Impl> mImpl;

  uint32_t mWidth = 0;
  uint32_t mHeight = 0;
};

} // namespace graphics::gpu


#endif // CORAL_GPU_SURFACE_H

