// Headless EffectEngine benchmark: TestPattern -> pass -> ReadbackOutputNode.
//
// Usage: effect_benchmark [--frames N] [--width W] [--height H] [--backend auto|gl|vulkan] [--switches N] [--async] [--async-pipeline] [--settle-each] [--pipeline-cache PATH]
//                        [--switches N] [--async] [--settle-each]
//
// `--switches N` replaces the pass shader at N points during the run. `--async` enables the M9 worker
// compile path; `--settle-each` waits (by executing frames) until each switch is ready, which makes a
// sync-vs-async responsiveness comparison where both runs apply the same programs.

#include <effect/EffectEngine.h>
#include <effect/EffectPassNode.h>
#include <effect/ReadbackOutputNode.h>
#include <effect/TestPatternInputNode.h>

// The device-ready callback hands out the RHI device, so the benchmark can report which backend actually
// came up. This is an example, not a client of the public boundary, so naming the RHI here is fine.
#include <backend/Device.h>
#include <backend/Types.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>

namespace {

bool parseUint(const char* text, uint32_t& value) {
  if (text == nullptr || *text == '\0') return false;
  const std::string_view view(text);
  const auto result = std::from_chars(view.data(), view.data() + view.size(), value);
  return result.ec == std::errc() && result.ptr == view.data() + view.size();
}

} // namespace

