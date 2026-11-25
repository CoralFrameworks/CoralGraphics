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
 * Tests for the SPIR-V -> MSL translation path.
 *
 * The Metal twin of `test_ShaderCross.cpp`, and it exists for the same reason: the translator is the piece most likely
 * to break silently when SPIRV-Cross is updated, and a failure there otherwise shows up as a pipeline that cannot be
 * created rather than as a message about the shader.
 *
 * The fixture is the real compiler-produced module the GLSL tests use, so a failure here cannot be blamed on a
 * hand-assembled input. It declares a sampled image, a push-constant block and a stage output, which between them cover
 * the three parts of the Metal interface this translation has to get right: the `(set, binding)` -> argument-slot
 * remap, the pinned push-constant buffer, and the renamed entry point.
 *
 * **None of this needs a GPU.** `crossCompileToMsl` is SPIRV-Cross and a string, so the test runs on a build machine
 * with no Metal device and on every platform that can build the backend.
 */

#include "test_asserts.h"

#include <metal/MetalCross.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace backend;
using namespace backend::metal;

namespace {

#ifndef CORAL_BACKEND_FIXTURE_DIR
#define CORAL_BACKEND_FIXTURE_DIR ""
#endif

std::string fixturePath(const char* name) {
  const std::string configured = std::string(CORAL_BACKEND_FIXTURE_DIR) + "/" + name;
  if (std::FILE* probe = std::fopen(configured.c_str(), "rb")) {
    std::fclose(probe);
    return configured;
  }
  // The test runs from the module directory under CTest; walking up from it finds the source tree fixture.
  const std::string candidates[] = { std::string("tests/fixtures/") + name,
    std::string("backend/tests/fixtures/") + name, std::string("../tests/fixtures/") + name };
  for (const std::string& candidate: candidates) {
    if (std::FILE* probe = std::fopen(candidate.c_str(), "rb")) {
      std::fclose(probe);
      return candidate;
    }
  }
  return {};
}

std::vector<uint32_t> readSpirv(const char* name) {
  std::vector<uint32_t> words;
  const std::string path = fixturePath(name);
  if (path.empty()) return words;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return words;
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (size > 0 && size % 4 == 0) {
    words.resize(static_cast<size_t>(size) / 4);
    if (std::fread(words.data(), 1, static_cast<size_t>(size), file) != static_cast<size_t>(size)) {
      words.clear();
    }
  }
  std::fclose(file);
  return words;
}

/** The fixture's one sampled image and one push-constant block, mapped onto this backend's template slots. */
MslCompileInfo fragmentFixtureInfo(const std::vector<uint32_t>& module, MslResourceBinding* bindings) {
  bindings[0] = MslResourceBinding{};
  bindings[0].stage = ShaderStage::Fragment;
  bindings[0].set = 0;
  bindings[0].binding = 0;
  bindings[0].count = 1;
  bindings[0].mslTexture = 3;
  bindings[0].mslSampler = 2;

  // The fixture merges an image and a sampler; covering (0, 1) as well means the test does not depend on which of the
  // two bindings SPIRV-Cross used for the combined variable.
  bindings[1] = MslResourceBinding{};
  bindings[1].stage = ShaderStage::Fragment;
  bindings[1].set = 0;
  bindings[1].binding = 1;
  bindings[1].count = 1;
  bindings[1].mslTexture = 3;
  bindings[1].mslSampler = 2;

  MslCompileInfo info;
  info.spirv = module.data();
  info.wordCount = module.size();
  info.stage = ShaderStage::Fragment;
  info.bindings = bindings;
  info.bindingCount = 2;
  info.hasPushConstants = true;
  info.ios = false;
  return info;
}

} // namespace anonymous

TEST(MetalCross, IsAvailableWhenTheBackendIsBuilt) {
  EXPECT_TRUE(isMslCrossAvailable()) << "the Metal backend was built without SPIRV-Cross's MSL backend";
}

TEST(MetalCross, RejectsEmptyInput) {
  MslCompileInfo info;
  const MslCompileResult result = crossCompileToMsl(info);
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error.empty());
  EXPECT_NE(result.error.find("SPIR-V"), std::string::npos) << result.error;
}

TEST(MetalCross, RejectsInputWithoutTheSpirvMagicNumber) {
  const uint32_t words[4] = { 0xDEADBEEFu, 0u, 0u, 0u };
  MslCompileInfo info;
  info.spirv = words;
  info.wordCount = 4;
  info.stage = ShaderStage::Fragment;
  const MslCompileResult result = crossCompileToMsl(info);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("magic number"), std::string::npos) << result.error;
}

TEST(MetalCross, RejectsATruncatedModuleWithoutCrashing) {
  // A header-only module: the magic number is right and everything after it is missing. The translator must report a
  // diagnostic rather than abort, because the input comes from a file a caller loaded.
  const uint32_t words[3] = { 0x07230203u, 0x00010000u, 0u };
  MslCompileInfo info;
  info.spirv = words;
  info.wordCount = 3;
  info.stage = ShaderStage::Fragment;
  const MslCompileResult result = crossCompileToMsl(info);
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error.empty());
}

TEST(MetalCross, TranslatesTheFixtureToMsl) {
  if (!isMslCrossAvailable()) GTEST_SKIP() << "this build has no SPIRV-Cross MSL backend";

  const std::vector<uint32_t> module = readSpirv("fragment_simple.spv");
  ASSERT_FALSE(module.empty()) << "the SPIR-V fixture could not be read";

  MslResourceBinding bindings[2];
  const MslCompileInfo info = fragmentFixtureInfo(module, bindings);
  const MslCompileResult result = crossCompileToMsl(info);
  ASSERT_TRUE(result.success) << result.error;
  ASSERT_FALSE(result.source.empty());

  // A fragment shader keeps its output at an explicit colour index, and the texture is sampled with Metal's spelling.
  EXPECT_NE(result.source.find("[[color(0)]]"), std::string::npos) << result.source;
  EXPECT_NE(result.source.find("[[texture("), std::string::npos) << result.source;
  EXPECT_NE(result.source.find("[[sampler("), std::string::npos) << result.source;

  // The push-constant block is a buffer argument at the backend's pinned slot. If this drifts, a draw silently reads
  // whatever else occupies the slot instead of the pushed bytes.
  EXPECT_NE(result.source.find("[[buffer(30)]]"), std::string::npos) << result.source;

  // SPIRV-Cross renames `main` because MSL reserves it; the name the backend must hand `newFunctionWithName:` is the
  // cleansed one, and the result reports it rather than making the device guess.
  EXPECT_EQ(result.entryPoint, "main0") << result.source;
}

TEST(MetalCross, RejectsAStageMismatch) {
  if (!isMslCrossAvailable()) GTEST_SKIP() << "this build has no SPIRV-Cross MSL backend";

  const std::vector<uint32_t> module = readSpirv("fragment_simple.spv");
  ASSERT_FALSE(module.empty()) << "the SPIR-V fixture could not be read";

  MslResourceBinding bindings[2];
  MslCompileInfo info = fragmentFixtureInfo(module, bindings);
  // The module's execution model is Fragment; asking for Vertex is the packaging bug of a swapped pair.
  info.stage = ShaderStage::Vertex;
  const MslCompileResult result = crossCompileToMsl(info);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("execution model"), std::string::npos) << result.error;
}
