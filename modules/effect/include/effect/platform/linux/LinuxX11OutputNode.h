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

#ifndef CORAL_LINUX_X11_OUTPUT_NODE_H
#define CORAL_LINUX_X11_OUTPUT_NODE_H

#include <effect/EffectOutputNode.h>

// `Window` is an XID (an unsigned long) and `Display` is a forward-declarable struct. Naming them here
// rather than including <X11/Xlib.h> keeps Xlib's `None`/`Success`/`Always` macros out of every
// translation unit that includes this header; the implementation includes Xlib in its own .cpp.
#ifndef _X11_XLIB_H_
struct _XDisplay;
using Display = _XDisplay;
using Window = unsigned long;
#endif

#include <cstdint>
#include <memory>

namespace coral {

/**
 * Presents the graph's final pooled frame to an X11 window.
 *
 * The display and window are borrowed and must outlive the node. The engine creates its device for that
 * window - a GLX context or a Vulkan `VK_KHR_xlib_surface` - so `setOutput` has to happen before
 * `start()`. The last pass writes a normal pooled RGBA8 frame and this node copies it to the RHI's
 * default-framebuffer view, which both backends resolve to the acquired window image.
 *
 * Xlib is not thread-safe by default and the render thread creates its surface there: call `XInitThreads()`
 * before creating the node (the engine and the demo both do).
 */
class LinuxX11OutputNode final : public EffectOutputNode {
public:
  /** @param display the X display; @param window the target window. Both are borrowed. */
  static std::shared_ptr<LinuxX11OutputNode> create(Display* display, Window window);

  ~LinuxX11OutputNode() override;

  bool isReady() const noexcept override;
  EffectResult configure(const EffectOutputConfig& config) override;

  [[nodiscard]] Display* display() const noexcept;
  [[nodiscard]] Window window() const noexcept;

  /** Implementation detail; opaque to a client. */
  struct State;

private:
  LinuxX11OutputNode(Display* display, Window window);

  std::shared_ptr<State> mState;
};

} // namespace coral

#endif // CORAL_LINUX_X11_OUTPUT_NODE_H
