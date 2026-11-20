# ==================================================================================================
# CoralGraphics examples
# ==================================================================================================
# The framework's demo and verification programs. Two ways to build them:
#
#   cmake -S examples -B out/examples ...   # standalone: dependencies, framework, then demos
#   cmake -S . -B build ...                 # the framework root adds this directory when top-level
#
# As a client of the framework the demos use the public API only -- one include path, no private
# headers. The graphics demos write a PNG, so they run on a build machine with no display; the windowed
# players and the effect demos skip themselves (exit code 77) when there is no device or no window
# system.
#
# What is here:
#
#   graphics/win/       the console scene demo, the Vulkan verification demo and the Lottie player
#   graphics/linux/     the X11/Wayland demo and the Lottie player
#   effect/             the effect render-graph demos and its benchmark
#
# The shared third-party libraries (libz, libyuv, libgtest) are this repository's own submodules under
# third_party/, so a standalone configure only needs `git submodule update --init --recursive`. A project
# that consumes CoralGraphics as a submodule provides its own targets, which the dependency manifest
# prefers; see cmake/CoralGraphicsDependencies.cmake.

set(CORALGRAPHICS_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  # Standalone. Resolve the dependencies and add the framework the same way the application root does:
  # the dependency graph first, then the framework, then the demos.
  cmake_minimum_required(VERSION 3.19)
  project(CoralGraphicsExamples LANGUAGES C CXX)

  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)

  include("${CORALGRAPHICS_ROOT}/cmake/CoralBuild.cmake")
  coral_apply_global_build_settings()

  set(IS_HOST_PLATFORM TRUE)

  # The demos link the framework's libraries; they do not build or run its unit suites, which is why
  # the examples project does not need GoogleTest even when the shared third-party tree carries it.
  set(CORAL_BUILD_TESTS OFF CACHE BOOL "Build the CoralGraphics tests" FORCE)

  include("${CORALGRAPHICS_ROOT}/cmake/CoralGraphicsDependencies.cmake")
  add_subdirectory("${CORALGRAPHICS_ROOT}" "${CMAKE_BINARY_DIR}/coral-graphics")
endif ()

# The Linux demos compile the wayland-scanner's generated protocol as C. A standalone configure already
# enables C through project(); a configure added by the framework root (which is C++-only) needs it here,
# otherwise the generated source has no compiler on a Linux host.
enable_language(C)

# ==================================================================================================
# Example options
# ==================================================================================================
# The Lottie player's no-window branch is otherwise never compiled on a Windows host; turning it on is
# what a port to a new platform starts from.
option(CORAL_LOTTIE_PLAYER_STUB "Compile the Lottie player's no-window branch" OFF)

# A handful of diagnostics that answer "is the decoder wrong or the drawing?" and are useful while
# developing the library, but are not demo material.
option(CORAL_GRAPHICS_EXAMPLES_TOOLS "Build the example project's diagnostic tools" OFF)

enable_testing()

add_subdirectory(graphics)

if (CORAL_GRAPHICS_WITH_EFFECT)
  add_subdirectory(effect)
endif ()
