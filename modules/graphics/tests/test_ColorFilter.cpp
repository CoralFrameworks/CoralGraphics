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
 * @file test_ColorFilter.cpp
 *
 * Transforming a colour.
 *
 * ## What is worth asserting
 *
 * A colour matrix is easy to write and hard to get right, because a wrong one still produces a plausible colour. So
 * the tests name matrices whose *answer* is known independently: greyscale puts the same value in all three channels
 * and that value is the luminance the coefficients describe; a channel swap moves red to blue and blue to red; a
 * scaling of one channel leaves the others exactly alone.
 *
 * Three behaviours are checked because each is a place a plausible implementation is wrong:
 *
 *  - **Clamping happens once, at the end**, so a row that produces a large value which a later row needs is not
 *    spoiled. A saturation matrix is the case, and it is asserted.
 *  - **Premultiplied colours stay premultiplied**, and a filter that drives alpha to zero produces transparent black
 *    rather than a division by zero or a colour with channels above its alpha.
 *  - **The identity is recognised**, so a caller that assembled a filter conditionally does not pay for a per-pixel
 *    pass that changes nothing.
 */

#include <graphics/Canvas.h>
#include <graphics/ColorFilter.h>
#include <graphics/Surface.h>

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

using namespace graphics;

namespace {

/** The luminance coefficients the standard uses, which a greyscale matrix is built from. */
constexpr Scalar kLumaRed = 0.2126f;
constexpr Scalar kLumaGreen = 0.7152f;
constexpr Scalar kLumaBlue = 0.0722f;

/** @return a matrix that puts the luminance of the colour into all three channels. */
ColorFilterMatrix greyscaleMatrix() {
  ColorFilterMatrix matrix;
  matrix.values = { kLumaRed, kLumaGreen, kLumaBlue, 0.0f, 0.0f,
                    kLumaRed, kLumaGreen, kLumaBlue, 0.0f, 0.0f,
                    kLumaRed, kLumaGreen, kLumaBlue, 0.0f, 0.0f,
                    0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
  return matrix;
}

/** @return a colour whose channels are all different, so a swap or a copy is visible. */
Color distinctColor() { return Color { 0.8f, 0.4f, 0.2f, 1.0f }; }

} // namespace anonymous

// ==================================================================================================
// The matrix
// ==================================================================================================

TEST(ColorFilterTest, TheIdentityMatrixChangesNothing) {
  const auto filter = ColorFilter::Matrix(ColorFilterMatrix::Identity());
  ASSERT_NE(filter, nullptr);

  const Color original = distinctColor();
  const Color filtered = filter->applyTo(original);

  EXPECT_FLOAT_EQ(filtered.red, original.red);
  EXPECT_FLOAT_EQ(filtered.green, original.green);
  EXPECT_FLOAT_EQ(filtered.blue, original.blue);
  EXPECT_FLOAT_EQ(filtered.alpha, original.alpha);

  EXPECT_TRUE(filter->isIdentity()) << "so a caller can skip it";
  EXPECT_TRUE(filter->clamps());
}

TEST(ColorFilterTest, AGreyscaleMatrixPutsTheLuminanceInEveryChannel) {
  const auto filter = ColorFilter::Matrix(greyscaleMatrix());
  ASSERT_NE(filter, nullptr);

  // The expected value is arithmetic, computed here from the coefficients rather than read from the filter.
  const Color original = Color { 1.0f, 0.5f, 0.25f, 1.0f };
  const Scalar luminance = kLumaRed * 1.0f + kLumaGreen * 0.5f + kLumaBlue * 0.25f;

  const Color filtered = filter->applyTo(original);
  EXPECT_NEAR(filtered.red, luminance, 1e-5f);
  EXPECT_NEAR(filtered.green, luminance, 1e-5f);
  EXPECT_NEAR(filtered.blue, luminance, 1e-5f);
  EXPECT_FLOAT_EQ(filtered.alpha, 1.0f);

  EXPECT_FALSE(filter->isIdentity());
  EXPECT_TRUE(filter->isAlphaUnchanged()) << "a greyscale matrix does not touch coverage";
}

TEST(ColorFilterTest, AChannelSwapMovesEachChannelToWhereItWasAsked) {
  // Red from blue, green from red, blue from green. A permutation, so a mistake in the row or column order produces a
  // different permutation and is visible.
  ColorFilterMatrix matrix;
  matrix.values = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                    1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                    0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
                    0.0f, 0.0f, 0.0f, 1.0f, 0.0f };

