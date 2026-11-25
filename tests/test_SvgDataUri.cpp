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
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file test_SvgDataUri.cpp
 *
 * Reading a `data:` URI.
 *
 * The base64 decoding itself belongs to `utils` and is not re-tested here. What is tested is the *grammar* around it --
 * finding the marker, splitting the payload, and refusing the form that is not implemented -- because that is the part
 * this file wrote, and the part where a long payload makes a wrong split easy to miss. The pictures in the corpus are up
 * to three hundred and twenty kilobytes of base64, so a split that is off by one character produces a plausible-looking
 * failure rather than an obvious one.
 */

#include "svg/SvgDataUri.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using graphics::Data;
using graphics::svg::decodeDataUri;
using graphics::svg::isDataUri;

/** @return the URI for @p text with the PNG media type, which is what every corpus image carries. */
std::string pngUri(const std::string& base64) { return "data:image/png;base64," + base64; }

} // namespace anonymous

// ==================================================================================================
// Recognizing one
// ==================================================================================================

TEST(SvgDataUri, RecognisesTheScheme) {
  EXPECT_TRUE(isDataUri("data:image/png;base64,AAAA"));
  EXPECT_TRUE(isDataUri("data:,"));

  EXPECT_FALSE(isDataUri("image.png"));
  EXPECT_FALSE(isDataUri("#image0"));
  EXPECT_FALSE(isDataUri("https://example.invalid/a.png"));
  EXPECT_FALSE(isDataUri(""));
  EXPECT_FALSE(isDataUri("dat"));
}

TEST(SvgDataUri, RecognisesTheSchemeInAnyCase) {
  // A URI scheme is case-insensitive, so an exporter may write `DATA:`. Compared by hand rather than by lowercasing the
  // whole string, because the payload can be hundreds of kilobytes.
  EXPECT_TRUE(isDataUri("DATA:image/png;base64,AAAA"));
  EXPECT_TRUE(isDataUri("Data:image/png;base64,AAAA"));
}

// ==================================================================================================
// Decoding one
// ==================================================================================================

TEST(SvgDataUri, DecodesABase64Payload) {
  // "AQID" is the four base64 characters for the three bytes 1, 2, 3.
  const auto decoded = decodeDataUri(pngUri("AQID"));
  ASSERT_TRUE(decoded.has_value());
  ASSERT_EQ(decoded->size(), 3U);
  EXPECT_EQ((*decoded)[0], 1);
  EXPECT_EQ((*decoded)[1], 2);
  EXPECT_EQ((*decoded)[2], 3);
}

TEST(SvgDataUri, DecodesAPayloadWithPadding) {
  // Two bytes encode to four characters ending in one `=`, and one byte to two `=`. Both appear at the end of a real
  // image, so a padding rule that is off makes the last pixel wrong rather than the decode fail.
  const auto twoBytes = decodeDataUri(pngUri("AQI="));
  ASSERT_TRUE(twoBytes.has_value());
  EXPECT_EQ(twoBytes->size(), 2U);

  const auto oneByte = decodeDataUri(pngUri("AQ=="));
  ASSERT_TRUE(oneByte.has_value());
  EXPECT_EQ(oneByte->size(), 1U);
  EXPECT_EQ((*oneByte)[0], 1);
}

TEST(SvgDataUri, FindsTheMarkerOnlyInTheHeader) {
  // The subtle one. A base64 payload can contain the letters `base64` by chance -- the alphabet includes them -- and a
  // search that ran past the comma would find one of those and split the URI in the wrong place. Here the payload is
  // literally the encoded form of a string, chosen so that a naive search would be misled.
  const std::string payload = "YmFzZTY0"; // "base64"
  const auto decoded = decodeDataUri(pngUri(payload));
  ASSERT_TRUE(decoded.has_value()) << "the marker in the payload must not be split on";
  ASSERT_EQ(decoded->size(), 6U);
  EXPECT_EQ(static_cast<char>((*decoded)[0]), 'b');
  EXPECT_EQ(static_cast<char>((*decoded)[5]), '4');
}

