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

// A two-input transition, run the way a player would run one.
//
// The interesting part is not `mix`; it is what happens before the mix has anything to say. Video B is preloaded -
// the node is in the graph and its decoder is open - while the timeline is still playing A, so B has never produced
// a frame. The engine cannot tell that apart from a stalled or broken input (both look like "produce returned false
// with no error"), which is why the graph fails the frame until the input has produced something at least once.
//
// So the demo does what the application is supposed to do: it submits one placeholder frame to B while preloading,
// and from then on `repeatLastFrame()` keeps B's input satisfied with that single frame. At progress 0 the
// transition shader never shows B, so the placeholder is invisible.
//
// Three phases, and the frame counter decides which one is running:
//
//   preload     A plays; B holds its placeholder; progress = 0
//   transition  both play; progress ramps 0 -> 1
//   hold        B plays; A holds its last frame; progress = 1
//
// `--no-placeholder` skips the placeholder, which is how the demo shows the failure it exists to avoid: the first
// frame comes back Busy because B has no frame and nothing to repeat.
//
// The frames are generated on the main thread, just before `execute()`. A real decoder submits from its own thread
// - `VideoFrameInputNode::submit` is built for that and copies under its own mutex - but a demo that prints numbers
// has to be reproducible, and the thread is not what is being demonstrated here.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <effect/EffectEngine.h>
#include <effect/EffectPassNode.h>
#include <effect/NullOutputNode.h>
#include <effect/VideoFrameInputNode.h>

#if defined(_WIN32)
#include <effect/platform/win/WindowsSwapChainOutputNode.h>
#include <windows.h>
#endif

namespace {

constexpr uint32_t kWidth = 640;
constexpr uint32_t kHeight = 360;

#if defined(_WIN32)
constexpr wchar_t kWindowClass[] = L"coral-transition-demo";

HWND createWindow() {
  WNDCLASSEXW windowClass {};
  windowClass.cbSize = sizeof(windowClass);
  windowClass.style = CS_HREDRAW | CS_VREDRAW;
  windowClass.lpfnWndProc = DefWindowProcW;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
  windowClass.lpszClassName = kWindowClass;
  (void) RegisterClassExW(&windowClass);
  RECT rect { 0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight) };
  (void) AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  return CreateWindowExW(0, kWindowClass, L"coral effect engine - preload, transition, hold",
                         WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                         rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                         GetModuleHandleW(nullptr), nullptr);
}
#endif

/** A stand-in for a decoder: an animated pattern that makes it obvious which source is on screen. */
class PatternSource final {
public:
  explicit PatternSource(const bool warm) : mWarm(warm) {
    mPixels.resize(static_cast<size_t>(kWidth) * kHeight * 4);
  }

  /** @return the frame for @p index, satisfying the node's contract that it copies before returning. */
  void render(const uint32_t index) {
    // Two clearly different pictures: A is a scrolling warm gradient with a bright bar, B is a cool checkerboard
    // with a moving block. A viewer can tell which one a frame came from without a label.
    for (uint32_t y = 0; y < kHeight; ++y) {
      const float v = static_cast<float>(y) / static_cast<float>(kHeight - 1);
      for (uint32_t x = 0; x < kWidth; ++x) {
        const float u = static_cast<float>(x) / static_cast<float>(kWidth - 1);
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        if (mWarm) {
          const float scroll = std::fmod(u * 2.0f + static_cast<float>(index % 120) / 120.0f, 1.0f);
          r = 0.35f + 0.6f * scroll;
          g = 0.25f + 0.5f * (1.0f - v);
          b = 0.15f + 0.3f * (1.0f - scroll);
          const float bar = static_cast<float>(index % 120) / 120.0f;
          if (std::fabs(v - bar) < 0.02f) {
            r = 1.0f;
            g = 1.0f;
            b = 1.0f;
          }
          if (u > 0.9f) {  // a corner marker, so a frozen frame is recognisable
            r = 1.0f;
            g = 0.9f;
            b = 0.2f;
          }
        } else {
          const float checker =
                  (static_cast<int>(u * 12.0f + static_cast<float>(index) / 30.0f) +
                   static_cast<int>(v * 8.0f)) % 2 == 0
                          ? 1.0f
                          : 0.35f;
          r = 0.15f * checker;
          g = 0.45f * checker + 0.2f * v;
          b = 0.75f * checker + 0.2f * (1.0f - v);
          const float block = 0.25f + 0.5f * static_cast<float>(index % 90) / 90.0f;
          if (u > block - 0.08f && u < block + 0.08f && v > 0.42f && v < 0.58f) {
            r = 0.95f;
            g = 0.95f;
            b = 1.0f;
          }
        }
        const size_t offset = (static_cast<size_t>(y) * kWidth + x) * 4;
        mPixels[offset + 0] = static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
        mPixels[offset + 1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
        mPixels[offset + 2] = static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
        mPixels[offset + 3] = 255;
      }
    }
  }

  /** @return a description of the most recent frame. The pixels stay valid until the next `render`. */
  coral::VideoFrameDesc frame() const {
    coral::VideoFrameDesc desc;
    desc.width = kWidth;
    desc.height = kHeight;
    desc.format = coral::EffectPixelFormat::RGBA8;
    desc.colorSpace = coral::EffectColorSpace::SRGB;
    desc.planes[0].data = mPixels.data();
    desc.planes[0].rowBytes = kWidth * 4;
    desc.planeCount = 1;
    return desc;
  }

private:
  bool mWarm = true;
  std::vector<uint8_t> mPixels;
};

struct Options {
  bool hidden = false;
  bool nullOutput = false;
  bool placeholder = true;
  bool perFrame = false;
  uint32_t preloadFrames = 30;
  uint32_t transitionFrames = 45;
  uint32_t holdFrames = 30;
};

enum class Phase { Preload, Transition, Hold };

const char* phaseName(const Phase phase) {
  switch (phase) {
    case Phase::Preload:
      return "preload";
    case Phase::Transition:
      return "transition";
    case Phase::Hold:
      return "hold";
  }
  return "?";
}

} // namespace

