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
 * @file ThreadSampler.cpp
 *
 * See ThreadSampler.h. The sampling loop suspends one thread at a time, copies its raw return addresses,
 * and resumes it; symbol resolution happens afterwards on the sampler thread, because dbghelp allocates and
 * holding another thread suspended while it does is how a profiler deadlocks its subject.
 */

#include "ThreadSampler.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#endif

namespace coral::test {

#if !defined(_WIN32)

void ThreadSampler::start(std::chrono::milliseconds) {}

void ThreadSampler::report() const {}

ThreadSampler::~ThreadSampler() = default;

#else

namespace {

constexpr size_t kMaxFrames = 48;

/** Resolves @p address to "dll!function+0xN" using the process-wide dbghelp session. */
std::string symbolise(const DWORD64 address) {
  alignas(SYMBOL_INFO) unsigned char buffer[sizeof(SYMBOL_INFO) + 256 * sizeof(char)] = {};
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = 255;

  IMAGEHLP_MODULE64 module = {};
  module.SizeOfStruct = sizeof(module);
  const bool haveModule = SymGetModuleInfo64(GetCurrentProcess(), address, &module) != FALSE;

  DWORD64 displacement = 0;
  if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol) != FALSE) {
    char text[512];
    std::snprintf(text, sizeof(text), "%s!%s+0x%llx", haveModule ? module.ModuleName : "?",
                  symbol->Name, static_cast<unsigned long long>(displacement));
    return text;
  }
  if (haveModule) {
    char text[256];
    std::snprintf(text, sizeof(text), "%s+0x%llx", module.ModuleName,
                  static_cast<unsigned long long>(address - module.BaseOfImage));
    return text;
  }
  char text[64];
  std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
  return text;
}

/** @return true when @p address belongs to a module whose name contains the test's or engine's own code. */
bool isOwnCode(const DWORD64 address) {
  IMAGEHLP_MODULE64 module = {};
  module.SizeOfStruct = sizeof(module);
  if (SymGetModuleInfo64(GetCurrentProcess(), address, &module) == FALSE) return false;
  const std::string name = module.ModuleName != nullptr ? module.ModuleName : "";
  return name.find("effect") != std::string::npos || name.find("test_effect") != std::string::npos ||
         name.find("demo") != std::string::npos;
}

std::vector<DWORD> processThreadIds() {
  std::vector<DWORD> ids;
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return ids;
  THREADENTRY32 entry = {};
  entry.dwSize = sizeof(entry);
  const DWORD self = GetCurrentProcessId();
  if (Thread32First(snapshot, &entry) != FALSE) {
    do {
      if (entry.th32OwnerProcessID == self) ids.push_back(entry.th32ThreadID);
    } while (Thread32Next(snapshot, &entry) != FALSE);
  }
  CloseHandle(snapshot);
  return ids;
}

} // namespace

struct ThreadSampler::Impl {
  std::chrono::milliseconds interval { 1 };
  std::atomic<bool> running { false };
  std::thread worker;

  /** One entry per sample: the address of the frame inside our own code, and its raw stack. */
  struct Sample {
    DWORD thread = 0;
    DWORD64 leaf = 0;
    DWORD64 leafOwn = 0;
  };

  std::atomic<int> ticks { 0 };
  std::mutex mutex;
  std::vector<Sample> samples;

  void sampleThread(const DWORD threadId) {
    const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                     threadId);
    if (thread == nullptr) return;

    if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
      CloseHandle(thread);
      return;
    }

    CONTEXT context = {};
    context.ContextFlags = CONTEXT_FULL;
    DWORD64 addresses[kMaxFrames] = {};
    USHORT count = 0;
    if (GetThreadContext(thread, &context) != FALSE) {
      STACKFRAME64 frame = {};
#if defined(_M_X64) || defined(__x86_64__)
      const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
      frame.AddrPC.Offset = context.Rip;
      frame.AddrPC.Mode = AddrModeFlat;
      frame.AddrFrame.Offset = context.Rbp;
      frame.AddrFrame.Mode = AddrModeFlat;
      frame.AddrStack.Offset = context.Rsp;
      frame.AddrStack.Mode = AddrModeFlat;
#else
      const DWORD machine = IMAGE_FILE_MACHINE_I386;
      frame.AddrPC.Offset = context.Eip;
      frame.AddrPC.Mode = AddrModeFlat;
      frame.AddrFrame.Offset = context.Ebp;
      frame.AddrFrame.Mode = AddrModeFlat;
      frame.AddrStack.Offset = context.Esp;
      frame.AddrStack.Mode = AddrModeFlat;
#endif
      while (count < kMaxFrames) {
        if (StackWalk64(machine, GetCurrentProcess(), thread, &frame, &context, nullptr,
                        SymFunctionTableAccess64, SymGetModuleBase64, nullptr) == FALSE) {
          break;
        }
        if (frame.AddrPC.Offset == 0) break;
        addresses[count++] = frame.AddrPC.Offset;
      }
    }
    ResumeThread(thread);
    CloseHandle(thread);

    DWORD64 leaf = count > 0 ? addresses[0] : 0;
    DWORD64 leafOwn = 0;
    for (USHORT index = 0; index < count; ++index) {
      if (isOwnCode(addresses[index])) {
        leafOwn = addresses[index];
        break;
      }
    }
    const std::lock_guard<std::mutex> lock(mutex);
    samples.push_back(Sample { threadId, leaf, leafOwn });
  }

  void run() {
    while (running.load(std::memory_order_acquire)) {
      for (const DWORD thread : processThreadIds()) {
        if (!running.load(std::memory_order_acquire)) break;
        if (thread == GetCurrentThreadId()) continue;
        sampleThread(thread);
      }
      ticks.fetch_add(1);
      std::this_thread::sleep_for(interval);
    }
  }
};

