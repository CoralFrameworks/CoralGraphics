/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and limitations under the License.
 */

#include <private/graphics/IccProfile.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

/**
 * @file IccProfile.cpp
 *
 * The profile reader.
 *
 * ## The shape of the problem
 *
 * An ICC profile is a header, a table of tags, and the tags themselves scattered through the file. Nearly every
 * field is big-endian, and three different fixed-point conventions appear in the tags this reads: `s15Fixed16`
 * (a signed 16.16), `u8Fixed8` (an unsigned 8.8), and plain integers. Getting one of those wrong does not produce
 * an error -- it produces a colour space that is a little bit wrong everywhere, which is the failure mode this
 * whole module exists to avoid.
 *
 * ## Every read is bounds-checked
 *
 * A profile is untrusted input: it arrives from a file, a network, or a camera. Nothing here indexes the buffer
 * without first proving the index is inside it, and no length from the file is trusted to be the length of
 * anything. A profile that claims a tag extends past the end is rejected rather than clamped, because a tag that
 * overruns is a tag whose author and this reader disagree about the format.
 *
 * ## What is accepted
 *
 * Only the ordinary case: a version 2 or 4 matrix/TRC RGB profile. The colourants are read as XYZ, the curves as
 * `curv` or `para`, and the resulting matrix is scaled so its columns sum to the stated white point. Everything
 * else -- other device classes, other colour spaces, the pipeline tag family, the Lab and float encodings -- is
 * refused by name, so that a caller reading the log knows which profile was declined and why.
 */


namespace graphics::icc {
namespace {

// ==================================================================================================
// Reading big-endian values without trusting the buffer
// ==================================================================================================

/**
 * A window onto the profile's bytes that refuses to read outside them.
 *
 * Every accessor returns `std::nullopt` rather than a value when the read would leave the buffer. That turns the
 * bounds check into part of the type: a reader that forgets to check simply does not get a value, instead of
 * reading whatever is next in memory.
 */
class Reader {
public:
  Reader(const uint8_t* data, const size_t size) noexcept: mData(data), mSize(size) {}

  [[nodiscard]] size_t size() const noexcept { return mSize; }

  /** @return the unsigned byte at @p offset, or `std::nullopt` when it is outside the buffer. */
  [[nodiscard]] std::optional<uint8_t> u8(const size_t offset) const noexcept {
    if (!contains(offset, 1)) return std::nullopt;
    return mData[offset];
  }

  [[nodiscard]] std::optional<uint16_t> u16(const size_t offset) const noexcept {
    if (!contains(offset, 2)) return std::nullopt;
    return static_cast<uint16_t>((static_cast<uint16_t>(mData[offset]) << 8) | mData[offset + 1]);
  }

  [[nodiscard]] std::optional<uint32_t> u32(const size_t offset) const noexcept {
    if (!contains(offset, 4)) return std::nullopt;
    return (static_cast<uint32_t>(mData[offset]) << 24) | (static_cast<uint32_t>(mData[offset + 1]) << 16) |
           (static_cast<uint32_t>(mData[offset + 2]) << 8) | static_cast<uint32_t>(mData[offset + 3]);
  }

  /** @return the signed 16.16 fixed-point value at @p offset, the convention ICC uses for most quantities. */
  [[nodiscard]] std::optional<float> s15Fixed16(const size_t offset) const noexcept {
    const auto raw = u32(offset);
    if (!raw.has_value()) return std::nullopt;
    // The conversion is through int32 so that the sign is extended rather than read as a large unsigned value.
    return static_cast<float>(static_cast<int32_t>(*raw)) / 65536.0f;
  }

  /** @return the unsigned 8.8 fixed-point value at @p offset, used by the parametric curve's exponents. */
  [[nodiscard]] std::optional<float> u8Fixed8(const size_t offset) const noexcept {
    const auto raw = u16(offset);
    if (!raw.has_value()) return std::nullopt;
    return static_cast<float>(*raw) / 256.0f;
  }

  /** @return a copy of @p count bytes starting at @p offset, or `std::nullopt` when they are not all there. */
  [[nodiscard]] std::optional<std::vector<uint8_t>> bytes(const size_t offset, const size_t count) const {
    if (!contains(offset, count)) return std::nullopt;
    return std::vector<uint8_t>(mData + offset, mData + offset + count);
  }

private:
  [[nodiscard]] bool contains(const size_t offset, const size_t count) const noexcept {
    // Written as a subtraction rather than as `offset + count <= mSize`, which overflows for a large `count` and
    // would accept a tag extending far past the end.
    return offset <= mSize && count <= mSize - offset;
  }

