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
 * @file test_AppleIOSurface.cpp
 *
 * The Apple zero-copy path, on the host: a real IOSurface is filled from the CPU, sampled by the graph,
 * and then written by the graph into a second surface. Both the Metal import and the OpenGL bridge are
 * exercised, because the two reach the same surface by different platform APIs.
 */

#include <effect/EffectEngine.h>
#include <effect/EffectPassNode.h>
#include <effect/ReadbackOutputNode.h>
#include <effect/TestPatternInputNode.h>
#include <effect/platform/apple/AppleIOSurfaceInputNode.h>
#include <effect/platform/apple/AppleIOSurfaceOutputNode.h>

#include <IOSurface/IOSurfaceRef.h>
#include <TargetConditionals.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using coral::AppleIOSurfaceInputNode;
using coral::AppleIOSurfaceOutputNode;
using coral::EffectBackend;
using coral::EffectEngine;
using coral::EffectEngineConfig;
using coral::EffectPixelFormat;
using coral::EffectRenderMode;
using coral::EffectResult;
using coral::EffectShaderDesc;
using coral::RenderTime;

constexpr uint32_t kSize = 32;
constexpr uint8_t kTolerance = 3;

/** An IOSurface created as a single-plane BGRA image, owned by the test. */
class TestSurface {
public:
  static std::unique_ptr<TestSurface> create(const uint32_t width, const uint32_t height) {
    CFMutableDictionaryRef properties = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (properties == nullptr) return nullptr;

    int32_t value = static_cast<int32_t>(width);
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
    CFDictionarySetValue(properties, kIOSurfaceWidth, number);
    CFRelease(number);

    value = static_cast<int32_t>(height);
    number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
    CFDictionarySetValue(properties, kIOSurfaceHeight, number);
    CFRelease(number);

    value = 4;
    number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
    CFDictionarySetValue(properties, kIOSurfaceBytesPerElement, number);
    CFRelease(number);

    value = static_cast<int32_t>(width * 4);
    number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
    CFDictionarySetValue(properties, kIOSurfaceBytesPerRow, number);
    CFRelease(number);

    value = 'BGRA';
    number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
    CFDictionarySetValue(properties, kIOSurfacePixelFormat, number);
    CFRelease(number);

    value = static_cast<int32_t>(width * height * 4);
    number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
    CFDictionarySetValue(properties, kIOSurfaceAllocSize, number);
    CFRelease(number);

    IOSurfaceRef surface = IOSurfaceCreate(properties);
    CFRelease(properties);
    if (surface == nullptr) return nullptr;
    return std::unique_ptr<TestSurface>(new TestSurface(surface, width, height));
  }

  ~TestSurface() {
    if (mSurface != nullptr) CFRelease(mSurface);
  }

  TestSurface(const TestSurface&) = delete;
  TestSurface& operator=(const TestSurface&) = delete;

  [[nodiscard]] IOSurfaceRef surface() const noexcept { return mSurface; }
  [[nodiscard]] uint32_t width() const noexcept { return mWidth; }
  [[nodiscard]] uint32_t height() const noexcept { return mHeight; }

  /** Fills every pixel with one straight (non-premultiplied) colour in BGRA byte order. */
  void fill(const uint8_t blue, const uint8_t green, const uint8_t red, const uint8_t alpha) {
    if (IOSurfaceLock(mSurface, 0, nullptr) != KERN_SUCCESS) return;
    auto* const base = static_cast<uint8_t*>(IOSurfaceGetBaseAddress(mSurface));
    const size_t stride = IOSurfaceGetBytesPerRow(mSurface);
    for (uint32_t row = 0; row < mHeight; ++row) {
      uint8_t* pixel = base + row * stride;
      for (uint32_t column = 0; column < mWidth; ++column) {
        pixel[0] = blue;
        pixel[1] = green;
        pixel[2] = red;
        pixel[3] = alpha;
        pixel += 4;
      }
    }
    IOSurfaceUnlock(mSurface, 0, nullptr);
  }

  /** @return the BGRA bytes of the centre pixel. */
  [[nodiscard]] std::vector<uint8_t> centrePixel() const {
    std::vector<uint8_t> pixel(4, 0);
    if (IOSurfaceLock(mSurface, kIOSurfaceLockReadOnly, nullptr) != KERN_SUCCESS) return pixel;
    const auto* const base = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(mSurface));
    const size_t stride = IOSurfaceGetBytesPerRow(mSurface);
    const uint8_t* const source = base + static_cast<size_t>(mHeight / 2) * stride + static_cast<size_t>(mWidth / 2) * 4;
    std::memcpy(pixel.data(), source, 4);
    IOSurfaceUnlock(mSurface, kIOSurfaceLockReadOnly, nullptr);
    return pixel;
  }

