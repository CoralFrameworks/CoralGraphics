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
 * @file test_Surface.cpp
 *
 * Surfaces, images, and the decode/encode path joining them.
 *
 * The three properties worth pinning are the ones the types exist for. An `Image` must not change -- not when the
 * surface it came from is drawn into again, and not when the bitmap it was made from is redrawn. A `Surface` must
 * not hand out two canvases at once, because a caller in that state is confused about its own frame and silently
 * sharing the canvas would hide it. And the codec round trip must be lossless where the format is, because a
 * difference there means a conversion happened that should not have.
 */

#include <graphics/Image.h>
#include <graphics/Surface.h>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>

using namespace graphics;

namespace {

constexpr uint32_t argb(const uint32_t a, const uint32_t r, const uint32_t g, const uint32_t b) {
  return (a << 24) | (r << 16) | (g << 8) | b;
}

/** A paint that fills with opaque red, so coverage shows directly in the colour channels. */
Paint redPaint() {
  Paint paint;
  paint.color = Color::Red();
  paint.style = Paint::Style::kFill;
  return paint;
}

} // namespace anonymous

// ==================================================================================================
// Surface
// ==================================================================================================

TEST(SurfaceTest, MakesARasterSurfaceOfTheRequestedSize) {
  const auto surface = Surface::makeRaster(8, 4);
  ASSERT_NE(surface, nullptr);

  EXPECT_EQ(surface->width(), 8);
  EXPECT_EQ(surface->height(), 4);
  EXPECT_EQ(surface->info().colorType(), ColorType::kRGBA_8888);
  EXPECT_EQ(surface->info().alphaType(), AlphaType::kPremultiplied);

  // A fresh surface is transparent black, and the bounds run from the origin to its size.
  const Pixmap pixels = surface->peekPixels();
  EXPECT_EQ(pixels.getPixelARGB32(0, 0), 0u);
  EXPECT_EQ(pixels.getPixelARGB32(7, 3), 0u);
  EXPECT_FLOAT_EQ(surface->bounds().getRight(), 8.0f);
  EXPECT_FLOAT_EQ(surface->bounds().getBottom(), 4.0f);
}

TEST(SurfaceTest, RefusesASizeThatCannotBeAllocated) {
  EXPECT_EQ(Surface::makeRaster(0, 10), nullptr);
  EXPECT_EQ(Surface::makeRaster(10, 0), nullptr);
  EXPECT_EQ(Surface::makeRaster(-1, 10), nullptr);
}

TEST(SurfaceTest, RefusesASecondCanvasWithoutASnapshot) {
  const auto surface = Surface::makeRaster(8, 8);
  ASSERT_NE(surface, nullptr);

  EXPECT_TRUE(surface->isDrawable());
  const auto first = surface->getCanvas();
  ASSERT_NE(first, nullptr);
  EXPECT_FALSE(surface->isDrawable()) << "a canvas is outstanding";

  // Asked again without a snapshot. Refused rather than re-returned: a caller in this state is confused about its
  // own frame, and silently sharing the canvas would let it draw half of one frame and half of the next.
  EXPECT_EQ(surface->getCanvas(), nullptr);

  // A snapshot ends the frame, and the surface becomes drawable again.
  const Image snapshot = surface->snapshot();
  EXPECT_FALSE(snapshot.isEmpty());
  EXPECT_TRUE(surface->isDrawable());
  EXPECT_NE(surface->getCanvas(), nullptr);
}

TEST(SurfaceTest, SnapshotIsNotAffectedByLaterDrawing) {
  // The property that makes `Image` worth having. The surface is drawn into again after the snapshot, and the image
  // must still show the first frame -- an image that changed underneath its holder would be worse than useless.
  const auto surface = Surface::makeRaster(8, 8);
  ASSERT_NE(surface, nullptr);

  {
    const auto canvas = surface->getCanvas();
    ASSERT_NE(canvas, nullptr);
    canvas->drawRect(Rect::MakeLTRB(0.0f, 0.0f, 8.0f, 8.0f), redPaint());
  }

  const Image first = surface->snapshot();
  ASSERT_FALSE(first.isEmpty());
  EXPECT_EQ(first.makePixmap().getPixelARGB32(4, 4) & 0x00FFFFFFu, 0x00FF0000u) << "the snapshot is red";

  // Draw white over everything and take a second snapshot.
  {
    const auto canvas = surface->getCanvas();
    ASSERT_NE(canvas, nullptr);
    canvas->clear(Color(1.0f, 1.0f, 1.0f, 1.0f));
  }
  const Image second = surface->snapshot();
  ASSERT_FALSE(second.isEmpty());

  // The first image still reads red. This is the assertion that fails if `snapshot` ever hands back a view.
  EXPECT_EQ(first.makePixmap().getPixelARGB32(4, 4) & 0x00FFFFFFu, 0x00FF0000u)
          << "the first snapshot must not have changed";
  EXPECT_EQ(second.makePixmap().getPixelARGB32(4, 4) & 0x00FFFFFFu, 0x00FFFFFFu) << "the second is white";
  EXPECT_FALSE(first == second);
}

