cmake_minimum_required(VERSION 3.19)
set(TARGET effect)
set(PUBLIC_HDR_DIR include)
set(PRIVATE_HDR_DIR src)
set(SRC_DIR src)

# ==================================================================================================
# Sources and headers
# ==================================================================================================

set(PUBLIC_HDRS
    include/effect/CompletionToken.h
    include/effect/EffectEngine.h
    include/effect/EffectFrame.h
    include/effect/EffectInputNode.h
    include/effect/EffectNode.h
    include/effect/EffectOutputNode.h
    include/effect/EffectPassNode.h
    include/effect/EffectShader.h
    include/effect/EffectTexture.h
    include/effect/EffectTypes.h
    include/effect/Fence.h
    include/effect/ExternalTextureInputNode.h
    include/effect/ImageInputNode.h
    include/effect/NullOutputNode.h
    include/effect/PixelBufferView.h
    include/effect/ReadbackOutputNode.h
    include/effect/TestPatternInputNode.h
    include/effect/TextureOutputNode.h
    include/effect/VideoFrameInputNode.h
)

set(PRIVATE_HDRS
    src/effect/EffectExecutor.h
    src/effect/EffectInternal.h
    src/effect/EffectRenderCore.h
    src/effect/ShaderCacheWatch.h
)

set(SRCS
    src/EffectEngine.cpp
    src/EffectFrame.cpp
    src/EffectInputNode.cpp
    src/EffectNode.cpp
    src/EffectOutputNode.cpp
    src/EffectPassNode.cpp
    src/ExternalTextureInputNode.cpp
    src/Fence.cpp
    src/ImageInputNode.cpp
    src/NullOutputNode.cpp
    src/ReadbackOutputNode.cpp
    src/TestPatternInputNode.cpp
    src/TextureOutputNode.cpp
    src/VideoFrameInputNode.cpp
    src/effect/EffectExecutor.cpp
    src/effect/EffectGraph.cpp
    src/effect/EffectProgram.cpp
    src/effect/EffectRenderCore.cpp
    src/effect/ShaderCacheWatch.cpp
    src/effect/ShaderCompilePool.cpp
    src/effect/EffectSnapshot.cpp
    src/effect/EffectTexture.cpp
    src/effect/EffectTypes.cpp
)

# ==================================================================================================
# Includes & target definition
# ==================================================================================================
add_library(${TARGET} STATIC ${PUBLIC_HDRS} ${PRIVATE_HDRS} ${SRCS})

