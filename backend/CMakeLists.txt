cmake_minimum_required(VERSION 3.19)

# The implementation layer. Everything under backend is this target's business: the RHI
# vocabulary, the OpenGL backend and its per-platform context creation. The root CMakeLists is the
# facade that clients use, so nothing here is meant to be reached for directly except by an advanced
# caller that wants the raw device.
set(TARGET backend)

# Published to CMakeLists.txt, which links against it. The parent cannot simply guess the name:
# the whole point of the subdirectory is that the implementation owns its own target identity.
set(BACKEND_TARGET ${TARGET} PARENT_SCOPE)

set(PUBLIC_HDR_DIR ${CMAKE_CURRENT_LIST_DIR}/include)   # backend/...            -> backend::Device
set(PRIVATE_HDR_DIR ${CMAKE_CURRENT_LIST_DIR}/include/private) # private/backend/gl/... -> GL internals
set(SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/src)

# The facade headers live one level up, in include/gpu. The backend reads a few of them (a
# viewport is a value type shared by both layers), so the parent include root is on this target's
# interface. It is not a cycle: the facade depends on backend, and backend only depends on the *header*
# directory, never on the facade target.
set(ROOT_HDR_DIR ${CMAKE_CURRENT_LIST_DIR}/../include)

# ==================================================================================================
# Backend selection
# ==================================================================================================
# The RHI is designed so that a backend is a build time choice, not a source change: upper layers only
# ever see backend/, never private/backend/<api>. These options decide which implementation lands in
# libbackend and which CORAL_BACKEND_HAS_* macros are defined.
#
# The options are declared by the root CMakeLists, which has to read them before adding the
# SPIRV-Cross subdirectory. Defaults are repeated here so a configure that adds CoralGraphics
# directly also works.

if (NOT DEFINED CORAL_BACKEND_SUPPORTS_OPENGL)
  option(CORAL_BACKEND_SUPPORTS_OPENGL "Build the OpenGL backend for the RHI" ON)
endif ()

# Vulkan is not offered on Apple platforms: macOS and iOS have Metal and no Vulkan loader, so a Vulkan build there
# could only ever report `Unsupported` after paying for a loader lookup. Forced off rather than merely defaulted off, so
# a cached `-DCORAL_BACKEND_SUPPORTS_VULKAN=ON` in an existing macOS build tree (CLion's, for instance) is corrected on
# the next configure instead of bringing the dead backend back.
if (APPLE)
  if (CORAL_BACKEND_SUPPORTS_VULKAN)
    message(STATUS "The RHI's Vulkan backend is not offered on Apple platforms; Metal is the Apple backend")
  endif ()
  set(CORAL_BACKEND_SUPPORTS_VULKAN OFF CACHE BOOL
      "Build the Vulkan backend for the RHI (not offered on Apple platforms)" FORCE)
endif ()

if (NOT DEFINED CORAL_BACKEND_SUPPORTS_VULKAN)
  option(CORAL_BACKEND_SUPPORTS_VULKAN "Build the Vulkan backend for the RHI" OFF)
endif ()

# Metal is the third backend and the only Apple-only one. Its default is "on where it can work": a macOS or iOS build
# with the Metal backend compiled out is a build that has to select the deprecated OpenGL path on hardware that has
# Metal, which is not what a default should do. A non-Apple configure forces it off rather than failing, because the
# option is also set by the application's root and a Windows tree should not inherit an error from it.
if (NOT DEFINED CORAL_BACKEND_SUPPORTS_METAL)
  if (APPLE)
    option(CORAL_BACKEND_SUPPORTS_METAL "Build the Metal backend for the RHI" ON)
  else ()
    option(CORAL_BACKEND_SUPPORTS_METAL "Build the Metal backend for the RHI" OFF)
  endif ()
endif ()
if (CORAL_BACKEND_SUPPORTS_METAL AND NOT APPLE)
  message(STATUS "CORAL_BACKEND_SUPPORTS_METAL is off: Metal exists only on Apple platforms")
  set(CORAL_BACKEND_SUPPORTS_METAL OFF)
endif ()

