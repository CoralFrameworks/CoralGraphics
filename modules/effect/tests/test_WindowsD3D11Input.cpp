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
 * @file test_WindowsD3D11Input.cpp
 *
 * The Windows D3D11 shared-texture input, end to end: a D3D11 texture is cleared to green, handed to the
 * engine as a shared NT handle, sampled by a pass, and read back. On the Vulkan backend this exercises the
 * zero-copy import; on the OpenGL backend it exercises the staging fallback.
 */

#include <effect/EffectEngine.h>
#include <effect/EffectPassNode.h>
#include <effect/ReadbackOutputNode.h>
#include <effect/platform/win/WindowsD3D11InputNode.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <iterator>
#include <memory>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t kSize = 16;

struct D3D11Texture {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Texture2D> texture;

  bool create(const float rgba[4]) {
    // 11_1 is deliberately absent: requesting it on a runtime without the 11.1 interface makes the
    // whole D3D11CreateDevice call fail with E_INVALIDARG.
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
    HRESULT created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                        levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(created)) {
      // A headless/remote session has no hardware D3D11 device; WARP can still produce and share a texture.
      const HRESULT hardware = created;
      created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                  static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device, &level, &context);
      if (FAILED(created)) {
        std::fprintf(stderr, "D3D11CreateDevice hardware=0x%08X warp=0x%08X\n", static_cast<unsigned>(hardware),
                     static_cast<unsigned>(created));
        return false;
      }
    }

    D3D11_TEXTURE2D_DESC description{};
    description.Width = kSize;
    description.Height = kSize;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    // A shared NT handle is what the Vulkan import consumes; the node turns it into one itself, but the
    // resource has to be created shareable. WARP/headless devices reject NT handles, so the test falls
    // back to the legacy shared handle, which still exercises the node's staging/upload path.
    description.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HRESULT textureResult = device->CreateTexture2D(&description, nullptr, &texture);
    if (FAILED(textureResult) || texture == nullptr) {
      std::fprintf(stderr, "CreateTexture2D shared NT failed: 0x%08X; trying the legacy shared handle\n",
                   static_cast<unsigned>(textureResult));
      texture.Reset();
      description.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
      textureResult = device->CreateTexture2D(&description, nullptr, &texture);
    }
    if (FAILED(textureResult) || texture == nullptr) {
      std::fprintf(stderr, "CreateTexture2D legacy shared failed: 0x%08X\n", static_cast<unsigned>(textureResult));
      return false;
    }

    ComPtr<ID3D11RenderTargetView> view;
    const HRESULT viewResult = device->CreateRenderTargetView(texture.Get(), nullptr, &view);
    if (FAILED(viewResult) || view == nullptr) {
      std::fprintf(stderr, "CreateRenderTargetView failed: 0x%08X\n", static_cast<unsigned>(viewResult));
      return false;
    }
    context->ClearRenderTargetView(view.Get(), rgba);
    context->Flush();
    return true;
  }
};

