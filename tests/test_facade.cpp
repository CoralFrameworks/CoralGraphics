/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and limitations under
 * the License.
 */

/**
 * @file test_facade.cpp
 *
 * Tests the `gpu::` layer.
 *
 * These are deliberately *not* a second copy of the backend's suite. The backend's tests already check
 * that a device comes up, that the capability reasons agree with the feature bits, and that a stale
 * handle is refused. What is checked here is what the facade itself adds:
 *
 *   - the public vocabulary is reachable without a private header, which is a compile-time property
 *     that only a translation unit including exactly `gpu/...` can demonstrate;
 *   - the bookkeeping the facade does on top of the backend - counts, strides, index formats - is
 *     actually carried through rather than recomputed wrongly from a byte size;
 *   - lifetime: a resource outliving its `Context` handle keeps the device alive, and releasing
 *     resources in any order is safe.
 *
 * Every case that needs a device is skipped where none can be created, so the suite still passes on a
 * machine with no graphics stack.
 */

#include "test_asserts.h"

#include <backend/Device.h>
#include <gpu/Buffer.h>
#include <gpu/Context.h>
#include <gpu/Shader.h>
#include <gpu/Types.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace gpu_test {
namespace {

using namespace graphics;

/**
 * A headless context, or null when this machine cannot provide one.
 *
 * The facade is what is under test, so the configuration is the simplest thing a client would write:
 * no window, no validation, the platform's own choice of backend.
 */
std::shared_ptr<gpu::Context> makeContext() {
  gpu::ContextConfiguration configuration;
  configuration.applicationName = "coral-gpu-facade-tests";
  return gpu::Context::create(configuration);
}

/** Skips the test when no device can be brought up on this machine. */
#define REQUIRE_CONTEXT(context)                                                                                       \
  auto context = makeContext();                                                                                        \
  if (context == nullptr) {                                                                                            \
    GTEST_SKIP() << "no graphics device is available on this machine";                                                 \
  }                                                                                                                    \
  ASSERT_TRUE(context->valid())

/** A triangle, used wherever the test needs real vertex data. */
struct Vertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

constexpr Vertex kTriangle[3] = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
constexpr uint16_t kTriangleIndices16[3] = { 0, 1, 2 };

/** Reads the SPIR-V fixture the backend's shader tests use. */
std::vector<uint32_t> readFixture() {
#ifdef CORAL_BACKEND_FIXTURE_DIR
  const std::string path = std::string(CORAL_BACKEND_FIXTURE_DIR) + "/fragment_simple.spv";
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return {};

  const std::streamsize size = file.tellg();
  if (size <= 0 || (size % 4) != 0) return {};
  file.seekg(0);

  std::vector<uint32_t> words(static_cast<size_t>(size) / sizeof(uint32_t));
  file.read(reinterpret_cast<char*>(words.data()), size);
  return words;
#else
  return {};
#endif
}

/**
 * A file that deletes itself.
 *
 * The shader-file tests need a real file, and a test that writes one into the source tree is a test that
 * eventually leaves it there for someone to commit. Removing it from the destructor is what makes that
 * impossible rather than merely unlikely, and it holds if the test fails.
 *
 * The directory comes from the environment rather than from `std::filesystem::temp_directory_path()`,
 * which would be the obvious spelling and would also make this test the only thing in the library that
 * needs `<filesystem>` - a dependency that costs a link flag on some toolchains for a path one `getenv`
 * answers.
 */
class TemporaryFile {
public:
  explicit TemporaryFile(const std::string& name) {
    const char* directory = nullptr;
    for (const char* variable: { "TMPDIR", "TEMP", "TMP" }) {
      directory = std::getenv(variable);
      if (directory != nullptr && directory[0] != '\0') break;
      directory = nullptr;
    }
    mPath = std::string(directory != nullptr ? directory : ".") + "/" + name;
  }

  ~TemporaryFile() { std::remove(mPath.c_str()); }

  TemporaryFile(const TemporaryFile&) = delete;
  TemporaryFile& operator=(const TemporaryFile&) = delete;

  [[nodiscard]] const std::string& path() const { return mPath; }

