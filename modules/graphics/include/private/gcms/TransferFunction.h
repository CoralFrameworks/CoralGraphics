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

#ifndef CORAL_GRAPHICS_GCMS_TRANSFER_FUNCTION_H
#define CORAL_GRAPHICS_GCMS_TRANSFER_FUNCTION_H

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

/**
 * @file TransferFunction.h
 *
 * The curve that maps an encoded channel value to a linear one, and back.
 *
 * A colour space is two things: a transfer function per channel (how encoded numbers relate to light)
 * and a matrix (what that light is). This file is the first half.
 *
 * ## The seven-parameter form
 *
 * ICC represents a curve as this piecewise function, and so do we:
 *
 * ```
 * linear = sign(encoded) * (c*|encoded| + f)                  , |encoded| < d
 *        = sign(encoded) * ((a*|encoded| + b)^g + e)           , |encoded| >= d
 * ```
 *
 * Every ordinary curve -- sRGB, pure gamma, Rec.709 -- is an instance of it. Two families are not and
 * cannot be expressed this way: the HDR perceptual quantiser (PQ) and the hybrid log-gamma (HLG)
 * functions are rational and exponential respectively. Both are supported, as their own cases.
 *
 * ## The two directions are not always one expression
 *
 * For the seven-parameter form the inverse is another seven-parameter curve, so one `eval` covers both directions and
 * `inverse()` returns the other curve. HLG is different in kind: its decode is a power branch joined to an exponential
 * one, and its encode is a square root joined to a logarithm. No rearrangement of one is the other, so the family
 * cannot be represented by coefficients at all -- it is stored as a marker and evaluated in both directions.
 *
 * `eval` therefore always means "encoded in, linear out", and `encode` always means "linear in, encoded out". For the
 * parametric families `encode` is `inverse()->eval`, and writing that once is what keeps the two from drifting apart.
 *
 * ## Why this is a sum type and not seven floats
 *
 * skcms stores all five families in the same seven float fields, marks the exotic ones by storing a
 * *negative* `g`, and recovers the family by re-reading those floats on every evaluation. That works,
 * but it means an sRGB curve with a negative gamma is indistinguishable from a PQ curve, and it makes
 * "which family is this?" a question that can only be answered by inspecting values rather than by
 * looking at the type.
 *
 * Here the family is the type. @c Parameters is a `std::variant` with exactly one alternative per
 * family, so an sRGB curve cannot hold HLG coefficients, setting a curve's parameters and asking what
 * kind it is are guaranteed to agree, and the answer is available without a conversion step.
 */

namespace gcms {

/** The seven-parameter ICC curve: the shape every ordinary transfer function takes. */
struct SRGBishParameters {
  float g = 0.0f;
  float a = 0.0f;
  float b = 0.0f;
  float c = 0.0f;
  float d = 0.0f;
  float e = 0.0f;
  float f = 0.0f;

  friend bool operator==(const SRGBishParameters&, const SRGBishParameters&) noexcept = default;
};

/**
 * The perceptual quantiser's rational form, SMPTE ST 2084:
 *
 * ```
 * linear = sign(encoded) * ( (A + B*|encoded|^C) / (D + E*|encoded|^C) ) ^ F
 * ```
 */
struct PQishParameters {
  float a = 0.0f;
  float b = 0.0f;
  float c = 0.0f;
  float d = 0.0f;
  float e = 0.0f;
  float f = 0.0f;

  friend bool operator==(const PQishParameters&, const PQishParameters&) noexcept = default;
};

/**
 * Marker for the identity curve, which has no coefficients.
 *
 * The equality operator is spelled out rather than defaulted because a type with no members is
 * trivially equal to itself, and saying so explicitly keeps `std::variant`'s generated comparison
 * happy on compilers that are strict about instantiating `operator==` for every alternative.
 */
struct IdentityParameters {
  friend constexpr bool operator==(IdentityParameters, IdentityParameters) noexcept { return true; }
};

/**
 * Marker for hybrid log-gamma, whose coefficients are fixed by BT.2100 and are therefore not stored.
 *
 * HLG's two directions are different functions -- see the file comment -- so there is nothing to parameterise: a
 * transfer function of this kind *is* the standard curve. The one thing worth recording is the white level it is
 * referenced to, so that `parameters()` says what the curve means rather than leaving it to be inferred from the family.
 */
struct HLGParameters {
  /** Reference white luminance in cd/m^2. 1000 for the BT.2100 reference display. */
  float referenceWhiteLuminance = 1000.0f;

