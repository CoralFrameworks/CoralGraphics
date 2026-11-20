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
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF
 * ANY KIND, either express or implied. See the License for the specific language governing permissions
 * and limitations under the License.
 */

/**
 * @file test_GlslCompiler.cpp
 *
 * GLSL source to SPIR-V.
 *
 * The cases here are chosen around the constraint that shapes the whole file: **glslang will not emit
 * SPIR-V below GLSL ES 3.10 or desktop GLSL 3.30**, so ESSL 1.00 and ESSL 3.00 source has to be
 * retargeted before it will compile. That is a surprising enough behaviour that it needs to be pinned
 * by tests rather than described in a comment, because a future glslang that changes the floor - in
 * either direction - would otherwise change what Coral silently does to a caller's shader.
 *
 * Everything here is device-free: a compiler is a library call, not a GPU.
 */

#include "test_asserts.h"

#include "GlslCompiler.h"
#include "ShaderCross.h"

#include <backend/Descriptors.h>
#include <backend/Device.h>
#include <backend/Types.h>

#include <string>

using namespace backend;

namespace {

/** A vertex shader body that is valid in ESSL 3.10 and desktop GLSL 4.10 alike. */
const char* kModernBody = "precision mediump float;\n"
                          "layout(location = 0) in vec4 aPos;\n"
                          "layout(location = 0) out vec4 vColor;\n"
                          "void main() { vColor = aPos; gl_Position = aPos; }\n";

/** An ESSL 1.00 body that uses the keywords ES 3.00 removed. */
const char* kLegacyBody = "precision mediump float;\n"
                          "attribute vec4 aPos;\n"
                          "varying vec4 vColor;\n"
                          "void main() { vColor = aPos; gl_Position = aPos; }\n";

} // namespace anonymous

TEST(GlslCompiler, ReportsWhetherSourceCompilationIsSupported) {
  // Coral can be built without shaderc, and a caller has to be told which build it got. The two
  // accessors must agree: a version string that is not "unavailable" for a build that cannot compile
  // would make a cache key lie about what produced the words.
  const bool available = isShaderCompilerAvailable();
  const char* version = getShaderCompilerVersion();
  ASSERT_NE(version, nullptr);

  if (available) {
    EXPECT_STRNE(version, "unavailable");
  } else {
    EXPECT_STREQ(version, "unavailable");
  }
}

TEST(GlslCompiler, ParsesTheVersionDirective) {
  // The parse decides which language profile a shader is compiled as, so getting it wrong compiles a
  // shader as a language it was not written in. Each case below is one the specification has an answer
  // for, and the answer is not always "whatever looks closest".
  struct Case {
    const char* source;
    ShaderSourceLanguage language;
    uint32_t version;
    bool declared;
  };

  const Case cases[] = {
    // No directive at all is ESSL 1.00 by the specification's own rule, not an error.
    { "void main() {}\n", ShaderSourceLanguage::ESSL100, 100, false },
    { "#version 300 es\nvoid main() {}\n", ShaderSourceLanguage::ESSL300, 300, true },
    { "#version 310 es\nvoid main() {}\n", ShaderSourceLanguage::ESSL300, 310, true },
    { "#version 100\nvoid main() {}\n", ShaderSourceLanguage::ESSL100, 100, true },
    { "#version 410 core\nvoid main() {}\n", ShaderSourceLanguage::GLSL410, 410, true },
    // Blank lines and comments may precede the directive; only the first *code* line may not.
    { "\n\n// a comment\n#version 300 es\nvoid main() {}\n", ShaderSourceLanguage::ESSL300, 300, true },
    // A `#version` after code is not a version directive, and a source that starts with code has none.
    { "void main() {}\n#version 300 es\n", ShaderSourceLanguage::ESSL100, 100, false },
    // A BOM is tolerated rather than rejected as an unexpected token on line 1.
    { "\xEF\xBB\xBF#version 300 es\nvoid main() {}\n", ShaderSourceLanguage::ESSL300, 300, true },
  };

  for (const Case& testCase: cases) {
    ShaderSourceInfo info;
    parseGlslVersion(testCase.source, &info);
    SCOPED_TRACE(testCase.source);
    EXPECT_EQ(info.language, testCase.language);
    EXPECT_EQ(info.version, testCase.version);
    EXPECT_EQ(info.versionWasDeclared, testCase.declared);
    EXPECT_NE(info.sourceHash, 0u) << "a non-empty source must hash to something";
  }
}

