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

// The fragment half of the instanced quad pass, evaluated as a **gradient** rather than a solid colour.
//
// ## Why this is not a branch inside `quad.frag`
//
// `quad.frag` already has one branch on the fifth instance attribute -- solid or textured -- and adding a third
// evaluation to it would cost every solid rectangle in every frame a comparison against a uniform it never reads.
// More importantly the two programs want different uniform *blocks*: a texture needs a sampler at binding 0, while a
// gradient needs six matrix coefficients, two points, a radius, a tile mode and four stops. Putting both blocks in
// one program means both are bound for every draw, and a uniform block that cannot be omitted is a cost paid by the
// 99% of draws that fill a flat colour.
//
// So this is the same vertex stage (`quad.vert`, so the geometry, the coverage path and the y flip are shared) with a
// different fragment stage, and the renderer picks it from `DrawList::ShaderKind`.
//
// ## Where the gradient is evaluated
//
// `vTexCoord` is the quad's own coordinate in {0,1}^2, which is what `quad.frag` uses to locate a fragment inside the
// rectangle. For a gradient the fragment has to be located in the *gradient's* space instead, and the canvas has
// already composed the three transforms that get there: the shader's local matrix, the caller's transform and the
// device mapping. `uGradient.uMatrix` is that composition, so one 2x3 multiply per fragment puts the fragment
// where the gradient can be asked its colour.
//
// `vRectSize` is the rectangle's size in device pixels, which is how the quad's {0,1}^2 coordinate becomes a device
// position: `vTexCoord * vRectSize` plus the rectangle's origin is the fragment. The origin is not carried here
// because the composed matrix already includes it -- see `fillGradient` in `GraphiteCanvasBackend`, which folds the
// device-to-gradient mapping into one matrix precisely so the fragment stage needs no fourth value.

precision highp float;

// The quad's own coordinate, in {0,1}^2, and the rectangle's size in device pixels. Both arrive from `quad.vert`,
// which is why this program declares no attributes of its own.
layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec4 vParams;
layout(location = 3) in vec2 vRectSize;

layout(location = 0) out vec4 oColor;

