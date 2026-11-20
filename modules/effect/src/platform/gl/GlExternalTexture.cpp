#include <effect/platform/gl/GlExternalTexture.h>

#include <backend/Device.h>

#if defined(_WIN32)
#include <windows.h>
#include <GL/gl.h>
#else
#include <GLES3/gl3.h>
#endif

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

class GlExternalTextureResolver final : public EffectExternalTextureResolver {
public:
  explicit GlExternalTextureResolver(backend::Device& device) noexcept : mDevice(&device) {}

  uint64_t resolve(const EffectExternalTexture& texture, uint64_t& resolvedVersion,
                   std::string& error) override {
    const auto* gl = dynamic_cast<const GlExternalTexture*>(&texture);
    if (gl == nullptr) {
      error = "the GL resolver received a non-GL texture";
      return 0;
    }
    const auto found = mCache.find(gl);
    if (found != mCache.end() && found->second.version == texture.version()) {
      resolvedVersion = found->second.version;
      return found->second.handle.valid() ? found->second.handle.value() : 0;
    }
    if (glIsTexture(static_cast<GLuint>(gl->glTexture())) == GL_FALSE) {
      error = "the external GL texture is not visible in the engine context";
      return 0;
    }
    backend::NativeTextureDescriptor descriptor;
    descriptor.kind = backend::NativeTextureKind::OpenGlTexture;
    descriptor.glTexture = gl->glTexture();
    descriptor.glTarget = gl->glTarget();
    descriptor.width = gl->width();
    descriptor.height = gl->height();
    descriptor.format = toBackendFormat(gl->format());
    backend::TextureHandle handle = backend::TextureHandle::invalid();
    if (!backend::isSuccess(mDevice->importNativeTexture(descriptor, &handle)) || !handle.valid()) {
      error = "the GL device refused to import the external texture";
      return 0;
    }
    mCache[gl] = { texture.version(), handle };
    resolvedVersion = texture.version();
    return handle.value();
  }

private:
  struct Entry {
    uint64_t version = 0;
    backend::TextureHandle handle;
  };
  backend::Device* mDevice = nullptr;
  std::unordered_map<const GlExternalTexture*, Entry> mCache;
};

} // namespace

GlExternalTexture::GlExternalTexture(const uint32_t glTexture, const uint32_t glTarget,
                                     const uint32_t width, const uint32_t height,
                                     const EffectPixelFormat format, const uint64_t version)
    : mTexture(glTexture), mTarget(glTarget), mWidth(width), mHeight(height), mFormat(format),
      mVersion(version) {}

GlExternalTexture::~GlExternalTexture() = default;

void* GlExternalTexture::nativeHandle() const noexcept {
  return reinterpret_cast<void*>(static_cast<uintptr_t>(mTexture));
}

std::shared_ptr<EffectExternalTextureResolver> createGlExternalTextureResolver(backend::Device& device) {
  return std::make_shared<GlExternalTextureResolver>(device);
}

} // namespace coral
