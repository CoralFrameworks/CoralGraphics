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
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF
 * ANY KIND, either express or implied. See the License for the specific language governing permissions
 * and limitations under the License.
 */

/**
 * @file test_Pixmap.cpp
 *
 * Pixel access and geometry validation.
 *
 * The cases worth testing are the ones where a plausible implementation is wrong rather than missing: a
 * row stride assumed to be `width * bytesPerPixel`, a red and blue channel swapped, a premultiplied pixel
 * read as if it were not, and a buffer that is too small for the geometry it is asked to describe. Each of
 * those produces a picture that looks almost right, which is why they are pinned here rather than left to
 * a visual check.
 */

#include <graphics/Pixmap.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace graphics;

namespace {

/** ARGB32 as this layer spells it: alpha in the top byte, then red, green, blue. */
constexpr uint32_t argb(const uint32_t a, const uint32_t r, const uint32_t g, const uint32_t b) {
  return (a << 24) | (r << 16) | (g << 8) | b;
}

/** A tightly packed image of the given format. */
std::vector<uint8_t> makeBuffer(const ImageInfo& info) { return std::vector<uint8_t>(info.computeByteSize()); }

} // namespace anonymous

TEST(PixmapTest, ReadsAndWritesThroughAView) {
  const ImageInfo info(4, 3, ColorType::kRGBA_8888, AlphaType::kUnpremultiplied);
  std::vector<uint8_t> buffer = makeBuffer(info);
  Pixmap pixmap(info, buffer.data());

  EXPECT_TRUE(pixmap.isValid());
  EXPECT_EQ(pixmap.width(), 4);
  EXPECT_EQ(pixmap.height(), 3);
  EXPECT_EQ(pixmap.rowBytes(), 16u) << "four pixels of four bytes";

  pixmap.setPixelARGB32(0, 0, argb(255, 10, 20, 30));
  EXPECT_EQ(pixmap.getPixelARGB32(0, 0), argb(255, 10, 20, 30));

  // The bytes landed where the format says they should, which is the check that catches a layout mismatch
  // between the accessor and the storage.
  EXPECT_EQ(buffer[0], 10) << "red first in RGBA";
  EXPECT_EQ(buffer[1], 20);
  EXPECT_EQ(buffer[2], 30);
  EXPECT_EQ(buffer[3], 255);

  // A view is a view: writing through a copy is visible through the original.
  Pixmap alias = pixmap;
  alias.setPixelARGB32(1, 0, argb(255, 1, 2, 3));
  EXPECT_EQ(pixmap.getPixelARGB32(1, 0), argb(255, 1, 2, 3));
}

TEST(PixmapTest, SwapsRedAndBlueForBGRA) {
  const ImageInfo info(2, 1, ColorType::kBGRA_8888, AlphaType::kUnpremultiplied);
  std::vector<uint8_t> buffer = makeBuffer(info);
  Pixmap pixmap(info, buffer.data());

  pixmap.setPixelARGB32(0, 0, argb(255, 200, 100, 50));

  // The accessor reports RGBA whatever the storage order, and the storage is B first.
  EXPECT_EQ(buffer[0], 50) << "blue is stored first in BGRA";
  EXPECT_EQ(buffer[1], 100);
  EXPECT_EQ(buffer[2], 200) << "red is stored third";
  EXPECT_EQ(buffer[3], 255);

  // Reading it back gives the colour that was written, not a channel-swapped one. An implementation that
  // relabelled the bytes instead of converting them would fail here and nowhere else.
  EXPECT_EQ(pixmap.getPixelARGB32(0, 0), argb(255, 200, 100, 50));
}

TEST(PixmapTest, PremultipliesOnWriteAndUnpremultipliesOnRead) {
  const ImageInfo info(2, 1, ColorType::kRGBA_8888, AlphaType::kPremultiplied);
  std::vector<uint8_t> buffer = makeBuffer(info);
  Pixmap pixmap(info, buffer.data());

  // Half opacity red: the stored value is about half of 255 in every colour channel, so a device that
  // ignores alpha cannot accidentally draw full-strength colour.
  pixmap.setPixelARGB32(0, 0, argb(128, 255, 0, 0));
  EXPECT_NEAR(buffer[0], 128, 2) << "red is premultiplied by a half";
  EXPECT_EQ(buffer[1], 0);
  EXPECT_EQ(buffer[2], 0);
  EXPECT_EQ(buffer[3], 128) << "alpha itself is not scaled";

  // And reading it back recovers the original colour, which is the round trip a caller relies on.
  EXPECT_EQ(pixmap.getPixelARGB32(0, 0), argb(128, 255, 0, 0));
}