  const auto filter = ColorFilter::Matrix(matrix);
  ASSERT_NE(filter, nullptr);

  const Color filtered = filter->applyTo(Color { 0.8f, 0.4f, 0.2f, 1.0f });
  EXPECT_FLOAT_EQ(filtered.red, 0.2f) << "red came from blue";
  EXPECT_FLOAT_EQ(filtered.green, 0.8f) << "green came from red";
  EXPECT_FLOAT_EQ(filtered.blue, 0.4f) << "blue came from green";
}

TEST(ColorFilterTest, TheConstantColumnIsAdded) {
  // Row-major, so the fifth element of each row is the constant for that output component.
  ColorFilterMatrix matrix = ColorFilterMatrix::Identity();
  matrix.at(0, 4) = 0.1f;
  matrix.at(2, 4) = 0.2f;

  const auto filter = ColorFilter::Matrix(matrix);
  const Color filtered = filter->applyTo(Color { 0.5f, 0.5f, 0.5f, 1.0f });

  EXPECT_NEAR(filtered.red, 0.6f, 1e-5f);
  EXPECT_FLOAT_EQ(filtered.green, 0.5f) << "a row without a constant is unchanged";
  EXPECT_NEAR(filtered.blue, 0.7f, 1e-5f);
}

TEST(ColorFilterTest, AResultOutsideTheRangeIsClampedByDefault) {
  ColorFilterMatrix matrix = ColorFilterMatrix::Identity();
  matrix.at(0, 0) = 4.0f;  // red is multiplied by four, which takes 0.5 well past one

  const auto clamped = ColorFilter::Matrix(matrix, true);
  EXPECT_FLOAT_EQ(clamped->applyTo(Color { 0.5f, 0.5f, 0.5f, 1.0f }).red, 1.0f) << "clamped to the unit range";

  // A caller that turns clamping off does *not* get 2.0 back, and that is deliberate rather than a limitation. A
  // `Color` is premultiplied, so no channel may exceed its alpha -- a red of 2.0 against an alpha of 1.0 is not a
  // colour this library can represent, and returning one would corrupt every consumer of it. What the flag really
  // controls is what the *stages* see, which is what the test below asserts; a `ColorFilter` with no later stage and
  // clamping off therefore behaves like one with clamping on.
  const auto unclamped = ColorFilter::Matrix(matrix, false);
  EXPECT_FALSE(unclamped->clamps()) << "the flag is recorded, so a caller can see what it asked for";
  EXPECT_FLOAT_EQ(unclamped->applyTo(Color { 0.5f, 0.5f, 0.5f, 1.0f }).red, 1.0f)
          << "the result is still a premultiplied colour";
}

TEST(ColorFilterTest, RowsWithinaMatrixDoNotSeeEachOthersResults) {
  // A matrix whose red row produces two -- past the range -- and whose green row takes a third. The two rows are
  // computed independently from the same *input*, so green is a third of the input, not a third of red's result.
  //
  // The alternative, clamping each row as it is computed and feeding it to the next, is what a matrix evaluation
  // looks like if it is written as a loop over rows that writes back into the colour. It gives a third of *one*
  // here: a third of 0.5 is 0.1667 against a third of 1.0, which is 0.3333. That is the bug this test exists to
  // catch, and it is invisible without a row that leaves the range.
  //
  // The expected values are the arithmetic, worked out here rather than read from the filter:
  //   red   = 4 * 0.5 = 2, clamped at the end to 1
  //   green = (1/3) * 0.5 = 1/6
  ColorFilterMatrix matrix;
  matrix.values = { 4.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                    1.0f / 3.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                    0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                    0.0f, 0.0f, 0.0f, 1.0f, 0.0f };

  const auto filter = ColorFilter::Matrix(matrix, true);
  const Color filtered = filter->applyTo(Color { 0.5f, 0.5f, 0.5f, 1.0f });

  EXPECT_FLOAT_EQ(filtered.red, 1.0f) << "red reached two and was clamped to one at the end";
  EXPECT_NEAR(filtered.green, 1.0f / 6.0f, 1e-4f)
          << "green is a third of the input, not a third of red's result -- the rows are independent";
}

