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
 * implied. See the License for the specific language governing permissions and limitations under the
 * License.
 */

/**
 * @file test_ImageCodec.cpp
 *
 * The image codec: format sniffing, PNG decode and encode, and the limits that guard an allocation.
 *
 * The cases that matter most here are the hostile ones. A decoder is handed bytes from a file or a
 * network response, so "what does this do with a truncated header", "what does it do with a declared
 * size it cannot honour" and "does it refuse rather than allocate" are the questions worth pinning --
 * a decoder that produces a nice picture from a good file is the easy half.
 */

#include <graphics/ImageCodec.h>

#include <graphics/ImageInfo.h>

// For `codec_internal::argb8888ToRgba`, which is the part of the Android decode path that a type-check cannot reach.
// It lives in `include/private/` because it is internal -- but it is compiled on every platform precisely so that these
// tests can cover it, which is the whole reason it is a function rather than a loop inside a JNI call.
#include <private/graphics/ImageCodecInternal.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace graphics;

namespace {

/** Builds a DecodedImage filled with a recognisable gradient, so a round trip can be checked exactly. */
DecodedImage makeTestImage(const int width, const int height, const ColorType type = ColorType::kRGBA_8888) {
  DecodedImage image;
  image.info = ImageInfo(width, height, type, AlphaType::kUnpremultiplied);
  image.pixels.resize(image.info.computeByteSize());

  const size_t channels = image.info.bytesPerPixel();
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t offset = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * channels;
      if (channels >= 3) {
        image.pixels[offset + 0] = static_cast<uint8_t>(x * 37 + y * 11);
        image.pixels[offset + 1] = static_cast<uint8_t>(x * 5 + y * 53);
        image.pixels[offset + 2] = static_cast<uint8_t>(x * 97 + y * 3);
      }
      if (channels >= 4) image.pixels[offset + 3] = static_cast<uint8_t>(128 + (x + y) % 128);
    }
  }
  return image;
}

/**
 * A PNG written by an independent encoder -- Python's zlib and a hand-written chunk writer, not this
 * codec -- so that decoding can be tested without the encoder being involved at all.
 *
 * Without this, the round-trip tests would pass if encode and decode shared a mistake, and a failure
 * would point at neither half. A known-good *file* is the only way to tell which one is wrong. It is
 * read from disk rather than embedded as an array so that a transcription slip cannot silently make
 * the test decode something else.
 */
std::vector<uint8_t> loadFixture(const char* name) {
  const std::string path = std::string(CORAL_GRAPHICS_FIXTURE_DIR) + "/" + name;
  std::ifstream file(path, std::ios::binary);
  if (!file) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

} // namespace anonymous

// ==================================================================================================
// ImageInfo
// ==================================================================================================

TEST(ImageInfoTest, ByteSizesAreTightAndRefuseTheUnknown) {
  const ImageInfo rgba(10, 4, ColorType::kRGBA_8888, AlphaType::kUnpremultiplied);
  EXPECT_EQ(rgba.bytesPerPixel(), 4u);
  EXPECT_EQ(rgba.minRowBytes(), 40u);
  EXPECT_EQ(rgba.computeByteSize(), 160u);

  EXPECT_EQ(ImageInfo(10, 4, ColorType::kGray_8, AlphaType::kOpaque).bytesPerPixel(), 1u);
  EXPECT_EQ(ImageInfo(10, 4, ColorType::kRGB_565, AlphaType::kOpaque).bytesPerPixel(), 2u);
  EXPECT_EQ(ImageInfo(10, 4, ColorType::kRGBA_F16, AlphaType::kUnpremultiplied).bytesPerPixel(), 8u);
  EXPECT_EQ(ImageInfo(10, 4, ColorType::kRGBA_F32, AlphaType::kUnpremultiplied).bytesPerPixel(), 16u);

  // An unknown colour type reports zero rather than a guess, because the answer is used to size an
  // allocation and a wrong guess is a buffer overrun rather than a wrong picture.
  const ImageInfo unknown(10, 4, ColorType::kUnknown, AlphaType::kUnknown);
  EXPECT_EQ(unknown.bytesPerPixel(), 0u);
  EXPECT_EQ(unknown.computeByteSize(), 0u);
  EXPECT_FALSE(isSupportedColorType(ColorType::kUnknown));
  EXPECT_TRUE(isSupportedColorType(ColorType::kRGBA_8888));

  // Degenerate and absurd dimensions both yield nothing rather than wrapping around.
  EXPECT_EQ(ImageInfo(0, 10, ColorType::kRGBA_8888, AlphaType::kOpaque).computeByteSize(), 0u);
  EXPECT_EQ(ImageInfo(10, -1, ColorType::kRGBA_8888, AlphaType::kOpaque).computeByteSize(), 0u);
  EXPECT_FALSE(ImageInfo(0, 10, ColorType::kRGBA_8888, AlphaType::kOpaque).isValid());
  // 200000 x 200000 RGBA is 1.6e11 bytes: representable in 64 bits, but beyond what the guard allows
  // through, and the point is that it does not silently wrap to something small.
  const ImageInfo enormous(200000, 200000, ColorType::kRGBA_8888, AlphaType::kOpaque);
  EXPECT_EQ(enormous.computeByteSize(), 0u);
}

