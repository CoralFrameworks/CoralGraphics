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
 * @file test_ImageGenerator.cpp
 *
 * Holding encoded bytes and producing pixels from them more than once.
 *
 * ## What is worth asserting
 *
 * The value of a generator over `Image::decode` is that the decode happens *once*, so the assertions that matter are
 * about identity rather than about pixels: the second `image()` must be over the same pixels as the first, and the
 * answers must be consistent. A generator that decoded on every call would produce equal pixels and pass a comparison
 * of *contents*, so the check is on the underlying bytes being shared -- that is the only thing that tells the two
 * implementations apart.
 *
 * The other thing worth asserting is what a generator refuses. It is handed data from a file or a network response,
 * which is the least trustworthy input in a graphics stack, so a generator over bytes that are not an image, or asked
 * to decode into a buffer of the wrong shape, must say so rather than write something plausible.
 *
 * ## Where the bytes come from
 *
 * The test encodes its own images with `encodeImage` and decodes them back, so it depends on nothing outside this
 * library -- no fixture files, no working directory, and no format this build might not support. A test that needed a
 * checked-in PNG would fail for a reason that had nothing to do with generators.
 */

#include <graphics/ImageGenerator.h>

#include <graphics/Bitmap.h>
#include <graphics/Image.h>
#include <graphics/Pixmap.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace graphics;

namespace {

constexpr int kWidth = 8;
constexpr int kHeight = 6;

/** @return a bitmap with a deterministic pattern, so two decodes can be compared byte by byte. */
Bitmap makeBitmap() {
  Bitmap bitmap = Bitmap::allocate(kWidth, kHeight);
  Pixmap pixmap = bitmap.makePixmap();
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const auto r = static_cast<uint32_t>(x * 32);
      const auto g = static_cast<uint32_t>(y * 40);
      const auto b = static_cast<uint32_t>((x + y) * 16);
      pixmap.setPixelARGB32(x, y, (uint32_t { 255 } << 24) | (r << 16) | (g << 8) | b);
    }
  }
  return bitmap;
}

/** @return the encoded bytes of a small image, or an empty vector when the format is unsupported. */
std::vector<uint8_t> encodeTestImage(const ImageFormat format) {
  if (!isEncodeSupported(format)) return {};

  const Image image = Image::fromBitmap(makeBitmap());
  const Data data = image.encode(format, 100);
  if (data.size() == 0) return {};
  return { data.bytes(), data.bytes() + data.size() };
}

/** @return the pixels of an image, row by row without padding, for comparing two decodes. */
std::vector<uint8_t> pixelsOf(const Image& image) {
  const Pixmap pixmap = image.makePixmap();
  if (pixmap.isEmpty()) return {};

  const size_t rowBytes = static_cast<size_t>(pixmap.width()) * 4u;
  std::vector<uint8_t> pixels;
  pixels.reserve(rowBytes * static_cast<size_t>(pixmap.height()));
  for (int y = 0; y < pixmap.height(); ++y) {
    const auto* row = static_cast<const uint8_t*>(pixmap.addr(0, y));
    if (row == nullptr) return {};
    pixels.insert(pixels.end(), row, row + rowBytes);
  }
  return pixels;
}

/** @return a format this build can both encode and decode, or `Unknown` when there is none. */
ImageFormat usableFormat() {
  for (const ImageFormat format: { ImageFormat::PNG, ImageFormat::JPEG, ImageFormat::WebP }) {
    if (isEncodeSupported(format) && isDecodeSupported(format)) return format;
  }
  return ImageFormat::Unknown;
}

/** @return true when PNG can be both written and read here, which the exact-round-trip tests need. */
bool canRoundTripPng() { return isEncodeSupported(ImageFormat::PNG) && isDecodeSupported(ImageFormat::PNG); }

} // namespace anonymous

// ==================================================================================================
// Building one
// ==================================================================================================

TEST(ImageGeneratorTest, RefusesBytesThatAreNotAnImage) {
  const std::vector<uint8_t> notAnImage { 'h', 'e', 'l', 'l', 'o' };
  EXPECT_EQ(ImageGenerator::fromEncoded(notAnImage), nullptr) << "the format is judged by content, not by a name";

  EXPECT_EQ(ImageGenerator::fromEncoded(nullptr, 0), nullptr);
  EXPECT_EQ(ImageGenerator::fromEncoded(std::vector<uint8_t> {}), nullptr);
}

TEST(ImageGeneratorTest, RecognisesTheFormatFromTheBytes) {
  const ImageFormat format = usableFormat();
  if (format == ImageFormat::Unknown) GTEST_SKIP() << "this build has no format it can both encode and decode";

  const std::vector<uint8_t> encoded = encodeTestImage(format);
  ASSERT_FALSE(encoded.empty());

  const auto generator = ImageGenerator::fromEncoded(encoded);
  ASSERT_NE(generator, nullptr);
  EXPECT_EQ(generator->format(), format) << "recognised by its magic number";
}

