/*
 * A fuzz harness for the path operations.
 *
 * Unit tests describe the cases somebody thought of. This generates paths from a seed and asserts properties that must
 * hold for *every* input, which is the only way to reach the ones nobody thought of. It is run deliberately, often under
 * the sanitizer, rather than on every `ctest`: the point of a fuzzer is to be pointed at a much larger input space than
 * a suite should sit through.
 *
 *   fuzz_pathops [firstSeed] [iterations]
 *
 * ## The properties
 *
 * Only invariants that follow from what the operations mean, so a failure is a defect rather than a taste disagreement:
 *
 *   1. **No operation throws, and no accessor misbehaves.** The baseline.
 *   2. **`Union` contains both operands' bounds**, and **`Intersection` is contained in both.** Bounds come from the
 *      curve rather than from its control points, so this is real containment and not a comparison of two loosely
 *      related rectangles.
 *   3. **The commutative operations agree whichever way round they are asked.** `Union(a,b)` and `Union(b,a)` need not
 *      produce the same representation, but they describe the same region, and bounds are what is observable here.
 *   4. **`simplify` is idempotent.** Simplifying an already-simplified path has nothing left to do.
 *   5. **`Xor` covers the same region as `Difference(a,b) union Difference(b,a)`**, which is what symmetric difference
 *      means.
 *
 * Where an operation reports failure by returning an empty path, the check that depends on it is skipped rather than
 * failed: `op` and `simplify` both document that they can give up, and treating a documented refusal as a defect would
 * make the harness useless.
 *
 * ## The containment tolerance, and what it cannot tell you
 *
 * The containment checks allow a flat `1e-2`, which is an absolute figure and therefore means different things at
 * different scales. `Rect` stores `Scalar`, which is `float`: at coordinates near 1e6 one ulp is about 0.12, so a
 * reported break of 0.03 at that scale is arithmetic and says nothing about the operation, while a break of 1e3 is the
 * operation losing geometry. The harness therefore prints the *magnitude* of every containment failure instead of a bare
 * boolean, and the magnitude -- not the failure -- is what to triage on. A relative tolerance would be the principled
 * fix and is not applied here, because it would also hide the large breaks, which are the interesting ones.
 *
 * ## Coordinates
 *
 * Drawn from several populations rather than one, because the defects in a geometry engine live at the boundaries of what
 * it is asked to represent: exact integers, so curves are axis-aligned and degenerate cases are reachable; values that
 * repeat, so vertices coincide exactly; tiny values, so subtraction loses everything; and values around 1e6, so
 * subtracting two of them is exact while adding them is not.
 *
 * ## Reading the output
 *
 * Every seed is printed before it is run and the stream is unbuffered, because the failure this harness found first was
 * a *hang* rather than a wrong answer: the process never returned, so a report written at the end of the loop was never
 * written at all. The last seed printed is the one that failed, and `--dump <seed>` prints the path it generates in a
 * form that can be pasted into a test.
 *
 * Note that a hang also makes this harness unusable under the sanitizer for that seed -- the run does not finish, so no
 * report is produced. Run it plain first to find the seed, then take the seed to a shorter input before instrumenting.
 */

#include <graphics/Path.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using graphics::Path;
using graphics::PathOp;
using graphics::Point;
using graphics::Rect;
using graphics::Scalar;

/** A tiny deterministic PRNG, so a failing seed can be replayed exactly. */
class Rng {
  public:
  explicit Rng(const uint64_t seed) : mState(seed * 6364136223846793005ULL + 1442695040888963407ULL) {}

  uint64_t next() {
    mState ^= mState << 13;
    mState ^= mState >> 7;
    mState ^= mState << 17;
    return mState;
  }

  /** @return a value in [0, bound). */
  size_t below(const size_t bound) { return bound == 0 ? 0 : static_cast<size_t>(next() % bound); }

  private:
  uint64_t mState;
};

/** Set while a path is being dumped, so the generator prints every verb it emits. */
bool gDump = false;

