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

#ifndef CORAL_BACKEND_PIPELINES_H
#define CORAL_BACKEND_PIPELINES_H

#include <backend/Descriptors.h>
#include <backend/Handles.h>
#include <backend/Types.h>

#include <cstdint>

namespace backend {

// ==================================================================================================
// Vertex input
// ==================================================================================================

struct VertexAttribute {
  /** `location` in the shader's vertex input interface. */
  uint32_t shaderLocation = 0;
  /** Index into RenderPipelineDescriptor::vertexBuffers. */
  uint32_t bufferIndex = 0;
  VertexFormat format = VertexFormat::Invalid;
  /** Byte offset of the attribute within the vertex. */
  uint64_t offset = 0;
};

struct VertexBufferLayout {
  /** Distance in bytes between consecutive vertices (or instances). */
  uint64_t stride = 0;
  VertexStepMode stepMode = VertexStepMode::Vertex;
  /** Attributes fed from this buffer, as an index range into the pipeline's attribute array. */
  uint32_t firstAttribute = 0;
  uint32_t attributeCount = 0;
};

struct VertexInputDescriptor {
  VertexBufferLayout buffers[kMaxVertexBufferLayouts] = {};
  uint32_t bufferCount = 0;
  VertexAttribute attributes[kMaxVertexAttributes] = {};
  uint32_t attributeCount = 0;
};

// ==================================================================================================
// Fixed-function state
// ==================================================================================================

struct RasterizerState {
  PrimitiveTopology topology = PrimitiveTopology::TriangleList;
  CullMode cullMode = CullMode::None;
  FrontFace frontFace = FrontFace::CounterClockwise;
  /** Fills polygons as wireframe. Requires Capabilities::fillModeNonSolid. */
  bool wireframe = false;
  /**
   * When true, fragments are discarded instead of written, and the depth/stencil attachment must
   * not also be written. Requires Capabilities::depthClamping.
   */
  bool depthClampEnable = false;
  /** Flip the viewport Y axis, e.g. to match a render target whose origin is top-left. */
  bool flipY = false;
  /** Constant used by BlendFactor::ConstantColor / OneMinusConstantColor. */
  Color blendConstant = Color::black();
  /** Bias added to depth values, for shadow-map acne reduction. */
  float depthBiasConstant = 0.0f;
  float depthBiasSlope = 0.0f;
};

struct StencilFaceState {
  CompareOp compare = CompareOp::Always;
  StencilOp failOp = StencilOp::Keep;
  StencilOp depthFailOp = StencilOp::Keep;
  StencilOp passOp = StencilOp::Keep;
  /** Read/write mask applied to the stencil buffer. */
  uint32_t readMask = 0xFF;
  uint32_t writeMask = 0xFF;
};

struct DepthStencilState {
  /** Disable depth testing and writing entirely. */
  bool depthTestEnable = true;
  bool depthWriteEnable = true;
  CompareOp depthCompare = CompareOp::LessEqual;

  bool stencilEnable = false;
  StencilFaceState front = {};
  StencilFaceState back = {};
  /** Reference value compared against the stencil buffer. */
  uint32_t stencilReference = 0;
};

struct BlendComponent {
  BlendOp operation = BlendOp::Add;
  BlendFactor srcFactor = BlendFactor::One;
  BlendFactor dstFactor = BlendFactor::Zero;
};

struct BlendState {
  bool blendEnable = false;
  BlendComponent color = {};
  BlendComponent alpha = {};

  static BlendState opaque() noexcept {
    BlendState state;
    state.blendEnable = false;
    return state;
  }

  /** Standard non-premultiplied source-over alpha blending. */
  static BlendState alphaBlend() noexcept {
    BlendState state;
    state.blendEnable = true;
    state.color = {
      .operation = BlendOp::Add,
      .srcFactor = BlendFactor::SrcAlpha,
      .dstFactor = BlendFactor::OneMinusSrcAlpha
    };
    state.alpha = {
      .operation = BlendOp::Add,
      .srcFactor = BlendFactor::One,
      .dstFactor = BlendFactor::OneMinusSrcAlpha
    };
    return state;
  }

  /** Premultiplied source-over, which is what most compositors and UI toolkits use. */
  static BlendState premultipliedBlend() noexcept {
    BlendState state;
    state.blendEnable = true;
    state.color = {
      .operation = BlendOp::Add,
      .srcFactor = BlendFactor::One,
      .dstFactor = BlendFactor::OneMinusSrcAlpha
    };
    state.alpha = {
      .operation = BlendOp::Add,
      .srcFactor = BlendFactor::One,
      .dstFactor = BlendFactor::OneMinusSrcAlpha
    };
    return state;
  }

