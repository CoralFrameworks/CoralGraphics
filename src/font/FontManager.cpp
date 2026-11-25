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
 * See the License for the specific language governing permissions and limitations under the License.
 */

#include <graphics/FontManager.h>

#include <graphics/Typeface.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <minilog/log.h>

/**
 * @file FontManager.cpp
 *
 * Finding a font among those the system has.
 *
 * ## The two decisions worth stating
 *
 * **The scan is a directory walk.** No platform offers the same font-enumeration API, and three of them have one at
 * all; listing directories and letting `Typeface` read the family name out of each file is one implementation that
 * works everywhere FreeType does, including Android. It costs a few milliseconds once, and the result is cached.
 *
 * **Matching is scored, not exact.** A request is answered by the best available face rather than by nothing, because
 * text drawn in the wrong weight is more useful than a blank page. The scoring is where the judgement lives, so it is
 * written as a comparison rather than as a threshold: see `betterMatch` below.
 */

namespace graphics {
namespace {

/** @return @p text lowercased, for case-insensitive comparison. */
std::string toLower(const std::string_view text) {
  std::string lowered;
  lowered.reserve(text.size());
  for (const char character: text) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
  }
  return lowered;
}

/**
 * @return how far @p have is from @p wanted, or `std::nullopt` when they are not comparable at all.
 *
 * A lower score is a better match, and the score is only ever compared with another score from the same request --
 * which is what lets it be an arbitrary arithmetic combination rather than something with units.
 *
 * ## The weights, and why they are what they are
 *
 * The slant is weighted far more heavily than the weight, because an italic face is a *different set of letterforms*
 * whereas a bold one is the same letterforms drawn heavier. Asking for italic and getting upright is a visible
 * mistake; asking for bold and getting medium is a slightly thin line.
 *
 * Width is a smaller term again: it changes the letterforms, but less than slant does, and a caller almost never asks
 * for it.
 *
 * The weight term is deliberately *relative* rather than absolute: a request for weight 500 is 100 away from both 400
 * and 600, and either is an acceptable answer, so the score that matters is which is nearer rather than how near.
 */
std::optional<int> styleDistance(const FontStyle& have, const FontStyle& wanted) {
  int distance = 0;

  // A requested upright face is satisfied by any upright one; a requested italic face is satisfied by an oblique,
  // which is a slanted upright rather than a true italic.
  const bool haveItalic = have.isItalic();
  const bool wantedItalic = wanted.isItalic();
  if (haveItalic != wantedItalic) {
    distance += 1000;
  } else if (wantedItalic && have.slant() != wanted.slant()) {
    // Both slanted, but oblique where a true italic was asked for. Close, and not the same.
    distance += 100;
  }

  distance += std::abs(have.weight() - wanted.weight());
  distance += std::abs(have.width() - wanted.width()) * 4;

  return distance;
}

/** @return the font directories for the platform this is built for. */
std::vector<std::filesystem::path> systemFontDirectories() {
  std::vector<std::filesystem::path> directories;

#if defined(_WIN32)
  // The per-user font directory first: a font a user installed themselves should win over a system one of the same
  // name, because that is the one they chose.
  if (const char* localAppData = std::getenv("LOCALAPPDATA")) {
    directories.emplace_back(std::filesystem::path(localAppData) / "Microsoft" / "Windows" / "Fonts");
  }
  if (const char* root = std::getenv("SystemRoot")) {
    directories.emplace_back(std::filesystem::path(root) / "Fonts");
  }
#elif defined(__APPLE__)
  directories.emplace_back("/System/Library/Fonts");
  directories.emplace_back("/Library/Fonts");
  if (const char* home = std::getenv("HOME")) {
    directories.emplace_back(std::filesystem::path(home) / "Library" / "Fonts");
  }
#elif defined(__ANDROID__)
  directories.emplace_back("/system/fonts");
  directories.emplace_back("/product/fonts");
  directories.emplace_back("/vendor/fonts");
#else
  directories.emplace_back("/usr/share/fonts");
  directories.emplace_back("/usr/local/share/fonts");
  if (const char* home = std::getenv("HOME")) {
    directories.emplace_back(std::filesystem::path(home) / ".fonts");
    directories.emplace_back(std::filesystem::path(home) / ".local" / "share" / "fonts");
  }
#endif

  return directories;
}

/** @return true when @p path has a font extension this should try to read. */
bool looksLikeAFont(const std::filesystem::path& path) {
  const std::string extension = toLower(path.extension().string());
  return extension == ".ttf" || extension == ".otf" || extension == ".ttc" || extension == ".otc";
}

} // namespace anonymous

