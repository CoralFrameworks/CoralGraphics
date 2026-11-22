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

#include "EffectVerification.h"

#include <effect/EffectPassNode.h>
#include <effect/ReadbackOutputNode.h>
#include <effect/TestPatternInputNode.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace effect_verification {
namespace {

constexpr uint32_t kWidth = 320;
constexpr uint32_t kHeight = 240;

/** The eight bars `TestPatternInputNode` draws, left to right. */
constexpr uint8_t kBarColors[8][3] = {
  { 255, 255, 255 }, // white
  { 255, 255, 0 },   // yellow
  { 0, 255, 255 },   // cyan
  { 0, 255, 0 },     // green
  { 255, 0, 255 },   // magenta
  { 255, 0, 0 },     // red
  { 0, 0, 255 },     // blue
  { 0, 0, 0 },       // black
};

/** @return the device line a bug report wants: API, device, backend and implementation. */
std::string describeDevice(const backend::Device& device) {
  const backend::DeviceInfo& info = device.getInfo();
  std::string line = info.backendName != nullptr ? info.backendName : "unknown backend";
  line += " / ";
  line += info.deviceName != nullptr ? info.deviceName : "unknown device";
  line += " / ";
  line += info.apiVersion != nullptr ? info.apiVersion : "unknown API";
  return line;
}

/** Maps the engine's public backend selector onto the RHI's, without needing an engine instance. */
backend::Backend rhiBackendOf(const coral::EffectBackend backend) noexcept {
  switch (backend) {
    case coral::EffectBackend::Default:
      return backend::Backend::Null;
    case coral::EffectBackend::OpenGL:
      return backend::Backend::OpenGL;
    case coral::EffectBackend::Vulkan:
      return backend::Backend::Vulkan;
    case coral::EffectBackend::Metal:
      return backend::Backend::Metal;
  }
  return backend::Backend::Null;
}

} // namespace anonymous

const char* backendName(const backend::Backend backend) noexcept {
  return backend::toString(backend);
}

Run runCopyGraph(const coral::EffectBackend requested) {
  Run run;

  const backend::Backend wanted = rhiBackendOf(requested);
  backend::DeviceConfiguration deviceConfiguration;
  deviceConfiguration.preferredBackend = wanted;
  // A pinned backend must not fall back: the whole point of `--backend metal` is to fail when Metal cannot be used
  // rather than to verify whichever backend answered. `Default` keeps the platform's order and may fall back.
  deviceConfiguration.allowFallback = wanted == backend::Backend::Null;
  deviceConfiguration.appName = "coral-effect-verification";

  backend::Device* device = nullptr;
  const backend::Result created = backend::createDevice(deviceConfiguration, &device);
  if (!backend::isSuccess(created) || device == nullptr) {
    run.deviceUnavailable = true;
    run.error = std::string("no device for ") + backend::toString(wanted) + " (result " +
                std::to_string(static_cast<int>(created)) + ')';
    return run;
  }
  // The engine borrows the device (its shared_ptr has a no-op deleter), so this owner outlives it: the engine is
  // declared after it, and therefore destroyed before it.
  std::unique_ptr<backend::Device> deviceOwner(device);

  coral::EffectEngineConfig config;
  config.renderWidth = kWidth;
  config.renderHeight = kHeight;
  config.renderMode = coral::EffectRenderMode::Manual;
  config.timeBase = 60;
  config.maxFramesInFlight = 2;
  config.externalDevice = deviceOwner.get();

  const std::shared_ptr<coral::EffectEngine> engine = coral::EffectEngine::create(config);
  if (engine == nullptr) {
    run.error = "the effect engine could not be created";
    return run;
  }

  const auto input = coral::TestPatternInputNode::create(kWidth, kHeight);
  const auto pass = engine->createEffectNode("copy");
  const auto output = coral::ReadbackOutputNode::create();
  if (input == nullptr || pass == nullptr || output == nullptr) {
    run.error = "the graph could not be built";
    return run;
  }

  coral::EffectShaderDesc shader;
  shader.name = "copy";
  shader.fragmentSource = "  return texture(uInput0, uv);";
  shader.inputCount = 1;
  if (pass->setShader(shader) != coral::EffectResult::Success) {
    run.error = "the shader was refused: " + pass->diagnostics();
    return run;
  }

  if (engine->attach(input) != coral::EffectResult::Success ||
      engine->attach(output) != coral::EffectResult::Success) {
    run.error = "the nodes could not be attached";
    return run;
  }
  if (engine->connect(input, pass) != coral::EffectResult::Success ||
      engine->connect(pass, output) != coral::EffectResult::Success ||
      engine->setOutput(output) != coral::EffectResult::Success) {
    run.error = "the graph could not be connected: " + engine->diagnostics();
    return run;
  }

  const coral::EffectResult started = engine->start();
  if (started == coral::EffectResult::DeviceLost) {
    // No GPU for this backend. The caller decides whether that is a skip or a failure (a pinned backend is a failure).
    run.deviceUnavailable = true;
    run.error = engine->diagnostics();
    return run;
  }
  if (started != coral::EffectResult::Success) {
    run.error = "the engine did not start: " + engine->diagnostics();
    return run;
  }

  backend::Device* engineDevice = engine->device();
  if (engineDevice == nullptr) {
    run.error = "the engine started without a device";
    return run;
  }
  run.backend = engineDevice->getInfo().backend;
  run.deviceLine = describeDevice(*engineDevice);

  if (engine->execute() != coral::EffectResult::Success) {
    run.error = "the frame did not render: " + engine->diagnostics();
    engine->stop();
    return run;
  }

  coral::EffectFrame frame;
  std::vector<uint8_t> pixels;
  if (!output->waitForFrame(&frame, &pixels)) {
    run.error = "the frame was not read back";
    engine->stop();
    return run;
  }

  run.width = frame.width();
  run.height = frame.height();
  run.pixels = std::move(pixels);
  run.ok = true;
  engine->stop();
  return run;
}

