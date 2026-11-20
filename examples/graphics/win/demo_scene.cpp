/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License is distributed on
 * an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 */

/**
 * @file demo_scene.cpp
 *
 * The drawing half of the demo.
 *
 * ## How it is organised
 *
 * One function per panel, and a `drawScene` that lays the panels out and calls them. That is the structure a reader
 * wants from an example: the interesting code is a page long, and the part explaining a transform is not tangled up
 * with the part explaining a gradient.
 *
 * ## What it demonstrates
 *
 * The library's public surface, in the order a client meets it:
 *
 *  - **Colour** -- `Paint::color`, the named constants, and translucent fills composited over one another.
 *  - **Shapes** -- filled and stroked rectangles, a path with quadratic and cubic segments, a rounded panel built by
 *    hand, and a circle built from four cubics. There is no `addCircle`: these are the primitives the library offers,
 *    and building a circle from them is the honest example of what a caller does.
 *  - **Transforms and clipping** -- a rotated, translated grid, drawn once and clipped to a panel.
 *  - **Text** -- a `Font`, `Font::shape`, and `Canvas::drawTextBlob`, including a measured line and a run of mixed
 *    sizes.
 *  - **Images** -- `Image::decode` from a file, drawn at its own size and scaled, plus a colour filter applied through
 *    the paint rather than to the image.
 *  - **SVG** -- `Svg::parse` on the documents in `resources/apitest/SVG`, the same corpus the SVG tests run against, and
 *    a montage of them. A document that failed to parse is named in the panel rather than leaving a hole.
 *  - **Lottie** -- `Lottie::parse` on `resources/apitest/lottie/gear.json`, drawn at four moments so the panel shows
 *    that it animates. The playback is `examples/graphics/win/lottie_player.cpp`.
 */

#include "demo_scene.h"

#include <graphics/Bitmap.h>
#include <graphics/ColorFilter.h>
#include <graphics/Data.h>
#include <graphics/Paint.h>
#include <graphics/Path.h>
#include <graphics/PathEffect.h>
#include <graphics/Pixmap.h>
#include <graphics/Typeface.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <string>
#include <system_error>
#include <vector>

namespace demo {
namespace {

using namespace graphics;

// ==================================================================================================
// Small helpers
// ==================================================================================================

/** @return @p image decoded from @p path, or an empty image with @p note set to why not. */
Image decodeFile(const std::string& path, std::string* note) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.good()) {
    *note = "not found: " + path;
    return {};
  }

  const auto size = file.tellg();
  if (size <= 0) {
    *note = "empty: " + path;
    return {};
  }

  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  file.seekg(0, std::ios::beg);
  file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

  std::string error;
  Image image = Image::decode(Data::makeCopy(bytes.data(), bytes.size()), &error);
  if (image.isEmpty()) *note = error.empty() ? "could not decode: " + path : error + ": " + path;
  return image;
}

/** @return a paint filled with @p color. */
Paint fill(const Color color) {
  Paint paint;
  paint.color = color;
  paint.style = Paint::Style::kFill;
  return paint;
}

/** @return a paint that strokes with @p color at @p width. */
Paint stroke(const Color color, const Scalar width, const Paint::Cap cap = Paint::Cap::kButt,
        const Paint::Join join = Paint::Join::kMiter) {
  Paint paint;
  paint.color = color;
  paint.style = Paint::Style::kStroke;
  paint.strokeWidth = width;
  paint.strokeCap = cap;
  paint.strokeJoin = join;
  return paint;
}

/** @return an 8-bit colour, which is what most of this file wants to write. */
Color rgb(const uint8_t r, const uint8_t g, const uint8_t b, const uint8_t a = 255) {
  return Color::MakeRGBA8(r, g, b, a);
}

/**
 * Adds a circle to @p path as four cubic segments.
 *
 * The magic constant is the one that makes a cubic approximate a quarter circle to within a tenth of a percent:
 * `4/3 * tan(pi/8)`. Written out rather than derived so a reader can see it is the standard value.
 *
 * There is no `Path::addCircle`, which is why this exists. An example that hid it behind a helper of its own would be
 * showing the helper; one that spells it out is showing the library.
 */
void addCircle(Path& path, const Point center, const Scalar radius) {
  constexpr Scalar k = 0.5522847498f;  // 4/3 * tan(pi/8)
  const Scalar c = radius * k;

  path.moveTo(Point { center.x + radius, center.y });
  path.cubicTo(Point { center.x + radius, center.y + c }, Point { center.x + c, center.y + radius },
          Point { center.x, center.y + radius });
  path.cubicTo(Point { center.x - c, center.y + radius }, Point { center.x - radius, center.y + c },
          Point { center.x - radius, center.y });
  path.cubicTo(Point { center.x - radius, center.y - c }, Point { center.x - c, center.y - radius },
          Point { center.x, center.y - radius });
  path.cubicTo(Point { center.x + c, center.y - radius }, Point { center.x + radius, center.y - c },
          Point { center.x + radius, center.y });
  path.close();
}

/** @return a rounded rectangle, built from lines and quarter-circle cubics. */
Path roundedRect(const Rect rect, const Scalar radius) {
  const Scalar left = rect.getLeft();
  const Scalar top = rect.getTop();
  const Scalar right = rect.getRight();
  const Scalar bottom = rect.getBottom();
  const Scalar r = std::min(radius, std::min((right - left) / 2.0f, (bottom - top) / 2.0f));
  const Scalar k = r * 0.5522847498f;

  Path path;
  path.moveTo(Point { left + r, top });
  path.lineTo(Point { right - r, top });
  path.cubicTo(Point { right - r + k, top }, Point { right, top + r - k }, Point { right, top + r });
  path.lineTo(Point { right, bottom - r });
  path.cubicTo(Point { right, bottom - r + k }, Point { right - r + k, bottom }, Point { right - r, bottom });
  path.lineTo(Point { left + r, bottom });
  path.cubicTo(Point { left + r - k, bottom }, Point { left, bottom - r + k }, Point { left, bottom - r });
  path.lineTo(Point { left, top + r });
  path.cubicTo(Point { left, top + r - k }, Point { left + r - k, top }, Point { left + r, top });
  path.close();
  return path;
}

