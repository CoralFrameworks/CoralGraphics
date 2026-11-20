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

#include <graphics/ColorSpace.h>

#include <graphics/Data.h>
#include <private/graphics/IccProfile.h>

#include "gcms/Gamut.h"
#include "gcms/TransferFunction.h"

#include <algorithm>
#include <cmath>

namespace graphics {
namespace {

/** FNV-1a over a byte range. The same mixing the rest of the graphics layer uses. */
uint32_t hashBytes(const void* data, const size_t size) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  uint32_t hash = 2166136261u;
  for (size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= 16777619u;
  }
  return hash;
}

/**
 * @return the hash of @p matrix, quantised before hashing.
 *
 * The quantisation is the whole point. A gamut matrix that arrived from ICC fixed-point and one that
 * was computed from chromaticities differ in the last few bits, and hashing the raw floats would treat
 * those as different gamuts -- so two profiles that a user would call "both sRGB" would fail to
 * compare equal, and the pipeline would run a conversion between them that does nothing. Rounding to
 * 1/65536 (the ICC fixed-point step) makes the hash answer "same gamut, as it would be stored".
 */
uint32_t hashGamut(const ColorMatrix& matrix) noexcept {
  float quantised[9];
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      quantised[row * 3 + col] = std::round(matrix.values[row][col] * 65536.0f) / 65536.0f;
    }
  }
  return hashBytes(quantised, sizeof(quantised));
}

/**
 * @return @p matrix in the primitive layer's vocabulary.
 *
 * The facade and `gcms` both describe a 3x3 colour matrix, and they describe it with different types:
 * `ColorMatrix` is the public one, spelled `float values[3][3]` so that a client can read it without
 * including anything of ours, while `gcms::Matrix3x3` owns its storage and participates in the
 * arithmetic. The two conversions below are the only place the boundary is crossed, which keeps the
 * brace-initialisation trap that comes with the primitive type out of the facade entirely.
 */
gcms::Matrix3x3 toGcms(const ColorMatrix& matrix) noexcept {
  return gcms::Matrix3x3::fromRows(matrix.values[0][0], matrix.values[0][1], matrix.values[0][2],
          matrix.values[1][0], matrix.values[1][1], matrix.values[1][2], matrix.values[2][0],
          matrix.values[2][1], matrix.values[2][2]);
}

/** @return @p matrix in the facade's vocabulary. */
ColorMatrix fromGcms(const gcms::Matrix3x3& matrix) noexcept {
  ColorMatrix result {};
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      result.values[row][col] = matrix.values[row][col];
    }
  }
  return result;
}

/** @return the hash of @p function, quantised the same way and for the same reason. */
uint32_t hashTransferFunction(const ColorTransFunc& function) noexcept {
  const float values[7] = { function.g, function.a, function.b, function.c, function.d, function.e, function.f };
  float quantised[7];
  for (int i = 0; i < 7; ++i) quantised[i] = std::round(values[i] * 1024.0f) / 1024.0f;
  return hashBytes(quantised, sizeof(quantised));
}

} // namespace anonymous

bool ColorSpacePrimaries::toXYZD50(ColorMatrix* toXYZD50) const {
  if (toXYZD50 == nullptr) return false;

  const auto matrix = gcms::primariesToXYZD50(rx, ry, gx, gy, bx, by, wx, wy);
  if (!matrix) return false;

  *toXYZD50 = fromGcms(*matrix);
  return true;
}

// ==================================================================================================
// Construction
// ==================================================================================================

constexpr ColorSpace::ColorSpace(const ColorTransFunc& transFunc, const ColorMatrix& toXYZ) noexcept
    : mTransFuncHash(0), mToXYZD50Hash(0), mTransFunc(transFunc), mToXYZD50(toXYZ) {}

