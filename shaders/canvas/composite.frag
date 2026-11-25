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

// The fragment half of the layer composite: combine a saved layer with the target it was rendered
// into, at a global alpha and under one selectable blend mode. This is the pass behind
// `Canvas::saveLayer`'s matching pop.
//
// It pairs with `composite.vert`, which draws the one quad that covers the whole attachment. See
// `present.vert` for the conventions this directory follows - in particular that `#version` has to be
// the first line, ahead of the licence header.
//
// ## When not to use this shader at all - the renderer's rule
//
// A fixed-function blend happens while the fragment is written; it needs no second texture, no second
// pass and no snapshot of the target. Wherever a mode can be expressed as `srcFactor * src` combined
// with `dstFactor * dst`, the renderer takes that path and never loads this shader. So the first
// thing to say about this file is what it is *not* for.
//
// Fifteen modes have a coefficient pair in `graphics::BlendModeAsCoeff` - `kClear` through `kScreen` -
// and every coefficient they use has a matching `BlendFactor` in
// `libs/graphics/backend/include/backend/Types.h`, colour terms included. All fifteen are therefore
// expressible as `BlendState` components:
//
//     kSrcOver   One, OneMinusSrcAlpha          kModulate  DstColor, Zero
//     kDstIn     Zero, SrcAlpha                 kScreen    One, OneMinusSrcColor
//     kPlus      One, One                       ...and the twelve Porter-Duff operators
//
// ES 3.0's `glBlendEquationSeparate` offers Add, Subtract, ReverseSubtract, Min and Max, which covers
// all of them: none needs an equation other than Add. `BlendState::premultipliedBlend()` is the one
// the canvas uses for the common case.
//
// **This shader consequently implements no Porter-Duff operator at all.** A second copy of the
// twelve-operator table here would be a second thing to keep in step with `BlendModeAsCoeff`, it
// would cost a full-attachment snapshot to run, and it would be slower than the blend unit it
// duplicated. The table lives in one place, on the CPU side, and the modes that reach this file are
// exactly the ones that cannot go there.
//
// What is left is fourteen modes that no blend factor can name, because a blend factor cannot be a
// computed colour:
//
//   * ten separable colour functions - `kOverlay`, `kDarken`, `kLighten`, `kColorDodge`,
//     `kColorBurn`, `kHardLight`, `kSoftLight`, `kDifference`, `kExclusion`, `kMultiply` - which
//     compute a new colour from a destination channel and a source channel;
//   * four non-separable ones - `kHue`, `kSaturation`, `kColor`, `kLuminosity` - which move between
//     hue, saturation and luminosity and are not a channel-at-a-time operation at all.
//
// Those fourteen are what this pass is for. A caller that routes a coefficient mode here gets a
// defined picture rather than an undefined one (see the note on `uBlendMode`), but that is a caller
// bug and the picture will be source-over whatever the mode asked for.
//
// ## Which modes are exact, and the one approximation the caller owns
//
// Every mode this file *does* implement is computed to its formula, on colours unpremultiplied where
// the formula is defined that way and composited source-over, which is what the compositing model
// prescribes. Nothing in the switch below is a fitted or truncated version of its mode.
//
// The modes are transcribed from `libs/graphics/include/private/graphics/BlendMode.h`, which is the
// CPU rasterizer this pass has to agree with. That file is the authority here - not Skia, not the W3C
// compositing model - because a canvas that drew one picture with hardware acceleration on and a
// different one with it off would be worse than one that is consistently wrong. Where those
// references disagree, this shader follows the rasterizer, and the disagreements are written down
// rather than left for a reader to rediscover:
//
//   * `graphics::Color.h` documents Skia's *premultiplied* forms for `kDarken`, `kLighten`,
//     `kDifference` and `kExclusion` (`rc = s + d - max(s*da, d*sa)` and so on), and Skia's `kMultiply`
//     as `r = s*(1-da) + d*(1-sa) + s*d`. `BlendMode.h` computes the W3C colour function for each and
//     composites it source-over. The two agree exactly when both inputs are opaque - which is why
//     `TheCoefficientTableAgreesWithTheImplementations` passes - and differ when either is
//     translucent. This shader implements `BlendMode.h`.
//   * `kHue`, `kSaturation`, `kColor` and `kLuminosity` in `BlendMode.h` implement "move the
//     destination toward the source in the attribute the mode names", which is not the W3C
//     definition. Two consequences follow, and this shader reproduces both deliberately: `kHue` and
//     `kSaturation` compute the *same* picture there (their cases differ only by a `setLuminosity` to
//     the luminosity the preceding `setSaturation` already produced, which is a no-op for an in-gamut
//     colour), and `kLuminosity` is the identity on an in-gamut destination. Reproducing them keeps
//     the two paths in step, and
//     `test_Canvas.cpp`'s `TheNonSeparableModesMoveBetweenHueSaturationAndLuminosity` pins the
//     current answers - so changing either path alone would break that test rather than fix it.
//     Reconciling all four with the specification is a decision for whoever owns both paths, not one
//     a shader should take on its own.
//
// The approximation is not in a formula; it is in what this pass is not told.
//
//   * **There is no coverage input.** A whole-target composite has one alpha per pixel and it is the
//     layer's own, already carrying whatever coverage the layer was rasterised with. That is the
//     specification's standard treatment for the colour functions. A renderer that needed them exact
//     on a soft edge would blend per subpixel sample, which one coverage value per pixel cannot
//     express; the Porter-Duff operators that a partially covered edge actually needs never come
//     through here at all, because they are the blend unit's.
//   * **The destination is a snapshot.** If the caller writes to the target between taking the
//     snapshot and running this pass, the composite blends against a stale picture. The snapshot must
//     be taken after the last write that precedes the composite, and this shader cannot check that -
//     nor can the RHI, because it cannot see the caller's pass ordering across a texture read.
//   * **The global alpha is applied in the shader, in float.** The CPU path quantises it through
//     `(alpha * globalAlpha + 127) / 255` in bytes; this path multiplies in float. A caller
//     comparing the two paths byte for byte will see that rounding, one unit on some pixels.

