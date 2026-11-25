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

#include "EffectInternal.h"

#include <effect/EffectFrame.h>

namespace coral {

EffectFrame::EffectFrame() noexcept = default;
EffectFrame::~EffectFrame() = default;

bool EffectFrame::isValid() const noexcept {
  return mInfo != nullptr;
}

uint64_t EffectFrame::id() const noexcept {
  return mInfo != nullptr ? mInfo->id : 0;
}

uint32_t EffectFrame::width() const noexcept {
  return mInfo != nullptr ? mInfo->width : 0;
}

uint32_t EffectFrame::height() const noexcept {
  return mInfo != nullptr ? mInfo->height : 0;
}

EffectPixelFormat EffectFrame::format() const noexcept {
  return mInfo != nullptr ? mInfo->format : EffectPixelFormat::RGBA8;
}

EffectColorSpace EffectFrame::colorSpace() const noexcept {
  return mInfo != nullptr ? mInfo->colorSpace : EffectColorSpace::SRGB;
}

RenderTime EffectFrame::timestamp() const noexcept {
  return mInfo != nullptr ? mInfo->timestamp : RenderTime{};
}

} // namespace coral
