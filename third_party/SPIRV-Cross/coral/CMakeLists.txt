cmake_minimum_required(VERSION 3.19)
project(SPIRV-Cross)

# ==================================================================================================
# SPIRV-Cross
# ==================================================================================================
#
# SPIRV-Cross is vendored under third_party/SPIRV-Cross and wrapped here rather than added with
# add_subdirectory(), for three reasons:
#
#  1. Upstream's CMakeLists builds the command line tools, the tests, the C API and every backend
#     (GLSL, MSL, HLSL, CPP, reflection) as separate targets. Coral needs exactly one static library
#     containing the GLSL backend, so building upstream's project wholesale would add minutes to
#     every clean build.
#  2. Upstream sets its own CMAKE_CXX_STANDARD, which leaks into the parent scope and would
#     downgrade Coral's C++20 translation units.
#  3. Upstream is written in an older style that does not warn-clean under Coral's flag set, so its
#     warning configuration must be isolated.
#
# Only the sources below are listed. They are the transitive closure needed by CompilerGLSL:
#   spirv_cross.{cpp,hpp}        - shared parsing, reflection, type system
#   spirv_parsed_ir.cpp          - the parsed IR container
#   spirv_cfg.cpp                - control flow graph, used by the variable hoisting passes
#   spirv_glsl.cpp               - the GLSL/GLSL ES backend
#
# Adding a backend later (Metal, Direct3D 12) means adding its .cpp here: spirv_msl.cpp,
# spirv_hlsl.cpp.

# Only the sources below are listed. They are the transitive closure needed by CompilerGLSL:
#   spirv_cross.cpp                - shared parsing entry points, reflection, type system
#   spirv_cross_parsed_ir.cpp      - the parsed IR container and type helpers
#   spirv_parser.cpp               - the SPIR-V binary parser
#   spirv_cfg.cpp                  - control flow graph, used by the variable hoisting passes
#   spirv_glsl.cpp                 - the GLSL / GLSL ES backend
#
# Adding a backend later (Metal, Direct3D 12) means adding its .cpp here: spirv_msl.cpp,
# spirv_hlsl.cpp.

set(TARGET SPIRV-Cross)

# Resolve the vendored source tree from the wrapper's own location rather than from
# CMAKE_CURRENT_SOURCE_DIR, so this file works both as an add_subdirectory() target of the Coral root
# and from a standalone development build.
get_filename_component(SPIRV_CROSS_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# Both consumers are built: the OpenGL backend needs CompilerGLSL and the Metal backend needs CompilerMSL. The Metal
# backend's MSL backend is the larger of the two by a wide margin, and a GL-only application pays for it in archive size
# rather than in run time -- the linker discards what is not referenced, and the localised MSVC/ninja configuration that
# cannot see header dependencies would otherwise need a second library variant.
set(SRCS
    ${SPIRV_CROSS_DIR}/spirv_cfg.cpp
    ${SPIRV_CROSS_DIR}/spirv_cross.cpp
    ${SPIRV_CROSS_DIR}/spirv_cross_parsed_ir.cpp
    ${SPIRV_CROSS_DIR}/spirv_glsl.cpp
    ${SPIRV_CROSS_DIR}/spirv_msl.cpp
    ${SPIRV_CROSS_DIR}/spirv_parser.cpp
)

# Every source is checked up front so that a partially vendored checkout produces one clear message
# instead of an obscure link error later on.
foreach (SOURCE ${SRCS})
  if (NOT EXISTS "${SOURCE}")
    message(FATAL_ERROR
        "SPIRV-Cross source ${SOURCE} is missing. Re-vendor third_party/SPIRV-Cross, or configure "
        "with -DCORAL_BACKEND_SUPPORTS_SPIRV_CROSS=OFF and supply GLSL ES shader source instead.")
  endif ()
endforeach ()

set(PUBLIC_HDRS
    ${SPIRV_CROSS_DIR}/GLSL.std.450.h
    ${SPIRV_CROSS_DIR}/spirv.hpp
    ${SPIRV_CROSS_DIR}/spirv_cfg.hpp
    ${SPIRV_CROSS_DIR}/spirv_common.hpp
    ${SPIRV_CROSS_DIR}/spirv_cross_containers.hpp
    ${SPIRV_CROSS_DIR}/spirv_cross_error_handling.hpp
    ${SPIRV_CROSS_DIR}/spirv_cross.hpp
    ${SPIRV_CROSS_DIR}/spirv_glsl.hpp
    ${SPIRV_CROSS_DIR}/spirv_msl.hpp
    ${SPIRV_CROSS_DIR}/spirv_parser.hpp
)

add_library(${TARGET} STATIC ${SRCS} ${PUBLIC_HDRS})

# SPIRV-Cross is C++11 era code. Pin its standard so Coral's C++20 setting cannot change its
# semantics, and so the compiler stops warning about the parent's dialect.
set_target_properties(${TARGET} PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    FOLDER ThirdParty
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(${TARGET} PUBLIC ${SPIRV_CROSS_DIR})

# SPIRV-Cross reports malformed input by throwing spirv_cross::CompilerError, and the RHI's wrapper
# catches it in ShaderCross.cpp to turn a corrupt shader asset into Result::ShaderCompilationFailed.
#
# SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS must stay UNDEFINED for that to work: upstream tests it with
# `#ifdef`, not `#if`, so even defining it as 0 would select the abort() path and make a malformed
# shader kill the process instead of returning an error.
if (MSVC)
  target_compile_options(${TARGET} PRIVATE /W0 /EHsc)
else ()
  # -w suppresses upstream's warnings without affecting Coral's own sources.
  target_compile_options(${TARGET} PRIVATE
      -w
      -fexceptions
  )
endif ()

if (LINUX OR ANDROID)
  target_compile_options(${TARGET} PRIVATE -fPIC)
endif ()

install(TARGETS ${TARGET} ARCHIVE DESTINATION lib)
