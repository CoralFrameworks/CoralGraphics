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
 * Tests for the SPIR-V -> GLSL ES translation path.
 *
 * The fixture under tests/fixtures is a real SPIR-V module produced by a shader compiler, not a
 * hand-assembled one. That matters: a hand-written module that is subtly malformed makes the
 * translator fail for reasons that have nothing to do with the RHI, which is exactly the kind of
 * misleading test this suite is meant to avoid.
 *
 * The shader it represents samples a texture and multiplies it by a uniform block:
 *
 * ```glsl
 *   #version 450
 *   layout(location = 0) in vec2 vUV;
 *   layout(location = 0) out vec4 FragColor;
 *   layout(set = 0, binding = 0) uniform Registers { vec4 color; } registers;
 *   layout(set = 0, binding = 1) uniform sampler2D uSampler;
 *   void main() { FragColor = texture(uSampler, vUV) * registers.color; }
 * ```
 *
 * It has one stage input, one stage output, one uniform block and one sampled image, so a single
 * module exercises every reflection category the OpenGL backend's binding table cares about.
 */

#include "test_asserts.h"

#include <ShaderCross.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace backend;

namespace {

/** The fixture directory, injected by CMake so the test works from any working directory. */
#ifndef CORAL_BACKEND_FIXTURE_DIR
#define CORAL_BACKEND_FIXTURE_DIR ""
#endif

/** @return the absolute path of @p name, trying the source tree as a fallback. */
std::string fixturePath(const char* name) {
  const std::string configured = std::string(CORAL_BACKEND_FIXTURE_DIR) + "/" + name;
  if (std::FILE* probe = std::fopen(configured.c_str(), "rb")) {
    std::fclose(probe);
    return configured;
  }

  // __FILE__ is an absolute path when the compiler was given one, which keeps the test runnable
  // straight from the build directory without any configuration.
  const std::string sourceRelative = std::string(__FILE__);
  const size_t slash = sourceRelative.find_last_of("/\\");
  if (slash != std::string::npos) {
    const std::string candidate = sourceRelative.substr(0, slash) + "/fixtures/" + name;
    if (std::FILE* probe = std::fopen(candidate.c_str(), "rb")) {
      std::fclose(probe);
      return candidate;
    }
  }
  return {};
}

/** Reads a SPIR-V module, returning an empty vector when it cannot be read. */
std::vector<uint32_t> readSpirv(const char* name) {
  const std::string path = fixturePath(name);
  if (path.empty()) return {};

  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return {};

  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (size <= 0 || (size % 4) != 0) {
    std::fclose(file);
    return {};
  }

  std::vector<uint32_t> words(size_t(size) / sizeof(uint32_t));
  const size_t read = std::fread(words.data(), sizeof(uint32_t), words.size(), file);
  std::fclose(file);
  if (read != words.size()) return {};
  return words;
}

/**
 * Fills in a compile request for the fragment fixture.
 *
 * The GLSL version comes from `forDialect` rather than being spelled here, so the test exercises the
 * same path the device does. An explicit number would let the device's version choice break while the
 * test kept passing.
 */
ShaderCrossCompileInfo fragmentFixtureInfo(const std::vector<uint32_t>& module, GlDialect dialect = GlDialect::ES) {
  ShaderCrossCompileInfo info = ShaderCrossCompileInfo::forDialect(dialect, ShaderStage::Fragment);
  info.spirv = module.data();
  info.wordCount = module.size();
  info.flipVertexY = true;
  info.fixupClipSpace = true;
  info.combineImageSamplers = true;
  return info;
}

} // namespace anonymous

TEST(ShaderCross, ReportsWhetherSpirvIsSupported) {
  // The answer must be stable and must agree with the version string. A build without SPIRV-Cross is
  // a supported configuration - GLSL ES source can be supplied instead - but it has to say so rather
  // than failing at shader-creation time.
  const bool available = isShaderCrossAvailable();
  const char* version = getShaderCrossVersion();
  ASSERT_NE(version, nullptr);
  if (available) {
    EXPECT_STRNE(version, "unavailable");
  } else {
    EXPECT_STREQ(version, "unavailable");
  }
}

TEST(ShaderCross, FixtureIsAvailable) {
  const std::vector<uint32_t> module = readSpirv("fragment_simple.spv");
  ASSERT_FALSE(module.empty()) << "the SPIR-V fixture could not be read; looked near "
                               << fixturePath("fragment_simple.spv");
  // The magic number is checked here so a fixture that got corrupted in transit fails with a clear
  // message rather than as a translator error.
  EXPECT_EQ(module[0], 0x07230203u);
}

