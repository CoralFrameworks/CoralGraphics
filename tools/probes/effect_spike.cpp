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

/*
 * The spike the RenderPassChain framework is built on: does the RHI already carry a *caller-supplied*
 * full-screen effect pass, with two samplers and a uniform block, into an offscreen target?
 *
 * ## Why this exists before the framework
 *
 * Every claim the framework makes rests on six things being true of the layer underneath it, and a
 * failure in any one of them would otherwise be debugged through a framework that is itself new:
 *
 *   1. a GLSL fragment shader handed to `Device::createShaderModule` compiles (the SPIR-V path);
 *   2. `getShaderReflection` reports the samplers and the uniform block, so a descriptor set layout can
 *      be built from what the shader says rather than from a table kept in step by hand;
 *   3. two samplers can be sampled in one draw;
 *   4. the uniform block's bytes arrive, so `t` can be a uniform rather than part of the shader;
 *   5. a target rendered by `gpu::Surface` can be sampled by that pass, with the same orientation the
 *      surface drew in -- the round trip a transition is made of;
 *   6. the result can be read back, so a test can assert on pixels rather than on a log line.
 *
 * It deliberately uses `graphics::gpu::Surface` for the scene and for the *device*: one device, one GL
 * context, and `Surface::target()` is the texture the effect samples. A second device would be a second
 * context, and would not answer claim 5 at all.
 *
 * ## What it is not
 *
 * Not a test and not registered with CTest: it prints a line per check and returns non-zero on failure,
 * so it can be run by hand while the framework is being built. It is built only when the tree asks for
 * the developer tools:
 *
 *   cmake -S . -B build -DCORAL_GRAPHICS_BUILD_DEV_TOOLS=ON
 *   cmake --build build --config Debug --target effect_spike
 *   build/Debug/effect_spike.exe
 */

#include <backend/RHI.h>

#include <gpu/Surface.h>

#include <graphics/Canvas.h>
#include <graphics/Color.h>
#include <graphics/Paint.h>

#include <geometry/Rect.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace backend;

constexpr uint32_t kWidth = 32;
constexpr uint32_t kHeight = 16;

/**
 * The framework's parameter block, byte for byte what the shader below declares.
 *
 * Two `vec4`s the framework owns (the source and target extents, which every nontrivial effect needs and
 * none should have to be handed separately) and fourteen the effect owns. 256 bytes is not a coincidence:
 * it is `GpuRenderer`'s uniform slot size, and it is the size this framework's own slot buffer uses.
 */
struct EffectParams {
  float sourceSize[4];
  float targetSize[4];
  float params[14][4];
};
static_assert(sizeof(EffectParams) == 256, "the parameter block is one 256-byte uniform slot");

/**
 * One full-screen quad vertex: clip-space position plus the texture coordinate that goes with it.
 *
 * The same shape `composite.vert` takes, deliberately. The texture coordinate is supplied rather than
 * derived from the position because a target this library rendered into stores the device image with
 * `v = 1` at device row 0 -- see `GpuRenderer::assembleArena`'s `CloseLayer` branch -- and a pass that
 * derived `v` from the clip position would sample its own source upside down.
 */
struct FullTargetVertex {
  float position[2];
  float texCoord[2];
};
constexpr uint32_t kFullTargetVertexStride = sizeof(FullTargetVertex);
constexpr uint32_t kFullTargetVertexCount = 6;

