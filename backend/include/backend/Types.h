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

#ifndef CORAL_BACKEND_TYPES_H
#define CORAL_BACKEND_TYPES_H

#include <utils/compiler.h>

#include <cstddef>
#include <cstdint>

namespace backend {

/**
 * Result codes returned by every operation that can fail. The vocabulary deliberately mirrors
 * VkResult so that the Vulkan backend can translate one-to-one, while the OpenGL backend
 * synthesises the same codes from `glGetError` and friends.
 */
enum class Result : int32_t {
  /** The operation completed successfully. */
  Success = 0,
  /** The operation is still in flight (used by polling APIs such as waitForFence). */
  NotReady,
  /** The operation timed out. */
  Timeout,
  /** The operation completed but the surface is out of date and must be rebuilt. */
  OutOfDate,
  /** A generic, unspecified failure. */
  Error,
  /** The caller passed a parameter that is invalid, out of range, or mutually inconsistent. */
  InvalidArgument,
  /** A handle referred to a resource that has already been destroyed or was never created. */
  InvalidHandle,
  /** The requested feature is not available on this device. */
  Unsupported,
  /** Host or device memory could not be allocated. */
  OutOfMemory,
  /** The device was lost (GPU reset, driver crash, context destruction). */
  DeviceLost,
  /** The shader module could not be compiled or reflected. */
  ShaderCompilationFailed,
  /** An operation was attempted from a thread that does not own the resource. */
  WrongThread,
};

/** @return true when @p result represents successful completion. */
[[nodiscard]] constexpr bool isSuccess(const Result result) noexcept { return result == Result::Success; }

/** @return true when @p result represents a recoverable "try again later" condition. */
[[nodiscard]] constexpr bool isTransient(const Result result) noexcept {
  return result == Result::NotReady || result == Result::Timeout || result == Result::OutOfDate;
}

/**
 * Identifies the graphics API a backend drives. Capability decisions, shader dialects and
 * synchronisation rules all key off this value.
 *
 * There is one OpenGL backend rather than two, because OpenGL ES and desktop OpenGL are the same API
 * at different capability levels. The backend selects its dialect from the platform it was built
 * for: `getPlatformGlDialect()` reports which one this build targets, and `DeviceInfo::backendName`
 * reports which one a device actually came up on.
 */
enum class Backend : uint8_t {
  Null,
  /** Desktop OpenGL 4.1+ on Windows, Linux and macOS; OpenGL ES 3.0+ on Android and iOS. */
  OpenGL,
  /** Available on Windows, Linux and Android. */
  Vulkan,
  /** Only available on MacOS and iOS. */
  Metal,
};

/** Human readable name of @p backend, never null. */
[[nodiscard]] const char* toString(Backend backend) noexcept;

/**
 * Which flavour of OpenGL this build targets.
 *
 * The two are not interchangeable: they need different shader versions, different capability
 * baselines and different context creation. Everything that differs is keyed off this value rather
 * than off preprocessor conditionals scattered through the backend.
 */
enum class GlDialect : uint8_t {
  /**
   * OpenGL ES 3.0 or newer. Android and iOS.
   *
   * Also the default value of `DeviceConfiguration::glDialect`, and therefore of every
   * value-initialised configuration. It is *not* the default *request*: device creation resolves this
   * to the platform's own dialect unless the caller asked for something else. See
   * `DeviceConfiguration::glDialect`.
   */
  ES,
  /** Desktop OpenGL 4.1 or newer, core profile. Windows, Linux and macOS. */
  Desktop,
};

/** @return the OpenGL dialect this build targets, decided at compile time from the platform. */
[[nodiscard]] GlDialect getPlatformGlDialect() noexcept;

/** @return "OpenGL ES 3.0+" or "OpenGL 4.1+", for logging and diagnostics. */
[[nodiscard]] const char* getPlatformGlDialectName() noexcept;

/** Human readable name of @p dialect, never null. */
[[nodiscard]] const char* toString(GlDialect dialect) noexcept;

// ==================================================================================================
// Pixel formats
// ==================================================================================================

/**
 * Fully typed texture/buffer pixel format. Named parts are optional, so an app can probe whether
 * it really needs alpha by asking for R8G8B8 instead of R8G8B8A8.
 *
 * The trailing "Srgb" suffix marks formats whose sampling hardware performs the sRGB->linear
 * conversion automatically; the corresponding *_UNORM format has no such conversion.
 */
enum class PixelFormat : uint16_t {
  Undefined = 0,