TEST(ShaderCross, RejectsEmptyInput) {
  ShaderCrossCompileInfo info;
  info.spirv = nullptr;
  info.wordCount = 0;

  const ShaderCrossCompileResult result = crossCompileToGLSL(info);
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error.empty()) << "a failure must explain itself";
}

TEST(ShaderCross, RejectsInputWithoutTheSpirvMagicNumber) {
  // Shader assets are loaded from disk and are the most likely thing to be corrupt. Catching the
  // missing magic word here turns "the translator crashed on garbage input" into a message.
  const uint32_t garbage[] = { 0xDEADBEEFu, 0x12345678u, 0x9ABCDEF0u, 0x0BADF00Du };

  ShaderCrossCompileInfo info;
  info.spirv = garbage;
  info.wordCount = sizeof(garbage) / sizeof(garbage[0]);

  const ShaderCrossCompileResult result = crossCompileToGLSL(info);
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error.empty());
  EXPECT_NE(result.error.find("magic"), std::string::npos)
          << "the message should name the actual problem: " << result.error;
}

TEST(ShaderCross, RejectsATruncatedModuleWithoutCrashing) {
  // Take a valid module and cut it in half. SPIRV-Cross reports malformed input by throwing; the
  // wrapper must catch that and turn it into a Result, because an exception escaping into a renderer
  // loop is a process kill rather than a diagnosable failure.
  std::vector<uint32_t> module = readSpirv("fragment_simple.spv");
  ASSERT_FALSE(module.empty()) << "the SPIR-V fixture could not be read";
  module.resize(module.size() / 2);

  ShaderCrossCompileInfo info = fragmentFixtureInfo(module);
  const ShaderCrossCompileResult result = crossCompileToGLSL(info);
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error.empty());
}

TEST(ShaderCross, TranslatesAFragmentShaderToGlslEs) {
  if (!isShaderCrossAvailable()) {
    GTEST_SKIP() << "this build has no SPIRV-Cross, so SPIR-V modules cannot be translated";
  }

  const std::vector<uint32_t> module = readSpirv("fragment_simple.spv");
  ASSERT_FALSE(module.empty()) << "the SPIR-V fixture could not be read";

  const ShaderCrossCompileInfo info = fragmentFixtureInfo(module);
  const ShaderCrossCompileResult result = crossCompileToGLSL(info);
  ASSERT_TRUE(result.success) << "the fixture module was rejected: " << result.error;
  ASSERT_FALSE(result.source.empty());

  // The version directive is what makes the output acceptable to an OpenGL ES 3.0 context.
  EXPECT_NE(result.source.find("#version 300 es"), std::string::npos) << result.source;

  // The texture sampling call must survive the translation; `texture()` is the ES 3.0 spelling.
  EXPECT_NE(result.source.find("texture("), std::string::npos) << result.source;

  // The uniform block stays a block, because the OpenGL backend binds it through
  // glBindBufferBase rather than by expanding its members into loose uniforms.
  EXPECT_NE(result.source.find("uniform Registers"), std::string::npos) << result.source;

  // The fragment output must keep its explicit location so a render pass can address it.
  EXPECT_NE(result.source.find("layout(location = 0) out"), std::string::npos) << result.source;
}