/** Draws @p text at @p x, @p y in @p font, returning the width it occupied. */
Scalar drawText(Canvas& canvas, const std::shared_ptr<Font>& font, const std::string& text, const Scalar x,
        const Scalar y, const Color color, const Paint::Style style = Paint::Style::kFill) {
  if (font == nullptr) return 0.0f;

  Paint paint = fill(color);
  paint.style = style;

  const TextBlob blob = font->shape(text);
  canvas.drawTextBlob(blob, x, y, *font, paint);
  return blob.advance().x;
}

// ==================================================================================================
// The recording stage's attribution
// ==================================================================================================

/**
 * Times the sections of one scene recording, and prints them when `CORAL_DEMO_PROFILE` is set.
 *
 * ## Why the recording stage needed this at all
 *
 * Every stage timer in the library covers `submit` -- the renderer's own work. A frame also has to be *recorded*, which is
 * what this file does: paths flattened, text shaped and rasterised into coverage bitmaps, SVG documents walked, Lottie
 * geometry built, commands appended. Measured in 4.1.38, that stage costs **100 ms a frame against `submit`'s 13.2 ms**
 * on this scene: the renderer is 11.6% of a frame, and the other 88% had never been attributed to anything.
 *
 * ## Why the sections are the panels
 *
 * Because that is where the work already is, and each one draws through a different part of the library: paths and
 * strokes, text (shaping + glyph rasterisation), images, colour filters, SVG documents, and Lottie. A section that
 * dominates names the library area to look at next; a table where nothing dominates says the cost is spread through the
 * scene's construction rather than concentrated in one route.
 *
 * The print happens once per recorded scene, so run with `--frames 1` for one recording's attribution.
 */
class SceneProfile {
public:
  SceneProfile() noexcept : mEnabled(std::getenv("CORAL_DEMO_PROFILE") != nullptr) {}

  /** Runs @p draw, attributing its wall clock to @p name when profiling is on. */
  template<typename Draw>
  void section(const char* name, Draw&& draw) {
    if (!mEnabled) {
      draw();
      return;
    }
    const auto start = std::chrono::steady_clock::now();
    draw();
    const double milliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    mSections.push_back(Entry { name, milliseconds });
  }

  /** Forgets the sections so far, which `drawScene` does at the top of each recorded scene. */
  void reset() { mSections.clear(); }

  /** Prints the sections largest first and forgets them, which `drawScene` does at the bottom of each scene. */
  void print() {
    if (!mEnabled) return;
    std::sort(mSections.begin(), mSections.end(),
            [](const Entry& a, const Entry& b) { return a.milliseconds > b.milliseconds; });
    double total = 0.0;
    for (const Entry& entry: mSections) total += entry.milliseconds;
    std::fprintf(stderr, "[record] scene sections, largest first:");
    for (const Entry& entry: mSections) {
      std::fprintf(stderr, " %s=%.2f", entry.name, entry.milliseconds);
    }
    // The sum double counts the nested sections (a document is inside the SVG panel and both are timed), which is stated
    // rather than avoided: the number that matters is each section's own, and nesting is what makes the drill-down a
    // drill-down.
    std::fprintf(stderr, " | sum (nested sections counted twice)=%.2f ms\n", total);
    std::fflush(stderr);
    mSections.clear();
  }

private:
  struct Entry {
    const char* name;
    double milliseconds;
  };

  bool mEnabled = false;
  std::vector<Entry> mSections;
};

/** The one profile a scene recording writes -- see `SceneProfile`; a function-local so any panel can reach it. */
SceneProfile& sceneProfile() {
  static SceneProfile profile;
  return profile;
}

// ==================================================================================================
// Panels
// ==================================================================================================

/** A panel: a rounded card with a title, so the scene reads as a set of demonstrations rather than one picture. */
struct Panel {
  Rect bounds;
  std::string title;
};

/** Draws the panel's card and returns the area inside it for the demonstration to use. */
Rect drawPanel(Canvas& canvas, const Panel& panel, const std::shared_ptr<Font>& titleFont,
        const std::shared_ptr<Font>& bodyFont) {
  canvas.drawPath(roundedRect(panel.bounds, 14.0f), fill(rgb(30, 34, 46)));
  canvas.drawPath(roundedRect(panel.bounds, 14.0f), stroke(rgb(64, 72, 94), 1.0f));

  // A short accent bar rather than a full-width rule: it marks the panel without boxing the content in.
  canvas.drawRect(Rect::MakeLTRB(panel.bounds.getLeft() + 18.0f, panel.bounds.getTop() + 18.0f,
                         panel.bounds.getLeft() + 22.0f, panel.bounds.getTop() + 40.0f),
          fill(rgb(88, 166, 255)));

  drawText(canvas, titleFont, panel.title, panel.bounds.getLeft() + 34.0f, panel.bounds.getTop() + 36.0f,
          rgb(226, 232, 240));

  return Rect::MakeLTRB(panel.bounds.getLeft() + 18.0f, panel.bounds.getTop() + 54.0f,
          panel.bounds.getRight() - 18.0f, panel.bounds.getBottom() - 18.0f);
}

