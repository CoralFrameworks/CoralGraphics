#include <effect/platform/vulkan/VulkanExternalTexture.h>

#include <backend/Device.h>

#include <string>
#include <unordered_map>

namespace coral {
namespace {

backend::PixelFormat toBackendFormat(const EffectPixelFormat format) {
  switch (format) {
    case EffectPixelFormat::RGBA8: return backend::PixelFormat::R8G8B8A8_UNORM;
    case EffectPixelFormat::BGRA8: return backend::PixelFormat::B8G8R8A8_UNORM;
    case EffectPixelFormat::RGBA16F: return backend::PixelFormat::R16G16B16A16_SFLOAT;
    default: return backend::PixelFormat::R8G8B8A8_UNORM;
  }
}

class VulkanExternalTextureResolver final : public EffectExternalTextureResolver {
public:
  explicit VulkanExternalTextureResolver(backend::Device& device) noexcept : mDevice(&device) {}

  uint64_t resolve(const EffectExternalTexture& texture, uint64_t& resolvedVersion,
                   std::string& error) override {
    const auto* external = dynamic_cast<const VulkanExternalTexture*>(&texture);
    if (external == nullptr) {
      error = "the Vulkan resolver received a non-Vulkan texture";
      return 0;
    }
    const auto found = mCache.find(external);
    if (found != mCache.end() && found->second.version == texture.version()) {
      resolvedVersion = found->second.version;
      return found->second.handle.valid() ? found->second.handle.value() : 0;
    }
    backend::NativeTextureDescriptor descriptor;
    descriptor.kind = backend::NativeTextureKind::VulkanImage;
    descriptor.platformHandle = external->image();
    descriptor.platformView = external->view();
    descriptor.width = external->width();
    descriptor.height = external->height();
    descriptor.format = toBackendFormat(external->format());
    backend::TextureHandle handle = backend::TextureHandle::invalid();
    if (!backend::isSuccess(mDevice->importNativeTexture(descriptor, &handle)) || !handle.valid()) {
      error = "the Vulkan device refused to import the external VkImage";
      return 0;
    }
    mCache[external] = { texture.version(), handle };
    resolvedVersion = texture.version();
    return handle.value();
  }

private:
  struct Entry {
    uint64_t version = 0;
    backend::TextureHandle handle;
  };
  backend::Device* mDevice = nullptr;
  std::unordered_map<const VulkanExternalTexture*, Entry> mCache;
};

} // namespace

VulkanExternalTexture::VulkanExternalTexture(void* const image, void* const view,
                                             const uint32_t width, const uint32_t height,
                                             const EffectPixelFormat format, const uint64_t version)
    : mImage(image), mView(view), mWidth(width), mHeight(height), mFormat(format), mVersion(version) {}

VulkanExternalTexture::~VulkanExternalTexture() = default;

void* VulkanExternalTexture::nativeHandle() const noexcept {
  return mImage;
}

uint32_t VulkanExternalTexture::nativeKind() const noexcept {
  return static_cast<uint32_t>(backend::NativeTextureKind::VulkanImage);
}

std::shared_ptr<EffectExternalTextureResolver> createVulkanExternalTextureResolver(backend::Device& device) {
  return std::make_shared<VulkanExternalTextureResolver>(device);
}

} // namespace coral
