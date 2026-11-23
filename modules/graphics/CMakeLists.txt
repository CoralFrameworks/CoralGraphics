cmake_minimum_required(VERSION 3.19)
project(graphics)

# ==================================================================================================
# The facade
# ==================================================================================================
# modules/graphics is the layer a client uses. It owns the `gpu::` vocabulary - Context, VertexBuffer,
# IndexBuffer, Texture, Shader - and nothing else. The implementation lives one level down in
# modules/graphics/backend, which is added below as its own target, so that the two have separate build
# options, separate compile-time configuration and a one-way dependency between them.
#
# The dependency direction is the point: gpu depends on backend, backend knows nothing about gpu beyond
# the header directory it publishes (a viewport is a value type both layers name). A client that links
# gpu gets backend linked for it and never has to know the target exists, while a caller that wants the
# raw device can still include <backend/RHI.h> because those headers are installed alongside.

set(TARGET graphics)

set(PUBLIC_HDR_DIR ${CMAKE_CURRENT_LIST_DIR}/include)    # graphics/...   -> graphics::Path
# Internal headers shared between translation units of this target. On this target's include path and
# nowhere else, so a consumer physically cannot include them -- the build fails to find the header rather
# than failing to link, which is the diagnostic a reader can act on.
set(PRIVATE_HDR_DIR ${CMAKE_CURRENT_LIST_DIR}/include/private)
set(SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/src)

# ==================================================================================================
# The implementation layer
# ==================================================================================================
# Declared before anything else so that `backend` exists by the time this file links against it. The
# subdirectory owns its own target name, its own options and its own tests.
# A standalone Graphics configure does not have the application's EXTERNAL variable. The fallback is
# third_party/ two levels up (this repository's own); a framework checkout can still pass -DEXTERNAL=<dir>.
if (NOT DEFINED EXTERNAL)
  set(EXTERNAL "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party")
endif()

add_subdirectory(backend)

# ==================================================================================================
# Geometry
# ==================================================================================================
# `geometry` is the value-type library the facade's public headers are written against - graphics/Paint.h
# includes <geometry/Color.h>, and everything downstream inherits that requirement. It lives here rather
# than under a shared module because that is the only layer that needs it.
#
# Its PUBLIC include directory therefore has to reach every client of `graphics`, not just this target's
# own translation units: a consumer including <graphics/Paint.h> would otherwise compile this library
# fine and then fail in its own code. That is why the link below is PUBLIC rather than PRIVATE, and it
# is the whole reason this line exists instead of the root adding the subdirectory.
add_subdirectory(geometry)

# The build-time shader compiler and the GLSL it compiles belong to Graphics: the backend owns the
# compile settings, and the shaders are the backend's data. Both live here so a standalone Graphics
# repository can build its own tool instead of reaching into the application's tools/.
#
# The compiler is a *host* tool: a cross build cannot execute the executable it produces, so the
# directory is skipped there. Such a build either receives a host compiler with
# `-DCORAL_SHADERS_COMPILER=<path>` (the Android two-step in `shaders/README.md`) or uses the committed
# `src/gpu/CanvasShadersEmbedded.h`. iOS is the case that made the distinction visible: CMake turns an
# executable into a bundle there, and an install rule without a bundle destination fails the configure.
if (NOT CMAKE_CROSSCOMPILING)
  add_subdirectory(tools/shader_compiler)
endif ()
add_subdirectory(shaders)

# ==================================================================================================
# Sources and headers
# ==================================================================================================

set(PUBLIC_HDRS
    ${PUBLIC_HDR_DIR}/codec/ImageCodec.h
    ${PUBLIC_HDR_DIR}/graphics/ImageCodec.h
    ${PUBLIC_HDR_DIR}/gpu/Buffer.h
    ${PUBLIC_HDR_DIR}/gpu/Context.h
    ${PUBLIC_HDR_DIR}/gpu/DrawList.h
    ${PUBLIC_HDR_DIR}/gpu/Layer.h
    ${PUBLIC_HDR_DIR}/gpu/Recorder.h
    ${PUBLIC_HDR_DIR}/gpu/Shader.h
    ${PUBLIC_HDR_DIR}/gpu/Surface.h
    ${PUBLIC_HDR_DIR}/gpu/Types.h
    ${PUBLIC_HDR_DIR}/gpu/Viewport.h
    ${PUBLIC_HDR_DIR}/graphics/Bitmap.h
    ${PUBLIC_HDR_DIR}/graphics/Canvas.h
    ${PUBLIC_HDR_DIR}/graphics/Color.h
    ${PUBLIC_HDR_DIR}/graphics/ColorFilter.h
    ${PUBLIC_HDR_DIR}/graphics/ColorSpace.h
    ${PUBLIC_HDR_DIR}/graphics/ColorTable.h
    ${PUBLIC_HDR_DIR}/graphics/Data.h
    ${PUBLIC_HDR_DIR}/graphics/DataTable.h
    ${PUBLIC_HDR_DIR}/graphics/Enums.h
    ${PUBLIC_HDR_DIR}/graphics/ExternalImage.h
    ${PUBLIC_HDR_DIR}/graphics/Font.h
    ${PUBLIC_HDR_DIR}/graphics/FontFallback.h
    ${PUBLIC_HDR_DIR}/graphics/FontManager.h
    ${PUBLIC_HDR_DIR}/graphics/FontMetrics.h
    ${PUBLIC_HDR_DIR}/graphics/FontStyle.h
    ${PUBLIC_HDR_DIR}/graphics/GlyphAtlas.h
    ${PUBLIC_HDR_DIR}/graphics/Image.h
    ${PUBLIC_HDR_DIR}/graphics/ImageFilter.h
    ${PUBLIC_HDR_DIR}/graphics/ImageGenerator.h
    ${PUBLIC_HDR_DIR}/graphics/ImageInfo.h
    ${PUBLIC_HDR_DIR}/graphics/MaskFilter.h
    ${PUBLIC_HDR_DIR}/graphics/Paint.h
    ${PUBLIC_HDR_DIR}/graphics/Path.h
    ${PUBLIC_HDR_DIR}/graphics/PathEffect.h
    ${PUBLIC_HDR_DIR}/graphics/PathMeasure.h
    ${PUBLIC_HDR_DIR}/graphics/Picture.h
    ${PUBLIC_HDR_DIR}/graphics/SdfGlyph.h
    ${PUBLIC_HDR_DIR}/graphics/PictureRecorder.h
    ${PUBLIC_HDR_DIR}/graphics/Pixmap.h
    ${PUBLIC_HDR_DIR}/graphics/SamplingOptions.h
    ${PUBLIC_HDR_DIR}/graphics/Shader.h
    ${PUBLIC_HDR_DIR}/graphics/Stream.h
    ${PUBLIC_HDR_DIR}/graphics/StrokeRec.h
    ${PUBLIC_HDR_DIR}/graphics/Surface.h
    ${PUBLIC_HDR_DIR}/graphics/TextBlob.h
    ${PUBLIC_HDR_DIR}/graphics/Typeface.h
    ${PUBLIC_HDR_DIR}/graphics/YUVAInfo.h
    ${PUBLIC_HDR_DIR}/lottie/Lottie.h
    ${PUBLIC_HDR_DIR}/svg/Svg.h
)