// The gradient's parameters, at binding 2. A block rather than push constants because the RHI's push-constant
// emulation is a uniform buffer at binding 0 and this needs more room than one draw's worth of scalars.
//
// ## Why every member of this block is a four-component type
//
// The six coefficients of the composed matrix used to be declared as `float uInverseLocal[6]`, and that one declaration
// is what made this program paint flat **black** while every CPU-side trace reported two stops. In `std140` an array of
// scalars has its *element stride* rounded up to the base alignment of a `vec4`, so those six floats occupied
// ninety-six bytes rather than twenty-four and every member after them moved. The driver's own reflection, printed
// beside the host's with `CORAL_GL_TRACE_BLOCKS=1`, is what settled it:
//
//   shader:  uInverseLocal[0] offset=48 stride=16 size=6   uStopCount offset=148   block size 288
//   host:    inverseLocal     offset=36 (packed)           stopCount  offset=64    sizeof 208
//
// So the shader read `uStopCount` from byte 148 while the host wrote it to byte 64, found a zero, and `sampleStops`
// fell through to its first-stop fallback -- an all-zero premultiplied colour, which is the flat black every
// measurement kept reporting. The range was never the problem: the descriptor covered all 288 bytes.
//
// A `vec4` and an array of `vec4`s have a sixteen-byte stride under that same rule, so when *every* member is one the
// layout is simply "sixteen bytes per member, in declaration order" and there is no rule left to get wrong. The host
// struct mirrors the table below member for member and `static_assert`s each offset against it; a change to either
// side that is not made to both is a compile error or a visible trace line rather than a black rectangle.
//
//   | offset | member           | contents                                        |
//   |--------|------------------|-------------------------------------------------|
//   |      0 | `vec4 uPoints[2]`| one point per entry, in `xy`                    |
//   |     32 | `vec4 uShape`    | `x` is the radius; zero selects the linear form |
//   |     48 | `vec4 uMatrix[2]`| `(m00, m01, m10, m11)`, then `(tx, ty, ...)`    |
//   |     80 | `ivec4 uParams`  | `x` is `TileMode`, `y` the live stop count      |
//   |     96 | `vec4 uStopColor[4]` | `(offset, red, green, blue)` per stop       |
//   |    160 | `vec4 uStopAlpha[4]` | the alpha beside each stop                  |
//   |    224 |                  | the whole block, and a whole number of `vec4`s  |
layout(std140, set = 0, binding = 2) uniform GradientUniforms {
  // `uPoints[0]` is the linear gradient's start, or the radial gradient's centre; `uPoints[1]` is the linear
  // gradient's end, or a point on the radial gradient's radius. The last two lanes of each entry are unused and are
  // what the array's sixteen-byte stride costs either way -- so they are declared rather than implied.
  vec4 uPoints[2];
  // `x` is the radius, and zero is what tells the two gradients apart here. The radius is meaningless for a linear
  // gradient, which is why zero is the sentinel rather than a flag beside it.
  vec4 uShape;
  // Row major: `(m00, m01, m10, m11)` in the first entry and the translation `(tx, ty)` at the front of the second.
  // One 2x3 multiply per fragment puts the fragment where the gradient can be asked its colour.
  vec4 uMatrix[2];
  // `x` is `TileMode`: 0 clamp, 1 repeat, 2 mirror, 3 decal. `y` is how many of the four stops below are live.
  ivec4 uParams;
  // Four stops of (offset, red, green, blue), with the alpha beside them. Unpremultiplied on the way in, premultiplied
  // on the way out, because that is what `graphics::Color` holds and what the blend expects.
  vec4 uStopColor[4];
  vec4 uStopAlpha[4];
} uGradient;
// Applies the tile mode to a gradient coordinate. One function for both kinds, because "what happens outside the
// gradient's domain" is a property of the coordinate and not of how it was computed.
float tile(const float value, const int mode) {
  if (mode == 1) {
    // Repeat: the fractional part, with a negative value folded up so that `-0.25` becomes `0.75` rather than `-0.25`.
    return value - floor(value);
  }
  if (mode == 2) {
    // Mirror: the triangle wave. `abs(fract(v * 0.5) * 2.0 - 1.0)` is the standard form and it is continuous at
    // every integer, which is what makes adjacent tiles seam.
    float folded = abs(fract(value * 0.5) * 2.0 - 1.0);
    return folded;
  }
  if (mode == 3) {
    // Decal: outside the domain the gradient contributes nothing. The sentinel is negative and the caller tests for
    // it, because "no contribution" is not a coordinate and returning one would evaluate a stop.
    return value < 0.0 || value > 1.0 ? -1.0 : value;
  }
  // Clamp, the default and the only mode most callers use.
  return clamp(value, 0.0, 1.0);
}

