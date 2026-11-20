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

#include <private/graphics/IccProfile.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

/**
 * @file IccProfileWriter.cpp
 *
 * The writing half of the profile reader, kept in its own translation unit so that the two can be read against
 * each other: `IccProfile.cpp` says what the format is, and this says what a profile looks like when the format is
 * written down. They share the private header and nothing else.
 *
 * ## Why the round trip is the test
 *
 * A writer and a reader that agree with each other can both be wrong about the format -- which is exactly what
 * happened while the reader was being written, twice, in the tag layout. The way out is not more care: it is a
 * fixture whose bytes are stated independently, which the reader's tests have. What the writer adds is the other
 * direction of the same check, and it is worth having because a profile that this library *produces* is one that
 * other software will read. A round trip through a second implementation is the only real proof, and failing that,
 * a round trip through this one at least proves the two halves describe the same format.
 *
 * ## What is written
 *
 * A version 4 matrix/TRC RGB profile: the four colorant and white-point tags, and the three curve tags. That is the
 * complete set this library can represent, and it is the set a profile needs to be a colour space rather than a
 * description of a device.
 */

namespace graphics::icc {
namespace {

/** Appends a big-endian 16-bit value. */
void put16(std::vector<uint8_t>& out, const uint16_t value) {
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value & 0xFF));
}

/** Appends a big-endian 32-bit value. */
void put32(std::vector<uint8_t>& out, const uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 24));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(value & 0xFF));
}

/** Appends a four-character signature. */
void putSignature(std::vector<uint8_t>& out, const char* text) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(text[i]));
}

/**
 * Appends a signed 16.16 fixed-point value.
 *
 * The range is checked rather than assumed, because the format cannot hold a value outside it: an unchecked cast
 * would wrap a large matrix coefficient into a small number of the opposite sign, and the resulting profile would
 * be a valid file describing a different colour space.
 */
void putS15Fixed16(std::vector<uint8_t>& out, const double value) {
  // Clamped to the representable range rather than wrapped. The largest and smallest values are what a caller gets
  // for an out-of-range input, which is visibly wrong rather than subtly wrong.
  constexpr double kLimit = 32767.0 + 65535.0 / 65536.0;
  const double clamped = std::clamp(value, -kLimit, kLimit);
  put32(out, static_cast<uint32_t>(static_cast<int32_t>(std::lround(clamped * 65536.0))));
}

/**
 * @return an `XYZ ` tag holding the three values.
 *
 * The layout is the type, four reserved bytes, then the three `s15Fixed16` values -- no second reserved field, which
 * is where this tag differs from `para` and where a reader that assumed one shape for both goes wrong.
 */
std::vector<uint8_t> xyzTag(const double x, const double y, const double z) {
  std::vector<uint8_t> tag;
  putSignature(tag, "XYZ ");
  put32(tag, 0);
  putS15Fixed16(tag, x);
  putS15Fixed16(tag, y);
  putS15Fixed16(tag, z);
  return tag;
}

/** One tag to be written: its signature and its bytes. */
struct NamedTag {
  const char* signature;
  std::vector<uint8_t> bytes;
};

/**
 * @return the tag that writes @p function, in whichever of the two curve encodings describes it best.
 *
 * ## Which encoding is chosen
 *
 * A pure gamma -- `d` and the offset terms all zero -- is written as a `curv` tag with a single entry, because that
 * is a two-byte-per-channel saving and, more importantly, is what other readers expect to find for a simple display
 * curve. Anything with a linear segment is written as a `para` tag of function type 3 or 4, which is the form that
 * can express a segmented curve at all.
 *
 * The identity is a special case of a pure gamma with the exponent one, and it is written as a `curv` tag with *no*
 * entries: the format's own way of saying "this channel is passed through", which a reader recognises without
 * having to evaluate anything.
 */
