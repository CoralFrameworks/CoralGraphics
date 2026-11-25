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

#include "MetalCross.h"

#include "private/backend/metal/MetalEnums.h"

#include <utility>

#if CORAL_BACKEND_HAS_SPIRV_CROSS
// SPIRV-Cross is third-party code compiled with a relaxed warning set; wrapping the includes keeps its diagnostics out
// of this translation unit without turning warnings off for our own code.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wshadow"
#endif

#include <spirv.hpp>
#include <spirv_msl.hpp>

#if defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

#endif // CORAL_BACKEND_HAS_SPIRV_CROSS

namespace backend::metal {

namespace {

#if CORAL_BACKEND_HAS_SPIRV_CROSS

ShaderStage stageFromExecutionModel(const spv::ExecutionModel model) noexcept {
  switch (model) {
    case spv::ExecutionModelVertex:
      return ShaderStage::Vertex;
    case spv::ExecutionModelFragment:
      return ShaderStage::Fragment;
    case spv::ExecutionModelGLCompute:
    case spv::ExecutionModelKernel:
      return ShaderStage::Compute;
    case spv::ExecutionModelGeometry:
      return ShaderStage::Geometry;
    case spv::ExecutionModelTessellationControl:
      return ShaderStage::TessellationControl;
    case spv::ExecutionModelTessellationEvaluation:
      return ShaderStage::TessellationEvaluation;
    default:
      return ShaderStage::Vertex;
  }
}

spv::ExecutionModel executionModelFor(const ShaderStage stage) noexcept {
  switch (stage) {
    case ShaderStage::Vertex:
      return spv::ExecutionModelVertex;
    case ShaderStage::Fragment:
      return spv::ExecutionModelFragment;
    case ShaderStage::Compute:
      return spv::ExecutionModelGLCompute;
    case ShaderStage::Geometry:
      return spv::ExecutionModelGeometry;
    case ShaderStage::TessellationControl:
      return spv::ExecutionModelTessellationControl;
    case ShaderStage::TessellationEvaluation:
      return spv::ExecutionModelTessellationEvaluation;
  }
  return spv::ExecutionModelVertex;
}

#endif // CORAL_BACKEND_HAS_SPIRV_CROSS

} // namespace anonymous

bool isMslCrossAvailable() noexcept {
#if CORAL_BACKEND_HAS_SPIRV_CROSS
  return true;
#else
  return false;
#endif
}

MslCompileResult crossCompileToMsl(const MslCompileInfo& info) {
  MslCompileResult result;

  if (info.spirv == nullptr || info.wordCount == 0) {
    result.error = "no SPIR-V input was supplied";
    return result;
  }

  // The magic-number check runs before SPIRV-Cross is constructed, for the same reason as in ShaderCross: a corrupt
  // shader asset should produce this message rather than a translator exception.
  constexpr uint32_t kSpirvMagicNumber = 0x07230203u;
  if (info.spirv[0] != kSpirvMagicNumber) {
    result.error = "the supplied data is not a SPIR-V module (the magic number is missing)";
    return result;
  }

#if !CORAL_BACKEND_HAS_SPIRV_CROSS
  result.error = "this build was compiled without SPIRV-Cross, so a SPIR-V module cannot be translated to Metal "
                 "Shading Language; rebuild with CORAL_BACKEND_SUPPORTS_SPIRV_CROSS=ON";
  return result;
#else
  try {
    spirv_cross::CompilerMSL compiler(info.spirv, info.wordCount);

    spirv_cross::CompilerMSL::Options msl = compiler.get_msl_options();
    msl.platform = info.ios ? spirv_cross::CompilerMSL::Options::iOS : spirv_cross::CompilerMSL::Options::macOS;
    msl.set_msl_version(info.mslVersionMajor, info.mslVersionMinor);
    // Classic explicit indices rather than argument buffers: the RHI's descriptor model is (set, binding) and the
    // backend assigns flat slots, so an argument buffer would add an encoding layer for no benefit at this stage.
    msl.argument_buffers = false;
    msl.enable_decoration_binding = false;
    // A fragment output must have at least as many components as the target. The canvas has A8/R8 targets whose
    // shaders still write a vec4, and without this a pipeline for a narrow target fails to create.
    msl.pad_fragment_output_components = true;
    msl.enable_point_size_builtin = true;
    msl.invariant_float_math = false;
    compiler.set_msl_options(msl);

    // `CompilerMSL` does *not* inherit `CompilerGLSL::Options`; the vertex fixups live in the latter and are read by
    // MSL's codegen, so both have to be set. See the note on this in MetalCross.h.
    spirv_cross::CompilerGLSL::Options common;
    common.vertex.fixup_clipspace = false;
    common.vertex.flip_vert_y = true;
    compiler.set_common_options(common);

    // The module's own execution model is authoritative; a caller-supplied stage is a hint. Using the module's catches the
    // packaging bug of a vertex and fragment module being swapped, which would otherwise fail much later in Metal.
    if (const ShaderStage actual = stageFromExecutionModel(compiler.get_execution_model()); actual != info.stage) {
      result.error = std::string("the SPIR-V module's execution model (") + toString(actual) +
                     ") does not match the requested stage (" + toString(info.stage) + ')';
      return result;
    }

    const spv::ExecutionModel model = executionModelFor(info.stage);
    const std::string entry = info.entryPoint != nullptr && *info.entryPoint != '\0' ? info.entryPoint : "main";

    // Selecting the entry point is unconditional: `get_entry_point()` is protected, and the translator reports a name
    // the module does not declare itself when the module is compiled.
    compiler.set_entry_point(entry, model);

    for (size_t index = 0; index < info.bindingCount; ++index) {
      const MslResourceBinding& source = info.bindings[index];
      spirv_cross::MSLResourceBinding binding;
      binding.stage = model;
      binding.desc_set = source.set;
      binding.binding = source.binding;
      binding.count = source.count;
      binding.msl_buffer = source.mslBuffer;
      binding.msl_texture = source.mslTexture;
      binding.msl_sampler = source.mslSampler;
      compiler.add_msl_resource_binding(binding);
    }

    if (info.hasPushConstants) {
      // The push-constant block is a buffer in Metal, and its slot is pinned by the backend's argument-table
      // convention. `kPushConstDescSet`/`kPushConstBinding` is how SPIRV-Cross names that pseudo-resource.
      spirv_cross::MSLResourceBinding push;
      push.stage = model;
      push.desc_set = spirv_cross::kPushConstDescSet;
      push.binding = spirv_cross::kPushConstBinding;
      push.msl_buffer = kPushConstantBufferSlot;
      compiler.add_msl_resource_binding(push);
    }

    // The stage inputs are collected before `compile()`, because a module whose translation fails still has a usable
    // input list for the diagnostic the caller prints.
    if (model == spv::ExecutionModelVertex) {
      for (const spirv_cross::Resource& input: compiler.get_shader_resources().stage_inputs) {
        if (compiler.has_decoration(input.id, spv::DecorationLocation)) {
          result.attributeLocations.push_back(compiler.get_decoration(input.id, spv::DecorationLocation));
        }
      }
    }

    result.source = compiler.compile();
    if (result.source.empty()) {
      result.error = "SPIRV-Cross produced no output for this module";
      return result;
    }

    result.entryPoint = compiler.get_cleansed_entry_point_name(entry, model);
    if (result.entryPoint == entry && entry == "main") {
      // Defensive: SPIRV-Cross renames `main` to `main0` in MSL, and asking Metal for a function called `main` would
      // fail with a message that does not mention the renaming.
      result.entryPoint = "main0";
    }
    result.success = true;
    return result;
  } catch (const std::exception& exception) {
    result.success = false;
    result.error = std::string("SPIRV-Cross could not translate the module to MSL: ") + exception.what();
    return result;
  } catch (...) {
    result.success = false;
    result.error = "SPIRV-Cross could not translate the module to MSL (unknown error)";
    return result;
  }
#endif
}

} // namespace backend::metal
