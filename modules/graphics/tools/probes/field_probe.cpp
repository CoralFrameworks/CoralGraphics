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

/**
 * @file field_probe.cpp
 *
 * Where the Live demo's 35 ms of coverage-field time goes, measured on the desktop.
 *
 * The device numbers that prompted this probe: at 2528x1291 the direct path ran at ~26 fps with
 * `stages: fields 35.69 | ... | execute 0.69 | total 36.60 ms` -- so the frame was not vsync-bound
 * (`Immediate` changed it by 1 ms) and not fill-bound (`execute` was under a millisecond). The cost
 * was coverage-field generation, which is CPU work: antialiased shapes are rasterised into coverage
 * texels and packed into an atlas before the GPU ever sees them.
 *
 * The probe renders the same scene at the same size and reports `fields` for variants that remove one
 * contributor at a time, then for a run with a worker pool. That turns "35 ms in fields" into "the
 * line costs X, the circle Y, the rotated square Z, and threading divides it by N".
 *
 * Run it from the build directory: `./graphics/field_probe` (Debug or Release).
 */

#include <gpu/Surface.h>

#include <geometry/Rect.h>
#include <graphics/Canvas.h>
#include <graphics/Paint.h>

#include <utils/JobSystem.h>

#include <cstdio>
#include <memory>

namespace {

constexpr int kWidth = 2528;
constexpr int kHeight = 1291;

/** The Live demo's scene, with each antialiased contributor switchable. */
struct Scene {
  bool antiAlias = true;
  bool line = true;
  bool circle = true;
  bool rotatedSquare = true;
};

void drawScene(graphics::Canvas* canvas, const Scene& scene) {
  const float width = static_cast<float>(kWidth);
  const float height = static_cast<float>(kHeight);
  const float unit = static_cast<float>(kHeight) / 8.0f;
  const float centreX = width / 2.0f;
  const float centreY = height / 2.0f;

  graphics::Paint background;
  background.color = graphics::Color::MakeRGBA8(0x10, 0x18, 0x20, 0xFF);
  canvas->clear(background.color);

  graphics::Paint panel;
  panel.color = graphics::Color::MakeRGBA8(0x14, 0x32, 0x4A, 0xFF);
  canvas->drawRect(graphics::Rect::MakeLTRB(0.0f, centreY, width, height), panel);

  if (scene.rotatedSquare) {
    // Rotated, so this is a path rather than an axis-aligned fill: it needs coverage.
    canvas->save();
    canvas->translate(centreX, centreY);
    canvas->rotate(0.0f);
    graphics::Paint square;
    square.color = graphics::Color::MakeRGBA8(0x2E, 0x86, 0xDE, 0xFF);
    square.antiAlias = scene.antiAlias;
    canvas->drawRect(graphics::Rect::MakeLTRB(-unit * 1.2f, -unit * 1.2f, unit * 1.2f, unit * 1.2f), square);
    canvas->restore();
  }

  if (scene.circle) {
    graphics::Paint dot;
    dot.color = graphics::Color::MakeRGBA8(0xFF, 0x6B, 0x6B, 0xFF);
    dot.antiAlias = scene.antiAlias;
    canvas->drawCircle(centreX + unit * 2.4f, centreY, unit * 0.7f, dot);
  }

  if (scene.line) {
    // The widest antialiased stroke in the scene: it spans the entire width.
    graphics::Paint bar;
    bar.color = graphics::Color::MakeRGBA8(0xFF, 0xFF, 0xFF, 0xFF);
    bar.style = graphics::Paint::Style::kStroke;
    bar.strokeWidth = 4.0f;
    bar.antiAlias = scene.antiAlias;
    canvas->drawLine(graphics::Point::MakeXY(0.0f, centreY), graphics::Point::MakeXY(width, centreY), bar);
  }
}

/**
 * Renders @p scene twice with the second frame nudged by a pixel, and prints both.
 *
 * The nudge is the point: coverage fields are cached and reused when a shape's geometry repeats, so a probe that draws the
 * same scene twice measures the *cache*, not the generation. One pixel of movement is enough to miss the reuse window and
 * pay for the field again.
 */
void measure(graphics::gpu::Surface& surface, const char* label, const Scene& scene) {
  double regenerated = 0.0;
  double total = 0.0;
  uint64_t area = 0;
  uint32_t fields = 0;
  for (int pass = 0; pass < 2; ++pass) {
    graphics::Canvas* canvas = surface.beginFrame();
    if (canvas == nullptr) {
      std::printf("%-28s no canvas\n", label);
      return;
    }
    const Scene shifted = scene;
    canvas->translate(static_cast<float>(pass), 0.0f);
    drawScene(canvas, shifted);
    if (!surface.submit()) {
      std::printf("%-28s submit refused\n", label);
      return;
    }
    // The second pass is the one no cache can serve.
    if (pass == 1) {
      const graphics::gpu::FrameStages stages = surface.lastFrameStages();
      regenerated = stages.fields;
      total = stages.total;
      area = surface.lastFrameCoverageFieldArea();
      fields = surface.lastFrameCoverageFieldCount();
    }
  }
  std::printf("%-28s fields %7.2f ms   total %7.2f ms   %u field(s), %llu px   batch %s\n", label, regenerated,
    total, fields, static_cast<unsigned long long>(area), surface.lastFrameBatchCount() > 0 ? "yes" : "no");
}

} // namespace anonymous

