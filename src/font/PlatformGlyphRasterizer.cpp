/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "PlatformGlyphRasterizer.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <dwrite.h>
#include <windows.h>
#elif defined(__linux__) && defined(CORAL_GRAPHICS_HAS_FONTCONFIG)
#include <fontconfig/fontconfig.h>
#endif

namespace graphics::font_detail {

#if defined(_WIN32)

namespace {

std::wstring toWide(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
    static_cast<int>(utf8.size()), nullptr, 0);
  if (length <= 0) return {};
  std::wstring result(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
    result.data(), length);
  return result;
}

/**
 * DirectWrite's glyph run analyser, cached per font file.
 *
 * DirectWrite is the Windows ClearType rasteriser: it applies the platform's gamma, contrast,
 * hinting and colour-fringing policy to a glyph run. The analyser is created for one em size and one
 * baseline origin; `pixelsPerDip` is 1 because the caller's `pixelSize` is already in device pixels.
 */
class DirectWriteRasterizer {
 public:
  static DirectWriteRasterizer& instance() {
    static DirectWriteRasterizer rasterizer;
    return rasterizer;
  }

  [[nodiscard]] bool rasterize(const std::string& path, const uint32_t faceIndex,
      const uint32_t glyphId, const float pixelSize, const float phaseX, GlyphBitmap& out) {
    if (path.empty() || glyphId > 0xFFFFu || !(pixelSize > 0.0f)) return false;
    IDWriteFontFace* face = faceFor(path, faceIndex);
    if (face == nullptr) return false;

    DWRITE_GLYPH_RUN run{};
    run.fontFace = face;
    run.fontEmSize = pixelSize;
    run.glyphCount = 1;
    const auto index = static_cast<UINT16>(glyphId);
    run.glyphIndices = &index;
    FLOAT advance = 0.0f;
    run.glyphAdvances = &advance;
    run.glyphOffsets = nullptr;
    run.isSideways = FALSE;
    run.bidiLevel = 0;

    IDWriteGlyphRunAnalysis* analysis = nullptr;
    const HRESULT created = factory_->CreateGlyphRunAnalysis(&run, 1.0f, nullptr,
      DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL_SYMMETRIC, DWRITE_MEASURING_MODE_NATURAL,
      std::clamp(phaseX, 0.0f, 0.999f), 0.0f, &analysis);
    if (FAILED(created) || analysis == nullptr) return false;

    RECT bounds{};
    const HRESULT measured = analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds);
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (FAILED(measured) || width <= 0 || height <= 0) {
      analysis->Release();
      return false;
    }

    const size_t stride = static_cast<size_t>(width) * 3u;
    const size_t textureSize = stride * static_cast<size_t>(height);
    std::vector<uint8_t> coverage(textureSize);
    const HRESULT rendered = analysis->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds,
      coverage.data(), static_cast<UINT32>(coverage.size()));
    analysis->Release();
    if (FAILED(rendered)) return false;

    Bitmap pixels = Bitmap::allocate(width, height);
    if (pixels.isEmpty()) return false;
    Pixmap view = pixels.lockPixels();
    if (!view.isValid()) return false;
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const uint8_t* texel = coverage.data() + static_cast<size_t>(y) * stride +
          static_cast<size_t>(x) * 3u;
        // DWRITE_TEXTURE_CLEARTYPE_3x1 stores one coverage byte per colour channel; alpha stays
        // opaque because the three channels are the coverage, not an alpha mask.
        const uint32_t argb = (255u << 24) | (static_cast<uint32_t>(texel[0]) << 16) |
                              (static_cast<uint32_t>(texel[1]) << 8) |
                              static_cast<uint32_t>(texel[2]);
        view.setPixelARGB32(x, y, argb);
      }
    }

    out.pixels = std::move(pixels);
    out.left = static_cast<float>(bounds.left);
    out.top = static_cast<float>(-bounds.top);
    out.width = static_cast<float>(width);
    out.height = static_cast<float>(height);
    out.color = false;
    out.subpixel = true;
    return true;
  }

 private:
  DirectWriteRasterizer() {
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
          reinterpret_cast<IUnknown**>(&factory_)))) {
      factory_ = nullptr;
    }
  }

  ~DirectWriteRasterizer() {
    for (auto& entry: faces_) {
      if (entry.second != nullptr) entry.second->Release();
    }
    faces_.clear();
    if (factory_ != nullptr) factory_->Release();
  }

  [[nodiscard]] IDWriteFontFace* faceFor(const std::string& path, const uint32_t faceIndex) {
    if (factory_ == nullptr) return nullptr;
    const std::string key = path + "#" + std::to_string(faceIndex);
    {
      const std::lock_guard lock(mutex_);
      const auto found = faces_.find(key);
      if (found != faces_.end()) return found->second;
    }

    const std::wstring wide = toWide(path);
    if (wide.empty()) return nullptr;
    IDWriteFontFile* file = nullptr;
    if (FAILED(factory_->CreateFontFileReference(wide.c_str(), nullptr, &file)) || file == nullptr) {
      return nullptr;
    }

    IDWriteFontFace* face = nullptr;
    // `.otf` is usually CFF and `.ttf` usually TrueType; try both because a font's extension is not a
    // promise about its outline format.
    const DWRITE_FONT_FACE_TYPE types[] = {
      DWRITE_FONT_FACE_TYPE_CFF,
      DWRITE_FONT_FACE_TYPE_TRUETYPE,
    };
    for (const DWRITE_FONT_FACE_TYPE type: types) {
      if (SUCCEEDED(factory_->CreateFontFace(type, 1, &file, faceIndex,
            DWRITE_FONT_SIMULATIONS_NONE, &face)) &&
          face != nullptr) {
        break;
      }
      face = nullptr;
    }
    file->Release();
    if (face == nullptr) return nullptr;

    const std::lock_guard lock(mutex_);
    const auto [position, inserted] = faces_.emplace(key, face);
    if (!inserted) {
      face->Release();
      return position->second;
    }
    return face;
  }

  IDWriteFactory* factory_ = nullptr;
  std::mutex mutex_;
  std::unordered_map<std::string, IDWriteFontFace*> faces_;
};

} // namespace

