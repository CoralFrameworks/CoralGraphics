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
 * @file test_LottieParser.cpp
 *
 * Reading Lottie documents.
 *
 * Most of these use the corpus's own `gear.json`, because a parser tested only against hand-written JSON tends to
 * understand exactly the subset its author had in mind. The assertions are on the values the file states -- measured out
 * of it first and written down here -- so a parser that quietly gave up on a field would fail rather than produce a
 * document that merely renders differently.
 */

#include "lottie/LottieModel.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifndef CORAL_GRAPHICS_RESOURCE_DIR
#error "CORAL_GRAPHICS_RESOURCE_DIR must be defined by the build; see CMakeLists.txt"
#endif

namespace {

namespace fs = std::filesystem;

using graphics::lottie::Animation;
using graphics::lottie::MergeMode;
using graphics::lottie::parseAnimation;
using graphics::lottie::Shape;
using graphics::lottie::ShapeKind;
using graphics::lottie::VertexPath;

/** The corpus document, parsed. */
Animation gear(std::string* error = nullptr) {
  const fs::path path = fs::path(CORAL_GRAPHICS_RESOURCE_DIR) / "apitest" / "lottie" / "gear.json";
  std::ifstream file(path, std::ios::binary);
  std::ostringstream contents;
  contents << file.rdbuf();

  auto parsed = parseAnimation(contents.str(), error);
  EXPECT_TRUE(parsed.has_value()) << (error != nullptr ? *error : "no reason given");
  return parsed.has_value() ? std::move(*parsed) : Animation {};
}

/** Walks the tree and counts the items of one kind. */
int countKind(const std::vector<graphics::lottie::ShapePtr>& items, const ShapeKind kind) {
  int count = 0;
  for (const auto& item : items) {
    if (item->kind == kind) ++count;
    count += countKind(item->items, kind);
  }
  return count;
}

const Shape* findKind(const std::vector<graphics::lottie::ShapePtr>& items, const ShapeKind kind) {
  for (const auto& item : items) {
    if (item->kind == kind) return item.get();
    if (const Shape* found = findKind(item->items, kind)) return found;
  }
  return nullptr;
}

} // namespace anonymous

// ==================================================================================================
// The composition
// ==================================================================================================

TEST(LottieParser, ReadsTheComposition) {
  const Animation animation = gear();
  EXPECT_EQ(animation.name, "Comp 1");
  EXPECT_FLOAT_EQ(animation.width, 128.0f);
  EXPECT_FLOAT_EQ(animation.height, 128.0f);
  EXPECT_FLOAT_EQ(animation.frameRate, 60.0f);
  EXPECT_FLOAT_EQ(animation.inPoint, 0.0f);
  EXPECT_FLOAT_EQ(animation.outPoint, 300.0f);
  // 300 frames at 60 frames a second is five seconds, which is what a player counts in.
  EXPECT_FLOAT_EQ(animation.duration(), 5.0f);
}

TEST(LottieParser, ReadsTheLayerAndItsTransform) {
  const Animation animation = gear();
  ASSERT_EQ(animation.layers.size(), 1U);

  const auto& layer = animation.layers.front();
  EXPECT_EQ(layer.name, "GearHollowAltWeb Outlines");
  EXPECT_FLOAT_EQ(layer.inPoint, 0.0f);
  EXPECT_FLOAT_EQ(layer.outPoint, 300.0f);

  EXPECT_FLOAT_EQ(layer.transform.position.at(0).x, 64.0f);
  EXPECT_FLOAT_EQ(layer.transform.position.at(0).y, 64.0f);
  EXPECT_FLOAT_EQ(layer.transform.anchor.at(0).x, 320.0f);
  EXPECT_FLOAT_EQ(layer.transform.anchor.at(0).y, 320.0f);

  // The scale is in percent and stays in percent in the model, so this reads as the file does.
  const auto scale = layer.transform.scale.at(0);
  ASSERT_EQ(scale.size(), 3U);
  EXPECT_NEAR(scale[0], 11.562f, 0.001f);
  EXPECT_NEAR(scale[1], 11.562f, 0.001f);
  EXPECT_FLOAT_EQ(scale[2], 100.0f);

  EXPECT_FLOAT_EQ(layer.transform.opacityAt(0), 1.0f) << "100% is fully opaque";
}