TEST(GlslCompiler, SourceHashTracksTheText) {
  // The hash is what a reload path and a cache use to decide whether anything changed, so it has to be
  // stable for identical text and different for text that differs by even one byte.
  ShaderSourceInfo first;
  ShaderSourceInfo second;
  parseGlslVersion("#version 300 es\nvoid main() {}\n", &first);
  parseGlslVersion("#version 300 es\nvoid main() {}\n", &second);
  EXPECT_EQ(first.sourceHash, second.sourceHash) << "identical sources must hash identically";

  ShaderSourceInfo third;
  parseGlslVersion("#version 300 es\nvoid main() { }\n", &third);
  EXPECT_NE(first.sourceHash, third.sourceHash) << "one extra byte must change the hash";

  ShaderSourceInfo empty;
  parseGlslVersion("", &empty);
  EXPECT_EQ(empty.sourceSize, 0u);
}

TEST(GlslCompiler, ReportsTheVersionFloorTheCompilerEnforces) {
  // The floor is a property of glslang, not a choice, and it is what makes ESSL 1.00 and ESSL 3.00
  // source need retargeting. Pinning it here means a glslang upgrade that moved the floor would fail a
  // test rather than silently start compiling shaders as a different language.
  EXPECT_EQ(getMinimumSpirvSourceVersion(ShaderSourceLanguage::ESSL100), 310u);
  EXPECT_EQ(getMinimumSpirvSourceVersion(ShaderSourceLanguage::ESSL300), 310u);
  EXPECT_EQ(getMinimumSpirvSourceVersion(ShaderSourceLanguage::GLSL410), 410u);
}

TEST(GlslCompiler, CompilesModernSourceToSpirv) {
  if (!isShaderCompilerAvailable()) {
    GTEST_SKIP() << "this build has no GLSL compiler";
  }

  for (const auto& [label, version]: { std::pair<const char*, uint32_t>{ "#version 310 es\n", 310 },
           std::pair<const char*, uint32_t>{ "#version 410 core\n", 410 } }) {
    SCOPED_TRACE(label);
    const GlslCompileResult result =
            compileGlslToSpirv(std::string(label) + kModernBody, ShaderStage::Vertex, GlDialect::ES);
    ASSERT_TRUE(result.success()) << result.error;

    // The SPIR-V magic number is the one thing that proves the output is a module rather than a
    // compiler's idea of one.
    ASSERT_FALSE(result.spirv.empty());
    EXPECT_EQ(result.spirv[0], 0x07230203u);
    EXPECT_EQ(result.sourceInfo.version, version);
    EXPECT_FALSE(result.versionWasRetargeted) << "a source at or above the floor must not be retargeted";
  }
}

TEST(GlslCompiler, RetargetsEssl300AndSaysSo) {
  if (!isShaderCompilerAvailable()) {
    GTEST_SKIP() << "this build has no GLSL compiler";
  }

  // Without the retarget this fails outright: glslang's own message is
  // "#version: ES shaders for SPIR-V require version 310 or higher".
  const GlslCompileResult result =
          compileGlslToSpirv(std::string("#version 300 es\n") + kModernBody, ShaderStage::Vertex, GlDialect::ES);

  ASSERT_TRUE(result.success()) << result.error;
  // What the author wrote, and what the compiler used, must both be reported and must not be conflated.
  EXPECT_EQ(result.sourceInfo.version, 300u);
  EXPECT_EQ(result.compiledVersion, 310u);
  EXPECT_TRUE(result.versionWasRetargeted);
  EXPECT_EQ(result.spirv[0], 0x07230203u);
}