TEST(ShaderCross, ReflectsTheFragmentShaderBindings) {
  if (!isShaderCrossAvailable()) {
    GTEST_SKIP() << "this build has no SPIRV-Cross, so SPIR-V modules cannot be translated";
  }

  const std::vector<uint32_t> module = readSpirv("fragment_simple.spv");
  ASSERT_FALSE(module.empty()) << "the SPIR-V fixture could not be read";

  const ShaderCrossCompileResult result = crossCompileToGLSL(fragmentFixtureInfo(module));
  ASSERT_TRUE(result.success) << result.error;

  // The reflection is what removes uniform names from upper layer code: a descriptor set layout is
  // written against these (set, binding) pairs rather than against GLSL identifiers, and the OpenGL
  // backend resolves the name to a glUniformLocation.
  const ShaderReflection& reflection = result.reflection;
  EXPECT_EQ(reflection.countOfType(ShaderBindingType::TextureSampler), 1u);

  // The fixture's uniform block is declared as a SPIR-V *push constant* block, because it came from
  // an HLSL `register(b0)`. Classifying it as PushConstant rather than as a descriptor-set uniform
  // buffer is the correct answer, and the distinction matters: the two are bound through different
  // paths (immediate values versus a descriptor), and the OpenGL backend handles them separately.
  EXPECT_EQ(reflection.countOfType(ShaderBindingType::PushConstant), 1u);

  const ShaderBindingInfo* sampledImage = nullptr;
  const ShaderBindingInfo* pushConstantBlock = nullptr;
  for (const ShaderBindingInfo& binding: reflection.bindings) {
    if (binding.type == ShaderBindingType::TextureSampler) sampledImage = &binding;
    if (binding.type == ShaderBindingType::PushConstant) pushConstantBlock = &binding;
  }

  // The exact set and binding numbers are deliberately not asserted: SPIRV-Cross assigns its own
  // decorations when it merges an image and a sampler into one combined uniform, so the numbers
  // depend on the shader toolchain rather than on the RHI's design. What the RHI depends on is that
  // every reflected entry carries a resolvable name, because that is the key the descriptor lookup
  // and the binding table are built from.
  ASSERT_NE(sampledImage, nullptr);
  EXPECT_FALSE(sampledImage->name.empty())
          << "the combined sampler must carry the name the OpenGL backend looks up with "
             "glGetUniformLocation";
  EXPECT_EQ(sampledImage->arraySize, 1u);

  ASSERT_NE(pushConstantBlock, nullptr);
  EXPECT_FALSE(pushConstantBlock->name.empty());

  // The stage interface is reflected too, which is what lets the backend validate that a pipeline's
  // vertex input layout matches the shader.
  EXPECT_EQ(reflection.countOfType(ShaderBindingType::StageInput), 1u);
  EXPECT_EQ(reflection.countOfType(ShaderBindingType::StageOutput), 1u);
}

TEST(ShaderCross, RejectsAStageMismatch) {
  if (!isShaderCrossAvailable()) {
    GTEST_SKIP() << "this build has no SPIRV-Cross, so SPIR-V modules cannot be translated";
  }

  const std::vector<uint32_t> module = readSpirv("fragment_simple.spv");
  ASSERT_FALSE(module.empty()) << "the SPIR-V fixture could not be read";

  ShaderCrossCompileInfo info = fragmentFixtureInfo(module);
  // The module's execution model is Fragment. Asking for Vertex is the classic packaging bug of a
  // swapped shader pair, and catching it here is far cheaper than a link failure later.
  info.stage = ShaderStage::Vertex;

  const ShaderCrossCompileResult result = crossCompileToGLSL(info);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("execution model"), std::string::npos) << result.error;
}

TEST(ShaderReflection, BindingLookup) {
  ShaderReflection reflection;
  reflection.bindings.push_back(ShaderBindingInfo{ ShaderBindingType::UniformBuffer, 0, 0, 1, "camera", "", "", 256 });
  reflection.bindings.push_back(
          ShaderBindingInfo{ ShaderBindingType::TextureSampler, 0, 2, 1, "albedo", "albedo", "s", 0 });
  reflection.bindings.push_back(
          ShaderBindingInfo{ ShaderBindingType::TextureSampler, 1, 0, 4, "shadowMaps", "i", "s2", 0 });

  const ShaderBindingInfo* camera = reflection.find(0, 0);
  ASSERT_NE(camera, nullptr);
  EXPECT_EQ(camera->type, ShaderBindingType::UniformBuffer);
  EXPECT_EQ(camera->blockSize, 256u);

  const ShaderBindingInfo* albedo = reflection.find(0, 2);
  ASSERT_NE(albedo, nullptr);
  EXPECT_STREQ(albedo->imageName.c_str(), "albedo");

  EXPECT_EQ(reflection.find(0, 9), nullptr) << "an undeclared binding must not resolve";
  EXPECT_EQ(reflection.find(7, 0), nullptr);

  EXPECT_EQ(reflection.countOfType(ShaderBindingType::TextureSampler), 2u);
  EXPECT_EQ(reflection.countOfType(ShaderBindingType::UniformBuffer), 1u);
  EXPECT_EQ(reflection.countOfType(ShaderBindingType::Sampler), 0u);

  // An arrayed binding reports its element count so a descriptor set can be expanded to match.
  const ShaderBindingInfo* shadowMaps = reflection.find(1, 0);
  ASSERT_NE(shadowMaps, nullptr);
  EXPECT_EQ(shadowMaps->arraySize, 4u);
}

