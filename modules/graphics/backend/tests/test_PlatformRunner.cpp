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
 * @file test_PlatformRunner.cpp
 *
 * Creates a real device for every supported backend and exercises the parts of the contract that only
 * a running driver can check.
 *
 * ## Why this is a separate file
 *
 * Everything else in `modules/graphics/tests` is device-free on purpose, so the suite runs anywhere. This file
 * is the opposite: it needs hardware. Keeping the two apart is what lets the device-free suites run in
 * CI on a machine with no graphics stack while this one degrades to skipped tests instead of failures.
 *
 * ## The runner
 *
 * `PlatformRunner` is the per-platform half: it decides which backends this platform is *expected* to
 * be able to bring up, and how to describe a surface for them.
 *
 * | Platform | OpenGL dialect | Backends expected |
 * |---|---|---|
 * | Windows | desktop 4.1+ | OpenGL |
 * | Linux | desktop 4.1+ | OpenGL |
 * | macOS | desktop 4.1+ | OpenGL, Metal (once implemented) |
 * | Android | ES 3.0+ | OpenGL, Vulkan (once implemented) |
 * | iOS | ES 3.0+ | OpenGL, Metal (once implemented) |
 *
 * A backend that is compiled in but not implemented yet is skipped with a reason rather than failed;
 * one that is implemented but cannot come up **is** a failure, because that is what a user would hit.
 *
 * ## Headless
 *
 * Every context is offscreen: a hidden window on Windows, a pbuffer on Linux, a bare CGL context on
 * macOS, a pbuffer on Android. A test binary must not open a window in CI, and an offscreen context
 * exercises the same resource, command-buffer and capability paths as an on-screen one. Presentation
 * is the one thing it cannot check, which is why there is no presentation test here.
 */

#include "test_asserts.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// For `gl::GlPlatform::backendName()`. Included explicitly rather than reached through `backend/RHI.h`: this is a
// private header, and a test that depends on it arriving transitively breaks the day the public one stops pulling it in.
#include "private/backend/gl/GlPlatform.h"

// For the one module the library's own compiler cannot produce: one with a `layout(push_constant)` block. Linked to
// the test target for the same reason and guarded the same way (see the test target in CMakeLists.txt).
#if CORAL_BACKEND_HAS_SHADERC
#include <shaderc/shaderc.h>
#endif

using namespace backend;

namespace test {
namespace {

/** A backend this platform is expected to be able to create a device for. */
struct ExpectedBackend {
  Backend backend;
  /** Human readable name, for the test trace and diagnostics. */
  const char* name;
  /** The dialect the OpenGL backend should come up in. Only meaningful for `Backend::OpenGL`. */
  GlDialect dialect;
  /** False when the backend is API-complete but not implemented, so its absence is a skip. */
  bool implemented;
};

/**
 * The per-platform runner.
 *
 * One implementation covers every platform, because the surface descriptor for an offscreen context is
 * empty everywhere: the platform layer creates a hidden window, a pbuffer or a bare CGL context as it
 * sees fit. The class exists so that a platform which later needs something extra - an `NSView` on
 * macOS, an `ANativeWindow` on Android - has an obvious place to put it.
 */
class PlatformRunner {
  public:
  /** @return the backends this platform is expected to support, given what was compiled in. */
  static std::vector<ExpectedBackend> expectedBackends() {
    std::vector<ExpectedBackend> expected;

#if defined(CORAL_BUILD_FOR_ANDROID) || defined(CORAL_BUILD_FOR_IOS)
    constexpr GlDialect kDialect = GlDialect::ES;
    constexpr const char* kGlName = "OpenGL ES 3.0+";
#else
    constexpr GlDialect kDialect = GlDialect::Desktop;
    constexpr const char* kGlName = "OpenGL 4.1+";
#endif

    if (isBackendAvailable(Backend::OpenGL)) {
      expected.push_back(ExpectedBackend{ Backend::OpenGL, kGlName, kDialect, true });
    }

    // Listed when the build supports them, so that enabling one immediately gets it exercised rather
    // than silently untested.
    if (isBackendAvailable(Backend::Vulkan)) {
      expected.push_back(ExpectedBackend{ Backend::Vulkan, "Vulkan 1.0", kDialect, false });
    }
    if (isBackendAvailable(Backend::Metal)) {
      expected.push_back(ExpectedBackend{ Backend::Metal, "Metal", GlDialect::ES, false });
    }
    return expected;
  }

  /**
   * @return a surface descriptor for an offscreen context.
   *
   * Deliberately empty on every platform. Pinning a size is the one thing that helps, because it saves
   * the backend from having to query a drawable it did not create.
   */
  static SurfaceDescriptor offscreenSurface() {
    SurfaceDescriptor surface;
    surface.width = 256;
    surface.height = 256;
    surface.preferredFormat = PixelFormat::R8G8B8A8_UNORM;
    surface.label = "backend-platform-runner";
    return surface;
  }

  /** Fills in a device configuration for @p expected, offscreen. */
  static DeviceConfiguration configurationFor(const ExpectedBackend& expected, const SurfaceDescriptor& surface) {
    DeviceConfiguration configuration;
    configuration.preferredBackend = expected.backend;
    configuration.glDialect = expected.dialect;
    configuration.surface = &surface;
    // No fallback: a test that silently comes up on a different backend than it asked for proves
    // nothing about the backend it named.
    configuration.allowFallback = false;
    configuration.appName = "coral-backend-tests";
    // Opt-in driver validation for a focused run, e.g.
    //   CORAL_BACKEND_VALIDATION=1 test_backend --gtest_filter=PlatformRunner.Multisample*
    // It is not enabled by default because installing the validation layer is not a test prerequisite.
    configuration.enableValidation = std::getenv("CORAL_BACKEND_VALIDATION") != nullptr;
    return configuration;
  }
};

/** Collects creation-time diagnostics, so a failed creation can report why it was refused. */
struct DeviceCreationLog {
  std::string messages;

  static void callback(MessageSeverity severity, const char* message, void* userData) {
    auto* log = static_cast<DeviceCreationLog*>(userData);
    if (log == nullptr || message == nullptr) return;
    if (severity == MessageSeverity::Error || severity == MessageSeverity::Warning) {
      if (!log->messages.empty()) log->messages += "\n";
      log->messages += message;
    }
  }
};

/** Owns a device for the duration of one check. */
class DeviceHandle {
  public:
  DeviceHandle() = default;
  ~DeviceHandle() {
    if (mDevice != nullptr) {
      mDevice->waitIdle();
      delete mDevice;
    }
  }

  DeviceHandle(const DeviceHandle&) = delete;
  DeviceHandle& operator=(const DeviceHandle&) = delete;

  [[nodiscard]] Device* get() const noexcept { return mDevice; }
  [[nodiscard]] const std::string& log() const noexcept { return mLog; }

  Result create(const ExpectedBackend& expected) {
    mSurface = PlatformRunner::offscreenSurface();
    const DeviceConfiguration configuration = PlatformRunner::configurationFor(expected, mSurface);
    DeviceCreationLog log;
    const Result result = createDevice(configuration, &mDevice, &DeviceCreationLog::callback, &log);
    mLog = log.messages;
    return result;
  }

