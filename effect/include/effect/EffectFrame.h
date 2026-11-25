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

#ifndef CORAL_EFFECT_FRAME_H
#define CORAL_EFFECT_FRAME_H

#include <effect/EffectTypes.h>

#include <cstdint>
#include <memory>

namespace coral {

namespace detail {
struct FrameInfo;
struct FrameAccess;
} // namespace detail

/**
 * Immutable metadata for one frame that crosses the engine's node graph.
 *
 * A frame is **not** a texture and does not own GPU storage: the pool owns the texture, and the frame
 * only names it for as long as the graph holds the lease. Consequently there is no `texture()`,
 * `nativeHandle()` or `backendHandle()`: pixels leave the engine through an output node's readback, and
 * the metadata here is what a callback, a statistic or a test needs to correlate what it received.
 */
class EffectFrame {
public:
  EffectFrame() noexcept;
  ~EffectFrame();

  EffectFrame(const EffectFrame&) noexcept = default;
  EffectFrame& operator=(const EffectFrame&) noexcept = default;

  /** @return true when this names a frame. A default-constructed frame is invalid. */
  [[nodiscard]] bool isValid() const noexcept;

  /** A monotonically increasing identity, unique for the lifetime of the engine. */
  [[nodiscard]] uint64_t id() const noexcept;

  [[nodiscard]] uint32_t width() const noexcept;
  [[nodiscard]] uint32_t height() const noexcept;
  [[nodiscard]] EffectPixelFormat format() const noexcept;
  [[nodiscard]] EffectColorSpace colorSpace() const noexcept;

  /** The presentation time the producing input associated with this frame, in the engine's time base. */
  [[nodiscard]] RenderTime timestamp() const noexcept;

private:
  friend struct detail::FrameAccess;

  std::shared_ptr<const detail::FrameInfo> mInfo;
};

} // namespace coral

#endif // CORAL_EFFECT_FRAME_H
