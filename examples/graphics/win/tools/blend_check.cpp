/*
 * Prints what each blend mode produces for a pair of colours, through `blendWith`.
 *
 * This is the tool to reach for when a blend result is in doubt: it shows the mode, its name, both inputs and the
 * output channel by channel, so a wrong answer can be attributed to the formula, to the expectation, or to the
 * compositing around it. Every value the blend-mode tests assert was confirmed with this first -- twice a "broken
 * formula" turned out to be a wrong expectation, and once a genuinely swapped pair of mode definitions.
 *
 * It reads the private headers, because the formulas live there. It is a tool for this library's internals rather than
 * an example of how to use it.
 *
 * Usage:  blend_check [destination-hex] [source-hex]
 *         both default to 0xFF404040 and 0xFF40BF80, the pair the tests use
 */

#include <private/graphics/Blend.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace graphics;
using namespace graphics::raster;

namespace {

uint32_t parseArgb(const char* text, const uint32_t fallback) {
  if (text == nullptr) return fallback;

  const char* digits = (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) ? text + 2 : text;
  char* end = nullptr;
  const unsigned long value = std::strtoul(digits, &end, 16);
  if (end == digits || *end != '\0' || value > 0xFFFFFFFFul) {
    std::fprintf(stderr, "not a 32-bit hex colour: %s\n", text);
    std::exit(2);
  }
  return static_cast<uint32_t>(value);
}

} // namespace anonymous

int main(int argc, char** argv) {
  const uint32_t destination = parseArgb(argc > 1 ? argv[1] : nullptr, 0xFF404040u);
  const uint32_t sourcePixel = parseArgb(argc > 2 ? argv[2] : nullptr, 0xFF40BF80u);

  const BlendSource source { (sourcePixel >> 16) & 0xFFu, (sourcePixel >> 8) & 0xFFu, sourcePixel & 0xFFu,
    (sourcePixel >> 24) & 0xFFu };

  std::fprintf(stderr, "destination = %08X   source = %08X\n\n", destination, sourcePixel);

  for (int mode = 0; mode <= static_cast<int>(BlendMode::kLastMode); ++mode) {
    const auto value = static_cast<BlendMode>(mode);
    const uint32_t out = blendWith(value, source, destination, 255);

    std::fprintf(stderr, "%-12s -> %08X  (r=%3u g=%3u b=%3u a=%3u)\n", graphics::toString(value), out,
            (out >> 16) & 0xFFu, (out >> 8) & 0xFFu, out & 0xFFu, (out >> 24) & 0xFFu);
  }
  return 0;
}
