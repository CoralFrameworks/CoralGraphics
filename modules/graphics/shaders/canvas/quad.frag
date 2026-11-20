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

// The fragment half of the instanced quad pass: turn one fragment of a rectangle into one
// **premultiplied** colour, with an exact rounded-rectangle coverage when the instance asks for one.
// See `quad.vert` for the conventions this directory follows - in particular that `#version` has to
// be the first line, ahead of the licence header - and for the meaning of `aParams`, whose
// components arrive here as `vParams`.
//
// ## Why the precision is highp
//
// This is not a default worth copying from `present.frag`, which uses `mediump` because it only
// moves a colour from one texture to another. Everything here is measured in **device pixels**, and
// a mediump float has a ten-bit significand: at a device coordinate of 1024 - an ordinary tablet or
// a 2x-scaled 512-pixel target - the spacing between representable values is already a full pixel,
// so the signed distance below quantises to whole numbers and the antialiased edge collapses into
// visible steps. `highp` is the qualifier for a distance in pixels, and ESSL 3.00 requires
// fragment-shader support for it, so this costs nothing in portability that ES 3.00 was not already
// promising. The alternative - keeping mediump and computing the distance relative to the rect's
// centre, where the values are small - would be correct in the middle of a large rect and still
// wrong at its edges, which is the one place the distance matters.

// ## Why the locals below are not `const`
//
// glslang refuses a `const` declaration whose initializer is not a constant expression in the ES
// profile - "'non-constant initializer' : not supported with this profile: es" - and that is the ES
// profile's rule, not a version floor, so the answer is to drop the qualifier rather than to raise
// `#version`. `quad.vert` carries the longer note and the glslang reference.

precision highp float;

// The quad's own coordinate, in {0,1}^2. Its two jobs are the reason there is one varying and not
// two: it locates the fragment inside the rectangle for the distance below, and it is the texture
// coordinate, so a rectangle the size of the texture is a 1:1 blit.
layout(location = 0) in vec2 vTexCoord;

layout(location = 1) in vec4 vColor;
layout(location = 2) in vec4 vParams;
layout(location = 3) in vec2 vRectSize;

layout(location = 0) out vec4 oColor;

// The source image for a textured instance. Declared unconditionally even though a frame of solid
// rectangles never samples it: a sampler that a shader does not statically use still has to be bound
// by the caller, because the backend reflects the declaration rather than the use. The alternative -
// a separate untextured program - is a second module, a second pipeline and a second descriptor set
// layout for a branch that costs one comparison.
//
// `set` and `binding` are what the backend's reflection reads. The OpenGL translation drops `set` -
// OpenGL has one namespace - and keeps the binding.
//
// Binding 0 is free for a *sampler* and reserved for a *block*: the OpenGL backend emulates push
// constants through a uniform buffer at `kPushConstantBinding = 0`, so the canvas uniform block is at
// binding 1 (`quad.vert`) while this sampler takes binding 0. A sampler is a texture unit and a block
// is a uniform-block binding point, which is why the same number can be used for both without
// collision.
layout(set = 0, binding = 0) uniform sampler2D uTexture;