TEST(GlslCompiler, RetargetsEssl100AndSaysSo) {
  if (!isShaderCompilerAvailable()) {
    GTEST_SKIP() << "this build has no GLSL compiler";
  }

  // A source with no `#version` is ESSL 1.00, and a body that happens to be valid modern GLSL compiles
  // once retargeted.
  const GlslCompileResult result = compileGlslToSpirv(kModernBody, ShaderStage::Vertex, GlDialect::ES);

  ASSERT_TRUE(result.success()) << result.error;
  EXPECT_EQ(result.sourceInfo.language, ShaderSourceLanguage::ESSL100);
  EXPECT_EQ(result.sourceInfo.version, 100u);
  EXPECT_FALSE(result.sourceInfo.versionWasDeclared);
  EXPECT_TRUE(result.versionWasRetargeted);
  EXPECT_EQ(result.spirv[0], 0x07230203u);
}

TEST(GlslCompiler, ExplainsWhyLegacyEssl100CannotCompile) {
  if (!isShaderCompilerAvailable()) {
    GTEST_SKIP() << "this build has no GLSL compiler";
  }

  // `attribute` and `varying` were removed in ES 3.00, so no amount of version retargeting makes this
  // compile. The point of the test is the *message*: the compiler's own error names a line and a token,
  // and the added advice names the cause, which is the difference between a five-minute fix and an
  // afternoon of guessing why ES 1.00 source will not produce SPIR-V.
  const GlslCompileResult result = compileGlslToSpirv(kLegacyBody, ShaderStage::Vertex, GlDialect::ES);

  EXPECT_FALSE(result.success());
  EXPECT_FALSE(result.error.empty());
  EXPECT_NE(result.error.find("attribute"), std::string::npos) << result.error;
  EXPECT_NE(result.error.find("removed in OpenGL ES 3.00"), std::string::npos) << result.error;
  EXPECT_TRUE(result.spirv.empty()) << "a failed compile must not leave partial output";
}

TEST(GlslCompiler, AnExplicitLanguageOverridesTheSource) {
  if (!isShaderCompilerAvailable()) {
    GTEST_SKIP() << "this build has no GLSL compiler";
  }

  // A desktop override needs the source to declare its own `#version`, and this is not fussiness: the
  // natural alternative, shaderc's forced-version hook, makes glslang fail on its own built-in symbols
  // for a desktop profile ("Unable to parse built-ins"). So an undeclared desktop source is refused.
  const GlslCompileResult refused =
          compileGlslToSpirv(kModernBody, ShaderStage::Vertex, GlDialect::Desktop, ShaderSourceLanguage::GLSL410);
  EXPECT_FALSE(refused.success());
  EXPECT_NE(refused.error.find("must declare its own"), std::string::npos) << refused.error;

  // Declared, it compiles, and the reported language is the one that was asked for rather than the one
  // the parse would have inferred.
  const GlslCompileResult result = compileGlslToSpirv(std::string("#version 410 core\n") + kModernBody,
          ShaderStage::Vertex, GlDialect::Desktop, ShaderSourceLanguage::GLSL410);
  ASSERT_TRUE(result.success()) << result.error;
  EXPECT_EQ(result.sourceInfo.language, ShaderSourceLanguage::GLSL410);
  EXPECT_EQ(result.sourceInfo.version, 410u);
  // Retargeted *upwards*, which is the one direction that cannot break a valid source: 420 is a superset of
  // 410, and it is where `layout(binding = N)` becomes legal -- see
  // `GlslCompiler.AcceptsTheBindingQualifierAtTheDesktopFloor`, which is the test that would fail if this
  // floor were lowered back to the context's own 410.
  EXPECT_EQ(result.compiledVersion, getRetargetVersion(ShaderSourceLanguage::GLSL410, GlDialect::Desktop));
  EXPECT_TRUE(result.versionWasRetargeted) << "the desktop floor is above 410, so a 410 source is retargeted";
}

