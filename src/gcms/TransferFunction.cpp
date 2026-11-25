/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and limitations under
 * the License.
 */

#include "gcms/TransferFunction.h"

#include "gcms/Numeric.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace gcms {

namespace {

// ------------------------------------------------------------------------------------------------
// Hybrid log-gamma, BT.2100
// ------------------------------------------------------------------------------------------------
//
// `expApprox` and `logApprox` used to be defined here -- natural-exponential wrappers over the base-2 approximations.
// They are gone because nothing used them: the curves in this file reach `powApprox` directly, and HLG's exponential
// branch is evaluated at an argument of 2.8, where `expApprox` was out by nearly a factor of two. Rather than leave two
// functions whose only caller was a branch that could not use them, that branch calls `std::exp`.

/**
 * BT.2100's hybrid log-gamma constants.
 *
 * `a` is the standard's stated value. `b` and `c` are not independent choices: they are what make the two branches meet
 * at E = 1 with the same slope, so the curve has no seam where a gradient would show one. Checking that relationship is
 * the only thing standing between a correct HLG and one that inverts into a kink at a value nobody looks at.
 *
 * `c` is *computed* rather than written as a decimal, and that is not fussiness. The standard states it as
 * `0.5 - a*ln(4a)`, and `ln(4a)` is negative here -- about -0.335 -- so a transcription that loses the sign moves `c`
 * from 0.5599 to 0.4380. The curve then still joins its branches at the right place for `b`, still round trips, and is
 * wrong by a factor of two at peak white: `eval(1.0)` came out at 1.95. Computing it from `a` makes that particular
 * mistake impossible, and the test that asserts `eval(1.0) == 1` catches any other.
 */
constexpr float kHlgA = 0.17883277f;
constexpr float kHlgB = 1.0f - 4.0f * kHlgA;

/** The natural logarithm, as a constant expression so that `kHlgC` can be derived rather than transcribed. */
constexpr float hlgConstantLog(const float x, const int terms = 64) noexcept {
  // Atanh series: ln(x) = 2 * atanh((x-1)/(x+1)), which converges quickly for the x here.
  const float z = (x - 1.0f) / (x + 1.0f);
  float zSquared = z * z;
  float sum = 0.0f;
  float term = z;
  for (int n = 0; n < terms; ++n) {
    sum += term / static_cast<float>(2 * n + 1);
    term *= zSquared;
  }
  return 2.0f * sum;
}

constexpr float kHlgC = 0.5f - kHlgA * hlgConstantLog(4.0f * kHlgA);

/**
 * The scene-linear range HLG encodes: twelve times diffuse white, which is the headroom the standard carries.
 *
 * `eval` divides by this and `encode` multiplies by it, so the curve maps [0,1] to [0,1] like every other family and a
 * caller comparing two spaces is comparing like with like.
 */
constexpr float kHlgHeadroom = 12.0f;

/** Where the OETF's branches meet, in the scene-linear domain. Both give 0.5 there. */
constexpr float kHlgLinearBreak = 1.0f;

/** Where the inverse's branches meet, in the encoded domain. Both give 1/12 scene-linear there. */
constexpr float kHlgEncodedBreak = 0.5f;

/**
 * @return scene-linear light for encoded @p encoded, normalised so that 1.0 is peak white.
 *
 * The break is at an encoded 0.5, which corresponds to a scene-linear *one* -- not to 1/12, which is what `12 * 0.5`
 * suggests and what the removed first attempt assumed. That single mistake makes both directions individually plausible
 * and the round trip fail, which is exactly the failure the removed attempt recorded.
 *
 * `std::exp` rather than the module's `expApprox`, and the difference is not a preference. The exponential branch is
 * evaluated at `(E' - c) / a`, which reaches 2.8 at peak white, and `expApprox` is out by nearly a factor of two there:
 * it returns about 23 where `e^2.8` is 16.8, so `eval(1.0)` came out at 1.95 instead of 1.0. HLG's round trip still
 * closed, because `encode` used the matching approximation and they cancelled -- which is why the round trip alone does
 * not catch this, and why the curve's identifying values are asserted separately.
 *
 * The module uses approximations throughout to avoid libm. That is the right trade for a curve evaluated per pixel per
 * channel with a bounded argument, and the wrong one for a branch whose argument reaches 2.8: the error is visible, and
 * one `std::exp` per pixel is not what a colour pipeline is spending its time on.
 */
[[nodiscard]] float hlgDecode(const float encoded) noexcept {
  const float scene = (encoded <= kHlgEncodedBreak)
          ? (encoded / 0.5f) * (encoded / 0.5f)
          : std::exp((encoded - kHlgC) / kHlgA) + kHlgB;
  return scene / kHlgHeadroom;
}

/** @return the encoded value for normalised linear @p linear. The exact inverse of @ref hlgDecode. */
[[nodiscard]] float hlgEncode(const float linear) noexcept {
  const float scene = linear * kHlgHeadroom;
  return (scene <= kHlgLinearBreak) ? 0.5f * std::sqrt(scene) : kHlgA * std::log(scene - kHlgB) + kHlgC;
}

/**
 * @return true when @p p describes a curve that can actually be evaluated.
 *
 * These are the soundness conditions skcms checks, and they are not arbitrary: a negative @c a or a
 * negative @c g would raise a negative number to a fractional power somewhere in the domain, and
 * `a*d + b >= 0` is the condition that the argument of the power stays non-negative across the join at
 * @c d. A profile that fails them is malformed, and accepting it would produce complex numbers that
 * propagate through every later stage as NaN.
 */
bool isUsable(const SRGBishParameters& p) noexcept {
  return isFinite(p.a + p.b + p.c + p.d + p.e + p.f + p.g) && p.a >= 0.0f && p.c >= 0.0f && p.d >= 0.0f &&
         p.g >= 0.0f && p.a * p.d + p.b >= 0.0f;
}

} // namespace anonymous