// The exact signed distance, in device pixels, from a point inside the rectangle to the boundary of a
// rectangle with rounded corners.
//
//   @param point   the fragment's position relative to the rectangle's top-left corner, in pixels.
//   @param size    the rectangle's size in pixels.
//   @param radius  the corner radius in pixels, clamped internally.
//
// Negative inside, positive outside, zero on the boundary. That sign convention is what makes the
// coverage a single clamp rather than a case analysis over four edges: there is one number to filter,
// and every shape this pass can draw is a level set of it.
//
// The construction is the standard rounded-box distance and it is exact - no polynomial
// approximation of the arc and no fan of corner triangles. A point's distance to a rectangle with
// rounded corners is the distance to the inner rectangle that the four corner arcs are centred on,
// less the radius. `max(abs(centred) - (halfSize - radius), 0.0)` is exactly the distance to that
// inner rectangle, because it is zero anywhere along the straights and grows only in the corners.
float roundedRectDistance(const vec2 point, const vec2 size, const float radius) {
  // The rectangle is moved so its centre is at the origin. The distance is then symmetric in x and y
  // and one expression covers all four edges and all four corners, instead of four sign cases.
  vec2 halfSize = 0.5 * size;
  vec2 centred = point - halfSize;

  // The radius is clamped to half the shorter side, which is the largest radius the rectangle has
  // room for. Without this an over-specified radius makes `halfSize - radius` negative, the
  // `max(..., 0.0)` below then measures from a point outside the shape, and the rectangle renders as
  // a blob that grows as the radius grows. Clamping is also what the CPU rasterizer does, so a caller
  // that passes a silly radius gets the same picture from both paths rather than a difference to
  // debug.
  float clamped = min(radius, min(halfSize.x, halfSize.y));

  // The distance to the inner rectangle the four arcs are centred on, then out to the arc.
  //
  // The `min(max(q.x, q.y), 0.0)` term is the interior half of the sign, and without it this function
  // is *not* the signed distance the note above describes: `length(max(q, 0.0))` is zero for every
  // point along the straights and anywhere inside them, so an interior fragment came out at exactly
  // zero and the filter below turned that into a coverage of one half. That is a whole rectangle
  // drawn at 50% alpha, and it is what the gradient stage's copy of this had to be fixed for as well
  // -- the two copies must agree, and the other one is in `gradient.frag`.
  //
  // `min(max(q.x, q.y), 0.0)` is negative by the depth of the point inside that inner rectangle and
  // zero outside it, which is what makes the sum negative inside, zero on the boundary and positive
  // outside. On the straights `q.y` is negative and `q.x` is zero, in a corner both are, and outside
  // both are positive so the term vanishes and the distance is the one to the arc.
  vec2 distanceToInner = abs(centred) - (halfSize - clamped);
  return length(max(distanceToInner, vec2(0.0))) + min(max(distanceToInner.x, distanceToInner.y), 0.0) - clamped;
}

// The fraction of a fragment that the rectangle covers, in 0..1.
//
// A one-pixel box filter over the signed distance: full coverage half a feather-width inside the
// boundary, none half a feather-width outside, linear in between. That is the standard
// coverage-from-distance filter, and it is why the SDF is worth computing in the first place - a
// rotated or curved edge cannot be antialiased by a coverage mask that was rasterised on the CPU at
// a different scale.
//
// ## Why not `fwidth`
//
// `fwidth` is the usual way to obtain a filter width without being told, and it *is* core in
// ESSL 3.00 - it needed an extension in ES 2.00, which is where the habit of avoiding it comes from -
// so nothing here would fail to compile if it were used. It is not used, for three reasons:
//
//   1. The caller already knows the feather in device pixels. The shape it is drawing was measured in
//      device pixels, so the caller's answer is the intended one and a screen-space derivative is a
//      guess at it.
//   2. `fwidth` makes the edge softness a function of the triangle's screen area, so a zoomed-in
//      rectangle and a zoomed-out one would feather differently for no reason the caller asked for.
//   3. It is undefined where the derivative is zero - a fully covered fragment inside the rectangle
//      has a constant distance across the quad, so the derivative is zero and the division by it is
//      infinite. The first and last fragments of a triangle have the same problem for a different
//      reason: `dFdx` is undefined in a non-uniform control flow region and along the edge of a
//      primitive that has no neighbour to difference against.
//
// Reason 3 alone settles it: this shader's `params.z` gate is exactly the non-uniform control flow
// that would corrupt a derivative, and a filter width that is occasionally infinite is a bug that
// appears on some drivers and not others.
float roundedRectCoverage(const vec2 point, const vec2 size, const float radius, const float feather) {
  if (feather <= 0.0) {
    // A hard edge. The caller asked for one - a pixel-aligned rectangle that is known to land on the
    // grid has nothing to antialias - and returning the sign is cheaper than a filter over a width
    // that is zero and would divide by it.
    return roundedRectDistance(point, size, radius) <= 0.0 ? 1.0 : 0.0;
  }

  // The filter has half its weight on each side of the boundary, which is why it is centred rather
  // than offset: at the boundary the coverage is a half, which is the answer a covering filter must
  // give.
  float distance = roundedRectDistance(point, size, radius);
  return clamp(0.5 - distance / feather, 0.0, 1.0);
}

