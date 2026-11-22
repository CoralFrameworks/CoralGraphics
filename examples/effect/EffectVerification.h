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

#ifndef CORAL_EXAMPLES_EFFECT_VERIFICATION_H
#define CORAL_EXAMPLES_EFFECT_VERIFICATION_H

/**
 * @file EffectVerification.h
 *
 * The effect graph the macOS verification programs run, and the checks they apply to its output.
 *
 * This is example code, not framework code: it lives here so that a demo and its checks share one definition of "the
 * graph" instead of each spelling out the same five nodes. The graph is deliberately the smallest one that touches
 * every layer a client uses - input node, pass node with a shader, output node, frame pool, readback - because a
 * verification that cannot build that graph has nothing to say about a larger one.
 *
 * The pattern is the deterministic `TestPatternInputNode::Pattern::ColorBars`: eight vertical SMPTE-style bars, which
 * is a picture whose correct answer is a table of colours rather than "some pixels changed". A copy pass must reproduce
 * it exactly at the centre of each bar, and two backends running the same graph must produce the same image.
 */

#include <backend/Device.h>
#include <effect/EffectEngine.h>
#include <effect/EffectTypes.h>

#include <cstdint>
#include <string>
#include <vector>

namespace effect_verification {

/** One run of the copy graph on one backend. */
struct Run {
  /** True when the graph rendered and the frame came back. */
  bool ok = false;
  /**
   * True when no device could be created for the requested backend. Distinct from a failure: a machine without the
   * backend should skip the check, while a requested backend that comes up and then produces the wrong pixels should
   * fail it.
   */
  bool deviceUnavailable = false;
  /** Why the run failed, or why the device was unavailable. Empty on success. */
  std::string error;

  /** The backend the engine actually came up on, which is not necessarily the one requested (fallback is enabled). */
  backend::Backend backend = backend::Backend::Null;
  /** One line naming the device, for the program's output and for a bug report. */
  std::string deviceLine;

  uint32_t width = 0;
  uint32_t height = 0;
  /** RGBA8 pixels of the frame the output node read back. */
  std::vector<uint8_t> pixels;
};

/**
 * Builds input -> copy pass -> readback, renders one frame and returns the pixels.
 *
 * @param requested `EffectBackend::Default` lets the platform choose; the others pin the backend and are checked.
 */
[[nodiscard]] Run runCopyGraph(coral::EffectBackend requested);

/**
 * Checks the eight bars of the test pattern at their centres.
 *
 * The tolerance is two units per channel: a bar's centre is a texel boundary away from its neighbours, and a linear
 * sampler at exactly that texel must still return the bar's own colour, but a backend that rounds the conversion
 * slightly differently is not the defect this check is looking for.
 *
 * @param run   a successful run.
 * @param error receives a description naming the bar and the pixel when a check fails.
 */
[[nodiscard]] bool verifyColorBars(const Run& run, std::string& error);

/**
 * Checks that two runs produced the same image within @p tolerance per channel.
 *
 * This is the cross-backend check: the same graph on Metal and OpenGL has to agree, because the engine's contract is
 * that a backend is a choice and not a different picture. A mismatch reports the pixel index and both values so it can
 * be traced to a pass rather than to "the image looks different".
 */
[[nodiscard]] bool verifySameImage(const Run& expected, const Run& actual, std::string& error, uint32_t tolerance = 2);

/** @return the RHI backend name for @p backend ("Metal", "OpenGL", ...). */
[[nodiscard]] const char* backendName(backend::Backend backend) noexcept;

} // namespace effect_verification

#endif // CORAL_EXAMPLES_EFFECT_VERIFICATION_H
