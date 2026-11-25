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

#include <graphics/ColorFilter.h>

#include <algorithm>
#include <cmath>
#include <utility>

/**
 * @file ColorFilter.cpp
 *
 * Applying a colour transformation, and the three things that make it less obvious than it looks.
 *
 * **Premultiplication.** A colour here is premultiplied, and the matrix is applied to it as it is. That is what makes
 * the alpha row of a matrix meaningful: a row of zeros there, with a non-zero constant, *adds* coverage. The reader
 * who expects the matrix to apply to straight colour will be surprised by exactly one case -- a matrix that drives
 * alpha to zero -- and the code below handles that case rather than dividing by it.
 *
 * **Clamping and where it happens.** The result is clamped at the end, not per row, because a matrix row can
 * legitimately produce a large intermediate that a later row cancels. A saturation matrix is the example: its rows sum
 * to one, so a value of two in the red channel arrives at green as two thirds, and clamping red to one first would
 * change the answer.
 *
 * **The 8-bit path.** The rasterizer works in premultiplied 8-bit values, and filtering there is not the same as
 * filtering in floats and quantising afterwards -- the rounding differs. So the 8-bit entry point converts to the unit
 * range, applies the same stages, and writes back, rather than carrying a second implementation that would drift from
 * the first.
 */

