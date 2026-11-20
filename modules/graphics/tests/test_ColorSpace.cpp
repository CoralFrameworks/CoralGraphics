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

/**
 * @file test_ColorSpace.cpp
 *
 * The colour management layer: `gcms`'s primitives and the `graphics::ColorSpace` built on them.
 *
 * The most valuable case here is @c SrgbGamutMatchesTheReferenceConstants. Coral computes the sRGB
 * gamut from its chromaticities rather than copying the published matrix, which is the right call for
 * arbitrary profiles but means the well-known result is no longer a literal that cannot be wrong. That
 * test pins the computed matrix against the constants so a mistake in the primaries arithmetic cannot
 * hide behind a plausible-looking number.
 */

#include <graphics/ColorSpace.h>

#include "gcms/Gamut.h"
#include "gcms/Numeric.h"
#include "gcms/TransferFunction.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace graphics;

namespace {

/** Asserts that two floats agree to within @p tolerance. */
void expectNear(const float actual, const float expected, const float tolerance, const char* what) {
  EXPECT_NEAR(actual, expected, tolerance) << what;
}

} // namespace anonymous

// ==================================================================================================
// gcms: primitives
// ==================================================================================================

TEST(Gcms, InvertsAndComposesMatrices) {
  const auto identity = gcms::Matrix3x3::identity();
  const auto inverse = gcms::invert(identity);
  ASSERT_TRUE(inverse.has_value());
  expectNear(inverse->at(1, 1), 1.0f, 1e-6f, "identity inverse");

  // A matrix that is not its own inverse, so the round trip is actually exercised.
  const auto m = gcms::Matrix3x3::fromRows(2.0f, 0.0f, 0.0f, 0.0f, 4.0f, 0.0f, 0.0f, 0.0f, 8.0f);
  const auto mInv = gcms::invert(m);
  ASSERT_TRUE(mInv.has_value());

  const auto product = gcms::concat(m, *mInv);
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      expectNear(product.at(row, col), row == col ? 1.0f : 0.0f, 1e-5f, "M * M^-1");
    }
  }
}

TEST(Gcms, RejectsASingularMatrix) {
  // The third row is the sum of the first two, so there is no inverse. Returning one would hand back
  // infinities that silently destroy every colour passing through it.
  const auto singular = gcms::Matrix3x3::fromRows(1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 5.0f, 7.0f, 9.0f);
  EXPECT_FALSE(gcms::invert(singular).has_value());
}

// ==================================================================================================
// gcms: transfer functions
// ==================================================================================================

TEST(Gcms, SrgbCurveHasTheExpectedShape) {
  const auto sRGB = gcms::TransferFunction::makeSRGB();

  expectNear(sRGB.eval(0.0f), 0.0f, 1e-6f, "black maps to black");
  expectNear(sRGB.eval(1.0f), 1.0f, 1e-6f, "white maps to white");

  // Below the join the curve is exactly linear, which is the whole point of the sRGB piecewise form.
  expectNear(sRGB.eval(0.02f), (1.0f / 12.92f) * 0.02f, 1e-7f, "linear segment");

  // The curve is odd-symmetric, so a value below zero continues it rather than clamping.
  expectNear(sRGB.eval(-0.5f), -sRGB.eval(0.5f), 1e-6f, "odd symmetry");
}

TEST(Gcms, SrgbInverseRoundTrips) {
  const auto sRGB = gcms::TransferFunction::makeSRGB();
  const auto inverse = sRGB.inverse();
  ASSERT_TRUE(inverse.has_value());

  // Every sample must survive the round trip. A curve that is only correct at the endpoints would pass
  // a weaker test and fail visibly in the middle of a gradient.
  for (int i = 0; i <= 64; ++i) {
    const float x = static_cast<float>(i) / 64.0f;
    expectNear(inverse->eval(sRGB.eval(x)), x, 1e-4f, "sRGB round trip");
  }

  // The endpoint invariant specifically: without the correction `inverse` applies, white drifts and
  // stops being white after a few conversions.
  expectNear(inverse->eval(sRGB.eval(1.0f)), 1.0f, 1e-5f, "white stays white");
}

