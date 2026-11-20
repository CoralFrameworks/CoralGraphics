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

cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The submodules are pinned commits, so a standalone build is reproducible and needs no network access
after the initial checkout.

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
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The project is also configurable on its own, which keeps the framework build to libraries and tests:

```sh
cmake -S examples -B out/examples -G "Visual Studio 17 2022" -A x64
cmake --build out/examples --config Release
ctest --test-dir out/examples -C Release --output-on-failure
```

`graphics_demo` writes a PNG rather than opening a window, so it runs anywhere; the Vulkan verification,
the windowed Lottie players and the effect demos skip (exit code 77) when the device or display they
need is not there.

## Developer tools

The probes (`effect_spike`, `field_probe`, `effect_orient_probe`) and the canvas-shader generator are
not part of the default build:

```sh
cmake -S . -B build -DCORAL_GRAPHICS_BUILD_DEV_TOOLS=ON
cmake --build build --config Release --target effect_spike field_probe canvas_glsl_generator
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
