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

#include <effect/Fence.h>

#include "effect/EffectRenderCore.h"

#include <memory>
#include <utility>

namespace coral {

namespace detail {

/**
 * Observes one frame's CPU-side completion signal.
 *
 * No backend handle is kept here: the render thread polls the submission and raises the signal, so the
 * public handle is safe even if it outlives the frame pool or the engine.
 */
class FenceImpl {
public:
  explicit FenceImpl(std::shared_ptr<FrameCompletion> completion) : mCompletion(std::move(completion)) {}

  FenceImpl(const FenceImpl&) = delete;
  FenceImpl& operator=(const FenceImpl&) = delete;

  [[nodiscard]] bool isValid() const noexcept {
    return mCompletion != nullptr && !mCompletion->isDone();
  }

  Fence::Status wait(const Fence::Mode /*mode*/, const uint64_t timeoutNanoseconds) noexcept {
    if (mCompletion == nullptr) return Fence::Status::kError;
    return mCompletion->waitFor(timeoutNanoseconds);
  }

private:
  std::shared_ptr<FrameCompletion> mCompletion;
};

struct FenceAccess {
  static std::shared_ptr<FenceImpl> make(std::shared_ptr<FrameCompletion> completion) {
    return std::make_shared<FenceImpl>(std::move(completion));
  }

  static std::shared_ptr<Fence> makeFence(std::shared_ptr<FrameCompletion> completion) {
    if (completion == nullptr) return nullptr;
    auto fence = std::shared_ptr<Fence>(new Fence());
    fence->mImpl = make(std::move(completion));
    return fence;
  }
};

std::shared_ptr<Fence> makeFence(std::shared_ptr<FrameCompletion> completion) {
  return FenceAccess::makeFence(std::move(completion));
}

} // namespace detail

Fence::Fence() noexcept = default;
Fence::~Fence() = default;
Fence::Fence(Fence&&) noexcept = default;
Fence& Fence::operator=(Fence&&) noexcept = default;

bool Fence::isValid() const noexcept { return mImpl != nullptr && mImpl->isValid(); }

Fence::Status Fence::wait(const Mode mode, const uint64_t timeoutNanoseconds) {
  if (mImpl == nullptr) return Status::kError;
  return mImpl->wait(mode, timeoutNanoseconds);
}

Fence::Status Fence::waitAndDestroy(Fence* const fence, const Mode mode) {
  if (fence == nullptr) return Status::kError;
  const Status status = fence->wait(mode);
  delete fence;
  return status;
}

} // namespace coral
