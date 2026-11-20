cmake_minimum_required(VERSION 3.19)
project(shaderc-coral)

# ==================================================================================================
# shaderc
# ==================================================================================================
#
# shaderc is vendored under third_party/shaderc and wrapped here so that Coral gets a `shaderc` target
# and nothing else. What Coral needs from it is narrow: compile a GLSL string to SPIR-V through the
# libshaderc C++ API, so that shader sources can be authored as ESSL 1.00 or ESSL 3.00 and still reach
# every backend.
#
# ## Why add_subdirectory and not a source list
#
# SPIRV-Cross is wrapped by listing its sources, because it is five files at the root of a tree with no
# meaningful build system of its own. shaderc is the opposite: libshaderc depends on glslang, which
# depends on SPIRV-Tools, which depends on SPIRV-Headers, and glslang additionally runs code generators
# whose output is compiled in. Reproducing that graph by hand would be a standing maintenance liability
# every time shaderc is re-vendored, and getting it subtly wrong fails as a link error inside a
# generated file.
#
# So this wrapper drives shaderc's own CMake with its options pinned to the minimum. The cost is that
# `add_subdirectory` also creates targets Coral never links - glslc, the spirv-* tools, the optimisation
# and reducer libraries. That is a build-tree cost, not a shipped one: only `shaderc` is on any link
# line, and the rest is simply not built when a target is requested directly.

get_filename_component(SHADERC_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if (NOT EXISTS "${SHADERC_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
      "shaderc is not vendored at ${SHADERC_DIR}. Re-vendor third_party/shaderc, or configure with "
      "-DCORAL_BACKEND_SUPPORTS_SHADERC=OFF to build without GLSL source compilation.")
endif ()

# shaderc, glslang and SPIRV-Tools each declare cmake_minimum_required(VERSION 3.22.1). Entering them
# from a project configured with the Coral root's floor of 3.19 aborts with "CMake 3.22.1 or higher is
# required", which would be a confusing way to learn it. Fail with the reason instead.
if (CMAKE_VERSION VERSION_LESS 3.22.1)
  message(FATAL_ERROR
      "shaderc requires CMake 3.22.1 or newer (this is ${CMAKE_VERSION}). Either upgrade CMake, or "
      "configure with -DCORAL_BACKEND_SUPPORTS_SHADERC=OFF to build without GLSL source compilation.")
endif ()

# ==================================================================================================
# What to build
# ==================================================================================================
# shaderc's options are declared with `option(<name> <doc> ${<name>})` and read before anything else in
# its tree, which is the pattern CMP0077 was added for: adding these as normal cache entries lets the
# value win over the default without leaking a `CACHE ... FORCE` write into Coral's own cache.

set(SHADERC_SKIP_TESTS ON CACHE BOOL "shaderc: skip tests" FORCE)
set(SHADERC_SKIP_EXAMPLES ON CACHE BOOL "shaderc: skip examples" FORCE)
set(SHADERC_SKIP_COPYRIGHT_CHECK ON CACHE BOOL "shaderc: skip the copyright check target" FORCE)
set(SHADERC_ENABLE_WGSL_OUTPUT OFF CACHE BOOL "shaderc: no WGSL output" FORCE)

# shaderc 2025.2 compiles clean under its own CI, not necessarily under a newer MSVC or a warnings-as-
# errors configuration Coral may have set. A warning in vendored code must not fail Coral's build.
set(SHADERC_ENABLE_WERROR_COMPILE OFF CACHE BOOL "shaderc: do not fail on warnings" FORCE)

# The CRT has to match Coral's, because a shaderc static library and a Coral translation unit end up in
# the same process and both allocate from the same heap. shaderc defaults to the static CRT, which is
# also Coral's default, but `-DUSE_STATIC_CRT=OFF` is a supported Coral configuration and this is the
# line that keeps it working.
if (MSVC)
  if (USE_STATIC_CRT)
    set(SHADERC_ENABLE_SHARED_CRT OFF CACHE BOOL "shaderc: match Coral's static CRT" FORCE)
  else ()
    set(SHADERC_ENABLE_SHARED_CRT ON CACHE BOOL "shaderc: match Coral's shared CRT" FORCE)
  endif ()
endif ()

# `ENABLE_OPT=OFF` is load-bearing rather than an optimisation, and it is the reason this wrapper exists
# in the shape it does. glslang attaches SPIRV-Tools-opt to its `SPIRV` target whenever optimisation is
# on (third_party/glslang/SPIRV/CMakeLists.txt:115), and glslang declares an unconditional
# `install(EXPORT glslang-targets)` that then references a target outside the export set. CMake rejects
# that at generate time - "requires target SPIRV-Tools-opt that is not in any export set" - and no
# install-skipping option prevents it, because the export rule is not guarded by one.
#
# Coral does not want optimised SPIR-V here anyway. The reason to feed this path GLSL at all is that a
# debugger should be able to map the SPIR-V back to what was written, and spirv-opt is exactly the pass
# that destroys that correspondence.
set(ENABLE_OPT OFF CACHE BOOL "glslang: no spirv-opt; see the comment above" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "glslang: no command line tools" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "glslang: no spirv-remap" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "glslang: no tests" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "glslang: nothing to install" FORCE)
set(ENABLE_PCH OFF CACHE BOOL "glslang: no precompiled header, which fights Coral's global flags" FORCE)

# SPIRV-Tools. Only the core and optimiser libraries are reachable from libshaderc; the tools, tests,
# fuzzers and reducer are not.
set(SPIRV_SKIP_TESTS ON CACHE BOOL "SPIRV-Tools: no tests" FORCE)
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "SPIRV-Tools: no command line tools" FORCE)
set(SPIRV_WERROR OFF CACHE BOOL "SPIRV-Tools: do not fail on warnings" FORCE)
set(SPIRV_TOOLS_BUILD_STATIC ON CACHE BOOL "SPIRV-Tools: static library" FORCE)

