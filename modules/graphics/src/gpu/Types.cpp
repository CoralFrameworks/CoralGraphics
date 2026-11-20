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

#include <gpu/Types.h>

#include <backend/Types.h>

namespace graphics::gpu {

const char* toString(const Result result) noexcept {
  switch (result) {
    case Result::Success:
      return "Success";
    case Result::NotReady:
      return "NotReady";
    case Result::Timeout:
      return "Timeout";
    case Result::OutOfDate:
      return "OutOfDate";
    case Result::Error:
      return "Error";
    case Result::InvalidArgument:
      return "InvalidArgument";
    case Result::InvalidHandle:
      return "InvalidHandle";
    case Result::Unsupported:
      return "Unsupported";
    case Result::OutOfMemory:
      return "OutOfMemory";
    case Result::DeviceLost:
      return "DeviceLost";
    case Result::ShaderCompilationFailed:
      return "ShaderCompilationFailed";
    case Result::WrongThread:
      return "WrongThread";
  }
  // The backend's enum is exhaustive, so reaching here means a value was cast in from outside it.
  return "Unknown";
}

} // namespace graphics::gpu