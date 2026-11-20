/*
 * A one-off: draws a figure far larger than a clip and checks where the ink actually landed. If the clip works, no ink
 * appears outside it; if it does not, the extent is drawn.
 *
 * Usage:  clip_dump <output.png>
 */

#include <graphics/Bitmap.h>
#include <graphics/Canvas.h>
#include <graphics/Data.h>
#include <graphics/Image.h>
#include <graphics/Paint.h>
#include <graphics/Pixmap.h>
#include <graphics/Surface.h>

#include <cstdio>
#include <fstream>

int main(int argc, char** argv) {
  const char* path = (argc > 1) ? argv[1] : "clip_dump.png";

  constexpr int kSize = 200;
  constexpr float kClipLeft = 40.0f;
  constexpr float kClipTop = 40.0f;
  constexpr float kClipRight = 140.0f;
  constexpr float kClipBottom = 140.0f;

  auto surface = graphics::Surface::makeRaster(kSize, kSize);
  if (surface == nullptr) return 1;

  const auto canvas = surface->getCanvas();
  canvas->clear(graphics::Color::MakeRGBA8(0, 0, 0, 255));

  // A red rectangle covering the whole surface, under a clip a quarter of its size: if the clip is honoured, red
  // appears only inside it.
  canvas->save();
  canvas->clipRect(graphics::Rect::MakeLTRB(kClipLeft, kClipTop, kClipRight, kClipBottom));

  graphics::Paint red;
  red.color = graphics::Color::MakeRGBA8(255, 0, 0, 255);
  canvas->drawRect(graphics::Rect::MakeLTRB(0.0f, 0.0f, kSize, kSize), red);

  // And a green rectangle entirely *outside* the clip, which must not appear at all.
  graphics::Paint green;
  green.color = graphics::Color::MakeRGBA8(0, 255, 0, 255);
  canvas->drawRect(graphics::Rect::MakeLTRB(150.0f, 150.0f, 190.0f, 190.0f), green);

  canvas->restore();

  // Where is the ink? Walk the pixels and report the bounding box of anything that is not black.
  const graphics::Pixmap pixmap = surface->peekPixels();
  int minX = kSize;
  int minY = kSize;
  int maxX = -1;
  int maxY = -1;
  int redCount = 0;
  int greenCount = 0;

  for (int y = 0; y < kSize; ++y) {
    for (int x = 0; x < kSize; ++x) {
      const uint32_t pixel = pixmap.getPixelARGB32(x, y);
      const uint32_t r = (pixel >> 16) & 0xFFu;
      const uint32_t g = (pixel >> 8) & 0xFFu;
      const uint32_t b = pixel & 0xFFu;
      if (r == 0 && g == 0 && b == 0) continue;

      minX = (x < minX) ? x : minX;
      minY = (y < minY) ? y : minY;
      maxX = (x > maxX) ? x : maxX;
      maxY = (y > maxY) ? y : maxY;
      if (r > 128) ++redCount;
      if (g > 128) ++greenCount;
    }
  }

  std::fprintf(stderr, "clip    : (%.0f,%.0f)-(%.0f,%.0f)\n", static_cast<double>(kClipLeft),
          static_cast<double>(kClipTop), static_cast<double>(kClipRight), static_cast<double>(kClipBottom));
  std::fprintf(stderr, "ink bbox: (%d,%d)-(%d,%d)\n", minX, minY, maxX, maxY);
  std::fprintf(stderr, "red pixels inside/at all: %d, green pixels (must be 0): %d\n", redCount, greenCount);

  const graphics::Image picture = surface->snapshot();
  std::string error;
  const graphics::Data out = picture.encode(graphics::ImageFormat::PNG, 100, &error);
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(out.bytes()), static_cast<std::streamsize>(out.size()));
  std::fprintf(stderr, "wrote %s\n", path);
  return 0;
}
