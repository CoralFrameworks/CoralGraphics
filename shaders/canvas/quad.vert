#version 300 es

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

// The workhorse of the hardware 2D canvas: one instanced draw call draws N rectangles.
//
// ## Why instanced rectangles rather than a draw call each
//
// A frame is thousands of small axis-aligned rects - a UI's boxes, a text run's one quad per glyph, a
// list's row backgrounds - and one draw call apiece makes the CPU the bottleneck long before the GPU
// is idle. Putting the per-rectangle data in instance attributes collapses that to one call per
// pipeline. It is also why `aRect`, `aParams` and `aColor` below are attributes and not uniforms: a
// uniform is per draw call, which is precisely the granularity this pass exists to avoid. The cost is
// a vertex fetch per vertex instead of a constant-bank read, which is a good trade for four vec4s.
//
// ## The conventions every shader in this directory follows
//
// `#version` is the first line, ahead of this comment and the licence header, because glslang refuses
// an ES-profile shader whose directive is not the very first thing; `layout(location = N)` on every
// stage input and output because SPIR-V requires explicit locations and a shader without them does
// not compile to it; `layout(set = N, binding = M)` on every resource because those decorations are
// what the backend's reflection reads; and a `precision` statement because GLSL ES has no default
// precision for floats. See `shaders/README.md` and `present.vert`.
//
// ## Which way Y runs
//
// Device pixels here are a canvas's: origin at the **top-left** of the target, +x right, +y **down**.
// The block's `uScale`/`uOffset` carry that into the RHI's clip space, and the RHI's clip space is
// Vulkan's - y = -1 is the **top** of the viewport, not y = +1. So the caller fills in
//
//     uScale  = vec2(2.0 / width, 2.0 / height)
//     uOffset = vec2(-1.0,        -1.0)
//
// and `uScale.y` is **positive**. Both signs are load-bearing, so here is the whole chain, because
// the obvious reading of "device y runs down and GL y runs up" gets it exactly backwards:
//
//   1. This stage writes y = -1 for a device y of 0, which is the top of the viewport in the RHI's
//      own convention. That is the same convention `present.vert` assumes when it passes a
//      clip-space quad straight through.
//   2. The OpenGL backend translates every SPIR-V module with SPIRV-Cross's `flip_vert_y`
//      (`ShaderCrossCompileInfo::flipVertexY` defaults to true and `GlDevice` passes it explicitly),
//      so the GLSL that reaches the driver negates y - the same device row arrives as +1.
//      `shader_compiler --verify` passes the same flag, which is why what it prints is the module a
//      device will run rather than a neighbouring configuration.
//   3. OpenGL maps y = +1 to the top of a positive-height viewport, and `GLCommandBuffer::setViewport`
//      mirrors the RHI's top-left-origin rectangle into GL's bottom-left origin rather than negating
//      anything. So device y = 0 lands at the top of the target, and device y = height at the bottom.
//
// Negating `uScale.y` here - writing y = +1 at device y = 0 - would invert step 1 and put the top of
// the canvas at the bottom of the target.
//
// `RasterizerState::flipY` is *not* part of this chain and is not a reason to negate anything: it
// defaults to false, and `GlCommandBuffer` reads it only to choose between `GL_CW` and `GL_CCW` for
// the front face. The flip that matters is the compile-time one in step 2, and a shader that tried
// to compensate for `flipY` as well would be compensating twice.
//
// Why this is worth a paragraph rather than a line: a screen of axis-aligned rectangles drawn upside
// down still looks like a screen of rectangles, so the mistake survives review. What catches it is
// `quad.frag`, which measures a signed distance in device pixels - a flipped rectangle has a flipped
// corner, and a rounded corner or a feathered edge comes out wrong in a way that is visible.

// ## Why nothing below is `const`
//
// glslang refuses a `const` declaration whose initializer is not a *constant expression* in the ES
// profile, and says so in exactly these words:
//
//     error: 'non-constant initializer' : not supported with this profile: es
//
// That is the ES profile's rule rather than a version floor - `requireProfile(loc, ~EEsProfile, ...)`
// in glslang's `ParseHelper.cpp` - so the same source is legal in desktop GLSL 4.20 and above and
// illegal at every ES version, and the fix is to drop the qualifier rather than to raise `#version`.
// A local `const` is a promise the compiler already keeps; what the pipeline needs is `const` on
// *globals* like the mode constants in `composite.frag`, whose initializers are constant expressions
// and which are the ones worth freezing.

