#version 300 es

/* Copyright (C) 2026 The Coral Project
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

// The image fragment stage: an instanced rectangle sampling a **sub-rectangle** of its texture.
//
// ## Why this is a program of its own rather than a branch in `quad.frag`
//
// `quad.vert`/`quad.frag` already draws an image, and it samples the whole texture: `vTexCoord` is the
// quad's own {0,1}^2 coordinate, so texel (u, v) lands on rectangle pixel (u * width, v * height). A
// blit from *part* of a texture -- which is what an atlas needs, and what a repeated tile needs --
// therefore wants four more numbers per command, and `quad.frag` has nowhere to put them: `vParams.x/y`
// are the rounded-rectangle radius and feather, `vParams.z` says whether to compute coverage,
// `vParams.w` says whether to sample at all, and `vRectSize` is the *geometry* size that `quad.vert`
// needs for the position. Adding a fifth vertex attribute is the obvious fix and the wrong one: it
// would be paid by every flat fill and every coverage field in the frame, which is 559 image commands
// against 41000 field commands on the demo scene (路径边缘抗锯齿设计与现状.md 4.1.27).
//
// A separate fragment stage has the same vertex attributes as the quad -- `quad.vert` is literally its
// vertex stage, and the pipeline's vertex input is the quad's -- and *none* of the quad's per-command
// parameters, because it has no rounded-rectangle path and no untextured path. So `vParams` is free to
// carry the sub-rectangle, at no cost to any other command in the frame. This is the same trade
// `coverage.frag` makes, and for the same reason.
//
// The shader is deliberately tiny: no flags, no branches, no distance field. Every command that runs it
// samples.

// The same ES 3.00 note as `quad.frag`: the vertex stage declares `#version 300 es` and this stage is
// paired with it.
precision highp float;

// The quad's own coordinate, in {0,1}^2 -- the same varying `quad.vert` writes for every program that
// uses it. It is the *rectangle's* coordinate, not the texture's: the sub-rectangle mapping below is
// what turns it into a texture coordinate.
layout(location = 0) in vec2 vTexCoord;

layout(location = 1) in vec4 vColor;

// The sub-rectangle of the texture this command blits, in **normalized texture coordinates**:
// `xy` is its origin and `zw` its size. A command blitting the whole texture passes (0, 0, 1, 1), which
// is exactly what `quad.frag` computes for one, so a frame's picture cannot differ between the two
// programs until something actually uses a sub-rectangle.
layout(location = 2) in vec4 vParams;

layout(location = 0) out vec4 oColor;

// The source texture. Binding 0 is free for a *sampler* and reserved for a *block*: the OpenGL backend
// emulates push constants through a uniform buffer at `kPushConstantBinding = 0`, so the canvas uniform
// block is at binding 1 in `quad.vert` while this sampler takes binding 0. See `quad.frag` for the
// longer note.
layout(set = 0, binding = 0) uniform sampler2D uTexture;

// The canvas block `quad.vert` and `image_cubic.frag` declare, at the same binding: this stage reads
// only its last member (`uOutputPq`), and the block has to be declared here because it *is* the uniform
// block the image set already binds at binding 1 -- a sampler lives at binding 0 in every backend.
layout(std140, set = 0, binding = 1) uniform CanvasUniforms {
  vec2 uViewport;
  vec2 uScale;
  vec2 uOffset;
  vec2 uPadding;
  vec4 uImageFilter;
  float uOutputPq;
} uCanvas;

/**
 * Converts one sRGB-encoded, premultiplied texel to the PQ code a 10-bit HDR10 swapchain expects.
 *
 * Two transfers happen here and both are deliberate:
 *
 * - **sRGB encode to linear**, then the linear sRGB primaries to BT.2020 with the standard D65
 *   matrix, because a PQ signal is defined over BT.2020 primaries and writing sRGB primaries into it
 *   would shift every colour;
 * - **linear light to PQ** with SDR diffuse white at 203 cd/m^2 (BT.2408) over PQ's 10000 cd/m^2
 *   range. Everything above diffuse white clips: this is a presentation transform for SDR content in
 *   an HDR signal, not a tone mapper.
 */
vec3 pqFromSrgb(vec3 encoded) {
  vec3 linear = mix(encoded / 12.92, pow((encoded + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), encoded));
  const mat3 srgbToBt2020 = mat3(
    0.6274, 0.0691, 0.0164,
    0.3293, 0.9195, 0.0880,
    0.0433, 0.0114, 0.8956);
  linear = clamp(srgbToBt2020 * linear, 0.0, 1.0);
  vec3 nits = linear * 203.0;
  vec3 l = clamp(nits / 10000.0, 0.0, 1.0);
  const float m1 = 2610.0 / 16384.0;
  const float m2 = 2523.0 / 4096.0 * 128.0;
  const float c1 = 3424.0 / 4096.0;
  const float c2 = 2413.0 / 4096.0 * 32.0;
  const float c3 = 2392.0 / 4096.0 * 32.0;
  vec3 p = pow(l, vec3(m1));
  return pow((c1 + c2 * p) / (1.0 + c3 * p), vec3(m2));
}

void main() {
  // The paint colour, premultiplied once, exactly as `quad.frag` does it: `vColor` arrives
  // non-premultiplied because that is what `graphics::Color` holds, and the texture's texels are
  // premultiplied because that is what the sampler reads back from a premultiplied bitmap.
  vec4 paint = vec4(vColor.rgb * vColor.a, vColor.a);

  // The texture coordinate, which is the whole of what this program adds to the quad's: the fragment's
  // position inside the command's sub-rectangle. With (0, 0, 1, 1) this is `vTexCoord` itself, so the
  // status quo is the identity and the picture is unchanged -- which is the property the change that
  // introduced this shader was verified against.
  vec2 uv = vParams.xy + vTexCoord * vParams.zw;

  // A modulate of premultiplied by unpremultiplied, written as `texture * paint` because `paint` is
  // already premultiplied -- the same arithmetic as `quad.frag`'s textured branch, and for the same
  // reason: the texel's colour scales by the paint's colour and its alpha by the paint's alpha, and
  // neither has to be unpremultiplied.
  //
  // Premultiplied out, which is the contract every consumer of the quad's program depends on:
  // `BlendState::premultipliedBlend()` is `ONE, ONE_MINUS_SRC_ALPHA`, and blending this output as if it
  // were non-premultiplied would multiply the alpha in twice -- the classic "my antialiased edges are
  // too dark" picture.
  vec4 sampled = texture(uTexture, uv) * paint;
  // The SDR path is byte-identical to before: the flag is zero in every pass that writes an sRGB
  // target, and set only by the blit into a PQ swapchain (or a test surface).
  oColor = uCanvas.uOutputPq > 0.5 ? vec4(pqFromSrgb(clamp(sampled.rgb, 0.0, 1.0)), sampled.a) : sampled;
}