TransferFunction TransferFunction::makeIdentity() noexcept {
  return TransferFunction(Kind::Identity, IdentityParameters {});
}

TransferFunction TransferFunction::makeSRGB() noexcept {
  return TransferFunction(Kind::SRGBish,
          SRGBishParameters { 2.4f, static_cast<float>(1 / 1.055), static_cast<float>(0.055 / 1.055),
                  static_cast<float>(1 / 12.92), 0.04045f, 0.0f, 0.0f });
}

TransferFunction TransferFunction::makeGamma(const float gamma) noexcept {
  // d is zero, which collapses the linear segment to the single point at the origin: a pure power
  // curve. Keeping it in the same family rather than special-casing it means every consumer of a
  // transfer function has one code path.
  return TransferFunction(Kind::SRGBish, SRGBishParameters { gamma, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f });
}

std::optional<TransferFunction> TransferFunction::makeSRGBish(const SRGBishParameters& parameters) noexcept {
  if (!isUsable(parameters)) return std::nullopt;
  return TransferFunction(Kind::SRGBish, parameters);
}

std::optional<TransferFunction> TransferFunction::makePQish(
        const float a, const float b, const float c, const float d, const float e, const float f) noexcept {
  if (!isFinite(a + b + c + d + e + f)) return std::nullopt;
  if (c == 0.0f || f == 0.0f) return std::nullopt;
  return TransferFunction(Kind::PQish, PQishParameters { a, b, c, d, e, f });
}

TransferFunction TransferFunction::makePQ() noexcept {
  return TransferFunction(Kind::PQ, PQParameters { 10000.0f });
}

TransferFunction TransferFunction::makeHLG(const float referenceWhiteLuminance) noexcept {
  return TransferFunction(Kind::HLG, HLGParameters { referenceWhiteLuminance });
}

const SRGBishParameters* TransferFunction::sRGBish() const noexcept {
  return std::get_if<SRGBishParameters>(&mParameters);
}

float TransferFunction::eval(const float encoded) const noexcept {
  // Every curve is evaluated on the magnitude and has its sign restored afterwards, so that a value
  // below zero continues the curve rather than being clamped. See the header for why.
  const float sign = encoded < 0.0f ? -1.0f : 1.0f;
  const float x = encoded * sign;

  switch (mKind) {
    case Kind::SRGBish: {
      const auto& p = std::get<SRGBishParameters>(mParameters);
      return sign * (x < p.d ? p.c * x + p.f : powApprox(p.a * x + p.b, p.g) + p.e);
    }

    case Kind::PQish: {
      const auto& p = std::get<PQishParameters>(mParameters);
      const float xc = powApprox(x, p.c);
      return sign * powApprox((p.a + p.b * xc) / (p.d + p.e * xc), p.f);
    }

    case Kind::Identity:
      return encoded;

    case Kind::PQ: {
      // The standard curve, in the closed form the specification gives. Written out rather than
      // routed through makePQish so that the coefficients are visible where they are used.
      constexpr float c1 = 107.0f / 128.0f;
      constexpr float c2 = 2413.0f / 128.0f;
      constexpr float c3 = 2392.0f / 128.0f;
      constexpr float m1 = 1305.0f / 8192.0f;
      constexpr float m2 = 2523.0f / 32.0f;
      const float p = powApprox(x, 1.0f / m2);
      return powApprox((p - c1) / (c2 - c3 * p), 1.0f / m1);
    }

    case Kind::HLG:
      // Its own function rather than a coefficient pair, because HLG's two directions are different functions -- see
      // the note in the header. The sign is applied by the caller, which is what makes the curve odd-symmetric here as
      // it is for every other family.
      return sign * hlgDecode(x);
  }
  return 0.0f;
}

