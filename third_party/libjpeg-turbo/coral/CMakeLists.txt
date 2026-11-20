cmake_minimum_required(VERSION 3.19)
project(jpeg-coral)

# ==================================================================================================
# libjpeg-turbo
# ==================================================================================================
#
# The JPEG half of the image codec. Vendored under third_party/libjpeg-turbo; this wrapper pins its
# options so Coral gets a static v6b-ABI library and nothing else -- no shared object, no TurboJPEG, no
# Java bindings, no command line tools and no tests.
#
# ## Why the v6b ABI matters
#
# libjpeg-turbo can emulate the v7 and v8 APIs, which change the shape of the public structs. Coral
# does not use those, and every other consumer of libjpeg in an application expects the v6b layout --
# so matching the default is what keeps a future third-party addition from finding a library that
# looks like libjpeg and does not have libjpeg's ABI.
#
# ## Why in-memory source and destination
#
# `WITH_MEM_SRCDST` is required rather than optional here: the codec decodes from a byte buffer that
# came from a file, a network response or an asset, and never from a `FILE*`. Without it there is no
# way to hand JPEG a buffer at all.

get_filename_component(JPEG_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if (NOT EXISTS "${JPEG_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
      "libjpeg-turbo is not vendored at ${JPEG_DIR}. Re-vendor third_party/libjpeg-turbo, or configure "
      "with -DCORAL_GRAPHICS_SUPPORTS_JPEG=OFF to build without JPEG support.")
endif ()

# Static only. A shared libjpeg in an application that also links a system one is the classic source of
# a decoder that works on the developer's machine and not on a user's.
set(ENABLE_SHARED OFF CACHE BOOL "vendored: static libraries only" FORCE)
set(ENABLE_STATIC ON CACHE BOOL "vendored: static libraries only" FORCE)

# The TurboJPEG API, the Java bindings, the fuzz targets and the tools are all things Coral does not
# use and would have to keep compiling.
set(WITH_TURBOJPEG OFF CACHE BOOL "vendored: the libjpeg API is enough" FORCE)
set(WITH_JAVA OFF CACHE BOOL "vendored: no Java bindings" FORCE)
set(WITH_FUZZ OFF CACHE BOOL "vendored: no fuzz targets" FORCE)

# 8-bit samples, which is what an 8-bit-per-channel image is. 12-bit is a different library ABI and a
# different set of structs, so it cannot be added later without breaking callers.
set(WITH_12BIT OFF CACHE BOOL "vendored: 8-bit samples only" FORCE)

# v6b, as discussed above.
set(WITH_JPEG7 OFF CACHE BOOL "vendored: the v6b ABI" FORCE)
set(WITH_JPEG8 OFF CACHE BOOL "vendored: the v6b ABI" FORCE)

# The in-memory source and destination managers, which the codec needs to decode from a buffer.
set(WITH_MEM_SRCDST ON CACHE BOOL "vendored: decode from a buffer, not a FILE" FORCE)

# SIMD speeds decoding up materially and libjpeg-turbo's SIMD is well tested. It is not forced, because
# an unsupported platform falls back gracefully and `REQUIRE_SIMD` would turn that fallback into a
# configure error.
set(WITH_SIMD ON CACHE BOOL "vendored: SIMD where available" FORCE)
set(REQUIRE_SIMD OFF CACHE BOOL "vendored: fall back when SIMD is unavailable" FORCE)

# Coral links its own runtime, chosen globally; a subproject choosing the other one produces the
# mismatched-CRT link error that is otherwise very hard to read.
if (MSVC AND USE_STATIC_CRT)
  set(WITH_CRT_DLL OFF CACHE BOOL "vendored: match Coral's static CRT" FORCE)
endif ()

# libjpeg-turbo's install rules use GNUInstallDirs' variables (`CMAKE_INSTALL_DOCDIR` among them) but
# never includes that module, relying on its own top-level build to have done so. Entered as a
# subdirectory from a project that has not, the configure fails with "install FILES given no
# DESTINATION". Including it here defines the whole set, which is more robust than defining the one
# variable whose absence happens to be fatal today.
include(GNUInstallDirs)

# libjpeg-turbo declares `cmake_minimum_required(VERSION 2.8.12)`, and CMake 4 removed compatibility
# with anything below 3.5 -- it fails the configure outright with "Compatibility with CMake < 3.5 has
# been removed". Setting this floor is the documented way to configure such a project without editing
# its CMakeLists, which matters because the file is vendored and any local edit is lost on the next
# re-vendor. It only raises the *policy* floor; libjpeg-turbo's own minimum is left alone.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

add_subdirectory("${JPEG_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/libjpeg-turbo" EXCLUDE_FROM_ALL)

if (NOT TARGET jpeg-static)
  message(FATAL_ERROR
      "libjpeg-turbo's CMakeLists did not create a `jpeg-static` target. The vendored copy may be "
      "incomplete; re-vendor third_party/libjpeg-turbo or configure with -DCORAL_GRAPHICS_SUPPORTS_JPEG=OFF.")
endif ()

set_target_properties(jpeg-static PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    FOLDER ThirdParty
    POSITION_INDEPENDENT_CODE ON
)

# No `target_include_directories` here on purpose.
#
# libjpeg-turbo declares an unconditional `install(EXPORT libjpeg-turbo-targets)`, and CMake rejects an
# exported target whose INTERFACE_INCLUDE_DIRECTORIES contains a build-tree path -- "contains path ...
# which is prefixed in the source directory". The generator-expression form would satisfy the export
# check, but there is nothing to export: Coral installs the library's archive, not a libjpeg CMake
# package. The include directory is therefore attached to the consumer that needs it, which is
# `libs/graphics`, and the same applies to libwebp.

if (MSVC)
  # Vendored code is not Coral's to fix; its warnings must not become Coral's problems.
  target_compile_options(jpeg-static PRIVATE /W0)
endif ()

install(TARGETS jpeg-static ARCHIVE DESTINATION lib)
