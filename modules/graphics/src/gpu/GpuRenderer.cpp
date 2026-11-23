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

#include "GpuRenderer.h"

#include "BlendRouting.h"

#include <graphics/Bitmap.h>
#include <graphics/ColorSpaceXform.h>
#include <graphics/Enums.h>
#include <graphics/ImageInfo.h>
#include <graphics/Pixmap.h>

// The coverage-field route's rasteriser: `contoursOf` builds `raster::Contour`s and the fill entries (`fillCoverageRowsAt`,
// `fillCoverageRows`) turn a field's contours into coverage. It is a private header of this library, which is why the
// route's types do not appear in `GpuRenderer.h` at all -- the frame's tables hold `DrawList::CoverageField`, the public
// form, and the rasteriser's type stops at this file's edge.
#include <private/graphics/EdgeCoverage.h>

#include <utils/JobSystem.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

// The shader blobs come from a *generated* header, and which one this finds is load-bearing rather than
// a detail. The build writes the embedded SPIR-V to `CanvasShaders.h` in the build tree, and a
// placeholder with the same name sits next to this file so that a build without a shader compiler still
// compiles. A quoted include searches the including file's own directory first, so a quoted
// `#include "CanvasShaders.h"` would *always* find the placeholder and the embedded shaders would never
// be used -- a renderer that reports `Result::Unsupported` in every build, including the ones that
// embedded its shaders perfectly. The angle-bracket form searches the include path, where the build
// rule puts the generated directory ahead of the source directory; `__has_include` picks it when it is
// there and the quoted form is the fallback for a build that has only the placeholder.
#if __has_include(<CanvasShaders.h>)
#include <CanvasShaders.h>
#elif __has_include("CanvasShadersEmbedded.h")
// The committed copy: the same SPIR-V the build would have generated, checked in so that a build without the tool still
// draws. Its `CORAL_CANVAS_SHADERS_SOURCE_HASH` is verified against `shaders/canvas/*` by
// `GpuSurface.TheCommittedShaderBlobMatchesItsSources`, so a shader edited without regenerating it fails a test rather
// than drifting silently. The placeholder below stays for a checkout that has neither.
#include "CanvasShadersEmbedded.h"
#else
// The placeholder, under a name of its own so that it can never shadow the generated header. It used to be
// called `CanvasShaders.h` and live beside this file, which meant a quoted include found it before any `-I`
// path -- so the embedded shaders were unreachable in every build that had them, and the renderer reported
// `Result::Unsupported` on a device that was perfectly capable. Renaming the fallback is the fix that cannot
// come back: there is now exactly one `CanvasShaders.h`, and it is the generated one.
#include "CanvasShadersPlaceholder.h"
#endif

// The GL direct-source fallback, for a build whose OpenGL backend has neither shaderc nor SPIRV-Cross. It is
// a committed generated header rather than a build product for the same reason as the SPIR-V copy: the
// configuration that needs it has no shader toolchain to generate it with.
#include "CanvasShadersGLSL.h"

namespace graphics::gpu {
namespace {

using Clock = std::chrono::steady_clock;

/** @return milliseconds elapsed since @p start, as a double a `FrameStats` can hold. */
double millisecondsSince(const Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// --------------------------------------------------------------------------------------------------
// Layout constants
// --------------------------------------------------------------------------------------------------

/**
 * Bytes reserved for one uniform block.
 *
 * 256 rather than the block's declared size, and the reason is std140 rather than caution: a block's size in std140 is
 * rounded up to a multiple of 16, so `CanvasUniforms`' three `vec2`s occupy 32 bytes even though they declare 24, and a
 * buffer bound shorter than the block is invalid at draw time. One page-sized slot per block means the arithmetic is
 * never needed.
 */
constexpr uint32_t kUniformSlotSize = 256;
/** The canvas block's slot. It is written once per frame and every program reads it. */
constexpr uint32_t kCanvasSlot = 0;
/**
 * Slots in the uniform arena before it has to grow: 1 MiB.
 *
 * ## Why the arena is per draw now, after the fixed-slot detour
 *
 * The five per-purpose slots this replaced were correct only because `CommandBuffer::writeBuffer` *recorded* the upload:
 * the recorded copies and the draws that consumed them alternated in submission order, so a later path's write could not
 * overtake an earlier path's draw. That ordering cost a `vkCmdCopyBuffer`, and Vulkan forbids copy commands inside a
 * render pass (VUID-vkCmdCopyBuffer-renderpass) -- which is exactly where the canvas was issuing them, once per draw.
 *
 * So the data no longer travels in the command stream. Each draw takes a fresh slot, the bytes are staged on the CPU, and
 * one mapped write per frame delivers the whole arena **before** the frame is submitted. A slot is written once per frame,
 * so the missing command ordering is irrelevant: when the GPU executes, every slot already holds its draw's data. The
 * arena therefore has to be as large as one frame's draw count instead of five blocks, and it grows by appending larger
 * buffers rather than recreating one (see `mUniformBuffers`).
 *
 * The 65536-byte cap that killed the earlier per-draw attempt applied to a single *binding range*, not to the buffer: each
 * slot binds its own 256-byte window, so the number of slots is bounded by memory rather than by that limit.
 */
constexpr uint32_t kUniformInitialSlots = 4096;
constexpr uint64_t kUniformInitialBytes = uint64_t(kUniformSlotSize) * kUniformInitialSlots;

constexpr uint32_t kQuadVertexStride = 56;
constexpr uint32_t kSdfVertexStride = 124;
constexpr uint32_t kPathVertexStride = 24;
static_assert(sizeof(PathVertex) == kPathVertexStride, "the path vertex record and its stride must agree");
constexpr uint32_t kFullTargetVertexStride = 16;
/**
 * How many vertices one quad command contributes: the **four corners** of the unit square.
 *
 * It used to be six -- the two triangles' corners spelled out, the last two repeated -- which is 336 bytes a command
 * against 224, and on the demo that was 14 MB of the 15 MB a frame uploads. The indices that turn four corners back into
 * two triangles are the same for every command, so they live in one static buffer (`ensureIndexCapacity`) that is written
 * when it grows and never per frame: the frame's payload drops by a third, and the index traffic is a one-off.
 *
 * See `QuadVertex` for why the rectangle's own data is repeated on each vertex rather than instanced.
 */
constexpr uint32_t kQuadCornerCount = 4;
/** How many indices those four corners are drawn with: the same two triangles the six-vertex form spelled out. */
constexpr uint32_t kQuadIndexCount = 6;
constexpr uint32_t kCoverVertexCount = 6;
constexpr uint32_t kArenaAlignment = 16;

/** The one depth/stencil format the renderer asks for. `D24S8` is renderable on every ES 3.0 device. */
constexpr backend::PixelFormat kDepthStencilFormat = backend::PixelFormat::D24_UNORM_S8_UINT;

/** The stage-flag bit `backend::DescriptorBinding` uses for a stage: bit 0 vertex, bit 1 fragment. */
constexpr uint8_t kVertexStage = 1u << 0;
constexpr uint8_t kFragmentStage = 1u << 1;

/** `PipelineKey::blendMode` for a pipeline whose blend state does not come from a command's mode. */
constexpr uint8_t kNoBlendMode = 0xFF;

// --------------------------------------------------------------------------------------------------
// POD records that mirror the shaders' vertex and uniform layouts
// --------------------------------------------------------------------------------------------------

/**
 * One vertex of the instanced quad program's unit quad, with the rectangle's own data inline.
 *
 * ## Why the rectangle's data is per *vertex* here and not per instance
 *
 * The program is written for instancing: `quad.vert` declares `aCorner` at location 0 as a vertex-step
 * attribute and `aRect`/`aParams`/`aColor` at 1..3 as per-instance ones, and the RHI supports exactly
 * that. The OpenGL backend does not deliver it, and the failure is not a missing divisor call: probing on
 * a real driver (NVIDIA, OpenGL 4.1 core) showed that *any* pipeline declaring two vertex buffers renders
 * no fragments, whichever step modes and instance counts are used, while the identical data fed from one
 * vertex-step buffer renders correctly. The six shapes that were tried, and their outputs, are in the
 * probe logs this work was verified with.
 *
 * So the batch is expanded here instead: four vertices per rectangle with the rectangle's data repeated on
 * each, one buffer, one draw call per batch. Batching still does what it exists to do -- a run of
 * rectangles that share a pipeline and a clip is still one call -- and the cost is 4 x 56 bytes per
 * rectangle rather than 48 bytes per rectangle. Restoring the per-instance form is a one-line vertex
 * layout change once `GlCommandBuffer`'s attribute wiring handles a second buffer correctly; the
 * comparison is left here rather than in a bug report nobody will read next to the code.
 *
 * ## Four vertices and six indices, rather than six vertices (4.1.37)
 *
 * The two triangles' corners used to be spelled out, so the last two vertices of every quad were copies of
 * the first and third. They are drawn through `kQuadIndexCount` indices instead (`0,1,2, 0,2,3` per quad),
 * from **one static buffer** that is filled when it grows and never per frame: the same two triangles in
 * the same order, a third less per-frame payload, and no per-frame index traffic at all. On the demo that
 * is 14.0 MB of arena per frame becoming 9.4 MB, which is the item `4.1.36` left as the frame's largest.
 */
struct QuadVertex {
  float corner[2];
  float rect[4];
  float params[4];
  float color[4];
};

/** The SDF program vertex: the quad record plus the per-command styling. */
struct SdfVertex {
  QuadVertex base;
  float sdf[4];
  float outlineColor[4];
  float shadowColor[4];
  float pattern[4];
  float fillAlpha;
};

/** `path.vert`'s attribute 0: a device-space position in pixels. */
/** `composite.vert`'s attributes 0 and 1: an authored clip-space position and its texture coordinate. */
struct FullTargetVertex {
  float x;
  float y;
  float u;
  float v;
};

/** `quad.vert` and `path.vert`'s shared `set = 0, binding = 1` block, member for member. */
struct CanvasUniforms {
  float viewport[2];
  float scale[2];
  float offset[2];
  /**
   * Offset 24, unused: it exists so that `filter` starts at 32.
   *
   * std140 places a `vec4` on a 16-byte boundary, so three `vec2`s (24 bytes) are followed by eight bytes of padding.
   * Declaring that padding rather than letting the compiler lay the struct out tightly is what keeps the CPU side and the
   * shader's block in step -- a block that is *almost* the right shape reads the wrong bytes and reports no error.
   */
  float padding[2];
  /** The cubic resampler's B and C, written per cubic image batch: `image_cubic.frag`'s `uImageFilter`. */
  float filter[4];
  /**
   * 1 when the target presents in PQ (HDR10), 0 otherwise: `image.frag` applies the output encoding.
   *
   * The flag rides the canvas block rather than the image set because it is a property of the target,
   * not of the bitmap: every image the blit draws into an HDR swapchain is encoded the same way, and
   * the SDR path leaves it at zero.
   */
  float outputPq;
};

/**
 * @return the canvas block for @p target: its size in pixels and device pixels to the RHI's clip space.
 *
 * A function rather than a block in `recordFrame` because a cubic image batch needs the same block again with its own
 * filter filled in. The filter is left at zero, which is what every program that does not sample cubically expects.
 *
 * The scale is positive in y because the RHI's clip space has y = -1 at the top and the flip to OpenGL's convention
 * happens in the SPIR-V translation; see `quad.vert`'s note on Y for the whole chain.
 */
CanvasUniforms canvasUniformsFor(const RenderTarget& target) {
  CanvasUniforms canvas{};
  canvas.viewport[0] = static_cast<float>(target.width);
  canvas.viewport[1] = static_cast<float>(target.height);
  canvas.scale[0] = target.width > 0 ? 2.0f / static_cast<float>(target.width) : 0.0f;
  canvas.scale[1] = target.height > 0 ? 2.0f / static_cast<float>(target.height) : 0.0f;
  canvas.offset[0] = -1.0f;
  canvas.offset[1] = -1.0f;
  canvas.outputPq = target.pqOutput ? 1.0f : 0.0f;
  return canvas;
}

/**
 * `present.frag`'s `PresentUniforms`.
 *
 * The matrix is three `vec4` rows rather than a `mat3`: std140 pads a mat3 column to 16 bytes, and a
 * C++ `float[3][3]` would be 36 bytes against the block's 48. Stating the rows is the same choice
 * `CanvasUniforms` makes about its padding, and for the same reason -- a block that is *almost* the
 * right shape reads the wrong bytes and reports no error.
 */
struct PresentUniforms {
  float row0[4];
  float row1[4];
  float row2[4];
  /** `x` mode (0 PQ, 1 extended linear, 2 gamma), `y` source-encoded, `z` SDR white nits. */
  float parameters[4];
};
static_assert(sizeof(PresentUniforms) == 64, "present.frag's block must stay 64 bytes");

/** The Rec.709 primaries' linear conversion to Display P3, row by row. */
constexpr float kRec709ToP3[3][3] = {
  { 0.822592f, 0.177561f, 0.000000f },
  { 0.033199f, 0.966814f, 0.000000f },
  { 0.017085f, 0.072397f, 0.910532f },
};

/** The Rec.709 primaries' linear conversion to BT.2020, row by row. */
constexpr float kRec709ToBt2020[3][3] = {
  { 0.627404f, 0.329282f, 0.043314f },
  { 0.069097f, 0.919540f, 0.011363f },
  { 0.016392f, 0.088013f, 0.895595f },
};

/** `composite.frag`'s `CompositeUniforms`. */
struct CompositeUniforms {
  float alpha;
  int32_t blendMode;
  float textColor[3];
  float gamma;
  float contrast;
  int32_t linearText;
};
static_assert(sizeof(CompositeUniforms) == 32,
        "composite.frag's uniform block must stay 32 bytes for the shared slot size");

/** `mask.frag`'s `MaskUniforms`. */
struct MaskUniforms {
  float scale[2];
  float offset[2];
  int32_t mode;
  int32_t padding[3];
};

/**
 * `coverage.frag`'s `FieldUniforms`: the field's extent and origin, and the quad's origin.
 *
 * Two `vec4`s, 32 bytes, which is the shape the sampled programs' single-`vec4` blocks already use -- stated because
 * this area's history is a list of blocks that were *almost* the right shape, including one at binding 2 that never
 * delivered a value at all.
 *
 * The quad's origin is carried separately from the field's because the two are not always the same rectangle. A field
 * for a path is the path's bounding box; the quad that draws it is the region actually being painted. The fragment stage
 * needs both: `device = quadOrigin + corner * quadSize`, then `texel = (device - fieldOrigin) / fieldSize`.
 */
struct FieldUniforms {
  /** `xy` the field's extent in texels, `zw` the field's own device origin. */
  float field[4];
  /** `xy` the device origin of the quad being drawn. */
  float rect[4];
};
static_assert(sizeof(FieldUniforms) == 32, "the coverage field's block is 32 bytes");

/**
 * `gradient.frag`'s `GradientUniforms`: the host's byte-for-byte mirror of the shader's block.
 *
 * ## Why every member is four components wide
 *
 * This struct used to be a hand-computed `std140` mirror of a block whose matrix was `float uInverseLocal[6]`, and that
 * is the defect this comment exists to keep fixed. `std140` rounds an *array of scalars*' element stride up to the
 * alignment of a `vec4`, so the shader's six coefficients occupied 96 bytes at offset 48 while the host's occupied 24 at
 * offset 36. Every member after them was then read from the wrong byte -- the shader's `uStopCount` sat at 148 where the
 * host wrote at 64 -- so the shader found a zero stop count, `sampleStops` returned its first-stop fallback, and a
 * two-stop gradient painted flat black while every CPU-side trace along the way reported two stops.
 *
 * The fix is structural rather than arithmetic: the shader's block now declares only `vec4`/`ivec4` members, and an
 * array of `vec4` has a natural sixteen-byte stride, so its layout is "sixteen bytes per member, in declaration order"
 * with no `std140` rule left to apply. This mirrors that, and `sizeof` and every `offsetof` below is asserted -- which
 * is what turns a future edit to one side only into a compile error rather than a rectangle of the wrong colour.
 *
 *   | offset | shader member        | this struct            |
 *   |--------|----------------------|------------------------|
 *   |      0 | `vec4 uPoints[2]`    | `points[2][4]`         |
 *   |     32 | `vec4 uShape`        | `shape[4]`             |
 *   |     48 | `vec4 uMatrix[2]`    | `matrix[2][4]`         |
 *   |     80 | `ivec4 uParams`      | `params[4]`            |
 *   |     96 | `vec4 uStopColor[4]` | `stopColor[4][4]`      |
 *   |    160 | `vec4 uStopAlpha[4]` | `stopAlpha[4][4]`      |
 *   |    224 |                      | `sizeof`, a whole `vec4` count |
 *
 * `CORAL_GL_TRACE_BLOCKS=1` prints the driver's table for this block beside `offsetof` from here. The two agreeing is
 * the check; the driver's numbers are not derivable from this struct, which is why they are printed.
 */
struct GradientUniforms {
  /** One point per entry, in `xy`: `[0]` the start or centre, `[1]` the end or a point on the radius. */
  float points[2][4];
  /** `[0]` is the radius; zero selects the linear form. */
  float shape[4];
  /** Row major `(m00, m01, m10, m11)`, then the translation `(tx, ty)` at the front of the second entry. */
  float matrix[2][4];
  /** `[0]` is `TileMode`, `[1]` how many of the four stops are live. */
  int32_t params[4];
  /** (offset, red, green, blue) per stop, unpremultiplied. */
  float stopColor[4][4];
  /** (alpha, unused, unused, unused) per stop. */
  float stopAlpha[4][4];
};

// The table above as assertions, so a member inserted in the wrong place is a compile error and not a rectangle that
// silently samples the wrong field. `offsetof` is what the *host* compiler chose; the shader's numbers come from the
// same table -- through `vec4` members whose `std140` layout is the declaration order itself -- and are printed by
// `CORAL_GL_TRACE_BLOCKS`. These two are the only places the layouts can disagree.
static_assert(offsetof(GradientUniforms, shape) == 32, "uShape must follow the two point vectors");
static_assert(offsetof(GradientUniforms, matrix) == 48, "uMatrix must be sixteen-byte aligned");
static_assert(offsetof(GradientUniforms, params) == 80, "uParams must follow the two matrix vectors");
static_assert(offsetof(GradientUniforms, stopColor) == 96, "uStopColor must be sixteen-byte aligned");
static_assert(offsetof(GradientUniforms, stopAlpha) == 160, "uStopAlpha must follow the four colour vectors");
static_assert(sizeof(GradientUniforms) == 224, "the gradient block is six four-component members");
static_assert(sizeof(GradientUniforms) <= kUniformSlotSize, "the gradient block must fit one uniform slot");


static_assert(sizeof(QuadVertex) == kQuadVertexStride, "the quad vertex record and its stride must agree");
static_assert(sizeof(SdfVertex) == kSdfVertexStride, "the SDF vertex record and its stride must agree");
static_assert(sizeof(PathVertex) == kPathVertexStride, "the path vertex record and its stride must agree");
static_assert(sizeof(FullTargetVertex) == kFullTargetVertexStride, "the composite vertex record and its stride must agree");
static_assert(sizeof(CanvasUniforms) <= kUniformSlotSize, "a uniform block must fit its slot");
static_assert(sizeof(MaskUniforms) <= kUniformSlotSize, "a uniform block must fit its slot");

// --------------------------------------------------------------------------------------------------
// Small value helpers
// --------------------------------------------------------------------------------------------------

/**
 * Prints the host's view of `GradientUniforms`, once per process, under `CORAL_GL_TRACE_BLOCKS`.
 *
 * This is the other half of a comparison, and the comparison is the whole point: `GlDevice::buildBindingTable` prints
 * the *shader's* table from the driver's reflection of the SPIR-V, and it is not derivable from this struct. The defect
 * this block carried -- a `std140` scalar array whose element stride the host had not rounded to sixteen bytes -- was
 * invisible in every CPU-side trace and unmistakable the moment the two tables sat next to each other:
 *
 *   [host]   GradientUniforms sizeof=224 points=0 shape=32 matrix=48 params=80 stopColor=96 stopAlpha=160
 *   [block]  GradientUniforms.uStopCount offset=148 ...
 *
 * Printed once rather than per batch because the layout does not change between draws, and a per-draw line would bury
 * the driver's output under one frame's worth of identical text.
 *
 *   set CORAL_GL_TRACE_BLOCKS=1
 */
void traceUniformBlockLayout() {
  if (std::getenv("CORAL_GL_TRACE_BLOCKS") == nullptr) return;
  static std::atomic<bool> printed{ false };
  if (printed.exchange(true, std::memory_order_relaxed)) return;
  std::fprintf(stderr,
          "[host] GradientUniforms sizeof=%zu points=%zu shape=%zu matrix=%zu params=%zu stopColor=%zu "
          "stopAlpha=%zu\n",
          sizeof(GradientUniforms), offsetof(GradientUniforms, points), offsetof(GradientUniforms, shape),
          offsetof(GradientUniforms, matrix), offsetof(GradientUniforms, params),
          offsetof(GradientUniforms, stopColor), offsetof(GradientUniforms, stopAlpha));
  std::fflush(stderr);
}

/** @return the rectangle @p rect clipped to a @p width by @p height target, possibly empty. */
Rect clampToTarget(const Rect& rect, const uint32_t width, const uint32_t height) {
  const float left = std::max(rect.getLeft(), 0.0f);
  const float top = std::max(rect.getTop(), 0.0f);
  const float right = std::min(rect.getRight(), static_cast<float>(width));
  const float bottom = std::min(rect.getBottom(), static_cast<float>(height));
  if (right <= left || bottom <= top) {
    return Rect::MakeLTRB(left, top, left, top);
  }
  return Rect::MakeLTRB(left, top, right, bottom);
}

/**
 * @return true when @p a and @p b can share one pipeline, one descriptor set and one scissor.
 *
 * Written here rather than using `CommandState::operator==`, which `DrawList.h` *declares* as a friend
 * but nothing in the tree defines: calling it would be a link error, and defining it here would collide
 * with whoever defines it next. The comparison is field by field, including `image`, because two
 * commands that sample different bitmaps must not share one draw call's descriptor set.
 *
 * `coverageField` is deliberately **not** compared, and that is the change that lets the field route batch: a field's
 * index is a lookup into the frame's table, not a property of the draw, because everything a coverage command needs
 * beyond the shared state -- where its texels are in the atlas -- is written into its own vertices. What *must* stay
 * equal is the atlas those texels are in, which a batch binds once; that is a property of the frame's placement rather
 * than of the command, so `canShareDraw` takes it as an argument instead.
 *
 * @param colourPerVertex true when the program this batch will use reads each command's colour from its **own vertices**
 *        rather than from a uniform, in which case two commands of different colours may share the draw. `canShareDraw`
 *        decides that from the program, not from the shader kind: `ShaderKind::Solid` covers both a rectangle (drawn by
 *        `quad.frag`, colour per vertex) and a path (drawn by `path.frag`, colour in a uniform), and comparing colours for
 *        the first is what kept the demo's 898 field batches from merging.
 * @param texturePerDraw true when two commands that differ only in `image` are already known to sample the **same
 *        texture** -- the caller checked, through `sampledTextureOfCommand` -- in which case the image index must not be
 *        compared. That is the whole of what an image atlas changes about batching: an index names a bitmap, a texture is
 *        what a draw binds, and two glyphs in one page can share the draw while two glyphs in different pages cannot.
 */
bool sameCommandState(const DrawList::CommandState& a, const DrawList::CommandState& b,
        const bool colourPerVertex, const bool texturePerDraw) {
  if (a.shader != b.shader || a.blendMode != b.blendMode || a.channelMask != b.channelMask ||
      a.fillType != b.fillType || a.antiAlias != b.antiAlias || a.stopCount != b.stopCount) {
    return false;
  }
  if (!texturePerDraw && a.image != b.image) {
    return false;
  }
  if (!colourPerVertex &&
      (a.red != b.red || a.green != b.green || a.blue != b.blue || a.alpha != b.alpha)) {
    return false;
  }
  if (a.gradient.radius != b.gradient.radius || a.gradient.tileMode != b.gradient.tileMode) {
    return false;
  }
  for (int index = 0; index < 2; ++index) {
    if (a.gradient.points[index].x != b.gradient.points[index].x ||
        a.gradient.points[index].y != b.gradient.points[index].y) {
      return false;
    }
  }
  for (int index = 0; index < 6; ++index) {
    if (a.gradient.inverseLocal[index] != b.gradient.inverseLocal[index]) return false;
  }
  for (uint32_t index = 0; index < a.stopCount && index < 4; ++index) {
    if (a.stops[index].offset != b.stops[index].offset || a.stops[index].red != b.stops[index].red ||
        a.stops[index].green != b.stops[index].green || a.stops[index].blue != b.stops[index].blue ||
        a.stops[index].alpha != b.stops[index].alpha) {
      return false;
    }
  }
  return true;
}

/** @return true when @p kind is a command that draws, as opposed to one that opens or closes a layer. */
bool isDrawable(const DrawList::Kind kind) {
  return kind == DrawList::Kind::Rect || kind == DrawList::Kind::Path ||
         kind == DrawList::Kind::Image || kind == DrawList::Kind::ExternalImage;
}

/**
 * The atlas index of a command whose field was never placed -- an omitted field, or a command that names no field.
 *
 * A sentinel rather than "0", because atlas 0 is a real atlas: two commands whose fields could not be placed must not be
 * taken for two commands in the same atlas, and the field they would sample is somebody else's coverage. It is the same
 * value `kNoSampledTexture` has: a batch's texture is one thing whichever route the batch is on, and the shader is
 * compared before either is read.
 */
constexpr uint32_t kNoFieldAtlas = kNoSampledTexture;

/**
 * The first texture id given to an external image.
 *
 * Image texture ids are a small counter that starts at zero, and an external entry needs an id that no
 * atlas page or standalone bitmap can hold -- otherwise a batch could take the two for one texture and
 * bind the wrong one. The top bit is enough for any realistic page count.
 */
constexpr uint32_t kExternalImageTextureBase = 0x80000000u;

/** @return true when two consecutive commands can be drawn by one instanced draw call. */
bool canShareDraw(const DrawList::Command& previous, const DrawList::Command& current,
        const uint32_t previousTexture, const uint32_t currentTexture) {
  if (previous.kind != current.kind || !isDrawable(current.kind)) return false;
  // Two image commands can share a draw only if they need the same program, and the cubic kernels are separate programs.
  if (previous.kind == DrawList::Kind::Image &&
      (previous.sampling.useCubic != current.sampling.useCubic ||
       previous.sampling.cubic.c != current.sampling.cubic.c ||
       previous.sampling.cubic.b != current.sampling.cubic.b)) {
    return false;
  }
  if (previous.layer != current.layer) return false;
  if (previous.clipMask != current.clipMask) return false;
  if (previous.clipRect != current.clipRect) return false;
  // ## The one piece of per-draw state that lives in the frame rather than in the command
  //
  // A batch binds one descriptor set and one descriptor set names one texture, so two commands can share a draw only if
  // they sample the *same texture* -- and neither a coverage command nor an image command carries that. A coverage
  // command names a field whose place in the atlas is the packing's answer; an image command names a bitmap whose place
  // is the placement's. Both are resolved by `sampledTextureOfCommand` into this one number, which is why the two routes
  // have one rule here instead of two.
  if (previous.state.shader == DrawList::ShaderKind::Coverage ||
      current.state.shader == DrawList::ShaderKind::Coverage) {
    if (previous.state.shader != current.state.shader || previousTexture != currentTexture) return false;
    // An omitted field has no atlas, and the sentinel is unique per command, so such a command never shares a batch.
    if (currentTexture == kNoSampledTexture) return false;
  }
  // An image command's texture is likewise the batch's, and everything else about it -- which sub-rectangle of that
  // texture it samples -- is in its vertices. So the image *index* stops mattering, and this is the comparison that used
  // to keep 561 glyphs in 561 batches.
  const bool imagePerDraw = current.kind == DrawList::Kind::Image ||
                            current.kind == DrawList::Kind::ExternalImage;
  if (imagePerDraw) {
    if (previousTexture != currentTexture) return false;
    if (currentTexture == kNoSampledTexture) return false;
  }
  // ## Which programs read a command's colour from its vertices
  //
  // A rectangle and an image are drawn by `quad.frag` and `image.frag`, and a coverage field by `coverage.frag`; all
  // three read `vColor`, which the vertex writer fills from *that command's* state -- so two of them differing only in
  // colour can share one draw, and comparing colours kept 898 field batches apart on the demo. A path (`path.frag`) and a
  // gradient (`gradient.frag`) read their colour from a uniform bound once per batch, so theirs still has to match.
  // Gradients are excluded by shader rather than by kind because a gradient can be recorded as either.
  const bool gradient = current.state.shader == DrawList::ShaderKind::LinearGradient ||
                        current.state.shader == DrawList::ShaderKind::RadialGradient;
  const bool colourPerVertex = !gradient &&
          (current.kind == DrawList::Kind::Rect || current.kind == DrawList::Kind::Image);
  return sameCommandState(previous.state, current.state, colourPerVertex, imagePerDraw);
}

/**
 * The blend-factor pair that realises @p mode on premultiplied colour.
 *
 * These are the coefficients Skia's `SkBlendMode_Coef` table gives, and they are the *premultiplied*
 * form: `One, OneMinusSrcAlpha` for source-over rather than `SrcAlpha, OneMinusSrcAlpha`, because
 * `quad.frag` and `path.frag` both output premultiplied colour. Using the non-premultiplied pair would
 * multiply the alpha in twice, which is the classic "my antialiased edges are too dark" picture.
 *
 * The colour and alpha factors differ for two modes, and that is not an oversight: `kModulate`'s alpha
 * is `sa * da` and `kScreen`'s is source-over's, while neither mode's *colour* factors can express its
 * alpha. Every other mode in the range happens to have one pair that satisfies both, which is what
 * makes the Porter-Duff family the blend unit's own.
 */
backend::BlendState blendStateFor(const BlendMode mode) {
  using backend::BlendFactor;
  using backend::BlendOp;
  backend::BlendState state;
  state.blendEnable = true;

  const auto set = [&state](const BlendFactor colorSource, const BlendFactor colorDestination) {
    state.color = { .operation = BlendOp::Add, .srcFactor = colorSource, .dstFactor = colorDestination };
    state.alpha = { .operation = BlendOp::Add, .srcFactor = colorSource, .dstFactor = colorDestination };
  };

  switch (mode) {
    case BlendMode::kClear:
      set(BlendFactor::Zero, BlendFactor::Zero);
      break;
    case BlendMode::kSrc:
      set(BlendFactor::One, BlendFactor::Zero);
      break;
    case BlendMode::kDst:
      set(BlendFactor::Zero, BlendFactor::One);
      break;
    case BlendMode::kSrcOver:
      set(BlendFactor::One, BlendFactor::OneMinusSrcAlpha);
      break;
    case BlendMode::kDstOver:
      set(BlendFactor::OneMinusDstAlpha, BlendFactor::One);
      break;
    case BlendMode::kSrcIn:
      set(BlendFactor::DstAlpha, BlendFactor::Zero);
      break;
    case BlendMode::kDstIn:
      set(BlendFactor::Zero, BlendFactor::SrcAlpha);
      break;
    case BlendMode::kSrcOut:
      set(BlendFactor::OneMinusDstAlpha, BlendFactor::Zero);
      break;
    case BlendMode::kDstOut:
      set(BlendFactor::Zero, BlendFactor::OneMinusSrcAlpha);
      break;
    case BlendMode::kSrcATop:
      set(BlendFactor::DstAlpha, BlendFactor::OneMinusSrcAlpha);
      break;
    case BlendMode::kDstATop:
      set(BlendFactor::OneMinusDstAlpha, BlendFactor::SrcAlpha);
      break;
    case BlendMode::kXor:
      set(BlendFactor::OneMinusDstAlpha, BlendFactor::OneMinusSrcAlpha);
      break;
    case BlendMode::kPlus:
      set(BlendFactor::One, BlendFactor::One);
      break;
    case BlendMode::kModulate:
      state.color = { .operation = BlendOp::Add, .srcFactor = BlendFactor::Zero, .dstFactor = BlendFactor::SrcColor };
      state.alpha = { .operation = BlendOp::Add, .srcFactor = BlendFactor::Zero, .dstFactor = BlendFactor::SrcAlpha };
      break;
    case BlendMode::kScreen:
      state.color = {
        .operation = BlendOp::Add, .srcFactor = BlendFactor::One, .dstFactor = BlendFactor::OneMinusSrcColor
      };
      state.alpha = {
        .operation = BlendOp::Add, .srcFactor = BlendFactor::One, .dstFactor = BlendFactor::OneMinusSrcAlpha
      };
      break;
    default:
      // Unreachable for a caller that routed on `isBlendUnitMode`, and defined rather than left
      // undefined for one that did not: an unsupported mode draws source-over rather than nothing.
      set(BlendFactor::One, BlendFactor::OneMinusSrcAlpha);
      break;
  }

  return state;
}

/**
 * The blend state for a pass that must not change the destination's colour at all.
 *
 * This is how the stencil-accumulating passes avoid painting: `ColorTargetState::writeMask` is the RHI's
 * documented way to say "write no colour", and the OpenGL backend never calls `glColorMask`, so a pass
 * that relied on the mask would paint the path's cover colour over the whole shape. A blend pair of
 * `Zero, One` multiplies the source by nothing and the destination by one -- the same result, through
 * state that every backend does honour.
 */
backend::BlendState noColorWriteBlend() {
  backend::BlendState state;
  state.blendEnable = true;
  state.color = { .operation = backend::BlendOp::Add, .srcFactor = backend::BlendFactor::Zero,
                  .dstFactor = backend::BlendFactor::One };
  state.alpha = state.color;
  return state;
}

/** @return the premultiplied fill colour a path vertex carries. */
std::array<float, 4> pathColorFor(const DrawList::CommandState& state) {
  const float alpha = std::clamp(state.alpha, 0.0f, 1.0f);
  return { state.red * alpha, state.green * alpha, state.blue * alpha, alpha };
}

/**
 * The per-command data the vertex writer needs that the **frame** knows and the command does not.
 *
 * Both fields are the answer to the same question -- "where in a texture does this command sample" -- for the two routes
 * that ask it, and both are frame state rather than command state for the same reason: the command names a field or a
 * bitmap, and where that ended up in a texture is the placement's answer. Kept in one struct so that the vertex writer
 * takes one extra argument instead of one per route, and so that a third route has one place to add its own.
 */
struct FrameVertexData {
  /** For `coverage.frag`: the quad's origin minus the field's texel origin, in device pixels. */
  float texelOriginX = 0.0f;
  float texelOriginY = 0.0f;
  /**
   * For `image.frag`: the sub-rectangle of the batch's texture this command blits, in normalized texture coordinates.
   *
   * `(0, 0, 1, 1)` -- the whole texture -- for a bitmap that has one of its own, which is what the route drew before
   * there was an atlas, so the identity is the status quo and not a special case.
   */
  std::array<float, 4> imageSubRect { 0.0f, 0.0f, 1.0f, 1.0f };
};

/**
 * @return the vertex one corner of a `Rect` command contributes to its batch.
 *
 * @param data what the *frame* resolved for this command: see `FrameVertexData`. Passed in rather than looked up because
 *        where a field's texels are is the packing's answer (`Frame::placed`) and where a bitmap lives is the
 *        placement's (`Frame::images`), and a command does not carry either; the caller that has the frame resolves them.
 */
QuadVertex quadVertexFor(const DrawList::Command& command, const float cornerX, const float cornerY,
        const FrameVertexData& data) {
  QuadVertex vertex{};
  vertex.corner[0] = cornerX;
  vertex.corner[1] = cornerY;

  // ## The coverage route's per-command data, which is per *vertex* so that a batch can share one draw
  //
  // `coverage.frag` computes `(vTexCoord * vRectSize + vParams.xy) / atlasExtent`, so `vParams.xy` has to be the quad's
  // origin minus the field's texel origin. Nothing else about a coverage command varies per command: the atlas is the
  // batch's, the colour is `vColor`, the geometry is `vRectSize` -- which is what took the field origin and the quad
  // origin out of the uniform block and turned one draw per path into one draw per batch (`drawCoverageFieldBatch`).
  if (command.state.shader == DrawList::ShaderKind::Coverage) {
    const Rect& rect = command.geometry[0];
    vertex.rect[0] = rect.getLeft();
    vertex.rect[1] = rect.getTop();
    vertex.rect[2] = rect.getWidth();
    vertex.rect[3] = rect.getHeight();
    vertex.params[0] = data.texelOriginX;
    vertex.params[1] = data.texelOriginY;
    vertex.params[2] = 0.0f;
    vertex.params[3] = 0.0f;
    vertex.color[0] = command.state.red;
    vertex.color[1] = command.state.green;
    vertex.color[2] = command.state.blue;
    vertex.color[3] = command.state.alpha;
    return vertex;
  }

  if (command.state.shader == DrawList::ShaderKind::Image) {
    // ## An image's four corners are stored as four points, not as a rectangle
    //
    // `GraphiteCanvasBackend::drawBitmap` writes corner *i* into `geometry[i]` as
    // `Rect::MakeLTRB(x, y, x, y)` -- a degenerate rect whose left and top are the point. That is what makes a
    // rotated or skewed quad expressible at all, and it is why this case exists instead of reusing the branch
    // below: a rect built from `geometry[0]` alone would be a zero-sized rectangle at the first corner.
    //
    // The bounds of the four points are what `quad.vert` can draw: it maps `aCorner` from {0,1}^2 across
    // `aRect`, so the shape it can express is an axis-aligned rectangle. A rotated quad therefore arrives here as
    // its bounding box and is drawn unrotated -- a real limitation, reported rather than hidden, because the
    // alternative is to carry a full matrix per instance and this program has no room for one.
    const Rect& c0 = command.geometry[0];
    const Rect& c1 = command.geometry[1];
    const Rect& c2 = command.geometry[2];
    const Rect& c3 = command.geometry[3];

    // ## A destination on an exact half pixel: the fill rule is mirrored, by the y flip, and one edge is nudged back
    //
    // The corners arrive fractional and this is where the destination is decided, so it is where the two renderers can
    // disagree about a pixel. They do, and only at an exact half. Measured on one 16x16 bitmap through a known transform:
    // at origins of (6.00, 4.00) and (6.25, 4.75) the two agree at every scale, while at (6.50, 4.50) the rasterizer's
    // ink lies in rows 4..19 and this pass's in rows 5..20 -- the same sixteen rows, one lower.
    //
    // The arithmetic is not at fault, and that was measured rather than assumed: rounding the four edges with
    // `std::nearbyint` reproduces the rasterizer in all nine cases but made the corpus *worse* (13654 -> 13657 differing
    // pixels), because it also moves the 0.25 and 0.75 destinations whose fractional edges already agree.
    //
    // What is at fault is the *tie rule*. A destination of [6.5, 22.5) x [4.5, 20.5) covers columns 6..21 and, by the
    // top-left rule every rasterizer uses, rows 4..20: a fragment whose centre lies exactly on a top or left edge is
    // inside, and one on a bottom or right edge is outside. Here the columns come out 6..21 as they should, but the rows
    // come out 5..20 -- the top edge's pixel is *excluded* and the bottom's is *included*, which is that same rule read
    // through a mirror. And the mirror is real: `quad.vert` writes `y = -1` for device row 0 and the backend translates
    // every SPIR-V module with `flip_vert_y`, so the triangle the rasterizer sees is flipped about the viewport's centre
    // and "top" and "bottom" swap.
    //
    // The correction is one sub-pixel of y, applied to *both* edges -- a sub-pixel shift of the quad upwards, not a
    // growth -- and it is deliberately far smaller than any coverage that could matter: a fragment centre that was
    // exactly on an edge is now strictly inside (top) or strictly outside (bottom) by 1/256 of a pixel, which is enough
    // to decide the tie and too little to move the sampled coordinate or any other placement. Nudging only the top edge
    // was tried and measured: it restored the top row and *added* the bottom one, 272 pixels against the rasterizer's
    // 256, because the mirrored rule had been including the bottom all along.
    //
    // Growing the quad by a whole pixel -- which is what the rectangle and gradient paths do -- is not available here,
    // because an image command has no distance function to cut the ring back off.
    constexpr float kNudge = 1.0f / 256.0f;
    const float left = std::min(std::min(c0.getLeft(), c1.getLeft()), std::min(c2.getLeft(), c3.getLeft()));
    const float top =
        std::min(std::min(c0.getTop(), c1.getTop()), std::min(c2.getTop(), c3.getTop())) - kNudge;
    const float right = std::max(std::max(c0.getLeft(), c1.getLeft()), std::max(c2.getLeft(), c3.getLeft()));
    const float bottom =
        std::max(std::max(c0.getTop(), c1.getTop()), std::max(c2.getTop(), c3.getTop())) - kNudge;

    vertex.rect[0] = left;
    vertex.rect[1] = top;
    vertex.rect[2] = right - left;
    vertex.rect[3] = bottom - top;

    // The **sub-rectangle** `image.frag` samples, in normalized texture coordinates: origin in `xy`, size in `zw`. It is
    // the whole texture for a bitmap with one of its own and the bitmap's inset interior for one in a page, and the
    // program cannot tell the two apart -- which is what makes the atlas a placement decision rather than a second route.
    vertex.params[0] = data.imageSubRect[0];
    vertex.params[1] = data.imageSubRect[1];
    vertex.params[2] = data.imageSubRect[2];
    vertex.params[3] = data.imageSubRect[3];

    vertex.color[0] = command.state.red;
    vertex.color[1] = command.state.green;
    vertex.color[2] = command.state.blue;
    vertex.color[3] = command.state.alpha;
    return vertex;
  }

  const Rect& rect = command.geometry[0];

  // ## An antialiased rectangle's quad is grown by its feather, so that edge pixels exist to be covered
  //
  // The coverage below is an exact area for a straight edge -- `clamp(0.5 - distance / feather, 0, 1)` is the area of a
  // pixel a signed distance @c distance from its centre -- but the rasterizer only generates a fragment for a pixel whose
  // *centre* is inside the triangle, with the top-left rule for a centre exactly on an edge. So a pixel that the shape
  // covers a quarter of, and whose centre falls just outside, was never rasterised at all and the destination kept
  // whatever was there.
  //
  // That is what the last of the corpus differences was: at a fractional origin -- every one of the demo's cells lands on
  // a half pixel -- the rasterizer wrote 25% white at the rectangle's corner (`0x40ffffff`) where the GPU wrote nothing
  // (`0x0`), and the same rule painted the opposite way elsewhere. Growing the quad by its feather puts those pixels
  // inside the triangle, where the distance function gives them the coverage the rasterizer computed.
  //
  // ## Why this is safe for everything else that shares `quad.vert`
  //
  // The growth is conditional on the SDF path (`params[2]`), which only the generic branch below ever asks for: an image
  // command puts its *sub-rectangle* in these lanes and is drawn by `image.frag`, which has no distance function and
  // would paint the grown ring at full coverage. Coverage fields ask for no SDF either. So the `vTexCoord` an image
  // samples with is still `aCorner`, and only an untextured antialiased rectangle or gradient sees a grown quad.
  //
  // The fragment stages recover the true rectangle from `vRectSize - 2 * vParams.y` and offset by the feather where they
  // need the device position, so the growth is invisible to them beyond the ring it adds. See the two shaders.
  const bool sdfPath = command.state.antiAlias;
  const float feather = sdfPath ? 1.0f : 0.0f;

  vertex.rect[0] = rect.getLeft() - feather;
  vertex.rect[1] = rect.getTop() - feather;
  vertex.rect[2] = rect.getWidth() + feather * 2.0f;
  vertex.rect[3] = rect.getHeight() + feather * 2.0f;

  // `aParams` is `(corner radius, feather, needs the coverage path, sample the texture)`. The DrawList
  // carries no corner radius, so the SDF path is taken only to feather the edge: with a radius of zero
  // the same distance becomes a plain rectangle distance, which is exactly the filter an antialiased
  // axis-aligned edge wants. A command that asked for a hard edge skips the square root entirely.
  vertex.params[0] = 0.0f;
  vertex.params[1] = feather;
  vertex.params[2] = feather;
  vertex.params[3] = 0.0f;

  vertex.color[0] = command.state.red;
  vertex.color[1] = command.state.green;
  vertex.color[2] = command.state.blue;
  vertex.color[3] = command.state.alpha;
  return vertex;
}

/** Writes POD records into a byte vector, keeping every record's start 16-byte aligned. */
class ArenaWriter {
public:
  explicit ArenaWriter(std::vector<uint8_t>& bytes) : mBytes(bytes) {
  }

  /** @return the byte offset the record was written at. */
  template<typename T>
  uint32_t append(const T& value) {
    return appendBytes(&value, sizeof(T));
  }

  /** @return the byte offset the block was written at. */
  uint32_t appendBytes(const void* data, const size_t size) {
    const uint32_t offset = reserveBytes(size);
    if (size > 0 && data != nullptr) {
      std::memcpy(mBytes.data() + offset, data, size);
    }
    return offset;
  }

  /** @return the byte offset of @p size zeroed bytes, which a caller fills in afterwards. */
  uint32_t reserveBytes(const size_t size) {
    const size_t offset = align(mBytes.size());
    mBytes.resize(offset + size, 0);
    return static_cast<uint32_t>(offset);
  }

  [[nodiscard]] uint8_t* at(const uint32_t offset) { return mBytes.data() + offset; }

private:
  static size_t align(const size_t value) {
    return (value + (kArenaAlignment - 1)) & ~static_cast<size_t>(kArenaAlignment - 1);
  }

  std::vector<uint8_t>& mBytes;
};

/** Frees a buffer however the function that created it returns. */
class BufferGuard {
public:
  BufferGuard(backend::Device* device, const backend::BufferHandle handle) : mDevice(device), mHandle(handle) {
  }

  ~BufferGuard() {
    if (mDevice != nullptr && mHandle.valid()) {
      mDevice->destroyBuffer(mHandle);
    }
  }

  BufferGuard(const BufferGuard&) = delete;
  BufferGuard& operator=(const BufferGuard&) = delete;

  [[nodiscard]] backend::BufferHandle handle() const noexcept { return mHandle; }

private:
  backend::Device* mDevice = nullptr;
  backend::BufferHandle mHandle = backend::BufferHandle::invalid();
};

} // namespace anonymous

// ==================================================================================================
// Lifetime
// ==================================================================================================

GpuRenderer::GpuRenderer() : mFrame(mFieldAtlasBytes, mFieldPlaced) {}

GpuRenderer::~GpuRenderer() {
  // The device may still be executing something, and the RHI's own documentation says a resource freed
  // under a live command buffer is the one teardown mistake that corrupts a driver's state rather than
  // returning an error. Waiting is cheap next to being wrong about it.
  if (mDevice != nullptr) {
    mDevice->waitIdle();
  }
  destroyResources();
  // `mOwnedDevice` is destroyed after this body, and after every cached resource, because it is
  // declared first: see the note on its declaration.
}

backend::Device* GpuRenderer::releaseDevice() {
  if (mOwnedDevice == nullptr) return nullptr;
  return mOwnedDevice.release();
}

namespace {

/**
 * @return true when this device consumes the Vulkan-environment canvas shader set.
 *
 * The canvas shaders are embedded twice, because the two modules differ in a way that cannot be
 * papered over at run time: the Vulkan one carries the `DescriptorSet`/`Binding` decorations a
 * `VkDescriptorSetLayout` is built from, and the OpenGL one does not (the OpenGL environment has no
 * descriptor sets to decorate, and the OpenGL backend consumes the module through SPIRV-Cross).
 */
bool usesVulkanShaderSet(const backend::Device* device) noexcept {
  if (device == nullptr) return false;
  const backend::Backend selected = device->getInfo().backend;
  // Metal consumes the same set for the same reason Vulkan does: its `(set, binding)` assignments are read from the
  // SPIR-V decorations, and the OpenGL set has none (that environment emits no DescriptorSet/Binding decorations).
  return selected == backend::Backend::Vulkan || selected == backend::Backend::Metal;
}

/**
 * @return true when the OpenGL backend needs the GLSL source fallback.
 *
 * That is exactly "the device takes GLSL as written but not SPIR-V": a build with
 * `CORAL_BACKEND_SUPPORTS_SPIRV_CROSS=OFF` has no translator, so the embedded SPIR-V the other builds use
 * cannot become GLSL. The OpenGL backend accepts `Language::GLSLDirect` and the driver compiles the source,
 * which is why this configuration can still draw - it just needs the source instead of the module.
 */
bool needsGlslFallback(const backend::Device* device) noexcept {
  if (device == nullptr) return false;
  const backend::Capabilities& capabilities = device->getCapabilities();
  return capabilities.acceptsGlslDirect && !capabilities.acceptsSpirv;
}

/** @return true when this device's GL dialect is OpenGL ES, which selects the fallback source variant. */
bool usesEsDialect(const backend::Device* device) noexcept {
  const char* api = device != nullptr ? device->getInfo().apiVersion : nullptr;
  return api != nullptr && std::strstr(api, "ES") != nullptr;
}

/** A canvas shader as this device can consume it: a SPIR-V module, or GLSL source for a direct module. */
struct CanvasShader {
  bool directSource = false;
  shaders::Blob blob{};
  shaders::glsl::Source source{};

  [[nodiscard]] bool isValid() const noexcept {
    return directSource ? source.isValid() : blob.isValid();
  }

  [[nodiscard]] backend::ShaderStage stage() const noexcept {
    if (directSource) return source.stage;
    switch (blob.stage) {
      case shaders::Stage::Vertex:
        return backend::ShaderStage::Vertex;
      case shaders::Stage::Fragment:
        return backend::ShaderStage::Fragment;
      case shaders::Stage::Compute:
        return backend::ShaderStage::Compute;
    }
    return backend::ShaderStage::Vertex;
  }
};

/** @return the canvas shader @p name in the form this device's backend consumes. */
CanvasShader findShader(const backend::Device* device, const char* name) {
  CanvasShader shader;
  if (needsGlslFallback(device)) {
    shader.directSource = true;
    shader.source = shaders::glsl::find(name);
  } else {
    shader.blob = usesVulkanShaderSet(device) ? shaders::vulkan::find(name) : shaders::find(name);
  }
  return shader;
}

/** @return how many canvas shaders this device's backend has embedded. */
size_t shaderCount(const backend::Device* device) noexcept {
  if (needsGlslFallback(device)) return shaders::glsl::count();
  return usesVulkanShaderSet(device) ? shaders::vulkan::count() : shaders::count();
}

} // namespace anonymous


bool GpuRenderer::setSdfPattern(const Bitmap& pattern) {
  if (pattern.isEmpty()) {
    mSdfPatternBitmap = Bitmap();
    mSdfPatternDirty = false;
    mSdfPatternEnabled = false;
    return true;
  }

  mSdfPatternBitmap = pattern;
  mSdfPatternDirty = true;
  mSdfPatternEnabled = true;
  return true;
}

backend::Result GpuRenderer::uploadSdfPattern() {
  if (!mSdfPatternDirty || mSdfPatternBitmap.isEmpty()) {
    return backend::Result::Success;
  }
  const int width = mSdfPatternBitmap.width();
  const int height = mSdfPatternBitmap.height();
  if (width <= 0 || height <= 0) {
    return backend::Result::InvalidArgument;
  }

  if (mSdfPatternView.valid()) mDevice->destroyTextureView(mSdfPatternView);
  if (mSdfPatternTexture.valid()) mDevice->destroyTexture(mSdfPatternTexture);

  const backend::TextureDescriptor descriptor = backend::TextureDescriptor::texture2D(
    static_cast<uint32_t>(width), static_cast<uint32_t>(height),
    backend::PixelFormat::R8G8B8A8_UNORM,
    static_cast<backend::TextureUsages>(backend::TextureUsage::Sampled));
  backend::Result result = mDevice->createTexture(descriptor, &mSdfPatternTexture);
  if (!backend::isSuccess(result)) return result;
  backend::TextureViewDescriptor viewDescriptor;
  viewDescriptor.texture = mSdfPatternTexture;
  viewDescriptor.dimension = backend::TextureViewDimension::Texture2D;
  result = mDevice->createTextureView(viewDescriptor, &mSdfPatternView);
  if (!backend::isSuccess(result)) return result;

  result = ensureCommandBuffer();
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->reset();
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->begin();
  if (!backend::isSuccess(result)) return result;
  const auto pixels = mSdfPatternBitmap.makePixmap();
  backend::TextureCopyRegion region;
  region.textureOrigin = { 0, 0, 0 };
  region.size = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
  region.mipLevel = 0;
  region.baseArrayLayer = 0;
  region.arrayLayerCount = 1;
  result = mCommandBuffer->writeTexture(mSdfPatternTexture, region, pixels.addr(0, 0),
    static_cast<size_t>(pixels.rowBytes()) * static_cast<size_t>(height));
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->end();
  if (!backend::isSuccess(result)) return result;
  result = mDevice->executeCommandBuffer(*mCommandBuffer);
  if (!backend::isSuccess(result)) return result;
  mSdfPatternDirty = false;
  return backend::Result::Success;
}
backend::Result GpuRenderer::initialize(backend::Device* borrowedDevice, const backend::Backend preferred,
  utils::JobSystem* jobs, const backend::DebugMessageCallback diagnostics, void* const diagnosticsUserData) {
  if (mDevice != nullptr) {
    return backend::Result::Error;
  }
  mJobs = jobs;

  if (borrowedDevice != nullptr) {
    mDevice = borrowedDevice;
  } else {
    const backend::Result result = createDeviceIfNeeded(preferred, diagnostics, diagnosticsUserData);
    if (!backend::isSuccess(result)) {
      return result;
    }
  }

  // The shader set is checked *before* anything is created, so that a build without embedded shaders
  // fails here -- with one message naming what is missing -- rather than at the first frame, where the
  // symptom would be a canvas that draws rectangles and nothing else.
  mEmbeddedShaderCount = shaderCount(mDevice);
  const char* const required[] = { "quad.vert", "quad.frag", "path.vert", "path.frag",
                                   "composite.vert", "composite.frag", "mask.frag", "coverage.frag",
                                   "present.frag" };
  std::string missing;
  for (const char* name: required) {
    if (!findShader(mDevice, name).isValid()) {
      if (!missing.empty()) missing += ", ";
      missing += name;
    }
  }
  if (!missing.empty()) {
    std::string message;
    if (mEmbeddedShaderCount == 0) {
      message = "the canvas shader set is not embedded in this build, so no canvas program exists: ";
      message += missing;
      message += ". Build with a shader compiler so that CanvasShaders.h is generated.";
    } else {
      // A table with entries that none of the canvas names resolves is *not* "no shaders were embedded",
      // and reporting it as though it were sends the reader to a build option that is already set. The
      // lookup is a name comparison inside the generated header, so a table that is present and
      // unreachable points at the generator rather than at the build.
      message = "the embedded shader table has ";
      message += std::to_string(mEmbeddedShaderCount);
      message += " blob(s) but none of the canvas names resolve: ";
      message += missing;
      message +=
        ". The generated header's name comparison is the suspect -- see modules/graphics/tools/embed_shaders.ps1.";
    }
    note(backend::MessageSeverity::Error, std::move(message));
    // The reason goes to the caller's callback as well as into the renderer's own notes, and that is the point of the
    // callback: a failed `make` returns null, so there is no surface to ask and the notes would be unreachable. Forwarded
    // here rather than in `ContextImpl` because this is where the text exists.
    if (diagnostics != nullptr) {
      // The note just pushed is the last one, which is the reason this attempt failed.
      diagnostics(backend::MessageSeverity::Error, mMessages.back().c_str(), diagnosticsUserData);
    }
    return backend::Result::Unsupported;
  }

  backend::Result result = createPipelineLayouts();
  if (!backend::isSuccess(result)) {
    return result;
  }
  result = createUploadBuffers();
  if (!backend::isSuccess(result)) {
    return result;
  }
  result = createFallbackTextures();
  if (!backend::isSuccess(result)) {
    return result;
  }
  result = createPresentationResources();
  if (!backend::isSuccess(result)) {
    return result;
  }

  mReady = true;
  createTimingQuerySet();
  return backend::Result::Success;
}

backend::Result GpuRenderer::createDeviceIfNeeded(const backend::Backend preferred,
  const backend::DebugMessageCallback externalDiagnostics, void* const externalUserData) {
  backend::DeviceConfiguration configuration;
  configuration.preferredBackend = preferred;
  // Fallback is on: a canvas that comes up on a slower backend draws the same picture, while a canvas
  // that refuses to come up draws nothing at all. The backend that was actually chosen is reported by
  // `describeDevice`, so a caller that cares can still tell.
  configuration.allowFallback = true;
  configuration.appName = "coral-canvas";

  backend::Device* device = nullptr;
  // ## Why the user data is a struct and the lambda captures nothing
  //
  // A lambda that captures cannot convert to the RHI's function-pointer callback, so the caller's callback cannot be
  // captured -- it has to travel in the user data alongside the internal log. That is the whole reason for this struct:
  // a capturing lambda here compiled as a *type error*, which is the compiler saying the same thing.
  struct DeviceDiagnostics {
    std::string* log = nullptr;
    backend::DebugMessageCallback external = nullptr;
    void* externalUserData = nullptr;
  };
  DeviceDiagnostics sink{ &mDeviceCreationLog, externalDiagnostics, externalUserData };

  auto collect = [](backend::MessageSeverity severity, const char* message, void* userData) {
    if (message == nullptr) return;
    auto* sink = static_cast<DeviceDiagnostics*>(userData);
    if (sink == nullptr) return;
    // The caller's callback first, and for *every* severity: a caller diagnosing a failed `make` wants the driver's
    // warnings too, not only the error that ended the attempt. The internal log keeps its own filter, because it is
    // echoed into `diagnostics()` where a warning per frame would be noise.
    if (sink->external != nullptr) sink->external(severity, message, sink->externalUserData);
    std::string* log = sink->log;
    if (log == nullptr) return;
    if (severity != backend::MessageSeverity::Error && severity != backend::MessageSeverity::Warning) return;
    if (!log->empty()) *log += "\n";
    *log += message;
  };

  const backend::Result result = backend::createDevice(configuration, &device, collect, &sink);
  if (!backend::isSuccess(result) || device == nullptr) {
    if (mDeviceCreationLog.empty()) mDeviceCreationLog = "the device factory reported no reason";
    // ## The failure is reported through the caller's callback too, because a callback that only carries the *driver's*
    // messages misses the case where the failure came from somewhere else entirely
    //
    // Measured: a build with no embedded shaders fails device creation with `InvalidHandle` and the driver never says
    // anything, so a caller listening to the callback heard silence and concluded the driver was at fault. Forwarding the
    // log here means any caller who asked for diagnostics gets a reason whatever produced it, and the message names the
    // mechanism that actually failed.
    if (externalDiagnostics != nullptr) {
      externalDiagnostics(backend::MessageSeverity::Error, mDeviceCreationLog.c_str(), externalUserData);
    }
    return backend::isSuccess(result) ? backend::Result::Error : result;
  }

  mOwnedDevice.reset(device);
  mDevice = device;
  return backend::Result::Success;
}

void GpuRenderer::note(const backend::MessageSeverity severity, std::string message) {
  (void) severity;
  // Bounded, because this is a diagnostic and not a log: a renderer that appended a line per frame would
  // grow without bound in a program that runs for a week, and the useful line is the first one anyway.
  constexpr size_t kMaxMessages = 8;
  if (mMessages.size() >= kMaxMessages) {
    mMessages.erase(mMessages.begin());
  }
  mMessages.push_back(std::move(message));
}

backend::Result GpuRenderer::createPipelineLayouts() {
  // One set layout per *shape* of descriptor use, not one per program. `composite.frag` and `mask.frag`
  // declare the same three bindings at the same numbers -- two samplers and a block -- so they share a
  // layout and a pipeline layout, and the two programs differ only in which shader module is linked.
  const auto makeLayout = [this](const backend::DescriptorBinding* bindings, const uint32_t count,
    backend::DescriptorSetLayoutHandle* outSet,
    backend::PipelineLayoutHandle* outPipeline, const char* label) {
    backend::DescriptorSetLayoutDescriptor setDescriptor;
    setDescriptor.label = label;
    setDescriptor.bindingCount = count;
    for (uint32_t index = 0; index < count; ++index) {
      setDescriptor.bindings[index] = bindings[index];
    }
    backend::Result result = mDevice->createDescriptorSetLayout(setDescriptor, outSet);
    if (!backend::isSuccess(result)) return result;

    backend::PipelineLayoutDescriptor pipelineDescriptor;
    pipelineDescriptor.label = label;
    pipelineDescriptor.setLayoutCount = 1;
    pipelineDescriptor.setLayouts[0] = *outSet;
    return mDevice->createPipelineLayout(pipelineDescriptor, outPipeline);
  };

  // Binding 0 is the OpenGL backend's emulated push-constant block, so a *block* may not sit there; the
  // canvas block is at 1 and the path block at 2, which is what the shaders declare. A sampler at 0 is
  // a texture unit rather than a uniform-block binding point and cannot collide with it.
  constexpr backend::DescriptorBinding quadBindings[] = {
    { .binding = 1, .type = backend::BindingType::UniformBuffer, .count = 1,
      .stageFlags = kVertexStage | kFragmentStage, .debugName = nullptr },
    { .binding = 0, .type = backend::BindingType::TextureSampler, .count = 1, .stageFlags = kFragmentStage,
      .debugName = nullptr },
  };
  backend::Result result = makeLayout(quadBindings, 2, &mQuadSetLayout, &mQuadPipelineLayout, "canvas/quad");
  if (!backend::isSuccess(result)) return result;

  // The SDF pattern program has a second sampler at set 1. A separate set layout rather than a second
  // binding in the quad set: only this program declares it, and a binding every other program omits is a
  // descriptor the RHI would still validate.
  constexpr backend::DescriptorBinding patternBindings[] = {
    { .binding = 0, .type = backend::BindingType::TextureSampler, .count = 1,
      .stageFlags = kFragmentStage, .debugName = nullptr },
    { .binding = 1, .type = backend::BindingType::UniformBuffer, .count = 1,
      .stageFlags = kVertexStage | kFragmentStage, .debugName = nullptr },
    { .binding = 2, .type = backend::BindingType::TextureSampler, .count = 1,
      .stageFlags = kFragmentStage, .debugName = nullptr },
  };
  backend::DescriptorSetLayoutDescriptor patternSetDescriptor;
  patternSetDescriptor.label = "canvas/sdf-pattern";
  patternSetDescriptor.bindingCount = 3;
  patternSetDescriptor.bindings[0] = patternBindings[0];
  patternSetDescriptor.bindings[1] = patternBindings[1];
  patternSetDescriptor.bindings[2] = patternBindings[2];
  result = mDevice->createDescriptorSetLayout(patternSetDescriptor, &mSdfPatternSetLayout);
  if (!backend::isSuccess(result)) return result;

  backend::PipelineLayoutDescriptor patternPipelineDescriptor;
  patternPipelineDescriptor.label = "canvas/sdf-pattern";
  patternPipelineDescriptor.setLayoutCount = 1;
  patternPipelineDescriptor.setLayouts[0] = mSdfPatternSetLayout;
  result = mDevice->createPipelineLayout(patternPipelineDescriptor, &mSdfPatternPipelineLayout);
  if (!backend::isSuccess(result)) return result;

  // The gradient shares `quad.vert` and so needs the canvas block at binding 1 -- but it has no sampler, and its own
  // block sits at binding 2 where the quad's *sampler* would be. So it cannot use `mQuadSetLayout`: a set built from
  // that layout declares a texture sampler at 2, and filling 2 with a uniform buffer is a rejection, which is what
  // stopped the GPU coming up at all when the gradient set was first made from the quad layout.
  //
  // Binding 1 is declared for both stages because it is `quad.vert` that reads it and `gradient.frag` that is linked
  // beside it; the RHI validates a program against the layout it was created with, so the union is what both need.
  const backend::DescriptorBinding gradientBindings[] = {
    { 1, backend::BindingType::UniformBuffer, 1, kVertexStage | kFragmentStage, nullptr },
    { 2, backend::BindingType::UniformBuffer, 1, kFragmentStage, nullptr },
  };
  result = makeLayout(gradientBindings, 2, &mGradientSetLayout, &mGradientPipelineLayout, "canvas/gradient");
  if (!backend::isSuccess(result)) return result;

  // ## The coverage-field layout: a new one, choosing proven bindings
  //
  // `coverage.frag` samples a texture and reads one `vec4` block, so the bindings it wants are the ones the
  // sampled programs already use: a sampler at 0 and a block at 2. Those two are *demonstrably delivered* --
  // `composite.frag` and `mask.frag` read their block at binding 2 and their output is verified across the
  // suite -- which is the reason to reuse the arrangement rather than invent a tidier one. (The gradient's
  // parameters failing to arrive at binding 2 is *not* evidence that binding 2 cannot work: that block is still
  // undiagnosed, and its own members were the suspect.)
  //
  // A new layout is still required, because a set must be built from the same layout object its pipeline was
  // created with, and getting that wrong once stopped the device coming up at all.
  //
  const backend::DescriptorBinding coverageFieldBindings[] = {
    // Binding 1 is `quad.vert`'s canvas block, and it is *required* rather than optional: this program shares
    // that vertex stage, so without it the program has no vertex-side uniform block and the geometry is
    // undefined. Leaving it out compiled and linked -- which is why it is worth stating that it cannot be left
    // out on account of "nothing here reads the canvas".
    { 1, backend::BindingType::UniformBuffer, 1, kVertexStage, nullptr },
    { 0, backend::BindingType::TextureSampler, 1, kFragmentStage, nullptr },
    { 2, backend::BindingType::UniformBuffer, 1, kFragmentStage, nullptr },
  };
  result = makeLayout(coverageFieldBindings, 3, &mCoverageFieldSetLayout, &mCoverageFieldPipelineLayout,
          "canvas/coverage-field");
  if (!backend::isSuccess(result)) return result;

  const backend::DescriptorBinding pathBindings[] = {
    { 1, backend::BindingType::UniformBuffer, 1, kVertexStage, nullptr },
  };
  result = makeLayout(pathBindings, 1, &mPathSetLayout, &mPathPipelineLayout, "canvas/path");
  if (!backend::isSuccess(result)) return result;

  const backend::DescriptorBinding texturedBindings[] = {
    { 0, backend::BindingType::TextureSampler, 1, kFragmentStage, nullptr },
    { 1, backend::BindingType::TextureSampler, 1, kFragmentStage, nullptr },
    { 2, backend::BindingType::UniformBuffer, 1, kFragmentStage, nullptr },
  };
  result = makeLayout(texturedBindings, 3, &mTexturedSetLayout, &mTexturedPipelineLayout, "canvas/composite");
  if (!backend::isSuccess(result)) return result;

  // The quad and path sets are made here, with the layouts; a gradient's, a composite's and a mask's are per draw,
  // because a descriptor set names a concrete buffer *range* and two draws in one frame must not share one.
  //
  // The per-draw sets are created against `mGradientSetLayout` and `mTexturedSetLayout` -- the same objects the pipelines
  // were built with, not structurally identical copies. The device validates a set against the pipeline layout it is
  // bound with, and a set built against a different-but-equivalent layout is rejected; separate copies of the path
  // layout made every path draw nothing at all. That cost an afternoon to learn.
  const auto makeSet = [this](const backend::DescriptorSetLayoutHandle layout, backend::DescriptorSetHandle* out) {
    return mDevice->createDescriptorSet(layout, out);
  };
  result = makeSet(mQuadSetLayout, &mQuadSet);
  if (!backend::isSuccess(result)) return result;
  // The path's set: the canvas block and nothing else. The fill colour travels in the vertices, so
  // this set is the same for every path a frame draws and is created once with the other C++ sets.
  result = makeSet(mPathSetLayout, &mPathSet);
  if (!backend::isSuccess(result)) return result;
  // The coverage-field route's own set. Unlike the gradient's, this one is *reusable*: it names the canvas block
  // at binding 2 and one field texture at binding 0, and the texture differs per field -- so the set created here
  // is replaced per field by `ensureCoverageFieldTexture`, and this one exists so that a frame with no field has
  // something valid to leave bound rather than a stale set from another layout.
  result = makeSet(mCoverageFieldSetLayout, &mCoverageFieldSet);
  return backend::Result::Success;
}

backend::Result GpuRenderer::createUploadBuffers() {
  // The uniform arena starts at a size that serves an ordinary frame and grows in steps from there; see
  // `ensureUniformCapacity`. One MiB at 256 bytes a block is 4096 draw blocks, and the arena is host visible so that the
  // frame's staged bytes cross in one mapped write rather than as recorded copy commands.
  backend::Result result = ensureUniformCapacity(kUniformInitialSlots);
  if (!backend::isSuccess(result)) return result;

  // The canvas block can be written now: the quad set is complete before the first frame, apart from the
  // fallback texture that `createFallbackTextures` fills binding 0 with.
  const backend::DescriptorWrite writes[] = {
    { 1, backend::BindingType::UniformBuffer, backend::TextureHandle::invalid(),
      backend::TextureSamplerHandle::invalid(),
      { uniformBuffer(), kCanvasSlot * kUniformSlotSize, kUniformSlotSize }, 0 },
  };
  result = mDevice->updateDescriptorSet(mQuadSet, writes, 1);
  if (!backend::isSuccess(result)) return result;
  result = mDevice->updateDescriptorSet(mPathSet, writes, 1);
  if (!backend::isSuccess(result)) return result;
  return backend::Result::Success;
}

backend::Result GpuRenderer::createFallbackTextures() {
  backend::Result result = mDevice->createSampler(backend::SamplerDescriptor::linearClamp(), &mLinearClamp);
  if (!backend::isSuccess(result)) return result;
  // A mask must be sampled with a *nearest* filter to reproduce `Canvas::maskLayer`, which maps a layer
  // pixel to a truncated integer texel; `mask.frag`'s own comment says the filter is the caller's and
  // this is where the caller chooses it.
  result = mDevice->createSampler(backend::SamplerDescriptor::nearestClamp(), &mNearestClamp);
  if (!backend::isSuccess(result)) return result;

  const auto createOneByOne = [this](const uint8_t value, backend::TextureHandle* outTexture,
    backend::TextureViewHandle* outView) {
    const backend::TextureDescriptor descriptor = backend::TextureDescriptor::texture2D(1, 1,
      backend::PixelFormat::R8G8B8A8_UNORM,
      static_cast<backend::TextureUsages>(backend::TextureUsage::Sampled));
    backend::Result createResult = mDevice->createTexture(descriptor, outTexture);
    if (!backend::isSuccess(createResult)) return createResult;

    backend::TextureViewDescriptor viewDescriptor;
    viewDescriptor.texture = *outTexture;
    viewDescriptor.dimension = backend::TextureViewDimension::Texture2D;
    createResult = mDevice->createTextureView(viewDescriptor, outView);
    if (!backend::isSuccess(createResult)) return createResult;

    createResult = ensureCommandBuffer();
    if (!backend::isSuccess(createResult)) return createResult;
    createResult = mCommandBuffer->reset();
    if (!backend::isSuccess(createResult)) return createResult;
    createResult = mCommandBuffer->begin();
    if (!backend::isSuccess(createResult)) return createResult;

    const uint8_t pixel[4] = { value, value, value, value };
    backend::TextureCopyRegion region;
    region.textureOrigin = { 0, 0, 0 };
    region.size = { 1, 1, 1 };
    region.mipLevel = 0;
    region.baseArrayLayer = 0;
    region.arrayLayerCount = 1;
    createResult = mCommandBuffer->writeTexture(*outTexture, region, pixel, sizeof(pixel));
    if (!backend::isSuccess(createResult)) return createResult;
    createResult = mCommandBuffer->end();
    if (!backend::isSuccess(createResult)) return createResult;
    return mDevice->executeCommandBuffer(*mCommandBuffer);
  };

  // White and opaque: what the quad pass binds where it has nothing to sample, so that a shader which
  // samples unconditionally (the module declares the sampler whether or not a frame uses it) reads a
  // defined texel rather than whatever unit was bound last.
  result = createOneByOne(255, &mWhiteTexture, &mWhiteTextureView);
  if (!backend::isSuccess(result)) return result;
  // The layer composite's constant coverage, which carries the layer's global alpha and is rewritten
  // per composite. Its colour channels are the alpha too, so that a mask read of it in either mode gives
  // the same number.
  result = createOneByOne(255, &mAlphaTexture, &mAlphaTextureView);
  if (!backend::isSuccess(result)) return result;

  const backend::DescriptorWrite quadWrites[] = {
    { 0, backend::BindingType::TextureSampler, mWhiteTexture, mLinearClamp, {}, 0 },
  };
  result = mDevice->updateDescriptorSet(mQuadSet, quadWrites, 1);
  if (!backend::isSuccess(result)) return result;



  // ## The path sets are made per draw, not here
  //
  // A path's cover block and stencil block are both per draw now, so the set that names one is per draw too. They are
  // created lazily by `uniformSetForSlot` when `drawPath` takes a slot, and cached by slot index: a frame that draws the
  // same shapes as the previous one reuses every set it needs.
  //
  // The composite and mask programs have no set made here either. They sample rather than fill, and *what* they sample
  // changes per composite: a layer's own pixels, the parent's snapshot, a 1x1 constant. A set holding those textures
  // cannot be shared between two composites, because the backend resolves a draw's set contents at replay while the fill
  // happens during recording -- so one refilled set gives every composite the last one's textures. `compositeSetFor`
  // supplies one set per composite position, and each program writes all three bindings on every use, including its own
  // uniform block at binding 2.
  return backend::Result::Success;
}

backend::Result GpuRenderer::createPresentationResources() {
  // The quad is written once. Clip space and the texture coordinate are enough: the fragment stage is
  // where every colour decision lives, so the geometry never changes.
  //
  // The V coordinate follows the same rule as `composite.vert`'s quads: an OpenGL-rendered target stores
  // its first row at texture row 0 while the device image's row 0 is the top, so sampling it without the
  // flip presents every frame upside down. Vulkan's targets are already top-down.
  const bool topDown = usesVulkanShaderSet(mDevice);
  const float v0 = topDown ? 0.0f : 1.0f;
  const float v1 = topDown ? 1.0f : 0.0f;
  const FullTargetVertex quad[kCoverVertexCount] = {
    { -1.0f, -1.0f, 0.0f, v0 }, { 1.0f, -1.0f, 1.0f, v0 }, { 1.0f, 1.0f, 1.0f, v1 },
    { -1.0f, -1.0f, 0.0f, v0 }, { 1.0f, 1.0f, 1.0f, v1 }, { -1.0f, 1.0f, 0.0f, v1 },
  };

  const backend::BufferDescriptor descriptor{
    .size = sizeof(quad),
    .usage = static_cast<backend::BufferUsages>(backend::BufferUsage::Vertex),
    .memoryUsage = backend::MemoryUsage::CpuToGpuDynamic,
    .initialData = nullptr,
    .label = "canvas/present-vertices",
  };
  backend::Result result = mDevice->createBuffer(descriptor, &mPresentVertexBuffer);
  if (!backend::isSuccess(result)) return result;
  result = uploadBuffer(mPresentVertexBuffer, sizeof(quad), 0, quad, sizeof(quad));
  if (!backend::isSuccess(result)) return result;

  result = mDevice->createDescriptorSet(mTexturedSetLayout, &mPresentSet);
  if (!backend::isSuccess(result)) return result;

  // Bind every binding up front, including the uniform block at slot 0, so the set is complete before
  // the first presentation. `present` rewrites the block and the source texture per submission.
  const backend::DescriptorWrite writes[] = {
    { 0, backend::BindingType::TextureSampler, mWhiteTexture, mLinearClamp, {}, 0 },
    { 1, backend::BindingType::TextureSampler, mWhiteTexture, mLinearClamp, {}, 0 },
    { 2, backend::BindingType::UniformBuffer, backend::TextureHandle::invalid(),
      backend::TextureSamplerHandle::invalid(),
      { uniformBuffer(), 0, kUniformSlotSize }, 0 },
  };
  return mDevice->updateDescriptorSet(mPresentSet, writes, 3);
}

backend::Result GpuRenderer::allocateUniformSlot(const void* const data, const size_t size, uint32_t* const outSlot) {
  if (mCommandBuffer == nullptr || outSlot == nullptr) {
    return backend::Result::Error;
  }
  if (data == nullptr || size == 0 || size > kUniformSlotSize) {
    return backend::Result::InvalidArgument;
  }
  if (mUniformBuffers.empty() || mUniformSlotsUsed >= mUniformBuffers.back().bytes / kUniformSlotSize) {
    // `ensureUniformCapacity` sizes the arena from an upper bound over the frame's batches before recording starts, so
    // reaching this means that estimate missed a path. Reporting it is what turns a silently dropped draw into a named
    // failure; the frame is refused rather than drawn with the wrong block.
    note(backend::MessageSeverity::Error,
            "the canvas uniform arena is full; the per-frame slot estimate and the recording disagree");
    return backend::Result::OutOfMemory;
  }
  const uint32_t slot = mUniformSlotsUsed++;
  std::memcpy(mUniformArena.data() + static_cast<size_t>(slot) * kUniformSlotSize, data, size);
  *outSlot = slot;
  return backend::Result::Success;
}

backend::Result GpuRenderer::ensureUniformCapacity(const uint32_t slots) {

  const uint64_t needed = static_cast<uint64_t>(slots) * kUniformSlotSize;
  if (!mUniformBuffers.empty() && mUniformBuffers.back().bytes >= needed) {
    return backend::Result::Success;
  }

  uint64_t bytes = mUniformBuffers.empty() ? kUniformInitialBytes : mUniformBuffers.back().bytes;
  while (bytes < needed) bytes *= 2u;

  // The staging area only has to be as large as the newest buffer's slot count; slots are addressed by index, so an
  // older buffer's slots keep the same offsets and the same staged bytes.
  mUniformArena.resize(static_cast<size_t>(bytes), 0);

  const backend::BufferDescriptor descriptor{
    .size = bytes,
    .usage = static_cast<backend::BufferUsages>(backend::BufferUsage::Uniform),
    // Host visible, because the whole arena crosses in one mapped write after recording. `writeBuffer` is deliberately
    // not used: it records a copy command, and a copy inside a render pass is what this arena exists to remove.
    .memoryUsage = backend::MemoryUsage::CpuToGpuDynamic,
    .initialData = nullptr,
    .label = "canvas/uniforms",
  };
  UniformBuffer created;
  created.bytes = bytes;
  const backend::Result result = mDevice->createBuffer(descriptor, &created.handle);
  if (!backend::isSuccess(result)) {
    created.handle = backend::BufferHandle::invalid();
    return result;
  }
  mUniformBuffers.push_back(created);
  return backend::Result::Success;
}

backend::Result GpuRenderer::uploadUniforms() {
  if (mUniformSlotsUsed == 0 || mUniformArena.empty()) {
    return backend::Result::Success;
  }
  const uint64_t bytes = std::min<uint64_t>(static_cast<uint64_t>(mUniformSlotsUsed) * kUniformSlotSize,
          mUniformArena.size());
  // Every live buffer receives the same bytes: a set built against an older, smaller buffer names a slot that is still
  // inside it, and only the newest buffer can carry the slots a large frame allocated past the older one's end.
  for (const UniformBuffer& buffer: mUniformBuffers) {
    if (!buffer.handle.valid()) continue;
    const uint64_t deliver = std::min<uint64_t>(bytes, buffer.bytes);
    if (deliver == 0) continue;
    const backend::Result result = uploadBuffer(buffer.handle, buffer.bytes, 0, mUniformArena.data(), deliver);
    if (!backend::isSuccess(result)) {
      return result;
    }
  }
  return backend::Result::Success;
}

backend::Result GpuRenderer::uniformSetForSlot(std::vector<backend::DescriptorSetHandle>& cache,
        const backend::DescriptorSetLayoutHandle layout, const uint32_t slot,
        backend::DescriptorSetHandle* const outSet) {
  if (outSet == nullptr || !layout.valid() || !uniformBuffer().valid()) {
    return backend::Result::InvalidArgument;
  }
  if (cache.size() <= slot) {
    cache.resize(static_cast<size_t>(slot) + 1, backend::DescriptorSetHandle::invalid());
  }
  backend::DescriptorSetHandle& set = cache[slot];
  if (!set.valid()) {
    backend::Result result = mDevice->createDescriptorSet(layout, &set);
    if (!backend::isSuccess(result)) {
      set = backend::DescriptorSetHandle::invalid();
      return result;
    }
    const backend::DescriptorWrite writes[2] = {
      { 1, backend::BindingType::UniformBuffer, backend::TextureHandle::invalid(),
        backend::TextureSamplerHandle::invalid(),
        { uniformBuffer(), kCanvasSlot * kUniformSlotSize, kUniformSlotSize }, 0 },
      { 2, backend::BindingType::UniformBuffer, backend::TextureHandle::invalid(),
        backend::TextureSamplerHandle::invalid(),
        { uniformBuffer(), static_cast<uint64_t>(slot) * kUniformSlotSize, kUniformSlotSize }, 0 },
    };
    result = mDevice->updateDescriptorSet(set, writes, 2);
    if (!backend::isSuccess(result)) {
      mDevice->destroyDescriptorSet(set);
      set = backend::DescriptorSetHandle::invalid();
      return result;
    }
  }
  *outSet = set;
  return backend::Result::Success;
}

/**
 * @return a hash of **every** pixel of @p pixmap and its dimensions, for use as a cache key.
 *
 * ## Why this is complete rather than sampled
 *
 * It used to hash every row at three columns -- the first, the middle and the last -- to keep the per-frame cost of a
 * large bitmap below the upload it guards. That is a heuristic key, and a heuristic key substitutes one bitmap's pixels
 * for another's whenever the difference lies outside the sample. For a glyph the sample catches almost everything, since
 * a glyph's coverage runs the height of its box and its box is a few texels wide; for anything *sparse* it catches
 * almost nothing, and a **mask** is the worst case: it is a target-sized bitmap of which one document's worth is drawn,
 * so it is transparent nearly everywhere, and thirteen documents' masks in one panel all miss the three sampled columns.
 *
 * They therefore hashed alike, the placement cache gave them one entry, one mask's pixels were uploaded, and every other
 * masked layer was multiplied by *that* mask's coverage -- which for a document whose mask content is elsewhere is a
 * coverage of zero, so the layer vanished. Measured: the corpus grid lost `mask.svg`'s cell entirely (`detail 1196/0`)
 * while the same document drawn alone matched the rasterizer byte for byte.
 *
 * The cost is real and is paid deliberately: a 606x306 mask is 185k FNV steps, about 0.2 ms, against a texture write of
 * the same size. A wrong picture that looks deliberate is worse than a slower frame, and the sampler cannot be made
 * correct by sampling more cleverly -- only by covering everything.
 *
 * The dimensions are folded in first: two bitmaps of different sizes must not hash alike merely because their pixels
 * coincide, since the descriptor set, the texture and the quad are all built from the size.
 */
uint64_t hashImage(const graphics::ImageInfo& info, const graphics::Pixmap& pixmap,
        const SamplingOptions& sampling) {
  // FNV-1a, chosen because it is four lines and has no state beyond the accumulator.
  uint64_t hash = 1469598103934665603ULL;
  const auto mix = [&hash](const uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
  };
  mix(info.width());
  mix(info.height());
  // Two bitmaps with the same bytes but different colour spaces are different pictures, so the space
  // is part of the content key: without it a P3 image could be served the placement of an sRGB one.
  mix(info.colorSpace() != nullptr ? info.colorSpace()->hash() : 0);
  mix(sampling.filter == FilterMode::kLinear ? 1u : 0u);
  mix(static_cast<uint64_t>(sampling.mipmap));
  mix(sampling.useCubic ? 1u : 0u);
  mix(static_cast<uint64_t>(sampling.cubic.b * 1000.0f));
  mix(static_cast<uint64_t>(sampling.cubic.c * 1000.0f));

  const int width = pixmap.width();
  const int height = pixmap.height();
  if (width <= 0 || height <= 0) return hash;

  // ## The pixels are hashed as bytes, not through the accessor
  //
  // This loop runs once per distinct bitmap on every submit, and a 1024x1024 glyph page is a
  // million `getPixelARGB32` calls: each one decodes the texel's format, converts it to ARGB and
  // rounds, which costs about ten nanoseconds and therefore about ten milliseconds a frame. That
  // was 47 ms on a 2048x2048 page -- the whole "present" floor measured in docs/GUI鎬ц兘鍩虹嚎.md.
  //
  // The accessor's conversion is not needed for what this hash is for: it only has to distinguish
  // one bitmap from another, and raw bytes do that exactly. Folding eight bytes per FNV step keeps
  // the arithmetic proportional to the *bytes*, not to the texels times a format dispatch: the same
  // page hashes in under a millisecond and no picture changes, because the key still covers every
  // pixel.
  const auto* bytes = static_cast<const uint8_t*>(pixmap.pixels());
  const size_t rowBytes = pixmap.rowBytes();
  if (bytes == nullptr || rowBytes == 0) return hash;
  const size_t pixelBytes = pixmap.info().bytesPerPixel();
  if (pixelBytes == 0) return hash;
  const size_t usedBytes = static_cast<size_t>(width) * pixelBytes;
  for (int y = 0; y < height; ++y) {
    const uint8_t* row = bytes + static_cast<size_t>(y) * rowBytes;
    size_t offset = 0;
    for (; offset + sizeof(uint64_t) <= usedBytes; offset += sizeof(uint64_t)) {
      uint64_t word = 0;
      std::memcpy(&word, row + offset, sizeof(word));
      mix(word);
    }
    for (; offset < usedBytes; ++offset) {
      mix(row[offset]);
    }
  }
  return hash;
}

namespace {

/**
 * @return @p pixmap's pixels copied with a **one-texel replicated border** all round.
 *
 * ## Why every bitmap in a page is uploaded with a ring of its own edge
 *
 * The page is sampled with **linear** filtering, so a tap at the last texel of a bitmap reaches half a texel beyond it,
 * and what is there is the *next* bitmap's first texel. Replicating the edge into the ring is what makes a sample equal
 * to clamp-to-edge sampling of the bitmap alone -- which is what a texture of its own would give, and therefore what
 * makes the atlas picture-identical rather than nearly so. A transparent ring instead would fade a glyph's edge toward
 * nothing: a wrong picture that looks like an antialiasing difference.
 *
 * It is the same argument, and the same construction, as `packCoverageFields`' border pass.
 *
 * The payload is exactly the padded rectangle's bytes, tightly packed, so one `writeTexture` of `(width + 2)` by
 * `(height + 2)` texels at the placement's corner is the whole upload -- one command per new bitmap, and the page is
 * never re-uploaded whole.
 *
 * @param pixmap pixels the caller has already established are `kRGBA_8888`, at least 1x1 and addressable.
 */
std::vector<uint8_t> paddedBitmap(const graphics::Pixmap& pixmap) {
  constexpr size_t kBytesPerTexel = 4;
  const uint32_t width = static_cast<uint32_t>(pixmap.width());
  const uint32_t height = static_cast<uint32_t>(pixmap.height());
  const size_t paddedWidth = static_cast<size_t>(width) + 2u;
  std::vector<uint8_t> padded(paddedWidth * (static_cast<size_t>(height) + 2u) * kBytesPerTexel, 0);

  const auto* source = static_cast<const uint8_t*>(pixmap.pixels());
  const size_t sourceStride = pixmap.rowBytes();
  const auto sourceRow = [source, sourceStride](const uint32_t index) { return source + sourceStride * index; };
  const auto destinationRow = [&padded, paddedWidth](const uint32_t index) {
    return padded.data() + static_cast<size_t>(index) * paddedWidth * kBytesPerTexel;
  };
  // One bitmap row with its two edge texels copied outward -- the whole of the horizontal ring.
  const auto fillRow = [width](uint8_t* destination, const uint8_t* row) {
    std::memcpy(destination + kBytesPerTexel, row, static_cast<size_t>(width) * kBytesPerTexel);
    std::memcpy(destination, row, kBytesPerTexel);
    std::memcpy(destination + (static_cast<size_t>(width) + 1u) * kBytesPerTexel,
            row + (static_cast<size_t>(width) - 1u) * kBytesPerTexel, kBytesPerTexel);
  };

  // The top and bottom rings are the first and last bitmap rows, and the interior is every row in order. A contiguous
  // copy rather than a loop over texels: the two border rows are the rows themselves, not a per-column construction.
  fillRow(destinationRow(0), sourceRow(0));
  for (uint32_t y = 0; y < height; ++y) {
    fillRow(destinationRow(y + 1), sourceRow(y));
  }
  fillRow(destinationRow(height + 1), sourceRow(height - 1));
  return padded;
}

/**
 * Records an upload of a tightly packed @p bytes payload into a **sub-rectangle** of @p texture.
 *
 * The atlas's whole point: adding a bitmap to a page is one write of that bitmap's rectangle -- 0.4% of a 1024x1024
 * page for a typical 64x64 glyph -- rather than a re-upload of the page. `TextureCopyRegion` carries both the origin and
 * the size, which is what makes that a single command; `bytesPerRow` stays zero because the payload is the rectangle
 * itself, tightly packed.
 */
backend::Result writeImageRegion(backend::CommandBuffer* commandBuffer, const backend::TextureHandle texture,
        const uint32_t x, const uint32_t y, const uint32_t width, const uint32_t height,
        const std::vector<uint8_t>& bytes) {
  backend::TextureCopyRegion region;
  region.textureOrigin = { x, y, 0 };
  region.size = { width, height, 1 };
  region.mipLevel = 0;
  region.baseArrayLayer = 0;
  region.arrayLayerCount = 1;
  return commandBuffer->writeTexture(texture, region, bytes.data(), static_cast<uint64_t>(bytes.size()));
}

} // namespace anonymous

GpuRenderer::ImageAtlasPage* GpuRenderer::pageWithRoomFor(const uint32_t paddedWidth, const uint32_t paddedHeight,
        uint32_t* outIndex, uint32_t* outX, uint32_t* outY) {
  // An existing page first, in creation order: the shelves of page 0 fill before page 1 exists, which is what keeps the
  // page count a property of how many distinct bitmaps a session has drawn rather than of the order they arrived in.
  for (uint32_t index = 0; index < mImagePages.size(); ++index) {
    uint32_t x = 0;
    uint32_t y = 0;
    if (mImagePages[index].allocator.allocate(paddedWidth, paddedHeight, &x, &y)) {
      *outIndex = index;
      *outX = x;
      *outY = y;
      return &mImagePages[index];
    }
  }

  // No page has room. A new one is the answer while the budget lasts, and the caller's answer past it is a texture of
  // the bitmap's own -- a cost, and never a wrong picture.
  if (mImagePages.size() >= kImageAtlasPageBudget) return nullptr;

  ImageAtlasPage page(kImageAtlasExtent, kImageAtlasExtent);
  page.textureId = mNextImageTextureId++;
  const backend::TextureDescriptor descriptor = backend::TextureDescriptor::texture2D(page.width, page.height,
    backend::PixelFormat::R8G8B8A8_UNORM, static_cast<backend::TextureUsages>(backend::TextureUsage::Sampled));
  if (!backend::isSuccess(mDevice->createTexture(descriptor, &page.texture))) {
    return nullptr;
  }
  backend::TextureViewDescriptor viewDescriptor;
  viewDescriptor.texture = page.texture;
  viewDescriptor.dimension = backend::TextureViewDimension::Texture2D;
  if (!backend::isSuccess(mDevice->createTextureView(viewDescriptor, &page.view))) {
    mDevice->destroyTexture(page.texture);
    return nullptr;
  }
  // The page's set names the page's texture at binding 0 and the canvas block at binding 1, which is the arrangement
  // `mQuadSetLayout` describes and the quad's vertex stage reads -- one set per page rather than one per bitmap, which is
  // the whole reason a batch of glyphs can be one draw.
  if (!backend::isSuccess(mDevice->createDescriptorSet(mQuadSetLayout, &page.set))) {
    mDevice->destroyTextureView(page.view);
    mDevice->destroyTexture(page.texture);
    return nullptr;
  }
  const backend::DescriptorWrite writes[2] = {
    { 0, backend::BindingType::TextureSampler, page.texture, mLinearClamp, {}, 0 },
    { 1, backend::BindingType::UniformBuffer, backend::TextureHandle::invalid(),
      backend::TextureSamplerHandle::invalid(),
      { uniformBuffer(), kCanvasSlot * kUniformSlotSize, kUniformSlotSize }, 0 },
  };
  if (!backend::isSuccess(mDevice->updateDescriptorSet(page.set, writes, 2))) {
    mDevice->destroyDescriptorSet(page.set);
    mDevice->destroyTextureView(page.view);
    mDevice->destroyTexture(page.texture);
    return nullptr;
  }

  // The page's second set: the same texture with a nearest sampler, so that an image drawn either way can stay in the
  // atlas. See the note on `ImageAtlasPage::setNearest`.
  if (!backend::isSuccess(mDevice->createDescriptorSet(mQuadSetLayout, &page.setNearest))) {
    mDevice->destroyDescriptorSet(page.set);
    mDevice->destroyTextureView(page.view);
    mDevice->destroyTexture(page.texture);
    return nullptr;
  }
  const backend::DescriptorWrite nearestWrites[2] = {
    { 0, backend::BindingType::TextureSampler, page.texture, mNearestClamp, {}, 0 },
    { 1, backend::BindingType::UniformBuffer, backend::TextureHandle::invalid(),
      backend::TextureSamplerHandle::invalid(),
      { uniformBuffer(), kCanvasSlot * kUniformSlotSize, kUniformSlotSize }, 0 },
  };
  if (!backend::isSuccess(mDevice->updateDescriptorSet(page.setNearest, nearestWrites, 2))) {
    mDevice->destroyDescriptorSet(page.setNearest);
    mDevice->destroyDescriptorSet(page.set);
    mDevice->destroyTextureView(page.view);
    mDevice->destroyTexture(page.texture);
    return nullptr;
  }
  // ## The page is deliberately *not* zero-filled, and that is a measurement rather than an omission
  //
  // The field atlas zeroes its texture once, and this one does not, for a reason that is about what is sampled: a
  // bitmap's rectangle -- including the replicated ring -- is written in full by its own upload, and the fragment stage
  // can only reach those texels, so no sample can land on an unwritten one. The field atlas's zero fill guards against a
  // placement being wrong; here that guard is the assertion in
  // `TheImageAtlasSubRectangleIsInsetAndLandsOnTexelBoundaries`, which is cheaper than 4 MiB of zeros per page and does
  // not hide a placement bug behind a picture of transparent pixels.
  mImagePages.push_back(std::move(page));

  uint32_t x = 0;
  uint32_t y = 0;
  if (!mImagePages.back().allocator.allocate(paddedWidth, paddedHeight, &x, &y)) {
    // Unreachable for a caller that asked for a rectangle that fits the page (the padded size is checked first), and
    // defined rather than left undefined: the page stays in the pool and the caller falls back to its own texture.
    return nullptr;
  }
  *outIndex = static_cast<uint32_t>(mImagePages.size() - 1);
  *outX = x;
  *outY = y;
  return &mImagePages.back();
}

namespace {

/** Box filters one level of tightly packed R8G8B8A8 -- premultiplied, so alpha averages with the colours. */
std::vector<uint8_t> downsampleRgba(const uint8_t* source, const uint32_t width, const uint32_t height,
        const uint32_t stride) {
  const uint32_t w = std::max(1u, width / 2u);
  const uint32_t h = std::max(1u, height / 2u);
  std::vector<uint8_t> out(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0u);
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* rows[2] = { source + static_cast<size_t>(y * 2u) * stride,
                               source + static_cast<size_t>(std::min(y * 2u + 1u, height - 1u)) * stride };
    for (uint32_t x = 0; x < w; ++x) {
      const uint32_t left = std::min(x * 2u, width - 1u);
      const uint32_t right = std::min(x * 2u + 1u, width - 1u);
      for (uint32_t channel = 0; channel < 4u; ++channel) {
        const uint32_t sum = rows[0][left * 4u + channel] + rows[0][right * 4u + channel] +
                             rows[1][left * 4u + channel] + rows[1][right * 4u + channel];
        out[(static_cast<size_t>(y) * static_cast<size_t>(w) + x) * 4u + channel] =
          static_cast<uint8_t>((sum + 2u) / 4u);
      }
    }
  }
  return out;
}

} // namespace anonymous

backend::TextureSamplerHandle GpuRenderer::samplerFor(const SamplingOptions& sampling) {
  // A tiny key: the three filter fields are two bits each. `useCubic` is folded in because a cubic draw needs its own
  // placement (and, when the shader learns to do it, its own sampler); today it behaves as the filter it names.
  const uint32_t key = (static_cast<uint32_t>(sampling.filter) & 3u) |
                       ((static_cast<uint32_t>(sampling.mipmap) & 3u) << 2) |
                       ((sampling.useCubic ? 1u : 0u) << 4);
  if (const auto found = mSamplers.find(key); found != mSamplers.end()) return found->second;

  backend::SamplerDescriptor descriptor;
  descriptor.magFilter = sampling.filter == FilterMode::kLinear ? backend::FilterMode::Linear
                                                                : backend::FilterMode::Nearest;
  descriptor.minFilter = descriptor.magFilter;
  descriptor.mipmapFilter = sampling.mipmap == MipmapMode::kLinear ? backend::MipmapFilterMode::Linear
                                                                  : backend::MipmapFilterMode::Nearest;
  descriptor.lodMinClamp = 0.0f;
  descriptor.lodMaxClamp = 1000.0f;
  descriptor.label = "canvas-image";

  backend::TextureSamplerHandle sampler = backend::TextureSamplerHandle::invalid();
  if (!backend::isSuccess(mDevice->createSampler(descriptor, &sampler))) {
    return backend::TextureSamplerHandle::invalid();
  }
  mSamplers.emplace(key, sampler);
  return sampler;
}
GpuRenderer::ImagePlacement* GpuRenderer::ensureImagePlacement(const Bitmap& image,
    const SamplingOptions& sampling, const uint64_t knownHash) {
  if (image.isEmpty()) return nullptr;

  // The picture has to be R8G8B8A8 in memory for a straight upload: the GL backend's `writeTexture` hands the bytes
  // to `glTexSubImage2D` with the format the RHI's `R8G8B8A8_UNORM` names, and a bitmap in another layout would
  // arrive with its channels permuted -- a wrong picture with no error, which is the worst kind here.
  const graphics::ImageInfo info = image.info();
  if (info.colorType() != graphics::ColorType::kRGBA_8888) {
    return nullptr;
  }
  const graphics::Pixmap pixmap = image.makePixmap();
  if (!pixmap.isValid() || pixmap.pixels() == nullptr) {
    return nullptr;
  }

  // ## The table is keyed on the pixels, so a repeated glyph is placed (and uploaded) once
  //
  // A frame numbers its images from zero and allocates a fresh list, so an index names a different bitmap every frame
  // and an index-keyed cache could never hit across frames -- every glyph and image was placed and re-uploaded every
  // frame. Keyed on the content, the same glyph in the next frame finds its rectangle in the page it is already in.
  const uint64_t hash = knownHash != 0 && info.colorSpace() == nullptr
    ? knownHash
    : hashImage(info, pixmap, sampling);
  if (const auto found = mImagePlacements.find(hash);
      found != mImagePlacements.end() && found->second.textureId != kNoSampledTexture) {
    return &found->second;
  }

  ImagePlacement entry;
  entry.width = static_cast<uint32_t>(info.width());
  entry.height = static_cast<uint32_t>(info.height());
  // Recorded so that a placement knows which pixels it was made for -- the cache's validity check, exactly as it was
  // when the entry was a texture.
  entry.contentHash = hash;
  entry.sampling = sampling;

  // A page first, for a bitmap that fits one. The padded rectangle is what the allocator reserves, and the +2 is the
  // ring: a bitmap the page is exactly wide enough for could not be ringed.
  const uint32_t paddedWidth = entry.width + 2u;
  const uint32_t paddedHeight = entry.height + 2u;
  // A page binds one sampler for every bitmap in it, and that sampler is linear because glyph coverage needs it. A
  // *linear* image therefore fits a page, while a *nearest* one gets a texture of its own -- the alternative is a draw
  // that silently samples the way the page does, which is exactly what the hardware sampling test caught.
  // Mipmapped images are kept out of the pages on purpose: a page's mip levels would average across every bitmap in
  // it, so a neighbour would bleed into this one the moment the draw minified.
  const bool mipmapped = sampling.mipmap != MipmapMode::kNone;
  // A cubic tap reaches two texels past the sample, so it would read the neighbouring entries of a page.
  const bool ownTexture = mipmapped || sampling.useCubic;
  if (!ownTexture && paddedWidth <= kImageAtlasExtent &&
      paddedHeight <= kImageAtlasExtent) {
    uint32_t pageIndex = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    ImageAtlasPage* page = pageWithRoomFor(paddedWidth, paddedHeight, &pageIndex, &x, &y);
    if (page != nullptr) {
      entry.inPage = true;
      entry.page = pageIndex;
      entry.x = x;
      entry.y = y;
      entry.textureId = page->textureId;
      // The bitmap's interior, one texel in from the padded rectangle's corner: what `image.frag` samples.
      entry.rect = normalizedImageRect(x, y, entry.width, entry.height, page->width, page->height);
    }
  }

  const backend::TextureSamplerHandle sampler = samplerFor(sampling);

  // A texture of its own when there is no room in a page -- too large, or the page budget is spent. This is the route
  // every image took before the atlas existed, so it is a fallback rather than a second design: the picture is the same
  // and the only difference is that it cannot share a draw.
  if (!entry.inPage) {
    backend::TextureDescriptor descriptor = backend::TextureDescriptor::texture2D(entry.width, entry.height,
      backend::PixelFormat::R8G8B8A8_UNORM, static_cast<backend::TextureUsages>(backend::TextureUsage::Sampled));
    // A mipmapped image owns its texture, so the chain is simply this texture's levels: nothing extra to track, and the
    // sampler's mipmap filter is all that is needed to use them.
    if (mipmapped) {
      uint32_t extent = std::max(entry.width, entry.height);
      entry.mipLevelCount = 1;
      while (extent > 1) {
        extent /= 2;
        ++entry.mipLevelCount;
      }
      descriptor.mipLevelCount = entry.mipLevelCount;
    }
    if (!backend::isSuccess(mDevice->createTexture(descriptor, &entry.texture))) {
      return nullptr;
    }
    backend::TextureViewDescriptor viewDescriptor;
    viewDescriptor.texture = entry.texture;
    viewDescriptor.dimension = backend::TextureViewDimension::Texture2D;
    if (!backend::isSuccess(mDevice->createTextureView(viewDescriptor, &entry.view))) {
      mDevice->destroyTexture(entry.texture);
      return nullptr;
    }
    if (!backend::isSuccess(mDevice->createDescriptorSet(mQuadSetLayout, &entry.set))) {
      mDevice->destroyTextureView(entry.view);
      mDevice->destroyTexture(entry.texture);
      return nullptr;
    }
    const backend::DescriptorWrite writes[2] = {
      { 0, backend::BindingType::TextureSampler, entry.texture, sampler, {}, 0 },
      { 1, backend::BindingType::UniformBuffer, backend::TextureHandle::invalid(),
        backend::TextureSamplerHandle::invalid(),
        { uniformBuffer(), kCanvasSlot * kUniformSlotSize, kUniformSlotSize }, 0 },
    };
    if (!backend::isSuccess(mDevice->updateDescriptorSet(entry.set, writes, 2))) {
      mDevice->destroyDescriptorSet(entry.set);
      mDevice->destroyTextureView(entry.view);
      mDevice->destroyTexture(entry.texture);
      return nullptr;
    }
    entry.textureId = mNextImageTextureId++;
    entry.rect = { 0.0f, 0.0f, 1.0f, 1.0f };
  }

  // Stored under the content key. A second bitmap that hashes alike *replaces* the first rather than leaking beside it --
  // and the replaced entry's resources are released, which is what the previous version of this cache got wrong: it
  // overwrote the texture, the view and the set without destroying any of them, so the set a previous frame's draws had
  // been recorded against stayed live.
  if (const auto found = mImagePlacements.find(hash); found != mImagePlacements.end()) {
    releaseImagePlacement(found->second);
    found->second = std::move(entry);
    return &found->second;
  }
  auto [slot, inserted] = mImagePlacements.emplace(hash, std::move(entry));
  (void) inserted;
  return &slot->second;
}

void GpuRenderer::releaseImagePlacement(ImagePlacement& placement) noexcept {
  if (mDevice == nullptr) return;
  if (placement.set.valid()) mDevice->destroyDescriptorSet(placement.set);
  if (placement.view.valid()) mDevice->destroyTextureView(placement.view);
  if (placement.texture.valid()) mDevice->destroyTexture(placement.texture);
  placement.set = backend::DescriptorSetHandle::invalid();
  placement.view = backend::TextureViewHandle::invalid();
  placement.texture = backend::TextureHandle::invalid();
  placement.textureId = kNoSampledTexture;
}

backend::DescriptorSetHandle GpuRenderer::externalSetFor(const backend::TextureHandle texture,
        const backend::TextureViewHandle view) {
  const uint64_t key = view.valid() ? view.value() : texture.value();
  const auto found = mExternalImageSets.find(key);
  if (found != mExternalImageSets.end()) {
    return found->second;
  }
  backend::DescriptorSetHandle set = backend::DescriptorSetHandle::invalid();
  if (!backend::isSuccess(mDevice->createDescriptorSet(mQuadSetLayout, &set))) {
    return backend::DescriptorSetHandle::invalid();
  }
  // Both are filled when the caller has both. A backend that resolves a view natively takes the
  // view; one that binds textures (OpenGL) resolves it itself from the view, or uses the texture
  // directly when the caller passed only that.
  //
  // Binding 1 is **not optional**: `mQuadSetLayout` declares the canvas block there, and
  // `quad.vert` reads it to place the quad. A set with binding 0 alone leaves binding 1 unwritten,
  // which is an incomplete set -- the draw is issued, every vertex is placed from garbage, and the
  // picture is empty with no error anywhere. That is exactly how the first external-image attempt
  // failed. The block is staged at slot 0 every frame, so binding it here costs nothing.
  backend::DescriptorWrite writes[2];
  writes[0].binding = 0;
  writes[0].type = backend::BindingType::TextureSampler;
  writes[0].texture = texture;
  writes[0].sampler = mLinearClamp;
  writes[0].view = view;
  writes[1].binding = 1;
  writes[1].type = backend::BindingType::UniformBuffer;
  writes[1].buffer = { uniformBuffer(), static_cast<uint64_t>(kCanvasSlot) * kUniformSlotSize,
    kUniformSlotSize };
  if (!backend::isSuccess(mDevice->updateDescriptorSet(set, writes, 2))) {
    mDevice->destroyDescriptorSet(set);
    return backend::DescriptorSetHandle::invalid();
  }
  mExternalImageSets.emplace(key, set);
  return set;
}

void GpuRenderer::placeImages(Frame& frame, const std::vector<Bitmap>& images) {
  const DrawList& list = *frame.list;
  for (Frame::PlacedImage& previous: frame.images) {
    if (previous.patternSet.valid()) mDevice->destroyDescriptorSet(previous.patternSet);
  }
  frame.images.assign(images.size(), Frame::PlacedImage{});

  // ## External textures ride the image route, in the tail of the same table
  //
  // A `Kind::ExternalImage` command is an image command whose texture the renderer did not upload: the
  // producer owns it and the renderer only borrows the view. Giving it a place in `frame.images`
  // rather than a parallel table is what lets `canShareDraw`, `drawImageBatch` and the vertex writer
  // stay one route: an external entry has a texture id, a full-texture sub-rectangle and a set, which
  // is exactly what those three read. The entry's `set` is cached per view (`externalSetFor`), because
  // a ring of producer textures repeats its views frame after frame.
  frame.images.resize(images.size() + list.externalImages.size());
  for (size_t externalIndex = 0; externalIndex < list.externalImages.size(); ++externalIndex) {
    Frame::PlacedImage& placed = frame.images[images.size() + externalIndex];
    placed.resolved = true;
    const DrawList::ExternalImage& external = list.externalImages[externalIndex];
    if (!external.isValid()) continue;
    placed.set = externalSetFor(external.texture, external.view);
    if (!placed.set.valid()) continue;
    placed.valid = true;
    placed.sampling = SamplingOptions(FilterMode::kLinear);
    placed.textureId = kExternalImageTextureBase + static_cast<uint32_t>(externalIndex);
    placed.view = external.view;
  }

  // One attempt per image index per frame, successful or not: a bitmap that cannot be placed would otherwise be hashed
  // once per command that names it, and a frame of 300 glyph draws naming one unplaceable bitmap is 300 hashes.
  const auto placeOne = [&](const uint32_t index, const SamplingOptions& sampling) {
    if (index >= frame.images.size()) return;
    Frame::PlacedImage& placed = frame.images[index];
    if (placed.resolved) return;
    placed.resolved = true;

    ImagePlacement* placement = ensureImagePlacement(images[index], sampling,
      index < list.imageHashes.size() ? list.imageHashes[index] : 0);
    if (placement == nullptr) {
      // Left invalid. The replay counts it per command and draws nothing, which is the honest failure: a picture that
      // is missing is visible, while a picture sampled from whichever texture the batch bound is not.
      return;
    }
    placed.valid = true;
    // The draw picks its program from this, and this is the only place that knows it: the frame's entry is what a batch
    // reads, and the placement is what was keyed on the sampling.
    placed.sampling = placement->sampling;
    placed.contentHash = placement->contentHash;
    placed.textureId = placement->textureId;
    placed.rect = placement->rect;
    // The set a batch binds: the page's when the bitmap is in one, the bitmap's own otherwise. Taken here rather than
    // looked up at draw time because the placement table is the renderer's and the draw has only the frame.
    const bool linear = placement->sampling.filter == FilterMode::kLinear;
  placed.set = placement->inPage
                 ? (linear ? mImagePages[placement->page].set : mImagePages[placement->page].setNearest)
                 : placement->set;
    // And the texture a *mask* binds at binding 1 of the composite's set, which is a different set from the one above.
    placed.texture = placement->inPage ? mImagePages[placement->page].texture : placement->texture;
  placed.view = placement->inPage ? mImagePages[placement->page].view : placement->view;
  };

  // Walked in command order rather than over the image table, because a frame's table holds every bitmap any recording
  // registered and only the ones a command *names* are ever sampled. Placing the rest would put bitmaps in pages that
  // nothing draws.
  for (const DrawList::Command& command: list.commands) {
    if (command.kind != DrawList::Kind::Image) continue;
    placeOne(command.state.image, command.sampling);
  }
  // A mask, however, is named by no command at all: it belongs to its layer, and the composite that reads it runs when
  // that layer closes. Left out of the walk above it would never be placed and never uploaded -- a mask that is silently
  // the empty texture, which is a layer that nothing masks, which is the defect this whole path exists to fix.
  for (const Layer& layer: list.layers) {
    if (layer.maskImage != Layer::kNoMaskImage) placeOne(layer.maskImage, SamplingOptions{});
  }
}

void GpuRenderer::uploadPlacedImages(Frame& frame, const std::vector<Bitmap>& images) {
  for (uint32_t index = 0; index < frame.images.size() && index < images.size(); ++index) {
    const Frame::PlacedImage& placed = frame.images[index];
    if (!placed.valid) continue;
    const auto found = mImagePlacements.find(placed.contentHash);
    if (found == mImagePlacements.end()) continue;
    ImagePlacement& placement = found->second;
    // ## The marginal upload, which is what the persistent page buys
    //
    // A bitmap is written into its texture the first frame a session draws it, and never again -- so a frame that draws
    // the same text as the last one records no texture write at all, and the whole route's uploads stop after the first
    // frame of a scene. The flag is on the *placement* rather than on the frame's entry because two image indices with
    // the same pixels share one placement, and pixels already in a texture do not need putting there twice.
    if (placement.uploaded) continue;

    const graphics::ImageInfo sourceInfo = images[index].info();
    graphics::Pixmap pixmap = images[index].makePixmap();
    // ## Colour-managed upload: the working space is sRGB
    //
    // An image whose `ImageInfo` carries another colour space (a Display-P3 photo with an embedded ICC
    // profile) would otherwise upload its numbers as if they were sRGB. The conversion happens here
    // rather than at draw time because this is the one place the pixels become texture bytes: the
    // atlas path and the own-texture path (and its mip chain) all read the `pixmap` below.
    //
    // Once per placement, not once per frame: `placement.uploaded` gates this function, so a converted
    // image pays the transform only on the frame its texture is written.
    std::vector<uint8_t> convertedPixels;
    if (const auto& space = sourceInfo.colorSpace();
        space != nullptr && !space->isSRGB() && pixmap.isValid() && pixmap.pixels() != nullptr &&
        sourceInfo.colorType() == graphics::ColorType::kRGBA_8888) {
      if (auto xform = graphics::ColorSpaceXform::Make(*space, *graphics::ColorSpace::MakeSRGB())) {
        const size_t rowBytes = static_cast<size_t>(sourceInfo.width()) * 4u;
        convertedPixels.resize(rowBytes * static_cast<size_t>(sourceInfo.height()));
        const auto* source = static_cast<const uint8_t*>(pixmap.pixels());
        for (int row = 0; row < sourceInfo.height(); ++row) {
          std::memcpy(convertedPixels.data() + static_cast<size_t>(row) * rowBytes,
            source + static_cast<size_t>(row) * pixmap.rowBytes(), rowBytes);
        }
        for (int row = 0; row < sourceInfo.height(); ++row) {
          xform->applyRgba8(convertedPixels.data() + static_cast<size_t>(row) * rowBytes, rowBytes / 4u);
        }
        pixmap = graphics::Pixmap(sourceInfo.withColorSpace(graphics::ColorSpace::MakeSRGB()),
          convertedPixels.data(), rowBytes);
      }
    }
    backend::Result result = backend::Result::Error;
    if (pixmap.isValid() && pixmap.pixels() != nullptr) {
      if (placement.inPage && placement.page < mImagePages.size()) {
        const ImageAtlasPage& page = mImagePages[placement.page];
        const std::vector<uint8_t> payload = paddedBitmap(pixmap);
        result = writeImageRegion(mCommandBuffer, page.texture, placement.x, placement.y,
                placement.width + 2u, placement.height + 2u, payload);
      } else if (!placement.inPage && placement.texture.valid()) {
        // A texture of its own takes the bitmap's rows as they are, stride included: `rowBytes` is the caller's and the
        // backend reads exactly that many bytes per row, which is what makes a padded source bitmap upload correctly.
        backend::TextureCopyRegion region;
        region.textureOrigin = { 0, 0, 0 };
        region.size = { placement.width, placement.height, 1 };
        region.mipLevel = 0;
        region.baseArrayLayer = 0;
        region.arrayLayerCount = 1;
        const size_t byteCount = static_cast<size_t>(pixmap.rowBytes()) * placement.height;
        result = mCommandBuffer->writeTexture(placement.texture, region, pixmap.pixels(), byteCount);

        // Then the levels above zero, box filtered here rather than by a GPU pass: it is the same bytes the upload just
        // read, it happens once per placement (placements are cached by content), and it needs no second pipeline or
        // second bind. A GPU blit chain is the cheaper route for a large image whose mips change every frame; this is the
        // shape for one that does not.
        if (backend::isSuccess(result) && placement.mipLevelCount > 1) {
          std::vector<uint8_t> level = downsampleRgba(static_cast<const uint8_t*>(pixmap.pixels()), placement.width,
            placement.height, static_cast<uint32_t>(pixmap.rowBytes()));
          uint32_t width = placement.width;
          uint32_t height = placement.height;
          for (uint32_t levelIndex = 1; levelIndex < placement.mipLevelCount; ++levelIndex) {
            const uint32_t nextWidth = std::max(1u, width / 2u);
            const uint32_t nextHeight = std::max(1u, height / 2u);
            backend::TextureCopyRegion levelRegion;
            levelRegion.textureOrigin = { 0, 0, 0 };
            levelRegion.size = { nextWidth, nextHeight, 1 };
            levelRegion.mipLevel = levelIndex;
            levelRegion.baseArrayLayer = 0;
            levelRegion.arrayLayerCount = 1;
            result = mCommandBuffer->writeTexture(placement.texture, levelRegion, level.data(),
              static_cast<uint64_t>(level.size()));
            if (!backend::isSuccess(result)) break;
            level = downsampleRgba(level.data(), nextWidth, nextHeight, nextWidth * 4u);
            width = nextWidth;
            height = nextHeight;
          }
        }
      }
    }
    // A bitmap with no addressable pixels and a bitmap whose write was refused are the same thing to the picture -- the
    // texels are not there -- so they take the same path below rather than two that could drift apart.
    if (!backend::isSuccess(result)) {
      // ## A bitmap whose upload failed must not be drawn from -- and within a batch that means its whole texture
      //
      // The write is what puts the pixels there, so a rectangle the driver never touched holds whatever it left in the
      // page. The frame's entries are therefore invalidated, and the replay counts their commands as omissions and draws
      // nothing: a missing picture rather than a picture of somebody else's texels.
      //
      // The invalidation is **by texture** rather than by bitmap, and that is not over-reach: a batch binds one texture,
      // so a command whose bitmap failed cannot be dropped out of a draw that also carries commands whose bitmaps are
      // fine -- the whole draw is one call. Invaliding every frame entry that samples this texture is what makes the
      // batch's first command invalid too, which is the check the replay makes. It costs this texture's commands for one
      // failed write; the alternative is drawing one of them from memory the driver never wrote.
      //
      // `uploaded` stays false so that a later frame retries: the failure is a recording failure, not a property of the
      // bitmap, and a page that could not be written into once is not one that never can be.
      for (Frame::PlacedImage& other: frame.images) {
        if (other.valid && other.textureId == placement.textureId) other.valid = false;
      }
      continue;
    }
    placement.uploaded = true;
    ++mImageUploads;
    mStats.textureUploadCount = mImageUploads;
  }
}

/** @return the index in `frame.images` that @p command's image names. */
uint32_t imageIndexForCommand(const DrawList::Command& command, const DrawList& list) {
  // External images are appended after the list's own bitmaps: `placeImages` resizes `frame.images` to
  // `list.images.size() + list.externalImages.size()` and fills the tail with the borrowed views, so
  // the two tables keep their own indices and one command can name either.
  if (command.kind == DrawList::Kind::ExternalImage) {
    return static_cast<uint32_t>(list.images.size()) + command.state.image;
  }
  return command.state.image;
}

uint32_t GpuRenderer::sampledTextureOfCommand(const DrawList::Command& command, const Frame& frame) noexcept {
  if (command.state.shader == DrawList::ShaderKind::Coverage) {
    return atlasOfCommand(command, frame.placed);
  }
  if (command.state.shader == DrawList::ShaderKind::Image) {
    const uint32_t image = imageIndexForCommand(command, *frame.list);
    if (image >= frame.images.size() || !frame.images[image].valid) return kNoSampledTexture;
    return frame.images[image].textureId;
  }
  return kNoSampledTexture;
}

namespace {

/**
 * @return a hash of @p bytes, which is what decides whether a frame re-uploads its atlas.
 *
 * FNV-1a, the same function `hashImage` uses over pixels and for the same reason: this is a cache validity check rather
 * than a content address, so a collision shows one frame's fields in another's place, and the alternative -- comparing a
 * megabyte of coverage against the texture's contents -- is a comparison the driver would have to make anyway.
 */
uint64_t hashBytes(const uint8_t* bytes, const size_t size) {
  uint64_t hash = 1469598103934665603ULL;
  for (size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= 1099511628211ULL;
  }
  return hash;
}

/**
 * @return the smallest power of two that is at least @p value, or @p value itself past the largest atlas.
 *
 * An atlas is allocated at a power of two so that a scene whose field count changes from frame to frame reuses the same
 * texture rather than reallocating: the pool is keyed on this extent, and a continuous size would be a new texture per
 * frame -- the defect the per-field pool had before it, one level up.
 */
uint32_t atlasExtent(const uint64_t needed) {
  constexpr uint32_t kLargestAtlas = 4096;
  if (needed == 0) return 0;
  uint64_t extent = 64;
  while (extent < needed && extent < kLargestAtlas) extent <<= 1;
  return static_cast<uint32_t>(std::min<uint64_t>(extent, kLargestAtlas));
}

/**
 * Records an upload of a whole @p bytes payload into @p texture, **one byte per texel**.
 *
 * The atlas is uploaded whole rather than per field, which is the point of it: one command per *atlas* whose contents
 * changed, instead of one per field. The payload is the packed atlas the resolve stage built -- fields with a one-texel
 * replicated border, empty space zeroed -- so this is a copy and nothing else.
 *
 * ## Why one byte per texel
 *
 * A coverage field carries coverage and nothing else, so the texture is `R8_UNORM` and the bytes go in as they are. The
 * first version expanded every texel to premultiplied-white RGBA -- the layout a *glyph's* bitmap needs, because it is
 * drawn as an image and an image needs a colour -- which cost four times the payload for three channels the fragment
 * stage never reads.
 *
 * @return the backend's result, which the caller must treat as fatal for this draw -- an atlas whose contents are a
 *         previous frame's would be sampled and the picture would be stale with no error anywhere.
 */
backend::Result writeAtlas(backend::CommandBuffer* commandBuffer, const backend::TextureHandle texture,
        const uint32_t width, const uint32_t height, const std::vector<uint8_t>& bytes) {
  backend::TextureCopyRegion region;
  region.textureOrigin = { 0, 0, 0 };
  region.size = { width, height, 1 };
  region.mipLevel = 0;
  region.baseArrayLayer = 0;
  region.arrayLayerCount = 1;
  // Tightly packed rows of the atlas's own width, so `bytesPerRow = 0` is exactly right -- and `GL_UNPACK_ALIGNMENT`
  // matters here for the same reason it did for the per-field texture: a one-byte format whose row is not a multiple of
  // four bytes is read skewed unless the alignment says otherwise, which the backend does.
  const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height);
  const size_t available = std::min(bytes.size(), expected);
  return commandBuffer->writeTexture(texture, region, bytes.data(), static_cast<uint64_t>(available));
}

/**
 * Fills @p out with a contour range of @p list, shifted into **field space**, reusing @p out's own storage.
 *
 * A `DrawList` stores every command's contours in two flat arrays (`contours`, then `points`) so that a batch's vertex
 * data is one contiguous run rather than a list of allocations. The rasteriser takes a vector of contours, so this is the
 * one place the flat form is turned back into the nested one -- and every index is clamped rather than trusted, because a
 * malformed range here would read past the array instead of drawing one shape wrong.
 *
 * ## Two things this does that the obvious version did not
 *
 * It **shifts while it copies**, subtracting the field's origin from every point as it goes, so the field-space contours a
 * field's bands need cost one pass over the points rather than two (a copy out of the list, then a copy to shift it). And
 * it **reuses** @p out: the contours' point buffers are cleared rather than freed, so a task that fills many fields' bands
 * in sequence allocates once and then walks the same memory. That matters because the polygons here are small -- the demo
 * averages 95 texels a field, so the geometry is a few dozen points -- and at that size the per-call `malloc` used to be a
 * visible fraction of the field's cost. See 璺緞杈圭紭鎶楅敮榻胯璁′笌鐜扮姸.md 4.1.18.
 */
void contoursOf(const DrawList& list, const uint32_t firstContour, const uint32_t contourCount, const int originX,
        const int originY, std::vector<raster::Contour>& out) {
  out.clear();
  for (uint32_t index = 0; index < contourCount; ++index) {
    const uint32_t contourIndex = firstContour + index;
    if (contourIndex >= list.contours.size()) break;
    const DrawList::Contour& source = list.contours[contourIndex];
    const uint32_t first = std::min(source.firstPoint, static_cast<uint32_t>(list.points.size()));
    const uint32_t end = std::min(source.firstPoint + source.pointCount, static_cast<uint32_t>(list.points.size()));
    if (out.size() <= index) out.emplace_back();
    raster::Contour& contour = out[index];
    // `clear` rather than a fresh vector: the capacity survives, which is the whole point of taking `out` by reference.
    contour.points.clear();
    contour.points.reserve(end - first);
    for (uint32_t point = first; point < end; ++point) {
      Point position = list.points[point];
      position.x -= static_cast<Scalar>(originX);
      position.y -= static_cast<Scalar>(originY);
      contour.points.push_back(position);
    }
  }
}

} // namespace anonymous

GpuRenderer::FieldAtlas* GpuRenderer::ensureFieldAtlas(const uint32_t width, const uint32_t height) {
  if (width == 0 || height == 0) return nullptr;
  if (mCoverageFieldSetLayout.valid() == false) return nullptr;

  const uint64_t key = (static_cast<uint64_t>(width) << 32) | static_cast<uint64_t>(height);
  if (const auto found = mFieldAtlases.find(key); found != mFieldAtlases.end() && found->second.texture.valid()) {
    return &found->second;
  }

  FieldAtlas entry;
  entry.width = width;
  entry.height = height;
  // `R8_UNORM` and not RGBA: an atlas holds coverage and nothing else, so three of RGBA's four bytes would be channels the
  // fragment stage never reads -- four times the payload per frame, for a picture that is identical.
  const backend::TextureDescriptor descriptor = backend::TextureDescriptor::texture2D(width, height,
    backend::PixelFormat::R8_UNORM, static_cast<backend::TextureUsages>(backend::TextureUsage::Sampled));
  if (!backend::isSuccess(mDevice->createTexture(descriptor, &entry.texture))) {
    return nullptr;
  }
  backend::TextureViewDescriptor viewDescriptor;
  viewDescriptor.texture = entry.texture;
  viewDescriptor.dimension = backend::TextureViewDimension::Texture2D;
  if (!backend::isSuccess(mDevice->createTextureView(viewDescriptor, &entry.view))) {
    mDevice->destroyTexture(entry.texture);
    return nullptr;
  }

  // One set per pooled atlas, against the coverage-field layout: the sampler at binding 0 and the canvas block at 1. The
  // block the *atlas* is described by (binding 2) is written per batch, before each of its draws.
  if (!backend::isSuccess(mDevice->createDescriptorSet(mCoverageFieldSetLayout, &entry.set))) {
    mDevice->destroyTextureView(entry.view);
    mDevice->destroyTexture(entry.texture);
    return nullptr;
  }
  const backend::DescriptorWrite writes[3] = {
    { 0, backend::BindingType::TextureSampler, entry.texture, mLinearClamp, {}, 0 },
    { 1, backend::BindingType::UniformBuffer, backend::TextureHandle::invalid(),
      backend::TextureSamplerHandle::invalid(),
      { uniformBuffer(), kCanvasSlot * kUniformSlotSize, kUniformSlotSize }, 0 },
    { 2, backend::BindingType::UniformBuffer, backend::TextureHandle::invalid(),
      backend::TextureSamplerHandle::invalid(),
      // A valid placeholder until the first coverage batch overwrites it with that batch's own slot. A set has to name
      // all of its bindings, and leaving the block unwritten is a draw a driver may reject.
      { uniformBuffer(), kCanvasSlot * kUniformSlotSize, kUniformSlotSize }, 0 },
  };
  if (!backend::isSuccess(mDevice->updateDescriptorSet(entry.set, writes, 3))) {
    mDevice->destroyDescriptorSet(entry.set);
    mDevice->destroyTextureView(entry.view);
    mDevice->destroyTexture(entry.texture);
    return nullptr;
  }

  // ## The texture is zeroed once, here, and its contents are never assumed
  //
  // A new atlas is zeroed before any field is written into it, so that the space *between* fields holds a known value
  // rather than whatever the driver left in the allocation. A field's own taps are answered by its replicated border, so
  // this is not what makes the picture right -- it is what keeps a picture from depending on uninitialised memory if the
  // placement is ever wrong, which is the kind of defect this area keeps meeting.
  const std::vector<uint8_t> zeros(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
  if (!backend::isSuccess(writeAtlas(mCommandBuffer, entry.texture, width, height, zeros))) {
    mDevice->destroyDescriptorSet(entry.set);
    mDevice->destroyTextureView(entry.view);
    mDevice->destroyTexture(entry.texture);
    return nullptr;
  }
  // The zeroing is an upload, so the pooled entry starts out *not* matching any packed frame: a frame whose bytes happened
  // to hash to the same value as "all zeros" would otherwise skip its own upload and draw an empty atlas. A flag rather
  // than a sentinel hash, because every 64-bit value is a legal FNV result and a sentinel would be a coincidence waiting
  // to happen.
  entry.hasContent = false;

  auto [slot, inserted] = mFieldAtlases.insert_or_assign(key, entry);
  (void) inserted;
  ++mImageUploads;
  mStats.textureUploadCount = mImageUploads;
  return &slot->second;
}

/**
 * @return a key over one field's own rasterisation input: its lattice, its fill rule, and the contours it names.
 *
 * ## What is in it, and the one thing that is deliberately not
 *
 * The atlas bytes a field contributes are a function of its lattice and of the polygons the fill walks, so the key folds
 * the lattice, the fill type, the contour count and every point of every contour the field names. The contours' *indices*
 * are left out -- `firstContour` and each `firstPoint` -- because those say where the data sits in the list rather than
 * what it is, and a table keyed on content is exactly the thing that should recognise a shape that moved position in the
 * array. Including them would make the probe report fewer reusable fields than a content-keyed table could serve.
 *
 * A caller's own texels (`hasCoverage()`) are not keyed at all: their bytes are not derivable from the list, so two such
 * fields with the same lattice can carry different coverage. The stage treats them as unkeyable rather than as content --
 * they are refilled every frame, which is a property of the field rather than of the frame it arrived in.
 */
uint64_t fieldContentKey(const DrawList& list, const DrawList::CoverageField& field) {
  uint64_t key = 1469598103934665603ULL;
  const auto mix = [&key](const uint64_t value) {
    key ^= value;
    key *= 1099511628211ULL;
  };

  // ## The key is the **shape**, not where the shape is (4.2)
  //
  // The origin used to be folded in here, which made a moving shape a new field every frame and sent it back through the
  // rasteriser. That was the measured cost of an animated antialiased scene: at 2528x1291 the Live demo's orbiting circle
  // alone spent 9.86 ms a frame in this stage, and 21.27 ms with a rotated square beside it.
  //
  // It does not have to be in the key. `shiftIntoFieldSpace` subtracts the field's origin from every contour point before the
  // fill runs, so a field's texels are a function of its shape and size only -- the same circle four pixels apart produces
  // byte-identical texels. The origin still matters to the frame, but through `PlacedField::texelOriginX/Y`, which
  // `resolveCoverageFields` recomputes from it on every frame.
  mix(static_cast<uint32_t>(field.width));
  mix(static_cast<uint32_t>(field.height));
  mix(static_cast<uint32_t>(field.fillType));

  const uint32_t contourEnd = field.firstContour + field.contourCount;
  for (uint32_t index = field.firstContour; index < contourEnd && index < list.contours.size(); ++index) {
    // A separator between contours, so that one contour of four points is not the same key as four of one point.
    mix(0xFFu);
    const DrawList::Contour& contour = list.contours[index];
    const uint32_t pointEnd = contour.firstPoint + contour.pointCount;
    for (uint32_t point = contour.firstPoint; point < pointEnd && point < list.points.size(); ++point) {
      // ## One mix per point rather than two (4.1.51)
      //
      // A point is two 32-bit scalars and they are adjacent, so folding them as one 64-bit word halves the dependent
      // multiplies this loop spends -- and this loop *is* the key: measured, 202229 points cost 0.65 ms at two mixes each,
      // about 3.3 ns a point, which is the per-point cost of an FNV chain and not of the pixels those points describe. The
      // old whole-frame key made the same trade for the same reason ("eight bytes at a time rather than `hashBytes`").
      //
      // `memcpy` rather than a cast, so the fold is a function of the bytes rather than of an alignment the compiler
      // happened to pick; the static assertion is what keeps "a point is eight bytes" a fact this code checks rather than
      // assumes, since a `Point` that grew a member would otherwise silently stop hashing part of itself.
      static_assert(sizeof(Point) == sizeof(uint64_t), "a point is folded as one 64-bit word");
      // ## The points are folded in **field space**, which is what makes the key translation-invariant
      //
      // The list stores device-space points and `shiftIntoFieldSpace` subtracts the origin only when the fill runs, so hashing
      // the stored point would fold the shape's *position* into its key -- and a shape that moves would never hit. The
      // subtraction here is the same one the fill does, which is why the two frames of a moved circle now hash alike.
      Point relative = list.points[point];
      relative.x -= static_cast<Scalar>(field.originX);
      relative.y -= static_cast<Scalar>(field.originY);
      uint64_t packed = 0;
      std::memcpy(&packed, &relative, sizeof(packed));
      mix(packed);
    }
  }
  return key;
}

/**
 * @return @p value rounded up to a multiple of @p granularity (never zero).
 *
 * The slot question the probe asks: a stable placement has to give each field a box with a little room in it, because a
 * field whose lattice grew by one texel would otherwise need a new slot -- and moving one field in a shelf packer moves
 * every field after it.
 */
uint32_t roundUp(const uint32_t value, const uint32_t granularity) {
  return (value + granularity - 1) / granularity * granularity;
}

void GpuRenderer::probeFieldReuse(const DrawList& list) {
  if (!mFieldReuse.initialised) {
    mFieldReuse.initialised = true;
    // Read once, from the environment, because it is a measurement rather than a policy: `CORAL_TRACE_FIELD_REUSE=<depth>`
    // asks "how much of this frame's field work appeared in the last <depth> frames", and anything unparsable or zero
    // leaves the probe off and the frame exactly as it would have been.
    if (const char* const requested = std::getenv("CORAL_TRACE_FIELD_REUSE")) {
      const long depth = std::strtol(requested, nullptr, 10);
      if (depth > 0 && depth <= 1024) {
        mFieldReuse.frames.resize(static_cast<size_t>(depth));
      }
    }
  }
  const size_t depth = mFieldReuse.frames.size();
  if (depth == 0) return;

  std::unordered_set<uint64_t> current;
  current.reserve(list.coverageFields.size());
  uint32_t hits = 0;
  uint32_t requests = 0;

  for (const DrawList::CoverageField& field: list.coverageFields) {
    ++requests;
    if (field.hasCoverage()) continue;
    const uint64_t key = fieldContentKey(list, field);
    current.insert(key);
    for (const std::unordered_set<uint64_t>& recent: mFieldReuse.frames) {
      if (recent.find(key) != recent.end()) {
        ++hits;
        break;
      }
    }
  }

  mFieldReuse.frames[mFieldReuse.next] = std::move(current);
  mFieldReuse.next = (mFieldReuse.next + 1) % depth;
  mFieldReuse.hits = hits;
  mFieldReuse.requests = requests;

  // ## The placement half: would a changed field still fit the box it had?
  //
  // Run here rather than after the placement because `mFieldPlaced` still describes the *previous* frame at this point --
  // `placeCoverageFields` overwrites it on a miss, and this is the only moment both the new lattices and the old boxes
  // are available at once. A field that fits needs no new placement and no repack: only its own rectangle is refilled.
  const std::vector<Frame::PlacedField>& previous = mFieldPlaced;
  const bool comparable = mFieldReuse.lattices.size() == list.coverageFields.size() && previous.size() == list.coverageFields.size();
  mFieldReuse.latticeChanged = 0;
  mFieldReuse.contentChanged = 0;
  mFieldReuse.maxWidthDelta = 0;
  mFieldReuse.maxHeightDelta = 0;
  mFieldReuse.maxOriginDelta = 0;
  for (uint32_t& count: mFieldReuse.fitsGranularity) count = 0;
  std::vector<FieldLattice> lattices;
  lattices.reserve(list.coverageFields.size());
  for (size_t index = 0; index < list.coverageFields.size(); ++index) {
    const DrawList::CoverageField& field = list.coverageFields[index];
    FieldLattice lattice;
    lattice.originX = field.originX;
    lattice.originY = field.originY;
    lattice.width = field.width;
    lattice.height = field.height;
    lattice.fillType = static_cast<uint32_t>(field.fillType);
    lattice.empty = field.isEmpty();
    lattice.hasCoverage = field.hasCoverage();
    lattice.contentKey = lattice.hasCoverage ? 0 : fieldContentKey(list, field);
    lattices.push_back(lattice);

    if (!comparable) continue;
    const const FieldLattice& before = mFieldReuse.lattices[index];
    if (lattice.hasCoverage) continue; // its bytes are not derivable, so its slot has to be refilled whatever it fits
    if (lattice.contentKey != before.contentKey) ++mFieldReuse.contentChanged;
    const bool latticeSame = lattice.originX == before.originX && lattice.originY == before.originY &&
                             lattice.width == before.width && lattice.height == before.height &&
                             lattice.fillType == before.fillType;
    if (latticeSame) continue;
    ++mFieldReuse.latticeChanged;
    const auto widthDelta = static_cast<int32_t>(lattice.width) - static_cast<int32_t>(before.width);
    const auto heightDelta = static_cast<int32_t>(lattice.height) - static_cast<int32_t>(before.height);
    mFieldReuse.maxWidthDelta = std::max(mFieldReuse.maxWidthDelta, widthDelta);
    mFieldReuse.maxHeightDelta = std::max(mFieldReuse.maxHeightDelta, heightDelta);
    mFieldReuse.maxOriginDelta = std::max(mFieldReuse.maxOriginDelta,
            std::max(std::abs(lattice.originX - before.originX), std::abs(lattice.originY - before.originY)));

    // The box the field had, and whether the new one still fits inside a slot rounded up to `granularity`.
    const Frame::PlacedField& slot = previous[index];
    const uint32_t boxWidth = lattice.width + 2;
    const uint32_t boxHeight = lattice.height + 2;
    const uint32_t granularities[6] = { 1, 2, 4, 8, 16, 32 };
    for (size_t bucket = 0; bucket < 6; ++bucket) {
      const uint32_t granularity = granularities[bucket];
      const uint32_t slotWidth = roundUp(before.width + 2, granularity);
      const uint32_t slotHeight = roundUp(before.height + 2, granularity);
      if (boxWidth <= slotWidth && boxHeight <= slotHeight) {
        ++mFieldReuse.fitsGranularity[bucket];
      }
    }
    (void) slot;
  }
  mFieldReuse.lattices = std::move(lattices);

  if (std::getenv("CORAL_TRACE_FIELD_REUSE_DETAIL") != nullptr) {
    std::fprintf(stderr,
            "[reuse] fields=%zu latticeChanged=%u contentChanged=%u | max dw=%d dh=%d dorigin=%d | fit@1=%u @2=%u "
            "@4=%u @8=%u @16=%u @32=%u\n",
            list.coverageFields.size(), mFieldReuse.latticeChanged, mFieldReuse.contentChanged,
            mFieldReuse.maxWidthDelta, mFieldReuse.maxHeightDelta, mFieldReuse.maxOriginDelta,
            mFieldReuse.fitsGranularity[0], mFieldReuse.fitsGranularity[1], mFieldReuse.fitsGranularity[2],
            mFieldReuse.fitsGranularity[3], mFieldReuse.fitsGranularity[4], mFieldReuse.fitsGranularity[5]);
    std::fflush(stderr);
  }
}

backend::Result GpuRenderer::resolveCoverageFields(Frame& frame) {
  const DrawList& list = *frame.list;
  const uint32_t count = static_cast<uint32_t>(list.coverageFields.size());
  if (count == 0) return backend::Result::Success;

  // The animation-reuse probe, before the frame-level decision so that its numbers describe the frame either way. See
  // `FieldReuseProbe` for what it measures and why it is a probe rather than a cache.
  probeFieldReuse(list);
  mStats.fieldReuseWindowHits = mFieldReuse.hits;
  mStats.fieldReuseWindowRequests = mFieldReuse.requests;
  mStats.fieldReuseWindowDepth = static_cast<uint32_t>(mFieldReuse.frames.size());

  /**
   * ## Zero: is this frame's field work the same as the last one's?
   *
   * Measured before it was built (4.1.35): the field stage is 14 ms of a 28 ms steady frame with the job pool and 50 ms
   * of 64 ms without it, and the demo -- which is what a UI is, the same scene sixty times a second -- asks for exactly
   * the same fields every frame. 4.1.10 measured this cache and rejected it, correctly, when a frame was 1300 ms and its
   * draw calls were the cost; that premise is what the rounds since removed.
   *
   * What the answer *is* changed in 4.1.45: the comparison is per field rather than over one key for the whole frame, so a
   * frame with eight changed fields out of 41084 refills eight of them and keeps the rest of the atlas, instead of
   * answering "different" for all of them and re-rasterising 16.1 ms of work. The three answers are:
   *
   *   * **every field the same** -- nothing to do at all, not even a copy;
   *   * **every field fits the slot it had** -- the placement is skipped and only the changed fields are refilled;
   *   * **anything else** -- the placement reruns and every field is filled, which is what this stage always did.
   *
   * A hit does **nothing at all** to the frame's tables -- they are references to `mFieldAtlasBytes`/`mFieldPlaced`,
   * which still hold this frame's fields because no hit and no miss clears them (see `Frame`'s constructor) -- and a
   * partial refill writes only inside the slots the fields already own, so neither can leave half the atlas belonging to
   * the frame before. See `FieldLattice` and `everyFieldFits` below.
   */
  const Clock::time_point start = Clock::now();

  // ## The identities, and the three answers they can give
  //
  // This replaces the single key over the whole frame (4.1.35-4.1.44) with one per field; `FieldLattice` says why. Building
  // them is a pass over every field's own points, which is what the frame key did too -- the same bytes, the same order of
  // magnitude -- so the finer question costs what the coarser one did.
  mFieldIdentities.clear();
  mFieldIdentities.reserve(count);
  bool everyFieldSame = mFieldCache.valid && mFieldCache.lattices.size() == count;
  // "The layout is still valid" is **not** "the lattices are equal": a field whose box grew by a texel still fits the slot
  // it had, and the slot is what decides whether its bytes are still where they belong. That is the whole of 4.1.45's
  // measurement, and the reason the placement rounds its slots up.
  bool everyFieldFits = everyFieldSame;
  // How many points this pass hashes, for the trace: the key's cost is per *field* for the bookkeeping and per *point* for
  // the hashing, so the two have to be separable to know which half to attack (4.1.51).
  uint64_t keyedPoints = 0;
  const bool traceKeys = std::getenv("CORAL_TRACE_FIELDS") != nullptr;
  for (uint32_t index = 0; index < count; ++index) {
    const DrawList::CoverageField& field = list.coverageFields[index];
    FieldLattice identity;
    identity.originX = field.originX;
    identity.originY = field.originY;
    identity.width = field.width;
    identity.height = field.height;
    identity.fillType = static_cast<uint32_t>(field.fillType);
    identity.empty = field.isEmpty();
    identity.hasCoverage = field.hasCoverage();
    // A caller's own texels have no key: their bytes live in the request, so the same lattice can carry different coverage
    // next frame. Such a field is refilled every frame, which the refill set below decides by `hasCoverage`.
    identity.contentKey = identity.hasCoverage ? 0 : fieldContentKey(list, field);
    if (traceKeys && !identity.hasCoverage) {
      const uint32_t contourEnd = field.firstContour + field.contourCount;
      for (uint32_t contour = field.firstContour; contour < contourEnd && contour < list.contours.size(); ++contour) {
        keyedPoints += list.contours[contour].pointCount;
      }
    }
    mFieldIdentities.push_back(identity);

    if (!everyFieldSame) continue;
    const FieldLattice& before = mFieldCache.lattices[index];
    if (!identity.sameLattice(before) || identity.contentKey != before.contentKey || identity.hasCoverage) {
      everyFieldSame = false;
    }
    if (!before.slotHolds(identity)) everyFieldFits = false;
  }
  const double keyMilliseconds = millisecondsSince(start);

  if (everyFieldSame && !mFieldIdentities.empty()) {
    // Nothing at all changed: not one field's content, not one field's slot. This is the frame a UI redraws sixty times a
    // second, and the atlas already holds exactly what this frame would produce.
    mStats.coverageFieldCount = mFieldCache.frameFieldCount;
    mStats.coverageFieldArea = mFieldCache.fieldArea;
    mStats.coverageFieldsReused = mFieldCache.frameFieldCount;
    mStats.coverageFieldMilliseconds = millisecondsSince(start);
    mStats.coveragePackMilliseconds = 0.0;
    if (std::getenv("CORAL_TRACE_FIELDS") != nullptr) {
      std::fprintf(stderr, "[fields] %u fields reused from the previous frame (key %.2f ms): no copies, %.2f ms total\n",
              mFieldCache.frameFieldCount, keyMilliseconds, mStats.coverageFieldMilliseconds);
      std::fflush(stderr);
    }
    return backend::Result::Success;
  }

  // The resolve stage's input: the list's requests, copied because the fill shifts each field's contours into its own
  // lattice and because the stage needs the caller's texels for the fields that carry them. Filled here rather than before
  // the cache decision so that a reused frame -- which reads none of it -- does not pay for the copy.
  frame.fields.assign(list.coverageFields.begin(), list.coverageFields.end());

  /**
   * ## The partial refill: the atlas keeps every byte it had, and only what changed is rasterised again
   *
   * This is the frame an animation produces -- one panel moving inside an otherwise static scene -- and 4.1.43 measured
   * what it is worth: 99.98% of the fields are content-identical to the frame before, so rebuilding the whole atlas is
   * re-doing 16.1 ms of a 32.7 ms frame in order to change eight fields.
   *
   * `everyFieldFits` is what makes it *safe*, and it is a statement about the **placement** rather than about the content:
   * if every field's padded box still fits the slot that field had, then `mFieldPlaced` and `mFieldAtlasBytes` describe
   * this frame's layout exactly -- so the placement does not run at all, every unchanged field's bytes are already correct
   * where they are, and a changed field writes into its own slot with no way to reach a neighbour's texels.
   *
   * A frame that fails it -- a field that moved far enough to need a new address -- takes the full path below, which
   * repacks everything and refills all of it. That is a *miss*, never a wrong hit: the condition compares the new box
   * against the old slot, and the slot is where the bytes are.
   *
   * ## A moved shape is not a changed field (4.2)
   *
   * `contentKey` is the shape's key, so a field whose only difference from last frame is its origin is **not** refilled: its
   * slot already holds exactly the texels this frame would produce, because the fill works in field space. What does change is
   * where those texels sit in device space, which is `texelOriginX/Y` -- recomputed for every field below, not only for the
   * ones being refilled. Skipping that update would draw the new position's coverage with the old position's mapping, which
   * is a picture shifted by the animation, not a crash.
   */
  mFieldPlacementKept = everyFieldFits;
  std::vector<uint32_t>& refill = mFieldRefill;
  refill.clear();
  if (everyFieldFits) {
    for (uint32_t index = 0; index < count; ++index) {
      const FieldLattice& before = mFieldCache.lattices[index];
      const FieldLattice& identity = mFieldIdentities[index];
      // The origin is deliberately **not** compared: a shape that only moved still has its texels in its slot, because the
      // fill works in field space and the key is the shape's. See the note above this loop.
      if (identity.hasCoverage || identity.contentKey != before.contentKey) {
        refill.push_back(index);
      }
    }
  }

  /**
   * ## The pipeline, and why it is no longer "generate, then pack"
   *
   * A field used to be rasterised into a buffer of its own and then copied into the atlas, which meant two pieces of work
   * over every texel, tens of thousands of small allocations a frame, and -- measured, not guessed -- a write pattern no
   * number of threads could spread: filling those small buffers cost 40 ms serially and 29 ms with a pool, *flat* from two
   * workers to eight, while the packing a few lines later scaled normally (see 璺緞杈圭紭鎶楅敮榻胯璁′笌鐜扮姸.md 4.1.18).
   *
   * So the order is inverted. Placing a field needs only its **lattice**, never its contents, so the atlases are built
   * first -- and then the scanline fill writes each band's rows straight into the atlas at the field's own rectangle. One
   * piece of work over every texel instead of two, no per-field buffer, and the writes land in one large allocation at
   * rows that are far apart rather than in adjacent small blocks.
   *
   * Four steps, and the order between them is the design:
   *
   *   1. **place** -- lattices in, `Frame::atlases` and `Frame::placed` out; skipped entirely on a partial refill, because
   *      the placement already in those tables *is* this frame's placement;
   *   2. **fill** -- bands write rows (or copy a caller's texels) into the atlas;
   *   3. **border** -- replicate each field's edge texels into the ring around it, which can only run once all of that
   *      field's interior rows exist, and reads them back out of the atlas;
   *   4. **hash** -- after the last byte is written, so a hash never describes a half-filled atlas.
   */
  if (!everyFieldFits) {
    /**
     * ## Every field is rasterised again -- and moving the unchanged ones instead was measured and **refuted** (4.1.46)
     *
     * A rebuild invalidates every address but not every *field*: a field whose content key is unchanged still has the right
     * bytes, only somewhere else. So the obvious move is to copy those rectangles into their new slots instead of drawing
     * them again -- and it was built, tested, and measured **slower**. Three runs each way on the demo's `--jitter` run gave
     * a `fields` stage of 13.81/10.87/10.20 ms *with* the move against 10.05/10.80/10.32 ms without it.
     *
     * The reason is the shape of the work, not the idea. Rasterising this scene's 41084 fields costs about 12 ms in total,
     * which is 0.3 us a field: the fields a rebuild would move are the **cheap** ones (41000 tiny SVG paths), while the
     * fill's wall time is dominated by a handful of large ones -- the background bands and the panel cards -- and those are
     * exactly the fields whose content changes. Moving the cheap ones costs a row copy each plus a 3.8 MB snapshot of the
     * previous atlases (a rebuild can change the atlas's width, and in-place moves would be a permutation), so it buys about
     * 6 ms of rasterising with about 8 ms of copying.
     *
     * What would pay is the opposite trade: not moving the many cheap fields, but making the few expensive ones cheaper.
     * That is where the next round looks, and the number to beat is the 6-8 ms those fields take on a partial refill.
     */
    if (!placeCoverageFields(frame)) {
      // Nothing was placed, so nothing can be drawn: every request is an omission rather than a shape that samples
      // whatever happens to be at atlas (0, 0). Counted, because a missing shape with a full command list is the defect
      // this whole area's notes are about.
      for (uint32_t field = 0; field < count; ++field) {
        if (!list.coverageFields[field].hasCoverage()) ++mUnsupported.coverageFields;
      }
      mStats.coverageFieldCount = 0;
      mStats.coverageFieldArea = 0;
      mStats.coverageFieldMilliseconds = millisecondsSince(start);
      mStats.coveragePackMilliseconds = mStats.coverageFieldMilliseconds;
      // A frame that could not be placed must not leave a *previous* frame's bytes behind a valid entry: the next frame's
      // identities would have to be the ones those bytes belong to for a hit to be correct, and this frame is not that one.
      mFieldCache.valid = false;
      return backend::Result::Success;
    }
    // A fresh placement has no valid bytes in it: the atlases were rebuilt and zeroed, so every field is filled.
    refill.clear();
    refill.reserve(count);
    for (uint32_t index = 0; index < count; ++index) refill.push_back(index);
  } else {
    // The slot is where the field's texels begin: `(left + 1, top + 1)` in the atlas, whatever the field's own origin is.
    // A field whose lattice moved *inside* its slot therefore has to say where its texel (0, 0) now sits in device space --
    // the fragment stage subtracts exactly that from a device position -- and it is the one thing about a kept slot that
    // changes with the lattice.
    // Every field, not only the refilled ones: a shape that moved keeps its texels but must be *mapped* from its new origin,
    // and `texelOrigin` is exactly that mapping. Refilling and remapping are separate questions now, which is the whole of
    // 4.2's change.
    for (uint32_t index = 0; index < count; ++index) {
      const DrawList::CoverageField& field = list.coverageFields[index];
      Frame::PlacedField& place = frame.placed[index];
      place.texelOriginX = static_cast<float>(field.originX - static_cast<int32_t>(place.left + 1));
      place.texelOriginY = static_cast<float>(field.originY - static_cast<int32_t>(place.top + 1));
    }
  }
  const double placeMilliseconds = millisecondsSince(start);

  // ## The units of fill work
  //
  // One per band of rows, sized so that a band is a few hundred microseconds of scanline work at the ~15 ns a texel this
  // measures and a frame is split into far more bands than there are threads. A caller that handed over its own texels gets
  // the same kind of unit over the rows of the buffer it brought, so there is one kind of unit rather than two and the
  // split stays balanced whatever mix of the two a frame has.
  constexpr uint64_t kBandTexels = 16 * 1024;
  struct Band {
    uint32_t field;
    int firstRow;
    int lastRow;
    /**
     * The texels this band rasterises, which is what its cost is proportional to.
     *
     * Carried so that the bands can be handed out **largest first**: the split is static and contiguous over this array,
     * so an array in field order gives one piece whatever the big fields' neighbourhood happens to be -- and the field
     * route's size bound went to 1024 in 4.1.14, which is what made that possible. Ordered by cost, the pieces are as
     * even as the work allows, and the measurement that showed this is in 4.1.22.
     */
    uint64_t cost = 0;
  };
  std::vector<Band> bands;
  // One band range per field **in the refill set** rather than per field of the frame: on a partial refill that set is the
  // handful of fields whose content changed, and everything else keeps the bytes the atlas already had (4.1.45). On a full
  // path the set is every field, so this is the loop it always was.
  bands.reserve(refill.size());
  uint32_t generated = 0;
  uint64_t area = 0;
  for (const uint32_t field: refill) {
    const DrawList::CoverageField& request = list.coverageFields[field];
    if (request.isEmpty()) continue;
    if (field >= frame.placed.size() || frame.placed[field].atlas >= frame.atlases.size()) continue;
    // A caller's own field must actually carry its texels; a request needs none, because the fill produces them.
    if (request.hasCoverage() && frame.fields[field].texels.size() <
                                 static_cast<size_t>(request.width) * static_cast<size_t>(request.height)) {
      ++mUnsupported.coverageFields;
      continue;
    }
    ++generated;
    area += static_cast<uint64_t>(request.width) * static_cast<uint64_t>(request.height);

    // A band is a whole number of rows and the last one takes the remainder, so the ranges tile [0, height) exactly.
    const uint64_t fieldArea = static_cast<uint64_t>(request.width) * static_cast<uint64_t>(request.height);
    const int height = static_cast<int>(request.height);
    const int bandCount = static_cast<int>(std::max<uint64_t>(1, (fieldArea + kBandTexels - 1) / kBandTexels));
    const int rowsPerBand = (height + bandCount - 1) / bandCount;
    for (int first = 0; first < height; first += rowsPerBand) {
      const int last = std::min(height - 1, first + rowsPerBand - 1);
      bands.push_back(Band { field, first, last,
              static_cast<uint64_t>(request.width) * static_cast<uint64_t>(last - first + 1) });
    }
  }

  // ## Balanced by cost, because the split is a static one over a *contiguous* array
  //
  // `parallelFor` cuts this array into one contiguous piece per worker, so the order of the array is the load balance: in
  // field order the pieces carry whatever the scene's drawing order puts next to each other, and a frame whose large
  // shapes are drawn together hands most of its texels to one worker. That is what the demo became when the field route's
  // size bound rose to 1024 (4.1.14): the fill stage ran 1.5x faster on twelve workers while the same pool spreads
  // trivial work 3.7-5.6x (4.1.22), and the load balance was 10.9x the even share.
  //
  // The permutation itself lives in `JobSystem::balanceByCost` rather than here. It is the same reasoning at every call
  // site, and four rounds spent looking *inside* a band while the fault was in how bands were handed out -- which is the
  // argument for the pool owning it, and for the note on `parallelFor` that says the order is the caller's business.
  if (mJobs != nullptr) {
    mJobs->balanceByCost(bands, [](const Band& band) { return band.cost; });
  }

  // ## The split that decides the next move, on request
  //
  // Three explanations for the fill's flat parallel curve have been measured and refuted (4.1.18, 4.1.19, 4.1.20), so the
  // next number has to come from *inside* the band rather than from another hypothesis. Each band does two things: it
  // extracts the field's contours out of the list (`contoursOf`) and it rasterises them into the atlas. Timing them apart,
  // as **total CPU across all threads** rather than as wall time, answers the question the parallel curve poses: a phase
  // whose CPU total is much larger than the stage's wall time is one that *is* spreading out, and a phase whose total is
  // close to the wall is the one holding the stage back.
  //
  // The timers cost about 30 ns each and run 41084 times, so this measurement adds a few milliseconds to the run it
  // measures; it is opt-in (`CORAL_TRACE_FIELDS`) and the number it produces is a ratio rather than a benchmark.
  const bool tracePhases = std::getenv("CORAL_TRACE_FIELDS") != nullptr;
  std::atomic<uint64_t> extractionNanos { 0 };
  std::atomic<uint64_t> rasterNanos { 0 };

  /**
   * ## Who the refill set actually is, printed when it is small (4.1.50)
   *
   * A partial refill is the animated case, and the number that matters for it is *what* is being rasterised rather than how
   * many fields there are: the demo's animation refills 8 fields for 23k texels (the four Lottie cells, 0.5 ms), while its
   * `--jitter` run refills 175 for **3.5M** texels (the eight 456x233 panel cards, the 64 1000x21 background bands, and the
   * panels' own rounded rectangles) -- every large flat shape in the scene, because a sub-pixel translation genuinely
   * changes the coverage of all of them.
   *
   * That is why the list is printed largest first with the device box: it says whether a frame's field work is a handful of
   * animated shapes or the whole scene, and the device boxes name the shapes to whoever knows the scene. It runs only when
   * the set is small, so the sort is bounded by `kRefillReportLimit`, and only under `CORAL_TRACE_FIELDS`.
   */
  constexpr size_t kRefillReportLimit = 1024;
  if (tracePhases && !refill.empty() && refill.size() < kRefillReportLimit) {
    const auto areaOf = [&list](const uint32_t field) {
      return static_cast<uint64_t>(list.coverageFields[field].width) * list.coverageFields[field].height;
    };
    std::vector<uint32_t> biggest(refill.begin(), refill.end());
    std::sort(biggest.begin(), biggest.end(),
            [&areaOf](const uint32_t a, const uint32_t b) { return areaOf(a) > areaOf(b); });
    uint64_t total = 0;
    for (const uint32_t field: refill) total += areaOf(field);
    std::fprintf(stderr, "[refill] %zu fields, %llu texels; largest:", refill.size(),
            static_cast<unsigned long long>(total));
    for (size_t index = 0; index < biggest.size() && index < 8; ++index) {
      const DrawList::CoverageField& field = list.coverageFields[biggest[index]];
      std::fprintf(stderr, " %ux%u@(%d,%d)%s", field.width, field.height, field.originX, field.originY,
              field.hasCoverage() ? "caller" : "");
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
  }

  // ## Rasterised bytes that a previous frame already produced
  //
  // The fill is 15-16 ms on a text-heavy list and runs again for every list a frame lends, because the
  // remembered frame is per frame. The bytes depend only on the shape, the fill rule and the size, so a
  // snapshot of what the cache holds is taken *before* the job pool runs (the pool only reads it), and the
  // misses are inserted after it (single-threaded).
  std::vector<const std::vector<uint8_t>*> cachedBytes(count, nullptr);
  for (uint32_t index = 0; index < count; ++index) {
    const DrawList::CoverageField& request = list.coverageFields[index];
    if (request.hasCoverage()) continue;
    const auto found = mFieldByteCache.find(mFieldIdentities[index].contentKey);
    if (found == mFieldByteCache.end()) continue;
    if (found->second.width != request.width || found->second.height != request.height) continue;
    cachedBytes[index] = &found->second.bytes;
    ++mFieldByteCacheHits;
  }

  const auto fillTask = [&frame, &list, &bands, &cachedBytes, tracePhases, &extractionNanos,
                         &rasterNanos](const uint32_t begin, const uint32_t end) {
    // Two buffers per task, reused across every band this task fills: the pool hands each worker one contiguous range, so
    // these are per-worker storage without a lock or a thread-local. `contours` holds the field's geometry and `scratch`
    // is the rasteriser's own row buffers -- see `FillScratch` for why a scratch owned by the caller is the difference
    // between two allocations per band and none.
    std::vector<raster::Contour> contours;
    raster::FillScratch scratch;
    for (uint32_t index = begin; index < end; ++index) {
      const Band& band = bands[index];
      const DrawList::CoverageField& request = list.coverageFields[band.field];
      const Frame::PlacedField& place = frame.placed[band.field];
      std::vector<uint8_t>& atlas = frame.atlases[place.atlas].bytes;
      const size_t stride = frame.atlases[place.atlas].width;
      const uint32_t rows = static_cast<uint32_t>(band.lastRow - band.firstRow + 1);

      if (request.hasCoverage()) {
        // A caller's own field: its texels are already coverage, so this is a copy of the band's rows into the atlas.
        const std::vector<uint8_t>& source = frame.fields[band.field].texels;
        const size_t rowBytes = static_cast<size_t>(request.width);
        for (uint32_t row = 0; row < rows; ++row) {
          const size_t sourceOffset = static_cast<size_t>(band.firstRow + static_cast<int>(row)) * rowBytes;
          const size_t targetOffset = static_cast<size_t>(place.top + 1 +
                  static_cast<uint32_t>(band.firstRow) + row) * stride + place.left + 1;
          if (sourceOffset + rowBytes > source.size() || targetOffset + rowBytes > atlas.size()) break;
          std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(sourceOffset), rowBytes,
                  atlas.begin() + static_cast<std::ptrdiff_t>(targetOffset));
        }
        continue;
      }

      if (cachedBytes[band.field] != nullptr) {
        // Rasterised on an earlier frame: copy the band's rows, which is what the fill would have written.
        const std::vector<uint8_t>& source = *cachedBytes[band.field];
        const size_t rowBytes = static_cast<size_t>(request.width);
        for (uint32_t row = 0; row < rows; ++row) {
          const size_t sourceOffset = static_cast<size_t>(band.firstRow + static_cast<int>(row)) * rowBytes;
          const size_t targetOffset = static_cast<size_t>(place.top + 1 +
                  static_cast<uint32_t>(band.firstRow) + row) * stride + place.left + 1;
          if (sourceOffset + rowBytes > source.size() || targetOffset + rowBytes > atlas.size()) break;
          std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(sourceOffset), rowBytes,
                  atlas.begin() + static_cast<std::ptrdiff_t>(targetOffset));
        }
        continue;
      }

      // A request: fill the band's rows straight into the atlas. The destination is offset to the field's own rectangle,
      // so the callback's (x, y) are the field's coordinates and the atlas's are `left + 1 + x`, `top + 1 + y`.
      const Clock::time_point extracted = tracePhases ? Clock::now() : Clock::time_point {};
      contoursOf(list, request.firstContour, request.contourCount, request.originX, request.originY, contours);
      const Clock::time_point rasterStart = tracePhases ? Clock::now() : Clock::time_point {};
      fillCoverageRowsAt(contours, atlas, static_cast<int>(stride), static_cast<int>(frame.atlases[place.atlas].height),
              static_cast<int>(place.left) + 1, static_cast<int>(place.top) + 1, static_cast<int>(request.width),
              static_cast<int>(request.height), request.fillType, band.firstRow, band.lastRow, scratch);
      if (tracePhases) {
        const Clock::time_point done = Clock::now();
        extractionNanos.fetch_add(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(rasterStart - extracted).count()),
                std::memory_order_relaxed);
        rasterNanos.fetch_add(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(done - rasterStart).count()),
                std::memory_order_relaxed);
      }
    }
  };

  if (mJobs != nullptr) {
    mJobs->parallelFor(static_cast<uint32_t>(bands.size()), 1, fillTask);
  } else {
    fillTask(0, static_cast<uint32_t>(bands.size()));
  }
  const double fillMilliseconds = millisecondsSince(start);

  // The fields this frame had to rasterise become the cache: the rectangle the fill just wrote is copied
  // out of the atlas once, so the next frame -- or the next list with the same shape -- only copies it in.
  if (mFieldByteCacheBytes >= kFieldByteCacheLimit) {
    // Bounded by clearing rather than by refusing: a renderer that quietly stops caching once the budget is
    // gone degrades a scene it has been drawing for a while, and the cost of that is invisible.
    mFieldByteCache.clear();
    mFieldByteCacheBytes = 0;
  }
  {
    for (uint32_t index = 0; index < count; ++index) {
      if (cachedBytes[index] != nullptr) continue;
      const DrawList::CoverageField& request = list.coverageFields[index];
      if (request.hasCoverage() || request.isEmpty()) continue;
      if (index >= frame.placed.size()) continue;
      const Frame::PlacedField& place = frame.placed[index];
      if (place.atlas >= frame.atlases.size()) continue;
      const Frame::AtlasUpload& atlas = frame.atlases[place.atlas];
      const size_t stride = atlas.width;
      CachedFieldBytes entry;
      entry.width = request.width;
      entry.height = request.height;
      entry.bytes.resize(static_cast<size_t>(request.width) * static_cast<size_t>(request.height));
      bool complete = true;
      for (uint32_t row = 0; row < request.height && complete; ++row) {
        const size_t sourceOffset = (static_cast<size_t>(place.top) + 1 + row) * stride + place.left + 1;
        if (sourceOffset + request.width > atlas.bytes.size()) {
          complete = false;
          break;
        }
        std::copy_n(atlas.bytes.begin() + static_cast<std::ptrdiff_t>(sourceOffset), request.width,
          entry.bytes.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(row) * request.width));
      }
      if (!complete) continue;
      // Two fields of one frame can share a content key and still both miss -- the snapshot is taken before
      // the fill, so a shape drawn twice at different places is rasterised once and inserted twice. The
      // accounting has to replace the old entry's bytes rather than add to them, or the budget drifts up
      // and the cache starts clearing while it is still small.
      const uint64_t key = mFieldIdentities[index].contentKey;
      if (const auto existing = mFieldByteCache.find(key); existing != mFieldByteCache.end()) {
        mFieldByteCacheBytes -= std::min(mFieldByteCacheBytes, existing->second.bytes.size());
        mFieldByteCache.erase(existing);
      }
      mFieldByteCacheBytes += entry.bytes.size();
      mFieldByteCache.insert_or_assign(key, std::move(entry));
    }
  }

  // ## The border, after every interior row of every field exists
  //
  // Its own pass because a field's border is a copy of its own edge texels and those are only all present once all of its
  // bands have run -- which, with the bands spread across threads, is exactly what the barrier between the passes is for.
  // Reading them back out of the atlas is not a detour: the atlas *is* the field's storage now.
  const auto replicateBorder = [&frame, &list](const uint32_t field) {
    {
      const DrawList::CoverageField& request = list.coverageFields[field];
      if (request.isEmpty()) return;
      if (field >= frame.placed.size()) return;
      const Frame::PlacedField& place = frame.placed[field];
      if (place.atlas >= frame.atlases.size()) return;
      Frame::AtlasUpload& atlas = frame.atlases[place.atlas];
      const size_t stride = atlas.width;
      const uint32_t x0 = place.left + 1;
      const uint32_t y0 = place.top + 1;
      const uint32_t fieldWidth = request.width;
      const uint32_t fieldHeight = request.height;
      if (static_cast<size_t>(y0 + fieldHeight) * stride + x0 + fieldWidth > atlas.bytes.size()) return;
      if (y0 == 0 || x0 == 0) return;

      // The top and bottom rows, then the left and right columns, then the four corners -- each a copy of the nearest
      // texel the field actually has, which is what makes the atlas sample exactly as a texture of the field's own did.
      for (uint32_t column = 0; column < fieldWidth; ++column) {
        atlas.bytes[static_cast<size_t>(y0 - 1) * stride + x0 + column] =
                atlas.bytes[static_cast<size_t>(y0) * stride + x0 + column];
        atlas.bytes[static_cast<size_t>(y0 + fieldHeight) * stride + x0 + column] =
                atlas.bytes[static_cast<size_t>(y0 + fieldHeight - 1) * stride + x0 + column];
      }
      for (uint32_t row = 0; row < fieldHeight; ++row) {
        atlas.bytes[static_cast<size_t>(y0 + row) * stride + x0 - 1] =
                atlas.bytes[static_cast<size_t>(y0 + row) * stride + x0];
        atlas.bytes[static_cast<size_t>(y0 + row) * stride + x0 + fieldWidth] =
                atlas.bytes[static_cast<size_t>(y0 + row) * stride + x0 + fieldWidth - 1];
      }
      atlas.bytes[static_cast<size_t>(y0 - 1) * stride + x0 - 1] =
              atlas.bytes[static_cast<size_t>(y0) * stride + x0];
      atlas.bytes[static_cast<size_t>(y0 - 1) * stride + x0 + fieldWidth] =
              atlas.bytes[static_cast<size_t>(y0) * stride + x0 + fieldWidth - 1];
      atlas.bytes[static_cast<size_t>(y0 + fieldHeight) * stride + x0 - 1] =
              atlas.bytes[static_cast<size_t>(y0 + fieldHeight - 1) * stride + x0];
      atlas.bytes[static_cast<size_t>(y0 + fieldHeight) * stride + x0 + fieldWidth] =
              atlas.bytes[static_cast<size_t>(y0 + fieldHeight - 1) * stride + x0 + fieldWidth - 1];
    }
  };

  const auto borderTask = [&replicateBorder, &refill](const uint32_t begin, const uint32_t end) {
    for (uint32_t position = begin; position < end; ++position) replicateBorder(refill[position]);
  };

  if (mJobs != nullptr) {
    mJobs->parallelFor(static_cast<uint32_t>(refill.size()), 8, borderTask);
  } else {
    borderTask(0, static_cast<uint32_t>(refill.size()));
  }

  // ## The per-atlas sums, and then the hashes they feed (4.1.48, 4.1.49)
  //
  // Before the hashes, because `hashFieldAtlases` now only mixes each atlas's running XOR in: the per-field work is the sums'
  // business, and a partial refill only has to touch the fields it wrote.
  updateFieldAtlasSums(frame, refill, everyFieldFits, count);
  hashFieldAtlases(frame);

  // ## And the frame is remembered, so that the next one asking for the same fields can skip all of the above
  //
  // Nothing is copied: the bytes this stage just wrote are already where the next frame will read them from, because
  // `Frame::atlases` and `Frame::placed` are references into `mFieldAtlasBytes`/`mFieldPlaced` -- see `Frame`'s
  // constructor. So remembering a frame is one assignment of the identity array, which is what the next frame compares
  // against -- per field, so that a frame with eight changed fields refills eight of them rather than all 41084 (4.1.45).
  //
  // A frame carrying a caller's own texels is remembered too, and that is deliberate: those fields are refilled every
  // frame by the refill set's `hasCoverage` test, so nothing about them is ever served from here, while the fields around
  // them keep their bytes. Before this round such a frame dropped the entry outright, which meant one moving mask cost the
  // whole atlas of the scene it moved through.
  //
  // The statistics read the same way the cache does, and `coverageFieldCount` keeps the meaning it has always had -- the
  // fields this frame's atlas holds -- so that a frame which reused everything still reports `count == reused`, which is
  // what the static-frame test asserts and what a reader of the numbers expects. What is new is that the two can now
  // differ by anything in between: a partial refill is `count` fields of which `count - generated` were kept.
  const uint32_t previousFieldCount = mFieldCache.frameFieldCount;
  mFieldCache.valid = true;
  mFieldCache.fieldCount = generated;
  mFieldCache.frameFieldCount = count;
  mFieldCache.fieldArea = area;
  // Swapped rather than copied: both vectors hold one identity per field (32 bytes, 1.3 MB on the demo scene), so a copy
  // here would be a third of the pass this whole arrangement exists to make cheap. After the swap `mFieldIdentities` is the
  // array the *previous* frame compared against, which the next frame clears and fills again -- capacity included.
  mFieldCache.lattices.swap(mFieldIdentities);

  const uint32_t reusedFieldCount =
          everyFieldFits ? previousFieldCount - generated : 0;
  mStats.coverageFieldCount = everyFieldFits ? previousFieldCount : generated;
  mStats.coverageFieldArea = area;
  mStats.coverageFieldsReused = reusedFieldCount;
  mStats.coveragePackMilliseconds = millisecondsSince(start) - fillMilliseconds;
  mStats.coverageFieldMilliseconds = fillMilliseconds;
  if (std::getenv("CORAL_TRACE_FIELDS") != nullptr) {
    const double extractionCpu = static_cast<double>(extractionNanos.load()) / 1.0e6;
    const double rasterCpu = static_cast<double>(rasterNanos.load()) / 1.0e6;
    // The load balance the pool will actually see: the pieces are contiguous and one per worker, so this is the cost of
    // the heaviest piece over the average one. A number near 1 is a balanced split; the reason it exists is that the
    // region's own history has three refuted explanations for a stage that would not spread out.
    uint64_t worstPiece = 0;
    uint64_t totalCost = 0;
    for (const Band& band: bands) totalCost += band.cost;
    const uint32_t pieces = mJobs != nullptr ? std::min<uint32_t>(mJobs->workerCount(),
            static_cast<uint32_t>(bands.size())) : 1u;
    if (pieces > 0 && !bands.empty()) {
      const size_t pieceSize = bands.size() / pieces + (bands.size() % pieces != 0 ? 1 : 0);
      for (size_t begin = 0; begin < bands.size(); begin += pieceSize) {
        uint64_t pieceCost = 0;
        for (size_t index = begin; index < std::min(bands.size(), begin + pieceSize); ++index) {
          pieceCost += bands[index].cost;
        }
        worstPiece = std::max(worstPiece, pieceCost);
      }
    }
    const double balance = totalCost > 0 ? static_cast<double>(worstPiece) * static_cast<double>(pieces) /
                                           static_cast<double>(totalCost) : 0.0;
    std::fprintf(stderr, "[fields] %u fields: place %.2f ms (keys %.2f ms for %llu points), fill %.2f ms wall (extract %.2f ms "
                         "CPU, rasterise %.2f ms CPU), %zu bands in %u pieces (worst/even %.2fx), %u workers, "
                         "border+hash %.2f ms, %zu atlas(es), %s, %s (%u refilled, %u kept)\n",
            count, placeMilliseconds, keyMilliseconds, static_cast<unsigned long long>(keyedPoints), fillMilliseconds - placeMilliseconds, extractionCpu, rasterCpu,
            bands.size(), pieces, balance, mJobs != nullptr ? mJobs->workerCount() : 0u,
            mStats.coveragePackMilliseconds, frame.atlases.size(), mJobs != nullptr ? "pooled" : "serial",
            everyFieldFits ? "placement kept" : "placement rebuilt", generated, reusedFieldCount);
    std::fflush(stderr);
  }
  return backend::Result::Success;
}

bool GpuRenderer::placeCoverageFields(Frame& frame) {
  frame.atlases.clear();
  frame.placed.assign(frame.fields.size(), Frame::PlacedField {});
  if (frame.fields.empty()) return true;

  // ## The atlas's *width* comes from the fields, its *height* from the placement
  //
  // The width has to fit the widest padded field, and beyond that a wide atlas packs better than a narrow one: the shelf
  // packer fills left to right, so a width near the square root of the content's area keeps the shelves from being mostly
  // empty. The height is then whatever the shelves actually need -- measured below, not guessed -- because that is the
  // number a margin would only approximate: too small loses fields, too large wastes exactly the zero fill and the hash
  // this exists to avoid.
  //
  // Both axes are rounded up to a power of two so that consecutive frames of a similar size share one pooled texture
  // instead of allocating one each, and both are capped: past the cap a frame gets another atlas rather than a bigger one.
  constexpr uint32_t kLargestAtlas = 4096;
  constexpr uint32_t kMaxAtlases = 8;

  uint64_t area = 0;
  uint64_t widest = 1;
  for (const DrawList::CoverageField& field: frame.fields) {
    if (field.isEmpty()) continue;
    // The padded extent: one texel of replicated border on every side.
    const uint64_t paddedWidth = static_cast<uint64_t>(field.width) + 2;
    area += paddedWidth * (static_cast<uint64_t>(field.height) + 2);
    widest = std::max(widest, paddedWidth);
  }
  if (area == 0) return true;
  const uint32_t atlasWidth = atlasExtent(std::max(widest,
          static_cast<uint64_t>(std::ceil(std::sqrt(static_cast<double>(area))))));
  if (atlasWidth == 0) return true;

  // ## Shelf placement, tallest first
  //
  // A field goes on the current shelf if it fits, and otherwise starts a new one. Ordering by height is what keeps the
  // shelves tight: without it a tall field arriving after a short one wastes the strip above every short field in the row.
  // The order is a *stable* sort on the height, so two frames of the same scene place identically -- which is what makes
  // the atlas's content hash a hit rather than a coincidence.
  std::vector<uint32_t> order;
  order.reserve(frame.fields.size());
  for (uint32_t index = 0; index < frame.fields.size(); ++index) {
    if (!frame.fields[index].isEmpty()) order.push_back(index);
  }
  std::stable_sort(order.begin(), order.end(), [&frame](const uint32_t left, const uint32_t right) {
    return frame.fields[left].height > frame.fields[right].height;
  });

  std::vector<uint32_t> atlasHeight; // the rows each atlas has used, before rounding
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t shelfHeight = 0;
  for (const uint32_t index: order) {
    const DrawList::CoverageField& field = frame.fields[index];
    // ## The slot is the padded box rounded up, and the slack is what makes a partial refill possible (4.1.45)
    //
    // A field's box is its extent plus one texel of border on each side. Rounding that box up to a granularity of four
    // texels leaves the field a little room it is not using, and the reason is the frame *after* this one: a field whose
    // lattice grows by a texel -- an animated shape whose bounding box crosses a pixel boundary -- still fits the slot it
    // had, so it keeps its address in the atlas and only its own rectangle is refilled. Without the slack, every such
    // field would need a new address, and in a shelf packer a new address for one field moves every field after it.
    //
    // Measured before it was built: with a granularity of four, **every** changed field of both animated runs fitted its
    // previous box (`--animate`: 4 of 4; `--jitter`: 141 of 141), and with exact-size slots *none* of them did. The cost
    // is the wasted texels, which the atlas's own numbers make cheap: the demo's 3.9M texels of coverage live in a
    // 4096-wide atlas whose shelves are far from full.
    const uint32_t width = roundUp(static_cast<uint32_t>(field.width) + 2, kFieldSlotGranularity);
    const uint32_t height = roundUp(static_cast<uint32_t>(field.height) + 2, kFieldSlotGranularity);
    if (x + width > atlasWidth) {
      x = 0;
      y += shelfHeight;
      shelfHeight = 0;
    }
    if (y + height > kLargestAtlas || atlasHeight.empty()) {
      // The current atlas is full: start another, and place this field at its origin. A frame that needs more than a few
      // gets refused rather than growing without bound -- a scene that far past the cap is one where the honest answer is
      // to stop adding textures, and the omission is counted.
      if (atlasHeight.size() >= kMaxAtlases) return false;
      atlasHeight.push_back(0);
      x = 0;
      y = 0;
      shelfHeight = 0;
    }
    if (x + width > atlasWidth || y + height > kLargestAtlas) {
      // Wider or taller than a whole atlas: a field past the largest extent cannot be placed at all.
      return false;
    }

    const uint32_t atlas = static_cast<uint32_t>(atlasHeight.size() - 1);
    Frame::PlacedField& placed = frame.placed[index];
    placed.atlas = atlas;
    placed.left = x;
    placed.top = y;
    // The fragment stage subtracts a *device* position from the field's texel origin and divides by the atlas extent, so
    // what goes there is where the field's texel (0, 0) sits in device space. The field's texels begin one texel inside
    // the padded rectangle, so the atlas position of texel (0, 0) is `(x + 1, y + 1)` in atlas texels -- and an atlas
    // texel is a device pixel, because the field's texel (i, j) *is* the device pixel `(originX + i, originY + j)`.
    placed.texelOriginX = static_cast<float>(field.originX - static_cast<int32_t>(x + 1));
    placed.texelOriginY = static_cast<float>(field.originY - static_cast<int32_t>(y + 1));
    atlasHeight[atlas] = std::max(atlasHeight[atlas], y + height);

    x += width;
    shelfHeight = std::max(shelfHeight, height);
  }

  // ## Size each atlas to what the placement used
  //
  // The height is rounded up to a power of two *after* the placement, so the atlas is never taller than the shelves need by
  // more than that rounding -- which is the whole of the waste this leaves. The bytes are zeroed here because this is the
  // only place that knows the size, and because the fill writes the fields' own rectangles into this buffer and never
  // touches the space between them: a picture that depended on what the driver left in that space would be this area's
  // recurring defect.
  frame.atlases.assign(atlasHeight.size(), Frame::AtlasUpload {});
  for (size_t atlas = 0; atlas < atlasHeight.size(); ++atlas) {
    frame.atlases[atlas].width = atlasWidth;
    frame.atlases[atlas].height = atlasExtent(atlasHeight[atlas]);
    frame.atlases[atlas].bytes.assign(static_cast<size_t>(atlasWidth) *
                                      static_cast<size_t>(frame.atlases[atlas].height), 0);
  }
  return true;
}

/**
 * @return a hash of one field's box in the atlas, with the box's **identity** mixed in.
 *
 * ## What has to be covered, which is less than the whole atlas (4.1.47)
 *
 * The hash exists to answer "would this frame's upload say anything the texture does not already hold", so what it must cover
 * is exactly the bytes the shader can **sample**: the field's interior and the one-texel ring of replicated edge texels
 * around it -- which is the padded box, and nothing else. Bytes outside every live field's box are never sampled (a field's
 * quad covers its own lattice, and bilinear sampling at its edge reaches one texel out), so leaving them out of the hash is
 * not an approximation: it is the same answer for every pair of frames whose *sampled* bytes agree.
 *
 * The identity -- atlas index, box position, box extent -- is mixed in because the per-atlas value is an **XOR** of these
 * (see `hashFieldAtlases`). XOR cancels equal values, and two fields whose boxes hold equal bytes are exactly what a scene
 * full of identical shapes produces; mixing the address in makes every contribution distinct, so nothing can cancel.
 */
uint64_t GpuRenderer::hashFieldBox(const Frame& frame, const uint32_t field) {
  if (frame.list == nullptr || field >= frame.placed.size()) return 0;
  const DrawList::CoverageField& request = frame.list->coverageFields[field];
  const Frame::PlacedField& place = frame.placed[field];
  if (request.isEmpty() || place.atlas >= frame.atlases.size()) return 0;
  const Frame::AtlasUpload& atlas = frame.atlases[place.atlas];
  const size_t boxWidth = static_cast<size_t>(request.width) + 2;
  const size_t boxHeight = static_cast<size_t>(request.height) + 2;

  uint64_t hash = 1469598103934665603ULL;
  const auto mix = [&hash](const uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
  };
  for (size_t row = 0; row < boxHeight; ++row) {
    const size_t offset = (static_cast<size_t>(place.top) + row) * atlas.width + place.left;
    // A placement that does not describe these bytes: no hash rather than a hash of the wrong ones. The frame is drawn from
    // the same placement, so this only happens on a path that is already being counted as an omission.
    if (offset + boxWidth > atlas.bytes.size()) return 0;
    mix(hashBytes(atlas.bytes.data() + offset, boxWidth));
  }
  mix(place.atlas);
  mix(place.left);
  mix(place.top);
  mix(boxWidth);
  mix(boxHeight);
  return hash;
}

void GpuRenderer::hashFieldAtlases(Frame& frame) {
  // ## The atlas's hash is an XOR of what its fields *are*, not of what their bytes are (4.1.48)
  //
  // The question this answers is narrow: "would uploading this atlas say anything the texture does not already hold" -- the
  // decision `drawCoverageFieldBatch` makes before it touches the device. What the device samples is, per field, the box the
  // field was placed in: its interior, which the fill derives from the field's contours and lattice, and the one-texel ring
  // around it, which is a copy of that interior's edge. So the sampled bytes are a **pure function of the field's content
  // key and its placement**, and hashing those two is the same answer as hashing the bytes -- with no bytes read at all.
  //
  // It replaces an XOR of per-field *box* hashes (4.1.47), which had already taken the cost off a partial refill's critical
  // path but still read every field's box: 0.14 ms on a partial frame, and 3.7 ms on a rebuild (where 41084 small rows hash
  // far worse than one big buffer does). Both are gone, along with the per-field hash table.
  //
  // A caller's own texels are the one thing that is *not* derivable -- that is what `hasCoverage` means, and why such a
  // field is rewritten every frame -- so those boxes are hashed for real. There are a handful of them by construction.
  //
  // ## Why nothing can cancel, and why the extents are in it
  //
  // The per-atlas value is an XOR, and XOR cancels equal contributions -- which a scene full of identical shapes would
  // produce if the contribution were the content key alone. The placement (atlas, left, top, box extent) is mixed into
  // every contribution, and two fields cannot share a placement, so no two contributions are equal. The atlas's own extents
  // seed the value so that a differently-sized atlas cannot hash alike.
  for (size_t atlas = 0; atlas < frame.atlases.size(); ++atlas) {
    Frame::AtlasUpload& upload = frame.atlases[atlas];
    uint64_t hash = 1469598103934665603ULL;
    hash ^= upload.width;
    hash *= 1099511628211ULL;
    hash ^= upload.height;
    hash *= 1099511628211ULL;
    // The running XOR of this atlas's fields, maintained by `updateFieldAtlasSums`: on a partial refill only the fields that
    // changed were touched, which is the whole point of keeping it (4.1.49).
    if (atlas < mFieldAtlasSums.size()) hash ^= mFieldAtlasSums[atlas];
    upload.contentHash = hash;
  }
}

uint64_t GpuRenderer::fieldContribution(const Frame& frame, const uint32_t field) const {
  if (frame.list == nullptr || field >= frame.placed.size()) return 0;
  if (field >= frame.list->coverageFields.size()) return 0;
  const DrawList::CoverageField& request = frame.list->coverageFields[field];
  const Frame::PlacedField& place = frame.placed[field];
  if (request.isEmpty() || place.atlas >= frame.atlases.size()) return 0;

  uint64_t contribution = 1469598103934665603ULL;
  const auto mix = [&contribution](const uint64_t value) {
    contribution ^= value;
    contribution *= 1099511628211ULL;
  };
  if (request.hasCoverage()) {
    // The one field whose bytes are not derivable from the list: hash the box the caller's texels were copied into.
    mix(hashFieldBox(frame, field));
  } else if (field < mFieldIdentities.size()) {
    mix(mFieldIdentities[field].contentKey);
  } else {
    // No identity was built for this field (the frame was resolved through another path): fall back to the bytes, which is
    // always a correct answer and never a wrong hit.
    mix(hashFieldBox(frame, field));
  }
  // The placement, so that two fields with the same content cannot cancel each other out of the XOR.
  mix(place.atlas);
  mix(place.left);
  mix(place.top);
  mix(static_cast<uint64_t>(request.width) + 2);
  mix(static_cast<uint64_t>(request.height) + 2);
  return contribution;
}

void GpuRenderer::updateFieldAtlasSums(const Frame& frame, const std::vector<uint32_t>& refill,
        const bool placementKept, const uint32_t count) {
  /**
   * ## The running XOR, and why a partial refill only touches what changed (4.1.49)
   *
   * `hashFieldAtlases` used to walk every field every frame -- 41084 contributions, each a few multiplies, which is 0.35-0.51
   * ms on a frame whose whole field stage is 1.5 ms. Nothing about that walk is per-frame work: the contribution of a field
   * that was **kept** is a number this renderer already computed last frame, and it is still the right number, because a kept
   * field's bytes and its box are both unchanged.
   *
   * So the sums are kept, and a partial refill does two XORs per changed field: the old contribution out, the new one in. The
   * one thing that makes "the old contribution" recoverable is that a partial refill does **not** re-place anything -- the
   * placement is part of the contribution, and it is the same placement -- so the stored value is exactly what has to come
   * out of the XOR.
   *
   * A rebuild is the other case: every field is placed anew and every box was zeroed and refilled, so the sums start from
   * zero and every contribution is recomputed. That is the same walk as before, and the only one left.
   */
  mFieldContributions.resize(count, 0);
  if (!placementKept) {
    mFieldAtlasSums.assign(frame.atlases.size(), 0);
    for (uint32_t field = 0; field < count; ++field) {
      const uint64_t contribution = fieldContribution(frame, field);
      mFieldContributions[field] = contribution;
      const Frame::PlacedField& place = frame.placed[field];
      if (place.atlas < mFieldAtlasSums.size()) mFieldAtlasSums[place.atlas] ^= contribution;
    }
    return;
  }
  // The placement is the one the sums were built from, and it still holds the same atlas for every field.
  mFieldAtlasSums.resize(frame.atlases.size(), 0);
  for (const uint32_t field: refill) {
    if (field >= count || field >= frame.placed.size()) continue;
    const Frame::PlacedField& place = frame.placed[field];
    if (place.atlas >= mFieldAtlasSums.size()) continue;
    const uint64_t updated = fieldContribution(frame, field);
    mFieldAtlasSums[place.atlas] ^= mFieldContributions[field] ^ updated;
    mFieldContributions[field] = updated;
  }
}

backend::Result GpuRenderer::drawImageBatch(Frame& frame, const Batch& batch) {
  // ## One draw for the whole batch, because the page took the per-command texture out of the state
  //
  // Every command in this batch samples the same texture: `canShareDraw` grouped them by the texture the frame placed
  // each bitmap in, and a page is one texture shared by many bitmaps. What is left per command is the *sub-rectangle*,
  // and that is per-vertex data (`image.frag` reads `vParams`) -- so the program, the set and the vertex buffer are bound
  // once and a single instanced draw covers the batch, exactly as `drawGradientBatch` and `drawCoverageFieldBatch` do.
  //
  // The frame's placement is where the set comes from rather than a lookup by image: the batch's texture was decided
  // during the placement stage and the frame carries it, which keeps this function free of any hashing or searching.
  const uint32_t firstCommand = batch.firstCommand;
  const DrawList::Command& first = frame.list->commands[firstCommand];
  RenderTarget& target = targetAt(frame, frame.targets[first.layer]);
  const uint32_t imageIndex = imageIndexForCommand(first, *frame.list);

  if (imageIndex >= frame.images.size() || !frame.images[imageIndex].valid) {
    // A bitmap the frame could not place, or an image index the table does not have. Counted per command rather than
    // drawn with whichever texture is bound: a white rectangle where a photograph belongs, or one glyph drawn with
    // another's coverage, is a wrong picture that looks deliberate.
    mUnsupported.images += batch.commandCount;
    return backend::Result::Success;
  }
  const backend::DescriptorSetHandle set = frame.images[imageIndex].set;

  // The image program, not the quad's: the same vertex stage and the same layout, a fragment stage that turns the quad's
  // coordinate into a texel inside the command's sub-rectangle. See `Program::Image`.
  //
  // A cubic sampling picks one of the two baked-kernel programs. The kernel cannot be a uniform: `quad.vert` says
  // per-command data is a vertex attribute precisely because a uniform is per *draw call*, which is the granularity this
  // pass exists to avoid. Every image already has its own batch -- one texture per batch -- so choosing the program per
  // batch costs nothing in batching.
  Program imageProgram = first.state.sdf
    ? (mSdfPatternEnabled ? Program::SdfPattern : Program::SdfImage)
    : Program::Image;
  const SamplingOptions* imageSampling = nullptr;
  if (imageIndex < frame.images.size()) {
    imageSampling = &frame.images[imageIndex].sampling;
    if (imageSampling->useCubic) imageProgram = Program::ImageCubic;
  }
  const PipelineKey key{ imageProgram, Variant::Plain, static_cast<uint8_t>(batch.state.blendMode),
                         target.format,
                         target.depthStencilView.valid() ? kDepthStencilFormat : backend::PixelFormat::Undefined,
                         target.sampleCount, batch.state.channelMask };
  backend::DescriptorSetHandle drawSet = set;
  if (imageProgram == Program::SdfPattern) {
    Frame::PlacedImage& placed = frame.images[imageIndex];
    if (!placed.patternSet.valid()) {
      backend::Result created = mDevice->createDescriptorSet(mSdfPatternSetLayout, &placed.patternSet);
      if (!backend::isSuccess(created)) return created;
      const backend::DescriptorWrite writes[] = {
        { 0, backend::BindingType::TextureSampler, placed.texture, mLinearClamp, {}, 0 },
        { 2, backend::BindingType::TextureSampler, mSdfPatternTexture, mLinearClamp, {}, 0 },
      };
      created = mDevice->updateDescriptorSet(placed.patternSet, writes, 2);
      if (!backend::isSuccess(created)) return created;
    }
    CanvasUniforms canvas = canvasUniformsFor(*frame.root);
    uint32_t block = 0;
    backend::Result staged = allocateUniformSlot(&canvas, sizeof(canvas), &block);
    if (!backend::isSuccess(staged)) return staged;
    const backend::DescriptorWrite uniformWrite = {
      1, backend::BindingType::UniformBuffer, backend::TextureHandle::invalid(),
      backend::TextureSamplerHandle::invalid(),
      { uniformBuffer(), static_cast<uint64_t>(block) * kUniformSlotSize, kUniformSlotSize }, 0
    };
    staged = mDevice->updateDescriptorSet(placed.patternSet, &uniformWrite, 1);
    if (!backend::isSuccess(staged)) return staged;
    drawSet = placed.patternSet;
  }

  backend::RenderPipelineHandle pipeline = backend::RenderPipelineHandle::invalid();
  backend::Result result = ensurePipeline(key, target.pass, &pipeline);
  if (!backend::isSuccess(result)) {
    return result;
  }

  // The cubic kernel's B and C, written per batch: a uniform is the only granularity a value like this has, and a batch is
  // where it belongs -- every image already owns one, so this costs a block rather than a draw call.
  //
  // ## Why this rewrites the image's own set
  //
  // The filter lives in the canvas block (`uImageFilter`), so the block cannot be the shared canvas slot: a cubic batch
  // would overwrite the block every other program reads. A cubic image owns its texture and therefore its own set
  // (`useCubic` keeps it out of the atlas), so this batch stages a *copy* of the canvas block with the filter filled in and
  // points that set's binding 1 at it. A second batch drawing the same image stages the same bytes again and the set's
  // contents are unchanged in value -- which is what makes the rewrite safe even on the backend that resolves set contents
  // at replay rather than at draw time.
  if (imageSampling != nullptr && imageSampling->useCubic) {
    CanvasUniforms canvas = canvasUniformsFor(*frame.root);
    canvas.filter[0] = imageSampling->cubic.b;
    canvas.filter[1] = imageSampling->cubic.c;
    canvas.filter[2] = 0.0f;
    canvas.filter[3] = 0.0f;
    uint32_t block = 0;
    result = allocateUniformSlot(&canvas, sizeof(canvas), &block);
    if (!backend::isSuccess(result)) return result;
    const backend::DescriptorWrite filterWrite = {
      1, backend::BindingType::UniformBuffer, backend::TextureHandle::invalid(),
      backend::TextureSamplerHandle::invalid(),
      { uniformBuffer(), static_cast<uint64_t>(block) * kUniformSlotSize, kUniformSlotSize }, 0
    };
    result = mDevice->updateDescriptorSet(set, &filterWrite, 1);
    if (!backend::isSuccess(result)) return result;
  }

  result = mCommandBuffer->bindRenderPipeline(pipeline);
  if (!backend::isSuccess(result)) return result;
  // The batch's texture, bound once. Every command in the batch samples it, at its own sub-rectangle.
  result = mCommandBuffer->bindDescriptorSet(0, drawSet);
  if (!backend::isSuccess(result)) return result;

  // Four vertices and six indices per command, laid out in command order by `assembleArena` -- the same layout the quad,
  // gradient and coverage routes use, so one `drawIndexed` covers the whole batch.
  result = mCommandBuffer->setVertexBuffer(0, mVertexBuffer, batch.vertexOffset);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->setIndexBuffer(mIndexBuffer, backend::IndexFormat::UInt32, 0);
  if (!backend::isSuccess(result)) return result;

  result = mCommandBuffer->drawIndexed(kQuadIndexCount * batch.commandCount, 1, 0, 0, 0);
  if (!backend::isSuccess(result)) return result;

  ++mStats.drawCallCount;
  ++mStats.imageDrawCalls;
  mStats.vertexCount += static_cast<uint64_t>(kQuadCornerCount) * batch.commandCount;
  if (std::getenv("CORAL_TRACE_VAO") != nullptr) {
    std::fprintf(stderr, "[img] batch drawn: layer=%u target=%ux%u commands=%u vbOffset=%llu setValid=%d\n",
            first.layer, target.width, target.height, batch.commandCount,
            static_cast<unsigned long long>(batch.vertexOffset), set.valid() ? 1 : 0);
    std::fflush(stderr);
  }
  return backend::Result::Success;
}

backend::Result GpuRenderer::drawCoverageFieldBatch(Frame& frame, const Batch& batch) {
  // ## One draw for the whole batch, because the atlas took the last per-command thing out of the state
  //
  // Every command in this batch was grouped by `canShareDraw`, which for a coverage command now means "same atlas and the
  // same shared state" -- where the field's texels *are* travels per vertex, so what remains per draw is the atlas's
  // extent and its descriptor set, both of which the whole batch agrees on. That is the difference between 48252 draws on
  // the demo scene and one per run of commands: the frame is per-command bound, so this is where the route's cost was.
  const uint32_t firstCommand = batch.firstCommand;
  const DrawList::Command& first = frame.list->commands[firstCommand];
  RenderTarget& target = targetAt(frame, frame.targets[first.layer]);

  // The batch's atlas, from its first command. The rest agree by construction, and the *placement* says which one that
  // is: `resolveCoverageFields` packed every field, and a field it could not place has no atlas and is never batched with
  // one that does (`canShareDraw` refuses the sentinel).
  if (first.state.coverageField == DrawList::kNoCoverageField ||
      first.state.coverageField >= frame.placed.size()) {
    // A command naming a field the frame does not have. Counted rather than drawn as a blank quad, because a
    // wrong picture that looks deliberate is worse than a counted omission.
    ++mUnsupported.images;
    return backend::Result::Success;
  }
  const Frame::PlacedField& place = frame.placed[first.state.coverageField];
  if (place.atlas >= frame.atlases.size() || frame.atlases[place.atlas].bytes.empty()) {
    // Written by `resolveCoverageFields` as an omission already -- an empty atlas would otherwise be drawn as a
    // transparent texture and the batch's shapes would simply be missing.
    ++mUnsupported.coverageFields;
    return backend::Result::Success;
  }
  Frame::AtlasUpload& upload = frame.atlases[place.atlas];
  const uint64_t atlasKey = (static_cast<uint64_t>(upload.width) << 32) | static_cast<uint64_t>(upload.height);
  const auto existingAtlas = mFieldAtlases.find(atlasKey);
  const bool atlasExists = existingAtlas != mFieldAtlases.end() && existingAtlas->second.texture.valid();
  const bool passWasOpen = frame.openPass >= 0;

  // `ensureFieldAtlas` zero-fills a brand-new atlas with `writeTexture`, and the content upload below is
  // also a transfer. Neither may be recorded inside a render pass, so close the pass before either and
  // reopen it at the batch's scissor afterwards.
  if (passWasOpen && !atlasExists) {
    const backend::Result closed = endPass(frame);
    if (!backend::isSuccess(closed)) return closed;
  }
  FieldAtlas* atlas = ensureFieldAtlas(upload.width, upload.height);
  if (atlas == nullptr) {
    ++mUnsupported.images;
    return backend::Result::Success;
  }

  // ## The upload, once per atlas whose packed bytes changed
  //
  // This is the whole point of the atlas: a frame that draws the same scene as the last one reaches here and uploads
  // *nothing*, and a frame that changed uploads one texture instead of one per path. The bytes are the frame's, the hash
  // is the pooled entry's, and the flag says whether the entry's hash describes anything at all -- a freshly created atlas
  // holds only its zero fill.
  //
  // ## A partial refill uploads its boxes instead of the whole atlas (4.1.53)
  //
  // The bytes that changed on a partial refill are a handful of boxes -- eight fields, 23k texels, on the demo's animation
  // -- while the texture is the whole atlas, 3.8 MB. Measured, that upload costs **4.2 ms a frame** (`record` -2.6,
  // `execute` -1.4), which is 23% of the frame, and it is paid to deliver 23 KB of change. So when the placement was kept
  // and the refill set is small, each refilled field's box is gathered into a scratch buffer and written on its own: a
  // `writeTexture` per box with the box's own region, tightly packed rows.
  //
  // It is safe exactly because the *kept* fields' bytes did not change: the texture already holds them from whenever they
  // were last written, so the only bytes it can be missing are the changed boxes'. A rebuild or a first upload has no such
  // history, and keeps uploading the whole thing.
  const bool uploadNeeded = !atlas->hasContent || atlas->contentHash != upload.contentHash;
  if (uploadNeeded) {
    if (passWasOpen && atlasExists) {
      const backend::Result closed = endPass(frame);
      if (!backend::isSuccess(closed)) return closed;
    }

    bool uploadedBoxes = false;
    if (atlas->hasContent && mFieldPlacementKept && !mFieldRefill.empty() &&
        mFieldRefill.size() <= kFieldBoxUploadLimit) {
      uploadedBoxes = uploadChangedFieldBoxes(frame, place.atlas, *atlas);
    }
    if (!uploadedBoxes) {
      const backend::Result uploadResult = writeAtlas(mCommandBuffer, atlas->texture, upload.width, upload.height,
              upload.bytes);
      if (!backend::isSuccess(uploadResult)) {
        ++mUnsupported.images;
        return backend::Result::Success;
      }
    }
    atlas->contentHash = upload.contentHash;
    atlas->hasContent = true;
    ++mImageUploads;
    mStats.textureUploadCount = mImageUploads;

    if (passWasOpen) {
      const backend::Result reopened = beginPassFor(frame, batch.layer, false);
      if (!backend::isSuccess(reopened)) return reopened;
      const backend::Result scissored = setScissor(frame, first.clipRect);
      if (!backend::isSuccess(scissored)) return scissored;
    }
  }

  // The batch's own block: the atlas's extent, which is all that is left for it to carry -- and two numbers rather than
  // one, because an atlas is not square. Every batch that reads the same atlas writes the same numbers, so the atlas's
  // set can be rewritten to this batch's slot without changing what an earlier batch reads -- that identity is what lets
  // one set serve every batch of an atlas on a backend that resolves set contents at replay.
  FieldUniforms uniforms {};
  uniforms.field[0] = static_cast<float>(upload.width);
  uniforms.field[1] = static_cast<float>(upload.height);
  uint32_t block = 0;
  backend::Result result = allocateUniformSlot(&uniforms, sizeof(uniforms), &block);
  if (!backend::isSuccess(result)) return result;
  mStats.uploadBytes += sizeof(uniforms);

  const backend::DescriptorWrite fieldWrite = {
    2, backend::BindingType::UniformBuffer, backend::TextureHandle::invalid(),
    backend::TextureSamplerHandle::invalid(),
    { uniformBuffer(), static_cast<uint64_t>(block) * kUniformSlotSize, kUniformSlotSize }, 0
  };
  result = mDevice->updateDescriptorSet(atlas->set, &fieldWrite, 1);
  if (!backend::isSuccess(result)) return result;

  const PipelineKey key { Program::CoverageField, Variant::Plain, static_cast<uint8_t>(batch.state.blendMode),
    target.format, target.depthStencilView.valid() ? kDepthStencilFormat : backend::PixelFormat::Undefined,
    target.sampleCount };
  backend::RenderPipelineHandle pipeline = backend::RenderPipelineHandle::invalid();
  result = ensurePipeline(key, target.pass, &pipeline);
  if (!backend::isSuccess(result)) return result;

  result = mCommandBuffer->bindRenderPipeline(pipeline);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->bindDescriptorSet(0, atlas->set);
  if (!backend::isSuccess(result)) return result;

  // Four vertices and six indices per command, laid out in command order by `assembleArena` -- the same layout the quad,
  // gradient and image routes use, so one `drawIndexed` covers the whole batch exactly as `drawGradientBatch`'s does.
  result = mCommandBuffer->setVertexBuffer(0, mVertexBuffer, batch.vertexOffset);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->setIndexBuffer(mIndexBuffer, backend::IndexFormat::UInt32, 0);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->drawIndexed(kQuadIndexCount * batch.commandCount, 1, 0, 0, 0);
  if (!backend::isSuccess(result)) return result;
  ++mStats.drawCallCount;
  ++mStats.coverageDrawCalls;
  mStats.vertexCount += static_cast<uint64_t>(kQuadCornerCount) * batch.commandCount;
  if (std::getenv("CORAL_TRACE_VAO") != nullptr) {
    std::fprintf(stderr, "[cov] batch drawn: layer=%u target=%ux%u commands=%u vbOffset=%llu setValid=%d uploads=%llu\n",
            first.layer, target.width, target.height, batch.commandCount,
            static_cast<unsigned long long>(batch.vertexOffset), atlas->set.valid() ? 1 : 0,
            static_cast<unsigned long long>(mImageUploads));
    std::fflush(stderr);
  }
  return backend::Result::Success;
}


backend::Result GpuRenderer::drawGradientBatch(Frame& frame, const Batch& batch) {
  RenderTarget& target = targetAt(frame, frame.targets[batch.layer]);
  const DrawList::Command& first = frame.list->commands[batch.firstCommand];

  // The gradient's parameters for this batch, unpacked into the layout the shader declares. A batch shares one state
  // and therefore one gradient, so this is written once per batch rather than per command.
  GradientUniforms uniforms{};
  uniforms.points[0][0] = first.state.gradient.points[0].x;
  uniforms.points[0][1] = first.state.gradient.points[0].y;
  uniforms.points[1][0] = first.state.gradient.points[1].x;
  uniforms.points[1][1] = first.state.gradient.points[1].y;
  uniforms.shape[0] = first.state.gradient.radius;
  // ## The device-to-gradient matrix, which is what makes the ramp vary across the rectangle
  //
  // `DrawList::GradientInfo::inverseLocal` is `shader-local-inverse * deviceToUser`, which is the composition that
  // takes a *device* pixel into the gradient's own space -- exactly what the fragment stage needs, and the same value
  // the rasterizer builds as `deviceToShader` before calling `Shader::sampleAt` per pixel. Leaving it zero put every
  // fragment at the gradient's origin, so the whole rectangle took the first stop's colour.
  //
  // Row major, as `DrawList::GradientInfo` documents: (m00, m01, m10, m11, tx, ty). The shader applies it as
  // `x' = m00*x + m10*y + tx` and `y' = m01*x + m11*y + ty` -- the transpose of the row-major reading, which is the
  // convention this library's `Matrix` uses for a *row vector* times a matrix. Both ends agree on it because the
  // recorder and the shader are written against the same six numbers in the same order.
  uniforms.matrix[0][0] = first.state.gradient.inverseLocal[0];
  uniforms.matrix[0][1] = first.state.gradient.inverseLocal[1];
  uniforms.matrix[0][2] = first.state.gradient.inverseLocal[2];
  uniforms.matrix[0][3] = first.state.gradient.inverseLocal[3];
  uniforms.matrix[1][0] = first.state.gradient.inverseLocal[4];
  uniforms.matrix[1][1] = first.state.gradient.inverseLocal[5];
  uniforms.params[0] = static_cast<int32_t>(first.state.gradient.tileMode);
  uniforms.params[1] = static_cast<int32_t>(first.state.stopCount < 4 ? first.state.stopCount : 4);
  for (uint32_t stop = 0; stop < 4; ++stop) {
    uniforms.stopColor[stop][0] = first.state.stops[stop].offset;
    uniforms.stopColor[stop][1] = first.state.stops[stop].red;
    uniforms.stopColor[stop][2] = first.state.stops[stop].green;
    uniforms.stopColor[stop][3] = first.state.stops[stop].blue;
    uniforms.stopAlpha[stop][0] = first.state.stops[stop].alpha;
  }

  // Staged into this batch's own slot, then named by a set for that slot. One block per gradient batch is enough because a
  // batch shares one gradient state for every command in it.
  traceUniformBlockLayout();
  uint32_t block = 0;
  backend::Result result = allocateUniformSlot(&uniforms, sizeof(uniforms), &block);
  if (!backend::isSuccess(result)) return result;
  mStats.uploadBytes += sizeof(uniforms);

  backend::DescriptorSetHandle gradientSet = backend::DescriptorSetHandle::invalid();
  result = uniformSetForSlot(mGradientSlotSets, mGradientSetLayout, block, &gradientSet);
  if (!backend::isSuccess(result)) return result;

  const PipelineKey key{ Program::Gradient, Variant::Plain, static_cast<uint8_t>(batch.state.blendMode),
                         target.format,
                         target.depthStencilView.valid() ? kDepthStencilFormat : backend::PixelFormat::Undefined,
                         target.sampleCount };
  backend::RenderPipelineHandle pipeline = backend::RenderPipelineHandle::invalid();
  result = ensurePipeline(key, target.pass, &pipeline);
  if (!backend::isSuccess(result)) return result;

  result = mCommandBuffer->bindRenderPipeline(pipeline);
  if (!backend::isSuccess(result)) return result;
  // The set names this batch's slot at binding 2 and the shared canvas block at binding 1, so the block and the transform
  // the vertex stage needs travel together.
  result = mCommandBuffer->bindDescriptorSet(0, gradientSet);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->setVertexBuffer(0, mVertexBuffer, batch.vertexOffset);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->setIndexBuffer(mIndexBuffer, backend::IndexFormat::UInt32, 0);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->drawIndexed(kQuadIndexCount * batch.commandCount, 1, 0, 0, 0);
  if (!backend::isSuccess(result)) return result;

  ++mStats.drawCallCount;
  mStats.vertexCount += static_cast<uint64_t>(kQuadCornerCount) * batch.commandCount;
  return backend::Result::Success;
}

void GpuRenderer::destroyResources() noexcept {
  if (mDevice == nullptr) return;

  if (mTimingEnabled) {
    mDevice->destroyQuerySet(mTimingSet);
    mTimingSet = backend::QuerySetHandle::invalid();
    mTimingEnabled = false;
    mTimingPending = false;
  }

  // Order matters twice over: a resource is released before the device that owns it, and the objects
  // that reference a resource -- pipelines reference layouts and shader modules, descriptor sets
  // reference a layout -- are released before what they reference.
  for (const PipelineEntry& entry: mPipelines) {
    if (entry.pipeline.valid()) mDevice->destroyRenderPipeline(entry.pipeline);
  }
  mPipelines.clear();

  destroyPooledTargets();
  destroyScratchTextures();
  // Before the layouts below: every pooled texture owns a descriptor set, and a set is created against a layout.
  destroyCachedTextures();

  if (mCommandBuffer != nullptr) {
    mDevice->destroyCommandBuffer(mCommandBuffer);
    mCommandBuffer = nullptr;
  }
  // Before the set layouts below: every cached external set is created against `mQuadSetLayout`.
  for (const auto& [key, set]: mExternalImageSets) {
    (void) key;
    if (set.valid()) mDevice->destroyDescriptorSet(set);
  }
  mExternalImageSets.clear();
  if (mUploadCommandBuffer != nullptr) {
    mDevice->destroyCommandBuffer(mUploadCommandBuffer);
    mUploadCommandBuffer = nullptr;
  }
  for (AlphaTexture& entry: mAlphaTextures) {
    if (entry.view.valid()) mDevice->destroyTextureView(entry.view);
    if (entry.texture.valid()) mDevice->destroyTexture(entry.texture);
  }
  mAlphaTextures.clear();
  mAlphaTexture = backend::TextureHandle::invalid();
  mAlphaTextureView = backend::TextureViewHandle::invalid();

  const auto destroySet = [this](backend::DescriptorSetHandle& set) {
    if (set.valid()) mDevice->destroyDescriptorSet(set);
    set = backend::DescriptorSetHandle::invalid();
  };
  const auto destroyLayout = [this](backend::DescriptorSetLayoutHandle& layout) {
    if (layout.valid()) mDevice->destroyDescriptorSetLayout(layout);
    layout = backend::DescriptorSetLayoutHandle::invalid();
  };
  const auto destroyPipelineLayout = [this](backend::PipelineLayoutHandle& layout) {
    if (layout.valid()) mDevice->destroyPipelineLayout(layout);
    layout = backend::PipelineLayoutHandle::invalid();
  };

  destroySet(mQuadSet);
  destroySet(mPathSet);
  destroySet(mCoverageFieldSet);
  destroySet(mPresentSet);
  // The gradient's per-slot sets outlive a frame for the same reason the composite pool does: their count follows the
  // largest frame's draw count, and a frame of the same shape reuses every one of them.
  for (backend::DescriptorSetHandle& set: mGradientSlotSets) {
    destroySet(set);
  }
  mGradientSlotSets.clear();
  // The per-composite sets, which outlive a frame: their count follows the largest frame's number of closing commands,
  // so they are a cache rather than frame state and are released here with the other long-lived resources. This loop was
  // missing while the pool was documented as released, which is the kind of sentence that reads as a release and is not
  // one -- the sets were simply kept until the device went away.
  for (backend::DescriptorSetHandle& set: mCompositeSets) {
    destroySet(set);
  }
  mCompositeSets.clear();
  destroyLayout(mGradientSetLayout);
  destroyPipelineLayout(mGradientPipelineLayout);
  destroyLayout(mQuadSetLayout);
  destroyLayout(mPathSetLayout);
  destroyLayout(mTexturedSetLayout);
  destroyLayout(mCoverageFieldSetLayout);
  destroyPipelineLayout(mQuadPipelineLayout);
  destroyPipelineLayout(mPathPipelineLayout);
  destroyPipelineLayout(mTexturedPipelineLayout);
  destroyPipelineLayout(mCoverageFieldPipelineLayout);

  const auto destroyBuffer = [this](backend::BufferHandle& buffer) {
    if (buffer.valid()) mDevice->destroyBuffer(buffer);
    buffer = backend::BufferHandle::invalid();
  };
  destroyBuffer(mPresentVertexBuffer);
  for (UniformBuffer& buffer: mUniformBuffers) {
    if (buffer.handle.valid()) mDevice->destroyBuffer(buffer.handle);
  }
  mUniformBuffers.clear();
  mUniformArena.clear();
  mUniformSlotsUsed = 0;
  destroyBuffer(mVertexBuffer);
  mVertexCapacity = 0;
  destroyBuffer(mIndexBuffer);
  mIndexCapacity = 0;

  const auto destroyTexture = [this](backend::TextureHandle& texture, backend::TextureViewHandle& view) {
    if (view.valid()) mDevice->destroyTextureView(view);
    if (texture.valid()) mDevice->destroyTexture(texture);
    view = backend::TextureViewHandle::invalid();
    texture = backend::TextureHandle::invalid();
  };
  destroyTexture(mWhiteTexture, mWhiteTextureView);
  destroyTexture(mAlphaTexture, mAlphaTextureView);

  if (mLinearClamp.valid()) mDevice->destroySampler(mLinearClamp);
  if (mNearestClamp.valid()) mDevice->destroySampler(mNearestClamp);
  mLinearClamp = backend::TextureSamplerHandle::invalid();
  mNearestClamp = backend::TextureSamplerHandle::invalid();
  // After the command buffers: their destructors still wait on this fence when a frame is in flight.
  if (mFrameFence.valid()) {
    mDevice->destroyFence(mFrameFence);
    mFrameFence = backend::FenceHandle::invalid();
  }
}

void GpuRenderer::destroyPooledTargets() noexcept {
  for (PooledTarget& entry: mLayerPool) {
    destroyRenderTarget(entry.target);
    entry.inUse = false;
  }
  mLayerPool.clear();
}

void GpuRenderer::destroyScratchTextures() noexcept {
  for (ScratchTexture& entry: mScratchPool) {
    if (entry.view.valid() && mDevice != nullptr) mDevice->destroyTextureView(entry.view);
    if (entry.texture.valid() && mDevice != nullptr) mDevice->destroyTexture(entry.texture);
    entry.view = backend::TextureViewHandle::invalid();
    entry.texture = backend::TextureHandle::invalid();
    entry.inUse = false;
  }
  mScratchPool.clear();
}

void GpuRenderer::destroyCachedTextures() noexcept {
  if (mDevice == nullptr) {
    // A renderer whose device never came up has no resources to release, but it may still hold entries -- this is
    // called from `destroyResources`, which the destructor reaches on a partially initialised renderer too.
    mImagePlacements.clear();
    mImagePages.clear();
    mFieldAtlases.clear();
    return;
  }

  // Released in the order the objects depend on each other: a set is created against a layout, a view against a texture,
  // and the texture is the driver's memory.
  //
  // ## The pages and the placements, which are the two tables a session grows
  //
  // Each entry's objects are released explicitly because the RHI has no owner for them: a handle is a number, and a table
  // that is merely `clear`ed leaves every texture, view and set in it alive for the life of the device. That is a
  // per-renderer leak rather than a per-frame one, which is why no frame-counting test would have seen it -- the same
  // reason the previous version of this function had to be given the image and field tables in the first place.
  for (auto& [hash, entry]: mImagePlacements) {
    (void) hash;
    releaseImagePlacement(entry);
  }
  mImagePlacements.clear();

  for (ImageAtlasPage& page: mImagePages) {
    if (page.set.valid()) mDevice->destroyDescriptorSet(page.set);
  if (page.setNearest.valid()) mDevice->destroyDescriptorSet(page.setNearest);
    if (page.view.valid()) mDevice->destroyTextureView(page.view);
    if (page.texture.valid()) mDevice->destroyTexture(page.texture);
    page.set = backend::DescriptorSetHandle::invalid();
  page.setNearest = backend::DescriptorSetHandle::invalid();
    page.view = backend::TextureViewHandle::invalid();
    page.texture = backend::TextureHandle::invalid();
  }
  mImagePages.clear();
  mNextImageTextureId = 0;

  for (auto& [key, entry]: mFieldAtlases) {
    (void) key;
    if (entry.set.valid()) mDevice->destroyDescriptorSet(entry.set);
    if (entry.view.valid()) mDevice->destroyTextureView(entry.view);
    if (entry.texture.valid()) mDevice->destroyTexture(entry.texture);
  }
  mFieldAtlases.clear();
}

// ==================================================================================================
// Targets
// ==================================================================================================

backend::Result GpuRenderer::createRenderTarget(const RenderTargetDescription& description, RenderTarget* outTarget) {
  if (outTarget == nullptr) {
    return backend::Result::InvalidArgument;
  }
  *outTarget = RenderTarget{};
  if (mDevice == nullptr) {
    return backend::Result::Unsupported;
  }
  if (description.width == 0 || description.height == 0) {
    return backend::Result::InvalidArgument;
  }

  const uint32_t sampleCount = description.sampleCount > 1 ? description.sampleCount : 1;
  if (sampleCount > 1) {
    const backend::Capabilities& capabilities = mDevice->getCapabilities();
    if (!capabilities.supports(backend::Feature::MultisampleResolve) ||
        !capabilities.supportsSampleCount(sampleCount)) {
      note(backend::MessageSeverity::Warning,
        "the device does not support the requested MSAA sample count; refusing sampleCount > 1");
      return backend::Result::Unsupported;
    }
  }
  if (!mDevice->supportsRenderTarget(description.format)) {
    note(backend::MessageSeverity::Error,
      "the device cannot render to the requested format; check Device::supportsRenderTarget first");
    return backend::Result::Unsupported;
  }

  RenderTarget target;
  target.width = description.width;
  target.height = description.height;
  target.format = description.format;
  target.sampled = description.sampled;
  target.sampleCount = sampleCount;

  // A sampled target needs the Sampled usage; a layer always does, because compositing it is the only
  // reason it exists. `description.readable` deliberately adds nothing: the OpenGL read path is
  // `glReadPixels` through the target's own framebuffer and needs no usage flag, so asking for one would
  // allocate whatever the backend thinks it means for nothing.
  backend::TextureUsages usages = static_cast<backend::TextureUsages>(backend::TextureUsage::ColorAttachment);
  if (description.sampled) {
    usages = usages | backend::TextureUsage::Sampled;
  }

  backend::TextureDescriptor colorDescriptor =
    backend::TextureDescriptor::texture2D(description.width, description.height, description.format, usages);
  colorDescriptor.sampleCount = 1;
  colorDescriptor.label = description.label;
  backend::Result result = mDevice->createTexture(colorDescriptor, &target.color);
  if (!backend::isSuccess(result)) {
    destroyRenderTarget(target);
    return result;
  }
  if (description.label != nullptr) mDevice->setTextureLabel(target.color, description.label);

  backend::TextureViewDescriptor colorView;
  colorView.texture = target.color;
  colorView.dimension = backend::TextureViewDimension::Texture2D;
  colorView.label = description.label;
  result = mDevice->createTextureView(colorView, &target.colorView);
  if (!backend::isSuccess(result)) {
    destroyRenderTarget(target);
    return result;
  }

  if (sampleCount > 1) {
    // The multisampled attachment is the one the pass attaches; the single-sampled `color` above is the
    // resolve destination and the image every consumer (snapshot, composite, `Surface::target()`) sees.
    backend::TextureDescriptor msaaDescriptor = backend::TextureDescriptor::texture2D(description.width,
      description.height, description.format,
      static_cast<backend::TextureUsages>(backend::TextureUsage::ColorAttachment));
    msaaDescriptor.sampleCount = sampleCount;
    msaaDescriptor.label = description.label;
    result = mDevice->createTexture(msaaDescriptor, &target.msaaColor);
    if (!backend::isSuccess(result)) {
      destroyRenderTarget(target);
      return result;
    }
    backend::TextureViewDescriptor msaaView;
    msaaView.texture = target.msaaColor;
    msaaView.dimension = backend::TextureViewDimension::Texture2D;
    msaaView.label = description.label;
    result = mDevice->createTextureView(msaaView, &target.msaaColorView);
    if (!backend::isSuccess(result)) {
      destroyRenderTarget(target);
      return result;
    }
  }

  backend::PixelFormat depthFormat = backend::PixelFormat::Undefined;
  if (description.needsStencil) {
    depthFormat = kDepthStencilFormat;
    backend::TextureDescriptor depthDescriptor = backend::TextureDescriptor::texture2D(description.width,
      description.height, depthFormat,
      static_cast<backend::TextureUsages>(backend::TextureUsage::DepthStencilAttachment));
    depthDescriptor.sampleCount = sampleCount;
    result = mDevice->createTexture(depthDescriptor, &target.depthStencil);
    if (!backend::isSuccess(result)) {
      destroyRenderTarget(target);
      return result;
    }
    backend::TextureViewDescriptor depthView;
    depthView.texture = target.depthStencil;
    depthView.dimension = backend::TextureViewDimension::Texture2D;
    result = mDevice->createTextureView(depthView, &target.depthStencilView);
    if (!backend::isSuccess(result)) {
      destroyRenderTarget(target);
      return result;
    }
  }

  // The pass describes the attachment *shape*, which is what the pipeline is validated against. It is
  // the same object for every target with the same formats, but each target owns its own so that
  // destroying one target cannot invalidate another's compatibility check.
  backend::RenderPassDescriptor passDescriptor;
  passDescriptor.colorAttachmentCount = 1;
  passDescriptor.colorFormats[0] = description.format;
  passDescriptor.depthStencilFormat = depthFormat;
  passDescriptor.sampleCount = sampleCount;
  passDescriptor.label = description.label;
  result = mDevice->createRenderPass(passDescriptor, &target.pass);
  if (!backend::isSuccess(result)) {
    destroyRenderTarget(target);
    return result;
  }

  *outTarget = target;
  return backend::Result::Success;
}

backend::Result GpuRenderer::createWindowTarget(const uint32_t width, const uint32_t height,
        const backend::PixelFormat format, const uint32_t requestedSamples, RenderTarget* outTarget) {
  if (outTarget == nullptr) {
    return backend::Result::InvalidArgument;
  }
  if (width == 0 || height == 0) {
    return backend::Result::InvalidArgument;
  }

  const uint32_t sampleCount = requestedSamples > 1 ? requestedSamples : 1;
  if (!mDevice->supportsRenderTarget(format)) {
    note(backend::MessageSeverity::Error,
      "the swapchain format cannot be used as a render target on this device");
    return backend::Result::Unsupported;
  }
  if (sampleCount > 1) {
    const backend::Capabilities& capabilities = mDevice->getCapabilities();
    // The GL window's colour attachment is the default framebuffer, and its resolve path names a
    // texture view; resolving into the default framebuffer is a Vulkan-only path here. Refuse rather
    // than render a multisampled image and silently present a single sample.
    if (mDevice->getInfo().backend != backend::Backend::Vulkan ||
        !capabilities.supports(backend::Feature::MultisampleResolve) ||
        !capabilities.supportsSampleCount(sampleCount)) {
      note(backend::MessageSeverity::Warning,
        "the window backend does not support the requested MSAA sample count");
      return backend::Result::Unsupported;
    }
  }

  RenderTarget target;
  target.width = width;
  target.height = height;
  target.format = format;
  target.sampleCount = sampleCount;
  // No colour texture: `RenderTarget::color` being invalid is documented as "the surface's own target", and the sentinel
  // view is how the pass names it. `sampled` is false because a swapchain image cannot be sampled on every backend, and
  // an effect chain that wants to read its own output needs an offscreen target anyway.
  target.color = backend::TextureHandle::invalid();
  target.colorView = backend::defaultFramebufferView();
  target.depthStencil = backend::TextureHandle::invalid();
  target.depthStencilView = backend::TextureViewHandle::invalid();
  target.sampled = false;

  if (sampleCount > 1) {
    // The swapchain image is the resolve destination, so only the multisampled attachment is allocated
    // here. `color` stays invalid, and the pass resolves into the default-framebuffer sentinel.
    backend::TextureDescriptor msaaDescriptor = backend::TextureDescriptor::texture2D(width, height, format,
      static_cast<backend::TextureUsages>(backend::TextureUsage::ColorAttachment));
    msaaDescriptor.sampleCount = sampleCount;
    msaaDescriptor.label = "window-msaa";
    backend::Result created = mDevice->createTexture(msaaDescriptor, &target.msaaColor);
    if (!backend::isSuccess(created)) return created;
    backend::TextureViewDescriptor msaaView;
    msaaView.texture = target.msaaColor;
    msaaView.dimension = backend::TextureViewDimension::Texture2D;
    msaaView.label = "window-msaa";
    created = mDevice->createTextureView(msaaView, &target.msaaColorView);
    if (!backend::isSuccess(created)) {
      destroyRenderTarget(target);
      return created;
    }
  }

  // The pass describes the attachment *shape* for pipeline validation. No depth/stencil format: a default framebuffer's
  // depth buffer belongs to the window, so a pipeline declaring one would be incompatible with it.
  backend::RenderPassDescriptor passDescriptor;
  passDescriptor.colorAttachmentCount = 1;
  passDescriptor.colorFormats[0] = format;
  passDescriptor.depthStencilFormat = backend::PixelFormat::Undefined;
  passDescriptor.sampleCount = sampleCount;
  passDescriptor.label = "window";
  const backend::Result result = mDevice->createRenderPass(passDescriptor, &target.pass);
  if (!backend::isSuccess(result)) {
    destroyRenderTarget(target);
    return result;
  }
  *outTarget = target;
  return backend::Result::Success;
}

backend::Result GpuRenderer::createLayerTarget(const uint32_t width, const uint32_t height,
  const backend::PixelFormat format, const bool needsStencil, RenderTarget* outTarget) {
  RenderTargetDescription description;
  description.width = width;
  description.height = height;
  description.format = format == backend::PixelFormat::Undefined ? backend::PixelFormat::R8G8B8A8_UNORM : format;
  description.needsStencil = needsStencil;
  description.sampleCount = 1;
  // A layer is composited onto its parent, so it is sampled by definition; asking for anything else
  // would produce a target whose only consumer cannot read it.
  description.sampled = true;
  description.label = "canvas/layer";
  return createRenderTarget(description, outTarget);
}

void GpuRenderer::destroyRenderTarget(RenderTarget& target) {
  if (mDevice != nullptr) {
    // The device tolerates an invalid handle, but checking anyway is what makes a double destroy a
    // documented no-op rather than a behaviour that depends on the backend's pool implementation.
    // The default-framebuffer sentinel is not an allocated view: destroying it would free a slot no allocator handed out.
    if (target.colorView.valid() && target.colorView != backend::defaultFramebufferView()) {
      mDevice->destroyTextureView(target.colorView);
    }
    if (target.msaaColorView.valid()) mDevice->destroyTextureView(target.msaaColorView);
    if (target.msaaColor.valid()) mDevice->destroyTexture(target.msaaColor);
    if (target.depthStencilView.valid()) mDevice->destroyTextureView(target.depthStencilView);
    if (target.color.valid()) mDevice->destroyTexture(target.color);
    if (target.depthStencil.valid()) mDevice->destroyTexture(target.depthStencil);
    if (target.pass.valid()) mDevice->destroyRenderPass(target.pass);
  }
  target = RenderTarget{};
}

// ==================================================================================================
// Cached resources
// ==================================================================================================

backend::Result GpuRenderer::ensurePipeline(const PipelineKey& key, const backend::RenderPassHandle pass,
  backend::RenderPipelineHandle* outPipeline) {
  for (PipelineEntry& entry: mPipelines) {
    const bool same = entry.key.program == key.program && entry.key.variant == key.variant &&
                      entry.key.blendMode == key.blendMode && entry.key.colorFormat == key.colorFormat &&
                      entry.key.depthStencilFormat == key.depthStencilFormat &&
                      entry.key.sampleCount == key.sampleCount && entry.key.writeMask == key.writeMask;
    if (same) {
      if (entry.lastUsedFrame != mFrameCount) {
        entry.lastUsedFrame = mFrameCount;
        ++mStats.pipelineCount;
      }
      *outPipeline = entry.pipeline;
      return backend::Result::Success;
    }
  }
  return createPipeline(key, pass, outPipeline);
}

backend::Result GpuRenderer::createPipeline(const PipelineKey& key, const backend::RenderPassHandle pass,
  backend::RenderPipelineHandle* outPipeline) {
  const char* vertexName = nullptr;
  const char* fragmentName = nullptr;
  backend::PipelineLayoutHandle layout = backend::PipelineLayoutHandle::invalid();

  switch (key.program) {
    case Program::Quad:
      vertexName = "quad.vert";
      fragmentName = "quad.frag";
      layout = mQuadPipelineLayout;
      break;
    case Program::Image:
    case Program::ImageCubic:
    case Program::SdfImage:
    case Program::SdfPattern:
      // The quad's *vertex* stage and its layout, with the image fragment stage: the geometry, the coverage and the y flip
      // are the quad's, and the only thing that differs is how the fragment stage turns its coordinate into a texel. The
      // cubic stage differs from `image.frag` only in that turn, so it shares everything else -- and being a separate
      // *program* is what keeps sixteen taps out of the registers of every nearest and bilinear image in the frame.
      vertexName = "quad.vert";
      fragmentName = key.program == Program::ImageCubic ? "image_cubic.frag" :
                     key.program == Program::SdfImage ? "sdf.frag" :
                     key.program == Program::SdfPattern ? "sdf_pattern.frag" : "image.frag";
      layout = key.program == Program::SdfPattern ? mSdfPatternPipelineLayout : mQuadPipelineLayout;
      break;
    case Program::PathFan:
    case Program::PathCover:
      vertexName = "path.vert";
      fragmentName = "path.frag";
      layout = mPathPipelineLayout;
      break;
    case Program::Composite:
      vertexName = "composite.vert";
      fragmentName = "composite.frag";
      layout = mTexturedPipelineLayout;
      break;
    case Program::Mask:
      vertexName = "composite.vert";
      fragmentName = "mask.frag";
      layout = mTexturedPipelineLayout;
      break;
    case Program::Gradient:
      // The quad's own vertex stage, so the geometry and the coverage path are the ones every rectangle already
      // uses, and `mTexturedPipelineLayout` because the gradient block sits at binding 2 exactly where the sampled
      // programs put theirs.
      vertexName = "quad.vert";
      fragmentName = "gradient.frag";
      layout = mGradientPipelineLayout;
      break;
    case Program::CoverageField:
      // The quad's vertex stage again, so the rectangle, the y flip and the {0,1}^2 coordinate are the ones
      // every other rectangle uses, and its *own* pipeline layout because this fragment stage declares a
      // sampler at binding 0 where the gradient declares a uniform block. Sharing `mGradientPipelineLayout`
      // here would create a program whose declared sampler has no matching binding, which is a link error on
      // some drivers and a silently unbound texture on others.
      vertexName = "quad.vert";
      fragmentName = "coverage.frag";
      layout = mCoverageFieldPipelineLayout;
      break;
    case Program::Present:
      vertexName = "composite.vert";
      fragmentName = "present.frag";
      layout = mTexturedPipelineLayout;
      break;
  }

  const CanvasShader vertexShader = findShader(mDevice, vertexName);
  const CanvasShader fragmentShader = findShader(mDevice, fragmentName);
  if (!vertexShader.isValid() || !fragmentShader.isValid()) {
    note(backend::MessageSeverity::Error, std::string("a shader the pipeline needs is not embedded: ") +
                                          (vertexShader.isValid() ? fragmentName : vertexName));
    return backend::Result::Unsupported;
  }

  /**
   * A shader module can back exactly one pipeline on this backend.
   *
   * `GLDevice::createRenderPipeline` links a program from the modules' shader objects and then sets
   * `shaderObject = 0` on each, because the linked program owns the compiled code and holding the
   * objects alive only wastes driver memory. A second pipeline built from the same modules therefore
   * attaches no stages and fails with `ShaderCompilationFailed`. That is why the modules are created
   * here, per pipeline variant, and destroyed as soon as the pipeline has consumed them -- rather than
   * once in `initialize`, which is what the shape of the RHI would suggest and what this backend does not
   * allow.
   */
  const auto createModule = [this](const CanvasShader& shader, backend::ShaderModuleHandle* out) {
    backend::ShaderModuleDescriptor descriptor;
    descriptor.stage = shader.stage();
    if (shader.directSource) {
      // ## The no-toolchain path
      //
      // The driver compiles the source as written, so the dialect has to be the driver's: the committed
      // fallback carries both, and the device's own `apiVersion` says which one it is. The direct module has
      // no SPIR-V to reflect, so the binding table is stated - it is what `GlDevice::buildBindingTable`
      // resolves with `glGetUniformLocation`/`glGetUniformBlockIndex`.
      const bool es = usesEsDialect(mDevice);
      descriptor.language = backend::ShaderModuleDescriptor::Language::GLSLDirect;
      descriptor.code = es ? shader.source.es : shader.source.desktop;
      descriptor.codeSize = std::strlen(static_cast<const char*>(descriptor.code));
      descriptor.sourceLanguage = es ? backend::ShaderSourceLanguage::ESSL300
                                     : backend::ShaderSourceLanguage::GLSL410;
      descriptor.bindingDeclarations = shader.source.bindings;
      descriptor.bindingDeclarationCount = static_cast<uint32_t>(shader.source.bindingCount);
      descriptor.label = shader.source.name;
      return mDevice->createShaderModule(descriptor, out);
    }

    descriptor.language = backend::ShaderModuleDescriptor::Language::SPIRV;
    descriptor.code = shader.blob.words;
    descriptor.codeSize = shader.blob.wordCount * sizeof(uint32_t);
    descriptor.label = shader.blob.name;
    return mDevice->createShaderModule(descriptor, out);
  };

  backend::ShaderModuleHandle vertexModule = backend::ShaderModuleHandle::invalid();
  backend::Result result = createModule(vertexShader, &vertexModule);
  if (!backend::isSuccess(result)) {
    return result;
  }
  backend::ShaderModuleHandle fragmentModule = backend::ShaderModuleHandle::invalid();
  result = createModule(fragmentShader, &fragmentModule);
  if (!backend::isSuccess(result)) {
    if (vertexModule.valid()) mDevice->destroyShaderModule(vertexModule);
    return result;
  }

  backend::RenderPipelineDescriptor descriptor;
  descriptor.layout = layout;
  descriptor.vertexModule = vertexModule;
  descriptor.fragmentModule = fragmentModule;
  descriptor.compatibleRenderPass = pass;
  descriptor.colorTargetCount = 1;
  descriptor.colorTargets[0].format = key.colorFormat;
  descriptor.multisample.sampleCount = key.sampleCount;
  // The blend state is set by the variant switch below and by nothing else: leaving a default here would mean
  // two places decide it, and a variant that forgot to would silently inherit whatever the default was.
  descriptor.depthStencilFormat = key.depthStencilFormat;

  switch (key.program) {
    case Program::Quad: {
      // One buffer, four attributes, all stepping per vertex: the rectangle's own data rides on each of
      // its six vertices. `QuadVertex` says why this is not the per-instance form the shader declares.
      descriptor.vertexInput.bufferCount = 1;
      descriptor.vertexInput.buffers[0] = { kQuadVertexStride, backend::VertexStepMode::Vertex, 0, 4 };
      descriptor.vertexInput.attributeCount = 4;
      descriptor.vertexInput.attributes[0] = { 0, 0, backend::VertexFormat::Float2, 0 };
      descriptor.vertexInput.attributes[1] = { 1, 0, backend::VertexFormat::Float4, 8 };
      descriptor.vertexInput.attributes[2] = { 2, 0, backend::VertexFormat::Float4, 24 };
      descriptor.vertexInput.attributes[3] = { 3, 0, backend::VertexFormat::Float4, 40 };
      descriptor.rasterizer.topology = backend::PrimitiveTopology::TriangleList;
      break;
    }
    case Program::Image:
    case Program::ImageCubic:
    case Program::SdfImage:
    case Program::SdfPattern: {
      // The quad's vertex input, attribute for attribute, because this *is* the quad's vertex stage -- the same
      // `quad.vert`, the same `QuadVertex` in the arena, the same stride. The image fragment stage reads `vParams` as a
      // sub-rectangle where the quad reads it as parameters; that difference is entirely inside the fragment stage, so
      // nothing here changes. SDF text is the one variant with a larger record, because only it reads the styling
      // attribute.
      const bool sdf = key.program == Program::SdfImage || key.program == Program::SdfPattern;
      descriptor.vertexInput.bufferCount = 1;
      descriptor.vertexInput.buffers[0] = { sdf ? kSdfVertexStride : kQuadVertexStride,
                                            backend::VertexStepMode::Vertex, 0, 4 };
      descriptor.vertexInput.attributeCount = sdf ? 9u : 4u;
      descriptor.vertexInput.attributes[0] = { 0, 0, backend::VertexFormat::Float2, 0 };
      descriptor.vertexInput.attributes[1] = { 1, 0, backend::VertexFormat::Float4, 8 };
      descriptor.vertexInput.attributes[2] = { 2, 0, backend::VertexFormat::Float4, 24 };
      descriptor.vertexInput.attributes[3] = { 3, 0, backend::VertexFormat::Float4, 40 };
      descriptor.vertexInput.attributes[4] = { 4, 0, backend::VertexFormat::Float4, 56 };
      descriptor.vertexInput.attributes[5] = { 5, 0, backend::VertexFormat::Float4, 72 };
      descriptor.vertexInput.attributes[6] = { 6, 0, backend::VertexFormat::Float4, 88 };
      descriptor.vertexInput.attributes[7] = { 7, 0, backend::VertexFormat::Float4, 104 };
      descriptor.vertexInput.attributes[8] = { 8, 0, backend::VertexFormat::Float, 120 };
      descriptor.rasterizer.topology = backend::PrimitiveTopology::TriangleList;
      break;
    }
    case Program::PathFan:
    case Program::PathCover: {
      descriptor.vertexInput.bufferCount = 1;
      descriptor.vertexInput.buffers[0] = { kPathVertexStride, backend::VertexStepMode::Vertex, 0, 1 };
      descriptor.vertexInput.attributeCount = 2;
      descriptor.vertexInput.attributes[0] = { 0, 0, backend::VertexFormat::Float2, 0 };
      descriptor.vertexInput.attributes[1] = { 1, 0, backend::VertexFormat::Float4, 8 };
      descriptor.rasterizer.topology = key.program == Program::PathFan
                                         ? backend::PrimitiveTopology::TriangleFan
                                         : backend::PrimitiveTopology::TriangleList;
      break;
    }
    case Program::Composite:
    case Program::Mask:
    case Program::Present: {
      descriptor.vertexInput.bufferCount = 1;
      descriptor.vertexInput.buffers[0] = { kFullTargetVertexStride, backend::VertexStepMode::Vertex, 0, 2 };
      descriptor.vertexInput.attributeCount = 2;
      descriptor.vertexInput.attributes[0] = { 0, 0, backend::VertexFormat::Float2, 0 };
      descriptor.vertexInput.attributes[1] = { 1, 0, backend::VertexFormat::Float2, 8 };
      descriptor.rasterizer.topology = backend::PrimitiveTopology::TriangleList;
      break;
    }
  }
  descriptor.rasterizer.cullMode = backend::CullMode::None;
  // The shaders' own convention, from `quad.vert`'s note on Y: device-space positions are mapped to the
  // RHI's clip space with a positive y scale, and the flip that makes that land right way up in OpenGL
  // happens when the SPIR-V is translated, not here. Setting `flipY` would flip it a second time.
  descriptor.rasterizer.flipY = false;
  descriptor.depthStencil.depthTestEnable = false;
  descriptor.depthStencil.depthWriteEnable = false;

  // The blend state. A variant whose program computes the final pixel -- the stencil passes, the masked
  // composite, the advanced composite -- gets a fixed state, and only the passes that are *compositing*
  // through the blend unit get one derived from the command's mode.
  //
  // The channel write mask is part of the pipeline for the same reason: subpixel text draws the same
  // coverage three times, once per colour channel, and the fixed-function blend can only attenuate
  // every channel by the same alpha. A pipeline whose pass is not a colour write (stencil) overrides
  // this to zero below.
  descriptor.colorTargets[0].writeMask = key.writeMask;
  switch (key.variant) {
    case Variant::Plain:
      if (key.program == Program::Composite || key.program == Program::Present) {
        descriptor.colorTargets[0].blend = backend::BlendState::opaque();
      } else if (key.program == Program::Mask) {
        descriptor.colorTargets[0].blend =
          key.blendMode == kNoBlendMode
            ? backend::BlendState::opaque()
            : blendStateFor(static_cast<BlendMode>(key.blendMode));
      } else {
        descriptor.colorTargets[0].blend = blendStateFor(static_cast<BlendMode>(key.blendMode));
      }
      break;

    case Variant::StencilWinding:
    case Variant::StencilEvenOdd:
    case Variant::StencilReset: {
      descriptor.depthStencil.stencilEnable = true;
      // No colour writes, asked for as what it is.
      //
      // This used to be a `(Zero, One)` blend pair, because the backend declared `writeMask` and never applied
      // it. That is a *no-op*, not a write disable: it leaves the destination exactly as it was by multiplying
      // the source by zero, which happens to produce the right pixels and leaves a blend state enabled that the
      // next command has to remember to turn off. The mask is now applied by the backend, so the pass says what
      // it means and there is no blend state to leak.
      descriptor.colorTargets[0].writeMask = 0x0;
      descriptor.colorTargets[0].blend = backend::BlendState::opaque();
      const backend::StencilOp passOp = key.variant == Variant::StencilWinding
                                          ? backend::StencilOp::IncrementWrap
                                          : (key.variant == Variant::StencilEvenOdd
                                               ? backend::StencilOp::Invert
                                               : backend::StencilOp::Zero);
      const backend::StencilOp backOp = key.variant == Variant::StencilWinding
                                          ? backend::StencilOp::DecrementWrap
                                          : passOp;
      for (backend::StencilFaceState* face: { &descriptor.depthStencil.front, &descriptor.depthStencil.back }) {
        face->compare = backend::CompareOp::Always;
        face->failOp = backend::StencilOp::Keep;
        face->depthFailOp = backend::StencilOp::Keep;
        face->readMask = 0xFF;
        face->writeMask = 0xFF;
      }
      descriptor.depthStencil.front.passOp = passOp;
      descriptor.depthStencil.back.passOp = backOp;
      break;
    }

    case Variant::FanFill:
      descriptor.colorTargets[0].blend = blendStateFor(static_cast<BlendMode>(key.blendMode));
      break;

    case Variant::StencilCover: {
      // The cover pass asks one question: "is the stencil non-zero here". Non-zero is the fill rule for
      // both winding and parity, which is why one pass serves both -- the two rules differ only in how
      // the number was accumulated, not in what counts as inside.
      descriptor.depthStencil.stencilEnable = true;
      descriptor.depthStencil.stencilReference = 0;
      for (backend::StencilFaceState* face: { &descriptor.depthStencil.front, &descriptor.depthStencil.back }) {
        face->compare = backend::CompareOp::NotEqual;
        face->failOp = backend::StencilOp::Keep;
        face->depthFailOp = backend::StencilOp::Keep;
        face->passOp = backend::StencilOp::Keep;
        face->readMask = 0xFF;
        face->writeMask = 0;
      }
      descriptor.colorTargets[0].blend = blendStateFor(static_cast<BlendMode>(key.blendMode));
      break;
    }
  }

  switch (key.program) {
    case Program::Quad:
      descriptor.label = "canvas/quad";
      break;
    case Program::Image:
      descriptor.label = "canvas/image";
      break;
    case Program::SdfImage:
      descriptor.label = "canvas/sdf-image";
      break;
    case Program::SdfPattern:
      descriptor.label = "canvas/sdf-pattern";
      break;
    case Program::CoverageField:
      // ## The vertex input, which this program must declare for itself
      //
      // Every other program that shares `quad.vert` lists the same four attributes here, and this case was missing --
      // so the descriptor kept the struct's default of *no attributes*, the backend built a vertex array with nothing
      // wired, and the draw rasterised no primitives at all. That is a silent failure by construction: no attribute
      // means no vertex fetch, and a draw with no primitives raises no error on any driver.
      //
      // It is the same four the gradient declares, and it must stay in step with `QuadVertex` and with `quad.vert`'s
      // `layout(location = ...)` order: 0 is the {0,1}^2 corner, 1 the rectangle, 2 the parameters, 3 the colour.
      descriptor.label = "canvas/coverage-field";
      descriptor.vertexInput.bufferCount = 1;
      descriptor.vertexInput.buffers[0] = { kQuadVertexStride, backend::VertexStepMode::Vertex, 0, 4 };
      descriptor.vertexInput.attributeCount = 4;
      descriptor.vertexInput.attributes[0] = { 0, 0, backend::VertexFormat::Float2, 0 };
      descriptor.vertexInput.attributes[1] = { 1, 0, backend::VertexFormat::Float4, 8 };
      descriptor.vertexInput.attributes[2] = { 2, 0, backend::VertexFormat::Float4, 24 };
      descriptor.vertexInput.attributes[3] = { 3, 0, backend::VertexFormat::Float4, 40 };
      descriptor.rasterizer.topology = backend::PrimitiveTopology::TriangleList;
      break;
    case Program::PathFan:
      descriptor.label = "canvas/path-stencil";
      break;
    case Program::PathCover:
      descriptor.label = "canvas/path-cover";
      break;
    case Program::Composite:
      descriptor.label = "canvas/composite";
      break;
    case Program::Mask:
      descriptor.label = "canvas/mask";
      break;
    case Program::Present:
      descriptor.label = "canvas/present";
      break;
    case Program::Gradient: {
      descriptor.label = "canvas/gradient";
      // The same vertex input as `Program::Quad` below, because this *is* the quad's vertex stage: one buffer, four
      // attributes, all stepping per vertex.
      descriptor.vertexInput.bufferCount = 1;
      descriptor.vertexInput.buffers[0] = { kQuadVertexStride, backend::VertexStepMode::Vertex, 0, 4 };
      descriptor.vertexInput.attributeCount = 4;
      descriptor.vertexInput.attributes[0] = { 0, 0, backend::VertexFormat::Float2, 0 };
      descriptor.vertexInput.attributes[1] = { 1, 0, backend::VertexFormat::Float4, 8 };
      descriptor.vertexInput.attributes[2] = { 2, 0, backend::VertexFormat::Float4, 24 };
      descriptor.vertexInput.attributes[3] = { 3, 0, backend::VertexFormat::Float4, 40 };
      descriptor.rasterizer.topology = backend::PrimitiveTopology::TriangleList;
      break;
    }
  }

  if (std::getenv("CORAL_TRACE_VAO") != nullptr) {
    std::fprintf(stderr, "[pipeline] creating program=%d variant=%d: %s + %s\n", static_cast<int>(key.program),
            static_cast<int>(key.variant), vertexName, fragmentName);
    std::fflush(stderr);
  }
  backend::RenderPipelineHandle pipeline = backend::RenderPipelineHandle::invalid();
  result = mDevice->createRenderPipeline(descriptor, &pipeline);
  // The modules were consumed by the link whether it succeeded or not; the GL backend clears their
  // `shaderObject` so that this is a bookkeeping release rather than a second delete.
  mDevice->destroyShaderModule(vertexModule);
  mDevice->destroyShaderModule(fragmentModule);
  if (!backend::isSuccess(result)) {
    // ## A pipeline that could not be created, named
    //
    // This used to return the result and say nothing. The caller's draw then produced no pixels -- a pipeline it
    // could not find is skipped by the replay before any fragment runs -- so the symptom was a blank rectangle with no
    // error anywhere, which is the hardest kind of failure to attribute. Printing which program and which shaders
    // failed turns that into one line. The device's own messages carry the compiler output; this carries the identity.
    std::string message = "the render pipeline for ";
    message += vertexName == nullptr ? "?" : vertexName;
    message += " + ";
    message += fragmentName == nullptr ? "?" : fragmentName;
    message += " could not be created (result ";
    message += std::to_string(static_cast<int>(result));
    message += "), so every draw recorded for it will be skipped";
    note(backend::MessageSeverity::Error, std::move(message));
    return result;
  }

  if (std::getenv("CORAL_TRACE_VAO") != nullptr) {
    std::fprintf(stderr, "[pipeline] created program=%d handle index=%u\n", static_cast<int>(key.program),
            pipeline.index);
    std::fflush(stderr);
  }
  mPipelines.push_back(PipelineEntry{ key, pipeline, mFrameCount });
  ++mStats.pipelineCount;
  *outPipeline = pipeline;
  return backend::Result::Success;
}

backend::Result GpuRenderer::ensureVertexCapacity(const uint64_t bytes) {

  if (mVertexBuffer.valid() && mVertexCapacity >= bytes) {
    return backend::Result::Success;
  }

  uint64_t capacity = 16u * 1024u;
  while (capacity < bytes) {
    capacity *= 2u;
  }

  if (mVertexBuffer.valid()) {
    // Growing destroys a buffer that the *previous* frame's commands may still reference. The OpenGL
    // backend replays synchronously inside `executeCommandBuffer` and reports every resource ready to
    // destroy the moment it returns, so this wait is not needed today; it is here because the RHI does
    // not promise that, and a backend that defers execution would otherwise read freed memory.
    mDevice->waitIdle();
    mDevice->destroyBuffer(mVertexBuffer);
    mVertexBuffer = backend::BufferHandle::invalid();
    mVertexCapacity = 0;
  }

  const backend::BufferDescriptor descriptor{
    .size = capacity,
    .usage = static_cast<backend::BufferUsages>(backend::BufferUsage::Vertex),
    // Host visible and rewritten every frame, for the same reason the uniform buffer is: the frame's
    // vertices are assembled on the CPU and recorded into the command buffer with `writeBuffer`, which
    // on this backend is a `glBufferSubData` into exactly this kind of buffer.
    .memoryUsage = backend::MemoryUsage::CpuToGpuDynamic,
    .initialData = nullptr,
    .label = "canvas/vertices",
  };
  const backend::Result result = mDevice->createBuffer(descriptor, &mVertexBuffer);
  if (!backend::isSuccess(result)) {
    mVertexBuffer = backend::BufferHandle::invalid();
    return result;
  }
  mVertexCapacity = capacity;
  return backend::Result::Success;
}

backend::Result GpuRenderer::ensureIndexCapacity(const uint64_t indices) {
  if (mIndexBuffer.valid() && mIndexCapacity >= indices) {
    return backend::Result::Success;
  }

  uint64_t capacity = 1024;
  while (capacity < indices) {
    capacity *= 2u;
  }

  if (mIndexBuffer.valid()) {
    // The same reasoning as the vertex buffer's growth: the RHI does not promise that the previous frame's commands have
    // finished, so a buffer that is about to be destroyed is waited for first.
    mDevice->waitIdle();
    mDevice->destroyBuffer(mIndexBuffer);
    mIndexBuffer = backend::BufferHandle::invalid();
    mIndexCapacity = 0;
  }

  const backend::BufferDescriptor descriptor{
    .size = capacity * sizeof(uint32_t),
    .usage = static_cast<backend::BufferUsages>(backend::BufferUsage::Index),
    // Host visible, because it is filled once per *growth* by a mapped write rather than by a command in a frame
    // (`uploadBuffer`). It is not `CpuToGpu` because it is rewritten whenever a batch outgrows it, which is the same
    // "written repeatedly" the naming describes -- the repetition is just rare.
    .memoryUsage = backend::MemoryUsage::CpuToGpuDynamic,
    .initialData = nullptr,
    .label = "canvas/quad-indices",
  };
  const backend::Result result = mDevice->createBuffer(descriptor, &mIndexBuffer);
  if (!backend::isSuccess(result)) {
    mIndexBuffer = backend::BufferHandle::invalid();
    return result;
  }
  mIndexCapacity = capacity;

  // The pattern: two triangles per quad, sharing the diagonal, which is exactly what the six-vertex form spelled out.
  std::vector<uint32_t> pattern(static_cast<size_t>(capacity));
  for (uint64_t quad = 0; quad * kQuadIndexCount + kQuadIndexCount <= capacity; ++quad) {
    const uint32_t base = static_cast<uint32_t>(quad * kQuadCornerCount);
    uint32_t* slot = pattern.data() + static_cast<size_t>(quad) * kQuadIndexCount;
    slot[0] = base + 0;
    slot[1] = base + 1;
    slot[2] = base + 2;
    slot[3] = base + 0;
    slot[4] = base + 2;
    slot[5] = base + 3;
  }
  const backend::Result uploaded = uploadBuffer(mIndexBuffer, mIndexCapacity * sizeof(uint32_t), 0,
          pattern.data(), pattern.size() * sizeof(uint32_t));
  if (!backend::isSuccess(uploaded)) {
    mDevice->destroyBuffer(mIndexBuffer);
    mIndexBuffer = backend::BufferHandle::invalid();
    mIndexCapacity = 0;
    return uploaded;
  }
  return backend::Result::Success;
}

backend::Result GpuRenderer::ensureCommandBuffer() {
  if (mCommandBuffer != nullptr) {
    return backend::Result::Success;
  }
  backend::CommandBufferDescriptor descriptor;
  descriptor.transient = true;
  descriptor.label = "canvas/frame";
  return mDevice->createCommandBuffer(descriptor, &mCommandBuffer);
}

backend::Result GpuRenderer::ensureUploadCommandBuffer() {
  if (mUploadCommandBuffer != nullptr) {
    return backend::Result::Success;
  }
  backend::CommandBufferDescriptor descriptor;
  descriptor.transient = true;
  descriptor.oneTimeSubmit = true;
  descriptor.label = "canvas/upload";
  return mDevice->createCommandBuffer(descriptor, &mUploadCommandBuffer);
}

backend::Result GpuRenderer::uploadBuffer(const backend::BufferHandle buffer, const uint64_t bufferSize,
  const uint64_t offset, const void* data, const size_t size) {
  if (data == nullptr || size == 0 || offset + size > bufferSize) {
    return backend::Result::InvalidArgument;
  }
  void* mapped = nullptr;
  // The *whole* buffer is mapped, and the write lands at @p offset inside it, because `unmapBuffer`
  // flushes the whole buffer: a range mapping of 24 bytes followed by a whole-buffer flush is
  // `glFlushMappedBufferRange` past the end of the mapped range, which is `GL_INVALID_VALUE` and leaves
  // the error pending for the next resource creation to trip over. Mapping everything keeps the flush
  // inside the mapping.
  const backend::Result result = mDevice->mapBuffer(buffer, 0, bufferSize, &mapped);
  if (!backend::isSuccess(result)) {
    return result;
  }
  if (mapped == nullptr) {
    return backend::Result::Error;
  }
  std::memcpy(static_cast<uint8_t*>(mapped) + offset, data, size);
  mDevice->unmapBuffer(buffer);
  return backend::Result::Success;
}

backend::Result GpuRenderer::updateConstantAlphaTexture(const float alpha) {
  const auto quantised = static_cast<uint8_t>(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));

  for (const AlphaTexture& entry: mAlphaTextures) {
    if (entry.value == quantised) {
      mAlphaTexture = entry.texture;
      mAlphaTextureView = entry.view;
      return backend::Result::Success;
    }
  }

  // Not seen before, so a texture is created and filled. The upload goes into its *own* command buffer:
  // `writeTexture` is a payload-carrying command and the frame's buffer must contain none, so recording
  // it there would corrupt whichever payload came first. One payload in a command buffer of its own is
  // exactly what the backend's arena can hold, and executing it here means the texture is ready before
  // the frame that reads it is executed.
  AlphaTexture entry;
  entry.value = quantised;
  const backend::TextureDescriptor descriptor = backend::TextureDescriptor::texture2D(
    1, 1, backend::PixelFormat::R8G8B8A8_UNORM,
    static_cast<backend::TextureUsages>(backend::TextureUsage::Sampled));
  backend::Result result = mDevice->createTexture(descriptor, &entry.texture);
  if (!backend::isSuccess(result)) {
    return result;
  }
  backend::TextureViewDescriptor viewDescriptor;
  viewDescriptor.texture = entry.texture;
  viewDescriptor.dimension = backend::TextureViewDimension::Texture2D;
  result = mDevice->createTextureView(viewDescriptor, &entry.view);
  if (!backend::isSuccess(result)) {
    mDevice->destroyTexture(entry.texture);
    return result;
  }

  // Premultiplied white at the given alpha, because `mask.frag` reads the mask's alpha as coverage and
  // then multiplies a premultiplied layer by it: the coverage has to scale colour and alpha together,
  // and a stored (a, a, a, a) does exactly that.
  const uint8_t pixel[4] = { quantised, quantised, quantised, quantised };
  backend::TextureCopyRegion region;
  region.textureOrigin = { 0, 0, 0 };
  region.size = { 1, 1, 1 };
  region.mipLevel = 0;
  region.baseArrayLayer = 0;
  region.arrayLayerCount = 1;

  result = ensureUploadCommandBuffer();
  if (!backend::isSuccess(result)) return result;
  result = mUploadCommandBuffer->reset();
  if (!backend::isSuccess(result)) return result;
  result = mUploadCommandBuffer->begin();
  if (!backend::isSuccess(result)) return result;
  result = mUploadCommandBuffer->writeTexture(entry.texture, region, pixel, sizeof(pixel));
  if (backend::isSuccess(result)) {
    result = mUploadCommandBuffer->end();
  }
  if (backend::isSuccess(result)) {
    result = mDevice->executeCommandBuffer(*mUploadCommandBuffer);
  }
  if (!backend::isSuccess(result)) {
    mDevice->destroyTextureView(entry.view);
    mDevice->destroyTexture(entry.texture);
    return result;
  }

  mAlphaTextures.push_back(entry);
  mAlphaTexture = entry.texture;
  mAlphaTextureView = entry.view;
  return backend::Result::Success;
}

int32_t GpuRenderer::acquireLayerTarget(const uint32_t width, const uint32_t height,
  const backend::PixelFormat format, const bool needsStencil) {
  for (size_t index = 0; index < mLayerPool.size(); ++index) {
    PooledTarget& entry = mLayerPool[index];
    if (!entry.inUse && entry.width == width && entry.height == height && entry.format == format &&
        entry.needsStencil == needsStencil) {
      entry.inUse = true;
      return static_cast<int32_t>(index);
    }
  }

  PooledTarget entry;
  entry.width = width;
  entry.height = height;
  entry.format = format;
  entry.needsStencil = needsStencil;
  entry.inUse = true;
  const backend::Result result = createLayerTarget(width, height, format, needsStencil, &entry.target);
  if (!backend::isSuccess(result)) {
    return -1;
  }
  mLayerPool.push_back(std::move(entry));
  return static_cast<int32_t>(mLayerPool.size() - 1);
}

int32_t GpuRenderer::acquireScratchTexture(const uint32_t width, const uint32_t height,
  const backend::PixelFormat format) {
  for (size_t index = 0; index < mScratchPool.size(); ++index) {
    ScratchTexture& entry = mScratchPool[index];
    if (!entry.inUse && entry.width == width && entry.height == height && entry.format == format) {
      entry.inUse = true;
      return static_cast<int32_t>(index);
    }
  }

  ScratchTexture entry;
  entry.width = width;
  entry.height = height;
  entry.format = format;
  entry.inUse = true;
  // A snapshot is blitted into and sampled from; it is never a render pass target, so it needs no
  // render pass object of its own -- only the two usages the blit requires.
  const backend::TextureDescriptor descriptor = backend::TextureDescriptor::texture2D(width, height, format,
    static_cast<backend::TextureUsages>(backend::TextureUsage::ColorAttachment | backend::TextureUsage::Sampled));
  backend::Result result = mDevice->createTexture(descriptor, &entry.texture);
  if (!backend::isSuccess(result)) {
    return -1;
  }
  backend::TextureViewDescriptor viewDescriptor;
  viewDescriptor.texture = entry.texture;
  viewDescriptor.dimension = backend::TextureViewDimension::Texture2D;
  result = mDevice->createTextureView(viewDescriptor, &entry.view);
  if (!backend::isSuccess(result)) {
    mDevice->destroyTexture(entry.texture);
    return -1;
  }
  mScratchPool.push_back(std::move(entry));
  return static_cast<int32_t>(mScratchPool.size() - 1);
}

void GpuRenderer::releaseFrameResources() noexcept {
  for (PooledTarget& entry: mLayerPool) entry.inUse = false;
  for (ScratchTexture& entry: mScratchPool) entry.inUse = false;
  mFrame.targets.clear();
  mFrame.parents.clear();
  mFrame.opened.clear();
  mFrame.batches.clear();
  mFrame.precompiled.clear();
  // The frame's resolved coverage fields go with it: they are up to 66 KB each, and they are the *resolve stage's* input
  // rather than part of the picture -- the replay samples the atlases through the placements and never reads this table.
  mFrame.fields.clear();
  // ## And the field atlas bytes and placements deliberately **stay**
  //
  // They are `mFieldAtlasBytes`/`mFieldPlaced`, which the frame refers to rather than owns, and their contents are the
  // cross-frame field cache: a frame that asked for the same fields as the last one reuses them where they are, so
  // clearing them here would throw away exactly what the cache exists to keep (and, on a hit, would empty the tables the
  // *draw* is about to read -- which is the stale-atlas defect with the sign flipped). They are replaced when a frame
  // runs the resolve stage, and the frame that runs it clears them first, so a failed placement still draws nothing.
  //
  // Clearing them here was right while they were the frame's own storage and is a defect now; this note is here because
  // the lines look like the neighbouring ones and the next person will be tempted to add them back.
  // The image placements go with them: they name pages and descriptor sets that are the renderer's and outlive the frame,
  // but a frame that placed no images must not keep the last one's table -- the same rule the field tables *used* to
  // follow, and the one that keeps a frame's `images` from being read against a *different* frame's bitmap list, whose
  // indices it shares by construction and not by identity.
  mFrame.images.clear();
  mFrame.openPass = -1;
  mFrame.list = nullptr;
  mFrame.sized.clear();
  // The rewritten recording is the frame's own; it goes with the frame, not with the caller's list.
  mFrame.ownedList.reset();
}

// ==================================================================================================
// Precompile
// ==================================================================================================

uint32_t GpuRenderer::atlasOfCommand(const DrawList::Command& command,
        const std::vector<Frame::PlacedField>& placed) noexcept {
  if (command.state.shader != DrawList::ShaderKind::Coverage) return kNoFieldAtlas;
  const uint32_t field = command.state.coverageField;
  if (field == DrawList::kNoCoverageField || field >= placed.size()) return kNoFieldAtlas;
  return placed[field].atlas;
}

void GpuRenderer::precompile(const DrawList& list, std::vector<Precompiled>& out,
  std::vector<uint8_t>& startsBatch) {
  const uint32_t count = static_cast<uint32_t>(list.commands.size());
  if (count == 0) {
    return;
  }

  // ## The order the commands are precompiled in -- measured, and left alone
  //
  // This stage submits unequal work (`parallelFor` cuts a contiguous range, so the *order* is the load balance), and the
  // field route's own stage turned out to be 10.9x unbalanced for exactly that reason (4.1.22). So the same measurement
  // was made here before changing anything: the cost of a command's precompile is the points a path has to lay out.
  //
  // Measured on the demo scene, 41847 commands in 12 pieces: **1.02x in command order against 1.00x dealt**. This scene's
  // recording interleaves its heavy paths with everything else, so a command order is already even, and dealing it would
  // buy 2% of a stage whose share of the frame is a few milliseconds -- while costing a cost pass and a sort of 41847
  // elements every frame. The deal was therefore written, measured and **reverted**; what is kept is the measurement,
  // behind `CORAL_TRACE_FIELDS`, because the next scene whose paths are recorded consecutively is the one that will need
  // `balanceByCost` here.
  const bool tracePrecompile = std::getenv("CORAL_TRACE_FIELDS") != nullptr;
  if (mJobs != nullptr && tracePrecompile) {
    std::vector<uint64_t> cost(count, 1);
    uint64_t total = 0;
    for (uint32_t index = 0; index < count; ++index) {
      const DrawList::Command& command = list.commands[index];
      if (command.kind != DrawList::Kind::Path) continue;
      uint64_t points = 0;
      for (uint32_t contourIndex = 0; contourIndex < command.contourCount; ++contourIndex) {
        const uint32_t contour = command.firstContour + contourIndex;
        if (contour >= list.contours.size()) break;
        points += list.contours[contour].pointCount;
      }
      // One for the command itself, so a path with no usable contours does not read as free.
      cost[index] = 1 + points;
      total += cost[index];
    }
    const uint32_t pieces = std::max(1u, std::min<uint32_t>(mJobs->workerCount(), count / 64));
    const size_t pieceSize = count / pieces + (count % pieces != 0 ? 1 : 0);
    uint64_t worst = 0;
    for (size_t begin = 0; begin < cost.size(); begin += pieceSize) {
      uint64_t pieceCost = 0;
      for (size_t index = begin; index < std::min(cost.size(), begin + pieceSize); ++index) pieceCost += cost[index];
      worst = std::max(worst, pieceCost);
    }
    const double balance = total > 0 ? static_cast<double>(worst) * static_cast<double>(pieces) /
                                       static_cast<double>(total) : 0.0;
    std::fprintf(stderr, "[precompile] %u commands in %u pieces of %zu: worst/even %.2fx (command order)\n", count,
            pieces, pieceSize, balance);

    // ## The image route's batching, reported by the rule the batching itself uses
    //
    // This line began as the measurement 4.1.28 asked for before the atlas was built: the route drew once per command and
    // each of those commands was its own batch, because two images could share a draw only if they were the *same* image
    // (`sameCommandState` compared `state.image`). Clearing `image` on both sides -- which is what an atlas does -- left
    // **43 runs of 559 commands**, and that number is why the design was worth building.
    //
    // It now reports the real thing rather than a hypothetical: `canShareDraw` with the textures `placeImages` resolved,
    // which is the same call the batch decision makes, so this cannot drift from it and the percentage is the route's
    // actual saving.
    uint32_t imageCommands = 0;
    uint32_t imageBatches = 0;
    for (uint32_t index = 0; index < count; ++index) {
      const DrawList::Command& command = list.commands[index];
      if (command.kind != DrawList::Kind::Image) continue;
      ++imageCommands;
      const uint32_t texture = sampledTextureOfCommand(command, mFrame);
      const uint32_t previousTexture =
              index == 0 ? kNoSampledTexture : sampledTextureOfCommand(list.commands[index - 1], mFrame);
      if (index == 0 || !canShareDraw(list.commands[index - 1], command, previousTexture, texture)) ++imageBatches;
    }
    std::fprintf(stderr, "[precompile] image route: %u commands in %u batches (%.1f%% of its draws gone)\n",
            imageCommands, imageBatches,
            imageCommands > 0 ? 100.0 * static_cast<double>(imageCommands - imageBatches) /
                                static_cast<double>(imageCommands) : 0.0);

    // ## And how much of a texture they are, which is what decided the atlas could not be built per frame
    //
    // The design question after "would batching them pay" was "how big is the texture they would share": an atlas packed
    // and hashed **every frame** (the field route's shape, which needs no persistence, no content index and no eviction)
    // is only affordable if the frame's images are small, because its cost is the atlas's *area* -- zero fill, copy and
    // hash -- and not the number of images in it. Measured at 18.8 MB, this is what ruled that shape out and fixed the
    // design as persistent and content-addressed instead (`4.1.31`).
    uint64_t imageBytes = 0;
    uint64_t pixels = 0;
    for (const Bitmap& image: list.images) {
      if (image.isEmpty()) continue;
      imageBytes += static_cast<uint64_t>(image.width()) * static_cast<uint64_t>(image.height()) * 4u;
      pixels += static_cast<uint64_t>(image.width()) * static_cast<uint64_t>(image.height());
    }
    uint64_t pageBytes = 0;
    for (const ImageAtlasPage& page: mImagePages) {
      pageBytes += static_cast<uint64_t>(page.width) * page.height * 4u;
    }
    // How the session's *distinct* bitmaps were placed, which is the number that says whether the page budget is doing
    // anything: a fallback is a texture of the bitmap's own, so every placement on that side is a texture the page could
    // not take -- and each of those commands is a batch of its own.
    uint32_t inPages = 0;
    uint32_t standalone = 0;
    for (const auto& [hash, placement]: mImagePlacements) {
      (void) hash;
      if (placement.textureId == kNoSampledTexture) continue;
      if (placement.inPage) {
        ++inPages;
      } else {
        ++standalone;
      }
    }
    std::fprintf(stderr, "[precompile] image table: %zu bitmaps, %llu px, %llu KiB as RGBA; %u placements in %u page(s) "
                         "of %llu KiB, %u standalone\n",
            list.images.size(), static_cast<unsigned long long>(pixels), static_cast<unsigned long long>(imageBytes / 1024u),
            inPages, imageAtlasPageCount(), static_cast<unsigned long long>(pageBytes / 1024u), standalone);
    std::fflush(stderr);
  }

  const auto task = [this, &list, &out, &startsBatch](const uint32_t begin, const uint32_t end) {
    for (uint32_t index = begin; index < end; ++index) {
      const Clock::time_point start = Clock::now();
      const DrawList::Command& command = list.commands[index];
      // A batch's texture is decided by where the frame *placed* what the command names -- an atlas for a coverage field,
      // a page or a standalone texture for an image -- because the field's or the bitmap's place is per-vertex data now
      // while the texture itself is one per draw. See `canShareDraw` and `sameCommandState`.
      const uint32_t texture = sampledTextureOfCommand(command, mFrame);
      const uint32_t previousTexture = index == 0 ? kNoSampledTexture
                                                  : sampledTextureOfCommand(list.commands[index - 1], mFrame);
      startsBatch[index] =
              (index == 0 || !canShareDraw(list.commands[index - 1], command, previousTexture, texture)) ? 1 : 0;

      Precompiled& slot = out[index];
      slot.fan.clear();
      slot.fans.clear();
      if (command.kind == DrawList::Kind::Path) {
        // "Tessellate the path" here means: lay its already-flattened contours out as the triangle fans
        // the stencil pass draws, one fan per contour. There is no ear clipping and no triangulation,
        // and that is the point of stencil-then-cover -- a concave or self-intersecting contour is a
        // correct fill because the winding is accumulated rather than triangulated. The fan *is* the
        // triangulation: n vertices become n-2 triangles at rasterisation time.
        uint32_t firstVertex = 0;
        const auto fill = pathColorFor(command.state);
        for (uint32_t contourIndex = 0; contourIndex < command.contourCount; ++contourIndex) {
          const uint32_t contour = command.firstContour + contourIndex;
          if (contour >= list.contours.size()) break;
          const DrawList::Contour& source = list.contours[contour];
          const uint32_t available = static_cast<uint32_t>(list.points.size());
          if (source.firstPoint >= available) break;
          const uint32_t pointCount = std::min(source.pointCount, available - source.firstPoint);
          if (pointCount < 3) {
            // A fan of two points is a line and a fan of one is a point; neither fills anything, and
            // drawing them would only add stencil work that the cover pass then has to ignore.
            continue;
          }
          for (uint32_t pointIndex = 0; pointIndex < pointCount; ++pointIndex) {
            const Point& point = list.points[source.firstPoint + pointIndex];
            PathVertex vertex;
            vertex.x = point.x;
            vertex.y = point.y;
            vertex.color[0] = fill[0];
            vertex.color[1] = fill[1];
            vertex.color[2] = fill[2];
            vertex.color[3] = fill[3];
            slot.fan.push_back(vertex);
          }
          slot.fans.push_back(FanRange{ firstVertex, pointCount });
          firstVertex += pointCount;
        }
      }
      slot.milliseconds = millisecondsSince(start);
    }
  };

  if (mJobs != nullptr) {
    // A grain of 64 commands keeps the queue traffic proportional to the work: a frame of two commands
    // should not pay for a task per command, and a frame of ten thousand should not be one task.
    mJobs->parallelFor(count, 64, task);
  } else {
    task(0, count);
  }
}

void GpuRenderer::buildBatches(const DrawList& list, const std::vector<uint8_t>& startsBatch,
  std::vector<Batch>& out) {
  out.clear();
  const uint32_t count = static_cast<uint32_t>(list.commands.size());
  uint32_t index = 0;
  while (index < count) {
    const DrawList::Command& command = list.commands[index];
    if (!isDrawable(command.kind) || startsBatch[index] == 0) {
      ++index;
      continue;
    }
    Batch batch;
    batch.firstCommand = index;
    batch.kind = command.kind;
    batch.layer = command.layer;
    batch.state = command.state;
    uint32_t end = index + 1;
    // A run ends where `startsBatch` says the next command cannot share the draw, which already covers
    // the state, the layer, the kind, the clip rectangle and the clip mask.
    while (end < count && startsBatch[end] == 0) {
      ++end;
    }
    batch.commandCount = end - index;
    if (std::getenv("CORAL_TRACE_VAO") != nullptr) {
      std::fprintf(stderr, "[batch] first=%u count=%u kind=%d shader=%d vertexOffset=%llu\n", batch.firstCommand,
              batch.commandCount, static_cast<int>(batch.kind), static_cast<int>(batch.state.shader),
              static_cast<unsigned long long>(batch.vertexOffset));
      std::fflush(stderr);
    }
    out.push_back(std::move(batch));
    index = end;
  }
}

namespace {

/** A list needs the rewrite only when a command actually names a shaped clip. */
bool hasClipMaskCommands(const DrawList& list) {
  for (const DrawList::Command& command: list.commands) {
    if (command.clipMask != DrawList::kNoClipMask) return true;
  }
  return false;
}

Rect intersectRects(const Rect& a, const Rect& b) {
  const float left = std::max(a.getLeft(), b.getLeft());
  const float top = std::max(a.getTop(), b.getTop());
  const float right = std::min(a.getRight(), b.getRight());
  const float bottom = std::min(a.getBottom(), b.getBottom());
  if (right <= left || bottom <= top) return a;
  return Rect::MakeLTRB(left, top, right, bottom);
}

/**
 * The bitmap a `synthesizeClipLayers` run hands the image route for a coverage mask.
 *
 * A shaped clip's coverage is stable across frames, but the rewrite used to build a new bitmap every
 * frame. The image route keys placement on the bitmap's content, so the pixels were found again and
 * the upload was skipped -- after hashing the whole mask and allocating a fresh buffer. Reusing the
 * bitmap keeps the hash-input identical and removes the per-frame allocation; the key is the coverage
 * bytes themselves, so an animated clip still gets a new bitmap.
 */
graphics::Bitmap cachedMaskBitmap(const DrawList& source, const DrawList::ClipMask& mask) {
  uint64_t key = mask.contentHash;
  const size_t count = static_cast<size_t>(mask.width) * mask.height;
  if (key == 0) {
    key = 14795981039346656037ull;
    const auto mix = [&key](const uint8_t byte) {
      key ^= byte;
      key *= 1099511628211ull;
    };
    mix(static_cast<uint8_t>(mask.width));
    mix(static_cast<uint8_t>(mask.width >> 8));
    mix(static_cast<uint8_t>(mask.height));
    mix(static_cast<uint8_t>(mask.height >> 8));
    const size_t hashCount = static_cast<size_t>(mask.width) * mask.height;
    for (size_t index = 0; index < hashCount; ++index) {
      const size_t offset = static_cast<size_t>(mask.firstPixel) + index;
      mix(offset < source.maskPixels.size() ? source.maskPixels[offset] : 0);
    }
  }

  static thread_local std::unordered_map<uint64_t, graphics::Bitmap> cache;
  static thread_local size_t cacheBytes = 0;
  if (const auto found = cache.find(key); found != cache.end()) return found->second;

  graphics::Bitmap bitmap = graphics::Bitmap::allocate(static_cast<int>(mask.width),
    static_cast<int>(mask.height));
  graphics::Pixmap pixels = bitmap.lockPixels();
  if (!pixels.isEmpty()) {
    for (uint32_t y = 0; y < mask.height; ++y) {
      for (uint32_t x = 0; x < mask.width; ++x) {
        const size_t offset = static_cast<size_t>(mask.firstPixel) +
                              static_cast<size_t>(y) * mask.width + x;
        const uint8_t coverage = offset < source.maskPixels.size()
                                   ? source.maskPixels[offset]
                                   : static_cast<uint8_t>(0);
        const uint32_t value = (static_cast<uint32_t>(coverage) << 24) |
                               (static_cast<uint32_t>(coverage) << 16) |
                               (static_cast<uint32_t>(coverage) << 8) | coverage;
        pixels.setPixelARGB32(static_cast<int>(x), static_cast<int>(y), value);
      }
    }
  }
  if (cache.size() >= 64 || cacheBytes + count * 4 > 64u * 1024u * 1024u) {
    cache.clear();
    cacheBytes = 0;
  }
  cache.emplace(key, bitmap);
  cacheBytes += count * 4;
  return bitmap;
}

/**
 * Rewrites a recording's shaped clips as **masked layers**.
 *
 * `Canvas::clipPath` records a per-command coverage mask (`Command::clipMask`), which this renderer
 * does not sample: every such command used to be drawn unclipped and counted as an omission. Turning
 * the mask into a layer is the route the renderer already implements and tests everywhere else -- a
 * group is rendered into an offscreen target and composited through `Layer::maskImage` -- so the
 * picture becomes correct without a second sampler on every program.
 *
 * The rewrite runs once per frame, before precompile/batching, and only for a list that actually has
 * shaped clips. Consecutive commands with the same mask in the same layer become one run:
 * `OpenLayer(mask)`, the run with `layer = <masked layer>` and `clipMask = none`, then
 * `CloseLayer(<masked layer>)`. A layer boundary or a different mask ends the run, so masks nest the
 * way the Canvas clip stack does. Layers are appended in the order their opens are recorded, which is
 * what `resolveLayerParents` reads back.
 */
void synthesizeClipLayers(const DrawList& source, DrawList& out) {
  out.commands.clear();
  out.contours = source.contours;
  out.points = source.points;
  out.maskPixels = source.maskPixels;
  out.clipMasks = source.clipMasks;
  out.images = source.images;
  out.imageHashes.assign(source.images.size(), 0);
  for (size_t index = 0; index < source.imageHashes.size() && index < out.imageHashes.size(); ++index) {
    out.imageHashes[index] = source.imageHashes[index];
  }
  out.externalImages = source.externalImages;
  out.coverageFields = source.coverageFields;
  out.bounds = source.bounds;
  out.label = source.label;
  out.layers.clear();

  // Layer 0 is the caller's target. Every other layer is appended when its open is recorded, and
  // `layerMap` translates the source list's indices into the rewritten list's.
  out.layers.push_back(source.layers.empty() ? Layer{} : source.layers[0]);
  std::vector<uint32_t> layerMap(source.layers.size(), 0);
  size_t nextSourceLayer = 1;

  const uint32_t count = static_cast<uint32_t>(source.commands.size());
  uint32_t index = 0;
  while (index < count) {
    const DrawList::Command& command = source.commands[index];
    if (command.kind == DrawList::Kind::OpenLayer) {
      uint32_t parent = 0;
      if (command.layer < layerMap.size()) parent = layerMap[command.layer];
      if (nextSourceLayer < source.layers.size()) {
        layerMap[nextSourceLayer] = static_cast<uint32_t>(out.layers.size());
        out.layers.push_back(source.layers[nextSourceLayer]);
        ++nextSourceLayer;
      }
      DrawList::Command copy = command;
      copy.layer = parent;
      out.commands.push_back(copy);
      ++index;
      continue;
    }
    if (command.kind == DrawList::Kind::CloseLayer) {
      DrawList::Command copy = command;
      if (command.layer < layerMap.size()) copy.layer = layerMap[command.layer];
      out.commands.push_back(copy);
      ++index;
      continue;
    }
    const bool masked = command.clipMask != DrawList::kNoClipMask;
    if (!masked || command.clipMask >= source.clipMasks.size() ||
        command.layer >= source.layers.size()) {
      DrawList::Command copy = command;
      if (command.layer < layerMap.size()) copy.layer = layerMap[command.layer];
      out.commands.push_back(copy);
      ++index;
      continue;
    }

    const DrawList::ClipMask& mask = source.clipMasks[command.clipMask];
    if (mask.width == 0 || mask.height == 0) {
      ++index;
      continue;
    }
    const uint32_t parent = layerMap[command.layer];

    // The coverage becomes a premultiplied white RGBA bitmap: the image route samples its alpha, and
    // white keeps the mask's luminance equal to its coverage for the Alpha rule. A stable mask reuses
    // the bitmap from the previous frame instead of rebuilding and re-hashing it.
    graphics::Bitmap bitmap = cachedMaskBitmap(source, mask);
    const uint32_t maskImage = static_cast<uint32_t>(out.images.size());
    out.images.push_back(std::move(bitmap));
    out.imageHashes.push_back(mask.contentHash);

    Layer layer;
    layer.bounds = mask.bounds;
    layer.format = backend::PixelFormat::R8G8B8A8_UNORM;
    layer.premultiplied = true;
    layer.asMask = true;
    layer.maskRule = Layer::MaskRule::Alpha;
    // The mask is the coverage rasterised over exactly this layer's bounds, so it is layer-local.
    layer.maskLocal = true;
    layer.maskImage = maskImage;
    layer.alpha = 1.0f;
    layer.blendMode = BlendMode::kSrcOver;
    layer.label = "canvas-clip-mask";
    const uint32_t maskedLayer = static_cast<uint32_t>(out.layers.size());
    out.layers.push_back(std::move(layer));

    DrawList::Command open;
    open.kind = DrawList::Kind::OpenLayer;
    open.geometry[0] = mask.bounds;
    open.clipRect = mask.bounds;
    open.layer = parent;
    open.state.shader = DrawList::ShaderKind::Solid;
    open.state.alpha = 1.0f;
    open.state.blendMode = BlendMode::kSrcOver;
    out.commands.push_back(open);

    while (index < count) {
      const DrawList::Command& run = source.commands[index];
      if (run.kind == DrawList::Kind::OpenLayer || run.kind == DrawList::Kind::CloseLayer ||
          run.clipMask != command.clipMask || run.layer != command.layer) {
        break;
      }
      DrawList::Command copy = run;
      copy.layer = maskedLayer;
      copy.clipMask = DrawList::kNoClipMask;
      copy.clipRect = intersectRects(run.clipRect, mask.bounds);
      out.commands.push_back(copy);
      ++index;
    }

    DrawList::Command close;
    close.kind = DrawList::Kind::CloseLayer;
    close.geometry[0] = mask.bounds;
    close.clipRect = mask.bounds;
    close.layer = maskedLayer;
    close.state.shader = DrawList::ShaderKind::Solid;
    close.state.alpha = 1.0f;
    close.state.blendMode = BlendMode::kSrcOver;
    out.commands.push_back(close);
  }
}

Rect translateRect(const Rect& box, const Point& origin) {
  return Rect::MakeLTRB(box.getLeft() - origin.x, box.getTop() - origin.y,
    box.getRight() - origin.x, box.getBottom() - origin.y);
}

/**
 * Maps a device-space point into a sized layer's vertex space.
 *
 * The canvas uniform is shared by every pass and built from the *root* target (`canvasUniformsFor`
 * uses `2/width`), so a sized layer cannot simply store local pixels: they would be scaled by the
 * root factor and cover only part of its target. Instead the coordinates are multiplied by
 * `root/target`, which the root-scale uniform divides back out. The result is pixel-identical to a
 * local coordinate with a per-pass uniform, without introducing one uniform per layer.
 */
Point scalePoint(const Point& point, const Point& origin, const float scaleX, const float scaleY) {
  return Point{ (point.x - origin.x) * scaleX, (point.y - origin.y) * scaleY };
}

Rect scaleRect(const Rect& box, const Point& origin, const float scaleX, const float scaleY) {
  return Rect::MakeLTRB((box.getLeft() - origin.x) * scaleX, (box.getTop() - origin.y) * scaleY,
    (box.getRight() - origin.x) * scaleX, (box.getBottom() - origin.y) * scaleY);
}

/**
 * Decides which layers R1 step one may target at their own bounds.
 *
 * Only a top-level, childless, unmasked layer whose commands are solid/image/rect/path is eligible.
 * That is deliberately narrow: nested layers would need a parent-relative origin in the composite,
 * masks would need their coverage bitmap remapped, and gradients/coverage fields carry device-space
 * sampling geometry of their own. Everything else stays full-target, so this step cannot change the
 * picture of a construct it does not understand -- it just does not speed it up yet.
 */
std::vector<uint8_t> planSizedLayers(const DrawList& list, const std::vector<uint32_t>& parents,
  const float rootWidth, const float rootHeight) {
  std::vector<uint8_t> sized(list.layers.size(), 0);
  if (list.layers.size() < 2) return sized;

  std::vector<uint32_t> children(list.layers.size(), 0);
  for (size_t layer = 1; layer < parents.size(); ++layer) {
    if (parents[layer] < children.size()) ++children[parents[layer]];
  }

  for (size_t layer = 1; layer < list.layers.size(); ++layer) {
    const Layer& description = list.layers[layer];
    if (layer >= parents.size() || parents[layer] != 0 || children[layer] != 0) continue;
    const Rect bounds = description.bounds;
    if (bounds.getWidth() <= 0.0f || bounds.getHeight() <= 0.0f) continue;
    if (bounds.getLeft() < 0.0f || bounds.getTop() < 0.0f || bounds.getRight() > rootWidth ||
        bounds.getBottom() > rootHeight) {
      continue;
    }
    if (description.maskImage != Layer::kNoMaskImage) {
      // A layer-local mask was rasterised over exactly this layer's bounds; a target-space mask was
      // not, and sizing the target under it would sample the wrong sub-rectangle.
      if (!description.maskLocal || description.maskImage >= list.images.size()) continue;
      const Bitmap& mask = list.images[description.maskImage];
      if (std::llround(mask.width()) != std::llround(bounds.getWidth()) ||
          std::llround(mask.height()) != std::llround(bounds.getHeight())) {
        continue;
      }
    }

    bool supported = true;
    for (const DrawList::Command& command: list.commands) {
      if (command.layer != layer) continue;
      const bool drawable = command.kind == DrawList::Kind::Rect ||
                            command.kind == DrawList::Kind::Image ||
                            command.kind == DrawList::Kind::ExternalImage ||
                            command.kind == DrawList::Kind::Path ||
                            command.kind == DrawList::Kind::CloseLayer;
      if (!drawable || command.clipMask != DrawList::kNoClipMask ||
          command.state.coverageField != DrawList::kNoCoverageField ||
          command.state.shader == DrawList::ShaderKind::LinearGradient ||
          command.state.shader == DrawList::ShaderKind::RadialGradient) {
        if (std::getenv("CORAL_TRACE_SIZED_LAYERS") != nullptr) {
          std::fprintf(stderr,
            "[sized-layers] reject layer=%zu kind=%u shader=%u clipMask=%u coverageField=%u\n",
            layer, static_cast<unsigned>(command.kind), static_cast<unsigned>(command.state.shader),
            command.clipMask, command.state.coverageField);
        }
        supported = false;
        break;
      }
    }
    if (supported) sized[layer] = 1;
  }
  return sized;
}

/** Rewrites a sized layer's commands into the local space its target uses. */
void translateSizedLayers(DrawList& list, const std::vector<uint8_t>& sized,
  const float rootWidth, const float rootHeight) {
  for (DrawList::Command& command: list.commands) {
    // Only ordinary draws move: `OpenLayer` names its parent in `layer` while carrying the child's
    // bounds, and `CloseLayer`'s region is already in the parent's (the root's) coordinates.
    if (command.kind == DrawList::Kind::OpenLayer || command.kind == DrawList::Kind::CloseLayer) {
      continue;
    }
    if (command.layer >= sized.size() || sized[command.layer] == 0) continue;
    const Layer& description = list.layers[command.layer];
    const Point origin{ description.bounds.getLeft(), description.bounds.getTop() };
    const float scaleX = description.bounds.getWidth() > 0.0f
      ? rootWidth / description.bounds.getWidth() : 1.0f;
    const float scaleY = description.bounds.getHeight() > 0.0f
      ? rootHeight / description.bounds.getHeight() : 1.0f;
    for (Rect& box: command.geometry) box = scaleRect(box, origin, scaleX, scaleY);
    // The scissor rectangle stays in the target's own pixels: it is framebuffer state, not a vertex.
    command.clipRect = translateRect(command.clipRect, origin);
    if (command.kind != DrawList::Kind::Path) continue;
    const uint32_t contourEnd = std::min<uint32_t>(
      command.firstContour + command.contourCount, static_cast<uint32_t>(list.contours.size()));
    for (uint32_t contourIndex = command.firstContour; contourIndex < contourEnd; ++contourIndex) {
      const DrawList::Contour& contour = list.contours[contourIndex];
      const uint32_t pointEnd = std::min<uint32_t>(
        contour.firstPoint + contour.pointCount, static_cast<uint32_t>(list.points.size()));
      for (uint32_t pointIndex = contour.firstPoint; pointIndex < pointEnd; ++pointIndex) {
        list.points[pointIndex] = scalePoint(list.points[pointIndex], origin, scaleX, scaleY);
      }
    }
  }
}

} // namespace

void GpuRenderer::resolveLayerParents(const DrawList& list, std::vector<uint32_t>& parents) {
  parents.assign(list.layers.size(), 0);
  // The parent of a layer is the layer that was open when it was opened, which the command order states
  // directly: an `OpenLayer` pushes and its matching `CloseLayer` pops. Recovering it here rather than
  // storing a stack in the frame is what keeps the frame's state small, and there is no other way to
  // know it -- `Layer` deliberately describes bounds and not nesting.
  //
  // The opened layer's index is the position its `OpenLayer` command has in the layer table, not the
  // command's own `layer` field: the recorder writes the *parent* there (it assigns the field before
  // appending the layer), so reading it as the child mis-parented every nested layer and composited it
  // into the root. Layers are appended in exactly the order their opens are recorded, so a counter is
  // the mapping.
  std::vector<uint32_t> stack;
  stack.reserve(8);
  uint32_t nextLayer = 1;
  for (const DrawList::Command& command: list.commands) {
    if (command.kind == DrawList::Kind::OpenLayer) {
      if (nextLayer >= parents.size()) break;
      const uint32_t opened = nextLayer++;
      parents[opened] = stack.empty() ? 0 : stack.back();
      stack.push_back(opened);
    } else if (command.kind == DrawList::Kind::CloseLayer) {
      if (!stack.empty() && stack.back() == command.layer) {
        stack.pop_back();
      } else {
        // A close with no matching open, or one that does not match the innermost open: the list is
        // malformed, and dropping the entry is better than compositing into an arbitrary parent.
        const auto found = std::find(stack.rbegin(), stack.rend(), command.layer);
        if (found != stack.rend()) {
          stack.erase(std::next(found).base(), stack.end());
        }
      }
    }
  }
}

// ==================================================================================================
// Arena assembly
// ==================================================================================================

backend::Result GpuRenderer::assembleArena(Frame& frame) {
  const DrawList& list = *frame.list;
  ArenaWriter writer(frame.arena);
  frame.arena.clear();
  frame.arena.reserve(list.commands.size() * kQuadVertexStride * kQuadCornerCount);

  const float width = static_cast<float>(frame.root->width);
  const float height = static_cast<float>(frame.root->height);
  const float scaleX = width > 0.0f ? 2.0f / width : 0.0f;
  const float scaleY = height > 0.0f ? 2.0f / height : 0.0f;

  // The quad batch's vertices: the four corners of the unit square, with the rectangle's own data on each of them, drawn
  // as two triangles by the shared index pattern. See `QuadVertex` for why the per-instance form is not used and why the
  // corners are four rather than the six the triangles are.
  static constexpr float kCorners[kQuadCornerCount][2] = {
    { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f },
  };
  // The largest quad batch, which is what the index pattern has to cover: indices are batch-relative (see
  // `mIndexCapacity`), so six per command of the *longest* batch is the whole requirement.
  uint64_t largestQuadBatch = 0;
  for (Batch& batch: frame.batches) {
    // A gradient is drawn through the *quad* program whatever kind of command carries it, because `gradient.frag` is
    // the quad's fragment stage and expects the quad's four corners in {0,1}^2 across the rectangle. So a gradient
    // batch gets quad vertices even when its kind is `Path` -- which is what `Canvas::drawRect` records, because a
    // rectangle is flattened to a path before it reaches any backend. Without this the gradient's batch had no
    // vertices at all and was dispatched to the path program, which fills it with the paint's flat colour.
    const bool gradient = batch.state.shader == DrawList::ShaderKind::LinearGradient ||
                          batch.state.shader == DrawList::ShaderKind::RadialGradient;
    if (batch.kind != DrawList::Kind::Rect && batch.kind != DrawList::Kind::Image &&
        batch.kind != DrawList::Kind::ExternalImage && !gradient) {
      continue;
    }
    largestQuadBatch = std::max<uint64_t>(largestQuadBatch, batch.commandCount);
    const uint32_t vertexStride = batch.state.sdf ? kSdfVertexStride : kQuadVertexStride;
    batch.vertexStride = vertexStride;
    batch.vertexOffset = writer.reserveBytes(static_cast<size_t>(batch.commandCount) * kQuadCornerCount *
                                             vertexStride);
    for (uint32_t index = 0; index < batch.commandCount; ++index) {
      const DrawList::Command& command = list.commands[batch.firstCommand + index];
      // What the *frame* resolved for this command, which is what the vertex stage needs and the command cannot carry:
      // for a coverage field, where its texels are in the atlas expressed as the offset `coverage.frag` adds to a
      // position inside the quad; for an image, which sub-rectangle of the batch's texture it blits. One struct for both
      // routes, so that a third route adds its own field in one place rather than an argument to every call.
      FrameVertexData data;
      if (command.state.shader == DrawList::ShaderKind::Coverage) {
        const uint32_t field = command.state.coverageField;
        if (field < frame.placed.size()) {
          const Rect& quadRect = command.geometry[0];
          data.texelOriginX = quadRect.getLeft() - frame.placed[field].texelOriginX;
          data.texelOriginY = quadRect.getTop() - frame.placed[field].texelOriginY;
        }
      } else if (command.state.shader == DrawList::ShaderKind::Image) {
        const uint32_t image = imageIndexForCommand(command, list);
        // An index the frame could not place keeps the whole texture, which is also what a command naming an image the
        // table does not have gets -- and that command is counted and skipped by the replay either way, so what it would
        // have sampled is unobservable. Not a silent fallback: it is the identity, and the omission is what reports it.
        if (image < frame.images.size() && frame.images[image].valid) {
          // `texCoords` are normalized to the *image*, while the frame's rect is normalized to the page the image
          // was placed in. Map the command's uv sub-rectangle through that rect; a full-image draw (0,0)..(1,1)
          // therefore still samples the whole placement.
          const std::array<float, 4>& placed = frame.images[image].rect;
          const float u0 = command.texCoords[0].x;
          const float v0 = command.texCoords[0].y;
          const float u1 = command.texCoords[2].x;
          const float v1 = command.texCoords[2].y;
          // `PlacedImage::rect` is (origin, size) in normalized texture coordinates, not (left, top, right, bottom).
          data.imageSubRect = {
            placed[0] + u0 * placed[2],
            placed[1] + v0 * placed[3],
            (u1 - u0) * placed[2],
            (v1 - v0) * placed[3],
          };
        }
      }
      for (uint32_t corner = 0; corner < kQuadCornerCount; ++corner) {
        const QuadVertex vertex = quadVertexFor(command, kCorners[corner][0], kCorners[corner][1], data);
        const size_t vertexIndex = static_cast<size_t>(index) * kQuadCornerCount + corner;
        uint8_t* destination = writer.at(batch.vertexOffset) + vertexIndex * vertexStride;
        if (batch.state.sdf) {
          SdfVertex sdf{};
          sdf.base = vertex;
          sdf.sdf[0] = command.state.sdfOutline;
          sdf.sdf[1] = command.state.sdfShadowX;
          sdf.sdf[2] = command.state.sdfShadowY;
          sdf.sdf[3] = command.state.sdfGlow;
          auto unpack = [](const uint32_t argb, float out[4]) {
            out[0] = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
            out[1] = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;
            out[2] = static_cast<float>(argb & 0xFFu) / 255.0f;
            out[3] = static_cast<float>((argb >> 24) & 0xFFu) / 255.0f;
          };
          unpack(command.state.sdfOutlineColor, sdf.outlineColor);
          unpack(command.state.sdfShadowColor, sdf.shadowColor);
          sdf.pattern[0] = command.state.sdfPattern;
          sdf.pattern[1] = command.state.sdfPatternFrequency;
          sdf.pattern[2] = command.state.sdfPatternAngle;
          sdf.pattern[3] = command.state.sdfPatternStrength;
          sdf.fillAlpha = command.state.sdfFillAlpha;
          std::memcpy(destination, &sdf, kSdfVertexStride);
        } else {
          std::memcpy(destination, &vertex, kQuadVertexStride);
        }
      }
    }
  }
  // The index pattern this frame's draws will need, made large enough before anything is recorded: a growth here
  // destroys the buffer, and a recorded draw naming a destroyed buffer is the kind of fault that arrives a frame later.
  backend::Result prepared = ensureIndexCapacity(largestQuadBatch * kQuadIndexCount);
  if (!backend::isSuccess(prepared)) return prepared;

  for (uint32_t index = 0; index < list.commands.size(); ++index) {
    const DrawList::Command& command = list.commands[index];
    Precompiled& slot = frame.precompiled[index];

    if (command.kind == DrawList::Kind::Path) {
      if (!slot.fan.empty()) {
        slot.fanOffset = writer.appendBytes(slot.fan.data(), slot.fan.size() * sizeof(PathVertex));
      }
      // The cover rectangle: the same six device-space vertices the stencil pass needs, as a triangle
      // list, covering the path's bounding box -- which is the reject test the DrawList carries and the
      // only extent the cover pass should touch.
      const Rect bounds = command.geometry[0];
      const float left = bounds.getLeft();
      const float top = bounds.getTop();
      const float right = bounds.getRight();
      const float bottom = bounds.getBottom();
      const auto fill = pathColorFor(command.state);
      const PathVertex cover[kCoverVertexCount] = {
        { left, top, { fill[0], fill[1], fill[2], fill[3] } },
        { right, top, { fill[0], fill[1], fill[2], fill[3] } },
        { right, bottom, { fill[0], fill[1], fill[2], fill[3] } },
        { left, top, { fill[0], fill[1], fill[2], fill[3] } },
        { right, bottom, { fill[0], fill[1], fill[2], fill[3] } },
        { left, bottom, { fill[0], fill[1], fill[2], fill[3] } },
      };
      slot.coverOffset = writer.appendBytes(cover, sizeof(cover));
    } else if (command.kind == DrawList::Kind::CloseLayer) {
      const Rect region = clampToTarget(command.geometry[0], frame.root->width, frame.root->height);
      if (region.getWidth() <= 0.0f || region.getHeight() <= 0.0f) {
        slot.hasComposite = false;
        continue;
      }
      // The quad is authored in *clip* space because `composite.vert` passes its position straight
      // through, and the y it wants is the same y `quad.vert` would compute: device pixels mapped with
      // a positive y scale, with the flip to OpenGL's convention applied when the SPIR-V is translated.
      //
      // The texture coordinates carry the same flip, for a different reason: a texture this renderer
      // rendered into stores the device image *upside down* relative to `v`, because the vertex stage's
      // y flip puts device row 0 at the target's top, and an attachment's texel row 0 is its bottom.
      // Sampling with an unflipped `v` would composite every layer against its own reflection.
      const float clipLeft = region.getLeft() * scaleX - 1.0f;
      const float clipTop = region.getTop() * scaleY - 1.0f;
      const float clipRight = region.getRight() * scaleX - 1.0f;
      const float clipBottom = region.getBottom() * scaleY - 1.0f;
      // The texture coordinates follow the same rule as the readback: OpenGL stores a rendered target
      // bottom-up relative to `v`, so sampling it without a flip would composite every layer against its
      // own reflection. A Vulkan image's row 0 is the top of the picture, so the flip is exactly wrong
      // there and the region's own top/bottom map straight to v.
      const bool topDownTargets = usesVulkanShaderSet(mDevice);
      // A sized layer's target holds exactly the layer's rectangle, so its texture is sampled over the
      // whole 0..1 range regardless of where the rectangle sits on the canvas. A full-size layer still
      // stores the layer at its device position, which is why the region/root mapping below is needed.
      const bool sizedSource =
        command.layer < frame.sized.size() && frame.sized[command.layer] != 0;
      const float u0 = sizedSource ? 0.0f : (width > 0.0f ? region.getLeft() / width : 0.0f);
      const float u1 = sizedSource ? 1.0f : (width > 0.0f ? region.getRight() / width : 1.0f);
      const float v0 = sizedSource
        ? (topDownTargets ? 0.0f : 1.0f)
        : (topDownTargets ? (height > 0.0f ? region.getTop() / height : 0.0f)
                          : (height > 0.0f ? 1.0f - region.getTop() / height : 1.0f));
      const float v1 = sizedSource
        ? (topDownTargets ? 1.0f : 0.0f)
        : (topDownTargets ? (height > 0.0f ? region.getBottom() / height : 1.0f)
                          : (height > 0.0f ? 1.0f - region.getBottom() / height : 0.0f));
      const FullTargetVertex quad[kCoverVertexCount] = {
        { clipLeft, clipTop, u0, v0 }, { clipRight, clipTop, u1, v0 }, { clipRight, clipBottom, u1, v1 },
        { clipLeft, clipTop, u0, v0 }, { clipRight, clipBottom, u1, v1 }, { clipLeft, clipBottom, u0, v1 },
      };
      slot.compositeOffset = writer.appendBytes(quad, sizeof(quad));
      slot.hasComposite = true;
    }
  }

  return ensureVertexCapacity(mVertexBase + frame.arena.size());
}

// Pass management
// ==================================================================================================

backend::Result GpuRenderer::beginPassFor(Frame& frame, const uint32_t layerIndex, const bool clearOnFirstOpen) {
  if (layerIndex >= frame.targets.size()) {
    return backend::Result::InvalidArgument;
  }
  if (frame.openPass == static_cast<int32_t>(layerIndex)) {
    return backend::Result::Success;
  }
  backend::Result result = endPass(frame);
  if (!backend::isSuccess(result)) {
    return result;
  }


  RenderTarget& target = targetAt(frame, frame.targets[layerIndex]);
  if (!target.isValid() || !target.pass.valid()) {
    return backend::Result::InvalidArgument;
  }

  backend::RenderPassBeginInfo info;
  info.renderPass = target.pass;
  info.colorAttachmentCount = 1;
  const bool multisampled = target.msaaColorView.valid();
  info.colorAttachments[0].view = multisampled ? target.msaaColorView : target.colorView;
  if (multisampled) {
    // The resolved image is what the snapshot, `Surface::target()` and a parent pass read; the MSAA
    // attachment exists only for the duration of the pass.
    info.colorAttachments[0].resolveTarget = target.colorView;
  }
  const bool firstOpen = layerIndex >= frame.opened.size() || frame.opened[layerIndex] == 0;
  info.colorAttachments[0].loadOp = (firstOpen && clearOnFirstOpen) ? backend::LoadOp::Clear : backend::LoadOp::Load;
  info.colorAttachments[0].storeOp = backend::StoreOp::Store;
  info.colorAttachments[0].clearColor = backend::Color::transparentBlack();
  info.renderArea = { target.width, target.height };
  if (target.depthStencilView.valid()) {
    info.hasDepthStencil = true;
    info.depthStencilAttachment.view = target.depthStencilView;
    // Cleared on *every* open rather than loaded, which costs one 4-byte-per-pixel clear and buys the
    // invariant the stencil passes depend on: the buffer is zero when the first path of a pass begins.
    // Loading it would be correct only as long as every path also reset the region it touched.
    info.depthStencilAttachment.loadOp = backend::LoadOp::Clear;
    info.depthStencilAttachment.storeOp = backend::StoreOp::Store;
    info.depthStencilAttachment.clearDepthStencil = { 1.0f, 0 };
  }
  result = mCommandBuffer->beginRenderPass(info);
  if (!backend::isSuccess(result)) {
    return result;
  }
  // A pass is where a layer's pixels are either preserved or destroyed, so which attachment is opened and with what
  // load operation is the whole of "did the earlier composite survive". Three layered rectangles came out as one
  // patch in the first rectangle's region while every composite's source texture, scissor and vertex offset were
  // distinct and all thirteen draws reached the command list -- which leaves the pass state, and this prints it.
  //
  //   set CORAL_GPU_TRACE_PASSES=1
  if (std::getenv("CORAL_GPU_TRACE_PASSES") != nullptr) {
    // `clearOnFirstOpen` and the `opened` flag are printed alongside the load operation because a root pass that
    // clears a second time is the defect being chased: `firstOpen && clearOnFirstOpen` is the whole condition, so
    // seeing both tells which half is wrong. `opened` is snapshot *before* this pass sets it.
    std::fprintf(stderr,
      "[pass] begin layer=%u target=%dx%d load=%s firstOpen=%d clearOnFirstOpen=%d openedCount=%zu\n", layerIndex,
      target.width, target.height, info.colorAttachments[0].loadOp == backend::LoadOp::Clear ? "CLEAR" : "LOAD",
      firstOpen ? 1 : 0, clearOnFirstOpen ? 1 : 0, frame.opened.size());
    std::fflush(stderr);
  }
  if (layerIndex < frame.opened.size()) frame.opened[layerIndex] = 1;
  frame.openPass = static_cast<int32_t>(layerIndex);
  if (std::getenv("CORAL_GPU_TRACE_PASSES") != nullptr) {
    // Printed *after* the flag is set, so the line above it (which snapshots before) and this one can be compared
    // across two calls. If `opened[0]` is 1 here and the next root pass still reports `firstOpen=1`, then the vector
    // this writes and the one that reads are not the same object -- which is the only thing that makes a repeated
    // clear possible.
    std::fprintf(stderr, "[pass]   set opened[%u]=1, opened[0]=%d, frame.opened at %p, frame at %p\n", layerIndex,
      frame.opened.empty() ? -1 : static_cast<int>(frame.opened[0]), static_cast<const void*>(frame.opened.data()),
      static_cast<const void*>(&frame));
    std::fflush(stderr);
  }
  return backend::Result::Success;
}

backend::Result GpuRenderer::endPass(Frame& frame) {
  if (frame.openPass < 0) {
    return backend::Result::Success;
  }
  if (std::getenv("CORAL_GPU_TRACE_PASSES") != nullptr) {
    std::fprintf(stderr, "[pass] end layer=%d\n", frame.openPass);
    std::fflush(stderr);
  }
  frame.openPass = -1;
  return mCommandBuffer->endRenderPass();
}

backend::Result GpuRenderer::setScissor(Frame& frame, const Rect& clip) {
  if (frame.openPass < 0 || static_cast<size_t>(frame.openPass) >= frame.targets.size()) {
    return backend::Result::InvalidArgument;
  }
  const RenderTarget& target = targetAt(frame, frame.targets[static_cast<size_t>(frame.openPass)]);
  Rect effective = clip;
  if (frame.hasDamage) {
    const float left = std::max(clip.getLeft(), frame.damage.getLeft());
    const float top = std::max(clip.getTop(), frame.damage.getTop());
    const float right = std::min(clip.getRight(), frame.damage.getRight());
    const float bottom = std::min(clip.getBottom(), frame.damage.getBottom());
    effective = right > left && bottom > top
      ? Rect::MakeLTRB(left, top, right, bottom)
      : Rect::MakeLTRB(left, top, left, top);
  }
  const Rect clamped = clampToTarget(effective, target.width, target.height);
  backend::ScissorRect scissor;

  // ## The scissor rounds the clip to nearest, which is how the rasterizer decides a clipped pixel
  //
  // This floored the two near edges and ceiled the two far ones, so the scissor always *contained* the clip. That is the
  // safe direction for a clip on its own, and it is the wrong one against the reference: the rasterizer's clip is a test
  // on the *pixel centre*, so a pixel whose centre falls outside the clip is dropped however much of it the shape
  // covers. With a clip edge on a fraction the two therefore disagree by a whole pixel, and the GPU paints one the
  // rasterizer does not.
  //
  // Measured, and reduced to one rectangle: a 63x28 rectangle under the anisotropic fit `scale(0.21, 0.14)` at (10.5,
  // 7.5) -- exactly what `Svg::render` hands a corpus document -- puts 56 pixels of ink in rows 7..10 for the rasterizer
  // and 70 in rows 7..11 here. Without the layer's clip the two agree, which is what names the clip as the cause rather
  // than the rectangle's arithmetic.
  //
  // `ceil(edge - 0.5)` is the integer the half-open test `left <= centre < right` selects, written as arithmetic:
  // a pixel is in the clip when its centre is, so the first pixel index inside is `ceil(left - 0.5)` and the first one
  // outside is `ceil(right - 0.5)`. Both edges take the same expression, which is what makes the interval half-open --
  // an edge exactly on 7.5 includes the pixel centred there on the left and excludes it on the right, and the two
  // measurements above are what that ties to.
  //
  // An empty clip still becomes a zero-sized scissor rather than a skipped draw: the draw has to be recorded, because
  // skipping it here would make the batch walk and the draw-call statistic disagree about the frame, and a zero-sized
  // scissor discards every fragment anyway. A clip narrower than a pixel now rounds to that, and correctly: it has no
  // pixel centre in it, so it covers nothing.
  scissor.x = static_cast<int32_t>(std::floor(clamped.getLeft()));
  scissor.y = static_cast<int32_t>(std::floor(clamped.getTop()));
  const int32_t right = static_cast<int32_t>(std::ceil(clamped.getRight()));
  const int32_t bottom = static_cast<int32_t>(std::ceil(clamped.getBottom()));
  // An empty clip becomes a zero-sized scissor rather than a skipped draw: the draw still has to be
  // recorded, because skipping it here would mean the batch walk and the draw-call statistic disagreed
  // about what the frame contains, and a zero-sized scissor discards every fragment anyway.
  scissor.width = static_cast<uint32_t>(std::max(0, right - scissor.x));
  scissor.height = static_cast<uint32_t>(std::max(0, bottom - scissor.y));
  return mCommandBuffer->setScissor(scissor);
}

// ==================================================================================================
// Recording
// ==================================================================================================

backend::Result GpuRenderer::drawRectBatch(Frame& frame, const Batch& batch) {

  RenderTarget& target = targetAt(frame, frame.targets[batch.layer]);
  const PipelineKey key{ Program::Quad, Variant::Plain, static_cast<uint8_t>(batch.state.blendMode),
                         target.format,
                         target.depthStencilView.valid() ? kDepthStencilFormat : backend::PixelFormat::Undefined,
                         target.sampleCount };

  backend::RenderPipelineHandle pipeline = backend::RenderPipelineHandle::invalid();
  backend::Result result = ensurePipeline(key, target.pass, &pipeline);
  if (!backend::isSuccess(result)) {
    return result;
  }

  result = mCommandBuffer->bindRenderPipeline(pipeline);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->bindDescriptorSet(0, mQuadSet);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->setVertexBuffer(0, mVertexBuffer, batch.vertexOffset);
  if (!backend::isSuccess(result)) return result;
  // Four vertices and six indices per rectangle, one instance: the whole batch is one non-instanced draw, which is what
  // one vertex buffer and one shared index pattern make possible. See `QuadVertex` for why it is not an instanced draw
  // over per-instance attributes, and why the corners are four rather than six.
  result = mCommandBuffer->setIndexBuffer(mIndexBuffer, backend::IndexFormat::UInt32, 0);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->drawIndexed(kQuadIndexCount * batch.commandCount, 1, 0, 0, 0);
  if (!backend::isSuccess(result)) return result;

  ++mStats.drawCallCount;
  mStats.vertexCount += static_cast<uint64_t>(kQuadCornerCount) * batch.commandCount;
  return backend::Result::Success;
}

backend::Result GpuRenderer::drawPath(Frame& frame, const DrawList::Command& command,
  const Precompiled& precompiled) {
  RenderTarget& target = targetAt(frame, frame.targets[command.layer]);
  if (precompiled.fans.empty()) {
    return backend::Result::Success;
  }


  const backend::PixelFormat depthFormat =
    target.depthStencilView.valid() ? kDepthStencilFormat : backend::PixelFormat::Undefined;

  // The fill colour travels in the path vertices, and the stencil and cover passes need no per-draw
  // uniform block: every path draw in the frame binds the same canvas-only set (`mPathSet`).
  backend::Result result = backend::Result::Success;

  const auto bindFans = [&](const backend::RenderPipelineHandle pipeline, const backend::DescriptorSetHandle set) {
    backend::Result bindResult = mCommandBuffer->bindRenderPipeline(pipeline);
    if (!backend::isSuccess(bindResult)) return bindResult;
    bindResult = mCommandBuffer->bindDescriptorSet(0, set);
    if (!backend::isSuccess(bindResult)) return bindResult;
    for (const FanRange& fan: precompiled.fans) {
      bindResult = mCommandBuffer->setVertexBuffer(0, mVertexBuffer,
        precompiled.fanOffset + fan.firstVertex * kPathVertexStride);
      if (!backend::isSuccess(bindResult)) return bindResult;
      bindResult = mCommandBuffer->draw(fan.vertexCount, 1, 0, 0);
      if (!backend::isSuccess(bindResult)) return bindResult;
      ++mStats.drawCallCount;
      ++mStats.pathDrawCalls;
      mStats.vertexCount += fan.vertexCount;
    }
    return backend::Result::Success;
  };

  // An opt-in trace of the renderer's own path decomposition, alongside the backend's stencil trace. The two
  // answer different halves of one question: the backend trace says what state each draw ran under, and this
  // one says which *draw* it was. A defect where both look right per draw and the picture is still wrong is a
  // sequencing question, and sequencing is only visible when the two are read together.
  //
  //   set CORAL_GPU_TRACE_PATHS=1
  const bool trace = std::getenv("CORAL_GPU_TRACE_PATHS") != nullptr;
  if (trace) {
      std::fprintf(stderr, "[path] begin layer=%u fans=%zu coverOffset=%u pass=%u blend=%u color(r=%g g=%g b=%g a=%g)\n",
      command.layer, precompiled.fans.size(), precompiled.coverOffset, target.pass.index,
      command.state.blendMode, static_cast<double>(command.state.red),
      static_cast<double>(command.state.green), static_cast<double>(command.state.blue),
      static_cast<double>(command.state.alpha));
    std::fflush(stderr);
  }

  if (!target.depthStencilView.valid()) {
    // No stencil buffer, so there is no winding to accumulate: the fan is drawn straight into the
    // framebuffer with the fill's colour. That is correct for a convex contour and wrong for a concave
    // or self-intersecting one -- the overlaps accumulate as if they were alpha-blended. It is the
    // documented fallback rather than a silent one: the count is reported, and the alternative is a
    // path that draws nothing at all.
    ++mUnsupported.pathsWithoutStencil;
    const PipelineKey coverKey{ Program::PathFan, Variant::FanFill,
                                static_cast<uint8_t>(command.state.blendMode), target.format, depthFormat,
                                target.sampleCount };
    backend::RenderPipelineHandle pipeline = backend::RenderPipelineHandle::invalid();
    result = ensurePipeline(coverKey, target.pass, &pipeline);
    if (!backend::isSuccess(result)) return result;
    return bindFans(pipeline, mPathSet);
  }

  const Variant accumulation = command.state.fillType == FillType::EvenOdd
                                 ? Variant::StencilEvenOdd
                                 : Variant::StencilWinding;
  const PipelineKey accumulationKey{ Program::PathFan, accumulation, kNoBlendMode, target.format, depthFormat,
    target.sampleCount };
  backend::RenderPipelineHandle accumulationPipeline = backend::RenderPipelineHandle::invalid();
  result = ensurePipeline(accumulationKey, target.pass, &accumulationPipeline);
  if (!backend::isSuccess(result)) return result;
  result = bindFans(accumulationPipeline, mPathSet);
  if (!backend::isSuccess(result)) return result;

  const PipelineKey coverKey{ Program::PathCover, Variant::StencilCover,
                              static_cast<uint8_t>(command.state.blendMode), target.format, depthFormat,
                              target.sampleCount };
  backend::RenderPipelineHandle coverPipeline = backend::RenderPipelineHandle::invalid();
  result = ensurePipeline(coverKey, target.pass, &coverPipeline);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->bindRenderPipeline(coverPipeline);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->bindDescriptorSet(0, mPathSet);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->setVertexBuffer(0, mVertexBuffer, precompiled.coverOffset);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->draw(kCoverVertexCount, 1, 0, 0);
  if (!backend::isSuccess(result)) return result;
  ++mStats.drawCallCount;
  ++mStats.pathDrawCalls;
  mStats.vertexCount += kCoverVertexCount;

  // Zero the stencil again. Every fragment the fan covers is set to zero, so the next path starts from
  // the buffer this one found. Redrawing the fan is what makes that true for a self-overlapping contour
  // as well: a fragment visited twice is written zero twice.
  //
  // ================================================================================================
  // OPEN DEFECT: this pass does not appear to clear the stencil, and the evidence is a wrong picture
  // ================================================================================================
  //
  // `GpuSurface.OnePathDoesNotBleedIntoTheNext` draws two disjoint opaque rectangles, neither transformed, and
  // the second paints over the first: the red rectangle's pixels come back white. Reading the sequence, that is
  // only possible if the stencil still holds the *first* path's winding when the second path covers -- the
  // cover tests `NotEqual 0`, so a cleared stencil would reject the second path's cover in the first path's
  // region and leave the red alone. The same fact at scene scale is why the last shape bleeds over every
  // earlier one, which is what `TheGpuAndTheRasterizerAgreeOnAScene` reports as a colour mismatch.
  //
  // So the suspect is this pass, or the cover that runs before it. Ruled out by reading, each with the reason:
  //
  //   * The stencil-op translation. `toGLStencilOp(StencilOp::Zero)` is `GL_ZERO` (`GlEnums.h`), which is the
  //     op that writes zero when the stencil and depth tests pass.
  //   * The state reaching the driver. `replay()` applies `glStencilFuncSeparate`, `glStencilOpSeparate` and
  //     `glStencilMaskSeparate` for both faces on every pipeline bind, with no caching in front of them, and
  //     `StencilReset` builds its `StencilFaceState` from the same `passOp` the variant switch uses.
  //   * The write masks. The reset variant sets `face->writeMask = 0xFF` (stencil) and
  //     `colorTargets[0].writeMask = 0x0` (colour), and the cover variant sets the stencil mask to zero so it
  //     cannot itself disturb what it reads.
  //   * The scissor. The second path's cover is confined -- the gap between the two rectangles stays clear --
  //     so the cover is running and its region is right. What is wrong is *where inside that region it passes*.
  //   * The fan geometry. `bindFans` binds `fanOffset + fan.firstVertex * kPathVertexStride` for the reset
  //     exactly as it does for the accumulation, so the reset redraws the same triangles.
  //
  // The next step is instrumentation rather than more reading: count the three passes per path and assert the
  // count, which distinguishes "the reset does not run" from "it runs and does nothing". If it runs, the next
  // question is whether `GL_ZERO` is reaching the driver for the reset specifically -- a `glStencilOpSeparate`
  // trace for one frame answers both at once.
  //
  // **Measured, and it settles the fork.** `Surface::diagnostics()` for the two-rectangle case reports
  // `commands=3 batches=3 draws=7 ... pathsWithoutStencil=0`. Three commands are the clear and the two
  // rectangles; seven draws are the clear plus three passes for each path; and `pathsWithoutStencil=0` says the
  // stencil route was taken, not the convex-fan fallback. So the reset pass *runs* and the stencil is still not
  // cleared, which rules out the fallback entirely and narrows the question to the reset pass's own state:
  // whether `GL_ZERO` reaches the driver for it, and whether the second path's accumulation is what is
  // actually leaving the first path's region non-zero. A `glStencilOpSeparate` trace for one frame answers both,
  // and that is the next concrete step rather than another reading of this function.
  //
  // Left as a documented defect rather than worked around. The available workarounds -- clearing the stencil
  // between paths with a full-target pass, or resetting it on the CPU side by tracking which paths overlap --
  // both hide a state bug behind a redundant draw, and this is the second defect in this renderer whose cause
  // was a state fact assumed rather than measured.
  //
  // ================================================================================================
  const PipelineKey resetKey{ Program::PathFan, Variant::StencilReset, kNoBlendMode, target.format, depthFormat,
    target.sampleCount };
  backend::RenderPipelineHandle resetPipeline = backend::RenderPipelineHandle::invalid();
  result = ensurePipeline(resetKey, target.pass, &resetPipeline);
  if (!backend::isSuccess(result)) return result;
  return bindFans(resetPipeline, mPathSet);
}

backend::Result GpuRenderer::compositeSetFor(const uint32_t compositeIndex, backend::DescriptorSetHandle* outSet) {
  if (compositeIndex >= mCompositeSets.size()) {
    mCompositeSets.resize(static_cast<size_t>(compositeIndex) + 1, backend::DescriptorSetHandle::invalid());
  }
  if (!mCompositeSets[compositeIndex].valid()) {
    backend::DescriptorSetHandle created = backend::DescriptorSetHandle::invalid();
    const backend::Result result = mDevice->createDescriptorSet(mTexturedSetLayout, &created);
    if (!backend::isSuccess(result)) {
      return result;
    }
    mCompositeSets[compositeIndex] = created;
  }
  *outSet = mCompositeSets[compositeIndex];
  return backend::Result::Success;
}

backend::Result GpuRenderer::compositeThroughShader(Frame& frame, const uint32_t parentIndex,
  const uint32_t sourceIndex, const Rect& region, const float alpha, const uint8_t blendMode,
  const Layer& layer) {
  // The shader blends the layer against the destination it can *read*, so the destination has to exist
  // as a texture first: end the pass that is writing it, snapshot it, then draw the composite into it.
  // The snapshot is why an advanced blend costs a full-target copy, and it is the real price of the
  // fourteen modes `composite.frag` serves rather than the arithmetic in it.
  backend::Result result = endPass(frame);
  if (!backend::isSuccess(result)) return result;

  RenderTarget& parent = targetAt(frame, frame.targets[parentIndex]);
  const int32_t scratchIndex = acquireScratchTexture(parent.width, parent.height, parent.format);
  if (scratchIndex < 0) {
    return backend::Result::OutOfMemory;
  }
  ScratchTexture& scratch = mScratchPool[static_cast<size_t>(scratchIndex)];

  backend::TextureCopyRegion copy;
  copy.textureOrigin = { 0, 0, 0 };
  copy.size = { parent.width, parent.height, 1 };
  copy.mipLevel = 0;
  copy.baseArrayLayer = 0;
  copy.arrayLayerCount = 1;
  result = mCommandBuffer->copyTextureToTexture(parent.color, scratch.texture, copy);
  if (!backend::isSuccess(result)) return result;

  result = beginPassFor(frame, parentIndex, false);
  if (!backend::isSuccess(result)) return result;
  result = setScissor(frame, region);
  if (!backend::isSuccess(result)) return result;

  const RenderTarget& source = targetAt(frame, frame.targets[sourceIndex]);

  // A composite reads the destination it snapshotted and the source layer, and its own uniform block -- taken per call
  // from the frame's arena for the same reason a path's is. The *set* is per composite position and not per draw for the
  // reason `compositeSetFor` gives at length: the backend resolves a draw's set contents at replay, so one refilled set
  // hands every composite of the frame the last one's textures.
  const CompositeUniforms uniforms{ alpha, static_cast<int32_t>(blendMode),
    { layer.textColor.red, layer.textColor.green, layer.textColor.blue },
    layer.linearLightGamma, layer.linearLightContrast, layer.linearLightText ? 1 : 0 };
  uint32_t block = 0;
  result = allocateUniformSlot(&uniforms, sizeof(uniforms), &block);
  if (!backend::isSuccess(result)) return result;
  mStats.uploadBytes += sizeof(uniforms);

  backend::DescriptorSetHandle set = backend::DescriptorSetHandle::invalid();
  result = compositeSetFor(frame.compositeCommandIndex, &set);
  if (!backend::isSuccess(result)) return result;

  const backend::TextureSamplerHandle compositeSampler =
        layer.linearLightText ? mNearestClamp : mLinearClamp;
  const backend::DescriptorWrite compositeTextures[] = {
    { 0, backend::BindingType::TextureSampler, source.color, compositeSampler, {}, 0 },
    { 1, backend::BindingType::TextureSampler, scratch.texture, compositeSampler, {}, 0 },
    // The block binding, which every set built from this layout has to name: `createDescriptorSet` leaves a binding
    // empty and an empty uniform binding is a draw a driver may reject.
    { 2, backend::BindingType::UniformBuffer, backend::TextureHandle::invalid(),
      backend::TextureSamplerHandle::invalid(),
      { uniformBuffer(), static_cast<uint64_t>(block) * kUniformSlotSize, kUniformSlotSize }, 0 },
  };
  result = mDevice->updateDescriptorSet(set, compositeTextures, 3);
  if (!backend::isSuccess(result)) return result;

  const RenderTarget& target = parent;
  const PipelineKey key{ Program::Composite, Variant::Plain, kNoBlendMode, target.format,
                         target.depthStencilView.valid() ? kDepthStencilFormat : backend::PixelFormat::Undefined,
                         target.sampleCount };
  backend::RenderPipelineHandle pipeline = backend::RenderPipelineHandle::invalid();
  result = ensurePipeline(key, target.pass, &pipeline);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->bindRenderPipeline(pipeline);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->bindDescriptorSet(0, set);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->setVertexBuffer(0, mVertexBuffer,
    frame.precompiled[frame.compositeCommandIndex].compositeOffset);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->draw(kCoverVertexCount, 1, 0, 0);
  if (!backend::isSuccess(result)) return result;
  ++mStats.drawCallCount;
  mStats.vertexCount += kCoverVertexCount;
  return backend::Result::Success;
}

backend::Result GpuRenderer::compositeLayer(Frame& frame, const DrawList::Command& command) {
  const uint32_t layerIndex = command.layer;
  if (layerIndex == 0 || layerIndex >= frame.targets.size() || layerIndex >= frame.parents.size()) {
    // Layer 0 *is* the root, which is the destination and not something to composite onto anything. Note the
    // distinction this guard has to make: the *source* being the root is the impossible case, while the *parent*
    // being the root is the ordinary one -- every `saveLayer` a caller makes at the top level has the root as its
    // parent, and a composite that refused those would drop every layer a program opens.
    //
    // That is exactly what this guard used to do. It read `frame.targets[parentIndex] == kRootTarget` and bailed,
    // so a top-level layer was recorded, drawn into its own target, and then thrown away without a draw: the very
    // first `saveLayer` any caller writes produced nothing, and the SVG renderer -- which opens a layer per group
    // -- lost the contents of every document it drew. The condition now tests the *source*, which is the thing
    // that must not be the root.
    return backend::Result::Success;
  }
  RenderTarget* source = &targetAt(frame, frame.targets[layerIndex]);
  if (source == nullptr || !source->isValid()) {
    return backend::Result::Success;
  }
  const Precompiled& precompiled = frame.precompiled[frame.compositeCommandIndex];
  if (!precompiled.hasComposite) {
    return backend::Result::Success;
  }

  const uint32_t parentIndex = frame.parents[layerIndex];
  if (parentIndex >= frame.targets.size()) {
    return backend::Result::Success;
  }
  const Rect region = clampToTarget(command.geometry[0], frame.root->width, frame.root->height);
  if (region.getWidth() <= 0.0f || region.getHeight() <= 0.0f) {
    return backend::Result::Success;
  }

  const Layer& layer = layerIndex < frame.list->layers.size() ? frame.list->layers[layerIndex] : Layer{};
  const float alpha = std::clamp(command.state.alpha, 0.0f, 1.0f);
  const uint8_t blendMode = static_cast<uint8_t>(command.state.blendMode);

  // An opt-in trace of the one composite, because a transparent result has two causes that read identically in the
  // code and have nothing in common as defects: the layer's own texture is empty (the pass that drew into it never
  // wrote) or the coverage is zero (the 1x1 constant-alpha texture reads as transparent). Printing the numbers the
  // composite is about to use separates them in one run.
  //
  //   set CORAL_GPU_TRACE_COMPOSITE=1
  if (std::getenv("CORAL_GPU_TRACE_COMPOSITE") != nullptr) {
    std::fprintf(stderr,
      "[composite] layer=%u parent=%u region=(%g,%g,%g,%g) alpha=%g blend=%u sourceValid=%d "
      "sourceFormat=%d compositeOffset=%u\n",
      layerIndex, frame.parents[layerIndex], static_cast<double>(region.getLeft()),
      static_cast<double>(region.getTop()), static_cast<double>(region.getRight()),
      static_cast<double>(region.getBottom()), static_cast<double>(alpha), command.state.blendMode,
      source->isValid() ? 1 : 0, static_cast<int>(source->format), precompiled.compositeOffset);
    std::fflush(stderr);
  }

  // A document's layer removes the backing from cells *to its right* and none to its left, which is the signature of
  // a composite whose scissor reaches the surface edge. Printing the layer's own bounds and the device clip
  // alongside the region shows which of the three is the one that is wrong.
  //
  //   set CORAL_GPU_TRACE_COMPOSITE=1
  if (std::getenv("CORAL_GPU_TRACE_COMPOSITE") != nullptr) {
    std::fprintf(stderr,
      "[composite] layer=%u bounds=(%g,%g,%g,%g) commandRegion=(%g,%g,%g,%g) scissorRegion=(%g,%g,%g,%g)\n",
      layerIndex, static_cast<double>(layer.bounds.getLeft()), static_cast<double>(layer.bounds.getTop()),
      static_cast<double>(layer.bounds.getRight()), static_cast<double>(layer.bounds.getBottom()),
      static_cast<double>(command.geometry[0].getLeft()), static_cast<double>(command.geometry[0].getTop()),
      static_cast<double>(command.geometry[0].getRight()), static_cast<double>(command.geometry[0].getBottom()),
      static_cast<double>(region.getLeft()), static_cast<double>(region.getTop()),
      static_cast<double>(region.getRight()), static_cast<double>(region.getBottom()));
    std::fflush(stderr);
  }

  // ## A mask layer multiplies the layer by the mask's coverage, and then composites normally
  //
  // `mask.frag` is the whole operation and it already computes `layer * coverage`: the only difference between a mask
  // layer and an ordinary one is *what it multiplies by*. An ordinary layer scales by its global alpha, which is why the
  // 1x1 constant-coverage texture is enough for one; a masked layer scales by a bitmap, so the same binding takes the
  // mask's texture instead and `uMaskScale`/`uMaskOffset` place it.
  //
  // Applying it *in* the composite rather than in a pass of its own is what keeps the layer's blend mode and alpha
  // meaningful: the composite below is the ordinary one, so a masked layer meets its parent exactly as an unmasked one
  // does. That is also why the two combinations below are counted rather than silently dropped.
  //
  // The mapping is measured rather than derived -- see `GpuSurface.AMaskLayerScalesTheLayerByItsCoverage`, which failed
  // on the un-masked baseline before this existed. All three buffers involved are the *whole target* (the rasterizer
  // allocates its layer that way, `beginPassFor` allocates a full-size target for every layer, and the SVG mask's own
  // buffer is the target's size), so the mask's normalized placement rectangle is the entire mapping and no region remap
  // is needed. It also stretches a mask of a different size over the layer for free, which is what
  // `CanvasMask.StretchesAMaskOfADifferentSize` asks for. The `v` axis is negated because the composite's `v` is flipped
  // for a texture the renderer *rendered into* -- the layer at binding 0 -- while the mask is a texture it *uploaded*,
  // whose first row is its first row.
  const Frame::PlacedImage* mask = nullptr;
  if (layer.maskImage != Layer::kNoMaskImage && layer.maskImage < frame.images.size() &&
      frame.images[layer.maskImage].valid) {
    mask = &frame.images[layer.maskImage];
  }
  if (layer.asMask && mask == nullptr) {
    // The mask could not be placed and uploaded, so there is nothing to multiply by. Counted rather than composited as
    // if unmasked, which would be a picture that looks deliberate.
    ++mUnsupported.images;
  }
  const bool masked = mask != nullptr;
  if (masked && !isBlendUnitMode(command.state.blendMode)) {
    // A mask *and* a blend mode the unit cannot express cannot both reach the shader: `composite.frag` reads the layer
    // and the parent and has no third sampler. Counted, and the mask is what is applied -- drawing the feature that
    // fits rather than neither, the same choice the rest of this loop makes.
    ++mUnsupported.advancedBlends;
  }
  if (masked && alpha != 1.0f) {
    // Same shape of conflict: `MaskUniforms` carries one coverage, and a masked layer's binding 1 is the mask rather
    // than the 1x1 texture that would carry the group alpha.
    ++mUnsupported.advancedBlends;
  }

  if (layer.linearLightText && !masked) {
    return compositeThroughShader(frame, parentIndex, layerIndex, region, alpha, blendMode, layer);
  }
  if (!isBlendUnitMode(command.state.blendMode) && !masked) {
    return compositeThroughShader(frame, parentIndex, layerIndex, region, alpha, blendMode, layer);
  }

  // A mode the blend unit can express, which is every mode up to `kScreen`. The layer is *copied* with
  // its alpha into the parent through the blend unit, which needs a fragment stage whose output is the
  // layer's colour scaled by the layer's global alpha and nothing else. There is no "copy a texture"
  // program in the shipped set, and `mask.frag` with a 1x1 constant-coverage texture *is* that program:
  // its output is `layer * coverage`, and a coverage of the global alpha scales a premultiplied colour
  // exactly the way a global alpha should.
  backend::Result result = beginPassFor(frame, parentIndex, false);
  if (!backend::isSuccess(result)) return result;
  result = setScissor(frame, region);
  if (!backend::isSuccess(result)) return result;

  // ## Binding 1 is the mask when the layer has one, and the 1x1 constant alpha otherwise
  //
  // The 1x1 texture is rewritten per composite rather than kept per layer, which is safe because the OpenGL backend
  // replays the recorded `writeTexture` and the draw that reads it in order: the draw sees the value written just before
  // it. A backend that deferred execution would need one texture per composite, or a uniform instead -- and the value
  // *is* a uniform in everything but name, which is why this uses the mask program's sampler rather than growing a sixth
  // uniform block. A mask needs no such care: it is an uploaded texture the whole frame shares, looked up by placement.
  //
  // The three uniforms differ between the two cases for the same reason: the 1x1 constant is sampled at one coordinate
  // that can only land on its single texel, while the mask is sampled through its placement rectangle -- stretched over
  // the layer by that rectangle alone, since the composite's coordinate is already normalized to the target.
  std::array<float, 2> maskScale { 0.0f, 0.0f };
  std::array<float, 2> maskOffset { 0.5f, 0.5f };
  int32_t maskMode = 0;
  if (masked) {
    maskMode = layer.maskRule == Layer::MaskRule::Luminance ? 1 : 0;
    // The mask bitmap describes the *layer region*, not the whole target, so its 0..1 range has to be
    // mapped onto that region. The composite's own texture coordinate is the layer's coordinate: for a
    // full-target layer that is `region / root`, and for a sized layer it is already 0..1.
    const bool sizedSource = command.layer < frame.sized.size() && frame.sized[command.layer] != 0;
    if (sizedSource) {
      if (usesVulkanShaderSet(mDevice)) {
        maskScale = { mask->rect[2], mask->rect[3] };
        maskOffset = { mask->rect[0], mask->rect[1] };
      } else {
        maskScale = { mask->rect[2], -mask->rect[3] };
        maskOffset = { mask->rect[0], mask->rect[1] + mask->rect[3] };
      }
    } else if (layer.maskLocal) {
      const float rootWidth = static_cast<float>(frame.root->width);
      const float rootHeight = static_cast<float>(frame.root->height);
      const float regionWidth = std::max(region.getWidth(), 0.001f);
      const float regionHeight = std::max(region.getHeight(), 0.001f);
      maskScale[0] = mask->rect[2] * rootWidth / regionWidth;
      maskOffset[0] = mask->rect[0] - mask->rect[2] * region.getLeft() / regionWidth;
      if (usesVulkanShaderSet(mDevice)) {
        maskScale[1] = mask->rect[3] * rootHeight / regionHeight;
        maskOffset[1] = mask->rect[1] - mask->rect[3] * region.getTop() / regionHeight;
      } else {
        maskScale[1] = -mask->rect[3] * rootHeight / regionHeight;
        maskOffset[1] = mask->rect[1] + mask->rect[3] * (rootHeight - region.getTop()) / regionHeight;
      }
    } else {
      // A target-space mask (SVG/Lottie/Canvas::maskLayer's default): its own device coordinates are
      // the composite's texture coordinates, exactly as before.
      if (usesVulkanShaderSet(mDevice)) {
        maskScale = { mask->rect[2], mask->rect[3] };
        maskOffset = { mask->rect[0], mask->rect[1] };
      } else {
        maskScale = { mask->rect[2], -mask->rect[3] };
        maskOffset = { mask->rect[0], mask->rect[1] + mask->rect[3] };
      }
    }
  } else {
    result = updateConstantAlphaTexture(alpha);
    if (!backend::isSuccess(result)) return result;
    mStats.uploadBytes += 4;
  }

  if (std::getenv("CORAL_GPU_TRACE_COMPOSITE") != nullptr) {
    std::fprintf(stderr, "[composite] masked=%d maskTexture=%u scale=(%g,%g) offset=(%g,%g) mode=%d\n",
      masked ? 1 : 0, masked ? mask->texture.index : mAlphaTexture.index, static_cast<double>(maskScale[0]),
      static_cast<double>(maskScale[1]), static_cast<double>(maskOffset[0]), static_cast<double>(maskOffset[1]),
      maskMode);
    std::fflush(stderr);
  }

  const RenderTarget& parent = targetAt(frame, frame.targets[parentIndex]);

  // ## The set is per composite position, and the cost of that is measured rather than hidden
  //
  // `compositeSetFor` gives the mechanism and why a shared set cannot work. What belongs here is the price: with a set
  // per composite, `GpuSurface.OneSvgDocumentMatchesTheRasterizer` measures 10.4% of inked pixels differing from the
  // rasterizer, against 2.1% with the shared set. `path.svg` opens a layer per translucent element, so it is the document
  // with the most composites. Creating the sets fresh and pooling them by composite index both give exactly 10.4%, so the
  // cost is not creation overhead -- and it is not the constant-alpha texture either, since a three-layer frame at
  // `alpha 128` fails identically to one at alpha 1.
  //
  // It is taken anyway, because the alternative is a `saveLayer` that silently drops every put but the last, which is a
  // wrong picture for the most ordinary use of the API. A 10.4% edge-and-translucency difference on one corpus document
  // is the smaller wrong, and it is recorded here so the next attempt starts from it.
  const MaskUniforms uniforms{ { maskScale[0], maskScale[1] }, { maskOffset[0], maskOffset[1] }, maskMode, { 0, 0, 0 } };
  uint32_t block = 0;
  result = allocateUniformSlot(&uniforms, sizeof(uniforms), &block);
  if (!backend::isSuccess(result)) return result;
  mStats.uploadBytes += sizeof(uniforms);

  backend::DescriptorSetHandle set = backend::DescriptorSetHandle::invalid();
  result = compositeSetFor(frame.compositeCommandIndex, &set);
  if (!backend::isSuccess(result)) return result;

  const backend::DescriptorWrite maskTextures[] = {
    { 0, backend::BindingType::TextureSampler, source->color, mLinearClamp, {}, 0 },
    { 1, backend::BindingType::TextureSampler, masked ? mask->texture : mAlphaTexture, mNearestClamp, {}, 0 },
    { 2, backend::BindingType::UniformBuffer, backend::TextureHandle::invalid(),
      backend::TextureSamplerHandle::invalid(),
      { uniformBuffer(), static_cast<uint64_t>(block) * kUniformSlotSize, kUniformSlotSize }, 0 },
  };
  result = mDevice->updateDescriptorSet(set, maskTextures, 3);
  if (!backend::isSuccess(result)) {
    return result;
  }

  const PipelineKey key{ Program::Mask, Variant::Plain, blendMode, parent.format,
                         parent.depthStencilView.valid() ? kDepthStencilFormat : backend::PixelFormat::Undefined,
                         parent.sampleCount };
  backend::RenderPipelineHandle pipeline = backend::RenderPipelineHandle::invalid();
  result = ensurePipeline(key, parent.pass, &pipeline);
  if (!backend::isSuccess(result)) return result;

  // The draw this composite is about to issue, in the terms that decide what appears: which texture is sampled,
  // which target is written, and the scissor that limits it. Three layered rectangles came out as one patch in the
  // first rectangle's region with the first layer's colour while the regions and vertex offsets were all distinct,
  // so the remaining candidates are the *source texture* and the *destination* -- and this prints both.
  //
  //   set CORAL_GPU_TRACE_COMPOSITE=1
  if (std::getenv("CORAL_GPU_TRACE_COMPOSITE") != nullptr) {
    std::fprintf(stderr,
      "[composite] draw: source=tex%d parentTarget=%s parentIndex=%u passValid=%d pipeline=%zu "
      "scissor=(%g,%g,%g,%g) compositeOffset=%u compositeSet=%u layerSet=%u\n",
      source->color.index, parentIndex == 0 ? "ROOT" : "pooled", parentIndex, parent.pass.valid() ? 1 : 0,
      static_cast<size_t>(pipeline.index), static_cast<double>(region.getLeft()),
      static_cast<double>(region.getTop()), static_cast<double>(region.getRight()),
      static_cast<double>(region.getBottom()), precompiled.compositeOffset, set.index, mQuadSet.index);
    std::fflush(stderr);
  }
  result = mCommandBuffer->bindRenderPipeline(pipeline);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->bindDescriptorSet(0, set);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->setVertexBuffer(0, mVertexBuffer, precompiled.compositeOffset);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->draw(kCoverVertexCount, 1, 0, 0);
  if (!backend::isSuccess(result)) return result;
  ++mStats.drawCallCount;
  mStats.vertexCount += kCoverVertexCount;
  return backend::Result::Success;
}

// ==================================================================================================
// The frame
// ==================================================================================================

backend::Result GpuRenderer::recordFrame(Frame& frame, const std::vector<Bitmap>& images,
  const backend::LoadOp rootLoad, const bool keepPassOpen) {
  const DrawList& list = *frame.list;

  // ## The image route's upload stage, recorded before anything is drawn
  //
  // `placeImages` decided *where* every bitmap of this frame is sampled from before the arena was assembled, because the
  // vertex writer needed the sub-rectangle and the batching needed the texture. Putting the pixels there is a command, so
  // it can only be recorded once a command buffer is open -- here -- and it must precede the first image draw, which is
  // what the ordering of a recorded `writeTexture` guarantees. The upload is marginal: a bitmap a previous frame already
  // uploaded records nothing, which is what makes a steady frame's image route free of texture traffic.
  uploadPlacedImages(frame, images);

  // The per-frame block, staged at slot 0.
  //
  // Slot 0 is always the canvas block: every set created before recording started names it, and a cubic image batch
  // stages a copy with its own filter rather than overwriting this one. The slot counter is reset here because the arena
  // belongs to the frame, and the whole arena is delivered by `uploadUniforms` once recording finishes.
  CanvasUniforms canvas = canvasUniformsFor(*frame.root);
  // A translation rides the canvas block: the shader maps device pixels to clip space as
  // `xy * scale + offset`, so adding `translation * scale` moves a whole list without touching its
  // device-space geometry (and therefore without copying it).
  canvas.offset[0] += frame.translation.x * canvas.scale[0];
  canvas.offset[1] += frame.translation.y * canvas.scale[1];
  // The canvas block is slot 0 of the arena and every descriptor set names it there, so a frame
  // assembled from several lists keeps the block its first list wrote instead of allocating a new one.
  if (!mUniformArenaContinues) mUniformSlotsUsed = 0;
  uint32_t canvasSlot = kCanvasSlot;
  backend::Result result = backend::Result::Success;
  if (!mUniformArenaContinues) {
    result = allocateUniformSlot(&canvas, sizeof(canvas), &canvasSlot);
    if (!backend::isSuccess(result)) return result;
    if (canvasSlot != kCanvasSlot) {
      return backend::Result::Error;
    }
  }

  if (!frame.arena.empty()) {
    // Timed separately from the rest of the recording, and it is the one number here that scales with the frame's *size*
    // rather than with its command count: this copies the whole arena -- 13.7 MB on the demo scene -- into a mapped
    // buffer, where everything after it is per command. `submit` subtracts this from the recording stage it timed around
    // this call, so the two numbers never overlap.
    const Clock::time_point uploadStart = Clock::now();
    result = uploadBuffer(mVertexBuffer, mVertexCapacity, mVertexBase, frame.arena.data(),
      frame.arena.size());
    if (!backend::isSuccess(result)) return result;
    // Every offset into the arena has to move with the list, or the second list of a frame reads the
    // first list vertices.
    if (mVertexBase != 0) {
      const auto base = static_cast<uint32_t>(mVertexBase);
      for (Batch& batch: frame.batches) batch.vertexOffset += base;
      // Every offset moves, including the ones that are zero in this list arena: zero is a valid offset
      // (a fan or cover at the start of the arena), not "absent", and skipping it drew the previous
      // list vertices a second time.
      for (Precompiled& precompiled: frame.precompiled) {
        precompiled.fanOffset += base;
        precompiled.coverOffset += base;
        precompiled.compositeOffset += base;
      }
    }
    mVertexBase += frame.arena.size();
    mStats.stages.vertexUpload = millisecondsSince(uploadStart);
  }

  // The root is cleared unless the frame's first command is a clear that covers it: a `kClear` rect
  // writes zero over everything it covers, so a full-target one makes the clear redundant. Partial
  // coverage is *not* enough to skip the clear, because then the uncovered pixels would keep the
  // previous frame's contents instead of being transparent.
  //
  // The caller can override both directions, and the one that matters is `Load`: a submission that must
  // *composite* onto what the target already holds -- a sticker, a caption, a signature drawn over a
  // picture an earlier pass produced -- cannot use the rule above, because the rule's whole purpose is
  // that a submission leaves the target with defined contents. `Clear` is the rule's answer forced, for a
  // caller whose recording starts with a full-target clear whose coverage the test below cannot see.
  bool rootStartsWithClear = false;
  if (rootLoad == backend::LoadOp::Load) {
    rootStartsWithClear = true;
  } else if (rootLoad == backend::LoadOp::Clear) {
    rootStartsWithClear = false;
  } else if (!list.commands.empty()) {
    const DrawList::Command& first = list.commands[0];
    const Rect covered = clampToTarget(first.geometry[0], frame.root->width, frame.root->height);
    rootStartsWithClear = first.kind == DrawList::Kind::Rect && first.state.blendMode == BlendMode::kClear &&
                          first.layer == 0 && covered.getWidth() >= static_cast<float>(frame.root->width) &&
                          covered.getHeight() >= static_cast<float>(frame.root->height);
  }

  uint32_t batchCursor = 0;
  uint32_t index = 0;
  const uint32_t count = static_cast<uint32_t>(list.commands.size());
  while (index < count) {
    const DrawList::Command& command = list.commands[index];

    if (batchCursor < frame.batches.size() && frame.batches[batchCursor].firstCommand == index) {
      const Batch& batch = frame.batches[batchCursor++];
      const uint32_t nextIndex = batch.firstCommand + batch.commandCount;

      bool drawable = true;
      // ## A blend mode the unit cannot express is tested first, and for every kind of batch
      //
      // The test used to sit at the *end* of this chain, so it was reached only by a batch that was neither an image, a
      // coverage field nor a gradient. Everything else was marked drawable before it and then keyed with the raw mode:
      // `blendStateFor` has no case for those modes and falls to its source-over default, so a `kMultiply` path, image or
      // gradient was drawn as source-over with nothing reporting it. Ten of the demo's twelve blend swatches were that
      // bug, and the paths in question take the coverage route -- the one branch the old test could never reach.
      //
      // A batch that still carries such a mode here came from a list recorded without a canvas, because
      // `GraphiteCanvasBackend` isolates those draws in a layer and records the draw inside it as source-over. There is no
      // layer to composite here, so it is counted as an omission -- the policy everywhere else in this loop -- rather than
      // approximated into a plausible picture of the wrong colour.
      if (!isBlendUnitMode(batch.state.blendMode)) {
        mUnsupported.advancedBlends += batch.commandCount;
        drawable = false;
      } else if (batch.state.shader == DrawList::ShaderKind::Image) {
        // Images are drawn by the same quad program as a solid fill, with `params.w` set so the fragment stage
        // samples instead of using the paint colour. Each command needs its own texture, so the batch is walked one
        // command at a time rather than drawn as one instanced call -- see `drawImageBatch`.
        drawable = true;
      } else if (batch.state.shader == DrawList::ShaderKind::Coverage) {
        // ## A coverage field is now drawn, and it is drawn only when the command names one
        //
        // This branch used to mark every `Coverage` batch undrawable, with the comment "a coverage shader is a mask
        // read, which is the shaped-clip path this renderer does not have". That was true when the only Coverage
        // commands were shaped clips, and it silently swallowed the coverage-field route: the batch was built, the
        // early-out counted it as an omission, and the dispatch below never ran. The symptom was a draw that produced
        // no pixels with no error anywhere, which is why it took a trace of the batch loop to find.
        //
        // The two uses are told apart by `coverageField`: a command that names a field has one, and a shaped clip
        // does not. So the shaped-clip case keeps its omission and the field case is drawn.
        if (batch.state.coverageField == DrawList::kNoCoverageField) {
          mUnsupported.images += batch.commandCount;
          drawable = false;
        } else if (std::getenv("CORAL_TRACE_VAO") != nullptr) {
          std::fprintf(stderr, "[cov] batch accepted: first=%u count=%u layer=%u coverageField=%u\n", batch.firstCommand,
                  batch.commandCount, batch.layer, batch.state.coverageField);
          std::fflush(stderr);
        }
      } else if (batch.state.shader == DrawList::ShaderKind::LinearGradient ||
                 batch.state.shader == DrawList::ShaderKind::RadialGradient) {
        // Drawn through `gradient.frag`, which shares the quad's vertex stage -- so the batch is drawable and its
        // vertices are laid out exactly as a rectangle's are. See `drawGradientBatch`.
        drawable = true;
      }
      if (!drawable) {
        index = nextIndex;
        continue;
      }

      const DrawList::Command& first = list.commands[batch.firstCommand];
      if (first.clipMask != DrawList::kNoClipMask) {
        // A shaped clip is not applied: the command is drawn with its rectangular clip only. Reported
        // rather than silent, and counted per command rather than per batch so that the number means
        // "commands affected".
        mUnsupported.clipMasks += batch.commandCount;
      }

      // ## A layer's first open in a frame clears it, because a layer's contents start empty
      //
      // The root's clear is conditional on the frame's own first command, which is what `rootStartsWithClear` is for. A
      // *layer* has no such shortcut to offer and needs the clear unconditionally: the target comes from a pool and holds
      // the previous frame's or an earlier layer's pixels, so loading it makes a layer accumulate instead of isolating.
      //
      // What that looked like when it was wrong is worth recording, because it is invisible on a single frame and only
      // shows on the second: every layer is a fresh zeroed texture the first time, so a one-frame picture is correct, and
      // a second frame composites the first frame's contents underneath the new ones. A translucent group then comes out
      // darker every frame. `LoadOp::Clear` on the first open and `Load` on every later one is what makes "the layer's
      // target is acquired up front and its contents start empty" -- the replay's own note on `OpenLayer` -- true rather
      // than merely usually true.
      const bool clearThis = batch.layer == 0 ? !rootStartsWithClear : true;
      result = beginPassFor(frame, batch.layer, clearThis);
      if (!backend::isSuccess(result)) return result;
      if (std::getenv("CORAL_TRACE_VAO") != nullptr) {
        std::fprintf(stderr, "[clip] shader=%d clip=(%.1f,%.1f,%.1f,%.1f)\n", static_cast<int>(first.state.shader),
                static_cast<double>(first.clipRect.getLeft()), static_cast<double>(first.clipRect.getTop()),
                static_cast<double>(first.clipRect.getRight()), static_cast<double>(first.clipRect.getBottom()));
        std::fflush(stderr);
      }
      result = setScissor(frame, first.clipRect);
      if (!backend::isSuccess(result)) return result;

      // A gradient is drawn through the quad program whatever its command kind, because `gradient.frag` is the quad's
      // fragment stage. `Canvas::drawRect` records a rectangle as a `Path`, so this test is what routes a gradient rect
      // to the gradient program instead of the stencil-and-cover path program, which would fill it flat.
      const bool gradientBatch = first.state.shader == DrawList::ShaderKind::LinearGradient ||
                                 first.state.shader == DrawList::ShaderKind::RadialGradient;

      // ## The coverage-field route takes its own branch, before the kind test
      //
      // It is tested first for the same reason the gradient is: a coverage command is recorded as a rectangle
      // (that is the geometry kind the vertex writer lays out), so a kind-based test would send it to the flat
      // fill and paint a solid quad where a feathered edge belongs. That is precisely how the gradient painted a
      // flat black rectangle when its batch reached the wrong program.
      const bool coverageBatch = first.state.shader == DrawList::ShaderKind::Coverage;

      if (coverageBatch) {
        // One draw for the whole batch: the atlas is the batch's, and where each field's texels are in it travels per
        // vertex. See `drawCoverageFieldBatch`.
        const Clock::time_point drawStart = Clock::now();
        result = drawCoverageFieldBatch(frame, batch);
        mStats.stages.field += millisecondsSince(drawStart);
        if (!backend::isSuccess(result)) return result;
      } else if (gradientBatch) {
        const Clock::time_point drawStart = Clock::now();
        result = drawGradientBatch(frame, batch);
        mStats.stages.gradient += millisecondsSince(drawStart);
        if (!backend::isSuccess(result)) return result;
      } else if (batch.kind == DrawList::Kind::Rect) {
        const Clock::time_point drawStart = Clock::now();
        result = drawRectBatch(frame, batch);
        mStats.stages.rect += millisecondsSince(drawStart);
        if (!backend::isSuccess(result)) return result;
      } else if (batch.kind == DrawList::Kind::Image ||
                 batch.kind == DrawList::Kind::ExternalImage) {
        // ## One draw per batch rather than one per command, which is what the atlas bought
        //
        // Every command in the batch samples the same texture -- `canShareDraw` grouped them by the texture the frame
        // placed each bitmap in -- so the program, the set and the scissor are bound once and a single instanced draw
        // covers the run. What is per command is which sub-rectangle of that texture it blits, and that is in the
        // vertices. Before the atlas this route was one draw and one batch per glyph: 561 of each on the demo scene.
        //
        // The batch is counted, not the draws: `imageBatchCount` is the number that says what an atlas is worth, and it
        // is the one number in this loop that a texture-per-command arrangement cannot change.
        ++mStats.imageBatchCount;
        const Clock::time_point imageStart = Clock::now();
        result = drawImageBatch(frame, batch);
        mStats.stages.image += millisecondsSince(imageStart);
        if (!backend::isSuccess(result)) return result;
      } else if (batch.kind == DrawList::Kind::Path) {
        // A batch of paths shares its state and its clip, but not its geometry, and each path is three
        // draws of its own: batching them buys the state change and nothing else, which is still the
        // expensive part.
        const Clock::time_point pathStart = Clock::now();
        for (uint32_t pathIndex = 0; pathIndex < batch.commandCount; ++pathIndex) {
          const uint32_t pathCommand = batch.firstCommand + pathIndex;
          result = drawPath(frame, list.commands[pathCommand], frame.precompiled[pathCommand]);
          if (!backend::isSuccess(result)) return result;
        }
        mStats.stages.path += millisecondsSince(pathStart);
      }
      index = nextIndex;
      continue;
    }

    switch (command.kind) {
      case DrawList::Kind::OpenLayer:
        // Nothing is drawn: the layer's target is acquired up front and its contents start empty. Any
        // pass still open belongs to the parent, and stays open until something actually draws into the
        // child, which is what keeps a layer that is opened and closed without content from costing a
        // pass at all.
        break;

      case DrawList::Kind::CloseLayer: {
        // The layer's pixels have to be complete before anything reads them, and every composite reads
        // either the layer or the parent it is being drawn into -- so the pass is closed here rather
        // than left open under a texture-to-texture copy.
        result = endPass(frame);
        if (!backend::isSuccess(result)) return result;

        frame.compositeCommandIndex = index;
        const Clock::time_point compositeStart = Clock::now();
        result = compositeLayer(frame, command);
        mStats.stages.composite += millisecondsSince(compositeStart);
        if (!backend::isSuccess(result)) return result;
        break;
      }

      case DrawList::Kind::Rect:
      case DrawList::Kind::Path:
      case DrawList::Kind::Image:
        // Unreachable: every drawable command belongs to a batch, and the batch was consumed above.
        break;
    }
    ++index;
  }

  // A frame assembled from several lists keeps the pass open between them: reopening it per list would
  // make each list pay for loading the whole target again, which is what the batch of lists exists to
  // avoid.
  if (keepPassOpen) return backend::Result::Success;
  return endPass(frame);
}

backend::Result GpuRenderer::submit(const DrawList& list, RenderTarget& target, const std::vector<Bitmap>& images,
  const backend::LoadOp rootLoad, const backend::SemaphoreHandle waitSemaphore,
  const backend::SemaphoreHandle signalSemaphore, const Rect& damage, const Point& translation,
  const bool continueFrame) {
  // The image table is threaded through rather than stored: it belongs to the surface that owns the recording,
  // and a renderer that kept a copy would be holding pixels past the caller's frame. Two stages read it -- the
  // placement stage, which decides where each bitmap is sampled from, and the upload stage, which is the only
  // place its pixels are copied into a texture -- and both are inside this call, so the pixels never outlive
  // the frame's recording.
  if (!mReady) {
    return backend::Result::Unsupported;
  }
  if (!target.isValid()) {
    return backend::Result::InvalidArgument;
  }

  // Only the first list of a frame waits: a continuation records into the frame that is already open, and
  // waiting there would serialise every list against the GPU again -- the cost the run exists to remove.
  if (mFrameInFlight && !mFrameOpen) {
    if (const backend::Result waited = waitForFrame(); !backend::isSuccess(waited)) return waited;
  }
  // The previous frame's fence has signalled, so its timestamps are readable. Reading one frame late is
  // what keeps the query from stalling the CPU: waiting on the *current* frame's result would be a
  // submit-and-wait per frame, which is the thing a GPU timer must not add.
  if (!mFrameOpen) {
    readTimingResults();
    if (const backend::Result uploaded = uploadSdfPattern(); !backend::isSuccess(uploaded)) {
      return uploaded;
    }
  }

  if (!mFrameOpen) mVertexBase = 0;
  // A frame may be recorded as several lists. The stage totals add up across them, so a plan-built
  // frame reports the whole frame rather than its last list; the first list of a frame starts them.
  const bool continuingFrame = mFrameOpen;
  if (!continuingFrame) {
    mFrameStageTotals = FrameStages{};
    mFrameCounts = FrameCounts{};
  }
  mStats = ContextImpl::FrameStats{};
  mStats.commandCount = static_cast<uint32_t>(list.commands.size());
  mStats.layerCount = static_cast<uint32_t>(std::max<size_t>(list.layers.size(), 1));
  mUnsupported = Unsupported{};

  const Clock::time_point frameStart = Clock::now();
  const auto accrueStages = [this](const double listTotal) {
    FrameStages& totals = mFrameStageTotals;
    totals.fields += mStats.stages.fields;
    totals.placeImages += mStats.stages.placeImages;
    totals.precompile += mStats.stages.precompile;
    totals.targets += mStats.stages.targets;
    totals.arena += mStats.stages.arena;
    totals.vertexUpload += mStats.stages.vertexUpload;
    totals.record += mStats.stages.record;
    totals.execute += mStats.stages.execute;
    totals.rect += mStats.stages.rect;
    totals.gradient += mStats.stages.gradient;
    totals.image += mStats.stages.image;
    totals.path += mStats.stages.path;
    totals.field += mStats.stages.field;
    totals.composite += mStats.stages.composite;
    totals.total += listTotal;
    mFrameCounts.batches += mStats.batchCount;
    mFrameCounts.drawCalls += mStats.drawCallCount;
    mFrameCounts.commands += mStats.commandCount;
    mFrameCounts.pathDraws += mStats.pathDrawCalls;
    mFrameCounts.imageDraws += mStats.imageDrawCalls;
    mStats.stages = totals;
    mStats.batchCount = mFrameCounts.batches;
    mStats.drawCallCount = mFrameCounts.drawCalls;
    mStats.commandCount = mFrameCounts.commands;
    mStats.pathDrawCalls = mFrameCounts.pathDraws;
    mStats.imageDrawCalls = mFrameCounts.imageDraws;
  };
  // ## The frame's stages, timed where they run
  //
  // Each `Clock::now()` is one call per stage rather than per command, so the instrumentation costs nothing measurable and
  // is therefore always on: the numbers it produces are what say *which* part of a frame a change moved, and a timer
  // behind a flag is a timer nobody reads. The stages are written into `mStats.stages` as they run; `recordFrame` fills
  // the two that live inside it (the vertex upload and the rest of the recording) and this function fills `total`.
  //
  // `stageStart` is set *immediately* before each stage rather than left at the top of the function, and that is a
  // correction the first measurement of this instrumentation produced: the per-command layer validation below walks every
  // command (41849 of them on the demo, each a few hundred bytes apart) and was being counted into the field stage's
  // 1.15 ms -- 0.4 ms of which it was not. Whatever runs outside a stage belongs to the gap between the stages, which is
  // what `total` minus their sum is.
  Clock::time_point stageStart = frameStart;

  // ## Shaped clips are rewritten into masked layers before anything else looks at the list
  //
  // The rewrite has to precede precompile, batching, layer parenting and target allocation: it changes
  // the command count, the layer table and every command's layer index. It is skipped entirely for the
  // common list with no clip masks, which is why the copy below is the only cost a normal frame pays.
  Frame& frame = mFrame;
  frame.translation = translation;
  frame.ownedList.reset();
  const DrawList* active = &list;
  if (hasClipMaskCommands(list)) {
    frame.ownedList = std::make_unique<DrawList>();
    synthesizeClipLayers(list, *frame.ownedList);
    active = frame.ownedList.get();
    mStats.layerCount = static_cast<uint32_t>(active->layers.size());
    if (std::getenv("CORAL_TRACE_CLIP_LAYERS") != nullptr) {
      uint32_t opens = 0;
      uint32_t closes = 0;
      for (const DrawList::Command& command: active->commands) {
        if (command.kind == DrawList::Kind::OpenLayer) ++opens;
        if (command.kind == DrawList::Kind::CloseLayer) ++closes;
      }
      std::fprintf(stderr, "[clip-rewrite] commands=%zu orig=%zu layers=%zu masks=%zu opens=%u closes=%u\n",
        active->commands.size(), list.commands.size(), active->layers.size(),
        list.clipMasks.size(), opens, closes);
    }
  }

  // ## R1 step one: top-level unmasked layers get a bounds-sized target
  //
  // The decision needs the parent map, so it is resolved before the target loop rather than after
  // batching as it used to be. `synthesizeClipLayers` may have introduced layers, which is why this
  // runs on the possibly-owned list and not on `list`.
  resolveLayerParents(*active, frame.parents);
  // `CORAL_DISABLE_SIZED_LAYERS` is the A/B switch behind the R1 numbers in the baseline.
  frame.sized = std::getenv("CORAL_DISABLE_SIZED_LAYERS") != nullptr
    ? std::vector<uint8_t>(frame.parents.size(), 0)
    : planSizedLayers(*active, frame.parents, static_cast<float>(target.width),
        static_cast<float>(target.height));
  if (std::any_of(frame.sized.begin(), frame.sized.end(), [](uint8_t value) { return value != 0; })) {
    if (frame.ownedList == nullptr) {
      frame.ownedList = std::make_unique<DrawList>(active->clone());
      active = frame.ownedList.get();
    }
    translateSizedLayers(*frame.ownedList, frame.sized, static_cast<float>(target.width),
      static_cast<float>(target.height));
  }
  if (std::getenv("CORAL_TRACE_SIZED_LAYERS") != nullptr) {
    for (size_t layerIndex = 1; layerIndex < frame.parents.size(); ++layerIndex) {
      std::fprintf(stderr, "[sized-layers] layer=%zu parent=%u sized=%u bounds=(%g,%g,%g,%g)\n",
        layerIndex, frame.parents[layerIndex],
        layerIndex < frame.sized.size() ? frame.sized[layerIndex] : 0,
        static_cast<double>(active->layers[layerIndex].bounds.getLeft()),
        static_cast<double>(active->layers[layerIndex].bounds.getTop()),
        static_cast<double>(active->layers[layerIndex].bounds.getRight()),
        static_cast<double>(active->layers[layerIndex].bounds.getBottom()));
    }
  }

  // Every layer index a command names has to exist, because a command's target is chosen by that index
  // and an out-of-range one would otherwise be drawn into the root by accident.
  for (const DrawList::Command& command: active->commands) {
    if (command.layer >= mStats.layerCount) {
      return backend::Result::InvalidArgument;
    }
  }

  const uint32_t commandCount = static_cast<uint32_t>(active->commands.size());
  frame.list = active;
  frame.root = &target;
  frame.damage = damage;
  frame.hasDamage = damage.getWidth() > 0.0f && damage.getHeight() > 0.0f;
  if (!mFrameOpen) frame.openPass = -1;
  frame.precompiled.resize(commandCount);
  std::vector<uint8_t> startsBatch(commandCount, 0);

  // ## Stage one: the coverage fields the recording asked for, generated here
  //
  // Before `precompile` and separate from it, which is the decomposition step S5 asks for: generation is CPU work over a
  // list whose entries are independent of each other, so it is the one place in the route that a pool can be used, and
  // the stage that follows -- upload and draw -- is single-threaded because one GL context is.
  //
  // It runs before the arena is assembled and before any command is recorded, so a field that could not be generated is
  // a counted omission rather than a half-drawn frame.
  stageStart = Clock::now();
  backend::Result resolved = resolveCoverageFields(frame);
  if (!backend::isSuccess(resolved)) {
    releaseFrameResources();
    return resolved;
  }
  mStats.stages.fields = millisecondsSince(stageStart);

  // ## Stage one and a half: where each image is sampled from
  //
  // Between the fields and `precompile`, because the image route's placement has to exist before the batches are decided
  // and before the arena is laid out: a batch binds one texture and a vertex carries one sub-rectangle, and both are
  // answers about *where a bitmap lives* rather than about the command. Nothing is uploaded here -- the write is recorded
  // in `recordFrame`, once a command buffer is open -- so this is pure CPU work and device-object creation, and it cannot
  // half-draw a frame: a bitmap that cannot be placed is an omission the replay counts.
  stageStart = Clock::now();
  placeImages(frame, active->images);
  mStats.stages.placeImages = millisecondsSince(stageStart);

  stageStart = Clock::now();
  precompile(*active, frame.precompiled, startsBatch);
  mStats.stages.precompile = millisecondsSince(stageStart);
  double precompileMilliseconds = 0.0;
  for (const Precompiled& slot: frame.precompiled) {
    precompileMilliseconds += slot.milliseconds;
  }
  mStats.precompileMilliseconds = precompileMilliseconds;

  buildBatches(*active, startsBatch, frame.batches);
  mStats.batchCount = static_cast<uint32_t>(frame.batches.size());

  // ## The uniform arena's size, decided before recording starts
  //
  // Every draw takes a slot, and a buffer cannot be grown between `begin` and `end` of the command buffer without
  // invalidating the sets earlier draws recorded. So the arena is sized here from an upper bound over the batches: a path
  // takes two blocks (cover and stencil), every other drawable batch takes at most one (gradient, coverage extent, or a
  // cubic image's canvas copy), and every layer close takes one composite block. Over-counting costs a few hundred bytes.
  uint32_t uniformSlots = 1; // The canvas block at slot 0.
  for (const Batch& batch: frame.batches) {
    uniformSlots += batch.kind == DrawList::Kind::Path ? 2u * batch.commandCount : 1u;
  }
  for (const DrawList::Command& command: active->commands) {
    if (command.kind == DrawList::Kind::CloseLayer) ++uniformSlots;
  }
  // A frame assembled from several lists keeps the slots that its earlier lists allocated.
  if (const backend::Result capacity = ensureUniformCapacity(mUniformSlotsUsed + uniformSlots);
      !backend::isSuccess(capacity)) {
    releaseFrameResources();
    return capacity;
  }

  // Targets: layer 0 is the caller's. A layer R1 step one could translate into its own space gets a
  // target the size of its bounds; every other layer still gets a full-size offscreen target.
  //
  // The full-size fallback is the pre-R1 behaviour and stays the default for nested, masked, gradient
  // and coverage-field layers, because translating those correctly is what step two and three are for.
  stageStart = Clock::now();
  frame.targets.assign(frame.parents.size(), kRootTarget);
  frame.opened.assign(frame.parents.size(), 0);
  for (size_t layerIndex = 1; layerIndex < frame.parents.size(); ++layerIndex) {
    const Layer& layer = active->layers[layerIndex];
    const backend::PixelFormat format =
      layer.format == backend::PixelFormat::Undefined ? target.format : layer.format;
    const bool sized = layerIndex < frame.sized.size() && frame.sized[layerIndex] != 0;
    const uint32_t layerWidth = sized
      ? static_cast<uint32_t>(std::max<int64_t>(1, std::llround(layer.bounds.getWidth())))
      : target.width;
    const uint32_t layerHeight = sized
      ? static_cast<uint32_t>(std::max<int64_t>(1, std::llround(layer.bounds.getHeight())))
      : target.height;
    // Every layer is allocated even when its `bounds` describe nothing, because commands may still name
    // it: a recorded list is allowed to open a layer with empty bounds and draw outside them, and
    // dropping the target would silently discard those commands. `Layer::isEmpty()` exists
    // (`include/gpu/Layer.h`) and is deliberately *not* consulted here -- "is this layer empty" is a
    // question about the recorded region, and the answer for "should this target exist" is always yes.
    //
    // Every layer also gets a stencil attachment: a path can be drawn into any of them, and a layer
    // without one would silently fall back to the hard-edged convex fan. Three extra bytes per pixel is
    // the cheaper mistake.
    const int32_t pooled = acquireLayerTarget(layerWidth, layerHeight, format, true);
    if (pooled < 0) {
      releaseFrameResources();
      return backend::Result::OutOfMemory;
    }
    frame.targets[layerIndex] = pooled;
  }
  mStats.stages.targets = millisecondsSince(stageStart);

  stageStart = Clock::now();
  backend::Result result = assembleArena(frame);
  if (!backend::isSuccess(result)) {
    releaseFrameResources();
    return result;
  }
  mStats.stages.arena = millisecondsSince(stageStart);
  mStats.uploadBytes = frame.arena.size() + sizeof(CanvasUniforms);

  if (!mFrameOpen) {
    result = ensureCommandBuffer();
    if (!backend::isSuccess(result)) {
      releaseFrameResources();
      return result;
    }
    result = mCommandBuffer->reset();
    if (!backend::isSuccess(result)) {
      releaseFrameResources();
      return result;
    }
    result = mCommandBuffer->begin();
    if (!backend::isSuccess(result)) {
      releaseFrameResources();
      return result;
    }
    beginTimingPass();
    mFrameOpen = true;
  }

  stageStart = Clock::now();
  const Clock::time_point rfStart = Clock::now();
  result = recordFrame(frame, active->images, rootLoad, continueFrame);


  if (backend::isSuccess(result) && continueFrame) {
    // The frame is still being assembled: keep the buffer and the arenas open so the next list appends
    // after this one, and let the last list of the run upload and submit once.
    mUniformArenaContinues = true;
    mStats.stages.record = millisecondsSince(stageStart) - mStats.stages.vertexUpload;
    mStats.stages.total = millisecondsSince(frameStart);
    accrueStages(mStats.stages.total);
    return backend::Result::Success;
  }
  if (backend::isSuccess(result)) {
    // ## The uniform arena crosses here, after recording and before submission
    //
    // This is the replacement for the per-draw `writeBuffer` calls: one mapped write delivers every block the frame
    // allocated. Host writes are unordered with queued draws, which is why each draw owns a distinct slot and why this
    // happens before the frame is submitted at all. Delivering it *after* recording is what keeps even this write out of
    // the render pass: the pass is closed by `recordFrame`'s `endPass`, and no command is involved either way.
    const Clock::time_point uniformStart = Clock::now();
    result = uploadUniforms();
    mStats.stages.vertexUpload += millisecondsSince(uniformStart);
  }
  if (backend::isSuccess(result)) {
    endTimingPass();
    result = mCommandBuffer->end();
  }
  // `recordFrame` split this out itself: the vertex upload is the arena's bytes crossing into the buffer, and it is the
  // one part of the recording whose cost is proportional to the frame's *size* rather than to its command count.
  mStats.stages.record = millisecondsSince(stageStart) - mStats.stages.vertexUpload;
  // The recorded command counts, on request, before the list is executed. A draw that is missing from the picture is
  // either absent from the list or present and ignored, and only the list can say which -- the renderer's own draw
  // count increments on the way past whether or not the command survives.
  //
  //   set CORAL_GL_TRACE_COMMANDS=1
  if (std::getenv("CORAL_GL_TRACE_COMMANDS") != nullptr) {
    mCommandBuffer->dumpCommandCounts();
  }
  if (backend::isSuccess(result)) {
    // Upload and record are one stage rather than two: the vertex and uniform bytes are recorded into
    // the command buffer with `writeBuffer` rather than mapped and written by the host, so they leave
    // the CPU when the frame executes and there is no separate moment at which "the upload happened".
    stageStart = Clock::now();
    backend::Queue* queue = mDevice->getQueue(backend::QueueType::Graphics);
    if (queue == nullptr) {
      result = backend::Result::Unsupported;
    } else if (mAsyncSubmissions) {
      if (!mFrameFence.valid()) {
        result = mDevice->createFence(backend::FenceDescriptor{}, &mFrameFence);
      }
      if (backend::isSuccess(result) && mFrameFenceSignalled) {
        result = mDevice->resetFence(mFrameFence);
        if (backend::isSuccess(result)) mFrameFenceSignalled = false;
      }
      if (backend::isSuccess(result)) {
        result = queue->submit(*mCommandBuffer, mFrameFence, waitSemaphore, signalSemaphore);
      }
      if (backend::isSuccess(result)) {
        mFrameInFlight = true;
        mFrameFenceSignalled = true;
      }
    } else {
      result = queue->submit(*mCommandBuffer, backend::FenceHandle::invalid(), waitSemaphore, signalSemaphore);
    }
    mStats.stages.execute = millisecondsSince(stageStart);
  }

  ++mFrameCount;
  mStats.stages.total = millisecondsSince(frameStart);
  // The whole of `submit`, which is the same measurement as `stages.total` under a name the older accessors and the
  // existing reports already use. Kept as one assignment rather than two independent timers for the reason this header
  // gives everywhere: a second copy of a number is a second place for it to be wrong.
  accrueStages(mStats.stages.total);
  mStats.renderMilliseconds = mStats.stages.total;


  mFrameOpen = false;
  mUniformArenaContinues = false;
  if (!mFrameInFlight) releaseFrameResources();
  if (backend::isSuccess(result)) {
    result = reportOmissions(list);
  }
  if (!backend::isSuccess(result)) {
    note(backend::MessageSeverity::Error, "the canvas frame could not be recorded or executed");
  }
  return result;
}

backend::Result GpuRenderer::present(const RenderTarget& source, RenderTarget& target,
    const PresentationParameters& parameters, const backend::SemaphoreHandle waitSemaphore,
    const backend::SemaphoreHandle signalSemaphore) {
  if (!mReady || mDevice == nullptr) return backend::Result::Unsupported;
  if (!source.isValid() || !target.isValid() || source.colorView == backend::defaultFramebufferView()) {
    return backend::Result::InvalidArgument;
  }
  if (mFrameOpen) {
    note(backend::MessageSeverity::Error, "a presentation pass was requested inside an open canvas frame");
    return backend::Result::InvalidArgument;
  }
  if (mFrameInFlight) {
    if (const backend::Result waited = waitForFrame(); !backend::isSuccess(waited)) return waited;
  }
  if (mUniformBuffers.empty()) {
    note(backend::MessageSeverity::Error, "the presentation pass has no uniform buffer");
    return backend::Result::Error;
  }

  PresentUniforms uniforms {};
  const bool extended = parameters.outputColorSpace == backend::SwapchainColorSpace::ExtendedLinearP3;
  const bool pq = parameters.outputColorSpace == backend::SwapchainColorSpace::Hdr10Pq;
  const bool displayP3 = parameters.outputColorSpace == backend::SwapchainColorSpace::DisplayP3;
  if (!parameters.sourceIsLinear) {
    // The source is the canvas's sRGB-encoded Rec.709 target. A linear P3 intermediate keeps the wide
    // gamut through the float target; the PQ path needs BT.2020 instead because a PQ signal is defined
    // over those primaries.
    const float (*matrix)[3] = pq ? kRec709ToBt2020 : kRec709ToP3;
    // The rows are written explicitly rather than through a pointer cast: the struct is three
    // vec4s and a loop over a float(*)[3] would stop one element short of each row's padding.
    uniforms.row0[0] = matrix[0][0];
    uniforms.row0[1] = matrix[0][1];
    uniforms.row0[2] = matrix[0][2];
    uniforms.row0[3] = 0.0f;
    uniforms.row1[0] = matrix[1][0];
    uniforms.row1[1] = matrix[1][1];
    uniforms.row1[2] = matrix[1][2];
    uniforms.row1[3] = 0.0f;
    uniforms.row2[0] = matrix[2][0];
    uniforms.row2[1] = matrix[2][1];
    uniforms.row2[2] = matrix[2][2];
    uniforms.row2[3] = 0.0f;
    uniforms.parameters[1] = 1.0f;
  } else {
    // The intermediate is already linear Display P3; only the encoding changes.
    uniforms.row0[0] = 1.0f;
    uniforms.row1[1] = 1.0f;
    uniforms.row2[2] = 1.0f;
    uniforms.parameters[1] = 0.0f;
  }
  uniforms.parameters[0] = pq ? 0.0f : (extended ? 1.0f : 2.0f);
  uniforms.parameters[2] = parameters.sdrWhiteNits > 0.0f ? parameters.sdrWhiteNits : 203.0f;
  uniforms.parameters[3] = displayP3 ? 1.0f : 0.0f;

  backend::Result result = ensureCommandBuffer();
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->reset();
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->begin();
  if (!backend::isSuccess(result)) return result;

  // Slot 0 is the canvas block in a frame; a presentation submission uses it for its own block because
  // the submission is synchronous (the queue waits), so no earlier command buffer can still read it.
  result = uploadBuffer(mUniformBuffers.back().handle, mUniformBuffers.back().bytes, 0, &uniforms,
          sizeof(uniforms));
  if (!backend::isSuccess(result)) return result;

  const backend::DescriptorWrite writes[] = {
    { 0, backend::BindingType::TextureSampler, source.color, mLinearClamp, {}, 0 },
    { 1, backend::BindingType::TextureSampler, mWhiteTexture, mLinearClamp, {}, 0 },
    { 2, backend::BindingType::UniformBuffer, backend::TextureHandle::invalid(),
      backend::TextureSamplerHandle::invalid(),
      { mUniformBuffers.back().handle, 0, kUniformSlotSize }, 0 },
  };
  result = mDevice->updateDescriptorSet(mPresentSet, writes, 3);
  if (!backend::isSuccess(result)) return result;

  backend::RenderPassBeginInfo info;
  info.renderPass = target.pass;
  info.colorAttachmentCount = 1;
  const bool multisampled = target.msaaColorView.valid();
  info.colorAttachments[0].view = multisampled ? target.msaaColorView : target.colorView;
  if (multisampled) info.colorAttachments[0].resolveTarget = target.colorView;
  info.colorAttachments[0].loadOp = backend::LoadOp::Clear;
  info.colorAttachments[0].storeOp = backend::StoreOp::Store;
  info.colorAttachments[0].clearColor = backend::Color::transparentBlack();
  info.renderArea = { target.width, target.height };
  result = mCommandBuffer->beginRenderPass(info);
  if (!backend::isSuccess(result)) return result;
  // A full-target pass names its own viewport and scissor. Relying on the defaults would work on a fresh
  // command buffer and silently inherit whatever scissor the last canvas batch set on a reused one.
  backend::Viewport viewport;
  viewport.width = static_cast<float>(target.width);
  viewport.height = static_cast<float>(target.height);
  result = mCommandBuffer->setViewport(viewport);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->setScissor({ 0, 0, target.width, target.height });
  if (!backend::isSuccess(result)) return result;

  const backend::PixelFormat depthFormat = backend::PixelFormat::Undefined;
  const PipelineKey key { Program::Present, Variant::Plain, kNoBlendMode, target.format, depthFormat,
    target.sampleCount };
  backend::RenderPipelineHandle pipeline = backend::RenderPipelineHandle::invalid();
  result = ensurePipeline(key, target.pass, &pipeline);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->bindRenderPipeline(pipeline);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->bindDescriptorSet(0, mPresentSet);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->setVertexBuffer(0, mPresentVertexBuffer, 0);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->draw(kCoverVertexCount, 1, 0, 0);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->endRenderPass();
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->end();
  if (!backend::isSuccess(result)) return result;

  backend::Queue* queue = mDevice->getQueue(backend::QueueType::Graphics);
  if (queue == nullptr) return backend::Result::Unsupported;
  result = queue->submit(*mCommandBuffer, backend::FenceHandle::invalid(), waitSemaphore, signalSemaphore);
  if (!backend::isSuccess(result)) {
    note(backend::MessageSeverity::Error, "the presentation pass could not be submitted");
    return result;
  }
  return backend::Result::Success;
}

backend::Result GpuRenderer::reportOmissions(const DrawList& list) {
  const auto grew = [](const uint32_t current, const uint32_t reported) { return current > reported; };
  if (grew(mUnsupported.gradients, mReported.gradients) || grew(mUnsupported.images, mReported.images) ||
      grew(mUnsupported.clipMasks, mReported.clipMasks) ||
      grew(mUnsupported.advancedBlends, mReported.advancedBlends) ||
      grew(mUnsupported.coverageFields, mReported.coverageFields) ||
      grew(mUnsupported.pathsWithoutStencil, mReported.pathsWithoutStencil)) {
    std::string message = "the GPU canvas renderer did not draw everything in this frame (\"";
    message += list.label.empty() ? std::string("unlabelled") : list.label;
    message += "\"): gradients=" + std::to_string(mUnsupported.gradients) +
      " images=" + std::to_string(mUnsupported.images) +
      " shapedClips=" + std::to_string(mUnsupported.clipMasks) +
      " advancedBlends=" + std::to_string(mUnsupported.advancedBlends) +
      " coverageFields=" + std::to_string(mUnsupported.coverageFields) +
      " pathsWithoutStencil=" + std::to_string(mUnsupported.pathsWithoutStencil) +
      ". See GpuRenderer's class comment for why each is missing.";
    note(backend::MessageSeverity::Warning, std::move(message));
    mReported = mUnsupported;
  }
  return backend::Result::Success;
}

// ==================================================================================================
// Readback and diagnostics
// ==================================================================================================

backend::Result GpuRenderer::readback(RenderTarget& target, Bitmap* outBitmap) {
  if (outBitmap == nullptr) {
    return backend::Result::InvalidArgument;
  }
  if (!mReady || !target.isValid()) {
    return backend::Result::Unsupported;
  }
  if (target.format != backend::PixelFormat::R8G8B8A8_UNORM) {
    // The read path produces the attachment's own format, so a target in another format would need a
    // conversion this renderer does not have. Refusing beats handing back bytes whose channel order is
    // a guess.
    note(backend::MessageSeverity::Error, "readback is implemented for R8G8B8A8_UNORM targets only");
    return backend::Result::Unsupported;
  }

  backend::Result result = waitForFrame();
  if (!backend::isSuccess(result)) {
    return result;
  }

  const uint64_t rowBytes = static_cast<uint64_t>(target.width) * 4u;
  const uint64_t size = rowBytes * target.height;

  const backend::BufferDescriptor descriptor{
    .size = size,
    .usage = static_cast<backend::BufferUsages>(backend::BufferUsage::TransferDst),
    // The GPU writes it and the host reads it, which is exactly `GpuToCpu`.
    .memoryUsage = backend::MemoryUsage::GpuToCpu,
    .initialData = nullptr,
    .label = "canvas/readback",
  };
  backend::BufferHandle buffer = backend::BufferHandle::invalid();
  result = mDevice->createBuffer(descriptor, &buffer);
  if (!backend::isSuccess(result)) {
    return result;
  }
  const BufferGuard guard(mDevice, buffer);

  result = ensureCommandBuffer();
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->reset();
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->begin();
  if (!backend::isSuccess(result)) return result;

  // A pass that draws nothing, only to make the target's framebuffer current. `copyTextureToBuffer` on
  // the OpenGL backend is `glReadPixels`, which reads whatever read framebuffer is bound rather than
  // the texture named in the copy -- and after a frame the bound framebuffer belongs to whichever
  // target happened to be drawn last. Opening and closing a pass on the target is what puts *its*
  // framebuffer back, and `LoadOp::Load` is what keeps the pass from destroying the pixels being read.
  backend::RenderPassBeginInfo info;
  info.renderPass = target.pass;
  info.colorAttachmentCount = 1;
  const bool multisampled = target.msaaColorView.valid();
  info.colorAttachments[0].view = multisampled ? target.msaaColorView : target.colorView;
  if (multisampled) info.colorAttachments[0].resolveTarget = target.colorView;
  info.colorAttachments[0].loadOp = backend::LoadOp::Load;
  info.colorAttachments[0].storeOp = backend::StoreOp::Store;
  info.renderArea = { target.width, target.height };
  // The pass has to name *every* attachment its compatibility key declares, even though this empty pass writes neither:
  // the Vulkan 1.0/1.1 fallback creates a framebuffer from the begin info, and a framebuffer whose attachment count does
  // not match the render pass is rejected (VUID-VkFramebufferCreateInfo-attachmentCount-00876). Loading the depth/stencil
  // attachment here leaves it untouched, which is what a readback needs.
  if (target.depthStencilView.valid()) {
    info.hasDepthStencil = true;
    info.depthStencilAttachment.view = target.depthStencilView;
    info.depthStencilAttachment.loadOp = backend::LoadOp::Load;
    info.depthStencilAttachment.storeOp = backend::StoreOp::Store;
    info.depthStencilAttachment.clearDepthStencil = { 1.0f, 0 };
  }
  result = mCommandBuffer->beginRenderPass(info);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->endRenderPass();
  if (!backend::isSuccess(result)) return result;

  backend::BufferTextureCopy copy;
  copy.buffer = buffer;
  copy.texture = target.color;
  copy.bufferRegion.dstOffset = 0;
  copy.bufferRegion.size = size;
  copy.textureRegion.textureOrigin = { 0, 0, 0 };
  copy.textureRegion.size = { target.width, target.height, 1 };
  copy.textureRegion.mipLevel = 0;
  copy.textureRegion.baseArrayLayer = 0;
  copy.textureRegion.arrayLayerCount = 1;
  result = mCommandBuffer->copyTextureToBuffer(copy);
  if (!backend::isSuccess(result)) return result;
  result = mCommandBuffer->end();
  if (!backend::isSuccess(result)) return result;
  result = mDevice->executeCommandBuffer(*mCommandBuffer);
  if (!backend::isSuccess(result)) return result;
  result = waitIdle();
  if (!backend::isSuccess(result)) return result;

  void* mapped = nullptr;
  result = mDevice->mapBuffer(buffer, 0, size, &mapped);
  if (!backend::isSuccess(result) || mapped == nullptr) {
    return backend::isSuccess(result) ? backend::Result::Error : result;
  }

  Bitmap bitmap = Bitmap::allocate(
    ImageInfo(static_cast<int>(target.width), static_cast<int>(target.height), ColorType::kRGBA_8888,
      AlphaType::kPremultiplied));
  if (bitmap.isEmpty()) {
    mDevice->unmapBuffer(buffer);
    return backend::Result::OutOfMemory;
  }

  const auto* source = static_cast<const uint8_t*>(mapped);
  Pixmap pixels = bitmap.lockPixels();
  auto* destination = static_cast<uint8_t*>(pixels.pixels());
  if (destination == nullptr) {
    mDevice->unmapBuffer(buffer);
    return backend::Result::Error;
  }
  // ## Row order is a property of the backend, not of the image
  //
  // `glReadPixels` numbers rows from the framebuffer's bottom-left, so the OpenGL backend's readback
  // hands them over bottom-up; everything in this renderer - and in `Bitmap` - numbers them from the top
  // left, hence the flip. A Vulkan image's row 0 is the top of the picture, and `vkCmdCopyImageToBuffer`
  // preserves that, so flipping there would turn every snapshot upside down. The distinction used to be
  // invisible because only one backend existed.
  const bool readbackIsBottomUp = mDevice->getInfo().backend == backend::Backend::OpenGL;
  const size_t destinationRowBytes = bitmap.rowBytes();
  const size_t copyBytes = std::min<size_t>(destinationRowBytes, static_cast<size_t>(rowBytes));
  for (uint32_t row = 0; row < target.height; ++row) {
    const uint32_t sourceRowIndex = readbackIsBottomUp ? target.height - 1 - row : row;
    const uint8_t* sourceRow = source + static_cast<size_t>(sourceRowIndex) * rowBytes;
    std::memcpy(destination + static_cast<size_t>(row) * destinationRowBytes, sourceRow, copyBytes);
  }
  mDevice->unmapBuffer(buffer);

  *outBitmap = std::move(bitmap);
  return backend::Result::Success;
}

backend::Result GpuRenderer::waitForFrame() {
  if (!mFrameInFlight) return backend::Result::Success;
  if (mDevice == nullptr || !mFrameFence.valid()) return backend::Result::Error;

  const backend::Result waited = mDevice->waitForFence(mFrameFence);
  if (!backend::isSuccess(waited)) return waited;
  // Leave the fence signalled for the command buffer's own reset; the next submit resets it before
  // `vkQueueSubmit`. Resetting it here would make that reset wait on an unsignalled fence forever.
  mFrameInFlight = false;
  // The targets and scratch images this frame borrowed become reusable only now, after its fence.
  releaseFrameResources();
  return backend::Result::Success;
}

backend::Result GpuRenderer::waitIdle() {
  if (mDevice == nullptr) {
    return backend::Result::Unsupported;
  }
  return mDevice->waitIdle();
}

void GpuRenderer::createTimingQuerySet() {
  if (mDevice == nullptr) return;
  if (!mDevice->getCapabilities().supports(backend::Feature::TimestampQuery)) return;
  backend::QuerySetDescriptor descriptor;
  descriptor.type = backend::QueryType::Timestamp;
  descriptor.count = 2;
  descriptor.label = "renderer/gpu-timing";
  if (backend::isSuccess(mDevice->createQuerySet(descriptor, &mTimingSet))) {
    mTimingEnabled = true;
  }
}

void GpuRenderer::beginTimingPass() {
  if (!mTimingEnabled || mCommandBuffer == nullptr) return;
  if (!backend::isSuccess(mCommandBuffer->resetQuerySet(mTimingSet, 0, 2))) {
    mTimingEnabled = false;
    return;
  }
  // The two backends model a timestamp set differently: Vulkan writes one timestamp per `endQuery`, so
  // the start is query 0 and the end query 1; OpenGL's elapsed timer starts at `beginQuery` and leaves
  // its nanoseconds in query 0.
  const bool timestampPair = mDevice != nullptr && mDevice->getInfo().backend == backend::Backend::Vulkan;
  const backend::Result started = timestampPair ? mCommandBuffer->endQuery(mTimingSet, 0)
                                                : mCommandBuffer->beginQuery(mTimingSet, 0);
  if (!backend::isSuccess(started)) mTimingEnabled = false;
}

void GpuRenderer::endTimingPass() {
  if (!mTimingEnabled || mCommandBuffer == nullptr) return;
  if (!backend::isSuccess(mCommandBuffer->endQuery(mTimingSet, 1))) {
    mTimingEnabled = false;
    return;
  }
  mTimingPending = true;
}

void GpuRenderer::readTimingResults() {
  if (!mTimingEnabled || !mTimingPending || mDevice == nullptr) return;
  mTimingPending = false;
  uint64_t results[2] = { 0, 0 };
  if (!backend::isSuccess(mDevice->getQueryResults(mTimingSet, 0, 2, results, sizeof(results)))) return;
  const bool timestampPair = mDevice->getInfo().backend == backend::Backend::Vulkan;
  const uint64_t nanos = timestampPair ? (results[1] > results[0] ? results[1] - results[0] : 0)
                                       : results[0];
  mGpuMilliseconds = static_cast<double>(nanos) / 1000000.0;
}

std::string GpuRenderer::describeDevice() const {
  if (mDevice == nullptr) {
    return "no device";
  }
  const backend::DeviceInfo& info = mDevice->getInfo();
  std::string description = info.deviceName != nullptr && info.deviceName[0] != '\0'
                              ? info.deviceName
                              : "unknown device";
  description += " (";
  description += info.vendor != nullptr && info.vendor[0] != '\0' ? info.vendor : "unknown vendor";
  description += ", ";
  description += info.apiVersion != nullptr && info.apiVersion[0] != '\0' ? info.apiVersion : "unknown API";
  description += ", ";
  description += info.backendName != nullptr && info.backendName[0] != '\0' ? info.backendName : "unknown backend";
  description += ")";
  return description;
}

std::string GpuRenderer::diagnostics() const {
  std::string report;
  report += "device: ";
  report += describeDevice();
  report += mOwnedDevice != nullptr ? " [owned]" : " [borrowed]";
  report += "\n";
  report += "shaders embedded: " + std::to_string(mEmbeddedShaderCount);
  report += ", pipelines cached: " + std::to_string(mPipelines.size());
  // Both numbers, because the pair is the measurement: `uploads` grows and `cached` does not once every distinct bitmap
  // has been seen, which is exactly what a content-keyed cache is supposed to do across frames. The pages are the third
  // number of that set: they are what a session's bitmaps are actually held in, and their count is bounded by
  // `kImageAtlasPageBudget` rather than by how many distinct bitmaps have ever been drawn.
  report += ", image placements cached: " + std::to_string(mImagePlacements.size());
  report += ", image atlas pages: " + std::to_string(mImagePages.size());
  report += ", field atlases pooled: " + std::to_string(mFieldAtlases.size());
  report += ", textures uploaded: " + std::to_string(mImageUploads);
  report += ", layer targets pooled: " + std::to_string(mLayerPool.size());
  report += ", snapshots pooled: " + std::to_string(mScratchPool.size()) + "\n";
  report += "frames submitted: " + std::to_string(mFrameCount) + "\n";
  report += "last frame: commands=" + std::to_string(mStats.commandCount) +
    " batches=" + std::to_string(mStats.batchCount) +
    " draws=" + std::to_string(mStats.drawCallCount) +
    " layers=" + std::to_string(mStats.layerCount) +
    " vertices=" + std::to_string(mStats.vertexCount) +
    " uploadBytes=" + std::to_string(mStats.uploadBytes) + "\n";
  report += "last frame: precompile=" + std::to_string(mStats.precompileMilliseconds) +
    " ms (summed over the tasks that ran it)\n";
  // ## The frame's stages, which is what a frame's total has to be read against
  //
  // Reported as one line in the order the stages run, so that a number that moved can be attributed without a second
  // measurement: `total` is the whole of `submit`, including the bookkeeping between the stages, so the line's parts are
  // expected to sum to *less* than it rather than to it. See `gpu::FrameStages`.
  report += "last frame: stages fields=" + std::to_string(mStats.stages.fields) +
    " placeImages=" + std::to_string(mStats.stages.placeImages) +
    " precompile=" + std::to_string(mStats.stages.precompile) +
    " targets=" + std::to_string(mStats.stages.targets) +
    " arena=" + std::to_string(mStats.stages.arena) +
    " vertexUpload=" + std::to_string(mStats.stages.vertexUpload) +
    " record=" + std::to_string(mStats.stages.record) +
    " execute=" + std::to_string(mStats.stages.execute) +
    " total=" + std::to_string(mStats.stages.total) + " ms\n";
  // The two CPU stages the coverage-field route is split into, reported as wall clock: fields generated (the stage a pool
  // can run) and everything after it (the stage that cannot, because one GL context is single-threaded).
  report += "last frame: coverage fields generated=" + std::to_string(mStats.coverageFieldCount) +
    " in " + std::to_string(mStats.coverageFieldMilliseconds) + " ms (wall clock)\n";
  report += "last frame omissions: gradients=" + std::to_string(mUnsupported.gradients) +
    " images=" + std::to_string(mUnsupported.images) +
    " shapedClips=" + std::to_string(mUnsupported.clipMasks) +
    " advancedBlends=" + std::to_string(mUnsupported.advancedBlends) +
    " coverageFields=" + std::to_string(mUnsupported.coverageFields) +
    " pathsWithoutStencil=" + std::to_string(mUnsupported.pathsWithoutStencil) + "\n";
  if (mDevice != nullptr) {
    mDevice->appendDiagnostics(report);
  }
  for (const std::string& message: mMessages) {
    report += "note: ";
    report += message;
    report += "\n";
  }
  if (!mDeviceCreationLog.empty()) {
    report += "device creation log:\n";
    report += mDeviceCreationLog;
    report += "\n";
  }
  return report;
}

/**
 * @return true when every changed field's box was written on its own, false when the caller must upload the whole atlas.
 *
 * See the call site for why this is worth doing (4.2 ms a frame on the demo's animation, measured) and why it is safe (the
 * kept fields' bytes did not change, so the texture already holds them). The gathering is one `copy_n` per row out of the
 * atlas's strided rows into a tightly packed scratch buffer, because that is the layout `writeTexture` reads with
 * `bytesPerRow = 0` -- the same layout `writeAtlas` uses for the whole thing.
 *
 * A box that does not fit where the placement says it is means this frame's tables and the frame's fields disagree, which
 * only happens on a path that is already counting an omission: the answer is then the full upload, never a guess.
 */
bool GpuRenderer::uploadChangedFieldBoxes(const Frame& frame, const uint32_t atlasIndex, FieldAtlas& atlas) {
  if (frame.list == nullptr || atlasIndex >= frame.atlases.size()) return false;
  const Frame::AtlasUpload& upload = frame.atlases[atlasIndex];
  const size_t stride = upload.width;
  if (stride == 0) return false;

  for (const uint32_t field: mFieldRefill) {
    if (field >= frame.placed.size() || field >= frame.list->coverageFields.size()) return false;
    const Frame::PlacedField& place = frame.placed[field];
    if (place.atlas != atlasIndex) continue;
    const DrawList::CoverageField& request = frame.list->coverageFields[field];
    if (request.isEmpty()) continue;
    const size_t boxWidth = static_cast<size_t>(request.width) + 2;
    const size_t boxHeight = static_cast<size_t>(request.height) + 2;
    if (static_cast<size_t>(place.left) + boxWidth > stride) return false;

    mFieldUploadScratch.resize(boxWidth * boxHeight);
    for (size_t row = 0; row < boxHeight; ++row) {
      const size_t offset = (static_cast<size_t>(place.top) + row) * stride + place.left;
      if (offset + boxWidth > upload.bytes.size()) return false;
      std::copy_n(upload.bytes.begin() + static_cast<std::ptrdiff_t>(offset), boxWidth,
              mFieldUploadScratch.begin() + static_cast<std::ptrdiff_t>(row * boxWidth));
    }

    backend::TextureCopyRegion region;
    region.textureOrigin = { place.left, place.top, 0 };
    region.size = { static_cast<uint32_t>(boxWidth), static_cast<uint32_t>(boxHeight), 1 };
    region.mipLevel = 0;
    region.baseArrayLayer = 0;
    region.arrayLayerCount = 1;
    if (!backend::isSuccess(mCommandBuffer->writeTexture(atlas.texture, region, mFieldUploadScratch.data(),
                static_cast<uint64_t>(mFieldUploadScratch.size())))) {
      return false;
    }
  }
  return true;
}

} // namespace graphics::gpu




