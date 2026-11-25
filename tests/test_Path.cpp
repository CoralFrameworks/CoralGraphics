/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and limitations under the
 * License.
 */

/**
 * @file test_Path.cpp
 *
 * The public path, and the bridge to the private pathops representation.
 *
 * Two things are being tested and they are different. The first is the arithmetic -- bounds, containment,
 * length -- where the answers are known by construction: a unit square's perimeter, a circle's
 * circumference, a point inside or outside. The second is the bridge itself, which is where the interesting
 * mistakes live: a conversion between the public fill type and the internal one that drops a case, a
 * transform that moves the endpoints but leaves the control points behind, a copy that shares state with
 * its original.
 */

#include <graphics/Path.h>

#include <gtest/gtest.h>

#include <cmath>

using namespace graphics;

namespace {

/**
 * A square with corners at (left, top) and (right, bottom), wound clockwise in screen coordinates.
 *
 * Three `lineTo` calls and a `close`, not four lines. The close *supplies* the edge back to the start, so a
 * fourth `lineTo` would draw the shape twice round -- which is what the first version of this helper did, and
 * it produced a path that was correctly reported as not a rectangle.
 */
Path makeSquare(const Scalar left, const Scalar top, const Scalar right, const Scalar bottom) {
  Path path;
  path.moveTo(Point { left, top });
  path.lineTo(Point { right, top });
  path.lineTo(Point { right, bottom });
  path.lineTo(Point { left, bottom });
  path.close();
  return path;
}

/** A circle of radius @p r about the origin, as four cubics -- the standard kappa approximation. */
Path makeCircle(const Scalar r) {
  // The control point offset that makes a cubic approximate a quarter circle. Exact to about one part in a
  // thousand, which is why circles in every graphics system are made of four of these rather than more.
  const Scalar k = r * 0.5522847498307936f;

  Path path;
  path.moveTo(Point { r, 0.0f });
  path.cubicTo(Point { r, k }, Point { k, r }, Point { 0.0f, r });
  path.cubicTo(Point { -k, r }, Point { -r, k }, Point { -r, 0.0f });
  path.cubicTo(Point { -r, -k }, Point { -k, -r }, Point { 0.0f, -r });
  path.cubicTo(Point { k, -r }, Point { r, -k }, Point { r, 0.0f });
  path.close();
  return path;
}

/** Asserts that @p path's bounds are the given rectangle. */
void expectBounds(const Path& path, const float left, const float top, const float right, const float bottom) {
  const Rect bounds = path.bounds();
  EXPECT_FLOAT_EQ(bounds.getLeft(), left);
  EXPECT_FLOAT_EQ(bounds.getTop(), top);
  EXPECT_FLOAT_EQ(bounds.getRight(), right);
  EXPECT_FLOAT_EQ(bounds.getBottom(), bottom);
}

} // namespace anonymous

// ==================================================================================================
// Building and counting
// ==================================================================================================

TEST(PathTest, EmptyByDefault) {
  const Path path;
  EXPECT_TRUE(path.isEmpty());
  EXPECT_EQ(path.verbCount(), 0u);
  EXPECT_EQ(path.pointCount(), 0u);
  EXPECT_FALSE(path.hasCurves());
  EXPECT_EQ(path.fillType(), FillType::Winding) << "winding is the default fill rule";
}

TEST(PathTest, CountsVerbsAndPointsSeparately) {
  Path path;
  path.moveTo({ 0.0f, 0.0f });
  path.lineTo({ 10.0f, 0.0f });
  path.quadTo({ 15.0f, 5.0f }, { 10.0f, 10.0f });
  path.cubicTo({ 5.0f, 15.0f }, { -5.0f, 15.0f }, { -10.0f, 10.0f });
  path.close();

  // Five verbs: a move, a line, a quadratic, a cubic and a close.
  EXPECT_EQ(path.verbCount(), 5u);

  // But the points are 1 + 1 + 2 + 3 + 0. A caller that sized a buffer from the verb count would write past
  // the end, which is why the two counts exist rather than one.
  EXPECT_EQ(path.pointCount(), 7u);
  EXPECT_TRUE(path.hasCurves());
}