  private:
  Device* mDevice = nullptr;
  SurfaceDescriptor mSurface;
  std::string mLog;
};

/**
 * Runs @p check for every expected backend.
 *
 * A template rather than a macro, so the check can be an ordinary lambda with ordinary scoping.
 */
template<typename Check>
void forEachExpectedBackend(Check&& check) {
  const std::vector<ExpectedBackend> backends = PlatformRunner::expectedBackends();
  if (backends.empty()) {
    GTEST_SKIP() << "this build has no rendering backend compiled in";
  }
  for (const ExpectedBackend& expected: backends) {
    SCOPED_TRACE(expected.name);
    check(expected);
  }
}

/**
 * Creates a device for the traced backend, or decides that the check cannot run.
 *
 * A function taking an out-parameter rather than one returning the device, because gtest's
 * `GTEST_SKIP()` expands to a bare `return` and therefore cannot appear in a value-returning function.
 * The caller does:
 *
 * ```
 * DeviceHandle device;
 * Device* api = nullptr;
 * if (!createDeviceOrSkip(device, expected, api)) return;
 * ```
 *
 * @param outDevice receives the device, or null when the backend could not be created.
 * @return true when the caller should continue with @p outDevice.
 */
bool createDeviceOrSkip(DeviceHandle& device, const ExpectedBackend& expected, Device*& outDevice) {
  outDevice = nullptr;
  if (isSuccess(device.create(expected))) {
    outDevice = device.get();
    return true;
  }
  if (expected.implemented) {
    ADD_FAILURE() << "creating a " << expected.name << " device failed" << (device.log().empty() ? "" : ": ")
                  << device.log();
  }
  return false;
}

// The expected dialect and its name are file-scope constants rather than `#if` blocks inside the test
// below. A preprocessor conditional cannot live inside a `TEST` body: gtest's TEST expands to a class
// declaration with an inline member function, and a directive inside that function is a syntax error,
// reported at the closing brace of the file rather than at the directive.
#if defined(CORAL_BUILD_FOR_ANDROID) || defined(CORAL_BUILD_FOR_IOS)
constexpr GlDialect kExpectedDialect = GlDialect::ES;
constexpr const char* kExpectedDialectName = "OpenGL ES 3.0+";
#else
constexpr GlDialect kExpectedDialect = GlDialect::Desktop;
constexpr const char* kExpectedDialectName = "OpenGL 4.1+";
#endif

/**
 * Prints the device identity and the interesting capability answers.
 *
 * This is a diagnostic, not an assertion: it is here so that a failing run on an unfamiliar driver says
 * what it actually got rather than only which expectation broke. The output is attached to the test
 * result, so `--gtest_output=xml` carries it.
 */
TEST(PlatformRunner, ReportDeviceIdentity) {
  forEachExpectedBackend([](const ExpectedBackend& expected) {
    DeviceHandle device;
    Device* api = nullptr;
    if (!createDeviceOrSkip(device, expected, api)) {
      GTEST_SKIP() << expected.name << " is compiled in but not implemented yet";
    }

    const DeviceInfo& info = api->getInfo();
    const Capabilities& capabilities = api->getCapabilities();

    RecordProperty("backend", info.backendName);
    RecordProperty("vendor", info.vendor);
    RecordProperty("device", info.deviceName);
    RecordProperty("apiVersion", info.apiVersion);
    RecordProperty("shadingLanguageVersion", info.driverVersion);

    std::string diagnostics;
    api->appendDiagnostics(diagnostics);
    std::cout << "--- " << expected.name << " ---\n" << diagnostics << std::endl;
  });
}

// The window-system layer this build uses, as a name. A list rather than an expression because each platform is a
// different string, and the point is that the platform layer reports which one it is -- see the note on
// `GlPlatform::backendName`.
#if defined(CORAL_BUILD_FOR_WIN)
constexpr const char* kExpectedWindowSystemName = "WGL";
#elif defined(CORAL_BUILD_FOR_ANDROID)
constexpr const char* kExpectedWindowSystemName = "EGL";
#elif defined(CORAL_BUILD_FOR_IOS)
constexpr const char* kExpectedWindowSystemName = "EAGL";
#elif defined(CORAL_BUILD_FOR_MAC)
constexpr const char* kExpectedWindowSystemName = "CGL";
#elif defined(CORAL_BUILD_FOR_LINUX)
// Linux compiles both window systems into the same library: GLX for X11 and EGL for Wayland (and the
// surfaceless fallback), selected per surface. The name has to say so rather than name one of them.
constexpr const char* kExpectedWindowSystemName = "GLX/EGL";
#else
// No window system at all: the platform layer is the null implementation, which reports that rather than a name it
// does not have.
constexpr const char* kExpectedWindowSystemName = "None";
#endif

TEST(PlatformRunner, ReportsTheDialectThisBuildTargets) {
  // The dialect is a compile-time property of the platform, and every capability decision downstream
  // reads it, so it is worth asserting directly rather than only through its effects.
  EXPECT_EQ(getPlatformGlDialect(), kExpectedDialect);
  EXPECT_STREQ(getPlatformGlDialectName(), kExpectedDialectName);
}

TEST(PlatformRunner, WindowSystemNameIsReportedForThisPlatform) {
  // The window-system layer is the one thing about a GL context that differs between machines running the same build,
  // so it is what a bug report needs and what a caller cannot work out for themselves.
  //
  // Checked directly rather than through a device, because the function is a compile-time answer and this way the test
  // does not skip on a machine where no GL context can be created -- which would be exactly the machine where knowing
  // the name matters.
  const char* const name = gl::GlPlatform::backendName();
  ASSERT_NE(name, nullptr);
  EXPECT_STREQ(name, kExpectedWindowSystemName);

  // A name, not a sentence: it goes into a diagnostic line beside the vendor and the API version.
  EXPECT_GT(std::string(name).size(), 0u);
  EXPECT_LT(std::string(name).size(), 16u);
}

// A directive cannot live inside a TEST body, for the reason given above, so the build configuration is
// captured as a constant here instead.
#if CORAL_BACKEND_HAS_OPENGL
constexpr bool kOpenGLCompiledIn = true;
#else
constexpr bool kOpenGLCompiledIn = false;
#endif

TEST(PlatformRunner, BackendTableMatchesTheConfiguration) {
  // A backend the build claims to support must be creatable in principle; the per-backend checks below
  // prove it actually comes up. This only checks the table is self-consistent, which catches the
  // "macro defined but sources excluded" class of build mistake.
  if (kOpenGLCompiledIn) {
    EXPECT_TRUE(isBackendAvailable(Backend::OpenGL));
  }

  const Backend defaultBackend = getDefaultBackend();
  // The default is either the platform's first choice or Null when nothing is compiled in. What it must
  // never be is a backend the build does not have.
  if (defaultBackend != Backend::Null) {
    EXPECT_TRUE(isBackendAvailable(defaultBackend));
  }
}

TEST(PlatformRunner, CreatesADeviceForEveryExpectedBackend) {
  forEachExpectedBackend([](const ExpectedBackend& expected) {
    DeviceHandle device;
    Device* api = nullptr;
    if (!createDeviceOrSkip(device, expected, api)) {
      return;
    }
    EXPECT_NE(api, nullptr);
  });
}

TEST(PlatformRunner, DeviceReportsItsIdentityAndCapabilities) {
  forEachExpectedBackend([](const ExpectedBackend& expected) {
    DeviceHandle device;
    Device* api = nullptr;
    if (!createDeviceOrSkip(device, expected, api)) {
      return;
    }

    const DeviceInfo& info = api->getInfo();
    EXPECT_EQ(info.backend, expected.backend);
    ASSERT_NE(info.vendor, nullptr);
    ASSERT_NE(info.deviceName, nullptr);
    ASSERT_NE(info.apiVersion, nullptr);
    ASSERT_NE(info.backendName, nullptr);
    EXPECT_GT(std::string(info.backendName).size(), 0u);

    const Capabilities& capabilities = api->getCapabilities();
    if (expected.backend == Backend::OpenGL) {
      // The window-system layer's name has to reach the caller, not merely be available. It did not: this field was
      // one of two fixed strings chosen by the dialect, so every desktop build reported the same name whether it was
      // running on WGL, GLX or CGL, and the function that knew the difference was never called by anything.
      //
      // Asserted as a suffix rather than an equality so that the dialect half stays free to change, and because a
      // suffix is what "which of the four context paths is running" means.
      EXPECT_NE(std::string(info.backendName).find(kExpectedWindowSystemName), std::string::npos) << info.backendName;

      // The dialect is what the whole capability set keys off, so a device that came up in the wrong
      // one would make every downstream answer wrong.
      const char* expectedPrefix = expected.dialect == GlDialect::ES ? "OpenGL ES" : "OpenGL";
      EXPECT_NE(std::string(capabilities.apiVersion).find(expectedPrefix), std::string::npos)
              << capabilities.apiVersion;
    }

    // Limits are never zero unless zero genuinely means "impossible". A zero reads as "unsupported" to
    // upper layers, so a probe that forgot to fill one in is a real defect.
    EXPECT_GT(capabilities.limits.maxTextureDimension2D, 0u);
    EXPECT_GT(capabilities.limits.maxVertexAttributes, 0u);
    EXPECT_GT(capabilities.limits.maxColorAttachments, 0u);
    EXPECT_GT(capabilities.maxTextureSize, 0u);

    // Every backend the RHI ships must be able to render into something and present it.
    EXPECT_TRUE(capabilities.supports(Feature::Swapchain));
    EXPECT_TRUE(capabilities.supports(Feature::TextureCube));
    EXPECT_TRUE(capabilities.supportsSampleCount(1)) << "1 sample (no MSAA) is always supported";

    // A renderable format must exist, or the backend cannot draw at all.
    const TextureFormatCapabilities& rgba = capabilities.queryTextureFormat(PixelFormat::R8G8B8A8_UNORM);
    EXPECT_TRUE(rgba.sampled);
    EXPECT_TRUE(rgba.renderable);

    // And the universally supported depth format must be usable as an attachment.
    EXPECT_TRUE(capabilities.queryTextureFormat(PixelFormat::D24_UNORM_S8_UINT).renderable);
  });
}

TEST(PlatformRunner, CapabilityReasonsAgreeWithTheFeatureBits) {
  forEachExpectedBackend([](const ExpectedBackend& expected) {
    DeviceHandle device;
    Device* api = nullptr;
    if (!createDeviceOrSkip(device, expected, api)) {
      return;
    }

    const Capabilities& capabilities = api->getCapabilities();

    // The contract upper layers rely on: a feature bit that is set reports Available, and one that is
    // clear reports something that is not. A backend that recorded no reason would make every "why is
    // this technique disabled" diagnostic useless.
    uint32_t supported = 0;
    uint32_t unsupported = 0;
    for (uint32_t bit = 0; bit < 32; ++bit) {
      const auto feature = static_cast<Feature>(1u << bit);
      if (feature == Feature::None) continue;

      const bool present = capabilities.supports(feature);
      const FeatureUnavailableReason reason = capabilities.reasonFor(feature);

      if (present) {
        ++supported;
        EXPECT_EQ(reason, FeatureUnavailableReason::Available) << toString(feature);
      } else {
        ++unsupported;
        EXPECT_NE(reason, FeatureUnavailableReason::Available) << toString(feature);
      }
    }

    // A backend that claims nothing, or everything, is almost certainly mis-probing rather than
    // genuinely useless or genuinely omnipotent.
    EXPECT_GT(supported, 0u);
    EXPECT_GT(unsupported, 0u) << "no real device supports every feature the RHI models";

    // A report that cannot be produced is a report nobody can attach to a bug.
    std::string diagnostics;
    api->appendDiagnostics(diagnostics);
    EXPECT_GT(diagnostics.size(), 0u);
  });
}

TEST(PlatformRunner, CreatesAndDestroysEveryResourceKind) {
  forEachExpectedBackend([](const ExpectedBackend& expected) {
    DeviceHandle device;
    Device* api = nullptr;
    if (!createDeviceOrSkip(device, expected, api)) {
      return;
    }

    // ---------------------------------------------------------------- texture, view, sampler
    const TextureDescriptor textureDescriptor = TextureDescriptor::texture2D(64, 64, PixelFormat::R8G8B8A8_UNORM,
            static_cast<TextureUsages>(TextureUsage::Sampled | TextureUsage::ColorAttachment));
    TextureHandle texture = TextureHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createTexture(textureDescriptor, &texture));
    EXPECT_TRUE(texture.valid());
    EXPECT_TRUE(api->isResourceReadyToDestroy(texture));

    TextureViewDescriptor viewDescriptor;
    viewDescriptor.texture = texture;
    viewDescriptor.dimension = TextureViewDimension::Texture2D;
    TextureViewHandle view = TextureViewHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createTextureView(viewDescriptor, &view));
    EXPECT_TRUE(view.valid());

    TextureSamplerHandle sampler = TextureSamplerHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createSampler(SamplerDescriptor::linearClamp(), &sampler));
    EXPECT_TRUE(sampler.valid());

    // ---------------------------------------------------------------- buffer
    const float vertices[9] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    BufferHandle buffer = BufferHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createBuffer(BufferDescriptor::vertex(sizeof(vertices), vertices), &buffer));
    EXPECT_TRUE(buffer.valid());

    // ---------------------------------------------------------------- layouts, set, pipeline
    DescriptorSetLayoutDescriptor setLayoutDescriptor;
    setLayoutDescriptor.bindingCount = 1;
    setLayoutDescriptor.bindings[0].binding = 0;
    setLayoutDescriptor.bindings[0].type = BindingType::TextureSampler;
    setLayoutDescriptor.bindings[0].count = 1;
    setLayoutDescriptor.bindings[0].stageFlags = 1u << 1; // fragment

    DescriptorSetLayoutHandle setLayout = DescriptorSetLayoutHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createDescriptorSetLayout(setLayoutDescriptor, &setLayout));

    DescriptorSetHandle set = DescriptorSetHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createDescriptorSet(setLayout, &set));

    DescriptorWrite write;
    write.binding = 0;
    write.type = BindingType::TextureSampler;
    write.texture = texture;
    write.sampler = sampler;
    ASSERT_RHI_SUCCESS(api->updateDescriptorSet(set, &write, 1));

    PipelineLayoutDescriptor pipelineLayoutDescriptor;
    pipelineLayoutDescriptor.setLayoutCount = 1;
    pipelineLayoutDescriptor.setLayouts[0] = setLayout;
    PipelineLayoutHandle pipelineLayout = PipelineLayoutHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createPipelineLayout(pipelineLayoutDescriptor, &pipelineLayout));

    RenderPassDescriptor passDescriptor;
    passDescriptor.colorAttachmentCount = 1;
    passDescriptor.colorFormats[0] = PixelFormat::R8G8B8A8_UNORM;
    passDescriptor.sampleCount = 1;
    RenderPassHandle renderPass = RenderPassHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createRenderPass(passDescriptor, &renderPass));

    RenderPassBeginInfo passInfo;
    passInfo.renderPass = renderPass;
    passInfo.colorAttachmentCount = 1;
    passInfo.colorAttachments[0].view = view;
    passInfo.colorAttachments[0].loadOp = LoadOp::Clear;
    passInfo.colorAttachments[0].clearColor = Color(0.1, 0.2, 0.3, 1.0);
    passInfo.renderArea = { 64, 64 };

    // ---------------------------------------------------------------- command buffer
    CommandBuffer* commandBuffer = nullptr;
    ASSERT_RHI_SUCCESS(api->createCommandBuffer(CommandBufferDescriptor{}, &commandBuffer));
    ASSERT_NE(commandBuffer, nullptr);

    EXPECT_RHI_SUCCESS(commandBuffer->begin());
    EXPECT_TRUE(commandBuffer->isRecording());
    EXPECT_RHI_SUCCESS(commandBuffer->beginRenderPass(passInfo));
    EXPECT_RHI_SUCCESS(commandBuffer->endRenderPass());
    EXPECT_RHI_SUCCESS(commandBuffer->end());

    // Executing it must succeed even with nothing drawn: an empty pass is still a clear, and the clear
    // is where the backend's load-operation handling is actually exercised.
    EXPECT_RHI_SUCCESS(api->executeCommandBuffer(*commandBuffer));

    // ---------------------------------------------------------------- fence
    FenceHandle fence = FenceHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createFence(FenceDescriptor{}, &fence));
    EXPECT_RHI_SUCCESS(api->resetFence(fence));

    // ---------------------------------------------------------------- teardown, reverse order
    api->destroyCommandBuffer(commandBuffer);
    api->destroyFence(fence);
    api->destroyRenderPass(renderPass);
    api->destroyPipelineLayout(pipelineLayout);
    api->destroyDescriptorSet(set);
    api->destroyDescriptorSetLayout(setLayout);
    api->destroyBuffer(buffer);
    api->destroySampler(sampler);
    api->destroyTextureView(view);
    api->destroyTexture(texture);

    // The device must still be usable after a full create/destroy cycle.
    EXPECT_RHI_SUCCESS(api->waitIdle());
  });
}

