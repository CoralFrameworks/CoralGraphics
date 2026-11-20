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
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file SvgPathData.cpp
 *
 * The SVG path `d` attribute.
 *
 * ## The three things that make this more than a command table
 *
 * **Repetition.** After a command letter, further argument groups repeat that command. `L 1 2 3 4` is two lines, and
 * `M 0 0 1 1 2 2` is a move followed by *two lines* -- not two more moves, which is the rule that catches people out.
 *
 * **Relative coordinates are relative to the current point**, not to the start of the subpath, and `Z` moves the current
 * point back to where the subpath began. Getting that wrong shows up only on a path with more than one subpath.
 *
 * **The smooth commands carry state.** `S` and `T` are defined in terms of the reflection of the previous command's
 * control point about the current point -- and only when the previous command was of the matching kind. After anything
 * else the reflection is the current point itself, which makes the smooth command a straight line. Carrying the state
 * unconditionally bends a line that should be straight, which is a mistake that renders as a picture that is almost
 * right.
 *
 * ## Arcs
 *
 * `A` takes an endpoint parameterisation -- two radii, a rotation, two flags and an endpoint -- which is not something a
 * renderer can draw. The conversion to a centre parameterisation and then to cubic Beziers is the algorithm from the SVG
 * specification's implementation notes, and it is here rather than in the renderer because it is part of what the
 * attribute *means*.
 */

#include "svg/SvgTypes.h"

#include "SvgScanner.h"

#include <algorithm>
#include <cmath>

