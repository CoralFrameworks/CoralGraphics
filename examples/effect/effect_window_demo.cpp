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
 * @file effect_window_demo.cpp
 *
 * The M6 Windows end-to-end demo: `TestPatternInputNode -> invert pass -> WindowsSwapChainOutputNode`.
 *
 * With no arguments it opens a window, renders 180 frames and exits; with `--hidden --frames N` it renders
 * the same graph into a hidden window, which is the form CTest runs. The engine is in Manual mode so a
 * frame is exactly one `execute()`, and the picture on screen is the inverted colour bars - the same
 * proof of a working pass that the Android preview activity uses.
 *
 * Exit code 77 means no GPU device, which CTest records as a skip rather than a failure.
 */

#include <effect/EffectEngine.h>
#include <effect/EffectPassNode.h>
#include <effect/NullOutputNode.h>
#include <effect/TestPatternInputNode.h>
#include <effect/platform/win/WindowsSwapChainOutputNode.h>

#include <backend/ShaderCompiler.h>

#include "ThreadSampler.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace {

constexpr wchar_t kWindowClass[] = L"coral-effect-window-demo";
constexpr uint32_t kWidth = 640;
constexpr uint32_t kHeight = 360;

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

HWND createWindow(const bool hidden) {
  WNDCLASSEXW windowClass{};
  windowClass.cbSize = sizeof(windowClass);
  windowClass.lpfnWndProc = windowProc;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.lpszClassName = kWindowClass;
  windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
  // A second registration in the same process is expected; the class is what matters.
  (void) RegisterClassExW(&windowClass);

  RECT rect{ 0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight) };
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  return CreateWindowExW(0, kWindowClass, L"coral effect engine - M6 Windows output",
                         hidden ? WS_POPUP : WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                         rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                         windowClass.hInstance, nullptr);
}

void pumpMessages() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

} // namespace

