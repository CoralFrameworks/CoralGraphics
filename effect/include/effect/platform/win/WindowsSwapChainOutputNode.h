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

#ifndef CORAL_WINDOWS_SWAP_CHAIN_OUTPUT_NODE_H
#define CORAL_WINDOWS_SWAP_CHAIN_OUTPUT_NODE_H

#include <effect/EffectOutputNode.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <memory>

namespace coral {

/**
 * Presents the graph's final pooled frame to a Win32 window through the RHI's swapchain.
 *
 * The node takes its own device context (`GetDC`) for the window and releases it at destruction; the
 * window itself is borrowed and must outlive the node. As with the Android surface output, the engine
 * creates its device for this window - so `setOutput` has to happen before `start()` - and the last pass
 * writes a normal pooled RGBA8 frame that this node copies to the RHI's default-framebuffer view.
 *
 * Works on both Windows backends: the OpenGL backend presents through the window's WGL context, and the
 * Vulkan backend through `VK_KHR_win32_surface`. Both are reached through the same RHI calls.
 */
class WindowsSwapChainOutputNode final : public EffectOutputNode {
public:
  /** @param window the target window; the node borrows it and must not outlive it. Null is refused. */
  static std::shared_ptr<WindowsSwapChainOutputNode> create(HWND window);

  ~WindowsSwapChainOutputNode() override;

  bool isReady() const noexcept override;
  EffectResult configure(const EffectOutputConfig& config) override;

  /** The window this node presents to. Borrowed for the duration of the call. */
  [[nodiscard]] HWND window() const noexcept;

  /** Implementation detail; opaque to a client. */
  struct State;

private:
  explicit WindowsSwapChainOutputNode(HWND window);

  std::shared_ptr<State> mState;
};

} // namespace coral

#endif // CORAL_WINDOWS_SWAP_CHAIN_OUTPUT_NODE_H
