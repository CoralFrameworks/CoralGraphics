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
 * @file test_Picture.cpp
 *
 * Recording drawing calls and playing them back.
 *
 * ## The property that matters
 *
 * A picture is only worth having if a replay draws *the same pixels* as the original. Everything else about the type
 * -- the cull rectangle, the operation count, the identifier -- is bookkeeping that a caller can do without if the
 * replay is faithful, and none of it is worth anything if it is not.
 *
 * So most of these tests draw a scene twice, once directly and once through a recording replayed onto a second
 * surface, and compare the pixels. That comparison catches every way the pair can disagree at once: a call the
 * recorder forgot, an argument captured when it should have been copied, a state call that did not record. A test that
 * asserted the operation count instead would pass with every one of those bugs present.
 *
 * The second thing worth checking is what a recording *cannot* do, because a silent wrong answer is worse than a
 * refusal: replaying onto a recording canvas would nest without end, and is refused.
 */

#include <graphics/Canvas.h>
#include <graphics/Picture.h>
#include <graphics/PictureRecorder.h>
#include <graphics/Surface.h>

#include <gtest/gtest.h>

#include <memory>

using namespace graphics;

namespace {

constexpr int kWidth = 32;
constexpr int kHeight = 32;

/** @return a surface cleared to transparent, so an untouched pixel is distinguishable from a drawn one. */
std::shared_ptr<Surface> makeSurface() {
  auto surface = Surface::makeRaster(kWidth, kHeight);
  if (surface == nullptr) return nullptr;

  // Cleared through a canvas and then *snapshotted*, which is what ends the frame. A surface refuses a second canvas
  // while one is outstanding -- deliberately, so a caller cannot draw half a frame and half the next by accident --
  // so a helper that took a canvas and kept it would leave every later `getCanvas` returning null.
  const auto canvas = surface->getCanvas();
  if (canvas != nullptr) canvas->clear(Color { 0.0f, 0.0f, 0.0f, 0.0f });
  (void) surface->snapshot();
  return surface;
}

/**
 * @return the surface's pixels, for comparing two renders.
 *
 * Row by row rather than as one run: a pixmap's rows may be padded to an alignment, so the buffer is longer than
 * `width * 4 * height` and comparing it whole would compare the padding as well -- which is uninitialised, and would
 * make the comparison fail for a reason that has nothing to do with what was drawn.
 */
std::vector<uint8_t> pixelsOf(const Surface& surface) {
  const Pixmap pixmap = surface.peekPixels();
  if (pixmap.isEmpty()) return {};

  std::vector<uint8_t> pixels;
  pixels.reserve(static_cast<size_t>(pixmap.width()) * static_cast<size_t>(pixmap.height()) * 4u);
  for (int y = 0; y < pixmap.height(); ++y) {
    const auto* row = static_cast<const uint8_t*>(pixmap.addr(0, y));
    if (row == nullptr) return {};
    pixels.insert(pixels.end(), row, row + static_cast<size_t>(pixmap.width()) * 4u);
  }
  return pixels;
}

/**
 * Draws a scene that uses as many recorded operations as possible.
 *
 * Deliberately not a single rectangle: a scene with a save, a transform, a clip, a path and a colour exercises the
 * state calls as well as the drawing ones, and those are what a recorder is most likely to forget -- a forgotten
 * `concat` produces a picture whose second shape is in the wrong place, which a single-shape scene would not reveal.
 */
void drawScene(Canvas* canvas) {
  canvas->save();
  canvas->concat(Matrix::MakeTranslation(Vector3 { 4.0f, 4.0f, 0.0f }));
  canvas->clipRect(Rect::MakeLTRB(0.0f, 0.0f, 24.0f, 24.0f));

  Paint paint;
  paint.color = Color { 1.0f, 0.0f, 0.0f, 1.0f };
  canvas->drawRect(Rect::MakeLTRB(0.0f, 0.0f, 10.0f, 10.0f), paint);

  paint.color = Color { 0.0f, 1.0f, 0.0f, 1.0f };
  canvas->drawRect(Rect::MakeLTRB(12.0f, 12.0f, 20.0f, 20.0f), paint);

  Path triangle;
  triangle.moveTo(Point { 2.0f, 22.0f });
  triangle.lineTo(Point { 10.0f, 22.0f });
  triangle.lineTo(Point { 6.0f, 14.0f });
  triangle.close();
  paint.color = Color { 0.0f, 0.0f, 1.0f, 1.0f };
  canvas->drawPath(triangle, paint);

  canvas->restore();
}

} // namespace anonymous