TEST(ImageGeneratorTest, ReportsTheDimensions) {
  const ImageFormat format = usableFormat();
  if (format == ImageFormat::Unknown) GTEST_SKIP() << "no usable format";

  const std::vector<uint8_t> encoded = encodeTestImage(format);
  ASSERT_FALSE(encoded.empty());

  const auto generator = ImageGenerator::fromEncoded(encoded);
  ASSERT_NE(generator, nullptr);

  const auto info = generator->info();
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->width(), kWidth);
  EXPECT_EQ(info->height(), kHeight);
  EXPECT_TRUE(generator->isValid());
}

TEST(ImageGeneratorTest, RefusesAMissingFile) {
  EXPECT_EQ(ImageGenerator::fromFile("this-file-does-not-exist-at-all.png"), nullptr);
}

// ==================================================================================================
// Decoding once
// ==================================================================================================

TEST(ImageGeneratorTest, TheDecodeHappensOnceAndThePixelsAreShared) {
  // The property that is the whole point of the type. Asserted on the *bytes* being shared rather than on the pixels
  // being equal: a generator that decoded on every call would give equal pixels and pass a content comparison, so
  // only the sharing tells the two apart.
  const ImageFormat format = usableFormat();
  if (format == ImageFormat::Unknown) GTEST_SKIP() << "no usable format";

  const std::vector<uint8_t> encoded = encodeTestImage(format);
  ASSERT_FALSE(encoded.empty());

  const auto generator = ImageGenerator::fromEncoded(encoded);
  ASSERT_NE(generator, nullptr);

  const Image first = generator->image();
  ASSERT_FALSE(first.isEmpty());
  const Image second = generator->image();

  // Two `Image`s over one buffer, so the second call cost nothing.
  EXPECT_EQ(first.data().bytes(), second.data().bytes())
          << "the second call must reuse the first's pixels, not produce a second buffer that happens to match";
  EXPECT_EQ(first.data().size(), second.data().size());

  // And the observable answers agree.
  EXPECT_EQ(pixelsOf(first), pixelsOf(second));
  EXPECT_EQ(generator->info()->width(), first.info().width());
}

TEST(ImageGeneratorTest, APngRoundTripsExactly) {
  // A lossless format hands back what was put in, which is the check that the generator produces the image rather
  // than an image of the right shape. Only PNG is asserted this way; JPEG is lossy and cannot be.
  if (!canRoundTripPng()) GTEST_SKIP() << "PNG is not available in this build";

  const std::vector<uint8_t> encoded = encodeTestImage(ImageFormat::PNG);
  ASSERT_FALSE(encoded.empty());

  const auto generator = ImageGenerator::fromEncoded(encoded);
  ASSERT_NE(generator, nullptr);

  const std::vector<uint8_t> decoded = pixelsOf(generator->image());
  ASSERT_FALSE(decoded.empty());

  const Bitmap original = makeBitmap();
  const Pixmap pixmap = original.makePixmap();
  const size_t rowBytes = static_cast<size_t>(kWidth) * 4u;

  for (int y = 0; y < kHeight; ++y) {
    const auto* row = static_cast<const uint8_t*>(pixmap.addr(0, y));
    ASSERT_NE(row, nullptr);
    for (size_t i = 0; i < rowBytes; ++i) {
      EXPECT_EQ(decoded[static_cast<size_t>(y) * rowBytes + i], row[i])
              << "row " << y << " byte " << i << " differs, so the round trip was not exact";
    }
  }
}

TEST(ImageGeneratorTest, TheEncodedBytesAreHandedBackUnchanged) {
  // The caller gave these bytes and may want them for a cache key or a checksum, so a generator that re-encoded would
  // hand back different bytes describing the same picture -- breaking the key for no reason.
  const ImageFormat format = usableFormat();
  if (format == ImageFormat::Unknown) GTEST_SKIP() << "no usable format";

  const std::vector<uint8_t> encoded = encodeTestImage(format);
  ASSERT_FALSE(encoded.empty());

  const auto generator = ImageGenerator::fromEncoded(encoded);
  ASSERT_NE(generator, nullptr);
  EXPECT_EQ(generator->encodedData(), encoded);
}

TEST(ImageGeneratorTest, AGeneratorOverPixelsNeedsNoDecoding) {
  const Image source = Image::fromBitmap(makeBitmap());
  ASSERT_FALSE(source.isEmpty());

  const auto generator = ImageGenerator::fromImage(source);
  ASSERT_NE(generator, nullptr);

  EXPECT_TRUE(generator->isValid()) << "pixels are already pixels";
  EXPECT_EQ(generator->format(), ImageFormat::Unknown) << "there are no bytes to judge a format from";
  EXPECT_TRUE(generator->encodedData().empty());

  EXPECT_EQ(pixelsOf(generator->image()), pixelsOf(source));
}

TEST(ImageGeneratorTest, AGeneratorOverNothingIsRefused) {
  EXPECT_EQ(ImageGenerator::fromImage(Image {}), nullptr);
}

// ==================================================================================================
// Decoding into a caller's buffer
// ==================================================================================================