/** Colour: alpha compositing on the left, the blend modes on the right. */
void drawColourPanel(Canvas& canvas, const Rect area, const LoadedResources& resources) {
  const Scalar bodyLeft = area.getLeft();
  const Scalar bodyTop = area.getTop();
  const Scalar bodyRight = area.getRight();
  const Scalar bodyBottom = area.getBottom();

  // The panel is split rather than shared, so neither half can be laid out into the other's space.
  const Scalar leftWidth = (bodyRight - bodyLeft) * 0.42f;
  const Scalar leftRight = bodyLeft + leftWidth;
  const Scalar rightLeft = leftRight + 18.0f;

  // ---------------------------------------------------------------- left: alpha
  const Scalar swatchSize = 54.0f;
  const Scalar swatchGap = 8.0f;

  struct Swatch {
    Color color;
    const char* label;
  };

  const Swatch swatches[] = {
    { rgb(239, 68, 68), "opaque" },
    { rgb(239, 68, 68, 128), "50%" },
    { rgb(239, 68, 68, 64), "25%" },
  };

  // Down the left edge, with the label under each, so the column reads as a scale of alpha.
  Scalar swatchY = bodyTop;
  Scalar widest = 0.0f;
  for (const Swatch& swatch: swatches) {
    canvas.drawRect(Rect::MakeLTRB(bodyLeft, swatchY, bodyLeft + swatchSize, swatchY + swatchSize), fill(swatch.color));
    drawText(canvas, resources.bodyFont, swatch.label, bodyLeft + swatchSize + 8.0f, swatchY + swatchSize * 0.5f + 5.0f,
            rgb(148, 163, 184));
    swatchY += swatchSize + swatchGap;
    widest = std::max(widest, swatchSize);
  }

  // Two translucent squares over one another at the bottom of the left half: the composite is visible where they
  // meet, which is the one thing a single shape cannot show.
  const Scalar overlapTop = bodyBottom - 58.0f;
  if (overlapTop > swatchY - swatchGap) {
    canvas.drawRect(Rect::MakeLTRB(bodyLeft, overlapTop, bodyLeft + 46.0f, overlapTop + 46.0f), fill(rgb(59, 130, 246)));
    canvas.drawRect(Rect::MakeLTRB(bodyLeft + 23.0f, overlapTop + 23.0f, bodyLeft + 69.0f, overlapTop + 69.0f),
            fill(rgb(250, 204, 21, 150)));
  }

  // ---------------------------------------------------------------- right: the modes
  const BlendMode showcase[] = {
    BlendMode::kSrcOver,
    BlendMode::kMultiply,
    BlendMode::kScreen,
    BlendMode::kOverlay,
    BlendMode::kDarken,
    BlendMode::kLighten,
    BlendMode::kColorDodge,
    BlendMode::kColorBurn,
    BlendMode::kHardLight,
    BlendMode::kSoftLight,
    BlendMode::kDifference,
    BlendMode::kExclusion,
  };

  // Two columns, six rows. The height is divided by the row count so the grid fills the half it was given, and the
  // swatch takes what is left after its name -- which is drawn above it, so no width has to be reserved for text.
  constexpr int kColumns = 2;
  constexpr Scalar kNameHeight = 12.0f;
  constexpr Scalar kRowGap = 4.0f;

  const Scalar rightWidth = bodyRight - rightLeft;
  const Scalar cellGap = 8.0f;
  const Scalar cellWidth = (rightWidth - cellGap * (kColumns - 1)) / kColumns;
  const auto rows = static_cast<int>((std::size(showcase) + kColumns - 1) / kColumns);
  const Scalar rowHeight = (bodyBottom - bodyTop - kRowGap * (rows - 1)) / static_cast<Scalar>(rows);
  const Scalar swatchHeight = std::max(10.0f, rowHeight - kNameHeight);

  for (size_t i = 0; i < std::size(showcase); ++i) {
    const auto column = static_cast<int>(i) % kColumns;
    const auto row = static_cast<int>(i) / kColumns;

    const Scalar cellX = rightLeft + static_cast<Scalar>(column) * (cellWidth + cellGap);
    const Scalar cellY = bodyTop + static_cast<Scalar>(row) * (rowHeight + kRowGap);

    // The name, above its swatch.
    drawText(canvas, resources.bodyFont, graphics::toString(showcase[i]), cellX, cellY + kNameHeight - 3.0f,
            rgb(148, 163, 184));

    const Scalar swatchTop = cellY + kNameHeight;

    // The ground: an opaque blue, so every mode has something to combine with.
    canvas.drawRect(Rect::MakeLTRB(cellX, swatchTop, cellX + cellWidth, swatchTop + swatchHeight),
            fill(rgb(37, 99, 235)));

    // The overlay: a translucent yellow with the mode on its paint, over the right two thirds, so one swatch shows
    // both the blended and the unblended colour and the difference is visible without a second reference.
    Paint blended = fill(rgb(250, 204, 21, 220));
    blended.blendMode = showcase[i];
    canvas.drawRect(
            Rect::MakeLTRB(cellX + cellWidth * 0.34f, swatchTop, cellX + cellWidth, swatchTop + swatchHeight), blended);
  }
}