TEST(WindowsD3D11Input, QueuedTexturesKeepTheirOwnTimestamps) {
  const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
  const float green[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
  D3D11Texture first;
  D3D11Texture second;
  if (!first.create(red) || !second.create(green)) GTEST_SKIP() << "no D3D11 device on this machine";

  coral::EffectEngineConfig config;
  config.renderWidth = kSize;
  config.renderHeight = kSize;
  config.renderMode = coral::EffectRenderMode::Manual;

  const auto engine = coral::EffectEngine::create(config);
  ASSERT_NE(engine, nullptr);
  const auto input = coral::WindowsD3D11InputNode::create(first.texture.Get(), kSize, kSize,
                                                          coral::EffectPixelFormat::RGBA8, 2);
  const auto pass = engine->createEffectNode("queue");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(input->submit(first.texture.Get(), kSize, kSize, coral::EffectPixelFormat::RGBA8,
                          coral::RenderTime::fromTicks(10, 60)),
            coral::EffectResult::Success);
  ASSERT_EQ(input->submit(second.texture.Get(), kSize, kSize, coral::EffectPixelFormat::RGBA8,
                          coral::RenderTime::fromTicks(20, 60)),
            coral::EffectResult::Success);

  coral::EffectShaderDesc shader;
  shader.name = "test/d3d11-queue";
  shader.fragmentSource = "  return texture(uInput0, uv);";
  shader.inputCount = 1;
  ASSERT_EQ(pass->setShader(shader), coral::EffectResult::Success);
  ASSERT_EQ(engine->attach(input), coral::EffectResult::Success);
  ASSERT_EQ(engine->attach(output), coral::EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), coral::EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), coral::EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), coral::EffectResult::Success);

  const coral::EffectResult started = engine->start();
  if (started == coral::EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, coral::EffectResult::Success) << engine->diagnostics();

  std::vector<uint8_t> pixels;
  coral::EffectFrame delivered;
  for (uint32_t frame = 0; frame < 2; ++frame) {
    ASSERT_EQ(engine->execute(), coral::EffectResult::Success) << engine->diagnostics();
    ASSERT_TRUE(output->waitForFrame(&delivered, &pixels));
    const size_t centre = (static_cast<size_t>(kSize / 2) * kSize + kSize / 2) * 4;
    if (frame == 0) {
      EXPECT_EQ(delivered.timestamp().value, 10);
      EXPECT_NEAR(pixels[centre + 0], 255, 4);
      EXPECT_NEAR(pixels[centre + 1], 0, 4);
    } else {
      EXPECT_EQ(delivered.timestamp().value, 20);
      EXPECT_NEAR(pixels[centre + 0], 0, 4);
      EXPECT_NEAR(pixels[centre + 1], 255, 4);
    }
  }
  engine->stop();
}

} // namespace

TEST(WindowsD3D11Input, ASharedTextureReachesTheReadback) {
  const float green[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
  D3D11Texture producer;
  if (!producer.create(green)) GTEST_SKIP() << "no D3D11 device on this machine";

  coral::EffectEngineConfig config;
  config.renderWidth = kSize;
  config.renderHeight = kSize;
  config.renderMode = coral::EffectRenderMode::Manual;
  config.timeBase = 60;

  const auto engine = coral::EffectEngine::create(config);
  ASSERT_NE(engine, nullptr);
  const auto input = coral::WindowsD3D11InputNode::create(producer.texture.Get(), kSize, kSize);
  const auto pass = engine->createEffectNode("copy");
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(input->submit(producer.texture.Get(), kSize, kSize, coral::EffectPixelFormat::RGBA8,
                          coral::RenderTime::fromTicks(77, 60)),
            coral::EffectResult::Success);

  coral::EffectShaderDesc shader;
  shader.name = "test/d3d11-copy";
  shader.fragmentSource = "  return texture(uInput0, uv);";
  shader.inputCount = 1;
  ASSERT_EQ(pass->setShader(shader), coral::EffectResult::Success);
  ASSERT_EQ(engine->attach(input), coral::EffectResult::Success);
  ASSERT_EQ(engine->attach(output), coral::EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), coral::EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), coral::EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), coral::EffectResult::Success);

  const coral::EffectResult started = engine->start();
  if (started == coral::EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, coral::EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), coral::EffectResult::Success) << engine->diagnostics();

  std::vector<uint8_t> pixels;
  coral::EffectFrame delivered;
  ASSERT_TRUE(output->waitForFrame(&delivered, &pixels)) << engine->diagnostics();
  ASSERT_TRUE(delivered.isValid());
  EXPECT_EQ(delivered.timestamp().value, 77);
  EXPECT_EQ(delivered.timestamp().timescale, 60u);
  ASSERT_EQ(pixels.size(), static_cast<size_t>(kSize) * kSize * 4);
  const size_t centre = (static_cast<size_t>(kSize / 2) * kSize + kSize / 2) * 4;
  EXPECT_NEAR(pixels[centre + 0], 0, 4);
  EXPECT_NEAR(pixels[centre + 1], 255, 4);
  EXPECT_NEAR(pixels[centre + 2], 0, 4);
  EXPECT_NEAR(pixels[centre + 3], 255, 4);
  engine->stop();
}
