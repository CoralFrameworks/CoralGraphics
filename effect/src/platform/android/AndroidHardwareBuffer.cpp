#include <effect/platform/android/AndroidHardwareBuffer.h>


#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <vector>

namespace coral {
namespace {

struct HardwareBufferApi {
  int (*allocate)(const AHardwareBuffer_Desc*, AHardwareBuffer**) = nullptr;
  void (*describe)(const AHardwareBuffer*, AHardwareBuffer_Desc*) = nullptr;
  int (*lockPlanes)(AHardwareBuffer*, uint64_t, int32_t, const ARect*, AHardwareBuffer_Planes*) = nullptr;
  int (*lock)(AHardwareBuffer*, uint64_t, int32_t, const ARect*, void**) = nullptr;
  int (*unlock)(AHardwareBuffer*, int32_t*) = nullptr;
  void (*release)(AHardwareBuffer*) = nullptr;
  bool available = false;
};

HardwareBufferApi& apiState() {
  static HardwareBufferApi state;
  return state;
}

std::mutex& apiMutex() {
  static std::mutex mutex;
  return mutex;
}

bool loadApi(HardwareBufferApi& api) {
  void* library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
  if (library == nullptr) return false;
  api.allocate = reinterpret_cast<decltype(api.allocate)>(dlsym(library, "AHardwareBuffer_allocate"));
  api.describe = reinterpret_cast<decltype(api.describe)>(dlsym(library, "AHardwareBuffer_describe"));
  api.lockPlanes = reinterpret_cast<decltype(api.lockPlanes)>(dlsym(library, "AHardwareBuffer_lockPlanes"));
  api.lock = reinterpret_cast<decltype(api.lock)>(dlsym(library, "AHardwareBuffer_lock"));
  api.unlock = reinterpret_cast<decltype(api.unlock)>(dlsym(library, "AHardwareBuffer_unlock"));
  api.release = reinterpret_cast<decltype(api.release)>(dlsym(library, "AHardwareBuffer_release"));
  if (api.allocate == nullptr || api.lockPlanes == nullptr || api.lock == nullptr || api.unlock == nullptr ||
      api.release == nullptr) {
    api = HardwareBufferApi{};
    dlclose(library);
    return false;
  }
  api.available = true;
  return true;
}

const HardwareBufferApi* hardwareBufferApi(std::string* error) {
  HardwareBufferApi& api = apiState();
  const std::lock_guard<std::mutex> lock(apiMutex());
  if (!api.available && !loadApi(api)) {
    if (error != nullptr) *error = "AHardwareBuffer requires Android 8.0 (API 26)";
    return nullptr;
  }
  return &api;
}

struct Candidate {
  AndroidHardwareBufferFormat format;
  EffectPixelFormat pixelFormat;
  uint32_t ahbFormat;
  uint32_t bytesPerPixel;
  const char* name;
};

constexpr Candidate kCandidates[] = {
  { AndroidHardwareBufferFormat::RGBA8, EffectPixelFormat::RGBA8, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 4,
    "RGBA8" },
  { AndroidHardwareBufferFormat::RGBA16F, EffectPixelFormat::RGBA16F, AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT, 8,
    "RGBA16F" },
};

std::array<Candidate, 2> candidateOrder(AndroidHardwareBufferFormat preferred) {
  std::array<Candidate, 2> order{ kCandidates[0], kCandidates[1] };
  if (preferred == AndroidHardwareBufferFormat::RGBA16F) std::swap(order[0], order[1]);
  return order;
}

AndroidHardwareBufferLayout makeLayout(const Candidate& candidate, uint32_t width, uint32_t height,
                                       const AHardwareBuffer_Planes& planes) {
  AndroidHardwareBufferLayout layout;
  layout.format = candidate.pixelFormat;
  layout.width = width;
  layout.height = height;
  layout.planeCount = planes.planeCount;
  for (uint32_t plane = 0; plane < planes.planeCount && plane < layout.planes.size(); ++plane) {
    layout.planes[plane].rowStride = planes.planes[plane].rowStride;
    layout.planes[plane].pixelStride = planes.planes[plane].pixelStride;
    layout.planes[plane].rows = height;
  }
  // Some drivers return a single-plane descriptor with zero strides for RGBA16F. The allocation was
  // accepted, so use the minimum packed stride as the fallback; callers can still validate with GPU
  // readback and reject the allocation if a padded layout is required.
  if (layout.planeCount == 1 && layout.planes[0].rowStride == 0) {
    layout.planes[0].rowStride = width * candidate.bytesPerPixel;
    layout.planes[0].pixelStride = candidate.bytesPerPixel;
    layout.strideInferred = true;
  }
  return layout;
}

} // namespace

AndroidHardwareBufferMapping::~AndroidHardwareBufferMapping() {
  if (!mLocked || mBuffer == nullptr) return;
  if (const HardwareBufferApi* api = hardwareBufferApi(nullptr); api != nullptr) api->unlock(mBuffer, nullptr);
  mLocked = false;
}

void* AndroidHardwareBufferMapping::data(const uint32_t plane) const {
  if (!mLocked || plane >= mPlanes.planeCount || plane >= mLayout.planeCount) return nullptr;
  return mPlanes.planes[plane].data;
}

AndroidHardwareBufferAllocation::~AndroidHardwareBufferAllocation() {
  if (mBuffer == nullptr) return;
  if (const HardwareBufferApi* api = hardwareBufferApi(nullptr); api != nullptr) api->release(mBuffer);
  mBuffer = nullptr;
}

bool AndroidHardwareBufferAllocation::writePixels(const void* pixels, const uint32_t rowBytes,
        const uint32_t rowStride, std::string* error) {
  if (pixels == nullptr || mLayout.planeCount != 1) {
    if (error != nullptr) *error = "pixel upload needs a single-plane allocation";
    return false;
  }
  const uint32_t bytesPerPixel = mLayout.format == EffectPixelFormat::RGBA16F ? 8u : 4u;
  if (mLayout.format != EffectPixelFormat::RGBA8 && mLayout.format != EffectPixelFormat::RGBA16F) {
    if (error != nullptr) *error = "pixel upload supports RGBA8/RGBA16F only";
    return false;
  }
  const uint32_t packedRow = mLayout.width * bytesPerPixel;
  const uint32_t sourceRow = rowBytes == 0 ? packedRow : rowBytes;
  if (sourceRow < packedRow) {
    if (error != nullptr) *error = "pixel upload row is shorter than the packed width";
    return false;
  }
  auto mapping = lock(error);
  if (mapping == nullptr) return false;
  auto* destination = static_cast<uint8_t*>(mapping->data(0));
  if (destination == nullptr) {
    if (error != nullptr) *error = "pixel upload mapping has no plane";
    return false;
  }
  const uint32_t destinationStride = rowStride == 0 ? mLayout.planes[0].rowStride : rowStride;
  if (destinationStride < packedRow) {
    if (error != nullptr) *error = "pixel upload rowStride is shorter than the packed width";
    return false;
  }
  const auto* source = static_cast<const uint8_t*>(pixels);
  for (uint32_t y = 0; y < mLayout.height; ++y) {
    std::memcpy(destination + static_cast<size_t>(y) * destinationStride,
            source + static_cast<size_t>(y) * sourceRow, packedRow);
  }
  return true;
}

std::shared_ptr<AndroidHardwareBufferMapping> AndroidHardwareBufferAllocation::lock(std::string* error) {
  const HardwareBufferApi* api = hardwareBufferApi(error);
  if (api == nullptr || mBuffer == nullptr) return nullptr;
  auto mapping = std::shared_ptr<AndroidHardwareBufferMapping>(new AndroidHardwareBufferMapping());
  mapping->mBuffer = mBuffer;
  mapping->mLayout = mLayout;
  if (api->lockPlanes(mBuffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &mapping->mPlanes) == 0 &&
      mapping->mPlanes.planeCount != 0) {
    mapping->mLocked = true;
    return mapping;
  }
  // A single-plane RGBA allocation can still be mapped through the non-planar entry point even when
  // lockPlanes is unavailable for its format.
  if (mLayout.planeCount == 1) {
    void* base = nullptr;
    if (api->lock(mBuffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &base) == 0 &&
        base != nullptr) {
      mapping->mPlanes.planeCount = 1;
      mapping->mPlanes.planes[0].data = base;
      mapping->mPlanes.planes[0].rowStride = mLayout.planes[0].rowStride;
      mapping->mPlanes.planes[0].pixelStride = mLayout.planes[0].pixelStride;
      mapping->mLocked = true;
      return mapping;
    }
  }
  if (error != nullptr) *error = "AHardwareBuffer could not be locked";
  return nullptr;
}

std::shared_ptr<AndroidHardwareBufferAllocation> allocateAndroidHardwareBuffer(
        const uint32_t width, const uint32_t height, const AndroidHardwareBufferFormat preferred,
        std::string* error) {
  if (width == 0 || height == 0) {
    if (error != nullptr) *error = "AHardwareBuffer needs a non-zero size";
    return nullptr;
  }
  const HardwareBufferApi* api = hardwareBufferApi(error);
  if (api == nullptr) return nullptr;

  const std::array<Candidate, 2> order = candidateOrder(preferred);
  for (const Candidate& candidate : order) {
    AHardwareBuffer_Desc description{};
    description.width = width;
    description.height = height;
    description.layers = 1;
    description.format = candidate.ahbFormat;
    description.usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    AHardwareBuffer* buffer = nullptr;
    if (api->allocate(&description, &buffer) != 0 || buffer == nullptr) continue;

    AHardwareBuffer_Planes planes{};
    if (api->lockPlanes(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &planes) != 0 ||
        planes.planeCount == 0) {
      api->release(buffer);
      continue;
    }
    const AndroidHardwareBufferLayout layout = makeLayout(candidate, width, height, planes);
    api->unlock(buffer, nullptr);

    auto allocation = std::shared_ptr<AndroidHardwareBufferAllocation>(new AndroidHardwareBufferAllocation());
    allocation->mBuffer = buffer;
    allocation->mLayout = layout;
    return allocation;
  }
  if (error != nullptr) *error = "no standard AHardwareBuffer format could be allocated";
  return nullptr;
}

std::shared_ptr<AndroidHardwareBufferAllocation> allocateAndroidHardwareBufferForGpu(
        const uint32_t width, const uint32_t height, const AndroidHardwareBufferFormat preferred,
        std::string* error) {
  if (width == 0 || height == 0) {
    if (error != nullptr) *error = "AHardwareBuffer needs a non-zero size";
    return nullptr;
  }
  const HardwareBufferApi* api = hardwareBufferApi(error);
  if (api == nullptr) return nullptr;

  const std::array<Candidate, 2> order = candidateOrder(preferred);
  for (const Candidate& candidate : order) {
    AHardwareBuffer_Desc description{};
    description.width = width;
    description.height = height;
    description.layers = 1;
    description.format = candidate.ahbFormat;
    description.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    AHardwareBuffer* buffer = nullptr;
    if (api->allocate(&description, &buffer) != 0 || buffer == nullptr) continue;

    AHardwareBuffer_Planes planes{};
    planes.planeCount = 1;
    planes.planes[0].rowStride = width * candidate.bytesPerPixel;
    planes.planes[0].pixelStride = candidate.bytesPerPixel;
    if (api->describe != nullptr) {
      AHardwareBuffer_Desc actual{};
      api->describe(buffer, &actual);
      if (actual.stride != 0) planes.planes[0].rowStride = actual.stride * candidate.bytesPerPixel;
    }
    auto allocation = std::shared_ptr<AndroidHardwareBufferAllocation>(new AndroidHardwareBufferAllocation());
    allocation->mBuffer = buffer;
    allocation->mLayout = makeLayout(candidate, width, height, planes);
    return allocation;
  }
  if (error != nullptr) *error = "no GPU AHardwareBuffer format could be allocated";
  return nullptr;
}

} // namespace coral