std::shared_ptr<ColorSpace> ColorSpace::MakeRGB(const ColorTransFunc& transFunc, const ColorMatrix& toXYZ) {
  // `new` rather than `make_shared` because the constructor is private: the class hands out its
  // instances through these factories so that the lazy fields are always initialised consistently.
  auto space = std::shared_ptr<ColorSpace>(new ColorSpace(transFunc, toXYZ));
  space->mTransFuncHash = hashTransferFunction(transFunc);
  space->mToXYZD50Hash = hashGamut(toXYZ);
  return space;
}

std::shared_ptr<ColorSpace> ColorSpace::MakeSRGB() {
  static const std::shared_ptr<ColorSpace> sRGB = MakeRGB(NamedTransferFn::kSRGB, NamedGamut::kSRGB);
  return sRGB;
}

std::shared_ptr<ColorSpace> ColorSpace::MakeSRGBLinear() {
  static const std::shared_ptr<ColorSpace> linear = MakeRGB(NamedTransferFn::kLinear, NamedGamut::kSRGB);
  return linear;
}

const std::shared_ptr<ColorSpace>& ColorSpace::DisplayP3() {
  static const std::shared_ptr<ColorSpace> p3 = MakeRGB(NamedTransferFn::kSRGB, NamedGamut::kDisplayP3);
  return p3;
}

std::shared_ptr<ColorSpace> ColorSpace::MakeCICP(const NamedPrimaries::CicpId colorPrimaries,
        const NamedTransferFn::CicpId transCharacteristics) {
  // The two tables are the rows of ITU-T H.273's tables 2 and 3 that this build supports. A value not
  // named here is one the specification either reserves or that has no RGB interpretation, and the
  // answer for those is "no colour space" rather than a guess.
  ColorSpacePrimaries primaries;
  switch (colorPrimaries) {
    case NamedPrimaries::CicpId::kRec709: primaries = NamedPrimaries::kRec709; break;
    case NamedPrimaries::CicpId::kRec470SystemM: primaries = NamedPrimaries::kRec470SystemM; break;
    case NamedPrimaries::CicpId::kRec470SystemBG: primaries = NamedPrimaries::kRec470SystemBG; break;
    case NamedPrimaries::CicpId::kRec601: primaries = NamedPrimaries::kRec601; break;
    case NamedPrimaries::CicpId::kSMPTE_ST_240: primaries = NamedPrimaries::kSMPTE_ST_240; break;
    case NamedPrimaries::CicpId::kGenericFilm: primaries = NamedPrimaries::kGenericFilm; break;
    case NamedPrimaries::CicpId::kRec2020: primaries = NamedPrimaries::kRec2020; break;
    case NamedPrimaries::CicpId::kSMPTE_ST_428_1: primaries = NamedPrimaries::kSMPTE_ST_428_1; break;
    case NamedPrimaries::CicpId::kSMPTE_RP_431_2: primaries = NamedPrimaries::kSMPTE_RP_431_2; break;
    case NamedPrimaries::CicpId::kSMPTE_EG_432_1: primaries = NamedPrimaries::kSMPTE_EG_432_1; break;
    case NamedPrimaries::CicpId::kITU_T_H273_Value22: primaries = NamedPrimaries::kITU_T_H273_Value22; break;
    default: return nullptr;
  }

  ColorTransFunc transferFunction;
  switch (transCharacteristics) {
    case NamedTransferFn::CicpId::kRec709: transferFunction = NamedTransferFn::kRec709; break;
    case NamedTransferFn::CicpId::kRec470SystemM: transferFunction = NamedTransferFn::kRec470SystemM; break;
    case NamedTransferFn::CicpId::kRec470SystemBG: transferFunction = NamedTransferFn::kRec470SystemBG; break;
    case NamedTransferFn::CicpId::kRec601: transferFunction = NamedTransferFn::kRec601; break;
    case NamedTransferFn::CicpId::kSMPTE_ST_240: transferFunction = NamedTransferFn::kSMPTE_ST_240; break;
    case NamedTransferFn::CicpId::kLinear: transferFunction = NamedTransferFn::kLinear; break;
    case NamedTransferFn::CicpId::kIEC61966_2_4: transferFunction = NamedTransferFn::kIEC61966_2_4; break;
    case NamedTransferFn::CicpId::kIEC61966_2_1: transferFunction = NamedTransferFn::kIEC61966_2_1; break;
    case NamedTransferFn::CicpId::kRec2020_10bit: transferFunction = NamedTransferFn::kRec2020_10bit; break;
    case NamedTransferFn::CicpId::kRec2020_12bit: transferFunction = NamedTransferFn::kRec2020_12bit; break;
    case NamedTransferFn::CicpId::kPQ: transferFunction = NamedTransferFn::kPQ; break;
    case NamedTransferFn::CicpId::kSMPTE_ST_428_1: transferFunction = NamedTransferFn::kSMPTE_ST_428_1; break;
    case NamedTransferFn::CicpId::kHLG: transferFunction = NamedTransferFn::kHLG; break;
    default: return nullptr;
  }

  ColorMatrix toXYZ {};
  if (!primaries.toXYZD50(&toXYZ)) return nullptr;

  // A colour space with a transfer function that cannot be evaluated is not a colour space, so the function is
  // validated here rather than at the point of use.
  //
  // The question is "can `gcms` evaluate this?", not "is it seven-parameter?" -- and those differ for the two HDR
  // families, which are marked by a *negative* `g` because the seven-parameter form cannot express them. Asking
  // `makeSRGBish` therefore refused PQ and HLG both: it rejects a negative gamma on sound grounds, since a negative
  // gamma in that form would raise a negative number to a fractional power. Using it as the validity check for a
  // sentinel was the mistake, and it went unnoticed because the only CICP test used Rec.709 and Rec.2020.
  //
  // Each family is now checked against the constructor that can actually accept it.
  const bool evaluable = transferFunction.g > 0.0f
          ? gcms::TransferFunction::makeSRGBish(gcms::SRGBishParameters { transferFunction.g, transferFunction.a,
                    transferFunction.b, transferFunction.c, transferFunction.d, transferFunction.e,
                    transferFunction.f })
                  .has_value()
          : (transferFunction.g == NamedTransferFn::kPQ.g || transferFunction.g == NamedTransferFn::kHLG.g);

  if (!evaluable) return nullptr;
  return MakeRGB(transferFunction, toXYZ);
}

