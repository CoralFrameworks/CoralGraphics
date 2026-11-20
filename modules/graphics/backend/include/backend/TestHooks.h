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

#ifndef CORAL_BACKEND_TESTHOOKS_H
#define CORAL_BACKEND_TESTHOOKS_H

namespace backend::test {

/**
 * Makes the next @p count frame acquires report `Result::DeviceLost`.
 *
 * This is a process-wide test seam used by the device-loss recovery tests. It defaults to zero and
 * must not be armed in production code.
 */
void failNextFrameAcquires(int count) noexcept;

/** Clears any injected frame failures. */
void clearDeviceLossInjection() noexcept;

/** Consumes one injected failure; @return true when the caller should report DeviceLost. */
[[nodiscard]] bool consumeDeviceLossInjection() noexcept;

} // namespace backend::test

#endif // CORAL_BACKEND_TESTHOOKS_H
