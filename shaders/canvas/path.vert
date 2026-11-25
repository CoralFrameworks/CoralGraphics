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

// The vertex half of the path fill pass: the geometry a stencil-then-cover fill draws.
//
// ## What stencil-then-cover is, and why a path needs it
//
// A path fill is "every fragment whose winding number is non-zero". Rasterising that directly means
// computing a winding number per fragment, which is what a great many triangles and a lot of fill
// rate look like. The standard alternative is two passes: draw the path's edges into the stencil
// buffer, incrementing and decrementing it so the stencil ends up holding the winding number, then
// draw one rectangle over the path's bounds with a stencil test of "not zero" and let the fixed
// function do the rest. The second pass costs one rectangle of fill rate and no triangle soup.
//
// This stage is the first pass - or either of them, because it is the same stage for both. It is a
// plain transform and nothing else, and that is the point: the portability, the triangle count and
// the fill rate all live in how the caller feeds it, not here.
//
// ## Why the vertices are in device pixels
//
// The caller has already transformed and flattened the path - a Bezier is not a straight line and the
// stencil pass needs the flattened one - and the CPU rasterizer does that work in device space. Taking
// clip-space vertices here would mean the shader cannot be reused for a second target size without a
// resubmit, and it would put a divide per vertex in a pass that can have thousands of them.
//
// ## The conventions
//
// See `present.vert` and `shaders/README.md`: `#version` on the first line ahead of the licence
// header, `layout(location = N)` on every stage input and output, `layout(set = N, binding = M)` on
// every resource, and a `precision` statement.

precision highp float;

// The path's flattened vertices, in device pixels - the same space `quad.vert` takes its rectangles
// in, which is what lets a caller build one buffer for both.
//
// A triangle **fan** over this buffer, with the topology coming from the pipeline rather than from
// here: a fan is what a convex path needs and what a caller emitting a triangulated concave path also
// satisfies, and neither is a fact this stage can see.
layout(location = 0) in vec2 aPosition;

// The fill colour, per vertex. Carried in the geometry rather than read from a uniform block: the
// cover rectangle is six vertices of one draw, so a per-draw block would have to be rebound and
// range-offset for every path, and keeping the colour with the vertices makes the cover pass a
// function of its own vertex buffer alone.
layout(location = 1) in vec4 aColor;

layout(location = 0) out vec4 vColor;

// The per-frame block, declared byte for byte as `quad.vert` declares it.
//
// That is deliberate and load-bearing. One uniform buffer object holds these three vectors for the
// whole frame, and the same buffer is bound for the quad pass and the path pass; the two programs
// read it with the same offsets because they declare the same members in the same order. Changing a
// member or its type in one file and not the other does not fail to compile, does not fail to link,
// and does not fail validation - it draws the canvas shifted by whatever the difference happens to
// be. The two declarations are one declaration in two places.
//
// The binding is 1, not 0, even though this stage has no texture at binding 0. A descriptor set
// layout is shared across the programs that use it, and a block at a different binding in this
// program would need a second set layout and a second set per frame to carry the same three vectors.
layout(std140, set = 0, binding = 1) uniform CanvasUniforms {
  // See `quad.vert`: carried by the block, unread by these two stages, and present so that a pass
  // needing the target's size in pixels does not have to add a member and invalidate every buffer
  // already laid out against this one.
  vec2 uViewport;
  vec2 uScale;
  vec2 uOffset;
  // The remaining members are unread by this stage. They are declared so this block is identical to `quad.vert`'s:
  // OpenGL requires every stage using a uniform block to declare the same members, in the same order.
  vec2 uPadding;
  vec4 uImageFilter;
  float uOutputPq;
} uCanvas;

void main() {
  // Device pixels to clip space, the same conversion `quad.vert` performs and with the same Y
  // convention: device +y runs down from the target's top-left, the RHI's clip space has y = -1 at
  // the top, so `uScale.y` is positive - `uScale = (2/width, 2/height)` and `uOffset = (-1, -1)` -
  // and the flip that puts it right way up in OpenGL happens at translation time, not here. See the
  // file-level note on Y in `quad.vert` for the whole chain.
  //
  // The two stages agreeing here is not cosmetic. A fill drawn with one convention and a rect drawn
  // with the other would put the fill's cover rectangle somewhere else on the target than the
  // stencil it is supposed to be testing against, and the result is a path that vanishes or fills
  // the wrong region rather than one that is visibly flipped - the stencil test is where the two
  // passes have to agree, and a flipped cover rectangle mostly misses it.
  //
  // No depth: this pass writes none, and the stencil buffer rather than the depth buffer is what
  // orders the two passes.
  gl_Position = vec4(aPosition * uCanvas.uScale + uCanvas.uOffset, 0.0, 1.0);
  vColor = aColor;
}
