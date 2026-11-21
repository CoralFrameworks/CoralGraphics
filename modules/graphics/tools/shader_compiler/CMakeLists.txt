cmake_minimum_required(VERSION 3.19)
project(coral_shader_compiler CXX)

set(TARGET shader_compiler)

# ==================================================================================================
# The tool
# ==================================================================================================
#
# ## Why it links `backend` rather than carrying its own copy of the settings
#
# The settings that matter here - the target environment, the SPIR-V version, the version retarget that
# glslang's floor forces on ES 3.00 source - live in `GlslCompiler.cpp`. `backend::compileGlslToSpirv`
# is the one function that applies them.
#
# A tool that restated them would drift, and the drift would be invisible: both outputs would be valid
# SPIR-V, both shaders would render, and the two paths would disagree only in the cases nobody tests.
# Linking the library this tool is a front end *for* makes the settings identical by construction.
# The cost is that the tool links a library that can open a GL context and does not; that is a few
# hundred kilobytes of unreferenced objects the linker discards.
#
# ## The private header
#
# `GlslCompiler.h` is deliberately not a public header - it is `backend`'s business, not a consumer's.
# A build tool in the same repository reading it is a different relationship from an SDK consumer
# reading it, and the alternative - publishing the compiler entry point - would widen `backend`'s
# public surface to serve a build-time caller.

add_executable(${TARGET} shader_compiler.cpp)

target_include_directories(${TARGET}
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../backend/src"
)

if (NOT TARGET backend)
  message(FATAL_ERROR
      "shader_compiler needs the `backend` target, which modules/graphics defines. Add "
      "`add_subdirectory(backend)` before `add_subdirectory(tools/shader_compiler)` in the root "
      "CMakeLists, or configure with -DCORAL_BACKEND_SUPPORTS_SHADERC=OFF to skip the tool entirely.")
endif ()

target_link_libraries(${TARGET} PRIVATE backend)

set_target_properties(${TARGET} PROPERTIES FOLDER Tools)

# The `tools` component lets the prebuild driver install only this executable, without building (or
# installing) the rest of the tree it links against:
#
#   cmake --install <build> --component tools
install(TARGETS ${TARGET} RUNTIME DESTINATION bin BUNDLE DESTINATION bin COMPONENT tools)
