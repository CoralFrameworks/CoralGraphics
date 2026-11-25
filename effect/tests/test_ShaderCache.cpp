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
 * @file test_ShaderCache.cpp
 *
 * The cross-process SPIR-V cache's contract, without a GPU: what a hit is, what a miss is, and what invalidates.
 *
 * These cases exist because the first version of the cache wrote every entry correctly and read none of them
 * back - a hash stored at the wrong width - and "the files are there" was true the whole time. Every assertion
 * here is about behaviour, not files.
 */

#include <backend/ShaderCompiler.h>

#include "effect/ShaderCacheWatch.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr const char* kVertex = "#version 300 es\nlayout(location = 0) in vec2 aPos;\nvoid main() { gl_Position = "
                                "vec4(aPos, 0.0, 1.0); }\n";

std::string fragment(const char* body) {
  // `#version 310 es` is the floor the Vulkan compiler accepts; 300 es is retargeted upward by the caller but a
  // direct call here must already be legal.
  return std::string("#version 310 es\nprecision mediump float;\nlayout(location = 0) out vec4 o;\nvoid main() { ") +
         body + " }\n";
}

/** A directory that lives for one test and is removed afterwards, inside the working directory. */
class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    mPath = std::filesystem::current_path() / ("coral-shader-cache-test-" + std::to_string(stamp));
    std::filesystem::create_directories(mPath);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(mPath, error);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] std::string path() const { return mPath.string(); }

private:
  std::filesystem::path mPath;
};

/** Compiles @p source and reports whether it reached the compiler (a miss) or came from the cache (a hit). */
struct CompileOutcome {
  bool compiled = false;
  uint64_t hits = 0;
  uint64_t misses = 0;
  bool success = false;
};

CompileOutcome compile(const std::string& source, const backend::ShaderStage stage) {
  const uint64_t hitsBefore = backend::shaderCacheHits();
  const uint64_t missesBefore = backend::shaderCacheMisses();
  backend::ShaderCompileResult result;
  const backend::Result status = backend::compileShaderToSpirv(source, stage, result);
  CompileOutcome outcome;
  outcome.hits = backend::shaderCacheHits() - hitsBefore;
  outcome.misses = backend::shaderCacheMisses() - missesBefore;
  outcome.compiled = outcome.misses > 0;
  outcome.success = backend::isSuccess(status) && !result.spirv.empty();
  if (!outcome.success) {
    std::fprintf(stderr, "[shader-cache-test] compile failed: %s\n", result.error.c_str());
  }
  return outcome;
}

/** Restores the process-wide cache settings when the test ends, so one case cannot leak into the next. */
class CacheSettingsGuard {
public:
  CacheSettingsGuard()
      : mDirectory(backend::shaderCacheDirectory()), mSalt(backend::shaderCacheSalt()),
        mBudget(backend::shaderCacheBudget()) {}
  ~CacheSettingsGuard() {
    backend::setShaderCacheDirectory(mDirectory);
    backend::setShaderCacheSalt(mSalt);
    backend::setShaderCacheBudget(mBudget);
  }

  CacheSettingsGuard(const CacheSettingsGuard&) = delete;
  CacheSettingsGuard& operator=(const CacheSettingsGuard&) = delete;

private:
  std::string mDirectory;
  std::string mSalt;
  uint64_t mBudget = 0;
};

} // namespace

TEST(ShaderCache, UnchangedSourceIsServedFromTheCacheTheSecondTime) {
  if (!backend::isShaderCompilerAvailable()) GTEST_SKIP() << "this build has no GLSL compiler";
  const CacheSettingsGuard guard;
  const TemporaryDirectory directory;
  backend::setShaderCacheDirectory(directory.path());
  backend::setShaderCacheSalt("test-unchanged");

  const std::string source = fragment("o = vec4(1.0, 0.0, 0.0, 1.0);");
  const CompileOutcome first = compile(source, backend::ShaderStage::Fragment);
  ASSERT_TRUE(first.success) << "the first compile should succeed";
  EXPECT_TRUE(first.compiled) << "the first compile must reach the compiler";

  const CompileOutcome second = compile(source, backend::ShaderStage::Fragment);
  ASSERT_TRUE(second.success);
  EXPECT_FALSE(second.compiled) << "the identical source must come from the cache";
  EXPECT_EQ(second.hits, 1u);
}