/**
 * Set by `--modest`, which draws coordinates from the ordinary working ranges only.
 *
 * The default population is deliberately hostile -- features of 1e-3 inside spans of 1e6, which is where the engine's
 * defects live. Measuring a refusal rate against it answers whether the engine can be made to refuse, which is not the
 * question that matters for a change turning a partial result into a refusal. The question is how often it refuses
 * something a person would draw.
 */
bool gModest = false;

void dump(const char* verb, const Point& point) {
  std::printf("  path.%s(Point(%gf, %gf));\n", verb, static_cast<double>(point.x), static_cast<double>(point.y));
}

Scalar coordinate(Rng& rng) {
  if (gModest) {
    // A few hundred units either side of the origin, with the same mix of integers, repeats and detail, so the shapes
    // are still varied and still degenerate in places.
    switch (rng.below(5)) {
      case 0: return 0.0f;
      case 1: return static_cast<Scalar>(rng.below(41)) - 20.0f;
      case 2: return static_cast<Scalar>(rng.below(2001)) / 100.0f - 10.0f;
      case 3: return static_cast<Scalar>(rng.below(2001)) / 10.0f - 100.0f;
      default: return static_cast<Scalar>(rng.below(3)) - 1.0f;
    }
  }
  switch (rng.below(8)) {
    case 0: return 0.0f;                                                      // repeated, so vertices coincide
    case 1: return static_cast<Scalar>(rng.below(21)) - 10.0f;                // integers: axis-aligned, degenerate
    case 2: return static_cast<Scalar>(rng.below(2001)) / 1000.0f - 1.0f;     // fine detail near the origin
    case 3: return static_cast<Scalar>(rng.below(200001)) / 1000.0f - 100.0f; // the ordinary working range
    case 4: return static_cast<Scalar>(rng.below(2000001)) / 1000.0f - 1000.0f;  // large enough to lose precision
    case 5: return 1e-3f;                                                     // tiny: subtracting loses everything
    case 6: return 1e6f;                                                      // huge: adding loses precision
    default: return static_cast<Scalar>(rng.below(3)) - 1.0f;                 // -1, 0, 1
  }
}

Point point(Rng& rng) { return Point(coordinate(rng), coordinate(rng)); }

/** Builds a path of @p contours, each a run of random verbs. */
Path makePath(Rng& rng, const size_t contours) {
  Path path;
  for (size_t contour = 0; contour < contours; ++contour) {
    const Point start = point(rng);
    if (gDump) dump("moveTo", start);
    path.moveTo(start);

    const size_t segments = rng.below(6);
    for (size_t segment = 0; segment < segments; ++segment) {
      switch (rng.below(4)) {
        case 0: {
          const Point a = point(rng);
          if (gDump) dump("lineTo", a);
          path.lineTo(a);
          break;
        }
        case 1: {
          const Point control = point(rng);
          const Point a = point(rng);
          if (gDump) { dump("__quad_control", control); dump("quadTo", a); }
          path.quadTo(control, a);
          break;
        }
        case 2: {
          const Point c1 = point(rng);
          const Point c2 = point(rng);
          const Point a = point(rng);
          if (gDump) { dump("__cubic_c1", c1); dump("__cubic_c2", c2); dump("cubicTo", a); }
          path.cubicTo(c1, c2, a);
          break;
        }
        default: {
          // Two lines rather than one, so a contour has more than one way to be polygonal.
          const Point a = point(rng);
          const Point b = point(rng);
          if (gDump) { dump("lineTo", a); dump("lineTo", b); }
          path.lineTo(a);
          path.lineTo(b);
          break;
        }
      }
    }
    if (rng.below(4) != 0) {
      if (gDump) std::printf("  path.close();\n");
      path.close();
    }
    if (gDump) std::printf("\n");
  }
  return path;
}

/**
 * How far @p inner reaches outside @p outer, in the units of the coordinates.
 *
 * A boolean rather than a distance is not enough to triage a failure. A containment break of 1e-2 on coordinates around
 * 1e6 is a rounding artefact and says nothing about the operation; a break of 1e3 says the operation is wrong. Reporting
 * the number is what separates those, and the harness's own epsilon is only defensible if that number is tiny next to
 * the geometry.
 *
 * @return 0 when @p inner is within @p outer.
 */