namespace {

/**
 * @return the single transfer function an RGB colour space can be built from, or `std::nullopt`.
 *
 * ## Why one function and not three
 *
 * A `ColorSpace` carries one transfer function, which is applied to every channel. An ICC profile is free to give
 * each channel its own curve, and a profiler that measured a display's channels separately usually does. Those two
 * facts cannot both hold for every profile, so a profile whose channels genuinely differ is refused rather than
 * averaged: an average would be a colour space nobody measured, and it would be applied to every pixel of every
 * image the caller converts.
 *
 * Channels that agree are the common case -- the same curve written three times -- so agreeing is accepted and
 * disagreeing is not.
 *
 * ## Approximating a table
 *
 * A curve that arrived as a sampled table has to become parameters before it can be used, which is what
 * `Curve::approximate` is for. It reports the error of the fit it found, and a fit that is not good enough comes
 * back as `std::nullopt` -- so a table that is not really a curve is refused here for free, with no second
 * threshold to keep in step with the first.
 */
std::optional<ColorTransFunc> commonTransferFunction(const std::array<gcms::Curve, 3>& curves) {
  std::array<ColorTransFunc, 3> functions {};

  for (size_t channel = 0; channel < 3; ++channel) {
    // A parametric curve is used as it is; a table is fitted and the fit is believed only if it is good.
    const auto fitted = curves[channel].approximate(256, nullptr);
    if (!fitted.has_value()) return std::nullopt;
    const auto* parameters = fitted->sRGBish();
    if (parameters == nullptr) {
      // The only other shape a profile's curve can take here is the identity, which the fit represents as a
      // parametric curve with gamma one -- so anything else means the fit found something this cannot use.
      if (fitted->kind() == gcms::Kind::Identity) {
        functions[channel] = ColorTransFunc { 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        continue;
      }
      return std::nullopt;
    }
    functions[channel] =
            ColorTransFunc { parameters->g, parameters->a, parameters->b, parameters->c, parameters->d, parameters->e, parameters->f };
  }

  // The channels have to agree, to within the precision a profile states its curves at. The tolerance is a thousandth
  // of a unit on the curve's output -- comfortably inside the error a fit is already allowed, and far below the point
  // where a viewer would see two channels diverge.
  constexpr float kTolerance = 1e-3f;
  for (size_t channel = 1; channel < 3; ++channel) {
    const ColorTransFunc& a = functions[0];
    const ColorTransFunc& b = functions[channel];
    const float difference = std::max({ std::fabs(a.g - b.g), std::fabs(a.a - b.a), std::fabs(a.b - b.b),
      std::fabs(a.c - b.c), std::fabs(a.d - b.d), std::fabs(a.e - b.e), std::fabs(a.f - b.f) });
    if (!(difference <= kTolerance)) return std::nullopt;
  }

  return functions[0];
}

} // namespace anonymous

std::shared_ptr<ColorSpace> ColorSpace::MakeFromICC(const void* data, const size_t size) {
  const auto profile = icc::parse(data, size);
  if (!profile.has_value()) return nullptr;

  // The profile's matrix is device RGB to PCS XYZ, which is the same direction `ColorSpace` stores -- and the
  // parser has already scaled the colorant columns so that they sum to the stated white point, which is the step
  // that makes the matrix a transform rather than a set of measurements.
  ColorMatrix toXyz {};
  for (size_t row = 0; row < 3; ++row) {
    for (size_t column = 0; column < 3; ++column) toXyz.values[row][column] = profile->toXyz[row][column];
  }

  const auto transferFunction = commonTransferFunction(profile->curves);
  if (!transferFunction.has_value()) return nullptr;

  // The rendering intent is read and discarded rather than stored. `ColorSpace` has no intent -- a space is a
  // space, and the intent belongs to the transform that uses it -- so keeping the number here would be a field
  // that nothing reads and every caller would assume meant something.
  (void) profile->renderingIntent;

  return MakeRGB(*transferFunction, toXyz);
}

// ==================================================================================================
// Queries
// ==================================================================================================

bool ColorSpace::gammaCloseToSRGB() const {
  // The question a renderer asks before deciding whether it can use an sRGB framebuffer for a space
  // that is not exactly sRGB. The tolerance is deliberately loose -- a quarter of the way from linear
  // to the sRGB curve's shape -- because the answer only gates whether a conversion can be skipped.
  const float gamma = mTransFunc.g;
  return std::fabs(gamma - 2.4f) < 0.6f && std::fabs(mTransFunc.a) > 0.0f;
}

bool ColorSpace::gammaIsLinear() const {
  return mTransFunc.g == 1.0f && mTransFunc.a == 1.0f && mTransFunc.b == 0.0f && mTransFunc.c == 0.0f &&
         mTransFunc.d == 0.0f && mTransFunc.e == 0.0f && mTransFunc.f == 0.0f;
}

bool ColorSpace::isNumericalTransferFn(ColorTransFunc* fn) const {
  // PQ and HLG are expressible as numbers but not in the seven-parameter form, so they have a negative
  // `g` by convention -- the caller can tell from the returned function that its own evaluator will not
  // reproduce the curve. See the note on `ColorTransFunc::g`.
  if (fn != nullptr) *fn = mTransFunc;
  return mTransFunc.g > 0.0f;
}

namespace {

/**
 * The `gcms` curve this space's parameters describe.
 *
 * PQ and HLG are marked by their negative `g` (see `ColorTransFunc::g`) and rebuilt from the named
 * constructors, which carry the standard coefficients; every other space is the seven-parameter form.
 * A curve `gcms` cannot build from the parameters falls back to the identity, which keeps
 * `decodeTransfer` total without inventing a curve nobody specified.
 */
gcms::TransferFunction evaluateCurve(const ColorTransFunc& function) {
  if (function.g == NamedTransferFn::kPQ.g) return gcms::TransferFunction::makePQ();
  if (function.g == NamedTransferFn::kHLG.g) return gcms::TransferFunction::makeHLG();
  const std::optional<gcms::TransferFunction> curve = gcms::TransferFunction::makeSRGBish(
    gcms::SRGBishParameters { function.g, function.a, function.b, function.c, function.d, function.e,
      function.f });
  return curve.value_or(gcms::TransferFunction::makeIdentity());
}

} // namespace

float ColorSpace::decodeTransfer(const float encoded) const {
  return evaluateCurve(mTransFunc).eval(encoded);
}

float ColorSpace::encodeTransfer(const float linear) const {
  return evaluateCurve(mTransFunc).encode(linear);
}

bool ColorSpace::toXYZD50(ColorMatrix* toXYZD50) const {
  if (toXYZD50 == nullptr) return false;
  *toXYZD50 = mToXYZD50;
  return true;
}

void ColorSpace::computeLazyDstFields() const {
  // The inverse and the reverse gamut are needed by `gamutTransformTo` and by any conversion into this
  // space, but not by a space that is only ever a source. Computing them on first use keeps a space
  // that is built and compared -- the common case for the named spaces -- from paying for both.
  if (mLazyDstFields.load(std::memory_order_acquire) != 0) return;

  // `mutable` plus an atomic flag rather than a lock: two threads racing here compute the same values
  // from the same inputs, so the second one's result is identical and the race is benign. The flag
  // only has to guarantee that a reader either sees the finished values or does the work itself.
  if (const auto inverse = gcms::TransferFunction::makeSRGBish(gcms::SRGBishParameters { mTransFunc.g,
              mTransFunc.a, mTransFunc.b, mTransFunc.c, mTransFunc.d, mTransFunc.e, mTransFunc.f })) {
    if (const auto inverted = inverse->inverse()) {
      if (const auto* p = inverted->sRGBish()) {
        mInvTransFunc = ColorTransFunc { p->g, p->a, p->b, p->c, p->d, p->e, p->f };
      }
    }
  }

  const gcms::Matrix3x3 matrix = toGcms(mToXYZD50);
  if (const auto inverse = gcms::invert(matrix)) {
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 3; ++col) {
        mFromXYZD50.values[row][col] = inverse->values[row][col];
      }
    }
  }

  mLazyDstFields.store(1, std::memory_order_release);
}

