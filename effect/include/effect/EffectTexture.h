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

#ifndef CORAL_EFFECT_TEXTURE_H
#define CORAL_EFFECT_TEXTURE_H

#include <effect/EffectFrame.h>
#include <effect/EffectTypes.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coral {

namespace detail {
struct TextureSourceAccess;
} // namespace detail

/** Whether an image's colour channels are already multiplied by its alpha. */
enum class EffectAlphaMode : uint8_t {
  Premultiplied,
  Unpremultiplied,
};

/**
 * A CPU image, as an effect pass that samples a bitmap needs it.
 *
 * `create` copies the pixels into the returned image, so the caller may release the input buffer as soon
 * as it returns. The copy is what lets the render thread upload the image on its own schedule without
 * the application having to keep a decode buffer alive.
 */
struct EffectImageDesc {
  uint32_t width = 0;
  uint32_t height = 0;
  EffectPixelFormat format = EffectPixelFormat::RGBA8;
  EffectColorSpace colorSpace = EffectColorSpace::SRGB;
  EffectAlphaMode alphaMode = EffectAlphaMode::Premultiplied;
  /** Bytes between the start of one row and the next; 0 means tightly packed. */
  uint32_t rowBytes = 0;
  const void* pixels = nullptr;
  size_t byteSize = 0;

  /** @return a tightly packed RGBA8 description of @p pixels, with @p rowBytes when it is non-zero. */
  static EffectImageDesc rgba8(const void* pixels, uint32_t width, uint32_t height, uint32_t rowBytes = 0);
};

/** An immutable, CPU-side image. */
class EffectImage final {
public:
  /** Copies `desc.pixels` into storage this image owns. @return null when the description is invalid. */
  static std::shared_ptr<const EffectImage> create(const EffectImageDesc& desc);

  ~EffectImage();

  EffectImage(const EffectImage&) = delete;
  EffectImage& operator=(const EffectImage&) = delete;

  [[nodiscard]] uint32_t width() const noexcept;
  [[nodiscard]] uint32_t height() const noexcept;
  [[nodiscard]] EffectPixelFormat format() const noexcept;
  [[nodiscard]] EffectColorSpace colorSpace() const noexcept;
  [[nodiscard]] EffectAlphaMode alphaMode() const noexcept;
  [[nodiscard]] uint32_t rowBytes() const noexcept;
  [[nodiscard]] size_t byteSize() const noexcept;
  /** Borrowed pixel data, valid for this image's lifetime. Never null for a valid image. */
  [[nodiscard]] const void* pixels() const noexcept;

private:
  struct Impl;

  explicit EffectImage(std::shared_ptr<const Impl> impl) noexcept;

  std::shared_ptr<const Impl> mImpl;
};

/**
 * A platform texture the engine can sample without owning it.
 *
 * The platform node that creates one implements `version`; the engine re-binds the texture when the
 * version changes and never copies its pixels. The base class deliberately exposes nothing else: what a
 * platform texture *is* stays in the platform target.
 */
class EffectExternalTexture {
public:
  virtual ~EffectExternalTexture();

  /** A monotonically changing value identifying the current contents of the texture. */
  [[nodiscard]] virtual uint64_t version() const noexcept = 0;

  /** Opaque platform object (for example a GL texture name); only a platform resolver interprets it. */
  [[nodiscard]] virtual void* nativeHandle() const noexcept = 0;

  /** Platform-defined kind understood by the matching resolver. */
  [[nodiscard]] virtual uint32_t nativeKind() const noexcept = 0;
};

/** Platform-side binding of EffectExternalTexture objects. */
class EffectExternalTextureResolver {
public:
  virtual ~EffectExternalTextureResolver();

  /**
   * Resolves the texture to an opaque backend texture token.
   * @return 0 when the platform cannot sample this texture.
   */
  virtual uint64_t resolve(const EffectExternalTexture& texture, uint64_t& resolvedVersion,
                           std::string& error) = 0;
};

/**
 * A description of where a pass's sampler input comes from.
 *
 * This is a **description**, not a GPU texture: the render thread resolves it by uploading an image,
 * binding an external texture, sampling a pooled frame, or using a one-texel solid colour. Setting the
 * same source twice is detected by version and does not upload again.
 */
class TextureSource {
public:
  enum class Kind : uint8_t {
    Empty,
    Image,
    External,
    Frame,
    Solid,
    Native,
  };

  TextureSource() noexcept = default;

  static TextureSource fromImage(std::shared_ptr<const EffectImage> image);
  static TextureSource fromExternal(std::shared_ptr<EffectExternalTexture> texture);

  /**
   * Samples a texture the caller owns on the engine device, without a copy.
   *
   * `textureToken` is the opaque value of a texture handle of the same device the engine renders on,
   * for example `gpu::Surface::target().value()` of a surface created with that device. The engine never
   * owns or releases the texture; the caller keeps it alive until the pass that samples it has finished.
   */
  static TextureSource fromNativeTexture(uint64_t textureToken, uint32_t width, uint32_t height,
    EffectPixelFormat format = EffectPixelFormat::RGBA8);

  static TextureSource fromFrame(const EffectFrame& frame);
  static TextureSource solid(EffectColor color);
  static TextureSource empty();

  [[nodiscard]] Kind kind() const noexcept { return mKind; }
  [[nodiscard]] bool isValid() const noexcept { return mKind != Kind::Empty; }

private:
  friend struct detail::TextureSourceAccess;

  Kind mKind = Kind::Empty;
  std::shared_ptr<const EffectImage> mImage;
  std::shared_ptr<EffectExternalTexture> mExternal;
  EffectFrame mFrame;
  EffectColor mColor;
  uint64_t mNativeTexture = 0;
  uint32_t mNativeWidth = 0;
  uint32_t mNativeHeight = 0;
  EffectPixelFormat mNativeFormat = EffectPixelFormat::RGBA8;
};

/**
 * A sequence of textures played back against `RenderTime`.
 *
 * The default driver is time: the render thread selects
 * `floor((time - startTime) / frameDuration)`, so a seek or a dropped frame lands directly on the right
 * index instead of replaying the frames in between. `setSequenceFrame` switches one sequence to manual
 * indexing until `setSequenceTime` or `clearSequenceOverride` returns it to the clock.
 */
struct EffectTextureSequenceDesc {
  std::vector<TextureSource> frames;
  /** How long one frame is shown. Two ticks at 60 Hz is 30 fps. */
  RenderTime frameDuration { 2, 60 };
  /** The time the first frame starts at, in the engine's time base. */
  RenderTime startTime { 0, 60 };
  /** Wrap around at the end instead of holding the last frame. */
  bool loop = true;
};

} // namespace coral

#endif // CORAL_EFFECT_TEXTURE_H
