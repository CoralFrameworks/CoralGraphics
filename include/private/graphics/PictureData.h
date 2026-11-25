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

#ifndef CORAL_PRIVATE_GRAPHICS_PICTURE_DATA_H
#define CORAL_PRIVATE_GRAPHICS_PICTURE_DATA_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

/**
 * @file PictureData.h
 *
 * What a recording holds, and why it holds it this way.
 *
 * This header is private to the project: it is included by the canvas that fills a recording and by the picture that
 * replays one, and it is not installed. See the note on `include/private` in `docs/graphics.md`.
 *
 * ## The representation, and the choice it makes
 *
 * A recording is a list of *bound calls*: each entry is a `std::function` that, given a canvas, performs one drawing
 * operation. Replaying is then a loop.
 *
 * The obvious alternative is a list of tagged structures -- a verb and its arguments -- which is what the reference
 * does, and which is what a picture has to do to be serialisable. It is much more machinery: every drawing operation
 * needs a verb, a destructuring in the recorder, a reconstructing in the player, and they have to be kept in step.
 *
 * The bound call avoids all of that at the cost of two properties:
 *
 *  - **A picture cannot be serialised.** There is no data to write out, only closures. The reference can write a
 *    picture to a stream and read it back; this cannot. That is a real limitation and it is why it is stated here
 *    rather than discovered.
 *  - **The recorded geometry is the recorded objects'. ** A closure captures its arguments by value, so a picture
 *    holds the path and the paint as they were when the call was made -- which is the property that matters. It
 *    cannot hold them more compactly than that.
 *
 * What it buys is that recording is *correct by construction*: a recorded call is the same call made later, so there
 * is no way for the pair to disagree. The reference's verb list can drift from its player, and a verb added to one
 * and not the other is a bug that only shows when that verb is drawn.
 *
 * ## What a recording cannot report
 *
 * `approximateOpCount` and `approximateBytesUsed` are estimates in the reference and remain estimates here; what a
 * closure holds is not knowable from outside it. The count is the number of recorded calls, which is exact for this
 * representation, and the byte estimate accounts for the closures themselves and not for what they captured.
 */

namespace graphics::picture_data {

/**
 * One recorded drawing operation.
 *
 * Takes the canvas by reference because a replay draws onto a caller's canvas, not onto one the recording owns. The
 * canvas's lifetime is the caller's, which is the whole point: the same recording plays onto any canvas.
 */
using Operation = std::function<void(class Canvas&)>;

/**
 * The calls a canvas has recorded, in the order they were made.
 *
 * A vector rather than a list: a replay walks it front to back and never inserts in the middle, which is what a
 * contiguous buffer is for. Reserving on the first record is what keeps a long recording from reallocating its way
 * through a frame.
 */
class Recording {
public:
  /** Adds @p operation to the end, taking ownership of whatever it captured. */
  void add(Operation operation) { mOperations.push_back(std::move(operation)); }

  /** @return true when nothing has been recorded. */
  [[nodiscard]] bool isEmpty() const noexcept { return mOperations.empty(); }

  /** @return how many calls were recorded, which is what the picture reports as its operation count. */
  [[nodiscard]] size_t size() const noexcept { return mOperations.size(); }

  /** @return the recorded calls, for a replay to walk. */
  [[nodiscard]] const std::vector<Operation>& operations() const noexcept { return mOperations; }

  /** Empties the recording, releasing every closure and whatever it captured. */
  void clear() noexcept { mOperations.clear(); }

  /** @return an estimate of the memory the closures themselves occupy, excluding what they captured. */
  [[nodiscard]] size_t approximateBytesUsed() const noexcept {
    return mOperations.size() * sizeof(Operation) + sizeof(*this);
  }

private:
  std::vector<Operation> mOperations;
};

} // namespace graphics::picture_data

#endif //CORAL_PRIVATE_GRAPHICS_PICTURE_DATA_H
