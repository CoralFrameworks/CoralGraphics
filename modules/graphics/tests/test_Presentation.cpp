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
 * @file test_Presentation.cpp
 *
 * The colour-managed presentation pass, verified by its pixels rather than by its return code.
 *
 * A red canvas is presented into an 8-bit Display P3 target and read back. sRGB red converted to P3 is
 * not sRGB red: the primaries move, so the green and blue channels gain about 50 and 35 in 8-bit terms
 * even though the source has none. That is the signature of the Rec.709 -> P3 matrix actually running,
 * and it is what a broken presentation pass (a no-op copy, a missing matrix, the wrong transfer
 * function) fails.
 */

#include <gpu/ContextGroup.h>
#include <gpu/DrawList.h>

#include <graphics/Canvas.h>
#include <graphics/Paint.h>

#include <private/gpu/ContextImpl.h>

#include <backend/Device.h>

#include <gtest/gtest.h>

#include <memory>

namespace {

using namespace graphics;

/** A context on a headless Metal/GL device, or null when the machine has no device to test with. */
std::shared_ptr<gpu::ContextImpl> makeContext(backend::Device** outDevice) {
  backend::DeviceConfiguration configuration;
  configuration.preferredBackend = backend::Backend::Null;
  configuration.allowFallback = true;
  configuration.appName = "coral-presentation-test";
  backend::Device* device = nullptr;
  if (!backend::isSuccess(backend::createDevice(configuration, &device)) || device == nullptr) {
    return nullptr;
  }

  auto context = std::make_shared<gpu::ContextImpl>();
  if (!backend::isSuccess(context->initialize(device, device->getInfo().backend, nullptr))) {
    delete device;
    return nullptr;
  }
  *outDevice = device;
  return context;
}

} // namespace

TEST(PresentationPass, EachOutputEncodingWritesItsOwnNumbers) {
  backend::Device* device = nullptr;
  auto context = makeContext(&device);
  if (context == nullptr) GTEST_SKIP() << "no graphics device is available";

  constexpr uint32_t kWidth = 16;
  constexpr uint32_t kHeight = 16;

  gpu::RenderTargetDescription sourceDescription;
  sourceDescription.width = kWidth;
  sourceDescription.height = kHeight;
  sourceDescription.format = backend::PixelFormat::R8G8B8A8_UNORM;
  sourceDescription.needsStencil = false;
  sourceDescription.sampled = false;
  sourceDescription.label = "presentation-test-source";
  gpu::RenderTarget source;
  ASSERT_TRUE(backend::isSuccess(context->createRenderTarget(sourceDescription, &source)));

  auto list = std::make_shared<gpu::DrawList>();
  auto canvas = Canvas::createRecordingBackend(static_cast<int>(kWidth), static_cast<int>(kHeight),
          context, list);
  ASSERT_NE(canvas, nullptr);
  Paint red;
  red.color = Color::MakeRGBA8(255, 0, 0, 255);
  canvas->drawRect(Rect::MakeLTRB(0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight)), red);

  ASSERT_TRUE(backend::isSuccess(context->submit(*list, source, {}, backend::LoadOp::Clear)));
  ASSERT_TRUE(backend::isSuccess(context->waitFrameSlot(0)));

  struct Case {
    backend::SwapchainColorSpace space;
    int red;
    int green;
    int blue;
    const char* description;
  };
  // sRGB red -> linear Rec.709 (1, 0, 0). Display P3 gamma-encodes the P3 primaries; extended linear
  // writes the same primaries without the curve; PQ converts to BT.2020, scales by 203 cd/m^2 diffuse
  // white and applies the PQ curve. A no-op copy would be (255, 0, 0) in every row.
  const Case cases[] = {
    { backend::SwapchainColorSpace::DisplayP3, 234, 51, 35, "Display P3 gamma" },
    { backend::SwapchainColorSpace::ExtendedLinearP3, 210, 8, 4, "extended-linear P3" },
    { backend::SwapchainColorSpace::Hdr10Pq, 132, 81, 53, "HDR10 PQ" },
  };

  for (const Case& entry: cases) {
    gpu::RenderTargetDescription targetDescription = sourceDescription;
    targetDescription.label = "presentation-test-target";
    gpu::RenderTarget target;
    ASSERT_TRUE(backend::isSuccess(context->createRenderTarget(targetDescription, &target)));

    gpu::PresentationParameters parameters;
    parameters.sourceIsLinear = false;
    parameters.outputColorSpace = entry.space;
    parameters.sdrWhiteNits = 203.0f;
    ASSERT_TRUE(backend::isSuccess(context->present(source, target, parameters)))
            << entry.description << ": " << context->diagnostics();

    Bitmap pixels;
    ASSERT_TRUE(backend::isSuccess(context->readback(target, &pixels)))
            << entry.description << ": " << context->diagnostics();
    ASSERT_FALSE(pixels.isEmpty());
    const Pixmap pixmap = pixels.makePixmap();
    const uint32_t centre = pixmap.getPixelARGB32(kWidth / 2, kHeight / 2);
    const int red8 = static_cast<int>((centre >> 16) & 0xFFu);
    const int green8 = static_cast<int>((centre >> 8) & 0xFFu);
    const int blue8 = static_cast<int>(centre & 0xFFu);

    EXPECT_NEAR(red8, entry.red, 4) << entry.description;
    EXPECT_NEAR(green8, entry.green, 4) << entry.description;
    EXPECT_NEAR(blue8, entry.blue, 4) << entry.description;
    context->destroyRenderTarget(target);
  }

  context->destroyRenderTarget(source);
  // The canvas holds a shared_ptr to the context, so it has to go before the context does -- otherwise
  // `context.reset()` only drops one reference and the device is deleted under a live renderer.
  canvas.reset();
  list.reset();
  context.reset();
  delete device;
}