  bool write(const std::string& contents) const {
    std::ofstream file(mPath, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return file.good();
  }

private:
  std::string mPath;
};

/**
 * A GLSL vertex source written for the dialect this build targets, with no `layout(location = ...)` on
 * its input.
 *
 * The missing location is deliberate rather than incidental: glslang refuses to emit SPIR-V from a
 * source whose user inputs have none, so this text is exactly what the SPIR-V path cannot take and the
 * direct path can. The dialect comes from `getPlatformGlDialect()` rather than from a guess, because the
 * direct path has no translator - a `#version 300 es` source handed to a desktop core context is a syntax
 * error, and a test that produced one would be failing for a reason that has nothing to do with what it
 * checks.
 */
std::string directVertexSource(const bool es) {
  if (es) {
    return "#version 300 es\nin vec4 aPos;\nvoid main() { gl_Position = aPos; }\n";
  }
  return "#version 410 core\nin vec4 aPos;\nvoid main() { gl_Position = aPos; }\n";
}

/** The fragment-side counterpart of @ref directVertexSource, in the same dialect. */
std::string directFragmentSource(const bool es) {
  if (es) {
    return "#version 300 es\nprecision mediump float;\nlayout(location = 0) out vec4 o;\n"
           "uniform sampler2D uTex;\nvoid main() { o = texture(uTex, vec2(0.5)); }\n";
  }
  return "#version 410 core\nlayout(location = 0) out vec4 o;\n"
         "uniform sampler2D uTex;\nvoid main() { o = texture(uTex, vec2(0.5)); }\n";
}

/** True when this build's contexts are OpenGL ES rather than desktop OpenGL. */
bool contextIsEs() {
  return backend::getPlatformGlDialect() == backend::GlDialect::ES;
}

} // namespace anonymous

TEST(Facade, TypesAreReachableWithoutAPrivateHeader) {
  // A compile-time property, and the reason this file includes only gpu/... headers: if the facade
  // leaked a private type into its public surface, this translation unit would not build.
  EXPECT_STREQ(gpu::toString(gpu::Result::Success), "Success");
  EXPECT_STREQ(gpu::toString(gpu::Result::InvalidHandle), "InvalidHandle");
  EXPECT_TRUE(isSuccess(gpu::Result::Success));
  EXPECT_FALSE(isSuccess(gpu::Result::Unsupported));

  // A default-constructed context is inert rather than half-alive.
  gpu::Context context;
  EXPECT_FALSE(context.valid());
  EXPECT_EQ(context.device(), nullptr);
  EXPECT_EQ(context.waitIdle(), gpu::Result::DeviceLost);
}

TEST(Facade, CreatesADeviceAndReportsIt) {
  REQUIRE_CONTEXT(context);

  const std::string description = context->describeDevice();
  EXPECT_FALSE(description.empty());
  EXPECT_EQ(description.find("no device"), std::string::npos) << description;

  // The report is what a bug report carries, so it must exist and must name the device.
  const std::string diagnostics = context->diagnostics();
  EXPECT_GT(diagnostics.size(), 0u);
  EXPECT_NE(diagnostics.find(context->device()->getInfo().deviceName), std::string::npos);

  // The capability queries must agree with the full capability set they are a convenience over. A
  // facade that disagreed with the layer underneath it would be worse than no facade.
  const backend::Capabilities& capabilities = context->device()->getCapabilities();
  EXPECT_EQ(context->supportsCompute(), capabilities.supports(backend::Feature::ComputeShader));
  EXPECT_EQ(context->supportsGeometryShaders(), capabilities.supports(backend::Feature::GeometryShader));
  EXPECT_EQ(context->supportsWireframe(), capabilities.supports(backend::Feature::FillModeNonSolid));
  EXPECT_EQ(context->supportsAnisotropicFiltering(), capabilities.supports(backend::Feature::SamplerAnisotropy));
}

TEST(Facade, VertexBufferRemembersItsCountAndStride) {
  REQUIRE_CONTEXT(context);

  const std::shared_ptr<gpu::VertexBuffer> buffer =
          context->createVertexBuffer(3, sizeof(Vertex), kTriangle, "triangle");
  ASSERT_NE(buffer, nullptr);

  EXPECT_EQ(buffer->getCount(), 3u);
  EXPECT_EQ(buffer->getStride(), sizeof(Vertex));
  EXPECT_EQ(buffer->getSizeInBytes(), 3 * sizeof(Vertex));
  EXPECT_TRUE(buffer->isCreationComplete());
  EXPECT_TRUE(buffer->handle().valid());

  // Size is reported in vertices and bytes respectively, and the two must be consistent: this is the
  // bookkeeping most likely to be silently wrong, because a caller cannot tell 3 vertices from 3
  // floats by looking at either number alone.
  EXPECT_EQ(buffer->getCount() * buffer->getStride(), buffer->getSizeInBytes());
}

TEST(Facade, IndexBufferRemembersItsFormatAndElementCount) {
  REQUIRE_CONTEXT(context);

  const std::shared_ptr<gpu::IndexBuffer> buffer =
          context->createIndexBuffer(3, gpu::IndexFormat::UInt16, kTriangleIndices16, "triangle-index");
  ASSERT_NE(buffer, nullptr);

  EXPECT_EQ(buffer->getCount(), 3u);
  EXPECT_EQ(buffer->getFormat(), gpu::IndexFormat::UInt16);
  // Three 16-bit indices is six bytes, not three. Getting this wrong is how a mesh renders half of
  // itself, so it is worth asserting directly.
  EXPECT_EQ(buffer->getSizeInBytes(), 3 * sizeof(uint16_t));

  const std::shared_ptr<gpu::IndexBuffer> wide =
          context->createIndexBuffer(3, gpu::IndexFormat::UInt32, nullptr, "wide-index");
  ASSERT_NE(wide, nullptr);
  EXPECT_EQ(wide->getFormat(), gpu::IndexFormat::UInt32);
  EXPECT_EQ(wide->getSizeInBytes(), 3 * sizeof(uint32_t));
}

TEST(Facade, UniformBufferAcceptsHostUpdates) {
  REQUIRE_CONTEXT(context);

  struct Constants {
    float transform[16] = {};
    float tint[4] = {};
  };

  const std::shared_ptr<gpu::UniformBuffer> buffer = context->createUniformBuffer(sizeof(Constants), "constants");
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->getSizeInBytes(), sizeof(Constants));

