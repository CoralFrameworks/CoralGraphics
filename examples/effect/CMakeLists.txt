# ==================================================================================================
# CoralGraphics examples - the effect render-graph demos
# ==================================================================================================
# The effect module is a separate archive over `graphics`: a frame graph of input nodes, shader passes
# and output nodes. These programs demonstrate the public API at three levels:
#
#   effect_demo            the smallest client that uses every public layer; no window beyond an
#                          offscreen device, and exit code 77 means "no GPU here"
#   effect_window_demo     the Windows swap-chain output with a real message loop, and its frame-time
#                          sampler
#   effect_transition_demo two video inputs into one pass: preload, transition and hold
#   effect_x11_demo        the Linux X11 output
#   effect_wayland_demo    the Linux Wayland output
#   effect_benchmark       per-stage timings of the same graph, without a window
#
# The project is configured from examples/ (see the comment there); this file only declares targets.

# ==================================================================================================
# The headless demo
# ==================================================================================================
# Its exit code 77 means "no GPU here", which CTest records as a skip rather than a failure.
add_executable(effect_demo effect_engine_demo.cpp)
target_link_libraries(effect_demo PRIVATE effect)
set_target_properties(effect_demo PROPERTIES
    FOLDER Tests
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
add_test(NAME effect_demo COMMAND effect_demo)
set_tests_properties(effect_demo PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 120
)

# ==================================================================================================
# The benchmark
# ==================================================================================================
# It measures the same graph the demos build, so it belongs beside them rather than in the module's
# unit suite.
add_executable(effect_benchmark effect_benchmark.cpp)
target_link_libraries(effect_benchmark PRIVATE effect)
set_target_properties(effect_benchmark PROPERTIES
    FOLDER Tests
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

if (MSVC)
  target_compile_options(effect_demo PRIVATE /utf-8 /W3)
  target_compile_options(effect_benchmark PRIVATE /utf-8 /W3)
else ()
  target_compile_options(effect_demo PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_compile_options(effect_benchmark PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif ()

# ==================================================================================================
# macOS: the offscreen cross-backend verification
# ==================================================================================================
# `effect_demo` next door proves the graph runs; this proves the picture is right and that the backends agree. It is
# registered three times: once as the whole-machine check and once per backend, so a failure names the backend without
# a reader having to parse the combined output. A backend this machine cannot bring up exits 77, which CTest reports as
# a skip -- the same contract the other GPU checks use.
if (APPLE AND NOT IOS)
  add_executable(effect_mac_demo effect_mac_demo.cpp EffectVerification.cpp)
  target_link_libraries(effect_mac_demo PRIVATE effect)
  set_target_properties(effect_mac_demo PROPERTIES
      FOLDER Tests
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  )
  add_test(NAME effect_mac_demo COMMAND effect_mac_demo)
  add_test(NAME effect_mac_demo_metal COMMAND effect_mac_demo --backend metal)
  add_test(NAME effect_mac_demo_opengl COMMAND effect_mac_demo --backend opengl)
  set_tests_properties(effect_mac_demo effect_mac_demo_metal effect_mac_demo_opengl PROPERTIES
      SKIP_RETURN_CODE 77
      TIMEOUT 120
  )
  target_compile_options(effect_mac_demo PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif ()

# ==================================================================================================
# Windows: the swap-chain window
# ==================================================================================================
if (WIN32 AND TARGET effect_win)
  # The sampler is user-mode stack sampling: `wpr`/`xperf` need an elevated kernel session, which an
  # ordinary shell does not have, and a frame-time question needs an answer anyway.
  add_executable(effect_window_demo effect_window_demo.cpp ThreadSampler.cpp)
  target_link_libraries(effect_window_demo PRIVATE effect effect_win dbghelp)
  set_target_properties(effect_window_demo PROPERTIES
      FOLDER Tests
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  )
  add_test(NAME effect_window_demo COMMAND effect_window_demo --hidden --frames 5)
  set_tests_properties(effect_window_demo PROPERTIES
      SKIP_RETURN_CODE 77
      TIMEOUT 120
  )

  # The preload/transition/hold demo: two video inputs into one pass, and the placeholder frame that
  # keeps the engine from having to guess what "preloaded but not started" means.
  add_executable(effect_transition_demo effect_transition_demo.cpp)
  target_link_libraries(effect_transition_demo PRIVATE effect effect_win)
  set_target_properties(effect_transition_demo PROPERTIES
      FOLDER Tests
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  )
  add_test(NAME effect_transition_demo COMMAND effect_transition_demo --hidden --null-output)
  set_tests_properties(effect_transition_demo PROPERTIES
      SKIP_RETURN_CODE 77
      TIMEOUT 120
  )

  if (MSVC)
    target_compile_options(effect_window_demo PRIVATE /utf-8 /W3)
    target_compile_options(effect_transition_demo PRIVATE /utf-8 /W3)
  else ()
    target_compile_options(effect_window_demo PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_compile_options(effect_transition_demo PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif ()
endif ()

# ==================================================================================================
# Linux: X11 and Wayland
# ==================================================================================================
if (LINUX AND TARGET effect_x11)
  add_executable(effect_x11_demo effect_x11_demo.cpp)
  target_link_libraries(effect_x11_demo PRIVATE effect effect_x11 ${X11_LIBRARIES})
  target_include_directories(effect_x11_demo PRIVATE ${X11_INCLUDE_DIR})
  set_target_properties(effect_x11_demo PROPERTIES
      FOLDER Tests
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  )
  add_test(NAME effect_x11_demo COMMAND effect_x11_demo --frames 5)
  set_tests_properties(effect_x11_demo PROPERTIES
      SKIP_RETURN_CODE 77
      TIMEOUT 120
  )
endif ()

if (LINUX AND TARGET effect_wayland)
  find_program(WAYLAND_SCANNER_EXECUTABLE wayland-scanner REQUIRED)
  pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
  set(XDG_SHELL_XML "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
  set(WAYLAND_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/wayland-protocols")
  add_custom_command(
      OUTPUT "${WAYLAND_GEN_DIR}/xdg-shell-client-protocol.h" "${WAYLAND_GEN_DIR}/xdg-shell-protocol.c"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${WAYLAND_GEN_DIR}"
      COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header "${XDG_SHELL_XML}"
      "${WAYLAND_GEN_DIR}/xdg-shell-client-protocol.h"
      COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code "${XDG_SHELL_XML}"
      "${WAYLAND_GEN_DIR}/xdg-shell-protocol.c"
      DEPENDS "${XDG_SHELL_XML}"
      COMMENT "Generating xdg-shell client protocol"
  )
  add_executable(effect_wayland_demo
      effect_wayland_demo.cpp
      "${WAYLAND_GEN_DIR}/xdg-shell-protocol.c"
  )
  target_include_directories(effect_wayland_demo PRIVATE ${WAYLAND_GEN_DIR} ${WAYLAND_INCLUDE_DIRS})
  target_link_libraries(effect_wayland_demo PRIVATE effect effect_wayland ${WAYLAND_LIBRARIES})
  set_target_properties(effect_wayland_demo PROPERTIES
      FOLDER Tests
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  )
  add_test(NAME effect_wayland_demo COMMAND effect_wayland_demo --frames 5)
  set_tests_properties(effect_wayland_demo PROPERTIES
      SKIP_RETURN_CODE 77
      TIMEOUT 120
      ENVIRONMENT "WAYLAND_DISPLAY=wayland-0"
  )
endif ()
