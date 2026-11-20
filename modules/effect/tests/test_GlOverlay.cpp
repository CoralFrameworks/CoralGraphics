#include <gtest/gtest.h>

#include <effect/EffectEngine.h>
#include <effect/EffectPassNode.h>
#include <effect/EffectTexture.h>
#include <effect/ReadbackOutputNode.h>
#include <effect/TestPatternInputNode.h>
#include <effect/platform/gl/GlExternalTexture.h>
#include <backend/Device.h>

#include <windows.h>
#include <GL/gl.h>

#include <cstdint>
#include <vector>

namespace {
constexpr uint32_t kWidth = 4;
constexpr uint32_t kHeight = 4;

bool makeContext(HWND& window, HDC& dc, HGLRC& context) {
  WNDCLASSA klass {};
  klass.style = CS_OWNDC;
  klass.lpfnWndProc = DefWindowProcA;
  klass.hInstance = GetModuleHandleA(nullptr);
  klass.lpszClassName = "CoralGlSolid";
  RegisterClassA(&klass);
  window = CreateWindowExA(0, klass.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 32, 32, nullptr,
                           nullptr, klass.hInstance, nullptr);
  if (window == nullptr) return false;
  dc = GetDC(window);
  PIXELFORMATDESCRIPTOR format {};
  format.nSize = sizeof(format);
  format.nVersion = 1;
  format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  format.iPixelType = PFD_TYPE_RGBA;
  format.cColorBits = 32;
  const int pixelFormat = ChoosePixelFormat(dc, &format);
  if (pixelFormat == 0 || !SetPixelFormat(dc, pixelFormat, &format)) return false;
  context = wglCreateContext(dc);
  return context != nullptr && wglMakeCurrent(dc, context);
}
} // namespace

TEST(GlExternalTexture, SolidOverlaySamples) {
  HWND window = nullptr;
  HDC dc = nullptr;
  HGLRC context = nullptr;
  if (!makeContext(window, dc, context)) GTEST_SKIP() << "no GL context";

  coral::EffectEngineConfig config;
  config.backend = coral::EffectBackend::OpenGL;
  config.sharedContext = context;
  config.renderWidth = kWidth;
  config.renderHeight = kHeight;
  config.renderMode = coral::EffectRenderMode::Manual;
  config.timeBase = 60;
  const auto engine = coral::EffectEngine::create(config);
  const auto input = coral::TestPatternInputNode::create(kWidth, kHeight);
  const auto pass = engine != nullptr ? engine->createEffectNode("solid-overlay") : nullptr;
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(engine, nullptr);
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);

  coral::EffectShaderDesc shader;
  shader.name = "test/solid-overlay";
  shader.fragmentSource = "  return texture(uOverlay, uv);";
  shader.inputCount = 1;
  ASSERT_EQ(pass->setShader(shader), coral::EffectResult::Success);
  ASSERT_EQ(pass->setTexture("uOverlay", coral::TextureSource::solid({ 0.0f, 1.0f, 0.0f, 1.0f })),
            coral::EffectResult::Success);
  coral::EffectOutputConfig out;
  out.width = kWidth;
  out.height = kHeight;
  out.format = coral::EffectPixelFormat::RGBA8;
  ASSERT_EQ(output->configure(out), coral::EffectResult::Success);
  ASSERT_EQ(engine->attach(input), coral::EffectResult::Success);
  ASSERT_EQ(engine->attach(output), coral::EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), coral::EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), coral::EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), coral::EffectResult::Success);

  wglMakeCurrent(nullptr, nullptr);
  const coral::EffectResult started = engine->start();
  if (started == coral::EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, coral::EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), coral::EffectResult::Success) << engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
  EXPECT_GT(pixels[(kHeight / 2) * kWidth * 4 + (kWidth / 2) * 4 + 1], 200);
  engine->stop();
  wglMakeCurrent(nullptr, nullptr);
  wglDeleteContext(context);
  DestroyWindow(window);
}

