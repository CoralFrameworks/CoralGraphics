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

#ifndef CORAL_EFFECT_TYPES_H
#define CORAL_EFFECT_TYPES_H

#include <cstddef>
#include <cstdint>

namespace coral {

namespace detail {
/** Gives the implementation access to `EffectValue`'s fixed-size storage without publishing it. */
struct EffectValueAccess;
} // namespace detail

/**
 * Severity of a diagnostic the engine reports through `EffectEngineConfig::diagnosticsCallback`.
 */
enum class EffectSeverity : uint8_t {
  Info,
  Warning,
  Error,
};

/**
 * Result of every public operation that can fail.
 *
 * The vocabulary is deliberately small: a caller is expected to branch on it, and a code that only ever
 * means "something went wrong" belongs in `diagnostics()` rather than in the enum. No public API throws
 * across a thread boundary; failures are returned here and explained as text.
 */
enum class EffectResult : uint8_t {
  Success = 0,
  /** A parameter was null, out of range, of the wrong type, or mutually inconsistent. */
  InvalidArgument,
  /** The named object (node, parameter, texture, sequence) does not exist. */
  NotFound,
  /** The node is not attached to this engine, so it has no runtime state. */
  NotAttached,
  /** The engine has not been started, or has been stopped. */
  NotRunning,
  /** A queue or the frame pool has no capacity right now; retry later. */
  Busy,
  /** The request is valid but this build/backend does not implement it (yet). */
  Unsupported,
  /** The frame pool's budget is exhausted and no frame could be recycled. */
  FramePoolExhausted,
  /** The device or a platform surface was lost and must be recreated. */
  DeviceLost,
};

/** @return a stable, human readable name for @p result. Never null. */
[[nodiscard]] const char* toString(EffectResult result) noexcept;

/** Which graphics API the engine drives. The public layer maps it to the RHI's own enum internally. */
enum class EffectBackend : uint8_t {
  Default,
  OpenGL,
  Vulkan,
  Metal,
};

/** How the engine produces frames after `start()`. */
enum class EffectRenderMode : uint8_t {
  /** Render continuously at `frameRate`, or as fast as the output surface allows when it is 0. */
  Continuous,
  /** Render one frame per `execute()`, which is what tests and offline export use. */
  Manual,
};

/**
 * What the engine does when an input or the frame pool cannot keep up.
 *
 * Preview wants the newest frame; an export wants every frame. The engine only chooses the policy; an
 * input node's queue depth is a platform property and stays with the platform node.
 */
enum class EffectBackpressureMode : uint8_t {
  /** Drop the oldest queued frame and never block the application thread. */
  DropOldest,
  /** Wait for capacity, or report `Busy`. Never drop a frame. */
  Block,
};

/**
 * A canonical time value: `value / timescale` seconds, in ticks of `timescale` per second.
 *
 * `effect` owns this type rather than borrowing one from `libs/media`, so the public headers depend on
 * nothing but the standard library. Every timestamp the engine handles - an input frame's presentation
 * time, a texture sequence's start time, `setSequenceTime` - uses the engine's `timeBase`, which makes
 * seeking a matter of integer arithmetic rather than of accumulating floating point seconds.
 */
struct RenderTime {
  int64_t value = 0;
  uint32_t timescale = 60;

  static constexpr uint32_t kDefaultTimeBase = 60;

  /** @return @p ticks at @p timeBase, i.e. the same instant expressed in that base. */
  static RenderTime fromTicks(int64_t ticks, uint32_t timeBase = kDefaultTimeBase) noexcept {
    return RenderTime{ .value = ticks, .timescale = timeBase };
  }

  /**
   * @return this instant expressed in @p timeBase ticks, rounded to nearest.
   *
   * Integer arithmetic on purpose: a converted time that drifted by a fraction of a tick on every frame
   * would eventually select the wrong texture-sequence frame, which is exactly the class of bug the
   * canonical time base exists to prevent.
   */
  [[nodiscard]] int64_t toTicks(uint32_t timeBase) const noexcept;