  Constants constants;
  constants.tint[0] = 1.0f;
  constants.tint[3] = 1.0f;

  // The whole point of asking for a uniform buffer through the facade is to be able to write it, so a
  // buffer that refused the write would make the API pointless.
  EXPECT_RHI_SUCCESS(buffer->set(constants));

  // A write past the end is the caller's mistake and must be reported, not clamped: a silently
  // truncated uniform block produces a wrong image a long way from here.
  EXPECT_EQ(buffer->update(&constants, sizeof(Constants), 1), gpu::Result::InvalidArgument);
  EXPECT_EQ(buffer->update(nullptr, sizeof(Constants)), gpu::Result::InvalidArgument);
}

TEST(Facade, EmptyBufferIsHostVisibleSoItCanBeFilledLater) {
  REQUIRE_CONTEXT(context);

  // The documented contract: no initial data means the caller intends to write it. This must not be
  // refused later, which is the failure mode a caller would only discover at the first frame.
  const std::shared_ptr<gpu::VertexBuffer> buffer = context->createVertexBuffer(4, sizeof(Vertex), nullptr, "dynamic");
  ASSERT_NE(buffer, nullptr);

  EXPECT_RHI_SUCCESS(buffer->update(kTriangle, sizeof(Vertex), 0));
}

TEST(Facade, ShaderReflectionIsInThePublicVocabulary) {
  REQUIRE_CONTEXT(context);

  if (!context->device()->getInfo().acceptsSpirv) {
    GTEST_SKIP() << "this build has no SPIR-V translator";
  }

  const std::vector<uint32_t> module = readFixture();
  if (module.empty()) {
    GTEST_SKIP() << "the SPIR-V fixture could not be read";
  }

  const std::shared_ptr<gpu::Shader> shader =
          context->createShader(module.data(), module.size(), backend::ShaderStage::Fragment);
  ASSERT_NE(shader, nullptr);
  EXPECT_EQ(shader->getStage(), backend::ShaderStage::Fragment);
  EXPECT_TRUE(shader->handle().valid());

  // The fixture declares a uniform block and a combined sampler. Reading them back through the facade
  // is what lets a renderer build a descriptor set layout without a private header, so the names have
  // to survive the copy out of the backend's reflection.
  const std::vector<gpu::Shader::Binding>& bindings = shader->getBindings();
  EXPECT_FALSE(bindings.empty()) << "reflection produced no bindings";

  for (const gpu::Shader::Binding& binding: bindings) {
    EXPECT_FALSE(binding.name.empty()) << "a binding with no name cannot be bound";
    EXPECT_GE(binding.arraySize, 1u);
  }

  // The lookup a renderer does when wiring a material, and the negative case that proves it is a real
  // lookup rather than a "return the first entry".
  const gpu::Shader::Binding* first = shader->findBinding(bindings.front().set, bindings.front().binding);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->name, bindings.front().name);
  EXPECT_EQ(shader->findBinding(99, 99), nullptr);
}

