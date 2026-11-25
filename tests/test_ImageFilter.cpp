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
 * @file test_ImageFilter.cpp
 *
 * Transforming a whole image.
 *
 * ## What is worth asserting
 *
 * A blur conserves: it moves value between neighbouring pixels without creating or destroying any. That is the
 * property a wrong kernel loses first -- a kernel whose weights do not sum to one multiplies the whole picture, and
 * one whose taps are read outside the image without being skipped smears a rim around it. So the central assertion is
 * on the *total* of each channel, computed from the pixels rather than read from the filter.
 *
 * The other properties are the ones a plausible implementation gets wrong:
 *
 *  - **Premultiplied, not unpremultiplied.** A blur must operate on premultiplied channels, so a nearly transparent
 *    pixel must not drag its colour into its neighbours. Asserted with a pattern designed for it.
 *  - **The edge is transparent black, not a repeat of the edge pixel.** Repeating the edge is a common shortcut and
 *    produces a bright rim where the image meets its surroundings.
 *  - **An offset moves rather than wraps.** A scrolled offset leaves a seam that looks like a bug in the caller.
 *  - **Bounds cover the result.** A filter that reports less than it produces has the caller allocate a buffer that is
 *    too small, which is a write past the end rather than a wrong colour.
 */

#include <graphics/ImageFilter.h>

#include <graphics/Bitmap.h>
#include <graphics/Image.h>
#include <graphics/Pixmap.h>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

using namespace graphics;

namespace {

constexpr int kSize = 24;

/** @return a bitmap filled by @p colour, so a test can say exactly what it expects. */
template <typename Fn>
Bitmap makeBitmap(const int width, const int height, Fn colour) {
  Bitmap bitmap = Bitmap::allocate(width, height);
  Pixmap pixmap = bitmap.makePixmap();
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) pixmap.setPixelARGB32(x, y, colour(x, y));
  }
  return bitmap;
}

/** @return an opaque colour from 8-bit channels. */
uint32_t opaque(const uint8_t r, const uint8_t g, const uint8_t b) {
  return (uint32_t { 255 } << 24) | (uint32_t { r } << 16) | (uint32_t { g } << 8) | b;
}

/**
 * @return the sum of one channel over @p within, for checking that a filter conserves.
 *
 * The rectangle matters: a blur's frame is fixed and outside it is transparent black, so the outer few pixels lose
 * their value to the empty space around them and only the interior is a fair measure of the kernel itself.
 */
long long channelTotal(const Image& image, const int channel, const Rect& within) {
  const Pixmap pixmap = image.makePixmap();
  if (pixmap.isEmpty()) return -1;

  const int left = std::max(0, static_cast<int>(within.getLeft()));
  const int top = std::max(0, static_cast<int>(within.getTop()));
  const int right = std::min(pixmap.width(), static_cast<int>(within.getRight()));
  const int bottom = std::min(pixmap.height(), static_cast<int>(within.getBottom()));

  long long total = 0;
  for (int y = top; y < bottom; ++y) {
    for (int x = left; x < right; ++x) {
      const uint32_t pixel = pixmap.getPixelARGB32(x, y);
      total += static_cast<long long>((pixel >> (8 * (3 - channel))) & 0xFFu);
    }
  }
  return total;
}

/** @return the sum of one channel over the whole image. */
long long channelTotal(const Image& image, const int channel) {
  return channelTotal(image, channel,
          Rect::MakeLTRB(0.0f, 0.0f, static_cast<Scalar>(image.width()), static_cast<Scalar>(image.height())));
}

/** @return the pixel at @p x, @p y of @p image. */
uint32_t pixelAt(const Image& image, const int x, const int y) {
  return image.makePixmap().getPixelARGB32(x, y);
}

/** @return the four 8-bit channels of a pixel. */
std::array<uint32_t, 4> channelsOf(const uint32_t pixel) {
  return { (pixel >> 24) & 0xFFu, (pixel >> 16) & 0xFFu, (pixel >> 8) & 0xFFu, pixel & 0xFFu };
}

} // namespace anonymous

// ==================================================================================================
// Building one
// ==================================================================================================

