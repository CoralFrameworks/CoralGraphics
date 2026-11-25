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

/**
 * @file test_ColorSpaceXform.cpp
 *
 * The pixel conversion between colour spaces. The cases that matter:
 *
 * - a round trip through a wider SDR space must come back to where it started (the whole contract
 *   the renderer relies on when it converts an image to sRGB);
 * - a neutral grey must survive unchanged, because both spaces share a D65 white point and a matrix
 *   that moved grey would tint every converted photo;
 * - alpha is not colour and must not move;
 * - HDR content is mapped by its own rule (BT.2408 white, clip above it) instead of being treated as
 *   sRGB, and the scene-referred HLG family is refused rather than guessed at.
 */

#include <graphics/ColorSpaceXform.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

using namespace graphics;

namespace {

std::shared_ptr<ColorSpace> srgb() {
  return ColorSpace::MakeSRGB();
}

std::shared_ptr<ColorSpace> displayP3() {
  return ColorSpace::DisplayP3();
}

std::shared_ptr<ColorSpace> pqRec2020() {
  return ColorSpace::MakeCICP(NamedPrimaries::CicpId::kRec2020, NamedTransferFn::CicpId::kPQ);
}

std::shared_ptr<ColorSpace> hlgRec2020() {
  return ColorSpace::MakeCICP(NamedPrimaries::CicpId::kRec2020, NamedTransferFn::CicpId::kHLG);
}

std::vector<uint8_t> singlePixel(const uint8_t red, const uint8_t green, const uint8_t blue,
    const uint8_t alpha = 255) {
  return { red, green, blue, alpha };
}

/** Converts one pixel @p from → @p to and returns the result. */
std::vector<uint8_t> convertOne(const std::vector<uint8_t>& pixel, const ColorSpace& from,
    const ColorSpace& to) {
  std::vector<uint8_t> result = pixel;
  const auto xform = ColorSpaceXform::Make(from, to);
  if (xform != nullptr) xform->applyRgba8(result.data(), 1);
  return result;
}

/** The 8-bit code a PQ space uses for an absolute luminance in cd/m^2. */
uint8_t pqCodeFor(const ColorSpace& pq, const float nits) {
  return static_cast<uint8_t>(
    std::lround(std::clamp(pq.encodeTransfer(nits / 10000.0f), 0.0f, 1.0f) * 255.0f));
}

void expectPixelNear(const std::vector<uint8_t>& actual, const uint8_t red, const uint8_t green,
    const uint8_t blue, const int tolerance, const char* what) {
  EXPECT_NEAR(static_cast<int>(actual[0]), static_cast<int>(red), tolerance) << what << " red";
  EXPECT_NEAR(static_cast<int>(actual[1]), static_cast<int>(green), tolerance) << what << " green";
  EXPECT_NEAR(static_cast<int>(actual[2]), static_cast<int>(blue), tolerance) << what << " blue";
}

} // namespace

TEST(ColorSpaceXform, RefusesIdentityAndUnsupportedFamilies) {
  EXPECT_EQ(ColorSpaceXform::Make(*srgb(), *srgb()), nullptr)
    << "equal spaces need no conversion";

  const auto hlg = hlgRec2020();
  ASSERT_NE(hlg, nullptr);
  EXPECT_EQ(ColorSpaceXform::Make(*hlg, *srgb()), nullptr)
    << "HLG is scene-referred; mapping it to SDR white is a display decision";

  const auto pq = pqRec2020();
  ASSERT_NE(pq, nullptr);
  EXPECT_EQ(ColorSpaceXform::Make(*srgb(), *pq), nullptr)
    << "encoding into PQ is the HDR output stage's job";
}