TEST(PixmapTest, PremultiplyRoundTripsExactlyAtEveryAlpha) {
  // The regression test for a premultiply that was wrong by one part in 259.
  //
  // That bug was invisible at the values a test naturally reaches for: a red of 10 came back as 10, so the round
  // trip looked exact and the defect survived a full round of tests. It only showed at 30, where it read as 30 and
  // looked like a blit that multiplied by three. Sweeping every value at every alpha is what makes an error
  // visible wherever it is largest rather than only where someone happened to look.
  const ImageInfo info(1, 1, ColorType::kRGBA_8888, AlphaType::kPremultiplied);

  for (uint32_t alpha = 0; alpha <= 255; alpha += 5) {
    std::vector<uint8_t> buffer = makeBuffer(info);
    Pixmap pixmap(info, buffer.data());

    for (uint32_t value = 0; value <= 255; value += 5) {
      pixmap.setPixelARGB32(0, 0, argb(alpha, value, 0, 0));

      EXPECT_EQ(buffer[3], alpha) << "alpha is stored unchanged; value " << value << " alpha " << alpha;

      // The assertion that actually finds the bug is on the *stored* byte, because that is where the quantisation
      // happens. `value * alpha / 255` is the exact answer and a correct implementation is within one of it.
      //
      // Testing the round-tripped colour instead looks more natural and is far weaker: un-premultiplying divides
      // by the alpha, so a one-unit storage error becomes `255 / alpha` in the output. At an alpha of 5 that is a
      // swing of 51, which swamps the bug being hunted -- the doubled scaling produced an error of one part in
      // 259, invisible next to the quantisation it was hiding behind.
      const uint32_t expected = (value * alpha + 127u) / 255u;
      const uint32_t stored = buffer[0];
      EXPECT_LE(stored > expected ? stored - expected : expected - stored, 1u)
              << "stored " << stored << " should be within one of " << expected << "; value " << value
              << " alpha " << alpha;

      // The direction, which quantisation preserves and a scaling error does not: stored premultiplied colour can
      // never exceed the colour it came from, or a device ignoring alpha would draw too bright.
      EXPECT_LE(stored, value) << "premultiplied colour must not exceed the source; alpha " << alpha;
    }
  }
}

TEST(PixmapTest, AnOpaquePixelIsUnchangedByThePremultiply) {
  // The sharpest single case, and the one the doubled scaling broke most visibly: at full alpha the premultiply
  // is the identity, so a value that changes is wrong no matter how slightly. The earlier bug turned opaque white
  // into 257, which truncated to 1 -- a white pixel stored almost black.
  const ImageInfo info(1, 1, ColorType::kRGBA_8888, AlphaType::kPremultiplied);

  for (uint32_t value = 0; value <= 255; ++value) {
    std::vector<uint8_t> buffer = makeBuffer(info);
    Pixmap pixmap(info, buffer.data());
    pixmap.setPixelARGB32(0, 0, argb(255, value, value, value));

    EXPECT_EQ(buffer[0], value) << "opaque premultiply must be the identity at " << value;
    EXPECT_EQ((pixmap.getPixelARGB32(0, 0) >> 16) & 0xFFu, value)
            << "and the round trip must return the value at " << value;
  }
}

TEST(PixmapTest, PremultipliedStorageIsDarkerNotBrighter) {
  // The direction matters and the magnitude is bounded. Storing half-opacity white must give about half of 255 in
  // the colour channel, so a device that ignores alpha cannot draw full-strength colour by accident.
  const ImageInfo info(1, 1, ColorType::kRGBA_8888, AlphaType::kPremultiplied);
  std::vector<uint8_t> buffer = makeBuffer(info);
  Pixmap pixmap(info, buffer.data());

  pixmap.setPixelARGB32(0, 0, argb(128, 255, 255, 255));
  EXPECT_NEAR(static_cast<int>(buffer[0]), 128, 2) << "half alpha halves the stored colour";
  EXPECT_EQ(buffer[3], 128u) << "and the alpha itself is stored unchanged";

  // Opaque white stores as white. This is the case the double-scaled premultiply got wrong: it produced 257,
  // which truncated to 1 in the byte, so a white pixel came out almost black.
  pixmap.setPixelARGB32(0, 0, argb(255, 255, 255, 255));
  EXPECT_EQ(buffer[0], 255u) << "opaque white must store as white";
  EXPECT_EQ(buffer[1], 255u);
  EXPECT_EQ(buffer[2], 255u);
}