// ==================================================================================================
// The property: a replay draws what was drawn
// ==================================================================================================

TEST(PictureTest, AReplayDrawsTheSamePixelsAsTheOriginal) {
  // The direct render.
  auto direct = makeSurface();
  ASSERT_NE(direct, nullptr);
  drawScene(direct->getCanvas().get());
  const std::vector<uint8_t> expected = pixelsOf(*direct);
  ASSERT_FALSE(expected.empty());

  // The same scene recorded and replayed onto a second surface.
  PictureRecorder recorder;
  Canvas* recording = recorder.beginRecording(Rect::MakeLTRB(0.0f, 0.0f, kWidth, kHeight));
  ASSERT_NE(recording, nullptr);
  drawScene(recording);

  auto picture = recorder.finishRecording();
  ASSERT_NE(picture, nullptr);
  EXPECT_GT(picture->operationCount(), 0) << "the scene recorded something";

  auto replayed = makeSurface();
  ASSERT_NE(replayed, nullptr);
  picture->playback(replayed->getCanvas().get());

  EXPECT_EQ(pixelsOf(*replayed), expected)
          << "a replay must produce the same pixels, not merely a picture that looks similar";
}

TEST(PictureTest, AReplayIsRepeatable) {
  // The same picture drawn twice must give the same result both times. A recording that consumed its calls, or that
  // let a replay's transforms accumulate into it, would pass the first comparison and fail the second.
  PictureRecorder recorder;
  Canvas* recording = recorder.beginRecording(Rect::MakeLTRB(0.0f, 0.0f, kWidth, kHeight));
  drawScene(recording);
  auto picture = recorder.finishRecording();
  ASSERT_NE(picture, nullptr);

  auto first = makeSurface();
  auto second = makeSurface();
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  picture->playback(first->getCanvas().get());
  picture->playback(second->getCanvas().get());
  EXPECT_EQ(pixelsOf(*first), pixelsOf(*second)) << "a picture is reusable";
}

TEST(PictureTest, TheCanvasStateAtReplayAppliesToTheWholeRecording) {
  // A recording is drawn onto whatever state the canvas is in, as the calls would have been. So a translated canvas
  // moves the whole picture, which is what makes a picture composable -- and is the difference between replaying the
  // calls and blitting an image of them.
  PictureRecorder recorder;
  Canvas* recording = recorder.beginRecording(Rect::MakeLTRB(0.0f, 0.0f, 8.0f, 8.0f));
  Paint paint;
  paint.color = Color { 1.0f, 1.0f, 1.0f, 1.0f };
  recording->drawRect(Rect::MakeLTRB(0.0f, 0.0f, 4.0f, 4.0f), paint);
  auto picture = recorder.finishRecording();
  ASSERT_NE(picture, nullptr);

  auto atOrigin = makeSurface();
  auto translated = makeSurface();
  ASSERT_NE(atOrigin, nullptr);
  ASSERT_NE(translated, nullptr);

  picture->playback(atOrigin->getCanvas().get());
  translated->getCanvas()->concat(Matrix::MakeTranslation(Vector3 { 8.0f, 0.0f, 0.0f }));
  picture->playback(translated->getCanvas().get());

  EXPECT_NE(pixelsOf(*atOrigin), pixelsOf(*translated))
          << "the canvas's transform must apply to the replay, or a picture could not be placed";
}

// ==================================================================================================
// What a recording reports
// ==================================================================================================