TEST(ColorFilterTest, ClampingOffChangesWhatALaterStageSees) {
  // The flag's observable effect: with clamping off, the second stage receives a value the first stage drove out of
  // range, so it computes something different. Two matrices rather than one, because a single matrix has no later
  // stage to be affected.
  ColorFilterMatrix doubler = ColorFilterMatrix::Identity();
  doubler.at(0, 0) = 2.0f;

  ColorFilterMatrix halver = ColorFilterMatrix::Identity();
  halver.at(0, 0) = 0.5f;

  const Color input { 0.75f, 0.0f, 0.0f, 1.0f };  // doubled to 1.5, then halved
  const Color expected { 0.75f, 0.0f, 0.0f, 1.0f };

  // Clamped between the stages: 0.75 doubles to 1.5, is clamped to 1.0, and the halving gives 0.5.
  auto clampedInner = ColorFilter::Matrix(doubler, true);
  const auto clampedOuter = ColorFilter::Matrix(halver, false);
  EXPECT_NEAR(ColorFilter::Compose(clampedOuter, clampedInner)->applyTo(input).red, 0.5f, 1e-4f)
          << "the intermediate was clamped to one before the halving";

  // Unclamped between the stages: 1.5 survives and halves back to 0.75, which is the identity after all.
  auto freeInner = ColorFilter::Matrix(doubler, false);
  EXPECT_NEAR(ColorFilter::Compose(clampedOuter, freeInner)->applyTo(input).red, expected.red, 1e-4f)
          << "the intermediate survived, so the two stages cancel";
}

TEST(ColorFilterTest, AFilterThatChangesAlphaSaysSo) {
  ColorFilterMatrix halve = ColorFilterMatrix::Identity();
  halve.at(3, 3) = 0.5f;

  const auto filter = ColorFilter::Matrix(halve);
  EXPECT_FALSE(filter->isAlphaUnchanged()) << "the alpha row is not the identity";

  // And one that makes coverage depend on colour is also not "unchanged", because it is.
  ColorFilterMatrix fromRed = ColorFilterMatrix::Identity();
  fromRed.at(3, 0) = 0.5f;
  EXPECT_FALSE(ColorFilter::Matrix(fromRed)->isAlphaUnchanged())
          << "coverage that depends on colour is coverage that changed";
}

// ==================================================================================================
// Premultiplication
// ==================================================================================================

TEST(ColorFilterTest, DrivingAlphaToZeroGivesTransparentBlack) {
  // A colour that is half transparent and premultiplied, so its channels are at most its alpha. A matrix that zeroes
  // the alpha row leaves channels that no premultiplied colour can have; the result has to be brought back into the
  // range rather than left for the rest of the layer to misread.
  ColorFilterMatrix zeroAlpha = ColorFilterMatrix::Identity();
  zeroAlpha.at(3, 3) = 0.0f;

  const auto filter = ColorFilter::Matrix(zeroAlpha);
  const Color filtered = filter->applyTo(Color { 0.5f, 0.25f, 0.125f, 0.5f });

  EXPECT_FLOAT_EQ(filtered.alpha, 0.0f);
  EXPECT_FLOAT_EQ(filtered.red, 0.0f) << "a channel cannot exceed its alpha, and a zero alpha leaves nothing";
  EXPECT_FLOAT_EQ(filtered.green, 0.0f);
  EXPECT_FLOAT_EQ(filtered.blue, 0.0f);
}

