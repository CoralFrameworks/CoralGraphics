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

#ifndef CORAL_GPU_VIEWPORT_H
#define CORAL_GPU_VIEWPORT_H

#include <backend/Types.h>

namespace graphics::gpu {

/**
 * Viewport describes a view port in pixel coordinates
 *
 * A view port is represented by its left-bottom coordinate, width and height in pixels.
 */
class Viewport : backend::Viewport {
public:
  Viewport() noexcept
    : backend::Viewport() {
  }

  Viewport(const float x, const float y, const float width, const float height) noexcept
    : backend::Viewport{ .x = x, .y = y, .width = width, .height = height } {
  }

  /**
   * Returns whether the area of the view port is null.
   *
   * @return true if either width or height is 0 pixel.
   */
  [[nodiscard]] bool empty() const noexcept { return width <= 0 || height <= 0; }

  [[nodiscard]] float left() const noexcept { return this->x; }

  [[nodiscard]] float right() const noexcept { return this->x + this->width; }

  [[nodiscard]] float top() const noexcept { return this->y; }

  [[nodiscard]] float bottom() const noexcept { return this->y + this->height; }

  friend bool operator==(const Viewport& lhs, const Viewport& rhs) noexcept {
    return (&lhs == &rhs) ||
           (rhs.x == lhs.x && rhs.y == lhs.y
            && rhs.width == lhs.width && rhs.height == lhs.height &&
            rhs.minDepth == lhs.minDepth && rhs.maxDepth == lhs.maxDepth);
  }

  friend bool operator!=(const Viewport& lhs, const Viewport& rhs) noexcept { return !(lhs == rhs); }
};

} // namespace graphics::gpu

#endif // CORAL_GPU_VIEWPORT_H