  // 8 bit
  R8_UNORM,
  R8_SNORM,
  R8_UINT,
  R8_SINT,

  // 16 bit, two channel
  R8G8_UNORM,
  R8G8_SNORM,
  R8G8_UINT,
  R8G8_SINT,

  // 16 bit, single or multi channel
  R16_UNORM,
  R16_SNORM,
  R16_UINT,
  R16_SINT,
  R16_SFLOAT,

  // 24 bit
  R8G8B8_UNORM,
  R8G8B8_SNORM,
  R8G8B8_SRGB,
  B8G8R8_UNORM,
  B8G8R8_SRGB,

  // 32 bit
  R8G8B8A8_UNORM,
  R8G8B8A8_SNORM,
  R8G8B8A8_SRGB,
  R8G8B8A8_UINT,
  R8G8B8A8_SINT,
  B8G8R8A8_UNORM,
  B8G8R8A8_SRGB,
  R16G16_UNORM,
  R16G16_SNORM,
  R16G16_UINT,
  R16G16_SINT,
  R16G16_SFLOAT,
  R32_UINT,
  R32_SINT,
  R32_SFLOAT,
  R10G10B10A2_UNORM,
  R11G11B10_SFLOAT,

  // 64 bit
  R16G16B16A16_UNORM,
  R16G16B16A16_SNORM,
  R16G16B16A16_UINT,
  R16G16B16A16_SINT,
  R16G16B16A16_SFLOAT,
  R32G32_UINT,
  R32G32_SINT,
  R32G32_SFLOAT,

  // 96 bit
  R32G32B32_UINT,
  R32G32B32_SINT,
  R32G32B32_SFLOAT,

  // 128 bit
  R32G32B32A32_UINT,
  R32G32B32A32_SINT,
  R32G32B32A32_SFLOAT,

  // Depth / stencil
  D16_UNORM,
  D24_UNORM_S8_UINT,
  D32_SFLOAT,
  D32_SFLOAT_S8_UINT,