constexpr const char* kVertexSource = R"(#version 300 es
precision highp float;
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;
layout(location = 0) out vec2 vUV;
void main() {
  vUV = aTexCoord;
  gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";

/**
 * The whole of what a transition is: two sources and a `t`.
 *
 * ## Why there is no uniform block here, which is the spike's most expensive lesson
 *
 * The first three drafts of this file declared a `layout(set = 0, binding = 8) uniform EffectParams { ... }`,
 * which is what `shaders/canvas/*.frag` declare and therefore what "the conventions this directory
 * follows" would suggest. It cannot be compiled at run time on a desktop context, and the two failures
 * are worth recording because each looks like a different problem:
 *
 *   * `'binding' : not supported for this version or the enabled extensions` -- with `set`/`binding` on
 *     the declaration, or with `layout(binding = 1)` alone;
 *   * `'binding' : uniform/buffer blocks require layout(binding=X)` -- with no binding qualifier at all.
 *
 * Both come from glslang, and the reason is that `compileGlslToSpirv` compiles with
 * `shaderc_target_env_opengl` (`GlslCompiler.cpp:400`) at a version retargeted for the *platform's*
 * dialect (`getRetargetVersion`, `GlslCompiler.cpp:274-281`). On a desktop context that floor is 410,
 * and at GLSL 410 a uniform block may not carry a `binding` and may not omit one either -- the qualifier
 * needs 420, which the retarget never asks for. The canvas shaders are not counter-evidence: they are
 * compiled *offline* by `tools/shader_compiler`, whose default target dialect is ES, so their retarget
 * lands on `#version 310 es` -- a version where the qualifier *is* accepted.
 *
 * So a shader compiled by this device at run time may not use a uniform block at all, and the framework's
 * parameters travel as a 16x1 `RGBA32F` texture read with `texelFetch` instead. That is not a
 * workaround with a cost: `EffectParams` is an array of `vec4` slots, which is exactly what a row of such
 * a texture is, `texelFetch` needs no filtering and no sampler state, and the upload is one
 * `writeTexture` of 256 bytes -- recorded, so it is ordered with the draws around it exactly as a
 * recorded uniform-buffer write would be
 * (`GpuRenderer::recordUniforms`, `GpuRenderer.cpp:1276-1288`, is the precedent for why ordering matters).
 *
 * ## Why the samplers carry no `layout(binding = ...)` either
 *
 * For the same reason, and this half *does* have a documented way round it: the RHI's
 * `ShaderModuleDescriptor::bindingDeclarations` is "the escape hatch ... a source whose decorations the
 * caller has to restate" (`Descriptors.h:330-337`), applied by the OpenGL backend for both source
 * languages (`GlDevice.cpp:1498-1533`). The shader declares plain `uniform sampler2D`s and this file
 * states where each one lives.
 *
 * The numbers are not free: with no decorations SPIRV-Cross reports **every** resource at
 * `set = 0, binding = 0` (`ShaderCross.cpp:153-159`), so only the first declaration would resolve -- which
 * is why the table below is what makes the second and third samplers bind at all.
 */
constexpr const char* kFragmentSource = R"(#version 300 es
precision highp float;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
uniform sampler2D uInput0;
uniform sampler2D uInput1;
uniform sampler2D uParams;
void main() {
  // The effect, restored after the orientation probe: two sources and a `t` from slot 2.
  vec4 a = texture(uInput0, vUV);
  vec4 b = texture(uInput1, vUV);
  float t = clamp(texelFetch(uParams, ivec2(2, 0), 0).x, 0.0, 1.0);
  oColor = mix(a, b, t);
}
)";

/** The binding input `i` occupies: `kInputBinding + i`. */
constexpr uint32_t kInputBinding = 0;
/** The binding the parameter texture occupies: one past the last input. */
constexpr uint32_t kParamsBinding = 2;
/** Texels in the parameter texture: one per `EffectParams` slot. */
constexpr uint32_t kParamsWidth = 16;

int gFailures = 0;
int gChecks = 0;

void check(const bool condition, const char* what) {
  ++gChecks;
  if (!condition) {
    ++gFailures;
    std::printf("  FAIL  %s\n", what);
  } else {
    std::printf("  ok    %s\n", what);
  }
}

void onMessage(const MessageSeverity severity, const char* message, void* userData) {
  auto* count = static_cast<int*>(userData);
  if (severity == MessageSeverity::Verbose || severity == MessageSeverity::Info) return;
  if (count != nullptr) ++*count;
  std::printf("  backend: %s\n", message != nullptr ? message : "(null message)");
}

/**
 * The six vertices of a quad covering the whole attachment.
 *
 * ## The `v` pairing is measured, not derived, and the measurement is the spike's second lesson
 *
 * The reasoning that looks right -- and that `GpuRenderer::assembleArena`'s composite quad follows --
 * is "a target this library rendered into has its device row 0 at `v = 1`, so pair the clip-space top
 * with `v = 1`". Written that way, this pass **mirrors its input**: an identity pass (output = the
 * source) compared against `Surface::snapshot()` differs in every row, 1024 bytes of a 32x16 picture.
 *
 * What the pictures say instead, measured rather than argued:
 *
 *   * `Surface::snapshot()` is the oracle. It is what `test_Gpu.cpp` compares pixel for pixel against the
 *     CPU rasterizer, so its row 0 is the canvas's row 0 by proof.
 *   * Sampling the source at `v = 0.97` returns the source's red band, and the band is in the snapshot's
 *     rows 0..7. So the source's row 0 of the *snapshot* is at `v` near 1 -- as the derivation said.
 *   * Yet a fragment at the output's row `r` interpolates `vUV.y = (r + 0.5) / height` with the pairing
 *     below reversed, so pairing the clip-space top with `v = 1` puts the source's row 0 at the output's
 *     *bottom*. Reversing the pairing makes the fragment at output row `r` sample `1 - (r + 0.5)/height`,
 *     which is the source's row `r`, and the identity comparison then differs in **zero** rows.
 *
 * So `vUV` is a **texture coordinate** for the framework's targets: `v = 1` is the device's top row. An
 * effect that wants device-space coordinates -- "0 at the top, for a wipe that moves down" -- uses
 * `1.0 - vUV.y`, and the framework's composed shaders hand it both (`vUV` and `vDevice`) so that no
 * author has to remember which one `texture()` wants.
 *
 * The underlying mechanism is not fully explained and is recorded as such in the progress note: both
 * vertex modules are translated with `flip_vert_y` (`ShaderCross.cpp:336`), so the clip-space y of this
 * quad and of `quad.vert` ought to land in the same place, and they measurably do not. What is settled is
 * the *contract*: this pairing reproduces the renderer's own observable orientation, and
 * `tests/test_RenderPassChain.cpp` pins it against `snapshot()` so a change in either half fails loudly.
 */