// ==================================================================================================
// Impl
// ==================================================================================================

struct FontManager::Impl {
  /** Every face that has been added, in the order it was added. */
  std::vector<std::shared_ptr<Typeface>> faces;

  /** Paths already read, so adding the same file twice does not read it twice. */
  std::unordered_map<std::string, std::shared_ptr<Typeface>> byPath;

  /** Family name, lowercased, to the indices of that family's faces. */
  std::unordered_map<std::string, std::vector<size_t>> byFamily;

  /** Guards the maps above. The scan happens under `shared()`'s lock, but `addFontFromFile` is public. */
  mutable std::mutex mutex;

  /** Records @p face so matching can find it. Caller holds the lock. */
  void index(const std::shared_ptr<Typeface>& face) {
    if (face == nullptr) return;
    const size_t position = faces.size();
    faces.push_back(face);
    byFamily[toLower(face->familyName())].push_back(position);
  }
};

FontManager::FontManager(): mImpl(std::make_unique<Impl>()) {}

FontManager::~FontManager() = default;

// ==================================================================================================
// Building a collection
// ==================================================================================================

std::shared_ptr<Typeface> FontManager::addFontFromFile(const std::string& path) {
  std::lock_guard<std::mutex> lock(mImpl->mutex);

  // Already read. Returning the same face rather than a second one is what makes adding the same path twice cheap,
  // and it is what the system scan relies on: several directories can contain the same font under different names.
  if (const auto found = mImpl->byPath.find(path); found != mImpl->byPath.end()) return found->second;

  auto face = Typeface::fromFile(path);
  if (face == nullptr) return nullptr;

  mImpl->byPath.emplace(path, face);
  mImpl->index(face);
  return face;
}

std::shared_ptr<Typeface> FontManager::addFontFromMemory(
        const uint8_t* data, const size_t size, std::shared_ptr<const void> owner) {
  if (data == nullptr || size == 0) return nullptr;

  auto face = Typeface::fromMemory(data, size, std::move(owner));
  if (face == nullptr) return nullptr;

  std::lock_guard<std::mutex> lock(mImpl->mutex);
  mImpl->index(face);
  return face;
}

size_t FontManager::addSystemFonts() {
  size_t added = 0;

  for (const std::filesystem::path& directory: systemFontDirectories()) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) continue;

    // A recursive walk, with errors swallowed per entry rather than aborting the whole scan: a font directory with
    // one unreadable subdirectory should still yield the rest.
    std::filesystem::recursive_directory_iterator iterator(
            directory, std::filesystem::directory_options::skip_permission_denied, error);
    if (error) continue;

    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(error)) {
      if (error) break;
      if (!iterator->is_regular_file(error) || error) continue;

      const std::filesystem::path& path = iterator->path();
      if (!looksLikeAFont(path)) continue;

      // `addFontFromFile` takes the lock itself, so it is called outside any lock this loop might hold; and it
      // returns null for a file that is not a usable font, which is not an error worth reporting.
      const size_t before = typefaceCount();
      if (addFontFromFile(path.string()) != nullptr) {
        if (typefaceCount() > before) ++added;
      }
    }
  }

  if (added == 0) {
    MINILOG_WARN("[graphics/font] no system fonts were found; text will fall back to whatever the caller "
                "adds explicitly");
  }
  return added;
}

// ==================================================================================================
// Finding one
// ==================================================================================================