TEST(ShaderCache, ChangedSourceIsRecompiledAndTheOldEntryStaysUsable) {
  if (!backend::isShaderCompilerAvailable()) GTEST_SKIP() << "this build has no GLSL compiler";
  const CacheSettingsGuard guard;
  const TemporaryDirectory directory;
  backend::setShaderCacheDirectory(directory.path());
  backend::setShaderCacheSalt("test-changed");

  const std::string red = fragment("o = vec4(1.0, 0.0, 0.0, 1.0);");
  const std::string green = fragment("o = vec4(0.0, 1.0, 0.0, 1.0);");
  ASSERT_TRUE(compile(red, backend::ShaderStage::Fragment).success);

  // A dynamically delivered shader with different text must not be served the old module.
  const CompileOutcome changed = compile(green, backend::ShaderStage::Fragment);
  ASSERT_TRUE(changed.success);
  EXPECT_TRUE(changed.compiled) << "different source text must be a miss";

  // Both entries now exist, so going back to the first one is a hit again: a version switch does not have to
  // recompile, it only has to be a different key.
  const CompileOutcome back = compile(red, backend::ShaderStage::Fragment);
  ASSERT_TRUE(back.success);
  EXPECT_FALSE(back.compiled) << "the earlier shader is still cached under its own key";
}

TEST(ShaderCache, BumpingTheSaltInvalidatesEveryEntry) {
  if (!backend::isShaderCompilerAvailable()) GTEST_SKIP() << "this build has no GLSL compiler";
  const CacheSettingsGuard guard;
  const TemporaryDirectory directory;
  backend::setShaderCacheDirectory(directory.path());

  backend::setShaderCacheSalt("pipeline-1");
  const std::string source = fragment("o = vec4(0.0, 0.0, 1.0, 1.0);");
  ASSERT_TRUE(compile(source, backend::ShaderStage::Fragment).success);
  ASSERT_FALSE(compile(source, backend::ShaderStage::Fragment).compiled);

  // A new engine pipeline that composes something different around the same text: the salt is how that is
  // expressed, because the text alone cannot say it.
  backend::setShaderCacheSalt("pipeline-2");
  const CompileOutcome afterBump = compile(source, backend::ShaderStage::Fragment);
  ASSERT_TRUE(afterBump.success);
  EXPECT_TRUE(afterBump.compiled) << "a new salt must force a recompile";

  EXPECT_FALSE(compile(source, backend::ShaderStage::Fragment).compiled) << "the new salt is cached in its turn";
}

TEST(ShaderCache, StageIsPartOfTheKey) {
  if (!backend::isShaderCompilerAvailable()) GTEST_SKIP() << "this build has no GLSL compiler";
  const CacheSettingsGuard guard;
  const TemporaryDirectory directory;
  backend::setShaderCacheDirectory(directory.path());
  backend::setShaderCacheSalt("test-stage");

  ASSERT_TRUE(compile(kVertex, backend::ShaderStage::Vertex).success);
  const CompileOutcome again = compile(kVertex, backend::ShaderStage::Vertex);
  EXPECT_FALSE(again.compiled) << "the identical vertex shader is a hit";

  // The same text as a fragment shader is a different program: it may fail to compile, but it must not be served
  // the vertex stage's module.
  const CompileOutcome asFragment = compile(kVertex, backend::ShaderStage::Fragment);
  EXPECT_TRUE(asFragment.compiled) << "a different stage must not be served from another stage's entry";
  EXPECT_EQ(asFragment.hits, 0u);
}

TEST(ShaderCache, AnEmptyDirectoryDisablesTheCache) {
  if (!backend::isShaderCompilerAvailable()) GTEST_SKIP() << "this build has no GLSL compiler";
  const CacheSettingsGuard guard;
  backend::setShaderCacheDirectory(std::string());
  backend::setShaderCacheSalt("test-disabled");

  const std::string source = fragment("o = vec4(0.5, 0.5, 0.5, 1.0);");
  ASSERT_TRUE(compile(source, backend::ShaderStage::Fragment).success);
  const CompileOutcome second = compile(source, backend::ShaderStage::Fragment);
  ASSERT_TRUE(second.success);
  EXPECT_TRUE(second.compiled) << "with no directory every compile reaches the compiler";
  EXPECT_EQ(second.hits, 0u);
}

