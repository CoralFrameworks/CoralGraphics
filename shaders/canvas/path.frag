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

// The fragment half of the path fill pass: write one constant colour, and nothing else. See
// `path.vert` for what the pass is for and `present.vert` for the conventions this directory follows
// - in particular that `#version` has to be the first line, ahead of the licence header.
//
// ## Why this shader is three lines
//
// Every decision a path fill needs has already been made by the time a fragment gets here: the
// winding is in the stencil buffer, the coverage of an antialiased edge is in the stencil buffer too
// (a caller that wants one draws the cover rectangle through a coverage mask rather than asking this
// stage for a distance), and the shape is the rasterizer's. A fragment shader that computed
// something here would be re-deriving what the stencil pass already knows.
//
// ## Why the precision is mediump
//
// Unlike `quad.frag`, nothing here is measured in device pixels - the value is a uniform copied
// straight to an output, and the only arithmetic anywhere near it is the fixed-function blend the
// write goes through. A colour channel is eight bits, which mediump's ten-bit significand holds
// exactly, so `highp` would buy precision no channel of the destination can represent. `present.frag`
// makes the same choice for the same reason.
//
// This is not a claim that mediump is safe for the *blend*: that happens after the shader, in the
// fixed-function unit, at the framebuffer's own precision.

precision mediump float;

layout(location = 0) out vec4 oColor;

// The pass's one parameter: the fill colour, **premultiplied**.
//
// Premultiplied because this output feeds `BlendState::premultipliedBlend()` - `ONE,
// ONE_MINUS_SRC_ALPHA` - and because the coverage a partially covered fragment carries is expressed
// as an alpha, which has to scale the colour by the same factor for the result to be a colour rather
// than a hole. A caller that has a non-premultiplied `graphics::Color` multiplies its three channels
// by its alpha before writing the buffer, once, rather than here, once per fragment.
//
// The stencil pass's constant 1.0 is this uniform set to `vec4(1.0, 1.0, 1.0, 1.0)`, and it is a
// uniform rather than a literal in the shader on purpose. Hard-coding it would mean a second fragment
// module for the cover pass, differing in one constant, and then a second pipeline, a second
// descriptor set layout and a second round of state changes per path - all to avoid four floats in a
// buffer that is already being written. The usual stencil pass writes no colour at all, because
// `ColorTargetState::writeMask` is zero, so the value is discarded either way; it is specified rather
// than left undefined because a pass that writes no colour by *state* is one state change away from
// writing an uninitialised value.
//
// ## Why the block rather than a plain `uniform vec4`
//
// A plain default-block uniform would carry no `binding`, so a caller could only reach it by name via
// `glGetUniformLocation`, which is exactly the string-typed lookup that `layout(set, binding)`
// reflection exists to avoid. It would also make this program's set layout differ from every other
// canvas program's for the sake of one vector. A block at the same set as the others costs one
// descriptor and keeps the layouts comparable.
//
// ## Why binding 2 and not binding 0
//
// The OpenGL backend emulates push constants with a uniform buffer, and
// `kPushConstantBinding = 0` (`libs/graphics/backend/include/private/backend/gl/GlDevice.h`) is the
// block binding it uploads them to - `GlCommandBuffer` matches a descriptor's binding against that
// constant to decide whether the recorded bytes go to the scratch push-constant buffer. A **uniform
// block** declared at binding 0 would therefore be the same binding point as every push-constant
// range in the RHI, and would read the push-constant buffer instead of the caller's. Binding 0 is
// reserved for that; `quad.frag`'s sampler sits there only because a sampler is a *texture unit* in
// OpenGL and not a uniform-block binding, so the two cannot collide.
//
// Binding 2 rather than 1 because binding 1 is `CanvasUniforms`, declared by `path.vert` for the same
// draw: two distinct blocks at one binding point would both read whichever buffer was bound there.
// The fill colour, interpolated from the cover rectangle's six vertices. A constant across the
// primitive, so interpolation is exact. It is the only thing this stage reads: the cover pass has no
// uniform block of its own any more.
layout(location = 0) in vec4 vColor;

void main() {
  // Straight out: no coverage, no texture, no branch. The `path.vert` cover rectangle is the shape,
  // and the stencil test upstream is what decides whether this fragment exists at all.
  oColor = vColor;
}