# Internal to libgpu: the shared device state a resource holds. It is not installed, because a client
# never names it - only the resources it keeps alive - and putting it in the public tree would invite
# exactly the reaching-into-internals the split exists to prevent.
# Two groups, because they answer different questions.
#
# The headers under `include/private/` are the ones *shared across `src/` directories*: they are reached from outside
# the directory that owns them, so they live where that is visible in the path. `backend/include/private/backend/gl/`
# is the same arrangement one level up.
#
# The headers still under `src/` have no includer outside their own directory: `gpu/ContextState.h` is shared by the
# `gpu/` sources, `font/FontContext.h` by the `font/` ones, and `pathops/Intersection.h` by the `pathops/` ones. Nothing
# else may include them, so a `src/` path is already as private as they need to be. `gcms/Numeric.h` used to be one of
# these; it moved with its module when `TransferFunction.h` did.
set(PRIVATE_HDRS
    include/private/gcms/Gamut.h
    include/private/gcms/Numeric.h
    include/private/gcms/TransferFunction.h
    include/private/gpu/ImageAtlas.h
    include/private/pathops/Curve.h
    include/private/pathops/pathops.h
    include/private/xml/Xml.h
    include/private/svg/SvgPresentation.h
    include/private/svg/SvgTypes.h
    include/private/svg/SvgDocument.h
    include/private/svg/SvgGradient.h
    include/private/svg/SvgDataUri.h
    include/private/svg/SvgFilter.h
    include/private/lottie/LottieValue.h
    include/private/lottie/LottieGeometry.h
    include/private/lottie/LottieTrim.h
    include/private/lottie/LottieModel.h
    include/private/lottie/LottieRenderer.h
    include/private/graphics/CanvasBackend.h
    include/private/gpu/ContextImpl.h
    src/gpu/ContextState.h
    src/gpu/CoverageRecord.h
    src/gpu/BlendRouting.h
    src/gpu/GpuRenderer.h
    src/gpu/CanvasShadersGLSL.h
    src/font/FontContext.h
    src/pathops/Intersection.h
)

set(SRCS
    src/codec/ImageCodec.cpp
    src/codec/PixelLayout.cpp
    src/font/Font.cpp
    src/font/FontFallback.cpp
    src/font/FontManager.cpp
    src/font/FontContext.cpp
    src/font/GlyphAtlas.cpp
    src/font/PlatformGlyphRasterizer.cpp
    src/font/Typeface.cpp
    src/gcms/Gamut.cpp
    src/gcms/Numeric.cpp
    src/gcms/TransferFunction.cpp
    src/gpu/Buffer.cpp
    src/gpu/Context.cpp
    src/gpu/ContextImpl.cpp
    src/gpu/ContextState.cpp
    src/gpu/GpuRenderer.cpp
    src/gpu/GpuSurface.cpp
  src/gpu/ListBake.cpp
    src/gpu/ContextGroup.cpp
    src/gpu/WindowSurface.cpp
    src/gpu/GraphiteCanvasBackend.cpp
    src/gpu/FrameClock.cpp
    src/gpu/Recorder.cpp
    src/gpu/Shader.cpp
    src/gpu/Types.cpp
    src/lottie/Lottie.cpp
    src/lottie/LottieGeometry.cpp
    src/lottie/LottieModel.cpp
    src/lottie/LottieRenderer.cpp
    src/lottie/LottieTrim.cpp
    src/lottie/LottieValue.cpp
    src/measure/FlatPath.cpp
    src/pathops/Boolean.cpp
    src/pathops/Curve.cpp
    src/graphics/DecodeLimits.cpp
    src/pathops/Intersection.cpp
    src/pathops/pathops.cpp
    src/raster/Blend.cpp
    src/raster/Flatten.cpp
    src/raster/RasterCanvasBackend.cpp
    src/svg/Svg.cpp
    src/svg/SvgDataUri.cpp
    src/svg/SvgDocument.cpp
    src/svg/SvgDom.cpp
    src/svg/SvgFilter.cpp
    src/svg/SvgGradient.cpp
    src/svg/SvgPathData.cpp
    src/svg/SvgPresentation.cpp
    src/svg/SvgTypes.cpp
    src/xml/Xml.cpp
    src/Bitmap.cpp
    src/Canvas.cpp
    src/ColorFilter.cpp
    src/ColorSpace.cpp
    src/ColorSpaceXform.cpp
    src/Data.cpp
    src/IccProfile.cpp
    src/IccProfileWriter.cpp
    src/Image.cpp
    src/ImageFilter.cpp
    src/ImageGenerator.cpp
    src/ImageInfo.cpp
    src/Mask.cpp
    src/MaskFilter.cpp
    src/Paint.cpp
    src/Path.cpp
    src/SdfGlyph.cpp
    src/PathEffect.cpp
    src/PathMeasure.cpp
    src/Picture.cpp
    src/Pixmap.cpp
    src/Shader.cpp
    src/Stream.cpp
    src/StrokeRec.cpp
    src/Surface.cpp
    src/TextBlob.cpp
    src/Types.cpp
    src/YUVAInfo.cpp
)

