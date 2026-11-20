cmake_minimum_required(VERSION 3.19)
project(webp-coral)

# ==================================================================================================
# libwebp
# ==================================================================================================
#
# The WebP half of the image codec. Vendored under third_party/libwebp; this wrapper pins its options
# so Coral gets the decoder and the encoder and nothing else -- no `cwebp`, no `dwebp`, no `vwebp`, no
# GIF conversion tools, no muxer and no demuxer.
#
# ## Which targets exist
#
# libwebp splits itself into object libraries -- `webpdecode`, `webpdspdecode`, `webpencode`,
# `webpdspencode`, `webputils` and so on -- and then assembles `webp`, `webpdecoder` and
# `webpdemux` from them. The object libraries exist so that a build which only decodes does not link
# the encoder, which is worth having: WebP encoding is several times the size of decoding, and a
# viewer that only shows WebP files should not carry it.
#
# Coral wants both directions, so it links `webp`, which is the combined static library.
#
# ## Why `WEBP_LINK_STATIC`
#
# libwebp links `webp` and `webpdsp` together and by default prefers the shared forms. Coral's whole
# vendoring convention is static, and a shared libwebp next to a system one is the same trap libjpeg
# has.

get_filename_component(WEBP_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if (NOT EXISTS "${WEBP_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
      "libwebp is not vendored at ${WEBP_DIR}. Re-vendor third_party/libwebp, or configure with "
      "-DCORAL_GRAPHICS_SUPPORTS_WEBP=OFF to build without WebP support.")
endif ()

# Static against the in-tree libraries.
set(WEBP_LINK_STATIC ON CACHE BOOL "vendored: link the static libraries" FORCE)

# Every command line tool and the muxer. Coral reads and writes single images; it has no use for an
# animation muxer or a viewer, and each one is another executable in the build graph.
set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "vendored: no animation tools" FORCE)
set(WEBP_BUILD_CWEBP OFF CACHE BOOL "vendored: no command line tools" FORCE)
set(WEBP_BUILD_DWEBP OFF CACHE BOOL "vendored: no command line tools" FORCE)
set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL "vendored: no command line tools" FORCE)
set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL "vendored: no command line tools" FORCE)
set(WEBP_BUILD_VWEBP OFF CACHE BOOL "vendored: no command line tools" FORCE)
set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL "vendored: no command line tools" FORCE)
set(WEBP_BUILD_WEBPMUX OFF CACHE BOOL "vendored: no command line tools" FORCE)
set(WEBP_BUILD_LIBWEBPMUX OFF CACHE BOOL "vendored: no muxer" FORCE)
set(WEBP_BUILD_EXTRAS OFF CACHE BOOL "vendored: no extra tools" FORCE)
set(WEBP_BUILD_WEBP_JS OFF CACHE BOOL "vendored: no Emscripten build" FORCE)

# SIMD, for the same reason as libjpeg-turbo: a large speedup that degrades to the portable path on a
# platform that lacks it rather than failing to configure.
set(WEBP_ENABLE_SIMD ON CACHE BOOL "vendored: SIMD where available" FORCE)

add_subdirectory("${WEBP_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/libwebp" EXCLUDE_FROM_ALL)

if (NOT TARGET webp)
  message(FATAL_ERROR
      "libwebp's CMakeLists did not create a `webp` target. The vendored copy may be incomplete; "
      "re-vendor third_party/libwebp or configure with -DCORAL_GRAPHICS_SUPPORTS_WEBP=OFF.")
endif ()

set_target_properties(webp PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    FOLDER ThirdParty
    POSITION_INDEPENDENT_CODE ON
)

# No `target_include_directories` here, for the same reason as libjpeg-turbo: libwebp exports its
# targets and CMake refuses an export whose interface names a build-tree path. The consumer attaches
# the include directory instead.

if (MSVC)
  target_compile_options(webp PRIVATE /W0)
endif ()

# `PUBLIC_HEADER DESTINATION` is required here, and its absence was a warning on every configure:
#
#   Target webp has PUBLIC_HEADER files but no PUBLIC_HEADER DESTINATION.
#
# The `webp` target carries `PUBLIC_HEADER` (upstream sets it at `libwebp/CMakeLists.txt`), and CMake expects an
# `install(TARGETS)` that names the target to say where those files go. Omitting it is not fatal -- the ARCHIVE rule
# still runs -- but CMake emits a developer warning, and the two sibling wrappers that install a target with public
# headers (`harfbuzz`, `SPIRV-Cross`) both give the destination.
#
# Nothing is lost by naming it, and it is where libwebp's own install rules put the same headers.
install(TARGETS webp
    ARCHIVE DESTINATION lib
    PUBLIC_HEADER DESTINATION include/webp)