float TransferFunction::encode(const float linear) const noexcept {
  const float sign = linear < 0.0f ? -1.0f : 1.0f;
  const float x = linear * sign;

  switch (mKind) {
    case Kind::Identity:
      return linear;

    case Kind::HLG:
      return sign * hlgEncode(x);

    default: {
      // Every other family's inverse is a curve, so asking for it and evaluating that is the encode direction -- and
      // writing it this way rather than as a third implementation is what keeps the two directions from drifting
      // apart. A family whose inverse is not expressible returns `nullopt`, and there is genuinely no answer then;
      // HLG is handled above precisely because it is that case.
      const auto other = inverse();
      return other.has_value() ? other->eval(linear) : 0.0f;
    }
  }
}

std::optional<TransferFunction> TransferFunction::inverse() const noexcept {
  switch (mKind) {
    case Kind::PQish: {
      // Inverting a ratio of powers swaps the coefficients and reciprocates the exponents:
      //   y = ((A + B x^C) / (D + E x^C))^F  =>  x = ((-A + D y^(1/F)) / (B - E y^(1/F)))^(1/C)
      const auto& p = std::get<PQishParameters>(mParameters);
      return makePQish(-p.a, p.d, 1.0f / p.f, p.b, -p.e, 1.0f / p.c);
    }

    case Kind::Identity:
      return *this;

    case Kind::HLG:
      // No curve to return. HLG's encode is a square root joined to a logarithm, which this vocabulary cannot express
      // -- the seven-parameter form has one power branch and one linear one, and the PQ-ish form is a ratio of powers.
      // `encode` performs the operation, and that is the honest answer: the *direction* exists, its representation as a
      // curve does not.
      return std::nullopt;

    case Kind::PQ: {
      // The standard PQ curve, which `eval` writes in the PQish form with the specification's coefficients:
      //
      //     y = ((x^(1/m2) - c1) / (c2 - c3 x^(1/m2)))^(1/m1)
      //
      // Matching that against the PQish form `y = ((a + b x^c) / (d + e x^c))^f` gives
      //
      //     a = -c1   b = 1   c = 1/m2   d = c2   e = -c3   f = 1/m1
      //
      // Note `b = 1`, not 0: the numerator is `x^(1/m2) - c1`, so the coefficient of `x^c` is one and the constant is
      // the negative of c1. Reading the numerator as a bare constant is the mistake that makes this case look right
      // and produce a curve that inverts almost nothing.
      //
      // This case used to be absent entirely, so inverting a PQ curve fell through to `nullopt` -- a caller asking for
      // the inverse of the standard HDR curve was told there is none, while inverting the *same curve* written as an
      // explicit `PQish` worked. Clang's `-Wswitch` reported the missing enumerator; MSVC does not warn about it.
      constexpr float c1 = 107.0f / 128.0f;
      constexpr float c2 = 2413.0f / 128.0f;
      constexpr float c3 = 2392.0f / 128.0f;
      constexpr float m1 = 1305.0f / 8192.0f;
      constexpr float m2 = 2523.0f / 32.0f;

      // The substitution the explicit `PQish` case above performs on its six parameters, applied to these. In order,
      // that is `(-a, d, 1/f, b, -e, 1/c)`:
      //
      //     -a  =  c1
      //      d  =  c2
      //     1/f =  m1
      //      b  =  1
      //     -e  =  c3
      //     1/c =  m2
      //
      // Position by position, because the two reciprocals are easy to transpose -- and transposing them gives a curve
      // that still looks like PQ and inverts none of the range.
      return makePQish(c1, c2, m1, 1.0f, c3, m2);
    }

    case Kind::SRGBish: {
      const auto& s = std::get<SRGBishParameters>(mParameters);

      // The two segments have to meet at d, or the function is discontinuous and has no inverse.
      // Checking that here rather than trusting the caller is what makes a round trip safe.
      const float dLeft = s.c * s.d + s.f;
      const float dRight = powApprox(s.a * s.d + s.b, s.g) + s.e;
      if (!isFinite(dLeft) || !isFinite(dRight) || std::fabs(dLeft - dRight) > 1 / 512.0f) {
        return std::nullopt;
      }

      SRGBishParameters inv;
      inv.d = dLeft;

      // A d of zero means the linear segment is a single point at the origin, so its inverse has no
      // extent either: c, d and f stay zero and the whole curve is the power segment.
      if (inv.d > 0.0f) {
        if (s.c == 0.0f) return std::nullopt;
        inv.c = 1.0f / s.c;
        inv.f = -s.f / s.c;
      }

      if (s.g == 0.0f || s.a == 0.0f) return std::nullopt;

      // Solving `y = (a x + b)^g + e` for x and rearranging it into the same seven-parameter form
      // requires moving the 1/a inside the exponentiation, which is what k is for.
      const float k = powApprox(s.a, -s.g);
      inv.g = 1.0f / s.g;
      inv.a = k;
      inv.b = -k * s.e;
      inv.e = -s.b / s.a;

      if (inv.a < 0.0f) return std::nullopt;
      // Unlike a negative scale, an `a*d + b` that has drifted slightly below zero after the algebra
      // can be pulled back to the boundary, which is where it should have been.
      if (inv.a * inv.d + inv.b < 0.0f) inv.b = -inv.a * inv.d;

      const auto candidate = makeSRGBish(inv);
      if (!candidate) return std::nullopt;

      // Preserve `inverse(curve(1)) == 1`, which is the invariant that makes a round trip through a
      // profile land on white exactly. Without this the endpoints drift by an ulp or two, and white
      // stops being white after a few conversions.
      const float s1 = eval(1.0f);
      if (!isFinite(s1)) return std::nullopt;
      const float sign = s1 < 0.0f ? -1.0f : 1.0f;
      const float magnitude = s1 * sign;

      SRGBishParameters adjusted = inv;
      if (magnitude < inv.d) {
        adjusted.f = 1.0f - sign * inv.c * magnitude;
      } else {
        adjusted.e = 1.0f - sign * powApprox(inv.a * magnitude + inv.b, inv.g);
      }
      return makeSRGBish(adjusted);
    }
  }
  return std::nullopt;
}