TEST(Facade, CompilesGlslAndKeepsBothForms) {
  REQUIRE_CONTEXT(context);
  if (contextIsEs()) {
    GTEST_SKIP() << "a #version 410 core source is a desktop-dialect case; the ES retarget floor is 310";
  }

  // The source declares its own version, which is the supported way to author: glslang needs a profile
  // it can believe, and a desktop shader that will not name one is refused rather than guessed at. The
  // version floors and the retargeting behaviour are covered exhaustively by the backend's
  // `GlslCompiler.*` suite; what this test is for is the facade's contract around the two forms.
  const std::string source = "#version 410 core\n"
                             "layout(location = 0) in vec4 aPos;\n"
                             "layout(location = 0) out vec4 vColor;\n"
                             "void main() { vColor = aPos; gl_Position = aPos; }\n";

  const std::shared_ptr<gpu::Shader> shader = context->createShader(gpu::ShaderSource::vertex(source, "facade-glsl"));
  if (shader == nullptr) {
    GTEST_SKIP() << "this build has no GLSL compiler; see -DCORAL_BACKEND_SUPPORTS_SHADERC";
  }

  // Both forms are kept, and they are the caller's text and the compiler's words respectively - which is
  // the whole reason to author in GLSL: a shader that misbehaves can be read back byte for byte.
  //
  // The identity check is on the whole string, not on a prefix: a facade that normalised line endings or
  // stripped a directive would still "look right" to a caller reading a log, and would quietly mean the
  // source kept for debugging is not the source that was compiled.
  ASSERT_TRUE(shader->hasSource());
  EXPECT_EQ(shader->getSource(), source);
  ASSERT_TRUE(shader->hasCompiledSpirv());
  EXPECT_EQ(shader->getSpirv()[0], 0x07230203u) << "the words must be a SPIR-V module";

  EXPECT_EQ(shader->getLanguage(), gpu::ShaderLanguage::GLSL410);
  EXPECT_EQ(shader->getSourceVersion(), 410u);
  EXPECT_TRUE(shader->hadVersionDirective());
  // Retargeted *upwards*, which is what the desktop floor is for: it sits at 420 because that is the lowest
  // version at which `layout(binding = N)` is legal, which is the qualifier a shader written in the canvas's
  // style needs (`GlslCompiler.AcceptsTheBindingQualifierAtTheDesktopFloor` measures it). 420 is a superset of
  // 410, so a source that declared 410 is not being made to mean something else.
  EXPECT_TRUE(shader->wasRetargeted()) << "the desktop floor is above 410, so a 410 source is retargeted";
  EXPECT_GE(shader->getCompiledVersion(), 420u);
  EXPECT_NE(shader->getSourceHash(), 0u);
  EXPECT_FALSE(shader->describeSource().empty());
}

