# ==================================================================================================
# CoralGraphics
# ==================================================================================================
# The graphics framework: the gpu facade, the backend RHI, geometry, image codecs, fonts, shaders and
# their tests, plus the effect render-graph module. The modules live under modules/; this root resolves
# the framework's dependencies and gives a standalone checkout the same entry point as the application.
#
# As a subproject the application has already added CoralCore and its third-party targets; as a
# top-level checkout this file resolves them through cmake/CoralGraphicsDependencies.cmake.
#
# The demos live in the examples/ project. A top-level configure adds them alongside the libraries,
# and the same project can be configured on its own:
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   ctest --test-dir build --output-on-failure
#
#   # macOS (Command Line Tools; Ninja is optional)
#   cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j8
#   ctest --test-dir build --output-on-failure
#
#   # iOS simulator, from a macOS host with full Xcode (cross build; see README.md for the test commands)
#   cmake -S . -B build-ios -G "Unix Makefiles" -DCMAKE_SYSTEM_NAME=iOS \
#         -DCMAKE_SYSTEM_PROCESSOR=arm64 -DCMAKE_OSX_SYSROOT=iphonesimulator \
#         -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-ios -j8
#
#   cmake -S examples -B build/examples -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/examples

cmake_minimum_required(VERSION 3.19)
project(CoralGraphics LANGUAGES CXX)

# Apple platforms have one Objective-C++ translation unit each: the iOS EAGL platform layer and the macOS
# demo's Cocoa window. The language is enabled only on Apple; a Windows or Linux configure never names an
# Objective-C++ compiler.
if (APPLE)
  enable_language(OBJCXX)
endif ()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(cmake/CoralBuild.cmake)
coral_apply_global_build_settings()

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(CORALGRAPHICS_TOP_LEVEL ON)
else ()
  set(CORALGRAPHICS_TOP_LEVEL OFF)
endif ()

if (CORALGRAPHICS_TOP_LEVEL)
  if (NOT DEFINED IS_HOST_PLATFORM)
    set(IS_HOST_PLATFORM TRUE)
  endif ()
  # The public switch: a caller's -DCORAL_BUILD_TESTS=OFF must win over the standalone default of ON.
  if (NOT DEFINED CORAL_BUILD_TESTS)
    set(CORAL_BUILD_TESTS ON CACHE BOOL "Build the CoralGraphics tests")
  endif ()
else ()
  if (NOT DEFINED CORAL_BUILD_TESTS)
    set(CORAL_BUILD_TESTS OFF CACHE BOOL "Build the CoralGraphics tests")
  endif ()
endif ()
# The module-level name mirrors the public one, and is forced so that changing the public switch in an
# existing build tree takes effect on the next configure rather than being masked by the old cache entry.
set(CORALGRAPHICS_BUILD_TESTS ${CORAL_BUILD_TESTS} CACHE BOOL "Build the CoralGraphics tests" FORCE)
set(CORAL_BUILD_TESTS ${CORALGRAPHICS_BUILD_TESTS})
if (CORAL_BUILD_TESTS)
  enable_testing()
endif ()

# Feature switches. The application root sets these before adding the framework; the defaults here
# match the host application so a standalone checkout works without a cache file.
option(CORAL_GRAPHICS_SUPPORTS_JPEG "Build the JPEG codec" ON)
option(CORAL_GRAPHICS_SUPPORTS_WEBP "Build the WebP codec" ON)
option(CORAL_GRAPHICS_SUPPORTS_FONTS "Build the font stack" ON)
option(CORAL_GRAPHICS_SUPPORTS_JSON "Build the JSON/Lottie reader" ON)
option(CORAL_BACKEND_SUPPORTS_OPENGL "Build the OpenGL backend" ON)
option(CORAL_BACKEND_SUPPORTS_VULKAN "Build the Vulkan backend" ON)
option(CORAL_BACKEND_SUPPORTS_SPIRV_CROSS "Build SPIRV-Cross" ON)
option(CORAL_BACKEND_SUPPORTS_SHADERC "Build shaderc" ON)
option(CORAL_SUPPORTS_FFMPEG "Build FFmpeg integration" OFF)
# effect is the render-graph layer used by CoralPlayback and the application's media pipeline. CoralUI
# and GUI-only consumers do not need it; set this OFF to omit its archive and platform targets.
option(CORAL_GRAPHICS_WITH_EFFECT "Build the effect render-graph module" ON)

# ==================================================================================================
# Optional sanitizer
# ==================================================================================================
# A sanitizer build of the suites is worth having because the raster paths, the boolean operations and
# the recording closures all index buffers by arithmetic, and the tests exercise them on edges.
#
#   cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCORAL_GRAPHICS_SANITIZE=address
#
# Scoped with add_compile_options/add_link_options rather than CMAKE_CXX_FLAGS, because setting that
# variable *replaces* CMake's own flags rather than extending them. Only a top-level configure applies
# it: a subproject must not change the application's flags.
set(CORAL_GRAPHICS_SANITIZE "" CACHE STRING "Sanitizer for this tree, e.g. address (standalone only)")

if (CORALGRAPHICS_TOP_LEVEL AND CORAL_GRAPHICS_SANITIZE)
  if (MSVC)
    message(STATUS "CoralGraphics: building with /fsanitize=${CORAL_GRAPHICS_SANITIZE}")
    add_compile_options("/fsanitize=${CORAL_GRAPHICS_SANITIZE}" /Zi)
    add_link_options("/fsanitize=${CORAL_GRAPHICS_SANITIZE}" /INCREMENTAL:NO)
  else ()
    message(STATUS "CoralGraphics: building with -fsanitize=${CORAL_GRAPHICS_SANITIZE}")
    add_compile_options("-fsanitize=${CORAL_GRAPHICS_SANITIZE}" -fno-omit-frame-pointer -g)
    add_link_options("-fsanitize=${CORAL_GRAPHICS_SANITIZE}")
  endif ()
endif ()

# ==================================================================================================
# Dependencies
# ==================================================================================================
# Resolves CoralCore and every third-party target the modules link, for a configure that starts here or
# in the examples project. The application root resolves them itself; the manifest is guarded by
# `NOT TARGET`, so including it there adds nothing.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/CoralGraphicsDependencies.cmake")

if (NOT TARGET png)
  message(FATAL_ERROR
      "CoralGraphics third-party targets are missing. Configure this repository standalone so that "
      "cmake/CoralGraphicsDependencies.cmake runs, or add the application dependencies before "
      "CoralGraphics.")
endif ()

# The graphics-only third-party libraries now live in this repository. The backend's path-based
# Vulkan/SPIRV checks and the tooling read EXTERNAL; keep it pointing at this framework's own tree so
# the application's shared EXTERNAL (libyuv/libz/libgtest) is untouched.
set(EXTERNAL "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

add_subdirectory(modules/graphics)

if (CORAL_GRAPHICS_WITH_EFFECT)
  add_subdirectory(modules/effect)
endif ()

# ==================================================================================================
# Examples
# ==================================================================================================
# A standalone configure builds the demos too, so `ctest` covers the programs a client would write.
# The project is also configurable on its own (`cmake -S examples`), which is what an IDE opens when
# working on a demo; it detects the two modes itself. An application that adds this framework as a
# subproject does not get example executables.
if (CORALGRAPHICS_TOP_LEVEL)
  add_subdirectory(examples)
endif ()