precision highp float;

// The unit quad, one vertex at a time: a corner in {0,1}^2. Corners rather than absolute positions
// because the same six vertices then serve every instance and every rectangle size - the rect
// attribute supplies the position and the extent, and this supplies only "which corner am I".
//
// The alternative, a 4-vertex triangle strip, saves two vertices per instance and costs a topology
// the backend has to support and a winding the rasterizer has to agree with. Six vertices in a
// triangle list is the shape that is obviously correct in both directions.
layout(location = 0) in vec2 aCorner;

// The rectangle, once per instance, as `(x, y, width, height)` in **device pixels**. A component
// pair rather than an origin and a size in separate attributes so that a caller writing a packed
// instance record writes one contiguous vec4.
layout(location = 1) in vec4 aRect;

// The general-purpose selector, once per instance. One program serves several shapes because a
// second shape is a second pipeline, a second shader module and a second round of state changes per
// frame; a selector costs four floats per instance and no draw calls at all. Its meaning:
//
//   x  corner radius, in device pixels, for a rounded rectangle. 0 is a plain rectangle, and the
//      radius is clamped to half the shorter side in `quad.frag` so an over-large one cannot invert
//      the shape.
//   y  antialias feather, in device pixels: the width of the filter the edge coverage is computed
//      over. 0 asks for a hard edge, which is what a caller that knows it is drawing pixel-aligned
//      rectangles and wants no blending wants.
//   z  0 or 1. 1 asks for the rounded-rectangle coverage; 0 says the quad is fully covered
//      everywhere. Most instances in a frame take the second path - a text quad and a box fill are
//      axis-aligned and unrounded - and skipping the SDF for them is the difference between a
//      fragment shader that is a texture fetch and one that is a square root.
//   w  0 or 1. 1 asks for `uTexture` to be sampled; 0 says the colour is flat.
//
// Two flags rather than one integer enumerating the four combinations, because the combinations are
// genuinely independent: a rounded image quad is a legitimate thing to draw, and so is a plain
// untextured fill. A single mode enum would have to name the cross product and then be decoded here.
layout(location = 2) in vec4 aParams;

// The paint colour, once per instance: **non-premultiplied** RGBA in the unit interval, matching
// `graphics::Color`, which is what a caller has. Premultiplying it is done in `quad.frag`, once per
// fragment, because the coverage the SDF produces has to scale it and scaling a premultiplied colour
// scales its alpha by the same factor - which is the whole reason to store it premultiplied.
layout(location = 3) in vec4 aColor;

// Per-command styling for the SDF text program: x outline width, yz shadow offset, w glow radius.
layout(location = 4) in vec4 aSdf;
layout(location = 5) in vec4 aSdfOutlineColor;
layout(location = 6) in vec4 aSdfShadowColor;
layout(location = 7) in vec4 aSdfPattern;
layout(location = 8) in float aSdfFillAlpha;

// The quad's own coordinate, in {0,1}^2 across the rectangle. Two jobs, which is why there is one
// varying and not two: it is the local coordinate the SDF measures from, and it is the texture
// coordinate, so a rectangle the size of the texture is a 1:1 blit with no arithmetic in between.
//
// Set from `aCorner` directly, so the value at a fragment is the bilinear interpolation of two 0s and
// two 1s - exact in the corners and exact along the edges, which is what keeps a blit from sampling
// half a texel off. A per-fragment reconstruction from `gl_FragCoord` would need the rect back and
// would be worse for it.
layout(location = 0) out vec2 vTexCoord;

// The paint colour, flat. `flat` would be tempting - it is per instance, so it is constant across
// the quad - but the qualifier would make the varying's value undefined unless the *provoking*
// vertex carried it, which differs between backends for a triangle list. Interpolating four
// identical values is exact and portable, so it is the honest choice.
layout(location = 1) out vec4 vColor;

// The selector, flat for the same reason: it is per instance, and the fragment stage branches on it.
// Interpolating a 0/1 flag across the quad is exact, so the branch's condition is the same at every
// fragment of the quad; a `flat` qualifier would be faster on paper and ambiguous in practice.
layout(location = 2) out vec4 vParams;

// The rectangle's size in device pixels, which the fragment stage needs to place a fragment inside
// the rounded rectangle. Passed rather than recomputed from `vTexCoord` for the obvious reason:
// `vTexCoord` is normalized and the size is not, and dividing by a normalized coordinate to recover
// a pixel count is a division by zero at the quad's own edges.
layout(location = 3) out vec2 vRectSize;