ColorTransFunc ColorSpace::transFunc() const { return mTransFunc; }

ColorTransFunc ColorSpace::invTransFunc() const {
  computeLazyDstFields();
  return mInvTransFunc;
}

void ColorSpace::gamutTransformTo(const ColorSpace* dst, ColorMatrix* srcToDst) const {
  if (dst == nullptr || srcToDst == nullptr) return;

  // Working in D50 throughout is what makes this composable: the source's gamut matrix takes its RGB
  // to D50, and the destination's takes D50 to its RGB. Neither profile needs to know the other's
  // white point, which is why every ICC profile is stored adapted to D50 in the first place.
  computeLazyDstFields();
  dst->computeLazyDstFields();

  const gcms::Matrix3x3 combined = gcms::concat(toGcms(dst->mFromXYZD50), toGcms(mToXYZD50));
  *srcToDst = fromGcms(combined);
}

// ==================================================================================================
// Derived spaces
// ==================================================================================================

std::shared_ptr<ColorSpace> ColorSpace::makeLinearGamma() const {
  // A new space even when this one is already linear. The alternative -- returning `this` through
  // `shared_from_this` -- would be cheaper, but it is only valid on an object that a `shared_ptr`
  // already owns, so it would turn a method that is safe on any instance into one that is not. The
  // equality of two colour spaces is their value, so a distinct instance costs nothing that matters.
  return MakeRGB(NamedTransferFn::kLinear, mToXYZD50);
}

