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

#include <backend/TestHooks.h>

#include <atomic>

namespace backend::test {

namespace {

std::atomic<int> gInjectedFrameFailures{ 0 };

} // namespace

void failNextFrameAcquires(const int count) noexcept {
  gInjectedFrameFailures.store(count < 0 ? 0 : count, std::memory_order_relaxed);
}

void clearDeviceLossInjection() noexcept {
  gInjectedFrameFailures.store(0, std::memory_order_relaxed);
}

bool consumeDeviceLossInjection() noexcept {
  int remaining = gInjectedFrameFailures.load(std::memory_order_relaxed);
  while (remaining > 0) {
    if (gInjectedFrameFailures.compare_exchange_weak(remaining, remaining - 1,
          std::memory_order_relaxed)) {
      return true;
    }
  }
  return false;
}

} // namespace backend::test