layout(location = 4) out vec4 vSdf;
layout(location = 5) out vec4 vSdfOutlineColor;
layout(location = 6) out vec4 vSdfShadowColor;
layout(location = 7) out vec4 vSdfPattern;
layout(location = 8) out float vSdfFillAlpha;

// The per-frame block, shared by every device-space canvas program.
//
// `path.vert` declares this block with the same members in the same order and at the same binding,
// and that is not a coincidence to be maintained by hand: it is what lets one uniform buffer object,
// filled once per frame, serve the quad pass and the path pass. Reordering a member or changing its
// type here silently reinterprets the other program's data - the symptom is a canvas drawn shifted
// or scaled, with nothing in either shader to point at - so the two declarations are a pair.
//
// The binding is 1, not 0, and both reasons matter. Binding 1 is where the RHI's per-frame canvas
// data goes; binding 0 in this set is the backend's *push-constant* binding point
// (`kPushConstantBinding` in `libs/graphics/backend/include/private/backend/gl/GlDevice.h`), which
// the OpenGL backend emulates by uploading recorded push-constant bytes into a scratch uniform buffer
// and binding it there. A uniform block declared at binding 0 would be that same binding point and
// would read the push-constant buffer rather than the caller's. The fragment stage's sampler is at
// binding 0 only because a sampler is a texture unit in OpenGL and not a uniform-block binding, so
// the two are different namespaces and cannot collide.
layout(std140, set = 0, binding = 1) uniform CanvasUniforms {
  // The target's size in device pixels.
  //
  // This stage does not read it, and says so rather than leaving a reader to wonder: `uScale` and
  // `uOffset` below are the same information already converted, and deriving them here from
  // `uViewport` would be a second source of truth for one number, which is how a shader and the code
  // that fills its buffer start disagreeing. It is in the block because the block is shared, and
  // because the passes that will join it - a tiled fill counting repeats, a blur stepping in texels,
  // a clip pass measuring a mask - need the size in pixels rather than a transform, and a block is
  // the wrong place to discover a missing member.
  vec2 uViewport;

  // Device pixels to clip space, as `clip = device * uScale + uOffset`. A scale and an offset rather
  // than a matrix because this is one uniform scale per axis and no rotation: a `mat4` would cost
  // sixteen floats and a multiply to say what four floats and a multiply-add say, and a caller that
  // transforms its geometry does it in device space, where the SDF in the fragment stage still
  // measures pixels.
  //
  // The caller fills these in as `uScale = (2/width, 2/height)` and `uOffset = (-1, -1)` - the RHI's
  // clip space has y = -1 at the top, so `uScale.y` is positive. See the file-level note on Y.
  vec2 uScale;
  vec2 uOffset;

  // Padding so that `uImageFilter` lands at offset 32, which is where std140 puts a `vec4` after three `vec2`s. Stated
  // rather than silent: without it the CPU struct and the block disagree by eight bytes, and a block that is *almost* the
  // right shape is how this area's earlier defects began.
  vec2 uPadding;

  // The cubic resampler's B and C, written per image batch by the renderer. Only `image_cubic.frag` reads them; they live
  // in the shared block because a second binding would be a second thing to keep in step.
  vec4 uImageFilter;

  // 1 when the target presents in PQ (HDR10). Only `image.frag` reads it, but every stage that shares this binding must
  // declare the identical block: OpenGL links a vertex/fragment pair by matching the uniform block member for member,
  // and a stage that stops at `uImageFilter` makes the program fail to link with "struct type mismatch".
  float uOutputPq;
} uCanvas;

void main() {
  // The corner's device position, in the rect's own coordinates scaled up to pixels.
  vec2 device = aRect.xy + aCorner * aRect.zw;

  // Straight to clip space: this pass writes no depth, so z is 0 and w is 1 and there is no
  // perspective divide to fold the viewport in.
  gl_Position = vec4(device * uCanvas.uScale + uCanvas.uOffset, 0.0, 1.0);

  vTexCoord = aCorner;
  vColor = aColor;
  vParams = aParams;
  vRectSize = aRect.zw;
  vSdf = aSdf;
  vSdfOutlineColor = aSdfOutlineColor;
  vSdfShadowColor = aSdfShadowColor;
  vSdfPattern = aSdfPattern;
  vSdfFillAlpha = aSdfFillAlpha;
}