TEST(GlslCompiler, AcceptsTheBindingQualifierAtTheDesktopFloor) {
  if (!isShaderCompilerAvailable()) {
    GTEST_SKIP() << "this build has no GLSL compiler";
  }

  // The qualifier every shader written in the canvas's style needs, at the floor this backend compiles a
  // desktop source at. At 410 glslang rejects both declarations -- `'binding' : not supported for this
  // version` for the sampler, and `'binding' : uniform/buffer blocks require layout(binding=X)` for a block
  // that omits it, so a block cannot be declared at all. That made a run-time shader unable to use the shape
  // the library's own shaders use, and left reflection with nothing to report.
  const char* source = R"(#version 410 core
layout(location = 0) out vec4 o;
layout(binding = 0) uniform sampler2D uInput;
layout(binding = 1) uniform Block {
  vec4 uColour;
} params;
void main() {
  o = texture(uInput, vec2(0.0)) * params.uColour;
}
)";
  const GlslCompileResult compiled = compileGlslToSpirv(source, ShaderStage::Fragment, GlDialect::Desktop,
          ShaderSourceLanguage::GLSL410);
  ASSERT_TRUE(compiled.success()) << compiled.error;
  EXPECT_GE(compiled.compiledVersion, 420u) << "the qualifier needs 420, so the floor has to be at least that";

  // And the decorations survive into the SPIR-V, which is what makes the bindings *reflectable* rather than
  // something the caller has to restate. `spirv-cross` reads exactly these.
  ShaderCrossCompileInfo info = ShaderCrossCompileInfo::forDialect(GlDialect::Desktop, ShaderStage::Fragment);
  info.spirv = compiled.spirv.data();
  info.wordCount = compiled.spirv.size();
  info.flipVertexY = true;
  info.fixupClipSpace = true;
  info.combineImageSamplers = true;
  const ShaderCrossCompileResult translated = crossCompileToGLSL(info);
  ASSERT_TRUE(translated.success) << translated.error;

  const ShaderBindingInfo* sampler = nullptr;
  const ShaderBindingInfo* block = nullptr;
  for (const ShaderBindingInfo& binding: translated.reflection.bindings) {
    if (binding.binding == 0 && binding.type == ShaderBindingType::TextureSampler) sampler = &binding;
    if (binding.binding == 1 && binding.type == ShaderBindingType::UniformBuffer) block = &binding;
  }
  ASSERT_NE(sampler, nullptr) << "the sampler's binding was not reflected";
  EXPECT_EQ(sampler->name, "uInput");
  ASSERT_NE(block, nullptr) << "the uniform block's binding was not reflected";
  EXPECT_EQ(block->name, "Block");
}

TEST(GlslCompiler, RejectsPushConstantsForTheOpenGlTarget) {
  if (!isShaderCompilerAvailable()) {
    GTEST_SKIP() << "this build has no GLSL compiler";
  }

  // `push_constant` is Vulkan's spelling, and glslang refuses it for the OpenGL target environment at every
  // version -- 420 and 450 included, so this is not the version floor of
  // `AcceptsTheBindingQualifierAtTheDesktopFloor` being too low. It is pinned because it decides what the
  // OpenGL backend's *emulated* push constants are for: a module only carries a push-constant block if it was
  // compiled for Vulkan (offline, or by the caller's own tool) and handed over as words. A caller who reads
  // `Limits::maxPushConstantSize` as "I can write this in GLSL" is wrong, and this test is the record of that.
  const char* source = R"(#version 420 core
layout(location = 0) out vec4 o;
layout(push_constant) uniform Params { vec4 uColour; } params;
void main() { o = params.uColour; }
)";
  for (const auto& [target, language]: { std::pair{ GlDialect::Desktop, ShaderSourceLanguage::GLSL410 },
           std::pair{ GlDialect::ES, ShaderSourceLanguage::ESSL300 } }) {
    const GlslCompileResult result =
            compileGlslToSpirv(source, ShaderStage::Fragment, target, language);
    EXPECT_FALSE(result.success()) << "an OpenGL target must not accept a push-constant declaration";
    EXPECT_NE(result.error.find("push_constant"), std::string::npos) << result.error;
    EXPECT_NE(result.error.find("Vulkan"), std::string::npos)
            << "the message has to name the reason, or the caller has nothing to act on: " << result.error;
  }
}