// ## Why the locals below are not `const`, and the globals are
//
// glslang refuses a `const` declaration whose initializer is not a constant expression in the ES
// profile - "'non-constant initializer' : not supported with this profile: es" - and that is the ES
// profile's rule rather than a version floor, so the answer is to drop the qualifier rather than to
// raise `#version`. `quad.vert` carries the longer note and the glslang reference. The mode
// constants below *are* `const`, because their initializers are literals, which is the case `const`
// is for.

precision highp float;

// See `composite.vert`: the quad's texture coordinate, in {0,1}^2 across the whole attachment.
layout(location = 0) in vec2 vTexCoord;

// Premultiplied out. Both inputs are premultiplied too, so the colour functions unpremultiply once,
// blend, and are repremultiplied by the output alpha in the same expression.
layout(location = 0) out vec4 oColor;

// The layer, **premultiplied**. Produced by the passes that drew the layer, which write premultiplied
// colour - `quad.frag` says so, and it is why a coverage can be folded into an alpha.
//
// Binding 0 is free for a *sampler* and reserved for a *block*: the OpenGL backend emulates push
// constants through a uniform buffer at `kPushConstantBinding = 0`
// (`libs/graphics/backend/include/private/backend/gl/GlDevice.h`), so the parameter block below is at
// binding 2. A sampler is a texture unit and a block is a uniform-block binding point, which is why
// the same number can be used for both without collision.
layout(set = 0, binding = 0) uniform sampler2D uSource;

// A readable copy of the target, **premultiplied**, in the target's own orientation.
//
// The orientation matters and is the one thing that can silently go wrong here: this sampler and
// `uSource` are indexed by the same `vTexCoord`, so if the caller took the snapshot through a blit
// that flipped it, every fragment blends with its own reflection. That looks like a smear or a mirror
// ghost rather than like a flipped image, which is why it is worth stating here rather than leaving to
// a comment in the caller: snapshot with the same orientation the layer was rendered in.
//
// The alternative to having this sampler at all is framebuffer fetch
// (`GL_EXT_shader_framebuffer_fetch`), which is not in ES 3.0 and is an extension on the devices that
// have it. Reading the destination is the portable way, and one blit of the attachment is its price -
// which is the real cost of the fourteen modes this file serves, not the arithmetic.
layout(set = 0, binding = 1) uniform sampler2D uDestination;