Scalar overshoot(const Rect& outer, const Rect& inner) {
  if (inner.isEmpty() || outer.isEmpty()) return 0.0f;
  return std::max({ outer.getLeft() - inner.getLeft(), outer.getTop() - inner.getTop(),
                    inner.getRight() - outer.getRight(), inner.getBottom() - outer.getBottom(), 0.0f });
}

/** True when @p inner lies within @p outer, allowing for accumulated rounding. */
bool contains(const Rect& outer, const Rect& inner, const Scalar epsilon = 1e-2f) {
  if (inner.isEmpty()) return true;
  if (outer.isEmpty()) return false;
  return overshoot(outer, inner) <= epsilon;
}

int gFailures = 0;

/** How many times `simplify` was given a non-empty path and returned nothing. */
int gRefusals = 0;

void fail(const uint64_t seed, const char* property, const Scalar magnitude = 0.0f) {
  ++gFailures;
  std::printf("FAIL seed=%llu property=%s magnitude=%.9g\n", static_cast<unsigned long long>(seed), property,
              static_cast<double>(magnitude));
}

/** Prints a rectangle so a failure names the geometry rather than only a distance. */
void printRect(const char* label, const Rect& rect) {
  std::printf("       %s %g,%g,%g,%g empty=%d\n", label, static_cast<double>(rect.getLeft()),
              static_cast<double>(rect.getTop()), static_cast<double>(rect.getRight()),
              static_cast<double>(rect.getBottom()), rect.isEmpty() ? 1 : 0);
}

/** How many interior samples of @p inner fall outside @p outer, and how many were tested. */
struct Escapes {
  int inside = 0;
  int escaped = 0;
  std::vector<Point> where;
};

/**
 * Samples a grid over @p inner's ink bounds and counts the points that are inside @p inner and outside @p outer.
 *
 * This is the sound form of "the result contains the operand": `contains` answers the question about the *region*, which
 * is what the operation is defined on, while comparing bounding rectangles answers it about the ink and can be wrong in
 * both directions. Points on an edge are ambiguous, so the caller requires a fraction to escape rather than one.
 */
Escapes sampleEscapes(const Path& outer, const Path& inner, const int steps = 9) {
  Escapes escapes;
  const Rect rect = inner.tightBounds();
  if (rect.isEmpty()) return escapes;

  for (int i = 0; i <= steps; ++i) {
    for (int j = 0; j <= steps; ++j) {
      // Inset from the edge, because a sample exactly on a boundary is a coin toss for either path.
      const Scalar fraction = 0.02f + 0.96f * static_cast<Scalar>(i) / static_cast<Scalar>(steps);
      const Scalar fy = 0.02f + 0.96f * static_cast<Scalar>(j) / static_cast<Scalar>(steps);
      const Point point { rect.getLeft() + (rect.getRight() - rect.getLeft()) * fraction,
                          rect.getTop() + (rect.getBottom() - rect.getTop()) * fy };
      if (!inner.contains(point)) continue;
      ++escapes.inside;
      if (!outer.contains(point)) {
        ++escapes.escaped;
        if (escapes.where.size() < 3) escapes.where.push_back(point);
      }
    }
  }
  return escapes;
}

/**
 * Reports when points inside @p inner are outside @p outer, which is what it means for @p outer not to contain @p inner.
 *
 * A tenth of the interior is the threshold: below that, the samples are at edges and the answer is about arithmetic
 * rather than about the operation.
 */
void failRegion(const uint64_t seed, const char* property, const Path& outer, const Path& inner) {
  const Escapes escapes = sampleEscapes(outer, inner);
  if (escapes.inside < 4 || escapes.escaped * 10 <= escapes.inside) return;
  ++gFailures;
  std::printf("FAIL seed=%llu property=%s escaped=%d of %d inside\n", static_cast<unsigned long long>(seed), property,
              escapes.escaped, escapes.inside);
  for (const Point& point : escapes.where) {
    std::printf("       point %g,%g is inside the inner path and outside the outer one\n", static_cast<double>(point.x),
                static_cast<double>(point.y));
  }
}