TEST(GlslCompiler, RejectsEmptySource) {
  const GlslCompileResult result = compileGlslToSpirv("", ShaderStage::Vertex, GlDialect::ES);
  EXPECT_FALSE(result.success());
  EXPECT_FALSE(result.error.empty()) << "a failure must explain itself";
}

TEST(GlslCompiler, RejectsMalformedSource) {
  if (!isShaderCompilerAvailable()) {
    GTEST_SKIP() << "this build has no GLSL compiler";
  }

  // A shader asset is the most likely thing in a build to be corrupt or half-written, so this must come
  // back as a diagnostic rather than as a crash or as an empty module reported as success.
  const GlslCompileResult result =
          compileGlslToSpirv("#version 310 es\nvoid main() { this is not glsl }\n", ShaderStage::Vertex, GlDialect::ES);

  EXPECT_FALSE(result.success());
  EXPECT_FALSE(result.error.empty());
  EXPECT_TRUE(result.spirv.empty());
}

TEST(GlslCompiler, CompilesEveryStageItClaims) {
  if (!isShaderCompilerAvailable()) {
    GTEST_SKIP() << "this build has no GLSL compiler";
  }

  // A stage that maps to the wrong shaderc kind compiles the source as the wrong stage, which either
  // fails confusingly or, worse, succeeds and produces a module for the wrong pipeline stage.
  const struct {
    ShaderStage stage;
    const char* source;
  } cases[] = {
    { ShaderStage::Vertex, "#version 310 es\nvoid main() { gl_Position = vec4(0.0); }\n" },
    { ShaderStage::Fragment, "#version 310 es\nprecision mediump float;\n"
                             "layout(location = 0) out vec4 o;\nvoid main() { o = vec4(1.0); }\n" },
    { ShaderStage::Compute, "#version 310 es\nlayout(local_size_x = 1) in;\n"
                            "layout(std430, binding = 0) buffer B { float v[]; } b;\n"
                            "void main() { b.v[0] = 1.0; }\n" },
  };

  for (const auto& testCase: cases) {
    SCOPED_TRACE(toString(testCase.stage));
    const GlslCompileResult result = compileGlslToSpirv(testCase.source, testCase.stage, GlDialect::ES);
    ASSERT_TRUE(result.success()) << result.error;
    ASSERT_FALSE(result.spirv.empty());
    EXPECT_EQ(result.spirv[0], 0x07230203u);
  }
}

TEST(GlslCompiler, FunctionPointersReportHonestly) {
  // These are the two questions a caller asks before deciding whether to author in GLSL at all, so they
  // must be answerable without compiling anything and must agree with what compilation then does.
  const bool available = isShaderCompilerAvailable();
  const GlslCompileResult probe =
          compileGlslToSpirv(std::string("#version 310 es\n") + kModernBody, ShaderStage::Vertex, GlDialect::ES);

  EXPECT_EQ(probe.success(), available)
          << "a build that claims to compile must compile, and one that does not must say why";
  if (available) {
    EXPECT_TRUE(probe.success()) << probe.error;
    EXPECT_STRNE(getShaderCompilerVersion(), "unavailable");
  }
}
