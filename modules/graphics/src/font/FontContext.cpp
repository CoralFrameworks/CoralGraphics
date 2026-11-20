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
 * implied. See the License for the specific language governing permissions and limitations under the
 * License.
 */

#include "FontContext.h"

#include <minilog/log.h>

#include <mutex>

#include FT_FREETYPE_H

namespace graphics::font_detail {
namespace {

/**
 * @return the one library handle, created on first use.
 *
 * Held in a function-local static so that its creation is thread-safe without a lock of Coral's own --
 * the C++ runtime guarantees the initialisation happens once, which is what a `std::call_once` would be
 * used for otherwise.
 */
std::shared_ptr<Library>& sharedLibrarySlot() {
  static std::shared_ptr<Library> slot;
  return slot;
}

std::mutex& libraryMutex() {
  static std::mutex mutex;
  return mutex;
}

/** Releases the FreeType library. Bound to the `shared_ptr` that owns it. */
struct LibraryDeleter {
  void operator()(Library* library) const noexcept { delete library; }
};

/**
 * The concrete library holder.
 *
 * `Library`'s constructor is private so that only `acquire` can create one, which is what keeps the
 * handle a singleton without a public constructor inviting a second one.
 */
class LibraryImpl final : public Library {
public:
  static std::shared_ptr<Library> create() {
    // `new` rather than `make_shared` because the constructor is private; the deleter is bound here so
    // that the custom destruction path is part of the type rather than a detail at each call site.
    return std::shared_ptr<Library>(new LibraryImpl(), LibraryDeleter {});
  }

  LibraryImpl() {
    const FT_Error error = FT_Init_FreeType(&mLibrary);
    if (error != 0) {
      mLibrary = nullptr;
      MINILOG_WARN("[graphics/font] FreeType could not be initialised (error {}); no font can be loaded",
              static_cast<int>(error));
    }
  }

  ~LibraryImpl() override {
    if (handle() != nullptr) FT_Done_FreeType(handle());
  }
};

} // namespace anonymous

std::shared_ptr<Library> Library::acquire() {
  std::lock_guard<std::mutex> lock(libraryMutex());
  auto& slot = sharedLibrarySlot();
  if (!slot) {
    slot = LibraryImpl::create();
    // A failed initialisation is not cached: `slot` stays null and the next caller tries again, which
    // costs a retry but means a transient failure cannot poison every later font load for the lifetime
    // of the process.
    if (slot && slot->handle() == nullptr) slot.reset();
  }
  return slot;
}

Library::~Library() = default;

TypefaceImpl::~TypefaceImpl() {
  // The face must be released before the library it came from, which is why the library is held here
  // as a `shared_ptr` member rather than looked up: destruction order within this struct is reverse of
  // declaration, so `face` is done first and `library` last.
  if (face != nullptr) FT_Done_Face(face);
}

std::string ftString(const char* string) { return string != nullptr ? std::string(string) : std::string(); }

} // namespace graphics::font_detail
