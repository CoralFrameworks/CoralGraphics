#ifndef CORAL_ANDROID_HARDWARE_BUFFER_H
#define CORAL_ANDROID_HARDWARE_BUFFER_H

#include <effect/EffectTypes.h>

#include <android/hardware_buffer.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coral {

/** A standard AHardwareBuffer format the engine can import. Vendor-private formats are not listed. */
enum class AndroidHardwareBufferFormat : uint8_t {
  /** Probe RGBA8, then RGBA16F. */
  Auto,
  RGBA8,
  RGBA16F,
};

struct AndroidHardwareBufferPlane {
  uint32_t rowStride = 0;
  uint32_t pixelStride = 0;
  uint32_t rows = 0;
};

/**
 * The layout a driver reported for an allocation. `planes` is always filled from
 * `AHardwareBuffer_lockPlanes` when that reports usable strides; a single-plane RGBA buffer whose
 * driver returns zero strides falls back to the minimum packed stride for its format.
 */
struct AndroidHardwareBufferLayout {
  EffectPixelFormat format = EffectPixelFormat::RGBA8;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t planeCount = 0;
  /** True when a zero driver stride was replaced by the minimum packed stride. */
  bool strideInferred = false;
  std::array<AndroidHardwareBufferPlane, 4> planes{};
};

/** A locked CPU mapping. Unlocks when destroyed; the Allocation must outlive it. */
class AndroidHardwareBufferMapping {
public:
  ~AndroidHardwareBufferMapping();
  AndroidHardwareBufferMapping(const AndroidHardwareBufferMapping&) = delete;
  AndroidHardwareBufferMapping& operator=(const AndroidHardwareBufferMapping&) = delete;

  [[nodiscard]] void* data(uint32_t plane) const;
  [[nodiscard]] const AndroidHardwareBufferLayout& layout() const noexcept { return mLayout; }

private:
  friend class AndroidHardwareBufferAllocation;
  AndroidHardwareBufferMapping() = default;

  AHardwareBuffer* mBuffer = nullptr;
  AHardwareBuffer_Planes mPlanes{};
  AndroidHardwareBufferLayout mLayout;
  bool mLocked = false;
};

/**
 * An AHardwareBuffer allocated after a runtime capability probe. The probe is the allocation itself:
 * a candidate is used only after the driver allocates it and exposes a lockable layout.
 */
class AndroidHardwareBufferAllocation {
public:
  ~AndroidHardwareBufferAllocation();
  AndroidHardwareBufferAllocation(const AndroidHardwareBufferAllocation&) = delete;
  AndroidHardwareBufferAllocation& operator=(const AndroidHardwareBufferAllocation&) = delete;

  [[nodiscard]] AHardwareBuffer* buffer() const noexcept { return mBuffer; }
  [[nodiscard]] const AndroidHardwareBufferLayout& layout() const noexcept { return mLayout; }

  /** Locks the buffer for CPU write and returns the driver-provided plane layout. */
  [[nodiscard]] std::shared_ptr<AndroidHardwareBufferMapping> lock(std::string* error = nullptr);

  /**
   * Copies packed RGBA8/RGBA16F pixels into the single-plane allocation. `rowStride` overrides the
   * layout stride for stride probing; pass 0 to use the allocation layout.
   */
  bool writePixels(const void* pixels, uint32_t rowBytes, uint32_t rowStride = 0, std::string* error = nullptr);

private:
  friend std::shared_ptr<AndroidHardwareBufferAllocation> allocateAndroidHardwareBuffer(
          uint32_t width, uint32_t height, AndroidHardwareBufferFormat preferred, std::string* error);
  friend std::shared_ptr<AndroidHardwareBufferAllocation> allocateAndroidHardwareBufferForGpu(
          uint32_t width, uint32_t height, AndroidHardwareBufferFormat preferred, std::string* error);
  AndroidHardwareBufferAllocation() = default;

  AHardwareBuffer* mBuffer = nullptr;
  AndroidHardwareBufferLayout mLayout;
};

/**
 * Allocates a GPU-sampled AHardwareBuffer using only standard formats. `preferred` is tried first;
 * the remaining format follows as fallback. The returned allocation owns one AHardwareBuffer reference.
 */
std::shared_ptr<AndroidHardwareBufferAllocation> allocateAndroidHardwareBuffer(
        uint32_t width, uint32_t height, AndroidHardwareBufferFormat preferred, std::string* error = nullptr);

/**
 * Allocates a GPU-only AHardwareBuffer that is a render/copy destination and can be sampled by GL.
 *
 * Unlike {@link allocateAndroidHardwareBuffer} this does not lock the buffer for CPU access: the
 * usage is `GPU_SAMPLED_IMAGE | GPU_FRAMEBUFFER`, which is what a Vulkan writeback and the EGL
 * consumer of an encoder output need.
 */
std::shared_ptr<AndroidHardwareBufferAllocation> allocateAndroidHardwareBufferForGpu(
        uint32_t width, uint32_t height, AndroidHardwareBufferFormat preferred, std::string* error = nullptr);

} // namespace coral

#endif // CORAL_ANDROID_HARDWARE_BUFFER_H