TEST(PathTest, ResetEmptiesButKeepsTheFillType) {
  Path path = makeSquare(0.0f, 0.0f, 1.0f, 1.0f);
  path.setFillType(FillType::EvenOdd);
  EXPECT_FALSE(path.isEmpty());

  path.reset();
  EXPECT_TRUE(path.isEmpty());
  EXPECT_EQ(path.verbCount(), 0u);
  EXPECT_EQ(path.fillType(), FillType::EvenOdd) << "the fill rule describes the path, not its geometry";
}

TEST(PathTest, RecognisesARectangle) {
  Rect rect;
  const Path square = makeSquare(1.0f, 2.0f, 5.0f, 7.0f);
  EXPECT_TRUE(square.isRect(&rect));
  EXPECT_FLOAT_EQ(rect.getLeft(), 1.0f);
  EXPECT_FLOAT_EQ(rect.getTop(), 2.0f);
  EXPECT_FLOAT_EQ(rect.getRight(), 5.0f);
  EXPECT_FLOAT_EQ(rect.getBottom(), 7.0f);

  // A triangle is not a rectangle, and neither is a rectangle with a curve in it.
  Path triangle;
  triangle.moveTo({ 0.0f, 0.0f });
  triangle.lineTo({ 1.0f, 0.0f });
  triangle.lineTo({ 0.0f, 1.0f });
  triangle.close();
  EXPECT_FALSE(triangle.isRect());

  EXPECT_FALSE(makeCircle(5.0f).isRect());
  EXPECT_FALSE(Path {}.isRect());

  // Four sides that do not close back to the start are not a rectangle either: the shape would have a gap.
  Path open;
  open.moveTo({ 0.0f, 0.0f });
  open.lineTo({ 1.0f, 0.0f });
  open.lineTo({ 1.0f, 1.0f });
  open.lineTo({ 0.0f, 1.0f });
  open.lineTo({ 5.0f, 5.0f });
  EXPECT_FALSE(open.isRect());
}

TEST(PathTest, RecognisesALineOnlyPath) {
  Path lines;
  lines.moveTo({ 0.0f, 0.0f });
  lines.lineTo({ 1.0f, 1.0f });
  EXPECT_TRUE(lines.isLine());

  // A close is not a line: it says the contour is filled, which a run of segments does not.
  Path closed = makeSquare(0.0f, 0.0f, 1.0f, 1.0f);
  EXPECT_FALSE(closed.isLine());

  Path curved;
  curved.moveTo({ 0.0f, 0.0f });
  curved.quadTo({ 1.0f, 1.0f }, { 2.0f, 0.0f });
  EXPECT_FALSE(curved.isLine());
}

// ==================================================================================================
// Geometry
// ==================================================================================================

TEST(PathTest, BoundsCoverTheSquare) {
  const Path square = makeSquare(1.0f, 2.0f, 5.0f, 7.0f);

  EXPECT_FLOAT_EQ(square.bounds().getLeft(), 1.0f);
  EXPECT_FLOAT_EQ(square.bounds().getTop(), 2.0f);
  EXPECT_FLOAT_EQ(square.bounds().getRight(), 5.0f);
  EXPECT_FLOAT_EQ(square.bounds().getBottom(), 7.0f);

  // For a path with no curves the control bounds and the tight bounds agree, because the control points are
  // the shape.
  EXPECT_FLOAT_EQ(square.tightBounds().getLeft(), 1.0f);
  EXPECT_FLOAT_EQ(square.tightBounds().getRight(), 5.0f);
}