// @return the stop colour at @p t, interpolated between the two stops that bracket it.
//
// A linear walk over at most four stops rather than a loop with an early exit: the count is a uniform, the driver
// unrolls four iterations, and a `break` in a fragment shader is a divergence the hardware handles worse than the
// arithmetic it saves.
//
// ## Why the colour is `.yzw` and not `.rgb`
//
// A stop is packed as `(offset, red, green, blue)`, so the *first* lane is the position along the gradient and the three
// after it are the colour. Reading `.rgb` therefore took the offset as the red channel, the red as the green and the
// green as the blue -- and dropped blue entirely. It is invisible on a grey ramp, which is exactly what the test used
// while this was being diagnosed: a black-to-white gradient has `red == green == blue`, so mistaking one channel for
// another changes nothing. A red-to-blue gradient is where it shows.
vec4 sampleStops(const float t) {
  vec4 previousStop = uGradient.uStopColor[0];
  float previousAlpha = uGradient.uStopAlpha[0].x;
  float previousOffset = previousStop.x;

  for (int i = 1; i < 4; ++i) {
    if (i >= uGradient.uParams.y) break;
    vec4 stop = uGradient.uStopColor[i];
    float stopOffset = stop.x;
    if (t <= stopOffset) {
      // The interpolation is in the *unpremultiplied* channel values, which is what the rasterizer does: it lerps
      // the four channels and premultiplies the result. Lerping premultiplied values would give a different colour
      // wherever two stops have different alphas, and the difference is most visible on the transparent end of a
      // fade -- the one place a caller looks.
      float span = stopOffset - previousOffset;
      float weight = span > 0.0 ? (t - previousOffset) / span : 0.0;
      vec4 blended = mix(vec4(previousStop.yzw, previousAlpha), vec4(stop.yzw, uGradient.uStopAlpha[i].x), weight);
      return vec4(blended.rgb * blended.a, blended.a);
    }
    previousStop = stop;
    previousAlpha = uGradient.uStopAlpha[i].x;
    previousOffset = stopOffset;
  }

  // Past the last stop, which is where a `t` of one lands and where a clamped coordinate beyond the domain ends up.
  vec4 last = uGradient.uStopColor[max(uGradient.uParams.y - 1, 0)];
  float lastAlpha = uGradient.uStopAlpha[max(uGradient.uParams.y - 1, 0)].x;
  return vec4(last.yzw * lastAlpha, lastAlpha);
}

