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

#ifndef CORAL_GPU_EXTERNAL_IMAGE_RECORD_H
#define CORAL_GPU_EXTERNAL_IMAGE_RECORD_H

#include <gpu/DrawList.h>

#include <graphics/Paint.h>

#include <algorithm>

namespace graphics::gpu {

/**
 * Appends @p image and the command that samples it to @p list.
 *
 * The record is the GPU counterpart of `Canvas::drawImage`: where that one names pixels the list owns,
 * this names a view another producer on the same device owns. Everything else -- the four corners, the
 * texture coordinates, the paint's alpha and the clip -- is laid out the same way, because `image.frag`
 * cannot tell the two apart.
 *
 * @param quad the device-space corners, in the order top-left, top-right, bottom-right, bottom-left.
 *             Texture coordinates run `(0, 0)` to `(1, 1)` across it, so a caller that wants a
 *             sub-rectangle of the external texture must record one command per sub-rectangle.
 * @param clipRect the device-space clip in force. `Command::clipRect` defaults to a zero rect and the
 *             replay turns that into a zero-sized scissor, so a command that leaves it unset draws
 *             nothing while reporting no error anywhere.
 * @param clipMask the shaped clip in force, or `DrawList::kNoClipMask`.
 * @param layer which layer the command draws into: an index into `list.layers`.
 */
inline void appendExternalImageCommand(DrawList& list, const DrawList::ExternalImage& image,
        const Rect& quad, const Rect& clipRect, const uint32_t clipMask, const uint32_t layer,
        const Paint& paint) {
  list.externalImages.push_back(image);

  DrawList::Command command;
  command.kind = DrawList::Kind::ExternalImage;
  command.state.shader = DrawList::ShaderKind::Image;
  command.state.image = static_cast<uint32_t>(list.externalImages.size() - 1);
  command.state.red = std::clamp(paint.color.red, 0.0f, 1.0f);
  command.state.green = std::clamp(paint.color.green, 0.0f, 1.0f);
  command.state.blue = std::clamp(paint.color.blue, 0.0f, 1.0f);
  command.state.alpha = std::clamp(paint.color.alpha, 0.0f, 1.0f);
  command.state.blendMode = paint.blendMode;
  command.state.antiAlias = paint.antiAlias;
  // `Command::geometry` carries four `Rect`s because a rectangle and a layer region need one; an image
  // reuses them as four points, corner *i* being `(geometry[i].getLeft(), geometry[i].getTop())`.
  const float left = quad.getLeft();
  const float top = quad.getTop();
  const float right = quad.getRight();
  const float bottom = quad.getBottom();
  command.geometry[0] = Rect::MakeLTRB(left, top, left, top);
  command.geometry[1] = Rect::MakeLTRB(right, top, right, top);
  command.geometry[2] = Rect::MakeLTRB(right, bottom, right, bottom);
  command.geometry[3] = Rect::MakeLTRB(left, bottom, left, bottom);
  // Top-left, top-right, bottom-right, bottom-left -- paired with a full-texture UV rectangle.
  command.texCoords[0] = Point { 0.0f, 0.0f };
  command.texCoords[1] = Point { 1.0f, 0.0f };
  command.texCoords[2] = Point { 1.0f, 1.0f };
  command.texCoords[3] = Point { 0.0f, 1.0f };
  command.sampling = SamplingOptions(FilterMode::kLinear);
  command.clipRect = clipRect;
  command.clipMask = clipMask;
  command.layer = layer;
  list.commands.push_back(command);
}

} // namespace graphics::gpu

#endif // CORAL_GPU_EXTERNAL_IMAGE_RECORD_H