namespace graphics::svg {
namespace {

constexpr Scalar kPi = 3.14159265358979323846f;

/**
 * Appends the cubic Beziers approximating one elliptical arc.
 *
 * The centre parameterisation from the specification's appendix, then each sweep broken into segments no larger than a
 * quarter turn -- the error of the standard cubic approximation grows with the angle, and a quarter turn is where it is
 * still well under a pixel for any radius this is used with.
 */
void appendArc(Path& path, const Point from, const Scalar radiusX, const Scalar radiusY, const Scalar rotationDegrees,
               const bool largeArc, const bool sweep, const Point to) {
  // A zero radius cannot describe an arc, and the specification says the command is then a straight line to the
  // endpoint. Dropping it would leave a gap in the outline, which fills differently -- so it becomes a line here.
  if (radiusX == 0 || radiusY == 0) {
    path.lineTo(to);
    return;
  }
  if (from.x == to.x && from.y == to.y) {
    // An arc whose endpoints coincide draws nothing at all, rather than a full circle.
    return;
  }

  const Scalar phi = rotationDegrees * kPi / 180.0f;
  const Scalar cosPhi = std::cos(phi);
  const Scalar sinPhi = std::sin(phi);

  // Step 1: the midpoint difference, rotated into the ellipse's own frame.
  const Scalar dx = (from.x - to.x) / 2;
  const Scalar dy = (from.y - to.y) / 2;
  const Scalar x1 = cosPhi * dx + sinPhi * dy;
  const Scalar y1 = -sinPhi * dx + cosPhi * dy;

  Scalar rx = std::abs(radiusX);
  Scalar ry = std::abs(radiusY);

  // Step 2: enlarge the radii when they are too small to span the endpoints. The specification requires this rather
  // than failing, and it is what makes a hand-written `A 1 1 ...` between distant points render as a visible curve.
  const Scalar lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
  if (lambda > 1) {
    const Scalar scale = std::sqrt(lambda);
    rx *= scale;
    ry *= scale;
  }

  // Step 3: the centre, in the ellipse's frame and then rotated back.
  const Scalar numerator = std::max(0.0f, rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1);
  const Scalar denominator = rx * rx * y1 * y1 + ry * ry * x1 * x1;
  Scalar factor = denominator > 0 ? std::sqrt(numerator / denominator) : 0;
  if (largeArc == sweep) factor = -factor;

  const Scalar centreX = factor * rx * y1 / ry;
  const Scalar centreY = -factor * ry * x1 / rx;
  const Scalar cx = cosPhi * centreX - sinPhi * centreY + (from.x + to.x) / 2;
  const Scalar cy = sinPhi * centreX + cosPhi * centreY + (from.y + to.y) / 2;

  // Step 4: the start angle and the sweep, both in the ellipse's frame.
  const auto angleOf = [](const Scalar ux, const Scalar uy, const Scalar vx, const Scalar vy) {
    const Scalar dot = ux * vx + uy * vy;
    const Scalar lengths = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
    Scalar angle = lengths > 0 ? std::acos(std::clamp(dot / lengths, -1.0f, 1.0f)) : 0;
    if (ux * vy - uy * vx < 0) angle = -angle;
    return angle;
  };

  const Scalar ux = (x1 - centreX) / rx;
  const Scalar uy = (y1 - centreY) / ry;
  const Scalar vx = (-x1 - centreX) / rx;
  const Scalar vy = (-y1 - centreY) / ry;

  const Scalar startAngle = angleOf(1, 0, ux, uy);
  Scalar sweepAngle = angleOf(ux, uy, vx, vy);
  if (!sweep && sweepAngle > 0) sweepAngle -= 2 * kPi;
  if (sweep && sweepAngle < 0) sweepAngle += 2 * kPi;

  const int segments = std::max(1, static_cast<int>(std::ceil(std::abs(sweepAngle) / (kPi / 2))));
  const Scalar delta = sweepAngle / static_cast<Scalar>(segments);

  // The standard cubic constant for a circular arc of angle t: 4/3 * tan(t/4).
  const Scalar alpha = 4.0f / 3.0f * std::tan(delta / 4);

  Scalar angle = startAngle;
  Scalar cosAngle = std::cos(angle);
  Scalar sinAngle = std::sin(angle);

  for (int i = 0; i < segments; ++i) {
    const Scalar nextAngle = angle + delta;
    const Scalar cosNext = std::cos(nextAngle);
    const Scalar sinNext = std::sin(nextAngle);

    // The two control points, in the ellipse's frame.
    const Scalar c1x = cosAngle - alpha * sinAngle;
    const Scalar c1y = sinAngle + alpha * cosAngle;
    const Scalar c2x = cosNext + alpha * sinNext;
    const Scalar c2y = sinNext - alpha * cosNext;

    // And back out through the radius, the rotation and the centre.
    //
    // The unit circle is mapped to the ellipse by scaling each axis by its own radius and *then* rotating -- so the two
    // radii multiply different terms. Writing this as one factor applied to the whole expression is the mistake that
    // makes a circular arc on a non-circular ellipse wrong everywhere except at the axes.
    const auto toUser = [&](const Scalar unitX, const Scalar unitY) {
      const Scalar ex = rx * unitX;
      const Scalar ey = ry * unitY;
      return Point {cx + cosPhi * ex - sinPhi * ey, cy + sinPhi * ex + cosPhi * ey};
    };

    path.cubicTo(toUser(c1x, c1y), toUser(c2x, c2y),
                 // The last segment ends at the endpoint the command asked for, taken from the command rather than
                 // recomputed: a chain of arcs then shares its vertices exactly instead of accumulating a
                 // rounding-sized seam at every joint.
                 i + 1 == segments ? to : toUser(cosNext, sinNext));

    angle = nextAngle;
    cosAngle = cosNext;
    sinAngle = sinNext;
  }
}

/**
 * Parses the flag a large-arc or sweep argument is written as.
 *
 * SVG allows `A 5 5 0 0 1 10 10` and also `A 5 5 0 0110 10`, where `0` and `1` run together: a flag is a single
 * character, not a number, so it terminates itself. Reading it with the number scanner would swallow `0110` as one
 * number, which is why this exists.
 */
std::optional<bool> takeFlag(Scanner& scanner) {
  scanner.skipSpaceAndCommas();
  const char c = scanner.peek();
  if (c == '0') {
    scanner.take("0");
    return false;
  }
  if (c == '1') {
    scanner.take("1");
    return true;
  }
  return std::nullopt;
}

} // namespace anonymous

std::optional<Path> parsePathData(const std::string_view text) {
  Path path;
  Scanner scanner(text);

  // The state the relative commands and the smooth commands both need. `current` is where the pen is; `subpathStart`
  // is where `Z` returns it to; the two control points are the last ones written, which `S` and `T` reflect.
  Point current {0, 0};
  Point subpathStart {0, 0};
  Point lastCubicControl {0, 0};
  Point lastQuadControl {0, 0};
  bool hasCubicControl = false;
  bool hasQuadControl = false;
  char previousCommand = '\0';

  // Whitespace and separators may precede the first command; anything else is malformed.
  scanner.skipSpaceAndCommas();
  if (scanner.rest().empty()) return path;

  char command = '\0';
  for (;;) {
    scanner.skipSpaceAndCommas();
    if (scanner.rest().empty()) break;

    const char next = scanner.peek();
    if ((next >= 'A' && next <= 'Z') || (next >= 'a' && next <= 'z')) {
      command = next;
      scanner.take(std::string_view {&command, 1});
    } else if (command == '\0') {
      // A number with no command before it: the data is malformed rather than merely empty.
      return std::nullopt;
    } else if (command == 'M') {
      // A repeated `M` argument group is an implicit `L`, in the same case as the `M` that started it.
      command = 'L';
    } else if (command == 'm') {
      command = 'l';
    }

    const bool relative = command >= 'a' && command <= 'z';
    const auto take = [&scanner]() -> std::optional<Scalar> { return scanner.takeNumber(); };
    const auto point = [&](const Scalar x, const Scalar y) {
      return relative ? Point {current.x + x, current.y + y} : Point {x, y};
    };

    switch (command) {
      case 'M':
      case 'm': {
        const auto x = take();
        const auto y = x ? take() : std::nullopt;
        if (!x || !y) return std::nullopt;
        current = point(*x, *y);
        path.moveTo(current);
        subpathStart = current;
        hasCubicControl = false;
        hasQuadControl = false;
        break;
      }

      case 'L':
      case 'l': {
        const auto x = take();
        const auto y = x ? take() : std::nullopt;
        if (!x || !y) return std::nullopt;
        current = point(*x, *y);
        path.lineTo(current);
        hasCubicControl = false;
        hasQuadControl = false;
        break;
      }

      case 'H':
      case 'h': {
        const auto x = take();
        if (!x) return std::nullopt;
        current = Point {relative ? current.x + *x : *x, current.y};
        path.lineTo(current);
        hasCubicControl = false;
        hasQuadControl = false;
        break;
      }

      case 'V':
      case 'v': {
        const auto y = take();
        if (!y) return std::nullopt;
        current = Point {current.x, relative ? current.y + *y : *y};
        path.lineTo(current);
        hasCubicControl = false;
        hasQuadControl = false;
        break;
      }

      case 'C':
      case 'c': {
        const auto x1 = take();
        const auto y1 = x1 ? take() : std::nullopt;
        const auto x2 = y1 ? take() : std::nullopt;
        const auto y2 = x2 ? take() : std::nullopt;
        const auto x = y2 ? take() : std::nullopt;
        const auto y = x ? take() : std::nullopt;
        if (!y) return std::nullopt;

        const Point control1 = point(*x1, *y1);
        const Point control2 = point(*x2, *y2);
        current = point(*x, *y);
        path.cubicTo(control1, control2, current);
        lastCubicControl = control2;
        hasCubicControl = true;
        hasQuadControl = false;
        break;
      }

      case 'S':
      case 's': {
        const auto x2 = take();
        const auto y2 = x2 ? take() : std::nullopt;
        const auto x = y2 ? take() : std::nullopt;
        const auto y = x ? take() : std::nullopt;
        if (!y) return std::nullopt;

        // The reflection is only meaningful after a cubic; after anything else the control point is the current point,
        // which is what makes this a straight line rather than an invented curve.
        const Point control1 = hasCubicControl
                ? Point {2 * current.x - lastCubicControl.x, 2 * current.y - lastCubicControl.y}
                : current;
        const Point control2 = point(*x2, *y2);
        current = point(*x, *y);
        path.cubicTo(control1, control2, current);
        lastCubicControl = control2;
        hasCubicControl = true;
        hasQuadControl = false;
        break;
      }

      case 'Q':
      case 'q': {
        const auto x1 = take();
        const auto y1 = x1 ? take() : std::nullopt;
        const auto x = y1 ? take() : std::nullopt;
        const auto y = x ? take() : std::nullopt;
        if (!y) return std::nullopt;

        const Point control = point(*x1, *y1);
        current = point(*x, *y);
        path.quadTo(control, current);
        lastQuadControl = control;
        hasQuadControl = true;
        hasCubicControl = false;
        break;
      }

      case 'T':
      case 't': {
        const auto x = take();
        const auto y = x ? take() : std::nullopt;
        if (!y) return std::nullopt;

        const Point control = hasQuadControl ? Point {2 * current.x - lastQuadControl.x, 2 * current.y - lastQuadControl.y}
                                             : current;
        current = point(*x, *y);
        path.quadTo(control, current);
        lastQuadControl = control;
        hasQuadControl = true;
        hasCubicControl = false;
        break;
      }

      case 'A':
      case 'a': {
        const auto rx = take();
        const auto ry = rx ? take() : std::nullopt;
        const auto rotation = ry ? take() : std::nullopt;
        const auto largeArc = rotation ? takeFlag(scanner) : std::nullopt;
        const auto sweep = largeArc ? takeFlag(scanner) : std::nullopt;
        const auto x = sweep ? take() : std::nullopt;
        const auto y = x ? take() : std::nullopt;
        if (!y) return std::nullopt;

        const Point from = current;
        current = point(*x, *y);
        appendArc(path, from, *rx, *ry, *rotation, *largeArc, *sweep, current);
        hasCubicControl = false;
        hasQuadControl = false;
        break;
      }

      case 'Z':
      case 'z': {
        path.close();
        current = subpathStart;
        hasCubicControl = false;
        hasQuadControl = false;
        break;
      }

      default:
        return std::nullopt;
    }

    previousCommand = command;
    (void)previousCommand;
  }

  return path;
}

} // namespace graphics::svg