TEST(LottieParser, ReadsTheRotationAsAnimated) {
  // The gear's rotation is the one animated property in the document, so a parser that dropped the keyframes would give
  // a gear that never moves -- and one that read only the first keyframe would give a gear that is correctly upright.
  const Animation animation = gear();
  ASSERT_EQ(animation.layers.size(), 1U);

  const auto& rotation = animation.layers.front().transform.rotation;
  EXPECT_TRUE(rotation.isAnimated());
  EXPECT_GE(rotation.keyframes().size(), 2U);
  EXPECT_FLOAT_EQ(rotation.at(0), 0.0f) << "upright at the start";
  EXPECT_GT(rotation.at(150), 90.0f) << "and well past a quarter turn by the middle";
  EXPECT_NEAR(rotation.at(300), 360.0f, 1.0f) << "a full turn over the composition";
}

TEST(LottieParser, TheRotationTurnsAtAConstantRateInThisDocument) {
  // Measured out of the file rather than assumed: both of the gear's rotation keyframes carry `o = (0.167, 0.167)` and
  // `i = (0.833, 0.833)`, and a curve whose control points have equal coordinates on both axes is the identity. So the
  // gear turns at a constant rate, and a reader that invented an ease would move it to the wrong angle everywhere
  // between the two keyframes.
  //
  // Note the times: the keyframes are at 0 and 299, not at 0 and 300. The value at the composition's last frame is
  // therefore 360 by interpolation rather than by arriving.
  const Animation animation = gear();
  const auto& rotation = animation.layers.front().transform.rotation;

  const auto& frames = rotation.keyframes();
  ASSERT_EQ(frames.size(), 2U);
  EXPECT_FLOAT_EQ(frames.front().time, 0.0f);
  EXPECT_FLOAT_EQ(frames.back().time, 299.0f);
  EXPECT_FLOAT_EQ(frames.front().outTangent.x, frames.front().outTangent.y) << "the identity curve";
  EXPECT_FLOAT_EQ(frames.front().inTangent.x, frames.front().inTangent.y);

  // Which means a quarter of the time is a quarter of the turn.
  EXPECT_NEAR(rotation.at(299.0f * 0.25f), 90.0f, 0.5f);
  EXPECT_NEAR(rotation.at(299.0f * 0.75f), 270.0f, 0.5f);
  EXPECT_FLOAT_EQ(rotation.at(150), 360.0f * 150.0f / 299.0f);
}

TEST(LottieParser, HonoursANonLinearEase) {
  // The other half of the claim, which the gear cannot make because it is linear: a keyframe whose control points are
  // not the identity has to produce a value off the straight line. `cubic-bezier(0.42, 0, 1, 1)` at a quarter of the
  // time is about 9.3% of the change, where linear would be 25%.
  const char* document = R"({
    "w": 100, "h": 100, "fr": 60, "ip": 0, "op": 100,
    "layers": [{"ty": 4, "nm": "shapes", "ks": {"r": {"a": 1, "k": [
        {"t": 0, "s": [0], "o": {"x": [0.42], "y": [0]}, "i": {"x": [1], "y": [1]}},
        {"t": 100, "s": [100]}]}}}]})";
  const auto parsed = parseAnimation(document);
  ASSERT_TRUE(parsed.has_value());

  const auto& rotation = parsed->layers.front().transform.rotation;
  EXPECT_NEAR(rotation.at(25), 9.3f, 1.5f) << "the eased value";
  EXPECT_LT(rotation.at(25), 15.0f) << "and nowhere near the linear 25";
  EXPECT_FLOAT_EQ(rotation.at(100), 100.0f) << "the endpoint is reached regardless";
}