int main(int argc, char** argv) {
  bool hidden = false;
  uint32_t frames = 180;
  coral::EffectPresentMode presentMode = coral::EffectPresentMode::Fifo;
  coral::EffectPresentPolicy presentPolicy = coral::EffectPresentPolicy::Blocking;
  coral::EffectPresentOverflow presentOverflow = coral::EffectPresentOverflow::DropOldest;
  bool sampling = false;
  uint32_t presentIntervalMs = 0;
  bool nullOutputMode = false;
  uint32_t framesInFlight = 2;
  bool warmUp = true;
  bool asyncCompiler = false;
  bool asyncPipeline = false;
  for (int index = 1; index < argc; ++index) {
    if (std::strcmp(argv[index], "--hidden") == 0) {
      hidden = true;
    } else if (std::strcmp(argv[index], "--frames") == 0 && index + 1 < argc) {
      frames = static_cast<uint32_t>(std::strtoul(argv[index + 1], nullptr, 10));
      ++index;
    } else if (std::strcmp(argv[index], "--present-mode") == 0 && index + 1 < argc) {
      // `fifo` waits for the display, `immediate` queues and returns, `mailbox` replaces the queued
      // image. This is the difference between "a frame costs a millisecond" and "a frame waits for a
      // refresh": with Fifo the wait lands inside `execute()`.
      const char* mode = argv[++index];
      if (std::strcmp(mode, "immediate") == 0) {
        presentMode = coral::EffectPresentMode::Immediate;
      } else if (std::strcmp(mode, "mailbox") == 0) {
        presentMode = coral::EffectPresentMode::Mailbox;
      } else if (std::strcmp(mode, "fifo") == 0) {
        presentMode = coral::EffectPresentMode::Fifo;
      } else {
        std::fprintf(stderr, "unknown present mode %s (fifo|immediate|mailbox)\n", mode);
        return 2;
      }
    } else if (std::strcmp(argv[index], "--present-policy") == 0 && index + 1 < argc) {
      // `deferred` lets the engine skip a present the display cannot take; `blocking` is the old behaviour.
      const char* policy = argv[++index];
      if (std::strcmp(policy, "deferred") == 0) {
        presentPolicy = coral::EffectPresentPolicy::Deferred;
      } else if (std::strcmp(policy, "blocking") == 0) {
        presentPolicy = coral::EffectPresentPolicy::Blocking;
      } else {
        std::fprintf(stderr, "unknown present policy %s (blocking|deferred)\n", policy);
        return 2;
      }
    } else if (std::strcmp(argv[index], "--present-overflow") == 0 && index + 1 < argc) {
      const char* overflow = argv[++index];
      if (std::strcmp(overflow, "drop") == 0) {
        presentOverflow = coral::EffectPresentOverflow::DropOldest;
      } else if (std::strcmp(overflow, "wait") == 0) {
        presentOverflow = coral::EffectPresentOverflow::Wait;
      } else {
        std::fprintf(stderr, "unknown present overflow %s (drop|wait)\n", overflow);
        return 2;
      }
    } else if (std::strcmp(argv[index], "--sample") == 0) {
      sampling = true;
    } else if (std::strcmp(argv[index], "--present-interval") == 0 && index + 1 < argc) {
      presentIntervalMs = static_cast<uint32_t>(std::strtoul(argv[++index], nullptr, 10));
    } else if (std::strcmp(argv[index], "--null-output") == 0) {
      nullOutputMode = true;
    } else if (std::strcmp(argv[index], "--frames-in-flight") == 0 && index + 1 < argc) {
      framesInFlight = static_cast<uint32_t>(std::strtoul(argv[++index], nullptr, 10));
    } else if (std::strcmp(argv[index], "--no-warmup") == 0) {
      warmUp = false;
    } else if (std::strcmp(argv[index], "--async") == 0) {
      asyncCompiler = true;
    } else if (std::strcmp(argv[index], "--async-pipeline") == 0) {
      asyncCompiler = true;
      asyncPipeline = true;
    }
  }

  HWND window = createWindow(hidden);
  if (window == nullptr) {
    std::fprintf(stderr, "the demo window could not be created\n");
    return 1;
  }
  HWND secondWindow = createWindow(true);
  if (secondWindow == nullptr) {
    std::fprintf(stderr, "the second demo window could not be created\n");
    DestroyWindow(window);
    return 1;
  }
  if (!hidden) {
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
  }

  coral::EffectEngineConfig config;
  config.renderWidth = kWidth;
  config.renderHeight = kHeight;
  config.renderMode = coral::EffectRenderMode::Manual;
  config.timeBase = 60;
  config.maxFramesInFlight = framesInFlight;
  // Off with `--no-warmup`, so the demo can show what the trade costs and buys on this machine.
  config.warmUpShaderCompiler = warmUp;
  // Async compilation keeps the first shader off the frame that needs it: the pass draws nothing (or the old
  // program) until the worker finishes. Async pipeline creation moves the pipeline as well.
  config.asyncShaderCompilation = asyncCompiler;
  config.asyncPipelineCreation = asyncPipeline;

  auto engine = coral::EffectEngine::create(config);
  auto input = coral::TestPatternInputNode::create(kWidth, kHeight);
  auto pass = engine != nullptr ? engine->createEffectNode("invert") : nullptr;
  auto output = coral::WindowsSwapChainOutputNode::create(window);
  auto secondOutput = coral::WindowsSwapChainOutputNode::create(secondWindow);
  if (engine == nullptr || input == nullptr || pass == nullptr || output == nullptr || secondOutput == nullptr) {
    std::fprintf(stderr, "the demo graph could not be built\n");
    DestroyWindow(window);
    DestroyWindow(secondWindow);
    return 1;
  }

  coral::EffectShaderDesc shader;
  shader.name = "demo/invert";
  shader.fragmentSource = "  return vec4(1.0 - texture(uInput0, uv).rgb, 1.0);";
  shader.inputCount = 1;
  if (pass->setShader(shader) != coral::EffectResult::Success) {
    std::fprintf(stderr, "the shader was refused: %s\n", pass->diagnostics().c_str());
    DestroyWindow(window);
    DestroyWindow(secondWindow);
    return 1;
  }

  coral::EffectOutputConfig outputConfig;
  outputConfig.presentMode = presentMode;
  outputConfig.presentPolicy = presentPolicy;
  outputConfig.presentOverflow = presentOverflow;
  outputConfig.presentIntervalMilliseconds = presentIntervalMs;
  output->configure(outputConfig);
  secondOutput->configure(outputConfig);

  // `--null-output` swaps the swapchain outputs for a node that consumes the frame and does nothing. It is a
  // diagnostic: it separates "the engine costs this much" from "having a window attached costs this much".
  std::shared_ptr<coral::EffectOutputNode> primaryOutput = output;
  std::shared_ptr<coral::EffectOutputNode> secondaryOutput = secondOutput;
  std::shared_ptr<coral::NullOutputNode> nullOutput;
  if (nullOutputMode) {
    nullOutput = coral::NullOutputNode::create();
    if (nullOutput == nullptr) {
      std::fprintf(stderr, "the null output could not be created\n");
      DestroyWindow(window);
      DestroyWindow(secondWindow);
      return 1;
    }
    nullOutput->configure(outputConfig);
    primaryOutput = nullOutput;
    secondaryOutput = nullptr;
  }

  if (engine->attach(input) != coral::EffectResult::Success ||
      engine->attach(primaryOutput) != coral::EffectResult::Success ||
      engine->connect(input, pass) != coral::EffectResult::Success ||
      engine->connect(pass, primaryOutput) != coral::EffectResult::Success ||
      engine->setOutput(primaryOutput) != coral::EffectResult::Success ||
      (secondaryOutput != nullptr && (engine->attach(secondaryOutput) != coral::EffectResult::Success ||
                                             engine->connect(pass, secondaryOutput) != coral::EffectResult::Success ||
                                             engine->addOutput(secondaryOutput) != coral::EffectResult::Success))) {
    std::fprintf(stderr, "the demo graph could not be connected: %s\n", engine->diagnostics().c_str());
    DestroyWindow(window);
    DestroyWindow(secondWindow);
    return 1;
  }

  // `start()` is where the shader compiler is warmed up, so its duration belongs next to the first frame's
  // compile time: warming moves work between the two, and a number that showed only one of them would be a
  // half-truth.
  const auto startBegin = std::chrono::steady_clock::now();
  const coral::EffectResult started = engine->start();
  const double startMilliseconds =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startBegin).count();
  if (started == coral::EffectResult::DeviceLost) {
    std::fprintf(stderr, "no GPU device: %s\n", engine->diagnostics().c_str());
    DestroyWindow(window);
    DestroyWindow(secondWindow);
    return 77;
  }
  if (started != coral::EffectResult::Success) {
    std::fprintf(stderr, "the engine did not start: %s\n", engine->diagnostics().c_str());
    DestroyWindow(window);
    DestroyWindow(secondWindow);
    return 1;
  }

  // Sampling starts just before the loop so the profile covers frames and not start-up.
  coral::test::ThreadSampler sampler;
  if (sampling) sampler.start();

  const auto renderStart = std::chrono::steady_clock::now();
  double firstFrameMilliseconds = 0.0;
  for (uint32_t frame = 0; frame < frames; ++frame) {
    pumpMessages();
    if (frame == 2) {
      // Exercise the output's runtime resize path: the swapchain must follow the new client area.
      SetWindowPos(window, nullptr, 0, 0, 800, 480, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    const auto frameBegin = std::chrono::steady_clock::now();
    const coral::EffectResult executed = engine->execute();
    if (frame == 0) {
      firstFrameMilliseconds =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameBegin).count();
    }
    if (executed != coral::EffectResult::Success) {
      std::fprintf(stderr, "frame %u failed: %s\n", frame, engine->diagnostics().c_str());
      engine->stop();
      DestroyWindow(window);
      DestroyWindow(secondWindow);
      return 1;
    }
    if (!hidden) Sleep(16);
  }
  const auto renderEnd = std::chrono::steady_clock::now();

  const coral::EffectEngineStats stats = engine->stats();
  std::printf("frames=%llu programs=%u pool hits/misses=%llu/%llu backbuffer=%ux%u\n",
              static_cast<unsigned long long>(stats.frameCount), stats.programCount,
              static_cast<unsigned long long>(stats.framePoolHits),
              static_cast<unsigned long long>(stats.framePoolMisses), kWidth, kHeight);
  // What the engine actually granted, not what was asked for: `deferred` is a request that a blocking
  // present cannot honour, and a measurement that did not say so would be a lie.
  std::printf("deferred_supported=%d requested_policy=%s\n", engine->supportsDeferredPresent() ? 1 : 0,
              presentPolicy == coral::EffectPresentPolicy::Deferred ? "deferred" : "blocking");
  std::printf("start_ms=%.3f first_frame_ms=%.3f\n", startMilliseconds, firstFrameMilliseconds);
  std::printf("compile_count=%llu compile_ms=%.3f pipeline_count=%llu pipeline_ms=%.3f\n",
              static_cast<unsigned long long>(stats.programCompileCount), stats.programCompileMilliseconds,
              static_cast<unsigned long long>(stats.pipelineCreationCount), stats.pipelineCreationMilliseconds);
  uint64_t cacheEntries = 0;
  uint64_t cacheBytes = 0;
  backend::shaderCacheUsage(&cacheEntries, &cacheBytes);
  std::printf("shader_cache hits=%llu misses=%llu entries=%llu bytes=%llu budget=%llu directory=%s\n",
              static_cast<unsigned long long>(backend::shaderCacheHits()),
              static_cast<unsigned long long>(backend::shaderCacheMisses()),
              static_cast<unsigned long long>(cacheEntries), static_cast<unsigned long long>(cacheBytes),
              static_cast<unsigned long long>(backend::shaderCacheBudget()),
              backend::shaderCacheDirectory().c_str());
  // Warnings otherwise only reach a caller that reads the diagnostics, which in a CI log is nobody. A cache that
  // stops working is exactly the kind of thing that has to be visible in the log rather than in a debugger.
  {
    const std::string diagnostics = engine->diagnostics();
    if (!diagnostics.empty() && stats.frameCount > 0) {
      std::printf("diagnostics: %s\n", diagnostics.c_str());
    }
  }
  std::printf("last_frame cpu_ms=%.3f gpu_ms=%.3f\n", stats.cpuMilliseconds, stats.gpuMilliseconds);
  // Wall time over the present loop, first frame included: it is the number a windowed client sees, and
  // the first frame's compile is what `max_frame_ms` in the benchmark attributes separately.
  const double elapsedMs = std::chrono::duration<double, std::milli>(renderEnd - renderStart).count();
  if (stats.frameCount > 0) {
    std::printf("present_ms_per_frame=%.3f present_fps=%.1f\n", elapsedMs / static_cast<double>(stats.frameCount),
                static_cast<double>(stats.frameCount) * 1000.0 / elapsedMs);
  }
  if (sampling) sampler.report();

  engine->stop();
  DestroyWindow(window);
  DestroyWindow(secondWindow);
  return stats.frameCount == frames ? 0 : 1;
}