std::shared_ptr<ColorSpace> ColorSpace::makeSRGBGamma() const {
  return MakeRGB(NamedTransferFn::kSRGB, mToXYZD50);
}

std::shared_ptr<ColorSpace> ColorSpace::makeColorSpin() const {
  // Rotating the channels turns RGB into GBR. It is deliberately a wrong-but-invertible transform: a
  // colour space that maps to a different space entirely is what a test needs to prove that a
  // conversion is actually running rather than being optimised away.
  ColorMatrix spun = mToXYZD50;
  for (int col = 0; col < 3; ++col) {
    std::swap(spun.values[0][col], spun.values[1][col]);
    std::swap(spun.values[1][col], spun.values[2][col]);
  }
  return MakeRGB(mTransFunc, spun);
}

bool ColorSpace::isSRGB() const {
  // A hash comparison would be the cheap way to answer this, and it is wrong. The sRGB gamut exists in
  // two forms in this codebase -- the published 16.16 fixed-point constants, and the matrix derived
  // from sRGB's chromaticities -- and the two differ in the last bits. Hashing quantises, so whether
  // they collide depends on where the values happen to fall either side of a quantum, and a space
  // derived from Rec.709 code points would report itself as not-sRGB.
  //
  // A hash answers "are these bit-identical", which is the right question for a cache key and the
  // wrong one for colour equality. So the values are compared directly, with the tolerance that the
  // fixed-point round trip requires.
  static const std::shared_ptr<ColorSpace> sRGB = MakeSRGB();
  const ColorTransFunc sRGBFunc = sRGB->transFunc();
  const float lhs[7] = { mTransFunc.g, mTransFunc.a, mTransFunc.b, mTransFunc.c, mTransFunc.d, mTransFunc.e,
    mTransFunc.f };
  const float rhs[7] = { sRGBFunc.g, sRGBFunc.a, sRGBFunc.b, sRGBFunc.c, sRGBFunc.d, sRGBFunc.e, sRGBFunc.f };
  for (int i = 0; i < 7; ++i) {
    if (std::fabs(lhs[i] - rhs[i]) > 1.0f / 512.0f) return false;
  }

  // The gamut, to the same tolerance. 1/512 of a unit is well below the 16.16 fixed-point step an ICC
  // profile stores, so a profile-written sRGB still compares equal, while Display P3 -- whose matrix
  // differs in the first decimal place -- does not.
  const ColorMatrix expected = sRGB->mToXYZD50;
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      if (std::fabs(mToXYZD50.values[row][col] - expected.values[row][col]) > 1.0f / 512.0f) return false;
    }
  }
  return true;
}

