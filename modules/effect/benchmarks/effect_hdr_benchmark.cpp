// HDR conversion microbenchmark: VideoFrameInputNode -> copy pass -> readback.
//
// Every case warms up the graph and compiles its programs before timing, then measures
// submit + execute + readback. The readback wait is deliberate: it makes the GPU path
// measurable without presenting to a surface.

#include <benchmark/benchmark.h>

#include <effect/EffectEngine.h>
#include <effect/EffectPassNode.h>
#include <effect/ReadbackOutputNode.h>
#include <effect/VideoFrameInputNode.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

struct Fixture {
  std::shared_ptr<coral::EffectEngine> engine;
  std::shared_ptr<coral::VideoFrameInputNode> input;
  std::shared_ptr<coral::ReadbackOutputNode> output;
  std::vector<uint8_t> y;
  std::vector<uint8_t> u;
  std::vector<uint8_t> v;
  std::vector<uint8_t> p010Y;
  std::vector<uint8_t> p010U;
  std::vector<uint8_t> p010V;
  coral::VideoFrameDesc frame;
  uint32_t width = 0;
  uint32_t height = 0;
};

void fillFixture(Fixture& fixture, const uint32_t width, const uint32_t p010, const bool gpu,
                 const bool hdr) {
  fixture.width = width;
  fixture.height = width * 9 / 16;
  const uint32_t w = fixture.width;
  const uint32_t h = fixture.height;
  const uint32_t cw = (w + 1) / 2;
  const uint32_t ch = (h + 1) / 2;
  fixture.y.resize(static_cast<size_t>(w) * h);
  fixture.u.resize(static_cast<size_t>(cw) * ch);
  fixture.v.resize(static_cast<size_t>(cw) * ch);
  for (uint32_t row = 0; row < h; ++row) {
    for (uint32_t col = 0; col < w; ++col) {
      fixture.y[static_cast<size_t>(row) * w + col] = static_cast<uint8_t>((col * 3 + row * 5) & 0xFF);
    }
  }
  for (uint32_t row = 0; row < ch; ++row) {
    for (uint32_t col = 0; col < cw; ++col) {
      fixture.u[static_cast<size_t>(row) * cw + col] = static_cast<uint8_t>((col * 7 + row * 3 + 64) & 0xFF);
      fixture.v[static_cast<size_t>(row) * cw + col] = static_cast<uint8_t>((col * 5 + row * 9 + 128) & 0xFF);
    }
  }
  if (p010 != 0) {
    fixture.p010Y.resize(fixture.y.size() * 2);
    fixture.p010U.resize(fixture.u.size() * 2);
    fixture.p010V.resize(fixture.v.size() * 2);
    const auto expand = [](const std::vector<uint8_t>& source, std::vector<uint8_t>& target) {
      for (size_t i = 0; i < source.size(); ++i) {
        const uint16_t value = static_cast<uint16_t>(source[i] * 4) << 6;
        target[i * 2] = static_cast<uint8_t>(value & 0xFF);
        target[i * 2 + 1] = static_cast<uint8_t>(value >> 8);
      }
    };
    expand(fixture.y, fixture.p010Y);
    expand(fixture.u, fixture.p010U);
    expand(fixture.v, fixture.p010V);
  }

  coral::EffectEngineConfig config;
  config.backend = coral::EffectBackend::Default;
  config.renderWidth = w;
  config.renderHeight = h;
  config.renderMode = coral::EffectRenderMode::Manual;
  config.maxFramesInFlight = 2;
  fixture.engine = coral::EffectEngine::create(config);
  fixture.input = coral::VideoFrameInputNode::create(w, h);
  const auto pass = fixture.engine != nullptr ? fixture.engine->createEffectNode("hdr-benchmark") : nullptr;
  fixture.output = coral::ReadbackOutputNode::create();
  fixture.input->setGpuConversion(gpu);
  fixture.input->setHdrToneMap(coral::EffectToneMapMode::Reinhard, 1.0f, 100.0f);
  coral::EffectShaderDesc shader;
  shader.name = "benchmark/hdr-copy";
  shader.fragmentSource = "  return texture(uInput0, uv);";
  pass->setShader(shader);
  fixture.engine->attach(fixture.input);
  fixture.engine->attach(fixture.output);
  fixture.engine->connect(fixture.input, pass);
  fixture.engine->connect(pass, fixture.output);
  fixture.engine->setOutput(fixture.output);
  coral::EffectOutputConfig outputConfig;
  outputConfig.width = w;
  outputConfig.height = h;
  outputConfig.format = coral::EffectPixelFormat::RGBA8;
  fixture.output->configure(outputConfig);

  fixture.frame.width = w;
  fixture.frame.height = h;
  fixture.frame.format = coral::EffectPixelFormat::YUV420;
  fixture.frame.bt709 = false;
  fixture.frame.fullRange = false;
  fixture.frame.p010 = p010 != 0;
  fixture.frame.hdr = hdr;
  fixture.frame.hdrTransfer = coral::EffectHdrTransfer::HLG;
  if (p010 != 0) {
    fixture.frame.planes[0] = { fixture.p010Y.data(), w * 2 };
    fixture.frame.planes[1] = { fixture.p010U.data(), cw * 2 };
    fixture.frame.planes[2] = { fixture.p010V.data(), cw * 2 };
  } else {
    fixture.frame.planes[0] = { fixture.y.data(), w };
    fixture.frame.planes[1] = { fixture.u.data(), cw };
    fixture.frame.planes[2] = { fixture.v.data(), cw };
  }
  fixture.frame.planeCount = 3;
  fixture.frame.timestamp = coral::RenderTime::fromTicks(0, 60);
}

void runHdr(benchmark::State& state) {
  const uint32_t width = static_cast<uint32_t>(state.range(0));
  const bool p010 = state.range(1) != 0;
  const bool gpu = state.range(2) != 0;
  const bool hdr = state.range(3) != 0;
  Fixture fixture;
  fillFixture(fixture, width, p010 ? 1 : 0, gpu, hdr);
  if (fixture.engine == nullptr || fixture.input == nullptr || fixture.output == nullptr ||
      fixture.engine->start() != coral::EffectResult::Success) {
    state.SkipWithError("the benchmark graph could not start");
    return;
  }
  // Warm up shader/pipeline compilation and pool allocation before timing.
  for (int i = 0; i < 3; ++i) {
    fixture.input->submit(fixture.frame);
    fixture.engine->execute();
    fixture.output->waitForFrame(nullptr, nullptr);
  }
  for (auto _ : state) {
    fixture.input->submit(fixture.frame);
    fixture.engine->execute();
    fixture.output->waitForFrame(nullptr, nullptr);
  }
  fixture.engine->stop();
}

} // namespace

BENCHMARK(runHdr)
    ->ArgsProduct({ { 1280, 1920, 3840 }, { 0, 1 }, { 0, 1 }, { 0, 1 } })
    ->ArgNames({ "width", "p010", "gpu", "hdr" })
    ->Name("HDR")
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK_MAIN();
