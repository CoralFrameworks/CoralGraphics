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

#include <graphics/SdfGlyph.h>

#include <graphics/Bitmap.h>
#include <graphics/Canvas.h>
#include <graphics/Color.h>
#include <graphics/Paint.h>
#include <graphics/Pixmap.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace graphics {
namespace {

struct BoundaryPoint {
  float x = 0.0f;
  float y = 0.0f;
};

} // namespace

SdfGlyph makeSdfGlyph(const Path& devicePath, const float padding, const float spread) {
  SdfGlyph result;
  if (devicePath.isEmpty() || padding <= 0.0f) {
    return result;
  }
  const Rect bounds = devicePath.tightBounds();
  if (bounds.getRight() <= bounds.getLeft() || bounds.getBottom() <= bounds.getTop()) {
    return result;
  }
  const int left = static_cast<int>(std::floor(bounds.getLeft() - padding));
  const int top = static_cast<int>(std::floor(bounds.getTop() - padding));
  const int right = static_cast<int>(std::ceil(bounds.getRight() + padding));
  const int bottom = static_cast<int>(std::ceil(bounds.getBottom() + padding));
  const int width = right - left;
  const int height = bottom - top;
  if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
    return result;
  }

  Bitmap coverage = Bitmap::allocate(width, height);
  auto canvas = Canvas::create(coverage);
  if (coverage.isEmpty() || canvas == nullptr) {
    return result;
  }
  canvas->translate(static_cast<Scalar>(-left), static_cast<Scalar>(-top));
  Paint paint;
  paint.color = Color(1.0f, 1.0f, 1.0f, 1.0f);
  paint.antiAlias = true;
  canvas->drawPath(devicePath, paint);
  canvas.reset();

  const Pixmap pixels = coverage.makePixmap();
  if (pixels.isEmpty()) {
    return result;
  }

  // Classify the pixels and collect the outline: an inside pixel with an outside neighbour is an edge.
  std::vector<uint8_t> inside(static_cast<size_t>(width) * height);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const auto* pixel = static_cast<const uint8_t*>(pixels.addr(x, y));
      inside[static_cast<size_t>(y) * width + x] = pixel[3] >= 128 ? 1 : 0;
    }
  }

  // A second pass, because the neighbour lookup needs the whole mask: filling and testing in one pass
  // read the right and lower neighbours as outside, which made every inside pixel an edge pixel.
  std::vector<BoundaryPoint> boundary;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      if (inside[static_cast<size_t>(y) * width + x] == 0) {
        continue;
      }
      bool edge = x == 0 || y == 0 || x == width - 1 || y == height - 1;
      if (!edge && (inside[static_cast<size_t>(y) * width + x - 1] == 0 ||
                    inside[static_cast<size_t>(y) * width + x + 1] == 0 ||
                    inside[static_cast<size_t>(y - 1) * width + x] == 0 ||
                    inside[static_cast<size_t>(y + 1) * width + x] == 0)) {
        edge = true;
      }
      if (edge) {
        boundary.push_back(BoundaryPoint { static_cast<float>(x) + 0.5f,
          static_cast<float>(y) + 0.5f });
      }
    }
  }
  if (boundary.empty()) {
    return result;
  }

  // A two-pass chamfer distance transform: the same field as a brute-force nearest search, in linear
  // time. One pass propagates the distances from the top-left, one from the bottom-right, and the
  // diagonal weight approximates the Euclidean distance closely enough for an antialiased edge.
  constexpr float kStraight = 1.0f;
  constexpr float kDiagonal = 1.41421356f;
  constexpr float kFar = 1.0e9f;
  std::vector<float> distances(static_cast<size_t>(width) * height, kFar);
  for (const BoundaryPoint& point: boundary) {
    const int x = static_cast<int>(point.x);
    const int y = static_cast<int>(point.y);
    if (x >= 0 && y >= 0 && x < width && y < height) {
      distances[static_cast<size_t>(y) * width + x] = 0.0f;
    }
  }
  auto at = [&distances, width](const int x, const int y) -> float& {
    return distances[static_cast<size_t>(y) * width + x];
  };
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      float value = at(x, y);
      if (x > 0) value = std::min(value, at(x - 1, y) + kStraight);
      if (y > 0) value = std::min(value, at(x, y - 1) + kStraight);
      if (x > 0 && y > 0) value = std::min(value, at(x - 1, y - 1) + kDiagonal);
      if (x + 1 < width && y > 0) value = std::min(value, at(x + 1, y - 1) + kDiagonal);
      at(x, y) = value;
    }
  }
  for (int y = height - 1; y >= 0; --y) {
    for (int x = width - 1; x >= 0; --x) {
      float value = at(x, y);
      if (x + 1 < width) value = std::min(value, at(x + 1, y) + kStraight);
      if (y + 1 < height) value = std::min(value, at(x, y + 1) + kStraight);
      if (x + 1 < width && y + 1 < height) value = std::min(value, at(x + 1, y + 1) + kDiagonal);
      if (x > 0 && y + 1 < height) value = std::min(value, at(x - 1, y + 1) + kDiagonal);
      at(x, y) = value;
    }
  }

  result.width = static_cast<uint32_t>(width);
  result.height = static_cast<uint32_t>(height);
  result.padding = padding;
  result.left = static_cast<float>(left);
  result.top = static_cast<float>(top);
  result.distance.resize(static_cast<size_t>(width) * height);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t index = static_cast<size_t>(y) * width + x;
      const float nearest = std::min(at(x, y), padding);
      const float signedDistance = inside[index] != 0 ? nearest : -nearest;
      const float normalised = std::clamp(signedDistance / padding, -1.0f, 1.0f);
      result.distance[index] = static_cast<uint8_t>(std::lround(128.0f + normalised * 127.0f));
    }
  }  static_cast<void>(spread);
  return result;
}

} // namespace graphics
