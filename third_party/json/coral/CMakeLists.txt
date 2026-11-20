cmake_minimum_required(VERSION 3.19)
project(nlohmann_json)

# ==================================================================================================
# nlohmann/json
# ==================================================================================================
# A JSON reader and writer, used by the Skottie translation to read a Lottie document.
#
# It is header-only, so this is an INTERFACE library rather than the static one every other wrapper in
# `third_party/` builds. The `include/` tree is the modular form rather than `single_include/`: it is the
# same version and the same code, and 34 small headers compile incrementally where one 986 KB header
# recompiles every translation unit that touches it.
#
# Consumers spell the include as <nlohmann/json.hpp>, which is what the upstream documentation and every
# example use, so that is the directory exposed rather than anything renamed.

set(TARGET nlohmann_json)

set(PUBLIC_HDR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../include)

# Listed rather than globbed, so that a header missing from the vendored copy is a configure error here
# instead of a compile error somewhere that has no reason to know about it.
set(PUBLIC_HDRS
    ${PUBLIC_HDR_DIR}/nlohmann/json.hpp
    ${PUBLIC_HDR_DIR}/nlohmann/json_fwd.hpp
    ${PUBLIC_HDR_DIR}/nlohmann/adl_serializer.hpp
    ${PUBLIC_HDR_DIR}/nlohmann/byte_container_with_subtype.hpp
    ${PUBLIC_HDR_DIR}/nlohmann/ordered_map.hpp
)

foreach (header ${PUBLIC_HDRS})
  if (NOT EXISTS ${header})
    message(FATAL_ERROR "nlohmann/json is incomplete: ${header} is missing")
  endif ()
endforeach ()

add_library(${TARGET} INTERFACE)

# The headers are attached to the interface as well as the include directory, so that an IDE opened on
# this tree shows them under the target rather than leaving it looking empty.
target_sources(${TARGET} INTERFACE ${PUBLIC_HDRS})
target_include_directories(${TARGET} INTERFACE ${PUBLIC_HDR_DIR})

set_target_properties(${TARGET} PROPERTIES FOLDER ThirdParty)
