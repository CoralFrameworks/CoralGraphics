# ==================================================================================================
# CoralGraphics examples - the Linux demos
# ==================================================================================================
# One program that opens a real native window and draws the same scene through the public
# `graphics::gpu::WindowSurface` API, and a Lottie player over the same window layer. The window system
# is selected at run time:
#
#   out/examples/bin/linux_demo --platform x11
#   out/examples/bin/linux_demo --platform wayland
#   out/examples/bin/linux_demo --verify          # offscreen, no display required
#
# The project is configured from examples/ (see the comment there); this file only declares the
# targets and the window-system dependencies they need.

if (NOT UNIX OR APPLE)
  message(STATUS "examples/graphics/linux: not a Linux host; skipping")
  return()
endif ()

# ==================================================================================================
# Window-system dependencies
# ==================================================================================================
find_package(X11 REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(WAYLAND_EGL REQUIRED wayland-egl)
pkg_check_modules(EGL REQUIRED egl)
find_program(WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

set(XDG_SHELL_XML "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
if (NOT EXISTS "${XDG_SHELL_XML}")
  message(FATAL_ERROR "xdg-shell.xml was not found under '${WAYLAND_PROTOCOLS_DIR}'. Install wayland-protocols.")
endif ()

set(GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GENERATED_DIR})
set(XDG_SHELL_HEADER ${GENERATED_DIR}/xdg-shell-client-protocol.h)
set(XDG_SHELL_SOURCE ${GENERATED_DIR}/xdg-shell-protocol.c)

add_custom_command(
    OUTPUT ${XDG_SHELL_HEADER} ${XDG_SHELL_SOURCE}
    COMMAND ${WAYLAND_SCANNER} client-header ${XDG_SHELL_XML} ${XDG_SHELL_HEADER}
    COMMAND ${WAYLAND_SCANNER} private-code ${XDG_SHELL_XML} ${XDG_SHELL_SOURCE}
    DEPENDS ${XDG_SHELL_XML}
    COMMENT "Generating xdg-shell client protocol"
    VERBATIM
)

# ==================================================================================================
# The demo
# ==================================================================================================
add_executable(linux_demo
    demo_main.cpp
    demo_scene.cpp
    x11_window.cpp
    wayland_window.cpp
    ${XDG_SHELL_SOURCE}
)
set_source_files_properties(${XDG_SHELL_SOURCE} PROPERTIES LANGUAGE C)

target_include_directories(linux_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${GENERATED_DIR}
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${WAYLAND_EGL_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
)
target_compile_options(linux_demo PRIVATE ${WAYLAND_CLIENT_CFLAGS_OTHER} ${EGL_CFLAGS_OTHER})
target_link_directories(linux_demo PRIVATE
    ${WAYLAND_CLIENT_LIBRARY_DIRS}
    ${WAYLAND_EGL_LIBRARY_DIRS}
    ${EGL_LIBRARY_DIRS}
)
target_link_libraries(linux_demo PRIVATE
    graphics
    X11::X11
    ${WAYLAND_CLIENT_LIBRARIES}
    ${WAYLAND_EGL_LIBRARIES}
    ${EGL_LIBRARIES}
)

set_target_properties(linux_demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# ==================================================================================================
# The Lottie player
# ==================================================================================================
# The Linux counterpart of `examples/graphics/win/lottie_player.cpp`. It uses the same parser and
# renderer, but presents through `gpu::WindowSurface` on X11 and Wayland instead of Win32.
# `--backend raster` is deliberately not implemented here: the subject is the GPU path, and a CPU blit
# would need a second window implementation on each platform.
#
#   out/examples/bin/lottie_player --platform x11 --gpu-api vulkan --frames 120
#   out/examples/bin/lottie_player --platform wayland --gpu-api vulkan --frames 120
#   out/examples/bin/lottie_player --verify --gpu-api vulkan
add_executable(lottie_player
    lottie_player.cpp
    ${XDG_SHELL_SOURCE}
)
set_source_files_properties(${XDG_SHELL_SOURCE} PROPERTIES LANGUAGE C)

target_include_directories(lottie_player PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${GENERATED_DIR}
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${WAYLAND_EGL_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
)
target_compile_options(lottie_player PRIVATE ${WAYLAND_CLIENT_CFLAGS_OTHER} ${EGL_CFLAGS_OTHER})
target_link_directories(lottie_player PRIVATE
    ${WAYLAND_CLIENT_LIBRARY_DIRS}
    ${WAYLAND_EGL_LIBRARY_DIRS}
    ${EGL_LIBRARY_DIRS}
)
target_link_libraries(lottie_player PRIVATE
    graphics
    X11::X11
    ${WAYLAND_CLIENT_LIBRARIES}
    ${WAYLAND_EGL_LIBRARIES}
    ${EGL_LIBRARIES}
)
set_target_properties(lottie_player PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# ==================================================================================================
# Verification
# ==================================================================================================
# `--verify` uses the offscreen path, so a build machine with no window system can still answer
# whether the OpenGL device drew and read back a frame.
add_test(NAME linux_demo_verify COMMAND linux_demo --verify --backend opengl)
set_tests_properties(linux_demo_verify PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 120
)

add_test(NAME lottie_player_verify COMMAND lottie_player --verify --gpu-api gl)
set_tests_properties(lottie_player_verify PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 120
)

if (CORAL_BACKEND_SUPPORTS_VULKAN)
  # The Vulkan paths are registered only when the backend is in this build. A headless run returns
  # 77, so a machine without a display still reports a skip rather than a failure.
  add_test(NAME linux_demo_verify_vulkan COMMAND linux_demo --verify --backend vulkan)
  add_test(NAME linux_demo_x11_vulkan
      COMMAND linux_demo --platform x11 --backend vulkan --frames 3
  )
  add_test(NAME linux_demo_wayland_vulkan
      COMMAND linux_demo --platform wayland --backend vulkan --frames 3 --immediate
  )
  add_test(NAME lottie_player_verify_vulkan COMMAND lottie_player --verify --gpu-api vulkan)
  add_test(NAME lottie_player_x11_vulkan
      COMMAND lottie_player --platform x11 --gpu-api vulkan --frames 3
  )
  add_test(NAME lottie_player_wayland_vulkan
      COMMAND lottie_player --platform wayland --gpu-api vulkan --frames 3
  )
  set_tests_properties(
      linux_demo_verify_vulkan
      linux_demo_x11_vulkan
      linux_demo_wayland_vulkan
      lottie_player_verify_vulkan
      lottie_player_x11_vulkan
      lottie_player_wayland_vulkan
      PROPERTIES
          SKIP_RETURN_CODE 77
          TIMEOUT 120
  )
endif ()
