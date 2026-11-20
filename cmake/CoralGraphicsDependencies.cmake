# ==================================================================================================
# CoralGraphics standalone dependencies
# ==================================================================================================
# Adds every target that `modules/graphics` and `modules/effect` name in their link lines, so that this
# repository can be configured on its own:
#
#   cmake -S . -B build            # the framework: libraries + tests
#   cmake -S examples -B build/examples   # the demos
#
# The application root adds its own dependency graph before CoralGraphics, and every `add_subdirectory`
# here is guarded by `NOT TARGET`, so including this file in that tree is a no-op.
#
# What comes from where:
#
#   external/CoralCore (or ../CoralCore)   utils, minilog, and the rest of the foundation framework
#   third_party/ in this repository        every library this framework bundles: png, jpeg, webp,
#                                          freetype, harfbuzz, nlohmann/json, SPIRV-Cross, shaderc,
#                                          vkmemalloc, benchmark, and the three shared ones --
#                                          libz, libyuv and libgtest
#
# The three shared libraries are git submodules here so a standalone checkout is self-contained after
#
#   git submodule update --init --recursive
#
# When CoralGraphics is added as a subproject, the application has already added its own libz, libyuv,
# libgtest and CoralCore before this file runs. The `NOT TARGET` guards then skip every bundled copy, so
# the application's versions are the ones the framework links. `CORAL_SHARED_THIRD_PARTY_DIR` remains an
# explicit override for a tree that keeps them outside this repository.

include_guard(GLOBAL)