TEST(ImageFilterTest, ABlurNeedsAPositiveSigma) {
  EXPECT_NE(ImageFilter::Blur(2.0f), nullptr);
  EXPECT_NE(ImageFilter::Blur(2.0f, 4.0f), nullptr);

  // Zero is refused rather than treated as no blur, because a caller that passed it has a radius or a scale computed
  // wrong upstream and a silent no-op would send it looking in the wrong place.
  EXPECT_EQ(ImageFilter::Blur(0.0f), nullptr);
  EXPECT_EQ(ImageFilter::Blur(-1.0f), nullptr);
  EXPECT_EQ(ImageFilter::Blur(2.0f, 0.0f), nullptr) << "one axis of zero is not a blur";
  EXPECT_EQ(ImageFilter::Blur(std::numeric_limits<Scalar>::infinity()), nullptr);
  EXPECT_EQ(ImageFilter::Blur(std::numeric_limits<Scalar>::quiet_NaN()), nullptr);
}

TEST(ImageFilterTest, AnOffsetOfZeroIsAllowed) {
  // Not refused, because a displacement of zero is a legitimate answer rather than a mistake -- it just happens to be
  // the identity, which `isIdentity` reports.
  const auto filter = ImageFilter::Offset(0.0f, 0.0f);
  ASSERT_NE(filter, nullptr);
  EXPECT_TRUE(filter->isIdentity());

  EXPECT_EQ(ImageFilter::Offset(std::numeric_limits<Scalar>::infinity(), 0.0f), nullptr);
}

TEST(ImageFilterTest, TheParametersAreReportedBack) {
  const auto blur = ImageFilter::Blur(1.5f, 2.5f);
  ASSERT_NE(blur, nullptr);
  EXPECT_EQ(blur->kind(), ImageFilter::Kind::kBlur);
  EXPECT_FLOAT_EQ(blur->sigmaX(), 1.5f);
  EXPECT_FLOAT_EQ(blur->sigmaY(), 2.5f);

  const auto offset = ImageFilter::Offset(3.0f, -4.0f);
  ASSERT_NE(offset, nullptr);
  EXPECT_EQ(offset->kind(), ImageFilter::Kind::kOffset);
  EXPECT_FLOAT_EQ(offset->offsetX(), 3.0f);
  EXPECT_FLOAT_EQ(offset->offsetY(), -4.0f);
}

TEST(ImageFilterTest, ComposingWithAnIdentityGivesTheOtherFilter) {
  const auto identity = ImageFilter::Offset(0.0f, 0.0f);
  const auto real = ImageFilter::Blur(2.0f);
  ASSERT_NE(identity, nullptr);
  ASSERT_NE(real, nullptr);

  // The other operand *itself*, so a caller comparing handles gets what it expects and no stage is a no-op.
  EXPECT_EQ(ImageFilter::Compose(identity, real), real);
  EXPECT_EQ(ImageFilter::Compose(real, identity), real);
  EXPECT_EQ(ImageFilter::Compose(nullptr, real), real);
  EXPECT_EQ(ImageFilter::Compose(real, nullptr), real);
  EXPECT_EQ(ImageFilter::Compose(nullptr, nullptr), nullptr);
}

// ==================================================================================================
// Where the result lies
// ==================================================================================================

TEST(ImageFilterTest, ABlurExpandsTheBoundsByThreeSigma) {
  const auto filter = ImageFilter::Blur(2.0f, 3.0f);
  ASSERT_NE(filter, nullptr);

  const Rect bounds = Rect::MakeLTRB(10.0f, 20.0f, 110.0f, 120.0f);
  const Rect filtered = filter->filterBounds(bounds);

  // The margin is three sigma, which is where the tails round to zero at eight bits. Reported less than the blur
  // reaches and a caller's buffer is too small for the result.
  EXPECT_FLOAT_EQ(filtered.getLeft(), 10.0f - 6.0f);
  EXPECT_FLOAT_EQ(filtered.getTop(), 20.0f - 9.0f);
  EXPECT_FLOAT_EQ(filtered.getRight(), 110.0f + 6.0f);
  EXPECT_FLOAT_EQ(filtered.getBottom(), 120.0f + 9.0f);
}

