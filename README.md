# CoralGraphics

The graphics framework: the `gpu` facade, the backend RHI, geometry, image codecs, fonts, the SVG and
Lottie readers, the shader toolchain, and the `effect` render-graph module. It is one repository in two
roles:

- the application adds it as a framework subproject -- the application has already added CoralCore and
  the third-party targets, so this root only adds the modules;
- a standalone checkout configures from this root for independent development, and configures
  `examples/` for the demos.

## Layout

```
modules/graphics/     the framework: facade, backend RHI, geometry, codecs, fonts, SVG/Lottie, tests
modules/effect/       the effect render-graph module and its tests
examples/             the demo and verification project (graphics and effect demos)
cmake/                CoralBuild.cmake (the shared settings) and the standalone dependency manifest
third_party/          the vendored libraries, including the libz / libyuv / libgtest submodules
external/CoralCore/   the foundation framework, as a submodule
```

`modules/graphics/README.md` documents the library's design and rules; `examples/README.md` documents
the demo project.

## Building standalone

The framework bundles everything it needs as git submodules: CoralCore, the graphics third-party tree,
and the three libraries that are shared with the other frameworks (`libz`, `libyuv`, `libgtest`) under
`third_party/`.

```sh
git submodule update --init --recursive
```

Then configure for the host. Windows and macOS use the generator each one ships with; Linux uses Ninja.

```sh
# Windows -- Visual Studio 17 2022
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure

# macOS -- the Command Line Tools' make (Ninja works too if it is installed)
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
ctest --test-dir build \
      -R "test_(graphics|backend|geometry|effect)|effect_(demo|public_header_boundary)|fuzz_parsers_smoke" \
      --output-on-failure

# Linux
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The submodules are pinned commits, so a standalone build is reproducible and needs no network access
after the initial checkout.

On macOS the prerequisites are the Xcode Command Line Tools (`xcode-select --install`, which also
provides the Python 3 the shader-embedding step uses) and CMake 3.19 or newer (Homebrew's `cmake`, or
the CMake.app bundle). **Metal is the default backend**: `CORAL_BACKEND_SUPPORTS_METAL` is on for an
Apple configure, SPIR-V shaders are translated to MSL with SPIRV-Cross, and the OpenGL backend remains
compiled in as the fallback. Vulkan is compiled in but the system has no Vulkan loader, so the Vulkan
cases skip themselves rather than fail. `test_backend` prints the negotiated device for every backend
it can create (`Apple M...` / `Metal 2.2+` / `coral/backend/metal`, and `OpenGL 4.1 core` / `CGL`) which
is the quickest way to confirm both are real:

```sh
# The Metal backend on its own, the way a regression run pins it
CORAL_RHI_BACKEND=metal ./build/modules/graphics/backend/test_backend
CORAL_RHI_BACKEND=opengl ./build/modules/graphics/backend/test_backend

# The whole suite, macOS
ctest --test-dir build -R "test_(graphics|backend|geometry)|effect_(demo|public_header_boundary)" \
      --output-on-failure
```

`modules/graphics/backend/src/metal/` holds the implementation and
`Metal后端实现进度与问题记录.md` records its status, its design decisions, its verified results on macOS
and the iOS simulator, and what it does not do yet.

The macOS `ctest` line names this repository's suites rather than running everything: a standalone
tree also registers the vendored libjpeg-turbo command-line tests, whose executables the codec wrapper
does not build, so a plain `ctest` reports them as `Not Run`. The named set above is the complete
CoralGraphics check -- facade, backend, geometry, effect, shader/fuzz smoke and the offscreen
`effect_demo`.

### iOS

iOS is a cross build from a macOS host with full Xcode (the Command Line Tools alone have no iOS SDK).
Its default backend is Metal (`CAMetalLayer` for presentation, `MTLDevice` for everything else), and its
OpenGL backend is OpenGL ES 3.0 through EAGL, not EGL, so that platform layer is `GlPlatformEagl.mm`; an
offscreen device needs no window and the tests run in the simulator.

**If `xcode-select -p` still reports the Command Line Tools**, `xcrun --sdk iphonesimulator` and
`xcrun metal` fail even though Xcode is installed. Either select Xcode once
(`sudo xcode-select -s /Applications/Xcode.app`) or prefix the commands below with
`DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`. The Metal backend compiles MSL at run time
with `newLibraryWithSource:`, so the Metal toolchain's offline compiler is not a build prerequisite; the
SDK headers are.

```sh
# The build-time shader compiler is a host tool. Build it in the host tree first so the iOS build
# embeds freshly compiled canvas shaders instead of the committed copy.
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8 --target shader_compiler

cmake -S . -B build-ios -G "Unix Makefiles" \
      -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_SYSTEM_PROCESSOR=arm64 \
      -DCMAKE_OSX_SYSROOT=iphonesimulator \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
      -DCMAKE_BUILD_TYPE=Release \
      -DCORAL_SHADERS_COMPILER="$PWD/build/modules/graphics/tools/shader_compiler/shader_compiler"
