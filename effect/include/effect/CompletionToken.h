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

#ifndef CORAL_EFFECT_COMPLETION_TOKEN_H
#define CORAL_EFFECT_COMPLETION_TOKEN_H

#include <cstdint>

namespace coral {

/**
 * A producer-side completion signal attached to a frame.
 *
 * The effect library deliberately owns this interface instead of borrowing `media::SyncToken`, so
 * `effect`'s public headers do not depend on the media module. A platform source adapts its own
 * fence/token to this interface before submitting a frame.
 */
class CompletionToken {
public:
  virtual ~CompletionToken() = default;

  /** Blocks until signalled or @p timeoutNanoseconds elapsed. Returns false on timeout/error. */
  virtual bool wait(uint64_t timeoutNanoseconds) = 0;

  /** Polls without blocking. */
  [[nodiscard]] virtual bool isSignaled() const = 0;
};

} // namespace coral

#endif // CORAL_EFFECT_COMPLETION_TOKEN_H