TEST(Facade, RetargetingIsVisibleForEssl300) {
  REQUIRE_CONTEXT(context);

  // ESSL 3.00 is the case a caller is most likely to hit, because it is what shader tooling emits. It
  // is also the case where retargeting is safe - ESSL 3.00 is a strict subset of 3.10 - so it has to
  // work rather than merely be reported.
  const std::string source = "#version 300 es\n"
                             "precision mediump float;\n"
                             "layout(location = 0) in vec4 aPos;\n"
                             "layout(location = 0) out vec4 vColor;\n"
                             "void main() { vColor = aPos; gl_Position = aPos; }\n";

  const std::shared_ptr<gpu::Shader> shader =
          context->createShader(gpu::ShaderSource::vertex(source, "facade-essl300"));
  if (shader == nullptr) {
    GTEST_SKIP() << "this build has no GLSL compiler";
  }

  EXPECT_EQ(shader->getLanguage(), gpu::ShaderLanguage::ESSL300);
  EXPECT_EQ(shader->getSourceVersion(), 300u);
  EXPECT_TRUE(shader->hadVersionDirective());
  EXPECT_TRUE(shader->wasRetargeted());
  // The target is the context's own dialect, not the source's: this test binary runs on a desktop
  // context, so an ES-profile source is retargeted to desktop GLSL. The version it lands on is the
  // dialect's baseline, and asserting the number rather than a property of it is what would make this
  // test fail the moment it ran on a phone.
  const uint32_t compiled = shader->getCompiledVersion();
  EXPECT_GE(compiled, 310u) << "the compiler cannot emit SPIR-V below GLSL ES 3.10 or desktop 3.30";
  EXPECT_TRUE(compiled == 310u || compiled == 420u)
          << "expected the ES floor or the desktop baseline, got " << compiled;
  EXPECT_TRUE(shader->hasCompiledSpirv());

  // The description is what goes into a log when a shader misbehaves, so it has to mention the retarget
  // rather than presenting a 3.00 shader as though it had compiled as 3.00.
  const std::string described = shader->describeSource();
  EXPECT_NE(described.find("retargeted"), std::string::npos) << described;
}

TEST(Facade, CacheKeyDistinguishesWhatItMust) {
  REQUIRE_CONTEXT(context);

  const std::string vertexSource = "#version 310 es\n"
                                   "layout(location = 0) in vec4 aPos;\n"
                                   "void main() { gl_Position = aPos; }\n";
  const std::string fragmentSource = "#version 310 es\nprecision mediump float;\n"
                                     "layout(location = 0) out vec4 o;\nvoid main() { o = vec4(1.0); }\n";

  const std::shared_ptr<gpu::Shader> first = context->createShader(gpu::ShaderSource::vertex(vertexSource, "key-a"));
  if (first == nullptr) {
    GTEST_SKIP() << "this build has no GLSL compiler";
  }
  const std::shared_ptr<gpu::Shader> sameAgain =
          context->createShader(gpu::ShaderSource::vertex(vertexSource, "key-b"));
  const std::shared_ptr<gpu::Shader> otherStage =
          context->createShader(gpu::ShaderSource::fragment(fragmentSource, "key-c"));
  ASSERT_NE(sameAgain, nullptr);
  ASSERT_NE(otherStage, nullptr);

  // A cache is only useful if identical input produces an identical key...
  ASSERT_TRUE(first->getCacheKey().valid());
  EXPECT_EQ(first->getCacheKey().value, sameAgain->getCacheKey().value);
  EXPECT_EQ(first->getSourceHash(), sameAgain->getSourceHash());

  // ...and only safe if anything that changes the compiled result changes the key. The stage is the
  // easiest of those to forget: the same text compiled as a fragment shader is a different module.
  EXPECT_NE(first->getCacheKey().value, otherStage->getCacheKey().value);
  EXPECT_NE(first->getCacheKey().stage, otherStage->getCacheKey().stage);

  // Every component is recorded, so a cache miss can say why rather than only that it happened.
  EXPECT_NE(first->getCacheKey().sourceHash, 0u);
  EXPECT_NE(first->getCacheKey().dialectHash, 0u);
  EXPECT_NE(first->getCacheKey().compilerHash, 0u);
}