TEST(PlatformRunner, StaleHandlesAreRejectedAfterDestruction) {
  forEachExpectedBackend([](const ExpectedBackend& expected) {
    DeviceHandle device;
    Device* api = nullptr;
    if (!createDeviceOrSkip(device, expected, api)) {
      return;
    }

    // Create two textures, free the first, and confirm the freed handle no longer resolves while the
    // live one still does. This exercises the generation counter through a real backend rather than
    // through the pool in isolation.
    TextureHandle first = TextureHandle::invalid();
    TextureHandle second = TextureHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createTexture(TextureDescriptor::texture2D(16, 16, PixelFormat::R8G8B8A8_UNORM), &first));
    ASSERT_RHI_SUCCESS(api->createTexture(TextureDescriptor::texture2D(16, 16, PixelFormat::R8G8B8A8_UNORM), &second));
    EXPECT_NE(first, second);

    api->destroyTexture(first);

    // A command that names the destroyed handle must be refused, not silently accepted. This is the case
    // that matters in practice: a renderer replacing a texture per frame while a command buffer still
    // refers to the old one.
    CommandBuffer* commandBuffer = nullptr;
    ASSERT_RHI_SUCCESS(api->createCommandBuffer(CommandBufferDescriptor{}, &commandBuffer));
    ASSERT_RHI_SUCCESS(commandBuffer->begin());
    const Result stale = commandBuffer->clearColorTexture(first, TextureSubresourceRange::all(), Color::black());
    EXPECT_EQ(stale, Result::InvalidHandle) << "a destroyed texture handle must not be usable";
    ASSERT_RHI_SUCCESS(commandBuffer->end());

    api->destroyCommandBuffer(commandBuffer);
    api->destroyTexture(second);
  });
}

TEST(PlatformRunner, FrameLifecycleIsSafeWithoutASwapchain) {
  forEachExpectedBackend([](const ExpectedBackend& expected) {
    DeviceHandle device;
    Device* api = nullptr;
    if (!createDeviceOrSkip(device, expected, api)) {
      return;
    }

    // Without a surface there is no swapchain to acquire, so beginFrame must report an invalid handle
    // rather than pretending to have acquired a frame. A backend that returned Success here would make
    // an upper layer believe it can present to nothing.
    SwapchainInfo info = {};
    const Result result = api->beginFrame(SwapchainHandle::invalid(), &info);
    EXPECT_EQ(result, Result::InvalidHandle);

    EXPECT_RHI_SUCCESS(api->waitIdle());
  });
}

/**
 * Two **indexed** draws with different pipelines, read back from the target -- the case 4.1.37's defect needed.
 *
 * ## What this covers that the graphics suite cannot
 *
 * `GL_ELEMENT_ARRAY_BUFFER` is *vertex array state*, and the GL backend's `acquireVertexArray` deliberately clears it
 * when it builds a vertex array ("the replay path re-binds it when it needs it"). The replay only re-bound it while
 * binding a *new* vertex array, so a `SetIndexBuffer` that ran while an older array was current was lost, and the draw
 * read its indices from an empty binding -- not a GL error, an access violation inside `glDrawElements`. That is
 * 4.1.37, where the canvas quad route became the first code in the tree to issue an indexed draw at all.
 *
 * The graphics suite caught it, but as **38 failures and a crash** in a suite of a thousand, which says nothing about
 * where to look. This is the same defect at the level it lives at, in one test, and it is built so the two failures are
 * distinguishable:
 *
 *   * the draw happens at all (a lost index binding here is a crash or a blank target, not a subtle colour);
 *   * the second pipeline's quad is in the other half, in its own colour, so a draw that kept the *first* vertex array
 *     (or the first index buffer) paints the wrong half rather than nothing;
 *   * the middle of the target is left clear, so a pipeline that never drew cannot pass by leaving the clear colour.
 *
 * ## Why it needs two pipelines
 *
 * One indexed draw after a clear can pass with or without the re-bind, depending on which vertex array happened to be
 * current. The bind only changes at a draw whose pipeline differs from the last one's, so the sequence has to be
 * draw, then *another* pipeline, then draw again -- which is exactly what a frame with two batches does, and what the
 * canvas does with every batch it records.
 */
TEST(PlatformRunner, IndexedDrawsKeepTheirIndicesWhenTheVertexArrayChanges) {
  forEachExpectedBackend([](const ExpectedBackend& expected) {
    DeviceHandle device;
    Device* api = nullptr;
    if (!createDeviceOrSkip(device, expected, api)) {
      return;
    }

    constexpr uint32_t kSize = 32;
    /**
     * A pixel as the readback hands it over: raw `R8G8B8A8` bytes, so a little-endian `uint32` is
     * `(A << 24) | (B << 16) | (G << 8) | R` -- **not** the graphics layer's `ARGB32` packing, which puts red in the
     * top byte. Spelled out rather than written as a literal because getting it backwards swaps red and blue in exactly
     * the way a wrong fragment shader does, and a test that fails for the wrong reason is worse than no test.
     */
    constexpr auto rgba = [](const uint32_t red, const uint32_t green, const uint32_t blue) {
      return 0xFF000000u | (blue << 16) | (green << 8) | red;
    };
    const uint32_t kLeft = rgba(0xFF, 0x00, 0x00);  // the first draw's fragment colour
    const uint32_t kRight = rgba(0x00, 0x00, 0xFF); // the second draw's
    const uint32_t kClear = rgba(0x00, 0x00, 0x00);

    // ---------------------------------------------------------------- target and its view
    const TextureDescriptor targetDescriptor = TextureDescriptor::texture2D(kSize, kSize,
            PixelFormat::R8G8B8A8_UNORM,
            static_cast<TextureUsages>(TextureUsage::ColorAttachment));
    TextureHandle target = TextureHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createTexture(targetDescriptor, &target));

    TextureViewDescriptor targetViewDescriptor;
    targetViewDescriptor.texture = target;
    targetViewDescriptor.dimension = TextureViewDimension::Texture2D;
    TextureViewHandle targetView = TextureViewHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createTextureView(targetViewDescriptor, &targetView));

    RenderPassDescriptor passDescriptor;
    passDescriptor.colorAttachmentCount = 1;
    passDescriptor.colorFormats[0] = PixelFormat::R8G8B8A8_UNORM;
    passDescriptor.sampleCount = 1;
    RenderPassHandle renderPass = RenderPassHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createRenderPass(passDescriptor, &renderPass));

    FramebufferDescriptor framebufferDescriptor;
    framebufferDescriptor.renderPass = renderPass;
    framebufferDescriptor.colorAttachmentCount = 1;
    framebufferDescriptor.colorAttachments[0] = targetView;
    framebufferDescriptor.width = kSize;
    framebufferDescriptor.height = kSize;
    FramebufferHandle framebuffer = FramebufferHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createFramebuffer(framebufferDescriptor, &framebuffer));

    // ---------------------------------------------------------------- two quads as 4 vertices + 6 indices each
    //
    // A quad as four corners and six indices is the shape the canvas's batch route uses (`kQuadCornerCount` /
    // `kQuadIndexCount`, 4.1.37), so an index count that is wrong here is wrong there too.
    //
    // The two quads sit in the left and right thirds of the clip volume, with a gap between them: the gap is what says
    // the clear survived, and it is where a draw that covered the target anyway would be visible.
    const float leftQuad[8] = { -1.0f, -1.0f, -0.3f, -1.0f, -0.3f, 1.0f, -1.0f, 1.0f };
    const float rightQuad[8] = { 0.3f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 0.3f, 1.0f };
    const uint32_t indices[6] = { 0, 1, 2, 0, 2, 3 };

    BufferHandle leftVertices = BufferHandle::invalid();
    BufferHandle rightVertices = BufferHandle::invalid();
    BufferHandle leftIndices = BufferHandle::invalid();
    BufferHandle rightIndices = BufferHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createBuffer(BufferDescriptor::vertex(sizeof(leftQuad), leftQuad), &leftVertices));
    ASSERT_RHI_SUCCESS(api->createBuffer(BufferDescriptor::vertex(sizeof(rightQuad), rightQuad), &rightVertices));
    ASSERT_RHI_SUCCESS(api->createBuffer(BufferDescriptor::index(sizeof(indices), indices), &leftIndices));
    ASSERT_RHI_SUCCESS(api->createBuffer(BufferDescriptor::index(sizeof(indices), indices), &rightIndices));

    // ---------------------------------------------------------------- two pipelines, so the vertex array changes
    const std::string vertexSource = "#version 310 es\n"
                                     "layout(location = 0) in vec2 aPos;\n"
                                     "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";
    const std::string leftFragment = "#version 310 es\nprecision mediump float;\n"
                                     "layout(location = 0) out vec4 o;\nvoid main() { o = vec4(1.0, 0.0, 0.0, 1.0); }\n";
    const std::string rightFragment = "#version 310 es\nprecision mediump float;\n"
                                      "layout(location = 0) out vec4 o;\nvoid main() { o = vec4(0.0, 0.0, 1.0, 1.0); }\n";

    const auto makeModule = [&](const std::string& source, const ShaderStage stage) {
      ShaderModuleDescriptor descriptor;
      descriptor.stage = stage;
      descriptor.language = ShaderModuleDescriptor::Language::GLSL;
      descriptor.code = source.c_str();
      descriptor.codeSize = source.size() + 1; // a source module's size includes the NUL terminator
      descriptor.label = "indexed-draw-test";
      ShaderModuleHandle handle = ShaderModuleHandle::invalid();
      const Result result = api->createShaderModule(descriptor, &handle);
      return std::make_pair(result, handle);
    };

    const auto [vertexResult, vertexModule] = makeModule(vertexSource, ShaderStage::Vertex);
    if (!isSuccess(vertexResult)) {
      // A build with no GLSL compiler cannot make a module from source, and that is a supported configuration rather
      // than a failure of this test (see `test_facade.cpp`, which skips for the same reason).
      GTEST_SKIP() << "this build cannot compile GLSL source into a shader module";
    }
    const auto [leftResult, leftModule] = makeModule(leftFragment, ShaderStage::Fragment);
    const auto [rightResult, rightModule] = makeModule(rightFragment, ShaderStage::Fragment);
    ASSERT_RHI_SUCCESS(leftResult);
    ASSERT_RHI_SUCCESS(rightResult);

    PipelineLayoutDescriptor pipelineLayoutDescriptor;
    PipelineLayoutHandle pipelineLayout = PipelineLayoutHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createPipelineLayout(pipelineLayoutDescriptor, &pipelineLayout));

    const auto makePipeline = [&](const ShaderModuleHandle fragment) {
      RenderPipelineDescriptor descriptor;
      descriptor.layout = pipelineLayout;
      descriptor.vertexModule = vertexModule;
      descriptor.fragmentModule = fragment;
      descriptor.vertexInput.bufferCount = 1;
      descriptor.vertexInput.buffers[0].stride = sizeof(float) * 2;
      descriptor.vertexInput.buffers[0].stepMode = VertexStepMode::Vertex;
      descriptor.vertexInput.buffers[0].firstAttribute = 0;
      descriptor.vertexInput.buffers[0].attributeCount = 1;
      descriptor.vertexInput.attributeCount = 1;
      descriptor.vertexInput.attributes[0].shaderLocation = 0;
      descriptor.vertexInput.attributes[0].bufferIndex = 0;
      descriptor.vertexInput.attributes[0].format = VertexFormat::Float2;
      descriptor.vertexInput.attributes[0].offset = 0;
      // Nothing is written to depth or stencil, so both are off rather than left at their defaults -- a pipeline that
      // asked for a depth test the pass has no attachment for is refused by some backends.
      descriptor.depthStencil.depthTestEnable = false;
      descriptor.depthStencil.depthWriteEnable = false;
      descriptor.colorTargetCount = 1;
      descriptor.colorTargets[0].format = PixelFormat::R8G8B8A8_UNORM;
      descriptor.compatibleRenderPass = renderPass;
      descriptor.label = "indexed-draw-test";
      RenderPipelineHandle handle = RenderPipelineHandle::invalid();
      const Result result = api->createRenderPipeline(descriptor, &handle);
      return std::make_pair(result, handle);
    };

    const auto [leftPipelineResult, leftPipeline] = makePipeline(leftModule);
    ASSERT_RHI_SUCCESS(leftPipelineResult);
    const auto [rightPipelineResult, rightPipeline] = makePipeline(rightModule);
    ASSERT_RHI_SUCCESS(rightPipelineResult);

    // ---------------------------------------------------------------- the readback buffer
    const uint64_t readbackSize = static_cast<uint64_t>(kSize) * kSize * 4;
    BufferDescriptor readbackDescriptor;
    readbackDescriptor.size = readbackSize;
    readbackDescriptor.usage = static_cast<BufferUsages>(BufferUsage::TransferDst);
    readbackDescriptor.memoryUsage = MemoryUsage::GpuToCpu;
    readbackDescriptor.label = "indexed-draw-readback";
    BufferHandle readback = BufferHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createBuffer(readbackDescriptor, &readback));

    // ---------------------------------------------------------------- record: draw, other pipeline, draw
    CommandBuffer* commandBuffer = nullptr;
    ASSERT_RHI_SUCCESS(api->createCommandBuffer(CommandBufferDescriptor{}, &commandBuffer));
    ASSERT_RHI_SUCCESS(commandBuffer->begin());

    RenderPassBeginInfo passInfo;
    passInfo.renderPass = renderPass;
    passInfo.colorAttachmentCount = 1;
    passInfo.colorAttachments[0].view = targetView;
    passInfo.colorAttachments[0].loadOp = LoadOp::Clear;
    passInfo.colorAttachments[0].storeOp = StoreOp::Store;
    passInfo.colorAttachments[0].clearColor = Color::black();
    passInfo.renderArea = { kSize, kSize };
    ASSERT_RHI_SUCCESS(commandBuffer->beginRenderPass(passInfo));

    // The index buffer is set *before* the draw, while the previous pipeline's vertex array is still current -- the
    // order the canvas records in, and the order the bug needed.
    ASSERT_RHI_SUCCESS(commandBuffer->bindRenderPipeline(leftPipeline));
    ASSERT_RHI_SUCCESS(commandBuffer->setVertexBuffer(0, leftVertices, 0));
    ASSERT_RHI_SUCCESS(commandBuffer->setIndexBuffer(leftIndices, IndexFormat::UInt32, 0));
    ASSERT_RHI_SUCCESS(commandBuffer->drawIndexed(6, 1, 0, 0, 0));

    ASSERT_RHI_SUCCESS(commandBuffer->bindRenderPipeline(rightPipeline));
    ASSERT_RHI_SUCCESS(commandBuffer->setVertexBuffer(0, rightVertices, 0));
    ASSERT_RHI_SUCCESS(commandBuffer->setIndexBuffer(rightIndices, IndexFormat::UInt32, 0));
    ASSERT_RHI_SUCCESS(commandBuffer->drawIndexed(6, 1, 0, 0, 0));

    ASSERT_RHI_SUCCESS(commandBuffer->endRenderPass());

    // A pass that draws nothing, to make the target's framebuffer current before the read. `copyTextureToBuffer` on the
    // OpenGL backend is `glReadPixels`, which reads whatever read framebuffer is bound rather than the texture named in
    // the copy -- and closing a pass can leave another target's bound. `LoadOp::Load` is what keeps this from destroying
    // the pixels being read. The renderer's own readback does the same thing for the same reason.
    RenderPassBeginInfo readbackPass;
    readbackPass.renderPass = renderPass;
    readbackPass.colorAttachmentCount = 1;
    readbackPass.colorAttachments[0].view = targetView;
    readbackPass.colorAttachments[0].loadOp = LoadOp::Load;
    readbackPass.colorAttachments[0].storeOp = StoreOp::Store;
    readbackPass.renderArea = { kSize, kSize };
    ASSERT_RHI_SUCCESS(commandBuffer->beginRenderPass(readbackPass));
    ASSERT_RHI_SUCCESS(commandBuffer->endRenderPass());

    BufferTextureCopy copy;
    copy.buffer = readback;
    copy.texture = target;
    copy.bufferRegion.dstOffset = 0;
    copy.bufferRegion.size = readbackSize;
    copy.textureRegion.textureOrigin = { 0, 0, 0 };
    copy.textureRegion.size = { kSize, kSize, 1 };
    copy.textureRegion.mipLevel = 0;
    copy.textureRegion.baseArrayLayer = 0;
    copy.textureRegion.arrayLayerCount = 1;
    ASSERT_RHI_SUCCESS(commandBuffer->copyTextureToBuffer(copy));

    ASSERT_RHI_SUCCESS(commandBuffer->end());
    ASSERT_RHI_SUCCESS(api->executeCommandBuffer(*commandBuffer));
    ASSERT_RHI_SUCCESS(api->waitIdle());

    void* mapped = nullptr;
    ASSERT_RHI_SUCCESS(api->mapBuffer(readback, 0, readbackSize, &mapped));
    ASSERT_NE(mapped, nullptr);
    const auto* pixels = static_cast<const uint32_t*>(mapped);
    const auto at = [&](const uint32_t x, const uint32_t y) { return pixels[y * kSize + x]; };
    const uint32_t leftPixel = at(kSize / 8, kSize / 2);
    const uint32_t rightPixel = at(kSize - kSize / 8, kSize / 2);
    const uint32_t middlePixel = at(kSize / 2, kSize / 2);
    api->unmapBuffer(readback);

    std::printf("  indexed draws: left %08x (expected %08x), middle %08x (expected %08x), right %08x (expected "
                "%08x)\n",
            leftPixel, kLeft, middlePixel, kClear, rightPixel, kRight);

    EXPECT_EQ(leftPixel, kLeft) << "the first indexed draw did not reach the target";
    EXPECT_EQ(middlePixel, kClear)
            << "something covered the target between the two draws, so the pass's clear did not survive";
    EXPECT_EQ(rightPixel, kRight)
            << "the second indexed draw read its indices from the wrong vertex array, or from none";

    api->destroyBuffer(readback);
    api->destroyCommandBuffer(commandBuffer);
    api->destroyRenderPipeline(leftPipeline);
    api->destroyRenderPipeline(rightPipeline);
    api->destroyPipelineLayout(pipelineLayout);
    api->destroyShaderModule(vertexModule);
    api->destroyShaderModule(leftModule);
    api->destroyShaderModule(rightModule);
    api->destroyFramebuffer(framebuffer);
    api->destroyRenderPass(renderPass);
    api->destroyTextureView(targetView);
    api->destroyTexture(target);
    EXPECT_RHI_SUCCESS(api->waitIdle());
  });
}