int main(int argc, char** argv) {
  uint32_t frames = 300;
  uint32_t width = 1280;
  uint32_t height = 720;
  uint32_t switches = 0;
  bool async = false;
  bool asyncPipeline = false;
  bool settleEach = false;
  coral::EffectBackend backend = coral::EffectBackend::Default;
  std::string pipelineCachePath;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if (option == "--async") {
      async = true;
      continue;
    }
    if (option == "--async-pipeline") {
      asyncPipeline = true;
      continue;
    }
    if (option == "--settle-each") {
      settleEach = true;
      continue;
    }
    if (option == "--pipeline-cache") {
      if (index + 1 >= argc) {
        std::fprintf(stderr, "missing value for --pipeline-cache\n");
        return 2;
      }
      pipelineCachePath = argv[++index];
      continue;
    }
    if (option == "--backend") {
      // Which API to drive. `auto` leaves the engine's own priority order in place, which is what a
      // product does; the explicit values are what makes a backend comparison on one machine possible.
      if (index + 1 >= argc) {
        std::fprintf(stderr, "missing value for --backend\n");
        return 2;
      }
      const std::string_view value(argv[++index]);
      if (value == "auto") backend = coral::EffectBackend::Default;
      else if (value == "gl") backend = coral::EffectBackend::OpenGL;
      else if (value == "vulkan") backend = coral::EffectBackend::Vulkan;
      else {
        std::fprintf(stderr, "unknown backend %s (auto|gl|vulkan)\n", argv[index]);
        return 2;
      }
      continue;
    }
    if (index + 1 >= argc) {
      std::fprintf(stderr, "missing value for %s\n", argv[index]);
      return 2;
    }
    uint32_t value = 0;
    if (option != "--frames" && option != "--width" && option != "--height" && option != "--switches") {
      std::fprintf(stderr, "unknown option %s\n", argv[index]);
      return 2;
    }
    if (!parseUint(argv[++index], value)) {
      std::fprintf(stderr, "invalid value for %s\n", argv[index - 1]);
      return 2;
    }
    if (option == "--frames") frames = value;
    else if (option == "--width") width = value;
    else if (option == "--height") height = value;
    else switches = value;
  }
  if (frames == 0 || width == 0 || height == 0) {
    std::fprintf(stderr, "frames/width/height must be non-zero\n");
    return 2;
  }

  coral::EffectEngineConfig config;
  config.backend = backend;
  config.renderWidth = width;
  config.renderHeight = height;
  config.enableGpuTiming = true;
  config.asyncShaderCompilation = async;
  config.asyncPipelineCreation = asyncPipeline;
  config.vulkanPipelineCachePath = pipelineCachePath;
  const auto engine = coral::EffectEngine::create(config);
  if (engine == nullptr) {
    std::fprintf(stderr, "EffectEngine::create failed\n");
    return 1;
  }
  // The requested backend is a preference, not a promise: `auto` follows the engine's priority order and an
  // explicit one may fall back. Recording what actually came up is what keeps a comparison table honest -
  // a run labelled "vulkan" that silently used something else would be worse than no table.
  std::string actualBackend = "unknown";
  std::string actualDevice = "unknown";
  engine->setOnDeviceReady([&actualBackend, &actualDevice](backend::Device& device) {
    const backend::DeviceInfo& info = device.getInfo();
    actualBackend = backend::toString(info.backend);
    actualDevice = info.deviceName != nullptr ? info.deviceName : "unknown";
  });
  const auto input = coral::TestPatternInputNode::create(width, height);
  const auto pass = engine->createEffectNode("benchmark");
  const auto output = coral::ReadbackOutputNode::create();
  if (input == nullptr || pass == nullptr || output == nullptr) {
    std::fprintf(stderr, "node creation failed\n");
    return 1;
  }

  coral::EffectShaderDesc shader;
  shader.name = "benchmark/invert";
  shader.fragmentSource = "  return vec4(1.0 - texture(uInput0, uv).rgb, 1.0);";
  shader.authoring = coral::EffectShaderAuthoring::Body;
  shader.inputCount = 1;
  if (pass->setShader(shader) != coral::EffectResult::Success ||
      engine->attach(input) != coral::EffectResult::Success ||
      engine->attach(output) != coral::EffectResult::Success ||
      engine->connect(input, pass) != coral::EffectResult::Success ||
      engine->connect(pass, output) != coral::EffectResult::Success ||
      engine->setOutput(output) != coral::EffectResult::Success) {
    std::fprintf(stderr, "graph setup failed: %s\n", engine->diagnostics().c_str());
    return 1;
  }

  const coral::EffectResult started = engine->start();
  if (started != coral::EffectResult::Success) {
    std::fprintf(stderr, "start failed: %s\n", engine->diagnostics().c_str());
    return started == coral::EffectResult::DeviceLost ? 77 : 1;
  }

  const auto begin = std::chrono::steady_clock::now();
  uint32_t delivered = 0;
  uint32_t executed = 0;
  uint32_t appliedSwitches = 0;
  double maxFrameMilliseconds = 0.0;
  double maxSwitchFrameMilliseconds = 0.0;
  double settleMilliseconds = 0.0;
  const uint32_t switchInterval = switches == 0 ? 0 : std::max(1u, frames / switches);

  const auto runFrame = [&]() -> bool {
    const auto frameBegin = std::chrono::steady_clock::now();
    if (engine->execute() != coral::EffectResult::Success) {
      std::fprintf(stderr, "execute failed: %s\n", engine->diagnostics().c_str());
      return false;
    }
    if (output->waitForFrame(nullptr, nullptr)) ++delivered;
    const double frameMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameBegin).count();
    maxFrameMilliseconds = std::max(maxFrameMilliseconds, frameMilliseconds);
    ++executed;
    if (executed > 1) maxSwitchFrameMilliseconds = std::max(maxSwitchFrameMilliseconds, frameMilliseconds);
    return true;
  };

  const auto settle = [&]() -> bool {
    const auto settleBegin = std::chrono::steady_clock::now();
    while (pass->shaderState() == coral::EffectShaderState::Pending) {
      if (std::chrono::steady_clock::now() - settleBegin > std::chrono::seconds(10)) {
        std::fprintf(stderr, "shader did not become ready within 10s\n");
        return false;
      }
      if (!runFrame()) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (pass->shaderState() == coral::EffectShaderState::Failed) {
      std::fprintf(stderr, "shader failed: %s\n", pass->diagnostics().c_str());
      return false;
    }
    settleMilliseconds +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - settleBegin).count();
    return true;
  };

  for (uint32_t frame = 0; frame < frames; ++frame) {
    if (switchInterval != 0 && frame != 0 && frame % switchInterval == 0) {
      const float shade = 0.2f + 0.6f * static_cast<float>(appliedSwitches % 5u) / 4.0f;
      shader.name = "benchmark/switch" + std::to_string(appliedSwitches);
      shader.fragmentSource = "  return vec4(" + std::to_string(shade) + ", 0.4, 0.6, 1.0);";
      if (pass->setShader(shader) != coral::EffectResult::Success) {
        std::fprintf(stderr, "setShader failed: %s\n", engine->diagnostics().c_str());
        engine->stop();
        return 1;
      }
      ++appliedSwitches;
      if (settleEach && !settle()) {
        engine->stop();
        return 1;
      }
    }
    if (!runFrame()) {
      engine->stop();
      return 1;
    }
  }
  if (appliedSwitches != 0 && !settle()) {
    engine->stop();
    return 1;
  }
  const auto end = std::chrono::steady_clock::now();
  const double totalMilliseconds = std::chrono::duration<double, std::milli>(end - begin).count();
  const coral::EffectEngineStats stats = engine->stats();
  const coral::EffectShaderState shaderState = pass->shaderState();
  const std::string shaderDiagnostics = pass->diagnostics();
  engine->stop();

  const double seconds = totalMilliseconds / 1000.0;
  const double fps = seconds > 0.0 ? static_cast<double>(delivered) / seconds : 0.0;
  std::printf("frames=%u delivered=%u size=%ux%u async=%d switches=%u\n", frames, delivered, width, height,
              async ? 1 : 0, appliedSwitches);
  std::printf("device=%s backend=%s\n", actualDevice.c_str(), actualBackend.c_str());
  std::printf("total_ms=%.3f fps=%.2f max_frame_ms=%.3f max_switch_frame_ms=%.3f settle_ms=%.3f cpu_ms=%.3f gpu_ms=%.3f\n",
              totalMilliseconds, fps, maxFrameMilliseconds, maxSwitchFrameMilliseconds, settleMilliseconds,
              stats.cpuMilliseconds, stats.gpuMilliseconds);
  std::printf("shader_state=%u diagnostics=%s\n", static_cast<unsigned>(shaderState), shaderDiagnostics.c_str());
  // The engine's own diagnostics, which is where a cache health warning lands. Without this a headless run on a CI
  // host says nothing about the cache at all - and the cache is exactly what a CI host can check cheaply.
  {
    const std::string engineDiagnostics = engine->diagnostics();
    if (!engineDiagnostics.empty()) std::printf("engine_diagnostics=%s\n", engineDiagnostics.c_str());
  }
  std::printf("programs=%u pipelines=%u textures=%u compile_count=%llu compile_ms=%.3f pipeline_count=%llu pipeline_ms=%.3f\n",
              stats.programCount, stats.pipelineCount, stats.textureCount,
              static_cast<unsigned long long>(stats.programCompileCount), stats.programCompileMilliseconds,
              static_cast<unsigned long long>(stats.pipelineCreationCount), stats.pipelineCreationMilliseconds);
  std::printf("pool free=%u in_use=%u in_flight=%u bytes=%llu peak=%llu\n", stats.framePoolFree,
              stats.framePoolInUse, stats.framePoolInFlight,
              static_cast<unsigned long long>(stats.framePoolBytes),
              static_cast<unsigned long long>(stats.framePoolPeakBytes));
  return delivered >= frames && shaderState != coral::EffectShaderState::Failed ? 0 : 1;
}
