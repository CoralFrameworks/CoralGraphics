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

/**
 * @file DecodeLimits.cpp
 *
 * The process-wide decode bounds. They are two relaxed atomics: a decoder reads them once at entry, and
 * the only writer is the application's start-up code, so a torn pair would mean a decode that used the
 * old byte limit with the new pixel limit -- harmless, and cheaper than a lock on a hot path.
 */

#include <graphics/DecodeLimits.h>

#include <atomic>

namespace graphics {

namespace {

std::atomic<std::size_t> gMaxBytes{ 64u * 1024u * 1024u };
std::atomic<std::size_t> gMaxPixels{ 16384u * 16384u };
std::atomic<std::size_t> gMaxDepth{ 256 };
std::atomic<std::size_t> gMaxNodes{ 1000000 };

} // namespace

DecodeLimits decodeLimits() noexcept {
  DecodeLimits limits;
  limits.maxBytes = gMaxBytes.load(std::memory_order_relaxed);
  limits.maxPixels = gMaxPixels.load(std::memory_order_relaxed);
  limits.maxDepth = gMaxDepth.load(std::memory_order_relaxed);
  limits.maxNodes = gMaxNodes.load(std::memory_order_relaxed);
  return limits;
}

void setDecodeLimits(const DecodeLimits& limits) noexcept {
  gMaxBytes.store(limits.maxBytes, std::memory_order_relaxed);
  gMaxPixels.store(limits.maxPixels, std::memory_order_relaxed);
  gMaxDepth.store(limits.maxDepth, std::memory_order_relaxed);
  gMaxNodes.store(limits.maxNodes, std::memory_order_relaxed);
}

} // namespace graphics