TEST(ColorSpaceXform, RoundTripsSrgbThroughDisplayP3) {
  const auto p3 = displayP3();
  ASSERT_NE(p3, nullptr);
  const auto toP3 = ColorSpaceXform::Make(*srgb(), *p3);
  const auto fromP3 = ColorSpaceXform::Make(*p3, *srgb());
  ASSERT_NE(toP3, nullptr);
  ASSERT_NE(fromP3, nullptr);

  const std::vector<uint8_t> samples = { 255, 0, 0, 0, 255, 0, 0, 0, 255, 12, 34, 200, 128, 128, 128 };
  for (size_t index = 0; index < samples.size(); index += 3) {
    std::vector<uint8_t> pixel = { samples[index], samples[index + 1], samples[index + 2], 255 };
    const std::vector<uint8_t> original = pixel;
    toP3->applyRgba8(pixel.data(), 1);
    fromP3->applyRgba8(pixel.data(), 1);
    // Two code values of slack: each direction quantizes to 8 bits, and a near-zero channel of a
    // saturated colour is exactly where that double rounding shows.
    expectPixelNear(pixel, original[0], original[1], original[2], 2,
      "an sRGB colour that went through P3 and back");
    EXPECT_EQ(pixel[3], 255) << "alpha is untouched";
  }
}

TEST(ColorSpaceXform, NeutralGreySurvivesTheGamutChange) {
  // Both spaces are D65, so grey must not move: a non-identity result here means the white-point
  // adaptation is wrong and every converted photograph would be tinted.
  const auto p3 = displayP3();
  ASSERT_NE(p3, nullptr);
  const std::vector<uint8_t> grey = singlePixel(128, 128, 128);
  const std::vector<uint8_t> converted = convertOne(grey, *p3, *srgb());
  expectPixelNear(converted, 128, 128, 128, 1, "a neutral grey");
}

TEST(ColorSpaceXform, MapsPqWhiteToSdrWhite) {
  const auto pq = pqRec2020();
  const auto working = srgb();
  ASSERT_NE(pq, nullptr);
  ASSERT_NE(working, nullptr);
  const auto xform = ColorSpaceXform::Make(*pq, *working);
  ASSERT_NE(xform, nullptr) << "PQ content is convertible";

  // A PQ pixel at the BT.2408 diffuse-white luminance (203 cd/m^2) becomes sRGB white, and one at
  // 100 cd/m^2 becomes the sRGB code for 100/203.
  std::vector<uint8_t> white = singlePixel(pqCodeFor(*pq, 203.0f), pqCodeFor(*pq, 203.0f),
    pqCodeFor(*pq, 203.0f));
  xform->applyRgba8(white.data(), 1);
  expectPixelNear(white, 255, 255, 255, 1, "PQ diffuse white");

  const uint8_t expected100 = static_cast<uint8_t>(
    std::lround(std::clamp(working->encodeTransfer(100.0f / 203.0f), 0.0f, 1.0f) * 255.0f));
  std::vector<uint8_t> mid = singlePixel(pqCodeFor(*pq, 100.0f), pqCodeFor(*pq, 100.0f),
    pqCodeFor(*pq, 100.0f));
  xform->applyRgba8(mid.data(), 1);
  expectPixelNear(mid, expected100, expected100, expected100, 2, "100 cd/m^2 in PQ");

  // Above the SDR white the policy clips rather than rolling off; the number is documented because
  // a tone-mapped highlight would come out dimmer and that is a different feature.
  std::vector<uint8_t> highlight = singlePixel(pqCodeFor(*pq, 1000.0f), pqCodeFor(*pq, 1000.0f),
    pqCodeFor(*pq, 1000.0f));
  xform->applyRgba8(highlight.data(), 1);
  expectPixelNear(highlight, 255, 255, 255, 0, "1000 cd/m^2 clips at white");
}

TEST(ColorSpaceXform, PreservesAlpha) {
  const auto p3 = displayP3();
  ASSERT_NE(p3, nullptr);
  const std::vector<uint8_t> translucent = singlePixel(200, 40, 10, 77);
  const std::vector<uint8_t> converted = convertOne(translucent, *p3, *srgb());
  EXPECT_EQ(converted[3], 77) << "alpha is not colour";
}

TEST(ColorSpaceXform, ConvertsRowsInPlace) {
  const auto p3 = displayP3();
  ASSERT_NE(p3, nullptr);
  const auto xform = ColorSpaceXform::Make(*p3, *srgb());
  ASSERT_NE(xform, nullptr);
  std::vector<uint8_t> row = { 0, 255, 0, 255, 255, 255, 255, 128 };
  xform->applyRgba8(row.data(), 2);
  // Green in P3 is outside sRGB: the result clips to pure green rather than staying flat.
  EXPECT_EQ(row[0], 0);
  EXPECT_EQ(row[1], 255);
  EXPECT_EQ(row[3], 255);
  EXPECT_EQ(row[7], 128);
}
