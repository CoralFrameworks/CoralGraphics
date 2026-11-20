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

#ifndef CORAL_GRAPHICS_IMAGE_FILTER_H
#define CORAL_GRAPHICS_IMAGE_FILTER_H

#include <graphics/Image.h>

#include <geometry/Rect.h>
#include <geometry/Scalar.h>

#include <cstdint>
#include <memory>

namespace graphics {

/**
 * A transformation that takes an image and produces another.
 *
 * ## How it differs from the other three filters
 *
 * There are four filters in this library and each acts at a different stage:
 *
 *  - a **colour filter** changes the value of a pixel;
 *  - a **mask filter** changes a shape's coverage before it is blended;
 *  - a **path effect** changes the geometry, before rasterisation;
 *  - an **image filter** changes a whole *image*.
 *
 * The first three are all properties of a draw: they belong to a paint and are applied to whatever that paint is used
 * on. An image filter is not. It has an image in and an image out, with no paint and no canvas involved, which is what
 * makes it usable on a picture that has already been drawn -- a screenshot, a decoded file, the result of another
 * filter -- and what makes it the right tool for the effects that need to see the whole thing at once.
 *
 * ## The bounds question
 *
 * Every filter here has to answer where its result lies before it computes it. That is not an optimisation: a caller
 * sizes a buffer, and a filter that produced pixels outside the box it promised would write past the end of it. So
 * `filterBounds` is part of the interface rather than something a caller derives, and a blur expands the bounds by
 * its margin while an offset moves them.
 *
 * ## What is here
 *
 * Three operations, chosen because each is a different kind of transformation rather than because they are the three
 * most used:
 *
 *  - `Blur` convolves, and is the only one that reads neighbouring pixels.
 *  - `Offset` translates, and is the only one that changes the shape of the result.
 *  - `Compose` chains, so the output of one is the input of the next.
 *
 * Drop shadows, inner shadows and the tile and merge variants of the reference are compositions and parameterisations
 * of these. The two shadows are shown in the tests, built from `Offset` and `Compose`, rather than being factories
 * here -- because a factory for every named effect is a list to maintain rather than an abstraction, and the three
 * above are the abstraction.
 */
class ImageFilter {
public:
  ~ImageFilter();

  ImageFilter(const ImageFilter&) = delete;
  ImageFilter& operator=(const ImageFilter&) = delete;
  ImageFilter(ImageFilter&&) noexcept;
  ImageFilter& operator=(ImageFilter&&) noexcept;

  /** Which transformation this filter performs. */
  enum class Kind : uint8_t {
    /** A Gaussian blur, which reads neighbouring pixels. */
    kBlur,
    /** A translation, which moves pixels without changing them. */
    kOffset,
    /** Applies one filter to the result of another. */
    kCompose,
  };

  /**
   * @param sigmaX the standard deviation along x, in pixels.
   * @param sigmaY the standard deviation along y. Equal to @p sigmaX for an isotropic blur.
   * @return a blur, or null when either sigma is not a positive finite number.
   *
   * Zero is refused rather than treated as no blur: a caller that passed it has a radius or a scale computed wrong
   * upstream, and a filter that silently did nothing would send it looking at the caller of the filter instead.
   *
   * The blur is separable -- a two-dimensional Gaussian is the product of two one-dimensional ones -- so a caller
   * with a wide, short blur pays for two short passes rather than one enormous one.
   */
  [[nodiscard]] static std::shared_ptr<ImageFilter> Blur(Scalar sigmaX, Scalar sigmaY);

  /** @return a blur with the same standard deviation in both directions, or null when it is not positive. */
  [[nodiscard]] static std::shared_ptr<ImageFilter> Blur(Scalar sigma);

