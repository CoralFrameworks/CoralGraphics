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
 * @file effect_wayland_demo.cpp
 *
 * The M6 Wayland end-to-end demo: a minimal xdg-shell window, then
 * `TestPatternInputNode -> invert pass -> LinuxWaylandOutputNode`.
 *
 * The application owns the xdg role; the node wraps the `wl_surface` in the `wl_egl_window` the OpenGL
 * EGL surface needs. A machine without a Wayland compositor exits 77 so CTest records a skip.
 */

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#include <effect/EffectEngine.h>
#include <effect/EffectPassNode.h>
#include <effect/TestPatternInputNode.h>
#include <effect/platform/linux/LinuxWaylandOutputNode.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

constexpr uint32_t kWidth = 640;
constexpr uint32_t kHeight = 360;

struct App {
  wl_display* display = nullptr;
  wl_registry* registry = nullptr;
  wl_compositor* compositor = nullptr;
  xdg_wm_base* shell = nullptr;
  wl_surface* surface = nullptr;
  xdg_surface* xdgSurface = nullptr;
  xdg_toplevel* toplevel = nullptr;
  bool configured = false;
};

void shellPing(void*, xdg_wm_base* shell, const uint32_t serial) { xdg_wm_base_pong(shell, serial); }

const xdg_wm_base_listener kShellListener = { shellPing };

void xdgSurfaceConfigure(void* data, xdg_surface* surface, const uint32_t serial) {
  auto* app = static_cast<App*>(data);
  xdg_surface_ack_configure(surface, serial);
  app->configured = true;
}

const xdg_surface_listener kXdgSurfaceListener = { xdgSurfaceConfigure };

void registryGlobal(void* data, wl_registry* registry, const uint32_t name, const char* interface,
                    const uint32_t /*version*/) {
  auto* app = static_cast<App*>(data);
  if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
    app->compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
  } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
    app->shell = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
    xdg_wm_base_add_listener(app->shell, &kShellListener, app);
  }
}

void registryGlobalRemove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener kRegistryListener = { registryGlobal, registryGlobalRemove };

} // namespace

int main(int argc, char** argv) {
  uint32_t frames = 5;
  for (int index = 1; index < argc; ++index) {
    if (std::strcmp(argv[index], "--frames") == 0 && index + 1 < argc) {
      frames = static_cast<uint32_t>(std::strtoul(argv[index + 1], nullptr, 10));
      ++index;
    }
  }

  App app;
  app.display = wl_display_connect(nullptr);
  if (app.display == nullptr) {
    std::fprintf(stderr, "no Wayland compositor (WAYLAND_DISPLAY is unset or unreachable)\n");
    return 77;
  }
  app.registry = wl_display_get_registry(app.display);
  wl_registry_add_listener(app.registry, &kRegistryListener, &app);
  wl_display_roundtrip(app.display);
  if (app.compositor == nullptr || app.shell == nullptr) {
    std::fprintf(stderr, "the Wayland compositor lacks wl_compositor or xdg_wm_base\n");
    wl_display_disconnect(app.display);
    return 1;
  }

  app.surface = wl_compositor_create_surface(app.compositor);
  app.xdgSurface = xdg_wm_base_get_xdg_surface(app.shell, app.surface);
  xdg_surface_add_listener(app.xdgSurface, &kXdgSurfaceListener, &app);
  app.toplevel = xdg_surface_get_toplevel(app.xdgSurface);
  xdg_toplevel_set_title(app.toplevel, "coral effect engine - M6 Wayland output");
  xdg_toplevel_set_app_id(app.toplevel, "com.cgfay.coral.effect");
  wl_surface_commit(app.surface);
  while (!app.configured) {
    if (wl_display_dispatch(app.display) < 0) {
      std::fprintf(stderr, "the Wayland display disconnected during configure\n");
      return 1;
    }
  }

  coral::EffectEngineConfig config;
  config.renderWidth = kWidth;
  config.renderHeight = kHeight;
  config.renderMode = coral::EffectRenderMode::Manual;
  config.timeBase = 60;
  config.maxFramesInFlight = 2;

  auto engine = coral::EffectEngine::create(config);
  auto input = coral::TestPatternInputNode::create(kWidth, kHeight);
  auto pass = engine != nullptr ? engine->createEffectNode("invert") : nullptr;
  auto output = coral::LinuxWaylandOutputNode::create(app.display, app.surface, kWidth, kHeight);
  if (engine == nullptr || input == nullptr || pass == nullptr || output == nullptr) {
    std::fprintf(stderr, "the Wayland demo graph could not be built\n");
    return 1;
  }

  coral::EffectShaderDesc shader;
  shader.name = "demo/invert";
  shader.fragmentSource = "  return vec4(1.0 - texture(uInput0, uv).rgb, 1.0);";
  shader.inputCount = 1;
  if (pass->setShader(shader) != coral::EffectResult::Success) {
    std::fprintf(stderr, "the shader was refused: %s\n", pass->diagnostics().c_str());
    return 1;
  }
  if (engine->attach(input) != coral::EffectResult::Success ||
      engine->attach(output) != coral::EffectResult::Success ||
      engine->connect(input, pass) != coral::EffectResult::Success ||
      engine->connect(pass, output) != coral::EffectResult::Success ||
      engine->setOutput(output) != coral::EffectResult::Success) {
    std::fprintf(stderr, "the Wayland demo graph could not be connected: %s\n", engine->diagnostics().c_str());
    return 1;
  }

  const coral::EffectResult started = engine->start();
  if (started == coral::EffectResult::DeviceLost) {
    std::fprintf(stderr, "no GPU device: %s\n", engine->diagnostics().c_str());
    return 77;
  }
  if (started != coral::EffectResult::Success) {
    std::fprintf(stderr, "the engine did not start: %s\n", engine->diagnostics().c_str());
    return 1;
  }

  for (uint32_t frame = 0; frame < frames; ++frame) {
    const coral::EffectResult executed = engine->execute();
    if (executed != coral::EffectResult::Success) {
      std::fprintf(stderr, "frame %u failed: %s\n", frame, engine->diagnostics().c_str());
      engine->stop();
      return 1;
    }
    wl_display_flush(app.display);
  }

  const coral::EffectEngineStats stats = engine->stats();
  std::printf("frames=%llu programs=%u pool hits/misses=%llu/%llu\n",
              static_cast<unsigned long long>(stats.frameCount), stats.programCount,
              static_cast<unsigned long long>(stats.framePoolHits),
              static_cast<unsigned long long>(stats.framePoolMisses));

  engine->stop();
  xdg_toplevel_destroy(app.toplevel);
  xdg_surface_destroy(app.xdgSurface);
  wl_surface_destroy(app.surface);
  xdg_wm_base_destroy(app.shell);
  wl_compositor_destroy(app.compositor);
  wl_registry_destroy(app.registry);
  wl_display_disconnect(app.display);
  return stats.frameCount == frames ? 0 : 1;
}
