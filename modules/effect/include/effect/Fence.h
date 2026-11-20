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

#ifndef CORAL_EFFECT_FENCE_H
#define CORAL_EFFECT_FENCE_H

#include <cstdint>
#include <memory>

namespace coral {

namespace detail {
class FenceImpl;
struct FenceAccess;
} // namespace detail

/**
 * A waitable signal for one submitted frame's GPU completion.
 *
 * `EffectEngine::execute(&fence)` returns one; waiting blocks until that frame's submission fence is
 * signalled, the timeout expires, or the engine shuts down (then it reports `kError`). Ordinary
 * applications can keep using `flushAndWait()` for "everything is done" barriers.
 *
 * The object is a handle, not the signal: waiting blocks the calling thread and must never be done on
 * the render thread, which would be waiting for work it has not submitted yet.
 */
class Fence {
public:
  //! Special timeout value that disables `wait()`'s timeout.
  static constexpr uint64_t kWaitForEver = static_cast<uint64_t>(-1);

  enum class Status : int8_t {
    /** The wait failed, or the fence was not valid. */
    kError = -1,
    /** The signal has been raised. */
    kConditionSatisfied = 0,
    /** The timeout expired before the signal was raised. */
    kTimeoutExpired = 1,
  };

  enum class Mode : uint8_t {
    /** Submit any pending command stream before waiting. */
    kFlush,
    /** Wait without flushing. */
    kDontFlush,
  };

  Fence() noexcept;
  ~Fence();

  Fence(const Fence&) = delete;
  Fence& operator=(const Fence&) = delete;
  Fence(Fence&&) noexcept;
  Fence& operator=(Fence&&) noexcept;

  /**
   * Blocks until the fence is signalled, the timeout expires, or the wait fails.
   *
   * @param mode    whether the engine should flush pending work first.
   * @param timeout nanoseconds to wait; `kWaitForEver` waits indefinitely, 0 polls.
   */
  Status wait(Mode mode = Mode::kFlush, uint64_t timeoutNanoseconds = kWaitForEver);

  /**
   * Waits on @p fence and then destroys it.
   *
   * The fence must have been allocated with `new`; ownership passes to this call. Prefer letting a
   * `shared_ptr<Fence>` go out of scope, which needs no explicit destroy.
   */
  static Status waitAndDestroy(Fence* fence, Mode mode = Mode::kFlush);

  /** @return true when this handle names a fence that has not been destroyed. */
  [[nodiscard]] bool isValid() const noexcept;

private:
  friend struct detail::FenceAccess;

  std::shared_ptr<detail::FenceImpl> mImpl;
};

} // namespace coral

#endif // CORAL_EFFECT_FENCE_H