get_filename_component(CORALGRAPHICS_REPO_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# ==================================================================================================
# Feature switches
# ==================================================================================================
# Declared here as well as in the root CMakeLists so that a project which includes this file before
# adding the framework (the examples project) configures the same feature set. `option()` keeps a value
# the caller supplied.
option(CORAL_GRAPHICS_SUPPORTS_JPEG "Build the JPEG codec" ON)
option(CORAL_GRAPHICS_SUPPORTS_WEBP "Build the WebP codec" ON)
option(CORAL_GRAPHICS_SUPPORTS_FONTS "Build the font stack" ON)
option(CORAL_GRAPHICS_SUPPORTS_JSON "Build the JSON/Lottie reader" ON)
option(CORAL_BACKEND_SUPPORTS_OPENGL "Build the OpenGL backend" ON)
option(CORAL_BACKEND_SUPPORTS_VULKAN "Build the Vulkan backend" ON)
option(CORAL_BACKEND_SUPPORTS_SPIRV_CROSS "Build SPIRV-Cross" ON)
option(CORAL_BACKEND_SUPPORTS_SHADERC "Build shaderc" ON)
option(CORAL_SHADERC_SYNC_DEPS "Run shaderc git-sync-deps when its nested third_party tree is absent" ON)
option(CORAL_GRAPHICS_WITH_EFFECT "Build the effect render-graph module" ON)

# The Lottie reader's no-JSON branch is otherwise never compiled, because the wrapper is always present.
option(CORAL_GRAPHICS_WITHOUT_JSON "Compile the Lottie reader's no-JSON fallback instead of the real parser" OFF)

# ==================================================================================================
# Platform switches
# ==================================================================================================
# The application root sets these; a standalone configure has to set the same ones, because the effect
# platform targets and several backend source lists are selected with them. Setting them here means the
# framework root and the examples project agree without either repeating the rule.
if (UNIX AND NOT APPLE AND NOT ANDROID AND NOT WASM)
  set(LINUX TRUE)
else ()
  set(LINUX FALSE)
endif ()

if (NOT DEFINED IS_HOST_PLATFORM)
  set(IS_HOST_PLATFORM TRUE)
endif ()

# ==================================================================================================
# The shared dependencies bundled with this repository
# ==================================================================================================
# libz, libyuv and libgtest are shared with the other frameworks, and this repository carries its own
# pinned copies as git submodules under third_party/, so a standalone configure needs no network access
# beyond initializing them:
#
#   git submodule update --init --recursive
#
# When CoralGraphics is added as a subproject, the application has already added these three libraries
# (and CoralCore) before this file runs. Every `add_subdirectory` below is guarded by `NOT TARGET`, so
# the application's copies win and the bundled ones are not added at all. `CORAL_SHARED_THIRD_PARTY_DIR`
# remains an explicit override for a tree that keeps them elsewhere.

# ==================================================================================================
# CoralCore
# ==================================================================================================
# utils and minilog come from the foundation framework. A standalone checkout carries it as the
# `external/CoralCore` submodule; a workgroup checkout keeps it beside this repository. The directory is
# resolved here but added after GoogleTest, so that this repository's bundled gtest wins over
# CoralCore's copy (CoralCore's CMakeLists is guarded by `NOT TARGET gtest` too).
set(_coral_core_dir "")
if (NOT TARGET utils)
  if (DEFINED CORAL_CORE_DIR AND EXISTS "${CORAL_CORE_DIR}/CMakeLists.txt")
    set(_coral_core_dir "${CORAL_CORE_DIR}")
  elseif (EXISTS "${CORALGRAPHICS_REPO_DIR}/external/CoralCore/CMakeLists.txt")
    set(_coral_core_dir "${CORALGRAPHICS_REPO_DIR}/external/CoralCore")
  elseif (EXISTS "${CORALGRAPHICS_REPO_DIR}/../CoralCore/CMakeLists.txt")
    set(_coral_core_dir "${CORALGRAPHICS_REPO_DIR}/../CoralCore")
  else ()
    message(FATAL_ERROR
        "CoralGraphics needs CoralCore for its `utils` and `minilog` targets. Initialize the submodule "
        "with 'git submodule update --init external/CoralCore', keep a sibling ../CoralCore checkout, "
        "pass -DCORAL_CORE_DIR=<path>, or add the CoralCore targets before CoralGraphics.")
  endif ()
endif ()

# ==================================================================================================
# GoogleTest
# ==================================================================================================
# Only the test suites name it. This repository's third_party/libgtest is the default, so the framework
# does not depend on CoralCore's copy of it. An explicit CORAL_GTEST_DIR or a shared tree wins, and
# CoralCore's copy is the last fallback for a checkout whose bundled submodule is missing.
if (CORAL_BUILD_TESTS AND NOT TARGET gtest)
  if (DEFINED CORAL_GTEST_DIR AND EXISTS "${CORAL_GTEST_DIR}/CMakeLists.txt")
    add_subdirectory("${CORAL_GTEST_DIR}" "${CMAKE_BINARY_DIR}/gtest")
  elseif (EXISTS "${CORAL_SHARED_THIRD_PARTY_DIR}/libgtest/coral/CMakeLists.txt")
    add_subdirectory("${CORAL_SHARED_THIRD_PARTY_DIR}/libgtest/coral" "${CMAKE_BINARY_DIR}/gtest")
  elseif (EXISTS "${CORALGRAPHICS_REPO_DIR}/third_party/libgtest/coral/CMakeLists.txt")
    add_subdirectory("${CORALGRAPHICS_REPO_DIR}/third_party/libgtest/coral" "${CMAKE_BINARY_DIR}/gtest")
  elseif (DEFINED _coral_core_dir AND EXISTS "${_coral_core_dir}/third_party/libgtest/coral/CMakeLists.txt")
    add_subdirectory("${_coral_core_dir}/third_party/libgtest/coral" "${CMAKE_BINARY_DIR}/gtest")
  endif ()

  if (NOT TARGET gtest)
    message(FATAL_ERROR
        "CoralGraphics tests need GoogleTest. Initialize this repository's submodules with "
        "'git submodule update --init --recursive', pass -DCORAL_GTEST_DIR=<path>, point "
        "-DCORAL_SHARED_THIRD_PARTY_DIR at a tree with libgtest, or configure with "
        "-DCORAL_BUILD_TESTS=OFF.")
  endif ()
endif ()

# ==================================================================================================
# Adding CoralCore
# ==================================================================================================
if (NOT TARGET utils)
  add_subdirectory("${_coral_core_dir}" "${CMAKE_BINARY_DIR}/coral-core")
endif ()

# ==================================================================================================
# libz and libyuv
# ==================================================================================================
# libz is libpng's dependency and libyuv is what the effect framework's YUV input nodes convert with.
# Each comes from this repository's bundled submodule, or from an explicit shared tree.
if (NOT TARGET z)
  if (EXISTS "${CORAL_SHARED_THIRD_PARTY_DIR}/libz/coral/CMakeLists.txt")
    add_subdirectory("${CORAL_SHARED_THIRD_PARTY_DIR}/libz/coral" "${CMAKE_BINARY_DIR}/libz")
  elseif (EXISTS "${CORALGRAPHICS_REPO_DIR}/third_party/libz/coral/CMakeLists.txt")
    add_subdirectory("${CORALGRAPHICS_REPO_DIR}/third_party/libz/coral" "${CMAKE_BINARY_DIR}/libz")
  endif ()

  if (NOT TARGET z)
    message(FATAL_ERROR
        "CoralGraphics needs the zlib wrapper (target `z`). Initialize this repository's submodules "
        "with 'git submodule update --init --recursive', pass "
        "-DCORAL_SHARED_THIRD_PARTY_DIR=<application third_party>, or add the zlib target before "
        "CoralGraphics.")
  endif ()
endif ()

if (CORAL_GRAPHICS_WITH_EFFECT AND NOT TARGET yuv)
  if (EXISTS "${CORAL_SHARED_THIRD_PARTY_DIR}/libyuv/CMakeLists.txt")
    add_subdirectory("${CORAL_SHARED_THIRD_PARTY_DIR}/libyuv" "${CMAKE_BINARY_DIR}/libyuv")
    set(_coral_yuv_include "${CORAL_SHARED_THIRD_PARTY_DIR}/libyuv/include")
  elseif (EXISTS "${CORALGRAPHICS_REPO_DIR}/third_party/libyuv/CMakeLists.txt")
    add_subdirectory("${CORALGRAPHICS_REPO_DIR}/third_party/libyuv" "${CMAKE_BINARY_DIR}/libyuv")
    set(_coral_yuv_include "${CORALGRAPHICS_REPO_DIR}/third_party/libyuv/include")
  endif ()

  if (NOT TARGET yuv)
    message(FATAL_ERROR
        "CoralGraphics' effect module needs libyuv (target `yuv`). Initialize this repository's "
        "submodules with 'git submodule update --init --recursive', pass "
        "-DCORAL_SHARED_THIRD_PARTY_DIR=<application third_party>, add the yuv target before "
        "CoralGraphics, or configure with -DCORAL_GRAPHICS_WITH_EFFECT=OFF.")
  endif ()

  # libyuv's own CMake lists its include directory at directory scope, which reaches the `yuv` target
  # but not the targets that link it. The effect module includes <libyuv/convert_argb.h> in its own
  # translation units, so the directory has to be part of the target's usage requirements.
  target_include_directories(yuv PUBLIC "${_coral_yuv_include}")
endif ()

# ==================================================================================================
# The graphics-only third-party libraries
# ==================================================================================================
# Everything below is vendored in this repository. `png` is the sentinel: when the application root has
# already added this graph, every target below exists and the whole section does nothing.
set(_coral_vendor "${CORALGRAPHICS_REPO_DIR}/third_party")

if (NOT TARGET png)
  if (NOT EXISTS "${_coral_vendor}/libpng/coral/CMakeLists.txt")
    message(FATAL_ERROR
        "CoralGraphics needs its vendored third-party tree at ${_coral_vendor}. Initialize the "
        "third_party submodules, or add the application dependency graph before CoralGraphics.")
  endif ()

  # The benchmark library is only linked by `effect_hdr_benchmark`, which the test build produces.
  if (CORAL_BUILD_TESTS AND NOT TARGET benchmark)
    add_subdirectory("${_coral_vendor}/benchmark/coral" "${CMAKE_BINARY_DIR}/benchmark")
  endif ()

  add_subdirectory("${_coral_vendor}/libpng/coral" "${CMAKE_BINARY_DIR}/libpng")

  if (CORAL_GRAPHICS_SUPPORTS_JPEG AND EXISTS "${_coral_vendor}/libjpeg-turbo/coral/CMakeLists.txt")
    add_subdirectory("${_coral_vendor}/libjpeg-turbo/coral" "${CMAKE_BINARY_DIR}/libjpeg-turbo")
  elseif (CORAL_GRAPHICS_SUPPORTS_JPEG)
    message(STATUS "CoralGraphics: libjpeg-turbo is not vendored; JPEG support is disabled")
    set(CORAL_GRAPHICS_SUPPORTS_JPEG OFF)
  endif ()

  if (CORAL_GRAPHICS_SUPPORTS_WEBP AND EXISTS "${_coral_vendor}/libwebp/coral/CMakeLists.txt")
    add_subdirectory("${_coral_vendor}/libwebp/coral" "${CMAKE_BINARY_DIR}/libwebp")
  elseif (CORAL_GRAPHICS_SUPPORTS_WEBP)
    message(STATUS "CoralGraphics: libwebp is not vendored; WebP support is disabled")
    set(CORAL_GRAPHICS_SUPPORTS_WEBP OFF)
  endif ()

  # freetype loads the font and harfbuzz shapes the text. The font API needs both, so one switch turns
  # them on together; each reports its own absence.
  if (CORAL_GRAPHICS_SUPPORTS_FONTS AND EXISTS "${_coral_vendor}/freetype/CMakeLists.txt")
    # This tree does not install FreeType, and its export set cannot name the vendored `png` target
    # that the colour-emoji shim links.
    set(SKIP_INSTALL_ALL ON)
    add_subdirectory("${_coral_vendor}/freetype" "${CMAKE_BINARY_DIR}/freetype")

    # FreeType's own CMake only enables the PNG shim when it finds a system libpng at configure time;
    # this tree already builds the vendored one, so hand it over explicitly. Without this,
    # NotoColorEmoji loads and every glyph fails with `FT_Err_Unimplemented_Feature`.
    if (TARGET freetype AND TARGET png)
      target_compile_definitions(freetype PRIVATE FT_CONFIG_OPTION_USE_PNG=1)
      target_include_directories(freetype PRIVATE "${_coral_vendor}/libpng")
      target_link_libraries(freetype PRIVATE png)
    endif ()
  elseif (CORAL_GRAPHICS_SUPPORTS_FONTS)
    message(STATUS "CoralGraphics: FreeType is not vendored; the font stack is disabled")
  endif ()

  if (CORAL_GRAPHICS_SUPPORTS_FONTS AND EXISTS "${_coral_vendor}/harfbuzz/coral/CMakeLists.txt")
    add_subdirectory("${_coral_vendor}/harfbuzz/coral" "${CMAKE_BINARY_DIR}/harfbuzz")
  elseif (CORAL_GRAPHICS_SUPPORTS_FONTS)
    message(STATUS "CoralGraphics: harfbuzz is not vendored; the font stack is disabled")
    set(CORAL_GRAPHICS_SUPPORTS_FONTS OFF)
  endif ()

  # nlohmann/json is header-only; the target is what the Lottie translation names, so a build without
  # it compiles `LottieParserUnavailable.cpp` instead.
  if (CORAL_GRAPHICS_SUPPORTS_JSON AND NOT CORAL_GRAPHICS_WITHOUT_JSON
      AND EXISTS "${_coral_vendor}/json/include/nlohmann/json.hpp")
    add_subdirectory("${_coral_vendor}/json/coral" "${CMAKE_BINARY_DIR}/json")
  else ()
    set(CORAL_GRAPHICS_SUPPORTS_JSON OFF)
  endif ()

  # SPIRV-Cross is what lets the OpenGL backend consume SPIR-V shaders. Honoured as given: an OFF
  # configuration is one this tree has to be able to build, so nothing is forced back on here.
  if (CORAL_BACKEND_SUPPORTS_SPIRV_CROSS AND EXISTS "${_coral_vendor}/SPIRV-Cross/spirv_glsl.cpp")
    add_subdirectory("${_coral_vendor}/SPIRV-Cross/coral" "${CMAKE_BINARY_DIR}/spirv-cross")
  elseif (CORAL_BACKEND_SUPPORTS_SPIRV_CROSS)
    message(STATUS "CoralGraphics: SPIRV-Cross was requested but is not vendored; GLSLDirect is the only shader path")
    set(CORAL_BACKEND_SUPPORTS_SPIRV_CROSS OFF)
  endif ()

  # shaderc compiles GLSL source to SPIR-V. The Vulkan backend cannot work without it, which the backend
  # target itself reports; a GL-only build can.
  if (CORAL_BACKEND_SUPPORTS_SHADERC AND EXISTS "${_coral_vendor}/shaderc/libshaderc/src/shaderc.cc")
    include("${CMAKE_CURRENT_LIST_DIR}/SyncShadercDeps.cmake")
    coral_sync_shaderc_deps("${_coral_vendor}/shaderc" _coral_shaderc_synced)
    if (NOT _coral_shaderc_synced)
      message(STATUS "CoralGraphics: shaderc dependencies are unavailable; shader modules must be supplied as SPIR-V")
      set(CORAL_BACKEND_SUPPORTS_SHADERC OFF)
    endif ()
  endif ()

  if (CORAL_BACKEND_SUPPORTS_SHADERC AND EXISTS "${_coral_vendor}/shaderc/libshaderc/src/shaderc.cc")
    add_subdirectory("${_coral_vendor}/shaderc/coral" "${CMAKE_BINARY_DIR}/shaderc")
  elseif (CORAL_BACKEND_SUPPORTS_SHADERC)
    message(STATUS "CoralGraphics: shaderc was requested but is not vendored; shader modules must be supplied as SPIR-V")
    set(CORAL_BACKEND_SUPPORTS_SHADERC OFF)
  endif ()

  # VMA is header-only apart from one translation unit; the wrapper target carries its include path.
  # The Vulkan headers are used directly from third_party/vkheaders by the backend target.
  if (CORAL_BACKEND_SUPPORTS_VULKAN AND EXISTS "${_coral_vendor}/vkmemalloc/include/vk_mem_alloc.h")
    add_subdirectory("${_coral_vendor}/vkmemalloc/coral" "${CMAKE_BINARY_DIR}/vkmemalloc")
  endif ()
endif ()
