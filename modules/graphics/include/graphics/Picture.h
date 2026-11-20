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

#ifndef CORAL_GRAPHICS_PICTURE_H
#define CORAL_GRAPHICS_PICTURE_H

#include <geometry/Rect.h>

#include <cstdint>
#include <memory>

namespace graphics {

class Canvas;

/**
 * A recorded sequence of drawing calls that can be played onto a canvas.
 *
 * ## What it is for
 *
 * Drawing the same content more than once, or drawing it somewhere the calls did not anticipate. A picture is the
 * resolved form of a scene: every decision -- the transform stack, the clip, the resolved paint -- has been folded
 * into the calls, so a replay does not need the code that produced them and cannot disagree with it about what was
 * meant.
 *
 * The alternative is to draw the scene again from its description, which is often not possible: the description may be
 * gone by then, or may not be the caller's to re-run. A picture is the answer to "keep this and draw it later".
 *
 * ## What it is not
 *
 * **It is not serializable.** A recording here holds bound calls rather than a description of them, so there is
 * nothing to write to a stream and nothing to read back. The reference can do this; this cannot, deliberately -- see
 * the note in `include/private/graphics/PictureData.h` for what that buys and what it costs.
 *
 * **It is not a display list to be edited.** A picture is finished when it is made. There is no way to add to one or
 * to find a call in it, because the representation does not keep what would be needed to answer either question.
 *
 * ## Replay and the cull rectangle
 *
 * `playback` draws onto a caller's canvas, which must not be recording another picture -- a recording canvas replays
 * by recording, which would nest indefinitely. `cullRect` is the bound the recording was made with, and it is a
 * promise about where the content lies rather than a clip: a replay draws outside it if the recording did.
 */
class Picture {
public:
  ~Picture();

  Picture(const Picture&) = delete;
  Picture& operator=(const Picture&) = delete;
  Picture(Picture&&) noexcept;
  Picture& operator=(Picture&&) noexcept;

  /**
   * Draws the recording onto @p canvas, in the order the calls were made.
   *
   * The canvas's own transform and clip apply to the whole replay, as they would if the calls had been made directly.
   * Nothing is saved or restored around the replay, so a recording that leaves the canvas's state changed leaves it
   * changed -- which is what replaying the calls means, and the caller that wants isolation adds a `save`.
   */
  void playback(Canvas* canvas) const;

  /** @return the bound the recording was made with. */
  [[nodiscard]] const Rect& cullRect() const noexcept { return mCullRect; }

  /** @return the number of recorded drawing calls. */
  [[nodiscard]] int operationCount() const noexcept;

  /** @return an estimate of the memory the recording occupies. */
  [[nodiscard]] size_t approximateBytesUsed() const noexcept;

  /**
   * @return an identifier that is different for every picture made in this process.
   *
   * A counter rather than the object's address, because an address is reused after the object dies and a caller
   * keeping pictures in a set would then see two different pictures collide.
   */
  [[nodiscard]] uint32_t uniqueId() const noexcept { return mUniqueId; }

  struct Impl;

private:
  friend class PictureRecorder;

  explicit Picture(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> mImpl;
  Rect mCullRect = Rect::MakeLTRB(0.0f, 0.0f, 0.0f, 0.0f);
  uint32_t mUniqueId = 0;
};

} // namespace graphics

#endif //CORAL_GRAPHICS_PICTURE_H