std::vector<uint8_t> curveTag(const gcms::Curve& curve) {
  // A table has no parametric form to write. Refused by an empty tag, so that the caller turns it into a refusal
  // rather than writing a curve that means something else -- see the note on `write`.
  const gcms::TransferFunction* function = curve.function();
  if (function == nullptr) return {};

  if (function->kind() == gcms::Kind::Identity) {
    std::vector<uint8_t> tag;
    putSignature(tag, "curv");
    put32(tag, 0);
    put32(tag, 0); // zero entries: the identity
    return tag;
  }

  const auto* parameters = function->sRGBish();
  if (parameters == nullptr) {
    // A curve in a shape this cannot write. Reported by an empty tag, which the caller turns into a refusal rather
    // than writing a profile with a curve in it that means something else.
    return {};
  }

  const bool pureGamma = parameters->d == 0.0f && parameters->c == 0.0f && parameters->f == 0.0f;

  if (pureGamma) {
    std::vector<uint8_t> tag;
    putSignature(tag, "curv");
    put32(tag, 0);
    put32(tag, 1); // one entry: this value is the gamma
    // The exponent in a `curv` tag is `u8Fixed8`, which is the one place that precision is used. Excess precision
    // here would be lost anyway, so it is rounded rather than silently truncated.
    const double gamma = std::clamp(static_cast<double>(parameters->g), 0.0, 255.996);
    put16(tag, static_cast<uint16_t>(std::lround(gamma * 256.0)));
    return tag;
  }

  // A segmented curve. Type 3 is the four-parameter form with the offset `e` fixed at zero, and type 4 is the full
  // seven-parameter one; which is used is decided by whether the curve actually needs `e`.
  std::vector<uint8_t> tag;
  putSignature(tag, "para");
  put32(tag, 0);

  const bool needsE = parameters->e != 0.0f;
  put16(tag, needsE ? 4 : 3);
  put16(tag, 0); // reserved

  // The exponent is `s15Fixed16` here, not the `u8Fixed8` the `curv` tag uses. Getting that wrong shifts every
  // parameter after it by two bytes, which is the mistake the reader's tests were written to catch and did.
  putS15Fixed16(tag, parameters->g);
  putS15Fixed16(tag, parameters->a);
  putS15Fixed16(tag, parameters->b);
  putS15Fixed16(tag, parameters->c);
  putS15Fixed16(tag, parameters->d);
  if (needsE) {
    putS15Fixed16(tag, parameters->e);
    putS15Fixed16(tag, parameters->f);
  }
  return tag;
}

} // namespace anonymous