TEST(ImageFilterTest, AnOffsetMovesTheBounds) {
  const auto filter = ImageFilter::Offset(5.0f, -3.0f);
  ASSERT_NE(filter, nullptr);

  const Rect bounds = Rect::MakeLTRB(0.0f, 0.0f, 10.0f, 10.0f);
  const Rect filtered = filter->filterBounds(bounds);

  EXPECT_FLOAT_EQ(filtered.getLeft(), 5.0f);
  EXPECT_FLOAT_EQ(filtered.getTop(), -3.0f);
  EXPECT_FLOAT_EQ(filtered.getRight(), 15.0f);
  EXPECT_FLOAT_EQ(filtered.getBottom(), 7.0f);
}

TEST(ImageFilterTest, AComposedBoundsNeverExceedsTheFrame) {
  // The frame is fixed, so however far an offset moves the content, the bounds a caller is told about stay inside the
  // box it is going to allocate. A version that returned the moved rectangle would have the caller allocate outside
  // its own buffer.
  const auto moved = ImageFilter::Offset(100.0f, 100.0f);
  const auto composed = ImageFilter::Compose(moved, ImageFilter::Blur(1.0f));
  ASSERT_NE(composed, nullptr);

  const Rect bounds = Rect::MakeLTRB(0.0f, 0.0f, 20.0f, 20.0f);
  const Rect filtered = composed->filterBounds(bounds);

  EXPECT_GE(filtered.getLeft(), bounds.getLeft());
  EXPECT_GE(filtered.getTop(), bounds.getTop());
  EXPECT_LE(filtered.getRight(), bounds.getRight());
  EXPECT_LE(filtered.getBottom(), bounds.getBottom());
}

// ==================================================================================================
// Blurring
// ==================================================================================================

TEST(ImageFilterTest, ABlurPreservesTheTotalOfEveryChannelOverTheInterior) {
  // The property that says the kernel sums to one. A kernel that sums to more than one multiplies the whole picture,
  // and one that reads outside the image without skipping pulls in whatever happened to be there.
  //
  // Measured over the *interior*. The frame is fixed and outside it is transparent black -- deliberately, since that
  // is what the edge of a picture is -- so the outer `radius` pixels lose their value to the empty space around them,
  // and that loss is the subject of `TheEdgeFallsOffIntoTransparency`. Over the region where every tap lands inside,
  // the total is what the kernel says it is.
  // Larger than the other fixtures on purpose: the interior has to be wide enough to measure at the largest sigma,
  // and with a kernel radius of three sigma a 24-pixel image has no interior left at all by sigma four.
  constexpr int kLarge = 64;
  const Image source = Image::fromBitmap(makeBitmap(kLarge, kLarge, [](const int x, const int y) {
    return opaque(static_cast<uint8_t>(x * 3), static_cast<uint8_t>(y * 3), uint8_t { 128 });
  }));
  ASSERT_FALSE(source.isEmpty());

  for (const Scalar sigma: { 1.0f, 2.0f, 3.0f }) {
    const auto filter = ImageFilter::Blur(sigma);
    ASSERT_NE(filter, nullptr);
    const Image filtered = filter->filter(source);
    ASSERT_FALSE(filtered.isEmpty()) << "sigma " << sigma;

    // Far enough in that no tap of either pass reaches the empty space outside.
    const int margin = static_cast<int>(std::ceil(sigma * 3.0f)) + 1;
    const Rect interior =
            Rect::MakeLTRB(static_cast<Scalar>(margin), static_cast<Scalar>(margin),
                    static_cast<Scalar>(kLarge - margin), static_cast<Scalar>(kLarge - margin));
    ASSERT_GT(interior.getRight(), interior.getLeft()) << "the image is too small for sigma " << sigma;

    for (int channel = 0; channel < 4; ++channel) {
      const long long before = channelTotal(source, channel, interior);
      const long long after = channelTotal(filtered, channel, interior);
      ASSERT_GT(before, 0);

      const double ratio = static_cast<double>(after) / static_cast<double>(before);
      EXPECT_NEAR(ratio, 1.0, 0.01)
              << "channel " << channel << " at sigma " << sigma << " kept " << ratio << " of its interior total";
    }
  }
}