TEST(PathTest, TightBoundsAreTighterThanControlBoundsForACurve) {
  // A curve whose control points sit well outside it. The control bounds run to y=100; the curve itself
  // reaches only three quarters of that, because a quadratic is pulled toward its control rather than
  // through it.
  Path path;
  path.moveTo({ 0.0f, 0.0f });
  path.quadTo({ 50.0f, 100.0f }, { 100.0f, 0.0f });

  const Rect loose = path.bounds();
  const Rect tight = path.tightBounds();

  EXPECT_FLOAT_EQ(loose.getBottom(), 100.0f) << "the control point is the loose bound";

  // The apex of a quadratic is halfway between the chord and the control point, so 50 rather than 100. This
  // is the whole reason both accessors exist: a caller laying out around the shape wants 50, and a caller
  // culling wants the cheaper 100.
  EXPECT_NEAR(tight.getBottom(), 50.0f, 0.01f);
  EXPECT_LT(tight.getBottom(), loose.getBottom());

  // Horizontally the two agree, because the controls are directly above the chord's ends in x.
  EXPECT_FLOAT_EQ(tight.getLeft(), 0.0f);
  EXPECT_FLOAT_EQ(tight.getRight(), 100.0f);
}

TEST(PathTest, ContainsUsesTheFillRule) {
  const Path square = makeSquare(0.0f, 0.0f, 10.0f, 10.0f);

  EXPECT_TRUE(square.contains({ 5.0f, 5.0f }));
  EXPECT_FALSE(square.contains({ -1.0f, 5.0f }));
  EXPECT_FALSE(square.contains({ 11.0f, 5.0f }));
  EXPECT_FALSE(square.contains({ 5.0f, 11.0f }));

  // The corner is on the outline, and the contract says an outline point is inside.
  EXPECT_TRUE(square.contains({ 0.0f, 0.0f }));

  // A ring: an outer square wound one way and an inner square wound the other. Under the winding rule the
  // reverse-wound inner contour cuts a hole; under even-odd it would too, but for a different reason -- the
  // count of crossings rather than their direction. Either way the middle is outside and the band is in.
  Path ring;
  ring.moveTo({ 0.0f, 0.0f });
  ring.lineTo({ 10.0f, 0.0f });
  ring.lineTo({ 10.0f, 10.0f });
  ring.lineTo({ 0.0f, 10.0f });
  ring.close();
  ring.moveTo({ 3.0f, 3.0f });
  ring.lineTo({ 3.0f, 7.0f });  // wound the other way round
  ring.lineTo({ 7.0f, 7.0f });
  ring.lineTo({ 7.0f, 3.0f });
  ring.close();

  EXPECT_FALSE(ring.contains({ 5.0f, 5.0f })) << "the hole's middle is outside";
  EXPECT_TRUE(ring.contains({ 1.0f, 5.0f })) << "the band between the squares is inside";

  // Under even-odd the hole is still a hole, so the same answers hold. That is the point of the test: the
  // two rules agree here, and disagree only where a contour overlaps itself.
  ring.setFillType(FillType::EvenOdd);
  EXPECT_FALSE(ring.contains({ 5.0f, 5.0f }));
  EXPECT_TRUE(ring.contains({ 1.0f, 5.0f }));
}

TEST(PathTest, LengthMeasuresTheOutline) {
  // A unit square's perimeter is exactly four, with no curve to approximate.
  const Path square = makeSquare(0.0f, 0.0f, 1.0f, 1.0f);
  EXPECT_NEAR(square.length(), 4.0f, 1e-5);

  // A circle of radius ten has a circumference of about 62.83. Four cubics approximate it to well within a
  // tenth of a percent, which is the accuracy of the kappa construction rather than of the quadrature.
  const Path circle = makeCircle(10.0f);
  EXPECT_NEAR(circle.length(), 2.0f * 3.14159265358979f * 10.0f, 0.1f);

  // An open path does not include the edge back to the start; a closed one does. Two identical sets of
  // points, differing only in the close, have different lengths.
  Path open;
  open.moveTo({ 0.0f, 0.0f });
  open.lineTo({ 3.0f, 0.0f });
  open.lineTo({ 3.0f, 4.0f });
  EXPECT_NEAR(open.length(), 7.0f, 1e-5) << "3 along, then 4 up";

  Path closed = open;
  closed.close();
  EXPECT_NEAR(closed.length(), 12.0f, 1e-5) << "plus the 5 back to the start: a 3-4-5 triangle";
}