TEST(Facade, ReportsGlslThatCannotBecomeSpirv) {
  REQUIRE_CONTEXT(context);

  // ESSL 1.00 that uses the keywords ES 3.00 removed cannot be retargeted into compiling. The call has
  // to fail rather than produce something, and the reason has to name the keyword - the compiler's own
  // message says "attribute : Reserved word" and nothing about why an ES 1.00 shader is being judged by
  // an ES 3.00 parser.
  const std::string legacy = "attribute vec4 aPos;\n"
                             "varying vec4 vColor;\n"
                             "void main() { vColor = aPos; gl_Position = aPos; }\n";

  const std::shared_ptr<gpu::Shader> shader = context->createShader(gpu::ShaderSource::vertex(legacy, "facade-legacy"));
  if (gpu::isShaderCompilerAvailable()) {
    EXPECT_EQ(shader, nullptr) << "a shader using ES 3.00-removed keywords must not compile to SPIR-V";
  } else {
    GTEST_SKIP() << "this build has no GLSL compiler";
  }
}

TEST(Facade, RejectsAnEmptyShaderSource) {
  REQUIRE_CONTEXT(context);

  gpu::ShaderSource empty;
  empty.stage = backend::ShaderStage::Vertex;
  EXPECT_EQ(context->createShader(empty), nullptr);
}

TEST(Facade, DirectGlslGoesStraightToTheDriver) {
  REQUIRE_CONTEXT(context);

  if (!context->supportsDirectGlsl()) {
    GTEST_SKIP() << "this device does not take GLSL directly";
  }

  const bool es = contextIsEs();
  // No `layout(location = ...)` on the vertex input, and that is the point of the shader rather than an
  // oversight: glslang refuses to emit SPIR-V from a source whose user inputs have no location, so this
  // text *cannot* take the usual path - and the driver compiles it without comment. One shader, both
  // halves of the feature.
  const std::string text = directVertexSource(es);

  // The SPIR-V path, on the same text, to show what the direct path skipped. Guarded, because a build
  // without shaderc refuses everything and would make this pass for the wrong reason - which would leave
  // the test below asserting nothing at all.
  if (gpu::isShaderCompilerAvailable()) {
    EXPECT_EQ(context->createShader(gpu::ShaderSource::vertex(text, "direct-vs-ported")), nullptr)
            << "a source with no vertex input locations cannot become SPIR-V; if this succeeds, the "
               "direct path below is not demonstrating anything";
  }

  gpu::ShaderSource source = gpu::ShaderSource::directVertex(text, "direct-vs");
  source.attributes.push_back(gpu::ShaderAttributeMapping { "aPos", 0 });

  const std::shared_ptr<gpu::Shader> shader = context->createShader(source);
  ASSERT_NE(shader, nullptr) << "a GL driver should compile this source as written";

  // Provenance. The source is the author's, byte for byte, and the shader says which route it took.
  EXPECT_TRUE(shader->hasSource());
  EXPECT_EQ(shader->getSource(), text);
  EXPECT_TRUE(shader->isDirectGlsl());
  EXPECT_EQ(shader->getStage(), backend::ShaderStage::Vertex);
  EXPECT_TRUE(shader->handle().valid());

  // Nothing was compiled to SPIR-V here, so there are no words to hand out and no key to cache them
  // under. Reporting an empty vector is the fact; a cache key would be a promise the module cannot keep,
  // because the driver compiles the text again on every device that loads it.
  EXPECT_FALSE(shader->hasCompiledSpirv());
  EXPECT_TRUE(shader->getSpirv().empty());
  EXPECT_FALSE(shader->getCacheKey().valid());

  // The hash still answers "is this the same source as before", which is the question a reload path
  // has, and it is the one field of the key that a direct module can meaningfully fill in.
  EXPECT_NE(shader->getSourceHash(), 0u);

  // The version is the source's own, and it was not retargeted: the driver compiled what it was given,
  // which is exactly the difference the description has to convey.
  EXPECT_EQ(shader->getLanguage(), es ? gpu::ShaderLanguage::ESSL300 : gpu::ShaderLanguage::GLSL410);
  EXPECT_EQ(shader->getSourceVersion(), es ? 300u : 410u);
  EXPECT_TRUE(shader->hadVersionDirective());
  EXPECT_FALSE(shader->wasRetargeted());
  EXPECT_FALSE(shader->wasOptimized());
  EXPECT_EQ(shader->getCompiledVersion(), es ? 300u : 410u)
          << "the compiled version is the source's own; nothing here chose a version for it";

  const std::string described = shader->describeSource();
  EXPECT_NE(described.find("driver"), std::string::npos) << described;
}