TEST(Gcms, PureGammaInverts) {
  const auto gamma = gcms::TransferFunction::makeGamma(2.2f);
  expectNear(gamma.eval(0.5f), std::pow(0.5f, 2.2f), 1e-3f, "gamma 2.2");

  const auto inverse = gamma.inverse();
  ASSERT_TRUE(inverse.has_value());
  for (int i = 1; i <= 64; ++i) {
    const float x = static_cast<float>(i) / 64.0f;
    // The bound is 1e-3 rather than something tighter because the curve is evaluated with `powApprox`,
    // the fast polynomial approximation the module deliberately uses for cross-platform determinism,
    // and the round trip applies it twice. See the note on `powApprox`: it is accurate to well under a
    // percent, so demanding exactness here would be testing `std::pow` rather than this module.
    expectNear(inverse->eval(gamma.eval(x)), x, 1e-3f, "gamma round trip");
  }
}

TEST(Gcms, TheStandardPqCurveInverts) {
  // `makePQ` is the standard ST 2084 curve, and it is the same curve as the canonical `PQish` below -- but written
  // with its coefficients in `eval` rather than stored, which is why the two took different code paths through
  // `inverse`.
  //
  // The path for `Kind::PQ` was missing, so this call returned no inverse at all while inverting the identical curve
  // as an explicit `PQish` worked. Nothing caught it: the existing test covers the `PQish` spelling, and MSVC does not
  // warn about an unhandled enumerator in a switch. Clang's `-Wswitch` does, which is how it was found.
  const auto pq = gcms::TransferFunction::makePQ();
  EXPECT_EQ(pq.kind(), gcms::Kind::PQ);

  const auto inverse = pq.inverse();
  ASSERT_TRUE(inverse.has_value()) << "the standard PQ curve has an inverse, and it is the same one as its PQish form";

  // The round trip, over the range the curve is defined on. The bound matches `PqishInverts` because it is the same
  // arithmetic through `powApprox`.
  for (int i = 1; i < 64; ++i) {
    const float x = static_cast<float>(i) / 64.0f;
    expectNear(inverse->eval(pq.eval(x)), x, 2e-3f, "PQ round trip at x");
  }

  // And the two spellings agree, so a caller that recognises the canonical coefficients gets the same curve as one
  // that names it.
  const auto spelledOut = gcms::TransferFunction::makePQish(-107.0f / 128.0f, 1.0f, 32.0f / 2523.0f, 2413.0f / 128.0f,
          -2392.0f / 128.0f, 8192.0f / 1305.0f);
  ASSERT_TRUE(spelledOut.has_value());
  for (int i = 1; i < 32; ++i) {
    const float x = static_cast<float>(i) / 32.0f;
    expectNear(pq.eval(x), spelledOut->eval(x), 1e-4f, "the named PQ curve and its explicit form agree");
  }
}

TEST(Gcms, PqishInverts) {
  // The canonical PQ coefficients, which is what a profile carrying ST 2084 uses.
  const auto pq = gcms::TransferFunction::makePQish(-107.0f / 128.0f, 1.0f, 32.0f / 2523.0f, 2413.0f / 128.0f,
          -2392.0f / 128.0f, 8192.0f / 1305.0f);
  ASSERT_TRUE(pq.has_value());
  EXPECT_EQ(pq->kind(), gcms::Kind::PQish);

  const auto inverse = pq->inverse();
  ASSERT_TRUE(inverse.has_value());
  for (int i = 1; i < 64; ++i) {
    const float x = static_cast<float>(i) / 64.0f;
    expectNear(inverse->eval(pq->eval(x)), x, 2e-3f, "PQish round trip");
  }
}