if (NOT DEFINED CORAL_BACKEND_SUPPORTS_SPIRV_CROSS)
  if (EXISTS ${EXTERNAL}/SPIRV-Cross/spirv_glsl.cpp)
    option(CORAL_BACKEND_SUPPORTS_SPIRV_CROSS "Build SPIRV-Cross so the RHI can consume SPIR-V shaders" ON)
  else ()
    set(CORAL_BACKEND_SUPPORTS_SPIRV_CROSS OFF)
    message(STATUS "SPIRV-Cross not found under third_party; RHI shader modules must be supplied as GLSL source")
  endif ()
endif ()

# shaderc compiles GLSL source to SPIR-V. Without it the RHI can still consume SPIR-V, it just cannot
# produce it from source, which is the difference between "a shader must be compiled offline" and "a
# shader is written in GLSL".
if (NOT DEFINED CORAL_BACKEND_SUPPORTS_SHADERC)
  if (EXISTS ${EXTERNAL}/shaderc/libshaderc/src/shaderc.cc)
    option(CORAL_BACKEND_SUPPORTS_SHADERC "Build shaderc so the RHI can compile GLSL source to SPIR-V" ON)
  else ()
    set(CORAL_BACKEND_SUPPORTS_SHADERC OFF)
    message(STATUS "shaderc not found under third_party; RHI shader modules must be supplied as SPIR-V")
  endif ()
endif ()

if (NOT CORAL_BACKEND_SUPPORTS_OPENGL AND NOT CORAL_BACKEND_SUPPORTS_VULKAN AND NOT CORAL_BACKEND_SUPPORTS_METAL)
  message(FATAL_ERROR "No RHI backend was selected; enable CORAL_BACKEND_SUPPORTS_OPENGL, _VULKAN or _METAL")
endif ()

# ## Vulkan needs shaderc, so the two options are not independent
#
# GLSL source compilation (shaderc) is optional for OpenGL, which consumes SPIR-V through SPIRV-Cross and can also be
# handed GLSL text directly. It is not optional for Vulkan: the platform API takes SPIR-V and nothing else, so a Vulkan
# build with `-DCORAL_BACKEND_SUPPORTS_SHADERC=OFF` would be a backend that cannot accept a single shader the way its
# callers write them. Refused here rather than at the first shader, because a configure error names the two options and a
# runtime one names neither. Metal will join this rule when the backend lands: it takes its shaders through the same
# offline-compilation path.
if (CORAL_BACKEND_SUPPORTS_VULKAN AND NOT CORAL_BACKEND_SUPPORTS_SHADERC)
  message(FATAL_ERROR
      "CORAL_BACKEND_SUPPORTS_VULKAN requires CORAL_BACKEND_SUPPORTS_SHADERC=ON: the Vulkan backend consumes SPIR-V and "
      "has no way to produce it from GLSL source. Either drop -DCORAL_BACKEND_SUPPORTS_SHADERC=OFF, or configure with "
      "-DCORAL_BACKEND_SUPPORTS_VULKAN=OFF to build the OpenGL backend, which does not need shaderc.")
endif ()

# ## Metal has the same dependency, one translation further along
#
# A Metal shader is MSL, and the only MSL this tree produces comes from SPIR-V through SPIRV-Cross's MSL backend. The
# module a caller hands the RHI is either SPIR-V already, or GLSL that shaderc has to turn into it first -- so Metal
# needs both toolchains, for the same reason Vulkan needs shaderc and one more. Refused here, where the message can
# name both options, rather than at the first shader.
if (CORAL_BACKEND_SUPPORTS_METAL AND NOT CORAL_BACKEND_SUPPORTS_SPIRV_CROSS)
  message(FATAL_ERROR
      "CORAL_BACKEND_SUPPORTS_METAL requires CORAL_BACKEND_SUPPORTS_SPIRV_CROSS=ON: Metal consumes MSL, which this "
      "tree produces by translating SPIR-V with SPIRV-Cross's MSL backend. Configure with "
      "-DCORAL_BACKEND_SUPPORTS_SPIRV_CROSS=ON, or with -DCORAL_BACKEND_SUPPORTS_METAL=OFF.")
endif ()
if (CORAL_BACKEND_SUPPORTS_METAL AND NOT CORAL_BACKEND_SUPPORTS_SHADERC)
  message(FATAL_ERROR
      "CORAL_BACKEND_SUPPORTS_METAL requires CORAL_BACKEND_SUPPORTS_SHADERC=ON: a GLSL shader module has to become "
      "SPIR-V before SPIRV-Cross can translate it to MSL. Configure with -DCORAL_BACKEND_SUPPORTS_SHADERC=ON, or with "
      "-DCORAL_BACKEND_SUPPORTS_METAL=OFF.")
