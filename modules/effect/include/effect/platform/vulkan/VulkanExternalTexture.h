#ifndef CORAL_EFFECT_VULKAN_EXTERNAL_TEXTURE_H
#define CORAL_EFFECT_VULKAN_EXTERNAL_TEXTURE_H

#include <effect/EffectTexture.h>

#include <memory>

namespace backend {
class Device;
}

namespace coral {

/** A VkImage owned by the caller and created on the same VkDevice the engine uses. */
class VulkanExternalTexture final : public EffectExternalTexture {
public:
  VulkanExternalTexture(void* image, void* view, uint32_t width, uint32_t height,
                        EffectPixelFormat format, uint64_t version = 1);
  ~VulkanExternalTexture() override;

  [[nodiscard]] uint64_t version() const noexcept override { return mVersion; }
  [[nodiscard]] void* nativeHandle() const noexcept override;
  [[nodiscard]] uint32_t nativeKind() const noexcept override;

  [[nodiscard]] void* image() const noexcept { return mImage; }
  [[nodiscard]] void* view() const noexcept { return mView; }
  [[nodiscard]] uint32_t width() const noexcept { return mWidth; }
  [[nodiscard]] uint32_t height() const noexcept { return mHeight; }
  [[nodiscard]] EffectPixelFormat format() const noexcept { return mFormat; }

private:
  void* mImage = nullptr;
  void* mView = nullptr;
  uint32_t mWidth = 0;
  uint32_t mHeight = 0;
  EffectPixelFormat mFormat = EffectPixelFormat::RGBA8;
  uint64_t mVersion = 1;
};

std::shared_ptr<EffectExternalTextureResolver> createVulkanExternalTextureResolver(backend::Device& device);

} // namespace coral

#endif // CORAL_EFFECT_VULKAN_EXTERNAL_TEXTURE_H