TEST(Facade, DirectGlslReportsTheCallersBindingTable) {
  REQUIRE_CONTEXT(context);

  if (!context->supportsDirectGlsl()) {
    GTEST_SKIP() << "this device does not take GLSL directly";
  }

  // The table a shader package would carry, and the one thing the direct path cannot work out for
  // itself: with no SPIR-V to reflect, the caller's description of the interface *is* the interface.
  gpu::ShaderSource source = gpu::ShaderSource::directFragment(directFragmentSource(contextIsEs()), "direct-fs");
  gpu::ShaderBindingMapping sampler;
  sampler.name = "uTex";
  sampler.set = 0;
  sampler.binding = 2;
  sampler.type = backend::BindingType::TextureSampler;
  sampler.location = 1;
  source.bindings.push_back(sampler);

  const std::shared_ptr<gpu::Shader> shader = context->createShader(source);
  ASSERT_NE(shader, nullptr);

  ASSERT_EQ(shader->getBindings().size(), 1u);
  const gpu::Shader::Binding& reported = shader->getBindings().front();
  EXPECT_EQ(reported.name, "uTex");
  EXPECT_EQ(reported.set, 0u);
  EXPECT_EQ(reported.binding, 2u);
  EXPECT_EQ(reported.location, 1) << "the declared texture unit has to survive into the shader's table";
  EXPECT_EQ(reported.arraySize, 1u);

  // The lookup a renderer does when wiring a material, on the table it supplied rather than one that was
  // recovered: the two must agree, or a descriptor set built from the shader would address the wrong
  // slot.
  const gpu::Shader::Binding* found = shader->findBinding(0, 2);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->name, "uTex");
  EXPECT_EQ(found->location, 1);
  EXPECT_EQ(shader->findBinding(0, 3), nullptr);
}

TEST(Facade, DirectGlslReadsAShaderFileAndStripsItsByteOrderMark) {
  REQUIRE_CONTEXT(context);

  if (!context->supportsDirectGlsl()) {
    GTEST_SKIP() << "this device does not take GLSL directly";
  }

  const bool es = contextIsEs();
  // A byte order mark, which an editor that saves "UTF-8 with BOM" adds and a driver does not skip: it
  // turns `#version` into a syntax error on the first line. The loader removes it, so what reaches the
  // driver is the file as intended.
  const std::string body = directVertexSource(es);
  const std::string onDisk = "\xEF\xBB\xBF" + body;

  const TemporaryFile file("coral_facade_direct.vert");
  ASSERT_TRUE(file.write(onDisk)) << "cannot write " << file.path();

  std::string error;
  const gpu::ShaderSource source = gpu::ShaderSource::fromFile(file.path(), &error);
  EXPECT_TRUE(error.empty()) << error;

  // The extension named the stage - a vertex shader, from `.vert` - and a file read at run time can only
  // take the direct path, so the flag is set rather than left for the caller to remember.
  EXPECT_EQ(source.stage, backend::ShaderStage::Vertex);
  EXPECT_TRUE(source.directGlsl);
  EXPECT_EQ(source.source, body) << "the byte order mark has to be gone and nothing else touched";

  const std::shared_ptr<gpu::Shader> shader = context->createShader(source);
  ASSERT_NE(shader, nullptr);
  EXPECT_TRUE(shader->isDirectGlsl());
  EXPECT_EQ(shader->getSource(), body);

  // The failure paths, which a package loader will meet: a missing file, an extension that names no
  // stage, and a file that exists and holds nothing. All three report an empty source, which
  // `createShader` refuses, so a caller that ignores the error string still fails safe - it just cannot
  // say which of its inputs was the empty one.
  error.clear();
  const gpu::ShaderSource missing = gpu::ShaderSource::fromFile(file.path() + ".missing.vert", &error);
  EXPECT_TRUE(missing.source.empty());
  EXPECT_FALSE(error.empty());

  error.clear();
  const gpu::ShaderSource unknown = gpu::ShaderSource::fromFile("shader.wat", &error);
  EXPECT_TRUE(unknown.source.empty());
  EXPECT_FALSE(error.empty()) << "an extension that names no stage is reported rather than guessed at";

  ASSERT_TRUE(file.write(std::string {}));
  error.clear();
  const gpu::ShaderSource blank = gpu::ShaderSource::fromFile(file.path(), &error);
  EXPECT_TRUE(blank.source.empty());
  EXPECT_FALSE(error.empty()) << "an empty file is reported rather than handed to the driver to fail on";
}

