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
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CORAL_GRAPHICS_LOTTIE_LOTTIE_RENDERER_H
#define CORAL_GRAPHICS_LOTTIE_LOTTIE_RENDERER_H

#include "lottie/LottieModel.h"

#include <graphics/Canvas.h>

#include <geometry/Rect.h>
#include <geometry/Scalar.h>

/**
 * @file LottieRenderer.h
 *
 * Drawing a parsed document at a moment.
 *
 * ## Why the document is walked rather than compiled
 *
 * Skottie builds a scene-graph of animation nodes and then renders it, so that the structure is built once and only the
 * values change per frame. That is the right shape for a player that runs for minutes; it is the wrong shape for this
 * one. A document that is scrubbed, exported frame by frame, or played for five seconds gains nothing from the
 * indirection, and the tree would have to mirror the model exactly -- a second representation of the same thing, kept in
 * step by hand.
 *
 * So the walk happens per frame. The cost is that a group's path is rebuilt from its vertices every time, which is
 * measuring a few hundred cubics; the benefit is that the model is the only description of the document.
 *
 * ## The shape stack
 *
 * A Lottie group's items are ordered, and the order carries meaning: geometry accumulates, a merge combines what has
 * accumulated, and a fill paints what the merge produced. Walking them in order and keeping a stack of pending geometry
 * is that rule written once -- and it is why a fill must follow the geometry it paints, which every exporter respects and
 * the format's own examples show.
 */

namespace graphics::lottie {

/**
 * Draws @p animation at @p frame into @p canvas, mapped so its composition size fills @p bounds.
 *
 * @param frame in the document's own frames, not seconds. A frame outside the composition's in and out points draws the
 *        nearest frame within them, because a player that has run off the end shows the last frame rather than nothing.
 */
void renderFrame(const Animation& animation, Canvas& canvas, const Rect& bounds, Scalar frame);

/** Draws at the composition's own size, at the origin. */
void renderFrame(const Animation& animation, Canvas& canvas, Scalar frame);

} // namespace graphics::lottie

#endif // CORAL_GRAPHICS_LOTTIE_LOTTIE_RENDERER_H