void main() {
  // The paint colour, premultiplied once, here, rather than per use below. `aColor` arrives
  // non-premultiplied because that is what `graphics::Color` holds and what a caller has; the
  // coverage multiplies a premultiplied colour's four components by the same factor, which is the
  // property this pass exists to exploit.
  vec4 paint = vec4(vColor.rgb * vColor.a, vColor.a);

  // Untextured by default, which is the common instance: a solid fill has no image to fetch and a
  // fetch that is not needed is a dependent texture read per fragment.
  vec4 fill = paint;

  if (vParams.w > 0.5) {
    // A threshold rather than an equality test: the flag crossed a varying, and `== 1.0` on an
    // interpolated float is the kind of comparison that works until a driver interpolates in a
    // different order and 1.0 arrives as 0.9999999.
    //
    // The multiply is a modulate of premultiplied by unpremultiplied, written as `texture * paint`
    // because `paint` is already premultiplied: the texel's colour is scaled by the paint's colour
    // and its alpha by the paint's alpha, and neither the texel nor the result ever has to be
    // unpremultiplied. Writing it as `texel.rgb * vColor.rgb, texel.a * vColor.a` with `vColor`
    // unpremultiplied would be the same arithmetic with one more multiply per fragment.
    //
    // `vTexCoord` is the quad's own coordinate, so texel (u, v) lands on rectangle pixel
    // (u * width, v * height): a rectangle the size of the texture is a 1:1 blit and any other size
    // is a scale. This pass carries no source-rectangle uniform, so a sub-rectangle blit or a
    // repeated tile needs one; it is left out because a solid fill is what the instanced quad is
    // for and an image is what it also does, and a fifth instance attribute is a real cost on every
    // solid fill in the frame.
    fill = texture(uTexture, vTexCoord) * paint;
  }

  // Full coverage by default. Most instances take this path - see `params.z` in `quad.vert` - and
  // taking it means the square root inside the distance is not executed at all.
  float coverage = 1.0;

  if (vParams.z > 0.5) {
    // ## The rectangle is not the quad
    //
    // An antialiased quad is *grown* by its feather in the vertex stage, because a rasterizer only produces a fragment
    // for a pixel whose centre is inside the triangle: a pixel the shape covers a quarter of, with its centre just
    // outside, used to keep whatever the destination already held instead of being covered by the fraction the distance
    // below computes. So `vRectSize` is the grown size, the true rectangle begins one feather in from the grown quad's
    // origin, and both the point and the size handed to the distance function are corrected here. With a hard edge
    // `vParams.y` is zero and this is the plain `vTexCoord * vRectSize` it always was.
    //
    // `vTexCoord * vRectSize` is the fragment's position inside the *quad* in pixels, which is what the distance is
    // defined in. Reconstructing it from `gl_FragCoord` instead would need the rectangle's origin as well and would pick
    // up the viewport's own origin, which is not the rectangle's.
    coverage = roundedRectCoverage(vTexCoord * vRectSize - vParams.y, vRectSize - 2.0 * vParams.y, vParams.x, vParams.y);
  }

  // Premultiplied out, which is the contract every consumer of this pass depends on:
  // `BlendState::premultipliedBlend()` is `ONE, ONE_MINUS_SRC_ALPHA`, and it is only that mode that
  // lets a coverage scale the alpha and the colour by the same factor. Blending this output as if it
  // were non-premultiplied - `SrcAlpha, OneMinusSrcAlpha` - would multiply the alpha in twice. That
  // is the classic "my antialiased edges are too dark" picture, and it is a state mismatch rather
  // than anything in this shader.
  oColor = fill * coverage;
}