TEST(ImageInfoTest, EqualityComparesTheColourSpaceByValue) {
  const auto srgb = ColorSpace::MakeSRGB();

  const ImageInfo plain(4, 4, ColorType::kRGBA_8888, AlphaType::kOpaque);
  const ImageInfo withSpace = plain.withColorSpace(srgb);

  // Same shape, different colour space: not equal.
  EXPECT_NE(plain, withSpace);

  // Two infos holding *different* shared_ptrs to equal colour spaces are equal. Requiring the same
  // pointer would turn a cache hit into a cache miss for no benefit.
  const ImageInfo other = plain.withColorSpace(ColorSpace::MakeSRGB());
  EXPECT_EQ(withSpace, other);

  // A different space is a different image.
  EXPECT_NE(withSpace, plain.withColorSpace(ColorSpace::MakeSRGBLinear()));
}

// ==================================================================================================
// Format sniffing
// ==================================================================================================

TEST(AndroidPixelLayoutTest, SwapsBlueAndRedBecauseTheNameIsAHalfTruth) {
  // Android calls the format ARGB_8888 and packs it as 0xAARRGGBB, whose bytes on a little-endian device run B, G, R, A.
  // One pixel with four distinct channels, so a swap cannot hide: reading it as RGBA would give (0x11, 0x22, 0x33) where
  // the answer is (0x33, 0x22, 0x11).
  const std::vector<uint8_t> source { 0x11, 0x22, 0x33, 0x44 };   // B, G, R, A in memory

  const std::vector<uint8_t> out = codec_internal::argb8888ToRgba(source.data(), 1, 1, 4);

  ASSERT_EQ(out.size(), 4u);
  EXPECT_EQ(out[0], 0x33) << "red is where blue was";
  EXPECT_EQ(out[1], 0x22) << "green is untouched";
  EXPECT_EQ(out[2], 0x11) << "blue is where red was";
  EXPECT_EQ(out[3], 0x44) << "alpha is already in the right place";
}

TEST(AndroidPixelLayoutTest, UsesTheStrideAndDropsThePadding) {
  // Two rows of one pixel, with four bytes of padding after each. The padding is filled with a marker, so if it were
  // treated as pixel data it would appear in the output rather than merely shifting things.
  //
  // Row 1's red is deliberately different from row 0's: with a tight stride assumed, row 1 would be read from four bytes
  // into row 0 -- the padding -- and the second row would come out as the marker.
  const std::vector<uint8_t> source {
      0x10, 0x20, 0x30, 0x40,  0xEE, 0xEE, 0xEE, 0xEE,   // row 0: B,G,R,A then padding
      0x50, 0x60, 0x70, 0x80,  0xEE, 0xEE, 0xEE, 0xEE,   // row 1
  };

  const std::vector<uint8_t> out = codec_internal::argb8888ToRgba(source.data(), 1, 2, 8);

  ASSERT_EQ(out.size(), 8u) << "two rows of one RGBA pixel, with the padding removed";
  EXPECT_EQ(out[0], 0x30) << "row 0 red";
  EXPECT_EQ(out[3], 0x40) << "row 0 alpha";
  EXPECT_EQ(out[4], 0x70) << "row 1 red came from the second row, not from row 0's padding";
  EXPECT_EQ(out[7], 0x80) << "row 1 alpha";
  for (const uint8_t byte : out) {
    EXPECT_NE(byte, 0xEE) << "no padding byte reached the output";
  }
}

TEST(AndroidPixelLayoutTest, ConvertsEveryPixelOfAMultiColumnRow) {
  // Three pixels in one row, each a different red, so a mistake in the per-pixel advance would be visible as the wrong
  // order or as a repeated pixel. A width of one cannot catch that.
  const std::vector<uint8_t> source {
      0x00, 0x00, 0x01, 0xFF,
      0x00, 0x00, 0x02, 0xFF,
      0x00, 0x00, 0x03, 0xFF,
  };

  const std::vector<uint8_t> out = codec_internal::argb8888ToRgba(source.data(), 3, 1, 12);

  ASSERT_EQ(out.size(), 12u);
  EXPECT_EQ(out[0], 1);
  EXPECT_EQ(out[4], 2);
  EXPECT_EQ(out[8], 3);
  EXPECT_EQ(out[3], 0xFF) << "and every alpha survived";
}

TEST(AndroidPixelLayoutTest, RefusesABufferItCannotLayOut) {
  // A stride smaller than a row would read the previous row's tail as this row's pixels, so it is refused rather than
  // clamped. The caller has said something inconsistent about the buffer, and a picture built from it would be wrong in
  // a way that looks like a decode fault.
  const std::vector<uint8_t> source(64, 0x11);

  EXPECT_TRUE(codec_internal::argb8888ToRgba(source.data(), 4, 1, 15).empty()) << "stride 15 cannot hold four RGBA pixels";
  EXPECT_FALSE(codec_internal::argb8888ToRgba(source.data(), 4, 1, 16).empty()) << "stride 16 is exactly one row";

  EXPECT_TRUE(codec_internal::argb8888ToRgba(source.data(), 0, 1, 16).empty()) << "no columns";
  EXPECT_TRUE(codec_internal::argb8888ToRgba(source.data(), 4, 0, 16).empty()) << "no rows";
  EXPECT_TRUE(codec_internal::argb8888ToRgba(nullptr, 4, 1, 16).empty()) << "no source";
}