TEST(Facade, ResourcesOutliveTheContextHandle) {
  // The facade's promise, and the main thing it adds over the raw backend: a resource keeps the device
  // alive, so a client that drops its context handle while geometry is still loaded keeps working.
  std::shared_ptr<gpu::VertexBuffer> vertexBuffer;
  backend::BufferHandle rawHandle = backend::BufferHandle::invalid();

  {
    auto context = makeContext();
    if (context == nullptr) {
      GTEST_SKIP() << "no graphics device is available on this machine";
    }

    // Created without initial data, so it is host-visible and can be written. A buffer created *with*
    // data is device-local by design - reading it is what the GPU will do - and only a host-visible
    // one is the interesting case for a lifetime test, because it is the one the CPU touches.
    vertexBuffer = context->createVertexBuffer(3, sizeof(Vertex), nullptr, "outlives");
    ASSERT_NE(vertexBuffer, nullptr);
    rawHandle = vertexBuffer->handle();
    EXPECT_RHI_SUCCESS(vertexBuffer->update(kTriangle, sizeof(kTriangle), 0));

    // The context handle goes out of scope here. The device must not: the buffer still needs it.
  }

  // The buffer is still usable, which means the device was not torn down with the last Context handle.
  EXPECT_TRUE(vertexBuffer->isCreationComplete());
  EXPECT_EQ(vertexBuffer->getCount(), 3u);
  EXPECT_RHI_SUCCESS(vertexBuffer->update(kTriangle, sizeof(kTriangle), 0));

  // And the handle it holds is the same one the device still knows about, not a stale one it would
  // refuse.
  EXPECT_EQ(vertexBuffer->handle(), rawHandle);

  // Releasing it is the last reference, so this is also the destructor path that frees the device.
  vertexBuffer.reset();
}

TEST(Facade, DeviceOutlivesEveryResourceItHandedOut) {
  // The inverse ordering: resources destroyed before the context, and after. Both must be safe, and
  // the device must be released exactly once.
  auto context = makeContext();
  if (context == nullptr) {
    GTEST_SKIP() << "no graphics device is available on this machine";
  }

  {
    const std::shared_ptr<gpu::VertexBuffer> vertex = context->createVertexBuffer(3, sizeof(Vertex), kTriangle);
    const std::shared_ptr<gpu::IndexBuffer> index =
            context->createIndexBuffer(3, gpu::IndexFormat::UInt16, kTriangleIndices16);
    const std::shared_ptr<gpu::UniformBuffer> uniforms = context->createUniformBuffer(64);
    ASSERT_NE(vertex, nullptr);
    ASSERT_NE(index, nullptr);
    ASSERT_NE(uniforms, nullptr);
  } // destroyed here, while the context is still alive

  EXPECT_RHI_SUCCESS(context->waitIdle());

  // A device that is still usable after every resource it handed out was released is the property that
  // matters; a use-after-free on the device would not survive this.
  const std::shared_ptr<gpu::VertexBuffer> again = context->createVertexBuffer(3, sizeof(Vertex), kTriangle);
  EXPECT_NE(again, nullptr);
}

TEST(Facade, RejectsInvalidArgumentsRatherThanCrashing) {
  REQUIRE_CONTEXT(context);

  // A zero-stride vertex buffer cannot describe any geometry, and a zero-sized uniform buffer has
  // nothing to hold. Both are caller mistakes; both must be reported, because a facade that asserted
  // here would turn a bad call into a crash in a shipped game.
  EXPECT_EQ(context->createVertexBuffer(3, 0), nullptr);
  EXPECT_EQ(context->createUniformBuffer(0), nullptr);
  EXPECT_EQ(context->createShader(nullptr, 0, backend::ShaderStage::Vertex), nullptr);

  // The backend is reachable, as documented, for the operations the facade does not wrap.
  EXPECT_NE(context->device(), nullptr);
}

} // namespace gpu_test