TEST(GlExternalTexture, ExternalTextureSamples) {
  HWND window = nullptr;
  HDC dc = nullptr;
  HGLRC context = nullptr;
  if (!makeContext(window, dc, context)) GTEST_SKIP() << "no GL context";
  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  const uint8_t green[4] = { 0, 255, 0, 255 };
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, green);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  coral::EffectEngineConfig config;
  config.backend = coral::EffectBackend::OpenGL;
  config.sharedContext = context;
  config.renderWidth = kWidth;
  config.renderHeight = kHeight;
  config.renderMode = coral::EffectRenderMode::Manual;
  config.timeBase = 60;
  const auto engine = coral::EffectEngine::create(config);
  const auto input = coral::TestPatternInputNode::create(kWidth, kHeight);
  const auto pass = engine != nullptr ? engine->createEffectNode("external") : nullptr;
  const auto output = coral::ReadbackOutputNode::create();
  ASSERT_NE(engine, nullptr);
  ASSERT_NE(input, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(output, nullptr);
  coral::EffectShaderDesc shader;
  shader.name = "test/external";
  shader.fragmentSource = "  return texture(uOverlay, uv);";
  shader.inputCount = 1;
  ASSERT_EQ(pass->setShader(shader), coral::EffectResult::Success);
  auto external = std::make_shared<coral::GlExternalTexture>(texture, GL_TEXTURE_2D, 1, 1,
                                                             coral::EffectPixelFormat::RGBA8, 1);
  ASSERT_EQ(pass->setTexture("uOverlay", coral::TextureSource::fromExternal(external)),
            coral::EffectResult::Success);
  coral::EffectOutputConfig out;
  out.width = kWidth;
  out.height = kHeight;
  out.format = coral::EffectPixelFormat::RGBA8;
  ASSERT_EQ(output->configure(out), coral::EffectResult::Success);
  ASSERT_EQ(engine->attach(input), coral::EffectResult::Success);
  ASSERT_EQ(engine->attach(output), coral::EffectResult::Success);
  ASSERT_EQ(engine->connect(input, pass), coral::EffectResult::Success);
  ASSERT_EQ(engine->connect(pass, output), coral::EffectResult::Success);
  ASSERT_EQ(engine->setOutput(output), coral::EffectResult::Success);
  engine->setOnDeviceReady([&engine](backend::Device& device) {
    engine->setExternalTextureResolver(coral::createGlExternalTextureResolver(device));
  });
  wglMakeCurrent(nullptr, nullptr);
  const coral::EffectResult started = engine->start();
  if (started == coral::EffectResult::DeviceLost) GTEST_SKIP() << engine->diagnostics();
  ASSERT_EQ(started, coral::EffectResult::Success) << engine->diagnostics();
  ASSERT_EQ(engine->execute(), coral::EffectResult::Success) << engine->diagnostics();
  std::vector<uint8_t> pixels;
  ASSERT_TRUE(output->waitForFrame(nullptr, &pixels));
  EXPECT_GT(pixels[(kHeight / 2) * kWidth * 4 + (kWidth / 2) * 4 + 1], 200);

  ASSERT_TRUE(wglMakeCurrent(dc, context));
  const uint8_t red[4] = { 255, 0, 0, 255 };
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, red);
  wglMakeCurrent(nullptr, nullptr);

  auto updated = std::make_shared<coral::GlExternalTexture>(texture, GL_TEXTURE_2D, 1, 1,
                                                            coral::EffectPixelFormat::RGBA8, 2);
  ASSERT_EQ(pass->setTexture("uOverlay", coral::TextureSource::fromExternal(updated)),
            coral::EffectResult::Success);
  ASSERT_EQ(engine->execute(), coral::EffectResult::Success) << engine->diagnostics();
  std::vector<uint8_t> second;
  ASSERT_TRUE(output->waitForFrame(nullptr, &second));
  EXPECT_GT(second[(kHeight / 2) * kWidth * 4 + (kWidth / 2) * 4], 200);
  engine->stop();
  glDeleteTextures(1, &texture);
  wglMakeCurrent(nullptr, nullptr);
  wglDeleteContext(context);
  DestroyWindow(window);
}