TEST(PixmapTest, AFullyTransparentPremultipliedPixelReadsAsNothing) {
  const ImageInfo info(2, 1, ColorType::kRGBA_8888, AlphaType::kPremultiplied);
  std::vector<uint8_t> buffer = makeBuffer(info);
  Pixmap pixmap(info, buffer.data());

  pixmap.setPixelARGB32(0, 0, argb(0, 255, 255, 255));

  // Zero alpha with non-zero colour. Unpremultiplying would divide by zero, so the answer is defined
  // directly: a premultiplied pixel with no alpha carries no colour.
  EXPECT_EQ(pixmap.getPixelARGB32(0, 0), 0u);
}

TEST(PixmapTest, HonoursAnExplicitRowStride) {
  // Two pixels per row, but each row padded out to 16 bytes. An implementation that assumed the stride
  // from the width would read the padding as pixel data and produce a skewed image -- the failure this
  // type exists to prevent.
  const ImageInfo info(2, 2, ColorType::kRGBA_8888, AlphaType::kUnpremultiplied);
  constexpr size_t kStride = 16;
  std::vector<uint8_t> buffer(kStride * 2, 0);
  Pixmap pixmap(info, buffer.data(), kStride);

  EXPECT_TRUE(pixmap.isValid());
  EXPECT_EQ(pixmap.rowBytes(), kStride);

  pixmap.setPixelARGB32(0, 0, argb(255, 1, 1, 1));
  pixmap.setPixelARGB32(0, 1, argb(255, 2, 2, 2));

  EXPECT_EQ(buffer[0], 1) << "row 0 starts at byte 0";
  EXPECT_EQ(buffer[kStride], 2) << "row 1 starts at the stride, not at two pixels in";

  // The padding between them is untouched, so nothing wrote outside its own row.
  for (size_t i = 8; i < kStride; ++i) EXPECT_EQ(buffer[i], 0) << "padding byte " << i;
}

TEST(PixmapTest, RejectsAStrideNarrowerThanARow) {
  const ImageInfo info(4, 2, ColorType::kRGBA_8888, AlphaType::kUnpremultiplied);
  std::vector<uint8_t> buffer = makeBuffer(info);

  // 8 bytes is two pixels' worth, but a row needs 16. Treating this as valid would make every access to
  // the second half of a row read the next row's pixels.
  const Pixmap pixmap(info, buffer.data(), 8);
  EXPECT_FALSE(pixmap.isValid());
  EXPECT_EQ(pixmap.addr(0, 0), nullptr) << "an invalid pixmap addresses nothing";
}

TEST(PixmapTest, RejectsAnUnsupportedFormat) {
  const ImageInfo info(4, 4, ColorType::kUnknown, AlphaType::kUnpremultiplied);
  std::vector<uint8_t> buffer(64);
  const Pixmap pixmap(info, buffer.data());
  EXPECT_FALSE(pixmap.isValid()) << "a format with no known pixel size cannot be addressed";
}

TEST(PixmapTest, RefusesABufferTooSmallForItsGeometry) {
  const ImageInfo info(4, 4, ColorType::kRGBA_8888, AlphaType::kUnpremultiplied);  // needs 64 bytes
  const Data tooSmall = Data::makeUninitialised(16);

  // The `Data` constructor checks, because it can: claiming a height the buffer cannot hold would make
  // every later bounds check agree with a lie.
  const Pixmap pixmap(info, tooSmall);
  EXPECT_FALSE(pixmap.isValid());
  EXPECT_EQ(pixmap.pixels(), nullptr);

  // Exactly enough is enough.
  const Data exact = Data::makeUninitialised(64);
  const Pixmap ok(info, exact);
  EXPECT_TRUE(ok.isValid());
}

TEST(PixmapTest, KeepsItsBufferAlive) {
  const ImageInfo info(2, 2, ColorType::kRGBA_8888, AlphaType::kUnpremultiplied);

  std::weak_ptr<int> observer;
  Pixmap pixmap;
  {
    auto owner = std::make_shared<int>(1);
    observer = owner;
    const Data data = Data::fromBorrowed(Data::makeUninitialised(16).bytes(), 16,
            std::static_pointer_cast<const void>(owner));
    pixmap = Pixmap(info, data);
  }

  EXPECT_FALSE(observer.expired()) << "the pixmap holds a reference to the bytes it views";
  pixmap = Pixmap {};
  EXPECT_TRUE(observer.expired());
}