/** Every query the public surface offers, so a misbehaving accessor is a failure rather than a silent skip. */
void touchEveryAccessor(const Path& path) {
  (void) path.isEmpty();
  (void) path.verbCount();
  (void) path.pointCount();
  (void) path.hasCurves();
  (void) path.isLine();
  (void) path.bounds();
  (void) path.tightBounds();
  (void) path.length();
  Rect rect;
  (void) path.isRect(&rect);
  (void) path.contains(Point(0.0f, 0.0f));
  (void) path.reversed();
}

const PathOp kCommutative[] = { PathOp::Union, PathOp::Intersection, PathOp::Xor };
const PathOp kAllOps[] = { PathOp::Union, PathOp::Intersection, PathOp::Difference, PathOp::Xor,
                           PathOp::ReverseDifference };

void runOne(const uint64_t seed) {
  Rng rng(seed);

  const Path first = makePath(rng, 1 + rng.below(3));
  const Path second = makePath(rng, 1 + rng.below(3));

  touchEveryAccessor(first);
  touchEveryAccessor(second);

  const Path simplified = graphics::simplify(first);
  touchEveryAccessor(simplified);

  // Counted rather than failed: `simplify` documents that it may give up, so an empty result for a non-empty input is a
  // legal outcome. It is still the number to watch when the classification changes, because a rule that resolves more
  // cases by refusing them would otherwise look like an improvement -- a refusal is trivially idempotent, so it inflates
  // the idempotence count while destroying output.
  if (!first.isEmpty() && simplified.isEmpty()) {
    ++gRefusals;
  }

  const Path simplifiedTwice = graphics::simplify(simplified);
  if (!(simplified == simplifiedTwice)) {
    // Reported as the two counts and the bounds rather than just "not equal", because there are two very different
    // reasons this can fail: the second pass changed the geometry, or it produced the same region with a different
    // number of vertices. Only the first is a defect, and equality alone cannot tell them apart.
    fail(seed, "simplify-is-idempotent", overshoot(simplified.tightBounds(), simplifiedTwice.tightBounds()));
    std::printf("       verbs %zu -> %zu, points %zu -> %zu, bounds %g,%g,%g,%g -> %g,%g,%g,%g\n",
                simplified.verbCount(), simplifiedTwice.verbCount(), simplified.pointCount(),
                simplifiedTwice.pointCount(), static_cast<double>(simplified.tightBounds().getLeft()),
                static_cast<double>(simplified.tightBounds().getTop()),
                static_cast<double>(simplified.tightBounds().getRight()),
                static_cast<double>(simplified.tightBounds().getBottom()),
                static_cast<double>(simplifiedTwice.tightBounds().getLeft()),
                static_cast<double>(simplifiedTwice.tightBounds().getTop()),
                static_cast<double>(simplifiedTwice.tightBounds().getRight()),
                static_cast<double>(simplifiedTwice.tightBounds().getBottom()));
  }

  for (const PathOp pathOp : kAllOps) {
    const Path result = graphics::op(first, second, pathOp);
    touchEveryAccessor(result);
    const Rect resultBounds = result.tightBounds();

    if (!result.isEmpty()) {
      // Region containment, sampled -- not bound containment.
      //
      // The first version of these checks compared bounding rectangles, and that is not a sound test of what a union
      // means. A union's region contains each operand's region, so its *ink* bounds contain each operand's *filled
      // region* bounds -- but an operand's ink bounds can be far larger than its own filled region, so comparing ink
      // bounds against ink bounds proves nothing. Two things make that gap wide, and the generator produces both: an
      // open contour is closed implicitly when it is filled, and a contour that crosses itself has outline the fill rule
      // discards. Every "union does not contain its operand" this harness reported before was that mistake; measured on
      // seed 2, the operand's ink spans 1e6 while its filled region is a sliver, and the union matched the sliver.
      //
      // `contains` asks the question directly, so sampling it is sound. Points near an edge are genuinely ambiguous, so
      // a failure needs a tenth of the interior samples to escape rather than one.
      if (pathOp == PathOp::Union) {
        failRegion(seed, "union-contains-first", result, first);
        failRegion(seed, "union-contains-second", result, second);
      }
      if (pathOp == PathOp::Intersection) {
        failRegion(seed, "intersection-within-first", first, result);
        failRegion(seed, "intersection-within-second", second, result);
      }
    }

    const bool commutative = std::find(std::begin(kCommutative), std::end(kCommutative), pathOp) !=
                             std::end(kCommutative);
    if (commutative) {
      const Path swapped = graphics::op(second, first, pathOp);
      const Rect swappedBounds = swapped.tightBounds();
      if (!(result == swapped) &&
          !(contains(resultBounds, swappedBounds) && contains(swappedBounds, resultBounds))) {
        // How far apart the two orders land. A few units on coordinates of a million is rounding; a different order of
        // magnitude is the operation genuinely disagreeing with itself.
        fail(seed, "commutative-operands-agree",
             std::max(overshoot(resultBounds, swappedBounds), overshoot(swappedBounds, resultBounds)));
      }
    }
  }

  const Path symmetric = graphics::op(first, second, PathOp::Xor);
  const Path aNotB = graphics::op(first, second, PathOp::Difference);
  const Path bNotA = graphics::op(second, first, PathOp::Difference);
  if (!symmetric.isEmpty() && !aNotB.isEmpty() && !bNotA.isEmpty()) {
    const Path unionOfDifferences = graphics::op(aNotB, bNotA, PathOp::Union);
    if (!unionOfDifferences.isEmpty()) {
      const Rect a = symmetric.tightBounds();
      const Rect b = unionOfDifferences.tightBounds();
      if (!contains(a, b) || !contains(b, a)) {
        fail(seed, "xor-equals-union-of-differences", std::max(overshoot(a, b), overshoot(b, a)));
      }
    }
  }
}

} // namespace anonymous

