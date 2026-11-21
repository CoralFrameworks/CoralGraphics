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

#include "demo_scene.h"

#include <graphics/Paint.h>
#include <graphics/Path.h>
#include <geometry/Rect.h>

#include <algorithm>
#include <cmath>

namespace demo {

namespace {

graphics::Color rgb(const uint8_t r, const uint8_t g, const uint8_t b) {
  return graphics::Color::MakeRGBA8(r, g, b, 255);
}

} // namespace anonymous

void drawFrame(graphics::Canvas& canvas, const uint32_t width, const uint32_t height, const double seconds) {
  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);
  canvas.clear(rgb(14, 18, 28));

  graphics::Paint background;
  background.color = rgb(22, 28, 44);
  canvas.drawRect(graphics::Rect::MakeLTRB(0.0f, 0.0f, w, h), background);

  // A horizon-like bar, so an inverted or badly scaled frame is obvious rather than plausible.
  graphics::Paint bar;
  bar.color = rgb(32, 42, 64);
  canvas.drawRect(graphics::Rect::MakeLTRB(0.0f, h * 0.58f, w, h), bar);

  const float pulse = static_cast<float>(0.5 + 0.5 * std::sin(seconds * 1.7));
  const float radius = std::max(18.0f, std::min(w, h) * (0.06f + 0.02f * pulse));
  const float cx = w * (0.30f + 0.16f * static_cast<float>(std::sin(seconds * 0.7)));
  const float cy = h * (0.38f + 0.10f * static_cast<float>(std::cos(seconds * 0.9)));

  graphics::Paint sun;
  sun.color = rgb(255, 176, 64);
  canvas.drawCircle(cx, cy, radius, sun);

  graphics::Paint inner;
  inner.color = rgb(255, 226, 148);
  canvas.drawCircle(cx, cy, radius * 0.55f, inner);

  // Three bars with different phases: the animation is visible even on a frame with no text.
  for (int index = 0; index < 3; ++index) {
    const float phase = seconds * (0.6 + 0.23 * index) + index * 1.7f;
    const float barW = w * 0.10f;
    const float barH = h * (0.10f + 0.03f * std::sin(phase));
    const float x = w * (0.60f + 0.09f * index);
    const float y = h * 0.70f - barH;

    canvas.save();
    canvas.translate(x, y);
    canvas.rotate(static_cast<float>(std::sin(phase)) * 8.0f);
    graphics::Paint paint;
    paint.color = rgb(static_cast<uint8_t>(54 + index * 42), static_cast<uint8_t>(180 - index * 34),
      static_cast<uint8_t>(220 - index * 46));
    canvas.drawRect(graphics::Rect::MakeLTRB(0.0f, 0.0f, barW, barH), paint);
    canvas.restore();
  }

  // One stroked path: it exercises the path/stroke route rather than only axis-aligned rectangles.
  graphics::Path wave;
  wave.moveTo(graphics::Point { w * 0.08f, h * 0.86f });
  for (int step = 1; step <= 24; ++step) {
    const float t = static_cast<float>(step) / 24.0f;
    const float x = w * (0.08f + 0.84f * t);
    const float y = h * (0.86f + 0.035f * std::sin(t * 6.2831853f * 2.0f + static_cast<float>(seconds) * 2.4f));
    wave.lineTo(graphics::Point { x, y });
  }
  graphics::Paint stroke;
  stroke.style = graphics::Paint::Style::kStroke;
  stroke.strokeWidth = std::max(2.0f, h * 0.006f);
  stroke.strokeCap = graphics::Paint::Cap::kRound;
  stroke.color = rgb(96, 220, 190);
  canvas.drawPath(wave, stroke);
}

} // namespace demo