TEST(PlatformRunner, ReportsConfiguredFramesInFlight) {
  if (!isBackendAvailable(Backend::Vulkan)) {
    GTEST_SKIP() << "this build has no Vulkan backend";
  }

  DeviceConfiguration configuration;
  configuration.preferredBackend = Backend::Vulkan;
  configuration.allowFallback = false;
  configuration.maxFramesInFlight = 3;
  configuration.appName = "frames-in-flight-test";
  Device* device = nullptr;
  // Compiled in is not the same as present: macOS has no system Vulkan loader, and MoltenVK is an optional
  // install there. A machine without a Vulkan device has nothing for this test to report about, so it skips
  // rather than failing a check whose subject is missing.
  if (!isSuccess(createDevice(configuration, &device))) {
    GTEST_SKIP() << "this machine has no Vulkan device";
  }
  ASSERT_NE(device, nullptr);
  EXPECT_EQ(device->getCapabilities().limits.maxFramesInFlight, 3u)
          << "the backend silently ignored DeviceConfiguration::maxFramesInFlight";
  delete device;
}

TEST(PlatformRunner, QueueSubmitWithFenceCanOverlapTheGpu) {
  forEachExpectedBackend([](const ExpectedBackend& expected) {
    if (expected.backend != Backend::Vulkan) {
      // The OpenGL backend's queue replays synchronously by construction; this is a Vulkan queue test.
      return;
    }

    DeviceHandle device;
    Device* api = nullptr;
    if (!createDeviceOrSkip(device, expected, api)) return;

    TextureHandle texture = TextureHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createTexture(
            TextureDescriptor::texture2D(16, 16, PixelFormat::R8G8B8A8_UNORM,
                    static_cast<TextureUsages>(TextureUsage::ColorAttachment)),
            &texture));

    CommandBuffer* commandBuffer = nullptr;
    ASSERT_RHI_SUCCESS(api->createCommandBuffer(CommandBufferDescriptor{}, &commandBuffer));
    ASSERT_RHI_SUCCESS(commandBuffer->begin());
    ASSERT_RHI_SUCCESS(commandBuffer->clearColorTexture(texture, TextureSubresourceRange::all(), Color::black()));
    ASSERT_RHI_SUCCESS(commandBuffer->end());

    FenceHandle fence = FenceHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createFence(FenceDescriptor{}, &fence));

    Queue* queue = api->getQueue(QueueType::Graphics);
    ASSERT_NE(queue, nullptr);
    ASSERT_RHI_SUCCESS(queue->submit(*commandBuffer, fence));
    EXPECT_FALSE(api->isResourceReadyToDestroy(texture))
            << "an asynchronous submission must keep resources pending until its fence is waited";

    ASSERT_RHI_SUCCESS(api->waitForFence(fence));
    ASSERT_RHI_SUCCESS(commandBuffer->reset());
    EXPECT_TRUE(api->isResourceReadyToDestroy(texture));

    api->destroyFence(fence);
    api->destroyCommandBuffer(commandBuffer);
    api->destroyTexture(texture);
    EXPECT_RHI_SUCCESS(api->waitIdle());
  });
}

