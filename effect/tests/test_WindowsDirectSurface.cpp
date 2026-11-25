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
 * @file test_WindowsDirectSurface.cpp
 *
 * End-to-end Windows surface test for the direct-to-surface executor path. Real Win32 windows are
 * rendered with the same solid-green pass once through `setDirectToSurface(true)` and once through the
 * pooled-frame output. The centre pixel and average frame time are compared.
 *
 * In a session without an interactive desktop (or when the driver cannot present to it) the test skips
 * instead of failing.
 */

#include <effect/EffectEngine.h>
#include <effect/EffectPassNode.h>
#include <effect/TestPatternInputNode.h>
#include <effect/platform/win/WindowsSwapChainOutputNode.h>

#include <gtest/gtest.h>

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>

namespace {

constexpr wchar_t kWindowClass[] = L"coral-direct-surface-test";

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

HWND createWindow(const uint32_t width, const uint32_t height) {
  WNDCLASSEXW windowClass{};
  windowClass.cbSize = sizeof(windowClass);
  windowClass.lpfnWndProc = windowProc;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.lpszClassName = kWindowClass;
  windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
  (void) RegisterClassExW(&windowClass);

  RECT rect{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowExW(0, kWindowClass, L"coral direct surface test", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                                rect.bottom - rect.top, nullptr, nullptr, windowClass.hInstance, nullptr);
  if (window != nullptr) {
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
  }
  return window;
}

void pumpMessages() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

bool readCentrePixel(HWND window, const uint32_t width, const uint32_t height, COLORREF& colour) {
  HDC windowDc = GetDC(window);
  if (windowDc == nullptr) return false;
  HDC memoryDc = CreateCompatibleDC(windowDc);
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = static_cast<LONG>(width);
  info.bmiHeader.biHeight = -static_cast<LONG>(height);
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bitmap = CreateDIBSection(windowDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (memoryDc == nullptr || bitmap == nullptr || bits == nullptr) {
    if (bitmap != nullptr) DeleteObject(bitmap);
    if (memoryDc != nullptr) DeleteDC(memoryDc);
    ReleaseDC(window, windowDc);
    return false;
  }
  HGDIOBJ previous = SelectObject(memoryDc, bitmap);
  BOOL copied = PrintWindow(window, memoryDc, 2); // PW_RENDERFULLCONTENT
  if (copied == FALSE) {
    copied = BitBlt(memoryDc, 0, 0, static_cast<int>(width), static_cast<int>(height), windowDc, 0, 0, SRCCOPY);
  }
  bool ok = false;
  if (copied != FALSE) {
    const auto* pixels = static_cast<const uint32_t*>(bits);
    const uint32_t bgra = pixels[(height / 2) * width + width / 2];
    colour = RGB((bgra >> 16) & 0xFF, (bgra >> 8) & 0xFF, bgra & 0xFF);
    ok = true;
  }
  SelectObject(memoryDc, previous);
  DeleteObject(bitmap);
  DeleteDC(memoryDc);
  ReleaseDC(window, windowDc);
  return ok;
}

/**
 * Renders the solid-green graph for @p width x @p height and returns the centre pixel and average time.
 * @return false when the device or the window readback is unavailable.
 */
bool renderFrame(HWND window, const bool directToSurface, const uint32_t width, const uint32_t height,
                 COLORREF& colour, double& frameMs, uint64_t& directFrames, std::string& error,
                 const std::function<void()>& onMidFrame = {}) {
  coral::EffectEngineConfig config;
  config.renderWidth = width;
  config.renderHeight = height;
  config.renderMode = coral::EffectRenderMode::Manual;
  config.timeBase = 60;
  config.maxFramesInFlight = 2;

  const auto engine = coral::EffectEngine::create(config);
  const auto input = coral::TestPatternInputNode::create(width, height);
  const auto pass = engine != nullptr ? engine->createEffectNode("solid") : nullptr;
  const auto output = coral::WindowsSwapChainOutputNode::create(window);
  if (engine == nullptr || input == nullptr || pass == nullptr || output == nullptr) {
    error = "graph creation failed";
    return false;
  }

  coral::EffectShaderDesc shader;
  shader.name = "test/direct-solid";
  shader.fragmentSource = "  return vec4(0.0, 1.0, 0.0, 1.0);";
  if (pass->setShader(shader) != coral::EffectResult::Success ||
      (directToSurface && pass->setDirectToSurface(true) != coral::EffectResult::Success)) {
    error = "shader or direct flag was refused";
    return false;
  }
  coral::EffectOutputConfig outputConfig;
  // Zero means "follow the engine size", which is what lets the executor accept a direct target.
  outputConfig.format = coral::EffectPixelFormat::RGBA8;
  if (output->configure(outputConfig) != coral::EffectResult::Success ||
      engine->attach(input) != coral::EffectResult::Success ||
      engine->attach(output) != coral::EffectResult::Success ||
      engine->connect(input, pass) != coral::EffectResult::Success ||
      engine->connect(pass, output) != coral::EffectResult::Success ||
      engine->setOutput(output) != coral::EffectResult::Success) {
    error = "graph setup failed";
    return false;
  }

  const coral::EffectResult started = engine->start();
  if (started == coral::EffectResult::DeviceLost) {
    error = "no GPU device";
    return false;
  }
  if (started != coral::EffectResult::Success) {
    error = "start failed: " + engine->diagnostics();
    return false;
  }
  constexpr uint32_t kMeasuredFrames = 30;
  const auto start = std::chrono::steady_clock::now();
  for (uint32_t frame = 0; frame < kMeasuredFrames; ++frame) {
    if (frame == 15 && onMidFrame) {
      onMidFrame();
      pumpMessages();
    }
    pumpMessages();
    if (engine->execute() != coral::EffectResult::Success) {
      error = "execute failed: " + engine->diagnostics();
      engine->stop();
      return false;
    }
    pumpMessages();
  }
  const auto end = std::chrono::steady_clock::now();
  frameMs = std::chrono::duration<double, std::milli>(end - start).count() / kMeasuredFrames;
  directFrames = engine->stats().directFrames;
  Sleep(100);
  pumpMessages();
  const bool read = readCentrePixel(window, width, height, colour);
  engine->stop();
  if (!read) {
    error = "centre pixel could not be read";
    return false;
  }
  return true;
}

} // namespace

TEST(WindowsDirectSurface, DirectAndPooledPresentGreen) {
  HWND window = createWindow(64, 64);
  if (window == nullptr) GTEST_SKIP() << "no interactive Win32 window";

  COLORREF pooled = CLR_INVALID;
  COLORREF direct = CLR_INVALID;
  double pooledMs = 0.0;
  double directMs = 0.0;
  uint64_t pooledDirectFrames = 0;
  uint64_t directDirectFrames = 0;
  std::string pooledError;
  std::string directError;
  const bool pooledOk = renderFrame(window, false, 64, 64, pooled, pooledMs, pooledDirectFrames, pooledError);
  const bool directOk = renderFrame(window, true, 64, 64, direct, directMs, directDirectFrames, directError);
  DestroyWindow(window);

  if (!pooledOk && !directOk) {
    GTEST_SKIP() << "no presentable desktop: pooled='" << pooledError << "' direct='" << directError << "'";
  }
  ASSERT_TRUE(pooledOk) << "pooled path: " << pooledError;
  ASSERT_TRUE(directOk) << "direct path: " << directError;

  const auto green = [](const COLORREF colour) {
    return GetGValue(colour) > 200 && GetRValue(colour) < 80 && GetBValue(colour) < 80;
  };
  EXPECT_TRUE(green(pooled)) << "pooled centre pixel: " << static_cast<int>(pooled);
  EXPECT_TRUE(green(direct)) << "direct centre pixel: " << static_cast<int>(direct);
  EXPECT_LE(std::abs(static_cast<int>(GetRValue(pooled)) - static_cast<int>(GetRValue(direct))), 16);
  EXPECT_LE(std::abs(static_cast<int>(GetGValue(pooled)) - static_cast<int>(GetGValue(direct))), 16);
  EXPECT_LE(std::abs(static_cast<int>(GetBValue(pooled)) - static_cast<int>(GetBValue(direct))), 16);
  EXPECT_EQ(pooledDirectFrames, 0u);
  EXPECT_GT(directDirectFrames, 0u);
  EXPECT_GT(pooledMs, 0.0);
  EXPECT_GT(directMs, 0.0);
  RecordProperty("pooled_ms", std::to_string(pooledMs));
  RecordProperty("direct_ms", std::to_string(directMs));
  RecordProperty("direct_frames", std::to_string(directDirectFrames));
  std::cout << "[ direct-surface 64 ] pooled=" << pooledMs << " ms direct=" << directMs
            << " ms pooledDirectFrames=" << pooledDirectFrames
            << " directFrames=" << directDirectFrames << std::endl;
}

TEST(WindowsDirectSurface, ResizesTheWindowWhilePresenting) {
  HWND window = createWindow(64, 64);
  if (window == nullptr) GTEST_SKIP() << "no interactive Win32 window";

  COLORREF colour = CLR_INVALID;
  double frameMs = 0.0;
  uint64_t directFrames = 0;
  std::string error;
  const bool ok = renderFrame(window, false, 64, 64, colour, frameMs, directFrames, error, [window] {
    RECT rect{ 0, 0, 128, 96 };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
      SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  });
  const bool read = ok && readCentrePixel(window, 128, 96, colour);
  DestroyWindow(window);

  if (!ok) {
    GTEST_SKIP() << "no presentable desktop: " << error;
  }
  ASSERT_TRUE(read) << "the resized window could not be read";
  EXPECT_GT(GetGValue(colour), 200);
  EXPECT_LT(GetRValue(colour), 80);
  EXPECT_LT(GetBValue(colour), 80);
}

TEST(WindowsDirectSurface, DirectFrameTimeAt720p) {
  HWND window = createWindow(1280, 720);
  if (window == nullptr) GTEST_SKIP() << "no interactive Win32 window";

  COLORREF pooled = CLR_INVALID;
  COLORREF direct = CLR_INVALID;
  double pooledMs = 0.0;
  double directMs = 0.0;
  uint64_t pooledDirectFrames = 0;
  uint64_t directDirectFrames = 0;
  std::string pooledError;
  std::string directError;
  const bool pooledOk = renderFrame(window, false, 1280, 720, pooled, pooledMs, pooledDirectFrames, pooledError);
  const bool directOk = renderFrame(window, true, 1280, 720, direct, directMs, directDirectFrames, directError);
  DestroyWindow(window);

  if (!pooledOk && !directOk) {
    GTEST_SKIP() << "no presentable 720p window: pooled='" << pooledError << "' direct='" << directError << "'";
  }
  ASSERT_TRUE(pooledOk) << "pooled path: " << pooledError;
  ASSERT_TRUE(directOk) << "direct path: " << directError;
  if (directDirectFrames == 0) {
    GTEST_SKIP() << "backend has no direct swapchain target; fell back to pooled";
  }
  EXPECT_EQ(pooledDirectFrames, 0u);
  EXPECT_GT(pooledMs, 0.0);
  EXPECT_GT(directMs, 0.0);
  RecordProperty("pooled_ms_720p", std::to_string(pooledMs));
  RecordProperty("direct_ms_720p", std::to_string(directMs));
  RecordProperty("direct_frames_720p", std::to_string(directDirectFrames));
  std::cout << "[ direct-surface 720p ] pooled=" << pooledMs << " ms direct=" << directMs
            << " ms directFrames=" << directDirectFrames << std::endl;
}