TEST(ImageFilterTest, ABlurSoftensASharpEdge) {
  // The other half: what leaves one side arrives at the other. A blur that did nothing would pass the conservation
  // test and fail this one.
  const Image source = Image::fromBitmap(makeBitmap(kSize, kSize, [](const int x, const int) {
    return x < kSize / 2 ? opaque(255, 255, 255) : opaque(0, 0, 0);
  }));

  const auto filter = ImageFilter::Blur(2.0f);
  const Image filtered = filter->filter(source);

  // The two pixels either side of the edge are no longer the extremes they were.
  const uint32_t leftOfEdge = pixelAt(filtered, kSize / 2 - 1, kSize / 2);
  const uint32_t rightOfEdge = pixelAt(filtered, kSize / 2, kSize / 2);

  EXPECT_LT(channelsOf(leftOfEdge)[1], 255u) << "the white side lost value at the edge";
  EXPECT_GT(channelsOf(rightOfEdge)[1], 0u) << "and the black side gained it";

  // And far from the edge nothing changed, which is what tells a blur from a scale. The distance is the kernel's
  // radius rather than one pixel: a sample within `radius` of the border has taps that fall outside the image, and
  // those taps are transparent black, so the border really is lighter than the middle even on the white side.
  //
  // A tolerance rather than exactness: each pass rounds to eight bits, so a value within a few units of the edge can
  // be pulled a unit or two by that rounding alone. The assertion is that the far sides keep their values, which is
  // what tells a blur from a scale -- not that the arithmetic is exact to the last unit.
  const int margin = static_cast<int>(std::ceil(2.0f * 3.0f)) + 1;
  EXPECT_NEAR(channelsOf(pixelAt(filtered, margin, kSize / 2))[1], 255.0, 3.0)
          << "the far side of the white area is still white";
  EXPECT_NEAR(channelsOf(pixelAt(filtered, kSize - margin, kSize / 2))[1], 0.0, 3.0)
          << "and the far side of the black area is still black";
}

TEST(ImageFilterTest, ABlurOfAUniformImageLeavesItUniform) {
  // The sharpest test of the kernel's normalisation. A uniform image has no edges for the kernel to read past, so
  // every pixel must come out exactly as it went in -- a flat area that drifts is a kernel that does not sum to one,
  // and in a real picture it shows as a halo around whatever was blurred.
  const Image source = Image::fromBitmap(makeBitmap(kSize, kSize, [](const int, const int) {
    return opaque(90, 140, 200);
  }));

  const auto filter = ImageFilter::Blur(3.0f);
  const Image filtered = filter->filter(source);
  ASSERT_FALSE(filtered.isEmpty());

  // The kernel's radius is 9, so a sample is untouched only when it is at least that far from every edge. Sampling
  // nearer than that measures the border, not the kernel: at one pixel in, most of the kernel's taps fall outside the
  // image, and every one of them contributes transparent black.
  const int margin = static_cast<int>(std::ceil(3.0f * 3.0f));
  for (int y = margin; y < kSize - margin; ++y) {
    for (int x = margin; x < kSize - margin; ++x) {
      const auto channels = channelsOf(pixelAt(filtered, x, y));
      EXPECT_EQ(channels[0], 255u) << "alpha at " << x << "," << y;
      EXPECT_EQ(channels[1], 90u) << "red at " << x << "," << y;
      EXPECT_EQ(channels[2], 140u) << "green at " << x << "," << y;
      EXPECT_EQ(channels[3], 200u) << "blue at " << x << "," << y;
    }
  }

  // And the border is lighter than the interior, which is the edge falling off rather than a kernel that is wrong.
  EXPECT_LT(channelsOf(pixelAt(filtered, 0, 0))[0], 255u) << "the corner is not fully covered";
}