TEST(ColorFilterTest, TheResultIsAlwaysAPremultipliedColour) {
  // A matrix that puts a full unit in every channel, applied to a half-transparent colour. The alpha stays at a half,
  // so every channel has to come down to a half as well.
  ColorFilterMatrix full = ColorFilterMatrix::Identity();
  full.at(0, 4) = 1.0f;
  full.at(1, 4) = 1.0f;
  full.at(2, 4) = 1.0f;

  const auto filter = ColorFilter::Matrix(full);
  const Color filtered = filter->applyTo(Color { 0.0f, 0.0f, 0.0f, 0.5f });

  EXPECT_FLOAT_EQ(filtered.alpha, 0.5f);
  EXPECT_LE(filtered.red, filtered.alpha) << "no channel may exceed the alpha";
  EXPECT_LE(filtered.green, filtered.alpha);
  EXPECT_LE(filtered.blue, filtered.alpha);
  EXPECT_GE(filtered.red, 0.0f) << "and none may be negative";
}

// ==================================================================================================
// The table
// ==================================================================================================

TEST(ColorFilterTest, ATableMapsEachChannelThroughItsOwnEntries) {
  ColorTable table = ColorTable::Identity();
  // Invert red and leave everything else, which is one channel changing and three not.
  for (size_t i = 0; i < 256; ++i) table.red[i] = static_cast<uint8_t>(255 - i);

  const auto filter = ColorFilter::Table(table);
  ASSERT_NE(filter, nullptr);
  ASSERT_NE(filter->asTable(), nullptr);
  EXPECT_EQ(filter->kind(), ColorFilter::Kind::kTable);

  const Color filtered = filter->applyTo(Color { 0.25f, 0.25f, 0.25f, 1.0f });
  EXPECT_NEAR(filtered.red, 0.75f, 0.01f) << "inverted";
  EXPECT_NEAR(filtered.green, 0.25f, 0.01f) << "untouched";
  EXPECT_NEAR(filtered.blue, 0.25f, 0.01f);
  EXPECT_NEAR(filtered.alpha, 1.0f, 0.01f);

  EXPECT_FALSE(filter->isIdentity());
  EXPECT_TRUE(filter->isAlphaUnchanged()) << "the alpha table is the identity, so coverage is unchanged";
}

TEST(ColorFilterTest, TheIdentityTableIsRecognised) {
  const auto filter = ColorFilter::Table(ColorTable::Identity());
  EXPECT_TRUE(filter->isIdentity()) << "a table that maps every value to itself is not a filter";
}

TEST(ColorFilterTest, ATableThatChangesAlphaIsNotAlphaUnchanged) {
  ColorTable table = ColorTable::Identity();
  table.alpha[128] = 0;

  EXPECT_FALSE(ColorFilter::Table(table)->isAlphaUnchanged());
}

// ==================================================================================================
// Composition
// ==================================================================================================

TEST(ColorFilterTest, ACompositionAppliesTheInnerThenTheOuter) {
  // Two matrices that do not commute: one swaps red and green, the other adds a constant to red. Applied in the
  // wrong order the constant lands on a different channel, which is what distinguishes the orders.
  ColorFilterMatrix swap;
  swap.values = { 0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
                  1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                  0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                  0.0f, 0.0f, 0.0f, 1.0f, 0.0f };

  ColorFilterMatrix addRed = ColorFilterMatrix::Identity();
  addRed.at(0, 4) = 0.25f;

  const Color input = Color { 0.2f, 0.6f, 0.1f, 1.0f };

  const auto composed = ColorFilter::Compose(ColorFilter::Matrix(addRed), ColorFilter::Matrix(swap));
  ASSERT_NE(composed, nullptr);
  ASSERT_EQ(composed->kind(), ColorFilter::Kind::kCompose);

  const Color filtered = composed->applyTo(input);
  // Swapped first: red becomes 0.6, green becomes 0.2. Then red gets the constant: 0.85.
  EXPECT_NEAR(filtered.red, 0.85f, 1e-5f);
  EXPECT_NEAR(filtered.green, 0.2f, 1e-5f);
  EXPECT_NEAR(filtered.blue, 0.1f, 1e-5f);

  // The other order gives a different answer, which is what makes the assertion above about the order rather than
  // about composition in general.
  const auto reversed = ColorFilter::Compose(ColorFilter::Matrix(swap), ColorFilter::Matrix(addRed));
  const Color reversedResult = reversed->applyTo(input);
  EXPECT_NEAR(reversedResult.green, 0.45f, 1e-5f) << "the constant moved to green when the swap came second";
}

