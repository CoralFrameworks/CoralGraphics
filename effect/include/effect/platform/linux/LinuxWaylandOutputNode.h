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

#ifndef CORAL_LINUX_WAYLAND_OUTPUT_NODE_H
#define CORAL_LINUX_WAYLAND_OUTPUT_NODE_H

#include <effect/EffectOutputNode.h>

#include <cstdint>
#include <memory>

struct wl_display;
struct wl_surface;
struct wl_egl_window;

namespace coral {

/**
 * Presents the graph's final pooled frame to a Wayland surface.
 *
 * The display and surface are borrowed from the application (which owns the `xdg_surface`/`xdg_toplevel`
 * role); the node creates the `wl_egl_window` the OpenGL backend's EGL surface needs and destroys it at
 * release. The engine creates its device for that surface before `start()`, so `setOutput` has to happen
 * first. Vulkan uses the `wl_surface` directly through `VK_KHR_wayland_surface`; OpenGL wraps it with the
 * `wl_egl_window`.
 *
 * Wayland's client library is not safe to use from two threads at once unless the application has set up
 * event queues deliberately, so the application must not dispatch the same display while the engine is
 * running.
 */
class LinuxWaylandOutputNode final : public EffectOutputNode {
public:
  static std::shared_ptr<LinuxWaylandOutputNode> create(wl_display* display, wl_surface* surface, uint32_t width,
                                                        uint32_t height);

  ~LinuxWaylandOutputNode() override;

  bool isReady() const noexcept override;
  EffectResult configure(const EffectOutputConfig& config) override;

  [[nodiscard]] wl_display* display() const noexcept;
  [[nodiscard]] wl_surface* surface() const noexcept;

  /** Implementation detail; opaque to a client. */
  struct State;

private:
  LinuxWaylandOutputNode(wl_display* display, wl_surface* surface, uint32_t width, uint32_t height);

  std::shared_ptr<State> mState;
};

} // namespace coral

#endif // CORAL_LINUX_WAYLAND_OUTPUT_NODE_H
