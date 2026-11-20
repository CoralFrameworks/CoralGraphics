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

#ifndef CORAL_GRAPHICS_GPU_COVERAGE_RECORD_H
#define CORAL_GRAPHICS_GPU_COVERAGE_RECORD_H

#include <gpu/DrawList.h>
#include <graphics/Paint.h>

#include <algorithm>
#include <utility>

/**
 * @file CoverageRecord.h
 *
 * **The one construction of "a command that draws a coverage field".**
 *
 * ## Why this is a function rather than two copies
 *
 * Two callers record a coverage field, and they are recording the *same* command with different provenance:
 *
 *   * `GraphiteCanvasBackend::recordCoverageField`, which rasterises a path into a field and is how an antialiased path
 *     edge reaches the GPU at all;
 *   * `Surface::drawCoverageField`, which draws a field the caller built.
 *
 * Everything that decides what the picture *is* -- that the command is a rectangle carrying `ShaderKind::Coverage`, that
 * it names a field by index, that it is always antialiased, that its quad is the rectangle drawn -- is in the function
 * below. Two copies of that list is how one of them gets a fix and the other does not, and this area has already had
 * exactly that failure once: `Program::CoverageField` was missing from one `switch` over pipeline vertex inputs where
 * every other program sharing `quad.vert` appeared, so it kept a vertex input of *no attributes* and its draw
 * rasterised nothing at all -- silently, on every driver.
 *
 * ## What the caller still owns
 *
 * The clip in force, the clip mask, and the layer. Those are the recorder's canvas state (a path is recorded under the
 * clip and inside the layer that were in force when it was drawn) and the surface's target extent (a direct field draw
 * has no canvas to ask), so they are parameters rather than decisions this function can make for either.
 */

namespace graphics::gpu {

/**
 * Appends @p field and the command that draws it to @p list.
 *
 * @param field the coverage to draw, moved into the list: a recording outlives the call that produced it, so a command
 *              cannot point at a caller's buffer. Texel `(0, 0)` is the device pixel (`originX`, `originY`).
 * @param quad  the device rectangle the field is drawn over. It is the field's own lattice
 *              `[originX, originX + width) x [originY, originY + height)` for a field that was built at its lattice,
 *              and it must not be shrunk to the geometry's bounding box: a fragment exists only for a pixel whose
 *              *centre* is inside the quad, so a shape whose edge is at `x = 48.4` would lose the partly covered pixel
 *              48 -- measured, and recorded on `recordCoverageField`.
 * @param clipRect the device-space clip in force, as a rectangle. `Command::clipRect` defaults to a **zero** rect and
 *              the replay turns that into a zero-sized scissor, so a command that leaves it unset draws nothing while
 *              reporting no error anywhere. It is a parameter rather than a default for that reason.
 * @param clipMask the shaped clip in force, or `DrawList::kNoClipMask`.
 * @param layer which layer the command draws into: an index into `list.layers`.
 */
inline void appendCoverageFieldCommand(DrawList& list, DrawList::CoverageField field, const Rect& quad,
        const Rect& clipRect, const uint32_t clipMask, const uint32_t layer, const Paint& paint) {
  list.coverageFields.push_back(std::move(field));

  DrawList::Command command;
  // Recorded as a rectangle because that is the geometry the vertex writer lays out for a quad. The *shader* kind is
  // what routes it to the coverage-field program, and the replay tests that before the rectangle branch can claim it.
  command.kind = DrawList::Kind::Rect;
  command.state.shader = DrawList::ShaderKind::Coverage;
  command.state.coverageField = static_cast<uint32_t>(list.coverageFields.size() - 1);
  command.state.red = std::clamp(paint.color.red, 0.0f, 1.0f);
  command.state.green = std::clamp(paint.color.green, 0.0f, 1.0f);
  command.state.blue = std::clamp(paint.color.blue, 0.0f, 1.0f);
  command.state.alpha = std::clamp(paint.color.alpha, 0.0f, 1.0f);
  command.state.blendMode = paint.blendMode;
  // Always antialiased: a coverage field exists *because* the edge is feathered, so recording one as a hard edge would
  // contradict what it is. A caller that wants a hard edge has no reason to build a field.
  command.state.antiAlias = true;
  command.geometry[0] = quad;
  command.clipRect = clipRect;
  command.clipMask = clipMask;
  command.layer = layer;
  list.commands.push_back(command);
  // The caller's own bounds bookkeeping is theirs: the recorder unites the shape it drew, and the surface unites the
  // rectangle it was handed. Neither is derivable from the command shape above, which is why this returns nothing.
}

} // namespace graphics::gpu

#endif // CORAL_GRAPHICS_GPU_COVERAGE_RECORD_H