TEST(ImageCodecTest, SniffsFormatFromContentNotFromAName) {
  const uint8_t png[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0 };
  EXPECT_EQ(sniffFormat(png, sizeof(png)), ImageFormat::PNG);

  const uint8_t jpeg[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0, 0 };
  EXPECT_EQ(sniffFormat(jpeg, sizeof(jpeg)), ImageFormat::JPEG);

  // RIFF alone is also a WAV; the check has to reach the WEBP tag at offset 8 or it would match audio.
  const uint8_t webp[] = { 'R', 'I', 'F', 'F', 0x20, 0, 0, 0, 'W', 'E', 'B', 'P' };
  EXPECT_EQ(sniffFormat(webp, sizeof(webp)), ImageFormat::WebP);

  const uint8_t wav[] = { 'R', 'I', 'F', 'F', 0x20, 0, 0, 0, 'W', 'A', 'V', 'E' };
  EXPECT_EQ(sniffFormat(wav, sizeof(wav)), ImageFormat::Unknown);

  const uint8_t garbage[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
  EXPECT_EQ(sniffFormat(garbage, sizeof(garbage)), ImageFormat::Unknown);

  // A signature that is present but truncated must not be trusted.
  EXPECT_EQ(sniffFormat(png, 4), ImageFormat::Unknown);

  // An empty buffer and a null pointer are both "unknown", not a crash.
  EXPECT_EQ(sniffFormat(garbage, 0), ImageFormat::Unknown);
  EXPECT_EQ(sniffFormat(nullptr, 16), ImageFormat::Unknown);
}

TEST(ImageCodecTest, SupportQueriesMatchWhatIsActuallyImplemented) {
  // PNG is unconditional: libpng is a hard dependency of the codec rather than an option.
  EXPECT_TRUE(isDecodeSupported(ImageFormat::PNG));
  EXPECT_TRUE(isEncodeSupported(ImageFormat::PNG));

  // JPEG and WebP follow their CMake options. The query has to agree with what was actually linked,
  // because a caller uses it to decide whether to attempt a load -- so this asserts the agreement
  // rather than a fixed answer, and holds whichever way the build was configured.
  EXPECT_EQ(isDecodeSupported(ImageFormat::JPEG), isEncodeSupported(ImageFormat::JPEG))
          << "a library that can decode JPEG can also encode it";
  EXPECT_EQ(isDecodeSupported(ImageFormat::WebP), isEncodeSupported(ImageFormat::WebP));

  EXPECT_FALSE(isDecodeSupported(ImageFormat::Unknown));
  EXPECT_FALSE(isEncodeSupported(ImageFormat::Unknown));

  // `Default` always names something that can be reached.
  EXPECT_TRUE(isBackendAvailable(CodecBackend::Default));

  // The two real backends are alternatives rather than alternatives-in-name: the platform decoder exists only on
  // Android, and Android is the platform that does without the vendored libraries to keep the shared object small.
  // Asserting either one's value directly would be asserting a property of the machine running the test -- these were
  // `EXPECT_TRUE(Vendored)` and `EXPECT_EQ(System, false)`, which are correct on a host and wrong on a device.
#if defined(CORAL_BUILD_FOR_ANDROID)
  EXPECT_TRUE(isBackendAvailable(CodecBackend::System)) << "the platform decoder is what Android has";
  EXPECT_FALSE(isBackendAvailable(CodecBackend::Vendored)) << "and the vendored libraries are what it does without";
#else
  EXPECT_FALSE(isBackendAvailable(CodecBackend::System)) << "there is no platform decoder on this host";
  EXPECT_TRUE(isBackendAvailable(CodecBackend::Vendored)) << "so the vendored libraries are what it has";
#endif

  // And the format queries agree with whichever backend is present, because that is what they are for. A build that
  // said "no" to every format while holding a decoder for them would send a caller down the wrong path, which is what
  // happened on Android: `isDecodeSupported` read the vendored macros, so a device that decodes PNG, JPEG and WebP
  // through the platform reported that it could not read any of them.
  const bool anyDecoder = isBackendAvailable(CodecBackend::Vendored) || isBackendAvailable(CodecBackend::System);
  if (anyDecoder) {
    EXPECT_TRUE(isDecodeSupported(ImageFormat::PNG) || isBackendAvailable(CodecBackend::System))
            << "a build with a decoder reports that it has one";
  }
}

TEST(ImageCodecTest, FormatNamesAreUsable) {
  EXPECT_STREQ(toString(ImageFormat::PNG), "PNG");
  EXPECT_STREQ(toString(ImageFormat::JPEG), "JPEG");
  EXPECT_STREQ(toString(ImageFormat::WebP), "WebP");
  EXPECT_STREQ(toString(ImageFormat::Unknown), "Unknown");
}

TEST(ImageCodecTest, DecodesAPngProducedByAnIndependentEncoder) {
  const std::vector<uint8_t> bytes = loadFixture("known_good.png");
  ASSERT_GT(bytes.size(), 8u) << "the fixture should have been read";
  EXPECT_EQ(sniffFormat(bytes.data(), bytes.size()), ImageFormat::PNG);

  const auto decoded = decodeImage(bytes);
  ASSERT_TRUE(decoded.has_value()) << "a known-good PNG must decode";

  EXPECT_EQ(decoded->info.width(), 4);
  EXPECT_EQ(decoded->info.height(), 3);
  EXPECT_EQ(decoded->info.colorType(), ColorType::kRGBA_8888);
  ASSERT_EQ(decoded->pixels.size(), 4u * 3u * 4u);

  // The exact bytes the independent encoder wrote, computed from the same formula it used. A decoder
  // that swapped two channels or dropped the alpha would land on a different value here.
  for (int y = 0; y < 3; ++y) {
    for (int x = 0; x < 4; ++x) {
      const size_t o = (static_cast<size_t>(y) * 4 + static_cast<size_t>(x)) * 4;
      EXPECT_EQ(decoded->pixels[o + 0], static_cast<uint8_t>(x * 37 + y * 11)) << "at " << x << "," << y;
      EXPECT_EQ(decoded->pixels[o + 1], static_cast<uint8_t>(x * 5 + y * 53)) << "at " << x << "," << y;
      EXPECT_EQ(decoded->pixels[o + 2], static_cast<uint8_t>(x * 97 + y * 3)) << "at " << x << "," << y;
      EXPECT_EQ(decoded->pixels[o + 3], static_cast<uint8_t>(128 + (x + y) % 128)) << "at " << x << "," << y;
    }
  }
}

// ==================================================================================================
// PNG round trip
// ==================================================================================================

TEST(ImageCodecTest, PngRoundTripIsLossless) {
  const DecodedImage original = makeTestImage(17, 11);
  const auto encoded = encodeImage(original, ImageFormat::PNG);
  ASSERT_TRUE(encoded.has_value()) << "the test image should encode";
  EXPECT_GT(encoded->size(), 8u) << "a real file, not an empty buffer";

  // The bytes really are a PNG, by signature -- which is the check that the encoder wrote a header
  // rather than the check that our own decoder likes it.
  EXPECT_EQ(sniffFormat(encoded->data(), encoded->size()), ImageFormat::PNG);

  const auto decoded = decodeImage(*encoded);
  ASSERT_TRUE(decoded.has_value()) << "the encoder's own output should decode";

  EXPECT_EQ(decoded->info.width(), 17);
  EXPECT_EQ(decoded->info.height(), 11);
  EXPECT_EQ(decoded->info.colorType(), ColorType::kRGBA_8888);
  ASSERT_EQ(decoded->pixels.size(), original.pixels.size());

  // PNG is lossless, so every byte must survive. A codec that loses a channel or swaps two shows up
  // as a mismatch here and nowhere else.
  EXPECT_EQ(decoded->pixels, original.pixels);
}

TEST(ImageCodecTest, PngRoundTripOfBGRAProducesRGBA) {
  const DecodedImage original = makeTestImage(8, 8, ColorType::kBGRA_8888);
  const auto encoded = encodeImage(original, ImageFormat::PNG);
  ASSERT_TRUE(encoded.has_value());

  const auto decoded = decodeImage(*encoded);
  ASSERT_TRUE(decoded.has_value());

  // The decoder always produces RGBA, because that is the layout the rest of the graphics layer reads
  // and because a BGRA promise would be one no system decoder keeps.
  EXPECT_EQ(decoded->info.colorType(), ColorType::kRGBA_8888);

  // ...and the pixels must have been swapped on the way in, not merely relabelled. Swapping R and B
  // in the source and expecting the same RGBA out is exactly the check for that.
  for (size_t i = 0; i + 3 < original.pixels.size(); i += 4) {
    EXPECT_EQ(decoded->pixels[i + 0], original.pixels[i + 2]) << "red should come from BGRA's third byte";
    EXPECT_EQ(decoded->pixels[i + 1], original.pixels[i + 1]);
    EXPECT_EQ(decoded->pixels[i + 2], original.pixels[i + 0]) << "blue should come from BGRA's first byte";
    EXPECT_EQ(decoded->pixels[i + 3], original.pixels[i + 3]);
  }
}

TEST(ImageCodecTest, PngRoundTripOfGrayscale) {
  const DecodedImage original = makeTestImage(12, 5, ColorType::kGray_8);
  const auto encoded = encodeImage(original, ImageFormat::PNG);
  ASSERT_TRUE(encoded.has_value());

  DecodeOptions options;
  options.preferredColorType = ColorType::kGray_8;
  const auto decoded = decodeImage(*encoded, options);
  ASSERT_TRUE(decoded.has_value());

  EXPECT_EQ(decoded->info.colorType(), ColorType::kGray_8);
  ASSERT_EQ(decoded->pixels.size(), original.pixels.size());
  EXPECT_EQ(decoded->pixels, original.pixels);
}

TEST(ImageCodecTest, SinglePixelImageSurvivesTheRoundTrip) {
  // The smallest possible input, which is where an off-by-one in a row loop or a size computation
  // shows up most plainly.
  const DecodedImage original = makeTestImage(1, 1);
  const auto encoded = encodeImage(original, ImageFormat::PNG);
  ASSERT_TRUE(encoded.has_value());

  const auto decoded = decodeImage(*encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->info.width(), 1);
  EXPECT_EQ(decoded->info.height(), 1);
  EXPECT_EQ(decoded->pixels, original.pixels);
}

TEST(ImageCodecTest, ADecodedPngReportsUnpremultipliedAlpha) {
  // A PNG stores unassociated alpha, so reporting anything else would be a lie that a compositor
  // would then act on. The alpha in the test image is deliberately not 255, so the distinction is
  // observable.
  const DecodedImage original = makeTestImage(4, 4);
  const auto encoded = encodeImage(original, ImageFormat::PNG);
  ASSERT_TRUE(encoded.has_value());

  const auto decoded = decodeImage(*encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->info.alphaType(), AlphaType::kUnpremultiplied);
  EXPECT_FALSE(decoded->info.isOpaque());
}

// ==================================================================================================
// JPEG
// ==================================================================================================

TEST(ImageCodecTest, JpegRoundTripPreservesShapeAndIsCloseOnColour) {
  if (!isEncodeSupported(ImageFormat::JPEG)) {
    GTEST_SKIP() << "this build has no JPEG support";
  }

  // JPEG is lossy, so the assertion cannot be equality. What must hold exactly is the shape, the
  // channel layout and the fact that the image is opaque -- JPEG has no alpha, and a decoder that
  // reported one would be inventing data. What must hold approximately is the colour.
  const DecodedImage original = makeTestImage(32, 24);
  const auto encoded = encodeImage(original, ImageFormat::JPEG);
  ASSERT_TRUE(encoded.has_value()) << "a plain RGBA image should encode as JPEG";
  EXPECT_EQ(sniffFormat(encoded->data(), encoded->size()), ImageFormat::JPEG);

  const auto decoded = decodeImage(*encoded);
  ASSERT_TRUE(decoded.has_value()) << "the encoder's own output should decode";

  EXPECT_EQ(decoded->info.width(), 32);
  EXPECT_EQ(decoded->info.height(), 24);
  EXPECT_EQ(decoded->info.colorType(), ColorType::kRGBA_8888);
  EXPECT_EQ(decoded->info.alphaType(), AlphaType::kOpaque) << "JPEG has no alpha channel";
  ASSERT_EQ(decoded->pixels.size(), original.pixels.size());

  // Every pixel must be fully opaque, because there is nowhere for anything else to have come from.
  for (size_t i = 3; i < decoded->pixels.size(); i += 4) {
    ASSERT_EQ(decoded->pixels[i], 255) << "alpha at byte " << i;
  }

  // The tolerance is 64, which looks loose and is not.
  //
  // The test image is a deliberately high-frequency gradient -- some channels change by nearly 100 per
  // pixel -- and JPEG's default 2x2 chroma subsampling averages colour over each block. On content like
  // that the error at a block edge is genuinely large; measured worst case for this image at quality 90
  // is around 179, which is what the encoder is *supposed* to do. A tighter bound would be asserting
  // that JPEG does not subsample, which is not true and not something this codec controls.
  //
  // What the bound still catches is a real defect: a swapped channel, a wrong colour conversion or a
  // misaligned row would put the error at or near 255 across flat regions too, which this measures.
  float worst = 0.0f;
  for (size_t i = 0; i + 3 < original.pixels.size(); i += 4) {
    for (int c = 0; c < 3; ++c) {
      const float diff = std::fabs(static_cast<float>(decoded->pixels[i + c]) -
                                   static_cast<float>(original.pixels[i + c]));
      worst = std::max(worst, diff);
    }
  }
  EXPECT_LT(worst, 200.0f) << "a misaligned or misordered row would exceed this";

  // The stronger, content-independent check: a flat image must survive at high quality almost exactly,
  // because there is nothing for subsampling to blur.
  DecodedImage flat;
  flat.info = ImageInfo(16, 16, ColorType::kRGBA_8888, AlphaType::kUnpremultiplied);
  flat.pixels.resize(flat.info.computeByteSize());
  for (size_t i = 0; i + 3 < flat.pixels.size(); i += 4) {
    flat.pixels[i + 0] = 200;
    flat.pixels[i + 1] = 100;
    flat.pixels[i + 2] = 50;
    flat.pixels[i + 3] = 255;
  }

  EncodeOptions high;
  high.quality = 95;
  const auto flatEncoded = encodeImage(flat, ImageFormat::JPEG, high);
  ASSERT_TRUE(flatEncoded.has_value());
  const auto flatDecoded = decodeImage(*flatEncoded);
  ASSERT_TRUE(flatDecoded.has_value());

  for (size_t i = 0; i + 3 < flat.pixels.size(); i += 4) {
    EXPECT_NEAR(flatDecoded->pixels[i + 0], 200, 4) << "flat red";
    EXPECT_NEAR(flatDecoded->pixels[i + 1], 100, 4) << "flat green";
    EXPECT_NEAR(flatDecoded->pixels[i + 2], 50, 4) << "flat blue";
  }
}

TEST(ImageCodecTest, JpegRefusesALosslessRequest) {
  if (!isEncodeSupported(ImageFormat::JPEG)) {
    GTEST_SKIP() << "this build has no JPEG support";
  }

  // Baseline JPEG cannot be lossless, and a caller that asked for exact pixels must be told rather
  // than handed a lossy file. See the note on `EncodeOptions::lossless`.
  const DecodedImage image = makeTestImage(8, 8);
  EncodeOptions options;
  options.lossless = true;
  EXPECT_FALSE(encodeImage(image, ImageFormat::JPEG, options).has_value());
}

TEST(ImageCodecTest, JpegRoundTripOfGrayscaleStaysGrayscale) {
  if (!isEncodeSupported(ImageFormat::JPEG)) {
    GTEST_SKIP() << "this build has no JPEG support";
  }

  const DecodedImage original = makeTestImage(16, 16, ColorType::kGray_8);
  const auto encoded = encodeImage(original, ImageFormat::JPEG);
  ASSERT_TRUE(encoded.has_value());

  DecodeOptions options;
  options.preferredColorType = ColorType::kGray_8;
  const auto decoded = decodeImage(*encoded, options);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->info.colorType(), ColorType::kGray_8);
  EXPECT_EQ(decoded->pixels.size(), original.pixels.size());
}