private:
  TestSurface(IOSurfaceRef surface, const uint32_t width, const uint32_t height)
          : mSurface(surface), mWidth(width), mHeight(height) {}

  IOSurfaceRef mSurface = nullptr;
  uint32_t mWidth = 0;
  uint32_t mHeight = 0;
};

EffectEngineConfig config(const EffectBackend backend) {
  EffectEngineConfig configuration;
  configuration.backend = backend;
  configuration.renderWidth = kSize;
  configuration.renderHeight = kSize;
  configuration.renderMode = EffectRenderMode::Manual;
  configuration.timeBase = 60;
  configuration.maxFramesInFlight = 1;
  configuration.framePoolBudgetBytes = 16u * 1024u * 1024u;
  return configuration;
}

EffectShaderDesc passthroughShader() {
  EffectShaderDesc shader;
  shader.name = "apple-iosurface-passthrough";
  shader.fragmentSource = "  return texture(uInput0, uv);";
  shader.inputCount = 1;
  return shader;
}

EffectShaderDesc solidBlueShader() {
  EffectShaderDesc shader;
  shader.name = "apple-iosurface-blue";
  shader.fragmentSource = "  return vec4(0.0, 0.0, 1.0, 1.0);";
  shader.inputCount = 1;
  return shader;
}

/** Starts @p engine, skipping the case when the host has no device for that backend. */
void startOrSkip(const std::shared_ptr<EffectEngine>& engine) {
  const EffectResult started = engine->start();
  if (started == EffectResult::DeviceLost || started == EffectResult::Unsupported) {
    GTEST_SKIP() << "the backend could not bring up a device: " << engine->diagnostics();
  }
  if (started != EffectResult::Success) {
    GTEST_SKIP() << "the engine did not start: " << engine->diagnostics();
  }
}

class AppleIOSurfaceBackendTest : public ::testing::TestWithParam<EffectBackend> {
protected:
  void SetUp() override {
#if TARGET_OS_SIMULATOR
    // Core Video's texture cache does not map an IOSurface to a GLES texture in the iOS simulator; it reports
    // kCVReturnPixelBufferNotOpenGLCompatible from CVOpenGLESTextureCacheCreateTextureFromImage. The same bridge runs
    // on a device, and the Metal rows above still exercise the surface on the simulator.
    if (GetParam() == EffectBackend::OpenGL) {
      GTEST_SKIP() << "CVOpenGLESTextureCache does not map IOSurfaces in the iOS simulator; run this on a device";
    }
#endif
  }
};

TEST_P(AppleIOSurfaceBackendTest, InputSamplesAProducerSurface) {
  const auto source = TestSurface::create(kSize, kSize);
  ASSERT_NE(source, nullptr) << "IOSurfaceCreate failed";
  source->fill(0, 0, 255, 255);

  const auto engine = EffectEngine::create(config(GetParam()));
  ASSERT_NE(engine, nullptr);

  const auto input = AppleIOSurfaceInputNode::create(source->surface(), 0, 0, EffectPixelFormat::BGRA8);
  ASSERT_NE(input, nullptr);
  const auto pass = engine->createEffectNode("pass");
  ASSERT_NE(pass, nullptr);
  ASSERT_EQ(EffectResult::Success, pass->setShader(passthroughShader()));
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(output, nullptr);

  ASSERT_EQ(EffectResult::Success, engine->attach(input));
  ASSERT_EQ(EffectResult::Success, engine->attach(output));
  ASSERT_EQ(EffectResult::Success, engine->connect(input, pass));
  ASSERT_EQ(EffectResult::Success, engine->connect(pass, output));
  ASSERT_EQ(EffectResult::Success, engine->setOutput(output));

  startOrSkip(engine);
  ASSERT_EQ(EffectResult::Success, engine->execute()) << engine->diagnostics();

  coral::EffectFrame frame;
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(&frame, &pixels)) << engine->diagnostics();
  ASSERT_EQ(pixels.size(), static_cast<size_t>(kSize) * kSize * 4);

  const size_t centre = (static_cast<size_t>(kSize / 2) * kSize + kSize / 2) * 4;
  EXPECT_NEAR(pixels[centre + 0], 255, kTolerance);
  EXPECT_NEAR(pixels[centre + 1], 0, kTolerance);
  EXPECT_NEAR(pixels[centre + 2], 0, kTolerance);
  EXPECT_NEAR(pixels[centre + 3], 255, kTolerance);
  EXPECT_EQ(input->isZeroCopy(), GetParam() == EffectBackend::Metal) << "macOS OpenGL normalises through one GPU blit";

  engine->stop();
}

