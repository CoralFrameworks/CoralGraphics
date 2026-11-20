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

#ifndef CORAL_GRAPHICS_SVG_SVGDATAURI_H
#define CORAL_GRAPHICS_SVG_SVGDATAURI_H

#include <graphics/Data.h>

#include <optional>
#include <string>
#include <string_view>

/**
 * @file SvgDataUri.h
 *
 * `data:` URIs, which is how an SVG document carries a picture inside itself.
 *
 * ## Why a document embeds rather than links
 *
 * Every `<image>` in this corpus is a base64 `data:` URI -- nine of them, from four kilobytes to three hundred and
 * twenty. That is not a quirk of the corpus: an SVG exported from a drawing tool has no way to know where its images will
 * be relative to the document that includes it, so it inlines them. A renderer that only resolved relative paths would
 * draw nothing for any of them.
 *
 * ## What is supported, and what is refused
 *
 * A `data:` URI with a `;base64` marker and a payload. The percent-encoded form -- a payload with its bytes spelled as
 * `%XX` -- is *refused with a reason* rather than half-decoded, because a partially decoded image is a corrupt picture
 * rather than a missing one, and corrupt pictures get debugged as codec bugs. Nothing in the corpus uses that form.
 *
 * The media type is read but not checked against the bytes: the decoders dispatch on the bytes themselves, which is both
 * more robust and the only thing that can be right when the two disagree.
 */

namespace graphics::svg {

/**
 * @return the bytes a `data:` URI holds, or nothing with @p error set.
 *
 * @param href the attribute's value, exactly as written.
 * @param error filled in when @p href is not a `data:` URI, uses an encoding this does not implement, or does not
 *        decode; may be null.
 */
[[nodiscard]] std::optional<Data> decodeDataUri(std::string_view href, std::string* error = nullptr);

/** @return true when @p href begins with `data:`, so a caller can tell an embedded image from a linked one. */
[[nodiscard]] bool isDataUri(std::string_view href) noexcept;

} // namespace graphics::svg

#endif // CORAL_GRAPHICS_SVG_SVGDATAURI_H
