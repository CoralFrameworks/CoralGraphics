# CoralGraphics examples

Demo and verification programs built as a client of the framework: they include `<graphics/...>`,
`<gpu/...>` and `<effect/...>` and nothing private. The project configures in two modes: on its own
(shown below), or as a subdirectory of the framework root, which adds it when the root is top-level.
This keeps the framework build to libraries and tests while one root configure can still build and test
every demo.

```
examples/
├── CMakeLists.txt        # the project: dependencies, then the framework, then the demos
├── graphics/
│   ├── win/              # the console scene demo, the Vulkan verification demo, the Lottie player
│   └── linux/            # the X11/Wayland demo and the Lottie player
└── effect/               # the effect render-graph demos and its benchmark
```

## Configuring

The framework's dependencies have to exist before the demos name them, the same way the application
root adds its dependency graph before its frameworks. `examples/CMakeLists.txt` includes
`cmake/CoralGraphicsDependencies.cmake` for that reason.

Two checkouts have to be initialized:

- **CoralCore** -- `utils` and `minilog`, from the `external/CoralCore` submodule (or a sibling
  `../CoralCore` checkout, or `-DCORAL_CORE_DIR=<path>`).
- **`libz`, `libyuv` and `libgtest`** -- this repository's own pinned submodules under `third_party/`.
  They are shared with the other frameworks, so a project that consumes CoralGraphics as a submodule
  normally provides its own copies; this project uses the bundled ones when building on its own.

  ```sh
  git submodule update --init --recursive

  # Windows
  cmake -S examples -B out/examples -G "Visual Studio 17 2022" -A x64

  # macOS
  cmake -S examples -B out/examples -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
  ```

  To use a different copy instead of the bundled submodules, pass
  `-DCORAL_SHARED_THIRD_PARTY_DIR=<application third_party>` (for `libz`/`libyuv`/`libgtest`) and
  `-DCORAL_GTEST_DIR=<path>` for GoogleTest alone.

On Linux the X11/Wayland demos additionally need `libx11-dev`, `libwayland-dev`, `libwayland-bin`,
`wayland-protocols`, `libegl-dev` and `libxkbcommon-dev`.

## Building and running

```sh
# Windows
cmake -S examples -B out/examples -G "Visual Studio 17 2022" -A x64
cmake --build out/examples --config Release

out/examples/bin/Release/graphics_demo.exe --resources modules/graphics/resources --output demo.png
out/examples/bin/Release/vulkan_demo.exe --verify --output out/vulkan
out/examples/bin/Release/lottie_player.exe --animation modules/graphics/resources/apitest/lottie/gear.json

# macOS
cmake -S examples -B out/examples -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build out/examples -j8

out/examples/bin/mac_demo                          # windowed; Esc/Q or close the window to exit
out/examples/bin/mac_demo --frames 120 --immediate
out/examples/bin/mac_demo --frames 1 --snapshot out/mac_demo.png   # windowed, then read the frame back
out/examples/bin/mac_demo --verify --backend opengl
```

`graphics_demo` writes a PNG rather than opening a window, so it runs on a build machine, in a
container, or in a session with no display. The windowed programs and the effect demos are where a
display or a GPU is the subject.

On macOS the demo project builds `mac_demo` from `graphics/mac/` and `effect_demo`/`effect_benchmark`
from `effect/`. `mac_demo` is the Cocoa window: it owns an `NSOpenGLView` and its `NSOpenGLContext`,
and hands the CGL context to `gpu::WindowSurface`, so the frame goes straight to the view's drawable.
`--verify` renders the same scene through an offscreen `gpu::Surface`, which is what a machine without
a display runs. `effect_demo` brings up an offscreen device through the same OpenGL backend and exits
77 when it cannot, which is the same skip the other GPU checks use.

There is no `graphics_demo` or `lottie_player` on macOS: those are the Win32 and X11/Wayland window
implementations, and `mac_demo` is the Cocoa counterpart.

An iOS cross build produces the same `effect_demo` and `effect_benchmark` as `.app` bundles, over the
OpenGL ES 3.0 / EAGL backend. They are run through the simulator:
`xcrun simctl spawn booted out/ios/bin/effect_demo.app/effect_demo`. iOS has no `examples/graphics`
window demo; that directory now has Win32, X11/Wayland and Cocoa implementations.

## Verification

`enable_testing()` is called and the checks that do not need a person looking at a window are
registered:

| Test | What it proves |
|---|---|
| `graphics_demo_raster` | the rasterizer and the PNG codec produce the demo picture |
| `vulkan_demo_checks`, `vulkan_demo_checks_render_pass` | the Vulkan backend draws and reads back the measured colours, through both the dynamic-rendering and render-pass paths |
| `mac_demo_verify` | the CGL window path's scene renders and reads back through the offscreen device |
| `effect_demo` | the effect graph runs end to end |
| `effect_window_demo`, `effect_transition_demo` | the Windows swap-chain output and the preload/transition path |
| `effect_x11_demo`, `effect_wayland_demo` | the Linux outputs |

A check whose subject is unavailable returns **77**, which CTest reports as a skip: no Vulkan device,
no X server, no compositor. On macOS the effect demo and the effect module's own suites run against
the OpenGL backend; the Vulkan and windowed checks are not part of the macOS demo build. Run the
registered checks with

```sh
# Windows (multi-config generators need the configuration)
ctest --test-dir out/examples -C Release --output-on-failure

# macOS and Linux (single-config generators)
ctest --test-dir out/examples --output-on-failure
```

## Options

| Option | Default | Effect |
|---|---|---|
| `CORAL_LOTTIE_PLAYER_STUB` | `OFF` | Compile the Lottie player's no-window branch, which is what a port to a new platform starts from |
| `CORAL_GRAPHICS_EXAMPLES_TOOLS` | `OFF` | Build the decode, shape, blend, filter and clip diagnostics |
| `CORAL_SHARED_THIRD_PARTY_DIR` | bundled submodules | Use a project's local `libz`/`libyuv`/`libgtest` instead |
| `CORAL_GTEST_DIR` | bundled submodule | Use a specific GoogleTest checkout |
| `CORAL_CORE_DIR` | auto | Where the CoralCore checkout lives |
| `CORAL_BACKEND_SUPPORTS_VULKAN` | `ON` | Build the Vulkan demo and register its checks |
| `CORAL_BACKEND_SUPPORTS_SHADERC` / `SPIRV_CROSS` | `ON` | The shader toolchain the framework links against |

The probes and the shader maintenance tools are not demos and are not here: the framework root builds
them with `-DCORAL_GRAPHICS_BUILD_DEV_TOOLS=ON`.