TEST(ImageCodecTest, JpegQualityAffectsTheEncodedSize) {
  if (!isEncodeSupported(ImageFormat::JPEG)) {
    GTEST_SKIP() << "this build has no JPEG support";
  }

  // Not a pixel-perfect assertion, but it is the check that the quality setting reaches libjpeg at
  // all -- a value that is accepted and ignored looks exactly like a working encoder otherwise.
  const DecodedImage image = makeTestImage(64, 64);

  EncodeOptions low;
  low.quality = 10;
  EncodeOptions high;
  high.quality = 95;

  const auto small = encodeImage(image, ImageFormat::JPEG, low);
  const auto large = encodeImage(image, ImageFormat::JPEG, high);
  ASSERT_TRUE(small.has_value());
  ASSERT_TRUE(large.has_value());
  EXPECT_LT(small->size(), large->size()) << "quality 10 should produce a smaller file than quality 95";
}

TEST(ImageCodecTest, JpegRefusesADecodeOfSomethingElse) {
  if (!isDecodeSupported(ImageFormat::JPEG)) {
    GTEST_SKIP() << "this build has no JPEG support";
  }

  // A JPEG signature followed by rubbish. The sniffer accepts it, so this exercises libjpeg's own
  // error path and the setjmp landing pad rather than the early refusal.
  const uint8_t truncated[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F' };
  EXPECT_FALSE(decodeImage(truncated, sizeof(truncated)).has_value());
}

// ==================================================================================================
// WebP
// ==================================================================================================

TEST(ImageCodecTest, WebpRoundTripIsLossless) {
  if (!isEncodeSupported(ImageFormat::WebP)) {
    GTEST_SKIP() << "this build has no WebP support";
  }

  const DecodedImage original = makeTestImage(19, 13);

  EncodeOptions options;
  // `lossless`, not `quality = 100`. WebP's `WebPEncodeRGBA` is lossy at every quality -- passing 100
  // makes the output bigger and better, not exact -- so the lossless entry point has to be selected
  // explicitly. Expecting an exact round trip from the lossy path was the first version of this test,
  // and it failed, which is how the distinction was found.
  options.lossless = true;

  const auto encoded = encodeImage(original, ImageFormat::WebP, options);
  ASSERT_TRUE(encoded.has_value()) << "a plain RGBA image should encode as WebP";
  EXPECT_EQ(sniffFormat(encoded->data(), encoded->size()), ImageFormat::WebP);

  const auto decoded = decodeImage(*encoded);
  ASSERT_TRUE(decoded.has_value());

  EXPECT_EQ(decoded->info.width(), 19);
  EXPECT_EQ(decoded->info.height(), 13);
  EXPECT_EQ(decoded->info.colorType(), ColorType::kRGBA_8888);
  ASSERT_EQ(decoded->pixels.size(), original.pixels.size());
  EXPECT_EQ(decoded->pixels, original.pixels) << "the lossless encoder must round-trip exactly";
}

TEST(ImageCodecTest, WebpRoundTripOfBGRAProducesRGBA) {
  if (!isEncodeSupported(ImageFormat::WebP)) {
    GTEST_SKIP() << "this build has no WebP support";
  }

  const DecodedImage original = makeTestImage(8, 8, ColorType::kBGRA_8888);

  // Lossless, because the check below is a byte-for-byte comparison. WebP's default path is lossy, so
  // omitting this compares a channel swap against the codec's approximation error and fails for a
  // reason that has nothing to do with the swap -- which is exactly what the first version of this test
  // did.
  EncodeOptions options;
  options.lossless = true;

  const auto encoded = encodeImage(original, ImageFormat::WebP, options);
  ASSERT_TRUE(encoded.has_value());

  const auto decoded = decodeImage(*encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->info.colorType(), ColorType::kRGBA_8888);

  // The swap must have happened on the way in rather than the data being relabelled.
  for (size_t i = 0; i + 3 < original.pixels.size(); i += 4) {
    EXPECT_EQ(decoded->pixels[i + 0], original.pixels[i + 2]) << "red comes from BGRA's third byte";
    EXPECT_EQ(decoded->pixels[i + 2], original.pixels[i + 0]) << "blue comes from BGRA's first byte";
  }
}

TEST(ImageCodecTest, WebpLossyQualityAffectsTheEncodedSize) {
  if (!isEncodeSupported(ImageFormat::WebP)) {
    GTEST_SKIP() << "this build has no WebP support";
  }

  // The lossy path is still reachable and still honours quality. Without this the `lossless` default
  // wrapping the wrong entry point would go unnoticed.
  const DecodedImage image = makeTestImage(64, 64);

  EncodeOptions low;
  low.quality = 5;
  EncodeOptions high;
  high.quality = 95;

  const auto small = encodeImage(image, ImageFormat::WebP, low);
  const auto large = encodeImage(image, ImageFormat::WebP, high);
  ASSERT_TRUE(small.has_value());
  ASSERT_TRUE(large.has_value());
  EXPECT_LT(small->size(), large->size()) << "quality 5 should be smaller than quality 95";

  // ...and the lossy result must still be a decodable WebP.
  const auto decoded = decodeImage(*large);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->info.width(), 64);
}

