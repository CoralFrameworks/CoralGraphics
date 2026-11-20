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

#ifndef CORAL_GRAPHICS_SVG_SVG_H
#define CORAL_GRAPHICS_SVG_SVG_H

#include <graphics/Canvas.h>

#include <geometry/Rect.h>
#include <geometry/Scalar.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

/**
 * @file Svg.h
 *
 * SVG documents: parse one, then draw it.
 *
 * ## What is implemented, and what is not
 *
 * Implemented: the container elements (`svg`, `g`, `defs`, `use`, `a`), the shape elements (`path`, `rect`, `circle`,
 * `ellipse`, `line`, `polyline`, `polygon`), `text`, presentation attributes with the CSS cascade including `<style>`
 * rules and `style` attributes, transforms, `viewBox` with `preserveAspectRatio`, and the path grammar in full including
 * elliptical arcs.
 *
 * Not implemented, and named here rather than left to be discovered: gradients, patterns, clips, masks, filters, and
 * `@font-face`. A `fill="url(#g)"` therefore falls back to the colour written beside it, or draws unfilled when there is
 * none. That is SVG's own rule for a reference that cannot be resolved, so a document using one of these renders as
 * correctly as it can rather than not at all -- but it is not the reference image, and the difference is stated here
 * instead of being implied by silence.
 */

namespace graphics {

/** An SVG document, parsed and ready to draw. */
class Svg {
  public:
    Svg() = default;
    Svg(Svg&&) noexcept;
    Svg& operator=(Svg&&) noexcept;
    Svg(const Svg&) = delete;
    Svg& operator=(const Svg&) = delete;
    ~Svg();

    /**
     * Parses @p document.
     *
     * @param error filled in with a line and column when the document does not parse or does not state a usable size;
     *              may be null.
     * @return the document, or nothing.
     */
    [[nodiscard]] static std::optional<Svg> parse(std::string_view document, std::string* error = nullptr);

    /** @return true when there is a document to draw. */
    [[nodiscard]] bool isEmpty() const noexcept;

    /** @return the size the document asks for, in user units. This is what `render(canvas)` uses. */
    [[nodiscard]] Scalar width() const noexcept;
    [[nodiscard]] Scalar height() const noexcept;

    /**
     * Draws the document so that its viewport fills @p bounds.
     *
     * The viewBox, if there is one, is fitted into @p bounds; without one the document's own units are the bounds' and
     * only the origin moves. A document scaled up is drawn at the resolution of the destination, not of its own
     * coordinates, because nothing here rasterises in between.
     */
    void render(Canvas& canvas, const Rect& bounds) const;

    /** Draws at the document's own size, at the origin. */
    void render(Canvas& canvas) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace graphics

#endif // CORAL_GRAPHICS_SVG_SVG_H
