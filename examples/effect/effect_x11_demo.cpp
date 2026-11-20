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
 * @file effect_x11_demo.cpp
 *
 * The M6 X11 end-to-end demo: `TestPatternInputNode -> invert pass -> LinuxX11OutputNode`.
 *
 * Renders a few frames manually into a real X11 window. Under WSLg it runs against the WSLg X server and
 * the Mesa driver, which is enough to verify the X11 surface, swapchain and present path; a machine
 * without a display exits 77 so CTest records a skip.
 */

#include <effect/EffectEngine.h>
#include <effect/EffectPassNode.h>
#include <effect/TestPatternInputNode.h>
#include <effect/platform/linux/LinuxX11OutputNode.h>

#include <X11/Xlib.h>

#undef None
#undef Success
#undef Status
#undef Always
#undef Bool
#undef True
#undef False

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

constexpr uint32_t kWidth = 640;
constexpr uint32_t kHeight = 360;

void pumpEvents(Display* display) {
  while (XPending(display) > 0) {
    XEvent event{};
    XNextEvent(display, &event);
  }
}

} // namespace

int main(int argc, char** argv) {
  uint32_t frames = 5;
  for (int index = 1; index < argc; ++index) {
    if (std::strcmp(argv[index], "--frames") == 0 && index + 1 < argc) {
      frames = static_cast<uint32_t>(std::strtoul(argv[index + 1], nullptr, 10));
      ++index;
    }
  }

  // The engine creates its GLX/Vulkan surface from the render thread; Xlib needs this before any call
  // that could race with the application thread.
  if (XInitThreads() == 0) {
    std::fprintf(stderr, "XInitThreads failed\n");
    return 1;
  }
  Display* display = XOpenDisplay(nullptr);
  if (display == nullptr) {
    std::fprintf(stderr, "no X display (DISPLAY is unset or unreachable)\n");
    return 77;
  }

  const Window root = DefaultRootWindow(display);
  XSetWindowAttributes attributes{};
  attributes.event_mask = StructureNotifyMask;
  const Window window = XCreateWindow(display, root, 0, 0, kWidth, kHeight, 0, CopyFromParent, InputOutput,
                                      CopyFromParent, CWEventMask, &attributes);
  if (window == 0) {
    std::fprintf(stderr, "the X11 window could not be created\n");
    XCloseDisplay(display);
    return 1;
  }
  XStoreName(display, window, "coral effect engine - M6 X11 output");
  XMapWindow(display, window);
  XFlush(display);
  pumpEvents(display);

  coral::EffectEngineConfig config;
  config.renderWidth = kWidth;
  config.renderHeight = kHeight;
  config.renderMode = coral::EffectRenderMode::Manual;
  config.timeBase = 60;
  config.maxFramesInFlight = 2;

  auto engine = coral::EffectEngine::create(config);
  auto input = coral::TestPatternInputNode::create(kWidth, kHeight);
  auto pass = engine != nullptr ? engine->createEffectNode("invert") : nullptr;
  auto output = coral::LinuxX11OutputNode::create(display, window);
  if (engine == nullptr || input == nullptr || pass == nullptr || output == nullptr) {
    std::fprintf(stderr, "the X11 demo graph could not be built\n");
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 1;
  }

  coral::EffectShaderDesc shader;
  shader.name = "demo/invert";
  shader.fragmentSource = "  return vec4(1.0 - texture(uInput0, uv).rgb, 1.0);";
  shader.inputCount = 1;
  if (pass->setShader(shader) != coral::EffectResult::Success) {
    std::fprintf(stderr, "the shader was refused: %s\n", pass->diagnostics().c_str());
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 1;
  }
  if (engine->attach(input) != coral::EffectResult::Success ||
      engine->attach(output) != coral::EffectResult::Success ||
      engine->connect(input, pass) != coral::EffectResult::Success ||
      engine->connect(pass, output) != coral::EffectResult::Success ||
      engine->setOutput(output) != coral::EffectResult::Success) {
    std::fprintf(stderr, "the X11 demo graph could not be connected: %s\n", engine->diagnostics().c_str());
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 1;
  }

  const coral::EffectResult started = engine->start();
  if (started == coral::EffectResult::DeviceLost) {
    std::fprintf(stderr, "no GPU device: %s\n", engine->diagnostics().c_str());
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 77;
  }
  if (started != coral::EffectResult::Success) {
    std::fprintf(stderr, "the engine did not start: %s\n", engine->diagnostics().c_str());
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 1;
  }

  for (uint32_t frame = 0; frame < frames; ++frame) {
    pumpEvents(display);
    const coral::EffectResult executed = engine->execute();
    if (executed != coral::EffectResult::Success) {
      std::fprintf(stderr, "frame %u failed: %s\n", frame, engine->diagnostics().c_str());
      engine->stop();
      XDestroyWindow(display, window);
      XCloseDisplay(display);
      return 1;
    }
  }

  const coral::EffectEngineStats stats = engine->stats();
  std::printf("frames=%llu programs=%u pool hits/misses=%llu/%llu\n",
              static_cast<unsigned long long>(stats.frameCount), stats.programCount,
              static_cast<unsigned long long>(stats.framePoolHits),
              static_cast<unsigned long long>(stats.framePoolMisses));

  engine->stop();
  XDestroyWindow(display, window);
  XCloseDisplay(display);
  return stats.frameCount == frames ? 0 : 1;
}