TEST(PlatformRunner, MultisampleResolveProducesTheDrawnColour) {
  forEachExpectedBackend([](const ExpectedBackend& expected) {
    // The OpenGL backend's MSAA resolve is a different path; this test exists for the Vulkan resolve
    // attachment (dynamic rendering) and the `vkCmdResolveImage` fallback used on 1.0/1.1 devices.
    if (expected.backend != Backend::Vulkan) {
      return;
    }

    DeviceHandle device;
    Device* api = nullptr;
    if (!createDeviceOrSkip(device, expected, api)) {
      return;
    }

    const Capabilities& capabilities = api->getCapabilities();
    if (!capabilities.supports(Feature::MultisampleResolve) || !capabilities.supportsSampleCount(4)) {
      GTEST_SKIP() << "this Vulkan device does not support 4x MSAA resolve";
    }

    constexpr uint32_t kSize = 32;
    constexpr uint32_t kSampleCount = 4;
    constexpr auto rgba = [](const uint32_t red, const uint32_t green, const uint32_t blue) {
      return 0xFF000000u | (blue << 16) | (green << 8) | red;
    };
    const uint32_t kDrawn = rgba(0xFF, 0x00, 0x00);
    const uint32_t kClear = rgba(0x00, 0x00, 0x00);

    // The multisampled attachment and its single-sampled resolve destination. TransferSrc/Dst are what
    // `vkCmdResolveImage` needs; the backend adds the bits internally too, so a caller that only names
    // ColorAttachment/Sampled (as the canvas does) still works.
    TextureDescriptor multisampleDescriptor = TextureDescriptor::texture2D(kSize, kSize,
            PixelFormat::R8G8B8A8_UNORM, static_cast<TextureUsages>(TextureUsage::ColorAttachment));
    multisampleDescriptor.sampleCount = kSampleCount;
    TextureHandle multisample = TextureHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createTexture(multisampleDescriptor, &multisample));

    TextureViewDescriptor multisampleViewDescriptor;
    multisampleViewDescriptor.texture = multisample;
    multisampleViewDescriptor.dimension = TextureViewDimension::Texture2D;
    TextureViewHandle multisampleView = TextureViewHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createTextureView(multisampleViewDescriptor, &multisampleView));

    TextureHandle resolved = TextureHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createTexture(
            TextureDescriptor::texture2D(kSize, kSize, PixelFormat::R8G8B8A8_UNORM,
                    static_cast<TextureUsages>(TextureUsage::ColorAttachment | TextureUsage::Sampled)),
            &resolved));
    TextureViewDescriptor resolvedViewDescriptor;
    resolvedViewDescriptor.texture = resolved;
    resolvedViewDescriptor.dimension = TextureViewDimension::Texture2D;
    TextureViewHandle resolvedView = TextureViewHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createTextureView(resolvedViewDescriptor, &resolvedView));

    RenderPassDescriptor passDescriptor;
    passDescriptor.colorAttachmentCount = 1;
    passDescriptor.colorFormats[0] = PixelFormat::R8G8B8A8_UNORM;
    passDescriptor.sampleCount = kSampleCount;
    RenderPassHandle renderPass = RenderPassHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createRenderPass(passDescriptor, &renderPass));

    const std::string vertexSource = "#version 310 es\n"
                                     "layout(location = 0) in vec2 aPos;\n"
                                     "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";
    const std::string fragmentSource = "#version 310 es\nprecision mediump float;\n"
                                       "layout(location = 0) out vec4 o;\n"
                                       "void main() { o = vec4(1.0, 0.0, 0.0, 1.0); }\n";
    const auto makeModule = [&](const std::string& source, const ShaderStage stage) {
      ShaderModuleDescriptor descriptor;
      descriptor.stage = stage;
      descriptor.language = ShaderModuleDescriptor::Language::GLSL;
      descriptor.code = source.c_str();
      descriptor.codeSize = source.size() + 1;
      descriptor.label = "multisample-resolve-test";
      ShaderModuleHandle handle = ShaderModuleHandle::invalid();
      const Result result = api->createShaderModule(descriptor, &handle);
      return std::make_pair(result, handle);
    };
    const auto [vertexResult, vertexModule] = makeModule(vertexSource, ShaderStage::Vertex);
    if (!isSuccess(vertexResult)) {
      GTEST_SKIP() << "this build cannot compile GLSL source into a shader module";
    }
    const auto [fragmentResult, fragmentModule] = makeModule(fragmentSource, ShaderStage::Fragment);
    ASSERT_RHI_SUCCESS(fragmentResult);

    PipelineLayoutDescriptor pipelineLayoutDescriptor;
    PipelineLayoutHandle pipelineLayout = PipelineLayoutHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createPipelineLayout(pipelineLayoutDescriptor, &pipelineLayout));

    const float triangle[6] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
    BufferHandle vertices = BufferHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createBuffer(BufferDescriptor::vertex(sizeof(triangle), triangle), &vertices));

    RenderPipelineDescriptor pipelineDescriptor;
    pipelineDescriptor.layout = pipelineLayout;
    pipelineDescriptor.vertexModule = vertexModule;
    pipelineDescriptor.fragmentModule = fragmentModule;
    pipelineDescriptor.vertexInput.bufferCount = 1;
    pipelineDescriptor.vertexInput.buffers[0].stride = sizeof(float) * 2;
    pipelineDescriptor.vertexInput.buffers[0].stepMode = VertexStepMode::Vertex;
    pipelineDescriptor.vertexInput.buffers[0].firstAttribute = 0;
    pipelineDescriptor.vertexInput.buffers[0].attributeCount = 1;
    pipelineDescriptor.vertexInput.attributeCount = 1;
    pipelineDescriptor.vertexInput.attributes[0].shaderLocation = 0;
    pipelineDescriptor.vertexInput.attributes[0].bufferIndex = 0;
    pipelineDescriptor.vertexInput.attributes[0].format = VertexFormat::Float2;
    pipelineDescriptor.vertexInput.attributes[0].offset = 0;
    pipelineDescriptor.depthStencil.depthTestEnable = false;
    pipelineDescriptor.depthStencil.depthWriteEnable = false;
    pipelineDescriptor.colorTargetCount = 1;
    pipelineDescriptor.colorTargets[0].format = PixelFormat::R8G8B8A8_UNORM;
    pipelineDescriptor.multisample.sampleCount = kSampleCount;
    pipelineDescriptor.compatibleRenderPass = renderPass;
    pipelineDescriptor.label = "multisample-resolve-test";
    RenderPipelineHandle pipeline = RenderPipelineHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createRenderPipeline(pipelineDescriptor, &pipeline));

    const uint64_t readbackSize = static_cast<uint64_t>(kSize) * kSize * 4;
    BufferDescriptor readbackDescriptor;
    readbackDescriptor.size = readbackSize;
    readbackDescriptor.usage = static_cast<BufferUsages>(BufferUsage::TransferDst);
    readbackDescriptor.memoryUsage = MemoryUsage::GpuToCpu;
    readbackDescriptor.label = "multisample-resolve-readback";
    BufferHandle readback = BufferHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createBuffer(readbackDescriptor, &readback));

    CommandBuffer* commandBuffer = nullptr;
    ASSERT_RHI_SUCCESS(api->createCommandBuffer(CommandBufferDescriptor{}, &commandBuffer));
    ASSERT_RHI_SUCCESS(commandBuffer->begin());

    RenderPassBeginInfo passInfo;
    passInfo.renderPass = renderPass;
    passInfo.colorAttachmentCount = 1;
    passInfo.colorAttachments[0].view = multisampleView;
    passInfo.colorAttachments[0].resolveTarget = resolvedView;
    passInfo.colorAttachments[0].loadOp = LoadOp::Clear;
    passInfo.colorAttachments[0].storeOp = StoreOp::Store;
    passInfo.colorAttachments[0].clearColor = Color::black();
    passInfo.renderArea = { kSize, kSize };
    ASSERT_RHI_SUCCESS(commandBuffer->beginRenderPass(passInfo));
    ASSERT_RHI_SUCCESS(commandBuffer->bindRenderPipeline(pipeline));
    ASSERT_RHI_SUCCESS(commandBuffer->setVertexBuffer(0, vertices, 0));
    ASSERT_RHI_SUCCESS(commandBuffer->draw(3, 1, 0, 0));
    ASSERT_RHI_SUCCESS(commandBuffer->endRenderPass());

    BufferTextureCopy copy;
    copy.buffer = readback;
    copy.texture = resolved;
    copy.bufferRegion.dstOffset = 0;
    copy.bufferRegion.size = readbackSize;
    copy.textureRegion.textureOrigin = { 0, 0, 0 };
    copy.textureRegion.size = { kSize, kSize, 1 };
    copy.textureRegion.mipLevel = 0;
    copy.textureRegion.baseArrayLayer = 0;
    copy.textureRegion.arrayLayerCount = 1;
    ASSERT_RHI_SUCCESS(commandBuffer->copyTextureToBuffer(copy));
    ASSERT_RHI_SUCCESS(commandBuffer->end());
    ASSERT_RHI_SUCCESS(api->executeCommandBuffer(*commandBuffer));
    ASSERT_RHI_SUCCESS(api->waitIdle());

    void* mapped = nullptr;
    ASSERT_RHI_SUCCESS(api->mapBuffer(readback, 0, readbackSize, &mapped));
    ASSERT_NE(mapped, nullptr);
    const auto* pixels = static_cast<const uint32_t*>(mapped);
    const uint32_t centre = pixels[(kSize / 2) * kSize + kSize / 2];
    api->unmapBuffer(readback);

    std::printf("  multisample resolve: centre %08x (expected %08x), clear %08x\n", centre, kDrawn, kClear);
    EXPECT_EQ(centre, kDrawn) << "the resolved image does not contain the colour the pass drew";

    api->destroyBuffer(readback);
    api->destroyCommandBuffer(commandBuffer);
    api->destroyBuffer(vertices);
    api->destroyRenderPipeline(pipeline);
    api->destroyPipelineLayout(pipelineLayout);
    api->destroyShaderModule(vertexModule);
    api->destroyShaderModule(fragmentModule);
    api->destroyRenderPass(renderPass);
    api->destroyTextureView(resolvedView);
    api->destroyTexture(resolved);
    api->destroyTextureView(multisampleView);
    api->destroyTexture(multisample);
    EXPECT_RHI_SUCCESS(api->waitIdle());
  });
}

/**
 * The GLSL-direct path, end to end: a shader handed to the driver as written, with the caller's own
 * name -> location table standing in for the reflection a SPIR-V module would have carried.
 *
 * ## What this proves that a compile-only check cannot
 *
 * That the declared locations actually reached the linked program. `glBindAttribLocation` is the only
 * mechanism involved and it says nothing about a name it does not recognise, so a check that stopped at
 * "the module compiled" would pass whether or not any of it worked: the draw would look its vertex data
 * up at a location the shader never bound, which one driver turns into an all-zero attribute and another
 * into whatever the previous draw left behind.
 *
 * The location is deliberately not 0. A driver that ignored the declaration would assign the input to
 * *some* location, and 0 is the one it would most plausibly choose; asking for 2 makes "the binding was
 * applied" and "the binding was ignored" produce different pixels rather than the same ones.
 */