TEST(Gcms, HybridLogGammaRoundTripsAcrossItsWholeDomain) {
  // The property the removed attempt could not meet, and the one this is written against: encode then decode returns
  // the input, everywhere, including at the join between the branches.
  const gcms::TransferFunction hlg = gcms::TransferFunction::makeHLG();

  // The join is at an encoded 0.5, and that is the trap the removed attempt fell into: `12 * 0.5` makes 1/12 look like
  // the matching point on the scene-linear side, and it is not -- the OETF's break is where its branches *meet*, which
  // is a scene-linear one. The samples below are clustered around both, because a wrong break agrees with the right one
  // almost everywhere else.
  const auto close = [&hlg](const float x, const char* what) {
    const float linear = hlg.eval(x);
    const float back = hlg.encode(linear);

    // A part in 512, which is the tolerance `Curve::isApproximateInverse` uses and well below one 8-bit step.
    EXPECT_NEAR(back, x, 1.0f / 512.0f) << what << ": encode(eval(" << x << ")) should return " << x;
  };

  for (int i = 0; i <= 1000; ++i) close(static_cast<float>(i) / 1000.0f, "across the encoded domain");

  // The join, and either side of it, at a resolution the sweep above does not reach.
  for (const float x: { 0.4999f, 0.5f, 0.5001f, 0.25f, 0.75f, 0.0f, 1.0f }) {
    close(x, "at the join and the endpoints");
  }

  // The curve's identity, which is what makes it BT.2100 rather than something shaped like it. Both branches give
  // exactly one half at the join, and the endpoints are the ends of the range.
  EXPECT_NEAR(hlg.eval(0.5f), 1.0f / 12.0f, 1e-4f)
          << "an encoded one half is scene-linear 1/12 -- the OETF's break, seen from the other side";
  EXPECT_FLOAT_EQ(hlg.eval(0.0f), 0.0f) << "black";
  EXPECT_NEAR(hlg.eval(1.0f), 1.0f, 1e-4f) << "peak white, because the curve is normalised by the 12x headroom";

  // Those two join values are the whole reason the constants are what they are, so they are checked as a pair: an
  // implementation whose `b` or `c` had drifted would be continuous at one of them and not the other.
  EXPECT_NEAR(hlg.eval(0.5f) * 12.0f, 1.0f, 1e-3f) << "and the same point in the scene-linear domain";
}

TEST(Gcms, HybridLogGammaIsItsOwnFamily) {
  // Its two directions are different functions rather than one expression with two sets of coefficients, so it is a
  // family of its own and `inverse()` has no curve to return. `encode` performs the operation instead, which is the
  // distinction the header draws.
  const gcms::TransferFunction hlg = gcms::TransferFunction::makeHLG();

  EXPECT_EQ(hlg.kind(), gcms::Kind::HLG);
  EXPECT_EQ(hlg.inverse(), std::nullopt) << "there is no seven-parameter curve that is HLG's encode";

  // The parameters say what the curve is referenced to, so a caller reading them learns something.
  const auto* parameters = std::get_if<gcms::HLGParameters>(&hlg.parameters());
  ASSERT_NE(parameters, nullptr);
  EXPECT_FLOAT_EQ(parameters->referenceWhiteLuminance, 1000.0f) << "the BT.2100 reference display";

  // Equality is by value, so two HLG curves compare equal and an HLG curve does not equal sRGB.
  EXPECT_EQ(gcms::TransferFunction::makeHLG(), gcms::TransferFunction::makeHLG());
  EXPECT_NE(hlg, gcms::TransferFunction::makeSRGB());
}

