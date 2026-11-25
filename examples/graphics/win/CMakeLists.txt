# ==================================================================================================
# CoralGraphics examples - the Windows demos
# ==================================================================================================
# Three programs over the same public `graphics` API, each answering a different question:
#
#   graphics_demo   draws one scene and writes a PNG; runs on a build machine with no display
#   vulkan_demo     the Vulkan backend: a window, and an offscreen `--verify` mode for CI
#   lottie_player   parses and plays a Lottie document in a window
#
# The project is configured from examples/ (see the comment there). This file only declares the
# targets, so the dependency graph exists once, in cmake/CoralGraphicsDependencies.cmake.
#
#   cmake -S examples -B out/examples -G "Visual Studio 17 2022" -A x64
#   cmake --build out/examples --config Release
#   out/examples/bin/Release/graphics_demo.exe --output demo.png
#   ctest --test-dir out/examples -C Release --output-on-failure

if (NOT WIN32)
  message(STATUS "examples/graphics/win: not a Windows host; skipping")
  return()
endif ()

# ==================================================================================================
# The console demo
# ==================================================================================================
# The demo writes a PNG, so it runs anywhere: no window, no display, no graphics driver. What it draws
# is the point, and a file is the most portable way to show it. The windowed player is a separate
# target, below.
add_executable(graphics_demo
    demo_main.cpp
    demo_scene.cpp
)

target_include_directories(graphics_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# `graphics` already carries the include paths and the third-party link dependencies as PUBLIC, so this
# is the whole of what the demo needs to say about them.
target_link_libraries(graphics_demo PRIVATE graphics)

set_target_properties(graphics_demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# The raster path needs no device, so it is the one demo check that runs on every Windows build
# machine. It writes the picture the demo exists to produce and fails if the codec or the rasterizer
# cannot.
add_test(NAME graphics_demo_raster
    COMMAND graphics_demo
        --resources "${CORALGRAPHICS_RESOURCES}"
        --output "${CMAKE_BINARY_DIR}/graphics_demo.png")
set_tests_properties(graphics_demo_raster PROPERTIES TIMEOUT 120)

# ==================================================================================================
# The Vulkan backend demo
# ==================================================================================================
# The one program in this tree whose subject is the Vulkan backend itself. It has two modes because
# "Vulkan draws" is a claim about a picture *and* about pixels:
#
#   out/examples/bin/Release/vulkan_demo.exe                          # a window, until Escape
#   out/examples/bin/Release/vulkan_demo.exe --verify --output out/vulkan   # offscreen checks + PNG
#
# `--verify` renders the same scene offscreen, reads it back and compares six pixels against the colour
# each shape was drawn with, so a machine with a Vulkan driver answers the question without a person
# looking at the window. Exit code 77 means no Vulkan device, which is a skip rather than a failure.
if (CORAL_BACKEND_SUPPORTS_VULKAN)
  add_executable(vulkan_demo vulkan_demo.cpp)
  target_link_libraries(vulkan_demo PRIVATE graphics)
  set_target_properties(vulkan_demo PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  )

  add_test(NAME vulkan_demo_checks COMMAND vulkan_demo --verify --output "${CMAKE_BINARY_DIR}/vulkan_demo")
  set_tests_properties(vulkan_demo_checks PROPERTIES
      SKIP_RETURN_CODE 77
      TIMEOUT 120
  )

  # The render-pass path is what Vulkan 1.0/1.1 devices use; a modern CI GPU always takes dynamic
  # rendering unless it is forced. Running the same demo under the force switch is the regression test
  # for the fallback without needing a 1.1 device on every machine.
  add_test(NAME vulkan_demo_checks_render_pass
      COMMAND vulkan_demo --verify --output "${CMAKE_BINARY_DIR}/vulkan_demo_render_pass")
  set_tests_properties(vulkan_demo_checks_render_pass PROPERTIES
      ENVIRONMENT "CORAL_VULKAN_FORCE_RENDER_PASS=1"
      SKIP_RETURN_CODE 77
      TIMEOUT 120
  )
endif ()

# ==================================================================================================
# The Lottie player
# ==================================================================================================
# A second program over the same library, because it needs a window and the demo deliberately does not:
# the demo writes a PNG and runs on a build machine, in a container, or in a session with no display.
# Putting a message loop inside it would make it fail on exactly the machines where it is most useful.
#
#   out/examples/bin/Release/lottie_player.exe --animation resources/apitest/lottie/gear.json
#   out/examples/bin/Release/lottie_player.exe --backend gpu --gpu-api gl --frames 60
#
# Where there is no window implementation the source compiles to a program that explains what a port
# needs, rather than to one that exits successfully having played nothing.
add_executable(lottie_player lottie_player.cpp)

target_include_directories(lottie_player PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lottie_player PRIVATE graphics)

if (CORAL_LOTTIE_PLAYER_STUB)
  target_compile_definitions(lottie_player PRIVATE CORAL_LOTTIE_PLAYER_STUB=1)
elseif (WIN32)
  # The windowing entry point rather than the console one, so the program does not open a console window
  # alongside itself. The message it prints at startup is then lost, which is why the same information
  # is in the window's title.
  #
  # Not set for the stub, which defines `main`: a GUI-subsystem program is linked against `WinMain`, and
  # asking for one while providing the other is a link error that names the entry point rather than the
  # option.
  set_target_properties(lottie_player PROPERTIES WIN32_EXECUTABLE TRUE)
endif ()

set_target_properties(lottie_player PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# ==================================================================================================
# Optional: diagnostics
# ==================================================================================================
# Decode, shape, blend, filter and clip checkers. They answer questions the demo cannot: when a picture
# looks wrong, is the decoder wrong or the drawing? Off by default, because it is a diagnostic and a
# demo project should build the demo.
#
#   cmake -S examples -B out/examples -DCORAL_GRAPHICS_EXAMPLES_TOOLS=ON
if (CORAL_GRAPHICS_EXAMPLES_TOOLS)
  add_executable(decode_dump tools/decode_dump.cpp)
  target_link_libraries(decode_dump PRIVATE graphics)
  set_target_properties(decode_dump PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

  # Shapes a string and reports the glyph ids, their advances and whether each has an outline. This is
  # what tells a shaping problem from a drawing problem when text comes out wrong.
  add_executable(shape_dump tools/shape_dump.cpp)
  target_link_libraries(shape_dump PRIVATE graphics)
  set_target_properties(shape_dump PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

  # Draws the same colour twice, once through a greyscale filter, and reports both pixels. This is what
  # tells a filter that is applied from one that is silently ignored. It reaches into the private
  # headers, because the blend formulas live there: it checks this library's internals rather than
  # demonstrating its API.
  add_executable(blend_check tools/blend_check.cpp)
  target_link_libraries(blend_check PRIVATE graphics)
  set_target_properties(blend_check PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

  add_executable(filter_dump tools/filter_dump.cpp)
  target_link_libraries(filter_dump PRIVATE graphics)
  set_target_properties(filter_dump PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

  # Draws a figure larger than a clip and reports where the ink landed, which is how a clip that
  # silently does nothing is told from one that works.
  add_executable(clip_dump tools/clip_dump.cpp)
  target_link_libraries(clip_dump PRIVATE graphics)
  set_target_properties(clip_dump PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif ()