# A `-DBUILD_SHARED_LIBS=ON` from a user must not turn glslang and SPIRV-Tools into DLLs underneath a
# static shaderc.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "vendored: static libraries only" FORCE)

# glslc is a separate executable that shaderc's CMakeLists always declares, and `shaderc_shared` and
# `shaderc_combined` are always declared too. None is built unless asked for, and Coral never asks.
add_subdirectory("${SHADERC_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/shaderc" EXCLUDE_FROM_ALL)

if (NOT TARGET shaderc)
  message(FATAL_ERROR
      "shaderc's CMakeLists did not create a `shaderc` target. The vendored copy may be incomplete; "
      "re-vendor third_party/shaderc, or configure with -DCORAL_BACKEND_SUPPORTS_SHADERC=OFF.")
endif ()

# shaderc's own CMakeLists sets CMAKE_CXX_STANDARD 17 in its directory scope, which does not propagate
# upward, but which also does not survive onto the target as a readable property. Restating it makes the
# dialect explicit and stops a future shaderc release from silently changing it.
set_target_properties(shaderc PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    FOLDER ThirdParty
    POSITION_INDEPENDENT_CODE ON
)

# Coral consumes shaderc as <shaderc/shaderc.hpp>, so the include directory belongs to the interface.
# The SPIRV-Headers path is here because `shaderc.h` includes `spirv/unified1/spirv.hpp`.
target_include_directories(shaderc PUBLIC
    "${SHADERC_DIR}/libshaderc/include"
    "${SHADERC_DIR}/third_party/spirv-headers/include"
)

if (MSVC)
  # Vendored code is not Coral's to fix; its warnings must not become Coral's problems.
  target_compile_options(shaderc PRIVATE /W0)
endif ()

install(TARGETS shaderc ARCHIVE DESTINATION lib)