TEST(ColorSpaceTest, CicpReachesBothHdrTransferFunctions) {
  // Wiring HLG into the CICP switch is what exposed this: the validation below that switch asked a seven-parameter
  // constructor whether the function was usable, and that constructor refuses a negative `g` -- which is the convention
  // this file uses to mark PQ and HLG. So PQ was refused too, and had been since it was added. The only CICP test used
  // Rec.709 and Rec.2020, both ordinary curves, so nothing noticed.
  const auto pq = ColorSpace::MakeCICP(NamedPrimaries::CicpId::kRec2020, NamedTransferFn::CicpId::kPQ);
  ASSERT_NE(pq, nullptr) << "PQ is an HDR transfer function and is reachable by its H.273 code point";

  const auto hlg = ColorSpace::MakeCICP(NamedPrimaries::CicpId::kRec2020, NamedTransferFn::CicpId::kHLG);
  ASSERT_NE(hlg, nullptr) << "H.273 value 18 is HLG";

  // And they are not the same space: the two curves differ by a great deal at a mid grey.
  EXPECT_NE(pq.get(), hlg.get());

  // A reserved code point is still refused rather than guessed at.
  EXPECT_EQ(ColorSpace::MakeCICP(NamedPrimaries::CicpId::kRec2020, static_cast<NamedTransferFn::CicpId>(0)), nullptr);
}

TEST(Gcms, CurveTableInterpolatesAndClamps) {
  const auto sRGB = gcms::TransferFunction::makeSRGB();

  // A table sampled from the curve it approximates must track that curve, and must agree exactly at
  // the endpoints -- an off-by-one in the index would show up here and nowhere else.
  uint8_t samples[256];
  for (int i = 0; i < 256; ++i) {
    samples[i] = static_cast<uint8_t>(std::lround(sRGB.eval(static_cast<float>(i) / 255.0f) * 255.0f));
  }
  const gcms::Curve curve = gcms::Curve::fromTable8(samples, 256);

  EXPECT_EQ(curve.tableEntryCount(), 256u);
  EXPECT_EQ(curve.function(), nullptr) << "a table is not a parametric curve";
  expectNear(curve.eval(0.0f), 0.0f, 1e-6f, "table(0)");
  expectNear(curve.eval(1.0f), 1.0f, 1e-6f, "table(1)");
  expectNear(curve.eval(0.5f), sRGB.eval(0.5f), 2e-3f, "table tracks the curve it sampled");

  // Callers filter images, which produces values outside the unit interval. Clamping matches what a
  // GPU sampler does with the same input, and keeps the index in range.
  expectNear(curve.eval(-1.0f), curve.eval(0.0f), 1e-7f, "clamps below zero");
  expectNear(curve.eval(2.0f), curve.eval(1.0f), 1e-7f, "clamps above one");

  // A table sampled from sRGB is *not* within the 1/512 bound that `isApproximateInverse` applies.
  // That is a property of sRGB, not of this implementation: its two segments meet with a slope change
  // of two orders of magnitude, so an 8-bit table cannot resolve the join and the round-trip error
  // peaks there at around 9e-3. Pinning the measured behaviour is more useful than a threshold that
  // would only pass for a curve smoother than the one every display actually uses.
  const auto inverse = sRGB.inverse();
  ASSERT_TRUE(inverse.has_value());
  EXPECT_FALSE(curve.isApproximateInverse(*inverse)) << "sRGB is not smooth enough for the 1/512 bound";

  // The measured worst case, which is worth stating: roughly 2.4% of a unit, concentrated at the join.
  // The reference's own threshold is not a claim that every curve meets it -- it is a scale test that
  // sRGB happens to fail, because a uniform table cannot track a slope discontinuity any closer than
  // the entry spacing allows.
  EXPECT_LT(curve.maxRoundtripError(*inverse), 3e-2f) << "but it is still a close approximation";
}

// ==================================================================================================
// graphics::ColorSpace
// ==================================================================================================