TEST(SurfaceTest, DrawingThroughTheCanvasReachesThePixels) {
  const auto surface = Surface::makeRaster(16, 16);
  ASSERT_NE(surface, nullptr);

  {
    const auto canvas = surface->getCanvas();
    ASSERT_NE(canvas, nullptr);
    canvas->drawRect(Rect::MakeLTRB(4.0f, 4.0f, 12.0f, 12.0f), redPaint());
  }

  // Seen through the surface's own view, without a snapshot.
  const Pixmap pixels = surface->peekPixels();
  EXPECT_EQ((pixels.getPixelARGB32(8, 8) >> 16) & 0xFFu, 255u) << "inside the rectangle";
  EXPECT_EQ((pixels.getPixelARGB32(1, 1) >> 16) & 0xFFu, 0u) << "outside it";

  // And through a snapshot, which holds the same content.
  const Image image = surface->snapshot();
  EXPECT_EQ((image.makePixmap().getPixelARGB32(8, 8) >> 16) & 0xFFu, 255u);
}

TEST(SurfaceTest, DetachMovesThePixelsOut) {
  auto surface = Surface::makeRaster(4, 4);
  ASSERT_NE(surface, nullptr);

  {
    const auto canvas = surface->getCanvas();
    ASSERT_NE(canvas, nullptr);
    canvas->clear(Color(0.0f, 1.0f, 0.0f, 1.0f));
  }

  const Bitmap detached = surface->detachBitmap();
  EXPECT_EQ(detached.width(), 4);
  EXPECT_EQ((detached.makePixmap().getPixelARGB32(0, 0) >> 8) & 0xFFu, 255u) << "the green survives the move";

  // The surface is left empty rather than holding a moved-from bitmap it cannot use.
  EXPECT_EQ(surface->width(), 0);
  EXPECT_FALSE(surface->info().isValid());
}

TEST(SurfaceTest, SnapshotWorksWithoutEverTakingACanvas) {
  // A caller's first frame should not fail because it had nothing to draw. The surface holds transparent black and
  // that is a legitimate picture.
  const auto surface = Surface::makeRaster(4, 4);
  ASSERT_NE(surface, nullptr);

  const Image image = surface->snapshot();
  EXPECT_FALSE(image.isEmpty());
  EXPECT_EQ(image.width(), 4);
  EXPECT_EQ(image.makePixmap().getPixelARGB32(2, 2), 0u);
}

TEST(SurfaceTest, MakesASurfaceFromAnImage) {
  const auto source = Surface::makeRaster(4, 4);
  ASSERT_NE(source, nullptr);
  {
    const auto canvas = source->getCanvas();
    ASSERT_NE(canvas, nullptr);
    canvas->clear(Color(0.0f, 0.0f, 1.0f, 1.0f));
  }
  const Image image = source->snapshot();

  const auto copy = Surface::makeRaster(image);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(copy->width(), 4);

  // The pixels came across.
  EXPECT_EQ(copy->peekPixels().getPixelARGB32(0, 0) & 0xFFu, 255u) << "blue";

  // And it is a copy, not a reference: drawing into the new surface must not touch the image.
  {
    const auto canvas = copy->getCanvas();
    ASSERT_NE(canvas, nullptr);
    canvas->clear(Color(1.0f, 1.0f, 1.0f, 1.0f));
  }
  EXPECT_EQ(image.makePixmap().getPixelARGB32(0, 0) & 0xFFu, 255u)
          << "the image the surface was made from must be unaffected";
}

// ==================================================================================================
// Image
// ==================================================================================================

TEST(ImageTest, EmptyByDefault) {
  const Image image;
  EXPECT_TRUE(image.isEmpty());
  EXPECT_EQ(image.width(), 0);
  EXPECT_EQ(image.height(), 0);
  EXPECT_EQ(image.makePixmap().pixels(), nullptr);
}

