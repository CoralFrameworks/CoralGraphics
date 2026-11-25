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

#ifndef CORAL_GRAPHICS_LOTTIE_LOTTIE_H
#define CORAL_GRAPHICS_LOTTIE_LOTTIE_H

#include <graphics/Canvas.h>

#include <geometry/Rect.h>
#include <geometry/Scalar.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

/**
 * @file Lottie.h
 *
 * Lottie animations: parse a document, then draw it at a time.
 *
 * ## What is implemented, and what is not
 *
 * Implemented: shape layers, groups, path/fill/stroke/merge/transform items, cubic vertex paths with their tangents,
 * every transform property including skew, group and layer opacity through a layer, and keyframed animation with hold
 * and cubic-bezier easing.
 *
 * Not implemented, and named here rather than left to be discovered: precomposition layers, image and solid layers,
 * text, gradient fills and strokes, trim paths, repeaters, and masks and mattes. A document using one of those renders
 * without it rather than not rendering -- the item is kept in the tree and nothing draws it -- so the picture is
 * incomplete and not wrong.
 *
 * ## Time
 *
 * The public API counts in **seconds**, because that is what a player has and what a caller can reason about. The
 * document counts in frames, and the conversion happens once, inside. A caller stepping a frame at a time divides by the
 * frame rate themselves; `frameRate` is exposed for exactly that.
 */

namespace graphics {

/** A parsed Lottie animation. */
class Lottie {
  public:
    /**
     * An empty animation, which draws nothing and answers zero for everything.
     *
     * Declared here and defined in the implementation rather than defaulted in place: a defaulted constructor written
     * inside the class is defined inline, and its definition has to be able to destroy the members -- which needs the
     * incomplete `Impl` to be complete. Out of line is where it is.
     */
    Lottie();
    Lottie(Lottie&&) noexcept;
    Lottie& operator=(Lottie&&) noexcept;
    Lottie(const Lottie&) = delete;
    Lottie& operator=(const Lottie&) = delete;
    ~Lottie();

    /**
     * Parses @p json.
     *
     * @param json  a lottie json string to parse.
     * @param error filled in when the text is not JSON, is not a Lottie document, or holds no shape layers; may be null.
     * @return the animation, or nothing.
     */
    [[nodiscard]] static std::optional<Lottie> parse(std::string_view json, std::string* error = nullptr);

    [[nodiscard]] bool isEmpty() const noexcept;

    /** The composition's size in its own units. This is what `render(canvas, seconds)` draws at. */
    [[nodiscard]] Scalar width() const noexcept;
    [[nodiscard]] Scalar height() const noexcept;

    /** How many frames the document states per second, which a caller stepping frame by frame needs. */
    [[nodiscard]] Scalar frameRate() const noexcept;

    /** The length in seconds. */
    [[nodiscard]] Scalar duration() const noexcept;

    /**
     * Draws the frame at @p seconds into @p bounds.
     *
     * A time before the start or past the end draws the nearest frame within the document's range, so a player that has
     * run off the end holds the last frame rather than going blank.
     */
    void render(Canvas& canvas, const Rect& bounds, Scalar seconds) const;

    /** Draws at the composition's own size, at the origin. */
    void render(Canvas& canvas, Scalar seconds) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace graphics

#endif // CORAL_GRAPHICS_LOTTIE_LOTTIE_H
