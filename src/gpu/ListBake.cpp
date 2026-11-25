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

/**
 * @file ListBake.cpp
 *
 * The one implementation of "a list with its own translation and clip, baked into commands", shared
 * by the offscreen surface and the swapchain one. Keeping it in one place is what makes the two
 * submission paths agree on which lists can share a frame and which cannot.
 */

#include <private/gpu/ListBake.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace graphics::gpu {

namespace {

/** @return true when @p kind paints pixels, as opposed to opening or closing a layer. */
[[nodiscard]] bool isDrawCommand(const DrawList::Kind kind) noexcept {
  return kind == DrawList::Kind::Rect || kind == DrawList::Kind::Path ||
    kind == DrawList::Kind::Image || kind == DrawList::Kind::ExternalImage;
}

/** @return the device rectangle a command's geometry occupies. */
[[nodiscard]] Rect commandBounds(const DrawList::Command& command) noexcept {
  if (command.kind == DrawList::Kind::Image || command.kind == DrawList::Kind::ExternalImage) {
    // Four corners, which may be rotated: the bounding box is the cull rectangle.
    float left = command.geometry[0].getLeft();
    float top = command.geometry[0].getTop();
    float right = command.geometry[0].getRight();
    float bottom = command.geometry[0].getBottom();
    for (Rect corner: command.geometry) {
      left = std::min(left, corner.getLeft());
      top = std::min(top, corner.getTop());
      right = std::max(right, corner.getRight());
      bottom = std::max(bottom, corner.getBottom());
    }
    return Rect::MakeLTRB(left, top, right, bottom);
  }
  return command.geometry[0];
}

/** @return true when @p a and @p b share any area. */
[[nodiscard]] bool rectsOverlap(const Rect& a, const Rect& b) noexcept {
  return a.getRight() > b.getLeft() && a.getLeft() < b.getRight() &&
    a.getBottom() > b.getTop() && a.getTop() < b.getBottom();
}

} // namespace

std::shared_ptr<const DrawList> bakeListState(const DrawList& source, const Rect clip,
        const Point translation) {
  const bool moved = translation.x != 0.0f || translation.y != 0.0f;
  if (!moved && clip.isEmpty()) return nullptr;
  if (moved && (translation.x != std::floor(translation.x) ||
                translation.y != std::floor(translation.y))) {
    return nullptr;
  }
  if (!source.clipMasks.empty() || !source.externalImages.empty()) return nullptr;
  const auto shifted = [&translation](const Rect rect) noexcept {
    return Rect::MakeLTRB(rect.getLeft() + translation.x, rect.getTop() + translation.y,
            rect.getRight() + translation.x, rect.getBottom() + translation.y);
  };
  auto list = std::make_shared<DrawList>(source.clone());
  list->commands.clear();
  list->commands.reserve(source.commands.size());
  for (DrawList::Command command: source.commands) {
    if (moved) {
      for (Rect& geometry: command.geometry) geometry = shifted(geometry);
    }
    // The command's clip stays where it was recorded: a translated list moves its geometry and keeps
    // its clip, which is exactly what `Canvas::translate` does to a canvas clip. The item clip is the
    // frame's scissor and is folded in here so the list needs no frame-level state of its own.
    if (!clip.isEmpty()) {
      const float left = std::max(command.clipRect.getLeft(), clip.getLeft());
      const float top = std::max(command.clipRect.getTop(), clip.getTop());
      const float right = std::min(command.clipRect.getRight(), clip.getRight());
      const float bottom = std::min(command.clipRect.getBottom(), clip.getBottom());
      if (right <= left || bottom <= top) continue;
      command.clipRect = Rect::MakeLTRB(left, top, right, bottom);
      // ## A command the clip cannot see is not recorded at all
      //
      // The command's own clipRect is the recording canvas's clip (the whole band), so it says nothing
      // about where the command draws. A band is two viewports tall; leaving the half inside the band
      // but outside the viewport in the list costs a draw call each for pixels the scissor would
      // discard -- 718 draws where the merged path records 409. Layer commands are kept: they pair with
      // their close, and dropping one would unbalance the list.
      if (isDrawCommand(command.kind) && !rectsOverlap(commandBounds(command), clip)) continue;
    }
    list->commands.push_back(std::move(command));
  }
  if (moved) {
    for (Point& point: list->points) {
      point.x += translation.x;
      point.y += translation.y;
    }
    // Layer 0 is the list's root and has no bounds of its own that a caller sees; the rest are device
    // rectangles for the layer targets the renderer allocates.
    for (size_t index = 1; index < list->layers.size(); ++index) {
      list->layers[index].bounds = shifted(list->layers[index].bounds);
    }
    for (DrawList::CoverageField& field: list->coverageFields) {
      field.originX += static_cast<int32_t>(std::lround(translation.x));
      field.originY += static_cast<int32_t>(std::lround(translation.y));
    }
    list->bounds = shifted(list->bounds);
  }
  return list;
}

} // namespace graphics::gpu
