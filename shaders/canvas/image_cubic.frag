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

// The image fragment stage with the Mitchell-Netravali cubic family, B and C read from the canvas block.
//
// ## Why one program for every kernel, while `image.frag` stays separate
//
// The filter is a property of the draw, so B and C arrive as a uniform -- written once per image batch, which is the only
// granularity a uniform has and is fine because every image already owns a batch (one texture per batch). That is what
// makes any `CubicResampler` honest here, rather than only the two the renderer used to ship.
//
// What is deliberately *not* merged is the cheap path. `image.frag` samples four texels; this one samples sixteen, and a
// shader's register footprint is paid by every fragment that runs it. Folding the cubic branch into `image.frag` would
// have made every nearest and bilinear image in the frame carry the cubic path's registers -- the trade Skia refuses as
// well. So: two programs, the hot one small, the expensive one bound only for a batch that asked for it.

precision highp float;

layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec4 vParams;

layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uTexture;

// The same block `quad.vert` declares, at the same binding: the two stages must agree on its shape, and a second block at
// a second binding would be a second thing to keep in step. `uPadding` exists so that `uImageFilter` lands at offset 32,
// which is where std140 puts a `vec4` after three `vec2`s.
layout(std140, set = 0, binding = 1) uniform CanvasUniforms {
  vec2 uViewport;
  vec2 uScale;
  vec2 uOffset;
  vec2 uPadding;
  vec4 uImageFilter;
  // Declared but unread here: the block must match `image.frag` and `quad.vert` member for member, or the two stages
  // fail to link under OpenGL.
  float uOutputPq;
} uCanvas;

float cubicWeight(float x, float b, float c) {
  float ax = abs(x);
  if (ax < 1.0) {
    return ((12.0 - 9.0 * b - 6.0 * c) * ax * ax * ax + (-18.0 + 12.0 * b + 6.0 * c) * ax * ax + (6.0 - 2.0 * b)) / 6.0;
  }
  if (ax < 2.0) {
    return ((-b - 6.0 * c) * ax * ax * ax + (6.0 * b + 30.0 * c) * ax * ax + (-12.0 * b - 48.0 * c) * ax +
            (8.0 * b + 24.0 * c)) / 6.0;
  }
  return 0.0;
}

void main() {
  vec4 paint = vec4(vColor.rgb * vColor.a, vColor.a);
  vec2 uv = vParams.xy + vTexCoord * vParams.zw;

  float b = uCanvas.uImageFilter.x;
  float c = uCanvas.uImageFilter.y;

  // Sixteen taps at level 0, weighted by the separable kernel and normalised per axis so that a flat region stays flat
  // where the taps clamp to the edge. Level 0 rather than a mip level: this is a magnification filter in practice, and a
  // minified cubic draw is better served by the mipmapped bilinear path than by sixteen taps on one level.
  vec2 size = vec2(textureSize(uTexture, 0));
  vec2 pixel = uv * size - 0.5;
  vec2 base = floor(pixel);
  vec2 fraction = pixel - base;

  float wx[4];
  float wy[4];
  float sumX = 0.0;
  float sumY = 0.0;
  for (int i = 0; i < 4; ++i) {
    wx[i] = cubicWeight(float(i) - 1.0 - fraction.x, b, c);
    wy[i] = cubicWeight(float(i) - 1.0 - fraction.y, b, c);
    sumX += wx[i];
    sumY += wy[i];
  }

  vec4 sum = vec4(0.0);
  for (int j = 0; j < 4; ++j) {
    for (int i = 0; i < 4; ++i) {
      vec2 coord = clamp(base + vec2(float(i) - 1.0, float(j) - 1.0), vec2(0.0), size - 1.0);
      sum += texelFetch(uTexture, ivec2(coord), 0) * ((wx[i] / sumX) * (wy[j] / sumY));
    }
  }

  oColor = sum * paint;
}