// ==================================================================================================
// Curve
// ==================================================================================================

Curve::Curve() : mStorage(Storage::Function), mFunction(TransferFunction::makeIdentity()) {}

Curve::Curve(const TransferFunction& function) : mStorage(Storage::Function), mFunction(function) {}

Curve Curve::fromTable8(const uint8_t* samples, const uint32_t count) {
  Curve curve;
  if (samples == nullptr || count == 0) return curve;

  curve.mStorage = Storage::Table8;
  curve.mSourceMax = 255;
  // Widened to 16 bits so that interpolation has one implementation, but kept on the 0..255 lattice:
  // the samples below are exact multiples of 257, and interpolating those is what an 8-bit table
  // means.
  curve.mSamples.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    curve.mSamples[i] = static_cast<uint16_t>(samples[i] * 257u);
  }
  return curve;
}

Curve Curve::fromTable16Be(const uint8_t* samples, const uint32_t count) {
  Curve curve;
  if (samples == nullptr || count == 0) return curve;

  curve.mStorage = Storage::Table16;
  curve.mSourceMax = 65535;
  curve.mSamples.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    // ICC stores these big-endian regardless of the host, so the bytes are read explicitly rather
    // than reinterpreted -- a cast would be both endian- and alignment-dependent.
    curve.mSamples[i] = static_cast<uint16_t>((samples[2 * i] << 8) | samples[2 * i + 1]);
  }
  return curve;
}

const TransferFunction* Curve::function() const noexcept {
  return mStorage == Storage::Function ? &mFunction : nullptr;
}

uint32_t Curve::tableEntryCount() const noexcept {
  return mStorage == Storage::Function ? 0u : static_cast<uint32_t>(mSamples.size());
}

float Curve::eval(const float encoded) const noexcept {
  if (mStorage == Storage::Function) return mFunction.eval(encoded);

  // Indexing is clamped at both ends: a curve is only defined on [0,1], and callers filter images,
  // which produces values outside it. Clamping matches what a GPU sampler does with the same input.
  const float clamped = std::clamp(encoded, 0.0f, 1.0f);
  const float scaled = clamped * static_cast<float>(mSamples.size() - 1);

  const auto lo = static_cast<size_t>(scaled);
  // The upper index is derived from the *next* float up rather than rounded, so that an exact hit on
  // a sample interpolates between that sample and itself (t == 0) instead of between it and the next.
  const auto hi = static_cast<size_t>(static_cast<float>(minusOneUlp(scaled + 1.0f)));
  const float t = scaled - static_cast<float>(lo);

  const float loValue = static_cast<float>(mSamples[lo]) / 65535.0f;
  const float hiValue = static_cast<float>(mSamples[std::min(hi, mSamples.size() - 1)]) / 65535.0f;
  return loValue + (hiValue - loValue) * t;
}

