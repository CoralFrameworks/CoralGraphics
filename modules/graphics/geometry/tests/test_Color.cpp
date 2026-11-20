/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License is distributed on
 * an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 */

/**
 * @file test_Color.cpp
 *
 * The two ways a `Color` becomes text, which produce different text on purpose.
 *
 * This file exists because `Color::toString()` had no test at all, and a free `ColorToString` sat declared in the public
 * header with no definition behind it. Neither was noticed, because nothing called either one: an untested formatter and
 * an unimplemented promise look exactly like working code from every direction except a call site.
 */

#include <geometry/Color.h>

#include <gtest/gtest.h>

#include <sstream>
#include <string>

namespace graphics {
namespace {


TEST(ColorTest, ToStringUsesTheLogFormat) {
  // Pinned exactly, because this string goes into log lines and a diagnostic that quietly changes shape is worse than
  // one that never existed -- whoever is reading the log has a pattern in mind.
  //
  // The values are the inputs rather than near them. `Color(1.0f, ...)` is 1.0f, and `%.1f` renders it "1.0", which is
  // what makes this an equality rather than a tolerance.
  EXPECT_EQ(Color(1.0f, 2.0f, 3.0f, 4.0f).toString(), "{R=1.0,G=2.0,B=3.0,A=4.0}");
}

TEST(ColorTest, ToStringRoundsToOneDecimal) {
  // One decimal is the contract of `%.1f`. The value below is 0.25f, which is 0.25 exactly in binary, so a change to
  // `%.2f` would show up as "0.25" rather than being absorbed by rounding.
  EXPECT_EQ(Color(0.25f, 0.25f, 0.25f, 0.25f).toString(), "{R=0.2,G=0.2,B=0.2,A=0.2}")
          << "printf rounds a trailing 5 to even, so 0.25 renders as 0.2";

  // And the other side of that tie, so the assertion above is known to be about rounding rather than about truncation.
  EXPECT_EQ(Color(0.75f, 0.75f, 0.75f, 0.75f).toString(), "{R=0.8,G=0.8,B=0.8,A=0.8}");
}

TEST(ColorTest, ToStringHandlesValuesWiderThanOne) {
  // The buffer that holds the formatted string is 128 bytes, which is room for far more than this. A value in the tens of
  // thousands is checked rather than assumed: `toString` is on a diagnostic path, and one that truncated a large value
  // would be reporting the wrong number rather than failing.
  EXPECT_EQ(Color(12345.0f, 0.0f, -6.5f, 1.0f).toString(), "{R=12345.0,G=0.0,B=-6.5,A=1.0}");
}

TEST(ColorTest, StreamOutputUsesTheCompactFormat) {
  // The second format, and the reason this file pins both: they are deliberately different, so a change that made one
  // match the other would be a silent behaviour change for every existing log and every existing test failure message
  // that streams a colour. `operator<<` is the debugging format -- short, because it appears inside a sentence -- and
  // `toString` is the standing format, with field names, because it appears on its own line.
  std::ostringstream out;
  out << Color(1.0f, 2.0f, 3.0f, 4.0f);
  EXPECT_EQ(out.str(), "(1, 2, 3, 4)");
}

TEST(ColorTest, TheTwoFormatsAreNotTheSame) {
  // The assertion that keeps the difference honest. If someone "tidied up" one of these to call the other, the tests
  // above would fail -- but this one says why, and it fails for the right reason rather than looking like a stray
  // expectation somebody should update.
  const Color color(1.0f, 2.0f, 3.0f, 4.0f);

  std::ostringstream streamed;
  streamed << color;

  EXPECT_NE(color.toString(), streamed.str())
          << "the log format and the stream format are separate on purpose; see the note in Color.h";
}

TEST(ColorTest, StreamOutputIsStableAcrossValues) {
  // The compact format with values that are not whole numbers, so the assertion above is not the only thing standing
  // between a formatting change and a silent difference in every failure message in the suite.
  std::ostringstream out;
  out << Color(0.5f, 0.25f, 0.125f, 1.0f);
  EXPECT_EQ(out.str(), "(0.5, 0.25, 0.125, 1)");
}

} // namespace anonymous
} // namespace graphics
