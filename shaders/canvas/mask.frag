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

// The coverage-multiply pass behind `Canvas::maskLayer`: sample a mask and a layer, turn the mask into
// one coverage value per pixel, and scale the premultiplied layer by it.
//
// It pairs with `composite.vert`, which draws the one quad covering the whole attachment and hands over
// the texture coordinate - there is no `mask.vert` for the reason that file gives. See `present.vert`
// for the conventions this directory follows, in particular that `#version` has to be the first line
// ahead of the licence header.
//
// ## Why this is a scale and not a blend
//
// `Canvas.cpp`'s CPU implementation of the same operation does exactly one thing per pixel: it scales
// the layer's alpha by the coverage and writes the layer back. Because the layer is stored
// premultiplied, scaling its alpha scales its stored colour by the same factor, so the operation is a
// single multiply of all four components and nothing else - no compositing, no alpha weighting, no
// destination. Masking a layer is not "draw this over that"; it is a coefficient applied to a picture.
//
// ## Why the mask's colour has to be unpremultiplied
//
// This is the one place where a plausible implementation is silently wrong, so it is worth stating in
// full. A mask is stored premultiplied like every other bitmap in this renderer - `Canvas::clipPath`
// writes `(coverage, coverage, coverage, coverage)`, which is white at that coverage and is exactly
// what a premultiplied pixel looks like. `kLuminance` wants the mask's *colour* weighted by Rec. 709
// and then multiplied by its alpha, and the colour it wants is the unpremultiplied one.
//
// Taking the dot product of the stored channels instead would confound the two: a white mask at 50%
// coverage stores (0.5, 0.5, 0.5, 0.5), whose dot product with the luminance weights is 0.5 - and then
// multiplying by the alpha again gives 0.25, a mask that is half as strong as it should be, with the
// error growing as coverage falls. The picture that produces looks like a slightly-too-transparent
// mask rather than like a bug, which is why `Canvas.cpp` says the same thing at the site where it
// unpremultiplies (`libs/graphics/src/Canvas.cpp`, the `MaskMode::kLuminance` branch).
//
// The division is guarded rather than assumed: a premultiplied pixel with zero alpha has zero colour
// by definition, so the quotient is 0/0, and zero is the answer the representation already implies.

// ## Why the locals below are not `const`
//
// glslang refuses a `const` declaration whose initializer is not a constant expression in the ES
// profile - "'non-constant initializer' : not supported with this profile: es" - and that is the ES
// profile's rule rather than a version floor, so the answer is to drop the qualifier rather than to
// raise `#version`. `quad.vert` carries the longer note and the glslang reference. The coefficient
// vector below *is* `const`, because its initializer is a constant expression.

precision highp float;

// The quad's texture coordinate - the *layer's* coordinate. See the note on `uMaskScale` below for how
// the mask's own coordinate is derived from it.
layout(location = 0) in vec2 vTexCoord;

// Premultiplied, and scaled by the coverage: the whole result of this pass.
layout(location = 0) out vec4 oColor;

// The layer being masked, **premultiplied**.
layout(set = 0, binding = 0) uniform sampler2D uLayer;

// The mask, **premultiplied**, and in the mask's own resolution.
//
// The sampler's filter is the caller's, not this shader's, and it has to be **nearest** to reproduce
// `Canvas::maskLayer`: that code maps a layer pixel to `(x * scaleX, y * scaleY)` truncated to an
// integer, which is nearest-neighbour on a grid-aligned stretch. A linear filter here would give a
// smoother mask than the CPU path and a different picture on every edge, and the shader cannot choose
// the filter - a `sampler2D` carries whatever the texture and sampler were created with. Stating it
// here is the only place it can be stated.
layout(set = 0, binding = 1) uniform sampler2D uMask;

// The mask's parameters.
//
// The mask's coordinate is `vTexCoord * uMaskScale + uMaskOffset` rather than `vTexCoord`, because a
// caller is allowed to render its mask at its own resolution and `Canvas::maskLayer` stretches such a
// mask over the layer instead of ignoring it or clipping it at the top-left. A scale-and-offset rather
// than a rect because that is all the stretch is, and because it lets a mask cover part of the layer -
// which is what a mask on a `drawImage`-sized layer actually does.
//
// The alternative, requiring the caller to render the mask at the layer's resolution, would make the
// common case (a small mask over a large layer) cost a full-size intermediate for no gain.
layout(std140, set = 0, binding = 2) uniform MaskUniforms {
  vec2 uMaskScale;
  vec2 uMaskOffset;

  // The mode, numerically equal to `graphics::MaskMode` in `libs/graphics/include/graphics/Enums.h`:
  // 0 is `kAlpha`, 1 is `kLuminance`.
  //
  // The two answers agree for the commonest mask - white shapes on nothing, where white is opaque - and
  // disagree everywhere a mask varies its colour without varying its alpha. A black-to-white gradient
  // has opaque stops, so its alpha is one across the whole thing and only its luminance carries the
  // shape; read as an alpha it is a rectangle. That is the picture that looks deliberate, which is why
  // the mode is explicit rather than inferred from the mask.
  //
  // An `int` rather than a float so the comparison below is exact. A value that is neither 0 nor 1 is
  // treated as `kLuminance` here, matching `Canvas.cpp`'s `if (mode == MaskMode::kLuminance)` inverted;
  // a caller should have validated it against `MaskMode` first.
  int uMaskMode;
} uMaskParams;

// Rec. 709's coefficients: the ones SVG 1.1 states for a `mask`, and the ones `Canvas::maskLayer` and
// `SvgFilter.cpp` use. Deliberately *not* the Rec. 601 coefficients `composite.frag` uses for the
// hue/saturation/luminosity blend family - those answer a different question. See the note there.
const vec3 kLuminanceCoefficients = vec3(0.2125, 0.7154, 0.0721);

void main() {
  // The layer texel, premultiplied, at the layer's own coordinate.
  vec4 layer = texture(uLayer, vTexCoord);

  // The mask texel, premultiplied, at the mask's coordinate. One fetch either way: the two modes read
  // the same texel and differ only in which of its components they believe.
  vec4 mask = texture(uMask, vTexCoord * uMaskParams.uMaskScale + uMaskParams.uMaskOffset);

  // `kAlpha`: the mask's own alpha, which for a mask rasterised as coverage is the coverage itself.
  float coverage = mask.a;

  if (uMaskParams.uMaskMode == 1) {
    // `kLuminance`: the mask's colour weighted by Rec. 709, and then multiplied by its alpha.
    //
    // The alpha factor is not redundant. A black pixel that is fully transparent and a black pixel that
    // is fully opaque both have no luminance and both must hide the layer - but a *white* transparent
    // pixel must hide it too, and luminance alone cannot say so.
    vec3 colour = mask.a > 0.0 ? mask.rgb / mask.a : vec3(0.0);
    coverage = dot(colour, kLuminanceCoefficients) * mask.a;
  }

  // One multiply, on premultiplied components, which is the whole operation. Scaling the layer's alpha
  // by the coverage scales its stored colour by the same factor because it is premultiplied - the same
  // property `quad.frag` relies on to fold an antialiased edge into an alpha, and the reason the
  // coverage does not have to be unpacked into a per-channel weight here.
  //
  // A coverage of exactly one is a multiply by one, so the pass is correct on the fully-masked interior
  // as well; `Canvas.cpp` skips those pixels rather than multiplying them, which is an optimisation for
  // a CPU loop and not part of the operation.
  oColor = layer * coverage;
}