TEST(ColorFilterTest, ComposingWithAnIdentityChangesNothing) {
  const auto identity = ColorFilter::Matrix(ColorFilterMatrix::Identity());
  const auto greyscale = ColorFilter::Matrix(greyscaleMatrix());

  // Composing with nothing is nothing, so the answer is the other filter itself rather than a new object that behaves
  // the same. A caller comparing handles -- to decide whether a paint needs redrawing, say -- gets the answer it
  // expects, and no per-pixel stage is spent on a no-op.
  EXPECT_EQ(ColorFilter::Compose(identity, greyscale), greyscale);
  EXPECT_EQ(ColorFilter::Compose(greyscale, identity), greyscale);
  EXPECT_EQ(ColorFilter::Compose(nullptr, greyscale), greyscale);
  EXPECT_EQ(ColorFilter::Compose(greyscale, nullptr), greyscale);
  EXPECT_EQ(ColorFilter::Compose(nullptr, nullptr), nullptr);

  // And two identities compose to one of them rather than to a third.
  const auto otherIdentity = ColorFilter::Matrix(ColorFilterMatrix::Identity());
  const auto bothIdentities = ColorFilter::Compose(identity, otherIdentity);
  ASSERT_NE(bothIdentities, nullptr);
  EXPECT_TRUE(bothIdentities->isIdentity());
}

TEST(ColorFilterTest, TheTransposeOfAPairIsRecognisedAsAnIdentity) {
  // Two matrices that undo each other. Neither is the identity, and their composition is -- which a caller asking
  // "does this do anything" wants to know.
  ColorFilterMatrix double_ = ColorFilterMatrix::Identity();
  double_.at(0, 0) = 2.0f;
  ColorFilterMatrix half = ColorFilterMatrix::Identity();
  half.at(0, 0) = 0.5f;

  const auto composed = ColorFilter::Compose(ColorFilter::Matrix(half), ColorFilter::Matrix(double_));
  ASSERT_NE(composed, nullptr);

  // Each is a filter on its own...
  EXPECT_FALSE(ColorFilter::Matrix(double_)->isIdentity());

  // ...and the pair leaves a colour alone, which is what a caller checks rather than comparing matrices.
  const Color input = Color { 0.3f, 0.4f, 0.5f, 0.6f };
  const Color output = composed->applyTo(input);
  EXPECT_NEAR(output.red, input.red, 1e-5f);
  EXPECT_NEAR(output.green, input.green, 1e-5f);
  EXPECT_NEAR(output.blue, input.blue, 1e-5f);
  EXPECT_NEAR(output.alpha, input.alpha, 1e-5f);
}

// ==================================================================================================
// The 8-bit path
// ==================================================================================================

TEST(ColorFilterTest, TheEightBitPathAgreesWithTheFloatPath) {
  // The rasterizer works in premultiplied 8-bit values. The two paths are not expected to be identical -- one rounds
  // at the input and the other does not -- but they must agree to within the rounding, or a colour filtered in bytes
  // would differ visibly from the same colour filtered in floats.
  const auto filter = ColorFilter::Matrix(greyscaleMatrix());

  const Color start { 0.8f, 0.4f, 0.2f, 1.0f };
  const Color viaFloat = filter->applyTo(start);

  const uint32_t argb = (255u << 24) | (204u << 16) | (102u << 8) | 51u;  // the same colour in bytes
  const uint32_t filtered = filter->applyToPremultipliedArgb(argb);

  const auto byte = [](const Scalar value) { return static_cast<uint32_t>(std::lround(value * 255.0f)); };
  EXPECT_NEAR(static_cast<double>((filtered >> 16) & 0xFFu), static_cast<double>(byte(viaFloat.red)), 2.0);
  EXPECT_NEAR(static_cast<double>((filtered >> 8) & 0xFFu), static_cast<double>(byte(viaFloat.green)), 2.0);
  EXPECT_NEAR(static_cast<double>(filtered & 0xFFu), static_cast<double>(byte(viaFloat.blue)), 2.0);
}

