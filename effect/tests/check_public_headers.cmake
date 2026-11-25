# Checks that the installed public headers do not name the RHI or private implementation headers.
#
# Invoked by CTest with -DHEADER_DIR=<effect/include/effect>. Keeping it a script rather than a
# compile-time trick means it also covers headers no translation unit happens to include.

if (NOT DEFINED HEADER_DIR)
  message(FATAL_ERROR "HEADER_DIR was not supplied")
endif ()

file(GLOB_RECURSE headers "${HEADER_DIR}/*.h")
list(LENGTH headers header_count)
if (header_count EQUAL 0)
  message(FATAL_ERROR "no public headers found under ${HEADER_DIR}")
endif ()

set(banned_patterns
    "#[ ]*include[ ]*[<\"]backend/"
    "#[ ]*include[ ]*[<\"]private/"
    "#[ ]*include[ ]*[<\"]gpu/"
    "backend::"
    "RenderPassHandle"
    "CommandBuffer"
)

foreach(header IN LISTS headers)
  file(READ "${header}" content)
  # `backend::Device` is the one RHI name this API deliberately allows. `setOnDeviceReady` and
  # `setOnBeforeFrame` hand the running device to the application so a platform texture resolver can
  # share it; it is a forward declaration in a callback signature, never an RHI include. Stripping
  # exactly that spelling keeps the check meaningful for every other backend type without forcing the
  # effect engine to hide the device it is already running on behind a `void*`.
  string(REGEX REPLACE "backend::Device" "ForwardDeclaredDevice" content "${content}")
  foreach(pattern IN LISTS banned_patterns)
    if (content MATCHES "${pattern}")
      message(FATAL_ERROR
          "public header ${header} matches the banned pattern '${pattern}'; "
          "backend and private types must stay behind pimpl / src")
    endif ()
  endforeach()
endforeach()

message(STATUS "checked ${header_count} public headers for backend leakage")