/** Shapes: rectangles, a curve, a rounded card, a circle, and a stroked path with joins and caps. */
void drawShapePanel(Canvas& canvas, const Rect area) {
  // Filled and stroked rectangles, side by side so the difference in the paint is visible rather than described.
  canvas.drawRect(Rect::MakeLTRB(area.getLeft(), area.getTop(), area.getLeft() + 70.0f, area.getTop() + 50.0f),
          fill(rgb(52, 211, 153)));
  canvas.drawRect(
          Rect::MakeLTRB(area.getLeft() + 84.0f, area.getTop(), area.getLeft() + 154.0f, area.getTop() + 50.0f),
          stroke(rgb(52, 211, 153), 4.0f));

  // A wave: two quadratic segments and a cubic, which is the whole of the path vocabulary beyond lines.
  Path wave;
  wave.moveTo(Point { area.getLeft(), area.getTop() + 110.0f });
  wave.quadTo(Point { area.getLeft() + 40.0f, area.getTop() + 60.0f },
          Point { area.getLeft() + 80.0f, area.getTop() + 110.0f });
  wave.cubicTo(Point { area.getLeft() + 110.0f, area.getTop() + 150.0f },
          Point { area.getLeft() + 140.0f, area.getTop() + 60.0f }, Point { area.getLeft() + 180.0f, area.getTop() + 110.0f });
  canvas.drawPath(wave, stroke(rgb(251, 191, 36), 3.0f, Paint::Cap::kRound, Paint::Join::kRound));

  // A stroked polyline, which is where the join setting shows: a miter on a sharp angle produces a spike, a round
  // join does not.
  Path zigzag;
  zigzag.moveTo(Point { area.getLeft(), area.getTop() + 190.0f });
  zigzag.lineTo(Point { area.getLeft() + 34.0f, area.getTop() + 156.0f });
  zigzag.lineTo(Point { area.getLeft() + 68.0f, area.getTop() + 190.0f });
  zigzag.lineTo(Point { area.getLeft() + 102.0f, area.getTop() + 156.0f });
  canvas.drawPath(zigzag, stroke(rgb(244, 114, 182), 10.0f, Paint::Cap::kRound, Paint::Join::kRound));

  // A circle built from the primitives, filled and then stroked, with a translucent inner circle over it.
  const Point centre { area.getLeft() + 250.0f, area.getTop() + 70.0f };
  Path circle;
  addCircle(circle, centre, 54.0f);
  canvas.drawPath(circle, fill(rgb(139, 92, 246)));
  canvas.drawPath(circle, stroke(rgb(196, 181, 253), 3.0f));

  Path inner;
  addCircle(inner, centre, 30.0f);
  canvas.drawPath(inner, fill(rgb(255, 255, 255, 70)));

  // A rounded card, which is the shape a caller reaches for most often and which the library does not provide.
  canvas.drawPath(roundedRect(Rect::MakeLTRB(area.getLeft() + 200.0f, area.getTop() + 150.0f,
                                     area.getLeft() + 340.0f, area.getTop() + 215.0f),
                          18.0f),
          fill(rgb(30, 41, 59)));
  canvas.drawPath(roundedRect(Rect::MakeLTRB(area.getLeft() + 200.0f, area.getTop() + 150.0f,
                                     area.getLeft() + 340.0f, area.getTop() + 215.0f),
                          18.0f),
          stroke(rgb(71, 85, 105), 2.0f));
}

/** Transforms and clipping: the same shape drawn through a rotated matrix, clipped to the panel. */
void drawTransformPanel(Canvas& canvas, const Rect area) {
  canvas.save();
  canvas.clipRect(area);

  // A grid drawn in the canvas's own space, then rotated as a whole. Transforming the canvas rather than each
  // rectangle is the point: one `concat` places everything drawn after it.
  //
  // Its size is derived from the room, not chosen: a rotated square's diagonal is what has to fit, so the half-extent
  // is `side * sqrt(2) / 2` and the grid is built to stay inside the shorter side of the body. An earlier version used
  // fixed numbers that happened to be larger than the panel and relied on the clip to hide the overflow.
  const Scalar gridRoom = std::min(area.getRight() - area.getLeft(), area.getBottom() - area.getTop()) * 0.62f;
  const Scalar cell = gridRoom / 2.0f;             // half-extent of one cell
  const Scalar step = cell * 1.38f;                // spacing, so the cells do not touch
  const Point gridCentre { area.getLeft() + 128.0f, area.getTop() + (area.getBottom() - area.getTop()) * 0.5f };

  canvas.save();
  canvas.concat(Matrix::MakeTranslation(Vector3 { gridCentre.x, gridCentre.y, 0.0f }));
  canvas.concat(Matrix::MakeRotationZ(Radians { 0.28f }));

  for (int i = -1; i <= 1; ++i) {
    for (int j = -1; j <= 1; ++j) {
      const Scalar x = static_cast<Scalar>(i) * step;
      const Scalar y = static_cast<Scalar>(j) * step;

      // The alpha falls off with distance from the centre, which makes the rotation legible: without it the grid is
      // symmetric enough that a rotated copy looks like an unrotated one.
      const float distance = std::sqrt(static_cast<float>(i * i + j * j));
      const auto alpha = static_cast<uint8_t>(std::min(255.0f, 236.0f - distance * 54.0f));

      canvas.drawRect(Rect::MakeLTRB(x - cell, y - cell, x + cell, y + cell), fill(rgb(56, 189, 248, alpha)));
    }
  }
  canvas.restore();

  // A circle drawn *after* the transform is restored, so it is upright -- the contrast shows what the transform did.
  const Scalar markerRadius = gridRoom * 0.34f;
  Path marker;
  addCircle(marker, Point { area.getRight() - markerRadius - 26.0f, gridCentre.y }, markerRadius);
  canvas.drawPath(marker, stroke(rgb(248, 113, 113), 3.0f));

  canvas.restore();
}

/** Text: a heading, a paragraph line, and the same run at three sizes, all shaped by harfbuzz. */
void drawTextPanel(Canvas& canvas, const Rect area, const LoadedResources& resources) {
  if (!resources.hasFonts()) {
    drawText(canvas, resources.bodyFont, "(no font loaded)", area.getLeft(), area.getTop() + 20.0f,
            rgb(248, 113, 113));
    return;
  }

  Scalar y = area.getTop() + 26.0f;
  canvas.drawTextBlob(resources.titleFont->shape("Shaped by harfbuzz, drawn as outlines"), area.getLeft(), y,
          *resources.titleFont, fill(rgb(226, 232, 240)));

  y += 34.0f;
  const std::string line = "The quick brown fox jumps over the lazy dog";
  const Scalar width = drawText(canvas, resources.bodyFont, line, area.getLeft(), y, rgb(148, 163, 184));

  // The measured width, drawn as a rule underneath. This is `measureWidth` agreeing with what was actually drawn,
  // which is the property a caller laying out text depends on.
  canvas.drawRect(Rect::MakeLTRB(area.getLeft(), y + 6.0f, area.getLeft() + width, y + 7.0f),
          fill(rgb(88, 166, 255, 140)));

  y += 40.0f;
  const std::string pangram = "Pack my box with five dozen liquor jugs";

  // One run at three sizes, to show that a font's size is a property of the font rather than of the paint.
  const float sizes[] = { 13.0f, 18.0f, 26.0f };
  Scalar x = area.getLeft();
  for (const float size: sizes) {
    const auto sized = resources.bodyFont->withSize(size);
    x += drawText(canvas, sized, pangram.substr(0, 8), x, y + (26.0f - size), rgb(203, 213, 225)) + 10.0f;
  }

  // A stroke rather than a fill, which is what a caller uses for an outline heading.
  y += 46.0f;
  drawText(canvas, resources.titleFont, "Outlined", area.getLeft(), y, rgb(250, 204, 21), Paint::Style::kStroke);
  drawText(canvas, resources.titleFont, "Outlined", area.getLeft() + 130.0f, y, rgb(250, 204, 21));

  // Colour emoji on the same baseline, at the right edge of the panel. These are `CBDT` bitmaps
  // with no outline at all, so they exercise the bitmap-glyph path on both the rasterizer and whatever
  // GPU backend `--backend gpu` selected -- the same scene, the same `Canvas` calls.
  if (resources.hasEmoji()) {
    drawText(canvas, resources.emojiFont, "🤡👻🐠🤩😃🤪", area.getRight() - 245.0f, y, rgb(255, 255, 255));
  }
}