void fillFullTargetQuad(FullTargetVertex* out) {
  const float left = -1.0f;
  const float right = 1.0f;
  const float top = -1.0f;
  const float bottom = 1.0f;
  const float u0 = 0.0f;
  const float u1 = 1.0f;
  // v = 1 at the device's top row, which is the row `Surface::snapshot` calls row 0.
  const float vTop = 1.0f;
  const float vBottom = 0.0f;
  const FullTargetVertex quad[kFullTargetVertexCount] = {
    { { left, bottom }, { u0, vTop } },
    { { right, bottom }, { u1, vTop } },
    { { right, top }, { u1, vBottom } },
    { { left, bottom }, { u0, vTop } },
    { { right, top }, { u1, vBottom } },
    { { left, top }, { u0, vBottom } },
  };
  std::memcpy(out, quad, sizeof(quad));
}

/** Everything one compiled effect pass owns. Freed by the process exiting; the spike never tears down. */
struct EffectPass {
  ShaderModuleHandle vertexModule = ShaderModuleHandle::invalid();
  ShaderModuleHandle fragmentModule = ShaderModuleHandle::invalid();
  DescriptorSetLayoutHandle setLayout = DescriptorSetLayoutHandle::invalid();
  DescriptorSetHandle set = DescriptorSetHandle::invalid();
  PipelineLayoutHandle pipelineLayout = PipelineLayoutHandle::invalid();
  RenderPipelineHandle pipeline = RenderPipelineHandle::invalid();
  BufferHandle vertices = BufferHandle::invalid();
  TextureHandle params = TextureHandle::invalid();
  TextureSamplerHandle sampler = TextureSamplerHandle::invalid();
};

/** An offscreen colour target the effect can render into and the spike can read back. */
struct Target {
  TextureHandle color = TextureHandle::invalid();
  TextureViewHandle view = TextureViewHandle::invalid();
  RenderPassHandle pass = RenderPassHandle::invalid();
  uint32_t width = 0;
  uint32_t height = 0;
};

Target createTarget(Device& device, const uint32_t width, const uint32_t height, const char* label) {
  Target target;
  target.width = width;
  target.height = height;

  const TextureDescriptor colorDescriptor = TextureDescriptor::texture2D(width, height,
    PixelFormat::R8G8B8A8_UNORM,
    static_cast<TextureUsages>(TextureUsage::ColorAttachment | TextureUsage::Sampled));
  device.createTexture(colorDescriptor, &target.color);

  TextureViewDescriptor viewDescriptor;
  viewDescriptor.texture = target.color;
  viewDescriptor.dimension = TextureViewDimension::Texture2D;
  device.createTextureView(viewDescriptor, &target.view);

  RenderPassDescriptor passDescriptor;
  passDescriptor.colorAttachmentCount = 1;
  passDescriptor.colorFormats[0] = PixelFormat::R8G8B8A8_UNORM;
  passDescriptor.sampleCount = 1;
  passDescriptor.label = label;
  device.createRenderPass(passDescriptor, &target.pass);
  return target;
}

/** A 4x4 texture of one colour, so a sample of it is the colour rather than a guess about filtering. */
TextureHandle createFlatTexture(Device& device, const uint8_t r, const uint8_t g, const uint8_t b,
  const uint8_t a) {
  const TextureDescriptor descriptor = TextureDescriptor::texture2D(4, 4, PixelFormat::R8G8B8A8_UNORM,
    static_cast<TextureUsages>(TextureUsage::Sampled));
  TextureHandle texture = TextureHandle::invalid();
  device.createTexture(descriptor, &texture);

  uint8_t pixels[4 * 4 * 4];
  for (size_t texel = 0; texel < 16; ++texel) {
    pixels[texel * 4 + 0] = r;
    pixels[texel * 4 + 1] = g;
    pixels[texel * 4 + 2] = b;
    pixels[texel * 4 + 3] = a;
  }

  CommandBuffer* commands = nullptr;
  if (!isSuccess(device.createCommandBuffer(CommandBufferDescriptor{}, &commands)) || commands == nullptr) {
    return texture;
  }
  commands->begin();
  TextureCopyRegion region;
  region.textureOrigin = { 0, 0, 0 };
  region.size = { 4, 4, 1 };
  region.mipLevel = 0;
  region.baseArrayLayer = 0;
  region.arrayLayerCount = 1;
  commands->writeTexture(texture, region, pixels, sizeof(pixels));
  commands->end();
  device.executeCommandBuffer(*commands);
  device.destroyCommandBuffer(commands);
  return texture;
}

