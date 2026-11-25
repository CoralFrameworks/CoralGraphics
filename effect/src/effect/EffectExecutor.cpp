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
 * @file EffectExecutor.cpp
 *
 * One frame of the v1 executor: produce each input, apply each pass's snapshot, draw it into a pooled
 * frame, and hand the last frame to the output. The traversal order comes from `EngineImpl::buildPlan`;
 * this file assumes every node has exactly one consumer and at most one producer per port.
 */

#include "EffectExecutor.h"
#include <backend/ShaderCompiler.h>
#include <backend/TestHooks.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <cmath>
#include <cstring>
#include <utility>


namespace coral::detail {

namespace {

EffectPixelFormat fromPixelFormat(const backend::PixelFormat format) {
  switch (format) {
    case backend::PixelFormat::R8G8B8A8_UNORM:
      return EffectPixelFormat::RGBA8;
    case backend::PixelFormat::B8G8R8A8_UNORM:
      return EffectPixelFormat::BGRA8;
    case backend::PixelFormat::R16G16B16A16_SFLOAT:
      return EffectPixelFormat::RGBA16F;
    default:
      return EffectPixelFormat::RGBA8;
  }
}

const char* kindName(const EffectNodeKind kind) noexcept {
  switch (kind) {
    case EffectNodeKind::Input:
      return "input";
    case EffectNodeKind::Pass:
      return "pass";
    case EffectNodeKind::Output:
      return "output";
  }
  return "node";
}

/** @return the frame index a sequence should show at @p now, or false when it shows nothing yet. */
bool selectSequenceFrame(const SequenceState& sequence, const uint32_t timeBase, const RenderTime& nowTime,
                         uint32_t& index) {
  if (sequence.desc.frames.empty() || timeBase == 0) return false;
  const uint32_t count = static_cast<uint32_t>(sequence.desc.frames.size());

  if (!sequence.timeDriven) {
    index = std::min(sequence.frameIndex, count - 1);
    return true;
  }

  const int64_t duration = sequence.desc.frameDuration.toTicks(timeBase);
  if (duration <= 0) return false;
  const int64_t start = sequence.desc.startTime.toTicks(timeBase);
  const int64_t now = sequence.explicitTime ? sequence.time.toTicks(timeBase) : nowTime.toTicks(timeBase);
  if (now < start) return false;

  const int64_t step = (now - start) / duration;
  if (step < 0) return false;
  if (static_cast<uint64_t>(step) >= count) {
    if (!sequence.desc.loop) {
      index = count - 1;
      return true;
    }
    index = static_cast<uint32_t>(static_cast<uint64_t>(step) % count);
    return true;
  }
  index = static_cast<uint32_t>(step);
  return true;
}

} // namespace anonymous

// ==================================================================================================
// PassRuntime
// ==================================================================================================

void PassRuntime::adoptNextProgram() {
  if (nextProgram == nullptr) return;
  applied = nextApplied;
  program = nextProgram;
  values = nextValues;
  appliedVersion = nextVersion;
  state = EffectShaderState::Ready;
  diagnostics.clear();
  nextApplied.reset();
  nextProgram = nullptr;
  nextPipeline.reset();
  nextPipelineStarted = false;
}

void PassRuntime::apply(EffectNode& node, const NodeSnapshot& snapshot, RenderCore& core,
                        const FrameContext& /*context*/) {
  if (nextPipeline != nullptr && nextPipeline->ready.load(std::memory_order_acquire)) {
    if (nextPipeline->failed.load(std::memory_order_acquire)) {
      diagnostics = "the pipeline for the new shader could not be built";
      nextApplied.reset();
      nextProgram = nullptr;
      nextPipeline.reset();
      nextPipelineStarted = false;
    } else {
      adoptNextProgram();
    }
  }
  // An async compile only publishes a program when it has finished; until then the previous program
  // keeps drawing. `wait_for(0)` is a poll, never a wait on the render thread.
  if (compileFuture != nullptr) {
    if (compileFuture->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      AsyncCompileResult result = compileFuture->get();
      const std::shared_ptr<const ProgramSnapshot> source = compiling;
      compileFuture.reset();
      compiling.reset();
      const bool stale = source != nullptr && snapshot.pendingProgram != nullptr &&
                         snapshot.pendingProgram != source;
      if (stale) {
        // A newer shader arrived while this one compiled. Drop the result rather than briefly
        // publishing a shader the application has already superseded.
      } else if (result.success() && source != nullptr) {
        auto prepared = std::make_shared<ProgramSnapshot>(*source);
        prepared->desc.authoring = EffectShaderAuthoring::SpirvModule;
        prepared->desc.vertexSpirv = std::move(result.vertexSpirv);
        prepared->desc.fragmentSpirv = std::move(result.fragmentSpirv);
        prepared->hash = hashProgram(prepared->desc, prepared->parameters, prepared->textureNames,
                                      prepared->externalInputMask);
        std::string error;
        RenderProgram* compiled = core.programFor(prepared, error);
        if (compiled != nullptr) {
          const ParameterBlock pendingValues =
              snapshot.pendingProgram == source ? snapshot.pendingValues : snapshot.values;
          if (program != nullptr && core.asyncPipelineCreation()) {
            nextApplied = prepared;
            nextProgram = compiled;
            nextValues = pendingValues;
            nextVersion = snapshot.version;
            nextPipeline = std::make_shared<PendingPipelineState>();
            // A fresh pending state has had no pipeline request made for it yet. Leaving the flag set from the
            // previous one meant that a second shader finishing its compile before the first pipeline was
            // adopted never got a request at all, and so was never adopted either. That stayed invisible while
            // compiles were slow, because the first pipeline was adopted - which clears the flag - long before
            // the second compile finished; faster compiles put both in the same window and exposed it.
            nextPipelineStarted = false;
            NodeAccess::promote(node, source);
          } else {
            applied = prepared;
            program = compiled;
            values = pendingValues;
            appliedVersion = snapshot.version;
            state = EffectShaderState::Ready;
            diagnostics.clear();
            NodeAccess::promote(node, source);
          }
        } else {
          rejected = source;
          state = EffectShaderState::Failed;
          diagnostics = error.empty() ? "the shader could not be compiled" : error;
          NodeAccess::reject(node, source);
          NodeAccess::setDiagnostics(node, diagnostics);
        }
      } else if (source != nullptr) {
        rejected = source;
        state = EffectShaderState::Failed;
        diagnostics = result.error.empty() ? "the shader could not be compiled" : result.error;
        NodeAccess::reject(node, source);
        NodeAccess::setDiagnostics(node, diagnostics);
      }
    }
  }

  // A new pending description is compiled once. A failed one is remembered so a render loop does not
  // recompile the same broken shader every frame; the previous program keeps drawing meanwhile.
  const std::shared_ptr<const ProgramSnapshot> pending = snapshot.pendingProgram;
  if (pending != nullptr && pending != applied && pending != rejected && pending != compiling) {
    bool startedAsync = false;
    bool compositionFailed = false;
    if (core.canCompileAsync() && program != nullptr) {
      ComposedProgramSources sources;
      std::string compositionError;
      if (!composeProgramSources(*pending, false, false, core.topDownTargets(), sources, compositionError)) {
        rejected = pending;
        state = EffectShaderState::Failed;
        diagnostics = std::move(compositionError);
        NodeAccess::reject(node, pending);
        NodeAccess::setDiagnostics(node, diagnostics);
        compositionFailed = true;
      } else {
        const bool compileVertex = pending->desc.vertexSpirv.empty();
        const std::vector<uint32_t> preparedVertex = pending->desc.vertexSpirv;
        auto promise = std::make_shared<std::promise<AsyncCompileResult>>();
        auto task = [sources = std::move(sources), compileVertex, preparedVertex, promise]() {
          AsyncCompileResult result;
          try {
            if (compileVertex) {
              backend::ShaderCompileResult vertex;
              if (!backend::isSuccess(
                      backend::compileShaderToSpirv(sources.vertexSource, backend::ShaderStage::Vertex, vertex))) {
                result.error = vertex.error.empty() ? "vertex shader compilation failed" : vertex.error;
              } else {
                result.vertexSpirv = std::move(vertex.spirv);
              }
            } else {
              result.vertexSpirv = preparedVertex;
            }
            if (result.error.empty()) {
              backend::ShaderCompileResult fragment;
              if (!backend::isSuccess(
                      backend::compileShaderToSpirv(sources.fragmentSource, backend::ShaderStage::Fragment, fragment))) {
                result.error = fragment.error.empty() ? "fragment shader compilation failed" : fragment.error;
              } else {
                result.fragmentSpirv = std::move(fragment.spirv);
              }
            }
          } catch (const std::exception& exception) {
            result.error = exception.what();
          } catch (...) {
            result.error = "shader compilation failed";
          }
          promise->set_value(std::move(result));
        };
        if (core.submitShaderCompile(std::move(task))) {
          compiling = pending;
          compileFuture = std::make_shared<std::future<AsyncCompileResult>>(promise->get_future());
          startedAsync = true;
        }
      }
    }
    if (!startedAsync && !compositionFailed) {
      std::string error;
      RenderProgram* compiled = core.programFor(pending, error);
      if (compiled != nullptr) {
        const ParameterBlock pendingValues = snapshot.pendingValues;
        applied = pending;
        program = compiled;
        values = pendingValues;
        appliedVersion = snapshot.version + 1;
        state = EffectShaderState::Ready;
        diagnostics.clear();
        NodeAccess::promote(node, pending);
      } else {
        rejected = pending;
        state = EffectShaderState::Failed;
        diagnostics = error.empty() ? "the shader could not be compiled" : error;
        NodeAccess::reject(node, pending);
        NodeAccess::setDiagnostics(node, diagnostics);
      }
    }
  }

  // A node whose program was installed without a pending step (a re-attach) still needs it resolved.
  if (applied == nullptr && snapshot.program != nullptr && snapshot.program != rejected) {
    std::string error;
    RenderProgram* compiled = core.programFor(snapshot.program, error);
    if (compiled != nullptr) {
      applied = snapshot.program;
      program = compiled;
      values = snapshot.values;
      appliedVersion = snapshot.version;
      state = EffectShaderState::Ready;
      diagnostics.clear();
      NodeAccess::setShaderState(node, EffectShaderState::Ready);
    } else {
      rejected = snapshot.program;
      state = EffectShaderState::Failed;
      diagnostics = error.empty() ? "the shader could not be compiled" : error;
      NodeAccess::reject(node, snapshot.program);
      NodeAccess::setDiagnostics(node, diagnostics);
    }
  }

  textures = snapshot.textures;
  sequences = snapshot.sequences;
  if (applied != nullptr && appliedVersion != snapshot.version && snapshot.pendingProgram == nullptr) {
    // No shader change, just parameter values: copying the 256-byte block is the whole update.
    values = snapshot.values;
    appliedVersion = snapshot.version;
  }
}

// ==================================================================================================
// GraphExecutor
// ==================================================================================================

GraphExecutor::GraphExecutor(RenderCore& core) : mCore(core) {}

GraphExecutor::~GraphExecutor() = default;

PassRuntime& GraphExecutor::passRuntime(const NodeId id) {
  NodeState& state = mNodes[id];
  if (state.pass == nullptr) state.pass = std::make_unique<PassRuntime>();
  return *state.pass;
}

void GraphExecutor::prune(const std::unordered_set<NodeId>& live) {
  for (auto it = mNodes.begin(); it != mNodes.end();) {
    if (live.count(it->first) == 0) {
      it = mNodes.erase(it);
    } else {
      ++it;
    }
  }
}

void GraphExecutor::releaseResources() {
  mNodes.clear();
}

bool GraphExecutor::resolveTexture(RenderCore& core, const TextureSource& source,
                                   const std::unordered_map<uint64_t, backend::TextureHandle>& frameTextures,
                                   backend::TextureHandle& out, std::string& error) const {
  switch (source.kind()) {
    case TextureSource::Kind::Empty:
      // A transparent one-texel image rather than an unbound sampler: a pass that samples an empty
      // binding reads defined pixels, which is a picture that can be diagnosed instead of a driver event.
      out = core.textureForSolid(EffectColor { 0.0f, 0.0f, 0.0f, 0.0f }, error);
      return out.valid();
    case TextureSource::Kind::Image:
      out = core.textureForImage(TextureSourceAccess::image(source), error);
      return out.valid();
    case TextureSource::Kind::Solid:
      out = core.textureForSolid(TextureSourceAccess::color(source), error);
      return out.valid();
    case TextureSource::Kind::Frame: {
      const EffectFrame frame = TextureSourceAccess::frame(source);
      const auto it = frameTextures.find(frame.id());
      if (it == frameTextures.end()) {
        error = "a texture source names a frame that is not in this frame's chain";
        return false;
      }
      out = it->second;
      return true;
    }
    case TextureSource::Kind::Native: {
      const uint64_t token = TextureSourceAccess::nativeTexture(source);
      const backend::TextureHandle handle(
        static_cast<uint32_t>(token & backend::TextureHandle::kIndexMask),
        static_cast<uint32_t>(token >> backend::TextureHandle::kIndexBits));
      if (!handle.valid()) {
        error = "a native texture source has no valid handle";
        return false;
      }
      out = handle;
      return true;
    }
    case TextureSource::Kind::External: {
      const std::shared_ptr<EffectExternalTexture> texture = TextureSourceAccess::external(source);
      if (texture == nullptr) {
        error = "an external texture source has no texture";
        return false;
      }
      out = mCore.resolveExternalTexture(*texture, error);
      return out.valid();
    }
  }
  return false;
}

EffectResult GraphExecutor::execute(const GraphPlan& plan, const FrameContext& context, std::string& error,
                                   std::shared_ptr<FrameCompletion> completion) {
  if (plan.output == kInvalidNodeId) {
    error = "the graph has no output";
    return EffectResult::InvalidArgument;
  }

  const auto outputNodeIt = plan.nodes.find(plan.output);
  if (outputNodeIt == plan.nodes.end()) {
    error = "the output node is missing from the plan";
    return EffectResult::InvalidArgument;
  }

  backend::CommandBuffer* commands = mCore.beginRecording(true);
  if (commands == nullptr) {
    error = "no command buffer could be acquired for the frame";
    return EffectResult::Busy;
  }

  std::unordered_map<NodeId, FrameView> outputs;
  std::unordered_map<uint64_t, backend::TextureHandle> frameTextures;
  std::vector<FrameLease> leases;
  std::vector<std::shared_ptr<InputRuntime>> inputRuntimes;
  std::vector<std::shared_ptr<OutputRuntime>> outputRuntimes;
  outputs.reserve(plan.order.size());
  leases.reserve(plan.order.size());

  FrameContext frameContext = context;
  frameContext.commands = commands;
  frameContext.pool = &mCore.framePool();

  std::shared_ptr<OutputRuntime> directOutput;
  NodeId directOutputId = kInvalidNodeId;
  NodeId directPassId = kInvalidNodeId;
  backend::SwapchainTargetInfo directTargetInfo {};
  bool directActive = false;
  if (plan.outputs.size() == 1) {
    directOutputId = plan.outputs.front();
    const auto directNodeIt = plan.nodes.find(directOutputId);
    if (directNodeIt != plan.nodes.end() && directNodeIt->second != nullptr) {
      directOutput = NodeAccess::outputRuntime(*directNodeIt->second);
      for (const Edge& edge : plan.edges) {
        if (edge.to != directOutputId || edge.inputIndex != 0) continue;
        const auto passIt = plan.nodes.find(edge.from);
        if (passIt != plan.nodes.end() && passIt->second != nullptr &&
            passIt->second->kind() == EffectNodeKind::Pass) {
          directPassId = edge.from;
        }
        break;
      }
    }
    if (directOutput != nullptr && directPassId != kInvalidNodeId &&
        directOutput->supportsDirectSurface()) {
      const auto passIt = plan.nodes.find(directPassId);
      auto* pass = static_cast<EffectPassNode*>(passIt->second.get());
      const NodeSnapshot passSnapshot = NodeAccess::snapshot(*pass);
      EffectRotation rotation = EffectRotation::None;
      bool mirrorH = false;
      bool mirrorV = false;
      uint32_t targetW = 0;
      uint32_t targetH = 0;
      const bool hasTransform = directOutput->targetTransform(rotation, mirrorH, mirrorV);
      const bool hasTarget = directOutput->targetSize(context.renderWidth, context.renderHeight, targetW, targetH);
      if (pass->directToSurface() && passSnapshot.outputScale == 1.0f &&
          passSnapshot.outputFormat == directOutput->targetFormat() && !hasTransform && !hasTarget) {
        std::string directError;
        if (directOutput->beginDirectFrame(frameContext, context.renderWidth, context.renderHeight, directError) &&
            backend::isSuccess(directOutput->acquireDirectTarget(directTargetInfo))) {
          directActive = true;
          mDirectFrames.fetch_add(1, std::memory_order_acq_rel);
        } else {
          directOutput->presentDirectTarget();
        }
      }
    }
  }
  const auto fail = [&](const EffectResult result, const std::string& message) {
    // An output that recorded a copy against this frame must discard it: there will be no submission to
    // pair it with, and the next frame must not deliver the aborted frame's pixels. An input that rented a
    // pooled frame releases it for the same reason.
    for (const std::shared_ptr<InputRuntime>& runtime : inputRuntimes) runtime->afterAbort();
    for (const std::shared_ptr<OutputRuntime>& runtime : outputRuntimes) runtime->afterAbort();
    if (directActive && directOutput != nullptr) {
      directOutput->presentDirectTarget();
      directActive = false;
    }
    mCore.abortRecording();
    error = message;
    return result;
  };

  for (const NodeId id : plan.order) {
    const auto nodeIt = plan.nodes.find(id);
    if (nodeIt == plan.nodes.end() || nodeIt->second == nullptr) {
      return fail(EffectResult::InvalidArgument, "the plan names a node that is not in the graph");
    }
    EffectNode& node = *nodeIt->second;
    const NodeSnapshot snapshot = NodeAccess::snapshot(node);
    NodeState& state = mNodes[id];

    if (node.kind() == EffectNodeKind::Input) {
      if (!snapshot.enabled) {
        return fail(EffectResult::Busy, "the input node '" + snapshot.name + "' is disabled");
      }
      const std::shared_ptr<InputRuntime> runtime = NodeAccess::inputRuntime(node);
      if (runtime == nullptr) {
        return fail(EffectResult::Unsupported,
                    "the input node '" + snapshot.name + "' has no runtime; a platform target provides one");
      }
      runtime->setBackpressure(mCore.backpressureMode());
      inputRuntimes.push_back(runtime);
      FrameView view;
      EffectFrame metadata;
      std::string produceError;
      if (!runtime->produce(frameContext, view, metadata, produceError)) {
        if (!produceError.empty()) return fail(EffectResult::Unsupported, produceError);
        if (state.lastView.isValid() && static_cast<EffectInputNode&>(node).repeatLastFrame()) {
          view = state.lastView;
          metadata = state.lastMetadata;
        } else {
          // Naming the node matters: the usual cause is an input that was preloaded but never produced a frame,
          // and a bare "the input node has no frame" sends the reader looking for a device problem instead.
          return fail(EffectResult::Busy, "the input node '" + snapshot.name + "' has no frame");
        }
      }
      state.lastView = view;
      state.lastMetadata = metadata;
      outputs[id] = view;
      if (metadata.isValid() && view.texture.valid()) frameTextures[metadata.id()] = view.texture;
      continue;
    }

    if (node.kind() == EffectNodeKind::Output) {
      if (!snapshot.enabled) {
        // A disabled output still consumes the frame's graph work but presents nothing.
        continue;
      }
      if (directActive && id == directOutputId) {
        // The last pass wrote the swapchain target directly; consuming would blit it twice.
        continue;
      }
      const std::shared_ptr<OutputRuntime> outputRuntime = NodeAccess::outputRuntime(node);
      if (outputRuntime == nullptr) {
        return fail(EffectResult::Unsupported, "the output node has no runtime");
      }
      outputRuntimes.push_back(outputRuntime);
      FrameView final;
      for (const Edge& edge : plan.edges) {
        if (edge.to != id || edge.inputIndex != 0) continue;
        const auto it = outputs.find(edge.from);
        if (it != outputs.end()) final = it->second;
      }
      if (!final.isValid()) {
        return fail(EffectResult::InvalidArgument, "the output node received no frame");
      }
      uint32_t targetWidth = 0;
      uint32_t targetHeight = 0;
      EffectRotation targetRotation = EffectRotation::None;
      bool targetMirrorHorizontal = false;
      bool targetMirrorVertical = false;
      const bool hasTransform = outputRuntime->targetTransform(targetRotation, targetMirrorHorizontal,
                                                               targetMirrorVertical);
      const bool hasTarget = outputRuntime->targetSize(final.width, final.height, targetWidth, targetHeight) &&
                             targetWidth != 0 && targetHeight != 0;
      const EffectPixelFormat targetFormat = outputRuntime->targetFormat();
      const EffectScaleMode targetScaleMode = outputRuntime->targetScaleMode();
      const bool needsFormat = targetFormat != final.format;
      const RenderTime finalTimestamp = final.timestamp;
      if ((hasTarget && (hasTransform || targetWidth != final.width || targetHeight != final.height)) || needsFormat) {
        if (!hasTarget) {
          targetWidth = final.width;
          targetHeight = final.height;
        }
        const backend::PixelFormat pixelFormat = toPixelFormat(targetFormat);
        if (pixelFormat == backend::PixelFormat::Undefined) {
          return fail(EffectResult::Unsupported, "the output requested an unsupported pixel format");
        }
        FrameKey key;
        key.width = targetWidth;
        key.height = targetHeight;
        key.format = pixelFormat;
        key.sampleCount = 1;
        std::string poolError;
        FrameLease scaledLease = mCore.framePool().acquire(key, poolError);
        if (!scaledLease.isValid()) return fail(EffectResult::FramePoolExhausted, poolError);
        RenderProgram* const scale = context.core->outputScaleProgram(
            targetRotation, targetMirrorHorizontal, targetMirrorVertical, targetScaleMode, error);
        if (scale == nullptr) return fail(EffectResult::Unsupported, error);
        const backend::RenderPipelineHandle pipeline =
            scale->pipelineFor(key.format, scaledLease.get()->pass, nullptr);
        if (!pipeline.valid()) return fail(EffectResult::Unsupported, "the output scale pipeline could not be built");
        const std::vector<backend::TextureHandle> inputs { final.texture };
        const backend::DescriptorSetHandle set =
            scale->setFor(inputs, mCore.parameterTexture(), mCore.linearSampler(), {}, nullptr);
        if (!set.valid()) return fail(EffectResult::Unsupported, "the output scale pass could not bind its input");
        const bool swapped = targetRotation == EffectRotation::Clockwise90 ||
                             targetRotation == EffectRotation::Clockwise270;
        const uint32_t sourceWidth = swapped ? final.height : final.width;
        const uint32_t sourceHeight = swapped ? final.width : final.height;
        ParameterBlock block;
        block.reset();
        block.values[0] = static_cast<float>(sourceWidth);
        block.values[1] = static_cast<float>(sourceHeight);
        block.values[2] = sourceWidth != 0 ? 1.0f / static_cast<float>(sourceWidth) : 0.0f;
        block.values[3] = sourceHeight != 0 ? 1.0f / static_cast<float>(sourceHeight) : 0.0f;
        block.values[4] = static_cast<float>(targetWidth);
        block.values[5] = static_cast<float>(targetHeight);
        block.values[6] = targetWidth != 0 ? 1.0f / static_cast<float>(targetWidth) : 0.0f;
        block.values[7] = targetHeight != 0 ? 1.0f / static_cast<float>(targetHeight) : 0.0f;
        const EffectColor letterbox = outputRuntime->targetLetterboxColor();
        block.values[8] = letterbox.r;
        block.values[9] = letterbox.g;
        block.values[10] = letterbox.b;
        block.values[11] = letterbox.a;
        if (!mCore.writeParameters(block, error)) return fail(EffectResult::Unsupported, error);
        backend::RenderPassBeginInfo info;
        info.renderPass = scaledLease.get()->pass;
        info.colorAttachmentCount = 1;
        info.colorAttachments[0].view = scaledLease.get()->view;
        info.colorAttachments[0].loadOp = backend::LoadOp::Clear;
        info.colorAttachments[0].storeOp = backend::StoreOp::Store;
        info.colorAttachments[0].clearColor = backend::Color::transparentBlack();
        info.renderArea = { targetWidth, targetHeight };
        const bool recorded =
            backend::isSuccess(commands->beginRenderPass(info)) &&
            backend::isSuccess(commands->bindRenderPipeline(pipeline)) &&
            backend::isSuccess(commands->bindDescriptorSet(0, set)) &&
            backend::isSuccess(commands->setVertexBuffer(0, mCore.quadBuffer(), 0)) &&
            backend::isSuccess(commands->draw(kFullTargetVertexCount, 1, 0, 0)) &&
            backend::isSuccess(commands->endRenderPass());
        if (!recorded) return fail(EffectResult::DeviceLost, "the output scale pass could not be recorded");
        final = scaledLease.view();
        final.timestamp = finalTimestamp;
        final.transformApplied = hasTransform;
        leases.push_back(std::move(scaledLease));
      }
      // The present is the one stage an application cannot make cheaper by rendering less, so it is split
      // into acquire/draw and commit: an output that can answer "no image free" returns false with an empty
      // error, which means skip this frame's present - the graph already ran and its result stays readable -
      // rather than fail the frame.
      if (backend::test::consumeDeviceLossInjection()) {
        return fail(EffectResult::DeviceLost, "injected device loss at the output boundary");
      }
      const bool canDefer = outputRuntime->supportsDeferredPresent() &&
              outputRuntime->presentPolicy() == EffectPresentPolicy::Deferred;
      if (!outputRuntime->beginPresent(frameContext, final, error)) {
        if (error.empty() && canDefer) {
          // Busy target: throttling, not failure.
          continue;
        }
        if (error.empty()) error = "the output node refused the frame";
        return fail(EffectResult::DeviceLost, error);
      }
      if (!backend::isSuccess(outputRuntime->commitPresent())) {
        return fail(EffectResult::DeviceLost, "the output node could not present the frame");
      }
      continue;
    }

    // ---- a pass ----
    std::vector<FrameView> inputs(node.inputCount());
    uint32_t sourceWidth = context.renderWidth;
    uint32_t sourceHeight = context.renderHeight;
    for (const Edge& edge : plan.edges) {
      if (edge.to != id || edge.inputIndex >= inputs.size()) continue;
      const auto it = outputs.find(edge.from);
      if (it == outputs.end()) {
        return fail(EffectResult::InvalidArgument, "an input of the pass '" + snapshot.name + "' produced no frame");
      }
      inputs[edge.inputIndex] = it->second;
      if (edge.inputIndex == 0) {
        sourceWidth = it->second.width;
        sourceHeight = it->second.height;
      }
    }

    if (!snapshot.enabled) {
      // A disabled pass is a pass-through, not a hole in the chain.
      if (!inputs.empty() && inputs[0].isValid()) outputs[id] = inputs[0];
      continue;
    }

    PassRuntime& runtime = passRuntime(id);
    runtime.apply(node, snapshot, mCore, context);
    if (runtime.program == nullptr) {
      if (!inputs.empty() && inputs[0].isValid()) {
        NodeAccess::setDiagnostics(node, runtime.diagnostics);
        outputs[id] = inputs[0];
        continue;
      }
      return fail(EffectResult::Unsupported,
                  "the pass '" + snapshot.name + "' has no compiled program: " + runtime.diagnostics);
    }

    const float scale = snapshot.outputScale > 0.0f ? snapshot.outputScale : 1.0f;
    const uint32_t targetWidth = std::max(1u, static_cast<uint32_t>(std::lround(context.renderWidth * scale)));
    const uint32_t targetHeight = std::max(1u, static_cast<uint32_t>(std::lround(context.renderHeight * scale)));
    FrameKey key;
    key.width = targetWidth;
    key.height = targetHeight;
    key.format = toPixelFormat(snapshot.outputFormat);
    if (key.format == backend::PixelFormat::Undefined) {
      return fail(EffectResult::Unsupported, "the pass requested an unsupported output format");
    }
    key.sampleCount = 1;

    struct PassTarget {
      FrameLease lease;
      backend::RenderPassHandle pass = backend::RenderPassHandle::invalid();
      backend::TextureViewHandle view = backend::TextureViewHandle::invalid();
      uint32_t width = 0;
      uint32_t height = 0;
      backend::PixelFormat format = backend::PixelFormat::Undefined;
      bool direct = false;
    };
    PassTarget target;
    if (directActive && id == directPassId) {
      target.direct = true;
      target.view = directTargetInfo.view;
      target.pass = directTargetInfo.renderPass;
      target.width = directTargetInfo.width;
      target.height = directTargetInfo.height;
      target.format = directTargetInfo.format;
      key.width = target.width;
      key.height = target.height;
      key.format = target.format;
    } else {
      std::string poolError;
      target.lease = mCore.framePool().acquire(key, poolError);
      if (!target.lease.isValid()) {
        return fail(EffectResult::FramePoolExhausted, poolError);
      }
      target.pass = target.lease.get()->pass;
      target.view = target.lease.get()->view;
      target.width = targetWidth;
      target.height = targetHeight;
      target.format = key.format;
    }

    ParameterBlock block = runtime.values;
    block.values[0] = static_cast<float>(sourceWidth);
    block.values[1] = static_cast<float>(sourceHeight);
    block.values[2] = sourceWidth != 0 ? 1.0f / static_cast<float>(sourceWidth) : 0.0f;
    block.values[3] = sourceHeight != 0 ? 1.0f / static_cast<float>(sourceHeight) : 0.0f;
    block.values[4] = static_cast<float>(targetWidth);
    block.values[5] = static_cast<float>(targetHeight);
    block.values[6] = 1.0f / static_cast<float>(targetWidth);
    block.values[7] = 1.0f / static_cast<float>(targetHeight);
    if (!mCore.writeParameters(block, error)) return fail(EffectResult::Unsupported, error);

    std::vector<backend::TextureHandle> inputTextures;
    std::vector<backend::TextureSamplerHandle> inputSamplers;
    inputTextures.reserve(inputs.size());
    inputSamplers.reserve(inputs.size());
    for (const FrameView& view : inputs) {
      inputTextures.push_back(view.texture);
      if (view.ycbcr.has_value()) {
        inputSamplers.push_back(mCore.ycbcrSampler(*view.ycbcr, error));
        if (!inputSamplers.back().valid()) return fail(EffectResult::Unsupported, error);
      } else {
        inputSamplers.push_back(backend::TextureSamplerHandle::invalid());
      }
    }

    std::vector<backend::TextureHandle> extras;
    const ProgramSnapshot& program = runtime.program->snapshot();
    extras.reserve(program.textureNames.size());
    for (const std::string& name : program.textureNames) {
      const TextureSource* chosen = nullptr;
      for (const TextureBinding& binding : runtime.textures) {
        if (binding.name == name) {
          chosen = &binding.source;
          break;
        }
      }
      if (chosen == nullptr) {
        for (const SequenceState& sequence : runtime.sequences) {
          if (sequence.name != name) continue;
          uint32_t frameIndex = 0;
          if (!selectSequenceFrame(sequence, context.core->timeBase(), context.timestamp, frameIndex)) {
            static const TextureSource transparent = TextureSource::solid(EffectColor { 0.0f, 0.0f, 0.0f, 0.0f });
            chosen = &transparent;
          } else {
            chosen = &sequence.desc.frames[frameIndex];
          }
          break;
        }
      }
      if (chosen == nullptr) {
        static const TextureSource transparent = TextureSource::solid(EffectColor { 0.0f, 0.0f, 0.0f, 0.0f });
        chosen = &transparent;
      }
      backend::TextureHandle texture = backend::TextureHandle::invalid();
      if (!resolveTexture(mCore, *chosen, frameTextures, texture, error)) {
        return fail(EffectResult::Unsupported, error);
      }
      extras.push_back(texture);
    }

    if (runtime.nextProgram != nullptr && runtime.program != nullptr && runtime.nextPipeline != nullptr &&
        !runtime.nextPipelineStarted && !runtime.nextPipeline->ready.load(std::memory_order_acquire)) {
      runtime.nextPipelineStarted = true;
      const auto pending = runtime.nextPipeline;
      const bool accepted = runtime.nextProgram->requestPipelineAsync(
              key.format, target.pass, [pending](const bool success) {
                pending->failed.store(!success, std::memory_order_release);
                pending->ready.store(true, std::memory_order_release);
              });
      if (!accepted) {
        runtime.nextPipelineStarted = false;
        const backend::RenderPipelineHandle next =
            runtime.nextProgram->pipelineFor(key.format, target.pass, nullptr);
        if (!next.valid()) {
          return fail(EffectResult::Unsupported, "the pass '" + snapshot.name + "' has no pipeline for its target");
        }
        runtime.adoptNextProgram();
      }
    }

    bool pipelineCreated = false;
    const backend::RenderPipelineHandle pipeline =
        runtime.program->pipelineFor(key.format, target.pass, &pipelineCreated);
    if (!pipeline.valid()) {
      return fail(EffectResult::Unsupported, "the pass '" + snapshot.name + "' has no pipeline for its target");
    }

    backend::DescriptorSetHandle set = runtime.program->setFor(inputTextures, mCore.parameterTexture(),
                                                               mCore.linearSampler(), extras, nullptr, &inputSamplers);
    if (!set.valid()) {
      return fail(EffectResult::Unsupported, "the pass '" + snapshot.name + "' could not bind its textures");
    }

    backend::RenderPassBeginInfo info;
    info.renderPass = target.pass;
    info.colorAttachmentCount = 1;
    info.colorAttachments[0].view = target.view;
    info.colorAttachments[0].loadOp = backend::LoadOp::Clear;
    info.colorAttachments[0].storeOp = backend::StoreOp::Store;
    info.colorAttachments[0].clearColor = backend::Color::transparentBlack();
    info.renderArea = { target.width, target.height };

    const bool recorded =
        backend::isSuccess(commands->beginRenderPass(info)) &&
        backend::isSuccess(commands->bindRenderPipeline(pipeline)) &&
        backend::isSuccess(commands->bindDescriptorSet(0, set)) &&
        backend::isSuccess(commands->setVertexBuffer(0, mCore.quadBuffer(), 0)) &&
        backend::isSuccess(commands->draw(kFullTargetVertexCount, 1, 0, 0)) &&
        backend::isSuccess(commands->endRenderPass());
    if (!recorded) {
      return fail(EffectResult::DeviceLost, "the pass '" + snapshot.name + "' could not be recorded");
    }

    FrameView outputView;
    if (target.direct) {
      outputView.width = target.width;
      outputView.height = target.height;
      outputView.format = fromPixelFormat(directTargetInfo.format);
      outputView.colorSpace = EffectColorSpace::SRGB;
      outputView.external = false;
    } else {
      outputView = target.lease.view();
    }
    // The source PTS follows the picture through every pass; a pass with no input falls back to the
    // engine clock.
    const uint32_t timestampIndex = std::min<uint32_t>(snapshot.timestampInput, inputs.empty() ? 0u : static_cast<uint32_t>(inputs.size() - 1));
    outputView.timestamp = !inputs.empty() && inputs[timestampIndex].isValid() ? inputs[timestampIndex].timestamp : context.timestamp;
    outputs[id] = outputView;
    if (target.lease.isValid()) leases.push_back(std::move(target.lease));
  }

  std::shared_ptr<GpuSubmission> submission;
  if (!mCore.submitFrame(submission, error, std::move(completion))) {
    for (const std::shared_ptr<InputRuntime>& runtime : inputRuntimes) runtime->afterAbort();
    for (const std::shared_ptr<OutputRuntime>& runtime : outputRuntimes) runtime->afterAbort();
    return EffectResult::DeviceLost;
  }
  // Every frame the recording wrote is now known to be read by this submission; only after the fence
  // completes may the pool recycle it.
  for (FrameLease& lease : leases) lease.markSubmitted(submission);
  for (const std::shared_ptr<InputRuntime>& runtime : inputRuntimes) runtime->afterSubmit(submission);
  for (const std::shared_ptr<OutputRuntime>& runtime : outputRuntimes) runtime->afterSubmit(submission);
  if (directActive && directOutput != nullptr) {
    directOutput->presentDirectTarget();
    directActive = false;
  }
  return EffectResult::Success;
}

} // namespace coral::detail
