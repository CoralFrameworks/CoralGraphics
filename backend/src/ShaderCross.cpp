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

#include "ShaderCross.h"

#include <algorithm>
#include <ranges>
#include <utility>

#if CORAL_BACKEND_HAS_SPIRV_CROSS
// SPIRV-Cross is third-party code: it is compiled with a relaxed warning set, and the headers pull in
// a large amount of template machinery. Wrapping the include keeps its diagnostics out of our build
// without turning off warnings for our own translation units.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wshadow"
#endif

#include <spirv.hpp>
#include <spirv_glsl.hpp>

#if defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

#endif // CORAL_BACKEND_HAS_SPIRV_CROSS

namespace backend {

const char* toString(const ShaderBindingType type) noexcept {
  switch (type) {
    case ShaderBindingType::TextureSampler:
      return "TextureSampler";
    case ShaderBindingType::Sampler:
      return "Sampler";
    case ShaderBindingType::SampledTexture:
      return "SampledTexture";
    case ShaderBindingType::UniformBuffer:
      return "UniformBuffer";
    case ShaderBindingType::StorageBuffer:
      return "StorageBuffer";
    case ShaderBindingType::ReadOnlyStorageBuffer:
      return "ReadOnlyStorageBuffer";
    case ShaderBindingType::PushConstant:
      return "PushConstant";
    case ShaderBindingType::StageInput:
      return "StageInput";
    case ShaderBindingType::StageOutput:
      return "StageOutput";
  }
  return "UnknownBinding";
}

const ShaderBindingInfo* ShaderReflection::find(const uint32_t set, const uint32_t binding) const noexcept {
  for (const ShaderBindingInfo& info: bindings) {
    if (info.set == set && info.binding == binding) {
      return &info;
    }
  }
  return nullptr;
}

std::vector<const ShaderBindingInfo*> ShaderReflection::ofType(const ShaderBindingType type) const {
  std::vector<const ShaderBindingInfo*> result;
  for (const ShaderBindingInfo& info: bindings) {
    if (info.type == type) {
      result.push_back(&info);
    }
  }
  return result;
}

uint32_t ShaderReflection::countOfType(const ShaderBindingType type) const noexcept {
  uint32_t count = 0;
  for (const ShaderBindingInfo& info: bindings) {
    if (info.type == type) {
      ++count;
    }
  }
  return count;
}

bool isShaderCrossAvailable() noexcept {
#if CORAL_BACKEND_HAS_SPIRV_CROSS
  return true;
#else
  return false;
#endif
}

const char* getShaderCrossVersion() noexcept {
#if CORAL_BACKEND_HAS_SPIRV_CROSS
  return "SPIRV-Cross (bundled)";
#else
  return "unavailable";
#endif
}

#if CORAL_BACKEND_HAS_SPIRV_CROSS
namespace {

using namespace spv;

/** Maps a SPIR-V execution model to the RHI's ShaderStage. */
ShaderStage stageFromExecutionModel(const spv::ExecutionModel model) noexcept {
  switch (model) {
    case ExecutionModelVertex:
      return ShaderStage::Vertex;
    case ExecutionModelFragment:
      return ShaderStage::Fragment;
    case ExecutionModelGLCompute:
    case ExecutionModelKernel:
      return ShaderStage::Compute;
    case ExecutionModelGeometry:
      return ShaderStage::Geometry;
    case ExecutionModelTessellationControl:
      return ShaderStage::TessellationControl;
    case ExecutionModelTessellationEvaluation:
      return ShaderStage::TessellationEvaluation;
    default:
      return ShaderStage::Vertex;
  }
}

/**
 * Builds the binding table from a module's resources.
 *
 * This must run *before* `build_combined_image_samplers()`. That pass rewrites separate image and
 * sampler variables into combined ones, and the SPIR-V-Cross resource lists obtained beforehand stop
 * describing the module: reflecting afterwards silently loses every uniform block. The combined
 * sampler names are therefore patched in separately, by `applyCombinedSamplerBindings`.
 */
void collectBindings(spirv_cross::CompilerGLSL& compiler, spirv_cross::ShaderResources& resources,
        ShaderReflection& reflection) {
  const auto readDecorations = [&compiler](const spirv_cross::ID id, uint32_t* set, uint32_t* binding) {
    *set = compiler.has_decoration(id, spv::DecorationDescriptorSet)
                   ? compiler.get_decoration(id, spv::DecorationDescriptorSet)
                   : 0u;
    *binding = compiler.has_decoration(id, spv::DecorationBinding) ? compiler.get_decoration(id, spv::DecorationBinding)
                                                                   : 0u;
  };

  const auto addResource = [&](const spirv_cross::Resource& resource, ShaderBindingType type,
          const char* nameOverride = nullptr) {
    ShaderBindingInfo info;
    info.type = type;
    readDecorations(resource.id, &info.set, &info.binding);
    info.arraySize =
            compiler.get_type(resource.type_id).array.empty() ? 1u : compiler.get_type(resource.type_id).array[0];
    info.name = nameOverride != nullptr ? nameOverride : resource.name;
    reflection.bindings.push_back(std::move(info));
  };

  for (const spirv_cross::Resource& resource: resources.uniform_buffers) {
    addResource(resource, ShaderBindingType::UniformBuffer);
  }
  for (const spirv_cross::Resource& resource: resources.storage_buffers) {
    addResource(resource, ShaderBindingType::StorageBuffer);
  }
  for (const spirv_cross::Resource& resource: resources.separate_images) {
    addResource(resource, ShaderBindingType::SampledTexture);
  }
  for (const spirv_cross::Resource& resource: resources.separate_samplers) {
    addResource(resource, ShaderBindingType::Sampler);
  }
  for (const spirv_cross::Resource& resource: resources.sampled_images) {
    addResource(resource, ShaderBindingType::TextureSampler);
  }
  for (const spirv_cross::Resource& resource: resources.push_constant_buffers) {
    // The block's *name* rather than the variable's, because the name is what `glGetUniformBlockIndex` looks up and
    // GLSL names a block after its type: `layout(push_constant) uniform Params { ... } params;` has the block name
    // "Params" and the variable name "params". Measured: reflecting the variable name made the backend's lookup
    // return `GL_INVALID_INDEX`, which left the emulated push-constant buffer with no block to bind -- the last
    // piece of the defect in note 4.2, and invisible because nothing errors when a buffer is bound to nothing.
    const std::string blockName = compiler.get_name(resource.base_type_id);
    addResource(resource, ShaderBindingType::PushConstant, blockName.empty() ? nullptr : blockName.c_str());
  }
  for (const spirv_cross::Resource& resource: resources.stage_inputs) {
    addResource(resource, ShaderBindingType::StageInput);
  }
  for (const spirv_cross::Resource& resource: resources.stage_outputs) {
    addResource(resource, ShaderBindingType::StageOutput);
  }

  // Uniform blocks report a size, which the OpenGL backend uses to size its emulated push-constant
  // storage and to report a truthful maxUniformBufferBindingSize.
  for (ShaderBindingInfo& info: reflection.bindings) {
    if (info.type != ShaderBindingType::UniformBuffer) {
      continue;
    }
    for (const spirv_cross::Resource& resource: resources.uniform_buffers) {
      if (resource.name != info.name) {
        continue;
      }
      info.blockSize = compiler.get_declared_struct_size(compiler.get_type(resource.base_type_id));
      break;
    }
  }
}

/**
 * Replaces the separate image/sampler entries with the combined `sampler2D` uniforms that
 * `build_combined_image_samplers()` synthesized.
 *
 * OpenGL ES 3.0 has no separate sampler objects, so an image and a sampler that a Vulkan shader
 * declared independently become one combined uniform. The name to bind is the synthesized one, which
 * is why this runs after the pass rather than being folded into `collectBindings`.
 */
void applyCombinedSamplerBindings(spirv_cross::CompilerGLSL& compiler, ShaderReflection& reflection) {
  const auto& combined = compiler.get_combined_image_samplers();
  if (combined.empty()) {
    return;
  }

  // Drop the entries the pass has superseded.
  reflection.bindings.erase(std::ranges::remove_if(reflection.bindings,
                                    [](const ShaderBindingInfo& info) {
                                      return info.type == ShaderBindingType::SampledTexture ||
                                             info.type == ShaderBindingType::Sampler;
                                    })
                                    .begin(),
          reflection.bindings.end());

  for (const spirv_cross::CombinedImageSampler& entry: combined) {
    ShaderBindingInfo info;
    info.type = ShaderBindingType::TextureSampler;

    // The descriptor set and binding come from the original *image* variable, not from the
    // synthesized combined one. SPIRV-Cross gives the combined variable its own generated
    // decorations, which have nothing to do with the author's `layout(set = N, binding = M)`; using
    // them would silently produce a descriptor set layout that never matches the shader.
    const spirv_cross::ID source = entry.image_id != 0 ? entry.image_id : entry.combined_id;
    info.set = compiler.has_decoration(source, spv::DecorationDescriptorSet)
                       ? compiler.get_decoration(source, spv::DecorationDescriptorSet)
                       : 0u;
    info.binding = compiler.has_decoration(source, spv::DecorationBinding)
                           ? compiler.get_decoration(source, spv::DecorationBinding)
                           : 0u;

    const spirv_cross::SPIRType& type = compiler.get_type_from_variable(entry.combined_id);
    info.arraySize = type.array.empty() ? 1u : type.array[0];

    // The name is the one part that must come from the combined variable: it is the identifier the
    // generated GLSL declares, and therefore the only name glGetUniformLocation will accept.
    info.name = compiler.get_name(entry.combined_id);
    info.imageName = compiler.get_name(entry.image_id);
    info.samplerName = compiler.get_name(entry.sampler_id);

    // A combined uniform can collide with an already-reflected `sampled_images` entry, because the
    // pass preserves the original sampled-image decorations. Keeping the combined name is what makes
    // glGetUniformLocation succeed, so the existing entry is replaced rather than duplicated.
    const auto existing = std::ranges::find_if(reflection.bindings, [&info](const ShaderBindingInfo& candidate) {
      return candidate.type == ShaderBindingType::TextureSampler && candidate.set == info.set &&
             candidate.binding == info.binding;
    });
    if (existing != reflection.bindings.end()) {
      *existing = std::move(info);
    } else {
      reflection.bindings.push_back(std::move(info));
    }
  }
}

} // namespace anonymous
#endif // CORAL_BACKEND_HAS_SPIRV_CROSS

ShaderCrossCompileResult crossCompileToGLSL(const ShaderCrossCompileInfo& info) {
  ShaderCrossCompileResult result;
  result.dialect = info.glDialect;

  if (info.spirv == nullptr || info.wordCount == 0) {
    result.error = "no SPIR-V input was supplied";
    return result;
  }

  // SPIR-V modules start with a magic word. Checking it here turns "the translator crashes on
  // garbage input" into a clear message, which matters because shader assets are often loaded from
  // disk and are the most likely thing to be corrupt.
  //
  // The value is spelled out rather than taken from spv::MagicNumber because this check must also
  // work in a build without SPIRV-Cross, where that header is not available.
  constexpr uint32_t kSpirvMagicNumber = 0x07230203u;
  if (info.spirv[0] != kSpirvMagicNumber) {
    result.error = "the supplied data is not a SPIR-V module (the magic number is missing)";
    return result;
  }

#if !CORAL_BACKEND_HAS_SPIRV_CROSS
  result.error = "this build was compiled without SPIRV-Cross, so SPIR-V shaders cannot be translated to GLSL; "
                 "rebuild with CORAL_BACKEND_SUPPORTS_SPIRV_CROSS=ON, or supply GLSL source directly via "
                 "ShaderModuleDescriptor::language";
  (void) info;
  return result;
#else
  try {
    spirv_cross::CompilerGLSL compiler(info.spirv, info.wordCount);

    const bool desktop = info.glDialect == GlDialect::Desktop;

    spirv_cross::CompilerGLSL::Options options;
    options.version = info.glslVersion;
    options.es = !desktop;

    // Desktop OpenGL 4.1 is used in core profile, which means no default-block uniforms and no
    // separate-shader-objects trickery. GLSL ES 3.00 has no separate sampler objects, so image and
    // sampler pairs must be collapsed into combined uniforms there; on desktop they may stay
    // separate, but the conversion is harmless and keeps both dialects binding identically.
    options.separate_shader_objects = false;
    options.vulkan_semantics = false;
    options.emit_uniform_buffer_as_plain_uniforms = false;

    // A push-constant block is *emulated*: the backend fills a small uniform buffer and binds it with
    // `glBindBufferBase`, so the module has to reach the driver as a uniform block. SPIRV-Cross's default is the
    // opposite -- it flattens a push-constant block into plain uniforms, which is fine for a shader that only reads
    // it but leaves nothing to bind: measured, `glGetUniformBlockIndex` then returns `GL_INVALID_INDEX` and the
    // emulation has no buffer to aim at, so the shader reads zeros. That was the last piece of the defect in note
    // 4.2, and it is only reachable through this flag.
    options.emit_push_constant_as_uniform_buffer = true;

    if (desktop) {
      // `GL_ARB_shading_language_420pack` provides the `layout(binding = N)` qualifiers on desktop, which is how
      // uniform blocks and samplers get their binding points without the backend having to assign them.
      //
      // Enabled unconditionally, and that is the fix for the case this used to get wrong: a module whose SPIR-V
      // carries explicit binding decorations (anything written with `layout(binding = N)`, which the 420 source
      // floor now lets a run-time shader declare) makes SPIRV-Cross emit those qualifiers even at `#version 410`,
      // and then the *driver* rejects the source it is handed
      // (`layout(binding) requires "#version 420" or later ... or #extension GL_ARB_shading_language_420pack`).
      // Emitting the extension line whenever the qualifiers appear is what makes the two agree, and SPIRV-Cross
      // only emits it when it emits one of them -- `#extension ... : enable` is also a soft request, so a driver
      // without the extension warns rather than fails.
      options.enable_420pack_extension = true;
      // A core-profile context rejects `gl_FragColor` and the deprecated built-ins; SPIRV-Cross
      // already emits explicit `out` variables for 4.x targets, so nothing further is needed here.
    } else {
      // ES 3.1 and later provide the same packed-binding qualifiers under a different extension.
      options.enable_420pack_extension = info.glslVersion >= 310;
    }

    options.vertex.fixup_clipspace = info.fixupClipSpace;
    options.vertex.flip_vert_y = info.flipVertexY;

    compiler.set_common_options(options);

    // Reflection runs against the module as authored, before the combined-sampler pass rewrites it:
    // after that pass the resource lists no longer describe the module, and reflecting them loses
    // every uniform block. The combined sampler names are patched in immediately afterwards.
    spirv_cross::ShaderResources resources = compiler.get_shader_resources();

    collectBindings(compiler, resources, result.reflection);

    if (info.combineImageSamplers) {
      compiler.build_combined_image_samplers();
      applyCombinedSamplerBindings(compiler, result.reflection);
    }

    // The execution model is authoritative for the stage; the caller's `stage` is only a hint. Using
    // the module's own model catches the common packaging bug of a vertex and fragment module being
    // swapped, which would otherwise fail much later inside glLinkProgram.
    const ShaderStage actualStage = stageFromExecutionModel(compiler.get_execution_model());
    if (actualStage != info.stage) {
      result.error = std::string("the SPIR-V module's execution model (") + toString(actualStage) +
                     ") does not match the requested stage (" + toString(info.stage) + ")";
      return result;
    }

    result.source = compiler.compile();
    result.success = !result.source.empty();
    if (!result.success) {
      result.error = "SPIRV-Cross produced no output for this module";
    }
    return result;
  } catch (const std::exception& exception) {
    result.success = false;
    result.error = std::string("SPIRV-Cross could not translate the module: ") + exception.what();
    return result;
  } catch (...) {
    result.success = false;
    result.error = "SPIRV-Cross could not translate the module (unknown error)";
    return result;
  }
#endif
}

ShaderReflection reflectSpirvModule(const uint32_t* spirv, const size_t wordCount, uint32_t* outPushConstantSize) {
  ShaderReflection reflection;
  if (outPushConstantSize != nullptr) {
    *outPushConstantSize = 0;
  }
  if (spirv == nullptr || wordCount == 0) {
    return reflection;
  }

  constexpr uint32_t kSpirvMagicNumber = 0x07230203u;
  if (spirv[0] != kSpirvMagicNumber) {
    return reflection;
  }

#if CORAL_BACKEND_HAS_SPIRV_CROSS
  try {
    spirv_cross::CompilerGLSL compiler(spirv, wordCount);
    spirv_cross::ShaderResources resources = compiler.get_shader_resources();

    // No `build_combined_image_samplers()` here: a Vulkan module's descriptor set layout is what the
    // shader itself declares, and merging images with samplers would report a binding the pipeline
    // layout does not have.
    collectBindings(compiler, resources, reflection);

    for (const spirv_cross::Resource& resource: resources.push_constant_buffers) {
      if (outPushConstantSize != nullptr) {
        *outPushConstantSize = static_cast<uint32_t>(compiler.get_declared_struct_size(compiler.get_type(resource.base_type_id)));
      }
      break;
    }
  } catch (...) {
    // A module the translator cannot parse is one the driver will reject too; reporting an empty table
    // keeps the failure at `vkCreateShaderModule`/pipeline creation, where the driver's message is
    // more specific than anything this layer could add.
    reflection.bindings.clear();
    if (outPushConstantSize != nullptr) {
      *outPushConstantSize = 0;
    }
  }
#endif

  return reflection;
}

} // namespace backend