float Curve::maxRoundtripError(const TransferFunction& inverse, const uint32_t sampleCount) const noexcept {
  // At least as many samples as a table could have, so the error is measured everywhere it could
  // matter rather than between the entries.
  const uint32_t n = std::max(sampleCount, tableEntryCount() > 256u ? tableEntryCount() : 256u);
  if (n < 2) return 0.0f;

  const float dx = 1.0f / static_cast<float>(n - 1);
  float error = 0.0f;
  for (uint32_t i = 0; i < n; ++i) {
    const float x = static_cast<float>(i) * dx;
    const float y = eval(x);
    error = std::max(error, std::fabs(x - inverse.eval(y)));
  }
  return error;
}

bool Curve::isApproximateInverse(const TransferFunction& inverse) const noexcept {
  // 1/512 of a unit: below one 8-bit step (1/255), so a curve that passes this is one a caller can
  // substitute without the difference surviving quantisation to eight bits.
  return maxRoundtripError(inverse) < (1 / 512.0f);
}

namespace {

/** The result of the linear-segment search: how many leading entries lie on the line, and what it is. */
struct LinearFit {
  int pointCount = 1;
  float slope = 0.0f;
  float intercept = 0.0f;
};

/**
 * @return how many of the leading entries of @p samples lie on a line from @p samples[0], within @p tolerance,
 *         along with that line.
 *
 * ## Why this is not a least-squares fit
 *
 * A parametric curve's linear segment has to *join* its power segment, so the question is not "what line fits
 * these points best" but "how far can the line reach before the points leave it". Walking outward from the
 * origin, each point admits an interval of slopes that would pass within the tolerance of it, and the line
 * survives while those intervals keep overlapping. When they stop, no single line covers one more point.
 *
 * The subtlety the reference records and this keeps: a point's interval can overlap the running one without
 * being *contained* by it. Such a point extends how far the line may reach but is not itself a valid endpoint,
 * so the last candidate that was fully inside is remembered separately and the line is built through that one.
 */
LinearFit fitLinearSegment(const std::vector<float>& samples, const float tolerance) {
  LinearFit fit;
  const auto count = static_cast<int>(samples.size());
  if (count < 2) return fit;

  const float dx = 1.0f / static_cast<float>(count - 1);
  fit.intercept = samples.front();

  float slopeMin = -std::numeric_limits<float>::infinity();
  float slopeMax = std::numeric_limits<float>::infinity();

  for (int i = 1; i < count; ++i) {
    const float x = static_cast<float>(i) * dx;
    const float y = samples[static_cast<size_t>(i)];

    const float upper = (y + tolerance - fit.intercept) / x;
    const float lower = (y - tolerance - fit.intercept) / x;
    if (upper < slopeMin || slopeMax < lower) break;  // no slope satisfies every point so far

    slopeMax = std::min(slopeMax, upper);
    slopeMin = std::max(slopeMin, lower);

    const float slope = (y - fit.intercept) / x;
    if (slopeMin <= slope && slope <= slopeMax) {
      fit.pointCount = i + 1;
      fit.slope = slope;
    }
  }

  return fit;
}

/** @return @p parameters with the constraints a curve must satisfy reapplied. */
std::optional<SRGBishParameters> fixup(SRGBishParameters parameters) {
  // A negative `a` would make the curve decrease with its input, which no display curve does and which the
  // optimisation has no way to rule out on its own.
  if (parameters.a < 0.0f) return std::nullopt;

  // `a*d + b` has to be non-negative or the power takes a negative base. Clamping `b` is the smallest change
  // that restores it.
  if (parameters.a * parameters.d + parameters.b < 0.0f) parameters.b = -parameters.a * parameters.d;

  // `e` is solved for rather than fitted, so that the two segments meet exactly. That is why the reference
  // keeps it as a free parameter: continuity is a constraint, not something to optimise towards.
  const float join = parameters.a * parameters.d + parameters.b;
  if (!(join >= 0.0f)) return std::nullopt;
  parameters.e = parameters.c * parameters.d + parameters.f - powApprox(join, parameters.g);
  if (!std::isfinite(parameters.e)) return std::nullopt;
  return parameters;
}

/**
 * The residual of the *inverse* curve at @p x and its gradient with respect to gamma, `a` and `b`.
 *
 * The fit is made in the inverse direction because that is the direction a display curve runs -- encoded to
 * linear -- and because the parameters of a forward and an inverse curve are not independent: fitting the one
 * that is actually used avoids a round trip through an inversion that may not exist.
 */
float inverseResidual(const Curve& curve, const SRGBishParameters& p, const float x, float dfdP[3]) {
  const float y = curve.eval(x);

  const float scaled = std::max(p.a * y + p.b, 0.0f);
  const float atD = p.a * p.d + p.b;
  if (!(scaled > 0.0f) || !(atD > 0.0f)) {
    // The parametrisation has left the domain where the power is real. A zero gradient lets the caller reject
    // the step instead of carrying a NaN into the next one.
    dfdP[0] = dfdP[1] = dfdP[2] = 0.0f;
    return 0.0f;
  }

  dfdP[0] = std::log(scaled) * powApprox(scaled, p.g) - std::log(atD) * powApprox(atD, p.g);
  dfdP[1] = y * p.g * powApprox(scaled, p.g - 1.0f) - p.d * p.g * powApprox(atD, p.g - 1.0f);
  dfdP[2] = p.g * powApprox(scaled, p.g - 1.0f) - p.g * powApprox(atD, p.g - 1.0f);

  const float inverseValue = powApprox(scaled, p.g) - powApprox(atD, p.g) + p.c * p.d + p.f;
  return x - inverseValue;
}

/**
 * One Gauss-Newton step on the three parameters that shape the power segment.
 *
 * The step is `P' = P + (J^T J)^-1 J^T r`, accumulated over the samples as it goes: both `J^T J` and `J^T r`
 * are sums over the sample index, so one pass builds them and nothing scales with the sample count.
 */
std::optional<SRGBishParameters> gaussNewtonStep(
        const Curve& curve, const SRGBishParameters& p, const int firstIndex, const int sampleCount, const float dx) {
  double lhs[3][3] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
  double rhs[3] = { 0, 0, 0 };

  for (int i = 0; i < sampleCount; ++i) {
    const float x = static_cast<float>(firstIndex + i) * dx;
    float dfdP[3] = { 0.0f, 0.0f, 0.0f };
    const float residual = inverseResidual(curve, p, x, dfdP);

    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) lhs[r][c] += static_cast<double>(dfdP[r]) * dfdP[c];
      rhs[r] += static_cast<double>(dfdP[r]) * residual;
    }
  }

  // A parameter that no sample depends on leaves a zero row and column, which makes the matrix singular. A unit
  // diagonal in its place makes it invertible and leaves that parameter where it is, which is right: there is
  // no information in the residual to move it with.
  for (int k = 0; k < 3; ++k) {
    if (lhs[k][0] == 0.0 && lhs[k][1] == 0.0 && lhs[k][2] == 0.0 &&
        lhs[0][k] == 0.0 && lhs[1][k] == 0.0 && lhs[2][k] == 0.0) {
      lhs[k][k] = 1.0;
    }
  }

  const auto determinantOf = [](const double m[3][3]) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
           m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  };

  const double determinant = determinantOf(lhs);
  if (!(std::fabs(determinant) > 0.0)) return std::nullopt;

  // Cramer's rule for the three components, which avoids writing an explicit 3x3 inverse.
  double delta[3] = { 0, 0, 0 };
  for (int k = 0; k < 3; ++k) {
    double substituted[3][3] = {};
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) substituted[r][c] = lhs[r][c];
    }
    for (int r = 0; r < 3; ++r) substituted[r][k] = rhs[r];
    delta[k] = determinantOf(substituted) / determinant;
  }

  SRGBishParameters next = p;
  next.g += static_cast<float>(delta[0]);
  next.a += static_cast<float>(delta[1]);
  next.b += static_cast<float>(delta[2]);
  if (!std::isfinite(next.g) || !std::isfinite(next.a) || !std::isfinite(next.b)) return std::nullopt;
  return next;
}