namespace graphics {
namespace {

/** @return @p value confined to the unit range when @p clamp, and unchanged otherwise. */
Scalar clampUnit(const Scalar value, const bool clamp) noexcept {
  return clamp ? std::clamp(value, 0.0f, 1.0f) : value;
}

/** @return the four premultiplied components of @p color, in the order the matrix expects. */
std::array<Scalar, 4> componentsOf(const Color& color) noexcept {
  return { color.red, color.green, color.blue, color.alpha };
}

/** @return the colour those four components describe. */
Color colorOf(const std::array<Scalar, 4>& components) noexcept {
  return Color { components[0], components[1], components[2], components[3] };
}

/**
 * @return the four outputs of applying @p matrix to @p in.
 *
 * Every row is computed before any of them is clamped. Clamping between rows would make the result depend on the
 * order the rows happen to be in, which it must not: the rows are independent, and a caller reordering them changes
 * nothing else about the matrix.
 */
std::array<Scalar, 4> applyMatrix(const ColorFilterMatrix& matrix, const std::array<Scalar, 4>& in) noexcept {
  std::array<Scalar, 4> out {};
  for (int row = 0; row < 4; ++row) {
    out[static_cast<size_t>(row)] = matrix.at(row, 0) * in[0] + matrix.at(row, 1) * in[1] +
                                    matrix.at(row, 2) * in[2] + matrix.at(row, 3) * in[3] + matrix.at(row, 4);
  }
  return out;
}

/** @return @p in with each channel looked up in its table. */
std::array<Scalar, 4> applyTable(const ColorTable& table, const std::array<Scalar, 4>& in) noexcept {
  const auto lookup = [](const std::array<uint8_t, 256>& channel, const Scalar value) {
    // Rounded to the nearest of the 256 entries rather than truncated, because the entries are samples: a value of
    // 0.5 belongs at index 128, and truncating would put it at 127 and shift every midtone by a step.
    const Scalar clamped = std::clamp(value, 0.0f, 1.0f);
    const auto index = static_cast<size_t>(std::lround(clamped * 255.0f));
    return static_cast<Scalar>(channel[index]) / 255.0f;
  };

  return { lookup(table.red, in[0]), lookup(table.green, in[1]), lookup(table.blue, in[2]),
    lookup(table.alpha, in[3]) };
}

} // namespace anonymous

ColorFilter::~ColorFilter() = default;

ColorFilter::ColorFilter(ColorFilter&&) noexcept = default;
ColorFilter& ColorFilter::operator=(ColorFilter&&) noexcept = default;

// ==================================================================================================
// Building one
// ==================================================================================================

std::shared_ptr<ColorFilter> ColorFilter::Matrix(const ColorFilterMatrix& matrix, const bool clamp) {
  auto filter = std::shared_ptr<ColorFilter>(new ColorFilter());

  Stage stage;
  stage.kind = Kind::kMatrix;
  stage.matrix = matrix;
  stage.clamp = clamp;
  filter->mStages.push_back(stage);

  filter->mKind = Kind::kMatrix;
  filter->mClamp = clamp;
  return filter;
}

std::shared_ptr<ColorFilter> ColorFilter::Matrix(const float rowMajor[20], const bool clamp) {
  if (rowMajor == nullptr) return nullptr;

  ColorFilterMatrix matrix;
  for (size_t i = 0; i < matrix.values.size(); ++i) matrix.values[i] = rowMajor[i];
  return Matrix(matrix, clamp);
}

std::shared_ptr<ColorFilter> ColorFilter::Table(const ColorTable& table) {
  auto filter = std::shared_ptr<ColorFilter>(new ColorFilter());

  Stage stage;
  stage.kind = Kind::kTable;
  stage.table = table;
  // A table cannot leave the unit range, so the flag is meaningless for it. Recorded as true rather than false so
  // that "does this filter clamp" has one answer for every filter that does, and a caller need not special-case it.
  stage.clamp = true;
  filter->mStages.push_back(stage);

  filter->mKind = Kind::kTable;
  filter->mClamp = true;
  return filter;
}

std::shared_ptr<ColorFilter> ColorFilter::Compose(
        const std::shared_ptr<ColorFilter>& outer, const std::shared_ptr<ColorFilter>& inner) {
  if (outer == nullptr) return inner;
  if (inner == nullptr) return outer;

  // Composing with a filter that does nothing returns the other operand *itself*. Returning a new object that behaves
  // the same would break `compose(identity, f) == f` for a caller comparing handles, and there is no reason for the
  // extra stage: it would be a per-pixel no-op.
  if (outer->isIdentity()) return inner;
  if (inner->isIdentity()) return outer;

  auto filter = std::shared_ptr<ColorFilter>(new ColorFilter());
  filter->mKind = Kind::kCompose;
  filter->mInner = inner;
  filter->mOuter = outer;
  filter->mClamp = outer->mClamp;

  // The stages are concatenated rather than nested, so applying a composition of five filters is one loop rather than
  // five recursive calls. The order is the innermost first, which is the order they are applied in.
  filter->mStages.reserve(inner->mStages.size() + outer->mStages.size());
  filter->mStages.insert(filter->mStages.end(), inner->mStages.begin(), inner->mStages.end());
  filter->mStages.insert(filter->mStages.end(), outer->mStages.begin(), outer->mStages.end());
  return filter;
}

// ==================================================================================================
// What it is
// ==================================================================================================

const ColorFilterMatrix* ColorFilter::asMatrix() const noexcept {
  if (mKind != Kind::kMatrix || mStages.size() != 1) return nullptr;
  return &mStages.front().matrix;
}

const ColorTable* ColorFilter::asTable() const noexcept {
  if (mKind != Kind::kTable || mStages.size() != 1) return nullptr;
  return &mStages.front().table;
}

bool ColorFilter::isIdentity() const {
  // Asked of the stages rather than of the kind, so a composition of two identities answers true without a special
  // case: every stage has to be the identity, and a composition of identities is one.
  for (const Stage& stage: mStages) {
    if (stage.kind == Kind::kMatrix) {
      if (stage.matrix != ColorFilterMatrix::Identity()) return false;
    } else {
      if (!stage.table.isIdentity()) return false;
    }
  }
  return !mStages.empty();
}

bool ColorFilter::isAlphaUnchanged() const {
  for (const Stage& stage: mStages) {
    if (stage.kind == Kind::kTable) {
      // A table that changes the alpha channel changes coverage, however it was built.
      const ColorTable& identity = ColorTable::Identity();
      if (stage.table.alpha != identity.alpha) return false;
      continue;
    }

    // The alpha row is the fourth. It has to be exactly `a' = a` and nothing else -- any coefficient on red, green or
    // blue would make coverage depend on colour, and any constant would change it everywhere.
    const ColorFilterMatrix& m = stage.matrix;
    if (m.at(3, 0) != 0.0f || m.at(3, 1) != 0.0f || m.at(3, 2) != 0.0f || m.at(3, 3) != 1.0f || m.at(3, 4) != 0.0f) {
      return false;
    }
  }
  return !mStages.empty();
}

// ==================================================================================================
// Applying it
// ==================================================================================================

void ColorFilter::apply(Color* color) const {
  if (color == nullptr) return;

  std::array<Scalar, 4> components = componentsOf(*color);

  for (const Stage& stage: mStages) {
    components = (stage.kind == Kind::kMatrix) ? applyMatrix(stage.matrix, components)
                                               : applyTable(stage.table, components);

    // Clamped between stages, not inside one. A caller that turned clamping off asked for the intermediate values,
    // and a stage that follows it is applying to those -- clamping there would silently undo the request.
    if (stage.clamp) {
      for (Scalar& component: components) component = clampUnit(component, true);
    }
  }

  // A negative component is possible with clamping off, and a premultiplied colour cannot be negative: the channel
  // values may not exceed the alpha, and neither may be below zero. Rather than leave a colour the rest of the layer
  // would misread, the components are confined to what a premultiplied colour can be.
  for (Scalar& component: components) {
    if (!std::isfinite(component)) component = 0.0f;
    component = std::max(component, 0.0f);
  }
  if (components[3] > 1.0f) components[3] = 1.0f;
  for (size_t i = 0; i < 3; ++i) {
    if (components[i] > components[3]) components[i] = components[3];
  }

  *color = colorOf(components);
}

Color ColorFilter::applyTo(const Color& color) const {
  Color result = color;
  apply(&result);
  return result;
}

uint32_t ColorFilter::applyToPremultipliedArgb(const uint32_t argb) const {
  // From `0xAARRGGBB` to the unit range. The stored bytes are premultiplied and rounded to eight bits, so this is
  // where that rounding is -- and why filtering in floats and filtering in bytes do not give bit-identical answers.
  const auto toUnit = [](const uint32_t byte) { return static_cast<Scalar>(byte) / 255.0f; };

  Color color;
  color.alpha = toUnit((argb >> 24) & 0xFFu);
  color.red = toUnit((argb >> 16) & 0xFFu);
  color.green = toUnit((argb >> 8) & 0xFFu);
  color.blue = toUnit(argb & 0xFFu);

  apply(&color);

  // Back to eight bits, rounded rather than truncated, and back into the same packing.
  const auto toByte = [](const Scalar value) {
    return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)) & 0xFFu;
  };
  return (toByte(color.alpha) << 24) | (toByte(color.red) << 16) | (toByte(color.green) << 8) | toByte(color.blue);
}

} // namespace graphics