/**
 * Builds the effect pass: two GLSL stages compiled by the device, a descriptor set layout built from the
 * binding convention the framework composes its shaders with, and a pipeline compatible with
 * @p compatiblePass.
 *
 * @param sourceA the texture input 0 samples, and @p sourceB input 1. The parameters are a 16x1
 *                `RGBA32F` texture this creates and returns in the pass.
 */
EffectPass createEffectPass(Device& device, const RenderPassHandle compatiblePass, const TextureHandle sourceA,
  const TextureHandle sourceB) {
  EffectPass pass;

  ShaderModuleDescriptor vertexDescriptor;
  vertexDescriptor.stage = ShaderStage::Vertex;
  vertexDescriptor.language = ShaderModuleDescriptor::Language::GLSL;
  vertexDescriptor.code = kVertexSource;
  vertexDescriptor.codeSize = std::strlen(kVertexSource) + 1;
  vertexDescriptor.label = "effect_spike/fullscreen.vert";
  Result result = device.createShaderModule(vertexDescriptor, &pass.vertexModule);
  std::printf("  createShaderModule(vertex, GLSL) -> %d\n", static_cast<int>(result));

  ShaderModuleDescriptor fragmentDescriptor;
  fragmentDescriptor.stage = ShaderStage::Fragment;
  fragmentDescriptor.language = ShaderModuleDescriptor::Language::GLSL;
  fragmentDescriptor.code = kFragmentSource;
  fragmentDescriptor.codeSize = std::strlen(kFragmentSource) + 1;
  fragmentDescriptor.label = "effect_spike/fade.frag";

  // The binding table the source cannot carry. `location` is the driver-side slot each binding occupies,
  // which for a sampler is a texture unit.
  const ShaderBindingDeclaration declarations[] = {
    { 0, kInputBinding + 0, BindingType::TextureSampler, 1, "uInput0", 0 },
    { 0, kInputBinding + 1, BindingType::TextureSampler, 1, "uInput1", 1 },
    { 0, kParamsBinding, BindingType::TextureSampler, 1, "uParams", 2 },
  };
  fragmentDescriptor.bindingDeclarations = declarations;
  fragmentDescriptor.bindingDeclarationCount = 3;

  result = device.createShaderModule(fragmentDescriptor, &pass.fragmentModule);
  std::printf("  createShaderModule(fragment, GLSL) -> %d\n", static_cast<int>(result));

  // Printed, and deliberately not what the layout is built from: with no decorations SPIRV-Cross reports
  // every resource at (0, 0), so a layout built from this table would declare one binding three times.
  // The framework composes the shader, so it knows the real answer without asking.
  std::vector<ShaderBindingDeclaration> bindings;
  result = device.getShaderReflection(pass.fragmentModule, &bindings);
  std::printf("  getShaderReflection -> %d, %zu entry/entries:\n", static_cast<int>(result), bindings.size());
  for (const ShaderBindingDeclaration& binding: bindings) {
    std::printf("    set=%u binding=%u type=%d name=%s location=%d\n", binding.set, binding.binding,
      static_cast<int>(binding.type), binding.name != nullptr ? binding.name : "(unnamed)",
      static_cast<int>(binding.location));
  }

  DescriptorSetLayoutDescriptor setLayoutDescriptor;
  setLayoutDescriptor.label = "effect_spike/effect";
  setLayoutDescriptor.bindingCount = 3;
  for (uint32_t index = 0; index < 3; ++index) {
    setLayoutDescriptor.bindings[index] = { kInputBinding + index, BindingType::TextureSampler, 1,
      static_cast<uint8_t>((1u << 0) | (1u << 1)), nullptr };
  }
  result = device.createDescriptorSetLayout(setLayoutDescriptor, &pass.setLayout);
  std::printf("  createDescriptorSetLayout(3 samplers) -> %d\n", static_cast<int>(result));
  result = device.createDescriptorSet(pass.setLayout, &pass.set);
  check(isSuccess(result), "the descriptor set was created");

  PipelineLayoutDescriptor pipelineLayoutDescriptor;
  pipelineLayoutDescriptor.setLayoutCount = 1;
  pipelineLayoutDescriptor.setLayouts[0] = pass.setLayout;
  pipelineLayoutDescriptor.label = "effect_spike/effect";
  result = device.createPipelineLayout(pipelineLayoutDescriptor, &pass.pipelineLayout);

  // The parameters, as a 16x1 float texture: one texel per `EffectParams` slot, read with `texelFetch`.
  // `R32G32B32A32_SFLOAT` because `EffectParams` is a block of `vec4`s and a slot has to survive the round
  // trip exactly -- an 8-bit target would quantise a `t` of 0.004 to zero and a blur's radius to an integer.
  const TextureDescriptor paramsDescriptor = TextureDescriptor::texture2D(kParamsWidth, 1,
    PixelFormat::R32G32B32A32_SFLOAT,
    static_cast<TextureUsages>(TextureUsage::Sampled));
  result = device.createTexture(paramsDescriptor, &pass.params);
  std::printf("  createTexture(params, 16x1 RGBA32F) -> %d\n", static_cast<int>(result));

  result = device.createSampler(SamplerDescriptor::linearClamp(), &pass.sampler);

  const BufferDescriptor vertexDescriptorBuffer { .size = kFullTargetVertexStride * kFullTargetVertexCount,
    .usage = static_cast<BufferUsages>(BufferUsage::Vertex),
    .memoryUsage = MemoryUsage::CpuToGpuDynamic,
    .initialData = nullptr,
    .label = "effect_spike/vertices" };
  result = device.createBuffer(vertexDescriptorBuffer, &pass.vertices);

  RenderPipelineDescriptor pipelineDescriptor;
  pipelineDescriptor.layout = pass.pipelineLayout;
  pipelineDescriptor.vertexModule = pass.vertexModule;
  pipelineDescriptor.fragmentModule = pass.fragmentModule;
  pipelineDescriptor.compatibleRenderPass = compatiblePass;
  pipelineDescriptor.rasterizer.topology = PrimitiveTopology::TriangleList;
  pipelineDescriptor.rasterizer.cullMode = CullMode::None;
  pipelineDescriptor.depthStencil.depthTestEnable = false;
  pipelineDescriptor.depthStencil.depthWriteEnable = false;
  pipelineDescriptor.multisample.sampleCount = 1;
  pipelineDescriptor.colorTargetCount = 1;
  pipelineDescriptor.colorTargets[0].format = PixelFormat::R8G8B8A8_UNORM;
  pipelineDescriptor.colorTargets[0].blend = BlendState::opaque();
  pipelineDescriptor.vertexInput.bufferCount = 1;
  pipelineDescriptor.vertexInput.buffers[0] = { kFullTargetVertexStride, VertexStepMode::Vertex, 0, 2 };
  pipelineDescriptor.vertexInput.attributeCount = 2;
  pipelineDescriptor.vertexInput.attributes[0] = { 0, 0, VertexFormat::Float2, 0 };
  pipelineDescriptor.vertexInput.attributes[1] = { 1, 0, VertexFormat::Float2, 8 };
  pipelineDescriptor.label = "effect_spike/effect";
  result = device.createRenderPipeline(pipelineDescriptor, &pass.pipeline);
  std::printf("  createRenderPipeline -> %d\n", static_cast<int>(result));
  check(isSuccess(result), "the effect pipeline was created");

  // The descriptor writes: the two inputs and the parameter texture, at the bindings the convention names.
  DescriptorWrite writes[3];
  writes[0].binding = kInputBinding + 0;
  writes[0].type = BindingType::TextureSampler;
  writes[0].texture = sourceA;
  writes[0].sampler = pass.sampler;
  writes[1].binding = kInputBinding + 1;
  writes[1].type = BindingType::TextureSampler;
  writes[1].texture = sourceB;
  writes[1].sampler = pass.sampler;
  writes[2].binding = kParamsBinding;
  writes[2].type = BindingType::TextureSampler;
  writes[2].texture = pass.params;
  writes[2].sampler = pass.sampler;
  result = device.updateDescriptorSet(pass.set, writes, 3);
  std::printf("  updateDescriptorSet(2 inputs + params) -> %d\n", static_cast<int>(result));
  check(isSuccess(result), "the effect's two inputs and its parameter texture were bound");

  return pass;
}