TEST(PictureTest, TheCullRectIsTheBoundTheRecordingStartedWith) {
  PictureRecorder recorder;
  ASSERT_NE(recorder.beginRecording(Rect::MakeLTRB(1.0f, 2.0f, 30.0f, 40.0f)), nullptr);

  auto picture = recorder.finishRecording();
  ASSERT_NE(picture, nullptr);

  EXPECT_FLOAT_EQ(picture->cullRect().getLeft(), 1.0f);
  EXPECT_FLOAT_EQ(picture->cullRect().getTop(), 2.0f);
  EXPECT_FLOAT_EQ(picture->cullRect().getRight(), 30.0f);
  EXPECT_FLOAT_EQ(picture->cullRect().getBottom(), 40.0f);
}

TEST(PictureTest, TheWidthAndHeightFormStartsAtTheOrigin) {
  PictureRecorder recorder;
  ASSERT_NE(recorder.beginRecording(20.0f, 10.0f), nullptr);

  auto picture = recorder.finishRecording();
  ASSERT_NE(picture, nullptr);
  EXPECT_FLOAT_EQ(picture->cullRect().getLeft(), 0.0f);
  EXPECT_FLOAT_EQ(picture->cullRect().getTop(), 0.0f);
  EXPECT_FLOAT_EQ(picture->cullRect().getRight(), 20.0f);
  EXPECT_FLOAT_EQ(picture->cullRect().getBottom(), 10.0f);
}

TEST(PictureTest, EveryPictureHasItsOwnIdentifier) {
  PictureRecorder recorder;

  recorder.beginRecording(Rect::MakeLTRB(0.0f, 0.0f, 4.0f, 4.0f));
  auto first = recorder.finishRecording();
  recorder.beginRecording(Rect::MakeLTRB(0.0f, 0.0f, 4.0f, 4.0f));
  auto second = recorder.finishRecording();

  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(first->uniqueId(), second->uniqueId())
          << "two pictures with the same identifier would collide in any cache keyed on it";
}

TEST(PictureTest, AnEmptyRecordingIsAPictureOfNothing) {
  // Not an error: a scene that drew nothing is a scene, and a replay of it draws nothing. Reporting a null picture
  // would make a caller special-case an empty frame, which happens whenever content is culled away.
  PictureRecorder recorder;
  ASSERT_NE(recorder.beginRecording(16.0f, 16.0f), nullptr);

  auto picture = recorder.finishRecording();
  ASSERT_NE(picture, nullptr);
  EXPECT_EQ(picture->operationCount(), 0);
  EXPECT_GT(picture->approximateBytesUsed(), 0u) << "even an empty picture occupies something";

  auto surface = makeSurface();
  ASSERT_NE(surface, nullptr);
  const std::vector<uint8_t> before = pixelsOf(*surface);
  picture->playback(surface->getCanvas().get());
  EXPECT_EQ(pixelsOf(*surface), before) << "an empty recording leaves the canvas alone";
}

// ==================================================================================================
// The recorder's lifecycle
// ==================================================================================================

TEST(PictureRecorderTest, FinishingTwiceGivesOnePictureAndThenNothing) {
  PictureRecorder recorder;
  recorder.beginRecording(8.0f, 8.0f);
  ASSERT_NE(recorder.finishRecording(), nullptr);

  // The second call has nothing to finish. Returning the same picture again would make a destructor path that calls
  // this unconditionally produce a picture that is already owned elsewhere.
  EXPECT_EQ(recorder.finishRecording(), nullptr);
  EXPECT_EQ(recorder.recordingCanvas(), nullptr) << "and no recording is in progress";
}

TEST(PictureRecorderTest, FinishingWithoutStartingGivesNothing) {
  PictureRecorder recorder;
  EXPECT_EQ(recorder.finishRecording(), nullptr);
  EXPECT_EQ(recorder.recordingCanvas(), nullptr);
}

