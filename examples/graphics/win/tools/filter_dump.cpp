/*
 * A one-off: draws the same opaque colour twice, once through a greyscale colour filter and once without, and reports
 * both pixels. A filter that is applied turns the left square grey; one that is ignored leaves it red, and the two
 * pixel values are identical.
 *
 * Usage:  filter_dump [output.png]
 */

#include <graphics/Bitmap.h>
#include <graphics/Canvas.h>
#include <graphics/ColorFilter.h>
#include <graphics/Paint.h>
#include <graphics/Pixmap.h>
#include <graphics/Surface.h>

#include <cstdio>

int main(int argc, char** argv) {
  auto surface = graphics::Surface::makeRaster(120, 60);
  if (surface == nullptr) return 1;

  const auto canvas = surface->getCanvas();
  canvas->clear(graphics::Color::MakeRGBA8(0, 0, 0, 255));

  graphics::ColorFilterMatrix greyscale;
  greyscale.values = { 0.2126f, 0.7152f, 0.0722f, 0.0f, 0.0f, 0.2126f, 0.7152f, 0.0722f, 0.0f, 0.0f, 0.2126f, 0.7152f,
    0.0722f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };

  // Left: plain red.
  graphics::Paint plain;
  plain.color = graphics::Color::MakeRGBA8(220, 40, 40, 255);
  canvas->drawRect(graphics::Rect::MakeLTRB(10.0f, 10.0f, 50.0f, 50.0f), plain);

  // Right: the same red, through the filter.
  graphics::Paint filtered;
  filtered.color = graphics::Color::MakeRGBA8(220, 40, 40, 255);
  filtered.colorFilter = graphics::ColorFilter::Matrix(greyscale);
  canvas->drawRect(graphics::Rect::MakeLTRB(70.0f, 10.0f, 110.0f, 50.0f), filtered);

  const graphics::Pixmap pixmap = surface->peekPixels();
  const auto report = [&pixmap](const char* name, const int x) {
    const uint32_t pixel = pixmap.getPixelARGB32(x, 30);
    std::fprintf(stderr, "%s: argb=%08X  r=%u g=%u b=%u a=%u\n", name, pixel, (pixel >> 16) & 0xFFu,
            (pixel >> 8) & 0xFFu, pixel & 0xFFu, (pixel >> 24) & 0xFFu);
  };
  report("plain   ", 30);
  report("filtered", 90);

  std::fprintf(stderr, "the filter should make the right square grey and slightly darker: r==g==b, and about %d\n",
          static_cast<int>(0.2126 * 220 + 0.7152 * 40 + 0.0722 * 40));

  (void) argc;
  (void) argv;
  return 0;
}
