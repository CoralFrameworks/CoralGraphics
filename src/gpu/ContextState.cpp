/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and limitations under
 * the License.
 */

#include "ContextState.h"

#include <backend/Device.h>

namespace graphics::gpu {

ContextState::~ContextState() {
  if (device == nullptr) {
    return;
  }

  // The wait happens before the backend releases anything, and exactly once. It lives here rather than
  // in ~Context because the last resource to let go is the one that gets here, and that is the moment
  // the GPU genuinely has nothing left to reference.
  device->waitIdle();
  delete device;
  device = nullptr;
}

} // namespace graphics::gpu