  // Compressed
  BC1_RGBA_UNORM,
  BC1_RGBA_SRGB,
  BC3_RGBA_UNORM,
  BC3_RGBA_SRGB,
  BC4_R_UNORM,
  BC5_RG_UNORM,
  BC7_RGBA_UNORM,
  BC7_RGBA_SRGB,
  ETC2_RGB8_UNORM,
  ETC2_RGB8_SRGB,
  ETC2_RGBA8_UNORM,
  ETC2_RGBA8_SRGB,
  ASTC_4x4_UNORM,
  ASTC_4x4_SRGB,
  ASTC_8x8_UNORM,
  ASTC_8x8_SRGB,
};

/** Static description of a PixelFormat, independent of any device. */
struct PixelFormatInfo {
  /** Bytes per block, or 0 when the format is Undefined. */
  uint8_t bytesPerBlock = 0;
  /** Width (and height, for formats that are square) of a block, 1 for uncompressed formats. */
  uint8_t blockWidth = 1;
  uint8_t blockHeight = 1;
  uint8_t channelCount = 0;
  /** True for depth and depth/stencil formats. */
  bool depth = false;
  bool stencil = false;
  /** True when all channels are stored as floating point or normalised integers. */
  bool filterable = false;
  /** True when the format carries an sRGB transfer function. */
  bool srgb = false;
  /** True when the format can be a colour attachment. */
  bool renderable = false;
  const char* name = "Undefined";
};

/** @return the compile-time description of @p format. Never null. */
[[nodiscard]] const PixelFormatInfo& getPixelFormatInfo(PixelFormat format) noexcept;

/** @return true when @p format is a compressed block format. */
[[nodiscard]] bool isCompressedFormat(PixelFormat format) noexcept;

/**
 * Computes the number of bytes needed for a @p width x @p height x @p depth region of
 * @p format, honouring block compression. Returns 0 for Undefined.
 */
[[nodiscard]] size_t getFormatByteSize(PixelFormat format, uint32_t width, uint32_t height,
        uint32_t depth = 1) noexcept;

// ==================================================================================================
// Resource enums
// ==================================================================================================

enum class TextureType : uint8_t {
  Texture1D,
  Texture2D,
  Texture3D,
  TextureCube,
  Texture2DArray,
  TextureCubeArray,
};

/** How a texture is going to be used. Backends validate this against the device capabilities. */
enum class TextureUsage : uint8_t {
  Sampled = 1 << 0,
  Storage = 1 << 1,
  ColorAttachment = 1 << 2,
  DepthStencilAttachment = 1 << 3,
  /** Data is uploaded once and never changed again; enables immutable storage on some backends. */
  Immutable = 1 << 4,
  /** The texture is shared with an external consumer (video decoder, camera, other context). */
  External = 1 << 5,
};
using TextureUsages = uint8_t;

[[nodiscard]] constexpr bool hasUsage(const TextureUsage value, const TextureUsage test) noexcept {
  return (static_cast<uint8_t>(value) & static_cast<uint8_t>(test)) != 0;
}

[[nodiscard]] constexpr bool hasUsage(const TextureUsages values, const TextureUsage test) noexcept {
  return (values & static_cast<uint8_t>(test)) != 0;
}

/**
 * Combines texture usage flags.
 *
 * Declared as an operator rather than leaving callers to write `static_cast<TextureUsages>(a | b)`
 * because the cast is easy to forget and the resulting expression silently produces an int, which
 * then fails to compare against a TextureUsages parameter somewhere far from the mistake.
 */
[[nodiscard]] constexpr TextureUsages operator|(const TextureUsage a, const TextureUsage b) noexcept {
  return static_cast<TextureUsages>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

[[nodiscard]] constexpr TextureUsages operator|(const TextureUsages a, const TextureUsage b) noexcept {
  return static_cast<TextureUsages>(a | static_cast<uint8_t>(b));
}

[[nodiscard]] constexpr TextureUsages operator|(const TextureUsage a, const TextureUsages b) noexcept { return b | a; }

enum class BufferUsage : uint8_t {
  Vertex = 1 << 0,
  Index = 1 << 1,
  Uniform = 1 << 2,
  Storage = 1 << 3,
  Indirect = 1 << 4,
  /** Source or destination of a transfer (copyBufferToTexture and friends). */
  TransferSrc = 1 << 5,
  TransferDst = 1 << 6,
};
using BufferUsages = uint8_t;

[[nodiscard]] constexpr bool hasUsage(const BufferUsage value, const BufferUsage test) noexcept {
  return (static_cast<uint8_t>(value) & static_cast<uint8_t>(test)) != 0;
}

[[nodiscard]] constexpr bool hasUsage(const BufferUsages values, const BufferUsage test) noexcept {
  return (values & static_cast<uint8_t>(test)) != 0;
}

/** Combines buffer usage flags. @see operator| for TextureUsage. */
[[nodiscard]] constexpr BufferUsages operator|(const BufferUsage a, const BufferUsage b) noexcept {
  return static_cast<BufferUsages>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

[[nodiscard]] constexpr BufferUsages operator|(const BufferUsages a, const BufferUsage b) noexcept {
  return static_cast<BufferUsages>(a | static_cast<uint8_t>(b));
}

[[nodiscard]] constexpr BufferUsages operator|(const BufferUsage a, const BufferUsages b) noexcept { return b | a; }

/** How the host intends to touch a buffer's memory after creation. */
enum class MemoryUsage : uint8_t {
  /** Device local only. Cheapest for GPU-only data. */
  GpuOnly,
  /** Host visible, written once, read many times by the GPU. */
  CpuToGpu,
  /** Host visible, written by the GPU and read back by the host. */
  GpuToCpu,
  /** Host visible and updated repeatedly; a backend may choose to double buffer. */
  CpuToGpuDynamic,
};

enum class IndexFormat : uint8_t {
  UInt16,
  UInt32,
};

enum class ShaderStage : uint8_t {
  Vertex,
  Fragment,
  Compute,
  Geometry,
  TessellationControl,
  TessellationEvaluation,
};

[[nodiscard]] const char* toString(ShaderStage stage) noexcept;

/**
 * Which shading language a source module is written in.
 *
 * The distinction matters at compile time, not only in prose. Each value selects a different glslang
 * profile, and the profiles disagree about things that look universal: `attribute`, `varying`,
 * `gl_FragColor` and `texture2D` are legal in ESSL 1.00 and removed in ESSL 3.00, while
 * `layout(location = ...)` and `in`/`out` are the reverse. Handing a 1.00 shader to the 3.00 profile
 * produces a wall of errors about identifiers the author used correctly.
 *
 * `Detect` exists because the language is normally already stated by the file itself: every GLSL
 * source begins with `#version`, and its absence means ESSL 1.00. A caller that knows better can say
 * so explicitly.
 */
enum class ShaderSourceLanguage : uint8_t {
  /** Infer from the source's `#version` directive. A source without one is ESSL 1.00. */
  Detect,
  /** OpenGL ES Shading Language 1.00, the `#version 100` profile. */
  ESSL100,
  /** OpenGL ES Shading Language 3.00 and newer, the `#version 300 es` profile. */
  ESSL300,
  /** Desktop GLSL, the `#version 410 core` profile. */
  GLSL410,
};

/** Human readable name of @p language, never null. */
[[nodiscard]] const char* toString(ShaderSourceLanguage language) noexcept;

// ==================================================================================================
// Pipeline state enums
// ==================================================================================================

enum class PrimitiveTopology : uint8_t {
  PointList,
  LineList,
  LineStrip,
  TriangleList,
  TriangleStrip,
  TriangleFan,
  /** Requires Capabilities::geometryShader or Capabilities::tessellationShader. */
  PatchList,
};

enum class CullMode : uint8_t {
  None,
  Front,
  Back,
};

enum class FrontFace : uint8_t {
  CounterClockwise,
  Clockwise,
};

enum class CompareOp : uint8_t {
  Never,
  Less,
  Equal,
  LessEqual,
  Greater,
  NotEqual,
  GreaterEqual,
  Always,
};

enum class StencilOp : uint8_t {
  Keep,
  Zero,
  Replace,
  IncrementClamp,
  DecrementClamp,
  Invert,
  IncrementWrap,
  DecrementWrap,
};

enum class BlendFactor : uint8_t {
  Zero,
  One,
  SrcColor,
  OneMinusSrcColor,
  DstColor,
  OneMinusDstColor,
  SrcAlpha,
  OneMinusSrcAlpha,
  DstAlpha,
  OneMinusDstAlpha,
  ConstantColor,
  OneMinusConstantColor,
  SrcAlphaSaturate,
  Src1Color,
  OneMinusSrc1Color,
  Src1Alpha,
  OneMinusSrc1Alpha,
};

enum class BlendOp : uint8_t {
  Add,
  Subtract,
  ReverseSubtract,
  Min,
  Max,
};

enum class LoadOp : uint8_t {
  /** Keep the previous attachment contents; requires the attachment to be writeable. */
  Load,
  /** Clear to the value given in the RenderPassBeginInfo. */
  Clear,
  /** The previous contents are undefined; this is the fastest option. */
  DontCare,
};

enum class StoreOp : uint8_t {
  Store,
  DontCare,
};

enum class VertexFormat : uint8_t {
  Invalid,
  Float,  // 1 x 32 bit
  Float2, // 2 x 32 bit
  Float3,
  Float4,
  UInt,
  UInt2,
  UInt3,
  UInt4,
  SInt,
  SInt2,
  SInt3,
  SInt4,
  /** 4 x 8 bit normalised unsigned */
  UByte4Norm,
  UByte4,
  /** 4 x 8 bit normalised signed */
  Byte4Norm,
  UByte2Norm,
  Short2Norm,
  UShort2Norm,
  Half2,
  Half4,
  /** 10/10/10/2 packed unsigned normalised */
  UInt1010102Norm,
};

/** Byte size of a single vertex of @p format, or 0 when Invalid. */
[[nodiscard]] uint8_t getVertexFormatByteSize(VertexFormat format) noexcept;

/** Number of components of @p format, or 0 when Invalid. */
[[nodiscard]] uint8_t getVertexFormatComponentCount(VertexFormat format) noexcept;

[[nodiscard]] bool isIntegerVertexFormat(VertexFormat format) noexcept;

enum class VertexStepMode : uint8_t {
  /** Advance once per vertex. */
  Vertex,
  /** Advance once per instance. */
  Instance,
};

enum class TextureViewDimension : uint8_t {
  Texture1D,
  Texture2D,
  Texture2DArray,
  Texture3D,
  Cube,
  CubeArray,
};

enum class SamplerAddressMode : uint8_t {
  ClampToEdge,
  Repeat,
  MirroredRepeat,
  ClampToBorder,
};

enum class FilterMode : uint8_t {
  Nearest,
  Linear,
};

/** How a mip level in between two sampled levels is produced. */
enum class MipmapFilterMode : uint8_t {
  Nearest,
  Linear,
};

enum class SamplerBorderColor : uint8_t {
  TransparentBlack,
  OpaqueBlack,
  OpaqueWhite,
};

enum class FilterType : uint8_t {
  Comparison,
  NonComparison,
};

// ==================================================================================================
// YUV sampling
//
// A YUV image is not sampled channel by channel: the backend's sampler converts it to RGB, using the
// coefficients, the range and the chroma siting named below. Vulkan does this in a
// `VkSamplerYcbcrConversion`; OpenGL has no equivalent on this backend and refuses a sampler that asks for
// one, because a GL shader would have to do the conversion itself.
// ==================================================================================================

/** Which set of luma/chroma coefficients turns the image's YCbCr into RGB. */
enum class YcbcrModelConversion : uint8_t {
  /** ITU-R BT.601, the coefficients of standard-definition video. */
  Bt601,
  /** ITU-R BT.709, the coefficients of high-definition video. */
  Bt709,
  /** ITU-R BT.2020, the coefficients of UHD/HDR video. */
  Bt2020,
  /** The image is already RGB and is not converted; the sampler must not do any matrix work. */
  RgbIdentity,
};

/** Whether the image's codes use the full 0..255 range or the video (16..235) range. */
enum class YcbcrRange : uint8_t {
  /** Video range: luma 16..235, chroma 16..240. */
  Narrow,
  /** Full range: 0..255 for both. */
  Full,
};

/** Where the chroma sample of a 4:2:0 pair sits relative to the two luma samples it belongs to. */
enum class ChromaLocation : uint8_t {
  CositedEven,
  Midpoint,
};

/**
 * How a YUV image is converted to RGB when it is sampled.
 *
 * The conversion is a property of the *image* as much as of the sampler: an image imported with an external
 * format can only be read through a view and a sampler that both carry this, which is why it appears on the
 * texture view, on the sampler, and on the import that creates the image.
 */
struct YcbcrConversionDescriptor {
  YcbcrModelConversion model = YcbcrModelConversion::Bt601;
  YcbcrRange range = YcbcrRange::Narrow;
  ChromaLocation xChromaOffset = ChromaLocation::CositedEven;
  ChromaLocation yChromaOffset = ChromaLocation::CositedEven;
  /**
   * The driver's opaque external format for the image, as reported when it was imported, or 0 when the
   * image has a real pixel format and the conversion only describes how to read its channels.
   */
  uint64_t externalFormat = 0;
  /** Forces the sampler to reconstruct rather than rely on the driver's implicit filtering. Rarely on. */
  bool forceExplicitReconstruction = false;

  friend constexpr bool operator==(const YcbcrConversionDescriptor& a,
          const YcbcrConversionDescriptor& b) noexcept {
    return a.model == b.model && a.range == b.range && a.xChromaOffset == b.xChromaOffset &&
           a.yChromaOffset == b.yChromaOffset && a.externalFormat == b.externalFormat &&
           a.forceExplicitReconstruction == b.forceExplicitReconstruction;
  }
  friend constexpr bool operator!=(const YcbcrConversionDescriptor& a,
          const YcbcrConversionDescriptor& b) noexcept {
    return !(a == b);
  }
};

// ==================================================================================================
// Extent / viewport / scissors
// ==================================================================================================

struct Extent2D {
  uint32_t width = 0;
  uint32_t height = 0;

  friend constexpr bool operator==(const Extent2D a, const Extent2D b) noexcept {
    return a.width == b.width && a.height == b.height;
  }
  friend constexpr bool operator!=(const Extent2D a, const Extent2D b) noexcept { return !(a == b); }
};

struct Extent3D {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depthOrArrayLayers = 1;

  friend constexpr bool operator==(const Extent3D a, const Extent3D b) noexcept {
    return a.width == b.width && a.height == b.height && a.depthOrArrayLayers == b.depthOrArrayLayers;
  }
};

struct Offset2D {
  int32_t x = 0;
  int32_t y = 0;
};

struct Offset3D {
  int32_t x = 0;
  int32_t y = 0;
  int32_t z = 0;
};

struct Origin3D {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t z = 0;
};

struct Viewport {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
  float minDepth = 0.0f;
  float maxDepth = 1.0f;
};

struct ScissorRect {
  int32_t x = 0;
  int32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

struct Color {
  double r = 0.0;
  double g = 0.0;
  double b = 0.0;
  double a = 1.0;

  constexpr Color() noexcept = default;
  constexpr Color(const double red, const double green, const double blue, const double alpha = 1.0) noexcept
          : r(red),
            g(green),
            b(blue),
            a(alpha) {}

  static constexpr Color black() noexcept { return { 0.0, 0.0, 0.0, 1.0 }; }
  static constexpr Color transparentBlack() noexcept { return { 0.0, 0.0, 0.0, 0.0 }; }
};

struct DepthStencilValue {
  float depth = 1.0f;
  uint32_t stencil = 0;
};

// ==================================================================================================
// Sub-resource addressing
// ==================================================================================================

/**
 * Identifies a region inside a multi-mip / multi-layer texture. The same struct is used for texture
 * views during creation and for copy / clear operations. Following Vulkan, an all-zero
 * mipLevelCount / arrayLayerCount means "as many as remain from the given base".
 */
struct TextureSubresourceRange {
  uint32_t baseMipLevel = 0;
  uint32_t mipLevelCount = 0;
  uint32_t baseArrayLayer = 0;
  uint32_t arrayLayerCount = 0;

  /** A range spanning the entire texture, given its mip and layer counts. */
  static constexpr TextureSubresourceRange all(const uint32_t mipCount = 0, const uint32_t layerCount = 0) noexcept {
    TextureSubresourceRange range;
    range.mipLevelCount = mipCount;
    range.arrayLayerCount = layerCount;
    return range;
  }

  friend constexpr bool operator==(const TextureSubresourceRange& a, const TextureSubresourceRange& b) noexcept {
    return a.baseMipLevel == b.baseMipLevel && a.mipLevelCount == b.mipLevelCount &&
           a.baseArrayLayer == b.baseArrayLayer && a.arrayLayerCount == b.arrayLayerCount;
  }
};

struct TextureSubresourceLayers {
  uint32_t mipLevel = 0;
  uint32_t baseArrayLayer = 0;
  uint32_t arrayLayerCount = 1;

  static constexpr TextureSubresourceLayers all(const uint32_t layerCount) noexcept {
    return { .mipLevel = 0, .baseArrayLayer = 0, .arrayLayerCount = layerCount };
  }
};

/** A copy region for buffer <-> texture transfers, in texels. */
struct TextureCopyRegion {
  Origin3D textureOrigin = {};
  Offset3D bufferOffset = {}; // in bytes for bufferTextureCopy
  Extent3D size = {};
  uint32_t mipLevel = 0;
  uint32_t baseArrayLayer = 0;
  uint32_t arrayLayerCount = 1;
  uint32_t bytesPerRow = 0;  // 0 == tightly packed
  uint32_t rowsPerImage = 0; // 0 == tightly packed
};

struct BufferCopyRegion {
  uint64_t srcOffset = 0;
  uint64_t dstOffset = 0;
  uint64_t size = 0;
};

// BufferTextureCopy lives in Descriptors.h rather than here: it names both a BufferHandle and a
// TextureHandle, and this header is a dependency of the header that defines those handles.

// ==================================================================================================
// Format capabilities
// ==================================================================================================

/**
 * Combines the device-level support flags for a single PixelFormat. This is the mechanism by which
 * upper layers implement graceful degradation: ask for a feature, fall back when it is absent.
 *
 * @see Capabilities::queryTextureFormat
 */
struct TextureFormatCapabilities {
  /** The format can be sampled from a shader (with a sampler). */
  bool sampled = false;
  /** The format can be used as a colour or depth/stencil attachment. */
  bool renderable = false;
  /** The format can be written from a shader (image store). */
  bool storage = false;
  /** The format can be used as a blend source/target. */
  bool blendable = false;
  /** Multisample resolve is supported for this format. */
  bool multisampleResolve = false;
  /** Number of samples, per sample count, that are supported for this format. */
  bool sampleCounts[8] = {};

  /** @return true when every flag is set, i.e. the format is fully usable. */
  [[nodiscard]] bool fullySupported() const noexcept { return sampled && renderable && blendable; }

  [[nodiscard]] bool supportsSampleCount(const uint32_t count) const noexcept {
    return count < 8 && sampleCounts[count];
  }
};

// ==================================================================================================
// Limits and device identification
// ==================================================================================================

/**
 * Hard resource limits. Values are always populated - a backend that cannot report a real limit
 * must fill in a conservative default rather than leave zero, so that upper layers can size their
 * pools without additional branching.
 */
struct Limits {
  uint32_t maxTextureDimension1D = 4096;
  uint32_t maxTextureDimension2D = 4096;
  uint32_t maxTextureDimension3D = 256;
  uint32_t maxTextureDimensionCube = 4096;
  uint32_t maxTextureArrayLayers = 256;

  uint32_t maxVertexBuffers = 8;
  uint32_t maxVertexAttributes = 16;
  uint32_t maxVertexBufferStride = 2048;
  uint32_t maxBufferSize = 256u * 1024u * 1024u;
  uint32_t maxUniformBufferBindingSize = 16u * 1024u;
  uint32_t maxStorageBufferBindingSize = 128u * 1024u * 1024u;

  /** Uniform buffer bindings per shader stage. */
  uint32_t maxUniformBuffersPerStage = 12;
  uint32_t maxStorageBuffersPerStage = 0;
  uint32_t maxSampledTexturesPerStage = 16;
  uint32_t maxStorageTexturesPerStage = 0;
  uint32_t maxSamplersPerStage = 16;

  /** Descriptor sets that may be bound simultaneously. */
  uint32_t maxBoundDescriptorSets = 4;

  /**
   * Bytes of push-constant data available per pipeline layout. 0 means the backend has no native
   * push constants; backends that emulate them (OpenGL ES) still report a budget, because from the
   * upper layer's point of view an emulated push constant is indistinguishable from a real one.
   *
   * ## What the OpenGL backend's budget does and does not mean
   *
   * It reports 256, and a caller should read that as "an emulated block of up to 256 bytes will be filled and
   * bound for every draw that recorded push constants" -- which is what the emulation now does. It does
   * **not** mean a shader can be *written* against it in GLSL: `layout(push_constant)` is Vulkan's spelling,
   * and glslang refuses it for the OpenGL target environment at every version
   * (`'push_constant' : only allowed when using GLSL for Vulkan`, measured by `tools/probes/effect_orient_probe.exe`).
   * A module only carries a push-constant block if it was compiled by a Vulkan-environment compiler -- the
   * offline `shader_compiler`, or a tool of the caller's -- and `Device::createShaderModule` is then given
   * the words rather than the source.
   *
   * A caller that wants per-draw constants from GLSL source should use a small uniform block, which the
   * backend binds from a buffer range; that is what the effect framework does, and it is portable to every
   * backend rather than to the ones with push constants.
   */
  uint32_t maxPushConstantSize = 128;

  uint32_t maxColorAttachments = 4;
  uint32_t maxComputeWorkgroupSizeX = 128;
  uint32_t maxComputeWorkgroupSizeY = 128;
  uint32_t maxComputeWorkgroupSizeZ = 64;
  uint32_t maxComputeInvocationsPerWorkgroup = 128;
  uint32_t maxComputeWorkgroupCountX = 65535;
  uint32_t maxComputeWorkgroupCountY = 65535;
  uint32_t maxComputeWorkgroupCountZ = 65535;

  /** Number of frames that may be recorded ahead of the GPU. 1 means "fully serialised". */
  uint32_t maxFramesInFlight = 1;
};

struct DeviceInfo {  Backend backend = Backend::Null;
  /** Vendor name as reported by the driver; may be empty but never null. */
  const char* vendor = "";
  const char* deviceName = "";
  const char* driverVersion = "";

  /** The API version the backend was created against, e.g. "OpenGL ES 3.0". */
  const char* apiVersion = "";

  /** True when the backend compiles shaders from SPIR-V at runtime. */
  bool acceptsSpirv = false;

  /**
   * True when the backend hands GLSL source to its driver unchanged, with no SPIR-V in between.
   *
   * The mirror of `acceptsSpirv` for the other end of the pipeline: a caller holding GLSL that is
   * already written for this backend's dialect can skip the compiler on a device that reports this.
   */
  bool acceptsGlslDirect = false;

  /** Human readable description of the backend implementation, e.g. "coral/backend/gl". */
  const char* backendName = "";
};

/**
 * Device memory a backend currently has **live**, in bytes.
 *
 * Counted at the resource tables: a texture or buffer adds its allocation when the table takes it and
 * subtracts it when the table releases it, so the number is residency rather than bandwidth. Transient
 * staging buffers a backend creates internally are not in a table and are therefore not counted; the
 * number is what a frame keeps, which is what a VRAM budget is about.
 */
struct DeviceMemoryStatistics {
  uint64_t textureBytes = 0;
  uint64_t bufferBytes = 0;

  [[nodiscard]] uint64_t totalBytes() const noexcept { return textureBytes + bufferBytes; }
};

/**
 * Which native window system a surface's handles belong to.
 *
 * Linux has two at once: X11 and Wayland. Both carry a `void*` window and a `void*` display, so a
 * backend cannot tell them apart from the handles alone. Naming the system once is also what lets a
 * future Vulkan surface choose `VK_KHR_xlib_surface` or `VK_KHR_wayland_surface` without guessing.
 *
 * `Unknown` keeps the historical behaviour: an X11/GLX window on Linux, the platform's own window
 * system elsewhere.
 *
 * It lives here rather than in Device.h so that the facade headers (`gpu/Context.h`,
 * `gpu/WindowSurface.h`) can carry a field of this type without pulling in Device.h -- and with it the
 * opaque EGL typedefs, which collide with `<EGL/egl.h>` in Android translation units.
 */
enum class NativeWindowSystem : uint8_t {
  Unknown,
  X11,
  Wayland,
  Win32,
  Android,
  Metal,
};

} // namespace backend

#endif // CORAL_BACKEND_TYPES_H

