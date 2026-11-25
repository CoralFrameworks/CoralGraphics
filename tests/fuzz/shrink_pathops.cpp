/*
 * A probe that builds one subset of the path `graphics::simplify` hangs on, and simplifies it.
 *
 * Called with a decimal bitmask: bit N set means "keep verb N". The path is built from the eight verbs the fuzz harness
 * printed and then simplified. The program prints nothing useful and does not need to: it either returns, or it does not,
 * and a driver reads that from whether the process exited.
 *
 * ## Why a process per candidate
 *
 * The first attempt at this ran each candidate on a worker thread with a deadline, and it crashed with an access
 * violation. The cause was mine: a hung worker was detached, and the vector it had captured by reference went out of
 * scope when the loop moved on, so the still-running worker read freed memory. There is no way to make a hung call
 * return, so any in-process scheme has to keep every hung worker alive for the life of the program.
 *
 * A process per candidate has no such problem -- the operating system reclaims a process that is still stuck -- and it
 * makes the search reproducible from a shell, which matters more than speed for a defect that has to be reported.
 */

#include <graphics/Path.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using graphics::Path;
using graphics::Point;

struct Verb {
  const char* kind;
  double x1, y1, x2, y2, x3, y3;
};

/** The failing path exactly as the fuzz harness printed it. */
const std::vector<Verb> kFound = {
  { "move",  0.0,     1e6,     0,       0,       0,     0 },
  { "cubic", -0.796, -111.651, 0.001,   -0.562,  0.001, 1.759 },
  { "line",  320.251, 0.001,   0,       0,       0,     0 },
  { "cubic", 1.0,     1e6,    -189.714, -108.254, 0.0,  -0.458 },
  { "close", 0, 0, 0, 0, 0, 0 },
  { "move",  0.0,     0.001,   0,       0,       0,     0 },
  { "quad",  -0.242, -66.159, 1e6,     -401.895, 0,     0 },
  { "close", 0, 0, 0, 0, 0, 0 },
};

Path build(const unsigned mask) {
  Path path;
  const auto f = [](const double value) { return static_cast<float>(value); };
  for (size_t index = 0; index < kFound.size(); ++index) {
    if ((mask & (1u << index)) == 0) {
      continue;
    }
    const Verb& verb = kFound[index];
    const std::string kind = verb.kind;
    if (kind == "move") {
      path.moveTo(Point(f(verb.x1), f(verb.y1)));
    } else if (kind == "line") {
      path.lineTo(Point(f(verb.x1), f(verb.y1)));
    } else if (kind == "quad") {
      path.quadTo(Point(f(verb.x1), f(verb.y1)), Point(f(verb.x2), f(verb.y2)));
    } else if (kind == "cubic") {
      path.cubicTo(Point(f(verb.x1), f(verb.y1)), Point(f(verb.x2), f(verb.y2)), Point(f(verb.x3), f(verb.y3)));
    } else if (kind == "close") {
      path.close();
    }
  }
  return path;
}

} // namespace anonymous

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const unsigned mask = argc > 1 ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10)) : 0xFFu;

  const Path path = build(mask);
  std::printf("mask=%u verbs=%zu points=%zu\n", mask, path.verbCount(), path.pointCount());
  const Path simplified = graphics::simplify(path);
  std::printf("simplified: verbs=%zu points=%zu\n", simplified.verbCount(), simplified.pointCount());
  return 0;
}