endif ()

# ==================================================================================================
# Sources and headers
# ==================================================================================================
set(PUBLIC_HDRS
    ${PUBLIC_HDR_DIR}/backend/Capabilities.h
    ${PUBLIC_HDR_DIR}/backend/Descriptors.h
    ${PUBLIC_HDR_DIR}/backend/Device.h
    ${PUBLIC_HDR_DIR}/backend/Handles.h
    ${PUBLIC_HDR_DIR}/backend/Pipelines.h
    ${PUBLIC_HDR_DIR}/backend/RHI.h
    ${PUBLIC_HDR_DIR}/backend/Types.h
    ${PUBLIC_HDR_DIR}/backend/Validation.h
)

# Backend internals. Kept under include/private so that a backend's structs are never visible to upper
# layers even though the headers have to be reachable from the backend's own translation units.
set(PRIVATE_HDRS
    ${PRIVATE_HDR_DIR}/backend/gl/GlCommandBuffer.h
    ${PRIVATE_HDR_DIR}/backend/gl/GlDevice.h
    ${PRIVATE_HDR_DIR}/backend/gl/GlEnums.h
    ${PRIVATE_HDR_DIR}/backend/gl/GlEntryPoints.h
    ${PRIVATE_HDR_DIR}/backend/gl/GlPlatform.h
)

if (CORAL_BACKEND_SUPPORTS_VULKAN)
  list(APPEND PRIVATE_HDRS
      ${PRIVATE_HDR_DIR}/backend/vulkan/VulkanAndroidHardwareBuffer.h
      ${PRIVATE_HDR_DIR}/backend/vulkan/VulkanCommandBuffer.h
      ${PRIVATE_HDR_DIR}/backend/vulkan/VulkanDevice.h
      ${PRIVATE_HDR_DIR}/backend/vulkan/VulkanEnums.h
      ${PRIVATE_HDR_DIR}/backend/vulkan/VulkanEntryPoints.h
      ${PRIVATE_HDR_DIR}/backend/vulkan/VulkanPlatform.h
      ${PRIVATE_HDR_DIR}/backend/vulkan/VulkanX11Compat.h
  )
endif ()

if (CORAL_BACKEND_SUPPORTS_METAL)
  list(APPEND PRIVATE_HDRS
      ${PRIVATE_HDR_DIR}/backend/metal/MetalCommandBuffer.h
      ${PRIVATE_HDR_DIR}/backend/metal/MetalDevice.h
      ${PRIVATE_HDR_DIR}/backend/metal/MetalEnums.h
      ${PRIVATE_HDR_DIR}/backend/metal/MetalFactory.h
  )
endif ()

set(SRCS
    src/Capabilities.cpp
    src/Device.cpp
    src/ShaderCross.h
    src/ShaderCross.cpp
    src/GlslCompiler.h
    src/GlslCompiler.cpp
      src/TestHooks.cpp
    src/Types.cpp
    src/Validation.cpp
)

if (CORAL_BACKEND_SUPPORTS_OPENGL)
  list(APPEND SRCS
      # Window-system independent.
      src/gl/GlCommandBuffer.cpp
      src/gl/GlDevice.cpp
      src/gl/GlEntryPoints.cpp
      src/gl/GlPlatform.cpp
  )
  # Each platform compiles the implementation it needs. Linux is the one exception: it compiles GLX and
  # EGL together behind GlPlatformLinux.cpp, because one binary has to serve both X11 and Wayland. Every
  # file is also guarded by its own platform macro, so a mis-selected file produces an empty translation
  # unit rather than a duplicate symbol.
  #
  # The `else` branch is the null implementation rather than a fatal error: the device factory walks a
  # priority list, so a platform with no OpenGL has to report Unsupported at run time so the
  # application can move on to another backend. Refusing to configure would take that choice away.
  if (WIN32)
    list(APPEND SRCS src/gl/platform/GlPlatformWin32.cpp)
  elseif (APPLE AND NOT IOS)
    list(APPEND SRCS src/gl/platform/GlPlatformCgl.cpp)
  elseif (IOS)
    # iOS has no EGL: EAGL is its context API. The source is Objective-C++ because `EAGLContext` is; the root
    # project enables that language for iOS only.
    list(APPEND SRCS src/gl/platform/GlPlatformEagl.mm)
  elseif (ANDROID)
    list(APPEND SRCS src/gl/platform/GlPlatformEgl.cpp)
  elseif (LINUX)
    # One binary, two window systems: GLX for X11 and EGL for Wayland (and the surfaceless fallback).
    # GlPlatformLinux.cpp is the dispatcher; the other two name their entry points glx.../egl... so
    # they can coexist in one library.
    list(APPEND SRCS
        src/gl/platform/GlPlatformLinux.cpp
        src/gl/platform/GlPlatformGlx.cpp
        src/gl/platform/GlPlatformEgl.cpp
    )
  else ()
    message(STATUS
        "The RHI's OpenGL backend has no window-system implementation for this platform; it will "
        "report Result::Unsupported at run time. Add one under backend/src/gl/platform/ "
        "implementing createPlatformContext and friends.")
    list(APPEND SRCS src/gl/platform/GlPlatformNull.cpp)
  endif ()
