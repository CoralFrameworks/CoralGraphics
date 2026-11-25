#ifndef CORAL_EFFECT_GL_EXTERNAL_TEXTURE_H
#define CORAL_EFFECT_GL_EXTERNAL_TEXTURE_H

#include <effect/EffectTexture.h>

#include <memory>

namespace backend {
class Device;
}

namespace coral {

/** A GL texture the engine samples without owning it. */
class GlExternalTexture final : public EffectExternalTexture {
public:
  GlExternalTexture(uint32_t glTexture, uint32_t glTarget, uint32_t width, uint32_t height,
                    EffectPixelFormat format, uint64_t version = 1);
  ~GlExternalTexture() override;

  [[nodiscard]] uint64_t version() const noexcept override { return mVersion; }
  [[nodiscard]] void* nativeHandle() const noexcept override;
  [[nodiscard]] uint32_t nativeKind() const noexcept override { return mTarget; }

  [[nodiscard]] uint32_t glTexture() const noexcept { return mTexture; }
  [[nodiscard]] uint32_t glTarget() const noexcept { return mTarget; }
  [[nodiscard]] uint32_t width() const noexcept { return mWidth; }
  [[nodiscard]] uint32_t height() const noexcept { return mHeight; }
  [[nodiscard]] EffectPixelFormat format() const noexcept { return mFormat; }

private:
  uint32_t mTexture = 0;
  uint32_t mTarget = 0;
  uint32_t mWidth = 0;
  uint32_t mHeight = 0;
  EffectPixelFormat mFormat = EffectPixelFormat::RGBA8;
  uint64_t mVersion = 1;
};

/** Creates a GL resolver bound to @p device; register it from setOnDeviceReady. */
std::shared_ptr<EffectExternalTextureResolver> createGlExternalTextureResolver(backend::Device& device);

} // namespace coral

#endif // CORAL_EFFECT_GL_EXTERNAL_TEXTURE_H