  /**
   * @param dx how far the result is moved along x. Positive moves it right.
   * @param dy how far it is moved along y. Positive moves it down.
   * @return an offset, or null when neither amount is a finite number.
   *
   * Both amounts being zero is *not* refused: an offset of nothing is the identity, and a caller that computed a
   * displacement of zero has a legitimate answer rather than a mistake. `isIdentity` reports it.
   *
   * The amount is rounded to whole pixels. A sub-pixel offset would need a resampling kernel and a choice of filter
   * to go with it, which is a different operation -- a scale -- rather than a translation.
   */
  [[nodiscard]] static std::shared_ptr<ImageFilter> Offset(Scalar dx, Scalar dy);

  /**
   * @return a filter that applies @p outer to the result of @p inner, or the other operand when either is null.
   *
   * Composing with a filter that does nothing returns the other operand itself rather than a new object that behaves
   * the same, so that a caller comparing handles gets the answer it expects and no stage is spent on a no-op.
   */
  [[nodiscard]] static std::shared_ptr<ImageFilter> Compose(
          const std::shared_ptr<ImageFilter>& outer, const std::shared_ptr<ImageFilter>& inner);

  [[nodiscard]] Kind kind() const noexcept { return mKind; }

  /** @return the blur's standard deviations, or zero for any other kind. */
  [[nodiscard]] Scalar sigmaX() const noexcept { return mSigmaX; }
  [[nodiscard]] Scalar sigmaY() const noexcept { return mSigmaY; }

  /** @return the offset's displacement, or zero for any other kind. */
  [[nodiscard]] Scalar offsetX() const noexcept { return mOffsetX; }
  [[nodiscard]] Scalar offsetY() const noexcept { return mOffsetY; }

  /** @return the outer filter of a composition, or null. */
  [[nodiscard]] const std::shared_ptr<ImageFilter>& outer() const noexcept { return mOuter; }

  /** @return the inner filter of a composition, or null. */
  [[nodiscard]] const std::shared_ptr<ImageFilter>& inner() const noexcept { return mInner; }

  /**
   * @return where the result of filtering an image bounded by @p bounds will lie.
   *
   * A blur expands it by three standard deviations on each side and a composition intersects the two answers, because
   * the inner filter's result is what the outer one sees. A caller sizes its buffer from this, so a filter that
   * under-reported would have its result written past the end of that buffer.
   */
  [[nodiscard]] Rect filterBounds(const Rect& bounds) const;

  /**
   * @return @p source with this filter applied.
   *
   * An empty image when the filter produces nothing, which is a legitimate answer rather than a failure: an offset
   * that moves an image entirely off its own bounds leaves nothing behind.
   *
   * The result keeps the source's dimensions where the filter does not change them -- a blur is computed into the
   * same box, so its edges fall off into transparency rather than the image growing. That is the choice the reference
   * makes for a filter used as a paint property, and it is what makes the bounds question above about *clipping*
   * rather than about growing.
   */
  [[nodiscard]] Image filter(const Image& source) const;

  /**
   * @return true when this filter would leave every image alone.
   *
   * A blur with a rounding-to-nothing sigma, an offset of zero, and a composition of two of those. A caller uses it to
   * drop a filter that was assembled conditionally and turned out to be nothing, which saves a whole pass over the
   * image.
   */
  [[nodiscard]] bool isIdentity() const;

private:
  ImageFilter() = default;

  /** Applies a Gaussian blur, which is where all the work is. */
  [[nodiscard]] Image applyBlur(const Image& source) const;

  /** Applies a translation. */
  [[nodiscard]] Image applyOffset(const Image& source) const;

  Kind mKind = Kind::kBlur;
  Scalar mSigmaX = 0.0f;
  Scalar mSigmaY = 0.0f;
  Scalar mOffsetX = 0.0f;
  Scalar mOffsetY = 0.0f;
  std::shared_ptr<ImageFilter> mOuter;
  std::shared_ptr<ImageFilter> mInner;
};

} // namespace graphics

#endif //CORAL_GRAPHICS_IMAGE_FILTER_H