int main(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    if (std::strcmp(argv[index], "--hidden") == 0) {
      options.hidden = true;
    } else if (std::strcmp(argv[index], "--null-output") == 0) {
      options.nullOutput = true;
    } else if (std::strcmp(argv[index], "--no-placeholder") == 0) {
      options.placeholder = false;
    } else if (std::strcmp(argv[index], "--per-frame") == 0) {
      options.perFrame = true;
    } else if (std::strcmp(argv[index], "--preload") == 0 && index + 1 < argc) {
      options.preloadFrames = static_cast<uint32_t>(std::strtoul(argv[++index], nullptr, 10));
    } else if (std::strcmp(argv[index], "--transition") == 0 && index + 1 < argc) {
      options.transitionFrames = static_cast<uint32_t>(std::strtoul(argv[++index], nullptr, 10));
    } else if (std::strcmp(argv[index], "--hold") == 0 && index + 1 < argc) {
      options.holdFrames = static_cast<uint32_t>(std::strtoul(argv[++index], nullptr, 10));
    } else {
      std::fprintf(stderr,
                   "usage: %s [--hidden] [--null-output] [--no-placeholder] [--per-frame] "
                   "[--preload N] [--transition N] [--hold N]\n",
                   argv[0]);
      return 2;
    }
  }

#if defined(_WIN32)
  HWND window = nullptr;
  if (!options.nullOutput) {
    window = createWindow();
    if (window == nullptr) {
      std::fprintf(stderr, "the demo window could not be created\n");
      return 1;
    }
    if (!options.hidden) {
      ShowWindow(window, SW_SHOW);
      UpdateWindow(window);
    }
  }
#else
  if (!options.nullOutput) {
    std::fprintf(stderr, "this demo draws into a Win32 window; use --null-output on other platforms\n");
    return 2;
  }
#endif

  coral::EffectEngineConfig config;
  config.renderWidth = kWidth;
  config.renderHeight = kHeight;
  config.renderMode = coral::EffectRenderMode::Manual;
  config.timeBase = 1000000000u;  // nanoseconds, the unit a decoder reports PTS in
  config.maxFramesInFlight = 2;

  auto engine = coral::EffectEngine::create(config);
  auto videoA = coral::VideoFrameInputNode::create(kWidth, kHeight, 4);
  auto videoB = coral::VideoFrameInputNode::create(kWidth, kHeight, 4);
  auto transition = engine != nullptr ? engine->createEffectNode("transition") : nullptr;
  std::shared_ptr<coral::EffectOutputNode> output;
  if (options.nullOutput) {
    auto nullOutput = coral::NullOutputNode::create();
    if (nullOutput != nullptr) output = nullOutput;
  }
#if defined(_WIN32)
  else if (window != nullptr) {
    auto windowOutput = coral::WindowsSwapChainOutputNode::create(window);
    if (windowOutput != nullptr) output = windowOutput;
  }
