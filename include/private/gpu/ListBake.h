/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CORAL_GPU_LISTBAKE_H
#define CORAL_GPU_LISTBAKE_H

#include <gpu/DrawList.h>

#include <memory>

namespace graphics::gpu {

/**
 * A copy of @p source moved by @p translation and cut to @p clip, or null when it cannot be baked.
 *
 * A GPU frame's canvas block carries one translation and one scissor, so a list that needs its own
 * would open a frame of its own. Baking both into the list's commands keeps every list of a frame in
 * one command buffer: the copy walks the commands once, shifts every device-space field and intersects
 * every command's clip, which is cheaper than a frame's setup and, unlike a frame, does not cost a
 * render pass.
 *
 * Whole device pixels only for the translation. A coverage field's origin is an integer device pixel
 * and its texels are sampled at that origin, and a shaped clip mask is rasterised in device space at
 * record time; neither can express a fractional shift, so both refuse rather than draw shifted
 * coverage. The clip itself may be fractional -- it becomes a scissor, which rounds the same way the
 * frame's did.
 *
 * Both submission paths use this: `Surface::submitLists` for an offscreen frame and
 * `WindowSurface::presentListItems` for a swapchain one, so the two cannot drift apart on what a
 * translated list means.
 */
[[nodiscard]] std::shared_ptr<const DrawList> bakeListState(const DrawList& source, Rect clip,
        Point translation);

} // namespace graphics::gpu

#endif // CORAL_GPU_LISTBAKE_H
