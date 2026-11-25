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
 * @file Svg.cpp
 *
 * The public wrapper around the document tree.
 *
 * `Svg` holds a pointer to an incomplete type rather than the tree itself, for the same reason `Typeface` and `Path` do:
 * the tree's headers are private, and a client that included them would be able to depend on the shape of a document
 * this library intends to be free to change. The cost is one indirection per call, on calls that draw a picture.
 */

#include <svg/Svg.h>

#include <graphics/DecodeLimits.h>

#include "svg/SvgDocument.h"

#include "xml/Xml.h"

#include <utility>

namespace graphics {

struct Svg::Impl {
  svg::Document document;
  std::string error;
};

Svg::Svg(Svg&&) noexcept = default;
Svg& Svg::operator=(Svg&&) noexcept = default;
Svg::~Svg() = default;

std::optional<Svg> Svg::parse(const std::string_view document, std::string* error) {
  // The size is checked before the XML walker sees the document: a hostile file is refused by its
  // length, not by how much of it the parser has already turned into nodes.
  if (const std::size_t maxBytes = decodeLimits().maxBytes;
      maxBytes != 0 && document.size() > maxBytes) {
    if (error != nullptr) *error = "document is larger than the decode limit";
    return std::nullopt;
  }
  xml::ParseError parseError;
  const auto root = xml::parse(document, &parseError);
  if (!root) {
    if (error != nullptr) *error = parseError.describe();
    return std::nullopt;
  }

  std::string buildError;
  auto built = svg::buildDocument(*root, &buildError);
  if (!built) {
    if (error != nullptr) *error = buildError;
    return std::nullopt;
  }

  Svg parsed;
  parsed.mImpl = std::make_unique<Impl>();
  parsed.mImpl->document = std::move(*built);
  return parsed;
}

bool Svg::isEmpty() const noexcept { return mImpl == nullptr || mImpl->document.isEmpty(); }

Scalar Svg::width() const noexcept { return mImpl == nullptr ? 0 : mImpl->document.width(); }
Scalar Svg::height() const noexcept { return mImpl == nullptr ? 0 : mImpl->document.height(); }

void Svg::render(Canvas& canvas, const Rect& bounds) const {
  if (mImpl == nullptr) return;
  mImpl->document.render(canvas, bounds);
}

void Svg::render(Canvas& canvas) const {
  if (mImpl == nullptr) return;
  mImpl->document.render(canvas);
}

} // namespace graphics
