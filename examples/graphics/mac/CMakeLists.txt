# ==================================================================================================
# CoralGraphics examples - the macOS demo
# ==================================================================================================
# A Cocoa window drawing the same animated scene as the Linux demo through the public
# `graphics::gpu::WindowSurface` API. The window and its NSOpenGLContext are this directory's job; the
# backend adopts the CGL context through `WindowSurface::Configuration::cglContext`, so the frame goes
# straight to the view's drawable rather than through a readback.
#
#   out/examples/bin/mac_demo
#   out/examples/bin/mac_demo --frames 120 --immediate
#   out/examples/bin/mac_demo --verify --backend opengl   # offscreen, no display required
#
# The project is configured from examples/ (see the comment there); this file only declares the target.

if (NOT APPLE OR IOS)
  message(STATUS "examples/graphics/mac: not a macOS host; skipping")
  return()
endif ()

find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)

add_executable(mac_demo
    demo_main.mm
    demo_scene.cpp
)

target_include_directories(mac_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(mac_demo PRIVATE -fobjc-arc)
target_link_libraries(mac_demo PRIVATE
    graphics
    ${COCOA_FRAMEWORK}
    ${OPENGL_FRAMEWORK}
)

set_target_properties(mac_demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# The verification path is the same `gpu::Surface` the other platforms use, so a machine with no window
# server still answers whether the CGL device drew and read back a frame.
add_test(NAME mac_demo_verify COMMAND mac_demo --verify --backend opengl)
set_tests_properties(mac_demo_verify PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 120
)
