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

#include <effect/EffectTypes.h>

namespace coral {

const char* toString(const EffectResult result) noexcept {
  switch (result) {
    case EffectResult::Success:
      return "Success";
    case EffectResult::InvalidArgument:
      return "InvalidArgument";
    case EffectResult::NotFound:
      return "NotFound";
    case EffectResult::NotAttached:
      return "NotAttached";
    case EffectResult::NotRunning:
      return "NotRunning";
    case EffectResult::Busy:
      return "Busy";
    case EffectResult::Unsupported:
      return "Unsupported";
    case EffectResult::FramePoolExhausted:
      return "FramePoolExhausted";
    case EffectResult::DeviceLost:
      return "DeviceLost";
  }
  return "Unknown";
}

int64_t RenderTime::toTicks(const uint32_t timeBase) const noexcept {
  if (timescale == 0 || timeBase == 0) return 0;
  const int64_t scaled = value * static_cast<int64_t>(timeBase);
  // Round to nearest rather than truncating: 29.97 fps worth of ticks expressed at 60 Hz would otherwise
  // always lose the same fraction, and a texture sequence driven by these values would drift early.
  if (scaled >= 0) return (scaled + static_cast<int64_t>(timescale) / 2) / static_cast<int64_t>(timescale);
  return -((-scaled + static_cast<int64_t>(timescale) / 2) / static_cast<int64_t>(timescale));
}

EffectValue EffectValue::fromFloat(const float value) noexcept {
  EffectValue result;
  result.mType = EffectValueType::Float;
  result.mCount = 1;
  result.mValues[0] = value;
  return result;
}

EffectValue EffectValue::fromInt(const int32_t value) noexcept {
  EffectValue result;
  result.mType = EffectValueType::Int;
  result.mCount = 1;
  // Stored as the number rather than the bit pattern: a shader reads it with `int(x + 0.5)`, which is
  // exact for every integer a float can represent and needs no `floatBitsToInt` from the author.
  result.mValues[0] = static_cast<float>(value);
  return result;
}

EffectValue EffectValue::fromVec2(const EffectVec2 value) noexcept {
  EffectValue result;
  result.mType = EffectValueType::Vec2;
  result.mCount = 2;
  result.mValues[0] = value.x;
  result.mValues[1] = value.y;
  return result;
}

EffectValue EffectValue::fromVec3(const EffectVec3 value) noexcept {
  EffectValue result;
  result.mType = EffectValueType::Vec3;
  result.mCount = 3;
  result.mValues[0] = value.x;
  result.mValues[1] = value.y;
  result.mValues[2] = value.z;
  return result;
}

EffectValue EffectValue::fromVec4(const EffectVec4 value) noexcept {
  EffectValue result;
  result.mType = EffectValueType::Vec4;
  result.mCount = 4;
  result.mValues[0] = value.x;
  result.mValues[1] = value.y;
  result.mValues[2] = value.z;
  result.mValues[3] = value.w;
  return result;
}

EffectValue EffectValue::fromColor(const EffectColor value) noexcept {
  EffectValue result;
  result.mType = EffectValueType::Color;
  result.mCount = 4;
  result.mValues[0] = value.r;
  result.mValues[1] = value.g;
  result.mValues[2] = value.b;
  result.mValues[3] = value.a;
  return result;
}

EffectValue EffectValue::fromMatrix4(const EffectMatrix4& value) noexcept {
  EffectValue result;
  result.mType = EffectValueType::Matrix4;
  result.mCount = 16;
  for (size_t index = 0; index < 16; ++index) result.mValues[index] = value.m[index];
  return result;
}

} // namespace coral