// ==================================================================================================
// The shapes
// ==================================================================================================

TEST(LottieParser, ReadsTheShapeGroups) {
  const Animation animation = gear();
  ASSERT_EQ(animation.layers.size(), 1U);
  const auto& shapes = animation.layers.front().shapes;

  ASSERT_EQ(shapes.size(), 2U) << "the gear is two groups";
  EXPECT_EQ(shapes[0]->kind, ShapeKind::Group);
  EXPECT_EQ(shapes[0]->name, "Group 1");
  EXPECT_EQ(shapes[1]->name, "Group 2");

  // Each group holds two paths, a merge, a fill and a transform.
  EXPECT_EQ(countKind(shapes, ShapeKind::Path), 4);
  EXPECT_EQ(countKind(shapes, ShapeKind::Merge), 2);
  EXPECT_EQ(countKind(shapes, ShapeKind::Fill), 2);
  EXPECT_EQ(countKind(shapes, ShapeKind::Transform), 2);
}

TEST(LottieParser, ReadsTheMergeMode) {
  // `mm: 1` is the splice, and it is what makes the gear hollow: a union of the two contours would fill the ring in.
  const Animation animation = gear();
  const Shape* merge = findKind(animation.layers.front().shapes, ShapeKind::Merge);
  ASSERT_NE(merge, nullptr);
  EXPECT_EQ(merge->merge, MergeMode::kMerge);
}

TEST(LottieParser, ReadsTheFill) {
  const Animation animation = gear();
  const Shape* fill = findKind(animation.layers.front().shapes, ShapeKind::Fill);
  ASSERT_NE(fill, nullptr);

  const graphics::Color colour = fill->color.at(0);
  EXPECT_FLOAT_EQ(colour.red, 0.0f) << "the gear is black";
  EXPECT_FLOAT_EQ(colour.green, 0.0f);
  EXPECT_FLOAT_EQ(colour.blue, 0.0f);
  EXPECT_FLOAT_EQ(colour.alpha, 1.0f);
  EXPECT_EQ(fill->rule, graphics::FillType::Winding) << "r: 1 is the nonzero rule";
}

TEST(LottieParser, ReadsTheGroupTransform) {
  // A group's `tr` is an item inside it rather than a field beside it, and the group's own transform is that item.
  const Animation animation = gear();
  const auto& group = animation.layers.front().shapes.front();
  EXPECT_FLOAT_EQ(group->transform.position.at(0).x, 320.0f);
  EXPECT_NEAR(group->transform.position.at(0).y, 319.95f, 0.01f);
  EXPECT_FLOAT_EQ(group->transform.scale.at(0).at(0), 100.0f) << "the group is not scaled";
}

TEST(LottieParser, ReadsTheVertexPaths) {
  const Animation animation = gear();
  const Shape* path = findKind(animation.layers.front().shapes, ShapeKind::Path);
  ASSERT_NE(path, nullptr);
  ASSERT_TRUE(path->path.isEmpty() == false);

  const VertexPath vertices = path->path.at(0);
  EXPECT_EQ(vertices.size(), 28U) << "the first path's vertex count, from the file";
  EXPECT_EQ(vertices.inTangents.size(), 28U);
  EXPECT_EQ(vertices.outTangents.size(), 28U);
  EXPECT_TRUE(vertices.closed);

  // The first vertex is where the file says it is.
  EXPECT_NEAR(vertices.vertices[0].x, 255.3f, 0.01f);
  EXPECT_NEAR(vertices.vertices[0].y, 133.25f, 0.01f);
  // And its incoming tangent, which the file stores relative to that vertex.
  EXPECT_NEAR(vertices.inTangents[0].x, 8.101f, 0.01f);
  EXPECT_NEAR(vertices.inTangents[0].y, -15.5f, 0.01f);
}