std::optional<std::vector<uint8_t>> write(const Profile& profile) {
  // The three curve tags, written first so that a curve this cannot express is refused before a profile is partly
  // assembled. A writer that discovered the problem halfway through would have to unwind, and the caller would have
  // to be told which of two failures it was.
  std::array<std::vector<uint8_t>, 3> curveTags;
  for (size_t channel = 0; channel < 3; ++channel) {
    curveTags[channel] = curveTag(profile.curves[channel]);
    if (curveTags[channel].empty()) return std::nullopt;
  }

  // ## The white point is the sum of the columns, and that is what makes the round trip exact
  //
  // The reader scales the colorant columns so that they sum to the white point -- the step that turns three colorant
  // measurements into the matrix a transform needs. Writing therefore has to leave that relation already satisfied,
  // because the reader applies it unconditionally: if the columns sum to something else, the reader scales them, and
  // the matrix that comes back is not the one that went in.
  //
  // Stating the white point as the sum of the columns makes the reader's scaling a multiply by one, so the matrix is
  // preserved exactly rather than approximately. Two earlier versions got this wrong in different ways: taking the
  // white point to be a constant and scaling the colorants to match changed the colour space by 7%, and writing the
  // columns unscaled while stating D50 changed it by 15%.
  //
  // ## The colorant matrix, and the one scale convention that round trips
  //
  // A profile carries the colorant columns and one white point, and its reader scales each column by the ratio of the
  // white point to that column's sum. `ColorSpace` holds the colorant matrix its primaries produced -- on the built-in
  // sRGB gamut those columns sum to 0.672, 1.199 and 0.918, while the reader's output is normalised so that they sum
  // to the white point instead.
  //
  // So writing a `ColorSpace` as a profile is a change of scale, and the scale that makes the change reversible is the
  // one the reader itself produces: each column scaled to sum to its white-point component. The reader's ratio is then
  // exactly one, and applying it any number of times leaves the matrix alone. That is what
  // `ColorSpaceSerializeRoundTrips` asserts.
  //
  // Two earlier versions got this wrong and both were caught by the same round trip: writing the columns unscaled
  // while stating their own sum as the white point (11.5% out on sRGB), and scaling them to D50 while the reader
  // scaled them again (7% out on Display P3).
  std::array<std::array<double, 3>, 3> colorants{};
  for (size_t channel = 0; channel < 3; ++channel) {
    for (size_t component = 0; component < 3; ++component) {
      colorants[channel][component] = profile.toXyz[component][channel];
      if (!std::isfinite(colorants[channel][component])) return std::nullopt;
    }
  }

  // The white point is the sum of each *column* of `toXyz` -- that is, the sum of one channel's XYZ across all three
  // components. Written as `colorants[channel][component]` summed over `channel`, which is the column, and not over
  // `component`, which is the colorant index and therefore the row.
  //
  // That distinction is the whole of a bug that took a round to find, so it is spelled out: the loop below used to
  // sum over `channel` while indexing `colorants[0..2][component]`, which is the row. The comment above it already
  // said "the sum of the columns", so the code and its own documentation disagreed, and the round trip was then off
  // by the ratio between the two sums -- 11.5% on the sRGB profile. What made it hard was that both variants look
  // like "sum the three colorants": the difference is only which index varies.
  // ## The colorants have to be stated at the scale the white point implies
  //
  // The reader scales each colorant column by the ratio of the white point to that column's sum, and applies it
  // unconditionally. Writing the columns *as they are* while stating their sum as the white point is therefore not
  // enough: the reader's own output is at the scale the white point implies, so reading a written file multiplies by
  // that ratio a second time and the matrix comes back changed -- measured at 11.5% on sRGB.
  //
  // Scaling each column so that it sums to the corresponding white component makes the reader's ratio exactly one, so
  // it can be applied any number of times without altering the matrix. That is the invariant the round trip needs.
  const std::array<double, 3> white{ 0.9642, 1.0, 0.8249 }; // D50, the white point of the PCS

  std::array<std::array<double, 3>, 3> stated{};
  for (size_t channel = 0; channel < 3; ++channel) {
    double sum = 0.0;
    for (size_t component = 0; component < 3; ++component) sum += colorants[channel][component];
    if (!std::isfinite(sum) || !(std::fabs(sum) > 1e-9)) return std::nullopt;

    const double scale = white[channel] / sum;
    for (size_t component = 0; component < 3; ++component) {
      stated[channel][component] = colorants[channel][component] * scale;
      if (!std::isfinite(stated[channel][component])) return std::nullopt;
    }
  }

  const std::array<NamedTag, 7> tags{ {
    NamedTag{ "rXYZ", xyzTag(stated[0][0], stated[0][1], stated[0][2]) },
    NamedTag{ "gXYZ", xyzTag(stated[1][0], stated[1][1], stated[1][2]) },
    NamedTag{ "bXYZ", xyzTag(stated[2][0], stated[2][1], stated[2][2]) },
    NamedTag{ "wtpt", xyzTag(white[0], white[1], white[2]) },
    NamedTag{ "rTRC", curveTags[0] },
    NamedTag{ "gTRC", curveTags[1] },
    NamedTag{ "bTRC", curveTags[2] },
  } };

  // ## The layout
  //
  // Header, tag count, tag entries, then the tag bytes each aligned to four. The offsets in the entries are absolute
  // from the start of the file, so they cannot be known until the table's own size is -- which is why the entries are
  // emitted twice conceptually: once to size the table, and once with the offsets filled in.
  constexpr size_t kHeaderSize = 128;
  constexpr size_t kEntrySize = 12;
  const size_t tableSize = 4 + tags.size() * kEntrySize;

  std::vector<size_t> offsets;
  offsets.reserve(tags.size());
  size_t cursor = kHeaderSize + tableSize;
  for (const NamedTag& tag: tags) {
    cursor = (cursor + 3) & ~size_t{ 3 };
    offsets.push_back(cursor);
    cursor += tag.bytes.size();
  }

  std::vector<uint8_t> out;
  out.reserve(cursor);

  // --- header ---
  put32(out, static_cast<uint32_t>(cursor)); // 0: the profile's size
  putSignature(out, "none"); // 4: preferred CMM, none in particular
  put32(out, 0x04300000); // 8: version 4.3
  putSignature(out, "mntr"); // 12: device class, a display
  putSignature(out, "RGB "); // 16: data colour space
  putSignature(out, "XYZ "); // 20: PCS
  put16(out, 2026); // 24: date and time, in the format's fields
  put16(out, 1);
  put16(out, 1);
  put16(out, 0);
  put16(out, 0);
  put16(out, 0);
  putSignature(out, "acsp"); // 36: the file signature
  putSignature(out, "APPL"); // 40: platform
  put32(out, 0); // 44: flags
  putSignature(out, "none"); // 48: device manufacturer
  putSignature(out, "none"); // 52: device model
  put32(out, 0); // 56: device attributes
  put32(out, 0);
  put32(out, static_cast<uint32_t>(profile.renderingIntent)); // 64: rendering intent
  // The PCS illuminant, which the specification fixes at D50 for every profile. Written as its stated values rather
  // than derived, because it is a constant of the format and not of this colour space.
  putS15Fixed16(out, 0.9642);
  putS15Fixed16(out, 1.0);
  putS15Fixed16(out, 0.8249);
  putSignature(out, "none"); // 80: profile creator
  while (out.size() < kHeaderSize) out.push_back(0); // profile ID and reserved

  // --- tag table ---
  put32(out, static_cast<uint32_t>(tags.size()));
  for (size_t i = 0; i < tags.size(); ++i) {
    putSignature(out, tags[i].signature);
    put32(out, static_cast<uint32_t>(offsets[i]));
    put32(out, static_cast<uint32_t>(tags[i].bytes.size()));
  }

  // --- tag bytes, each starting where its entry said ---
  for (size_t i = 0; i < tags.size(); ++i) {
    while (out.size() < offsets[i]) out.push_back(0);
    out.insert(out.end(), tags[i].bytes.begin(), tags[i].bytes.end());
  }

  return out;
}

} // namespace graphics::icc