int main() {
  auto surface = graphics::gpu::Surface::make(static_cast<uint32_t>(kWidth), static_cast<uint32_t>(kHeight));
  if (surface == nullptr) {
    std::printf("no GPU device\n");
    return 1;
  }
  std::printf("surface %dx%d, device: %s\n\n", kWidth, kHeight, surface->describeDevice().c_str());

  // Warm up: the first frame pays pipeline and atlas setup that no later frame does.
  {
    Scene warm;
    graphics::Canvas* canvas = surface->beginFrame();
    if (canvas != nullptr) {
      drawScene(canvas, warm);
      surface->submit();
    }
  }

  Scene full;
  measure(*surface, "full scene (AA on)", full);

  Scene noAa;
  noAa.antiAlias = false;
  measure(*surface, "AA off", noAa);

  Scene noLine;
  noLine.line = false;
  measure(*surface, "no line", noLine);

  Scene noCircle;
  noCircle.circle = false;
  measure(*surface, "no circle", noCircle);

  Scene noSquare;
  noSquare.rotatedSquare = false;
  measure(*surface, "no rotated square", noSquare);

  Scene nothing;
  nothing.line = false;
  nothing.circle = false;
  nothing.rotatedSquare = false;
  measure(*surface, "no antialiased shape at all", nothing);

  // ## Moving versus static: the cache's potential
  //
  // The renderer keys a field's identity on its origin as well as its shape, so a shape that *moves* is a new field every
  // frame while one that stays put is free. These two rows measure that difference directly: same circle, same size, one
  // orbiting and one still.
  {
    auto still = [&](graphics::Canvas* canvas, float offsetX) {
      graphics::Paint background;
      background.color = graphics::Color::MakeRGBA8(0, 0, 0, 255);
      canvas->clear(background.color);
      graphics::Paint disc;
      disc.color = graphics::Color::MakeRGBA8(255, 255, 255, 255);
      canvas->drawCircle(static_cast<float>(kHeight) / 2.0f + offsetX,
        static_cast<float>(kHeight) / 2.0f, static_cast<float>(kHeight) / 8.0f * 0.7f, disc);
    };
    double movingFields = 0.0;
    double staticFields = 0.0;
    for (int pass = 0; pass < 3; ++pass) {
      graphics::Canvas* canvas = surface->beginFrame();
      if (canvas == nullptr) break;
      // 4 px of movement per frame: enough to change the field's origin, far below anything visible.
      still(canvas, static_cast<float>(pass) * 4.0f);
      if (!surface->submit()) break;
      if (pass > 0) movingFields += surface->lastFrameStages().fields;
    }
    for (int pass = 0; pass < 3; ++pass) {
      graphics::Canvas* canvas = surface->beginFrame();
      if (canvas == nullptr) break;
      still(canvas, 0.0f);
      if (!surface->submit()) break;
      if (pass > 0) staticFields += surface->lastFrameStages().fields;
    }
    // The same circle moved by **half a pixel**: the shape in field space genuinely differs (that is what sub-pixel
    // antialiasing means), so this cannot be a cache hit. It is here to keep the boundary visible in the numbers rather than
    // only in a comment.
    double subPixelFields = 0.0;
    for (int pass = 0; pass < 3; ++pass) {
      graphics::Canvas* canvas = surface->beginFrame();
      if (canvas == nullptr) break;
      still(canvas, static_cast<float>(pass) * 0.5f);
      if (!surface->submit()) break;
      if (pass > 0) subPixelFields += surface->lastFrameStages().fields;
    }
    std::printf("\n%-28s fields %7.2f ms/frame (two frames averaged)\n", "moving circle", movingFields / 2.0);
    std::printf("%-28s fields %7.2f ms/frame (two frames averaged)\n", "circle moving by 0.5 px", subPixelFields / 2.0);
    std::printf("%-28s fields %7.2f ms/frame (two frames averaged)\n", "static circle", staticFields / 2.0);
  }

  // The same scene with a worker pool: coverage-field rasterisation is parallelisable, and this is how much.
  {
    utils::JobSystem jobs;
    graphics::gpu::SurfaceConfiguration configuration;
    configuration.width = kWidth;
    configuration.height = kHeight;
    configuration.jobSystem = &jobs;
    auto parallel = graphics::gpu::Surface::make(configuration);
    if (parallel == nullptr) {
      std::printf("\nparallel surface unavailable\n");
      return 0;
    }
    graphics::Canvas* warmCanvas = parallel->beginFrame();
    if (warmCanvas != nullptr) {
      drawScene(warmCanvas, full);
      parallel->submit();
    }
    std::printf("\n");
    measure(*parallel, "full scene, worker pool", full);
  }
  return 0;
}