cmake --build build-ios -j8
```

Each test target is an `.app` bundle, so run it through the simulator rather than from the host shell:

```sh
xcrun simctl spawn booted build-ios/modules/graphics/backend/test_backend.app/test_backend
xcrun simctl spawn booted build-ios/modules/graphics/test_graphics.app/test_graphics
xcrun simctl spawn booted build-ios/modules/effect/test_effect.app/test_effect
xcrun simctl spawn booted build-ios/modules/graphics/geometry/test_geometry.app/test_geometry
```

`test_backend` prints the negotiated device (`OpenGL ES 3.0`, `EAGL`) and the GPU tests render through
the backend's framebuffer objects. A physical-device configure uses `-DCMAKE_OSX_SYSROOT=iphoneos` and
adds the application's signing settings; the simulator is what the commands above verify. The
`GpuSurface.TheWholeSvgCorpusInTheDemoGridMatchesTheRasterizer` comparison is skipped on the simulator
because its software ES renderer does not reproduce the filter/image-heavy SVG corpus pixel for pixel;
the individual SVG tests still run there.

### When CoralGraphics is used as a submodule of a project

The application adds its own `libz`, `libyuv`, `libgtest` and CoralCore before it adds this framework.
Every dependency this repository bundles is added behind a `NOT TARGET` guard, so the application's
versions are the ones the framework links and the bundled submodules are not compiled at all. That is
the intended arrangement for a project that consumes CoralGraphics as a submodule: one copy of each
shared library, owned by the application.

A project that keeps the three libraries somewhere else can still point at them explicitly:

```sh
cmake -S . -B build -DCORAL_SHARED_THIRD_PARTY_DIR=<application third_party>
```

`-DCORAL_BUILD_TESTS=OFF` configures the libraries without GoogleTest. The feature switches (JPEG, WebP,
fonts, JSON, OpenGL, Vulkan, shaderc, SPIRV-Cross, effect) are described in `CMakeLists.txt`; the
manifest in `cmake/CoralGraphicsDependencies.cmake` documents where each dependency is looked for.

## The demos

The demos live in `examples/`. A top-level configure of this root adds them, so one build produces the
libraries, their tests and the demo checks:

```sh
# Windows
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure

# macOS
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
ctest --test-dir build \
      -R "test_(graphics|backend|geometry|effect)|effect_(demo|public_header_boundary)|fuzz_parsers_smoke" \
      --output-on-failure
```

The project is also configurable on its own, which keeps the framework build to libraries and tests:

```sh
# Windows
cmake -S examples -B out/examples -G "Visual Studio 17 2022" -A x64
cmake --build out/examples --config Release
ctest --test-dir out/examples -C Release --output-on-failure

# macOS
cmake -S examples -B out/examples -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build out/examples -j8
ctest --test-dir out/examples --output-on-failure
```

`graphics_demo` writes a PNG rather than opening a window, so it runs anywhere; the Vulkan verification,
the windowed Lottie players and the effect demos skip (exit code 77) when the device or display they
need is not there. On macOS the demo set is `mac_demo` (a Cocoa window over CGL/OpenGL 4.1, presenting
through `gpu::WindowSurface`) plus the effect demos (`effect_demo`, `effect_benchmark` and the macOS
verification `effect_mac_demo`). The macOS `examples/graphics` window is interactive; `--verify` is the
offscreen path for a build machine:

```sh
out/examples/bin/mac_demo                          # windowed, runs until closed or Esc/Q
out/examples/bin/mac_demo --frames 120 --immediate
out/examples/bin/mac_demo --frames 1 --snapshot out/mac_demo.png   # windowed, then read the frame back
out/examples/bin/mac_demo --verify --backend opengl

out/examples/bin/effect_mac_demo                   # Metal + OpenGL offscreen: colour bars and cross-backend match
out/examples/bin/effect_mac_demo --backend metal   # pinned; a fallback to another backend is a failure
```

`effect_mac_demo` renders the deterministic colour-bar graph through the effect engine on every backend the
machine can bring up, checks the bars at their centres and compares the backends' frames, so a backend that
draws a wrong or blank picture fails with the pixel named. It is registered with CTest together with a pinned
row per backend.

There is no macOS `graphics_demo`/`lottie_player`: those two are the Win32 and X11/Wayland window
implementations, while `mac_demo` is the Cocoa counterpart.

## Developer tools

The probes (`effect_spike`, `field_probe`, `effect_orient_probe`) and the canvas-shader generator are
not part of the default build:

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCORAL_GRAPHICS_BUILD_DEV_TOOLS=ON
cmake --build build --config Release --target effect_spike field_probe canvas_glsl_generator

# macOS
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DCORAL_GRAPHICS_BUILD_DEV_TOOLS=ON
cmake --build build -j8 --target effect_spike field_probe canvas_glsl_generator
```

The pathops and parser fuzzers build with `-DCORAL_BUILD_TESTS=ON` and live in
`modules/graphics/tests/fuzz/`.

## Using it as a subproject

```cmake
add_subdirectory(CoralCore)      # provides utils and minilog
add_subdirectory(Dependencies)   # provides png, z, yuv, gtest, ...
add_subdirectory(CoralGraphics)  # provides graphics, backend, geometry, effect
target_link_libraries(my_target PRIVATE graphics)
```

As a subproject the framework does not build its tests unless the caller sets `CORAL_BUILD_TESTS`.