TEST(ColorSpaceTest, SrgbGamutMatchesTheReferenceConstants) {
  // Coral derives this matrix from sRGB's chromaticities rather than copying it, so it is worth
  // checking that the derivation lands where the published constants say it should. If the primaries
  // arithmetic were wrong -- columns swapped, the white point not solved for -- this would be the test
  // that catches it, and nothing else would.
  const ColorSpacePrimaries sRGB = NamedPrimaries::kRec709;
  ColorMatrix computed {};
  ASSERT_TRUE(sRGB.toXYZD50(&computed));

  const ColorMatrix expected = NamedGamut::kSRGB;
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      EXPECT_NEAR(computed.values[row][col], expected.values[row][col], 1.0f / 512.0f)
              << "row " << row << " col " << col;
    }
  }
}

TEST(ColorSpaceTest, NamedSpacesHaveTheExpectedShape) {
  const auto sRGB = ColorSpace::MakeSRGB();
  ASSERT_NE(sRGB, nullptr);
  EXPECT_TRUE(sRGB->isSRGB());
  EXPECT_TRUE(sRGB->gammaCloseToSRGB());
  EXPECT_FALSE(sRGB->gammaIsLinear());

  const auto linear = ColorSpace::MakeSRGBLinear();
  ASSERT_NE(linear, nullptr);
  EXPECT_TRUE(linear->gammaIsLinear());
  EXPECT_FALSE(linear->gammaCloseToSRGB());
  EXPECT_FALSE(linear->isSRGB()) << "a linear space is not sRGB";

  // Both share the sRGB gamut, which is the property that makes "convert to linear and back" free of
  // any gamut change.
  ColorMatrix sRGBGamut {};
  ColorMatrix linearGamut {};
  ASSERT_TRUE(sRGB->toXYZD50(&sRGBGamut));
  ASSERT_TRUE(linear->toXYZD50(&linearGamut));
  EXPECT_EQ(sRGB->toXYZD50Hash(), linear->toXYZD50Hash());
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      EXPECT_FLOAT_EQ(sRGBGamut.values[row][col], linearGamut.values[row][col]);
    }
  }

  const auto& p3 = ColorSpace::DisplayP3();
  ASSERT_NE(p3, nullptr);
  EXPECT_FALSE(p3->isSRGB()) << "Display P3 has a wider gamut than sRGB";
  EXPECT_TRUE(p3->gammaCloseToSRGB()) << "Display P3 does use the sRGB transfer function";
  EXPECT_NE(p3->toXYZD50Hash(), sRGB->toXYZD50Hash());
}

TEST(ColorSpaceTest, TransferFunctionIsReportedAndHashIsStable) {
  const auto sRGB = ColorSpace::MakeSRGB();

  ColorTransFunc reported {};
  ASSERT_TRUE(sRGB->isNumericalTransferFn(&reported));
  expectNear(reported.g, 2.4f, 1e-6f, "sRGB gamma");

  // The hash has to be stable across instances of the same space, because it is what a cache key and
  // an equality test are built from.
  const auto again = ColorSpace::MakeSRGB();
  EXPECT_EQ(sRGB->hash(), again->hash());
  EXPECT_EQ(sRGB->transFuncHash(), again->transFuncHash());

  // And distinct for a space that differs.
  EXPECT_NE(sRGB->hash(), ColorSpace::MakeSRGBLinear()->hash());
}

TEST(ColorSpaceTest, InverseTransferFunctionUndoesTheForwardOne) {
  const auto sRGB = ColorSpace::MakeSRGB();
  const ColorTransFunc forward = sRGB->transFunc();
  const ColorTransFunc inverse = sRGB->invTransFunc();

  // Evaluating the inverse through gcms, which is the same evaluator the pipeline uses.
  const auto inverseFn = gcms::TransferFunction::makeSRGBish(gcms::SRGBishParameters { inverse.g, inverse.a,
          inverse.b, inverse.c, inverse.d, inverse.e, inverse.f });
  ASSERT_TRUE(inverseFn.has_value()) << "the computed inverse must itself be a usable curve";

  for (int i = 0; i <= 32; ++i) {
    const float x = static_cast<float>(i) / 32.0f;
    const auto forwardFn = gcms::TransferFunction::makeSRGBish(gcms::SRGBishParameters { forward.g, forward.a,
            forward.b, forward.c, forward.d, forward.e, forward.f });
    ASSERT_TRUE(forwardFn.has_value());
    expectNear(inverseFn->eval(forwardFn->eval(x)), x, 1e-3f, "forward then inverse");
  }
}

