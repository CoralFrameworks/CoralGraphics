# ==================================================================================================
# Shaders
# ==================================================================================================
#
# The GLSL the RHI's hardware-accelerated paths are built from, compiled here into the SPIR-V the RHI
# consumes.
#
# ## Why the compilation is not left to the device
#
# The RHI accepts GLSL at run time, which is what makes a shader debuggable in place - but it does so by
# linking shaderc, and shaderc brings glslang and SPIRV-Tools, which together are an order of magnitude
# larger than SPIRV-Cross. A build that compiles its own shaders ahead of time does not need any of it,
# so this directory is the difference between a shipped binary that can compile shaders and one that
# can only load them.
#
# ## Why the tool is located rather than built here
#
# `tools/shader_compiler` is a native executable that the build has to *run*. Under the Android
# toolchain `add_executable` would produce an Android binary the build machine cannot execute, so the
# tool is host-only and an Android configure is given its path instead:
#
#   cmake -S . -B build/host  -G Ninja && cmake --build build/host --target shader_compiler
#   cmake -S . -B build/android -DCMAKE_TOOLCHAIN_FILE=... \
#         -DCORAL_SHADERS_COMPILER=$PWD/build/host/tools/shader_compiler/shader_compiler
#
# The same two-step Filament uses for `matc`, and for the same reason.
#
# ## Why the sources are globbed
#
# Everywhere else in this tree a source list is explicit, because a glob over code lets a file be added
# without anyone deciding it belongs. A shader directory is the opposite case: its contents are *data*
# for a tool that walks a directory anyway, adding one is the whole point, and the failure mode a glob
# guards against - a source that is present but never compiled - is exactly what an explicit list would
# cause here. `CONFIGURE_DEPENDS` keeps the glob honest by re-running CMake when it changes.

set(CORAL_SHADERS_COMPILER "" CACHE FILEPATH
    "Path to the shader_compiler executable. Set automatically for a host build; required for a cross build.")

set(CORAL_SHADERS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders" CACHE PATH
    "Where the compiled .spv are written. A packaging step (the Android assets) points at this.")

# Without shaderc the compiler tool can be built but cannot compile anything ("this build has no GLSL
# compiler"), so a GL-only configure must not try: it ships the committed SPIR-V headers instead.
if (DEFINED CORAL_BACKEND_SUPPORTS_SHADERC AND NOT CORAL_BACKEND_SUPPORTS_SHADERC)
  message(STATUS "shaders: shaderc is off, so shaders/ is not compiled; the committed SPIR-V is used")
  return()
endif ()

# A host build built the tool itself, two directories up, so no path has to be supplied by hand.
if (NOT CORAL_SHADERS_COMPILER AND TARGET shader_compiler)
  set(CORAL_SHADERS_COMPILER "$<TARGET_FILE:shader_compiler>")
endif ()

if (NOT CORAL_SHADERS_COMPILER)
  message(STATUS
      "shaders: no compiler available, so nothing under shaders/ will be compiled. "
      "Build the host tree first and pass -DCORAL_SHADERS_COMPILER=<path to shader_compiler>, "
      "or configure with -DCORAL_BACKEND_SUPPORTS_SHADERC=OFF and supply SPIR-V another way.")
  return()
endif ()

file(GLOB_RECURSE CORAL_SHADER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.geom"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.tesc"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.tese"
)

# A file sitting in this directory that the build never compiles is the one failure mode a directory whose
# whole purpose is "everything here is compiled" cannot tolerate, so `.glsl` is reported rather than
# skipped. It carries no stage in its name, and inferring one from the file name - `vertex.glsl` is a
# vertex shader - is the kind of guess this tree deliberately avoids elsewhere: a wrong guess produces a
# shader compiled as the wrong stage, which surfaces as a link error on a device rather than here.
#
# Reported and not failed, because a project part-way through adopting this pipeline has such files
# already and blocking every build on them helps nobody. `message(STATUS)` is where a developer looks.
file(GLOB_RECURSE CORAL_UNCLASSIFIED_SHADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.glsl")
if (CORAL_UNCLASSIFIED_SHADERS)
  list(LENGTH CORAL_UNCLASSIFIED_SHADERS unclassified_count)
  message(STATUS
      "shaders: ${unclassified_count} .glsl file(s) are NOT compiled, because a .glsl carries no stage in its "
      "name. Rename each to .vert/.frag/.comp/.geom/.tesc/.tese, or compile it by hand with "
      "`shader_compiler <file> --stage <stage>`:")
  foreach (unclassified IN LISTS CORAL_UNCLASSIFIED_SHADERS)
    file(RELATIVE_PATH relative "${CMAKE_CURRENT_SOURCE_DIR}" "${unclassified}")
    message(STATUS "  shaders/${relative}")
  endforeach ()
endif ()

if (NOT CORAL_SHADER_SOURCES)
  message(STATUS "shaders: no shader sources found under ${CMAKE_CURRENT_SOURCE_DIR}")
  return()
endif ()

set(CORAL_SHADER_OUTPUTS "")

foreach (shader IN LISTS CORAL_SHADER_SOURCES)
  # The tree is mirrored, so two shaders with the same name in different directories stay distinct, and
  # the stage is kept in the file name - `present.vert.spv`, not `present.spv`. Replacing the extension
  # instead would make `present.vert` and `present.frag` both write `present.spv`, and the build would
  # fail on a duplicate output rule rather than on anything that names the real mistake.
  file(RELATIVE_PATH relative "${CMAKE_CURRENT_SOURCE_DIR}" "${shader}")
  set(output "${CORAL_SHADERS_OUTPUT_DIR}/${relative}.spv")

  # One command per shader rather than one for the directory: this way a shader that changed is the only
  # one recompiled, they compile in parallel, and CMake knows the exact set of files produced - which is
  # what the packaging step needs in order to depend on them.
  add_custom_command(
      OUTPUT "${output}"
      # The canvas sources carry Vulkan descriptor-set qualifiers (`layout(set = 1, ...)`), and glslang only
      # accepts those in the Vulkan environment. The SPIR-V this produces is what SPIRV-Cross consumes for
      # the OpenGL backend, so the flag is right for every consumer rather than an Android-only exception.
      COMMAND ${CORAL_SHADERS_COMPILER} "${shader}" -o "${output}" --environment vulkan
      DEPENDS "${shader}" ${CORAL_SHADERS_COMPILER}
      COMMENT "Compiling ${relative}"
      VERBATIM
  )

  list(APPEND CORAL_SHADER_OUTPUTS "${output}")
endforeach ()

list(LENGTH CORAL_SHADER_SOURCES shader_count)

# A named target so a build can ask for the shaders alone, and so packaging has something to depend on.
add_custom_target(shaders ALL
    DEPENDS ${CORAL_SHADER_OUTPUTS}
    COMMENT "Compiling ${shader_count} shader(s) to SPIR-V"
)

set_target_properties(shaders PROPERTIES FOLDER Shaders)

# The .spv travel with the libraries: they are as much a part of what Coral ships as the headers are.
install(DIRECTORY "${CORAL_SHADERS_OUTPUT_DIR}/" DESTINATION "shaders"
    FILES_MATCHING PATTERN "*.spv"
    PATTERN "CMakeFiles" EXCLUDE)