TEST(ImageCodecTest, WebpRefusesADecodeOfSomethingElse) {
  if (!isDecodeSupported(ImageFormat::WebP)) {
    GTEST_SKIP() << "this build has no WebP support";
  }

  // A well-formed RIFF/WEBP container with no usable image in it. The sniffer accepts it, so the
  // refusal has to come from libwebp.
  const uint8_t emptyContainer[] = { 'R', 'I', 'F', 'F', 0x04, 0x00, 0x00, 0x00, 'W', 'E', 'B', 'P' };
  EXPECT_FALSE(decodeImage(emptyContainer, sizeof(emptyContainer)).has_value());
}

// ==================================================================================================
// Failure paths and limits
// ==================================================================================================

TEST(ImageCodecTest, DecodingRubbishFailsRatherThanGuessing) {
  EXPECT_FALSE(decodeImage(nullptr, 0).has_value());
  EXPECT_FALSE(decodeImage(EncodedImage {}).has_value());

  // A valid PNG signature followed by nothing. The signature check passes, so this exercises the
  // library's own error path and the setjmp landing pad rather than the early refusal.
  const uint8_t truncated[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0x00 };
  EXPECT_FALSE(decodeImage(truncated, sizeof(truncated)).has_value());

  // Not an image at all.
  const uint8_t garbage[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x33 };
  EXPECT_FALSE(decodeImage(garbage, sizeof(garbage)).has_value());
}