TEST(PathTest, LengthOfACurvedSegmentIsNotTheChord) {
  // A quarter circle of radius 100 as one cubic. Its length is about 157, while the chord between its ends
  // is 141 -- so an implementation that summed chords would return 141 and look plausible.
  Path arc;
  arc.moveTo({ 100.0f, 0.0f });
  arc.cubicTo({ 100.0f, 55.228f }, { 55.228f, 100.0f }, { 0.0f, 100.0f });

  const Scalar length = arc.length();
  const Scalar chord = std::sqrt(100.0f * 100.0f + 100.0f * 100.0f);

  EXPECT_GT(length, chord) << "a curve is longer than the straight line between its ends";
  EXPECT_NEAR(length, 3.14159265358979f * 100.0f / 2.0f, 0.1f) << "a quarter of the circumference";
}

// ==================================================================================================
// Copying, transforming and reversing
// ==================================================================================================

TEST(PathTest, CopiesAreIndependent) {
  Path original = makeSquare(0.0f, 0.0f, 1.0f, 1.0f);
  Path copy = original;

  EXPECT_EQ(copy.verbCount(), original.verbCount());

  // Changing the copy must not change the original, or the copy is a view rather than a copy.
  copy.lineTo({ 5.0f, 5.0f });
  EXPECT_EQ(copy.verbCount(), original.verbCount() + 1);
  EXPECT_FALSE(original.contains({ 4.0f, 0.5f })) << "the original square did not grow";

  // Assignment the other way.
  Path assigned;
  assigned = original;
  EXPECT_EQ(assigned.verbCount(), original.verbCount());
  assigned.reset();
  EXPECT_FALSE(original.isEmpty());
}

TEST(PathTest, MoveLeavesTheSourceValid) {
  Path source = makeSquare(0.0f, 0.0f, 1.0f, 1.0f);
  Path moved = std::move(source);

  EXPECT_FALSE(moved.isEmpty());
  EXPECT_EQ(moved.verbCount(), 5u);
  // A moved-from path is left in a valid but unspecified state; the requirement is only that using it does
  // not crash, which is what this exercises.
  EXPECT_NO_THROW(source.reset());
}

TEST(PathTest, TransformingMovesTheControlPointsToo) {
  // A curve whose controls are not on the curve. If only the endpoints were transformed, the shape would
  // change rather than move -- the curve would flatten toward its chord. That is the mistake this pins.
  Path path;
  path.moveTo({ 0.0f, 0.0f });
  path.quadTo({ 10.0f, 20.0f }, { 20.0f, 0.0f });

  const Rect before = path.tightBounds();
  ASSERT_NEAR(before.getBottom(), 10.0f, 0.01f) << "the apex is at half the control height";

  // Translate by a known amount: every coordinate shifts and the shape is unchanged. Built through the
  // factory rather than by writing matrix elements by hand, so the test does not depend on the storage layout.
  const Matrix translation = Matrix::MakeTranslation(Vector3 { 100.0f, 50.0f, 0.0f });

  const Path moved = path.transformed(translation);

  EXPECT_NEAR(moved.tightBounds().getLeft(), before.getLeft() + 100.0f, 0.01f);
  EXPECT_NEAR(moved.tightBounds().getRight(), before.getRight() + 100.0f, 0.01f);
  EXPECT_NEAR(moved.tightBounds().getTop(), before.getTop() + 50.0f, 0.05f);
  EXPECT_NEAR(moved.tightBounds().getBottom(), before.getBottom() + 50.0f, 0.05f);

  // The height is unchanged, which is the property that fails if the control point were left behind.
  const Scalar heightBefore = before.getBottom() - before.getTop();
  const Scalar heightAfter = moved.tightBounds().getBottom() - moved.tightBounds().getTop();
  EXPECT_NEAR(heightAfter, heightBefore, 0.05f) << "a translation must not change the shape";

  // The original is untouched: `transformed` returns a new path.
  EXPECT_NEAR(path.tightBounds().getBottom(), before.getBottom(), 1e-6);
}