  friend bool operator==(const HLGParameters&, const HLGParameters&) noexcept = default;
};

/** The canonical PQ coefficients, both as a named state and as the values they stand for. */
struct PQParameters {
  /** Reference white luminance in cd/m^2. 10000 for the standard curve. */
  float referenceWhiteLuminance = 10000.0f;

  friend bool operator==(const PQParameters&, const PQParameters&) noexcept = default;
};

/** The coefficients of one transfer function, tagged by which family they belong to. */
using Parameters =
        std::variant<IdentityParameters, SRGBishParameters, PQishParameters, PQParameters, HLGParameters>;

/** The families a transfer function can belong to, one alternative per @c Parameters case. */
enum class Kind : uint8_t {
  /** The identity curve. Its own case so that "is this a no-op?" is answerable without arithmetic. */
  Identity,
  /** The seven-parameter form, which includes sRGB and every pure gamma. */
  SRGBish,
  /** Perceptual quantiser with explicit coefficients. */
  PQish,
  /** The standard PQ curve, identifiable by its canonical coefficients alone. */
  PQ,
  /** Hybrid log-gamma, BT.2100. Its two directions are different functions, so `encode` is not `eval` of an inverse. */
  HLG,
};

/**
 * A transfer function.
 *
 * Deliberately not a plain aggregate: the only way to build one is through the named constructors
 * below, each of which validates its inputs, so a @c TransferFunction that exists is one that can be
 * evaluated. There is no "invalid" state to check for at every call site.
 */
class TransferFunction {
public:
  /** The identity curve: encoded value in, same value out. */
  [[nodiscard]] static TransferFunction makeIdentity() noexcept;

  /** The sRGB curve, which is also Rec.709's. */
  [[nodiscard]] static TransferFunction makeSRGB() noexcept;

  /** A pure gamma curve, `linear = encoded^gamma`. */
  [[nodiscard]] static TransferFunction makeGamma(float gamma) noexcept;

  /** The seven-parameter form. @return `std::nullopt` when the parameters are not a usable curve. */
  [[nodiscard]] static std::optional<TransferFunction> makeSRGBish(const SRGBishParameters& parameters) noexcept;

  /** A PQ curve with explicit rational coefficients. */
  [[nodiscard]] static std::optional<TransferFunction> makePQish(
          float a, float b, float c, float d, float e, float f) noexcept;

  /** The standard PQ curve, mapping encoded [0,1] to linear [0,1]. */
  [[nodiscard]] static TransferFunction makePQ() noexcept;

  /**
   * The standard hybrid log-gamma curve, BT.2100, mapping encoded [0,1] to linear [0,1].
   *
   * HLG carries twelve times the headroom of diffuse white, so its scene-linear range is [0, 12]. This curve is
   * normalised to [0, 1] by dividing by that twelve, which is what makes it comparable with the other families and what
   * makes `eval(1.0f)` one.
   *
   * @param referenceWhiteLuminance the white level this is referenced to, in cd/m^2. Defaults to 1000, the BT.2100
   *        reference display. It is recorded rather than applied: HLG's curve is fully determined by the standard, and
   *        turning it into display light is the display's job rather than the transfer function's.
   */
  [[nodiscard]] static TransferFunction makeHLG(float referenceWhiteLuminance = 1000.0f) noexcept;

  /** Which family this is, and therefore how it is evaluated. */
  [[nodiscard]] Kind kind() const noexcept { return mKind; }

  /** The coefficients, as the alternative matching @c kind. */
  [[nodiscard]] const Parameters& parameters() const noexcept { return mParameters; }

  /**
   * @return the linear value for @p encoded.
   *
   * The sign is carried through, so the curve is odd-symmetric about zero. That is not a detail: an
   * encoded value slightly below zero is reachable through filtering and interpolation, and a curve
   * that folded it to zero instead of continuing would produce a visible discontinuity at black.
   */
  [[nodiscard]] float eval(float encoded) const noexcept;

  /**
   * @return the encoded value for @p linear: the other direction of this curve.
   *
   * A transfer function has two directions and only one of them can be called `eval`, so this is the second. It is its
   * own entry point because for HLG there is no curve to obtain it from -- the two directions are different functions
   * rather than one expression with two sets of coefficients -- and a caller made to write `inverse()->eval` would have
   * to know which families those were.
   *
   * For every family `encode(eval(x)) == x` to within a part in 512, which is the round trip `maxRoundtripError`
   * measures.
   */
  [[nodiscard]] float encode(float linear) const noexcept;