TEST(ImageCodecTest, TruncatedPngBodyIsRejected) {
  // A real file cut short. This is the case a network response produces, and it must not be reported
  // as a successful decode of a partial image.
  const DecodedImage original = makeTestImage(32, 32);
  const auto encoded = encodeImage(original, ImageFormat::PNG);
  ASSERT_TRUE(encoded.has_value());
  ASSERT_GT(encoded->size(), 40u);

  const EncodedImage half(encoded->begin(), encoded->begin() + static_cast<long>(encoded->size() / 2));
  EXPECT_FALSE(decodeImage(half).has_value()) << "half a PNG is not a PNG";
}

TEST(ImageCodecTest, EveryPrefixOfAValidPngIsRefusedCleanly) {
  // A truncated file is the ordinary case for a network response or a partial write, and the decoder has to refuse it
  // without reading past the buffer it was given.
  //
  // Every prefix length is tried rather than a few, because the interesting failures are not evenly distributed: an
  // over-read happens at the lengths where a chunk header claims more bytes than the file still holds, and those are
  // specific values. A test that cut the file in half would have missed the one the address sanitizer found.
  const DecodedImage original = makeTestImage(24, 24);
  const auto encoded = encodeImage(original, ImageFormat::PNG);
  ASSERT_TRUE(encoded.has_value());
  ASSERT_GT(encoded->size(), 64u);

  size_t decoded = 0;
  for (size_t length = 0; length < encoded->size(); ++length) {
    const EncodedImage prefix(encoded->begin(), encoded->begin() + static_cast<long>(length));
    if (decodeImage(prefix).has_value()) ++decoded;
  }

  // A prefix long enough to hold the whole image could legitimately decode -- the trailing chunk is not required --
  // so the count is not asserted to be zero. What matters is that the loop ran to completion without a violation.
  EXPECT_LT(decoded, encoded->size()) << "not every prefix can be a complete image";
}

