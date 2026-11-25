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

#ifndef CORAL_GRAPHICS_DECODELIMITS_H
#define CORAL_GRAPHICS_DECODELIMITS_H

#include <cstddef>

namespace graphics {

/**
 * Bounds a decoder applies before it walks untrusted input.
 *
 * A parser that trusts a length field is how a thumbnail turns into a multi-gigabyte allocation; the
 * limits are the one place that decision is made, rather than each format deciding for itself. They are
 * process-wide so an embedded product can tighten them for its own threat model without every call site
 * carrying a parameter, and the defaults are the values a desktop product can live with.
 */
struct DecodeLimits {
  /** Encoded input bytes; a document or image larger than this is refused before parsing. */
  std::size_t maxBytes = 64u * 1024u * 1024u;
  /** Decoded pixels for an image (width times height), checked before the allocation. */
  std::size_t maxPixels = 16384u * 16384u;
  /**
   * Nesting depth for a document tree.
   *
   * A recursive parser turns a document that nests a thousand elements into a thousand stack frames; the
   * depth cap is what makes a hostile SVG or Lottie file fail on its shape rather than on the thread's
   * stack. 256 is far deeper than any real document and far shallower than the stack.
   */
  std::size_t maxDepth = 256;
  /** Nodes in a parsed document tree; 0 means no limit. */
  std::size_t maxNodes = 1000000;
};

/** The limits in effect for this process. */
[[nodiscard]] DecodeLimits decodeLimits() noexcept;

/**
 * Replaces the limits for this process.
 *
 * A zero field means "no limit for this dimension"; a product that accepts user images should keep both
 * non-zero. Call it during start-up: a decode already running reads the limits once when it starts.
 */
void setDecodeLimits(const DecodeLimits& limits) noexcept;

} // namespace graphics

#endif // CORAL_GRAPHICS_DECODELIMITS_H