/** Reads @p target's pixels as top-left-origin RGBA8 rows, exactly as `GpuRenderer::readback` does. */
bool readTarget(Device& device, const Target& target, std::vector<uint8_t>* out) {
  const uint64_t rowBytes = static_cast<uint64_t>(target.width) * 4u;
  const uint64_t size = rowBytes * target.height;

  const BufferDescriptor descriptor { .size = size,
    .usage = static_cast<BufferUsages>(BufferUsage::TransferDst),
    .memoryUsage = MemoryUsage::GpuToCpu,
    .initialData = nullptr,
    .label = "effect_spike/readback" };
  BufferHandle buffer = BufferHandle::invalid();
  if (!isSuccess(device.createBuffer(descriptor, &buffer))) return false;

  CommandBuffer* commands = nullptr;
  if (!isSuccess(device.createCommandBuffer(CommandBufferDescriptor{}, &commands)) || commands == nullptr) {
    return false;
  }
  commands->begin();

  // `copyTextureToBuffer` on the OpenGL backend is `glReadPixels`, which reads whichever framebuffer is
  // bound rather than the texture the copy names. Opening an empty `Load` pass on the target is what
  // makes *its* framebuffer current.
  RenderPassBeginInfo info;
  info.renderPass = target.pass;
  info.colorAttachmentCount = 1;
  info.colorAttachments[0].view = target.view;
  info.colorAttachments[0].loadOp = LoadOp::Load;
  info.colorAttachments[0].storeOp = StoreOp::Store;
  info.renderArea = { target.width, target.height };
  commands->beginRenderPass(info);
  commands->endRenderPass();

  BufferTextureCopy copy;
  copy.buffer = buffer;
  copy.texture = target.color;
  copy.bufferRegion.dstOffset = 0;
  copy.bufferRegion.size = size;
  copy.textureRegion.textureOrigin = { 0, 0, 0 };
  copy.textureRegion.size = { target.width, target.height, 1 };
  copy.textureRegion.mipLevel = 0;
  copy.textureRegion.baseArrayLayer = 0;
  copy.textureRegion.arrayLayerCount = 1;
  commands->copyTextureToBuffer(copy);
  commands->end();

  const bool executed = isSuccess(device.executeCommandBuffer(*commands));
  device.destroyCommandBuffer(commands);
  if (!executed) return false;
  device.waitIdle();

  void* mapped = nullptr;
  if (!isSuccess(device.mapBuffer(buffer, 0, size, &mapped)) || mapped == nullptr) return false;

  out->resize(static_cast<size_t>(size));
  const auto* source = static_cast<const uint8_t*>(mapped);
  for (uint32_t row = 0; row < target.height; ++row) {
    // Rows are reversed on the way out: `glReadPixels` numbers them from the framebuffer's bottom left,
    // and everything above this layer numbers them from the top left.
    const uint8_t* sourceRow = source + static_cast<size_t>(target.height - 1 - row) * rowBytes;
    std::memcpy(out->data() + static_cast<size_t>(row) * rowBytes, sourceRow, static_cast<size_t>(rowBytes));
  }
  device.unmapBuffer(buffer);
  device.destroyBuffer(buffer);
  return true;
}

