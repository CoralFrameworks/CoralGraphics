/*
 * Copyright (C) 2026 The Coral Project
 *
 * A one-off check: decode each resource and write it back out, to tell a decoder problem from a display problem.
 * Kept out of the demo's build; compile it by hand when a decode looks wrong.
 *
 * Usage:  decode_dump <input> <output.png>
 */

#include <graphics/Bitmap.h>
#include <graphics/Data.h>
#include <graphics/Image.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: decode_dump <input> <output.png>\n");
    return 1;
  }

  std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
  if (!file.good()) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 1;
  }

  const auto size = file.tellg();
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  file.seekg(0, std::ios::beg);
  file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

  std::string error;
  const graphics::Image image = graphics::Image::decode(graphics::Data::makeCopy(bytes.data(), bytes.size()), &error);
  if (image.isEmpty()) {
    std::fprintf(stderr, "decode failed: %s\n", error.c_str());
    return 1;
  }

  // The colour and alpha types are reported as numbers: `toString` covers the enums a log line usually needs and these
  // are not among them, and adding an overload for a diagnostic tool would be the tail wagging the dog.
  std::fprintf(stderr, "decoded %s: %dx%d, colorType %d, alphaType %d\n", argv[1], image.width(), image.height(),
          static_cast<int>(image.info().colorType()), static_cast<int>(image.info().alphaType()));

  // Re-encoded rather than copied, so the output is a PNG whatever the input was.
  std::string encodeError;
  const graphics::Data out = image.encode(graphics::ImageFormat::PNG, 100, &encodeError);
  if (out.size() == 0) {
    std::fprintf(stderr, "encode failed: %s\n", encodeError.c_str());
    return 1;
  }

  std::ofstream outFile(argv[2], std::ios::binary);
  outFile.write(reinterpret_cast<const char*>(out.bytes()), static_cast<std::streamsize>(out.size()));
  std::fprintf(stderr, "wrote %s (%zu bytes)\n", argv[2], out.size());
  return 0;
}