TEST(ImageGeneratorTest, DecodesIntoACallerBuffer) {
  if (!canRoundTripPng()) GTEST_SKIP() << "PNG is not available in this build";

  const std::vector<uint8_t> encoded = encodeTestImage(ImageFormat::PNG);
  ASSERT_FALSE(encoded.empty());

  const auto generator = ImageGenerator::fromEncoded(encoded);
  ASSERT_NE(generator, nullptr);

  const auto info = generator->info();
  ASSERT_TRUE(info.has_value());

  std::vector<uint8_t> destination(info->computeByteSize(), 0);
  ASSERT_TRUE(generator->decodeInto(destination.data(), info->minRowBytes(), *info));

  EXPECT_EQ(destination, pixelsOf(generator->image())) << "the buffer holds the image";
}

TEST(ImageGeneratorTest, DecodesIntoABufferWithLongerRows) {
  // The case a single `memcpy` of the whole image gets wrong. A destination whose rows are padded -- to an alignment,
  // or because it is a sub-rectangle of a larger buffer -- must have each row written at its own stride, with the
  // padding left alone.
  if (!canRoundTripPng()) GTEST_SKIP() << "PNG is not available in this build";

  const std::vector<uint8_t> encoded = encodeTestImage(ImageFormat::PNG);
  const auto generator = ImageGenerator::fromEncoded(encoded);
  ASSERT_NE(generator, nullptr);

  const auto info = generator->info();
  ASSERT_TRUE(info.has_value());

  const size_t paddedRow = info->minRowBytes() + 7u;
  std::vector<uint8_t> destination(paddedRow * static_cast<size_t>(info->height()), 0xAB);
  ASSERT_TRUE(generator->decodeInto(destination.data(), paddedRow, *info));

  const std::vector<uint8_t> expected = pixelsOf(generator->image());
  const size_t imageRow = info->minRowBytes();

  for (int y = 0; y < info->height(); ++y) {
    const size_t at = static_cast<size_t>(y) * paddedRow;
    for (size_t i = 0; i < imageRow; ++i) {
      EXPECT_EQ(destination[at + i], expected[static_cast<size_t>(y) * imageRow + i])
              << "row " << y << " byte " << i;
    }
    // A version that wrote the rows end to end would have put the next row's first byte here, which is how the bug
    // would show.
    for (size_t i = imageRow; i < paddedRow; ++i) {
      EXPECT_EQ(destination[at + i], 0xAB) << "the padding after row " << y << " must be left alone";
    }
  }
}

TEST(ImageGeneratorTest, RefusesADestinationOfTheWrongShape) {
  if (!canRoundTripPng()) GTEST_SKIP() << "PNG is not available in this build";

  const std::vector<uint8_t> encoded = encodeTestImage(ImageFormat::PNG);
  const auto generator = ImageGenerator::fromEncoded(encoded);
  ASSERT_NE(generator, nullptr);

  const auto info = generator->info();
  ASSERT_TRUE(info.has_value());

  std::vector<uint8_t> destination(info->computeByteSize(), 0);

  // Dimensions that do not match. Honouring this by scaling would write past the end of a buffer the caller sized for
  // the smaller image, so it is refused rather than approximated.
  const ImageInfo half(info->width() / 2, info->height() / 2, info->colorType(), info->alphaType());
  EXPECT_FALSE(generator->decodeInto(destination.data(), half.minRowBytes(), half))
          << "a change of dimensions is refused, not scaled";

  // A row shorter than the image needs.
  EXPECT_FALSE(generator->decodeInto(destination.data(), info->minRowBytes() - 1, *info))
          << "a row too short to hold the image is refused";

  // And no destination at all.
  EXPECT_FALSE(generator->decodeInto(nullptr, info->minRowBytes(), *info));
}

TEST(ImageGeneratorTest, DecodingFromAFileGivesTheSameImage) {
  const ImageFormat format = usableFormat();
  if (format == ImageFormat::Unknown) GTEST_SKIP() << "no usable format";

  const std::vector<uint8_t> encoded = encodeTestImage(format);
  ASSERT_FALSE(encoded.empty());

  const std::filesystem::path path = std::filesystem::temp_directory_path() / "coral-generator-test.bin";
  {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    if (!file.good()) {
      // The test failed here silently until this check: `fromFile` then reported "cannot open" and the
      // failure looked like a decoder bug. An environment that cannot write its own temp directory is a
      // skip, not a defect in the code under test.
      GTEST_SKIP() << "this environment cannot write a temporary file: " << path.string();
    }
  }

  const auto generator = ImageGenerator::fromFile(path.string());
  ASSERT_NE(generator, nullptr);
  EXPECT_EQ(generator->format(), format);
  EXPECT_EQ(generator->encodedData(), encoded) << "the file's bytes are what is held";

  const auto fromMemory = ImageGenerator::fromEncoded(encoded);
  ASSERT_NE(fromMemory, nullptr);
  EXPECT_EQ(pixelsOf(generator->image()), pixelsOf(fromMemory->image()));

  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}
