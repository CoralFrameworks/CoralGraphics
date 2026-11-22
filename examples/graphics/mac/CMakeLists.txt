# ==================================================================================================
# CoralGraphics examples - the macOS demo
# ==================================================================================================
# A Cocoa window drawing the same animated scene as the Linux demo through the public
# `graphics::gpu::WindowSurface` API. The window is this directory's job, in two forms: a plain NSView whose
# backing layer is a CAMetalLayer (Metal, the default), or an NSOpenGLView with its NSOpenGLContext whose
# CGLContextObj the backend adopts (OpenGL). Either way the frame goes straight to the view's drawable rather
# than through a readback.
#
#   out/examples/bin/mac_demo
#   out/examples/bin/mac_demo --frames 120 --immediate
#   out/examples/bin/mac_demo --backend opengl
#   out/examples/bin/mac_demo --verify --backend metal   # offscreen, no display required
#
# The project is configured from examples/ (see the comment there); this file only declares the target.

if (NOT APPLE OR IOS)
  message(STATUS "examples/graphics/mac: not a macOS host; skipping")
  return()
endif ()

find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
# The Metal window owns a CAMetalLayer; QuartzCore is its framework and Metal is what the layer and the
# device come from. The graphics target links both transitively, but naming them here keeps this target's
# own Objective-C++ includes honest.
find_library(METAL_FRAMEWORK Metal REQUIRED)
find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)

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
    ${METAL_FRAMEWORK}
    ${QUARTZCORE_FRAMEWORK}
)

set_target_properties(mac_demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# The verification path is the same `gpu::Surface` the other platforms use, so a machine with no window
# server still answers whether the device drew and read back a frame. The auto row follows the platform
# (Metal), and the pinned rows make a failure name the backend instead of leaving a reader to parse the
# auto row's output.
add_test(NAME mac_demo_verify COMMAND mac_demo --verify)
set_tests_properties(mac_demo_verify PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 120
)
if (CORAL_BACKEND_SUPPORTS_METAL)
  add_test(NAME mac_demo_verify_metal COMMAND mac_demo --verify --backend metal)
  set_tests_properties(mac_demo_verify_metal PROPERTIES
      SKIP_RETURN_CODE 77
      TIMEOUT 120
  )
endif ()
if (CORAL_BACKEND_SUPPORTS_OPENGL)
  add_test(NAME mac_demo_verify_opengl COMMAND mac_demo --verify --backend opengl)
  set_tests_properties(mac_demo_verify_opengl PROPERTIES
      SKIP_RETURN_CODE 77
      TIMEOUT 120
  )
endif ()
