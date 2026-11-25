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

#ifndef CORAL_GRAPHICS_COLOR_FILTER_H
#define CORAL_GRAPHICS_COLOR_FILTER_H

#include <graphics/ColorTable.h>

#include <geometry/Color.h>
#include <geometry/Scalar.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace graphics {

/**
 * A 4x5 matrix that maps a premultiplied colour to another.
 *
 * Twenty values in five columns: the first four columns are the coefficients of red, green, blue and alpha, and the
 * fifth is a constant added afterwards. A row therefore reads as one output component,
 *
 *     out.r = m[0]*r + m[1]*g + m[2]*b + m[3]*a + m[4]
 *
 * which is the form every colour-matrix API uses and the form the eye is easiest to reason about: the identity is
 * one on the diagonal, a saturation matrix has its diagonal near a third, and a channel swap is a permutation of the
 * first three columns.
 *
 * Row-major, and flat, because that is how a matrix is handed to a shader.
 */
struct ColorFilterMatrix {
  std::array<Scalar, 20> values {};

  /** @return the identity matrix, which leaves every colour alone. */
  [[nodiscard]] static ColorFilterMatrix Identity() noexcept {
    ColorFilterMatrix matrix;
    matrix.values = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                      0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
                      0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                      0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    return matrix;
  }

  /** @return the element at @p row, @p column, where column 4 is the constant term. */
  [[nodiscard]] Scalar at(const int row, const int column) const noexcept {
    return values[static_cast<size_t>(row) * 5u + static_cast<size_t>(column)];
  }

  Scalar& at(const int row, const int column) noexcept {
    return values[static_cast<size_t>(row) * 5u + static_cast<size_t>(column)];
  }

  friend bool operator==(const ColorFilterMatrix&, const ColorFilterMatrix&) noexcept = default;
};

/**
 * A transformation applied to every colour drawn through a paint.
 *
 * ## What it is for
 *
 * Recolouring content without redrawing it: a greyscale filter over an icon, a saturation boost over a photograph, a
 * tint over a disabled control. The filter belongs to the paint rather than to the drawing call, so it applies to
 * whatever the paint is used on -- shapes, images, text -- and the code that draws does not have to know.
 *
 * ## The two forms
 *
 * A **matrix** is the general case: any affine transformation of the colour channels. A **table** is the special case
 * of one that treats each channel independently, expressed as four lookups of 256 entries. They overlap, and which to
 * use is a question of what the caller has: a formula becomes a matrix, a measured curve becomes a table, and a table
 * that turns out to be a curve is cheaper as one.
 *
 * ## What the filters do *not* carry
 *
 * The reference has a blending filter -- blend the source against a constant colour -- and the HSL and lighting
 * variants. Those are compositions of matrices rather than new kinds, and building them here would mean either a
 * second representation or a factory for every named effect. The matrix form covers all of them, so a caller that
 * wants a saturation filter writes the matrix, and the tests below show what one looks like.
 *
 * ## Premultiplication
 *
 * Colours in this library are premultiplied, and a matrix is applied to them as they are. That is not a detail: a
 * matrix whose fifth column is non-zero adds a constant to a premultiplied channel, which is a colour the caller
 * probably did not mean unless the alpha row also moves. The reference applies matrices the same way, so the
 * behaviour matches; what is worth stating is that a filter changing alpha should say so in its fourth row rather
 * than relying on the constant.
 */
class ColorFilter {
public:
  ~ColorFilter();

  ColorFilter(const ColorFilter&) = delete;
  ColorFilter& operator=(const ColorFilter&) = delete;
  ColorFilter(ColorFilter&&) noexcept;
  ColorFilter& operator=(ColorFilter&&) noexcept;

  /** Which of the representations this filter uses. */
  enum class Kind : uint8_t {
    /** Maps every channel through a 4x5 matrix. */
    kMatrix,
    /** Maps each channel through its own 256-entry table. */
    kTable,
    /** Applies one filter and then another. */
    kCompose,
  };