TEST(LottieParser, TurnsAVertexPathIntoAClosedCubicOutline) {
  const Animation animation = gear();
  const Shape* path = findKind(animation.layers.front().shapes, ShapeKind::Path);
  ASSERT_NE(path, nullptr);

  const graphics::Path outline = path->path.at(0).toPath();
  EXPECT_FALSE(outline.isEmpty());
  // A closed path of 28 vertices is 28 cubics plus the move and the close: one verb per edge and two more.
  EXPECT_EQ(outline.verbCount(), 28U + 2U);
  EXPECT_TRUE(outline.hasCurves()) << "the vertices carry tangents, so the outline is curved";
}

TEST(LottieParser, AVerticesOutlineReachesTheVerticesItWasBuiltFrom) {
  // The tangents are stored relative to their vertex, so a reader that forgot to add them would put every control point
  // at the origin and produce a shape whose outline loops through the middle of the composition. The bounds catch that.
  const Animation animation = gear();
  const Shape* path = findKind(animation.layers.front().shapes, ShapeKind::Path);
  ASSERT_NE(path, nullptr);

  const VertexPath vertices = path->path.at(0);
  const graphics::Rect bounds = path->path.at(0).toPath().bounds();

  graphics::Scalar minX = vertices.vertices.front().x;
  graphics::Scalar minY = vertices.vertices.front().y;
  for (const graphics::Point& vertex : vertices.vertices) {
    minX = std::min(minX, vertex.x);
    minY = std::min(minY, vertex.y);
  }

  // The control hull cannot be smaller than the vertices, and it is not wildly larger: a relative-tangent mistake would
  // put points at zero, which is far outside this path's own coordinates.
  EXPECT_LE(bounds.getLeft(), minX + 1.0f);
  EXPECT_LE(bounds.getTop(), minY + 1.0f);
  EXPECT_GE(bounds.getRight(), minX) << "the hull encloses the vertices";
}

// ==================================================================================================
// Degenerate documents
// ==================================================================================================

TEST(LottieParser, RefusesTextThatIsNotJson) {
  std::string error;
  EXPECT_FALSE(parseAnimation("{not json", &error).has_value());
  EXPECT_FALSE(error.empty()) << "a reason is given";
}

TEST(LottieParser, RefusesADocumentWithNoLayers) {
  std::string error;
  EXPECT_FALSE(parseAnimation(R"({"w":10,"h":10,"fr":60,"ip":0,"op":10,"layers":[]})", &error).has_value());
  EXPECT_NE(error.find("layers"), std::string::npos);
}

TEST(LottieParser, RefusesADocumentWithNoSize) {
  std::string error;
  EXPECT_FALSE(parseAnimation(R"({"w":0,"h":0,"fr":60,"ip":0,"op":10,"layers":[{"ty":4}]})", &error).has_value());
  EXPECT_NE(error.find("size"), std::string::npos);
}

TEST(LottieParser, AnUnsupportedLayerKindDoesNotFailTheDocument) {
  // A precomp needs a source this renderer does not have. It must not fail the document, and the shape layer beside it
  // must still be there -- but the precomp itself is *kept*, as `graphics::lottie::Layer::Kind::Other`, not dropped.
  //
  // Dropping it was the earlier behaviour and it was wrong for a reason that is easy to miss: removing a layer renumbers
  // the ones after it, and a track matte is written as "the layer above me". A dropped layer therefore re-points a matte
  // at a different layer, silently, and the picture changes for a reason nothing in the document says.
  const char* document = R"({
    "w": 10, "h": 10, "fr": 60, "ip": 0, "op": 10,
    "layers": [{"ty": 0, "nm": "a precomp"}, {"ty": 4, "nm": "shapes"}]})";
  const auto parsed = parseAnimation(document);
  ASSERT_TRUE(parsed.has_value());

  ASSERT_EQ(parsed->layers.size(), 2U) << "the precomp keeps its place in the list";
  EXPECT_EQ(parsed->layers[0].name, "a precomp");
  EXPECT_EQ(parsed->layers[0].kind, graphics::lottie::Layer::Kind::Other) << "recorded as a kind that draws nothing";
  EXPECT_EQ(parsed->layers[1].name, "shapes");
  EXPECT_EQ(parsed->layers[1].kind, graphics::lottie::Layer::Kind::Shape) << "and the shape layer still draws";
}