  /**
   * @return the inverse of @c *this, or `std::nullopt` when it cannot be represented *as a curve*.
   *
   * The standard PQ curve has no invertible parametric form in this vocabulary, so it returns `std::nullopt` -- asking
   * for its inverse as a curve has no answer, even though `encode` performs the operation. HLG is the same and more so:
   * its two directions are different functions rather than two sets of coefficients.
   *
   * A caller wanting the other direction wants `encode`, not this.
   */
  [[nodiscard]] std::optional<TransferFunction> inverse() const noexcept;

  /** @return the sRGB-shaped parameters, or `std::nullopt` when this is a PQ or HLG family. */
  [[nodiscard]] const SRGBishParameters* sRGBish() const noexcept;

  friend bool operator==(const TransferFunction& lhs, const TransferFunction& rhs) noexcept {
    return lhs.mKind == rhs.mKind && lhs.mParameters == rhs.mParameters;
  }

private:
  TransferFunction(Kind kind, Parameters parameters) noexcept: mKind(kind), mParameters(parameters) {}

  Kind mKind;
  Parameters mParameters;
};

/**
 * A channel curve: either a parametric function, or a table of samples.
 *
 * Some profiles carry their curves as sampled tables rather than as parameters -- either because the
 * curve came from measurement, or because the authoring tool did not know a fit. Both forms have to be
 * supported, and both have to evaluate identically at the endpoints.
 *
 * ### Ownership
 *
 * The table is owned, not referenced. In skcms a curve holds `const uint8_t*` into the ICC buffer and
 * the documentation says the buffer must outlive the profile -- an invariant that is easy to state and
 * impossible to enforce. Here the samples are copied during parsing, so a @c Curve is self-contained
 * and a profile can be moved, copied or stored without dragging its source bytes along.
 *
 * The samples are normalised to 16-bit on construction. An 8-bit table keeps its 8-bit resolution
 * rather than being scaled up, because interpolating between two values that differ by one 16-bit step
 * gives a visibly different result from interpolating between two 8-bit ones, and the reference
 * behaviour is what a profile's author measured against.
 */
class Curve {
public:
  /** An identity curve: no samples, and a parametric function that passes values through. */
  Curve();

  /** A curve defined by its parameters. */
  explicit Curve(const TransferFunction& function);

  /** A curve defined by 8-bit samples, as an ICC `curv` tag with one byte per entry. */
  [[nodiscard]] static Curve fromTable8(const uint8_t* samples, uint32_t count);

  /** A curve defined by big-endian 16-bit samples, as an ICC `curv` tag with two bytes per entry. */
  [[nodiscard]] static Curve fromTable16Be(const uint8_t* samples, uint32_t count);

  /** @return the linear value for @p encoded, by evaluating the function or interpolating the table. */
  [[nodiscard]] float eval(float encoded) const noexcept;

  /** @return the parametric function, or `std::nullopt` when this curve is a table. */
  [[nodiscard]] const TransferFunction* function() const noexcept;

  /** @return the number of table entries, or 0 when this curve is parametric. */
  [[nodiscard]] uint32_t tableEntryCount() const noexcept;

  /**
   * @return the largest error, over the unit interval, of evaluating @p inverse after this curve.
   *
   * This is how a caller decides whether a curve it did not author is "close enough" to a known
   * function -- whether a display's measured gamma can be treated as sRGB, say. The metric is the
   * round-trip error rather than the distance between the curves, because the round trip is what the
   * pipeline actually does.
   */
  [[nodiscard]] float maxRoundtripError(const TransferFunction& inverse, uint32_t sampleCount = 256) const noexcept;

  /** @return true when @p inverse undoes this curve to within 1/512 of a unit. */
  [[nodiscard]] bool isApproximateInverse(const TransferFunction& inverse) const noexcept;

  /**
   * @return a parametric approximation of this table, when one can be found.
   *
   * A table has to be interpolated per pixel; a parametric function is a handful of arithmetic
   * operations. For a curve that is close to one of the standard shapes -- which most real display
   * curves are -- replacing it is a substantial saving, provided the fit is good. @p maxError receives
   * the worst deviation of the fit, so a caller can reject an approximation that is technically
   * correct but visibly wrong.
   */
  [[nodiscard]] std::optional<TransferFunction> approximate(uint32_t sampleCount, float* maxError) const noexcept;

private:
  enum class Storage : uint8_t { Function, Table8, Table16 };

  Storage mStorage = Storage::Function;
  TransferFunction mFunction;
  /** Samples normalised to the 0..65535 range regardless of the source's depth. */
  std::vector<uint16_t> mSamples;
  /** The table's original depth, so interpolation can reproduce the reference's rounding. */
  uint32_t mSourceMax = 0;
};

} // namespace gcms

#endif // CORAL_GRAPHICS_GCMS_TRANSFER_FUNCTION_H