// The pass's parameters.
//
// Binding 2 rather than 0 or 1: 0 is the backend's push-constant block binding, and binding 1 is
// `CanvasUniforms`, the per-frame block the device-space stages declare. This block is per composite
// and has nothing to do with either.
//
// `int` in a uniform block rather than a float or a packed vector: the mode is an enumerator, the
// comparison below is exact, and a float would need a tolerance on every case. In std140 an `int` is
// four bytes, so a block of one float and one int is eight.
layout(std140, set = 0, binding = 2) uniform CompositeUniforms {
  // The layer's global alpha, in the unit interval, from the `Paint` the layer was saved with.
  // Applied to the premultiplied source, which scales its colour and its alpha together - the same
  // operation `Canvas::compositeLayer` performs per pixel when it composites a layer on the CPU.
  float uAlpha;

  // The mode, numerically equal to `graphics::BlendMode` in
  // `libs/graphics/geometry/include/geometry/Color.h`.
  //
  // The numbers are spelled out in the constants below rather than read from anywhere, because GLSL
  // cannot see a C++ enumerator. That makes the two a pair to be kept in step by hand, and the
  // failure mode is worth naming: inserting a mode in the middle of `BlendMode` shifts every later
  // enumerator by one, this shader goes on compiling, and the symptom is one blend mode rendering as
  // its neighbour. The list below is in the enum's own order, one per line, so a diff against
  // `Color.h` is mechanical.
  //
  // A mode this file does not serve - one of the fifteen coefficient modes, or a value outside the
  // enum - is treated as `kSrcOver` by `colourFunction`'s default, not left undefined. A caller
  // should have routed it to `BlendState` instead
  // (`graphics::raster::isBlendModeSupported` answers whether a value is a mode at all), but a defined
  // picture beats an undefined one when the alternative is a NaN spread through the framebuffer.
  int uBlendMode;
  // Linear-light text layer parameters. The layer's RGB is per-channel coverage; text color is the
  // source endpoint, and gamma/contrast shape the composite against the readable destination.
  float uTextR;
  float uTextG;
  float uTextB;
  float uGamma;
  float uContrast;
  int uLinearText;
} uComposite;

// The blend modes, in `graphics::BlendMode`'s order.
//
// All twenty-nine are listed even though this file switches on only the last fourteen. The list is
// what pins the numbers to the enum, and a list with a hole in it would be a list nobody could check
// against `Color.h` at a glance.
const int kClear = 0;
const int kSrc = 1;
const int kDst = 2;
const int kSrcOver = 3;
const int kDstOver = 4;
const int kSrcIn = 5;
const int kDstIn = 6;
const int kSrcOut = 7;
const int kDstOut = 8;
const int kSrcATop = 9;
const int kDstATop = 10;
const int kXor = 11;
const int kPlus = 12;
const int kModulate = 13;
const int kScreen = 14;
const int kOverlay = 15;
const int kDarken = 16;
const int kLighten = 17;
const int kColorDodge = 18;
const int kColorBurn = 19;
const int kHardLight = 20;
const int kSoftLight = 21;
const int kDifference = 22;
const int kExclusion = 23;
const int kMultiply = 24;
const int kHue = 25;
const int kSaturation = 26;
const int kColor = 27;
const int kLuminosity = 28;

// The first mode this file serves, and the last that the blend unit does. Everything at or below
// `kScreen` has a coefficient pair and belongs to `BlendState`; see the file-level note.
const int kLastBlendUnitMode = kScreen;

// The premultiplied colour as an unpremultiplied one, which is what the colour functions are defined
// on.
//
// The guard is not defensive. A premultiplied colour with zero alpha *must* have zero colour for the
// representation to be consistent, so the quotient there is 0/0; zero is the only value the
// representation allows and returning it is exact rather than a fudge. Letting the NaN through would
// spread it into the framebuffer, where it shows as a single black or white pixel that no amount of
// reading the blend code explains.
vec3 unpremultiplied(const vec4 colour) {
  if (colour.a <= 0.0) {
    return vec3(0.0);
  }
  return colour.rgb / colour.a;
}

