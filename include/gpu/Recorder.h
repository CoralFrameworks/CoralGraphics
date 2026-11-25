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

#ifndef CORAL_GPU_RECORDER_H
#define CORAL_GPU_RECORDER_H

#include <gpu/DrawList.h>

#include <graphics/Bitmap.h>
#include <graphics/Canvas.h>

#include <cstdint>
#include <memory>
#include <string>

namespace graphics::gpu {

class ContextImpl;
struct RenderTarget;

/**
 * Fills a `DrawList` from one thread, through a `Canvas`.
 *
 * ## The shape of a frame
 *
 * ```
 *   gpu::Recorder recorder(width, height);
 *   graphics::Canvas& canvas = recorder.canvas();     // the same API the rasterizer implements
 *   svg.render(canvas);                               // or lottie.draw(canvas), or hand-written calls
 *   context.submit(recorder.finish(), target);        // hand the list to the one thread that renders
 * ```
 *
 * The recorder is what makes a GPU canvas *recordable* rather than immediate: every drawing call is
 * turned into device-space geometry in a list and nothing touches the device. That is the same
 * separation the RHI's command buffer documents, one level up, and it is the reason N threads can each
 * record their own list without a lock anywhere on the path.
 *
 * ## Why the canvas is the recording surface
 *
 * The alternative -- a `Recorder` with its own `fillRect`/`drawPath`/`drawImage` methods mirroring
 * `Canvas` -- would be a second drawing vocabulary to keep in step with the first, and every existing
 * caller of the drawing API (SVG, Lottie, text, the rasterizer's own tests) would need a second code
 * path. Reusing `Canvas` means an SVG document renders through the GPU backend with no change at all,
 * which is the property worth having.
 *
 * ## Threading
 *
 * One recorder per thread, and every method here is called from that thread only. `finish()` *moves* the
 * list out, so after it returns the recorder is empty and may be reused for the next frame -- and the
 * list it returned may be handed to any thread, because ownership is what carries the data, not a lock.
 * A recorder is not copyable and not movable: it holds a canvas, and a canvas's `save`/`restore`
 * balance is a property of the object, not of a value.
 *
 * ## Lifetime
 *
 * RAII: the canvas and the list are members, so a recorder on the stack releases both when it goes out
 * of scope. The context is *borrowed* -- it outlives the recorder by construction, because a recorder
 * that owned the device would start a second one per thread.
 */
class Recorder {
public:
  /**
   * @param width  the drawing surface's width in device pixels.
   * @param height the drawing surface's height in device pixels.
   * @param context the context this recording will eventually be submitted to. Borrowed, and may be null: the recording
   *        half never dereferences it, so a null one is right both for a list that is only inspected (a test checking
   *        the commands a canvas produced) and for one that is submitted *through a `Surface`*, which is the only thing
   *        that ever needs a device. The demo's retained path records its scene with a null context and submits the list
   *        it gets back for every frame.
   * @param label  a name for a capture tool.
   */
  Recorder(int width, int height, ContextImpl* context, const char* label = nullptr);
  ~Recorder();

  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;
  Recorder(Recorder&&) = delete;
  Recorder& operator=(Recorder&&) = delete;

  /** @return the canvas to draw through. Its transform stack starts identity and unclipped. */
  [[nodiscard]] graphics::Canvas& canvas() noexcept { return *mCanvas; }

  /** @return the canvas, for a caller holding a const recorder. */
  [[nodiscard]] const graphics::Canvas& canvas() const noexcept { return *mCanvas; }

  /**
   * Ends the recording and hands over the list.
   *
   * The recorder is left empty and ready for the next frame, so a caller that records once per frame
   * allocates one recorder for the life of its surface. The returned list is moved out, so it may cross
   * a thread boundary -- which is the whole hand-off this design is built around.
   *
   * A list that is **kept** rather than handed on once is the retained path: `Surface::submitRecording` lends it to the
   * surface on every frame, and `Surface::addDrawList` consumes it for one. Those are the two ways on, and the reason
   * `finish()` moves rather than copies is that the caller is the owner from here.
   *
   * A canvas with an unbalanced `save` is *not* an error here: the list records what was asked for, and
   * a pass that rendered it would simply find the extra state entries unused. The recording is
   * discarded rather than repaired, because repairing it would need a guess about what the caller meant.
   */
  [[nodiscard]] DrawList finish();

  /**
   * Draws @p image into this recording, placed by @p matrix and composited with @p paint.
   *
   * ## What it is for, which its earlier doc got wrong
   *
   * This is the *placing* half of compositing a cached picture: the caller has a bitmap and this puts it into the
   * recording under a transform, with a blend mode and an alpha. It does **not** render this recorder's own work into
   * @p image, and no draw list is involved -- the bitmap comes from the caller, which is what makes the resolution, the
   * format and the lifetime the caller's decisions.
   *
   * The route it belongs to is: record a document once, render that recording into a bitmap by whatever means the caller
   * has, and place the bitmap on the frames that follow. It is cheaper per frame than re-rendering the geometry, at a
   * fixed resolution.
   *
   * ## The other retained route, which keeps the geometry
   *
   * `Surface::submitRecording` lends the *recording itself* to the surface on every frame, so the device re-renders it at
   * full quality each time (12.3-14.3 ms a frame for the demo scene's whole display list, 4.1.41). Choose this method
   * when the picture is one element *inside* a frame that is being recorded anyway; choose that one when the recording
   * is the frame's content.
   *
   * @param image  the picture to place. Non-empty.
   * @param matrix the device-space transform to place it under.
   * @param paint  blend mode and alpha for the composite.
   */
  void drawImage(const Bitmap& image, const Matrix& matrix, const Paint& paint);

  /** @return true when a canvas has been recorded into and nothing has been taken out. */
  [[nodiscard]] bool hasRecording() const noexcept { return mRecorded; }

  /** @return the list being filled, for a caller that wants to inspect it before finishing. */
  [[nodiscard]] const DrawList& drawList() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> mImpl;

  graphics::Canvas* mCanvas = nullptr;
  bool mRecorded = false;
};

} // namespace graphics::gpu


#endif // CORAL_GPU_RECORDER_H