/** Images: decoded from disk, drawn at its own size and scaled, the second through a colour filter. */
void drawImagePanel(Canvas& canvas, const Rect area, const LoadedResources& resources) {
  if (!resources.hasImage()) {
    drawText(canvas, resources.bodyFont, "(no image loaded)", area.getLeft(), area.getTop() + 20.0f,
            rgb(248, 113, 113));
    return;
  }

  const Scalar bodyWidth = area.getRight() - area.getLeft();
  const Scalar bodyHeight = area.getBottom() - area.getTop();

  // Two thumbnails along the long axis of the body, with a caption under each. The room is a wide, short rectangle,
  // so the images sit side by side rather than stacked -- which is also what makes the caption fit.
  constexpr Scalar kCaptionRoom = 22.0f;
  const Scalar gap = 16.0f;
  const Scalar tileWidth = (bodyWidth - gap) / 2.0f;
  const Scalar tileHeight = bodyHeight - kCaptionRoom;

  // Fit inside the tile rather than filling it: an image stretched to the tile's aspect ratio is not the image.
  const auto fitScale = [](const Scalar imageWidth, const Scalar imageHeight, const Scalar intoWidth,
                            const Scalar intoHeight) {
    if (imageWidth <= 0.0f || imageHeight <= 0.0f) return 0.0f;
    return std::min(intoWidth / imageWidth, intoHeight / imageHeight);
  };

  const Scalar imageWidth = static_cast<Scalar>(resources.image.width());
  const Scalar imageHeight = static_cast<Scalar>(resources.image.height());
  const Scalar scale = fitScale(imageWidth, imageHeight, tileWidth, tileHeight);

  // A plate behind each thumbnail, so an image with transparent corners reads as a picture rather than as a hole.
  canvas.drawPath(roundedRect(Rect::MakeLTRB(area.getLeft(), area.getTop(), area.getLeft() + tileWidth,
                                     area.getTop() + tileHeight),
                          6.0f),
          fill(rgb(22, 26, 36)));

  // Left: fitted, drawn through a clip so an image larger than the tile is cropped rather than overflowing it.
  canvas.save();
  canvas.clipRect(Rect::MakeLTRB(area.getLeft(), area.getTop(), area.getLeft() + tileWidth, area.getTop() + tileHeight));
  canvas.concat(Matrix::MakeTranslation(Vector3 { area.getLeft(), area.getTop(), 0.0f }));
  canvas.concat(Matrix::MakeScale(Vector2 { scale, scale }));
  canvas.drawImage(resources.image, 0.0f, 0.0f, fill(Color::White()));
  canvas.restore();

  const Scalar rightX = area.getLeft() + tileWidth + gap;

  // Right: the same image through a colour filter on the paint. The image is untouched, so the same one can be drawn
  // again without the filter -- which is the point of the filter belonging to the paint.
  canvas.drawPath(roundedRect(Rect::MakeLTRB(rightX, area.getTop(), rightX + tileWidth, area.getTop() + tileHeight),
                          6.0f),
          fill(rgb(22, 26, 36)));

  ColorFilterMatrix greyscale;
  greyscale.values = { 0.2126f, 0.7152f, 0.0722f, 0.0f, 0.0f, 0.2126f, 0.7152f, 0.0722f, 0.0f, 0.0f, 0.2126f, 0.7152f,
    0.0722f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };

  Paint filtered = fill(Color::White());
  filtered.colorFilter = ColorFilter::Matrix(greyscale);

  canvas.save();
  canvas.clipRect(
          Rect::MakeLTRB(rightX, area.getTop(), rightX + tileWidth, area.getTop() + tileHeight));
  canvas.concat(Matrix::MakeTranslation(Vector3 { rightX, area.getTop(), 0.0f }));
  canvas.concat(Matrix::MakeScale(Vector2 { scale, scale }));
  canvas.drawImage(resources.image, 0.0f, 0.0f, filtered);
  canvas.restore();

  // Captions under each, and the scale, so the picture says what was done to it.
  const Scalar captionY = area.getBottom() - 6.0f;
  drawText(canvas, resources.bodyFont, "decoded from a file", area.getLeft(), captionY, rgb(148, 163, 184));

  char note[96] = {};
  std::snprintf(note, sizeof(note), "drawn at %.0f%% through a colour filter",
          static_cast<double>(scale * 100.0f));
  drawText(canvas, resources.bodyFont, note, rightX, captionY, rgb(148, 163, 184));
}