  /**
   * @param matrix the colour matrix, applied to premultiplied values.
   * @param clamp whether the result is confined to the unit range.
   *
   * Clamping is on by default because a matrix that produces a value outside the range is almost always a mistake in
   * the matrix rather than a request for an out-of-gamut colour -- which the rest of this library cannot represent
   * anyway. A caller that genuinely wants the intermediate values unclamped, to feed a second filter, turns it off
   * and is responsible for the result.
   */
  [[nodiscard]] static std::shared_ptr<ColorFilter> Matrix(
          const ColorFilterMatrix& matrix, bool clamp = true);

  /** The same, from the twenty values directly, in the row-major order `ColorFilterMatrix` documents. */
  [[nodiscard]] static std::shared_ptr<ColorFilter> Matrix(const float rowMajor[20], bool clamp = true);

  /** @return a filter that maps each channel through its table. */
  [[nodiscard]] static std::shared_ptr<ColorFilter> Table(const ColorTable& table);

  /**
   * @return a filter that applies @p outer to the result of @p inner.
   *
   * Composing rather than multiplying, because the two may be of different kinds: a matrix after a table is not a
   * matrix. The composition is recorded as a pair and applied in order, which is what makes it exact -- folding them
   * into one representation would mean approximating one of them.
   */
  [[nodiscard]] static std::shared_ptr<ColorFilter> Compose(
          const std::shared_ptr<ColorFilter>& outer, const std::shared_ptr<ColorFilter>& inner);

  [[nodiscard]] Kind kind() const noexcept { return mKind; }

  /** @return the matrix, or `std::nullopt` when this is a table or a composition. */
  [[nodiscard]] const ColorFilterMatrix* asMatrix() const noexcept;

  /** @return the table, or `std::nullopt` when this is a matrix or a composition. */
  [[nodiscard]] const ColorTable* asTable() const noexcept;

  /** @return the inner filter of a composition, or null when this is not one. */
  [[nodiscard]] const std::shared_ptr<ColorFilter>& composedInner() const noexcept { return mInner; }

  /** @return true when the matrix is clamped. Always true for a table, which cannot leave the range. */
  [[nodiscard]] bool clamps() const noexcept { return mClamp; }

  /**
   * @return true when this cannot change a colour at all.
   *
   * A caller uses it to drop a filter that was assembled conditionally and turned out to be nothing, which saves a
   * per-pixel pass. An identity matrix and the identity table are both recognised; a composition of two identities is
   * recognised by asking its parts.
   */
  [[nodiscard]] bool isIdentity() const;

  /**
   * @return true when the alpha channel is left exactly as it was.
   *
   * A caller uses it to know whether a filter can be skipped for an opaque surface, and it is the question that
   * distinguishes a colour adjustment from one that changes coverage.
   */
  [[nodiscard]] bool isAlphaUnchanged() const;

  /**
   * Applies this filter to @p color in place.
   *
   * The colour is premultiplied, as everywhere else in this library, and the result is premultiplied too. A filter
   * that drives alpha to zero produces transparent black rather than a division by zero.
   */
  void apply(Color* color) const;

  /** @return @p color with this filter applied. */
  [[nodiscard]] Color applyTo(const Color& color) const;

  /** @return an 8-bit-per-channel filter applied in place, which is what the rasterizer works in. */
  [[nodiscard]] uint32_t applyToPremultipliedArgb(uint32_t argb) const;

private:
  ColorFilter() = default;

  /** One entry of the expression this filter represents, innermost first. */
  struct Stage {
    Kind kind = Kind::kMatrix;
    ColorFilterMatrix matrix {};
    ColorTable table {};
    bool clamp = true;
  };

  /** The stages, in application order. A single filter is one stage; a composition is the inner then the outer. */
  std::vector<Stage> mStages;

  Kind mKind = Kind::kMatrix;
  bool mClamp = true;

  /** The inner filter when this is a composition, so that a caller can walk it. */
  std::shared_ptr<ColorFilter> mInner;

  /** The outer filter when this is a composition. */
  std::shared_ptr<ColorFilter> mOuter;
};

} // namespace graphics

#endif //CORAL_GRAPHICS_COLOR_FILTER_H