TEST(ImageTest, CopiesTheBitmapItIsMadeFrom) {
  Bitmap bitmap = Bitmap::allocate(ImageInfo(4, 4, ColorType::kRGBA_8888, AlphaType::kUnpremultiplied));
  bitmap.lockPixels().setPixelARGB32(0, 0, argb(255, 10, 20, 30));

  const Image image = Image::fromBitmap(bitmap);
  ASSERT_FALSE(image.isEmpty());
  EXPECT_EQ(image.makePixmap().getPixelARGB32(0, 0), argb(255, 10, 20, 30));

  // Redrawing the bitmap must not change the image. This is what "a copy" has to mean -- sharing the buffer would
  // make the image a window onto something the caller still owns.
  bitmap.lockPixels().setPixelARGB32(0, 0, argb(255, 99, 99, 99));
  EXPECT_EQ(image.makePixmap().getPixelARGB32(0, 0), argb(255, 10, 20, 30))
          << "the image must not follow the bitmap it was made from";
}

TEST(ImageTest, MakeBitmapReturnsAnIndependentCopy) {
  const auto surface = Surface::makeRaster(4, 4);
  ASSERT_NE(surface, nullptr);
  {
    const auto canvas = surface->getCanvas();
    ASSERT_NE(canvas, nullptr);
    canvas->clear(Color::Red());
  }
  const Image image = surface->snapshot();

  Bitmap copy = image.makeBitmap();
  ASSERT_FALSE(copy.isEmpty());
  EXPECT_TRUE(copy.isWritable()) << "the copy exists to be drawn into";

  // Drawing into the copy leaves the image alone.
  copy.lockPixels().setPixelARGB32(0, 0, argb(255, 0, 0, 255));
  EXPECT_EQ((image.makePixmap().getPixelARGB32(0, 0) >> 16) & 0xFFu, 255u) << "the image is still red";
}

TEST(ImageTest, EqualityComparesContents) {
  Bitmap a = Bitmap::allocate(ImageInfo(2, 2, ColorType::kRGBA_8888, AlphaType::kUnpremultiplied));
  a.lockPixels().setPixelARGB32(0, 0, argb(255, 10, 0, 0));

  const Image first = Image::fromBitmap(a);
  const Image second = Image::fromBitmap(a);  // a separate copy of the same pixels

  EXPECT_TRUE(first == second) << "two images with the same pixels are the same picture";
  EXPECT_TRUE(first.equals(second));

  a.lockPixels().setPixelARGB32(0, 0, argb(255, 11, 0, 0));
  const Image different = Image::fromBitmap(a);
  EXPECT_TRUE(first != different);

  // Two empty images are equal, and an empty one differs from a non-empty one.
  EXPECT_TRUE(Image {} == Image {});
  EXPECT_TRUE(first != Image {});
}

TEST(ImageTest, FromDataRefusesABufferTooSmallForItsGeometry) {
  const ImageInfo info(4, 4, ColorType::kRGBA_8888, AlphaType::kUnpremultiplied);  // needs 64 bytes

  // Exactly enough is accepted.
  const Image ok = Image::fromData(info, Data::makeUninitialised(64));
  EXPECT_FALSE(ok.isEmpty());
  EXPECT_EQ(ok.width(), 4);

  // A short buffer is refused rather than becoming an image that reads past its own end.
  EXPECT_TRUE(Image::fromData(info, Data::makeUninitialised(16)).isEmpty());

  // A stride narrower than a row would make two rows overlap.
  EXPECT_TRUE(Image::fromData(info, Data::makeUninitialised(128), 8).isEmpty());
  EXPECT_FALSE(Image::fromData(info, Data::makeUninitialised(128), 24).isEmpty());
}

TEST(ImageTest, FromDataBorrowsRatherThanCopies) {
  const ImageInfo info(2, 2, ColorType::kRGBA_8888, AlphaType::kUnpremultiplied);

  // The `Data` handed in is the one the image now holds, which is what makes wrapping a decoder's output free --
  // the bytes are not copied a second time.
  const Data buffer = Data::makeUninitialised(16);
  const Image image = Image::fromData(info, buffer);
  ASSERT_FALSE(image.isEmpty());
  EXPECT_EQ(image.data().bytes(), buffer.bytes()) << "the same bytes, not a copy of them";
}

