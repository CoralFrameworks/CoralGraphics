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

#include "effect/ShaderCacheWatch.h"

#include <backend/ShaderCompiler.h>

#include <cstdio>

namespace coral::detail {

std::string ShaderCacheWatch::record(const ShaderCacheSample& sample) {
  if (!sample.directorySet) {
    // Nothing is configured, so there is nothing to be wrong with it. A build without a cache is a choice.
    reset();
    return {};
  }

  const bool compiled = sample.hits + sample.misses > 0;
  const bool stored = sample.entries > mLastEntries;
  mLastEntries = sample.entries;

  if (!compiled) {
    // Nothing was compiled this start: a warm cache that hit everywhere, or a backend that never uses it. Either
    // way it is evidence the cache works, not evidence against it.
    mConsecutiveMisses = 0;
    mConsecutiveEmptyStarts = 0;
    return {};
  }

  if (sample.misses == 0) mConsecutiveMisses = 0;
  else ++mConsecutiveMisses;

  if (stored) mConsecutiveEmptyStarts = 0;
  else ++mConsecutiveEmptyStarts;

  if (overBudget(sample) && !mReportedOverBudget) {
    mReportedOverBudget = true;
    return "shader cache: " + std::to_string(sample.bytes) + " bytes held against a budget of " +
           std::to_string(sample.budget) + "; the trimmer is not keeping up";
  }

  // A cache with entries that stores nothing on a start which compiled is the shape of a missing write.
  if (sample.entries == 0 && !stored && mConsecutiveEmptyStarts >= kStartsBeforeBroken && !mReportedBroken) {
    mReportedBroken = true;
    return "shader cache: " + std::to_string(mConsecutiveEmptyStarts) +
           " starts compiled shaders and stored none, with an empty cache directory; writes are not landing";
  }

  // A cache that grows yet still misses every start means the key moves between processes.
  if (sample.entries > 0 && mConsecutiveMisses >= kStartsBeforeUnstable && !mReportedUnstable) {
    mReportedUnstable = true;
    return "shader cache: " + std::to_string(mConsecutiveMisses) +
           " starts in a row each missed while the cache grew to " + std::to_string(sample.entries) +
           " entries; the key is probably not stable across processes (salt, path or timestamp)";
  }
  return {};
}

bool ShaderCacheWatch::overBudget(const ShaderCacheSample& sample) noexcept {
  return sample.budget != 0 && sample.bytes > sample.budget;
}

void ShaderCacheWatch::reset() noexcept {
  mConsecutiveMisses = 0;
  mConsecutiveEmptyStarts = 0;
  mLastEntries = 0;
  mReportedUnstable = false;
  mReportedBroken = false;
  mReportedOverBudget = false;
}

/** @return the shader cache as it is right now, read from the backend's process-wide counters. */
ShaderCacheSample sampleShaderCache();

std::string ShaderCacheWatch::save() const {
  char text[128];
  std::snprintf(text, sizeof(text), "coral-cache-watch 1 misses=%u empty=%u entries=%llu reported=%d%d%d",
                mConsecutiveMisses, mConsecutiveEmptyStarts,
                static_cast<unsigned long long>(mLastEntries), mReportedUnstable ? 1 : 0,
                mReportedBroken ? 1 : 0, mReportedOverBudget ? 1 : 0);
  return text;
}

bool ShaderCacheWatch::restore(const std::string& text) {
  unsigned misses = 0;
  unsigned empty = 0;
  unsigned long long entries = 0;
  int unstable = 0;
  int broken = 0;
  int overBudget = 0;
  // Anything that is not exactly this shape is from an older or a corrupted file: start clean rather than guess.
  if (std::sscanf(text.c_str(), "coral-cache-watch 1 misses=%u empty=%u entries=%llu reported=%1d%1d%1d", &misses,
                  &empty, &entries, &unstable, &broken, &overBudget) != 6) {
    return false;
  }
  mConsecutiveMisses = misses;
  mConsecutiveEmptyStarts = empty;
  mLastEntries = entries;
  mReportedUnstable = unstable != 0;
  mReportedBroken = broken != 0;
  mReportedOverBudget = overBudget != 0;
  return true;
}

ShaderCacheSample sampleShaderCache() {
  ShaderCacheSample sample;
  sample.directorySet = !backend::shaderCacheDirectory().empty();
  sample.hits = backend::shaderCacheHits();
  sample.misses = backend::shaderCacheMisses();
  backend::shaderCacheUsage(&sample.entries, &sample.bytes);
  sample.budget = backend::shaderCacheBudget();
  return sample;
}

} // namespace coral::detail
