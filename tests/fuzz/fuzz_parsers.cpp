/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file fuzz_parsers.cpp
 *
 * A fuzz harness for the three parsers that see untrusted files: SVG, Lottie and the image codecs.
 * Each one is exercised through its public entry point, which is also the path the product takes, so
 * the limits and the refusal paths are fuzzed together with the happy path.
 *
 * ## Two ways to run it
 *
 * `LLVMFuzzerTestOneInput` is the libFuzzer entry point: build with `-fsanitize=fuzzer` and the harness
 * owns no `main`. Without libFuzzer (MSVC, or a quick check) the fallback `main` reads the files named
 * on the command line, or generates deterministic mutations of a small seed corpus when given no
 * arguments:
 *
 *   fuzz_parsers                     # built-in corpus + mutations
 *   fuzz_parsers svg a.svg b.svg     # one kind, explicit files
 *   fuzz_parsers image icon.png
 *
 * The kinds are `svg`, `lottie` and `image`; the file's bytes decide nothing about which parser runs.
 *
 * ## What a failure means
 *
 * The harness asserts nothing about the output: a parser is allowed to refuse any input. What it must
 * not do is crash, hang or allocate without bound. Run it under AddressSanitizer and with the
 * process-wide decode limits at their defaults.
 */

#include <graphics/ImageCodec.h>
#include <lottie/Lottie.h>
#include <svg/Svg.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

enum class Kind { Svg, Lottie, Image };

void parseSvg(const std::string_view input) {
  std::string error;
  (void)graphics::Svg::parse(input, &error);
}

void parseLottie(const std::string_view input) {
  std::string error;
  (void)graphics::Lottie::parse(input, &error);
}

void parseImage(const std::string_view input) {
  graphics::DecodeOptions options;
  (void)graphics::decodeImage(reinterpret_cast<const uint8_t*>(input.data()), input.size(), options);
}

void parse(const Kind kind, const std::string_view input) {
  switch (kind) {
    case Kind::Svg: parseSvg(input); break;
    case Kind::Lottie: parseLottie(input); break;
    case Kind::Image: parseImage(input); break;
  }
}

/** @return the kind named by @p name, or null when it is not one of the known kinds. */
bool kindFromName(const std::string& name, Kind* kind) {
  if (name == "svg") { *kind = Kind::Svg; return true; }
  if (name == "lottie") { *kind = Kind::Lottie; return true; }
  if (name == "image") { *kind = Kind::Image; return true; }
  return false;
}

/**
 * A tiny seed corpus per kind, plus deterministic mutations.
 *
 * A fuzzer with a lexer-level corpus is better, but this is what makes the harness useful without
 * libFuzzer: the seeds are the smallest inputs that reach each parser's document setup, and the
 * mutations walk the byte values that trip length and state handling (0, 0xFF, the byte before and
 * after a structural character).
 */
std::vector<std::string> seedCorpus(const Kind kind) {
  switch (kind) {
    case Kind::Svg:
      return {
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"4\" height=\"4\"><rect width=\"4\" height=\"4\"/></svg>",
        "<svg width=\"4\" height=\"4\"><path d=\"M0 0L4 0L4 4Z\"/></svg>",
        "<svg width=\"4\" height=\"4\"><g transform=\"translate(1,1)\"><circle r=\"2\"/></g></svg>",
      };
    case Kind::Lottie:
      return {
        R"({"v":"5.7.0","w":4,"h":4,"fr":30,"ip":0,"op":30,"layers":[]})",
        R"({"v":"5.7.0","w":4,"h":4,"fr":30,"ip":0,"op":30,"layers":[{"ty":4,"ks":{"o":{"a":0,"k":100}}}]})",
      };
    case Kind::Image: {
      const unsigned char png[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
      return { std::string(reinterpret_cast<const char*>(png), sizeof(png)) };
    }
  }
  return {};
}

/** @return a copy of @p seed with a few bytes flipped, deterministically for @p iteration. */
std::string mutate(const std::string& seed, const std::size_t iteration) {
  std::string out = seed;
  if (out.empty()) return out;
  // A cheap LCG keeps the same iterations in the same order on every platform, so a failing iteration
  // is reproducible without saving the corpus.
  std::uint32_t state = static_cast<std::uint32_t>(iteration * 2654435761u + 1u);
  const auto next = [&state] {
    state = state * 1664525u + 1013904223u;
    return state;
  };
  const std::size_t flips = 1 + (next() % 4u);
  for (std::size_t index = 0; index < flips; ++index) {
    const std::size_t offset = next() % out.size();
    const std::uint32_t choice = next() % 4u;
    const unsigned char values[4] = { 0x00, 0xFF, static_cast<unsigned char>('<'), static_cast<unsigned char>('"') };
    out[offset] = static_cast<char>(values[choice]);
  }
  if ((next() & 1u) != 0u) {
    out.resize(1 + next() % out.size());
  }
  return out;
}

bool readFile(const char* path, std::string* out) {
  std::FILE* file = std::fopen(path, "rb");
  if (file == nullptr) return false;
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (size <= 0) {
    std::fclose(file);
    return false;
  }
  out->resize(static_cast<std::size_t>(size));
  const std::size_t read = std::fread(out->data(), 1, out->size(), file);
  std::fclose(file);
  out->resize(read);
  return true;
}

} // namespace

#if defined(CORAL_FUZZ_WITH_LIBFUZZER)
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, const size_t size) {
  // libFuzzer has no kind argument, so a byte in the input picks it: the first byte chooses the
  // parser and the rest is the document. That keeps one target able to reach all three parsers.
  if (size < 2) return 0;
  const int kind = data[0] % 3;
  const std::string_view document(reinterpret_cast<const char*>(data + 1), size - 1);
  parse(static_cast<Kind>(kind), document);
  return 0;
}
#else
int main(const int argc, char** argv) {
  if (argc >= 3) {
    Kind kind = Kind::Svg;
    if (!kindFromName(argv[1], &kind)) {
      std::fprintf(stderr, "unknown kind '%s' (svg, lottie, image)\n", argv[1]);
      return 2;
    }
    for (int index = 2; index < argc; ++index) {
      std::string document;
      if (!readFile(argv[index], &document)) {
        std::fprintf(stderr, "could not read %s\n", argv[index]);
        return 2;
      }
      parse(kind, document);
    }
    std::printf("parsed %d file(s) as %s\n", argc - 2, argv[1]);
    return 0;
  }
  const Kind kinds[] = { Kind::Svg, Kind::Lottie, Kind::Image };
  std::size_t iterations = 0;
  for (const Kind kind: kinds) {
    const std::vector<std::string> seeds = seedCorpus(kind);
    for (const std::string& seed: seeds) {
      parse(kind, seed);
      for (std::size_t step = 0; step < 400; ++step) {
        parse(kind, mutate(seed, step));
        ++iterations;
      }
    }
  }
  std::printf("fuzz_parsers: %zu mutations across 3 parsers\n", iterations);
  return 0;
}
#endif