TEST(ImageTest, BoundsRunFromTheOrigin) {
  const auto surface = Surface::makeRaster(6, 3);
  ASSERT_NE(surface, nullptr);
  const Image image = surface->snapshot();

  const Rect bounds = image.bounds();
  EXPECT_FLOAT_EQ(bounds.getLeft(), 0.0f);
  EXPECT_FLOAT_EQ(bounds.getTop(), 0.0f);
  EXPECT_FLOAT_EQ(bounds.getRight(), 6.0f);
  EXPECT_FLOAT_EQ(bounds.getBottom(), 3.0f);
}

// ==================================================================================================
// Encoding and decoding
// ==================================================================================================

TEST(ImageTest, RoundTripsThroughPng) {
  const auto surface = Surface::makeRaster(8, 8);
  ASSERT_NE(surface, nullptr);
  {
    const auto canvas = surface->getCanvas();
    ASSERT_NE(canvas, nullptr);
    canvas->clear(Color::Red());
    canvas->drawRect(Rect::MakeLTRB(0.0f, 0.0f, 4.0f, 4.0f), Paint {});
  }
  const Image original = surface->snapshot();
  ASSERT_FALSE(original.isEmpty());

  std::string error;
  const Data encoded = original.encode(ImageFormat::PNG, 100, &error);
  ASSERT_FALSE(encoded.isEmpty()) << error;
  EXPECT_GT(encoded.size(), 8u);

  // The bytes are a PNG, by content rather than by assertion.
  EXPECT_EQ(sniffFormat(encoded.bytes(), encoded.size()), ImageFormat::PNG);

  const Image decoded = Image::decode(encoded, &error);
  ASSERT_FALSE(decoded.isEmpty()) << error;
  EXPECT_EQ(decoded.width(), 8);
  EXPECT_EQ(decoded.height(), 8);

  // PNG is lossless, so the pixels come back exactly. Any difference at all means a conversion happened that should
  // not have -- a colour-space transform, a premultiply, or a channel reorder.
  const Pixmap before = original.makePixmap();
  const Pixmap after = decoded.makePixmap();
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      EXPECT_EQ(before.getPixelARGB32(x, y), after.getPixelARGB32(x, y)) << "at (" << x << "," << y << ")";
    }
  }
}

TEST(ImageTest, DecodeRefusesRubbish) {
  const uint8_t rubbish[32] = { 'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'a', 'g', 'e', 0, 1, 2, 3 };
  std::string error;

  const Image image = Image::decode(Data::makeCopy(rubbish, sizeof(rubbish)), &error);
  EXPECT_TRUE(image.isEmpty());
  EXPECT_FALSE(error.empty()) << "a failure must say something about why";

  // And an empty buffer is refused before anything is attempted.
  error.clear();
  EXPECT_TRUE(Image::decode(Data {}, &error).isEmpty());
  EXPECT_FALSE(error.empty());
}

TEST(ImageTest, EncodeRefusesAnEmptyImage) {
  std::string error;
  const Data encoded = Image {}.encode(ImageFormat::PNG, 100, &error);
  EXPECT_TRUE(encoded.isEmpty());
  EXPECT_FALSE(error.empty());
}

TEST(ImageTest, EncodesToEachFormatTheBuildSupports) {
  const auto surface = Surface::makeRaster(16, 16);
  ASSERT_NE(surface, nullptr);
  {
    const auto canvas = surface->getCanvas();
    ASSERT_NE(canvas, nullptr);
    canvas->clear(Color(0.5f, 0.25f, 0.75f, 1.0f));
  }
  const Image image = surface->snapshot();

  for (const ImageFormat format: { ImageFormat::PNG, ImageFormat::JPEG, ImageFormat::WebP }) {
    if (!isEncodeSupported(format)) continue;

    std::string error;
    const Data encoded = image.encode(format, 90, &error);
    ASSERT_FALSE(encoded.isEmpty()) << toString(format) << ": " << error;

    // The encoder produced what it said it would, checked by sniffing rather than by trusting the format argument:
    // a mismatch there would mean the dispatch picked the wrong codec.
    EXPECT_EQ(sniffFormat(encoded.bytes(), encoded.size()), format) << toString(format);

    // And it decodes back to the same size.
    const Image decoded = Image::decode(encoded, &error);
    ASSERT_FALSE(decoded.isEmpty()) << toString(format) << ": " << error;
    EXPECT_EQ(decoded.width(), 16) << toString(format);
    EXPECT_EQ(decoded.height(), 16) << toString(format);
  }
}