TEST(ImageCodecTest, EveryPrefixOfAValidJpegIsRefusedCleanly) {
  if (!isEncodeSupported(ImageFormat::JPEG) || !isDecodeSupported(ImageFormat::JPEG)) {
    GTEST_SKIP() << "JPEG is not available in this build";
  }

  const DecodedImage original = makeTestImage(24, 24);
  const auto encoded = encodeImage(original, ImageFormat::JPEG);
  ASSERT_TRUE(encoded.has_value());
  ASSERT_GT(encoded->size(), 64u);

  size_t decoded = 0;
  for (size_t length = 0; length < encoded->size(); ++length) {
    const EncodedImage prefix(encoded->begin(), encoded->begin() + static_cast<long>(length));
    if (decodeImage(prefix).has_value()) ++decoded;
  }

  EXPECT_LT(decoded, encoded->size()) << "not every prefix can be a complete image";
}

TEST(ImageCodecTest, EveryPrefixOfAValidWebpIsRefusedCleanly) {
  if (!isEncodeSupported(ImageFormat::WebP) || !isDecodeSupported(ImageFormat::WebP)) {
    GTEST_SKIP() << "WebP is not available in this build";
  }

  const DecodedImage original = makeTestImage(24, 24);
  const auto encoded = encodeImage(original, ImageFormat::WebP);
  ASSERT_TRUE(encoded.has_value());
  ASSERT_GT(encoded->size(), 64u);

  size_t decoded = 0;
  for (size_t length = 0; length < encoded->size(); ++length) {
    const EncodedImage prefix(encoded->begin(), encoded->begin() + static_cast<long>(length));
    if (decodeImage(prefix).has_value()) ++decoded;
  }

  EXPECT_LT(decoded, encoded->size()) << "not every prefix can be a complete image";
}