// One branch of hard light, which overlay reuses with its arguments the other way round.
//
// A `mix` over a `step` rather than a branch, because the two arms are the same shape and a
// component-wise `if` would either be three branches or a lie. Both arms agree at exactly one half -
// `2 * 0.5 * base` and `1 - 2 * 0.5 * (1 - base)` are both `base` - so the boundary convention
// `BlendMode.h` writes as `<=` and `step` writes as `>=` cannot be told apart, which is why the
// branchless form is faithful rather than merely equivalent.
vec3 hardLight(const vec3 base, const vec3 blend) {
  return mix(2.0 * blend * base, 1.0 - 2.0 * (1.0 - blend) * (1.0 - base), step(vec3(0.5), blend));
}

// Brighten the destination by the source, one channel at a time. The endpoints are handled first:
// `1 - s` divides by zero at `s == 1`, where the limit is 1. Per channel rather than per vector,
// because `BlendMode.h` calls its channel function once per channel and the guards are genuinely per
// channel - a red channel at 1 does not stop the green one from dodging.
float colorDodgeChannel(const float d, const float s) {
  if (d <= 0.0) {
    return 0.0;
  }
  if (s >= 1.0) {
    return 1.0;
  }
  return min(1.0, d / (1.0 - s));
}

// The dual, with the same care at the other endpoint.
float colorBurnChannel(const float d, const float s) {
  if (d >= 1.0) {
    return 1.0;
  }
  if (s <= 0.0) {
    return 0.0;
  }
  return 1.0 - min(1.0, (1.0 - d) / s);
}

// A gentler overlay. The `d <= 0.25` break is the specification's; it is what keeps the curve's slope
// continuous where the polynomial meets the square root, and dropping it - using `sqrt(d)`
// throughout - shows as a visible band a quarter of the way up a soft-light gradient.
float softLightChannel(const float d, const float s) {
  if (s <= 0.5) {
    return d - (1.0 - 2.0 * s) * d * (1.0 - d);
  }
  float curve = (d <= 0.25) ? ((16.0 * d - 12.0) * d + 4.0) * d : sqrt(d);
  return d + (2.0 * s - 1.0) * (curve - d);
}

// The separable colour functions this file serves: one function of a destination channel and the
// source channel, applied the same way to red, green and blue. That is what "separable" means, and it
// is why the per-channel modes build a vec3 from one call each rather than being written as a vector
// expression - a mode whose guard is per channel cannot be one anyway.
//
// Both colours are in the unit interval and unpremultiplied.
vec3 separableBlend(const int mode, const vec3 d, const vec3 s) {
  switch (mode) {
    case kOverlay:
      // Multiply where the *destination* is dark, screen where it is light. That the choice is made
      // on the destination is the whole difference from hard light, and it is expressed by calling
      // the shared function with the arguments the other way round.
      return hardLight(d, s);

    case kDarken:
      return min(s, d);

    case kLighten:
      return max(s, d);

    case kColorDodge:
      return vec3(colorDodgeChannel(d.r, s.r), colorDodgeChannel(d.g, s.g), colorDodgeChannel(d.b, s.b));

    case kColorBurn:
      return vec3(colorBurnChannel(d.r, s.r), colorBurnChannel(d.g, s.g), colorBurnChannel(d.b, s.b));

    case kHardLight:
      return hardLight(s, d);

    case kSoftLight:
      return vec3(softLightChannel(d.r, s.r), softLightChannel(d.g, s.g), softLightChannel(d.b, s.b));

    case kDifference:
      return abs(s - d);

    case kExclusion:
      // Difference without the doubling: the two agree at the endpoints and differ in the middle.
      return s + d - 2.0 * s * d;

    case kMultiply:
      return s * d;

    default:
      // `kScreen` is the one separable-looking mode this file does not serve, and it is not an
      // oversight: it has a coefficient pair - `One, OneMinusSrcColor`, which is `s + (1-s)*d` - so
      // `BlendState` expresses it exactly and the renderer sends it there. Falling through to the
      // identity makes an accidentally-routed `kScreen` a source-over, which is the defined answer
      // the note on `uBlendMode` promises.
      return s;
  }
  return s;
}