TEST(PlatformRunner, DirectGlslDrawsWithTheCallersLocationTable) {
  forEachExpectedBackend([](const ExpectedBackend& expected) {
    DeviceHandle device;
    Device* api = nullptr;
    if (!createDeviceOrSkip(device, expected, api)) {
      GTEST_SKIP() << expected.name << " is compiled in but not implemented yet";
    }
    if (!api->getCapabilities().acceptsGlslDirect) {
      GTEST_SKIP() << expected.name << " does not take GLSL source directly";
    }

    constexpr uint32_t kSize = 32;
    constexpr uint32_t kAttributeLocation = 2;
    constexpr auto rgba = [](const uint32_t red, const uint32_t green, const uint32_t blue) {
      return 0xFF000000u | (blue << 16) | (green << 8) | red;
    };
    const uint32_t kDrawn = rgba(0xFF, 0x00, 0x00);
    const uint32_t kClear = rgba(0x00, 0x00, 0x00);

    // ---------------------------------------------------------------- target
    TextureHandle target = TextureHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createTexture(
            TextureDescriptor::texture2D(kSize, kSize, PixelFormat::R8G8B8A8_UNORM,
                    static_cast<TextureUsages>(TextureUsage::ColorAttachment)),
            &target));

    TextureViewDescriptor targetViewDescriptor;
    targetViewDescriptor.texture = target;
    targetViewDescriptor.dimension = TextureViewDimension::Texture2D;
    TextureViewHandle targetView = TextureViewHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createTextureView(targetViewDescriptor, &targetView));

    RenderPassDescriptor passDescriptor;
    passDescriptor.colorAttachmentCount = 1;
    passDescriptor.colorFormats[0] = PixelFormat::R8G8B8A8_UNORM;
    passDescriptor.sampleCount = 1;
    RenderPassHandle renderPass = RenderPassHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createRenderPass(passDescriptor, &renderPass));

    FramebufferDescriptor framebufferDescriptor;
    framebufferDescriptor.renderPass = renderPass;
    framebufferDescriptor.colorAttachmentCount = 1;
    framebufferDescriptor.colorAttachments[0] = targetView;
    framebufferDescriptor.width = kSize;
    framebufferDescriptor.height = kSize;
    FramebufferHandle framebuffer = FramebufferHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createFramebuffer(framebufferDescriptor, &framebuffer));

    // ---------------------------------------------------------------- the sources
    //
    // Written for the dialect this device came up in, because nothing translates them: a `#version 300
    // es` source handed to a desktop core context is a syntax error, which is the price of skipping the
    // translator and the reason the dialect is read from `expected` rather than guessed.
    const bool es = expected.dialect == GlDialect::ES;
    const std::string vertexSource = es
      ? "#version 300 es\nin vec2 aPos;\nvoid main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n"
      : "#version 410 core\nin vec2 aPos;\nvoid main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";
    // The vertex input has no `layout(location = ...)`, which is the whole point: the location comes
    // from the declaration table below or from nowhere.
    const std::string fragmentSource = es
      ? "#version 300 es\nprecision mediump float;\nlayout(location = 0) out vec4 o;\n"
        "void main() { o = vec4(1.0, 0.0, 0.0, 1.0); }\n"
      : "#version 410 core\nlayout(location = 0) out vec4 o;\n"
        "void main() { o = vec4(1.0, 0.0, 0.0, 1.0); }\n";

    ShaderAttributeDeclaration attribute;
    attribute.location = kAttributeLocation;
    attribute.name = "aPos";

    ShaderModuleDescriptor vertexDescriptor;
    vertexDescriptor.stage = ShaderStage::Vertex;
    vertexDescriptor.language = ShaderModuleDescriptor::Language::GLSLDirect;
    vertexDescriptor.code = vertexSource.c_str();
    vertexDescriptor.codeSize = vertexSource.size() + 1; // a source module's size includes the terminator
    vertexDescriptor.attributeDeclarations = &attribute;
    vertexDescriptor.attributeDeclarationCount = 1;
    vertexDescriptor.label = "direct-glsl-vertex";

    ShaderModuleHandle vertexModule = ShaderModuleHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createShaderModule(vertexDescriptor, &vertexModule));

    // The provenance queries answer for a direct module rather than reporting "no source": the text was
    // read for its version and hashed, and `directGlsl` is what tells this module apart from one that
    // went through the compiler.
    ShaderSourceInfo sourceInfo;
    ASSERT_RHI_SUCCESS(api->getShaderSourceInfo(vertexModule, &sourceInfo));
    EXPECT_TRUE(sourceInfo.directGlsl);
    EXPECT_EQ(sourceInfo.version, es ? 300u : 410u);
    EXPECT_NE(sourceInfo.sourceHash, 0u);

    // There is no SPIR-V to hand back, and saying so is the honest answer rather than an empty vector: a
    // caller deciding whether this module can be cached must not be told that it can.
    std::vector<uint32_t> words;
    EXPECT_EQ(api->getShaderSpirv(vertexModule, &words), Result::Unsupported);

    // And the binding table is the caller's, so the query reports it instead of refusing the way it does
    // for a hand-written GLSL source whose bindings it cannot see. A vertex stage with no sampler and no
    // block declares no bindings, and reporting that is a different statement from "cannot tell you".
    std::vector<ShaderBindingDeclaration> reflected;
    EXPECT_EQ(api->getShaderReflection(vertexModule, &reflected), Result::Success);
    EXPECT_TRUE(reflected.empty()) << "a vertex stage that declares no bindings reports none";

    ShaderModuleDescriptor fragmentDescriptor;
    fragmentDescriptor.stage = ShaderStage::Fragment;
    fragmentDescriptor.language = ShaderModuleDescriptor::Language::GLSLDirect;
    fragmentDescriptor.code = fragmentSource.c_str();
    fragmentDescriptor.codeSize = fragmentSource.size() + 1;
    fragmentDescriptor.label = "direct-glsl-fragment";
    ShaderModuleHandle fragmentModule = ShaderModuleHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createShaderModule(fragmentDescriptor, &fragmentModule));

    // ---------------------------------------------------------------- pipeline
    PipelineLayoutDescriptor pipelineLayoutDescriptor;
    PipelineLayoutHandle pipelineLayout = PipelineLayoutHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createPipelineLayout(pipelineLayoutDescriptor, &pipelineLayout));

    RenderPipelineDescriptor pipelineDescriptor;
    pipelineDescriptor.layout = pipelineLayout;
    pipelineDescriptor.vertexModule = vertexModule;
    pipelineDescriptor.fragmentModule = fragmentModule;
    pipelineDescriptor.vertexInput.bufferCount = 1;
    pipelineDescriptor.vertexInput.buffers[0].stride = sizeof(float) * 2;
    pipelineDescriptor.vertexInput.buffers[0].stepMode = VertexStepMode::Vertex;
    pipelineDescriptor.vertexInput.buffers[0].firstAttribute = 0;
    pipelineDescriptor.vertexInput.buffers[0].attributeCount = 1;
    pipelineDescriptor.vertexInput.attributeCount = 1;
    pipelineDescriptor.vertexInput.attributes[0].shaderLocation = kAttributeLocation;
    pipelineDescriptor.vertexInput.attributes[0].bufferIndex = 0;
    pipelineDescriptor.vertexInput.attributes[0].format = VertexFormat::Float2;
    pipelineDescriptor.vertexInput.attributes[0].offset = 0;
    pipelineDescriptor.depthStencil.depthTestEnable = false;
    pipelineDescriptor.depthStencil.depthWriteEnable = false;
    pipelineDescriptor.colorTargetCount = 1;
    pipelineDescriptor.colorTargets[0].format = PixelFormat::R8G8B8A8_UNORM;
    pipelineDescriptor.compatibleRenderPass = renderPass;
    pipelineDescriptor.label = "direct-glsl-pipeline";
    RenderPipelineHandle pipeline = RenderPipelineHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createRenderPipeline(pipelineDescriptor, &pipeline));

    // ---------------------------------------------------------------- geometry and readback buffer
    // One triangle that covers the whole target, so the pixel read back is the fragment colour whatever
    // the rasteriser does with the edges.
    const float kTriangle[6] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
    BufferHandle vertices = BufferHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createBuffer(BufferDescriptor::vertex(sizeof(kTriangle), kTriangle), &vertices));

    const uint64_t readbackSize = static_cast<uint64_t>(kSize) * kSize * 4;
    BufferDescriptor readbackDescriptor;
    readbackDescriptor.size = readbackSize;
    readbackDescriptor.usage = static_cast<BufferUsages>(BufferUsage::TransferDst);
    readbackDescriptor.memoryUsage = MemoryUsage::GpuToCpu;
    readbackDescriptor.label = "direct-glsl-readback";
    BufferHandle readback = BufferHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createBuffer(readbackDescriptor, &readback));

    // ---------------------------------------------------------------- record
    CommandBuffer* commandBuffer = nullptr;
    ASSERT_RHI_SUCCESS(api->createCommandBuffer(CommandBufferDescriptor{}, &commandBuffer));
    ASSERT_RHI_SUCCESS(commandBuffer->begin());

    RenderPassBeginInfo passInfo;
    passInfo.renderPass = renderPass;
    passInfo.colorAttachmentCount = 1;
    passInfo.colorAttachments[0].view = targetView;
    passInfo.colorAttachments[0].loadOp = LoadOp::Clear;
    passInfo.colorAttachments[0].storeOp = StoreOp::Store;
    passInfo.colorAttachments[0].clearColor = Color::black();
    passInfo.renderArea = { kSize, kSize };
    ASSERT_RHI_SUCCESS(commandBuffer->beginRenderPass(passInfo));

    ASSERT_RHI_SUCCESS(commandBuffer->bindRenderPipeline(pipeline));
    ASSERT_RHI_SUCCESS(commandBuffer->setVertexBuffer(0, vertices, 0));
    ASSERT_RHI_SUCCESS(commandBuffer->draw(3, 1, 0, 0));
    ASSERT_RHI_SUCCESS(commandBuffer->endRenderPass());

    // A pass that draws nothing, to make the target's framebuffer current before the read: the OpenGL
    // backend's `copyTextureToBuffer` is `glReadPixels`, which reads whatever framebuffer is bound.
    RenderPassBeginInfo readbackPass;
    readbackPass.renderPass = renderPass;
    readbackPass.colorAttachmentCount = 1;
    readbackPass.colorAttachments[0].view = targetView;
    readbackPass.colorAttachments[0].loadOp = LoadOp::Load;
    readbackPass.colorAttachments[0].storeOp = StoreOp::Store;
    readbackPass.renderArea = { kSize, kSize };
    ASSERT_RHI_SUCCESS(commandBuffer->beginRenderPass(readbackPass));
    ASSERT_RHI_SUCCESS(commandBuffer->endRenderPass());

    BufferTextureCopy copy;
    copy.buffer = readback;
    copy.texture = target;
    copy.bufferRegion.dstOffset = 0;
    copy.bufferRegion.size = readbackSize;
    copy.textureRegion.textureOrigin = { 0, 0, 0 };
    copy.textureRegion.size = { kSize, kSize, 1 };
    copy.textureRegion.mipLevel = 0;
    copy.textureRegion.baseArrayLayer = 0;
    copy.textureRegion.arrayLayerCount = 1;
    ASSERT_RHI_SUCCESS(commandBuffer->copyTextureToBuffer(copy));

    ASSERT_RHI_SUCCESS(commandBuffer->end());
    ASSERT_RHI_SUCCESS(api->executeCommandBuffer(*commandBuffer));
    ASSERT_RHI_SUCCESS(api->waitIdle());

    void* mapped = nullptr;
    ASSERT_RHI_SUCCESS(api->mapBuffer(readback, 0, readbackSize, &mapped));
    ASSERT_NE(mapped, nullptr);
    const auto* pixels = static_cast<const uint32_t*>(mapped);
    const uint32_t centrePixel = pixels[(kSize / 2) * kSize + (kSize / 2)];
    const uint32_t cornerPixel = pixels[0];
    api->unmapBuffer(readback);

    std::printf("  direct GLSL: centre %08x (expected %08x), corner %08x (expected %08x)\n", centrePixel, kDrawn,
            cornerPixel, kDrawn);

    // The corner is read as well as the centre because a triangle covering the target should cover it
    // entirely; a pixel that came back as the clear colour would mean the geometry was rejected rather
    // than that the attribute landed in the wrong slot, and the two have different causes.
    EXPECT_EQ(centrePixel, kDrawn)
            << "the vertex input did not arrive at location " << kAttributeLocation
            << ", so the triangle collapsed or read no vertex data";
    EXPECT_EQ(cornerPixel, kDrawn) << "the triangle did not cover the target";

    api->destroyBuffer(readback);
    api->destroyBuffer(vertices);
    api->destroyCommandBuffer(commandBuffer);
    api->destroyRenderPipeline(pipeline);
    api->destroyPipelineLayout(pipelineLayout);
    api->destroyShaderModule(fragmentModule);
    api->destroyShaderModule(vertexModule);
    api->destroyFramebuffer(framebuffer);
    api->destroyRenderPass(renderPass);
    api->destroyTextureView(targetView);
    api->destroyTexture(target);
    EXPECT_RHI_SUCCESS(api->waitIdle());
  });
}

// ==================================================================================================
// Push constants
// ==================================================================================================
//
// The one feature whose only route to the GPU is a module the *library's* compiler cannot produce.
// `layout(push_constant)` is Vulkan's spelling and glslang refuses it for the OpenGL target environment at every
// version, so there is no way to write a push-constant shader in GLSL, hand it to `createShaderModule`, and have
// shaderc compile it: the words have to come from a **Vulkan**-environment compile. This test asks shaderc for
// exactly that, which is why the test target links shaderc itself.
//
// What it pins is the defect that was found and fixed: the emulation existed, was validated, was budgeted for by
// `Limits::maxPushConstantSize`, and never fired -- the block was skipped out of the program's binding table, and
// the upload only happened for a caller that also named the same binding in a descriptor set. So the shader read
// zeros and drew black, with no error anywhere. The assertion is chosen to catch precisely that: the pushed colour
// has a non-zero value in every channel, so a block that read as zeros comes back black and fails.

#if CORAL_BACKEND_HAS_SHADERC