/** Path effects: the same stroke drawn dashed and solid, which compares two features in one panel. */
void drawEffectPanel(Canvas& canvas, const Rect area) {
  Path line;
  const Scalar y = area.getTop() + 30.0f;
  line.moveTo(Point { area.getLeft(), y });
  line.lineTo(Point { area.getRight() - 40.0f, y });

  canvas.drawPath(line, stroke(rgb(148, 163, 184), 3.0f));

  const auto dash = PathEffect::Dash({ 14.0f, 8.0f });
  if (dash != nullptr) {
    const Path dashed = dash->filter(line);
    canvas.save();
    canvas.concat(Matrix::MakeTranslation(Vector3 { 0.0f, 34.0f, 0.0f }));
    canvas.drawPath(dashed, stroke(rgb(88, 166, 255), 3.0f));
    canvas.restore();
  }

  const auto dots = PathEffect::Dash({ 2.0f, 9.0f });
  if (dots != nullptr) {
    const Path dotted = dots->filter(line);
    canvas.save();
    canvas.concat(Matrix::MakeTranslation(Vector3 { 0.0f, 68.0f, 0.0f }));
    canvas.drawPath(dotted, stroke(rgb(52, 211, 153), 4.0f, Paint::Cap::kRound));
    canvas.restore();
  }

  // A colour filter, which is the other kind of effect a paint carries: the same swatch, desaturated.
  const Scalar swatchY = y + 104.0f;
  canvas.drawRect(Rect::MakeLTRB(area.getLeft(), swatchY, area.getLeft() + 90.0f, swatchY + 40.0f),
          fill(rgb(239, 68, 68)));

  // A greyscale matrix: the luminance coefficients on every row, which is the standard saturation-zero matrix.
  ColorFilterMatrix greyscale;
  greyscale.values = { 0.2126f, 0.7152f, 0.0722f, 0.0f, 0.0f, 0.2126f, 0.7152f, 0.0722f, 0.0f, 0.0f, 0.2126f, 0.7152f,
    0.0722f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };

  Paint filtered = fill(rgb(239, 68, 68));
  filtered.colorFilter = ColorFilter::Matrix(greyscale);
  canvas.drawRect(Rect::MakeLTRB(area.getLeft() + 110.0f, swatchY, area.getLeft() + 200.0f, swatchY + 40.0f), filtered);
}

} // namespace anonymous

// ==================================================================================================
// Loading
// ==================================================================================================

LoadedResources load(const Resources& wanted) {
  LoadedResources loaded;

  if (!wanted.fontPath.empty()) {
    const auto typeface = Typeface::fromFile(wanted.fontPath);
    if (typeface != nullptr) {
      loaded.titleFont = Font::create(typeface, 19.0f);
      loaded.bodyFont = Font::create(typeface, 15.0f);
    } else {
      loaded.notes.push_back("could not load the font: " + wanted.fontPath);
    }
  } else {
    loaded.notes.push_back("no font was given");
  }

  if (!wanted.emojiFontPath.empty()) {
    const auto typeface = Typeface::fromFile(wanted.emojiFontPath);
    if (typeface != nullptr) {
      loaded.emojiFont = Font::create(typeface, 30.0f);
    } else {
      loaded.notes.push_back("could not load the emoji font: " + wanted.emojiFontPath);
    }
  } else {
    loaded.notes.push_back("no emoji font was given");
  }

  if (!wanted.imagePath.empty()) {
    std::string note;
    loaded.image = decodeFile(wanted.imagePath, &note);
    if (!note.empty()) loaded.notes.push_back(note);
  }

  if (!wanted.secondImagePath.empty()) {
    std::string note;
    loaded.secondImage = decodeFile(wanted.secondImagePath, &note);
    // A missing second image is not worth a note: the panel works without it and the demo says so by drawing one
    // image instead of two.
  }

  // ## The SVG documents
  //
  // Parsed here rather than in the panel, because parsing is what can fail -- a directory that exists may still hold a
  // file that does not parse -- and doing it once means the panel draws rather than reporting an error inside a
  // per-frame call.
  //
  // Sorted by name so the montage is in the same order on every machine. A directory iterator does not promise one, and
  // a demo whose picture changes between runs is not a demo of anything.
  if (!wanted.svgDirectory.empty()) {
    std::error_code failure;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(wanted.svgDirectory, failure)) {
      if (entry.path().extension() == ".svg") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());

    for (const std::filesystem::path& file : files) {
      std::ifstream stream(file, std::ios::binary);
      std::ostringstream contents;
      contents << stream.rdbuf();

      std::string error;
      auto document = graphics::Svg::parse(contents.str(), &error);
      if (!document) {
        // A document that will not parse is named in the notes rather than dropped, because a montage with a missing
        // cell looks like a document that drew nothing.
        loaded.notes.push_back("could not parse " + file.filename().string() + ": " + error);
        continue;
      }
      loaded.svgNames.push_back(file.filename().string());
      loaded.svgDocuments.push_back(std::move(*document));
    }

    if (loaded.svgDocuments.empty() && failure) {
      loaded.notes.push_back("could not read the SVG directory: " + wanted.svgDirectory);
    }
  }

  // ## The Lottie animation
  if (!wanted.lottiePath.empty()) {
    std::ifstream stream(wanted.lottiePath, std::ios::binary);
    std::ostringstream contents;
    contents << stream.rdbuf();

    std::string error;
    auto animation = graphics::Lottie::parse(contents.str(), &error);
    if (animation) {
      loaded.lottie = std::move(*animation);
    } else {
      loaded.notes.push_back("could not parse the Lottie animation: " + error);
    }
  }

  return loaded;
}

/**
 * A montage of the SVG documents, each scaled into a cell.
 *
 * The documents are drawn with `Svg::render`, which fits a document's viewBox into the rectangle it is given -- so each
 * one lands in its cell whatever its own size is. Putting several side by side is what makes a document that renders
 * wrongly visible: one blank cell among filled ones is a parse failure, and one that is all one colour is a paint
 * failure, and both are obvious at a glance and invisible in isolation.
 */