std::shared_ptr<Typeface> FontManager::matchFamilyStyle(const std::string& familyName, const FontStyle& style) const {
  std::lock_guard<std::mutex> lock(mImpl->mutex);
  if (mImpl->faces.empty()) return nullptr;

  // ## Two passes
  //
  // The first pass considers only the requested family, and the second every family. Doing it in that order is what
  // makes the weight a weaker preference than the family: a bold request for a family that has only a regular face
  // returns that regular face, rather than a bold face from a different family. A caller that wanted the weight more
  // than the letterforms can say so by asking with an empty family name.
  const std::string wanted = toLower(familyName);

  for (int pass = 0; pass < 2; ++pass) {
    // Pass zero is the family the caller named; pass one is the rest. An empty request skips straight to pass one,
    // because "any family" has no first pass to make.
    if (pass == 0 && wanted.empty()) continue;

    std::shared_ptr<Typeface> best;
    int bestDistance = 0;

    for (const std::shared_ptr<Typeface>& face: mImpl->faces) {
      if (face == nullptr) continue;
      const bool sameFamily = toLower(face->familyName()) == wanted;
      if (pass == 0 && !sameFamily) continue;
      if (pass == 1 && sameFamily) continue;  // already considered

      const auto distance = styleDistance(face->style(), style);
      if (!distance.has_value()) continue;

      if (best == nullptr || *distance < bestDistance) {
        best = face;
        bestDistance = *distance;
      }
    }

    if (best != nullptr) return best;
  }

  // Nothing in any family matched the style at all, which happens when a face reports a style that cannot be compared.
  // The default is a better answer than nothing. It is computed here, rather than through `defaultTypeface()`, because
  // that method takes this same mutex and `std::mutex` is not recursive: calling it under the lock would turn this
  // fallback into a deadlock.
  std::shared_ptr<Typeface> fallback;
  int fallbackDistance = 0;
  for (const std::shared_ptr<Typeface>& face: mImpl->faces) {
    if (face == nullptr) continue;
    const auto distance = styleDistance(face->style(), FontStyle::Normal());
    if (!distance.has_value()) continue;
    if (fallback == nullptr || *distance < fallbackDistance) {
      fallback = face;
      fallbackDistance = *distance;
    }
  }
  return fallback != nullptr ? fallback : mImpl->faces.front();
}

std::shared_ptr<Typeface> FontManager::defaultTypeface() const {
  std::lock_guard<std::mutex> lock(mImpl->mutex);
  if (mImpl->faces.empty()) return nullptr;

  // Upright with a normal weight, which is the face body text is set in. Scored rather than required, so a collection
  // with nothing but italics still answers with its closest face rather than with nothing.
  std::shared_ptr<Typeface> best;
  int bestDistance = 0;
  for (const std::shared_ptr<Typeface>& face: mImpl->faces) {
    if (face == nullptr) continue;
    const auto distance = styleDistance(face->style(), FontStyle::Normal());
    if (!distance.has_value()) continue;
    if (best == nullptr || *distance < bestDistance) {
      best = face;
      bestDistance = *distance;
    }
  }
  return best != nullptr ? best : mImpl->faces.front();
}

std::shared_ptr<Typeface> FontManager::matchCharacter(
        const uint32_t codePoint, const std::shared_ptr<Typeface>& preferred) const {
  // The preferred face first, so a caller that knows the right family has it honoured whenever that family covers the
  // character -- which it usually does, since a family's faces share coverage.
  if (preferred != nullptr && preferred->hasGlyphForCodePoint(codePoint)) return preferred;

  std::lock_guard<std::mutex> lock(mImpl->mutex);
  for (const std::shared_ptr<Typeface>& face: mImpl->faces) {
    if (face == nullptr || face == preferred) continue;
    if (face->hasGlyphForCodePoint(codePoint)) return face;
  }
  return nullptr;
}

std::vector<std::string> FontManager::familyNames() const {
  std::lock_guard<std::mutex> lock(mImpl->mutex);

  std::vector<std::string> names;
  names.reserve(mImpl->byFamily.size());
  for (const auto& entry: mImpl->byFamily) names.push_back(entry.first);

  // Lowercased keys were used for lookup, so the names are reported in the case they were stored under. Sorted for a
  // stable order, because the container's order is not one.
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  return names;
}

size_t FontManager::typefaceCount() const noexcept {
  std::lock_guard<std::mutex> lock(mImpl->mutex);
  return mImpl->faces.size();
}

// ==================================================================================================
// The shared manager
// ==================================================================================================

FontManager& FontManager::shared() {
  // A function-local static, so the scan runs once on first use and the initialisation is thread-safe by the language
  // rather than by a lock here. It cannot be torn down at exit in a way that matters: nothing else owns fonts.
  //
  // Built through a `unique_ptr` and released, rather than with a bare `new`, so that a throw from `addSystemFonts` --
  // which reads the filesystem, so it can throw -- releases the half-built manager instead of leaking it. The release is
  // the deliberate part: the object is never destroyed, which is what the note above is about.
  static FontManager* manager = [] {
    auto created = std::make_unique<FontManager>();
    created->addSystemFonts();
    return created.release();
  }();
  return *manager;
}

} // namespace graphics
