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

// The signed-distance-field fragment stage: the image quad, sampling a field instead of a picture.
//
// The field's red channel is 128 at the glyph outline, above it inside and below it outside (see
// `graphics::SdfGlyph`). A coverage value is derived from the distance, so the same texture serves the
// fill and, in a later version, an outline or a shadow: those are different bands of the same value,
// not different glyph rasterisations.
//
// This program exists separately from `image.frag` for the reason every other image program does: the
// fragment stage is the only difference, the vertex stage and the layout are the quad's, and a branch
// in `image.frag` would be paid by every photograph and every coverage blit in the frame.

precision highp float;

layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec4 vColor;

// The sub-rectangle of the field this command samples, in normalized texture coordinates.
layout(location = 2) in vec4 vParams;
layout(location = 4) in vec4 vSdf;
layout(location = 5) in vec4 vSdfOutlineColor;
layout(location = 6) in vec4 vSdfShadowColor;
layout(location = 7) in vec4 vSdfPattern;
layout(location = 8) in float vSdfFillAlpha;

layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uTexture;

void main() {
  vec2 uv = vParams.xy + vTexCoord * vParams.zw;
  float distanceValue = texture(uTexture, uv).r;

  // The half-texel band around 0.5 is the antialiased edge.
  float aa = 0.02;
  float fill = smoothstep(0.5 - aa, 0.5 + aa, distanceValue);

  vec4 paint = vec4(vColor.rgb * vColor.a, vColor.a);
  // Procedural border decoration: stripes or dots modulate the fill. This is the first step of the
  // border work; a texture-based lace pattern needs a second sampler and is a later extension.
  if (vSdfPattern.w > 0.0) {
    vec2 centred = uv - 0.5;
    float angle = radians(vSdfPattern.z);
    vec2 rotated = vec2(centred.x * cos(angle) - centred.y * sin(angle),
                        centred.x * sin(angle) + centred.y * cos(angle));
    float mask = 0.0;
    if (vSdfPattern.x < 0.5) {
      mask = step(0.5, fract((rotated.x + rotated.y) * vSdfPattern.y));
    } else {
      vec2 grid = fract(rotated * vSdfPattern.y) - 0.5;
      mask = 1.0 - step(0.25, length(grid));
    }
    paint *= (1.0 - vSdfPattern.w * mask);
  }
  vec4 result = paint * fill * vSdfFillAlpha;

  // Parameters are coherent across a glyph, so these branches cost a predicate rather than a divergent
  // path. The default fill has no outline, glow or shadow and therefore skips every extra tap and
  // smoothstep below -- which is what keeps the common subtitle case close to the coverage route.
  if (vSdf.x > 0.0) {
    float outlineInner = 0.5 - vSdf.x;
    float outline = clamp(smoothstep(outlineInner - aa, outlineInner + aa, distanceValue) - fill,
                          0.0, 1.0);
    result += vec4(vSdfOutlineColor.rgb * vSdfOutlineColor.a, vSdfOutlineColor.a) * outline;

    if (vSdf.w > 0.0) {
      float glowInner = 0.5 - vSdf.x - vSdf.w;
      float glow = clamp(smoothstep(glowInner - aa, glowInner + aa, distanceValue) - fill - outline,
                         0.0, 1.0);
      result += vec4(paint.rgb, paint.a) * glow * 0.35;
    }
  }

  if (vSdf.y != 0.0 || vSdf.z != 0.0) {
    float shadowDistance = texture(uTexture, uv - vSdf.yz).r;
    float shadow = smoothstep(0.5 - aa, 0.5 + aa, shadowDistance) * (1.0 - fill);
    result += vec4(vSdfShadowColor.rgb * vSdfShadowColor.a, vSdfShadowColor.a) * shadow;
  }

  oColor = clamp(result, 0.0, 1.0);}