# MSVC + Ninja on a localized toolchain records no header dependencies at all: ninja matches cl's
# `/showIncludes` lines against the `msvc_deps_prefix` CMake writes, CMake writes it as UTF-8 and cl emits the
# system code page, so nothing matches and every object ends up with `#deps 0` (check any with
# `ninja -t deps <object>`). A header change then rebuilds nothing, and the stale objects present themselves as
# runtime bugs - which has already happened twice here (a vtable layout mismatch and a config-struct field
# shift). Object dependencies by hand are coarse - any header in this library rebuilds it - but they are correct,
# and only where the compiler's own deps are known to be broken.
if (MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
  set(_coral_header_deps "")
  foreach (_header IN LISTS PUBLIC_HDRS PRIVATE_HDRS)
    # `OBJECT_DEPENDS` resolves against the build directory, so these have to be absolute.
    get_filename_component(_coral_header "${_header}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    list(APPEND _coral_header_deps "${_coral_header}")
  endforeach ()
  set_source_files_properties(${SRCS} PROPERTIES OBJECT_DEPENDS "${_coral_header_deps}")
endif ()

target_include_directories(${TARGET}
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/${PUBLIC_HDR_DIR}>
    PUBLIC $<INSTALL_INTERFACE:include>
    PRIVATE ${PRIVATE_HDR_DIR} ${SRC_DIR} ${SRC_DIR}/effect
)
set_target_properties(${TARGET} PROPERTIES FOLDER Libs)

# The sources carry comments in more than one language. MSVC otherwise decodes them using the system code page,
# which warns (C4819) and can mis-read multi-byte characters in a comment.
if (MSVC)
  target_compile_options(${TARGET} PRIVATE /utf-8)
endif ()

# ==================================================================================================
# Dependencies
# ==================================================================================================
# The public headers include only the standard library; the implementation reaches the RHI through
# `graphics`, whose `backend` include directories are part of its public usage requirements.
target_link_libraries(${TARGET} PUBLIC graphics)
target_link_libraries(${TARGET} PUBLIC utils)
target_link_libraries(${TARGET} PRIVATE minilog yuv)

# ==================================================================================================
# Android platform target
# ==================================================================================================
# The core `effect` target names no platform type. Android's input and output nodes live in their own
# static library so an application that does not render to an `ANativeWindow` never links the NDK surface
# libraries - and so the Android-specific code cannot leak into the core's include graph.
if (ANDROID)
  add_library(effect_android STATIC
      include/effect/platform/android/AndroidSurfaceOutputNode.h
      include/effect/platform/android/AndroidHardwareBufferOutputNode.h
      include/effect/platform/android/AndroidSurfaceTextureInputNode.h
      include/effect/platform/android/AndroidHardwareBuffer.h
      include/effect/platform/android/AndroidHardwareBufferInputNode.h
      src/platform/android/AndroidSurfaceOutputNode.cpp
      src/platform/android/AndroidHardwareBufferOutputNode.cpp
      src/platform/android/AndroidSurfaceTextureInputNode.cpp
      src/platform/android/AndroidHardwareBuffer.cpp
      src/platform/android/AndroidHardwareBufferInputNode.cpp
  )
  target_include_directories(effect_android
      PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/${PUBLIC_HDR_DIR}>
      PUBLIC $<INSTALL_INTERFACE:include>
      # The platform nodes implement the internal InputRuntime/OutputRuntime interfaces, so this target
      # sees `src/`; a platform target is part of this library, not a consumer of it.
      PRIVATE ${PRIVATE_HDR_DIR} ${SRC_DIR} ${SRC_DIR}/effect
  )
  target_link_libraries(effect_android PUBLIC ${TARGET} android log EGL)
  set_target_properties(effect_android PROPERTIES FOLDER Libs)
endif ()

# ==================================================================================================
# Windows platform target
# ==================================================================================================
if (WIN32 AND NOT ANDROID)
  add_library(effect_win STATIC
      include/effect/platform/win/WindowsD3D11InputNode.h
      include/effect/platform/win/WindowsD3D11EncoderOutputNode.h
      include/effect/platform/win/WindowsSwapChainOutputNode.h
      src/platform/gl/GlExternalTexture.cpp
      src/platform/vulkan/VulkanExternalTexture.cpp
      include/effect/platform/vulkan/VulkanExternalTexture.h
      include/effect/platform/gl/GlExternalTexture.h
      src/platform/win/WindowsD3D11InputNode.cpp
      src/platform/win/WindowsD3D11EncoderOutputNode.cpp
      src/platform/win/WindowsSwapChainOutputNode.cpp
  )
  target_include_directories(effect_win
      PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/${PUBLIC_HDR_DIR}>
      PUBLIC $<INSTALL_INTERFACE:include>
      PRIVATE ${PRIVATE_HDR_DIR} ${SRC_DIR} ${SRC_DIR}/effect
  )
  target_link_libraries(effect_win PUBLIC ${TARGET} user32 gdi32 d3d11 dxgi PRIVATE minilog)
  set_target_properties(effect_win PROPERTIES FOLDER Libs)
endif ()

# ==================================================================================================
# Linux X11 platform target
# ==================================================================================================
if (LINUX)
  find_package(X11 REQUIRED)
  add_library(effect_x11 STATIC
      include/effect/platform/linux/LinuxX11OutputNode.h
      src/platform/linux/LinuxX11OutputNode.cpp
  )
  target_include_directories(effect_x11
      PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/${PUBLIC_HDR_DIR}>
      PUBLIC $<INSTALL_INTERFACE:include>
      PRIVATE ${PRIVATE_HDR_DIR} ${SRC_DIR} ${SRC_DIR}/effect ${X11_INCLUDE_DIR}
  )
  target_link_libraries(effect_x11 PUBLIC ${TARGET} ${X11_LIBRARIES})
  set_target_properties(effect_x11 PROPERTIES FOLDER Libs)

  find_package(PkgConfig REQUIRED)
  pkg_check_modules(WAYLAND REQUIRED wayland-client wayland-egl)
  add_library(effect_wayland STATIC
      include/effect/platform/linux/LinuxWaylandOutputNode.h
      src/platform/linux/LinuxWaylandOutputNode.cpp
  )
  target_include_directories(effect_wayland
      PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/${PUBLIC_HDR_DIR}>
      PUBLIC $<INSTALL_INTERFACE:include>
      PRIVATE ${PRIVATE_HDR_DIR} ${SRC_DIR} ${SRC_DIR}/effect ${WAYLAND_INCLUDE_DIRS}
  )
  target_link_libraries(effect_wayland PUBLIC ${TARGET} ${WAYLAND_LIBRARIES})
  set_target_properties(effect_wayland PROPERTIES FOLDER Libs)
endif ()

# ==================================================================================================
# Installation
# ==================================================================================================
install(TARGETS ${TARGET} ARCHIVE DESTINATION lib)
# Platform headers belong to their platform target, not to a book build that happens to share the tree.
install(DIRECTORY ${PUBLIC_HDR_DIR}/${TARGET} DESTINATION include PATTERN "platform" EXCLUDE)
if (ANDROID)
  install(TARGETS effect_android ARCHIVE DESTINATION lib)
  install(DIRECTORY ${PUBLIC_HDR_DIR}/${TARGET}/platform/android
      DESTINATION include/${TARGET}/platform)
endif ()
if (WIN32 AND NOT ANDROID)
  install(TARGETS effect_win ARCHIVE DESTINATION lib)
  install(DIRECTORY ${PUBLIC_HDR_DIR}/${TARGET}/platform/win
      DESTINATION include/${TARGET}/platform)
endif ()
if (LINUX)
  install(TARGETS effect_x11 effect_wayland ARCHIVE DESTINATION lib)
  install(DIRECTORY ${PUBLIC_HDR_DIR}/${TARGET}/platform/x11
      DESTINATION include/${TARGET}/platform)
  install(DIRECTORY ${PUBLIC_HDR_DIR}/${TARGET}/platform/wayland
      DESTINATION include/${TARGET}/platform)
endif ()

# ==================================================================================================
# Public header boundary
# ==================================================================================================
# One translation unit per installed header, so a header that only compiles behind another header's
# includes fails here rather than in a client. Generated rather than checked in because the list is the
# `PUBLIC_HDRS` list above, and a check-in copy would drift from it.
set(HEADER_CHECK_SRCS)
set(HEADER_CHECK_DIR "${CMAKE_CURRENT_BINARY_DIR}/header_check")
foreach (header IN LISTS PUBLIC_HDRS)
  get_filename_component(header_name "${header}" NAME_WE)
  set(source "${HEADER_CHECK_DIR}/${header_name}.cpp")
  file(WRITE "${source}" "#include <effect/${header_name}.h>\n\nint main() { return 0; }\n")
  list(APPEND HEADER_CHECK_SRCS "${source}")
endforeach ()
add_library(${TARGET}_header_check OBJECT ${HEADER_CHECK_SRCS})
target_link_libraries(${TARGET}_header_check PRIVATE ${TARGET})
set_target_properties(${TARGET}_header_check PROPERTIES FOLDER Tests)
if (MSVC)
  target_compile_options(${TARGET}_header_check PRIVATE /utf-8)
endif ()

# ==================================================================================================
# Test and benchmark
# ==================================================================================================
# The graph and snapshot tests need no GPU; the GPU cases are inside the suite and skip themselves when no
# device can be brought up. Everything here is host-only because the test binary links gtest and the host's
# graphics stack. The demos are in the root examples/ project, which links this module as a client.
if (CORAL_BUILD_TESTS)
  enable_testing()

  add_executable(test_${TARGET}
      tests/test_main.cpp
      tests/CrashReport.cpp
      tests/test_DeviceLoss.cpp
      tests/test_EffectGraph.cpp
      tests/test_EffectNativeTexture.cpp
      tests/test_ExternalDevice.cpp
      tests/test_SdfText.cpp
      tests/test_EffectPipeline.cpp
      tests/test_PublicHeaders.cpp
      tests/test_ShaderCache.cpp
      tests/test_VideoFrameColor.cpp
  )
  target_link_libraries(test_${TARGET} PRIVATE ${TARGET} gtest)
  # The injected DeviceLost test implements an internal OutputRuntime; only the test target sees src/.
  target_include_directories(test_${TARGET} PRIVATE ${SRC_DIR} ${SRC_DIR}/effect)
  set_target_properties(test_${TARGET} PROPERTIES FOLDER Tests)

  # The test sources subclass the engine's internal interfaces, so they need the same header dependencies the
  # library gets above - and they are not in ${SRCS}, which is exactly how a vtable mismatch survived a rebuild
  # and crashed `DeviceLoss.ContinuousModeRecoversOnTheRenderThread` in `consume`.
  if (MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    get_target_property(_coral_test_sources test_${TARGET} SOURCES)
    set(_coral_test_deps "")
    foreach (_header IN LISTS PUBLIC_HDRS PRIVATE_HDRS)
      get_filename_component(_coral_header "${_header}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
      list(APPEND _coral_test_deps "${_coral_header}")
    endforeach ()
    file(GLOB_RECURSE _coral_backend_headers CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../graphics/backend/include/*.h")
    list(APPEND _coral_test_deps ${_coral_backend_headers})
    set_source_files_properties(${_coral_test_sources} PROPERTIES OBJECT_DEPENDS "${_coral_test_deps}")
  endif ()

  add_test(NAME test_${TARGET} COMMAND test_${TARGET})

  # The installed headers must not name the RHI. Done as a CTest so it runs with the rest of the suite and
  # fails a build that leaks a `backend::` type into an include a client can see.
  add_test(NAME effect_public_header_boundary
      COMMAND ${CMAKE_COMMAND}
      -DHEADER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/${PUBLIC_HDR_DIR}/${TARGET}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_public_headers.cmake
  )

  if (MSVC)
    target_compile_options(test_${TARGET} PRIVATE /utf-8 /W3)
  else ()
    target_compile_options(test_${TARGET} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif ()

  if (WIN32 AND TARGET effect_win)
    target_sources(test_${TARGET} PRIVATE tests/test_WindowsD3D11Input.cpp tests/test_WindowsDirectSurface.cpp tests/test_GlOverlay.cpp)
    target_link_libraries(test_${TARGET} PRIVATE effect_win d3d11 dxgi opengl32)
    # The crash handler symbolizes stacks with dbghelp; only the Windows test binary needs it.
    target_link_libraries(test_${TARGET} PRIVATE dbghelp)
  endif ()

  # The header-level benchmark stays with the module: it measures the engine itself rather than
  # demonstrating it, and the example project builds its own user-facing benchmark from the public API.
  add_executable(effect_hdr_benchmark benchmarks/effect_hdr_benchmark.cpp)
  target_link_libraries(effect_hdr_benchmark PRIVATE ${TARGET} benchmark benchmark_main)
  set_target_properties(effect_hdr_benchmark PROPERTIES
      FOLDER Tests
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  )
endif ()
