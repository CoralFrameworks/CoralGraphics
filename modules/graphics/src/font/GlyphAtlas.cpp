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

#include <graphics/GlyphAtlas.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace graphics {

namespace {
constexpr int kPageSize = 1024;
constexpr uint32_t kMaxPages = 16;
}

struct GlyphAtlas::Impl {
  struct Page {
    Bitmap bitmap;
    int penX = 1;
    int penY = 1;
    int rowHeight = 0;
  };

  struct Key {
    const Typeface* typeface = nullptr;
    uint32_t glyphId = 0;
    uint32_t sizeKey = 0;
    bool operator==(const Key& other) const noexcept {
      return typeface == other.typeface && glyphId == other.glyphId && sizeKey == other.sizeKey;
    }
  };

  struct KeyHash {
    size_t operator()(const Key& key) const noexcept {
      size_t value = std::hash<const void*>{}(key.typeface);
      value ^= std::hash<uint32_t>{}(key.glyphId) + 0x9e3779b9u + (value << 6) + (value >> 2);
      value ^= std::hash<uint32_t>{}(key.sizeKey) + 0x9e3779b9u + (value << 6) + (value >> 2);
      return value;
    }
  };

  std::mutex mutex;
  std::vector<Page> pages;
  std::unordered_map<Key, Entry, KeyHash> entries;

  Page* pageWithRoom(const int width, const int height) {
    for (Page& page: pages) {
      if (page.bitmap.isEmpty()) page.bitmap = Bitmap::allocate(kPageSize, kPageSize);
      if (page.bitmap.isEmpty()) continue;
      if (page.penX + width + 1 > kPageSize) {
        page.penX = 1;
        page.penY += page.rowHeight + 1;
        page.rowHeight = 0;
      }
      if (page.penY + height + 1 <= kPageSize) return &page;
    }
    if (pages.size() >= kMaxPages) return nullptr;
    pages.push_back(Page{});
    Page& page = pages.back();
    page.bitmap = Bitmap::allocate(kPageSize, kPageSize);
    if (page.bitmap.isEmpty()) {
      pages.pop_back();
      return nullptr;
    }
    return &page;
  }
};

GlyphAtlas::GlyphAtlas() : mImpl(new Impl()) {}

GlyphAtlas::~GlyphAtlas() {
  delete mImpl;
}

GlyphAtlas& GlyphAtlas::instance() {
  static GlyphAtlas atlas;
  return atlas;
}

std::optional<GlyphAtlas::Entry> GlyphAtlas::add(const GlyphBitmap& glyph, const uint32_t glyphId,
    const float pixelSize, const Typeface* typeface) {
  if (!glyph.isValid() || glyph.color || glyph.subpixel) return std::nullopt;
  const auto quantized = static_cast<uint64_t>(std::llround(pixelSize * 64.0));
  const Impl::Key key{ typeface, glyphId, static_cast<uint32_t>(quantized) };
  const std::lock_guard lock(mImpl->mutex);
  if (const auto found = mImpl->entries.find(key); found != mImpl->entries.end()) {
    return found->second;
  }
  const int width = static_cast<int>(std::ceil(glyph.width));
  const int height = static_cast<int>(std::ceil(glyph.height));
  if (width <= 0 || height <= 0 || width + 2 > kPageSize || height + 2 > kPageSize) {
    return std::nullopt;
  }
  Impl::Page* page = mImpl->pageWithRoom(width, height);
  if (page == nullptr) return std::nullopt;
  Pixmap target = page->bitmap.lockPixels();
  if (!target.isValid()) return std::nullopt;
  const Pixmap source = glyph.pixels.makePixmap();
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      target.setPixelARGB32(page->penX + x, page->penY + y, source.getPixelARGB32(x, y));
    }
  }
  Entry entry;
  entry.page = static_cast<uint32_t>(page - mImpl->pages.data());
  entry.source = Rect::MakeXYWH(static_cast<float>(page->penX), static_cast<float>(page->penY),
    static_cast<float>(width), static_cast<float>(height));
  mImpl->entries.emplace(key, entry);
  page->penX += width + 1;
  page->rowHeight = std::max(page->rowHeight, height);
  return entry;
}

const Bitmap& GlyphAtlas::bitmap(const uint32_t page) const {
  static const Bitmap empty;
  const std::lock_guard lock(mImpl->mutex);
  if (page >= mImpl->pages.size()) return empty;
  return mImpl->pages[page].bitmap;
}

uint32_t GlyphAtlas::pageCount() const noexcept {
  const std::lock_guard lock(mImpl->mutex);
  return static_cast<uint32_t>(mImpl->pages.size());
}

size_t GlyphAtlas::bytes() const noexcept {
  const std::lock_guard lock(mImpl->mutex);
  size_t total = 0;
  for (const Impl::Page& page: mImpl->pages) total += page.bitmap.data().size();
  return total;
}

} // namespace graphics