TEST(PixmapTest, OutOfRangeAccessIsTransparentRatherThanUndefined) {
  const ImageInfo info(2, 2, ColorType::kRGBA_8888, AlphaType::kUnpremultiplied);
  std::vector<uint8_t> buffer = makeBuffer(info);
  Pixmap pixmap(info, buffer.data());
  pixmap.setPixelARGB32(1, 1, argb(255, 9, 9, 9));

  EXPECT_EQ(pixmap.addr(-1, 0), nullptr);
  EXPECT_EQ(pixmap.addr(0, -1), nullptr);
  EXPECT_EQ(pixmap.addr(2, 0), nullptr);
  EXPECT_EQ(pixmap.addr(0, 2), nullptr);
  EXPECT_EQ(pixmap.getPixelARGB32(5, 5), 0u) << "reading outside reads transparent";

  // A write outside is dropped, and must not have landed anywhere in the buffer.
  pixmap.setPixelARGB32(5, 5, argb(255, 255, 0, 0));
  EXPECT_EQ(pixmap.getPixelARGB32(1, 1), argb(255, 9, 9, 9));
  for (size_t i = 0; i < buffer.size(); ++i) {
    if (i >= 12) break;  // the pixel we set deliberately
    EXPECT_EQ(buffer[i], 0) << "byte " << i << " should be untouched";
  }
}

TEST(PixmapTest, SubsetIsClippedToTheParent) {
  const ImageInfo info(4, 4, ColorType::kRGBA_8888, AlphaType::kUnpremultiplied);
  std::vector<uint8_t> buffer = makeBuffer(info);
  Pixmap pixmap(info, buffer.data());

  // Fill so the subset's contents are identifiable by position.
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      pixmap.setPixelARGB32(x, y, argb(255, static_cast<uint32_t>(x), static_cast<uint32_t>(y), 0));
    }
  }

  Pixmap corner = Pixmap::makeSubset(pixmap, Rect::MakeLTRB(2.0f, 1.0f, 4.0f, 3.0f));
  ASSERT_TRUE(corner.isValid());
  EXPECT_EQ(corner.width(), 2);
  EXPECT_EQ(corner.height(), 2);

  // The subset's (0,0) is the parent's (2,1) -- which is the whole point of an offset view.
  EXPECT_EQ(corner.getPixelARGB32(0, 0), argb(255, 2, 1, 0));
  EXPECT_EQ(corner.getPixelARGB32(1, 1), argb(255, 3, 2, 0));

  // It still shares the parent's storage: writing through the subset is visible through the parent.
  corner.setPixelARGB32(0, 0, argb(255, 99, 99, 99));
  EXPECT_EQ(pixmap.getPixelARGB32(2, 1), argb(255, 99, 99, 99));

  // A request running off the edge yields the part that exists rather than reading outside the buffer.
  const Pixmap clipped = Pixmap::makeSubset(pixmap, Rect::MakeLTRB(3.0f, 3.0f, 100.0f, 100.0f));
  ASSERT_TRUE(clipped.isValid());
  EXPECT_EQ(clipped.width(), 1) << "clipped to the parent's width";
  EXPECT_EQ(clipped.height(), 1);

  // A request entirely outside has nothing to give.
  EXPECT_TRUE(Pixmap::makeSubset(pixmap, Rect::MakeLTRB(10.0f, 10.0f, 20.0f, 20.0f)).isEmpty());
}

TEST(PixmapTest, GrayscaleConvertsWithLuminanceWeights) {
  const ImageInfo info(2, 1, ColorType::kGray_8, AlphaType::kOpaque);
  std::vector<uint8_t> buffer = makeBuffer(info);
  Pixmap pixmap(info, buffer.data());

  // Pure green is far brighter than pure blue to the eye, so a weighted conversion gives very different
  // values. An implementation that averaged the channels would give the same value for both, which is the
  // mistake this pins.
  pixmap.setPixelARGB32(0, 0, argb(255, 0, 255, 0));
  pixmap.setPixelARGB32(1, 0, argb(255, 0, 0, 255));

  EXPECT_GT(buffer[0], 140) << "green is bright";
  EXPECT_LT(buffer[1], 40) << "blue is dark";
  EXPECT_GT(buffer[0], buffer[1]) << "and the two must differ, which an average would not";

  // Grey has no colour to report, so red, green and blue all come back equal.
  const uint32_t readBack = pixmap.getPixelARGB32(0, 0);
  EXPECT_EQ((readBack >> 16) & 0xFF, (readBack >> 8) & 0xFF);
  EXPECT_EQ((readBack >> 8) & 0xFF, readBack & 0xFF);
  EXPECT_EQ((readBack >> 24) & 0xFF, 255u) << "a format with no alpha is opaque";
}