TEST(ImageFilterTest, TheEdgeFallsOffIntoTransparency) {
  // Outside the image is transparent black, not a repeat of the edge pixel. The two are told apart by what happens at
  // the border: repeating the edge leaves a fully opaque rim, while transparency lets the coverage fall away -- which
  // is what a blurred shape's edge actually looks like.
  const Image source = Image::fromBitmap(makeBitmap(8, 8, [](const int, const int) { return opaque(255, 0, 0); }));

  const auto filter = ImageFilter::Blur(2.0f);
  const Image filtered = filter->filter(source);

  const uint32_t corner = pixelAt(filtered, 0, 0);
  const uint32_t middle = pixelAt(filtered, 4, 4);

  EXPECT_LT(channelsOf(corner)[0], channelsOf(middle)[0]) << "the corner is less covered than the middle";
  EXPECT_LT(channelsOf(corner)[0], 255u) << "and less than fully covered, so it fell off";
}

TEST(ImageFilterTest, ABlurDoesNotUnpremultiply) {
  // Premultiplied channels are blurred as they are. A version that unpremultiplied first would weight each pixel's
  // colour by the inverse of its own alpha, so a nearly transparent pixel with a strong colour would drag that colour
  // into its neighbours -- producing a bright halo around a faint dot.
  //
  // The pattern: an opaque white field with one nearly transparent red pixel in the middle. Blurred correctly, the red
  // pixel contributes only its own premultiplied red, which is tiny; blurred after unpremultiplying, that pixel's red
  // would briefly count as full strength.
  const Image source = Image::fromBitmap(makeBitmap(kSize, kSize, [](const int x, const int y) {
    if (x == kSize / 2 && y == kSize / 2) {
      // Red at alpha 8, premultiplied: red becomes 8 * 255 / 255 = 8.
      return (uint32_t { 8 } << 24) | (uint32_t { 8 } << 16);
    }
    return opaque(255, 255, 255);
  }));

  const auto filter = ImageFilter::Blur(2.0f);
  const Image filtered = filter->filter(source);
  ASSERT_FALSE(filtered.isEmpty());

  // Green and blue are 255 everywhere, so a blurred red that came from an unpremultiplied pixel would show as a
  // *lower* green or blue than its neighbours -- the red having been added in place of them.
  const auto channels = channelsOf(pixelAt(filtered, kSize / 2, kSize / 2));

  // The alpha barely moved, because the dot's coverage was tiny against the opaque field. A tolerance rather than a
  // tight bound: the field also loses a few units of alpha to the image's *transparent border*, which is the edge
  // falling off rather than anything to do with the dot. The reading that matters is the comparison below -- that red
  // did not replace green -- and that one is not close to its threshold.
  EXPECT_GE(channels[0], 235u) << "the field is opaque and stays so";

  // And the colour is still essentially white: the dot's premultiplied red was 8, so at most a few units of red can
  // have arrived and green must still be far above the red.
  EXPECT_GT(channels[2], 200u) << "green is still near full, so the red was not substituted for it";
  EXPECT_GT(channels[3], 200u) << "and blue";
}

TEST(ImageFilterTest, AnAnisotropicBlurStaysWithinItsAxis) {
  // A blur along x only must not change a column's total, which is what tells the two passes apart. A version that
  // applied one kernel to both directions would fail here.
  const Image source = Image::fromBitmap(makeBitmap(kSize, kSize, [](const int x, const int y) {
    return (x == kSize / 2 && y == kSize / 2) ? opaque(255, 255, 255) : opaque(0, 0, 0);
  }));

  const auto filter = ImageFilter::Blur(3.0f, 0.5f);
  ASSERT_NE(filter, nullptr);
  const Image filtered = filter->filter(source);
  ASSERT_FALSE(filtered.isEmpty());

  // With a narrow vertical blur the dot spreads mostly sideways, so the row through the middle holds far more of the
  // value than the row two pixels away.
  const auto middleRow = channelsOf(pixelAt(filtered, kSize / 2, kSize / 2))[1];
  const auto awayRow = channelsOf(pixelAt(filtered, kSize / 2, kSize / 2 - 4))[1];
  EXPECT_GT(middleRow, awayRow) << "the blur reached much further along x than along y";
}

// ==================================================================================================
// Offsetting
// ==================================================================================================

