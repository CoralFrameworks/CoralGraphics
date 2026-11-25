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

// The fragment half of the instanced quad pass, evaluated as a **coverage field sample**.
//
// ## What this is, in one sentence
//
// Step S3 of the edge-coverage plan: the fragment stage's half of the unified coverage definition, which is "bilinearly
// sample the field the software rasterizer produced".
//
// ## Why coverage and not a signed distance
//
// The plan first called for a signed distance field on the assumption that the rasterizer already computed one. It does
// not, and step S2 measured both against geometry neither defines -- the exact area of an axis-aligned rectangle at
// fractional coordinates -- and found the rasterizer within **13** of 255 while a radial distance mapped to coverage was
// within **39**:
//
//   for a rectangle spanning x = 10.3 .. 30.4, the pixel centred at x = 30.5 is *outside* and 0.1 past the edge, so the
//   radial distance is -0.1 and `clamp(0.5 - d)` reports 0.6 covered, while the exact overlap is 0.12.
//
// A distance to the nearest edge cannot see that the shape is a tenth of a pixel wide on the far side. So the field
// stores coverage, and this stage samples it -- which reproduces the rasterizer instead of adding a third answer.
//
// ## The sampling expression, which the CPU reference mirrors exactly
//
// `vRectSize` is the rectangle's size in device pixels, so `vTexCoord * vRectSize` is the fragment's device position
// within the rectangle, and `uFieldOrigin` is where the field starts in that same space. Texel `i` holds the rasterizer's
// coverage of the **whole** device pixel `[fieldOrigin + i, fieldOrigin + i + 1)`, so its value sits at that pixel's
// centre and the texel-centre coordinate is simply `(device - fieldOrigin) / uFieldExtent` -- no half-texel term.
//
// That last point was wrong here for a round and the mistake is worth keeping written down, because it produces a picture
// that looks plausible. With `+ 0.5 / fieldSize` the texel was placed on a device pixel *corner*, so a fragment at a pixel
// centre landed exactly between two texels and `texture()` averaged them: a two-texel box blur of a quantity that is
// already a per-pixel average. The interior of a shape is unaffected (every texel there is 255) and every edge is wrong --
// a band's last covered row read 0.5 instead of 1, and a field's one-pixel margin painted a quarter of the interior over
// pixels the shape never touches.
//
// ## Why the divisor is the *texture's* extent and not the field's
//
// A field texture is allocated in power-of-two **size classes** (`GpuRenderer::CoverageFieldTexture`), so it is usually
// larger than the field it holds: a 20x14 field lives in a 32x16 texture, with the field's texels in the top-left corner
// and zeroed padding beside them. The divisor here is therefore the texture's extent -- and what makes that safe is the
// property above: at a fragment on a device pixel's *centre*, `(device - fieldOrigin)` is an integer, so the linear
// filter's fractional part is zero and the neighbouring texel gets weight zero. The padding can never reach the picture,
// which is why it does not have to hold the field's own values.
//
// A consequence worth stating: the field's own extent is not a uniform here at all. The fragment stage never needs to
// know where the field ends, because the quad it draws *is* the field's lattice and every fragment outside it is clipped.
//
// `sampleCoverageField` in `EdgeCoverage.h` computes the same function on the CPU, term for term (with the field's own
// extent, since it holds no texture). `EdgeCoverage.TheShaderExpressionMatchesTheReference` evaluates this expression
// over the same grid, and `EdgeCoverage.APaddedTextureSamplesLikeTheFieldAtPixelCentres` states the paragraph above as a
// test. If this arithmetic changes, those tests have to change with it -- which is stated here because a shader cannot
// include the header that would enforce it.
//
// ## What it is not yet used by
//
// Nothing includes this program: the renderer still draws paths through the stencil and the coverage-image route. Wiring
// it in is step S4, deliberately after the formula is verified on its own -- the ordering the plan sets out, and the
// reason the previous attempt at this (the coverage-image route) shipped a formula nobody had checked.

precision highp float;

layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec4 vParams;
layout(location = 3) in vec2 vRectSize;

layout(location = 0) out vec4 oColor;

// The coverage field, bound like every other sampled program's texture: the sampler at binding 0, the canvas block at
// binding 1 -- `quad.vert` declares that one and reads it for the geometry.
//
// **One channel**, `R8_UNORM`, read from `.r`: the texture holds coverage and nothing else, so the three channels an RGBA
// upload would carry are three channels this stage never reads -- four times the bandwidth for a picture that is
// identical. The alpha the *image* route samples is a different program (`quad.frag`), which draws a glyph's bitmap
// premultiplied white because an image needs a colour; a coverage field does not.
layout(set = 0, binding = 0) uniform sampler2D uCoverageField;

// One `vec4`, matching `CompositeUniforms` on the host exactly, because that shape is already proven to arrive.
// `xy` is the extent of the field's **atlas** and the rest is unused, packed rather than split into a `vec2` block so
// that the block keeps the shape every other sampled program's block has -- this area's history is largely a list of
// blocks that were almost the right shape.
//
// What used to be here and is not any more: the field's device origin. It moved into the **vertex** data
// (`vParams.xy`), and that move is what lets a batch of field commands share one draw call: the descriptor set this
// block belongs to is bound once per batch, so anything that differs per field has to arrive per vertex rather than
// per draw. A batch samples one atlas, which is why the atlas extent can stay here.
layout(std140, set = 0, binding = 2) uniform FieldUniforms {
  // xy is the field atlas's extent in texels: what a device position is divided by to get a texel coordinate. zw is
  // unused, and stated rather than quietly relied on -- a reader who assumes the field's own extent is here will get a
  // picture that is subtly the wrong scale.
  vec4 uField;
} uField;

void main() {
  // ## The fragment's device position, built from per-vertex data
  //
  // `vTexCoord` is the quad's own {0,1}^2 coordinate and `vRectSize` is the quad's size in device pixels, so
  // `vTexCoord * vRectSize` is the fragment's position *within the quad*; `vParams.xy` carries the quad's origin minus
  // the field's texel origin, which is the whole of what this stage needs to know about the geometry. Both arrive per
  // vertex, and that is what lets a batch of field commands share one draw: nothing here is per *draw* except the atlas
  // extent.
  //
  // Texel-centre coordinates: the field's texel `i` is device pixel `fieldOrigin + i`, so its centre is at
  // `fieldOrigin + i + 0.5` and the texel index a fragment at a pixel centre lands on is an integer -- the texel the
  // rasterizer filled for that pixel. The divisor is the **atlas's** extent, which is usually much larger than the
  // field; see the note above for why the replicated border around the field makes the result exactly what a texture of
  // the field's own would have given, and why no half-texel offset belongs here either.
  vec2 coordinate = (vTexCoord * vRectSize + vParams.xy) / uField.uField.xy;

  // The linear sampler does the bilinear blend, which is the whole of the shared definition on this side. `.r`, because
  // the field is an `R8_UNORM` coverage atlas -- see the declaration above.
  float coverage = texture(uCoverageField, coordinate).r;
  // The paint's own colour and alpha modulate the fill, premultiplied, as `quad.frag` does for a texture.
  vec4 paint = vec4(vColor.rgb * vColor.a, vColor.a);
  oColor = paint * coverage;
}
