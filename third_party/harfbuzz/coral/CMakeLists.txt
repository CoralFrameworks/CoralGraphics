cmake_minimum_required(VERSION 3.19)
project(harfbuzz-coral)

# ==================================================================================================
# harfbuzz
# ==================================================================================================
#
# Text shaping: given a font and a run of text, harfbuzz produces the glyphs to draw and where to put
# them. That is the piece FreeType does not do -- FreeType loads a font and rasterises a glyph, but
# knows nothing about how a script joins, reorders or substitutes them.
#
# ## Why every optional dependency is off
#
# harfbuzz can be built against glib, ICU, graphite2, cairo, CoreText, Uniscribe, GDI, DirectWrite and
# GObject. All of them are integration surface Coral does not use, and several would drag in a platform
# framework or a large library. harfbuzz ships its own Unicode tables and its own shapers, so a build
# with none of them enabled still shapes Arabic, Devanagari and the rest -- which is the part that
# matters. Turning them off is what keeps this a library rather than a dependency tree.
#
# `HB_HAVE_CORETEXT` is the one that defaults on and is therefore explicitly disabled: on Apple it would
# pull in CoreText, and Coral's plan for that platform is to reach for CoreText directly for
# vectorisation rather than through harfbuzz's shaper.
#
# ## What is built
#
# `harfbuzz` only. The subsetter, the GObject bindings, the cairo helper and the `hb-*` command line
# tools are all created by the upstream CMakeLists and none of them is linked; `EXCLUDE_FROM_ALL` keeps
# them out of a default build.

get_filename_component(HARFBUZZ_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if (NOT EXISTS "${HARFBUZZ_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
      "harfbuzz is not vendored at ${HARFBUZZ_DIR}. Re-vendor third_party/harfbuzz, or configure with "
      "-DCORAL_GRAPHICS_SUPPORTS_HARFBUZZ=OFF to build without text shaping.")
endif ()

# harfbuzz declares `cmake_minimum_required(VERSION 3.12)`, which CMake 4 accepts -- unlike
# libjpeg-turbo's 2.8.12. The policy floor is still set for the same reason as that wrapper: a future
# re-vendor should not be able to break the configure by lowering the declaration.
set(CMAKE_POLICY_VERSION_MINIMUM 3.12)

# Every optional backend off. See the file comment.
set(HB_HAVE_CAIRO OFF CACHE BOOL "vendored: no cairo interop" FORCE)
set(HB_HAVE_FREETYPE OFF CACHE BOOL "vendored: freetype is bridged in Coral's code, not harfbuzz's" FORCE)
set(HB_HAVE_GRAPHITE2 OFF CACHE BOOL "vendored: no graphite2 shaper" FORCE)
set(HB_HAVE_GLIB OFF CACHE BOOL "vendored: no glib" FORCE)
set(HB_HAVE_ICU OFF CACHE BOOL "vendored: harfbuzz's own Unicode tables are used" FORCE)
set(HB_HAVE_CORETEXT OFF CACHE BOOL "vendored: CoreText is reached directly on Apple" FORCE)
set(HB_HAVE_UNISCRIBE OFF CACHE BOOL "vendored: no Uniscribe" FORCE)
set(HB_HAVE_GDI OFF CACHE BOOL "vendored: no GDI" FORCE)
set(HB_HAVE_DIRECTWRITE OFF CACHE BOOL "vendored: no DirectWrite" FORCE)
set(HB_HAVE_GOBJECT OFF CACHE BOOL "vendored: no GObject bindings" FORCE)
set(HB_HAVE_INTROSPECTION OFF CACHE BOOL "vendored: no introspection" FORCE)

# The subsetter and the command line tools. `HB_BUILD_UTILS` needs cairo, freetype and glib installed
# as real packages, so it could not be enabled here even if it were wanted.
set(HB_BUILD_SUBSET OFF CACHE BOOL "vendored: no subsetter" FORCE)
set(HB_BUILD_UTILS OFF CACHE BOOL "vendored: no command line tools" FORCE)

add_subdirectory("${HARFBUZZ_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/harfbuzz" EXCLUDE_FROM_ALL)

if (NOT TARGET harfbuzz)
  message(FATAL_ERROR
      "harfbuzz's CMakeLists did not create a `harfbuzz` target. The vendored copy may be incomplete; "
      "re-vendor third_party/harfbuzz or configure with -DCORAL_GRAPHICS_SUPPORTS_HARFBUZZ=OFF.")
endif ()

set_target_properties(harfbuzz PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    FOLDER ThirdParty
    POSITION_INDEPENDENT_CODE ON
)

# No `target_include_directories` here: harfbuzz exports its targets and CMake refuses an export whose
# interface names a build-tree path. The consumer attaches the include directory, as it does for
# libjpeg-turbo and libwebp.

if (MSVC)
  target_compile_options(harfbuzz PRIVATE /W0)
endif ()

install(TARGETS harfbuzz ARCHIVE DESTINATION lib)