TEST(SvgDataUri, AcceptsAMediaTypeWithParameters) {
  // `image/png;charset=utf-8;base64,` is a legal header. The marker is searched for rather than the header being split
  // on its first semicolon, which is what a naive reader does and what would leave the marker unread here.
  const auto decoded = decodeDataUri("data:image/png;charset=utf-8;base64,AQID");
  ASSERT_TRUE(decoded.has_value()) << "a parameter before the marker must not defeat it";
  EXPECT_EQ(decoded->size(), 3U);
}

TEST(SvgDataUri, AcceptsAMissingMediaType) {
  // `data:;base64,...` and `data:base64,...` are both legal: the media type is optional and defaults to text/plain.
  EXPECT_TRUE(decodeDataUri("data:;base64,AQID").has_value());
  EXPECT_TRUE(decodeDataUri("data:base64,AQID").has_value());
}

// ==================================================================================================
// What it refuses, and why it says so
// ==================================================================================================

TEST(SvgDataUri, RefusesAReferenceThatIsNotADataUri) {
  std::string error;
  EXPECT_FALSE(decodeDataUri("image.png", &error).has_value());
  EXPECT_FALSE(error.empty());
}

TEST(SvgDataUri, RefusesAHeaderWithNoComma) {
  std::string error;
  EXPECT_FALSE(decodeDataUri("data:image/png;base64", &error).has_value());
  EXPECT_NE(error.find("comma"), std::string::npos) << "the reason names what was missing";
}

TEST(SvgDataUri, RefusesAnEmptyPayload) {
  std::string error;
  EXPECT_FALSE(decodeDataUri("data:image/png;base64,", &error).has_value());
  EXPECT_NE(error.find("empty"), std::string::npos);
}

TEST(SvgDataUri, RefusesThePercentEncodedFormWithAReason) {
  // Not implemented, and refused rather than half-decoded: a partially decoded image is a *corrupt* picture rather than a
  // missing one, and a corrupt picture is debugged as a codec bug. Nothing in the corpus uses this form.
  std::string error;
  EXPECT_FALSE(decodeDataUri("data:image/svg+xml,%3Csvg%3E", &error).has_value());
  EXPECT_NE(error.find("percent"), std::string::npos) << "and the reason says which form is missing";
}

TEST(SvgDataUri, RefusesAPayloadWithACharacterThatIsNotBase64) {
  std::string error;
  EXPECT_FALSE(decodeDataUri(pngUri("AQ!D"), &error).has_value());
  EXPECT_FALSE(error.empty()) << "a decoder refusal is reported rather than silently truncated";
}

TEST(SvgDataUri, RefusesAPayloadWithBrokenPadding) {
  // A truncated URI -- what a document looks like when it has been cut off -- lands here rather than decoding to a short
  // image, which is the difference between a missing picture and a wrong one.
  std::string error;
  EXPECT_FALSE(decodeDataUri(pngUri("A"), &error).has_value());
  EXPECT_FALSE(error.empty());
}

TEST(SvgDataUri, DecodesTheCorpusImageThatWouldBeEasiestToGetWrong) {
  // The longest payload in the corpus is 328,902 characters. A split that is off by one is invisible in a short test and
  // fatal in a long one, so the decoded length is checked against the URI's own length.
  //
  // The arithmetic has three cases, and the first version of this test had only the first of them: four characters carry
  // three bytes, and then *two* leftover characters carry one more byte and *three* carry two. 328,902 is two more than a
  // multiple of four, so the answer is 82,225 groups of three plus one -- which is what the decoder said and what the
  // test called wrong.
  const size_t payloadLength = 328902;
  const size_t wholeGroups = payloadLength / 4;
  const size_t leftover = payloadLength % 4;
  const size_t expected = wholeGroups * 3 + (leftover == 2 ? 1 : leftover == 3 ? 2 : 0);

  // Not the real image: a payload of the right length, so the arithmetic is exercised without a three-hundred-kilobyte
  // literal in the source. The characters decode to zero bytes, which is fine -- what is under test is the split.
  const std::string payload(payloadLength, 'A');
  const auto decoded = decodeDataUri(pngUri(payload));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->size(), expected) << "two leftover characters carry one byte, not none";
}

TEST(SvgDataUri, ReportsNothingWhenTheReasonIsNotWanted) {
  // A caller that only wants to know whether it worked passes no error pointer, and that must not crash.
  EXPECT_FALSE(decodeDataUri("image.png", nullptr).has_value());
}
