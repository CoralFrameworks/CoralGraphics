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

#ifndef CORAL_GRAPHICS_FONT_FONT_CONTEXT_H
#define CORAL_GRAPHICS_FONT_FONT_CONTEXT_H

#include <graphics/Typeface.h>

#include <ft2build.h>
#include FT_FREETYPE_H


#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

/**
 * @file FontContext.h
 *
 * The FreeType library handle, shared by every typeface.
 *
 * Private to `modules/graphics`, and not installed -- it names FreeType and harfbuzz types, which the
 * public font headers deliberately do not.
 *
 * ## Why the library handle is shared and reference counted
 *
 * `FT_Init_FreeType` is not cheap -- it builds FreeType's internal module list and allocates its memory
 * management state -- and it is not something to do once per font. But a global that is initialised on
 * first use and destroyed at exit has its own problem: a `Typeface` that outlives it, or a static
 * destructor order that runs it before the last face is freed, gives a use-after-free that is very hard
 * to attribute.
 *
 * A `shared_ptr` with a custom deleter solves both. The library is created once, every `Typeface` holds
 * a reference, and it is destroyed when the last face goes -- in whatever order that happens to be.
 */

namespace graphics::font_detail {

/** A reference to the process-wide FreeType library, destroyed with the last reference. */
class Library {
public:
  /** @return the shared library handle, or null when FreeType could not be initialised at all. */
  [[nodiscard]] static std::shared_ptr<Library> acquire();

  virtual ~Library();

  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;

  [[nodiscard]] FT_Library handle() const noexcept { return mLibrary; }

protected:
  Library() = default;

  /**
   * The handle, writable only by the concrete holder.
   *
   * Protected rather than assigned through a setter so that the base class needs no public mutator: the
   * only thing entitled to set it is the subclass that initialises it, and that entitlement is
   * expressed by inheritance rather than by an accessor anyone could call.
   */
  FT_Library mLibrary = nullptr;
};

/**
 * The private state of a `Typeface`.
 *
 * Defined here rather than in `Typeface.cpp` because two translation units need it complete:
 * `Typeface.cpp` to build one, and `Font.cpp` to reach the FreeType face out of it. `Typeface.h`
 * declares `Impl` as an incomplete nested type so that the public header stays free of FreeType, and
 * `Typeface` defines it as a subclass of this -- which is the standard way to keep the name nested while
 * the definition is shared.
 */
struct TypefaceImpl {
  /** Keeps the library alive for as long as any face from it exists. */
  std::shared_ptr<Library> library;

  FT_Face face = nullptr;

  /**
   * The file the face came from, empty for an in-memory face.
   *
   * A platform text engine (DirectWrite, CoreText) needs to open the same file itself; keeping the
   * path here is what lets `Typeface::glyphBitmap(subpixel=true)` hand the platform a real face
   * instead of re-deriving one from a glyph bitmap.
   */
  std::string filePath;

  /** The face index inside `filePath`; zero for the common single-face file. */
  uint32_t faceIndex = 0;

  /**
   * The bytes a memory face was created from, and their size.
   *
   * Kept so that a variation request can clone the face with `FT_New_Memory_Face` without asking the
   * caller for the buffer again. The bytes themselves are owned by @c owner, exactly as before; this
   * is a borrowed view that lives no longer than that owner.
   */
  const uint8_t* memoryData = nullptr;
  size_t memorySize = 0;

  /**
   * Keeps the font's bytes alive when the face was created from memory.
   *
   * FreeType does not copy the data it is handed: it reads the tables out of the buffer on demand, for
   * as long as the face lives. A caller that passes a temporary therefore gets a use-after-free that
   * manifests as garbage glyphs. Holding the owner here is what makes the requirement enforceable
   * rather than merely documented -- and it is empty for a face created from a file, where FreeType
   * owns the stream itself.
   */
  std::shared_ptr<const void> owner;

  /**
   * Serialises the FreeType face's mutable state: the selected size and the glyph slot.
   *
   * `glyphPath` and `glyphBitmap` both load into that slot. Without this, a text run recorded on
   * one thread and a fallback bitmap load on another would read each other's glyph. The lock also
   * guards `glyphBitmaps`; it is mutable because both accessors are `const`.
   */
  mutable std::mutex glyphMutex;

  /** Rendered colour bitmaps, keyed by glyph id and the requested size in 26.6 pixels. */
  mutable std::unordered_map<uint64_t, GlyphBitmap> glyphBitmaps;

  ~TypefaceImpl();
};

/** @return the name FreeType reports for @p string, or an empty string when it is null. */
[[nodiscard]] std::string ftString(const char* string);

} // namespace graphics::font_detail

namespace graphics {

/**
 * The definition of the nested `Impl` that `Typeface.h` declares.
 *
 * It has to be defined in a header rather than in `Typeface.cpp`, because two translation units need it
 * complete: `Typeface.cpp` to build one, and `Font.cpp` to reach the FreeType face out of it. Defining it
 * in the implementation file would leave `Font.cpp` looking at an incomplete type, and the failure --
 * "member `face` of an undefined type" -- reads as though the member were misspelled rather than as
 * though the definition were merely invisible.
 *
 * Open code after a namespace declaration is how a nested class of a class in that namespace is defined
 * from outside: the same syntax as `void Typeface::method()`, applied to a type.
 */
struct Typeface::Impl final : font_detail::TypefaceImpl {};

} // namespace graphics

#endif // CORAL_GRAPHICS_FONT_FONT_CONTEXT_H
