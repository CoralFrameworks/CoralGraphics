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

#include <graphics/Picture.h>

#include <graphics/Canvas.h>
#include <graphics/PictureRecorder.h>
#include <private/graphics/PictureData.h>

#include <atomic>
#include <utility>

#include <minilog/log.h>

namespace graphics {

/**
 * The recording, and the identifier counter.
 *
 * The counter is a function-local static rather than a member: a picture needs an identifier that no other picture in
 * the process has ever had, and the counter has to outlive every picture. `std::atomic` because two threads can finish
 * a recording at the same time, and two pictures that shared an identifier would make any cache keyed on it serve the
 * wrong content.
 */
struct Picture::Impl {
  picture_data::Recording recording;
};

namespace {
std::atomic<uint32_t>& nextPictureId() {
  static std::atomic<uint32_t> counter { 1 };
  return counter;
}
} // namespace anonymous

Picture::Picture(std::unique_ptr<Impl> impl)
    : mImpl(std::move(impl)), mUniqueId(nextPictureId().fetch_add(1, std::memory_order_relaxed)) {}

Picture::~Picture() = default;

Picture::Picture(Picture&&) noexcept = default;
Picture& Picture::operator=(Picture&&) noexcept = default;

void Picture::playback(Canvas* canvas) const {
  if (canvas == nullptr || mImpl == nullptr) return;

  // Replaying onto a recording canvas would record the replay, and a replay of *that* would record it again. The
  // nesting has no fixed point, so it is refused rather than allowed to grow. A caller that wants a picture of a
  // picture draws it onto a surface and records that.
  if (canvas->recordedOps() != nullptr) {
    MINILOG_WARN("[graphics/picture] playback onto a recording canvas is refused; it would record the replay");
    return;
  }

  // In order, and without a save around it: a replay is the recorded calls made again, so a recording that leaves the
  // canvas transformed leaves it transformed. A caller that wants the replay isolated adds its own save, which is one
  // call rather than a subtle difference in what playback means.
  for (const picture_data::Operation& operation: mImpl->recording.operations()) {
    if (operation) operation(*canvas);
  }
}

int Picture::operationCount() const noexcept {
  return mImpl == nullptr ? 0 : static_cast<int>(mImpl->recording.size());
}

size_t Picture::approximateBytesUsed() const noexcept {
  return mImpl == nullptr ? 0 : mImpl->recording.approximateBytesUsed();
}

// ==================================================================================================
// The recorder
// ==================================================================================================

struct PictureRecorder::Impl {
  /** The canvas whose calls are recorded. Owned, because it exists only for this recording. */
  std::unique_ptr<Canvas> canvas;

  /** The recording being filled, which is moved into the picture when it is finished. */
  picture_data::Recording recording;

  /** The bound the current recording was started with, which becomes the picture's cull rectangle. */
  Rect bounds = Rect::MakeLTRB(0.0f, 0.0f, 0.0f, 0.0f);

  /** Whether a recording is in progress. */
  bool active = false;

  /** Ends the recording, detaching the canvas from it. */
  void stop() {
    if (canvas != nullptr) canvas->attachRecorder(nullptr);
    active = false;
  }
};

PictureRecorder::PictureRecorder(): mImpl(std::make_unique<Impl>()) {}

PictureRecorder::~PictureRecorder() {
  // The canvas holds a pointer to the recording, so it has to be detached before the recording dies. The order here
  // is the whole of the destructor: `Impl`'s members are destroyed in reverse declaration order, which would release
  // the recording before the canvas that points at it.
  if (mImpl != nullptr) mImpl->stop();
}

PictureRecorder::PictureRecorder(PictureRecorder&&) noexcept = default;
PictureRecorder& PictureRecorder::operator=(PictureRecorder&&) noexcept = default;

Canvas* PictureRecorder::beginRecording(const Rect& bounds) {
  // Anything previously recorded is discarded, and the canvas is detached from it before it is cleared -- so a
  // recording in progress cannot be left pointing at a buffer that has been emptied underneath it.
  mImpl->stop();
  mImpl->recording.clear();
  mImpl->bounds = bounds;

  if (mImpl->canvas == nullptr) mImpl->canvas = std::unique_ptr<Canvas>(new Canvas());
  mImpl->canvas->attachRecorder(&mImpl->recording);
  mImpl->active = true;
  return mImpl->canvas.get();
}

Canvas* PictureRecorder::beginRecording(const Scalar width, const Scalar height) {
  return beginRecording(Rect::MakeLTRB(0.0f, 0.0f, width, height));
}

Canvas* PictureRecorder::recordingCanvas() const noexcept {
  return mImpl->active ? mImpl->canvas.get() : nullptr;
}

std::unique_ptr<Picture> PictureRecorder::finishRecording() {
  if (!mImpl->active) return nullptr;

  mImpl->stop();

  auto impl = std::make_unique<Picture::Impl>();
  impl->recording = std::move(mImpl->recording);

  // The recording was moved out, so what remains is empty; `clear` is what puts it back in a state `beginRecording`
  // can reuse without a second allocation of its buffer.
  mImpl->recording.clear();

  auto picture = std::unique_ptr<Picture>(new Picture(std::move(impl)));
  picture->mCullRect = mImpl->bounds;
  return picture;
}

void PictureRecorder::reset() {
  mImpl->stop();
  mImpl->recording.clear();
  mImpl->bounds = Rect::MakeLTRB(0.0f, 0.0f, 0.0f, 0.0f);
}

} // namespace graphics