#endif // _WIN32

bool rasterizePlatformGlyph(const std::string& filePath, const uint32_t faceIndex,
    const uint32_t glyphId, const float pixelSize, const float phaseX, GlyphBitmap& out) {
#if defined(_WIN32)
  return DirectWriteRasterizer::instance().rasterize(filePath, faceIndex, glyphId, pixelSize, phaseX,
    out);
#else
  (void)filePath;
  (void)faceIndex;
  (void)glyphId;
  (void)pixelSize;
  (void)phaseX;
  (void)out;
  return false;
#endif
}

} // namespace graphics::font_detail

namespace graphics {

namespace {

SubpixelRenderingParams querySubpixelRenderingParams() {
#if defined(_WIN32)
  SubpixelRenderingParams result;
  IDWriteFactory* factory = nullptr;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&factory))) ||
      factory == nullptr) {
    return result;
  }
  IDWriteRenderingParams* rendering = nullptr;
  if (SUCCEEDED(factory->CreateRenderingParams(&rendering)) && rendering != nullptr) {
    result.gamma = rendering->GetGamma();
    result.contrast = rendering->GetEnhancedContrast();
    result.clearTypeLevel = rendering->GetClearTypeLevel();
    result.pixelGeometry = static_cast<int>(rendering->GetPixelGeometry());
    rendering->Release();
  }
  factory->Release();
  return result;
#else
  SubpixelRenderingParams result;
#if defined(__linux__)
  // Fontconfig describes the panel's subpixel order as FC_RGBA. The FreeType LCD path already
  // produces three-channel coverage; the compositor only needs to know whether to swap R/B.
  int geometry = 1;
#if defined(CORAL_GRAPHICS_HAS_FONTCONFIG)
  int rgba = FC_RGBA_UNKNOWN;
  FcPattern* pattern = FcPatternCreate();
  if (pattern != nullptr) {
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcPatternGetInteger(pattern, FC_RGBA, 0, &rgba);
    FcPatternDestroy(pattern);
  }
  if (rgba == FC_RGBA_BGR || rgba == FC_RGBA_VBGR) geometry = 2;
  else if (rgba == FC_RGBA_NONE) geometry = 0;
#endif
  if (const char* overrideOrder = std::getenv("CORAL_SUBPIXEL_ORDER"); overrideOrder != nullptr) {
    if (std::string(overrideOrder) == "bgr") geometry = 2;
    else if (std::string(overrideOrder) == "rgb") geometry = 1;
    else if (std::string(overrideOrder) == "flat") geometry = 0;
  }
  result.pixelGeometry = geometry;
#endif
  return result;
#endif
}

std::mutex& paramsMutex() {
  static std::mutex mutex;
  return mutex;
}

std::shared_ptr<const SubpixelRenderingParams>& cachedParams() {
  static std::shared_ptr<const SubpixelRenderingParams> params;
  return params;
}

std::chrono::steady_clock::time_point& paramsTimestamp() {
  static std::chrono::steady_clock::time_point timestamp{};
  return timestamp;
}

} // namespace anonymous

SubpixelRenderingParams subpixelRenderingParams() {
  const std::lock_guard lock(paramsMutex());
  const auto now = std::chrono::steady_clock::now();
  // A library has no window to receive WM_SETTINGCHANGE, so re-read the system parameters once a
  // second. The explicit refresh API still gives an immediate update for callers that do listen.
  if (cachedParams() == nullptr || now - paramsTimestamp() >= std::chrono::seconds(1)) {
    cachedParams() = std::make_shared<const SubpixelRenderingParams>(querySubpixelRenderingParams());
    paramsTimestamp() = now;
  }
  return *cachedParams();
}

void refreshSubpixelRenderingParams() {
  const std::lock_guard lock(paramsMutex());
  cachedParams().reset();
  paramsTimestamp() = std::chrono::steady_clock::time_point{};
}

} // namespace graphics
