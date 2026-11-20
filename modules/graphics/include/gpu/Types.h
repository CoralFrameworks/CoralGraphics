/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and limitations under
 * the License.
 */

#ifndef CORAL_GPU_TYPES_H
#define CORAL_GPU_TYPES_H

#include <backend/Types.h>

#include <cstdint>

namespace graphics::gpu {

/**
 * Result of an operation that can fail.
 *
 * This is the backend's own enum rather than a parallel one, on purpose: a facade that renumbered its
 * error codes would force every caller to translate between two vocabularies, and the translation is
 * exactly the kind of place a real error gets folded into "something went wrong".
 *
 * `isSuccess` and `toString` are deliberately *not* redeclared here. An alias shares its argument's
 * namespace, so the backend's own helpers already accept a `gpu::Result` - and adding a second
 * overload in `gpu` would make every unqualified call ambiguous between two functions that do the
 * same thing.
 */
using Result = backend::Result;

/** Human readable name of @p result, owned by the facade so it needs no backend call. */
[[nodiscard]] const char* toString(Result result) noexcept;

/** How buffer indices are encoded. 16-bit indices halve the bandwidth of a large mesh. */
using IndexFormat = backend::IndexFormat;

/** Which colour channels a texture format carries; the facade uses the backend's pixel vocabulary. */
using PixelFormat = backend::PixelFormat;

/** A two-dimensional extent in pixels. */
using Extent2D = backend::Extent2D;

/** A colour value. */
using Color = backend::Color;

/** Whether a numeric value is a valid, non-null handle. Never a raw driver pointer. */
template<typename Handle>
[[nodiscard]] constexpr bool isValid(const Handle handle) noexcept {
  return handle.valid();
}

/**
 * Where the last frame's `submit` spent its wall clock, in the order the stages run.
 *
 * ## Why one struct rather than nine accessors
 *
 * The stages of a frame only mean anything *against each other*: "the field stage is 40 ms" says nothing until the
 * frame's total is beside it, and nine numbers read through nine calls are nine chances to read them across a frame
 * boundary and report a breakdown that never happened. So they are one value, taken at one moment, from the frame that
 * just ran.
 *
 * ## Why these stages and not others
 *
 * Each one is a place where a different lever applies, which is the test for whether a number is worth carrying:
 *
 *   * **fields** is `resolveCoverageFields` -- CPU rasterising, parallel, per *shape*;
 *   * **placeImages** is where each bitmap is sampled from -- pure bookkeeping once a session has seen the bitmaps, and
 *     the stage that says whether an image atlas is still paying for itself;
 *   * **precompile** is path tessellation, which the job pool can run;
 *   * **targets** is the layer pool, which is where an offscreen frame spends memory rather than time;
 *   * **arena** is writing the frame's vertex bytes on the CPU and **vertexUpload** is copying them into the buffer --
 *     the two halves of the arena, which are the frame's largest per-frame payload (measured at 13.7 MB on the demo);
 *   * **record** is everything else the command buffer records (uniforms, bindings, draws, texture writes), which is a
 *     function of how many *commands* the frame has;
 *   * **execute** is the driver's own work, which is a function of how much state the recorded frame changes.
 *
 * `total` is the whole of `submit`: the sum of the stages plus the bookkeeping between them (batch building, layer
 * parent resolution, opening and closing the command buffer). It is reported rather than derived so that the arithmetic
 * can be checked against reality -- and a stage that grows while `total` does not is a stage that moved work rather than
 * removed it, which is the distinction the field route's own history keeps running into.
 */
struct FrameStages {
  /** `resolveCoverageFields`: generating this frame's coverage fields (rasterising plus packing). */
  double fields = 0.0;
  /** `placeImages`: deciding where each of the frame's bitmaps is sampled from. */
  double placeImages = 0.0;
  /** `precompile`: tessellating the frame's paths into the fans the stencil pass draws. */
  double precompile = 0.0;
  /** Acquiring this frame's layer targets from the pool. */
  double targets = 0.0;
  /** `assembleArena`: writing the frame's vertex bytes on the CPU. */
  double arena = 0.0;
  /** Copying those bytes into the vertex buffer, which is inside `recordFrame`. */
  double vertexUpload = 0.0;
  /** The rest of `recordFrame`: uniforms, bindings, texture writes and the draw calls themselves. */
  double record = 0.0;
  /**
   * Where `record` went, by draw source.
   *
   * A frame's recording is a walk over batches and composites, and the CPU cost per draw differs by
   * route: a quad batch is one instanced call, a path is a stencil pass plus a cover, an image batch is
   * one call per command, and a composite is a copy plus a draw. Splitting `record` this way says
   * whether the next step is fewer draw calls or a cheaper one, which the total cannot. Each number is
   * a part of `record`, not a stage of its own.
   */
  double rect = 0.0;
  double gradient = 0.0;
  double image = 0.0;
  double path = 0.0;
  double field = 0.0;
  double composite = 0.0;
  /** `Device::executeCommandBuffer`: the driver's own work for the recorded frame. */
  double execute = 0.0;
  /** The whole of `submit`, wall clock. */
  double total = 0.0;
};

} // namespace graphics::gpu

#endif // CORAL_GPU_TYPES_H