void main() {
  // The fragment, in the gradient's own space. `vTexCoord` is {0,1}^2 across the *grown* quad -- see the coverage below
  // for why it is grown -- so scaling by the rectangle's size gives device pixels offset by the feather, and the composed
  // matrix takes it from there. `vParams.y` is zero for a quad that was not grown, which is what makes this the plain
  // `vTexCoord * vRectSize` in the hard-edged case.
  vec2 device = vTexCoord * vRectSize - vParams.y;
  // ## Row major, which is what the block says and what the host writes
  //
  // `uMatrix[0]` is `(m00, m01, m10, m11)` -- row major, see the layout table above -- so the coefficient of `device.y`
  // in `x` is `.y` and the coefficient of `device.x` in `y` is `.z`. These two were the other way round, which is
  // invisible for a matrix with zero off-diagonals: a scale and a translation gave the same answer either way, and every
  // test and every corpus document up to this point used one. A *rotation* does not, and the one document that has one
  // (`gradientTransform="translate(50 50) rotate(90) scale(50)"`) came out as a displaced gradient -- a flat clamped
  // stop where the reference has a ramp -- because the transposed matrix put the gradient's centre somewhere else.
  vec2 gradientSpace = vec2(
          uGradient.uMatrix[0].x * device.x + uGradient.uMatrix[0].y * device.y + uGradient.uMatrix[1].x,
          uGradient.uMatrix[0].z * device.x + uGradient.uMatrix[0].w * device.y + uGradient.uMatrix[1].y);

  // The gradient's coordinate: the projection onto the line for a linear gradient, or the distance from the centre
  // for a radial one. `uShape.x` of zero is what selects the linear form, and it is zero rather than a flag because
  // the radius is meaningless for a linear gradient either way.
  float t;
  if (uGradient.uShape.x > 0.0) {
    t = length(gradientSpace - uGradient.uPoints[0].xy) / uGradient.uShape.x;
  } else {
    vec2 axis = uGradient.uPoints[1].xy - uGradient.uPoints[0].xy;
    float lengthSquared = dot(axis, axis);
    t = lengthSquared > 0.0 ? dot(gradientSpace - uGradient.uPoints[0].xy, axis) / lengthSquared : 0.0;
  }

  float tiled = tile(t, uGradient.uParams.x);
  vec4 gradient = sampleStops(tiled);

  // ## The paint contributes its *alpha* and nothing else, which is the documented contract
  //
  // `Paint::shader` says it in one line: when a shader is set it "supplies the colour at every covered pixel and `color`
  // is used only for its alpha, which is what lets a caller fade a gradient without rebuilding it -- and what the SVG
  // layer relies on when it folds `opacity` into the paint."
  //
  // This used to multiply the gradient by the paint's premultiplied *colour* as well, which is the same picture only
  // when that colour is white. The SVG layer leaves it at the element's `fill`, and for `fill="url(#...)"` that is the
  // default -- black -- so every gradient-filled SVG shape came out flat black: `radialGradient.svg` and the
  // gradient-filled body of `complex2.svg` were the two largest differences in the demo's SVG panel, and neither was a
  // blend or alpha defect. The rasterizer had the contract right, so the two backends disagreed on a documented
  // behaviour rather than on an arithmetic detail.
  //
  // The alpha scales the whole premultiplied colour, so a faded gradient stays a valid premultiplied colour for the
  // blend unit. The rasterizer scales only the alpha channel (`RasterCanvasBackend.cpp`'s `sampled.alpha *= shadeAlpha`),
  // which is the same picture for an opaque paint -- every SVG document in the corpus -- and differs for a faded one by
  // leaving the colour unscaled. That divergence is recorded here rather than copied: the premultiplied form is the one
  // the blend state is written for, and the rasterizer's line is the one to change if the two are ever brought
  // together.
  vec4 fill = gradient * vColor.a;

  // The rectangle's own coverage, which every program sharing `quad.vert` applies the same way. A gradient painted
  // over a rounded rectangle has to honour the rounding, and the coverage is what does it.
  float coverage = 1.0;
  if (vParams.z > 0.5) {
    // The rounded-box distance, duplicated from `quad.frag` rather than shared: glslang has no include mechanism, and
    // a second copy is the price of a second fragment stage. The two must agree, so a change to one is a change to both
    // -- which the comment here is the only guard against.
    //
    // ## The interior term, which is what made this rectangle half transparent
    //
    // The distance has to be *negative inside* the box, and `length(max(q, 0.0))` alone is not: it measures the
    // distance to the inner rectangle, which is zero for every point along the straights and anywhere inside them. So
    // every interior fragment reached the filter below with a distance of exactly zero and got
    // `clamp(0.5 - 0.0 / feather, 0, 1)` -- a coverage of one half. The whole gradient then drew at 50% alpha, which
    // reads back as a correct-looking colour beside a half-alpha channel because the readback is unpremultiplied.
    //
    // `min(max(q.x, q.y), 0.0)` is the missing term: negative by the depth of the point inside the inner rectangle and
    // zero outside it, so adding it gives the signed distance the comment above `quad.frag`'s copy already describes --
    // negative inside, zero on the boundary, positive outside. With it, an interior fragment is at least half a feather
    // inside and the filter returns a full one; a fragment on the edge still gets its half, which is what a covering
    // filter must say.
    // ## The rectangle is not the quad
    //
    // The vertex stage grows an antialiased quad by its feather (`vParams.y`) so that the rasterizer generates the edge
    // pixels at all -- the fill rule keeps a pixel whose centre falls outside the triangle, and the coverage below is
    // what should decide such a pixel. `vRectSize` is therefore the *grown* size, the true rectangle is
    // `vRectSize - 2 * feather`, and the two are centred on each other, so `centred` needs no correction while
    // `halfSize` -- the box the distance is measured to -- does.
    vec2 size = vRectSize - 2.0 * vParams.y;
    vec2 halfSize = 0.5 * size;
    vec2 centred = vTexCoord * vRectSize - 0.5 * vRectSize;
    float clamped = min(vParams.x, min(halfSize.x, halfSize.y));
    vec2 distanceToInner = abs(centred) - (halfSize - clamped);
    float distance = length(max(distanceToInner, vec2(0.0))) + min(max(distanceToInner.x, distanceToInner.y), 0.0) -
                     clamped;
    coverage = vParams.y <= 0.0 ? (distance <= 0.0 ? 1.0 : 0.0) : clamp(0.5 - distance / vParams.y, 0.0, 1.0);
  }

  oColor = fill * coverage;
}