TEST(LottieParser, DefaultsAFrameRateOfZeroRatherThanDividingByIt) {
  const char* document = R"({
    "w": 10, "h": 10, "fr": 0, "ip": 0, "op": 60,
    "layers": [{"ty": 4, "nm": "shapes"}]})";
  const auto parsed = parseAnimation(document);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FLOAT_EQ(parsed->frameRate, 60.0f);
  EXPECT_FLOAT_EQ(parsed->duration(), 1.0f) << "sixty frames at sixty a second";
}

TEST(LottieParser, DefaultsAMissingOpacityToOpaque) {
  // The format's own initial value is 100%, and a default-constructed property would give zero -- which would make a
  // layer that omitted `o` invisible.
  const char* document = R"({
    "w": 10, "h": 10, "fr": 60, "ip": 0, "op": 10,
    "layers": [{"ty": 4, "nm": "shapes", "ks": {"p": {"a": 0, "k": [1, 2]}}}]})";
  const auto parsed = parseAnimation(document);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FLOAT_EQ(parsed->layers.front().transform.opacityAt(0), 1.0f);
  EXPECT_FLOAT_EQ(parsed->layers.front().transform.position.at(0).x, 1.0f);
  EXPECT_FLOAT_EQ(parsed->layers.front().transform.scale.at(0).at(0), 100.0f) << "an absent scale is 100%";
}

TEST(LottieParser, ReadsAnAnimatedPositionWithoutFreezingIt) {
  // The bug this rules out: treating a vector property as a constant by taking its first keyframe. The layer would be in
  // the right place at frame zero and nowhere else.
  const char* document = R"({
    "w": 100, "h": 100, "fr": 60, "ip": 0, "op": 60,
    "layers": [{"ty": 4, "nm": "shapes", "ks": {"p": {"a": 1, "k": [
        {"t": 0, "s": [0, 0]},
        {"t": 60, "s": [100, 50]}]}}}]})";
  const auto parsed = parseAnimation(document);
  ASSERT_TRUE(parsed.has_value());

  const auto& position = parsed->layers.front().transform.position;
  EXPECT_TRUE(position.isAnimated());
  EXPECT_FLOAT_EQ(position.at(0).x, 0.0f);
  EXPECT_FLOAT_EQ(position.at(30).x, 50.0f) << "halfway";
  EXPECT_FLOAT_EQ(position.at(30).y, 25.0f);
  EXPECT_FLOAT_EQ(position.at(60).x, 100.0f);
}

TEST(LottieParser, AStaticPropertyNeedsNoCurve) {
  // The common case: most properties of most layers never move, and asking one for its value at a time it was never
  // given should answer the value rather than interpolate towards nothing.
  const char* document = R"({
    "w": 10, "h": 10, "fr": 60, "ip": 0, "op": 10,
    "layers": [{"ty": 4, "nm": "shapes", "ks": {"r": {"a": 0, "k": 45}}}]})";
  const auto parsed = parseAnimation(document);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FALSE(parsed->layers.front().transform.rotation.isAnimated());
  EXPECT_FLOAT_EQ(parsed->layers.front().transform.rotation.at(500), 45.0f);
}

TEST(LottieParser, ReadsAPropertyWhoseValueIsAOneElementArray) {
  // Exporters write a scalar either way, and a reader that only accepted a bare number would silently take the default.
  const char* document = R"({
    "w": 10, "h": 10, "fr": 60, "ip": 0, "op": 10,
    "layers": [{"ty": 4, "nm": "shapes", "ks": {"r": {"a": 0, "k": [30]}}}]})";
  const auto parsed = parseAnimation(document);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FLOAT_EQ(parsed->layers.front().transform.rotation.at(0), 30.0f);
}