/** @return @p source compiled to SPIR-V for the **Vulkan** environment, or an empty vector with @p error set. */
std::vector<uint32_t> compileForVulkan(const char* source, const shaderc_shader_kind kind, std::string* error) {
  shaderc_compiler_t compiler = shaderc_compiler_initialize();
  if (compiler == nullptr) {
    *error = "shaderc_compiler_initialize failed";
    return {};
  }
  shaderc_compile_options_t options = shaderc_compile_options_initialize();
  // The whole reason this helper exists: the OpenGL environment rejects `layout(push_constant)` outright.
  shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);

  shaderc_compilation_result_t result = shaderc_compile_into_spv(compiler, source, std::strlen(source), kind,
          "push-constant-test", "main", options);
  std::vector<uint32_t> words;
  if (result == nullptr || shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success) {
    *error = result != nullptr ? shaderc_result_get_error_message(result) : "shaderc_compile_into_spv failed";
  } else {
    const auto* bytes = reinterpret_cast<const uint32_t*>(shaderc_result_get_bytes(result));
    words.assign(bytes, bytes + shaderc_result_get_length(result) / sizeof(uint32_t));
  }

  if (result != nullptr) shaderc_result_release(result);
  shaderc_compile_options_release(options);
  shaderc_compiler_release(compiler);
  return words;
}

TEST(PlatformRunner, PushConstantsReachTheShader) {
  forEachExpectedBackend([](const ExpectedBackend& expected) {
    DeviceHandle device;
    Device* api = nullptr;
    if (!createDeviceOrSkip(device, expected, api)) {
      GTEST_SKIP() << expected.name << " is compiled in but not implemented yet";
    }

    constexpr uint32_t kSize = 32;
    // A colour with a non-zero value in every channel, so "the block read as zeros" is distinguishable from "the
    // block read the pushed bytes" by looking at one pixel.
    const struct { float r, g, b, a; } kPushed = { 64.0f / 255.0f, 128.0f / 255.0f, 192.0f / 255.0f, 1.0f };

    std::string error;
    const char* vertexSource = R"(#version 450
layout(location = 0) in vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
)";
    const char* fragmentSource = R"(#version 450
layout(location = 0) out vec4 oColor;
layout(push_constant) uniform Params { vec4 uColour; } params;
void main() { oColor = params.uColour; }
)";
    const std::vector<uint32_t> vertexWords = compileForVulkan(vertexSource, shaderc_vertex_shader, &error);
    const std::vector<uint32_t> fragmentWords = compileForVulkan(fragmentSource, shaderc_fragment_shader, &error);
    if (vertexWords.empty() || fragmentWords.empty()) {
      GTEST_SKIP() << "the Vulkan-environment compile is unavailable: " << error;
    }

    // ---------------------------------------------------------------- target
    TextureHandle target = TextureHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createTexture(
            TextureDescriptor::texture2D(kSize, kSize, PixelFormat::R8G8B8A8_UNORM,
                    static_cast<TextureUsages>(TextureUsage::ColorAttachment)), &target));
    TextureViewDescriptor targetViewDescriptor;
    targetViewDescriptor.texture = target;
    targetViewDescriptor.dimension = TextureViewDimension::Texture2D;
    TextureViewHandle targetView = TextureViewHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createTextureView(targetViewDescriptor, &targetView));

    RenderPassDescriptor passDescriptor;
    passDescriptor.colorAttachmentCount = 1;
    passDescriptor.colorFormats[0] = PixelFormat::R8G8B8A8_UNORM;
    passDescriptor.sampleCount = 1;
    RenderPassHandle renderPass = RenderPassHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createRenderPass(passDescriptor, &renderPass));

    // ---------------------------------------------------------------- modules
    ShaderModuleDescriptor vertexDescriptor;
    vertexDescriptor.stage = ShaderStage::Vertex;
    vertexDescriptor.language = ShaderModuleDescriptor::Language::SPIRV;
    vertexDescriptor.code = vertexWords.data();
    vertexDescriptor.codeSize = vertexWords.size() * sizeof(uint32_t);
    vertexDescriptor.label = "push-constant-vertex";
    ShaderModuleHandle vertexModule = ShaderModuleHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createShaderModule(vertexDescriptor, &vertexModule));

    ShaderModuleDescriptor fragmentDescriptor;
    fragmentDescriptor.stage = ShaderStage::Fragment;
    fragmentDescriptor.language = ShaderModuleDescriptor::Language::SPIRV;
    fragmentDescriptor.code = fragmentWords.data();
    fragmentDescriptor.codeSize = fragmentWords.size() * sizeof(uint32_t);
    fragmentDescriptor.label = "push-constant-fragment";
    ShaderModuleHandle fragmentModule = ShaderModuleHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createShaderModule(fragmentDescriptor, &fragmentModule));

    // ---------------------------------------------------------------- pipeline
    PipelineLayoutDescriptor pipelineLayoutDescriptor;
    PipelineLayoutHandle pipelineLayout = PipelineLayoutHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createPipelineLayout(pipelineLayoutDescriptor, &pipelineLayout));

    RenderPipelineDescriptor pipelineDescriptor;
    pipelineDescriptor.layout = pipelineLayout;
    pipelineDescriptor.vertexModule = vertexModule;
    pipelineDescriptor.fragmentModule = fragmentModule;
    pipelineDescriptor.vertexInput.bufferCount = 1;
    pipelineDescriptor.vertexInput.buffers[0].stride = sizeof(float) * 2;
    pipelineDescriptor.vertexInput.buffers[0].stepMode = VertexStepMode::Vertex;
    pipelineDescriptor.vertexInput.buffers[0].attributeCount = 1;
    pipelineDescriptor.vertexInput.attributeCount = 1;
    pipelineDescriptor.vertexInput.attributes[0].shaderLocation = 0;
    pipelineDescriptor.vertexInput.attributes[0].bufferIndex = 0;
    pipelineDescriptor.vertexInput.attributes[0].format = VertexFormat::Float2;
    pipelineDescriptor.depthStencil.depthTestEnable = false;
    pipelineDescriptor.depthStencil.depthWriteEnable = false;
    pipelineDescriptor.colorTargetCount = 1;
    pipelineDescriptor.colorTargets[0].format = PixelFormat::R8G8B8A8_UNORM;
    pipelineDescriptor.compatibleRenderPass = renderPass;
    pipelineDescriptor.label = "push-constant-pipeline";
    RenderPipelineHandle pipeline = RenderPipelineHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createRenderPipeline(pipelineDescriptor, &pipeline));

    // ---------------------------------------------------------------- geometry and readback
    const float kTriangle[6] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
    BufferHandle vertices = BufferHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createBuffer(BufferDescriptor::vertex(sizeof(kTriangle), kTriangle), &vertices));

    const uint64_t readbackSize = static_cast<uint64_t>(kSize) * kSize * 4;
    BufferDescriptor readbackDescriptor;
    readbackDescriptor.size = readbackSize;
    readbackDescriptor.usage = static_cast<BufferUsages>(BufferUsage::TransferDst);
    readbackDescriptor.memoryUsage = MemoryUsage::GpuToCpu;
    readbackDescriptor.label = "push-constant-readback";
    BufferHandle readback = BufferHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createBuffer(readbackDescriptor, &readback));

    // ---------------------------------------------------------------- record
    CommandBuffer* commandBuffer = nullptr;
    ASSERT_RHI_SUCCESS(api->createCommandBuffer(CommandBufferDescriptor{}, &commandBuffer));
    ASSERT_RHI_SUCCESS(commandBuffer->begin());

    RenderPassBeginInfo passInfo;
    passInfo.renderPass = renderPass;
    passInfo.colorAttachmentCount = 1;
    passInfo.colorAttachments[0].view = targetView;
    passInfo.colorAttachments[0].loadOp = LoadOp::Clear;
    passInfo.colorAttachments[0].storeOp = StoreOp::Store;
    passInfo.colorAttachments[0].clearColor = Color::black();
    passInfo.renderArea = { kSize, kSize };
    ASSERT_RHI_SUCCESS(commandBuffer->beginRenderPass(passInfo));

    ASSERT_RHI_SUCCESS(commandBuffer->bindRenderPipeline(pipeline));
    ASSERT_RHI_SUCCESS(commandBuffer->setVertexBuffer(0, vertices, 0));
    // The call under test, recorded between the pipeline and the draw -- the same place a caller would put it.
    ASSERT_RHI_SUCCESS(commandBuffer->pushConstants(&kPushed, sizeof(kPushed)));
    ASSERT_RHI_SUCCESS(commandBuffer->draw(3, 1, 0, 0));
    ASSERT_RHI_SUCCESS(commandBuffer->endRenderPass());

    RenderPassBeginInfo readbackPass;
    readbackPass.renderPass = renderPass;
    readbackPass.colorAttachmentCount = 1;
    readbackPass.colorAttachments[0].view = targetView;
    readbackPass.colorAttachments[0].loadOp = LoadOp::Load;
    readbackPass.colorAttachments[0].storeOp = StoreOp::Store;
    readbackPass.renderArea = { kSize, kSize };
    ASSERT_RHI_SUCCESS(commandBuffer->beginRenderPass(readbackPass));
    ASSERT_RHI_SUCCESS(commandBuffer->endRenderPass());

    BufferTextureCopy copy;
    copy.buffer = readback;
    copy.texture = target;
    copy.bufferRegion.size = readbackSize;
    copy.textureRegion.size = { kSize, kSize, 1 };
    copy.textureRegion.arrayLayerCount = 1;
    ASSERT_RHI_SUCCESS(commandBuffer->copyTextureToBuffer(copy));

    ASSERT_RHI_SUCCESS(commandBuffer->end());
    ASSERT_RHI_SUCCESS(api->executeCommandBuffer(*commandBuffer));
    ASSERT_RHI_SUCCESS(api->waitIdle());

    void* mapped = nullptr;
    ASSERT_RHI_SUCCESS(api->mapBuffer(readback, 0, readbackSize, &mapped));
    ASSERT_NE(mapped, nullptr);
    const uint32_t centre = static_cast<const uint32_t*>(mapped)[(kSize / 2) * kSize + (kSize / 2)];
    api->unmapBuffer(readback);

    const uint32_t red = centre & 0xFFu;
    const uint32_t green = (centre >> 8) & 0xFFu;
    const uint32_t blue = (centre >> 16) & 0xFFu;
    std::printf("  push constants: centre %08x -> rgba(%u, %u, %u)\n", centre, red, green, blue);

    // A one-step tolerance per channel: the pushed values are exact in 8 bits, but a driver may round rather than
    // truncate the float-to-UNORM conversion, and which of the two it does is not what this test is about.
    EXPECT_NEAR(red, 64, 1) << "the red channel did not come from the pushed block";
    EXPECT_NEAR(green, 128, 1) << "the green channel did not come from the pushed block";
    EXPECT_NEAR(blue, 192, 1) << "the blue channel did not come from the pushed block";
    // The clear colour is black and the pushed colour is not, so a block that never received the bytes reads as
    // zeros here -- the defect this covers, invisible without this assertion.
    EXPECT_NE(centre & 0x00FFFFFFu, 0u) << "the push constants never reached the shader (it drew the zero block)";

    api->destroyBuffer(readback);
    api->destroyBuffer(vertices);
    api->destroyCommandBuffer(commandBuffer);
    api->destroyRenderPipeline(pipeline);
    api->destroyPipelineLayout(pipelineLayout);
    api->destroyShaderModule(fragmentModule);
    api->destroyShaderModule(vertexModule);
    api->destroyRenderPass(renderPass);
    api->destroyTextureView(targetView);
    api->destroyTexture(target);
    EXPECT_RHI_SUCCESS(api->waitIdle());
  });
}