# ==================================================================================================
# The canvas shader set, embedded as SPIR-V
# ==================================================================================================
# The GPU canvas backend needs its shaders as SPIR-V *in the binary*, not as files it goes looking for at
# run time. A renderer that loads by path has to be told where they are, on every platform, including the
# ones where "a path" means something inside an APK or a bundle -- three platform layers whose only job is
# to answer a question this build already knows the answer to. A byte array has no answer to give and no
# failure mode: if the shader is not in the library, the program does not link to it.
#
# The sources live in the Graphics repository's `shaders/canvas/`, which is the "one shader source, every
# backend" directory the RHI's own `shaders/` target compiles. This target compiles the same sources itself
# rather than depending on that target, because a build has to be orderable: `modules/graphics` may be added
# before `shaders/`, and a target that depends on another's output has to say so.
#
# `shader_compiler` is a host tool, so it is only used when the host build produced it. Without it -- a
# cross build given no `CORAL_SHADERS_COMPILER`, or a configure with shaderc off -- the placeholder header
# at `src/gpu/CanvasShadersPlaceholder.h` is used instead and every shader lookup reports an invalid blob, which the
# renderer turns into `Result::Unsupported` rather than into a picture that is quietly missing its shapes.
set(CORAL_GRAPHICS_SHADER_SRC "${CMAKE_CURRENT_LIST_DIR}/shaders/canvas")
set(CORAL_GRAPHICS_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/canvas-shaders")
set(CORAL_GRAPHICS_VULKAN_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/canvas-shaders-vulkan")
set(CORAL_GRAPHICS_SHADER_HEADER "${CORAL_GRAPHICS_SHADER_OUT}/CanvasShaders.h")

set(CORAL_GRAPHICS_SHADER_COMPILER "${CORAL_SHADERS_COMPILER}")
if (DEFINED CORAL_BACKEND_SUPPORTS_SHADERC AND NOT CORAL_BACKEND_SUPPORTS_SHADERC)
  # A configure without shaderc cannot compile the canvas shaders: the tool exists but reports "no
  # GLSL compiler" for every input, which fails the build. Fall back to the committed embedded header
  # (the same SPIR-V the shader compiler produces), which is what a GL-only package ships.
  set(CORAL_GRAPHICS_SHADER_COMPILER "")
elseif (NOT CORAL_GRAPHICS_SHADER_COMPILER AND TARGET shader_compiler)
  set(CORAL_GRAPHICS_SHADER_COMPILER "$<TARGET_FILE:shader_compiler>")
endif ()

# The Vulkan-environment shader set is not the Vulkan backend's private property: Metal consumes the same set, because
# the `DescriptorSet`/`Binding` decorations it carries are what a Metal pipeline's slot table is built from. Gating the
# set on `CORAL_BACKEND_SUPPORTS_VULKAN` used to be right by accident -- the two generally came together -- but a
# Metal-only Apple build has Metal on and Vulkan off, and it must still embed this set.
set(CORAL_GRAPHICS_NEEDS_VULKAN_SHADER_SET OFF)
if (CORAL_BACKEND_SUPPORTS_VULKAN OR CORAL_BACKEND_SUPPORTS_METAL)
  set(CORAL_GRAPHICS_NEEDS_VULKAN_SHADER_SET ON)
endif ()

set(CORAL_GRAPHICS_EMBEDDED_SHADERS "")
set(CORAL_GRAPHICS_EMBEDDED_VULKAN_SHADERS "")
if (CORAL_GRAPHICS_SHADER_COMPILER AND EXISTS "${CORAL_GRAPHICS_SHADER_SRC}")
  file(GLOB CORAL_GRAPHICS_SHADER_SOURCES CONFIGURE_DEPENDS
      "${CORAL_GRAPHICS_SHADER_SRC}/*.vert"
      "${CORAL_GRAPHICS_SHADER_SRC}/*.frag"
      "${CORAL_GRAPHICS_SHADER_SRC}/*.comp"
  )

  foreach (shader IN LISTS CORAL_GRAPHICS_SHADER_SOURCES)
    get_filename_component(shaderName "${shader}" NAME)
    set(spirv "${CORAL_GRAPHICS_SHADER_OUT}/${shaderName}.spv")
    # One command per shader rather than one for the directory, so a shader that changed is the only one
    # recompiled and the two can compile in parallel.
    add_custom_command(
        OUTPUT "${spirv}"
        COMMAND ${CORAL_GRAPHICS_SHADER_COMPILER} "${shader}" -o "${spirv}"
        DEPENDS "${shader}" ${CORAL_GRAPHICS_SHADER_COMPILER}
        COMMENT "Compiling canvas shader ${shaderName} to SPIR-V"
        VERBATIM
    )
    list(APPEND CORAL_GRAPHICS_EMBEDDED_SHADERS "${spirv}")

    # ## The Vulkan set, from the same source
    #
    # The two modules are not interchangeable. The OpenGL environment accepts the shaders'
    # `layout(set = N, binding = M)` but emits no descriptor decorations for it, so a Vulkan pipeline
    # built from that module would see every resource at (0, 0). Compiling again with
    # `--environment vulkan` is what produces the decorations, and embedding both sets is what lets one
    # binary draw on either backend.
    if (CORAL_GRAPHICS_NEEDS_VULKAN_SHADER_SET)
      set(vulkanSpirv "${CORAL_GRAPHICS_VULKAN_SHADER_OUT}/${shaderName}.spv")
      add_custom_command(
          OUTPUT "${vulkanSpirv}"
          COMMAND ${CORAL_GRAPHICS_SHADER_COMPILER} "${shader}" --environment vulkan -o "${vulkanSpirv}"
          DEPENDS "${shader}" ${CORAL_GRAPHICS_SHADER_COMPILER}
          COMMENT "Compiling canvas shader ${shaderName} to Vulkan SPIR-V"
          VERBATIM
      )
      list(APPEND CORAL_GRAPHICS_EMBEDDED_VULKAN_SHADERS "${vulkanSpirv}")
    endif ()
  endforeach ()

  if (CORAL_GRAPHICS_EMBEDDED_SHADERS)
    # The embedder has two implementations with the same output. PowerShell is what the Windows hosts
    # already have; a normal Linux installation does not ship `pwsh`, so the Python script is used there.
    # Both write only when the content differs, so a no-op configure does not make every dependent object
    # look stale.
    #
    # Either script is handed the *directory* the SPIR-V landed in rather than the list, and that is
    # deliberate: CMake passes a list as separate command-line tokens, PowerShell binds only the first of
    # them to a `[string[]]` parameter, and a directory is one argument that needs no joining.
    find_program(CORAL_PWSH_EXECUTABLE NAMES pwsh powershell)
    if (CORAL_PWSH_EXECUTABLE)
      set(CORAL_SHADER_EMBED_COMMAND
          "${CORAL_PWSH_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
          -File "${CMAKE_CURRENT_LIST_DIR}/tools/embed_shaders.ps1"
          -SourceDir "${CORAL_GRAPHICS_SHADER_OUT}"
          -ShaderSourceDir "${CORAL_GRAPHICS_SHADER_SRC}")
      # The script rejects a directory it was told about but that does not exist, which is what makes a build that
      # asked for the set and embedded nothing a failure. So it is only named when the set was compiled.
      if (CORAL_GRAPHICS_NEEDS_VULKAN_SHADER_SET)
        list(APPEND CORAL_SHADER_EMBED_COMMAND -VulkanSourceDir "${CORAL_GRAPHICS_VULKAN_SHADER_OUT}")
      endif ()
      list(APPEND CORAL_SHADER_EMBED_COMMAND
          -CommittedFile "${CMAKE_CURRENT_LIST_DIR}/src/gpu/CanvasShadersEmbedded.h"
          -Output "${CORAL_GRAPHICS_SHADER_HEADER}")
      set(CORAL_SHADER_EMBED_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/tools/embed_shaders.ps1")
    else ()
      find_package(Python3 COMPONENTS Interpreter REQUIRED)
      set(CORAL_SHADER_EMBED_COMMAND
          "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/tools/embed_shaders.py"
          --source-dir "${CORAL_GRAPHICS_SHADER_OUT}"
          --shader-source-dir "${CORAL_GRAPHICS_SHADER_SRC}"
          --committed-file "${CMAKE_CURRENT_LIST_DIR}/src/gpu/CanvasShadersEmbedded.h"
          --output "${CORAL_GRAPHICS_SHADER_HEADER}")
      if (CORAL_GRAPHICS_NEEDS_VULKAN_SHADER_SET)
        list(APPEND CORAL_SHADER_EMBED_COMMAND
            --vulkan-source-dir "${CORAL_GRAPHICS_VULKAN_SHADER_OUT}")
      endif ()
      set(CORAL_SHADER_EMBED_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/tools/embed_shaders.py")
    endif ()

    add_custom_command(
        OUTPUT "${CORAL_GRAPHICS_SHADER_HEADER}"
        COMMAND ${CORAL_SHADER_EMBED_COMMAND}
        DEPENDS ${CORAL_GRAPHICS_EMBEDDED_SHADERS} ${CORAL_GRAPHICS_EMBEDDED_VULKAN_SHADERS}
        "${CORAL_SHADER_EMBED_SCRIPT}"
        COMMENT "Embedding ${CORAL_GRAPHICS_EMBEDDED_SHADERS} as CanvasShaders.h"
        VERBATIM
    )
  endif ()
else ()
  message(STATUS
      "graphics: no shader compiler, so this build does not compile `shaders/canvas/*` itself. The committed copy at "
      "src/gpu/CanvasShadersEmbedded.h is used instead, which draws the same picture; a build that edits a canvas shader "
      "must regenerate it, and the embed script refuses to run when the two disagree. Pass "
      "-DCORAL_SHADERS_COMPILER=<path to shader_compiler> to compile them here instead.")
endif ()

# ==================================================================================================
# Definition and includes
# ==================================================================================================
add_library(${TARGET} STATIC ${PUBLIC_HDRS} ${PRIVATE_HDRS} ${SRCS})


set(_coral_public_include "${PUBLIC_HDR_DIR}")
target_include_directories(${TARGET}
    PUBLIC $<BUILD_INTERFACE:${_coral_public_include}> $<INSTALL_INTERFACE:include>
    PRIVATE ${SRC_DIR}
)

# The generated header's directory comes *first*, so that a build which embedded its shaders always wins
# over the placeholder that is checked in beside the sources. `BEFORE` is load-bearing: without it the
# source directory's copy would be found first and the embedded shaders would never be seen.
if (CORAL_GRAPHICS_EMBEDDED_SHADERS)
  target_include_directories(${TARGET} BEFORE PRIVATE "${CORAL_GRAPHICS_SHADER_OUT}")
  # An explicit dependency, because `target_include_directories` does not create one and CMake cannot see
  # that a source file includes a generated header it does not name in `SRCS`.
  set_source_files_properties(${SRCS} PROPERTIES OBJECT_DEPENDS "${CORAL_GRAPHICS_SHADER_HEADER}")
endif ()
# The private headers are on this target's include path only. `SYSTEM` is deliberately not used:
# a warning from Coral's own internal header should be visible.
target_include_directories(${TARGET} PRIVATE ${PRIVATE_HDR_DIR})

set_target_properties(${TARGET} PROPERTIES FOLDER Libs)

# Every source in this library is UTF-8 without a BOM. MSVC otherwise decodes source files using the
# system code page, which both warns (C4819) and can mis-read multi-byte characters in comments; on
# some locales that mis-parse ends up reported as a spurious syntax error in an unrelated line.
if (MSVC)
  target_compile_options(${TARGET} PRIVATE /utf-8)
endif ()

# ==================================================================================================
# Dependencies
# ==================================================================================================
# PUBLIC on purpose. A client that links `gpu` gets a working facade; it also gets the backend's
# include directories, because `gpu/` and `backend/` are one installed package with one include tree,
# and a caller that needs the raw device for an operation the facade does not wrap should be able to
# include <backend/RHI.h> without changing its link line. The *archive* still stays an implementation
# detail: `gpu` is a static library, so a client resolves the backend's symbols through it rather than
# naming the target.
target_link_libraries(${TARGET} PUBLIC ${BACKEND_TARGET})

# PUBLIC, and load-bearing: graphics/Paint.h includes <geometry/Color.h>, so every client of `graphics`
# needs geometry's include directory to compile that header. A PRIVATE link would let this library build
# and then break each consumer individually, which is the most confusing possible failure mode.
target_link_libraries(${TARGET} PUBLIC geometry)

# An explicit dependency, not just a link.
#
# `target_link_libraries` alone does not express this: the facade's translation units include backend
# *type* headers but none of the backend's implementation, so a change confined to the backend - a fix
# in GlslCompiler.cpp, say - leaves every `gpu` object file "up to date" and nothing relinks. The
# symptom is the worst kind: an executable that keeps running yesterday's backend while the tree says
# otherwise, which is exactly how a fixed bug appears to still be present.
add_dependencies(${TARGET} ${BACKEND_TARGET})

target_link_libraries(${TARGET} PUBLIC utils)
target_link_libraries(${TARGET} PRIVATE $<BUILD_INTERFACE:minilog>)

if (WIN32)
  # `IDXGIOutput::WaitForVBlank` in the FrameClock's Windows vblank source, and DirectWrite for
  # platform ClearType glyph coverage.
  target_link_libraries(${TARGET} PRIVATE dxgi dwrite)
endif ()

if (UNIX AND NOT APPLE)
  # The X11 per-window vblank source (PresentNotifyMSC); Wayland's frame callback is loaded with
  # dlopen so libwayland-client stays optional.
  find_package(X11 QUIET)
  if (X11_FOUND)
    target_link_libraries(${TARGET} PRIVATE $<BUILD_INTERFACE:X11::X11>)
    target_compile_definitions(${TARGET} PRIVATE CORAL_HAVE_X11=1)
  endif ()
  target_link_libraries(${TARGET} PRIVATE ${CMAKE_DL_LIBS})
  # Fontconfig's FC_RGBA tells the FreeType LCD path whether the panel is RGB or BGR. Optional: a
  # minimal Linux build without fontconfig keeps the conventional RGB default.
  find_package(PkgConfig QUIET)
  if (PkgConfig_FOUND)
    pkg_check_modules(FONTCONFIG QUIET IMPORTED_TARGET fontconfig)
    if (FONTCONFIG_FOUND)
      target_link_libraries(${TARGET} PRIVATE $<BUILD_INTERFACE:PkgConfig::FONTCONFIG>)
      target_compile_definitions(${TARGET} PRIVATE CORAL_GRAPHICS_HAS_FONTCONFIG=1)
    endif ()
  endif ()
endif ()

# The image codecs. PRIVATE because libpng is an implementation detail of the codec: a client gets
# pixels, not a `png_structp`, and keeping the include directory out of the public interface is what
# makes it possible to swap a format's library without an ABI change.
if (TARGET png)
  target_sources(${TARGET} PRIVATE src/codec/PngCodec.cpp)
  target_link_libraries(${TARGET} PRIVATE $<BUILD_INTERFACE:png>)
  target_compile_definitions(${TARGET} PRIVATE CORAL_GRAPHICS_HAS_LIBPNG=1)
endif ()

if (TARGET jpeg-static)
  target_sources(${TARGET} PRIVATE src/codec/JpegCodec.cpp)
  target_link_libraries(${TARGET} PRIVATE $<BUILD_INTERFACE:jpeg-static>)
  # The include directory belongs to the consumer rather than to the vendored target: libjpeg-turbo
  # exports its own targets and CMake refuses an export whose interface names a build-tree path. This
  # target is not exported, so it is free to name one.
  # Two directories, not one: libjpeg-turbo's own headers are in its source tree, but `jconfig.h` is
  # *generated* by its configure step. Where it lands depends on how deeply the wrapper's binary
  # directory is nested, which differs between the application root and this repository -- so the path is asked of
  # the target rather than guessed, and a wrong guess surfaces as `jpeglib.h` failing on its first
  # include rather than as anything that points at the real cause.
  target_include_directories(${TARGET} PRIVATE
      "${EXTERNAL}/libjpeg-turbo"
      "$<TARGET_PROPERTY:jpeg-static,BINARY_DIR>")
  # The codec's own switch, not the root's option: the option says what the user asked for, this says
  # what the linker actually got. A build configured with the option on but the subdirectory skipped
  # would otherwise compile a decoder that references symbols no library provides.
  target_compile_definitions(${TARGET} PRIVATE CORAL_GRAPHICS_HAS_JPEG=1)
endif ()

# Text shaping and font loading. Both are PRIVATE: a client gets glyphs and metrics, not an `FT_Face`.
# The include directories go on this target rather than on the vendored ones, for the same export reason
# as libjpeg-turbo and libwebp above.
if (TARGET harfbuzz)
  target_link_libraries(${TARGET} PRIVATE $<BUILD_INTERFACE:harfbuzz>)
  target_include_directories(${TARGET} PRIVATE "${EXTERNAL}/harfbuzz/src")
  target_compile_definitions(${TARGET} PRIVATE CORAL_GRAPHICS_HAS_HARFBUZZ=1)
endif ()

if (TARGET freetype)
  target_link_libraries(${TARGET} PRIVATE $<BUILD_INTERFACE:freetype>)
  # freetype's headers are consumed as <ft2build.h>, which lives one level down in its include tree.
  target_include_directories(${TARGET} PRIVATE "${EXTERNAL}/freetype/include")
  target_compile_definitions(${TARGET} PRIVATE CORAL_GRAPHICS_HAS_FREETYPE=1)
endif ()

# The platform image decoder.
#
# On Android, decoding through the system's own `BitmapFactory` keeps the shared object smaller than
# linking a second copy of libjpeg-turbo and libpng for the device's benefit, and the device's decoder
# has the hardware paths. The switch is `CORAL_BUILD_FOR_ANDROID`, and it is defined here rather than
# left to the caller: nothing else defining it meant the system branch in the codec was dead code
# that could never be reached, however the project was configured.
if (ANDROID)
  target_sources(${TARGET} PRIVATE "src/codec/AndroidCodec.cpp")
  target_compile_definitions(${TARGET} PRIVATE CORAL_BUILD_FOR_ANDROID=1)
  # `jni.h` itself ships with the NDK's sysroot and is found through the toolchain, so no include
  # directory is added for it; a build that cannot see it is one that is not an Android build.
  #
  # `jni` is the project's own module, and it is what owns the `JavaVM`: this target no longer defines
  # `JNI_OnLoad` itself, because the Android application does, and two definitions in one shared object
  # is a duplicate symbol. Linking the module also carries its `include` directory, which is where
  # `<jni/JvmEnv.h>` comes from -- the module exports it as PUBLIC for exactly this reason.
  target_link_libraries(${TARGET} PRIVATE $<BUILD_INTERFACE:jni> jnigraphics)
endif ()

if (TARGET webp)
  target_sources(${TARGET} PRIVATE src/codec/WebpCodec.cpp)
  target_link_libraries(${TARGET} PRIVATE $<BUILD_INTERFACE:webp>)
  # Same reasoning as libjpeg-turbo above: libwebp's headers are consumed as <webp/decode.h> from the
  # `src` directory at its root, and the vendored target cannot carry that path through an export.
  target_include_directories(${TARGET} PRIVATE "${EXTERNAL}/libwebp/src")
  target_compile_definitions(${TARGET} PRIVATE CORAL_GRAPHICS_HAS_WEBP=1)
endif ()

# nlohmann/json is what the Lottie reader parses a document with. It is header-only, so this is an
# interface dependency rather than a link, and the define is what lets the translation compile in a tree
# without it -- the same arrangement the codec formats use, because a requirement that a vendored library
# be present is a requirement on how the project was cloned rather than on the code.
if (TARGET nlohmann_json)
  # The source that reads a document is added here rather than listed above, which is the same arrangement the codec
  # uses for WebP: a file that includes a header belonging to a library the tree may not have cannot be compiled when
  # the library is absent, and a preprocessor guard inside it would be a second way of saying the same thing.
  target_sources(${TARGET} PRIVATE src/lottie/LottieParser.cpp)
  target_link_libraries(${TARGET} PRIVATE $<BUILD_INTERFACE:nlohmann_json>)
  target_compile_definitions(${TARGET} PRIVATE CORAL_GRAPHICS_HAS_JSON=1)
else ()
  # And something has to answer for `parseAnimation` either way, because `Lottie.cpp` calls it. This reports why it
  # cannot rather than failing to link, so a build without the dependency produces a program that says so.
  target_sources(${TARGET} PRIVATE src/lottie/LottieParserUnavailable.cpp)
endif ()

# CADisplayLink is the display tick on both Apple platforms, and CAMetalDisplayLink is the Metal
# presentation driver. Both live in QuartzCore; the macOS tick also needs AppKit to ask a screen or a
# view for its link. The Objective-C++ sources are added here rather than to SRCS because they are
# Apple-only and a non-Apple configure must not try to compile them.
if (APPLE)
  target_sources(${TARGET} PRIVATE
      src/gpu/FrameClockApple.mm
      src/gpu/MetalDisplayLink.mm)
  # The two Apple files own Objective-C objects (CADisplayLink, CAMetalDisplayLink, the delegate).
  # ARC is per source in this tree, as it is for the Metal backend, so the flag is attached to these
  # files rather than to the target.
  set_source_files_properties(
      src/gpu/FrameClockApple.mm
      src/gpu/MetalDisplayLink.mm
      PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
  find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
  find_library(METAL_FRAMEWORK Metal REQUIRED)
  target_link_libraries(${TARGET} PRIVATE
      $<BUILD_INTERFACE:${QUARTZCORE_FRAMEWORK}>
      $<BUILD_INTERFACE:${FOUNDATION_FRAMEWORK}>
      $<BUILD_INTERFACE:${METAL_FRAMEWORK}>)
  if (NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    target_link_libraries(${TARGET} PRIVATE $<BUILD_INTERFACE:${APPKIT_FRAMEWORK}>)
  endif ()
endif ()

# ==================================================================================================
# Installation
# ==================================================================================================
# The two layers ship as one package with one include tree, even though they are two targets: a
# consumer should see `include/gpu/` and `include/backend/`, not two separate installations.
install(TARGETS ${TARGET} EXPORT CoralUITargets ARCHIVE DESTINATION lib)

install(DIRECTORY ${PUBLIC_HDR_DIR}/codec DESTINATION include)
install(DIRECTORY ${PUBLIC_HDR_DIR}/gpu DESTINATION include)
install(DIRECTORY ${PUBLIC_HDR_DIR}/graphics DESTINATION include)
# The two translations' public headers. Installed for the same reason the other three are: a client names
# `graphics::Svg` and `graphics::Lottie`, and a package that shipped the library without them would ship a library
# whose new half could not be used.
install(DIRECTORY ${PUBLIC_HDR_DIR}/svg DESTINATION include)
install(DIRECTORY ${PUBLIC_HDR_DIR}/lottie DESTINATION include)
# The backend's public headers are installed from the subdirectory rather than from this
# one, because that is where they live and where the subdirectory already declares them.
install(DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/backend/include/backend DESTINATION include)
install(FILES "README.md" DESTINATION .)

# ==================================================================================================
# Developer tools and probes
# ==================================================================================================
# Not examples -- the root examples/ project is that -- and not part of the default build: they answer
# maintenance and measurement questions about this module's internals, and a build that iterates on the
# RHI or the rasterizer turns them on deliberately.
#
#   cmake -S . -B build -DCORAL_GRAPHICS_BUILD_DEV_TOOLS=ON
option(CORAL_GRAPHICS_BUILD_DEV_TOOLS "Build the graphics probes and shader maintenance tools" OFF)

if (CORAL_GRAPHICS_BUILD_DEV_TOOLS)
  # The probes print a report rather than a picture: effect_spike answers whether the RHI carries a
  # caller-supplied multi-sampler pass into an offscreen target with the orientation a transition needs,
  # and field_probe breaks a frame's coverage-field time down by contributor.
  foreach (probe effect_spike field_probe)
    add_executable(${probe} "tools/probes/${probe}.cpp")
    target_link_libraries(${probe} PRIVATE ${TARGET})
    set_target_properties(${probe} PROPERTIES FOLDER Tests)
  endforeach ()

  # effect_orient_probe reads the generated canvas shader module and two backend translation internals,
  # so it exists only when this build compiled the shaders itself.
  if (CORAL_GRAPHICS_EMBEDDED_SHADERS)
    add_executable(effect_orient_probe tools/probes/effect_orient_probe.cpp)
    target_include_directories(effect_orient_probe PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/backend/src"
        "${CORAL_GRAPHICS_SHADER_OUT}")
    target_link_libraries(effect_orient_probe PRIVATE ${TARGET})
    set_target_properties(effect_orient_probe PROPERTIES FOLDER Tests)
  else ()
    message(STATUS "graphics: effect_orient_probe needs the generated canvas shaders; build with a shader compiler to get it")
  endif ()

  # Regenerates `src/gpu/CanvasShadersGLSL.h`, the OpenGL backend's canvas shader set for builds without
  # SPIRV-Cross. It needs both halves of the normal toolchain (shaderc to compile the Vulkan-dialect source
  # and SPIRV-Cross to translate it to the GL dialects), so it only exists when both are configured.
  #
  #   cmake --build build --config Release --target canvas_glsl_generator
  #   build/bin/Release/canvas_glsl_generator --source-dir modules/graphics/shaders/canvas \
  #       --output modules/graphics/src/gpu/CanvasShadersGLSL.h
  if (TARGET backend AND CORAL_BACKEND_SUPPORTS_SPIRV_CROSS AND CORAL_BACKEND_SUPPORTS_SHADERC)
    add_executable(canvas_glsl_generator tools/canvas_glsl_generator/canvas_glsl_generator.cpp)
    target_include_directories(canvas_glsl_generator PRIVATE "${CMAKE_CURRENT_LIST_DIR}/backend/src")
    target_link_libraries(canvas_glsl_generator PRIVATE backend)
    set_target_properties(canvas_glsl_generator PROPERTIES
        FOLDER Tests
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  endif ()
endif ()

# ==================================================================================================
# Tests
# ==================================================================================================
# The facade's own tests. They cover what the facade adds - resource lifetime, the count and stride
# bookkeeping, shader reflection in the public vocabulary - and they need a device, so they live
# behind the same host-platform guard as the backend's.
#
# `enable_testing()` belongs in *this* directory, not only in the build root. CMake writes a
# `CTestTestfile.cmake` for a directory only when `enable_testing()` has been called there, and the top-level file
# reaches the rest through `subdirs()`; a directory with `add_test` and no `enable_testing()` is simply not visited, so
# its tests never appear. Every other library in the tree calls it here. This one did not, which is why the three
# graphics suites were absent from `ctest` while their executables were built and run by hand.
if (CORAL_BUILD_TESTS)
  # See the note above: without this call the `add_test` below is written into a file that the top-level CTest file
  # never visits, and the suite is invisible to `ctest` while still being built.
  enable_testing()
  add_executable(test_${TARGET}
      tests/test_asserts.h
      tests/test_ColorFilter.cpp
      tests/test_ColorSpace.cpp
      tests/test_ColorSpaceXform.cpp
      tests/test_Data.cpp
      tests/test_Font.cpp
      tests/test_FontManager.cpp
      tests/test_Boolean.cpp
      tests/test_Atlas.cpp
      tests/test_Canvas.cpp
      tests/test_Curve.cpp
      tests/test_EdgeCoverage.cpp
      tests/test_Path.cpp
      tests/test_MaskFilter.cpp
      tests/test_PathEffect.cpp
      tests/test_PathMeasure.cpp
      tests/test_PathMeasureSegment.cpp
      tests/test_Picture.cpp
      tests/test_Pixmap.cpp
      tests/test_SamplingOptions.cpp
      tests/test_Shader.cpp
      tests/test_ShaderImage.cpp
      tests/test_CanvasLayer.cpp
      tests/test_CanvasMask.cpp
      tests/test_Stream.cpp
      tests/test_StrokeRec.cpp
      tests/test_Surface.cpp
      tests/test_Tables.cpp
      tests/test_YUVAInfo.cpp
      tests/test_IccProfile.cpp
      tests/test_Gpu.cpp
      tests/test_GpuDamage.cpp
      tests/test_FrameClock.cpp
      tests/test_WindowSurface.cpp
      tests/test_GlyphPath.cpp
      tests/test_DecodeLimits.cpp
      tests/test_ImageCodec.cpp
      tests/test_ImageFilter.cpp
      tests/test_ImageGenerator.cpp
      tests/test_Intersection.cpp
      tests/test_PathOps.cpp
      tests/test_Xml.cpp
      tests/test_SvgTypes.cpp
      tests/test_SvgPresentation.cpp
      tests/test_SvgResources.cpp
      tests/test_SvgRender.cpp
      tests/test_SvgGradient.cpp
      tests/test_SvgClipLayer.cpp
      tests/test_SvgMask.cpp
      tests/test_SvgDataUri.cpp
      tests/test_SvgImage.cpp
      tests/test_SvgPattern.cpp
      tests/test_SvgFilter.cpp
      tests/test_SvgFilterRender.cpp
      tests/test_LottieValue.cpp
      tests/test_LottieParser.cpp
      tests/test_LottieRender.cpp
      tests/test_LottieGradientMatte.cpp
      tests/test_LottieGeometry.cpp
      tests/test_LottieTrim.cpp
      tests/test_LottieRepeater.cpp
      tests/test_facade.cpp
      tests/test_main.cpp
  )
  target_link_libraries(test_${TARGET} PRIVATE ${TARGET} gtest)

  # The colour tests reach into `gcms` on purpose: the primitives are what the facade is built from,
  # and testing them only through the facade would leave the arithmetic itself uncovered.
  #
  # Both include paths are needed, and for different headers. `PRIVATE_HDR_DIR` is where the headers shared
  # across `src/` directories live -- `gcms/` and `pathops/` among them -- and `SRC_DIR` is where the ones
  # confined to a single directory still are, `pathops/Intersection.h` being the one the tests include.
  target_include_directories(test_${TARGET} PRIVATE ${PRIVATE_HDR_DIR} ${SRC_DIR})

  # The shader case reads the same SPIR-V fixture the backend's suite uses, so that both layers are
  # tested against one real compiler-produced module rather than two that could drift apart.
  target_compile_definitions(test_${TARGET} PRIVATE
      CORAL_BACKEND_FIXTURE_DIR="${CMAKE_CURRENT_LIST_DIR}/backend/tests/fixtures"
      # The codec tests decode a PNG written by an independent encoder -- Python's zlib and a
      # hand-written chunk writer. Keeping it as a file rather than an array in the source means the
      # test cannot be broken by a transcription mistake, which is exactly the failure it exists to
      # rule out.
      CORAL_GRAPHICS_FIXTURE_DIR="${CMAKE_CURRENT_LIST_DIR}/tests/fixtures"
      # The SVG and Lottie tests read the real documents under `resources/`, by path rather than by copying them into
      # the source. A parser tested only against strings written in the test file is tested against what its author
      # believed SVG looks like, and the whole point of parsing a format is that other people write the files.
      CORAL_GRAPHICS_RESOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}/resources")

  set_target_properties(test_${TARGET} PROPERTIES FOLDER Tests)

  if (MSVC)
    target_compile_options(test_${TARGET} PRIVATE /utf-8 /W3)
  else ()
    target_compile_options(test_${TARGET} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif ()

  # Registered with CTest, which this suite was not.
  #
  # Every other library in the tree adds its tests here; these three did not, so `ctest` in the build root ran fourteen
  # tests and none of the graphics ones -- including the suite for the whole facade. The executables were built and were
  # run by hand, which is how they passed and how the omission went unnoticed: a build that produces a test binary looks
  # like a suite that is tested.
  #
  # `enable_testing()` is called at the top level now, which is what makes a registration here reachable from the build
  # root -- see the note in the root CMakeLists. Before that it would have been a third silent no-op.
  add_test(NAME test_${TARGET} COMMAND test_${TARGET})

  # The suite reads PNG and SPIR-V fixtures from the source tree, so it has to run from a known directory rather than
  # from wherever `ctest` was invoked. Without this it passes when run from the build root by hand -- which is what has
  # always happened -- and would fail through CTest, which is the sort of difference that gets a test disabled.
  set_tests_properties(test_${TARGET} PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}")
  # The parser fuzz harness. Built with the suite so its deterministic corpus runs on every host
  # build; a sanitizer build compiles the same file with `CORAL_FUZZ_WITH_LIBFUZZER` and gets the
  # libFuzzer entry point instead of the `main`.
  add_executable(fuzz_parsers "${CMAKE_CURRENT_LIST_DIR}/tests/fuzz/fuzz_parsers.cpp")
  target_link_libraries(fuzz_parsers PRIVATE ${TARGET})
  set_target_properties(fuzz_parsers PROPERTIES FOLDER Tests)
  add_test(NAME fuzz_parsers_smoke COMMAND fuzz_parsers)
  set_tests_properties(fuzz_parsers_smoke PROPERTIES LABELS fuzz TIMEOUT 120)

  # The pathops fuzzer and its input shrinker. Deliberately not registered with CTest: they take a seed
  # and an iteration count and are run on purpose, often under the sanitizer, rather than on every run.
  add_executable(fuzz_pathops "${CMAKE_CURRENT_LIST_DIR}/tests/fuzz/fuzz_pathops.cpp")
  target_link_libraries(fuzz_pathops PRIVATE ${TARGET})
  set_target_properties(fuzz_pathops PROPERTIES FOLDER Tests)

  # The shrinker deliberately leaks a thread per accepted candidate -- the call it is testing never
  # returns, so neither can the worker it runs on.
  add_executable(shrink_pathops "${CMAKE_CURRENT_LIST_DIR}/tests/fuzz/shrink_pathops.cpp")
  target_link_libraries(shrink_pathops PRIVATE ${TARGET})
  set_target_properties(shrink_pathops PROPERTIES FOLDER Tests)
endif ()
