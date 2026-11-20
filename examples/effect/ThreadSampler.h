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

#ifndef CORAL_TESTS_THREAD_SAMPLER_H
#define CORAL_TESTS_THREAD_SAMPLER_H

#include <chrono>
#include <memory>

/**
 * @file ThreadSampler.h
 *
 * A user-mode sampling profiler for the examples.
 *
 * ETW captures (`wpr`, `xperf`) need an elevated kernel session, which CI and an ordinary developer shell
 * do not have. This samples the process's own threads instead: suspend, read the context, walk the stack,
 * resume, and resolve the addresses afterwards. It is crude - a sample every millisecond, one thread at a
 * time - but it answers "which code is the render thread sitting in", which is exactly the question a
 * frame-time investigation asks.
 *
 * Windows only; elsewhere `start()` does nothing and `report()` prints nothing.
 */
namespace coral::test {

class ThreadSampler {
public:
  /** Starts sampling every thread of this process. @p interval how long to sleep between samples. */
  void start(std::chrono::milliseconds interval = std::chrono::milliseconds(1));

  /** Stops sampling and prints the hottest stacks, grouped by leaf frame. */
  void report() const;

  ~ThreadSampler();

  ThreadSampler() = default;
  ThreadSampler(const ThreadSampler&) = delete;
  ThreadSampler& operator=(const ThreadSampler&) = delete;

private:
  struct Impl;
  std::shared_ptr<Impl> mImpl;
};

} // namespace coral::test

#endif // CORAL_TESTS_THREAD_SAMPLER_H

