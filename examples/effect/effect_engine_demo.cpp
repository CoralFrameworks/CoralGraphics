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
 * @file effect_engine_demo.cpp
 *
 * A minimal client: build `input -> pass -> readback`, render one frame manually, and print what came
 * back. It is deliberately the smallest program that uses every public layer - graph, shader snapshot,
 * render thread, frame pool - and it is also the check that the installed headers are usable on their own.
 *
 * Exit code 77 means "no GPU on this machine", which is how CTest records a skip rather than a failure.
 */

#include <effect/EffectEngine.h>
#include <effect/EffectPassNode.h>
#include <effect/ReadbackOutputNode.h>
#include <effect/TestPatternInputNode.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

int main() {
  coral::EffectEngineConfig config;
  config.renderWidth = 320;
  config.renderHeight = 240;
  config.renderMode = coral::EffectRenderMode::Manual;
  config.timeBase = 60;
  config.maxFramesInFlight = 2;

  const std::shared_ptr<coral::EffectEngine> engine = coral::EffectEngine::create(config);
  if (engine == nullptr) {
    std::fprintf(stderr, "the effect engine could not be created\n");
    return 1;
  }

  const auto input = coral::TestPatternInputNode::create(config.renderWidth, config.renderHeight);
  const auto pass = engine->createEffectNode("copy");
  const auto output = coral::ReadbackOutputNode::create();
  if (input == nullptr || pass == nullptr || output == nullptr) {
    std::fprintf(stderr, "the graph could not be built\n");
    return 1;
  }

  coral::EffectShaderDesc shader;
  shader.name = "copy";
  shader.fragmentSource = "  return texture(uInput0, uv);";
  shader.inputCount = 1;
  if (pass->setShader(shader) != coral::EffectResult::Success) {
    std::fprintf(stderr, "the shader was refused: %s\n", pass->diagnostics().c_str());
    return 1;
  }

  if (engine->attach(input) != coral::EffectResult::Success ||
      engine->attach(output) != coral::EffectResult::Success) {
    std::fprintf(stderr, "the nodes could not be attached\n");
    return 1;
  }
  if (engine->connect(input, pass) != coral::EffectResult::Success ||
      engine->connect(pass, output) != coral::EffectResult::Success ||
      engine->setOutput(output) != coral::EffectResult::Success) {
    std::fprintf(stderr, "the graph could not be connected: %s\n", engine->diagnostics().c_str());
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

  if (engine->execute() != coral::EffectResult::Success) {
    std::fprintf(stderr, "the frame did not render: %s\n", engine->diagnostics().c_str());
    engine->stop();
    return 1;
  }

  coral::EffectFrame frame;
  std::vector<uint8_t> pixels;
  if (!output->waitForFrame(&frame, &pixels)) {
    std::fprintf(stderr, "the frame was not read back\n");
    engine->stop();
    return 1;
  }

  const size_t centre = (static_cast<size_t>(frame.height() / 2) * frame.width() + frame.width() / 2) * 4;
  const coral::EffectEngineStats stats = engine->stats();
  std::printf("frame %llu: %ux%u, centre rgba=(%u,%u,%u,%u)\n", static_cast<unsigned long long>(frame.id()),
              frame.width(), frame.height(), pixels[centre + 0], pixels[centre + 1], pixels[centre + 2],
              pixels[centre + 3]);
  std::printf("frames=%llu programs=%u pool hits/misses=%llu/%llu\n",
              static_cast<unsigned long long>(stats.frameCount), stats.programCount,
              static_cast<unsigned long long>(stats.framePoolHits),
              static_cast<unsigned long long>(stats.framePoolMisses));

  engine->stop();
  return 0;
}