TEST(ShaderCache, WarmingUpTheCompilerDoesNotTouchTheCache) {
  if (!backend::isShaderCompilerAvailable()) GTEST_SKIP() << "this build has no GLSL compiler";
  const CacheSettingsGuard guard;
  const TemporaryDirectory directory;
  backend::setShaderCacheDirectory(directory.path());
  backend::setShaderCacheSalt("test-warm-up");

  // The engine warms the compiler during start() so the first frame does not pay for it. That must not leave a
  // dummy entry behind: the cache is keyed by real shaders, and a warm-up entry would be a permanent stranger in
  // the user's directory. The counters are process-wide, so this compares against the values it found.
  const uint64_t hitsBefore = backend::shaderCacheHits();
  const uint64_t missesBefore = backend::shaderCacheMisses();
  EXPECT_TRUE(backend::warmUpShaderCompiler());
  uint64_t entries = 1;
  uint64_t bytes = 1;
  backend::shaderCacheUsage(&entries, &bytes);
  EXPECT_EQ(entries, 0u) << "the warm-up must not write a cache entry";
  EXPECT_EQ(backend::shaderCacheHits(), hitsBefore) << "the warm-up is neither a hit nor a miss";
  EXPECT_EQ(backend::shaderCacheMisses(), missesBefore);

  // And the compiler really is ready: a real compile immediately afterwards is fast and correct.
  const CompileOutcome outcome = compile(fragment("o = vec4(0.25, 0.5, 0.75, 1.0);"), backend::ShaderStage::Fragment);
  EXPECT_TRUE(outcome.success);
  EXPECT_TRUE(outcome.compiled);
  backend::shaderCacheUsage(&entries, &bytes);
  EXPECT_EQ(entries, 1u) << "the real compile is what populates the cache";
}

TEST(ShaderCache, TheBudgetEvictsTheLeastRecentlyUsedEntries) {
  if (!backend::isShaderCompilerAvailable()) GTEST_SKIP() << "this build has no GLSL compiler";
  const CacheSettingsGuard guard;
  const TemporaryDirectory directory;
  backend::setShaderCacheDirectory(directory.path());
  backend::setShaderCacheSalt("test-lru");

  // Enough distinct shaders to overflow the budget below, and a budget of three entries' worth.
  constexpr int kShaders = 12;
  std::vector<std::string> sources;
  sources.reserve(kShaders);
  for (int index = 0; index < kShaders; ++index) {
    sources.push_back(fragment(("o = vec4(" + std::to_string(index) + ".0, 0.0, 0.0, 1.0);").c_str()));
  }
  ASSERT_TRUE(compile(sources[0], backend::ShaderStage::Fragment).success);

  uint64_t entriesBefore = 0;
  uint64_t bytesBefore = 0;
  backend::shaderCacheUsage(&entriesBefore, &bytesBefore);
  ASSERT_EQ(entriesBefore, 1u);
  ASSERT_GT(bytesBefore, 0u);

  // Three entries' worth: the pass runs on the first write of a process, so lower the budget first and let the
  // writes themselves trigger it.
  backend::setShaderCacheBudget(bytesBefore * 3);

  for (int index = 1; index < kShaders; ++index) {
    ASSERT_TRUE(compile(sources[index], backend::ShaderStage::Fragment).success);
  }

  uint64_t entriesAfter = 0;
  uint64_t bytesAfter = 0;
  backend::shaderCacheUsage(&entriesAfter, &bytesAfter);
  EXPECT_LT(entriesAfter, static_cast<uint64_t>(kShaders)) << "the cache must have evicted something";
  EXPECT_LE(bytesAfter, bytesBefore * 3) << "the directory must fit its budget";

  // The most recently used shader is still served from disk; the very first one was evicted and recompiles.
  const CompileOutcome recent = compile(sources[kShaders - 1], backend::ShaderStage::Fragment);
  ASSERT_TRUE(recent.success);
  EXPECT_FALSE(recent.compiled) << "the newest entry must survive the pass";

  const CompileOutcome oldest = compile(sources[0], backend::ShaderStage::Fragment);
  ASSERT_TRUE(oldest.success);
  EXPECT_TRUE(oldest.compiled) << "the oldest entry must have been evicted";
}