endif ()

# --------------------------------------------------------------------------------------------------
# Vulkan
# --------------------------------------------------------------------------------------------------
# The Vulkan backend's sources. `VulkanPlatform.cpp` contains the per-platform surface creation
# guarded by platform macros; unlike the OpenGL backend there is exactly one such file because Vulkan
# has no context to create and no per-platform loader to abstract.
if (CORAL_BACKEND_SUPPORTS_VULKAN)
  list(APPEND SRCS
      src/vulkan/VulkanAllocator.cpp
      src/vulkan/VulkanAndroidHardwareBuffer.cpp
      src/vulkan/VulkanCommandBuffer.cpp
      src/vulkan/VulkanDevice.cpp
      src/vulkan/VulkanEnums.cpp
      src/vulkan/VulkanEntryPoints.cpp
      src/vulkan/VulkanPlatform.cpp
      src/vulkan/VulkanPresentation.cpp
      src/vulkan/VulkanResources.cpp
  )

  # ## Where the Vulkan headers come from
  #
  # `third_party/vkheaders` is the Khronos header repository itself. It is used as an include
  # directory rather than through `add_subdirectory`: the vendored CMakeLists requires CMake 3.22 and
  # installs a package, and a header-only dependency the build already has should not add either.
  #
  # `EXTERNAL` is set by the Coral root and by CoralGraphics; the fallback path is what makes this file work
  # when it is included from a tree that only adds CoralGraphics.
  set(CORAL_VULKAN_HEADERS_DIR "${EXTERNAL}/vkheaders/include")
  if (NOT EXISTS "${CORAL_VULKAN_HEADERS_DIR}/vulkan/vulkan.h")
    set(CORAL_VULKAN_HEADERS_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/vkheaders/include")
  endif ()
  if (NOT EXISTS "${CORAL_VULKAN_HEADERS_DIR}/vulkan/vulkan.h")
    message(FATAL_ERROR
        "CORAL_BACKEND_SUPPORTS_VULKAN is ON but third_party/vkheaders/include/vulkan/vulkan.h was not "
        "found. Restore the vendored headers or configure with -DCORAL_BACKEND_SUPPORTS_VULKAN=OFF.")
  endif ()

  # Vulkan Memory Allocator is header-only apart from the one translation unit that defines
  # VMA_IMPLEMENTATION, which is in SRCS above.
  if (NOT TARGET vkmemalloc)
    set(CORAL_VMEMMALLOC_DIR "${EXTERNAL}/vkmemalloc/include")
    if (NOT EXISTS "${CORAL_VMEMMALLOC_DIR}/vk_mem_alloc.h")
      set(CORAL_VMEMMALLOC_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/vkmemalloc/include")
    endif ()
    if (NOT EXISTS "${CORAL_VMEMMALLOC_DIR}/vk_mem_alloc.h")
      message(FATAL_ERROR
          "CORAL_BACKEND_SUPPORTS_VULKAN is ON but third_party/vkmemalloc/include/vk_mem_alloc.h was not found.")
    endif ()
  endif ()
endif ()

# --------------------------------------------------------------------------------------------------
# Metal
# --------------------------------------------------------------------------------------------------
# Six Objective-C++ translation units, because every resource payload holds an ARC-managed Metal object and the
# compiler is the only thing that can generate the release. The split mirrors the Vulkan backend's: lifecycle and
# capabilities, resources and pipelines, command recording, presentation, enum translation, and the SPIR-V -> MSL pass.
if (CORAL_BACKEND_SUPPORTS_METAL)
  list(APPEND SRCS
      src/metal/MetalCommandBuffer.mm
      src/metal/MetalCross.h
      src/metal/MetalCross.mm
      src/metal/MetalDevice.mm
      src/metal/MetalEnums.mm
      src/metal/MetalPresentation.mm
      src/metal/MetalResources.mm
  )
endif ()

# ==================================================================================================
# Definition and includes
# ==================================================================================================
add_library(${TARGET} STATIC ${PUBLIC_HDRS} ${PRIVATE_HDRS} ${SRCS})

if (CORAL_BACKEND_SUPPORTS_METAL)
  # ## ARC for the Metal sources only
  #
  # The OpenGL backend's EAGL platform layer is Objective-C++ written against manual retain/release, so the flag cannot
  # go on the target. Attaching it per source keeps the two styles from colliding and makes "a Metal payload releases
  # itself" a property of the files that hold Metal objects rather than of the whole library.
  set(CORAL_METAL_SRCS
      ${SRC_DIR}/metal/MetalCommandBuffer.mm
      ${SRC_DIR}/metal/MetalCross.mm
      ${SRC_DIR}/metal/MetalDevice.mm
      ${SRC_DIR}/metal/MetalEnums.mm
      ${SRC_DIR}/metal/MetalPresentation.mm
      ${SRC_DIR}/metal/MetalResources.mm
  )
  set_source_files_properties(${CORAL_METAL_SRCS} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif ()

# See the note in effect/CMakeLists.txt: on a localized MSVC toolchain ninja records no header dependencies,
# so they are declared here instead. Coarse (a header change rebuilds this library) but correct, and confined to
# the configuration where the compiler's own dependency output cannot be matched.
if (MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
  set(_coral_header_deps "")
  foreach(_header IN LISTS PUBLIC_HDRS PRIVATE_HDRS)
    # `OBJECT_DEPENDS` resolves against the build directory, so these have to be absolute. The two libraries in
    # this tree disagree about whether their header variables are absolute, hence the normalisation.
    get_filename_component(_coral_header "${_header}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    list(APPEND _coral_header_deps "${_coral_header}")
  endforeach()
  set_source_files_properties(${SRCS} PROPERTIES OBJECT_DEPENDS "${_coral_header_deps}")
endif()

target_include_directories(${TARGET}
    # PUBLIC: what a consumer needs to resolve `#include <backend/...>` and `#include <gpu/...>`. The
    # build interface carries the source tree, the install interface the `include` directory the
    # headers are installed into; an absolute build path here would break install(EXPORT).
    PUBLIC $<BUILD_INTERFACE:${PUBLIC_HDR_DIR}> $<BUILD_INTERFACE:${ROOT_HDR_DIR}> $<INSTALL_INTERFACE:include>
    PRIVATE ${PRIVATE_HDR_DIR} ${SRC_DIR}
)

set_target_properties(${TARGET} PROPERTIES FOLDER Libs)

# ==================================================================================================
# Vulkan configuration (the target exists from here on)
# ==================================================================================================
if (CORAL_BACKEND_SUPPORTS_VULKAN)
  target_include_directories(${TARGET} PRIVATE "${CORAL_VULKAN_HEADERS_DIR}")
  if (TARGET vkmemalloc)
    target_link_libraries(${TARGET} PRIVATE $<BUILD_INTERFACE:vkmemalloc>)
  else ()
    target_include_directories(${TARGET} PRIVATE "${CORAL_VMEMMALLOC_DIR}")
  endif ()

  # ## Prototypes off, functions loaded by name
  #
  # `VK_NO_PROTOTYPES` makes `vulkan.h` declare only the `PFN_*` types, so nothing in this backend can
  # accidentally call a global `vkCreateBuffer` that would then need a loader import library at link
  # time. Every call goes through the `VulkanApi` table. `VMA_STATIC_VULKAN_FUNCTIONS=0` is the same
  # decision for VMA: it receives `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr` and resolves everything
  # else itself.
  target_compile_definitions(${TARGET} PRIVATE
      VK_NO_PROTOTYPES=1
      VMA_STATIC_VULKAN_FUNCTIONS=0
  )

  # The platform surface structure headers (`VkWin32SurfaceCreateInfoKHR` and friends) are included by
  # `vulkan.h` only when the corresponding `VK_USE_PLATFORM_*` macro is defined.
  if (WIN32)
    target_compile_definitions(${TARGET} PRIVATE VK_USE_PLATFORM_WIN32_KHR=1)
  elseif (ANDROID)
    target_compile_definitions(${TARGET} PRIVATE VK_USE_PLATFORM_ANDROID_KHR=1)
    # The hardware buffer import takes its reference on the buffer through libandroid.so, and the symbols
    # start at API 26 while the app targets 23: they are resolved with dlopen at run time, so the loader
    # library is what has to be linked.
    target_link_libraries(${TARGET} PRIVATE dl)
  elseif (LINUX)
    # Both Linux surfaces are compiled in: Xlib for X11 and Wayland for Wayland. The Wayland header
    # only forward-declares `wl_display`/`wl_surface`, so the backend does not need wayland-client
    # headers or a link dependency; the application owns the connection and the handles.
    target_compile_definitions(${TARGET} PRIVATE
        VK_USE_PLATFORM_XLIB_KHR=1
        VK_USE_PLATFORM_WAYLAND_KHR=1
    )
    if (X11_FOUND)
      target_include_directories(${TARGET} PRIVATE ${X11_INCLUDE_DIR})
    endif ()
  endif ()
endif ()

# Every source in this library is UTF-8 without a BOM. MSVC otherwise decodes source files using the
# system code page, which both warns (C4819) and can mis-read multi-byte characters in comments; on
# some locales that mis-parse ends up reported as a spurious syntax error in an unrelated line.
if (MSVC)
  target_compile_options(${TARGET} PRIVATE /utf-8)
endif ()

if (WIN32)
  # `vulkan_win32.h` includes `<windows.h>`, which defines `min`/`max` as macros and turns every
  # `std::max(...)` in a Vulkan translation unit into a syntax error. The macros are never wanted here.
  target_compile_definitions(${TARGET} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif ()

# ==================================================================================================
# Configuration macros
# ==================================================================================================
if (CORAL_BACKEND_SUPPORTS_OPENGL)
  target_compile_definitions(${TARGET} PUBLIC CORAL_BACKEND_HAS_OPENGL=1)
endif ()

if (CORAL_BACKEND_SUPPORTS_METAL)
  target_compile_definitions(${TARGET} PUBLIC CORAL_BACKEND_HAS_METAL=1)
else ()
  target_compile_definitions(${TARGET} PUBLIC CORAL_BACKEND_HAS_METAL=0)
endif ()

if (CORAL_BACKEND_SUPPORTS_VULKAN)
  target_compile_definitions(${TARGET} PUBLIC CORAL_BACKEND_HAS_VULKAN=1)
else ()
  # Defined as 0 rather than left undefined, so `#if CORAL_BACKEND_HAS_VULKAN` reads as a statement
  # about the build; the Android export reads it to avoid an output node only the Vulkan backend can
  # serve.
  target_compile_definitions(${TARGET} PUBLIC CORAL_BACKEND_HAS_VULKAN=0)
endif ()

if (CORAL_BACKEND_SUPPORTS_SPIRV_CROSS)
  target_compile_definitions(${TARGET} PUBLIC CORAL_BACKEND_HAS_SPIRV_CROSS=1)
endif ()

if (CORAL_BACKEND_SUPPORTS_SHADERC)
  target_compile_definitions(${TARGET} PUBLIC CORAL_BACKEND_HAS_SHADERC=1)
else ()
  # Defined as 0 rather than left undefined, so every `#if CORAL_BACKEND_HAS_SHADERC` in this tree reads as a statement
  # about the build instead of relying on an undefined identifier evaluating to zero.
  target_compile_definitions(${TARGET} PUBLIC CORAL_BACKEND_HAS_SHADERC=0)
endif ()

# The GL entry points are resolved at run time. On Windows and Linux the backend still links against
# the system GL library, for the version 1.1 symbols the window-system loaders do not provide.

# ==================================================================================================
# Dependencies
# ==================================================================================================
target_link_libraries(${TARGET} PUBLIC utils)
target_link_libraries(${TARGET} PRIVATE $<BUILD_INTERFACE:minilog>)

# The SPIRV-Cross target is added by the Coral root (and by the standalone framework build) only when
# the vendored sources are present, so the dependency is keyed off the target rather than off the
# option: an option that is ON while the subdirectory was skipped would otherwise fail at generate time
# with a confusing "target not found".
if (CORAL_BACKEND_SUPPORTS_SPIRV_CROSS AND TARGET SPIRV-Cross)
  target_link_libraries(${TARGET} PRIVATE $<BUILD_INTERFACE:SPIRV-Cross>)
endif ()

# Keyed off the target rather than the option, for the same reason as SPIRV-Cross above: an option that
# is ON while the subdirectory was skipped would otherwise fail at generate time.
if (CORAL_BACKEND_SUPPORTS_SHADERC AND TARGET shaderc)
  target_link_libraries(${TARGET} PRIVATE $<BUILD_INTERFACE:shaderc>)
endif ()

if (WIN32)
  # opengl32 supplies the GL 1.1 entry points and wglGetProcAddress; everything newer comes from the
  # driver through that loader.
  target_link_libraries(${TARGET} PUBLIC opengl32)
elseif (ANDROID)
  # Android's EGL and GLES entry points come from the platform libraries, which the run-time loader
  # falls back to when eglGetProcAddress declines to resolve a core ES 3.0 function.
  target_link_libraries(${TARGET} PUBLIC EGL GLESv3)
elseif (IOS)
  find_library(OPENGLES_FRAMEWORK OpenGLES REQUIRED)
  target_link_libraries(${TARGET} PUBLIC $<BUILD_INTERFACE:${OPENGLES_FRAMEWORK}>)
elseif (APPLE)
  find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
  target_link_libraries(${TARGET} PUBLIC $<BUILD_INTERFACE:${OPENGL_FRAMEWORK}>)
  # Apple has deprecated OpenGL since 10.14 but still ships a working 4.1 core driver, which is the highest
  # version the desktop dialect targets. The deprecation attribute on every CGL call is noise about a
  # decision this backend has already made, so it is silenced for the backend and its tests rather than
  # repeated as a hundred warnings. The global flags suppress `-Wdeprecated-declarations` but the SDK's
  # `OPENGL_DEPRECATED` attribute needs this define to stay quiet.
  target_compile_definitions(${TARGET} PUBLIC GL_SILENCE_DEPRECATION=1)
elseif (LINUX)
  find_package(OpenGL QUIET)
  if (OPENGL_FOUND)
    target_link_libraries(${TARGET} PUBLIC $<BUILD_INTERFACE:OpenGL::GL>)
  else ()
    find_library(GL_LIBRARY GL)
    if (GL_LIBRARY)
      target_link_libraries(${TARGET} PUBLIC $<BUILD_INTERFACE:${GL_LIBRARY}>)
    endif ()
  endif ()
  find_package(X11 QUIET)
  if (X11_FOUND)
    target_link_libraries(${TARGET} PUBLIC $<BUILD_INTERFACE:X11::X11>)
  endif ()
  target_link_libraries(${TARGET} PUBLIC ${CMAKE_DL_LIBS})
endif ()

# Metal and the frameworks it needs. `QuartzCore` is CAMetalLayer's home; `Foundation` provides NSString/NSError, which
# the diagnostics and the shader compiler use. Linked PUBLIC because the private headers import Metal, and a consumer of
# `backend` that includes one of them (the backend's own tests do) needs the same link line.
if (CORAL_BACKEND_SUPPORTS_METAL)
  find_library(METAL_FRAMEWORK Metal REQUIRED)
  find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
  find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
  # CoreGraphics owns CGColorSpace, which is how CAMetalLayer is told whether its drawable is sRGB,
  # Display P3, extended-linear P3 or PQ.
  find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
  # IOSurface is the shared-surface object the Metal backend wraps as an `id<MTLTexture>` for zero-copy
  # video frames; it is also what a CVPixelBuffer is backed by.
  find_library(IOSURFACE_FRAMEWORK IOSurface REQUIRED)
  target_link_libraries(${TARGET} PUBLIC
      $<BUILD_INTERFACE:${METAL_FRAMEWORK}>
      $<BUILD_INTERFACE:${QUARTZCORE_FRAMEWORK}>
      $<BUILD_INTERFACE:${FOUNDATION_FRAMEWORK}>
      $<BUILD_INTERFACE:${COREGRAPHICS_FRAMEWORK}>
      $<BUILD_INTERFACE:${IOSURFACE_FRAMEWORK}>
  )
endif ()

# ==================================================================================================
# Installation
# ==================================================================================================
# libbackend is an implementation detail of libgpu, so it is installed as an archive but its headers
# are installed by CMakeLists.txt together with the facade's: a consumer of the installed
# package should see one `include/` tree, not two. Nothing else is installed from here, including this
# file's own documentation - the package has one README, at docs/graphics.md.
install(TARGETS ${TARGET} EXPORT CoralUITargets ARCHIVE DESTINATION lib)

# ==================================================================================================
# Tests
# ==================================================================================================
# The tests run on host platforms only. Besides the device-free suites they include a PlatformRunner
# that creates a real device for every backend the build supports, which needs a desktop or mobile
# environment rather than a cross-compile staging directory.
if (CORAL_BUILD_TESTS)
  # Required for the `add_test` at the end of this block to be reachable: CMake writes a directory's
  # `CTestTestfile.cmake` only when `enable_testing()` has been called in that directory. See the longer note in
  # CMakeLists.txt.
  enable_testing()
  add_executable(test_${TARGET}
      tests/test_asserts.h
      tests/test_Capabilities.cpp
      tests/test_GlslCompiler.cpp
      tests/test_Handles.cpp
      tests/test_main.cpp
      tests/test_PlatformRunner.cpp
      tests/test_ShaderCross.cpp
      tests/test_Threading.cpp
      tests/test_Types.cpp
      tests/test_Validation.cpp
  )
  target_link_libraries(test_${TARGET} PRIVATE ${TARGET} gtest)

  # The MSL translation's own suite. It is device-free -- SPIRV-Cross and a string -- but it can only link when the MSL
  # backend was compiled in, so it is added only for a Metal build.
  if (CORAL_BACKEND_SUPPORTS_METAL)
    target_sources(test_${TARGET} PRIVATE tests/test_MetalCross.cpp)
  endif ()

  # ShaderCross.h is a private header of the library, but the tests exercise the SPIR-V translation
  # path directly: it is the piece most likely to break silently when SPIRV-Cross is updated, and a
  # failure there otherwise only shows up as a black screen.
  #
  # `PRIVATE_HDR_DIR` is here for the same reason one level over: `test_Threading.cpp` drives `GLDevice`'s
  # command-buffer container and the write-once metadata blocks around `HandlePool`, both of which are
  # declared in `private/backend/gl/GlDevice.h`. Testing them through the public API is not possible -- they
  # exist precisely because the public API must not expose them -- so the test reaches in.
  target_include_directories(test_${TARGET} PRIVATE ${PRIVATE_HDR_DIR} ${SRC_DIR})

  # shaderc, for the one thing that cannot be reached through the library's own compiler: a module that carries a
  # `layout(push_constant)` block. That declaration is refused for the OpenGL target environment at every version,
  # so the only way to build such a module is to ask shaderc for a **Vulkan**-environment compile -- which is what
  # `PlatformRunner.PushConstantsReachTheShader` does, and why the test needs the library's own dependency rather
  # than `compileGlslToSpirv`. Linked rather than declared so the include directories come with it; guarded the same
  # way the library guards it, because a build without shaderc cannot build that module at all.
  if (CORAL_BACKEND_SUPPORTS_SHADERC AND TARGET shaderc)
    target_link_libraries(test_${TARGET} PRIVATE shaderc)
  endif ()

  # The SPIR-V fixture directory is passed in so the test runs from any working directory; the test
  # also falls back to locating the fixture relative to its own source file.
  target_compile_definitions(test_${TARGET} PRIVATE
      CORAL_BACKEND_FIXTURE_DIR="${CMAKE_CURRENT_LIST_DIR}/tests/fixtures")

  set_target_properties(test_${TARGET} PROPERTIES FOLDER Tests)

  if (MSVC)
    target_compile_options(test_${TARGET} PRIVATE /utf-8 /W3)
  else ()
    target_compile_options(test_${TARGET} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif ()

  # Registered with CTest, which this suite was not -- see the longer note in CMakeLists.txt. Without it
  # `ctest` in the build root ran fourteen tests and none of the graphics ones, while these executables were built and
  # run by hand.
  add_test(NAME test_${TARGET} COMMAND test_${TARGET})
  set_tests_properties(test_${TARGET} PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}")
endif ()


