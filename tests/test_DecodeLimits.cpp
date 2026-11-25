/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <graphics/DecodeLimits.h>

#include <graphics/ImageCodec.h>
#include <lottie/Lottie.h>
#include <svg/Svg.h>

#include <gtest/gtest.h>

#include <string>

namespace {

/** Restores the process-wide limits when a test leaves, so one test cannot bend another. */
struct RestoreLimits {
  graphics::DecodeLimits limits;
  ~RestoreLimits() { graphics::setDecodeLimits(limits); }
};

} // namespace

TEST(DecodeLimits, SvgAndLottieRefuseDocumentsOverTheByteLimit) {
  const graphics::DecodeLimits previous = graphics::decodeLimits();
  const RestoreLimits restore{ previous };
  graphics::DecodeLimits limits = previous;
  limits.maxBytes = 8;
  graphics::setDecodeLimits(limits);

  std::string error;
  EXPECT_FALSE(graphics::Svg::parse("<svg></svg>", &error).has_value());
  EXPECT_NE(error.find("decode limit"), std::string::npos) << error;

  error.clear();
  EXPECT_FALSE(graphics::Lottie::parse(R"({"layers":[]})", &error).has_value());
  EXPECT_NE(error.find("decode limit"), std::string::npos) << error;
}

TEST(DecodeLimits, AnImageOverTheEncodedLimitIsRefusedBeforeSignatureDetection) {
  const graphics::DecodeLimits previous = graphics::decodeLimits();
  const RestoreLimits restore{ previous };
  graphics::DecodeLimits limits = previous;
  limits.maxBytes = 4;
  graphics::setDecodeLimits(limits);

  const unsigned char png[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
  EXPECT_FALSE(graphics::decodeImage(png, sizeof(png), graphics::DecodeOptions{}).has_value())
    << "a payload over the limit is refused before the format is even inspected";
}

TEST(DecodeLimits, TheLimitsAreProcessWideAndRestorable) {
  const graphics::DecodeLimits previous = graphics::decodeLimits();
  const RestoreLimits restore{ previous };
  graphics::DecodeLimits limits = previous;
  limits.maxBytes = 123;
  limits.maxPixels = 456;
  graphics::setDecodeLimits(limits);
  EXPECT_EQ(graphics::decodeLimits().maxBytes, 123u);
  EXPECT_EQ(graphics::decodeLimits().maxPixels, 456u);

  graphics::setDecodeLimits(previous);
  EXPECT_EQ(graphics::decodeLimits().maxBytes, previous.maxBytes);
  EXPECT_EQ(graphics::decodeLimits().maxPixels, previous.maxPixels);
}

TEST(DecodeLimits, DeepNestingIsRefusedBeforeTheTreeIsBuilt) {
  const graphics::DecodeLimits previous = graphics::decodeLimits();
  const RestoreLimits restore{ previous };
  graphics::DecodeLimits limits = previous;
  limits.maxDepth = 8;
  graphics::setDecodeLimits(limits);

  std::string svg;
  for (int depth = 0; depth < 64; ++depth) svg += "<a>";
  for (int depth = 0; depth < 64; ++depth) svg += "</a>";
  std::string error;
  EXPECT_FALSE(graphics::Svg::parse(svg, &error).has_value());
  EXPECT_NE(error.find("decode limit"), std::string::npos) << error;

  // A Lottie document that nests objects: the JSON library would recurse while building the model, so
  // the scan has to refuse it first.
  std::string json = "{\"a\":";
  for (int depth = 0; depth < 64; ++depth) json += "{\"a\":";
  json += "0";
  for (int depth = 0; depth < 64; ++depth) json += "}";
  json += "}";
  error.clear();
  EXPECT_FALSE(graphics::Lottie::parse(json, &error).has_value());
  EXPECT_NE(error.find("decode limit"), std::string::npos) << error;
}

TEST(DecodeLimits, ADocumentWithTooManyElementsIsRefused) {
  const graphics::DecodeLimits previous = graphics::decodeLimits();
  const RestoreLimits restore{ previous };
  graphics::DecodeLimits limits = previous;
  limits.maxNodes = 10;
  graphics::setDecodeLimits(limits);

  std::string svg = "<svg width=\"4\" height=\"4\">";
  for (int index = 0; index < 32; ++index) svg += "<g/>";
  svg += "</svg>";
  std::string error;
  EXPECT_FALSE(graphics::Svg::parse(svg, &error).has_value());
  EXPECT_NE(error.find("decode limit"), std::string::npos) << error;
}

TEST(DecodeLimits, AReasonableDocumentStillParsesUnderTheSameLimits) {
  const graphics::DecodeLimits previous = graphics::decodeLimits();
  const RestoreLimits restore{ previous };
  graphics::DecodeLimits limits = previous;
  limits.maxDepth = 16;
  limits.maxNodes = 100;
  graphics::setDecodeLimits(limits);

  std::string error;
  EXPECT_TRUE(graphics::Svg::parse(
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"4\" height=\"4\"><g><rect width=\"4\" height=\"4\"/></g></svg>",
    &error).has_value()) << error;
}