// The luminosity of an unpremultiplied colour, with Rec. 601's coefficients.
//
// Rec. 601 rather than the Rec. 709 that `mask.frag` and `SvgFilter.cpp` use for luminance, because
// the two answer different questions. Luminance is a photometric quantity - how bright a colour is as
// a display reproduces it - and belongs in a mask or a filter. Luminosity here is the axis of the
// hue/saturation/luminosity blend space as the compositing specification defines it, which is what a
// caller comparing against a CSS `mix-blend-mode` is comparing with. Using one constant for both
// would make the blend modes agree with the wrong reference and the masks agree with the wrong one.
float luminosityOf(const vec3 colour) {
  return 0.3 * colour.r + 0.59 * colour.g + 0.11 * colour.b;
}

// The saturation of an unpremultiplied colour: its largest channel less its smallest.
float saturationOf(const vec3 colour) {
  return max(max(colour.r, colour.g), colour.b) - min(min(colour.r, colour.g), colour.b);
}

// The colour moved to `luminosity`, keeping its hue and saturation.
//
// Moving a colour's luminosity can push a channel out of range, and the specification brings it back
// by *desaturating towards the grey of that luminosity* rather than by clamping. Clamping would shift
// the hue as well, which shows as a colour cast on a saturated blend - the kind of wrong that looks
// like the blend mode being misimplemented when it is the gamut mapping that is.
vec3 withLuminosity(vec3 colour, const float luminosity) {
  float delta = luminosity - luminosityOf(colour);
  colour += vec3(delta);

  // Pull each channel back by moving the others, which is a step towards the grey and so changes no
  // hue. The weights are the luminosity coefficients, so the luminosity is what stays fixed while the
  // channel moves. The order is the reference's and is load-bearing: each branch tests the value the
  // previous branch may have just changed.
  if (colour.r < 0.0) {
    colour.g -= colour.r * 0.59 / 0.3;
    colour.b -= colour.r * 0.11 / 0.3;
    colour.r = 0.0;
  }
  if (colour.g < 0.0) {
    colour.r -= colour.g * 0.3 / 0.59;
    colour.b -= colour.g * 0.11 / 0.59;
    colour.g = 0.0;
  }
  if (colour.b < 0.0) {
    colour.r -= colour.b * 0.3 / 0.11;
    colour.g -= colour.b * 0.59 / 0.11;
    colour.b = 0.0;
  }

  // Anything above one is scaled towards the grey rather than clamped, for the same reason.
  float maximum = max(max(colour.r, colour.g), colour.b);
  if (maximum > 1.0) {
    colour /= maximum;
  }
  return colour;
}

// The colour moved to `saturation`, keeping its hue and luminosity.
//
// Scaling about the colour's own smallest channel, which leaves the hue alone because hue depends on
// the *ratios* between the channels' differences rather than on their sizes.
vec3 withSaturation(vec3 colour, const float saturation) {
  float current = saturationOf(colour);
  if (current <= 0.0) {
    // A grey has no hue to preserve, so the only colour at this saturation and that luminosity is the
    // grey itself.
    return vec3(luminosityOf(colour));
  }

  float scale = saturation / current;
  float minimum = min(min(colour.r, colour.g), colour.b);
  vec3 result = minimum + (colour - minimum) * scale;

  // The requested saturation may not be reachable at this luminosity; the specification's answer is
  // the most saturated colour that is, which is what desaturating back to the original luminosity
  // gives.
  if (result.r > 1.0 || result.g > 1.0 || result.b > 1.0) {
    result = withLuminosity(result, luminosityOf(colour));
  }
  return result;
}

// The four non-separable colour functions.
//
// Transcribed from `nonSeparableBlend` in `BlendMode.h`, consequences included - see the file-level
// note about `kHue`/`kSaturation` computing the same picture and `kLuminosity` being the identity.
// That is a transcription on purpose and not an oversight, and the note says why.
vec3 nonSeparableBlend(const int mode, const vec3 d, const vec3 s) {
  switch (mode) {
    case kHue: {
      // Two steps, and the second takes the luminosity of the *first step's result* rather than of the
      // destination. That is what `BlendMode.h` does, and for an in-gamut colour the second step is a
      // no-op - which is exactly why `kHue` and `kSaturation` agree there.
      vec3 result = withSaturation(d, saturationOf(s));
      return withLuminosity(result, luminosityOf(result));
    }

    case kSaturation:
      return withSaturation(d, saturationOf(s));

    case kColor:
      return withLuminosity(d, luminosityOf(s));

    case kLuminosity:
      return withLuminosity(d, luminosityOf(d));

    default:
      return d;
  }
  return d;
}