TEST(PictureRecorderTest, ASecondRecordingDiscardsTheFirst) {
  PictureRecorder recorder;

  recorder.beginRecording(8.0f, 8.0f);
  recorder.recordingCanvas()->clear(Color { 1.0f, 0.0f, 0.0f, 1.0f });
  recorder.beginRecording(8.0f, 8.0f);
  recorder.recordingCanvas()->clear(Color { 0.0f, 1.0f, 0.0f, 1.0f });

  auto picture = recorder.finishRecording();
  ASSERT_NE(picture, nullptr);

  // Only the second recording is in the picture. Rendering it must show green and not red, which is the check that the
  // first recording's calls were discarded rather than accumulated.
  auto surface = makeSurface();
  ASSERT_NE(surface, nullptr);
  picture->playback(surface->getCanvas().get());

  const Pixmap pixmap = surface->peekPixels();
  ASSERT_FALSE(pixmap.isEmpty());

  // `getPixelARGB32` returns `0xAARRGGBB`, so red is the third byte and green the second. Reading them the other way
  // round made a correct result look like the wrong colour -- and would have made a wrong one look right, since the
  // two channels are compared with each other.
  const uint32_t pixel = pixmap.getPixelARGB32(4, 4);
  const uint32_t red = (pixel >> 16) & 0xFF;
  const uint32_t green = (pixel >> 8) & 0xFF;
  EXPECT_GT(green, red) << "the second recording is the one that was kept";
}

TEST(PictureRecorderTest, ResetEndsTheRecording) {
  PictureRecorder recorder;
  recorder.beginRecording(8.0f, 8.0f);
  ASSERT_NE(recorder.recordingCanvas(), nullptr);

  recorder.reset();
  EXPECT_EQ(recorder.recordingCanvas(), nullptr);
  EXPECT_EQ(recorder.finishRecording(), nullptr) << "a reset recording cannot be finished";
}

TEST(PictureRecorderTest, TheCanvasIsUsableForSeveralRecordings) {
  // The recorder owns one canvas and reuses it, which is what keeps a loop that records many pictures from allocating
  // one per picture.
  PictureRecorder recorder;

  recorder.beginRecording(8.0f, 8.0f);
  Canvas* first = recorder.recordingCanvas();
  auto firstPicture = recorder.finishRecording();
  ASSERT_NE(first, nullptr);
  ASSERT_NE(firstPicture, nullptr);

  recorder.beginRecording(8.0f, 8.0f);
  EXPECT_EQ(recorder.recordingCanvas(), first) << "the same canvas is reused";
  ASSERT_NE(recorder.finishRecording(), nullptr);
}

// ==================================================================================================
// What is refused
// ==================================================================================================

TEST(PictureTest, PlayingOntoARecordingCanvasIsRefused) {
  // A replay onto a recording canvas would record the replay, and replaying *that* would record it again -- there is no
  // fixed point. Refusing is the only answer that terminates, and a picture that silently asked to be recorded twice
  // would be a picture of a picture of a picture.
  PictureRecorder source;
  source.beginRecording(8.0f, 8.0f);
  Paint paint;
  paint.color = Color { 1.0f, 1.0f, 1.0f, 1.0f };
  source.recordingCanvas()->drawRect(Rect::MakeLTRB(0.0f, 0.0f, 4.0f, 4.0f), paint);
  auto picture = source.finishRecording();
  ASSERT_NE(picture, nullptr);

  PictureRecorder destination;
  Canvas* recording = destination.beginRecording(Rect::MakeLTRB(0.0f, 0.0f, 8.0f, 8.0f));
  ASSERT_NE(recording, nullptr);

  const int before = picture->operationCount();
  picture->playback(recording);
  EXPECT_EQ(picture->operationCount(), before) << "the picture was not modified by the refused replay";

  auto second = destination.finishRecording();
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->operationCount(), 0) << "and nothing was recorded onto the destination";
}

TEST(PictureTest, PlayingOntoNothingIsRefused) {
  PictureRecorder recorder;
  recorder.beginRecording(8.0f, 8.0f);
  auto picture = recorder.finishRecording();
  ASSERT_NE(picture, nullptr);

  // There is nowhere to draw. Nothing happens, and nothing crashes -- which is the property that matters for a caller
  // holding an optional canvas.
  picture->playback(nullptr);
}