/**
 * @return the largest amount by which @p function disagrees with @p curve at the curve's own sample points.
 *
 * ## Why this is the deviation and not the round-trip error
 *
 * `Curve::maxRoundtripError` measures `x - inverse.eval(curve.eval(x))`, which is the right metric for asking
 * "is this the inverse of that". It is the wrong metric here, and using it was a mistake worth recording.
 *
 * To measure a candidate, the round trip has to invert the candidate and then invert *that* again, and each
 * inversion is itself approximate. The result is that a fit which is exactly right reports the noise of two
 * inversions rather than its own error -- 0.039 for a perfect power-curve fit, against the 0.002 a caller would
 * accept. The fit was correct and the measurement said it was not, which is the most expensive kind of wrong
 * because it looks like an algorithmic failure.
 *
 * What a caller wants to know is simply how far the fitted curve is from the table it replaces, and that needs no
 * inversion at all. It is also what a caller can check: the fit is only ever evaluated in place of the table, so
 * the deviation is the error they will see.
 */
float deviationFrom(const Curve& curve, const TransferFunction& forward) {
  // ## Both sides run in the same direction
  //
  // A curve tag holds the mapping a decoder applies -- encoded to linear -- and the function this file returns is
  // that same mapping, so comparing them directly is meaningful and needs no inversion. Two earlier attempts got
  // this wrong in ways worth keeping:
  //
  //  1. Comparing `forward.eval(x)` with `curve.eval(x)` gave a deviation of 0.51 for a *correct* fit, because the
  //     fit is computed in the inverse direction and the value that had been fitted was the inverse, not the
  //     forward function. A function compared against its own inverse differs by most of the range.
  //  2. Measuring the round trip through `forward.inverse()` gave 0.039 for the same fit. `inverse()` re-solves the
  //     offset `e` so that the pair maps one to one exactly -- a deliberate property, since it keeps white white
  //     through repeated conversions -- which means `forward.inverse()` is not precisely `forward`'s inverse and the
  //     round trip measures that adjustment rather than the fit.
  //
  // Evaluating both at the same input avoids both traps. It is also the comparison the caller can make: the fitted
  // curve is used *in place of* the table, at the same inputs and in the same direction, so the deviation is the
  // error that will be seen.
  //
  // The sample points are the table's own entries, which is where the table is exact and the fit is being asked to
  // reproduce it. Between the entries the table interpolates, so a comparison there would measure the interpolation
  // rather than the fit.
  const auto entries = curve.tableEntryCount();
  if (entries < 2) return std::numeric_limits<float>::infinity();

  const float dx = 1.0f / static_cast<float>(entries - 1);
  float worst = 0.0f;
  for (uint32_t i = 0; i < entries; ++i) {
    const float x = static_cast<float>(i) * dx;
    worst = std::max(worst, std::fabs(forward.eval(x) - curve.eval(x)));
  }
  return worst;
}

