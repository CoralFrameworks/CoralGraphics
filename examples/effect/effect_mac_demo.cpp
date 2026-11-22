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
 * @file effect_mac_demo.cpp
 *
 * The macOS verification for the effect render graph.
 *
 * ## Why this exists
 *
 * `effect_demo` is the minimal client: it builds one graph, renders one frame and prints the centre pixel. It says the
 * graph ran; it does not say the picture is right, and on a platform with more than one backend it does not say which
 * one produced it. This program is the verification instead:
 *
 *  - it runs the same deterministic graph (`TestPatternInputNode` -> copy pass -> readback) on every backend this
 *    machine can bring up, so a newly enabled backend is exercised rather than silently untested;
 *  - it checks the eight colour bars at their centres, so a blank frame, a wrong sampling path or a wrong channel order
 *    fails with the bar and the pixel named;
 *  - it compares the backends' frames against each other, so "Metal renders, OpenGL renders, they disagree" is caught
 *    here rather than in a screenshot review.
 *
 * It is the macOS counterpart of the other platforms' offscreen `--verify` modes, and it is registered with CTest both as
 * a whole-machine check and as one row per backend.
 *
 * ## Usage
 *
 * ```sh
 * effect_mac_demo                    # Metal and OpenGL, both verified
 * effect_mac_demo --backend metal    # pinned; falling back to another backend is a failure
 * effect_mac_demo --backend opengl
 * ```
 *
 * Vulkan is not an option on macOS: the backend is not compiled there (see the root CMakeLists), so the program does
 * not attempt it and does not report it as unavailable.
 *
 * Exit codes: 0 = every check passed, 1 = a check failed, 77 = no backend could be brought up (which CTest reports as
 * a skip).
 *
 * The checks need no window: everything renders offscreen and reads back. That is deliberate -- a window would make the
 * program unusable on a build machine, which is exactly where a backend regression needs to be caught.
 */

#include "EffectVerification.h"

#include <backend/Device.h>
#include <effect/EffectTypes.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kNoDeviceExitCode = 77;

coral::EffectBackend parseBackend(const char* name, bool* recognised) {
  *recognised = true;
  if (std::strcmp(name, "auto") == 0 || std::strcmp(name, "default") == 0) return coral::EffectBackend::Default;
  if (std::strcmp(name, "metal") == 0) return coral::EffectBackend::Metal;
  if (std::strcmp(name, "opengl") == 0 || std::strcmp(name, "gl") == 0) return coral::EffectBackend::OpenGL;
  if (std::strcmp(name, "vulkan") == 0 || std::strcmp(name, "vk") == 0) return coral::EffectBackend::Vulkan;
  *recognised = false;
  return coral::EffectBackend::Default;
}

/** The RHI backend an `EffectBackend` names, for availability and for the fallback check. */
backend::Backend rhiBackend(const coral::EffectBackend backend) {
  switch (backend) {
    case coral::EffectBackend::Default:
      return backend::Backend::Null;
    case coral::EffectBackend::Metal:
      return backend::Backend::Metal;
    case coral::EffectBackend::OpenGL:
      return backend::Backend::OpenGL;
    case coral::EffectBackend::Vulkan:
      return backend::Backend::Vulkan;
  }
  return backend::Backend::Null;
}

struct Outcome {
  bool verified = false;
  bool unavailable = false;
  bool failed = false;
};

/**
 * Runs and verifies one backend.
 *
 * @param requested       the backend to pin; `Default` is not used here.
 * @param reference       the first successful run, used for the cross-backend comparison; updated when empty.
 * @param referenceName   what the reference is called in the output.
 */
Outcome verifyBackend(const coral::EffectBackend requested, effect_verification::Run* reference,
        std::string* referenceName) {
  Outcome outcome;
  const char* name = effect_verification::backendName(rhiBackend(requested));

  const effect_verification::Run run = effect_verification::runCopyGraph(requested);
  if (run.deviceUnavailable) {
    std::printf("%s: unavailable (%s)\n", name, run.error.c_str());
    outcome.unavailable = true;
    return outcome;
  }
  if (!run.ok) {
    std::printf("%s: FAILED to render: %s\n", name, run.error.c_str());
    outcome.failed = true;
    return outcome;
  }

  std::printf("%s: %s\n", name, run.deviceLine.c_str());
  if (run.backend != rhiBackend(requested)) {
    // The engine is allowed to fall back in general, so this is the check that a *pinned* request was honoured.
    std::printf("%s: FAILED: requested %s but the engine came up on %s\n", name, name,
            effect_verification::backendName(run.backend));
    outcome.failed = true;
    return outcome;
  }

  std::string error;
  if (!effect_verification::verifyColorBars(run, error)) {
    std::printf("%s: FAILED: colour bars: %s\n", name, error.c_str());
    outcome.failed = true;
    return outcome;
  }
  std::printf("%s: colour bars ok (8 of 8)\n", name);

  if (reference->ok) {
    if (!effect_verification::verifySameImage(*reference, run, error)) {
      std::printf("%s: FAILED: differs from %s: %s\n", name, referenceName->c_str(), error.c_str());
      outcome.failed = true;
      return outcome;
    }
    std::printf("%s: matches %s\n", name, referenceName->c_str());
  } else {
    // The first successful backend is the reference every later one is compared against. On macOS that is Metal,
    // because the list below is in platform preference order.
    *reference = run;
    *referenceName = name;
    std::printf("%s: this run is the reference for the comparison\n", name);
  }

  outcome.verified = true;
  return outcome;
}

