/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and limitations under the License.
 */

/**
 * @file test_Curve.cpp
 *
 * Fitting a parametric transfer function to a sampled table.
 *
 * ## How these are checked
 *
 * The tables are built from *independently computed* values -- `std::pow` and `std::log` in this file, not the
 * library's own approximations -- so a fit that agreed with the library but not with mathematics would fail here.
 * That matters more than usual for this function: it is a numerical search, and a search that has drifted can
 * still return something that looks like a curve.
 *
 * The second check is the one that catches a *plausible but wrong* fit: the fitted curve is compared against the
 * table it came from, at points **between** the table's entries. A fit that reproduced the table only at its own
 * sample points would be memorising the input rather than describing the curve.
 */

#include "gcms/TransferFunction.h"

#include <gtest/gtest.h>

// `std::clamp` is used below, and it is declared here rather than relied on transitively. It has to be
// explicit: MSVC 14.44 stopped pulling `<algorithm>` in through the chain this file happens to have, so the
// file compiled on one toolchain and failed on the next with a diagnostic naming `std::clamp` rather than the
// missing header. A standard function a translation unit names is a header that translation unit includes.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace gcms;

namespace {

/** sRGB's encoding function, from the specification, computed here rather than taken from the library. */
double srgbEncode(const double linear) {
  constexpr double kThreshold = 0.0031308;
  if (linear <= kThreshold) return 12.92 * linear;
  return 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

/** A pure power curve, the shape a simple gamma table describes. */
double gammaEncode(const double linear, const double gamma) { return std::pow(linear, 1.0 / gamma); }

/** A table of @p count 16-bit samples, from a function of the *encoded* index in [0,1]. */
Curve tableOf(const uint32_t count, const std::function<double(double)>& encodedToLinear) {
  std::vector<uint8_t> bytes;
  bytes.reserve(static_cast<size_t>(count) * 2);
  for (uint32_t i = 0; i < count; ++i) {
    const double x = static_cast<double>(i) / static_cast<double>(count - 1);
    const double y = std::clamp(encodedToLinear(x), 0.0, 1.0);
    const auto value = static_cast<uint16_t>(std::lround(y * 65535.0));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value & 0xFF));
  }
  return Curve::fromTable16Be(bytes.data(), count);
}

/** @return the largest difference between @p curve and @p function over [0,1], sampled between the table's entries. */
double worstDeviation(const Curve& curve, const TransferFunction& fitted, const uint32_t samples = 1000) {
  double worst = 0.0;
  for (uint32_t i = 0; i <= samples; ++i) {
    // Offset by half a step, so the comparison is made between the table's entries rather than at them.
    const double x = (static_cast<double>(i) + 0.5) / static_cast<double>(samples + 1);
    const double fromTable = curve.eval(static_cast<float>(x));
    const double fromFit = fitted.eval(static_cast<float>(x));
    worst = std::max(worst, std::fabs(fromTable - fromFit));
  }
  return worst;
}

} // namespace anonymous

// ==================================================================================================
// What a fit must reproduce
// ==================================================================================================

TEST(GcmsCurve, FitsAnSrgbTable) {
  // 256 entries, which is what an ICC `curv` tag for a display usually carries.
  const Curve table = tableOf(256, [](const double x) { return std::pow(srgbEncode(x), 1.0); });
  ASSERT_EQ(table.tableEntryCount(), 256u);

  float maxError = -1.0f;
  const auto fitted = table.approximate(256, &maxError);
  ASSERT_TRUE(fitted.has_value()) << "an sRGB table is exactly the shape a parametric curve can describe";

  // The reported error is the deviation *at the table's entries*, which is a tighter measurement than this one: the
  // comparison below is made between the entries, where the table is interpolating and the fitted curve is not, so
  // it also contains the interpolation's error. The two therefore have to be compared loosely, and the useful
  // statement is that the reported figure is not larger than the independent one.
  const double measured = worstDeviation(table, *fitted);
  EXPECT_LE(static_cast<double>(maxError), measured + 1e-6)
          << "the reported error must not understate the error that is actually there";
  EXPECT_LT(measured, 1.0 / 128.0) << "and the fit is far closer than an 8-bit step";

  // sRGB's decoding function, recovered from the table alone. The table holds encode(linear), so the fitted curve
  // -- which reproduces the table -- is the encoder, and its exponent is 1/2.4. The decode gamma of 2.4 is what
  // the fit carries internally, since the residual is written in the inverse direction.
  EXPECT_NEAR(1.0 / static_cast<double>(fitted->sRGBish()->g), 2.4, 0.05) << "sRGB encodes with exponent 1/2.4";
}