const uint8_t* pixelAt(const std::vector<uint8_t>& pixels, const uint32_t width, const uint32_t x,
  const uint32_t y) {
  return pixels.data() + (static_cast<size_t>(y) * width + x) * 4u;
}

} // namespace anonymous

int main() {
  std::printf("effect_spike: does the RHI carry a caller-supplied multi-sampler effect pass?\n");

  int backendMessages = 0;

  // The surface is the device owner *and* the scene renderer: its target is one of the effect's inputs,
  // which is the round trip a transition is actually made of.
  graphics::gpu::SurfaceConfiguration configuration;
  configuration.width = kWidth;
  configuration.height = kHeight;
  configuration.diagnosticsCallback = &onMessage;
  configuration.diagnosticsUserData = &backendMessages;
  const std::unique_ptr<graphics::gpu::Surface> surface = graphics::gpu::Surface::make(configuration);
  if (surface == nullptr) {
    std::printf("no GPU device could be brought up; nothing to spike\n");
    return 0;
  }
  std::printf("device: %s\n", surface->describeDevice().c_str());

  Device& device = *surface->device();

  // --- the scene: a red band across the device *top* half, which is what makes the orientation check real
  graphics::Canvas* canvas = surface->beginFrame();
  if (canvas == nullptr) {
    std::printf("the surface refused a frame\n");
    return 1;
  }
  graphics::Paint paint;
  paint.color = graphics::Color::Red();
  canvas->drawRect(graphics::Rect::MakeLTRB(0.0f, 0.0f, static_cast<float>(kWidth),
                   static_cast<float>(kHeight) / 2.0f), paint);
  if (!surface->submit()) {
    std::printf("the surface refused the frame\n");
    return 1;
  }
  graphics::Bitmap reference = surface->snapshot();
  graphics::Pixmap referencePixels = reference.lockPixels();

  // --- the effect's other input: a flat blue texture, so `mix` has two known ends
  const TextureHandle blue = createFlatTexture(device, 0, 0, 255, 255);

  // --- what the surface's target actually holds, read through the same path the effect's output is
  // read through. This is the measurement that decides the orientation question rather than an argument
  // about it: "which device row is the red band in" has to be answered before "did the pass keep it
  // there" can mean anything.
  {
    TextureViewHandle surfaceView = TextureViewHandle::invalid();
    TextureViewDescriptor viewDescriptor;
    viewDescriptor.texture = surface->target();
    viewDescriptor.dimension = TextureViewDimension::Texture2D;
    device.createTextureView(viewDescriptor, &surfaceView);
    RenderPassDescriptor surfacePassDescriptor;
    surfacePassDescriptor.colorAttachmentCount = 1;
    surfacePassDescriptor.colorFormats[0] = PixelFormat::R8G8B8A8_UNORM;
    surfacePassDescriptor.sampleCount = 1;
    RenderPassHandle surfacePass = RenderPassHandle::invalid();
    device.createRenderPass(surfacePassDescriptor, &surfacePass);

    Target surfaceTarget;
    surfaceTarget.color = surface->target();
    surfaceTarget.view = surfaceView;
    surfaceTarget.pass = surfacePass;
    surfaceTarget.width = kWidth;
    surfaceTarget.height = kHeight;

    std::vector<uint8_t> sourcePixels;
    if (readTarget(device, surfaceTarget, &sourcePixels)) {
      std::printf("  the surface's own target, as this readback sees it:\n");
      for (uint32_t row: { 0u, 3u, 7u, 8u, 12u, 15u }) {
        const uint8_t* pixel = pixelAt(sourcePixels, kWidth, kWidth / 2, row);
        std::printf("    row %2u rgba=(%3u,%3u,%3u,%3u)\n", row, pixel[0], pixel[1], pixel[2], pixel[3]);
      }
    } else {
      std::printf("  the surface's target could not be read back\n");
    }
  }

  Target output = createTarget(device, kWidth, kHeight, "effect_spike/output");
  EffectPass pass = createEffectPass(device, output.pass, surface->target(), blue);

  FullTargetVertex quad[kFullTargetVertexCount];
  fillFullTargetQuad(quad);

  CommandBuffer* commands = nullptr;
  if (!isSuccess(device.createCommandBuffer(CommandBufferDescriptor{}, &commands)) || commands == nullptr) {
    std::printf("no command buffer\n");
    return 1;
  }

  const auto writeParams = [&](const float t) {
    EffectParams params {};
    params.sourceSize[0] = static_cast<float>(kWidth);
    params.sourceSize[1] = static_cast<float>(kHeight);
    params.sourceSize[2] = 1.0f / static_cast<float>(kWidth);
    params.sourceSize[3] = 1.0f / static_cast<float>(kHeight);
    params.targetSize[0] = static_cast<float>(kWidth);
    params.targetSize[1] = static_cast<float>(kHeight);
    params.params[0][0] = t;
    // Recorded, not mapped: the GPU sees this write before the draw that reads it, which is what makes one
    // parameter texture correct for a whole chain (`GpuRenderer.cpp:1280-1286`).
    TextureCopyRegion region;
    region.textureOrigin = { 0, 0, 0 };
    region.size = { kParamsWidth, 1, 1 };
    region.mipLevel = 0;
    region.baseArrayLayer = 0;
    region.arrayLayerCount = 1;
    commands->writeTexture(pass.params, region, &params, sizeof(params));
  };

  // --- case 1: surface.target as source A, the flat blue as source B, sampled at three values of t
  struct Case {
    float t;
    const char* what;
  };
  const Case cases[] = {
    { 0.0f, "t=0 shows the source and none of the destination" },
    { 0.25f, "t=0.25 shows the source three quarters" },
    { 1.0f, "t=1 shows the destination and none of the source" },
  };

  bool allRead = true;
  for (const Case& test: cases) {
    commands->reset();
    commands->begin();
    commands->writeBuffer(pass.vertices, 0, quad, sizeof(quad));
    writeParams(test.t);

    RenderPassBeginInfo info;
    info.renderPass = output.pass;
    info.colorAttachmentCount = 1;
    info.colorAttachments[0].view = output.view;
    info.colorAttachments[0].loadOp = LoadOp::Clear;
    info.colorAttachments[0].storeOp = StoreOp::Store;
    info.colorAttachments[0].clearColor = Color::transparentBlack();
    info.renderArea = { output.width, output.height };
    commands->beginRenderPass(info);
    commands->bindRenderPipeline(pass.pipeline);
    commands->bindDescriptorSet(0, pass.set);
    commands->setVertexBuffer(0, pass.vertices, 0);
    commands->draw(kFullTargetVertexCount, 1, 0, 0);
    commands->endRenderPass();
    commands->end();
    if (!isSuccess(device.executeCommandBuffer(*commands))) {
      std::printf("  the effect pass was refused at t=%g\n", static_cast<double>(test.t));
      allRead = false;
      continue;
    }

    std::vector<uint8_t> pixels;
    if (!readTarget(device, output, &pixels)) {
      std::printf("  readback failed at t=%g\n", static_cast<double>(test.t));
      allRead = false;
      continue;
    }

    // Device row 1 is inside the red band; device row kHeight-1 is outside it.
    const uint8_t* inside = pixelAt(pixels, output.width, kWidth / 2, 1);
    const uint8_t* outside = pixelAt(pixels, output.width, kWidth / 2, kHeight - 1);
    std::printf("  probe: out row 1 (r=%3u g=%3u)  row %u (r=%3u g=%3u)\n", inside[0], inside[1],
      kHeight - 1, outside[0], outside[1]);

    // The oracle: `Surface::snapshot()` reads the surface's own target through the renderer's readback,
    // which `test_Gpu.cpp` compares pixel for pixel against the CPU rasterizer -- so its row 0 is the
    // canvas's row 0 by proof rather than by argument. An identity pass (t = 0, output = the source) must
    // reproduce it exactly, and that is the claim this comparison pins.
    if (test.t == 0.0f && !reference.isEmpty()) {
      const auto* referenceBytes = static_cast<const uint8_t*>(referencePixels.pixels());
      uint32_t differingRows = 0;
      uint32_t firstDifferingRow = UINT32_MAX;
      uint32_t differingBytes = 0;
      for (uint32_t row = 0; row < kHeight; ++row) {
        const uint8_t* a = pixels.data() + static_cast<size_t>(row) * output.width * 4u;
        const uint8_t* b = referenceBytes + static_cast<size_t>(row) * reference.rowBytes();
        if (std::memcmp(a, b, static_cast<size_t>(output.width) * 4u) != 0) {
          ++differingRows;
          if (firstDifferingRow == UINT32_MAX) firstDifferingRow = row;
          for (size_t byte = 0; byte < static_cast<size_t>(output.width) * 4u; ++byte) {
            if (a[byte] != b[byte]) ++differingBytes;
          }
        }
      }
      const uint8_t* referenceTop = referenceBytes;
      const uint8_t* referenceBottom = referenceBytes + static_cast<size_t>(kHeight - 1) * reference.rowBytes();
      std::printf("  snapshot   row 0 rgba=(%3u,%3u,%3u,%3u)  row %u rgba=(%3u,%3u,%3u,%3u)\n",
        referenceTop[0], referenceTop[1], referenceTop[2], referenceTop[3], kHeight - 1, referenceBottom[0],
        referenceBottom[1], referenceBottom[2], referenceBottom[3]);
      std::printf("  identity pass vs snapshot: %u differing row(s), first at %u, %u differing byte(s)\n",
        differingRows, firstDifferingRow, differingBytes);
      check(differingRows == 0, "an identity effect pass reproduces the surface's own snapshot");
    }

    const double alpha = static_cast<double>(test.t);
    const double expectedRedInside = 255.0 * (1.0 - alpha) + 0.0 * alpha;
    const double expectedBlueInside = 0.0 * (1.0 - alpha) + 255.0 * alpha;
    const double expectedAlphaInside = 255.0;
    const double expectedAlphaOutside = 255.0 * alpha;

    std::printf("  t=%-5g inside  rgba=(%3u,%3u,%3u,%3u) outside rgba=(%3u,%3u,%3u,%3u)\n",
      static_cast<double>(test.t), inside[0], inside[1], inside[2], inside[3], outside[0], outside[1],
      outside[2], outside[3]);

    char label[160];
    std::snprintf(label, sizeof(label), "%s: alpha at the top half is the mix of red and blue",
      test.what);
    check(std::abs(static_cast<double>(inside[0]) - expectedRedInside) <= 3.0 &&
            std::abs(static_cast<double>(inside[2]) - expectedBlueInside) <= 3.0 &&
            std::abs(static_cast<double>(inside[3]) - expectedAlphaInside) <= 3.0,
      label);

    // The blue texture is opaque everywhere, so the row below the red band is blue at every t > 0 and
    // transparent at t = 0 -- which is what says the surface's target was sampled with the device's own
    // orientation: a vertically flipped sample would put the red band at the *bottom*.
    std::snprintf(label, sizeof(label), "%s: the row outside the band follows the blue source", test.what);
    check(std::abs(static_cast<double>(outside[3]) - expectedAlphaOutside) <= 3.0, label);

    const bool bandIsAtTheTop = test.t < 1.0f ? (inside[0] > outside[0]) : true;
    std::snprintf(label, sizeof(label), "%s: the surface's red band is sampled at the top, not flipped",
      test.what);
    check(bandIsAtTheTop, label);
  }

  check(allRead, "every case produced a readable picture");
  device.destroyCommandBuffer(commands);
  std::printf("effect_spike: %d check(s), %d failure(s), %d backend message(s)\n", gChecks, gFailures,
    backendMessages);
  return gFailures == 0 ? 0 : 1;
}