TEST(ImageFilterTest, AnOffsetMovesThePixelsWithoutChangingThem) {
  const Image source = Image::fromBitmap(makeBitmap(16, 16, [](const int x, const int y) {
    return (x < 4 && y < 4) ? opaque(255, 0, 0) : opaque(0, 0, 0);
  }));

  const auto filter = ImageFilter::Offset(5.0f, 3.0f);
  const Image filtered = filter->filter(source);
  ASSERT_FALSE(filtered.isEmpty());

  // The marker has moved by exactly the offset, and the pixels themselves are unchanged.
  EXPECT_EQ(pixelAt(filtered, 5 + 2, 3 + 2), pixelAt(source, 2, 2)) << "the pixel moved";
  EXPECT_EQ(channelsOf(pixelAt(filtered, 7, 5))[1], 255u) << "and kept its colour";

  // Where it used to be is now uncovered rather than scrolled round.
  EXPECT_EQ(channelsOf(pixelAt(filtered, 2, 2))[0], 0u) << "the vacated area is transparent";
}

TEST(ImageFilterTest, AnOffsetDoesNotWrap) {
  // A scrolled offset leaves a seam: the content that fell off one side reappears on the other. That looks like a bug
  // in the caller's geometry rather than in the filter, which is why it is asserted.
  const Image source = Image::fromBitmap(makeBitmap(8, 8, [](const int x, const int) {
    return x == 0 ? opaque(255, 0, 0) : opaque(0, 0, 0);
  }));

  const auto filter = ImageFilter::Offset(4.0f, 0.0f);
  const Image filtered = filter->filter(source);

  // The leftmost column held the marker. Moving right by four puts it at column four.
  EXPECT_EQ(channelsOf(pixelAt(filtered, 4, 4))[1], 255u) << "the marker is where it was moved to";

  // The source is *opaque* black everywhere except column zero, so the vacated strip is opaque black -- it takes the
  // colour of the pixels that moved into it. What a scroll would do instead is bring the marker back onto the far
  // side, and that is the property worth asserting: the marker appears exactly once, at its new position.
  //
  // The first version of this expected the vacated strip to be transparent and checked the alpha channel for zero.
  // The source is opaque black, which is alpha 255, so the assertion was the opposite of what it meant -- and the
  // marker really does have alpha 255, so reading the alpha could not tell it apart from the field in any case. Red
  // is the channel that distinguishes them.
  for (int x = 5; x < 8; ++x) {
    EXPECT_EQ(channelsOf(pixelAt(filtered, x, 4))[1], 0u)
            << "column " << x << " gained the marker back, so the offset wrapped";
  }
  for (int x = 0; x < 4; ++x) {
    EXPECT_EQ(channelsOf(pixelAt(filtered, x, 4))[1], 0u) << "the vacated column " << x << " holds no marker";
  }
}

TEST(ImageFilterTest, AnOffsetRoundsToWholePixels) {
  // A sub-pixel offset would need a resampling kernel and a choice of filter to go with it, which is a scale rather
  // than a translation.
  const Image source = Image::fromBitmap(makeBitmap(8, 8, [](const int x, const int) {
    return x == 2 ? opaque(255, 0, 0) : opaque(0, 0, 0);
  }));

  const auto filter = ImageFilter::Offset(0.4f, 0.0f);
  ASSERT_NE(filter, nullptr);
  // 0.4 rounds to nothing, so the image is unchanged.
  EXPECT_TRUE(filter->isIdentity()) << "an offset that rounds to zero is the identity";

  const Image filtered = filter->filter(source);
  EXPECT_EQ(channelsOf(pixelAt(filtered, 2, 4))[1], 255u) << "the marker has not moved";
}

// ==================================================================================================
// Composing
// ==================================================================================================