void drawSvgPanel(Canvas& canvas, const Rect area, const LoadedResources& resources) {
  if (!resources.hasSvg()) {
    drawText(canvas, resources.bodyFont, "no SVG documents were found", area.getLeft() + 12.0f, area.getTop() + 24.0f,
            rgb(148, 163, 184));
    return;
  }

  // Enough cells for the whole corpus rather than the first twelve of it. The two documents this work unblocked sort
  // thirteenth and nineteenth, so a twelve-cell grid showed a byte-identical picture before and after -- which is the one
  // thing a demonstration panel must not do.
  constexpr int kColumns = 6;
  constexpr int kRows = 4;
  const Scalar gap = 6.0f;
  // The caption sits under the grid, so the grid is given the height less the room the caption needs. Sharing the height
  // between them instead draws the last row over the text, which is what the first version of this panel did.
  constexpr Scalar kCaptionRoom = 22.0f;
  const Scalar gridHeight = area.getHeight() - kCaptionRoom;
  const Scalar cellWidth = (area.getWidth() - gap * (kColumns - 1)) / kColumns;
  const Scalar cellHeight = (gridHeight - gap * (kRows - 1)) / kRows;

  const size_t cells = static_cast<size_t>(kColumns) * kRows;
  const size_t count = std::min(cells, resources.svgDocuments.size());

  for (size_t i = 0; i < count; ++i) {
    const Scalar x = area.getLeft() + static_cast<Scalar>(i % kColumns) * (cellWidth + gap);
    const Scalar y = area.getTop() + static_cast<Scalar>(i / kColumns) * (cellHeight + gap);

    // A pale cell behind each document, because most of the corpus draws dark or coloured marks on nothing and would be
    // invisible against the panel's own background.
    canvas.drawRect(Rect::MakeLTRB(x, y, x + cellWidth, y + cellHeight), fill(rgb(226, 232, 240)));
    // Each document under its own name when profiling, because one panel of twenty-two documents is not an attribution:
    // the next question after "SVG is 91% of the recording" is *which* document, and the corpus is deliberately varied.
    // The name outlives the profile (both are inside this call), so the pointer is safe.
    const char* const name = i < resources.svgNames.size() ? resources.svgNames[i].c_str() : "svg";
    sceneProfile().section(name, [&] {
      resources.svgDocuments[i].render(canvas, Rect::MakeLTRB(x, y, x + cellWidth, y + cellHeight));
    });
  }

  // Named rather than silent when there are more documents than cells: a montage that quietly dropped half the corpus
  // would look complete.
  std::string caption = std::to_string(resources.svgDocuments.size()) + " documents";
  if (resources.svgDocuments.size() > cells) {
    // Still possible if the corpus grows past the grid. Saying so is better than a montage that looks complete while
    // quietly dropping documents.
    caption += ", first " + std::to_string(cells) + " shown";
  }
  drawText(canvas, resources.bodyFont, caption, area.getLeft() + 12.0f, area.getBottom() - 6.0f, rgb(100, 116, 139));
}

/**
 * The Lottie animation at four moments, left to right.
 *
 * Four stills rather than one, because the point of an animation that does not move is invisible in a single frame -- and
 * because four frames of a turning gear is what shows that the keyframes are interpolated rather than snapped.
 */
void drawLottiePanel(Canvas& canvas, const Rect area, const LoadedResources& resources,
        const Scalar animationSeconds = 0.0f) {
  if (!resources.hasLottie()) {
    drawText(canvas, resources.bodyFont, "no Lottie animation was found", area.getLeft() + 12.0f, area.getTop() + 24.0f,
            rgb(148, 163, 184));
    return;
  }

  constexpr int kFrames = 4;
  const Scalar gap = 6.0f;
  constexpr Scalar kCaptionRoom = 22.0f;
  const Scalar cellWidth = (area.getWidth() - gap * (kFrames - 1)) / kFrames;
  const Scalar cellHeight = area.getHeight() - kCaptionRoom;

  const Scalar duration = resources.lottie.duration();
  for (int i = 0; i < kFrames; ++i) {
    const Scalar x = area.getLeft() + static_cast<Scalar>(i) * (cellWidth + gap);
    const Scalar y = area.getTop();
    // The last still is at three quarters of the animation rather than at the end, so the four are evenly spaced in
    // time: a still at exactly the duration would be the same as the first for a rotation that completes a full turn.
    //
    // `animationSeconds` shifts all four, which is what `--animate lottie` uses to make the gears turn frame by frame
    // while the rest of the scene stays exactly where it is. At zero -- every run that is compared against a baseline --
    // the four offsets are the ones above, so the picture is unchanged.
    const Scalar seconds = duration * static_cast<Scalar>(i) / kFrames + animationSeconds;

    canvas.drawRect(Rect::MakeLTRB(x, y, x + cellWidth, y + cellHeight), fill(rgb(226, 232, 240)));
    resources.lottie.render(canvas, Rect::MakeLTRB(x, y, x + cellWidth, y + cellHeight), seconds);
  }

  // Two decimal places, through `snprintf` rather than `std::to_string`, which has no fixed-point form that does not
  // also drag in a locale. The first version rounded with integer division and then printed the double, which gave
  // "5.000000s".
  char seconds[32];
  std::snprintf(seconds, sizeof(seconds), "%.2fs", static_cast<double>(duration));
  const std::string caption =
          std::string(seconds) + " at " + std::to_string(static_cast<int>(resources.lottie.frameRate())) + " fps";
  drawText(canvas, resources.bodyFont, caption, area.getLeft() + 12.0f, area.getBottom() - 6.0f, rgb(100, 116, 139));
}

// ==================================================================================================
// The scene
// ==================================================================================================