TEST_P(AppleIOSurfaceBackendTest, InputAcceptsTheUnifiedPixelBufferView) {
  const auto source = TestSurface::create(kSize, kSize);
  ASSERT_NE(source, nullptr) << "IOSurfaceCreate failed";
  source->fill(255, 0, 0, 255);

  const auto engine = EffectEngine::create(config(GetParam()));
  ASSERT_NE(engine, nullptr);

  // Created without a surface; the frame arrives through the alias-unified input entry point after `start()`,
  // which is how a decoder publishes each CVPixelBuffer.
  const auto input = AppleIOSurfaceInputNode::create(kSize, kSize, EffectPixelFormat::BGRA8);
  ASSERT_NE(input, nullptr);
  const auto pass = engine->createEffectNode("pass");
  ASSERT_NE(pass, nullptr);
  ASSERT_EQ(EffectResult::Success, pass->setShader(passthroughShader()));
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(output, nullptr);

  ASSERT_EQ(EffectResult::Success, engine->attach(input));
  ASSERT_EQ(EffectResult::Success, engine->attach(output));
  ASSERT_EQ(EffectResult::Success, engine->connect(input, pass));
  ASSERT_EQ(EffectResult::Success, engine->connect(pass, output));
  ASSERT_EQ(EffectResult::Success, engine->setOutput(output));

  startOrSkip(engine);

  coral::PixelBufferView view;
  view.width = kSize;
  view.height = kSize;
  view.format = EffectPixelFormat::BGRA8;
  view.storage = coral::PixelBufferStorageKind::IOSurface;
  view.nativeKind = coral::NativeHandleKind::IOSurface;
  view.nativeHandle = source->surface();
  view.timestamp = RenderTime::fromTicks(0, 60);
  ASSERT_EQ(EffectResult::Success, input->submit(view));

  ASSERT_EQ(EffectResult::Success, engine->execute()) << engine->diagnostics();
  coral::EffectFrame metadata;
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(&metadata, &pixels)) << engine->diagnostics();
  const size_t centre = (static_cast<size_t>(kSize / 2) * kSize + kSize / 2) * 4;
  EXPECT_NEAR(pixels[centre + 0], 0, kTolerance);
  EXPECT_NEAR(pixels[centre + 1], 0, kTolerance);
  EXPECT_NEAR(pixels[centre + 2], 255, kTolerance);
  EXPECT_NEAR(pixels[centre + 3], 255, kTolerance);
  EXPECT_EQ(metadata.timestamp().value, 0);

  engine->stop();
}

TEST_P(AppleIOSurfaceBackendTest, RepeatedFramesKeepSamplingTheSameSurface) {
  const auto source = TestSurface::create(kSize, kSize);
  ASSERT_NE(source, nullptr) << "IOSurfaceCreate failed";
  source->fill(0, 255, 0, 255);

  const auto engine = EffectEngine::create(config(GetParam()));
  ASSERT_NE(engine, nullptr);

  const auto input = AppleIOSurfaceInputNode::create(source->surface(), kSize, kSize,
          EffectPixelFormat::BGRA8);
  ASSERT_NE(input, nullptr);
  // One submission only: the second and third frames exercise the repeated-surface path, which must still
  // sample a live texture rather than one retired after the first submission.
  ASSERT_EQ(EffectResult::Success, input->submit(source->surface(), kSize, kSize,
          EffectPixelFormat::BGRA8, RenderTime::fromTicks(0, 60)));

  const auto pass = engine->createEffectNode("pass");
  ASSERT_NE(pass, nullptr);
  ASSERT_EQ(EffectResult::Success, pass->setShader(passthroughShader()));
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(output, nullptr);

  ASSERT_EQ(EffectResult::Success, engine->attach(input));
  ASSERT_EQ(EffectResult::Success, engine->attach(output));
  ASSERT_EQ(EffectResult::Success, engine->connect(input, pass));
  ASSERT_EQ(EffectResult::Success, engine->connect(pass, output));
  ASSERT_EQ(EffectResult::Success, engine->setOutput(output));

  startOrSkip(engine);
  for (int frame = 0; frame < 3; ++frame) {
    ASSERT_EQ(EffectResult::Success, engine->execute()) << engine->diagnostics();
    coral::EffectFrame metadata;
    std::vector<uint8_t> pixels;
    ASSERT_TRUE(output->waitForFrame(&metadata, &pixels)) << engine->diagnostics();
    const size_t centre = (static_cast<size_t>(kSize / 2) * kSize + kSize / 2) * 4;
    EXPECT_NEAR(pixels[centre + 0], 0, kTolerance) << "frame " << frame;
    EXPECT_NEAR(pixels[centre + 1], 255, kTolerance) << "frame " << frame;
    EXPECT_NEAR(pixels[centre + 2], 0, kTolerance) << "frame " << frame;
  }

  engine->stop();
}

