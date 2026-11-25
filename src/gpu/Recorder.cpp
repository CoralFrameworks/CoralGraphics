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

/**
 * @file Recorder.cpp
 *
 * The recorder: one thread's drawing turned into a `DrawList`.
 *
 * See the header for what it is for and for the threading contract. This file is the mechanics -- a `Canvas` over the
 * recording backend, a `DrawList` behind it, and the hand-off in `finish` -- and it exists because `Recorder` was
 * declared with a constructor and **no definition anywhere**: every translation unit that named one would have failed to
 * link, and none did, which is exactly why the gap survived. A class nothing calls is a class whose absence nothing
 * reports. It is the piece the design calls for -- N threads recording N lists with no lock on the path -- so this is
 * that piece, and the test beside it starts the threads.
 */

#include <gpu/Recorder.h>

#include <private/gpu/ContextImpl.h>

#include <utility>

namespace graphics::gpu {

class Recorder::Impl {
public:
  Impl(const int width, const int height, ContextImpl* context, const char* label)
    : mWidth(width), mHeight(height), mLabel(label != nullptr ? label : "recorder"),
      mList(std::make_shared<DrawList>()) {
    mList->label = mLabel;
    // The context is *borrowed*. `createRecordingBackend` takes a `shared_ptr` because the backend keeps one and a
    // backend that outlived its context would dangle -- so the pointer is wrapped with a no-op deleter rather than
    // taken ownership of. A recorder that owned the device would start a second one per thread, which is the opposite of
    // what recording in parallel is for.
    mContext = context != nullptr ? std::shared_ptr<ContextImpl>(context, [](ContextImpl*) {}) : nullptr;
    mCanvas = Canvas::createRecordingBackend(width, height, mContext, mList);
  }

  /** Starts a fresh recording, which is what `finish` leaves behind and what the constructor does. */
  void begin() {
    mList = std::make_shared<DrawList>();
    mList->label = mLabel;
    mCanvas = Canvas::createRecordingBackend(mWidth, mHeight, mContext, mList);
  }

  int mWidth = 0;
  int mHeight = 0;
  std::shared_ptr<ContextImpl> mContext;
  std::string mLabel = "recorder";
  std::shared_ptr<DrawList> mList;
  std::unique_ptr<Canvas> mCanvas;
};

Recorder::Recorder(const int width, const int height, ContextImpl* context, const char* label)
  : mImpl(std::make_unique<Impl>(width, height, context, label)) {
  mCanvas = mImpl->mCanvas.get();
  mRecorded = mCanvas != nullptr;
}

Recorder::~Recorder() = default;

DrawList Recorder::finish() {
  if (mImpl == nullptr) return DrawList {};

  // Moved out rather than copied, which is what lets the caller hand the list to another thread: ownership carries the
  // data, and a lock would be the alternative. The recorder then starts a fresh recording, so a caller that records once
  // per frame keeps one recorder for the life of its surface.
  DrawList list = mImpl->mList != nullptr ? std::move(*mImpl->mList) : DrawList {};
  mImpl->begin();
  mCanvas = mImpl->mCanvas.get();
  mRecorded = mCanvas != nullptr;
  return list;
}

const DrawList& Recorder::drawList() const noexcept {
  static const DrawList kEmpty;
  return mImpl != nullptr && mImpl->mList != nullptr ? *mImpl->mList : kEmpty;
}

void Recorder::drawImage(const Bitmap& image, const Matrix& matrix, const Paint& paint) {
  if (mCanvas == nullptr || image.isEmpty()) return;

  // One call on the canvas rather than a command assembled here, and the difference matters: `Canvas::drawBitmap`
  // already knows how to place a bitmap under a transform, how to carry the paint's blend mode and alpha, and how to
  // register the bitmap with the list. Assembling a `DrawList::Command` directly would be a second implementation of
  // that, and the two would drift.
  //
  // The matrix is applied as the canvas's own transform around the call rather than passed, because a recording backend
  // has no per-draw matrix: it records device-space geometry, so the placement belongs in the transform in force when
  // the call is made. `save`/`restore` is what keeps that from leaking into whatever the caller draws next.
  mCanvas->save();
  mCanvas->concat(matrix);
  mCanvas->drawBitmap(image, 0.0f, 0.0f, paint);
  mCanvas->restore();
}

} // namespace graphics::gpu