  [[nodiscard]] bool isValid() const noexcept { return timescale != 0; }
};

/** Where a video frame comes from. */
enum class EffectFrameSource : uint8_t { Planes, HardwareBuffer };

/** The render/output pixel formats the engine understands. */
enum class EffectPixelFormat : uint8_t {
  /** The default colour format. */
  RGBA8,
  /** A platform surface's exchange format. */
  BGRA8,
  /** HDR and long feedback chains; implemented for passes, readback and Android surfaces. */
  RGBA16F,
  /** Single channel (mask, luminance). Reserved, not implemented. */
  R8,
  /** An input frame's YUV layout; the platform input node converts it to RGBA8. */
  YUV420,
};

/** HDR transfer function of a frame. */
enum class EffectHdrTransfer : uint8_t { PQ, HLG };

enum class EffectToneMapMode : uint8_t { Reinhard, Aces };

/**
 * The colour space of a frame or image. sRGB is the default; DisplayP3/BT.2020 are surface data-space
 * hints, not an end-to-end colour-managed pipeline. `Linear` is reserved for a future linear working
 * space and is not used by an automatic conversion today.
 */
enum class EffectColorSpace : uint8_t {
  SRGB,
  /** Linear working space inside a shader only; no end-to-end linear pipeline in v1. */
  Linear,
  /** Wide gamut SDR. */
  DisplayP3,
  /** HDR10: BT.2020 primaries, PQ transfer. */
  BT2020PQ,
  /** HLG: BT.2020 primaries, HLG transfer. */
  BT2020HLG,
};

/** The type of a declared effect parameter. */
enum class EffectValueType : uint8_t {
  Float,
  Int,
  Vec2,
  Vec3,
  Vec4,
  Color,
  Matrix4,
};

/** @return how many 16-byte parameter slots a value of @p type occupies. */
[[nodiscard]] constexpr size_t effectValueSlotCount(const EffectValueType type) noexcept {
  return type == EffectValueType::Matrix4 ? 4 : 1;
}

struct EffectVec2 {
  float x = 0;
  float y = 0;
};

struct EffectVec3 {
  float x = 0;
  float y = 0;
  float z = 0;
};

struct EffectVec4 {
  float x = 0;
  float y = 0;
  float z = 0;
  float w = 0;
};

/** A colour whose components are **not** premultiplied by alpha. */
struct EffectColor {
  float r = 0;
  float g = 0;
  float b = 0;
  float a = 0;
};

/** A 4x4 matrix in row-major order. */
struct EffectMatrix4 {
  float m[16] = {};
};

/**
 * One typed parameter value, carried across the application/render thread boundary.
 *
 * The storage is fixed and inline, so a per-frame parameter update allocates nothing. A `Matrix4`
 * occupies all sixteen components; every other type uses as many as it has, with the rest zero.
 */
class EffectValue {
public:
  EffectValue() noexcept = default;

  static EffectValue fromFloat(float value) noexcept;
  static EffectValue fromInt(int32_t value) noexcept;
  static EffectValue fromVec2(EffectVec2 value) noexcept;
  static EffectValue fromVec3(EffectVec3 value) noexcept;
  static EffectValue fromVec4(EffectVec4 value) noexcept;
  static EffectValue fromColor(EffectColor value) noexcept;
  static EffectValue fromMatrix4(const EffectMatrix4& value) noexcept;

  /** @return the type this value was created with. */
  [[nodiscard]] EffectValueType type() const noexcept { return mType; }

private:
  friend struct detail::EffectValueAccess;

  EffectValueType mType = EffectValueType::Float;
  uint32_t mCount = 0;
  float mValues[16] = {};
};

} // namespace coral

#endif // CORAL_EFFECT_TYPES_H
