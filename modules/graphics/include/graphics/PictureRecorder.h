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

#ifndef CORAL_GRAPHICS_PICTURE_RECORDER_H
#define CORAL_GRAPHICS_PICTURE_RECORDER_H

#include <graphics/Picture.h>

#include <geometry/Rect.h>
#include <geometry/Scalar.h>

#include <memory>

namespace graphics {

class Canvas;

/**
 * Collects drawing calls into a `Picture`.
 *
 * ## Use
 *
 * ```cpp
 * PictureRecorder recorder;
 * Canvas* canvas = recorder.beginRecording(bounds);
 * // ... draw, exactly as onto any other canvas ...
 * auto picture = recorder.finishRecording();
 * ```
 *
 * The canvas returned is an ordinary canvas in every respect except that its calls are remembered. Nothing about
 * drawing changes: the same methods, the same transforms, the same paints. A caller that draws something conditionally
 * gets a recording of what it actually drew, not of what it might have.
 *
 * ## The one thing that does not record
 *
 * The recording canvas has no target, so it draws nothing and holds no pixels. `peekPixels`, `snapshot` and
 * `detachBitmap` are therefore not meaningful on it, and a caller that needs pixels draws the finished picture onto a
 * real canvas instead -- `Picture::playback` exists for exactly that.
 *
 * ## Lifetime, and why the recorder is not the picture
 *
 * The recorder may be destroyed as soon as `finishRecording` returns, and a caller that wants a second picture from
 * the same recorder calls `beginRecording` again: that resets it. Nothing is shared between a recorder and the
 * pictures it has produced, so this is the ordinary RAII case rather than the reference's, where a partial replay can
 * be taken from a recorder that is still recording.
 */
class PictureRecorder {
public:
  PictureRecorder();
  ~PictureRecorder();

  PictureRecorder(const PictureRecorder&) = delete;
  PictureRecorder& operator=(const PictureRecorder&) = delete;
  PictureRecorder(PictureRecorder&&) noexcept;
  PictureRecorder& operator=(PictureRecorder&&) noexcept;

  /**
   * Starts a recording bounded by @p bounds, discarding anything recorded before.
   *
   * @return the canvas to draw on, which is owned by this recorder and valid until the next `beginRecording` or until
   *         the recorder dies.
   *
   * The bounds are a statement about where the content will be, and are carried into the picture as its cull
   * rectangle. They are not a clip: drawing outside them is recorded, and replays outside them.
   */
  Canvas* beginRecording(const Rect& bounds);

  /** The same, for a width and a height from the origin. */
  Canvas* beginRecording(Scalar width, Scalar height);

  /**
   * @return the recording so far, which is non-null while a recording is in progress.
   *
   * Null before `beginRecording` and after `finishRecording`. A caller that kept the pointer from `beginRecording`
   * does not need this; it is here for the caller that passed the recorder to something else.
   */
  [[nodiscard]] Canvas* recordingCanvas() const noexcept;

  /**
   * @return the finished picture, or null when nothing is being recorded.
   *
   * The recorder is left with nothing recorded, so a second call without another `beginRecording` returns null rather
   * than the same picture twice. That is the contract that makes `finishRecording` safe to call in a destructor path
   * or an error path without having to know how many times it has run.
   */
  [[nodiscard]] std::unique_ptr<Picture> finishRecording();

  /** Ends the recording, discarding it. */
  void reset();

  struct Impl;

private:
  std::unique_ptr<Impl> mImpl;
};

} // namespace graphics

#endif //CORAL_GRAPHICS_PICTURE_RECORDER_H