int main(int argc, char** argv) {
  // Unbuffered, and that is not a detail. The first failure this harness found was a hang, and with a block-buffered
  // stdout the abort that ended the process took every report with it -- including the seed, which is the one thing
  // needed to reproduce. A harness whose output disappears exactly when it finds something is worse than none.
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  // `--dump <seed>` prints the path a seed generates, in a form that can be pasted into a test.
  // Flags are collected first and the positional arguments are the ones that are not flags, so `--modest 1 60` means
  // seed 1 with 60 iterations rather than seed 0 with one.
  std::vector<const char*> positional;
  for (int index = 1; index < argc; ++index) {
    if (std::string(argv[index]) == "--modest") {
      gModest = true;
    } else {
      positional.push_back(argv[index]);
    }
  }

  if (argc > 2 && std::string(argv[1]) == "--dump") {
    gDump = true;
    Rng rng(std::strtoull(argv[2], nullptr, 10));
    std::printf("Path path;\n");
    (void) makePath(rng, 1 + rng.below(3));
    return 0;
  }

  const uint64_t firstSeed = positional.size() > 0 ? std::strtoull(positional[0], nullptr, 10) : 1;
  const uint64_t iterations = positional.size() > 1 ? std::strtoull(positional[1], nullptr, 10) : 2000;

  std::printf("fuzz_pathops: seeds %llu..%llu, %llu iterations\n", static_cast<unsigned long long>(firstSeed),
              static_cast<unsigned long long>(firstSeed + iterations - 1),
              static_cast<unsigned long long>(iterations));

  for (uint64_t index = 0; index < iterations; ++index) {
    const uint64_t seed = firstSeed + index;
    // Printed and flushed before the work, so a seed that hangs or aborts is still identified.
    std::printf("seed %llu\n", static_cast<unsigned long long>(seed));
    runOne(seed);
  }

  std::printf(
          "done: %llu iterations from seed %llu, %d failure(s), %d first-pass refusal(s) of a non-empty path\n",
          static_cast<unsigned long long>(iterations), static_cast<unsigned long long>(firstSeed), gFailures,
          gRefusals);
  return gFailures == 0 ? 0 : 1;
}
