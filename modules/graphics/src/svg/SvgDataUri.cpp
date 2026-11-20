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
 * @file SvgDataUri.cpp
 *
 * The reader declared in SvgDataUri.h.
 *
 * The base64 half is `utils::Base64`, which is already in this library's dependency set -- so this file is only the URI
 * grammar: find the marker, split the payload, and turn the decoder's three error codes into one sentence. Writing a
 * second base64 would have meant two implementations of the same padding rules, and the padding rules are the part that
 * is easy to get wrong.
 */

#include "svg/SvgDataUri.h"

#include "SvgScanner.h"

#include <utils/Base64.h>

#include <string>
#include <vector>

namespace graphics::svg {
namespace {

constexpr std::string_view kPrefix = "data:";

/**
 * The `;base64` marker, which is what says the payload is encoded rather than percent-escaped.
 *
 * Searched for before the first comma rather than in the whole string: a base64 payload can itself contain the letters
 * `base64`, and a search that ran past the comma would find one of those in a long image and split the URI in the wrong
 * place. A URI whose payload happened to spell `;base64` would be misread, which is why the marker is looked for only in
 * the header -- where the format puts it.
 */
constexpr std::string_view kBase64Marker = "base64";

/** @return the sentence for a decoder that refused the payload. */
std::string describeBase64Error(const utils::Base64::Error error) {
  switch (error) {
    case utils::Base64::kNoError:
      return {};
    case utils::Base64::kPadError:
      // The padding is the `=` characters that make the payload a whole number of four-byte groups. Getting it wrong is
      // almost always a truncated URI, which is what a document looks like when it has been cut off in transit.
      return "the base64 payload has the wrong padding, which usually means it was truncated";
    case utils::Base64::kBadCharError:
      return "the base64 payload contains a character that is not base64";
  }
  return "the base64 payload could not be decoded";
}

} // namespace anonymous

bool isDataUri(const std::string_view href) noexcept {
  // Case-insensitively, because the scheme is case-insensitive and an exporter is free to write `DATA:`. Compared by
  // hand rather than by lowercasing the whole URI: the payload can be hundreds of kilobytes, and copying it to answer a
  // question about its first five bytes is a waste a document pays for.
  if (href.size() < kPrefix.size()) return false;
  for (size_t i = 0; i < kPrefix.size(); ++i) {
    const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(href[i])));
    if (a != kPrefix[i]) return false;
  }
  return true;
}

std::optional<Data> decodeDataUri(const std::string_view href, std::string* error) {
  const auto fail = [error](const std::string& message) -> std::optional<Data> {
    if (error != nullptr) *error = message;
    return std::nullopt;
  };

  if (!isDataUri(href)) return fail("the reference is not a data: URI");

  const std::string_view rest = href.substr(kPrefix.size());
  const size_t comma = rest.find(',');
  if (comma == std::string_view::npos) return fail("the data: URI has no comma separating its header from its payload");

  const std::string_view header = rest.substr(0, comma);
  const std::string_view payload = rest.substr(comma + 1);

  // The header is `mediatype[;base64]`, and the media type itself may carry parameters -- `image/png;charset=utf-8`.
  // Only the marker matters, so the header is searched rather than split on the first semicolon.
  if (header.find(kBase64Marker) == std::string_view::npos) {
    return fail("the data: URI is not base64-encoded; the percent-encoded form is not implemented");
  }
  if (payload.empty()) return fail("the data: URI has an empty payload");

  // The decoded size is at most three quarters of the payload's, rounded up to a group. Asking the decoder for the exact
  // size first is the documented two-call pattern; the estimate is only to avoid a second call, so an over-estimate is
  // harmless -- the decoder reports what it actually wrote.
  std::vector<uint8_t> decoded(payload.size() / 4 * 3 + 3);
  size_t written = decoded.size();
  const utils::Base64::Error result =
          utils::Base64::decode(payload.data(), payload.size(), decoded.data(), &written);
  if (result != utils::Base64::kNoError) return fail(describeBase64Error(result));
  if (written == 0) return fail("the data: URI decoded to no bytes");

  decoded.resize(written);
  return Data::makeCopy(decoded.data(), decoded.size());
}

} // namespace graphics::svg