  /** Additive blending, for light accumulation and particle effects. */
  static BlendState additiveBlend() noexcept {
    BlendState state;
    state.blendEnable = true;
    state.color = { .operation = BlendOp::Add, .srcFactor = BlendFactor::SrcAlpha, .dstFactor = BlendFactor::One };
    state.alpha = { .operation = BlendOp::Add, .srcFactor = BlendFactor::Zero, .dstFactor = BlendFactor::One };
    return state;
  }
};

/** Per-attachment colour target configuration. */
struct ColorTargetState {
  PixelFormat format = PixelFormat::Undefined;
  BlendState blend = {};
  /** Bitmask of colour channels that may be written, bit 0 = R. Defaults to all channels. */
  uint8_t writeMask = 0x0F;
};

struct MultisampleState {
  uint32_t sampleCount = 1;
  /** Sample coverage mask; 0 lets the backend choose the default. */
  uint32_t sampleMask = 0;
  bool alphaToCoverageEnable = false;
};

// ==================================================================================================
// Pipeline descriptors
// ==================================================================================================

struct RenderPipelineDescriptor {
  PipelineLayoutHandle layout = PipelineLayoutHandle::invalid();

  ShaderModuleHandle vertexModule = ShaderModuleHandle::invalid();
  ShaderModuleHandle fragmentModule = ShaderModuleHandle::invalid();

  VertexInputDescriptor vertexInput = {};

  /** Primitive assembly cap when topology is PatchList. */
  uint32_t patchControlPoints = 0;

  RasterizerState rasterizer = {};
  DepthStencilState depthStencil = {};
  MultisampleState multisample = {};

  /**
   * Colour attachment formats, in attachment order. These must match the formats of the render pass
   * the pipeline is used with; backends check this at bind time in development builds.
   */
  ColorTargetState colorTargets[kMaxColorAttachments] = {};
  uint32_t colorTargetCount = 0;

  /** Format of the depth/stencil attachment, or Undefined when the pipeline writes no depth. */
  PixelFormat depthStencilFormat = PixelFormat::Undefined;

  /** Render pass layout this pipeline is compatible with; used for validation. */
  RenderPassHandle compatibleRenderPass = RenderPassHandle::invalid();

  const char* label = nullptr;
};

struct ComputePipelineDescriptor {
  PipelineLayoutHandle layout = PipelineLayoutHandle::invalid();
  ShaderModuleHandle computeModule = ShaderModuleHandle::invalid();

  /** Local workgroup size, mirroring the SPIR-V LocalSize execution mode. */
  uint32_t workgroupSizeX = 1;
  uint32_t workgroupSizeY = 1;
  uint32_t workgroupSizeZ = 1;

  const char* label = nullptr;
};

// ==================================================================================================
// Render passes
// ==================================================================================================

/**
 * Describes the shape of a render pass: which attachment slots exist and what formats they carry.
 *
 * The RHI deliberately follows the Vulkan 1.3 / Metal / WebGPU "dynamic rendering" model rather than
 * the older Vulkan 1.0 render-pass-object model. A RenderPass here is a lightweight compatibility
 * *key* - it lets a backend pre-create the driver-side frame buffer object (as OpenGL must) and lets
 * the upper layer validate that a pipeline matches its attachments. The actual load/store
 * operations, clear values and concrete attachment images are supplied at command-recording time.
 *
 * That choice removes one of the biggest sources of boilerplate in Vulkan ports: no VkRenderPass /
 * VkFramebuffer compatibility matrix, and no render-pass permutations when an effect changes its
 * attachment count.
 */
struct RenderPassDescriptor {
  /** Formats of each colour attachment slot. Undefined entries terminate the list. */
  PixelFormat colorFormats[kMaxColorAttachments] = {};
  uint32_t colorAttachmentCount = 0;

  PixelFormat depthStencilFormat = PixelFormat::Undefined;
  uint32_t sampleCount = 1;

  const char* label = nullptr;
};

/**
 * Describes one attachment at command-recording time.
 *
 * `resolveTarget` enables MSAA resolve: when the pass ends the multisampled `view` is resolved into
 * `resolveTarget`. On OpenGL ES this becomes glBlitFramebuffer; on Vulkan 1.3/dynamic rendering it is a
 * resolve attachment, and on the Vulkan 1.0/1.1 fallback it is a `vkCmdResolveImage` after the pass.
 */
struct RenderPassAttachment {
  TextureViewHandle view = TextureViewHandle::invalid();
  TextureViewHandle resolveTarget = TextureViewHandle::invalid();
  LoadOp loadOp = LoadOp::Clear;
  StoreOp storeOp = StoreOp::Store;
  Color clearColor = Color::black();
  DepthStencilValue clearDepthStencil = {};
};

/**
 * Starts a render pass on a command buffer. The attachments are ordinary texture views; the backend
 * binds or creates whatever framebuffer object the API needs.
 */
struct RenderPassBeginInfo {
  RenderPassHandle renderPass = RenderPassHandle::invalid();
  RenderPassAttachment colorAttachments[kMaxColorAttachments] = {};
  uint32_t colorAttachmentCount = 0;
  RenderPassAttachment depthStencilAttachment = {};
  bool hasDepthStencil = false;

  /** Rendering area. When width/height are zero the whole attachment is used. */
  Extent2D renderArea = {};

  const char* label = nullptr;
};

struct FramebufferDescriptor {
  RenderPassHandle renderPass = RenderPassHandle::invalid();
  TextureViewHandle colorAttachments[kMaxColorAttachments] = {};
  uint32_t colorAttachmentCount = 0;
  TextureViewHandle depthStencilAttachment = TextureViewHandle::invalid();

  uint32_t width = 0;
  uint32_t height = 0;

  const char* label = nullptr;
};

} // namespace backend

#endif // CORAL_BACKEND_PIPELINES_H