void drawScene(Canvas& canvas, const LoadedResources& resources, const Scalar animationSeconds) {
  // The recording stage's attribution -- see `SceneProfile`. Off unless `CORAL_DEMO_PROFILE` is set.
  SceneProfile& profile = sceneProfile();
  profile.reset();

  // The background is a vertical ramp built from bands rather than a gradient, because a gradient shader is not part
  // of this library's surface yet. Bands are one rectangle each and show the same thing: the canvas composites.
  profile.section("background", [&] {
    constexpr int kBands = 64;
    for (int i = 0; i < kBands; ++i) {
      const auto t = static_cast<float>(i) / static_cast<float>(kBands - 1);
      const auto r = static_cast<uint8_t>(18.0f + t * 10.0f);
      const auto g = static_cast<uint8_t>(20.0f + t * 12.0f);
      const auto b = static_cast<uint8_t>(30.0f + t * 24.0f);

      const Scalar y0 = static_cast<Scalar>(i) * (static_cast<Scalar>(kHeight) / kBands);
      const Scalar y1 = static_cast<Scalar>(i + 1) * (static_cast<Scalar>(kHeight) / kBands);
      canvas.drawRect(Rect::MakeLTRB(0.0f, y0, static_cast<Scalar>(kWidth), y1), fill(rgb(r, g, b)));
    }
  });

  // The heading, and a rule under it.
  profile.section("heading", [&] {
    drawText(canvas, resources.titleFont, "Coral graphics - Canvas demonstration", 40.0f, 58.0f, rgb(241, 245, 249));
    canvas.drawRect(Rect::MakeLTRB(40.0f, 72.0f, 960.0f, 73.0f), fill(rgb(51, 65, 85)));
    drawText(canvas, resources.bodyFont, "Every panel below is drawn with the public API only.", 40.0f, 92.0f,
            rgb(100, 116, 139));
  });

  // The layout: two columns, three rows, each panel a rounded card. The numbers are literal because an example that
  // computed its layout would bury the drawing in arithmetic.
  constexpr Scalar kLeft = 40.0f;
  constexpr Scalar kRight = 960.0f;
  constexpr Scalar kTop = static_cast<Scalar>(kHeadingRoom);
  constexpr Scalar kBottom = static_cast<Scalar>(kHeight - kFooterRoom);
  constexpr Scalar kGap = 16.0f;

  constexpr int kRows = 4;
  const Scalar columnWidth = ((kRight - kLeft) - kGap) / 2.0f;
  const Scalar rowHeight = ((kBottom - kTop) - kGap * (kRows - 1)) / kRows;

  // The row's top edge, so the six entries below do not each repeat the arithmetic.
  const auto rowTop = [kTop, rowHeight, kGap](const int row) {
    return kTop + (rowHeight + kGap) * static_cast<Scalar>(row);
  };
  const Scalar leftColumn = kLeft + columnWidth;
  const Scalar rightColumn = kLeft + columnWidth + kGap;

  const Panel panels[] = {
    { Rect::MakeLTRB(kLeft, rowTop(0), leftColumn, rowTop(0) + rowHeight), "Colour and blending" },
    { Rect::MakeLTRB(rightColumn, rowTop(0), kRight, rowTop(0) + rowHeight), "Shapes and paths" },
    { Rect::MakeLTRB(kLeft, rowTop(1), leftColumn, rowTop(1) + rowHeight), "Transforms and clipping" },
    { Rect::MakeLTRB(rightColumn, rowTop(1), kRight, rowTop(1) + rowHeight), "Images" },
    { Rect::MakeLTRB(kLeft, rowTop(2), leftColumn, rowTop(2) + rowHeight), "Text" },
    { Rect::MakeLTRB(rightColumn, rowTop(2), kRight, rowTop(2) + rowHeight), "Path effects and colour filters" },
    { Rect::MakeLTRB(kLeft, rowTop(3), leftColumn, rowTop(3) + rowHeight), "SVG documents" },
    { Rect::MakeLTRB(rightColumn, rowTop(3), kRight, rowTop(3) + rowHeight), "Lottie animation" },
  };

  Rect areas[8] = {};
  profile.section("panel cards", [&] {
    for (int panel = 0; panel < 8; ++panel) {
      areas[panel] = drawPanel(canvas, panels[panel], resources.titleFont, resources.bodyFont);
    }
  });

  // ## The order is a debugging tool as well as the paint order
  //
  // `CORAL_DEMO_PANEL_ORDER=svg-first` draws the SVG panel before anything else. A panel that renders correctly on
  // its own and wrongly in the full frame is a defect in *accumulated state*, and moving it to the front is the
  // cheapest way to tell that apart from a defect in the panel: if it draws correctly first and wrongly later, the
  // state carried by the earlier panels is the cause and the panel itself is innocent.
  const char* const order = std::getenv("CORAL_DEMO_PANEL_ORDER");
  const bool svgFirst = order != nullptr && std::string(order) == "svg-first";

  if (svgFirst) {
    profile.section("svg", [&] { drawSvgPanel(canvas, areas[6], resources); });
  }

  profile.section("colour", [&] { drawColourPanel(canvas, areas[0], resources); });
  profile.section("shapes", [&] { drawShapePanel(canvas, areas[1]); });
  profile.section("transforms", [&] { drawTransformPanel(canvas, areas[2]); });
  profile.section("images", [&] { drawImagePanel(canvas, areas[3], resources); });
  profile.section("text", [&] { drawTextPanel(canvas, areas[4], resources); });
  profile.section("effects", [&] { drawEffectPanel(canvas, areas[5]); });

  // The two panels the translation added. They are last so that a reader comparing this file against the one before the
  // SVG and Lottie work can see exactly what was appended -- unless the environment asked for the other order.
  if (!svgFirst) {
    profile.section("svg", [&] { drawSvgPanel(canvas, areas[6], resources); });
  }
  profile.section("lottie", [&] { drawLottiePanel(canvas, areas[7], resources, animationSeconds); });

  // A footer naming what was actually loaded, so a run with missing resources says so on the picture rather than only
  // on the console.
  profile.section("footer", [&] {
    std::string footer = std::to_string(kWidth) + " x " + std::to_string(kHeight) +
                              ", raster surface, drawn in one pass";
    for (const std::string& note: resources.notes) footer += "  |  " + note;
    drawText(canvas, resources.bodyFont, footer, 40.0f, static_cast<Scalar>(kHeight) - 16.0f,
            rgb(100, 116, 139));
  });

  // The attribution goes out at the end of the scene it describes, so a one-frame run prints one table.
  profile.print();
}

} // namespace demo