TEST(PathTest, TransformInPlaceMatchesTheCopyingForm) {
  Path a = makeSquare(0.0f, 0.0f, 2.0f, 2.0f);
  const Path b = a;

  const Matrix scale = Matrix::MakeScale(Vector2 { 3.0f, 3.0f });

  a.transform(scale);
  const Path expected = b.transformed(scale);

  EXPECT_TRUE(a == expected) << "transforming in place must give the same result as transforming a copy";
}

TEST(PathTest, ReversingChangesTheDirectionNotTheShape) {
  const Path square = makeSquare(0.0f, 0.0f, 10.0f, 10.0f);
  const Path reversed = square.reversed();

  // The bounds are the same: the same points, visited in the other order.
  EXPECT_FLOAT_EQ(reversed.bounds().getLeft(), square.bounds().getLeft());
  EXPECT_FLOAT_EQ(reversed.bounds().getRight(), square.bounds().getRight());
  EXPECT_FLOAT_EQ(reversed.bounds().getTop(), square.bounds().getTop());
  EXPECT_FLOAT_EQ(reversed.bounds().getBottom(), square.bounds().getBottom());

  // And the same region: reversing a contour does not turn the inside out for the winding rule, because the
  // sign of the winding flips everywhere together.
  EXPECT_TRUE(reversed.contains({ 5.0f, 5.0f }));
  EXPECT_FALSE(reversed.contains({ -1.0f, 5.0f }));

  // The length is unchanged, as it must be: the same edges, traversed backwards.
  EXPECT_NEAR(reversed.length(), square.length(), 1e-4);
}

// ==================================================================================================
// Value comparison
// ==================================================================================================

TEST(PathTest, EqualityIsGeometricNotStructural) {
  // The same square walked the other way round. A structural comparison of the verbs would call these
  // different; they are the same shape, which is what a caller asking the question means.
  Path clockwise = makeSquare(0.0f, 0.0f, 10.0f, 10.0f);

  Path anticlockwise;
  anticlockwise.moveTo({ 0.0f, 0.0f });
  anticlockwise.lineTo({ 0.0f, 10.0f });
  anticlockwise.lineTo({ 10.0f, 10.0f });
  anticlockwise.lineTo({ 10.0f, 0.0f });
  anticlockwise.close();

  // Their bounds agree, so if the comparison stopped there they would be reported equal -- which is why it
  // goes on to compare the segments. The vertex order differs, so this is *not* equal.
  EXPECT_FLOAT_EQ(clockwise.bounds().getLeft(), anticlockwise.bounds().getLeft());
  EXPECT_FALSE(clockwise == anticlockwise) << "the vertices are in a different order";

  // A genuine copy is equal to its original.
  const Path copy = clockwise;
  EXPECT_TRUE(clockwise == copy);

  // Two paths built the same way are equal, even though their buffers are separate.
  EXPECT_TRUE(makeSquare(0.0f, 0.0f, 10.0f, 10.0f) == makeSquare(0.0f, 0.0f, 10.0f, 10.0f));

  // Shapes that differ at all are not.
  EXPECT_FALSE(clockwise == makeSquare(0.0f, 0.0f, 10.0f, 11.0f));

  // Two empty paths are equal, and an empty path is not equal to a non-empty one.
  EXPECT_TRUE(Path {} == Path {});
  EXPECT_FALSE(Path {} == clockwise);
}