TEST(PlatformRunner, SpirvModuleIntrospectionReturnsTheCallersWords) {
  forEachExpectedBackend([](const ExpectedBackend& expected) {
    DeviceHandle device;
    Device* api = nullptr;
    if (!createDeviceOrSkip(device, expected, api)) return;
    if (!api->getCapabilities().acceptsSpirv) {
      GTEST_SKIP() << expected.name << " does not accept SPIR-V in this build";
    }

    const char* source = R"(#version 450
layout(location = 0) out vec4 oColor;
void main() { oColor = vec4(1.0); }
)";
    std::string error;
    const std::vector<uint32_t> input = compileForVulkan(source, shaderc_fragment_shader, &error);
    if (input.empty()) GTEST_SKIP() << "the Vulkan-environment compile is unavailable: " << error;

    ShaderModuleDescriptor descriptor;
    descriptor.stage = ShaderStage::Fragment;
    descriptor.language = ShaderModuleDescriptor::Language::SPIRV;
    descriptor.code = input.data();
    descriptor.codeSize = input.size() * sizeof(uint32_t);
    descriptor.label = "spirv-introspection";
    ShaderModuleHandle module = ShaderModuleHandle::invalid();
    ASSERT_RHI_SUCCESS(api->createShaderModule(descriptor, &module));

    std::vector<uint32_t> returned;
    ASSERT_RHI_SUCCESS(api->getShaderSpirv(module, &returned));
    EXPECT_EQ(returned, input) << "a caller-supplied module must report its own words";

    ShaderSourceInfo info;
    ASSERT_RHI_SUCCESS(api->getShaderSourceInfo(module, &info));
    EXPECT_TRUE(info.spirvInput) << "the provenance must say the module came in as SPIR-V";
    EXPECT_EQ(info.sourceSize, descriptor.codeSize);
    EXPECT_NE(info.sourceHash, 0u);
    EXPECT_EQ(info.version, 0u) << "a binary module has no source version";

    api->destroyShaderModule(module);
  });
}

#endif // CORAL_BACKEND_HAS_SHADERC

// ==================================================================================================
// Binding points declared by the shader
// ==================================================================================================
//
// `layout(binding = N)` is how a shader names its own binding points, and it is the shape every shader written in
// the canvas's style uses. Two things used to stop it, and both are pinned here rather than in an example program:
//
//   * the source floor. A desktop source is compiled at the pinned version (`GlslCompiler`); at 410 glslang refuses
//     the qualifier for a sampler and refuses a block that omits it either way, so nothing with a binding point could
//     be written at run time at all.
//   * the driver still has to accept the *translated* source. The translator emits the qualifiers for a module that
//     carries them, and a 4.1 core driver wants `GL_ARB_shading_language_420pack` enabled for that -- the backend
//     adds the directive where it hands the source over, which is the only place that cannot be bypassed.
//
// This test therefore asserts the observable pair: the module compiles, and its reflection reports both binding
// points. Before those two fixes the first assertion failed with the driver's own message
// (`layout(binding) requires "#version 420" or later`).

TEST(PlatformRunner, AShaderCanDeclareItsBindingPoints) {
  forEachExpectedBackend([](const ExpectedBackend& expected) {
    DeviceHandle device;
    Device* api = nullptr;
    if (!createDeviceOrSkip(device, expected, api)) {
      GTEST_SKIP() << expected.name << " is compiled in but not implemented yet";
    }

    // `Language::GLSL` is compiled here, so a build without shaderc cannot create such a module at all.
    // That is a supported configuration (the OpenGL backend still takes SPIR-V and GLSLDirect), not a
    // failure of this check: the capability the device reports is what decides.
    if (!api->getCapabilities().runtimeShaderCompilation) {
      GTEST_SKIP() << expected.name << " has no GLSL compiler in this build, so there is nothing to compile";
    }

    // `Language::GLSL` and not `GLSLDirect`: this is the path that compiles at the pinned floor and then *translates*
    // the result, which is the pair of halves the qualifier has to survive. A direct module skips both.
    const char* source = R"(#version 300 es
precision highp float;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uInput;
layout(binding = 1) uniform Block { vec4 uTint; } params;
void main() { oColor = texture(uInput, vec2(0.5)) * params.uTint; }
)";

    ShaderModuleDescriptor descriptor;
    descriptor.stage = ShaderStage::Fragment;
    descriptor.language = ShaderModuleDescriptor::Language::GLSL;
    descriptor.code = source;
    descriptor.codeSize = std::strlen(source) + 1;
    descriptor.label = "binding-points";
    ShaderModuleHandle module = ShaderModuleHandle::invalid();
    const Result created = api->createShaderModule(descriptor, &module);
    // A failure here is the driver's own message about the qualifier, which the backend echoes.
    ASSERT_RHI_SUCCESS(created);
    ASSERT_TRUE(module.valid());

    std::vector<ShaderBindingDeclaration> reflected;
    ASSERT_RHI_SUCCESS(api->getShaderReflection(module, &reflected));

    bool samplerAtZero = false;
    bool blockAtOne = false;
    for (const ShaderBindingDeclaration& binding : reflected) {
      if (binding.set != 0) continue;
      if (binding.binding == 0 && binding.type == BindingType::TextureSampler) samplerAtZero = true;
      if (binding.binding == 1 && binding.type == BindingType::UniformBuffer) blockAtOne = true;
    }
    std::printf("  binding points: %zu binding(s) reflected, sampler at 0 = %d, block at 1 = %d\n",
            reflected.size(), samplerAtZero ? 1 : 0, blockAtOne ? 1 : 0);
    EXPECT_TRUE(samplerAtZero) << "the sampler's declared binding point was not reflected";
    EXPECT_TRUE(blockAtOne) << "the uniform block's declared binding point was not reflected";

    api->destroyShaderModule(module);
    EXPECT_RHI_SUCCESS(api->waitIdle());
  });
}

#if CORAL_BACKEND_HAS_SHADERC
// The Vulkan-environment compile below comes from shaderc; a GL-only build has no such entry point.
TEST(PlatformRunner, VulkanAsyncPipelineCreation) {
  const ExpectedBackend expected{ Backend::Vulkan, "Vulkan async", GlDialect::ES, false };
  DeviceHandle device;
  Device* api = nullptr;
  if (!createDeviceOrSkip(device, expected, api)) {
    GTEST_SKIP() << "Vulkan is not available on this machine";
  }

  std::string error;
  const std::vector<uint32_t> vertexWords = compileForVulkan(R"(#version 450
layout(location = 0) in vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
)", shaderc_vertex_shader, &error);
  const std::vector<uint32_t> fragmentWords = compileForVulkan(R"(#version 450
layout(location = 0) out vec4 oColor;
void main() { oColor = vec4(1.0); }
)", shaderc_fragment_shader, &error);
  if (vertexWords.empty() || fragmentWords.empty()) {
    GTEST_SKIP() << "the Vulkan-environment compile is unavailable: " << error;
  }

  RenderPassDescriptor passDescriptor;
  passDescriptor.colorAttachmentCount = 1;
  passDescriptor.colorFormats[0] = PixelFormat::R8G8B8A8_UNORM;
  passDescriptor.sampleCount = 1;
  RenderPassHandle renderPass = RenderPassHandle::invalid();
  ASSERT_RHI_SUCCESS(api->createRenderPass(passDescriptor, &renderPass));

  ShaderModuleDescriptor vertexDescriptor;
  vertexDescriptor.stage = ShaderStage::Vertex;
  vertexDescriptor.language = ShaderModuleDescriptor::Language::SPIRV;
  vertexDescriptor.code = vertexWords.data();
  vertexDescriptor.codeSize = vertexWords.size() * sizeof(uint32_t);
  ShaderModuleHandle vertexModule = ShaderModuleHandle::invalid();
  ASSERT_RHI_SUCCESS(api->createShaderModule(vertexDescriptor, &vertexModule));

  ShaderModuleDescriptor fragmentDescriptor;
  fragmentDescriptor.stage = ShaderStage::Fragment;
  fragmentDescriptor.language = ShaderModuleDescriptor::Language::SPIRV;
  fragmentDescriptor.code = fragmentWords.data();
  fragmentDescriptor.codeSize = fragmentWords.size() * sizeof(uint32_t);
  ShaderModuleHandle fragmentModule = ShaderModuleHandle::invalid();
  ASSERT_RHI_SUCCESS(api->createShaderModule(fragmentDescriptor, &fragmentModule));

  PipelineLayoutDescriptor layoutDescriptor;
  PipelineLayoutHandle layout = PipelineLayoutHandle::invalid();
  ASSERT_RHI_SUCCESS(api->createPipelineLayout(layoutDescriptor, &layout));

  RenderPipelineDescriptor pipelineDescriptor;
  pipelineDescriptor.layout = layout;
  pipelineDescriptor.vertexModule = vertexModule;
  pipelineDescriptor.fragmentModule = fragmentModule;
  pipelineDescriptor.vertexInput.bufferCount = 1;
  pipelineDescriptor.vertexInput.buffers[0].stride = sizeof(float) * 2;
  pipelineDescriptor.vertexInput.buffers[0].stepMode = VertexStepMode::Vertex;
  pipelineDescriptor.vertexInput.buffers[0].attributeCount = 1;
  pipelineDescriptor.vertexInput.attributeCount = 1;
  pipelineDescriptor.vertexInput.attributes[0].shaderLocation = 0;
  pipelineDescriptor.vertexInput.attributes[0].bufferIndex = 0;
  pipelineDescriptor.vertexInput.attributes[0].format = VertexFormat::Float2;
  pipelineDescriptor.colorTargetCount = 1;
  pipelineDescriptor.colorTargets[0].format = PixelFormat::R8G8B8A8_UNORM;
  pipelineDescriptor.compatibleRenderPass = renderPass;
  pipelineDescriptor.label = "vulkan-async-pipeline";

  std::atomic<bool> done{ false };
  std::atomic<int> result{ -1 };
  RenderPipelineHandle pipeline = RenderPipelineHandle::invalid();
  if (!api->createRenderPipelineAsync(
              pipelineDescriptor, [&](const RenderPipelineHandle handle, const Result status, std::string message) {
                pipeline = handle;
                result.store(static_cast<int>(status), std::memory_order_release);
                if (!isSuccess(status)) error = std::move(message);
                done.store(true, std::memory_order_release);
              })) {
    GTEST_SKIP() << "this Vulkan device has no async pipeline support";
  }
  for (uint32_t attempt = 0; attempt < 5000 && !done.load(std::memory_order_acquire); ++attempt) {
    api->pollAsyncOperations();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(done.load(std::memory_order_acquire));
  ASSERT_EQ(static_cast<Result>(result.load(std::memory_order_acquire)), Result::Success) << error;
  ASSERT_TRUE(pipeline.valid());
  api->destroyRenderPipeline(pipeline);

  // A request still queued when the device goes away must be discarded by the worker shutdown.
  std::atomic<bool> abandonedCompleted{ false };
  ASSERT_TRUE(api->createRenderPipelineAsync(
          pipelineDescriptor, [&](const RenderPipelineHandle, const Result, std::string) {
            abandonedCompleted.store(true, std::memory_order_release);
          }));

  // A rejected descriptor must complete with a failure, never a valid handle.
  RenderPipelineDescriptor badDescriptor = pipelineDescriptor;
  badDescriptor.vertexModule = ShaderModuleHandle::invalid();
  std::atomic<bool> badDone{ false };
  std::atomic<int> badResult{ -1 };
  ASSERT_TRUE(api->createRenderPipelineAsync(
          badDescriptor, [&](const RenderPipelineHandle handle, const Result status, std::string) {
            EXPECT_FALSE(handle.valid());
            badResult.store(static_cast<int>(status), std::memory_order_release);
            badDone.store(true, std::memory_order_release);
          }));
  for (uint32_t attempt = 0; attempt < 5000 && !badDone.load(std::memory_order_acquire); ++attempt) {
    api->pollAsyncOperations();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(badDone.load(std::memory_order_acquire));
  EXPECT_NE(static_cast<Result>(badResult.load(std::memory_order_acquire)), Result::Success);

  // The device destructor stops the worker before these pools are destroyed.
  (void)abandonedCompleted;
}
#endif // CORAL_BACKEND_HAS_SHADERC

} // namespace anonymous
} // namespace test

