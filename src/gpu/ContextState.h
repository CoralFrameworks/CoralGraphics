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

#ifndef CORAL_GPU_CONTEXT_STATE_H
#define CORAL_GPU_CONTEXT_STATE_H

#include <memory>

namespace backend {
class Device;
} // namespace backend

namespace graphics::gpu {

/**
 * The device a context owns, shared with every resource that context hands out.
 *
 * A resource holds one of these rather than a `Context` so that dropping the last `Context` handle
 * while a buffer is still alive keeps the device running, and so that a resource header does not have
 * to include the context header. It is a separate type from `Context` for the same reason: the two
 * have genuinely different lifetimes.
 *
 * This header is internal to `libgpu`. Clients never name `ContextState`; they only ever hold the
 * resources that do.
 */
struct ContextState {
  backend::Device* device = nullptr;

  ~ContextState();

  // Non-copyable, and this is load-bearing rather than tidiness.
  //
  // The destructor deletes `device`, so the implicitly generated copy constructor would duplicate the *pointer* without
  // duplicating the device, and the second `ContextState` to be destroyed would release it again. Nothing copies one
  // today -- every use is a `shared_ptr` built by `make_shared` -- which is exactly why the default had to go: the type
  // was copyable and the compiler had no reason to say anything, so the first copy anybody wrote would have been a
  // double free rather than a compile error.
  //
  // Moves are deleted with the copies. Sharing is how this is meant to be held, and a movable owner invites a
  // `shared_ptr` to be built from a moved-from object.
  ContextState(const ContextState&) = delete;
  ContextState& operator=(const ContextState&) = delete;
  ContextState(ContextState&&) = delete;
  ContextState& operator=(ContextState&&) = delete;

  // The compiler would not generate a default constructor once any other constructor is declared; `make_shared` needs
  // one, so it is asked for explicitly.
  ContextState() = default;
};

} // namespace graphics::gpu

#endif // CORAL_GPU_CONTEXT_STATE_H
