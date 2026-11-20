# ==================================================================================================
# shaderc dependency bootstrap
# ==================================================================================================
# shaderc commits its own DEPS file but ignores the materialized third_party tree. A clean checkout
# therefore has the shaderc sources and DEPS, but no SPIRV-Tools/SPIRV-Headers/glslang. Run shaderc's
# own git-sync-deps when shaderc is requested and the nested tree is absent.
#
# Set CORAL_SHADERC_SYNC_DEPS=OFF to forbid network access during configure.

function(coral_sync_shaderc_deps shaderc_dir result_var)
  set(_result FALSE)
  set(_probe "${shaderc_dir}/third_party/spirv-tools/CMakeLists.txt")
  if (EXISTS "${_probe}")
    set(${result_var} TRUE PARENT_SCOPE)
    return()
  endif ()

  if (NOT CORAL_SHADERC_SYNC_DEPS)
    message(STATUS "shaderc: nested third_party is missing and CORAL_SHADERC_SYNC_DEPS=OFF")
    set(${result_var} FALSE PARENT_SCOPE)
    return()
  endif ()

  find_package(Python3 QUIET COMPONENTS Interpreter)
  if (NOT Python3_Interpreter_FOUND)
    message(WARNING "shaderc: nested third_party is missing and Python3 was not found")
    set(${result_var} FALSE PARENT_SCOPE)
    return()
  endif ()

  message(STATUS "shaderc: syncing DEPS into ${shaderc_dir}/third_party")
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${shaderc_dir}/utils/git-sync-deps"
    WORKING_DIRECTORY "${shaderc_dir}"
    RESULT_VARIABLE _sync_result
    OUTPUT_VARIABLE _sync_stdout
    ERROR_VARIABLE _sync_stderr
    TIMEOUT 1800
  )
  if (NOT _sync_result EQUAL 0)
    message(WARNING "shaderc: git-sync-deps failed (${_sync_result})\n${_sync_stderr}")
  endif ()

  if (EXISTS "${_probe}")
    set(${result_var} TRUE PARENT_SCOPE)
  else ()
    set(${result_var} FALSE PARENT_SCOPE)
  endif ()
endfunction()
