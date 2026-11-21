# ==================================================================================================
# CoralGraphics examples - the platform demos
# ==================================================================================================
# The demos are selected by host platform: the Windows tree opens a Win32 window (or writes a PNG), the
# Linux tree opens an X11 or Wayland window, and the macOS tree opens a Cocoa/NSOpenGLView window. All
# are clients of the same public `graphics` API.
#
# The project is configured from examples/, not from these directories, because the dependencies and
# the framework target have to exist before any demo names them.

set(CORALGRAPHICS_RESOURCES "${CORALGRAPHICS_ROOT}/modules/graphics/resources")

if (WIN32)
  add_subdirectory(win)
elseif (APPLE AND NOT IOS)
  add_subdirectory(mac)
elseif (UNIX AND NOT APPLE)
  add_subdirectory(linux)
else ()
  message(STATUS "CoralGraphics examples: no demo for this platform")
endif ()