// ==================================================================================================
// The watch: turning "the counters look odd" into a diagnostic
//
// Both defects the watch exists for were found by reading these numbers by hand - a compile path that never wrote,
// and a key that might drift between processes. It is a state machine over starts, so it is tested as one, without
// touching the filesystem.
// ==================================================================================================

namespace {

coral::detail::ShaderCacheSample watchSample(const uint64_t hits, const uint64_t misses, const uint64_t entries,
                                             const uint64_t bytes, const uint64_t budget = 8u * 1024u * 1024u) {
  coral::detail::ShaderCacheSample sample;
  sample.directorySet = true;
  sample.hits = hits;
  sample.misses = misses;
  sample.entries = entries;
  sample.bytes = bytes;
  sample.budget = budget;
  return sample;
}

} // namespace

TEST(ShaderCacheWatch, WritesThatNeverLandAreReportedAfterAFewStarts) {
  coral::detail::ShaderCacheWatch watch;
  // The shape of the OpenGL defect: every start compiles, nothing is ever stored.
  EXPECT_TRUE(watch.record(watchSample(0, 2, 0, 0)).empty());
  EXPECT_TRUE(watch.record(watchSample(0, 4, 0, 0)).empty());
  const std::string warning = watch.record(watchSample(0, 6, 0, 0));
  EXPECT_NE(warning.find("writes are not landing"), std::string::npos) << warning;
  // Once is enough: a warning every start becomes noise that hides the next problem.
  EXPECT_TRUE(watch.record(watchSample(0, 8, 0, 0)).empty());
}

TEST(ShaderCacheWatch, AKeyThatMovesBetweenProcessesIsReportedAfterTenStarts) {
  coral::detail::ShaderCacheWatch watch;
  for (uint32_t start = 1; start <= coral::detail::ShaderCacheWatch::kStartsBeforeUnstable; ++start) {
    const std::string warning = watch.record(watchSample(0, 1, start, start * 512u));
    if (start < coral::detail::ShaderCacheWatch::kStartsBeforeUnstable) {
      EXPECT_TRUE(warning.empty()) << "start " << start << ": " << warning;
    } else {
      EXPECT_NE(warning.find("not stable across processes"), std::string::npos) << warning;
    }
  }
}

TEST(ShaderCacheWatch, HitsKeepItQuiet) {
  coral::detail::ShaderCacheWatch watch;
  for (int start = 0; start < 20; ++start) {
    EXPECT_TRUE(watch.record(watchSample(4, 0, 2, 1024)).empty());
  }
  EXPECT_EQ(watch.consecutiveMisses(), 0u);
}

TEST(ShaderCacheWatch, AStartThatCompilesNothingIsNotEvidenceOfABug) {
  coral::detail::ShaderCacheWatch watch;
  // A fully warm cache compiles nothing at all, which looks like "no activity" rather than "no writes".
  for (int start = 0; start < 20; ++start) {
    EXPECT_TRUE(watch.record(watchSample(0, 0, 2, 1024)).empty());
  }
  EXPECT_EQ(watch.consecutiveEmptyStarts(), 0u) << "an idle start must not accumulate";
}

TEST(ShaderCacheWatch, WithoutADirectoryThereIsNothingToReport) {
  coral::detail::ShaderCacheWatch watch;
  coral::detail::ShaderCacheSample sample = watchSample(0, 5, 0, 0);
  sample.directorySet = false;
  for (int start = 0; start < 20; ++start) {
    EXPECT_TRUE(watch.record(sample).empty());
  }
  EXPECT_EQ(watch.consecutiveEmptyStarts(), 0u);
}

TEST(ShaderCacheWatch, ExceedingTheBudgetIsReportedOnce) {
  coral::detail::ShaderCacheWatch watch;
  const std::string first = watch.record(watchSample(0, 1, 100, 9000, 1000));
  EXPECT_NE(first.find("trimmer is not keeping up"), std::string::npos) << first;
  EXPECT_TRUE(watch.record(watchSample(0, 2, 100, 9000, 1000)).empty());
}
