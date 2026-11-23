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

// The colour-managed presentation pass: one full-target draw that samples either the canvas's 8-bit
// sRGB target or the linear Display P3 16F intermediate, converts primaries, and writes the display
// encoding.
//
// ## Why the transform is not in `image.frag`
//
// `image.frag`'s PQ branch was the first HDR output stage and it encoded straight from the 8-bit
// canvas. That is correct for SDR content in an HDR container, and it is why it stays for the offscreen
// `SurfaceConfiguration::pqOutput` test path. It cannot carry values above diffuse white: the canvas
// is 8-bit sRGB, so the intermediate that should hold the highlight headroom does not exist. This pass
// is the one that does -- a 16F linear P3 target -- and it is deliberately separate from the canvas so
// that all of the canvas's premultiplied-8-bit contracts stay as they are.
//
// ## The two passes and their parameters
//
// The first pass reads the canvas (sRGB-encoded) and writes linear Display P3 into an RGBA16F target:
// `uSourceEncoded` decodes sRGB, the matrix converts Rec.709 primaries to P3, and `uMode` is linear.
// The second reads that target and writes the swapchain: the matrix is identity, `uSourceEncoded` is
// zero, and `uMode` chooses PQ, extended linear or gamma-encoded P3. A caller can run either pass on
// its own; the shader has no state that assumes the sequence.
//
// `uWhiteNits` is BT.2408's SDR diffuse white (203 cd/m^2 by default), and PQ's ceiling is 10000.
// Everything above the ceiling clips, deliberately: this is a presentation transform, not a tone
// mapper. An EDR surface keeps the values above 1.0 and lets the window server's tone mapping decide.

precision highp float;

layout(location = 0) in vec2 vTexCoord;

layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSource;

layout(std140, set = 0, binding = 2) uniform PresentUniforms {
  vec4 uRow0;
  vec4 uRow1;
  vec4 uRow2;
  // x: 0 = PQ, 1 = extended linear, 2 = gamma-encoded sRGB/P3
  // y: 1 when the source is sRGB-encoded
  // z: SDR diffuse white in cd/m^2
  vec4 uParameters;
} uPresent;

/** Decodes one sRGB-encoded channel to linear light. */
vec3 srgbToLinear(vec3 encoded) {
  return mix(encoded / 12.92, pow((encoded + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), encoded));
}

/** Encodes linear light with the sRGB curve (Display P3 uses the same transfer function). */
vec3 linearToSrgb(vec3 linear) {
  return mix(linear * 12.92, 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055,
             step(vec3(0.0031308), linear));
}

/** Encodes linear light in cd/m^2 as the BT.2100 PQ signal, normalised to [0, 1]. */
vec3 pqFromNits(vec3 nits) {
  const float m1 = 2610.0 / 16384.0;
  const float m2 = 2523.0 / 4096.0 * 128.0;
  const float c1 = 3424.0 / 4096.0;
  const float c2 = 2413.0 / 4096.0 * 32.0;
  const float c3 = 2392.0 / 4096.0 * 32.0;
  vec3 l = clamp(nits / 10000.0, 0.0, 1.0);
  vec3 p = pow(l, vec3(m1));
  return pow((c1 + c2 * p) / (1.0 + c3 * p), vec3(m2));
}

void main() {
  vec4 sampled = texture(uSource, vTexCoord);

  // The canvas is premultiplied. Unpremultiplying before a matrix is not necessary for a linear
  // transform (the matrix commutes with the alpha scale), but it is necessary before the PQ curve,
  // which is not linear. The output is opaque, so a transparent page composites onto black rather
  // than carrying alpha the swapchain cannot show.
  vec3 linear = sampled.rgb;
  if (sampled.a > 0.0001 && sampled.a < 0.9999) linear /= sampled.a;

  if (uPresent.uParameters.y > 0.5) linear = srgbToLinear(clamp(linear, 0.0, 1.0));

  // `uRow0..2` are the rows of the Rec.709 -> destination-primary matrix. They are a vec4 each because
  // std140 pads a mat3's columns to 16 bytes and stating the padding is what keeps the C++ struct and
  // the shader block in step.
  vec3 converted = vec3(dot(uPresent.uRow0.xyz, linear),
                        dot(uPresent.uRow1.xyz, linear),
                        dot(uPresent.uRow2.xyz, linear));
  converted = max(converted, vec3(0.0));

  if (uPresent.uParameters.x < 0.5) {
    oColor = vec4(pqFromNits(converted * uPresent.uParameters.z), 1.0);
  } else if (uPresent.uParameters.x < 1.5) {
    // Extended linear: the target is RGBA16F in an extended-linear P3 colour space, so 1.0 is SDR
    // white and values above it are the display's headroom. No encode, no clip.
    oColor = vec4(converted, 1.0);
  } else {
    oColor = vec4(linearToSrgb(clamp(converted, 0.0, 1.0)), 1.0);
  }
}