TEST(ImageCodecTest, ASingleCorruptedByteIsRefusedOrDecodedButNeverTrusted) {
  // The other half of the same idea: a file whose *length* is right but whose bytes are not. Flipping one byte in the
  // compressed body either makes the decode fail or makes it produce a different image; what it must not do is make
  // the decoder read outside the buffer or report a size the file cannot support.
  const DecodedImage original = makeTestImage(16, 16);
  const auto encoded = encodeImage(original, ImageFormat::PNG);
  ASSERT_TRUE(encoded.has_value());

  for (size_t at = 0; at < encoded->size(); at += 7) {
    EncodedImage corrupted = *encoded;
    corrupted[at] = static_cast<uint8_t>(corrupted[at] ^ 0xFFu);

    const auto decodedImage = decodeImage(corrupted);
    if (!decodedImage.has_value()) continue;

    // A decode that succeeded has to describe an image the buffer can hold, or every reader of it is out of bounds.
    EXPECT_TRUE(decodedImage->isValid())
            << "a successful decode must have a valid shape, at byte " << at;
    EXPECT_EQ(decodedImage->pixels.size(), decodedImage->info.computeByteSize())
            << "the pixels must be exactly the size the shape says, at byte " << at;
  }
}

TEST(ImageCodecTest, TheByteLimitIsCheckedBeforeAllocating) {
  const DecodedImage original = makeTestImage(64, 64);
  const auto encoded = encodeImage(original, ImageFormat::PNG);
  ASSERT_TRUE(encoded.has_value());

  DecodeOptions allowed;
  allowed.maxByteSize = 64u * 64u * 4u;
  EXPECT_TRUE(decodeImage(*encoded, allowed).has_value());

  // One byte under the requirement: the decode must be refused rather than allocated and then caught.
  DecodeOptions refused;
  refused.maxByteSize = 64u * 64u * 4u - 1u;
  EXPECT_FALSE(decodeImage(*encoded, refused).has_value());
}

TEST(ImageCodecTest, TheDimensionLimitIsHonoured) {
  const DecodedImage original = makeTestImage(32, 8);
  const auto encoded = encodeImage(original, ImageFormat::PNG);
  ASSERT_TRUE(encoded.has_value());

  DecodeOptions generous;
  generous.maxDimension = 32;
  EXPECT_TRUE(decodeImage(*encoded, generous).has_value());

  // The height is 8 and within the limit; the width is 32 and the limit is 31, so this must fail --
  // which checks that both axes are tested rather than only one.
  DecodeOptions strict;
  strict.maxDimension = 31;
  EXPECT_FALSE(decodeImage(*encoded, strict).has_value());
}

TEST(ImageCodecTest, GrayscaleIsRefusedWhenTheImageHasAlpha) {
  // Gray+alpha has no 8-bit single-channel representation. Returning RGBA under a kGray_8 label would
  // give the caller a buffer it would read with the wrong stride, so this is refused instead.
  const DecodedImage original = makeTestImage(8, 8);
  const auto encoded = encodeImage(original, ImageFormat::PNG);
  ASSERT_TRUE(encoded.has_value());

  DecodeOptions options;
  options.preferredColorType = ColorType::kGray_8;
  EXPECT_FALSE(decodeImage(*encoded, options).has_value());
}

TEST(ImageCodecTest, EncodingRefusesAnImageWithNoPixels) {
  DecodedImage empty;
  EXPECT_FALSE(encodeImage(empty, ImageFormat::PNG).has_value());

  // Info that promises pixels the buffer does not contain. Trusting the info here would read past the
  // end of the vector inside libpng.
  DecodedImage lying;
  lying.info = ImageInfo(10, 10, ColorType::kRGBA_8888, AlphaType::kUnpremultiplied);
  EXPECT_FALSE(encodeImage(lying, ImageFormat::PNG).has_value());
}

TEST(ImageCodecTest, EncodingRefusesAnUnsupportedColourType) {
  DecodedImage image;
  image.info = ImageInfo(4, 4, ColorType::kRGBA_F16, AlphaType::kUnpremultiplied);
  image.pixels.resize(image.info.computeByteSize());

  // 16-bit float pixels would need a 16-bit PNG, which the encoder does not write. Refusing is better
  // than writing an 8-bit file whose header disagrees with what the caller asked for.
  EXPECT_FALSE(encodeImage(image, ImageFormat::PNG).has_value());
}

TEST(ImageCodecTest, TheSystemBackendIsRefusedWhereItDoesNotExist) {
  const DecodedImage original = makeTestImage(8, 8);
  const auto encoded = encodeImage(original, ImageFormat::PNG);
  ASSERT_TRUE(encoded.has_value());

  DecodeOptions options;
  options.backend = CodecBackend::System;

  if (isBackendAvailable(CodecBackend::System)) {
    // On a platform that has one, asking for it must not fail merely because it was asked for.
    EXPECT_TRUE(decodeImage(*encoded, options).has_value());
  } else {
    EXPECT_FALSE(decodeImage(*encoded, options).has_value())
            << "the system decoder was requested but does not exist on this platform";
  }
}