void printUsage() {
  std::printf("usage: effect_mac_demo [--backend auto|metal|opengl]\n"
              "       without --backend, Metal and OpenGL are both verified\n");
}

} // namespace anonymous

int main(int argc, char** argv) {
  bool allBackends = true;
  coral::EffectBackend requested = coral::EffectBackend::Default;

  for (int index = 1; index < argc; ++index) {
    const char* argument = argv[index];
    if (std::strcmp(argument, "--help") == 0 || std::strcmp(argument, "-h") == 0) {
      printUsage();
      return 0;
    }
    if (std::strcmp(argument, "--backend") == 0 || std::strcmp(argument, "-b") == 0) {
      if (index + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", argument);
        printUsage();
        return 1;
      }
      if (std::strcmp(argv[index + 1], "vulkan") == 0 || std::strcmp(argv[index + 1], "vk") == 0) {
        // Named rather than "unknown": a caller asking for Vulkan on macOS is asking for a backend this platform does
        // not build, and the answer should say so instead of suggesting a typo.
        std::fprintf(stderr,
                "the Vulkan backend is not offered on Apple platforms; use --backend metal or --backend opengl\n");
        return 1;
      }
      bool recognised = false;
      const coral::EffectBackend parsed = parseBackend(argv[++index], &recognised);
      if (!recognised) {
        std::fprintf(stderr, "unknown backend '%s'\n", argv[index]);
        printUsage();
        return 1;
      }
      if (parsed == coral::EffectBackend::Default) {
        // `--backend auto` keeps the platform preference but still verifies every available backend, because this
        // program's job is verification rather than "pick one".
        allBackends = true;
        continue;
      }
      requested = parsed;
      allBackends = false;
      continue;
    }
    std::fprintf(stderr, "unknown argument '%s'\n", argument);
    printUsage();
    return 1;
  }

  std::printf("effect mac verification\n");
  std::vector<coral::EffectBackend> backends;
  if (allBackends) {
    // Platform preference order. Both Apple backends are compiled in on macOS; a backend whose device cannot be
    // created is reported as unavailable rather than failing the run, which is what keeps the check usable on a machine
    // with no GPU. Vulkan is not in the list because it is not built on Apple platforms.
    for (const coral::EffectBackend candidate: { coral::EffectBackend::Metal, coral::EffectBackend::OpenGL }) {
      if (backend::isBackendAvailable(rhiBackend(candidate))) backends.push_back(candidate);
    }
    if (backends.empty()) {
      std::fprintf(stderr, "this build has no graphics backend compiled in\n");
      return kNoDeviceExitCode;
    }
  } else {
    if (!backend::isBackendAvailable(rhiBackend(requested))) {
      std::fprintf(stderr, "%s is not compiled into this build\n", effect_verification::backendName(rhiBackend(requested)));
      return kNoDeviceExitCode;
    }
    backends.push_back(requested);
  }

  effect_verification::Run reference;
  std::string referenceName;
  uint32_t verified = 0;
  uint32_t unavailable = 0;
  uint32_t failed = 0;

  for (const coral::EffectBackend candidate: backends) {
    const Outcome outcome = verifyBackend(candidate, &reference, &referenceName);
    verified += outcome.verified ? 1u : 0u;
    unavailable += outcome.unavailable ? 1u : 0u;
    failed += outcome.failed ? 1u : 0u;
  }

  std::printf("\nverified: %u, unavailable: %u, failed: %u\n", verified, unavailable, failed);
  if (failed != 0) return 1;
  if (verified == 0) {
    // Every backend was unavailable. That is the "no GPU here" answer, and 77 is how CTest records a skip.
    return kNoDeviceExitCode;
  }
  return 0;
}