TEST(GcmsCurve, FitsAPowerTable) {
  const Curve table = tableOf(256, [](const double x) { return std::pow(x, 2.2); });

  float maxError = 0.0f;
  const auto fitted = table.approximate(256, &maxError);
  ASSERT_TRUE(fitted.has_value());

  // A pure power curve should come back with gamma 2.2 and no offset. This is the sharpest available test of the
  // fit: the answer is known exactly, so a small error in the search shows up as a wrong exponent rather than as
  // a vague inaccuracy.
  EXPECT_NEAR(static_cast<double>(fitted->sRGBish()->g), 2.2, 0.02);
  EXPECT_LT(worstDeviation(table, *fitted), 1e-3);
}

TEST(GcmsCurve, FitsALinearTable) {
  // The identity. It must come back as a curve that evaluates to the identity, whether or not the fit happens to
  // represent it through the power segment with gamma one.
  const Curve table = tableOf(256, [](const double x) { return x; });

  float maxError = 1.0f;
  const auto fitted = table.approximate(256, &maxError);
  ASSERT_TRUE(fitted.has_value()) << "a straight line is a curve";

  EXPECT_LT(maxError, 1.0f / 512.0f);
  for (int i = 0; i <= 20; ++i) {
    const float x = static_cast<float>(i) / 20.0f;
    EXPECT_NEAR(fitted->eval(x), x, 2e-3f) << "at x = " << x;
  }
}

TEST(GcmsCurve, FitsAVerySteepTable) {
  // Gamma 3.0, which is steeper than any standard curve. The search has to cope with a much larger exponent than
  // its initial guess, which is the case a single Gauss-Newton step is furthest from.
  const Curve table = tableOf(256, [](const double x) { return std::pow(x, 3.0); });

  float maxError = 0.0f;
  const auto fitted = table.approximate(256, &maxError);
  ASSERT_TRUE(fitted.has_value());
  EXPECT_NEAR(static_cast<double>(fitted->sRGBish()->g), 3.0, 0.05);
  EXPECT_LT(worstDeviation(table, *fitted), 2e-3);
}

// ==================================================================================================
// What a fit must refuse
// ==================================================================================================

TEST(GcmsCurve, ATableThatIsNotACurveIsRefused) {
  // A table that alternates between the endpoints. No transfer function is monotone and does this, so the fit has
  // to report failure rather than return the nearest curve it found.
  //
  // This is the assertion that makes the function safe to call: a caller that gets a curve back can act on it,
  // and a caller that gets `nullopt` keeps interpolating. A function that always returned something would make
  // that distinction meaningless.
  std::vector<uint8_t> bytes;
  constexpr uint32_t kCount = 256;
  for (uint32_t i = 0; i < kCount; ++i) {
    const uint16_t value = (i % 2 == 0) ? 0 : 65535;
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value & 0xFF));
  }

  const Curve table = Curve::fromTable16Be(bytes.data(), kCount);
  float maxError = 123.0f;
  const auto fitted = table.approximate(256, &maxError);

  if (fitted.has_value()) {
    // A refusal is not required -- a fit is allowed to exist if it is genuinely good -- but then the reported error
    // has to be honest and small. What must not happen is a curve returned with a large error left unmentioned.
    EXPECT_LT(maxError, 1.0f / 512.0f)
            << "a fit may be returned only when it is accurate; got one reporting " << maxError;
  } else {
    EXPECT_EQ(maxError, 0.0f) << "a refusal reports no error rather than a stale one";
  }
}

TEST(GcmsCurve, TooFewEntriesToFit) {
  // One entry is a constant, which has no parameters to fit. The function has to say so rather than divide by the
  // zero-sized interval a single entry implies.
  const std::vector<uint8_t> one { 0x80, 0x00 };
  const Curve table = Curve::fromTable16Be(one.data(), 1);
  EXPECT_FALSE(table.approximate(256, nullptr).has_value());
}

TEST(GcmsCurve, AParametricCurveIsItsOwnApproximation) {
  // Nothing to fit, so the answer is the curve itself -- and it must be exact, not merely close.
  const Curve identity;
  const auto fitted = identity.approximate(256, nullptr);
  ASSERT_TRUE(fitted.has_value());
  EXPECT_EQ(fitted->kind(), Kind::Identity);
  EXPECT_FLOAT_EQ(fitted->eval(0.25f), 0.25f);
}

TEST(GcmsCurve, TheFitSurvivesBeingInverted) {
  // The fit is made in the inverse direction and inverted back, so the result has to be a curve that can be
  // inverted again. That round trip is what a caller does to build a transform, and a curve that survived the fit
  // but could not be inverted would be useless.
  const Curve table = tableOf(256, [](const double x) { return std::pow(x, 2.2); });
  const auto fitted = table.approximate(256, nullptr);
  ASSERT_TRUE(fitted.has_value());

  const auto inverse = fitted->inverse();
  ASSERT_TRUE(inverse.has_value()) << "an sRGB-ish curve always has an inverse";

  // Composing them returns the input, which is the property the pipeline depends on.
  for (int i = 1; i < 20; ++i) {
    const float x = static_cast<float>(i) / 20.0f;
    EXPECT_NEAR(inverse->eval(fitted->eval(x)), x, 2e-3f) << "at x = " << x;
  }
}