bool verifyColorBars(const Run& run, std::string& error) {
  if (!run.ok) {
    error = run.error.empty() ? "the run did not produce a frame" : run.error;
    return false;
  }
  if (run.width == 0 || run.height == 0) {
    error = "the frame has no extent";
    return false;
  }
  const size_t expectedBytes = static_cast<size_t>(run.width) * run.height * 4;
  if (run.pixels.size() != expectedBytes) {
    error = "the readback is " + std::to_string(run.pixels.size()) + " bytes, expected " +
            std::to_string(expectedBytes);
    return false;
  }

  const uint32_t y = run.height / 2;
  for (uint32_t bar = 0; bar < 8; ++bar) {
    // A point in the middle of the bar: the pattern assigns a texel to bar i when floor(x * 8 / width) == i, so the
    // exact centres of the eight bars are at (2i + 1) * width / 16.
    const uint32_t x = (2 * bar + 1) * run.width / 16;
    const size_t offset = (static_cast<size_t>(y) * run.width + x) * 4;
    const uint8_t* pixel = run.pixels.data() + offset;
    for (uint32_t channel = 0; channel < 3; ++channel) {
      const int actual = pixel[channel];
      const int expected = kBarColors[bar][channel];
      if (std::abs(actual - expected) > 2) {
        error = "bar " + std::to_string(bar) + " at (" + std::to_string(x) + ',' + std::to_string(y) +
                ") is rgba(" + std::to_string(pixel[0]) + ',' + std::to_string(pixel[1]) + ',' +
                std::to_string(pixel[2]) + ',' + std::to_string(pixel[3]) + "), expected channel " +
                std::to_string(channel) + " = " + std::to_string(expected);
        return false;
      }
    }
    // The pattern is opaque and the copy pass must not introduce alpha.
    if (std::abs(static_cast<int>(pixel[3]) - 255) > 2) {
      error = "bar " + std::to_string(bar) + " is not opaque (alpha " + std::to_string(pixel[3]) + ')';
      return false;
    }
  }
  return true;
}

bool verifySameImage(const Run& expected, const Run& actual, std::string& error, const uint32_t tolerance) {
  if (!expected.ok || !actual.ok) {
    error = "a run did not produce a frame";
    return false;
  }
  if (expected.width != actual.width || expected.height != actual.height) {
    error = "the frames have different extents (" + std::to_string(expected.width) + 'x' +
            std::to_string(expected.height) + " vs " + std::to_string(actual.width) + 'x' +
            std::to_string(actual.height) + ')';
    return false;
  }
  if (expected.pixels.size() != actual.pixels.size()) {
    error = "the readbacks have different sizes";
    return false;
  }

  for (size_t index = 0; index < expected.pixels.size(); ++index) {
    const int difference = std::abs(static_cast<int>(expected.pixels[index]) - static_cast<int>(actual.pixels[index]));
    if (difference > static_cast<int>(tolerance)) {
      const size_t pixel = index / 4;
      const uint32_t x = static_cast<uint32_t>(pixel % expected.width);
      const uint32_t y = static_cast<uint32_t>(pixel / expected.width);
      error = "the images differ at (" + std::to_string(x) + ',' + std::to_string(y) + ") channel " +
              std::to_string(index % 4) + ": " + std::to_string(expected.pixels[index]) + " vs " +
              std::to_string(actual.pixels[index]);
      return false;
    }
  }
  return true;
}

} // namespace effect_verification
