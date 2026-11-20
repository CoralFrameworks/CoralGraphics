/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstdint>
#include <string>

namespace coral::detail {

/**
 * A shader cache as it looks at the end of one `start()`.
 *
 * The counters are process-wide, so a caller takes the difference around its own start-up to get the per-start
 * numbers this watch reasons about.
 */
struct ShaderCacheSample {
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t entries = 0;
  uint64_t bytes = 0;
  uint64_t budget = 0;
  bool directorySet = false;
};

/**
 * Watches the shader cache across starts and says when it has stopped doing its job.
 *
 * A cache that is configured but never hits is invisible in a single run: the frames still render, the numbers just
 * keep saying `misses=N entries=M`. Two of those shapes are defects rather than cold starts, and both were found by
 * reading the counters by hand:
 *
 * - writes are not landing (`directorySet`, misses every start, entries never grow) - an unwritable directory or a
 *   store path that was never called, which is exactly what the OpenGL compile path did for weeks;
 * - keys are unstable (entries grow, yet every start misses) - something in the key changes per process, such as a
 *   salt, a temporary path, or a timestamp.
 *
 * One start proves nothing, so this only reports after a run of them, and never more than once per cause.
 */
class ShaderCacheWatch final {
public:
  /** How many starts in a row must miss before a cache that has entries is called unstable. */
  static constexpr uint32_t kStartsBeforeUnstable = 10;
  /** How many starts in a row may store nothing before a configured cache is called broken. */
  static constexpr uint32_t kStartsBeforeBroken = 3;

  /**
   * Records one start's cache numbers.
   *
   * @return a warning the caller should report, or an empty string while the cache looks healthy or unproven.
   */
  [[nodiscard]] std::string record(const ShaderCacheSample& sample);

  /** @return true when a snapshot shows the cache is over its byte budget, which the trimmer should prevent. */
  [[nodiscard]] static bool overBudget(const ShaderCacheSample& sample) noexcept;

  /** Forget everything, for a test or a deliberate reconfiguration. */
  void reset() noexcept;

  /**
   * @return a one-line form of the state, so it can be carried across processes.
   *
   * A cache that never hits is only visible over a run of *starts*, and most applications create the engine once
   * per process - so the state has to outlive the process to be worth anything.
   */
  [[nodiscard]] std::string save() const;

  /** Restores state from `save()`. @return false when @p text is not one of ours; the state is left unchanged. */
  bool restore(const std::string& text);

  [[nodiscard]] uint32_t consecutiveMisses() const noexcept { return mConsecutiveMisses; }
  [[nodiscard]] uint32_t consecutiveEmptyStarts() const noexcept { return mConsecutiveEmptyStarts; }

private:
  uint32_t mConsecutiveMisses = 0;
  uint32_t mConsecutiveEmptyStarts = 0;
  uint64_t mLastEntries = 0;
  bool mReportedUnstable = false;
  bool mReportedBroken = false;
  bool mReportedOverBudget = false;
};

/** @return the shader cache as it is right now, read from the backend's process-wide counters. */
[[nodiscard]] ShaderCacheSample sampleShaderCache();

} // namespace coral::detail