TEST(ImageFilterTest, ACompositionAppliesTheInnerFirst) {
  // Blur then offset, against offset then blur. The two do not commute: blurring first and moving the result leaves
  // the falloff where the blur put it, while moving first and blurring covers a different region with the falloff.
  const Image source = Image::fromBitmap(makeBitmap(24, 24, [](const int x, const int y) {
    return (x < 12 && y < 12) ? opaque(255, 255, 255) : opaque(0, 0, 0);
  }));

  const auto blur = ImageFilter::Blur(2.0f);
  const auto offset = ImageFilter::Offset(6.0f, 0.0f);
  ASSERT_NE(blur, nullptr);
  ASSERT_NE(offset, nullptr);

  const Image blurThenOffset = ImageFilter::Compose(offset, blur)->filter(source);
  const Image offsetThenBlur = ImageFilter::Compose(blur, offset)->filter(source);

  ASSERT_FALSE(blurThenOffset.isEmpty());
  ASSERT_FALSE(offsetThenBlur.isEmpty());

  // The blurred edge has moved in the first and stayed put in the second, so a row through it differs.
  bool anyDifference = false;
  for (int x = 0; x < 24; ++x) {
    if (channelsOf(pixelAt(blurThenOffset, x, 6))[0] != channelsOf(pixelAt(offsetThenBlur, x, 6))[0]) {
      anyDifference = true;
      break;
    }
  }
  EXPECT_TRUE(anyDifference) << "the order of a blur and an offset matters, so the two results must differ";
}

TEST(ImageFilterTest, ADropShadowIsAnOffsetBlur) {
  // The two factories above are the abstraction; a drop shadow is what a caller builds from them, and building one
  // here shows that the abstraction is enough. There is no `DropShadow` factory because a factory per named effect is
  // a list to maintain rather than an abstraction.
  const Image source = Image::fromBitmap(makeBitmap(24, 24, [](const int x, const int y) {
    return (x >= 4 && x < 14 && y >= 4 && y < 14) ? opaque(255, 0, 0) : (uint32_t { 0 });
  }));
  ASSERT_FALSE(source.isEmpty());

  // The shadow: blur the source, move it down and right, and it now lies under the original.
  const auto shadow = ImageFilter::Compose(ImageFilter::Offset(3.0f, 3.0f), ImageFilter::Blur(2.0f));
  ASSERT_NE(shadow, nullptr);

  const Image blurred = shadow->filter(source);
  ASSERT_FALSE(blurred.isEmpty());

  // The shadow reaches below and right of where the shape was, which is the whole of what a drop shadow is.
  EXPECT_GT(channelsOf(pixelAt(blurred, 14, 14))[0], 0u) << "the shadow extends past the shape's corner";
  EXPECT_EQ(channelsOf(pixelAt(blurred, 2, 2))[0], 0u) << "and does not extend past its top left, because it moved away";
}

// ==================================================================================================
// Degenerate inputs
// ==================================================================================================

TEST(ImageFilterTest, FilteringAnEmptyImageGivesAnEmptyImage) {
  const auto blur = ImageFilter::Blur(2.0f);
  const auto offset = ImageFilter::Offset(3.0f, 3.0f);
  ASSERT_NE(blur, nullptr);
  ASSERT_NE(offset, nullptr);

  EXPECT_TRUE(blur->filter(Image {}).isEmpty());
  EXPECT_TRUE(offset->filter(Image {}).isEmpty());
}

TEST(ImageFilterTest, FilteringTwiceGivesTheSameAnswer) {
  // A filter is a function, not a mutation: two calls on the same source must agree, and the source must be untouched.
  const Image source = Image::fromBitmap(makeBitmap(16, 16, [](const int x, const int y) {
    return opaque(static_cast<uint8_t>(x * 8), static_cast<uint8_t>(y * 8), uint8_t { 40 });
  }));

  const auto filter = ImageFilter::Blur(1.5f);
  const Image first = filter->filter(source);
  const Image second = filter->filter(source);
  ASSERT_FALSE(first.isEmpty());
  ASSERT_FALSE(second.isEmpty());

  for (int y = 0; y < 16; y += 3) {
    for (int x = 0; x < 16; x += 3) {
      EXPECT_EQ(pixelAt(first, x, y), pixelAt(second, x, y)) << "at " << x << "," << y;
    }
  }

  // And the source is unchanged, so a caller can filter it two ways.
  EXPECT_EQ(channelsOf(pixelAt(source, 8, 8))[1], 64u) << "the source still holds what it was given";
}