void ThreadSampler::start(const std::chrono::milliseconds interval) {
  if (mImpl != nullptr) return;
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
  SymInitialize(GetCurrentProcess(), nullptr, TRUE);

  mImpl = std::make_shared<Impl>();
  mImpl->interval = interval;
  mImpl->running.store(true, std::memory_order_release);
  Impl* const impl = mImpl.get();
  mImpl->worker = std::thread([impl] { impl->run(); });
}

void ThreadSampler::report() const {
  if (mImpl == nullptr) return;
  std::vector<Impl::Sample> samples;
  {
    const std::lock_guard<std::mutex> lock(mImpl->mutex);
    samples = mImpl->samples;
  }
  if (samples.empty()) {
    std::fprintf(stderr, "[sample] no samples\n");
    return;
  }

  // Group by the first frame in our own code: the leaf is usually inside ntdll or the driver, which says
  // "waiting" but not "in which of our functions".
  std::map<std::pair<DWORD, DWORD64>, int> byOwn;
  std::map<std::pair<DWORD, DWORD64>, int> byLeaf;
  for (const Impl::Sample& sample : samples) {
    if (sample.leafOwn != 0) ++byOwn[{ sample.thread, sample.leafOwn }];
    if (sample.leaf != 0) ++byLeaf[{ sample.thread, sample.leaf }];
  }

  // Per thread, because a percentage over the whole process says nothing: the threads that matter are the
  // caller blocked in `execute` and the render thread doing the work.
  std::map<DWORD, int> threadTotals;
  for (const Impl::Sample& sample : samples) ++threadTotals[sample.thread];

  std::vector<std::pair<int, DWORD>> busiest;
  busiest.reserve(threadTotals.size());
  for (const auto& entry : threadTotals) busiest.emplace_back(entry.second, entry.first);
  std::sort(busiest.begin(), busiest.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

  std::fprintf(stderr, "[sample] %zu samples over %zu threads\n", samples.size(), threadTotals.size());
  // Rank by *own-code* samples: a thread that is idle in ntdll is not interesting, and the round-robin
  // sampling gives every thread the same number of samples regardless of whether it ever runs.
  std::vector<std::pair<int, DWORD>> byOwnCount;
  for (const Impl::Sample& sample : samples) {
    if (sample.leafOwn != 0) ++threadTotals[sample.thread];
  }
  for (const auto& entry : threadTotals) byOwnCount.emplace_back(entry.second, entry.first);
  for (size_t threadIndex = 0; threadIndex < byOwnCount.size() && threadIndex < 4; ++threadIndex) {
    const DWORD thread = byOwnCount[threadIndex].second;
    const int total = byOwnCount[threadIndex].first;
    std::map<DWORD64, int> counts;
    for (const Impl::Sample& sample : samples) {
      if (sample.thread != thread || sample.leafOwn == 0) continue;
      ++counts[sample.leafOwn];
    }
    if (counts.empty()) continue;
    const int ownTotal = total;
    std::vector<std::pair<int, DWORD64>> ranked;
    ranked.reserve(counts.size());
    for (const auto& entry : counts) ranked.emplace_back(entry.second, entry.first);
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::fprintf(stderr, "[sample] tid=%5lu  %d own-code samples\n", thread, ownTotal);
    for (size_t index = 0; index < ranked.size() && index < 5; ++index) {
      std::fprintf(stderr, "[sample]   %5.1f%%  %s\n",
                   100.0 * ranked[index].first / static_cast<double>(ownTotal),
                   symbolise(ranked[index].second).c_str());
    }
  }
  (void) byOwn;
  (void) byLeaf;
}

ThreadSampler::~ThreadSampler() {
  if (mImpl == nullptr) return;
  mImpl->running.store(false, std::memory_order_release);
  if (mImpl->worker.joinable()) mImpl->worker.join();
}

#endif // _WIN32

} // namespace coral::test