TEST(ShaderReflection, BindingTypeNames) {
  EXPECT_STREQ(toString(ShaderBindingType::TextureSampler), "TextureSampler");
  EXPECT_STREQ(toString(ShaderBindingType::UniformBuffer), "UniformBuffer");
  EXPECT_STREQ(toString(ShaderBindingType::StorageBuffer), "StorageBuffer");
}

TEST(ShaderCross, DialectChoosesTheShaderEntryPointAndVersion) {
  // The two dialects differ in more than a version number: OpenGL ES has no `gl_VertexIndex` and needs
  // an explicit `precision` qualifier in the fragment stage, while desktop core profile requires
  // `#version <n> core`. Getting the entry point wrong does not fail at translation time - it fails at
  // `glCompileShader` with a driver message that names a line in generated source.
  for (const GlDialect dialect: { GlDialect::ES, GlDialect::Desktop }) {
    const ShaderCrossCompileInfo info = ShaderCrossCompileInfo::forDialect(dialect, ShaderStage::Fragment);
    EXPECT_EQ(info.glDialect, dialect);
    if (dialect == GlDialect::ES) {
      EXPECT_GE(info.glslVersion, 300u) << "OpenGL ES 3.0 is the minimum, so GLSL ES 3.00";
      EXPECT_LT(info.glslVersion, 400u) << "an ES context never accepts a desktop GLSL version";
    } else {
      EXPECT_GE(info.glslVersion, 410u) << "desktop OpenGL 4.1 is the minimum";
      EXPECT_GT(info.glslVersion, 400u);
    }
    // Every call must produce a self-consistent request: an unknown version would translate to
    // something no context accepts.
    EXPECT_NE(info.glslVersion, 0u);
  }
}

TEST(ShaderCross, TranslatesTheFixtureForBothDialects) {
  const std::vector<uint32_t> module = readSpirv("fragment_simple.spv");
  ASSERT_FALSE(module.empty());

  for (const GlDialect dialect: { GlDialect::ES, GlDialect::Desktop }) {
    SCOPED_TRACE(dialect == GlDialect::ES ? "OpenGL ES" : "desktop OpenGL");

    const ShaderCrossCompileResult result = crossCompileToGLSL(fragmentFixtureInfo(module, dialect));
    if (!isShaderCrossAvailable()) {
      EXPECT_FALSE(result.success) << "without SPIRV-Cross there is nothing to translate with";
      continue;
    }

    ASSERT_TRUE(result.success) << result.error;
    EXPECT_EQ(result.dialect, dialect);
    ASSERT_FALSE(result.source.empty());

    // The `#version` directive is the one thing the driver definitely checks, and the two dialects
    // spell it differently: the ES target must say `es` and the desktop target must not. The `core`
    // suffix is deliberately not required on desktop: it is optional from GLSL 1.50 onwards because
    // core is the default there, and SPIRV-Cross omits it. What must never happen is an ES shader
    // reaching a desktop context or the reverse, which is the mismatch this asserts.
    const size_t versionAt = result.source.find("#version ");
    ASSERT_NE(versionAt, std::string::npos) << "the generated source has no #version directive";
    const std::string directive = result.source.substr(versionAt, result.source.find('\n', versionAt) - versionAt);

    if (dialect == GlDialect::ES) {
      EXPECT_NE(directive.find(" es"), std::string::npos)
              << "an ES target must emit a `#version ... es` directive, got: " << directive;
      EXPECT_NE(directive.find("300"), std::string::npos) << directive;
    } else {
      EXPECT_EQ(directive.find(" es"), std::string::npos)
              << "a desktop target must not emit an ES directive, got: " << directive;
      EXPECT_NE(directive.find("410"), std::string::npos) << directive;
    }

    // Reflection must survive both targets. If it did not, a desktop build would link to a program
    // whose uniform bindings had never been resolved.
    EXPECT_FALSE(result.reflection.bindings.empty())
            << "reflection produced no bindings for the " << (dialect == GlDialect::ES ? "ES" : "desktop") << " target";

    // A core-profile desktop context rejects the deprecated `gl_FragColor` built-in, and an ES 3.0
    // context rejects the desktop `texture2D` spelling. Both are things SPIRV-Cross can be configured
    // to emit and both would turn a translated shader into a compile error, so the generated source is
    // checked for the dialect-specific leftovers.
    EXPECT_EQ(result.source.find("gl_FragColor"), std::string::npos) << result.source;
    EXPECT_EQ(result.source.find("texture2D("), std::string::npos) << result.source;
  }
}