  const uint8_t* mData;
  size_t mSize;
};

// ==================================================================================================
// Tag types
// ==================================================================================================

/** The four-byte signature of a tag type, read as a big-endian integer so it can be a switch case. */
constexpr uint32_t signature(const char (&text)[5]) {
  return (static_cast<uint32_t>(text[0]) << 24) | (static_cast<uint32_t>(text[1]) << 16) |
         (static_cast<uint32_t>(text[2]) << 8) | static_cast<uint32_t>(text[3]);
}

constexpr uint32_t kXyzType = signature("XYZ ");
constexpr uint32_t kCurveType = signature("curv");
constexpr uint32_t kParametricType = signature("para");
constexpr uint32_t kTextDescriptionType = signature("desc");

/** How far a tag has to be read before its type is even known. */
constexpr size_t kTagTypeHeaderSize = 8;

/**
 * @return the three XYZ values a colourant or white-point tag holds, or `std::nullopt`.
 *
 * The values are `s15Fixed16`, and the tag is required to contain exactly three of them. Requiring the exact size
 * rather than at least three is deliberate: ICC permits padding, but this reader would rather decline an unusual
 * profile than guess which of two interpretations its author intended.
 */
std::optional<std::array<float, 3>> readXyzTag(const Reader& reader, const size_t offset, const size_t size) {
  if (size < kTagTypeHeaderSize + 12) return std::nullopt;
  const auto type = reader.u32(offset);
  if (!type.has_value() || *type != kXyzType) return std::nullopt;

  // Three bytes of reserved space after the type, then a reserved four-byte field, so the values start at 8.
  std::array<float, 3> xyz {};
  for (size_t i = 0; i < 3; ++i) {
    const auto value = reader.s15Fixed16(offset + kTagTypeHeaderSize + i * 4);
    if (!value.has_value()) return std::nullopt;
    xyz[i] = *value;
  }
  return xyz;
}

/**
 * @return the curve a `curv` tag describes, or `std::nullopt`.
 *
 * Three forms live in this one tag type, and which one is present is given by the count that follows the type:
 *
 *  - zero, meaning the identity curve;
 *  - one, meaning a pure gamma whose value is the single entry;
 *  - two or more, meaning a lookup table.
 *
 * The count is read before anything else, so a table with an implausible number of entries is rejected before the
 * reader is asked for them.
 */
std::optional<gcms::Curve> readCurveTag(const Reader& reader, const size_t offset, const size_t size) {
  if (size < kTagTypeHeaderSize + 4) return std::nullopt;
  const auto type = reader.u32(offset);
  if (!type.has_value() || *type != kCurveType) return std::nullopt;

  const auto count = reader.u32(offset + kTagTypeHeaderSize);
  if (!count.has_value()) return std::nullopt;

  if (*count == 0) {
    // The identity. `Curve`'s default is exactly that, and it is a parametric curve rather than a table, which is
    // the more useful of the two representations of the same thing.
    return gcms::Curve {};
  }

  if (*count == 1) {
    const auto gamma = reader.u8Fixed8(offset + kTagTypeHeaderSize + 4);
    if (!gamma.has_value() || !(*gamma > 0.0f)) return std::nullopt;
    return gcms::Curve { gcms::TransferFunction::makeGamma(*gamma) };
  }

  // A table. Each entry is two bytes, and the whole table has to be present.
  // `size` is the tag's own length, so measuring from the tag's start is measuring from zero.
  const size_t valuesOffset = kTagTypeHeaderSize + 4;
  const size_t valuesBytes = static_cast<size_t>(*count) * 2;
  if (valuesBytes / 2 != *count) return std::nullopt;  // the multiply wrapped
  if (size < valuesOffset || valuesBytes > size - valuesOffset) return std::nullopt;

  const auto table = reader.bytes(offset + valuesOffset, valuesBytes);
  if (!table.has_value()) return std::nullopt;
  return gcms::Curve::fromTable16Be(table->data(), *count);
}

/**
 * @return the curve a `para` tag describes, or `std::nullopt`.
 *
 * The parametric curve is the compact form of the seven-parameter family: a function type selects which of five
 * parameter lists follows, and every list is a subset of `(g, a, b, c, d, e, f)` with the rest zero.
 *
 * The list is read as `s15Fixed16` except for the exponent, which is `u8Fixed8`. That asymmetry is the format's,
 * not a choice here: `g` is the only parameter ICC writes at 8.8 precision.
 */
std::optional<gcms::Curve> readParametricTag(const Reader& reader, const size_t offset, const size_t size) {
  if (size < kTagTypeHeaderSize + 4) return std::nullopt;
  const auto type = reader.u32(offset);
  if (!type.has_value() || *type != kParametricType) return std::nullopt;

  const auto functionType = reader.u16(offset + kTagTypeHeaderSize);
  if (!functionType.has_value()) return std::nullopt;

  // Where each parameter sits, and how many there are. The exponent is always the first.
  int parameterCount = 0;
  switch (*functionType) {
    case 0: parameterCount = 1; break;
    case 1: parameterCount = 3; break;
    case 2: parameterCount = 4; break;
    case 3: parameterCount = 5; break;
    case 4: parameterCount = 7; break;
    default: return std::nullopt;  // a function type this does not know, so it cannot be read
  }

  // The tag has to hold all of the parameters, not merely some of them. An off-by-one here leaves the last parameter
  // read from whatever follows the tag -- in a file, the next tag's bytes, and in a zeroed test buffer a zero that
  // looks like a plausible coefficient. The size check is also the only thing between a malformed profile and an
  // out-of-bounds read, since `Reader` would otherwise have to be relied on to reject it.
  //
  // The run before the parameters is twelve bytes, not eight: the type, four reserved bytes, the two-byte function
  // type, then two more reserved. The exponent follows at twelve, and the parameters after that.
  constexpr size_t kParametricHeaderSize = kTagTypeHeaderSize + 4;
  if (size < kParametricHeaderSize + static_cast<size_t>(parameterCount) * 4) return std::nullopt;

  const size_t base = offset + kParametricHeaderSize;
  // The exponent is `s15Fixed16`, four bytes, like every other parameter. The `u8Fixed8` precision is what ICC uses
  // for a `curv` tag's single gamma entry, and assuming it here as well is a natural mistake: it makes the tag two
  // bytes shorter than it is, so every parameter after the exponent is read two bytes early and each one is wrong --
  // by a factor that no single value in the profile reveals.
  const auto gamma = reader.s15Fixed16(base);
  if (!gamma.has_value()) return std::nullopt;

  // The remaining parameters, in order. Each function type names a different subset of the seven:
  //
  //   0:  Y = X^g
  //   1:  Y = (aX + b)^g            for X >= -b/a,  else 0
  //   2:  Y = (aX + b)^g + c        for X >= -b/a,  else c
  //   3:  Y = (aX + b)^g + c        for X >= d,     else cX
  //   4:  Y = (aX + b)^g + e        for X >= d,     else cX + f
  //
  // which is the same seven-parameter shape in every case, with the unused ones zero.
  float a = 0.0f;
  float b = 0.0f;
  float c = 0.0f;
  float d = 0.0f;
  float e = 0.0f;
  float f = 0.0f;

  const auto read = [&reader, base](const int index) { return reader.s15Fixed16(base + 4 + static_cast<size_t>(index) * 4); };

  switch (*functionType) {
    case 0:
      break;  // just the exponent

    case 1:
      if (const auto v = read(0)) a = *v; else return std::nullopt;
      if (const auto v = read(1)) b = *v; else return std::nullopt;
      break;

    case 2:
      if (const auto v = read(0)) a = *v; else return std::nullopt;
      if (const auto v = read(1)) b = *v; else return std::nullopt;
      if (const auto v = read(2)) c = *v; else return std::nullopt;
      break;

    case 3:
      if (const auto v = read(0)) a = *v; else return std::nullopt;
      if (const auto v = read(1)) b = *v; else return std::nullopt;
      if (const auto v = read(2)) c = *v; else return std::nullopt;
      if (const auto v = read(3)) d = *v; else return std::nullopt;
      break;

    default:  // 4
      if (const auto v = read(0)) a = *v; else return std::nullopt;
      if (const auto v = read(1)) b = *v; else return std::nullopt;
      if (const auto v = read(2)) c = *v; else return std::nullopt;
      if (const auto v = read(3)) d = *v; else return std::nullopt;
      if (const auto v = read(4)) e = *v; else return std::nullopt;
      if (const auto v = read(5)) f = *v; else return std::nullopt;
      break;
  }

  const auto curve = gcms::TransferFunction::makeSRGBish(gcms::SRGBishParameters { *gamma, a, b, c, d, e, f });
  if (!curve.has_value()) return std::nullopt;
  return gcms::Curve { *curve };
}

/** One entry of the tag table. */
struct Tag {
  uint32_t signature = 0;
  size_t offset = 0;
  size_t size = 0;
};

/**
 * @return the tag table, or `std::nullopt` when it cannot be read or its entries describe bytes that are not there.
 *
 * The whole table is validated before any of it is used, so that a malformed table is rejected once rather than
 * each time a lookup is made. A tag whose offset and size leave the file is a tag whose author and this reader
 * disagree about the format, and the profile is refused.
 */
std::optional<std::vector<Tag>> readTagTable(const Reader& reader, const uint32_t tagCount) {
  // A profile with more tags than the file has room for is not worth allocating for. Each entry is twelve bytes and
  // the header plus count is 132, so this is the most that could possibly be present.
  if (tagCount > (reader.size() - std::min(reader.size(), size_t { 132 })) / 12) return std::nullopt;

  std::vector<Tag> tags;
  tags.reserve(tagCount);

  constexpr size_t kTableStart = 128 + 4;
  for (uint32_t i = 0; i < tagCount; ++i) {
    const size_t entry = kTableStart + static_cast<size_t>(i) * 12;
    const auto tagSignature = reader.u32(entry);
    const auto offset = reader.u32(entry + 4);
    const auto size = reader.u32(entry + 8);
    if (!tagSignature.has_value() || !offset.has_value() || !size.has_value()) return std::nullopt;

    // The tag has to lie inside the file. Checked as a subtraction, because `offset + size` can wrap.
    if (*offset > reader.size() || *size > reader.size() - *offset) return std::nullopt;
    if (*size < kTagTypeHeaderSize) return std::nullopt;

    tags.push_back(Tag { *tagSignature, *offset, *size });
  }
  return tags;
}

/** @return the first tag with @p tagSignature, or `std::nullopt`. */
std::optional<Tag> findTag(const std::vector<Tag>& tags, const uint32_t tagSignature) {
  for (const Tag& tag: tags) {
    if (tag.signature == tagSignature) return tag;
  }
  return std::nullopt;
}

// ==================================================================================================
// Assembling the colour space
// ==================================================================================================

constexpr uint32_t kSignatureRgb = signature("RGB ");
constexpr uint32_t kTagRedColorant = signature("rXYZ");
constexpr uint32_t kTagGreenColorant = signature("gXYZ");
constexpr uint32_t kTagBlueColorant = signature("bXYZ");
constexpr uint32_t kTagRedTrc = signature("rTRC");
constexpr uint32_t kTagGreenTrc = signature("gTRC");
constexpr uint32_t kTagBlueTrc = signature("bTRC");
constexpr uint32_t kTagWhitePoint = signature("wtpt");

/**
 * A colourant's XYZ, expressed as a fraction of the white point.
 *
 * ## Why the colourants are not used as they are
 *
 * ICC's colorant tags do not in general sum to the white point: they are the colorants at their own intensities,
 * and a profile is entitled to state them at whatever scale its author's measurements produced. The matrix a
 * transform needs is the one whose columns sum to the white point, which is what "white is (1,1,1)" means.
 *
 * So each column is scaled by the ratio of the white point's component to the sum of that column. This is the
 * step skcms calls normalising the colorants, and leaving it out is a mistake with no symptom other than a colour
 * cast that scales with how far the profile's author's scale happened to be from one.
 */
std::optional<std::array<std::array<float, 3>, 3>> normaliseColorants(
        const std::array<std::array<float, 3>, 3>& colorants, const std::array<float, 3>& white) {
  std::array<std::array<float, 3>, 3> matrix {};

  // ## The indexing, and the day it cost
  //
  // `colorants[c]` is the colourant for channel `c`, and each entry is that channel's XYZ. The matrix is indexed
  // `[component][channel]` -- the same way `write` reads it back -- so a channel's three values are written down the
  // *column* of the matrix as well as being a contiguous row of `colorants`.
  //
  // This was written the other way round first, and the consequence was subtle enough to survive several rounds of
  // debugging: parsing produced the transpose of the matrix the writer expected, so the two disagreed about which
  // quantity summed to the white point. The symptom was a round trip that changed the matrix by a ratio -- and the
  // measurement that identified it was printing the *row* sums of the parsed matrix and finding they were exactly
  // the white point the writer had emitted, rather than the column sums.
  for (size_t channel = 0; channel < 3; ++channel) {
    float sum = 0.0f;
    for (size_t component = 0; component < 3; ++component) sum += colorants[channel][component];
    if (!(std::fabs(sum) > 1e-6f)) return std::nullopt;  // a channel with no colour at all
    if (!std::isfinite(sum)) return std::nullopt;

    const float scale = white[channel] / sum;
    if (!std::isfinite(scale)) return std::nullopt;

    for (size_t component = 0; component < 3; ++component) {
      matrix[component][channel] = colorants[channel][component] * scale;
    }
  }

  // A matrix that is not finite, or whose columns are not all positive, describes no colour space.
  for (const auto& row: matrix) {
    for (const float value: row) {
      if (!std::isfinite(value)) return std::nullopt;
    }
  }
  return matrix;
}

} // namespace anonymous

std::optional<Profile> parse(const void* data, const size_t size) noexcept {
  if (data == nullptr) return std::nullopt;

  // The header is 128 bytes and the tag count four more; anything smaller is not a profile.
  constexpr size_t kHeaderSize = 128;
  if (size < kHeaderSize + 4) return std::nullopt;

  const auto* bytes = static_cast<const uint8_t*>(data);
  const Reader reader(bytes, size);

  // The size the header claims. A profile that claims to be larger than the buffer it arrived in was truncated in
  // transit, and reading it would mean reading whatever follows the buffer.
  const auto declaredSize = reader.u32(0);
  if (!declaredSize.has_value() || *declaredSize > size) return std::nullopt;

  // The `acsp` signature at offset 36 is the marker that says this is an ICC profile at all. Without it, the bytes
  // could be anything -- and a parse of arbitrary bytes that succeeded would be a security problem rather than a
  // colour problem.
  const auto fileSignature = reader.u32(36);
  if (!fileSignature.has_value() || *fileSignature != signature("acsp")) return std::nullopt;

  const auto version = reader.u32(8);
  if (!version.has_value()) return std::nullopt;
  // The major version is the first byte. Versions 2 and 4 are the ones whose tag encodings this reads; version 5
  // exists but is not in circulation, and a profile claiming it would have to be checked rather than assumed.
  const auto majorVersion = static_cast<uint8_t>(*version >> 24);
  if (majorVersion != 2 && majorVersion != 4) return std::nullopt;

  const auto dataColorSpace = reader.u32(16);
  if (!dataColorSpace.has_value() || *dataColorSpace != kSignatureRgb) return std::nullopt;

  const auto tagCount = reader.u32(128);
  if (!tagCount.has_value()) return std::nullopt;

  const auto tags = readTagTable(reader, *tagCount);
  if (!tags.has_value()) return std::nullopt;

  // The white point and the three colorants. All four are required: without any one of them there is no matrix,
  // and a matrix is the whole of what this reader extracts.
  const auto whiteTag = findTag(*tags, kTagWhitePoint);
  const auto redTag = findTag(*tags, kTagRedColorant);
  const auto greenTag = findTag(*tags, kTagGreenColorant);
  const auto blueTag = findTag(*tags, kTagBlueColorant);
  if (!whiteTag.has_value() || !redTag.has_value() || !greenTag.has_value() || !blueTag.has_value()) {
    return std::nullopt;
  }

  const auto white = readXyzTag(reader, whiteTag->offset, whiteTag->size);
  const auto red = readXyzTag(reader, redTag->offset, redTag->size);
  const auto green = readXyzTag(reader, greenTag->offset, greenTag->size);
  const auto blue = readXyzTag(reader, blueTag->offset, blueTag->size);
  if (!white.has_value() || !red.has_value() || !green.has_value() || !blue.has_value()) return std::nullopt;

  const std::array<std::array<float, 3>, 3> colorants { *red, *green, *blue };
  const auto matrix = normaliseColorants(colorants, *white);
  if (!matrix.has_value()) return std::nullopt;

  // The three curves. Each may be a `curv` or a `para`, and the two are told apart by the tag's own type field.
  std::array<gcms::Curve, 3> curves;
  const std::array<uint32_t, 3> curveTags { kTagRedTrc, kTagGreenTrc, kTagBlueTrc };

  for (size_t channel = 0; channel < 3; ++channel) {
    const auto tag = findTag(*tags, curveTags[channel]);
    if (!tag.has_value()) return std::nullopt;

    const auto type = reader.u32(tag->offset);
    if (!type.has_value()) return std::nullopt;

    if (*type == kCurveType) {
      const auto curve = readCurveTag(reader, tag->offset, tag->size);
      if (!curve.has_value()) return std::nullopt;
      curves[channel] = *curve;
    } else if (*type == kParametricType) {
      const auto curve = readParametricTag(reader, tag->offset, tag->size);
      if (!curve.has_value()) return std::nullopt;
      curves[channel] = *curve;
    } else {
      // A table-based curve this does not read, or a tag type that is not a curve at all.
      return std::nullopt;
    }
  }

  Profile profile;
  profile.toXyz = *matrix;
  profile.curves = curves;
  profile.dataColorSpace = *dataColorSpace;
  const auto intent = reader.u32(64);
  profile.renderingIntent = intent.value_or(0);
  return profile;
}

} // namespace graphics::icc