TEST(ColorSpaceTest, GamutTransformComposesBackToIdentity) {
  const auto sRGB = ColorSpace::MakeSRGB();
  const auto& p3 = ColorSpace::DisplayP3();

  ColorMatrix sRGBToP3 {};
  sRGB->gamutTransformTo(p3.get(), &sRGBToP3);

  ColorMatrix p3ToSRGB {};
  p3->gamutTransformTo(sRGB.get(), &p3ToSRGB);

  // sRGB -> P3 -> sRGB must be the identity, which is the check that both directions were built from
  // the right pair of matrices rather than the same one twice.
  const auto roundTrip = gcms::concat(
          gcms::Matrix3x3::fromRows(p3ToSRGB.values[0][0], p3ToSRGB.values[0][1], p3ToSRGB.values[0][2],
                  p3ToSRGB.values[1][0], p3ToSRGB.values[1][1], p3ToSRGB.values[1][2],
                  p3ToSRGB.values[2][0], p3ToSRGB.values[2][1], p3ToSRGB.values[2][2]),
          gcms::Matrix3x3::fromRows(sRGBToP3.values[0][0], sRGBToP3.values[0][1], sRGBToP3.values[0][2],
                  sRGBToP3.values[1][0], sRGBToP3.values[1][1], sRGBToP3.values[1][2],
                  sRGBToP3.values[2][0], sRGBToP3.values[2][1], sRGBToP3.values[2][2]));

  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      EXPECT_NEAR(roundTrip.at(row, col), row == col ? 1.0f : 0.0f, 1e-3f) << "round trip element";
    }
  }
}

TEST(ColorSpaceTest, DerivedSpacesPreserveTheirGamut) {
  const auto p3 = ColorSpace::DisplayP3();

  const auto linearP3 = p3->makeLinearGamma();
  ASSERT_NE(linearP3, nullptr);
  EXPECT_TRUE(linearP3->gammaIsLinear());
  EXPECT_EQ(linearP3->toXYZD50Hash(), p3->toXYZD50Hash()) << "only the gamma should have changed";

  const auto spun = p3->makeColorSpin();
  ASSERT_NE(spun, nullptr);
  EXPECT_NE(spun->toXYZD50Hash(), p3->toXYZD50Hash()) << "a spin must change the gamut";
  EXPECT_EQ(spun->transFuncHash(), p3->transFuncHash()) << "a spin must not change the gamma";
}

TEST(ColorSpaceTest, CicpNamesMapToSpaces) {
  // Rec.709 primaries with the sRGB transfer function is sRGB, reached through the H.273 code points.
  const auto rec709 = ColorSpace::MakeCICP(NamedPrimaries::CicpId::kRec709, NamedTransferFn::CicpId::kIEC61966_2_1);
  ASSERT_NE(rec709, nullptr);
  EXPECT_TRUE(rec709->isSRGB());

  // Rec.2020 uses Rec.709's transfer function but a much wider gamut, so the two must not collide.
  const auto rec2020 = ColorSpace::MakeCICP(NamedPrimaries::CicpId::kRec2020, NamedTransferFn::CicpId::kRec2020_10bit);
  ASSERT_NE(rec2020, nullptr);
  EXPECT_FALSE(rec2020->isSRGB());

  // A reserved code point has no colour space, and saying so is better than guessing at one.
  EXPECT_EQ(ColorSpace::MakeCICP(static_cast<NamedPrimaries::CicpId>(0),
                    NamedTransferFn::CicpId::kRec709),
          nullptr);
}