TEST(PixmapTest, Rgb565PacksAndUnpacks) {
  const ImageInfo info(2, 1, ColorType::kRGB_565, AlphaType::kOpaque);
  std::vector<uint8_t> buffer = makeBuffer(info);
  Pixmap pixmap(info, buffer.data());

  EXPECT_EQ(pixmap.rowBytes(), 4u) << "two pixels of two bytes";

  // Pure white must survive the round trip. Packing by truncation rather than rounding gives 30 in the
  // five-bit channels instead of 31, so white comes back grey -- which is the failure this catches.
  pixmap.setPixelARGB32(0, 0, argb(255, 255, 255, 255));
  const uint32_t white = pixmap.getPixelARGB32(0, 0);
  EXPECT_EQ((white >> 16) & 0xFF, 255u) << "white stays white";
  EXPECT_EQ((white >> 8) & 0xFF, 255u);
  EXPECT_EQ(white & 0xFF, 255u);

  // Black is exact, being the other end of every range.
  pixmap.setPixelARGB32(1, 0, argb(255, 0, 0, 0));
  EXPECT_EQ(pixmap.getPixelARGB32(1, 0) & 0x00FFFFFFu, 0u);
}

TEST(PixmapTest, AlphaOnlyFormatCarriesCoverageAndNothingElse) {
  const ImageInfo info(2, 1, ColorType::kAlpha_8, AlphaType::kPremultiplied);
  std::vector<uint8_t> buffer = makeBuffer(info);
  Pixmap pixmap(info, buffer.data());

  pixmap.setPixelARGB32(0, 0, argb(200, 255, 255, 255));
  EXPECT_EQ(buffer[0], 200) << "only the coverage is stored";

  // What comes back is coverage with no colour -- the format has none to give, and inventing white would
  // be a plausible lie.
  const uint32_t readBack = pixmap.getPixelARGB32(0, 0);
  EXPECT_EQ((readBack >> 24) & 0xFF, 200u);
  EXPECT_EQ(readBack & 0x00FFFFFFu, 0u);
}

TEST(PixmapTest, WideFormatsAreReportedAsNoColourRatherThanApproximated) {
  // Reading a half-float format through the 8-bit accessor is not a shift, and an approximation would
  // produce a plausible colour that is simply wrong. No colour is the honest answer, and is what a caller
  // can detect.
  const ImageInfo info(2, 1, ColorType::kRGBA_F16, AlphaType::kUnpremultiplied);
  std::vector<uint8_t> buffer = makeBuffer(info);
  Pixmap pixmap(info, buffer.data());

  EXPECT_TRUE(pixmap.isValid()) << "the format is storable, so the geometry is fine";

  const uint32_t readBack = pixmap.getPixelARGB32(0, 0);
  EXPECT_EQ(readBack & 0x00FFFFFFu, 0u) << "no colour comes out of a format this accessor cannot read";

  // The alpha is opaque, not transparent, and the distinction matters: alpha is not read out of the pixel at
  // all for a format with no alpha channel, so it is a property of the format rather than of the bytes. An
  // implementation that zeroed the alpha too would make every such image fully transparent -- invisible
  // rather than merely colourless.
  EXPECT_EQ((readBack >> 24) & 0xFFu, 255u) << "a format with no alpha channel reads as opaque";
}

TEST(PixmapTest, DefaultConstructedIsEmptyAndUnusable) {
  const Pixmap pixmap;
  EXPECT_TRUE(pixmap.isEmpty());
  EXPECT_FALSE(pixmap.isValid());
  EXPECT_EQ(pixmap.pixels(), nullptr);
  EXPECT_EQ(pixmap.width(), 0);
  EXPECT_EQ(pixmap.getPixelARGB32(0, 0), 0u);
}