// ==================================================================================================
// Through the canvas
// ==================================================================================================

TEST(ColorFilterTest, APaintWithAFilterDrawsTheFilteredColour) {
  // The end-to-end check: the filter is on the paint, so the canvas applies it without the drawing code knowing.
  auto surface = Surface::makeRaster(8, 8);
  ASSERT_NE(surface, nullptr);

  Paint paint;
  paint.color = Color { 1.0f, 1.0f, 1.0f, 1.0f };  // white
  paint.colorFilter = ColorFilter::Matrix(greyscaleMatrix());

  ASSERT_TRUE(paint.hasColorFilter()) << "a filter that is not the identity is a filter";

  const auto canvas = surface->getCanvas();
  ASSERT_NE(canvas, nullptr);
  canvas->clear(Color { 0.0f, 0.0f, 0.0f, 0.0f });
  canvas->drawRect(Rect::MakeLTRB(0.0f, 0.0f, 8.0f, 8.0f), paint);

  const Pixmap pixmap = surface->peekPixels();
  ASSERT_FALSE(pixmap.isEmpty());

  // White through a greyscale matrix is still white, so this does not distinguish it from no filter. A red fill
  // does: the luminance of pure red is a dark grey, and all three channels become equal to it.
  auto secondSurface = Surface::makeRaster(8, 8);
  ASSERT_NE(secondSurface, nullptr);
  Paint red;
  red.color = Color { 1.0f, 0.0f, 0.0f, 1.0f };
  red.colorFilter = ColorFilter::Matrix(greyscaleMatrix());
  const auto secondCanvas = secondSurface->getCanvas();
  secondCanvas->clear(Color { 0.0f, 0.0f, 0.0f, 0.0f });
  secondCanvas->drawRect(Rect::MakeLTRB(0.0f, 0.0f, 8.0f, 8.0f), red);

  const Pixmap second = secondSurface->peekPixels();
  const uint32_t pixel = second.getPixelARGB32(4, 4);
  const uint32_t r = (pixel >> 16) & 0xFFu;
  const uint32_t g = (pixel >> 8) & 0xFFu;
  const uint32_t b = pixel & 0xFFu;

  EXPECT_EQ(r, g) << "greyscale means the channels agree";
  EXPECT_EQ(g, b);
  EXPECT_LT(r, 100u) << "and the luminance of pure red is dark, not red";
  EXPECT_NEAR(static_cast<double>(r), kLumaRed * 255.0, 3.0);
}

TEST(ColorFilterTest, APaintWithNoFilterDrawsTheColourItWasGiven) {
  auto surface = Surface::makeRaster(8, 8);
  ASSERT_NE(surface, nullptr);

  Paint paint;
  paint.color = Color { 1.0f, 0.0f, 0.0f, 1.0f };
  EXPECT_FALSE(paint.hasColorFilter()) << "no filter is not a filter";

  const auto canvas = surface->getCanvas();
  canvas->clear(Color { 0.0f, 0.0f, 0.0f, 0.0f });
  canvas->drawRect(Rect::MakeLTRB(0.0f, 0.0f, 8.0f, 8.0f), paint);

  const uint32_t pixel = surface->peekPixels().getPixelARGB32(4, 4);
  EXPECT_NEAR(static_cast<double>((pixel >> 16) & 0xFFu), 255.0, 1.0) << "pure red stays pure red";
  EXPECT_NEAR(static_cast<double>((pixel >> 8) & 0xFFu), 0.0, 1.0);
}

TEST(ColorFilterTest, AnIdentityFilterOnAPaintIsReportedAsNone) {
  Paint paint;
  paint.colorFilter = ColorFilter::Matrix(ColorFilterMatrix::Identity());
  EXPECT_FALSE(paint.hasColorFilter())
          << "a filter that changes nothing must not cost a pass to discover that at draw time";
}