/**
 * @return the worst round-trip error of @p inverse against @p curve, or infinity when it cannot be measured.
 *
 * Kept for the places that genuinely want the round trip; the fit does not use it.
 */
float roundtripError(const Curve& curve, const TransferFunction& inverse) {
  const auto forward = inverse.inverse();
  if (!forward.has_value() || forward->kind() != Kind::SRGBish) {
    return std::numeric_limits<float>::infinity();
  }
  const auto backAgain = forward->inverse();
  if (!backAgain.has_value()) return std::numeric_limits<float>::infinity();
  return curve.maxRoundtripError(*backAgain);
}

} // namespace anonymous

std::optional<TransferFunction> Curve::approximate(const uint32_t sampleCount, float* maxError) const noexcept {
  (void) sampleCount;
  if (maxError != nullptr) *maxError = 0.0f;

  // A parametric curve is already its own best approximation, and there is nothing to fit.
  if (mStorage == Storage::Function) return mFunction;

  const auto entries = static_cast<int>(mSamples.size());
  if (entries < 2) return std::nullopt;

  // The table as values in [0,1], which is the domain the fit works on.
  std::vector<float> samples;
  samples.reserve(static_cast<size_t>(entries));
  for (const uint16_t sample: mSamples) samples.push_back(static_cast<float>(sample) / 65535.0f);

  const float dx = 1.0f / static_cast<float>(entries - 1);

  // Two tolerances, and the first that produces a usable fit wins.
  //
  // The tighter one describes a curve whose linear segment is meant to be exact -- a table sampled from a
  // function -- and the looser one a curve that came off an instrument, where a step and a half of 16-bit
  // precision is the noise floor. The order matters: at the loose tolerance a curve that *is* exactly linear
  // comes back as a power curve with gamma one, which evaluates the same but has forgotten that it was a line.
  const float tolerances[] = { 1.5f / 65535.0f, 1.0f / 512.0f };

  for (const float tolerance: tolerances) {
    const LinearFit linear = fitLinearSegment(samples, tolerance);

    if (linear.pointCount >= entries) {
      // The whole curve is a line. It is expressed through the power segment with gamma one and the offset that
      // puts the join at the origin -- the form that makes `d` zero and keeps the representation canonical, so
      // that two curves which evaluate identically also compare equal.
      SRGBishParameters p;
      p.g = 1.0f;
      p.a = linear.slope;
      p.b = linear.intercept;
      const auto fixed = fixup(p);
      if (!fixed.has_value()) continue;
      const auto candidate = TransferFunction::makeSRGBish(*fixed);
      if (!candidate.has_value()) continue;
      // Measured like any other fit rather than assumed exact: the line was chosen to pass within the tolerance of
      // every entry, which is not the same as every entry lying on it.
      const float error = deviationFrom(*this, *candidate);
      if (!(error <= 1.0f / 512.0f)) continue;
      if (maxError != nullptr) *maxError = error;
      return candidate;
    }

    if (linear.pointCount == entries - 1) {
      // Only two points are left for the power segment, which is not enough to fit anything: they determine the
      // line through them exactly, and there is no freedom left for gamma.
      SRGBishParameters p;
      p.g = 1.0f;
      p.c = linear.slope;
      p.d = static_cast<float>(linear.pointCount - 1) * dx;
      p.a = (samples.back() - samples[static_cast<size_t>(entries - 2)]) / dx;
      p.b = samples[static_cast<size_t>(entries - 2)] - p.a * static_cast<float>(entries - 2) * dx;
      p.e = 0.0f;
      const auto fixed = fixup(p);
      if (!fixed.has_value()) continue;
      const auto candidate = TransferFunction::makeSRGBish(*fixed);
      if (!candidate.has_value()) continue;
      const float error = deviationFrom(*this, *candidate);
      if (!(error <= 1.0f / 512.0f)) continue;
      if (maxError != nullptr) *maxError = error;
      return candidate;
    }

    // A gamma-only guess through the middle of the power segment, then Gauss-Newton from there.
    const int middle = (linear.pointCount + entries) / 2;
    const float midX = static_cast<float>(middle) / static_cast<float>(entries - 1);
    const float midY = samples[static_cast<size_t>(middle)];

    SRGBishParameters guess;
    guess.g = (midY > 0.0f && midX > 0.0f) ? log2Approx(midY) / log2Approx(midX) : 1.0f;
    guess.a = 1.0f;
    guess.b = 0.0f;
    guess.c = linear.slope;
    guess.d = static_cast<float>(linear.pointCount - 1) * dx;
    guess.f = linear.intercept;
    const auto fixedGuess = fixup(guess);
    if (!fixedGuess.has_value()) continue;

    // The fit is made in the inverse direction, so the guess is inverted first and the result inverted back at
    // the end. Using the fitted inverse directly would put the parameters on the wrong side of the conversion.
    const auto forwardGuess = TransferFunction::makeSRGBish(*fixedGuess);
    if (!forwardGuess.has_value()) continue;
    const auto inverseGuess = forwardGuess->inverse();
    if (!inverseGuess.has_value()) continue;
    const auto inverseFixed = fixup(*inverseGuess->sRGBish());
    if (!inverseFixed.has_value()) continue;

    // ## The iteration runs on the inverse; the measurement is made on the forward
    //
    // The Gauss-Newton step adjusts the *inverse* parameters, because that is the direction the residual is written
    // in and the direction a display curve is used. The function handed back is the forward one, obtained by
    // inverting. Those are not the same function, and scoring the first while returning the second compares a
    // function with its own inverse: the reported error was 0.51 for a fit whose forward evaluation matched the table
    // to four decimal places, which reads as a broken search and is only a broken measurement.
    //
    // So the loop carries the inverse parameters that the next step needs *and* keeps the best forward function it
    // has seen, scoring that. Neither is derived from the other where the measurement is made.
    const auto forwardOf = [](const SRGBishParameters& inverse) {
      const auto asFunction = TransferFunction::makeSRGBish(inverse);
      if (!asFunction.has_value()) return std::optional<TransferFunction> {};
      return asFunction->inverse();
    };

    auto bestForward = forwardOf(*inverseFixed);
    if (!bestForward.has_value()) continue;
    float bestError = deviationFrom(*this, *bestForward);
    if (!std::isfinite(bestError)) continue;

    SRGBishParameters working = *inverseFixed;

    // A fixed number of steps, and the best iterate kept rather than the last. The residual is not monotone, so
    // a later step can be worse than an earlier one, and one step does not converge from a gamma-only guess.
    for (int step = 0; step < 8; ++step) {
      const auto next = gaussNewtonStep(*this, working, linear.pointCount, entries - linear.pointCount, dx);
      if (!next.has_value()) break;
      const auto nextFixed = fixup(*next);
      if (!nextFixed.has_value()) break;
      working = *nextFixed;

      auto candidate = forwardOf(working);
      if (!candidate.has_value()) break;
      const float error = deviationFrom(*this, *candidate);
      if (error < bestError) {
        bestError = error;
        bestForward = std::move(candidate);
      }
    }

    // ## A fit is returned only when it is good
    //
    // The loop above keeps the best iterate, and keeping the best of eight bad candidates still leaves a bad
    // candidate. Returning it would be worse than failing: a caller that gets a curve back stops interpolating its
    // table, so a curve reporting an error of 0.996 would be used, and the only sign of trouble would be an output
    // image that is visibly wrong with nothing to point at.
    //
    // The threshold is the same 1/512 the inverse check uses, which is a third of an 8-bit step -- below the point
    // where a difference survives quantisation to eight bits, and therefore below the point where a caller can see
    // it. A table that does not fit inside that is one that genuinely is not an sRGB-ish curve.
    constexpr float kAcceptedError = 1.0f / 512.0f;
    if (!(bestError <= kAcceptedError)) continue;

    if (maxError != nullptr) *maxError = bestError;
    return bestForward;
  }

  // No fit within either tolerance. Reporting that is better than returning one that has not been checked: the
  // caller keeps interpolating its table, which is slower and correct, rather than evaluating a curve that is
  // visibly wrong on the display it will be looked at on.
  return std::nullopt;
}

} // namespace gcms