std::shared_ptr<Data> ColorSpace::serialize() const {
  // The profile this space would have been read from. Everything it needs is already here: the matrix is the gamut,
  // and the transfer function is carried once, so the same curve is written to all three channels -- which is what a
  // space built from `MakeRGB` means. A space whose channels genuinely differed cannot be represented at all, which
  // is why `MakeFromICC` refuses to build one.
  icc::Profile profile;
  for (size_t row = 0; row < 3; ++row) {
    for (size_t column = 0; column < 3; ++column) profile.toXyz[row][column] = mToXYZD50.values[row][column];
  }

  const auto function = gcms::TransferFunction::makeSRGBish(gcms::SRGBishParameters { mTransFunc.g, mTransFunc.a,
    mTransFunc.b, mTransFunc.c, mTransFunc.d, mTransFunc.e, mTransFunc.f });
  if (!function.has_value()) {
    // A transfer function that cannot be expressed as the seven-parameter family is one this reader could not have
    // produced, so a profile written from it would describe a different space.
    return nullptr;
  }

  profile.curves = { gcms::Curve { *function }, gcms::Curve { *function }, gcms::Curve { *function } };

  const auto bytes = icc::write(profile);
  if (!bytes.has_value()) return nullptr;

  // `Data::makeCopy` takes a copy rather than adopting the vector, which is one copy of a few hundred bytes in
  // exchange for the caller's buffer not having to share an allocation strategy with this one.
  return std::make_shared<Data>(Data::makeCopy(bytes->data(), bytes->size()));
}

} // namespace graphics