// The colour function of a mode, in unpremultiplied space.
//
// The two branches below are the whole scope of this file: the ten separable colour functions and the
// four non-separable ones. Everything else - the fifteen modes with a coefficient pair, and any value
// outside the enum - is answered with the source itself, which makes the composite source-over. That
// is a defined picture, and the note on `uBlendMode` says why it is preferred to an undefined one.
vec3 colourFunction(const int mode, const vec3 d, const vec3 s) {
  if (mode <= kLastBlendUnitMode) {
    // The blend unit's fifteen. See the file-level note: this is a caller bug, not a mode this shader
    // implements badly.
    return s;
  }
  if (mode >= kHue) {
    return nonSeparableBlend(mode, d, s);
  }

  // Clamped, because the colour functions are not all bounded: soft light and dodge can leave the
  // unit interval, and the compositing step below is defined for unit-interval values.
  return clamp(separableBlend(mode, d, s), 0.0, 1.0);
}

void main() {
  vec4 source = texture(uSource, vTexCoord);

  // A linear-light text layer is not a premultiplied colour: RGB is per-channel coverage. Match
  // DirectWrite's coverage-space gamma/contrast curve first, then interpolate the destination and
  // text colours in sRGB. That is the same per-channel result as the known-background path, but with
  // the destination read per pixel, so an arbitrary image/gradient needs no caller snapshot.
  if (uComposite.uLinearText != 0) {
    vec4 destination = texture(uDestination, vTexCoord);
    vec3 coverage = clamp(source.rgb, 0.0, 1.0) *
                    clamp(uComposite.uAlpha, 0.0, 1.0);
    vec3 text = clamp(vec3(uComposite.uTextR, uComposite.uTextG, uComposite.uTextB), 0.0, 1.0);
    vec3 background = destination.a > 0.0001 ? destination.rgb / destination.a : vec3(0.0);
    vec3 shaped = pow(max(coverage, vec3(0.0)), vec3(1.0 / uComposite.uGamma));
    shaped = (shaped * (1.0 + uComposite.uContrast)) /
             (shaped * uComposite.uContrast + 1.0);
    // Source-over with per-channel coverage. Keeping the destination alpha in the result is what
    // makes a text layer inside a transparent group or an opacity group survive: the old code wrote
    // destination.a, so text in a transparent layer disappeared entirely.
    float textAlpha = max(max(shaped.r, shaped.g), shaped.b);
    vec3 premultiplied = text * shaped + background * destination.a * (1.0 - shaped);
    float outAlpha = textAlpha + destination.a * (1.0 - textAlpha);
    oColor = vec4(clamp(premultiplied, 0.0, max(outAlpha, 0.0)), clamp(outAlpha, 0.0, 1.0));
    return;
  }

  // The layer, at the global alpha. Scaling a premultiplied colour scales its alpha by the same
  // factor, which is the global alpha's whole effect - the same operation `Canvas::compositeLayer`
  // performs per pixel when it composites a layer on the CPU.
  source *= clamp(uComposite.uAlpha, 0.0, 1.0);

  // The target as it was when the snapshot was taken, in the same orientation. See the note on
  // `uDestination`.
  vec4 destination = texture(uDestination, vTexCoord);

  // The colour functions say what colour a pixel is, not how much of it there is, so the coverage is
  // always source-over over the result: `(1 - sa) * d + sa * blended`, on premultiplied values.
  //
  // This is why the destination had to be readable and why there is no weighted-sum branch here: a
  // Porter-Duff operator would be a two-term sum of the two premultiplied colours with no
  // unpremultiply anywhere, and it is exactly that operation the blend unit already does for nothing.
  float sa = source.a;
  float da = destination.a;
  vec3 blended = colourFunction(uComposite.uBlendMode, unpremultiplied(destination), unpremultiplied(source));

  // Source-over's alpha: `sa + da * (1 - sa)`, which is `BlendState::premultipliedBlend()`'s alpha
  // component written out. It is computed rather than left to the blend unit because the blend unit is
  // not enabled for this pass - the shader is doing the mode, so the pass must write the final pixel.
  float outAlpha = sa + da * (1.0 - sa);

  oColor = vec4((1.0 - sa) * destination.rgb + sa * blended, outAlpha);
}