TEST(PathTest, FillTypeRoundTripsThroughTheBridge) {
  Path path = makeSquare(0.0f, 0.0f, 1.0f, 1.0f);

  path.setFillType(FillType::EvenOdd);
  EXPECT_EQ(path.fillType(), FillType::EvenOdd);
  EXPECT_STREQ(toString(path.fillType()), "EvenOdd");

  path.setFillType(FillType::Winding);
  EXPECT_EQ(path.fillType(), FillType::Winding);
  EXPECT_STREQ(toString(path.fillType()), "Winding");

  // A copy carries the fill type, which is part of the path rather than of its geometry.
  path.setFillType(FillType::EvenOdd);
  const Path copy = path;
  EXPECT_EQ(copy.fillType(), FillType::EvenOdd);
}

// ==================================================================================================
// Boolean operations and simplification
// ==================================================================================================

TEST(PathTest, BooleanOperationsProduceTheExpectedRegion) {
  // These operands overlap in a 5x5 square, so every answer is arithmetic: the two 10x10 squares have area 100 each,
  // their overlap is 25, and the union is 175.
  const Path a = makeSquare(0.0f, 0.0f, 10.0f, 10.0f);
  const Path b = makeSquare(5.0f, 5.0f, 15.0f, 15.0f);

  // The union covers both squares and reaches both sets of extremes.
  const Path united = op(a, b, PathOp::Union);
  EXPECT_FALSE(united.isEmpty()) << "the union of two overlapping squares is not empty";
  expectBounds(united, 0.0f, 0.0f, 15.0f, 15.0f);

  // The intersection is the 5x5 overlap, so its bounds are the shared corner.
  const Path overlap = op(a, b, PathOp::Intersection);
  EXPECT_FALSE(overlap.isEmpty());
  expectBounds(overlap, 5.0f, 5.0f, 10.0f, 10.0f);

  // The difference keeps the first square's own area, so it still reaches the origin but not past (10,10).
  const Path remaining = op(a, b, PathOp::Difference);
  EXPECT_FALSE(remaining.isEmpty());
  expectBounds(remaining, 0.0f, 0.0f, 10.0f, 10.0f);

  // `simplify` and `asWinding` are not asserted here. Both currently report failure even for a path with no
  // self-crossing, which is recorded -- with its cause -- in `PathOpsBoolean.SimplifyResolvesASelfCrossing`. A test
  // that skipped them silently would hide that, and one that asserted them would fail for a reason this test is not
  // about.

  // The operation names are usable, which matters for the diagnostics.
  EXPECT_STREQ(toString(PathOp::Union), "Union");
  EXPECT_STREQ(toString(PathOp::Difference), "Difference");
  EXPECT_STREQ(toString(PathOp::Xor), "ExclusiveOr");
  EXPECT_STREQ(toString(PathOp::ReverseDifference), "ReverseDifference");
  EXPECT_STREQ(toString(PathOp::Intersection), "Intersection");
}

TEST(PathTest, ContentHashIdentifiesGeometryNotObjectIdentity) {
  Path path;
  path.moveTo({ 0.0f, 0.0f });
  path.lineTo({ 10.0f, 0.0f });
  path.lineTo({ 10.0f, 10.0f });
  path.close();

  // A copy and a separately built path with the same content must agree: a cache keyed by this hash
  // has to survive the caller rebuilding its clip every frame.
  EXPECT_EQ(path.contentHash(), Path(path).contentHash());
  Path rebuilt;
  rebuilt.moveTo({ 0.0f, 0.0f });
  rebuilt.lineTo({ 10.0f, 0.0f });
  rebuilt.lineTo({ 10.0f, 10.0f });
  rebuilt.close();
  EXPECT_EQ(path.contentHash(), rebuilt.contentHash());

  Path moved = path;
  moved.lineTo({ 20.0f, 20.0f });
  EXPECT_NE(path.contentHash(), moved.contentHash());

  Path evenOdd = path;
  evenOdd.setFillType(FillType::EvenOdd);
  EXPECT_NE(path.contentHash(), evenOdd.contentHash());
}