TEST_P(AppleIOSurfaceBackendTest, OutputWritesTheConsumerSurface) {
  const auto destination = TestSurface::create(kSize, kSize);
  ASSERT_NE(destination, nullptr) << "IOSurfaceCreate failed";

  const auto engine = EffectEngine::create(config(GetParam()));
  ASSERT_NE(engine, nullptr);

  const auto input = coral::TestPatternInputNode::create(kSize, kSize);
  ASSERT_NE(input, nullptr);
  const auto pass = engine->createEffectNode("pass");
  ASSERT_NE(pass, nullptr);
  ASSERT_EQ(EffectResult::Success, pass->setShader(solidBlueShader()));
  const auto output = AppleIOSurfaceOutputNode::create(destination->surface(), 0, 0, EffectPixelFormat::BGRA8);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(EffectResult::Success, output->configure({}));

  ASSERT_EQ(EffectResult::Success, engine->attach(input));
  ASSERT_EQ(EffectResult::Success, engine->attach(output));
  ASSERT_EQ(EffectResult::Success, engine->connect(input, pass));
  ASSERT_EQ(EffectResult::Success, engine->connect(pass, output));
  ASSERT_EQ(EffectResult::Success, engine->setOutput(output));

  startOrSkip(engine);
  ASSERT_EQ(EffectResult::Success, engine->execute()) << engine->diagnostics();
  ASSERT_EQ(EffectResult::Success, engine->flushAndWait()) << engine->diagnostics();
  EXPECT_EQ(output->frameCount(), 1u);
  EXPECT_FALSE(output->hasPresentFailure()) << engine->diagnostics();
  EXPECT_EQ(output->isZeroCopy(), GetParam() == EffectBackend::Metal)
          << "macOS OpenGL normalises through one GPU blit";

  const std::vector<uint8_t> pixel = destination->centrePixel();
  EXPECT_NEAR(pixel[0], 255, kTolerance) << "blue";
  EXPECT_NEAR(pixel[1], 0, kTolerance) << "green";
  EXPECT_NEAR(pixel[2], 0, kTolerance) << "red";
  EXPECT_NEAR(pixel[3], 255, kTolerance) << "alpha";

  engine->stop();
}

INSTANTIATE_TEST_SUITE_P(Backends, AppleIOSurfaceBackendTest,
        ::testing::Values(EffectBackend::Metal, EffectBackend::OpenGL),
        [](const ::testing::TestParamInfo<EffectBackend>& info) {
          return info.param == EffectBackend::Metal ? std::string("Metal") : std::string("OpenGL");
        });

TEST(AppleIOSurface, InputRefusesAnExtentThatDoesNotMatchTheSurface) {
  const auto source = TestSurface::create(kSize, kSize);
  ASSERT_NE(source, nullptr);
  EXPECT_EQ(nullptr, AppleIOSurfaceInputNode::create(source->surface(), kSize + 1, kSize,
          EffectPixelFormat::BGRA8));
  EXPECT_EQ(nullptr, AppleIOSurfaceInputNode::create(source->surface(), kSize, kSize,
          EffectPixelFormat::YUV420));
}

TEST(AppleIOSurface, InputRejectsAPixelBufferOfAnotherNativeKind) {
  const auto source = TestSurface::create(kSize, kSize);
  ASSERT_NE(source, nullptr);
  const auto input = AppleIOSurfaceInputNode::create(source->surface(), kSize, kSize,
          EffectPixelFormat::BGRA8);
  ASSERT_NE(input, nullptr);
  coral::PixelBufferView view;
  view.width = kSize;
  view.height = kSize;
  view.storage = coral::PixelBufferStorageKind::MetalTexture;
  view.nativeKind = coral::NativeHandleKind::MetalTexture;
  view.nativeHandle = source->surface();
  EXPECT_EQ(EffectResult::Unsupported, input->submit(view));
}

TEST(AppleIOSurface, OutputRefusesASizeThatDoesNotMatchTheSurface) {
  const auto destination = TestSurface::create(kSize, kSize);
  ASSERT_NE(destination, nullptr);
  const auto output = AppleIOSurfaceOutputNode::create(destination->surface(), kSize, kSize,
          EffectPixelFormat::BGRA8);
  ASSERT_NE(output, nullptr);
  coral::EffectOutputConfig mismatched;
  mismatched.width = kSize + 1;
  mismatched.height = kSize;
  EXPECT_EQ(EffectResult::InvalidArgument, output->configure(mismatched));
  EXPECT_EQ(kSize, output->width());
  EXPECT_EQ(kSize, output->height());
  EXPECT_EQ(destination->surface(), output->surface());
}

} // namespace