#endif
  if (engine == nullptr || videoA == nullptr || videoB == nullptr || transition == nullptr ||
      output == nullptr) {
    std::fprintf(stderr, "the transition graph could not be built\n");
    return 1;
  }
  // Named, because a failed frame reports which input had no frame and an anonymous node would not help.
  (void) videoA->setName("outgoing");
  (void) videoB->setName("incoming");

  coral::EffectShaderDesc shader;
  shader.name = "demo/transition";
  // uInput0 is the outgoing picture, uInput1 the incoming one. `progress` is an ordinary parameter: the engine has
  // no opinion about transition curves, and should not.
  shader.parameters = { coral::EffectParameterDesc { .name = "progress", .type = coral::EffectValueType::Float } };
  shader.fragmentSource =
          "  float t = effectParams(0).x;\n"
          "  vec4 outgoing = texture(uInput0, uv);\n"
          "  vec4 incoming = texture(uInput1, uv);\n"
          "  return mix(outgoing, incoming, clamp(t, 0.0, 1.0));\n";
  shader.inputCount = 2;
  if (transition->setShader(shader) != coral::EffectResult::Success) {
    std::fprintf(stderr, "the transition shader was refused: %s\n", transition->diagnostics().c_str());
    return 1;
  }
  // The frame's PTS follows the outgoing video; switching this at the end of the transition is the application's
  // call, not the engine's, so the demo states its choice here instead of hiding it.
  (void) transition->setTimestampSource(0);

  if (engine->attach(videoA) != coral::EffectResult::Success ||
      engine->attach(videoB) != coral::EffectResult::Success ||
      engine->attach(output) != coral::EffectResult::Success ||
      engine->connect(videoA, transition, 0) != coral::EffectResult::Success ||
      engine->connect(videoB, transition, 1) != coral::EffectResult::Success ||
      engine->connect(transition, output) != coral::EffectResult::Success ||
      engine->setOutput(output) != coral::EffectResult::Success) {
    std::fprintf(stderr, "the transition graph could not be connected: %s\n", engine->diagnostics().c_str());
    return 1;
  }

  // The placeholder. One frame, submitted once, before the timeline reaches B: from here on `repeatLastFrame()`
  // keeps B satisfied, and the engine never has to guess what "preloaded but not started" means.
  PatternSource sourceA(true);
  PatternSource sourceB(false);
  constexpr uint32_t kTimeBase = 1000000000u;  // nanoseconds, the unit a decoder reports PTS in
  int64_t timestamp = 0;
  if (options.placeholder) {
    std::vector<uint8_t> blackPixels(static_cast<size_t>(kWidth) * kHeight * 4, 0);
    for (size_t index = 3; index < blackPixels.size(); index += 4) blackPixels[index] = 255;
    coral::VideoFrameDesc placeholder;
    placeholder.width = kWidth;
    placeholder.height = kHeight;
    placeholder.format = coral::EffectPixelFormat::RGBA8;
    placeholder.planes[0].data = blackPixels.data();
    placeholder.planes[0].rowBytes = kWidth * 4;
    placeholder.planeCount = 1;
    placeholder.timestamp = coral::RenderTime::fromTicks(timestamp, kTimeBase);
    if (videoB->submit(placeholder) != coral::EffectResult::Success) {
      std::fprintf(stderr, "the placeholder frame was refused\n");
      return 1;
    }
  }

  const auto startBegin = std::chrono::steady_clock::now();
  const coral::EffectResult started = engine->start();
  const double startMilliseconds =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startBegin).count();
  if (started != coral::EffectResult::Success) {
    if (started == coral::EffectResult::DeviceLost) {
      std::fprintf(stderr, "no GPU device: %s\n", engine->diagnostics().c_str());
#if defined(_WIN32)
      if (window != nullptr) DestroyWindow(window);
#endif
      return 77;
    }
    std::fprintf(stderr, "the engine did not start: %s\n", engine->diagnostics().c_str());
    return 1;
  }
  std::printf("start_ms=%.3f placeholder=%d\n", startMilliseconds, options.placeholder ? 1 : 0);

  const uint32_t totalFrames = options.preloadFrames + options.transitionFrames + options.holdFrames;
  constexpr int64_t kFrameStep = kTimeBase / 30;  // 30 fps steps
  uint32_t submittedA = 0;
  uint32_t submittedB = options.placeholder ? 1u : 0u;
  uint32_t delivered = 0;
  uint32_t busyFrames = 0;
  uint32_t otherFailures = 0;
  std::string firstFailure;
  Phase phase = Phase::Preload;
  uint32_t phaseFrame = 0;
  const auto renderBegin = std::chrono::steady_clock::now();

  for (uint32_t frame = 0; frame < totalFrames; ++frame) {
    phaseFrame = phase == Phase::Preload ? frame
                 : phase == Phase::Transition
                         ? frame - options.preloadFrames
                         : frame - options.preloadFrames - options.transitionFrames;
    Phase next = Phase::Preload;
    if (frame >= options.preloadFrames + options.transitionFrames) next = Phase::Hold;
    else if (frame >= options.preloadFrames) next = Phase::Transition;
    if (next != phase) {
      std::printf("phase=%s begins at frame %u (a_submitted=%u b_submitted=%u)\n", phaseName(next), frame,
                  submittedA, submittedB);
      phase = next;
      phaseFrame = 0;
    }

    double progress = 0.0;
    if (phase == Phase::Transition && options.transitionFrames > 0) {
      progress = static_cast<double>(phaseFrame + 1) / static_cast<double>(options.transitionFrames);
    } else if (phase == Phase::Hold) {
      progress = 1.0;
    }

    // What the application feeds each tick: A until its picture is no longer needed, B once it is playing. A stops
    // at the start of the hold phase, which is what makes its frozen last frame the interesting thing to look at.
    if (phase == Phase::Preload || phase == Phase::Transition) {
      sourceA.render(submittedA);
      coral::VideoFrameDesc desc = sourceA.frame();
      desc.timestamp = coral::RenderTime::fromTicks(timestamp, kTimeBase);
      if (videoA->submit(desc) == coral::EffectResult::Success) ++submittedA;
    }
    if (phase == Phase::Transition || phase == Phase::Hold) {
      sourceB.render(submittedB);
      coral::VideoFrameDesc desc = sourceB.frame();
      desc.timestamp = coral::RenderTime::fromTicks(timestamp, kTimeBase);
      if (videoB->submit(desc) == coral::EffectResult::Success) ++submittedB;
    }
    (void) transition->setParameter("progress", static_cast<float>(progress));

    const coral::EffectResult executed = engine->execute();
    if (executed == coral::EffectResult::Success) {
      ++delivered;
    } else if (executed == coral::EffectResult::Busy) {
      ++busyFrames;
    } else {
      ++otherFailures;
    }
    if (executed != coral::EffectResult::Success && firstFailure.empty()) {
      // Kept once and printed at the end: the message names the input that had no frame, which is the whole
      // difference between "the device is busy" and "this node was never fed".
      firstFailure = engine->diagnostics();
    }
    if (options.perFrame) {
      std::printf("frame=%u phase=%s progress=%.2f result=%d a=%u b=%u\n", frame, phaseName(phase), progress,
                  static_cast<int>(executed), submittedA, submittedB);
    }
    timestamp += kFrameStep;
    if (!options.hidden && !options.nullOutput) {
#if defined(_WIN32)
      MSG message {};
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
#endif
    }
    // Paced at 30 fps so a visible run looks like playback. `--hidden` runs flat out: the number that matters here
    // is which frames succeeded, not how fast they were.
    if (!options.hidden) std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }

  const double elapsedMilliseconds =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - renderBegin).count();
  const coral::EffectEngineStats stats = engine->stats();
  std::printf("frames=%u delivered=%u busy=%u failed=%u\n", totalFrames, delivered, busyFrames, otherFailures);
  std::printf("submitted a=%u b=%u elapsed_ms=%.1f fps=%.1f\n", submittedA, submittedB, elapsedMilliseconds,
              elapsedMilliseconds > 0.0 ? static_cast<double>(delivered) * 1000.0 / elapsedMilliseconds : 0.0);
  std::printf("engine frames=%llu dropped=%u cpu_ms=%.3f\n",
              static_cast<unsigned long long>(stats.frameCount), stats.droppedFrames, stats.cpuMilliseconds);
  const std::string diagnostics = engine->diagnostics();
  if (!diagnostics.empty()) std::printf("diagnostics: %s\n", diagnostics.c_str());
  if (!firstFailure.empty()) std::printf("first_failure: %s\n", firstFailure.c_str());
  engine->stop();
#if defined(_WIN32)
  if (window != nullptr) DestroyWindow(window);
#endif
  // The exit code is a verdict, so a CI run turns a regression into a failure rather than a log to read.
  if (busyFrames != 0 || otherFailures != 0) return 1;
  if (delivered != totalFrames) return 1;
  return 0;
}
